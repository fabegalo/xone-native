// SPDX-License-Identifier: BSD-3-Clause
#include "xboxone_native/era_loader.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <iterator>
#include <string_view>

namespace xboxone::era {
namespace {

// Same-name Win32 exports with identical ABI and semantics for a title process.
// Memory (Virtual*), device, file-deletion and Xbox-only exports are absent on
// purpose: they need title-memory/storage contracts, not a host forward.
constexpr std::string_view forwarded[]{
    "CloseHandle", "CreateEventW", "CreateMutexW", "CreateSemaphoreW", "CreateThread",
    "DeleteCriticalSection", "DuplicateHandle", "EnterCriticalSection", "ExitProcess", "ExitThread",
    "FlsAlloc", "FlsFree", "FlsGetValue", "FlsSetValue", "GetCurrentProcess", "GetCurrentProcessId",
    "GetCurrentThread", "GetCurrentThreadId", "GetExitCodeThread", "GetLastError", "GetProcessHeap",
    "GetSystemTimeAsFileTime", "GetTickCount", "GetTickCount64", "HeapAlloc", "HeapFree", "HeapReAlloc",
    "HeapSize", "InitializeCriticalSection", "InitializeCriticalSectionAndSpinCount",
    "InitializeSRWLock", "LeaveCriticalSection", "QueryPerformanceCounter", "QueryPerformanceFrequency",
    "ReleaseMutex", "ReleaseSemaphore", "ResetEvent", "ResumeThread", "SetEvent", "SetLastError",
    "SetThreadPriority", "Sleep", "SleepEx", "SwitchToThread", "TlsAlloc", "TlsFree", "TlsGetValue",
    "TlsSetValue", "WaitForMultipleObjects", "WaitForSingleObject", "WaitForSingleObjectEx",
    "TryEnterCriticalSection"};

bool same_library(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
        return (x >= 'A' && x <= 'Z' ? x + 32 : x) == (y >= 'A' && y <= 'Z' ? y + 32 : y);
    });
}

} // namespace

void* resolve_kernelx(const ImportRequest& request) {
    if (!same_library(request.library, "kernelx.dll") || request.name.empty()) return nullptr;
    if (std::find(std::begin(forwarded), std::end(forwarded), request.name) == std::end(forwarded)) return nullptr;
    static const HMODULE kernel32 = GetModuleHandleW(L"kernel32.dll");
    return kernel32 ? reinterpret_cast<void*>(GetProcAddress(kernel32, request.name.c_str())) : nullptr;
}

} // namespace xboxone::era
