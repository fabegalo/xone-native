// SPDX-License-Identifier: BSD-3-Clause
// d3d11_x.dll: D3D11.X device objects with the title-visible Xbox layout,
// translated onto Direct3D 12. Layout facts (IGraphicsUnknown data and IIDs)
// follow WinDurango's public interface definitions; the implementation is ours.
//
// Only the ABI used by the first title is described: combase 10.0.14393.2152
// up to (excluding) 10.0.19041.3453, where every object starts with
//   [vtable][ULONG DeviceIndex:3, PrivateDataPresent:1, Reserved:28][ULONG RefCount]
// Titles may touch RefCount inline, so it is a plain field, not hidden state.
// Every vtable slot not implemented yet traps and reports interface + slot.
#include "xboxone_native/era_loader.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <array>
#include <atomic>
#include <cstdio>
#include <utility>

namespace xboxone::era {
namespace {

constexpr GUID iid_graphics_unknown{0xACEEEA63, 0xE0A9, 0x4A1C, {0xBB, 0xEC, 0x71, 0xB2, 0xF4, 0x85, 0xF7, 0x58}};
constexpr GUID iid_unknown{0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr GUID iid_device{0xDB6F6DDB, 0xAC77, 0x4E88, {0x82, 0x53, 0x81, 0x9D, 0xF9, 0xBB, 0xF1, 0x40}};
constexpr GUID iid_device_x{0x177700F9, 0x876A, 0x4436, {0xB3, 0x68, 0x36, 0xA6, 0x04, 0xF8, 0x2C, 0xEF}};
constexpr GUID iid_context{0xC0BFA96C, 0xE089, 0x44FB, {0x8E, 0xAF, 0x26, 0xF8, 0x79, 0x61, 0x90, 0xDA}};
constexpr GUID iid_context_x{0x48800095, 0x7134, 0x4BE7, {0x91, 0x86, 0xB8, 0x6B, 0xEC, 0xB2, 0x64, 0x77}};

constexpr std::size_t vtable_slots = 512; // Above any D3D11.X interface; extra slots only trap.

struct GraphicsObject {
    const void* const* vtable;
    ULONG flags;    // DeviceIndex:3, PrivateDataPresent:1, Reserved:28 (zero here).
    ULONG refcount; // Title-visible.
};
static_assert(offsetof(GraphicsObject, flags) == 8 && offsetof(GraphicsObject, refcount) == 12);

struct Context;
struct Device : GraphicsObject {
    ID3D12Device* d3d12 = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
};
struct Context : GraphicsObject {
    Device* device = nullptr; // Owns one device reference.
};

std::atomic<D3D11XTrapHandler> trap_handler{nullptr};

[[noreturn]] void default_trap(const char* interface_name, unsigned slot) {
    char message[160];
    std::snprintf(message, sizeof message, "xone-era: D3D11.X %s vtable slot %u is not implemented\n",
                  interface_name, slot);
    OutputDebugStringA(message);
    std::fputs(message, stderr);
    std::fflush(stderr);
    __fastfail(FAST_FAIL_FATAL_APP_EXIT); // Never continue with a fabricated result.
}

template <unsigned Slot>
std::uintptr_t trap_device(void*) {
    if (auto handler = trap_handler.load()) return handler("ID3D11DeviceX", Slot);
    default_trap("ID3D11DeviceX", Slot);
}
template <unsigned Slot>
std::uintptr_t trap_context(void*) {
    if (auto handler = trap_handler.load()) return handler("ID3D11DeviceContextX", Slot);
    default_trap("ID3D11DeviceContextX", Slot);
}

template <std::size_t... I>
std::array<const void*, vtable_slots> device_traps(std::index_sequence<I...>) {
    return {reinterpret_cast<const void*>(&trap_device<static_cast<unsigned>(I)>)...};
}
template <std::size_t... I>
std::array<const void*, vtable_slots> context_traps(std::index_sequence<I...>) {
    return {reinterpret_cast<const void*>(&trap_context<static_cast<unsigned>(I)>)...};
}

ULONG add_ref(GraphicsObject* self) { return InterlockedIncrement(reinterpret_cast<volatile LONG*>(&self->refcount)); }

HRESULT query(GraphicsObject* self, REFIID riid, void** out, std::initializer_list<GUID> accepted) {
    if (!out) return E_POINTER;
    *out = nullptr;
    for (const auto& iid : accepted)
        if (IsEqualGUID(riid, iid)) {
            add_ref(self);
            *out = self;
            return S_OK;
        }
    return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE device_query(Device* self, REFIID riid, void** out) {
    return query(self, riid, out, {iid_unknown, iid_graphics_unknown, iid_device, iid_device_x});
}
ULONG STDMETHODCALLTYPE device_add_ref(Device* self) { return add_ref(self); }
ULONG STDMETHODCALLTYPE device_release(Device* self) {
    const ULONG left = InterlockedDecrement(reinterpret_cast<volatile LONG*>(&self->refcount));
    if (left == 0) {
        if (self->queue) self->queue->Release();
        if (self->d3d12) self->d3d12->Release();
        delete self;
    }
    return left;
}

HRESULT STDMETHODCALLTYPE context_query(Context* self, REFIID riid, void** out) {
    return query(self, riid, out, {iid_unknown, iid_graphics_unknown, iid_context, iid_context_x});
}
ULONG STDMETHODCALLTYPE context_add_ref(Context* self) { return add_ref(self); }
ULONG STDMETHODCALLTYPE context_release(Context* self) {
    const ULONG left = InterlockedDecrement(reinterpret_cast<volatile LONG*>(&self->refcount));
    if (left == 0) {
        device_release(self->device);
        delete self;
    }
    return left;
}

const std::array<const void*, vtable_slots>& device_vtable() {
    static const auto table = [] {
        auto t = device_traps(std::make_index_sequence<vtable_slots>{});
        t[0] = reinterpret_cast<const void*>(&device_query);
        t[1] = reinterpret_cast<const void*>(&device_add_ref);
        t[2] = reinterpret_cast<const void*>(&device_release);
        return t;
    }();
    return table;
}
const std::array<const void*, vtable_slots>& context_vtable() {
    static const auto table = [] {
        auto t = context_traps(std::make_index_sequence<vtable_slots>{});
        t[0] = reinterpret_cast<const void*>(&context_query);
        t[1] = reinterpret_cast<const void*>(&context_add_ref);
        t[2] = reinterpret_cast<const void*>(&context_release);
        return t;
    }();
    return table;
}

// Highest requested level the D3D12 device supports, capped at 11_1 (the
// Xbox One D3D11.X level); the default list matches D3D11's when none is given.
bool select_level(ID3D12Device* device, const D3D_FEATURE_LEVEL* levels, UINT count, D3D_FEATURE_LEVEL& out) {
    static constexpr D3D_FEATURE_LEVEL defaults[]{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                                  D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    if (!levels) { levels = defaults; count = static_cast<UINT>(std::size(defaults)); }
    for (UINT i = 0; i < count; ++i) {
        if (levels[i] > D3D_FEATURE_LEVEL_11_1) continue;
        D3D12_FEATURE_DATA_FEATURE_LEVELS query{1, &levels[i], D3D_FEATURE_LEVEL_9_1};
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &query, sizeof query)) &&
            query.MaxSupportedFeatureLevel >= levels[i]) {
            out = levels[i];
            return true;
        }
    }
    return false;
}

HRESULT create_d3d12(D3D_DRIVER_TYPE type, ID3D12Device** out) {
    if (type == D3D_DRIVER_TYPE_HARDWARE || type == D3D_DRIVER_TYPE_UNKNOWN)
        return D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(out));
    if (type != D3D_DRIVER_TYPE_WARP) return DXGI_ERROR_UNSUPPORTED;
    IDXGIFactory4* factory = nullptr;
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return hr;
    IDXGIAdapter* warp = nullptr;
    hr = factory->EnumWarpAdapter(IID_PPV_ARGS(&warp));
    factory->Release();
    if (FAILED(hr)) return hr;
    hr = D3D12CreateDevice(warp, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(out));
    warp->Release();
    return hr;
}

HRESULT WINAPI D3D11CreateDevice(void* adapter, D3D_DRIVER_TYPE type, HMODULE software, UINT, const D3D_FEATURE_LEVEL* levels,
                                 UINT level_count, UINT, void** device_out, D3D_FEATURE_LEVEL* level_out, void** context_out) {
    if (device_out) *device_out = nullptr;
    if (context_out) *context_out = nullptr;
    if (adapter || software || (levels && level_count == 0)) return E_INVALIDARG; // Not observed on the title path.
    ID3D12Device* d3d12 = nullptr;
    HRESULT hr = create_d3d12(type, &d3d12);
    if (FAILED(hr)) return hr;
    D3D_FEATURE_LEVEL level{};
    if (!select_level(d3d12, levels, levels ? level_count : 0, level)) {
        d3d12->Release();
        return E_INVALIDARG;
    }
    if (level_out) *level_out = level;
    if (!device_out && !context_out) { d3d12->Release(); return S_FALSE; }

    D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
    ID3D12CommandQueue* queue = nullptr;
    hr = d3d12->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue));
    if (FAILED(hr)) { d3d12->Release(); return hr; }

    auto* device = new (std::nothrow) Device;
    if (!device) { queue->Release(); d3d12->Release(); return E_OUTOFMEMORY; }
    device->vtable = device_vtable().data();
    device->flags = 0;
    device->refcount = 1;
    device->d3d12 = d3d12;
    device->queue = queue;
    device->level = level;
    if (context_out) {
        auto* context = new (std::nothrow) Context;
        if (!context) { device_release(device); return E_OUTOFMEMORY; }
        context->vtable = context_vtable().data();
        context->flags = 0;
        context->refcount = 1;
        context->device = device;
        add_ref(device);
        *context_out = context;
    }
    if (device_out) *device_out = device;
    else device_release(device);
    return S_OK;
}

} // namespace

void set_d3d11x_trap_handler(D3D11XTrapHandler handler) { trap_handler.store(handler); }

void* resolve_d3d11x(const ImportRequest& request) {
    if (!same_library(request.library, "d3d11_x.dll")) return nullptr;
    if (request.name == "D3D11CreateDevice") return reinterpret_cast<void*>(&D3D11CreateDevice);
    return nullptr;
}

} // namespace xboxone::era
