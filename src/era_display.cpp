// SPDX-License-Identifier: BSD-3-Clause
#include "era_display.h"
#include "era_session.h"
#include "era_memory.h"
#include <d3dcompiler.h>
#include <cstring>
#include <climits>
#include <utility>
#include <cstdio>
#include <atomic>
#include <algorithm>
#include <cstdlib>
#include <wrl/wrappers/corewrappers.h>

namespace xboxone::era {
namespace {
std::atomic<SIZE_T> layout_bytes{0};
std::atomic<SIZE_T> sample_layout_bytes{0};
constexpr SIZE_T layout_budget=384u<<20,sample_layout_budget=512u<<20;
void layout_transition(ID3D12GraphicsCommandList* list,ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};list->ResourceBarrier(1,&barrier);
}
HRESULT cache_immutable_offsets(ID3D12Device* device,ID3D12Resource* upload,SIZE_T bytes,Microsoft::WRL::ComPtr<ID3D12Resource>& out) {
    auto desc=upload->GetDesc();D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
    HRESULT hr=device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&out));
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    const D3D12_COMMAND_QUEUE_DESC q{};
    if(SUCCEEDED(hr))hr=device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue));
    if(SUCCEEDED(hr))hr=device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator));
    if(SUCCEEDED(hr))hr=device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list));
    if(SUCCEEDED(hr))hr=device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence));
    if(FAILED(hr))return hr;
    Microsoft::WRL::Wrappers::Event completed(CreateEventW(nullptr,FALSE,FALSE,nullptr));
    if(!completed.IsValid())return HRESULT_FROM_WIN32(GetLastError());
    list->CopyBufferRegion(out.Get(),0,upload,0,bytes);
    layout_transition(list.Get(),out.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    hr=list->Close();if(SUCCEEDED(hr))hr=fence->SetEventOnCompletion(1,completed.Get());if(FAILED(hr))return hr;
    ID3D12CommandList* lists[]{list.Get()};queue->ExecuteCommandLists(1,lists);hr=queue->Signal(fence.Get(),1);
    // Submitted initialization must retire before local staging and allocator lifetimes end.
    if(FAILED(hr) || WaitForSingleObject(completed.Get(),5000)!=WAIT_OBJECT_0 || fence->GetCompletedValue()!=1) {
        std::fputs("xone-era: immutable offset initialization did not retire\n",stderr);
        RaiseFailFastException(nullptr,nullptr,0);std::abort();
    }
    return S_OK;
}
}
TextureLayoutTransfer::~TextureLayoutTransfer() {
    offsets_.Reset();upload_.Reset();linear_.Reset();tiled_.Reset();readback_.Reset();
    if(retained_bytes_)(sample_transport_ ? sample_layout_bytes:layout_bytes).fetch_sub(retained_bytes_);
}
HRESULT TextureLayoutTransfer::initialize(ID3D12Device* device,std::span<const UINT32> offsets,UINT width,UINT height,
        const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint,SIZE_T tiled_bytes) try {
    if(!device || retained_bytes_)return E_INVALIDARG;
    const UINT element=footprint.Footprint.Format>=DXGI_FORMAT_R8_TYPELESS && footprint.Footprint.Format<=DXGI_FORMAT_A8_UNORM ? 1:4;
    const UINT64 count=UINT64(width)*height,linear_bytes=UINT64(footprint.Footprint.RowPitch)*height;
    if(!width || !height || offsets.size()!=count || !tiled_bytes || footprint.Offset ||
        footprint.Footprint.Width!=width || footprint.Footprint.Height!=height || footprint.Footprint.Depth!=1 ||
        footprint.Footprint.RowPitch<UINT64(width)*element || footprint.Footprint.RowPitch%4)return E_INVALIDARG;
    if(count>65535u*64 || tiled_bytes>(16u<<20) || linear_bytes>(16u<<20))return S_FALSE;
    const SIZE_T padded=(tiled_bytes+3)&~SIZE_T(3);
    auto sorted=std::vector<UINT32>(offsets.begin(),offsets.end());std::sort(sorted.begin(),sorted.end());
    for(SIZE_T i=0;i<sorted.size();++i)
        if(sorted[i]%element || sorted[i]>tiled_bytes || tiled_bytes-sorted[i]<element || (i && sorted[i]==sorted[i-1]))return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    SIZE_T end=0;for(auto offset:sorted){if(offset>end)padding_.push_back({end,offset-end});end=SIZE_T(offset)+element;}
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
 uint linear_address=(id.x/width)*pitch+(id.x%width)*SAMPLE_BYTES;uint tiled_address=offsets.Load(id.x*4);
 uint source=into_tiled?linear_address:tiled_address;uint destination=into_tiled?tiled_address:linear_address;
#if SAMPLE_BYTES==1
 uint value=(input.Load(source&~3u)>>((source&3u)*8))&255u;
 uint shift=(destination&3u)*8;uint ignored;
 output.InterlockedAnd(destination&~3u,~(255u<<shift),ignored);
 output.InterlockedOr(destination&~3u,value<<shift,ignored);
#else
 output.Store(destination,input.Load(source));
#endif
})";
    return initialize_program(device,source,sizeof(source)-1,4,element);
}catch(const std::bad_alloc&){return E_OUTOFMEMORY;}
HRESULT TextureLayoutTransfer::initialize_program(ID3D12Device* device,const char* source,SIZE_T bytes,UINT constants,UINT sample_bytes,bool depth_stencil) {
    struct Compiler{HMODULE value=LoadLibraryExW(L"d3dcompiler_47.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);~Compiler(){if(value)FreeLibrary(value);}}compiler;
    if(!compiler.value)return HRESULT_FROM_WIN32(GetLastError());
    const auto compile=reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(compiler.value,"D3DCompile"));
    if(!compile)return E_NOINTERFACE;
    Microsoft::WRL::ComPtr<ID3DBlob> code,errors,serialized;
    const D3D_SHADER_MACRO macros[]{{"SAMPLE_BYTES",sample_bytes==1 ? "1":sample_bytes==4 ? "4":"8"},{"DEPTH_STENCIL",depth_stencil ? "1":"0"},{nullptr,nullptr}};
    HRESULT hr=compile(source,bytes,nullptr,sample_bytes ? macros:nullptr,nullptr,"main","cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&errors);
    if(FAILED(hr) && errors)std::fprintf(stderr,"xone-era: layout shader: %.2048s\n",static_cast<const char*>(errors->GetBufferPointer()));
    if(FAILED(hr))return hr;
    D3D12_ROOT_PARAMETER parameters[4]{};
    for(UINT i=0;i<2;++i){parameters[i].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV;parameters[i].Descriptor.ShaderRegister=i;}
    parameters[2].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV;parameters[2].Descriptor.ShaderRegister=0;
    parameters[3].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;parameters[3].Constants={0,0,constants};
    const D3D12_ROOT_SIGNATURE_DESC root{4,parameters,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};
    hr=D3D12SerializeRootSignature(&root,D3D_ROOT_SIGNATURE_VERSION_1,&serialized,nullptr);
    if(SUCCEEDED(hr))hr=device->CreateRootSignature(0,serialized->GetBufferPointer(),serialized->GetBufferSize(),IID_PPV_ARGS(&root_));
    if(SUCCEEDED(hr)){D3D12_COMPUTE_PIPELINE_STATE_DESC desc{};desc.pRootSignature=root_.Get();desc.CS={code->GetBufferPointer(),code->GetBufferSize()};hr=device->CreateComputePipelineState(&desc,IID_PPV_ARGS(&pipeline_));}
    return hr;
}
HRESULT TextureLayoutTransfer::initialize_samples(ID3D12Device* device,const TitleTextureSampleOffsets& offsets,UINT width,UINT height,
        std::span<const D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints,SIZE_T tiled_bytes,std::shared_ptr<void> owner) try {
    if(!device || retained_bytes_ || !owner || !width || !height || footprints.size()<2 || footprints.size()>8 ||
        offsets.count!=UINT64(width)*height*footprints.size() || !offsets.planes[0] ||
        (offsets.depth_stencil ? (!offsets.planes[1] || offsets.element_bytes!=8) :
            (offsets.htile_bytes || (offsets.element_bytes!=4 && offsets.element_bytes!=8))) ||
        !tiled_bytes || tiled_bytes%4 || offsets.htile_offset%4 || offsets.htile_bytes%4 ||
        offsets.htile_offset>tiled_bytes || offsets.htile_bytes>tiled_bytes-offsets.htile_offset)return E_INVALIDARG;
    if(tiled_bytes>(64u<<20) || offsets.count>(16u<<20))return S_FALSE;
    const auto& first=footprints[0];const UINT64 slice=footprints[1].Offset;
    const auto format=offsets.depth_stencil ? DXGI_FORMAT_R32G32_UINT : offsets.element_bytes==4 ? DXGI_FORMAT_R11G11B10_FLOAT : DXGI_FORMAT_R16G16B16A16_UINT;
    if(first.Offset || !slice || slice>UINT_MAX || first.Footprint.Format!=format ||
        first.Footprint.RowPitch<UINT64(width)*offsets.element_bytes || slice<UINT64(first.Footprint.RowPitch)*height)return E_INVALIDARG;
    for(SIZE_T i=0;i<footprints.size();++i) {
        const auto& f=footprints[i];
        if(f.Offset!=i*slice || f.Footprint.Width!=width || f.Footprint.Height!=height || f.Footprint.Depth!=1 ||
            f.Footprint.Format!=first.Footprint.Format || f.Footprint.RowPitch!=first.Footprint.RowPitch)return E_INVALIDARG;
    }
    const UINT64 linear_bytes=footprints.back().Offset+UINT64(first.Footprint.RowPitch)*height;
    if(linear_bytes>(128u<<20))return S_FALSE;
    // Reference 140211120/140212330: footprints, original address tables and raw buffers.
    std::vector<BYTE> covered((tiled_bytes+7)/8,0);
    const auto mark=[&](SIZE_T at,UINT bytes) {
        if(at>tiled_bytes || bytes>tiled_bytes-at)return false;
        for(UINT n=0;n<bytes;++n) {
            const SIZE_T bit=at+n;const BYTE mask=BYTE(1u<<(bit%8));
            if(covered[bit/8]&mask)return false;covered[bit/8]|=mask;
        }
        return true;
    };
    for(SIZE_T at=0;at<offsets.htile_bytes;at+=4)
        if(!mark(offsets.htile_offset+at,4))return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    for(SIZE_T i=0;i<offsets.count;++i)
        if(offsets.planes[0][i]%4 || !mark(offsets.planes[0][i],offsets.depth_stencil ? 4:offsets.element_bytes) ||
            (offsets.depth_stencil && !mark(offsets.planes[1][i],1)))
            return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    if(offsets.depth_stencil) {
        for(SIZE_T at=0;at<tiled_bytes;) {
            if(covered[at/8]&(1u<<(at%8))){++at;continue;}
            const SIZE_T gap_start=at;
            do {++at;}while(at<tiled_bytes && !(covered[at/8]&(1u<<(at%8))));
            // Batch nearby gaps; intervening texels are rewritten by the same shader.
            if(!padding_.empty() && gap_start-padding_.back().first-padding_.back().second<=4096) {
                padding_.back().second=at-padding_.back().first;continue;
            }
            if(padding_.size()==512){padding_.assign(1,{0,tiled_bytes});break;} // Fragmented maps retain the full-copy path.
            padding_.emplace_back(gap_start,at-gap_start);
        }
    } else padding_.emplace_back(0,tiled_bytes);
    const UINT table_words=offsets.depth_stencil ? 2:1;
    const SIZE_T table_bytes=offsets.count*4*table_words,required=tiled_bytes*3+SIZE_T(linear_bytes)+table_bytes+padding_.capacity()*sizeof(padding_[0]);
    auto used=sample_layout_bytes.load();
    do {if(required>sample_layout_budget-used)return S_FALSE;}while(!sample_layout_bytes.compare_exchange_weak(used,used+required));
    retained_bytes_=required;sample_transport_=true;tiled_bytes_=tiled_bytes;width_=width;height_=height;sample_pitch_=UINT(slice);
    sample_owner_=std::move(owner);sample_depth_offsets_=offsets.planes[0];
    sample_depth_stencil_=offsets.depth_stencil;
    sample_footprints_.assign(footprints.begin(),footprints.end());
    htile_offset_=UINT(offsets.htile_offset);htile_words_=UINT(offsets.htile_bytes/4);expanded_htile_=offsets.expanded_htile;
    const auto create=[&](SIZE_T bytes,D3D12_HEAP_TYPE type,D3D12_RESOURCE_FLAGS flags,D3D12_RESOURCE_STATES state,auto& out) {
        D3D12_RESOURCE_DESC d{};d.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;d.Width=bytes;
        d.Height=d.DepthOrArraySize=d.MipLevels=1;d.SampleDesc.Count=1;d.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;d.Flags=flags;
        D3D12_HEAP_PROPERTIES heap{};heap.Type=type;
        return device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&d,state,nullptr,IID_PPV_ARGS(&out));
    };
    HRESULT hr=create(table_bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_GENERIC_READ,offsets_);
    if(SUCCEEDED(hr))hr=create(tiled_bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_GENERIC_READ,upload_);
    if(SUCCEEDED(hr))hr=create(tiled_bytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COMMON,tiled_);
    if(SUCCEEDED(hr))hr=create(SIZE_T(linear_bytes),D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COMMON,linear_);
    if(SUCCEEDED(hr))hr=create(tiled_bytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_FLAG_NONE,D3D12_RESOURCE_STATE_COPY_DEST,readback_);
    if(FAILED(hr))return hr;
    void* mapped=nullptr;const D3D12_RANGE no_read{0,0};hr=offsets_->Map(0,&no_read,&mapped);if(FAILED(hr))return hr;
    auto* words=static_cast<UINT32*>(mapped);
    for(SIZE_T i=0;i<offsets.count;++i){words[i*table_words]=offsets.planes[0][i];if(offsets.depth_stencil)words[i*2+1]=offsets.planes[1][i];}
    offsets_->Unmap(0,nullptr);
    static constexpr char source[]=R"(
ByteAddressBuffer offsets:register(t0);ByteAddressBuffer input:register(t1);RWByteAddressBuffer output:register(u0);
cbuffer Layout:register(b0){uint width;uint height;uint samples;uint pitch;uint slice_pitch;uint count;uint htile_offset;uint htile_words;uint expanded;uint into_tiled;};
[numthreads(64,1,1)] void main(uint3 id:SV_DispatchThreadID){
 uint i=id.y*(65535u*64u)+id.x;
 if(into_tiled && i<htile_words)output.Store(htile_offset+i*4,expanded);
 if(i>=count)return;
 uint sample_index=i%samples;uint pixel=i/samples;uint source=sample_index*slice_pitch+(pixel/width)*pitch+(pixel%width)*SAMPLE_BYTES;
#if DEPTH_STENCIL
 uint stencil=offsets.Load(i*8+4);uint shift=(stencil&3)*8;uint ignored;
 if(!into_tiled){output.Store(source,input.Load(offsets.Load(i*8)));output.Store(source+4,(input.Load(stencil&~3u)>>shift)&255u);return;}
 output.Store(offsets.Load(i*8),input.Load(source));
 output.InterlockedAnd(stencil&~3u,~(255u<<shift),ignored);
 output.InterlockedOr(stencil&~3u,(input.Load(source+4)&255u)<<shift,ignored);
#else
 uint base=offsets.Load(i*4);uint from=into_tiled?source:base;uint to=into_tiled?base:source;
#if SAMPLE_BYTES == 8
 output.Store2(to,input.Load2(from));
#else
 output.Store(to,input.Load(from));
#endif
#endif
})";
    hr=initialize_program(device,source,sizeof(source)-1,10,offsets.element_bytes,offsets.depth_stencil);
    if(FAILED(hr) || !offsets.depth_stencil)return hr;
    // The original-XG table is immutable. Cache it once, without per-dispatch DMA or a second retained table.
    used=sample_layout_bytes.load();
    do {if(table_bytes>sample_layout_budget-used)return S_OK;}while(!sample_layout_bytes.compare_exchange_weak(used,used+table_bytes));
    retained_bytes_+=table_bytes;
    Microsoft::WRL::ComPtr<ID3D12Resource> cached;
    hr=cache_immutable_offsets(device,offsets_.Get(),table_bytes,cached);
    if(SUCCEEDED(hr)) {
        offsets_=std::move(cached);
        wchar_t timing[2]{};
        if(GetEnvironmentVariableW(L"XONE_ERA_TIMING",timing,2)==1 && timing[0]==L'1') {
            SIZE_T bytes=0;for(const auto& span:padding_)bytes+=span.second;
            std::fprintf(stderr,"xone-era: immutable sample offsets cached once bytes=%zu padding_bytes=%zu padding_regions=%zu\n",table_bytes,bytes,padding_.size());
        }
    } else {cached.Reset();std::fprintf(stderr,"xone-era: immutable offset cache unavailable 0x%08lX; retaining upload table\n",hr);}
    retained_bytes_-=table_bytes;sample_layout_bytes.fetch_sub(table_bytes);
    return S_OK;
}catch(const std::bad_alloc&){return E_OUTOFMEMORY;}
HRESULT TextureLayoutTransfer::validate_samples(const void* canonical,bool bypass_htile) const {
    if(!canonical || !sample_owner_)return E_INVALIDARG;
    if(!sample_depth_stencil_)return S_OK; // Packed color and half-float payloads are transported as raw bits.
    const auto* bytes=static_cast<const BYTE*>(canonical);
    if(!bypass_htile)for(UINT i=0;i<htile_words_;++i) {
        UINT32 word;std::memcpy(&word,bytes+htile_offset_+SIZE_T(i)*4,4);
        if(word!=expanded_htile_)return DXGI_ERROR_UNSUPPORTED;
    }
    const SIZE_T count=SIZE_T(width_)*height_*sample_footprints_.size();
    for(SIZE_T i=0;i<count;++i) {
        UINT32 bits;std::memcpy(&bits,bytes+sample_depth_offsets_[i],4);
        if(bits>0x3f800000u || (bits && !(bits&0x7f800000u)))return DXGI_ERROR_UNSUPPORTED;
    }
    return S_OK;
}
void TextureLayoutTransfer::detile_samples(ID3D12GraphicsCommandList* list,ID3D12Resource* array) const {
    layout_transition(list,tiled_.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyBufferRegion(tiled_.Get(),0,upload_.Get(),0,tiled_bytes_);
    layout_transition(list,tiled_.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    layout_transition(list,linear_.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list->SetPipelineState(pipeline_.Get());list->SetComputeRootSignature(root_.Get());
    list->SetComputeRootShaderResourceView(0,offsets_->GetGPUVirtualAddress());list->SetComputeRootShaderResourceView(1,tiled_->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(2,linear_->GetGPUVirtualAddress());
    const UINT count=width_*height_*UINT(sample_footprints_.size());
    const UINT constants[]{width_,height_,UINT(sample_footprints_.size()),sample_footprints_[0].Footprint.RowPitch,sample_pitch_,count,htile_offset_,htile_words_,expanded_htile_,0};
    list->SetComputeRoot32BitConstants(3,10,constants,0);
    const UINT groups=(count+63)/64;list->Dispatch((std::min)(groups,65535u),(groups+65534)/65535,1);
    layout_transition(list,tiled_.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
    layout_transition(list,linear_.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
    for(UINT i=0;i<sample_footprints_.size();++i) {
        D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=linear_.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;from.PlacedFootprint=sample_footprints_[i];
        to.pResource=array;to.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;to.SubresourceIndex=i;
        list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
    }
    layout_transition(list,linear_.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
}
void TextureLayoutTransfer::retile_samples(ID3D12GraphicsCommandList* list,ID3D12Resource* array) const {
    layout_transition(list,linear_.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
    for(UINT i=0;i<sample_footprints_.size();++i) {
        D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=array;from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;from.SubresourceIndex=i;
        to.pResource=linear_.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=sample_footprints_[i];
        list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
    }
    layout_transition(list,linear_.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    layout_transition(list,tiled_.Get(),D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
    // Every marked byte is rewritten by the shader; only its exact complement needs the preimage.
    for(const auto& [offset,bytes]:padding_)list->CopyBufferRegion(tiled_.Get(),offset,upload_.Get(),offset,bytes);
    layout_transition(list,tiled_.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    list->SetPipelineState(pipeline_.Get());list->SetComputeRootSignature(root_.Get());
    list->SetComputeRootShaderResourceView(0,offsets_->GetGPUVirtualAddress());list->SetComputeRootShaderResourceView(1,linear_->GetGPUVirtualAddress());
    list->SetComputeRootUnorderedAccessView(2,tiled_->GetGPUVirtualAddress());
    const UINT count=width_*height_*UINT(sample_footprints_.size());
    const UINT constants[]{width_,height_,UINT(sample_footprints_.size()),sample_footprints_[0].Footprint.RowPitch,sample_pitch_,count,htile_offset_,htile_words_,expanded_htile_,1};
    list->SetComputeRoot32BitConstants(3,10,constants,0);
    const UINT groups=((std::max)(count,htile_words_)+63)/64;list->Dispatch((std::min)(groups,65535u),(groups+65534)/65535,1);
    layout_transition(list,linear_.Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_COMMON);
    layout_transition(list,tiled_.Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
    list->CopyBufferRegion(readback_.Get(),0,tiled_.Get(),0,tiled_bytes_);
    layout_transition(list,tiled_.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_COMMON);
}
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
        DXGI_FORMAT depth_format = DXGI_FORMAT_D32_FLOAT_S8X24_UINT, bool overlay = false, UINT quality = 0) {
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
    desc.NumRenderTargets = 1; desc.RTVFormats[0] = output_format; desc.SampleDesc = {samples,quality};
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
HRESULT MipmapTransfer::initialize(ID3D12Device* device,DXGI_FORMAT format) {
    // Reference helper VS 1406eec80 / PS 140719880: float array sample, linear clamp, explicit LOD zero.
    static constexpr char source[]=R"(
Texture2DArray<float4> image:register(t0);SamplerState linear_clamp:register(s0);
struct Vertex {float4 position:SV_Position;float2 uv:TEXCOORD0;};
Vertex vs_main(uint id:SV_VertexID){Vertex v;v.uv=float2((id<<1)&2,id&2);v.position=float4(v.uv*float2(2,-2)+float2(-1,1),0,1);return v;}
float4 ps_main(Vertex v):SV_Target{return image.SampleLevel(linear_clamp,float3(v.uv,0),0);}
)";
    return graphics_pipeline(device,format,1,source,nullptr,root_,pipeline_);
}
HRESULT MipmapTransfer::record(ID3D12GraphicsCommandList* list,ID3D12DescriptorHeap* heap,
        D3D12_GPU_DESCRIPTOR_HANDLE source,D3D12_CPU_DESCRIPTOR_HANDLE target,UINT width,UINT height) const {
    return fullscreen(list,root_.Get(),pipeline_.Get(),heap,source,target,width,height,0);
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

HRESULT SampleTransfer::initialize(ID3D12Device* device, DXGI_FORMAT format, UINT samples, UINT quality) {
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
        1,UINT_MAX,depth_only ? DXGI_FORMAT_D32_FLOAT : DXGI_FORMAT_D32_FLOAT_S8X24_UINT,false,quality);
    if (SUCCEEDED(hr)) hr = graphics_pipeline(device, depth_stencil ? DXGI_FORMAT_R32G32_UINT : depth_only ? DXGI_FORMAT_R32_UINT : transport_format, 1, source, out,
        roots[1], pipelines[1],false,depth_stencil ? 2 : 1);
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>,8> stencil;
    if (depth_stencil) for (UINT bit=0;bit<8 && SUCCEEDED(hr);++bit) {
        const D3D_SHADER_MACRO masks[]{{"INTO_MSAA","1"},{"DEPTH_STENCIL","1"},{"STENCIL_BIT","1"},{nullptr,nullptr}};
        Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
        hr=graphics_pipeline(device,format,samples,source,masks,root,stencil[bit],true,1,bit,DXGI_FORMAT_D32_FLOAT_S8X24_UINT,false,quality);
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
