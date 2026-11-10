// SPDX-License-Identifier: BSD-3-Clause
#include "xboxone_native/era_loader.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

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

} // namespace

void* resolve_host_module(const ImportRequest& request) {
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
