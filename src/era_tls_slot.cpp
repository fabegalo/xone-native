// SPDX-License-Identifier: BSD-3-Clause
// Static TLS index for one in-process title image. Loading this DLL makes the
// Windows loader reserve a TLS index for every current and future thread. The
// title's index is pointed at it, and on each thread the slot entry is swapped
// for a block of the title's exact size holding a fresh template copy; the
// Windows-owned entry is restored before the thread exits. No CRT.
//
// Observed on Windows build 26300: a dynamically loaded module whose TLS
// template exceeds 16 bytes, or that uses SizeOfZeroFill, corrupts the heap
// during LoadLibrary/exit. The module's own template therefore stays 16 bytes.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>

#include "era_tls_slot.h"

extern "C" {
unsigned long _tls_index = 0;
#pragma section(".tls", read, write)
__declspec(allocate(".tls")) unsigned char era_tls_raw[16] = {};
}

namespace {
constexpr SIZE_T header = 16; // Saved Windows entry; keeps title data 16-byte aligned.
EraTlsTitle title{};
volatile LONG configured = 0;

void** slot_entry() {
    auto** blocks = reinterpret_cast<void**>(__readgsqword(0x58)); // TEB ThreadLocalStoragePointer
    return blocks ? &blocks[_tls_index] : nullptr;
}

bool install() {
    void** entry = slot_entry();
    if (!entry) return false;
    auto* block = static_cast<unsigned char*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, header + title.size));
    if (!block) return false;
    *reinterpret_cast<void**>(block) = *entry;
    // Intrinsic, not a loop: a loop may be lowered to memcpy, absent without CRT.
    __movsb(block + header, static_cast<const unsigned char*>(title.raw), title.raw_size);
    *entry = block + header;
    return true;
}

void uninstall() {
    void** entry = slot_entry();
    if (!entry || !*entry) return;
    auto* block = static_cast<unsigned char*>(*entry) - header;
    *entry = *reinterpret_cast<void**>(block);
    HeapFree(GetProcessHeap(), 0, block);
}

void call_title(DWORD reason) {
    for (auto* cb = title.callbacks; cb && *cb; ++cb) (*cb)(title.image, reason, nullptr);
}

void NTAPI on_thread(void*, DWORD reason, void*) {
    if (configured != 1) return;
    if (reason == DLL_THREAD_ATTACH) {
        if (!install()) __fastfail(FAST_FAIL_FATAL_APP_EXIT); // Title TLS would be invalid.
        call_title(DLL_THREAD_ATTACH);
    } else if (reason == DLL_THREAD_DETACH) {
        call_title(DLL_THREAD_DETACH);
        uninstall();
    }
}

const PIMAGE_TLS_CALLBACK callbacks[] = {on_thread, nullptr};
} // namespace

extern "C" const IMAGE_TLS_DIRECTORY64 _tls_used = {
    reinterpret_cast<ULONGLONG>(&era_tls_raw[0]), reinterpret_cast<ULONGLONG>(&era_tls_raw[16]),
    reinterpret_cast<ULONGLONG>(&_tls_index), reinterpret_cast<ULONGLONG>(callbacks), 0, 0};

extern "C" BOOL WINAPI era_tls_dll_main(HINSTANCE, DWORD, void*) { return TRUE; }

// Binds the index to one title (once per process) and installs the calling
// thread's block. Title TLS callbacks are not run here; the caller decides.
extern "C" __declspec(dllexport) BOOL era_tls_bind(const EraTlsTitle* request, DWORD* index) {
    if (!request || !index || request->raw_size > request->size || request->size > ERA_TLS_MAX_BYTES) return FALSE;
    if (InterlockedCompareExchange(&configured, 2, 0) != 0) return FALSE;
    title = *request;
    if (!install()) { InterlockedExchange(&configured, 0); return FALSE; }
    *index = _tls_index;
    InterlockedExchange(&configured, 1);
    return TRUE;
}

// Stops thread notifications and restores the calling thread's Windows entry.
// Threads still running keep their block; the title must not outlive its image.
extern "C" __declspec(dllexport) void era_tls_unbind() {
    if (InterlockedCompareExchange(&configured, 3, 1) == 1) uninstall();
}
