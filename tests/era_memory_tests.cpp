// SPDX-License-Identifier: BSD-3-Clause
#include "xboxone_native/era_loader.h"
#include "../src/era_memory.h"
#include "../src/era_display.h"
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <cstring>
#include <iostream>
#include <algorithm>
#include <vector>
#include <filesystem>
#include <cmath>

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
struct LinearTextureFixture { bool fail_writeback = false; };
HRESULT linear_texture_copy(void* opaque, bool into_tiled, void* linear, void* canonical, UINT subresource, UINT pitch, UINT slice) {
    if (!opaque || !linear || !canonical || subresource >= 3) return E_INVALIDARG;
    auto* fixture = static_cast<LinearTextureFixture*>(opaque);
    if (into_tiled && fixture->fail_writeback && subresource == 1) return E_FAIL;
    UINT offset = 0;
    for (UINT mip = 0; mip < subresource; ++mip) offset += (std::max)(1u, 8u >> mip) * (std::max)(1u, 4u >> mip) * 4;
    const UINT row_bytes = (std::max)(1u, 8u >> subresource) * 4, rows = (std::max)(1u, 4u >> subresource);
    if (pitch < row_bytes || UINT64(pitch) * rows > slice) return E_INVALIDARG;
    for (UINT row = 0; row < rows; ++row) {
        auto* a = static_cast<unsigned char*>(canonical) + offset + row * row_bytes;
        auto* b = static_cast<unsigned char*>(linear) + row * pitch;
        std::memcpy(into_tiled ? a : b, into_tiled ? b : a, row_bytes);
    }
    return S_OK;
}
HRESULT linear_sample_copy(void*, bool into, void* linear, void* canonical, UINT subresource, UINT pitch, UINT slice) {
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
    constexpr ULONGLONG attributes = 0x64034000;
    auto* memory = static_cast<unsigned char*>(allocate(0x129003, attributes));
    check(memory != nullptr, "observed XMem attributes allocate real CPU backing");
    if (!memory) return 1;
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
    if (FAILED(hr)) return 1;
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
    check(FAILED(acquire_title_gpu_buffer(memory, 1, device.Get(), second)), "retiring allocation cannot be rebound");
    check(release_title_gpu_resource(pin, fence.Get(), 2) == E_PENDING, "deferred free waits for the fence");
    check(SUCCEEDED(fence->Signal(2)), "fixture completion signaled");
    check(release_title_gpu_resource(pin, fence.Get(), 2) == S_OK, "deferred free completes after fence");
    VirtualQuery(memory, &info, sizeof info);
    check(info.State == MEM_FREE, "final pin releases CPU and GPU backing");

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
    const UINT invalid_page = 512;
    check(map_title_esram(0, esram_memory, 1, first_pages) == E_INVALIDARG &&
        map_title_esram(1, esram_memory, 1, &invalid_page) == E_INVALIDARG && esram_memory[0] == 0x44,
        "invalid ESRAM flags and pages preserve existing mappings");
    check(map_title_esram(1, esram_memory, 1, first_pages + 1) == S_OK && esram_memory[0] == 0x55,
          "ESRAM remapping changes identity without copying disconnected data");
    esram_memory[65536] = 0x66;
    check(esram_memory[0] == 0x66 && alias_memory[0] == 0x44, "repeated physical pages alias while distinct pages stay independent");
    check(map_title_esram(1, esram_memory, 2, nullptr) == S_OK, "ESRAM unmapping restores owned placeholders");
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
    VirtualQuery(chunk, &info, sizeof info);
    check(info.State == MEM_RESERVE, "decommit preserves parent reservation");
    check(virtual_allocate(chunk, 65536, 0x30001000, 0x10004) == chunk && chunk[0] == 0,
          "recommit resets released page contents");
    check(neighbor[0] == 0x5B, "recommit leaves the other backing intact");
    check(virtual_free(reservation, 0, MEM_RELEASE), "graphics reservation and committed backing released");
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
    const auto texture_fixture = [&](TitleTextureLayout layout, UINT block, UINT bytes_per_block, UINT64 signal,
                                     LinearTextureFixture* injected) -> bool {
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
                columns * bytes_per_block, columns * rows * bytes_per_block))) return false;
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
        hr = begin_title_gpu_texture(texture, list.Get(), depth_target ? D3D12_RESOURCE_STATE_DEPTH_WRITE :
            render_target ? D3D12_RESOURCE_STATE_RENDER_TARGET : D3D12_RESOURCE_STATE_COPY_DEST);
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
        ComPtr<ID3D12DescriptorHeap> render_heap;
        if (depth_target) {
            const D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_DSV,1,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};
            if (FAILED(device->CreateDescriptorHeap(&heap,IID_PPV_ARGS(&render_heap)))) return false;
            D3D12_DEPTH_STENCIL_VIEW_DESC view{};
            view.Format = DXGI_FORMAT_D32_FLOAT; view.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
            view.Texture2D.MipSlice = mutation_mip;
            device->CreateDepthStencilView(texture.resource,&view,render_heap->GetCPUDescriptorHandleForHeapStart());
            list->ClearDepthStencilView(render_heap->GetCPUDescriptorHandleForHeapStart(),D3D12_CLEAR_FLAG_DEPTH,.25f,0,0,nullptr);
        } else if (render_target) {
            const D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
            if (FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&render_heap)))) return false;
            D3D12_RENDER_TARGET_VIEW_DESC view{};
            view.Format = d.Format; view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D; view.Texture2D.MipSlice = mutation_mip;
            device->CreateRenderTargetView(texture.resource, &view, render_heap->GetCPUDescriptorHandleForHeapStart());
            constexpr UINT packed = 0xa7a7a7a7;
            float color[]{float(packed & 1023) / 1023, float((packed >> 10) & 1023) / 1023,
                float((packed >> 20) & 1023) / 1023, float(packed >> 30) / 3};
            if (d.Format == DXGI_FORMAT_B8G8R8A8_UNORM) std::fill(std::begin(color),std::end(color),167.f/255.f);
            list->ClearRenderTargetView(render_heap->GetCPUDescriptorHandleForHeapStart(), color, 0, nullptr);
        } else list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        std::fill(expected[mutation_mip].begin(), expected[mutation_mip].end(), 0xa7);
        if (depth_target) {
            const float cleared = .25f;
            for (SIZE_T offset=0;offset<expected[mutation_mip].size();offset+=4)
                std::memcpy(expected[mutation_mip].data()+offset,&cleared,4);
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
                    columns * bytes_per_block, columns * rows * bytes_per_block);
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
            last_columns * bytes_per_block, last_columns * last_rows * bytes_per_block);
        if (FAILED(unmap_title_gpu_resource(texture)) || FAILED(hr)) return false;
        if (FAILED(allocator->Reset()) || FAILED(list->Reset(allocator.Get(), nullptr))) return false;
        if (FAILED(begin_title_gpu_texture(texture, list.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE)) ||
            FAILED(end_title_gpu_texture(texture, list.Get())) || FAILED(list->Close())) return false;
        queue->ExecuteCommandLists(1, lists);
        if (!wait_for(signal + 1) || FAILED(complete_title_gpu_resource(texture, fence.Get(), signal + 1))) return false;
        check(matches(), "texture reuse refreshes canonical CPU changes and preserves previous GPU data");
        layout.context.reset(); // The registry now owns the last original-computer reference in the XG case.
        check(release_title_gpu_resource(texture, fence.Get(), signal + 1) == S_OK, "texture ownership retires after conversion and its fence");
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
    }
    const auto sample_fixture = [&](TitleTextureLayout layout, UINT64 signal, void* reserved = nullptr) -> bool {
        auto* storage = reserved ? nullptr : static_cast<unsigned char*>(allocate(layout.bytes + layout.alignment + 32, attributes));
        if (!reserved && !storage) return false;
        auto* canonical = reserved ? static_cast<unsigned char*>(reserved) :
            reinterpret_cast<unsigned char*>((reinterpret_cast<std::uintptr_t>(storage) + 16 + layout.alignment - 1) & ~(layout.alignment - 1));
        std::memset(canonical - 16, 0x5a, layout.bytes + 32);
        std::vector<UINT32> expected(32 * 16 * 4), actual(expected.size());
        for (UINT i = 0; i < expected.size(); ++i) {
            const UINT sample = i % 4;
            expected[i] = ((15 + sample) << 6) | (((16 + sample) << 6) << 11) | (((14 + sample) << 5) << 22);
        }
        if (FAILED(layout.copy(layout.context.get(), true, expected.data(), canonical, 0, 512, 8192))) return false;
        TitleGpuResource texture;
        HRESULT hr = acquire_title_gpu_texture(canonical, layout, device.Get(), texture);
        check(SUCCEEDED(hr), "four-sample resource acquires a real native MSAA texture");
        if (FAILED(hr)) return false;
        check(texture.resource->GetDesc().SampleDesc.Count == 4, "MSAA storage retains all four hardware samples");
        ComPtr<ID3D12DescriptorHeap> targets;
        const D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        if (FAILED(device->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&targets)))) return false;
        D3D12_RENDER_TARGET_VIEW_DESC view{};
        view.Format = DXGI_FORMAT_R11G11B10_FLOAT; view.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
        device->CreateRenderTargetView(texture.resource, &view, targets->GetCPUDescriptorHandleForHeapStart());
        for (UINT iteration = 0; iteration < 3; ++iteration) {
            if (iteration == 2) {
                void* cpu = nullptr;
                if (FAILED(map_title_gpu_resource(texture, &cpu))) return false;
                for (UINT i = 0; i < expected.size(); ++i) expected[i] ^= (i % 4 + 1) << 1; // Distinct finite mantissas in each sample.
                hr = layout.copy(layout.context.get(), true, expected.data(), cpu, 0, 512, 8192);
                if (FAILED(unmap_title_gpu_resource(texture)) || FAILED(hr)) return false;
            }
            if (FAILED(allocator->Reset()) || FAILED(list->Reset(allocator.Get(), nullptr))) return false;
            hr = begin_title_gpu_texture(texture, list.Get(), D3D12_RESOURCE_STATE_RENDER_TARGET);
            check(SUCCEEDED(hr), "sample upload uses per-sample GPU shading without resolve");
            if (FAILED(hr)) return false;
            if (iteration == 1) {
                const float color[]{.5f, 1, 2, 1};
                list->ClearRenderTargetView(targets->GetCPUDescriptorHandleForHeapStart(), color, 0, nullptr);
                std::fill(expected.begin(), expected.end(), (14u << 6) | ((15u << 6) << 11) | ((16u << 5) << 22));
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
            hr = layout.copy(layout.context.get(), false, actual.data(), canonical, 0, 512, 8192);
            check(SUCCEEDED(hr) && actual == expected, iteration == 1 ? "independent native MSAA clear updates every canonical sample" :
                "all four distinct MSAA samples survive GPU upload/readback and CPU reuse");
            for (UINT i = 0; i < 16; ++i)
                check(canonical[int(i) - 16] == 0x5a && canonical[layout.bytes + i] == 0x5a, "MSAA coherence preserves neighboring bytes");
        }
        if (FAILED(release_title_gpu_resource(texture, fence.Get(), signal + 2))) return false;
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
        check(SUCCEEDED(hr) && sample_fixture(std::move(sample_layout), 30), "original XG MSAA layout completes per-sample GPU coherence");
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
    check(virtual_free(pool_memory, 0, MEM_RELEASE) && virtual_free(pool_alias, 0, MEM_RELEASE), "GPU ESRAM fixtures release every owned reservation after retirement");
    return failures ? 1 : 0;
}
