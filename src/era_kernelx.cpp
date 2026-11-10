// SPDX-License-Identifier: BSD-3-Clause
#include "xboxone_native/era_loader.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <iterator>
#include <mutex>
#include <string>
#include <string_view>

namespace xboxone::era {
namespace {

// Same-name Win32 exports with identical ABI and semantics for a title process.
// Memory (Virtual*), device, file-deletion and Xbox-only exports are absent on
// purpose: they need title-memory/storage contracts, not a host forward.
constexpr std::string_view forwarded[]{
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
std::wstring storage_root; // Full path ending in '\'; empty = no writable storage.

std::wstring full_path(LPCWSTR path) {
    DWORD n = GetFullPathNameW(path, 0, nullptr, nullptr);
    if (!n) return {};
    std::wstring out(n, L'\0');
    n = GetFullPathNameW(path, n, out.data(), nullptr);
    out.resize(n);
    return out;
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
    for (std::size_t at = full.find(L'\\', root.size()); at != std::wstring::npos; at = full.find(L'\\', at + 1)) {
        const DWORD attributes = GetFileAttributesW(full.substr(0, at).c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    }
    return true;
}

BOOL WINAPI era_DeleteFileW(LPCWSTR path) {
    if (!path || !*path) { SetLastError(ERROR_INVALID_PARAMETER); return FALSE; }
    const std::wstring full = full_path(path);
    if (full.empty()) return FALSE; // GetFullPathNameW set the error.
    if (!inside_storage(full)) { SetLastError(ERROR_ACCESS_DENIED); return FALSE; }
    return DeleteFileW(full.c_str());
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
    if (request.name == "DeleteFileW") return reinterpret_cast<void*>(&era_DeleteFileW);
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
