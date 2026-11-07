// SPDX-License-Identifier: BSD-3-Clause
// Owned packaged desktop test. No CRT, window, renderer, disk writes or Xbox APIs.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <appmodel.h>

extern "C" void appmodel_test_entry() {
    wchar_t identity[512];
    UINT32 size = 512;
    if (GetCurrentPackageFullName(&size, identity) != ERROR_SUCCESS) ExitProcess(31);
    size = 512;
    if (GetCurrentApplicationUserModelId(&size, identity) != ERROR_SUCCESS) ExitProcess(32);
    constexpr wchar_t marker[] = L"--xboxone-appmodel-test=";
    const auto* command = GetCommandLineW();
    const wchar_t* nonce = nullptr;
    for (const auto* p = command; *p; ++p) {
        unsigned i = 0;
        while (marker[i] && p[i] == marker[i]) ++i;
        if (!marker[i]) { nonce = p + i; break; }
    }
    if (!nonce) {
        // Normal process observation has no fixture handshake or stop request.
        Sleep(1000);
        ExitProcess(0);
    }
    wchar_t token[39];
    token[38] = 0;
    for (unsigned i = 0; i < 38; ++i) {
        if (!nonce[i] || !(nonce[i] == '{' || nonce[i] == '}' || nonce[i] == '-' ||
            (nonce[i] >= '0' && nonce[i] <= '9') || (nonce[i] >= 'A' && nonce[i] <= 'F') ||
            (nonce[i] >= 'a' && nonce[i] <= 'f'))) ExitProcess(34);
        token[i] = nonce[i];
    }
    wchar_t name[128];
    lstrcpyW(name, L"Local\\XON-AppModel-"); lstrcatW(name, token);
    const auto base = lstrlenW(name);
    lstrcatW(name, L"-started");
    HANDLE started = OpenEventW(EVENT_MODIFY_STATE, FALSE, name);
    name[base] = 0; lstrcatW(name, L"-stop");
    HANDLE stop = OpenEventW(SYNCHRONIZE, FALSE, name);
    name[base] = 0; lstrcatW(name, L"-finished");
    HANDLE finished = OpenEventW(EVENT_MODIFY_STATE, FALSE, name);
    if (!started || !stop || !finished) ExitProcess(35);
    if (!SetEvent(started)) ExitProcess(36);
    // Self-bounded even if the supervisor disappears or activation returns late.
    if (WaitForSingleObject(stop, 10000) != WAIT_OBJECT_0) ExitProcess(37);
    if (!SetEvent(finished)) ExitProcess(38);
    CloseHandle(started); CloseHandle(stop); CloseHandle(finished);
    ExitProcess(0);
}
