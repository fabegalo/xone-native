// SPDX-License-Identifier: BSD-3-Clause
// Static TLS index for one in-process image. Loading this DLL makes the
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
#include <tlhelp32.h>
#include <intrin.h>

#include "era_tls_slot.h"

extern "C" {
unsigned long _tls_index = 0;
#pragma section(".tls", read, write)
__declspec(allocate(".tls")) unsigned char era_tls_raw[16] = {};
}

namespace {
struct alignas(16) Block {
    Block* next;
    void* original;
    DWORD thread;
    bool notified;
    unsigned char reserved[11];
};
constexpr SIZE_T header = sizeof(Block);
EraTlsTitle title{};
volatile LONG configured = 0;
volatile LONG blocks = 0;
DWORD owner = 0;
Block* installed = nullptr;
SRWLOCK registry = SRWLOCK_INIT;
struct ThreadInformation {
    LONG status;
    void* teb;
    ULONG_PTR process, thread, affinity;
    LONG priority, base_priority;
};
static_assert(sizeof(ThreadInformation) == 0x30);
using QueryThread = LONG(NTAPI*)(HANDLE, ULONG, void*, ULONG, ULONG*);
QueryThread query_thread = nullptr;

void** slot_entry() {
    auto** entries = reinterpret_cast<void**>(__readgsqword(0x58)); // TEB ThreadLocalStoragePointer
    return entries ? &entries[_tls_index] : nullptr;
}

Block** find_block(DWORD thread) {
    auto** at = &installed;
    while (*at && (*at)->thread != thread) at = &(*at)->next;
    return at;
}

Block* allocate(DWORD thread) {
    auto* block = static_cast<Block*>(HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, header + title.size));
    if (!block) return nullptr;
    block->thread = thread;
    // Intrinsic, not a loop: a loop may be lowered to memcpy, absent without CRT.
    __movsb(reinterpret_cast<unsigned char*>(block) + header, static_cast<const unsigned char*>(title.raw), title.raw_size);
    return block;
}

void publish(Block* block, void** entry) {
    block->original = *entry;
    block->next = installed;
    installed = block;
    *entry = reinterpret_cast<unsigned char*>(block) + header;
    InterlockedIncrement(&blocks);
}

bool install() {
    void** entry = slot_entry();
    if (!entry) return false;
    AcquireSRWLockExclusive(&registry);
    const LONG state = InterlockedCompareExchange(&configured, 0, 0);
    if (state == 3 || state == 0) { ReleaseSRWLockExclusive(&registry); return true; }
    auto* block = allocate(GetCurrentThreadId());
    if (!block) { ReleaseSRWLockExclusive(&registry); return false; }
    if (!*find_block(block->thread)) { publish(block, entry); block = nullptr; }
    ReleaseSRWLockExclusive(&registry);
    if (block) HeapFree(GetProcessHeap(), 0, block);
    return true;
}

void uninstall() {
    void** entry = slot_entry();
    AcquireSRWLockExclusive(&registry);
    auto** at = find_block(GetCurrentThreadId());
    auto* block = *at;
    if (block) {
        if (entry) *entry = block->original;
        *at = block->next;
        InterlockedDecrement(&blocks);
    }
    ReleaseSRWLockExclusive(&registry);
    if (block) HeapFree(GetProcessHeap(), 0, block);
}

// Same-process TEB/static slot access follows reference FUN_14054dc60. Allocate
// before suspension: the target may own the process heap or loader lock.
bool update_thread(DWORD thread, bool remove) {
    HANDLE handle = OpenThread(THREAD_QUERY_INFORMATION | THREAD_SUSPEND_RESUME | SYNCHRONIZE, FALSE, thread);
    if (!handle) return false;
    ThreadInformation info{};
    const LONG status = query_thread(handle, 0, &info, sizeof info, nullptr);
    if (status < 0 || !info.teb) { CloseHandle(handle); SetLastError(status < 0 ? static_cast<DWORD>(status) : ERROR_INVALID_ADDRESS); return false; }
    auto* block = remove ? nullptr : allocate(thread);
    if (!remove && !block) { CloseHandle(handle); SetLastError(ERROR_NOT_ENOUGH_MEMORY); return false; }
    bool success = false;
    AcquireSRWLockExclusive(&registry);
    auto** at = find_block(thread);
    if (remove ? !*at : *at != nullptr) success = true;
    else if (SuspendThread(handle) != DWORD(-1)) {
        auto** entries = *reinterpret_cast<void***>(static_cast<unsigned char*>(info.teb) + 0x58);
        if (entries) {
            if (remove) {
                block = *at;
                entries[_tls_index] = block->original;
                *at = block->next;
                InterlockedDecrement(&blocks);
            } else { publish(block, &entries[_tls_index]); block = nullptr; }
            success = true;
        }
        // A native thread still entering/leaving the loader may have no table.
        // Its helper callback installs/removes the block at the loader boundary.
        if (!entries) {
            if (remove) {
                block = *at;
                *at = block->next;
                InterlockedDecrement(&blocks);
            }
            success = true;
        }
        if (ResumeThread(handle) == DWORD(-1)) __fastfail(FAST_FAIL_FATAL_APP_EXIT);
    }
    const DWORD error = GetLastError();
    ReleaseSRWLockExclusive(&registry);
    if (block) HeapFree(GetProcessHeap(), 0, block);
    CloseHandle(handle);
    if (!success) SetLastError(error);
    return success;
}

bool existing_threads() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    THREADENTRY32 entry{}; entry.dwSize = sizeof entry;
    bool success = Thread32First(snapshot, &entry) != FALSE;
    DWORD error = success ? ERROR_SUCCESS : GetLastError();
    if (success) do {
        if (entry.th32OwnerProcessID == GetCurrentProcessId() && entry.th32ThreadID != owner &&
            !update_thread(entry.th32ThreadID, false)) {
            // A thread may exit between the snapshot and OpenThread.
            const DWORD update_error = GetLastError();
            HANDLE thread = OpenThread(SYNCHRONIZE, FALSE, entry.th32ThreadID);
            if (thread && WaitForSingleObject(thread, 0) != WAIT_OBJECT_0) { success = false; error = update_error; }
            if (!thread && GetLastError() != ERROR_INVALID_PARAMETER) { success = false; error = update_error; }
            if (thread) CloseHandle(thread);
        }
    } while (Thread32Next(snapshot, &entry));
    const DWORD enumeration_error = GetLastError();
    if (enumeration_error != ERROR_NO_MORE_FILES) { success = false; error = enumeration_error; }
    CloseHandle(snapshot);
    if (!success) SetLastError(error);
    return success;
}

void retire() {
    InterlockedExchange(&configured, 3);
    uninstall();
    // Enumerate IDs rather than retaining nodes across detach callbacks.
    DWORD previous = 0;
    for (;;) {
        DWORD next = 0;
        AcquireSRWLockShared(&registry);
        for (auto* at = installed; at; at = at->next)
            if (at->thread > previous && (!next || at->thread < next)) next = at->thread;
        ReleaseSRWLockShared(&registry);
        if (!next) break;
        update_thread(next, true);
        previous = next;
    }
#ifdef ERA_TLS_MODULE_SLOT
    if (InterlockedCompareExchange(&blocks, 0, 0) == 0) InterlockedCompareExchange(&configured, 0, 3);
#endif
}

void call_title(DWORD reason) {
    for (auto* cb = title.callbacks; cb && *cb; ++cb) (*cb)(title.image, reason, nullptr);
}

void NTAPI on_thread(void*, DWORD reason, void*) {
    const LONG state = InterlockedCompareExchange(&configured, 0, 0);
    if (reason == DLL_THREAD_ATTACH) {
        if (state != 1 && state != 2 && state != 5) return;
        if (!install()) __fastfail(FAST_FAIL_FATAL_APP_EXIT); // Title TLS would be invalid.
        if (InterlockedCompareExchange(&configured, 0, 0) == 1) {
            call_title(DLL_THREAD_ATTACH);
            if (title.module_entry) title.module_entry(static_cast<HINSTANCE>(title.image), DLL_THREAD_ATTACH, nullptr);
            AcquireSRWLockExclusive(&registry);
            if (auto* block = *find_block(GetCurrentThreadId())) block->notified = true;
            ReleaseSRWLockExclusive(&registry);
        }
    } else if (reason == DLL_THREAD_DETACH) {
        AcquireSRWLockShared(&registry);
        const auto* block = *find_block(GetCurrentThreadId());
        const bool notified = block && block->notified;
        ReleaseSRWLockShared(&registry);
        if (state == 1 && notified) {
            call_title(DLL_THREAD_DETACH);
            if (title.module_entry) title.module_entry(static_cast<HINSTANCE>(title.image), DLL_THREAD_DETACH, nullptr);
        }
        uninstall();
#ifdef ERA_TLS_MODULE_SLOT
        if (state == 3 && InterlockedCompareExchange(&blocks, 0, 0) == 0) InterlockedCompareExchange(&configured, 0, 3);
#endif
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
    if (!request || !index || (request->raw_size && !request->raw) ||
        request->raw_size > request->size || request->size > ERA_TLS_MAX_BYTES) return FALSE;
    if (InterlockedCompareExchange(&configured, 4, 0) != 0) { SetLastError(ERROR_BUSY); return FALSE; }
    query_thread = reinterpret_cast<QueryThread>(GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "NtQueryInformationThread"));
    if (!query_thread) { InterlockedExchange(&configured, 0); SetLastError(ERROR_NOT_SUPPORTED); return FALSE; }
    title = *request;
    if (!install()) { InterlockedExchange(&configured, 0); SetLastError(ERROR_NOT_ENOUGH_MEMORY); return FALSE; }
    owner = GetCurrentThreadId();
    *index = _tls_index;
    InterlockedExchange(&configured, 5);
    if (!existing_threads()) { const DWORD error = GetLastError(); retire(); SetLastError(error); return FALSE; }
    InterlockedExchange(&configured, 2);
    return TRUE;
}

extern "C" __declspec(dllexport) BOOL era_tls_attach() {
    if (GetCurrentThreadId() != owner) return FALSE;
    if (InterlockedCompareExchange(&configured, 1, 2) != 2) return InterlockedCompareExchange(&configured, 0, 0) == 1;
    call_title(DLL_PROCESS_ATTACH);
    if (title.module_entry && !title.module_entry(static_cast<HINSTANCE>(title.image), DLL_PROCESS_ATTACH, nullptr)) {
        call_title(DLL_PROCESS_DETACH);
        title.module_entry(static_cast<HINSTANCE>(title.image), DLL_PROCESS_DETACH, nullptr);
        InterlockedExchange(&configured, 6);
        return FALSE;
    }
    return TRUE;
}

// Stops guest notifications and restores Windows entries on reachable threads.
// A retiring thread keeps its block until its helper detach callback runs.
extern "C" __declspec(dllexport) void era_tls_unbind() {
    if (GetCurrentThreadId() != owner) __fastfail(FAST_FAIL_FATAL_APP_EXIT);
    const LONG previous = InterlockedExchange(&configured, 3);
    if (previous == 1) {
        call_title(DLL_PROCESS_DETACH);
        if (title.module_entry) title.module_entry(static_cast<HINSTANCE>(title.image), DLL_PROCESS_DETACH, nullptr);
    }
    retire();
}
