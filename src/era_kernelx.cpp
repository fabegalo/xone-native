// SPDX-License-Identifier: BSD-3-Clause
#include "xboxone_native/era_loader.h"
#include "era_memory.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wrl/client.h>
#include <wrl/wrappers/corewrappers.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
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
    "ReadFile", "SetFilePointer", "SetFilePointerEx", "WaitForMultipleObjectsEx", "WideCharToMultiByte",
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
std::mutex memory_mutex;
struct GpuBacking {
    SIZE_T size;
    DWORD guest_protection = 0;
    bool retiring = false;
    unsigned gpu_pins = 0;
    Microsoft::WRL::ComPtr<ID3D12Heap> heap;
    Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
    Microsoft::WRL::ComPtr<IUnknown> device;
    Microsoft::WRL::Wrappers::FileHandle mapping{};
    D3D12_RESOURCE_STATES initial_state = D3D12_RESOURCE_STATE_COMMON;
};
struct TitleRegion : GpuBacking {
    bool graphics;
    bool xmem = false;
    ULONGLONG attributes = 0;
    std::map<std::uintptr_t, GpuBacking> commits;
    TitleRegion(SIZE_T bytes, bool gpu) : GpuBacking{bytes}, graphics(gpu) {}
};
std::map<std::uintptr_t, TitleRegion> title_regions;
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
    title_regions.erase(region);
    return true;
}

void* WINAPI era_XMemAllocDefault(SIZE_T size, ULONGLONG attributes) {
    // XALLOC_ATTRIBUTES ABI: object:14, page:2, allocator:8, alignment:5, type:4.
    const auto page = (attributes >> 14) & 3;
    const auto alignment = (attributes >> 24) & 31;
    const auto type = (attributes >> 29) & 15;
    if (!size || (attributes >> 33) || page > 1 || type > 6) {
        SetLastError(ERROR_NOT_SUPPORTED); return nullptr;
    }
    constexpr SIZE_T granularity = 65536;
    if (size > SIZE_MAX - (granularity - 1)) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return nullptr; }
    const SIZE_T bytes = (size + granularity - 1) & ~(granularity - 1);
    MEM_ADDRESS_REQUIREMENTS address{};
    address.Alignment = SIZE_T{1} << (alignment < 16 ? 16 : alignment);
    if (type) {
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
    void* result = allocate(GetCurrentProcess(), nullptr, bytes, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE, &parameter, 1);
    if (!result) return nullptr;
    std::lock_guard lock(memory_mutex);
    TitleRegion region{bytes, type != 0};
    region.xmem = true;
    region.attributes = attributes;
    try {
        title_regions.emplace(reinterpret_cast<std::uintptr_t>(result), std::move(region));
    } catch (const std::bad_alloc&) {
        VirtualFree(result, 0, MEM_RELEASE);
        SetLastError(ERROR_NOT_ENOUGH_MEMORY); return nullptr;
    }
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
    backing.buffer.Reset(); backing.heap.Reset(); backing.device.Reset();
    const auto unmap = host_unmap2();
    if (!unmap) { SetLastError(ERROR_NOT_SUPPORTED); return false; }
    if (!unmap(GetCurrentProcess(), reinterpret_cast<void*>(address), MEM_PRESERVE_PLACEHOLDER)) return false;
    backing.mapping.Close();
    return true;
}

// MEM_TITLE selects title ownership; keep the requested address/protection.
// GPU commitments retain the reservation and bind each committed span to the
// same shared CPU/GPU registry. Physical aliases and ESRAM remain unsupported.
LPVOID WINAPI era_VirtualAlloc(LPVOID address, SIZE_T size, DWORD type, DWORD protection) {
    constexpr DWORD mem_title = 0x40000000;
    constexpr DWORD mem_graphics = 0x10000000;
    constexpr DWORD supported = MEM_COMMIT | MEM_RESERVE | MEM_TOP_DOWN | MEM_RESET | MEM_RESET_UNDO | MEM_WRITE_WATCH;
    const bool graphics = (type & mem_graphics) != 0;
    // This supported subset reserves at 2 MiB boundaries, without backing;
    // individual 64 KiB-aligned commitments replace owned placeholders.
    const bool graphics_reserve = graphics && address && (type & MEM_LARGE_PAGES) &&
        (type & ~(mem_graphics | MEM_LARGE_PAGES)) == MEM_RESERVE &&
        !(reinterpret_cast<std::uintptr_t>(address) & ((1u << 21) - 1)) && !(size & ((1u << 21) - 1));
    const bool graphics_commit = address && type == (mem_graphics | MEM_LARGE_PAGES | MEM_COMMIT) &&
        !(reinterpret_cast<std::uintptr_t>(address) & 65535) && !(size & 65535) &&
        (protection == PAGE_READWRITE || protection == (PAGE_READWRITE | 0x10000));
    if (!size || (!graphics_reserve && !graphics_commit && (type & ~(supported | mem_title)))) {
        memory_error(address, size, type, protection, ERROR_NOT_SUPPORTED); return nullptr;
    }
    std::lock_guard lock(memory_mutex);
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
                if (next->second.gpu_pins) { SetLastError(ERROR_BUSY); return nullptr; }
                if (next->second.guest_protection != protection) { SetLastError(ERROR_NOT_SUPPORTED); return nullptr; }
                return address; // The same span is already committed; preserve its bytes.
            }
            if ((next != commits.end() && size > next->first - at) ||
                (next != commits.begin() && std::prev(next)->second.size > at - std::prev(next)->first)) {
                SetLastError(ERROR_INVALID_ADDRESS); return nullptr;
            }
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
                return result;
            } catch (const std::bad_alloc&) { SetLastError(ERROR_NOT_ENOUGH_MEMORY); return nullptr; }
        }
        if (region->second.graphics || graphics_commit) {
            memory_error(address, size, type, protection, ERROR_NOT_SUPPORTED); return nullptr;
        }
    }
    void* result = nullptr;
    if (graphics_reserve) {
        const auto allocate = host_allocate2();
        if (!allocate || !host_map3() || !host_unmap2()) { SetLastError(ERROR_NOT_SUPPORTED); return nullptr; }
        result = allocate(GetCurrentProcess(), address, size, MEM_RESERVE | MEM_RESERVE_PLACEHOLDER,
                          PAGE_NOACCESS, nullptr, 0);
    } else result = VirtualAlloc(address, size, type & ~mem_title, protection);
    if (!result) memory_error(address, size, type, protection, GetLastError());
    if (result && ((type & MEM_RESERVE) || !address)) {
        MEMORY_BASIC_INFORMATION info{};
        if (!VirtualQuery(result, &info, sizeof info)) {
            const DWORD error = GetLastError();
            VirtualFree(result, 0, MEM_RELEASE); SetLastError(error); return nullptr;
        }
        try {
            title_regions.emplace(reinterpret_cast<std::uintptr_t>(info.AllocationBase), TitleRegion{info.RegionSize, graphics});
        } catch (const std::bad_alloc&) {
            VirtualFree(result, 0, MEM_RELEASE);
            SetLastError(ERROR_NOT_ENOUGH_MEMORY); return nullptr;
        }
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
            commits.erase(commit);
            return TRUE;
        }
        for (const auto& [base, commit] : commits)
            if (commit.gpu_pins) { SetLastError(ERROR_BUSY); return FALSE; }
        while (!commits.empty()) {
            auto commit = commits.begin();
            if (!unmap_graphics_backing(commit->first, commit->second)) return FALSE;
            commits.erase(commit);
        }
    }
    if (type == MEM_RELEASE && region->second.graphics && !coalesce_placeholder(address, region->second.size)) return FALSE;
    const BOOL ok = VirtualFree(address, size, type);
    if (ok && type == MEM_RELEASE) title_regions.erase(region);
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
    if (path.starts_with(L"\\\\?\\")) path.erase(0, 4);
    return writable_path(path.c_str());
}

HANDLE WINAPI era_CreateFileW(LPCWSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
                              DWORD disposition, DWORD flags, HANDLE template_file) {
    constexpr DWORD mutation = GENERIC_WRITE | GENERIC_ALL | MAXIMUM_ALLOWED | DELETE | WRITE_DAC | WRITE_OWNER |
                               FILE_WRITE_DATA | FILE_APPEND_DATA | FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES;
    if (((access & mutation) || disposition != OPEN_EXISTING || (flags & FILE_FLAG_DELETE_ON_CLOSE)) &&
        !writable_path(path)) return INVALID_HANDLE_VALUE;
    return CreateFileW(path, access, share, security, disposition, flags, template_file);
}

HANDLE WINAPI era_CreateFileA(LPCSTR path, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES security,
                              DWORD disposition, DWORD flags, HANDLE template_file) {
    const auto wide = wide_path(path);
    if (wide.empty()) return INVALID_HANDLE_VALUE;
    return era_CreateFileW(wide.c_str(), access, share, security, disposition, flags, template_file);
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
    return WriteFile(file, buffer, size, written, operation);
}

BOOL WINAPI era_FlushFileBuffers(HANDLE file) {
    return writable_handle(file) ? FlushFileBuffers(file) : FALSE;
}

BOOL WINAPI era_SetEndOfFile(HANDLE file) {
    return writable_handle(file) ? SetEndOfFile(file) : FALSE;
}

BOOL WINAPI era_CreateDirectoryW(LPCWSTR path, LPSECURITY_ATTRIBUTES security) {
    return writable_path(path) ? CreateDirectoryW(path, security) : FALSE;
}

BOOL WINAPI era_CreateDirectoryA(LPCSTR path, LPSECURITY_ATTRIBUTES security) {
    const auto wide = wide_path(path);
    return !wide.empty() && era_CreateDirectoryW(wide.c_str(), security);
}

BOOL WINAPI era_RemoveDirectoryA(LPCSTR path) {
    const auto wide = wide_path(path);
    return !wide.empty() && writable_path(wide.c_str()) && RemoveDirectoryW(wide.c_str());
}

BOOL WINAPI era_MoveFileExW(LPCWSTR source, LPCWSTR destination, DWORD flags) {
    if (!writable_path(source) || !writable_path(destination)) return FALSE;
    // Deferred moves escape the session's storage lifetime.
    if (flags & MOVEFILE_DELAY_UNTIL_REBOOT) { SetLastError(ERROR_NOT_SUPPORTED); return FALSE; }
    return MoveFileExW(source, destination, flags);
}

BOOL WINAPI era_DeleteFileW(LPCWSTR path) {
    if (!path || !*path) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    const std::wstring full = full_path(path);
    if (full.empty()) return FALSE; // GetFullPathNameW set the error.
    if (!inside_storage(full)) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    return DeleteFileW(full.c_str());
}

BOOL WINAPI era_DeleteFileA(LPCSTR path) {
    const auto wide = wide_path(path);
    return !wide.empty() && era_DeleteFileW(wide.c_str());
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

HRESULT acquire_title_gpu_buffer(void* address, std::size_t size, ID3D12Device* device, TitleGpuBuffer& out, D3D12_RESOURCE_FLAGS flags) {
    if (!address || !size || !device || out.resource) return E_INVALIDARG;
    if (flags & ~D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) return DXGI_ERROR_UNSUPPORTED;
    std::lock_guard lock(memory_mutex);
    const auto at = reinterpret_cast<std::uintptr_t>(address);
    auto found = title_regions.upper_bound(at);
    if (found == title_regions.begin()) return E_INVALIDARG;
    --found;
    auto base = found->first;
    auto* backing = static_cast<GpuBacking*>(&found->second);
    if (!found->second.xmem) {
        auto commit = found->second.commits.upper_bound(at);
        if (!found->second.graphics || commit == found->second.commits.begin()) return E_INVALIDARG;
        --commit;
        base = commit->first;
        backing = &commit->second;
    }
    auto& region = *backing;
    const auto offset = at - base;
    if (region.retiring || offset >= region.size || size > region.size - offset) return E_INVALIDARG;
    if (region.gpu_pins) return HRESULT_FROM_WIN32(ERROR_BUSY);
    Microsoft::WRL::ComPtr<IUnknown> identity;
    HRESULT hr = device->QueryInterface(IID_PPV_ARGS(&identity));
    if (FAILED(hr)) return hr;
    if (region.device && region.device.Get() != identity.Get()) return DXGI_ERROR_INVALID_CALL;
    if (region.buffer && (region.buffer->GetDesc().Flags & flags) != flags) return DXGI_ERROR_UNSUPPORTED;
    if (!region.buffer) {
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
        if (state == D3D12_RESOURCE_STATE_GENERIC_READ && flags) return DXGI_ERROR_UNSUPPORTED;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        desc.Width = region.size;
        desc.Height = 1; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
        desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        desc.Flags = flags;
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
    ++region.gpu_pins;
    out.resource = region.buffer.Get();
    out.resource->AddRef();
    out.allocation = reinterpret_cast<void*>(base);
    out.offset = offset;
    out.initial_state = region.initial_state;
    return S_OK;
}

HRESULT release_title_gpu_buffer(TitleGpuBuffer& buffer, ID3D12Fence* fence, UINT64 value) {
    if (!buffer.resource || !fence) return E_INVALIDARG;
    std::lock_guard lock(memory_mutex);
    const auto base = reinterpret_cast<std::uintptr_t>(buffer.allocation);
    auto found = title_regions.upper_bound(base);
    if (found == title_regions.begin()) return E_INVALIDARG;
    --found;
    auto* backing = static_cast<GpuBacking*>(&found->second);
    if (!found->second.xmem) {
        const auto commit = found->second.commits.find(base);
        if (commit == found->second.commits.end()) return E_INVALIDARG;
        backing = &commit->second;
    }
    if (!backing->gpu_pins || backing->buffer.Get() != buffer.resource) return E_INVALIDARG;
    Microsoft::WRL::ComPtr<IUnknown> device;
    HRESULT hr = fence->GetDevice(IID_PPV_ARGS(&device));
    if (FAILED(hr)) return hr;
    if (device.Get() != backing->device.Get()) return DXGI_ERROR_INVALID_CALL;
    const UINT64 completed = fence->GetCompletedValue();
    if (completed == UINT64_MAX) return DXGI_ERROR_DEVICE_REMOVED;
    if (completed < value) return E_PENDING;
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

void set_title_module(void* base) { title_module.store(static_cast<HMODULE>(base)); }

void set_title_storage_root(const std::wstring& root) {
    std::wstring full = root.empty() ? std::wstring{} : full_path(root.c_str());
    if (!full.empty() && full.back() != L'\\') full += L'\\';
    std::lock_guard lock(storage_mutex);
    storage_root = std::move(full);
}

void* resolve_kernelx(const ImportRequest& request) {
    if (!same_library(request.library, "kernelx.dll") || request.name.empty()) return nullptr;
    if (request.name == "GetModuleHandleW") return reinterpret_cast<void*>(&era_GetModuleHandleW);
    if (request.name == "VirtualAlloc") return reinterpret_cast<void*>(&era_VirtualAlloc);
    if (request.name == "VirtualFree") return reinterpret_cast<void*>(&era_VirtualFree);
    if (request.name == "XMemSetAllocationHooks") return reinterpret_cast<void*>(&era_XMemSetAllocationHooks);
    if (request.name == "XMemAlloc") return reinterpret_cast<void*>(&era_XMemAlloc);
    if (request.name == "XMemFree") return reinterpret_cast<void*>(&era_XMemFree);
    if (request.name == "XMemAllocDefault") return reinterpret_cast<void*>(&era_XMemAllocDefault);
    if (request.name == "XMemFreeDefault") return reinterpret_cast<void*>(&era_XMemFreeDefault);
    if (request.name == "DeleteFileW") return reinterpret_cast<void*>(&era_DeleteFileW);
    if (request.name == "DeleteFileA") return reinterpret_cast<void*>(&era_DeleteFileA);
    if (request.name == "CreateFileW") return reinterpret_cast<void*>(&era_CreateFileW);
    if (request.name == "CreateFileA") return reinterpret_cast<void*>(&era_CreateFileA);
    if (request.name == "CreateFile2") return reinterpret_cast<void*>(&era_CreateFile2);
    if (request.name == "WriteFile") return reinterpret_cast<void*>(&era_WriteFile);
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
