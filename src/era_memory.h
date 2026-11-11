// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <d3d12.h>
#include <cstddef>

namespace xboxone::era {
// A pin keeps the single CPU allocation alive while its buffer is used by GPU.
// Guest synchronization must finish CPU access before acquire and resume it
// only after release. Concurrent GPU pins are refused.
// A live pin must not be copied or used after a successful release.
// Return the buffer to initial_state before signaling the completion fence.
struct TitleGpuBuffer {
    ID3D12Resource* resource = nullptr;
    void* allocation = nullptr;
    UINT64 offset = 0;
    D3D12_RESOURCE_STATES initial_state = D3D12_RESOURCE_STATE_COMMON;
};
// Resource capabilities are fixed by the first binding; later requests never
// replace a cached resource with an unsynchronized alias.
HRESULT acquire_title_gpu_buffer(void* address, std::size_t size, ID3D12Device* device, TitleGpuBuffer& out,
                                 D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE);
// E_PENDING preserves the pin; retry after the last GPU use has completed.
HRESULT release_title_gpu_buffer(TitleGpuBuffer& buffer, ID3D12Fence* fence, UINT64 value);
}
