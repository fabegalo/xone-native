// SPDX-License-Identifier: BSD-3-Clause
#include <winsock2.h>
#include "xboxone_native/era_loader.h"
#include "../src/era_winrt_classes.h"
#include "../src/era_winrt_object.h"
#include "../src/era_memory.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <evntprov.h>
#include <d3dcommon.h>
#include <d3dcompiler.h>
#include <d3d12.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <activation.h>
#include <roapi.h>
#include <winstring.h>

#include <fstream>
#include <filesystem>
#include <string>
#include <iostream>
#include <iterator>
#include <utility>
#include <thread>
#include <cstring>
#include <array>
#include <algorithm>

namespace {
int failures = 0;
SIZE_T hook_size = 0;
ULONGLONG hook_attributes = 0;
void* hook_pointer = nullptr;
unsigned hook_frees = 0;
using GraphicsAllocate = void* (WINAPI*)(SIZE_T, ULONGLONG);
using GraphicsFree = void (WINAPI*)(void*, ULONGLONG);
GraphicsAllocate graphics_default_allocate = nullptr;
GraphicsFree graphics_default_free = nullptr;
unsigned graphics_hook_allocations = 0, graphics_hook_frees = 0, graphics_hook_wrong_allocator = 0;
bool graphics_force_header = false;
void* WINAPI graphics_allocate_hook(SIZE_T size, ULONGLONG attributes) {
    ++graphics_hook_allocations;
    const bool system = ((attributes >> 16) & 255) == 0x80;
    if (!system) ++graphics_hook_wrong_allocator;
    const bool header = graphics_force_header || !system;
    auto* memory = static_cast<char*>(graphics_default_allocate(size + (header ? 16 : 0), attributes));
    if (memory && header) std::memset(memory, 0x5a, 16);
    return memory ? memory + (header ? 16 : 0) : nullptr;
}
void WINAPI graphics_free_hook(void* memory, ULONGLONG attributes) {
    ++graphics_hook_frees;
    const bool system = ((attributes >> 16) & 255) == 0x80;
    graphics_default_free(static_cast<char*>(memory) - (graphics_force_header || !system ? 16 : 0), attributes);
}
void* WINAPI allocate_hook(SIZE_T size, ULONGLONG attributes) {
    hook_size = size; hook_attributes = attributes;
    return &hook_size;
}
void WINAPI release_hook(void* pointer, ULONGLONG attributes) {
    hook_pointer = pointer; hook_attributes = attributes; ++hook_frees;
}
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
    if (argc == 4 && (std::string(argv[1]) == "--xg-layout" || std::string(argv[1]) == "--xg-graphics" || std::string(argv[1]) == "--xg-msaa")) {
        const bool graphics = std::string(argv[1]) == "--xg-graphics";
        set_stop_on_missing_activation(true);
        HRESULT hr = bind_d3d11x_abi(std::filesystem::path(argv[2]).wstring());
        if (SUCCEEDED(hr)) hr = bind_xg_module(std::filesystem::path(argv[3]).wstring());
        if (SUCCEEDED(hr)) hr = initialize_xg_module();
        TitleTextureLayout layout;
        if (std::string(argv[1]) == "--xg-msaa") {
            if (SUCCEEDED(hr)) hr = create_xg_texture_layout(32, 16, 1, 1, DXGI_FORMAT_R11G11B10_FLOAT, UINT_MAX, layout,
                D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 4);
            std::cout << "original XG MSAA4 layout hr=0x" << std::hex << hr << std::dec << " bytes=" << layout.bytes << " alignment=" << layout.alignment << '\n';
            if (FAILED(hr)) return 1;
            std::vector<unsigned char> tiled(layout.bytes + 32, 0x5a), input(32 * 16 * 4 * 4), output(input.size(), 0);
            for (UINT i = 0; i < input.size(); ++i) input[i] = static_cast<unsigned char>(i * 13 + i / 4 + 7);
            hr = layout.copy(layout.context.get(), true, input.data(), tiled.data() + 16, 0, 512, 8192);
            if (SUCCEEDED(hr)) hr = layout.copy(layout.context.get(), false, output.data(), tiled.data() + 16, 0, 512, 8192);
            std::cout << "original XG MSAA4 copy hr=0x" << std::hex << hr << std::dec << " equal=" << (input == output) << '\n';
            check(SUCCEEDED(hr) && input == output, "MSAA CPU layout preserves four independent samples without resolve");
            check(std::all_of(tiled.begin(), tiled.begin() + 16, [](auto x) { return x == 0x5a; }) &&
                std::all_of(tiled.end() - 16, tiled.end(), [](auto x) { return x == 0x5a; }), "MSAA copies preserve canonical guards");
            hr = create_xg_texture_layout(32, 16, 1, 1, DXGI_FORMAT_R32G8X24_TYPELESS, 1, layout,
                D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE, 4);
            std::cout << "original XG depth/stencil MSAA4 layout hr=0x" << std::hex << hr << std::dec << " bytes=" << layout.bytes << " alignment=" << layout.alignment << '\n';
            check(hr == DXGI_ERROR_UNSUPPORTED && !layout.context && !layout.bytes,
                "three-plane depth/stencil layout is explicitly refused until every plane is translated");
            hr = create_xg_texture_layout(1920, 1080, 1, 1, DXGI_FORMAT_R32G8X24_TYPELESS, 1, layout,
                D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE, 4);
            check(hr == DXGI_ERROR_UNSUPPORTED && !layout.context && !layout.bytes,
                "observed full-size FH2 depth descriptor preserves the same explicit plane boundary");
            return failures ? 1 : 0;
        }
        if (SUCCEEDED(hr)) hr = create_xg_texture_layout(64, 64, 7, 1, DXGI_FORMAT_BC7_UNORM_SRGB, 14, layout);
        std::cout << "original XG BC7 layout hr=0x" << std::hex << hr << std::dec << " bytes=" << layout.bytes << " alignment=" << layout.alignment << '\n';
        if (FAILED(hr)) return 1;
        std::vector<std::uint8_t> tiled(layout.bytes + 32, 0x5a);
        std::array<std::vector<std::uint8_t>, 7> linear;
        for (UINT mip = 0; mip < 7; ++mip) {
            const UINT blocks = ((std::max)(1u, 64u >> mip) + 3) / 4;
            linear[mip].resize(blocks * blocks * 16);
            for (UINT i = 0; i < linear[mip].size(); ++i) linear[mip][i] = static_cast<std::uint8_t>(mip * 31 + i * 13 + 7);
            hr = layout.copy(layout.context.get(), true, linear[mip].data(), tiled.data() + 16, mip, blocks * 16, blocks * blocks * 16);
            check(SUCCEEDED(hr), "original XG tiles an owned BC7 mip");
        }
        for (UINT mip = 0; mip < 7; ++mip) {
            const UINT blocks = ((std::max)(1u, 64u >> mip) + 3) / 4;
            std::vector<std::uint8_t> actual(linear[mip].size(), 0);
            hr = layout.copy(layout.context.get(), false, actual.data(), tiled.data() + 16, mip, blocks * 16, blocks * blocks * 16);
            check(SUCCEEDED(hr) && actual == linear[mip], "original XG roundtrip preserves each owned mip's compressed blocks");
        }
        check(std::all_of(tiled.begin(), tiled.begin() + 16, [](auto x) { return x == 0x5a; }) &&
            std::all_of(tiled.end() - 16, tiled.end(), [](auto x) { return x == 0x5a; }), "original XG copies preserve canonical range guards");
        check(layout.copy(layout.context.get(), false, linear[0].data(), tiled.data() + 16, 7, 256, 4096) == E_INVALIDARG &&
            layout.copy(layout.context.get(), false, linear[0].data(), tiled.data() + 16, 0, 1, 4096) == E_INVALIDARG,
            "layout adapter rejects invalid subresources and undersized linear pitches");
        std::cout << "original XG CPU roundtrip " << (failures ? "FAILED" : "passed") << '\n';
        if (graphics && !failures) {
            set_stop_on_missing_activation(false);
            struct Parameters { UINT version, flags; void* tess; void* factors; UINT affinity, de_ring, ce_ring, de_segment, ce_segment; } parameters{};
            parameters.version = 0x1000b; parameters.flags = 0x4000000;
            using CreateDevice = HRESULT (WINAPI*)(const Parameters*, void**, void**);
            const auto create_device = reinterpret_cast<CreateDevice>(resolve_d3d11x({"d3d11_x.dll", "D3D11XCreateDeviceX", 0}));
            void* device = nullptr; void* context = nullptr;
            if (!create_device || FAILED(create_device(&parameters, &device, &context))) return 1;
            const auto vt = [](void* object) { return *static_cast<void***>(object); };
            using Count = ULONG (STDMETHODCALLTYPE*)(void*);
            using Allocate = void* (WINAPI*)(SIZE_T, UINT64);
            using Free = void (WINAPI*)(void*, UINT64);
            const auto allocate = reinterpret_cast<Allocate>(resolve_kernelx({"kernelx.dll", "XMemAllocDefault", 0}));
            const auto free_memory = reinterpret_cast<Free>(resolve_kernelx({"kernelx.dll", "XMemFreeDefault", 0}));
            constexpr UINT64 attributes = 0x64804000;
            void* root = allocate(layout.bytes + layout.alignment, attributes);
            if (!root) return 1;
            const auto aligned = (reinterpret_cast<std::uintptr_t>(root) + layout.alignment - 1) & ~(layout.alignment - 1);
            void* canonical = reinterpret_cast<void*>(aligned);
            struct Desc { D3D11_TEXTURE2D_DESC standard{}; UINT extension[2]{}; } desc;
            desc.standard = {64, 64, 7, 1, DXGI_FORMAT_BC7_UNORM_SRGB, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0};
            using CreateTexture = HRESULT (STDMETHODCALLTYPE*)(void*, const Desc*, UINT, UINT, void*, void**);
            void* texture = nullptr;
            hr = reinterpret_cast<CreateTexture>(vt(device)[64])(device, &desc, 14, 0, canonical, &texture);
            check(hr == S_OK && texture, "placed texture factory binds original XG storage");
            if (texture) {
                using GetDesc = void (STDMETHODCALLTYPE*)(void*, Desc*);
                struct { Desc desc{}; UINT guard = 0xaabbccdd; } actual;
                reinterpret_cast<GetDesc>(vt(texture)[12])(texture, &actual.desc);
                check(actual.desc.standard.MipLevels == 7 && actual.desc.standard.Format == desc.standard.Format &&
                    actual.guard == 0xaabbccdd, "native texture GetDesc preserves descriptor and fifty-two-byte extent");
                using CreateSrv = HRESULT (STDMETHODCALLTYPE*)(void*, void*, const D3D11_SHADER_RESOURCE_VIEW_DESC*, void**);
                const auto create_srv = reinterpret_cast<CreateSrv>(vt(device)[7]);
                void* view = nullptr;
                D3D11_SHADER_RESOURCE_VIEW_DESC partial{};
                partial.Format = desc.standard.Format; partial.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                partial.Texture2D = {1, 6};
                check(create_srv(device, texture, &partial, &view) == DXGI_ERROR_UNSUPPORTED && !view,
                      "untranslated partial texture views remain explicit");
                check(create_srv(device, texture, nullptr, nullptr) == S_FALSE, "default texture view validates without ownership changes");
                check(create_srv(device, texture, nullptr, &view) == S_OK && view, "default texture SRV owns a native descriptor");
                if (view) {
                    std::array<UINT32, 8> srd{};
                    std::memcpy(srd.data(), static_cast<char*>(view) + 48, sizeof srd);
                    check(srd[0] == (aligned >> 8) && ((srd[3] >> 12) & 15) == 0 && ((srd[3] >> 16) & 15) == 6,
                          "guest texture SRV preserves placement address and complete mip range");
                    using GetSrvDesc = void (STDMETHODCALLTYPE*)(void*, D3D11_SHADER_RESOURCE_VIEW_DESC*);
                    struct { D3D11_SHADER_RESOURCE_VIEW_DESC desc{}; UINT guard = 0xaabbccdd; } view_desc;
                    reinterpret_cast<GetSrvDesc>(vt(view)[9])(view, &view_desc.desc);
                    check(view_desc.desc.Format == desc.standard.Format && view_desc.desc.Texture2D.MipLevels == 7 &&
                        view_desc.guard == 0xaabbccdd, "texture view GetDesc returns the effective range and output extent");
                    check(reinterpret_cast<Count>(vt(texture)[2])(texture) == 1, "view retains texture after its caller releases it");
                    texture = nullptr;
                    reinterpret_cast<Count>(vt(view)[2])(view);
                }
                if (texture) reinterpret_cast<Count>(vt(texture)[2])(texture);
            }
            MEMORY_BASIC_INFORMATION memory{};
            VirtualQuery(root, &memory, sizeof memory);
            check(memory.State == MEM_COMMIT, "placed texture release preserves borrowed canonical storage");
            free_memory(root, attributes);
            struct VolumeDesc { D3D11_TEXTURE3D_DESC standard; std::array<UINT, 2> extension{}; } volume_desc{};
            static_assert(sizeof(VolumeDesc) == 44);
            volume_desc.standard = {16, 16, 16, 1, DXGI_FORMAT_R10G10B10A2_UNORM, D3D11_USAGE_DEFAULT,
                D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, 0, 0};
            using CreateVolume = HRESULT (STDMETHODCALLTYPE*)(void*, const VolumeDesc*, const D3D11_SUBRESOURCE_DATA*, void**);
            const auto create_volume = reinterpret_cast<CreateVolume>(vt(device)[6]);
            texture = nullptr;
            check(create_volume(device, &volume_desc, nullptr, nullptr) == S_FALSE,
                  "volume descriptor validation does not acquire ownership");
            check(create_volume(device, &volume_desc, nullptr, &texture) == S_OK && texture,
                  "observed volume factory owns original XG storage and a native UAV-capable texture");
            if (texture) {
                using GetVolumeDesc = void (STDMETHODCALLTYPE*)(void*, VolumeDesc*);
                struct { VolumeDesc desc{}; UINT guard = 0xaabbccdd; } result;
                reinterpret_cast<GetVolumeDesc>(vt(texture)[12])(texture, &result.desc);
                check(!std::memcmp(&result.desc, &volume_desc, sizeof volume_desc) && result.guard == 0xaabbccdd,
                      "volume GetDesc preserves the native forty-four-byte ABI");
                using GetType = void (STDMETHODCALLTYPE*)(void*, D3D11_RESOURCE_DIMENSION*);
                D3D11_RESOURCE_DIMENSION type{};
                reinterpret_cast<GetType>(vt(texture)[8])(texture, &type);
                check(type == D3D11_RESOURCE_DIMENSION_TEXTURE3D, "volume retains its real resource dimension");
                using CreateSrv = HRESULT (STDMETHODCALLTYPE*)(void*, void*, const D3D11_SHADER_RESOURCE_VIEW_DESC*, void**);
                void* view = nullptr;
                check(reinterpret_cast<CreateSrv>(vt(device)[7])(device, texture, nullptr, &view) == S_OK && view,
                      "default volume SRV owns a native texture3D descriptor");
                if (view) {
                    D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
                    using GetSrvDesc = void (STDMETHODCALLTYPE*)(void*, D3D11_SHADER_RESOURCE_VIEW_DESC*);
                    reinterpret_cast<GetSrvDesc>(vt(view)[9])(view, &desc);
                    check(desc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE3D && desc.Texture3D.MipLevels == 1 &&
                        desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM, "volume SRV preserves dimension, format and mip range");
                    check(reinterpret_cast<Count>(vt(texture)[2])(texture) == 1, "volume view retains its storage after caller release");
                    texture = nullptr;
                    reinterpret_cast<Count>(vt(view)[2])(view);
                }
                if (texture) reinterpret_cast<Count>(vt(texture)[2])(texture);
            }
            volume_desc.standard.MipLevels = 5; volume_desc.standard.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            volume_desc.standard.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            TitleTextureLayout volume_layout;
            hr = create_xg_texture3d_layout(16, 16, 16, 5, volume_desc.standard.Format,
                volume_desc.standard.BindFlags, volume_layout, 13);
            check(hr == S_OK, "placement volume uses its specified original tile mode");
            if (SUCCEEDED(hr)) {
                auto* storage = static_cast<unsigned char*>(allocate(volume_layout.bytes + volume_layout.alignment + 32, attributes));
                if (!storage) return 1;
                std::memset(storage, 0x5a, volume_layout.bytes + volume_layout.alignment + 32);
                const auto placed = (reinterpret_cast<std::uintptr_t>(storage) + 16 + volume_layout.alignment - 1) & ~(volume_layout.alignment - 1);
                using PlaceVolume = HRESULT (STDMETHODCALLTYPE*)(void*, const VolumeDesc*, UINT, UINT, void*, void**);
                const auto place_volume = reinterpret_cast<PlaceVolume>(vt(device)[65]);
                texture = nullptr;
                check(place_volume(device, &volume_desc, 13, 1, reinterpret_cast<void*>(placed), &texture) == DXGI_ERROR_UNSUPPORTED && !texture,
                      "unknown volume placement flags remain explicit");
                check(place_volume(device, &volume_desc, 13, 0, reinterpret_cast<void*>(placed), &texture) == S_OK && texture,
                      "native slot sixty-five pins borrowed tiled volume storage");
                if (texture) reinterpret_cast<Count>(vt(texture)[2])(texture);
                MEMORY_BASIC_INFORMATION retained{};
                VirtualQuery(storage, &retained, sizeof retained);
                check(retained.State == MEM_COMMIT &&
                    std::all_of(storage, storage + volume_layout.bytes + volume_layout.alignment + 32, [](auto x) { return x == 0x5a; }),
                    "placement volume retirement preserves all borrowed bytes and ownership");
                free_memory(storage, attributes);
            }
            for (bool multisample : {false, true}) {
                Desc target{};
                target.standard = {32, 16, 1, 1, DXGI_FORMAT_R10G10B10A2_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                    D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0};
                if (multisample) {
                    target.standard.SampleDesc.Count = 4; target.standard.Format = DXGI_FORMAT_R11G11B10_FLOAT;
                    target.standard.MiscFlags = 0x40000;
                }
                using CreateTarget = HRESULT (STDMETHODCALLTYPE*)(void*, const Desc*, const D3D11_SUBRESOURCE_DATA*, void**);
                const auto create_target = reinterpret_cast<CreateTarget>(vt(device)[5]);
                texture = nullptr;
                check(create_target(device, &target, nullptr, &texture) == S_OK && texture,
                      "default render-target owns the approved expanded coherent image");
                if (texture) {
                    using GetTextureDesc = void (STDMETHODCALLTYPE*)(void*, Desc*);
                    Desc actual{};
                    reinterpret_cast<GetTextureDesc>(vt(texture)[12])(texture, &actual);
                    check(actual.standard.SampleDesc.Count == target.standard.SampleDesc.Count && !actual.standard.MiscFlags,
                          "GetDesc preserves four samples and consumes the native auxiliary creation hint");
                    if (multisample) {
                        using CreateSrv = HRESULT (STDMETHODCALLTYPE*)(void*, void*, const D3D11_SHADER_RESOURCE_VIEW_DESC*, void**);
                        void* srv = nullptr;
                        check(reinterpret_cast<CreateSrv>(vt(device)[7])(device, texture, nullptr, &srv) == S_OK && srv,
                              "MSAA SRV owns a real multisample native descriptor");
                        if (srv) {
                            D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
                            using GetSrvDesc = void (STDMETHODCALLTYPE*)(void*, D3D11_SHADER_RESOURCE_VIEW_DESC*);
                            reinterpret_cast<GetSrvDesc>(vt(srv)[9])(srv, &desc);
                            UINT32 word = 0; std::memcpy(&word, static_cast<char*>(srv) + 60, 4);
                            check(desc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DMS && desc.Format == target.standard.Format &&
                                ((word >> 16) & 15) == 2, "MSAA SRD retains log2(samples) instead of treating it as the last mip");
                            reinterpret_cast<Count>(vt(srv)[2])(srv);
                        }
                        D3D11_SHADER_RESOURCE_VIEW_DESC mask_desc{};
                        mask_desc.Format = DXGI_FORMAT_R8_UINT; mask_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; mask_desc.Texture2D.MipLevels = 1;
                        srv = nullptr;
                        check(reinterpret_cast<CreateSrv>(vt(device)[7])(device, texture, &mask_desc, &srv) == S_OK && srv,
                              "FMASK view derives real expanded fragment topology from the MSAA resource");
                        if (srv) {
                            using GetResource = void (STDMETHODCALLTYPE*)(void*, void**);
                            void* owner = nullptr;
                            reinterpret_cast<GetResource>(vt(srv)[8])(srv, &owner);
                            check(owner == texture, "FMASK GetResource retains the original multisample texture");
                            if (owner) reinterpret_cast<Count>(vt(owner)[2])(owner);
                            void* canonical = nullptr;
                            std::memcpy(&canonical, static_cast<char*>(srv) + 80, sizeof canonical);
                            TitleTextureLayout mask_layout;
                            HRESULT mask_hr = create_xg_texture_layout(32, 16, 1, 1, DXGI_FORMAT_R8_UINT, UINT_MAX, mask_layout, D3D11_BIND_SHADER_RESOURCE);
                            std::array<unsigned char, 32 * 16> values{};
                            if (SUCCEEDED(mask_hr)) mask_hr = mask_layout.copy(mask_layout.context.get(), false, values.data(), canonical, 0, 32, 512);
                            UINT identity = 0;
                            for (UINT sample = 0; sample < actual.standard.SampleDesc.Count; ++sample) identity |= sample << (2 * sample);
                            check(SUCCEEDED(mask_hr) && std::all_of(values.begin(), values.end(), [identity](auto x) { return x == identity; }),
                                  "FMASK bytes map each of the four backed samples to its actual independent fragment");
                            reinterpret_cast<Count>(vt(srv)[2])(srv);
                        }
                    }
                    using CreateRtv = HRESULT (STDMETHODCALLTYPE*)(void*, void*, const D3D11_RENDER_TARGET_VIEW_DESC*, void**);
                    const auto create_rtv = reinterpret_cast<CreateRtv>(vt(device)[9]);
                    void* view = nullptr;
                    D3D11_RENDER_TARGET_VIEW_DESC invalid{};
                    invalid.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D; invalid.Texture2D.MipSlice = 1;
                    check(create_rtv(device, texture, &invalid, &view) == (multisample ? DXGI_ERROR_UNSUPPORTED : E_INVALIDARG) && !view,
                          "render-target view rejects a missing mip or incompatible sample dimension");
                    check(create_rtv(device, texture, nullptr, nullptr) == S_FALSE,
                          "default render-target view validates without acquiring references");
                    check(create_rtv(device, texture, nullptr, &view) == S_OK && view,
                          "native slot nine creates a real render-target descriptor");
                    if (view) {
                        struct { D3D11_RENDER_TARGET_VIEW_DESC desc{}; UINT guard = 0xaabbccdd; } result;
                        using GetDesc = void (STDMETHODCALLTYPE*)(void*, D3D11_RENDER_TARGET_VIEW_DESC*);
                        reinterpret_cast<GetDesc>(vt(view)[9])(view, &result.desc);
                        check(result.desc.Format == target.standard.Format && result.desc.ViewDimension ==
                            (multisample ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D) &&
                            !result.desc.Texture2D.MipSlice && result.guard == 0xaabbccdd, "RTV GetDesc preserves its twenty-byte ABI");
                        UINT type = 99; void* retained = nullptr;
                        std::memcpy(&type, static_cast<char*>(view) + 40, 4);
                        using GetResource = void (STDMETHODCALLTYPE*)(void*, void**);
                        reinterpret_cast<GetResource>(vt(view)[8])(view, &retained);
                        check(type == 0 && retained == texture, "RTV public type and retained resource match the original ABI");
                        if (retained) reinterpret_cast<Count>(vt(retained)[2])(retained);
                        check(reinterpret_cast<Count>(vt(texture)[2])(texture) == 1, "RTV retains owned texture after caller release");
                        texture = nullptr;
                        reinterpret_cast<Count>(vt(view)[2])(view);
                    }
                    if (texture) reinterpret_cast<Count>(vt(texture)[2])(texture);
                }
                }
            reinterpret_cast<Count>(vt(context)[2])(context);
            reinterpret_cast<Count>(vt(device)[2])(device);
            std::cout << "original XG graphics metadata " << (failures ? "FAILED" : "passed") << '\n';
        }
        return failures ? 1 : 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--missing-slot") {
        set_stop_on_missing_activation(true);
        const auto vt = winrt_vtable();
        WinrtObject object{vt.data(), L"Xone.Tests.MissingSlot", __uuidof(IInspectable), nullptr};
        reinterpret_cast<void(*)(WinrtObject*, std::uintptr_t, std::uintptr_t)>(const_cast<void*>(vt[6]))(&object, 17, 29);
        return 1;
    }
    if (argc == 2 && std::string(argv[1]) == "--missing-activation") {
        RoInitialize(RO_INIT_MULTITHREADED);
        set_stop_on_missing_activation(true);
        HSTRING name = nullptr;
        constexpr wchar_t missing[] = L"Xone.Tests.MissingActivation";
        WindowsCreateString(missing, _countof(missing) - 1, &name);
        void* factory = nullptr;
        get_title_activation_factory(name, __uuidof(IActivationFactory), &factory);
        return 1; // The diagnostic must terminate before returning here.
    }
    if (argc != 3 && argc != 4) { std::cerr << "usage: era_loader_tests <era_component.exe> <era_tls_component.exe> [era_module_component.dll]\n"; return 2; }
    {
        using Allocate = void* (WINAPI*)(SIZE_T, ULONGLONG);
        using Release = void (WINAPI*)(void*, ULONGLONG);
        using SetHooks = void (WINAPI*)(Allocate, Release);
        const auto hooks = reinterpret_cast<SetHooks>(resolve_kernelx({"kernelx.dll", "XMemSetAllocationHooks", 0}));
        const auto allocate = reinterpret_cast<Allocate>(resolve_kernelx({"kernelx.dll", "XMemAlloc", 0}));
        const auto release = reinterpret_cast<Release>(resolve_kernelx({"kernelx.dll", "XMemFree", 0}));
        check(hooks && allocate && release, "XMem hooks resolved");
        if (hooks && allocate && release) {
            hooks(&allocate_hook, &release_hook);
            constexpr ULONGLONG attributes = 0x1234567887654321ull;
            void* result = allocate(137, attributes);
            check(result == &hook_size && hook_size == 137 && hook_attributes == attributes,
                  "XMem hook preserves size, result and 64-bit attributes");
            release(result, attributes);
            check(hook_pointer == result && hook_frees == 1 && hook_attributes == attributes,
                  "XMem release calls the registered callback");
            hooks(nullptr, nullptr);
            void* default_memory = allocate(16, 0);
            check(default_memory != nullptr, "cleared hooks use the default allocator");
            if (default_memory) release(default_memory, 0);
            release(nullptr, 0);
        }
    }
    {
        const auto startup = reinterpret_cast<decltype(&WSAStartup)>(resolve_host_module({"WS2_32.dll", "", 115}));
        const auto cleanup = reinterpret_cast<decltype(&WSACleanup)>(resolve_host_module({"WS2_32.dll", "", 116}));
        const auto socket_create = reinterpret_cast<decltype(&socket)>(resolve_host_module({"WS2_32.dll", "", 23}));
        const auto socket_close = reinterpret_cast<decltype(&closesocket)>(resolve_host_module({"WS2_32.dll", "", 3}));
        check(startup && cleanup && socket_create && socket_close, "audited Winsock ordinals resolved");
        check(resolve_host_module({"WS2_32.dll", "WSAStartup", 0}) == reinterpret_cast<void*>(startup),
              "Winsock named and ordinal bindings agree");
        check(!resolve_host_module({"WS2_32.dll", "", 65000}) &&
              !resolve_host_module({"WS2_32.dll", "XoneUnknownSocketApi", 0}), "unverified socket imports rejected");
        if (startup && cleanup && socket_create && socket_close) {
            WSADATA data{};
            const int error = startup(MAKEWORD(2, 2), &data);
            check(error == 0 && data.wVersion == MAKEWORD(2, 2), "Winsock initializes the requested version");
            if (!error) {
                const SOCKET handle = socket_create(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                check(handle != INVALID_SOCKET, "socket created without connecting to a server");
                if (handle != INVALID_SOCKET) check(socket_close(handle) == 0, "socket closes");
                check(cleanup() == 0, "Winsock cleanup succeeds");
            }
        }
    }
    {
        // These redistributables are optional; absence must stay unresolved.
        const std::pair<const char*, const wchar_t*> runtimes[]{
            {"MSVCR110.dll", L"msvcr110.dll"}, {"MSVCP110.dll", L"msvcp110.dll"},
            {"vccorlib110.DLL", L"vccorlib110.dll"}};
        for (const auto& [name, path] : runtimes) {
            const HMODULE module = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            check(resolve_host_module({name, "__xone_missing_export", 0}) == nullptr,
                  "missing legacy runtime export stays unresolved");
            check(resolve_host_module({name, "", 1}) == nullptr, "legacy runtime ordinals are not guessed");
            const char* symbol = same_library(name, "msvcr110.dll") ? "malloc" :
                (same_library(name, "msvcp110.dll") ? "?uncaught_exception@std@@YA_NXZ" :
                 "?InitializeData@Details@Platform@@YAJH@Z");
            const auto expected = module ? GetProcAddress(module, symbol) : nullptr;
            if (module) check(expected != nullptr, "installed legacy runtime has the tested named export");
            check(resolve_host_module({name, symbol, 0}) == reinterpret_cast<void*>(expected),
                  "legacy runtime resolves only the same-version System32 export");
            if (module) FreeLibrary(module);
        }
        const auto allocate = reinterpret_cast<void* (*)(std::size_t)>(
            resolve_host_module({"MSVCR110.dll", "malloc", 0}));
        const auto release = reinterpret_cast<void (*)(void*)>(
            resolve_host_module({"MSVCR110.dll", "free", 0}));
        const auto length = reinterpret_cast<std::size_t (*)(const char*)>(
            resolve_host_module({"MSVCR110.dll", "strlen", 0}));
        if (allocate && release && length) {
            void* block = allocate(32);
            check(block != nullptr, "legacy CRT allocation executes");
            if (block) {
                static_cast<char*>(block)[0] = 'x';
                static_cast<char*>(block)[1] = '\0';
                check(length(static_cast<char*>(block)) == 1, "legacy CRT string ABI executes");
                release(block);
            }
        }
        check(resolve_host_module({"untrusted.dll", "malloc", 0}) == nullptr,
              "runtime support does not enable arbitrary modules");
    }
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
    check(bind_xg_module(std::filesystem::path(argv[1]).wstring()) == HRESULT_FROM_WIN32(ERROR_CRC),
          "unregistered XG snapshot is refused before mapping or execution");
    check(initialize_xg_module() == S_FALSE && !resolve_xg_module({"xg_x.dll", "XGComputeTexture2DLayout", 0}),
          "unbound original XG has no fabricated exports or initialization");
    {
        for (const auto& item : {std::pair{"msvcrt.dll", L"msvcrt.dll"}, std::pair{"ntdll.dll", L"ntdll.dll"}}) {
            const HMODULE host = LoadLibraryExW(item.second, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            const auto* symbol = same_library(item.first, "msvcrt.dll") ? "sqrt" : "RtlCaptureContext";
            check(host && resolve_host_module({item.first, symbol, 0}) == reinterpret_cast<void*>(GetProcAddress(host, symbol)),
                  "XG dependencies bind the audited named System32 function");
            check(!resolve_host_module({item.first, "unreviewed_export", 0}) && !resolve_host_module({item.first, {}, 1}),
                  "XG dependency support does not expose arbitrary names or ordinals");
            if (host) FreeLibrary(host);
        }
        const auto root = reinterpret_cast<double (*)(double)>(resolve_host_module({"msvcrt.dll", "sqrt", 0}));
        check(root && root(81.0) == 9.0, "plain-C CRT math dependency executes with the expected ABI");
    }

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
        if (argc >= 4) {
            const auto module_file = read(argv[3]);
            LoadResult module_result;
            ULONG detached = 0;
            {
                Image module;
                check(!module.load(module_file, resolve_kernelx, module_result), "DLL is still refused as a title entry");
                check(!module.load(file, resolve_kernelx, module_result, ImageKind::module), "module route requires a DLL");
                check(module.load(module_file, resolve_kernelx, module_result, ImageKind::module), "owned DLL maps through the same loader");
                for (const auto& error : module_result.errors) std::cerr << "  module: " << error << '\n';
                const auto exported = reinterpret_cast<unsigned long long (*)()>(module.find_export("era_test_module_export"));
                const auto receipt = reinterpret_cast<void (*)(ULONG*)>(module.find_export("era_test_module_receipt"));
                check(exported && receipt, "validated module exports resolve by name");
                check(!module.find_export("unknown_export") && !module.find_export({}, 65000), "unknown names/ordinals stay unresolved");
                bool ordinal_matches = false;
                for (WORD ordinal = 1; ordinal <= 4; ++ordinal) ordinal_matches |= module.find_export({}, ordinal) == reinterpret_cast<void*>(exported);
                check(exported && ordinal_matches, "module ordinal resolves the same actual export");
                if (exported && receipt) {
                    check(exported() == GetCurrentProcessId(), "mapping did not execute DLL initialization");
                    check(module.attach_module() && module.attach_module(), "module attach is explicit and idempotent");
                    check(exported() == (1ull << 32 | GetCurrentProcessId()), "DLL initialization ran once and used audited kernel imports");
                    receipt(&detached);
                }
                check(module_handle && module_handle(nullptr) == reinterpret_cast<HMODULE>(image.base()), "helper loading preserves the title's module identity");
            }
            check(detached == 1, "module detach runs once before unmapping");
            check(module_handle && module_handle(nullptr) == reinterpret_cast<HMODULE>(image.base()), "helper destruction does not clear title identity");
            auto malformed = module_file;
            if (malformed.size() > sizeof(IMAGE_DOS_HEADER)) {
                const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(malformed.data());
                auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(malformed.data() + dos->e_lfanew);
                nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT] = {nt->OptionalHeader.SizeOfImage - 8, 64};
                Image rejected;
                check(!rejected.load(malformed, resolve_kernelx, module_result, ImageKind::module) && !rejected.base(),
                      "module with export tables outside the image cannot publish a mapping");
                auto tls_module = read(argv[2]);
                const auto tls_dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(tls_module.data());
                reinterpret_cast<IMAGE_NT_HEADERS64*>(tls_module.data() + tls_dos->e_lfanew)->FileHeader.Characteristics |= IMAGE_FILE_DLL;
                check(!rejected.load(tls_module, resolve_kernelx, module_result, ImageKind::module), "module TLS cannot reuse the title's single static slot");
                check(module_handle && module_handle(nullptr) == reinterpret_cast<HMODULE>(image.base()), "failed module validation preserves title identity");
            }
        }
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
    {
        const auto allocate = reinterpret_cast<decltype(&VirtualAlloc)>(resolve_kernelx({"kernelx.dll", "VirtualAlloc", 0}));
        const auto release = reinterpret_cast<decltype(&VirtualFree)>(resolve_kernelx({"kernelx.dll", "VirtualFree", 0}));
        check(allocate && release && reinterpret_cast<void*>(allocate) != reinterpret_cast<void*>(&VirtualAlloc),
              "title memory has an owned allocation boundary");
        if (allocate && release) {
            void* region = allocate(nullptr, 65536, 0x40000000 | MEM_RESERVE, PAGE_READWRITE);
            check(region != nullptr, "MEM_TITLE reserves without committing");
            if (region) {
                MEMORY_BASIC_INFORMATION info{};
                VirtualQuery(region, &info, sizeof info);
                check(info.State == MEM_RESERVE, "title reservation has no physical commit");
                auto* page = static_cast<unsigned char*>(allocate(region, 4096, MEM_COMMIT, PAGE_READWRITE));
                check(page == region, "commit preserves title virtual address");
                if (page) { page[0] = 42; check(page[0] == 42, "committed title memory writable"); }
                check(release(region, 4096, MEM_DECOMMIT), "title page decommits");
                check(release(region, 0, MEM_RELEASE), "title reservation released");
                check(!release(region, 0, MEM_RELEASE), "double release rejected");
            }
            void* host = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            check(host != nullptr, "host memory fixture allocated");
            if (host) {
                check(!release(host, 0, MEM_RELEASE), "title cannot release host memory");
                check(!allocate(host, 4096, MEM_COMMIT, PAGE_READWRITE), "title cannot commit into host memory");
                VirtualFree(host, 0, MEM_RELEASE);
            }
            check(!allocate(nullptr, 4096, 0x10000000 | MEM_RESERVE, PAGE_READWRITE) &&
                      GetLastError() == ERROR_NOT_SUPPORTED, "unsupported graphics memory flags are explicit");
            constexpr SIZE_T graphics_page = 1u << 21;
            void* probe = VirtualAlloc(nullptr, 2 * graphics_page, MEM_RESERVE, PAGE_NOACCESS);
            check(probe != nullptr, "graphics reservation address fixture");
            if (probe) {
                const auto aligned = (reinterpret_cast<std::uintptr_t>(probe) + graphics_page - 1) & ~(graphics_page - 1);
                VirtualFree(probe, 0, MEM_RELEASE);
                void* graphics = allocate(reinterpret_cast<void*>(aligned), graphics_page,
                    0x10000000 | MEM_LARGE_PAGES | MEM_RESERVE, PAGE_READWRITE);
                check(graphics == reinterpret_cast<void*>(aligned), "graphics reservation preserves exact aligned address");
                if (graphics) {
                    MEMORY_BASIC_INFORMATION info{};
                    VirtualQuery(graphics, &info, sizeof info);
                    check(info.State == MEM_RESERVE, "graphics reservation has no fabricated backing");
                    check(!allocate(graphics, 4096, MEM_COMMIT, PAGE_READWRITE) && GetLastError() == ERROR_NOT_SUPPORTED,
                          "CPU commit cannot bypass graphics mapping contract");
                    check(release(graphics, 0, MEM_RELEASE), "unused graphics reservation released");
                }
            }
        }
    }
    check(resolve_kernelx({"kernel32.dll", "Sleep", 0}) == nullptr, "only kernelx.dll is handled");
    {
        const auto create = reinterpret_cast<decltype(&CreateIoCompletionPort)>(
            resolve_kernelx({"kernelx.dll", "CreateIoCompletionPort", 0}));
        const auto post = reinterpret_cast<decltype(&PostQueuedCompletionStatus)>(
            resolve_kernelx({"kernelx.dll", "PostQueuedCompletionStatus", 0}));
        const auto get = reinterpret_cast<decltype(&GetQueuedCompletionStatus)>(
            resolve_kernelx({"kernelx.dll", "GetQueuedCompletionStatus", 0}));
        check(create && post && get, "completion port imports resolved");
        if (create && post && get) {
            HANDLE port = create(INVALID_HANDLE_VALUE, nullptr, 0, 1);
            check(port != nullptr, "title completion port created");
            if (port) {
                OVERLAPPED operation{};
                DWORD bytes = 0;
                ULONG_PTR key = 0;
                OVERLAPPED* received = nullptr;
                check(post(port, 37, 0x123456789ull, &operation) != FALSE, "completion packet queued");
                check(get(port, &bytes, &key, &received, 100) && bytes == 37 &&
                          key == 0x123456789ull && received == &operation,
                      "completion packet preserves bytes, 64-bit key and operation");
                SetLastError(ERROR_SUCCESS);
                check(!get(port, &bytes, &key, &received, 0) && GetLastError() == WAIT_TIMEOUT && !received,
                      "empty completion queue preserves real timeout");
                CloseHandle(port);
            }
        }
        for (const char* name : {"CopyFile2", "LoadLibraryW"})
            check(resolve_kernelx({"kernelx.dll", name, 0}) == nullptr,
                  "storage mutation, title memory and private loading remain explicit gaps");
    }
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
        const auto open = reinterpret_cast<decltype(&CreateFileW)>(resolve_kernelx({"kernelx.dll", "CreateFileW", 0}));
        const auto write = reinterpret_cast<decltype(&WriteFile)>(resolve_kernelx({"kernelx.dll", "WriteFile", 0}));
        const auto truncate = reinterpret_cast<decltype(&SetEndOfFile)>(resolve_kernelx({"kernelx.dll", "SetEndOfFile", 0}));
        const auto flush = reinterpret_cast<decltype(&FlushFileBuffers)>(resolve_kernelx({"kernelx.dll", "FlushFileBuffers", 0}));
        check(open && write && truncate && flush, "storage handle APIs bound");
        if (open && write && truncate && flush) {
            set_title_storage_root({});
            check(open(inside.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr) == INVALID_HANDLE_VALUE &&
                      GetLastError() == ERROR_ACCESS_DENIED, "no storage: destructive open denied");
            set_title_storage_root(root);
            const DWORD mutation_access[]{GENERIC_WRITE, GENERIC_ALL, MAXIMUM_ALLOWED, DELETE, FILE_APPEND_DATA};
            for (DWORD access : mutation_access)
                check(open(outside.c_str(), access, 0, nullptr, OPEN_EXISTING, 0, nullptr) == INVALID_HANDLE_VALUE &&
                          GetLastError() == ERROR_ACCESS_DENIED, "package mutation access denied");
            check(open(outside.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_FLAG_DELETE_ON_CLOSE, nullptr) ==
                      INVALID_HANDLE_VALUE, "read access cannot request package deletion on close");
            check(open((inside + L":stream").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr) ==
                      INVALID_HANDLE_VALUE, "alternate data streams refused");
            HANDLE inherited = CreateFileW(outside.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            DWORD written = 0;
            check(inherited != INVALID_HANDLE_VALUE, "outside handle fixture opened");
            if (inherited != INVALID_HANDLE_VALUE) {
                check(!write(inherited, "x", 1, &written, nullptr) && GetLastError() == ERROR_ACCESS_DENIED,
                      "outside handle cannot bypass write policy");
                check(!truncate(inherited) && !flush(inherited), "outside handle cannot truncate or flush");
                CloseHandle(inherited);
            }
            HANDLE save = open(inside.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            check(save != INVALID_HANDLE_VALUE, "writable save opened");
            if (save != INVALID_HANDLE_VALUE) {
                check(write(save, "save", 4, &written, nullptr) && written == 4 && flush(save),
                      "save data written and flushed");
                SetFilePointer(save, 2, nullptr, FILE_BEGIN);
                check(truncate(save) && GetFileSize(save, nullptr) == 2, "save truncates at current offset");
                CloseHandle(save);
            }
            save = open(inside.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            if (save != INVALID_HANDLE_VALUE) {
                char data[4]{};
                DWORD count = 0;
                check(ReadFile(save, data, sizeof data, &count, nullptr) && count == 2 && data[0] == 's' && data[1] == 'a',
                      "save bytes persist across handle reopen");
                CloseHandle(save);
            } else check(false, "saved data reopens");
            const std::wstring extended = L"\\\\?\\" + inside;
            save = open(extended.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            check(save != INVALID_HANDLE_VALUE, "extended path uses the same storage policy");
            if (save != INVALID_HANDLE_VALUE) CloseHandle(save);
        }
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

            // DXGI chain the title walks to its swapchain: device -> IDXGIDevice1 -> adapter -> factory.
            {
                constexpr GUID dxgi_device1{0x77DB970F, 0x6276, 0x48BA, {0xBA, 0x28, 0x07, 0x01, 0x43, 0xB4, 0x39, 0x2C}};
                constexpr GUID factory2{0x50C83A1C, 0xE072, 0x4C48, {0x87, 0xB0, 0x36, 0x30, 0xFA, 0x36, 0xA6, 0xD0}};
                const ULONG before = refcount(device);
                void* dxgi = nullptr;
                void* adapter = nullptr;
                void* factory = nullptr;
                check(reinterpret_cast<Query>(vt(device)[0])(device, dxgi_device1, &dxgi) == S_OK && dxgi && dxgi != device &&
                      refcount(device) == before + 1, "device answers IDXGIDevice1 with a DXGI object holding it");
                using GetAdapter = HRESULT (STDMETHODCALLTYPE*)(void*, void**);
                using GetParent = HRESULT (STDMETHODCALLTYPE*)(void*, REFIID, void**);
                if (dxgi) check(reinterpret_cast<GetAdapter>(vt(dxgi)[7])(dxgi, &adapter) == S_OK && adapter, "GetAdapter");
                if (adapter) check(reinterpret_cast<GetParent>(vt(adapter)[6])(adapter, factory2, &factory) == S_OK && factory,
                                   "adapter parent is IDXGIFactory2");
                if (factory) {
                    using CreateForWindow = HRESULT (STDMETHODCALLTYPE*)(void*, void*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*,
                                                                         void*, void**);
                    DXGI_SWAP_CHAIN_DESC1 desc{1920, 1080, DXGI_FORMAT_B8G8R8A8_UNORM, FALSE, {1, 0}, DXGI_USAGE_RENDER_TARGET_OUTPUT,
                                               2, DXGI_SCALING_STRETCH, DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL};
                    void* chain = &chain;
                    IUnknown* foreign = reinterpret_cast<IUnknown*>(&desc);
                    check(reinterpret_cast<CreateForWindow>(vt(factory)[16])(factory, device, foreign, &desc, nullptr, &chain) ==
                              DXGI_ERROR_INVALID_CALL && !chain, "swapchain only for the title-layer CoreWindow");
                    reinterpret_cast<Count>(vt(factory)[2])(factory);
                }
                if (adapter) reinterpret_cast<Count>(vt(adapter)[2])(adapter);
                if (dxgi) reinterpret_cast<Count>(vt(dxgi)[2])(dxgi);
                check(refcount(device) == before, "DXGI objects release their device references");
            }

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
        // Synthetic resource binds the old ABI; no original DLL is loaded.
        check(bind_d3d11x_abi(std::filesystem::absolute(argv[1]).wstring()) == S_OK, "old graphics ABI version binding");
        struct Parameters {
            UINT version = 0x1000b, flags = 0x04000000;
            void* tessellation = nullptr;
            void* factors = nullptr;
            UINT affinity = 0x3f, de = 0, ce = 0, de_segment = 0, ce_segment = 0;
        } p;
        using Create = HRESULT (WINAPI*)(const Parameters*, void**, void**);
        const auto create = reinterpret_cast<Create>(resolve_title_import({"d3d11_x.dll", "D3D11XCreateDeviceX", 0}));
        check(create != nullptr, "bound old graphics factory resolved");
        using MapEsram = HRESULT (WINAPI*)(UINT, void*, UINT, const UINT*);
        const auto map_esram = reinterpret_cast<MapEsram>(resolve_title_import({"d3d11_x.dll", "D3DMapEsramMemory", 0}));
        check(map_esram && map_esram(1, nullptr, 1, nullptr) == E_INVALIDARG, "bound ESRAM import validates the original four-argument ABI");
        void* device = &p;
        void* context = &p;
        if (create) {
            check(create(nullptr, &device, &context) == E_INVALIDARG && !device && !context, "null parameters clear outputs");
            check(create(&p, &device, nullptr) == E_INVALIDARG && !device, "both device outputs required");
            p.version = 7;
            check(create(&p, &device, &context) == E_INVALIDARG && !device && !context, "unknown SDK rejected");
            p.version = 0x1000b;
            p.flags = 0;
            check(create(&p, &device, &context) == DXGI_ERROR_UNSUPPORTED && !device && !context,
                  "unimplemented deferred-deletion policy rejected");
            p.flags = 0x04000002;
            check(create(&p, &device, &context) == DXGI_ERROR_UNSUPPORTED, "unknown creation flags not ignored");
            p.flags = 0x04000000;
            p.de = 4096;
            check(create(&p, &device, &context) == DXGI_ERROR_UNSUPPORTED, "caller GPU ring storage not ignored");
            p.de = 0;
            check(create(&p, &device, &context) == S_OK && device && context, "legacy graphics device created");
            if (device && context) {
                const auto vt = [](void* o){return *static_cast<void* const* const*>(o);};
                const auto refs = [](void* o)->ULONG&{return *reinterpret_cast<ULONG*>(static_cast<char*>(o)+8);};
                using Count = ULONG (STDMETHODCALLTYPE*)(void*);
                using Flags = UINT (STDMETHODCALLTYPE*)(void*);
                check(refs(device) == 1 && refs(context) == 1, "legacy inline counts match original initial values");
                {
                    struct BufferDesc { D3D11_BUFFER_DESC standard; UINT64 extension; } desc{{72, D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0}, 0};
                    static_assert(sizeof(BufferDesc) == 32);
                    using SetHooks = void (WINAPI*)(GraphicsAllocate, GraphicsFree);
                    const auto set_hooks = reinterpret_cast<SetHooks>(resolve_kernelx({"kernelx.dll", "XMemSetAllocationHooks", 0}));
                    graphics_default_allocate = reinterpret_cast<GraphicsAllocate>(resolve_kernelx({"kernelx.dll", "XMemAllocDefault", 0}));
                    graphics_default_free = reinterpret_cast<GraphicsFree>(resolve_kernelx({"kernelx.dll", "XMemFreeDefault", 0}));
                    graphics_hook_allocations = graphics_hook_frees = graphics_hook_wrong_allocator = 0;
                    set_hooks(graphics_allocate_hook, graphics_free_hook);
                    using CreateBuffer = HRESULT (STDMETHODCALLTYPE*)(void*, const BufferDesc*, const D3D11_SUBRESOURCE_DATA*, void**);
                    const auto factory = reinterpret_cast<CreateBuffer>(vt(device)[3]);
                    std::array<UINT32, 18> vertices{};
                    for (UINT i = 0; i < vertices.size(); ++i) vertices[i] = 0xaabbcc00 + i;
                    D3D11_SUBRESOURCE_DATA initial{vertices.data(), 0, 0};
                    void* buffer = device;
                    check(factory(device, nullptr, &initial, &buffer) == E_INVALIDARG && !buffer, "buffer description required");
                    check(factory(device, &desc, &initial, nullptr) == S_FALSE, "buffer validation only");
                    check(factory(device, &desc, &initial, &buffer) == S_OK && buffer, "shared native vertex buffer created");
                    if (buffer) {
                        check(refs(buffer) == 1 && refs(device) == 1, "buffer owns internal device dependency");
                        void* allocation = nullptr;
                        std::memcpy(&allocation, static_cast<char*>(buffer) + 48, sizeof allocation);
                        check(allocation && !std::memcmp(allocation, vertices.data(), sizeof vertices), "initial data reaches actual title allocation");
                        std::array<UINT32, 4> srd{};
                        std::memcpy(srd.data(), static_cast<char*>(buffer) + 32, sizeof srd);
                        const UINT64 address = srd[0] | (UINT64(srd[1] & 255) << 32);
                        check(address == reinterpret_cast<std::uintptr_t>(allocation) && !(srd[1] & 0xffffff00) && srd[2] == 72 && srd[3] == 0x24fac,
                              "native buffer SRD addresses the same CPU/GPU allocation");
                        using GetType = void (STDMETHODCALLTYPE*)(void*, D3D11_RESOURCE_DIMENSION*);
                        D3D11_RESOURCE_DIMENSION dimension{};
                        reinterpret_cast<GetType>(vt(buffer)[8])(buffer, &dimension);
                        check(dimension == D3D11_RESOURCE_DIMENSION_BUFFER, "buffer GetType native slot");
                        using GetDescriptor = void (STDMETHODCALLTYPE*)(void*, void**);
                        void* resource = nullptr;
                        reinterpret_cast<GetDescriptor>(vt(buffer)[11])(buffer, &resource);
                        check(resource == buffer && refs(buffer) == 1, "resource descriptor is original identity without extra reference");
                        using GetDesc = void (STDMETHODCALLTYPE*)(void*, BufferDesc*);
                        struct { BufferDesc desc; UINT64 guard = 0x123456789; } result{};
                        reinterpret_cast<GetDesc>(vt(buffer)[12])(buffer, &result.desc);
                        check(result.desc.standard.ByteWidth == 72 && result.desc.standard.BindFlags == D3D11_BIND_VERTEX_BUFFER &&
                            result.desc.extension == 0 && result.guard == 0x123456789, "buffer GetDesc writes old thirty-two-byte description only");
                        check(reinterpret_cast<Count>(vt(buffer)[2])(buffer) == 0, "buffer resource and registry pin retired");
                        MEMORY_BASIC_INFORMATION memory{};
                        VirtualQuery(allocation, &memory, sizeof memory);
                        check(memory.State == MEM_FREE, "last buffer reference frees its title allocation");
                    }
                    desc.standard.Usage = D3D11_USAGE_IMMUTABLE;
                    check(factory(device, &desc, nullptr, &buffer) == E_INVALIDARG && !buffer, "immutable buffer needs initial bytes");
                    desc.standard.Usage = D3D11_USAGE_DEFAULT; desc.extension = 1;
                    check(factory(device, &desc, &initial, &buffer) == DXGI_ERROR_UNSUPPORTED && !buffer, "unknown old buffer extension rejected");
                    set_stop_on_missing_activation(true);
                    set_d3d11x_trap_handler([](const char* name, unsigned slot) -> std::uintptr_t {
                        return std::strcmp(name, "ID3D11DeviceX") == 0 && slot == 3 ? 0x80004001u : 0x80004005u;
                    });
                    check(factory(device, &desc, &initial, &buffer) == E_NOTIMPL && !buffer,
                          "diagnostic stops at unsupported buffer variant before caller consumes null resource");
                    set_d3d11x_trap_handler(nullptr);
                    set_stop_on_missing_activation(false);
                    desc.extension = 0; desc.standard.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
                    check(factory(device, &desc, &initial, &buffer) == E_INVALIDARG && !buffer, "constant buffer size must be a multiple of sixteen");
                    desc.standard.ByteWidth = 64;
                    check(factory(device, &desc, &initial, &buffer) == S_OK && buffer, "shared constant buffer created");
                    if (buffer) {
                        std::array<UINT32, 4> srd{};
                        std::memcpy(srd.data(), static_cast<char*>(buffer) + 32, sizeof srd);
                        check((srd[1] & 0x3fff0000) == 0x100000 && srd[2] == 4, "constant SRD preserves sixteen-byte stride and record count");
                        reinterpret_cast<Count>(vt(buffer)[2])(buffer);
                    }
                    desc.standard = {80, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_READ, 0, 0};
                    check(factory(device, &desc, nullptr, &buffer) == DXGI_ERROR_UNSUPPORTED && !buffer,
                          "dynamic buffers refuse unimplemented CPU-read storage");
                    desc.standard.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
                    check(factory(device, &desc, nullptr, &buffer) == S_OK && buffer, "observed eighty-byte dynamic constant buffer created");
                    if (buffer) {
                        using Map = HRESULT (STDMETHODCALLTYPE*)(void*, void*, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*);
                        using Unmap = void (STDMETHODCALLTYPE*)(void*, void*, UINT);
                        const auto map = reinterpret_cast<Map>(vt(context)[15]);
                        const auto unmap = reinterpret_cast<Unmap>(vt(context)[16]);
                        struct { D3D11_MAPPED_SUBRESOURCE mapped{}; UINT64 guard = 0xabcdef; } result;
                        check(map(context, buffer, 1, D3D11_MAP_WRITE_DISCARD, 0, &result.mapped) == E_INVALIDARG && !result.mapped.pData,
                              "buffer mapping validates the subresource and clears its output");
                        check(map(context, buffer, 0, D3D11_MAP_READ, 0, &result.mapped) == E_INVALIDARG,
                              "write-only dynamic storage cannot be read-mapped");
                        check(map(context, buffer, 0, D3D11_MAP_WRITE_NO_OVERWRITE, 0, &result.mapped) == DXGI_ERROR_UNSUPPORTED,
                              "constant buffer no-overwrite feature is explicit until implemented");
                        check(map(context, buffer, 0, D3D11_MAP_WRITE_DISCARD, 1, &result.mapped) == DXGI_ERROR_UNSUPPORTED,
                              "unknown Xbox map flags are not silently masked");
                        check(map(context, buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &result.mapped) == S_OK && result.mapped.pData,
                              "native context slot fifteen maps real dynamic storage");
                        void* allocation = nullptr;
                        std::memcpy(&allocation, static_cast<char*>(buffer) + 48, sizeof allocation);
                        check(result.mapped.pData == allocation && result.guard == 0xabcdef && refs(buffer) == 1,
                              "map preserves original buffer identity, address and output extent");
                        if (result.mapped.pData) {
                            auto* words = static_cast<UINT32*>(result.mapped.pData);
                            for (UINT i = 0; i < 20; ++i) words[i] = 0xabc00000 + i;
                            D3D11_MAPPED_SUBRESOURCE duplicate{};
                            check(map(context, buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &duplicate) == HRESULT_FROM_WIN32(ERROR_BUSY) && !duplicate.pData,
                                  "a second map cannot alias active CPU writes");
                            unmap(context, buffer, 0);
                            check(static_cast<UINT32*>(allocation)[19] == 0xabc00013, "unmap keeps CPU writes in canonical title bytes");
                            check(map(context, buffer, 0, D3D11_MAP_WRITE_DISCARD, D3D11_MAP_FLAG_DO_NOT_WAIT, &result.mapped) == DXGI_ERROR_UNSUPPORTED && !result.mapped.pData,
                                  "unimplemented map flag combinations remain explicit");
                            check(map(context, buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &result.mapped) == S_OK,
                                  "completed dynamic buffer can be mapped again");
                            if (result.mapped.pData) unmap(context, buffer, 0);
                        }
                        reinterpret_cast<Count>(vt(buffer)[2])(buffer);
                    }
                    desc.standard = {1332, D3D11_USAGE_DYNAMIC, D3D11_BIND_SHADER_RESOURCE,
                        D3D11_CPU_ACCESS_WRITE, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 36};
                    check(factory(device, &desc, nullptr, &buffer) == S_OK && buffer,
                          "observed dynamic structured buffer retains its Xbox CPU-write contract");
                    if (buffer) {
                        using Map = HRESULT (STDMETHODCALLTYPE*)(void*, void*, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*);
                        using Unmap = void (STDMETHODCALLTYPE*)(void*, void*, UINT);
                        const auto map = reinterpret_cast<Map>(vt(context)[15]);
                        D3D11_MAPPED_SUBRESOURCE mapped{};
                        check(map(context, buffer, 0, D3D11_MAP_WRITE_NO_OVERWRITE, 0, &mapped) == DXGI_ERROR_UNSUPPORTED && !mapped.pData,
                              "structured SRV no-overwrite remains unsupported");
                        check(map(context, buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped) == S_OK && mapped.pData && mapped.RowPitch == 1332,
                              "dynamic structured map exposes the complete canonical span");
                        if (mapped.pData) {
                            static_cast<unsigned char*>(mapped.pData)[1331] = 0xa7;
                            reinterpret_cast<Unmap>(vt(context)[16])(context, buffer, 0);
                            void* allocation = nullptr;
                            std::memcpy(&allocation, static_cast<char*>(buffer) + 48, sizeof allocation);
                            check(allocation == mapped.pData && static_cast<unsigned char*>(allocation)[1331] == 0xa7,
                                  "structured writes preserve shared memory identity");
                        }
                        reinterpret_cast<Count>(vt(buffer)[2])(buffer);
                    }
                    desc.standard = {280000, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
                        0, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 28};
                    check(factory(device, &desc, nullptr, &buffer) == S_OK && buffer, "observed structured UAV buffer binds actual shared GPU resource");
                    if (buffer) {
                        std::array<UINT32, 4> srd{};
                        std::memcpy(srd.data(), static_cast<char*>(buffer) + 32, sizeof srd);
                        check((srd[1] & 0x3fff0000) == (28u << 16) && srd[2] == 10000 && srd[3] == 0x24fac,
                              "structured resource SRD preserves stride and record count");
                        using CreateUav = HRESULT (STDMETHODCALLTYPE*)(void*, void*, const D3D11_UNORDERED_ACCESS_VIEW_DESC*, void**);
                        const auto create_uav = reinterpret_cast<CreateUav>(vt(device)[8]);
                        D3D11_UNORDERED_ACCESS_VIEW_DESC view_desc{};
                        view_desc.Format = DXGI_FORMAT_UNKNOWN; view_desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
                        view_desc.Buffer = {2, 3, D3D11_BUFFER_UAV_FLAG_APPEND};
                        void* view = buffer;
                        check(create_uav(device, nullptr, &view_desc, &view) == E_INVALIDARG && !view, "UAV needs actual resource");
                        view_desc.Buffer.FirstElement = 10000;
                        check(create_uav(device, buffer, &view_desc, &view) == E_INVALIDARG && !view, "UAV range cannot exceed logical buffer");
                        view_desc.Buffer.FirstElement = 2;
                        check(create_uav(device, buffer, &view_desc, nullptr) == S_FALSE && refs(buffer) == 1, "UAV validation-only preserves ownership");
                        check(create_uav(device, buffer, &view_desc, &view) == S_OK && view && refs(buffer) == 2,
                              "append UAV owns real resource and counter");
                        if (view) {
                            void* allocation = nullptr; void* view_allocation = nullptr; void* retained = nullptr;
                            std::memcpy(&allocation, static_cast<char*>(buffer) + 48, sizeof allocation);
                            std::memcpy(&view_allocation, static_cast<char*>(view) + 80, sizeof view_allocation);
                            std::memcpy(&retained, static_cast<char*>(view) + 32, sizeof retained);
                            std::array<UINT32, 8> descriptor{};
                            std::memcpy(descriptor.data(), static_cast<char*>(view) + 48, sizeof descriptor);
                            const UINT64 view_address = descriptor[0] | (UINT64(descriptor[1] & 255) << 32);
                            check(retained == buffer && allocation == view_allocation && view_address == reinterpret_cast<std::uintptr_t>(allocation) + 56 &&
                                (descriptor[1] & 0x3fff0000) == (28u << 16) && descriptor[2] == 3 && descriptor[3] == 0x24fac &&
                                !descriptor[4] && !descriptor[5] && !descriptor[6] && !descriptor[7],
                                "native UAV prefix and structured SRD retain base, offset, stride and extent");
                            using Query = HRESULT (STDMETHODCALLTYPE*)(void*, REFIID, void**);
                            void* queried = nullptr;
                            check(reinterpret_cast<Query>(vt(view)[0])(view, __uuidof(ID3D11UnorderedAccessView), &queried) == S_OK && queried == view,
                                  "native UAV IID returns original identity");
                            if (queried) reinterpret_cast<Count>(vt(queried)[2])(queried);
                            check(reinterpret_cast<Query>(vt(view)[0])(view, __uuidof(ID3D11ShaderResourceView), &queried) == E_NOINTERFACE && !queried,
                                  "UAV does not pretend to implement shader-resource-view methods");
                            using GetDesc = void (STDMETHODCALLTYPE*)(void*, D3D11_UNORDERED_ACCESS_VIEW_DESC*);
                            struct { D3D11_UNORDERED_ACCESS_VIEW_DESC desc{}; UINT32 guard = 0xabcdef; } actual;
                            reinterpret_cast<GetDesc>(vt(view)[9])(view, &actual.desc);
                            check(actual.desc.Buffer.FirstElement == 2 && actual.desc.Buffer.NumElements == 3 &&
                                actual.desc.Buffer.Flags == D3D11_BUFFER_UAV_FLAG_APPEND && actual.guard == 0xabcdef, "GetDesc preserves append range and output size");
                            using GetResource = void (STDMETHODCALLTYPE*)(void*, void**);
                            void* resource = nullptr;
                            reinterpret_cast<GetResource>(vt(view)[8])(view, &resource);
                            check(resource == buffer && refs(buffer) == 3, "UAV GetResource adds actual resource reference");
                            reinterpret_cast<Count>(vt(resource)[2])(resource);
                            check(reinterpret_cast<Count>(vt(buffer)[2])(buffer) == 1, "UAV keeps buffer alive after caller releases it");
                            buffer = nullptr;
                            check(reinterpret_cast<Count>(vt(view)[2])(view) == 0, "UAV and hidden counter retire together");
                            MEMORY_BASIC_INFORMATION memory{};
                            VirtualQuery(allocation, &memory, sizeof memory);
                            check(memory.State == MEM_FREE, "final view release frees the retained buffer allocation");
                        }
                        if (buffer) reinterpret_cast<Count>(vt(buffer)[2])(buffer);
                    }
                    desc.standard.StructureByteStride = 0;
                    check(factory(device, &desc, nullptr, &buffer) == E_INVALIDARG && !buffer, "structured buffer cannot have zero stride");
                    desc.standard = {16, D3D11_USAGE_DEFAULT, 0, 0, D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS, 0};
                    check(factory(device, &desc, &initial, &buffer) == S_OK && buffer, "indirect argument storage needs no shader binding flag");
                    if (buffer) {
                        std::array<UINT32, 4> srd{};
                        std::memcpy(srd.data(), static_cast<char*>(buffer) + 32, sizeof srd);
                        check(!(srd[1] & 0x3fff0000) && srd[2] == 16, "argument buffer header preserves byte count");
                        reinterpret_cast<Count>(vt(buffer)[2])(buffer);
                    }
                    graphics_force_header = true;
                    desc.standard = {280000, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
                        0, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 28};
                    check(factory(device, &desc, nullptr, &buffer) == S_OK && buffer, "header-bearing system allocation remains a valid buffer");
                    if (buffer) {
                        void* allocation = nullptr;
                        std::memcpy(&allocation, static_cast<char*>(buffer) + 48, sizeof allocation);
                        D3D11_UNORDERED_ACCESS_VIEW_DESC view_desc{};
                        view_desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
                        view_desc.Buffer = {0, 10000, D3D11_BUFFER_UAV_FLAG_APPEND};
                        using CreateUav = HRESULT (STDMETHODCALLTYPE*)(void*, void*, const D3D11_UNORDERED_ACCESS_VIEW_DESC*, void**);
                        void* view = buffer;
                        check(reinterpret_cast<CreateUav>(vt(device)[8])(device, buffer, &view_desc, &view) == S_OK && view && refs(buffer) == 2,
                              "aligned representation accepts actual sixteen-byte offset and stride twenty-eight");
                        bool header = (reinterpret_cast<std::uintptr_t>(allocation) & 65535) == 16;
                        for (unsigned i = 0; i < 16; ++i) header = header && static_cast<unsigned char*>(allocation)[int(i) - 16] == 0x5a;
                        check(header, "aligned view preserves original allocator address and metadata");
                        if (view) reinterpret_cast<Count>(vt(view)[2])(view);
                        using CreateSrv = HRESULT (STDMETHODCALLTYPE*)(void*, void*, const D3D11_SHADER_RESOURCE_VIEW_DESC*, void**);
                        const auto create_srv = reinterpret_cast<CreateSrv>(vt(device)[7]);
                        D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{};
                        srv_desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
                        srv_desc.Buffer = {1, 9999};
                        check(create_srv(device, nullptr, &srv_desc, &view) == E_INVALIDARG && !view, "SRV requires an actual buffer");
                        srv_desc.Buffer.NumElements = 10000;
                        check(create_srv(device, buffer, &srv_desc, &view) == E_INVALIDARG && !view, "SRV range cannot exceed logical buffer");
                        srv_desc.Buffer.NumElements = 9999;
                        srv_desc.Format = DXGI_FORMAT_R32_UINT;
                        check(create_srv(device, buffer, &srv_desc, &view) == DXGI_ERROR_UNSUPPORTED && !view, "structured SRV refuses typed reinterpretation");
                        srv_desc.Format = DXGI_FORMAT_UNKNOWN;
                        check(create_srv(device, buffer, &srv_desc, nullptr) == S_FALSE && refs(buffer) == 1,
                              "SRV validation-only preserves resource ownership");
                        check(create_srv(device, buffer, &srv_desc, &view) == S_OK && view && refs(buffer) == 2,
                              "SRV and UAV share the original header-bearing logical buffer");
                        if (view) {
                            UINT type = 0;
                            std::memcpy(&type, static_cast<char*>(view) + 40, sizeof type);
                            std::array<UINT32, 8> descriptor{};
                            std::memcpy(descriptor.data(), static_cast<char*>(view) + 48, sizeof descriptor);
                            const UINT64 address = descriptor[0] | (UINT64(descriptor[1] & 255) << 32);
                            check(type == 2 && address == reinterpret_cast<std::uintptr_t>(allocation) + 28 && descriptor[2] == 9999 &&
                                descriptor[3] == 0x24fac && !descriptor[4] && !descriptor[5] && !descriptor[6] && !descriptor[7],
                                "native SRV prefix and SRD preserve unaligned base, element range and upper zero bytes");
                            using GetDesc = void (STDMETHODCALLTYPE*)(void*, D3D11_SHADER_RESOURCE_VIEW_DESC*);
                            struct { D3D11_SHADER_RESOURCE_VIEW_DESC desc{}; UINT32 guard = 0xabcdef; } actual;
                            reinterpret_cast<GetDesc>(vt(view)[9])(view, &actual.desc);
                            check(actual.desc.Buffer.FirstElement == 1 && actual.desc.Buffer.NumElements == 9999 && actual.guard == 0xabcdef,
                                  "SRV GetDesc writes its actual twenty-four-byte descriptor");
                            using QueryView = HRESULT (STDMETHODCALLTYPE*)(void*, REFIID, void**);
                            void* queried = nullptr;
                            check(reinterpret_cast<QueryView>(vt(view)[0])(view, __uuidof(ID3D11ShaderResourceView), &queried) == S_OK && queried == view,
                                  "SRV IID returns its original identity");
                            if (queried) reinterpret_cast<Count>(vt(queried)[2])(queried);
                            check(reinterpret_cast<QueryView>(vt(view)[0])(view, __uuidof(ID3D11UnorderedAccessView), &queried) == E_NOINTERFACE && !queried,
                                  "SRV does not claim UAV methods");
                            using GetResource = void (STDMETHODCALLTYPE*)(void*, void**);
                            void* retained = nullptr;
                            reinterpret_cast<GetResource>(vt(view)[8])(view, &retained);
                            check(retained == buffer && refs(buffer) == 3, "SRV GetResource retains the same buffer");
                            if (retained) reinterpret_cast<Count>(vt(retained)[2])(retained);
                            reinterpret_cast<Count>(vt(view)[2])(view);
                        }
                        check(create_srv(device, buffer, nullptr, &view) == S_OK && view, "null SRV descriptor uses the full structured range");
                        if (view) reinterpret_cast<Count>(vt(view)[2])(view);
                        reinterpret_cast<Count>(vt(buffer)[2])(buffer);
                    }
                    graphics_force_header = false;
                    set_hooks(nullptr, nullptr);
                    check(graphics_hook_allocations != 0 && graphics_hook_allocations == graphics_hook_frees && !graphics_hook_wrong_allocator,
                          "graphics storage and counters preserve system allocator ID through actual title hooks");
                }
                {
                    using CreateState = HRESULT (STDMETHODCALLTYPE*)(void*, const D3D11_RASTERIZER_DESC*, void**);
                    const auto factory = reinterpret_cast<CreateState>(vt(device)[22]);
                    D3D11_RASTERIZER_DESC desc{};
                    desc.FillMode = D3D11_FILL_WIREFRAME; desc.CullMode = D3D11_CULL_FRONT;
                    desc.FrontCounterClockwise = TRUE; desc.DepthBias = -5; desc.DepthBiasClamp = 1.5f;
                    desc.SlopeScaledDepthBias = -2.f; desc.DepthClipEnable = TRUE; desc.ScissorEnable = TRUE;
                    desc.MultisampleEnable = TRUE; desc.AntialiasedLineEnable = TRUE;
                    void* state = device;
                    check(factory(device, nullptr, &state) == E_INVALIDARG && !state, "rasterizer description required");
                    check(factory(device, &desc, nullptr) == S_FALSE, "rasterizer validation only");
                    check(factory(device, &desc, &state) == S_OK && state, "rasterizer state created");
                    if (state) {
                        using GetDesc = void (STDMETHODCALLTYPE*)(void*, D3D11_RASTERIZER_DESC*);
                        D3D11_RASTERIZER_DESC actual{};
                        reinterpret_cast<GetDesc>(vt(state)[8])(state, &actual);
                        check(actual.FillMode == desc.FillMode && actual.CullMode == desc.CullMode && actual.FrontCounterClockwise &&
                            actual.DepthBias == -5 && actual.DepthBiasClamp == 1.5f && actual.SlopeScaledDepthBias == -2.f &&
                            actual.ScissorEnable && actual.DepthClipEnable && actual.MultisampleEnable && actual.AntialiasedLineEnable,
                            "rasterizer preserves culling, winding, bias, scissor, clipping and line/MSAA modes");
                        void* duplicate = nullptr;
                        check(factory(device, &desc, &duplicate) == S_OK && duplicate == state && refs(state) == 2,
                              "identical rasterizer shares live state");
                        if (duplicate) reinterpret_cast<Count>(vt(duplicate)[2])(duplicate);
                        check(reinterpret_cast<Count>(vt(state)[2])(state) == 0, "rasterizer cache entry retires");
                    }
                    desc.FillMode = static_cast<D3D11_FILL_MODE>(4);
                    check(factory(device, &desc, &state) == E_INVALIDARG && !state, "unsupported fill mode rejected");
                }
                {
                    using CreateState = HRESULT (STDMETHODCALLTYPE*)(void*, const D3D11_BLEND_DESC*, void**);
                    const auto factory = reinterpret_cast<CreateState>(vt(device)[20]);
                    D3D11_BLEND_DESC desc{};
                    desc.IndependentBlendEnable = TRUE; desc.AlphaToCoverageEnable = TRUE;
                    for (UINT i = 0; i < 8; ++i) {
                        auto& r = desc.RenderTarget[i];
                        r.BlendEnable = TRUE; r.SrcBlend = D3D11_BLEND_SRC_ALPHA; r.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
                        r.BlendOp = D3D11_BLEND_OP_ADD; r.SrcBlendAlpha = D3D11_BLEND_ONE; r.DestBlendAlpha = D3D11_BLEND_ZERO;
                        r.BlendOpAlpha = D3D11_BLEND_OP_REV_SUBTRACT; r.RenderTargetWriteMask = static_cast<UINT8>(i + 1);
                    }
                    void* state = device;
                    check(factory(device, nullptr, &state) == E_INVALIDARG && !state, "blend description required");
                    check(factory(device, &desc, nullptr) == S_FALSE, "blend validation only");
                    check(factory(device, &desc, &state) == S_OK && state, "independent blend state created");
                    if (state) {
                        using GetDesc = void (STDMETHODCALLTYPE*)(void*, D3D11_BLEND_DESC*);
                        D3D11_BLEND_DESC actual{};
                        reinterpret_cast<GetDesc>(vt(state)[8])(state, &actual);
                        bool all = actual.IndependentBlendEnable && actual.AlphaToCoverageEnable;
                        for (UINT i = 0; i < 8; ++i) all = all && actual.RenderTarget[i].RenderTargetWriteMask == i + 1 &&
                            actual.RenderTarget[i].DestBlend == D3D11_BLEND_INV_SRC_ALPHA && actual.RenderTarget[i].BlendOpAlpha == D3D11_BLEND_OP_REV_SUBTRACT;
                        check(all, "all blend targets retain color, alpha and write mask");
                        void* duplicate = nullptr;
                        check(factory(device, &desc, &duplicate) == S_OK && duplicate == state && refs(state) == 2,
                              "identical live blend state shares identity");
                        if (duplicate) reinterpret_cast<Count>(vt(duplicate)[2])(duplicate);
                        check(reinterpret_cast<Count>(vt(state)[2])(state) == 0, "blend cache entry retires");
                    }
                    desc.RenderTarget[7].SrcBlendAlpha = D3D11_BLEND_SRC_COLOR;
                    check(factory(device, &desc, &state) == E_INVALIDARG && !state, "color factor in alpha blend rejected");
                    desc.RenderTarget[7].SrcBlendAlpha = D3D11_BLEND_ONE; desc.RenderTarget[7].RenderTargetWriteMask = 0x80;
                    check(factory(device, &desc, &state) == E_INVALIDARG && !state, "invalid blend write mask rejected");
                }
                {
                    using CreateSampler = HRESULT (STDMETHODCALLTYPE*)(void*, const D3D12_SAMPLER_DESC*, void**);
                    const auto factory = reinterpret_cast<CreateSampler>(vt(device)[23]);
                    D3D12_SAMPLER_DESC desc{};
                    desc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
                    desc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP; desc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
                    desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP; desc.MaxAnisotropy = 16;
                    desc.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
                    desc.MipLODBias = -1.125f; desc.MinLOD = .125f; desc.MaxLOD = D3D12_FLOAT32_MAX;
                    void* sampler = device;
                    check(factory(device, nullptr, &sampler) == E_INVALIDARG && !sampler, "sampler description required");
                    check(factory(device, &desc, nullptr) == S_FALSE, "sampler validation only");
                    check(factory(device, &desc, &sampler) == S_OK && sampler, "native sampler descriptor created");
                    if (sampler) {
                        check((reinterpret_cast<std::uintptr_t>(sampler) & 15) == 0 && refs(sampler) == 1 && refs(device) == 1,
                              "sampler old prefix, alignment and internal ownership");
                        using GetX = void (STDMETHODCALLTYPE*)(void*, UINT32*);
                        std::array<UINT32, 5> packed{0, 0, 0, 0, 0x12345678};
                        reinterpret_cast<GetX>(vt(sampler)[9])(sampler, packed.data());
                        check(packed[0] == 0x3088 && packed[1] == 0xfff020 && packed[2] == 0x74003ee0 && !packed[3] && packed[4] == 0x12345678,
                              "sampler GCN words encode address, negative bias and fixed LOD; GetDescX writes exactly sixteen bytes");
                        check(!std::memcmp(static_cast<char*>(sampler) + 32, packed.data(), 16), "inline sampler SRD matches GetDescX");
                        using GetDesc = void (STDMETHODCALLTYPE*)(void*, D3D12_SAMPLER_DESC*);
                        D3D12_SAMPLER_DESC actual{};
                        reinterpret_cast<GetDesc>(vt(sampler)[8])(sampler, &actual);
                        check(actual.MaxLOD == desc.MaxLOD && actual.MaxAnisotropy == 16 && actual.MipLODBias == -1.125f,
                              "GetDesc preserves request before hardware quantization");
                        void* duplicate = nullptr;
                        check(factory(device, &desc, &duplicate) == S_OK && duplicate == sampler && refs(sampler) == 2,
                              "identical sampler shares live state and descriptor slot");
                        if (duplicate) reinterpret_cast<Count>(vt(duplicate)[2])(duplicate);
                        check(reinterpret_cast<Count>(vt(sampler)[2])(sampler) == 0, "sampler descriptor slot retired");
                    }
                    desc.Filter = D3D12_FILTER_MIN_LINEAR_MAG_MIP_POINT;
                    check(factory(device, &desc, &sampler) == S_OK && sampler, "asymmetric sampler filters supported");
                    if (sampler) {
                        UINT32 word = 0;
                        std::memcpy(&word, static_cast<char*>(sampler) + 40, 4);
                        check((word & 0xf00000) == 0x400000, "min and mag filter fields are not interchanged");
                        reinterpret_cast<Count>(vt(sampler)[2])(sampler);
                    }
                    desc.Filter = static_cast<D3D12_FILTER>(3);
                    check(factory(device, &desc, &sampler) == E_INVALIDARG && !sampler, "invalid sampler filter rejected");
                    desc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT; desc.BorderColor[0] = 1;
                    check(factory(device, &desc, &sampler) == DXGI_ERROR_UNSUPPORTED && !sampler, "custom border table remains explicit gap");
                }
                {
                    using CreateState = HRESULT (STDMETHODCALLTYPE*)(void*, const D3D12_DEPTH_STENCIL_DESC*, void**);
                    const auto factory = reinterpret_cast<CreateState>(vt(device)[21]);
                    D3D12_DEPTH_STENCIL_DESC desc{};
                    desc.DepthEnable = TRUE; desc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
                    desc.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
                    desc.StencilReadMask = 0x7f; desc.StencilWriteMask = 0xf0; desc.StencilEnable = TRUE;
                    desc.FrontFace = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_INCR, D3D12_STENCIL_OP_REPLACE, D3D12_COMPARISON_FUNC_EQUAL};
                    desc.BackFace = {D3D12_STENCIL_OP_ZERO, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_DECR, D3D12_COMPARISON_FUNC_NOT_EQUAL};
                    void* state = device;
                    check(factory(device, nullptr, &state) == E_INVALIDARG && !state, "depth description required");
                    check(factory(device, &desc, nullptr) == S_FALSE, "depth state validation only");
                    check(factory(device, &desc, &state) == S_OK && state, "depth stencil state created");
                    if (state) {
                        void* duplicate = nullptr;
                        check(factory(device, &desc, &duplicate) == S_OK && duplicate == state && refs(state) == 2,
                              "identical live depth states share identity");
                        using GetDesc = void (STDMETHODCALLTYPE*)(void*, D3D12_DEPTH_STENCIL_DESC*);
                        D3D12_DEPTH_STENCIL_DESC actual{};
                        reinterpret_cast<GetDesc>(vt(state)[8])(state, &actual);
                        check(actual.DepthEnable && actual.StencilEnable && actual.DepthFunc == desc.DepthFunc &&
                            actual.FrontFace.StencilPassOp == desc.FrontFace.StencilPassOp && actual.BackFace.StencilFunc == desc.BackFace.StencilFunc &&
                            actual.StencilReadMask == 0x7f && actual.StencilWriteMask == 0xf0, "depth and both stencil faces preserved");
                        if (duplicate) reinterpret_cast<Count>(vt(duplicate)[2])(duplicate);
                        check(reinterpret_cast<Count>(vt(state)[2])(state) == 0, "depth state cache entry retires with final reference");
                    }
                    desc.BackFace.StencilFunc = static_cast<D3D12_COMPARISON_FUNC>(9);
                    check(factory(device, &desc, &state) == E_INVALIDARG && !state, "invalid stencil comparison rejected");
                }
                {
                    const HMODULE compiler = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
                    const auto compile = compiler ? reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
                    check(compile != nullptr, "native compiler for owned shader fixtures");
                    if (compile) {
                        using CreateShader = HRESULT (STDMETHODCALLTYPE*)(void*, const void*, SIZE_T, void*, void**);
                        const char* source[]{"float4 main(float4 p:POSITION):SV_Position { return p; }",
                            "float4 main():SV_Target { return float4(1,0,0,1); }", "[numthreads(1,1,1)] void main() {}",
                            "struct V{float4 p:SV_Position;}; [maxvertexcount(3)] void main(triangle V input[3],inout TriangleStream<V> output)"
                            "{output.Append(input[0]);output.Append(input[1]);output.Append(input[2]);}"};
                        const char* profile[]{"vs_5_0", "ps_5_0", "cs_5_0", "gs_5_0"};
                        const unsigned slots[]{12, 15, 18, 13};
                        for (unsigned i = 0; i != 4; ++i) {
                            ID3DBlob* code = nullptr;
                            check(SUCCEEDED(compile(source[i], std::strlen(source[i]), nullptr, nullptr, nullptr,
                                "main", profile[i], 0, 0, &code, nullptr)) && code, "compile owned shader fixture");
                            if (!code) continue;
                            if (i == 0) {
                                using CreateLayout = HRESULT (STDMETHODCALLTYPE*)(void*, const D3D12_INPUT_ELEMENT_DESC*, UINT, const void*, SIZE_T, void**);
                                const auto create_layout = reinterpret_cast<CreateLayout>(vt(device)[11]);
                                char semantic[] = "position";
                                D3D12_INPUT_ELEMENT_DESC element{semantic, 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,
                                    D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
                                void* layout = device;
                                check(create_layout(device, &element, 1, code->GetBufferPointer(), code->GetBufferSize(), nullptr) == S_FALSE,
                                      "matching input signature validates without object");
                                element.SemanticIndex = 1;
                                check(FAILED(create_layout(device, &element, 1, code->GetBufferPointer(), code->GetBufferSize(), &layout)) && !layout,
                                      "missing shader semantic rejected");
                                element.SemanticIndex = 0; element.Format = DXGI_FORMAT_BC1_UNORM;
                                check(FAILED(create_layout(device, &element, 1, code->GetBufferPointer(), code->GetBufferSize(), &layout)) && !layout,
                                      "non-vertex input format rejected by device support");
                                element.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; element.InstanceDataStepRate = 1;
                                check(FAILED(create_layout(device, &element, 1, code->GetBufferPointer(), code->GetBufferSize(), &layout)) && !layout,
                                      "per-vertex element cannot have instance rate");
                                element.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA;
                                check(create_layout(device, &element, 1, code->GetBufferPointer(), code->GetBufferSize(), &layout) == S_OK && layout,
                                      "instanced input layout created from real signature");
                                if (layout) {
                                    check(refs(layout) == 1 && refs(device) == 1, "layout owns internal device dependency");
                                    semantic[0] = 'X'; // Creation must own names after the caller returns.
                                    check(reinterpret_cast<Count>(vt(layout)[2])(layout) == 0, "input layout released");
                                }
                            }
                            const auto factory = reinterpret_cast<CreateShader>(vt(device)[slots[i]]);
                            void* shader = device;
                            check(FAILED(factory(device, code->GetBufferPointer(), 16, nullptr, &shader)) && !shader,
                                  "truncated shader rejected and output cleared");
                            check(factory(device, code->GetBufferPointer(), code->GetBufferSize(), device, &shader) == DXGI_ERROR_UNSUPPORTED && !shader,
                                  "unimplemented dynamic linkage rejected");
                            check(FAILED(reinterpret_cast<CreateShader>(vt(device)[slots[(i + 1) % 4]])(device,
                                code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader)) && !shader, "wrong shader stage rejected");
                            check(factory(device, code->GetBufferPointer(), code->GetBufferSize(), nullptr, nullptr) == S_FALSE,
                                  "shader validation-only does not create an object");
                            check(factory(device, code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader) == S_OK && shader,
                                  "validated DXBC shader module created");
                            if (shader) {
                                check(refs(shader) == 1 && refs(device) == 1, "shader owns internal device dependency");
                                if (i == 3) {
                                    UINT native_flags = 1;
                                    std::memcpy(&native_flags, static_cast<char*>(shader) + 32, sizeof native_flags);
                                    check(native_flags == 0, "original geometry shader public flag at plus thirty-two stays zero");
                                    using QueryShader = HRESULT (STDMETHODCALLTYPE*)(void*, REFIID, void**);
                                    void* queried = nullptr;
                                    check(reinterpret_cast<QueryShader>(vt(shader)[0])(shader, __uuidof(ID3D11GeometryShader), &queried) == S_OK && queried == shader,
                                          "geometry shader IID preserves original identity");
                                    if (queried) reinterpret_cast<Count>(vt(queried)[2])(queried);
                                }
                                using Get = void (STDMETHODCALLTYPE*)(void*, void**);
                                void* owner = nullptr;
                                reinterpret_cast<Get>(vt(shader)[3])(shader, &owner);
                                check(owner == device && refs(device) == 2, "shader GetDevice retains original identity");
                                reinterpret_cast<Count>(vt(owner)[2])(owner);
                                check(reinterpret_cast<Count>(vt(shader)[2])(shader) == 0, "shader module released");
                            }
                            code->Release();
                        }
                    }
                    if (compiler) FreeLibrary(compiler);
                }
                constexpr GUID performance{0x88671610, 0x712E, 0x4F1E, {0x84, 0xAB, 0x01, 0xB5, 0x94, 0x8B, 0xD3, 0x73}};
                using PerformanceQuery = HRESULT (STDMETHODCALLTYPE*)(void*, REFIID, void**);
                void* performance_device = nullptr;
                check(reinterpret_cast<PerformanceQuery>(vt(device)[0])(device, performance, &performance_device) == S_OK &&
                      performance_device == device && refs(device) == 2, "legacy performance device shares identity and ownership");
                if (performance_device) reinterpret_cast<Count>(vt(performance_device)[2])(performance_device);
                using GetDevice = void (STDMETHODCALLTYPE*)(void*, void**);
                void* context_device = nullptr;
                reinterpret_cast<GetDevice>(vt(context)[3])(context, &context_device);
                check(context_device == device && refs(device) == 2, "context GetDevice returns original identity with reference");
                reinterpret_cast<Count>(vt(context_device)[2])(context_device);
                check(reinterpret_cast<Flags>(vt(device)[39])(device) == p.flags, "legacy creation flags preserved");
                void* immediate = nullptr;
                reinterpret_cast<GetDevice>(vt(device)[41])(device, &immediate);
                check(immediate == context && refs(context) == 2, "immediate context identity and reference");
                reinterpret_cast<Count>(vt(context)[2])(context);
                check(reinterpret_cast<Count>(vt(immediate)[2])(immediate) == 0 && refs(device) == 1,
                      "zero context count preserves device-owned context storage");
                reinterpret_cast<GetDevice>(vt(device)[55])(device, &immediate);
                check(immediate == context && refs(context) == 1, "context identity survives release and reacquisition");
                constexpr GUID dxgi1{0x77DB970F, 0x6276, 0x48BA, {0xBA, 0x28, 0x07, 0x01, 0x43, 0xB4, 0x39, 0x2C}};
                constexpr GUID factory2{0x50C83A1C, 0xE072, 0x4C48, {0x87, 0xB0, 0x36, 0x30, 0xFA, 0x36, 0xA6, 0xD0}};
                using Query = HRESULT (STDMETHODCALLTYPE*)(void*, REFIID, void**);
                using Adapter = HRESULT (STDMETHODCALLTYPE*)(void*, void**);
                void* dxgi = nullptr;
                void* adapter = nullptr;
                void* factory = nullptr;
                check(reinterpret_cast<Query>(vt(device)[0])(device, dxgi1, &dxgi) == S_OK && dxgi,
                      "legacy DXGI query");
                if (dxgi) {
                    check(reinterpret_cast<Adapter>(vt(dxgi)[8])(dxgi, &adapter) == S_OK && adapter,
                          "legacy GetAdapter is slot eight");
                    if (adapter) {
                        using Outputs = HRESULT (STDMETHODCALLTYPE*)(void*, UINT, IDXGIOutput**);
                        const auto outputs = reinterpret_cast<Outputs>(vt(adapter)[8]);
                        IDXGIOutput* output = nullptr;
                        check(outputs(adapter, 0, nullptr) == E_POINTER, "legacy output pointer required");
                        check(outputs(adapter, UINT_MAX, &output) == DXGI_ERROR_NOT_FOUND && !output,
                              "missing output preserves native not-found error");
                        const HRESULT enumerated = outputs(adapter, 0, &output);
                        check(enumerated == DXGI_ERROR_NOT_FOUND || (enumerated == S_OK && output),
                              "legacy output enumeration reflects actual attached outputs");
                        if (output) {
                            DXGI_OUTPUT_DESC desc{};
                            check(output->GetDesc(&desc) == S_OK && desc.DeviceName[0], "native output COM contract");
                            output->Release();
                        }
                        check(reinterpret_cast<Query>(vt(adapter)[7])(adapter, factory2, &factory) == S_OK && factory,
                              "legacy GetParent is slot seven");
                        if (factory) {
                            using CreateChain = HRESULT (STDMETHODCALLTYPE*)(void*, void*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, void*, void**);
                            const auto create_chain = reinterpret_cast<CreateChain>(vt(factory)[17]);
                            void* chain = reinterpret_cast<void*>(1);
                            check(create_chain(factory, device, nullptr, nullptr, nullptr, &chain) == DXGI_ERROR_INVALID_CALL,
                                  "legacy factory slot seventeen validates the native swapchain ABI");
                            DXGI_SWAP_CHAIN_DESC1 desc{};
                            desc.Flags = 0x100000;
                            check(create_chain(factory, device, nullptr, &desc, nullptr, &chain) == DXGI_ERROR_UNSUPPORTED && !chain,
                                  "unimplemented Xbox display flags cannot be silently removed from the desktop descriptor");
                            desc.Flags = 0x200000;
                            check(create_chain(factory, device, nullptr, &desc, nullptr, &chain) == DXGI_ERROR_INVALID_CALL && !chain,
                                  "translated display profile still requires a real owned CoreWindow");
                            reinterpret_cast<Count>(vt(factory)[2])(factory);
                        }
                        reinterpret_cast<Count>(vt(adapter)[2])(adapter);
                    }
                    reinterpret_cast<Count>(vt(dxgi)[2])(dxgi);
                }
                struct DmaDesc { UINT flags, ring, segment; } desc{1, 65536, 0};
                using CreateDma = HRESULT (STDMETHODCALLTYPE*)(void*, const DmaDesc*, void**);
                using Fence = UINT64 (STDMETHODCALLTYPE*)(void*, UINT);
                using Wait = void (STDMETHODCALLTYPE*)(void*, UINT, UINT64);
                using Pending = BOOL (STDMETHODCALLTYPE*)(void*, UINT64);
                using Submit = HRESULT (STDMETHODCALLTYPE*)(void*);
                const auto create_dma = reinterpret_cast<CreateDma>(vt(device)[59]);
                const auto pending = reinterpret_cast<Pending>(vt(device)[60]);
                void* producer = nullptr;
                void* consumer = nullptr;
                check(create_dma(device, nullptr, &producer) == E_INVALIDARG && !producer, "DMA description required");
                desc.flags = 3;
                check(create_dma(device, &desc, &producer) == DXGI_ERROR_UNSUPPORTED && !producer, "unknown DMA selector rejected");
                desc.flags = 1;
                check(create_dma(device, &desc, &producer) == S_OK && producer, "first native DMA copy queue");
                void* duplicate = nullptr;
                check(create_dma(device, &desc, &duplicate) == DXGI_ERROR_INVALID_CALL && !duplicate,
                      "duplicate live engine binding rejected");
                desc.flags = 2;
                check(create_dma(device, &desc, &consumer) == S_OK && consumer, "second native DMA copy queue");
                if (producer && consumer) {
                    using Type = UINT (STDMETHODCALLTYPE*)(void*);
                    check(reinterpret_cast<Type>(vt(producer)[8])(producer) == 2, "original DMA context type");
                    const UINT64 produced = reinterpret_cast<Fence>(vt(producer)[16])(producer, 1);
                    check(produced && pending(device, produced), "deferred DMA fence remains pending before Submit");
                    reinterpret_cast<Wait>(vt(consumer)[17])(consumer, 0, produced);
                    const UINT64 consumed = reinterpret_cast<Fence>(vt(consumer)[16])(consumer, 0);
                    check(consumed != produced && pending(device, consumed), "consumer cannot pass an unsubmitted producer fence");
                    check(reinterpret_cast<Submit>(vt(producer)[18])(producer) == S_OK, "producer submits real queue signals");
                    const auto deadline = GetTickCount64() + 3000;
                    while ((pending(device, produced) || pending(device, consumed)) && GetTickCount64() < deadline) Sleep(1);
                    check(!pending(device, produced) && !pending(device, consumed), "separate DMA queues preserve cross-fence order");
                    check(refs(device) == 1, "DMA ownership preserves public device count");
                }
                if (consumer) reinterpret_cast<Count>(vt(consumer)[2])(consumer);
                if (producer) reinterpret_cast<Count>(vt(producer)[2])(producer);
                producer = nullptr;
                desc.flags = 1;
                check(create_dma(device, &desc, &producer) == S_OK && producer, "completed engine can be rebound");
                if (producer) {
                    const UINT64 final = reinterpret_cast<Fence>(vt(producer)[16])(producer, 1);
                    reinterpret_cast<Count>(vt(producer)[2])(producer);
                    check(!pending(device, final), "last DMA release drains translated fence work");
                }
                using CreateDeferred = HRESULT (STDMETHODCALLTYPE*)(void*, UINT, void**);
                const auto create_deferred = reinterpret_cast<CreateDeferred>(vt(device)[27]);
                void* deferred = nullptr;
                check(create_deferred(device, 0, nullptr) == E_POINTER, "deferred context output required");
                check(create_deferred(device, 0x80000, &deferred) == DXGI_ERROR_UNSUPPORTED && !deferred,
                      "unimplemented deferred context flags rejected");
                check(create_deferred(device, 0, &deferred) == S_OK && deferred && deferred != context,
                      "deferred context has independent identity");
                if (deferred) {
                    using State = UINT (STDMETHODCALLTYPE*)(void*);
                    const auto data = static_cast<unsigned char*>(deferred);
                    check((reinterpret_cast<std::uintptr_t>(deferred) & 63) == 0 &&
                          reinterpret_cast<const void*>(vt(deferred)) == data + 0x3d0 && data[0x390] == 1,
                          "old context contains its inline function table and out-of-line dispatch marker");
                    check(reinterpret_cast<State>(vt(deferred)[112])(deferred) == 1 &&
                          reinterpret_cast<State>(vt(deferred)[113])(deferred) == 0,
                          "deferred type and creation flags match the requested contract");
                    UINT64 domain = 0x1234, shader_flags = 0x100;
                    UINT32 dirty = 0x80;
                    std::memcpy(data + 0x1c8, &domain, sizeof domain);
                    std::memcpy(data + 0x168, &dirty, sizeof dirty);
                    std::memcpy(data + 0x218, &shader_flags, sizeof shader_flags);
                    using Shader = void (STDMETHODCALLTYPE*)(void*, void*);
                    reinterpret_cast<Shader>(vt(deferred)[64])(deferred, nullptr);
                    std::memcpy(&domain, data + 0x1c8, sizeof domain);
                    std::memcpy(&dirty, data + 0x168, sizeof dirty);
                    std::memcpy(&shader_flags, data + 0x218, sizeof shader_flags);
                    check(!domain && dirty == 0x2480 && shader_flags == 0x104,
                          "domain unbind preserves unrelated public state and marks the native dirty bits");
                    UINT64 hull = 0x5678;
                    std::memcpy(data + 0x1c0, &hull, sizeof hull);
                    reinterpret_cast<Shader>(vt(deferred)[60])(deferred, nullptr);
                    std::memcpy(&hull, data + 0x1c0, sizeof hull);
                    std::memcpy(&dirty, data + 0x168, sizeof dirty);
                    std::memcpy(&shader_flags, data + 0x218, sizeof shader_flags);
                    check(!hull && dirty == 0x2680 && shader_flags == 0x106,
                          "hull unbind preserves domain state and native dirty bits");
                    UINT64 geometry[2]{0x1234, 0x5678};
                    std::memcpy(data + 0x1d0, geometry, sizeof geometry);
                    reinterpret_cast<Shader>(vt(deferred)[24])(deferred, nullptr);
                    std::memcpy(geometry, data + 0x1d0, sizeof geometry);
                    std::memcpy(&dirty, data + 0x168, sizeof dirty);
                    std::memcpy(&shader_flags, data + 0x218, sizeof shader_flags);
                    check(!geometry[0] && !geometry[1] && dirty == 0x3e80 && shader_flags == 0x10e,
                          "geometry unbind clears both native shader-data bindings without losing tessellation state");
                    static unsigned command_slot = 0;
                    set_d3d11x_trap_handler([](const char*, unsigned slot) -> std::uintptr_t {
                        command_slot = slot; return static_cast<UINT32>(E_NOTIMPL);
                    });
                    using Finish = HRESULT (STDMETHODCALLTYPE*)(void*, BOOL, void**);
                    void* command_list = nullptr;
                    check(reinterpret_cast<Finish>(vt(deferred)[114])(deferred, FALSE, &command_list) == E_NOTIMPL && command_slot == 114,
                          "native FinishCommandList slot stays unsupported, never aliases a successful flags getter");
                    set_d3d11x_trap_handler(nullptr);
                    void* owned = nullptr;
                    reinterpret_cast<GetDevice>(vt(deferred)[3])(deferred, &owned);
                    check(owned == device && refs(device) == 2, "inline dispatch GetDevice retains the same device");
                    reinterpret_cast<Count>(vt(owned)[2])(owned);
                    check(reinterpret_cast<Count>(vt(deferred)[2])(deferred) == 0 && refs(device) == 1,
                          "deferred context releases internal device ownership");
                }
                ++refs(context);
                check(reinterpret_cast<Count>(vt(context)[2])(context) == 1, "legacy inline count honoured");
                check(reinterpret_cast<Count>(vt(device)[2])(device) == 0, "legacy public device count reaches zero while context lives");
                check(reinterpret_cast<Count>(vt(context)[2])(context) == 0, "legacy queue retired and objects released");
            }
        }
        check(FAILED(bind_d3d11x_abi(L"xone-absent-graphics-version.dll")) &&
              !resolve_title_import({"d3d11_x.dll", "D3D11XCreateDeviceX", 0}), "failed rebinding clears old ABI");
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
                   "</mx:Extension></Extensions>"
                   "<Applications><Application Id=\"App\" Executable=\"t.exe\" EntryPoint=\"Title.App\"/></Applications>"
                   "</Package>";
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
        const HMODULE legacy_cx = GetModuleHandleW(L"vccorlib110.dll");
        if (legacy_cx) {
            check(hook_winrt_activation(legacy_cx), "legacy CX activation import hooked");
            check(hook_winrt_activation(legacy_cx), "legacy CX activation hook is idempotent");
            using LegacyFactory = HRESULT (*)(const wchar_t*, const GUID&, void**);
            const auto legacy_factory = reinterpret_cast<LegacyFactory>(resolve_host_module({
                "vccorlib110.dll", "?GetActivationFactoryByPCWSTR@@YAJPEAXAEAVGuid@Platform@@PEAPEAX@Z", 0}));
            check(legacy_factory != nullptr, "legacy CX factory export resolved");
            if (legacy_factory) {
                Statics* via_cx = nullptr;
                check(legacy_factory(L"Windows.Xbox.Services.XboxLiveConfiguration", __uuidof(Statics),
                                     reinterpret_cast<void**>(&via_cx)) == S_OK && via_cx,
                      "legacy CX reaches the title activation boundary");
                if (via_cx) via_cx->Release();
            }
        }
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
        void* missing = &missing;
        check(activate(L"Windows.Xbox.Input.Gamepad", __uuidof(IActivationFactory), &missing) == REGDB_E_CLASSNOTREG && !missing,
              "unimplemented Xbox class keeps the real host error");
        {
            constexpr GUID iid_transfer_statics{0xd6cf4cae, 0x9eb1, 0x4cbe, {0xb4,0x26,0x7e,0x75,0xd4,0x29,0xf8,0x46}};
            constexpr GUID iid_transfer{0xc2d4b94b, 0x0413, 0x4380, {0xb2,0xf2,0xab,0x98,0x72,0xd0,0x82,0x85}};
            IInspectable* transfer = nullptr;
            check(activate(L"Windows.Xbox.Management.Deployment.PackageTransferManager", iid_transfer_statics,
                reinterpret_cast<void**>(&transfer)) == S_OK && transfer, "package transfer statics available");
            if (transfer) {
                using Current = HRESULT(STDMETHODCALLTYPE*)(void*, IInspectable**);
                const auto get_current = reinterpret_cast<Current>((*reinterpret_cast<void***>(transfer))[6]);
                check(get_current(transfer, nullptr) == E_POINTER, "transfer Current validates its output");
                set_title_package_root({});
                IInspectable* instance = reinterpret_cast<IInspectable*>(1);
                check(FAILED(get_current(transfer, &instance)) && !instance, "transfer Current requires an active package");
                set_title_package_root(package);
                check(get_current(transfer, &instance) == S_OK && instance, "transfer Current binds the active package");
                if (instance) {
                    using Installed = HRESULT(STDMETHODCALLTYPE*)(void*, UINT32, boolean*);
                    const auto installed = reinterpret_cast<Installed>((*reinterpret_cast<void***>(instance))[7]);
                    check(installed(instance, 69, nullptr) == E_POINTER, "chunk query validates its output");
                    boolean present = true;
                    check(FAILED(installed(instance, 69, &present)) && !present, "missing catalog is an error, never installed");
                    const auto catalog = package + L"\\hash.manifest";
                    {
                        std::ofstream out(catalog, std::ios::binary);
                        const char truncated = 0;
                        out.write(&truncated, 1);
                    }
                    present = true;
                    check(installed(instance, 69, &present) == HRESULT_FROM_WIN32(ERROR_INVALID_DATA) && !present,
                          "malformed local catalog rejected");
                    {
                        std::ofstream out(catalog, std::ios::binary | std::ios::trunc);
                        const unsigned char record[12]{0,0,0,0,0,0,0,0,69,0,0,0};
                        out.write(reinterpret_cast<const char*>(record), sizeof record);
                    }
                    check(installed(instance, 69, &present) == S_OK && present, "local catalog supplies installed chunk");
                    check(installed(instance, 70, &present) == S_OK && !present, "absent chunk is not invented");
                    check(installed(instance, 0xffffffff, &present) == S_OK && !present, "reserved chunk never installed");
                    set_title_package_root(package);
                    DeleteFileW(catalog.c_str());
                    check(FAILED(installed(instance, 69, &present)), "package rebinding invalidates cached chunk state");
                    IInspectable* same = nullptr;
                    check(instance->QueryInterface(iid_transfer, reinterpret_cast<void**>(&same)) == S_OK && same == instance,
                          "transfer manager has the declared default interface");
                    if (same) same->Release();
                    instance->Release();
                }
                transfer->Release();
            }
        }
        // Windows.Xbox.System.User statics: slots 12/13 (UserRemoved add/remove) observed from the title.
        constexpr GUID iid_user_statics{0xB4F23A93, 0x7FC6, 0x4A50, {0xB1, 0x53, 0x95, 0x00, 0x16, 0x38, 0x9E, 0x50}};
        IActivationFactory* user = nullptr;
        check(activate(L"Windows.Xbox.System.User", __uuidof(IActivationFactory), reinterpret_cast<void**>(&user)) == S_OK && user,
              "User served by title layer");
        IInspectable* user_statics = nullptr;
        if (user) {
            check(user->QueryInterface(iid_user_statics, reinterpret_cast<void**>(&user_statics)) == S_OK && user_statics,
                  "User factory answers IUserStatics");
        }
        if (user_statics && host) {
            using Add = HRESULT(STDMETHODCALLTYPE*)(void*, IUnknown*, std::int64_t*);
            using Remove = HRESULT(STDMETHODCALLTYPE*)(void*, std::int64_t);
            auto** vt = *reinterpret_cast<void***>(user_statics);
            HSTRING value = nullptr;
            check(user_statics->GetRuntimeClassName(&value) == S_OK && text(value) == L"Windows.Xbox.System.User",
                  "User runtime class name");
            std::int64_t token = 0;
            check(reinterpret_cast<Add>(vt[12])(user_statics, host, &token) == S_OK && token &&
                  xboxone::era::xbox_user_removed_handlers() == 1, "UserRemoved handler stored");
            check(reinterpret_cast<Add>(vt[12])(user_statics, nullptr, &token) == E_INVALIDARG, "null handler rejected");
            check(reinterpret_cast<Remove>(vt[13])(user_statics, token) == S_OK &&
                  xboxone::era::xbox_user_removed_handlers() == 0, "UserRemoved handler removed");
        }
        // DownloadableContentPackageManager: no DLC on this host, FindPackages is empty.
        constexpr GUID vector_view_string{0x2f13c006, 0xa03a, 0x5f69, {0xb0, 0x90, 0x75, 0xa4, 0x3e, 0x33, 0x42, 0x3e}};
        check(xboxone::era::winrt_pinterface_iid("pinterface({bbe1fa4c-b0e3-4583-baef-1f1b2e483e56};string)") ==
                  vector_view_string, "pinterface IID of IVectorView<String>");
        constexpr GUID iid_dlc_manager{0x971FEAAF, 0x98F8, 0x4038, {0xB8, 0xC1, 0x3A, 0x5F, 0x1A, 0x04, 0x3B, 0x97}};
        IActivationFactory* dlc = nullptr;
        check(activate(L"Windows.Xbox.Management.Deployment.DownloadableContentPackageManager", __uuidof(IActivationFactory),
                       reinterpret_cast<void**>(&dlc)) == S_OK && dlc, "DLC manager served by title layer");
        IInspectable* dlc_instance = nullptr;
        IInspectable* manager = nullptr;
        if (dlc) check(dlc->ActivateInstance(&dlc_instance) == S_OK && dlc_instance, "DLC manager instance");
        if (dlc_instance)
            check(dlc_instance->QueryInterface(iid_dlc_manager, reinterpret_cast<void**>(&manager)) == S_OK && manager,
                  "DLC manager default interface");
        if (manager && host) {
            auto** vt = *reinterpret_cast<void***>(manager);
            using Find = HRESULT(STDMETHODCALLTYPE*)(void*, INT32, IInspectable**);
            using Size = HRESULT(STDMETHODCALLTYPE*)(void*, UINT32*);
            using GetAt = HRESULT(STDMETHODCALLTYPE*)(void*, UINT32, void**);
            using Add = HRESULT(STDMETHODCALLTYPE*)(void*, IUnknown*, std::int64_t*);
            using Remove = HRESULT(STDMETHODCALLTYPE*)(void*, std::int64_t);
            IInspectable* packages = nullptr;
            check(reinterpret_cast<Find>(vt[6])(manager, 0, &packages) == S_OK && packages, "FindPackages returns a view");
            if (packages) {
                auto** pv = *reinterpret_cast<void***>(packages);
                UINT32 size = 7;
                void* item = &item;
                check(reinterpret_cast<Size>(pv[7])(packages, &size) == S_OK && size == 0, "no DLC packages");
                check(reinterpret_cast<GetAt>(pv[6])(packages, 0, &item) == E_BOUNDS && !item, "empty view is bounded");
                IUnknown* typed = nullptr;
                check(packages->QueryInterface(xboxone::era::winrt_pinterface_iid(xboxone::era::dlc_package_view_signature),
                                               reinterpret_cast<void**>(&typed)) == S_OK && typed == packages,
                      "view answers IVectorView<DownloadableContentPackage>");
            }
            std::int64_t token = 0;
            check(reinterpret_cast<Add>(vt[7])(manager, host, &token) == S_OK && token &&
                  reinterpret_cast<Remove>(vt[8])(manager, token) == S_OK, "install-completed event registration");
        }
        if (manager) manager->Release();
        if (dlc_instance) dlc_instance->Release();
        if (dlc) dlc->Release();
        // CoreApplication: the title layer drives the view lifecycle in contract order.
        {
            static std::string lifecycle;
            static std::wstring loaded;
            static bool fail_load = false;
            struct Handler : IUnknown {
                HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override { *out = this; return S_OK; }
                ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
                ULONG STDMETHODCALLTYPE Release() override { return 1; }
                virtual HRESULT STDMETHODCALLTYPE Invoke(IInspectable* sender, IInspectable* args) {
                    lifecycle += sender && args ? "A" : "a";
                    if (args) {
                        using Kind = HRESULT(STDMETHODCALLTYPE*)(void*, INT32*);
                        const auto kind = reinterpret_cast<Kind>((*reinterpret_cast<void***>(args))[6]);
                        INT32 value = -1;
                        check(kind(args, nullptr) == E_POINTER, "activation kind validates its output");
                        check(kind(args, &value) == S_OK && value == 0, "Run dispatches an actual Launch activation");
                    }
                    return S_OK;
                }
            };
            static Handler handler;
            struct View : IInspectable {
                HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override { *out = this; return S_OK; }
                ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
                ULONG STDMETHODCALLTYPE Release() override { return 1; }
                HRESULT STDMETHODCALLTYPE GetIids(ULONG*, IID**) override { return E_NOTIMPL; }
                HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING*) override { return E_NOTIMPL; }
                HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel*) override { return E_NOTIMPL; }
                virtual HRESULT STDMETHODCALLTYPE Initialize(IInspectable* view) {
                    std::int64_t token = 0;
                    auto** vt = *reinterpret_cast<void***>(view);
                    lifecycle += "I";
                    return reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*, IUnknown*, std::int64_t*)>(vt[7])(view, &handler, &token);
                }
                virtual HRESULT STDMETHODCALLTYPE SetWindow(IInspectable* window) {
                    HSTRING name = nullptr;
                    window->GetRuntimeClassName(&name);
                    lifecycle += std::wstring(WindowsGetStringRawBuffer(name, nullptr)) == L"Windows.UI.Core.CoreWindow" ? "S" : "s";
                    WindowsDeleteString(name);
                    std::int64_t token = 0;
                    auto** vt = *reinterpret_cast<void***>(window);
                    IInspectable* dispatcher = nullptr;
                    using GetDispatcher = HRESULT (STDMETHODCALLTYPE*)(void*, IInspectable**);
                    check(reinterpret_cast<GetDispatcher>(vt[7])(window, nullptr) == E_POINTER, "dispatcher output required");
                    check(reinterpret_cast<GetDispatcher>(vt[7])(window, &dispatcher) == S_OK && dispatcher, "window dispatcher");
                    if (dispatcher) {
                        const auto dt = *reinterpret_cast<void***>(dispatcher);
                        using Access = HRESULT (STDMETHODCALLTYPE*)(void*, boolean*);
                        using Pump = HRESULT (STDMETHODCALLTYPE*)(void*, INT32);
                        const auto access = reinterpret_cast<Access>(dt[6]);
                        const auto pump = reinterpret_cast<Pump>(dt[7]);
                        boolean owns = false;
                        check(access(dispatcher, &owns) == S_OK && owns, "dispatcher owns window thread");
                        check(pump(dispatcher, 4) == E_INVALIDARG, "unknown dispatcher option rejected");
                        std::thread other([&] {
                            boolean owns = true;
                            check(access(dispatcher, &owns) == S_OK && !owns && pump(dispatcher, 3) == RPC_E_WRONG_THREAD,
                                  "dispatcher rejects pumping from another thread");
                        });
                        other.join();
                        check(pump(dispatcher, 3) == S_OK, "drain initial window messages");
                        PostThreadMessageW(GetCurrentThreadId(), WM_APP, 1, 0);
                        PostThreadMessageW(GetCurrentThreadId(), WM_APP, 2, 0);
                        check(pump(dispatcher, 1) == S_OK, "process one queued message");
                        MSG pending{};
                        check(PeekMessageW(&pending, nullptr, WM_APP, WM_APP, PM_NOREMOVE) && pending.wParam == 2,
                              "process one leaves the second message pending");
                        check(pump(dispatcher, 0) == S_OK && !PeekMessageW(&pending, nullptr, WM_APP, WM_APP, PM_NOREMOVE),
                              "process one and remaining queued messages");
                        PostQuitMessage(0);
                        check(pump(dispatcher, 2) == S_OK, "process until quit consumes real quit event");
                        dispatcher->Release();
                    }
                    return reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*, IUnknown*, std::int64_t*)>(vt[19])(window, &handler, &token);
                }
                virtual HRESULT STDMETHODCALLTYPE Load(HSTRING entry) {
                    if (fail_load) { lifecycle += "l"; return E_ABORT; }
                    loaded = WindowsGetStringRawBuffer(entry, nullptr);
                    lifecycle += "L";
                    return S_OK;
                }
                virtual HRESULT STDMETHODCALLTYPE Run() { lifecycle += "R"; return S_OK; }
                virtual HRESULT STDMETHODCALLTYPE Uninitialize() { lifecycle += "U"; return S_OK; }
            };
            static View fake_view;
            // IFrameworkViewSource: CreateView at slot 6.
            struct SourceShim : IInspectable {
                HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override { *out = this; return S_OK; }
                ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
                ULONG STDMETHODCALLTYPE Release() override { return 1; }
                HRESULT STDMETHODCALLTYPE GetIids(ULONG*, IID**) override { return E_NOTIMPL; }
                HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING*) override { return E_NOTIMPL; }
                HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel*) override { return E_NOTIMPL; }
                virtual HRESULT STDMETHODCALLTYPE CreateView(IInspectable** out) { *out = &fake_view; return S_OK; }
            };
            static SourceShim source;
            constexpr GUID iid_core_application{0x0aacf7a4, 0x5e1d, 0x49df, {0x80, 0x34, 0xfb, 0x6a, 0x68, 0xbc, 0x5e, 0xd1}};
            constexpr GUID iid_resources{0x9ff3799e, 0x6e83, 0x468c, {0xa5, 0xb0, 0x50, 0x1c, 0xe6, 0xbb, 0x0f, 0xa1}};
            IActivationFactory* core = nullptr;
            check(activate(L"Windows.ApplicationModel.Core.CoreApplication", __uuidof(IActivationFactory),
                           reinterpret_cast<void**>(&core)) == S_OK && core, "CoreApplication served by title layer");
            IInspectable* app = nullptr;
            IInspectable* resources = nullptr;
            if (core) {
                check(core->QueryInterface(iid_core_application, reinterpret_cast<void**>(&app)) == S_OK && app, "ICoreApplication");
                check(core->QueryInterface(iid_resources, reinterpret_cast<void**>(&resources)) == S_OK && resources,
                      "Xbox resource availability interface");
            }
            if (app) {
                auto** vt = *reinterpret_cast<void***>(app);
                std::int64_t token = 0;
                check(reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*, IUnknown*, std::int64_t*)>(vt[7])(app, &handler, &token) == S_OK &&
                      reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*, std::int64_t)>(vt[8])(app, token) == S_OK,
                      "Suspending registration");
                const HRESULT hr = reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*, IUnknown*)>(vt[13])(app, &source);
                check(hr == S_OK, "CoreApplication::Run succeeds");
                check(lifecycle == "ISLARU", "view lifecycle Initialize, SetWindow, Load, Activated, Run, Uninitialize");
                check(loaded == L"Title.App", "Load receives the manifest entry point");
                check(!FindWindowW(L"xone-era-core-window", nullptr), "window destroyed after Run");
                lifecycle.clear();
                fail_load = true;
                check(reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*, IUnknown*)>(vt[13])(app, &source) == E_ABORT &&
                          lifecycle == "ISlU",
                      "failed Load returns its HRESULT and still uninitializes");
                check(!FindWindowW(L"xone-era-core-window", nullptr), "window destroyed after a failed Run");
                fail_load = false;
                check(reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*, IUnknown*)>(vt[13])(app, nullptr) == E_INVALIDARG,
                      "Run without a view source");
            }
            if (resources) resources->Release();
            if (app) app->Release();
            if (core) core->Release();
        }
        if (user_statics) user_statics->Release();
        if (user) user->Release();
        if (host) host->Release();
        DeleteFileW(manifest.c_str());
        RemoveDirectoryW(package.c_str());
        RoUninitialize();
    }
    std::cout << (failures ? "era_loader: FAILED\n" : "era_loader: all checks passed\n");
    return failures ? 1 : 0;
}
