// SPDX-License-Identifier: BSD-3-Clause
// Owned synthetic title image. No CRT; every API comes from kernelx.dll, which
// only the in-process ERA loader can satisfy (no kernelx.dll exists on disk).
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static int value = 42;
static int* volatile pointer = &value; // Absolute address: requires a DIR64 relocation.

extern "C" unsigned long long era_test_entry() {
    Sleep(0);
    const DWORD pid = GetCurrentProcessId();
    return (static_cast<unsigned long long>(*pointer) << 32) | pid;
}
