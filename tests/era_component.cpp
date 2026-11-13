// SPDX-License-Identifier: BSD-3-Clause
// Owned synthetic title image. No CRT; every API comes from kernelx.dll, which
// only the in-process ERA loader can satisfy (no kernelx.dll exists on disk).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static int value = 42;
static int* volatile pointer = &value; // Absolute address: requires a DIR64 relocation.

#ifdef ERA_MODULE_FIXTURE
static ULONG attaches = 0;
static ULONG* detach_receipt = nullptr;
extern "C" __declspec(dllexport) unsigned long long era_test_module_export() {
    if (*pointer != 42) return 0; // Exercise a real DIR64 relocation in the helper too.
    return (static_cast<unsigned long long>(attaches) << 32) | GetCurrentProcessId();
}
extern "C" __declspec(dllexport) void era_test_module_receipt(ULONG* receipt) { detach_receipt = receipt; }
extern "C" BOOL WINAPI era_test_dll_main(HINSTANCE, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { ++attaches; Sleep(0); }
    if (reason == DLL_PROCESS_DETACH && detach_receipt) ++*detach_receipt;
    return TRUE;
}
#else
extern "C" unsigned long long era_test_entry() {
    Sleep(0);
    const DWORD pid = GetCurrentProcessId();
    return (static_cast<unsigned long long>(*pointer) << 32) | pid;
}
#endif
