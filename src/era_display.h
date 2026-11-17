// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <array>
#include <vector>
#include <span>
#include <utility>
#include <memory>

namespace xboxone::era {
struct TitleTextureSampleOffsets;
// Exact byte transport using verified offsets, not texture sampling or color math.
// The caller supplies texture states and retains its normal submission fence.
class TextureLayoutTransfer {
public:
    TextureLayoutTransfer()=default;
    ~TextureLayoutTransfer();
    TextureLayoutTransfer(const TextureLayoutTransfer&)=delete;
    TextureLayoutTransfer& operator=(const TextureLayoutTransfer&)=delete;
    HRESULT initialize(ID3D12Device* device,std::span<const UINT32> offsets,UINT width,UINT height,
                       const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& footprint,SIZE_T tiled_bytes);
    HRESULT prepare(const void* canonical,bool full_overwrite=false);
    HRESULT initialize_samples(ID3D12Device*,const TitleTextureSampleOffsets&,UINT width,UINT height,
                               std::span<const D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints,SIZE_T tiled_bytes,std::shared_ptr<void> owner);
    HRESULT validate_samples(const void* canonical,bool bypass_htile) const;
    void detile_samples(ID3D12GraphicsCommandList*,ID3D12Resource* array) const;
    void retile_samples(ID3D12GraphicsCommandList*,ID3D12Resource* array) const;
    void detile(ID3D12GraphicsCommandList* list,ID3D12Resource* texture) const;
    void retile(ID3D12GraphicsCommandList* list,ID3D12Resource* texture) const;
    ID3D12Resource* readback() const {return readback_.Get();}
private:
    HRESULT initialize_program(ID3D12Device*,const char*,SIZE_T,UINT constants,UINT sample_bytes=0,bool depth_stencil=false);
    void dispatch(ID3D12GraphicsCommandList* list,bool into_tiled) const;
    SIZE_T retained_bytes_=0,tiled_bytes_=0;
    bool sample_transport_=false;
    UINT width_=0,height_=0;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint_{};
    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> sample_footprints_;
    std::shared_ptr<void> sample_owner_; // Retains the immutable original-XG address table used by validation.
    const UINT32* sample_depth_offsets_=nullptr;
    UINT sample_pitch_=0,htile_offset_=0,htile_words_=0,expanded_htile_=0;
    bool sample_depth_stencil_=true;
    std::vector<std::pair<SIZE_T,SIZE_T>> padding_;
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_;
    Microsoft::WRL::ComPtr<ID3D12Resource> offsets_,upload_,linear_,tiled_,readback_;
};
void set_graphics_diagnostics(bool enabled, bool warp = false);
// Xbox display transfer inside the native GPU backend. The caller owns resource
// states, descriptor lifetime and submission fences, as for other GPU commands.
class DisplayTransfer {
public:
    HRESULT initialize(ID3D12Device* device, DXGI_FORMAT output_format, bool premultiplied_overlay = false);
    HRESULT record(ID3D12GraphicsCommandList* list, ID3D12DescriptorHeap* source_heap,
                   D3D12_GPU_DESCRIPTOR_HANDLE source, D3D12_CPU_DESCRIPTOR_HANDLE target,
                   UINT width, UINT height, bool srgb_to_709) const;
private:
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_;
};
// Reference 1405869a0: filter one source mip/array slice into its next mip.
class MipmapTransfer {
public:
    HRESULT initialize(ID3D12Device*,DXGI_FORMAT);
    HRESULT record(ID3D12GraphicsCommandList*,ID3D12DescriptorHeap*,D3D12_GPU_DESCRIPTOR_HANDLE,
                   D3D12_CPU_DESCRIPTOR_HANDLE,UINT width,UINT height) const;
private:
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_;
};
// Local system UI is composed only into the host output, after the title's
// display transfer. Guest textures and guest-visible graphics state stay intact.
class GuideComposition {
public:
    HRESULT record(ID3D12Device*, ID3D12GraphicsCommandList*, ID3D12Resource* output);
    bool active() const { return active_; }
private:
    bool active_ = false;
    DisplayTransfer transfer_;
    DXGI_FORMAT format_ = DXGI_FORMAT_UNKNOWN;
    UINT width_ = 0, height_ = 0;
    Microsoft::WRL::ComPtr<ID3D12Resource> image_, upload_;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> source_, target_;
    std::vector<std::uint32_t> pixels_;
};
// Transfers every MSAA sample independently through a single-sample array.
class SampleTransfer {
public:
    HRESULT initialize(ID3D12Device* device, DXGI_FORMAT format, UINT samples, UINT quality);
    HRESULT record(ID3D12GraphicsCommandList* list, ID3D12DescriptorHeap* heap,
                   D3D12_GPU_DESCRIPTOR_HANDLE source, D3D12_CPU_DESCRIPTOR_HANDLE target,
                   UINT width, UINT height, UINT sample, bool into_msaa) const;
private:
    UINT samples_ = 0;
    bool depth_stencil_ = false;
    bool depth_target_ = false;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 8> stencil_pipelines_;
    std::array<Microsoft::WRL::ComPtr<ID3D12RootSignature>, 2> roots_;
    std::array<Microsoft::WRL::ComPtr<ID3D12PipelineState>, 2> pipelines_;
};
}
