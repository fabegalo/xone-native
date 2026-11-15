// SPDX-License-Identifier: BSD-3-Clause
#include "era_display.h"
#include "era_session.h"
#include <d3dcompiler.h>
#include <cstring>
#include <climits>
#include <utility>
#include <cstdio>
#include <atomic>
#include <algorithm>

namespace xboxone::era {
namespace {
std::atomic<SIZE_T> layout_bytes{0};
constexpr SIZE_T layout_budget=128u<<20;
void layout_transition(ID3D12GraphicsCommandList* list,ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};list->ResourceBarrier(1,&barrier);
}
}
TextureLayoutTransfer::~TextureLayoutTransfer() {
    offsets_.Reset();upload_.Reset();linear_.Reset();tiled_.Reset();readback_.Reset();
    if(retained_bytes_)layout_bytes.fetch_sub(retained_bytes_);
}
HRESULT TextureLayoutTransfer::initialize(ID3D12Device* device,std::span<const UINT32> offsets,UINT width,UINT height,
        const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint,SIZE_T tiled_bytes) try {
    if(!device || retained_bytes_)return E_INVALIDARG;
    const UINT64 count=UINT64(width)*height,linear_bytes=UINT64(footprint.Footprint.RowPitch)*height;
    if(!width || !height || offsets.size()!=count || !tiled_bytes || footprint.Offset ||
        footprint.Footprint.Width!=width || footprint.Footprint.Height!=height || footprint.Footprint.Depth!=1 ||
        footprint.Footprint.RowPitch<UINT64(width)*4 || footprint.Footprint.RowPitch%4)return E_INVALIDARG;
    if(count>65535u*64 || tiled_bytes>(16u<<20) || linear_bytes>(16u<<20))return S_FALSE;
    const SIZE_T padded=(tiled_bytes+3)&~SIZE_T(3);
    auto sorted=std::vector<UINT32>(offsets.begin(),offsets.end());std::sort(sorted.begin(),sorted.end());
    for(SIZE_T i=0;i<sorted.size();++i)
        if(sorted[i]%4 || sorted[i]>tiled_bytes || tiled_bytes-sorted[i]<4 || (i && sorted[i]==sorted[i-1]))return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    SIZE_T end=0;for(auto offset:sorted){if(offset>end)padding_.push_back({end,offset-end});end=SIZE_T(offset)+4;}
    if(end<tiled_bytes)padding_.push_back({end,tiled_bytes-end});
    const SIZE_T required=padded*3+SIZE_T(linear_bytes)+offsets.size_bytes()+padding_.capacity()*sizeof(padding_[0]);
    auto used=layout_bytes.load();
    do {if(required>layout_budget-used)return S_FALSE;}while(!layout_bytes.compare_exchange_weak(used,used+required));
    retained_bytes_=required;tiled_bytes_=tiled_bytes;width_=width;height_=height;footprint_=footprint;
    const auto create=[&](SIZE_T bytes,D3D12_HEAP_TYPE type,D3D12_RESOURCE_FLAGS flags,D3D12_RESOURCE_STATES state,auto& out) {
        D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=bytes;
        desc.Height=desc.DepthOrArraySize=desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;desc.Flags=flags;
        D3D12_HEAP_PROPERTIES heap{};heap.Type=type;
        return device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,state,nullptr,IID_PPV_ARGS(&out));
    };
    HRESULT hr=create(offsets.size_bytes(),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_GENERIC_READ,offsets_);
    if(SUCCEEDED(hr))hr=create(padded,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_GENERIC_READ,upload_);
    if(SUCCEEDED(hr))hr=create(padded,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COMMON,tiled_);
    if(SUCCEEDED(hr))hr=create(SIZE_T(linear_bytes),D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COMMON,linear_);
    if(SUCCEEDED(hr))hr=create(padded,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_COPY_DEST,readback_);
    if(FAILED(hr))return hr;
    void* mapped=nullptr;const D3D12_RANGE no_read{0,0};hr=offsets_->Map(0,&no_read,&mapped);
    if(FAILED(hr))return hr;std::memcpy(mapped,offsets.data(),offsets.size_bytes());offsets_->Unmap(0,nullptr);
    static constexpr char source[]=R"(
ByteAddressBuffer offsets:register(t0);ByteAddressBuffer input:register(t1);RWByteAddressBuffer output:register(u0);
cbuffer Layout:register(b0){uint width;uint pitch;uint count;uint into_tiled;};
[numthreads(64,1,1)] void main(uint3 id:SV_DispatchThreadID){
 if(id.x>=count)return;
 uint linear_address=(id.x/width)*pitch+(id.x%width)*4;uint tiled_address=offsets.Load(id.x*4);
 output.Store(into_tiled?tiled_address:linear_address,input.Load(into_tiled?linear_address:tiled_address));
})";
    struct Compiler{HMODULE value=LoadLibraryExW(L"d3dcompiler_47.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);~Compiler(){if(value)FreeLibrary(value);}}compiler;
    if(!compiler.value)return HRESULT_FROM_WIN32(GetLastError());
    const auto compile=reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(compiler.value,"D3DCompile"));
    if(!compile)return E_NOINTERFACE;
    Microsoft::WRL::ComPtr<ID3DBlob> code,errors,serialized;
    hr=compile(source,sizeof(source)-1,nullptr,nullptr,nullptr,"main","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&errors);
    if(FAILED(hr) && errors)std::fprintf(stderr,"xone-era: layout shader: %.2048s\n",static_cast<const char*>(errors->GetBufferPointer()));
    if(FAILED(hr))return hr;
    D3D12_ROOT_PARAMETER parameters[4]{};
    for(UINT i=0;i<2;++i){parameters[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;parameters[i].Descriptor.ShaderRegister=i;}
    parameters[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;parameters[2].Descriptor.ShaderRegister=0;
    parameters[3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;parameters[3].Constants={0,0,4};
    const D3D12_ROOT_SIGNATURE_DESC root{4,parameters,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};
    hr=D3D12SerializeRootSignature(&root,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,nullptr);
    if(SUCCEEDED(hr))hr=device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&root_));
    if(SUCCEEDED(hr)){D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};desc.pRootSignature=root_.Get();desc.CS={code->GetBufferPointer(),code->GetBufferSize()};hr=device->CreateComputePipelineState(&desc,IID_PPV_ARGS(&pipeline_));}
    return hr;
}catch(const std::bad_alloc&){return E_OUTOFMEMORY;}
HRESULT TextureLayoutTransfer::prepare(const void* canonical,bool full_overwrite) {
    if(!canonical || !pipeline_)return E_INVALIDARG;
    void* mapped=nullptr;const D3D12_RANGE no_read{0,0};const auto hr=upload_->Map(0,&no_read,&mapped);
    if(FAILED(hr))return hr;
    if(full_overwrite) {
        for(const auto& [offset,bytes]:padding_)std::memcpy(static_cast<unsigned char*>(mapped)+offset,static_cast<const unsigned char*>(canonical)+offset,bytes);
    } else std::memcpy(mapped,canonical,tiled_bytes_);
    if(tiled_bytes_%4)std::memset(static_cast<unsigned char*>(mapped)+tiled_bytes_,0,4-tiled_bytes_%4);
    upload_->Unmap(0,nullptr);return S_OK;
}
void TextureLayoutTransfer::dispatch(ID3D12GraphicsCommandList* list,bool into) const {
    list->SetPipelineState(pipeline_.Get());list->SetComputeRootSignature(root_.Get());
    list->SetComputeRootShaderResourceView(0,offsets_->GetGPUVirtualAddress());
    list->SetComputeRootShaderResourceView(1,(into?linear_:tiled_)->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(2,(into?tiled_:linear_)->GetGPUVirtualAddress());
    const UINT constants[]{width_,footprint_.Footprint.RowPitch,width_*height_,UINT(into)};
    list->SetComputeRoot32BitConstants(3,4,constants,0);list->Dispatch((width_*height_+63)/64,1,1);
}
void TextureLayoutTransfer::detile(ID3D12GraphicsCommandList* list,ID3D12Resource* texture) const {
    layout_transition(list,tiled_.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyBufferRegion(tiled_.Get(),0,upload_.Get(),0,(tiled_bytes_+3)&~SIZE_T(3));
    layout_transition(list,tiled_.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    layout_transition(list,linear_.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    dispatch(list,false);
    layout_transition(list,tiled_.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
    layout_transition(list,linear_.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
    D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=linear_.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;from.PlacedFootprint=footprint_;
    to.pResource=texture;to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
    layout_transition(list,linear_.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
}
void TextureLayoutTransfer::retile(ID3D12GraphicsCommandList* list,ID3D12Resource* texture) const {
    layout_transition(list,linear_.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=texture;from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    to.pResource=linear_.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=footprint_;
    list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
    layout_transition(list,linear_.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    layout_transition(list,tiled_.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyBufferRegion(tiled_.Get(),0,upload_.Get(),0,(tiled_bytes_+3)&~SIZE_T(3));
    layout_transition(list,tiled_.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    dispatch(list,true);
    layout_transition(list,linear_.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
    layout_transition(list,tiled_.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(readback_.Get(),0,tiled_.Get(),0,(tiled_bytes_+3)&~SIZE_T(3));
    layout_transition(list,tiled_.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
}
static HRESULT graphics_pipeline(ID3D12Device* device, DXGI_FORMAT output_format, UINT samples,
        const char* source, const D3D_SHADER_MACRO* macros,
        Microsoft::WRL::ComPtr<ID3D12RootSignature>& out_root, Microsoft::WRL::ComPtr<ID3D12PipelineState>& out_pipeline,
        bool depth_target = false, UINT srv_count = 1, UINT stencil_bit = UINT_MAX,
        DXGI_FORMAT depth_format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT, bool overlay = false) {
    if (!device) return E_INVALIDARG;
    struct CompilerModule {
        HMODULE handle;
        ~CompilerModule() { if (handle) FreeLibrary(handle); }
    } library{LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)};
    const HMODULE compiler = library.handle; // Keep blob vtables mapped through their final Release.
    if (!compiler) return HRESULT_FROM_WIN32(GetLastError());
    const auto compile = reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(compiler, "D3DCompile"));
    Microsoft::WRL::ComPtr<ID3DBlob> vertex, pixel, serialized, errors;
    HRESULT hr = compile ? compile(source, std::strlen(source), "era_display", macros, nullptr, "vs_main", "vs_5_0",
        D3DCOMPILE_ENABLE_STRICTNESS, 0, &vertex, &errors) : E_NOINTERFACE;
    if (SUCCEEDED(hr)) hr = compile(source, std::strlen(source), "era_display", macros, nullptr, "ps_main", "ps_5_0",
        D3DCOMPILE_ENABLE_STRICTNESS, 0, &pixel, &errors);
    if (FAILED(hr) && errors) std::fprintf(stderr, "xone-era: display shader: %.4096s\n", static_cast<const char*>(errors->GetBufferPointer()));
    if (FAILED(hr)) return hr;
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, srv_count, 0, 0, 0};
    D3D12_ROOT_PARAMETER parameters[2]{};
    parameters[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameters[0].DescriptorTable = {1, &range}; parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    parameters[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    parameters[1].Constants = {0, 0, 1}; parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler{};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.MaxAnisotropy = 1; sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    sampler.MaxLOD = D3D12_FLOAT32_MAX; sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    const D3D12_ROOT_SIGNATURE_DESC signature{2, parameters, 1, &sampler, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    hr = D3D12SerializeRootSignature(&signature, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, nullptr);
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    if (SUCCEEDED(hr)) hr = device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&root));
    if (FAILED(hr)) return hr;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC desc{};
    desc.pRootSignature = root.Get();
    desc.VS = {vertex->GetBufferPointer(), vertex->GetBufferSize()};
    desc.PS = {pixel->GetBufferPointer(), pixel->GetBufferSize()};
    auto& blend = desc.BlendState.RenderTarget[0];
    blend.SrcBlend = blend.SrcBlendAlpha = D3D12_BLEND_ONE;
    blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_ZERO;
    if (overlay) {
        blend.BlendEnable = TRUE;
        blend.DestBlend = blend.DestBlendAlpha = D3D12_BLEND_INV_SRC_ALPHA;
    }
    blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    blend.LogicOp = D3D12_LOGIC_OP_NOOP; blend.RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    desc.SampleMask = UINT_MAX;
    desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    desc.RasterizerState.DepthClipEnable = TRUE;
    desc.RasterizerState.MultisampleEnable = samples > 1;
    desc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ZERO;
    desc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    desc.NumRenderTargets = 1; desc.RTVFormats[0] = output_format; desc.SampleDesc.Count = samples;
    if (depth_target) {
        desc.NumRenderTargets = 0; desc.RTVFormats[0] = DXGI_FORMAT_UNKNOWN;
        desc.DSVFormat = depth_format;
        auto& state = desc.DepthStencilState;
        state.DepthEnable = TRUE; state.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
        state.StencilEnable = stencil_bit<8; state.StencilReadMask = 255;
        state.StencilWriteMask = stencil_bit<8 ? static_cast<UINT8>(1u<<stencil_bit) : 255;
        if (stencil_bit<8) { state.DepthEnable=FALSE; state.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ZERO; }
        state.FrontFace = state.BackFace = {D3D12_STENCIL_OP_REPLACE,D3D12_STENCIL_OP_REPLACE,D3D12_STENCIL_OP_REPLACE,D3D12_COMPARISON_FUNC_ALWAYS};
    }
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline));
    if (SUCCEEDED(hr)) { out_root = std::move(root); out_pipeline = std::move(pipeline); }
    return hr;
}

static HRESULT fullscreen(ID3D12GraphicsCommandList* list, ID3D12RootSignature* root, ID3D12PipelineState* pipeline,
        ID3D12DescriptorHeap* source_heap,
        D3D12_GPU_DESCRIPTOR_HANDLE source, D3D12_CPU_DESCRIPTOR_HANDLE target,
        UINT width, UINT height, UINT parameter, bool depth_target = false) {
    if (!list || !source_heap || !source.ptr || !target.ptr || !width || !height || width > 16384 || height > 16384) return E_INVALIDARG;
    if (!root || !pipeline) return E_UNEXPECTED;
    if (list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) return DXGI_ERROR_UNSUPPORTED;
    const D3D12_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
    const D3D12_RECT scissor{0, 0, LONG(width), LONG(height)};
    list->SetPipelineState(pipeline); list->SetGraphicsRootSignature(root);
    list->SetDescriptorHeaps(1, &source_heap); list->SetGraphicsRootDescriptorTable(0, source);
    list->SetGraphicsRoot32BitConstant(1, parameter, 0);
    list->RSSetViewports(1, &viewport); list->RSSetScissorRects(1, &scissor);
    list->OMSetRenderTargets(depth_target ? 0 : 1, depth_target ? nullptr : &target, FALSE, depth_target ? &target : nullptr);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(3, 1, 0, 0);
    return S_OK;
}

HRESULT DisplayTransfer::initialize(ID3D12Device* device, DXGI_FORMAT output_format, bool overlay) {
    // Independent standard sRGB decoding and BT.709 encoding, selected by Xbox
    // flag 0x200000. BT.709-6 table 1: https://www.itu.int/rec/R-REC-BT.709/en
    static constexpr char source[] = R"(
Texture2D<float4> image : register(t0);
SamplerState linear_clamp : register(s0);
cbuffer Transfer : register(b0) { uint srgb_to_709; };
struct Vertex { float4 position : SV_Position; float2 uv : TEXCOORD0; };
Vertex vs_main(uint id : SV_VertexID) {
    Vertex v;
    v.uv = float2((id << 1) & 2, id & 2);
    v.position = float4(v.uv.x * 2 - 1, 1 - v.uv.y * 2, 0, 1);
    return v;
}
float4 ps_main(Vertex v) : SV_Target {
    float4 pixel = image.SampleLevel(linear_clamp, v.uv, 0);
    float3 rgb = saturate(pixel.rgb);
    if (srgb_to_709) {
        float3 light = rgb <= 0.04045 ? rgb / 12.92 : pow((rgb + 0.055) / 1.055, 2.4);
        rgb = light < 0.018 ? light * 4.5 : 1.099 * pow(light, 0.45) - 0.099;
    }
    return float4(rgb, pixel.a);
})";
    return graphics_pipeline(device, output_format, 1, source, nullptr, root_, pipeline_, false, 1, UINT_MAX,
                             DXGI_FORMAT_D32_FLOAT_S8X24_UINT, overlay);
}
HRESULT DisplayTransfer::record(ID3D12GraphicsCommandList* list, ID3D12DescriptorHeap* heap,
        D3D12_GPU_DESCRIPTOR_HANDLE source, D3D12_CPU_DESCRIPTOR_HANDLE target,
        UINT width, UINT height, bool srgb_to_709) const {
    return fullscreen(list, root_.Get(), pipeline_.Get(), heap, source, target, width, height, srgb_to_709 ? 1 : 0);
}
HRESULT GuideComposition::record(ID3D12Device* device, ID3D12GraphicsCommandList* list, ID3D12Resource* output) {
    active_ = false;
    UINT width = 0, height = 0;
    if (!session_guide_frame(pixels_, width, height)) return S_OK;
    const auto out = output->GetDesc();
    HRESULT hr = S_OK;
    if (format_ != out.Format) {
        hr = transfer_.initialize(device, out.Format, true);
        if (FAILED(hr)) return hr;
        format_ = out.Format;
    }
    if (!image_ || width != width_ || height != height_) {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width; desc.Height = height; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; desc.SampleDesc.Count = 1;
        D3D12_HEAP_PROPERTIES heap{}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
        image_.Reset(); upload_.Reset();
        hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&image_));
        UINT64 bytes = 0; device->GetCopyableFootprints(&desc, 0, 1, 0, nullptr, nullptr, nullptr, &bytes);
        desc = {}; desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width = bytes;
        desc.Height = desc.DepthOrArraySize = desc.MipLevels = 1; desc.SampleDesc.Count = 1;
        desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; heap.Type = D3D12_HEAP_TYPE_UPLOAD;
        if (SUCCEEDED(hr)) hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload_));
        const D3D12_DESCRIPTOR_HEAP_DESC srv{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
        const D3D12_DESCRIPTOR_HEAP_DESC rtv{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        if (SUCCEEDED(hr) && !source_) hr = device->CreateDescriptorHeap(&srv, IID_PPV_ARGS(&source_));
        if (SUCCEEDED(hr) && !target_) hr = device->CreateDescriptorHeap(&rtv, IID_PPV_ARGS(&target_));
        if (FAILED(hr)) { image_.Reset(); return hr; }
        D3D12_SHADER_RESOURCE_VIEW_DESC view{}; view.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D; view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        view.Texture2D.MipLevels = 1;
        device->CreateShaderResourceView(image_.Get(), &view, source_->GetCPUDescriptorHandleForHeapStart());
        width_ = width; height_ = height;
    }
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    const auto desc = image_->GetDesc();
    device->GetCopyableFootprints(&desc, 0, 1, 0, &footprint, nullptr, nullptr, nullptr);
    void* mapped = nullptr; const D3D12_RANGE no_read{0, 0};
    hr = upload_->Map(0, &no_read, &mapped);
    if (FAILED(hr)) return hr;
    for (UINT y = 0; y < height; ++y)
        std::memcpy(static_cast<char*>(mapped) + footprint.Offset + SIZE_T(y) * footprint.Footprint.RowPitch,
                    pixels_.data() + SIZE_T(y) * width, SIZE_T(width) * 4);
    upload_->Unmap(0, nullptr);
    D3D12_RESOURCE_BARRIER barrier{}; barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition = {image_.Get(), D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,
                          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST};
    list->ResourceBarrier(1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION from{}; from.pResource = upload_.Get(); from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    from.PlacedFootprint = footprint;
    D3D12_TEXTURE_COPY_LOCATION to{}; to.pResource = image_.Get(); to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter); list->ResourceBarrier(1, &barrier);
    device->CreateRenderTargetView(output, nullptr, target_->GetCPUDescriptorHandleForHeapStart());
    hr = transfer_.record(list, source_.Get(), source_->GetGPUDescriptorHandleForHeapStart(),
        target_->GetCPUDescriptorHandleForHeapStart(), UINT(out.Width), out.Height, false);
    active_ = SUCCEEDED(hr);
    return hr;
}

HRESULT SampleTransfer::initialize(ID3D12Device* device, DXGI_FORMAT format, UINT samples) {
    if (samples < 2 || samples > 8 || (samples & (samples - 1))) return E_INVALIDARG;
    const bool depth_stencil = format==DXGI_FORMAT_R32G8X24_TYPELESS;
    const bool depth_only = format==DXGI_FORMAT_R32_TYPELESS;
    const bool integer_color = format==DXGI_FORMAT_R16G16B16A16_TYPELESS;
    const auto transport_format=integer_color ? DXGI_FORMAT_R16G16B16A16_UINT : format;
    static constexpr char source[] = R"(
cbuffer Transfer : register(b0) { uint sample_index; };
struct Vertex { float4 position : SV_Position; };
Vertex vs_main(uint id : SV_VertexID) {
    Vertex v;
    float2 uv = float2((id << 1) & 2, id & 2);
    v.position = float4(uv.x * 2 - 1, 1 - uv.y * 2, 0, 1);
    return v;
}
#ifdef INTO_MSAA
#ifdef DEPTH_ONLY
Texture2DArray<uint> image : register(t0);
float ps_main(Vertex v, uint sample_id : SV_SampleIndex) : SV_Depth {
    return asfloat(image.Load(int4(int2(v.position.xy), sample_id, 0)));
}
#else
#ifdef DEPTH_STENCIL
Texture2DArray<uint2> image : register(t0);
#ifdef STENCIL_BIT
void ps_main(Vertex v, uint sample_id : SV_SampleIndex) {
    uint2 bits = image.Load(int4(int2(v.position.xy), sample_id, 0));
    if (!(bits.y & (1u << sample_index))) discard;
}
#else
float ps_main(Vertex v, uint sample_id : SV_SampleIndex) : SV_Depth {
    return asfloat(image.Load(int4(int2(v.position.xy), sample_id, 0)).x);
}
#endif
#else
#ifdef INTEGER_COLOR
Texture2DArray<uint4> image : register(t0);
uint4 ps_main(Vertex v, uint sample_id : SV_SampleIndex) : SV_Target {
    return image.Load(int4(int2(v.position.xy), sample_id, 0));
}
#else
Texture2DArray<float4> image : register(t0);
float4 ps_main(Vertex v, uint sample_id : SV_SampleIndex) : SV_Target {
    return image.Load(int4(int2(v.position.xy), sample_id, 0));
}
#endif
#endif
#endif
#else
#ifdef DEPTH_ONLY
Texture2DMS<float> image : register(t0);
uint ps_main(Vertex v) : SV_Target { return asuint(image.Load(int2(v.position.xy), sample_index)); }
#else
#ifdef DEPTH_STENCIL
Texture2DMS<float> depth_image : register(t0);
Texture2DMS<uint2> stencil_image : register(t1);
uint2 ps_main(Vertex v) : SV_Target {
    int2 xy = int2(v.position.xy);
    return uint2(asuint(depth_image.Load(xy, sample_index)),stencil_image.Load(xy, sample_index).y);
}
#else
#ifdef INTEGER_COLOR
Texture2DMS<uint4> image : register(t0);
uint4 ps_main(Vertex v) : SV_Target {
    return image.Load(int2(v.position.xy), sample_index);
}
#else
Texture2DMS<float4> image : register(t0);
float4 ps_main(Vertex v) : SV_Target {
    return image.Load(int2(v.position.xy), sample_index);
}
#endif
#endif
#endif
#endif
)";
    std::array<Microsoft::WRL::ComPtr<ID3D12RootSignature>, 2> roots;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 2> pipelines;
    const char* transport_macro=depth_stencil ? "DEPTH_STENCIL" : depth_only ? "DEPTH_ONLY" : integer_color ? "INTEGER_COLOR" : nullptr;
    const D3D_SHADER_MACRO into[]{{"INTO_MSAA", "1"}, {transport_macro,"1"}, {nullptr,nullptr}};
    const D3D_SHADER_MACRO out[]{{transport_macro,"1"}, {nullptr,nullptr}};
    HRESULT hr = graphics_pipeline(device, transport_format, samples, source, into, roots[0], pipelines[0],depth_stencil || depth_only,
        1,UINT_MAX,depth_only ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_D32_FLOAT_S8X24_UINT);
    if (SUCCEEDED(hr)) hr = graphics_pipeline(device, depth_stencil ? DXGI_FORMAT_R32G32_UINT : depth_only ? DXGI_FORMAT_R32_UINT : transport_format, 1, source, out,
        roots[1], pipelines[1],false,depth_stencil ? 2 : 1);
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>,8> stencil;
    if (depth_stencil) for (UINT bit=0;bit<8 && SUCCEEDED(hr);++bit) {
        const D3D_SHADER_MACRO masks[]{{"INTO_MSAA","1"},{"DEPTH_STENCIL","1"},{"STENCIL_BIT","1"},{nullptr,nullptr}};
        Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
        hr=graphics_pipeline(device,format,samples,source,masks,root,stencil[bit],true,1,bit);
    }
    if (SUCCEEDED(hr)) { roots_ = std::move(roots); pipelines_ = std::move(pipelines); stencil_pipelines_=std::move(stencil); samples_ = samples; depth_stencil_=depth_stencil; depth_target_=depth_stencil || depth_only; }
    return hr;
}
HRESULT SampleTransfer::record(ID3D12GraphicsCommandList* list, ID3D12DescriptorHeap* heap,
        D3D12_GPU_DESCRIPTOR_HANDLE source, D3D12_CPU_DESCRIPTOR_HANDLE target,
        UINT width, UINT height, UINT sample, bool into_msaa) const {
    if (sample >= samples_) return E_INVALIDARG;
    const UINT index = into_msaa ? 0 : 1;
    HRESULT hr=fullscreen(list, roots_[index].Get(), pipelines_[index].Get(), heap, source, target, width, height, sample,depth_target_ && into_msaa);
    if (SUCCEEDED(hr) && depth_stencil_ && into_msaa) {
        // Eight write masks reconstruct every stencil bit on hosts without the
        // optional PS stencil export. Sample-frequency shading preserves MSAA.
        list->ClearDepthStencilView(target,D3D12_CLEAR_FLAG_STENCIL,0,0,0,nullptr);
        list->OMSetStencilRef(255);
        for (UINT bit=0;bit<8 && SUCCEEDED(hr);++bit)
            hr=fullscreen(list,roots_[0].Get(),stencil_pipelines_[bit].Get(),heap,source,target,width,height,bit,true);
    }
    return hr;
}
}
