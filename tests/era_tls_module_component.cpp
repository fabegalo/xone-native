// SPDX-License-Identifier: BSD-3-Clause
// Owned, CRT-free DLL: independent module TLS, callback/DllMain order and reuse.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

extern "C" {
unsigned long _tls_index = 0;
#pragma section(".tls", read, write)
#pragma section(".tls$ZZZ", read, write)
__declspec(allocate(".tls")) char tls_start = 0;
__declspec(thread) volatile DWORD value = 0x51a7b007;
__declspec(thread) volatile unsigned char zero_fill[0x800] = {};
__declspec(allocate(".tls$ZZZ")) char tls_end = 0;
}
static DWORD events[32] = {};
static DWORD count = 0;
static DWORD* receipt = nullptr;
static bool fail_attach = false;
static void record(DWORD event) {
    if (count < 32) events[count++] = event;
    if (receipt) *receipt = (*receipt << 8) | event;
}
static void NTAPI on_tls(void*, DWORD reason, void*) {
    if (reason == DLL_PROCESS_ATTACH || reason == DLL_THREAD_ATTACH) {
        if (value != 0x51a7b007 || zero_fill[0] || zero_fill[0x7ff]) record(0xee);
        value = 37;
    }
    record(0x10 | reason);
}
static const PIMAGE_TLS_CALLBACK callbacks[] = {on_tls, nullptr};
extern "C" const IMAGE_TLS_DIRECTORY64 _tls_used = {
    reinterpret_cast<ULONGLONG>(&tls_start), reinterpret_cast<ULONGLONG>(&tls_end),
    reinterpret_cast<ULONGLONG>(&_tls_index), reinterpret_cast<ULONGLONG>(callbacks), 0, 0};

extern "C" __declspec(dllexport) DWORD era_tls_module_value(DWORD next) {
    const DWORD old = value; value = next; zero_fill[0x7ff] = 19; return old;
}
extern "C" __declspec(dllexport) DWORD era_tls_module_event(DWORD at) {
    return at < count ? events[at] : 0;
}
extern "C" __declspec(dllexport) void era_tls_module_receipt(DWORD* out, BOOL fail) {
    receipt = out; fail_attach = fail != FALSE;
}
extern "C" BOOL WINAPI era_tls_module_main(HINSTANCE, DWORD reason, LPVOID) {
    record(0x20 | reason);
    return reason != DLL_PROCESS_ATTACH || !fail_attach;
}
