// SPDX-License-Identifier: BSD-3-Clause
#include "xboxone_native/era_loader.h"
#include "era_audio.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <oleauto.h>

#include <string_view>
#include <atomic>
#include <utility>

namespace xboxone::era {
namespace {

HRESULT WINAPI title_CoCreateInstance(REFCLSID clsid, IUnknown* outer, DWORD context, REFIID iid, void** out) {
    if (clsid != __uuidof(MMDeviceEnumerator)) return CoCreateInstance(clsid, outer, context, iid, out);
    if (!out) return E_POINTER;
    *out = nullptr;
    if (outer) return CLASS_E_NOAGGREGATION;
    IMMDeviceEnumerator* host = nullptr;
    HRESULT hr = CoCreateInstance(clsid, nullptr, context, IID_PPV_ARGS(&host));
    if (SUCCEEDED(hr)) { hr = wrap_audio_enumerator(host, iid, out); host->Release(); }
    return hr;
}

// Keep each CRT/CX major version: VS2012 objects must not bind to VS2015+.
// Resolve named exports from the matching System32 runtime only.
// WinRT class activation through them reaches host classes; Xbox-only classes
// fail there with the host's real error until a title activation layer exists.
constexpr std::pair<std::string_view, const wchar_t*> host_modules[]{
    {"msvcr110.dll", L"msvcr110.dll"},
    {"msvcp110.dll", L"msvcp110.dll"},
    {"vccorlib110.dll", L"vccorlib110.dll"},
    {"ucrtbase.dll", L"ucrtbase.dll"},
    {"vcruntime140.dll", L"vcruntime140.dll"},
    {"vcruntime140_1.dll", L"vcruntime140_1.dll"},
    {"msvcp140.dll", L"msvcp140.dll"},
    {"concrt140.dll", L"concrt140.dll"},
    {"vccorlib140.dll", L"vccorlib140.dll"},
    {"combase.dll", L"combase.dll"}};

// Ordinal/name pairs verified against this title revision's ws2_32 exports
// and the desktop DLL. Resolve by name after validation, never an arbitrary ordinal.
constexpr std::pair<unsigned, const char*> socket_ordinals[]{
    {1,"accept"}, {2,"bind"}, {3,"closesocket"}, {4,"connect"}, {5,"getpeername"},
    {6,"getsockname"}, {7,"getsockopt"}, {8,"htonl"}, {9,"htons"}, {10,"ioctlsocket"},
    {11,"inet_addr"}, {12,"inet_ntoa"}, {13,"listen"}, {14,"ntohl"}, {15,"ntohs"},
    {16,"recv"}, {17,"recvfrom"}, {18,"select"}, {19,"send"}, {20,"sendto"},
    {21,"setsockopt"}, {22,"shutdown"}, {23,"socket"}, {51,"gethostbyaddr"}, {52,"gethostbyname"},
    {111,"WSAGetLastError"}, {112,"WSASetLastError"}, {115,"WSAStartup"}, {116,"WSACleanup"}, {151,"__WSAFDIsSet"}};
constexpr std::string_view socket_names[]{
    "WSAWaitForMultipleEvents", "WSASend", "WSARecv", "WSAGetOverlappedResult", "WSAEventSelect",
    "WSACloseEvent", "WSACreateEvent", "WSARecvFrom", "WSAResetEvent", "WSASendTo", "WSASetEvent",
    "WSASocketW", "getaddrinfo", "freeaddrinfo", "getnameinfo"};

void* socket_import(const ImportRequest& request) {
    if (!same_library(request.library, "ws2_32.dll")) return nullptr;
    const char* symbol = nullptr;
    for (const auto& [ordinal, name] : socket_ordinals)
        if ((request.name.empty() && request.ordinal == ordinal) || request.name == name) symbol = name;
    for (const auto name : socket_names)
        if (request.name == name) symbol = name.data();
    if (!symbol) return nullptr;
    const HMODULE module = LoadLibraryExW(L"ws2_32.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    return module ? reinterpret_cast<void*>(GetProcAddress(module, symbol)) : nullptr;
}

void fatal_note(const char* text) {
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), text, static_cast<DWORD>(lstrlenA(text)), &written, nullptr);
    OutputDebugStringA(text);
}

template<bool Modern>
[[noreturn]] void __cdecl era_raise_com_exception(HRESULT hr) {
    static std::atomic<UINT> reports{0};
    if (reports.fetch_add(1)<8) {
        char line[96];
        wsprintfA(line,"xone-era: C++/CX %u raise COMException HRESULT 0x%08lX\n",Modern ? 140 : 110,hr);
        fatal_note(line);
    }
    const auto module=GetModuleHandleW(Modern ? L"vccorlib140.dll" : L"vccorlib110.dll");
    const auto raise=reinterpret_cast<void(__cdecl*)(HRESULT)>(GetProcAddress(module,"?__abi_WinRTraiseCOMException@@YAXJ@Z"));
    raise(hr); // Preserve the matching host runtime's real exception and unwind.
    __assume(0);
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
    // Audited imports of the pinned original XG helpers; named MSVC ABI exports
    // are resolved from the same host CRT, never from console export ordinals.
    if (same_library(request.library, "msvcrt.dll") || same_library(request.library, "ntdll.dll")) {
        if (request.name.empty()) return nullptr;
        bool supported = false;
        if (same_library(request.library, "msvcrt.dll")) {
            for (const auto* name : {"memcpy", "memset", "vsprintf_s", "__C_specific_handler", "_initterm",
                "malloc", "free", "_amsg_exit", "_XcptFilter", "sqrt", "printf", "sprintf", "_purecall",
                "__CxxFrameHandler3", "_CxxThrowException", "??1type_info@@UEAA@XZ", "?what@exception@@UEBAPEBDXZ",
                "??1exception@@UEAA@XZ", "??0exception@@QEAA@AEBV0@@Z", "??0exception@@QEAA@AEBQEBD@Z"}) supported |= request.name == name;
        } else {
            for (const auto* name : {"RtlLookupFunctionEntry", "RtlCaptureContext", "RtlVirtualUnwind"}) supported |= request.name == name;
        }
        if (!supported) return nullptr;
        const HMODULE module = LoadLibraryExW(same_library(request.library, "msvcrt.dll") ? L"msvcrt.dll" : L"ntdll.dll",
            nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return module ? reinterpret_cast<void*>(GetProcAddress(module, request.name.c_str())) : nullptr;
    }
    if (same_library(request.library, "ws2_32.dll")) return socket_import(request);
    if (same_library(request.library, "bcrypt.dll") && request.name == "BCryptGenRandom") {
        const HMODULE module = LoadLibraryExW(L"bcrypt.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        return module ? reinterpret_cast<void*>(GetProcAddress(module, "BCryptGenRandom")) : nullptr;
    }
    if (same_library(request.library, "combase.dll") && LoadLibraryExW(L"combase.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
        if (request.name == "CoCreateInstance") return reinterpret_cast<void*>(&title_CoCreateInstance);
        if (request.name == "RoFailFastWithErrorContext") return reinterpret_cast<void*>(&era_RoFailFastWithErrorContext);
        if (request.name == "RoReportUnhandledError") return reinterpret_cast<void*>(&era_RoReportUnhandledError);
    }
    for (const auto& [library, file] : host_modules) {
        if (!same_library(request.library, library)) continue;
        // Console and desktop export ordinals are not an ABI contract.
        if (request.name.empty() && (library == "msvcr110.dll" || library == "msvcp110.dll" ||
                                     library == "vccorlib110.dll")) return nullptr;
        const HMODULE module = LoadLibraryExW(file, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) return nullptr;
        const char* symbol = request.name.empty() ? MAKEINTRESOURCEA(request.ordinal) : request.name.c_str();
        const auto address=GetProcAddress(module, symbol);
        if (address && request.name=="?__abi_WinRTraiseCOMException@@YAXJ@Z") {
            if (library=="vccorlib110.dll") return reinterpret_cast<void*>(&era_raise_com_exception<false>);
            if (library=="vccorlib140.dll") return reinterpret_cast<void*>(&era_raise_com_exception<true>);
        }
        return reinterpret_cast<void*>(address);
    }
    return nullptr;
}

void* resolve_title_import(const ImportRequest& request) {
    if (void* address = resolve_kernelx(request)) return address;
    if (void* address = resolve_acp(request)) return address;
    if (void* address = resolve_etwplus(request)) return address;
    if (void* address = resolve_d3d11x(request)) return address;
    if (void* address = resolve_xg_module(request)) return address;
    return resolve_host_module(request);
}

} // namespace xboxone::era
