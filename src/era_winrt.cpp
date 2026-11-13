// SPDX-License-Identifier: BSD-3-Clause
// WinRT activation boundary of the title. The title's C++/CX runtime (host
// vccorlib110/140) resolves classes through RoGetActivationFactory; that import is
// redirected here. Classes the title layer implements are served here; every
// other request is forwarded to the host and its real result returned. Every
// activation is logged.
#include "xboxone_native/era_loader.h"
#include "era_winrt_classes.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <roapi.h>
#include <winstring.h>

#include <cstring>
#include <atomic>
#include <string_view>

namespace xboxone::era {
namespace {

using GetFactory = HRESULT(WINAPI*)(HSTRING, REFIID, void**);
GetFactory host_get_factory = nullptr;
std::atomic<bool> stop_on_missing_activation{false};

struct TitleClass {
    std::wstring_view name;
    HRESULT (*factory)(REFIID, void**);
};
constexpr TitleClass title_classes[]{
    {L"Windows.ApplicationModel.Package", &package_factory},
    {L"Windows.Xbox.Services.XboxLiveConfiguration", &xbox_live_configuration_factory},
    {L"Windows.Xbox.System.User", &xbox_user_factory},
    {L"Windows.Xbox.Input.Controller", &xbox_controller_factory},
    {L"Windows.Xbox.Input.Gamepad", &xbox_gamepad_factory},
    {L"Windows.Xbox.Management.Deployment.DownloadableContentPackageManager", &dlc_package_manager_factory},
    {L"Windows.Xbox.Management.Deployment.PackageTransferManager", &package_transfer_manager_factory},
    {L"Windows.ApplicationModel.Core.CoreApplication", &core_application_factory},
    {L"Windows.UI.Core.CoreWindow", &core_window_factory},
};

void note(const wchar_t* text) {
    char line[600];
    wsprintfA(line, "%ls", text);
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), line, static_cast<DWORD>(lstrlenA(line)), &written, nullptr);
}

HRESULT WINAPI era_RoGetActivationFactory(HSTRING class_id, REFIID iid, void** factory) {
    const HRESULT hr = get_title_activation_factory(class_id, iid, factory);
    UINT32 length = 0;
    const wchar_t* name = WindowsGetStringRawBuffer(class_id, &length);
    bool served = false;
    for (const auto& c : title_classes) served = served || c.name == std::wstring_view(name, length);
    wchar_t line[400], requested[40] = L"?";
    StringFromGUID2(iid, requested, 40);
    wsprintfW(line, L"xone-era: activation %ls %ls -> 0x%08lX%ls\n", name, requested, static_cast<unsigned long>(hr),
              served ? L" (title layer)" : L"");
    note(line);
    return hr;
}

bool ascii_prefix(const char* text, const char* prefix) {
    for (; *prefix; ++text, ++prefix) {
        const char a = *text >= 'A' && *text <= 'Z' ? *text + 32 : *text;
        if (a != *prefix) return false;
    }
    return true;
}

} // namespace

HRESULT get_title_activation_factory(HSTRING class_id, REFIID iid, void** factory) {
    if (!factory) return E_POINTER;
    *factory = nullptr;
    UINT32 length = 0;
    const wchar_t* name = WindowsGetStringRawBuffer(class_id, &length);
    for (const auto& c : title_classes)
        if (c.name == std::wstring_view(name, length)) return c.factory(iid, factory);
    const HRESULT hr = host_get_factory ? host_get_factory(class_id, iid, factory) :
        RoGetActivationFactory(class_id, iid, factory);
    if (hr == REGDB_E_CLASSNOTREG && stop_on_missing_activation.load()) {
        wchar_t line[400];
        _snwprintf_s(line, _countof(line), _TRUNCATE, L"xone-era: unimplemented activation %ls; stopped\n", name);
        note(line);
        TerminateProcess(GetCurrentProcess(), 125);
    }
    return hr;
}

void set_stop_on_missing_activation(bool stop) { stop_on_missing_activation.store(stop); }
bool stop_on_missing_service() { return stop_on_missing_activation.load(); }

bool hook_winrt_activation(void* module_base) {
    auto* base = static_cast<std::uint8_t*>(module_base);
    if (!base) return false;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return false;
    for (auto* d = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name; ++d) {
        if (!ascii_prefix(reinterpret_cast<const char*>(base + d->Name), "api-ms-win-core-winrt-l1-") &&
            !ascii_prefix(reinterpret_cast<const char*>(base + d->Name), "combase.dll"))
            continue;
        auto* names = reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + d->OriginalFirstThunk);
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + d->FirstThunk);
        for (; names->u1.AddressOfData; ++names, ++slots) {
            if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal)) continue;
            const auto* by_name = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
            if (std::strcmp(reinterpret_cast<const char*>(by_name->Name), "RoGetActivationFactory") != 0) continue;
            DWORD old = 0;
            if (!VirtualProtect(&slots->u1.Function, sizeof slots->u1.Function, PAGE_READWRITE, &old)) return false;
            if (!host_get_factory) host_get_factory = reinterpret_cast<GetFactory>(slots->u1.Function);
            slots->u1.Function = reinterpret_cast<ULONGLONG>(&era_RoGetActivationFactory);
            VirtualProtect(&slots->u1.Function, sizeof slots->u1.Function, old, &old);
            return true;
        }
    }
    return false;
}

} // namespace xboxone::era
