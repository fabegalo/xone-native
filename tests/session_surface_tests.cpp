// SPDX-License-Identifier: BSD-3-Clause
// Synthetic integration: the production supervisor, CoreWindow, session channel
// and GPU compositor. No original system UI or gameplay is claimed.
#include "xboxone_native/session_supervisor.h"
#include "xboxone_native/era_loader.h"
#include "era_session.h"
#include "era_display.h"
#include "era_winrt_classes.h"
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <activation.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

using namespace xboxone;
using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
namespace {
int failures = 0;
void check(bool ok, const char* message) {
    if (!ok) { ++failures; std::cerr << "FAIL: " << message << std::endl; }
}
template<class R, class... A> R call(void* object, unsigned slot, A... args) {
    return reinterpret_cast<R(STDMETHODCALLTYPE*)(void*, A...)>((*static_cast<void***>(object))[slot])(object, args...);
}
void pump() {
    MSG message{};
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
}
template<class F> bool until(F ready) {
    const auto end = GetTickCount64() + 6000;
    do { pump(); if (ready()) return true; Sleep(5); } while (GetTickCount64() < end);
    return false;
}

// Every submission retires before reusing its upload/descriptor/command storage.
struct Gpu {
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Resource> output, readback;
    ComPtr<ID3D12DescriptorHeap> target;
    ComPtr<ID3D12Fence> fence;
    era::GuideComposition guide;
    UINT64 sequence = 0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    bool initialize(bool warp) {
        ComPtr<IDXGIFactory4> factory;
        ComPtr<IDXGIAdapter> adapter;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return false;
        if (warp && FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)))) return false;
        if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) return false;
        D3D12_COMMAND_QUEUE_DESC q{};
        if (FAILED(device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue))) ||
            FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
            FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr, IID_PPV_ARGS(&list))) ||
            FAILED(list->Close()) || FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) return false;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; desc.Width = desc.Height = 4;
        desc.DepthOrArraySize = desc.MipLevels = 1; desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1; desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&output)))) return false;
        UINT64 bytes = 0;
        device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, &bytes);
        desc = {}; desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width = bytes;
        desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1; desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; heap.Type = D3D12_HEAP_TYPE_READBACK;
        const D3D12_DESCRIPTOR_HEAP_DESC rtv{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readback))) ||
            FAILED(device->CreateDescriptorHeap(&rtv, IID_PPV_ARGS(&target)))) return false;
        device->CreateRenderTargetView(output.Get(), nullptr, target->GetCPUDescriptorHandleForHeapStart());
        return true;
    }
    bool verify(bool overlay) {
        if (FAILED(allocator->Reset()) || FAILED(list->Reset(allocator.Get(), nullptr))) return false;
        const float blue[]{0, 0, 1, 1};
        list->ClearRenderTargetView(target->GetCPUDescriptorHandleForHeapStart(), blue, 0, nullptr);
        if (FAILED(guide.record(device.Get(), list.Get(), output.Get()))) return false;
        D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition = {output.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
            D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE};
        list->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION from{}; from.pResource = output.Get();
        D3D12_TEXTURE_COPY_LOCATION to{}; to.pResource = readback.Get();
        to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; to.PlacedFootprint = footprint;
        list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter); list->ResourceBarrier(1, &barrier);
        if (FAILED(list->Close())) return false;
        ID3D12CommandList* commands[]{list.Get()}; queue->ExecuteCommandLists(1, commands);
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        const bool completed = event && SUCCEEDED(queue->Signal(fence.Get(), ++sequence)) &&
            SUCCEEDED(fence->SetEventOnCompletion(sequence, event)) && WaitForSingleObject(event, 5000) == WAIT_OBJECT_0;
        if (event) CloseHandle(event);
        if (!completed) return false;
        void* data = nullptr;
        if (FAILED(readback->Map(0, nullptr, &data))) return false;
        const auto* p = static_cast<const unsigned char*>(data) + footprint.Offset;
        const bool correct = overlay ? p[0] == 255 && p[1] == 0 && p[2] == 0 &&
            p[4] == 0 && p[5] == 128 && p[6] == 127 && p[8] == 0 && p[9] == 0 && p[10] == 255 :
            p[0] == 0 && p[1] == 0 && p[2] == 255 && p[4] == 0 && p[5] == 0 && p[6] == 255;
        readback->Unmap(0, nullptr);
        return correct;
    }
};
bool warp = false;
struct View : IInspectable {
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override { *out = this; return S_OK; }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE GetIids(ULONG*, IID**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel*) override { return E_NOTIMPL; }
    virtual HRESULT STDMETHODCALLTYPE Initialize(IInspectable*) { return S_OK; }
    virtual HRESULT STDMETHODCALLTYPE SetWindow(IInspectable* window) {
        check(GetParent(era::core_window_hwnd(window)) == era::session_parent(), "CoreWindow belongs to the persistent session frame");
        return call<HRESULT>(window, 11);
    }
    virtual HRESULT STDMETHODCALLTYPE Load(HSTRING) { return S_OK; }
    virtual HRESULT STDMETHODCALLTYPE Run() {
        Gpu gpu;
        if (!gpu.initialize(warp)) { check(false, "GPU initialization"); return E_FAIL; }
        check(gpu.verify(false), "title pixels unchanged while Guide is closed");
        std::cout << "SESSION_READY" << std::endl;
        std::vector<std::uint32_t> frame; UINT width = 0, height = 0;
        check(until([&] { return era::session_guide_frame(frame, width, height); }), "Guide frame arrived from the supervisor");
        check(!era::session_title_input(), "Guide owns input instead of the title");
        check(gpu.verify(true), "Guide opaque, translucent and transparent pixels compose over the title on GPU");
        std::cout << "GUIDE_COMPOSED" << std::endl;
        check(until([&] { return !era::session_guide_frame(frame, width, height); }), "Guide closed");
        check(gpu.verify(false), "closing Guide restores identical title pixels");
        std::cout << "TITLE_RESUMED" << std::endl;
        check(until([&] {
            session::SurfaceChannel observer;
            session::SurfaceMode mode{};
            return observer.connect(GetCurrentProcessId(), era::session_parent(), false) &&
                observer.read(mode, width, height) && mode == session::SurfaceMode::system_ui;
        }), "Home became the active surface");
        check(!era::session_title_input(), "Home blocks game input");
        check(gpu.verify(false), "Home never modifies title pixels");
        std::cout << "title entry returned" << std::endl;
        return failures ? E_FAIL : S_OK;
    }
    virtual HRESULT STDMETHODCALLTYPE Uninitialize() { return S_OK; }
} view;
// IFrameworkViewSource has CreateView directly after IInspectable.
struct Factory : IInspectable {
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override { *out = this; return S_OK; }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE GetIids(ULONG*, IID**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel*) override { return E_NOTIMPL; }
    virtual HRESULT STDMETHODCALLTYPE CreateView(IInspectable** out) { *out = &view; return S_OK; }
} factory;
std::string contents(const fs::path& path) {
    std::ifstream in(path); return {std::istreambuf_iterator<char>(in), {}};
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc > 2 && std::wstring(argv[1]) == L"run") {
        HWND parent = nullptr; std::wstring storage;
        for (int i = 3; i < argc; ++i) {
            if (std::wstring(argv[i]) == L"--session-window" && i + 1 < argc)
                parent = reinterpret_cast<HWND>(std::wcstoull(argv[++i], nullptr, 10));
            else if (std::wstring(argv[i]) == L"--storage" && i + 1 < argc) storage = argv[++i];
        }
        warp = fs::path(argv[2]).parent_path().filename() == L"warp";
        if (!era::bind_session_surface(parent)) return 2;
        era::set_title_package_root(fs::path(argv[2]).parent_path().wstring());
        era::set_title_storage_root(storage);
        constexpr GUID iid{0x0aacf7a4, 0x5e1d, 0x49df, {0x80, 0x34, 0xfb, 0x6a, 0x68, 0xbc, 0x5e, 0xd1}};
        void* app = nullptr;
        if (FAILED(era::core_application_factory(iid, &app))) return 3;
        const auto hr = call<HRESULT>(app, 13, static_cast<IUnknown*>(&factory));
        return FAILED(hr) || failures ? 1 : 0;
    }
    if (argc != 4) return 2;
    const auto scratch = fs::temp_directory_path() / ("xone-session-surface-" + std::to_string(GetCurrentProcessId()));
    fs::create_directories(scratch);
    struct Cleanup { fs::path p; ~Cleanup() { std::error_code ec; fs::remove_all(p, ec); } } cleanup{scratch};
    HWND parent = CreateWindowExW(0, L"STATIC", L"xone session regression", WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        0, 0, 320, 240, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!parent) return 3;
    ShowWindow(parent, SW_SHOWNOACTIVATE);
    wchar_t self[32768]{}; GetModuleFileNameW(nullptr, self, 32768);
    for (const auto backend : {L"hardware", L"warp"}) {
        const auto package = scratch / backend;
        fs::create_directories(package / "storage");
        fs::copy_file(argv[1], package / "AppxManifest.xml");
        const unsigned char profile[32]{'X','O','N','E','U','S','R','1',1,0,0,0,0,0,0,0,0x11,0x22};
        std::ofstream(package / "storage" / ".xone-local-profile", std::ios::binary).write(reinterpret_cast<const char*>(profile), 32);
        auto host = session::create_host();
        session::Request request{session::Mode::run, self, package / "AppxManifest.xml", package / "storage", package / "run.log"};
        request.session_window = reinterpret_cast<std::uintptr_t>(parent);
        std::string error;
        check(host->start(request, error), "start supervised UI fixture");
        std::vector<std::uint32_t> pixels(16, 0); pixels[0] = 0xFFFF0000; pixels[1] = 0x80008000;
        check(!host->present_system_ui({session::SurfaceMode::guide, 4, 5, pixels}), "reject malformed frame dimensions");
        check(until([&] { return contents(request.log).find("SESSION_READY") != std::string::npos; }), "child reached real CoreWindow and GPU");
        HWND child = FindWindowExW(parent, nullptr, L"xone-era-core-window", nullptr);
        check(child != nullptr, "title uses the session child window");
        SetWindowPos(parent, nullptr, 0, 0, 480, 270, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        session::Result pending;
        host->poll(pending);
        check(until([&] {
            RECT frame{}, title{}; GetClientRect(parent, &frame); GetClientRect(child, &title);
            return frame.right == title.right && frame.bottom == title.bottom;
        }), "title follows persistent window resize");
        check(host->present_system_ui({session::SurfaceMode::guide, 4, 4, pixels}), "publish Guide");
        check(until([&] { return contents(request.log).find("GUIDE_COMPOSED") != std::string::npos; }), "child composed Guide");
        check(host->present_system_ui({session::SurfaceMode::title, 4, 4, pixels}) && host->focus_title(), "return to the same title window");
        check(until([&] { return contents(request.log).find("TITLE_RESUMED") != std::string::npos; }), "title resumed without a new process");
        check(host->present_system_ui({session::SurfaceMode::system_ui, 4, 4, pixels}), "select Home");
        check(!IsWindowVisible(child), "Home hides the title surface");
        session::Result result;
        const bool ended = until([&] { return host->poll(result); });
        check(ended && result.exit_code == 0, "session ends with real child outcome");
        if (!ended || result.exit_code) std::cerr << contents(request.log);
        std::wcout << L"session composition checked: " << backend << std::endl;
    }
    // The actual ERA CLI must accept the same surface binding; it is not a
    // separate test-only execution route. This owned title has no graphics.
    auto real = session::create_host();
    session::Request request{session::Mode::run, fs::absolute(argv[2]), fs::absolute(argv[3]),
        scratch / "storage", scratch / "real-era.log"};
    request.session_window = reinterpret_cast<std::uintptr_t>(parent);
    std::string error;
    check(real->start(request, error), "start actual ERA with session surface");
    session::Result result;
    check(until([&] { return real->poll(result); }) && result.normal, "actual ERA accepts the session binding and reports title return");
    check(!era::bind_session_surface(parent), "a process without a supervised channel cannot attach to a session");
    DestroyWindow(parent);
    return failures ? 1 : 0;
}
