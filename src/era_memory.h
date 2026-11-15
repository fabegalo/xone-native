// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <d3d12.h>
#include <cstddef>
#include <memory>
#include <array>

namespace xboxone::era {
// ACP heaps retain registered CPU backing until their session ends.
HRESULT pin_title_audio_memory(void* address, std::size_t bytes);
void unpin_title_audio_memory(void* address);
struct TitleGpuClearState {
    UINT32 depth = 0; // Full IEEE bits, not quantized HTILE bounds.
    UINT8 stencil = 0, known = 0; // D3D11_CLEAR_DEPTH / D3D11_CLEAR_STENCIL.
    // Ingress only: read without the depth block (shader/copy). The GCN 2 texture unit
    // does not consult HTILE (TC-compatible HTILE starts with GFX8), so it reads the base plane.
    bool bypass_htile = false;
};
struct TitleTextureLayout {
    std::size_t bytes = 0, alignment = 0;
    bool demand_coherence = false; // Provider is retained and supports publication through a private canonical image.
    UINT tile_mode = UINT_MAX;
    D3D12_RESOURCE_DESC resource_desc{};
    std::array<UINT32, 8> guest_descriptor{}; // Produced by the pinned XG revision, before memory binding.
    std::array<UINT32, 8> stencil_descriptor{}; // Optional typed plane SRD, with relative physical base.
    std::shared_ptr<void> context;
    // Original helper copies one subresource between tiled canonical bytes and
    // a linear footprint. Both pitches describe the linear side, in bytes.
    HRESULT (*copy)(void* context, bool into_tiled, void* linear, void* tiled,
                    UINT subresource, UINT row_pitch, UINT slice_pitch,const TitleGpuClearState* clear) = nullptr;
    // Immutable, original-XG-verified 32-bit texel byte offsets, when available.
    // S_FALSE keeps the CPU reference path for other layouts/revisions.
    HRESULT (*offsets)(void* context,const UINT32** values,std::size_t* count) = nullptr;
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
    bool composite = false; // A graphics reservation spanning physical/shared commitments.
    TitleGpuClearState clear; // Logical resource state; physical aliases retain independent clear registers.
};
HRESULT set_title_gpu_clear(TitleGpuResource& resource,UINT aspects,UINT32 depth,UINT8 stencil);
HRESULT map_title_esram(UINT flags, void* address, UINT pages, const UINT* page_list);
// Resource capabilities are fixed by the first binding; later requests never
// replace a cached resource with an unsynchronized alias.
HRESULT acquire_title_gpu_buffer(void* address, std::size_t size, ID3D12Device* device, TitleGpuResource& out,
                                 D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE, bool aligned = false);
HRESULT acquire_title_gpu_texture(void* address, const TitleTextureLayout& layout, ID3D12Device* device, TitleGpuResource& out);
// Aligned bindings retain storage; GPU use remains explicit. Tracked graphics
// mappings keep native contents resident and publish canonical bytes on CPU
// access or incompatible alias use. Completion still requires the real fence.
// Overlapping representations cannot be active together. Never copy a live handle.
// Return the representation to the state passed to begin before calling end.
// read_only promises no writes or write-state transitions within this interval;
// it skips redundant writeback, but retains exclusion and fence retirement.
HRESULT begin_title_gpu_buffer(TitleGpuResource& buffer, ID3D12GraphicsCommandList* list, D3D12_RESOURCE_STATES state,bool read_only=false);
HRESULT end_title_gpu_buffer(TitleGpuResource& buffer, ID3D12GraphicsCommandList* list);
// discard_contents requires a full overwrite of every aspect of a single-subresource texture.
HRESULT begin_title_gpu_texture(TitleGpuResource& texture, ID3D12GraphicsCommandList* list, D3D12_RESOURCE_STATES state,bool discard_contents=false,
                                const D3D12_SUBRESOURCE_DATA* first_subresource=nullptr,bool read_only=false);
HRESULT end_title_gpu_texture(TitleGpuResource& texture, ID3D12GraphicsCommandList* list);
// Publish guest completion only after S_OK. For tracked mappings, protected CPU
// reads/writes retire the canonical image before the faulting access resumes.
HRESULT complete_title_gpu_resource(TitleGpuResource& buffer, ID3D12Fence* fence, UINT64 value);
// CPU maps share the canonical bytes and exclude GPU access to the parent.
// E_PENDING means a GPU interval still needs its completion fence. Blocking maps
// wait for another submission thread's retirement, with the normal 5 s deadline.
HRESULT map_title_gpu_resource(TitleGpuResource& buffer, void** address, bool wait=false);
HRESULT unmap_title_gpu_resource(TitleGpuResource& buffer);
// E_PENDING preserves the pin; retry after the last GPU use has completed.
HRESULT release_title_gpu_resource(TitleGpuResource& buffer, ID3D12Fence* fence, UINT64 value);
}
