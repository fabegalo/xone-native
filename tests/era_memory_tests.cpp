// SPDX-License-Identifier: BSD-3-Clause
#include "xboxone_native/era_loader.h"
#include "../src/era_memory.h"
#include "../src/era_display.h"
#include "../src/era_shader_translation.h"
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <d3d11.h>
#include <d3d12sdklayers.h>
#include <d3d11shader.h>
#include <wrl/client.h>
#include <cstring>
#include <iostream>
#include <algorithm>
#include <vector>
#include <filesystem>
#include <cmath>
#include <thread>
#include <future>

using Microsoft::WRL::ComPtr;
using namespace xboxone::era;
namespace {
int failures = 0;
void check(bool ok, const char* text) {
    if (!ok) { ++failures; std::cerr << "FAIL: " << text << '\n'; }
}
void barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
             D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = {resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after};
    list->ResourceBarrier(1, &b);
}
bool shader_arithmetic(ID3D12Device* device,ID3D12CommandQueue* queue,ID3D12CommandAllocator* allocator,
                       ID3D12GraphicsCommandList* list,ID3D12Resource* readback) {
    const HMODULE compiler=LoadLibraryExW(L"d3dcompiler_47.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    if(!compiler)return false;
    const auto compile=reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(compiler,"D3DCompile"));
    const auto reflect=reinterpret_cast<decltype(&D3DReflect)>(GetProcAddress(compiler,"D3DReflect"));
    constexpr char fixture[]="cbuffer Inputs:register(b0){float4 a;float4 b;} RWByteAddressBuffer output:register(u0);"
        "Texture2D<float4> empty0:register(t0);Texture2D<float4> empty1:register(t1);SamplerState border:register(s0);"
        "[numthreads(1,1,1)] void main(){output.Store4(0,asuint(a*b));output.Store4(16,asuint(a/b));"
        "output.Store4(32,asuint(mad(a,b,3.0)));output.Store(48,asuint(dot(a.xy,b.xy)));"
        "output.Store(52,asuint(dot(a.xyw,b.xyw)));output.Store(56,asuint(dot(a.xyww,b.xyww)));"
        "output.Store4(64,asuint(empty0.SampleLevel(border,float2(1,1),0)));"
        "output.Store4(80,asuint(empty1.SampleLevel(border,float2(1,1),0)));}";
    ComPtr<ID3DBlob> original,errors,signature;
    HRESULT hr=compile?compile(fixture,sizeof fixture-1,nullptr,nullptr,nullptr,"main","cs_5_0",D3DCOMPILE_SKIP_OPTIMIZATION,0,&original,&errors):E_NOINTERFACE;
    std::vector<std::uint8_t> translated;
    if(SUCCEEDED(hr)) {
        const auto* begin=static_cast<const std::uint8_t*>(original->GetBufferPointer());
        translated.assign(begin,begin+original->GetBufferSize());
        auto unchanged=translated;
        check(translate_reference_shader(unchanged,"Microsoft (R) HLSL Shader Compiler 9.30.9821.0")==S_FALSE && unchanged==translated,
              "the reference's modern compiler exception preserves the complete original shader");
        auto malformed=translated;malformed.resize(31);const auto preimage=malformed;
        check(translate_reference_shader(malformed,nullptr)==E_INVALIDARG && malformed==preimage,
              "malformed translation input is rejected without changing caller bytes");
        hr=translate_reference_shader(translated,"Microsoft (R) HLSL Shader Compiler 9.30.9200.16384");
        check(hr==S_OK,"owned arithmetic fixture exercises the reference shader rewrite");
        const auto set_part=reinterpret_cast<decltype(&D3DSetBlobPart)>(GetProcAddress(compiler,"D3DSetBlobPart"));
        const auto get_part=reinterpret_cast<decltype(&D3DGetBlobPart)>(GetProcAddress(compiler,"D3DGetBlobPart"));
        ComPtr<ID3DBlob> tagged,retained,verified;constexpr BYTE metadata[]{7,8,9};
        HRESULT tagged_hr=set_part?set_part(original->GetBufferPointer(),original->GetBufferSize(),D3D_BLOB_PRIVATE_DATA,0,metadata,sizeof metadata,&tagged):E_NOINTERFACE;
        if(SUCCEEDED(tagged_hr)) {
            const auto* bytes=static_cast<const BYTE*>(tagged->GetBufferPointer());std::vector<BYTE> tagged_code(bytes,bytes+tagged->GetBufferSize());
            tagged_hr=translate_reference_shader(tagged_code,nullptr);
            if(SUCCEEDED(tagged_hr))tagged_hr=get_part?get_part(tagged_code.data(),tagged_code.size(),D3D_BLOB_PRIVATE_DATA,0,&retained):E_NOINTERFACE;
            if(SUCCEEDED(tagged_hr))tagged_hr=set_part(tagged_code.data(),tagged_code.size(),D3D_BLOB_PRIVATE_DATA,0,metadata,sizeof metadata,&verified);
        }
        check(SUCCEEDED(tagged_hr)&&retained&&retained->GetBufferSize()==sizeof metadata&&
              std::memcmp(retained->GetBufferPointer(),metadata,sizeof metadata)==0,
              "the native compiler verifies rewritten checksums and retains non-word-sized private metadata");
    }
    ComPtr<ID3D11ShaderReflection> reflection;
    if(SUCCEEDED(hr))hr=reflect?reflect(translated.data(),translated.size(),IID_PPV_ARGS(&reflection)):E_NOINTERFACE;
    D3D11_SHADER_DESC shader_desc{};
    if(SUCCEEDED(hr))hr=reflection->GetDesc(&shader_desc);
    check(SUCCEEDED(hr)&&shader_desc.BoundResources==5,"rewritten DXBC retains real resource reflection");
    D3D12_ROOT_PARAMETER parameters[3]{};
    parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;parameters[0].Constants={0,0,8};
    parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;parameters[1].Descriptor={0,0};
    D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,2,0,0,0};
    parameters[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;parameters[2].DescriptorTable={1,&range};
    D3D12_STATIC_SAMPLER_DESC border{};border.Filter=D3D12_FILTER_MIN_MAG_MIP_POINT;
    border.AddressU=border.AddressV=border.AddressW=D3D12_TEXTURE_ADDRESS_MODE_BORDER;
    border.BorderColor=D3D12_STATIC_BORDER_COLOR_OPAQUE_WHITE;border.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;border.MaxLOD=FLT_MAX;
    const D3D12_ROOT_SIGNATURE_DESC root_desc{3,parameters,1,&border,D3D12_ROOT_SIGNATURE_FLAG_NONE};
    if(SUCCEEDED(hr))hr=D3D12SerializeRootSignature(&root_desc,D3D_ROOT_SIGNATURE_VERSION_1,&signature,&errors);
    ComPtr<ID3D12RootSignature> root;
    if(SUCCEEDED(hr))hr=device->CreateRootSignature(0,signature->GetBufferPointer(),signature->GetBufferSize(),IID_PPV_ARGS(&root));
    ComPtr<ID3D12Resource> output;
    ComPtr<ID3D12DescriptorHeap> descriptors;
    const D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,2,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
    if(SUCCEEDED(hr))hr=device->CreateDescriptorHeap(&heap_desc,IID_PPV_ARGS(&descriptors));
    if(SUCCEEDED(hr))for(UINT mip=0;mip<2;++mip) {
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};view.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
        view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;view.Texture2D.MipLevels=mip;
        auto handle=descriptors->GetCPUDescriptorHandleForHeapStart();handle.ptr+=mip*device->GetDescriptorHandleIncrementSize(heap_desc.Type);
        device->CreateShaderResourceView(nullptr,&view,handle);
    }
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=256;desc.Height=1;
    desc.DepthOrArraySize=desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if(SUCCEEDED(hr))hr=device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&output));
    ComPtr<ID3D12Fence> fence;
    if(SUCCEEDED(hr))hr=device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence));
    if(SUCCEEDED(hr))hr=list->Close();
    const UINT inputs[]{0,0,0x40000000,0x40000000,0x7f800000,0,0x7fc01234,0x40400000};
    for(UINT pass=0;SUCCEEDED(hr)&&pass<2;++pass) {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};pipeline.pRootSignature=root.Get();
        pipeline.CS=pass?D3D12_SHADER_BYTECODE{translated.data(),translated.size()}:D3D12_SHADER_BYTECODE{original->GetBufferPointer(),original->GetBufferSize()};
        ComPtr<ID3D12PipelineState> state;
        hr=device->CreateComputePipelineState(&pipeline,IID_PPV_ARGS(&state));
        if(SUCCEEDED(hr))hr=allocator->Reset();if(SUCCEEDED(hr))hr=list->Reset(allocator,state.Get());
        if(FAILED(hr))break;
        if(pass)barrier(list,output.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->SetComputeRootSignature(root.Get());list->SetComputeRoot32BitConstants(0,8,inputs,0);
        ID3D12DescriptorHeap* heaps[]{descriptors.Get()};list->SetDescriptorHeaps(1,heaps);
        list->SetComputeRootDescriptorTable(2,descriptors->GetGPUDescriptorHandleForHeapStart());
        list->SetComputeRootUnorderedAccessView(1,output->GetGPUVirtualAddress());list->Dispatch(1,1,1);
        barrier(list,output.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyBufferRegion(readback,1024+pass*128,output.Get(),0,96);hr=list->Close();
        if(FAILED(hr))break;
        ID3D12CommandList* lists[]{list};queue->ExecuteCommandLists(1,lists);
        hr=queue->Signal(fence.Get(),pass+1);const HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        if(!event){hr=HRESULT_FROM_WIN32(GetLastError());break;}
        if(SUCCEEDED(hr))hr=fence->SetEventOnCompletion(pass+1,event);
        if(SUCCEEDED(hr)&&WaitForSingleObject(event,3000)!=WAIT_OBJECT_0)hr=HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        CloseHandle(event);
    }
    if(SUCCEEDED(hr)) {
        void* data=nullptr;hr=readback->Map(0,nullptr,&data);
        if(SUCCEEDED(hr)) {
            const auto* raw=reinterpret_cast<const float*>(static_cast<const BYTE*>(data)+1024);const auto* actual=raw+32;
            check(std::isnan(raw[0])&&std::isnan(raw[5])&&std::isnan(raw[12]),"untranslated native arithmetic is a reproducible negative control");
            check(actual[0]==0&&actual[1]==0&&std::isnan(actual[2])&&actual[3]==6,
                  "legacy multiplication zeroes zero products and preserves other NaNs and finite products");
            check(actual[4]==0&&actual[5]==0&&std::isnan(actual[6])&&std::abs(actual[7]-2.f/3)<0.000001f,
                  "legacy division handles zero over zero without hiding unrelated NaNs");
            check(actual[8]==3&&actual[9]==3&&std::isnan(actual[10])&&actual[11]==9,
                  "legacy MAD preserves its addend after a zero product");
            check(actual[12]==0&&actual[13]==6&&actual[14]==12,"legacy DP2/DP3/DP4 reduce the same zero-preserving products");
            check(actual[19]==0&&actual[23]==0,"native null texture reads retain zero alpha at the reference and host mip counts");
            readback->Unmap(0,nullptr);
        }
    }
    if(SUCCEEDED(hr))hr=allocator->Reset();if(SUCCEEDED(hr))hr=list->Reset(allocator,nullptr);
    if(FAILED(hr))std::cerr<<"arithmetic fixture HRESULT 0x"<<std::hex<<hr<<std::dec<<'\n';
    reflection.Reset();original.Reset();errors.Reset();signature.Reset();FreeLibrary(compiler);return SUCCEEDED(hr);
}
bool shader_vertex_ids(ID3D12Device* device,ID3D12CommandQueue* queue,ID3D12CommandAllocator* allocator,
                       ID3D12GraphicsCommandList* list,ID3D12Resource* readback,ID3D12Resource* upload) {
    struct Compiler {HMODULE value=LoadLibraryExW(L"d3dcompiler_47.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);~Compiler(){if(value)FreeLibrary(value);}} compiler;
    const auto compile=compiler.value ? reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(compiler.value,"D3DCompile")):nullptr;
    if(!compile)return false;
    constexpr char source[]="struct V{float4 p:SV_Position;nointerpolation float r:COLOR;};"
        "V vs(float r:COLOR,uint v:SV_VertexID){uint n=(v+1)%3;float2 uv=float2((n<<1)&2,n&2);V o;o.p=float4(uv*float2(2,-2)+float2(-1,1),0,1);"
        "if(CHECK_ID && (v<5||v>7))o.p.x+=100;o.r=r;return o;} float ps(V v):SV_Target{return v.r;}";
    ComPtr<ID3DBlob> signature,ps;const D3D12_ROOT_SIGNATURE_DESC root_desc{0,nullptr,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
    HRESULT hr=D3D12SerializeRootSignature(&root_desc,D3D_ROOT_SIGNATURE_VERSION_1,&signature,nullptr);
    ComPtr<ID3D12RootSignature> root;ComPtr<ID3D12Resource> target;ComPtr<ID3D12DescriptorHeap> views;ComPtr<ID3D12Fence> fence;
    if(SUCCEEDED(hr))hr=device->CreateRootSignature(0,signature->GetBufferPointer(),signature->GetBufferSize(),IID_PPV_ARGS(&root));
    D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=32;desc.Height=16;desc.DepthOrArraySize=desc.MipLevels=1;
    desc.Format=DXGI_FORMAT_R32_FLOAT;desc.SampleDesc.Count=1;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    if(SUCCEEDED(hr))hr=device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&target));
    const D3D12_DESCRIPTOR_HEAP_DESC view_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV,1,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};
    if(SUCCEEDED(hr))hr=device->CreateDescriptorHeap(&view_desc,IID_PPV_ARGS(&views));
    if(SUCCEEDED(hr))device->CreateRenderTargetView(target.Get(),nullptr,views->GetCPUDescriptorHandleForHeapStart());
    if(SUCCEEDED(hr))hr=device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence));
    const FLOAT colors[]{1,1,1,.125f,.25f,.5f,.75f,1};void* mapped=nullptr;const D3D12_RANGE no_read{0,0};
    if(SUCCEEDED(hr))hr=upload->Map(0,&no_read,&mapped);
    if(SUCCEEDED(hr)){std::memcpy(mapped,colors,sizeof colors);upload->Unmap(0,nullptr);}
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};device->GetCopyableFootprints(&desc,0,1,0,&footprint,nullptr,nullptr,nullptr);
    if(SUCCEEDED(hr))hr=list->Close();UINT64 submission=0;
    for(UINT clipped=0;SUCCEEDED(hr)&&clipped<2;++clipped) {
        const D3D_SHADER_MACRO defines[]{{"CHECK_ID",clipped ? "1":"0"},{nullptr,nullptr}};
        ComPtr<ID3DBlob> vs;
        hr=compile(source,sizeof source-1,nullptr,defines,nullptr,"vs","vs_4_0",0,0,&vs,nullptr);
        if(SUCCEEDED(hr))hr=compile(source,sizeof source-1,nullptr,defines,nullptr,"ps","ps_4_0",0,0,&ps,nullptr);
        std::vector<BYTE> rewritten;
        if(SUCCEEDED(hr)){const auto* data=static_cast<const BYTE*>(vs->GetBufferPointer());rewritten.assign(data,data+vs->GetBufferSize());hr=translate_reference_shader(rewritten,nullptr);}
        std::array<float,512> expected{};
        for(UINT translated=0;SUCCEEDED(hr)&&translated<2;++translated) {
            D3D12_GRAPHICS_PIPELINE_STATE_DESC pipeline{};pipeline.pRootSignature=root.Get();
            pipeline.VS=translated ? D3D12_SHADER_BYTECODE{rewritten.data(),rewritten.size()}:D3D12_SHADER_BYTECODE{vs->GetBufferPointer(),vs->GetBufferSize()};
            pipeline.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
            const D3D12_INPUT_ELEMENT_DESC input{"COLOR",0,DXGI_FORMAT_R32_FLOAT,0,0,D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA,1};pipeline.InputLayout={&input,1};
            pipeline.SampleMask=UINT_MAX;pipeline.SampleDesc.Count=1;pipeline.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
            pipeline.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;pipeline.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;pipeline.RasterizerState.DepthClipEnable=TRUE;
            pipeline.BlendState.RenderTarget[0]={FALSE,FALSE,D3D12_BLEND_ONE,D3D12_BLEND_ZERO,D3D12_BLEND_OP_ADD,D3D12_BLEND_ONE,D3D12_BLEND_ZERO,D3D12_BLEND_OP_ADD,D3D12_LOGIC_OP_NOOP,D3D12_COLOR_WRITE_ENABLE_ALL};
            pipeline.NumRenderTargets=1;pipeline.RTVFormats[0]=desc.Format;
            ComPtr<ID3D12PipelineState> state;hr=device->CreateGraphicsPipelineState(&pipeline,IID_PPV_ARGS(&state));
            if(SUCCEEDED(hr))hr=allocator->Reset();if(SUCCEEDED(hr))hr=list->Reset(allocator,state.Get());if(FAILED(hr))break;
            if(submission)barrier(list,target.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_RENDER_TARGET);
            const auto view=views->GetCPUDescriptorHandleForHeapStart();const FLOAT black[4]{};list->ClearRenderTargetView(view,black,0,nullptr);list->OMSetRenderTargets(1,&view,FALSE,nullptr);
            const D3D12_VIEWPORT viewport{0,0,32,16,0,1};const D3D12_RECT scissor{0,0,32,16};list->RSSetViewports(1,&viewport);list->RSSetScissorRects(1,&scissor);
            list->SetGraphicsRootSignature(root.Get());list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
            const D3D12_VERTEX_BUFFER_VIEW vertices{upload->GetGPUVirtualAddress(),sizeof colors,4};list->IASetVertexBuffers(0,1,&vertices);list->DrawInstanced(3,2,5,3);
            barrier(list,target.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_SOURCE);
            D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=target.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            to.pResource=readback;to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=footprint;list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
            hr=list->Close();if(FAILED(hr))break;ID3D12CommandList* lists[]{list};queue->ExecuteCommandLists(1,lists);
            hr=queue->Signal(fence.Get(),++submission);const HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);
            if(!event){hr=HRESULT_FROM_WIN32(GetLastError());break;}
            if(SUCCEEDED(hr))hr=fence->SetEventOnCompletion(submission,event);
            if(SUCCEEDED(hr)&&WaitForSingleObject(event,3000)!=WAIT_OBJECT_0)hr=HRESULT_FROM_WIN32(ERROR_TIMEOUT);CloseHandle(event);
            if(FAILED(hr))break;
            const D3D12_RANGE read{0,SIZE_T(footprint.Footprint.RowPitch)*16};mapped=nullptr;hr=readback->Map(0,&read,&mapped);
            if(SUCCEEDED(hr)) {
                std::array<float,512> actual{};for(UINT y=0;y<16;++y)std::memcpy(actual.data()+y*32,static_cast<const BYTE*>(mapped)+y*footprint.Footprint.RowPitch,128);
                const D3D12_RANGE no_write{0,0};readback->Unmap(0,&no_write);
                const auto bounds=std::minmax_element(actual.begin(),actual.end());std::cerr<<"vertex-ID control clipped="<<clipped<<" translated="<<translated<<" first="<<actual[0]<<" range="<<*bounds.first<<'/'<<*bounds.second<<'\n';
                if(!translated){expected=actual;if(!clipped)check(std::all_of(actual.begin(),actual.end(),[](float value){return value==.25f;}),"native nonzero vertex/instance starts cover every control pixel");}
                else check(actual==expected,"reference vertex arithmetic preserves native modulo, finite position math and vertex-ID clipping at nonzero starts");
            }
        }
    }
    if(SUCCEEDED(hr))hr=allocator->Reset();if(SUCCEEDED(hr))hr=list->Reset(allocator,nullptr);
    if(FAILED(hr))std::cerr<<"vertex-ID control HRESULT 0x"<<std::hex<<hr<<std::dec<<'\n';return SUCCEEDED(hr);
}
bool native_resolve_rounding(ID3D12Device* device,ID3D12CommandQueue* queue,ID3D12CommandAllocator* allocator,
                             ID3D12GraphicsCommandList* list,ID3D12Resource* readback) {
    struct Compiler {HMODULE value=LoadLibraryExW(L"d3dcompiler_47.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);~Compiler(){if(value)FreeLibrary(value);}} compiler;
    const auto compile=compiler.value ? reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(compiler.value,"D3DCompile")):nullptr;
    if(!compile)return false;
    const char source[]="float4 vs(uint id:SV_VertexID):SV_Position{float2 p=float2((id<<1)&2,id&2);return float4(p*float2(2,-2)+float2(-1,1),0,1);}float4 ps():SV_Target{return 1;}";
    ComPtr<ID3DBlob> vs,ps,serialized;HRESULT hr=compile(source,sizeof source-1,nullptr,nullptr,nullptr,"vs","vs_4_0",0,0,&vs,nullptr);
    if(SUCCEEDED(hr))hr=compile(source,sizeof source-1,nullptr,nullptr,nullptr,"ps","ps_4_0",0,0,&ps,nullptr);
    const D3D12_ROOT_SIGNATURE_DESC root_desc{0,nullptr,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT};
    if(SUCCEEDED(hr))hr=D3D12SerializeRootSignature(&root_desc,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,nullptr);
    ComPtr<ID3D12RootSignature> root;ComPtr<ID3D12Fence> fence;
    if(SUCCEEDED(hr))hr=device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&root));
    if(SUCCEEDED(hr))hr=device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence));
    if(SUCCEEDED(hr))hr=list->Close();UINT64 value=0;
    for(const auto format:{DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R10G10B10A2_UNORM}) {
        if(FAILED(hr))break;
        ComPtr<ID3D12Resource> samples,target;ComPtr<ID3D12DescriptorHeap> views;ComPtr<ID3D12PipelineState> pipeline;
        const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=32;desc.Height=16;
        desc.DepthOrArraySize=desc.MipLevels=1;desc.SampleDesc.Count=4;desc.Format=format;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        hr=device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_RENDER_TARGET,nullptr,IID_PPV_ARGS(&samples));
        desc.SampleDesc.Count=1;desc.Flags=D3D12_RESOURCE_FLAG_NONE;
        if(SUCCEEDED(hr))hr=device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_RESOLVE_DEST,nullptr,IID_PPV_ARGS(&target));
        const D3D12_DESCRIPTOR_HEAP_DESC view_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV,1,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};
        if(SUCCEEDED(hr))hr=device->CreateDescriptorHeap(&view_desc,IID_PPV_ARGS(&views));
        if(SUCCEEDED(hr))device->CreateRenderTargetView(samples.Get(),nullptr,views->GetCPUDescriptorHandleForHeapStart());
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso{};pso.pRootSignature=root.Get();pso.VS={vs->GetBufferPointer(),vs->GetBufferSize()};pso.PS={ps->GetBufferPointer(),ps->GetBufferSize()};
        pso.SampleMask=0xc;pso.SampleDesc.Count=4;pso.PrimitiveTopologyType=D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;pso.NumRenderTargets=1;pso.RTVFormats[0]=format;
        pso.RasterizerState.FillMode=D3D12_FILL_MODE_SOLID;pso.RasterizerState.CullMode=D3D12_CULL_MODE_NONE;pso.RasterizerState.DepthClipEnable=TRUE;
        pso.BlendState.RenderTarget[0]={FALSE,FALSE,D3D12_BLEND_ONE,D3D12_BLEND_ZERO,D3D12_BLEND_OP_ADD,D3D12_BLEND_ONE,D3D12_BLEND_ZERO,D3D12_BLEND_OP_ADD,D3D12_LOGIC_OP_NOOP,D3D12_COLOR_WRITE_ENABLE_ALL};
        if(SUCCEEDED(hr))hr=device->CreateGraphicsPipelineState(&pso,IID_PPV_ARGS(&pipeline));
        if(SUCCEEDED(hr))hr=allocator->Reset();if(SUCCEEDED(hr))hr=list->Reset(allocator,pipeline.Get());if(FAILED(hr))break;
        const auto view=views->GetCPUDescriptorHandleForHeapStart();const FLOAT black[4]{};list->ClearRenderTargetView(view,black,0,nullptr);list->OMSetRenderTargets(1,&view,FALSE,nullptr);
        const D3D12_VIEWPORT viewport{0,0,32,16,0,1};const D3D12_RECT scissor{0,0,32,16};list->RSSetViewports(1,&viewport);list->RSSetScissorRects(1,&scissor);
        list->SetGraphicsRootSignature(root.Get());list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);list->DrawInstanced(3,1,0,0);
        barrier(list,samples.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_RESOLVE_SOURCE);list->ResolveSubresource(target.Get(),0,samples.Get(),0,format);
        barrier(list,target.Get(),D3D12_RESOURCE_STATE_RESOLVE_DEST,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};device->GetCopyableFootprints(&desc,0,1,0,&footprint,nullptr,nullptr,nullptr);
        D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=target.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to.pResource=readback;to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=footprint;list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
        hr=list->Close();if(FAILED(hr))break;ID3D12CommandList* lists[]{list};queue->ExecuteCommandLists(1,lists);
        hr=queue->Signal(fence.Get(),++value);const HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!event){hr=HRESULT_FROM_WIN32(GetLastError());break;}
        if(SUCCEEDED(hr))hr=fence->SetEventOnCompletion(value,event);if(SUCCEEDED(hr)&&WaitForSingleObject(event,3000)!=WAIT_OBJECT_0)hr=HRESULT_FROM_WIN32(ERROR_TIMEOUT);CloseHandle(event);
        void* mapped=nullptr;const D3D12_RANGE read{0,SIZE_T(footprint.Footprint.RowPitch)*16};if(SUCCEEDED(hr))hr=readback->Map(0,&read,&mapped);
        if(SUCCEEDED(hr)) {
            UINT pixel=0;std::memcpy(&pixel,mapped,4);bool uniform=true;for(UINT y=0;y<16;++y)for(UINT x=0;x<32;++x){UINT actual=0;std::memcpy(&actual,static_cast<BYTE*>(mapped)+y*footprint.Footprint.RowPitch+x*4,4);uniform=uniform && actual==pixel;}
            const D3D12_RANGE no_write{0,0};readback->Unmap(0,&no_write);
            const UINT low=format==DXGI_FORMAT_R8G8B8A8_UNORM ? 0x7f7f7f7f:511u|(511u<<10)|(511u<<20)|(1u<<30);
            const UINT high=format==DXGI_FORMAT_R8G8B8A8_UNORM ? 0x80808080:512u|(512u<<10)|(512u<<20)|(2u<<30);
            std::cerr<<"native resolve control format="<<format<<" pixel=0x"<<std::hex<<pixel<<std::dec<<'\n';
            check(uniform && (pixel==low || pixel==high),"native resolve exposes a uniform UNORM half-step result without HLE or shader translation");
        }
    }
    if(SUCCEEDED(hr))hr=allocator->Reset();if(SUCCEEDED(hr))hr=list->Reset(allocator,nullptr);return SUCCEEDED(hr);
}
struct LinearTextureFixture { bool fail_writeback = false, fail_upload=false; UINT uploads=0,publications=0,width=8,height=4; std::vector<UINT32> offsets; };
HRESULT linear_texture_offsets(void* opaque,const UINT32** offsets,std::size_t* count) {
    auto& fixture=*static_cast<LinearTextureFixture*>(opaque);
    *offsets=fixture.offsets.data();*count=fixture.offsets.size();return fixture.offsets.empty() ? S_FALSE:S_OK;
}
HRESULT linear_texture_copy(void* opaque, bool into_tiled, void* linear, void* canonical, UINT subresource, UINT pitch, UINT slice,const TitleGpuClearState*) {
    if (!opaque || !linear || !canonical || subresource >= 3) return E_INVALIDARG;
    auto* fixture = static_cast<LinearTextureFixture*>(opaque);
    if(into_tiled)++fixture->publications;else ++fixture->uploads;
    if(!into_tiled && fixture->fail_upload)return E_FAIL;
    if (into_tiled && fixture->fail_writeback && subresource == 1) return E_FAIL;
    UINT offset = 0;
    for (UINT mip = 0; mip < subresource; ++mip) offset += (std::max)(1u, fixture->width >> mip) * (std::max)(1u, fixture->height >> mip) * 4;
    const UINT row_bytes = (std::max)(1u, fixture->width >> subresource) * 4, rows = (std::max)(1u, fixture->height >> subresource);
    if (pitch < row_bytes || UINT64(pitch) * rows > slice) return E_INVALIDARG;
    for (UINT row = 0; row < rows; ++row) {
        auto* a = static_cast<unsigned char*>(canonical) + offset + row * row_bytes;
        auto* b = static_cast<unsigned char*>(linear) + row * pitch;
        std::memcpy(into_tiled ? a : b, into_tiled ? b : a, row_bytes);
    }
    return S_OK;
}
HRESULT linear_sample_copy(void*, bool into, void* linear, void* canonical, UINT subresource, UINT pitch, UINT slice,const TitleGpuClearState*) {
    if (!linear || !canonical || subresource || pitch < 512 || UINT64(pitch) * 16 > slice) return E_INVALIDARG;
    for (UINT y = 0; y < 16; ++y) {
        auto* a = static_cast<unsigned char*>(canonical) + y * 512;
        auto* b = static_cast<unsigned char*>(linear) + y * pitch;
        std::memcpy(into ? a : b, into ? b : a, 512);
    }
    return S_OK;
}
}
int main(int argc, char** argv) {
    wchar_t debug_value[2]{};
    if(GetEnvironmentVariableW(L"XONE_ERA_D3D12_DEBUG",debug_value,2)==1 && debug_value[0]==L'1') {
        ComPtr<ID3D12Debug> debug;
        if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))debug->EnableDebugLayer();
    }
    const bool warp = argc >= 2 && std::strcmp(argv[1], "warp") == 0;
    const bool bgra_only = argc == 5 && std::strcmp(argv[4],"--bgra") == 0;
    const bool original_xg = argc == 4 || bgra_only;
    if (original_xg) {
        HRESULT hr = bind_d3d11x_abi(std::filesystem::path(argv[2]).wstring());
        if (SUCCEEDED(hr)) hr = bind_xg_module(std::filesystem::path(argv[3]).wstring());
        if (SUCCEEDED(hr)) hr = initialize_xg_module();
        if (FAILED(hr)) return 1;
    }
    ComPtr<IDXGIAdapter> adapter;
    if (warp) {
        ComPtr<IDXGIFactory4> factory;
        if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))) ||
            FAILED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter)))) return 1;
    }
    ComPtr<ID3D12Device> device;
    const HRESULT created = D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
    if (FAILED(created)) { std::cout << "D3D12 backend unavailable\n"; return warp ? 1 : 77; }
    ComPtr<IDXGIFactory4> factory;
    ComPtr<IDXGIAdapter1> actual;
    DXGI_ADAPTER_DESC1 adapter_desc{};
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->EnumAdapterByLuid(device->GetAdapterLuid(), IID_PPV_ARGS(&actual))) ||
        FAILED(actual->GetDesc1(&adapter_desc))) return 1;
    const bool software = (adapter_desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0;
    std::wcout << L"adapter=" << adapter_desc.Description << L" software=" << software << L'\n';
    if (!warp && software) return 77;
    check(!warp || software, "WARP evidence comes from a software adapter");
    using Allocate = void* (WINAPI*)(SIZE_T, ULONGLONG);
    using Release = void (WINAPI*)(void*, ULONGLONG);
    const auto allocate = reinterpret_cast<Allocate>(resolve_kernelx({"kernelx.dll", "XMemAllocDefault", 0}));
    const auto release = reinterpret_cast<Release>(resolve_kernelx({"kernelx.dll", "XMemFreeDefault", 0}));
    if (!allocate || !release) return 1;
    struct MemoryStatus {
        DWORD length = 64, reserved = 0;
        UINT64 total = 0, available = 0, legacy_used = 0, legacy_peak = 0, legacy_available = 0, used = 0, title_available = 0;
    };
    static_assert(sizeof(MemoryStatus) == 64);
    const auto memory_status = reinterpret_cast<BOOL (WINAPI*)(MemoryStatus*)>(resolve_kernelx({"kernelx.dll", "TitleMemoryStatus", 0}));
    if (!memory_status) return 1;
    const auto status = [&] {
        MemoryStatus result;
        check(memory_status(&result) && GetLastError() == ERROR_SUCCESS, "64-byte title memory status succeeds");
        MEMORYSTATUSEX host{}; host.dwLength = sizeof host;
        check(GlobalMemoryStatusEx(&host) && result.total == (std::min)(UINT64{0x140000000}, host.ullTotalPhys) &&
            result.available <= result.total - result.used && result.available == result.title_available &&
            !result.reserved && !result.legacy_used && !result.legacy_peak && !result.legacy_available,
            "old ABI exposes bounded budget and usage with inactive legacy fields");
        return result;
    };
    MemoryStatus bad_length; bad_length.length = 80;
    check(!memory_status(nullptr) && GetLastError() == ERROR_INVALID_PARAMETER &&
        !memory_status(&bad_length) && GetLastError() == ERROR_INVALID_PARAMETER && bad_length.length == 80,
        "null and modern memory status ABI rejected without output");
    const UINT64 initial_used = status().used;
    const auto va = reinterpret_cast<decltype(&VirtualAlloc)>(resolve_kernelx({"kernelx.dll", "VirtualAlloc", 0}));
    const auto vf = reinterpret_cast<decltype(&VirtualFree)>(resolve_kernelx({"kernelx.dll", "VirtualFree", 0}));
    auto* cpu = static_cast<unsigned char*>(va(nullptr, 65536, MEM_RESERVE, PAGE_READWRITE));
    if (!cpu) return 1;
    check(status().used == initial_used, "virtual reservation costs no physical budget");
    check(va(cpu + 1, 4096, MEM_COMMIT, PAGE_READWRITE) == cpu && status().used == initial_used + 8192,
        "unaligned CPU commit accounts actual pages");
    cpu[0] = 0x42;
    check(va(cpu, 12288, MEM_COMMIT, PAGE_READWRITE) == cpu && cpu[0] == 0x42 && status().used == initial_used + 12288,
        "overlapping CPU recommit counts new pages only and preserves bytes");
    check(!va(cpu, 4096, MEM_COMMIT, 0xffffffff) && status().used == initial_used + 12288 && cpu[0] == 0x42,
        "host commit failure preserves budget and existing bytes");
    check(vf(cpu + 1, 4096, MEM_DECOMMIT) && status().used == initial_used + 4096,
        "partial CPU decommit subtracts rounded committed pages");
    check(vf(cpu, 8192, MEM_DECOMMIT) && status().used == initial_used + 4096,
        "repeated decommit cannot subtract twice");
    check(va(cpu, 4096, MEM_COMMIT, PAGE_READWRITE) == cpu && !cpu[0] && status().used == initial_used + 8192,
        "CPU recommit restores zeroed pages and budget");
    check(vf(cpu, 0, MEM_DECOMMIT) && status().used == initial_used && vf(cpu, 0, MEM_RELEASE),
        "zero-size decommit and release restore physical budget");
    auto* combined = va(nullptr, 1, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    check(combined && status().used == initial_used + 4096 && vf(combined, 0, MEM_RELEASE) && status().used == initial_used,
        "combined CPU reserve/commit/release accounts page rounding");
    auto* address_probe=static_cast<unsigned char*>(VirtualAlloc(nullptr,65536,MEM_RESERVE,PAGE_NOACCESS));
    if(!address_probe) return 1;
    VirtualFree(address_probe,0,MEM_RELEASE);
    auto* fixed_cpu=va(address_probe+4096,1,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE);
    MEMORY_BASIC_INFORMATION fixed_info{};
    check(fixed_cpu && VirtualQuery(fixed_cpu,&fixed_info,sizeof fixed_info) && fixed_info.State==MEM_COMMIT &&
        status().used==initial_used+fixed_info.RegionSize,"fixed unaligned combined allocation charges every page committed by the host");
    if(fixed_cpu) check(vf(fixed_cpu,0,MEM_RELEASE) && status().used==initial_used,"fixed CPU allocation returns its full commitment");
    constexpr SIZE_T over_budget = 0x140010000ull;
    check(!allocate(over_budget, 0) && GetLastError() == ERROR_NOT_ENOUGH_MEMORY && status().used == initial_used,
        "XMem rejects over-budget commitments transactionally");
    check(!va(nullptr, over_budget, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE) &&
        GetLastError() == ERROR_NOT_ENOUGH_MEMORY && status().used == initial_used,
        "ordinary VirtualAlloc rejects over-budget commitment before host allocation");
    auto* huge = va(nullptr, over_budget, 0x30002000, PAGE_READWRITE);
    if (!huge) return 1;
    check(status().used == initial_used && !va(huge, over_budget, 0x30001000, 0x10004) &&
        GetLastError() == ERROR_NOT_ENOUGH_MEMORY && status().used == initial_used && vf(huge, 0, MEM_RELEASE),
        "graphics reservation exceeds budget but physical commit is refused transactionally");
    constexpr ULONGLONG attributes = 0x64034000;
    auto* memory = static_cast<unsigned char*>(allocate(0x129003, attributes));
    check(memory != nullptr, "observed XMem attributes allocate real CPU backing");
    if (!memory) return 1;
    check(status().used == initial_used + 0x130000, "XMem charges its actual 64-KiB backing");
    check(!allocate(SIZE_T(status().total - status().used + 1), 0) && GetLastError() == ERROR_NOT_ENOUGH_MEMORY &&
        status().used == initial_used + 0x130000, "remaining title budget includes existing CPU/GPU commitments");
    check((reinterpret_cast<std::uintptr_t>(memory) & 65535) == 0 &&
              reinterpret_cast<std::uintptr_t>(memory) >= 0x100000000ull &&
              reinterpret_cast<std::uintptr_t>(memory) < 0x10000000000ull, "graphics alignment and virtual range");
    check(memory[0] == 0 && memory[0x129002] == 0, "new memory is zero initialized");
    check(!allocate(1, attributes | (1ull << 63)) && GetLastError() == ERROR_NOT_SUPPORTED,
          "reserved attributes rejected");
    check(!allocate(1, 7ull << 29) && GetLastError() == ERROR_NOT_SUPPORTED, "physical pages remain unsupported");
    std::memset(memory, 0x39, 64);
    TitleGpuResource pin;
    HRESULT hr = acquire_title_gpu_buffer(memory, 64, device.Get(), pin);
    std::cout << (warp ? "WARP" : "hardware") << " shared heap hr=0x" << std::hex << hr << std::dec << '\n';
    check(SUCCEEDED(hr), "CPU allocation binds to a D3D12 heap without a shadow copy");
    if (FAILED(hr)) { release(memory, attributes); return 1; }
    TitleGpuResource second;
    check(release_title_gpu_resource(pin, nullptr, 0) == E_INVALIDARG && pin.resource,
          "missing completion fence cannot release a pin");
    check(FAILED(acquire_title_gpu_buffer(memory, 64, device.Get(), second)), "concurrent GPU acquisition refused");
    check(FAILED(acquire_title_gpu_buffer(memory + 0x130000, 1, device.Get(), second)), "out-of-range binding refused");
    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC q{};
    q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    ComPtr<ID3D12Fence> fence;
    hr = device->CreateCommandQueue(&q, IID_PPV_ARGS(&queue));
    if (SUCCEEDED(hr)) hr = device->CreateCommandAllocator(q.Type, IID_PPV_ARGS(&allocator));
    if (SUCCEEDED(hr)) hr = device->CreateCommandList(0, q.Type, allocator.Get(), nullptr, IID_PPV_ARGS(&list));
    if (SUCCEEDED(hr)) hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    check(SUCCEEDED(hr), "GPU copy and fence objects created");
    if (FAILED(hr)) return 1;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = 65536; desc.Height = 1; desc.DepthOrArraySize = 1; desc.MipLevels = 1;
    desc.SampleDesc.Count = 1; desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_READBACK;
    ComPtr<ID3D12Resource> readback, upload;
    hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_COPY_DEST,
                                        nullptr, IID_PPV_ARGS(&readback));
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    if (SUCCEEDED(hr)) hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upload));
    check(SUCCEEDED(hr), "bounded copy fixtures created");
    if(SUCCEEDED(hr))check(shader_arithmetic(device.Get(),queue.Get(),allocator.Get(),list.Get(),readback.Get()),
                           "reference arithmetic executes through real native compute pipelines");
    if (FAILED(hr)) return 1;
    const bool vertex_control=shader_vertex_ids(device.Get(),queue.Get(),allocator.Get(),list.Get(),readback.Get(),upload.Get());
    check(vertex_control,"native and reference vertex-ID controls complete on the real GPU");if(!vertex_control)return 1;
    const bool resolve_control=native_resolve_rounding(device.Get(),queue.Get(),allocator.Get(),list.Get(),readback.Get());
    check(resolve_control,"native resolve rounding control completes on the selected GPU");if(!resolve_control)return 1;
    void* mapped = nullptr;
    hr = upload->Map(0, nullptr, &mapped);
    if (FAILED(hr)) return 1;
    std::memset(mapped, 0xA7, 64);
    upload->Unmap(0, nullptr);
    // CPU writes precede GPU execution; both copies use the same allocation.
    barrier(list.Get(), pin.resource, pin.initial_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(readback.Get(), 0, pin.resource, pin.offset, 64);
    barrier(list.Get(), pin.resource, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyBufferRegion(pin.resource, pin.offset, upload.Get(), 0, 64);
    barrier(list.Get(), pin.resource, D3D12_RESOURCE_STATE_COPY_DEST, pin.initial_state);
    if (FAILED(list->Close())) return 1;
    ID3D12CommandList* lists[]{list.Get()};
    queue->ExecuteCommandLists(1, lists);
    check(release_title_gpu_resource(pin, fence.Get(), 1) == E_PENDING && pin.resource,
          "unsignaled fence preserves the memory pin");
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) return 1;
    hr = queue->Signal(fence.Get(), 1);
    if (SUCCEEDED(hr)) hr = fence->SetEventOnCompletion(1, event);
    const bool complete = SUCCEEDED(hr) && WaitForSingleObject(event, 3000) == WAIT_OBJECT_0;
    CloseHandle(event);
    check(complete, "GPU copy completes within the deadline");
    if (!complete) return 1;
    check(release_title_gpu_resource(pin, fence.Get(), 1) == S_OK && !pin.resource, "completed GPU pin releases");
    check(acquire_title_gpu_buffer(memory, 64, device.Get(), second, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == DXGI_ERROR_UNSUPPORTED && !second.resource,
          "cached read/copy resource cannot silently become a UAV alias");
    check(memory[0] == 0xA7 && memory[63] == 0xA7, "GPU writes are visible in the original CPU allocation");
    hr = readback->Map(0, nullptr, &mapped);
    check(SUCCEEDED(hr) && static_cast<unsigned char*>(mapped)[0] == 0x39 &&
          static_cast<unsigned char*>(mapped)[63] == 0x39, "GPU read observes CPU data");
    if (SUCCEEDED(hr)) readback->Unmap(0, nullptr);
    check(acquire_title_gpu_buffer(memory + 16, 32, device.Get(), pin) == S_OK && pin.offset == 16,
          "subranges share the allocation and retain their offset");
    release(memory, attributes);
    MEMORY_BASIC_INFORMATION info{};
    VirtualQuery(memory, &info, sizeof info);
    check(info.State == MEM_COMMIT, "free defers while GPU pin exists");
    check(status().used == initial_used + 0x130000, "retiring allocation remains charged until last pin");
    check(FAILED(acquire_title_gpu_buffer(memory, 1, device.Get(), second)), "retiring allocation cannot be rebound");
    check(release_title_gpu_resource(pin, fence.Get(), 2) == E_PENDING, "deferred free waits for the fence");
    check(SUCCEEDED(fence->Signal(2)), "fixture completion signaled");
    check(release_title_gpu_resource(pin, fence.Get(), 2) == S_OK, "deferred free completes after fence");
    VirtualQuery(memory, &info, sizeof info);
    check(info.State == MEM_FREE, "final pin releases CPU and GPU backing");
    check(status().used == initial_used, "completed free returns the physical budget");

    const auto virtual_allocate = reinterpret_cast<decltype(&VirtualAlloc)>(resolve_kernelx({"kernelx.dll", "VirtualAlloc", 0}));
    const auto virtual_free = reinterpret_cast<decltype(&VirtualFree)>(resolve_kernelx({"kernelx.dll", "VirtualFree", 0}));
    void* dynamic_reservation = virtual_allocate(nullptr, 0x27f8000, 0x30002000, PAGE_READWRITE);
    check(dynamic_reservation && !(reinterpret_cast<std::uintptr_t>(dynamic_reservation) & 65535) &&
        reinterpret_cast<std::uintptr_t>(dynamic_reservation) >= 0x100000000ull &&
        reinterpret_cast<std::uintptr_t>(dynamic_reservation) < 0x10000000000ull,
        "observed null-address graphics reservation chooses a real address in the Xbox range");
    if (!dynamic_reservation) return 1;
    VirtualQuery(dynamic_reservation, &info, sizeof info);
    check(info.State == MEM_RESERVE && info.RegionSize == 0x2800000,
          "graphics reservation rounds its size without manufacturing committed pages");
    auto* esram_memory = static_cast<unsigned char*>(dynamic_reservation);
    const UINT first_pages[]{3, 5};
    check(map_title_esram(1, esram_memory, 2, first_pages) == S_OK, "ESRAM pages map into an owned graphics reservation");
    check(status().used == initial_used + (32u << 20), "ESRAM pool charges its committed backing once");
    VirtualQuery(esram_memory, &info, sizeof info);
    if (info.State != MEM_COMMIT) return 1;
    esram_memory[0] = 0x33; esram_memory[65536] = 0x55;
    auto* alias_memory = static_cast<unsigned char*>(virtual_allocate(nullptr, 65536, 0x30002000, PAGE_READWRITE));
    if (!alias_memory) return 1;
    check(map_title_esram(1, alias_memory, 1, first_pages) == S_OK && alias_memory[0] == 0x33,
          "two virtual mappings see the same physical ESRAM bytes");
    VirtualQuery(alias_memory, &info, sizeof info);
    if (info.State != MEM_COMMIT) return 1;
    alias_memory[0] = 0x44;
    check(esram_memory[0] == 0x44, "CPU writes are shared immediately across ESRAM aliases");
    check(status().used == initial_used + (32u << 20), "ESRAM alias adds no physical commitment");
    const UINT invalid_page = 512;
    check(map_title_esram(0, esram_memory, 1, first_pages) == E_INVALIDARG &&
        map_title_esram(1, esram_memory, 1, &invalid_page) == E_INVALIDARG && esram_memory[0] == 0x44,
        "invalid ESRAM flags and pages preserve existing mappings");
    check(map_title_esram(1, esram_memory, 1, first_pages + 1) == S_OK && esram_memory[0] == 0x55,
          "ESRAM remapping changes identity without copying disconnected data");
    esram_memory[65536] = 0x66;
    check(esram_memory[0] == 0x66 && alias_memory[0] == 0x44, "repeated physical pages alias while distinct pages stay independent");
    check(map_title_esram(1, esram_memory, 2, nullptr) == S_OK, "ESRAM unmapping restores owned placeholders");
    check(status().used == initial_used + (32u << 20), "unmapped retained ESRAM still owns physical backing");
    VirtualQuery(esram_memory, &info, sizeof info);
    check(info.State == MEM_RESERVE && alias_memory[0] == 0x44, "unmapping one alias preserves physical bytes and other mappings");
    std::array<UINT, 512> all_pages{};
    for (UINT i = 0; i < all_pages.size(); ++i) all_pages[i] = i;
    const HRESULT full_mapping = map_title_esram(1, esram_memory, UINT(all_pages.size()), all_pages.data());
    check(full_mapping == S_OK && esram_memory[3 * 65536] == 0x44,
          "observed 512-page mapping preserves the complete 32-MiB physical pool");
    if (FAILED(full_mapping)) return 1;
    VirtualQuery(esram_memory + (32u << 20), &info, sizeof info);
    check(info.State == MEM_RESERVE, "ESRAM mapping leaves the remaining graphics reservation uncommitted");
    auto* large_reservation = static_cast<unsigned char*>(virtual_allocate(nullptr, 8u << 20, 0x30002000, PAGE_READWRITE));
    if (!large_reservation) return 1;
    auto* large_page = reinterpret_cast<unsigned char*>((reinterpret_cast<std::uintptr_t>(large_reservation) + (4u << 20) - 1) & ~std::uintptr_t((4u << 20) - 1));
    const UINT last_large = 7, last_small = 448;
    check(map_title_esram(2, large_page, 1, &last_large) == S_OK, "4-MiB page mode uses the same ESRAM pool");
    VirtualQuery(large_page, &info, sizeof info);
    if (info.State != MEM_COMMIT) return 1;
    large_page[0] = 0x77;
    check(map_title_esram(1, alias_memory, 1, &last_small) == S_OK && alias_memory[0] == 0x77 && esram_memory[448 * 65536] == 0x77,
          "64-KiB and 4-MiB ESRAM mappings share physical identity");
    check(map_title_esram(2, large_page + 65536, 1, &last_large) == E_INVALIDARG && large_page[0] == 0x77,
          "misaligned large-page mappings cannot replace valid data");
    check(virtual_free(large_reservation, 0, MEM_RELEASE) && virtual_free(alias_memory, 0, MEM_RELEASE) &&
        virtual_free(dynamic_reservation, 0, MEM_RELEASE), "ESRAM reservations release every owned mapping and placeholder");
    constexpr SIZE_T reservation_size = 1u << 21;
    void* probe = VirtualAlloc(nullptr, 2 * reservation_size, MEM_RESERVE, PAGE_NOACCESS);
    if (!probe) return 1;
    const auto base = (reinterpret_cast<std::uintptr_t>(probe) + reservation_size - 1) & ~(reservation_size - 1);
    VirtualFree(probe, 0, MEM_RELEASE);
    void* reservation = virtual_allocate(reinterpret_cast<void*>(base), reservation_size, 0x30002000, PAGE_READWRITE);
    check(reservation != nullptr, "graphics reservation fixture");
    if (!reservation) return 1;
    auto* chunk = static_cast<unsigned char*>(reservation) + 65536;
    check(virtual_allocate(chunk, 65536, 0x30001000, 0x10004) == chunk,
          "observed graphics commitment preserves fixed address");
    VirtualQuery(chunk, &info, sizeof info);
    check(info.State == MEM_COMMIT && info.Protect == PAGE_READWRITE, "committed span has real CPU backing");
    check(status().used == initial_used + (32u << 20) + 65536, "DDR subcommit is charged separately from ESRAM aliases");
    if (info.State != MEM_COMMIT) { virtual_free(reservation, 0, MEM_RELEASE); return 1; }
    std::memset(chunk, 0x41, 64);
    check(!virtual_allocate(chunk, 131072, 0x30001000, 0x10004) && GetLastError() == ERROR_INVALID_ADDRESS && chunk[0] == 0x41,
          "overlapping commitment cannot replace existing bytes");
    check(!virtual_allocate(chunk, 65536, 0x30001000, PAGE_READWRITE) && GetLastError() == ERROR_NOT_SUPPORTED,
          "recommit cannot silently change the guest cache/protection mode");
    auto* neighbor = chunk + 131072;
    check(virtual_allocate(neighbor, 65536, 0x30001000, 0x10004) == neighbor && neighbor[0] == 0,
          "another committed span splits the remaining owned gap");
    neighbor[0] = 0x5B;
    hr = acquire_title_gpu_buffer(chunk, 64, device.Get(), pin);
    std::cout << "graphics subcommit heap hr=0x" << std::hex << hr << std::dec << '\n';
    check(SUCCEEDED(hr), "subcommit binds the same CPU bytes to D3D12");
    if (FAILED(hr)) { virtual_free(reservation, 0, MEM_RELEASE); return 1; }
    check(!virtual_free(reservation, 0, MEM_RELEASE) && GetLastError() == ERROR_BUSY,
          "pinned subcommit prevents releasing parent reservation");
    check(!virtual_free(chunk, 65536, MEM_DECOMMIT) && GetLastError() == ERROR_BUSY,
          "pinned span cannot be decommitted");
    check(FAILED(acquire_title_gpu_buffer(chunk - 65536, 1, device.Get(), second)), "uncommitted GPU pages cannot be bound");
    check(SUCCEEDED(allocator->Reset()) && SUCCEEDED(list->Reset(allocator.Get(), nullptr)), "reuse bounded copy command storage");
    barrier(list.Get(), pin.resource, pin.initial_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(readback.Get(), 0, pin.resource, pin.offset, 64);
    barrier(list.Get(), pin.resource, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyBufferRegion(pin.resource, pin.offset, upload.Get(), 0, 64);
    barrier(list.Get(), pin.resource, D3D12_RESOURCE_STATE_COPY_DEST, pin.initial_state);
    if (FAILED(list->Close())) return 1;
    queue->ExecuteCommandLists(1, lists);
    event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) return 1;
    hr = queue->Signal(fence.Get(), 3);
    if (SUCCEEDED(hr)) hr = fence->SetEventOnCompletion(3, event);
    const bool subcommit_complete = SUCCEEDED(hr) && WaitForSingleObject(event, 3000) == WAIT_OBJECT_0;
    CloseHandle(event);
    check(subcommit_complete, "GPU work over subcommit completes");
    if (!subcommit_complete) return 1;
    check(release_title_gpu_resource(pin, fence.Get(), 3) == S_OK, "subcommit pin retires");
    check(chunk[0] == 0xA7 && chunk[63] == 0xA7, "GPU writes reach committed CPU pages");
    hr = readback->Map(0, nullptr, &mapped);
    check(SUCCEEDED(hr) && static_cast<unsigned char*>(mapped)[0] == 0x41 &&
          static_cast<unsigned char*>(mapped)[63] == 0x41, "GPU reads original subcommit bytes");
    if (SUCCEEDED(hr)) readback->Unmap(0, nullptr);
    check(virtual_free(chunk, 65536, MEM_DECOMMIT), "completed GPU span decommits");
    check(status().used == initial_used + (32u << 20) + 65536, "DDR decommit returns only its physical commitment");
    VirtualQuery(chunk, &info, sizeof info);
    check(info.State == MEM_RESERVE, "decommit preserves parent reservation");
    check(virtual_allocate(chunk, 65536, 0x30001000, 0x10004) == chunk && chunk[0] == 0,
          "recommit resets released page contents");
    check(neighbor[0] == 0x5B, "recommit leaves the other backing intact");
    check(virtual_free(reservation, 0, MEM_RELEASE), "graphics reservation and committed backing released");
    check(status().used == initial_used + (32u << 20), "DDR release leaves only the retained ESRAM pool charged");
    {
        auto* storage = static_cast<UINT32*>(allocate(65536, attributes));
        if (!storage) return 1;
        std::memset(storage, 0, 64);
        hr = acquire_title_gpu_buffer(storage, 64, device.Get(), pin, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        std::cout << "shared UAV heap hr=0x" << std::hex << hr << std::dec << '\n';
        check(SUCCEEDED(hr), "shared title buffer supports actual unordered GPU writes");
        if (FAILED(hr)) { release(storage, attributes); return 1; }
        ComPtr<ID3D12DescriptorHeap> cpu_heap, gpu_heap;
        D3D12_DESCRIPTOR_HEAP_DESC descriptors{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        hr = device->CreateDescriptorHeap(&descriptors, IID_PPV_ARGS(&cpu_heap));
        descriptors.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        if (SUCCEEDED(hr)) hr = device->CreateDescriptorHeap(&descriptors, IID_PPV_ARGS(&gpu_heap));
        if (FAILED(hr)) return 1;
        D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R32_UINT; view.ViewDimension = D3D12_UAV_DIMENSION_BUFFER; view.Buffer.NumElements = 16;
        const auto cpu = cpu_heap->GetCPUDescriptorHandleForHeapStart();
        device->CreateUnorderedAccessView(pin.resource, nullptr, &view, cpu);
        device->CopyDescriptorsSimple(1, gpu_heap->GetCPUDescriptorHandleForHeapStart(), cpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        if (FAILED(allocator->Reset()) || FAILED(list->Reset(allocator.Get(), nullptr))) return 1;
        ID3D12DescriptorHeap* heaps[]{gpu_heap.Get()};
        list->SetDescriptorHeaps(1, heaps);
        barrier(list.Get(), pin.resource, pin.initial_state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        const UINT values[]{0x12345678, 0x12345678, 0x12345678, 0x12345678};
        list->ClearUnorderedAccessViewUint(gpu_heap->GetGPUDescriptorHandleForHeapStart(), cpu, pin.resource, values, 0, nullptr);
        barrier(list.Get(), pin.resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyBufferRegion(readback.Get(), 0, pin.resource, pin.offset, 64);
        barrier(list.Get(), pin.resource, D3D12_RESOURCE_STATE_COPY_SOURCE, pin.initial_state);
        if (FAILED(list->Close())) return 1;
        queue->ExecuteCommandLists(1, lists);
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event) return 1;
        hr = queue->Signal(fence.Get(), 4);
        if (SUCCEEDED(hr)) hr = fence->SetEventOnCompletion(4, event);
        const bool cleared = SUCCEEDED(hr) && WaitForSingleObject(event, 3000) == WAIT_OBJECT_0;
        CloseHandle(event);
        check(cleared, "GPU UAV clear completes within deadline");
        if (!cleared) return 1;
        check(release_title_gpu_resource(pin, fence.Get(), 4) == S_OK, "UAV memory pin retires through real fence");
        bool same = true;
        for (UINT i = 0; i < 16; ++i) same = same && storage[i] == values[0];
        check(same, "unordered GPU writes reach original CPU bytes");
        hr = readback->Map(0, nullptr, &mapped);
        same = SUCCEEDED(hr);
        if (same) {
            for (UINT i = 0; i < 16; ++i) same = same && static_cast<UINT32*>(mapped)[i] == values[0];
            readback->Unmap(0, nullptr);
        }
        check(same, "UAV readback matches the shared CPU allocation");
        // Actual title shape: seven DWORDs per record at canonical offset +16.
        // The hidden counter deliberately shares the same parent allocation.
        std::memset(storage + 4, 0x78, 56);
        std::memset(storage + 18, 0x5b, 24);
        auto* counter = storage + 24;
        *counter = 0;
        TitleGpuResource counter_pin, alias, duplicate;
        hr = acquire_title_gpu_buffer(storage + 4, 56, device.Get(), pin, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, true);
        if (SUCCEEDED(hr)) hr = acquire_title_gpu_buffer(counter, 4, device.Get(), counter_pin, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, true);
        if (SUCCEEDED(hr)) hr = acquire_title_gpu_buffer(storage + 4, 28, device.Get(), alias, D3D12_RESOURCE_FLAG_NONE, true);
        if (SUCCEEDED(hr)) hr = acquire_title_gpu_buffer(storage + 4, 56, device.Get(), duplicate, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, true);
        check(SUCCEEDED(hr) && pin.offset == 0 && counter_pin.offset == 0 && pin.resource == duplicate.resource &&
            pin.resource != alias.resource && pin.canonical_offset == 16 && counter_pin.canonical_offset == 96,
            "aligned windows preserve canonical ranges, share exact bindings and retain independent aliases");
        if (FAILED(hr)) return 1;
        check(release_title_gpu_resource(duplicate, fence.Get(), 4) == S_OK, "duplicate storage owner releases without GPU use");
        check(acquire_title_gpu_buffer(storage, 4, device.Get(), second) == HRESULT_FROM_WIN32(ERROR_BUSY),
              "direct GPU pin cannot bypass aligned representation coherence");
        D3D12_COMMAND_QUEUE_DESC copy_desc{};
        copy_desc.Type = D3D12_COMMAND_LIST_TYPE_COPY;
        ComPtr<ID3D12CommandQueue> copy_queue;
        ComPtr<ID3D12CommandAllocator> copy_allocator;
        ComPtr<ID3D12GraphicsCommandList> copy_list;
        ComPtr<ID3D12Fence> copy_fence;
        hr = device->CreateCommandQueue(&copy_desc, IID_PPV_ARGS(&copy_queue));
        if (SUCCEEDED(hr)) hr = device->CreateCommandAllocator(copy_desc.Type, IID_PPV_ARGS(&copy_allocator));
        if (SUCCEEDED(hr)) hr = device->CreateCommandList(0, copy_desc.Type, copy_allocator.Get(), nullptr, IID_PPV_ARGS(&copy_list));
        if (SUCCEEDED(hr)) hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&copy_fence));
        if (FAILED(hr)) return 1;
        const HMODULE compiler = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        const auto compile = compiler ? reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
        if (!compile) return 1;
        constexpr char source[] = "struct Record{uint a,b,c,d,e,f,g;}; AppendStructuredBuffer<Record> output:register(u0);"
            "[numthreads(1,1,1)] void main(){Record r={0xCAFEBABEu,2,3,4,5,6,7};output.Append(r);}";
        ComPtr<ID3DBlob> shader, errors, serialized;
        hr = compile(source, sizeof source - 1, nullptr, nullptr, nullptr, "main", "cs_5_0", 0, 0, &shader, &errors);
        D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 0};
        D3D12_ROOT_PARAMETER parameter{};
        parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameter.DescriptorTable = {1, &range}; parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        D3D12_ROOT_SIGNATURE_DESC root_desc{1, &parameter, 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
        if (SUCCEEDED(hr)) hr = D3D12SerializeRootSignature(&root_desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, nullptr);
        ComPtr<ID3D12RootSignature> root;
        if (SUCCEEDED(hr)) hr = device->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&root));
        ComPtr<ID3D12PipelineState> pipeline;
        if (SUCCEEDED(hr)) {
            D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline_desc{};
            pipeline_desc.pRootSignature = root.Get(); pipeline_desc.CS = {shader->GetBufferPointer(), shader->GetBufferSize()};
            hr = device->CreateComputePipelineState(&pipeline_desc, IID_PPV_ARGS(&pipeline));
        }
        shader.Reset(); errors.Reset(); serialized.Reset(); FreeLibrary(compiler);
        std::cout << "owned append compute pipeline hr=0x" << std::hex << hr << std::dec << '\n';
        check(SUCCEEDED(hr), "owned SM5 shader creates a native compute pipeline");
        if (FAILED(hr)) return 1;
        view.Format = DXGI_FORMAT_UNKNOWN; view.Buffer.StructureByteStride = 28; view.Buffer.NumElements = 2;
        device->CreateUnorderedAccessView(pin.resource, counter_pin.resource, &view, cpu);
        device->CopyDescriptorsSimple(1, gpu_heap->GetCPUDescriptorHandleForHeapStart(), cpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        if (FAILED(allocator->Reset()) || FAILED(list->Reset(allocator.Get(), pipeline.Get()))) return 1;
        list->SetDescriptorHeaps(1, heaps); list->SetComputeRootSignature(root.Get());
        list->SetComputeRootDescriptorTable(0, gpu_heap->GetGPUDescriptorHandleForHeapStart());
        check(begin_title_gpu_buffer(pin, list.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS) == S_OK,
              "begin uploads the unaligned payload to its aligned representation");
        void* cpu_address = storage;
        check(map_title_gpu_resource(pin, &cpu_address) == E_PENDING && !cpu_address,
              "CPU mapping cannot race an active GPU representation");
        check(map_title_gpu_resource(pin, &cpu_address,true) == E_PENDING && !cpu_address,
              "blocking maps refuse to wait on their own unsubmitted interval");
        check(begin_title_gpu_buffer(alias, list.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE) == HRESULT_FROM_WIN32(ERROR_BUSY),
              "simultaneous overlapping shader/copy aliases are explicitly refused");
        check(begin_title_gpu_buffer(counter_pin, copy_list.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE) == HRESULT_FROM_WIN32(ERROR_BUSY),
              "another queue cannot transition the canonical parent while a list owns it");
        check(begin_title_gpu_buffer(counter_pin, list.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS) == S_OK,
              "non-overlapping data and counter share one parent in the same command list");
        check(release_title_gpu_resource(pin, fence.Get(), 4) == E_PENDING && pin.resource,
              "unfinished GPU use cannot retire through an older completed fence");
        list->Dispatch(1, 1, 1);
        check(end_title_gpu_buffer(pin, list.Get()) == S_OK && end_title_gpu_buffer(counter_pin, list.Get()) == S_OK,
              "end copies actual append data and counter back before the completion fence");
        check(end_title_gpu_buffer(pin, list.Get()) == E_INVALIDARG, "writeback cannot be recorded twice");
        barrier(list.Get(), pin.resource, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        barrier(list.Get(), counter_pin.resource, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyBufferRegion(readback.Get(), 0, pin.resource, pin.offset, 56);
        list->CopyBufferRegion(readback.Get(), 64, counter_pin.resource, counter_pin.offset, 4);
        barrier(list.Get(), pin.resource, D3D12_RESOURCE_STATE_COPY_SOURCE, pin.initial_state);
        barrier(list.Get(), counter_pin.resource, D3D12_RESOURCE_STATE_COPY_SOURCE, counter_pin.initial_state);
        if (FAILED(list->Close())) return 1;
        queue->ExecuteCommandLists(1, lists);
        check(complete_title_gpu_resource(pin, fence.Get(), 5) == E_PENDING,
              "aligned use remains active until the actual completion fence");
        check(begin_title_gpu_buffer(alias, copy_list.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE) == HRESULT_FROM_WIN32(ERROR_BUSY),
              "overlapping use stays blocked after recording writeback and before completion");
        auto waiting_map=std::async(std::launch::async,[&] {
            void* address=nullptr;
            const HRESULT result=map_title_gpu_resource(pin,&address,true);
            const bool coherent=SUCCEEDED(result) && static_cast<UINT32*>(address)[0]==0xcafebabe;
            return coherent && unmap_title_gpu_resource(pin)==S_OK;
        });
        check(waiting_map.wait_for(std::chrono::milliseconds(30))==std::future_status::timeout,
              "blocking CPU map stays gated before the producer's real fence and publication");
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event) return 1;
        hr = queue->Signal(fence.Get(), 5);
        if (SUCCEEDED(hr)) hr = fence->SetEventOnCompletion(5, event);
        const bool appended = SUCCEEDED(hr) && WaitForSingleObject(event, 3000) == WAIT_OBJECT_0;
        CloseHandle(event);
        check(appended, "GPU append and counter complete within deadline");
        if (!appended) return 1;
        check(complete_title_gpu_resource(pin, fence.Get(), 5) == S_OK && complete_title_gpu_resource(counter_pin, fence.Get(), 5) == S_OK,
              "storage ownership survives completed GPU use for another dispatch");
        check(waiting_map.get(),"blocking CPU map resumes with coherent bytes after the submission thread retires");
        check(storage[4] == 0xcafebabe && storage[5] == 2 && storage[10] == 7 && storage[11] == 0x78787878 && *counter == 1,
              "native twenty-eight-byte append and hidden counter reach original unaligned CPU bytes");
        bool neighbors = true;
        for (UINT i = 0; i < 4; ++i) neighbors = neighbors && storage[i] == values[0];
        for (UINT i = 18; i < 24; ++i) neighbors = neighbors && storage[i] == 0x5b5b5b5b;
        check(neighbors, "writeback preserves the sixteen-byte allocator header and neighboring suballocations");
        hr = readback->Map(0, nullptr, &mapped);
        check(SUCCEEDED(hr) && static_cast<UINT32*>(mapped)[0] == 0xcafebabe && static_cast<UINT32*>(mapped)[16] == 1,
              "append data and counter readback match CPU-visible bytes");
        if (SUCCEEDED(hr)) readback->Unmap(0, nullptr);
        // A different representation on a real COPY queue must observe both the
        // preceding GPU write and CPU edits made at the completed boundary.
        check(map_title_gpu_resource(alias, &cpu_address) == S_OK && cpu_address == storage + 4,
              "CPU map returns the original unaligned canonical payload");
        void* overlapping = storage;
        check(map_title_gpu_resource(pin, &overlapping) == HRESULT_FROM_WIN32(ERROR_BUSY) && !overlapping,
              "overlapping CPU maps are explicitly refused");
        check(begin_title_gpu_buffer(pin, copy_list.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE) == HRESULT_FROM_WIN32(ERROR_BUSY),
              "GPU access cannot race a canonical CPU map");
        check(release_title_gpu_resource(alias, copy_fence.Get(), 0) == E_PENDING && alias.resource,
              "mapped canonical memory cannot retire");
        if (!cpu_address) return 1;
        static_cast<UINT32*>(cpu_address)[1] = 0x0badf00d;
        check(unmap_title_gpu_resource(alias) == S_OK && unmap_title_gpu_resource(alias) == E_INVALIDARG,
              "CPU unmap publishes bytes once and rejects a second unmap");
        void* separate_cpu = nullptr;
        check(map_title_gpu_resource(counter_pin, &separate_cpu) == S_OK && separate_cpu == counter,
              "a neighboring suballocation can remain mapped during an independent GPU copy");
        check(begin_title_gpu_buffer(alias, copy_list.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE) == S_OK,
              "overlapping alias can refresh canonical bytes after the prior fence");
        copy_list->CopyBufferRegion(readback.Get(), 128, alias.resource, 0, 28);
        barrier(copy_list.Get(), alias.resource, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
        copy_list->CopyBufferRegion(alias.resource, 24, upload.Get(), 0, 4);
        barrier(copy_list.Get(), alias.resource, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
        check(end_title_gpu_buffer(alias, copy_list.Get()) == S_OK, "copy-queue alias writes back the same canonical range");
        if (FAILED(copy_list->Close())) return 1;
        ID3D12CommandList* copy_lists[]{copy_list.Get()};
        copy_queue->ExecuteCommandLists(1, copy_lists);
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event) return 1;
        hr = copy_queue->Signal(copy_fence.Get(), 1);
        if (SUCCEEDED(hr)) hr = copy_fence->SetEventOnCompletion(1, event);
        const bool copied = SUCCEEDED(hr) && WaitForSingleObject(event, 3000) == WAIT_OBJECT_0;
        CloseHandle(event);
        if (!copied) return 1;
        check(separate_cpu && *static_cast<UINT32*>(separate_cpu) == 1 &&
              unmap_title_gpu_resource(counter_pin) == S_OK,
              "GPU copying a disjoint range preserves the live CPU mapping and its bytes");
        check(complete_title_gpu_resource(alias, copy_fence.Get(), 1) == S_OK && storage[10] == 0xa7a7a7a7,
              "second queue completes its writeback through a real fence");
        hr = readback->Map(0, nullptr, &mapped);
        check(SUCCEEDED(hr) && static_cast<UINT32*>(mapped)[32] == 0xcafebabe && static_cast<UINT32*>(mapped)[33] == 0x0badf00d,
              "alias refresh preserves prior GPU data and the latest CPU edit");
        if (SUCCEEDED(hr)) readback->Unmap(0, nullptr);
        if (FAILED(allocator->Reset()) || FAILED(list->Reset(allocator.Get(), pipeline.Get()))) return 1;
        list->SetDescriptorHeaps(1, heaps); list->SetComputeRootSignature(root.Get());
        list->SetComputeRootDescriptorTable(0, gpu_heap->GetGPUDescriptorHandleForHeapStart());
        check(begin_title_gpu_buffer(pin, list.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS) == S_OK &&
            begin_title_gpu_buffer(counter_pin, list.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS) == S_OK,
            "persistent representations refresh CPU and overlapping-alias changes on reuse");
        list->Dispatch(1, 1, 1);
        check(end_title_gpu_buffer(pin, list.Get()) == S_OK && end_title_gpu_buffer(counter_pin, list.Get()) == S_OK,
              "second append writes data and the persistent counter back");
        if (FAILED(list->Close())) return 1;
        queue->ExecuteCommandLists(1, lists);
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event) return 1;
        hr = queue->Signal(fence.Get(), 6);
        if (SUCCEEDED(hr)) hr = fence->SetEventOnCompletion(6, event);
        const bool reused = SUCCEEDED(hr) && WaitForSingleObject(event, 3000) == WAIT_OBJECT_0;
        CloseHandle(event);
        if (!reused) return 1;
        check(complete_title_gpu_resource(pin, fence.Get(), 6) == S_OK && complete_title_gpu_resource(counter_pin, fence.Get(), 6) == S_OK &&
            storage[5] == 0x0badf00d && storage[10] == 0xa7a7a7a7 && storage[11] == 0xcafebabe && storage[17] == 7 && *counter == 2,
            "repeated append preserves CPU/alias edits and increments the same counter from one to two");
        release(storage, attributes);
        VirtualQuery(storage, &info, sizeof info);
        check(info.State == MEM_COMMIT, "aligned storage owners defer canonical free");
        check(FAILED(acquire_title_gpu_buffer(storage, 4, device.Get(), second, D3D12_RESOURCE_FLAG_NONE, true)),
              "freed canonical allocation refuses new aligned bindings");
        check(release_title_gpu_resource(pin, fence.Get(), 6) == S_OK && release_title_gpu_resource(counter_pin, fence.Get(), 6) == S_OK &&
            release_title_gpu_resource(alias, copy_fence.Get(), 1) == S_OK, "aligned owners retire through their actual queue fences");
        VirtualQuery(storage, &info, sizeof info);
        check(info.State == MEM_FREE, "last aligned owner frees canonical and GPU representations together");
    }
    // Independent reverse-order map, padded rows, raw float payloads, CPU edits
    // and a GPU-authored replacement check both directions without XG reuse.
    for(const auto format:{DXGI_FORMAT_R32_FLOAT,DXGI_FORMAT_B8G8R8A8_UNORM,DXGI_FORMAT_R8_UNORM,DXGI_FORMAT_R8_UINT,DXGI_FORMAT_A8_UNORM}) {
        constexpr UINT width=31,height=17,count=width*height;
        const UINT element=format==DXGI_FORMAT_R32_FLOAT || format==DXGI_FORMAT_B8G8R8A8_UNORM ? 4:1;
        const SIZE_T bytes=count*element+32;
        std::vector<UINT32> offsets(count);
        for(UINT i=0;i<count;++i)offsets[i]=16+(count-1-i)*element;
        D3D12_RESOURCE_DESC image_desc{};image_desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        image_desc.Width=width;image_desc.Height=height;image_desc.DepthOrArraySize=image_desc.MipLevels=1;image_desc.Format=format;image_desc.SampleDesc.Count=1;
        D3D12_HEAP_PROPERTIES image_heap{};image_heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        ComPtr<ID3D12Resource> image;
        if(FAILED(device->CreateCommittedResource(&image_heap,D3D12_HEAP_FLAG_NONE,&image_desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&image))))return 1;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
        device->GetCopyableFootprints(&image_desc,0,1,0,&footprint,nullptr,nullptr,nullptr);
        TextureLayoutTransfer transfer;
        const auto initialized=transfer.initialize(device.Get(),offsets,width,height,footprint,bytes);
        if(initialized!=S_OK)std::cerr<<"layout transfer HRESULT 0x"<<std::hex<<initialized<<std::dec<<'\n';
        check(initialized==S_OK,"exact GPU layout transfer initializes for a bounded validated map");if(initialized!=S_OK)return 1;
        auto invalid=offsets;invalid[1]=invalid[0];TextureLayoutTransfer duplicate;
        check(duplicate.initialize(device.Get(),invalid,width,height,footprint,bytes)==HRESULT_FROM_WIN32(ERROR_INVALID_DATA),"duplicate offsets are rejected before a racy GPU scatter");
        invalid=offsets;invalid[0]=UINT(bytes);TextureLayoutTransfer outside;
        check(outside.initialize(device.Get(),invalid,width,height,footprint,bytes)==HRESULT_FROM_WIN32(ERROR_INVALID_DATA),"GPU layout rejects out-of-range offsets before publication");
        ComPtr<ID3D12Fence> layout_fence;if(FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&layout_fence))))return 1;
        for(UINT iteration=0;iteration<3;++iteration) {
            const bool full_overwrite=iteration==2;
            std::vector<unsigned char> canonical(bytes,static_cast<unsigned char>(0x5a+iteration));std::vector<UINT32> pixels(count);
            for(UINT i=0;i<count;++i){pixels[i]=element==1 ? (i*37+iteration*17)&255u:i==0?0x7fc00001u:i==1?0x80000000u:0xa3b40000u+iteration*count+i;std::memcpy(canonical.data()+offsets[i],&pixels[i],element);}
            if(FAILED(transfer.prepare(canonical.data(),full_overwrite)) || FAILED(allocator->Reset()) || FAILED(list->Reset(allocator.Get(),nullptr)))return 1;
            if(!full_overwrite){transfer.detile(list.Get(),image.Get());barrier(list.Get(),image.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_SOURCE);}
            D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=image.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            to.pResource=readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=footprint;
            if(!full_overwrite)list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
            auto expected=canonical;
            if(iteration) {
                if(FAILED(upload->Map(0,nullptr,&mapped)))return 1;
                for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x){UINT value=element==1 ? ((y*width+x)*53+iteration*7)&255u:0x7f800000u^(y*width+x);std::memcpy(static_cast<unsigned char*>(mapped)+SIZE_T(y)*footprint.Footprint.RowPitch+x*element,&value,element);std::memcpy(expected.data()+offsets[y*width+x],&value,element);if(full_overwrite)pixels[y*width+x]=value;}
                upload->Unmap(0,nullptr);from.pResource=upload.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;from.PlacedFootprint=footprint;
                to.pResource=image.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;to.SubresourceIndex=0;
                if(!full_overwrite)barrier(list.Get(),image.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
                list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
                barrier(list.Get(),image.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_SOURCE);
                if(full_overwrite){from.pResource=image.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;from.SubresourceIndex=0;
                    to.pResource=readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=footprint;list->CopyTextureRegion(&to,0,0,0,&from,nullptr);}
            }
            transfer.retile(list.Get(),image.Get());barrier(list.Get(),image.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
            if(FAILED(list->Close()))return 1;queue->ExecuteCommandLists(1,lists);
            HANDLE ready=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!ready)return 1;
            hr=queue->Signal(layout_fence.Get(),iteration+1);if(SUCCEEDED(hr))hr=layout_fence->SetEventOnCompletion(iteration+1,ready);
            const bool done=SUCCEEDED(hr)&&WaitForSingleObject(ready,3000)==WAIT_OBJECT_0;CloseHandle(ready);if(!done)return 1;
            if(FAILED(readback->Map(0,nullptr,&mapped)))return 1;bool exact=true;
            for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x){UINT value=0;std::memcpy(&value,static_cast<unsigned char*>(mapped)+SIZE_T(y)*footprint.Footprint.RowPitch+x*element,element);exact=exact&&value==pixels[y*width+x];}
            readback->Unmap(0,nullptr);check(exact,"GPU detile preserves all raw texel bits and refreshes CPU changes");
            if(FAILED(transfer.readback()->Map(0,nullptr,&mapped)))return 1;
            check(!std::memcmp(mapped,expected.data(),bytes),"GPU retile preserves raw GPU writes and every canonical padding/guard byte");transfer.readback()->Unmap(0,nullptr);
        }
    }
    const auto texture_fixture = [&](TitleTextureLayout layout, UINT block, UINT bytes_per_block, UINT64 signal,
                                     LinearTextureFixture* injected, bool packed_uav=false) -> bool {
        const auto& d = layout.resource_desc;
        auto* storage = static_cast<unsigned char*>(allocate(layout.bytes + layout.alignment + 32, attributes));
        if (!storage) return false;
        std::memset(storage, 0x5a, layout.bytes + layout.alignment + 32);
        const auto aligned = (reinterpret_cast<std::uintptr_t>(storage) + 16 + layout.alignment - 1) & ~(layout.alignment - 1);
        auto* canonical = reinterpret_cast<unsigned char*>(aligned);
        const UINT mips = d.MipLevels;
        const UINT mutation_mip = mips > 1 ? 1 : 0;
        std::vector<std::vector<unsigned char>> expected(mips);
        for (UINT mip = 0; mip < mips; ++mip) {
            const UINT columns = ((std::max)(1u, static_cast<UINT>(d.Width) >> mip) + block - 1) / block;
            const UINT rows = ((std::max)(1u, d.Height >> mip) + block - 1) / block;
            const UINT depth = d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D ? (std::max)(1u, UINT(d.DepthOrArraySize) >> mip) : 1;
            expected[mip].resize(columns * rows * depth * bytes_per_block);
            for (UINT i = 0; i < expected[mip].size(); ++i) expected[mip][i] = static_cast<unsigned char>(0x39 + mip * 11 + i % 17);
            if (FAILED(layout.copy(layout.context.get(), true, expected[mip].data(), canonical, mip,
                columns * bytes_per_block, columns * rows * bytes_per_block,nullptr))) return false;
        }
        const std::vector<unsigned char> preimage(canonical, canonical + layout.bytes);
        TitleGpuResource texture, alias;
        HRESULT hr = acquire_title_gpu_texture(canonical, layout, device.Get(), texture);
        std::cout << "texture binding format=" << d.Format << " bytes=" << layout.bytes << " hr=0x" << std::hex << hr << std::dec << '\n';
        if (FAILED(hr)) return false;
        hr = acquire_title_gpu_buffer(canonical, layout.bytes, device.Get(), alias, D3D12_RESOURCE_FLAG_NONE, true);
        if (FAILED(hr)) return false;
        check(texture.resource != alias.resource && texture.canonical_offset == alias.canonical_offset,
              "texture and buffer representations retain the same canonical range independently");
        if (FAILED(allocator->Reset()) || FAILED(list->Reset(allocator.Get(), nullptr))) return false;
        check(begin_title_gpu_buffer(texture, list.Get(), D3D12_RESOURCE_STATE_COPY_DEST) == E_INVALIDARG,
              "texture cannot bypass its layout conversion through the buffer path");
        const bool render_target = (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) != 0;
        const bool depth_target = (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) != 0;
        const bool integer_target=!render_target && !depth_target && (d.Flags&D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) &&
            (d.Format==DXGI_FORMAT_R8_TYPELESS || d.Format==DXGI_FORMAT_R10G10B10A2_TYPELESS);
        hr = begin_title_gpu_texture(texture, list.Get(), depth_target ? D3D12_RESOURCE_STATE_DEPTH_WRITE :
            render_target ? D3D12_RESOURCE_STATE_RENDER_TARGET : integer_target ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_COPY_DEST);
        if (FAILED(hr)) return false;
        check(begin_title_gpu_buffer(alias, list.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE) == HRESULT_FROM_WIN32(ERROR_BUSY),
              "buffer alias cannot race a native texture using the same canonical bytes");
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT mutation{};
        UINT rows = 0; UINT64 row_bytes = 0, total = 0;
        device->GetCopyableFootprints(&d, mutation_mip, 1, 0, &mutation, &rows, &row_bytes, &total);
        void* data = nullptr;
        if (FAILED(upload->Map(0, nullptr, &data))) return false;
        check(UINT64(mutation.Footprint.RowPitch) * rows * mutation.Footprint.Depth <= upload->GetDesc().Width,
              "independent mutation fits its bounded upload fixture");
        if (UINT64(mutation.Footprint.RowPitch) * rows * mutation.Footprint.Depth > upload->GetDesc().Width) return false;
        std::memset(data, 0xa7, mutation.Footprint.RowPitch * rows * mutation.Footprint.Depth);
        upload->Unmap(0, nullptr);
        D3D12_TEXTURE_COPY_LOCATION source{};
        source.pResource = upload.Get(); source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; source.PlacedFootprint = mutation;
        D3D12_TEXTURE_COPY_LOCATION destination{};
        destination.pResource = texture.resource; destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; destination.SubresourceIndex = mutation_mip;
        ComPtr<ID3D12DescriptorHeap> render_heap, clear_gpu_heap;
        if (depth_target) {
            const D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_DSV,1,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};
            if (FAILED(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&render_heap)))) return false;
            D3D12_DEPTH_STENCIL_VIEW_DESC view{};
            view.Format = d.Format==DXGI_FORMAT_R16_TYPELESS ? DXGI_FORMAT_D16_UNORM : DXGI_FORMAT_D32_FLOAT; view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
            view.Texture2D.MipSlice = mutation_mip;
            device->CreateDepthStencilView(texture.resource,&view,render_heap->GetCPUDescriptorHandleForHeapStart());
            list->ClearDepthStencilView(render_heap->GetCPUDescriptorHandleForHeapStart(),D3D12_CLEAR_FLAG_DEPTH,.25f,0,0,nullptr);
        } else if (render_target) {
            const D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
            if (FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&render_heap)))) return false;
            D3D12_RENDER_TARGET_VIEW_DESC view{};
            view.Format = d.Format==DXGI_FORMAT_R8_TYPELESS ? DXGI_FORMAT_R8_UNORM :
                d.Format==DXGI_FORMAT_R10G10B10A2_TYPELESS ? DXGI_FORMAT_R10G10B10A2_UNORM : d.Format;
            view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D; view.Texture2D.MipSlice = mutation_mip;
            device->CreateRenderTargetView(texture.resource, &view, render_heap->GetCPUDescriptorHandleForHeapStart());
            constexpr UINT packed = 0xa7a7a7a7;
            float color[]{float(packed & 1023) / 1023, float((packed >> 10) & 1023) / 1023,
                float((packed >> 20) & 1023) / 1023, float(packed >> 30) / 3};
            if (d.Format == DXGI_FORMAT_B8G8R8A8_UNORM || d.Format==DXGI_FORMAT_R8_TYPELESS) std::fill(std::begin(color),std::end(color),167.f/255.f);
            list->ClearRenderTargetView(render_heap->GetCPUDescriptorHandleForHeapStart(), color, 0, nullptr);
        } else if (integer_target) {
            D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,1,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};
            if (FAILED(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&render_heap)))) return false;
            heap.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            if (FAILED(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&clear_gpu_heap)))) return false;
            D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
            view.Format=packed_uav ? DXGI_FORMAT_R32_UINT : d.Format==DXGI_FORMAT_R8_TYPELESS ? DXGI_FORMAT_R8_UINT : DXGI_FORMAT_R10G10B10A2_UINT;
            view.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D; view.Texture2D.MipSlice=mutation_mip;
            device->CreateUnorderedAccessView(texture.resource,nullptr,&view,render_heap->GetCPUDescriptorHandleForHeapStart());
            device->CopyDescriptorsSimple(1,clear_gpu_heap->GetCPUDescriptorHandleForHeapStart(),render_heap->GetCPUDescriptorHandleForHeapStart(),heap.Type);
            auto* bound_heap=clear_gpu_heap.Get(); list->SetDescriptorHeaps(1,&bound_heap);
            constexpr UINT packed=0xa7a7a7a7;
            const bool narrow=view.Format==DXGI_FORMAT_R8_UINT;
            const UINT values[]{packed_uav ? packed : narrow ? 167u : packed&1023,narrow ? 167u : (packed>>10)&1023,narrow ? 167u : (packed>>20)&1023,narrow ? 167u : packed>>30};
            list->ClearUnorderedAccessViewUint(clear_gpu_heap->GetGPUDescriptorHandleForHeapStart(),render_heap->GetCPUDescriptorHandleForHeapStart(),texture.resource,values,0,nullptr);
        } else list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        std::fill(expected[mutation_mip].begin(), expected[mutation_mip].end(), 0xa7);
        if (depth_target) {
            const float cleared = .25f;
            const UINT16 d16=16384;
            const bool narrow=d.Format==DXGI_FORMAT_R16_TYPELESS;
            for (SIZE_T offset=0;offset<expected[mutation_mip].size();offset+=narrow ? 2 : 4)
                std::memcpy(expected[mutation_mip].data()+offset,narrow ? static_cast<const void*>(&d16) : &cleared,narrow ? 2 : 4);
        }
        if (FAILED(end_title_gpu_texture(texture, list.Get())) || FAILED(list->Close())) return false;
        queue->ExecuteCommandLists(1, lists);
        check(complete_title_gpu_resource(texture, fence.Get(), signal) == E_PENDING,
              "texture completion requires its actual GPU readback fence");
        const auto wait_for = [&](UINT64 value) {
            HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!ready) return false;
            HRESULT done = queue->Signal(fence.Get(), value);
            if (SUCCEEDED(done)) done = fence->SetEventOnCompletion(value, ready);
            const bool ok = SUCCEEDED(done) && WaitForSingleObject(ready, 3000) == WAIT_OBJECT_0;
            CloseHandle(ready); return ok;
        };
        if (!wait_for(signal)) return false;
        check(release_title_gpu_resource(texture, fence.Get(), signal) == E_PENDING,
              "completed GPU copy alone cannot retire a texture before canonical writeback");
        if (injected) {
            injected->fail_writeback = true;
            check(complete_title_gpu_resource(texture, fence.Get(), signal) == E_FAIL &&
                !std::memcmp(canonical, preimage.data(), preimage.size()), "failed retile preserves the complete canonical preimage");
            void* unavailable = nullptr;
            check(map_title_gpu_resource(alias, &unavailable) == E_PENDING && !unavailable,
                  "failed texture conversion retains the ownership gate for retry");
            injected->fail_writeback = false;
        }
        if (FAILED(complete_title_gpu_resource(texture, fence.Get(), signal))) return false;
        check(release_title_gpu_resource(alias, fence.Get(), signal) == S_OK,
              "retiring one representation does not erase its texture alias at the same range");
        const auto matches = [&]() {
            bool same = true;
            for (UINT mip = 0; mip < mips; ++mip) {
                const UINT columns = ((std::max)(1u, static_cast<UINT>(d.Width) >> mip) + block - 1) / block;
                const UINT rows = ((std::max)(1u, d.Height >> mip) + block - 1) / block;
                std::vector<unsigned char> actual(expected[mip].size());
                const HRESULT copied = layout.copy(layout.context.get(), false, actual.data(), canonical, mip,
                    columns * bytes_per_block, columns * rows * bytes_per_block,nullptr);
                same = same && SUCCEEDED(copied) && actual == expected[mip];
            }
            for (unsigned i = 0; i < 16; ++i) same = same && canonical[int(i) - 16] == 0x5a && canonical[layout.bytes + i] == 0x5a;
            return same;
        };
        check(matches(), "all texture mips reflect the independent GPU mutation while canonical neighbors stay intact");
        void* cpu = nullptr;
        if (FAILED(map_title_gpu_resource(texture, &cpu)) || cpu != canonical) return false;
        std::fill(expected.back().begin(), expected.back().end(), 0x66);
        const UINT last_columns = ((std::max)(1u, static_cast<UINT>(d.Width) >> (mips - 1)) + block - 1) / block;
        const UINT last_rows = ((std::max)(1u, d.Height >> (mips - 1)) + block - 1) / block;
        hr = layout.copy(layout.context.get(), true, expected.back().data(), canonical, mips - 1,
            last_columns * bytes_per_block, last_columns * last_rows * bytes_per_block,nullptr);
        if (FAILED(unmap_title_gpu_resource(texture)) || FAILED(hr)) return false;
        const std::vector<unsigned char> read_preimage(canonical,canonical+layout.bytes);
        if (FAILED(allocator->Reset()) || FAILED(list->Reset(allocator.Get(), nullptr))) return false;
        check(begin_title_gpu_texture(texture,list.Get(),D3D12_RESOURCE_STATE_COPY_DEST,false,nullptr,true)==E_INVALIDARG,
              "read-only intent rejects a writable GPU state without claiming the resource");
        if (FAILED(begin_title_gpu_texture(texture, list.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,false,nullptr,true)) ||
            FAILED(end_title_gpu_texture(texture, list.Get())) || FAILED(list->Close())) return false;
        queue->ExecuteCommandLists(1, lists);
        check(complete_title_gpu_resource(texture,fence.Get(),signal+1)==E_PENDING,
              "read-only texture access still requires its actual GPU fence");
        if(injected)injected->fail_writeback=true;
        hr=wait_for(signal+1) ? complete_title_gpu_resource(texture,fence.Get(),signal+1) : E_FAIL;
        if(injected)injected->fail_writeback=false;
        if(FAILED(hr))return false;
        check(!std::memcmp(canonical,read_preimage.data(),read_preimage.size()),
              "read-only texture use preserves every canonical byte without invoking writeback");
        check(matches(), "texture reuse refreshes canonical CPU changes and preserves previous GPU data");
        ComPtr<ID3D12Fence> final_fence=fence;UINT64 final_value=signal+1;
        const UINT32* verified=nullptr;SIZE_T verified_count=0;
        if(layout.offsets && layout.offsets(layout.context.get(),&verified,&verified_count)==S_OK) {
            ComPtr<ID3D12Fence> converted_fence;
            if(FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&converted_fence))) ||
                FAILED(allocator->Reset()) || FAILED(list->Reset(allocator.Get(),nullptr)) ||
                FAILED(begin_title_gpu_texture(texture,list.Get(),D3D12_RESOURCE_STATE_COPY_DEST)))return false;
            if(FAILED(upload->Map(0,nullptr,&data)))return false;
            std::memset(data,0x3c,mutation.Footprint.RowPitch*rows);upload->Unmap(0,nullptr);
            list->CopyTextureRegion(&destination,0,0,0,&source,nullptr);
            if(FAILED(end_title_gpu_texture(texture,list.Get())) || FAILED(list->Close()))return false;
            queue->ExecuteCommandLists(1,lists);
            check(complete_title_gpu_resource(texture,converted_fence.Get(),1)==E_PENDING,"GPU layout conversion cannot publish before its real fence");
            HANDLE ready=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!ready)return false;
            hr=queue->Signal(converted_fence.Get(),1);if(SUCCEEDED(hr))hr=converted_fence->SetEventOnCompletion(1,ready);
            const bool done=SUCCEEDED(hr)&&WaitForSingleObject(ready,3000)==WAIT_OBJECT_0;CloseHandle(ready);
            if(!done || FAILED(complete_title_gpu_resource(texture,converted_fence.Get(),1)))return false;
            std::fill(expected[0].begin(),expected[0].end(),0x3c);
            check(matches(),"verified original-XG layout uses GPU conversion with identical canonical texels and guards");
            final_fence=converted_fence;final_value=1;
        }
        layout.context.reset(); // The registry now owns the last original-computer reference in the XG case.
        check(release_title_gpu_resource(texture, final_fence.Get(), final_value) == S_OK, "texture ownership retires after conversion and its fence");
        release(storage, attributes);
        return true;
    };
    TitleTextureLayout linear_layout;
    auto linear_context = std::make_shared<LinearTextureFixture>();
    linear_layout.context = linear_context; linear_layout.copy = &linear_texture_copy;
    linear_layout.bytes = 168; linear_layout.alignment = 16;
    linear_layout.resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    linear_layout.resource_desc.Width = 8; linear_layout.resource_desc.Height = 4;
    linear_layout.resource_desc.DepthOrArraySize = 1; linear_layout.resource_desc.MipLevels = 3;
    linear_layout.resource_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; linear_layout.resource_desc.SampleDesc.Count = 1;
    check(texture_fixture(linear_layout, 1, 4, 7, linear_context.get()), "owned linear texture fixture completes");
    if (original_xg && bgra_only) {
        TitleTextureLayout layout;
        hr = create_xg_texture_layout(32,16,1,1,DXGI_FORMAT_B8G8R8A8_UNORM,UINT_MAX,layout,
            D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);
        check(SUCCEEDED(hr) && texture_fixture(std::move(layout),1,4,9,nullptr),
            "original 2018 XG BGRA backbuffer layout preserves independent GPU clears, canonical guards and CPU reuse");
        hr = create_xg_texture_layout(32,16,1,1,DXGI_FORMAT_D32_FLOAT,UINT_MAX,layout,D3D11_BIND_DEPTH_STENCIL);
        check(SUCCEEDED(hr) && texture_fixture(std::move(layout),1,4,11,nullptr),
            "expanded D32 image preserves GPU depth clear bits, aliases, guards, fences and CPU reuse");
        hr = create_xg_texture_layout(32,16,1,1,DXGI_FORMAT_B5G6R5_UNORM,UINT_MAX,layout,D3D11_BIND_SHADER_RESOURCE);
        check(SUCCEEDED(hr) && texture_fixture(std::move(layout),1,2,13,nullptr),
            "original RGB565 layout preserves all packed bits through native GPU copies and canonical CPU reuse");
    } else if (original_xg) {
        TitleTextureLayout layout;
        hr = create_xg_texture_layout(64, 64, 7, 1, DXGI_FORMAT_BC7_UNORM_SRGB, 14, layout);
        check(SUCCEEDED(hr) && texture_fixture(std::move(layout), 4, 16, 9, nullptr), "original XG BC7 texture completes the real GPU coherence path");
        hr = create_xg_texture3d_layout(16, 16, 16, 5, DXGI_FORMAT_R10G10B10A2_UNORM,
            D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, layout);
        check(SUCCEEDED(hr) && texture_fixture(std::move(layout), 1, 4, 11, nullptr),
              "original XG volume preserves every depth slice through independent GPU writes and canonical reuse");
        hr = create_xg_texture_layout(32, 16, 1, 1, DXGI_FORMAT_R10G10B10A2_UNORM, UINT_MAX, layout,
            D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET);
        check(SUCCEEDED(hr) && texture_fixture(std::move(layout), 1, 4, 13, nullptr),
              "expanded render-target preserves real GPU clear bytes, canonical guards and CPU reuse");
        hr = create_xg_texture_layout(32, 16, 1, 1, DXGI_FORMAT_R8_UINT, UINT_MAX, layout, D3D11_BIND_SHADER_RESOURCE);
        check(SUCCEEDED(hr) && texture_fixture(std::move(layout), 1, 1, 15, nullptr),
              "typed R8 metadata preserves canonical bytes through independent GPU mutation");
        hr=create_xg_texture_layout(32,16,1,1,DXGI_FORMAT_R32_TYPELESS,UINT_MAX,layout,
            D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE);
        check(SUCCEEDED(hr) && texture_fixture(std::move(layout),1,4,17,nullptr),
            "original single-sample depth preserves bits, expanded HTILE, GPU clears and canonical reuse");
    }
    const auto sample_fixture = [&](TitleTextureLayout layout, UINT64 signal, void* reserved = nullptr) -> bool {
        const bool depth_stencil=layout.resource_desc.Format==DXGI_FORMAT_R32G8X24_TYPELESS;
        const bool half_float=layout.resource_desc.Format==DXGI_FORMAT_R16G16B16A16_TYPELESS;
        const bool depth_only=layout.resource_desc.Format==DXGI_FORMAT_R32_TYPELESS;
        const bool depth_target=depth_stencil || depth_only;
        if (depth_stencil) {
            D3D12_FEATURE_DATA_D3D12_OPTIONS options{};
            const HRESULT feature=device->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS,&options,sizeof options);
            std::cout << "depth/stencil features hr=0x" << std::hex << feature << std::dec
                      << " PSStencilRef=" << options.PSSpecifiedStencilRefSupported << '\n';
        }
        const UINT width=UINT(layout.resource_desc.Width),height=layout.resource_desc.Height;
        const UINT words=depth_stencil || half_float ? 2 : 1,row=width*4*words*4,slice=row*height;
        auto* storage = reserved ? nullptr : static_cast<unsigned char*>(allocate(layout.bytes + layout.alignment + 32, attributes));
        if (!reserved && !storage) return false;
        auto* canonical = reserved ? static_cast<unsigned char*>(reserved) :
            reinterpret_cast<unsigned char*>((reinterpret_cast<std::uintptr_t>(storage) + 16 + layout.alignment - 1) & ~(layout.alignment - 1));
        std::memset(canonical - 16, 0x5a, layout.bytes + 32);
        std::vector<UINT32> expected(SIZE_T(width)*height*4*words), actual(expected.size());
        for (UINT i = 0; i < expected.size(); ++i) {
            const UINT sample = (i/words) % 4;
            const UINT32 depth_value=0x3e000000u+(i%65536u)*13;
            expected[i] = depth_stencil ? (i%2 ? ((i/2*17+3)&255) : depth_value) : depth_only ? depth_value :
                ((15 + sample) << 6) | (((16 + sample) << 6) << 11) | (((14 + sample) << 5) << 22);
        }
        if (depth_target) {
            const UINT32 edges[]{0,0x00800000u,0x3f7fffffu,0x3f800000u};
            for (UINT sample=0;sample<4;++sample) expected[sample*words]=edges[sample];
        }
        if (half_float) {
            for (UINT i=0;i<expected.size();++i) expected[i]=((i*13)&65535)|(((i*17+3)&65535)<<16);
            const UINT16 edges[]{0,0x8000,1,0x3ff,0x400,0x3c00,0x7bff,0x7c00,0xfc00,0x7e00,0x7fff,0xfe01,0x3555,0xb555,0x401,0x83ff};
            std::memcpy(expected.data(),edges,sizeof edges);
        }
        if (FAILED(layout.copy(layout.context.get(), true, expected.data(), canonical, 0, row, slice,nullptr))) return false;
        TitleGpuResource texture;
        HRESULT hr = acquire_title_gpu_texture(canonical, layout, device.Get(), texture);
        check(SUCCEEDED(hr), "four-sample resource acquires a real native MSAA texture");
        if (FAILED(hr)) return false;
        check(texture.resource->GetDesc().SampleDesc.Count == 4, "MSAA storage retains all four hardware samples");
        check(texture.resource->GetDesc().SampleDesc.Quality==layout.resource_desc.SampleDesc.Quality,
            "MSAA materialization preserves the selected native sample pattern");
        if(depth_target) {
            TitleGpuResource alias;
            check(!texture.clear.known && SUCCEEDED(acquire_title_gpu_texture(canonical,layout,device.Get(),alias)),
                "new logical depth resources have explicit unknown clear state even when physical storage is shared");
            if(!alias.resource) return false;
            check(set_title_gpu_clear(texture,1,0x3e800000,0)==S_OK && !alias.clear.known &&
                set_title_gpu_clear(alias,3,0x3f400000,0xa7)==S_OK && texture.clear.depth==0x3e800000 && texture.clear.known==1,
                "physical aliases retain independent full-precision depth/stencil clear registers");
            check(set_title_gpu_clear(texture,2,0,0x27)==S_OK && texture.clear.depth==0x3e800000 && texture.clear.stencil==0x27 &&
                set_title_gpu_clear(texture,1,0x7fc00001,0)==E_INVALIDARG && texture.clear.depth==0x3e800000,
                "partial clear state preserves the other aspect and rejects NaN without changing known state");
            if(FAILED(release_title_gpu_resource(alias,fence.Get(),fence->GetCompletedValue()))) return false;
            check(!alias.clear.known,"released logical depth resource discards its clear register ownership");
        }
        ComPtr<ID3D12DescriptorHeap> targets;
        const D3D12_DESCRIPTOR_HEAP_DESC heap{depth_target ? D3D12_DESCRIPTOR_HEAP_TYPE_DSV : D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        if (FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&targets)))) return false;
        D3D12_RENDER_TARGET_VIEW_DESC view{};
        view.Format = half_float ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R11G11B10_FLOAT; view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
        if (depth_target) {
            D3D12_DEPTH_STENCIL_VIEW_DESC depth_view{};
            depth_view.Format=depth_stencil ? DXGI_FORMAT_D32_FLOAT_S8X24_UINT : DXGI_FORMAT_D32_FLOAT; depth_view.ViewDimension=D3D12_DSV_DIMENSION_TEXTURE2DMS;
            device->CreateDepthStencilView(texture.resource,&depth_view,targets->GetCPUDescriptorHandleForHeapStart());
        } else device->CreateRenderTargetView(texture.resource, &view, targets->GetCPUDescriptorHandleForHeapStart());
        const UINT iterations=original_xg && depth_target && !depth_stencil && layout.guest_descriptor[0] ? 4 : 3;
        for (UINT iteration = 0; iteration < iterations; ++iteration) {
            if (iteration == 2) {
                void* cpu = nullptr;
                if (FAILED(map_title_gpu_resource(texture, &cpu))) return false;
                if(depth_target) check(set_title_gpu_clear(texture,1,0,0)==HRESULT_FROM_WIN32(ERROR_BUSY),
                    "CPU ownership excludes concurrent clear-register publication");
                for (UINT i = 0; i < expected.size(); ++i) if (!depth_target || i>=4*words || (depth_stencil && i%2))
                    expected[i] ^= ((i/words) % 4 + 1) << 1; // Distinct finite mantissas in each sample.
                hr = layout.copy(layout.context.get(), true, expected.data(), cpu, 0, row, slice,nullptr);
                if (FAILED(unmap_title_gpu_resource(texture)) || FAILED(hr)) return false;
            }
            if(iteration==3) {
                void* cpu=nullptr;
                if(FAILED(map_title_gpu_resource(texture,&cpu))) return false;
                std::fill(expected.begin(),expected.end(),0x3f000000u);
                hr=layout.copy(layout.context.get(),true,expected.data(),cpu,0,row,slice,nullptr);
                const UINT64 depth_base=UINT64(layout.guest_descriptor[0])<<8;
                if(FAILED(hr) || !depth_base || depth_base>=layout.bytes) {unmap_title_gpu_resource(texture);return false;}
                std::memset(cpu,0,SIZE_T(depth_base)); // Native XG depth SRD bounds the preceding HTILE plane.
                if(FAILED(unmap_title_gpu_resource(texture)) || FAILED(hr) || FAILED(set_title_gpu_clear(texture,1,0,0))) return false;
                std::fill(expected.begin(),expected.end(),0);
            }
            if (FAILED(allocator->Reset()) || FAILED(list->Reset(allocator.Get(), nullptr))) return false;
            if(iteration==3) {
                TitleGpuResource incompatible;
                if(FAILED(acquire_title_gpu_texture(canonical,layout,device.Get(),incompatible)) ||
                    FAILED(set_title_gpu_clear(incompatible,1,0x3f800000,0))) return false;
                check(begin_title_gpu_texture(incompatible,list.Get(),D3D12_RESOURCE_STATE_DEPTH_WRITE)==DXGI_ERROR_UNSUPPORTED,
                    "shared physical storage cannot reuse a different logical resource's fast-clear register");
                if(FAILED(release_title_gpu_resource(incompatible,fence.Get(),fence->GetCompletedValue())) || FAILED(list->Close()) ||
                    FAILED(allocator->Reset()) || FAILED(list->Reset(allocator.Get(),nullptr))) return false;
            }
            hr = begin_title_gpu_texture(texture, list.Get(), depth_target ? D3D12_RESOURCE_STATE_DEPTH_WRITE : D3D12_RESOURCE_STATE_RENDER_TARGET);
            if (FAILED(hr)) std::cerr << "MSAA upload format=" << layout.resource_desc.Format << " hr=0x" << std::hex << hr << std::dec << '\n';
            check(SUCCEEDED(hr), "sample upload uses per-sample GPU shading without resolve");
            if(depth_target && SUCCEEDED(hr)) check(set_title_gpu_clear(texture,1,0,0)==E_PENDING,
                "in-flight GPU ownership excludes premature clear-register changes");
            if (FAILED(hr)) return false;
            if (iteration == 1) {
                const float color[]{.5f, 1, 2, 1};
                if (depth_target) {
                    list->ClearDepthStencilView(targets->GetCPUDescriptorHandleForHeapStart(),depth_stencil ? D3D12_CLEAR_FLAG_DEPTH|D3D12_CLEAR_FLAG_STENCIL : D3D12_CLEAR_FLAG_DEPTH,.375f,0xa7,0,nullptr);
                    for (UINT i=0;i<expected.size();++i) expected[i]=depth_stencil && i%2 ? 0xa7 : 0x3ec00000u;
                } else {
                    list->ClearRenderTargetView(targets->GetCPUDescriptorHandleForHeapStart(), color, 0, nullptr);
                    if (half_float) for (UINT i=0;i<expected.size();++i) expected[i]=i%2 ? 0x3c004000u : 0x3c003800u;
                    else std::fill(expected.begin(), expected.end(), (14u << 6) | ((15u << 6) << 11) | ((16u << 5) << 22));
                }
            }
            if (FAILED(end_title_gpu_texture(texture, list.Get())) || FAILED(list->Close())) return false;
            queue->ExecuteCommandLists(1, lists);
            check(complete_title_gpu_resource(texture, fence.Get(), signal + iteration) == E_PENDING,
                  "MSAA CPU writeback waits for the real GPU fence");
            HANDLE ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!ready) return false;
            hr = queue->Signal(fence.Get(), signal + iteration);
            if (SUCCEEDED(hr)) hr = fence->SetEventOnCompletion(signal + iteration, ready);
            const bool completed = SUCCEEDED(hr) && WaitForSingleObject(ready, 3000) == WAIT_OBJECT_0;
            CloseHandle(ready); if (!completed || FAILED(complete_title_gpu_resource(texture, fence.Get(), signal + iteration))) return false;
            hr = layout.copy(layout.context.get(), false, actual.data(), canonical, 0, row, slice,nullptr);
            check(SUCCEEDED(hr) && actual == expected, iteration == 1 ? "independent native MSAA clear updates every canonical sample" :
                "all four distinct MSAA samples survive GPU upload/readback and CPU reuse");
            for (UINT i = 0; i < 16; ++i)
                check(canonical[int(i) - 16] == 0x5a && canonical[layout.bytes + i] == 0x5a, "MSAA coherence preserves neighboring bytes");
        }
        if (depth_target) {
            const std::vector<unsigned char> before(canonical,canonical+layout.bytes);
            void* cpu=nullptr;
            if (FAILED(map_title_gpu_resource(texture,&cpu))) return false;
            const auto valid=expected;
            expected[0]=0x7fc00001u;
            hr=layout.copy(layout.context.get(),true,expected.data(),cpu,0,row,slice,nullptr);
            if (FAILED(unmap_title_gpu_resource(texture)) || FAILED(hr)) return false;
            if (FAILED(allocator->Reset()) || FAILED(list->Reset(allocator.Get(),nullptr))) return false;
            check(begin_title_gpu_texture(texture,list.Get(),D3D12_RESOURCE_STATE_DEPTH_WRITE)==DXGI_ERROR_UNSUPPORTED,
                "MSAA depth rejects unrepresentable NaN payloads before submitting GPU work");
            if (FAILED(list->Close())) return false;
            check(map_title_gpu_resource(texture,&cpu)==S_OK && cpu==canonical,"failed MSAA depth upload releases CPU and physical alias gates");
            expected=valid;
            if (!cpu) return false;
            hr=layout.copy(layout.context.get(),true,expected.data(),cpu,0,row,slice,nullptr);
            if (FAILED(unmap_title_gpu_resource(texture)) || FAILED(hr)) return false;
            check(!std::memcmp(canonical,before.data(),before.size()),"failed MSAA upload can restore every canonical plane without stale GPU data");
        }
        if (FAILED(release_title_gpu_resource(texture, fence.Get(), signal + iterations-1))) return false;
        if (storage) release(storage, attributes);
        return true;
    };
    TitleTextureLayout sample_layout;
    sample_layout.bytes = 8192; sample_layout.alignment = 16;
    sample_layout.context = std::make_shared<int>(0); sample_layout.copy = &linear_sample_copy;
    sample_layout.resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    sample_layout.resource_desc.Width = 32; sample_layout.resource_desc.Height = 16;
    sample_layout.resource_desc.DepthOrArraySize = sample_layout.resource_desc.MipLevels = 1;
    sample_layout.resource_desc.Format = DXGI_FORMAT_R11G11B10_FLOAT; sample_layout.resource_desc.SampleDesc.Count = 4;
    sample_layout.resource_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    check(sample_fixture(sample_layout, 20), "owned four-sample texture completes GPU coherence");
    if (original_xg && !bgra_only) {
        hr = create_xg_texture_layout(32, 16, 1, 1, DXGI_FORMAT_R11G11B10_FLOAT, UINT_MAX, sample_layout,
            D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET, 4);
        check(SUCCEEDED(hr) && sample_layout.resource_desc.SampleDesc.Quality==UINT_MAX,
            "reference native MSAA policy selects the standard sample pattern without reducing samples");
        check(SUCCEEDED(hr) && sample_fixture(std::move(sample_layout), 30), "original XG MSAA layout completes per-sample GPU coherence");
        hr=create_xg_texture_layout(32,16,1,1,DXGI_FORMAT_R32G8X24_TYPELESS,1,sample_layout,
            D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE,4);
        check(SUCCEEDED(hr) && sample_fixture(std::move(sample_layout),33),
            "original XG depth/stencil preserves all sample bits through native GPU transfers and independent clears");
        hr=create_xg_texture_layout(32,16,1,1,DXGI_FORMAT_R16G16B16A16_FLOAT,UINT_MAX,sample_layout,
            D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE,4);
        check(SUCCEEDED(hr) && sample_fixture(std::move(sample_layout),36),
            "half-float MSAA preserves raw NaNs, signed zeros, subnormals and samples through integer transport and real float clears");
        hr=create_xg_texture_layout(32,16,1,1,DXGI_FORMAT_R32_TYPELESS,UINT_MAX,sample_layout,
            D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE,4);
        check(SUCCEEDED(hr) && sample_fixture(std::move(sample_layout),39),
            "D32 MSAA preserves four depth samples, independent clears and CPU reuse without requiring a stencil plane");
        TitleTextureLayout typed_layout;
        hr=create_xg_texture_layout(32,16,1,1,DXGI_FORMAT_R16_TYPELESS,UINT_MAX,typed_layout,D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE);
        check(SUCCEEDED(hr) && texture_fixture(std::move(typed_layout),1,2,45,nullptr),
            "D16 depth preserves packed bits, native clear quantization and canonical reuse");
        UINT64 typed_signal=48;
        for (const auto format:{DXGI_FORMAT_R8_TYPELESS,DXGI_FORMAT_R10G10B10A2_TYPELESS}) for (const UINT bind:{UINT(D3D11_BIND_RENDER_TARGET),UINT(D3D11_BIND_UNORDERED_ACCESS)}) {
            hr=create_xg_texture_layout(32,16,1,1,format,UINT_MAX,typed_layout,D3D11_BIND_SHADER_RESOURCE|bind);
            check(SUCCEEDED(hr) && texture_fixture(std::move(typed_layout),1,format==DXGI_FORMAT_R8_TYPELESS ? 1 : 4,typed_signal,nullptr),
                "typeless color preserves native typed RTV/UAV writes and aliases through canonical reuse");
            typed_signal+=3;
        }
        hr=create_xg_texture_layout(32,16,1,1,DXGI_FORMAT_R10G10B10A2_TYPELESS,UINT_MAX,typed_layout,D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS);
        check(SUCCEEDED(hr) && texture_fixture(std::move(typed_layout),1,4,typed_signal,nullptr,true),
            "packed R32_UINT UAV writes preserve all R10G10B10A2 bits through canonical reuse and aliases");
        hr=create_xg_texture_layout(32,16,1,1,DXGI_FORMAT_R16G16_FLOAT,UINT_MAX,typed_layout,D3D11_BIND_SHADER_RESOURCE);
        check(SUCCEEDED(hr) && texture_fixture(std::move(typed_layout),1,4,typed_signal+3,nullptr),
            "two-channel half-float preserves all packed bits through original XG and GPU copies");
        TitleTextureLayout cube_layout;
        hr=create_xg_texture_layout(8,8,2,6,DXGI_FORMAT_R8G8B8A8_UNORM,UINT_MAX,cube_layout,D3D11_BIND_SHADER_RESOURCE);
        if (FAILED(hr)) return 1;
        auto* cube_storage=static_cast<unsigned char*>(allocate(cube_layout.bytes+cube_layout.alignment,attributes));
        auto* output=static_cast<UINT32*>(allocate(256,attributes));
        if (!cube_storage || !output) return 1;
        auto* canonical=reinterpret_cast<unsigned char*>((reinterpret_cast<std::uintptr_t>(cube_storage)+cube_layout.alignment-1)&~(cube_layout.alignment-1));
        std::memset(canonical,0x5a,cube_layout.bytes);
        TitleGpuResource cube,results;
        if (FAILED(acquire_title_gpu_texture(canonical,cube_layout,device.Get(),cube)) ||
            FAILED(acquire_title_gpu_buffer(output,256,device.Get(),results,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,true))) return 1;
        const HMODULE compiler=LoadLibraryExW(L"d3dcompiler_47.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        const auto compile=compiler ? reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(compiler,"D3DCompile")) : nullptr;
        if (!compile) return 1;
        constexpr char source[]=R"(
TextureCube<float4> image:register(t0);SamplerState point_sampler:register(s0);RWByteAddressBuffer output:register(u0);
[numthreads(24,1,1)]void main(uint3 id:SV_DispatchThreadID){
 uint face=id.x/4,corner=id.x%4;float s=(corner&1)?0.875:-0.875,t=(corner&2)?0.875:-0.875;
 float3 ray=face==0?float3(1,-t,-s):face==1?float3(-1,-t,s):face==2?float3(s,1,t):
            face==3?float3(s,-1,-t):face==4?float3(s,-t,1):float3(-s,-t,-1);
 uint4 c=(uint4)round(image.SampleLevel(point_sampler,ray,id.y)*255);
 output.Store((id.y*24+id.x)*4,c.x|(c.y<<8)|(c.z<<16)|(c.w<<24));
})";
        ComPtr<ID3DBlob> shader,errors,serialized;
        hr=compile(source,sizeof source-1,nullptr,nullptr,nullptr,"main","cs_5_0",0,0,&shader,&errors);
        D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0};
        D3D12_ROOT_PARAMETER parameters[2]{};
        parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;parameters[0].DescriptorTable={1,&range};
        parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;
        D3D12_STATIC_SAMPLER_DESC sampler{};sampler.Filter=D3D12_FILTER_MIN_MAG_MIP_POINT;
        sampler.AddressU=sampler.AddressV=sampler.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        sampler.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;sampler.MaxLOD=D3D12_FLOAT32_MAX;sampler.MaxAnisotropy=1;
        const D3D12_ROOT_SIGNATURE_DESC root_desc{2,parameters,1,&sampler,D3D12_ROOT_SIGNATURE_FLAG_NONE};
        if (SUCCEEDED(hr)) hr=D3D12SerializeRootSignature(&root_desc,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,nullptr);
        ComPtr<ID3D12RootSignature> root;
        if (SUCCEEDED(hr)) hr=device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&root));
        ComPtr<ID3D12PipelineState> pipeline;
        if (SUCCEEDED(hr)) {
            D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};desc.pRootSignature=root.Get();desc.CS={shader->GetBufferPointer(),shader->GetBufferSize()};
            hr=device->CreateComputePipelineState(&desc,IID_PPV_ARGS(&pipeline));
        }
        shader.Reset();errors.Reset();serialized.Reset();FreeLibrary(compiler);
        check(SUCCEEDED(hr),"cube sampling compiles a real native compute pipeline");
        if (FAILED(hr)) return 1;
        ComPtr<ID3D12DescriptorHeap> heap;
        const D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,1,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
        if (FAILED(device->CreateDescriptorHeap(&heap_desc,IID_PPV_ARGS(&heap)))) return 1;
        D3D12_SHADER_RESOURCE_VIEW_DESC view{};view.Format=DXGI_FORMAT_R8G8B8A8_UNORM;view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURECUBE;
        view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;view.TextureCube.MipLevels=2;
        device->CreateShaderResourceView(cube.resource,&view,heap->GetCPUDescriptorHandleForHeapStart());
        for (UINT iteration=0;iteration<2;++iteration) {
            for (UINT face=0;face<6;++face) for (UINT mip=0;mip<2;++mip) {
                const UINT size=8>>mip;std::vector<UINT32> pixels(size*size);
                for (UINT y=0;y<size;++y) for (UINT x=0;x<size;++x) pixels[y*size+x]=(face*31+iteration*5+mip)|(x*29<<8)|(y*27<<16)|0xff000000u;
                if (FAILED(cube_layout.copy(cube_layout.context.get(),true,pixels.data(),canonical,face*2+mip,size*4,size*size*4,nullptr))) return 1;
            }
            std::fill(output,output+64,0x5a5a5a5a);
            if (FAILED(allocator->Reset()) || FAILED(list->Reset(allocator.Get(),pipeline.Get())) ||
                FAILED(begin_title_gpu_texture(cube,list.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)) ||
                FAILED(begin_title_gpu_buffer(results,list.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS))) return 1;
            auto* bound=heap.Get();list->SetDescriptorHeaps(1,&bound);list->SetComputeRootSignature(root.Get());
            list->SetComputeRootDescriptorTable(0,heap->GetGPUDescriptorHandleForHeapStart());
            list->SetComputeRootUnorderedAccessView(1,results.resource->GetGPUVirtualAddress()+results.offset);list->Dispatch(1,2,1);
            if (FAILED(end_title_gpu_texture(cube,list.Get())) || FAILED(end_title_gpu_buffer(results,list.Get())) || FAILED(list->Close())) return 1;
            queue->ExecuteCommandLists(1,lists);
            HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if (!event) return 1;
            hr=queue->Signal(fence.Get(),70+iteration);
            if (SUCCEEDED(hr)) hr=fence->SetEventOnCompletion(70+iteration,event);
            const bool done=SUCCEEDED(hr) && WaitForSingleObject(event,3000)==WAIT_OBJECT_0;CloseHandle(event);
            if (!done || FAILED(complete_title_gpu_resource(cube,fence.Get(),70+iteration)) || FAILED(complete_title_gpu_resource(results,fence.Get(),70+iteration))) return 1;
            bool exact=true;
            for (UINT mip=0;mip<2;++mip) for (UINT face=0;face<6;++face) for (UINT corner=0;corner<4;++corner) {
                const UINT size=8>>mip,x=(corner&1)?size-1:0,y=(corner&2)?size-1:0;
                const UINT32 expected=(face*31+iteration*5+mip)|(x*29<<8)|(y*27<<16)|0xff000000u;
                exact=exact && output[mip*24+face*4+corner]==expected;
            }
            check(exact && std::all_of(output+48,output+64,[](auto x){return x==0x5a5a5a5a;}),
                  "cube GPU sampling preserves every face orientation, two mip levels, guards and CPU refresh");
        }
        check(release_title_gpu_resource(cube,fence.Get(),71)==S_OK && release_title_gpu_resource(results,fence.Get(),71)==S_OK,
              "cube sampling retires through its real GPU fence");
        release(cube_storage,attributes);release(output,attributes);
    }
    // Independent pixel oracle: source samples include both transfer breakpoints,
    // out-of-range RGB, distinct rows, and all four two-bit alpha values.
    const float values[]{0, .003f, .01f, .040449f, .04045f, .040451f, .1f, .1428f,
        .1429f, .2f, .5f, .75f, 1, -.1f, 1.1f, .9f};
    std::array<std::array<float, 4>, 32> pixels{};
    for (UINT i = 0; i < pixels.size(); ++i)
        pixels[i] = {values[(i + (i / 16) * 7) % 16], values[(i + 3) % 16], values[(i + 7) % 16], float(i % 4) / 3};
    D3D12_RESOURCE_DESC image{};
    image.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; image.Width = 16; image.Height = 2;
    image.DepthOrArraySize = image.MipLevels = 1; image.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; image.SampleDesc.Count = 1;
    D3D12_HEAP_PROPERTIES image_heap{}; image_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    ComPtr<ID3D12Resource> source_image;
    hr = device->CreateCommittedResource(&image_heap, D3D12_HEAP_FLAG_NONE, &image,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&source_image));
    check(SUCCEEDED(hr), "display source texture created");
    if (FAILED(hr)) return 1;
    ComPtr<ID3D12DescriptorHeap> source_descriptors, targets;
    const D3D12_DESCRIPTOR_HEAP_DESC source_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE, 0};
    const D3D12_DESCRIPTOR_HEAP_DESC target_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
    hr = device->CreateDescriptorHeap(&source_desc, IID_PPV_ARGS(&source_descriptors));
    if (SUCCEEDED(hr)) hr = device->CreateDescriptorHeap(&target_desc, IID_PPV_ARGS(&targets));
    if (FAILED(hr)) return 1;
    D3D12_SHADER_RESOURCE_VIEW_DESC source_view{};
    source_view.Format = image.Format; source_view.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    source_view.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; source_view.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(source_image.Get(), &source_view, source_descriptors->GetCPUDescriptorHandleForHeapStart());
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT source_footprint{};
    device->GetCopyableFootprints(&image, 0, 1, 0, &source_footprint, nullptr, nullptr, nullptr);
    UINT64 display_signal = 100;
    for (const auto output_format : {DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM}) {
        DisplayTransfer transfer;
        hr = transfer.initialize(device.Get(), output_format);
        check(SUCCEEDED(hr), "display transfer compiles into a native graphics pipeline");
        if (FAILED(hr)) return 1;
        auto output_desc = image; output_desc.Format = output_format; output_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        ComPtr<ID3D12Resource> output;
        hr = device->CreateCommittedResource(&image_heap, D3D12_HEAP_FLAG_NONE, &output_desc,
            D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr, IID_PPV_ARGS(&output));
        if (FAILED(hr)) return 1;
        device->CreateRenderTargetView(output.Get(), nullptr, targets->GetCPUDescriptorHandleForHeapStart());
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT output_footprint{};
        device->GetCopyableFootprints(&output_desc, 0, 1, 0, &output_footprint, nullptr, nullptr, nullptr);
        for (bool convert : {false, true}) {
            if (FAILED(allocator->Reset()) || FAILED(list->Reset(allocator.Get(), nullptr))) return 1;
            void* data = nullptr;
            if (FAILED(upload->Map(0, nullptr, &data))) return 1;
            for (UINT y = 0; y < 2; ++y)
                std::memcpy(static_cast<char*>(data) + source_footprint.Offset + y * source_footprint.Footprint.RowPitch,
                    pixels.data() + y * 16, 16 * sizeof pixels[0]);
            upload->Unmap(0, nullptr);
            D3D12_TEXTURE_COPY_LOCATION from{}, to{};
            from.pResource = upload.Get(); from.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; from.PlacedFootprint = source_footprint;
            to.pResource = source_image.Get(); to.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
            barrier(list.Get(), source_image.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            hr = transfer.record(list.Get(), source_descriptors.Get(), source_descriptors->GetGPUDescriptorHandleForHeapStart(),
                targets->GetCPUDescriptorHandleForHeapStart(), 16, 2, convert);
            if (FAILED(hr)) return 1;
            barrier(list.Get(), output.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_COPY_SOURCE);
            from = {}; from.pResource = output.Get(); from.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            to = {}; to.pResource = readback.Get(); to.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; to.PlacedFootprint = output_footprint;
            list->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
            barrier(list.Get(), source_image.Get(), D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
            barrier(list.Get(), output.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_RENDER_TARGET);
            if (FAILED(list->Close())) return 1;
            queue->ExecuteCommandLists(1, lists);
            const auto signal = display_signal++;
            HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!event) return 1;
            hr = queue->Signal(fence.Get(), signal);
            if (SUCCEEDED(hr)) hr = fence->SetEventOnCompletion(signal, event);
            const bool completed = SUCCEEDED(hr) && WaitForSingleObject(event, 3000) == WAIT_OBJECT_0;
            CloseHandle(event); if (!completed) return 1;
            if (FAILED(readback->Map(0, nullptr, &data))) return 1;
            bool matches = true;
            for (UINT i = 0; i < pixels.size(); ++i) {
                const auto* pixel = static_cast<const unsigned char*>(data) + output_footprint.Offset + (i / 16) * output_footprint.Footprint.RowPitch;
                UINT32 packed = 0;
                if (output_format == DXGI_FORMAT_R10G10B10A2_UNORM) std::memcpy(&packed, pixel + (i % 16) * 4, 4);
                for (UINT c = 0; c < 4; ++c) {
                    double expected = pixels[i][c];
                    if (c != 3) {
                        expected = (std::clamp)(expected, 0., 1.);
                        if (convert) {
                            const double linear = expected <= .04045 ? expected / 12.92 : std::pow((expected + .055) / 1.055, 2.4);
                            expected = linear < .018 ? 4.5 * linear : 1.099 * std::pow(linear, .45) - .099;
                        }
                    }
                    if (output_format == DXGI_FORMAT_R10G10B10A2_UNORM) {
                        const UINT mask = c == 3 ? 3 : 1023;
                        const UINT actual = (packed >> (c * 10)) & mask;
                        matches = matches && std::abs(double(actual) - std::round(expected * mask)) <= (c == 3 ? 0 : 1);
                    } else if (output_format == DXGI_FORMAT_B8G8R8A8_UNORM) {
                        const UINT channel = c == 0 ? 2 : c == 2 ? 0 : c;
                        const UINT actual = pixel[(i % 16) * 4 + channel];
                        matches = matches && std::abs(double(actual) - std::round(expected * 255)) <= (c == 3 ? 0 : 1);
                    } else {
                        float actual = 0; std::memcpy(&actual, pixel + (i % 16) * 16 + c * 4, 4);
                        matches = matches && std::abs(actual - expected) < .00001;
                    }
                }
            }
            readback->Unmap(0, nullptr);
            check(matches, convert ? "Xbox 0x200000 transfer preserves BT.709 pixels, alpha and orientation" :
                "negative control preserves RGB without the Xbox transfer");
        }
    }
    auto* pool_memory = static_cast<unsigned char*>(virtual_allocate(nullptr, 4 * 65536, 0x30002000, PAGE_READWRITE));
    auto* pool_alias = static_cast<unsigned char*>(virtual_allocate(nullptr, 65536, 0x30002000, PAGE_READWRITE));
    if (!pool_memory || !pool_alias) return 1;
    const UINT pool_pages[]{1, 2, 3};
    if (FAILED(map_title_esram(1, pool_memory, 3, pool_pages)) || FAILED(map_title_esram(1, pool_alias, 1, pool_pages + 2)) ||
        virtual_allocate(pool_memory + 3 * 65536, 32768, 0x30001000, PAGE_READWRITE) != pool_memory + 3 * 65536) return 1;
    MEMORY_BASIC_INFORMATION rounded_commit{};
    check(VirtualQuery(pool_memory + 4 * 65536 - 1, &rounded_commit, sizeof rounded_commit) && rounded_commit.State == MEM_COMMIT,
        "graphics commit rounds a partial 64-KiB page within its owned reservation");
    auto* mixed_bytes = pool_memory + 3 * 65536 - 16;
    std::memset(mixed_bytes - 16, 0x5a, 96); std::memset(mixed_bytes, 0x39, 64);
    TitleGpuResource mixed, mirror;
    hr = acquire_title_gpu_buffer(mixed_bytes, 64, device.Get(), mixed, D3D12_RESOURCE_FLAG_NONE, true);
    check(SUCCEEDED(hr) && mixed.composite, "GPU buffer spans ESRAM and private graphics storage in one registry");
    if (FAILED(hr) || FAILED(acquire_title_gpu_buffer(pool_alias + 65536 - 16, 16, device.Get(), mirror, D3D12_RESOURCE_FLAG_NONE, true))) return 1;
    if (FAILED(allocator->Reset()) || FAILED(list->Reset(allocator.Get(), nullptr))) return 1;
    if (FAILED(begin_title_gpu_buffer(mixed, list.Get(), D3D12_RESOURCE_STATE_COPY_DEST))) return 1;
    check(begin_title_gpu_buffer(mirror, list.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE) == HRESULT_FROM_WIN32(ERROR_BUSY),
          "physical ESRAM aliases cannot run concurrent conflicting GPU intervals");
    void* cpu_alias = nullptr;
    check(map_title_gpu_resource(mirror, &cpu_alias) == E_PENDING && !cpu_alias,
          "CPU aliases wait for the physical page's real GPU interval");
    const UINT remapped_page = 4;
    check(map_title_esram(1, pool_alias, 1, &remapped_page) == HRESULT_FROM_WIN32(ERROR_BUSY) && pool_alias[65536 - 16] == 0x39,
          "remapping cannot retire a physical page in flight");
    // A new view of a page in another thread's GPU interval waits for it instead of failing creation.
    auto* waiting_view = static_cast<unsigned char*>(virtual_allocate(nullptr, 65536, 0x30002000, PAGE_READWRITE));
    if (!waiting_view) return 1;
    std::atomic<HRESULT> waited{E_FAIL};
    std::thread mapper([&] { waited = map_title_esram(1, waiting_view, 1, pool_pages + 2); });
    Sleep(100);
    check(waited.load() == E_FAIL, "a new ESRAM view waits while another thread's GPU interval owns the page");
    void* upload_bytes = nullptr;
    if (FAILED(upload->Map(0, nullptr, &upload_bytes))) return 1;
    std::memset(upload_bytes, 0xa7, 64); upload->Unmap(0, nullptr);
    list->CopyBufferRegion(mixed.resource, 0, upload.Get(), 0, 64);
    if (FAILED(end_title_gpu_buffer(mixed, list.Get()))) return 1;
    const auto finish_pool = [&](UINT64 value) {
        if (FAILED(list->Close())) return false;
        queue->ExecuteCommandLists(1, lists);
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event) return false;
        HRESULT hr = queue->Signal(fence.Get(), value);
        if (SUCCEEDED(hr)) hr = fence->SetEventOnCompletion(value, event);
        const bool done = SUCCEEDED(hr) && WaitForSingleObject(event, 3000) == WAIT_OBJECT_0;
        CloseHandle(event); return done;
    };
    if (!finish_pool(200)) return 1;
    check(mixed_bytes[0] == 0x39 && pool_alias[65536 - 16] == 0x39 && release_title_gpu_resource(mixed, fence.Get(), 200) == E_PENDING,
          "GPU completion alone does not publish pending CPU writeback");
    if (FAILED(complete_title_gpu_resource(mixed, fence.Get(), 200))) return 1;
    check(std::all_of(mixed_bytes, mixed_bytes + 64, [](auto x) { return x == 0xa7; }) &&
        pool_alias[65535] == 0xa7 && mixed_bytes[-1] == 0x5a && mixed_bytes[64] == 0x5a,
        "GPU writes update real ESRAM aliases and the private tail while preserving guards");
    mapper.join();
    check(waited.load() == S_OK && waiting_view[65535] == 0xa7,
          "the waiting ESRAM view maps after the GPU interval and observes its published bytes");
    if (FAILED(map_title_esram(1, waiting_view, 1, nullptr)) || !virtual_free(waiting_view, 0, MEM_RELEASE)) return 1;
    if (FAILED(map_title_esram(1, pool_memory + 2 * 65536, 1, &remapped_page))) return 1;
    std::memset(mixed_bytes, 0x66, 16);
    check(pool_alias[65535] == 0xa7, "remapping an inactive representation changes its physical source without changing the old alias");
    if (FAILED(allocator->Reset()) || FAILED(list->Reset(allocator.Get(), nullptr)) ||
        FAILED(begin_title_gpu_buffer(mixed, list.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE))) return 1;
    check(map_title_gpu_resource(mirror, &cpu_alias) == S_OK && cpu_alias == pool_alias + 65536 - 16,
          "physical ownership refreshes after remapping and preserves the guest CPU address");
    if (cpu_alias) unmap_title_gpu_resource(mirror);
    if (FAILED(begin_title_gpu_buffer(mirror, list.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE))) return 1;
    list->CopyBufferRegion(readback.Get(), 0, mixed.resource, 0, 64);
    if (FAILED(end_title_gpu_buffer(mixed, list.Get())) || FAILED(end_title_gpu_buffer(mirror, list.Get())) || !finish_pool(201)) return 1;
    if (FAILED(readback->Map(0, nullptr, &upload_bytes))) return 1;
    const auto* observed = static_cast<const unsigned char*>(upload_bytes);
    check(std::all_of(observed, observed + 16, [](auto x) { return x == 0x66; }) &&
        std::all_of(observed + 16, observed + 64, [](auto x) { return x == 0xa7; }), "GPU reuse refreshes the new physical page and preserves the private tail");
    readback->Unmap(0, nullptr);
    if (FAILED(complete_title_gpu_resource(mixed, fence.Get(), 201)) || FAILED(complete_title_gpu_resource(mirror, fence.Get(), 201)) ||
        FAILED(release_title_gpu_resource(mixed, fence.Get(), 201)) || FAILED(release_title_gpu_resource(mirror, fence.Get(), 201))) return 1;
    {
        auto* input= reinterpret_cast<UINT32*>(pool_alias+256);
        auto* output=static_cast<UINT32*>(allocate(64,attributes));
        if (!output) return 1;
        TitleGpuResource raw_input,raw_output;
        if (FAILED(acquire_title_gpu_buffer(input,64,device.Get(),raw_input,D3D12_RESOURCE_FLAG_NONE,true)) ||
            FAILED(acquire_title_gpu_buffer(output,64,device.Get(),raw_output,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,true))) return 1;
        const HMODULE compiler=LoadLibraryExW(L"d3dcompiler_47.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        const auto compile=compiler ? reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(compiler,"D3DCompile")) : nullptr;
        if (!compile) return 1;
        constexpr char source[]="ByteAddressBuffer input:register(t0);RWByteAddressBuffer output:register(u0);"
            "[numthreads(8,1,1)]void main(uint3 id:SV_DispatchThreadID){output.Store(id.x*4,input.Load(id.x*4)^0xDEADBEEFu);}";
        ComPtr<ID3DBlob> shader,errors,serialized;
        hr=compile(source,sizeof source-1,nullptr,nullptr,nullptr,"main","cs_5_0",0,0,&shader,&errors);
        D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,1,0,0,0};
        D3D12_ROOT_PARAMETER parameters[2]{};
        parameters[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[0].DescriptorTable={1,&range};
        parameters[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;
        const D3D12_ROOT_SIGNATURE_DESC root_desc{2,parameters,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};
        if (SUCCEEDED(hr)) hr=D3D12SerializeRootSignature(&root_desc,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,nullptr);
        ComPtr<ID3D12RootSignature> root;
        if (SUCCEEDED(hr)) hr=device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&root));
        ComPtr<ID3D12PipelineState> pipeline;
        if (SUCCEEDED(hr)) {
            D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};
            desc.pRootSignature=root.Get(); desc.CS={shader->GetBufferPointer(),shader->GetBufferSize()};
            hr=device->CreateComputePipelineState(&desc,IID_PPV_ARGS(&pipeline));
        }
        shader.Reset();errors.Reset();serialized.Reset();FreeLibrary(compiler);
        check(SUCCEEDED(hr),"raw buffer load fixture creates a real compute pipeline");
        if (FAILED(hr)) return 1;
        ComPtr<ID3D12DescriptorHeap> heap;
        const D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,1,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
        if (FAILED(device->CreateDescriptorHeap(&heap_desc,IID_PPV_ARGS(&heap)))) return 1;
        D3D12_SHADER_RESOURCE_VIEW_DESC raw{};
        raw.Format=DXGI_FORMAT_R32_TYPELESS;raw.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;
        raw.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        raw.Buffer.FirstElement=raw_input.offset/4+4;raw.Buffer.NumElements=8;raw.Buffer.Flags=D3D12_BUFFER_SRV_FLAG_RAW;
        device->CreateShaderResourceView(raw_input.resource,&raw,heap->GetCPUDescriptorHandleForHeapStart());
        for (UINT iteration=0;iteration<2;++iteration) {
            for (UINT i=0;i<16;++i) input[i]=0xabc00000u+iteration*256+i;
            std::fill(output,output+16,0x5a5a5a5a);
            if (FAILED(allocator->Reset()) || FAILED(list->Reset(allocator.Get(),pipeline.Get())) ||
                FAILED(begin_title_gpu_buffer(raw_input,list.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)) ||
                FAILED(begin_title_gpu_buffer(raw_output,list.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS))) return 1;
            auto* bound=heap.Get();list->SetDescriptorHeaps(1,&bound);list->SetComputeRootSignature(root.Get());
            list->SetComputeRootDescriptorTable(0,heap->GetGPUDescriptorHandleForHeapStart());
            list->SetComputeRootUnorderedAccessView(1,raw_output.resource->GetGPUVirtualAddress()+raw_output.offset);
            list->Dispatch(1,1,1);
            if (FAILED(end_title_gpu_buffer(raw_input,list.Get())) || FAILED(end_title_gpu_buffer(raw_output,list.Get())) || !finish_pool(202+iteration)) return 1;
            if (FAILED(complete_title_gpu_resource(raw_input,fence.Get(),202+iteration)) ||
                FAILED(complete_title_gpu_resource(raw_output,fence.Get(),202+iteration))) return 1;
            bool exact=true;
            for (UINT i=0;i<16;++i) exact=exact && output[i]==(i<8 ? ((0xabc00000u+iteration*256+i+4)^0xdeadbeefu) : 0x5a5a5a5a);
            check(exact,"raw GPU loads honor element offset, bounds, ESRAM bytes and CPU refresh through real fences");
        }
        check(release_title_gpu_resource(raw_input,fence.Get(),203)==S_OK && release_title_gpu_resource(raw_output,fence.Get(),203)==S_OK,
              "raw SRV representations retire only after their real GPU work");
        release(output,attributes);
    }
    if (FAILED(map_title_esram(1, pool_alias, 1, &remapped_page))) return 1;
    TitleTextureLayout pool_texture;
    pool_texture.bytes = 8192; pool_texture.alignment = 16; pool_texture.context = std::make_shared<int>(0); pool_texture.copy = &linear_sample_copy;
    pool_texture.resource_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    pool_texture.resource_desc.Width = 32; pool_texture.resource_desc.Height = 16;
    pool_texture.resource_desc.DepthOrArraySize = pool_texture.resource_desc.MipLevels = 1;
    pool_texture.resource_desc.Format = DXGI_FORMAT_R11G11B10_FLOAT; pool_texture.resource_desc.SampleDesc.Count = 4;
    pool_texture.resource_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    auto* mixed_texture = pool_memory + 3 * 65536 - 4096;
    check(sample_fixture(pool_texture, 210, mixed_texture), "four-sample texture spans ESRAM and private graphics pages coherently");
    check(!std::memcmp(mixed_texture, pool_alias + 65536 - 4096, 4096), "MSAA writeback reaches the physical ESRAM alias");
    if (original_xg && !bgra_only) {
        hr=create_xg_texture_layout(32,16,1,1,DXGI_FORMAT_R32G8X24_TYPELESS,1,pool_texture,
            D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE,4);
        auto* mixed_depth=pool_memory+2*65536-8192;
        check(SUCCEEDED(hr) && sample_fixture(std::move(pool_texture),220,mixed_depth),
            "original depth/stencil planes remain coherent across shared ESRAM and private DDR pages");
        check(!std::memcmp(mixed_depth+8192,pool_alias,65536),"depth/stencil writeback reaches every byte of its real ESRAM alias");
    }
    for(bool xmem_graphics : {false,true}) {
        const UINT64 epoch=fence->GetCompletedValue();
        auto* owned=xmem_graphics ? static_cast<BYTE*>(allocate(4*65536,attributes)) : nullptr;
        if(xmem_graphics && !owned)return 1;
        auto* backing=xmem_graphics ? owned : pool_memory;
        TitleGpuResource disjoint_cpu;
        auto* disjoint_bytes=backing+3*65536+4096;
        std::memset(disjoint_bytes,0x77,128);void* disjoint_map=nullptr;
        if(acquire_title_gpu_buffer(disjoint_bytes,128,device.Get(),disjoint_cpu,D3D12_RESOURCE_FLAG_NONE,true)!=S_OK ||
            map_title_gpu_resource(disjoint_cpu,&disjoint_map)!=S_OK)return 1;
        auto context=std::make_shared<LinearTextureFixture>();
        TitleTextureLayout tracked;
        tracked.bytes=128;tracked.alignment=16;tracked.context=context;tracked.copy=linear_texture_copy;tracked.demand_coherence=true;
        tracked.resource_desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        tracked.resource_desc.Width=8;tracked.resource_desc.Height=4;
        tracked.resource_desc.DepthOrArraySize=tracked.resource_desc.MipLevels=1;
        tracked.resource_desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;tracked.resource_desc.SampleDesc.Count=1;
        tracked.resource_desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        auto* canonical=backing+2*65536+4096;
        auto* physical_alias=xmem_graphics ? canonical : pool_alias+4096;
        std::memset(canonical-16,0x5a,160);
        TitleGpuResource texture,alias;
        auto independent_layout=tracked;independent_layout.context=std::make_shared<LinearTextureFixture>();
        check(acquire_title_gpu_texture(canonical,tracked,device.Get(),texture)==S_OK &&
            acquire_title_gpu_texture(physical_alias,independent_layout,device.Get(),alias)==S_OK,"tracked textures share canonical graphics bytes through native aliases");
        check(texture.resource!=alias.resource,"unidentified independent layout providers retain distinct native representations");
        check(texture.composite && alias.composite,"both graphics reservations and graphics XMem participate in native residency");
        const auto submit=[&](TitleGpuResource& resource,UINT64 value) {
            if(FAILED(end_title_gpu_texture(resource,list.Get())) || FAILED(list->Close()))return false;
            queue->ExecuteCommandLists(1,lists);
            HANDLE ready=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!ready)return false;
            HRESULT result=queue->Signal(fence.Get(),value);
            if(SUCCEEDED(result))result=fence->SetEventOnCompletion(value,ready);
            const bool done=SUCCEEDED(result) && WaitForSingleObject(ready,3000)==WAIT_OBJECT_0;CloseHandle(ready);
            return done && complete_title_gpu_resource(resource,fence.Get(),value)==S_OK;
        };
        const auto begin=[&](TitleGpuResource& resource,bool read_only) {
            return SUCCEEDED(allocator->Reset()) && SUCCEEDED(list->Reset(allocator.Get(),nullptr)) &&
                begin_title_gpu_texture(resource,list.Get(),read_only ? D3D12_RESOURCE_STATE_COPY_SOURCE : D3D12_RESOURCE_STATE_COPY_DEST,false,nullptr,read_only)==S_OK;
        };
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT rows=0;UINT64 row_bytes=0,total=0;
        device->GetCopyableFootprints(&tracked.resource_desc,0,1,0,&footprint,&rows,&row_bytes,&total);
        const auto mutate=[&](BYTE value,bool discard=false) {
            void* mapped=nullptr;if(FAILED(upload->Map(0,nullptr,&mapped)))return false;
            std::memset(mapped,value,SIZE_T(total));upload->Unmap(0,nullptr);
            if(discard) {
                if(FAILED(allocator->Reset()) || FAILED(list->Reset(allocator.Get(),nullptr)) ||
                    FAILED(begin_title_gpu_texture(texture,list.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,true)))return false;
                barrier(list.Get(),texture.resource,D3D12_RESOURCE_STATE_RENDER_TARGET,D3D12_RESOURCE_STATE_COPY_DEST);
            } else if(!begin(texture,false))return false;
            D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=upload.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;from.PlacedFootprint=footprint;
            to.pResource=texture.resource;to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
            if(discard)barrier(list.Get(),texture.resource,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_RENDER_TARGET);
            return true;
        };
        TitleGpuResource same_page_cpu,same_page_texture;void* same_page_map=nullptr;
        auto* private_texture=disjoint_bytes+4096;
        if(acquire_title_gpu_texture(private_texture,tracked,device.Get(),same_page_texture)!=S_OK ||
            acquire_title_gpu_buffer(private_texture+512,16,device.Get(),same_page_cpu,D3D12_RESOURCE_FLAG_NONE,true)!=S_OK ||
            map_title_gpu_resource(same_page_cpu,&same_page_map)!=S_OK || FAILED(allocator->Reset()) || FAILED(list->Reset(allocator.Get(),nullptr)))return 1;
        check(begin_title_gpu_texture(same_page_texture,list.Get(),D3D12_RESOURCE_STATE_COPY_DEST)==HRESULT_FROM_WIN32(ERROR_BUSY),
            "tracked DDR texture still excludes a disjoint CPU map inside its protected 4-KiB page");
        if(FAILED(list->Close()) || unmap_title_gpu_resource(same_page_cpu)!=S_OK ||
            release_title_gpu_resource(same_page_cpu,fence.Get(),fence->GetCompletedValue())!=S_OK ||
            release_title_gpu_resource(same_page_texture,fence.Get(),fence->GetCompletedValue())!=S_OK)return 1;
        const bool began=texture.resource && alias.resource && mutate(0xa7);
        check(began,"tracked GPU texture accepts a disjoint live CPU mapping in its canonical parent");
        if(!began || !submit(texture,(epoch+500)))return 1;
        MEMORY_BASIC_INFORMATION state{};
        check(VirtualQuery(canonical,&state,sizeof state) && state.Protect==PAGE_NOACCESS && context->publications==0,
            "completed GPU texture stays resident; canonical reads remain protected until publication");
        auto* unrelated=allocate(65536,attributes);
        if(!unrelated)return 1;
        release(unrelated,attributes);
        check(context->publications==0 && VirtualQuery(canonical,&state,sizeof state) && state.Protect==PAGE_NOACCESS,
            "freeing unrelated graphics XMem preserves pending native residency and CPU protection");
        if(!begin(texture,true) || !submit(texture,(epoch+501)))return 1;
        check(context->uploads==1 && context->publications==0,"unchanged GPU reuse skips CPU upload and publication");
        BYTE first=0,second=0;
        std::thread reader([&]{first=*static_cast<volatile BYTE*>(canonical);});
        std::thread alias_reader([&]{second=*static_cast<volatile BYTE*>(physical_alias);});
        reader.join();alias_reader.join();
        check(first==0xa7 && second==0xa7 && context->publications==1 && canonical[-1]==0x5a && canonical[128]==0x5a,
            "concurrent reads through both aliases publish one complete GPU image and preserve guards");
        const UINT before_retry=context->uploads;
        context->fail_upload=true;texture.clear.depth=1;
        check(!begin(texture,true),"failed layout conversion does not publish new residency metadata");
        if(FAILED(list->Close()))return 1;
        context->fail_upload=false;
        if(!begin(texture,true) || !submit(texture,(epoch+502)))return 1;
        check(context->uploads==before_retry+2,"retry under changed clear metadata must perform the previously failed conversion");
        const UINT before_cpu_write=context->uploads;
        physical_alias[0]=0x39;
        if(!begin(texture,true))return 1;
        D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=texture.resource;from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to.pResource=readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=footprint;
        list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
        if(!submit(texture,(epoch+503)))return 1;
        void* mapped=nullptr;if(FAILED(readback->Map(0,nullptr,&mapped)))return 1;
        check(static_cast<BYTE*>(mapped)[0]==0x39 && static_cast<BYTE*>(mapped)[1]==0xa7 && context->uploads==before_cpu_write+1,
            "CPU write through a physical alias invalidates the resident texture before the next GPU read");
        readback->Unmap(0,nullptr);
        if(!mutate(0xb8) || !submit(texture,(epoch+504)))return 1;
        wchar_t temporary[MAX_PATH]{},file_name[MAX_PATH]{};
        if(!GetTempPathW(MAX_PATH,temporary) || !GetTempFileNameW(temporary,L"xgw",0,file_name))return 1;
        HANDLE file=CreateFileW(file_name,GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_TEMPORARY|FILE_FLAG_DELETE_ON_CLOSE,nullptr);
        if(file==INVALID_HANDLE_VALUE){DeleteFileW(file_name);return 1;}
        BYTE byte=0x6c;DWORD transferred=0;
        const auto read_file=reinterpret_cast<decltype(&ReadFile)>(resolve_kernelx({"kernelx.dll","ReadFile",0}));
        const bool io=WriteFile(file,&byte,1,&transferred,nullptr) && SetFilePointer(file,0,nullptr,FILE_BEGIN)==0 &&
            read_file && read_file(file,physical_alias,1,&transferred,nullptr) && transferred==1;
        CloseHandle(file);
        check(io && canonical[0]==0x6c && canonical[1]==0xb8 && context->publications==2,
            "kernel I/O flushes pending GPU bytes before writing a protected canonical alias");
        if(!mutate(0xc9) || !submit(texture,(epoch+505)) || !begin(alias,true))return 1;
        from.pResource=alias.resource;
        list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
        if(!submit(alias,(epoch+506)) || FAILED(readback->Map(0,nullptr,&mapped)))return 1;
        check(static_cast<BYTE*>(mapped)[0]==0xc9 && context->publications==3,
            "a distinct GPU representation synchronizes a pending writer through physical identity");
        readback->Unmap(0,nullptr);
        if(!mutate(0xda) || !submit(texture,(epoch+507)))return 1;
        SetLastError(0x13579);
        const BYTE observed=*static_cast<volatile BYTE*>(physical_alias);
        check(observed==0xda && GetLastError()==0x13579,"fault-driven publication preserves the guest thread's last error");
        if(!mutate(0xeb) || !submit(texture,(epoch+508)))return 1;
        const UINT before_discard=context->publications;
        if(!mutate(0xfc,true) || !submit(texture,(epoch+509)))return 1;
        check(context->publications==before_discard,"full overwrite discards obsolete GPU contents without publishing them to CPU");
        check(physical_alias[0]==0xfc,"full overwrite publishes its latest canonical image on demand");
        ComPtr<ID3D12Fence> gate,queued;
        if(FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate))) ||
            FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&queued))) || !mutate(0x8e) ||
            FAILED(end_title_gpu_texture(texture,list.Get())) || FAILED(list->Close()))return 1;
        std::shared_ptr<void> lease;
        TitleGpuResource* pending_resources[]{&texture};
        TitleGpuResource* invalid_resources[]{nullptr};
        check(prepare_title_gpu_submission(invalid_resources,lease)==E_INVALIDARG && !lease,
            "queued resource preparation rejects null handles before submission");
        TitleGpuResource* duplicate_resources[]{&texture,&texture};
        check(prepare_title_gpu_submission(duplicate_resources,lease)==E_INVALIDARG && !lease,
            "queued resource preparation rejects duplicate ownership before submission");
        check(prepare_title_gpu_submission(pending_resources,lease)==S_OK && lease,
            "tracked submission retains native resources before GPU execution");
        if(!lease || FAILED(queue->Wait(gate.Get(),1)))return 1;
        queue->ExecuteCommandLists(1,lists);
        if(FAILED(queue->Signal(queued.Get(),1)) || publish_title_gpu_submission(lease,queue.Get(),queued.Get(),1)!=S_OK)return 1;
        ComPtr<ID3D12CommandAllocator> clear_allocator;ComPtr<ID3D12GraphicsCommandList> clear_list;
        ComPtr<ID3D12DescriptorHeap> clear_heap;
        const D3D12_DESCRIPTOR_HEAP_DESC heap_desc{D3D12_DESCRIPTOR_HEAP_TYPE_RTV,1,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};
        if(FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&clear_allocator))) ||
            FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,clear_allocator.Get(),nullptr,IID_PPV_ARGS(&clear_list))) ||
            FAILED(device->CreateDescriptorHeap(&heap_desc,IID_PPV_ARGS(&clear_heap))))return 1;
        const auto clear_target=clear_heap->GetCPUDescriptorHandleForHeapStart();device->CreateRenderTargetView(texture.resource,nullptr,clear_target);
        check(queued->GetCompletedValue()==0,"the resident-clear control starts behind an unsignaled native queue gate");
        texture.clear.depth^=1; // Full overwrite has no dependency on the previous logical clear registers.
        check(begin_title_gpu_texture(texture,clear_list.Get(),D3D12_RESOURCE_STATE_RENDER_TARGET,true,nullptr,false,queue.Get())==S_OK,
            "an ordered full clear reuses resident native storage without waiting for the preceding GPU fence");
        const FLOAT clear_color[]{108.0f/255,108.0f/255,108.0f/255,108.0f/255};
        clear_list->ClearRenderTargetView(clear_target,clear_color,0,nullptr);
        std::shared_ptr<void> clear_lease;
        if(end_title_gpu_texture(texture,clear_list.Get())!=S_OK || FAILED(clear_list->Close()) ||
            prepare_title_gpu_submission(pending_resources,clear_lease)!=S_OK)return 1;
        ID3D12CommandList* clears[]{clear_list.Get()};queue->ExecuteCommandLists(1,clears);
        if(FAILED(queue->Signal(queued.Get(),2)) || publish_title_gpu_submission(clear_lease,queue.Get(),queued.Get(),2)!=S_OK)return 1;
        check(queued->GetCompletedValue()==0,"resident clear publication preserves real pending native completion");
        void* blocked=nullptr;
        check(map_title_gpu_resource(texture,&blocked)==E_PENDING && !blocked &&
            map_title_gpu_resource(alias,&blocked)==E_PENDING && !blocked,
            "nonblocking maps preserve pending status for queued writers through either physical alias");
        check(queued->GetCompletedValue()==0 && release_title_gpu_resource(texture,fence.Get(),(epoch+509))==S_OK,
            "logical resource release does not destroy a submitted native image before its fence");
        HANDLE entered=CreateEventW(nullptr,FALSE,FALSE,nullptr),read_done=CreateEventW(nullptr,FALSE,FALSE,nullptr);
        if(!entered || !read_done)return 1;
        BYTE queued_byte=0;
        std::thread queued_reader([&] {SetEvent(entered);queued_byte=*static_cast<volatile BYTE*>(physical_alias);SetEvent(read_done);});
        const bool started=WaitForSingleObject(entered,3000)==WAIT_OBJECT_0;
        const bool excluded=WaitForSingleObject(read_done,30)==WAIT_TIMEOUT;
        const auto released=gate->Signal(1);
        const bool visible=WaitForSingleObject(read_done,3000)==WAIT_OBJECT_0;
        queued_reader.join();CloseHandle(entered);CloseHandle(read_done);
        check(started && excluded && SUCCEEDED(released) && visible && queued_byte==0x6c,
            "CPU reads through a physical alias wait for the queued GPU fence and receive its actual pixels");
        lease.reset();clear_lease.reset();
        check(release_title_gpu_resource(alias,fence.Get(),(epoch+509))==S_OK && physical_alias[0]==0x6c,
            "queued pins retire before the final canonical alias detaches");
        check(VirtualQuery(canonical,&state,sizeof state) && state.Protect==PAGE_READWRITE,"last tracked owner restores the guest page protection");
        check(disjoint_map==disjoint_bytes && std::all_of(disjoint_bytes,disjoint_bytes+128,[](BYTE value){return value==0x77;}) &&
            VirtualQuery(disjoint_bytes,&state,sizeof state) && state.Protect==PAGE_READWRITE,
            "texture GPU reuse, CPU faults and retirement preserve the independent CPU map and bytes");
        check(unmap_title_gpu_resource(disjoint_cpu)==S_OK && release_title_gpu_resource(disjoint_cpu,fence.Get(),(epoch+509))==S_OK,
            "independent CPU mapping retires through its existing ownership contract");
        if(owned) {
            release(owned,attributes);
            check(VirtualQuery(owned,&state,sizeof state) && state.State==MEM_FREE,
                "graphics XMem releases its native mapping and reservation after final retirement");
        }
        if(!xmem_graphics) {
            // Reference 14056ada0/140570490: distinct descriptions over shared ESRAM keep separate native images.
            auto narrow=tracked,wide=tracked;
            auto narrow_context=std::make_shared<LinearTextureFixture>(),wide_context=std::make_shared<LinearTextureFixture>();
            narrow.context=narrow_context;wide.context=wide_context;wide.resource_desc.Format=DXGI_FORMAT_R10G10B10A2_UNORM;
            TitleGpuResource first,second;
            check(acquire_title_gpu_texture(canonical,narrow,device.Get(),first)==S_OK &&
                acquire_title_gpu_texture(physical_alias,wide,device.Get(),second)==S_OK,"distinct descriptions bind the same ESRAM pages");
            const auto write=[&](TitleGpuResource& target,BYTE value,UINT64 signal) {
                void* mapped=nullptr;if(FAILED(upload->Map(0,nullptr,&mapped)))return false;
                std::memset(mapped,value,SIZE_T(total));upload->Unmap(0,nullptr);
                if(!begin(target,false))return false;
                auto layout=footprint;layout.Footprint.Format=target.resource->GetDesc().Format;
                D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=upload.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;from.PlacedFootprint=layout;
                to.pResource=target.resource;to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
                return submit(target,signal);
            };
            const UINT64 base=fence->GetCompletedValue();
            if(!first.resource || !second.resource || !write(first,0x21,base+1) || !write(second,0x43,base+2))return 1;
            check(narrow_context->publications==0 && wide_context->publications==0,
                "a GPU writer with another description does not publish the pending ESRAM image");
            const UINT uploads=narrow_context->uploads;
            if(!begin(first,true))return 1;
            D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=first.resource;from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            to.pResource=readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=footprint;
            list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
            void* mapped=nullptr;
            if(!submit(first,base+3) || FAILED(readback->Map(0,nullptr,&mapped)))return 1;
            check(static_cast<BYTE*>(mapped)[0]==0x21 && narrow_context->uploads==uploads,
                "each ESRAM description keeps its resident native contents without reloading");
            readback->Unmap(0,nullptr);
            second.clear.depth=1; // Changed clear registers republish the newer image for its own re-decode.
            if(!begin(second,true) || !submit(second,base+4))return 1;
            check(wide_context->publications==1 && narrow_context->publications==0,
                "a newer ESRAM image republishes without forcing a readback of the older pending image");
            auto* unrelated_view=virtual_allocate(nullptr,65536,0x30002000,PAGE_READWRITE);const UINT unrelated_page=7;
            check(unrelated_view && map_title_esram(1,unrelated_view,1,&unrelated_page)==S_OK &&
                narrow_context->publications==0 && wide_context->publications==1,
                "mapping an unrelated ESRAM page keeps other pending images resident (1401de820 range invalidation)");
            if(!unrelated_view || FAILED(map_title_esram(1,unrelated_view,1,nullptr)) || !virtual_free(unrelated_view,0,MEM_RELEASE))return 1;
            const BYTE observed=*static_cast<volatile BYTE*>(canonical);
            check(observed==0x43 && narrow_context->publications==1 && wide_context->publications==1 && canonical[-1]==0x5a,
                "later CPU access skips superseded ESRAM pages of the older image, so the newest bytes win");
            check(release_title_gpu_resource(first,fence.Get(),base+4)==S_OK && release_title_gpu_resource(second,fence.Get(),base+4)==S_OK,
                "independent ESRAM descriptions retire through their fences");
        }
    }
    {
        // A GPU-written depth image publishes expanded HTILE, so shader reads that bypass the depth
        // block decode the same image and must reuse it instead of a readback/re-upload round trip.
        auto* canonical=static_cast<BYTE*>(allocate(65536,attributes));
        if(!canonical)return 1;
        std::memset(canonical,0x3c,128);
        auto context=std::make_shared<LinearTextureFixture>();
        TitleTextureLayout depth;
        depth.bytes=128;depth.alignment=16;depth.context=context;depth.copy=linear_texture_copy;depth.demand_coherence=true;
        depth.resource_desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;depth.resource_desc.Width=8;depth.resource_desc.Height=4;
        depth.resource_desc.DepthOrArraySize=depth.resource_desc.MipLevels=1;depth.resource_desc.SampleDesc.Count=1;
        depth.resource_desc.Format=DXGI_FORMAT_R32_TYPELESS;depth.resource_desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        TitleGpuResource texture;
        if(acquire_title_gpu_texture(canonical,depth,device.Get(),texture)!=S_OK)return 1;
        UINT64 value=fence->GetCompletedValue()+1;
        const auto use=[&](bool read_only) {
            if(FAILED(allocator->Reset()) || FAILED(list->Reset(allocator.Get(),nullptr)) ||
                begin_title_gpu_texture(texture,list.Get(),read_only ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_DEPTH_WRITE,
                    false,nullptr,read_only)!=S_OK || FAILED(end_title_gpu_texture(texture,list.Get())) || FAILED(list->Close()))return false;
            queue->ExecuteCommandLists(1,lists);
            HANDLE ready=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!ready)return false;
            const UINT64 signal=value++;HRESULT result=queue->Signal(fence.Get(),signal);
            if(SUCCEEDED(result))result=fence->SetEventOnCompletion(signal,ready);
            const bool done=SUCCEEDED(result) && WaitForSingleObject(ready,3000)==WAIT_OBJECT_0;CloseHandle(ready);
            return done && complete_title_gpu_resource(texture,fence.Get(),signal)==S_OK;
        };
        check(use(false) && use(true) && use(false) && context->uploads==1 && context->publications==0,
            "GPU-written depth alternates depth and bypassing shader reads without readback or re-upload");
        canonical[0]=0x11;
        check(context->publications==1 && use(true) && context->uploads==2,"a CPU write still re-uploads depth for a bypassing read");
        check(use(false) && context->uploads==3,
            "an image decoded for a bypassing read is re-decoded through the depth block before depth use");
        if(FAILED(release_title_gpu_resource(texture,fence.Get(),value-1)))return 1;
        release(canonical,attributes);
    }
    {
        auto* canonical=static_cast<BYTE*>(allocate(65536,attributes));
        if(!canonical)return 1;
        std::memset(canonical,0xa7,64);
        std::atomic<UINT> callback_frees{0};
        TitleGpuResource buffer;
        if(acquire_title_gpu_buffer(canonical,64,device.Get(),buffer,D3D12_RESOURCE_FLAG_NONE,true)!=S_OK)return 1;
        buffer.allocation_owner=std::shared_ptr<void>(canonical,[release,attributes,&callback_frees](void* address) {
            release(address,attributes);++callback_frees;
        });
        std::weak_ptr<void> lifetime=buffer.allocation_owner;
        ComPtr<ID3D12Fence> gate,queued;
        ComPtr<ID3D12CommandAllocator> second_allocator;
        ComPtr<ID3D12GraphicsCommandList> second_list;
        if(FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&gate))) ||
            FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&queued))) ||
            FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&second_allocator))) ||
            FAILED(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,second_allocator.Get(),nullptr,IID_PPV_ARGS(&second_list))) ||
            FAILED(allocator->Reset()) || FAILED(list->Reset(allocator.Get(),nullptr)))return 1;
        std::shared_ptr<void> leases[2];TitleGpuResource* resources[]{&buffer};
        ID3D12GraphicsCommandList* commands[]{list.Get(),second_list.Get()};
        if(FAILED(queue->Wait(gate.Get(),1)))return 1;
        for(UINT epoch=0;epoch<2;++epoch) {
            auto* command=commands[epoch];
            if(begin_title_gpu_buffer(buffer,command,D3D12_RESOURCE_STATE_COPY_SOURCE,true,queue.Get())!=S_OK ||
                end_title_gpu_buffer(buffer,command)!=S_OK)return 1;
            barrier(command,buffer.resource,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_SOURCE);
            command->CopyBufferRegion(readback.Get(),epoch*64,buffer.resource,0,64);
            barrier(command,buffer.resource,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
            if(FAILED(command->Close()) || prepare_title_gpu_submission(resources,leases[epoch])!=S_OK)return 1;
            ID3D12CommandList* pending[]{command};queue->ExecuteCommandLists(1,pending);
            if(FAILED(queue->Signal(queued.Get(),epoch+1)) || publish_title_gpu_submission(leases[epoch],queue.Get(),queued.Get(),epoch+1)!=S_OK)return 1;
            void* current=nullptr;
            check(map_title_gpu_resource(buffer,&current)==S_OK && current==canonical,
                "CPU maps can reuse canonical bytes after a readonly native snapshot was captured");
            if(!current)return 1;
            std::memset(current,epoch ? 0x52:0x31,64);
            if(unmap_title_gpu_resource(buffer)!=S_OK)return 1;
        }
        check(queued->GetCompletedValue()==0 && release_title_gpu_resource(buffer,queued.Get(),2)==E_PENDING &&
            release_title_gpu_resource(buffer,queued.Get(),2,true)==S_OK && !callback_frees && !lifetime.expired(),
            "readonly buffer snapshots retain both queued versions and allocation callbacks after logical ownership is released");
        if(FAILED(gate->Signal(1)))return 1;
        HANDLE done=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!done)return 1;
        const bool completed=SUCCEEDED(queued->SetEventOnCompletion(2,done)) && WaitForSingleObject(done,3000)==WAIT_OBJECT_0;
        CloseHandle(done);if(!completed)return 1;
        void* bytes=nullptr;if(FAILED(readback->Map(0,nullptr,&bytes)))return 1;
        check(std::all_of(static_cast<BYTE*>(bytes),static_cast<BYTE*>(bytes)+64,[](BYTE value){return value==0xa7;}) &&
            std::all_of(static_cast<BYTE*>(bytes)+64,static_cast<BYTE*>(bytes)+128,[](BYTE value){return value==0x31;}) &&
            std::all_of(canonical,canonical+64,[](BYTE value){return value==0x52;}),
            "queued native buffer copies consume immutable snapshots and preserve later CPU writes");
        readback->Unmap(0,nullptr);leases[0].reset();leases[1].reset();
        check(callback_frees==1 && lifetime.expired(),"the original allocation callback runs exactly once after all queued fences retire");
    }
    {
        auto context=std::make_shared<LinearTextureFixture>();context->width=32;context->height=96;
        context->offsets.resize(32*96);for(UINT i=0;i<context->offsets.size();++i)context->offsets[i]=i*4;
        TitleTextureLayout layout;layout.bytes=32*96*4;layout.alignment=16;layout.context=context;
        layout.copy=linear_texture_copy;layout.offsets=linear_texture_offsets;layout.demand_coherence=true;
        auto& desc=layout.resource_desc;desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width=context->width;desc.Height=context->height;desc.DepthOrArraySize=desc.MipLevels=1;
        desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        auto* canonical=pool_memory+3*65536-8192;
        auto* alias=pool_alias+65536-8192;
        std::memset(canonical-16,0x5a,layout.bytes+32);
        TitleGpuResource texture;
        if(acquire_title_gpu_texture(canonical,layout,device.Get(),texture)!=S_OK)return 1;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT rows=0;UINT64 row_bytes=0,total=0;
        device->GetCopyableFootprints(&desc,0,1,0,&footprint,&rows,&row_bytes,&total);
        std::vector<BYTE> expected(layout.bytes);
        for(size_t i=0;i<expected.size();++i)expected[i]=BYTE(i*37+i/128);
        void* mapped=nullptr;if(FAILED(upload->Map(0,nullptr,&mapped)))return 1;
        for(UINT y=0;y<rows;++y)std::memcpy(static_cast<BYTE*>(mapped)+y*footprint.Footprint.RowPitch,expected.data()+y*row_bytes,SIZE_T(row_bytes));
        upload->Unmap(0,nullptr);
        if(FAILED(allocator->Reset()) || FAILED(list->Reset(allocator.Get(),nullptr)) ||
            begin_title_gpu_texture(texture,list.Get(),D3D12_RESOURCE_STATE_COPY_DEST)!=S_OK)return 1;
        D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=upload.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;from.PlacedFootprint=footprint;
        to.pResource=texture.resource;to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
        if(end_title_gpu_texture(texture,list.Get())!=S_OK || FAILED(list->Close()))return 1;
        queue->ExecuteCommandLists(1,lists);
        const UINT64 value=fence->GetCompletedValue()+1;
        HANDLE ready=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!ready)return 1;
        hr=queue->Signal(fence.Get(),value);if(SUCCEEDED(hr))hr=fence->SetEventOnCompletion(value,ready);
        const bool done=SUCCEEDED(hr) && WaitForSingleObject(ready,3000)==WAIT_OBJECT_0;CloseHandle(ready);
        if(!done || complete_title_gpu_resource(texture,fence.Get(),value)!=S_OK)return 1;
        bool sealed=true;MEMORY_BASIC_INFORMATION state{},left{},right{};
        for(auto* address:{canonical,canonical+4096,canonical+8192,alias,alias+4096})
            sealed=sealed && VirtualQuery(address,&state,sizeof state) && state.Protect==PAGE_NOACCESS;
        check(sealed && VirtualQuery(canonical,&left,sizeof left) && VirtualQuery(canonical+8192,&right,sizeof right) && left.AllocationBase!=right.AllocationBase,
            "batched protection seals contiguous aliases without crossing native view boundaries");
        SetLastError(0x2468);
        const BYTE observed=*static_cast<volatile BYTE*>(alias+4096);
        check(observed==expected[4096] && GetLastError()==0x2468 && !std::memcmp(canonical,expected.data(),expected.size()) &&
            !std::memcmp(alias,expected.data(),8192) && !context->uploads && !context->publications && canonical[-1]==0x5a && canonical[layout.bytes]==0x5a,
            "fault-driven publication preserves every page, physical alias, guard and last error across ESRAM/private views");
        check(release_title_gpu_resource(texture,fence.Get(),value)==S_OK,"mixed-view tracked texture retires after its actual fence");
        bool restored=true;
        for(auto* address:{canonical,canonical+4096,canonical+8192,alias,alias+4096})
            restored=restored && VirtualQuery(address,&state,sizeof state) && state.Protect==PAGE_READWRITE;
        check(restored,"retirement restores all mixed-view pages and aliases");
    }
    if(original_xg && !bgra_only)for(const auto format:{DXGI_FORMAT_R32G8X24_TYPELESS,DXGI_FORMAT_R11G11B10_FLOAT,DXGI_FORMAT_R16G16B16A16_FLOAT}) {
        TitleTextureLayout layout;
        const UINT bind=format==DXGI_FORMAT_R32G8X24_TYPELESS ? D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE:D3D11_BIND_RENDER_TARGET;
        hr=create_xg_texture_layout(3,5,1,1,format,1,layout,bind,4);
        if(FAILED(hr))return 1;
        const SIZE_T bytes=layout.bytes+layout.alignment+32;
        auto* region=static_cast<BYTE*>(virtual_allocate(nullptr,bytes,0x30002000,PAGE_READWRITE));
        if(!region || virtual_allocate(region,bytes,0x30001000,PAGE_READWRITE)!=region)return 1;
        auto* canonical=reinterpret_cast<BYTE*>((reinterpret_cast<std::uintptr_t>(region)+16+layout.alignment-1)&~(layout.alignment-1));
        check(sample_fixture(std::move(layout),fence->GetCompletedValue()+1,canonical),
            "tracked raw GPU publication preserves original MSAA samples, metadata, guards and CPU reuse");
        check(virtual_free(region,0,MEM_RELEASE),"tracked MSAA fixture retires its owned graphics reservation");
    }
    check(virtual_free(pool_memory, 0, MEM_RELEASE) && virtual_free(pool_alias, 0, MEM_RELEASE), "GPU ESRAM fixtures release every owned reservation after retirement");
    if(original_xg && !bgra_only) {
        TitleTextureLayout layouts[3];
        for(UINT i=0;i<3;++i)
            if(FAILED(create_xg_texture_layout(32,16,1,1,DXGI_FORMAT_R8G8B8A8_UNORM,1,layouts[i],
                D3D11_BIND_SHADER_RESOURCE|(i==2 ? 0:D3D11_BIND_RENDER_TARGET))))return 1;
        auto* allocation=static_cast<BYTE*>(allocate(3*65536,attributes));if(!allocation)return 1;
        auto* canonical=allocation+65536;std::memset(allocation,0x5a,3*65536);
        std::vector<BYTE> pixels(32*16*4,0x69);
        if(FAILED(layouts[0].copy(layouts[0].context.get(),true,pixels.data(),canonical,0,128,2048,nullptr)))return 1;
        TitleGpuResource first,shared,other_flags,other_range;
        if(acquire_title_gpu_texture(canonical,layouts[0],device.Get(),first)!=S_OK ||
            acquire_title_gpu_texture(canonical,layouts[1],device.Get(),shared)!=S_OK ||
            acquire_title_gpu_texture(canonical,layouts[2],device.Get(),other_flags)!=S_OK ||
            acquire_title_gpu_texture(allocation+2*65536,layouts[1],device.Get(),other_range)!=S_OK)return 1;
        check(first.resource==shared.resource && first.representation==shared.representation && layouts[0].context!=layouts[1].context,
            "independently created original XG layouts reuse live native storage for the same description and pinned range");
        check(first.resource!=other_flags.resource && first.resource!=other_range.resource,
            "native storage keys distinguish resource capabilities and physical ranges");
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};device->GetCopyableFootprints(&layouts[0].resource_desc,0,1,0,&footprint,nullptr,nullptr,nullptr);
        const auto capture=[&](TitleGpuResource& texture,UINT64 value,BYTE expected,bool release_first=false) {
            if(FAILED(allocator->Reset()) || FAILED(list->Reset(allocator.Get(),nullptr)) ||
                begin_title_gpu_texture(texture,list.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,false,nullptr,true)!=S_OK)return false;
            if(release_first && (release_title_gpu_resource(first,fence.Get(),fence->GetCompletedValue(),true)!=S_OK || first.resource))return false;
            D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=texture.resource;from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            to.pResource=readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=footprint;
            list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
            if(end_title_gpu_texture(texture,list.Get())!=S_OK || FAILED(list->Close()))return false;
            queue->ExecuteCommandLists(1,lists);HANDLE done=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!done)return false;
            const bool completed=SUCCEEDED(queue->Signal(fence.Get(),value)) && SUCCEEDED(fence->SetEventOnCompletion(value,done)) &&
                WaitForSingleObject(done,3000)==WAIT_OBJECT_0;CloseHandle(done);
            if(!completed || complete_title_gpu_resource(texture,fence.Get(),value)!=S_OK)return false;
            void* mapped=nullptr;if(FAILED(readback->Map(0,nullptr,&mapped)))return false;
            bool exact=true;
            for(UINT y=0;y<16;++y) {
                const auto* row=static_cast<BYTE*>(mapped)+footprint.Offset+SIZE_T(y)*footprint.Footprint.RowPitch;
                exact=exact && std::all_of(row,row+128,[expected](BYTE value){return value==expected;});
            }
            readback->Unmap(0,nullptr);return exact;
        };
        check(capture(first,6000,0x69) && capture(shared,6001,0x69),"cached native aliases preserve exact GPU pixels on reuse");
        std::fill(pixels.begin(),pixels.end(),0x31);
        if(FAILED(layouts[1].copy(layouts[1].context.get(),true,pixels.data(),canonical,0,128,2048,nullptr)))return 1;
        check(capture(shared,6002,0x31) && canonical[-1]==0x5a && canonical[layouts[0].bytes]==0x5a,
            "CPU writes invalidate cached native aliases without changing canonical guards");
        check(capture(shared,6003,0x31,true),"releasing a cached logical owner preserves another alias's active command list and exact pixels");
        if(first.resource || release_title_gpu_resource(other_flags,fence.Get(),6003)!=S_OK ||
            release_title_gpu_resource(other_range,fence.Get(),6003)!=S_OK)return 1;
        release(allocation,attributes);MEMORY_BASIC_INFORMATION state{};
        check(VirtualQuery(allocation,&state,sizeof state) && state.State==MEM_COMMIT,
            "a remaining cached alias retains the allocation after its first logical owner is released");
        check(release_title_gpu_resource(shared,fence.Get(),6003)==S_OK && VirtualQuery(allocation,&state,sizeof state) && state.State==MEM_FREE,
            "last cached native alias retires its storage and original allocation exactly once");
    }
    if(original_xg && !bgra_only) for(const UINT width:{3u,513u})for(const UINT height:{5u,513u})for(const auto format:{DXGI_FORMAT_R32G8X24_TYPELESS,DXGI_FORMAT_R11G11B10_FLOAT,DXGI_FORMAT_R16G16B16A16_FLOAT}) {
        if(width==513 && (height!=513 || format!=DXGI_FORMAT_R32G8X24_TYPELESS))continue;
        constexpr UINT samples=4;
        const bool depth_stencil=format==DXGI_FORMAT_R32G8X24_TYPELESS;
        const UINT element=format==DXGI_FORMAT_R11G11B10_FLOAT ? 4:8;
        const UINT bind=(depth_stencil ? D3D11_BIND_DEPTH_STENCIL:D3D11_BIND_RENDER_TARGET)|D3D11_BIND_SHADER_RESOURCE;
        TitleTextureLayout xg;
        hr=create_xg_texture_layout(width,height,1,1,format,1,xg,
            bind,samples);
        if(FAILED(hr))return 1;
        std::vector<BYTE> canonical(xg.bytes+32,0x5a),linear(width*height*samples*element);
        for(UINT iteration=0;iteration<2;++iteration)
            if(FAILED(xg.copy(xg.context.get(),true,linear.data(),canonical.data()+16,0,width*samples*element,UINT(linear.size()),nullptr)))return 1;
        TitleTextureSampleOffsets offsets;
        check(xg.sample_offsets && xg.sample_offsets(xg.context.get(),&offsets)==S_OK,
            "original XG exposes immutable addresses for each raw sample representation");
        if(!offsets.count)return 1;
        D3D12_RESOURCE_DESC image{};image.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;image.Width=width;image.Height=height;
        image.DepthOrArraySize=samples;image.MipLevels=1;image.Format=depth_stencil ? DXGI_FORMAT_R32G32_UINT : element==4 ? DXGI_FORMAT_R11G11B10_FLOAT : DXGI_FORMAT_R16G16B16A16_UINT;image.SampleDesc.Count=1;
        D3D12_HEAP_PROPERTIES image_heap{};image_heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        ComPtr<ID3D12Resource> array;
        if(FAILED(device->CreateCommittedResource(&image_heap,D3D12_HEAP_FLAG_NONE,&image,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&array))))return 1;
        std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT,samples> footprints;
        UINT64 bytes=0;device->GetCopyableFootprints(&image,0,samples,0,footprints.data(),nullptr,nullptr,&bytes);
        ComPtr<ID3D12Resource> sample_upload=upload,sample_readback=readback;
        if(bytes>sample_upload->GetDesc().Width) {
            auto staging=sample_upload->GetDesc();staging.Width=bytes;
            D3D12_HEAP_PROPERTIES staging_heap{};staging_heap.Type=D3D12_HEAP_TYPE_UPLOAD;
            if(FAILED(device->CreateCommittedResource(&staging_heap,D3D12_HEAP_FLAG_NONE,&staging,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&sample_upload))))return 1;
            staging_heap.Type=D3D12_HEAP_TYPE_READBACK;
            if(FAILED(device->CreateCommittedResource(&staging_heap,D3D12_HEAP_FLAG_NONE,&staging,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&sample_readback))))return 1;
        }
        TextureLayoutTransfer transfer;
        auto map_owner=xg.context;std::weak_ptr<void> retained_owner=map_owner;
        hr=transfer.initialize_samples(device.Get(),offsets,width,height,footprints,xg.bytes,map_owner);
        if(FAILED(hr))std::cerr<<"sample layout initialize 0x"<<std::hex<<hr<<std::dec<<'\n';
        check(hr==S_OK,"GPU sample layout creates a byte-exact raw transport from original XG tables");if(hr!=S_OK)return 1;
        const UINT plane=depth_stencil ? 1:0;
        auto invalid=offsets;std::vector<UINT32> duplicate(offsets.planes[plane],offsets.planes[plane]+offsets.count);duplicate[1]=duplicate[0];
        invalid.planes[plane]=duplicate.data();TextureLayoutTransfer rejected;
        check(rejected.initialize_samples(device.Get(),invalid,width,height,footprints,xg.bytes,map_owner)==HRESULT_FROM_WIN32(ERROR_INVALID_DATA),
            "sample layout rejects duplicate byte destinations before a racy scatter");
        if(depth_stencil) {
            invalid=offsets;duplicate[0]=offsets.planes[0][0];invalid.planes[1]=duplicate.data();
            check(rejected.initialize_samples(device.Get(),invalid,width,height,footprints,xg.bytes,map_owner)==HRESULT_FROM_WIN32(ERROR_INVALID_DATA),
                "sample layout rejects overlapping depth and stencil destinations");
            auto bad=canonical;bad[16+offsets.htile_offset]^=1;
            check(transfer.validate_samples(bad.data()+16,false)==DXGI_ERROR_UNSUPPORTED && transfer.validate_samples(bad.data()+16,true)==S_OK,
                "GPU sample ingress preserves expanded-HTILE validation and the verified texture-unit bypass");
            bad=canonical;const UINT32 nan=0x7fc00001u;std::memcpy(bad.data()+16+offsets.planes[0][0],&nan,4);
            check(transfer.validate_samples(bad.data()+16,true)==DXGI_ERROR_UNSUPPORTED,
                "GPU sample ingress refuses unrepresentable depth payloads before submitting work");
        }
        xg.context.reset();map_owner.reset();
        check(!retained_owner.expired(),"GPU sample validation retains its immutable original-XG table after producer release");
        for(UINT iteration=0;iteration<3;++iteration) {
            for(UINT i=0;i<width*height*samples;++i) {
                const UINT32 value=(depth_stencil ? 0x3e000000u:0x7fc00001u)+i*17+iteration;
                std::memcpy(linear.data()+i*element,&value,4);
                if(element==8) {
                    const UINT32 second=depth_stencil ? BYTE(i*13+iteration):0xfe01fc00u^(i*257+iteration);
                    std::memcpy(linear.data()+i*element+4,&second,4);
                }
            }
            canonical[16+xg.bytes-1]=BYTE(0x61+iteration);
            TitleTextureLayout oracle;
            hr=create_xg_texture_layout(width,height,1,1,format,1,oracle,
                bind,samples);
            if(FAILED(hr) || FAILED(oracle.copy(oracle.context.get(),true,linear.data(),canonical.data()+16,0,width*samples*element,UINT(linear.size()),nullptr)))return 1;
            const auto ingress=linear;
            if(FAILED(transfer.validate_samples(canonical.data()+16,false)) || FAILED(transfer.prepare(canonical.data()+16)) ||
                FAILED(allocator->Reset()) || FAILED(list->Reset(allocator.Get(),nullptr)))return 1;
            transfer.detile_samples(list.Get(),array.Get());
            barrier(list.Get(),array.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_SOURCE);
            for(UINT sample=0;sample<samples;++sample) {
                D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=array.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;from.SubresourceIndex=sample;
                to.pResource=sample_readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=footprints[sample];
                list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
            }
            barrier(list.Get(),array.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
            for(UINT i=0;i<width*height*samples;++i){linear[i*element]^=1;if(element==8)linear[i*element+4]^=0x5a;}
            auto expected=canonical;TitleTextureLayout expected_layout;
            hr=create_xg_texture_layout(width,height,1,1,format,1,expected_layout,
                bind,samples);
            if(FAILED(hr) || FAILED(expected_layout.copy(expected_layout.context.get(),true,linear.data(),expected.data()+16,0,width*samples*element,UINT(linear.size()),nullptr)))return 1;
            void* mapped=nullptr;if(FAILED(sample_upload->Map(0,nullptr,&mapped)))return 1;std::memset(mapped,0xa7,SIZE_T(bytes));
            for(UINT sample=0;sample<samples;++sample)for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x) {
                auto* destination=static_cast<BYTE*>(mapped)+footprints[sample].Offset+SIZE_T(y)*footprints[sample].Footprint.RowPitch+x*element;
                std::memcpy(destination,linear.data()+((y*width+x)*samples+sample)*element,element);
                if(depth_stencil){destination[5]=0xab;destination[6]=0xcd;destination[7]=0xef;}
            }
            sample_upload->Unmap(0,nullptr);
            for(UINT sample=0;sample<samples;++sample) {
                D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=sample_upload.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;from.PlacedFootprint=footprints[sample];
                to.pResource=array.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;to.SubresourceIndex=sample;
                list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
            }
            barrier(list.Get(),array.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_SOURCE);
            transfer.retile_samples(list.Get(),array.Get());
            barrier(list.Get(),array.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
            if(FAILED(list->Close()))return 1;queue->ExecuteCommandLists(1,lists);
            const UINT64 value=fence->GetCompletedValue()+1;HANDLE ready=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!ready)return 1;
            hr=queue->Signal(fence.Get(),value);if(SUCCEEDED(hr))hr=fence->SetEventOnCompletion(value,ready);
            const bool completed=SUCCEEDED(hr) && WaitForSingleObject(ready,3000)==WAIT_OBJECT_0;CloseHandle(ready);if(!completed)return 1;
            if(FAILED(sample_readback->Map(0,nullptr,&mapped)))return 1;
            bool exact=true;
            for(UINT sample=0;sample<samples;++sample)for(UINT y=0;y<height;++y)for(UINT x=0;x<width;++x)
                exact=exact && !std::memcmp(static_cast<BYTE*>(mapped)+footprints[sample].Offset+SIZE_T(y)*footprints[sample].Footprint.RowPitch+x*element,
                    ingress.data()+((y*width+x)*samples+sample)*element,element);
            check(exact,"GPU sample detile preserves every raw bit in independent color or depth/stencil slices");sample_readback->Unmap(0,nullptr);
            if(FAILED(transfer.readback()->Map(0,nullptr,&mapped)))return 1;
            check(!std::memcmp(mapped,expected.data()+16,xg.bytes),"GPU sample retile matches uncached original XG, metadata, padding and CPU edits");
            std::memcpy(canonical.data()+16,mapped,xg.bytes);transfer.readback()->Unmap(0,nullptr);
            check(canonical==expected,"GPU sample publication preserves every neighboring guard byte");
        }
        TitleTextureLayout narrow_layout;
        hr=create_xg_texture_layout(width,height,1,1,format,1,narrow_layout,
            bind,samples);
        check(SUCCEEDED(hr) && sample_fixture(std::move(narrow_layout),fence->GetCompletedValue()+1),
            "GPU sample transport preserves native clear, fence gating, CPU reuse and the original format validation");
    }
    for(const bool sparse:{false,true}) {
        const auto fail=[&](int line){
            std::cerr<<"scatter failed line="<<line<<" hr=0x"<<std::hex<<hr<<" device=0x"<<device->GetDeviceRemovedReason()<<std::dec<<'\n';
            ComPtr<ID3D12InfoQueue> info;
            if(SUCCEEDED(device.As(&info))) {
                const auto count=info->GetNumStoredMessages();
                for(UINT64 i=count>12 ? count-12:0;i<count;++i) {
                    SIZE_T size=0;info->GetMessage(i,nullptr,&size);if(size>8192)continue;
                    std::vector<BYTE> storage(size);auto* message=reinterpret_cast<D3D12_MESSAGE*>(storage.data());
                    if(SUCCEEDED(info->GetMessage(i,message,&size)))std::cerr<<message->pDescription<<'\n';
                }
            }
            return 1;
        };
        ComPtr<ID3D12Fence> fixture_fence;
        if(FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fixture_fence))))return fail(__LINE__);
        HANDLE drained=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!drained)return fail(__LINE__);
        hr=queue->Signal(fixture_fence.Get(),1);if(SUCCEEDED(hr))hr=fixture_fence->SetEventOnCompletion(1,drained);
        const bool idle=SUCCEEDED(hr) && WaitForSingleObject(drained,3000)==WAIT_OBJECT_0;CloseHandle(drained);if(!idle)return fail(__LINE__);
        struct Addresses { std::array<UINT32,2> depth{12,16},stencil{8,22}; };
        auto owner=std::make_shared<Addresses>();if(sparse)owner->stencil[1]=1000;
        const SIZE_T bytes=sparse ? 1024:64;
        TitleTextureSampleOffsets map;map.planes={owner->depth.data(),owner->stencil.data()};map.count=2;
        map.htile_offset=bytes-4;map.htile_bytes=4;map.expanded_htile=0xfffff3ffu;
        D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=desc.Height=1;
        desc.DepthOrArraySize=2;desc.MipLevels=1;desc.Format=DXGI_FORMAT_R32G32_UINT;desc.SampleDesc.Count=1;
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        ComPtr<ID3D12Resource> array;
        if(FAILED(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&array))))return fail(__LINE__);
        std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT,2> footprints;
        device->GetCopyableFootprints(&desc,0,2,0,footprints.data(),nullptr,nullptr,nullptr);
        TextureLayoutTransfer transfer;
        hr=transfer.initialize_samples(device.Get(),map,1,1,footprints,bytes,owner);
        if(hr!=S_OK)return fail(__LINE__);
        std::vector<BYTE> canonical(bytes,0x5a);
        for(UINT sample=0;sample<2;++sample){const UINT depth=0x3e800000u+sample*0x800000u;std::memcpy(canonical.data()+owner->depth[sample],&depth,4);canonical[owner->stencil[sample]]=BYTE(sample*37+5);}
        std::memcpy(canonical.data()+map.htile_offset,&map.expanded_htile,4);
        if(FAILED(transfer.prepare(canonical.data())) || FAILED(allocator->Reset()) || FAILED(list->Reset(allocator.Get(),nullptr)))return fail(__LINE__);
        transfer.detile_samples(list.Get(),array.Get());barrier(list.Get(),array.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_SOURCE);
        for(UINT sample=0;sample<2;++sample) {
            D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=array.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;from.SubresourceIndex=sample;
            to.pResource=readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=footprints[sample];list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
        }
        auto expected=canonical;void* mapped=nullptr;if(FAILED(upload->Map(0,nullptr,&mapped)))return fail(__LINE__);
        for(UINT sample=0;sample<2;++sample) {
            const UINT values[]{0x3e000000u+sample*0x1000000u,0xaabbcc00u+sample*71+13};
            std::memcpy(static_cast<BYTE*>(mapped)+footprints[sample].Offset,values,sizeof values);
            std::memcpy(expected.data()+owner->depth[sample],values,4);expected[owner->stencil[sample]]=BYTE(values[1]);
        }
        upload->Unmap(0,nullptr);barrier(list.Get(),array.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COPY_DEST);
        for(UINT sample=0;sample<2;++sample) {
            D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=upload.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;from.PlacedFootprint=footprints[sample];
            to.pResource=array.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;to.SubresourceIndex=sample;list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
        }
        barrier(list.Get(),array.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COPY_SOURCE);transfer.retile_samples(list.Get(),array.Get());
        if(FAILED(list->Close()))return fail(__LINE__);queue->ExecuteCommandLists(1,lists);
        const UINT64 value=2;HANDLE ready=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!ready)return fail(__LINE__);
        hr=queue->Signal(fixture_fence.Get(),value);if(SUCCEEDED(hr))hr=fixture_fence->SetEventOnCompletion(value,ready);
        const bool done=SUCCEEDED(hr) && WaitForSingleObject(ready,3000)==WAIT_OBJECT_0;CloseHandle(ready);
        if(!done){std::cerr<<"scatter fence="<<fixture_fence->GetCompletedValue()<<" requested="<<value<<'\n';return fail(__LINE__);}
        if(FAILED(readback->Map(0,nullptr,&mapped)))return fail(__LINE__);bool exact=true;
        for(UINT sample=0;sample<2;++sample){UINT values[2]{};std::memcpy(values,static_cast<BYTE*>(mapped)+footprints[sample].Offset,sizeof values);exact=exact && values[0]==0x3e800000u+sample*0x800000u && values[1]==sample*37+5;}
        readback->Unmap(0,nullptr);check(exact,"two-sample depth/stencil detile preserves the independent address planes");
        if(FAILED(transfer.readback()->Map(0,nullptr,&mapped)))return fail(__LINE__);
        check(!std::memcmp(mapped,expected.data(),bytes),sparse ? "sparse atomic stencil scatter preserves every unmapped byte" :
            "stencil scatter preserves partial words and intervening depth words");
        transfer.readback()->Unmap(0,nullptr);
    }
    return failures ? 1 : 0;
}
