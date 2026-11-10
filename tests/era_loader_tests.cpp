// SPDX-License-Identifier: BSD-3-Clause
#include "xboxone_native/era_loader.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <evntprov.h>
#include <d3dcommon.h>
#include <dxgi.h>
#include <activation.h>
#include <roapi.h>
#include <winstring.h>

#include <fstream>
#include <string>
#include <iostream>
#include <iterator>

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++failures; std::cerr << "FAIL: " << what << '\n'; }
}
std::vector<std::uint8_t> read(const char* path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}
} // namespace

int main(int argc, char** argv) {
    using namespace xboxone::era;
    if (argc != 3) { std::cerr << "usage: era_loader_tests <era_component.exe> <era_tls_component.exe>\n"; return 2; }
    {
        // Static TLS: one title per process, bound to xone-era-tls.dll's slot.
        const auto tls_file = read(argv[2]);
        LoadResult result;
        {
            Image image;
            check(image.load(tls_file, resolve_kernelx, result), "TLS title loads");
            for (const auto& e : result.errors) std::cerr << "  " << e << '\n';
            if (image.entry()) {
                image.process_attach();
                const auto code = reinterpret_cast<unsigned long long (*)()>(image.entry())();
                if (code) std::cerr << "  TLS title returned " << code << '\n';
                check(code == 0, "TLS template, callbacks and per-thread isolation hold");
            }
        }
        Image second;
        check(!second.load(tls_file, resolve_kernelx, result), "slot is not silently reused by a second title");
    }
    const auto file = read(argv[1]);
    check(file.size() > 512, "fixture readable");

    {
        Image image;
        LoadResult result;
        check(image.load(file, resolve_kernelx, result), "fixture loads with kernelx contract");
        for (const auto& e : result.errors) std::cerr << "  " << e << '\n';
        check(result.resolved == 2 && result.unresolved.empty(), "both kernelx imports bound");
        check(image.entry() != nullptr, "entry point mapped");
        const auto module_handle = reinterpret_cast<HMODULE (WINAPI*)(LPCWSTR)>(
            resolve_kernelx({"kernelx.dll", "GetModuleHandleW", 0}));
        check(module_handle && module_handle(nullptr) == reinterpret_cast<HMODULE>(image.base()),
              "GetModuleHandleW(NULL) names the title image, not the launcher");
        check(module_handle && module_handle(L"kernel32.dll") == GetModuleHandleW(L"kernel32.dll"),
              "named GetModuleHandleW stays a host lookup");
        if (image.entry()) {
            MEMORY_BASIC_INFORMATION info{};
            VirtualQuery(image.entry(), &info, sizeof info);
            check(info.Protect == PAGE_EXECUTE_READ, "code section is execute-read, not writable");
            DWORD64 unwind_base = 0;
            check(RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(image.entry()), &unwind_base, nullptr) &&
                  unwind_base == reinterpret_cast<DWORD64>(image.base()), "unwind data registered for title code");
            const auto entry = reinterpret_cast<unsigned long long (*)()>(image.entry());
            const auto value = entry();
            check((value & 0xffffffffull) == GetCurrentProcessId(), "title called host through kernelx binding");
            check((value >> 32) == 42, "DIR64 relocation applied at non-preferred base");
        }
    }
    {
        Image image;
        LoadResult result;
        check(!image.load(file, [](const ImportRequest&) -> void* { return nullptr; }, result), "missing contract blocks load");
        check(result.unresolved.size() == 2 && result.unresolved[0].library == "kernelx.dll",
              "every unresolved import reported with its library");
        check(image.base() == nullptr, "failed load releases the image");
    }
    {
        Image image;
        LoadResult result;
        auto truncated = file;
        truncated.resize(256);
        check(!image.load(truncated, resolve_kernelx, result) && !result.errors.empty(), "truncated image rejected");
        auto wrong = file;
        const auto lfanew = *reinterpret_cast<const LONG*>(wrong.data() + 0x3c);
        wrong[lfanew + 4] = 0x4c; wrong[lfanew + 5] = 0x01; // IMAGE_FILE_MACHINE_I386
        check(!image.load(wrong, resolve_kernelx, result), "non-AMD64 image rejected");
    }
    check(resolve_kernelx({"KERNELX.DLL", "GetTickCount64", 0}) != nullptr, "library name is case-insensitive");
    check(resolve_kernelx({"kernelx.dll", "AllocateTitlePhysicalPages", 0}) == nullptr,
          "Xbox-specific export is not faked by a host forward");
    check(resolve_kernelx({"kernelx.dll", "VirtualAlloc", 0}) == nullptr, "title memory API is not forwarded blindly");
    check(resolve_kernelx({"kernel32.dll", "Sleep", 0}) == nullptr, "only kernelx.dll is handled");
    {
        const auto remove = reinterpret_cast<BOOL (WINAPI*)(LPCWSTR)>(resolve_kernelx({"kernelx.dll", "DeleteFileW", 0}));
        check(remove != nullptr, "DeleteFileW bound to the title storage policy");
        wchar_t temp[MAX_PATH];
        GetTempPathW(MAX_PATH, temp);
        const std::wstring base = std::wstring(temp) + L"xone-era-storage-" + std::to_wstring(GetCurrentProcessId());
        const std::wstring root = base + L"\\root", outside = base + L"\\outside.bin", inside = root + L"\\save.bin";
        CreateDirectoryW(base.c_str(), nullptr);
        CreateDirectoryW(root.c_str(), nullptr);
        const auto touch = [](const std::wstring& p) {
            HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
            return h != INVALID_HANDLE_VALUE && CloseHandle(h);
        };
        const auto exists = [](const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; };
        check(touch(inside) && touch(outside), "storage fixtures created");
        if (remove) {
            set_title_storage_root({});
            check(!remove(inside.c_str()) && GetLastError() == ERROR_ACCESS_DENIED && exists(inside),
                  "no writable storage: delete denied like a read-only package");
            set_title_storage_root(root);
            check(!remove(outside.c_str()) && GetLastError() == ERROR_ACCESS_DENIED && exists(outside),
                  "host path outside title storage is never deleted");
            check(!remove((root + L"\\..\\outside.bin").c_str()) && exists(outside), "dot-dot cannot escape storage");
            check(!remove((base + L"\\root-sibling.bin").c_str()) && GetLastError() == ERROR_ACCESS_DENIED,
                  "prefix match requires a path separator");
            check(remove(inside.c_str()) && !exists(inside), "file inside title storage deleted");
            check(!remove(inside.c_str()) && GetLastError() == ERROR_FILE_NOT_FOUND, "real host error kept for missing file");
            set_title_storage_root({});
        }
        DeleteFileW(outside.c_str());
        RemoveDirectoryW(root.c_str());
        RemoveDirectoryW(base.c_str());
    }
    {
        using Register = ULONG (WINAPI*)(const void*, PREGHANDLE);
        using Write = ULONG (WINAPI*)(const void*, const void*, REGHANDLE, ULONG, void*);
        using Fill = void (WINAPI*)(void*, PUINT8, SIZE_T);
        const auto reg = reinterpret_cast<Register>(resolve_title_import({"EtwPlus.dll", "EtxRegister", 0}));
        const auto write = reinterpret_cast<Write>(resolve_title_import({"EtwPlus.dll", "EtxEventWrite", 0}));
        const auto fill = reinterpret_cast<Fill>(resolve_title_import({"EtwPlus.dll", "EtxFillCommonFields_v7", 0}));
        check(reg && write && fill, "EtwPlus imports bound");
        if (reg && write && fill) {
            const char descriptor[64] = {};
            REGHANDLE a = 0, b = 0;
            check(reg(descriptor, &a) == ERROR_SUCCESS && reg(descriptor, &b) == ERROR_SUCCESS && a && b && a != b,
                  "providers receive distinct handles");
            check(reg(nullptr, &a) == ERROR_INVALID_PARAMETER, "null provider rejected");
            check(write(descriptor, descriptor, a, 0, nullptr) == ERROR_SUCCESS, "event accepted offline");
            check(write(descriptor, descriptor, a, 1, nullptr) == ERROR_INVALID_PARAMETER, "missing event data rejected");
            unsigned char field[16];
            for (auto& c : field) c = 0xcc;
            unsigned char scratch[64];
            fill(field, scratch, sizeof scratch);
            bool zero = true;
            for (auto c : field) zero = zero && c == 0;
            check(zero, "common fields are an empty payload offline");
        }
        check(resolve_title_import({"EtwPlus.dll", "EtxUploadNow", 0}) == nullptr, "unobserved EtwPlus export not invented");
    }
    check(resolve_kernelx({"kernelx.dll", "EventRegister", 0}) ==
              reinterpret_cast<void*>(GetProcAddress(LoadLibraryW(L"advapi32.dll"), "EventRegister")),
          "ETW provider API resolves to advapi32");
    check(resolve_title_import({"UCRTBASE.dll", "malloc", 0}) ==
              reinterpret_cast<void*>(GetProcAddress(LoadLibraryW(L"ucrtbase.dll"), "malloc")),
          "CRT import binds to System32 ucrtbase");
    check(resolve_title_import({"VCRUNTIME140.dll", "memcpy", 0}) != nullptr, "vcruntime140 bound");
    {
        using Create = HRESULT (WINAPI*)(void*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
                                         void**, D3D_FEATURE_LEVEL*, void**);
        const auto create = reinterpret_cast<Create>(resolve_title_import({"d3d11_x.dll", "D3D11CreateDevice", 0}));
        const HMODULE host_d3d11 = LoadLibraryW(L"d3d11.dll");
        check(create && reinterpret_cast<void*>(create) != reinterpret_cast<void*>(GetProcAddress(host_d3d11, "D3D11CreateDevice")),
              "D3D11.X bound to the D3D12 translation, not host D3D11");
        check(resolve_title_import({"d3d11_x.dll", "D3D11XCreateDeviceX", 0}) == nullptr, "unobserved D3D11.X export not invented");
        void* device = nullptr;
        void* context = nullptr;
        D3D_FEATURE_LEVEL level{};
        HRESULT hr = create ? create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, 7, &device, &level, &context) : E_FAIL;
        if (FAILED(hr) && create) hr = create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, 7, &device, &level, &context);
        check(SUCCEEDED(hr) && device && context, "D3D11.X device and immediate context created over D3D12");
        if (device && context) {
            using Query = HRESULT (STDMETHODCALLTYPE*)(void*, REFIID, void**);
            using Count = ULONG (STDMETHODCALLTYPE*)(void*);
            const auto vt = [](void* object) { return *static_cast<void* const* const*>(object); };
            const auto refcount = [](void* object) { return *reinterpret_cast<const ULONG*>(static_cast<char*>(object) + 12); };
            check(level >= D3D_FEATURE_LEVEL_10_0 && level <= D3D_FEATURE_LEVEL_11_1, "feature level capped at 11_1");
            check(*reinterpret_cast<const ULONG*>(static_cast<char*>(device) + 8) == 0, "IGraphicsUnknown flags field zero");
            check(refcount(device) == 2 && refcount(context) == 1, "title-visible refcounts (context holds the device)");
            constexpr GUID device_x{0x177700F9, 0x876A, 0x4436, {0xB3, 0x68, 0x36, 0xA6, 0x04, 0xF8, 0x2C, 0xEF}};
            constexpr GUID context_x{0x48800095, 0x7134, 0x4BE7, {0x91, 0x86, 0xB8, 0x6B, 0xEC, 0xB2, 0x64, 0x77}};
            void* same = nullptr;
            check(reinterpret_cast<Query>(vt(device)[0])(device, device_x, &same) == S_OK && same == device &&
                  refcount(device) == 3, "QueryInterface(ID3D11DeviceX) returns the device with a reference");
            check(reinterpret_cast<Count>(vt(device)[2])(device) == 2, "Release decrements the visible refcount");
            same = &same;
            check(reinterpret_cast<Query>(vt(device)[0])(device, context_x, &same) == E_NOINTERFACE && !same,
                  "unknown interface rejected with E_NOINTERFACE");
            check(reinterpret_cast<Query>(vt(context)[0])(context, context_x, &same) == S_OK && same == context,
                  "context answers ID3D11DeviceContextX");
            reinterpret_cast<Count>(vt(context)[2])(context);
            *reinterpret_cast<ULONG*>(static_cast<char*>(context) + 12) += 1; // Inline title AddRef.
            check(reinterpret_cast<Count>(vt(context)[2])(context) == 1, "Release honours an inline refcount change");

            static const char* trapped_interface = nullptr;
            static unsigned trapped_slot = 0;
            set_d3d11x_trap_handler([](const char* name, unsigned slot) -> std::uintptr_t {
                trapped_interface = name;
                trapped_slot = slot;
                return 0xdead;
            });
            using Any = std::uintptr_t (*)(void*);
            check(reinterpret_cast<Any>(vt(device)[7])(device) == 0xdead && trapped_slot == 7 &&
                  std::string(trapped_interface) == "ID3D11DeviceX", "unimplemented device slot reports interface and slot");
            check(reinterpret_cast<Any>(vt(context)[300])(context) == 0xdead && trapped_slot == 300 &&
                  std::string(trapped_interface) == "ID3D11DeviceContextX", "unimplemented context slot reports itself");
            set_d3d11x_trap_handler(nullptr);
            check(reinterpret_cast<Count>(vt(device)[2])(device) == 1, "title releases its device reference");
            check(reinterpret_cast<Count>(vt(context)[2])(context) == 0, "last context release frees context and device");
        }
        D3D_FEATURE_LEVEL probe{};
        check(create && create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, 7, nullptr, &probe, nullptr) == S_FALSE &&
              probe != D3D_FEATURE_LEVEL{}, "level probe without outputs returns S_FALSE");
        check(create && create(nullptr, D3D_DRIVER_TYPE_REFERENCE, nullptr, 0, nullptr, 0, 7, &device, nullptr, nullptr) ==
              DXGI_ERROR_UNSUPPORTED && !device, "unsupported driver type rejected");
    }

    {
        // WinRT title layer: XboxLiveConfiguration from a synthetic package manifest.
        check(SUCCEEDED(RoInitialize(RO_INIT_MULTITHREADED)), "WinRT initialized");
        wchar_t temp[MAX_PATH];
        GetTempPathW(MAX_PATH, temp);
        const std::wstring package = std::wstring(temp) + L"xone-era-package-" + std::to_wstring(GetCurrentProcessId());
        const std::wstring manifest = package + L"\\AppxManifest.xml";
        CreateDirectoryW(package.c_str(), nullptr);
        {
            std::ofstream out(manifest, std::ios::binary);
            out << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                   "<Package xmlns=\"http://schemas.microsoft.com/appx/2010/manifest\" "
                   "xmlns:mx=\"http://schemas.microsoft.com/appx/2013/xbox/manifest\">"
                   "<Extensions><mx:Extension Category=\"xbox.live\">"
                   "<mx:XboxLive TitleId=\"0000F00D\" PrimaryServiceConfigId=\"00000000-1111-2222-3333-44440000f00d\"/>"
                   "</mx:Extension></Extensions></Package>";
        }
        set_title_package_root(package);
        struct __declspec(uuid("00193376-6a84-4c67-84a5-3b8a8aaea712")) Statics : IInspectable {
            virtual HRESULT STDMETHODCALLTYPE get_TitleId(HSTRING*) = 0;
            virtual HRESULT STDMETHODCALLTYPE get_PrimaryServiceConfigId(HSTRING*) = 0;
            virtual HRESULT STDMETHODCALLTYPE get_SandboxId(HSTRING*) = 0;
        };
        const auto text = [](HSTRING s) {
            std::wstring v = WindowsGetStringRawBuffer(s, nullptr);
            WindowsDeleteString(s);
            return v;
        };
        const auto activate = [](const wchar_t* name, REFIID iid, void** out) {
            HSTRING id = nullptr;
            WindowsCreateString(name, static_cast<UINT32>(wcslen(name)), &id);
            const HRESULT hr = get_title_activation_factory(id, iid, out);
            WindowsDeleteString(id);
            return hr;
        };
        Statics* statics = nullptr;
        check(activate(L"Windows.Xbox.Services.XboxLiveConfiguration", __uuidof(Statics),
                       reinterpret_cast<void**>(&statics)) == S_OK && statics, "XboxLiveConfiguration served by title layer");
        if (statics) {
            HSTRING value = nullptr;
            check(statics->get_TitleId(&value) == S_OK && text(value) == L"0000F00D", "TitleId read from package manifest");
            check(statics->get_PrimaryServiceConfigId(&value) == S_OK &&
                  text(value) == L"00000000-1111-2222-3333-44440000f00d", "PrimaryServiceConfigId read from package manifest");
            check(statics->get_SandboxId(&value) == S_OK && text(value) == L"RETAIL", "retail sandbox");
            check(statics->GetRuntimeClassName(&value) == S_OK &&
                  text(value) == L"Windows.Xbox.Services.XboxLiveConfiguration", "runtime class name");
            IActivationFactory* factory = nullptr;
            check(statics->QueryInterface(__uuidof(IActivationFactory), reinterpret_cast<void**>(&factory)) == S_OK,
                  "factory answers IActivationFactory");
            IInspectable* instance = reinterpret_cast<IInspectable*>(1);
            check(factory && factory->ActivateInstance(&instance) == E_NOTIMPL && !instance, "static class has no instances");
            statics->Release();
            if (factory) factory->Release();
        }
        IActivationFactory* host = nullptr;
        check(activate(L"Windows.Foundation.Uri", __uuidof(IActivationFactory), reinterpret_cast<void**>(&host)) == S_OK && host,
              "host class forwarded to Windows");
        if (host) host->Release();
        void* missing = &missing;
        check(activate(L"Windows.Xbox.System.User", __uuidof(IActivationFactory), &missing) == REGDB_E_CLASSNOTREG && !missing,
              "unimplemented Xbox class keeps the real host error");
        DeleteFileW(manifest.c_str());
        RemoveDirectoryW(package.c_str());
        RoUninitialize();
    }
    std::cout << (failures ? "era_loader: FAILED\n" : "era_loader: all checks passed\n");
    return failures ? 1 : 0;
}
