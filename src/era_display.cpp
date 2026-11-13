// SPDX-License-Identifier: BSD-3-Clause
#include "era_display.h"
#include <d3dcompiler.h>
#include <cstring>
#include <climits>
#include <utility>
#include <cstdio>

namespace xboxone::era {
static HRESULT graphics_pipeline(ID3D12Device* device, DXGI_FORMAT output_format, UINT samples,
        const char* source, const D3D_SHADER_MACRO* macros,
        Microsoft::WRL::ComPtr<ID3D12RootSignature>& out_root, Microsoft::WRL::ComPtr<ID3D12PipelineState>& out_pipeline) {
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
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0};
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
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline;
    hr = device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pipeline));
    if (SUCCEEDED(hr)) { out_root = std::move(root); out_pipeline = std::move(pipeline); }
    return hr;
}

static HRESULT fullscreen(ID3D12GraphicsCommandList* list, ID3D12RootSignature* root, ID3D12PipelineState* pipeline,
        ID3D12DescriptorHeap* source_heap,
        D3D12_GPU_DESCRIPTOR_HANDLE source, D3D12_CPU_DESCRIPTOR_HANDLE target,
        UINT width, UINT height, UINT parameter) {
    if (!list || !source_heap || !source.ptr || !target.ptr || !width || !height || width > 16384 || height > 16384) return E_INVALIDARG;
    if (!root || !pipeline) return E_UNEXPECTED;
    if (list->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) return DXGI_ERROR_UNSUPPORTED;
    const D3D12_VIEWPORT viewport{0, 0, float(width), float(height), 0, 1};
    const D3D12_RECT scissor{0, 0, LONG(width), LONG(height)};
    list->SetPipelineState(pipeline); list->SetGraphicsRootSignature(root);
    list->SetDescriptorHeaps(1, &source_heap); list->SetGraphicsRootDescriptorTable(0, source);
    list->SetGraphicsRoot32BitConstant(1, parameter, 0);
    list->RSSetViewports(1, &viewport); list->RSSetScissorRects(1, &scissor);
    list->OMSetRenderTargets(1, &target, FALSE, nullptr);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    list->DrawInstanced(3, 1, 0, 0);
    return S_OK;
}

HRESULT DisplayTransfer::initialize(ID3D12Device* device, DXGI_FORMAT output_format) {
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
    return graphics_pipeline(device, output_format, 1, source, nullptr, root_, pipeline_);
}
HRESULT DisplayTransfer::record(ID3D12GraphicsCommandList* list, ID3D12DescriptorHeap* heap,
        D3D12_GPU_DESCRIPTOR_HANDLE source, D3D12_CPU_DESCRIPTOR_HANDLE target,
        UINT width, UINT height, bool srgb_to_709) const {
    return fullscreen(list, root_.Get(), pipeline_.Get(), heap, source, target, width, height, srgb_to_709 ? 1 : 0);
}
HRESULT SampleTransfer::initialize(ID3D12Device* device, DXGI_FORMAT format, UINT samples) {
    if (samples < 2 || samples > 8 || (samples & (samples - 1))) return E_INVALIDARG;
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
Texture2DArray<float4> image : register(t0);
float4 ps_main(Vertex v, uint sample_id : SV_SampleIndex) : SV_Target {
    return image.Load(int4(int2(v.position.xy), sample_id, 0));
}
#else
Texture2DMS<float4> image : register(t0);
float4 ps_main(Vertex v) : SV_Target {
    return image.Load(int2(v.position.xy), sample_index);
}
#endif
)";
    std::array<Microsoft::WRL::ComPtr<ID3D12RootSignature>, 2> roots;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 2> pipelines;
    const D3D_SHADER_MACRO macros[]{{"INTO_MSAA", "1"}, {nullptr, nullptr}};
    HRESULT hr = graphics_pipeline(device, format, samples, source, macros, roots[0], pipelines[0]);
    if (SUCCEEDED(hr)) hr = graphics_pipeline(device, format, 1, source, nullptr, roots[1], pipelines[1]);
    if (SUCCEEDED(hr)) { roots_ = std::move(roots); pipelines_ = std::move(pipelines); samples_ = samples; }
    return hr;
}
HRESULT SampleTransfer::record(ID3D12GraphicsCommandList* list, ID3D12DescriptorHeap* heap,
        D3D12_GPU_DESCRIPTOR_HANDLE source, D3D12_CPU_DESCRIPTOR_HANDLE target,
        UINT width, UINT height, UINT sample, bool into_msaa) const {
    if (sample >= samples_) return E_INVALIDARG;
    const UINT index = into_msaa ? 0 : 1;
    return fullscreen(list, roots_[index].Get(), pipelines_[index].Get(), heap, source, target, width, height, sample);
}
}
