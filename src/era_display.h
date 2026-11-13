// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <d3d12.h>
#include <wrl/client.h>
#include <array>

namespace xboxone::era {
void set_graphics_diagnostics(bool enabled);
// Xbox display transfer inside the native GPU backend. The caller owns resource
// states, descriptor lifetime and submission fences, as for other GPU commands.
class DisplayTransfer {
public:
    HRESULT initialize(ID3D12Device* device, DXGI_FORMAT output_format);
    HRESULT record(ID3D12GraphicsCommandList* list, ID3D12DescriptorHeap* source_heap,
                   D3D12_GPU_DESCRIPTOR_HANDLE source, D3D12_CPU_DESCRIPTOR_HANDLE target,
                   UINT width, UINT height, bool srgb_to_709) const;
private:
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root_;
    Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_;
};
// Transfers every MSAA sample independently through a single-sample array.
class SampleTransfer {
public:
    HRESULT initialize(ID3D12Device* device, DXGI_FORMAT format, UINT samples);
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
