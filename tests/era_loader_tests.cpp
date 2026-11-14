// SPDX-License-Identifier: BSD-3-Clause
#include <winsock2.h>
#include "xboxone_native/era_loader.h"
#include "../src/era_winrt_classes.h"
#include "../src/era_winrt_object.h"
#include "../src/era_memory.h"
#include "../src/era_display.h"
#include "xboxone_native/appmodel.h"
#include "xboxone_native/inventory.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <evntprov.h>
#include <d3dcommon.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cmath>
#include <limits>
#include <d3d12.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <activation.h>
#include <roapi.h>
#include <winstring.h>
#include <weakreference.h>

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
unsigned esram_cpu_allocations = 0, esram_ddr_allocations = 0;
void* WINAPI esram_allocate_hook(SIZE_T size, ULONGLONG attributes) {
    if ((attributes>>29)&15) ++esram_ddr_allocations;
    else ++esram_cpu_allocations;
    return graphics_default_allocate(size,attributes);
}
void WINAPI esram_free_hook(void* pointer, ULONGLONG attributes) { graphics_default_free(pointer,attributes); }
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
    if(argc==4 && std::string(argv[1])=="--package-metadata") {
        const std::filesystem::path root=argv[2];std::string hash,error;
        if(!xboxone::inventory::fingerprint(root/argv[3],hash,error))return 1;
        set_title_package_root(root.wstring(),argv[3],hash);
        xboxone::appmodel::Manifest manifest;std::wstring location;std::string revision;
        const HRESULT hr=title_package_metadata(manifest,location,revision);
        std::cout<<"package metadata HRESULT 0x"<<std::hex<<static_cast<UINT>(hr)<<" name "<<manifest.name<<" revision "<<revision<<'\n';
        return FAILED(hr) ? 1 : 0;
    }
    if ((argc == 4 || (argc == 5 && std::string(argv[4]) == "--warp")) &&
        (std::string(argv[1]) == "--xg-layout" || std::string(argv[1]) == "--xg-graphics" || std::string(argv[1]) == "--xg-msaa" || std::string(argv[1]) == "--xg-depth")) {
        const bool graphics = std::string(argv[1]) == "--xg-graphics";
        set_stop_on_missing_activation(true);
        set_graphics_diagnostics(false, argc == 5);
        HRESULT hr = bind_d3d11x_abi(std::filesystem::path(argv[2]).wstring());
        if (SUCCEEDED(hr)) hr = bind_xg_module(std::filesystem::path(argv[3]).wstring());
        if (SUCCEEDED(hr)) hr = initialize_xg_module();
        if (std::string(argv[1]) == "--xg-depth") {
            if (FAILED(hr)) return 1;
            using Create = HRESULT(WINAPI*)(void*,D3D_DRIVER_TYPE,HMODULE,UINT,const D3D_FEATURE_LEVEL*,UINT,UINT,void**,D3D_FEATURE_LEVEL*,void**);
            const auto create = reinterpret_cast<Create>(resolve_d3d11x({"d3d11_x.dll","D3D11CreateDevice",0}));
            void* device = nullptr; void* context = nullptr;
            if (!create || FAILED(create(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,7,&device,nullptr,&context))) return 1;
            const auto vt = [](void* object){return *static_cast<void***>(object);};
            using Count = ULONG(STDMETHODCALLTYPE*)(void*);
            void* embedded_device = nullptr;
            using GetDevice = void(STDMETHODCALLTYPE*)(void*,void**);
            auto* embedded = reinterpret_cast<void**>(static_cast<char*>(context)+0x3d0);
            check(embedded[34] != nullptr,"observed modern inline OM call has an explicit dispatch target");
            reinterpret_cast<GetDevice>(embedded[3])(context,&embedded_device);
            check(embedded_device == device,"modern embedded context dispatch preserves its device identity");
            if (embedded_device) reinterpret_cast<Count>(vt(embedded_device)[2])(embedded_device);
            using SetViewport = void(STDMETHODCALLTYPE*)(void*,UINT,const D3D11_VIEWPORT*);
            using GetViewport = void(STDMETHODCALLTYPE*)(void*,UINT*,D3D11_VIEWPORT*);
            const auto set_viewports = reinterpret_cast<SetViewport>(embedded[45]);
            const auto get_viewports = reinterpret_cast<GetViewport>(embedded[96]);
            const D3D11_VIEWPORT viewport{13,17,1920,1080,.125f,.875f};
            set_viewports(context,1,&viewport);
            UINT viewport_count = 0;
            get_viewports(context,&viewport_count,nullptr);
            check(viewport_count==1,"viewport count queries retain the configured raster state");
            struct { D3D11_VIEWPORT value[2]; UINT64 guard=0x1122334455667788ull; } viewport_output{};
            viewport_count = 2;
            get_viewports(context,&viewport_count,viewport_output.value);
            const D3D11_VIEWPORT empty_viewport{};
            check(viewport_count==1 && !std::memcmp(&viewport,viewport_output.value,sizeof viewport) &&
                !std::memcmp(&empty_viewport,&viewport_output.value[1],sizeof viewport) && viewport_output.guard==0x1122334455667788ull,
                "viewport state preserves origin, dimensions, depth range and output bounds");
            static UINT invalid_viewport_slot = 0;
            set_d3d11x_trap_handler([](const char*,unsigned slot)->std::uintptr_t { invalid_viewport_slot=slot; return 0; });
            set_viewports(context,17,&viewport);
            set_d3d11x_trap_handler(nullptr);
            viewport_count = 1;
            get_viewports(context,&viewport_count,viewport_output.value);
            check(invalid_viewport_slot==45 && viewport_count==1 && !std::memcmp(&viewport,viewport_output.value,sizeof viewport),
                "invalid viewport binding reports its slot and preserves prior state");
            set_viewports(context,0,nullptr);
            get_viewports(context,&viewport_count,nullptr);
            check(!viewport_count,"zero viewport binding clears raster viewport state");
            D3D11_RASTERIZER_DESC raster{};
            raster.FillMode=D3D11_FILL_SOLID; raster.CullMode=D3D11_CULL_NONE; raster.DepthClipEnable=TRUE;
            D3D12_DEPTH_STENCIL_DESC depth_state{};
            depth_state.DepthEnable=TRUE; depth_state.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ALL; depth_state.DepthFunc=D3D12_COMPARISON_FUNC_LESS_EQUAL;
            depth_state.FrontFace=depth_state.BackFace={D3D12_STENCIL_OP_KEEP,D3D12_STENCIL_OP_KEEP,D3D12_STENCIL_OP_KEEP,D3D12_COMPARISON_FUNC_ALWAYS};
            D3D11_BLEND_DESC blend{};
            blend.RenderTarget[0].RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
            using CreateState=HRESULT(STDMETHODCALLTYPE*)(void*,const void*,void**);
            for(UINT kind=0;kind<3;++kind) {
                const UINT factory_slot=kind==0 ? 22 : kind==1 ? 21 : 20;
                const void* description=kind==0 ? static_cast<const void*>(&raster) : kind==1 ? static_cast<const void*>(&depth_state) : static_cast<const void*>(&blend);
                void* state=nullptr;
                check(reinterpret_cast<CreateState>(vt(device)[factory_slot])(device,description,&state)==S_OK && state,
                    "modern pipeline factory retains its state object");
                if(!state) continue;
                const auto bind=[&](void* value) {
                    if(kind==0) reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*)>(embedded[44])(context,value);
                    else if(kind==1) reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*,UINT)>(embedded[37])(context,value,7);
                    else {
                        const FLOAT factor[]{.25f,.5f,.75f,1};
                        reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*,const FLOAT*,UINT)>(embedded[36])(context,value,factor,0xff);
                    }
                };
                bind(state); bind(state);
                check(*reinterpret_cast<ULONG*>(static_cast<char*>(state)+12)==2,"repeated pipeline binding retains exactly one reference");
                bind(nullptr);
                check(reinterpret_cast<Count>(vt(state)[2])(state)==0,"pipeline unbinding releases ownership and its cache entry");
            }
            using CreateSampler = HRESULT(STDMETHODCALLTYPE*)(void*,const D3D12_SAMPLER_DESC*,void**);
            D3D12_SAMPLER_DESC sampler_desc{};
            sampler_desc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            sampler_desc.AddressU = sampler_desc.AddressV = sampler_desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
            sampler_desc.MaxAnisotropy = 1; sampler_desc.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
            sampler_desc.MaxLOD = D3D12_FLOAT32_MAX;
            for (float& channel : sampler_desc.BorderColor) channel = 1;
            void* sampler = nullptr;
            const auto create_sampler = reinterpret_cast<CreateSampler>(vt(device)[23]);
            check(create_sampler(device,&sampler_desc,&sampler)==S_OK && sampler,"modern factory creates a native white-border sampler");
            if (sampler) {
                std::array<UINT32,4> words{};
                using GetSamplerX = void(STDMETHODCALLTYPE*)(void*,UINT32*);
                reinterpret_cast<GetSamplerX>(vt(sampler)[9])(sampler,words.data());
                check(words[3]==0x80000000u && *reinterpret_cast<ULONG*>(static_cast<char*>(sampler)+12)==1,
                    "modern sampler retains its border SRD and reference ABI");
                void* duplicate = nullptr;
                check(create_sampler(device,&sampler_desc,&duplicate)==S_OK && duplicate==sampler,
                    "modern sampler cache shares the same live object");
                if (duplicate) reinterpret_cast<Count>(vt(duplicate)[2])(duplicate);
                check(reinterpret_cast<Count>(vt(sampler)[2])(sampler)==0,"modern sampler cache releases its device ownership");
            }
            using CreateBuffer = HRESULT(STDMETHODCALLTYPE*)(void*,const D3D11_BUFFER_DESC*,const D3D11_SUBRESOURCE_DATA*,void**);
            const auto create_buffer = reinterpret_cast<CreateBuffer>(vt(device)[3]);
            // Nonzero adjacent bytes catch accidental reads of the older 32-byte ABI.
            struct { D3D11_BUFFER_DESC desc; UINT64 guard; } buffer_input{
                {800,D3D11_USAGE_IMMUTABLE,D3D11_BIND_VERTEX_BUFFER,0,0,0},0xffffffffffffffffull};
            std::array<unsigned char,800> vertices{};
            for (UINT i=0;i<vertices.size();++i) vertices[i]=static_cast<unsigned char>(i*13+7);
            D3D11_SUBRESOURCE_DATA initial{vertices.data(),0,0};
            void* buffer = nullptr;
            check(create_buffer(device,&buffer_input.desc,nullptr,&buffer)==E_INVALIDARG && !buffer,
                "modern immutable buffers require initial data");
            check(create_buffer(device,&buffer_input.desc,&initial,&buffer)==S_OK && buffer,
                "modern CreateBuffer reads only its verified 24-byte descriptor");
            if (buffer) {
                struct { D3D11_BUFFER_DESC desc{}; UINT64 guard=0x1122334455667788ull; } output;
                using GetBufferDesc = void(STDMETHODCALLTYPE*)(void*,D3D11_BUFFER_DESC*);
                reinterpret_cast<GetBufferDesc>(vt(buffer)[12])(buffer,&output.desc);
                check(!std::memcmp(&output.desc,&buffer_input.desc,sizeof output.desc) && output.guard==0x1122334455667788ull,
                    "modern GetDesc preserves the descriptor and adjacent memory");
                check(*reinterpret_cast<ULONG*>(static_cast<char*>(buffer)+12)==1,
                    "modern buffers expose their live reference count at offset 12");
                const auto* canonical=*reinterpret_cast<void**>(static_cast<char*>(buffer)+48);
                check(canonical && !std::memcmp(canonical,vertices.data(),vertices.size()),
                    "modern immutable buffers preserve every initial vertex byte");
                reinterpret_cast<Count>(vt(buffer)[2])(buffer);
            }
            struct TextureDesc { D3D11_TEXTURE2D_DESC standard; UINT extra[2]{}; } desc{};
            desc.standard = {32,16,1,1,DXGI_FORMAT_D32_FLOAT,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_DEPTH_STENCIL,0,0};
            using Texture = HRESULT(STDMETHODCALLTYPE*)(void*,const TextureDesc*,const D3D11_SUBRESOURCE_DATA*,void**);
            for (const auto format : {DXGI_FORMAT_B5G6R5_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM}) {
                TextureDesc dynamic{};
                dynamic.standard = {33,17,1,1,format,{1,0},D3D11_USAGE_DYNAMIC,D3D11_BIND_SHADER_RESOURCE,0,0};
                void* image = nullptr;
                check(reinterpret_cast<Texture>(vt(device)[5])(device,&dynamic,nullptr,&image)==S_OK && image,
                    "modern dynamic image uses the shared canonical texture registry");
                if (!image) continue;
                using CreateSrv=HRESULT(STDMETHODCALLTYPE*)(void*,void*,const D3D11_SHADER_RESOURCE_VIEW_DESC*,void**);
                void* srv=nullptr;
                check(reinterpret_cast<CreateSrv>(vt(device)[7])(device,image,nullptr,&srv)==S_OK && srv,
                    "dynamic texture SRV retains the same canonical allocation");
                const auto canonical=srv ? *reinterpret_cast<void**>(static_cast<char*>(srv)+80) : nullptr;
                using Map = HRESULT(STDMETHODCALLTYPE*)(void*,void*,UINT,D3D11_MAP,UINT,D3D11_MAPPED_SUBRESOURCE*);
                using Unmap = void(STDMETHODCALLTYPE*)(void*,void*);
                const auto map = reinterpret_cast<Map>(embedded[16]);
                const auto unmap = reinterpret_cast<Unmap>(embedded[17]);
                const UINT pixel = format==DXGI_FORMAT_B5G6R5_UNORM ? 2 : 4;
                for (UINT pass=0;pass<2;++pass) {
                    D3D11_MAPPED_SUBRESOURCE mapped{};
                    hr=map(context,image,0,D3D11_MAP_WRITE_DISCARD,0,&mapped);
                    check(hr==S_OK && mapped.pData && mapped.RowPitch>=33*pixel && mapped.DepthPitch==17*mapped.RowPitch,
                        "texture map exposes an actual linear footprint with consistent pitches");
                    if (FAILED(hr)) break;
                    D3D11_MAPPED_SUBRESOURCE duplicate{};
                    check(map(context,image,0,D3D11_MAP_WRITE_DISCARD,0,&duplicate)==HRESULT_FROM_WIN32(ERROR_BUSY) && !duplicate.pData,
                        "nested texture maps are rejected without releasing CPU ownership");
                    auto* bytes=static_cast<unsigned char*>(mapped.pData);
                    for(UINT y=0;y<17;++y) for(UINT x=0;x<33*pixel;++x) bytes[y*mapped.RowPitch+x]=static_cast<unsigned char>(pass*29+y*13+x*7);
                    unmap(context,image);
                    TitleTextureLayout layout;
                    hr=create_xg_texture_layout(33,17,1,1,format,UINT_MAX,layout,D3D11_BIND_SHADER_RESOURCE);
                    std::vector<unsigned char> linear(33*17*pixel);
                    if(SUCCEEDED(hr)) hr=canonical ? layout.copy(layout.context.get(),false,linear.data(),canonical,0,33*pixel,33*17*pixel) : E_POINTER;
                    bool equal=SUCCEEDED(hr);
                    for(UINT y=0;y<17 && equal;++y) for(UINT x=0;x<33*pixel;++x)
                        equal=equal && linear[y*33*pixel+x]==static_cast<unsigned char>(pass*29+y*13+x*7);
                    check(equal,"texture unmap retiles every logical pixel and refreshes repeated writes");
                }
                if(srv) reinterpret_cast<Count>(vt(srv)[2])(srv);
                reinterpret_cast<Count>(vt(image)[2])(image);
            }
            void* texture = nullptr;
            {
                TextureDesc color_desc{};
                color_desc.standard={32,16,1,1,DXGI_FORMAT_B8G8R8A8_UNORM,{1,0},D3D11_USAGE_DEFAULT,
                    D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE,0,0};
                void* color=nullptr;
                check(reinterpret_cast<Texture>(vt(device)[5])(device,&color_desc,nullptr,&color)==S_OK && color,
                    "native clear fixture creates a shared renderable image");
                if(color) {
                    using View=HRESULT(STDMETHODCALLTYPE*)(void*,void*,const void*,void**);
                    void* rtv=nullptr; void* srv=nullptr;
                    check(reinterpret_cast<View>(vt(device)[9])(device,color,nullptr,&rtv)==S_OK && rtv,"native clear creates its render-target view");
                    check(reinterpret_cast<View>(vt(device)[7])(device,color,nullptr,&srv)==S_OK && srv,"native clear exposes the same image through its SRV");
                    if(rtv && srv) {
                        using Clear=void(STDMETHODCALLTYPE*)(void*,void*,const FLOAT*);
                        TitleTextureLayout layout;
                        hr=create_xg_texture_layout(32,16,1,1,DXGI_FORMAT_B8G8R8A8_UNORM,UINT_MAX,layout,color_desc.standard.BindFlags);
                        const auto canonical=*reinterpret_cast<void**>(static_cast<char*>(srv)+80);
                        for(UINT pass=0;pass<2;++pass) {
                            const FLOAT rgba[]{(17.f+pass*17)/255,34.f/255,51.f/255,68.f/255};
                            reinterpret_cast<Clear>(embedded[51])(context,rtv,rgba);
                            std::array<unsigned char,32*16*4> pixels{};
                            HRESULT copied=SUCCEEDED(hr) ? layout.copy(layout.context.get(),false,pixels.data(),canonical,0,128,2048) : hr;
                            bool equal=SUCCEEDED(copied);
                            for(UINT i=0;i<pixels.size();i+=4) equal=equal && pixels[i]==51 && pixels[i+1]==34 && pixels[i+2]==17+pass*17 && pixels[i+3]==68;
                            check(equal,"title clear executes on D3D12 and publishes every BGRA pixel after its fence, including reuse");
                        }
                        // Exercise an indexed textured quad through the same Xbox ABI.
                        // Separate VS/PS b0 values expose accidental cross-stage binding.
                        const HMODULE compiler=LoadLibraryExW(L"d3dcompiler_47.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
                        const auto compile=compiler ? reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(compiler,"D3DCompile")) : nullptr;
                        const char* sources[]{
                            "cbuffer C:register(b0){float4 scale;} struct V{float4 p:SV_Position;float2 uv:TEXCOORD0;};"
                            "V main(float2 p:POSITION,float2 uv:TEXCOORD0){V v;v.p=float4(p*scale.xy,0,1);v.uv=uv;return v;}",
                            "cbuffer C:register(b0){float4 tint;} Texture2D image:register(t0); SamplerState sample_state:register(s0);"
                            "float4 main(float4 p:SV_Position,float2 uv:TEXCOORD0):SV_Target{return image.Sample(sample_state,uv)*tint;}"};
                        Microsoft::WRL::ComPtr<ID3DBlob> codes[2];
                        void* objects[10]{}; // VS, PS, layout, vertex/index, VS/PS CB, image, SRV, sampler.
                        bool ready=compile!=nullptr;
                        using CreateShader=HRESULT(STDMETHODCALLTYPE*)(void*,const void*,SIZE_T,void*,void**);
                        for(UINT i=0;i<2 && ready;++i) {
                            ready=SUCCEEDED(compile(sources[i],std::strlen(sources[i]),nullptr,nullptr,nullptr,"main",i ? "ps_5_0" : "vs_5_0",0,0,&codes[i],nullptr));
                            if(ready) ready=reinterpret_cast<CreateShader>(vt(device)[i ? 15 : 12])(device,codes[i]->GetBufferPointer(),codes[i]->GetBufferSize(),nullptr,&objects[i])==S_OK;
                        }
                        using CreateLayout=HRESULT(STDMETHODCALLTYPE*)(void*,const D3D12_INPUT_ELEMENT_DESC*,UINT,const void*,SIZE_T,void**);
                        const D3D12_INPUT_ELEMENT_DESC elements[]{
                            {"POSITION",0,DXGI_FORMAT_R32G32_FLOAT,0,0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
                            {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,8,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0}};
                        if(ready) ready=reinterpret_cast<CreateLayout>(vt(device)[11])(device,elements,2,codes[0]->GetBufferPointer(),codes[0]->GetBufferSize(),&objects[2])==S_OK;
                        const FLOAT vertices[]{-1,-1,0,1,-1,1,0,0,1,1,1,0,1,-1,1,1};
                        const UINT16 indices[]{0,1,2,0,2,3};
                        for(UINT i=3;i<7 && ready;++i) {
                            const D3D11_BUFFER_DESC d{i==3 ? UINT(sizeof vertices) : i==4 ? UINT(sizeof indices) : 16,
                                i<5 ? D3D11_USAGE_IMMUTABLE : D3D11_USAGE_DYNAMIC,
                                UINT(i==3 ? D3D11_BIND_VERTEX_BUFFER : i==4 ? D3D11_BIND_INDEX_BUFFER : D3D11_BIND_CONSTANT_BUFFER),
                                i<5 ? 0u : D3D11_CPU_ACCESS_WRITE,0,0};
                            const D3D11_SUBRESOURCE_DATA initial{i==3 ? static_cast<const void*>(vertices) : static_cast<const void*>(indices),0,0};
                            ready=create_buffer(device,&d,i<5 ? &initial : nullptr,&objects[i])==S_OK;
                        }
                        TextureDesc image{};
                        image.standard={8,8,1,1,DXGI_FORMAT_B8G8R8A8_UNORM,{1,0},D3D11_USAGE_DYNAMIC,D3D11_BIND_SHADER_RESOURCE,0,0};
                        if(ready) ready=reinterpret_cast<Texture>(vt(device)[5])(device,&image,nullptr,&objects[7])==S_OK;
                        if(ready) ready=reinterpret_cast<View>(vt(device)[7])(device,objects[7],nullptr,&objects[8])==S_OK;
                        D3D12_SAMPLER_DESC sampler_desc{};
                        sampler_desc.Filter=D3D12_FILTER_MIN_MAG_MIP_POINT;
                        sampler_desc.AddressU=sampler_desc.AddressV=sampler_desc.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
                        sampler_desc.MaxAnisotropy=1;sampler_desc.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;
                        if(ready) ready=reinterpret_cast<CreateSampler>(vt(device)[23])(device,&sampler_desc,&objects[9])==S_OK;
                        void* raster_object=nullptr;
                        if(ready) ready=reinterpret_cast<CreateState>(vt(device)[22])(device,&raster,&raster_object)==S_OK;
                        check(ready,"indexed draw fixture prepares original-ABI shaders, vertices, constants, texture and sampler");
                        if(ready) {
                            using Map=HRESULT(STDMETHODCALLTYPE*)(void*,void*,UINT,D3D11_MAP,UINT,D3D11_MAPPED_SUBRESOURCE*);
                            using Unmap=void(STDMETHODCALLTYPE*)(void*,void*);
                            const auto map=reinterpret_cast<Map>(embedded[16]);const auto unmap=reinterpret_cast<Unmap>(embedded[17]);
                            D3D11_MAPPED_SUBRESOURCE mapped{};
                            const HRESULT mapped_image=map(context,objects[7],0,D3D11_MAP_WRITE_DISCARD,0,&mapped);
                            check(mapped_image==S_OK,"draw texture is writable through the coherent map path");
                            if(SUCCEEDED(mapped_image)) {std::memset(mapped.pData,255,mapped.DepthPitch);unmap(context,objects[7]);}
                            using Object=void(STDMETHODCALLTYPE*)(void*,void*);
                            reinterpret_cast<Object>(embedded[13])(context,objects[0]);reinterpret_cast<Object>(embedded[11])(context,objects[1]);
                            reinterpret_cast<Object>(embedded[19])(context,objects[2]);reinterpret_cast<Object>(embedded[44])(context,raster_object);
                            const UINT stride=16,offset=0;
                            using Vertices=void(STDMETHODCALLTYPE*)(void*,UINT,UINT,void* const*,const UINT*,const UINT*);
                            reinterpret_cast<Vertices>(embedded[20])(context,0,1,&objects[3],&stride,&offset);
                            using Indices=void(STDMETHODCALLTYPE*)(void*,UINT,void*,UINT);
                            reinterpret_cast<Indices>(embedded[21])(context,0,objects[4],0);
                            using Range=void(STDMETHODCALLTYPE*)(void*,UINT,UINT,void* const*);
                            reinterpret_cast<Range>(embedded[9])(context,0,1,&objects[5]);reinterpret_cast<Range>(embedded[18])(context,0,1,&objects[6]);
                            reinterpret_cast<Range>(embedded[12])(context,0,1,&objects[9]);
                            using Resources=void(STDMETHODCALLTYPE*)(void*,void* const*,UINT,UINT);
                            reinterpret_cast<Resources>(embedded[10])(context,&objects[8],0,0x76b40);
                            using Targets=void(STDMETHODCALLTYPE*)(void*,UINT,void* const*,void*);
                            reinterpret_cast<Targets>(embedded[34])(context,1,&rtv,nullptr);
                            const D3D11_VIEWPORT viewport{0,0,32,16,0,1};set_viewports(context,1,&viewport);
                            const UINT64 topology=0x000009ffc0009e00ull;std::memcpy(static_cast<char*>(context)+0x170,&topology,sizeof topology);
                            for(UINT pass=0;pass<2;++pass) {
                                const FLOAT constants[2][4]{{1,1,1,1},{pass ? .5f : .25f,pass ? .25f : .5f,pass ? .125f : .75f,1}};
                                bool written=true;
                                for(UINT stage=0;stage<2;++stage) {
                                    const HRESULT result=map(context,objects[5+stage],0,D3D11_MAP_WRITE_DISCARD,0,&mapped);
                                    written=written && SUCCEEDED(result);
                                    if(SUCCEEDED(result)) {std::memcpy(mapped.pData,constants[stage],16);unmap(context,objects[5+stage]);}
                                }
                                check(written,"per-stage constants update between indexed draws");
                                using Draw=void(STDMETHODCALLTYPE*)(void*,UINT64,INT);
                                reinterpret_cast<Draw>(embedded[14])(context,6ull<<32,0);
                                std::array<unsigned char,32*16*4> pixels{};
                                const HRESULT copied=layout.copy(layout.context.get(),false,pixels.data(),canonical,0,128,2048);
                                bool equal=SUCCEEDED(copied);
                                // D3D FLOAT->UNORM allows 0.6 ULP on the integer side.
                                for(UINT i=0;i<pixels.size();i+=4) equal=equal && std::abs(FLOAT(pixels[i])-255*constants[1][2])<=.6f &&
                                    std::abs(FLOAT(pixels[i+1])-255*constants[1][1])<=.6f && std::abs(FLOAT(pixels[i+2])-255*constants[1][0])<=.6f && pixels[i+3]==255;
                                check(equal,"native indexed draw produces expected textured pixels, separate stage constants and fresh CPU updates");
                                if(!equal) std::cerr << "draw pixels first BGRA " << UINT(pixels[0]) << ',' << UINT(pixels[1]) << ',' << UINT(pixels[2]) << ',' << UINT(pixels[3])
                                    << " center " << UINT(pixels[1024]) << ',' << UINT(pixels[1025]) << ',' << UINT(pixels[1026]) << ',' << UINT(pixels[1027]) << '\n';
                            }
                            reinterpret_cast<Object>(embedded[13])(context,nullptr);reinterpret_cast<Object>(embedded[11])(context,nullptr);
                            reinterpret_cast<Object>(embedded[19])(context,nullptr);reinterpret_cast<Object>(embedded[44])(context,nullptr);
                            reinterpret_cast<Vertices>(embedded[20])(context,0,1,nullptr,&stride,&offset);reinterpret_cast<Indices>(embedded[21])(context,0,nullptr,0);
                            reinterpret_cast<Range>(embedded[9])(context,0,1,nullptr);reinterpret_cast<Range>(embedded[18])(context,0,1,nullptr);
                            reinterpret_cast<Range>(embedded[12])(context,0,1,nullptr);reinterpret_cast<Resources>(embedded[10])(context,nullptr,0,0x76b40);
                            reinterpret_cast<Targets>(embedded[34])(context,0,nullptr,nullptr);
                        }
                        if(raster_object) reinterpret_cast<Count>(vt(raster_object)[2])(raster_object);
                        for(auto* object : objects) if(object) reinterpret_cast<Count>(vt(object)[2])(object);
                        codes[0].Reset();codes[1].Reset();if(compiler) FreeLibrary(compiler);
                    }
                    if(rtv) reinterpret_cast<Count>(vt(rtv)[2])(rtv);
                    if(srv) reinterpret_cast<Count>(vt(srv)[2])(srv);
                    reinterpret_cast<Count>(vt(color)[2])(color);
                }
            }
            hr = reinterpret_cast<Texture>(vt(device)[5])(device,&desc,nullptr,&texture);
            check(SUCCEEDED(hr) && texture,"modern ABI creates the expanded D32 texture");
            if (texture) {
                check(*reinterpret_cast<ULONG*>(static_cast<char*>(texture)+12) == 1,"D32 retains the modern public reference counter");
                using Dsv = HRESULT(STDMETHODCALLTYPE*)(void*,void*,const D3D11_DEPTH_STENCIL_VIEW_DESC*,void**);
                const auto create_dsv = reinterpret_cast<Dsv>(vt(device)[10]);
                void* view = reinterpret_cast<void*>(1);
                check(create_dsv(device,nullptr,nullptr,&view) == E_INVALIDARG && !view,"DSV clears its output on an invalid resource");
                D3D11_DEPTH_STENCIL_VIEW_DESC input{};
                input.Format = DXGI_FORMAT_D32_FLOAT; input.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
                input.Texture2D.MipSlice = 1;
                check(create_dsv(device,texture,&input,&view) == E_INVALIDARG && !view,"DSV rejects a mip outside the resource");
                input.Texture2D.MipSlice = 0; input.Flags = D3D11_DSV_READ_ONLY_STENCIL;
                check(create_dsv(device,texture,&input,&view) == DXGI_ERROR_UNSUPPORTED && !view,"D32 cannot claim a stencil plane");
                input.Flags = 0;
                check(create_dsv(device,texture,&input,&view) == S_OK && view,"modern ABI creates a real native depth view");
                if (view) {
                    check(*reinterpret_cast<UINT*>(static_cast<char*>(view)+40) == 1,"DSV public type agrees with the original UMD");
                    using ClearDepth=void(STDMETHODCALLTYPE*)(void*,void*,UINT,FLOAT,UINT8);
                    const auto clear_depth=reinterpret_cast<ClearDepth>(embedded[54]);
                    clear_depth(context,view,D3D11_CLEAR_DEPTH,0.375f,0x7f);
                    clear_depth(context,view,D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,1.f,0);
                    using Bind = void(STDMETHODCALLTYPE*)(void*,UINT,void* const*,void*);
                    const auto bind = reinterpret_cast<Bind>(embedded[34]);
                    bind(context,0,nullptr,view);
                    bind(context,0,nullptr,view);
                    check(*reinterpret_cast<ULONG*>(static_cast<char*>(view)+12) == 2,
                        "repeated OM binding retains one view without accumulating references");
                    bind(context,0,nullptr,nullptr);
                    check(*reinterpret_cast<ULONG*>(static_cast<char*>(view)+12) == 1,
                        "OM unbinding releases its retained depth view");
                    struct { D3D11_DEPTH_STENCIL_VIEW_DESC desc{}; UINT64 guard=0x1122334455667788ull; } output;
                    using GetDesc = void(STDMETHODCALLTYPE*)(void*,D3D11_DEPTH_STENCIL_VIEW_DESC*);
                    reinterpret_cast<GetDesc>(vt(view)[9])(view,&output.desc);
                    check(output.desc.Format == DXGI_FORMAT_D32_FLOAT && output.desc.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2D &&
                        !output.desc.Flags && output.guard == 0x1122334455667788ull,"DSV writes exactly its verified 24-byte description");
                    using Resource = void(STDMETHODCALLTYPE*)(void*,void**);
                    void* retained = nullptr;
                    reinterpret_cast<Resource>(vt(view)[8])(view,&retained);
                    check(retained == texture && *reinterpret_cast<ULONG*>(static_cast<char*>(texture)+12) == 3,
                        "DSV GetResource retains the same canonical texture identity");
                    reinterpret_cast<Count>(vt(retained)[2])(retained);
                    check(reinterpret_cast<Count>(vt(view)[2])(view) == 0 &&
                        *reinterpret_cast<ULONG*>(static_cast<char*>(texture)+12) == 1,"DSV retirement releases its parent ownership");
                }
                reinterpret_cast<Count>(vt(texture)[2])(texture);
            }
            reinterpret_cast<Count>(vt(context)[2])(context); reinterpret_cast<Count>(vt(device)[2])(device);
            return failures ? 1 : 0;
        }
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
            check(SUCCEEDED(hr) && layout.context && layout.bytes==98304,
                "three-plane depth/stencil layout retains the original physical allocation");
            if (SUCCEEDED(hr)) {
                std::vector<unsigned char> physical(layout.bytes+32,0x5a), samples(32*16*4*8), restored(samples.size(),0x3c);
                for (SIZE_T at=0;at<samples.size();at+=8) {
                    const UINT32 depth_bits=0x3e000000u+UINT32(at*13);
                    std::memcpy(samples.data()+at,&depth_bits,4);
                    samples[at+4]=static_cast<unsigned char>(at/8*17+3);
                }
                hr=layout.copy(layout.context.get(),true,samples.data(),physical.data()+16,0,1024,16384);
                if (SUCCEEDED(hr)) hr=layout.copy(layout.context.get(),false,restored.data(),physical.data()+16,0,1024,16384);
                check(SUCCEEDED(hr) && samples==restored,"depth/stencil CPU roundtrip preserves every sample's depth bits and stencil byte");
                check(std::all_of(physical.begin(),physical.begin()+16,[](auto x){return x==0x5a;}) &&
                    std::all_of(physical.end()-16,physical.end(),[](auto x){return x==0x5a;}),"depth/stencil tiling preserves allocation guards");
                physical[16]^=1;
                const auto before=restored;
                check(layout.copy(layout.context.get(),false,restored.data(),physical.data()+16,0,1024,16384)==DXGI_ERROR_UNSUPPORTED && restored==before,
                    "unknown HTILE encoding refuses readback without modifying the linear destination");
                const auto tiled_before=physical;
                check(layout.copy(layout.context.get(),true,samples.data(),physical.data()+16,0,1,16384)==E_INVALIDARG && physical==tiled_before,
                    "invalid depth/stencil pitch preserves every physical plane");
            }
            hr = create_xg_texture_layout(1920, 1080, 1, 1, DXGI_FORMAT_R32G8X24_TYPELESS, 1, layout,
                D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE, 4);
            check(SUCCEEDED(hr) && layout.context && layout.bytes==41910272,
                "observed full-size FH2 descriptor preserves the exact ESRAM and DDR allocation span");
            using CreateComputer = HRESULT(*)(const void*,void**);
            const auto create_computer = reinterpret_cast<CreateComputer>(resolve_xg_module({"xg_x.dll","XGCreateTexture2DComputer",0}));
            check(create_computer != nullptr,"revision-bound XG exposes its original depth layout factory");
            if (!create_computer) return 1;
            for (const auto dimensions : {std::array<UINT,2>{32,16},std::array<UINT,2>{1920,1080}}) {
                std::array<UINT,15> descriptor{dimensions[0],dimensions[1],1,1,19,4,0,0,0x40,0,0,0,0,1,0};
                void* computer = nullptr;
                hr = create_computer(descriptor.data(),&computer);
                check(SUCCEEDED(hr) && computer,"original XG constructs the observed depth/stencil descriptor");
                if (FAILED(hr) || !computer) continue;
                auto** table = *static_cast<void***>(computer);
                std::array<unsigned char,0xd40+16> raw;
                raw.fill(0x5a);
                hr = reinterpret_cast<HRESULT(*)(void*,void*)>(table[2])(computer,raw.data());
                const auto u64 = [&](SIZE_T offset) { UINT64 value; std::memcpy(&value,raw.data()+offset,8); return value; };
                const auto u32 = [&](SIZE_T offset) { UINT value; std::memcpy(&value,raw.data()+offset,4); return value; };
                const UINT64 bytes = u64(0);
                check(SUCCEEDED(hr) && u32(16)==1 && u32(20)==3 && bytes==(dimensions[0]==32 ? 98304ull : 41910272ull),
                    "original depth layout has verified size and three physical planes");
                check(std::all_of(raw.begin()+0xd40,raw.end(),[](auto x){return x==0x5a;}),"depth layout preserves ABI output bounds");
                std::cout << "original depth " << dimensions[0] << 'x' << dimensions[1] << " bytes=" << bytes << '\n';
                if (dimensions[0]==32) {
                    std::array<UINT32,8> words{};
                    std::memcpy(words.data(),static_cast<char*>(computer)+0x10,sizeof words);
                    check(std::all_of(words.begin(),words.end(),[](auto word){return !word;}),
                        "original XG depth computer does not publish a ready-made shader SRD");
                }
                // Reference helpers 140167760/1401677d0 establish the 11295 inline plane/mip strides.
                for (UINT plane=0;plane<3;++plane) {
                    const SIZE_T offset=24+plane*0x460;
                    const UINT usage=u32(offset), element=u32(offset+32);
                    const UINT64 size=u64(offset+8), base=u64(offset+16);
                    std::cout << " plane=" << plane << " usage=" << usage << " bytes=" << size << " base=" << base
                              << " alignment=" << u64(offset+24) << " element=" << element << '\n';
                    check(base<=bytes && size<=bytes-base,"depth plane fits the original allocation");
                    check(usage==(plane==0 ? 4u : plane==1 ? 7u : 8u),"physical plane order matches the original HTILE/depth/stencil layout");
                    if (dimensions[0]!=32 || usage==4) continue;
                    check((usage==7 || usage==8) && element==(usage==7 ? 4u : 1u),
                        "depth and stencil have independent verified element widths");
                    if ((usage!=7 && usage!=8) || element!=(usage==7 ? 4u : 1u)) continue;
                    using Address=UINT64(*)(void*,UINT,UINT,UINT64,UINT,UINT,UINT);
                    const auto address=reinterpret_cast<Address>(table[4]);
                    auto* plane_context=static_cast<unsigned char*>(computer)+0xd8+plane*0x1c18;
                    using PhysicalAddress=INT64(*)(UINT64,UINT,UINT,UINT,UINT64,UINT64,const void*);
                    const auto physical=reinterpret_cast<PhysicalAddress>(static_cast<unsigned char*>(table[4])+
                        ((plane_context[0x68]&0x80) ? 0xb440 : 0x88d0)-0x2a70);
                    auto typed_descriptor=descriptor;
                    typed_descriptor[4]=usage==7 ? 41 : 62; typed_descriptor[8]=0;
                    void* typed=nullptr;
                    const HRESULT typed_hr=create_computer(typed_descriptor.data(),&typed);
                    bool same_typed=SUCCEEDED(typed_hr) && typed;
                    void** typed_table=same_typed ? *static_cast<void***>(typed) : nullptr;
                    const auto typed_address=typed_table ? reinterpret_cast<Address>(typed_table[4]) : nullptr;
                    std::vector<unsigned char> touched(static_cast<SIZE_T>(bytes));
                    bool valid=true;
                    for (UINT y=0;y<16;++y) for (UINT x=0;x<32;++x) for (UINT sample=0;sample<4;++sample) {
                        const INT64 bits=physical(x,y,0,sample,0,0,plane_context);
                        const UINT64 at=static_cast<UINT64>(bits>>12);
                        if (!sample && at!=address(computer,plane,0,x,y,0,0)) valid=false;
                        if (sample && address(computer,plane,0,x,y,0,sample)!=UINT64_MAX) valid=false;
                        if (typed_address && typed_address(typed,0,0,x,y,0,sample)+base!=at) same_typed=false;
                        if (at<base || at>base+size || element>base+size-at) { valid=false; continue; }
                        for (UINT byte=0;byte<element;++byte) {
                            if (touched[static_cast<SIZE_T>(at+byte)]++) valid=false;
                        }
                    }
                    check(valid,"every depth/stencil sample has a distinct in-plane byte address");
                    check(same_typed,"typed XG computer preserves the exact depth/stencil address of every sample");
                    if (typed_table) reinterpret_cast<ULONG(*)(void*)>(typed_table[1])(typed);
                }
                reinterpret_cast<ULONG(*)(void*)>(table[1])(computer);
            }
            hr=create_xg_texture_layout(1920,1080,1,1,DXGI_FORMAT_R11G11B10_FLOAT,UINT_MAX,layout,
                D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS);
            std::cout << "original FH2 ESRAM color layout hr=0x" << std::hex << hr << std::dec
                      << " bytes=" << layout.bytes << " alignment=" << layout.alignment << '\n';
            check(SUCCEEDED(hr),"observed ESRAM color descriptor has an original XG expanded layout");
            using OptimalDepth=void(*)(UINT,UINT,UINT,UINT,UINT,UINT,UINT*,UINT*);
            const auto depth_modes=reinterpret_cast<OptimalDepth>(resolve_xg_module({"xg_x.dll","XGComputeOptimalDepthStencilTileModes",0}));
            check(depth_modes!=nullptr,"original XG exposes its separate depth/stencil tile selector");
            if (!depth_modes) return 1;
            for (const UINT samples:{1u,2u,4u,8u}) {
                struct { UINT depth=UINT_MAX,stencil=UINT_MAX; UINT64 guard=0x1122334455667788ull; } result;
                depth_modes(0,0,0,0,samples,0,&result.depth,&result.stencil);
                const UINT expected=samples==1 ? 0 : samples==8 ? 2 : 1;
                check(result.depth==expected && result.stencil==expected && result.guard==0x1122334455667788ull,
                    "original depth tile modes preserve scalar argument positions and output bounds");
            }
            depth_modes(0,0,0,0,1,0,nullptr,nullptr);
            for (const auto format:{DXGI_FORMAT_R32_TYPELESS,DXGI_FORMAT_R16_TYPELESS}) for (const auto dimensions:{std::array<UINT,2>{32,16},std::array<UINT,2>{1920,1080}}) {
                const UINT element=format==DXGI_FORMAT_R16_TYPELESS ? 2 : 4, row=32*element, slice=row*16;
                std::array<UINT,15> descriptor{dimensions[0],dimensions[1],1,1,UINT(format),1,0,0,0x40,0,0,0,0,0,0};
                void* computer=nullptr;
                hr=create_computer(descriptor.data(),&computer);
                check(SUCCEEDED(hr) && computer,"original XG constructs the observed scalar depth descriptor");
                if (!computer) continue;
                auto** table=*static_cast<void***>(computer);
                std::array<unsigned char,0xd40> raw{};
                hr=reinterpret_cast<HRESULT(*)(void*,void*)>(table[2])(computer,raw.data());
                UINT64 bytes=0; UINT planes=0;
                std::memcpy(&bytes,raw.data(),8);std::memcpy(&planes,raw.data()+20,4);
                std::cout << "original depth format=" << format << ' ' << dimensions[0] << 'x' << dimensions[1] << " bytes=" << bytes << " planes=" << planes << '\n';
                check(SUCCEEDED(hr) && planes==2,"scalar depth has separate HTILE and physical depth planes");
                hr=create_xg_texture_layout(dimensions[0],dimensions[1],1,1,format,UINT_MAX,layout,
                    D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE);
                check(SUCCEEDED(hr) && layout.bytes==bytes,"scalar depth adapter preserves the exact original physical depth allocation");
                if (SUCCEEDED(hr) && dimensions[0]==32) {
                    std::vector<unsigned char> expected(slice),actual(expected.size());
                    for (UINT i=0;i<expected.size();++i) expected[i]=static_cast<unsigned char>(i*13+i/7);
                    std::vector<unsigned char> physical(layout.bytes+32,0x5a);
                    hr=layout.copy(layout.context.get(),true,expected.data(),physical.data()+16,0,row,slice);
                    using Address=UINT64(*)(void*,UINT,UINT,UINT64,UINT,UINT,UINT);
                    const auto address=reinterpret_cast<Address>(table[4]);
                    bool exact=SUCCEEDED(hr);
                    for (UINT y=0;y<16;++y) for (UINT x=0;x<32;++x) {
                        const auto at=address(computer,1,0,x,y,0,0);
                        if (at>bytes || bytes-at<element) { exact=false;continue; }
                        if (std::memcmp(physical.data()+16+at,expected.data()+(y*32+x)*element,element)) exact=false;
                    }
                    check(exact,"scalar depth adapter matches the independent original depth computer at every pixel address");
                    if (SUCCEEDED(hr)) hr=layout.copy(layout.context.get(),false,actual.data(),physical.data()+16,0,row,slice);
                    check(SUCCEEDED(hr) && actual==expected,"single-sample scalar depth preserves every physical bit through expanded HTILE");
                    physical[16]^=1; const auto before=actual;
                    check(layout.copy(layout.context.get(),false,actual.data(),physical.data()+16,0,row,slice)==DXGI_ERROR_UNSUPPORTED && actual==before,
                        "single-sample depth refuses unknown HTILE without publishing partial pixels");
                    check(std::all_of(physical.begin(),physical.begin()+16,[](auto x){return x==0x5a;}) &&
                        std::all_of(physical.end()-16,physical.end(),[](auto x){return x==0x5a;}),"scalar depth conversion preserves canonical guards");
                }
                reinterpret_cast<ULONG(*)(void*)>(table[1])(computer);
            }
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
            using PresentArray=HRESULT(WINAPI*)(UINT,UINT,UINT,UINT,void* const*,const void*);
            const auto present_array=reinterpret_cast<PresentArray>(resolve_d3d11x({"d3d11_x.dll","DXGIXPresentArray",0}));
            check(present_array!=nullptr,"legacy PresentArray export is bound to the presentation runtime");
            if(present_array) {
                struct PresentParameters {UINT reserved[2]{};RECT source{};POINT destination{};FLOAT scale[2]{1,1};UINT64 cookie=0;UINT flags=0,tail=0;} p;
                static_assert(sizeof p==56);
                void* chain=nullptr;
                check(present_array(2,20,0,0,&chain,&p)==E_INVALIDARG && present_array(5,20,0,1,&chain,&p)==E_INVALIDARG,
                      "PresentArray rejects invalid counts and intervals before touching a swapchain");
                check(present_array(2,21,0,1,&chain,&p)==DXGI_ERROR_UNSUPPORTED &&
                      present_array(2,20,1,1,&chain,&p)==DXGI_ERROR_UNSUPPORTED && present_array(2,20,0,2,&chain,&p)==DXGI_ERROR_UNSUPPORTED,
                      "unknown presentation profiles, flags and multi-plane composition remain explicit");
                p.cookie=1;
                check(present_array(2,20,0,1,&chain,&p)==E_INVALIDARG,"opaque frame cookie is accepted before validating the null chain");
                p.cookie=UINT64_MAX;
                check(present_array(2,20,0,1,&chain,&p)==E_INVALIDARG,"frame cookie is never dereferenced or interpreted as a synchronization handle");
                p.cookie=0;p.scale[0]=2;
                check(present_array(2,20,0,1,&chain,&p)==DXGI_ERROR_UNSUPPORTED,"scaled presentation cannot silently become identity composition");
                p.scale[0]=1;
                check(present_array(2,20,0,1,&chain,reinterpret_cast<void*>(1))==E_INVALIDARG &&
                      present_array(2,20,0,1,&chain,&p)==E_INVALIDARG,"unreadable presentation data and null chains fail without presentation");
            }
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
            Desc placed_depth{};
            placed_depth.standard={32,16,1,1,DXGI_FORMAT_R32G8X24_TYPELESS,{4,0},D3D11_USAGE_DEFAULT,
                D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE,0,0x40000};
            auto* depth_storage=static_cast<unsigned char*>(allocate(98304+8192+32,attributes));
            if (!depth_storage) return 1;
            std::memset(depth_storage,0x5a,98304+8192+32);
            void* depth_base=reinterpret_cast<void*>((reinterpret_cast<std::uintptr_t>(depth_storage)+16+8191)&~std::uintptr_t(8191));
            texture=nullptr;
            const auto place_texture=reinterpret_cast<CreateTexture>(vt(device)[64]);
            check(place_texture(device,&placed_depth,1,1,depth_base,&texture)==DXGI_ERROR_UNSUPPORTED && !texture,
                "depth placement rejects unknown flags before acquiring the original storage");
            check(place_texture(device,&placed_depth,1,0,depth_base,&texture)==S_OK && texture,
                "depth placement binds the original three-plane allocation to native MSAA depth/stencil");
            if (texture) {
                using CreateDsv=HRESULT(STDMETHODCALLTYPE*)(void*,void*,const D3D11_DEPTH_STENCIL_VIEW_DESC*,void**);
                using CreateSrv=HRESULT(STDMETHODCALLTYPE*)(void*,void*,const D3D11_SHADER_RESOURCE_VIEW_DESC*,void**);
                const auto create_srv=reinterpret_cast<CreateSrv>(vt(device)[7]);
                for (const auto format:{DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS,DXGI_FORMAT_X32_TYPELESS_G8X24_UINT}) {
                    D3D11_SHADER_RESOURCE_VIEW_DESC description{};
                    description.Format=format; description.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2DMS;
                    void* view=nullptr;
                    check(create_srv(device,texture,&description,&view)==S_OK && view,"depth/stencil SRV binds a typed view of the same native resource");
                    if (!view) continue;
                    std::array<UINT32,8> descriptor{};
                    std::memcpy(descriptor.data(),static_cast<char*>(view)+48,sizeof descriptor);
                    const UINT64 plane_offset=format==DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS ? 16384 : 81920;
                    check(descriptor[0]==((reinterpret_cast<std::uintptr_t>(depth_base)+plane_offset)>>8) &&
                        ((descriptor[3]>>16)&15)==2 && (descriptor[3]>>28)>=14,
                        "depth/stencil SRD preserves its physical plane base and four-sample address contract");
                    struct { D3D11_SHADER_RESOURCE_VIEW_DESC value{}; UINT guard=0xaabbccdd; } actual;
                    reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,D3D11_SHADER_RESOURCE_VIEW_DESC*)>(vt(view)[9])(view,&actual.value);
                    check(actual.value.Format==format && actual.value.ViewDimension==description.ViewDimension && actual.guard==0xaabbccdd,
                        "typed MSAA SRV queries preserve format, dimension and output extent");
                    reinterpret_cast<Count>(vt(view)[2])(view);
                }
                const auto create_dsv=reinterpret_cast<CreateDsv>(vt(device)[10]);
                D3D11_DEPTH_STENCIL_VIEW_DESC depth_view{};
                depth_view.Format=DXGI_FORMAT_D32_FLOAT; depth_view.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2DMS;
                void* view=nullptr;
                check(create_dsv(device,texture,&depth_view,&view)==DXGI_ERROR_UNSUPPORTED && !view,
                    "combined depth/stencil rejects a depth-only reinterpretation");
                depth_view.Format=DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
                depth_view.Flags=D3D11_DSV_READ_ONLY_DEPTH|D3D11_DSV_READ_ONLY_STENCIL;
                check(create_dsv(device,texture,&depth_view,nullptr)==S_FALSE,
                    "read-only depth/stencil validation does not create ownership");
                check(create_dsv(device,texture,&depth_view,&view)==S_OK && view,
                    "original DSV factory creates a typed native four-sample view");
                if (view) {
                    struct { D3D11_DEPTH_STENCIL_VIEW_DESC desc{}; UINT guard=0xaabbccdd; } actual_view;
                    reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,D3D11_DEPTH_STENCIL_VIEW_DESC*)>(vt(view)[9])(view,&actual_view.desc);
                    check(!std::memcmp(&actual_view.desc,&depth_view,sizeof depth_view) && actual_view.guard==0xaabbccdd,
                        "MSAA DSV description preserves both read-only aspects and its ABI extent");
                    using SetDepthTarget=void(STDMETHODCALLTYPE*)(void*,UINT,void* const*,void*);
                    using GetDepthTarget=void(STDMETHODCALLTYPE*)(void*,UINT,void**,void**);
                    reinterpret_cast<SetDepthTarget>(vt(context)[33])(context,0,nullptr,view);
                    void* retained_depth=nullptr;
                    reinterpret_cast<GetDepthTarget>(vt(context)[89])(context,0,nullptr,&retained_depth);
                    check(retained_depth==view,"legacy OM depth-only query returns the bound MSAA DSV");
                    reinterpret_cast<SetDepthTarget>(vt(context)[33])(context,0,nullptr,nullptr);
                    check(reinterpret_cast<Count>(vt(view)[1])(view)==3,"queried DSV survives unbinding with independent caller ownership");
                    reinterpret_cast<Count>(vt(view)[2])(view);
                    if(retained_depth)reinterpret_cast<Count>(vt(retained_depth)[2])(retained_depth);
                    check(reinterpret_cast<Count>(vt(texture)[2])(texture)==1,"MSAA DSV retains its original parent storage");
                    texture=nullptr;
                    reinterpret_cast<Count>(vt(view)[2])(view);
                }
                if (texture) reinterpret_cast<Count>(vt(texture)[2])(texture);
            }
            check(std::all_of(depth_storage,depth_storage+98304+8192+32,[](auto x){return x==0x5a;}),
                "depth placement and view retirement preserve every borrowed physical byte");
            free_memory(depth_storage,attributes);
            auto* clear_storage=static_cast<unsigned char*>(allocate(98304+8192+32,attributes));
            if(!clear_storage) return 1;
            std::memset(clear_storage,0x5a,98304+8192+32);
            auto* clear_base=reinterpret_cast<unsigned char*>((reinterpret_cast<std::uintptr_t>(clear_storage)+16+8191)&~std::uintptr_t(8191));
            void* clear_texture=nullptr;void* clear_dsv=nullptr;
            check(place_texture(device,&placed_depth,1,0,clear_base,&clear_texture)==S_OK && clear_texture,"MSAA clear fixture retains guarded borrowed depth/stencil storage");
            if(clear_texture) {
                using MakeDsv=HRESULT(STDMETHODCALLTYPE*)(void*,void*,const D3D11_DEPTH_STENCIL_VIEW_DESC*,void**);
                D3D11_DEPTH_STENCIL_VIEW_DESC desc{};desc.Format=DXGI_FORMAT_D32_FLOAT_S8X24_UINT;desc.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2DMS;
                check(reinterpret_cast<MakeDsv>(vt(device)[10])(device,clear_texture,&desc,&clear_dsv)==S_OK && clear_dsv,"MSAA clear fixture creates a writable original DSV");
            }
            if(clear_dsv) {
                TitleTextureLayout planes;
                check(create_xg_texture_layout(32,16,1,1,DXGI_FORMAT_R32G8X24_TYPELESS,1,planes,D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE,4)==S_OK,
                      "MSAA clear uses the verified three-plane XG layout");
                std::vector<BYTE> linear(16384);
                const auto verify=[&](UINT bits,BYTE stencil) {
                    bool exact=planes.copy && planes.copy(planes.context.get(),false,linear.data(),clear_base,0,1024,16384)==S_OK;
                    for(SIZE_T at=0;exact && at<linear.size();at+=8) {UINT actual=0;std::memcpy(&actual,linear.data()+at,4);exact=actual==bits && linear[at+4]==stencil;}
                    check(exact,"native depth/stencil clear preserves exact depth bits and stencil in all four samples");
                };
                using ClearDepth=void(STDMETHODCALLTYPE*)(void*,void*,UINT,FLOAT,UINT8);
                const auto clear=reinterpret_cast<ClearDepth>(vt(context)[53]);
                clear(context,clear_dsv,3,.5f,0x6d);verify(0x3f000000,0x6d);
                clear(context,clear_dsv,1,.25f,0);verify(0x3e800000,0x6d);
                clear(context,clear_dsv,2,std::numeric_limits<FLOAT>::quiet_NaN(),0x27);verify(0x3e800000,0x27);
                using Deferred=HRESULT(STDMETHODCALLTYPE*)(void*,UINT,void**);
                using Finish=HRESULT(STDMETHODCALLTYPE*)(void*,BOOL,void**);
                using Execute=void(STDMETHODCALLTYPE*)(void*,void*,BOOL);
                void* producer=nullptr;void* list=nullptr;
                check(reinterpret_cast<Deferred>(vt(device)[27])(device,0x60000,&producer)==S_OK && producer,"MSAA clear records through the real deferred ABI");
                if(producer) {
                    reinterpret_cast<ClearDepth>(vt(producer)[53])(producer,clear_dsv,3,.75f,0xa7);
                    check(reinterpret_cast<Finish>(vt(producer)[114])(producer,FALSE,&list)==S_OK && list,"MSAA clear finishes without submitting");
                    verify(0x3e800000,0x27);
                    reinterpret_cast<Count>(vt(producer)[2])(producer);
                }
                reinterpret_cast<Count>(vt(clear_dsv)[2])(clear_dsv);clear_dsv=nullptr;
                reinterpret_cast<Count>(vt(clear_texture)[2])(clear_texture);clear_texture=nullptr;
                if(list) {
                    reinterpret_cast<Execute>(vt(context)[58])(context,list,FALSE);verify(0x3f400000,0xa7);
                    for(SIZE_T at=0;at<linear.size();at+=8) {const UINT bits=0x3e000000;std::memcpy(linear.data()+at,&bits,4);linear[at+4]=0x11;}
                    check(planes.copy(planes.context.get(),true,linear.data(),clear_base,0,1024,16384)==S_OK,"MSAA clear reuse accepts fresh canonical data between fences");
                    reinterpret_cast<Execute>(vt(context)[58])(context,list,TRUE);verify(0x3f400000,0xa7);
                    reinterpret_cast<Count>(vt(list)[2])(list);
                }
            }
            if(clear_dsv) reinterpret_cast<Count>(vt(clear_dsv)[2])(clear_dsv);
            if(clear_texture) reinterpret_cast<Count>(vt(clear_texture)[2])(clear_texture);
            check(std::all_of(clear_storage,clear_base,[](auto value){return value==0x5a;}) &&
                std::all_of(clear_base+98304,clear_storage+98304+8192+32,[](auto value){return value==0x5a;}),
                  "immediate/deferred MSAA clears preserve every physical allocation guard");
            free_memory(clear_storage,attributes);
            using Reserve=void*(WINAPI*)(void*,SIZE_T,DWORD,DWORD);
            using ReleaseVirtual=BOOL(WINAPI*)(void*,SIZE_T,DWORD);
            const auto reserve=reinterpret_cast<Reserve>(resolve_kernelx({"kernelx.dll","VirtualAlloc",0}));
            const auto release_virtual=reinterpret_cast<ReleaseVirtual>(resolve_kernelx({"kernelx.dll","VirtualFree",0}));
            auto* esram_mirror=static_cast<unsigned char*>(reserve(nullptr,65536,0x30002000,PAGE_READWRITE));
            for (const auto format:{DXGI_FORMAT_R11G11B10_FLOAT,DXGI_FORMAT_R32_TYPELESS}) {
                const bool depth=format==DXGI_FORMAT_R32_TYPELESS;
                UINT physical_page=depth ? 64 : 48;
                if (!esram_mirror || FAILED(map_title_esram(1,esram_mirror,1,&physical_page))) return 1;
                std::memset(esram_mirror,0x35,65536);
                Desc esram_desc{};
                esram_desc.standard={32,16,1,1,format,{1,0},D3D11_USAGE_DEFAULT,
                    UINT(D3D11_BIND_SHADER_RESOURCE|(depth ? D3D11_BIND_DEPTH_STENCIL : D3D11_BIND_RENDER_TARGET|D3D11_BIND_UNORDERED_ACCESS)),0,0x20000};
                esram_desc.extension[0]=physical_page*65536+8192; esram_desc.extension[1]=depth ? 0 : 8192;
                const UINT original_page=physical_page, mirror_offset=depth ? 24576 : 8192;
                using CreateOwned=HRESULT(STDMETHODCALLTYPE*)(void*,const Desc*,const D3D11_SUBRESOURCE_DATA*,void**);
                const auto create_owned=reinterpret_cast<CreateOwned>(vt(device)[5]);
                using SetAllocationHooks=void(WINAPI*)(GraphicsAllocate,GraphicsFree);
                const auto set_allocation_hooks=reinterpret_cast<SetAllocationHooks>(resolve_kernelx({"kernelx.dll","XMemSetAllocationHooks",0}));
                graphics_default_allocate=allocate; graphics_default_free=free_memory;
                esram_cpu_allocations=esram_ddr_allocations=0;
                set_allocation_hooks(esram_allocate_hook,esram_free_hook);
                texture=nullptr;
                auto invalid_esram=esram_desc; invalid_esram.extension[1]=1;
                check(create_owned(device,&invalid_esram,nullptr,&texture)==DXGI_ERROR_UNSUPPORTED && !texture,
                    "insufficient ESRAM coverage refuses a disconnected DDR substitution");
                const HRESULT esram_created=create_owned(device,&esram_desc,nullptr,&texture);
                check(esram_created==S_OK && texture,
                    "unplaced ESRAM texture owns a stable virtual alias of the existing physical pool");
                set_allocation_hooks(nullptr,nullptr);
                check(esram_cpu_allocations && !esram_ddr_allocations,"ESRAM-only texture preserves original XG CPU hooks without allocating DDR backing");
                unsigned char* owned_alias=nullptr;
                if (texture) {
                    using CreateSrv=HRESULT(STDMETHODCALLTYPE*)(void*,void*,const D3D11_SHADER_RESOURCE_VIEW_DESC*,void**);
                    void* view=nullptr;
                    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{};
                    srv_desc.Format=depth ? DXGI_FORMAT_R32_FLOAT : format; srv_desc.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D; srv_desc.Texture2D.MipLevels=1;
                    check(reinterpret_cast<CreateSrv>(vt(device)[7])(device,texture,&srv_desc,&view)==S_OK && view,
                        "ESRAM texture SRV retains the same physical image");
                    if (view) {
                        UINT32 base=0;std::memcpy(&base,static_cast<char*>(view)+48,4);
                        owned_alias=reinterpret_cast<unsigned char*>(UINT64(base)<<8);
                        check(owned_alias!=esram_mirror+mirror_offset && owned_alias[0]==0x35,"texture has an independent virtual view of the requested ESRAM byte");
                        owned_alias[0]=0x62;
                        check(esram_mirror[mirror_offset]==0x62,"ESRAM texture writes reach the title's real physical alias");
                        physical_page=original_page+1;
                        check(map_title_esram(1,esram_mirror,1,&physical_page)==S_OK,"title can remap its independent alias while the texture retains ownership");
                        esram_mirror[mirror_offset]=0x9a;
                        check(owned_alias[0]==0x62,"title remapping cannot change a texture's physical ESRAM identity");
                        physical_page=original_page;
                        check(map_title_esram(1,esram_mirror,1,&physical_page)==S_OK && esram_mirror[mirror_offset]==0x62,
                            "remapping back observes the original shared physical bytes");
                        reinterpret_cast<Count>(vt(view)[2])(view);
                    }
                    if (depth) {
                        using CreateDsv=HRESULT(STDMETHODCALLTYPE*)(void*,void*,const D3D11_DEPTH_STENCIL_VIEW_DESC*,void**);
                        D3D11_DEPTH_STENCIL_VIEW_DESC description{};
                        description.Format=DXGI_FORMAT_D32_FLOAT; description.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;
                        void* view=nullptr;
                        check(reinterpret_cast<CreateDsv>(vt(device)[10])(device,texture,&description,&view)==S_OK && view,
                            "typeless ESRAM depth owns a typed native D32 view of the same storage");
                        if (view) reinterpret_cast<Count>(vt(view)[2])(view);
                    }
                    reinterpret_cast<Count>(vt(texture)[2])(texture);
                }
                MEMORY_BASIC_INFORMATION retired_alias{};
                check(!owned_alias || (VirtualQuery(owned_alias,&retired_alias,sizeof retired_alias) && retired_alias.State==MEM_FREE),
                    "ESRAM texture retirement releases only its owned virtual view");
                check(esram_mirror[mirror_offset]==0x62,
                    "texture retirement preserves the physical pool and the title's mapping");
            }
            UINT buffer_page=80;
            check(map_title_esram(1,esram_mirror,1,&buffer_page)==S_OK,"buffer fixture maps the existing ESRAM pool");
            std::memset(esram_mirror,0x35,65536);
            struct EsramBufferDesc { D3D11_BUFFER_DESC standard; UINT64 extension; } esram_buffer{};
            esram_buffer.standard={128,D3D11_USAGE_DEFAULT,D3D11_BIND_SHADER_RESOURCE,0,0x20020,0};
            esram_buffer.extension=UINT64(buffer_page*65536+256)|(UINT64(128)<<32);
            using CreateEsramBuffer=HRESULT(STDMETHODCALLTYPE*)(void*,const EsramBufferDesc*,const D3D11_SUBRESOURCE_DATA*,void**);
            const auto create_esram_buffer=reinterpret_cast<CreateEsramBuffer>(vt(device)[3]);
            void* buffer=nullptr;
            check(create_esram_buffer(device,&esram_buffer,nullptr,nullptr)==S_FALSE,
                  "ESRAM buffer validation does not create a mapping");
            const D3D11_SUBRESOURCE_DATA esram_initial{esram_mirror,0,0};
            check(create_esram_buffer(device,&esram_buffer,&esram_initial,&buffer)==DXGI_ERROR_UNSUPPORTED && !buffer,
                  "ESRAM initialization remains explicit until writes over existing physical aliases are translated");
            esram_ddr_allocations=esram_cpu_allocations=0;
            using SetBufferHooks=void(WINAPI*)(void*,void*);
            const auto set_buffer_hooks=reinterpret_cast<SetBufferHooks>(resolve_kernelx({"kernelx.dll","XMemSetAllocationHooks",0}));
            set_buffer_hooks(reinterpret_cast<void*>(&esram_allocate_hook),reinterpret_cast<void*>(&esram_free_hook));
            const HRESULT buffer_created=create_esram_buffer(device,&esram_buffer,nullptr,&buffer);
            set_buffer_hooks(nullptr,nullptr);
            check(buffer_created==S_OK && buffer && !esram_ddr_allocations,
                  "ESRAM buffer owns a physical alias without allocating DDR pixels");
            unsigned char* buffer_alias=nullptr;
            if (buffer) {
                std::memcpy(&buffer_alias,static_cast<char*>(buffer)+48,sizeof buffer_alias);
                check(buffer_alias && buffer_alias!=esram_mirror+256 && buffer_alias[0]==0x35,
                      "ESRAM buffer has its own virtual alias at the requested physical byte");
                if (buffer_alias) {
                    buffer_alias[0]=0x62;
                    check(esram_mirror[256]==0x62,"buffer writes preserve shared ESRAM identity");
                    ++buffer_page;
                    check(map_title_esram(1,esram_mirror,1,&buffer_page)==S_OK,"title remaps its independent buffer alias");
                    esram_mirror[256]=0x9a;
                    check(buffer_alias[0]==0x62,"title remapping cannot redirect existing buffer storage");
                    --buffer_page;
                    check(map_title_esram(1,esram_mirror,1,&buffer_page)==S_OK && esram_mirror[256]==0x62,
                          "ESRAM physical bytes survive a temporary title remap");
                }
                using CreateRawSrv=HRESULT(STDMETHODCALLTYPE*)(void*,void*,const D3D11_SHADER_RESOURCE_VIEW_DESC*,void**);
                const auto create_raw_srv=reinterpret_cast<CreateRawSrv>(vt(device)[7]);
                D3D11_SHADER_RESOURCE_VIEW_DESC raw{};
                raw.Format=DXGI_FORMAT_R32_TYPELESS; raw.ViewDimension=D3D11_SRV_DIMENSION_BUFFEREX;
                raw.BufferEx={4,8,D3D11_BUFFEREX_SRV_FLAG_RAW};
                void* raw_view=nullptr;
                check(create_raw_srv(device,buffer,&raw,nullptr)==S_FALSE,"raw SRV validation does not acquire ownership");
                check(create_raw_srv(device,buffer,&raw,&raw_view)==S_OK && raw_view,"raw SRV binds the actual ESRAM buffer");
                if (raw_view) {
                    using FastResources=void(STDMETHODCALLTYPE*)(void*,const void*,const void*);
                    const auto fast=reinterpret_cast<FastResources>(vt(context)[209]);
                    std::array<UINT32,5> packet{0x80040001};
                    const auto view_address=reinterpret_cast<std::uintptr_t>(raw_view);
                    std::memcpy(packet.data()+1,&view_address,sizeof view_address);
                    fast(context,packet.data(),packet.data()+packet.size());
                    check(reinterpret_cast<Count>(vt(raw_view)[1])(raw_view)==3,"SetFastResources retains the signed packet SRV object");
                    reinterpret_cast<Count>(vt(raw_view)[2])(raw_view);
                    fast(context,packet.data(),packet.data()+packet.size());
                    check(reinterpret_cast<Count>(vt(raw_view)[1])(raw_view)==3,"fast SRV rebinding balances ownership");
                    reinterpret_cast<Count>(vt(raw_view)[2])(raw_view);
                    static unsigned rejections=0; rejections=0;
                    set_d3d11x_trap_handler([](const char* name,unsigned slot)->std::uintptr_t {
                        if(std::strcmp(name,"ID3D11DeviceContextX")==0 && slot==209) ++rejections;
                        return 0;
                    });
                    fast(context,packet.data(),packet.data()+4);
                    auto invalid=packet; invalid[3]=1;
                    fast(context,invalid.data(),invalid.data()+invalid.size());
                    invalid=packet; invalid[0]=0x80047f02;
                    fast(context,invalid.data(),invalid.data()+invalid.size());
                    invalid=packet; invalid[1]=1;invalid[2]=0;
                    fast(context,invalid.data(),invalid.data()+invalid.size());
                    std::array<UINT32,10> partial{0x80040001,0,0,0,0,0x80000001,0,0,0,0};
                    fast(context,partial.data(),partial.data()+partial.size());
                    set_d3d11x_trap_handler(nullptr);
                    check(rejections==5 && reinterpret_cast<Count>(vt(raw_view)[1])(raw_view)==3,
                          "truncated, metadata, range, pointer and unsupported-tail failures preserve prior fast bindings transactionally");
                    reinterpret_cast<Count>(vt(raw_view)[2])(raw_view);
                    fast(context,packet.data(),packet.data());
                    packet[0]=0x80000000;
                    fast(context,packet.data(),packet.data()+1);
                    packet[0]=0x80040001;packet[1]=packet[2]=0;
                    fast(context,packet.data(),packet.data()+packet.size());
                    check(reinterpret_cast<Count>(vt(raw_view)[1])(raw_view)==2,"fast null SRD unbind and empty packets preserve exact ownership");
                    reinterpret_cast<Count>(vt(raw_view)[2])(raw_view);
                    using BindResources=void(STDMETHODCALLTYPE*)(void*,UINT,UINT,void* const*);
                    const auto bind_vertex=reinterpret_cast<BindResources>(vt(context)[25]);
                    const auto bind_pixel=reinterpret_cast<BindResources>(vt(context)[9]);
                    const auto bind_compute=reinterpret_cast<BindResources>(vt(context)[67]);
                    void* views[]{raw_view};
                    bind_vertex(context,127,1,views);bind_vertex(context,127,1,views);
                    check(reinterpret_cast<Count>(vt(raw_view)[1])(raw_view)==3,"legacy VS SRV last-slot binding and rebind retain the same view");
                    reinterpret_cast<Count>(vt(raw_view)[2])(raw_view);
                    bind_vertex(context,127,1,nullptr);bind_vertex(context,128,0,nullptr);
                    check(reinterpret_cast<Count>(vt(raw_view)[1])(raw_view)==2,"legacy VS SRV unbind releases its retained view");
                    reinterpret_cast<Count>(vt(raw_view)[2])(raw_view);
                    bind_vertex(context,0,1,views);
                    bind_pixel(context,0,1,views);
                    bind_compute(context,0,1,views);bind_compute(context,0,1,views);
                    check(reinterpret_cast<Count>(vt(raw_view)[1])(raw_view)==5,"VS, PS and CS SRV bindings own independent references and balance rebinds");
                    reinterpret_cast<Count>(vt(raw_view)[2])(raw_view);
                    reinterpret_cast<void(STDMETHODCALLTYPE*)(void*)>(vt(context)[110])(context);
                    check(reinterpret_cast<Count>(vt(raw_view)[1])(raw_view)==2,"ClearState releases VS SRVs with the other common bindings");
                    reinterpret_cast<Count>(vt(raw_view)[2])(raw_view);
                    struct { D3D11_SHADER_RESOURCE_VIEW_DESC desc{}; UINT guard=0xaabbccdd; } queried;
                    reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,D3D11_SHADER_RESOURCE_VIEW_DESC*)>(vt(raw_view)[9])(raw_view,&queried.desc);
                    check(!std::memcmp(&queried.desc,&raw,sizeof raw) && queried.guard==0xaabbccdd,
                          "raw SRV preserves BUFFEREX flags and exact query extent");
                    std::array<UINT32,8> srd{};
                    std::memcpy(srd.data(),static_cast<char*>(raw_view)+48,sizeof srd);
                    check((UINT64(srd[0])|(UINT64(srd[1]&255)<<32))==reinterpret_cast<std::uintptr_t>(buffer_alias)+16 &&
                          !(srd[1]&0xffffff00) && srd[2]==32 && srd[3]==0x24fac &&
                          std::all_of(srd.begin()+4,srd.end(),[](auto x){return x==0;}),
                          "original raw SRD counts bytes, clears stride and preserves the physical base");
                    reinterpret_cast<Count>(vt(raw_view)[2])(raw_view);
                }
                raw.BufferEx.FirstElement=1;
                check(create_raw_srv(device,buffer,&raw,&raw_view)==DXGI_ERROR_UNSUPPORTED && !raw_view,
                      "raw view refuses an offset that violates host alignment");
                raw.BufferEx.FirstElement=32;
                check(create_raw_srv(device,buffer,&raw,&raw_view)==E_INVALIDARG && !raw_view,
                      "raw SRV range cannot exceed its logical buffer");
                reinterpret_cast<Count>(vt(buffer)[2])(buffer);
            }
            MEMORY_BASIC_INFORMATION buffer_retired{};
            check(buffer_alias && VirtualQuery(buffer_alias,&buffer_retired,sizeof buffer_retired) && buffer_retired.State==MEM_FREE && esram_mirror[256]==0x62,
                  "buffer retirement frees its alias while preserving pool and title ownership");
            esram_buffer.extension=UINT64(buffer_page*65536)|(UINT64(64)<<32);
            check(create_esram_buffer(device,&esram_buffer,nullptr,&buffer)==DXGI_ERROR_UNSUPPORTED && !buffer,
                  "partly ESRAM buffer remains explicit without a translated DDR tail");
            esram_buffer.extension=0x1fffff0;
            check(create_esram_buffer(device,&esram_buffer,nullptr,&buffer)==DXGI_ERROR_UNSUPPORTED && !buffer,
                  "buffer cannot extend beyond the physical ESRAM pool");
            check(release_virtual(esram_mirror,0,MEM_RELEASE),"ESRAM fixture releases its own title mapping");
            {
                using PlaceBuffer=HRESULT(STDMETHODCALLTYPE*)(void*,const EsramBufferDesc*,void*,void**);
                const auto place_buffer=reinterpret_cast<PlaceBuffer>(vt(device)[62]);
                auto* storage=static_cast<unsigned char*>(allocate(1024,attributes));
                if (!storage) return 1;
                std::memset(storage,0x35,1024);
                EsramBufferDesc placed{};
                placed.standard={128,D3D11_USAGE_IMMUTABLE,D3D11_BIND_CONSTANT_BUFFER,0,0,0};
                void* buffer=nullptr;
                check(place_buffer(device,&placed,storage+256,nullptr)==S_FALSE,"placement buffer validation does not acquire backing ownership");
                check(place_buffer(device,&placed,storage+256,&buffer)==S_OK && buffer,"placement constant buffer pins the supplied canonical range");
                if (buffer) {
                    void* base=nullptr;std::memcpy(&base,static_cast<char*>(buffer)+48,sizeof base);
                    std::array<UINT32,4> srd{};std::memcpy(srd.data(),static_cast<char*>(buffer)+32,sizeof srd);
                    check(base==storage+256 && (UINT64(srd[0])|(UINT64(srd[1]&255)<<32))==reinterpret_cast<std::uintptr_t>(base) &&
                          (srd[1]&0x3fff0000)==(16u<<16) && srd[2]==8,"placement CBV preserves physical base, sixteen-byte stride and count");
                    struct { EsramBufferDesc desc{}; UINT64 guard=0x1122334455667788ull; } queried;
                    reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,EsramBufferDesc*)>(vt(buffer)[12])(buffer,&queried.desc);
                    check(!std::memcmp(&queried.desc,&placed,sizeof placed) && queried.guard==0x1122334455667788ull,
                          "placement buffer GetDesc preserves caller semantics and exact ABI extent");
                    reinterpret_cast<Count>(vt(buffer)[2])(buffer);
                }
                MEMORY_BASIC_INFORMATION info{};
                check(VirtualQuery(storage,&info,sizeof info) && info.State==MEM_COMMIT &&
                      std::all_of(storage,storage+1024,[](auto x){return x==0x35;}),
                      "placement retirement leaves borrowed allocation and every guard intact");
                placed.standard={128,D3D11_USAGE_DEFAULT,D3D11_BIND_SHADER_RESOURCE,0,D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS,0};
                check(place_buffer(device,&placed,storage+512,&buffer)==S_OK && buffer,"placement raw buffer reuses the same canonical allocation");
                free_memory(storage,attributes);
                check(VirtualQuery(storage,&info,sizeof info) && info.State==MEM_COMMIT,"live placement buffer defers title allocation retirement");
                if (buffer) reinterpret_cast<Count>(vt(buffer)[2])(buffer);
                check(VirtualQuery(storage,&info,sizeof info) && info.State==MEM_FREE,"last placement owner retires the allocation the title already released");
                check(FAILED(place_buffer(device,&placed,reinterpret_cast<void*>(1),&buffer)) && !buffer,
                      "placement refuses an unregistered address without acquiring or freeing it");
                check(place_buffer(device,&placed,reinterpret_cast<void*>(0x10000000000ull-64),&buffer)==DXGI_ERROR_UNSUPPORTED && !buffer,
                      "placement rejects a span extending beyond the native forty-bit descriptor address range");
                placed.standard.BindFlags=D3D11_BIND_VERTEX_BUFFER;
                check(place_buffer(device,&placed,reinterpret_cast<void*>(1),&buffer)==DXGI_ERROR_UNSUPPORTED && !buffer,
                      "placement buffer keeps unsupported binding semantics explicit");
            }
            for (const auto storage_format:{DXGI_FORMAT_R8_TYPELESS,DXGI_FORMAT_R10G10B10A2_TYPELESS,DXGI_FORMAT_R16G16_FLOAT}) {
                Desc color{};
                color.standard={32,16,1,1,storage_format,{1,0},D3D11_USAGE_DEFAULT,
                    D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET|D3D11_BIND_UNORDERED_ACCESS,0,0};
                using CreateOwned=HRESULT(STDMETHODCALLTYPE*)(void*,const Desc*,const D3D11_SUBRESOURCE_DATA*,void**);
                texture=nullptr;
                check(reinterpret_cast<CreateOwned>(vt(device)[5])(device,&color,nullptr,&texture)==S_OK && texture,
                    "typeless color has one native texture for typed SRV RTV and UAV aliases");
                if (!texture) continue;
                const DXGI_FORMAT formats[]{storage_format==DXGI_FORMAT_R16G16_FLOAT ? storage_format : storage_format==DXGI_FORMAT_R8_TYPELESS ? DXGI_FORMAT_R8_UNORM : DXGI_FORMAT_R10G10B10A2_UNORM,
                    storage_format==DXGI_FORMAT_R16G16_FLOAT ? storage_format : storage_format==DXGI_FORMAT_R8_TYPELESS ? DXGI_FORMAT_R8_UINT : DXGI_FORMAT_R10G10B10A2_UINT};
                const std::array<DXGI_FORMAT,3> views{formats[0],formats[1],DXGI_FORMAT_R32_UINT};
                for (const auto format:views) for (UINT kind=0;kind<3;++kind) {
                    if (format==DXGI_FORMAT_R32_UINT && (storage_format!=DXGI_FORMAT_R10G10B10A2_TYPELESS || kind!=2)) continue;
                    using Factory=HRESULT(STDMETHODCALLTYPE*)(void*,void*,const void*,void**);
                    const auto factory=reinterpret_cast<Factory>(vt(device)[kind==0 ? 7 : kind==1 ? 9 : 8]);
                    D3D11_SHADER_RESOURCE_VIEW_DESC srv{}; srv.Format=format; srv.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D; srv.Texture2D.MipLevels=1;
                    D3D11_RENDER_TARGET_VIEW_DESC rtv{}; rtv.Format=format; rtv.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2D;
                    D3D11_UNORDERED_ACCESS_VIEW_DESC uav{}; uav.Format=format; uav.ViewDimension=D3D11_UAV_DIMENSION_TEXTURE2D;
                    const void* input=kind==0 ? static_cast<const void*>(&srv) : kind==1 ? static_cast<const void*>(&rtv) : &uav;
                    void* view=nullptr;
                    check(factory(device,texture,input,nullptr)==S_FALSE,"typed color view validates without taking ownership");
                    check(factory(device,texture,input,&view)==S_OK && view,"typed color view owns a real native descriptor");
                    if (!view) continue;
                    alignas(8) std::array<unsigned char,32> result;
                    result.fill(0x5a);
                    const SIZE_T bytes=kind==0 ? sizeof srv : kind==1 ? sizeof rtv : sizeof uav;
                    reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*)>(vt(view)[9])(view,result.data());
                    check(!std::memcmp(result.data(),input,bytes) && std::all_of(result.begin()+bytes,result.end(),[](auto x){return x==0x5a;}),
                        "typed color view queries preserve their format dimension and ABI bounds");
                    if (kind!=1) {
                        std::array<UINT32,8> descriptor{}; void* canonical=nullptr;
                        std::memcpy(descriptor.data(),static_cast<char*>(view)+48,sizeof descriptor);
                        std::memcpy(&canonical,static_cast<char*>(view)+80,sizeof canonical);
                        const bool integer=format==DXGI_FORMAT_R8_UINT || format==DXGI_FORMAT_R10G10B10A2_UINT || format==DXGI_FORMAT_R32_UINT;
                        check(descriptor[0]==(reinterpret_cast<std::uintptr_t>(canonical)>>8) &&
                            ((descriptor[1]>>26)&15)==(integer ? 4u : format==DXGI_FORMAT_R16G16_FLOAT ? 7u : 0u),"typed SRV/UAV SRDs keep physical identity and native numeric interpretation");
                    }
                    if (kind==1 && format==formats[0]) {
                        using SetTargets=void(STDMETHODCALLTYPE*)(void*,UINT,void* const*,void*);
                        const auto bind_targets=reinterpret_cast<SetTargets>(vt(context)[33]);
                        void* targets[]{view};
                        check(reinterpret_cast<Count>(vt(view)[1])(view)==2,"RTV fixture observes its public reference count");
                        reinterpret_cast<Count>(vt(view)[2])(view);
                        bind_targets(context,1,targets,nullptr);
                        check(reinterpret_cast<Count>(vt(view)[1])(view)==3,"legacy slot 33 retains its real render target view");
                        reinterpret_cast<Count>(vt(view)[2])(view);
                        using GetTargets=void(STDMETHODCALLTYPE*)(void*,UINT,void**,void**);
                        const auto get_targets=reinterpret_cast<GetTargets>(vt(context)[89]);
                        void* returned[9]{};returned[8]=reinterpret_cast<void*>(1);void* returned_depth=reinterpret_cast<void*>(1);
                        get_targets(context,8,returned,&returned_depth);
                        check(returned[0]==view && !returned[1] && !returned[7] && returned[8]==reinterpret_cast<void*>(1) && !returned_depth,
                              "legacy OMGetRenderTargets returns retained bindings, null holes and preserves the output guard");
                        check(reinterpret_cast<Count>(vt(view)[1])(view)==4,"OMGetRenderTargets owns a reference independent of the context binding");
                        reinterpret_cast<Count>(vt(view)[2])(view);
                        if(returned[0])reinterpret_cast<Count>(vt(returned[0])[2])(returned[0]);
                        get_targets(context,0,nullptr,nullptr);
                        reinterpret_cast<void(STDMETHODCALLTYPE*)(void*)>(vt(context)[110])(context);
                        check(reinterpret_cast<Count>(vt(view)[1])(view)==2,"ClearState releases the render binding while retaining caller view ownership");
                        reinterpret_cast<Count>(vt(view)[2])(view);
                        returned[0]=reinterpret_cast<void*>(1);get_targets(context,1,returned,nullptr);
                        check(!returned[0],"OMGetRenderTargets observes ClearState through the same binding state");
                        constexpr FLOAT white[]{1,1,1,1};
                        reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*,const FLOAT*)>(vt(context)[50])(context,view,white);
                        void* alias=nullptr;
                        // The SRV exposes the same canonical pixels as this RTV.
                        void* srv_view=nullptr;
                        using MakeSrv=HRESULT(STDMETHODCALLTYPE*)(void*,void*,const D3D11_SHADER_RESOURCE_VIEW_DESC*,void**);
                        check(reinterpret_cast<MakeSrv>(vt(device)[7])(device,texture,&srv,&srv_view)==S_OK && srv_view,
                              "legacy RTV clear keeps the resource visible through its original SRV");
                        if (srv_view) {
                            std::memcpy(&alias,static_cast<char*>(srv_view)+80,sizeof alias);
                            TitleTextureLayout pixels;
                            const UINT element=storage_format==DXGI_FORMAT_R8_TYPELESS ? 1 : 4;
                            hr=create_xg_texture_layout(32,16,1,1,storage_format,UINT_MAX,pixels,color.standard.BindFlags);
                            std::vector<unsigned char> linear(32*16*element);
                            check(SUCCEEDED(hr) && pixels.copy(pixels.context.get(),false,linear.data(),alias,0,32*element,32*16*element)==S_OK,
                                  "legacy clear publishes real GPU pixels through the original XG layout");
                            bool exact=true;
                            for (SIZE_T at=0;at<linear.size();++at) exact=exact && linear[at]==(storage_format==DXGI_FORMAT_R16G16_FLOAT ? (at%2 ? 0x3c : 0) : 0xff);
                            check(exact,"legacy context slot 50 clears every pixel with exact typed values");
                            if (storage_format==DXGI_FORMAT_R8_TYPELESS) {
                                using Deferred=HRESULT(STDMETHODCALLTYPE*)(void*,UINT,void**);
                                using Update=void(STDMETHODCALLTYPE*)(void*,void*,UINT,const D3D11_BOX*,const void*,UINT,UINT);
                                using FinishUpdate=HRESULT(STDMETHODCALLTYPE*)(void*,BOOL,void**);
                                using ExecuteUpdate=void(STDMETHODCALLTYPE*)(void*,void*,BOOL);
                                auto update=reinterpret_cast<Update>(vt(context)[48]);
                                std::vector<unsigned char> upload(40*16,0xee);
                                for(UINT y=0;y<16;++y) std::fill_n(upload.data()+40*y,32,0x51);
                                update(context,texture,0,nullptr,upload.data(),40,640);
                                check(pixels.copy(pixels.context.get(),false,linear.data(),alias,0,32,512)==S_OK &&
                                    std::all_of(linear.begin(),linear.end(),[](auto value){return value==0x51;}),
                                    "immediate UpdateSubresource preserves padded source rows through the original tiled layout");
                                void* updater=nullptr;void* update_list=nullptr;
                                check(reinterpret_cast<Deferred>(vt(device)[27])(device,0x60000,&updater)==S_OK && updater,
                                      "UpdateSubresource uses the existing deferred context");
                                if(updater) {
                                    const FLOAT black[]{0,0,0,0};
                                    reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*,const FLOAT*)>(vt(updater)[50])(updater,view,black);
                                    reinterpret_cast<void(STDMETHODCALLTYPE*)(void*)>(vt(updater)[153])(updater);
                                    std::fill(upload.begin(),upload.end(),0xb3);
                                    reinterpret_cast<Update>(vt(updater)[48])(updater,texture,0,nullptr,upload.data(),40,640);
                                    check(reinterpret_cast<FinishUpdate>(vt(updater)[114])(updater,FALSE,&update_list)==S_OK && update_list,
                                          "deferred UpdateSubresource owns a snapshot of the source data");
                                    std::fill(upload.begin(),upload.end(),0);
                                    check(pixels.copy(pixels.context.get(),false,linear.data(),alias,0,32,512)==S_OK &&
                                        std::all_of(linear.begin(),linear.end(),[](auto value){return value==0x51;}),
                                        "recording and FinishCommandList do not update destination pixels");
                                    reinterpret_cast<Count>(vt(updater)[2])(updater);
                                    for(UINT pass=0;pass<2 && update_list;++pass) {
                                        reinterpret_cast<ExecuteUpdate>(vt(context)[58])(context,update_list,FALSE);
                                        reinterpret_cast<void(STDMETHODCALLTYPE*)(void*)>(vt(context)[153])(context);
                                        check(pixels.copy(pixels.context.get(),false,linear.data(),alias,0,32,512)==S_OK &&
                                            std::all_of(linear.begin(),linear.end(),[](auto value){return value==0xb3;}),
                                            "clear then update executes in order and reuses the owned upload after producer release");
                                    }
                                    if(update_list) reinterpret_cast<Count>(vt(update_list)[2])(update_list);
                                }
                                struct UpdateBufferDesc {D3D11_BUFFER_DESC standard;UINT64 extension;} update_desc{{16,D3D11_USAGE_DEFAULT,D3D11_BIND_VERTEX_BUFFER,0,0,0},0};
                                std::array<unsigned char,16> original{};original.fill(0x11);
                                const D3D11_SUBRESOURCE_DATA update_initial{original.data(),0,0};
                                void* update_buffer=nullptr;updater=nullptr;update_list=nullptr;
                                using CreateUpdateBuffer=HRESULT(STDMETHODCALLTYPE*)(void*,const UpdateBufferDesc*,const D3D11_SUBRESOURCE_DATA*,void**);
                                check(reinterpret_cast<CreateUpdateBuffer>(vt(device)[3])(device,&update_desc,&update_initial,&update_buffer)==S_OK && update_buffer,
                                      "partial update creates a real default buffer");
                                if(update_buffer) {
                                    unsigned char* buffer_bytes=nullptr;
                                    std::memcpy(&buffer_bytes,static_cast<char*>(update_buffer)+48,sizeof buffer_bytes);
                                    check(reinterpret_cast<Deferred>(vt(device)[27])(device,0,&updater)==S_OK && updater,
                                          "partial buffer update creates a tracked deferred context");
                                    if(updater) {
                                        D3D11_BOX box{4,0,0,12,1,1};std::array<unsigned char,8> source{};source.fill(0xa7);
                                        reinterpret_cast<Update>(vt(updater)[48])(updater,update_buffer,0,&box,source.data(),0,0);
                                        check(reinterpret_cast<FinishUpdate>(vt(updater)[114])(updater,FALSE,&update_list)==S_OK && update_list &&
                                              buffer_bytes && !std::memcmp(buffer_bytes,original.data(),original.size()),
                                              "partial buffer update records bytes without changing its destination");
                                        source.fill(0);reinterpret_cast<Count>(vt(updater)[2])(updater);
                                    }
                                    reinterpret_cast<Count>(vt(update_buffer)[2])(update_buffer);
                                    for(UINT pass=0;pass<2 && update_list;++pass) {
                                        reinterpret_cast<ExecuteUpdate>(vt(context)[58])(context,update_list,FALSE);
                                        bool exact_buffer=true;
                                        for(UINT i=0;i<16;++i) exact_buffer=exact_buffer && buffer_bytes[i]==(i>=4 && i<12 ? 0xa7 : 0x11);
                                        check(exact_buffer,"partial buffer list retains the destination, source snapshot and untouched bytes across reuse");
                                    }
                                    if(update_list) reinterpret_cast<Count>(vt(update_list)[2])(update_list);
                                }
                                reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*,const FLOAT*)>(vt(context)[50])(context,view,white);
                                void* deferred=nullptr;
                                check(reinterpret_cast<Deferred>(vt(device)[27])(device,0x60000,&deferred)==S_OK && deferred,
                                      "observed deferred mode has its own real context state");
                                if (deferred) {
                                    const std::vector<unsigned char> before(static_cast<unsigned char*>(alias),static_cast<unsigned char*>(alias)+pixels.bytes);
                                    reinterpret_cast<SetTargets>(vt(deferred)[33])(deferred,1,targets,nullptr);
                                    void* deferred_view=reinterpret_cast<void*>(1);void* deferred_depth=reinterpret_cast<void*>(1);
                                    reinterpret_cast<GetTargets>(vt(deferred)[89])(deferred,1,&deferred_view,&deferred_depth);
                                    check(deferred_view==view && !deferred_depth,"tracked deferred OM query returns its retained render target binding");
                                    if(deferred_view) reinterpret_cast<Count>(vt(deferred_view)[2])(deferred_view);
                                    static UINT clear_slot=0;
                                    set_d3d11x_trap_handler([](const char*,UINT slot)->std::uintptr_t{clear_slot=slot;return 0;});
                                    const FLOAT black[]{0,0,0,0};
                                    reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*,const FLOAT*)>(vt(deferred)[50])(deferred,view,black);
                                    reinterpret_cast<void(STDMETHODCALLTYPE*)(void*)>(vt(deferred)[111])(deferred);
                                    set_d3d11x_trap_handler(nullptr);
                                    check(clear_slot==0 && !std::memcmp(before.data(),alias,before.size()),
                                          "deferred clear records without executing or modifying canonical pixels");
                                    reinterpret_cast<void(STDMETHODCALLTYPE*)(void*)>(vt(deferred)[110])(deferred);
                                    check(!std::memcmp(before.data(),alias,before.size()),
                                          "deferred ClearState releases bindings without submitting GPU work or changing physical pixels");
                                    using Finish=HRESULT(STDMETHODCALLTYPE*)(void*,BOOL,void**);
                                    using Execute=void(STDMETHODCALLTYPE*)(void*,void*,BOOL);
                                    void* list=nullptr;
                                    check(reinterpret_cast<Finish>(vt(deferred)[114])(deferred,FALSE,&list)==S_OK && list,
                                          "deferred clear survives ClearState and finishes without executing");
                                    if(list) {
                                        reinterpret_cast<Execute>(vt(context)[58])(context,list,FALSE);
                                        check(pixels.copy(pixels.context.get(),false,linear.data(),alias,0,32,512)==S_OK &&
                                            std::all_of(linear.begin(),linear.end(),[](auto v){return v==0;}),
                                            "ExecuteCommandList publishes the recorded clear after its real fence");
                                        reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*,const FLOAT*)>(vt(context)[50])(context,view,white);
                                        reinterpret_cast<Execute>(vt(context)[58])(context,list,TRUE);
                                        check(pixels.copy(pixels.context.get(),false,linear.data(),alias,0,32,512)==S_OK &&
                                            std::all_of(linear.begin(),linear.end(),[](auto v){return v==0;}),
                                            "reused clear list sees current resource ownership and clears again");
                                        reinterpret_cast<Count>(vt(list)[2])(list);
                                    }
                                    reinterpret_cast<Count>(vt(deferred)[2])(deferred);
                                }
                                deferred=nullptr;
                                check(reinterpret_cast<Deferred>(vt(device)[27])(device,0,&deferred)==S_OK && deferred,
                                      "binding-only list creates a normal deferred context");
                                if(deferred) {
                                    const ULONG baseline=reinterpret_cast<Count>(vt(view)[1])(view);
                                    reinterpret_cast<Count>(vt(view)[2])(view);
                                    bind_targets(deferred,1,targets,nullptr);
                                    reinterpret_cast<void(STDMETHODCALLTYPE*)(void*)>(vt(deferred)[110])(deferred);
                                    void* list=nullptr;
                                    using Finish=HRESULT(STDMETHODCALLTYPE*)(void*,BOOL,void**);
                                    check(reinterpret_cast<Finish>(vt(deferred)[114])(deferred,FALSE,&list)==S_OK && list,
                                          "binding-only list finishes after unbinding without requiring shaders");
                                    reinterpret_cast<Count>(vt(deferred)[2])(deferred);
                                    check(reinterpret_cast<Count>(vt(view)[1])(view)==baseline+1,
                                          "recorded bindings retain resources after unbinding and producer destruction, even without draws");
                                    reinterpret_cast<Count>(vt(view)[2])(view);
                                    if(list) reinterpret_cast<Count>(vt(list)[2])(list);
                                    check(reinterpret_cast<Count>(vt(view)[1])(view)==baseline,"list release retires binding-only references exactly once");
                                    reinterpret_cast<Count>(vt(view)[2])(view);
                                }
                                const HMODULE compiler=LoadLibraryExW(L"d3dcompiler_47.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
                                const auto compile=compiler ? reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(compiler,"D3DCompile")) : nullptr;
                                const char* sources[]{
                                    "float4 main(uint id:SV_VertexID):SV_Position {float2 p=float2((id<<1)&2,id&2);return float4(p*float2(2,-2)+float2(-1,1),0,1);}",
                                    "float4 main():SV_Target{return float4(.25,0,0,1);}",
                                    "float4 main():SV_Target{return float4(.75,0,0,1);}",
                                    "float4 main(float2 p:POSITION,float2 u:TEXCOORD0):SV_Position{return float4(p+u,0,1);}",
                                    "float4 main(float2 p:POSITION,uint id:SV_VertexID):SV_Position{return float4(float2(id&1,id>>1)*float2(2,-2)+float2(-1,1)+p,0,1);}"};
                                void* shaders[5]{};void* empty_layout=nullptr;void* vertex_layout=nullptr;void* vertex_buffer=nullptr;bool ready=compile!=nullptr;
                                using CreateShader=HRESULT(STDMETHODCALLTYPE*)(void*,const void*,SIZE_T,void*,void**);
                                for(UINT i=0;i<5 && ready;++i) {
                                    Microsoft::WRL::ComPtr<ID3DBlob> code;
                                    const bool vertex=i==0 || i==3 || i==4;
                                    ready=SUCCEEDED(compile(sources[i],std::strlen(sources[i]),nullptr,nullptr,nullptr,"main",vertex ? "vs_5_0" : "ps_5_0",0,0,&code,nullptr));
                                    if(ready) ready=reinterpret_cast<CreateShader>(vt(device)[vertex ? 12 : 15])(device,code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shaders[i])==S_OK;
                                    if(ready && vertex && i != 4) {
                                        using CreateLayout=HRESULT(STDMETHODCALLTYPE*)(void*,const D3D12_INPUT_ELEMENT_DESC*,UINT,const void*,SIZE_T,void**);
                                        const D3D12_INPUT_ELEMENT_DESC elements[]{{"POSITION",0,DXGI_FORMAT_R32G32_FLOAT,0,0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
                                            {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,D3D12_APPEND_ALIGNED_ELEMENT,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0}};
                                        ready=reinterpret_cast<CreateLayout>(vt(device)[11])(device,i ? elements : nullptr,i ? 2 : 0,code->GetBufferPointer(),code->GetBufferSize(),i ? &vertex_layout : &empty_layout)==S_OK;
                                    }
                                }
                                const FLOAT vertices[]{99,99,0,0,-1,1,0,0,3,1,0,0,-1,-3,0,0};
                                if(ready) {
                                    struct VertexDesc { D3D11_BUFFER_DESC standard;UINT64 extension; } vertex_desc{{sizeof vertices,D3D11_USAGE_DEFAULT,D3D11_BIND_VERTEX_BUFFER,0,0,0},0};
                                    const D3D11_SUBRESOURCE_DATA initial{vertices,0,0};
                                    using CreateBuffer=HRESULT(STDMETHODCALLTYPE*)(void*,const VertexDesc*,const D3D11_SUBRESOURCE_DATA*,void**);
                                    ready=reinterpret_cast<CreateBuffer>(vt(device)[3])(device,&vertex_desc,&initial,&vertex_buffer)==S_OK;
                                }
                                check(ready,"legacy procedural draw creates real VS/PS shader objects");
                                if(ready) {
                                    bind_targets(context,1,targets,nullptr);
                                    using Draw=void(STDMETHODCALLTYPE*)(void*,UINT,UINT);
                                    const auto draw=reinterpret_cast<Draw>(vt(context)[14]);
                                    const D3D11_VIEWPORT viewport{0,0,32,16,0,1};
                                    reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,UINT,const D3D11_VIEWPORT*)>(vt(context)[44])(context,1,&viewport);
                                    const UINT64 topology=0x000009ffc0009e00ull;
                                    std::memcpy(static_cast<char*>(context)+0x170,&topology,sizeof topology);
                                    std::memcpy(static_cast<char*>(context)+0x180,&shaders[0],sizeof(void*));
                                    std::memcpy(static_cast<char*>(context)+0x178,&empty_layout,sizeof empty_layout);
                                    for(UINT pass=0;pass<2;++pass) {
                                        std::memcpy(static_cast<char*>(context)+0x188,&shaders[1+pass],sizeof(void*));
                                        draw(context,3,0);
                                        const HRESULT copied=pixels.copy(pixels.context.get(),false,linear.data(),alias,0,32,512);
                                        check(SUCCEEDED(copied) && std::all_of(linear.begin(),linear.end(),[&](auto value){return value==(pass ? 191 : 64);}),
                                              "procedural draw publishes all pixels after its fence and refreshes inline shader changes");
                                    }
                                    check(reinterpret_cast<Count>(vt(shaders[1])[1])(shaders[1])==2,"inline PS replacement releases the prior shader binding");
                                    reinterpret_cast<Count>(vt(shaders[1])[2])(shaders[1]);
                                    std::memcpy(static_cast<char*>(context)+0x180,&shaders[3],sizeof(void*));
                                    std::memcpy(static_cast<char*>(context)+0x178,&vertex_layout,sizeof vertex_layout);
                                    using BindVertices=void(STDMETHODCALLTYPE*)(void*,UINT,UINT,void* const*,const UINT*,const UINT*);
                                    const auto bind_vertices=reinterpret_cast<BindVertices>(vt(context)[19]);
                                    const UINT stride=16,offset=0;
                                    bind_vertices(context,0,1,&vertex_buffer,&stride,&offset);
                                    const FLOAT black[]{0,0,0,0};
                                    reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*,const FLOAT*)>(vt(context)[50])(context,view,black);
                                    draw(context,3,1);
                                    check(pixels.copy(pixels.context.get(),false,linear.data(),alias,0,32,512)==S_OK &&
                                        std::all_of(linear.begin(),linear.end(),[](auto value){return value==191;}),
                                        "non-indexed draw preserves first vertex, stride, appended input elements and inline layout changes");
                                    reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*,const FLOAT*)>(vt(context)[50])(context,view,black);
                                    void* null_vertex = nullptr; const UINT zero = 0;
                                    bind_vertices(context,0,1,&null_vertex,&zero,&zero);
                                    std::memcpy(static_cast<char*>(context)+0x180,&shaders[4],sizeof(void*));
                                    const UINT64 strip = 0x00000dffc0009e00ull;
                                    std::memcpy(static_cast<char*>(context)+0x170,&strip,sizeof strip);
                                    draw(context,4,0);
                                    check(pixels.copy(pixels.context.get(),false,linear.data(),alias,0,32,512)==S_OK &&
                                        std::all_of(linear.begin(),linear.end(),[](auto value){return value==191;}),
                                        "four-vertex strip covers both triangles and null vertex input reads native zeroes after its fence");
                                    std::memcpy(static_cast<char*>(context)+0x180,&shaders[3],sizeof(void*));
                                    std::memcpy(static_cast<char*>(context)+0x170,&topology,sizeof topology);
                                    bind_vertices(context,0,1,&vertex_buffer,&stride,&offset);
                                    void* vertex_bytes=nullptr;std::memcpy(&vertex_bytes,static_cast<char*>(vertex_buffer)+48,sizeof vertex_bytes);
                                    check(vertex_bytes && !std::memcmp(vertex_bytes,vertices,sizeof vertices),"non-indexed draw preserves its canonical vertex bytes");
                                    const std::vector<unsigned char> before(static_cast<unsigned char*>(alias),static_cast<unsigned char*>(alias)+pixels.bytes);
                                    static UINT rejected_slot=0;rejected_slot=0;
                                    set_d3d11x_trap_handler([](const char*,UINT slot)->std::uintptr_t{rejected_slot=slot;return 0;});
                                    void* invalid_shader=reinterpret_cast<void*>(1);
                                    std::memcpy(static_cast<char*>(context)+0x188,&invalid_shader,sizeof invalid_shader);
                                    draw(context,3,0);
                                    check(rejected_slot==14 && !std::memcmp(before.data(),alias,before.size()),"invalid inline shader refuses draw without changing canonical pixels");
                                    std::memcpy(static_cast<char*>(context)+0x188,&shaders[2],sizeof(void*));
                                    const UINT invalid_offset=sizeof vertices+1;rejected_slot=0;
                                    bind_vertices(context,0,1,&vertex_buffer,&stride,&invalid_offset);draw(context,3,1);
                                    check(rejected_slot==14 && !std::memcmp(before.data(),alias,before.size()),"vertex offset outside the logical buffer refuses draw before GPU work");
                                    bind_vertices(context,0,1,&vertex_buffer,&stride,&offset);
                                    void* deferred_draw=nullptr;
                                    check(reinterpret_cast<Deferred>(vt(device)[27])(device,0x60000,&deferred_draw)==S_OK && deferred_draw,"deferred draw negative control creates a real deferred context");
                                    if(deferred_draw) {
                                        rejected_slot=0;reinterpret_cast<Draw>(vt(deferred_draw)[14])(deferred_draw,3,0);
                                        check(rejected_slot==14 && !std::memcmp(before.data(),alias,before.size()),"deferred draw is refused before any immediate GPU work");
                                        reinterpret_cast<Count>(vt(deferred_draw)[2])(deferred_draw);
                                    }
                                    set_d3d11x_trap_handler(nullptr);
                                    deferred_draw=nullptr;
                                    check(reinterpret_cast<Deferred>(vt(device)[27])(device,0x60000,&deferred_draw)==S_OK && deferred_draw,
                                          "deferred draw recording uses the existing context ABI");
                                    if(deferred_draw) {
                                        using Clear=void(STDMETHODCALLTYPE*)(void*,void*,const FLOAT*);
                                        using Finish=HRESULT(STDMETHODCALLTYPE*)(void*,BOOL,void**);
                                        using Execute=void(STDMETHODCALLTYPE*)(void*,void*,BOOL);
                                        const auto clear=reinterpret_cast<Clear>(vt(deferred_draw)[50]);
                                        bind_targets(deferred_draw,1,targets,nullptr);
                                        reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,UINT,const D3D11_VIEWPORT*)>(vt(deferred_draw)[44])(deferred_draw,1,&viewport);
                                        auto* data=static_cast<char*>(deferred_draw);
                                        std::memcpy(data+0x170,&topology,sizeof topology);
                                        std::memcpy(data+0x178,&empty_layout,sizeof empty_layout);
                                        std::memcpy(data+0x180,&shaders[0],sizeof(void*));
                                        clear(deferred_draw,view,black);
                                        std::memcpy(data+0x188,&shaders[1],sizeof(void*));
                                        draw(deferred_draw,3,0);
                                        clear(deferred_draw,view,white);
                                        std::memcpy(data+0x188,&shaders[2],sizeof(void*));
                                        draw(deferred_draw,3,0);
                                        const UINT16 indices[]{99,0,1,2};
                                        struct IndexDesc {D3D11_BUFFER_DESC standard;UINT64 extension;} index_desc{{sizeof indices,D3D11_USAGE_DEFAULT,D3D11_BIND_INDEX_BUFFER,0,0,0},0};
                                        const D3D11_SUBRESOURCE_DATA index_data{indices,0,0};
                                        using CreateIndex=HRESULT(STDMETHODCALLTYPE*)(void*,const IndexDesc*,const D3D11_SUBRESOURCE_DATA*,void**);
                                        void* recorded_indices=nullptr;
                                        check(reinterpret_cast<CreateIndex>(vt(device)[3])(device,&index_desc,&index_data,&recorded_indices)==S_OK && recorded_indices,
                                              "deferred indexed draw uses a real retained index resource");
                                        if(recorded_indices) {
                                            reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,UINT,void*,UINT)>(vt(deferred_draw)[20])(deferred_draw,0,recorded_indices,0);
                                            std::memcpy(data+0x188,&shaders[1],sizeof(void*));
                                            clear(deferred_draw,view,white);
                                            reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,UINT64,INT)>(vt(deferred_draw)[13])(deferred_draw,(UINT64(3)<<32)|1,0);
                                        }
                                        void* list=nullptr;
                                        check(reinterpret_cast<Finish>(vt(deferred_draw)[114])(deferred_draw,TRUE,&list)==S_OK && list &&
                                            !std::memcmp(before.data(),alias,before.size()),
                                            "FinishCommandList never executes clears or draws and captures changing inline shaders");
                                        void* reset_shader=reinterpret_cast<void*>(1);void* reset_layout=reinterpret_cast<void*>(1);
                                        std::memcpy(&reset_shader,data+0x188,sizeof reset_shader);std::memcpy(&reset_layout,data+0x178,sizeof reset_layout);
                                        check(!reset_shader && !reset_layout,"11295 FinishCommandList resets deferred inline bindings even with restore true");
                                        if(recorded_indices) reinterpret_cast<Count>(vt(recorded_indices)[2])(recorded_indices);
                                        reinterpret_cast<Count>(vt(deferred_draw)[2])(deferred_draw);
                                        if(list) {
                                            // Destroy the producer and then replay twice, preserving recorded state and order.
                                            for(UINT pass=0;pass<2;++pass) {
                                                reinterpret_cast<Clear>(vt(context)[50])(context,view,black);
                                                reinterpret_cast<Execute>(vt(context)[58])(context,list,pass!=0);
                                                check(pixels.copy(pixels.context.get(),false,linear.data(),alias,0,32,512)==S_OK &&
                                                    std::all_of(linear.begin(),linear.end(),[](auto value){return value==64;}),
                                                    "deferred clears and original shader draws replay in order after producer release, with fenced exact pixels");
                                            }
                                            void* merged=nullptr;
                                            check(reinterpret_cast<Deferred>(vt(device)[27])(device,0x60000,&merged)==S_OK && merged,"merged list has its own deferred producer");
                                            if(merged) {
                                                reinterpret_cast<Execute>(vt(merged)[58])(merged,list,FALSE);
                                                void* merged_list=nullptr;
                                                check(reinterpret_cast<Finish>(vt(merged)[114])(merged,FALSE,&merged_list)==S_OK && merged_list,
                                                      "executing on a deferred context records rather than submitting");
                                                reinterpret_cast<Count>(vt(merged)[2])(merged);
                                                reinterpret_cast<Count>(vt(list)[2])(list);list=nullptr;
                                                if(merged_list) {
                                                    reinterpret_cast<Clear>(vt(context)[50])(context,view,black);
                                                    reinterpret_cast<Execute>(vt(context)[58])(context,merged_list,FALSE);
                                                    check(pixels.copy(pixels.context.get(),false,linear.data(),alias,0,32,512)==S_OK &&
                                                        std::all_of(linear.begin(),linear.end(),[](auto value){return value==64;}),
                                                        "merged list retains commands and resources after both producer and source list release");
                                                    reinterpret_cast<Count>(vt(merged_list)[2])(merged_list);
                                                }
                                            }
                                            if(list) reinterpret_cast<Count>(vt(list)[2])(list);
                                        }
                                    }
                                    struct DynamicDesc {D3D11_BUFFER_DESC standard;UINT64 extension;} dynamic_desc{{sizeof vertices,D3D11_USAGE_DYNAMIC,D3D11_BIND_VERTEX_BUFFER,D3D11_CPU_ACCESS_WRITE,0,0},0};
                                    const D3D11_SUBRESOURCE_DATA dynamic_data{vertices,0,0};
                                    using CreateDynamic=HRESULT(STDMETHODCALLTYPE*)(void*,const DynamicDesc*,const D3D11_SUBRESOURCE_DATA*,void**);
                                    using MapDeferred=HRESULT(STDMETHODCALLTYPE*)(void*,void*,UINT,D3D11_MAP,UINT,D3D11_MAPPED_SUBRESOURCE*);
                                    using UnmapDeferred=void(STDMETHODCALLTYPE*)(void*,void*);
                                    using FinishDeferred=HRESULT(STDMETHODCALLTYPE*)(void*,BOOL,void**);
                                    using ExecuteDeferred=void(STDMETHODCALLTYPE*)(void*,void*,BOOL);
                                    void* dynamic=nullptr;void* mapper=nullptr;void* versions[2]{};
                                    check(reinterpret_cast<CreateDynamic>(vt(device)[3])(device,&dynamic_desc,&dynamic_data,&dynamic)==S_OK && dynamic,
                                          "deferred Map fixture creates a dynamic vertex resource");
                                    check(reinterpret_cast<Deferred>(vt(device)[27])(device,0x60000,&mapper)==S_OK && mapper,
                                          "deferred Map fixture uses the existing context");
                                    if(dynamic && mapper) {
                                        void* canonical=nullptr;std::memcpy(&canonical,static_cast<char*>(dynamic)+48,sizeof canonical);
                                        const std::vector<unsigned char> frozen(static_cast<unsigned char*>(alias),static_cast<unsigned char*>(alias)+pixels.bytes);
                                        for(UINT pass=0;pass<2;++pass) {
                                            bind_targets(mapper,1,targets,nullptr);
                                            reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,UINT,const D3D11_VIEWPORT*)>(vt(mapper)[44])(mapper,1,&viewport);
                                            auto* data=static_cast<char*>(mapper);
                                            std::memcpy(data+0x170,&topology,sizeof topology);std::memcpy(data+0x178,&vertex_layout,sizeof vertex_layout);
                                            std::memcpy(data+0x180,&shaders[3],sizeof(void*));std::memcpy(data+0x188,&shaders[2],sizeof(void*));
                                            bind_vertices(mapper,0,1,&dynamic,&stride,&offset);
                                            const auto map=reinterpret_cast<MapDeferred>(vt(mapper)[15]);
                                            const auto unmap=reinterpret_cast<UnmapDeferred>(vt(mapper)[16]);
                                            D3D11_MAPPED_SUBRESOURCE mapped{},nested{};
                                            check(map(mapper,dynamic,0,D3D11_MAP_WRITE_NO_OVERWRITE,0,&mapped)==DXGI_ERROR_INVALID_CALL && !mapped.pData,
                                                  "each deferred list requires WRITE_DISCARD before NO_OVERWRITE");
                                            const HRESULT result=map(mapper,dynamic,0,D3D11_MAP_WRITE_DISCARD,0,&mapped);
                                            check(result==S_OK && mapped.pData && mapped.pData!=canonical && mapped.RowPitch==sizeof vertices && mapped.DepthPitch==sizeof vertices,
                                                  "deferred WRITE_DISCARD returns private version storage with original buffer pitches");
                                            if(SUCCEEDED(result)) {
                                                check(map(mapper,dynamic,0,D3D11_MAP_WRITE_DISCARD,0,&nested)==HRESULT_FROM_WIN32(ERROR_BUSY) && !nested.pData,
                                                      "nested deferred map is rejected without losing the valid mapping");
                                                void* invalid=reinterpret_cast<void*>(1);
                                                check(reinterpret_cast<FinishDeferred>(vt(mapper)[114])(mapper,FALSE,&invalid)==DXGI_ERROR_INVALID_CALL && !invalid,
                                                      "unfinished CPU writes cannot be published as a Command List");
                                                if(pass) std::memset(mapped.pData,0,sizeof vertices);else std::memcpy(mapped.pData,vertices,sizeof vertices);
                                                unmap(mapper,dynamic);
                                                check(map(mapper,dynamic,0,D3D11_MAP_WRITE_NO_OVERWRITE,0,&nested)==S_OK && nested.pData==mapped.pData,
                                                      "deferred NO_OVERWRITE preserves the current version");
                                                if(nested.pData) unmap(mapper,dynamic);
                                                reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*,const FLOAT*)>(vt(mapper)[50])(mapper,view,black);
                                                draw(mapper,3,1);
                                            }
                                            check(reinterpret_cast<FinishDeferred>(vt(mapper)[114])(mapper,FALSE,&versions[pass])==S_OK && versions[pass],
                                                  "mapped buffer version is retained by its finished list");
                                            check(canonical && !std::memcmp(canonical,vertices,sizeof vertices) && !std::memcmp(frozen.data(),alias,frozen.size()),
                                                  "deferred recording never overwrites immediate buffer bytes or target pixels");
                                        }
                                    }
                                    if(mapper) reinterpret_cast<Count>(vt(mapper)[2])(mapper);
                                    if(dynamic) reinterpret_cast<Count>(vt(dynamic)[2])(dynamic);
                                    for(UINT pass : {0u,1u,0u}) if(versions[pass]) {
                                        reinterpret_cast<ExecuteDeferred>(vt(context)[58])(context,versions[pass],FALSE);
                                        check(pixels.copy(pixels.context.get(),false,linear.data(),alias,0,32,512)==S_OK &&
                                            std::all_of(linear.begin(),linear.end(),[&](auto value){return value==(pass ? 0 : 191);}),
                                            "separate WRITE_DISCARD versions replay exact pixels after producer/resource release and remain reusable");
                                    }
                                    for(auto* version:versions) if(version) reinterpret_cast<Count>(vt(version)[2])(version);
                                    const char* compute_source="Texture2D<float> src:register(t0); RWTexture2D<float> dst:register(u0); SamplerState point_sampler:register(s0); cbuffer C:register(b0){float amount;}; [numthreads(8,8,1)] void main(uint3 id:SV_DispatchThreadID){dst[id.xy]=src.SampleLevel(point_sampler,(float2(id.xy)+.5)/float2(32,16),0)+amount;}";
                                    Microsoft::WRL::ComPtr<ID3DBlob> compute_code;
                                    bool compute_ready=SUCCEEDED(compile(compute_source,std::strlen(compute_source),nullptr,nullptr,nullptr,"main","cs_5_0",0,0,&compute_code,nullptr));
                                    void* compute_objects[6]{}; // CS, source, SRV, UAV, sampler, dynamic CBV.
                                    if(compute_ready) compute_ready=reinterpret_cast<CreateShader>(vt(device)[18])(device,compute_code->GetBufferPointer(),compute_code->GetBufferSize(),nullptr,&compute_objects[0])==S_OK;
                                    std::array<BYTE,512> source_pixels;source_pixels.fill(51);
                                    auto input_desc=color;input_desc.standard.BindFlags=D3D11_BIND_SHADER_RESOURCE;
                                    const D3D11_SUBRESOURCE_DATA source_data{source_pixels.data(),32,512};
                                    if(compute_ready) compute_ready=reinterpret_cast<CreateOwned>(vt(device)[5])(device,&input_desc,&source_data,&compute_objects[1])==S_OK;
                                    if(compute_ready) compute_ready=reinterpret_cast<MakeSrv>(vt(device)[7])(device,compute_objects[1],&srv,&compute_objects[2])==S_OK;
                                    using MakeComputeUav=HRESULT(STDMETHODCALLTYPE*)(void*,void*,const D3D11_UNORDERED_ACCESS_VIEW_DESC*,void**);
                                    D3D11_UNORDERED_ACCESS_VIEW_DESC output_desc{};output_desc.Format=DXGI_FORMAT_R8_UNORM;output_desc.ViewDimension=D3D11_UAV_DIMENSION_TEXTURE2D;
                                    if(compute_ready) compute_ready=reinterpret_cast<MakeComputeUav>(vt(device)[8])(device,texture,&output_desc,&compute_objects[3])==S_OK;
                                    using MakeComputeSampler=HRESULT(STDMETHODCALLTYPE*)(void*,const D3D11_SAMPLER_DESC*,void**);
                                    D3D11_SAMPLER_DESC point{};point.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;point.AddressU=point.AddressV=point.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;
                                    point.MaxAnisotropy=1;point.ComparisonFunc=D3D11_COMPARISON_NEVER;point.MaxLOD=D3D11_FLOAT32_MAX;
                                    if(compute_ready) compute_ready=reinterpret_cast<MakeComputeSampler>(vt(device)[23])(device,&point,&compute_objects[4])==S_OK;
                                    DynamicDesc constant_desc{{16,D3D11_USAGE_DYNAMIC,D3D11_BIND_CONSTANT_BUFFER,D3D11_CPU_ACCESS_WRITE,0,0},0};
                                    const FLOAT amounts[4]{.05f,0,0,0};const D3D11_SUBRESOURCE_DATA constant_data{amounts,0,0};
                                    if(compute_ready) compute_ready=reinterpret_cast<CreateDynamic>(vt(device)[3])(device,&constant_desc,&constant_data,&compute_objects[5])==S_OK;
                                    check(compute_ready,"compute fixture creates original-format shader, texture views, sampler and constants");
                                    if(compute_ready) {
                                        using BindCompute=void(STDMETHODCALLTYPE*)(void*,UINT,UINT,void* const*);
                                        using Dispatch=void(STDMETHODCALLTYPE*)(void*,UINT,UINT,UINT);
                                        const auto bind_compute=[&](void* ctx) {
                                            reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*)>(vt(ctx)[69])(ctx,compute_objects[0]);
                                            reinterpret_cast<BindCompute>(vt(ctx)[67])(ctx,0,1,&compute_objects[2]);
                                            reinterpret_cast<BindCompute>(vt(ctx)[68])(ctx,0,1,&compute_objects[3]);
                                            reinterpret_cast<BindCompute>(vt(ctx)[70])(ctx,0,1,&compute_objects[4]);
                                            reinterpret_cast<BindCompute>(vt(ctx)[71])(ctx,0,1,&compute_objects[5]);
                                        };
                                        Desc sample_desc{};sample_desc.standard={32,16,1,1,DXGI_FORMAT_R11G11B10_FLOAT,{4,0},D3D11_USAGE_DEFAULT,D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE,0,0};
                                        void* sample_texture=nullptr;void* sample_view=nullptr;
                                        check(reinterpret_cast<CreateOwned>(vt(device)[5])(device,&sample_desc,nullptr,&sample_texture)==S_OK && sample_texture,
                                              "vertex sampling creates a real four-sample original-format texture");
                                        if(sample_texture) {
                                            D3D11_SHADER_RESOURCE_VIEW_DESC sample_srv{};sample_srv.Format=DXGI_FORMAT_R11G11B10_FLOAT;sample_srv.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2DMS;
                                            check(reinterpret_cast<MakeSrv>(vt(device)[7])(device,sample_texture,&sample_srv,&sample_view)==S_OK && sample_view,
                                                  "vertex sampling creates the native Texture2DMS SRV");
                                        }
                                        if(sample_view) {
                                            void* sample_bytes=nullptr;std::memcpy(&sample_bytes,static_cast<char*>(sample_view)+80,sizeof sample_bytes);
                                            TitleTextureLayout sample_layout;
                                            std::vector<UINT> samples(32*16*4);constexpr UINT red[]{0x300,0x340,0x380,0x3c0}; // R11 .125, .25, .5, 1.
                                            for(SIZE_T i=0;i<samples.size();++i) samples[i]=red[i%4];
                                            check(create_xg_texture_layout(32,16,1,1,DXGI_FORMAT_R11G11B10_FLOAT,UINT_MAX,sample_layout,sample_desc.standard.BindFlags,4)==S_OK &&
                                                sample_layout.copy(sample_layout.context.get(),true,samples.data(),sample_bytes,0,32*4*4,32*16*4*4)==S_OK,
                                                "MSAA fixture records four distinct canonical samples per pixel through original XG");
                                        }
                                        const char* vertex_source="Texture2DMS<float3> a:register(t4); Texture2D<float> b:register(t7); Texture2D<float4> n:register(t9); Texture2D<uint4> u:register(t10); Texture2DMS<float4> m:register(t11); Buffer<float4> q:register(t12); Texture2D<int4> s:register(t13); cbuffer C:register(b2){float amount;}; float4 main(float2 p:POSITION,float2 uv:TEXCOORD0):SV_Position {bool valid=a.Load(int2(0,0),0).x==.125 && a.Load(int2(0,0),1).x==.25 && a.Load(int2(0,0),2).x==.5 && a.Load(int2(0,0),3).x==1 && abs(b.Load(int3(0,0,0))+amount-.25)<.001 && all(n.Load(int3(0,0,0))==0) && all(u.Load(int3(0,0,0))==0) && all(m.Load(int2(0,0),0)==0) && all(q.Load(0)==0) && all(s.Load(int3(0,0,0))==0);return float4((p+uv)*(valid ? 1:0),0,1);}";
                                        Microsoft::WRL::ComPtr<ID3DBlob> vertex_code;void* resource_vertex=nullptr;
                                        check(SUCCEEDED(compile(vertex_source,std::strlen(vertex_source),nullptr,nullptr,nullptr,"main","vs_5_0",0,0,&vertex_code,nullptr)) &&
                                            reinterpret_cast<CreateShader>(vt(device)[12])(device,vertex_code->GetBufferPointer(),vertex_code->GetBufferSize(),nullptr,&resource_vertex)==S_OK && resource_vertex,
                                            "vertex resource fixture creates a shader using sparse t4/t7 and b2 bindings");
                                        if(resource_vertex) {
                                            const auto bind_vertex=[&](void* ctx) {
                                                bind_targets(ctx,1,targets,nullptr);
                                                reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,UINT,const D3D11_VIEWPORT*)>(vt(ctx)[44])(ctx,1,&viewport);
                                                std::memcpy(static_cast<char*>(ctx)+0x180,&resource_vertex,sizeof resource_vertex);
                                                std::memcpy(static_cast<char*>(ctx)+0x188,&shaders[2],sizeof(void*));
                                                std::memcpy(static_cast<char*>(ctx)+0x178,&vertex_layout,sizeof vertex_layout);
                                                std::memcpy(static_cast<char*>(ctx)+0x170,&topology,sizeof topology);
                                                bind_vertices(ctx,0,1,&vertex_buffer,&stride,&offset);
                                                reinterpret_cast<BindCompute>(vt(ctx)[25])(ctx,4,1,&sample_view);
                                                reinterpret_cast<BindCompute>(vt(ctx)[25])(ctx,7,1,&compute_objects[2]);
                                                reinterpret_cast<BindCompute>(vt(ctx)[8])(ctx,2,1,&compute_objects[5]);
                                            };
                                            reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*,const FLOAT*)>(vt(context)[50])(context,view,black);
                                            bind_vertex(context);draw(context,3,1);
                                            check(pixels.copy(pixels.context.get(),false,linear.data(),alias,0,32,512)==S_OK &&
                                                std::all_of(linear.begin(),linear.end(),[](auto value){return value==191;}),
                                                "vertex shader reads four distinct MSAA samples, sparse t7 and b2 through real non-pixel resource barriers");
                                            void* vertex_producer=nullptr;void* vertex_list=nullptr;
                                            check(reinterpret_cast<Deferred>(vt(device)[27])(device,0x60000,&vertex_producer)==S_OK && vertex_producer,
                                                  "vertex resource draw uses an existing deferred producer");
                                            if(vertex_producer) {
                                                bind_vertex(vertex_producer);draw(vertex_producer,3,1);
                                                check(reinterpret_cast<FinishDeferred>(vt(vertex_producer)[114])(vertex_producer,FALSE,&vertex_list)==S_OK && vertex_list,
                                                      "sparse vertex bindings survive FinishCommandList");
                                                reinterpret_cast<Count>(vt(vertex_producer)[2])(vertex_producer);
                                            }
                                            reinterpret_cast<void(STDMETHODCALLTYPE*)(void*)>(vt(context)[110])(context);
                                            reinterpret_cast<Count>(vt(resource_vertex)[2])(resource_vertex);
                                            if(sample_view) {reinterpret_cast<Count>(vt(sample_view)[2])(sample_view);sample_view=nullptr;}
                                            if(sample_texture) {reinterpret_cast<Count>(vt(sample_texture)[2])(sample_texture);sample_texture=nullptr;}
                                            for(UINT pass=0;pass<2 && vertex_list;++pass) {
                                                reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*,const FLOAT*)>(vt(context)[50])(context,view,black);
                                                reinterpret_cast<ExecuteDeferred>(vt(context)[58])(context,vertex_list,FALSE);
                                                check(pixels.copy(pixels.context.get(),false,linear.data(),alias,0,32,512)==S_OK &&
                                                    std::all_of(linear.begin(),linear.end(),[](auto value){return value==191;}),
                                                    "deferred sparse vertex descriptors retain exact fenced pixels after producer and shader release on reuse");
                                            }
                                            if(vertex_list) reinterpret_cast<Count>(vt(vertex_list)[2])(vertex_list);
                                        }
                                        if(sample_view) reinterpret_cast<Count>(vt(sample_view)[2])(sample_view);
                                        if(sample_texture) reinterpret_cast<Count>(vt(sample_texture)[2])(sample_texture);
                                        bind_compute(context);reinterpret_cast<Dispatch>(vt(context)[41])(context,4,2,1);
                                        reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,UINT)>(vt(context)[197])(context,0x407);
                                        reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,UINT,void*,SIZE_T)>(vt(context)[152])(context,0x400000,nullptr,0);
                                        check(pixels.copy(pixels.context.get(),false,linear.data(),alias,0,32,512)==S_OK &&
                                            std::all_of(linear.begin(),linear.end(),[](auto value){return value==64;}),
                                            "immediate Dispatch samples the original texture and writes every UAV pixel after a fence");
                                        void* producer=nullptr;void* compute_list=nullptr;
                                        check(reinterpret_cast<Deferred>(vt(device)[27])(device,0x60000,&producer)==S_OK && producer,"compute recording creates a deferred producer");
                                        if(producer) {
                                            bind_compute(producer);D3D11_MAPPED_SUBRESOURCE mapped{};
                                            const HRESULT result=reinterpret_cast<MapDeferred>(vt(producer)[15])(producer,compute_objects[5],0,D3D11_MAP_WRITE_DISCARD,0,&mapped);
                                            check(result==S_OK && mapped.pData,"deferred compute maps its private constant-buffer version");
                                            if(SUCCEEDED(result)) {
                                                const FLOAT updated[4]{.55f,0,0,0};std::memcpy(mapped.pData,updated,sizeof updated);
                                                reinterpret_cast<UnmapDeferred>(vt(producer)[16])(producer,compute_objects[5]);
                                                reinterpret_cast<Dispatch>(vt(producer)[41])(producer,4,2,1);
                                                reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,UINT)>(vt(producer)[197])(producer,0x407);
                                                reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,UINT)>(vt(producer)[197])(producer,0x80000001u);
                                                reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,UINT,void*,SIZE_T)>(vt(producer)[152])(producer,0x400000,nullptr,0);
                                            }
                                            check(reinterpret_cast<FinishDeferred>(vt(producer)[114])(producer,FALSE,&compute_list)==S_OK && compute_list,
                                                  "deferred Dispatch finishes without executing");
                                            check(pixels.copy(pixels.context.get(),false,linear.data(),alias,0,32,512)==S_OK &&
                                                std::all_of(linear.begin(),linear.end(),[](auto value){return value==64;}),"deferred compute leaves canonical pixels unchanged during recording");
                                            reinterpret_cast<Count>(vt(producer)[2])(producer);
                                        }
                                        reinterpret_cast<void(STDMETHODCALLTYPE*)(void*)>(vt(context)[110])(context);
                                        for(auto*& object:compute_objects) if(object) {reinterpret_cast<Count>(vt(object)[2])(object);object=nullptr;}
                                        if(compute_list) {
                                            for(UINT pass=0;pass<2;++pass) {
                                                reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*,const FLOAT*)>(vt(context)[50])(context,view,black);
                                                reinterpret_cast<ExecuteDeferred>(vt(context)[58])(context,compute_list,FALSE);
                                                check(pixels.copy(pixels.context.get(),false,linear.data(),alias,0,32,512)==S_OK &&
                                                    std::all_of(linear.begin(),linear.end(),[](auto value){return value==191;}),
                                                    "deferred Dispatch retains CS/SRV/UAV/sampler and renamed constants after producer release, with exact pixels on reuse");
                                            }
                                            reinterpret_cast<Count>(vt(compute_list)[2])(compute_list);
                                        }
                                    }
                                    for(auto* object:compute_objects) if(object) reinterpret_cast<Count>(vt(object)[2])(object);
                                    reinterpret_cast<void(STDMETHODCALLTYPE*)(void*)>(vt(context)[110])(context);
                                    void* retained_layout=nullptr;
                                    std::memcpy(&retained_layout,static_cast<char*>(context)+0x178,sizeof retained_layout);
                                    check(retained_layout==vertex_layout && reinterpret_cast<Count>(vt(vertex_layout)[1])(vertex_layout)==3,
                                          "11295 ClearState preserves its public input-layout cache and retained ownership");
                                    reinterpret_cast<Count>(vt(vertex_layout)[2])(vertex_layout);
                                }
                                for(auto* shader:shaders) if(shader) reinterpret_cast<Count>(vt(shader)[2])(shader);
                                if(empty_layout) reinterpret_cast<Count>(vt(empty_layout)[2])(empty_layout);
                                if(vertex_layout) reinterpret_cast<Count>(vt(vertex_layout)[2])(vertex_layout);
                                if(vertex_buffer) reinterpret_cast<Count>(vt(vertex_buffer)[2])(vertex_buffer);
                                if(compiler) FreeLibrary(compiler);
                            }
                            reinterpret_cast<Count>(vt(srv_view)[2])(srv_view);
                        }
                    }
                    void* parent=nullptr;
                    reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void**)>(vt(view)[8])(view,&parent);
                    check(parent==texture,"typed color views retain the same logical parent");
                    if (parent) reinterpret_cast<Count>(vt(parent)[2])(parent);
                    reinterpret_cast<Count>(vt(view)[2])(view);
                }
                using CreateUav=HRESULT(STDMETHODCALLTYPE*)(void*,void*,const D3D11_UNORDERED_ACCESS_VIEW_DESC*,void**);
                const auto create_uav=reinterpret_cast<CreateUav>(vt(device)[8]);
                D3D11_UNORDERED_ACCESS_VIEW_DESC invalid{};
                invalid.Format=formats[1]; invalid.ViewDimension=D3D11_UAV_DIMENSION_TEXTURE2D; invalid.Texture2D.MipSlice=1;
                void* view=nullptr;
                check(create_uav(device,texture,&invalid,&view)==E_INVALIDARG && !view,"texture UAV refuses an out-of-range mip");
                invalid.Format=DXGI_FORMAT_R32_FLOAT; invalid.Texture2D.MipSlice=0;
                check(create_uav(device,texture,&invalid,&view)==DXGI_ERROR_UNSUPPORTED && !view,"texture UAV refuses an incompatible format without replacing its parent");
                reinterpret_cast<Count>(vt(texture)[2])(texture);
            }
            for (const UINT faces:{1u,6u}) {
                Desc initialized{};
                initialized.standard={8,8,3,faces,DXGI_FORMAT_R8G8B8A8_UNORM,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_SHADER_RESOURCE,0,faces==6 ? UINT(D3D11_RESOURCE_MISC_TEXTURECUBE) : 0};
                std::vector<std::vector<unsigned char>> sources(faces*3);
                std::vector<D3D11_SUBRESOURCE_DATA> initial(faces*3);
                for (UINT mip=0;mip<faces*3;++mip) {
                    const UINT width=8>>(mip%3),pitch=width*4+12;
                    sources[mip].assign(pitch*width+16,0x5a);
                    for (UINT y=0;y<width;++y) for (UINT x=0;x<width*4;++x) sources[mip][y*pitch+x]=static_cast<unsigned char>(mip*53+y*7+x);
                    initial[mip]={sources[mip].data(),pitch,0};
                }
                const auto original_sources=sources;
                using CreateInitialized=HRESULT(STDMETHODCALLTYPE*)(void*,const Desc*,const D3D11_SUBRESOURCE_DATA*,void**);
                const auto create_initialized=reinterpret_cast<CreateInitialized>(vt(device)[5]);
                texture=nullptr;
                check(create_initialized(device,&initialized,initial.data(),&texture)==S_OK && texture,
                      "initial texture upload accepts padded source rows and every mip");
                if (texture) {
                    void* view=nullptr;
                    using MakeSrv=HRESULT(STDMETHODCALLTYPE*)(void*,void*,const D3D11_SHADER_RESOURCE_VIEW_DESC*,void**);
                    check(reinterpret_cast<MakeSrv>(vt(device)[7])(device,texture,nullptr,&view)==S_OK && view,
                          "initialized texture retains a native view of all uploaded mips");
                    if (view) {
                        if (faces==6) {
                            struct { D3D11_SHADER_RESOURCE_VIEW_DESC desc{}; UINT guard=0xaabbccdd; } queried;
                            reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,D3D11_SHADER_RESOURCE_VIEW_DESC*)>(vt(view)[9])(view,&queried.desc);
                            check(queried.desc.Format==DXGI_FORMAT_R8G8B8A8_UNORM && queried.desc.ViewDimension==D3D11_SRV_DIMENSION_TEXTURECUBE &&
                                  queried.desc.TextureCube.MostDetailedMip==0 && queried.desc.TextureCube.MipLevels==3 && queried.guard==0xaabbccdd,
                                  "default cube SRV preserves its native dimension, mip range and ABI bounds");
                            std::array<UINT32,8> srd{};std::memcpy(srd.data(),static_cast<char*>(view)+48,sizeof srd);
                            check((srd[3]>>28)==11 && (srd[4]&0x1fff)==0 && (srd[5]&0x3ffffff)==(5u<<13),
                                  "cube SRD retains faces zero through five and encodes the last cube like original UMD");
                        }
                        void* canonical=nullptr;std::memcpy(&canonical,static_cast<char*>(view)+80,sizeof canonical);
                        TitleTextureLayout pixels;
                        hr=create_xg_texture_layout(8,8,3,faces,DXGI_FORMAT_R8G8B8A8_UNORM,UINT_MAX,pixels,D3D11_BIND_SHADER_RESOURCE);
                        check(SUCCEEDED(hr),"initialized texture keeps the original XG mip layout");
                        for (UINT mip=0;SUCCEEDED(hr) && mip<faces*3;++mip) {
                            const UINT width=8>>(mip%3),pitch=width*4;
                            std::vector<unsigned char> linear(pitch*width);
                            check(pixels.copy(pixels.context.get(),false,linear.data(),canonical,mip,pitch,pitch*width)==S_OK,
                                  "initialized texture exposes tiled mip pixels through original XG");
                            bool exact=true;
                            for (UINT y=0;y<width;++y) exact=exact && !std::memcmp(linear.data()+y*pitch,sources[mip].data()+y*initial[mip].SysMemPitch,pitch);
                            check(exact,"upload preserves every source pixel without copying row padding");
                        }
                        reinterpret_cast<Count>(vt(view)[2])(view);
                    }
                    reinterpret_cast<Count>(vt(texture)[2])(texture);
                }
                check(sources==original_sources,"texture initialization leaves source pixels, padding and guards intact");
                initial[2].SysMemPitch=1;
                check(create_initialized(device,&initialized,initial.data(),&texture)==E_INVALIDARG && !texture,
                      "short initial mip pitch fails without publishing a partial texture");
                initial[2].SysMemPitch=20; initial[2].pSysMem=reinterpret_cast<void*>(1);
                check(create_initialized(device,&initialized,initial.data(),&texture)==E_INVALIDARG && !texture,
                      "unreadable initial pixels fail without dereferencing untrusted memory");
                if (faces==6) {
                    initialized.standard.Height=4;
                    check(create_initialized(device,&initialized,nullptr,&texture)==E_INVALIDARG && !texture,
                          "cube texture rejects non-square faces");
                    initialized.standard.Height=8; initialized.standard.ArraySize=5;
                    check(create_initialized(device,&initialized,nullptr,&texture)==E_INVALIDARG && !texture,
                          "cube texture rejects an incomplete face set");
                }
            }
            TitleTextureLayout placed_color;
            const UINT color_binds=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET|D3D11_BIND_UNORDERED_ACCESS;
            hr=create_xg_texture_layout(32,16,1,1,DXGI_FORMAT_R16G16_FLOAT,14,placed_color,color_binds);
            check(SUCCEEDED(hr),"placed half-float color keeps the original tile mode");
            if (SUCCEEDED(hr)) {
                void* storage=allocate(placed_color.bytes+placed_color.alignment+32,attributes);
                check(storage!=nullptr,"placed color fixture owns its canonical allocation");
                if (storage) {
                    const SIZE_T total=placed_color.bytes+placed_color.alignment+32;
                    std::memset(storage,0x5a,total);
                    const auto address=(reinterpret_cast<std::uintptr_t>(storage)+16+placed_color.alignment-1)&~(placed_color.alignment-1);
                    Desc color{}; color.standard={32,16,1,1,DXGI_FORMAT_R16G16_FLOAT,{1,0},D3D11_USAGE_DEFAULT,color_binds,0,0};
                    texture=nullptr;
                    check(reinterpret_cast<CreateTexture>(vt(device)[64])(device,&color,14,0,reinterpret_cast<void*>(address),&texture)==S_OK && texture,
                          "placed color binds native render and unordered access to existing storage");
                    if (texture) reinterpret_cast<Count>(vt(texture)[2])(texture);
                    check(std::all_of(static_cast<unsigned char*>(storage),static_cast<unsigned char*>(storage)+total,[](auto x){return x==0x5a;}),
                          "placed color creation and retirement preserve borrowed pixels and guards");
                    free_memory(storage,attributes);
                }
            }
            struct VolumeDesc { D3D11_TEXTURE3D_DESC standard; std::array<UINT, 2> extension{}; } volume_desc{};
            static_assert(sizeof(VolumeDesc) == 44);
            volume_desc.standard = {16, 16, 16, 1, DXGI_FORMAT_R10G10B10A2_UNORM, D3D11_USAGE_DEFAULT,
                D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, 0, 0};
            using CreateVolume = HRESULT (STDMETHODCALLTYPE*)(void*, const VolumeDesc*, const D3D11_SUBRESOURCE_DATA*, void**);
            const auto create_volume = reinterpret_cast<CreateVolume>(vt(device)[6]);
            constexpr UINT volume_pitch=16*4+8,volume_slice=volume_pitch*16+16;
            std::vector<unsigned char> volume_source(volume_slice*16+16,0x5a);
            for (UINT z=0;z<16;++z) for (UINT y=0;y<16;++y) for (UINT x=0;x<64;++x)
                volume_source[z*volume_slice+y*volume_pitch+x]=static_cast<unsigned char>(z*13+y*7+x);
            const auto original_volume_source=volume_source;
            D3D11_SUBRESOURCE_DATA volume_initial{volume_source.data(),volume_pitch,volume_slice};
            texture = nullptr;
            check(create_volume(device, &volume_desc, nullptr, nullptr) == S_FALSE,
                  "volume descriptor validation does not acquire ownership");
            check(create_volume(device, &volume_desc, &volume_initial, &texture) == S_OK && texture,
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
                using CreateUav = HRESULT (STDMETHODCALLTYPE*)(void*,void*,const D3D11_UNORDERED_ACCESS_VIEW_DESC*,void**);
                const auto create_uav=reinterpret_cast<CreateUav>(vt(device)[8]);
                void* uav=nullptr;
                check(create_uav(device,texture,nullptr,nullptr)==S_FALSE,
                      "default volume UAV validation does not acquire ownership");
                check(create_uav(device,texture,nullptr,&uav)==S_OK && uav,
                      "default volume UAV owns a native texture3D descriptor");
                if (uav) {
                    struct { D3D11_UNORDERED_ACCESS_VIEW_DESC desc{}; UINT guard=0xaabbccdd; } output;
                    using GetUavDesc=void(STDMETHODCALLTYPE*)(void*,D3D11_UNORDERED_ACCESS_VIEW_DESC*);
                    reinterpret_cast<GetUavDesc>(vt(uav)[9])(uav,&output.desc);
                    check(output.desc.Format==volume_desc.standard.Format && output.desc.ViewDimension==D3D11_UAV_DIMENSION_TEXTURE3D &&
                          output.desc.Texture3D.MipSlice==0 && output.desc.Texture3D.FirstWSlice==0 &&
                          output.desc.Texture3D.WSize==UINT_MAX && output.guard==0xaabbccdd,
                          "volume UAV preserves its complete native view descriptor and output bounds");
                    void* parent=nullptr;
                    using GetResource=void(STDMETHODCALLTYPE*)(void*,void**);
                    reinterpret_cast<GetResource>(vt(uav)[8])(uav,&parent);
                    check(parent==texture,"volume UAV retains the original resource");
                    if (parent) reinterpret_cast<Count>(vt(parent)[2])(parent);
                    const auto* view_words=reinterpret_cast<const UINT32*>(static_cast<const unsigned char*>(uav)+48);
                    void* canonical=nullptr;
                    std::memcpy(&canonical,static_cast<const unsigned char*>(uav)+80,sizeof canonical);
                    check(view_words[0]==(reinterpret_cast<std::uintptr_t>(canonical)>>8) && (view_words[3]>>28)==10,
                          "volume UAV preserves the physical XG base and native 3D resource type");
                    reinterpret_cast<Count>(vt(uav)[2])(uav);
                }
                D3D11_UNORDERED_ACCESS_VIEW_DESC invalid{};
                invalid.Format=volume_desc.standard.Format; invalid.ViewDimension=D3D11_UAV_DIMENSION_TEXTURE3D;
                invalid.Texture3D.WSize=17;
                check(create_uav(device,texture,&invalid,&uav)==E_INVALIDARG && !uav,
                      "volume UAV rejects a slice range outside the resource");
                invalid.Texture3D.WSize=8;
                check(create_uav(device,texture,&invalid,&uav)==DXGI_ERROR_UNSUPPORTED && !uav,
                      "partial volume UAV remains an explicit unsupported case");
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
                    void* canonical=nullptr;std::memcpy(&canonical,static_cast<char*>(view)+80,sizeof canonical);
                    TitleTextureLayout pixels;
                    hr=create_xg_texture3d_layout(16,16,16,1,volume_desc.standard.Format,volume_desc.standard.BindFlags,pixels);
                    std::vector<unsigned char> linear(16*16*16*4);
                    check(SUCCEEDED(hr) && pixels.copy(pixels.context.get(),false,linear.data(),canonical,0,64,16*64)==S_OK,
                          "volume initial data is stored in the original XG physical layout");
                    bool exact=true;
                    for (UINT z=0;z<16;++z) for (UINT y=0;y<16;++y)
                        exact=exact && !std::memcmp(linear.data()+(z*16+y)*64,volume_source.data()+z*volume_slice+y*volume_pitch,64);
                    check(exact && volume_source==original_volume_source,"volume initialization preserves every depth slice and source padding");
                    check(reinterpret_cast<Count>(vt(texture)[2])(texture) == 1, "volume view retains its storage after caller release");
                    texture = nullptr;
                    reinterpret_cast<Count>(vt(view)[2])(view);
                }
                if (texture) reinterpret_cast<Count>(vt(texture)[2])(texture);
            }
            volume_initial.SysMemSlicePitch=1;
            check(create_volume(device,&volume_desc,&volume_initial,&texture)==E_INVALIDARG && !texture,
                  "volume initialization rejects overlapping source slices before publishing storage");
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
            if (same_library(item.first,"msvcrt.dll"))
                for (const char* name : {"printf","sprintf","_purecall","__CxxFrameHandler3","_CxxThrowException",
                    "??1type_info@@UEAA@XZ","?what@exception@@UEBAPEBDXZ","??1exception@@UEAA@XZ",
                    "??0exception@@QEAA@AEBV0@@Z","??0exception@@QEAA@AEBQEBD@Z"})
                    check(host && GetProcAddress(host,name) && resolve_host_module({item.first,name,0}) == reinterpret_cast<void*>(GetProcAddress(host,name)),
                        "2018 XG CRT dependencies retain the exact host named ABI");
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
            const auto mkdir = reinterpret_cast<decltype(&CreateDirectoryW)>(resolve_kernelx({"kernelx.dll", "CreateDirectoryW", 0}));
            const auto attributes = reinterpret_cast<decltype(&GetFileAttributesExA)>(resolve_kernelx({"kernelx.dll", "GetFileAttributesExA", 0}));
            const auto enumerate = reinterpret_cast<decltype(&FindFirstFileExA)>(resolve_kernelx({"kernelx.dll", "FindFirstFileExA", 0}));
            const auto move = reinterpret_cast<decltype(&MoveFileExW)>(resolve_kernelx({"kernelx.dll", "MoveFileExW", 0}));
            check(mkdir && attributes && enumerate && move, "temporary volume uses shared filesystem APIs");
            if (mkdir && attributes && enumerate && move) {
                check(mkdir(L"T:\\cache\\", nullptr), "temporary directory created inside session storage");
                HANDLE temporary = open(L"t:\\cache\\data.bin", GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
                check(temporary != INVALID_HANDLE_VALUE, "temporary volume open shares directory translation");
                if (temporary != INVALID_HANDLE_VALUE) {
                    DWORD written = 0;
                    check(write(temporary, "temp", 4, &written, nullptr) && written == 4, "temporary handle passes storage write policy");
                    CloseHandle(temporary);
                }
                WIN32_FILE_ATTRIBUTE_DATA info{};
                check(attributes("T:\\cache\\data.bin", GetFileExInfoStandard, &info) && info.nFileSizeLow == 4,
                      "temporary attributes query observes actual bytes");
                WIN32_FIND_DATAA found{};
                HANDLE search = enumerate("T:\\cache\\*.bin", FindExInfoStandard, &found, FindExSearchNameMatch, nullptr, 0);
                check(search != INVALID_HANDLE_VALUE && std::string(found.cFileName) == "data.bin", "temporary enumeration shares namespace");
                if (search != INVALID_HANDLE_VALUE) FindClose(search);
                check(move(L"T:\\cache\\data.bin", L"T:\\cache\\moved.bin", 0), "temporary rename shares namespace");
                check(remove(L"T:\\cache\\moved.bin"), "temporary delete shares namespace");
                check(!mkdir(L"T:\\..\\escape", nullptr) && GetLastError() == ERROR_ACCESS_DENIED,
                      "temporary traversal cannot reach saves or host paths");
                check(open(L"T:relative", GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr) == INVALID_HANDLE_VALUE &&
                      GetLastError() == ERROR_INVALID_NAME, "temporary drive-relative paths are explicit errors");
                set_title_storage_root({});
                check(!mkdir(L"T:\\unbound", nullptr) && GetLastError() == ERROR_ACCESS_DENIED, "temporary volume requires session storage");
                set_title_storage_root(root);
                const auto unicode_root = base + L"\\\u7f13\u5b58";
                check(CreateDirectoryW(unicode_root.c_str(), nullptr), "Unicode storage root created");
                set_title_storage_root(unicode_root);
                check(mkdir(L"T:\\root-test", nullptr), "temporary root rebind supports Unicode host paths");
                check(attributes("T:\\root-test", GetFileExInfoStandard, &info) && (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY),
                      "ANSI guest queries preserve Unicode host root");
                search = enumerate("T:\\root-*", FindExInfoStandard, &found, FindExSearchNameMatch, nullptr, 0);
                check(search != INVALID_HANDLE_VALUE && std::string(found.cFileName) == "root-test", "ANSI enumeration supports Unicode host root");
                if (search != INVALID_HANDLE_VALUE) FindClose(search);
                RemoveDirectoryW((unicode_root + L"\\temp\\root-test").c_str());
                RemoveDirectoryW((unicode_root + L"\\temp").c_str());
                RemoveDirectoryW(unicode_root.c_str());
                set_title_storage_root(root);
                RemoveDirectoryW((root + L"\\temp\\cache").c_str());
                RemoveDirectoryW((root + L"\\temp").c_str());
            }
            std::wstring short_root(32768, L'\0');
            const DWORD short_length = GetShortPathNameW(root.c_str(), short_root.data(), static_cast<DWORD>(short_root.size()));
            check(short_length && short_length < short_root.size(), "storage root resolves its short-name alias");
            if (short_length && short_length < short_root.size()) {
                short_root.resize(short_length);
                set_title_storage_root(short_root);
            }
            const auto created = root + L"\\new-save.bin";
            HANDLE fresh = open(created.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
            check(fresh != INVALID_HANDLE_VALUE, "short-name root allows creation through its long-name alias");
            if (fresh != INVALID_HANDLE_VALUE) {
                DWORD bytes = 0;
                check(write(fresh, "new", 3, &bytes, nullptr) && bytes == 3 && flush(fresh),
                      "new save persists through normalized handle validation");
                CloseHandle(fresh);
            }
            DeleteFileW(created.c_str());
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
            check(reinterpret_cast<Any>(vt(device)[6])(device) == 0xdead && trapped_slot == 6 &&
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
                        using GarbageCollect=void(STDMETHODCALLTYPE*)(void*,UINT);
                        const auto collect=reinterpret_cast<GarbageCollect>(vt(device)[69]);
                        collect(device,0);
                        check(refs(buffer)==1,"synchronous garbage collection preserves live resource ownership");
                        static UINT collection_slot=0;collection_slot=0;
                        set_d3d11x_trap_handler([](const char*,UINT slot)->std::uintptr_t{collection_slot=slot;return 0;});
                        collect(device,1);set_d3d11x_trap_handler(nullptr);
                        check(collection_slot==69 && refs(buffer)==1,"unsupported garbage collection flags stay explicit and preserve live resources");
                        using BindVertices=void(STDMETHODCALLTYPE*)(void*,UINT,UINT,void* const*,const UINT*,const UINT*);
                        const auto bind_vertices=reinterpret_cast<BindVertices>(vt(context)[19]);
                        const UINT stride=12,offset=4;void* buffers[]{buffer};
                        bind_vertices(context,31,1,buffers,&stride,&offset);bind_vertices(context,31,1,buffers,&stride,&offset);
                        check(refs(buffer)==2,"legacy vertex binding retains the last slot and balances rebinds");
                        bind_vertices(context,0,1,buffers,&stride,&offset);
                        check(refs(buffer)==3,"independent vertex slots retain independent references");
                        bind_vertices(context,31,1,nullptr,nullptr,nullptr);
                        check(refs(buffer)==1,"11295 null vertex array clears all slots without reading stride or offset pointers");
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
                        using BindConstants = void(STDMETHODCALLTYPE*)(void*,UINT,UINT,void* const*);
                        const auto bind = reinterpret_cast<BindConstants>(vt(context)[8]);
                        void* buffers[]{buffer};
                        bind(context,13,1,buffers);
                        check(reinterpret_cast<Count>(vt(buffer)[1])(buffer)==3,"legacy VS constant binding retains the last valid slot");
                        reinterpret_cast<Count>(vt(buffer)[2])(buffer);
                        bind(context,13,1,buffers);
                        check(reinterpret_cast<Count>(vt(buffer)[1])(buffer)==3,"legacy VS constant rebinding balances ownership");
                        reinterpret_cast<Count>(vt(buffer)[2])(buffer);
                        bind(context,13,1,nullptr);
                        check(reinterpret_cast<Count>(vt(buffer)[1])(buffer)==2,"legacy VS constant unbind releases its retained buffer");
                        reinterpret_cast<Count>(vt(buffer)[2])(buffer);
                        bind(context,14,0,nullptr);
                        const auto bind_pixel = reinterpret_cast<BindConstants>(vt(context)[17]);
                        const auto bind_geometry = reinterpret_cast<BindConstants>(vt(context)[23]);
                        const auto bind_compute = reinterpret_cast<BindConstants>(vt(context)[71]);
                        bind(context,0,1,buffers); bind_pixel(context,0,1,buffers);
                        bind_geometry(context,13,1,buffers);bind_geometry(context,13,1,buffers);
                        bind_compute(context,0,1,buffers);bind_compute(context,0,1,buffers);
                        check(reinterpret_cast<Count>(vt(buffer)[1])(buffer)==6,"VS, PS, GS and CS constants retain independent references and balance rebinds");
                        reinterpret_cast<Count>(vt(buffer)[2])(buffer);
                        bind(context,0,1,nullptr);
                        check(reinterpret_cast<Count>(vt(buffer)[1])(buffer)==5,"VS unbind preserves PS, GS and CS constant ownership");
                        reinterpret_cast<Count>(vt(buffer)[2])(buffer);
                        reinterpret_cast<void(STDMETHODCALLTYPE*)(void*)>(vt(context)[110])(context);
                        check(reinterpret_cast<Count>(vt(buffer)[1])(buffer)==2,"legacy ClearState releases PS, GS and CS constants");
                        reinterpret_cast<Count>(vt(buffer)[2])(buffer);
                    }
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
                        using Unmap = void (STDMETHODCALLTYPE*)(void*, void*);
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
                            unmap(context, buffer);
                            check(static_cast<UINT32*>(allocation)[19] == 0xabc00013, "unmap keeps CPU writes in canonical title bytes");
                            check(map(context, buffer, 0, D3D11_MAP_WRITE_DISCARD, D3D11_MAP_FLAG_DO_NOT_WAIT, &result.mapped) == DXGI_ERROR_UNSUPPORTED && !result.mapped.pData,
                                  "unimplemented map flag combinations remain explicit");
                            check(map(context, buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &result.mapped) == S_OK,
                                  "completed dynamic buffer can be mapped again");
                            if (result.mapped.pData) unmap(context, buffer);
                        }
                        reinterpret_cast<Count>(vt(buffer)[2])(buffer);
                    }
                    desc.standard = {1332, D3D11_USAGE_DYNAMIC, D3D11_BIND_SHADER_RESOURCE,
                        D3D11_CPU_ACCESS_WRITE, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 36};
                    check(factory(device, &desc, nullptr, &buffer) == S_OK && buffer,
                          "observed dynamic structured buffer retains its Xbox CPU-write contract");
                    if (buffer) {
                        using Map = HRESULT (STDMETHODCALLTYPE*)(void*, void*, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*);
                        using Unmap = void (STDMETHODCALLTYPE*)(void*, void*);
                        const auto map = reinterpret_cast<Map>(vt(context)[15]);
                        D3D11_MAPPED_SUBRESOURCE mapped{};
                        check(map(context, buffer, 0, D3D11_MAP_WRITE_NO_OVERWRITE, 0, &mapped) == DXGI_ERROR_UNSUPPORTED && !mapped.pData,
                              "structured SRV no-overwrite remains unsupported");
                        check(map(context, buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped) == S_OK && mapped.pData && mapped.RowPitch == 1332,
                              "dynamic structured map exposes the complete canonical span");
                        if (mapped.pData) {
                            static_cast<unsigned char*>(mapped.pData)[1331] = 0xa7;
                            reinterpret_cast<Unmap>(vt(context)[16])(context, buffer);
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
                        using SetRaster=void(STDMETHODCALLTYPE*)(void*,void*);
                        const auto set_raster=reinterpret_cast<SetRaster>(vt(context)[43]);
                        set_raster(context,state);
                        check(refs(state)==2,"legacy raster slot 43 retains its native state");
                        set_raster(context,state);
                        check(refs(state)==2,"rebinding a rasterizer preserves one context owner");
                        set_raster(context,nullptr);
                        check(refs(state)==1,"legacy raster reset releases context ownership");
                        set_raster(context,state);
                        reinterpret_cast<void(STDMETHODCALLTYPE*)(void*)>(vt(context)[110])(context);
                        check(refs(state)==1,"ClearState releases a retained rasterizer without retiring caller ownership");
                        reinterpret_cast<void(STDMETHODCALLTYPE*)(void*)>(vt(context)[110])(context);
                        check(refs(state)==1,"repeated ClearState cannot release a rasterizer twice");
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
                        using SetBlend=void(STDMETHODCALLTYPE*)(void*,void*,const FLOAT*,UINT);
                        const auto set_blend=reinterpret_cast<SetBlend>(vt(context)[35]);
                        const FLOAT factor[]{.125f,.25f,.5f,1.f};
                        set_blend(context,state,factor,0x55555555);
                        check(refs(state)==2,"legacy slot 35 retains its real blend state");
                        set_blend(context,state,nullptr,UINT_MAX);
                        check(refs(state)==2,"rebinding the same legacy blend preserves exactly one context owner");
                        set_blend(context,nullptr,nullptr,UINT_MAX);
                        check(refs(state)==1,"legacy blend unbind retires context ownership");
                        set_blend(context,state,factor,0x55555555);
                        reinterpret_cast<void(STDMETHODCALLTYPE*)(void*)>(vt(context)[110])(context);
                        check(refs(state)==1,"ClearState releases blend ownership and permits later state reuse");
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
                        using FastResources=void(STDMETHODCALLTYPE*)(void*,const void*,const void*);
                        const auto fast=reinterpret_cast<FastResources>(vt(context)[209]);
                        std::array<UINT32,9> batch{0xa0040e02};
                        const auto object=reinterpret_cast<std::uintptr_t>(sampler);
                        std::memcpy(batch.data()+1,&object,sizeof object);std::memcpy(batch.data()+5,&object,sizeof object);
                        fast(context,batch.data(),batch.data()+batch.size());
                        check(refs(sampler)==3,"fast PS sampler packet retains both final slots independently");
                        fast(context,batch.data(),batch.data()+batch.size());
                        check(refs(sampler)==3,"fast sampler packet rebind balances ownership");
                        using BindSamplers=void(STDMETHODCALLTYPE*)(void*,UINT,UINT,void* const*);
                        const auto bind_samplers=reinterpret_cast<BindSamplers>(vt(context)[11]);
                        void* sampler_array[]{sampler};
                        bind_samplers(context,0,1,sampler_array);bind_samplers(context,0,1,sampler_array);
                        check(refs(sampler)==4,"legacy standard PS sampler binding shares fast state with balanced ownership");
                        bind_samplers(context,0,1,nullptr);bind_samplers(context,16,0,nullptr);
                        check(refs(sampler)==3,"legacy standard PS sampler unbind preserves the two fast-bound slots");
                        reinterpret_cast<void(STDMETHODCALLTYPE*)(void*)>(vt(context)[110])(context);
                        check(refs(sampler)==1,"ClearState releases fast sampler bindings without changing the descriptor");
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
                    desc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
                    for (UINT border = 1; border <= 2; ++border) {
                        for (UINT channel = 0; channel < 4; ++channel) desc.BorderColor[channel] = channel == 3 || border == 2 ? 1.f : 0.f;
                        check(factory(device,&desc,&sampler) == S_OK && sampler,"standard opaque sampler borders are represented exactly");
                        if (sampler) {
                            std::array<UINT32,4> words{};
                            using GetX = void(STDMETHODCALLTYPE*)(void*,UINT32*);
                            reinterpret_cast<GetX>(vt(sampler)[9])(sampler,words.data());
                            check(words[3] == (border << 30),"sampler SRD retains the standard border color type");
                            using GetDesc = void(STDMETHODCALLTYPE*)(void*,D3D12_SAMPLER_DESC*);
                            D3D12_SAMPLER_DESC actual{};
                            reinterpret_cast<GetDesc>(vt(sampler)[8])(sampler,&actual);
                            check(!std::memcmp(actual.BorderColor,desc.BorderColor,sizeof desc.BorderColor),"sampler GetDesc preserves all border channels");
                            reinterpret_cast<Count>(vt(sampler)[2])(sampler);
                        }
                    }
                    for (float& channel : desc.BorderColor) channel = 0;
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
                        using SetDepth=void(STDMETHODCALLTYPE*)(void*,void*,UINT);
                        const auto set_depth=reinterpret_cast<SetDepth>(vt(context)[36]);
                        set_depth(context,state,0x7a);set_depth(context,state,0x5c);
                        check(refs(state)==2,"legacy depth binding retains exactly one state reference across stencil-reference changes");
                        set_depth(context,nullptr,0);
                        check(refs(state)==1,"legacy depth unbinding releases the context state owner");
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
                using SetViewports=void(STDMETHODCALLTYPE*)(void*,UINT,const D3D11_VIEWPORT*);
                using GetViewports=void(STDMETHODCALLTYPE*)(void*,UINT*,D3D11_VIEWPORT*);
                const auto set_viewports=reinterpret_cast<SetViewports>(vt(context)[44]);
                const auto get_viewports=reinterpret_cast<GetViewports>(vt(context)[95]);
                const D3D11_VIEWPORT viewport_values[]{{2,3,32,16,0.2f,0.8f},{4,5,64,32,0,1}};
                set_viewports(context,2,viewport_values);
                struct { D3D11_VIEWPORT value{};UINT guard=0x12345678; } viewport_result;
                UINT viewport_capacity=1;get_viewports(context,&viewport_capacity,&viewport_result.value);
                check(viewport_capacity==1 && !std::memcmp(&viewport_result.value,viewport_values,sizeof(D3D11_VIEWPORT)) && viewport_result.guard==0x12345678,
                      "legacy viewport query respects capacity, origin, extent, depth and its output guard");
                viewport_capacity=0;get_viewports(context,&viewport_capacity,nullptr);
                check(viewport_capacity==2,"legacy viewport count-only query returns all active viewports");
                set_viewports(context,0,nullptr);viewport_capacity=1;get_viewports(context,&viewport_capacity,&viewport_result.value);
                const D3D11_VIEWPORT zero_viewport{};
                check(viewport_capacity==1 && !std::memcmp(&viewport_result.value,&zero_viewport,sizeof zero_viewport),
                      "original zero-count viewport setter substitutes one all-zero viewport");
                const auto create_deferred = reinterpret_cast<CreateDeferred>(vt(device)[27]);
                void* deferred = nullptr;
                check(create_deferred(device, 0, nullptr) == E_POINTER, "deferred context output required");
                check(create_deferred(device, 0x80000, &deferred) == DXGI_ERROR_UNSUPPORTED && !deferred,
                      "unimplemented deferred context flags rejected");
                for (const UINT flags:{0u,0x60000u,0xa0000u}) {
                    check(create_deferred(device, flags, &deferred) == S_OK && deferred && deferred != context,
                          "deferred context has independent identity");
                    if (deferred) {
                        reinterpret_cast<SetViewports>(vt(deferred)[44])(deferred,2,viewport_values);
                        viewport_capacity=1;reinterpret_cast<GetViewports>(vt(deferred)[95])(deferred,&viewport_capacity,&viewport_result.value);
                        check(flags&0x80000 ? (!viewport_capacity && !std::memcmp(&viewport_result.value,&zero_viewport,sizeof zero_viewport)) :
                            (viewport_capacity==1 && !std::memcmp(&viewport_result.value,&viewport_values[0],sizeof zero_viewport)),
                              "only FAST_SEMANTICS suppresses deferred viewport queries; tracked contexts preserve values and capacity");
                        using State = UINT (STDMETHODCALLTYPE*)(void*);
                        const auto data = static_cast<unsigned char*>(deferred);
                        check((reinterpret_cast<std::uintptr_t>(deferred) & 63) == 0 &&
                              reinterpret_cast<const void*>(vt(deferred)) == data + 0x3d0 && data[0x390] == 1,
                              "old context contains its inline function table and out-of-line dispatch marker");
                        check(reinterpret_cast<State>(vt(deferred)[112])(deferred) == 1 &&
                              reinterpret_cast<State>(vt(deferred)[113])(deferred) == flags,
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
                        const UINT64 pipeline[]={0x1234,0x5678,0x9abc,0xdef0,0x1357,0x2468};
                        std::memcpy(data+0x180,pipeline,16);
                        std::memcpy(data+0x1c0,pipeline+2,32);
                        const UINT64 predicate=0x5678,unrelated=0x1122334455667788ull;
                        const UINT hash=0x1234;
                        const UINT8 predicate_flags=0x80;
                        dirty=0x400000;shader_flags=0x10000000000ull;
                        std::memcpy(data+0x168,&dirty,4);std::memcpy(data+0x218,&shader_flags,8);
                        std::memcpy(data+0x1e4,&hash,4);std::memcpy(data+0x1f0,&predicate,8);
                        std::memcpy(data+0x1f8,&predicate_flags,1);std::memcpy(data+0x350,&unrelated,8);
                        const auto table=vt(deferred);
                        reinterpret_cast<void(STDMETHODCALLTYPE*)(void*)>(table[110])(deferred);
                        UINT64 observed[6]{};
                        std::memcpy(observed,data+0x180,16);std::memcpy(observed+2,data+0x1c0,32);
                        UINT64 guard=0,condition=1;UINT cleared_hash=1;UINT8 condition_flags=0;
                        std::memcpy(&dirty,data+0x168,4);std::memcpy(&shader_flags,data+0x218,8);
                        std::memcpy(&guard,data+0x350,8);std::memcpy(&condition,data+0x1f0,8);
                        std::memcpy(&condition_flags,data+0x1f8,1);std::memcpy(&cleared_hash,data+0x1e4,4);
                        check(std::all_of(std::begin(observed),std::end(observed),[](auto x){return !x;}) && !condition && !cleared_hash && condition_flags==0x81,
                              "ClearState resets public shader bindings, blend key and predication with original flag semantics");
                        check(dirty==0x42bfb0 && shader_flags==0x1004c00002eull && guard==unrelated && vt(deferred)==table && data[0x390]==1 &&
                              reinterpret_cast<State>(table[112])(deferred)==1 && reinterpret_cast<State>(table[113])(deferred)==flags,
                              "ClearState preserves unrelated public state, dispatch, type and creation flags while marking native dirty fields");
                        const UINT64 vertex_marker=0x398,pixel_marker=0x400;
                        std::memcpy(data+0x180,&vertex_marker,8);std::memcpy(data+0x188,&pixel_marker,8);
                        reinterpret_cast<void(STDMETHODCALLTYPE*)(void*)>(table[110])(deferred);
                        std::memcpy(observed,data+0x180,16);
                        check(observed[0]==vertex_marker && observed[1]==pixel_marker && data[0x390]==1,
                              "ClearState preserves the original special shader markers and out-of-line dispatch");
                        using Finish = HRESULT (STDMETHODCALLTYPE*)(void*, BOOL, void**);
                        using Execute = void (STDMETHODCALLTYPE*)(void*,void*,BOOL);
                        const auto finish=reinterpret_cast<Finish>(vt(deferred)[114]);
                        const auto execute=reinterpret_cast<Execute>(vt(context)[58]);
                        void* command_list = nullptr;
                        check(finish(deferred,FALSE,&command_list)==S_OK && command_list,
                              "FinishCommandList creates an immutable empty list at the native slot");
                        if(command_list) {
                            check(reinterpret_cast<State>(vt(command_list)[8])(command_list)==flags,
                                  "command list GetContextFlags preserves original deferred creation flags");
                            void* owner=nullptr;
                            reinterpret_cast<GetDevice>(vt(command_list)[3])(command_list,&owner);
                            check(owner==device,"command list shares device identity");
                            if(owner) reinterpret_cast<Count>(vt(owner)[2])(owner);
                            const D3D11_VIEWPORT keep{2,3,16,8,0,1};
                            reinterpret_cast<SetViewports>(vt(context)[44])(context,1,&keep);
                            execute(context,command_list,FALSE);execute(context,command_list,TRUE);
                            UINT capacity=1;D3D11_VIEWPORT result{};
                            reinterpret_cast<GetViewports>(vt(context)[95])(context,&capacity,&result);
                            const D3D11_VIEWPORT empty{};
                            check(flags&0x20000 ? (capacity==1 && !std::memcmp(&result,&keep,sizeof keep)) :
                                (!capacity && !std::memcmp(&result,&empty,sizeof empty)),
                                  "11295 ExecuteCommandList ignores restore: common lists clear tracked state, Draw Bundles preserve bindings on reuse");
                            reinterpret_cast<Count>(vt(command_list)[2])(command_list);
                        }
                        check(finish(deferred,TRUE,nullptr)==S_OK,"null FinishCommandList output discards the recording");
                        void* invalid=reinterpret_cast<void*>(1);
                        check(reinterpret_cast<Finish>(vt(context)[114])(context,FALSE,&invalid)==E_NOTIMPL && invalid==reinterpret_cast<void*>(1),
                              "11295 immediate FinishCommandList preserves the original error and untouched output");
                        static unsigned command_slot=0;
                        set_d3d11x_trap_handler([](const char*,unsigned slot)->std::uintptr_t {command_slot=slot;return E_NOTIMPL;});
                        execute(context,reinterpret_cast<void*>(1),FALSE);
                        check(command_slot==58,"ExecuteCommandList rejects unreadable list identity before submission");
                        reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,UINT)>(vt(deferred)[197])(deferred,0xffffffffu);
                        check(command_slot==197,"unknown GPU pipeline events stay explicitly unsupported");
                        reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,UINT,void*,SIZE_T)>(vt(deferred)[152])(deferred,0x400000,reinterpret_cast<void*>(1),16);
                        check(command_slot==152,"unverified cache ranges stay explicitly unsupported");
                        reinterpret_cast<void(STDMETHODCALLTYPE*)(void*)>(vt(deferred)[200])(deferred);
                        check(finish(deferred,FALSE,&invalid)==E_NOTIMPL && !invalid,
                              "unsupported deferred commands cannot produce a successful partial list");
                        set_d3d11x_trap_handler(nullptr);
                        void* owned = nullptr;
                        reinterpret_cast<GetDevice>(vt(deferred)[3])(deferred, &owned);
                        check(owned == device && refs(device) == 2, "inline dispatch GetDevice retains the same device");
                        reinterpret_cast<Count>(vt(owned)[2])(owned);
                        check(reinterpret_cast<Count>(vt(deferred)[2])(deferred) == 0 && refs(device) == 1,
                              "deferred context releases internal device ownership");
                    }
                }
                ++refs(context);
                check(reinterpret_cast<Count>(vt(context)[2])(context) == 1, "legacy inline count honoured");
                check(reinterpret_cast<Count>(vt(device)[2])(device) == 0, "legacy public device count reaches zero while context lives");
                check(reinterpret_cast<Count>(vt(context)[2])(context) == 0, "legacy queue retired and objects released");
            }
        }
        check(FAILED(bind_d3d11x_abi(L"xone-absent-graphics-version.dll")) &&
              !resolve_title_import({"d3d11_x.dll", "D3D11XCreateDeviceX", 0}), "failed rebinding clears old ABI");
        auto untrusted = read(argv[1]);
        const std::array<std::uint8_t,4> version_signature{0xbd,0x04,0xef,0xfe};
        auto info = std::search(untrusted.begin(),untrusted.end(),version_signature.begin(),version_signature.end());
        check(info != untrusted.end() && untrusted.end() - info >= 16, "own fixture contains fixed version metadata");
        if (info != untrusted.end() && untrusted.end() - info >= 16 && untrusted.size() < 896000) {
            const DWORD ms = MAKELONG(0,10), ls = MAKELONG(5104,16299);
            std::memcpy(&*(info+8),&ms,4); std::memcpy(&*(info+12),&ls,4);
            untrusted.resize(896000);
            wchar_t temp[MAX_PATH]; GetTempPathW(MAX_PATH,temp);
            const auto path = std::filesystem::path(temp) / (L"xone-era-graphics-identity-" + std::to_wstring(GetCurrentProcessId()) + L".dll");
            { std::ofstream file(path,std::ios::binary); file.write(reinterpret_cast<const char*>(untrusted.data()),untrusted.size()); }
            check(bind_d3d11x_abi(path.wstring()) == HRESULT_FROM_WIN32(ERROR_CRC) &&
                !resolve_title_import({"d3d11_x.dll","D3D11XCreateDeviceX",0}),
                "matching modern version and size cannot enable an unverified module hash or the old ABI");
            check(DeleteFileW(path.c_str()), "own graphics identity negative control is removed");
        }
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
        {
            const auto root=std::filesystem::path(package)/L"metadata-\u03a9";
            std::filesystem::create_directory(root);
            const auto xml=root/L"AppxManifest.xml",exe=root/L"owned.exe";
            std::filesystem::copy_file(std::filesystem::path(__FILE__).parent_path()/L"appmodel/AppxManifest.xml",xml);
            std::filesystem::copy_file(argv[1],exe);
            std::string hash,manifest_hash,error;
            check(xboxone::inventory::fingerprint(exe,hash,error) && xboxone::inventory::fingerprint(xml,manifest_hash,error),
                "package fixture has pinned executable and manifest bytes");
            xboxone::appmodel::Manifest expected;
            check(xboxone::appmodel::read_manifest(xml,manifest_hash,expected,error),"package fixture uses the existing SDK metadata parser");
            constexpr GUID iid_statics{0x4e534bdf,0x2960,0x4878,{0x97,0xa4,0x96,0x24,0xde,0xb7,0x2f,0x2d}};
            IInspectable* factory=nullptr;
            check(activate(L"Windows.ApplicationModel.Package",iid_statics,reinterpret_cast<void**>(&factory))==S_OK && factory,
                "Package statics use the session WinRT boundary");
            if(factory) {
                using Get=HRESULT(STDMETHODCALLTYPE*)(void*,IInspectable**);
                const auto get=reinterpret_cast<Get>((*reinterpret_cast<void***>(factory))[6]);
                IActivationFactory* activation_factory=nullptr;IUnknown* identity=nullptr;
                check(factory->QueryInterface(__uuidof(IActivationFactory),reinterpret_cast<void**>(&activation_factory))==S_OK &&
                    activation_factory && factory->QueryInterface(__uuidof(IUnknown),reinterpret_cast<void**>(&identity))==S_OK &&
                    identity==activation_factory,"Package factory and statics preserve one COM identity");
                if(identity)identity->Release();if(activation_factory)activation_factory->Release();
                IInspectable* retained=nullptr;
                IInspectable* current=reinterpret_cast<IInspectable*>(1);
                set_title_package_root({});
                check(get(factory,nullptr)==E_POINTER && get(factory,&current)==HRESULT_FROM_WIN32(APPMODEL_ERROR_NO_PACKAGE) && !current,
                    "Current refuses an absent package identity and clears failed outputs");
                set_title_package_root(root.wstring(),"owned.exe",hash);
                check(get(factory,&current)==S_OK && current,"Current binds only the verified selected manifest and executable");
                if(current) {
                    constexpr GUID iid_watcher_statics{0xa306e506,0x12f9,0x460a,{0xb0,0xae,0x50,0xba,0x32,0x8c,0x78,0x6c}};
                    IInspectable* watcher_factory=nullptr;
                    check(activate(L"Windows.Xbox.Management.Deployment.PackageTransferWatcher",iid_watcher_statics,
                        reinterpret_cast<void**>(&watcher_factory))==S_OK && watcher_factory,"watcher statics match the original factory IID");
                    if(watcher_factory) {
                        const auto create=reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*,IInspectable*,IInspectable**)>(
                            (*reinterpret_cast<void***>(watcher_factory))[7]);
                        const auto chunks=reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*,IInspectable*,IInspectable*,IInspectable**)>(
                            (*reinterpret_cast<void***>(watcher_factory))[6]);
                        IInspectable* observer=reinterpret_cast<IInspectable*>(1);
                        check(create(watcher_factory,current,nullptr)==E_POINTER && create(watcher_factory,nullptr,&observer)==E_INVALIDARG && !observer,
                            "watcher creation validates outputs and package arguments");
                        check(create(watcher_factory,factory,&observer)==E_INVALIDARG && !observer,
                            "watcher refuses a foreign package identity without dereferencing it");
                        check(chunks(watcher_factory,current,nullptr,&observer)==E_NOTIMPL && !observer,
                            "unproved chunk selection remains explicitly unsupported");
                        check(create(watcher_factory,current,&observer)==S_OK && observer,"watcher observes the verified package without deployment");
                        if(observer) {
                            struct Handler : IUnknown {
                                ULONG refs=1;unsigned calls=0;
                                HRESULT STDMETHODCALLTYPE QueryInterface(REFIID,void** out) override {if(!out)return E_POINTER;*out=nullptr;return E_NOINTERFACE;}
                                ULONG STDMETHODCALLTYPE AddRef() override {return ++refs;}
                                ULONG STDMETHODCALLTYPE Release() override {return --refs;}
                                virtual HRESULT STDMETHODCALLTYPE Invoke(IInspectable*,IInspectable*) {++calls;return S_OK;}
                            } handler;
                            constexpr GUID iid_watcher{0xe74b8e43,0xba51,0x4b09,{0xad,0x3f,0x01,0xba,0xff,0x31,0xf9,0x34}};
                            IInspectable* same=nullptr;
                            check(observer->QueryInterface(iid_watcher,reinterpret_cast<void**>(&same))==S_OK && same==observer,
                                "watcher preserves the Xbox instance IID and identity");
                            if(same)same->Release();
                            auto** vt=*reinterpret_cast<void***>(observer);
                            for(UINT slot:{7u,9u,11u}) {
                                auto add=reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*,IUnknown*,std::int64_t*)>(vt[slot]);
                                auto remove=reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*,std::int64_t)>(vt[slot+1]);
                                std::int64_t token=0;
                                check(add(observer,nullptr,&token)==E_INVALIDARG && add(observer,&handler,nullptr)==E_INVALIDARG && handler.refs==1,
                                    "watcher refuses invalid registrations without retaining handlers");
                                check(add(observer,&handler,&token)==S_OK && token!=0 && handler.refs==2 && handler.calls==0,
                                    "watcher retains subscriptions without inventing transfer events");
                                check(remove(observer,token)==S_OK && remove(observer,token)==S_OK && handler.refs==1,
                                    "watcher removes each event subscription exactly once");
                            }
                            std::int64_t token=0;
                            reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*,IUnknown*,std::int64_t*)>(vt[7])(observer,&handler,&token);
                            IWeakReferenceSource* source=nullptr;IWeakReference* weak=nullptr;
                            check(observer->QueryInterface(__uuidof(IWeakReferenceSource),reinterpret_cast<void**>(&source))==S_OK && source,
                                "watcher supports the original caller's weak event ownership");
                            if(source) {
                                IUnknown* identity=nullptr;
                                check(source->QueryInterface(__uuidof(IUnknown),reinterpret_cast<void**>(&identity))==S_OK && identity==observer,
                                    "weak source shares the watcher's COM identity");
                                if(identity)identity->Release();
                                check(source->GetWeakReference(nullptr)==E_POINTER && source->GetWeakReference(&weak)==S_OK && weak,
                                    "weak references validate outputs and retain their own control");
                                source->Release();
                            }
                            if(weak) {
                                check(weak->Resolve(iid_watcher,&same)==S_OK && same==observer,"live weak reference resolves the original watcher");
                                if(same)same->Release();
                                same=reinterpret_cast<IInspectable*>(1);
                                check(weak->Resolve(GUID_NULL,&same)==E_NOINTERFACE && !same,"weak resolution preserves unsupported IID failures");
                            }
                            set_title_package_root({});
                            IInspectable* absent=reinterpret_cast<IInspectable*>(1);
                            check(create(watcher_factory,current,&absent)==HRESULT_FROM_WIN32(APPMODEL_ERROR_NO_PACKAGE) && !absent,
                                "watcher cannot create an observer after session unbinding");
                            observer->Release();
                            check(handler.refs==1 && handler.calls==0,"watcher teardown releases pending handlers without completion callbacks");
                            if(weak) {
                                same=reinterpret_cast<IInspectable*>(1);
                                check(weak->Resolve(iid_watcher,&same)==S_OK && !same,"expired weak reference cannot resurrect a destroyed watcher");
                                weak->Release();
                            }
                            set_title_package_root(root.wstring(),"owned.exe",hash);
                        }
                        watcher_factory->Release();
                    }
                    IInspectable* second=nullptr;
                    check(get(factory,&second)==S_OK && second==current,"Current retains one COM identity for an immutable package revision");
                    if(second)second->Release();
                    xboxone::appmodel::Manifest metadata;std::wstring location;std::string revision;
                    check(title_package_metadata(metadata,location,revision)==S_OK && metadata.name==expected.name &&
                        metadata.version==expected.version && location==root.wstring() && revision==manifest_hash+hash,
                        "Current context preserves the real SDK metadata and both hashes");
                    set_title_package_root({});second=reinterpret_cast<IInspectable*>(1);
                    HSTRING value=nullptr;
                    check(get(factory,&second)==HRESULT_FROM_WIN32(APPMODEL_ERROR_NO_PACKAGE) && !second &&
                        current->GetRuntimeClassName(&value)==S_OK && text(value)==L"Windows.ApplicationModel.Package",
                        "rebinding invalidates Current while retained snapshots remain alive");
                    retained=current;
                }
                set_title_package_root(root.wstring(),"../owned.exe",hash);
                check(get(factory,&current)==E_INVALIDARG && !current,"package executable traversal is refused");
                set_title_package_root(root.wstring(),"owned.exe",std::string(64,'0'));
                check(get(factory,&current)==HRESULT_FROM_WIN32(ERROR_CRC) && !current,"selected executable hash mismatch cannot produce a package");
                set_title_package_root(root.wstring(),"absent.exe",hash);
                check(FAILED(get(factory,&current)) && !current,"missing selected executable cannot produce a package");
                const auto other=root/L"other.exe";std::filesystem::copy_file(exe,other);
                set_title_package_root(root.wstring(),"other.exe",hash);
                check(get(factory,&current)==HRESULT_FROM_WIN32(ERROR_BAD_EXE_FORMAT) && !current,
                    "a valid hash cannot bind an executable absent from the manifest");
                std::filesystem::remove(other);
                set_title_package_root(root.wstring(),"owned.exe",hash);
                {std::ofstream changed(xml,std::ios::app);changed<<' ';}
                check(get(factory,&current)==HRESULT_FROM_WIN32(ERROR_INVALID_DATA) && !current,"manifest mutation after binding invalidates the snapshot");
                {std::ofstream changed(xml);changed<<"<!DOCTYPE Package [<!ENTITY x SYSTEM 'file:///outside'>]><Package>&x;</Package>";}
                set_title_package_root(root.wstring(),"owned.exe",hash);
                check(FAILED(get(factory,&current)) && !current,"malformed or external-entity manifest cannot manufacture a package");
                {std::ofstream legacy(xml);legacy<<
                    "<Package xmlns=\"http://schemas.microsoft.com/appx/2010/manifest\">"
                    "<Identity Name=\"Owned.Legacy\" Publisher=\"CN=Owned\" Version=\"1.2.3.4\" ProcessorArchitecture=\"x64\"/>"
                    "<Properties><DisplayName>Owned</DisplayName><PublisherDisplayName>Owned</PublisherDisplayName><Logo>logo.png</Logo></Properties>"
                    "<Prerequisites><OSMinVersion>6.2</OSMinVersion><OSMaxVersionTested>6.2</OSMaxVersionTested></Prerequisites>"
                    "<Resources><Resource Language=\"en-US\"/></Resources><Applications>"
                    "<Application Id=\"App\" Executable=\"owned.exe\" EntryPoint=\"Owned.App\">"
                    "<VisualElements DisplayName=\"Owned\" Logo=\"logo.png\" SmallLogo=\"small.png\" Description=\"Owned\" ForegroundText=\"light\" BackgroundColor=\"#222222\"><SplashScreen Image=\"splash.png\"/></VisualElements>"
                    "</Application></Applications></Package>";}
                check(xboxone::inventory::fingerprint(xml,manifest_hash,error) &&
                    xboxone::appmodel::read_manifest(xml,manifest_hash,expected,error) && expected.device_families.empty() && !expected.unsupported_contracts.empty(),
                    "Appx 2010 metadata preserves absent device families and explicit unsupported activation contracts");
                set_title_package_root(root.wstring(),"owned.exe",hash);
                check(get(factory,&current)==S_OK && current,"Current supports verified legacy metadata without host deployment");
                check(expected.version==0x0001000200030004ull,"legacy metadata preserves all four version components");
                if(current)current->Release();
                if(retained) {
                    HSTRING value=nullptr;
                    check(retained->GetRuntimeClassName(&value)==S_OK && text(value)==L"Windows.ApplicationModel.Package",
                        "a retained snapshot survives cache replacement by another package revision");
                    retained->Release();
                }
                factory->Release();
            }
            set_title_package_root(package);
            std::filesystem::remove(exe);std::filesystem::remove(xml);std::filesystem::remove(root);
        }
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
        {
            const auto previous_input_root = title_storage_root();
            wchar_t input_temp[MAX_PATH]{}; GetTempPathW(MAX_PATH, input_temp);
            const auto input_root = std::filesystem::path(input_temp) / (L"xone-input-profile-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
            set_title_storage_root(input_root.wstring());
            constexpr GUID controller_statics{0x7400624f,0x144b,0x47fc,{0x9f,0x3b,0x1d,0xca,0xfb,0x48,0x28,0x73}};
            constexpr GUID gamepad_statics{0x8c4c1bcd,0xfdab,0x4bf8,{0xbd,0xe9,0xab,0xb2,0xb1,0xd6,0xfd,0xd3}};
            IInspectable* input = nullptr;
            check(activate(L"Windows.Xbox.Input.Controller",controller_statics,reinterpret_cast<void**>(&input)) == S_OK && input,
                "observed Controller statics are implemented in the common runtime");
            using GetView = HRESULT(STDMETHODCALLTYPE*)(void*, IInspectable**);
            using Size = HRESULT(STDMETHODCALLTYPE*)(void*, UINT*);
            using Get = HRESULT(STDMETHODCALLTYPE*)(void*, UINT, IUnknown**);
            IInspectable* view = nullptr;
            if (input) {
                const auto get = reinterpret_cast<GetView>((*reinterpret_cast<void***>(input))[6]);
                check(get(input,nullptr) == E_POINTER && get(input,&view) == S_OK && view, "real XInput enumeration validates outputs");
            }
            if (view) {
                const auto vt = *reinterpret_cast<void***>(view);
                UINT count = 99;
                check(reinterpret_cast<Size>(vt[7])(view,&count) == S_OK && count <= 4, "controller snapshot reflects the bounded host API");
                IUnknown* item = reinterpret_cast<IUnknown*>(1);
                check(reinterpret_cast<Get>(vt[6])(view,count,&item) == E_BOUNDS && !item, "snapshot rejects an out-of-range controller");
                if (count) {
                    check(reinterpret_cast<Get>(vt[6])(view,0,&item) == S_OK && item, "connected host controller has a retained interface");
                    if (item) {
                        IUnknown* identity = nullptr;
                        check(item->QueryInterface(__uuidof(IUnknown),reinterpret_cast<void**>(&identity)) == S_OK && identity == item,
                            "controller COM identity is preserved");
                        if (identity) identity->Release();
                        using GetUser = HRESULT(STDMETHODCALLTYPE*)(void*, IInspectable**);
                        const auto get_user = reinterpret_cast<GetUser>((*reinterpret_cast<void***>(item))[8]);
                        IInspectable* paired = nullptr; IInspectable* selected = nullptr;
                        check(get_user(item, nullptr) == E_POINTER, "Controller.User validates its output");
                        check(get_user(item, &paired) == S_OK && paired && title_local_user(reinterpret_cast<void**>(&selected)) == S_OK && paired == selected,
                            "physical controller retains the same local profile identity as users and saves");
                        if (paired) {
                            UINT id = 0; GUID profile{}; std::uint32_t expected = 0;
                            const auto user_id = reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*, UINT*)>((*reinterpret_cast<void***>(paired))[6]);
                            check(title_local_user_identity(&profile, &expected) == S_OK && user_id(paired, &id) == S_OK && id == expected,
                                "Controller.User exposes the persisted local user ID without an online identity");
                            stop_title_input();
                            IInspectable* retained_user = nullptr;
                            check(user_id(paired, &id) == S_OK && id == expected && get_user(item, &retained_user) == S_OK && retained_user == paired,
                                "retained controller and user snapshots survive input registry teardown");
                            if (retained_user) retained_user->Release();
                            paired->Release();
                        }
                        if (selected) selected->Release();
                        constexpr GUID gamepad_iid{0x082e402c,0x07df,0x45e1,{0xa5,0xab,0xa3,0x12,0x7a,0xf1,0x97,0xb5}};
                        IUnknown* gamepad=nullptr;
                        check(item->QueryInterface(gamepad_iid,reinterpret_cast<void**>(&gamepad))==S_OK && gamepad,
                            "host controller shares its gamepad identity");
                        if (gamepad) {
                            using Vibrate = HRESULT(STDMETHODCALLTYPE*)(void*, const std::array<float,4>*);
                            const auto vibrate = reinterpret_cast<Vibrate>((*reinterpret_cast<void***>(gamepad))[6]);
                            std::array<float,4> vibration{};
                            check(vibrate(gamepad,nullptr) == E_INVALIDARG && vibrate(gamepad,reinterpret_cast<const std::array<float,4>*>(1)) == E_INVALIDARG,
                                  "vibration validates its four-float x64 ABI input");
                            const HRESULT stopped = vibrate(gamepad,&vibration);
                            check(stopped == S_OK || stopped == HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED), "zero vibration reaches the real XInput device");
                            for (float invalid : {-1.f, 1.01f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
                                vibration[0] = invalid;
                                check(vibrate(gamepad,&vibration) == E_INVALIDARG, "invalid motor levels cannot reach the physical controller");
                            }
                            vibration = {0,0,1,0};
                            check(vibrate(gamepad,&vibration) == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED), "trigger motors remain explicit when the host transport cannot execute them");
                            using ReadObject = HRESULT(STDMETHODCALLTYPE*)(void*, IInspectable**);
                            const auto current_reading = reinterpret_cast<ReadObject>((*reinterpret_cast<void***>(gamepad))[7]);
                            IInspectable* snapshot = nullptr;
                            FILETIME before_time{}, after_time{}; GetSystemTimePreciseAsFileTime(&before_time);
                            check(current_reading(gamepad, nullptr) == E_POINTER, "Gamepad.GetCurrentReading validates its output");
                            const HRESULT snapshot_hr = current_reading(gamepad, &snapshot);
                            GetSystemTimePreciseAsFileTime(&after_time);
                            check(snapshot_hr == HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED) || (snapshot_hr == S_OK && snapshot),
                                "connected physical gamepad returns a real reading snapshot or its disconnection error");
                            if (snapshot) {
                                auto** reading_table = *reinterpret_cast<void***>(snapshot);
                                UINT64 timestamp = 0; UINT buttons = 0;
                                check(reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*,UINT64*)>(reading_table[6])(snapshot,&timestamp) == S_OK &&
                                      timestamp >= ((UINT64(before_time.dwHighDateTime)<<32)|before_time.dwLowDateTime) &&
                                      timestamp <= ((UINT64(after_time.dwHighDateTime)<<32)|after_time.dwLowDateTime), "reading DateTime uses the actual UTC FILETIME clock");
                                check(reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*,UINT*)>(reading_table[7])(snapshot,&buttons) == S_OK,
                                      "reading snapshot exposes its captured buttons");
                                const UINT masks[]{0x100,0x200,0x400,0x800,4,8,0x4000,0x8000,0x1000,0x2000,0x10,0x20,0x40,0x80};
                                for (UINT i = 0; i < std::size(masks); ++i) {
                                    struct { boolean value = false; BYTE guard = 0xa5; } pressed;
                                    const auto get = reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*,boolean*)>(reading_table[8+i]);
                                    check(get(snapshot,nullptr) == E_POINTER && get(snapshot,&pressed.value) == S_OK && bool(pressed.value) == bool(buttons & masks[i]) && pressed.guard == 0xa5,
                                          "reading button getter matches the same snapshot and preserves its one-byte ABI");
                                }
                                for (UINT i = 0; i < 6; ++i) {
                                    float value = 9, repeated = 10;
                                    const auto get = reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*,float*)>(reading_table[22+i]);
                                    check(get(snapshot,nullptr) == E_POINTER && get(snapshot,&value) == S_OK && get(snapshot,&repeated) == S_OK &&
                                          value == repeated && value >= (i < 2 ? 0.f : -1.f) && value <= 1.f, "reading axes preserve normalized immutable values");
                                }
                                IUnknown* canonical = nullptr;
                                check(snapshot->QueryInterface(__uuidof(IUnknown),reinterpret_cast<void**>(&canonical)) == S_OK && canonical == snapshot,
                                      "reading snapshot retains canonical COM identity");
                                if (canonical) canonical->Release();
                                snapshot->Release();
                            }
                            using Read=HRESULT(STDMETHODCALLTYPE*)(void*,RawGamepadReading*);
                            const auto read=reinterpret_cast<Read>((*reinterpret_cast<void***>(gamepad))[8]);
                            struct { RawGamepadReading value; UINT64 guard=0x1122334455667788ull; } sample{};
                            check(read(gamepad,nullptr)==E_POINTER,"raw gamepad reading validates its output");
                            const HRESULT result=read(gamepad,&sample.value);
                            check((result==HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED) || (result==S_OK && sample.value.timestamp &&
                                sample.value.left_trigger>=0 && sample.value.left_trigger<=1 && sample.value.right_trigger>=0 && sample.value.right_trigger<=1 &&
                                sample.value.left_x>=-1 && sample.value.left_x<=1 && sample.value.right_y>=-1 && sample.value.right_y<=1)) &&
                                sample.guard==0x1122334455667788ull,"real gamepad read respects normalized ranges, disconnection and its 40-byte ABI");
                            gamepad->Release();
                        }
                        item->Release();
                    }
                }
                view->Release();
            }
            struct InputHandler : IUnknown {
                std::atomic<ULONG> refs{1};bool agile=true;
                HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out) override {
                    if(!out)return E_POINTER;*out=nullptr;
                    if(iid!=__uuidof(IUnknown) && !(agile && iid==__uuidof(IAgileObject)))return E_NOINTERFACE;
                    *out=this;AddRef();return S_OK;
                }
                ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
                ULONG STDMETHODCALLTYPE Release() override { return --refs; }
                virtual HRESULT STDMETHODCALLTYPE Invoke(IInspectable*,IInspectable*) { return S_OK; }
            } handler;
            using AddInput = HRESULT(STDMETHODCALLTYPE*)(void*, IUnknown*, INT64*);
            using RemoveInput = HRESULT(STDMETHODCALLTYPE*)(void*, INT64);
            if (input) {
                auto vt = *reinterpret_cast<void***>(input); INT64 token = 0;
                check(reinterpret_cast<AddInput>(vt[7])(input,&handler,&token) == S_OK && token && handler.refs == 2,
                    "input subscription retains the real handler");
                check(reinterpret_cast<RemoveInput>(vt[8])(input,token) == S_OK && handler.refs == 1,
                    "input unsubscription releases handler ownership");
                HRESULT worker_add=E_FAIL,worker_poll=S_OK,nonagile_add=S_OK;INT64 worker_token=0,rejected_token=99;
                InputHandler nonagile;nonagile.agile=false;
                std::thread input_worker([&] {
                    worker_add=reinterpret_cast<AddInput>(vt[7])(input,&handler,&worker_token);
                    worker_poll=poll_title_input();
                    nonagile_add=reinterpret_cast<AddInput>(vt[7])(input,&nonagile,&rejected_token);
                });input_worker.join();
                check(worker_add==S_OK && worker_token && handler.refs==2 && worker_poll==RPC_E_WRONG_THREAD,
                    "worker subscription of an agile delegate preserves dispatcher polling ownership");
                check(nonagile_add==RPC_E_WRONG_THREAD && rejected_token==0 && nonagile.refs==1,
                    "non-agile worker delegate is refused without a marshaling path or leaked reference");
                check(reinterpret_cast<RemoveInput>(vt[8])(input,worker_token)==S_OK && handler.refs==1 && poll_title_input()==S_OK,
                    "worker event token can be removed while polling remains on the dispatcher");
                check(reinterpret_cast<AddInput>(vt[11])(input,&handler,&token) == S_OK && handler.refs == 2,
                    "observed pairing subscription preserves its callback");
                stop_title_input();
                check(handler.refs == 1, "session teardown releases subscriptions and stops the input timer");
                input->Release();
            }
            IInspectable* pads = nullptr;
            check(activate(L"Windows.Xbox.Input.Gamepad",gamepad_statics,reinterpret_cast<void**>(&pads)) == S_OK && pads,
                "Gamepad statics use the same host-backed controller registry");
            if (pads) pads->Release();
            stop_title_input();
            set_title_storage_root(previous_input_root);
            std::filesystem::remove_all(input_root);
        }
        check(activate(L"Windows.Xbox.Input.BodyController", __uuidof(IActivationFactory), &missing) == REGDB_E_CLASSNOTREG && !missing,
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
                    const auto batch=reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*,IInspectable*,boolean*)>(
                        (*reinterpret_cast<void***>(instance))[8]);
                    struct Sequence : WinrtObject {
                        struct Cursor : WinrtObject {Sequence* owner=nullptr;} cursor;
                        std::vector<UINT32> values;size_t position=0;ULONG cursor_refs=0;
                        HRESULT failure=S_OK;bool null_cursor=false;
                        static HRESULT STDMETHODCALLTYPE first(Sequence* self,void** out) {
                            *out=nullptr;self->position=0;
                            if(self->null_cursor)return S_OK;
                            ++self->cursor_refs;*out=&self->cursor;return S_OK;
                        }
                        static ULONG STDMETHODCALLTYPE retain(Cursor* self) {return ++self->owner->cursor_refs;}
                        static ULONG STDMETHODCALLTYPE release(Cursor* self) {return --self->owner->cursor_refs;}
                        static HRESULT STDMETHODCALLTYPE current(Cursor* self,UINT32* out) {
                            if(FAILED(self->owner->failure))return self->owner->failure;
                            if(self->owner->position>=self->owner->values.size())return E_BOUNDS;
                            *out=self->owner->values[self->owner->position];return S_OK;
                        }
                        static HRESULT STDMETHODCALLTYPE has(Cursor* self,boolean* out) {
                            *out=self->owner->position<self->owner->values.size();return S_OK;
                        }
                        static HRESULT STDMETHODCALLTYPE next(Cursor* self,boolean* out) {++self->owner->position;return has(self,out);}
                        Sequence() {
                            static const auto collection=[] {auto t=winrt_vtable();t[6]=reinterpret_cast<const void*>(&first);return t;}();
                            static const auto iterator=[] {auto t=winrt_vtable();t[1]=reinterpret_cast<const void*>(&retain);t[2]=reinterpret_cast<const void*>(&release);
                                t[6]=reinterpret_cast<const void*>(&current);t[7]=reinterpret_cast<const void*>(&has);t[8]=reinterpret_cast<const void*>(&next);return t;}();
                            static_cast<WinrtObject&>(*this)={collection.data(),L"Owned.Chunks",
                                winrt_pinterface_iid("pinterface({faa585ea-6214-4217-afda-7f46de5869b3};u4)"),nullptr};
                            static_cast<WinrtObject&>(cursor)={iterator.data(),L"Owned.ChunkIterator",GUID_NULL,nullptr};cursor.owner=this;
                        }
                    } sequence;
                    auto* iterable=reinterpret_cast<IInspectable*>(&sequence);
                    check(installed(instance, 69, nullptr) == E_POINTER, "chunk query validates its output");
                    boolean present = true;
                    check(batch(instance,iterable,nullptr)==E_POINTER && batch(instance,nullptr,&present)==E_INVALIDARG && !present,
                        "batch chunk query validates arguments and clears failed outputs");
                    check(FAILED(batch(instance,iterable,&present)) && !present && !sequence.cursor_refs,
                        "empty batch cannot hide a missing catalog or leak its iterator");
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
                    check(batch(instance,iterable,&present)==HRESULT_FROM_WIN32(ERROR_INVALID_DATA) && !present && !sequence.cursor_refs,
                        "empty batch preserves malformed catalog failures");
                    {
                        std::ofstream out(catalog, std::ios::binary | std::ios::trunc);
                        const unsigned char record[12]{0,0,0,0,0,0,0,0,69,0,0,0};
                        out.write(reinterpret_cast<const char*>(record), sizeof record);
                    }
                    check(installed(instance, 69, &present) == S_OK && present, "local catalog supplies installed chunk");
                    check(installed(instance, 70, &present) == S_OK && !present, "absent chunk is not invented");
                    check(installed(instance, 0xffffffff, &present) == S_OK && !present, "reserved chunk never installed");
                    check(batch(instance,iterable,&present)==S_OK && present && !sequence.cursor_refs,"empty batch is true only for a valid catalog");
                    sequence.values={69,69};
                    check(batch(instance,iterable,&present)==S_OK && present && !sequence.cursor_refs,"batch checks every requested local chunk, including duplicates");
                    sequence.values={69,70};
                    check(batch(instance,iterable,&present)==S_OK && !present,"a missing batch member is never reported installed");
                    sequence.values={0xffffffff};
                    check(batch(instance,iterable,&present)==S_OK && !present,"reserved batch chunk is never invented");
                    sequence.failure=E_ABORT;
                    check(batch(instance,iterable,&present)==E_ABORT && !present && !sequence.cursor_refs,"iterator failure propagates without publishing partial success");
                    sequence.failure=S_OK;sequence.null_cursor=true;
                    check(batch(instance,iterable,&present)==E_UNEXPECTED && !present,"malformed iterable cannot return a null successful iterator");
                    sequence.null_cursor=false;
                    check(batch(instance,instance,&present)==E_NOINTERFACE && !present,"batch refuses an object without IIterable<UInt32>");
                    set_title_package_root(package);
                    DeleteFileW(catalog.c_str());
                    check(FAILED(installed(instance, 69, &present)), "package rebinding invalidates cached chunk state");
                    check(FAILED(batch(instance,iterable,&present)) && !present,"batch shares catalog invalidation with single-chunk queries");
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
            {
                using Users=HRESULT(STDMETHODCALLTYPE*)(void*,void**);
                const auto users=reinterpret_cast<Users>((*reinterpret_cast<void***>(user_statics))[9]);
                const auto previous_root=xboxone::era::title_storage_root();
                wchar_t temporary[MAX_PATH]{}; GetTempPathW(MAX_PATH,temporary);
                const auto root=std::filesystem::path(temporary)/(L"xone-profile-test-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
                const auto second=std::filesystem::path(root.wstring()+L"-second");
                xboxone::era::set_title_storage_root(L"");
                void* missing=reinterpret_cast<void*>(1);
                check(users(user_statics,&missing)==HRESULT_FROM_WIN32(ERROR_NOT_READY) && !missing,
                      "missing profile storage remains explicit rather than returning an invented user or empty success");
                xboxone::era::set_title_storage_root(root.wstring());
                GUID identity{};UINT32 local_id=0;
                check(xboxone::era::title_local_user_identity(&identity,&local_id)==S_OK && identity!=GUID{} && local_id==1,
                      "offline profile persists a genuine local identity in the session storage root");
                const auto profile=root/L".xone-local-profile";
                check(std::filesystem::file_size(profile)==32,"profile publication is bounded to its versioned record");
                IInspectable* view=nullptr;
                check(users(user_statics,reinterpret_cast<void**>(&view))==S_OK && view,"Users returns an owning snapshot of the configured profile");
                if (view) {
                    auto** table=*reinterpret_cast<void***>(view);
                    using Get=HRESULT(STDMETHODCALLTYPE*)(void*,UINT,void**);
                    using Size=HRESULT(STDMETHODCALLTYPE*)(void*,UINT*);
                    using Many=HRESULT(STDMETHODCALLTYPE*)(void*,UINT,UINT,void**,UINT*);
                    using Index=HRESULT(STDMETHODCALLTYPE*)(void*,void*,UINT*,boolean*);
                    IInspectable* item=nullptr; UINT size=0;
                    check(reinterpret_cast<Size>(table[7])(view,&size)==S_OK && size==1,"snapshot count matches the persisted local profile");
                    check(reinterpret_cast<Get>(table[6])(view,0,reinterpret_cast<void**>(&item))==S_OK && item,"snapshot retains its user object");
                    void* invalid=reinterpret_cast<void*>(1);
                    check(reinterpret_cast<Get>(table[6])(view,1,&invalid)==E_BOUNDS && !invalid,"User GetAt rejects an out-of-range index");
                    UINT actual=99; void* values[2]{nullptr,reinterpret_cast<void*>(1)};
                    check(reinterpret_cast<Many>(table[9])(view,0,2,values,&actual)==S_OK && actual==1 && values[0]==item && values[1]==reinterpret_cast<void*>(1),
                          "User GetMany retains exactly the returned items and preserves output guards");
                    if (values[0]) static_cast<IUnknown*>(values[0])->Release();
                    check(reinterpret_cast<Many>(table[9])(view,1,0,nullptr,&actual)==S_OK && actual==0 &&
                          reinterpret_cast<Many>(table[9])(view,2,0,nullptr,&actual)==E_BOUNDS && actual==0,
                          "User GetMany distinguishes the end of a snapshot from invalid bounds");
                    UINT index=99;boolean found=false;
                    check(reinterpret_cast<Index>(table[8])(view,item,&index,&found)==S_OK && found && index==0,"User IndexOf preserves logical identity");
                    if (item) {
                        UINT id=0;auto** it=*reinterpret_cast<void***>(item);
                        check(reinterpret_cast<Size>(it[6])(item,&id)==S_OK && id==local_id,"IUser local ID comes from the shared profile record");
                        using UserBoolean=HRESULT(STDMETHODCALLTYPE*)(void*,boolean*);
                        boolean guest=true;
                        check(reinterpret_cast<UserBoolean>(it[10])(item,&guest)==S_OK && !guest &&
                              reinterpret_cast<UserBoolean>(it[10])(item,nullptr)==E_POINTER,
                              "persisted local profile is not a sponsored guest session");
                        boolean signed_in=false;UINT location=99;void* sponsor=reinterpret_cast<void*>(1);
                        check(reinterpret_cast<UserBoolean>(it[11])(item,&signed_in)==S_OK && signed_in &&
                              reinterpret_cast<Size>(it[12])(item,&location)==S_OK && location==0 &&
                              reinterpret_cast<Users>(it[13])(item,&sponsor)==S_OK && !sponsor,
                              "selected persistent user is locally signed in and has no guest sponsor");
                        using UserString=HRESULT(STDMETHODCALLTYPE*)(void*,HSTRING*);
                        HSTRING online_id=reinterpret_cast<HSTRING>(1);
                        check(reinterpret_cast<UserString>(it[15])(item,&online_id)==E_NOTIMPL && !online_id &&
                              reinterpret_cast<UserString>(it[15])(item,nullptr)==E_POINTER,
                              "local profile cannot manufacture an Xbox services identity");
                        IInspectable* display=nullptr;
                        check(reinterpret_cast<Users>(it[9])(item,reinterpret_cast<void**>(&display))==S_OK && display,
                              "User.DisplayInfo retains the same local profile owner");
                        if (display) {
                            HSTRING label=nullptr;
                            using Name=HRESULT(STDMETHODCALLTYPE*)(void*,HSTRING*);
                            check(reinterpret_cast<Name>((*reinterpret_cast<void***>(display))[6])(display,&label)==S_OK && text(label)==L"Local Player",
                                  "offline display label uses the reference's HSTRING ABI without claiming an online identity");
                            WindowsDeleteString(label);display->Release();
                        }
                    }
                    constexpr GUID iterable_iid{0x1e1813fd,0xf62e,0x5198,{0xb2,0x07,0x9a,0xa8,0x77,0x14,0x7c,0x08}};
                    IInspectable* iterable=nullptr;
                    check(view->QueryInterface(iterable_iid,reinterpret_cast<void**>(&iterable))==S_OK && iterable,"user snapshot exposes the reference's class-derived iterable IID");
                    if (iterable) {
                        IInspectable* iterator=nullptr;auto** vt=*reinterpret_cast<void***>(iterable);
                        check(reinterpret_cast<Users>(vt[6])(iterable,reinterpret_cast<void**>(&iterator))==S_OK && iterator,"First creates an independently owned iterator");
                        iterable->Release();
                        if (iterator) {
                            auto** iv=*reinterpret_cast<void***>(iterator);IInspectable* current=nullptr;
                            check(reinterpret_cast<Users>(iv[6])(iterator,reinterpret_cast<void**>(&current))==S_OK && current==item,"iterator retains the same profile identity");
                            if (current) current->Release();
                            using Advance=HRESULT(STDMETHODCALLTYPE*)(void*,boolean*);
                            boolean more=true;
                            check(reinterpret_cast<Advance>(iv[8])(iterator,&more)==S_OK && !more &&
                                  reinterpret_cast<Users>(iv[6])(iterator,reinterpret_cast<void**>(&current))==E_BOUNDS && !current,
                                  "iterator end never fabricates another user");
                            iterator->Release();
                        }
                    }
                    xboxone::era::set_title_storage_root(second.wstring());
                    GUID second_identity{};
                    check(xboxone::era::title_local_user_identity(&second_identity,&local_id)==S_OK && second_identity!=identity,
                          "separate session storage cannot alias a different persisted profile");
                    if (item) {
                        UINT id=0;check(reinterpret_cast<Size>((*reinterpret_cast<void***>(item))[6])(item,&id)==S_OK && id==1,
                            "existing user snapshots remain alive after storage-root replacement");item->Release();
                    }
                    view->Release();
                }
                xboxone::era::set_title_storage_root(root.wstring());GUID reloaded{};
                check(xboxone::era::title_local_user_identity(&reloaded,&local_id)==S_OK && reloaded==identity,
                      "profile identity survives cache reset and reopening its persistent record");
                { std::ofstream corrupt(profile,std::ios::binary|std::ios::trunc);corrupt<<"invalid"; }
                xboxone::era::set_title_storage_root(root.wstring());
                check(users(user_statics,&missing)==HRESULT_FROM_WIN32(ERROR_INVALID_DATA) && !missing && std::filesystem::file_size(profile)==7,
                      "corrupt profile is refused and preserved rather than regenerated over existing identity");
                xboxone::era::set_title_storage_root(previous_root);
                const auto remove_profile=[](const std::filesystem::path& directory) {
                    const auto file=directory/L".xone-local-profile";bool deleted=false;
                    for (UINT attempt=0;attempt<20;++attempt) {
                        if (!deleted) deleted=DeleteFileW(file.c_str())!=FALSE;
                        if (deleted && RemoveDirectoryW(directory.c_str())) return true;
                        Sleep(10); // A scanner can keep a just-deleted file pending briefly.
                    }
                    std::cerr<<"profile fixture cleanup Win32 "<<GetLastError()<<'\n';return false;
                };
                check(remove_profile(root),"profile fixture removes only its owned record and directory");
                const auto second_profile=second/L".xone-local-profile";
                if (std::filesystem::exists(second_profile)) check(remove_profile(second),"second profile fixture removes its own record");
            }
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
                    check(core_window_hwnd(nullptr) && core_window_hwnd(nullptr) == core_window_hwnd(window),
                        "Xbox null-window swapchain binding selects the same owned local window");
                    constexpr GUID iid_window_static{0x4d239005,0x3c2a,0x41b1,{0x90,0x22,0x53,0x6b,0xb9,0xcf,0x93,0xb1}};
                    IInspectable* statics = nullptr;
                    check(core_window_factory(iid_window_static,reinterpret_cast<void**>(&statics)) == S_OK && statics,
                        "CoreWindow static factory is served by the same runtime");
                    if (statics) {
                        using Current = HRESULT(STDMETHODCALLTYPE*)(void*,IInspectable**);
                        const auto get = reinterpret_cast<Current>((*reinterpret_cast<void***>(statics))[6]);
                        IInspectable* current = nullptr;
                        check(get(statics,nullptr) == E_POINTER && get(statics,&current) == S_OK && current == window,
                            "GetForCurrentThread returns the exact SetWindow identity");
                        if (current) current->Release();
                        std::thread foreign([&] {
                            check(!core_window_hwnd(nullptr), "null-window binding never selects another thread's window");
                            IInspectable* other = reinterpret_cast<IInspectable*>(1);
                            check(get(statics,&other) == S_OK && !other, "foreign thread has no runtime CoreWindow");
                        });
                        foreign.join(); statics->Release();
                    }
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
