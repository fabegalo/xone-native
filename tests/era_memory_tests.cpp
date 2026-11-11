// SPDX-License-Identifier: BSD-3-Clause
#include "xboxone_native/era_loader.h"
#include "../src/era_memory.h"
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cstring>
#include <iostream>

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
}
int main(int argc, char** argv) {
    const bool warp = argc == 2 && std::strcmp(argv[1], "warp") == 0;
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
    TitleGpuBuffer pin;
    HRESULT hr = acquire_title_gpu_buffer(memory, 64, device.Get(), pin);
    std::cout << (warp ? "WARP" : "hardware") << " shared heap hr=0x" << std::hex << hr << std::dec << '\n';
    check(SUCCEEDED(hr), "CPU allocation binds to a D3D12 heap without a shadow copy");
    if (FAILED(hr)) { release(memory, attributes); return 1; }
    TitleGpuBuffer second;
    check(release_title_gpu_buffer(pin, nullptr, 0) == E_INVALIDARG && pin.resource,
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
    check(release_title_gpu_buffer(pin, fence.Get(), 1) == E_PENDING && pin.resource,
          "unsignaled fence preserves the memory pin");
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) return 1;
    hr = queue->Signal(fence.Get(), 1);
    if (SUCCEEDED(hr)) hr = fence->SetEventOnCompletion(1, event);
    const bool complete = SUCCEEDED(hr) && WaitForSingleObject(event, 3000) == WAIT_OBJECT_0;
    CloseHandle(event);
    check(complete, "GPU copy completes within the deadline");
    if (!complete) return 1;
    check(release_title_gpu_buffer(pin, fence.Get(), 1) == S_OK && !pin.resource, "completed GPU pin releases");
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
    check(release_title_gpu_buffer(pin, fence.Get(), 2) == E_PENDING, "deferred free waits for the fence");
    check(SUCCEEDED(fence->Signal(2)), "fixture completion signaled");
    check(release_title_gpu_buffer(pin, fence.Get(), 2) == S_OK, "deferred free completes after fence");
    VirtualQuery(memory, &info, sizeof info);
    check(info.State == MEM_FREE, "final pin releases CPU and GPU backing");

    const auto virtual_allocate = reinterpret_cast<decltype(&VirtualAlloc)>(resolve_kernelx({"kernelx.dll", "VirtualAlloc", 0}));
    const auto virtual_free = reinterpret_cast<decltype(&VirtualFree)>(resolve_kernelx({"kernelx.dll", "VirtualFree", 0}));
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
    check(release_title_gpu_buffer(pin, fence.Get(), 3) == S_OK, "subcommit pin retires");
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
        check(release_title_gpu_buffer(pin, fence.Get(), 4) == S_OK, "UAV memory pin retires through real fence");
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
        auto* counter = static_cast<UINT32*>(allocate(4, attributes));
        if (!counter) return 1;
        TitleGpuBuffer counter_pin;
        hr = acquire_title_gpu_buffer(storage, 64, device.Get(), pin, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        if (SUCCEEDED(hr)) hr = acquire_title_gpu_buffer(counter, 4, device.Get(), counter_pin, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        if (FAILED(hr)) return 1;
        const HMODULE compiler = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        const auto compile = compiler ? reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
        if (!compile) return 1;
        constexpr char source[] = "AppendStructuredBuffer<uint> output:register(u0); [numthreads(1,1,1)] void main(){output.Append(0xCAFEBABEu);}";
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
        view.Format = DXGI_FORMAT_UNKNOWN; view.Buffer.StructureByteStride = 4;
        device->CreateUnorderedAccessView(pin.resource, counter_pin.resource, &view, cpu);
        device->CopyDescriptorsSimple(1, gpu_heap->GetCPUDescriptorHandleForHeapStart(), cpu, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        if (FAILED(allocator->Reset()) || FAILED(list->Reset(allocator.Get(), pipeline.Get()))) return 1;
        list->SetDescriptorHeaps(1, heaps); list->SetComputeRootSignature(root.Get());
        list->SetComputeRootDescriptorTable(0, gpu_heap->GetGPUDescriptorHandleForHeapStart());
        barrier(list.Get(), pin.resource, pin.initial_state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        barrier(list.Get(), counter_pin.resource, counter_pin.initial_state, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        list->Dispatch(1, 1, 1);
        barrier(list.Get(), pin.resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        barrier(list.Get(), counter_pin.resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyBufferRegion(readback.Get(), 0, pin.resource, pin.offset, 64);
        list->CopyBufferRegion(readback.Get(), 64, counter_pin.resource, counter_pin.offset, 4);
        barrier(list.Get(), pin.resource, D3D12_RESOURCE_STATE_COPY_SOURCE, pin.initial_state);
        barrier(list.Get(), counter_pin.resource, D3D12_RESOURCE_STATE_COPY_SOURCE, counter_pin.initial_state);
        if (FAILED(list->Close())) return 1;
        queue->ExecuteCommandLists(1, lists);
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event) return 1;
        hr = queue->Signal(fence.Get(), 5);
        if (SUCCEEDED(hr)) hr = fence->SetEventOnCompletion(5, event);
        const bool appended = SUCCEEDED(hr) && WaitForSingleObject(event, 3000) == WAIT_OBJECT_0;
        CloseHandle(event);
        check(appended, "GPU append and counter complete within deadline");
        if (!appended) return 1;
        check(release_title_gpu_buffer(pin, fence.Get(), 5) == S_OK && release_title_gpu_buffer(counter_pin, fence.Get(), 5) == S_OK,
              "data and append counter retire through the same completion boundary");
        check(storage[0] == 0xcafebabe && storage[1] == values[0] && *counter == 1,
              "native GPU append writes one element and increments actual shared counter");
        hr = readback->Map(0, nullptr, &mapped);
        check(SUCCEEDED(hr) && static_cast<UINT32*>(mapped)[0] == 0xcafebabe && static_cast<UINT32*>(mapped)[16] == 1,
              "append data and counter readback match CPU-visible bytes");
        if (SUCCEEDED(hr)) readback->Unmap(0, nullptr);
        release(counter, attributes);
        release(storage, attributes);
    }
    return failures ? 1 : 0;
}
