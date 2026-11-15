// SPDX-License-Identifier: BSD-3-Clause
#include "xboxone_native/era_loader.h"
#include "era_memory.h"
#include "era_audio.h"
#include "era_display.h"
#include "era_winrt_classes.h"
#include "xboxone_native/appmodel.h"
#include "file_path.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wrl/client.h>
#include <wrl/wrappers/corewrappers.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <chrono>
#include <cstdio>
#include <climits>
#include <iterator>
#include <map>
#include <mutex>
#include <new>
#include <string>
#include <string_view>

namespace xboxone::era {
namespace {

// Same-name Win32 exports with identical ABI and semantics for a title process.
// Memory (Virtual*), device, file-deletion and Xbox-only exports are absent on
// purpose: they need title-memory/storage contracts, not a host forward.
constexpr std::string_view forwarded[]{
    "GetProcessAffinityMask", "SetProcessAffinityMask", "SetThreadIdealProcessorEx", "SuspendThread",
    "CreateFiber", "DeleteFiber", "ConvertThreadToFiber", "ConvertFiberToThread", "SwitchToFiber",
    "FindFirstFileExA", "FindNextFileA", "FindClose", "GetDiskFreeSpaceExW",
    "CompareFileTime", "CreateEventA", "CreateEventExW", "CreateIoCompletionPort", "CreateMutexA",
    "CreateSemaphoreExW", "DebugBreak", "FileTimeToLocalFileTime", "GetFileAttributesExA",
    "GetFileAttributesExW", "GetFileInformationByHandleEx", "GetFileSizeEx", "GetOverlappedResultEx",
    "GetQueuedCompletionStatus", "GetThreadPriority", "GetThreadTimes", "LocaleNameToLCID",
    "MultiByteToWideChar", "OpenThread", "PostQueuedCompletionStatus", "RaiseException",
    "SetFilePointer", "SetFilePointerEx", "WaitForMultipleObjectsEx", "WideCharToMultiByte",
    "AcquireSRWLockExclusive", "AcquireSRWLockShared", "CloseHandle", "CreateEventW", "CreateMutexW",
    "CreateSemaphoreW", "CreateThread", "DecodePointer", "DeleteCriticalSection", "DuplicateHandle",
    "EncodePointer", "EnterCriticalSection", "EventRegister", "EventUnregister", "EventWrite", "ExitProcess",
    "ExitThread", "FileTimeToSystemTime", "FlsAlloc", "FlsFree", "FlsGetValue", "FlsSetValue", "FormatMessageW",
    "FreeLibrary", "GetCurrentProcess", "GetCurrentProcessId", "GetCurrentThread", "GetCurrentThreadId",
    "GetDateFormatEx", "GetExitCodeThread", "GetLastError", "GetLocalTime", "GetProcAddress", "GetProcessHeap",
    "GetStartupInfoW", "GetSystemTime", "GetSystemTimeAsFileTime", "GetTickCount", "GetTickCount64",
    "GetTimeFormatEx", "GetUserDefaultLocaleName", "HeapAlloc", "HeapFree", "HeapReAlloc", "HeapSize",
    "InitializeConditionVariable", "InitializeCriticalSection", "InitializeCriticalSectionAndSpinCount",
    "InitializeCriticalSectionEx", "InitializeSListHead", "InitializeSRWLock", "IsDebuggerPresent",
    "IsProcessorFeaturePresent", "LeaveCriticalSection", "LoadLibraryExW", "OutputDebugStringA",
    "OutputDebugStringW", "QueryPerformanceCounter", "QueryPerformanceFrequency", "ReleaseMutex",
    "ReleaseSRWLockExclusive", "ReleaseSRWLockShared", "ReleaseSemaphore", "ResetEvent", "ResumeThread",
    "RtlCaptureContext", "RtlLookupFunctionEntry", "RtlVirtualUnwind", "SetEvent", "SetLastError",
    "SetThreadAffinityMask", "SetThreadPriority", "SetUnhandledExceptionFilter", "Sleep",
    "SleepConditionVariableCS", "SleepConditionVariableSRW", "SleepEx", "SwitchToThread", "SystemTimeToFileTime",
    "TerminateProcess", "TlsAlloc", "TlsFree", "TlsGetValue", "TlsSetValue", "TryEnterCriticalSection",
    "UnhandledExceptionFilter", "WaitForMultipleObjects", "WaitForSingleObject", "WaitForSingleObjectEx",
    "WakeAllConditionVariable", "WakeConditionVariable"};

std::atomic<HMODULE> title_module{nullptr};
std::mutex storage_mutex;
std::recursive_mutex memory_mutex;
std::condition_variable_any memory_changed;
using WatchKey = std::pair<std::uintptr_t,SIZE_T>;
struct GpuWindow {
    Microsoft::WRL::ComPtr<ID3D12Resource> resource;
    unsigned owners = 0;
    TitleGpuResource* active = nullptr;
    TitleGpuResource* cpu = nullptr;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COMMON;
    bool copied_back = false;
    bool read_only = false;
    bool preparing = false, completing = false;
    TitleTextureLayout texture;
    Microsoft::WRL::ComPtr<ID3D12Resource> upload, readback;
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints;
    std::vector<UINT> rows;
    UINT64 staging_bytes = 0;
    Microsoft::WRL::ComPtr<ID3D12Resource> sample_array;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> sample_srv, sample_rtv, sample_dsv;
    std::unique_ptr<SampleTransfer> sample_transfer;
    UINT sample_srv_step = 0, sample_rtv_step = 0;
    std::vector<unsigned char> sample_linear;
    bool staged_buffer = false;
    std::unique_ptr<TextureLayoutTransfer> layout_transfer;
    bool gpu_layout_active = false, gpu_layout_refused = false;
    std::array<bool, 512> physical_pages{};
    bool tracked = false, resident = false, pending_cpu = false, reuse = false;
    DWORD active_thread = 0;
    TitleGpuResource tracked_resource;
    TitleGpuClearState resident_clear, active_clear;
    std::vector<WatchKey> watch_keys;
    struct Segment { std::shared_ptr<void> view; SIZE_T offset, length, logical; };
    std::vector<Segment> segments;
};
using GpuWindows = std::multimap<std::pair<SIZE_T, SIZE_T>, GpuWindow>;
struct CoherenceReadback {
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    Microsoft::WRL::Wrappers::Event completed{CreateEventW(nullptr,FALSE,FALSE,nullptr)};
    UINT64 value = 0;
};
struct GpuBacking {
    SIZE_T size;
    DWORD guest_protection = 0;
    bool retiring = false;
    UINT esram_page = UINT_MAX;
    unsigned gpu_pins = 0;
    Microsoft::WRL::ComPtr<ID3D12Heap> heap;
    Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
    Microsoft::WRL::ComPtr<IUnknown> device;
    Microsoft::WRL::Wrappers::FileHandle mapping{};
    std::shared_ptr<void> mirror;
    std::unique_ptr<CoherenceReadback> coherence;
    D3D12_RESOURCE_STATES initial_state = D3D12_RESOURCE_STATE_COMMON;
    GpuWindows windows; // Canonical range; independent native aliases share ownership gates.
};
struct TitleRegion : GpuBacking {
    SIZE_T physical_bytes = 0;
    bool graphics;
    bool xmem = false;
    ULONGLONG attributes = 0;
    std::map<std::uintptr_t, GpuBacking> commits;
    TitleRegion(SIZE_T bytes, bool gpu) : GpuBacking{bytes}, graphics(gpu) {}
};
std::map<std::uintptr_t, TitleRegion> title_regions;
constexpr SIZE_T esram_page_bytes = 65536, esram_bytes = 32u << 20;
struct PhysicalGate { TitleGpuResource* gpu = nullptr; TitleGpuResource* cpu = nullptr; };
struct EsramPool {
    Microsoft::WRL::Wrappers::FileHandle mapping;
    std::array<PhysicalGate, 512> pages{};
} esram;
HRESULT reset_gpu_tracking();
HRESULT cpu_memory_access(const void* address,SIZE_T size,bool write);
HRESULT flush_gpu_window(GpuWindow& window);
void detach_gpu_tracking(GpuWindow& window);
void forget_tracked_addresses(std::uintptr_t start,SIZE_T size);
constexpr UINT64 title_budget = 0x140000000ull;
UINT64 title_committed = 0; // Guarded by memory_mutex, including retiring allocations.

bool memory_capacity(UINT64 extra, UINT64* total = nullptr, UINT64* available = nullptr) {
    MEMORYSTATUSEX host{};
    host.dwLength = sizeof host;
    if (!GlobalMemoryStatusEx(&host)) return false;
    const auto limit = (std::min)(title_budget, host.ullTotalPhys);
    const auto remaining = title_committed < limit ? limit - title_committed : 0;
    const auto free = (std::min)(remaining, (std::min)(host.ullAvailPhys, host.ullAvailPageFile));
    if (total) *total = limit;
    if (available) *available = free;
    if (extra > free) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return false; }
    return true;
}

// Query only owned ordinary virtual ranges; the OS tracks partial commits/recommits.
bool committed_in_range(std::uintptr_t start, SIZE_T bytes, SIZE_T& committed) {
    committed = 0;
    const auto end = start + bytes;
    for (auto cursor = start; cursor < end;) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(reinterpret_cast<void*>(cursor), &info, sizeof info)) return false;
        const auto next = (std::min)(end, reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize);
        if (next <= cursor) { SetLastError(ERROR_INVALID_ADDRESS); return false; }
        if (info.State == MEM_COMMIT) committed += next - cursor;
        cursor = next;
    }
    return true;
}

struct TitleMemoryStatus {
    DWORD length, reserved;
    UINT64 total, available, legacy_used, legacy_peak, legacy_available, used, title_available;
};
static_assert(sizeof(TitleMemoryStatus) == 64 && offsetof(TitleMemoryStatus, used) == 48);
BOOL WINAPI era_TitleMemoryStatus(TitleMemoryStatus* status) {
    DWORD length = 0;
    SIZE_T read = 0;
    if (!status || !ReadProcessMemory(GetCurrentProcess(), status, &length, sizeof length, &read) ||
        read != sizeof length || length != sizeof(TitleMemoryStatus)) {
        SetLastError(ERROR_INVALID_PARAMETER); return FALSE;
    }
    std::lock_guard lock(memory_mutex);
    TitleMemoryStatus result{};
    result.length = sizeof result;
    if (!memory_capacity(0, &result.total, &result.available)) return FALSE;
    result.used = title_committed;
    result.title_available = result.available;
    SIZE_T written = 0;
    if (!WriteProcessMemory(GetCurrentProcess(), status, &result, sizeof result, &written) || written != sizeof result)
        return FALSE;
    SetLastError(ERROR_SUCCESS);
    return TRUE;
}
void* WINAPI era_XMemAllocDefault(SIZE_T size, ULONGLONG attributes);
void WINAPI era_XMemFreeDefault(void* address, ULONGLONG attributes);
using XAlloc = void* (WINAPI*)(SIZE_T, ULONGLONG);
using XFree = void (WINAPI*)(void*, ULONGLONG);
std::recursive_mutex allocation_hook_mutex;
XAlloc allocation_hook = nullptr;
XFree free_hook = nullptr;

void WINAPI era_XMemSetAllocationHooks(XAlloc allocate, XFree release) {
    std::lock_guard lock(allocation_hook_mutex);
    allocation_hook = allocate;
    free_hook = release;
}

void* WINAPI era_XMemAlloc(SIZE_T size, ULONGLONG attributes) {
    std::lock_guard lock(allocation_hook_mutex);
    if (allocation_hook) return allocation_hook(size, attributes);
    return era_XMemAllocDefault(size, attributes);
}

void WINAPI era_XMemFree(void* address, ULONGLONG attributes) {
    std::lock_guard lock(allocation_hook_mutex);
    if (free_hook) { free_hook(address, attributes); return; }
    era_XMemFreeDefault(address, attributes);
}

bool destroy_region(std::map<std::uintptr_t, TitleRegion>::iterator region) {
    region->second.buffer.Reset();
    region->second.heap.Reset();
    region->second.device.Reset();
    region->second.commits.clear();
    if (!VirtualFree(reinterpret_cast<void*>(region->first), 0, MEM_RELEASE)) return false;
    title_committed -= region->second.physical_bytes;
    title_regions.erase(region);
    return true;
}

void* WINAPI era_XMemAllocDefault(SIZE_T size, ULONGLONG attributes) {
    // XALLOC_ATTRIBUTES ABI: object:14, page:2, allocator:8, alignment:5, type:4.
    const auto page = (attributes >> 14) & 3;
    const auto alignment = (attributes >> 24) & 31;
    const auto type = (attributes >> 29) & 15;
    const bool audio = attributes == 0xec810000ull; // AcpHal 11295 XMem layout.
    if (!size || (attributes >> 33) || page > 1 || (type > 6 && !audio)) {
        SetLastError(ERROR_NOT_SUPPORTED); return nullptr;
    }
    constexpr SIZE_T granularity = 65536;
    if (size > SIZE_MAX - (granularity - 1)) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return nullptr; }
    const SIZE_T bytes = (size + granularity - 1) & ~(granularity - 1);
    MEM_ADDRESS_REQUIREMENTS address{};
    address.Alignment = SIZE_T{1} << (alignment < 16 ? 16 : alignment);
    if (audio) {
        // Software ACP uses real 32-bit CPU VAs as its address space. These
        // are not fabricated physical ACP addresses or a second memory pool.
        address.LowestStartingAddress = reinterpret_cast<void*>(65536);
        address.HighestEndingAddress = reinterpret_cast<void*>(ULONG_PTR(UINT32_MAX));
    } else if (type) {
        // ERA graphics virtual addresses occupy the low 40-bit window above 4 GiB.
        address.LowestStartingAddress = reinterpret_cast<void*>(0x100000000ull);
        address.HighestEndingAddress = reinterpret_cast<void*>(0xffffffffffull);
    }
    MEM_EXTENDED_PARAMETER parameter{};
    parameter.Type = MemExtendedParameterAddressRequirements;
    parameter.Pointer = &address;
    using Allocate = decltype(&VirtualAlloc2);
    const HMODULE host = GetModuleHandleW(L"kernelbase.dll");
    const auto allocate = reinterpret_cast<Allocate>(host ? GetProcAddress(host, "VirtualAlloc2") : nullptr);
    if (!allocate) { SetLastError(ERROR_NOT_SUPPORTED); return nullptr; }
    std::lock_guard lock(memory_mutex);
    if (!memory_capacity(bytes)) return nullptr;
    void* result = allocate(GetCurrentProcess(), nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, &parameter, 1);
    if (!result) return nullptr;
    TitleRegion region{bytes, type != 0 && !audio};
    region.physical_bytes = bytes;
    region.xmem = true;
    region.attributes = attributes;
    try {
        title_regions.emplace(reinterpret_cast<std::uintptr_t>(result), std::move(region));
    } catch (const std::bad_alloc&) {
        VirtualFree(result, 0, MEM_RELEASE);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY); return nullptr;
    }
    title_committed += bytes;
    return result;
}

void WINAPI era_XMemFreeDefault(void* address, ULONGLONG attributes) {
    if (!address) return;
    std::lock_guard lock(memory_mutex);
    auto region = title_regions.find(reinterpret_cast<std::uintptr_t>(address));
    // Allocation layout must match; object/allocator tags do not change backing.
    constexpr ULONGLONG layout = (3ull << 14) | (511ull << 24);
    if (region == title_regions.end() || !region->second.xmem || region->second.retiring ||
        ((attributes ^ region->second.attributes) & layout)) {
        SetLastError(ERROR_INVALID_PARAMETER); return;
    }
    if (region->second.gpu_pins) region->second.retiring = true;
    else (void)destroy_region(region);
}

void memory_error(LPVOID address, SIZE_T size, DWORD type, DWORD protection, DWORD error) {
    static std::atomic<unsigned> reports{0};
    if (reports.fetch_add(1) < 8) {
        char text[256];
        const int n = std::snprintf(text, sizeof text,
            "xone-era: VirtualAlloc address=%p size=0x%llX type=0x%lX protection=0x%lX failed Win32 %lu\n",
            address, static_cast<unsigned long long>(size), type, protection, error);
        DWORD written = 0;
        WriteFile(GetStdHandle(STD_ERROR_HANDLE), text, static_cast<DWORD>(n), &written, nullptr);
        if (error == ERROR_INVALID_ADDRESS && address && size <= UINTPTR_MAX - reinterpret_cast<std::uintptr_t>(address)) {
            const auto end = reinterpret_cast<std::uintptr_t>(address) + size;
            auto at = reinterpret_cast<std::uintptr_t>(address);
            for (unsigned i = 0; i != 16 && at < end; ++i) {
                MEMORY_BASIC_INFORMATION info{};
                if (!VirtualQuery(reinterpret_cast<void*>(at), &info, sizeof info)) break;
                if (info.State != MEM_FREE) {
                    const int detail = std::snprintf(text, sizeof text,
                        "xone-era: reservation conflict base=%p allocation=%p bytes=0x%llX state=0x%lX type=0x%lX\n",
                        info.BaseAddress, info.AllocationBase, static_cast<unsigned long long>(info.RegionSize), info.State, info.Type);
                    WriteFile(GetStdHandle(STD_ERROR_HANDLE), text, static_cast<DWORD>(detail), &written, nullptr);
                    MEMORY_BASIC_INFORMATION stack{};
                    if (VirtualQuery(&stack, &stack, sizeof stack) && stack.AllocationBase == info.AllocationBase) {
                        constexpr char message[] = "xone-era: reservation conflicts with the current host thread stack\n";
                        WriteFile(GetStdHandle(STD_ERROR_HANDLE), message, sizeof message - 1, &written, nullptr);
                    }
                    break;
                }
                const auto next = reinterpret_cast<std::uintptr_t>(info.BaseAddress) + info.RegionSize;
                if (next <= at) break;
                at = next;
            }
        }
    }
    SetLastError(error);
}

auto host_allocate2() {
    return reinterpret_cast<decltype(&VirtualAlloc2)>(GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "VirtualAlloc2"));
}
auto host_map3() {
    return reinterpret_cast<decltype(&MapViewOfFile3)>(GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "MapViewOfFile3"));
}
auto host_unmap2() {
    return reinterpret_cast<decltype(&UnmapViewOfFile2)>(GetProcAddress(GetModuleHandleW(L"kernelbase.dll"), "UnmapViewOfFile2"));
}
bool coalesce_placeholder(void* address, SIZE_T size) {
    MEMORY_BASIC_INFORMATION info{};
    if (!VirtualQuery(address, &info, sizeof info)) return false;
    if (info.RegionSize == size && info.BaseAddress == address) return true;
    return VirtualFree(address, size, MEM_RELEASE | MEM_COALESCE_PLACEHOLDERS) != FALSE;
}
bool unmap_graphics_backing(std::uintptr_t address, GpuBacking& backing) {
    const HRESULT synchronized=reset_gpu_tracking();
    if(FAILED(synchronized)){SetLastError(ERROR_BUSY);return false;}
    backing.buffer.Reset(); backing.heap.Reset(); backing.device.Reset();
    const auto unmap = host_unmap2();
    if (!unmap) { SetLastError(ERROR_NOT_SUPPORTED); return false; }
    if (!unmap(GetCurrentProcess(), reinterpret_cast<void*>(address), MEM_PRESERVE_PLACEHOLDER)) return false;
    forget_tracked_addresses(address,backing.size);
    backing.mapping.Close();
    backing.mirror.reset();
    return true;
}

// MEM_TITLE selects title ownership; keep the requested address/protection.
// GPU commitments retain the reservation and bind each committed span to the
// same shared CPU/GPU registry, including mapped ESRAM physical aliases.
LPVOID WINAPI era_VirtualAlloc(LPVOID address, SIZE_T size, DWORD type, DWORD protection) {
    constexpr DWORD mem_title = 0x40000000;
    constexpr DWORD mem_graphics = 0x10000000;
    constexpr DWORD supported = MEM_COMMIT | MEM_RESERVE | MEM_TOP_DOWN | MEM_RESET | MEM_RESET_UNDO | MEM_WRITE_WATCH;
    const bool graphics = (type & mem_graphics) != 0;
    // Xbox's large graphics pages are 64 KiB; reservations round to that size.
    // Null addresses select a free address in the same 40-bit graphics window.
    const bool graphics_reserve = graphics && (type & MEM_LARGE_PAGES) &&
        (type & ~(mem_graphics | MEM_LARGE_PAGES)) == MEM_RESERVE &&
        !(reinterpret_cast<std::uintptr_t>(address) & 65535) &&
        (protection == PAGE_READWRITE || protection == (PAGE_READWRITE | 0x10000));
    const bool graphics_commit = address && type == (mem_graphics | MEM_LARGE_PAGES | MEM_COMMIT) &&
        !(reinterpret_cast<std::uintptr_t>(address) & 65535) &&
        (protection == PAGE_READWRITE || protection == (PAGE_READWRITE | 0x10000));
    if (!size || (!graphics_reserve && !graphics_commit && (type & ~(supported | mem_title)))) {
        memory_error(address, size, type, protection, ERROR_NOT_SUPPORTED); return nullptr;
    }
    if (graphics_commit) {
        if (size > SIZE_MAX - 65535) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return nullptr; }
        size = (size + 65535) & ~SIZE_T{65535};
    }
    std::lock_guard lock(memory_mutex);
    TitleRegion* ordinary = nullptr;
    SIZE_T extra = 0;
    if (address && !(type & MEM_RESERVE)) {
        const auto at = reinterpret_cast<std::uintptr_t>(address);
        auto region = title_regions.upper_bound(at);
        if (region == title_regions.begin()) { SetLastError(ERROR_INVALID_ADDRESS); return nullptr; }
        --region;
        const auto offset = at - region->first;
        if (offset >= region->second.size || size > region->second.size - offset) {
            SetLastError(ERROR_INVALID_ADDRESS); return nullptr;
        }
        if (graphics_commit && region->second.graphics) {
            auto& commits = region->second.commits;
            auto next = commits.lower_bound(at);
            if (next != commits.end() && next->first == at && next->second.size == size) {
                if (next->second.esram_page != UINT_MAX) { SetLastError(ERROR_INVALID_ADDRESS); return nullptr; }
                if (next->second.gpu_pins) { SetLastError(ERROR_BUSY); return nullptr; }
                if (next->second.guest_protection != protection) { SetLastError(ERROR_NOT_SUPPORTED); return nullptr; }
                return address; // The same span is already committed; preserve its bytes.
            }
            if ((next != commits.end() && size > next->first - at) ||
                (next != commits.begin() && std::prev(next)->second.size > at - std::prev(next)->first)) {
                SetLastError(ERROR_INVALID_ADDRESS); return nullptr;
            }
            if (!memory_capacity(size)) return nullptr;
            try {
                auto [entry, inserted] = commits.emplace(at, GpuBacking{size});
                if (!inserted) { SetLastError(ERROR_INVALID_ADDRESS); return nullptr; }
                entry->second.guest_protection = protection;
                const auto fail = [&]() -> void* {
                    const DWORD error = GetLastError();
                    commits.erase(entry); memory_error(address, size, type, protection, error); return nullptr;
                };
                const auto map = host_map3();
                if (!map) { SetLastError(ERROR_NOT_SUPPORTED); return fail(); }
                // Coalesce the free gap before splitting an exact placeholder.
                const auto gap_start = entry == commits.begin() ? region->first :
                    std::prev(entry)->first + std::prev(entry)->second.size;
                const auto after = std::next(entry);
                const auto gap_end = after == commits.end() ? region->first + region->second.size : after->first;
                if (!coalesce_placeholder(reinterpret_cast<void*>(gap_start), gap_end - gap_start)) return fail();
                if (at != gap_start && !VirtualFree(reinterpret_cast<void*>(gap_start), at - gap_start,
                                                    MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) return fail();
                if (size != gap_end - at && !VirtualFree(address, size, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) return fail();
                entry->second.mapping.Attach(CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE,
                    static_cast<DWORD>(static_cast<UINT64>(size) >> 32), static_cast<DWORD>(size), nullptr));
                if (!entry->second.mapping.IsValid()) return fail();
                void* result = map(entry->second.mapping.Get(), GetCurrentProcess(), address, 0, size,
                                   MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0);
                if (!result) return fail();
                region->second.physical_bytes += size;
                title_committed += size;
                return result;
            } catch (const std::bad_alloc&) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return nullptr; }
        }
        if (region->second.graphics || graphics_commit) {
            memory_error(address, size, type, protection, ERROR_NOT_SUPPORTED); return nullptr;
        }
        if (region->second.xmem) { SetLastError(ERROR_NOT_SUPPORTED); return nullptr; }
        ordinary = &region->second;
    }
    if (!graphics_reserve && !graphics_commit && (type & MEM_COMMIT)) {
        const auto at = reinterpret_cast<std::uintptr_t>(address);
        const SIZE_T prefix = at & ((type & MEM_RESERVE) ? 65535 : 4095);
        if (size > SIZE_MAX - prefix - 4095) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return nullptr; }
        const SIZE_T bytes = (size + prefix + 4095) & ~SIZE_T{4095};
        SIZE_T already = 0;
        if (ordinary && !committed_in_range(at - prefix, bytes, already)) return nullptr;
        extra = bytes - already;
        if (extra && !memory_capacity(extra)) return nullptr;
    }
    void* result = nullptr;
    if (graphics_reserve) {
        const auto allocate = host_allocate2();
        if (!allocate || !host_map3() || !host_unmap2()) { SetLastError(ERROR_NOT_SUPPORTED); return nullptr; }
        if (size > SIZE_MAX - 65535) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return nullptr; }
        MEM_ADDRESS_REQUIREMENTS requirements{};
        requirements.Alignment = 65536;
        requirements.LowestStartingAddress = reinterpret_cast<void*>(0x100000000ull);
        requirements.HighestEndingAddress = reinterpret_cast<void*>(0xffffffffffull);
        MEM_EXTENDED_PARAMETER parameter{};
        parameter.Type = MemExtendedParameterAddressRequirements; parameter.Pointer = &requirements;
        result = allocate(GetCurrentProcess(), address, (size + 65535) & ~SIZE_T{65535}, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                          PAGE_NOACCESS, address ? nullptr : &parameter, address ? 0 : 1);
    } else result = VirtualAlloc(address, size, type & ~mem_title, protection);
    if (!result) memory_error(address, size, type, protection, GetLastError());
    if (result && ((type & MEM_RESERVE) || !address)) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(result, &info, sizeof info)) {
            const DWORD error = GetLastError();
            VirtualFree(result, 0, MEM_RELEASE); SetLastError(error); return nullptr;
        }
        try {
            auto [entry, inserted] = title_regions.emplace(reinterpret_cast<std::uintptr_t>(info.AllocationBase), TitleRegion{info.RegionSize, graphics});
            if (!inserted) { SetLastError(ERROR_INVALID_ADDRESS); return nullptr; }
            entry->second.physical_bytes = extra;
        } catch (const std::bad_alloc&) {
            VirtualFree(result, 0, MEM_RELEASE);
            SetLastError(ERROR_NOT_ENOUGH_MEMORY); return nullptr;
        }
    }
    if (result) {
        if (ordinary) ordinary->physical_bytes += extra;
        title_committed += extra;
    }
    return result;
}

BOOL WINAPI era_VirtualFree(LPVOID address, SIZE_T size, DWORD type) {
    std::lock_guard lock(memory_mutex);
    const auto at = reinterpret_cast<std::uintptr_t>(address);
    auto region = title_regions.upper_bound(at);
    if (region == title_regions.begin()) { SetLastError(ERROR_INVALID_ADDRESS); return FALSE; }
    --region;
    if (region->second.gpu_pins || region->second.xmem) { SetLastError(ERROR_BUSY); return FALSE; }
    const auto offset = at - region->first;
    if ((type == MEM_RELEASE && (offset || size)) ||
        (type == MEM_DECOMMIT && (offset >= region->second.size || size > region->second.size - offset)) ||
        (type != MEM_RELEASE && type != MEM_DECOMMIT)) {
        SetLastError(ERROR_INVALID_PARAMETER); return FALSE;
    }
    if (!region->second.commits.empty()) {
        auto& commits = region->second.commits;
        if (type == MEM_DECOMMIT) {
            auto commit = commits.find(at);
            if (commit == commits.end() || commit->second.size != size) { SetLastError(ERROR_NOT_SUPPORTED); return FALSE; }
            if (commit->second.gpu_pins) { SetLastError(ERROR_BUSY); return FALSE; }
            if (!unmap_graphics_backing(at, commit->second)) return FALSE;
            if (commit->second.esram_page == UINT_MAX) {
                region->second.physical_bytes -= commit->second.size;
                title_committed -= commit->second.size;
            }
            commits.erase(commit);
            return TRUE;
        }
        for (const auto& [base, commit] : commits)
            if (commit.gpu_pins) { SetLastError(ERROR_BUSY); return FALSE; }
        while (!commits.empty()) {
            auto commit = commits.begin();
            if (!unmap_graphics_backing(commit->first, commit->second)) return FALSE;
            if (commit->second.esram_page == UINT_MAX) {
                region->second.physical_bytes -= commit->second.size;
                title_committed -= commit->second.size;
            }
            commits.erase(commit);
        }
    }
    if (type == MEM_RELEASE && region->second.graphics && !coalesce_placeholder(address, region->second.size)) return FALSE;
    SIZE_T released = region->second.physical_bytes;
    if (type == MEM_DECOMMIT) {
        if (region->second.graphics) { SetLastError(ERROR_NOT_SUPPORTED); return FALSE; }
        if (!size && offset) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
        const auto start = at & ~std::uintptr_t{4095};
        const auto bytes = size ? ((size + (at - start) + 4095) & ~SIZE_T{4095}) : region->second.size;
        if (!committed_in_range(start, bytes, released)) return FALSE;
    }
    const BOOL ok = VirtualFree(address, size, type);
    if (ok) {
        title_committed -= released;
        region->second.physical_bytes -= released;
    }
    if (ok && type == MEM_RELEASE) {forget_tracked_addresses(region->first,region->second.size);title_regions.erase(region);}
    return ok;
}
std::wstring storage_root; // Full path ending in '\'; empty = no writable storage.

std::wstring full_path(LPCWSTR path) {
    if (!path || !*path) { SetLastError(ERROR_INVALID_PARAMETER); return {}; }
    std::wstring normalized = path;
    if (normalized.starts_with(L"\\\\?\\UNC\\")) normalized = L"\\\\" + normalized.substr(8);
    else if (normalized.starts_with(L"\\\\?\\")) normalized.erase(0, 4);
    path = normalized.c_str();
    DWORD n = GetFullPathNameW(path, 0, nullptr, nullptr);
    if (!n) return {};
    std::wstring out(n, L'\0');
    n = GetFullPathNameW(path, n, out.data(), nullptr);
    if (!n || n >= out.size()) { SetLastError(ERROR_INVALID_NAME); return {}; }
    out.resize(n);
    // Handle names are normalized to long names, while TEMP may contain 8.3 aliases.
    // Expand the existing prefix only; creation may target a missing leaf.
    std::size_t end = out.size();
    while (end > 3 && end != std::wstring::npos) {
        const auto prefix = out.substr(0, end);
        const std::wstring extended = prefix.starts_with(L"\\\\") ? L"\\\\?\\UNC\\" + prefix.substr(2) : L"\\\\?\\" + prefix;
        const DWORD capacity = GetLongPathNameW(extended.c_str(), nullptr, 0);
        if (capacity) {
            std::wstring expanded(capacity, L'\0');
            const DWORD length = GetLongPathNameW(extended.c_str(), expanded.data(), capacity);
            if (!length || length >= capacity) return {};
            expanded.resize(length);
            if (expanded.starts_with(L"\\\\?\\UNC\\")) expanded = L"\\\\" + expanded.substr(8);
            else if (expanded.starts_with(L"\\\\?\\")) expanded.erase(0, 4);
            out = expanded + out.substr(end);
            break;
        }
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) return {};
        end = out.rfind(L'\\', end - 1);
    }
    return out;
}

DWORD path_attributes(const std::wstring& path) {
    const std::wstring extended = path.starts_with(L"\\\\") ? L"\\\\?\\UNC\\" + path.substr(2) : L"\\\\?\\" + path;
    return GetFileAttributesW(extended.c_str());
}

// Title file mutations stay inside the writable title root. Outside it (the
// read-only package or any host path) they fail like a read-only volume.
// Reparse points under the root are refused so a junction cannot escape it.
bool inside_storage(const std::wstring& full) {
    std::wstring root;
    {
        std::lock_guard lock(storage_mutex);
        root = storage_root;
    }
    if (root.empty() || full.size() <= root.size() ||
        CompareStringOrdinal(full.c_str(), static_cast<int>(root.size()), root.c_str(),
                             static_cast<int>(root.size()), TRUE) != CSTR_EQUAL)
        return false;
    // Reject streams and reparse points, including the configured root itself.
    if (full.find(L':', 2) != std::wstring::npos) return false;
    for (std::size_t at = full.find(L'\\', 3); at != std::wstring::npos; at = full.find(L'\\', at + 1)) {
        const DWORD attributes = path_attributes(full.substr(0, at));
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    }
    const DWORD attributes = path_attributes(full);
    if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    return true;
}

std::wstring wide_path(LPCSTR path) {
    if (!path) { SetLastError(ERROR_INVALID_PARAMETER); return {}; }
    const int n = MultiByteToWideChar(CP_ACP, 0, path, -1, nullptr, 0);
    if (!n) return {};
    std::wstring out(n, L'\0');
    if (!MultiByteToWideChar(CP_ACP, 0, path, -1, out.data(), n)) return {};
    out.resize(n - 1);
    return out;
}

bool writable_path(LPCWSTR path) {
    const auto full = full_path(path);
    if (full.empty()) return false;
    if (inside_storage(full)) return true;
    SetLastError(ERROR_ACCESS_DENIED);
    return false;
}

bool writable_handle(HANDLE handle) {
    DWORD n = GetFinalPathNameByHandleW(handle, nullptr, 0, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!n) return false;
    std::wstring path(n, L'\0');
    n = GetFinalPathNameByHandleW(handle, path.data(), n, FILE_NAME_NORMALIZED | VOLUME_NAME_DOS);
    if (!n || n >= path.size()) return false;
    path.resize(n);
    return writable_path(path.c_str());
}

// The guest temporary volume belongs to the session, never the host's T: drive.
std::wstring title_path(LPCWSTR path) {
    if (!path || !*path) { SetLastError(ERROR_INVALID_PARAMETER); return {}; }
    std::wstring guest = path;
    // XAML content URIs such as /Media/UI/Videos/... are package-rooted,
    // not paths at the root of the host's current drive.
    if(guest.front()==L'/' && !guest.starts_with(L"//")) {
        const auto root=title_package_root();
        if(root.empty()){SetLastError(ERROR_ACCESS_DENIED);return {};}
        try {
            const auto name=std::filesystem::path(guest.substr(1)).generic_u8string();
            std::filesystem::path relative;
            if(!appmodel::package_relative_path(std::string(name.begin(),name.end()),relative)) {
                SetLastError(ERROR_INVALID_NAME);return {};
            }
            auto full=std::filesystem::path(root);
            for(const auto& component:relative) {
                full/=component;
                const auto attributes=GetFileAttributesW(detail::file_path(full).c_str());
                if(attributes!=INVALID_FILE_ATTRIBUTES && (attributes&FILE_ATTRIBUTE_REPARSE_POINT)) {
                    SetLastError(ERROR_ACCESS_DENIED);return {};
                }
            }
            return detail::file_path(full).wstring();
        } catch(const std::filesystem::filesystem_error&) {SetLastError(ERROR_INVALID_NAME);return {};}
    }
    if (guest.starts_with(L"\\\\?\\")) guest.erase(0, 4);
    if (guest.size() < 2 || (guest[0] != L'T' && guest[0] != L't') || guest[1] != L':') return path;
    if (guest.size() < 3 || (guest[2] != L'\\' && guest[2] != L'/')) {
        SetLastError(ERROR_INVALID_NAME); return {};
    }
    std::wstring root;
    { std::lock_guard lock(storage_mutex); root = storage_root; }
    if (root.empty()) { SetLastError(ERROR_ACCESS_DENIED); return {}; }
    const auto volume = root + L"temp";
    const auto candidate = volume + L"\\" + guest.substr(3);
    std::wstring full;
    const auto wildcard = candidate.find_first_of(L"*?");
    if (wildcard != std::wstring::npos) {
        const auto separator = candidate.find_last_of(L"\\/");
        if (wildcard < separator) { SetLastError(ERROR_INVALID_NAME); return {}; }
        full = full_path(candidate.substr(0, separator).c_str());
        if (!full.empty()) full += L"\\" + candidate.substr(separator + 1);
    } else full = full_path(candidate.c_str());
    if (full.empty()) return {};
    while (full.size() > volume.size() && full.back() == L'\\') full.pop_back();
    if (full.size() < volume.size() ||
        CompareStringOrdinal(full.c_str(), static_cast<int>(volume.size()), volume.c_str(),
                             static_cast<int>(volume.size()), TRUE) != CSTR_EQUAL ||
        (full.size() > volume.size() && full[volume.size()] != L'\\') || !inside_storage(volume)) {
        SetLastError(ERROR_ACCESS_DENIED); return {};
    }
    if (!CreateDirectoryW(volume.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return {};
    if (!inside_storage(full)) { SetLastError(ERROR_ACCESS_DENIED); return {}; }
    return full;
}

HANDLE WINAPI era_CreateFileW(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
                              DWORD disposition, DWORD flags, HANDLE template_file) {
    struct OpenTrace {
        LPCWSTR path; DWORD access,disposition; void* caller; HANDLE file=INVALID_HANDLE_VALUE;
        ~OpenTrace() {
            const DWORD error=GetLastError();
            static const bool enabled=[] {wchar_t value[2]{};return GetEnvironmentVariableW(L"XONE_ERA_TRACE_IO",value,2)==1 && value[0]==L'1';}();
            static std::atomic<UINT> observations{0};
            static std::atomic<UINT> late_observations{0};
            if(enabled && (observations.fetch_add(1)<256 ||
                ((file==INVALID_HANDLE_VALUE || (path && std::wstring_view(path).ends_with(L".bk2"))) && late_observations.fetch_add(1)<64)))
                std::fprintf(stderr,"xone-era: content open caller=%p access=0x%lX disposition=%lu handle=%p error=%lu path=%ls\n",caller,access,disposition,file,error,path ? path : L"<null>");
            SetLastError(error);
        }
    } trace{path,access,disposition,_ReturnAddress()};
    const auto translated = title_path(path);
    if (translated.empty()) return INVALID_HANDLE_VALUE;
    path = translated.c_str();
    constexpr DWORD mutation = GENERIC_WRITE | GENERIC_ALL | MAXIMUM_ALLOWED | DELETE | WRITE_DAC | WRITE_OWNER |
                               FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES;
    if (((access & mutation) || disposition != OPEN_EXISTING || (flags & FILE_FLAG_DELETE_ON_CLOSE)) &&
        !writable_path(path)) return INVALID_HANDLE_VALUE;
    const HANDLE file=CreateFileW(path, access, share, security, disposition, flags, template_file);
    trace.file=file;
    const DWORD error=GetLastError();
    if(file==INVALID_HANDLE_VALUE && stop_on_missing_service() && (error==ERROR_FILE_NOT_FOUND || error==ERROR_PATH_NOT_FOUND)) {
        static std::atomic<UINT> missing_files{0};
        if(missing_files.fetch_add(1)<8) std::fprintf(stderr,"xone-era: content open failed Win32 %lu path %ls\n",error,path ? path : L"<null>");
    }
    SetLastError(error);
    return file;
}

HANDLE WINAPI era_CreateFileA(LPCSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
                              DWORD disposition, DWORD flags, HANDLE template_file) {
    const auto wide = wide_path(path);
    if (wide.empty()) return INVALID_HANDLE_VALUE;
    return era_CreateFileW(wide.c_str(), access, share, security, disposition, flags, template_file);
}
FARPROC WINAPI era_GetProcAddress(HMODULE module,LPCSTR name) {
    if (const auto audio = resolve_xaudio2_export(module,name)) return audio;
    return GetProcAddress(module,name);
}

HANDLE WINAPI era_CreateFile2(LPCWSTR path, DWORD access, DWORD share, DWORD disposition,
                              LPCREATEFILE2_EXTENDED_PARAMETERS parameters) {
    if (parameters && parameters->dwSize != sizeof(*parameters)) {
        SetLastError(ERROR_INVALID_PARAMETER); return INVALID_HANDLE_VALUE;
    }
    return era_CreateFileW(path, access, share, parameters ? parameters->lpSecurityAttributes : nullptr,
        disposition, parameters ? parameters->dwFileAttributes | parameters->dwFileFlags | parameters->dwSecurityQosFlags : 0,
        parameters ? parameters->hTemplateFile : nullptr);
}

BOOL WINAPI era_WriteFile(HANDLE file, LPCVOID buffer, DWORD size, LPDWORD written, LPOVERLAPPED operation) {
    if (!writable_handle(file)) return FALSE;
    auto hr=cpu_memory_access(buffer,size,false);
    if(SUCCEEDED(hr) && written)hr=cpu_memory_access(written,sizeof *written,true);
    if(SUCCEEDED(hr) && operation)hr=cpu_memory_access(operation,sizeof *operation,true);
    if(FAILED(hr)){SetLastError(hr==E_PENDING ? ERROR_BUSY : HRESULT_FACILITY(hr)==FACILITY_WIN32 ? HRESULT_CODE(hr) : ERROR_GEN_FAILURE);return FALSE;}
    return WriteFile(file, buffer, size, written, operation);
}
BOOL WINAPI era_ReadFile(HANDLE file,LPVOID buffer,DWORD size,LPDWORD read,LPOVERLAPPED operation) {
    auto hr=cpu_memory_access(buffer,size,true);
    if(SUCCEEDED(hr) && read)hr=cpu_memory_access(read,sizeof *read,true);
    if(SUCCEEDED(hr) && operation)hr=cpu_memory_access(operation,sizeof *operation,true);
    if(FAILED(hr)){SetLastError(hr==E_PENDING ? ERROR_BUSY : HRESULT_FACILITY(hr)==FACILITY_WIN32 ? HRESULT_CODE(hr) : ERROR_GEN_FAILURE);return FALSE;}
    return ReadFile(file,buffer,size,read,operation);
}

BOOL WINAPI era_FlushFileBuffers(HANDLE file) {
    return writable_handle(file) ? FlushFileBuffers(file) : FALSE;
}

BOOL WINAPI era_SetEndOfFile(HANDLE file) {
    return writable_handle(file) ? SetEndOfFile(file) : FALSE;
}

BOOL WINAPI era_CreateDirectoryW(LPCWSTR path, LPSECURITY_ATTRIBUTES security) {
    const auto translated = title_path(path);
    return !translated.empty() && writable_path(translated.c_str()) && CreateDirectoryW(translated.c_str(), security);
}

BOOL WINAPI era_CreateDirectoryA(LPCSTR path, LPSECURITY_ATTRIBUTES security) {
    const auto wide = wide_path(path);
    return !wide.empty() && era_CreateDirectoryW(wide.c_str(), security);
}

BOOL WINAPI era_RemoveDirectoryA(LPCSTR path) {
    const auto wide = wide_path(path);
    const auto translated = wide.empty() ? std::wstring{} : title_path(wide.c_str());
    return !translated.empty() && writable_path(translated.c_str()) && RemoveDirectoryW(translated.c_str());
}

BOOL WINAPI era_MoveFileExW(LPCWSTR source, LPCWSTR destination, DWORD flags) {
    const auto from = title_path(source);
    if (from.empty()) return FALSE;
    const auto to = title_path(destination);
    if (to.empty()) return FALSE;
    source = from.c_str(); destination = to.c_str();
    if (!writable_path(source) || !writable_path(destination)) return FALSE;
    // Deferred moves escape the session's storage lifetime.
    if (flags & MOVEFILE_DELAY_UNTIL_REBOOT) { SetLastError(ERROR_NOT_SUPPORTED); return FALSE; }
    return MoveFileExW(source, destination, flags);
}

BOOL WINAPI era_DeleteFileW(LPCWSTR path) {
    if (!path || !*path) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    const auto translated = title_path(path);
    if (translated.empty()) return FALSE;
    const std::wstring full = full_path(translated.c_str());
    if (full.empty()) return FALSE; // GetFullPathNameW set the error.
    if (!inside_storage(full)) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    return DeleteFileW(full.c_str());
}

BOOL WINAPI era_DeleteFileA(LPCSTR path) {
    const auto wide = wide_path(path);
    return !wide.empty() && era_DeleteFileW(wide.c_str());
}

BOOL WINAPI era_GetFileAttributesExW(LPCWSTR path, GET_FILEEX_INFO_LEVELS level, LPVOID data) {
    const auto translated = title_path(path);
    return !translated.empty() && GetFileAttributesExW(translated.c_str(), level, data);
}
BOOL WINAPI era_GetFileAttributesExA(LPCSTR path, GET_FILEEX_INFO_LEVELS level, LPVOID data) {
    const auto wide = wide_path(path);
    return !wide.empty() && era_GetFileAttributesExW(wide.c_str(), level, data);
}
BOOL WINAPI era_GetDiskFreeSpaceExW(LPCWSTR path, PULARGE_INTEGER available, PULARGE_INTEGER total, PULARGE_INTEGER free) {
    if (!path) return GetDiskFreeSpaceExW(path, available, total, free);
    const auto translated = title_path(path);
    return !translated.empty() && GetDiskFreeSpaceExW(translated.c_str(), available, total, free);
}
HANDLE WINAPI era_FindFirstFileExA(LPCSTR path, FINDEX_INFO_LEVELS level, LPVOID data,
                                  FINDEX_SEARCH_OPS search, LPVOID filter, DWORD flags) {
    const auto wide = wide_path(path);
    const auto translated = wide.empty() ? std::wstring{} : title_path(wide.c_str());
    if (translated.empty()) return INVALID_HANDLE_VALUE;
    if (!data || (level != FindExInfoStandard && level != FindExInfoBasic)) {
        SetLastError(ERROR_INVALID_PARAMETER); return INVALID_HANDLE_VALUE;
    }
    WIN32_FIND_DATAW found{};
    const HANDLE handle = FindFirstFileExW(translated.c_str(), level, &found, search, filter, flags);
    if (handle == INVALID_HANDLE_VALUE) return handle;
    const DWORD error = GetLastError();
    auto* result = static_cast<WIN32_FIND_DATAA*>(data);
    static_assert(offsetof(WIN32_FIND_DATAA, cFileName) == offsetof(WIN32_FIND_DATAW, cFileName));
    std::memcpy(result, &found, offsetof(WIN32_FIND_DATAA, cFileName));
    if (!WideCharToMultiByte(CP_ACP, 0, found.cFileName, -1, result->cFileName, MAX_PATH, nullptr, nullptr) ||
        !WideCharToMultiByte(CP_ACP, 0, found.cAlternateFileName, -1, result->cAlternateFileName, 14, nullptr, nullptr)) {
        const DWORD conversion_error = GetLastError(); FindClose(handle); SetLastError(conversion_error);
        return INVALID_HANDLE_VALUE;
    }
    SetLastError(error);
    return handle;
}

// ERA semantics: the title is the process image, so NULL names the title, not
// the launcher. Named lookups are host modules.
HMODULE WINAPI era_GetModuleHandleW(LPCWSTR name) {
    if (!name) {
        if (HMODULE title = title_module.load()) return title;
    }
    return GetModuleHandleW(name);
}

} // namespace

HRESULT map_title_esram(UINT flags, void* address, UINT pages, const UINT* page_list) {
    const SIZE_T unit = flags == 1 ? esram_page_bytes : flags == 2 ? SIZE_T{4} << 20 : 0;
    if (!unit || !address || !pages || pages > esram_bytes / unit || reinterpret_cast<std::uintptr_t>(address) % unit)
        return E_INVALIDARG;
    try {
        std::vector<UINT> input(pages);
        SIZE_T read = 0;
        if (page_list && (!ReadProcessMemory(GetCurrentProcess(), page_list, input.data(), input.size() * sizeof(UINT), &read) ||
            read != input.size() * sizeof(UINT))) return E_INVALIDARG;
        if (page_list && std::any_of(input.begin(), input.end(), [unit](UINT page) { return page >= esram_bytes / unit; })) return E_INVALIDARG;
        const auto at = reinterpret_cast<std::uintptr_t>(address);
        const SIZE_T bytes = pages * unit;
        std::lock_guard lock(memory_mutex);
        auto region = title_regions.upper_bound(at);
        if (region == title_regions.begin()) return HRESULT_FROM_WIN32(ERROR_INVALID_ADDRESS);
        --region;
        if (!region->second.graphics || region->second.xmem || at - region->first >= region->second.size ||
            bytes > region->second.size - (at - region->first)) return HRESULT_FROM_WIN32(ERROR_INVALID_ADDRESS);
        for (const auto& [range, window] : region->second.windows)
            if ((window.active || window.cpu) && at - region->first < range.first + range.second &&
                range.first < at - region->first + bytes) return HRESULT_FROM_WIN32(ERROR_BUSY);
        struct Change { std::uintptr_t address; UINT before, after; bool changed = false; };
        std::vector<Change> changes;
        changes.reserve(bytes / esram_page_bytes);
        std::map<std::uintptr_t, GpuBacking> staged;
        for (SIZE_T offset = 0; offset < bytes; offset += esram_page_bytes) {
            const auto target = at + offset;
            auto found = region->second.commits.upper_bound(target);
            UINT before = UINT_MAX;
            if (found != region->second.commits.begin()) {
                --found;
                if (target - found->first < found->second.size) {
                    if (found->first != target || found->second.size != esram_page_bytes || found->second.esram_page == UINT_MAX)
                        return HRESULT_FROM_WIN32(ERROR_INVALID_ADDRESS);
                    if (found->second.gpu_pins) return HRESULT_FROM_WIN32(ERROR_BUSY);
                    before = found->second.esram_page;
                }
            }
            const UINT after = page_list ? UINT((SIZE_T(input[offset / unit]) * unit + offset % unit) / esram_page_bytes) : UINT_MAX;
            for (const UINT physical : {before, after})
                if (physical != UINT_MAX && (esram.pages[physical].gpu || esram.pages[physical].cpu)) return HRESULT_FROM_WIN32(ERROR_BUSY);
            changes.push_back({target, before, after});
            if (before == UINT_MAX && after != UINT_MAX) staged.emplace(target, GpuBacking{esram_page_bytes});
        }
        const auto map = host_map3(); const auto unmap = host_unmap2();
        if (!map || !unmap) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
        if (page_list && !esram.mapping.IsValid()) {
            if (!memory_capacity(esram_bytes)) return E_OUTOFMEMORY;
            const HANDLE handle = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, DWORD(esram_bytes), nullptr);
            if (!handle) return HRESULT_FROM_WIN32(GetLastError());
            esram.mapping.Attach(handle);
            // The entire retained pool is committed once; virtual aliases cost nothing.
            title_committed += esram_bytes;
        }
        if(std::any_of(changes.begin(),changes.end(),[](const auto& change){return change.before!=change.after;})) {
            const auto hr=reset_gpu_tracking();if(FAILED(hr))return hr;
        }
        DWORD failure = 0;
        for (auto& change : changes) {
            if (change.before == change.after) continue;
            if (change.before != UINT_MAX) {
                if (!unmap(GetCurrentProcess(), reinterpret_cast<void*>(change.address), MEM_PRESERVE_PLACEHOLDER)) { failure = GetLastError(); break; }
                change.changed = true;
            } else {
                MEMORY_BASIC_INFORMATION info{};
                if (!VirtualQuery(reinterpret_cast<void*>(change.address), &info, sizeof info) || info.State != MEM_RESERVE) {
                    failure = ERROR_INVALID_ADDRESS; break;
                }
                const auto start = reinterpret_cast<std::uintptr_t>(info.BaseAddress);
                if (change.address > start && !VirtualFree(info.BaseAddress, change.address - start, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
                    failure = GetLastError(); break;
                }
                if (start + info.RegionSize - change.address > esram_page_bytes &&
                    !VirtualFree(reinterpret_cast<void*>(change.address), esram_page_bytes, MEM_RELEASE | MEM_PRESERVE_PLACEHOLDER)) {
                    failure = GetLastError(); break;
                }
            }
            if (change.after != UINT_MAX && !map(esram.mapping.Get(), GetCurrentProcess(), reinterpret_cast<void*>(change.address),
                UINT64(change.after) * esram_page_bytes, esram_page_bytes, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0)) {
                failure = GetLastError(); break;
            }
            change.changed = true;
        }
        if (failure) {
            // Restore completed remaps before exposing the failure. No bytes in
            // the physical pool are overwritten by mapping or rollback itself.
            for (auto change = changes.rbegin(); change != changes.rend(); ++change) if (change->changed) {
                MEMORY_BASIC_INFORMATION info{};
                VirtualQuery(reinterpret_cast<void*>(change->address), &info, sizeof info);
                bool restored = info.State != MEM_COMMIT || unmap(GetCurrentProcess(), reinterpret_cast<void*>(change->address), MEM_PRESERVE_PLACEHOLDER);
                if (restored && change->before != UINT_MAX)
                    restored = map(esram.mapping.Get(), GetCurrentProcess(), reinterpret_cast<void*>(change->address),
                        UINT64(change->before) * esram_page_bytes, esram_page_bytes, MEM_REPLACE_PLACEHOLDER, PAGE_READWRITE, nullptr, 0) != nullptr;
                if (!restored) {
                    std::fputs("xone-era: ESRAM mapping rollback failed; refusing inconsistent address state\n", stderr);
                    TerminateProcess(GetCurrentProcess(), 126);
                }
            }
            return HRESULT_FROM_WIN32(failure);
        }
        for (const auto& change : changes) {
            if (change.after == UINT_MAX) region->second.commits.erase(change.address);
            else {
                if (change.before == UINT_MAX) region->second.commits.insert(staged.extract(change.address));
                if(change.before!=change.after) region->second.commits.at(change.address).mirror.reset();
                auto& commit = region->second.commits.at(change.address);
                commit.esram_page = change.after; commit.guest_protection = PAGE_READWRITE;
            }
        }
        return S_OK;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}

static HRESULT acquire_gpu_resource(void* address, std::size_t size, ID3D12Device* device, TitleGpuResource& out,
                                    D3D12_RESOURCE_FLAGS flags, bool aligned, const TitleTextureLayout* texture) {
    if (!address || !size || !device || out.resource) return E_INVALIDARG;
    if (flags & ~D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) return DXGI_ERROR_UNSUPPORTED;
    std::lock_guard lock(memory_mutex);
    const auto at = reinterpret_cast<std::uintptr_t>(address);
    auto found = title_regions.upper_bound(at);
    if (found == title_regions.begin()) return E_INVALIDARG;
    --found;
    auto base = found->first;
    auto* backing = static_cast<GpuBacking*>(&found->second);
    bool composite = false;
    if (!found->second.xmem) {
        const auto offset = at - found->first;
        if (!found->second.graphics || offset >= found->second.size || size > found->second.size - offset) return E_INVALIDARG;
        if (aligned) {
            auto cursor = at;
            while (cursor < at + size) {
                auto commit = found->second.commits.upper_bound(cursor);
                if (commit == found->second.commits.begin()) return E_INVALIDARG;
                --commit;
                const auto inside = cursor - commit->first;
                if (inside >= commit->second.size) return E_INVALIDARG;
                if (commit->second.gpu_pins) return HRESULT_FROM_WIN32(ERROR_BUSY);
                cursor += (std::min)(SIZE_T(at + size - cursor), commit->second.size - inside);
            }
            composite = true;
        } else {
            if (found->second.gpu_pins) return HRESULT_FROM_WIN32(ERROR_BUSY);
            auto commit = found->second.commits.upper_bound(at);
            if (!found->second.graphics || commit == found->second.commits.begin()) return E_INVALIDARG;
            --commit;
            if (commit->second.esram_page != UINT_MAX) return DXGI_ERROR_UNSUPPORTED; // Physical aliases use the coherent aligned path.
            base = commit->first;
            backing = &commit->second;
        }
    }
    auto& region = *backing;
    const auto offset = at - base;
    if (region.retiring || offset >= region.size || size > region.size - offset) return E_INVALIDARG;
    if (region.gpu_pins && (!aligned || region.windows.empty())) return HRESULT_FROM_WIN32(ERROR_BUSY);
    Microsoft::WRL::ComPtr<IUnknown> identity;
    HRESULT hr = device->QueryInterface(IID_PPV_ARGS(&identity));
    if (FAILED(hr)) return hr;
    if (region.device && region.device.Get() != identity.Get()) return DXGI_ERROR_INVALID_CALL;
    const auto canonical_flags = aligned ? D3D12_RESOURCE_FLAG_NONE : flags;
    if (region.buffer && (region.buffer->GetDesc().Flags & canonical_flags) != canonical_flags) return DXGI_ERROR_UNSUPPORTED;
    if (!region.buffer && !composite) {
        Microsoft::WRL::ComPtr<ID3D12Device3> device3;
        hr = device->QueryInterface(IID_PPV_ARGS(&device3));
        if (FAILED(hr)) return hr;
        Microsoft::WRL::ComPtr<ID3D12Heap> heap;
        hr = region.mapping.IsValid() ? device3->OpenExistingHeapFromFileMapping(region.mapping.Get(), IID_PPV_ARGS(&heap)) :
            device3->OpenExistingHeapFromAddress(reinterpret_cast<void*>(base), IID_PPV_ARGS(&heap));
        if (FAILED(hr)) return hr; // Never substitute a disconnected GPU allocation.
        const auto heap_desc = heap->GetDesc();
        const auto state = heap_desc.Properties.Type == D3D12_HEAP_TYPE_UPLOAD ?
            D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COMMON;
        if (state == D3D12_RESOURCE_STATE_GENERIC_READ && (flags || aligned)) return DXGI_ERROR_UNSUPPORTED;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = region.size;
        desc.Height = 1; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = canonical_flags;
        if (heap_desc.Flags & D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER)
            desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
        Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
        hr = device->CreatePlacedResource(heap.Get(), 0, &desc, state, nullptr, IID_PPV_ARGS(&buffer));
        if (FAILED(hr)) return hr;
        region.heap = std::move(heap);
        region.buffer = std::move(buffer);
        region.device = std::move(identity);
        region.initial_state = state;
    }
    if (aligned) {
        if (region.initial_state != D3D12_RESOURCE_STATE_COMMON) return DXGI_ERROR_UNSUPPORTED;
        try {
            const auto key = std::make_pair(static_cast<SIZE_T>(offset), static_cast<SIZE_T>(size));
            auto window = region.windows.end();
            if (!texture) {
                const auto range = region.windows.equal_range(key);
                for (auto candidate = range.first; candidate != range.second; ++candidate)
                    if (!candidate->second.texture.context) { window = candidate; break; }
            }
            if (window == region.windows.end()) {
                D3D12_HEAP_PROPERTIES heap{};
                heap.Type = D3D12_HEAP_TYPE_DEFAULT;
                D3D12_RESOURCE_DESC desc{};
                desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                if (size > UINT64_MAX - 255) return E_INVALIDARG;
                desc.Width = (size + 255) & ~UINT64{255}; // CBVs may cover a padded final 256-byte block.
                desc.Height = 1; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
                desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; desc.Flags = flags;
                GpuWindow entry;
                entry.staged_buffer = composite && !texture;
                if (texture) { entry.texture = *texture; desc = texture->resource_desc; }
                hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON,
                    nullptr, IID_PPV_ARGS(&entry.resource));
                if (FAILED(hr)) return hr;
                window = region.windows.emplace(key, std::move(entry));
            } else if ((window->second.resource->GetDesc().Flags & flags) != flags) return DXGI_ERROR_UNSUPPORTED;
            ++window->second.owners;
            ++region.gpu_pins;
            if (composite) region.device = identity;
            out.resource = window->second.resource.Get(); out.resource->AddRef();
            out.allocation = reinterpret_cast<void*>(base);
            out.representation = &window->second;
            out.canonical_offset = offset; out.size = size;
            out.composite = composite;
            return S_OK;
        } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
    }
    ++region.gpu_pins;
    out.resource = region.buffer.Get();
    out.resource->AddRef();
    out.allocation = reinterpret_cast<void*>(base);
    out.offset = offset;
    out.initial_state = region.initial_state;
    return S_OK;
}

HRESULT acquire_title_gpu_buffer(void* address, std::size_t size, ID3D12Device* device, TitleGpuResource& out,
                                 D3D12_RESOURCE_FLAGS flags, bool aligned) {
    return acquire_gpu_resource(address, size, device, out, flags, aligned, nullptr);
}
HRESULT acquire_title_gpu_texture(void* address, const TitleTextureLayout& layout, ID3D12Device* device, TitleGpuResource& out) {
    const auto& d = layout.resource_desc;
    if (!layout.context || !layout.copy || !layout.bytes || layout.bytes > (1u << 30) || !layout.alignment ||
        (layout.alignment & (layout.alignment - 1)) || reinterpret_cast<std::uintptr_t>(address) % layout.alignment) return E_INVALIDARG;
    if ((d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D && d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE3D) ||
        !d.Width || !d.Height || !d.DepthOrArraySize || !d.MipLevels || !d.SampleDesc.Count || d.SampleDesc.Quality ||
        (d.Flags & ~(D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) ||
        d.Layout != D3D12_TEXTURE_LAYOUT_UNKNOWN) return DXGI_ERROR_UNSUPPORTED;
    if (d.SampleDesc.Count != 1 && (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.MipLevels != 1 ||
        d.DepthOrArraySize != 1 || d.SampleDesc.Count > 8 || (d.SampleDesc.Count & (d.SampleDesc.Count - 1)) ||
        !((d.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) ||
          ((d.Format==DXGI_FORMAT_R32G8X24_TYPELESS || d.Format==DXGI_FORMAT_R32_TYPELESS) && (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))) ||
        (d.Format != DXGI_FORMAT_R11G11B10_FLOAT && d.Format != DXGI_FORMAT_R10G10B10A2_UNORM &&
         d.Format != DXGI_FORMAT_R8G8B8A8_UNORM && d.Format!=DXGI_FORMAT_R32G8X24_TYPELESS &&
         d.Format!=DXGI_FORMAT_R16G16B16A16_TYPELESS && d.Format!=DXGI_FORMAT_R32_TYPELESS)))
        return DXGI_ERROR_UNSUPPORTED;
    return acquire_gpu_resource(address, layout.bytes, device, out, D3D12_RESOURCE_FLAG_NONE, true, &layout);
}

namespace {
GpuBacking* gpu_backing(const TitleGpuResource& buffer) {
    const auto base = reinterpret_cast<std::uintptr_t>(buffer.allocation);
    auto found = title_regions.upper_bound(base);
    if (found == title_regions.begin()) return nullptr;
    --found;
    if (buffer.composite) return found->first == base && found->second.graphics && !found->second.xmem ? &found->second : nullptr;
    if (found->second.xmem) return found->first == base ? &found->second : nullptr;
    auto commit = found->second.commits.find(base);
    return commit == found->second.commits.end() ? nullptr : &commit->second;
}
GpuWindow* gpu_window(const TitleGpuResource& buffer, GpuBacking* backing) {
    if (!backing || !buffer.representation || !buffer.resource) return nullptr;
    const auto range = backing->windows.equal_range({buffer.canonical_offset, buffer.size});
    for (auto found = range.first; found != range.second; ++found)
        if (&found->second == buffer.representation && found->second.resource.Get() == buffer.resource) return &found->second;
    return nullptr;
}
HRESULT physical_pages(const TitleGpuResource& buffer, std::array<bool, 512>& pages, bool reject_duplicates) {
    pages.fill(false);
    if (!buffer.composite) return S_OK;
    const auto base = reinterpret_cast<std::uintptr_t>(buffer.allocation);
    const auto region = title_regions.find(base);
    if (region == title_regions.end()) return E_INVALIDARG;
    auto cursor = base + buffer.canonical_offset;
    const auto end = cursor + buffer.size;
    while (cursor < end) {
        auto commit = region->second.commits.upper_bound(cursor);
        if (commit == region->second.commits.begin()) return E_INVALIDARG;
        --commit;
        const auto inside = cursor - commit->first;
        if (inside >= commit->second.size) return E_INVALIDARG;
        const UINT page = commit->second.esram_page;
        if (page != UINT_MAX) {
            if (pages[page] && reject_duplicates) return DXGI_ERROR_UNSUPPORTED;
            pages[page] = true;
        }
        cursor += (std::min)(SIZE_T(end - cursor), commit->second.size - inside);
    }
    return S_OK;
}
void release_physical(GpuWindow& window, TitleGpuResource* owner, bool cpu) {
    for (UINT page = 0; page < window.physical_pages.size(); ++page) if (window.physical_pages[page]) {
        auto*& gate = cpu ? esram.pages[page].cpu : esram.pages[page].gpu;
        if (gate == owner) gate = nullptr;
    }
}
HRESULT completed_fence(GpuBacking* backing, ID3D12Fence* fence, UINT64 value) {
    if (!fence || value == UINT64_MAX) return E_INVALIDARG;
    Microsoft::WRL::ComPtr<IUnknown> device;
    const HRESULT hr = fence->GetDevice(IID_PPV_ARGS(&device));
    if (FAILED(hr)) return hr;
    if (device.Get() != backing->device.Get()) return DXGI_ERROR_INVALID_CALL;
    const UINT64 completed = fence->GetCompletedValue();
    if (completed == UINT64_MAX) return DXGI_ERROR_DEVICE_REMOVED;
    return completed < value ? E_PENDING : S_OK;
}
void gpu_transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    if (before == after) return;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    list->ResourceBarrier(1, &barrier);
}
struct WatchedPage {
    std::vector<std::uintptr_t> aliases;
    std::vector<GpuWindow*> windows;
    DWORD protection = PAGE_READWRITE;
};
std::map<WatchKey,WatchedPage> watched_physical;
std::map<std::uintptr_t,WatchKey> watched_virtual;
HRESULT protect_watched(WatchedPage& page,DWORD protection) {
    if(page.protection==protection)return S_OK;
    for(auto address:page.aliases) {
        DWORD before=0;
        if(!VirtualProtect(reinterpret_cast<void*>(address),4096,protection,&before)) {
            const DWORD error=GetLastError();page.protection=0;return HRESULT_FROM_WIN32(error);
        }
    }
    page.protection=protection;
    return S_OK;
}
HRESULT seal_watched(WatchedPage& page) {
    bool inaccessible=false;
    for(auto* window:page.windows) inaccessible|=window->pending_cpu || (window->active && !window->read_only);
    return protect_watched(page,inaccessible ? PAGE_NOACCESS : PAGE_READONLY);
}
HRESULT access_watched(WatchedPage& page,bool write) {
    for(auto* window:page.windows) {
        if(window->active || window->preparing || window->completing) return E_PENDING;
        const HRESULT hr=flush_gpu_window(*window);if(FAILED(hr))return hr;
    }
    if(write) {
        for(auto* window:page.windows) window->resident=false;
    }
    return protect_watched(page,write ? PAGE_READWRITE : PAGE_READONLY);
}
LONG CALLBACK gpu_memory_fault(EXCEPTION_POINTERS* exception) {
    struct LastError {DWORD value=GetLastError();~LastError(){SetLastError(value);}} last_error;
    static thread_local bool handling=false;
    if(handling)return EXCEPTION_CONTINUE_SEARCH;
    const auto* record=exception->ExceptionRecord;
    if(record->ExceptionCode!=EXCEPTION_ACCESS_VIOLATION || record->NumberParameters<2 || record->ExceptionInformation[0]>1)
        return EXCEPTION_CONTINUE_SEARCH;
    const auto address=std::uintptr_t(record->ExceptionInformation[1])&~std::uintptr_t(4095);
    try {
        std::unique_lock lock(memory_mutex);
        auto entry=watched_virtual.find(address);if(entry==watched_virtual.end())return EXCEPTION_CONTINUE_SEARCH;
        struct Handling {bool& value;Handling(bool& v):value(v){value=true;}~Handling(){value=false;}} guard(handling);
        const auto retired_fault=[&] {
            MEMORY_BASIC_INFORMATION info{};
            return VirtualQuery(reinterpret_cast<void*>(address),&info,sizeof info) && info.State==MEM_COMMIT &&
                (info.Protect==PAGE_READWRITE || (record->ExceptionInformation[0]==0 && info.Protect==PAGE_READONLY)) ?
                EXCEPTION_CONTINUE_EXECUTION : EXCEPTION_CONTINUE_SEARCH;
        };
        // A second fault may already be queued when another thread retires the watch.
        if(!entry->second.first)return retired_fault();
        const auto ready=[&] {
            const auto current=watched_virtual.find(address);if(current==watched_virtual.end() || !current->second.first)return true;
            for(auto* window:watched_physical.at(current->second).windows)
                if(window->active || window->preparing || window->completing)return false;
            return true;
        };
        if(!ready()) {
            for(auto* window:watched_physical.at(entry->second).windows)
                if(window->active_thread==GetCurrentThreadId())return EXCEPTION_CONTINUE_SEARCH;
            if(!memory_changed.wait_for(lock,std::chrono::seconds(5),ready))return EXCEPTION_CONTINUE_SEARCH;
            entry=watched_virtual.find(address);if(entry==watched_virtual.end())return EXCEPTION_CONTINUE_SEARCH;
            if(!entry->second.first)return retired_fault();
        }
        const HRESULT hr=access_watched(watched_physical.at(entry->second),record->ExceptionInformation[0]==1);
        if(FAILED(hr)) {
            std::fprintf(stderr,"xone-era: tracked CPU access failed 0x%08lX at %p\n",hr,reinterpret_cast<void*>(address));
            return EXCEPTION_CONTINUE_SEARCH;
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    } catch(...) { return EXCEPTION_CONTINUE_SEARCH; }
}
HRESULT initialize_gpu_tracking(TitleGpuResource& resource,GpuWindow& window) try {
    if(window.tracked)return S_OK;
    if(!window.texture.context || !window.texture.demand_coherence)return S_FALSE;
    const auto base=reinterpret_cast<std::uintptr_t>(resource.allocation);
    auto region=title_regions.upper_bound(base);if(region==title_regions.begin())return E_INVALIDARG;--region;
    if(region->second.xmem)return S_FALSE; // Ordinary allocations retain the existing synchronous path.
    struct Handler {
        PVOID token=AddVectoredExceptionHandler(1,gpu_memory_fault);
        ~Handler(){if(token)RemoveVectoredExceptionHandler(token);}
    };
    static Handler handler;
    if(!handler.token)return HRESULT_FROM_WIN32(ERROR_NOT_ENOUGH_MEMORY);
    const auto start=base+resource.canonical_offset,end=start+resource.size;
    window.tracked_resource=resource;
    window.tracked=true;
    for(auto cursor=start;cursor<end;) {
        auto commit=region->second.commits.upper_bound(cursor);if(commit==region->second.commits.begin()){detach_gpu_tracking(window);return E_INVALIDARG;}--commit;
        auto& backing=commit->second;
        const SIZE_T inside=cursor-commit->first;
        if(inside>=backing.size){detach_gpu_tracking(window);return E_INVALIDARG;}
        const bool esram_backing=backing.esram_page!=UINT_MAX;
        if(!backing.mirror) {
            const UINT64 offset=esram_backing ? UINT64(backing.esram_page)*esram_page_bytes : 0;
            HANDLE mapping=esram_backing ? esram.mapping.Get() : backing.mapping.Get();
            void* view=MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,DWORD(offset>>32),DWORD(offset),backing.size);
            if(!view){const DWORD error=GetLastError();detach_gpu_tracking(window);return HRESULT_FROM_WIN32(error);}
            backing.mirror=std::shared_ptr<void>(view,[](void* p){UnmapViewOfFile(p);});
        }
        const SIZE_T length=(std::min)(SIZE_T(end-cursor),backing.size-inside);
        window.segments.push_back({backing.mirror,inside,length,SIZE_T(cursor-start)});
        for(auto page=cursor&~std::uintptr_t(4095);page<cursor+length;page+=4096) {
            const SIZE_T physical=esram_backing ? SIZE_T(backing.esram_page)*esram_page_bytes+page-commit->first : page-commit->first;
            const WatchKey key{reinterpret_cast<std::uintptr_t>(esram_backing ? static_cast<void*>(&esram) : static_cast<void*>(&backing)),physical};
            // Commit boundaries are page-aligned; physical_pages already rejects repeated ESRAM pages.
            window.watch_keys.push_back(key);
            auto& watch=watched_physical[key];
            if(watch.aliases.empty()) {
                if(esram_backing) {
                    for(auto& [reservation,parent]:title_regions) for(auto& [alias,span]:parent.commits)
                        if(span.esram_page==backing.esram_page)watch.aliases.push_back(alias+(page-commit->first));
                } else watch.aliases.push_back(page);
                for(auto alias:watch.aliases)watched_virtual.insert_or_assign(alias,key);
            }
            watch.windows.push_back(&window);
            const HRESULT hr=seal_watched(watch);if(FAILED(hr)){detach_gpu_tracking(window);return hr;}
        }
        cursor+=length;
    }
    return S_OK;
} catch(const std::bad_alloc&) {detach_gpu_tracking(window);return E_OUTOFMEMORY;}
void detach_gpu_tracking(GpuWindow& window) {
    for(const auto& key:window.watch_keys) {
        auto found=watched_physical.find(key);if(found==watched_physical.end())continue;
        auto& page=found->second;std::erase(page.windows,&window);
        if(page.windows.empty()) {
            if(FAILED(protect_watched(page,PAGE_READWRITE)))__fastfail(FAST_FAIL_FATAL_APP_EXIT);
            for(auto alias:page.aliases)watched_virtual.at(alias)={};
            watched_physical.erase(found);
        } else if(FAILED(seal_watched(page)))__fastfail(FAST_FAIL_FATAL_APP_EXIT);
    }
    window.watch_keys.clear();window.segments.clear();window.tracked=false;window.resident=false;window.pending_cpu=false;
}
HRESULT reset_gpu_tracking() {
    for(auto& [key,page]:watched_physical)for(auto* window:page.windows)
        if(window->active || window->preparing || window->completing)return E_PENDING;
    for(auto& [key,page]:watched_physical)for(auto* window:page.windows){const auto hr=flush_gpu_window(*window);if(FAILED(hr))return hr;}
    for(auto& [key,page]:watched_physical){const auto hr=protect_watched(page,PAGE_READWRITE);if(FAILED(hr))return hr;}
    for(auto& [key,page]:watched_physical)for(auto* window:page.windows){window->tracked=false;window->resident=false;window->watch_keys.clear();window->segments.clear();}
    for(auto& [address,key]:watched_virtual)key={};
    watched_physical.clear();memory_changed.notify_all();return S_OK;
}
void forget_tracked_addresses(std::uintptr_t start,SIZE_T size) {
    for(auto entry=watched_virtual.lower_bound(start);entry!=watched_virtual.end() && entry->first-start<size;)
        entry=watched_virtual.erase(entry);
}
HRESULT cpu_memory_access(const void* address,SIZE_T size,bool write) {
    if(!size)return S_OK;
    const auto start=reinterpret_cast<std::uintptr_t>(address);if(size>UINTPTR_MAX-start)return E_INVALIDARG;
    std::lock_guard lock(memory_mutex);
    auto current=watched_virtual.lower_bound(start&~std::uintptr_t(4095));
    for(;current!=watched_virtual.end() && current->first<start+size;++current) {
        if(!current->second.first)continue;
        const auto hr=access_watched(watched_physical.at(current->second),write);if(FAILED(hr))return hr;
    }
    return S_OK;
}
HRESULT prepare_gpu_tracking(TitleGpuResource& resource,GpuWindow& window,const TitleGpuClearState& clear,bool discard) {
    const auto hr=initialize_gpu_tracking(resource,window);if(hr!=S_OK)return hr;
    for(const auto& key:window.watch_keys)for(auto* alias:watched_physical.at(key).windows)
        if(alias!=&window){const auto flushed=flush_gpu_window(*alias);if(FAILED(flushed))return flushed;}
    window.reuse=window.resident && window.resident_clear.depth==clear.depth && window.resident_clear.stencil==clear.stencil &&
        window.resident_clear.known==clear.known && window.resident_clear.bypass_htile==clear.bypass_htile;
    if(!window.reuse && !discard){const auto flushed=flush_gpu_window(window);if(FAILED(flushed))return flushed;}
    for(const auto& key:window.watch_keys){const auto sealed=seal_watched(watched_physical.at(key));if(FAILED(sealed))return sealed;}
    window.active_clear=clear;return S_OK;
}
void mirror_copy(GpuWindow& window,void* data,bool into_mirror) {
    for(const auto& span:window.segments) {
        auto* mirror=static_cast<unsigned char*>(span.view.get())+span.offset;
        auto* linear=static_cast<unsigned char*>(data)+span.logical;
        std::memcpy(into_mirror ? mirror : linear,into_mirror ? linear : mirror,span.length);
    }
}
}

static HRESULT claim_gpu_window(TitleGpuResource& buffer, ID3D12GraphicsCommandList* list, D3D12_RESOURCE_STATES state,
                                bool texture, GpuBacking*& backing, GpuWindow*& window) {
    if (!list || list->GetType() == D3D12_COMMAND_LIST_TYPE_BUNDLE) return E_INVALIDARG;
    if (list->GetType() == D3D12_COMMAND_LIST_TYPE_COPY && state != D3D12_RESOURCE_STATE_COMMON &&
        state != D3D12_RESOURCE_STATE_COPY_SOURCE && state != D3D12_RESOURCE_STATE_COPY_DEST) return DXGI_ERROR_UNSUPPORTED;
    backing = gpu_backing(buffer);
    window = gpu_window(buffer, backing);
    if (!window || static_cast<bool>(window->texture.context) != texture) return E_INVALIDARG;
    if (texture && window->texture.resource_desc.SampleDesc.Count > 1 && list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT)
        return DXGI_ERROR_UNSUPPORTED;
    if (backing->retiring) return E_INVALIDARG;
    Microsoft::WRL::ComPtr<IUnknown> device;
    HRESULT hr = list->GetDevice(IID_PPV_ARGS(&device));
    if (FAILED(hr)) return hr;
    if (device.Get() != backing->device.Get()) return DXGI_ERROR_INVALID_CALL;
    const auto busy=[&](const char* reason,SIZE_T offset,SIZE_T bytes,const GpuWindow* alias) {
        static std::atomic<UINT> observations{0};
        if(observations.fetch_add(1)<64)std::fprintf(stderr,"xone-era: GPU claim busy reason=%s request=%p+0x%llX bytes=0x%llX native=%p list=%p owner_offset=0x%llX owner_bytes=0x%llX active=%p cpu=%u owner_native=%p owner_list=%p\n",
            reason,buffer.allocation,UINT64(buffer.canonical_offset),UINT64(buffer.size),buffer.resource,list,UINT64(offset),UINT64(bytes),
            alias ? alias->active : nullptr,alias ? UINT(alias->cpu != nullptr) : 0,alias ? alias->resource.Get() : nullptr,alias ? alias->list.Get() : nullptr);
        return HRESULT_FROM_WIN32(ERROR_BUSY);
    };
    for (const auto& [range, alias] : backing->windows) {
        if (alias.cpu) return busy("CPU",range.first,range.second,&alias);
        // The canonical parent has one D3D12 state. Independent queues/lists
        // must finish their fence before another list can transition it.
        if (alias.active && alias.list.Get() != list) return busy("other list",range.first,range.second,&alias);
        if (alias.active && buffer.canonical_offset < range.first + range.second &&
            range.first < buffer.canonical_offset + buffer.size) return busy("overlap",range.first,range.second,&alias);
    }
    if ((state & D3D12_RESOURCE_STATE_UNORDERED_ACCESS) &&
        !(window->resource->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS)) return DXGI_ERROR_UNSUPPORTED;
    if ((state & D3D12_RESOURCE_STATE_RENDER_TARGET) &&
        !(window->resource->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET)) return DXGI_ERROR_UNSUPPORTED;
    if ((state & (D3D12_RESOURCE_STATE_DEPTH_WRITE | D3D12_RESOURCE_STATE_DEPTH_READ)) &&
        !(window->resource->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) return DXGI_ERROR_UNSUPPORTED;
    if (texture && (state & ~(D3D12_RESOURCE_STATE_COPY_SOURCE | D3D12_RESOURCE_STATE_COPY_DEST |
        D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS | D3D12_RESOURCE_STATE_RENDER_TARGET |
        D3D12_RESOURCE_STATE_DEPTH_WRITE | D3D12_RESOURCE_STATE_DEPTH_READ))) return DXGI_ERROR_UNSUPPORTED;
    hr = physical_pages(buffer, window->physical_pages, true);
    if (FAILED(hr)) return hr;
    for (UINT page = 0; page < window->physical_pages.size(); ++page)
        if (window->physical_pages[page] && (esram.pages[page].gpu || esram.pages[page].cpu)) {
            std::fprintf(stderr,"xone-era: ESRAM claim busy page=%u request=%p owner=%p cpu=%u\n",page,&buffer,esram.pages[page].gpu,UINT(esram.pages[page].cpu != nullptr));
            return busy("ESRAM page",page,esram_page_bytes,nullptr);
        }
    for (UINT page = 0; page < window->physical_pages.size(); ++page)
        if (window->physical_pages[page]) esram.pages[page].gpu = &buffer;
    window->active = &buffer; window->list = list; window->state = state; window->copied_back = false;
    window->active_thread=GetCurrentThreadId();
    return S_OK;
}

static HRESULT prepare_staged_buffer(GpuWindow& window, const void* canonical, SIZE_T bytes) {
    if (!bytes || bytes > (1ull << 30)) return DXGI_ERROR_UNSUPPORTED;
    if (!window.upload) {
        Microsoft::WRL::ComPtr<ID3D12Device> device;
        HRESULT hr = window.resource->GetDevice(IID_PPV_ARGS(&device));
        if (FAILED(hr)) return hr;
        auto desc = window.resource->GetDesc(); desc.Flags = D3D12_RESOURCE_FLAG_NONE;
        D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        Microsoft::WRL::ComPtr<ID3D12Resource> upload, readback;
        hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload));
        heap.Type = D3D12_HEAP_TYPE_READBACK;
        if (SUCCEEDED(hr)) hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));
        if (FAILED(hr)) return hr;
        window.upload = std::move(upload); window.readback = std::move(readback); window.staging_bytes = bytes;
    }
    void* mapped = nullptr; const D3D12_RANGE no_read{0, 0};
    const HRESULT hr = window.upload->Map(0, &no_read, &mapped);
    if (FAILED(hr)) return hr;
    std::memcpy(mapped, canonical, bytes);
    const D3D12_RANGE written{0, bytes}; window.upload->Unmap(0, &written);
    return S_OK;
}

static bool gpu_read_only(D3D12_RESOURCE_STATES state);
HRESULT begin_title_gpu_buffer(TitleGpuResource& buffer, ID3D12GraphicsCommandList* list, D3D12_RESOURCE_STATES state,bool read_only) {
    if(read_only && !gpu_read_only(state))return E_INVALIDARG;
    std::lock_guard lock(memory_mutex);
    GpuBacking* backing = nullptr; GpuWindow* window = nullptr;
    const HRESULT hr = claim_gpu_window(buffer, list, state, false, backing, window);
    if (FAILED(hr)) return hr;
    window->read_only=read_only;
    if (window->staged_buffer) {
        const auto* canonical = static_cast<unsigned char*>(buffer.allocation) + buffer.canonical_offset;
        const HRESULT prepared = prepare_staged_buffer(*window, canonical, buffer.size);
        if (FAILED(prepared)) {
            release_physical(*window, &buffer, false); window->active = nullptr; window->list.Reset();
            return prepared;
        }
        gpu_transition(list, buffer.resource, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyBufferRegion(buffer.resource, 0, window->upload.Get(), 0, buffer.size);
        gpu_transition(list, buffer.resource, D3D12_RESOURCE_STATE_COPY_DEST, state);
        return S_OK;
    }
    gpu_transition(list, backing->buffer.Get(), backing->initial_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    gpu_transition(list, buffer.resource, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyBufferRegion(buffer.resource, 0, backing->buffer.Get(), buffer.canonical_offset, buffer.size);
    gpu_transition(list, backing->buffer.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, backing->initial_state);
    gpu_transition(list, buffer.resource, D3D12_RESOURCE_STATE_COPY_DEST, state);
    return S_OK;
}

static bool gpu_read_only(D3D12_RESOURCE_STATES state) {
    constexpr UINT reads=D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER|D3D12_RESOURCE_STATE_INDEX_BUFFER|
        D3D12_RESOURCE_STATE_DEPTH_READ|D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE|D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE|
        D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT|D3D12_RESOURCE_STATE_COPY_SOURCE|D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
    return state!=D3D12_RESOURCE_STATE_COMMON && !(UINT(state)&~reads);
}
HRESULT end_title_gpu_buffer(TitleGpuResource& buffer, ID3D12GraphicsCommandList* list) {
    std::lock_guard lock(memory_mutex);
    auto* backing = gpu_backing(buffer);
    auto* window = gpu_window(buffer, backing);
    if (!window || window->texture.context || window->active != &buffer || window->list.Get() != list || window->copied_back) return E_INVALIDARG;
    if (window->read_only) {
        gpu_transition(list,buffer.resource,window->state,D3D12_RESOURCE_STATE_COMMON);
        window->copied_back=true;return S_OK;
    }
    gpu_transition(list, buffer.resource, window->state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    if (window->staged_buffer) list->CopyBufferRegion(window->readback.Get(), 0, buffer.resource, 0, buffer.size);
    else {
        gpu_transition(list, backing->buffer.Get(), backing->initial_state, D3D12_RESOURCE_STATE_COPY_DEST);
        list->CopyBufferRegion(backing->buffer.Get(), buffer.canonical_offset, buffer.resource, 0, buffer.size);
        gpu_transition(list, backing->buffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, backing->initial_state);
    }
    gpu_transition(list, buffer.resource, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    window->copied_back = true;
    return S_OK;
}

static UINT sample_pixel_bytes(DXGI_FORMAT format) {
    return format==DXGI_FORMAT_R32G8X24_TYPELESS || format==DXGI_FORMAT_R16G16B16A16_TYPELESS ? 8 : 4;
}
static HRESULT prepare_sample_array(GpuWindow& window, ID3D12Device* device, D3D12_RESOURCE_DESC& desc) {
    const UINT samples = desc.SampleDesc.Count;
    const bool depth_stencil=desc.Format==DXGI_FORMAT_R32G8X24_TYPELESS;
    const bool depth_only=desc.Format==DXGI_FORMAT_R32_TYPELESS;
    const bool half_float=desc.Format==DXGI_FORMAT_R16G16B16A16_TYPELESS;
    const UINT64 bytes = desc.Width * desc.Height * samples * sample_pixel_bytes(desc.Format);
    if (!bytes || bytes > (1ull << 30)) return DXGI_ERROR_UNSUPPORTED;
    auto transfer = std::make_unique<SampleTransfer>();
    HRESULT hr = transfer->initialize(device, desc.Format, samples);
    if (FAILED(hr)) return hr;
    desc.SampleDesc = {1, 0}; desc.DepthOrArraySize = static_cast<UINT16>(samples); desc.Alignment = 0;
    if (depth_stencil) { desc.Format=DXGI_FORMAT_R32G32_UINT; desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET; }
    else if (depth_only) { desc.Format=DXGI_FORMAT_R32_UINT; desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET; }
    else if (half_float) desc.Format=DXGI_FORMAT_R16G16B16A16_UINT;
    D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    Microsoft::WRL::ComPtr<ID3D12Resource> array;
    hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&array));
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> srv, rtv;
    const D3D12_DESCRIPTOR_HEAP_DESC srv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, depth_stencil ? 3u : 2u, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    const D3D12_DESCRIPTOR_HEAP_DESC rtv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, samples + 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    if (SUCCEEDED(hr)) hr = device->CreateDescriptorHeap(&srv_desc, IID_PPV_ARGS(&srv));
    if (SUCCEEDED(hr)) hr = device->CreateDescriptorHeap(&rtv_desc, IID_PPV_ARGS(&rtv));
    if (FAILED(hr)) return hr;
    const UINT srv_step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    const UINT rtv_step = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    auto srv_handle = srv->GetCPUDescriptorHandleForHeapStart();
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};
    view.Format = desc.Format; view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY; view.Texture2DArray.ArraySize = samples; view.Texture2DArray.MipLevels = 1;
    device->CreateShaderResourceView(array.Get(), &view, srv_handle);
    view = {}; view.Format = depth_stencil ? DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS : depth_only ? DXGI_FORMAT_R32_FLOAT : desc.Format; view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS; srv_handle.ptr += srv_step;
    device->CreateShaderResourceView(window.resource.Get(), &view, srv_handle);
    if (depth_stencil) {
        srv_handle.ptr+=srv_step; view.Format=DXGI_FORMAT_X32_TYPELESS_G8X24_UINT;
        device->CreateShaderResourceView(window.resource.Get(),&view,srv_handle);
    }
    auto rtv_handle = rtv->GetCPUDescriptorHandleForHeapStart();
    for (UINT sample = 0; sample < samples; ++sample) {
        D3D12_RENDER_TARGET_VIEW_DESC target{};
        target.Format = desc.Format; target.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
        target.Texture2DArray.FirstArraySlice = sample; target.Texture2DArray.ArraySize = 1;
        device->CreateRenderTargetView(array.Get(), &target, rtv_handle); rtv_handle.ptr += rtv_step;
    }
    if (depth_stencil || depth_only) {
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> dsv;
        const D3D12_DESCRIPTOR_HEAP_DESC dsv_desc{D3D12_DESCRIPTOR_HEAP_TYPE_DSV,1,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};
        hr=device->CreateDescriptorHeap(&dsv_desc,IID_PPV_ARGS(&dsv));
        if (FAILED(hr)) return hr;
        D3D12_DEPTH_STENCIL_VIEW_DESC target{};
        target.Format=depth_stencil ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_D32_FLOAT; target.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2DMS;
        device->CreateDepthStencilView(window.resource.Get(),&target,dsv->GetCPUDescriptorHandleForHeapStart());
        window.sample_dsv=std::move(dsv);
    } else {
        D3D12_RENDER_TARGET_VIEW_DESC target{};
        target.Format = desc.Format; target.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
        device->CreateRenderTargetView(window.resource.Get(), &target, rtv_handle);
    }
    window.sample_linear.resize(static_cast<SIZE_T>(bytes));
    window.sample_array = std::move(array); window.sample_transfer = std::move(transfer);
    window.sample_srv = std::move(srv); window.sample_rtv = std::move(rtv);
    window.sample_srv_step = srv_step; window.sample_rtv_step = rtv_step;
    return S_OK;
}

static void interleave_samples(GpuWindow& window, void* mapped, bool into_array) {
    const auto& desc = window.texture.resource_desc;
    const UINT samples = desc.SampleDesc.Count;
    const UINT element=sample_pixel_bytes(desc.Format);
    for (UINT sample = 0; sample < samples; ++sample) {
        const auto& footprint = window.footprints[sample];
        for (UINT y = 0; y < desc.Height; ++y) for (UINT x = 0; x < desc.Width; ++x) {
            auto* linear = window.sample_linear.data() + ((SIZE_T(y) * desc.Width + x) * samples + sample) * element;
            auto* array = static_cast<unsigned char*>(mapped) + footprint.Offset + SIZE_T(y) * footprint.Footprint.RowPitch + x * element;
            std::memcpy(into_array ? array : linear, into_array ? linear : array, element);
        }
    }
}

namespace {
HRESULT record_texture_readback(GpuWindow* window,ID3D12GraphicsCommandList* list,D3D12_RESOURCE_STATES before) {
    if(window->gpu_layout_active) {
        gpu_transition(list,window->resource.Get(),before,D3D12_RESOURCE_STATE_COPY_SOURCE);
        window->layout_transfer->retile(list,window->resource.Get());
        gpu_transition(list,window->resource.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
        return S_OK;
    }
    auto* copy_source = window->sample_array ? window->sample_array.Get() : window->resource.Get();
    if (window->sample_array) {
        gpu_transition(list, window->resource.Get(), before, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        gpu_transition(list, copy_source, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_RENDER_TARGET);
        auto target = window->sample_rtv->GetCPUDescriptorHandleForHeapStart();
        auto source = window->sample_srv->GetGPUDescriptorHandleForHeapStart(); source.ptr += window->sample_srv_step;
        const auto& desc = window->texture.resource_desc;
        for (UINT sample = 0; sample < desc.SampleDesc.Count; ++sample) {
            const HRESULT hr = window->sample_transfer->record(list, window->sample_srv.Get(), source, target,
                UINT(desc.Width), desc.Height, sample, false);
            if (FAILED(hr)) return hr;
            target.ptr += window->sample_rtv_step;
        }
        gpu_transition(list, copy_source, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
    } else gpu_transition(list, window->resource.Get(), before, D3D12_RESOURCE_STATE_COPY_SOURCE);
    for (UINT i = 0; i < window->footprints.size(); ++i) {
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = copy_source; source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        source.SubresourceIndex = i;
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = window->readback.Get(); destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint = window->footprints[i];
        list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    }
    gpu_transition(list, copy_source, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    if (window->sample_array) gpu_transition(list, window->resource.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    return S_OK;
}

HRESULT wait_coherence_copy(CoherenceReadback& copy) {
    auto completed=copy.fence->GetCompletedValue();
    if(completed==UINT64_MAX)return DXGI_ERROR_DEVICE_REMOVED;
    if(completed>=copy.value)return S_OK;
    HRESULT hr=copy.fence->SetEventOnCompletion(copy.value,copy.completed.Get());
    if(FAILED(hr))return hr;
    if(WaitForSingleObject(copy.completed.Get(),5000)!=WAIT_OBJECT_0)return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    completed=copy.fence->GetCompletedValue();
    return completed==UINT64_MAX ? DXGI_ERROR_DEVICE_REMOVED : completed>=copy.value ? S_OK : E_PENDING;
}
HRESULT capture_gpu_image(GpuWindow& window) {
    auto* backing=gpu_backing(window.tracked_resource);if(!backing)return E_INVALIDARG;
    if(!backing->coherence) {
        Microsoft::WRL::ComPtr<ID3D12Device> device;
        auto hr=window.resource->GetDevice(IID_PPV_ARGS(&device));if(FAILED(hr))return hr;
        auto copy=std::make_unique<CoherenceReadback>();
        if(!copy->completed.IsValid())return HRESULT_FROM_WIN32(GetLastError());
        const D3D12_COMMAND_QUEUE_DESC desc{D3D12_COMMAND_LIST_TYPE_DIRECT,0,D3D12_COMMAND_QUEUE_FLAG_NONE,0};
        hr=device->CreateCommandQueue(&desc,IID_PPV_ARGS(&copy->queue));
        if(SUCCEEDED(hr))hr=device->CreateCommandAllocator(desc.Type,IID_PPV_ARGS(&copy->allocator));
        if(SUCCEEDED(hr))hr=device->CreateCommandList(0,desc.Type,copy->allocator.Get(),nullptr,IID_PPV_ARGS(&copy->list));
        if(SUCCEEDED(hr))hr=copy->list->Close();
        if(SUCCEEDED(hr))hr=device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&copy->fence));
        if(FAILED(hr))return hr;
        backing->coherence=std::move(copy);
    }
    auto& copy=*backing->coherence;
    auto hr=wait_coherence_copy(copy);if(FAILED(hr))return hr;
    hr=copy.allocator->Reset();if(SUCCEEDED(hr))hr=copy.list->Reset(copy.allocator.Get(),nullptr);
    if(SUCCEEDED(hr))hr=record_texture_readback(&window,copy.list.Get(),D3D12_RESOURCE_STATE_COMMON);
    if(SUCCEEDED(hr))hr=copy.list->Close();if(FAILED(hr))return hr;
    ID3D12CommandList* lists[]{copy.list.Get()};copy.queue->ExecuteCommandLists(1,lists);
    ++copy.value;hr=copy.queue->Signal(copy.fence.Get(),copy.value);if(FAILED(hr))return hr;
    return wait_coherence_copy(copy);
}
HRESULT flush_gpu_window(GpuWindow& window) {
    if(!window.pending_cpu)return S_OK;
    if(window.completing)return E_PENDING;
    window.completing=true;
    HRESULT hr=S_OK;
    std::vector<unsigned char> commit;
    try {
        hr=capture_gpu_image(window);
        if(FAILED(hr)){window.completing=false;return hr;}
        commit.resize(window.tracked_resource.size);mirror_copy(window,commit.data(),false);
    }
    catch(const std::bad_alloc&){window.completing=false;return E_OUTOFMEMORY;}
    auto* readback=window.gpu_layout_active ? window.layout_transfer->readback() : window.readback.Get();
    const D3D12_RANGE read{0,window.gpu_layout_active ? window.tracked_resource.size : SIZE_T(window.staging_bytes)};
    void* mapped=nullptr;
    hr=readback->Map(0,&read,&mapped);
    if(SUCCEEDED(hr)) {
        try {
            if(window.gpu_layout_active)std::memcpy(commit.data(),mapped,commit.size());
            else if(window.sample_array) {
                interleave_samples(window,mapped,false);
                const auto& desc=window.texture.resource_desc;
                const UINT pitch=UINT(desc.Width)*desc.SampleDesc.Count*sample_pixel_bytes(desc.Format);
                hr=window.texture.copy(window.texture.context.get(),true,window.sample_linear.data(),commit.data(),0,pitch,pitch*desc.Height,nullptr);
            } else for(UINT i=0;i<window.footprints.size();++i) {
                const auto& footprint=window.footprints[i];
                hr=window.texture.copy(window.texture.context.get(),true,static_cast<unsigned char*>(mapped)+footprint.Offset,commit.data(),i,
                    footprint.Footprint.RowPitch,footprint.Footprint.RowPitch*window.rows[i],nullptr);
                if(FAILED(hr))break;
            }
        } catch(const std::bad_alloc&){hr=E_OUTOFMEMORY;}
        const D3D12_RANGE no_write{0,0};readback->Unmap(0,&no_write);
    }
    if(SUCCEEDED(hr)) {
        mirror_copy(window,commit.data(),true);window.pending_cpu=false;
        for(const auto& key:window.watch_keys){hr=seal_watched(watched_physical.at(key));if(FAILED(hr))break;}
    }
    window.completing=false;memory_changed.notify_all();return hr;
}
}
static HRESULT prepare_texture_upload(GpuWindow& window, void* canonical,const TitleGpuClearState& clear,bool discard_contents,
                                     const D3D12_SUBRESOURCE_DATA* input,bool gpu_allowed) {
    try {
        if (!window.upload) {
            Microsoft::WRL::ComPtr<ID3D12Device> device;
            HRESULT hr = window.resource->GetDevice(IID_PPV_ARGS(&device));
            if (FAILED(hr)) return hr;
            auto desc = window.resource->GetDesc();
            if (desc.SampleDesc.Count > 1) {
                hr = prepare_sample_array(window, device.Get(), desc);
                if (FAILED(hr)) return hr;
            }
            const UINT count = desc.MipLevels * (desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? 1 : desc.DepthOrArraySize);
            window.footprints.resize(count); window.rows.resize(count);
            device->GetCopyableFootprints(&desc, 0, count, 0, window.footprints.data(), window.rows.data(), nullptr, &window.staging_bytes);
            for (UINT i = 0; i < count; ++i)
                window.staging_bytes = (std::max)(window.staging_bytes,
                    window.footprints[i].Offset + UINT64(window.footprints[i].Footprint.RowPitch) * window.rows[i] * window.footprints[i].Footprint.Depth);
            if (!window.staging_bytes || window.staging_bytes > (1ull << 30)) return DXGI_ERROR_UNSUPPORTED;
            D3D12_RESOURCE_DESC staging{};
            staging.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; staging.Width = window.staging_bytes;
            staging.Height = 1; staging.DepthOrArraySize = 1; staging.MipLevels = 1;
            staging.SampleDesc.Count = 1; staging.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            D3D12_HEAP_PROPERTIES heap{};
            heap.Type = D3D12_HEAP_TYPE_UPLOAD;
            Microsoft::WRL::ComPtr<ID3D12Resource> upload, readback;
            hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &staging,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload));
            heap.Type = D3D12_HEAP_TYPE_READBACK;
            if (SUCCEEDED(hr)) hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &staging,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback));
            if (FAILED(hr)) return hr;
            window.upload = std::move(upload); window.readback = std::move(readback);
        }
        window.gpu_layout_active=false;
        static const bool cpu_layout=[] {wchar_t value[2]{};return GetEnvironmentVariableW(L"XONE_ERA_CPU_LAYOUT",value,2)==1 && value[0]==L'1';}();
        if(gpu_allowed && !cpu_layout && !input && !window.sample_array && window.footprints.size()==1 &&
            window.texture.offsets && !window.gpu_layout_refused) {
            if(!window.layout_transfer) {
                const UINT32* offsets=nullptr;SIZE_T count=0;
                const auto ready=window.texture.offsets(window.texture.context.get(),&offsets,&count);
                if(FAILED(ready))return ready;
                if(ready==S_OK) {
                    Microsoft::WRL::ComPtr<ID3D12Device> device;HRESULT hr=window.resource->GetDevice(IID_PPV_ARGS(&device));
                    if(FAILED(hr))return hr;
                    auto transfer=std::make_unique<TextureLayoutTransfer>();const auto& desc=window.texture.resource_desc;
                    hr=transfer->initialize(device.Get(),{offsets,count},UINT(desc.Width),desc.Height,window.footprints[0],window.texture.bytes);
                    if(FAILED(hr))return hr;
                    if(hr==S_OK)window.layout_transfer=std::move(transfer);else window.gpu_layout_refused=true;
                    wchar_t timing[2]{};
                    if(GetEnvironmentVariableW(L"XONE_ERA_TIMING",timing,2)==1 && timing[0]==L'1')
                        std::fprintf(stderr,"xone-era: timing layout=%s width=%llu height=%u format=%u bytes=%zu\n",
                            hr==S_OK ? "gpu" : "cpu-budget",desc.Width,desc.Height,desc.Format,window.texture.bytes);
                }
            }
            if(window.layout_transfer) {
                const HRESULT hr=window.layout_transfer->prepare(canonical,discard_contents);if(FAILED(hr))return hr;
                window.gpu_layout_active=true;return S_OK;
            }
        }
        if(discard_contents) return S_OK; // Readback/sample staging is still required after a full clear.
        UINT64 input_row_bytes=0;
        if(input) {
            if(window.sample_array || !input->pData || input->RowPitch<=0 || input->SlicePitch<=0 ||
                window.rows.empty() || !window.rows[0] || !window.footprints[0].Footprint.Depth) return E_INVALIDARG;
            Microsoft::WRL::ComPtr<ID3D12Device> device;
            HRESULT hr=window.resource->GetDevice(IID_PPV_ARGS(&device));if(FAILED(hr)) return hr;
            const auto desc=window.resource->GetDesc();
            device->GetCopyableFootprints(&desc,0,1,0,nullptr,nullptr,&input_row_bytes,nullptr);
            if(UINT64(input->RowPitch)<input_row_bytes || UINT64(input->RowPitch)>UINT64_MAX/window.rows[0] ||
                UINT64(input->SlicePitch)<UINT64(input->RowPitch)*window.rows[0] ||
                UINT64(input->SlicePitch)>SIZE_MAX/window.footprints[0].Footprint.Depth) return E_INVALIDARG;
        }
        void* mapped = nullptr;
        const D3D12_RANGE no_read{0, 0};
        HRESULT hr = window.upload->Map(0, &no_read, &mapped);
        if (FAILED(hr)) return hr;
        if (window.sample_array) {
            const auto& desc = window.texture.resource_desc;
            const UINT element=sample_pixel_bytes(desc.Format);
            const UINT row = UINT(desc.Width) * desc.SampleDesc.Count * element;
            hr = window.texture.copy(window.texture.context.get(), false, window.sample_linear.data(), canonical, 0, row, row * desc.Height,&clear);
            if (SUCCEEDED(hr) && (desc.Format==DXGI_FORMAT_R32G8X24_TYPELESS || desc.Format==DXGI_FORMAT_R32_TYPELESS)) for (SIZE_T at=0;at<window.sample_linear.size();at+=element) {
                UINT32 bits; std::memcpy(&bits,window.sample_linear.data()+at,4);
                // SV_Depth cannot promise lossless NaN, signed-zero or denormal payloads.
                if (bits>0x3f800000u || (bits && !(bits&0x7f800000u))) { hr=DXGI_ERROR_UNSUPPORTED; break; }
            }
            if (SUCCEEDED(hr)) interleave_samples(window, mapped, true);
        } else for (UINT i = 0; i < window.footprints.size(); ++i) {
            const auto& footprint = window.footprints[i];
            const UINT slice = footprint.Footprint.RowPitch * window.rows[i];
            if(i==0 && input) {
                for(UINT z=0;z<footprint.Footprint.Depth;++z) for(UINT y=0;y<window.rows[i];++y)
                    std::memcpy(static_cast<unsigned char*>(mapped)+footprint.Offset+SIZE_T(z)*slice+SIZE_T(y)*footprint.Footprint.RowPitch,
                        static_cast<const unsigned char*>(input->pData)+SIZE_T(z)*input->SlicePitch+SIZE_T(y)*input->RowPitch,SIZE_T(input_row_bytes));
            } else hr = window.texture.copy(window.texture.context.get(), false, static_cast<unsigned char*>(mapped) + footprint.Offset,
                canonical, i, footprint.Footprint.RowPitch, slice,&clear);
            if (FAILED(hr)) break;
        }
        const D3D12_RANGE written{0, static_cast<SIZE_T>(window.staging_bytes)};
        window.upload->Unmap(0, &written);
        return hr;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}

HRESULT begin_title_gpu_texture(TitleGpuResource& texture, ID3D12GraphicsCommandList* list, D3D12_RESOURCE_STATES state,bool discard_contents,
                                const D3D12_SUBRESOURCE_DATA* input,bool read_only) {
    if(discard_contents && input) return E_INVALIDARG;
    if(read_only && (discard_contents || input || !gpu_read_only(state)))return E_INVALIDARG;
    std::unique_lock lock(memory_mutex);
    GpuBacking* backing = nullptr; GpuWindow* window = nullptr;
    HRESULT hr = claim_gpu_window(texture, list, state, true, backing, window);
    if (FAILED(hr)) {std::fprintf(stderr,"xone-era: texture ingress claim failed 0x%08lX\n",hr);return hr;}
    window->read_only=read_only;
    if(discard_contents) {
        const auto& desc=window->texture.resource_desc;
        if(desc.Dimension!=D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.MipLevels!=1 || desc.DepthOrArraySize!=1 ||
            (state!=D3D12_RESOURCE_STATE_RENDER_TARGET && state!=D3D12_RESOURCE_STATE_DEPTH_WRITE)) {
            release_physical(*window,&texture,false);window->active=nullptr;window->list.Reset();return E_INVALIDARG;
        }
    }
    window->preparing = true;
    auto* canonical = static_cast<unsigned char*>(texture.allocation) + texture.canonical_offset;
    auto clear=texture.clear;
    clear.bypass_htile=(window->texture.resource_desc.Flags&D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) &&
        read_only && !(state&D3D12_RESOURCE_STATE_DEPTH_READ);
    hr=prepare_gpu_tracking(texture,*window,clear,discard_contents && !input);
    if(FAILED(hr)){release_physical(*window,&texture,false);window->active=nullptr;window->list.Reset();window->preparing=false;memory_changed.notify_all();return hr;}
    if(window->tracked && input) {
        hr=flush_gpu_window(*window);
        if(FAILED(hr)){release_physical(*window,&texture,false);window->active=nullptr;window->list.Reset();window->preparing=false;memory_changed.notify_all();return hr;}
    }
    if(window->tracked && window->reuse && !discard_contents && !input) {
        window->preparing=false;
        gpu_transition(list,texture.resource,D3D12_RESOURCE_STATE_COMMON,state);
        return S_OK;
    }
    std::vector<unsigned char> incoming;
    if(window->tracked) try {
        incoming.resize(texture.size);mirror_copy(*window,incoming.data(),false);canonical=incoming.data();
    } catch(const std::bad_alloc&) {
        release_physical(*window,&texture,false);window->active=nullptr;window->list.Reset();window->preparing=false;memory_changed.notify_all();return E_OUTOFMEMORY;
    }
    // The ownership gate keeps this range alive and exclusive while CPU layout
    // conversion runs; original helpers may call allocation hooks themselves.
    lock.unlock();
    hr = prepare_texture_upload(*window, canonical,clear,discard_contents,input,list->GetType()==D3D12_COMMAND_LIST_TYPE_DIRECT);
    lock.lock();
    window->preparing = false;
    if (FAILED(hr)) { std::fprintf(stderr,"xone-era: texture ingress layout failed 0x%08lX\n",hr);release_physical(*window, &texture, false); window->active = nullptr; window->list.Reset(); return hr; }
    if(discard_contents) {
        // A full clear defines all samples/aspects; do not decode an undefined preimage (e.g. HTILE).
        gpu_transition(list,texture.resource,D3D12_RESOURCE_STATE_COMMON,state);
        return S_OK;
    }
    if(window->gpu_layout_active) {
        gpu_transition(list,texture.resource,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
        window->layout_transfer->detile(list,texture.resource);
        gpu_transition(list,texture.resource,D3D12_RESOURCE_STATE_COPY_DEST,state);return S_OK;
    }
    auto* copy_target = window->sample_array ? window->sample_array.Get() : texture.resource;
    gpu_transition(list, copy_target, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    for (UINT i = 0; i < window->footprints.size(); ++i) {
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = window->upload.Get(); source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint = window->footprints[i];
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = copy_target; destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        destination.SubresourceIndex = i;
        list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    }
    if (window->sample_array) {
        gpu_transition(list, copy_target, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        const auto write_state=window->sample_dsv ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET;
        gpu_transition(list, texture.resource, D3D12_RESOURCE_STATE_COMMON, write_state);
        auto target = window->sample_rtv->GetCPUDescriptorHandleForHeapStart();
        const auto& desc = window->texture.resource_desc;
        target.ptr += SIZE_T(desc.SampleDesc.Count) * window->sample_rtv_step;
        if (window->sample_dsv) target=window->sample_dsv->GetCPUDescriptorHandleForHeapStart();
        hr = window->sample_transfer->record(list, window->sample_srv.Get(), window->sample_srv->GetGPUDescriptorHandleForHeapStart(),
            target, UINT(desc.Width), desc.Height, 0, true);
        if (FAILED(hr)) return hr;
        gpu_transition(list, copy_target, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        gpu_transition(list, texture.resource, write_state, state);
    } else gpu_transition(list, texture.resource, D3D12_RESOURCE_STATE_COPY_DEST, state);
    return S_OK;
}

HRESULT end_title_gpu_texture(TitleGpuResource& texture, ID3D12GraphicsCommandList* list) {
    std::lock_guard lock(memory_mutex);
    auto* window = gpu_window(texture, gpu_backing(texture));
    if (!window || !window->texture.context || window->active != &texture || window->list.Get() != list || window->copied_back)
        return E_INVALIDARG;
    if (window->preparing) return E_PENDING;
    if (window->read_only) {
        gpu_transition(list,texture.resource,window->state,D3D12_RESOURCE_STATE_COMMON);
        window->copied_back=true;return S_OK;
    }
    if(window->tracked) {
        gpu_transition(list,texture.resource,window->state,D3D12_RESOURCE_STATE_COMMON);
        window->copied_back=true;return S_OK;
    }
    const HRESULT hr=record_texture_readback(window,list,window->state);
    if(SUCCEEDED(hr))window->copied_back=true;
    return hr;
}

HRESULT complete_title_gpu_resource(TitleGpuResource& buffer, ID3D12Fence* fence, UINT64 value) {
    std::unique_lock lock(memory_mutex);
    auto* backing = gpu_backing(buffer);
    auto* window = gpu_window(buffer, backing);
    if (!window || window->active != &buffer) return E_INVALIDARG;
    if (!window->copied_back || window->completing) return E_PENDING;
    const HRESULT hr = completed_fence(backing, fence, value);
    if (FAILED(hr)) return hr;
    if(window->tracked) {
        window->tracked_resource=buffer;
        if(!window->read_only) {
            window->pending_cpu=true;
            for(const auto& key:window->watch_keys)for(auto* alias:watched_physical.at(key).windows)alias->resident=false;
        }
        window->resident_clear=window->active_clear;window->resident=true;
        release_physical(*window,&buffer,false);window->active=nullptr;window->list.Reset();window->copied_back=false;window->active_thread=0;
        for(const auto& key:window->watch_keys){const auto sealed=seal_watched(watched_physical.at(key));if(FAILED(sealed))return sealed;}
        memory_changed.notify_all();return S_OK;
    }
    if(window->gpu_layout_active && !window->read_only) {
        // The GPU snapshot already contains every texel and padding byte.
        // Map may fail before publication; no fallible conversion follows it.
        window->completing=true;auto* readback=window->layout_transfer->readback();
        const D3D12_RANGE read{0,buffer.size};void* mapped=nullptr;
        lock.unlock();const HRESULT mapped_result=readback->Map(0,&read,&mapped);lock.lock();
        if(FAILED(mapped_result)){window->completing=false;return mapped_result;}
        std::memcpy(static_cast<unsigned char*>(buffer.allocation)+buffer.canonical_offset,mapped,buffer.size);
        const D3D12_RANGE no_write{0,0};readback->Unmap(0,&no_write);window->completing=false;
    // Read-only use retains ownership/fence retirement without redundant writeback.
    } else if ((window->texture.context || window->staged_buffer) && !window->read_only) {
        window->completing = true;
        auto* canonical = static_cast<unsigned char*>(buffer.allocation) + buffer.canonical_offset;
        lock.unlock();
        HRESULT converted = S_OK;
        std::vector<unsigned char> commit;
        try {
            commit.assign(canonical, canonical + buffer.size);
            void* mapped = nullptr;
            const D3D12_RANGE read{0,static_cast<SIZE_T>(window->staging_bytes)};
            auto* readback=window->readback.Get();
            converted = readback->Map(0, &read, &mapped);
            if (SUCCEEDED(converted)) {
                if (window->staged_buffer) std::memcpy(commit.data(), mapped, buffer.size);
                else if (window->sample_array) {
                    interleave_samples(*window, mapped, false);
                    const auto& desc = window->texture.resource_desc;
                    const UINT row = UINT(desc.Width) * desc.SampleDesc.Count * sample_pixel_bytes(desc.Format);
                    converted = window->texture.copy(window->texture.context.get(), true, window->sample_linear.data(), commit.data(),
                        0, row, row * desc.Height,nullptr);
                } else for (UINT i = 0; i < window->footprints.size(); ++i) {
                    const auto& footprint = window->footprints[i];
                    converted = window->texture.copy(window->texture.context.get(), true,
                        static_cast<unsigned char*>(mapped) + footprint.Offset, commit.data(), i,
                        footprint.Footprint.RowPitch, footprint.Footprint.RowPitch * window->rows[i],nullptr);
                    if (FAILED(converted)) break;
                }
                const D3D12_RANGE no_write{0, 0};
                readback->Unmap(0, &no_write);
            }
        } catch (const std::bad_alloc&) { converted = E_OUTOFMEMORY; }
        lock.lock();
        window->completing = false;
        if (FAILED(converted)) return converted; // Preserve the gate and canonical preimage for a retry.
        std::memcpy(canonical, commit.data(), commit.size());
    }
    release_physical(*window, &buffer, false);
    window->active = nullptr; window->list.Reset(); window->copied_back = false;
    window->active_thread=0;memory_changed.notify_all();
    return S_OK;
}

HRESULT map_title_gpu_resource(TitleGpuResource& buffer, void** address,bool wait) {
    if (!address) return E_INVALIDARG;
    *address = nullptr;
    std::unique_lock lock(memory_mutex);
    const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(5);
    for(;;) {
        const auto attempt=[&]() -> HRESULT {
            auto* backing = gpu_backing(buffer);
            auto* window = gpu_window(buffer, backing);
            if (!window || backing->retiring) return E_INVALIDARG;
            const auto synchronized=cpu_memory_access(static_cast<unsigned char*>(buffer.allocation)+buffer.canonical_offset,buffer.size,true);
            if(FAILED(synchronized))return synchronized;
            for (const auto& [range, alias] : backing->windows) {
                if (alias.active) return E_PENDING;
                if (alias.cpu && buffer.canonical_offset < range.first + range.second &&
                    range.first < buffer.canonical_offset + buffer.size) return HRESULT_FROM_WIN32(ERROR_BUSY);
            }
            HRESULT hr = physical_pages(buffer, window->physical_pages, false);
            if (FAILED(hr)) return hr;
            for (UINT page = 0; page < window->physical_pages.size(); ++page) if (window->physical_pages[page]) {
                if (esram.pages[page].gpu) return E_PENDING;
                if (esram.pages[page].cpu) return HRESULT_FROM_WIN32(ERROR_BUSY);
            }
            for (UINT page = 0; page < window->physical_pages.size(); ++page)
                if (window->physical_pages[page]) esram.pages[page].cpu = &buffer;
            window->cpu = &buffer;
            *address = static_cast<unsigned char*>(buffer.allocation) + buffer.canonical_offset;
            return S_OK;
        };
        const HRESULT hr=attempt();
        if(hr!=E_PENDING || !wait)return hr;
        // Waiting on this thread's own unsubmitted interval would deadlock.
        auto* backing=gpu_backing(buffer);
        for(const auto& [range,alias]:backing->windows)
            if(alias.active && alias.active_thread==GetCurrentThreadId())return E_PENDING;
        if(memory_changed.wait_until(lock,deadline)==std::cv_status::timeout)return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    }
}

HRESULT unmap_title_gpu_resource(TitleGpuResource& buffer) {
    std::lock_guard lock(memory_mutex);
    auto* window = gpu_window(buffer, gpu_backing(buffer));
    if (!window || window->cpu != &buffer) return E_INVALIDARG;
    release_physical(*window, &buffer, true);
    window->cpu = nullptr;
    if(window->tracked)for(const auto& key:window->watch_keys){const auto hr=seal_watched(watched_physical.at(key));if(FAILED(hr))return hr;}
    memory_changed.notify_all();
    return S_OK;
}
HRESULT set_title_gpu_clear(TitleGpuResource& resource,UINT aspects,UINT32 depth,UINT8 stencil) {
    FLOAT value=0;std::memcpy(&value,&depth,sizeof value);
    if(!aspects || (aspects&~3u) || ((aspects&1) && !(value>=0.f && value<=1.f))) return E_INVALIDARG;
    std::lock_guard lock(memory_mutex);
    auto* backing=gpu_backing(resource);auto* window=gpu_window(resource,backing);
    if(!window || !window->texture.context || backing->retiring ||
        !(window->resource->GetDesc().Flags&D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)) return E_INVALIDARG;
    if(window->active) return E_PENDING;
    if(window->cpu) return HRESULT_FROM_WIN32(ERROR_BUSY);
    if(aspects&1) resource.clear.depth=depth;
    if(aspects&2) resource.clear.stencil=stencil;
    resource.clear.known|=UINT8(aspects);
    return S_OK;
}

HRESULT release_title_gpu_resource(TitleGpuResource& buffer, ID3D12Fence* fence, UINT64 value) {
    if (!buffer.resource || !fence) return E_INVALIDARG;
    GpuWindows::node_type retired_window; // Destroy original layout helpers after releasing the registry lock.
    std::lock_guard lock(memory_mutex);
    const auto base = reinterpret_cast<std::uintptr_t>(buffer.allocation);
    auto found = title_regions.upper_bound(base);
    if (found == title_regions.begin()) return E_INVALIDARG;
    --found;
    if (buffer.composite && (found->first != base || !found->second.graphics || found->second.xmem)) return E_INVALIDARG;
    auto* backing = static_cast<GpuBacking*>(&found->second);
    if (!found->second.xmem && !buffer.composite) {
        const auto commit = found->second.commits.find(base);
        if (commit == found->second.commits.end()) return E_INVALIDARG;
        backing = &commit->second;
    }
    if (!backing->gpu_pins) return E_INVALIDARG;
    auto* window = gpu_window(buffer, backing);
    if (buffer.representation ? !window : backing->buffer.Get() != buffer.resource) return E_INVALIDARG;
    if (window && window->cpu) return E_PENDING;
    if (window && (window->texture.context || window->staged_buffer) && window->active) return E_PENDING;
    if (window && window->active && (window->active != &buffer || !window->copied_back)) return E_PENDING;
    const HRESULT hr = completed_fence(backing, fence, value);
    if (FAILED(hr)) return hr;
    if (window) {
        const auto synchronized=flush_gpu_window(*window);if(FAILED(synchronized))return synchronized;
        window->active = nullptr; window->list.Reset(); window->copied_back = false;
        if (!--window->owners) {
            detach_gpu_tracking(*window);
            const auto range = backing->windows.equal_range({buffer.canonical_offset, buffer.size});
            for (auto at = range.first; at != range.second; ++at)
                if (&at->second == window) { retired_window = backing->windows.extract(at); break; }
        }
    }
    buffer.resource->Release();
    buffer = {};
    --backing->gpu_pins;
    if (backing->retiring && !backing->gpu_pins && !destroy_region(found))
        return HRESULT_FROM_WIN32(GetLastError());
    return S_OK;
}

bool same_library(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
        return (x >= 'A' && x <= 'Z' ? x + 32 : x) == (y >= 'A' && y <= 'Z' ? y + 32 : y);
    });
}

HRESULT pin_title_audio_memory(void* address, std::size_t bytes) {
    std::lock_guard lock(memory_mutex);
    const auto start = reinterpret_cast<std::uintptr_t>(address);
    auto found = title_regions.upper_bound(start);
    if (found == title_regions.begin()) return E_INVALIDARG;
    --found;
    auto& region = found->second;
    const auto offset = start - found->first;
    if (!bytes || !region.xmem || region.graphics || region.retiring ||
        offset > region.size || bytes > region.size - offset) return E_INVALIDARG;
    if (region.gpu_pins || !region.windows.empty()) return HRESULT_FROM_WIN32(ERROR_BUSY);
    ++region.gpu_pins; // Shared backing lifetime pin; no GPU resource is created.
    return S_OK;
}
void unpin_title_audio_memory(void* address) {
    std::lock_guard lock(memory_mutex);
    auto found = title_regions.upper_bound(reinterpret_cast<std::uintptr_t>(address));
    if (found == title_regions.begin()) return;
    --found;
    if (reinterpret_cast<std::uintptr_t>(address) - found->first >= found->second.size || !found->second.gpu_pins) return;
    if (!--found->second.gpu_pins && found->second.retiring) (void)destroy_region(found);
}

void set_title_module(void* base) { title_module.store(static_cast<HMODULE>(base)); }

void set_title_storage_root(const std::wstring& root) {
    std::wstring full = root.empty() ? std::wstring{} : full_path(root.c_str());
    if (!full.empty() && full.back() != L'\\') full += L'\\';
    {
        std::lock_guard lock(storage_mutex);
        storage_root = std::move(full);
    }
    reset_title_users();
}
std::wstring title_storage_root() { std::lock_guard lock(storage_mutex); return storage_root; }
bool title_writable_path(const std::wstring& path) { return writable_path(path.c_str()); }

void* resolve_kernelx(const ImportRequest& request) {
    if (!same_library(request.library, "kernelx.dll") || request.name.empty()) return nullptr;
    if (request.name == "GetFileAttributesExA") return reinterpret_cast<void*>(&era_GetFileAttributesExA);
    if (request.name == "GetFileAttributesExW") return reinterpret_cast<void*>(&era_GetFileAttributesExW);
    if (request.name == "GetDiskFreeSpaceExW") return reinterpret_cast<void*>(&era_GetDiskFreeSpaceExW);
    if (request.name == "FindFirstFileExA") return reinterpret_cast<void*>(&era_FindFirstFileExA);
    if (request.name == "GetModuleHandleW") return reinterpret_cast<void*>(&era_GetModuleHandleW);
    if (request.name == "VirtualAlloc") return reinterpret_cast<void*>(&era_VirtualAlloc);
    if (request.name == "TitleMemoryStatus") return reinterpret_cast<void*>(&era_TitleMemoryStatus);
    if (request.name == "VirtualFree") return reinterpret_cast<void*>(&era_VirtualFree);
    if (request.name == "XMemSetAllocationHooks") return reinterpret_cast<void*>(&era_XMemSetAllocationHooks);
    if (request.name == "XMemAlloc") return reinterpret_cast<void*>(&era_XMemAlloc);
    if (request.name == "XMemFree") return reinterpret_cast<void*>(&era_XMemFree);
    if (request.name == "XMemAllocDefault") return reinterpret_cast<void*>(&era_XMemAllocDefault);
    if (request.name == "XMemFreeDefault") return reinterpret_cast<void*>(&era_XMemFreeDefault);
    if (request.name == "DeleteFileW") return reinterpret_cast<void*>(&era_DeleteFileW);
    if (request.name == "DeleteFileA") return reinterpret_cast<void*>(&era_DeleteFileA);
    if (request.name == "CreateFileW") return reinterpret_cast<void*>(&era_CreateFileW);
    if (request.name == "GetProcAddress") return reinterpret_cast<void*>(&era_GetProcAddress);
    if (request.name == "CreateFileA") return reinterpret_cast<void*>(&era_CreateFileA);
    if (request.name == "CreateFile2") return reinterpret_cast<void*>(&era_CreateFile2);
    if (request.name == "WriteFile") return reinterpret_cast<void*>(&era_WriteFile);
    if (request.name == "ReadFile") return reinterpret_cast<void*>(&era_ReadFile);
    if (request.name == "FlushFileBuffers") return reinterpret_cast<void*>(&era_FlushFileBuffers);
    if (request.name == "SetEndOfFile") return reinterpret_cast<void*>(&era_SetEndOfFile);
    if (request.name == "CreateDirectoryW") return reinterpret_cast<void*>(&era_CreateDirectoryW);
    if (request.name == "CreateDirectoryA") return reinterpret_cast<void*>(&era_CreateDirectoryA);
    if (request.name == "RemoveDirectoryA") return reinterpret_cast<void*>(&era_RemoveDirectoryA);
    if (request.name == "MoveFileExW") return reinterpret_cast<void*>(&era_MoveFileExW);
    if (std::find(std::begin(forwarded), std::end(forwarded), request.name) == std::end(forwarded)) return nullptr;
    // ETW provider APIs live in advapi32; everything else in kernel32.
    for (const wchar_t* host : {L"kernel32.dll", L"advapi32.dll"}) {
        const HMODULE module = LoadLibraryExW(host, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (auto* address = module ? reinterpret_cast<void*>(GetProcAddress(module, request.name.c_str())) : nullptr)
            return address;
    }
    return nullptr;
}

} // namespace xboxone::era
