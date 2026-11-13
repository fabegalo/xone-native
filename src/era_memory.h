// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <d3d12.h>
#include <cstddef>
#include <memory>
#include <array>

namespace xboxone::era {
struct TitleTextureLayout {
    std::size_t bytes = 0, alignment = 0;
    D3D12_RESOURCE_DESC resource_desc{};
    std::array<UINT32, 8> guest_descriptor{}; // Produced by the pinned XG revision, before memory binding.
    std::shared_ptr<void> context;
    // Original helper copies one subresource between tiled canonical bytes and
    // a linear footprint. Both pitches describe the linear side, in bytes.
    HRESULT (*copy)(void* context, bool into_tiled, void* linear, void* tiled,
                    UINT subresource, UINT row_pitch, UINT slice_pitch) = nullptr;
};
HRESULT create_xg_texture_layout(UINT width, UINT height, UINT mips, UINT arrays, DXGI_FORMAT format,
                                 UINT tile_mode, TitleTextureLayout& out, UINT bind_flags = 0, UINT samples = 1);
HRESULT create_xg_texture3d_layout(UINT width, UINT height, UINT depth, UINT mips, DXGI_FORMAT format,
                                  UINT bind_flags, TitleTextureLayout& out, UINT tile_mode = ~UINT{0});
// A pin keeps the single CPU allocation alive while its buffer is used by GPU.
// Direct pins exclude other bindings; CPU access resumes only after release.
// Aligned bindings separate storage ownership from the begin/end GPU interval.
// A live pin must not be copied or used after a successful release.
// Return the buffer to initial_state before signaling the completion fence.
struct TitleGpuResource {
    ID3D12Resource* resource = nullptr;
    void* allocation = nullptr;
    UINT64 offset = 0;
    D3D12_RESOURCE_STATES initial_state = D3D12_RESOURCE_STATE_COMMON;
    void* representation = nullptr; // Registry-owned aligned window, if requested.
    std::size_t canonical_offset = 0, size = 0;
};
// Resource capabilities are fixed by the first binding; later requests never
// replace a cached resource with an unsynchronized alias.
HRESULT acquire_title_gpu_buffer(void* address, std::size_t size, ID3D12Device* device, TitleGpuResource& out,
                                 D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE, bool aligned = false);
HRESULT acquire_title_gpu_texture(void* address, const TitleTextureLayout& layout, ID3D12Device* device, TitleGpuResource& out);
// Aligned bindings retain storage, but GPU use is explicit. Begin uploads the
// canonical range; end writes it back and restores COMMON. Complete must see a
// real fence after that command list before CPU access or overlapping GPU use.
// Overlapping representations cannot be active together. Never copy a live handle.
// Return the representation to the state passed to begin before calling end.
HRESULT begin_title_gpu_buffer(TitleGpuResource& buffer, ID3D12GraphicsCommandList* list, D3D12_RESOURCE_STATES state);
HRESULT end_title_gpu_buffer(TitleGpuResource& buffer, ID3D12GraphicsCommandList* list);
HRESULT begin_title_gpu_texture(TitleGpuResource& texture, ID3D12GraphicsCommandList* list, D3D12_RESOURCE_STATES state);
HRESULT end_title_gpu_texture(TitleGpuResource& texture, ID3D12GraphicsCommandList* list);
// A texture's GPU fence is internal: complete also retiles readback bytes into
// canonical memory. Publish guest completion only after this returns S_OK.
HRESULT complete_title_gpu_resource(TitleGpuResource& buffer, ID3D12Fence* fence, UINT64 value);
// CPU maps share the canonical bytes and exclude GPU access to the parent.
// E_PENDING means a GPU interval still needs its completion fence.
HRESULT map_title_gpu_resource(TitleGpuResource& buffer, void** address);
HRESULT unmap_title_gpu_resource(TitleGpuResource& buffer);
// E_PENDING preserves the pin; retry after the last GPU use has completed.
HRESULT release_title_gpu_resource(TitleGpuResource& buffer, ID3D12Fence* fence, UINT64 value);
}
