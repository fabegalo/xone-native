// SPDX-License-Identifier: BSD-3-Clause
#include "xboxone_native/era_loader.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <oleauto.h>

#include <string_view>
#include <utility>

namespace xboxone::era {
namespace {

// Libraries whose PC build has the same ABI as the title's copy: the VS2015+
// CRT family, the C++/CX runtime and COM/WinRT base. Loaded from System32 only.
// WinRT class activation through them reaches host classes; Xbox-only classes
// fail there with the host's real error until a title activation layer exists.
constexpr std::pair<std::string_view, const wchar_t*> host_modules[]{
    {"ucrtbase.dll", L"ucrtbase.dll"},
    {"vcruntime140.dll", L"vcruntime140.dll"},
    {"vcruntime140_1.dll", L"vcruntime140_1.dll"},
    {"msvcp140.dll", L"msvcp140.dll"},
    {"concrt140.dll", L"concrt140.dll"},
    {"vccorlib140.dll", L"vccorlib140.dll"},
    {"combase.dll", L"combase.dll"}};

void fatal_note(const char* text) {
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), text, static_cast<DWORD>(lstrlenA(text)), &written, nullptr);
    OutputDebugStringA(text);
}

// Fatal WinRT error paths of the title: record the error, then the host call
// performs the real fail-fast. Nothing is suppressed.
using RoFailFast = void(WINAPI*)(HRESULT);
using RoReport = HRESULT(WINAPI*)(IUnknown*);

void WINAPI era_RoFailFastWithErrorContext(HRESULT hr) {
    char line[96];
    wsprintfA(line, "xone-era: RoFailFastWithErrorContext hr=0x%08lX\n", static_cast<unsigned long>(hr));
    fatal_note(line);
    reinterpret_cast<RoFailFast>(GetProcAddress(GetModuleHandleW(L"combase.dll"), "RoFailFastWithErrorContext"))(hr);
}

HRESULT WINAPI era_RoReportUnhandledError(IUnknown* error) {
    char line[1200] = "xone-era: RoReportUnhandledError (no details)\n";
    struct DECLSPEC_UUID("82BA7092-4C88-427D-A7BC-16DD93FEB67E") IRestrictedErrorInfo : IUnknown {
        virtual HRESULT STDMETHODCALLTYPE GetErrorDetails(BSTR*, HRESULT*, BSTR*, BSTR*) = 0;
        virtual HRESULT STDMETHODCALLTYPE GetReference(BSTR*) = 0;
    };
    IRestrictedErrorInfo* info = nullptr;
    if (error && SUCCEEDED(error->QueryInterface(__uuidof(IRestrictedErrorInfo), reinterpret_cast<void**>(&info)))) {
        BSTR description = nullptr, restricted = nullptr, sid = nullptr;
        HRESULT hr = S_OK;
        if (SUCCEEDED(info->GetErrorDetails(&description, &hr, &restricted, &sid)))
            wsprintfA(line, "xone-era: RoReportUnhandledError hr=0x%08lX %ls | %ls\n", static_cast<unsigned long>(hr),
                      description ? description : L"", restricted ? restricted : L"");
        SysFreeString(description); SysFreeString(restricted); SysFreeString(sid);
        info->Release();
    }
    fatal_note(line);
    return reinterpret_cast<RoReport>(GetProcAddress(GetModuleHandleW(L"combase.dll"), "RoReportUnhandledError"))(error);
}

} // namespace

void* resolve_host_module(const ImportRequest& request) {
    if (same_library(request.library, "combase.dll") && LoadLibraryExW(L"combase.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        if (request.name == "RoFailFastWithErrorContext") return reinterpret_cast<void*>(&era_RoFailFastWithErrorContext);
        if (request.name == "RoReportUnhandledError") return reinterpret_cast<void*>(&era_RoReportUnhandledError);
    }
    for (const auto& [library, file] : host_modules) {
        if (!same_library(request.library, library)) continue;
        const HMODULE module = LoadLibraryExW(file, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) return nullptr;
        const char* symbol = request.name.empty() ? MAKEINTRESOURCEA(request.ordinal) : request.name.c_str();
        return reinterpret_cast<void*>(GetProcAddress(module, symbol));
    }
    return nullptr;
}

void* resolve_title_import(const ImportRequest& request) {
    if (void* address = resolve_kernelx(request)) return address;
    if (void* address = resolve_etwplus(request)) return address;
    if (void* address = resolve_d3d11x(request)) return address;
    return resolve_host_module(request);
}

} // namespace xboxone::era
