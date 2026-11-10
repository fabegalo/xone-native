// SPDX-License-Identifier: BSD-3-Clause
// Owned synthetic title with static TLS, a TLS callback and worker threads.
// No CRT; all APIs come from kernelx.dll through the in-process ERA loader.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

extern "C" {
unsigned long _tls_index = 0;
#pragma section(".tls", read, write)
#pragma section(".tls$ZZZ", read, write)
__declspec(allocate(".tls")) char tls_start = 0;
__declspec(thread) volatile DWORD tls_value = 0x51a7b007;
__declspec(thread) volatile DWORD tls_zero = 0;
__declspec(allocate(".tls$ZZZ")) char tls_end = 0;
}
static volatile LONG process_attach = 0, thread_attach = 0, thread_detach = 0, callback_error = 0;

static void NTAPI on_tls(void*, DWORD reason, void*) {
    if (reason == DLL_PROCESS_ATTACH || reason == DLL_THREAD_ATTACH) {
        if (tls_value != 0x51a7b007 || tls_zero != 0) InterlockedExchange(&callback_error, 1);
        tls_value = 37;
        InterlockedIncrement(reason == DLL_PROCESS_ATTACH ? &process_attach : &thread_attach);
    } else if (reason == DLL_THREAD_DETACH) {
        if (tls_value != 99 || tls_zero != 19) InterlockedExchange(&callback_error, 2);
        InterlockedIncrement(&thread_detach);
    }
}

static const PIMAGE_TLS_CALLBACK callbacks[] = {on_tls, nullptr};
extern "C" const IMAGE_TLS_DIRECTORY64 _tls_used = {
    reinterpret_cast<ULONGLONG>(&tls_start), reinterpret_cast<ULONGLONG>(&tls_end),
    reinterpret_cast<ULONGLONG>(&_tls_index), reinterpret_cast<ULONGLONG>(callbacks), 0, 0};

static DWORD WINAPI worker(void*) {
    if (tls_value != 37 || tls_zero != 0) return 10;
    tls_value = 99;
    tls_zero = 19;
    return 0;
}

// Returns 0 only when template copy, callbacks and per-thread isolation all held.
extern "C" unsigned long long era_tls_entry() {
    if (process_attach != 1 || callback_error || tls_value != 37 || tls_zero != 0) return 20;
    tls_value = 123;
    tls_zero = 456;
    for (unsigned i = 0; i < 2; ++i) {
        HANDLE thread = CreateThread(nullptr, 0, worker, nullptr, 0, nullptr);
        if (!thread || WaitForSingleObject(thread, 2000) != WAIT_OBJECT_0) return 21;
        DWORD code = 1;
        if (!GetExitCodeThread(thread, &code) || code || !CloseHandle(thread)) return 22 + code * 100;
        if (tls_value != 123 || tls_zero != 456 || callback_error ||
            thread_attach != static_cast<LONG>(i + 1) || thread_detach != static_cast<LONG>(i + 1))
            return 30 + callback_error;
    }
    return 0;
}
