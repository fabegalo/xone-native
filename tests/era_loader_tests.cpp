// SPDX-License-Identifier: BSD-3-Clause
#include <winsock2.h>
#include "xboxone_native/era_loader.h"
#include "../src/era_winrt_classes.h"
#include "../src/era_winrt_object.h"
#include "../src/era_memory.h"
#include "../src/era_display.h"
#include "../src/era_graphics_stream.h"
#include "xboxone_native/appmodel.h"
#include "xboxone_native/inventory.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <evntprov.h>
#include <d3dcommon.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include <cmath>
#include <limits>
#include <d3d12.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <dxgi1_4.h>
#include <activation.h>
#include <asyncinfo.h>
#include <roapi.h>
#include <winstring.h>
#include <weakreference.h>

#include <fstream>
#include <filesystem>
#include <string>
#include <iostream>
#include <iterator>
#include <utility>
#include <thread>
#include <atomic>
#include <cstring>
#include <array>
#include <algorithm>
#include <memory>

namespace {
int failures = 0;
// Independent D3D12 control for reference native clears, without HLE descriptors or conversion.
struct NativeUavClear {
    Microsoft::WRL::ComPtr<ID3D12Device> device;
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
    Microsoft::WRL::ComPtr<ID3D12Resource> buffer,readback;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> cpu,gpu;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    HANDLE event=nullptr;UINT64 value=0;
    ~NativeUavClear(){if(event)CloseHandle(event);}
    HRESULT initialize(bool warp) {
        Microsoft::WRL::ComPtr<IDXGIFactory4> factory;Microsoft::WRL::ComPtr<IDXGIAdapter> adapter;
        HRESULT hr=CreateDXGIFactory1(IID_PPV_ARGS(&factory));if(FAILED(hr))return hr;
        if(warp && FAILED(hr=factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))))return hr;
        if(FAILED(hr=D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device))))return hr;
        const D3D12_COMMAND_QUEUE_DESC q{};if(FAILED(hr=device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue))))return hr;
        if(FAILED(hr=device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator))))return hr;
        if(FAILED(hr=device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list))))return hr;
        if(FAILED(hr=list->Close()) || FAILED(hr=device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence))))return hr;
        event=CreateEventW(nullptr,FALSE,FALSE,nullptr);if(!event)return HRESULT_FROM_WIN32(GetLastError());
        D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=256;
        desc.Height=desc.DepthOrArraySize=desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        D3D12_HEAP_PROPERTIES heap{};heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        if(FAILED(hr=device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,nullptr,IID_PPV_ARGS(&buffer))))return hr;
        heap.Type=D3D12_HEAP_TYPE_READBACK;desc.Flags=D3D12_RESOURCE_FLAG_NONE;desc.Width=65536;
        if(FAILED(hr=device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&readback))))return hr;
        D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,1,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};
        if(FAILED(hr=device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&cpu))))return hr;hd.Flags=D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        return device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&gpu));
    }
    HRESULT clear(DXGI_FORMAT format,UINT bytes,const std::array<UINT,4>& values,bool floating,std::vector<BYTE>& out,bool raw=false,UINT count=4) {
        D3D12_UNORDERED_ACCESS_VIEW_DESC view{};view.Format=format;view.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;view.Buffer.FirstElement=raw ? 4:2;view.Buffer.NumElements=count;
        if(raw)view.Buffer.Flags=D3D12_BUFFER_UAV_FLAG_RAW;
        device->CreateUnorderedAccessView(buffer.Get(),nullptr,&view,cpu->GetCPUDescriptorHandleForHeapStart());
        device->CopyDescriptorsSimple(1,gpu->GetCPUDescriptorHandleForHeapStart(),cpu->GetCPUDescriptorHandleForHeapStart(),D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        HRESULT hr=allocator->Reset();if(SUCCEEDED(hr))hr=list->Reset(allocator.Get(),nullptr);if(FAILED(hr))return hr;
        ID3D12DescriptorHeap* heaps[]{gpu.Get()};list->SetDescriptorHeaps(1,heaps);
        FLOAT color[4];std::memcpy(color,values.data(),sizeof color);
        if(floating)list->ClearUnorderedAccessViewFloat(gpu->GetGPUDescriptorHandleForHeapStart(),cpu->GetCPUDescriptorHandleForHeapStart(),buffer.Get(),color,0,nullptr);
        else list->ClearUnorderedAccessViewUint(gpu->GetGPUDescriptorHandleForHeapStart(),cpu->GetCPUDescriptorHandleForHeapStart(),buffer.Get(),values.data(),0,nullptr);
        D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition={buffer.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE};list->ResourceBarrier(1,&barrier);
        list->CopyBufferRegion(readback.Get(),0,buffer.Get(),view.Buffer.FirstElement*bytes,count*bytes);
        std::swap(barrier.Transition.StateBefore,barrier.Transition.StateAfter);list->ResourceBarrier(1,&barrier);
        if(FAILED(hr=list->Close()))return hr;ID3D12CommandList* lists[]{list.Get()};queue->ExecuteCommandLists(1,lists);
        if(FAILED(hr=queue->Signal(fence.Get(),++value)) || FAILED(hr=fence->SetEventOnCompletion(value,event)))return hr;
        if(WaitForSingleObject(event,5000)!=WAIT_OBJECT_0)return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        const D3D12_RANGE read{0,count*bytes};void* data=nullptr;if(FAILED(hr=readback->Map(0,&read,&data)))return hr;
        out.assign(static_cast<BYTE*>(data),static_cast<BYTE*>(data)+count*bytes);const D3D12_RANGE no_write{0,0};readback->Unmap(0,&no_write);return S_OK;
    }
    HRESULT texture(const D3D12_RESOURCE_DESC& desc,DXGI_FORMAT format,UINT bytes,const std::array<UINT,4>& values,bool floating,std::vector<BYTE>& out,bool render_target=false) {
        const bool volume=desc.Dimension==D3D12_RESOURCE_DIMENSION_TEXTURE3D;
        if(desc.MipLevels!=1 || desc.SampleDesc.Count!=1 || (!volume && desc.DepthOrArraySize!=1))return E_INVALIDARG;
        const D3D12_HEAP_PROPERTIES heap{D3D12_HEAP_TYPE_DEFAULT};Microsoft::WRL::ComPtr<ID3D12Resource> image;
        const auto state=render_target ? D3D12_RESOURCE_STATE_RENDER_TARGET:D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
        HRESULT hr=device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,state,nullptr,IID_PPV_ARGS(&image));if(FAILED(hr))return hr;
        Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> rtv;
        if(render_target) {
            if(volume)return E_INVALIDARG;
            const D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_RTV,1,D3D12_DESCRIPTOR_HEAP_FLAG_NONE,0};
            if(FAILED(hr=device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&rtv))))return hr;
            D3D12_RENDER_TARGET_VIEW_DESC target{};target.Format=format;target.ViewDimension=D3D12_RTV_DIMENSION_TEXTURE2D;
            device->CreateRenderTargetView(image.Get(),&target,rtv->GetCPUDescriptorHandleForHeapStart());
        } else {
        D3D12_UNORDERED_ACCESS_VIEW_DESC view{};view.Format=format;view.ViewDimension=volume ? D3D12_UAV_DIMENSION_TEXTURE3D:D3D12_UAV_DIMENSION_TEXTURE2D;
        if(volume)view.Texture3D.WSize=desc.DepthOrArraySize;
        device->CreateUnorderedAccessView(image.Get(),nullptr,&view,cpu->GetCPUDescriptorHandleForHeapStart());
        device->CopyDescriptorsSimple(1,gpu->GetCPUDescriptorHandleForHeapStart(),cpu->GetCPUDescriptorHandleForHeapStart(),D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        }
        if(FAILED(hr=allocator->Reset()) || FAILED(hr=list->Reset(allocator.Get(),nullptr)))return hr;
        ID3D12DescriptorHeap* heaps[]{gpu.Get()};list->SetDescriptorHeaps(1,heaps);FLOAT color[4];std::memcpy(color,values.data(),sizeof color);
        if(render_target)list->ClearRenderTargetView(rtv->GetCPUDescriptorHandleForHeapStart(),color,0,nullptr);
        else if(floating)list->ClearUnorderedAccessViewFloat(gpu->GetGPUDescriptorHandleForHeapStart(),cpu->GetCPUDescriptorHandleForHeapStart(),image.Get(),color,0,nullptr);
        else list->ClearUnorderedAccessViewUint(gpu->GetGPUDescriptorHandleForHeapStart(),cpu->GetCPUDescriptorHandleForHeapStart(),image.Get(),values.data(),0,nullptr);
        D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition={image.Get(),D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,state,D3D12_RESOURCE_STATE_COPY_SOURCE};list->ResourceBarrier(1,&barrier);
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};UINT64 total=0;device->GetCopyableFootprints(&desc,0,1,0,&footprint,nullptr,nullptr,&total);
        if(total>readback->GetDesc().Width)return E_INVALIDARG;
        D3D12_TEXTURE_COPY_LOCATION from{},to{};from.pResource=image.Get();from.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        to.pResource=readback.Get();to.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;to.PlacedFootprint=footprint;list->CopyTextureRegion(&to,0,0,0,&from,nullptr);
        if(FAILED(hr=list->Close()))return hr;ID3D12CommandList* lists[]{list.Get()};queue->ExecuteCommandLists(1,lists);
        if(FAILED(hr=queue->Signal(fence.Get(),++value)) || FAILED(hr=fence->SetEventOnCompletion(value,event)))return hr;
        if(WaitForSingleObject(event,5000)!=WAIT_OBJECT_0)return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
        void* data=nullptr;const D3D12_RANGE read{0,SIZE_T(total)};if(FAILED(hr=readback->Map(0,&read,&data)))return hr;
        const UINT depth=volume ? desc.DepthOrArraySize:1;const SIZE_T row=SIZE_T(desc.Width)*bytes;out.resize(row*desc.Height*depth);
        for(UINT z=0;z<depth;++z)for(UINT y=0;y<desc.Height;++y)
            std::memcpy(out.data()+(SIZE_T(z)*desc.Height+y)*row,static_cast<BYTE*>(data)+footprint.Offset+(SIZE_T(z)*desc.Height+y)*footprint.Footprint.RowPitch,row);
        const D3D12_RANGE no_write{0,0};readback->Unmap(0,&no_write);return S_OK;
    }
};
SIZE_T hook_size = 0;
ULONGLONG hook_attributes = 0;
void* hook_pointer = nullptr;
unsigned hook_frees = 0;
using GraphicsAllocate = void* (WINAPI*)(SIZE_T, ULONGLONG);
using GraphicsFree = void (WINAPI*)(void*, ULONGLONG);
GraphicsAllocate graphics_default_allocate = nullptr;
GraphicsFree graphics_default_free = nullptr;
unsigned graphics_hook_allocations = 0, graphics_hook_frees = 0, graphics_hook_wrong_allocator = 0;
unsigned esram_cpu_allocations = 0, esram_ddr_allocations = 0;
void* WINAPI esram_allocate_hook(SIZE_T size, ULONGLONG attributes) {
    if ((attributes>>29)&15) ++esram_ddr_allocations;
    else ++esram_cpu_allocations;
    return graphics_default_allocate(size,attributes);
}
void WINAPI esram_free_hook(void* pointer, ULONGLONG attributes) { graphics_default_free(pointer,attributes); }
bool graphics_force_header = false;
void* WINAPI graphics_allocate_hook(SIZE_T size, ULONGLONG attributes) {
    ++graphics_hook_allocations;
    const bool system = ((attributes >> 16) & 255) == 0x80;
    if (!system) ++graphics_hook_wrong_allocator;
    const bool header = graphics_force_header || !system;
    auto* memory = static_cast<char*>(graphics_default_allocate(size + (header ? 16 : 0), attributes));
    if (memory && header) std::memset(memory, 0x5a, 16);
    return memory ? memory + (header ? 16 : 0) : nullptr;
}
void WINAPI graphics_free_hook(void* memory, ULONGLONG attributes) {
    ++graphics_hook_frees;
    const bool system = ((attributes >> 16) & 255) == 0x80;
    graphics_default_free(static_cast<char*>(memory) - (graphics_force_header || !system ? 16 : 0), attributes);
}
void* WINAPI allocate_hook(SIZE_T size, ULONGLONG attributes) {
    hook_size = size; hook_attributes = attributes;
    return &hook_size;
}
void WINAPI release_hook(void* pointer, ULONGLONG attributes) {
    hook_pointer = pointer; hook_attributes = attributes; ++hook_frees;
}
void check(bool ok, const char* what) {
    if (!ok) { ++failures; std::cerr << "FAIL: " << what << '\n'; }
}
std::vector<std::uint8_t> read(const char* path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}
} // namespace

int main(int argc, char** argv) {
    if(argc==2 && std::string(argv[1])=="--graphics-stream") {
        using xboxone::era::GraphicsCallStream;
        std::vector<int> order;
        std::weak_ptr<int> retained;
        std::future<void> drained;
        const auto producer=std::this_thread::get_id();
        {
            GraphicsCallStream stream;
            std::promise<void> release;
            auto gate=release.get_future().share();
            auto lifetime=std::make_shared<int>(17);retained=lifetime;
            auto first=stream.submit([gate,lifetime,&order] {gate.wait();order.push_back(*lifetime);});
            lifetime.reset();
            std::vector<std::future<int>> calls;
            for(int i=0;i<64;++i)calls.push_back(stream.submit([i,&order,producer] {
                if(std::this_thread::get_id()==producer)throw std::runtime_error("producer executed graphics work");
                order.push_back(i);return i;
            }));
            check(!retained.expired(),"queued graphics commands retain their resources");
            release.set_value();first.get();
            for(int i=0;i<64;++i)check(calls[i].get()==i,"graphics stream returns each command result");
            bool ordered=order.size()==65 && order.front()==17;
            for(int i=0;i<64 && ordered;++i)ordered=order[i+1]==i;
            check(ordered,"graphics commands execute in committed stream order on the worker");
            check(stream.invoke([&] {return stream.invoke([] {return 42;});})==42,
                "worker replay can call graphics helpers without deadlocking or changing threads");
            check(stream.invoke([] {return E_INVALIDARG;})==E_INVALIDARG,"graphics worker preserves native failure codes");
            bool failed=false;
            try {stream.invoke([]() -> int {throw std::runtime_error("graphics test failure");});}
            catch(const std::runtime_error& e){failed=std::string(e.what())=="graphics test failure";}
            check(failed && stream.invoke([] {return 9;})==9,"failed graphics calls propagate and leave the consumer usable");
            std::atomic<int> active{0},completed{0};std::atomic<bool> serialized{true};
            std::vector<std::thread> producers;
            for(int i=0;i<4;++i)producers.emplace_back([&] {
                for(int n=0;n<32;++n)stream.invoke([&] {
                    if(active.fetch_add(1)!=0)serialized=false;
                    ++completed;--active;
                });
            });
            for(auto& thread:producers)thread.join();
            check(serialized && completed==128,"concurrent producers share one ordered graphics consumer");
            drained=stream.submit([&order] {order.push_back(99);});
        }
        check(drained.wait_for(std::chrono::seconds(0))==std::future_status::ready && order.back()==99,
            "graphics stream shutdown drains submitted commands before releasing storage");
        drained.get();
        check(retained.expired(),"retired graphics commands release retained resources");
        return failures ? 1:0;
    }

    if (argc == 4 && std::string(argv[1]) == "--tls-modules") {
        using namespace xboxone::era;
        const auto module_file = read(argv[2]);
        const auto title_file = read(argv[3]);
        using Value = DWORD(*)(DWORD);
        using Event = DWORD(*)(DWORD);
        using Receipt = void(*)(DWORD*, BOOL);
        Value existing_value = nullptr;
        HANDLE probe = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        HANDLE probed = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        HANDLE finish = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!probe || !probed || !finish) return 1;
        std::thread existing([&] {
            SetEvent(probed);
            if (WaitForSingleObject(probe, 5000) == WAIT_OBJECT_0 && existing_value)
                check(existing_value(505) == 0x51a7b007, "thread predating LoadLibrary receives the module template without a spurious thread attach");
            else check(false, "preexisting TLS worker received its bounded probe");
            SetEvent(probed);
            WaitForSingleObject(finish, 5000);
        });
        check(WaitForSingleObject(probed, 5000) == WAIT_OBJECT_0, "worker exists before module loading");
        Image title;
        LoadResult result;
        check(title.load(title_file, resolve_kernelx, result), "title TLS maps alongside library slots");
        for (const auto& error : result.errors) std::cerr << "  title TLS: " << error << '\n';
        auto first = std::make_unique<Image>();
        Image second, overflow;
        check(first->load(module_file, {}, result, ImageKind::module), "first DLL TLS maps without a title-specific binding");
        for (const auto& error : result.errors) std::cerr << "  module TLS: " << error << '\n';
        check(second.load(module_file, {}, result, ImageKind::module), "second DLL owns a separate TLS slot");
        check(!overflow.load(module_file, {}, result, ImageKind::module) && !overflow.base(), "exhausted module TLS pool fails explicitly");
        const auto a = reinterpret_cast<Value>(first->find_export("era_tls_module_value"));
        const auto b = reinterpret_cast<Value>(second.find_export("era_tls_module_value"));
        const auto event = reinterpret_cast<Event>(first->find_export("era_tls_module_event"));
        const auto receipt = reinterpret_cast<Receipt>(first->find_export("era_tls_module_receipt"));
        check(a && b && event && receipt, "TLS fixture exports were validated");
        if (!a || !b || !event || !receipt) {
            SetEvent(probe); SetEvent(finish); existing.join();
            CloseHandle(probe); CloseHandle(probed); CloseHandle(finish);
            return 1;
        }
        check(event(0) == 0, "mapping a DLL runs neither TLS callbacks nor DllMain");
        std::thread checked([&] { check(a(9) == 0x51a7b007 && b(10) == 0x51a7b007,
            "check-only thread receives independent raw templates without guest callbacks"); });
        checked.join();
        check(event(0) == 0, "check-only thread detach executes no guest code");
        check(first->attach_module() && first->attach_module() && second.attach_module(), "explicit TLS DLL attach is idempotent");
        check(event(0) == 0x11 && event(1) == 0x21 && event(2) == 0,
            "TLS process attach precedes DllMain exactly once");
        check(a(101) == 37 && b(202) == 37, "each module initialized its own calling-thread template");
        existing_value = a;
        SetEvent(probe);
        check(WaitForSingleObject(probed, 5000) == WAIT_OBJECT_0, "preexisting worker completed its TLS access");
        std::thread worker([&] { check(a(303) == 37 && b(404) == 37,
            "new worker receives both module templates and callbacks"); });
        worker.join();
        check(a(101) == 101 && b(202) == 202, "worker writes do not change either owner TLS block");
        check(event(2) == 0x12 && event(3) == 0x22 && event(4) == 0x13 && event(5) == 0x23,
            "thread notifications preserve TLS callback/DllMain order");
        DWORD detached = 0;
        receipt(&detached, FALSE);
        first.reset();
        check(detached == 0x1020, "process TLS detach precedes DllMain before releasing the mapping");
        SetEvent(finish); existing.join();
        CloseHandle(probe); CloseHandle(probed); CloseHandle(finish);
        check(overflow.load(module_file, {}, result, ImageKind::module), "quiescent module slot is reusable");
        const auto rejected_receipt = reinterpret_cast<Receipt>(overflow.find_export("era_tls_module_receipt"));
        DWORD rejected = 0;
        if (rejected_receipt) rejected_receipt(&rejected, TRUE);
        check(rejected_receipt && !overflow.attach_module() && rejected == 0x11211020,
            "failed DllMain preserves attach failure and balances TLS detach");
        check(!overflow.attach_module() && rejected == 0x11211020,
            "a failed DLL initialization cannot silently reenter partially initialized callbacks");
        auto malformed = module_file;
        const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(malformed.data());
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(malformed.data() + dos->e_lfanew);
        nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_TLS] = {nt->OptionalHeader.SizeOfImage - 8, 40};
        check(!overflow.load(malformed, {}, result, ImageKind::module) && !overflow.base(),
            "malformed TLS does not consume a reusable slot");
        if (title.entry()) {
            title.process_attach();
            check(reinterpret_cast<unsigned long long(*)()>(title.entry())() == 0,
                "title TLS remains independent of loaded libraries and their worker callbacks");
        }
        std::cout << (failures ? "module TLS: FAILED\n" : "module TLS: all checks passed\n");
        return failures ? 1 : 0;
    }
    using namespace xboxone::era;
    if(argc==5 && std::string(argv[1])=="--xg-uav-srd") {
        const std::filesystem::path original_path=argv[4];std::string hash,error;
        if(!xboxone::inventory::fingerprint(original_path,hash,error) || hash!="bca2d2d3dff9bd88926e5a2603bacbbcc203e67af77ce092db654ad7daa772f1")return 1;
        const HMODULE original=LoadLibraryExW(original_path.c_str(),nullptr,DONT_RESOLVE_DLL_REFERENCES);
        const HMODULE crt=LoadLibraryExW(L"ucrtbase.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        if(!original || !crt)return 1;
        // Only this private mapping's CRT data-copy imports are bound; no original initializers run.
        for(const auto& entry:std::array<std::pair<UINT,const char*>,3>{{{0xd61f0,"memset"},{0xd6248,"memcpy"},{0xd62a8,"memmove"}}}) {
            auto* slot=reinterpret_cast<void**>(reinterpret_cast<BYTE*>(original)+entry.first);DWORD protection=0,ignored=0;
            const auto address=GetProcAddress(crt,entry.second);
            if(!address || !VirtualProtect(slot,sizeof *slot,PAGE_READWRITE,&protection))return 1;
            *slot=reinterpret_cast<void*>(address);VirtualProtect(slot,sizeof *slot,protection,&ignored);
        }
        HRESULT hr=bind_d3d11x_abi(std::filesystem::path(argv[2]).wstring());
        if(SUCCEEDED(hr))hr=bind_xg_module(std::filesystem::path(argv[3]).wstring());
        if(SUCCEEDED(hr))hr=initialize_xg_module();if(FAILED(hr))return 1;
        struct Parameters {UINT version,flags;void* tess;void* factors;UINT affinity,de_ring,ce_ring,de_segment,ce_segment;} parameters{};
        parameters.version=0x1000b;parameters.flags=0x4000000;
        using Create=HRESULT(WINAPI*)(const Parameters*,void**,void**);
        const auto create=reinterpret_cast<Create>(resolve_d3d11x({"d3d11_x.dll","D3D11XCreateDeviceX",0}));
        void* device=nullptr;void* context=nullptr;
        if(!create || FAILED(create(&parameters,&device,&context)))return 1;
        const auto vt=[](void* object){return *static_cast<void***>(object);};
        using Count=ULONG(STDMETHODCALLTYPE*)(void*);
        using Original=void*(*)(UINT32*,const UINT32*,UINT,UINT,const UINT*,UINT,UINT,UINT,void*);
        const auto descriptor=reinterpret_cast<Original>(reinterpret_cast<BYTE*>(original)+0x9b2f0);
        UINT comparisons=0;
        for(const UINT width:{32u,64u,256u,384u})for(const bool volume:{false,true})for(const UINT arrays:{1u,4u,6u})
            for(const auto format:{DXGI_FORMAT_R8_UINT,DXGI_FORMAT_R10G10B10A2_UNORM,DXGI_FORMAT_R32_FLOAT,DXGI_FORMAT_R16_UNORM,DXGI_FORMAT_R16G16B16A16_FLOAT,
                DXGI_FORMAT_R8_SNORM,DXGI_FORMAT_R8G8_SNORM,DXGI_FORMAT_R8G8B8A8_SNORM,DXGI_FORMAT_R16_SNORM,DXGI_FORMAT_R16G16_SNORM,DXGI_FORMAT_R16G16B16A16_SNORM}) {
                if(volume && arrays!=1)continue;
                struct Desc2 {D3D11_TEXTURE2D_DESC standard{};UINT extension[2]{};} desc2;
                struct Desc3 {D3D11_TEXTURE3D_DESC standard{};UINT extension[2]{};} desc3;
                const UINT binds=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS;
                desc2.standard={width,width,4,arrays,format,{1,0},D3D11_USAGE_DEFAULT,binds,0,arrays==6 ? UINT(D3D11_RESOURCE_MISC_TEXTURECUBE):0};
                desc3.standard={width,width,16,4,format,D3D11_USAGE_DEFAULT,binds,0,0};
                void* resource=nullptr;
                hr=reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*,const void*,const D3D11_SUBRESOURCE_DATA*,void**)>(vt(device)[volume ? 6:5])(device,volume ? static_cast<void*>(&desc3):static_cast<void*>(&desc2),nullptr,&resource);
                check(hr==S_OK && resource,"original UAV descriptor control creates the shared native texture");if(!resource)return 1;
                TitleTextureLayout layout;
                hr=volume ? create_xg_texture3d_layout(width,width,16,4,format,binds,layout):create_xg_texture_layout(width,width,4,arrays,format,UINT_MAX,layout,binds);
                if(FAILED(hr))return 1;
                for(UINT mip=0;mip<4;++mip)for(UINT range=0;range<(volume ? 3u:1u);++range) {
                    D3D11_UNORDERED_ACCESS_VIEW_DESC selected{};selected.Format=format;
                    selected.ViewDimension=volume ? D3D11_UAV_DIMENSION_TEXTURE3D:arrays==1 ? D3D11_UAV_DIMENSION_TEXTURE2D:D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
                    selected.Texture2D.MipSlice=mip;
                    const UINT depth=(std::max)(1u,16u>>mip),first=volume ? (range ? 1:0):arrays>1 ? 1:0;
                    const UINT count=volume ? range==1 ? (std::max)(1u,depth-2):depth-first:arrays-first;
                    if(volume){selected.Texture3D.FirstWSlice=first;selected.Texture3D.WSize=range==2 ? UINT_MAX:count;}
                    else if(arrays>1){selected.Texture2DArray.FirstArraySlice=first;selected.Texture2DArray.ArraySize=count;}
                    void* view=nullptr;
                    check(reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*,void*,const D3D11_UNORDERED_ACCESS_VIEW_DESC*,void**)>(vt(device)[8])(device,resource,&selected,&view)==S_OK && view,"original UAV descriptor control creates the requested range");if(!view)return 1;
                    auto source=layout.guest_descriptor;void* base=nullptr;std::memcpy(&base,static_cast<BYTE*>(view)+80,sizeof base);
                    source[0]=UINT32(reinterpret_cast<std::uintptr_t>(base)>>8);
                    std::array<UINT32,8> expected{},actual{};std::array<UINT,4> selectors{};
                    for(UINT c=0;c<4;++c)selectors[c]=(source[3]>>(c*3))&7;
                    descriptor(expected.data(),source.data(),(source[1]>>20)&63,(source[1]>>26)&15,selectors.data(),mip,first,first+count-1,nullptr);
                    std::memcpy(actual.data(),static_cast<BYTE*>(view)+48,sizeof actual);
                    if(actual!=expected) {
                        std::cerr<<"UAV SRD mismatch width="<<width<<" volume="<<volume<<" arrays="<<arrays<<" format="<<format<<" mip="<<mip<<" range="<<range;
                        for(UINT word=0;word<8;++word)if(actual[word]!=expected[word])std::cerr<<" word"<<word<<"="<<std::hex<<actual[word]<<"/"<<expected[word]<<std::dec;
                        std::cerr<<'\n';
                    }
                    check(actual==expected,"UAV descriptor matches the original 11295 helper including mip rebasing and volume ranges");
                    ++comparisons;
                    reinterpret_cast<Count>(vt(view)[2])(view);
                }
                reinterpret_cast<Count>(vt(resource)[2])(resource);
            }
        reinterpret_cast<Count>(vt(context)[2])(context);reinterpret_cast<Count>(vt(device)[2])(device);
        FreeLibrary(original);FreeLibrary(crt);
        std::cout<<"Original UAV SRDs: "<<comparisons<<" comparisons, "<<failures<<" failures\n";return failures ? 1:0;
    }
    if(argc==4 && std::string(argv[1])=="--package-metadata") {
        const std::filesystem::path root=argv[2];std::string hash,error;
        if(!xboxone::inventory::fingerprint(root/argv[3],hash,error))return 1;
        set_title_package_root(root.wstring(),argv[3],hash);
        xboxone::appmodel::Manifest manifest;std::wstring location;std::string revision;
        const HRESULT hr=title_package_metadata(manifest,location,revision);
        std::cout<<"package metadata HRESULT 0x"<<std::hex<<static_cast<UINT>(hr)<<" name "<<manifest.name<<" revision "<<revision<<'\n';
        return FAILED(hr) ? 1 : 0;
    }
    if ((argc == 4 || (argc == 5 && std::string(argv[4]) == "--warp")) &&
        (std::string(argv[1]) == "--xg-layout" || std::string(argv[1]) == "--xg-graphics" || std::string(argv[1]) == "--xg-msaa" || std::string(argv[1]) == "--xg-depth")) {
        const bool graphics = std::string(argv[1]) == "--xg-graphics";
        set_stop_on_missing_activation(true);
        set_graphics_diagnostics(false, argc == 5);
        HRESULT hr = bind_d3d11x_abi(std::filesystem::path(argv[2]).wstring());
        if (SUCCEEDED(hr)) hr = bind_xg_module(std::filesystem::path(argv[3]).wstring());
        if (SUCCEEDED(hr)) hr = initialize_xg_module();
        if (std::string(argv[1]) == "--xg-depth") {
            if (FAILED(hr)) return 1;
            using Create = HRESULT(WINAPI*)(void*,D3D_DRIVER_TYPE,HMODULE,UINT,const D3D_FEATURE_LEVEL*,UINT,UINT,void**,D3D_FEATURE_LEVEL*,void**);
            const auto create = reinterpret_cast<Create>(resolve_d3d11x({"d3d11_x.dll","D3D11CreateDevice",0}));
            void* device = nullptr; void* context = nullptr;
            if (!create || FAILED(create(nullptr,D3D_DRIVER_TYPE_HARDWARE,nullptr,0,nullptr,0,7,&device,nullptr,&context))) return 1;
            const auto vt = [](void* object){return *static_cast<void***>(object);};
            using Count = ULONG(STDMETHODCALLTYPE*)(void*);
            void* embedded_device = nullptr;
            using GetDevice = void(STDMETHODCALLTYPE*)(void*,void**);
            auto* embedded = reinterpret_cast<void**>(static_cast<char*>(context)+0x3d0);
            check(embedded[34] != nullptr,"observed modern inline OM call has an explicit dispatch target");
            reinterpret_cast<GetDevice>(embedded[3])(context,&embedded_device);
            check(embedded_device == device,"modern embedded context dispatch preserves its device identity");
            if (embedded_device) reinterpret_cast<Count>(vt(embedded_device)[2])(embedded_device);
            using SetViewport = void(STDMETHODCALLTYPE*)(void*,UINT,const D3D11_VIEWPORT*);
            using GetViewport = void(STDMETHODCALLTYPE*)(void*,UINT*,D3D11_VIEWPORT*);
            const auto set_viewports = reinterpret_cast<SetViewport>(embedded[45]);
            const auto get_viewports = reinterpret_cast<GetViewport>(embedded[96]);
            const D3D11_VIEWPORT viewport{13,17,1920,1080,.125f,.875f};
            set_viewports(context,1,&viewport);
            UINT viewport_count = 0;
            get_viewports(context,&viewport_count,nullptr);
            check(viewport_count==1,"viewport count queries retain the configured raster state");
            struct { D3D11_VIEWPORT value[2]; UINT64 guard=0x1122334455667788ull; } viewport_output{};
            viewport_count = 2;
            get_viewports(context,&viewport_count,viewport_output.value);
            const D3D11_VIEWPORT empty_viewport{};
            check(viewport_count==1 && !std::memcmp(&viewport,viewport_output.value,sizeof viewport) &&
                !std::memcmp(&empty_viewport,&viewport_output.value[1],sizeof viewport) && viewport_output.guard==0x1122334455667788ull,
                "viewport state preserves origin, dimensions, depth range and output bounds");
            static UINT invalid_viewport_slot = 0;
            set_d3d11x_trap_handler([](const char*,unsigned slot)->std::uintptr_t { invalid_viewport_slot=slot; return 0; });
            set_viewports(context,17,&viewport);
            set_d3d11x_trap_handler(nullptr);
            viewport_count = 1;
            get_viewports(context,&viewport_count,viewport_output.value);
            check(invalid_viewport_slot==45 && viewport_count==1 && !std::memcmp(&viewport,viewport_output.value,sizeof viewport),
                "invalid viewport binding reports its slot and preserves prior state");
            set_viewports(context,0,nullptr);
            get_viewports(context,&viewport_count,nullptr);
            check(!viewport_count,"zero viewport binding clears raster viewport state");
            D3D11_RASTERIZER_DESC raster{};
            raster.FillMode=D3D11_FILL_SOLID; raster.CullMode=D3D11_CULL_NONE; raster.DepthClipEnable=TRUE;
            D3D12_DEPTH_STENCIL_DESC depth_state{};
            depth_state.DepthEnable=TRUE; depth_state.DepthWriteMask=D3D12_DEPTH_WRITE_MASK_ALL; depth_state.DepthFunc=D3D12_COMPARISON_FUNC_LESS_EQUAL;
            depth_state.FrontFace=depth_state.BackFace={D3D12_STENCIL_OP_KEEP,D3D12_STENCIL_OP_KEEP,D3D12_STENCIL_OP_KEEP,D3D12_COMPARISON_FUNC_ALWAYS};
            D3D11_BLEND_DESC blend{};
            blend.RenderTarget[0].RenderTargetWriteMask=D3D11_COLOR_WRITE_ENABLE_ALL;
            using CreateState=HRESULT(STDMETHODCALLTYPE*)(void*,const void*,void**);
            for(UINT kind=0;kind<3;++kind) {
                const UINT factory_slot=kind==0 ? 22 : kind==1 ? 21 : 20;
                const void* description=kind==0 ? static_cast<const void*>(&raster) : kind==1 ? static_cast<const void*>(&depth_state) : static_cast<const void*>(&blend);
                void* state=nullptr;
                check(reinterpret_cast<CreateState>(vt(device)[factory_slot])(device,description,&state)==S_OK && state,
                    "modern pipeline factory retains its state object");
                if(!state) continue;
                const auto bind=[&](void* value) {
                    if(kind==0) reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*)>(embedded[44])(context,value);
                    else if(kind==1) reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*,UINT)>(embedded[37])(context,value,7);
                    else {
                        const FLOAT factor[]{.25f,.5f,.75f,1};
                        reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*,const FLOAT*,UINT)>(embedded[36])(context,value,factor,0xff);
                    }
                };
                bind(state); bind(state);
                check(*reinterpret_cast<ULONG*>(static_cast<char*>(state)+12)==2,"repeated pipeline binding retains exactly one reference");
                bind(nullptr);
                check(reinterpret_cast<Count>(vt(state)[2])(state)==0,"pipeline unbinding releases ownership and its cache entry");
            }
            using CreateSampler = HRESULT(STDMETHODCALLTYPE*)(void*,const D3D12_SAMPLER_DESC*,void**);
            D3D12_SAMPLER_DESC sampler_desc{};
            sampler_desc.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
            sampler_desc.AddressU = sampler_desc.AddressV = sampler_desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
            sampler_desc.MaxAnisotropy = 1; sampler_desc.ComparisonFunc = D3D12_COMPARISON_FUNC_ALWAYS;
            sampler_desc.MaxLOD = D3D12_FLOAT32_MAX;
            for (float& channel : sampler_desc.BorderColor) channel = 1;
            void* sampler = nullptr;
            const auto create_sampler = reinterpret_cast<CreateSampler>(vt(device)[23]);
            check(create_sampler(device,&sampler_desc,&sampler)==S_OK && sampler,"modern factory creates a native white-border sampler");
            if (sampler) {
                std::array<UINT32,4> words{};
                using GetSamplerX = void(STDMETHODCALLTYPE*)(void*,UINT32*);
                reinterpret_cast<GetSamplerX>(vt(sampler)[9])(sampler,words.data());
                check(words[3]==0x80000000u && *reinterpret_cast<ULONG*>(static_cast<char*>(sampler)+12)==1,
                    "modern sampler retains its border SRD and reference ABI");
                void* duplicate = nullptr;
                check(create_sampler(device,&sampler_desc,&duplicate)==S_OK && duplicate==sampler,
                    "modern sampler cache shares the same live object");
                if (duplicate) reinterpret_cast<Count>(vt(duplicate)[2])(duplicate);
                check(reinterpret_cast<Count>(vt(sampler)[2])(sampler)==0,"modern sampler cache releases its device ownership");
            }
            using CreateBuffer = HRESULT(STDMETHODCALLTYPE*)(void*,const D3D11_BUFFER_DESC*,const D3D11_SUBRESOURCE_DATA*,void**);
            const auto create_buffer = reinterpret_cast<CreateBuffer>(vt(device)[3]);
            // Nonzero adjacent bytes catch accidental reads of the older 32-byte ABI.
            struct { D3D11_BUFFER_DESC desc; UINT64 guard; } buffer_input{
                {800,D3D11_USAGE_IMMUTABLE,D3D11_BIND_VERTEX_BUFFER,0,0,0},0xffffffffffffffffull};
            std::array<unsigned char,800> vertices{};
            for (UINT i=0;i<vertices.size();++i) vertices[i]=static_cast<unsigned char>(i*13+7);
            D3D11_SUBRESOURCE_DATA initial{vertices.data(),0,0};
            void* buffer = nullptr;
            check(create_buffer(device,&buffer_input.desc,nullptr,&buffer)==E_INVALIDARG && !buffer,
                "modern immutable buffers require initial data");
            check(create_buffer(device,&buffer_input.desc,&initial,&buffer)==S_OK && buffer,
                "modern CreateBuffer reads only its verified 24-byte descriptor");
            if (buffer) {
                struct { D3D11_BUFFER_DESC desc{}; UINT64 guard=0x1122334455667788ull; } output;
                using GetBufferDesc = void(STDMETHODCALLTYPE*)(void*,D3D11_BUFFER_DESC*);
                reinterpret_cast<GetBufferDesc>(vt(buffer)[12])(buffer,&output.desc);
                check(!std::memcmp(&output.desc,&buffer_input.desc,sizeof output.desc) && output.guard==0x1122334455667788ull,
                    "modern GetDesc preserves the descriptor and adjacent memory");
                check(*reinterpret_cast<ULONG*>(static_cast<char*>(buffer)+12)==1,
                    "modern buffers expose their live reference count at offset 12");
                const auto* canonical=*reinterpret_cast<void**>(static_cast<char*>(buffer)+48);
                check(canonical && !std::memcmp(canonical,vertices.data(),vertices.size()),
                    "modern immutable buffers preserve every initial vertex byte");
                reinterpret_cast<Count>(vt(buffer)[2])(buffer);
            }
            struct TextureDesc { D3D11_TEXTURE2D_DESC standard; UINT extra[2]{}; } desc{};
            desc.standard = {32,16,1,1,DXGI_FORMAT_D32_FLOAT,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_DEPTH_STENCIL,0,0};
            using Texture = HRESULT(STDMETHODCALLTYPE*)(void*,const TextureDesc*,const D3D11_SUBRESOURCE_DATA*,void**);
            for (const auto format : {DXGI_FORMAT_B5G6R5_UNORM,DXGI_FORMAT_B8G8R8A8_UNORM}) {
                TextureDesc dynamic{};
                dynamic.standard = {33,17,1,1,format,{1,0},D3D11_USAGE_DYNAMIC,D3D11_BIND_SHADER_RESOURCE,0,0};
                void* image = nullptr;
                check(reinterpret_cast<Texture>(vt(device)[5])(device,&dynamic,nullptr,&image)==S_OK && image,
                    "modern dynamic image uses the shared canonical texture registry");
                if (!image) continue;
                using CreateSrv=HRESULT(STDMETHODCALLTYPE*)(void*,void*,const D3D11_SHADER_RESOURCE_VIEW_DESC*,void**);
                void* srv=nullptr;
                check(reinterpret_cast<CreateSrv>(vt(device)[7])(device,image,nullptr,&srv)==S_OK && srv,
                    "dynamic texture SRV retains the same canonical allocation");
                const auto canonical=srv ? *reinterpret_cast<void**>(static_cast<char*>(srv)+80) : nullptr;
                using Map = HRESULT(STDMETHODCALLTYPE*)(void*,void*,UINT,D3D11_MAP,UINT,D3D11_MAPPED_SUBRESOURCE*);
                using Unmap = void(STDMETHODCALLTYPE*)(void*,void*);
                const auto map = reinterpret_cast<Map>(embedded[16]);
                const auto unmap = reinterpret_cast<Unmap>(embedded[17]);
                const UINT pixel = format==DXGI_FORMAT_B5G6R5_UNORM ? 2 : 4;
                for (UINT pass=0;pass<2;++pass) {
                    D3D11_MAPPED_SUBRESOURCE mapped{};
                    hr=map(context,image,0,D3D11_MAP_WRITE_DISCARD,0,&mapped);
                    check(hr==S_OK && mapped.pData && mapped.RowPitch>=33*pixel && mapped.DepthPitch==17*mapped.RowPitch,
                        "texture map exposes an actual linear footprint with consistent pitches");
                    if (FAILED(hr)) break;
                    D3D11_MAPPED_SUBRESOURCE duplicate{};
                    check(map(context,image,0,D3D11_MAP_WRITE_DISCARD,0,&duplicate)==HRESULT_FROM_WIN32(ERROR_BUSY) && !duplicate.pData,
                        "nested texture maps are rejected without releasing CPU ownership");
                    auto* bytes=static_cast<unsigned char*>(mapped.pData);
                    for(UINT y=0;y<17;++y) for(UINT x=0;x<33*pixel;++x) bytes[y*mapped.RowPitch+x]=static_cast<unsigned char>(pass*29+y*13+x*7);
                    unmap(context,image);
                    TitleTextureLayout layout;
                    hr=create_xg_texture_layout(33,17,1,1,format,UINT_MAX,layout,D3D11_BIND_SHADER_RESOURCE);
                    std::vector<unsigned char> linear(33*17*pixel);
                    if(SUCCEEDED(hr)) hr=canonical ? layout.copy(layout.context.get(),false,linear.data(),canonical,0,33*pixel,33*17*pixel,nullptr) : E_POINTER;
                    bool equal=SUCCEEDED(hr);
                    for(UINT y=0;y<17 && equal;++y) for(UINT x=0;x<33*pixel;++x)
                        equal=equal && linear[y*33*pixel+x]==static_cast<unsigned char>(pass*29+y*13+x*7);
                    check(equal,"texture unmap retiles every logical pixel and refreshes repeated writes");
                }
                if(srv) reinterpret_cast<Count>(vt(srv)[2])(srv);
                reinterpret_cast<Count>(vt(image)[2])(image);
            }
            void* texture = nullptr;
            {
                TextureDesc color_desc{};
                color_desc.standard={32,16,1,1,DXGI_FORMAT_B8G8R8A8_UNORM,{1,0},D3D11_USAGE_DEFAULT,
                    D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE,0,0};
                void* color=nullptr;
                check(reinterpret_cast<Texture>(vt(device)[5])(device,&color_desc,nullptr,&color)==S_OK && color,
                    "native clear fixture creates a shared renderable image");
                if(color) {
                    using View=HRESULT(STDMETHODCALLTYPE*)(void*,void*,const void*,void**);
                    void* rtv=nullptr; void* srv=nullptr;
                    check(reinterpret_cast<View>(vt(device)[9])(device,color,nullptr,&rtv)==S_OK && rtv,"native clear creates its render-target view");
                    check(reinterpret_cast<View>(vt(device)[7])(device,color,nullptr,&srv)==S_OK && srv,"native clear exposes the same image through its SRV");
                    if(rtv && srv) {
                        using Clear=void(STDMETHODCALLTYPE*)(void*,void*,const FLOAT*);
                        TitleTextureLayout layout;
                        hr=create_xg_texture_layout(32,16,1,1,DXGI_FORMAT_B8G8R8A8_UNORM,UINT_MAX,layout,color_desc.standard.BindFlags);
                        const auto canonical=*reinterpret_cast<void**>(static_cast<char*>(srv)+80);
                        for(UINT pass=0;pass<2;++pass) {
                            const FLOAT rgba[]{(17.f+pass*17)/255,34.f/255,51.f/255,68.f/255};
                            reinterpret_cast<Clear>(embedded[51])(context,rtv,rgba);
                            std::array<unsigned char,32*16*4> pixels{};
                            HRESULT copied=SUCCEEDED(hr) ? layout.copy(layout.context.get(),false,pixels.data(),canonical,0,128,2048,nullptr) : hr;
                            bool equal=SUCCEEDED(copied);
                            for(UINT i=0;i<pixels.size();i+=4) equal=equal && pixels[i]==51 && pixels[i+1]==34 && pixels[i+2]==17+pass*17 && pixels[i+3]==68;
                            check(equal,"title clear executes on D3D12 and publishes every BGRA pixel after its fence, including reuse");
                        }
                        // Exercise an indexed textured quad through the same Xbox ABI.
                        // Separate VS/PS b0 values expose accidental cross-stage binding.
                        const HMODULE compiler=LoadLibraryExW(L"d3dcompiler_47.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
                        const auto compile=compiler ? reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(compiler,"D3DCompile")) : nullptr;
                        const char* sources[]{
                            "cbuffer C:register(b0){float4 scale;} struct V{float4 p:SV_Position;float2 uv:TEXCOORD0;};"
                            "V main(float2 p:POSITION,float2 uv:TEXCOORD0){V v;v.p=float4(p*scale.xy,0,1);v.uv=uv;return v;}",
                            "cbuffer C:register(b0){float4 tint;} Texture2D image:register(t0); SamplerState sample_state:register(s0);"
                            "float4 main(float4 p:SV_Position,float2 uv:TEXCOORD0):SV_Target{return image.Sample(sample_state,uv)*tint;}"};
                        Microsoft::WRL::ComPtr<ID3DBlob> codes[2];
                        void* objects[10]{}; // VS, PS, layout, vertex/index, VS/PS CB, image, SRV, sampler.
                        bool ready=compile!=nullptr;
                        using CreateShader=HRESULT(STDMETHODCALLTYPE*)(void*,const void*,SIZE_T,void*,void**);
                        for(UINT i=0;i<2 && ready;++i) {
                            ready=SUCCEEDED(compile(sources[i],std::strlen(sources[i]),nullptr,nullptr,nullptr,"main",i ? "ps_5_0" : "vs_5_0",0,0,&codes[i],nullptr));
                            if(ready) ready=reinterpret_cast<CreateShader>(vt(device)[i ? 15 : 12])(device,codes[i]->GetBufferPointer(),codes[i]->GetBufferSize(),nullptr,&objects[i])==S_OK;
                        }
                        using CreateLayout=HRESULT(STDMETHODCALLTYPE*)(void*,const D3D12_INPUT_ELEMENT_DESC*,UINT,const void*,SIZE_T,void**);
                        const D3D12_INPUT_ELEMENT_DESC elements[]{
                            {"POSITION",0,DXGI_FORMAT_R32G32_FLOAT,0,0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
                            {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,8,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0}};
                        if(ready) ready=reinterpret_cast<CreateLayout>(vt(device)[11])(device,elements,2,codes[0]->GetBufferPointer(),codes[0]->GetBufferSize(),&objects[2])==S_OK;
                        const FLOAT vertices[]{-1,-1,0,1,-1,1,0,0,1,1,1,0,1,-1,1,1};
                        const UINT16 indices[]{0,1,2,0,2,3};
                        for(UINT i=3;i<7 && ready;++i) {
                            const D3D11_BUFFER_DESC d{i==3 ? UINT(sizeof vertices) : i==4 ? UINT(sizeof indices) : 16,
                                i<5 ? D3D11_USAGE_IMMUTABLE : D3D11_USAGE_DYNAMIC,
                                UINT(i==3 ? D3D11_BIND_VERTEX_BUFFER : i==4 ? D3D11_BIND_INDEX_BUFFER : D3D11_BIND_CONSTANT_BUFFER),
                                i<5 ? 0u : D3D11_CPU_ACCESS_WRITE,0,0};
                            const D3D11_SUBRESOURCE_DATA initial{i==3 ? static_cast<const void*>(vertices) : static_cast<const void*>(indices),0,0};
                            ready=create_buffer(device,&d,i<5 ? &initial : nullptr,&objects[i])==S_OK;
                        }
                        TextureDesc image{};
                        image.standard={8,8,1,1,DXGI_FORMAT_B8G8R8A8_UNORM,{1,0},D3D11_USAGE_DYNAMIC,D3D11_BIND_SHADER_RESOURCE,0,0};
                        if(ready) ready=reinterpret_cast<Texture>(vt(device)[5])(device,&image,nullptr,&objects[7])==S_OK;
                        if(ready) ready=reinterpret_cast<View>(vt(device)[7])(device,objects[7],nullptr,&objects[8])==S_OK;
                        D3D12_SAMPLER_DESC sampler_desc{};
                        sampler_desc.Filter=D3D12_FILTER_MIN_MAG_MIP_POINT;
                        sampler_desc.AddressU=sampler_desc.AddressV=sampler_desc.AddressW=D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
                        sampler_desc.MaxAnisotropy=1;sampler_desc.ComparisonFunc=D3D12_COMPARISON_FUNC_ALWAYS;
                        if(ready) ready=reinterpret_cast<CreateSampler>(vt(device)[23])(device,&sampler_desc,&objects[9])==S_OK;
                        void* raster_object=nullptr;
                        if(ready) ready=reinterpret_cast<CreateState>(vt(device)[22])(device,&raster,&raster_object)==S_OK;
                        check(ready,"indexed draw fixture prepares original-ABI shaders, vertices, constants, texture and sampler");
                        if(ready) {
                            using Map=HRESULT(STDMETHODCALLTYPE*)(void*,void*,UINT,D3D11_MAP,UINT,D3D11_MAPPED_SUBRESOURCE*);
                            using Unmap=void(STDMETHODCALLTYPE*)(void*,void*);
                            const auto map=reinterpret_cast<Map>(embedded[16]);const auto unmap=reinterpret_cast<Unmap>(embedded[17]);
                            D3D11_MAPPED_SUBRESOURCE mapped{};
                            const HRESULT mapped_image=map(context,objects[7],0,D3D11_MAP_WRITE_DISCARD,0,&mapped);
                            check(mapped_image==S_OK,"draw texture is writable through the coherent map path");
                            if(SUCCEEDED(mapped_image)) {std::memset(mapped.pData,255,mapped.DepthPitch);unmap(context,objects[7]);}
                            using Object=void(STDMETHODCALLTYPE*)(void*,void*);
                            reinterpret_cast<Object>(embedded[13])(context,objects[0]);reinterpret_cast<Object>(embedded[11])(context,objects[1]);
                            reinterpret_cast<Object>(embedded[19])(context,objects[2]);reinterpret_cast<Object>(embedded[44])(context,raster_object);
                            const UINT stride=16,offset=0;
                            using Vertices=void(STDMETHODCALLTYPE*)(void*,UINT,UINT,void* const*,const UINT*,const UINT*);
                            reinterpret_cast<Vertices>(embedded[20])(context,0,1,&objects[3],&stride,&offset);
                            using Indices=void(STDMETHODCALLTYPE*)(void*,UINT,void*,UINT);
                            reinterpret_cast<Indices>(embedded[21])(context,0,objects[4],0);
                            using Range=void(STDMETHODCALLTYPE*)(void*,UINT,UINT,void* const*);
                            reinterpret_cast<Range>(embedded[9])(context,0,1,&objects[5]);reinterpret_cast<Range>(embedded[18])(context,0,1,&objects[6]);
                            reinterpret_cast<Range>(embedded[12])(context,0,1,&objects[9]);
                            using Resources=void(STDMETHODCALLTYPE*)(void*,void* const*,UINT,UINT);
                            reinterpret_cast<Resources>(embedded[10])(context,&objects[8],UINT_MAX,0x76b40);
                            using Targets=void(STDMETHODCALLTYPE*)(void*,UINT,void* const*,void*);
                            reinterpret_cast<Targets>(embedded[34])(context,1,&rtv,nullptr);
                            const D3D11_VIEWPORT viewport{0,0,32,16,0,1};set_viewports(context,1,&viewport);
                            const UINT64 topology=0x000009ffc0009e00ull;std::memcpy(static_cast<char*>(context)+0x170,&topology,sizeof topology);
                            for(UINT pass=0;pass<2;++pass) {
                                const FLOAT constants[2][4]{{1,1,1,1},{pass ? .5f : .25f,pass ? .25f : .5f,pass ? .125f : .75f,1}};
                                bool written=true;
                                for(UINT stage=0;stage<2;++stage) {
                                    const HRESULT result=map(context,objects[5+stage],0,D3D11_MAP_WRITE_DISCARD,0,&mapped);
                                    written=written && SUCCEEDED(result);
                                    if(SUCCEEDED(result)) {std::memcpy(mapped.pData,constants[stage],16);unmap(context,objects[5+stage]);}
                                }
                                check(written,"per-stage constants update between indexed draws");
                                using Draw=void(STDMETHODCALLTYPE*)(void*,UINT64,INT);
                                reinterpret_cast<Draw>(embedded[14])(context,6ull<<32,0);
                                std::array<unsigned char,32*16*4> pixels{};
                                const HRESULT copied=layout.copy(layout.context.get(),false,pixels.data(),canonical,0,128,2048,nullptr);
                                bool equal=SUCCEEDED(copied);
                                // D3D FLOAT->UNORM allows 0.6 ULP on the integer side.
                                for(UINT i=0;i<pixels.size();i+=4) equal=equal && std::abs(FLOAT(pixels[i])-255*constants[1][2])<=.6f &&
                                    std::abs(FLOAT(pixels[i+1])-255*constants[1][1])<=.6f && std::abs(FLOAT(pixels[i+2])-255*constants[1][0])<=.6f && pixels[i+3]==255;
                                check(equal,"native indexed draw produces expected textured pixels, separate stage constants and fresh CPU updates");
                                using Suspend=HRESULT(STDMETHODCALLTYPE*)(void*,UINT);
                                using Resume=HRESULT(STDMETHODCALLTYPE*)(void*);
                                const auto suspend=reinterpret_cast<Suspend>(embedded[201]);
                                const auto resume=reinterpret_cast<Resume>(embedded[202]);
                                check(suspend(context,1)==E_NOTIMPL && resume(context)==DXGI_ERROR_INVALID_CALL,
                                      "unknown suspend flags preserve the running device state");
                                check(suspend(context,0)==S_OK && suspend(context,0)==DXGI_ERROR_INVALID_CALL,
                                      "modern suspend retires the real queue and rejects a duplicate transition");
                                const std::vector<unsigned char> before_suspend(static_cast<unsigned char*>(canonical),static_cast<unsigned char*>(canonical)+layout.bytes);
                                static UINT suspended_draw_slot=0;suspended_draw_slot=0;
                                set_d3d11x_trap_handler([](const char*,UINT slot)->std::uintptr_t{suspended_draw_slot=slot;return 0;});
                                reinterpret_cast<Draw>(embedded[14])(context,6ull<<32,0);
                                set_d3d11x_trap_handler(nullptr);
                                check(suspended_draw_slot==14 && !std::memcmp(canonical,before_suspend.data(),before_suspend.size()),
                                      "suspended draw is refused without changing canonical pixels");
                                check(resume(context)==S_OK && resume(context)==DXGI_ERROR_INVALID_CALL,
                                      "resume reopens the same queue and rejects an unmatched transition");
                                reinterpret_cast<Draw>(embedded[14])(context,6ull<<32,0);
                                check(!std::memcmp(canonical,before_suspend.data(),before_suspend.size()),
                                      "resumed draw preserves shader, resource and pixel state");
                                if(!equal) std::cerr << "draw pixels first BGRA " << UINT(pixels[0]) << ',' << UINT(pixels[1]) << ',' << UINT(pixels[2]) << ',' << UINT(pixels[3])
                                    << " center " << UINT(pixels[1024]) << ',' << UINT(pixels[1025]) << ',' << UINT(pixels[1026]) << ',' << UINT(pixels[1027]) << '\n';
                            }
                            reinterpret_cast<Object>(embedded[13])(context,nullptr);reinterpret_cast<Object>(embedded[11])(context,nullptr);
                            reinterpret_cast<Object>(embedded[19])(context,nullptr);reinterpret_cast<Object>(embedded[44])(context,nullptr);
                            reinterpret_cast<Vertices>(embedded[20])(context,0,1,nullptr,&stride,&offset);reinterpret_cast<Indices>(embedded[21])(context,0,nullptr,0);
                            reinterpret_cast<Range>(embedded[9])(context,0,1,nullptr);reinterpret_cast<Range>(embedded[18])(context,0,1,nullptr);
                            reinterpret_cast<Range>(embedded[12])(context,0,1,nullptr);reinterpret_cast<Resources>(embedded[10])(context,nullptr,0,0x76b40);
                            reinterpret_cast<Targets>(embedded[34])(context,0,nullptr,nullptr);
                        }
                        if(raster_object) reinterpret_cast<Count>(vt(raster_object)[2])(raster_object);
                        for(auto* object : objects) if(object) reinterpret_cast<Count>(vt(object)[2])(object);
                        codes[0].Reset();codes[1].Reset();if(compiler) FreeLibrary(compiler);
                    }
                    if(rtv) reinterpret_cast<Count>(vt(rtv)[2])(rtv);
                    if(srv) reinterpret_cast<Count>(vt(srv)[2])(srv);
                    reinterpret_cast<Count>(vt(color)[2])(color);
                }
            }
            hr = reinterpret_cast<Texture>(vt(device)[5])(device,&desc,nullptr,&texture);
            check(SUCCEEDED(hr) && texture,"modern ABI creates the expanded D32 texture");
            if (texture) {
                check(*reinterpret_cast<ULONG*>(static_cast<char*>(texture)+12) == 1,"D32 retains the modern public reference counter");
                using Dsv = HRESULT(STDMETHODCALLTYPE*)(void*,void*,const D3D11_DEPTH_STENCIL_VIEW_DESC*,void**);
                const auto create_dsv = reinterpret_cast<Dsv>(vt(device)[10]);
                void* view = reinterpret_cast<void*>(1);
                check(create_dsv(device,nullptr,nullptr,&view) == E_INVALIDARG && !view,"DSV clears its output on an invalid resource");
                D3D11_DEPTH_STENCIL_VIEW_DESC input{};
                input.Format = DXGI_FORMAT_D32_FLOAT; input.ViewDimension = D3D11_DSV_DIMENSION_TEXTURE2D;
                input.Texture2D.MipSlice = 1;
                check(create_dsv(device,texture,&input,&view) == E_INVALIDARG && !view,"DSV rejects a mip outside the resource");
                input.Texture2D.MipSlice = 0; input.Flags = D3D11_DSV_READ_ONLY_STENCIL;
                check(create_dsv(device,texture,&input,&view) == DXGI_ERROR_UNSUPPORTED && !view,"D32 cannot claim a stencil plane");
                input.Flags = 0;
                check(create_dsv(device,texture,&input,&view) == S_OK && view,"modern ABI creates a real native depth view");
                if (view) {
                    check(*reinterpret_cast<UINT*>(static_cast<char*>(view)+40) == 1,"DSV public type agrees with the original UMD");
                    using ClearDepth=void(STDMETHODCALLTYPE*)(void*,void*,UINT,FLOAT,UINT8);
                    const auto clear_depth=reinterpret_cast<ClearDepth>(embedded[54]);
                    clear_depth(context,view,D3D11_CLEAR_DEPTH,0.375f,0x7f);
                    clear_depth(context,view,D3D11_CLEAR_DEPTH|D3D11_CLEAR_STENCIL,1.f,0);
                    using Bind = void(STDMETHODCALLTYPE*)(void*,UINT,void* const*,void*);
                    const auto bind = reinterpret_cast<Bind>(embedded[34]);
                    bind(context,0,nullptr,view);
                    bind(context,0,nullptr,view);
                    check(*reinterpret_cast<ULONG*>(static_cast<char*>(view)+12) == 2,
                        "repeated OM binding retains one view without accumulating references");
                    bind(context,0,nullptr,nullptr);
                    check(*reinterpret_cast<ULONG*>(static_cast<char*>(view)+12) == 1,
                        "OM unbinding releases its retained depth view");
                    struct { D3D11_DEPTH_STENCIL_VIEW_DESC desc{}; UINT64 guard=0x1122334455667788ull; } output;
                    using GetDesc = void(STDMETHODCALLTYPE*)(void*,D3D11_DEPTH_STENCIL_VIEW_DESC*);
                    reinterpret_cast<GetDesc>(vt(view)[9])(view,&output.desc);
                    check(output.desc.Format == DXGI_FORMAT_D32_FLOAT && output.desc.ViewDimension == D3D11_DSV_DIMENSION_TEXTURE2D &&
                        !output.desc.Flags && output.guard == 0x1122334455667788ull,"DSV writes exactly its verified 24-byte description");
                    using Resource = void(STDMETHODCALLTYPE*)(void*,void**);
                    void* retained = nullptr;
                    reinterpret_cast<Resource>(vt(view)[8])(view,&retained);
                    check(retained == texture && *reinterpret_cast<ULONG*>(static_cast<char*>(texture)+12) == 3,
                        "DSV GetResource retains the same canonical texture identity");
                    reinterpret_cast<Count>(vt(retained)[2])(retained);
                    check(reinterpret_cast<Count>(vt(view)[2])(view) == 0 &&
                        *reinterpret_cast<ULONG*>(static_cast<char*>(texture)+12) == 1,"DSV retirement releases its parent ownership");
                }
                reinterpret_cast<Count>(vt(texture)[2])(texture);
            }
            reinterpret_cast<Count>(vt(context)[2])(context); reinterpret_cast<Count>(vt(device)[2])(device);
            return failures ? 1 : 0;
        }
        TitleTextureLayout layout;
        if (std::string(argv[1]) == "--xg-msaa") {
            set_graphics_diagnostics(true,argc==5);
            if (SUCCEEDED(hr)) hr = create_xg_texture_layout(32, 16, 1, 1, DXGI_FORMAT_R11G11B10_FLOAT, UINT_MAX, layout,
                D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 4);
            std::cout << "original XG MSAA4 layout hr=0x" << std::hex << hr << std::dec << " bytes=" << layout.bytes << " alignment=" << layout.alignment << '\n';
            if (FAILED(hr)) return 1;
            std::vector<unsigned char> tiled(layout.bytes + 32, 0x5a), input(32 * 16 * 4 * 4), output(input.size(), 0);
            for (UINT i = 0; i < input.size(); ++i) input[i] = static_cast<unsigned char>(i * 13 + i / 4 + 7);
            hr = layout.copy(layout.context.get(), true, input.data(), tiled.data() + 16, 0, 512, 8192,nullptr);
            if (SUCCEEDED(hr)) hr = layout.copy(layout.context.get(), false, output.data(), tiled.data() + 16, 0, 512, 8192,nullptr);
            std::cout << "original XG MSAA4 copy hr=0x" << std::hex << hr << std::dec << " equal=" << (input == output) << '\n';
            check(SUCCEEDED(hr) && input == output, "MSAA CPU layout preserves four independent samples without resolve");
            for(UINT repeat=0;repeat<3;++repeat) {
                for(SIZE_T i=0;i<input.size();++i)input[i]^=static_cast<unsigned char>(repeat+i/4+1);
                hr=layout.copy(layout.context.get(),true,input.data(),tiled.data()+16,0,512,8192,nullptr);
                if(SUCCEEDED(hr))hr=layout.copy(layout.context.get(),false,output.data(),tiled.data()+16,0,512,8192,nullptr);
                check(SUCCEEDED(hr) && input==output,"cached MSAA color addresses preserve changed bytes in all four samples");
                TitleTextureLayout fresh;
                auto oracle=create_xg_texture_layout(32,16,1,1,DXGI_FORMAT_R11G11B10_FLOAT,UINT_MAX,fresh,
                    D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE,4);
                std::vector<unsigned char> expected(tiled.size(),0x5a);
                if(SUCCEEDED(oracle))oracle=fresh.copy(fresh.context.get(),true,input.data(),expected.data()+16,0,512,8192,nullptr);
                check(SUCCEEDED(oracle) && tiled==expected,"cached MSAA physical bytes match an uncached original XG address getter");
            }
            const auto unchanged=tiled;
            check(layout.copy(layout.context.get(),true,input.data(),tiled.data()+16,0,511,8192,nullptr)==E_INVALIDARG && tiled==unchanged,
                "cached MSAA color copy rejects a short pitch without modifying canonical bytes");
            check(std::all_of(tiled.begin(), tiled.begin() + 16, [](auto x) { return x == 0x5a; }) &&
                std::all_of(tiled.end() - 16, tiled.end(), [](auto x) { return x == 0x5a; }), "MSAA copies preserve canonical guards");
            hr = create_xg_texture_layout(32, 16, 1, 1, DXGI_FORMAT_R32G8X24_TYPELESS, 1, layout,
                D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE, 4);
            std::cout << "original XG depth/stencil MSAA4 layout hr=0x" << std::hex << hr << std::dec << " bytes=" << layout.bytes << " alignment=" << layout.alignment << '\n';
            check(SUCCEEDED(hr) && layout.context && layout.bytes==98304,
                "three-plane depth/stencil layout retains the original physical allocation");
            if (SUCCEEDED(hr)) {
                std::vector<unsigned char> physical(layout.bytes+32,0x5a), samples(32*16*4*8), restored(samples.size(),0x3c);
                for (SIZE_T at=0;at<samples.size();at+=8) {
                    const UINT32 depth_bits=0x3e000000u+UINT32(at*13);
                    std::memcpy(samples.data()+at,&depth_bits,4);
                    samples[at+4]=static_cast<unsigned char>(at/8*17+3);
                }
                hr=layout.copy(layout.context.get(),true,samples.data(),physical.data()+16,0,1024,16384,nullptr);
                if (SUCCEEDED(hr)) hr=layout.copy(layout.context.get(),false,restored.data(),physical.data()+16,0,1024,16384,nullptr);
                check(SUCCEEDED(hr) && samples==restored,"depth/stencil CPU roundtrip preserves every sample's depth bits and stencil byte");
                for(unsigned repeat=0;repeat<3;++repeat) {
                    for(SIZE_T at=0;at<samples.size();at+=8) {
                        samples[at]^=static_cast<unsigned char>(repeat+1);
                        samples[at+4]^=static_cast<unsigned char>(repeat+17);
                    }
                    hr=layout.copy(layout.context.get(),true,samples.data(),physical.data()+16,0,1024,16384,nullptr);
                    if(SUCCEEDED(hr)) hr=layout.copy(layout.context.get(),false,restored.data(),physical.data()+16,0,1024,16384,nullptr);
                    check(SUCCEEDED(hr) && samples==restored,"cached depth addresses preserve changed depth/stencil data for every MSAA sample");
                }
                check(std::all_of(physical.begin(),physical.begin()+16,[](auto x){return x==0x5a;}) &&
                    std::all_of(physical.end()-16,physical.end(),[](auto x){return x==0x5a;}),"depth/stencil tiling preserves allocation guards");
                physical[16]^=1;
                const auto before=restored;
                check(layout.copy(layout.context.get(),false,restored.data(),physical.data()+16,0,1024,16384,nullptr)==DXGI_ERROR_UNSUPPORTED && restored==before,
                    "unknown HTILE encoding refuses readback without modifying the linear destination");
                const auto tiled_before=physical;
                check(layout.copy(layout.context.get(),true,samples.data(),physical.data()+16,0,1,16384,nullptr)==E_INVALIDARG && physical==tiled_before,
                    "invalid depth/stencil pitch preserves every physical plane");
            }
            for(const auto dimensions:{std::array<UINT,2>{17,9},std::array<UINT,2>{3,5}}) {
                TitleTextureLayout partial;
                const UINT width=dimensions[0],height=dimensions[1],pitch=width*4*8+16;
                hr=create_xg_texture_layout(width,height,1,1,DXGI_FORMAT_R32G8X24_TYPELESS,1,partial,
                    D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE,4);
                check(SUCCEEDED(hr),"depth microtile fixture accepts partial tiles and a small non-tiled table");
                if(FAILED(hr))continue;
                std::vector<BYTE> physical(partial.bytes+32,0x5a),samples(SIZE_T(pitch)*height,0x3c),restored(samples);
                for(UINT repeat=0;repeat<3;++repeat) {
                    for(UINT y=0;y<height;++y)for(UINT x=0;x<width*4;++x) {
                        auto* pixel=samples.data()+SIZE_T(y)*pitch+x*8;
                        const UINT32 depth=0x3e000000u+(y*width*4+x)*17+repeat;
                        std::memcpy(pixel,&depth,4);pixel[4]=BYTE(x+y+repeat);std::memset(pixel+5,0,3);
                    }
                    hr=partial.copy(partial.context.get(),true,samples.data(),physical.data()+16,0,pitch,pitch*height,nullptr);
                    if(SUCCEEDED(hr))hr=partial.copy(partial.context.get(),false,restored.data(),physical.data()+16,0,pitch,pitch*height,nullptr);
                    check(SUCCEEDED(hr) && restored==samples,"depth/stencil microtiles preserve partial rows, all samples and row padding");
                    TitleTextureLayout oracle;
                    hr=create_xg_texture_layout(width,height,1,1,DXGI_FORMAT_R32G8X24_TYPELESS,1,oracle,
                        D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE,4);
                    std::vector<BYTE> expected(physical.size(),0x5a);
                    if(SUCCEEDED(hr))hr=oracle.copy(oracle.context.get(),true,samples.data(),expected.data()+16,0,pitch,pitch*height,nullptr);
                    check(SUCCEEDED(hr) && physical==expected,"microtile output and guards match every uncached original XG address");
                }
            }
            hr = create_xg_texture_layout(1920, 1080, 1, 1, DXGI_FORMAT_R32G8X24_TYPELESS, 1, layout,
                D3D11_BIND_DEPTH_STENCIL | D3D11_BIND_SHADER_RESOURCE, 4);
            check(SUCCEEDED(hr) && layout.context && layout.bytes==41910272,
                "observed full-size FH2 descriptor preserves the exact ESRAM and DDR allocation span");
            using CreateComputer = HRESULT(*)(const void*,void**);
            const auto create_computer = reinterpret_cast<CreateComputer>(resolve_xg_module({"xg_x.dll","XGCreateTexture2DComputer",0}));
            check(create_computer != nullptr,"revision-bound XG exposes its original depth layout factory");
            if (!create_computer) return 1;
            for (const auto dimensions : {std::array<UINT,2>{32,16},std::array<UINT,2>{1920,1080}}) {
                std::array<UINT,15> descriptor{dimensions[0],dimensions[1],1,1,19,4,0,0,0x40,0,0,0,0,1,0};
                void* computer = nullptr;
                hr = create_computer(descriptor.data(),&computer);
                check(SUCCEEDED(hr) && computer,"original XG constructs the observed depth/stencil descriptor");
                if (FAILED(hr) || !computer) continue;
                auto** table = *static_cast<void***>(computer);
                std::array<unsigned char,0xd40+16> raw;
                raw.fill(0x5a);
                hr = reinterpret_cast<HRESULT(*)(void*,void*)>(table[2])(computer,raw.data());
                const auto u64 = [&](SIZE_T offset) { UINT64 value; std::memcpy(&value,raw.data()+offset,8); return value; };
                const auto u32 = [&](SIZE_T offset) { UINT value; std::memcpy(&value,raw.data()+offset,4); return value; };
                const UINT64 bytes = u64(0);
                check(SUCCEEDED(hr) && u32(16)==1 && u32(20)==3 && bytes==(dimensions[0]==32 ? 98304ull : 41910272ull),
                    "original depth layout has verified size and three physical planes");
                check(std::all_of(raw.begin()+0xd40,raw.end(),[](auto x){return x==0x5a;}),"depth layout preserves ABI output bounds");
                std::cout << "original depth " << dimensions[0] << 'x' << dimensions[1] << " bytes=" << bytes << '\n';
                if (dimensions[0]==32) {
                    std::array<UINT32,8> words{};
                    std::memcpy(words.data(),static_cast<char*>(computer)+0x10,sizeof words);
                    check(std::all_of(words.begin(),words.end(),[](auto word){return !word;}),
                        "original XG depth computer does not publish a ready-made shader SRD");
                }
                // Reference helpers 140167760/1401677d0 establish the 11295 inline plane/mip strides.
                for (UINT plane=0;plane<3;++plane) {
                    const SIZE_T offset=24+plane*0x460;
                    const UINT usage=u32(offset), element=u32(offset+32);
                    const UINT64 size=u64(offset+8), base=u64(offset+16);
                    std::cout << " plane=" << plane << " usage=" << usage << " bytes=" << size << " base=" << base
                              << " alignment=" << u64(offset+24) << " element=" << element << '\n';
                    check(base<=bytes && size<=bytes-base,"depth plane fits the original allocation");
                    check(usage==(plane==0 ? 4u : plane==1 ? 7u : 8u),"physical plane order matches the original HTILE/depth/stencil layout");
                    if (dimensions[0]!=32 || usage==4) continue;
                    check((usage==7 || usage==8) && element==(usage==7 ? 4u : 1u),
                        "depth and stencil have independent verified element widths");
                    if ((usage!=7 && usage!=8) || element!=(usage==7 ? 4u : 1u)) continue;
                    using Address=UINT64(*)(void*,UINT,UINT,UINT64,UINT,UINT,UINT);
                    const auto address=reinterpret_cast<Address>(table[4]);
                    auto* plane_context=static_cast<unsigned char*>(computer)+0xd8+plane*0x1c18;
                    using PhysicalAddress=INT64(*)(UINT64,UINT,UINT,UINT,UINT64,UINT64,const void*);
                    const auto physical=reinterpret_cast<PhysicalAddress>(static_cast<unsigned char*>(table[4])+
                        ((plane_context[0x68]&0x80) ? 0xb440 : 0x88d0)-0x2a70);
                    auto typed_descriptor=descriptor;
                    typed_descriptor[4]=usage==7 ? 41 : 62; typed_descriptor[8]=0;
                    void* typed=nullptr;
                    const HRESULT typed_hr=create_computer(typed_descriptor.data(),&typed);
                    bool same_typed=SUCCEEDED(typed_hr) && typed;
                    void** typed_table=same_typed ? *static_cast<void***>(typed) : nullptr;
                    const auto typed_address=typed_table ? reinterpret_cast<Address>(typed_table[4]) : nullptr;
                    std::vector<unsigned char> touched(static_cast<SIZE_T>(bytes));
                    bool valid=true;
                    for (UINT y=0;y<16;++y) for (UINT x=0;x<32;++x) for (UINT sample=0;sample<4;++sample) {
                        const INT64 bits=physical(x,y,0,sample,0,0,plane_context);
                        const UINT64 at=static_cast<UINT64>(bits>>12);
                        if (!sample && at!=address(computer,plane,0,x,y,0,0)) valid=false;
                        if (sample && address(computer,plane,0,x,y,0,sample)!=UINT64_MAX) valid=false;
                        if (typed_address && typed_address(typed,0,0,x,y,0,sample)+base!=at) same_typed=false;
                        if (at<base || at>base+size || element>base+size-at) { valid=false; continue; }
                        for (UINT byte=0;byte<element;++byte) {
                            if (touched[static_cast<SIZE_T>(at+byte)]++) valid=false;
                        }
                    }
                    check(valid,"every depth/stencil sample has a distinct in-plane byte address");
                    check(same_typed,"typed XG computer preserves the exact depth/stencil address of every sample");
                    if (typed_table) reinterpret_cast<ULONG(*)(void*)>(typed_table[1])(typed);
                }
                reinterpret_cast<ULONG(*)(void*)>(table[1])(computer);
            }
            hr=create_xg_texture_layout(1920,1080,1,1,DXGI_FORMAT_R11G11B10_FLOAT,UINT_MAX,layout,
                D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS);
            std::cout << "original FH2 ESRAM color layout hr=0x" << std::hex << hr << std::dec
                      << " bytes=" << layout.bytes << " alignment=" << layout.alignment << '\n';
            check(SUCCEEDED(hr),"observed ESRAM color descriptor has an original XG expanded layout");
            using OptimalDepth=void(*)(UINT,UINT,UINT,UINT,UINT,UINT,UINT*,UINT*);
            const auto depth_modes=reinterpret_cast<OptimalDepth>(resolve_xg_module({"xg_x.dll","XGComputeOptimalDepthStencilTileModes",0}));
            check(depth_modes!=nullptr,"original XG exposes its separate depth/stencil tile selector");
            if (!depth_modes) return 1;
            for (const UINT samples:{1u,2u,4u,8u}) {
                struct { UINT depth=UINT_MAX,stencil=UINT_MAX; UINT64 guard=0x1122334455667788ull; } result;
                depth_modes(0,0,0,0,samples,0,&result.depth,&result.stencil);
                const UINT expected=samples==1 ? 0 : samples==8 ? 2 : 1;
                check(result.depth==expected && result.stencil==expected && result.guard==0x1122334455667788ull,
                    "original depth tile modes preserve scalar argument positions and output bounds");
            }
            depth_modes(0,0,0,0,1,0,nullptr,nullptr);
            for (const auto format:{DXGI_FORMAT_R32_TYPELESS,DXGI_FORMAT_R16_TYPELESS}) for (const auto dimensions:{std::array<UINT,2>{32,16},std::array<UINT,2>{1920,1080}}) {
                const UINT element=format==DXGI_FORMAT_R16_TYPELESS ? 2 : 4, row=32*element, slice=row*16;
                std::array<UINT,15> descriptor{dimensions[0],dimensions[1],1,1,UINT(format),1,0,0,0x40,0,0,0,0,0,0};
                void* computer=nullptr;
                hr=create_computer(descriptor.data(),&computer);
                check(SUCCEEDED(hr) && computer,"original XG constructs the observed scalar depth descriptor");
                if (!computer) continue;
                auto** table=*static_cast<void***>(computer);
                std::array<unsigned char,0xd40> raw{};
                hr=reinterpret_cast<HRESULT(*)(void*,void*)>(table[2])(computer,raw.data());
                UINT64 bytes=0; UINT planes=0;
                std::memcpy(&bytes,raw.data(),8);std::memcpy(&planes,raw.data()+20,4);
                std::cout << "original depth format=" << format << ' ' << dimensions[0] << 'x' << dimensions[1] << " bytes=" << bytes << " planes=" << planes << '\n';
                check(SUCCEEDED(hr) && planes==2,"scalar depth has separate HTILE and physical depth planes");
                hr=create_xg_texture_layout(dimensions[0],dimensions[1],1,1,format,UINT_MAX,layout,
                    D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE);
                check(SUCCEEDED(hr) && layout.bytes==bytes,"scalar depth adapter preserves the exact original physical depth allocation");
                if (SUCCEEDED(hr) && dimensions[0]==1920) {
                    // Linear P4_16x16 HTILE (UMD 18009de20 LINEAR rule, XG pipe config): hand-computed addrlib words.
                    const UINT width=1920,height=1080,pitch=width*element,image=pitch*height;
                    std::vector<unsigned char> base(image),actual(image);
                    for (SIZE_T i=0;i<base.size();++i) base[i]=static_cast<unsigned char>((i*7+i/4099)|1); // never a clear value
                    if (element==4) for (SIZE_T i=0;i<base.size();i+=4) {const UINT32 v=0x3f000000u|UINT32(i&0x7fffff);std::memcpy(base.data()+i,&v,4);}
                    std::vector<unsigned char> physical(layout.bytes,0);
                    hr=layout.copy(layout.context.get(),true,base.data(),physical.data(),0,pitch,image,nullptr);
                    check(SUCCEEDED(hr),"scalar depth stores a 1920x1080 base plane with expanded HTILE");
                    UINT64 metadata_bytes=0,metadata_offset=0;
                    std::memcpy(&metadata_bytes,raw.data()+32,8);std::memcpy(&metadata_offset,raw.data()+40,8);
                    check(metadata_bytes==131072,"FH2 depth HTILE matches the UMD linear size formula");
                    auto* metadata=physical.data()+metadata_offset;
                    const auto tile_ok=[&](UINT tx,UINT ty,bool kept,UINT32 clear_bits) {
                        for (UINT y=ty*8;y<ty*8+8 && y<height;++y) for (UINT x=tx*8;x<tx*8+8;++x) {
                            const SIZE_T at=SIZE_T(y)*pitch+SIZE_T(x)*element;
                            if (kept ? std::memcmp(actual.data()+at,base.data()+at,element)!=0 :
                                (element==4 ? std::memcmp(actual.data()+at,&clear_bits,4)!=0 :
                                 (actual[at]!=(clear_bits ? 0xff : 0) || actual[at+1]!=(clear_bits ? 0xff : 0)))) return false;
                        }
                        return true;
                    };
                    for (const UINT32 clear_bits:{0u,0x3f800000u}) {
                        const UINT32 tag=clear_bits ? 0xfffffff0u : 0u, expanded=0xfffc000fu;
                        for (SIZE_T at=0;at<metadata_bytes;at+=4) std::memcpy(metadata+at,&tag,4);
                        for (const UINT word:{195u,1841u}) std::memcpy(metadata+SIZE_T(word)*4,&expanded,4); // tiles (2,0), (17,9)
                        TitleGpuClearState clear{clear_bits,0,1};
                        hr=layout.copy(layout.context.get(),false,actual.data(),physical.data(),0,pitch,image,&clear);
                        check(SUCCEEDED(hr) && tile_ok(2,0,true,clear_bits) && tile_ok(17,9,true,clear_bits) && tile_ok(0,0,false,clear_bits) &&
                            tile_ok(1,0,false,clear_bits) && tile_ok(3,1,false,clear_bits) && tile_ok(239,134,false,clear_bits),
                            "mixed fast-clear HTILE decodes per tile: expanded tiles keep the base plane, ZMask-0 tiles take the proven clear");
                        const auto before=actual;
                        clear.known=0;
                        check(layout.copy(layout.context.get(),false,actual.data(),physical.data(),0,pitch,image,&clear)==DXGI_ERROR_UNSUPPORTED && actual==before,
                            "mixed HTILE without a proven clear value is refused without partial output");
                        clear.known=1;const UINT32 compressed=0x12345670u;std::memcpy(metadata+64*4,&compressed,4);
                        check(layout.copy(layout.context.get(),false,actual.data(),physical.data(),0,pitch,image,&clear)==DXGI_ERROR_UNSUPPORTED && actual==before,
                            "other compressed HTILE states remain refused");
                    }
                }
                if (SUCCEEDED(hr) && dimensions[0]==32) {
                    std::vector<unsigned char> expected(slice),actual(expected.size());
                    for (UINT i=0;i<expected.size();++i) expected[i]=static_cast<unsigned char>(i*13+i/7);
                    std::vector<unsigned char> physical(layout.bytes+32,0x5a);
                    hr=layout.copy(layout.context.get(),true,expected.data(),physical.data()+16,0,row,slice,nullptr);
                    using Address=UINT64(*)(void*,UINT,UINT,UINT64,UINT,UINT,UINT);
                    const auto address=reinterpret_cast<Address>(table[4]);
                    bool exact=SUCCEEDED(hr);
                    for (UINT y=0;y<16;++y) for (UINT x=0;x<32;++x) {
                        const auto at=address(computer,1,0,x,y,0,0);
                        if (at>bytes || bytes-at<element) { exact=false;continue; }
                        if (std::memcmp(physical.data()+16+at,expected.data()+(y*32+x)*element,element)) exact=false;
                    }
                    check(exact,"scalar depth adapter matches the independent original depth computer at every pixel address");
                    if (SUCCEEDED(hr)) hr=layout.copy(layout.context.get(),false,actual.data(),physical.data()+16,0,row,slice,nullptr);
                    check(SUCCEEDED(hr) && actual==expected,"single-sample scalar depth preserves every physical bit through expanded HTILE");
                    physical[16]^=1; const auto before=actual;
                    check(layout.copy(layout.context.get(),false,actual.data(),physical.data()+16,0,row,slice,nullptr)==DXGI_ERROR_UNSUPPORTED && actual==before,
                        "single-sample depth refuses unknown HTILE without publishing partial pixels");
                    {
                        TitleGpuClearState sampled{};sampled.bypass_htile=true;
                        std::vector<unsigned char> raw_read(expected.size(),0xcc);
                        check(layout.copy(layout.context.get(),false,raw_read.data(),physical.data()+16,0,row,slice,&sampled)==S_OK && raw_read==expected,
                            "shader-only depth read ignores unknown HTILE and returns the exact base plane, as the GCN 2 texture unit does");
                        const auto canonical=physical;
                        check(physical==canonical && layout.copy(layout.context.get(),false,raw_read.data(),physical.data()+16,0,row,slice,nullptr)==DXGI_ERROR_UNSUPPORTED,
                            "the depth-block path keeps refusing the same unknown HTILE");
                    }
                    if(format==DXGI_FORMAT_R32_TYPELESS) {
                        UINT64 metadata_bytes=0,metadata_offset=0;
                        std::memcpy(&metadata_bytes,raw.data()+32,8);std::memcpy(&metadata_offset,raw.data()+40,8);
                        auto* metadata=physical.data()+16+metadata_offset;
                        std::memset(metadata,0,SIZE_T(metadata_bytes));
                        const auto compressed=physical;
                        TitleGpuClearState clear{0,0,1};
                        check(layout.copy(layout.context.get(),false,actual.data(),physical.data()+16,0,row,slice,&clear)==S_OK &&
                            std::all_of(actual.begin(),actual.end(),[](auto byte){return byte==0;}) && physical==compressed,
                            "uniform fast-clear zero uses explicit full clear state, ignores stale base pixels and preserves canonical bytes");
                        const auto zero=actual;
                        clear.known=0;
                        check(layout.copy(layout.context.get(),false,actual.data(),physical.data()+16,0,row,slice,&clear)==DXGI_ERROR_UNSUPPORTED && actual==zero,
                            "HTILE zero without known clear state cannot fabricate zero depth");
                        clear.known=1;clear.depth=0x358637bdu;
                        check(layout.copy(layout.context.get(),false,actual.data(),physical.data()+16,0,row,slice,&clear)==DXGI_ERROR_UNSUPPORTED && actual==zero,
                            "quantized zero bounds cannot replace an unsupported nonzero full-precision clear value");
                        clear.depth=0x3f800000;
                        check(layout.copy(layout.context.get(),false,actual.data(),physical.data()+16,0,row,slice,&clear)==DXGI_ERROR_UNSUPPORTED && actual==zero,
                            "mismatched clear register and HTILE encoding reject without partial output");
                        const UINT32 one_tag=0xfffffff0;
                        for(SIZE_T at=0;at<metadata_bytes;at+=4) std::memcpy(metadata+at,&one_tag,4);
                        hr=layout.copy(layout.context.get(),false,actual.data(),physical.data()+16,0,row,slice,&clear);
                        bool one=SUCCEEDED(hr);
                        for(SIZE_T at=0;at<actual.size();at+=4) {UINT32 bits=0;std::memcpy(&bits,actual.data()+at,4);one=one && bits==0x3f800000;}
                        check(one,"uniform fast-clear one restores the exact full-precision clear bits at every original sample address");
                        const auto saved=actual;metadata[metadata_bytes-4]^=1;
                        check(layout.copy(layout.context.get(),false,actual.data(),physical.data()+16,0,row,slice,&clear)==DXGI_ERROR_UNSUPPORTED && actual==saved,
                            "mixed or one-bit-near fast-clear metadata is never accepted as a whole-surface clear");
                    }
                    check(std::all_of(physical.begin(),physical.begin()+16,[](auto x){return x==0x5a;}) &&
                        std::all_of(physical.end()-16,physical.end(),[](auto x){return x==0x5a;}),"scalar depth conversion preserves canonical guards");
                }
                reinterpret_cast<ULONG(*)(void*)>(table[1])(computer);
            }
            return failures ? 1 : 0;
        }
        if (SUCCEEDED(hr)) hr = create_xg_texture_layout(64, 64, 7, 1, DXGI_FORMAT_BC7_UNORM_SRGB, 14, layout);
        std::cout << "original XG BC7 layout hr=0x" << std::hex << hr << std::dec << " bytes=" << layout.bytes << " alignment=" << layout.alignment << '\n';
        if (FAILED(hr)) return 1;
        std::vector<std::uint8_t> tiled(layout.bytes + 32, 0x5a);
        std::array<std::vector<std::uint8_t>, 7> linear;
        for (UINT mip = 0; mip < 7; ++mip) {
            const UINT blocks = ((std::max)(1u, 64u >> mip) + 3) / 4;
            linear[mip].resize(blocks * blocks * 16);
            for (UINT i = 0; i < linear[mip].size(); ++i) linear[mip][i] = static_cast<std::uint8_t>(mip * 31 + i * 13 + 7);
            hr = layout.copy(layout.context.get(), true, linear[mip].data(), tiled.data() + 16, mip, blocks * 16, blocks * blocks * 16,nullptr);
            check(SUCCEEDED(hr), "original XG tiles an owned BC7 mip");
        }
        for (UINT mip = 0; mip < 7; ++mip) {
            const UINT blocks = ((std::max)(1u, 64u >> mip) + 3) / 4;
            std::vector<std::uint8_t> actual(linear[mip].size(), 0);
            hr = layout.copy(layout.context.get(), false, actual.data(), tiled.data() + 16, mip, blocks * 16, blocks * blocks * 16,nullptr);
            check(SUCCEEDED(hr) && actual == linear[mip], "original XG roundtrip preserves each owned mip's compressed blocks");
        }
        check(std::all_of(tiled.begin(), tiled.begin() + 16, [](auto x) { return x == 0x5a; }) &&
            std::all_of(tiled.end() - 16, tiled.end(), [](auto x) { return x == 0x5a; }), "original XG copies preserve canonical range guards");
        check(layout.copy(layout.context.get(), false, linear[0].data(), tiled.data() + 16, 7, 256, 4096,nullptr) == E_INVALIDARG &&
            layout.copy(layout.context.get(), false, linear[0].data(), tiled.data() + 16, 0, 1, 4096,nullptr) == E_INVALIDARG,
            "layout adapter rejects invalid subresources and undersized linear pitches");
        std::cout << "original XG CPU roundtrip " << (failures ? "FAILED" : "passed") << '\n';
        if (graphics && !failures) {
            set_stop_on_missing_activation(false);
            struct Parameters { UINT version, flags; void* tess; void* factors; UINT affinity, de_ring, ce_ring, de_segment, ce_segment; } parameters{};
            parameters.version = 0x1000b; parameters.flags = 0x4000000;
            using CreateDevice = HRESULT (WINAPI*)(const Parameters*, void**, void**);
            const auto create_device = reinterpret_cast<CreateDevice>(resolve_d3d11x({"d3d11_x.dll", "D3D11XCreateDeviceX", 0}));
            void* device = nullptr; void* context = nullptr;
            if (!create_device || FAILED(create_device(&parameters, &device, &context))) return 1;
            using PresentArray=HRESULT(WINAPI*)(UINT,UINT,UINT,UINT,void* const*,const void*);
            const auto present_array=reinterpret_cast<PresentArray>(resolve_d3d11x({"d3d11_x.dll","DXGIXPresentArray",0}));
            check(present_array!=nullptr,"legacy PresentArray export is bound to the presentation runtime");
            if(present_array) {
                struct PresentParameters {UINT reserved[2]{};RECT source{};POINT destination{};FLOAT scale[2]{1,1};UINT64 cookie=0;UINT flags=0,tail=0;} p;
                static_assert(sizeof p==56);
                void* chain=nullptr;
                check(present_array(2,20,0,0,&chain,&p)==E_INVALIDARG && present_array(5,20,0,1,&chain,&p)==E_INVALIDARG,
                      "PresentArray rejects invalid counts and intervals before touching a swapchain");
                check(present_array(2,21,0,1,&chain,&p)==DXGI_ERROR_UNSUPPORTED &&
                      present_array(2,20,1,1,&chain,&p)==DXGI_ERROR_UNSUPPORTED && present_array(2,20,0,2,&chain,&p)==DXGI_ERROR_UNSUPPORTED,
                      "unknown presentation profiles, flags and multi-plane composition remain explicit");
                p.cookie=1;
                check(present_array(2,20,0,1,&chain,&p)==E_INVALIDARG,"opaque frame cookie is accepted before validating the null chain");
                p.cookie=UINT64_MAX;
                check(present_array(2,20,0,1,&chain,&p)==E_INVALIDARG,"frame cookie is never dereferenced or interpreted as a synchronization handle");
                p.cookie=0;p.scale[0]=2;
                check(present_array(2,20,0,1,&chain,&p)==DXGI_ERROR_UNSUPPORTED,"scaled presentation cannot silently become identity composition");
                p.scale[0]=1;
                check(present_array(2,20,0,1,&chain,reinterpret_cast<void*>(1))==E_INVALIDARG &&
                      present_array(2,20,0,1,&chain,&p)==E_INVALIDARG,"unreadable presentation data and null chains fail without presentation");
            }
            const auto vt = [](void* object) { return *static_cast<void***>(object); };
            using Count = ULONG (STDMETHODCALLTYPE*)(void*);
            using Allocate = void* (WINAPI*)(SIZE_T, UINT64);
            using Free = void (WINAPI*)(void*, UINT64);
            const auto allocate = reinterpret_cast<Allocate>(resolve_kernelx({"kernelx.dll", "XMemAllocDefault", 0}));
            const auto free_memory = reinterpret_cast<Free>(resolve_kernelx({"kernelx.dll", "XMemFreeDefault", 0}));
            constexpr UINT64 attributes = 0x64804000;
            void* root = allocate(layout.bytes + layout.alignment, attributes);
            if (!root) return 1;
            const auto aligned = (reinterpret_cast<std::uintptr_t>(root) + layout.alignment - 1) & ~(layout.alignment - 1);
            void* canonical = reinterpret_cast<void*>(aligned);
            struct Desc { D3D11_TEXTURE2D_DESC standard{}; UINT extension[2]{}; } desc;
            desc.standard = {64, 64, 7, 1, DXGI_FORMAT_BC7_UNORM_SRGB, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0};
            using CreateTexture = HRESULT (STDMETHODCALLTYPE*)(void*, const Desc*, UINT, UINT, void*, void**);
            void* texture = nullptr;
            hr = reinterpret_cast<CreateTexture>(vt(device)[64])(device, &desc, 14, 0, canonical, &texture);
            check(hr == S_OK && texture, "placed texture factory binds original XG storage");
            if (texture) {
                using GetDesc = void (STDMETHODCALLTYPE*)(void*, Desc*);
                struct { Desc desc{}; UINT guard = 0xaabbccdd; } actual;
                reinterpret_cast<GetDesc>(vt(texture)[12])(texture, &actual.desc);
                check(actual.desc.standard.MipLevels == 7 && actual.desc.standard.Format == desc.standard.Format &&
                    actual.guard == 0xaabbccdd, "native texture GetDesc preserves descriptor and fifty-two-byte extent");
                using CreateSrv = HRESULT (STDMETHODCALLTYPE*)(void*, void*, const D3D11_SHADER_RESOURCE_VIEW_DESC*, void**);
                const auto create_srv = reinterpret_cast<CreateSrv>(vt(device)[7]);
                void* view = nullptr;
                D3D11_SHADER_RESOURCE_VIEW_DESC partial{};
                partial.Format = desc.standard.Format; partial.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
                partial.Texture2D = {1, 6};
                check(create_srv(device, texture, &partial, &view) == S_OK && view,
                      "partial texture views translate the selected mip range");
                if(view) {
                    std::array<UINT32,8> range{};std::memcpy(range.data(),static_cast<char*>(view)+48,sizeof range);
                    check(((range[3]>>12)&15)==1 && ((range[3]>>16)&15)==6,"partial SRD preserves the first and last selected mip");
                    reinterpret_cast<Count>(vt(view)[2])(view);view=nullptr;
                }
                partial.Texture2D={7,1};
                check(create_srv(device,texture,&partial,&view)==E_INVALIDARG && !view,"partial texture view rejects an out-of-bounds base mip");
                check(create_srv(device, texture, nullptr, nullptr) == S_FALSE, "default texture view validates without ownership changes");
                check(create_srv(device, texture, nullptr, &view) == S_OK && view, "default texture SRV owns a native descriptor");
                if (view) {
                    std::array<UINT32, 8> srd{};
                    std::memcpy(srd.data(), static_cast<char*>(view) + 48, sizeof srd);
                    check(srd[0] == (aligned >> 8) && ((srd[3] >> 12) & 15) == 0 && ((srd[3] >> 16) & 15) == 6,
                          "guest texture SRV preserves placement address and complete mip range");
                    using GetSrvDesc = void (STDMETHODCALLTYPE*)(void*, D3D11_SHADER_RESOURCE_VIEW_DESC*);
                    struct { D3D11_SHADER_RESOURCE_VIEW_DESC desc{}; UINT guard = 0xaabbccdd; } view_desc;
                    reinterpret_cast<GetSrvDesc>(vt(view)[9])(view, &view_desc.desc);
                    check(view_desc.desc.Format == desc.standard.Format && view_desc.desc.Texture2D.MipLevels == 7 &&
                        view_desc.guard == 0xaabbccdd, "texture view GetDesc returns the effective range and output extent");
                    check(reinterpret_cast<Count>(vt(texture)[2])(texture) == 1, "view retains texture after its caller releases it");
                    texture = nullptr;
                    reinterpret_cast<Count>(vt(view)[2])(view);
                }
                if (texture) reinterpret_cast<Count>(vt(texture)[2])(texture);
            }
            MEMORY_BASIC_INFORMATION memory{};
            VirtualQuery(root, &memory, sizeof memory);
            check(memory.State == MEM_COMMIT, "placed texture release preserves borrowed canonical storage");
            free_memory(root, attributes);
            {
                std::array<BYTE,64> blocks{},guards;guards.fill(0x5a);for(UINT i=0;i<64;++i)blocks[i]=BYTE(i*17+11);
                Desc compressed{};compressed.standard={8,8,1,1,DXGI_FORMAT_BC7_UNORM_SRGB,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_SHADER_RESOURCE,0,0};
                using Create=HRESULT(STDMETHODCALLTYPE*)(void*,const Desc*,const D3D11_SUBRESOURCE_DATA*,void**);
                void* objects[2]{};void* view=nullptr;
                for(UINT i=0;i<2;++i) {
                    if(i)compressed.standard.Format=DXGI_FORMAT_BC7_UNORM;
                    const D3D11_SUBRESOURCE_DATA data{i ? guards.data():blocks.data(),32,64};
                    check(reinterpret_cast<Create>(vt(device)[5])(device,&compressed,&data,&objects[i])==S_OK && objects[i],"compressed region copy creates two textures with independent BC7 block contents");
                }
                if(!objects[0] || !objects[1])return 1;
                using Srv=HRESULT(STDMETHODCALLTYPE*)(void*,void*,const D3D11_SHADER_RESOURCE_VIEW_DESC*,void**);
                check(reinterpret_cast<Srv>(vt(device)[7])(device,objects[1],nullptr,&view)==S_OK && view,"compressed region destination retains its canonical storage");
                if(!view)return 1;
                void* canonical=nullptr;std::memcpy(&canonical,static_cast<BYTE*>(view)+80,sizeof canonical);
                TitleTextureLayout pixels;check(create_xg_texture_layout(8,8,1,1,DXGI_FORMAT_BC7_UNORM,UINT_MAX,pixels,D3D11_BIND_SHADER_RESOURCE)==S_OK,"compressed region oracle uses the original block layout");
                using Region=void(STDMETHODCALLTYPE*)(void*,void*,UINT,UINT,UINT,UINT,void*,UINT,const D3D11_BOX*);
                const auto copy=reinterpret_cast<Region>(vt(context)[46]);D3D11_BOX box{0,4,0,4,8,1};
                copy(context,objects[1],0,4,0,0,objects[0],0,&box);auto expected=guards;std::memcpy(expected.data()+16,blocks.data()+32,16);
                std::array<BYTE,64> actual{};
                check(pixels.copy(pixels.context.get(),false,actual.data(),canonical,0,32,64,nullptr)==S_OK && actual==expected,"BC7 region copy preserves raw blocks across sRGB/UNORM siblings without decompressing or changing untouched blocks");
                static UINT refused=0;set_d3d11x_trap_handler([](const char*,UINT slot)->std::uintptr_t{refused=slot;return 0;});
                for(UINT invalid=0;invalid<3;++invalid) {
                    box={0,4,0,4,8,1};UINT x=4;if(invalid==0)box.left=1;if(invalid==1)x=1;if(invalid==2)box.bottom=7;
                    refused=0;copy(context,objects[1],0,x,0,0,objects[0],0,&box);
                    check(refused==46 && pixels.copy(pixels.context.get(),false,actual.data(),canonical,0,32,64,nullptr)==S_OK && actual==expected,"misaligned BC7 source edges and destination offsets are rejected without changing block contents");
                }
                set_d3d11x_trap_handler(nullptr);
                reinterpret_cast<Count>(vt(view)[2])(view);for(auto* object:objects)reinterpret_cast<Count>(vt(object)[2])(object);
            }
            Desc placed_depth{};
            placed_depth.standard={32,16,1,1,DXGI_FORMAT_R32G8X24_TYPELESS,{4,0},D3D11_USAGE_DEFAULT,
                D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE,0,0x40000};
            auto* depth_storage=static_cast<unsigned char*>(allocate(98304+8192+32,attributes));
            if (!depth_storage) return 1;
            std::memset(depth_storage,0x5a,98304+8192+32);
            void* depth_base=reinterpret_cast<void*>((reinterpret_cast<std::uintptr_t>(depth_storage)+16+8191)&~std::uintptr_t(8191));
            texture=nullptr;
            const auto place_texture=reinterpret_cast<CreateTexture>(vt(device)[64]);
            check(place_texture(device,&placed_depth,1,1,depth_base,&texture)==DXGI_ERROR_UNSUPPORTED && !texture,
                "depth placement rejects unknown flags before acquiring the original storage");
            check(place_texture(device,&placed_depth,1,0,depth_base,&texture)==S_OK && texture,
                "depth placement binds the original three-plane allocation to native MSAA depth/stencil");
            if (texture) {
                using CreateDsv=HRESULT(STDMETHODCALLTYPE*)(void*,void*,const D3D11_DEPTH_STENCIL_VIEW_DESC*,void**);
                using CreateSrv=HRESULT(STDMETHODCALLTYPE*)(void*,void*,const D3D11_SHADER_RESOURCE_VIEW_DESC*,void**);
                const auto create_srv=reinterpret_cast<CreateSrv>(vt(device)[7]);
                for (const auto format:{DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS,DXGI_FORMAT_X32_TYPELESS_G8X24_UINT}) {
                    D3D11_SHADER_RESOURCE_VIEW_DESC description{};
                    description.Format=format; description.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2DMS;
                    void* view=nullptr;
                    check(create_srv(device,texture,&description,&view)==S_OK && view,"depth/stencil SRV binds a typed view of the same native resource");
                    if (!view) continue;
                    std::array<UINT32,8> descriptor{};
                    std::memcpy(descriptor.data(),static_cast<char*>(view)+48,sizeof descriptor);
                    const UINT64 plane_offset=format==DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS ? 16384 : 81920;
                    check(descriptor[0]==((reinterpret_cast<std::uintptr_t>(depth_base)+plane_offset)>>8) &&
                        ((descriptor[3]>>16)&15)==2 && (descriptor[3]>>28)>=14,
                        "depth/stencil SRD preserves its physical plane base and four-sample address contract");
                    struct { D3D11_SHADER_RESOURCE_VIEW_DESC value{}; UINT guard=0xaabbccdd; } actual;
                    reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,D3D11_SHADER_RESOURCE_VIEW_DESC*)>(vt(view)[9])(view,&actual.value);
                    check(actual.value.Format==format && actual.value.ViewDimension==description.ViewDimension && actual.guard==0xaabbccdd,
                        "typed MSAA SRV queries preserve format, dimension and output extent");
                    reinterpret_cast<Count>(vt(view)[2])(view);
                }
                const auto create_dsv=reinterpret_cast<CreateDsv>(vt(device)[10]);
                D3D11_DEPTH_STENCIL_VIEW_DESC depth_view{};
                depth_view.Format=DXGI_FORMAT_D32_FLOAT; depth_view.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2DMS;
                void* view=nullptr;
                check(create_dsv(device,texture,&depth_view,&view)==DXGI_ERROR_UNSUPPORTED && !view,
                    "combined depth/stencil rejects a depth-only reinterpretation");
                depth_view.Format=DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
                depth_view.Flags=D3D11_DSV_READ_ONLY_DEPTH|D3D11_DSV_READ_ONLY_STENCIL;
                check(create_dsv(device,texture,&depth_view,nullptr)==S_FALSE,
                    "read-only depth/stencil validation does not create ownership");
                check(create_dsv(device,texture,&depth_view,&view)==S_OK && view,
                    "original DSV factory creates a typed native four-sample view");
                if (view) {
                    struct { D3D11_DEPTH_STENCIL_VIEW_DESC desc{}; UINT guard=0xaabbccdd; } actual_view;
                    reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,D3D11_DEPTH_STENCIL_VIEW_DESC*)>(vt(view)[9])(view,&actual_view.desc);
                    check(!std::memcmp(&actual_view.desc,&depth_view,sizeof depth_view) && actual_view.guard==0xaabbccdd,
                        "MSAA DSV description preserves both read-only aspects and its ABI extent");
                    using SetDepthTarget=void(STDMETHODCALLTYPE*)(void*,UINT,void* const*,void*);
                    using GetDepthTarget=void(STDMETHODCALLTYPE*)(void*,UINT,void**,void**);
                    reinterpret_cast<SetDepthTarget>(vt(context)[33])(context,0,nullptr,view);
                    void* retained_depth=nullptr;
                    reinterpret_cast<GetDepthTarget>(vt(context)[89])(context,0,nullptr,&retained_depth);
                    check(retained_depth==view,"legacy OM depth-only query returns the bound MSAA DSV");
                    reinterpret_cast<SetDepthTarget>(vt(context)[33])(context,0,nullptr,nullptr);
                    check(reinterpret_cast<Count>(vt(view)[1])(view)==3,"queried DSV survives unbinding with independent caller ownership");
                    reinterpret_cast<Count>(vt(view)[2])(view);
                    if(retained_depth)reinterpret_cast<Count>(vt(retained_depth)[2])(retained_depth);
                    check(reinterpret_cast<Count>(vt(texture)[2])(texture)==1,"MSAA DSV retains its original parent storage");
                    texture=nullptr;
                    reinterpret_cast<Count>(vt(view)[2])(view);
                }
                if (texture) reinterpret_cast<Count>(vt(texture)[2])(texture);
            }
            check(std::all_of(depth_storage,depth_storage+98304+8192+32,[](auto x){return x==0x5a;}),
                "depth placement and view retirement preserve every borrowed physical byte");
            free_memory(depth_storage,attributes);
            auto* clear_storage=static_cast<unsigned char*>(allocate(98304+8192+32,attributes));
            if(!clear_storage) return 1;
            std::memset(clear_storage,0x5a,98304+8192+32);
            auto* clear_base=reinterpret_cast<unsigned char*>((reinterpret_cast<std::uintptr_t>(clear_storage)+16+8191)&~std::uintptr_t(8191));
            void* clear_texture=nullptr;void* clear_dsv=nullptr;
            check(place_texture(device,&placed_depth,1,0,clear_base,&clear_texture)==S_OK && clear_texture,"MSAA clear fixture retains guarded borrowed depth/stencil storage");
            if(clear_texture) {
                using MakeDsv=HRESULT(STDMETHODCALLTYPE*)(void*,void*,const D3D11_DEPTH_STENCIL_VIEW_DESC*,void**);
                D3D11_DEPTH_STENCIL_VIEW_DESC desc{};desc.Format=DXGI_FORMAT_D32_FLOAT_S8X24_UINT;desc.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2DMS;
                check(reinterpret_cast<MakeDsv>(vt(device)[10])(device,clear_texture,&desc,&clear_dsv)==S_OK && clear_dsv,"MSAA clear fixture creates a writable original DSV");
            }
            if(clear_dsv) {
                TitleTextureLayout planes;
                check(create_xg_texture_layout(32,16,1,1,DXGI_FORMAT_R32G8X24_TYPELESS,1,planes,D3D11_BIND_DEPTH_STENCIL|D3D11_BIND_SHADER_RESOURCE,4)==S_OK,
                      "MSAA clear uses the verified three-plane XG layout");
                std::vector<BYTE> linear(16384);
                const auto verify=[&](UINT bits,BYTE stencil) {
                    bool exact=planes.copy && planes.copy(planes.context.get(),false,linear.data(),clear_base,0,1024,16384,nullptr)==S_OK;
                    for(SIZE_T at=0;exact && at<linear.size();at+=8) {UINT actual=0;std::memcpy(&actual,linear.data()+at,4);exact=actual==bits && linear[at+4]==stencil;}
                    check(exact,"native depth/stencil clear preserves exact depth bits and stencil in all four samples");
                };
                using ClearDepth=void(STDMETHODCALLTYPE*)(void*,void*,UINT,FLOAT,UINT8);
                const auto clear=reinterpret_cast<ClearDepth>(vt(context)[53]);
                clear(context,clear_dsv,3,.5f,0x6d);verify(0x3f000000,0x6d);
                clear(context,clear_dsv,1,.25f,0);verify(0x3e800000,0x6d);
                clear(context,clear_dsv,2,std::numeric_limits<FLOAT>::quiet_NaN(),0x27);verify(0x3e800000,0x27);
                void* same_storage=nullptr;
                check(place_texture(device,&placed_depth,1,0,clear_base,&same_storage)==S_OK && same_storage,"MSAA alias fixture creates a distinct native texture over the same canonical depth/stencil storage");
                if(same_storage) {
                    clear(context,clear_dsv,3,.25f,0x27);
                    reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*,void*)>(vt(context)[47])(context,same_storage,clear_texture);
                    verify(0x3e800000,0x27);
                    clear(context,clear_dsv,3,.5f,0x6d);
                    reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*,UINT,UINT,UINT,UINT,void*,UINT,const D3D11_BOX*)>(vt(context)[46])(context,same_storage,0,0,0,0,clear_texture,0,nullptr);
                    verify(0x3f000000,0x6d);
                    reinterpret_cast<Count>(vt(same_storage)[2])(same_storage);
                    clear(context,clear_dsv,3,.25f,0x27);
                }
                auto* copy_storage=static_cast<BYTE*>(allocate(98304+8192+32,attributes));
                if(!copy_storage)return 1;
                std::memset(copy_storage,0x5a,98304+8192+32);
                auto* copy_base=reinterpret_cast<BYTE*>((reinterpret_cast<std::uintptr_t>(copy_storage)+16+8191)&~std::uintptr_t(8191));
                void* copied_texture=nullptr;
                check(place_texture(device,&placed_depth,1,0,copy_base,&copied_texture)==S_OK && copied_texture,"CopyResource creates a guarded multisample depth/stencil destination");
                if(copied_texture) {
                    for(UINT pass=0;pass<2;++pass) {
                        clear(context,clear_dsv,3,.25f,0x27);
                        std::memset(copy_base,0,98304);
                        if(pass)reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*,UINT,UINT,UINT,UINT,void*,UINT,const D3D11_BOX*)>(vt(context)[46])(context,copied_texture,0,0,0,0,clear_texture,0,nullptr);
                        else reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*,void*)>(vt(context)[47])(context,copied_texture,clear_texture);
                        bool exact=planes.copy(planes.context.get(),false,linear.data(),copy_base,0,1024,16384,nullptr)==S_OK;
                        for(SIZE_T at=0;exact && at<linear.size();at+=8) {UINT bits=0;std::memcpy(&bits,linear.data()+at,4);exact=bits==0x3e800000 && linear[at+4]==0x27;}
                        check(exact,"native CopyResource preserves GPU-cleared depth bits and stencil in all four samples after CPU invalidation");
                        const D3D11_BOX boxed{0,0,0,32,16,1};static UINT refused=0;refused=0;
                        set_d3d11x_trap_handler([](const char*,UINT slot)->std::uintptr_t{refused=slot;return 0;});
                        reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*,UINT,UINT,UINT,UINT,void*,UINT,const D3D11_BOX*)>(vt(context)[46])(context,copied_texture,0,0,0,0,clear_texture,0,&boxed);
                        set_d3d11x_trap_handler(nullptr);
                        check(refused==46 && planes.copy(planes.context.get(),false,linear.data(),copy_base,0,1024,16384,nullptr)==S_OK,
                            "reference depth/stencil region copy rejects even a full non-null box and requires whole-subresource arguments");
                        bool unchanged=true;for(SIZE_T at=0;at<linear.size();at+=8){UINT bits=0;std::memcpy(&bits,linear.data()+at,4);unchanged=unchanged && bits==0x3e800000 && linear[at+4]==0x27;}
                        check(unchanged,"rejected depth/stencil region preserves every sample");
                        verify(0x3e800000,0x27);
                    }
                    reinterpret_cast<Count>(vt(copied_texture)[2])(copied_texture);
                }
                check(std::all_of(copy_storage,copy_base,[](BYTE value){return value==0x5a;}) &&
                    std::all_of(copy_base+98304,copy_storage+98304+8192+32,[](BYTE value){return value==0x5a;}),"multisample resource copy preserves physical allocation guards");
                free_memory(copy_storage,attributes);
                using Deferred=HRESULT(STDMETHODCALLTYPE*)(void*,UINT,void**);
                using Finish=HRESULT(STDMETHODCALLTYPE*)(void*,BOOL,void**);
                using Execute=void(STDMETHODCALLTYPE*)(void*,void*,BOOL);
                void* producer=nullptr;void* list=nullptr;
                check(reinterpret_cast<Deferred>(vt(device)[27])(device,0x60000,&producer)==S_OK && producer,"MSAA clear records through the real deferred ABI");
                if(producer) {
                    reinterpret_cast<ClearDepth>(vt(producer)[53])(producer,clear_dsv,3,.75f,0xa7);
                    check(reinterpret_cast<Finish>(vt(producer)[114])(producer,FALSE,&list)==S_OK && list,"MSAA clear finishes without submitting");
                    verify(0x3e800000,0x27);
                    reinterpret_cast<Count>(vt(producer)[2])(producer);
                }
                reinterpret_cast<Count>(vt(clear_dsv)[2])(clear_dsv);clear_dsv=nullptr;
                reinterpret_cast<Count>(vt(clear_texture)[2])(clear_texture);clear_texture=nullptr;
                if(list) {
                    reinterpret_cast<Execute>(vt(context)[58])(context,list,FALSE);verify(0x3f400000,0xa7);
                    for(SIZE_T at=0;at<linear.size();at+=8) {const UINT bits=0x3e000000;std::memcpy(linear.data()+at,&bits,4);linear[at+4]=0x11;}
                    check(planes.copy(planes.context.get(),true,linear.data(),clear_base,0,1024,16384,nullptr)==S_OK,"MSAA clear reuse accepts fresh canonical data between fences");
                    reinterpret_cast<Execute>(vt(context)[58])(context,list,TRUE);verify(0x3f400000,0xa7);
                    reinterpret_cast<Count>(vt(list)[2])(list);
                }
            }
            if(clear_dsv) reinterpret_cast<Count>(vt(clear_dsv)[2])(clear_dsv);
            if(clear_texture) reinterpret_cast<Count>(vt(clear_texture)[2])(clear_texture);
            check(std::all_of(clear_storage,clear_base,[](auto value){return value==0x5a;}) &&
                std::all_of(clear_base+98304,clear_storage+98304+8192+32,[](auto value){return value==0x5a;}),
                  "immediate/deferred MSAA clears preserve every physical allocation guard");
            free_memory(clear_storage,attributes);
            using Reserve=void*(WINAPI*)(void*,SIZE_T,DWORD,DWORD);
            using ReleaseVirtual=BOOL(WINAPI*)(void*,SIZE_T,DWORD);
            const auto reserve=reinterpret_cast<Reserve>(resolve_kernelx({"kernelx.dll","VirtualAlloc",0}));
            const auto release_virtual=reinterpret_cast<ReleaseVirtual>(resolve_kernelx({"kernelx.dll","VirtualFree",0}));
            auto* esram_mirror=static_cast<unsigned char*>(reserve(nullptr,65536,0x30002000,PAGE_READWRITE));
            for (const auto format:{DXGI_FORMAT_R11G11B10_FLOAT,DXGI_FORMAT_R32_TYPELESS}) {
                const bool depth=format==DXGI_FORMAT_R32_TYPELESS;
                UINT physical_page=depth ? 64 : 48;
                if (!esram_mirror || FAILED(map_title_esram(1,esram_mirror,1,&physical_page))) return 1;
                std::memset(esram_mirror,0x35,65536);
                Desc esram_desc{};
                esram_desc.standard={32,16,1,1,format,{1,0},D3D11_USAGE_DEFAULT,
                    UINT(D3D11_BIND_SHADER_RESOURCE|(depth ? D3D11_BIND_DEPTH_STENCIL : D3D11_BIND_RENDER_TARGET|D3D11_BIND_UNORDERED_ACCESS)),0,0x20000};
                esram_desc.extension[0]=physical_page*65536+8192; esram_desc.extension[1]=depth ? 0 : 8192;
                const UINT original_page=physical_page, mirror_offset=depth ? 24576 : 8192;
                using CreateOwned=HRESULT(STDMETHODCALLTYPE*)(void*,const Desc*,const D3D11_SUBRESOURCE_DATA*,void**);
                const auto create_owned=reinterpret_cast<CreateOwned>(vt(device)[5]);
                using SetAllocationHooks=void(WINAPI*)(GraphicsAllocate,GraphicsFree);
                const auto set_allocation_hooks=reinterpret_cast<SetAllocationHooks>(resolve_kernelx({"kernelx.dll","XMemSetAllocationHooks",0}));
                graphics_default_allocate=allocate; graphics_default_free=free_memory;
                esram_cpu_allocations=esram_ddr_allocations=0;
                set_allocation_hooks(esram_allocate_hook,esram_free_hook);
                texture=nullptr;
                auto invalid_esram=esram_desc; invalid_esram.extension[1]=1;
                check(create_owned(device,&invalid_esram,nullptr,&texture)==DXGI_ERROR_UNSUPPORTED && !texture,
                    "insufficient ESRAM coverage refuses a disconnected DDR substitution");
                const HRESULT esram_created=create_owned(device,&esram_desc,nullptr,&texture);
                check(esram_created==S_OK && texture,
                    "unplaced ESRAM texture owns a stable virtual alias of the existing physical pool");
                set_allocation_hooks(nullptr,nullptr);
                check(esram_cpu_allocations && !esram_ddr_allocations,"ESRAM-only texture preserves original XG CPU hooks without allocating DDR backing");
                unsigned char* owned_alias=nullptr;
                if (texture) {
                    using CreateSrv=HRESULT(STDMETHODCALLTYPE*)(void*,void*,const D3D11_SHADER_RESOURCE_VIEW_DESC*,void**);
                    void* view=nullptr;
                    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{};
                    srv_desc.Format=depth ? DXGI_FORMAT_R32_FLOAT : format; srv_desc.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D; srv_desc.Texture2D.MipLevels=1;
                    check(reinterpret_cast<CreateSrv>(vt(device)[7])(device,texture,&srv_desc,&view)==S_OK && view,
                        "ESRAM texture SRV retains the same physical image");
                    if (view) {
                        UINT32 base=0;std::memcpy(&base,static_cast<char*>(view)+48,4);
                        owned_alias=reinterpret_cast<unsigned char*>(UINT64(base)<<8);
                        check(owned_alias!=esram_mirror+mirror_offset && owned_alias[0]==0x35,"texture has an independent virtual view of the requested ESRAM byte");
                        owned_alias[0]=0x62;
                        check(esram_mirror[mirror_offset]==0x62,"ESRAM texture writes reach the title's real physical alias");
                        physical_page=original_page+1;
                        check(map_title_esram(1,esram_mirror,1,&physical_page)==S_OK,"title can remap its independent alias while the texture retains ownership");
                        esram_mirror[mirror_offset]=0x9a;
                        check(owned_alias[0]==0x62,"title remapping cannot change a texture's physical ESRAM identity");
                        physical_page=original_page;
                        check(map_title_esram(1,esram_mirror,1,&physical_page)==S_OK && esram_mirror[mirror_offset]==0x62,
                            "remapping back observes the original shared physical bytes");
                        reinterpret_cast<Count>(vt(view)[2])(view);
                    }
                    {
                        const std::vector<BYTE> original(esram_mirror,esram_mirror+65536);
                        void* alias=nullptr;void* alias_view=nullptr;
                        check(create_owned(device,&esram_desc,nullptr,&alias)==S_OK && alias,"texture alias copy creates a second native representation of the same ESRAM pages");
                        if(alias) {
                            check(reinterpret_cast<CreateSrv>(vt(device)[7])(device,alias,&srv_desc,&alias_view)==S_OK && alias_view,"physical texture alias keeps an independent canonical view");
                            if(alias_view) {
                                void* canonical=nullptr;std::memcpy(&canonical,static_cast<BYTE*>(alias_view)+80,sizeof canonical);
                                TitleTextureLayout pixels;check(create_xg_texture_layout(32,16,1,1,format,UINT_MAX,pixels,esram_desc.standard.BindFlags)==S_OK,"texture alias oracle retains the original tiled layout");
                                std::vector<UINT> input(512),actual(512);
                                if(depth) {
                                    D3D11_DEPTH_STENCIL_VIEW_DESC dsv{};dsv.Format=DXGI_FORMAT_D32_FLOAT;dsv.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;void* target=nullptr;
                                    using MakeDsv=HRESULT(STDMETHODCALLTYPE*)(void*,void*,const D3D11_DEPTH_STENCIL_VIEW_DESC*,void**);
                                    check(reinterpret_cast<MakeDsv>(vt(device)[10])(device,texture,&dsv,&target)==S_OK && target,"depth alias source owns a real typed DSV");
                                    if(target) {
                                        reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*,UINT,FLOAT,UINT8)>(vt(context)[53])(context,target,1,.375f,0);
                                        reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*,void*)>(vt(context)[47])(context,alias,texture);
                                        check(pixels.copy(pixels.context.get(),false,actual.data(),canonical,0,128,2048,nullptr)==S_OK &&
                                            std::all_of(actual.begin(),actual.end(),[](UINT bits){return bits==0x3ec00000;}),"overlapping depth representations preserve GPU-written bits through the alias snapshot");
                                        reinterpret_cast<Count>(vt(target)[2])(target);
                                    }
                                }else {
                                    using Region=void(STDMETHODCALLTYPE*)(void*,void*,UINT,UINT,UINT,UINT,void*,UINT,const D3D11_BOX*);
                                    const D3D11_BOX box{0,0,0,16,8,1};
                                    for(UINT pass=0;pass<2;++pass) {
                                        for(UINT i=0;i<512;++i)input[i]=0x20000000u+i*1024+pass*7;
                                        check(pixels.copy(pixels.context.get(),true,input.data(),canonical,0,128,2048,nullptr)==S_OK,"physical texture alias imports changed source pixels");
                                        auto expected=input;for(UINT y=0;y<8;++y)std::memcpy(expected.data()+(y+4)*32+8,input.data()+y*32,64);
                                        reinterpret_cast<Region>(vt(context)[46])(context,alias,0,8,4,0,texture,0,&box);
                                        check(pixels.copy(pixels.context.get(),false,actual.data(),canonical,0,128,2048,nullptr)==S_OK && actual==expected,"overlapping color aliases preserve the source rectangle and all outside pixels across repeated copies");
                                    }
                                }
                                reinterpret_cast<Count>(vt(alias_view)[2])(alias_view);
                            }
                            reinterpret_cast<Count>(vt(alias)[2])(alias);
                        }
                        std::memcpy(esram_mirror,original.data(),original.size());
                    }
                    if (depth) {
                        using CreateDsv=HRESULT(STDMETHODCALLTYPE*)(void*,void*,const D3D11_DEPTH_STENCIL_VIEW_DESC*,void**);
                        D3D11_DEPTH_STENCIL_VIEW_DESC description{};
                        description.Format=DXGI_FORMAT_D32_FLOAT; description.ViewDimension=D3D11_DSV_DIMENSION_TEXTURE2D;
                        void* view=nullptr;
                        check(reinterpret_cast<CreateDsv>(vt(device)[10])(device,texture,&description,&view)==S_OK && view,
                            "typeless ESRAM depth owns a typed native D32 view of the same storage");
                        if (view) reinterpret_cast<Count>(vt(view)[2])(view);
                    }
                    reinterpret_cast<Count>(vt(texture)[2])(texture);
                }
                reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,UINT)>(vt(context)[153])(context,0);
                MEMORY_BASIC_INFORMATION retired_alias{};
                check(!owned_alias || (VirtualQuery(owned_alias,&retired_alias,sizeof retired_alias) && retired_alias.State==MEM_FREE),
                    "fenced ESRAM texture retirement releases only its owned virtual view");
                check(esram_mirror[mirror_offset]==0x62,
                    "texture retirement preserves the physical pool and the title's mapping");
            }
            UINT buffer_page=80;
            check(map_title_esram(1,esram_mirror,1,&buffer_page)==S_OK,"buffer fixture maps the existing ESRAM pool");
            std::memset(esram_mirror,0x35,65536);
            struct EsramBufferDesc { D3D11_BUFFER_DESC standard; UINT64 extension; } esram_buffer{};
            esram_buffer.standard={128,D3D11_USAGE_DEFAULT,D3D11_BIND_SHADER_RESOURCE,0,0x20020,0};
            esram_buffer.extension=UINT64(buffer_page*65536+256)|(UINT64(128)<<32);
            using CreateEsramBuffer=HRESULT(STDMETHODCALLTYPE*)(void*,const EsramBufferDesc*,const D3D11_SUBRESOURCE_DATA*,void**);
            const auto create_esram_buffer=reinterpret_cast<CreateEsramBuffer>(vt(device)[3]);
            void* buffer=nullptr;
            check(create_esram_buffer(device,&esram_buffer,nullptr,nullptr)==S_FALSE,
                  "ESRAM buffer validation does not create a mapping");
            const D3D11_SUBRESOURCE_DATA esram_initial{esram_mirror,0,0};
            check(create_esram_buffer(device,&esram_buffer,&esram_initial,&buffer)==DXGI_ERROR_UNSUPPORTED && !buffer,
                  "ESRAM initialization remains explicit until writes over existing physical aliases are translated");
            esram_ddr_allocations=esram_cpu_allocations=0;
            using SetBufferHooks=void(WINAPI*)(void*,void*);
            const auto set_buffer_hooks=reinterpret_cast<SetBufferHooks>(resolve_kernelx({"kernelx.dll","XMemSetAllocationHooks",0}));
            set_buffer_hooks(reinterpret_cast<void*>(&esram_allocate_hook),reinterpret_cast<void*>(&esram_free_hook));
            const HRESULT buffer_created=create_esram_buffer(device,&esram_buffer,nullptr,&buffer);
            set_buffer_hooks(nullptr,nullptr);
            check(buffer_created==S_OK && buffer && !esram_ddr_allocations,
                  "ESRAM buffer owns a physical alias without allocating DDR pixels");
            unsigned char* buffer_alias=nullptr;
            if (buffer) {
                std::memcpy(&buffer_alias,static_cast<char*>(buffer)+48,sizeof buffer_alias);
                check(buffer_alias && buffer_alias!=esram_mirror+256 && buffer_alias[0]==0x35,
                      "ESRAM buffer has its own virtual alias at the requested physical byte");
                if (buffer_alias) {
                    buffer_alias[0]=0x62;
                    check(esram_mirror[256]==0x62,"buffer writes preserve shared ESRAM identity");
                    ++buffer_page;
                    check(map_title_esram(1,esram_mirror,1,&buffer_page)==S_OK,"title remaps its independent buffer alias");
                    esram_mirror[256]=0x9a;
                    check(buffer_alias[0]==0x62,"title remapping cannot redirect existing buffer storage");
                    --buffer_page;
                    check(map_title_esram(1,esram_mirror,1,&buffer_page)==S_OK && esram_mirror[256]==0x62,
                          "ESRAM physical bytes survive a temporary title remap");
                    std::array<BYTE,144> original;std::memcpy(original.data(),esram_mirror+256,original.size());
                    auto alias_desc=esram_buffer;alias_desc.extension=UINT64(buffer_page*65536+272)|(UINT64(128)<<32);void* alias=nullptr;
                    check(create_esram_buffer(device,&alias_desc,nullptr,&alias)==S_OK && alias,"alias copy creates two owned buffers with overlapping physical ESRAM and independent virtual addresses");
                    if(alias) {
                        BYTE* alias_bytes=nullptr;std::memcpy(&alias_bytes,static_cast<BYTE*>(alias)+48,sizeof alias_bytes);
                        check(alias_bytes && alias_bytes!=buffer_alias+16,"alias copy uses a distinct virtual mapping of the shared physical source");
                        using Copy=void(STDMETHODCALLTYPE*)(void*,void*,void*);
                        using Region=void(STDMETHODCALLTYPE*)(void*,void*,UINT,UINT,UINT,UINT,void*,UINT,const D3D11_BOX*);
                        for(UINT reverse=0;reverse<2;++reverse) {
                            for(UINT i=0;i<144;++i)esram_mirror[256+i]=BYTE(i*17+reverse+3);
                            std::array<BYTE,144> expected;std::memcpy(expected.data(),esram_mirror+256,expected.size());
                            std::memmove(expected.data()+(reverse ? 16:0),expected.data()+(reverse ? 0:16),128);
                            reinterpret_cast<Copy>(vt(context)[47])(context,reverse ? alias:buffer,reverse ? buffer:alias);
                            check(!std::memcmp(esram_mirror+256,expected.data(),expected.size()),"full GPU copy preserves source bytes for both directions of physical ESRAM overlap");
                        }
                        using Defer=HRESULT(STDMETHODCALLTYPE*)(void*,UINT,void**);using Finish=HRESULT(STDMETHODCALLTYPE*)(void*,BOOL,void**);
                        void* producer=nullptr;void* list=nullptr;
                        if(reinterpret_cast<Defer>(vt(device)[27])(device,0,&producer)==S_OK && producer) {
                            const D3D11_BOX box{4,0,0,60,1,1};reinterpret_cast<Region>(vt(producer)[46])(producer,buffer,0,8,0,0,alias,0,&box);
                            check(reinterpret_cast<Finish>(vt(producer)[114])(producer,FALSE,&list)==S_OK && list,"deferred alias copy retains the original physical mappings");
                            reinterpret_cast<Count>(vt(producer)[2])(producer);
                        }else check(false,"alias copy creates a deferred producer");
                        reinterpret_cast<Count>(vt(alias)[2])(alias);
                        if(list) {
                            for(UINT pass=0;pass<2;++pass) {
                                for(UINT i=0;i<144;++i)esram_mirror[256+i]=BYTE(i*13+pass+9);
                                std::array<BYTE,144> expected;std::memcpy(expected.data(),esram_mirror+256,expected.size());
                                std::memmove(expected.data()+8,expected.data()+20,56);
                                reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*,BOOL)>(vt(context)[58])(context,list,TRUE);
                                check(!std::memcmp(esram_mirror+256,expected.data(),expected.size()),"deferred physical-alias region copy reads later data after caller release and preserves every outside byte on replay");
                            }
                            reinterpret_cast<Count>(vt(list)[2])(list);
                        }
                    }
                    std::memcpy(esram_mirror+256,original.data(),original.size());
                }
                using CreateRawSrv=HRESULT(STDMETHODCALLTYPE*)(void*,void*,const D3D11_SHADER_RESOURCE_VIEW_DESC*,void**);
                const auto create_raw_srv=reinterpret_cast<CreateRawSrv>(vt(device)[7]);
                D3D11_SHADER_RESOURCE_VIEW_DESC raw{};
                raw.Format=DXGI_FORMAT_R32_TYPELESS; raw.ViewDimension=D3D11_SRV_DIMENSION_BUFFEREX;
                raw.BufferEx={4,8,D3D11_BUFFEREX_SRV_FLAG_RAW};
                void* raw_view=nullptr;
                check(create_raw_srv(device,buffer,&raw,nullptr)==S_FALSE,"raw SRV validation does not acquire ownership");
                check(create_raw_srv(device,buffer,&raw,&raw_view)==S_OK && raw_view,"raw SRV binds the actual ESRAM buffer");
                if (raw_view) {
                    using FastResources=void(STDMETHODCALLTYPE*)(void*,const void*,const void*);
                    const auto fast=reinterpret_cast<FastResources>(vt(context)[209]);
                    std::array<UINT32,5> packet{0x80040001};
                    const auto view_address=reinterpret_cast<std::uintptr_t>(raw_view);
                    std::memcpy(packet.data()+1,&view_address,sizeof view_address);
                    fast(context,packet.data(),packet.data()+packet.size());
                    check(reinterpret_cast<Count>(vt(raw_view)[1])(raw_view)==3,"SetFastResources retains the signed packet SRV object");
                    reinterpret_cast<Count>(vt(raw_view)[2])(raw_view);
                    fast(context,packet.data(),packet.data()+packet.size());
                    check(reinterpret_cast<Count>(vt(raw_view)[1])(raw_view)==3,"fast SRV rebinding balances ownership");
                    reinterpret_cast<Count>(vt(raw_view)[2])(raw_view);
                    static unsigned rejections=0; rejections=0;
                    set_d3d11x_trap_handler([](const char* name,unsigned slot)->std::uintptr_t {
                        if(std::strcmp(name,"ID3D11DeviceContextX")==0 && slot==209) ++rejections;
                        return 0;
                    });
                    fast(context,packet.data(),packet.data()+4);
                    auto invalid=packet; invalid[3]=1;
                    fast(context,invalid.data(),invalid.data()+invalid.size());
                    invalid=packet; invalid[0]=0x80047f02;
                    fast(context,invalid.data(),invalid.data()+invalid.size());
                    invalid=packet; invalid[1]=1;invalid[2]=0;
                    fast(context,invalid.data(),invalid.data()+invalid.size());
                    std::array<UINT32,10> partial{0x80040001,0,0,0,0,0x80060001,0,0,0,0};
                    fast(context,partial.data(),partial.data()+partial.size());
                    invalid=packet;invalid[0]=0x80040000;
                    fast(context,invalid.data(),invalid.data()+1);
                    set_d3d11x_trap_handler(nullptr);
                    check(rejections==6 && reinterpret_cast<Count>(vt(raw_view)[1])(raw_view)==3,
                          "truncated, metadata, range, pointer, unsupported-tail and zero-count packets preserve prior fast bindings transactionally");
                    reinterpret_cast<Count>(vt(raw_view)[2])(raw_view);
                    fast(context,packet.data(),packet.data());
                    packet[0]=0x80040001;packet[1]=packet[2]=0;
                    fast(context,packet.data(),packet.data()+packet.size());
                    check(reinterpret_cast<Count>(vt(raw_view)[1])(raw_view)==2,"fast null SRD unbind and empty packets preserve exact ownership");
                    reinterpret_cast<Count>(vt(raw_view)[2])(raw_view);
                    using BindResources=void(STDMETHODCALLTYPE*)(void*,UINT,UINT,void* const*);
                    const auto bind_vertex=reinterpret_cast<BindResources>(vt(context)[25]);
                    const auto bind_pixel=reinterpret_cast<BindResources>(vt(context)[9]);
                    const auto bind_compute=reinterpret_cast<BindResources>(vt(context)[67]);
                    const auto bind_geometry=reinterpret_cast<BindResources>(vt(context)[31]);
                    void* views[]{raw_view};
                    bind_vertex(context,127,1,views);bind_vertex(context,127,1,views);
                    check(reinterpret_cast<Count>(vt(raw_view)[1])(raw_view)==3,"legacy VS SRV last-slot binding and rebind retain the same view");
                    reinterpret_cast<Count>(vt(raw_view)[2])(raw_view);
                    bind_vertex(context,127,1,nullptr);bind_vertex(context,128,0,nullptr);
                    check(reinterpret_cast<Count>(vt(raw_view)[1])(raw_view)==2,"legacy VS SRV unbind releases its retained view");
                    reinterpret_cast<Count>(vt(raw_view)[2])(raw_view);
                    bind_vertex(context,0,1,views);
                    bind_pixel(context,0,1,views);
                    bind_compute(context,0,1,views);bind_compute(context,0,1,views);
                    bind_geometry(context,127,1,views);bind_geometry(context,127,1,views);
                    bind_geometry(context,128,0,nullptr);
                    check(reinterpret_cast<Count>(vt(raw_view)[1])(raw_view)==6,"VS, PS, GS and CS SRV bindings own independent references and balance rebinds");
                    reinterpret_cast<Count>(vt(raw_view)[2])(raw_view);
                    reinterpret_cast<void(STDMETHODCALLTYPE*)(void*)>(vt(context)[110])(context);
                    check(reinterpret_cast<Count>(vt(raw_view)[1])(raw_view)==2,"ClearState releases VS SRVs with the other common bindings");
                    reinterpret_cast<Count>(vt(raw_view)[2])(raw_view);
                    struct { D3D11_SHADER_RESOURCE_VIEW_DESC desc{}; UINT guard=0xaabbccdd; } queried;
                    reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,D3D11_SHADER_RESOURCE_VIEW_DESC*)>(vt(raw_view)[9])(raw_view,&queried.desc);
                    check(!std::memcmp(&queried.desc,&raw,sizeof raw) && queried.guard==0xaabbccdd,
                          "raw SRV preserves BUFFEREX flags and exact query extent");
                    std::array<UINT32,8> srd{};
                    std::memcpy(srd.data(),static_cast<char*>(raw_view)+48,sizeof srd);
                    check((UINT64(srd[0])|(UINT64(srd[1]&255)<<32))==reinterpret_cast<std::uintptr_t>(buffer_alias)+16 &&
                          !(srd[1]&0xffffff00) && srd[2]==32 && srd[3]==0x24fac &&
                          std::all_of(srd.begin()+4,srd.end(),[](auto x){return x==0;}),
                          "original raw SRD counts bytes, clears stride and preserves the physical base");
                    reinterpret_cast<Count>(vt(raw_view)[2])(raw_view);
                }
                raw.BufferEx.FirstElement=1;
                check(create_raw_srv(device,buffer,&raw,&raw_view)==DXGI_ERROR_UNSUPPORTED && !raw_view,
                      "raw view refuses an offset that violates host alignment");
                raw.BufferEx.FirstElement=32;
                check(create_raw_srv(device,buffer,&raw,&raw_view)==E_INVALIDARG && !raw_view,
                      "raw SRV range cannot exceed its logical buffer");
                reinterpret_cast<Count>(vt(buffer)[2])(buffer);
            }
            MEMORY_BASIC_INFORMATION buffer_retired{};
            check(buffer_alias && VirtualQuery(buffer_alias,&buffer_retired,sizeof buffer_retired) && buffer_retired.State==MEM_FREE && esram_mirror[256]==0x62,
                  "buffer retirement frees its alias while preserving pool and title ownership");
            esram_buffer.extension=UINT64(buffer_page*65536)|(UINT64(64)<<32);
            check(create_esram_buffer(device,&esram_buffer,nullptr,&buffer)==DXGI_ERROR_UNSUPPORTED && !buffer,
                  "partly ESRAM buffer remains explicit without a translated DDR tail");
            esram_buffer.extension=0x1fffff0;
            check(create_esram_buffer(device,&esram_buffer,nullptr,&buffer)==DXGI_ERROR_UNSUPPORTED && !buffer,
                  "buffer cannot extend beyond the physical ESRAM pool");
            check(release_virtual(esram_mirror,0,MEM_RELEASE),"ESRAM fixture releases its own title mapping");
            {
                struct Format {DXGI_FORMAT value;UINT bytes;UINT64 one,half;};
                const Format formats[]{
                    {DXGI_FORMAT_R8G8B8A8_UNORM,4,0xfefefefe,0x7f7f7f7f},
                    {DXGI_FORMAT_R10G10B10A2_UNORM,4,1022u|(1022u<<10)|(1022u<<20)|(2u<<30),511u|(511u<<10)|(511u<<20)|(1u<<30)},
                    {DXGI_FORMAT_R11G11B10_FLOAT,4,(15u<<6)|((15u<<6)<<11)|((15u<<5)<<22),(14u<<6)|((14u<<6)<<11)|((14u<<5)<<22)},
                    {DXGI_FORMAT_R16G16B16A16_FLOAT,8,0x3c003c003c003c00ull,0x3800380038003800ull}};
                using Create=HRESULT(STDMETHODCALLTYPE*)(void*,const Desc*,const D3D11_SUBRESOURCE_DATA*,void**);
                using Srv=HRESULT(STDMETHODCALLTYPE*)(void*,void*,const D3D11_SHADER_RESOURCE_VIEW_DESC*,void**);
                using Resolve=void(STDMETHODCALLTYPE*)(void*,void*,UINT,void*,UINT,DXGI_FORMAT);
                using Defer=HRESULT(STDMETHODCALLTYPE*)(void*,UINT,void**);using Finish=HRESULT(STDMETHODCALLTYPE*)(void*,BOOL,void**);
                for(const auto& f:formats)for(const UINT samples:{1u,2u,4u,8u}) {
                    Desc source_desc{};source_desc.standard={32,32,1,1,f.value,{samples,0},D3D11_USAGE_DEFAULT,D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE,0,samples>1 ? 0x40000u:0u};
                    Desc target_desc{};target_desc.standard={64,64,3,6,f.value,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_SHADER_RESOURCE,0,D3D11_RESOURCE_MISC_TEXTURECUBE};
                    void* objects[2]{};void* views[2]{};void* canonical[2]{};
                    const auto create=reinterpret_cast<Create>(vt(device)[5]);const auto srv=reinterpret_cast<Srv>(vt(device)[7]);
                    const Desc* descriptions[]{&source_desc,&target_desc};
                    for(UINT i=0;i<2;++i) {
                        const auto created=create(device,descriptions[i],nullptr,&objects[i]);
                        if(FAILED(created))std::fprintf(stderr,"resolve fixture format=%u samples=%u operand=%u hr=0x%08lX\n",f.value,samples,i,created);
                        check(created==S_OK && objects[i],"resolve fixture creates real source/destination textures");if(!objects[i])return 1;
                        check(srv(device,objects[i],nullptr,&views[i])==S_OK && views[i],"resolve fixture retains canonical source/destination views");if(!views[i])return 1;
                        std::memcpy(&canonical[i],static_cast<BYTE*>(views[i])+80,sizeof canonical[i]);
                    }
                    TitleTextureLayout source_layout,target_layout;
                    check(create_xg_texture_layout(32,32,1,1,f.value,UINT_MAX,source_layout,source_desc.standard.BindFlags,samples)==S_OK &&
                        create_xg_texture_layout(64,64,3,6,f.value,UINT_MAX,target_layout,target_desc.standard.BindFlags)==S_OK,"resolve pixel oracles use the original sample and cube layouts");
                    const UINT pitch=32*samples*f.bytes;std::vector<BYTE> input(pitch*32);
                    const auto fill_source=[&](UINT phase) {
                        for(UINT y=0;y<32;++y)for(UINT x=0;x<32;++x)for(UINT sample=0;sample<samples;++sample) {
                            const UINT kind=(x+2*y+phase)%3;const UINT64 value=kind==1 || (kind==2 && sample>=samples/2) ? f.one:0;
                            std::memcpy(input.data()+((y*32+x)*samples+sample)*f.bytes,&value,f.bytes);
                        }
                        check(source_layout.copy(source_layout.context.get(),true,input.data(),canonical[0],0,pitch,UINT(input.size()),nullptr)==S_OK,"resolve source keeps distinct sample values and changed pixel patterns");
                    };
                    const auto fill_target=[&]() {
                        for(UINT sub=0;sub<18;++sub) {
                            const UINT width=64>>(sub%3);std::vector<BYTE> guards(width*width*f.bytes,0x19);
                            check(target_layout.copy(target_layout.context.get(),true,guards.data(),canonical[1],sub,width*f.bytes,UINT(guards.size()),nullptr)==S_OK,"resolve destination initializes every face/mip guard");
                        }
                    };
                    const auto verify=[&](UINT phase,bool resolved) {
                        for(UINT sub=0;sub<18;++sub) {
                            const UINT width=64>>(sub%3);std::vector<BYTE> expected(width*width*f.bytes,0x19),actual(expected.size());
                            if(resolved && sub==16)for(UINT y=0;y<32;++y)for(UINT x=0;x<32;++x) {
                                const UINT kind=(x+2*y+phase)%3;const UINT64 value=kind==0 ? 0:kind==1 || samples==1 ? f.one:f.half;
                                std::memcpy(expected.data()+(y*32+x)*f.bytes,&value,f.bytes);
                            }
                            const auto copied=target_layout.copy(target_layout.context.get(),false,actual.data(),canonical[1],sub,width*f.bytes,UINT(actual.size()),nullptr);
                            if(copied==S_OK && actual!=expected) {
                                const auto at=SIZE_T(std::mismatch(actual.begin(),actual.end(),expected.begin()).first-actual.begin())/f.bytes*f.bytes;
                                UINT64 observed=0,wanted=0;std::memcpy(&observed,actual.data()+at,f.bytes);std::memcpy(&wanted,expected.data()+at,f.bytes);
                                std::fprintf(stderr,"resolve pixels format=%u samples=%u phase=%u sub=%u byte=%zu actual=0x%llX expected=0x%llX\n",f.value,samples,phase,sub,at,observed,wanted);
                            }
                            check(copied==S_OK && actual==expected,"resolve preserves exact sample averages/copy pixels in the selected cube face/mip and every outside guard");
                        }
                    };
                    const auto resolve=reinterpret_cast<Resolve>(vt(context)[57]);const auto request=samples==1 ? DXGI_FORMAT(UINT_MAX):f.value;
                    fill_source(0);fill_target();
                    static UINT refused=0;set_d3d11x_trap_handler([](const char*,UINT slot)->std::uintptr_t{refused=slot;return 0;});
                    refused=0;resolve(context,objects[1],UINT_MAX,objects[0],0,request);check(refused==57,"resolve rejects invalid destination subresources");
                    refused=0;resolve(context,objects[1],16,reinterpret_cast<void*>(1),0,request);check(refused==57,"resolve rejects unreadable source resources");
                    if(samples>1) {
                        refused=0;resolve(context,objects[1],16,objects[0],0,DXGI_FORMAT_R32_UINT);check(refused==57,"MSAA resolve rejects an incompatible format before GPU submission");
                        if(f.value==DXGI_FORMAT_R8G8B8A8_UNORM) {
                            refused=0;resolve(context,objects[1],16,objects[0],0,DXGI_FORMAT_R8G8B8A8_UNORM_SRGB);
                            check(refused==57,"typed MSAA resources require their own resolve format rather than a sibling interpretation");
                        }
                        refused=0;resolve(context,objects[0],0,objects[0],0,f.value);check(refused==57,"resolve requires a single-sample destination");
                    }else if(f.value==DXGI_FORMAT_R8G8B8A8_UNORM) {
                        auto mismatch=source_desc;mismatch.standard.Format=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;void* sibling=nullptr;
                        check(create(device,&mismatch,nullptr,&sibling)==S_OK && sibling,"single-sample resolve negative control creates a valid sibling-format source");
                        if(sibling) {
                            refused=0;resolve(context,objects[1],16,sibling,0,f.value);
                            check(refused==57,"reference single-sample resolve rejects different source/destination formats even when native bit copies would allow them");
                            reinterpret_cast<Count>(vt(sibling)[2])(sibling);
                        }
                    }
                    set_d3d11x_trap_handler(nullptr);verify(0,false);
                    resolve(context,objects[1],16,objects[0],0,request);verify(0,true);
                    if(samples==4) {
                        Desc flat{};flat.standard={32,32,1,1,f.value,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_SHADER_RESOURCE,0,0};
                        TitleTextureLayout pixels;check(create_xg_texture_layout(32,32,1,1,f.value,UINT_MAX,pixels,flat.standard.BindFlags)==S_OK,"aliased resolve uses the original single-sample layout");
                        void* alias=nullptr;void* view=nullptr;
                        using Place=HRESULT(STDMETHODCALLTYPE*)(void*,const Desc*,UINT,UINT,void*,void**);
                        check(reinterpret_cast<Place>(vt(device)[64])(device,&flat,pixels.tile_mode,0,canonical[0],&alias)==S_OK && alias,"resolve creates a single-sample destination over the source's shared storage");
                        if(alias) {
                            check(srv(device,alias,nullptr,&view)==S_OK && view,"aliased resolve destination retains its canonical image");
                            resolve(context,alias,0,objects[0],0,f.value);
                            std::vector<BYTE> actual(32*32*f.bytes);void* data=nullptr;if(view)std::memcpy(&data,static_cast<BYTE*>(view)+80,sizeof data);
                            bool exact=view && pixels.copy(pixels.context.get(),false,actual.data(),data,0,32*f.bytes,UINT(actual.size()),nullptr)==S_OK;
                            for(UINT y=0;y<32;++y)for(UINT x=0;x<32;++x) {
                                const UINT kind=(x+2*y)%3;const UINT64 expected=kind==0 ? 0:kind==1 ? f.one:f.half;
                                exact=exact && !std::memcmp(actual.data()+(y*32+x)*f.bytes,&expected,f.bytes);
                            }
                            check(exact,"aliased MSAA resolve snapshots source samples on GPU before overwriting their canonical storage");
                            if(view)reinterpret_cast<Count>(vt(view)[2])(view);reinterpret_cast<Count>(vt(alias)[2])(alias);
                            fill_source(0);
                        }
                    }
                    fill_target();verify(0,false);
                    void* producer=nullptr;void* list=nullptr;
                    if(reinterpret_cast<Defer>(vt(device)[27])(device,0,&producer)==S_OK && producer) {
                        reinterpret_cast<Resolve>(vt(producer)[57])(producer,objects[1],16,objects[0],0,request);
                        check(reinterpret_cast<Finish>(vt(producer)[114])(producer,FALSE,&list)==S_OK && list,"deferred resolve retains both resources and the requested format/subresources");
                        verify(0,false);
                        reinterpret_cast<Count>(vt(producer)[2])(producer);
                    }else check(false,"resolve creates a deferred producer");
                    for(UINT i=0;i<2;++i){reinterpret_cast<Count>(vt(views[i])[2])(views[i]);reinterpret_cast<Count>(vt(objects[i])[2])(objects[i]);}
                    if(list) {
                        for(UINT phase=1;phase<3;++phase) {
                            fill_source(phase);fill_target();verify(phase,false);
                            reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*,BOOL)>(vt(context)[58])(context,list,TRUE);verify(phase,true);
                        }
                        reinterpret_cast<Count>(vt(list)[2])(list);
                    }
                }
            }
            {
                using PlaceBuffer=HRESULT(STDMETHODCALLTYPE*)(void*,const EsramBufferDesc*,void*,void**);
                const auto place_buffer=reinterpret_cast<PlaceBuffer>(vt(device)[62]);
                auto* storage=static_cast<unsigned char*>(allocate(1024,attributes));
                if (!storage) return 1;
                std::memset(storage,0x35,1024);
                EsramBufferDesc placed{};
                placed.standard={128,D3D11_USAGE_IMMUTABLE,D3D11_BIND_CONSTANT_BUFFER,0,0,0};
                void* buffer=nullptr;
                check(place_buffer(device,&placed,storage+256,nullptr)==S_FALSE,"placement buffer validation does not acquire backing ownership");
                check(place_buffer(device,&placed,storage+256,&buffer)==S_OK && buffer,"placement constant buffer pins the supplied canonical range");
                if (buffer) {
                    void* base=nullptr;std::memcpy(&base,static_cast<char*>(buffer)+48,sizeof base);
                    std::array<UINT32,4> srd{};std::memcpy(srd.data(),static_cast<char*>(buffer)+32,sizeof srd);
                    check(base==storage+256 && (UINT64(srd[0])|(UINT64(srd[1]&255)<<32))==reinterpret_cast<std::uintptr_t>(base) &&
                          (srd[1]&0x3fff0000)==(16u<<16) && srd[2]==8,"placement CBV preserves physical base, sixteen-byte stride and count");
                    struct { EsramBufferDesc desc{}; UINT64 guard=0x1122334455667788ull; } queried;
                    reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,EsramBufferDesc*)>(vt(buffer)[12])(buffer,&queried.desc);
                    check(!std::memcmp(&queried.desc,&placed,sizeof placed) && queried.guard==0x1122334455667788ull,
                          "placement buffer GetDesc preserves caller semantics and exact ABI extent");
                    void* sibling=nullptr;
                    check(place_buffer(device,&placed,storage+384,&sibling)==S_OK && sibling,"offset constants reuse an existing matching placement buffer");
                    if(sibling) {
                        using FastResources=void(STDMETHODCALLTYPE*)(void*,const void*,const void*);
                        const auto fast=reinterpret_cast<FastResources>(vt(context)[209]);
                        std::array<UINT32,5> packet{0x91040d01};UINT64 offset=128;
                        std::memcpy(packet.data()+1,&buffer,sizeof buffer);std::memcpy(packet.data()+3,&offset,sizeof offset);
                        fast(context,packet.data(),packet.data()+packet.size());
                        check(reinterpret_cast<Count>(vt(sibling)[1])(sibling)==4,"offset constants retain the existing object through parent and pixel binding");
                        reinterpret_cast<Count>(vt(sibling)[2])(sibling);
                        fast(context,packet.data(),packet.data()+packet.size());
                        check(reinterpret_cast<Count>(vt(sibling)[1])(sibling)==4,"repeated offset packets reuse identity without accumulating references");
                        reinterpret_cast<Count>(vt(sibling)[2])(sibling);
                        static UINT offset_rejections=0;offset_rejections=0;
                        set_d3d11x_trap_handler([](const char* name,UINT slot)->std::uintptr_t {
                            if(!std::strcmp(name,"ID3D11DeviceContextX") && slot==209)++offset_rejections;return 0;
                        });
                        offset=UINT64_MAX;std::memcpy(packet.data()+3,&offset,sizeof offset);
                        fast(context,packet.data(),packet.data()+packet.size());
                        set_d3d11x_trap_handler(nullptr);
                        check(offset_rejections==1 && reinterpret_cast<Count>(vt(sibling)[1])(sibling)==4,
                              "overflowing offset preserves the prior constants binding and existing references");
                        reinterpret_cast<Count>(vt(sibling)[2])(sibling);
                        packet[1]=packet[2]=0;
                        fast(context,packet.data(),packet.data()+packet.size());
                        check(reinterpret_cast<Count>(vt(sibling)[1])(sibling)==3,"null placement constants ignore metadata and unbind without discarding the parent cache");
                        reinterpret_cast<Count>(vt(sibling)[2])(sibling);
                        reinterpret_cast<Count>(vt(sibling)[2])(sibling);
                    }
                    reinterpret_cast<Count>(vt(buffer)[2])(buffer);
                }
                MEMORY_BASIC_INFORMATION info{};
                check(VirtualQuery(storage,&info,sizeof info) && info.State==MEM_COMMIT &&
                      std::all_of(storage,storage+1024,[](auto x){return x==0x35;}),
                      "placement retirement leaves borrowed allocation and every guard intact");
                placed.standard={128,D3D11_USAGE_DEFAULT,D3D11_BIND_SHADER_RESOURCE,0,D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS,0};
                check(place_buffer(device,&placed,storage+512,&buffer)==S_OK && buffer,"placement raw buffer reuses the same canonical allocation");
                free_memory(storage,attributes);
                check(VirtualQuery(storage,&info,sizeof info) && info.State==MEM_COMMIT,"live placement buffer defers title allocation retirement");
                if (buffer) reinterpret_cast<Count>(vt(buffer)[2])(buffer);
                check(VirtualQuery(storage,&info,sizeof info) && info.State==MEM_FREE,"last placement owner retires the allocation the title already released");
                check(FAILED(place_buffer(device,&placed,reinterpret_cast<void*>(1),&buffer)) && !buffer,
                      "placement refuses an unregistered address without acquiring or freeing it");
                check(place_buffer(device,&placed,reinterpret_cast<void*>(0x10000000000ull-64),&buffer)==DXGI_ERROR_UNSUPPORTED && !buffer,
                      "placement rejects a span extending beyond the native forty-bit descriptor address range");
                placed.standard.BindFlags=D3D11_BIND_VERTEX_BUFFER;
                check(place_buffer(device,&placed,reinterpret_cast<void*>(1),&buffer)==DXGI_ERROR_UNSUPPORTED && !buffer,
                      "placement buffer keeps unsupported binding semantics explicit");
            }
            for (const auto storage_format:{DXGI_FORMAT_R8_TYPELESS,DXGI_FORMAT_R10G10B10A2_TYPELESS,DXGI_FORMAT_R16G16_FLOAT}) {
                NativeUavClear native_texture;
                check(SUCCEEDED(native_texture.initialize(argc==5)),"independent native texture-clear control initializes on the same backend");if(!native_texture.gpu)return 1;
                Desc color{};
                color.standard={32,16,1,1,storage_format,{1,0},D3D11_USAGE_DEFAULT,
                    D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET|D3D11_BIND_UNORDERED_ACCESS,0,0};
                using CreateOwned=HRESULT(STDMETHODCALLTYPE*)(void*,const Desc*,const D3D11_SUBRESOURCE_DATA*,void**);
                texture=nullptr;
                check(reinterpret_cast<CreateOwned>(vt(device)[5])(device,&color,nullptr,&texture)==S_OK && texture,
                    "typeless color has one native texture for typed SRV RTV and UAV aliases");
                if (!texture) continue;
                const DXGI_FORMAT formats[]{storage_format==DXGI_FORMAT_R16G16_FLOAT ? storage_format : storage_format==DXGI_FORMAT_R8_TYPELESS ? DXGI_FORMAT_R8_UNORM : DXGI_FORMAT_R10G10B10A2_UNORM,
                    storage_format==DXGI_FORMAT_R16G16_FLOAT ? storage_format : storage_format==DXGI_FORMAT_R8_TYPELESS ? DXGI_FORMAT_R8_UINT : DXGI_FORMAT_R10G10B10A2_UINT};
                const std::array<DXGI_FORMAT,3> views{formats[0],formats[1],DXGI_FORMAT_R32_UINT};
                for (const auto format:views) for (UINT kind=0;kind<3;++kind) {
                    if (format==DXGI_FORMAT_R32_UINT && (storage_format!=DXGI_FORMAT_R10G10B10A2_TYPELESS || kind!=2)) continue;
                    using Factory=HRESULT(STDMETHODCALLTYPE*)(void*,void*,const void*,void**);
                    const auto factory=reinterpret_cast<Factory>(vt(device)[kind==0 ? 7 : kind==1 ? 9 : 8]);
                    D3D11_SHADER_RESOURCE_VIEW_DESC srv{}; srv.Format=format; srv.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D; srv.Texture2D.MipLevels=1;
                    D3D11_RENDER_TARGET_VIEW_DESC rtv{}; rtv.Format=format; rtv.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2D;
                    D3D11_UNORDERED_ACCESS_VIEW_DESC uav{}; uav.Format=format; uav.ViewDimension=D3D11_UAV_DIMENSION_TEXTURE2D;
                    const void* input=kind==0 ? static_cast<const void*>(&srv) : kind==1 ? static_cast<const void*>(&rtv) : &uav;
                    void* view=nullptr;
                    check(factory(device,texture,input,nullptr)==S_FALSE,"typed color view validates without taking ownership");
                    check(factory(device,texture,input,&view)==S_OK && view,"typed color view owns a real native descriptor");
                    if (!view) continue;
                    alignas(8) std::array<unsigned char,32> result;
                    result.fill(0x5a);
                    const SIZE_T bytes=kind==0 ? sizeof srv : kind==1 ? sizeof rtv : sizeof uav;
                    reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*)>(vt(view)[9])(view,result.data());
                    check(!std::memcmp(result.data(),input,bytes) && std::all_of(result.begin()+bytes,result.end(),[](auto x){return x==0x5a;}),
                        "typed color view queries preserve their format dimension and ABI bounds");
                    if (kind!=1) {
                        std::array<UINT32,8> descriptor{}; void* canonical=nullptr;
                        std::memcpy(descriptor.data(),static_cast<char*>(view)+48,sizeof descriptor);
                        std::memcpy(&canonical,static_cast<char*>(view)+80,sizeof canonical);
                        const bool integer=format==DXGI_FORMAT_R8_UINT || format==DXGI_FORMAT_R10G10B10A2_UINT || format==DXGI_FORMAT_R32_UINT;
                        check(descriptor[0]==(reinterpret_cast<std::uintptr_t>(canonical)>>8) &&
                            ((descriptor[1]>>26)&15)==(integer ? 4u : format==DXGI_FORMAT_R16G16_FLOAT ? 7u : 0u),"typed SRV/UAV SRDs keep physical identity and native numeric interpretation");
                    }
                    if(kind==2) {
                        using ClearUav=void(STDMETHODCALLTYPE*)(void*,void*,const void*);
                        using DeferredUav=HRESULT(STDMETHODCALLTYPE*)(void*,UINT,void**);
                        using FinishUav=HRESULT(STDMETHODCALLTYPE*)(void*,BOOL,void**);
                        using ExecuteUav=void(STDMETHODCALLTYPE*)(void*,void*,BOOL);
                        const bool integer=format==DXGI_FORMAT_R8_UINT || format==DXGI_FORMAT_R10G10B10A2_UINT || format==DXGI_FORMAT_R32_UINT;
                        const UINT slot=integer ? 51:52,element=storage_format==DXGI_FORMAT_R8_TYPELESS ? 1:4;
                        const auto clear=reinterpret_cast<ClearUav>(vt(context)[slot]);
                        void* canonical=nullptr;std::memcpy(&canonical,static_cast<char*>(view)+80,sizeof canonical);
                        TitleTextureLayout pixels;
                        check(create_xg_texture_layout(32,16,1,1,storage_format,UINT_MAX,pixels,color.standard.BindFlags)==S_OK,"texture clear obtains the original canonical layout");
                        if(!pixels.copy)return 1;
                        std::vector<BYTE> pixel_result(512*element),expected_pixels;
                        const auto verify=[&] {
                            const HRESULT copied=pixels.copy(pixels.context.get(),false,pixel_result.data(),canonical,0,32*element,512*element,nullptr);
                            const bool exact=SUCCEEDED(copied) && pixel_result==expected_pixels;
                            if(!exact)std::fprintf(stderr,"reference texture clear mismatch resource=%u view=%u\n",storage_format,format);
                            check(exact,"texture Uint/Float clear preserves the exact reference native pixels");
                        };
                        const std::array<UINT,4> inputs[]{{0x12345037,0x12,0x34,0x56},{0x3f000000,0x3e800000,0x3f800000,0}};
                        for(const auto& values:inputs)for(const UINT operation:{51u,52u}) {
                            if(FAILED(native_texture.texture(pixels.resource_desc,format,element,values,operation==52,expected_pixels)))return 1;
                            reinterpret_cast<ClearUav>(vt(context)[operation])(context,view,values.data());verify();
                        }
                        for(const UINT operation:{51u,52u}) {
                            std::array<UINT,4> values=operation==51 ? std::array<UINT,4>{0x61,0x12,0x34,0x56}:std::array<UINT,4>{0x40900061,0x3e800000,0x3f000000,0x3f800000};
                            if(FAILED(native_texture.texture(pixels.resource_desc,format,element,values,operation==52,expected_pixels)))return 1;
                            std::array<UINT,4> reset_values{};
                            if(std::none_of(expected_pixels.begin(),expected_pixels.end(),[](BYTE value){return value!=0;}))reset_values[0]=integer ? 0x61u:0x3f400000u;
                            std::vector<BYTE> reset_pixels;
                            if(FAILED(native_texture.texture(pixels.resource_desc,format,element,reset_values,!integer,reset_pixels)))return 1;
                            check(reset_pixels!=expected_pixels,"reference texture replay control starts from a different pixel image");
                            clear(context,view,reset_values.data());
                            const std::vector<BYTE> before(static_cast<BYTE*>(canonical),static_cast<BYTE*>(canonical)+pixels.bytes);
                            void* producer=nullptr;void* retained=nullptr;void* list=nullptr;
                            check(reinterpret_cast<DeferredUav>(vt(device)[27])(device,0,&producer)==S_OK && producer &&
                                factory(device,texture,input,&retained)==S_OK && retained,"texture clear creates a deferred producer and retained native view");
                            if(!producer || !retained)return 1;
                            const auto record=reinterpret_cast<ClearUav>(vt(producer)[operation]);
                            record(producer,nullptr,reinterpret_cast<const void*>(1));
                            record(producer,retained,values.data());
                            for(const UINT empty:{51u,52u})reinterpret_cast<ClearUav>(vt(producer)[empty])(producer,retained,nullptr);
                            values.fill(0);
                            check(reinterpret_cast<FinishUav>(vt(producer)[114])(producer,TRUE,&list)==S_OK && list &&
                                !std::memcmp(before.data(),canonical,before.size()),"texture clear recording captures values and operation without executing");
                            reinterpret_cast<Count>(vt(retained)[2])(retained);reinterpret_cast<Count>(vt(producer)[2])(producer);
                            if(!list)return 1;
                            for(UINT pass=0;pass<2;++pass) {
                                clear(context,view,reset_values.data());reinterpret_cast<ExecuteUav>(vt(context)[58])(context,list,pass!=0);verify();
                            }
                            reinterpret_cast<Count>(vt(list)[2])(list);
                        }
                    }
                    if (kind==1 && format==formats[0]) {
                        using SetTargets=void(STDMETHODCALLTYPE*)(void*,UINT,void* const*,void*);
                        const auto bind_targets=reinterpret_cast<SetTargets>(vt(context)[33]);
                        void* targets[]{view};
                        check(reinterpret_cast<Count>(vt(view)[1])(view)==2,"RTV fixture observes its public reference count");
                        reinterpret_cast<Count>(vt(view)[2])(view);
                        bind_targets(context,1,targets,nullptr);
                        check(reinterpret_cast<Count>(vt(view)[1])(view)==3,"legacy slot 33 retains its real render target view");
                        reinterpret_cast<Count>(vt(view)[2])(view);
                        using GetTargets=void(STDMETHODCALLTYPE*)(void*,UINT,void**,void**);
                        const auto get_targets=reinterpret_cast<GetTargets>(vt(context)[89]);
                        void* returned[9]{};returned[8]=reinterpret_cast<void*>(1);void* returned_depth=reinterpret_cast<void*>(1);
                        get_targets(context,8,returned,&returned_depth);
                        check(returned[0]==view && !returned[1] && !returned[7] && returned[8]==reinterpret_cast<void*>(1) && !returned_depth,
                              "legacy OMGetRenderTargets returns retained bindings, null holes and preserves the output guard");
                        check(reinterpret_cast<Count>(vt(view)[1])(view)==4,"OMGetRenderTargets owns a reference independent of the context binding");
                        reinterpret_cast<Count>(vt(view)[2])(view);
                        if(returned[0])reinterpret_cast<Count>(vt(returned[0])[2])(returned[0]);
                        get_targets(context,0,nullptr,nullptr);
                        reinterpret_cast<void(STDMETHODCALLTYPE*)(void*)>(vt(context)[110])(context);
                        check(reinterpret_cast<Count>(vt(view)[1])(view)==2,"ClearState releases the render binding while retaining caller view ownership");
                        reinterpret_cast<Count>(vt(view)[2])(view);
                        returned[0]=reinterpret_cast<void*>(1);get_targets(context,1,returned,nullptr);
                        check(!returned[0],"OMGetRenderTargets observes ClearState through the same binding state");
                        constexpr FLOAT white[]{1,1,1,1};
                        reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*,const FLOAT*)>(vt(context)[50])(context,view,white);
                        void* alias=nullptr;
                        // The SRV exposes the same canonical pixels as this RTV.
                        void* srv_view=nullptr;
                        using MakeSrv=HRESULT(STDMETHODCALLTYPE*)(void*,void*,const D3D11_SHADER_RESOURCE_VIEW_DESC*,void**);
                        check(reinterpret_cast<MakeSrv>(vt(device)[7])(device,texture,&srv,&srv_view)==S_OK && srv_view,
                              "legacy RTV clear keeps the resource visible through its original SRV");
                        if (srv_view) {
                            std::memcpy(&alias,static_cast<char*>(srv_view)+80,sizeof alias);
                            TitleTextureLayout pixels;
                            const UINT element=storage_format==DXGI_FORMAT_R8_TYPELESS ? 1 : 4;
                            hr=create_xg_texture_layout(32,16,1,1,storage_format,UINT_MAX,pixels,color.standard.BindFlags);
                            std::vector<unsigned char> linear(32*16*element);
                            check(SUCCEEDED(hr) && pixels.copy(pixels.context.get(),false,linear.data(),alias,0,32*element,32*16*element,nullptr)==S_OK,
                                  "legacy clear publishes real GPU pixels through the original XG layout");
                            bool exact=true;
                            for (SIZE_T at=0;at<linear.size();++at) exact=exact && linear[at]==(storage_format==DXGI_FORMAT_R16G16_FLOAT ? (at%2 ? 0x3c : 0) : 0xff);
                            check(exact,"legacy context slot 50 clears every pixel with exact typed values");
                            if (storage_format==DXGI_FORMAT_R8_TYPELESS) {
                                using Deferred=HRESULT(STDMETHODCALLTYPE*)(void*,UINT,void**);
                                using Update=void(STDMETHODCALLTYPE*)(void*,void*,UINT,const D3D11_BOX*,const void*,UINT,UINT);
                                using FinishUpdate=HRESULT(STDMETHODCALLTYPE*)(void*,BOOL,void**);
                                using ExecuteUpdate=void(STDMETHODCALLTYPE*)(void*,void*,BOOL);
                                auto update=reinterpret_cast<Update>(vt(context)[48]);
                                std::vector<unsigned char> upload(40*16,0xee);
                                for(UINT y=0;y<16;++y) std::fill_n(upload.data()+40*y,32,0x51);
                                update(context,texture,0,nullptr,upload.data(),40,640);
                                check(pixels.copy(pixels.context.get(),false,linear.data(),alias,0,32,512,nullptr)==S_OK &&
                                    std::all_of(linear.begin(),linear.end(),[](auto value){return value==0x51;}),
                                    "immediate UpdateSubresource preserves padded source rows through the original tiled layout");
                                void* updater=nullptr;void* update_list=nullptr;
                                check(reinterpret_cast<Deferred>(vt(device)[27])(device,0x60000,&updater)==S_OK && updater,
                                      "UpdateSubresource uses the existing deferred context");
                                if(updater) {
                                    const FLOAT black[]{0,0,0,0};
                                    reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*,const FLOAT*)>(vt(updater)[50])(updater,view,black);
                                    reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,UINT)>(vt(updater)[153])(updater,0);
                                    std::fill(upload.begin(),upload.end(),0xb3);
                                    reinterpret_cast<Update>(vt(updater)[48])(updater,texture,0,nullptr,upload.data(),40,640);
                                    check(reinterpret_cast<FinishUpdate>(vt(updater)[114])(updater,FALSE,&update_list)==S_OK && update_list,
                                          "deferred UpdateSubresource owns a snapshot of the source data");
                                    std::fill(upload.begin(),upload.end(),0);
                                    check(pixels.copy(pixels.context.get(),false,linear.data(),alias,0,32,512,nullptr)==S_OK &&
                                        std::all_of(linear.begin(),linear.end(),[](auto value){return value==0x51;}),
                                        "recording and FinishCommandList do not update destination pixels");
                                    reinterpret_cast<Count>(vt(updater)[2])(updater);
                                    for(UINT pass=0;pass<2 && update_list;++pass) {
                                        reinterpret_cast<ExecuteUpdate>(vt(context)[58])(context,update_list,FALSE);
                                        reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,UINT)>(vt(context)[153])(context,0);
                                        check(pixels.copy(pixels.context.get(),false,linear.data(),alias,0,32,512,nullptr)==S_OK &&
                                            std::all_of(linear.begin(),linear.end(),[](auto value){return value==0xb3;}),
                                            "clear then update executes in order and reuses the owned upload after producer release");
                                    }
                                    if(update_list) reinterpret_cast<Count>(vt(update_list)[2])(update_list);
                                }
                                struct UpdateBufferDesc {D3D11_BUFFER_DESC standard;UINT64 extension;} update_desc{{16,D3D11_USAGE_DEFAULT,D3D11_BIND_VERTEX_BUFFER|D3D11_BIND_UNORDERED_ACCESS,0,D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,4},0};
                                std::array<unsigned char,16> original{};original.fill(0x11);
                                const D3D11_SUBRESOURCE_DATA update_initial{original.data(),0,0};
                                void* update_buffer=nullptr;updater=nullptr;update_list=nullptr;
                                using CreateUpdateBuffer=HRESULT(STDMETHODCALLTYPE*)(void*,const UpdateBufferDesc*,const D3D11_SUBRESOURCE_DATA*,void**);
                                check(reinterpret_cast<CreateUpdateBuffer>(vt(device)[3])(device,&update_desc,&update_initial,&update_buffer)==S_OK && update_buffer,
                                      "partial update creates a real default buffer");
                                if(update_buffer) {
                                    void* buffer_uav=nullptr;void* clear_list=nullptr;void* clear_producer=nullptr;
                                    D3D11_UNORDERED_ACCESS_VIEW_DESC raw{};raw.Format=DXGI_FORMAT_UNKNOWN;raw.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;
                                    raw.Buffer.FirstElement=1;raw.Buffer.NumElements=2;raw.Buffer.Flags=0;
                                    using MakeRaw=HRESULT(STDMETHODCALLTYPE*)(void*,void*,const D3D11_UNORDERED_ACCESS_VIEW_DESC*,void**);
                                    check(reinterpret_cast<MakeRaw>(vt(device)[8])(device,update_buffer,&raw,&buffer_uav)==S_OK && buffer_uav &&
                                        reinterpret_cast<Deferred>(vt(device)[27])(device,0,&clear_producer)==S_OK && clear_producer,
                                        "partial structured UAV clear creates its view and deferred producer");
                                    if(buffer_uav && clear_producer) {
                                        UINT values[4]{0x12345678,3,4,5};
                                        reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*,const UINT*)>(vt(clear_producer)[51])(clear_producer,buffer_uav,values);
                                        check(reinterpret_cast<FinishUpdate>(vt(clear_producer)[114])(clear_producer,FALSE,&clear_list)==S_OK && clear_list,
                                            "partial structured UAV clear captures an executable list");
                                    }
                                    if(clear_producer) reinterpret_cast<Count>(vt(clear_producer)[2])(clear_producer);
                                    if(buffer_uav) reinterpret_cast<Count>(vt(buffer_uav)[2])(buffer_uav);
                                    unsigned char* buffer_bytes=nullptr;
                                    std::memcpy(&buffer_bytes,static_cast<char*>(update_buffer)+48,sizeof buffer_bytes);
                                    check(reinterpret_cast<Deferred>(vt(device)[27])(device,0,&updater)==S_OK && updater,
                                          "partial buffer update creates a tracked deferred context");
                                    if(updater) {
                                        D3D11_BOX box{4,0,0,12,1,1};std::array<unsigned char,8> source{};source.fill(0xa7);
                                        reinterpret_cast<Update>(vt(updater)[48])(updater,update_buffer,0,&box,source.data(),0,0);
                                        check(reinterpret_cast<FinishUpdate>(vt(updater)[114])(updater,FALSE,&update_list)==S_OK && update_list &&
                                              buffer_bytes && !std::memcmp(buffer_bytes,original.data(),original.size()),
                                              "partial buffer update records bytes without changing its destination");
                                        source.fill(0);reinterpret_cast<Count>(vt(updater)[2])(updater);
                                    }
                                    reinterpret_cast<Count>(vt(update_buffer)[2])(update_buffer);
                                    for(UINT pass=0;pass<2 && update_list;++pass) {
                                        reinterpret_cast<ExecuteUpdate>(vt(context)[58])(context,update_list,FALSE);
                                        bool exact_buffer=true;
                                        for(UINT i=0;i<16;++i) exact_buffer=exact_buffer && buffer_bytes[i]==(i>=4 && i<12 ? 0xa7 : 0x11);
                                        check(exact_buffer,"partial buffer list retains the destination, source snapshot and untouched bytes across reuse");
                                        if(clear_list) {
                                            reinterpret_cast<ExecuteUpdate>(vt(context)[58])(context,clear_list,FALSE);
                                            UINT a=0,b=0;std::memcpy(&a,buffer_bytes+4,4);std::memcpy(&b,buffer_bytes+8,4);
                                            check(a==0x12345678 && b==a && buffer_bytes[0]==0x11 && buffer_bytes[3]==0x11 && buffer_bytes[12]==0x11 && buffer_bytes[15]==0x11,
                                                "structured UAV clear touches only its original byte range and repeats the first word without modifying neighboring bytes");
                                        }
                                    }
                                    if(update_list) reinterpret_cast<Count>(vt(update_list)[2])(update_list);
                                    if(clear_list) {
                                        reinterpret_cast<ExecuteUpdate>(vt(context)[58])(context,clear_list,TRUE);
                                        reinterpret_cast<Count>(vt(clear_list)[2])(clear_list);
                                    }
                                }
                                reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*,const FLOAT*)>(vt(context)[50])(context,view,white);
                                void* deferred=nullptr;
                                check(reinterpret_cast<Deferred>(vt(device)[27])(device,0x60000,&deferred)==S_OK && deferred,
                                      "observed deferred mode has its own real context state");
                                if (deferred) {
                                    const std::vector<unsigned char> before(static_cast<unsigned char*>(alias),static_cast<unsigned char*>(alias)+pixels.bytes);
                                    reinterpret_cast<SetTargets>(vt(deferred)[33])(deferred,1,targets,nullptr);
                                    void* deferred_view=reinterpret_cast<void*>(1);void* deferred_depth=reinterpret_cast<void*>(1);
                                    reinterpret_cast<GetTargets>(vt(deferred)[89])(deferred,1,&deferred_view,&deferred_depth);
                                    check(deferred_view==view && !deferred_depth,"tracked deferred OM query returns its retained render target binding");
                                    if(deferred_view) reinterpret_cast<Count>(vt(deferred_view)[2])(deferred_view);
                                    static UINT clear_slot=0;
                                    set_d3d11x_trap_handler([](const char*,UINT slot)->std::uintptr_t{clear_slot=slot;return 0;});
                                    const FLOAT black[]{0,0,0,0};
                                    reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*,const FLOAT*)>(vt(deferred)[50])(deferred,view,black);
                                    reinterpret_cast<void(STDMETHODCALLTYPE*)(void*)>(vt(deferred)[111])(deferred);
                                    set_d3d11x_trap_handler(nullptr);
                                    check(clear_slot==0 && !std::memcmp(before.data(),alias,before.size()),
                                          "deferred clear records without executing or modifying canonical pixels");
                                    reinterpret_cast<void(STDMETHODCALLTYPE*)(void*)>(vt(deferred)[110])(deferred);
                                    check(!std::memcmp(before.data(),alias,before.size()),
                                          "deferred ClearState releases bindings without submitting GPU work or changing physical pixels");
                                    using Finish=HRESULT(STDMETHODCALLTYPE*)(void*,BOOL,void**);
                                    using Execute=void(STDMETHODCALLTYPE*)(void*,void*,BOOL);
                                    void* list=nullptr;
                                    check(reinterpret_cast<Finish>(vt(deferred)[114])(deferred,FALSE,&list)==S_OK && list,
                                          "deferred clear survives ClearState and finishes without executing");
                                    if(list) {
                                        reinterpret_cast<Execute>(vt(context)[58])(context,list,FALSE);
                                        check(pixels.copy(pixels.context.get(),false,linear.data(),alias,0,32,512,nullptr)==S_OK &&
                                            std::all_of(linear.begin(),linear.end(),[](auto v){return v==0;}),
                                            "ExecuteCommandList publishes the recorded clear after its real fence");
                                        reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*,const FLOAT*)>(vt(context)[50])(context,view,white);
                                        reinterpret_cast<Execute>(vt(context)[58])(context,list,TRUE);
                                        check(pixels.copy(pixels.context.get(),false,linear.data(),alias,0,32,512,nullptr)==S_OK &&
                                            std::all_of(linear.begin(),linear.end(),[](auto v){return v==0;}),
                                            "reused clear list sees current resource ownership and clears again");
                                        reinterpret_cast<Count>(vt(list)[2])(list);
                                    }
                                    reinterpret_cast<Count>(vt(deferred)[2])(deferred);
                                }
                                deferred=nullptr;
                                check(reinterpret_cast<Deferred>(vt(device)[27])(device,0,&deferred)==S_OK && deferred,
                                      "binding-only list creates a normal deferred context");
                                if(deferred) {
                                    const ULONG baseline=reinterpret_cast<Count>(vt(view)[1])(view);
                                    reinterpret_cast<Count>(vt(view)[2])(view);
                                    bind_targets(deferred,1,targets,nullptr);
                                    reinterpret_cast<void(STDMETHODCALLTYPE*)(void*)>(vt(deferred)[110])(deferred);
                                    void* list=nullptr;
                                    using Finish=HRESULT(STDMETHODCALLTYPE*)(void*,BOOL,void**);
                                    check(reinterpret_cast<Finish>(vt(deferred)[114])(deferred,FALSE,&list)==S_OK && list,
                                          "binding-only list finishes after unbinding without requiring shaders");
                                    reinterpret_cast<Count>(vt(deferred)[2])(deferred);
                                    check(reinterpret_cast<Count>(vt(view)[1])(view)==baseline+1,
                                          "recorded bindings retain resources after unbinding and producer destruction, even without draws");
                                    reinterpret_cast<Count>(vt(view)[2])(view);
                                    if(list) reinterpret_cast<Count>(vt(list)[2])(list);
                                    check(reinterpret_cast<Count>(vt(view)[1])(view)==baseline,"list release retires binding-only references exactly once");
                                    reinterpret_cast<Count>(vt(view)[2])(view);
                                }
                                const HMODULE compiler=LoadLibraryExW(L"d3dcompiler_47.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
                                const auto compile=compiler ? reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(compiler,"D3DCompile")) : nullptr;
                                const char* sources[]{
                                    "float4 main(uint id:SV_VertexID):SV_Position {float2 p=float2((id<<1)&2,id&2);return float4(p*float2(2,-2)+float2(-1,1),0,1);}",
                                    "float4 main():SV_Target{return float4(.25,0,0,1);}",
                                    "float4 main():SV_Target{return float4(.75,0,0,1);}",
                                    "float4 main(float2 p:POSITION,float2 u:TEXCOORD0):SV_Position{return float4(p+u,0,1);}",
                                    "float4 main(float2 p:POSITION,uint id:SV_VertexID):SV_Position{return float4(float2(id&1,id>>1)*float2(2,-2)+float2(-1,1)+p,0,1);}"};
                                void* shaders[5]{};void* empty_layout=nullptr;void* vertex_layout=nullptr;void* vertex_buffer=nullptr;bool ready=compile!=nullptr;
                                using CreateShader=HRESULT(STDMETHODCALLTYPE*)(void*,const void*,SIZE_T,void*,void**);
                                if(compile && storage_format==DXGI_FORMAT_R8_TYPELESS) {
                                    NativeUavClear native_clear;
                                    check(SUCCEEDED(native_clear.initialize(argc==5)),"independent native typed-clear control initializes on the same backend");
                                    if(!native_clear.gpu)return 1;
                                    struct TypedFormat {DXGI_FORMAT format;UINT bytes,channels,kind,bits;}; // uint, sint, float, UNORM, raw, SNORM.
                                    const TypedFormat formats[]{
                                        {DXGI_FORMAT_R32_UINT,4,1,0,32},{DXGI_FORMAT_R16_UINT,2,1,0,16},{DXGI_FORMAT_R8_UINT,1,1,0,8},
                                        {DXGI_FORMAT_R32_SINT,4,1,1,32},{DXGI_FORMAT_R16_SINT,2,1,1,16},{DXGI_FORMAT_R8_SINT,1,1,1,8},
                                        {DXGI_FORMAT_R32G32B32A32_UINT,16,4,0,32},{DXGI_FORMAT_R16G16B16A16_UINT,8,4,0,16},{DXGI_FORMAT_R10G10B10A2_UINT,4,4,0,10},
                                        {DXGI_FORMAT_R32_FLOAT,4,1,2,32},{DXGI_FORMAT_R16_FLOAT,2,1,2,16},{DXGI_FORMAT_R16G16B16A16_FLOAT,8,4,2,16},
                                        {DXGI_FORMAT_R32G32B32A32_FLOAT,16,4,2,32},{DXGI_FORMAT_R8_UNORM,1,1,3,8},{DXGI_FORMAT_R8G8_UNORM,2,2,3,8},
                                        {DXGI_FORMAT_R8G8B8A8_UNORM,4,4,3,8},{DXGI_FORMAT_R16_UNORM,2,1,3,16},{DXGI_FORMAT_R16G16_UNORM,4,2,3,16},
                                        {DXGI_FORMAT_R16G16B16A16_UNORM,8,4,3,16},{DXGI_FORMAT_R10G10B10A2_UNORM,4,4,3,10},
                                        {DXGI_FORMAT_R32_TYPELESS,4,1,4,32},
                                        {DXGI_FORMAT_R8_SNORM,1,1,5,8},{DXGI_FORMAT_R8G8_SNORM,2,2,5,8},{DXGI_FORMAT_R8G8B8A8_SNORM,4,4,5,8},
                                        {DXGI_FORMAT_R16_SNORM,2,1,5,16},{DXGI_FORMAT_R16G16_SNORM,4,2,5,16},{DXGI_FORMAT_R16G16B16A16_SNORM,8,4,5,16}};
                                    using Uav=HRESULT(STDMETHODCALLTYPE*)(void*,void*,const D3D11_UNORDERED_ACCESS_VIEW_DESC*,void**);
                                    using Srv=HRESULT(STDMETHODCALLTYPE*)(void*,void*,const D3D11_SHADER_RESOURCE_VIEW_DESC*,void**);
                                    using Clear=void(STDMETHODCALLTYPE*)(void*,void*,const UINT*);
                                    using Dispatch=void(STDMETHODCALLTYPE*)(void*,UINT,UINT,UINT);
                                    using Defer=HRESULT(STDMETHODCALLTYPE*)(void*,UINT,void**);using Finish=HRESULT(STDMETHODCALLTYPE*)(void*,BOOL,void**);
                                    struct TypedBufferDesc {D3D11_BUFFER_DESC standard;UINT64 extension;};
                                    using Buffer=HRESULT(STDMETHODCALLTYPE*)(void*,const TypedBufferDesc*,const D3D11_SUBRESOURCE_DATA*,void**);
                                    for(const auto& f:formats) {
                                        const bool raw=f.kind==4;const UINT first=raw ? 4:2;void* objects[3]{};
                                        const UINT misc=raw || f.format==DXGI_FORMAT_R16_UINT ? D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS:0;
                                        const TypedBufferDesc desc{{f.bytes*10+3,D3D11_USAGE_DEFAULT,D3D11_BIND_UNORDERED_ACCESS|D3D11_BIND_SHADER_RESOURCE,0,misc,0},0};
                                        std::vector<BYTE> guards(desc.standard.ByteWidth,0xa5);const D3D11_SUBRESOURCE_DATA initial{guards.data(),0,0};
                                        const auto buffer=reinterpret_cast<Buffer>(vt(device)[3]);const auto uav=reinterpret_cast<Uav>(vt(device)[8]);
                                        check(buffer(device,&desc,&initial,&objects[0])==S_OK && objects[0],"typed UAV fixture creates a real unstructured buffer with trailing byte guards");if(!objects[0])return 1;
                                        D3D11_UNORDERED_ACCESS_VIEW_DESC view{};view.Format=f.format;view.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;
                                        view.Buffer={first,4,raw ? UINT(D3D11_BUFFER_UAV_FLAG_RAW):0};
                                        const auto created=uav(device,objects[0],&view,&objects[1]);
                                        if(FAILED(created))std::fprintf(stderr,"typed UAV format=%u creation=0x%08lX\n",f.format,created);
                                        check(created==S_OK && objects[1],"typed/raw UAV creates a native sliced buffer descriptor");if(!objects[1])return 1;
                                        check(uav(device,objects[0],&view,nullptr)==S_FALSE,"typed UAV validation does not publish ownership");
                                        for(UINT invalid=0;invalid<4;++invalid) {
                                            auto bad=view;if(invalid==0)bad.Buffer.FirstElement=UINT_MAX;if(invalid==1)bad.Buffer.NumElements=0;
                                            if(invalid==2)bad.Buffer.Flags|=D3D11_BUFFER_UAV_FLAG_COUNTER;if(invalid==3)bad.ViewDimension=D3D11_UAV_DIMENSION_TEXTURE2D;
                                            void* refused=reinterpret_cast<void*>(1);const auto rejected=uav(device,objects[0],&bad,&refused);
                                            check(FAILED(rejected) && !refused,"typed UAV rejects range overflow, zero elements, counters and wrong dimensions without publishing a view");
                                        }
                                        void* absent=reinterpret_cast<void*>(1);check(uav(device,objects[0],nullptr,&absent)==E_INVALIDARG && !absent,"unstructured UAV requires the reference's explicit description");
                                        D3D11_UNORDERED_ACCESS_VIEW_DESC observed{};reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,D3D11_UNORDERED_ACCESS_VIEW_DESC*)>(vt(objects[1])[9])(objects[1],&observed);
                                        check(!std::memcmp(&observed,&view,sizeof view),"typed UAV GetDesc preserves first element, count, format and flags");
                                        D3D11_SHADER_RESOURCE_VIEW_DESC read{};read.Format=f.format;read.ViewDimension=raw ? D3D11_SRV_DIMENSION_BUFFEREX:D3D11_SRV_DIMENSION_BUFFER;
                                        read.BufferEx={first,4,raw ? UINT(D3D11_BUFFEREX_SRV_FLAG_RAW):0};void* srv=nullptr;
                                        check(reinterpret_cast<Srv>(vt(device)[7])(device,objects[0],&read,&srv)==S_OK && srv,"typed UAV has a matching original-format SRV descriptor");
                                        if(srv) {
                                            std::array<UINT,4> a{},b{};std::memcpy(a.data(),static_cast<BYTE*>(objects[1])+48,sizeof a);std::memcpy(b.data(),static_cast<BYTE*>(srv)+48,sizeof b);
                                            check(a==b,"typed buffer UAV and SRV SRDs share the native address, stride, range, numeric encoding and selectors");
                                            reinterpret_cast<Count>(vt(srv)[2])(srv);
                                        }
                                        const bool floating=f.kind==2 || f.kind==3 || f.kind==5;
                                        std::string type=f.kind==1 ? "int":"uint";if(floating)type="float";if(f.channels>1)type+=std::to_string(f.channels);
                                        std::string body;
                                        if(raw)body="uint value=u.Load(id.x*4);u.Store(id.x*4,value+id.x+1);";
                                        else if(floating)body="u[id.x]+=.125;";
                                        else if(f.kind==1)body="u[id.x]-=int(id.x)+1;";
                                        else if(f.channels==4)body=f.bits==10 ? "u[id.x]+=uint4(1,2,3,0)+id.x;":"u[id.x]+=uint4(1,2,3,4)+id.x;";
                                        else body="u[id.x]+=id.x+1;";
                                        const std::string source=(raw ? "RWByteAddressBuffer":"RWBuffer<"+type+">")+std::string(" u:register(u0);[numthreads(1,1,1)]void main(uint3 id:SV_DispatchThreadID){")+body+"}";
                                        Microsoft::WRL::ComPtr<ID3DBlob> code;
                                        check(SUCCEEDED(compile(source.data(),source.size(),nullptr,nullptr,nullptr,"main","cs_5_0",0,0,&code,nullptr)) &&
                                            reinterpret_cast<CreateShader>(vt(device)[18])(device,code->GetBufferPointer(),code->GetBufferSize(),nullptr,&objects[2])==S_OK && objects[2],"typed UAV shader creates a real native typed-load/store compute pipeline");if(!objects[2])return 1;
                                        BYTE* canonical=nullptr;std::memcpy(&canonical,static_cast<BYTE*>(objects[0])+48,sizeof canonical);
                                        for(const UINT slot:{51u,52u})reinterpret_cast<Clear>(vt(context)[slot])(context,objects[1],nullptr);
                                        check(!std::memcmp(canonical,guards.data(),guards.size()),"reference null clear values preserve every buffer byte");
                                        FLOAT color[]{.25f,.5f,.75f,1};
                                        std::vector<BYTE> clear_image;
                                        if(f.kind==5) {
                                            color[0]=-.5f;color[1]=.5f;color[2]=-2;color[3]=2;
                                        }
                                        std::array<UINT,4> clear{0x12345678,7,8,9};if(f.kind==0 && f.bits<32)clear[0]=55;
                                        if(f.bits==10)clear[3]=0; // Keep the compute step's alpha in its original 0..3 domain.
                                        if(f.kind==1)clear={UINT(-2),7,8,9};if(floating)std::memcpy(clear.data(),color,sizeof color);
                                        if(FAILED(native_clear.clear(f.format,f.bytes,clear,floating,clear_image,raw)))return 1;
                                        const auto verify=[&](bool computed) {
                                            auto expected=guards;
                                            std::memcpy(expected.data()+first*f.bytes,clear_image.data(),clear_image.size());
                                            for(UINT i=0;computed && i<4;++i) {
                                                UINT packed=0;
                                                for(UINT c=0;c<f.channels;++c) {
                                                    const auto* at=clear_image.data()+i*f.bytes+c*(f.bytes/f.channels);
                                                    UINT value=0;
                                                    if(f.bits==10){std::memcpy(&value,clear_image.data()+i*f.bytes,4);value=(value>>(c==3 ? 30:c*10))&(c==3 ? 3:1023);}
                                                    else std::memcpy(&value,at,f.bytes/f.channels);
                                                    if(f.kind==0 || raw)value+=(f.bits==10 && c==3 ? 0:c+1)+i;
                                                    else if(f.kind==1) {
                                                        INT q=0;if(f.bits==8){std::int8_t v=0;std::memcpy(&v,at,1);q=v;}else if(f.bits==16){std::int16_t v=0;std::memcpy(&v,at,2);q=v;}else std::memcpy(&q,at,4);
                                                        value=UINT(q)-i-1;
                                                    }
                                                    else if(f.kind==2) {
                                                        if(f.bits==32){const FLOAT v=color[c]+(computed ? .125f:0);std::memcpy(&value,&v,4);}
                                                        else {const UINT before[]{0x3400,0x3800,0x3a00,0x3c00},after[]{0x3600,0x3900,0x3b00,0x3c80};value=computed ? after[c]:before[c];}
                                                    }else if(f.kind==5) {
                                                        INT q=0;
                                                        if(f.bits==8){std::int8_t v=0;std::memcpy(&v,at,1);q=v;}else{std::int16_t v=0;std::memcpy(&v,at,2);q=v;}
                                                        const INT max=(1<<(f.bits-1))-1;
                                                        if(computed)q=std::clamp(q+(1<<(f.bits-4)),-max,max);
                                                        value=UINT(q);
                                                    }
                                                    else {
                                                        const UINT bits=f.bits==10 && c==3 ? 2:f.bits,max=(1u<<bits)-1;
                                                        value=(std::min)(max,value+UINT(.125f*FLOAT(max)+.5f));
                                                    }
                                                    if(f.bits==10)packed|=value<<(c==3 ? 30:c*10);
                                                    else std::memcpy(expected.data()+(first+i)*f.bytes+c*(f.bytes/f.channels),&value,f.bytes/f.channels);
                                                }
                                                if(f.bits==10)std::memcpy(expected.data()+(first+i)*f.bytes,&packed,4);
                                            }
                                            const bool exact=!std::memcmp(canonical,expected.data(),expected.size());
                                            if(!exact){const auto at=SIZE_T(std::mismatch(expected.begin(),expected.end(),canonical).first-expected.begin());std::fprintf(stderr,"typed UAV format=%u compute=%u byte=%zu actual=%u expected=%u\n",f.format,UINT(computed),at,canonical[at],expected[at]);}
                                            check(exact,"typed/raw UAV clear and compute preserve exact selected elements, all components and every surrounding byte");
                                        };
                                        const UINT clear_slot=floating ? 52:51;
                                        {
                                            const auto saved_image=clear_image;
                                            const std::array<UINT,4> probes[]{{0x12345678,7,8,9},{0x3e800000,0x3f000000,0x3f400000,0x3f800000}};
                                            for(const auto& values:probes)for(const UINT slot:{51u,52u}) {
                                                if(FAILED(native_clear.clear(f.format,f.bytes,values,slot==52,clear_image,raw)))return 1;
                                                reinterpret_cast<Clear>(vt(context)[slot])(context,objects[1],values.data());verify(false);
                                            }
                                            clear_image=saved_image;
                                        }
                                        if(f.kind==5) {
                                            const FLOAT inf=std::numeric_limits<FLOAT>::infinity(),nan=std::numeric_limits<FLOAT>::quiet_NaN();
                                            const FLOAT cases[]{-inf,-2,-1,-.5f,-.25f,-0.f,0,.25f,.5f,1,2,inf,nan};
                                            const auto saved_clear=clear;const auto saved_image=clear_image;
                                            for(const FLOAT probe:cases) {
                                                UINT bits=0;std::memcpy(&bits,&probe,4);clear.fill(bits);
                                                for(const UINT slot:{51u,52u}) {
                                                    if(FAILED(native_clear.clear(f.format,f.bytes,clear,slot==52,clear_image)))return 1;
                                                    reinterpret_cast<Clear>(vt(context)[slot])(context,objects[1],clear.data());verify(false);
                                                }
                                            }
                                            clear=saved_clear;clear_image=saved_image;
                                            const std::array<UINT,4> integer_values{0x12345678,7,8,9};
                                            for(const auto values:{integer_values,std::array<UINT,4>{UINT(-2),0x8001,0x7fff,UINT_MAX}}) {
                                                if(FAILED(native_clear.clear(f.format,f.bytes,values,false,clear_image)))return 1;
                                                reinterpret_cast<Clear>(vt(context)[51])(context,objects[1],values.data());verify(false);
                                            }
                                            if(FAILED(native_clear.clear(f.format,f.bytes,integer_values,false,clear_image)))return 1;
                                            void* integer_producer=nullptr;void* integer_list=nullptr;
                                            const auto before_record=std::vector<BYTE>(canonical,canonical+guards.size());
                                            check(reinterpret_cast<Defer>(vt(device)[27])(device,0x60000,&integer_producer)==S_OK && integer_producer,"SNORM Uint clear creates a deferred producer");
                                            if(!integer_producer)return 1;
                                            reinterpret_cast<Clear>(vt(integer_producer)[51])(integer_producer,objects[1],integer_values.data());
                                            check(reinterpret_cast<Finish>(vt(integer_producer)[114])(integer_producer,FALSE,&integer_list)==S_OK && integer_list &&
                                                !std::memcmp(canonical,before_record.data(),before_record.size()),"SNORM Uint recording preserves the distinct operation and leaves bytes unchanged");
                                            reinterpret_cast<Count>(vt(integer_producer)[2])(integer_producer);
                                            if(!integer_list)return 1;
                                            for(UINT pass=0;pass<2;++pass) {
                                                std::memcpy(canonical,guards.data(),guards.size());
                                                reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*,BOOL)>(vt(context)[58])(context,integer_list,TRUE);verify(false);
                                            }
                                            reinterpret_cast<Count>(vt(integer_list)[2])(integer_list);clear_image=saved_image;
                                        }
                                        reinterpret_cast<Clear>(vt(context)[clear_slot])(context,objects[1],clear.data());verify(false);
                                        if(!raw) {
                                            std::vector<BYTE> reference_bytes;
                                            if(FAILED(native_clear.clear(f.format,f.bytes,clear,floating,reference_bytes)))return 1;
                                            const bool same=!std::memcmp(canonical+first*f.bytes,reference_bytes.data(),reference_bytes.size());
                                            if(!same)std::fprintf(stderr,"reference clear mismatch format=%u\n",f.format);
                                            check(same,"typed buffer clear matches the supplied reference native operation");
                                        }
                                        const auto bind=[&](void* target) {
                                            reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*)>(vt(target)[69])(target,objects[2]);
                                            reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,UINT,UINT,void* const*)>(vt(target)[68])(target,0,1,&objects[1]);
                                        };
                                        bind(context);reinterpret_cast<Dispatch>(vt(context)[41])(context,4,1,1);verify(true);
                                        reinterpret_cast<void(STDMETHODCALLTYPE*)(void*)>(vt(context)[110])(context);std::memcpy(canonical,guards.data(),guards.size());
                                        void* producer=nullptr;void* list=nullptr;
                                        if(reinterpret_cast<Defer>(vt(device)[27])(device,0,&producer)==S_OK && producer) {
                                            reinterpret_cast<Clear>(vt(producer)[clear_slot])(producer,objects[1],clear.data());
                                            for(const UINT slot:{51u,52u})reinterpret_cast<Clear>(vt(producer)[slot])(producer,objects[1],nullptr);
                                            bind(producer);
                                            reinterpret_cast<Dispatch>(vt(producer)[41])(producer,4,1,1);
                                            check(reinterpret_cast<Finish>(vt(producer)[114])(producer,FALSE,&list)==S_OK && list && !std::memcmp(canonical,guards.data(),guards.size()),"typed UAV recording retains resources without clearing or dispatching");
                                            reinterpret_cast<Count>(vt(producer)[2])(producer);
                                        }else check(false,"typed UAV creates a deferred producer");
                                        for(auto* object:objects)reinterpret_cast<Count>(vt(object)[2])(object);
                                        if(list) {
                                            for(UINT pass=0;pass<2;++pass) {std::memcpy(canonical,guards.data(),guards.size());reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*,BOOL)>(vt(context)[58])(context,list,TRUE);verify(true);}
                                            reinterpret_cast<Count>(vt(list)[2])(list);
                                        }
                                    }
                                    for(const UINT stride:{4u,12u,16u})for(const UINT slot:{51u,52u}) {
                                        const TypedBufferDesc desc{{10*stride,D3D11_USAGE_DEFAULT,D3D11_BIND_UNORDERED_ACCESS,0,D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,stride},0};
                                        const std::vector<BYTE> guards(desc.standard.ByteWidth,0xa5);const D3D11_SUBRESOURCE_DATA initial{guards.data(),0,0};
                                        void* resource=nullptr;void* target=nullptr;
                                        check(reinterpret_cast<Buffer>(vt(device)[3])(device,&desc,&initial,&resource)==S_OK && resource,"structured clear creates shared storage with a verified DWORD stride");
                                        if(!resource)return 1;
                                        D3D11_UNORDERED_ACCESS_VIEW_DESC view{};view.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;view.Buffer={2,4,0};
                                        check(reinterpret_cast<Uav>(vt(device)[8])(device,resource,&view,&target)==S_OK && target,"structured clear creates its sliced unknown-format view");
                                        if(!target)return 1;
                                        BYTE* canonical=nullptr;std::memcpy(&canonical,static_cast<BYTE*>(resource)+48,sizeof canonical);
                                        const std::array<UINT,4> values=slot==51 ? std::array<UINT,4>{0x12345678,7,8,9}:std::array<UINT,4>{0x3e800000,0x3f000000,0x3f400000,0x3f800000};
                                        std::vector<BYTE> native;
                                        if(FAILED(native_clear.clear(DXGI_FORMAT_R32_UINT,4,values,slot==52,native,false,stride)))return 1;
                                        auto expected=guards;std::memcpy(expected.data()+2*stride,native.data(),native.size());
                                        const auto verify=[&]{check(!std::memcmp(canonical,expected.data(),expected.size()),"structured Uint/Float clear matches the reference DWORD alias and preserves both guards");};
                                        reinterpret_cast<Clear>(vt(context)[slot])(context,target,values.data());verify();
                                        std::memcpy(canonical,guards.data(),guards.size());
                                        void* producer=nullptr;void* list=nullptr;
                                        check(reinterpret_cast<Defer>(vt(device)[27])(device,0x60000,&producer)==S_OK && producer,"structured clear creates a deferred producer");
                                        if(!producer)return 1;
                                        reinterpret_cast<Clear>(vt(producer)[slot])(producer,target,values.data());
                                        check(reinterpret_cast<Finish>(vt(producer)[114])(producer,FALSE,&list)==S_OK && list && !std::memcmp(canonical,guards.data(),guards.size()),"structured clear records without executing");
                                        reinterpret_cast<Count>(vt(producer)[2])(producer);reinterpret_cast<Count>(vt(target)[2])(target);reinterpret_cast<Count>(vt(resource)[2])(resource);
                                        if(!list)return 1;
                                        for(UINT pass=0;pass<2;++pass){std::memcpy(canonical,guards.data(),guards.size());reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*,BOOL)>(vt(context)[58])(context,list,TRUE);verify();}
                                        reinterpret_cast<Count>(vt(list)[2])(list);
                                    }
                                }
                                for(UINT i=0;i<5 && ready;++i) {
                                    Microsoft::WRL::ComPtr<ID3DBlob> code;
                                    const bool vertex=i==0 || i==3 || i==4;
                                    ready=SUCCEEDED(compile(sources[i],std::strlen(sources[i]),nullptr,nullptr,nullptr,"main",vertex ? "vs_4_0" : "ps_4_0",0,0,&code,nullptr));
                                    if(ready) ready=reinterpret_cast<CreateShader>(vt(device)[vertex ? 12 : 15])(device,code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shaders[i])==S_OK;
                                    if(ready && vertex && i != 4) {
                                        using CreateLayout=HRESULT(STDMETHODCALLTYPE*)(void*,const D3D12_INPUT_ELEMENT_DESC*,UINT,const void*,SIZE_T,void**);
                                        const D3D12_INPUT_ELEMENT_DESC elements[]{{"POSITION",0,DXGI_FORMAT_R32G32_FLOAT,0,0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0},
                                            {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,D3D12_APPEND_ALIGNED_ELEMENT,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0}};
                                        ready=reinterpret_cast<CreateLayout>(vt(device)[11])(device,i ? elements : nullptr,i ? 2 : 0,code->GetBufferPointer(),code->GetBufferSize(),i ? &vertex_layout : &empty_layout)==S_OK;
                                    }
                                }
                                const FLOAT vertices[]{99,99,0,0,-1,1,0,0,3,1,0,0,-1,-3,0,0};
                                if(ready) {
                                    struct VertexDesc { D3D11_BUFFER_DESC standard;UINT64 extension; } vertex_desc{{sizeof vertices,D3D11_USAGE_IMMUTABLE,D3D11_BIND_VERTEX_BUFFER,0,0,40},0};
                                    const D3D11_SUBRESOURCE_DATA initial{vertices,0,0};
                                    using CreateBuffer=HRESULT(STDMETHODCALLTYPE*)(void*,const VertexDesc*,const D3D11_SUBRESOURCE_DATA*,void**);
                                    ready=reinterpret_cast<CreateBuffer>(vt(device)[3])(device,&vertex_desc,&initial,&vertex_buffer)==S_OK;
                                    if(ready) {
                                        VertexDesc observed{};
                                        reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,VertexDesc*)>(vt(vertex_buffer)[12])(vertex_buffer,&observed);
                                        check(observed.standard.StructureByteStride==40 && observed.standard.Usage==D3D11_USAGE_IMMUTABLE,
                                            "nonstructured vertex buffer retains its stride hint while native IA uses the bound stride");
                                    }
                                }
                                check(ready,"legacy procedural draw creates real Shader Model 4.0 VS/PS objects");
                                if(ready) {
                                    bind_targets(context,1,targets,nullptr);
                                    using Draw=void(STDMETHODCALLTYPE*)(void*,UINT,UINT);
                                    const auto draw=reinterpret_cast<Draw>(vt(context)[14]);
                                    const D3D11_VIEWPORT viewport{0,0,32,16,0,1};
                                    reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,UINT,const D3D11_VIEWPORT*)>(vt(context)[44])(context,1,&viewport);
                                    const UINT64 topology=0x000009ffc0009e00ull;
                                    std::memcpy(static_cast<char*>(context)+0x170,&topology,sizeof topology);
                                    static_cast<BYTE*>(context)[0x390]=1; // Exercise the legacy inline fallback independently of the reference dispatch mode.
                                    std::memcpy(static_cast<char*>(context)+0x180,&shaders[0],sizeof(void*));
                                    std::memcpy(static_cast<char*>(context)+0x178,&empty_layout,sizeof empty_layout);
                                    {
                                        auto* queued_base=static_cast<BYTE*>(reserve(nullptr,65536,0x30002000,PAGE_READWRITE));
                                        if(!queued_base || pixels.bytes>65536 || reserve(queued_base,65536,0x30001000,0x10004)!=queued_base)return 1;
                                        std::memset(queued_base,0x5a,65536);
                                        void* queued_texture=nullptr;void* queued_view=nullptr;
                                        using PlaceTracked=HRESULT(STDMETHODCALLTYPE*)(void*,const Desc*,UINT,UINT,void*,void**);
                                        check(reinterpret_cast<PlaceTracked>(vt(device)[64])(device,&color,pixels.tile_mode,0,queued_base,&queued_texture)==S_OK && queued_texture &&
                                            factory(device,queued_texture,&rtv,&queued_view)==S_OK && queued_view,
                                            "queued draw creates a tracked graphics texture with an original-XG layout");
                                        if(!queued_texture || !queued_view)return 1;
                                        bind_targets(context,1,&queued_view,nullptr);
                                        for(UINT pass=0;pass<2;++pass) {
                                            std::memcpy(static_cast<char*>(context)+0x188,&shaders[1+pass],sizeof(void*));
                                            draw(context,3,0);
                                            const auto copied=pixels.copy(pixels.context.get(),false,linear.data(),queued_base,0,32,512,nullptr);
                                            check(SUCCEEDED(copied) && std::all_of(linear.begin(),linear.end(),[&](auto value){return value==(pass ? 191:64);}),
                                                "CPU access retires a queued draw before original-XG pixel conversion");
                                        }
                                        draw(context,3,0);
                                        bind_targets(context,1,targets,nullptr);
                                        reinterpret_cast<Count>(vt(queued_view)[2])(queued_view);
                                        reinterpret_cast<Count>(vt(queued_texture)[2])(queued_texture);
                                        reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,UINT)>(vt(context)[153])(context,0);
                                        check(pixels.copy(pixels.context.get(),false,linear.data(),queued_base,0,32,512,nullptr)==S_OK &&
                                            std::all_of(linear.begin(),linear.end(),[](auto value){return value==191;}),
                                            "queued native resources survive all public view and texture releases until GPU idle");
                                        check(release_virtual(queued_base,0,MEM_RELEASE),"GPU idle retires retained storage before graphics reservation release");
                                    }
                                    for(UINT pass=0;pass<2;++pass) {
                                        std::memcpy(static_cast<char*>(context)+0x188,&shaders[1+pass],sizeof(void*));
                                        draw(context,3,0);
                                        const HRESULT copied=pixels.copy(pixels.context.get(),false,linear.data(),alias,0,32,512,nullptr);
                                        check(SUCCEEDED(copied) && std::all_of(linear.begin(),linear.end(),[&](auto value){return value==(pass ? 191 : 64);}),
                                              "procedural draw publishes all pixels after its fence and refreshes inline shader changes");
                                    }
                                    check(reinterpret_cast<Count>(vt(shaders[1])[1])(shaders[1])==2,"inline PS replacement releases the prior shader binding");
                                    reinterpret_cast<Count>(vt(shaders[1])[2])(shaders[1]);
                                    for(UINT epoch=0;epoch<3;++epoch) {
                                        const char* source="struct V{float4 p:SV_Position;nointerpolation float r:COLOR;}; V vs(float r:COLOR,float2 p:POSITION){V o;o.p=float4(p,0,1);o.r=r;return o;} float4 ps(V v):SV_Target{return float4(v.r*GAIN,0,0,1);}";
                                        const D3D_SHADER_MACRO defines[]{{"GAIN",epoch==2 ? "0.5":"1"},{nullptr,nullptr}};
                                        const BYTE multiple=epoch==2 ? 32:64,single=epoch==2 ? 96:191;
                                        void* objects[6]{};bool made=true;
                                        for(UINT stage=0;stage<2 && made;++stage) {
                                            Microsoft::WRL::ComPtr<ID3DBlob> code;
                                            made=SUCCEEDED(compile(source,std::strlen(source),nullptr,defines,nullptr,stage ? "ps":"vs",stage ? "ps_4_0":"vs_4_0",0,0,&code,nullptr)) &&
                                                reinterpret_cast<CreateShader>(vt(device)[stage ? 15:12])(device,code->GetBufferPointer(),code->GetBufferSize(),nullptr,&objects[stage])==S_OK;
                                            if(made && !stage) {
                                                const D3D12_INPUT_ELEMENT_DESC instance[]{{"COLOR",0,DXGI_FORMAT_R32_FLOAT,0,0,D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA,1},{"POSITION",0,DXGI_FORMAT_R32G32_FLOAT,1,0,D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA,0}};
                                                using Layout=HRESULT(STDMETHODCALLTYPE*)(void*,const D3D12_INPUT_ELEMENT_DESC*,UINT,const void*,SIZE_T,void**);
                                                made=reinterpret_cast<Layout>(vt(device)[11])(device,instance,2,code->GetBufferPointer(),code->GetBufferSize(),&objects[2])==S_OK;
                                            }
                                        }
                                        const FLOAT colors[]{1,1,1,.125f,.25f,.5f,.75f,1};const UINT16 indices[]{99,6,7,8,99};
                                        const FLOAT positions[]{99,99,99,99,99,99,99,99,99,99,-1,1,3,1,-1,-3};
                                        struct BufferDesc {D3D11_BUFFER_DESC standard;UINT64 extension;};
                                        using BufferFactory=HRESULT(STDMETHODCALLTYPE*)(void*,const BufferDesc*,const D3D11_SUBRESOURCE_DATA*,void**);
                                        for(UINT i=0;i<3 && made;++i) {
                                            BufferDesc desc{{i==2 ? UINT(sizeof positions):i==1 ? UINT(sizeof indices):UINT(sizeof colors),D3D11_USAGE_IMMUTABLE,i==1 ? UINT(D3D11_BIND_INDEX_BUFFER):UINT(D3D11_BIND_VERTEX_BUFFER),0,0,0},0};
                                            const D3D11_SUBRESOURCE_DATA initial{i==2 ? static_cast<const void*>(positions):i==1 ? static_cast<const void*>(indices):static_cast<const void*>(colors),0,0};
                                            made=reinterpret_cast<BufferFactory>(vt(device)[3])(device,&desc,&initial,&objects[3+i])==S_OK;
                                        }
                                        check(made,"reference instanced draw fixture creates per-instance input, indexed vertices and original-ABI shaders");
                                        if(made) {
                                            using Vertices=void(STDMETHODCALLTYPE*)(void*,UINT,UINT,void* const*,const UINT*,const UINT*);
                                            using Indices=void(STDMETHODCALLTYPE*)(void*,UINT,void*,UINT);
                                            using Instanced=void(STDMETHODCALLTYPE*)(void*,UINT,UINT64,UINT);
                                            using IndexedInstanced=void(STDMETHODCALLTYPE*)(void*,UINT64,UINT64,UINT);
                                            const auto bind=[&](void* target) {
                                                bind_targets(target,1,targets,nullptr);
                                                reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,UINT,const D3D11_VIEWPORT*)>(vt(target)[44])(target,1,&viewport);
                                                std::memcpy(static_cast<char*>(target)+0x170,&topology,sizeof topology);static_cast<BYTE*>(target)[0x390]=1;
                                                std::memcpy(static_cast<char*>(target)+0x180,&objects[0],sizeof(void*));std::memcpy(static_cast<char*>(target)+0x188,&objects[1],sizeof(void*));
                                                std::memcpy(static_cast<char*>(target)+0x178,&objects[2],sizeof(void*));
                                                const UINT strides[]{4,8},offsets[]{0,0};void* vertices[]{objects[3],objects[5]};
                                                reinterpret_cast<Vertices>(vt(target)[19])(target,0,2,vertices,strides,offsets);
                                                reinterpret_cast<Indices>(vt(target)[20])(target,0,objects[4],0);
                                            };
                                            const FLOAT black[]{0,0,0,0};
                                            const auto exact=[&](BYTE value) {
                                                const auto copied=pixels.copy(pixels.context.get(),false,linear.data(),alias,0,32,512,nullptr);
                                                const bool matches=copied==S_OK && std::all_of(linear.begin(),linear.end(),[&](BYTE pixel){return pixel==value;});
                                                if(!matches){const auto bounds=std::minmax_element(linear.begin(),linear.end());std::fprintf(stderr,"instanced pixels expected=%u first=%u min=%u max=%u copy=0x%08lX\n",value,linear[0],*bounds.first,*bounds.second,copied);}
                                                return matches;
                                            };
                                            bind(context);
                                            for(UINT indexed=0;indexed<2;++indexed) {
                                                reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*,const FLOAT*)>(vt(context)[50])(context,view,black);
                                                if(indexed)reinterpret_cast<IndexedInstanced>(vt(context)[21])(context,(UINT64(3)<<32)|1,(UINT64(3)<<32)|UINT(-1),2);
                                                else reinterpret_cast<Instanced>(vt(context)[22])(context,3,(UINT64(3)<<32)|5,2);
                                                check(exact(multiple),"instanced draws preserve packed starts and input addressing across shader recreation and changed shader contents");
                                            }
                                            {
                                                using Indirect=void(STDMETHODCALLTYPE*)(void*,void*,UINT);
                                                using ComputeBind=void(STDMETHODCALLTYPE*)(void*,UINT,UINT,void* const*);
                                                using Dispatch=void(STDMETHODCALLTYPE*)(void*,UINT,UINT,UINT);
                                                std::array<UINT,32> words{};words[0]=words[1]=words[2]=words[3]=words[8]=words[9]=words[15]=0x6d6d6d6d;
                                                words[4]=3;words[5]=2;words[6]=5;words[7]=3;
                                                words[10]=3;words[11]=2;words[12]=1;words[13]=UINT(-1);words[14]=3;
                                                words[20]=4;words[21]=2;words[22]=1;
                                                void* indirect_objects[5]{}; // Argument buffer/UAV, argument writer, fill shader, target UAV.
                                                BufferDesc args_desc{{sizeof words,D3D11_USAGE_DEFAULT,D3D11_BIND_UNORDERED_ACCESS,0,D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS|D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS,0},0};
                                                const D3D11_SUBRESOURCE_DATA args_data{words.data(),0,0};
                                                bool indirect_ready=reinterpret_cast<BufferFactory>(vt(device)[3])(device,&args_desc,&args_data,&indirect_objects[0])==S_OK;
                                                using Uav=HRESULT(STDMETHODCALLTYPE*)(void*,void*,const D3D11_UNORDERED_ACCESS_VIEW_DESC*,void**);
                                                D3D11_UNORDERED_ACCESS_VIEW_DESC raw{};raw.Format=DXGI_FORMAT_R32_TYPELESS;raw.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;raw.Buffer.NumElements=32;raw.Buffer.Flags=D3D11_BUFFER_UAV_FLAG_RAW;
                                                if(indirect_ready)indirect_ready=reinterpret_cast<Uav>(vt(device)[8])(device,indirect_objects[0],&raw,&indirect_objects[1])==S_OK;
                                                const char* compute[]{
                                                    "RWByteAddressBuffer args:register(u0);[numthreads(1,1,1)] void main(){args.Store4(16,uint4(3,1,5,6));args.Store4(40,uint4(3,1,1,0xffffffff));args.Store(56,6);args.Store3(80,uint3(0,2,1));}",
                                                    "RWTexture2D<float> output:register(u0);[numthreads(8,8,1)] void main(uint3 id:SV_DispatchThreadID){output[id.xy]=.125;}"};
                                                for(UINT i=0;i<2 && indirect_ready;++i) {
                                                    Microsoft::WRL::ComPtr<ID3DBlob> code;
                                                    indirect_ready=SUCCEEDED(compile(compute[i],std::strlen(compute[i]),nullptr,nullptr,nullptr,"main","cs_5_0",0,0,&code,nullptr)) &&
                                                        reinterpret_cast<CreateShader>(vt(device)[18])(device,code->GetBufferPointer(),code->GetBufferSize(),nullptr,&indirect_objects[2+i])==S_OK;
                                                }
                                                D3D11_UNORDERED_ACCESS_VIEW_DESC output{};output.Format=DXGI_FORMAT_R8_UNORM;output.ViewDimension=D3D11_UAV_DIMENSION_TEXTURE2D;
                                                if(indirect_ready)indirect_ready=reinterpret_cast<Uav>(vt(device)[8])(device,texture,&output,&indirect_objects[4])==S_OK;
                                                check(indirect_ready,"reference indirect fixture creates GPU-written argument data and native draw/dispatch targets");
                                                if(indirect_ready) {
                                                    D3D11_UNORDERED_ACCESS_VIEW_DESC observed{};
                                                    reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,D3D11_UNORDERED_ACCESS_VIEW_DESC*)>(vt(indirect_objects[1])[9])(indirect_objects[1],&observed);
                                                    check(std::memcmp(&observed,&raw,sizeof raw)==0,"raw UAV preserves the guest description");
                                                    for(UINT invalid=0;invalid<4;++invalid) {
                                                        auto bad=raw;
                                                        if(invalid==0)bad.Buffer.Flags|=D3D11_BUFFER_UAV_FLAG_COUNTER;
                                                        if(invalid==1)bad.Format=DXGI_FORMAT_R32_UINT;
                                                        if(invalid==2)bad.Buffer.NumElements=33;
                                                        if(invalid==3)bad.Buffer.FirstElement=UINT_MAX;
                                                        void* rejected=reinterpret_cast<void*>(1);
                                                        const auto result=reinterpret_cast<Uav>(vt(device)[8])(device,indirect_objects[0],&bad,&rejected);
                                                        check(result==E_INVALIDARG && !rejected,"raw UAV rejects mixed counter flags, typed formats and out-of-buffer ranges");
                                                    }
                                                    const auto draw_indirect=reinterpret_cast<Indirect>(vt(context)[40]);const auto indexed_indirect=reinterpret_cast<Indirect>(vt(context)[39]);
                                                    const auto dispatch_indirect=reinterpret_cast<Indirect>(vt(context)[42]);
                                                    const auto set_compute=[&](UINT shader,UINT uav) {
                                                        reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*)>(vt(context)[69])(context,indirect_objects[shader]);
                                                        reinterpret_cast<ComputeBind>(vt(context)[68])(context,0,1,&indirect_objects[uav]);
                                                    };
                                                    set_compute(3,4);
                                                    static UINT refused=0;
                                                    set_d3d11x_trap_handler([](const char*,UINT slot)->std::uintptr_t{refused=slot;return 0;});
                                                    const auto reject=[&](UINT slot,void* buffer,UINT offset) {
                                                        refused=0;reinterpret_cast<Indirect>(vt(context)[slot])(context,buffer,offset);
                                                        check(refused==slot,"indirect command rejects null/unreadable/wrong-type arguments, bad alignment and logical range overflow");
                                                    };
                                                    reject(40,nullptr,16);reject(39,reinterpret_cast<void*>(1),40);reject(42,texture,80);
                                                    reject(40,indirect_objects[0],2);reject(39,indirect_objects[0],112);reject(42,indirect_objects[0],120);
                                                    reject(40,indirect_objects[0],UINT_MAX-3);
                                                    set_d3d11x_trap_handler(nullptr);
                                                    check(exact(multiple),"invalid indirect commands leave the rendered image unchanged");
                                                    void* arguments=nullptr;std::memcpy(&arguments,static_cast<BYTE*>(indirect_objects[0])+48,sizeof arguments);
                                                    using Placement=HRESULT(STDMETHODCALLTYPE*)(void*,const BufferDesc*,void*,void**);
                                                    BufferDesc placed=args_desc;placed.standard.BindFlags=D3D11_BIND_SHADER_RESOURCE;placed.standard.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS;void* borrowed=nullptr;
                                                    const auto placed_hr=reinterpret_cast<Placement>(vt(device)[62])(device,&placed,arguments,&borrowed);
                                                    check(placed_hr==S_OK && borrowed,"placement argument rejection uses a real borrowed view of valid argument bytes");
                                                    if(borrowed) {
                                                        refused=0;set_d3d11x_trap_handler([](const char*,UINT slot)->std::uintptr_t{refused=slot;return 0;});
                                                        draw_indirect(context,borrowed,16);set_d3d11x_trap_handler(nullptr);
                                                        check(refused==40 && exact(multiple),"reference placement indirect arguments remain explicitly unsupported without CPU fallback");
                                                        reinterpret_cast<Count>(vt(borrowed)[2])(borrowed);
                                                    }
                                                    for(UINT indexed=0;indexed<2;++indexed) {
                                                        reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*,const FLOAT*)>(vt(context)[50])(context,view,black);
                                                        (indexed ? indexed_indirect:draw_indirect)(context,indirect_objects[0],indexed ? 40:16);
                                                        check(exact(multiple),"native indirect draw consumes all GPU argument fields at a nonzero byte offset");
                                                    }
                                                    dispatch_indirect(context,indirect_objects[0],80);
                                                    check(exact(32),"native indirect dispatch consumes current GPU group dimensions and writes every pixel");
                                                    void* indirect_producer=nullptr;void* argument_list=nullptr;void* dispatch_list=nullptr;
                                                    if(reinterpret_cast<Deferred>(vt(device)[27])(device,0,&indirect_producer)==S_OK && indirect_producer) {
                                                        bind(indirect_producer);reinterpret_cast<Indirect>(vt(indirect_producer)[39])(indirect_producer,indirect_objects[0],40);
                                                        using Finish=HRESULT(STDMETHODCALLTYPE*)(void*,BOOL,void**);
                                                        check(reinterpret_cast<Finish>(vt(indirect_producer)[114])(indirect_producer,FALSE,&argument_list)==S_OK && argument_list && exact(32),
                                                            "indirect recording retains the buffer without executing or capturing its current argument values");
                                                        reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*)>(vt(indirect_producer)[69])(indirect_producer,indirect_objects[3]);
                                                        reinterpret_cast<ComputeBind>(vt(indirect_producer)[68])(indirect_producer,0,1,&indirect_objects[4]);
                                                        reinterpret_cast<Indirect>(vt(indirect_producer)[42])(indirect_producer,indirect_objects[0],80);
                                                        check(reinterpret_cast<Finish>(vt(indirect_producer)[114])(indirect_producer,FALSE,&dispatch_list)==S_OK && dispatch_list && exact(32),
                                                            "indirect dispatch recording retains bindings without executing its nonzero groups");
                                                        reinterpret_cast<Count>(vt(indirect_producer)[2])(indirect_producer);
                                                    }else check(false,"indirect recording creates a deferred context");
                                                    set_compute(2,1);reinterpret_cast<Dispatch>(vt(context)[41])(context,1,1,1);
                                                    const auto* actual=static_cast<const UINT*>(arguments);
                                                    check(actual[0]==0x6d6d6d6d && actual[3]==actual[0] && actual[8]==actual[0] && actual[9]==actual[0] && actual[15]==actual[0] && actual[31]==0,
                                                        "GPU argument updates preserve prefix, gaps and final buffer guards");
                                                    for(UINT indexed=0;indexed<2;++indexed) {
                                                        reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*,const FLOAT*)>(vt(context)[50])(context,view,black);
                                                        (indexed ? indexed_indirect:draw_indirect)(context,indirect_objects[0],indexed ? 40:16);
                                                        check(exact(single),"indirect draws observe arguments changed by the real compute shader");
                                                    }
                                                    set_compute(3,4);dispatch_indirect(context,indirect_objects[0],80);
                                                    check(exact(single),"indirect dispatch observes GPU-written zero groups without modifying pixels");
                                                    reinterpret_cast<void(STDMETHODCALLTYPE*)(void*)>(vt(context)[110])(context);
                                                    for(auto*& object:indirect_objects)if(object){reinterpret_cast<Count>(vt(object)[2])(object);object=nullptr;}
                                                    if(argument_list) {
                                                        for(UINT pass=0;pass<2;++pass) {
                                                            reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*,const FLOAT*)>(vt(context)[50])(context,view,black);
                                                            reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*,BOOL)>(vt(context)[58])(context,argument_list,TRUE);
                                                            check(exact(single),"indirect list retains arguments and bindings after all callers release them and reads the later GPU values on reuse");
                                                        }
                                                        reinterpret_cast<Count>(vt(argument_list)[2])(argument_list);
                                                    }
                                                    if(dispatch_list) {
                                                        for(UINT pass=0;pass<2;++pass) {
                                                            reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*,const FLOAT*)>(vt(context)[50])(context,view,black);
                                                            reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*,BOOL)>(vt(context)[58])(context,dispatch_list,TRUE);
                                                            check(exact(0),"retained indirect dispatch reads later GPU-written zero groups after producer and resource release");
                                                        }
                                                        reinterpret_cast<Count>(vt(dispatch_list)[2])(dispatch_list);
                                                    }
                                                    bind(context);
                                                }
                                                for(auto* object:indirect_objects)if(object)reinterpret_cast<Count>(vt(object)[2])(object);
                                            }
                                            {
                                                BufferDesc desc{{128,D3D11_USAGE_DYNAMIC,0,D3D11_CPU_ACCESS_WRITE,0,0},0};void* arguments=nullptr;void* producer=nullptr;void* lists[2]{};
                                                const auto created=reinterpret_cast<BufferFactory>(vt(device)[3])(device,&desc,nullptr,&arguments);
                                                check(created==S_OK && arguments,"indirect arguments accept owned dynamic buffers without an artificial misc-flag requirement");
                                                if(created!=S_OK || !arguments)return 1;
                                                if(reinterpret_cast<Deferred>(vt(device)[27])(device,0,&producer)!=S_OK || !producer)return 1;
                                                for(UINT version=0;version<2;++version) {
                                                    bind(producer);D3D11_MAPPED_SUBRESOURCE mapped{};
                                                    using Map=HRESULT(STDMETHODCALLTYPE*)(void*,void*,UINT,UINT,UINT,D3D11_MAPPED_SUBRESOURCE*);
                                                    const auto mapped_hr=reinterpret_cast<Map>(vt(producer)[15])(producer,arguments,0,D3D11_MAP_WRITE_DISCARD,0,&mapped);
                                                    check(mapped_hr==S_OK && mapped.pData,"deferred indirect argument rename maps its private version");if(mapped_hr!=S_OK || !mapped.pData)return 1;
                                                    std::memset(mapped.pData,0,128);const UINT data[]{3,version ? 1u:2u,1,UINT(-1),version ? 6u:3u};std::memcpy(static_cast<BYTE*>(mapped.pData)+40,data,sizeof data);
                                                    reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*)>(vt(producer)[16])(producer,arguments);
                                                    reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*,UINT)>(vt(producer)[39])(producer,arguments,40);
                                                    using Finish=HRESULT(STDMETHODCALLTYPE*)(void*,BOOL,void**);
                                                    check(reinterpret_cast<Finish>(vt(producer)[114])(producer,FALSE,&lists[version])==S_OK && lists[version],"indirect list captures the renamed argument allocation");
                                                }
                                                reinterpret_cast<Count>(vt(producer)[2])(producer);reinterpret_cast<Count>(vt(arguments)[2])(arguments);
                                                reinterpret_cast<void(STDMETHODCALLTYPE*)(void*)>(vt(context)[110])(context);
                                                for(UINT version:{0u,1u,0u})if(lists[version]) {
                                                    reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*,const FLOAT*)>(vt(context)[50])(context,view,black);
                                                    reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*,BOOL)>(vt(context)[58])(context,lists[version],TRUE);
                                                    check(exact(version ? single:multiple),"indirect replay keeps each WRITE_DISCARD argument version distinct after producer and original buffer release");
                                                }
                                                for(auto* list:lists)if(list)reinterpret_cast<Count>(vt(list)[2])(list);bind(context);
                                            }
                                            {
                                                void* counter_objects[4]{}; // Structured buffer, counter UAV, destination, compute shader.
                                                BufferDesc structured{{64,D3D11_USAGE_DEFAULT,D3D11_BIND_UNORDERED_ACCESS,0,D3D11_RESOURCE_MISC_BUFFER_STRUCTURED,4},0};
                                                bool counter_ready=reinterpret_cast<BufferFactory>(vt(device)[3])(device,&structured,nullptr,&counter_objects[0])==S_OK;
                                                D3D11_UNORDERED_ACCESS_VIEW_DESC counter_desc{};counter_desc.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;
                                                counter_desc.Buffer.NumElements=16;counter_desc.Buffer.Flags=D3D11_BUFFER_UAV_FLAG_COUNTER;
                                                using Uav=HRESULT(STDMETHODCALLTYPE*)(void*,void*,const D3D11_UNORDERED_ACCESS_VIEW_DESC*,void**);
                                                if(counter_ready)counter_ready=reinterpret_cast<Uav>(vt(device)[8])(device,counter_objects[0],&counter_desc,&counter_objects[1])==S_OK;
                                                std::array<BYTE,16> guards;guards.fill(0xa5);
                                                BufferDesc target_desc{{16,D3D11_USAGE_DEFAULT,0,0,0,0},0};const D3D11_SUBRESOURCE_DATA initial{guards.data(),0,0};
                                                if(counter_ready)counter_ready=reinterpret_cast<BufferFactory>(vt(device)[3])(device,&target_desc,&initial,&counter_objects[2])==S_OK;
                                                const char* writer="RWStructuredBuffer<uint> output:register(u0);[numthreads(1,1,1)]void main(){uint i=output.IncrementCounter();output[i]=i+7;}";
                                                Microsoft::WRL::ComPtr<ID3DBlob> code;
                                                if(counter_ready)counter_ready=SUCCEEDED(compile(writer,std::strlen(writer),nullptr,nullptr,nullptr,"main","cs_5_0",0,0,&code,nullptr)) &&
                                                    reinterpret_cast<CreateShader>(vt(device)[18])(device,code->GetBufferPointer(),code->GetBufferSize(),nullptr,&counter_objects[3])==S_OK;
                                                check(counter_ready,"structure count fixture creates a real GPU counter, destination and shader");
                                                if(counter_ready) {
                                                    using CopyCount=void(STDMETHODCALLTYPE*)(void*,void*,UINT,void*);
                                                    const auto copy_count=reinterpret_cast<CopyCount>(vt(context)[49]);
                                                    BYTE* target=nullptr;std::memcpy(&target,static_cast<BYTE*>(counter_objects[2])+48,sizeof target);
                                                    const auto matches=[&](UINT offset,UINT value) {
                                                        auto expected=guards;std::memcpy(expected.data()+offset,&value,4);
                                                        return std::memcmp(target,expected.data(),expected.size())==0;
                                                    };
                                                    copy_count(context,counter_objects[2],0,counter_objects[1]);
                                                    check(matches(0,0),"new reference UAV counter starts at zero without synthetic shader initialization");
                                                    std::memcpy(target,guards.data(),guards.size());
                                                    reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*)>(vt(context)[69])(context,counter_objects[3]);
                                                    reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,UINT,UINT,void* const*)>(vt(context)[68])(context,0,1,&counter_objects[1]);
                                                    reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,UINT,UINT,UINT)>(vt(context)[41])(context,3,1,1);
                                                    copy_count(context,counter_objects[2],5,counter_objects[1]);
                                                    check(matches(5,3),"structure count copies four GPU-written bytes at an unaligned offset and preserves every surrounding byte");
                                                    {
                                                        std::array<BYTE,24> fill;fill.fill(0x3c);auto copied_desc=target_desc;copied_desc.standard.ByteWidth=24;
                                                        const D3D11_SUBRESOURCE_DATA copied_initial{fill.data(),0,0};void* copied=nullptr;
                                                        check(reinterpret_cast<BufferFactory>(vt(device)[3])(device,&copied_desc,&copied_initial,&copied)==S_OK && copied,
                                                            "resource copy creates a larger buffer with guarded tail bytes");
                                                        if(copied) {
                                                            reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*,void*)>(vt(context)[47])(context,copied,counter_objects[2]);
                                                            BYTE* bytes=nullptr;std::memcpy(&bytes,static_cast<BYTE*>(copied)+48,sizeof bytes);
                                                            std::memcpy(fill.data(),guards.data(),16);const UINT count=3;std::memcpy(fill.data()+5,&count,4);
                                                            check(!std::memcmp(bytes,fill.data(),fill.size()),"CopyResource consumes GPU-written buffer data and copies the smaller logical size without touching the destination tail");
                                                            reinterpret_cast<Count>(vt(copied)[2])(copied);
                                                        }
                                                    }
                                                    copy_count(context,counter_objects[2],13,counter_objects[1]);copy_count(context,counter_objects[2],UINT_MAX,counter_objects[1]);
                                                    check(matches(5,3),"reference out-of-range structure count copies leave the buffer unchanged");
                                                    static UINT refused_count=0;
                                                    set_d3d11x_trap_handler([](const char*,UINT slot)->std::uintptr_t{refused_count=slot;return 0;});
                                                    for(UINT invalid=0;invalid<3;++invalid) {
                                                        refused_count=0;copy_count(context,invalid==2 ? counter_objects[1]:counter_objects[2],0,
                                                            invalid==0 ? nullptr:invalid==1 ? counter_objects[0]:counter_objects[1]);
                                                        check(refused_count==49 && matches(5,3),"structure count rejects invalid destination and source objects without modifying pixels or bytes");
                                                    }
                                                    set_d3d11x_trap_handler(nullptr);
                                                    void* no_counter=nullptr;auto plain_desc=counter_desc;plain_desc.Buffer.Flags=0;
                                                    check(reinterpret_cast<Uav>(vt(device)[8])(device,counter_objects[0],&plain_desc,&no_counter)==S_OK && no_counter,
                                                        "counterless negative control creates a valid structured UAV");
                                                    if(no_counter) {
                                                        refused_count=0;set_d3d11x_trap_handler([](const char*,UINT slot)->std::uintptr_t{refused_count=slot;return 0;});
                                                        copy_count(context,counter_objects[2],0,no_counter);set_d3d11x_trap_handler(nullptr);
                                                        check(refused_count==49 && matches(5,3),"structure count rejects a valid UAV without a counter");
                                                        reinterpret_cast<Count>(vt(no_counter)[2])(no_counter);
                                                    }
                                                    auto placed_desc=target_desc;placed_desc.standard.BindFlags=D3D11_BIND_SHADER_RESOURCE;
                                                    using Placement=HRESULT(STDMETHODCALLTYPE*)(void*,const BufferDesc*,void*,void**);void* borrowed=nullptr;
                                                    check(reinterpret_cast<Placement>(vt(device)[62])(device,&placed_desc,target,&borrowed)==S_OK && borrowed,
                                                        "structure count placement rejection uses a valid borrowed buffer");
                                                    if(borrowed) {
                                                        refused_count=0;set_d3d11x_trap_handler([](const char*,UINT slot)->std::uintptr_t{refused_count=slot;return 0;});
                                                        copy_count(context,borrowed,0,counter_objects[1]);set_d3d11x_trap_handler(nullptr);
                                                        check(refused_count==49 && matches(5,3),"reference structure count rejects placement destinations without a CPU fallback");
                                                        reinterpret_cast<Count>(vt(borrowed)[2])(borrowed);
                                                    }
                                                    void* producer=nullptr;void* list=nullptr;
                                                    if(reinterpret_cast<Deferred>(vt(device)[27])(device,0,&producer)==S_OK && producer) {
                                                        reinterpret_cast<CopyCount>(vt(producer)[49])(producer,counter_objects[2],9,counter_objects[1]);
                                                        using Finish=HRESULT(STDMETHODCALLTYPE*)(void*,BOOL,void**);
                                                        check(reinterpret_cast<Finish>(vt(producer)[114])(producer,FALSE,&list)==S_OK && list && matches(5,3),
                                                            "structure count recording retains objects without capturing the current counter value or executing");
                                                        reinterpret_cast<Count>(vt(producer)[2])(producer);
                                                    }else check(false,"structure count creates a deferred producer");
                                                    reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,UINT,UINT,UINT)>(vt(context)[41])(context,2,1,1);
                                                    reinterpret_cast<void(STDMETHODCALLTYPE*)(void*)>(vt(context)[110])(context);
                                                    for(auto*& object:counter_objects)if(object){reinterpret_cast<Count>(vt(object)[2])(object);object=nullptr;}
                                                    if(list) {
                                                        for(UINT pass=0;pass<2;++pass) {
                                                            std::memcpy(target,guards.data(),guards.size());
                                                            reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*,BOOL)>(vt(context)[58])(context,list,TRUE);
                                                            check(matches(9,5),"structure count replay observes the later GPU value after caller release and CPU invalidation");
                                                        }
                                                        reinterpret_cast<Count>(vt(list)[2])(list);
                                                    }
                                                    bind(context);
                                                }
                                                for(auto* object:counter_objects)if(object)reinterpret_cast<Count>(vt(object)[2])(object);
                                            }
                                            for(const bool partial:{false,true}) {
                                                void* copied[2]{};void* producer=nullptr;void* list=nullptr;
                                                std::array<BYTE,16> source{},guard;guard.fill(0xee);for(UINT i=0;i<16;++i)source[i]=BYTE(i*13+3);
                                                BufferDesc desc{{16,D3D11_USAGE_DEFAULT,0,0,0,0},0};
                                                for(UINT i=0;i<2;++i) {
                                                    const D3D11_SUBRESOURCE_DATA data{i ? guard.data():source.data(),0,0};
                                                    check(reinterpret_cast<BufferFactory>(vt(device)[3])(device,&desc,&data,&copied[i])==S_OK && copied[i],"deferred CopyResource creates independent source and destination buffers");
                                                }
                                                if(!copied[0] || !copied[1])return 1;
                                                BYTE* input=nullptr;BYTE* output=nullptr;std::memcpy(&input,static_cast<BYTE*>(copied[0])+48,sizeof input);std::memcpy(&output,static_cast<BYTE*>(copied[1])+48,sizeof output);
                                                using Copy=void(STDMETHODCALLTYPE*)(void*,void*,void*);
                                                reinterpret_cast<Copy>(vt(context)[47])(context,copied[0],copied[0]);
                                                check(!std::memcmp(input,source.data(),16),"CopyResource into the same buffer is an unchanged identity copy");
                                                static UINT refused_copy=0;set_d3d11x_trap_handler([](const char*,UINT slot)->std::uintptr_t{refused_copy=slot;return 0;});
                                                for(UINT invalid=0;invalid<3;++invalid) {
                                                    refused_copy=0;reinterpret_cast<Copy>(vt(context)[47])(context,copied[1],invalid==0 ? nullptr:invalid==1 ? reinterpret_cast<void*>(1):texture);
                                                    check(refused_copy==47 && !std::memcmp(output,guard.data(),16),"CopyResource rejects unreadable sources and buffer/texture mismatches without changing the destination");
                                                }
                                                set_d3d11x_trap_handler(nullptr);
                                                using Region=void(STDMETHODCALLTYPE*)(void*,void*,UINT,UINT,UINT,UINT,void*,UINT,const D3D11_BOX*);
                                                if(!partial) {
                                                    auto placement=desc;placement.standard.ByteWidth=8;placement.standard.BindFlags=D3D11_BIND_SHADER_RESOURCE;
                                                    using Place=HRESULT(STDMETHODCALLTYPE*)(void*,const BufferDesc*,void*,void**);void* alias=nullptr;
                                                    check(reinterpret_cast<Place>(vt(device)[62])(device,&placement,output+4,&alias)==S_OK && alias,"DDR alias copy creates a borrowed source over the owned destination");
                                                    if(alias) {
                                                        for(UINT mode=0;mode<2;++mode) {
                                                            for(UINT i=0;i<16;++i)output[i]=BYTE(i*11+mode+1);std::array<BYTE,16> expected;std::memcpy(expected.data(),output,16);
                                                            if(mode) {
                                                                const D3D11_BOX box{1,0,0,7,1,1};std::memmove(expected.data()+8,expected.data()+5,6);
                                                                reinterpret_cast<Region>(vt(context)[46])(context,copied[1],0,8,0,0,alias,0,&box);
                                                            }else {std::memmove(expected.data(),expected.data()+4,8);reinterpret_cast<Copy>(vt(context)[47])(context,copied[1],alias);}
                                                            check(!std::memcmp(output,expected.data(),16),"DDR copies snapshot an overlapping borrowed source on GPU before the owned destination is changed");
                                                        }
                                                        reinterpret_cast<Count>(vt(alias)[2])(alias);
                                                        std::memcpy(output,guard.data(),16);
                                                    }
                                                    placement.standard.ByteWidth=16;alias=nullptr;
                                                    check(reinterpret_cast<Place>(vt(device)[62])(device,&placement,output,&alias)==S_OK && alias,"same-window buffer alias retains a separate resource handle");
                                                    if(alias) {
                                                        reinterpret_cast<Copy>(vt(context)[47])(context,copied[1],alias);
                                                        check(!std::memcmp(output,guard.data(),16),"copy between two handles of the same native buffer preserves the full owned contents");
                                                        reinterpret_cast<Count>(vt(alias)[2])(alias);
                                                    }
                                                }
                                                if(partial) {
                                                    const auto copy=reinterpret_cast<Region>(vt(context)[46]);
                                                    for(const UINT destination:{4u,0u,9u}) {
                                                        std::memcpy(input,source.data(),16);auto expected=source;
                                                        const UINT left=destination==0 ? 4:0,count=destination==9 ? 6:10;
                                                        D3D11_BOX box{left,UINT_MAX,UINT_MAX,left+count,0,0};
                                                        std::memmove(expected.data()+destination,expected.data()+left,count);
                                                        copy(context,copied[0],UINT_MAX,destination,UINT_MAX,UINT_MAX,copied[0],UINT_MAX,&box);
                                                        check(!std::memcmp(input,expected.data(),16),"reference buffer region copy uses GPU scratch for both overlapping directions and ignores texture-only coordinates");
                                                    }
                                                    std::memcpy(input,source.data(),16);
                                                    static UINT refused_region=0;set_d3d11x_trap_handler([](const char*,UINT slot)->std::uintptr_t{refused_region=slot;return 0;});
                                                    for(UINT invalid=0;invalid<4;++invalid) {
                                                        D3D11_BOX box{3,0,0,11,1,1};UINT x=0;
                                                        if(invalid==0)box.right=17;if(invalid==1)box.right=box.left;if(invalid==2)x=UINT_MAX;if(invalid==3){box.left=12;box.right=4;}
                                                        refused_region=0;copy(context,copied[1],0,x,0,0,copied[0],0,&box);
                                                        check(!refused_region && !std::memcmp(output,guard.data(),16),"empty, reversed, overflowing and out-of-buffer regions are reference no-ops");
                                                    }
                                                    refused_region=0;copy(context,copied[1],0,0,0,0,copied[0],0,reinterpret_cast<const D3D11_BOX*>(1));
                                                    check(refused_region==46 && !std::memcmp(output,guard.data(),16),"region copy rejects an unreadable source-box pointer before changing destination bytes");
                                                    refused_region=0;copy(context,copied[1],0,1,0,0,copied[0],0,nullptr);
                                                    check(!refused_region && !std::memcmp(output,guard.data(),16),"null-box buffer region copies the entire source or leaves the destination unchanged when it cannot fit");
                                                    set_d3d11x_trap_handler(nullptr);
                                                }
                                                if(reinterpret_cast<Deferred>(vt(device)[27])(device,0,&producer)==S_OK && producer) {
                                                    D3D11_BOX box{3,0,0,11,1,1};
                                                    if(partial)reinterpret_cast<Region>(vt(producer)[46])(producer,copied[1],0,2,0,0,copied[0],0,&box);
                                                    else reinterpret_cast<Copy>(vt(producer)[47])(producer,copied[1],copied[0]);
                                                    box={};
                                                    using Finish=HRESULT(STDMETHODCALLTYPE*)(void*,BOOL,void**);
                                                    check(reinterpret_cast<Finish>(vt(producer)[114])(producer,FALSE,&list)==S_OK && list && !std::memcmp(output,guard.data(),16),"CopyResource recording preserves destination bytes and retains both resources");
                                                    reinterpret_cast<Count>(vt(producer)[2])(producer);
                                                }else check(false,"CopyResource creates a deferred producer");
                                                for(auto* object:copied)reinterpret_cast<Count>(vt(object)[2])(object);
                                                if(list) {
                                                    for(UINT pass=0;pass<2;++pass) {
                                                        for(UINT i=0;i<16;++i)input[i]=BYTE(source[i]+pass+1);
                                                        std::memcpy(output,guard.data(),16);
                                                        reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*,BOOL)>(vt(context)[58])(context,list,TRUE);
                                                        auto expected=guard;
                                                        if(partial)for(UINT i=0;i<8;++i)expected[i+2]=BYTE(source[i+3]+pass+1);
                                                        else for(UINT i=0;i<16;++i)expected[i]=BYTE(source[i]+pass+1);
                                                        check(!std::memcmp(output,expected.data(),16),"full/partial copy replay retains the box and resources, reads later source bytes and preserves uncopied destination bytes");
                                                    }
                                                    reinterpret_cast<Count>(vt(list)[2])(list);
                                                }
                                                bind(context);
                                            }
                                            reinterpret_cast<Instanced>(vt(context)[22])(context,3,(UINT64(6)<<32)|5,1);
                                            check(exact(single),"one instance uses its nonzero first-instance input location");
                                            reinterpret_cast<Instanced>(vt(context)[22])(context,3,(UINT64(3)<<32)|5,0);
                                            check(exact(single),"zero instances preserve every destination pixel");
                                            void* producer=nullptr;void* list=nullptr;
                                            if(reinterpret_cast<Deferred>(vt(device)[27])(device,0,&producer)==S_OK && producer) {
                                                bind(producer);
                                                reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*,const FLOAT*)>(vt(producer)[50])(producer,view,black);
                                                reinterpret_cast<IndexedInstanced>(vt(producer)[21])(producer,(UINT64(3)<<32)|1,(UINT64(3)<<32)|UINT(-1),2);
                                                using Finish=HRESULT(STDMETHODCALLTYPE*)(void*,BOOL,void**);
                                                const bool unchanged=exact(single);const auto finished=reinterpret_cast<Finish>(vt(producer)[114])(producer,FALSE,&list);
                                                check(unchanged && finished==S_OK && list,
                                                    "deferred instanced draw records without publishing and retains all packed draw fields");
                                                reinterpret_cast<Count>(vt(producer)[2])(producer);
                                            }else check(false,"instanced recording creates a deferred producer");
                                            reinterpret_cast<void(STDMETHODCALLTYPE*)(void*)>(vt(context)[110])(context);
                                            for(auto*& object:objects)if(object){reinterpret_cast<Count>(vt(object)[2])(object);object=nullptr;}
                                            if(list) {
                                                for(UINT pass=0;pass<2;++pass) {
                                                    reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*,BOOL)>(vt(context)[58])(context,list,TRUE);
                                                    check(exact(multiple),"deferred instanced draw survives object release and replays exact pixels across pipeline reuse and shader replacement");
                                                }
                                                reinterpret_cast<Count>(vt(list)[2])(list);
                                            }
                                        }
                                        for(auto* object:objects)if(object)reinterpret_cast<Count>(vt(object)[2])(object);
                                        bind_targets(context,1,targets,nullptr);
                                        reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,UINT,const D3D11_VIEWPORT*)>(vt(context)[44])(context,1,&viewport);
                                        std::memcpy(static_cast<char*>(context)+0x170,&topology,sizeof topology);static_cast<BYTE*>(context)[0x390]=1;
                                        std::memcpy(static_cast<char*>(context)+0x180,&shaders[0],sizeof(void*));std::memcpy(static_cast<char*>(context)+0x188,&shaders[2],sizeof(void*));
                                        std::memcpy(static_cast<char*>(context)+0x178,&empty_layout,sizeof empty_layout);
                                    }
                                    {
                                        const char* source="cbuffer C:register(b2){float4 shift;} struct V{float4 p:SV_Position;}; [maxvertexcount(3)] void main(triangle V input[3],inout TriangleStream<V> output){for(uint i=0;i<3;i++){V v=input[i];v.p+=shift;output.Append(v);}}";
                                        Microsoft::WRL::ComPtr<ID3DBlob> code;
                                        void* geometry=nullptr;void* constants=nullptr;
                                        const FLOAT shift[]{8,0,0,0};
                                        struct ConstantDesc {D3D11_BUFFER_DESC standard;UINT64 extension;} desc{{16,D3D11_USAGE_DEFAULT,D3D11_BIND_CONSTANT_BUFFER,0,0,0},0};
                                        const D3D11_SUBRESOURCE_DATA initial{shift,0,0};
                                        using MakeBuffer=HRESULT(STDMETHODCALLTYPE*)(void*,const ConstantDesc*,const D3D11_SUBRESOURCE_DATA*,void**);
                                        const bool made=SUCCEEDED(compile(source,std::strlen(source),nullptr,nullptr,nullptr,"main","gs_5_0",0,0,&code,nullptr)) &&
                                            reinterpret_cast<CreateShader>(vt(device)[13])(device,code->GetBufferPointer(),code->GetBufferSize(),nullptr,&geometry)==S_OK &&
                                            reinterpret_cast<MakeBuffer>(vt(device)[3])(device,&desc,&initial,&constants)==S_OK;
                                        check(made,"geometry fixture creates an original-ABI shader and separate stage constant buffer");
                                        if(made) {
                                            using BindShader=void(STDMETHODCALLTYPE*)(void*,void*);
                                            using BindConstants=void(STDMETHODCALLTYPE*)(void*,UINT,UINT,void* const*);
                                            const FLOAT black[]{0,0,0,0};
                                            reinterpret_cast<BindConstants>(vt(context)[23])(context,2,1,&constants);
                                            reinterpret_cast<BindShader>(vt(context)[24])(context,geometry);
                                            reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*,const FLOAT*)>(vt(context)[50])(context,view,black);
                                            draw(context,3,0);
                                            check(pixels.copy(pixels.context.get(),false,linear.data(),alias,0,32,512,nullptr)==S_OK &&
                                                std::all_of(linear.begin(),linear.end(),[](auto value){return value==0;}),"real GS reads sparse b2 and clips the procedural triangle using geometry-stage visibility");
                                            reinterpret_cast<BindShader>(vt(context)[24])(context,nullptr);
                                            draw(context,3,0);
                                            check(pixels.copy(pixels.context.get(),false,linear.data(),alias,0,32,512,nullptr)==S_OK &&
                                                std::all_of(linear.begin(),linear.end(),[](auto value){return value==191;}),"unbinding GS restores original VS/PS pixels");
                                            reinterpret_cast<BindConstants>(vt(context)[23])(context,2,1,nullptr);
                                            const char* point_source="struct V{float4 p:SV_Position;}; [maxvertexcount(3)] void main(point V input[1],inout TriangleStream<V> output){for(uint i=0;i<3;i++){float2 p=float2((i<<1)&2,i&2);V v;v.p=float4(p*float2(2,-2)+float2(-1,1),0,1);output.Append(v);}}";
                                            Microsoft::WRL::ComPtr<ID3DBlob> point_code;void* point_geometry=nullptr;
                                            const bool point_ready=SUCCEEDED(compile(point_source,std::strlen(point_source),nullptr,nullptr,nullptr,"main","gs_5_0",0,0,&point_code,nullptr)) &&
                                                reinterpret_cast<CreateShader>(vt(device)[13])(device,point_code->GetBufferPointer(),point_code->GetBufferSize(),nullptr,&point_geometry)==S_OK;
                                            check(point_ready,"point-list regression creates a real point-input geometry shader");
                                            if(point_ready) {
                                                const UINT64 point_topology=0x000003ffc0009e00ull;
                                                std::memcpy(static_cast<char*>(context)+0x170,&point_topology,sizeof point_topology);
                                                reinterpret_cast<BindShader>(vt(context)[24])(context,point_geometry);
                                                reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*,const FLOAT*)>(vt(context)[50])(context,view,black);
                                                draw(context,1,0);
                                                check(pixels.copy(pixels.context.get(),false,linear.data(),alias,0,32,512,nullptr)==S_OK &&
                                                    std::all_of(linear.begin(),linear.end(),[](auto value){return value==191;}),
                                                    "original point topology reaches native Draw and GS expansion with exact expected pixels");
                                                reinterpret_cast<BindShader>(vt(context)[24])(context,nullptr);
                                                std::memcpy(static_cast<char*>(context)+0x170,&topology,sizeof topology);
                                            }
                                            if(point_geometry)reinterpret_cast<Count>(vt(point_geometry)[2])(point_geometry);
                                        }
                                        if(geometry) reinterpret_cast<Count>(vt(geometry)[2])(geometry);
                                        if(constants) reinterpret_cast<Count>(vt(constants)[2])(constants);
                                    }
                                    std::memcpy(static_cast<char*>(context)+0x180,&shaders[3],sizeof(void*));
                                    std::memcpy(static_cast<char*>(context)+0x178,&vertex_layout,sizeof vertex_layout);
                                    using BindVertices=void(STDMETHODCALLTYPE*)(void*,UINT,UINT,void* const*,const UINT*,const UINT*);
                                    const auto bind_vertices=reinterpret_cast<BindVertices>(vt(context)[19]);
                                    const UINT stride=16,offset=0;
                                    bind_vertices(context,0,1,&vertex_buffer,&stride,&offset);
                                    const FLOAT black[]{0,0,0,0};
                                    reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*,const FLOAT*)>(vt(context)[50])(context,view,black);
                                    draw(context,3,1);
                                    check(pixels.copy(pixels.context.get(),false,linear.data(),alias,0,32,512,nullptr)==S_OK &&
                                        std::all_of(linear.begin(),linear.end(),[](auto value){return value==191;}),
                                        "non-indexed draw preserves first vertex, stride, appended input elements and inline layout changes");
                                    reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*,const FLOAT*)>(vt(context)[50])(context,view,black);
                                    void* null_vertex = nullptr; const UINT zero = 0;
                                    bind_vertices(context,0,1,&null_vertex,&zero,&zero);
                                    std::memcpy(static_cast<char*>(context)+0x180,&shaders[4],sizeof(void*));
                                    const UINT64 strip = 0x00000dffc0009e00ull;
                                    std::memcpy(static_cast<char*>(context)+0x170,&strip,sizeof strip);
                                    draw(context,4,0);
                                    check(pixels.copy(pixels.context.get(),false,linear.data(),alias,0,32,512,nullptr)==S_OK &&
                                        std::all_of(linear.begin(),linear.end(),[](auto value){return value==191;}),
                                        "four-vertex strip covers both triangles and null vertex input reads native zeroes after its fence");
                                    std::memcpy(static_cast<char*>(context)+0x180,&shaders[3],sizeof(void*));
                                    std::memcpy(static_cast<char*>(context)+0x170,&topology,sizeof topology);
                                    bind_vertices(context,0,1,&vertex_buffer,&stride,&offset);
                                    void* vertex_bytes=nullptr;std::memcpy(&vertex_bytes,static_cast<char*>(vertex_buffer)+48,sizeof vertex_bytes);
                                    check(vertex_bytes && !std::memcmp(vertex_bytes,vertices,sizeof vertices),"non-indexed draw preserves its canonical vertex bytes");
                                    const std::vector<unsigned char> before(static_cast<unsigned char*>(alias),static_cast<unsigned char*>(alias)+pixels.bytes);
                                    static UINT rejected_slot=0;rejected_slot=0;
                                    set_d3d11x_trap_handler([](const char*,UINT slot)->std::uintptr_t{rejected_slot=slot;return 0;});
                                    void* invalid_shader=reinterpret_cast<void*>(1);
                                    std::memcpy(static_cast<char*>(context)+0x188,&invalid_shader,sizeof invalid_shader);
                                    draw(context,3,0);
                                    check(rejected_slot==14 && !std::memcmp(before.data(),alias,before.size()),"invalid inline shader refuses draw without changing canonical pixels");
                                    std::memcpy(static_cast<char*>(context)+0x188,&shaders[2],sizeof(void*));
                                    const UINT invalid_offset=sizeof vertices+1;rejected_slot=0;
                                    bind_vertices(context,0,1,&vertex_buffer,&stride,&invalid_offset);draw(context,3,1);
                                    check(rejected_slot==14 && !std::memcmp(before.data(),alias,before.size()),"vertex offset outside the logical buffer refuses draw before GPU work");
                                    bind_vertices(context,0,1,&vertex_buffer,&stride,&offset);
                                    void* deferred_draw=nullptr;
                                    check(reinterpret_cast<Deferred>(vt(device)[27])(device,0x60000,&deferred_draw)==S_OK && deferred_draw,"deferred draw negative control creates a real deferred context");
                                    if(deferred_draw) {
                                        rejected_slot=0;reinterpret_cast<Draw>(vt(deferred_draw)[14])(deferred_draw,3,0);
                                        check(rejected_slot==14 && !std::memcmp(before.data(),alias,before.size()),"deferred draw is refused before any immediate GPU work");
                                        reinterpret_cast<Count>(vt(deferred_draw)[2])(deferred_draw);
                                    }
                                    set_d3d11x_trap_handler(nullptr);
                                    deferred_draw=nullptr;
                                    check(reinterpret_cast<Deferred>(vt(device)[27])(device,0x60000,&deferred_draw)==S_OK && deferred_draw,
                                          "deferred draw recording uses the existing context ABI");
                                    if(deferred_draw) {
                                        using Clear=void(STDMETHODCALLTYPE*)(void*,void*,const FLOAT*);
                                        using Finish=HRESULT(STDMETHODCALLTYPE*)(void*,BOOL,void**);
                                        using Execute=void(STDMETHODCALLTYPE*)(void*,void*,BOOL);
                                        const auto clear=reinterpret_cast<Clear>(vt(deferred_draw)[50]);
                                        bind_targets(deferred_draw,1,targets,nullptr);
                                        reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,UINT,const D3D11_VIEWPORT*)>(vt(deferred_draw)[44])(deferred_draw,1,&viewport);
                                        auto* data=static_cast<char*>(deferred_draw);data[0x390]=1;
                                        std::memcpy(data+0x170,&topology,sizeof topology);
                                        std::memcpy(data+0x178,&empty_layout,sizeof empty_layout);
                                        std::memcpy(data+0x180,&shaders[0],sizeof(void*));
                                        clear(deferred_draw,view,black);
                                        std::memcpy(data+0x188,&shaders[1],sizeof(void*));
                                        draw(deferred_draw,3,0);
                                        clear(deferred_draw,view,white);
                                        bind_targets(deferred_draw,0,nullptr,nullptr);
                                        draw(deferred_draw,3,0);
                                        bind_targets(deferred_draw,1,targets,nullptr);
                                        std::memcpy(data+0x188,&shaders[2],sizeof(void*));
                                        draw(deferred_draw,3,0);
                                        const UINT16 indices[]{99,0,1,2};
                                        struct IndexDesc {D3D11_BUFFER_DESC standard;UINT64 extension;} index_desc{{sizeof indices,D3D11_USAGE_IMMUTABLE,D3D11_BIND_INDEX_BUFFER,0,0,2},0};
                                        const D3D11_SUBRESOURCE_DATA index_data{indices,0,0};
                                        using CreateIndex=HRESULT(STDMETHODCALLTYPE*)(void*,const IndexDesc*,const D3D11_SUBRESOURCE_DATA*,void**);
                                        void* recorded_indices=nullptr;
                                        check(reinterpret_cast<CreateIndex>(vt(device)[3])(device,&index_desc,&index_data,&recorded_indices)==S_OK && recorded_indices,
                                              "deferred indexed draw uses a real retained index resource");
                                        if(recorded_indices) {
                                            reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,UINT,void*,UINT)>(vt(deferred_draw)[20])(deferred_draw,0,recorded_indices,0);
                                            std::memcpy(data+0x188,&shaders[1],sizeof(void*));
                                            clear(deferred_draw,view,white);
                                            reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,UINT64,INT)>(vt(deferred_draw)[13])(deferred_draw,(UINT64(3)<<32)|1,0);
                                        }
                                        void* list=nullptr;
                                        check(reinterpret_cast<Finish>(vt(deferred_draw)[114])(deferred_draw,TRUE,&list)==S_OK && list &&
                                            !std::memcmp(before.data(),alias,before.size()),
                                            "FinishCommandList never executes clears or draws and captures changing inline shaders");
                                        void* reset_shader=reinterpret_cast<void*>(1);void* reset_layout=reinterpret_cast<void*>(1);
                                        std::memcpy(&reset_shader,data+0x188,sizeof reset_shader);std::memcpy(&reset_layout,data+0x178,sizeof reset_layout);
                                        check(!reset_shader && !reset_layout,"11295 FinishCommandList resets deferred inline bindings even with restore true");
                                        if(recorded_indices) reinterpret_cast<Count>(vt(recorded_indices)[2])(recorded_indices);
                                        reinterpret_cast<Count>(vt(deferred_draw)[2])(deferred_draw);
                                        if(list) {
                                            // Destroy the producer and then replay twice, preserving recorded state and order.
                                            for(UINT pass=0;pass<2;++pass) {
                                                reinterpret_cast<Clear>(vt(context)[50])(context,view,black);
                                                reinterpret_cast<Execute>(vt(context)[58])(context,list,pass!=0);
                                                check(pixels.copy(pixels.context.get(),false,linear.data(),alias,0,32,512,nullptr)==S_OK &&
                                                    std::all_of(linear.begin(),linear.end(),[](auto value){return value==64;}),
                                                    "deferred clears and original shader draws replay in order after producer release, with fenced exact pixels");
                                            }
                                            void* merged=nullptr;
                                            check(reinterpret_cast<Deferred>(vt(device)[27])(device,0x60000,&merged)==S_OK && merged,"merged list has its own deferred producer");
                                            if(merged) {
                                                reinterpret_cast<Execute>(vt(merged)[58])(merged,list,FALSE);
                                                void* merged_list=nullptr;
                                                check(reinterpret_cast<Finish>(vt(merged)[114])(merged,FALSE,&merged_list)==S_OK && merged_list,
                                                      "executing on a deferred context records rather than submitting");
                                                reinterpret_cast<Count>(vt(merged)[2])(merged);
                                                reinterpret_cast<Count>(vt(list)[2])(list);list=nullptr;
                                                if(merged_list) {
                                                    reinterpret_cast<Clear>(vt(context)[50])(context,view,black);
                                                    reinterpret_cast<Execute>(vt(context)[58])(context,merged_list,FALSE);
                                                    check(pixels.copy(pixels.context.get(),false,linear.data(),alias,0,32,512,nullptr)==S_OK &&
                                                        std::all_of(linear.begin(),linear.end(),[](auto value){return value==64;}),
                                                        "merged list retains commands and resources after both producer and source list release");
                                                    reinterpret_cast<Count>(vt(merged_list)[2])(merged_list);
                                                }
                                            }
                                            if(list) reinterpret_cast<Count>(vt(list)[2])(list);
                                        }
                                    }
                                    struct DynamicDesc {D3D11_BUFFER_DESC standard;UINT64 extension;} dynamic_desc{{sizeof vertices,D3D11_USAGE_DYNAMIC,D3D11_BIND_VERTEX_BUFFER,D3D11_CPU_ACCESS_WRITE,0,0},0};
                                    const D3D11_SUBRESOURCE_DATA dynamic_data{vertices,0,0};
                                    using CreateDynamic=HRESULT(STDMETHODCALLTYPE*)(void*,const DynamicDesc*,const D3D11_SUBRESOURCE_DATA*,void**);
                                    using MapDeferred=HRESULT(STDMETHODCALLTYPE*)(void*,void*,UINT,D3D11_MAP,UINT,D3D11_MAPPED_SUBRESOURCE*);
                                    using UnmapDeferred=void(STDMETHODCALLTYPE*)(void*,void*);
                                    using FinishDeferred=HRESULT(STDMETHODCALLTYPE*)(void*,BOOL,void**);
                                    using ExecuteDeferred=void(STDMETHODCALLTYPE*)(void*,void*,BOOL);
                                    void* dynamic=nullptr;void* mapper=nullptr;void* versions[2]{};
                                    check(reinterpret_cast<CreateDynamic>(vt(device)[3])(device,&dynamic_desc,&dynamic_data,&dynamic)==S_OK && dynamic,
                                          "deferred Map fixture creates a dynamic vertex resource");
                                    check(reinterpret_cast<Deferred>(vt(device)[27])(device,0x60000,&mapper)==S_OK && mapper,
                                          "deferred Map fixture uses the existing context");
                                    if(dynamic && mapper) {
                                        void* canonical=nullptr;std::memcpy(&canonical,static_cast<char*>(dynamic)+48,sizeof canonical);
                                        const std::vector<unsigned char> frozen(static_cast<unsigned char*>(alias),static_cast<unsigned char*>(alias)+pixels.bytes);
                                        for(UINT pass=0;pass<2;++pass) {
                                            bind_targets(mapper,1,targets,nullptr);
                                            reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,UINT,const D3D11_VIEWPORT*)>(vt(mapper)[44])(mapper,1,&viewport);
                                            auto* data=static_cast<char*>(mapper);data[0x390]=1;
                                            std::memcpy(data+0x170,&topology,sizeof topology);std::memcpy(data+0x178,&vertex_layout,sizeof vertex_layout);
                                            std::memcpy(data+0x180,&shaders[3],sizeof(void*));std::memcpy(data+0x188,&shaders[2],sizeof(void*));
                                            bind_vertices(mapper,0,1,&dynamic,&stride,&offset);
                                            const auto map=reinterpret_cast<MapDeferred>(vt(mapper)[15]);
                                            const auto unmap=reinterpret_cast<UnmapDeferred>(vt(mapper)[16]);
                                            D3D11_MAPPED_SUBRESOURCE mapped{},nested{};
                                            unmap(mapper,dynamic);
                                            check(map(mapper,dynamic,0,D3D11_MAP_WRITE_NO_OVERWRITE,0,&mapped)==DXGI_ERROR_INVALID_CALL && !mapped.pData,
                                                  "each deferred list requires WRITE_DISCARD before NO_OVERWRITE");
                                            const HRESULT result=map(mapper,dynamic,0,D3D11_MAP_WRITE_DISCARD,0,&mapped);
                                            check(result==S_OK && mapped.pData && mapped.pData!=canonical && mapped.RowPitch==sizeof vertices && mapped.DepthPitch==sizeof vertices,
                                                  "deferred WRITE_DISCARD returns private version storage with original buffer pitches");
                                            if(SUCCEEDED(result)) {
                                                check(map(mapper,dynamic,0,D3D11_MAP_WRITE_DISCARD,0,&nested)==HRESULT_FROM_WIN32(ERROR_BUSY) && !nested.pData,
                                                      "nested deferred map is rejected without losing the valid mapping");
                                                void* invalid=reinterpret_cast<void*>(1);
                                                check(reinterpret_cast<FinishDeferred>(vt(mapper)[114])(mapper,FALSE,&invalid)==DXGI_ERROR_INVALID_CALL && !invalid,
                                                      "unfinished CPU writes cannot be published as a Command List");
                                                if(pass) std::memset(mapped.pData,0,sizeof vertices);else std::memcpy(mapped.pData,vertices,sizeof vertices);
                                                unmap(mapper,dynamic);
                                                unmap(mapper,dynamic);
                                                check(map(mapper,dynamic,0,D3D11_MAP_WRITE_NO_OVERWRITE,0,&nested)==S_OK && nested.pData==mapped.pData,
                                                      "deferred NO_OVERWRITE preserves the current version");
                                                if(nested.pData) unmap(mapper,dynamic);
                                                reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*,const FLOAT*)>(vt(mapper)[50])(mapper,view,black);
                                                draw(mapper,3,1);
                                            }
                                            check(reinterpret_cast<FinishDeferred>(vt(mapper)[114])(mapper,FALSE,&versions[pass])==S_OK && versions[pass],
                                                  "mapped buffer version is retained by its finished list");
                                            check(canonical && !std::memcmp(canonical,vertices,sizeof vertices) && !std::memcmp(frozen.data(),alias,frozen.size()),
                                                  "deferred recording never overwrites immediate buffer bytes or target pixels");
                                        }
                                    }
                                    if(mapper) reinterpret_cast<Count>(vt(mapper)[2])(mapper);
                                    if(dynamic) reinterpret_cast<Count>(vt(dynamic)[2])(dynamic);
                                    for(UINT pass : {0u,1u,0u}) if(versions[pass]) {
                                        reinterpret_cast<ExecuteDeferred>(vt(context)[58])(context,versions[pass],FALSE);
                                        check(pixels.copy(pixels.context.get(),false,linear.data(),alias,0,32,512,nullptr)==S_OK &&
                                            std::all_of(linear.begin(),linear.end(),[&](auto value){return value==(pass ? 0 : 191);}),
                                            "separate WRITE_DISCARD versions replay exact pixels after producer/resource release and remain reusable");
                                    }
                                    for(auto* version:versions) if(version) reinterpret_cast<Count>(vt(version)[2])(version);
                                    const char* compute_source="Texture2D<float> src:register(t0); Buffer<float4> halfs:register(t1); Buffer<uint> words:register(t2); Texture2D<float4> alpha:register(t3); RWTexture2D<float> dst:register(u0); SamplerState point_sampler:register(s0); cbuffer C:register(b0){float amount;}; [numthreads(8,8,1)] void main(uint3 id:SV_DispatchThreadID){float4 mask=alpha.Load(int3(127,0,0));bool valid=src.SampleLevel(point_sampler,float2(-1,-1),0)==1 && all(mask.rgb==0) && mask.a>.599 && mask.a<.601 && all(halfs.Load(1)==float4(1,.5,-2,4)) && all(halfs.Load(2)==0) && words.Load(2)==400 && words.Load(3)==65530 && words.Load(4)==0;dst[id.xy]=valid ? src.SampleLevel(point_sampler,(float2(id.xy)+.5)/float2(32,16),0)+amount:0;}";
                                    Microsoft::WRL::ComPtr<ID3DBlob> compute_code;
                                    bool compute_ready=SUCCEEDED(compile(compute_source,std::strlen(compute_source),nullptr,nullptr,nullptr,"main","cs_5_0",0,0,&compute_code,nullptr));
                                    void* compute_objects[12]{}; // CS, texture/SRV/UAV, sampler, CBV, typed buffers/views, alpha texture/view.
                                    if(compute_ready) compute_ready=reinterpret_cast<CreateShader>(vt(device)[18])(device,compute_code->GetBufferPointer(),compute_code->GetBufferSize(),nullptr,&compute_objects[0])==S_OK;
                                    std::array<BYTE,512> source_pixels;source_pixels.fill(51);
                                    auto input_desc=color;input_desc.standard.BindFlags=D3D11_BIND_SHADER_RESOURCE;
                                    const D3D11_SUBRESOURCE_DATA source_data{source_pixels.data(),32,512};
                                    if(compute_ready) compute_ready=reinterpret_cast<CreateOwned>(vt(device)[5])(device,&input_desc,&source_data,&compute_objects[1])==S_OK;
                                    if(compute_ready) compute_ready=reinterpret_cast<MakeSrv>(vt(device)[7])(device,compute_objects[1],&srv,&compute_objects[2])==S_OK;
                                    using MakeComputeUav=HRESULT(STDMETHODCALLTYPE*)(void*,void*,const D3D11_UNORDERED_ACCESS_VIEW_DESC*,void**);
                                    D3D11_UNORDERED_ACCESS_VIEW_DESC output_desc{};output_desc.Format=DXGI_FORMAT_R8_UNORM;output_desc.ViewDimension=D3D11_UAV_DIMENSION_TEXTURE2D;
                                    if(compute_ready) compute_ready=reinterpret_cast<MakeComputeUav>(vt(device)[8])(device,texture,&output_desc,&compute_objects[3])==S_OK;
                                    using MakeComputeSampler=HRESULT(STDMETHODCALLTYPE*)(void*,const D3D11_SAMPLER_DESC*,void**);
                                    D3D11_SAMPLER_DESC point{};point.Filter=D3D11_FILTER_MIN_MAG_MIP_POINT;point.AddressU=point.AddressV=point.AddressW=D3D11_TEXTURE_ADDRESS_BORDER;
                                    point.MaxAnisotropy=1;point.ComparisonFunc=D3D11_COMPARISON_NEVER;point.MaxLOD=D3D11_FLOAT32_MAX;
                                    if(compute_ready) compute_ready=reinterpret_cast<MakeComputeSampler>(vt(device)[23])(device,&point,&compute_objects[4])==S_OK;
                                    if(compute_ready) {
                                        std::array<UINT32,4> descriptor{};
                                        reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,UINT32*)>(vt(compute_objects[4])[9])(compute_objects[4],descriptor.data());
                                        reinterpret_cast<Count>(vt(compute_objects[4])[2])(compute_objects[4]);compute_objects[4]=nullptr;
                                        descriptor[3]=(descriptor[3]&0x3fffffffu)|0x80000000u;
                                        compute_ready=reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*,const UINT32*,void**)>(vt(device)[67])(device,descriptor.data(),&compute_objects[4])==S_OK;
                                    }
                                    DynamicDesc constant_desc{{16,D3D11_USAGE_DYNAMIC,D3D11_BIND_CONSTANT_BUFFER,D3D11_CPU_ACCESS_WRITE,0,0},0};
                                    const FLOAT amounts[4]{.05f,0,0,0};const D3D11_SUBRESOURCE_DATA constant_data{amounts,0,0};
                                    if(compute_ready) compute_ready=reinterpret_cast<CreateDynamic>(vt(device)[3])(device,&constant_desc,&constant_data,&compute_objects[5])==S_OK;
                                    const std::array<UINT16,16> half_records{0,0,0,0,0,0,0,0,0x3c00,0x3800,0xc000,0x4400,0,0,0,0};
                                    const std::array<UINT16,8> word_records{17,23,101,400,65530,7,9,11};
                                    for(UINT typed=0;compute_ready && typed<2;++typed) {
                                        const auto bytes=typed ? sizeof word_records:sizeof half_records;
                                        DynamicDesc typed_desc{{UINT(bytes),D3D11_USAGE_DEFAULT,D3D11_BIND_SHADER_RESOURCE,0,typed ? 0u:UINT(D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS),0},0};
                                        D3D11_SUBRESOURCE_DATA typed_data{typed ? static_cast<const void*>(word_records.data()):static_cast<const void*>(half_records.data()),0,0};
                                        const UINT buffer_slot=typed ? 8:6,view_slot=buffer_slot+1;
                                        compute_ready=reinterpret_cast<CreateDynamic>(vt(device)[3])(device,&typed_desc,&typed_data,&compute_objects[buffer_slot])==S_OK;
                                        D3D11_SHADER_RESOURCE_VIEW_DESC typed_view{};typed_view.Format=typed ? DXGI_FORMAT_R16_UINT:DXGI_FORMAT_R16G16B16A16_FLOAT;
                                        typed_view.ViewDimension=D3D11_SRV_DIMENSION_BUFFER;typed_view.Buffer={1,typed ? 4u:2u};
                                        if(compute_ready)compute_ready=reinterpret_cast<MakeSrv>(vt(device)[7])(device,compute_objects[buffer_slot],&typed_view,&compute_objects[view_slot])==S_OK;
                                        if(compute_ready) {
                                            std::array<UINT,4> descriptor{};std::memcpy(descriptor.data(),static_cast<char*>(compute_objects[view_slot])+48,sizeof descriptor);
                                            check((descriptor[1]&0x3fff0000)==((typed ? 2u:8u)<<16) && descriptor[2]==(typed ? 4u:2u) &&
                                                descriptor[3]==(typed ? 0x14204u:0x67facu),"typed buffer SRD matches original UMD 180099860 stride, range, numeric format and swizzle");
                                            D3D11_SHADER_RESOURCE_VIEW_DESC actual{};
                                            reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,D3D11_SHADER_RESOURCE_VIEW_DESC*)>(vt(compute_objects[view_slot])[9])(compute_objects[view_slot],&actual);
                                            check(!std::memcmp(&actual,&typed_view,sizeof actual),"typed buffer GetDesc preserves its original sliced descriptor");
                                            auto invalid=typed_view;invalid.Buffer.NumElements=UINT(bytes);
                                            void* rejected=reinterpret_cast<void*>(1);
                                            check(reinterpret_cast<MakeSrv>(vt(device)[7])(device,compute_objects[buffer_slot],&invalid,&rejected)==E_INVALIDARG && !rejected,
                                                  "typed buffer slice cannot address elements beyond its logical bytes");
                                            reinterpret_cast<Count>(vt(compute_objects[buffer_slot])[2])(compute_objects[buffer_slot]);compute_objects[buffer_slot]=nullptr;
                                        }
                                    }
                                    check(compute_ready,"compute fixture creates typed half and uint16 SRVs, including a raw-capable typed buffer");
                                    auto alpha_desc=input_desc;alpha_desc.standard.Width=256;alpha_desc.standard.Height=1;
                                    alpha_desc.standard.Format=DXGI_FORMAT_A8_UNORM;alpha_desc.standard.Usage=D3D11_USAGE_IMMUTABLE;
                                    std::array<BYTE,256> alpha_bytes;alpha_bytes.fill(153);
                                    const D3D11_SUBRESOURCE_DATA alpha_data{alpha_bytes.data(),256,256};
                                    void* missing_alpha=reinterpret_cast<void*>(1);
                                    check(reinterpret_cast<CreateOwned>(vt(device)[5])(device,&alpha_desc,nullptr,&missing_alpha)==E_INVALIDARG && !missing_alpha,
                                          "immutable texture requires complete initial data and clears its failed output");
                                    if(compute_ready)compute_ready=reinterpret_cast<CreateOwned>(vt(device)[5])(device,&alpha_desc,&alpha_data,&compute_objects[10])==S_OK;
                                    if(compute_ready)compute_ready=reinterpret_cast<MakeSrv>(vt(device)[7])(device,compute_objects[10],nullptr,&compute_objects[11])==S_OK;
                                    check(compute_ready,"immutable A8 texture and default SRV preserve original alpha-only sampling");
                                    if(compute_ready) {
                                        using BindCompute=void(STDMETHODCALLTYPE*)(void*,UINT,UINT,void* const*);
                                        using Dispatch=void(STDMETHODCALLTYPE*)(void*,UINT,UINT,UINT);
                                        const auto bind_compute=[&](void* ctx) {
                                            std::array<BYTE,9> cached_compute{};
                                            std::memcpy(cached_compute.data(),static_cast<char*>(ctx)+0x1f0,cached_compute.size());
                                            reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*)>(vt(ctx)[69])(ctx,compute_objects[0]);
                                            if(static_cast<BYTE*>(ctx)[0x390]&2)check(!std::memcmp(cached_compute.data(),static_cast<char*>(ctx)+0x1f0,cached_compute.size()),
                                                "reference out-of-line compute binding preserves caller cache and flags while retaining the shader");
                                            void* resources[]{compute_objects[2],compute_objects[7],compute_objects[9],compute_objects[11]};
                                            reinterpret_cast<BindCompute>(vt(ctx)[67])(ctx,0,4,resources);
                                            reinterpret_cast<BindCompute>(vt(ctx)[68])(ctx,0,1,&compute_objects[3]);
                                            reinterpret_cast<BindCompute>(vt(ctx)[70])(ctx,0,1,&compute_objects[4]);
                                            reinterpret_cast<BindCompute>(vt(ctx)[71])(ctx,0,1,&compute_objects[5]);
                                        };
                                        Desc sample_desc{};sample_desc.standard={32,16,1,1,DXGI_FORMAT_R11G11B10_FLOAT,{4,0},D3D11_USAGE_DEFAULT,D3D11_BIND_RENDER_TARGET|D3D11_BIND_SHADER_RESOURCE,0,0};
                                        void* sample_texture=nullptr;void* sample_view=nullptr;
                                        void* cached_texture=nullptr;void* cached_view=nullptr;
                                        check(reinterpret_cast<CreateOwned>(vt(device)[5])(device,&sample_desc,nullptr,&sample_texture)==S_OK && sample_texture,
                                              "vertex sampling creates a real four-sample original-format texture");
                                        if(sample_texture) {
                                            D3D11_SHADER_RESOURCE_VIEW_DESC sample_srv{};sample_srv.Format=DXGI_FORMAT_R11G11B10_FLOAT;sample_srv.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2DMS;
                                            check(reinterpret_cast<MakeSrv>(vt(device)[7])(device,sample_texture,&sample_srv,&sample_view)==S_OK && sample_view,
                                                  "vertex sampling creates the native Texture2DMS SRV");
                                        }
                                        if(sample_view) {
                                            void* sample_bytes=nullptr;std::memcpy(&sample_bytes,static_cast<char*>(sample_view)+80,sizeof sample_bytes);
                                            TitleTextureLayout sample_layout;
                                            std::vector<UINT> samples(32*16*4);constexpr UINT red[]{0x300,0x340,0x380,0x3c0}; // R11 .125, .25, .5, 1.
                                            for(SIZE_T i=0;i<samples.size();++i) samples[i]=red[i%4];
                                            check(create_xg_texture_layout(32,16,1,1,DXGI_FORMAT_R11G11B10_FLOAT,UINT_MAX,sample_layout,sample_desc.standard.BindFlags,4)==S_OK &&
                                                sample_layout.copy(sample_layout.context.get(),true,samples.data(),sample_bytes,0,32*4*4,32*16*4*4,nullptr)==S_OK,
                                                "MSAA fixture records four distinct canonical samples per pixel through original XG");
                                            void* cached_bytes=nullptr;std::memcpy(&cached_bytes,static_cast<char*>(compute_objects[2])+80,sizeof cached_bytes);
                                            TitleTextureLayout cached_layout;
                                            check(create_xg_texture_layout(32,16,1,1,input_desc.standard.Format,UINT_MAX,cached_layout,input_desc.standard.BindFlags)==S_OK &&
                                                place_texture(device,&input_desc,cached_layout.tile_mode,0,cached_bytes,&cached_texture)==S_OK && cached_texture &&
                                                reinterpret_cast<MakeSrv>(vt(device)[7])(device,cached_texture,&srv,&cached_view)==S_OK && cached_view,
                                                "a second logical texture binds the same native storage through its own SRV");
                                        }
                                        const char* vertex_source="Texture2DMS<float3> a:register(t4); Texture2D<float> cached:register(t5); Texture2D<float> b:register(t7); SamplerState vertex_border:register(s3); Texture2D<float4> n:register(t9); Texture2D<uint4> u:register(t10); Texture2DMS<float4> m:register(t11); Buffer<float4> q:register(t12); Texture2D<int4> s:register(t13); Buffer<float4> halfs:register(t14); Buffer<uint> words:register(t15); cbuffer C:register(b2){float amount;}; float4 main(float2 p:POSITION,float2 uv:TEXCOORD0):SV_Position {bool valid=all(halfs.Load(1)==float4(1,.5,-2,4)) && all(halfs.Load(2)==0) && words.Load(2)==400 && words.Load(3)==65530 && words.Load(4)==0 && b.SampleLevel(vertex_border,float2(-1,-1),0)==1 && a.Load(int2(0,0),0).x==.125 && a.Load(int2(0,0),1).x==.25 && a.Load(int2(0,0),2).x==.5 && a.Load(int2(0,0),3).x==1 && cached.Load(int3(0,0,0))==b.Load(int3(0,0,0)) && abs(b.Load(int3(0,0,0))+amount-.25)<.001 && all(n.Load(int3(0,0,0))==0) && all(u.Load(int3(0,0,0))==0) && all(m.Load(int2(0,0),0)==0) && all(q.Load(0)==0) && all(s.Load(int3(0,0,0))==0);return float4((p+uv)*(valid ? 1:0),0,1);}";
                                        Microsoft::WRL::ComPtr<ID3DBlob> vertex_code;void* resource_vertex=nullptr;
                                        check(SUCCEEDED(compile(vertex_source,std::strlen(vertex_source),nullptr,nullptr,nullptr,"main","vs_5_0",0,0,&vertex_code,nullptr)) &&
                                            reinterpret_cast<CreateShader>(vt(device)[12])(device,vertex_code->GetBufferPointer(),vertex_code->GetBufferSize(),nullptr,&resource_vertex)==S_OK && resource_vertex,
                                            "vertex resource fixture creates a shader using sparse t4/t7 and b2 bindings");
                                        void* vertex_sampler=nullptr;
                                        auto border=point;border.AddressU=border.AddressV=border.AddressW=D3D11_TEXTURE_ADDRESS_BORDER;
                                        std::fill(std::begin(border.BorderColor),std::end(border.BorderColor),1.f);
                                        check(reinterpret_cast<MakeComputeSampler>(vt(device)[23])(device,&border,&vertex_sampler)==S_OK && vertex_sampler,
                                              "vertex stage has its own white-border sampler distinct from the pixel clamp sampler");
                                        std::array<FLOAT,128> vertex_constants_bytes{};vertex_constants_bytes[0]=.5f;vertex_constants_bytes[4]=.05f;
                                        DynamicDesc placement_constants{{sizeof vertex_constants_bytes,D3D11_USAGE_DEFAULT,D3D11_BIND_CONSTANT_BUFFER,0,0,0},0};
                                        D3D11_SUBRESOURCE_DATA placement_data{vertex_constants_bytes.data(),0,0};
                                        void* vertex_constants=nullptr;void* vertex_constants_base=nullptr;
                                        check(reinterpret_cast<CreateDynamic>(vt(device)[3])(device,&placement_constants,&placement_data,&vertex_constants)==S_OK && vertex_constants,
                                              "offset constants use an owned parent containing a distinct wrong value");
                                        if(vertex_constants)std::memcpy(&vertex_constants_base,static_cast<char*>(vertex_constants)+48,sizeof vertex_constants_base);
                                        if(resource_vertex && vertex_sampler && vertex_constants) {
                                            const auto bind_vertex=[&](void* ctx) {
                                                bind_targets(ctx,1,targets,nullptr);
                                                reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,UINT,const D3D11_VIEWPORT*)>(vt(ctx)[44])(ctx,1,&viewport);
                                                static_cast<BYTE*>(ctx)[0x390]=3;
                                                std::memset(static_cast<char*>(ctx)+0x180,0,16);
                                                void* cached_layout=nullptr;std::memcpy(&cached_layout,static_cast<char*>(ctx)+0x178,8);
                                                using BindObject=void(STDMETHODCALLTYPE*)(void*,void*);
                                                reinterpret_cast<BindObject>(vt(ctx)[12])(ctx,resource_vertex);
                                                reinterpret_cast<BindObject>(vt(ctx)[10])(ctx,shaders[2]);
                                                reinterpret_cast<BindObject>(vt(ctx)[18])(ctx,vertex_layout);
                                                std::array<void*,3> inline_cache{};
                                                std::memcpy(inline_cache.data(),static_cast<char*>(ctx)+0x178,sizeof inline_cache);
                                                check(inline_cache[0]==cached_layout && !inline_cache[1] && !inline_cache[2],
                                                      "reference out-of-line shaders and layout retain real state without overwriting guest inline caches");
                                                std::memcpy(static_cast<char*>(ctx)+0x170,&topology,sizeof topology);
                                                bind_vertices(ctx,0,1,&vertex_buffer,&stride,&offset);
                                                void* sampled_aliases[]{sample_view,cached_view};
                                                reinterpret_cast<BindCompute>(vt(ctx)[25])(ctx,4,2,sampled_aliases);
                                                reinterpret_cast<BindCompute>(vt(ctx)[25])(ctx,7,1,&compute_objects[2]);
                                                reinterpret_cast<BindCompute>(vt(ctx)[25])(ctx,14,1,&compute_objects[7]);
                                                reinterpret_cast<BindCompute>(vt(ctx)[25])(ctx,15,1,&compute_objects[9]);
                                                if(ctx==context)reinterpret_cast<BindCompute>(vt(ctx)[8])(ctx,2,1,&vertex_constants);
                                                reinterpret_cast<BindCompute>(vt(ctx)[26])(ctx,3,1,&compute_objects[4]);
                                                reinterpret_cast<BindCompute>(vt(ctx)[11])(ctx,3,1,&compute_objects[4]);
                                                const UINT zero_stride=0;
                                                bind_vertices(ctx,0,1,&vertex_buffer,&zero_stride,&offset);
                                                std::array<BYTE,80> packets{};SIZE_T packet_offset=0;
                                                const auto packet=[&](UINT header,void* object,UINT64 metadata=0) {
                                                    std::memcpy(packets.data()+packet_offset,&header,4);
                                                    std::memcpy(packets.data()+packet_offset+4,&object,8);
                                                    std::memcpy(packets.data()+packet_offset+12,&metadata,8);packet_offset+=20;
                                                };
                                                packet(0xa0000301,vertex_sampler);
                                                packet(0x91060001,vertex_buffer,(UINT64(stride)<<48)|offset);
                                                packet(0x80000701,compute_objects[2]);
                                                packet(0x91000201,vertex_constants,16);
                                                reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,const void*,const void*)>(vt(ctx)[209])(
                                                    ctx,packets.data(),packets.data()+packet_offset);
                                            };
                                            reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*,const FLOAT*)>(vt(context)[50])(context,view,black);
                                            bind_vertex(context);draw(context,3,1);
                                            check(pixels.copy(pixels.context.get(),false,linear.data(),alias,0,32,512,nullptr)==S_OK &&
                                                std::all_of(linear.begin(),linear.end(),[](auto value){return value==191;}),
                                                "vertex shader reads four distinct MSAA samples, sparse t7 and b2 through real non-pixel resource barriers");
                                            using SetScissors=void(STDMETHODCALLTYPE*)(void*,UINT,const D3D11_RECT*);
                                            using GetScissors=void(STDMETHODCALLTYPE*)(void*,UINT*,D3D11_RECT*);
                                            using SetRaster=void(STDMETHODCALLTYPE*)(void*,void*);
                                            D3D11_RASTERIZER_DESC clipped_desc{};
                                            clipped_desc.FillMode=D3D11_FILL_SOLID;clipped_desc.CullMode=D3D11_CULL_NONE;
                                            clipped_desc.DepthClipEnable=TRUE;clipped_desc.ScissorEnable=TRUE;
                                            void* clipped_raster=nullptr;
                                            check(reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*,const D3D11_RASTERIZER_DESC*,void**)>(vt(device)[22])(
                                                device,&clipped_desc,&clipped_raster)==S_OK && clipped_raster,"reference scissor draw creates its native raster state");
                                            const D3D11_RECT left_half{0,0,16,16},right_half{16,0,32,16};
                                            const bool clipped=clipped_raster!=nullptr;
                                            const auto clipped_pixels=[&] {
                                                for(SIZE_T i=0;i<linear.size();++i)if(linear[i]!=(i%32<16 ? 191:0))return false;
                                                return true;
                                            };
                                            if(clipped) {
                                                reinterpret_cast<SetRaster>(vt(context)[43])(context,clipped_raster);
                                                reinterpret_cast<SetScissors>(vt(context)[45])(context,1,&left_half);
                                                std::array<D3D11_RECT,17> query{};query.back()={1,2,3,4};UINT requested=17;
                                                reinterpret_cast<GetScissors>(vt(context)[96])(context,&requested,query.data());
                                                check(requested==1 && !std::memcmp(query.data(),&left_half,sizeof left_half) &&
                                                    query[1].right==0 && query[15].bottom==0 && query[16].left==1,
                                                    "reference scissor getter returns actual count, zero fills and caps writes at sixteen");
                                                reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*,const FLOAT*)>(vt(context)[50])(context,view,black);
                                                draw(context,3,1);
                                                check(pixels.copy(pixels.context.get(),false,linear.data(),alias,0,32,512,nullptr)==S_OK && clipped_pixels(),
                                                      "native scissor retains the left half and leaves pixels outside the rectangle untouched");
                                                reinterpret_cast<SetScissors>(vt(context)[45])(context,0,nullptr);
                                                for(bool read_clear : {false,true}) {
                                                    reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*,const FLOAT*)>(vt(context)[50])(context,view,black);
                                                    if(read_clear)check(pixels.copy(pixels.context.get(),false,linear.data(),alias,0,32,512,nullptr)==S_OK &&
                                                        std::all_of(linear.begin(),linear.end(),[](auto value){return value==0;}),
                                                        "clear before an empty scissor draw publishes exact zero pixels");
                                                    draw(context,3,1);
                                                    const auto empty_clip_copy=pixels.copy(pixels.context.get(),false,linear.data(),alias,0,32,512,nullptr);
                                                    const auto empty_clip_nonzero=std::count_if(linear.begin(),linear.end(),[](auto value){return value!=0;});
                                                    if(empty_clip_copy!=S_OK || empty_clip_nonzero)
                                                        std::fprintf(stderr,"empty scissor readback hr=0x%08lX nonzero=%zu first=%u last=%u\n",empty_clip_copy,SIZE_T(empty_clip_nonzero),UINT(linear.front()),UINT(linear.back()));
                                                    check(empty_clip_copy==S_OK && !empty_clip_nonzero,
                                                        "enabled scissor with no rectangles clips every fragment without skipping the draw");
                                                }
                                                reinterpret_cast<SetRaster>(vt(context)[43])(context,nullptr);
                                            }
                                            void* vertex_producer=nullptr;void* vertex_list=nullptr;
                                            check(reinterpret_cast<Deferred>(vt(device)[27])(device,0x60000,&vertex_producer)==S_OK && vertex_producer,
                                                  "vertex resource draw uses an existing deferred producer");
                                            if(vertex_producer) {
                                                bind_vertex(vertex_producer);
                                                if(clipped) {
                                                    reinterpret_cast<SetRaster>(vt(vertex_producer)[43])(vertex_producer,clipped_raster);
                                                    reinterpret_cast<SetScissors>(vt(vertex_producer)[45])(vertex_producer,1,&left_half);
                                                }
                                                draw(vertex_producer,3,1);
                                                if(clipped)reinterpret_cast<SetScissors>(vt(vertex_producer)[45])(vertex_producer,1,&right_half);
                                                check(reinterpret_cast<FinishDeferred>(vt(vertex_producer)[114])(vertex_producer,FALSE,&vertex_list)==S_OK && vertex_list,
                                                      "sparse vertex bindings survive FinishCommandList");
                                                reinterpret_cast<Count>(vt(vertex_producer)[2])(vertex_producer);
                                            }
                                            if(clipped_raster)reinterpret_cast<Count>(vt(clipped_raster)[2])(clipped_raster);
                                            reinterpret_cast<void(STDMETHODCALLTYPE*)(void*)>(vt(context)[110])(context);
                                            reinterpret_cast<Count>(vt(vertex_constants)[2])(vertex_constants);vertex_constants=nullptr;
                                            MEMORY_BASIC_INFORMATION constant_info{};
                                            check(VirtualQuery(vertex_constants_base,&constant_info,sizeof constant_info) && constant_info.State==MEM_COMMIT,
                                                  "offset alias retains the owned allocation after the parent buffer is destroyed");
                                            UINT cleared_count=16;std::array<D3D11_RECT,16> cleared_rectangles{};
                                            reinterpret_cast<GetScissors>(vt(context)[96])(context,&cleared_count,cleared_rectangles.data());
                                            check(cleared_count==0 && cleared_rectangles[0].right==0,"ClearState clears the shared scissor state");
                                            reinterpret_cast<Count>(vt(resource_vertex)[2])(resource_vertex);
                                            resource_vertex=nullptr;
                                            reinterpret_cast<Count>(vt(vertex_sampler)[2])(vertex_sampler);vertex_sampler=nullptr;
                                            if(sample_view) {reinterpret_cast<Count>(vt(sample_view)[2])(sample_view);sample_view=nullptr;}
                                            if(sample_texture) {reinterpret_cast<Count>(vt(sample_texture)[2])(sample_texture);sample_texture=nullptr;}
                                            if(cached_view) {reinterpret_cast<Count>(vt(cached_view)[2])(cached_view);cached_view=nullptr;}
                                            if(cached_texture) {reinterpret_cast<Count>(vt(cached_texture)[2])(cached_texture);cached_texture=nullptr;}
                                            for(UINT pass=0;pass<2 && vertex_list;++pass) {
                                                reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*,const FLOAT*)>(vt(context)[50])(context,view,black);
                                                reinterpret_cast<ExecuteDeferred>(vt(context)[58])(context,vertex_list,FALSE);
                                                check(pixels.copy(pixels.context.get(),false,linear.data(),alias,0,32,512,nullptr)==S_OK &&
                                                    (clipped ? clipped_pixels():std::all_of(linear.begin(),linear.end(),[](auto value){return value==191;})),
                                                    "deferred sparse vertex descriptors retain exact fenced pixels after producer and shader release on reuse");
                                            }
                                            if(vertex_list) reinterpret_cast<Count>(vt(vertex_list)[2])(vertex_list);
                                            reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,UINT)>(vt(context)[197])(context,0x407);
                                            check(VirtualQuery(vertex_constants_base,&constant_info,sizeof constant_info) && constant_info.State==MEM_FREE,
                                                  "last recorded offset alias retires the owned allocation after replay");
                                        }
                                        if(vertex_constants)reinterpret_cast<Count>(vt(vertex_constants)[2])(vertex_constants);
                                        if(resource_vertex)reinterpret_cast<Count>(vt(resource_vertex)[2])(resource_vertex);
                                        if(vertex_sampler)reinterpret_cast<Count>(vt(vertex_sampler)[2])(vertex_sampler);
                                        if(sample_view) reinterpret_cast<Count>(vt(sample_view)[2])(sample_view);
                                        if(sample_texture) reinterpret_cast<Count>(vt(sample_texture)[2])(sample_texture);
                                        if(cached_view) reinterpret_cast<Count>(vt(cached_view)[2])(cached_view);
                                        if(cached_texture) reinterpret_cast<Count>(vt(cached_texture)[2])(cached_texture);
                                        bind_compute(context);reinterpret_cast<Dispatch>(vt(context)[41])(context,4,2,1);
                                        reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,UINT)>(vt(context)[197])(context,0x407);
                                        for(const UINT profile:{0x400000u,0x80c00000u})
                                            reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,UINT,void*,SIZE_T)>(vt(context)[152])(context,profile,nullptr,0);
                                        check(pixels.copy(pixels.context.get(),false,linear.data(),alias,0,32,512,nullptr)==S_OK &&
                                            std::all_of(linear.begin(),linear.end(),[](auto value){return value==64;}),
                                            "immediate Dispatch samples the original texture and writes every UAV pixel after a fence");
                                        {
                                            std::string changed=compute_source;const auto at=changed.find("+amount:0");
                                            check(at!=std::string::npos,"compute replacement fixture locates its output expression");
                                            if(at==std::string::npos)return 1;
                                            // Avoid a UNORM half-step after hardware sampling (RTX/WARP can round the preceding arithmetic differently).
                                            changed.replace(at,std::strlen("+amount:0"),"+amount+.125:0");
                                            Microsoft::WRL::ComPtr<ID3DBlob> replacement;
                                            const auto compiled=compile(changed.data(),changed.size(),nullptr,nullptr,nullptr,"main","cs_5_0",0,0,&replacement,nullptr);
                                            check(SUCCEEDED(compiled) && replacement,"compute replacement fixture compiles its different output");
                                            if(FAILED(compiled) || !replacement)return 1;
                                            for(UINT epoch=0;epoch<3;++epoch) {
                                                reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*)>(vt(context)[69])(context,nullptr);
                                                check(reinterpret_cast<Count>(vt(compute_objects[0])[2])(compute_objects[0])==0,
                                                    "native compute pipeline cache does not retain the guest shader object");compute_objects[0]=nullptr;
                                                auto* code=epoch==1 ? replacement.Get():compute_code.Get();
                                                const auto created=reinterpret_cast<CreateShader>(vt(device)[18])(device,code->GetBufferPointer(),code->GetBufferSize(),nullptr,&compute_objects[0]);
                                                check(created==S_OK && compute_objects[0],"compute shader is recreated after its previous object is destroyed");
                                                if(created!=S_OK || !compute_objects[0])return 1;
                                                bind_compute(context);reinterpret_cast<Dispatch>(vt(context)[41])(context,4,2,1);
                                                const auto copied=pixels.copy(pixels.context.get(),false,linear.data(),alias,0,32,512,nullptr);
                                                const bool exact=copied==S_OK && std::all_of(linear.begin(),linear.end(),[&](BYTE pixel){return pixel==(epoch==1 ? 96:64);});
                                                if(!exact){const auto bounds=std::minmax_element(linear.begin(),linear.end());std::fprintf(stderr,"compute replacement epoch=%u first=%u min=%u max=%u copy=0x%08lX\n",epoch,linear[0],*bounds.first,*bounds.second,copied);}
                                                check(exact,
                                                    "compute cache preserves exact pixels across identical bytecode recreation, shader replacement and restoring the earlier shader");
                                            }
                                            reinterpret_cast<Dispatch>(vt(context)[41])(context,0,2,1);
                                            check(pixels.copy(pixels.context.get(),false,linear.data(),alias,0,32,512,nullptr)==S_OK &&
                                                std::all_of(linear.begin(),linear.end(),[](BYTE pixel){return pixel==64;}),
                                                "cached compute pipeline uses the current dispatch dimensions and preserves pixels for zero groups");
                                        }
                                        void* producer=nullptr;void* compute_list=nullptr;
                                        check(reinterpret_cast<Deferred>(vt(device)[27])(device,0x60000,&producer)==S_OK && producer,"compute recording creates a deferred producer");
                                        if(producer) {
                                            bind_compute(producer);D3D11_MAPPED_SUBRESOURCE mapped{};
                                            const HRESULT result=reinterpret_cast<MapDeferred>(vt(producer)[15])(producer,compute_objects[5],0,D3D11_MAP_WRITE_DISCARD,0,&mapped);
                                            check(result==S_OK && mapped.pData,"deferred compute maps its private constant-buffer version");
                                            if(SUCCEEDED(result)) {
                                                const FLOAT updated[4]{.55f,0,0,0};std::memcpy(mapped.pData,updated,sizeof updated);
                                                reinterpret_cast<UnmapDeferred>(vt(producer)[16])(producer,compute_objects[5]);
                                                reinterpret_cast<Dispatch>(vt(producer)[41])(producer,4,2,1);
                                                reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,UINT)>(vt(producer)[197])(producer,0x407);
                                                reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,UINT)>(vt(producer)[197])(producer,0x80000001u);
                                                for(const UINT profile:{0x400000u,0x80c00000u})
                                                    reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,UINT,void*,SIZE_T)>(vt(producer)[152])(producer,profile,nullptr,0);
                                            }
                                            check(reinterpret_cast<FinishDeferred>(vt(producer)[114])(producer,FALSE,&compute_list)==S_OK && compute_list,
                                                  "deferred Dispatch finishes without executing");
                                            check(pixels.copy(pixels.context.get(),false,linear.data(),alias,0,32,512,nullptr)==S_OK &&
                                                std::all_of(linear.begin(),linear.end(),[](auto value){return value==64;}),"deferred compute leaves canonical pixels unchanged during recording");
                                            reinterpret_cast<Count>(vt(producer)[2])(producer);
                                        }
                                        reinterpret_cast<void(STDMETHODCALLTYPE*)(void*)>(vt(context)[110])(context);
                                        for(auto*& object:compute_objects) if(object) {reinterpret_cast<Count>(vt(object)[2])(object);object=nullptr;}
                                        if(compute_list) {
                                            for(UINT pass=0;pass<2;++pass) {
                                                reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*,const FLOAT*)>(vt(context)[50])(context,view,black);
                                                reinterpret_cast<ExecuteDeferred>(vt(context)[58])(context,compute_list,FALSE);
                                                check(pixels.copy(pixels.context.get(),false,linear.data(),alias,0,32,512,nullptr)==S_OK &&
                                                    std::all_of(linear.begin(),linear.end(),[](auto value){return value==191;}),
                                                    "deferred Dispatch retains CS/SRV/UAV/sampler and renamed constants after producer release, with exact pixels on reuse");
                                            }
                                            reinterpret_cast<Count>(vt(compute_list)[2])(compute_list);
                                        }
                                    }
                                    for(auto* object:compute_objects) if(object) reinterpret_cast<Count>(vt(object)[2])(object);
                                    reinterpret_cast<void(STDMETHODCALLTYPE*)(void*)>(vt(context)[110])(context);
                                    void* retained_layout=nullptr;
                                    std::memcpy(&retained_layout,static_cast<char*>(context)+0x178,sizeof retained_layout);
                                    check(retained_layout==vertex_layout && reinterpret_cast<Count>(vt(vertex_layout)[1])(vertex_layout)==3,
                                          "11295 ClearState preserves its public input-layout cache and retained ownership");
                                    reinterpret_cast<Count>(vt(vertex_layout)[2])(vertex_layout);
                                }
                                for(auto* shader:shaders) if(shader) reinterpret_cast<Count>(vt(shader)[2])(shader);
                                if(empty_layout) reinterpret_cast<Count>(vt(empty_layout)[2])(empty_layout);
                                if(vertex_layout) reinterpret_cast<Count>(vt(vertex_layout)[2])(vertex_layout);
                                if(vertex_buffer) reinterpret_cast<Count>(vt(vertex_buffer)[2])(vertex_buffer);
                                if(compiler) FreeLibrary(compiler);
                            }
                            reinterpret_cast<Count>(vt(srv_view)[2])(srv_view);
                        }
                    }
                    void* parent=nullptr;
                    reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void**)>(vt(view)[8])(view,&parent);
                    check(parent==texture,"typed color views retain the same logical parent");
                    if (parent) reinterpret_cast<Count>(vt(parent)[2])(parent);
                    reinterpret_cast<Count>(vt(view)[2])(view);
                }
                using CreateUav=HRESULT(STDMETHODCALLTYPE*)(void*,void*,const D3D11_UNORDERED_ACCESS_VIEW_DESC*,void**);
                const auto create_uav=reinterpret_cast<CreateUav>(vt(device)[8]);
                D3D11_UNORDERED_ACCESS_VIEW_DESC invalid{};
                invalid.Format=formats[1]; invalid.ViewDimension=D3D11_UAV_DIMENSION_TEXTURE2D; invalid.Texture2D.MipSlice=1;
                void* view=nullptr;
                check(create_uav(device,texture,&invalid,&view)==E_INVALIDARG && !view,"texture UAV refuses an out-of-range mip");
                invalid.Format=DXGI_FORMAT_R32_FLOAT; invalid.Texture2D.MipSlice=0;
                check(create_uav(device,texture,&invalid,&view)==DXGI_ERROR_UNSUPPORTED && !view,"texture UAV refuses an incompatible format without replacing its parent");
                reinterpret_cast<Count>(vt(texture)[2])(texture);
            }
            if(graphics) {
                NativeUavClear native;
                if(FAILED(native.initialize(argc==5)))return 1;
                for(const UINT levels:{0u,1u}) {
                    Desc cube{};cube.standard={32,32,levels,6,DXGI_FORMAT_R32G32B32A32_FLOAT,{1,0},D3D11_USAGE_DEFAULT,
                        D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET|D3D11_BIND_UNORDERED_ACCESS,0,
                        D3D11_RESOURCE_MISC_TEXTURECUBE|D3D11_RESOURCE_MISC_GENERATE_MIPS};
                    void* resource=nullptr;
                    check(reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*,const Desc*,const D3D11_SUBRESOURCE_DATA*,void**)>(vt(device)[5])(device,&cube,nullptr,&resource)==S_OK && resource,
                        "reference RGBA32 float cubemap supports SRV RTV UAV and automatic or explicit mip counts");
                    if(!resource)return 1;
                    Desc queried{};reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,Desc*)>(vt(resource)[12])(resource,&queried);
                    check(queried.standard.Format==DXGI_FORMAT_R32G32B32A32_FLOAT && queried.standard.ArraySize==6 && queried.standard.MipLevels==(levels ? 1:6),
                        "RGBA32 float cubemap preserves format and faces and resolves a zero mip count");
                    void* view=nullptr;
                    check(reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*,void*,const D3D11_SHADER_RESOURCE_VIEW_DESC*,void**)>(vt(device)[7])(device,resource,nullptr,&view)==S_OK && view,
                        "RGBA32 float cubemap exposes its original cube SRV");
                    BYTE* canonical=nullptr;
                    if(view){std::memcpy(&canonical,static_cast<BYTE*>(view)+80,sizeof canonical);reinterpret_cast<Count>(vt(view)[2])(view);}
                    if(!canonical)return 1;
                    D3D11_RENDER_TARGET_VIEW_DESC selected{};selected.Format=cube.standard.Format;selected.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
                    selected.Texture2DArray={0,1,1};view=nullptr;
                    check(reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*,void*,const D3D11_RENDER_TARGET_VIEW_DESC*,void**)>(vt(device)[9])(device,resource,&selected,&view)==S_OK && view,
                        "RGBA32 float cube face exposes a native RTV");if(!view)return 1;
                    TitleTextureLayout pixels;
                    if(FAILED(create_xg_texture_layout(32,32,queried.standard.MipLevels,6,cube.standard.Format,UINT_MAX,pixels,cube.standard.BindFlags)))return 1;
                    std::memset(canonical,0x5a,pixels.bytes);
                    const std::array<UINT,4> color{0xbfa00000,0x3e000000,0x40600000,0x3f800000};
                    FLOAT clear[4];std::memcpy(clear,color.data(),sizeof clear);
                    reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*,const FLOAT*)>(vt(context)[50])(context,view,clear);
                    auto control=pixels.resource_desc;control.DepthOrArraySize=control.MipLevels=1;
                    std::vector<BYTE> expected,actual(32*32*16),untouched(actual.size());
                    if(FAILED(native.texture(control,cube.standard.Format,16,color,true,expected,true)))return 1;
                    check(pixels.copy(pixels.context.get(),false,actual.data(),canonical,queried.standard.MipLevels,512,16384,nullptr)==S_OK && actual==expected &&
                        pixels.copy(pixels.context.get(),false,untouched.data(),canonical,0,512,16384,nullptr)==S_OK &&
                        std::all_of(untouched.begin(),untouched.end(),[](BYTE value){return value==0x5a;}),
                        "RGBA32 float RTV clear matches every native component and preserves an unselected cube face");
                    reinterpret_cast<Count>(vt(view)[2])(view);reinterpret_cast<Count>(vt(resource)[2])(resource);
                }
                for(const auto format:{DXGI_FORMAT_R8G8B8A8_UNORM,DXGI_FORMAT_R32G32B32A32_FLOAT})for(const UINT faces:{1u,4u,6u})for(const UINT operation:{51u,52u})for(const bool remaining:{false,true}) {
                    const UINT element=format==DXGI_FORMAT_R32G32B32A32_FLOAT ? 16:4;
                    Desc desc{};desc.standard={32,32,3,faces,format,{1,0},D3D11_USAGE_DEFAULT,
                        D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS,0,faces==6 ? UINT(D3D11_RESOURCE_MISC_TEXTURECUBE):0};
                    void* resource=nullptr;void* view=nullptr;
                    check(reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*,const Desc*,const D3D11_SUBRESOURCE_DATA*,void**)>(vt(device)[5])(device,&desc,nullptr,&resource)==S_OK && resource,
                        "mipped UAV fixture owns a single texture or array");if(!resource)return 1;
                    const auto create=reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*,void*,const D3D11_UNORDERED_ACCESS_VIEW_DESC*,void**)>(vt(device)[8]);
                    check(create(device,resource,nullptr,&view)==S_OK && view,"reference default UAV accepts mips and arrays");if(!view)return 1;
                    D3D11_UNORDERED_ACCESS_VIEW_DESC queried{};
                    reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,D3D11_UNORDERED_ACCESS_VIEW_DESC*)>(vt(view)[9])(view,&queried);
                    check(queried.ViewDimension==(faces==1 ? D3D11_UAV_DIMENSION_TEXTURE2D:D3D11_UAV_DIMENSION_TEXTURE2DARRAY) &&
                        queried.Texture2D.MipSlice==0 && (faces==1 || queried.Texture2DArray.ArraySize==faces),"reference default UAV selects mip zero and every array slice");
                    reinterpret_cast<Count>(vt(view)[2])(view);view=nullptr;
                    D3D11_UNORDERED_ACCESS_VIEW_DESC selected{};selected.Format=desc.standard.Format;
                    selected.ViewDimension=faces==1 ? D3D11_UAV_DIMENSION_TEXTURE2D:D3D11_UAV_DIMENSION_TEXTURE2DARRAY;
                    selected.Texture2D.MipSlice=1;
                    const UINT first=faces==1 ? 0:1,count=faces==1 ? 1:remaining ? faces-first:2;
                    if(faces>1){selected.Texture2DArray.FirstArraySlice=first;selected.Texture2DArray.ArraySize=remaining ? UINT_MAX:count;}
                    check(create(device,resource,&selected,&view)==S_OK && view,"reference UAV selects one mip and a bounded slice range");if(!view)return 1;
                    reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,D3D11_UNORDERED_ACCESS_VIEW_DESC*)>(vt(view)[9])(view,&queried);
                    check(!std::memcmp(&selected,&queried,sizeof selected),"UAV GetDesc retains the requested remaining-slice sentinel");
                    std::array<UINT32,8> srd{};std::memcpy(srd.data(),static_cast<BYTE*>(view)+48,sizeof srd);
                    check((srd[3]&0xff000u)==0 && (srd[5]&0x3ffffffu)==(first|((first+count-1)<<13)) &&
                        (faces==1 || (srd[3]>>28)==13),"UAV SRD rebases the mip and retains the array range without turning cube faces into a cube sampler");
                    auto invalid=selected;invalid.Texture2D.MipSlice=3;void* rejected=nullptr;
                    check(create(device,resource,&invalid,&rejected)==E_INVALIDARG && !rejected,"mipped UAV rejects an out-of-resource mip");
                    if(faces>1) {
                        invalid=selected;invalid.Texture2DArray.FirstArraySlice=faces;
                        check(create(device,resource,&invalid,&rejected)==E_INVALIDARG && !rejected,"array UAV rejects an out-of-resource first slice");
                        invalid=selected;invalid.Texture2DArray.ArraySize=faces;
                        check(create(device,resource,&invalid,&rejected)==E_INVALIDARG && !rejected,"array UAV rejects a range beyond storage");
                    }
                    TitleTextureLayout pixels,control;
                    if(FAILED(create_xg_texture_layout(32,32,3,faces,desc.standard.Format,UINT_MAX,pixels,desc.standard.BindFlags)) ||
                        FAILED(create_xg_texture_layout(16,16,1,1,desc.standard.Format,UINT_MAX,control,desc.standard.BindFlags)))return 1;
                    BYTE* canonical=nullptr;std::memcpy(&canonical,static_cast<BYTE*>(view)+80,sizeof canonical);
                    std::array<UINT,4> values=operation==51 ? std::array<UINT,4>{0x61,0x12,0x34,0x56}:std::array<UINT,4>{0x3e800000,0x3f000000,0x3f400000,0x3f800000};
                    std::vector<BYTE> expected;
                    if(FAILED(native.texture(control.resource_desc,desc.standard.Format,element,values,operation==52,expected)))return 1;
                    const auto verify=[&] {
                        for(UINT face=0;face<faces;++face)for(UINT mip=0;mip<3;++mip) {
                            const UINT width=32>>mip;std::vector<BYTE> actual(width*width*element);
                            const bool targeted=mip==1 && face>=first && face<first+count;
                            check(pixels.copy(pixels.context.get(),false,actual.data(),canonical,face*3+mip,width*element,width*width*element,nullptr)==S_OK &&
                                (targeted ? actual==expected:std::all_of(actual.begin(),actual.end(),[](BYTE value){return value==0x5a;})),
                                "Uint/Float UAV clear preserves selected mip pixels and all untouched mips and slices");
                        }
                    };
                    std::memset(canonical,0x5a,pixels.bytes);
                    reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*,const UINT*)>(vt(context)[operation])(context,view,values.data());verify();
                    std::memset(canonical,0x5a,pixels.bytes);void* producer=nullptr;void* list=nullptr;
                    if(FAILED(reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*,UINT,void**)>(vt(device)[27])(device,0,&producer)) || !producer)return 1;
                    reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*,const UINT*)>(vt(producer)[operation])(producer,view,values.data());values.fill(0);
                    check(reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*,BOOL,void**)>(vt(producer)[114])(producer,FALSE,&list)==S_OK && list &&
                        std::all_of(canonical,canonical+pixels.bytes,[](BYTE value){return value==0x5a;}),"mipped array clear records without executing");if(!list)return 1;
                    reinterpret_cast<Count>(vt(producer)[2])(producer);reinterpret_cast<Count>(vt(view)[2])(view);reinterpret_cast<Count>(vt(resource)[2])(resource);
                    for(UINT pass=0;pass<2;++pass){std::memset(canonical,0x5a,pixels.bytes);reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*,BOOL)>(vt(context)[58])(context,list,FALSE);verify();}
                    reinterpret_cast<Count>(vt(list)[2])(list);
                }
            }
            for (const UINT faces:{1u,6u}) {
                Desc initialized{};
                initialized.standard={8,8,3,faces,DXGI_FORMAT_R8G8B8A8_UNORM,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_SHADER_RESOURCE,0,faces==6 ? UINT(D3D11_RESOURCE_MISC_TEXTURECUBE) : 0};
                std::vector<std::vector<unsigned char>> sources(faces*3);
                std::vector<D3D11_SUBRESOURCE_DATA> initial(faces*3);
                for (UINT mip=0;mip<faces*3;++mip) {
                    const UINT width=8>>(mip%3),pitch=width*4+12;
                    sources[mip].assign(pitch*width+16,0x5a);
                    for (UINT y=0;y<width;++y) for (UINT x=0;x<width*4;++x) sources[mip][y*pitch+x]=static_cast<unsigned char>(mip*53+y*7+x);
                    initial[mip]={sources[mip].data(),pitch,0};
                }
                const auto original_sources=sources;
                using CreateInitialized=HRESULT(STDMETHODCALLTYPE*)(void*,const Desc*,const D3D11_SUBRESOURCE_DATA*,void**);
                const auto create_initialized=reinterpret_cast<CreateInitialized>(vt(device)[5]);
                texture=nullptr;
                check(create_initialized(device,&initialized,initial.data(),&texture)==S_OK && texture,
                      "initial texture upload accepts padded source rows and every mip");
                if (texture) {
                    void* view=nullptr;
                    using MakeSrv=HRESULT(STDMETHODCALLTYPE*)(void*,void*,const D3D11_SHADER_RESOURCE_VIEW_DESC*,void**);
                    check(reinterpret_cast<MakeSrv>(vt(device)[7])(device,texture,nullptr,&view)==S_OK && view,
                          "initialized texture retains a native view of all uploaded mips");
                    if (view) {
                        if (faces==6) {
                            struct { D3D11_SHADER_RESOURCE_VIEW_DESC desc{}; UINT guard=0xaabbccdd; } queried;
                            reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,D3D11_SHADER_RESOURCE_VIEW_DESC*)>(vt(view)[9])(view,&queried.desc);
                            check(queried.desc.Format==DXGI_FORMAT_R8G8B8A8_UNORM && queried.desc.ViewDimension==D3D11_SRV_DIMENSION_TEXTURECUBE &&
                                  queried.desc.TextureCube.MostDetailedMip==0 && queried.desc.TextureCube.MipLevels==3 && queried.guard==0xaabbccdd,
                                  "default cube SRV preserves its native dimension, mip range and ABI bounds");
                            std::array<UINT32,8> srd{};std::memcpy(srd.data(),static_cast<char*>(view)+48,sizeof srd);
                            check((srd[3]>>28)==11 && (srd[4]&0x1fff)==0 && (srd[5]&0x3ffffff)==(5u<<13),
                                  "cube SRD retains faces zero through five and encodes the last cube like original UMD");
                        }
                        void* canonical=nullptr;std::memcpy(&canonical,static_cast<char*>(view)+80,sizeof canonical);
                        TitleTextureLayout pixels;
                        hr=create_xg_texture_layout(8,8,3,faces,DXGI_FORMAT_R8G8B8A8_UNORM,UINT_MAX,pixels,D3D11_BIND_SHADER_RESOURCE);
                        check(SUCCEEDED(hr),"initialized texture keeps the original XG mip layout");
                        for (UINT mip=0;SUCCEEDED(hr) && mip<faces*3;++mip) {
                            const UINT width=8>>(mip%3),pitch=width*4;
                            std::vector<unsigned char> linear(pitch*width);
                            check(pixels.copy(pixels.context.get(),false,linear.data(),canonical,mip,pitch,pitch*width,nullptr)==S_OK,
                                  "initialized texture exposes tiled mip pixels through original XG");
                            bool exact=true;
                            for (UINT y=0;y<width;++y) exact=exact && !std::memcmp(linear.data()+y*pitch,sources[mip].data()+y*initial[mip].SysMemPitch,pitch);
                            check(exact,"upload preserves every source pixel without copying row padding");
                        }
                        void* destination=nullptr;void* destination_view=nullptr;
                        auto copy_desc=initialized;copy_desc.standard.Format=DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
                        check(create_initialized(device,&copy_desc,nullptr,&destination)==S_OK && destination,"CopyResource creates a matching sRGB sibling texture with every face and mip");
                        if(destination) {
                            check(reinterpret_cast<MakeSrv>(vt(device)[7])(device,destination,nullptr,&destination_view)==S_OK && destination_view,"copied texture exposes the original XG layout");
                            if(destination_view) {
                                void* copied=nullptr;std::memcpy(&copied,static_cast<BYTE*>(destination_view)+80,sizeof copied);
                                using Copy=void(STDMETHODCALLTYPE*)(void*,void*,void*);
                                for(UINT pass=0;pass<2;++pass) {
                                    std::memset(copied,0x9d,pixels.bytes);
                                    if(!pass)for(UINT invalid=0;invalid<2;++invalid) {
                                        auto bad=initialized;if(invalid)bad.standard.Format=DXGI_FORMAT_R32_FLOAT;else {bad.standard.Width=4;bad.standard.Height=4;}
                                        void* wrong=nullptr;check(create_initialized(device,&bad,nullptr,&wrong)==S_OK && wrong,"texture copy negative control creates a valid incompatible source");
                                        if(wrong) {
                                            static UINT refused=0;refused=0;
                                            set_d3d11x_trap_handler([](const char*,UINT slot)->std::uintptr_t{refused=slot;return 0;});
                                            reinterpret_cast<Copy>(vt(context)[47])(context,destination,wrong);set_d3d11x_trap_handler(nullptr);
                                            check(refused==47 && std::all_of(static_cast<BYTE*>(copied),static_cast<BYTE*>(copied)+pixels.bytes,[](BYTE value){return value==0x9d;}),
                                                "CopyResource rejects mismatched extents and unrelated format families before touching destination storage");
                                            reinterpret_cast<Count>(vt(wrong)[2])(wrong);
                                        }
                                    }
                                    reinterpret_cast<Copy>(vt(context)[47])(context,destination,texture);
                                    reinterpret_cast<Copy>(vt(context)[47])(context,destination,destination);
                                    for(UINT mip=0;mip<faces*3;++mip) {
                                        const UINT width=8>>(mip%3),pitch=width*4;std::vector<BYTE> linear(pitch*width);
                                        bool exact=pixels.copy(pixels.context.get(),false,linear.data(),copied,mip,pitch,pitch*width,nullptr)==S_OK;
                                        for(UINT y=0;y<width;++y)exact=exact && !std::memcmp(linear.data()+y*pitch,sources[mip].data()+y*initial[mip].SysMemPitch,pitch);
                                        check(exact,"native CopyResource preserves every face/mip and replaces CPU-invalidated destination data on reuse");
                                    }
                                }
                                std::vector<std::vector<BYTE>> expected(faces*3);
                                for(UINT mip=0;mip<faces*3;++mip) {
                                    const UINT width=8>>(mip%3);expected[mip].resize(width*width*4);
                                    for(UINT y=0;y<width;++y)std::memcpy(expected[mip].data()+y*width*4,sources[mip].data()+y*initial[mip].SysMemPitch,width*4);
                                }
                                const auto verify_regions=[&]() {
                                    for(UINT mip=0;mip<faces*3;++mip) {
                                        const UINT width=8>>(mip%3);std::vector<BYTE> actual(width*width*4);
                                        check(pixels.copy(pixels.context.get(),false,actual.data(),copied,mip,width*4,UINT(actual.size()),nullptr)==S_OK && actual==expected[mip],
                                            "texture region copy preserves exact pixels in selected rectangles and every untouched mip/face");
                                    }
                                };
                                using Region=void(STDMETHODCALLTYPE*)(void*,void*,UINT,UINT,UINT,UINT,void*,UINT,const D3D11_BOX*);
                                const auto copy_region=reinterpret_cast<Region>(vt(context)[46]);const UINT selected=(faces-1)*3+1;
                                D3D11_BOX box{1,2,0,3,4,1};
                                copy_region(context,destination,selected,1,1,0,texture,0,&box);
                                for(UINT y=0;y<2;++y)std::memcpy(expected[selected].data()+((y+1)*4+1)*4,sources[0].data()+(y+2)*initial[0].SysMemPitch+4,8);
                                verify_regions();
                                box={1,1,0,3,3,1};
                                copy_region(context,destination,2,0,0,0,destination,selected,&box);
                                for(UINT y=0;y<2;++y)std::memcpy(expected[2].data()+y*8,expected[selected].data()+((y+1)*4+1)*4,8);
                                verify_regions();
                                const D3D11_BOX empty{1,1,0,1,2,1},outside{0,0,0,9,1,1};
                                copy_region(context,destination,selected,0,0,0,texture,0,&empty);
                                copy_region(context,destination,selected,0,0,0,texture,0,&outside);
                                copy_region(context,destination,UINT_MAX,0,0,0,texture,0,nullptr);
                                verify_regions();
                                using Defer=HRESULT(STDMETHODCALLTYPE*)(void*,UINT,void**);using Finish=HRESULT(STDMETHODCALLTYPE*)(void*,BOOL,void**);
                                void* producer=nullptr;void* list=nullptr;
                                if(reinterpret_cast<Defer>(vt(device)[27])(device,0,&producer)==S_OK && producer) {
                                    box={1,2,0,3,4,1};reinterpret_cast<Region>(vt(producer)[46])(producer,destination,selected,1,1,0,texture,0,&box);box={};
                                    check(reinterpret_cast<Finish>(vt(producer)[114])(producer,FALSE,&list)==S_OK && list,"deferred texture copy snapshots the box without copying source pixel data");
                                    verify_regions();reinterpret_cast<Count>(vt(producer)[2])(producer);
                                }else check(false,"texture region copy creates a deferred producer");
                                if(list) {
                                    reinterpret_cast<Count>(vt(destination_view)[2])(destination_view);destination_view=nullptr;
                                    reinterpret_cast<Count>(vt(destination)[2])(destination);destination=nullptr;
                                    for(UINT pass=0;pass<2;++pass) {
                                        std::array<BYTE,256> changed;changed.fill(BYTE(0x6b+pass));
                                        reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*,UINT,const D3D11_BOX*,const void*,UINT,UINT)>(vt(context)[48])(context,texture,0,nullptr,changed.data(),32,256);
                                        reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*,BOOL)>(vt(context)[58])(context,list,TRUE);
                                        for(UINT y=0;y<2;++y)std::fill_n(expected[selected].data()+((y+1)*4+1)*4,8,BYTE(0x6b+pass));
                                        verify_regions();
                                    }
                                    reinterpret_cast<Count>(vt(list)[2])(list);
                                }
                                if(destination_view)reinterpret_cast<Count>(vt(destination_view)[2])(destination_view);
                            }
                            if(destination)reinterpret_cast<Count>(vt(destination)[2])(destination);
                        }
                        reinterpret_cast<Count>(vt(view)[2])(view);
                    }
                    reinterpret_cast<Count>(vt(texture)[2])(texture);
                }
                check(sources==original_sources,"texture initialization leaves source pixels, padding and guards intact");
                initial[2].SysMemPitch=1;
                check(create_initialized(device,&initialized,initial.data(),&texture)==E_INVALIDARG && !texture,
                      "short initial mip pitch fails without publishing a partial texture");
                initial[2].SysMemPitch=20; initial[2].pSysMem=reinterpret_cast<void*>(1);
                check(create_initialized(device,&initialized,initial.data(),&texture)==E_INVALIDARG && !texture,
                      "unreadable initial pixels fail without dereferencing untrusted memory");
                if (faces==6) {
                    initialized.standard.Height=4;
                    check(create_initialized(device,&initialized,nullptr,&texture)==E_INVALIDARG && !texture,
                          "cube texture rejects non-square faces");
                    initialized.standard.Height=8; initialized.standard.ArraySize=5;
                    check(create_initialized(device,&initialized,nullptr,&texture)==E_INVALIDARG && !texture,
                          "cube texture rejects an incomplete face set");
                }
            }
            // Reference 1401ca160 / 1405869a0: full-chain creation, per-face mip filtering and retained deferred work.
            for(const UINT faces:{1u,6u})for(const bool restricted:{false,true}) {
                Desc generated{};generated.standard={8,8,0,faces,DXGI_FORMAT_R8G8B8A8_UNORM,{1,0},D3D11_USAGE_DEFAULT,
                    D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET,0,UINT(D3D11_RESOURCE_MISC_GENERATE_MIPS)|(faces==6 ? UINT(D3D11_RESOURCE_MISC_TEXTURECUBE):0)};
                std::vector<std::vector<BYTE>> sources(faces*4),expected(faces*4);
                std::vector<D3D11_SUBRESOURCE_DATA> initial(faces*4);
                for(UINT face=0;face<faces;++face)for(UINT mip=0;mip<4;++mip) {
                    const UINT width=8>>mip,pitch=width*4+12,index=face*4+mip;
                    sources[index].assign(pitch*width+16,0x5a);expected[index].resize(width*width*4);
                    for(UINT y=0;y<width;++y)for(UINT x=0;x<width;++x)for(UINT c=0;c<4;++c) {
                        const BYTE value=BYTE(x*(17+c*9)+y*(7+c*13)+face*29+mip*41+c*19);
                        sources[index][y*pitch+x*4+c]=value;expected[index][(y*width+x)*4+c]=value;
                    }
                    initial[index]={sources[index].data(),pitch,0};
                }
                const auto originals=sources;
                using CreateInitialized=HRESULT(STDMETHODCALLTYPE*)(void*,const Desc*,const D3D11_SUBRESOURCE_DATA*,void**);
                void* object=nullptr;hr=reinterpret_cast<CreateInitialized>(vt(device)[5])(device,&generated,initial.data(),&object);
                check(hr==S_OK && object,"reference autogen creation accepts a full mip chain for 2D and cube resources");if(!object)return 1;
                Desc queried{};reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,Desc*)>(vt(object)[12])(object,&queried);
                check(queried.standard.MipLevels==4 && queried.standard.ArraySize==faces && queried.standard.MiscFlags==generated.standard.MiscFlags,
                    "MipLevels zero expands the complete chain without losing cube/autogen metadata");
                D3D11_SHADER_RESOURCE_VIEW_DESC desc{};desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
                desc.ViewDimension=faces==6 ? D3D11_SRV_DIMENSION_TEXTURECUBE:D3D11_SRV_DIMENSION_TEXTURE2D;
                const UINT base=restricted ? 1:0,levels=restricted ? 2:4;
                desc.Texture2D.MostDetailedMip=base;desc.Texture2D.MipLevels=levels;
                using MakeSrv=HRESULT(STDMETHODCALLTYPE*)(void*,void*,const D3D11_SHADER_RESOURCE_VIEW_DESC*,void**);
                void* view=nullptr;hr=reinterpret_cast<MakeSrv>(vt(device)[7])(device,object,&desc,&view);
                check(hr==S_OK && view,"mip generation accepts the selected original view range");if(!view)return 1;
                std::array<UINT32,8> srd{};std::memcpy(srd.data(),static_cast<BYTE*>(view)+48,sizeof srd);
                check(((srd[3]>>12)&15)==base && ((srd[3]>>16)&15)==base+levels-1,"console SRD retains base/last mip of the selected view");
                void* canonical=nullptr;std::memcpy(&canonical,static_cast<BYTE*>(view)+80,sizeof canonical);
                TitleTextureLayout layout;hr=create_xg_texture_layout(8,8,4,faces,DXGI_FORMAT_R8G8B8A8_UNORM,UINT_MAX,layout,generated.standard.BindFlags);
                if(FAILED(hr))return 1;
                if(faces==6) {
                    D3D11_RENDER_TARGET_VIEW_DESC rtv{};rtv.Format=desc.Format;rtv.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
                    rtv.Texture2DArray={99,99,UINT_MAX};void* target=nullptr;
                    using MakeRtv=HRESULT(STDMETHODCALLTYPE*)(void*,void*,const D3D11_RENDER_TARGET_VIEW_DESC*,void**);
                    check(reinterpret_cast<MakeRtv>(vt(device)[9])(device,object,&rtv,&target)==S_OK && target,"cube RTV accepts source-compatible clamped mip and face ranges");
                    if(!target)return 1;
                    const FLOAT green[]{0,1,0,1};reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*,const FLOAT*)>(vt(context)[50])(context,target,green);
                    reinterpret_cast<Count>(vt(target)[2])(target);expected[faces*4-1]={0,255,0,255};
                    for(UINT index=0;index<faces*4;++index) {
                        const UINT width=8>>(index%4);std::vector<BYTE> actual(width*width*4);
                        check(layout.copy(layout.context.get(),false,actual.data(),canonical,index,width*4,UINT(actual.size()),nullptr)==S_OK && actual==expected[index],
                            "clamped array RTV clears only the last face/mip and preserves every other subresource");
                    }
                }
                const std::vector<BYTE> before(static_cast<BYTE*>(canonical),static_cast<BYTE*>(canonical)+layout.bytes);
                using Generate=void(STDMETHODCALLTYPE*)(void*,void*);
                using Finish=HRESULT(STDMETHODCALLTYPE*)(void*,BOOL,void**);
                void* commands=nullptr;
                if(faces==6) {
                    using Defer=HRESULT(STDMETHODCALLTYPE*)(void*,UINT,void**);void* producer=nullptr;
                    if(reinterpret_cast<Defer>(vt(device)[27])(device,0,&producer)!=S_OK || !producer)return 1;
                    reinterpret_cast<Generate>(vt(producer)[54])(producer,view);
                    check(!std::memcmp(before.data(),canonical,before.size()),"deferred GenerateMips does not alter canonical pixels while recording");
                    check(reinterpret_cast<Finish>(vt(producer)[114])(producer,FALSE,&commands)==S_OK && commands,"deferred mip generation finishes in the existing command list");
                    reinterpret_cast<Count>(vt(producer)[2])(producer);
                    reinterpret_cast<Count>(vt(view)[2])(view);view=nullptr;reinterpret_cast<Count>(vt(object)[2])(object);object=nullptr;
                    if(!commands)return 1;
                    reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*,BOOL)>(vt(context)[58])(context,commands,FALSE);
                } else reinterpret_cast<Generate>(vt(context)[54])(context,view);
                for(UINT face=0;face<faces;++face)for(UINT mip=base+1;mip<base+levels;++mip) {
                    const UINT width=8>>mip,source_width=width*2;
                    for(UINT y=0;y<width;++y)for(UINT x=0;x<width;++x)for(UINT c=0;c<4;++c) {
                        UINT sum=0;for(UINT dy=0;dy<2;++dy)for(UINT dx=0;dx<2;++dx)sum+=expected[face*4+mip-1][((2*y+dy)*source_width+2*x+dx)*4+c];
                        expected[face*4+mip][(y*width+x)*4+c]=BYTE((sum+2)/4);
                    }
                }
                for(UINT index=0;index<faces*4;++index) {
                    const UINT mip=index%4,width=8>>mip;std::vector<BYTE> actual(width*width*4);
                    hr=layout.copy(layout.context.get(),false,actual.data(),canonical,index,width*4,UINT(actual.size()),nullptr);
                    const int tolerance=mip>base && mip<base+levels ? 2:0;bool exact=SUCCEEDED(hr);
                    for(size_t i=0;i<actual.size();++i)exact=exact && std::abs(int(actual[i])-int(expected[index][i]))<=tolerance;
                    check(exact,"GenerateMips linearly filters each selected face/mip, including alpha, and preserves the source and unselected levels");
                }
                check(sources==originals,"mip generation preserves producer pixels, row padding and guards");
                if(commands)reinterpret_cast<Count>(vt(commands)[2])(commands);
                if(view)reinterpret_cast<Count>(vt(view)[2])(view);if(object)reinterpret_cast<Count>(vt(object)[2])(object);
            }
            {
                Desc cube{};cube.standard={32,32,0,6,DXGI_FORMAT_R11G11B10_FLOAT,{1,0},D3D11_USAGE_DEFAULT,0xA8,0,D3D11_RESOURCE_MISC_TEXTURECUBE|D3D11_RESOURCE_MISC_GENERATE_MIPS};
                using CreateOwned=HRESULT(STDMETHODCALLTYPE*)(void*,const Desc*,const D3D11_SUBRESOURCE_DATA*,void**);void* object=nullptr;
                check(reinterpret_cast<CreateOwned>(vt(device)[5])(device,&cube,nullptr,&object)==S_OK && object,"FH2-demonstrated generic 32x32 float cube creation expands its full mip chain");
                if(object){Desc actual{};reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,Desc*)>(vt(object)[12])(object,&actual);check(actual.standard.MipLevels==6,"32x32 cube exposes six original mip levels");reinterpret_cast<Count>(vt(object)[2])(object);}
            }

            TitleTextureLayout placed_color;
            const UINT color_binds=D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_RENDER_TARGET|D3D11_BIND_UNORDERED_ACCESS;
            hr=create_xg_texture_layout(32,16,1,1,DXGI_FORMAT_R16G16_FLOAT,14,placed_color,color_binds);
            check(SUCCEEDED(hr),"placed half-float color keeps the original tile mode");
            if (SUCCEEDED(hr)) {
                void* storage=allocate(placed_color.bytes+placed_color.alignment+32,attributes);
                check(storage!=nullptr,"placed color fixture owns its canonical allocation");
                if (storage) {
                    const SIZE_T total=placed_color.bytes+placed_color.alignment+32;
                    std::memset(storage,0x5a,total);
                    const auto address=(reinterpret_cast<std::uintptr_t>(storage)+16+placed_color.alignment-1)&~(placed_color.alignment-1);
                    Desc color{}; color.standard={32,16,1,1,DXGI_FORMAT_R16G16_FLOAT,{1,0},D3D11_USAGE_DEFAULT,color_binds,0,0};
                    texture=nullptr;
                    check(reinterpret_cast<CreateTexture>(vt(device)[64])(device,&color,14,0,reinterpret_cast<void*>(address),&texture)==S_OK && texture,
                          "placed color binds native render and unordered access to existing storage");
                    if (texture) reinterpret_cast<Count>(vt(texture)[2])(texture);
                    check(std::all_of(static_cast<unsigned char*>(storage),static_cast<unsigned char*>(storage)+total,[](auto x){return x==0x5a;}),
                          "placed color creation and retirement preserve borrowed pixels and guards");
                    free_memory(storage,attributes);
                }
            }
            struct VolumeDesc { D3D11_TEXTURE3D_DESC standard; std::array<UINT, 2> extension{}; } volume_desc{};
            static_assert(sizeof(VolumeDesc) == 44);
            volume_desc.standard = {16, 16, 16, 1, DXGI_FORMAT_R10G10B10A2_UNORM, D3D11_USAGE_DEFAULT,
                D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS, 0, 0};
            using CreateVolume = HRESULT (STDMETHODCALLTYPE*)(void*, const VolumeDesc*, const D3D11_SUBRESOURCE_DATA*, void**);
            const auto create_volume = reinterpret_cast<CreateVolume>(vt(device)[6]);
            constexpr UINT volume_pitch=16*4+8,volume_slice=volume_pitch*16+16;
            std::vector<unsigned char> volume_source(volume_slice*16+16,0x5a);
            for (UINT z=0;z<16;++z) for (UINT y=0;y<16;++y) for (UINT x=0;x<64;++x)
                volume_source[z*volume_slice+y*volume_pitch+x]=static_cast<unsigned char>(z*13+y*7+x);
            const auto original_volume_source=volume_source;
            D3D11_SUBRESOURCE_DATA volume_initial{volume_source.data(),volume_pitch,volume_slice};
            texture = nullptr;
            check(create_volume(device, &volume_desc, nullptr, nullptr) == S_FALSE,
                  "volume descriptor validation does not acquire ownership");
            check(create_volume(device, &volume_desc, &volume_initial, &texture) == S_OK && texture,
                  "observed volume factory owns original XG storage and a native UAV-capable texture");
            if (texture) {
                using GetVolumeDesc = void (STDMETHODCALLTYPE*)(void*, VolumeDesc*);
                struct { VolumeDesc desc{}; UINT guard = 0xaabbccdd; } result;
                reinterpret_cast<GetVolumeDesc>(vt(texture)[12])(texture, &result.desc);
                check(!std::memcmp(&result.desc, &volume_desc, sizeof volume_desc) && result.guard == 0xaabbccdd,
                      "volume GetDesc preserves the native forty-four-byte ABI");
                using GetType = void (STDMETHODCALLTYPE*)(void*, D3D11_RESOURCE_DIMENSION*);
                D3D11_RESOURCE_DIMENSION type{};
                reinterpret_cast<GetType>(vt(texture)[8])(texture, &type);
                check(type == D3D11_RESOURCE_DIMENSION_TEXTURE3D, "volume retains its real resource dimension");
                using CreateUav = HRESULT (STDMETHODCALLTYPE*)(void*,void*,const D3D11_UNORDERED_ACCESS_VIEW_DESC*,void**);
                const auto create_uav=reinterpret_cast<CreateUav>(vt(device)[8]);
                void* uav=nullptr;
                check(create_uav(device,texture,nullptr,nullptr)==S_FALSE,
                      "default volume UAV validation does not acquire ownership");
                check(create_uav(device,texture,nullptr,&uav)==S_OK && uav,
                      "default volume UAV owns a native texture3D descriptor");
                if (uav) {
                    struct { D3D11_UNORDERED_ACCESS_VIEW_DESC desc{}; UINT guard=0xaabbccdd; } output;
                    using GetUavDesc=void(STDMETHODCALLTYPE*)(void*,D3D11_UNORDERED_ACCESS_VIEW_DESC*);
                    reinterpret_cast<GetUavDesc>(vt(uav)[9])(uav,&output.desc);
                    check(output.desc.Format==volume_desc.standard.Format && output.desc.ViewDimension==D3D11_UAV_DIMENSION_TEXTURE3D &&
                          output.desc.Texture3D.MipSlice==0 && output.desc.Texture3D.FirstWSlice==0 &&
                          output.desc.Texture3D.WSize==UINT_MAX && output.guard==0xaabbccdd,
                          "volume UAV preserves its complete native view descriptor and output bounds");
                    void* parent=nullptr;
                    using GetResource=void(STDMETHODCALLTYPE*)(void*,void**);
                    reinterpret_cast<GetResource>(vt(uav)[8])(uav,&parent);
                    check(parent==texture,"volume UAV retains the original resource");
                    if (parent) reinterpret_cast<Count>(vt(parent)[2])(parent);
                    const auto* view_words=reinterpret_cast<const UINT32*>(static_cast<const unsigned char*>(uav)+48);
                    void* canonical=nullptr;
                    std::memcpy(&canonical,static_cast<const unsigned char*>(uav)+80,sizeof canonical);
                    check(view_words[0]==(reinterpret_cast<std::uintptr_t>(canonical)>>8) && (view_words[3]>>28)==13 && (view_words[5]&0x3ffffffu)==(15u<<13),
                          "volume UAV preserves the physical XG base and native 3D resource type");
                    reinterpret_cast<Count>(vt(uav)[2])(uav);
                }
                D3D11_UNORDERED_ACCESS_VIEW_DESC invalid{};
                invalid.Format=volume_desc.standard.Format; invalid.ViewDimension=D3D11_UAV_DIMENSION_TEXTURE3D;
                invalid.Texture3D.WSize=17;
                check(create_uav(device,texture,&invalid,&uav)==E_INVALIDARG && !uav,
                      "volume UAV rejects a slice range outside the resource");
                invalid.Texture3D.WSize=8;
                check(create_uav(device,texture,&invalid,&uav)==S_OK && uav,
                      "partial volume UAV accepts the reference depth range");
                if(uav)reinterpret_cast<Count>(vt(uav)[2])(uav);
                using CreateSrv = HRESULT (STDMETHODCALLTYPE*)(void*, void*, const D3D11_SHADER_RESOURCE_VIEW_DESC*, void**);
                void* view = nullptr;
                check(reinterpret_cast<CreateSrv>(vt(device)[7])(device, texture, nullptr, &view) == S_OK && view,
                      "default volume SRV owns a native texture3D descriptor");
                if (view) {
                    D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
                    using GetSrvDesc = void (STDMETHODCALLTYPE*)(void*, D3D11_SHADER_RESOURCE_VIEW_DESC*);
                    reinterpret_cast<GetSrvDesc>(vt(view)[9])(view, &desc);
                    check(desc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE3D && desc.Texture3D.MipLevels == 1 &&
                        desc.Format == DXGI_FORMAT_R10G10B10A2_UNORM, "volume SRV preserves dimension, format and mip range");
                    void* canonical=nullptr;std::memcpy(&canonical,static_cast<char*>(view)+80,sizeof canonical);
                    TitleTextureLayout pixels;
                    hr=create_xg_texture3d_layout(16,16,16,1,volume_desc.standard.Format,volume_desc.standard.BindFlags,pixels);
                    std::vector<unsigned char> linear(16*16*16*4);
                    check(SUCCEEDED(hr) && pixels.copy(pixels.context.get(),false,linear.data(),canonical,0,64,16*64,nullptr)==S_OK,
                          "volume initial data is stored in the original XG physical layout");
                    bool exact=true;
                    for (UINT z=0;z<16;++z) for (UINT y=0;y<16;++y)
                        exact=exact && !std::memcmp(linear.data()+(z*16+y)*64,volume_source.data()+z*volume_slice+y*volume_pitch,64);
                    check(exact && volume_source==original_volume_source,"volume initialization preserves every depth slice and source padding");
                    void* destination=nullptr;void* copied_view=nullptr;
                    check(create_volume(device,&volume_desc,nullptr,&destination)==S_OK && destination,"CopyResource creates a matching volume texture");
                    if(destination) {
                        check(reinterpret_cast<CreateSrv>(vt(device)[7])(device,destination,nullptr,&copied_view)==S_OK && copied_view,"volume copy retains its native texture3D view");
                        if(copied_view) {
                            void* copied=nullptr;std::memcpy(&copied,static_cast<BYTE*>(copied_view)+80,sizeof copied);
                            reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*,void*)>(vt(context)[47])(context,destination,texture);
                            bool matches=pixels.copy(pixels.context.get(),false,linear.data(),copied,0,64,16*64,nullptr)==S_OK;
                            for(UINT z=0;z<16;++z)for(UINT y=0;y<16;++y)matches=matches && !std::memcmp(linear.data()+(z*16+y)*64,volume_source.data()+z*volume_slice+y*volume_pitch,64);
                            check(matches,"native CopyResource preserves all texels and depth slices of a packed volume");
                            const D3D11_BOX box{2,3,4,5,6,7};auto expected=linear;
                            for(UINT z=0;z<3;++z)for(UINT y=0;y<3;++y)std::memcpy(expected.data()+((z+9)*16+y+8)*64+7*4,volume_source.data()+(z+4)*volume_slice+(y+3)*volume_pitch+2*4,12);
                            reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*,UINT,UINT,UINT,UINT,void*,UINT,const D3D11_BOX*)>(vt(context)[46])(context,destination,0,7,8,9,texture,0,&box);
                            check(pixels.copy(pixels.context.get(),false,linear.data(),copied,0,64,16*64,nullptr)==S_OK && linear==expected,
                                "3D region copy preserves the selected volume box, nonzero destination XYZ and all surrounding texels");
                            reinterpret_cast<Count>(vt(copied_view)[2])(copied_view);
                        }
                        reinterpret_cast<Count>(vt(destination)[2])(destination);
                    }
                    check(reinterpret_cast<Count>(vt(texture)[2])(texture) == 1, "volume view retains its storage after caller release");
                    texture = nullptr;
                    reinterpret_cast<Count>(vt(view)[2])(view);
                }
                if (texture) reinterpret_cast<Count>(vt(texture)[2])(texture);
            }
            if(graphics) {
                NativeUavClear native;
                if(FAILED(native.initialize(argc==5)))return 1;
                struct ClearFormat {DXGI_FORMAT format;UINT bytes;bool snorm=false;};
                const ClearFormat clear_formats[]{{DXGI_FORMAT_R8_UNORM,1},{DXGI_FORMAT_R8_UINT,1},{DXGI_FORMAT_R16_UNORM,2},
                    {DXGI_FORMAT_R8G8B8A8_UNORM,4},{DXGI_FORMAT_R11G11B10_FLOAT,4},{DXGI_FORMAT_R16G16B16A16_FLOAT,8},
                    {DXGI_FORMAT_R32_FLOAT,4},{DXGI_FORMAT_R32_UINT,4},{DXGI_FORMAT_R10G10B10A2_UNORM,4},
                    {DXGI_FORMAT_R8_SNORM,1,true},{DXGI_FORMAT_R8G8_SNORM,2,true},{DXGI_FORMAT_R8G8B8A8_SNORM,4,true},
                    {DXGI_FORMAT_R16_SNORM,2,true},{DXGI_FORMAT_R16G16_SNORM,4,true},{DXGI_FORMAT_R16G16B16A16_SNORM,8,true}};
                for(const auto& item:clear_formats)for(const bool alias:{false,true})for(const UINT operation:{51u,52u}) {
                    if(alias && item.format!=DXGI_FORMAT_R8_SNORM && item.format!=DXGI_FORMAT_R10G10B10A2_UNORM)continue;
                    Desc desc{};desc.standard={32,16,1,1,item.format,{1,0},D3D11_USAGE_DEFAULT,D3D11_BIND_SHADER_RESOURCE|D3D11_BIND_UNORDERED_ACCESS,0,0};
                    if(alias)desc.standard.Format=item.format==DXGI_FORMAT_R8_SNORM ? DXGI_FORMAT_R8_TYPELESS:DXGI_FORMAT_R10G10B10A2_TYPELESS;
                    if(item.snorm)desc.standard.BindFlags|=D3D11_BIND_RENDER_TARGET;
                    void* resource=nullptr;void* view=nullptr;
                    check(reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*,const Desc*,const D3D11_SUBRESOURCE_DATA*,void**)>(vt(device)[5])(device,&desc,nullptr,&resource)==S_OK && resource,"typed texture clear creates a UAV-capable original layout");if(!resource)return 1;
                    D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc{};uav_desc.Format=item.format;uav_desc.ViewDimension=D3D11_UAV_DIMENSION_TEXTURE2D;
                    check(reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*,void*,const D3D11_UNORDERED_ACCESS_VIEW_DESC*,void**)>(vt(device)[8])(device,resource,alias ? &uav_desc:nullptr,&view)==S_OK && view,"typed texture clear creates its matching native view");if(!view)return 1;
                    if(item.snorm) {
                        D3D11_SHADER_RESOURCE_VIEW_DESC read{};read.Format=item.format;read.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE2D;read.Texture2D.MipLevels=1;
                        void* srv=nullptr;
                        check(reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*,void*,const D3D11_SHADER_RESOURCE_VIEW_DESC*,void**)>(vt(device)[7])(device,resource,&read,&srv)==S_OK && srv,"SNORM texture owns a native signed-normalized SRV");if(!srv)return 1;
                        D3D11_SHADER_RESOURCE_VIEW_DESC queried{};reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,D3D11_SHADER_RESOURCE_VIEW_DESC*)>(vt(srv)[9])(srv,&queried);
                        check(queried.Format==item.format && queried.ViewDimension==read.ViewDimension,"SNORM SRV preserves the requested numeric format");
                        reinterpret_cast<Count>(vt(srv)[2])(srv);
                        if(alias) {
                            auto invalid=uav_desc;invalid.Format=DXGI_FORMAT_R16_SNORM;void* rejected=nullptr;
                            check(reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*,void*,const D3D11_UNORDERED_ACCESS_VIEW_DESC*,void**)>(vt(device)[8])(device,resource,&invalid,&rejected)==DXGI_ERROR_UNSUPPORTED && !rejected,"SNORM alias refuses a different storage family");
                        }
                    }
                    TitleTextureLayout pixels;
                    if(FAILED(create_xg_texture_layout(32,16,1,1,desc.standard.Format,UINT_MAX,pixels,desc.standard.BindFlags)))return 1;
                    BYTE* canonical=nullptr;std::memcpy(&canonical,static_cast<BYTE*>(view)+80,sizeof canonical);
                    std::array<UINT,4> values=operation==51 ? std::array<UINT,4>{0x61,0x12,0x34,0x56}:std::array<UINT,4>{0x40900061,0x3e800000,0x3f000000,0x3f800000};
                    std::vector<BYTE> expected,actual(32*16*item.bytes);
                    if(FAILED(native.texture(pixels.resource_desc,item.format,item.bytes,values,operation==52,expected)))return 1;
                    const auto verify=[&] {check(pixels.copy(pixels.context.get(),false,actual.data(),canonical,0,32*item.bytes,512*item.bytes,nullptr)==S_OK && actual==expected,"typed texture clear preserves every native reference pixel");};
                    reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*,const UINT*)>(vt(context)[operation])(context,view,values.data());verify();
                    if(item.snorm) {
                        const auto saved=expected;const FLOAT inf=std::numeric_limits<FLOAT>::infinity(),nan=std::numeric_limits<FLOAT>::quiet_NaN();
                        for(const FLOAT probe:{-inf,-2.f,-1.f,-.5f,-.25f,-0.f,0.f,.25f,.5f,1.f,2.f,inf,nan}) {
                            UINT bits=0;std::memcpy(&bits,&probe,4);const std::array<UINT,4> words{bits,bits,bits,bits};
                            if(FAILED(native.texture(pixels.resource_desc,item.format,item.bytes,words,operation==52,expected)))return 1;
                            reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*,const UINT*)>(vt(context)[operation])(context,view,words.data());verify();
                        }
                        expected=saved;
                    }
                    if(item.snorm && operation==52) {
                        const auto saved=expected;const std::array<UINT,4> words{0xbf000000,0xbe800000,0x3f000000,0x3f800000};
                        if(FAILED(native.texture(pixels.resource_desc,item.format,item.bytes,words,true,expected,true)))return 1;
                        D3D11_RENDER_TARGET_VIEW_DESC target{};target.Format=item.format;target.ViewDimension=D3D11_RTV_DIMENSION_TEXTURE2D;
                        void* rtv=nullptr;
                        check(reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*,void*,const D3D11_RENDER_TARGET_VIEW_DESC*,void**)>(vt(device)[9])(device,resource,&target,&rtv)==S_OK && rtv,"SNORM storage retains a native RTV alongside its UAV and SRV");if(!rtv)return 1;
                        FLOAT color[4];std::memcpy(color,words.data(),sizeof color);
                        reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*,const FLOAT*)>(vt(context)[50])(context,rtv,color);verify();
                        reinterpret_cast<Count>(vt(rtv)[2])(rtv);expected=saved;
                    }
                    std::memset(canonical,0x5a,pixels.bytes);
                    void* producer=nullptr;void* list=nullptr;
                    check(reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*,UINT,void**)>(vt(device)[27])(device,0,&producer)==S_OK && producer,"typed texture clear creates a deferred producer");if(!producer)return 1;
                    reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*,const UINT*)>(vt(producer)[operation])(producer,view,values.data());values.fill(0);
                    check(reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*,BOOL,void**)>(vt(producer)[114])(producer,FALSE,&list)==S_OK && list &&
                        std::all_of(canonical,canonical+pixels.bytes,[](BYTE value){return value==0x5a;}),"typed texture clear captures values and operation without execution");
                    reinterpret_cast<Count>(vt(producer)[2])(producer);reinterpret_cast<Count>(vt(view)[2])(view);reinterpret_cast<Count>(vt(resource)[2])(resource);if(!list)return 1;
                    for(UINT pass=0;pass<2;++pass){std::memset(canonical,0x5a,pixels.bytes);reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*,BOOL)>(vt(context)[58])(context,list,FALSE);verify();}
                    reinterpret_cast<Count>(vt(list)[2])(list);
                }
                using ClearVolume=void(STDMETHODCALLTYPE*)(void*,void*,const UINT*);
                for(const auto& item:clear_formats)for(const bool alias:{false,true}) {
                if((!item.snorm && item.format!=volume_desc.standard.Format) ||
                    (alias && item.format!=DXGI_FORMAT_R8_SNORM && item.format!=DXGI_FORMAT_R10G10B10A2_UNORM))continue;
                for(const UINT levels:{1u,3u})for(UINT selected_mip=0;selected_mip<levels;++selected_mip)for(const UINT operation:{51u,52u})for(UINT range=0;range<4;++range) {
                    auto desc=volume_desc;desc.standard.MipLevels=levels;
                    desc.standard.Format=alias ? (item.format==DXGI_FORMAT_R8_SNORM ? DXGI_FORMAT_R8_TYPELESS:DXGI_FORMAT_R10G10B10A2_TYPELESS):item.format;
                    const UINT size=16>>selected_mip;
                    TitleTextureLayout layout,control;
                    if(FAILED(create_xg_texture3d_layout(16,16,16,levels,desc.standard.Format,desc.standard.BindFlags,layout)) ||
                        FAILED(create_xg_texture3d_layout(size,size,size,1,desc.standard.Format,desc.standard.BindFlags,control)))return 1;
                    void* resource=nullptr;void* view=nullptr;
                    check(create_volume(device,&desc,nullptr,&resource)==S_OK && resource,"volume clear creates a fresh native 3D resource");if(!resource)return 1;
                    if(!alias) {
                        check(reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*,void*,const D3D11_UNORDERED_ACCESS_VIEW_DESC*,void**)>(vt(device)[8])(device,resource,nullptr,&view)==S_OK && view,"volume clear retains a full-depth UAV");if(!view)return 1;
                        reinterpret_cast<Count>(vt(view)[2])(view);view=nullptr;
                    }
                    const auto create=reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*,void*,const D3D11_UNORDERED_ACCESS_VIEW_DESC*,void**)>(vt(device)[8]);
                    D3D11_UNORDERED_ACCESS_VIEW_DESC selected{};selected.Format=item.format;selected.ViewDimension=D3D11_UAV_DIMENSION_TEXTURE3D;
                    const UINT first=range>=2 ? 1:0,count=range>=2 ? (range&1 ? size-first:size-2):size;
                    selected.Texture3D.MipSlice=selected_mip;selected.Texture3D.FirstWSlice=first;selected.Texture3D.WSize=range&1 ? UINT_MAX:count;
                    check(create(device,resource,&selected,&view)==S_OK && view,"3D UAV selects the requested mip and depth range");if(!view)return 1;
                    if(alias && range==0 && operation==51) {
                        D3D11_SHADER_RESOURCE_VIEW_DESC read{};read.Format=item.format;read.ViewDimension=D3D11_SRV_DIMENSION_TEXTURE3D;read.Texture3D.MostDetailedMip=selected_mip;read.Texture3D.MipLevels=1;
                        void* srv=nullptr;
                        check(reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*,void*,const D3D11_SHADER_RESOURCE_VIEW_DESC*,void**)>(vt(device)[7])(device,resource,&read,&srv)==S_OK && srv,"typeless volume retains its requested numeric SRV alias");if(!srv)return 1;
                        reinterpret_cast<Count>(vt(srv)[2])(srv);
                    }
                    D3D11_UNORDERED_ACCESS_VIEW_DESC queried{};
                    reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,D3D11_UNORDERED_ACCESS_VIEW_DESC*)>(vt(view)[9])(view,&queried);
                    check(!std::memcmp(&selected,&queried,sizeof selected),"3D UAV preserves the requested mip and depth sentinel in GetDesc");
                    auto invalid=selected;invalid.Texture3D.WSize=size+1;void* rejected=nullptr;
                    check(create(device,resource,&invalid,&rejected)==E_INVALIDARG && !rejected,"3D UAV validates depth against the selected mip");
                    invalid=selected;invalid.Texture3D.FirstWSlice=size;
                    check(create(device,resource,&invalid,&rejected)==E_INVALIDARG && !rejected,"3D UAV rejects a first slice outside the selected mip");
                    invalid=selected;invalid.Texture3D.MipSlice=levels;
                    check(create(device,resource,&invalid,&rejected)==E_INVALIDARG && !rejected,"3D UAV rejects a nonexistent mip");
                    BYTE* canonical=nullptr;std::memcpy(&canonical,static_cast<BYTE*>(view)+80,sizeof canonical);
                    std::array<UINT,4> values=operation==51 ? std::array<UINT,4>{0x61,0x12,0x34,0x56}:std::array<UINT,4>{0x40900061,0x3e800000,0x3f000000,0x3f800000};
                    if(item.snorm && operation==52)values={0xbf000000,0xbe800000,0x3f000000,0x3f800000};
                    std::vector<BYTE> expected;
                    if(FAILED(native.texture(control.resource_desc,item.format,item.bytes,values,operation==52,expected)))return 1;
                    const auto verify=[&] {
                        for(UINT mip=0;mip<levels;++mip) {
                            const UINT width=16>>mip;std::vector<BYTE> actual(width*width*width*item.bytes);
                            bool exact=layout.copy(layout.context.get(),false,actual.data(),canonical,mip,width*item.bytes,width*width*item.bytes,nullptr)==S_OK;
                            const SIZE_T slice=SIZE_T(width)*width*item.bytes;
                            for(UINT z=0;z<width;++z) {
                                const auto begin=actual.begin()+z*slice,end=begin+slice;
                                exact=exact && (mip==selected_mip && z>=first && z<first+count ?
                                    std::equal(begin,end,expected.begin()+z*slice):std::all_of(begin,end,[](BYTE value){return value==0x5a;}));
                            }
                            check(exact,"3D Uint/Float clear matches native pixels in selected slices and preserves every other mip and slice");
                        }
                    };
                    std::memset(canonical,0x5a,layout.bytes);
                    reinterpret_cast<ClearVolume>(vt(context)[operation])(context,view,values.data());verify();
                    std::memset(canonical,0x5a,layout.bytes);
                    void* producer=nullptr;void* list=nullptr;
                    check(reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*,UINT,void**)>(vt(device)[27])(device,0,&producer)==S_OK && producer,"volume clear creates a deferred producer");if(!producer)return 1;
                    reinterpret_cast<ClearVolume>(vt(producer)[operation])(producer,view,values.data());values.fill(0);
                    check(reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*,BOOL,void**)>(vt(producer)[114])(producer,FALSE,&list)==S_OK && list &&
                        std::all_of(canonical,canonical+layout.bytes,[](BYTE value){return value==0x5a;}),"3D clear recording captures values without executing");
                    reinterpret_cast<Count>(vt(producer)[2])(producer);reinterpret_cast<Count>(vt(view)[2])(view);reinterpret_cast<Count>(vt(resource)[2])(resource);if(!list)return 1;
                    for(UINT pass=0;pass<2;++pass) {
                        std::memset(canonical,0x5a,layout.bytes);reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*,BOOL)>(vt(context)[58])(context,list,FALSE);verify();
                    }
                    reinterpret_cast<Count>(vt(list)[2])(list);
                }
                }
            }
            volume_initial.SysMemSlicePitch=1;
            check(create_volume(device,&volume_desc,&volume_initial,&texture)==E_INVALIDARG && !texture,
                  "volume initialization rejects overlapping source slices before publishing storage");
            volume_desc.standard.MipLevels = 5; volume_desc.standard.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            volume_desc.standard.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            TitleTextureLayout volume_layout;
            hr = create_xg_texture3d_layout(16, 16, 16, 5, volume_desc.standard.Format,
                volume_desc.standard.BindFlags, volume_layout, 13);
            check(hr == S_OK, "placement volume uses its specified original tile mode");
            if (SUCCEEDED(hr)) {
                auto* storage = static_cast<unsigned char*>(allocate(volume_layout.bytes + volume_layout.alignment + 32, attributes));
                if (!storage) return 1;
                std::memset(storage, 0x5a, volume_layout.bytes + volume_layout.alignment + 32);
                const auto placed = (reinterpret_cast<std::uintptr_t>(storage) + 16 + volume_layout.alignment - 1) & ~(volume_layout.alignment - 1);
                using PlaceVolume = HRESULT (STDMETHODCALLTYPE*)(void*, const VolumeDesc*, UINT, UINT, void*, void**);
                const auto place_volume = reinterpret_cast<PlaceVolume>(vt(device)[65]);
                texture = nullptr;
                check(place_volume(device, &volume_desc, 13, 1, reinterpret_cast<void*>(placed), &texture) == DXGI_ERROR_UNSUPPORTED && !texture,
                      "unknown volume placement flags remain explicit");
                check(place_volume(device, &volume_desc, 13, 0, reinterpret_cast<void*>(placed), &texture) == S_OK && texture,
                      "native slot sixty-five pins borrowed tiled volume storage");
                if (texture) reinterpret_cast<Count>(vt(texture)[2])(texture);
                MEMORY_BASIC_INFORMATION retained{};
                VirtualQuery(storage, &retained, sizeof retained);
                check(retained.State == MEM_COMMIT &&
                    std::all_of(storage, storage + volume_layout.bytes + volume_layout.alignment + 32, [](auto x) { return x == 0x5a; }),
                    "placement volume retirement preserves all borrowed bytes and ownership");
                free_memory(storage, attributes);
            }
            for (bool multisample : {false, true}) {
                Desc target{};
                target.standard = {32, 16, 1, 1, DXGI_FORMAT_R10G10B10A2_UNORM, {1, 0}, D3D11_USAGE_DEFAULT,
                    D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0};
                if (multisample) {
                    target.standard.SampleDesc.Count = 4; target.standard.Format = DXGI_FORMAT_R11G11B10_FLOAT;
                    target.standard.MiscFlags = 0x40000;
                }
                using CreateTarget = HRESULT (STDMETHODCALLTYPE*)(void*, const Desc*, const D3D11_SUBRESOURCE_DATA*, void**);
                const auto create_target = reinterpret_cast<CreateTarget>(vt(device)[5]);
                texture = nullptr;
                check(create_target(device, &target, nullptr, &texture) == S_OK && texture,
                      "default render-target owns the approved expanded coherent image");
                if (texture) {
                    using GetTextureDesc = void (STDMETHODCALLTYPE*)(void*, Desc*);
                    Desc actual{};
                    reinterpret_cast<GetTextureDesc>(vt(texture)[12])(texture, &actual);
                    check(actual.standard.SampleDesc.Count == target.standard.SampleDesc.Count && !actual.standard.MiscFlags,
                          "GetDesc preserves four samples and consumes the native auxiliary creation hint");
                    if (multisample) {
                        using CreateSrv = HRESULT (STDMETHODCALLTYPE*)(void*, void*, const D3D11_SHADER_RESOURCE_VIEW_DESC*, void**);
                        void* srv = nullptr;
                        check(reinterpret_cast<CreateSrv>(vt(device)[7])(device, texture, nullptr, &srv) == S_OK && srv,
                              "MSAA SRV owns a real multisample native descriptor");
                        if (srv) {
                            D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
                            using GetSrvDesc = void (STDMETHODCALLTYPE*)(void*, D3D11_SHADER_RESOURCE_VIEW_DESC*);
                            reinterpret_cast<GetSrvDesc>(vt(srv)[9])(srv, &desc);
                            UINT32 word = 0; std::memcpy(&word, static_cast<char*>(srv) + 60, 4);
                            check(desc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2DMS && desc.Format == target.standard.Format &&
                                ((word >> 16) & 15) == 2, "MSAA SRD retains log2(samples) instead of treating it as the last mip");
                            reinterpret_cast<Count>(vt(srv)[2])(srv);
                        }
                        D3D11_SHADER_RESOURCE_VIEW_DESC mask_desc{};
                        mask_desc.Format = DXGI_FORMAT_R8_UINT; mask_desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; mask_desc.Texture2D.MipLevels = 1;
                        srv = nullptr;
                        check(reinterpret_cast<CreateSrv>(vt(device)[7])(device, texture, &mask_desc, &srv) == S_OK && srv,
                              "FMASK view derives real expanded fragment topology from the MSAA resource");
                        if (srv) {
                            using GetResource = void (STDMETHODCALLTYPE*)(void*, void**);
                            void* owner = nullptr;
                            reinterpret_cast<GetResource>(vt(srv)[8])(srv, &owner);
                            check(owner == texture, "FMASK GetResource retains the original multisample texture");
                            if (owner) reinterpret_cast<Count>(vt(owner)[2])(owner);
                            void* canonical = nullptr;
                            std::memcpy(&canonical, static_cast<char*>(srv) + 80, sizeof canonical);
                            TitleTextureLayout mask_layout;
                            HRESULT mask_hr = create_xg_texture_layout(32, 16, 1, 1, DXGI_FORMAT_R8_UINT, UINT_MAX, mask_layout, D3D11_BIND_SHADER_RESOURCE);
                            std::array<unsigned char, 32 * 16> values{};
                            if (SUCCEEDED(mask_hr)) mask_hr = mask_layout.copy(mask_layout.context.get(), false, values.data(), canonical, 0, 32, 512,nullptr);
                            UINT identity = 0;
                            for (UINT sample = 0; sample < actual.standard.SampleDesc.Count; ++sample) identity |= sample << (2 * sample);
                            check(SUCCEEDED(mask_hr) && std::all_of(values.begin(), values.end(), [identity](auto x) { return x == identity; }),
                                  "FMASK bytes map each of the four backed samples to its actual independent fragment");
                            const HMODULE compiler=LoadLibraryExW(L"d3dcompiler_47.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
                            const auto compile=compiler?reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(compiler,"D3DCompile")):nullptr;
                            ID3DBlob* code=nullptr;ID3DBlob* compiler_errors=nullptr;
                            constexpr char shader_source[]="Texture2D<uint> mask:register(t0);RWStructuredBuffer<uint> output:register(u0);"
                                "[numthreads(1,1,1)]void main(){output[0]=mask.Load(int3(0,0,0));}";
                            void* shader=nullptr;void* output=nullptr;void* uav=nullptr;
                            HRESULT sample_hr=compile?compile(shader_source,sizeof shader_source-1,nullptr,nullptr,nullptr,"main","cs_5_0",0,0,&code,&compiler_errors):E_NOINTERFACE;
                            if(SUCCEEDED(sample_hr))sample_hr=reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*,const void*,SIZE_T,void*,void**)>(vt(device)[18])(
                                device,code->GetBufferPointer(),code->GetBufferSize(),nullptr,&shader);
                            struct MaskBufferDesc {D3D11_BUFFER_DESC standard;UINT64 extension;} output_desc{};
                            output_desc.standard.ByteWidth=16;output_desc.standard.BindFlags=D3D11_BIND_UNORDERED_ACCESS;
                            output_desc.standard.MiscFlags=D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;output_desc.standard.StructureByteStride=4;
                            const UINT zeros[4]{};D3D11_SUBRESOURCE_DATA initial{zeros,0,0};
                            if(SUCCEEDED(sample_hr))sample_hr=reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*,const MaskBufferDesc*,const D3D11_SUBRESOURCE_DATA*,void**)>(vt(device)[3])(
                                device,&output_desc,&initial,&output);
                            D3D11_UNORDERED_ACCESS_VIEW_DESC out_view{};out_view.ViewDimension=D3D11_UAV_DIMENSION_BUFFER;out_view.Buffer.NumElements=4;
                            if(SUCCEEDED(sample_hr))sample_hr=reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*,void*,const D3D11_UNORDERED_ACCESS_VIEW_DESC*,void**)>(vt(device)[8])(
                                device,output,&out_view,&uav);
                            if(SUCCEEDED(sample_hr)) {
                                using Bind=void(STDMETHODCALLTYPE*)(void*,UINT,UINT,void* const*);
                                const auto set_shader=reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,void*)>(vt(context)[69]);
                                set_shader(context,shader);reinterpret_cast<Bind>(vt(context)[67])(context,0,1,&srv);reinterpret_cast<Bind>(vt(context)[68])(context,0,1,&uav);
                                reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,UINT,UINT,UINT)>(vt(context)[41])(context,1,1,1);
                                void* result=nullptr;std::memcpy(&result,static_cast<BYTE*>(output)+48,sizeof result);
                                UINT sampled=0;SIZE_T read=0;
                                check(result && ReadProcessMemory(GetCurrentProcess(),result,&sampled,sizeof sampled,&read) && read==sizeof sampled && sampled==identity,
                                      "native compute samples the FMASK view's actual GPU texture, not only its canonical CPU bytes");
                                void* empty=nullptr;reinterpret_cast<Bind>(vt(context)[67])(context,0,1,&empty);reinterpret_cast<Bind>(vt(context)[68])(context,0,1,&empty);set_shader(context,nullptr);
                            }
                            check(SUCCEEDED(sample_hr),"owned FMASK GPU sampling fixture creates real shader and UAV resources");
                            if(FAILED(sample_hr))std::cerr<<"FMASK fixture HRESULT 0x"<<std::hex<<sample_hr<<std::dec<<'\n';
                            if(compiler_errors){if(FAILED(sample_hr))std::cerr<<static_cast<const char*>(compiler_errors->GetBufferPointer());compiler_errors->Release();}
                            for(void* object:{uav,output,shader})if(object)reinterpret_cast<Count>(vt(object)[2])(object);
                            if(code)code->Release();if(compiler)FreeLibrary(compiler);
                            reinterpret_cast<Count>(vt(srv)[2])(srv);
                        }
                    }
                    using CreateRtv = HRESULT (STDMETHODCALLTYPE*)(void*, void*, const D3D11_RENDER_TARGET_VIEW_DESC*, void**);
                    const auto create_rtv = reinterpret_cast<CreateRtv>(vt(device)[9]);
                    void* view = nullptr;
                    D3D11_RENDER_TARGET_VIEW_DESC invalid{};
                    invalid.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D; invalid.Texture2D.MipSlice = 1;
                    check(create_rtv(device,texture,&invalid,&view)==(multisample ? DXGI_ERROR_UNSUPPORTED:S_OK) && (multisample ? !view:view!=nullptr),
                          "legacy RTV clamps its native mip like reference 14047ced0 and rejects incompatible sample dimensions");
                    if(view){D3D11_RENDER_TARGET_VIEW_DESC actual{};reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,D3D11_RENDER_TARGET_VIEW_DESC*)>(vt(view)[9])(view,&actual);
                        check(actual.Texture2D.MipSlice==1,"legacy RTV GetDesc preserves the requested mip before native clamping");reinterpret_cast<Count>(vt(view)[2])(view);view=nullptr;}
                    check(create_rtv(device, texture, nullptr, nullptr) == S_FALSE,
                          "default render-target view validates without acquiring references");
                    check(create_rtv(device, texture, nullptr, &view) == S_OK && view,
                          "native slot nine creates a real render-target descriptor");
                    if (view) {
                        struct { D3D11_RENDER_TARGET_VIEW_DESC desc{}; UINT guard = 0xaabbccdd; } result;
                        using GetDesc = void (STDMETHODCALLTYPE*)(void*, D3D11_RENDER_TARGET_VIEW_DESC*);
                        reinterpret_cast<GetDesc>(vt(view)[9])(view, &result.desc);
                        check(result.desc.Format == target.standard.Format && result.desc.ViewDimension ==
                            (multisample ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D) &&
                            !result.desc.Texture2D.MipSlice && result.guard == 0xaabbccdd, "RTV GetDesc preserves its twenty-byte ABI");
                        UINT type = 99; void* retained = nullptr;
                        std::memcpy(&type, static_cast<char*>(view) + 40, 4);
                        using GetResource = void (STDMETHODCALLTYPE*)(void*, void**);
                        reinterpret_cast<GetResource>(vt(view)[8])(view, &retained);
                        check(type == 0 && retained == texture, "RTV public type and retained resource match the original ABI");
                        if (retained) reinterpret_cast<Count>(vt(retained)[2])(retained);
                        check(reinterpret_cast<Count>(vt(texture)[2])(texture) == 1, "RTV retains owned texture after caller release");
                        texture = nullptr;
                        reinterpret_cast<Count>(vt(view)[2])(view);
                    }
                    if (texture) reinterpret_cast<Count>(vt(texture)[2])(texture);
                }
                }
            reinterpret_cast<Count>(vt(context)[2])(context);
            reinterpret_cast<Count>(vt(device)[2])(device);
            std::cout << "original XG graphics metadata " << (failures ? "FAILED" : "passed") << '\n';
        }
        return failures ? 1 : 0;
    }
    if (argc == 2 && std::string(argv[1]) == "--missing-slot") {
        set_stop_on_missing_activation(true);
        const auto vt = winrt_vtable();
        WinrtObject object{vt.data(), L"Xone.Tests.MissingSlot", __uuidof(IInspectable), nullptr};
        reinterpret_cast<void(*)(WinrtObject*, std::uintptr_t, std::uintptr_t)>(const_cast<void*>(vt[6]))(&object, 17, 29);
        return 1;
    }
    if (argc == 2 && std::string(argv[1]) == "--missing-activation") {
        RoInitialize(RO_INIT_MULTITHREADED);
        set_stop_on_missing_activation(true);
        HSTRING name = nullptr;
        constexpr wchar_t missing[] = L"Xone.Tests.MissingActivation";
        WindowsCreateString(missing, _countof(missing) - 1, &name);
        void* factory = nullptr;
        get_title_activation_factory(name, __uuidof(IActivationFactory), &factory);
        return 1; // The diagnostic must terminate before returning here.
    }
    if (argc != 3 && argc != 4) { std::cerr << "usage: era_loader_tests <era_component.exe> <era_tls_component.exe> [era_module_component.dll]\n"; return 2; }
    {
        using Allocate = void* (WINAPI*)(SIZE_T, ULONGLONG);
        using Release = void (WINAPI*)(void*, ULONGLONG);
        using SetHooks = void (WINAPI*)(Allocate, Release);
        const auto hooks = reinterpret_cast<SetHooks>(resolve_kernelx({"kernelx.dll", "XMemSetAllocationHooks", 0}));
        const auto allocate = reinterpret_cast<Allocate>(resolve_kernelx({"kernelx.dll", "XMemAlloc", 0}));
        const auto release = reinterpret_cast<Release>(resolve_kernelx({"kernelx.dll", "XMemFree", 0}));
        check(hooks && allocate && release, "XMem hooks resolved");
        if (hooks && allocate && release) {
            hooks(&allocate_hook, &release_hook);
            constexpr ULONGLONG attributes = 0x1234567887654321ull;
            void* result = allocate(137, attributes);
            check(result == &hook_size && hook_size == 137 && hook_attributes == attributes,
                  "XMem hook preserves size, result and 64-bit attributes");
            release(result, attributes);
            check(hook_pointer == result && hook_frees == 1 && hook_attributes == attributes,
                  "XMem release calls the registered callback");
            hooks(nullptr, nullptr);
            void* default_memory = allocate(16, 0);
            check(default_memory != nullptr, "cleared hooks use the default allocator");
            if (default_memory) release(default_memory, 0);
            release(nullptr, 0);
        }
    }
    {
        const auto startup = reinterpret_cast<decltype(&WSAStartup)>(resolve_host_module({"WS2_32.dll", "", 115}));
        const auto cleanup = reinterpret_cast<decltype(&WSACleanup)>(resolve_host_module({"WS2_32.dll", "", 116}));
        const auto socket_create = reinterpret_cast<decltype(&socket)>(resolve_host_module({"WS2_32.dll", "", 23}));
        const auto socket_close = reinterpret_cast<decltype(&closesocket)>(resolve_host_module({"WS2_32.dll", "", 3}));
        check(startup && cleanup && socket_create && socket_close, "audited Winsock ordinals resolved");
        check(resolve_host_module({"WS2_32.dll", "WSAStartup", 0}) == reinterpret_cast<void*>(startup),
              "Winsock named and ordinal bindings agree");
        check(!resolve_host_module({"WS2_32.dll", "", 65000}) &&
              !resolve_host_module({"WS2_32.dll", "XoneUnknownSocketApi", 0}), "unverified socket imports rejected");
        if (startup && cleanup && socket_create && socket_close) {
            WSADATA data{};
            const int error = startup(MAKEWORD(2, 2), &data);
            check(error == 0 && data.wVersion == MAKEWORD(2, 2), "Winsock initializes the requested version");
            if (!error) {
                const SOCKET handle = socket_create(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                check(handle != INVALID_SOCKET, "socket created without connecting to a server");
                if (handle != INVALID_SOCKET) check(socket_close(handle) == 0, "socket closes");
                check(cleanup() == 0, "Winsock cleanup succeeds");
            }
        }
    }
    {
        // These redistributables are optional; absence must stay unresolved.
        const std::pair<const char*, const wchar_t*> runtimes[]{
            {"MSVCR110.dll", L"msvcr110.dll"}, {"MSVCP110.dll", L"msvcp110.dll"},
            {"vccorlib110.DLL", L"vccorlib110.dll"}};
        for (const auto& [name, path] : runtimes) {
            const HMODULE module = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            check(resolve_host_module({name, "__xone_missing_export", 0}) == nullptr,
                  "missing legacy runtime export stays unresolved");
            check(resolve_host_module({name, "", 1}) == nullptr, "legacy runtime ordinals are not guessed");
            const char* symbol = same_library(name, "msvcr110.dll") ? "malloc" :
                (same_library(name, "msvcp110.dll") ? "?uncaught_exception@std@@YA_NXZ" :
                 "?InitializeData@Details@Platform@@YAJH@Z");
            const auto expected = module ? GetProcAddress(module, symbol) : nullptr;
            if (module) check(expected != nullptr, "installed legacy runtime has the tested named export");
            check(resolve_host_module({name, symbol, 0}) == reinterpret_cast<void*>(expected),
                  "legacy runtime resolves only the same-version System32 export");
            if (module) FreeLibrary(module);
        }
        const auto allocate = reinterpret_cast<void* (*)(std::size_t)>(
            resolve_host_module({"MSVCR110.dll", "malloc", 0}));
        const auto release = reinterpret_cast<void (*)(void*)>(
            resolve_host_module({"MSVCR110.dll", "free", 0}));
        const auto length = reinterpret_cast<std::size_t (*)(const char*)>(
            resolve_host_module({"MSVCR110.dll", "strlen", 0}));
        if (allocate && release && length) {
            void* block = allocate(32);
            check(block != nullptr, "legacy CRT allocation executes");
            if (block) {
                static_cast<char*>(block)[0] = 'x';
                static_cast<char*>(block)[1] = '\0';
                check(length(static_cast<char*>(block)) == 1, "legacy CRT string ABI executes");
                release(block);
            }
        }
        check(resolve_host_module({"untrusted.dll", "malloc", 0}) == nullptr,
              "runtime support does not enable arbitrary modules");
    }
    {
        // Static TLS: one title per process, bound to xone-era-tls.dll's slot.
        const auto tls_file = read(argv[2]);
        LoadResult result;
        {
            Image image;
            check(image.load(tls_file, resolve_kernelx, result), "TLS title loads");
            for (const auto& e : result.errors) std::cerr << "  " << e << '\n';
            if (image.entry()) {
                image.process_attach();
                const auto code = reinterpret_cast<unsigned long long (*)()>(image.entry())();
                if (code) std::cerr << "  TLS title returned " << code << '\n';
                check(code == 0, "TLS template, callbacks and per-thread isolation hold");
            }
        }
        Image second;
        check(!second.load(tls_file, resolve_kernelx, result), "slot is not silently reused by a second title");
    }
    const auto file = read(argv[1]);
    check(file.size() > 512, "fixture readable");
    check(bind_xg_module(std::filesystem::path(argv[1]).wstring()) == HRESULT_FROM_WIN32(ERROR_CRC),
          "unregistered XG snapshot is refused before mapping or execution");
    check(initialize_xg_module() == S_FALSE && !resolve_xg_module({"xg_x.dll", "XGComputeTexture2DLayout", 0}),
          "unbound original XG has no fabricated exports or initialization");
    {
        for (const auto& item : {std::pair{"msvcrt.dll", L"msvcrt.dll"}, std::pair{"ntdll.dll", L"ntdll.dll"}}) {
            const HMODULE host = LoadLibraryExW(item.second, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
            const auto* symbol = same_library(item.first, "msvcrt.dll") ? "sqrt" : "RtlCaptureContext";
            check(host && resolve_host_module({item.first, symbol, 0}) == reinterpret_cast<void*>(GetProcAddress(host, symbol)),
                  "XG dependencies bind the audited named System32 function");
            if (same_library(item.first,"msvcrt.dll"))
                for (const char* name : {"printf","sprintf","_purecall","__CxxFrameHandler3","_CxxThrowException",
                    "??1type_info@@UEAA@XZ","?what@exception@@UEBAPEBDXZ","??1exception@@UEAA@XZ",
                    "??0exception@@QEAA@AEBV0@@Z","??0exception@@QEAA@AEBQEBD@Z"})
                    check(host && GetProcAddress(host,name) && resolve_host_module({item.first,name,0}) == reinterpret_cast<void*>(GetProcAddress(host,name)),
                        "2018 XG CRT dependencies retain the exact host named ABI");
            check(!resolve_host_module({item.first, "unreviewed_export", 0}) && !resolve_host_module({item.first, {}, 1}),
                  "XG dependency support does not expose arbitrary names or ordinals");
            if (host) FreeLibrary(host);
        }
        const auto root = reinterpret_cast<double (*)(double)>(resolve_host_module({"msvcrt.dll", "sqrt", 0}));
        check(root && root(81.0) == 9.0, "plain-C CRT math dependency executes with the expected ABI");
    }

    {
        Image image;
        LoadResult result;
        check(image.load(file, resolve_kernelx, result), "fixture loads with kernelx contract");
        for (const auto& e : result.errors) std::cerr << "  " << e << '\n';
        check(result.resolved == 2 && result.unresolved.empty(), "both kernelx imports bound");
        check(image.entry() != nullptr, "entry point mapped");
        const auto module_handle = reinterpret_cast<HMODULE (WINAPI*)(LPCWSTR)>(
            resolve_kernelx({"kernelx.dll", "GetModuleHandleW", 0}));
        check(module_handle && module_handle(nullptr) == reinterpret_cast<HMODULE>(image.base()),
              "GetModuleHandleW(NULL) names the title image, not the launcher");
        check(module_handle && module_handle(L"kernel32.dll") == GetModuleHandleW(L"kernel32.dll"),
              "named GetModuleHandleW stays a host lookup");
        if (argc >= 4) {
            const auto module_file = read(argv[3]);
            LoadResult module_result;
            ULONG detached = 0;
            {
                Image module;
                check(!module.load(module_file, resolve_kernelx, module_result), "DLL is still refused as a title entry");
                check(!module.load(file, resolve_kernelx, module_result, ImageKind::module), "module route requires a DLL");
                check(module.load(module_file, resolve_kernelx, module_result, ImageKind::module), "owned DLL maps through the same loader");
                for (const auto& error : module_result.errors) std::cerr << "  module: " << error << '\n';
                const auto exported = reinterpret_cast<unsigned long long (*)()>(module.find_export("era_test_module_export"));
                const auto receipt = reinterpret_cast<void (*)(ULONG*)>(module.find_export("era_test_module_receipt"));
                check(exported && receipt, "validated module exports resolve by name");
                check(!module.find_export("unknown_export") && !module.find_export({}, 65000), "unknown names/ordinals stay unresolved");
                bool ordinal_matches = false;
                for (WORD ordinal = 1; ordinal <= 4; ++ordinal) ordinal_matches |= module.find_export({}, ordinal) == reinterpret_cast<void*>(exported);
                check(exported && ordinal_matches, "module ordinal resolves the same actual export");
                if (exported && receipt) {
                    check(exported() == GetCurrentProcessId(), "mapping did not execute DLL initialization");
                    check(module.attach_module() && module.attach_module(), "module attach is explicit and idempotent");
                    check(exported() == (1ull << 32 | GetCurrentProcessId()), "DLL initialization ran once and used audited kernel imports");
                    receipt(&detached);
                }
                check(module_handle && module_handle(nullptr) == reinterpret_cast<HMODULE>(image.base()), "helper loading preserves the title's module identity");
            }
            check(detached == 1, "module detach runs once before unmapping");
            check(module_handle && module_handle(nullptr) == reinterpret_cast<HMODULE>(image.base()), "helper destruction does not clear title identity");
            auto malformed = module_file;
            if (malformed.size() > sizeof(IMAGE_DOS_HEADER)) {
                const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(malformed.data());
                auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(malformed.data() + dos->e_lfanew);
                nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT] = {nt->OptionalHeader.SizeOfImage - 8, 64};
                Image rejected;
                check(!rejected.load(malformed, resolve_kernelx, module_result, ImageKind::module) && !rejected.base(),
                      "module with export tables outside the image cannot publish a mapping");
                auto tls_module = read(argv[2]);
                const auto tls_dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(tls_module.data());
                reinterpret_cast<IMAGE_NT_HEADERS64*>(tls_module.data() + tls_dos->e_lfanew)->FileHeader.Characteristics |= IMAGE_FILE_DLL;
                check(rejected.load(tls_module, resolve_kernelx, module_result, ImageKind::module), "module TLS uses a separate slot after the title slot retired");
                check(module_handle && module_handle(nullptr) == reinterpret_cast<HMODULE>(image.base()), "failed module validation preserves title identity");
            }
        }
        if (image.entry()) {
            MEMORY_BASIC_INFORMATION info{};
            VirtualQuery(image.entry(), &info, sizeof info);
            check(info.Protect == PAGE_EXECUTE_READ, "code section is execute-read, not writable");
            DWORD64 unwind_base = 0;
            check(RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(image.entry()), &unwind_base, nullptr) &&
                  unwind_base == reinterpret_cast<DWORD64>(image.base()), "unwind data registered for title code");
            const auto entry = reinterpret_cast<unsigned long long (*)()>(image.entry());
            const auto value = entry();
            check((value & 0xffffffffull) == GetCurrentProcessId(), "title called host through kernelx binding");
            check((value >> 32) == 42, "DIR64 relocation applied at non-preferred base");
        }
    }
    {
        Image image;
        LoadResult result;
        check(!image.load(file, [](const ImportRequest&) -> void* { return nullptr; }, result), "missing contract blocks load");
        check(result.unresolved.size() == 2 && result.unresolved[0].library == "kernelx.dll",
              "every unresolved import reported with its library");
        check(image.base() == nullptr, "failed load releases the image");
    }
    {
        Image image;
        LoadResult result;
        auto truncated = file;
        truncated.resize(256);
        check(!image.load(truncated, resolve_kernelx, result) && !result.errors.empty(), "truncated image rejected");
        auto wrong = file;
        const auto lfanew = *reinterpret_cast<const LONG*>(wrong.data() + 0x3c);
        wrong[lfanew + 4] = 0x4c; wrong[lfanew + 5] = 0x01; // IMAGE_FILE_MACHINE_I386
        check(!image.load(wrong, resolve_kernelx, result), "non-AMD64 image rejected");
        for (DWORD size : {DWORD(1), DWORD(13)}) {
            auto malformed = file;
            auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(malformed.data() + lfanew);
            nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION].Size = size;
            check(!image.load(malformed, resolve_kernelx, result) && !image.base() &&
                std::find(result.errors.begin(), result.errors.end(), "invalid unwind directory") != result.errors.end(),
                "incomplete unwind records are refused before registration");
        }
        auto padded = file;
        auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(padded.data() + lfanew);
        auto* sections = reinterpret_cast<IMAGE_SECTION_HEADER*>(reinterpret_cast<unsigned char*>(&nt->OptionalHeader) +
            nt->FileHeader.SizeOfOptionalHeader);
        auto& section = sections[0];
        check(section.SizeOfRawData > 1, "fixture has file-backed section padding");
        if (section.SizeOfRawData > 1) {
            section.Misc.VirtualSize = 1;
            padded[section.PointerToRawData + section.SizeOfRawData - 1] = 0x5a;
            check(image.load(padded, resolve_kernelx, result), "section raw bytes beyond VirtualSize map as in the reference");
            check(image.base() && image.base()[section.VirtualAddress + section.SizeOfRawData - 1] == 0x5a,
                "the full file-backed section survives mapping");
        }
    }
    check(resolve_kernelx({"KERNELX.DLL", "GetTickCount64", 0}) != nullptr, "library name is case-insensitive");
    check(resolve_kernelx({"kernelx.dll", "AllocateTitlePhysicalPages", 0}) == nullptr,
          "Xbox-specific export is not faked by a host forward");
    {
        const auto allocate = reinterpret_cast<decltype(&VirtualAlloc)>(resolve_kernelx({"kernelx.dll", "VirtualAlloc", 0}));
        const auto release = reinterpret_cast<decltype(&VirtualFree)>(resolve_kernelx({"kernelx.dll", "VirtualFree", 0}));
        check(allocate && release && reinterpret_cast<void*>(allocate) != reinterpret_cast<void*>(&VirtualAlloc),
              "title memory has an owned allocation boundary");
        if (allocate && release) {
            void* region = allocate(nullptr, 65536, 0x40000000 | MEM_RESERVE, PAGE_READWRITE);
            check(region != nullptr, "MEM_TITLE reserves without committing");
            if (region) {
                MEMORY_BASIC_INFORMATION info{};
                VirtualQuery(region, &info, sizeof info);
                check(info.State == MEM_RESERVE, "title reservation has no physical commit");
                auto* page = static_cast<unsigned char*>(allocate(region, 4096, MEM_COMMIT, PAGE_READWRITE));
                check(page == region, "commit preserves title virtual address");
                if (page) { page[0] = 42; check(page[0] == 42, "committed title memory writable"); }
                check(release(region, 4096, MEM_DECOMMIT), "title page decommits");
                check(release(region, 0, MEM_RELEASE), "title reservation released");
                check(!release(region, 0, MEM_RELEASE), "double release rejected");
            }
            void* host = VirtualAlloc(nullptr, 4096, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            check(host != nullptr, "host memory fixture allocated");
            if (host) {
                check(!release(host, 0, MEM_RELEASE), "title cannot release host memory");
                check(!allocate(host, 4096, MEM_COMMIT, PAGE_READWRITE), "title cannot commit into host memory");
                VirtualFree(host, 0, MEM_RELEASE);
            }
            check(!allocate(nullptr, 4096, 0x10000000 | MEM_RESERVE, PAGE_READWRITE) &&
                      GetLastError() == ERROR_NOT_SUPPORTED, "unsupported graphics memory flags are explicit");
            constexpr SIZE_T graphics_page = 1u << 21;
            void* probe = VirtualAlloc(nullptr, 2 * graphics_page, MEM_RESERVE, PAGE_NOACCESS);
            check(probe != nullptr, "graphics reservation address fixture");
            if (probe) {
                const auto aligned = (reinterpret_cast<std::uintptr_t>(probe) + graphics_page - 1) & ~(graphics_page - 1);
                VirtualFree(probe, 0, MEM_RELEASE);
                void* graphics = allocate(reinterpret_cast<void*>(aligned), graphics_page,
                    0x10000000 | MEM_LARGE_PAGES | MEM_RESERVE, PAGE_READWRITE);
                check(graphics == reinterpret_cast<void*>(aligned), "graphics reservation preserves exact aligned address");
                if (graphics) {
                    MEMORY_BASIC_INFORMATION info{};
                    VirtualQuery(graphics, &info, sizeof info);
                    check(info.State == MEM_RESERVE, "graphics reservation has no fabricated backing");
                    check(!allocate(graphics, 4096, MEM_COMMIT, PAGE_READWRITE) && GetLastError() == ERROR_NOT_SUPPORTED,
                          "CPU commit cannot bypass graphics mapping contract");
                    check(release(graphics, 0, MEM_RELEASE), "unused graphics reservation released");
                }
            }
        }
    }
    check(resolve_kernelx({"kernel32.dll", "Sleep", 0}) == nullptr, "only kernelx.dll is handled");
    {
        const auto create = reinterpret_cast<decltype(&CreateIoCompletionPort)>(
            resolve_kernelx({"kernelx.dll", "CreateIoCompletionPort", 0}));
        const auto post = reinterpret_cast<decltype(&PostQueuedCompletionStatus)>(
            resolve_kernelx({"kernelx.dll", "PostQueuedCompletionStatus", 0}));
        const auto get = reinterpret_cast<decltype(&GetQueuedCompletionStatus)>(
            resolve_kernelx({"kernelx.dll", "GetQueuedCompletionStatus", 0}));
        check(create && post && get, "completion port imports resolved");
        if (create && post && get) {
            HANDLE port = create(INVALID_HANDLE_VALUE, nullptr, 0, 1);
            check(port != nullptr, "title completion port created");
            if (port) {
                OVERLAPPED operation{};
                DWORD bytes = 0;
                ULONG_PTR key = 0;
                OVERLAPPED* received = nullptr;
                check(post(port, 37, 0x123456789ull, &operation) != FALSE, "completion packet queued");
                check(get(port, &bytes, &key, &received, 100) && bytes == 37 &&
                          key == 0x123456789ull && received == &operation,
                      "completion packet preserves bytes, 64-bit key and operation");
                SetLastError(ERROR_SUCCESS);
                check(!get(port, &bytes, &key, &received, 0) && GetLastError() == WAIT_TIMEOUT && !received,
                      "empty completion queue preserves real timeout");
                CloseHandle(port);
            }
        }
        for (const char* name : {"CopyFile2", "LoadLibraryW"})
            check(resolve_kernelx({"kernelx.dll", name, 0}) == nullptr,
                  "storage mutation, title memory and private loading remain explicit gaps");
    }
    {
        const auto remove = reinterpret_cast<BOOL (WINAPI*)(LPCWSTR)>(resolve_kernelx({"kernelx.dll", "DeleteFileW", 0}));
        check(remove != nullptr, "DeleteFileW bound to the title storage policy");
        wchar_t temp[MAX_PATH];
        GetTempPathW(MAX_PATH, temp);
        const std::wstring base = std::wstring(temp) + L"xone-era-storage-" + std::to_wstring(GetCurrentProcessId());
        const std::wstring root = base + L"\\root", outside = base + L"\\outside.bin", inside = root + L"\\save.bin";
        CreateDirectoryW(base.c_str(), nullptr);
        CreateDirectoryW(root.c_str(), nullptr);
        const auto touch = [](const std::wstring& p) {
            HANDLE h = CreateFileW(p.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
            return h != INVALID_HANDLE_VALUE && CloseHandle(h);
        };
        const auto exists = [](const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; };
        check(touch(inside) && touch(outside), "storage fixtures created");
        const auto open = reinterpret_cast<decltype(&CreateFileW)>(resolve_kernelx({"kernelx.dll", "CreateFileW", 0}));
        const auto write = reinterpret_cast<decltype(&WriteFile)>(resolve_kernelx({"kernelx.dll", "WriteFile", 0}));
        const auto truncate = reinterpret_cast<decltype(&SetEndOfFile)>(resolve_kernelx({"kernelx.dll", "SetEndOfFile", 0}));
        const auto flush = reinterpret_cast<decltype(&FlushFileBuffers)>(resolve_kernelx({"kernelx.dll", "FlushFileBuffers", 0}));
        check(open && write && truncate && flush, "storage handle APIs bound");
        if (open && write && truncate && flush) {
            set_title_package_root(root);
            HANDLE content=open(L"/save.bin",GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,0,nullptr);
            check(content!=INVALID_HANDLE_VALUE,"package-rooted content opens from the bound package, not the host drive root");
            if(content!=INVALID_HANDLE_VALUE)CloseHandle(content);
            check(open(L"/../outside.bin",GENERIC_READ,0,nullptr,OPEN_EXISTING,0,nullptr)==INVALID_HANDLE_VALUE && GetLastError()==ERROR_INVALID_NAME,
                "package-rooted content rejects traversal instead of escaping to the host");
            check(open(L"/save.bin:stream",GENERIC_READ,0,nullptr,OPEN_EXISTING,0,nullptr)==INVALID_HANDLE_VALUE && GetLastError()==ERROR_INVALID_NAME,
                "package-rooted content rejects alternate streams");
            set_title_package_root({});
            check(open(L"/save.bin",GENERIC_READ,0,nullptr,OPEN_EXISTING,0,nullptr)==INVALID_HANDLE_VALUE && GetLastError()==ERROR_ACCESS_DENIED,
                "unbound package-rooted content has no host fallback");
            set_title_storage_root({});
            check(open(inside.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr) == INVALID_HANDLE_VALUE &&
                      GetLastError() == ERROR_ACCESS_DENIED, "no storage: destructive open denied");
            set_title_storage_root(root);
            const auto mkdir = reinterpret_cast<decltype(&CreateDirectoryW)>(resolve_kernelx({"kernelx.dll", "CreateDirectoryW", 0}));
            const auto attributes = reinterpret_cast<decltype(&GetFileAttributesExA)>(resolve_kernelx({"kernelx.dll", "GetFileAttributesExA", 0}));
            const auto enumerate = reinterpret_cast<decltype(&FindFirstFileExA)>(resolve_kernelx({"kernelx.dll", "FindFirstFileExA", 0}));
            const auto move = reinterpret_cast<decltype(&MoveFileExW)>(resolve_kernelx({"kernelx.dll", "MoveFileExW", 0}));
            check(mkdir && attributes && enumerate && move, "temporary volume uses shared filesystem APIs");
            if (mkdir && attributes && enumerate && move) {
                check(mkdir(L"T:\\cache\\", nullptr), "temporary directory created inside session storage");
                HANDLE temporary = open(L"t:\\cache\\data.bin", GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
                check(temporary != INVALID_HANDLE_VALUE, "temporary volume open shares directory translation");
                if (temporary != INVALID_HANDLE_VALUE) {
                    DWORD written = 0;
                    check(write(temporary, "temp", 4, &written, nullptr) && written == 4, "temporary handle passes storage write policy");
                    CloseHandle(temporary);
                }
                WIN32_FILE_ATTRIBUTE_DATA info{};
                check(attributes("T:\\cache\\data.bin", GetFileExInfoStandard, &info) && info.nFileSizeLow == 4,
                      "temporary attributes query observes actual bytes");
                WIN32_FIND_DATAA found{};
                HANDLE search = enumerate("T:\\cache\\*.bin", FindExInfoStandard, &found, FindExSearchNameMatch, nullptr, 0);
                check(search != INVALID_HANDLE_VALUE && std::string(found.cFileName) == "data.bin", "temporary enumeration shares namespace");
                if (search != INVALID_HANDLE_VALUE) FindClose(search);
                check(move(L"T:\\cache\\data.bin", L"T:\\cache\\moved.bin", 0), "temporary rename shares namespace");
                check(remove(L"T:\\cache\\moved.bin"), "temporary delete shares namespace");
                check(!mkdir(L"T:\\..\\escape", nullptr) && GetLastError() == ERROR_ACCESS_DENIED,
                      "temporary traversal cannot reach saves or host paths");
                check(open(L"T:relative", GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr) == INVALID_HANDLE_VALUE &&
                      GetLastError() == ERROR_INVALID_NAME, "temporary drive-relative paths are explicit errors");
                set_title_storage_root({});
                check(!mkdir(L"T:\\unbound", nullptr) && GetLastError() == ERROR_ACCESS_DENIED, "temporary volume requires session storage");
                set_title_storage_root(root);
                const auto unicode_root = base + L"\\\u7f13\u5b58";
                check(CreateDirectoryW(unicode_root.c_str(), nullptr), "Unicode storage root created");
                set_title_storage_root(unicode_root);
                check(mkdir(L"T:\\root-test", nullptr), "temporary root rebind supports Unicode host paths");
                check(attributes("T:\\root-test", GetFileExInfoStandard, &info) && (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY),
                      "ANSI guest queries preserve Unicode host root");
                search = enumerate("T:\\root-*", FindExInfoStandard, &found, FindExSearchNameMatch, nullptr, 0);
                check(search != INVALID_HANDLE_VALUE && std::string(found.cFileName) == "root-test", "ANSI enumeration supports Unicode host root");
                if (search != INVALID_HANDLE_VALUE) FindClose(search);
                RemoveDirectoryW((unicode_root + L"\\temp\\root-test").c_str());
                RemoveDirectoryW((unicode_root + L"\\temp").c_str());
                RemoveDirectoryW(unicode_root.c_str());
                set_title_storage_root(root);
                RemoveDirectoryW((root + L"\\temp\\cache").c_str());
                RemoveDirectoryW((root + L"\\temp").c_str());
            }
            std::wstring short_root(32768, L'\0');
            const DWORD short_length = GetShortPathNameW(root.c_str(), short_root.data(), static_cast<DWORD>(short_root.size()));
            check(short_length && short_length < short_root.size(), "storage root resolves its short-name alias");
            if (short_length && short_length < short_root.size()) {
                short_root.resize(short_length);
                set_title_storage_root(short_root);
            }
            const auto created = root + L"\\new-save.bin";
            HANDLE fresh = open(created.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, 0, nullptr);
            check(fresh != INVALID_HANDLE_VALUE, "short-name root allows creation through its long-name alias");
            if (fresh != INVALID_HANDLE_VALUE) {
                DWORD bytes = 0;
                check(write(fresh, "new", 3, &bytes, nullptr) && bytes == 3 && flush(fresh),
                      "new save persists through normalized handle validation");
                CloseHandle(fresh);
            }
            DeleteFileW(created.c_str());
            const DWORD mutation_access[]{GENERIC_WRITE, GENERIC_ALL, MAXIMUM_ALLOWED, DELETE, FILE_APPEND_DATA};
            for (DWORD access : mutation_access)
                check(open(outside.c_str(), access, 0, nullptr, OPEN_EXISTING, 0, nullptr) == INVALID_HANDLE_VALUE &&
                          GetLastError() == ERROR_ACCESS_DENIED, "package mutation access denied");
            check(open(outside.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_FLAG_DELETE_ON_CLOSE, nullptr) ==
                      INVALID_HANDLE_VALUE, "read access cannot request package deletion on close");
            check(open((inside + L":stream").c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr) ==
                      INVALID_HANDLE_VALUE, "alternate data streams refused");
            HANDLE inherited = CreateFileW(outside.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            DWORD written = 0;
            check(inherited != INVALID_HANDLE_VALUE, "outside handle fixture opened");
            if (inherited != INVALID_HANDLE_VALUE) {
                check(!write(inherited, "x", 1, &written, nullptr) && GetLastError() == ERROR_ACCESS_DENIED,
                      "outside handle cannot bypass write policy");
                check(!truncate(inherited) && !flush(inherited), "outside handle cannot truncate or flush");
                CloseHandle(inherited);
            }
            HANDLE save = open(inside.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            check(save != INVALID_HANDLE_VALUE, "writable save opened");
            if (save != INVALID_HANDLE_VALUE) {
                check(write(save, "save", 4, &written, nullptr) && written == 4 && flush(save),
                      "save data written and flushed");
                SetFilePointer(save, 2, nullptr, FILE_BEGIN);
                check(truncate(save) && GetFileSize(save, nullptr) == 2, "save truncates at current offset");
                CloseHandle(save);
            }
            save = open(inside.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            if (save != INVALID_HANDLE_VALUE) {
                char data[4]{};
                DWORD count = 0;
                check(ReadFile(save, data, sizeof data, &count, nullptr) && count == 2 && data[0] == 's' && data[1] == 'a',
                      "save bytes persist across handle reopen");
                CloseHandle(save);
            } else check(false, "saved data reopens");
            const std::wstring extended = L"\\\\?\\" + inside;
            save = open(extended.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
            check(save != INVALID_HANDLE_VALUE, "extended path uses the same storage policy");
            if (save != INVALID_HANDLE_VALUE) CloseHandle(save);
        }
        if (remove) {
            set_title_storage_root({});
            check(!remove(inside.c_str()) && GetLastError() == ERROR_ACCESS_DENIED && exists(inside),
                  "no writable storage: delete denied like a read-only package");
            set_title_storage_root(root);
            check(!remove(outside.c_str()) && GetLastError() == ERROR_ACCESS_DENIED && exists(outside),
                  "host path outside title storage is never deleted");
            check(!remove((root + L"\\..\\outside.bin").c_str()) && exists(outside), "dot-dot cannot escape storage");
            check(!remove((base + L"\\root-sibling.bin").c_str()) && GetLastError() == ERROR_ACCESS_DENIED,
                  "prefix match requires a path separator");
            check(remove(inside.c_str()) && !exists(inside), "file inside title storage deleted");
            check(!remove(inside.c_str()) && GetLastError() == ERROR_FILE_NOT_FOUND, "real host error kept for missing file");
            set_title_storage_root({});
        }
        DeleteFileW(outside.c_str());
        RemoveDirectoryW(root.c_str());
        RemoveDirectoryW(base.c_str());
    }
    {
        using Register = ULONG (WINAPI*)(const void*, PREGHANDLE);
        using Write = ULONG (WINAPI*)(const void*, const void*, REGHANDLE, ULONG, void*);
        using Fill = void (WINAPI*)(void*, PUINT8, SIZE_T);
        const auto reg = reinterpret_cast<Register>(resolve_title_import({"EtwPlus.dll", "EtxRegister", 0}));
        const auto write = reinterpret_cast<Write>(resolve_title_import({"EtwPlus.dll", "EtxEventWrite", 0}));
        const auto fill = reinterpret_cast<Fill>(resolve_title_import({"EtwPlus.dll", "EtxFillCommonFields_v7", 0}));
        check(reg && write && fill, "EtwPlus imports bound");
        if (reg && write && fill) {
            const char descriptor[64] = {};
            REGHANDLE a = 0, b = 0;
            check(reg(descriptor, &a) == ERROR_SUCCESS && reg(descriptor, &b) == ERROR_SUCCESS && a && b && a != b,
                  "providers receive distinct handles");
            check(reg(nullptr, &a) == ERROR_INVALID_PARAMETER, "null provider rejected");
            check(write(descriptor, descriptor, a, 0, nullptr) == ERROR_SUCCESS, "event accepted offline");
            check(write(descriptor, descriptor, a, 1, nullptr) == ERROR_INVALID_PARAMETER, "missing event data rejected");
            unsigned char field[16];
            for (auto& c : field) c = 0xcc;
            unsigned char scratch[64];
            fill(field, scratch, sizeof scratch);
            bool zero = true;
            for (auto c : field) zero = zero && c == 0;
            check(zero, "common fields are an empty payload offline");
        }
        check(resolve_title_import({"EtwPlus.dll", "EtxUploadNow", 0}) == nullptr, "unobserved EtwPlus export not invented");
    }
    check(resolve_kernelx({"kernelx.dll", "EventRegister", 0}) ==
              reinterpret_cast<void*>(GetProcAddress(LoadLibraryW(L"advapi32.dll"), "EventRegister")),
          "ETW provider API resolves to advapi32");
    check(resolve_title_import({"UCRTBASE.dll", "malloc", 0}) ==
              reinterpret_cast<void*>(GetProcAddress(LoadLibraryW(L"ucrtbase.dll"), "malloc")),
          "CRT import binds to System32 ucrtbase");
    check(resolve_title_import({"VCRUNTIME140.dll", "memcpy", 0}) != nullptr, "vcruntime140 bound");
    {
        using Create = HRESULT (WINAPI*)(void*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT,
                                         void**, D3D_FEATURE_LEVEL*, void**);
        const auto create = reinterpret_cast<Create>(resolve_title_import({"d3d11_x.dll", "D3D11CreateDevice", 0}));
        const HMODULE host_d3d11 = LoadLibraryW(L"d3d11.dll");
        check(create && reinterpret_cast<void*>(create) != reinterpret_cast<void*>(GetProcAddress(host_d3d11, "D3D11CreateDevice")),
              "D3D11.X bound to the D3D12 translation, not host D3D11");
        check(resolve_title_import({"d3d11_x.dll", "D3D11XCreateDeviceX", 0}) == nullptr, "unobserved D3D11.X export not invented");
        void* device = nullptr;
        void* context = nullptr;
        D3D_FEATURE_LEVEL level{};
        HRESULT hr = create ? create(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, 7, &device, &level, &context) : E_FAIL;
        if (FAILED(hr) && create) hr = create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, 7, &device, &level, &context);
        check(SUCCEEDED(hr) && device && context, "D3D11.X device and immediate context created over D3D12");
        if (device && context) {
            using Query = HRESULT (STDMETHODCALLTYPE*)(void*, REFIID, void**);
            using Count = ULONG (STDMETHODCALLTYPE*)(void*);
            const auto vt = [](void* object) { return *static_cast<void* const* const*>(object); };
            const auto refcount = [](void* object) { return *reinterpret_cast<const ULONG*>(static_cast<char*>(object) + 12); };
            check(level >= D3D_FEATURE_LEVEL_10_0 && level <= D3D_FEATURE_LEVEL_11_1, "feature level capped at 11_1");
            check(*reinterpret_cast<const ULONG*>(static_cast<char*>(device) + 8) == 0, "IGraphicsUnknown flags field zero");
            check(refcount(device) == 2 && refcount(context) == 1, "title-visible refcounts (context holds the device)");
            constexpr GUID device_x{0x177700F9, 0x876A, 0x4436, {0xB3, 0x68, 0x36, 0xA6, 0x04, 0xF8, 0x2C, 0xEF}};
            constexpr GUID context_x{0x48800095, 0x7134, 0x4BE7, {0x91, 0x86, 0xB8, 0x6B, 0xEC, 0xB2, 0x64, 0x77}};
            void* same = nullptr;
            check(reinterpret_cast<Query>(vt(device)[0])(device, device_x, &same) == S_OK && same == device &&
                  refcount(device) == 3, "QueryInterface(ID3D11DeviceX) returns the device with a reference");
            check(reinterpret_cast<Count>(vt(device)[2])(device) == 2, "Release decrements the visible refcount");
            same = &same;
            check(reinterpret_cast<Query>(vt(device)[0])(device, context_x, &same) == E_NOINTERFACE && !same,
                  "unknown interface rejected with E_NOINTERFACE");
            check(reinterpret_cast<Query>(vt(context)[0])(context, context_x, &same) == S_OK && same == context,
                  "context answers ID3D11DeviceContextX");
            reinterpret_cast<Count>(vt(context)[2])(context);
            *reinterpret_cast<ULONG*>(static_cast<char*>(context) + 12) += 1; // Inline title AddRef.
            check(reinterpret_cast<Count>(vt(context)[2])(context) == 1, "Release honours an inline refcount change");

            // DXGI chain the title walks to its swapchain: device -> IDXGIDevice1 -> adapter -> factory.
            {
                constexpr GUID dxgi_device1{0x77DB970F, 0x6276, 0x48BA, {0xBA, 0x28, 0x07, 0x01, 0x43, 0xB4, 0x39, 0x2C}};
                constexpr GUID factory2{0x50C83A1C, 0xE072, 0x4C48, {0x87, 0xB0, 0x36, 0x30, 0xFA, 0x36, 0xA6, 0xD0}};
                const ULONG before = refcount(device);
                void* dxgi = nullptr;
                void* adapter = nullptr;
                void* factory = nullptr;
                check(reinterpret_cast<Query>(vt(device)[0])(device, dxgi_device1, &dxgi) == S_OK && dxgi && dxgi != device &&
                      refcount(device) == before + 1, "device answers IDXGIDevice1 with a DXGI object holding it");
                using GetAdapter = HRESULT (STDMETHODCALLTYPE*)(void*, void**);
                using GetParent = HRESULT (STDMETHODCALLTYPE*)(void*, REFIID, void**);
                if (dxgi) check(reinterpret_cast<GetAdapter>(vt(dxgi)[7])(dxgi, &adapter) == S_OK && adapter, "GetAdapter");
                if (adapter) check(reinterpret_cast<GetParent>(vt(adapter)[6])(adapter, factory2, &factory) == S_OK && factory,
                                   "adapter parent is IDXGIFactory2");
                if (factory) {
                    using CreateForWindow = HRESULT (STDMETHODCALLTYPE*)(void*, void*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*,
                                                                         void*, void**);
                    DXGI_SWAP_CHAIN_DESC1 desc{1920, 1080, DXGI_FORMAT_B8G8R8A8_UNORM, FALSE, {1, 0}, DXGI_USAGE_RENDER_TARGET_OUTPUT,
                                               2, DXGI_SCALING_STRETCH, DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL};
                    void* chain = &chain;
                    IUnknown* foreign = reinterpret_cast<IUnknown*>(&desc);
                    check(reinterpret_cast<CreateForWindow>(vt(factory)[16])(factory, device, foreign, &desc, nullptr, &chain) ==
                              DXGI_ERROR_INVALID_CALL && !chain, "swapchain only for the title-layer CoreWindow");
                    reinterpret_cast<Count>(vt(factory)[2])(factory);
                }
                if (adapter) reinterpret_cast<Count>(vt(adapter)[2])(adapter);
                if (dxgi) reinterpret_cast<Count>(vt(dxgi)[2])(dxgi);
                check(refcount(device) == before, "DXGI objects release their device references");
            }

            static const char* trapped_interface = nullptr;
            static unsigned trapped_slot = 0;
            set_d3d11x_trap_handler([](const char* name, unsigned slot) -> std::uintptr_t {
                trapped_interface = name;
                trapped_slot = slot;
                return 0xdead;
            });
            using Any = std::uintptr_t (*)(void*);
            check(reinterpret_cast<Any>(vt(device)[6])(device) == 0xdead && trapped_slot == 6 &&
                  std::string(trapped_interface) == "ID3D11DeviceX", "unimplemented device slot reports interface and slot");
            check(reinterpret_cast<Any>(vt(context)[300])(context) == 0xdead && trapped_slot == 300 &&
                  std::string(trapped_interface) == "ID3D11DeviceContextX", "unimplemented context slot reports itself");
            set_d3d11x_trap_handler(nullptr);
            check(reinterpret_cast<Count>(vt(device)[2])(device) == 1, "title releases its device reference");
            check(reinterpret_cast<Count>(vt(context)[2])(context) == 0, "last context release frees context and device");
        }
        D3D_FEATURE_LEVEL probe{};
        check(create && create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, 7, nullptr, &probe, nullptr) == S_FALSE &&
              probe != D3D_FEATURE_LEVEL{}, "level probe without outputs returns S_FALSE");
        check(create && create(nullptr, D3D_DRIVER_TYPE_REFERENCE, nullptr, 0, nullptr, 0, 7, &device, nullptr, nullptr) ==
              DXGI_ERROR_UNSUPPORTED && !device, "unsupported driver type rejected");
    }

    {
        // Synthetic resource binds the old ABI; no original DLL is loaded.
        check(bind_d3d11x_abi(std::filesystem::absolute(argv[1]).wstring()) == S_OK, "old graphics ABI version binding");
        struct Parameters {
            UINT version = 0x1000b, flags = 0x04000000;
            void* tessellation = nullptr;
            void* factors = nullptr;
            UINT affinity = 0x3f, de = 0, ce = 0, de_segment = 0, ce_segment = 0;
        } p;
        using Create = HRESULT (WINAPI*)(const Parameters*, void**, void**);
        const auto create = reinterpret_cast<Create>(resolve_title_import({"d3d11_x.dll", "D3D11XCreateDeviceX", 0}));
        check(create != nullptr, "bound old graphics factory resolved");
        using MapEsram = HRESULT (WINAPI*)(UINT, void*, UINT, const UINT*);
        const auto map_esram = reinterpret_cast<MapEsram>(resolve_title_import({"d3d11_x.dll", "D3DMapEsramMemory", 0}));
        check(map_esram && map_esram(1, nullptr, 1, nullptr) == E_INVALIDARG, "bound ESRAM import validates the original four-argument ABI");
        void* device = &p;
        void* context = &p;
        if (create) {
            check(create(nullptr, &device, &context) == E_INVALIDARG && !device && !context, "null parameters clear outputs");
            check(create(&p, &device, nullptr) == E_INVALIDARG && !device, "both device outputs required");
            p.version = 7;
            check(create(&p, &device, &context) == E_INVALIDARG && !device && !context, "unknown SDK rejected");
            p.version = 0x1000b;
            p.flags = 0;
            check(create(&p, &device, &context) == DXGI_ERROR_UNSUPPORTED && !device && !context,
                  "unimplemented deferred-deletion policy rejected");
            p.flags = 0x04000002;
            check(create(&p, &device, &context) == DXGI_ERROR_UNSUPPORTED, "unknown creation flags not ignored");
            p.flags = 0x04000000;
            p.de = 4096;
            check(create(&p, &device, &context) == DXGI_ERROR_UNSUPPORTED, "caller GPU ring storage not ignored");
            p.de = 0;
            check(create(&p, &device, &context) == S_OK && device && context, "legacy graphics device created");
            if (device && context) {
                const auto vt = [](void* o){return *static_cast<void* const* const*>(o);};
                const auto refs = [](void* o)->ULONG&{return *reinterpret_cast<ULONG*>(static_cast<char*>(o)+8);};
                using Count = ULONG (STDMETHODCALLTYPE*)(void*);
                using Flags = UINT (STDMETHODCALLTYPE*)(void*);
                check(refs(device) == 1 && refs(context) == 1, "legacy inline counts match original initial values");
                {
                    struct BufferDesc { D3D11_BUFFER_DESC standard; UINT64 extension; } desc{{72, D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER, 0, 0, 0}, 0};
                    static_assert(sizeof(BufferDesc) == 32);
                    using SetHooks = void (WINAPI*)(GraphicsAllocate, GraphicsFree);
                    const auto set_hooks = reinterpret_cast<SetHooks>(resolve_kernelx({"kernelx.dll", "XMemSetAllocationHooks", 0}));
                    graphics_default_allocate = reinterpret_cast<GraphicsAllocate>(resolve_kernelx({"kernelx.dll", "XMemAllocDefault", 0}));
                    graphics_default_free = reinterpret_cast<GraphicsFree>(resolve_kernelx({"kernelx.dll", "XMemFreeDefault", 0}));
                    graphics_hook_allocations = graphics_hook_frees = graphics_hook_wrong_allocator = 0;
                    set_hooks(graphics_allocate_hook, graphics_free_hook);
                    using CreateBuffer = HRESULT (STDMETHODCALLTYPE*)(void*, const BufferDesc*, const D3D11_SUBRESOURCE_DATA*, void**);
                    const auto factory = reinterpret_cast<CreateBuffer>(vt(device)[3]);
                    std::array<UINT32, 18> vertices{};
                    for (UINT i = 0; i < vertices.size(); ++i) vertices[i] = 0xaabbcc00 + i;
                    D3D11_SUBRESOURCE_DATA initial{vertices.data(), 0, 0};
                    void* buffer = device;
                    check(factory(device, nullptr, &initial, &buffer) == E_INVALIDARG && !buffer, "buffer description required");
                    check(factory(device, &desc, &initial, nullptr) == S_FALSE, "buffer validation only");
                    check(factory(device, &desc, &initial, &buffer) == S_OK && buffer, "shared native vertex buffer created");
                    if (buffer) {
                        check(refs(buffer) == 1 && refs(device) == 1, "buffer owns internal device dependency");
                        using GarbageCollect=void(STDMETHODCALLTYPE*)(void*,UINT);
                        const auto collect=reinterpret_cast<GarbageCollect>(vt(device)[69]);
                        collect(device,0);
                        check(refs(buffer)==1,"synchronous garbage collection preserves live resource ownership");
                        static UINT collection_slot=0;collection_slot=0;
                        set_d3d11x_trap_handler([](const char*,UINT slot)->std::uintptr_t{collection_slot=slot;return 0;});
                        collect(device,1);set_d3d11x_trap_handler(nullptr);
                        check(collection_slot==69 && refs(buffer)==1,"unsupported garbage collection flags stay explicit and preserve live resources");
                        using BindVertices=void(STDMETHODCALLTYPE*)(void*,UINT,UINT,void* const*,const UINT*,const UINT*);
                        const auto bind_vertices=reinterpret_cast<BindVertices>(vt(context)[19]);
                        const UINT stride=12,offset=4;void* buffers[]{buffer};
                        bind_vertices(context,31,1,buffers,&stride,&offset);bind_vertices(context,31,1,buffers,&stride,&offset);
                        check(refs(buffer)==2,"legacy vertex binding retains the last slot and balances rebinds");
                        bind_vertices(context,0,1,buffers,&stride,&offset);
                        check(refs(buffer)==3,"independent vertex slots retain independent references");
                        bind_vertices(context,31,1,nullptr,nullptr,nullptr);
                        check(refs(buffer)==1,"11295 null vertex array clears all slots without reading stride or offset pointers");
                        void* allocation = nullptr;
                        std::memcpy(&allocation, static_cast<char*>(buffer) + 48, sizeof allocation);
                        check(allocation && !std::memcmp(allocation, vertices.data(), sizeof vertices), "initial data reaches actual title allocation");
                        std::array<UINT32, 4> srd{};
                        std::memcpy(srd.data(), static_cast<char*>(buffer) + 32, sizeof srd);
                        const UINT64 address = srd[0] | (UINT64(srd[1] & 255) << 32);
                        check(address == reinterpret_cast<std::uintptr_t>(allocation) && !(srd[1] & 0xffffff00) && srd[2] == 72 && srd[3] == 0x24fac,
                              "native buffer SRD addresses the same CPU/GPU allocation");
                        using GetType = void (STDMETHODCALLTYPE*)(void*, D3D11_RESOURCE_DIMENSION*);
                        D3D11_RESOURCE_DIMENSION dimension{};
                        reinterpret_cast<GetType>(vt(buffer)[8])(buffer, &dimension);
                        check(dimension == D3D11_RESOURCE_DIMENSION_BUFFER, "buffer GetType native slot");
                        using GetDescriptor = void (STDMETHODCALLTYPE*)(void*, void**);
                        void* resource = nullptr;
                        reinterpret_cast<GetDescriptor>(vt(buffer)[11])(buffer, &resource);
                        check(resource == buffer && refs(buffer) == 1, "resource descriptor is original identity without extra reference");
                        using GetDesc = void (STDMETHODCALLTYPE*)(void*, BufferDesc*);
                        struct { BufferDesc desc; UINT64 guard = 0x123456789; } result{};
                        reinterpret_cast<GetDesc>(vt(buffer)[12])(buffer, &result.desc);
                        check(result.desc.standard.ByteWidth == 72 && result.desc.standard.BindFlags == D3D11_BIND_VERTEX_BUFFER &&
                            result.desc.extension == 0 && result.guard == 0x123456789, "buffer GetDesc writes old thirty-two-byte description only");
                        check(reinterpret_cast<Count>(vt(buffer)[2])(buffer) == 0, "buffer resource and registry pin retired");
                        MEMORY_BASIC_INFORMATION memory{};
                        VirtualQuery(allocation, &memory, sizeof memory);
                        check(memory.State == MEM_FREE, "last buffer reference frees its title allocation");
                    }
                    desc.standard.Usage = D3D11_USAGE_IMMUTABLE;
                    check(factory(device, &desc, nullptr, &buffer) == E_INVALIDARG && !buffer, "immutable buffer needs initial bytes");
                    desc.standard.Usage = D3D11_USAGE_DEFAULT; desc.extension = 1;
                    check(factory(device, &desc, &initial, &buffer) == DXGI_ERROR_UNSUPPORTED && !buffer, "unknown old buffer extension rejected");
                    set_stop_on_missing_activation(true);
                    set_d3d11x_trap_handler([](const char* name, unsigned slot) -> std::uintptr_t {
                        return std::strcmp(name, "ID3D11DeviceX") == 0 && slot == 3 ? 0x80004001u : 0x80004005u;
                    });
                    check(factory(device, &desc, &initial, &buffer) == E_NOTIMPL && !buffer,
                          "diagnostic stops at unsupported buffer variant before caller consumes null resource");
                    set_d3d11x_trap_handler(nullptr);
                    set_stop_on_missing_activation(false);
                    desc.extension = 0; desc.standard.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
                    check(factory(device, &desc, &initial, &buffer) == E_INVALIDARG && !buffer, "constant buffer size must be a multiple of sixteen");
                    desc.standard.ByteWidth = 64;
                    check(factory(device, &desc, &initial, &buffer) == S_OK && buffer, "shared constant buffer created");
                    if (buffer) {
                        using BindConstants = void(STDMETHODCALLTYPE*)(void*,UINT,UINT,void* const*);
                        const auto bind = reinterpret_cast<BindConstants>(vt(context)[8]);
                        void* buffers[]{buffer};
                        bind(context,13,1,buffers);
                        check(reinterpret_cast<Count>(vt(buffer)[1])(buffer)==3,"legacy VS constant binding retains the last valid slot");
                        reinterpret_cast<Count>(vt(buffer)[2])(buffer);
                        bind(context,13,1,buffers);
                        check(reinterpret_cast<Count>(vt(buffer)[1])(buffer)==3,"legacy VS constant rebinding balances ownership");
                        reinterpret_cast<Count>(vt(buffer)[2])(buffer);
                        bind(context,13,1,nullptr);
                        check(reinterpret_cast<Count>(vt(buffer)[1])(buffer)==2,"legacy VS constant unbind releases its retained buffer");
                        reinterpret_cast<Count>(vt(buffer)[2])(buffer);
                        bind(context,14,0,nullptr);
                        const auto bind_pixel = reinterpret_cast<BindConstants>(vt(context)[17]);
                        const auto bind_geometry = reinterpret_cast<BindConstants>(vt(context)[23]);
                        const auto bind_compute = reinterpret_cast<BindConstants>(vt(context)[71]);
                        bind(context,0,1,buffers); bind_pixel(context,0,1,buffers);
                        bind_geometry(context,13,1,buffers);bind_geometry(context,13,1,buffers);
                        bind_compute(context,0,1,buffers);bind_compute(context,0,1,buffers);
                        check(reinterpret_cast<Count>(vt(buffer)[1])(buffer)==6,"VS, PS, GS and CS constants retain independent references and balance rebinds");
                        reinterpret_cast<Count>(vt(buffer)[2])(buffer);
                        bind(context,0,1,nullptr);
                        check(reinterpret_cast<Count>(vt(buffer)[1])(buffer)==5,"VS unbind preserves PS, GS and CS constant ownership");
                        reinterpret_cast<Count>(vt(buffer)[2])(buffer);
                        reinterpret_cast<void(STDMETHODCALLTYPE*)(void*)>(vt(context)[110])(context);
                        check(reinterpret_cast<Count>(vt(buffer)[1])(buffer)==2,"legacy ClearState releases PS, GS and CS constants");
                        reinterpret_cast<Count>(vt(buffer)[2])(buffer);
                    }
                    if (buffer) {
                        std::array<UINT32, 4> srd{};
                        std::memcpy(srd.data(), static_cast<char*>(buffer) + 32, sizeof srd);
                        check((srd[1] & 0x3fff0000) == 0x100000 && srd[2] == 4, "constant SRD preserves sixteen-byte stride and record count");
                        reinterpret_cast<Count>(vt(buffer)[2])(buffer);
                    }
                    desc.standard = {80, D3D11_USAGE_DYNAMIC, D3D11_BIND_CONSTANT_BUFFER, D3D11_CPU_ACCESS_READ, 0, 0};
                    check(factory(device, &desc, nullptr, &buffer) == DXGI_ERROR_UNSUPPORTED && !buffer,
                          "dynamic buffers refuse unimplemented CPU-read storage");
                    desc.standard.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
                    check(factory(device, &desc, nullptr, &buffer) == S_OK && buffer, "observed eighty-byte dynamic constant buffer created");
                    if (buffer) {
                        using Map = HRESULT (STDMETHODCALLTYPE*)(void*, void*, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*);
                        using Unmap = void (STDMETHODCALLTYPE*)(void*, void*);
                        const auto map = reinterpret_cast<Map>(vt(context)[15]);
                        const auto unmap = reinterpret_cast<Unmap>(vt(context)[16]);
                        struct { D3D11_MAPPED_SUBRESOURCE mapped{}; UINT64 guard = 0xabcdef; } result;
                        check(map(context, buffer, 1, D3D11_MAP_WRITE_DISCARD, 0, &result.mapped) == E_INVALIDARG && !result.mapped.pData,
                              "buffer mapping validates the subresource and clears its output");
                        check(map(context, buffer, 0, D3D11_MAP_READ, 0, &result.mapped) == E_INVALIDARG,
                              "write-only dynamic storage cannot be read-mapped");
                        check(map(context, buffer, 0, D3D11_MAP_WRITE_NO_OVERWRITE, 0, &result.mapped) == DXGI_ERROR_UNSUPPORTED,
                              "constant buffer no-overwrite feature is explicit until implemented");
                        check(map(context, buffer, 0, D3D11_MAP_WRITE_DISCARD, 1, &result.mapped) == DXGI_ERROR_UNSUPPORTED,
                              "unknown Xbox map flags are not silently masked");
                        check(map(context, buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &result.mapped) == S_OK && result.mapped.pData,
                              "native context slot fifteen maps real dynamic storage");
                        void* allocation = nullptr;
                        std::memcpy(&allocation, static_cast<char*>(buffer) + 48, sizeof allocation);
                        check(result.mapped.pData == allocation && result.guard == 0xabcdef && refs(buffer) == 1,
                              "map preserves original buffer identity, address and output extent");
                        if (result.mapped.pData) {
                            auto* words = static_cast<UINT32*>(result.mapped.pData);
                            for (UINT i = 0; i < 20; ++i) words[i] = 0xabc00000 + i;
                            D3D11_MAPPED_SUBRESOURCE duplicate{};
                            check(map(context, buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &duplicate) == HRESULT_FROM_WIN32(ERROR_BUSY) && !duplicate.pData,
                                  "a second map cannot alias active CPU writes");
                            unmap(context, buffer);
                            check(static_cast<UINT32*>(allocation)[19] == 0xabc00013, "unmap keeps CPU writes in canonical title bytes");
                            check(map(context, buffer, 0, D3D11_MAP_WRITE_DISCARD, D3D11_MAP_FLAG_DO_NOT_WAIT, &result.mapped) == DXGI_ERROR_UNSUPPORTED && !result.mapped.pData,
                                  "unimplemented map flag combinations remain explicit");
                            check(map(context, buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &result.mapped) == S_OK,
                                  "completed dynamic buffer can be mapped again");
                            if (result.mapped.pData) unmap(context, buffer);
                        }
                        reinterpret_cast<Count>(vt(buffer)[2])(buffer);
                    }
                    desc.standard = {1332, D3D11_USAGE_DYNAMIC, D3D11_BIND_SHADER_RESOURCE,
                        D3D11_CPU_ACCESS_WRITE, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 36};
                    check(factory(device, &desc, nullptr, &buffer) == S_OK && buffer,
                          "observed dynamic structured buffer retains its Xbox CPU-write contract");
                    if (buffer) {
                        using Map = HRESULT (STDMETHODCALLTYPE*)(void*, void*, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*);
                        using Unmap = void (STDMETHODCALLTYPE*)(void*, void*);
                        const auto map = reinterpret_cast<Map>(vt(context)[15]);
                        D3D11_MAPPED_SUBRESOURCE mapped{};
                        check(map(context, buffer, 0, D3D11_MAP_WRITE_NO_OVERWRITE, 0, &mapped) == DXGI_ERROR_UNSUPPORTED && !mapped.pData,
                              "structured SRV no-overwrite remains unsupported");
                        check(map(context, buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped) == S_OK && mapped.pData && mapped.RowPitch == 1332,
                              "dynamic structured map exposes the complete canonical span");
                        if (mapped.pData) {
                            static_cast<unsigned char*>(mapped.pData)[1331] = 0xa7;
                            reinterpret_cast<Unmap>(vt(context)[16])(context, buffer);
                            void* allocation = nullptr;
                            std::memcpy(&allocation, static_cast<char*>(buffer) + 48, sizeof allocation);
                            check(allocation == mapped.pData && static_cast<unsigned char*>(allocation)[1331] == 0xa7,
                                  "structured writes preserve shared memory identity");
                        }
                        reinterpret_cast<Count>(vt(buffer)[2])(buffer);
                    }
                    desc.standard = {280000, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
                        0, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 28};
                    check(factory(device, &desc, nullptr, &buffer) == S_OK && buffer, "observed structured UAV buffer binds actual shared GPU resource");
                    if (buffer) {
                        std::array<UINT32, 4> srd{};
                        std::memcpy(srd.data(), static_cast<char*>(buffer) + 32, sizeof srd);
                        check((srd[1] & 0x3fff0000) == (28u << 16) && srd[2] == 10000 && srd[3] == 0x24fac,
                              "structured resource SRD preserves stride and record count");
                        using CreateUav = HRESULT (STDMETHODCALLTYPE*)(void*, void*, const D3D11_UNORDERED_ACCESS_VIEW_DESC*, void**);
                        const auto create_uav = reinterpret_cast<CreateUav>(vt(device)[8]);
                        D3D11_UNORDERED_ACCESS_VIEW_DESC view_desc{};
                        view_desc.Format = DXGI_FORMAT_UNKNOWN; view_desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
                        view_desc.Buffer = {2, 3, D3D11_BUFFER_UAV_FLAG_APPEND};
                        void* view = buffer;
                        check(create_uav(device, nullptr, &view_desc, &view) == E_INVALIDARG && !view, "UAV needs actual resource");
                        view_desc.Buffer.FirstElement = 10000;
                        check(create_uav(device, buffer, &view_desc, &view) == E_INVALIDARG && !view, "UAV range cannot exceed logical buffer");
                        view_desc.Buffer.FirstElement = 2;
                        check(create_uav(device, buffer, &view_desc, nullptr) == S_FALSE && refs(buffer) == 1, "UAV validation-only preserves ownership");
                        check(create_uav(device, buffer, &view_desc, &view) == S_OK && view && refs(buffer) == 2,
                              "append UAV owns real resource and counter");
                        if (view) {
                            void* allocation = nullptr; void* view_allocation = nullptr; void* retained = nullptr;
                            std::memcpy(&allocation, static_cast<char*>(buffer) + 48, sizeof allocation);
                            std::memcpy(&view_allocation, static_cast<char*>(view) + 80, sizeof view_allocation);
                            std::memcpy(&retained, static_cast<char*>(view) + 32, sizeof retained);
                            std::array<UINT32, 8> descriptor{};
                            std::memcpy(descriptor.data(), static_cast<char*>(view) + 48, sizeof descriptor);
                            const UINT64 view_address = descriptor[0] | (UINT64(descriptor[1] & 255) << 32);
                            check(retained == buffer && allocation == view_allocation && view_address == reinterpret_cast<std::uintptr_t>(allocation) + 56 &&
                                (descriptor[1] & 0x3fff0000) == (28u << 16) && descriptor[2] == 3 && descriptor[3] == 0x24fac &&
                                !descriptor[4] && !descriptor[5] && !descriptor[6] && !descriptor[7],
                                "native UAV prefix and structured SRD retain base, offset, stride and extent");
                            using Query = HRESULT (STDMETHODCALLTYPE*)(void*, REFIID, void**);
                            void* queried = nullptr;
                            check(reinterpret_cast<Query>(vt(view)[0])(view, __uuidof(ID3D11UnorderedAccessView), &queried) == S_OK && queried == view,
                                  "native UAV IID returns original identity");
                            if (queried) reinterpret_cast<Count>(vt(queried)[2])(queried);
                            check(reinterpret_cast<Query>(vt(view)[0])(view, __uuidof(ID3D11ShaderResourceView), &queried) == E_NOINTERFACE && !queried,
                                  "UAV does not pretend to implement shader-resource-view methods");
                            using GetDesc = void (STDMETHODCALLTYPE*)(void*, D3D11_UNORDERED_ACCESS_VIEW_DESC*);
                            struct { D3D11_UNORDERED_ACCESS_VIEW_DESC desc{}; UINT32 guard = 0xabcdef; } actual;
                            reinterpret_cast<GetDesc>(vt(view)[9])(view, &actual.desc);
                            check(actual.desc.Buffer.FirstElement == 2 && actual.desc.Buffer.NumElements == 3 &&
                                actual.desc.Buffer.Flags == D3D11_BUFFER_UAV_FLAG_APPEND && actual.guard == 0xabcdef, "GetDesc preserves append range and output size");
                            using GetResource = void (STDMETHODCALLTYPE*)(void*, void**);
                            void* resource = nullptr;
                            reinterpret_cast<GetResource>(vt(view)[8])(view, &resource);
                            check(resource == buffer && refs(buffer) == 3, "UAV GetResource adds actual resource reference");
                            reinterpret_cast<Count>(vt(resource)[2])(resource);
                            check(reinterpret_cast<Count>(vt(buffer)[2])(buffer) == 1, "UAV keeps buffer alive after caller releases it");
                            buffer = nullptr;
                            check(reinterpret_cast<Count>(vt(view)[2])(view) == 0, "UAV and hidden counter retire together");
                            MEMORY_BASIC_INFORMATION memory{};
                            VirtualQuery(allocation, &memory, sizeof memory);
                            check(memory.State == MEM_FREE, "final view release frees the retained buffer allocation");
                        }
                        if (buffer) reinterpret_cast<Count>(vt(buffer)[2])(buffer);
                    }
                    desc.standard.StructureByteStride = 0;
                    check(factory(device, &desc, nullptr, &buffer) == E_INVALIDARG && !buffer, "structured buffer cannot have zero stride");
                    desc.standard = {16, D3D11_USAGE_DEFAULT, 0, 0, D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS, 0};
                    check(factory(device, &desc, &initial, &buffer) == S_OK && buffer, "indirect argument storage needs no shader binding flag");
                    if (buffer) {
                        std::array<UINT32, 4> srd{};
                        std::memcpy(srd.data(), static_cast<char*>(buffer) + 32, sizeof srd);
                        check(!(srd[1] & 0x3fff0000) && srd[2] == 16, "argument buffer header preserves byte count");
                        reinterpret_cast<Count>(vt(buffer)[2])(buffer);
                    }
                    graphics_force_header = true;
                    desc.standard = {280000, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS,
                        0, D3D11_RESOURCE_MISC_BUFFER_STRUCTURED, 28};
                    check(factory(device, &desc, nullptr, &buffer) == S_OK && buffer, "header-bearing system allocation remains a valid buffer");
                    if (buffer) {
                        void* allocation = nullptr;
                        std::memcpy(&allocation, static_cast<char*>(buffer) + 48, sizeof allocation);
                        D3D11_UNORDERED_ACCESS_VIEW_DESC view_desc{};
                        view_desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
                        view_desc.Buffer = {0, 10000, D3D11_BUFFER_UAV_FLAG_APPEND};
                        using CreateUav = HRESULT (STDMETHODCALLTYPE*)(void*, void*, const D3D11_UNORDERED_ACCESS_VIEW_DESC*, void**);
                        void* view = buffer;
                        check(reinterpret_cast<CreateUav>(vt(device)[8])(device, buffer, &view_desc, &view) == S_OK && view && refs(buffer) == 2,
                              "aligned representation accepts actual sixteen-byte offset and stride twenty-eight");
                        bool header = (reinterpret_cast<std::uintptr_t>(allocation) & 65535) == 16;
                        for (unsigned i = 0; i < 16; ++i) header = header && static_cast<unsigned char*>(allocation)[int(i) - 16] == 0x5a;
                        check(header, "aligned view preserves original allocator address and metadata");
                        if (view) reinterpret_cast<Count>(vt(view)[2])(view);
                        using CreateSrv = HRESULT (STDMETHODCALLTYPE*)(void*, void*, const D3D11_SHADER_RESOURCE_VIEW_DESC*, void**);
                        const auto create_srv = reinterpret_cast<CreateSrv>(vt(device)[7]);
                        D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{};
                        srv_desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
                        srv_desc.Buffer = {1, 9999};
                        check(create_srv(device, nullptr, &srv_desc, &view) == E_INVALIDARG && !view, "SRV requires an actual buffer");
                        srv_desc.Buffer.NumElements = 10000;
                        check(create_srv(device, buffer, &srv_desc, &view) == E_INVALIDARG && !view, "SRV range cannot exceed logical buffer");
                        srv_desc.Buffer.NumElements = 9999;
                        srv_desc.Format = DXGI_FORMAT_R32_UINT;
                        check(create_srv(device, buffer, &srv_desc, &view) == DXGI_ERROR_UNSUPPORTED && !view, "structured SRV refuses typed reinterpretation");
                        srv_desc.Format = DXGI_FORMAT_UNKNOWN;
                        check(create_srv(device, buffer, &srv_desc, nullptr) == S_FALSE && refs(buffer) == 1,
                              "SRV validation-only preserves resource ownership");
                        check(create_srv(device, buffer, &srv_desc, &view) == S_OK && view && refs(buffer) == 2,
                              "SRV and UAV share the original header-bearing logical buffer");
                        if (view) {
                            UINT type = 0;
                            std::memcpy(&type, static_cast<char*>(view) + 40, sizeof type);
                            std::array<UINT32, 8> descriptor{};
                            std::memcpy(descriptor.data(), static_cast<char*>(view) + 48, sizeof descriptor);
                            const UINT64 address = descriptor[0] | (UINT64(descriptor[1] & 255) << 32);
                            check(type == 2 && address == reinterpret_cast<std::uintptr_t>(allocation) + 28 && descriptor[2] == 9999 &&
                                descriptor[3] == 0x24fac && !descriptor[4] && !descriptor[5] && !descriptor[6] && !descriptor[7],
                                "native SRV prefix and SRD preserve unaligned base, element range and upper zero bytes");
                            using GetDesc = void (STDMETHODCALLTYPE*)(void*, D3D11_SHADER_RESOURCE_VIEW_DESC*);
                            struct { D3D11_SHADER_RESOURCE_VIEW_DESC desc{}; UINT32 guard = 0xabcdef; } actual;
                            reinterpret_cast<GetDesc>(vt(view)[9])(view, &actual.desc);
                            check(actual.desc.Buffer.FirstElement == 1 && actual.desc.Buffer.NumElements == 9999 && actual.guard == 0xabcdef,
                                  "SRV GetDesc writes its actual twenty-four-byte descriptor");
                            using QueryView = HRESULT (STDMETHODCALLTYPE*)(void*, REFIID, void**);
                            void* queried = nullptr;
                            check(reinterpret_cast<QueryView>(vt(view)[0])(view, __uuidof(ID3D11ShaderResourceView), &queried) == S_OK && queried == view,
                                  "SRV IID returns its original identity");
                            if (queried) reinterpret_cast<Count>(vt(queried)[2])(queried);
                            check(reinterpret_cast<QueryView>(vt(view)[0])(view, __uuidof(ID3D11UnorderedAccessView), &queried) == E_NOINTERFACE && !queried,
                                  "SRV does not claim UAV methods");
                            using GetResource = void (STDMETHODCALLTYPE*)(void*, void**);
                            void* retained = nullptr;
                            reinterpret_cast<GetResource>(vt(view)[8])(view, &retained);
                            check(retained == buffer && refs(buffer) == 3, "SRV GetResource retains the same buffer");
                            if (retained) reinterpret_cast<Count>(vt(retained)[2])(retained);
                            reinterpret_cast<Count>(vt(view)[2])(view);
                        }
                        check(create_srv(device, buffer, nullptr, &view) == S_OK && view, "null SRV descriptor uses the full structured range");
                        if (view) reinterpret_cast<Count>(vt(view)[2])(view);
                        reinterpret_cast<Count>(vt(buffer)[2])(buffer);
                    }
                    graphics_force_header = false;
                    set_hooks(nullptr, nullptr);
                    check(graphics_hook_allocations != 0 && graphics_hook_allocations == graphics_hook_frees && !graphics_hook_wrong_allocator,
                          "graphics storage and counters preserve system allocator ID through actual title hooks");
                }
                {
                    using CreateState = HRESULT (STDMETHODCALLTYPE*)(void*, const D3D11_RASTERIZER_DESC*, void**);
                    const auto factory = reinterpret_cast<CreateState>(vt(device)[22]);
                    D3D11_RASTERIZER_DESC desc{};
                    desc.FillMode = D3D11_FILL_WIREFRAME; desc.CullMode = D3D11_CULL_FRONT;
                    desc.FrontCounterClockwise = TRUE; desc.DepthBias = -5; desc.DepthBiasClamp = 1.5f;
                    desc.SlopeScaledDepthBias = -2.f; desc.DepthClipEnable = TRUE; desc.ScissorEnable = TRUE;
                    desc.MultisampleEnable = TRUE; desc.AntialiasedLineEnable = TRUE;
                    void* state = device;
                    check(factory(device, nullptr, &state) == E_INVALIDARG && !state, "rasterizer description required");
                    check(factory(device, &desc, nullptr) == S_FALSE, "rasterizer validation only");
                    check(factory(device, &desc, &state) == S_OK && state, "rasterizer state created");
                    if (state) {
                        using GetDesc = void (STDMETHODCALLTYPE*)(void*, D3D11_RASTERIZER_DESC*);
                        D3D11_RASTERIZER_DESC actual{};
                        reinterpret_cast<GetDesc>(vt(state)[8])(state, &actual);
                        check(actual.FillMode == desc.FillMode && actual.CullMode == desc.CullMode && actual.FrontCounterClockwise &&
                            actual.DepthBias == -5 && actual.DepthBiasClamp == 1.5f && actual.SlopeScaledDepthBias == -2.f &&
                            actual.ScissorEnable && actual.DepthClipEnable && actual.MultisampleEnable && actual.AntialiasedLineEnable,
                            "rasterizer preserves culling, winding, bias, scissor, clipping and line/MSAA modes");
                        using SetRaster=void(STDMETHODCALLTYPE*)(void*,void*);
                        const auto set_raster=reinterpret_cast<SetRaster>(vt(context)[43]);
                        set_raster(context,state);
                        check(refs(state)==2,"legacy raster slot 43 retains its native state");
                        set_raster(context,state);
                        check(refs(state)==2,"rebinding a rasterizer preserves one context owner");
                        set_raster(context,nullptr);
                        check(refs(state)==1,"legacy raster reset releases context ownership");
                        set_raster(context,state);
                        reinterpret_cast<void(STDMETHODCALLTYPE*)(void*)>(vt(context)[110])(context);
                        check(refs(state)==1,"ClearState releases a retained rasterizer without retiring caller ownership");
                        reinterpret_cast<void(STDMETHODCALLTYPE*)(void*)>(vt(context)[110])(context);
                        check(refs(state)==1,"repeated ClearState cannot release a rasterizer twice");
                        void* duplicate = nullptr;
                        check(factory(device, &desc, &duplicate) == S_OK && duplicate == state && refs(state) == 2,
                              "identical rasterizer shares live state");
                        if (duplicate) reinterpret_cast<Count>(vt(duplicate)[2])(duplicate);
                        check(reinterpret_cast<Count>(vt(state)[2])(state) == 0, "rasterizer cache entry retires");
                    }
                    desc.FillMode = static_cast<D3D11_FILL_MODE>(4);
                    check(factory(device, &desc, &state) == E_INVALIDARG && !state, "unsupported fill mode rejected");
                }
                {
                    using CreateState = HRESULT (STDMETHODCALLTYPE*)(void*, const D3D11_BLEND_DESC*, void**);
                    const auto factory = reinterpret_cast<CreateState>(vt(device)[20]);
                    D3D11_BLEND_DESC desc{};
                    desc.IndependentBlendEnable = TRUE; desc.AlphaToCoverageEnable = TRUE;
                    for (UINT i = 0; i < 8; ++i) {
                        auto& r = desc.RenderTarget[i];
                        r.BlendEnable = TRUE; r.SrcBlend = D3D11_BLEND_SRC_ALPHA; r.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
                        r.BlendOp = D3D11_BLEND_OP_ADD; r.SrcBlendAlpha = D3D11_BLEND_ONE; r.DestBlendAlpha = D3D11_BLEND_ZERO;
                        r.BlendOpAlpha = D3D11_BLEND_OP_REV_SUBTRACT; r.RenderTargetWriteMask = static_cast<UINT8>(i + 1);
                    }
                    void* state = device;
                    check(factory(device, nullptr, &state) == E_INVALIDARG && !state, "blend description required");
                    check(factory(device, &desc, nullptr) == S_FALSE, "blend validation only");
                    check(factory(device, &desc, &state) == S_OK && state, "independent blend state created");
                    if (state) {
                        using GetDesc = void (STDMETHODCALLTYPE*)(void*, D3D11_BLEND_DESC*);
                        D3D11_BLEND_DESC actual{};
                        reinterpret_cast<GetDesc>(vt(state)[8])(state, &actual);
                        bool all = actual.IndependentBlendEnable && actual.AlphaToCoverageEnable;
                        for (UINT i = 0; i < 8; ++i) all = all && actual.RenderTarget[i].RenderTargetWriteMask == i + 1 &&
                            actual.RenderTarget[i].DestBlend == D3D11_BLEND_INV_SRC_ALPHA && actual.RenderTarget[i].BlendOpAlpha == D3D11_BLEND_OP_REV_SUBTRACT;
                        check(all, "all blend targets retain color, alpha and write mask");
                        using SetBlend=void(STDMETHODCALLTYPE*)(void*,void*,const FLOAT*,UINT);
                        const auto set_blend=reinterpret_cast<SetBlend>(vt(context)[35]);
                        const FLOAT factor[]{.125f,.25f,.5f,1.f};
                        set_blend(context,state,factor,0x55555555);
                        check(refs(state)==2,"legacy slot 35 retains its real blend state");
                        set_blend(context,state,nullptr,UINT_MAX);
                        check(refs(state)==2,"rebinding the same legacy blend preserves exactly one context owner");
                        set_blend(context,nullptr,nullptr,UINT_MAX);
                        check(refs(state)==1,"legacy blend unbind retires context ownership");
                        set_blend(context,state,factor,0x55555555);
                        reinterpret_cast<void(STDMETHODCALLTYPE*)(void*)>(vt(context)[110])(context);
                        check(refs(state)==1,"ClearState releases blend ownership and permits later state reuse");
                        void* duplicate = nullptr;
                        check(factory(device, &desc, &duplicate) == S_OK && duplicate == state && refs(state) == 2,
                              "identical live blend state shares identity");
                        if (duplicate) reinterpret_cast<Count>(vt(duplicate)[2])(duplicate);
                        check(reinterpret_cast<Count>(vt(state)[2])(state) == 0, "blend cache entry retires");
                    }
                    desc.RenderTarget[7].SrcBlendAlpha = D3D11_BLEND_SRC_COLOR;
                    check(factory(device, &desc, &state) == E_INVALIDARG && !state, "color factor in alpha blend rejected");
                    desc.RenderTarget[7].SrcBlendAlpha = D3D11_BLEND_ONE; desc.RenderTarget[7].RenderTargetWriteMask = 0x80;
                    check(factory(device, &desc, &state) == E_INVALIDARG && !state, "invalid blend write mask rejected");
                }
                {
                    using CreateSampler = HRESULT (STDMETHODCALLTYPE*)(void*, const D3D12_SAMPLER_DESC*, void**);
                    const auto factory = reinterpret_cast<CreateSampler>(vt(device)[23]);
                    D3D12_SAMPLER_DESC desc{};
                    desc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
                    desc.AddressU = D3D12_TEXTURE_ADDRESS_MODE_WRAP; desc.AddressV = D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
                    desc.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP; desc.MaxAnisotropy = 16;
                    desc.ComparisonFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
                    desc.MipLODBias = -1.125f; desc.MinLOD = .125f; desc.MaxLOD = D3D12_FLOAT32_MAX;
                    void* sampler = device;
                    check(factory(device, nullptr, &sampler) == E_INVALIDARG && !sampler, "sampler description required");
                    check(factory(device, &desc, nullptr) == S_FALSE, "sampler validation only");
                    check(factory(device, &desc, &sampler) == S_OK && sampler, "native sampler descriptor created");
                    if (sampler) {
                        check((reinterpret_cast<std::uintptr_t>(sampler) & 15) == 0 && refs(sampler) == 1 && refs(device) == 1,
                              "sampler old prefix, alignment and internal ownership");
                        using GetX = void (STDMETHODCALLTYPE*)(void*, UINT32*);
                        std::array<UINT32, 5> packed{0, 0, 0, 0, 0x12345678};
                        reinterpret_cast<GetX>(vt(sampler)[9])(sampler, packed.data());
                        check(packed[0] == 0x3088 && packed[1] == 0xfff020 && packed[2] == 0x74003ee0 && (packed[3]&0x3fffffff) && !(packed[3]>>30) && packed[4] == 0x12345678,
                              "sampler GCN words encode address, negative bias and fixed LOD; GetDescX writes exactly sixteen bytes");
                        check(!std::memcmp(static_cast<char*>(sampler) + 32, packed.data(), 12), "issued sampler keeps the native inline SRD words");
                        using GetDesc = void (STDMETHODCALLTYPE*)(void*, D3D12_SAMPLER_DESC*);
                        D3D12_SAMPLER_DESC actual{};
                        reinterpret_cast<GetDesc>(vt(sampler)[8])(sampler, &actual);
                        check(actual.MaxLOD == desc.MaxLOD && actual.MaxAnisotropy == 16 && actual.MipLODBias == -1.125f,
                              "GetDesc preserves request before hardware quantization");
                        void* duplicate = nullptr;
                        check(factory(device, &desc, &duplicate) == S_OK && duplicate == sampler && refs(sampler) == 2,
                              "identical sampler shares live state and descriptor slot");
                        if (duplicate) reinterpret_cast<Count>(vt(duplicate)[2])(duplicate);
                        using FastResources=void(STDMETHODCALLTYPE*)(void*,const void*,const void*);
                        const auto fast=reinterpret_cast<FastResources>(vt(context)[209]);
                        std::array<UINT32,9> batch{0xa0040e02};
                        const auto object=reinterpret_cast<std::uintptr_t>(sampler);
                        std::memcpy(batch.data()+1,&object,sizeof object);std::memcpy(batch.data()+5,&object,sizeof object);
                        fast(context,batch.data(),batch.data()+batch.size());
                        check(refs(sampler)==3,"fast PS sampler packet retains both final slots independently");
                        fast(context,batch.data(),batch.data()+batch.size());
                        check(refs(sampler)==3,"fast sampler packet rebind balances ownership");
                        using BindSamplers=void(STDMETHODCALLTYPE*)(void*,UINT,UINT,void* const*);
                        const auto bind_samplers=reinterpret_cast<BindSamplers>(vt(context)[11]);
                        void* sampler_array[]{sampler};
                        bind_samplers(context,0,1,sampler_array);bind_samplers(context,0,1,sampler_array);
                        check(refs(sampler)==4,"legacy standard PS sampler binding shares fast state with balanced ownership");
                        bind_samplers(context,0,1,nullptr);bind_samplers(context,16,0,nullptr);
                        check(refs(sampler)==3,"legacy standard PS sampler unbind preserves the two fast-bound slots");
                        const auto bind_geometry=reinterpret_cast<BindSamplers>(vt(context)[32]);
                        bind_geometry(context,15,1,sampler_array);bind_geometry(context,15,1,sampler_array);
                        bind_geometry(context,16,0,nullptr);
                        check(refs(sampler)==4,"legacy GS sampler last-slot binding retains one independent reference across rebinds");
                        reinterpret_cast<void(STDMETHODCALLTYPE*)(void*)>(vt(context)[110])(context);
                        check(refs(sampler)==1,"ClearState releases fast sampler bindings without changing the descriptor");
                        check(reinterpret_cast<Count>(vt(sampler)[2])(sampler) == 0, "sampler descriptor slot retired");
                    }
                    desc.Filter = D3D12_FILTER_MIN_LINEAR_MAG_MIP_POINT;
                    check(factory(device, &desc, &sampler) == S_OK && sampler, "asymmetric sampler filters supported");
                    if (sampler) {
                        UINT32 word = 0;
                        std::memcpy(&word, static_cast<char*>(sampler) + 40, 4);
                        check((word & 0xf00000) == 0x400000, "min and mag filter fields are not interchanged");
                        reinterpret_cast<Count>(vt(sampler)[2])(sampler);
                    }
                    desc.Filter = static_cast<D3D12_FILTER>(3);
                    check(factory(device, &desc, &sampler) == E_INVALIDARG && !sampler, "invalid sampler filter rejected");
                    desc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
                    for (UINT border = 1; border <= 2; ++border) {
                        for (UINT channel = 0; channel < 4; ++channel) desc.BorderColor[channel] = channel == 3 || border == 2 ? 1.f : 0.f;
                        check(factory(device,&desc,&sampler) == S_OK && sampler,"standard opaque sampler borders are represented exactly");
                        if (sampler) {
                            std::array<UINT32,4> words{};
                            using GetX = void(STDMETHODCALLTYPE*)(void*,UINT32*);
                            reinterpret_cast<GetX>(vt(sampler)[9])(sampler,words.data());
                            check((words[3]>>30)==border && (words[3]&0x3fffffff),"issued sampler descriptor retains the standard border color type and palette index");
                            using GetDesc = void(STDMETHODCALLTYPE*)(void*,D3D12_SAMPLER_DESC*);
                            D3D12_SAMPLER_DESC actual{};
                            reinterpret_cast<GetDesc>(vt(sampler)[8])(sampler,&actual);
                            check(!std::memcmp(actual.BorderColor,desc.BorderColor,sizeof desc.BorderColor),"sampler GetDesc preserves all border channels");
                            reinterpret_cast<Count>(vt(sampler)[2])(sampler);
                        }
                    }
                    {
                        using GetX=void(STDMETHODCALLTYPE*)(void*,UINT32*);
                        using GetDesc=void(STDMETHODCALLTYPE*)(void*,D3D12_SAMPLER_DESC*);
                        using CreateX=HRESULT(STDMETHODCALLTYPE*)(void*,const UINT32*,void**);
                        const auto create_x=reinterpret_cast<CreateX>(vt(device)[67]);
                        std::array<UINT32,4> issued{},returned{};
                        check(factory(device,&desc,&sampler)==S_OK && sampler,"sampler issued-descriptor producer created");
                        if(!sampler)return 1;
                        reinterpret_cast<GetX>(vt(sampler)[9])(sampler,issued.data());
                        reinterpret_cast<Count>(vt(sampler)[2])(sampler);sampler=nullptr;
                        check(create_x(device,nullptr,&sampler)==E_INVALIDARG && !sampler,"CreateSamplerStateX requires an input descriptor");
                        check(create_x(device,issued.data(),nullptr)==S_FALSE,"issued sampler validation shares the ordinary creation path");
                        for(UINT border=0;border<4;++border) {
                            auto changed=issued;changed[3]=(changed[3]&0x3fffffffu)|(border<<30);
                            check(create_x(device,changed.data(),&sampler)==S_OK && sampler,"issued descriptor survives release and permits each border-color type");
                            if(!sampler)return 1;
                            reinterpret_cast<GetX>(vt(sampler)[9])(sampler,returned.data());
                            D3D12_SAMPLER_DESC actual{};reinterpret_cast<GetDesc>(vt(sampler)[8])(sampler,&actual);
                            auto expected=desc;
                            if(border<3)for(UINT channel=0;channel<4;++channel)expected.BorderColor[channel]=border==2 || (border==1 && channel==3) ? 1.f:0.f;
                            check(returned==changed && !std::memcmp(&actual,&expected,sizeof actual),"CreateSamplerStateX preserves issued words and changes only the requested border type");
                            void* duplicate=nullptr;
                            check(create_x(device,changed.data(),&duplicate)==S_OK && duplicate==sampler && refs(sampler)==2,"extended sampler interning retains exact descriptor identity");
                            if(duplicate)reinterpret_cast<Count>(vt(duplicate)[2])(duplicate);
                            reinterpret_cast<Count>(vt(sampler)[2])(sampler);sampler=nullptr;
                        }
                        static UINT rejected_sampler_slot=0;
                        set_d3d11x_trap_handler([](const char*,UINT slot)->std::uintptr_t{rejected_sampler_slot=slot;return 0x80070057u;});
                        auto invalid=issued;invalid[0]^=1;
                        check(create_x(device,invalid.data(),&sampler)==E_INVALIDARG && !sampler && rejected_sampler_slot==67,"altering issued sampler fields reaches the explicit unsupported boundary");
                        rejected_sampler_slot=0;invalid=issued;invalid[3]&=0xc0000000u;
                        check(create_x(device,invalid.data(),&sampler)==E_INVALIDARG && !sampler && rejected_sampler_slot==67,"unissued raw sampler descriptors are not accepted as issued tokens");
                        set_d3d11x_trap_handler(nullptr);
                    }
                    for (float& channel : desc.BorderColor) channel = 0;
                    desc.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT; desc.BorderColor[0] = 1;
                    check(factory(device, &desc, &sampler) == DXGI_ERROR_UNSUPPORTED && !sampler, "custom border table remains explicit gap");
                }
                {
                    using CreateState = HRESULT (STDMETHODCALLTYPE*)(void*, const D3D12_DEPTH_STENCIL_DESC*, void**);
                    const auto factory = reinterpret_cast<CreateState>(vt(device)[21]);
                    D3D12_DEPTH_STENCIL_DESC desc{};
                    desc.DepthEnable = TRUE; desc.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
                    desc.DepthFunc = D3D12_COMPARISON_FUNC_LESS_EQUAL;
                    desc.StencilReadMask = 0x7f; desc.StencilWriteMask = 0xf0; desc.StencilEnable = TRUE;
                    desc.FrontFace = {D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_INCR, D3D12_STENCIL_OP_REPLACE, D3D12_COMPARISON_FUNC_EQUAL};
                    desc.BackFace = {D3D12_STENCIL_OP_ZERO, D3D12_STENCIL_OP_KEEP, D3D12_STENCIL_OP_DECR, D3D12_COMPARISON_FUNC_NOT_EQUAL};
                    void* state = device;
                    check(factory(device, nullptr, &state) == E_INVALIDARG && !state, "depth description required");
                    check(factory(device, &desc, nullptr) == S_FALSE, "depth state validation only");
                    check(factory(device, &desc, &state) == S_OK && state, "depth stencil state created");
                    if (state) {
                        void* duplicate = nullptr;
                        check(factory(device, &desc, &duplicate) == S_OK && duplicate == state && refs(state) == 2,
                              "identical live depth states share identity");
                        using GetDesc = void (STDMETHODCALLTYPE*)(void*, D3D12_DEPTH_STENCIL_DESC*);
                        D3D12_DEPTH_STENCIL_DESC actual{};
                        reinterpret_cast<GetDesc>(vt(state)[8])(state, &actual);
                        check(actual.DepthEnable && actual.StencilEnable && actual.DepthFunc == desc.DepthFunc &&
                            actual.FrontFace.StencilPassOp == desc.FrontFace.StencilPassOp && actual.BackFace.StencilFunc == desc.BackFace.StencilFunc &&
                            actual.StencilReadMask == 0x7f && actual.StencilWriteMask == 0xf0, "depth and both stencil faces preserved");
                        if (duplicate) reinterpret_cast<Count>(vt(duplicate)[2])(duplicate);
                        using SetDepth=void(STDMETHODCALLTYPE*)(void*,void*,UINT);
                        const auto set_depth=reinterpret_cast<SetDepth>(vt(context)[36]);
                        set_depth(context,state,0x7a);set_depth(context,state,0x5c);
                        check(refs(state)==2,"legacy depth binding retains exactly one state reference across stencil-reference changes");
                        set_depth(context,nullptr,0);
                        check(refs(state)==1,"legacy depth unbinding releases the context state owner");
                        check(reinterpret_cast<Count>(vt(state)[2])(state) == 0, "depth state cache entry retires with final reference");
                    }
                    desc.BackFace.StencilFunc = static_cast<D3D12_COMPARISON_FUNC>(9);
                    check(factory(device, &desc, &state) == E_INVALIDARG && !state, "invalid stencil comparison rejected");
                }
                {
                    const HMODULE compiler = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
                    const auto compile = compiler ? reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
                    check(compile != nullptr, "native compiler for owned shader fixtures");
                    if (compile) {
                        using CreateShader = HRESULT (STDMETHODCALLTYPE*)(void*, const void*, SIZE_T, void*, void**);
                        const char* source[]{"float4 main(float4 p:POSITION):SV_Position { return p; }",
                            "float4 main():SV_Target { return float4(1,0,0,1); }", "[numthreads(1,1,1)] void main() {}",
                            "struct V{float4 p:SV_Position;}; [maxvertexcount(3)] void main(triangle V input[3],inout TriangleStream<V> output)"
                            "{output.Append(input[0]);output.Append(input[1]);output.Append(input[2]);}"};
                        const char* profile[]{"vs_5_0", "ps_5_0", "cs_5_0", "gs_5_0"};
                        const unsigned slots[]{12, 15, 18, 13};
                        for (unsigned i = 0; i != 4; ++i) {
                            ID3DBlob* code = nullptr;
                            check(SUCCEEDED(compile(source[i], std::strlen(source[i]), nullptr, nullptr, nullptr,
                                "main", profile[i], 0, 0, &code, nullptr)) && code, "compile owned shader fixture");
                            if (!code) continue;
                            if (i == 0) {
                                using CreateLayout = HRESULT (STDMETHODCALLTYPE*)(void*, const D3D12_INPUT_ELEMENT_DESC*, UINT, const void*, SIZE_T, void**);
                                const auto create_layout = reinterpret_cast<CreateLayout>(vt(device)[11]);
                                char semantic[] = "position";
                                D3D12_INPUT_ELEMENT_DESC element{semantic, 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, 0,
                                    D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
                                void* layout = device;
                                check(create_layout(device, &element, 1, code->GetBufferPointer(), code->GetBufferSize(), nullptr) == S_FALSE,
                                      "matching input signature validates without object");
                                element.SemanticIndex = 1;
                                check(FAILED(create_layout(device, &element, 1, code->GetBufferPointer(), code->GetBufferSize(), &layout)) && !layout,
                                      "missing shader semantic rejected");
                                element.SemanticIndex = 0; element.Format = DXGI_FORMAT_BC1_UNORM;
                                check(FAILED(create_layout(device, &element, 1, code->GetBufferPointer(), code->GetBufferSize(), &layout)) && !layout,
                                      "non-vertex input format rejected by device support");
                                element.Format = DXGI_FORMAT_R32G32B32A32_FLOAT; element.InstanceDataStepRate = 1;
                                check(FAILED(create_layout(device, &element, 1, code->GetBufferPointer(), code->GetBufferSize(), &layout)) && !layout,
                                      "per-vertex element cannot have instance rate");
                                element.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA;
                                check(create_layout(device, &element, 1, code->GetBufferPointer(), code->GetBufferSize(), &layout) == S_OK && layout,
                                      "instanced input layout created from real signature");
                                if (layout) {
                                    check(refs(layout) == 1 && refs(device) == 1, "layout owns internal device dependency");
                                    semantic[0] = 'X'; // Creation must own names after the caller returns.
                                    check(reinterpret_cast<Count>(vt(layout)[2])(layout) == 0, "input layout released");
                                }
                            }
                            const auto factory = reinterpret_cast<CreateShader>(vt(device)[slots[i]]);
                            void* shader = device;
                            check(FAILED(factory(device, code->GetBufferPointer(), 16, nullptr, &shader)) && !shader,
                                  "truncated shader rejected and output cleared");
                            check(factory(device, code->GetBufferPointer(), code->GetBufferSize(), device, &shader) == DXGI_ERROR_UNSUPPORTED && !shader,
                                  "unimplemented dynamic linkage rejected");
                            check(FAILED(reinterpret_cast<CreateShader>(vt(device)[slots[(i + 1) % 4]])(device,
                                code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader)) && !shader, "wrong shader stage rejected");
                            check(factory(device, code->GetBufferPointer(), code->GetBufferSize(), nullptr, nullptr) == S_FALSE,
                                  "shader validation-only does not create an object");
                            check(factory(device, code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader) == S_OK && shader,
                                  "validated DXBC shader module created");
                            if (shader) {
                                check(refs(shader) == 1 && refs(device) == 1, "shader owns internal device dependency");
                                if (i == 3) {
                                    UINT native_flags = 1;
                                    std::memcpy(&native_flags, static_cast<char*>(shader) + 32, sizeof native_flags);
                                    check(native_flags == 0, "original geometry shader public flag at plus thirty-two stays zero");
                                    using QueryShader = HRESULT (STDMETHODCALLTYPE*)(void*, REFIID, void**);
                                    void* queried = nullptr;
                                    check(reinterpret_cast<QueryShader>(vt(shader)[0])(shader, __uuidof(ID3D11GeometryShader), &queried) == S_OK && queried == shader,
                                          "geometry shader IID preserves original identity");
                                    if (queried) reinterpret_cast<Count>(vt(queried)[2])(queried);
                                }
                                using Get = void (STDMETHODCALLTYPE*)(void*, void**);
                                void* owner = nullptr;
                                reinterpret_cast<Get>(vt(shader)[3])(shader, &owner);
                                check(owner == device && refs(device) == 2, "shader GetDevice retains original identity");
                                reinterpret_cast<Count>(vt(owner)[2])(owner);
                                check(reinterpret_cast<Count>(vt(shader)[2])(shader) == 0, "shader module released");
                            }
                            code->Release();
                        }
                    }
                    if (compiler) FreeLibrary(compiler);
                }
                constexpr GUID performance{0x88671610, 0x712E, 0x4F1E, {0x84, 0xAB, 0x01, 0xB5, 0x94, 0x8B, 0xD3, 0x73}};
                using PerformanceQuery = HRESULT (STDMETHODCALLTYPE*)(void*, REFIID, void**);
                void* performance_device = nullptr;
                check(reinterpret_cast<PerformanceQuery>(vt(device)[0])(device, performance, &performance_device) == S_OK &&
                      performance_device == device && refs(device) == 2, "legacy performance device shares identity and ownership");
                if (performance_device) reinterpret_cast<Count>(vt(performance_device)[2])(performance_device);
                using GetDevice = void (STDMETHODCALLTYPE*)(void*, void**);
                void* context_device = nullptr;
                reinterpret_cast<GetDevice>(vt(context)[3])(context, &context_device);
                check(context_device == device && refs(device) == 2, "context GetDevice returns original identity with reference");
                reinterpret_cast<Count>(vt(context_device)[2])(context_device);
                check(reinterpret_cast<Flags>(vt(device)[39])(device) == p.flags, "legacy creation flags preserved");
                void* immediate = nullptr;
                reinterpret_cast<GetDevice>(vt(device)[41])(device, &immediate);
                check(immediate == context && refs(context) == 2, "immediate context identity and reference");
                reinterpret_cast<Count>(vt(context)[2])(context);
                check(reinterpret_cast<Count>(vt(immediate)[2])(immediate) == 0 && refs(device) == 1,
                      "zero context count preserves device-owned context storage");
                reinterpret_cast<GetDevice>(vt(device)[55])(device, &immediate);
                check(immediate == context && refs(context) == 1, "context identity survives release and reacquisition");
                constexpr GUID dxgi1{0x77DB970F, 0x6276, 0x48BA, {0xBA, 0x28, 0x07, 0x01, 0x43, 0xB4, 0x39, 0x2C}};
                constexpr GUID factory2{0x50C83A1C, 0xE072, 0x4C48, {0x87, 0xB0, 0x36, 0x30, 0xFA, 0x36, 0xA6, 0xD0}};
                using Query = HRESULT (STDMETHODCALLTYPE*)(void*, REFIID, void**);
                using Adapter = HRESULT (STDMETHODCALLTYPE*)(void*, void**);
                void* dxgi = nullptr;
                void* adapter = nullptr;
                void* factory = nullptr;
                check(reinterpret_cast<Query>(vt(device)[0])(device, dxgi1, &dxgi) == S_OK && dxgi,
                      "legacy DXGI query");
                if (dxgi) {
                    check(reinterpret_cast<Adapter>(vt(dxgi)[8])(dxgi, &adapter) == S_OK && adapter,
                          "legacy GetAdapter is slot eight");
                    if (adapter) {
                        using Outputs = HRESULT (STDMETHODCALLTYPE*)(void*, UINT, IDXGIOutput**);
                        const auto outputs = reinterpret_cast<Outputs>(vt(adapter)[8]);
                        IDXGIOutput* output = nullptr;
                        check(outputs(adapter, 0, nullptr) == E_POINTER, "legacy output pointer required");
                        check(outputs(adapter, UINT_MAX, &output) == DXGI_ERROR_NOT_FOUND && !output,
                              "missing output preserves native not-found error");
                        const HRESULT enumerated = outputs(adapter, 0, &output);
                        check(enumerated == DXGI_ERROR_NOT_FOUND || (enumerated == S_OK && output),
                              "legacy output enumeration reflects actual attached outputs");
                        if (output) {
                            DXGI_OUTPUT_DESC desc{};
                            check(output->GetDesc(&desc) == S_OK && desc.DeviceName[0], "native output COM contract");
                            output->Release();
                        }
                        check(reinterpret_cast<Query>(vt(adapter)[7])(adapter, factory2, &factory) == S_OK && factory,
                              "legacy GetParent is slot seven");
                        if (factory) {
                            using CreateChain = HRESULT (STDMETHODCALLTYPE*)(void*, void*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, void*, void**);
                            const auto create_chain = reinterpret_cast<CreateChain>(vt(factory)[17]);
                            void* chain = reinterpret_cast<void*>(1);
                            check(create_chain(factory, device, nullptr, nullptr, nullptr, &chain) == DXGI_ERROR_INVALID_CALL,
                                  "legacy factory slot seventeen validates the native swapchain ABI");
                            DXGI_SWAP_CHAIN_DESC1 desc{};
                            desc.Flags = 0x100000;
                            check(create_chain(factory, device, nullptr, &desc, nullptr, &chain) == DXGI_ERROR_UNSUPPORTED && !chain,
                                  "unimplemented Xbox display flags cannot be silently removed from the desktop descriptor");
                            desc.Flags = 0x200000;
                            check(create_chain(factory, device, nullptr, &desc, nullptr, &chain) == DXGI_ERROR_INVALID_CALL && !chain,
                                  "translated display profile still requires a real owned CoreWindow");
                            reinterpret_cast<Count>(vt(factory)[2])(factory);
                        }
                        reinterpret_cast<Count>(vt(adapter)[2])(adapter);
                    }
                    reinterpret_cast<Count>(vt(dxgi)[2])(dxgi);
                }
                struct DmaDesc { UINT flags, ring, segment; } desc{1, 65536, 0};
                using CreateDma = HRESULT (STDMETHODCALLTYPE*)(void*, const DmaDesc*, void**);
                using Fence = UINT64 (STDMETHODCALLTYPE*)(void*, UINT);
                using Wait = void (STDMETHODCALLTYPE*)(void*, UINT, UINT64);
                using Pending = BOOL (STDMETHODCALLTYPE*)(void*, UINT64);
                using Submit = HRESULT (STDMETHODCALLTYPE*)(void*);
                const auto create_dma = reinterpret_cast<CreateDma>(vt(device)[59]);
                const auto pending = reinterpret_cast<Pending>(vt(device)[60]);
                void* producer = nullptr;
                void* consumer = nullptr;
                check(create_dma(device, nullptr, &producer) == E_INVALIDARG && !producer, "DMA description required");
                desc.flags = 3;
                check(create_dma(device, &desc, &producer) == DXGI_ERROR_UNSUPPORTED && !producer, "unknown DMA selector rejected");
                desc.flags = 1;
                check(create_dma(device, &desc, &producer) == S_OK && producer, "first native DMA copy queue");
                void* duplicate = nullptr;
                check(create_dma(device, &desc, &duplicate) == DXGI_ERROR_INVALID_CALL && !duplicate,
                      "duplicate live engine binding rejected");
                desc.flags = 2;
                check(create_dma(device, &desc, &consumer) == S_OK && consumer, "second native DMA copy queue");
                if (producer && consumer) {
                    using Type = UINT (STDMETHODCALLTYPE*)(void*);
                    check(reinterpret_cast<Type>(vt(producer)[8])(producer) == 2, "original DMA context type");
                    const UINT64 produced = reinterpret_cast<Fence>(vt(producer)[16])(producer, 1);
                    check(produced && pending(device, produced), "deferred DMA fence remains pending before Submit");
                    reinterpret_cast<Wait>(vt(consumer)[17])(consumer, 0, produced);
                    const UINT64 consumed = reinterpret_cast<Fence>(vt(consumer)[16])(consumer, 0);
                    check(consumed != produced && pending(device, consumed), "consumer cannot pass an unsubmitted producer fence");
                    check(reinterpret_cast<Submit>(vt(producer)[18])(producer) == S_OK, "producer submits real queue signals");
                    const auto deadline = GetTickCount64() + 3000;
                    while ((pending(device, produced) || pending(device, consumed)) && GetTickCount64() < deadline) Sleep(1);
                    check(!pending(device, produced) && !pending(device, consumed), "separate DMA queues preserve cross-fence order");
                    check(refs(device) == 1, "DMA ownership preserves public device count");
                }
                if (consumer) reinterpret_cast<Count>(vt(consumer)[2])(consumer);
                if (producer) reinterpret_cast<Count>(vt(producer)[2])(producer);
                producer = nullptr;
                desc.flags = 1;
                check(create_dma(device, &desc, &producer) == S_OK && producer, "completed engine can be rebound");
                if (producer) {
                    const UINT64 final = reinterpret_cast<Fence>(vt(producer)[16])(producer, 1);
                    reinterpret_cast<Count>(vt(producer)[2])(producer);
                    check(!pending(device, final), "last DMA release drains translated fence work");
                }
                using CreateDeferred = HRESULT (STDMETHODCALLTYPE*)(void*, UINT, void**);
                using SetViewports=void(STDMETHODCALLTYPE*)(void*,UINT,const D3D11_VIEWPORT*);
                using GetViewports=void(STDMETHODCALLTYPE*)(void*,UINT*,D3D11_VIEWPORT*);
                const auto set_viewports=reinterpret_cast<SetViewports>(vt(context)[44]);
                const auto get_viewports=reinterpret_cast<GetViewports>(vt(context)[95]);
                const D3D11_VIEWPORT viewport_values[]{{2,3,32,16,0.2f,0.8f},{4,5,64,32,0,1}};
                set_viewports(context,2,viewport_values);
                struct { D3D11_VIEWPORT value{};UINT guard=0x12345678; } viewport_result;
                UINT viewport_capacity=1;get_viewports(context,&viewport_capacity,&viewport_result.value);
                check(viewport_capacity==1 && !std::memcmp(&viewport_result.value,viewport_values,sizeof(D3D11_VIEWPORT)) && viewport_result.guard==0x12345678,
                      "legacy viewport query respects capacity, origin, extent, depth and its output guard");
                viewport_capacity=0;get_viewports(context,&viewport_capacity,nullptr);
                check(viewport_capacity==2,"legacy viewport count-only query returns all active viewports");
                set_viewports(context,0,nullptr);viewport_capacity=1;get_viewports(context,&viewport_capacity,&viewport_result.value);
                const D3D11_VIEWPORT zero_viewport{};
                check(viewport_capacity==1 && !std::memcmp(&viewport_result.value,&zero_viewport,sizeof zero_viewport),
                      "original zero-count viewport setter substitutes one all-zero viewport");
                const auto create_deferred = reinterpret_cast<CreateDeferred>(vt(device)[27]);
                void* deferred = nullptr;
                check(create_deferred(device, 0, nullptr) == E_POINTER, "deferred context output required");
                check(create_deferred(device, 0x80000, &deferred) == DXGI_ERROR_UNSUPPORTED && !deferred,
                      "unimplemented deferred context flags rejected");
                for (const UINT flags:{0u,0x60000u,0xa0000u}) {
                    check(create_deferred(device, flags, &deferred) == S_OK && deferred && deferred != context,
                          "deferred context has independent identity");
                    if (deferred) {
                        reinterpret_cast<SetViewports>(vt(deferred)[44])(deferred,2,viewport_values);
                        viewport_capacity=1;reinterpret_cast<GetViewports>(vt(deferred)[95])(deferred,&viewport_capacity,&viewport_result.value);
                        check(flags&0x80000 ? (!viewport_capacity && !std::memcmp(&viewport_result.value,&zero_viewport,sizeof zero_viewport)) :
                            (viewport_capacity==1 && !std::memcmp(&viewport_result.value,&viewport_values[0],sizeof zero_viewport)),
                              "only FAST_SEMANTICS suppresses deferred viewport queries; tracked contexts preserve values and capacity");
                        using State = UINT (STDMETHODCALLTYPE*)(void*);
                        const auto data = static_cast<unsigned char*>(deferred);
                        check((reinterpret_cast<std::uintptr_t>(deferred) & 63) == 0 &&
                              reinterpret_cast<const void*>(vt(deferred)) == data + 0x3d0 && data[0x390] == 3,
                              "old context contains its inline function table and both reference out-of-line dispatch flags");
                        check(reinterpret_cast<State>(vt(deferred)[112])(deferred) == 1 &&
                              reinterpret_cast<State>(vt(deferred)[113])(deferred) == flags,
                              "deferred type and creation flags match the requested contract");
                        UINT64 domain = 0x1234, shader_flags = 0x100;
                        UINT32 dirty = 0x80;
                        std::memcpy(data + 0x1c8, &domain, sizeof domain);
                        std::memcpy(data + 0x168, &dirty, sizeof dirty);
                        std::memcpy(data + 0x218, &shader_flags, sizeof shader_flags);
                        using Shader = void (STDMETHODCALLTYPE*)(void*, void*);
                        reinterpret_cast<Shader>(vt(deferred)[64])(deferred, nullptr);
                        std::memcpy(&domain,data+0x1c8,8);std::memcpy(&dirty,data+0x168,4);std::memcpy(&shader_flags,data+0x218,8);
                        check(domain==0x1234 && dirty==0x80 && shader_flags==0x100,
                              "reference out-of-line null stage leaves caller inline caches and dirty bits untouched");
                        data[0x390]=1;
                        reinterpret_cast<Shader>(vt(deferred)[64])(deferred, nullptr);
                        std::memcpy(&domain, data + 0x1c8, sizeof domain);
                        std::memcpy(&dirty, data + 0x168, sizeof dirty);
                        std::memcpy(&shader_flags, data + 0x218, sizeof shader_flags);
                        check(!domain && dirty == 0x2480 && shader_flags == 0x104,
                              "domain unbind preserves unrelated public state and marks the native dirty bits");
                        UINT64 hull = 0x5678;
                        std::memcpy(data + 0x1c0, &hull, sizeof hull);
                        reinterpret_cast<Shader>(vt(deferred)[60])(deferred, nullptr);
                        std::memcpy(&hull, data + 0x1c0, sizeof hull);
                        std::memcpy(&dirty, data + 0x168, sizeof dirty);
                        std::memcpy(&shader_flags, data + 0x218, sizeof shader_flags);
                        check(!hull && dirty == 0x2680 && shader_flags == 0x106,
                              "hull unbind preserves domain state and native dirty bits");
                        UINT64 geometry[2]{0x1234, 0x5678};
                        std::memcpy(data + 0x1d0, geometry, sizeof geometry);
                        reinterpret_cast<Shader>(vt(deferred)[24])(deferred, nullptr);
                        std::memcpy(geometry, data + 0x1d0, sizeof geometry);
                        std::memcpy(&dirty, data + 0x168, sizeof dirty);
                        std::memcpy(&shader_flags, data + 0x218, sizeof shader_flags);
                        check(!geometry[0] && !geometry[1] && dirty == 0x3e80 && shader_flags == 0x10e,
                              "geometry unbind clears both native shader-data bindings without losing tessellation state");
                        data[0x390]=3;
                        const UINT64 pipeline[]={0x1234,0x5678,0x9abc,0xdef0,0x1357,0x2468};
                        std::memcpy(data+0x180,pipeline,16);
                        std::memcpy(data+0x1c0,pipeline+2,32);
                        const UINT64 predicate=0x5678,unrelated=0x1122334455667788ull;
                        const UINT hash=0x1234;
                        const UINT8 predicate_flags=0x80;
                        dirty=0x400000;shader_flags=0x10000000000ull;
                        std::memcpy(data+0x168,&dirty,4);std::memcpy(data+0x218,&shader_flags,8);
                        std::memcpy(data+0x1e4,&hash,4);std::memcpy(data+0x1f0,&predicate,8);
                        std::memcpy(data+0x1f8,&predicate_flags,1);std::memcpy(data+0x350,&unrelated,8);
                        const auto table=vt(deferred);
                        reinterpret_cast<void(STDMETHODCALLTYPE*)(void*)>(table[110])(deferred);
                        UINT64 observed[6]{};
                        std::memcpy(observed,data+0x180,16);std::memcpy(observed+2,data+0x1c0,32);
                        UINT64 guard=0,condition=1;UINT cleared_hash=1;UINT8 condition_flags=0;
                        std::memcpy(&dirty,data+0x168,4);std::memcpy(&shader_flags,data+0x218,8);
                        std::memcpy(&guard,data+0x350,8);std::memcpy(&condition,data+0x1f0,8);
                        std::memcpy(&condition_flags,data+0x1f8,1);std::memcpy(&cleared_hash,data+0x1e4,4);
                        check(std::all_of(std::begin(observed),std::end(observed),[](auto x){return !x;}) && !condition && !cleared_hash && condition_flags==0x81,
                              "ClearState resets public shader bindings, blend key and predication with original flag semantics");
                        check(dirty==0x42bfb0 && shader_flags==0x1004c00002eull && guard==unrelated && vt(deferred)==table && data[0x390]==3 &&
                              reinterpret_cast<State>(table[112])(deferred)==1 && reinterpret_cast<State>(table[113])(deferred)==flags,
                              "ClearState preserves unrelated public state, dispatch, type and creation flags while marking native dirty fields");
                        const UINT64 vertex_marker=0x398,pixel_marker=0x400;
                        std::memcpy(data+0x180,&vertex_marker,8);std::memcpy(data+0x188,&pixel_marker,8);
                        reinterpret_cast<void(STDMETHODCALLTYPE*)(void*)>(table[110])(deferred);
                        std::memcpy(observed,data+0x180,16);
                        check(observed[0]==vertex_marker && observed[1]==pixel_marker && data[0x390]==3,
                              "ClearState preserves the original special shader markers and out-of-line dispatch");
                        using Finish = HRESULT (STDMETHODCALLTYPE*)(void*, BOOL, void**);
                        using Execute = void (STDMETHODCALLTYPE*)(void*,void*,BOOL);
                        const auto finish=reinterpret_cast<Finish>(vt(deferred)[114]);
                        const auto execute=reinterpret_cast<Execute>(vt(context)[58]);
                        void* command_list = nullptr;
                        check(finish(deferred,FALSE,&command_list)==S_OK && command_list,
                              "FinishCommandList creates an immutable empty list at the native slot");
                        if(command_list) {
                            check(reinterpret_cast<State>(vt(command_list)[8])(command_list)==flags,
                                  "command list GetContextFlags preserves original deferred creation flags");
                            void* owner=nullptr;
                            reinterpret_cast<GetDevice>(vt(command_list)[3])(command_list,&owner);
                            check(owner==device,"command list shares device identity");
                            if(owner) reinterpret_cast<Count>(vt(owner)[2])(owner);
                            const D3D11_VIEWPORT keep{2,3,16,8,0,1};
                            reinterpret_cast<SetViewports>(vt(context)[44])(context,1,&keep);
                            execute(context,command_list,FALSE);execute(context,command_list,TRUE);
                            UINT capacity=1;D3D11_VIEWPORT result{};
                            reinterpret_cast<GetViewports>(vt(context)[95])(context,&capacity,&result);
                            const D3D11_VIEWPORT empty{};
                            check(flags&0x20000 ? (capacity==1 && !std::memcmp(&result,&keep,sizeof keep)) :
                                (!capacity && !std::memcmp(&result,&empty,sizeof empty)),
                                  "11295 ExecuteCommandList ignores restore: common lists clear tracked state, Draw Bundles preserve bindings on reuse");
                            reinterpret_cast<Count>(vt(command_list)[2])(command_list);
                        }
                        check(finish(deferred,TRUE,nullptr)==S_OK,"null FinishCommandList output discards the recording");
                        void* invalid=reinterpret_cast<void*>(1);
                        check(reinterpret_cast<Finish>(vt(context)[114])(context,FALSE,&invalid)==E_NOTIMPL && invalid==reinterpret_cast<void*>(1),
                              "11295 immediate FinishCommandList preserves the original error and untouched output");
                        static unsigned command_slot=0;
                        set_d3d11x_trap_handler([](const char*,unsigned slot)->std::uintptr_t {command_slot=slot;return E_NOTIMPL;});
                        for(const UINT profile:{0x400000u,0x80c00000u}) {
                            command_slot=0;
                            const auto flush=reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,UINT,void*,SIZE_T)>(vt(context)[152]);
                            flush(context,profile,nullptr,0);
                            check(!command_slot,"reference immediate global cache profiles validate without a GPU barrier");
                            reinterpret_cast<decltype(flush)>(vt(deferred)[152])(deferred,profile,nullptr,0);
                            check(!command_slot,"reference deferred global cache profiles record their barrier");
                            void* cache_list=nullptr;
                            check(finish(deferred,FALSE,&cache_list)==S_OK && cache_list,"accepted cache profiles produce a complete deferred list");
                            if(cache_list) {execute(context,cache_list,FALSE);execute(context,cache_list,TRUE);reinterpret_cast<Count>(vt(cache_list)[2])(cache_list);}
                        }
                        for(const UINT wait_flags:{1u,UINT_MAX}) for(void* target:{context,deferred}) {
                            command_slot=0;
                            reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,UINT)>(vt(target)[153])(target,wait_flags);
                            check(command_slot==153,"reference InsertWaitUntilIdle rejects nonzero flags on immediate and deferred contexts");
                        }
                        execute(context,reinterpret_cast<void*>(1),FALSE);
                        check(command_slot==58,"ExecuteCommandList rejects unreadable list identity before submission");
                        reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,UINT)>(vt(deferred)[197])(deferred,0xffffffffu);
                        check(command_slot==197,"unknown GPU pipeline events stay explicitly unsupported");
                        command_slot=0;
                        reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,UINT,void*,SIZE_T)>(vt(context)[152])(context,0,nullptr,0);
                        check(command_slot==152,"cache flush rejects flags outside both reference profiles");
                        for(const UINT profile:{0x400000u,0x80c00000u}) {
                            command_slot=0;
                            reinterpret_cast<void(STDMETHODCALLTYPE*)(void*,UINT,void*,SIZE_T)>(vt(deferred)[152])(deferred,profile,reinterpret_cast<void*>(1),16);
                            check(command_slot==152,"both reference cache profiles reject non-global ranges");
                        }
                        reinterpret_cast<void(STDMETHODCALLTYPE*)(void*)>(vt(deferred)[200])(deferred);
                        check(finish(deferred,FALSE,&invalid)==E_NOTIMPL && !invalid,
                              "unsupported deferred commands cannot produce a successful partial list");
                        set_d3d11x_trap_handler(nullptr);
                        void* owned = nullptr;
                        reinterpret_cast<GetDevice>(vt(deferred)[3])(deferred, &owned);
                        check(owned == device && refs(device) == 2, "inline dispatch GetDevice retains the same device");
                        reinterpret_cast<Count>(vt(owned)[2])(owned);
                        check(reinterpret_cast<Count>(vt(deferred)[2])(deferred) == 0 && refs(device) == 1,
                              "deferred context releases internal device ownership");
                    }
                }
                ++refs(context);
                check(reinterpret_cast<Count>(vt(context)[2])(context) == 1, "legacy inline count honoured");
                check(reinterpret_cast<Count>(vt(device)[2])(device) == 0, "legacy public device count reaches zero while context lives");
                check(reinterpret_cast<Count>(vt(context)[2])(context) == 0, "legacy queue retired and objects released");
            }
        }
        check(FAILED(bind_d3d11x_abi(L"xone-absent-graphics-version.dll")) &&
              !resolve_title_import({"d3d11_x.dll", "D3D11XCreateDeviceX", 0}), "failed rebinding clears old ABI");
        auto untrusted = read(argv[1]);
        const std::array<std::uint8_t,4> version_signature{0xbd,0x04,0xef,0xfe};
        auto info = std::search(untrusted.begin(),untrusted.end(),version_signature.begin(),version_signature.end());
        check(info != untrusted.end() && untrusted.end() - info >= 16, "own fixture contains fixed version metadata");
        if (info != untrusted.end() && untrusted.end() - info >= 16 && untrusted.size() < 896000) {
            const DWORD ms = MAKELONG(0,10), ls = MAKELONG(5104,16299);
            std::memcpy(&*(info+8),&ms,4); std::memcpy(&*(info+12),&ls,4);
            untrusted.resize(896000);
            wchar_t temp[MAX_PATH]; GetTempPathW(MAX_PATH,temp);
            const auto path = std::filesystem::path(temp) / (L"xone-era-graphics-identity-" + std::to_wstring(GetCurrentProcessId()) + L".dll");
            { std::ofstream file(path,std::ios::binary); file.write(reinterpret_cast<const char*>(untrusted.data()),untrusted.size()); }
            check(bind_d3d11x_abi(path.wstring()) == HRESULT_FROM_WIN32(ERROR_CRC) &&
                !resolve_title_import({"d3d11_x.dll","D3D11XCreateDeviceX",0}),
                "matching modern version and size cannot enable an unverified module hash or the old ABI");
            check(DeleteFileW(path.c_str()), "own graphics identity negative control is removed");
        }
    }

    {
        // WinRT title layer: XboxLiveConfiguration from a synthetic package manifest.
        check(SUCCEEDED(RoInitialize(RO_INIT_MULTITHREADED)), "WinRT initialized");
        wchar_t temp[MAX_PATH];
        GetTempPathW(MAX_PATH, temp);
        const std::wstring package = std::wstring(temp) + L"xone-era-package-" + std::to_wstring(GetCurrentProcessId());
        const std::wstring manifest = package + L"\\AppxManifest.xml";
        CreateDirectoryW(package.c_str(), nullptr);
        {
            std::ofstream out(manifest, std::ios::binary);
            out << "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n"
                   "<Package xmlns=\"http://schemas.microsoft.com/appx/2010/manifest\" "
                   "xmlns:mx=\"http://schemas.microsoft.com/appx/2013/xbox/manifest\">"
                   "<Extensions><mx:Extension Category=\"xbox.live\">"
                   "<mx:XboxLive TitleId=\"0000F00D\" PrimaryServiceConfigId=\"00000000-1111-2222-3333-44440000f00d\"/>"
                   "</mx:Extension></Extensions>"
                   "<Applications><Application Id=\"App\" Executable=\"t.exe\" EntryPoint=\"Title.App\"/></Applications>"
                   "</Package>";
        }
        set_title_package_root(package);
        struct __declspec(uuid("00193376-6a84-4c67-84a5-3b8a8aaea712")) Statics : IInspectable {
            virtual HRESULT STDMETHODCALLTYPE get_TitleId(HSTRING*) = 0;
            virtual HRESULT STDMETHODCALLTYPE get_PrimaryServiceConfigId(HSTRING*) = 0;
            virtual HRESULT STDMETHODCALLTYPE get_SandboxId(HSTRING*) = 0;
        };
        const auto text = [](HSTRING s) {
            std::wstring v = WindowsGetStringRawBuffer(s, nullptr);
            WindowsDeleteString(s);
            return v;
        };
        const auto activate = [](const wchar_t* name, REFIID iid, void** out) {
            HSTRING id = nullptr;
            WindowsCreateString(name, static_cast<UINT32>(wcslen(name)), &id);
            const HRESULT hr = get_title_activation_factory(id, iid, out);
            WindowsDeleteString(id);
            return hr;
        };
        {
            const auto root=std::filesystem::path(package)/L"metadata-\u03a9";
            std::filesystem::create_directory(root);
            const auto xml=root/L"AppxManifest.xml",exe=root/L"owned.exe";
            std::filesystem::copy_file(std::filesystem::path(__FILE__).parent_path()/L"appmodel/AppxManifest.xml",xml);
            std::filesystem::copy_file(argv[1],exe);
            std::string hash,manifest_hash,error;
            check(xboxone::inventory::fingerprint(exe,hash,error) && xboxone::inventory::fingerprint(xml,manifest_hash,error),
                "package fixture has pinned executable and manifest bytes");
            xboxone::appmodel::Manifest expected;
            check(xboxone::appmodel::read_manifest(xml,manifest_hash,expected,error),"package fixture uses the existing SDK metadata parser");
            constexpr GUID iid_statics{0x4e534bdf,0x2960,0x4878,{0x97,0xa4,0x96,0x24,0xde,0xb7,0x2f,0x2d}};
            IInspectable* factory=nullptr;
            check(activate(L"Windows.ApplicationModel.Package",iid_statics,reinterpret_cast<void**>(&factory))==S_OK && factory,
                "Package statics use the session WinRT boundary");
            if(factory) {
                using Get=HRESULT(STDMETHODCALLTYPE*)(void*,IInspectable**);
                const auto get=reinterpret_cast<Get>((*reinterpret_cast<void***>(factory))[6]);
                IActivationFactory* activation_factory=nullptr;IUnknown* identity=nullptr;
                check(factory->QueryInterface(__uuidof(IActivationFactory),reinterpret_cast<void**>(&activation_factory))==S_OK &&
                    activation_factory && factory->QueryInterface(__uuidof(IUnknown),reinterpret_cast<void**>(&identity))==S_OK &&
                    identity==activation_factory,"Package factory and statics preserve one COM identity");
                if(identity)identity->Release();if(activation_factory)activation_factory->Release();
                IInspectable* retained=nullptr;
                IInspectable* current=reinterpret_cast<IInspectable*>(1);
                set_title_package_root({});
                check(get(factory,nullptr)==E_POINTER && get(factory,&current)==HRESULT_FROM_WIN32(APPMODEL_ERROR_NO_PACKAGE) && !current,
                    "Current refuses an absent package identity and clears failed outputs");
                set_title_package_root(root.wstring(),"owned.exe",hash);
                check(get(factory,&current)==S_OK && current,"Current binds only the verified selected manifest and executable");
                if(current) {
                    constexpr GUID iid_watcher_statics{0xa306e506,0x12f9,0x460a,{0xb0,0xae,0x50,0xba,0x32,0x8c,0x78,0x6c}};
                    IInspectable* watcher_factory=nullptr;
                    check(activate(L"Windows.Xbox.Management.Deployment.PackageTransferWatcher",iid_watcher_statics,
                        reinterpret_cast<void**>(&watcher_factory))==S_OK && watcher_factory,"watcher statics match the original factory IID");
                    if(watcher_factory) {
                        const auto create=reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*,IInspectable*,IInspectable**)>(
                            (*reinterpret_cast<void***>(watcher_factory))[7]);
                        const auto chunks=reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*,IInspectable*,IInspectable*,IInspectable**)>(
                            (*reinterpret_cast<void***>(watcher_factory))[6]);
                        IInspectable* observer=reinterpret_cast<IInspectable*>(1);
                        check(create(watcher_factory,current,nullptr)==E_POINTER && create(watcher_factory,nullptr,&observer)==E_INVALIDARG && !observer,
                            "watcher creation validates outputs and package arguments");
                        check(create(watcher_factory,factory,&observer)==E_INVALIDARG && !observer,
                            "watcher refuses a foreign package identity without dereferencing it");
                        check(chunks(watcher_factory,current,nullptr,&observer)==E_NOTIMPL && !observer,
                            "unproved chunk selection remains explicitly unsupported");
                        check(create(watcher_factory,current,&observer)==S_OK && observer,"watcher observes the verified package without deployment");
                        if(observer) {
                            struct Handler : IUnknown {
                                ULONG refs=1;unsigned calls=0;
                                HRESULT STDMETHODCALLTYPE QueryInterface(REFIID,void** out) override {if(!out)return E_POINTER;*out=nullptr;return E_NOINTERFACE;}
                                ULONG STDMETHODCALLTYPE AddRef() override {return ++refs;}
                                ULONG STDMETHODCALLTYPE Release() override {return --refs;}
                                virtual HRESULT STDMETHODCALLTYPE Invoke(IInspectable*,IInspectable*) {++calls;return S_OK;}
                            } handler;
                            constexpr GUID iid_watcher{0xe74b8e43,0xba51,0x4b09,{0xad,0x3f,0x01,0xba,0xff,0x31,0xf9,0x34}};
                            IInspectable* same=nullptr;
                            check(observer->QueryInterface(iid_watcher,reinterpret_cast<void**>(&same))==S_OK && same==observer,
                                "watcher preserves the Xbox instance IID and identity");
                            if(same)same->Release();
                            auto** vt=*reinterpret_cast<void***>(observer);
                            for(UINT slot:{7u,9u,11u}) {
                                auto add=reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*,IUnknown*,std::int64_t*)>(vt[slot]);
                                auto remove=reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*,std::int64_t)>(vt[slot+1]);
                                std::int64_t token=0;
                                check(add(observer,nullptr,&token)==E_INVALIDARG && add(observer,&handler,nullptr)==E_INVALIDARG && handler.refs==1,
                                    "watcher refuses invalid registrations without retaining handlers");
                                check(add(observer,&handler,&token)==S_OK && token!=0 && handler.refs==2 && handler.calls==0,
                                    "watcher retains subscriptions without inventing transfer events");
                                check(remove(observer,token)==S_OK && remove(observer,token)==S_OK && handler.refs==1,
                                    "watcher removes each event subscription exactly once");
                            }
                            std::int64_t token=0;
                            reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*,IUnknown*,std::int64_t*)>(vt[7])(observer,&handler,&token);
                            IWeakReferenceSource* source=nullptr;IWeakReference* weak=nullptr;
                            check(observer->QueryInterface(__uuidof(IWeakReferenceSource),reinterpret_cast<void**>(&source))==S_OK && source,
                                "watcher supports the original caller's weak event ownership");
                            if(source) {
                                IUnknown* identity=nullptr;
                                check(source->QueryInterface(__uuidof(IUnknown),reinterpret_cast<void**>(&identity))==S_OK && identity==observer,
                                    "weak source shares the watcher's COM identity");
                                if(identity)identity->Release();
                                check(source->GetWeakReference(nullptr)==E_POINTER && source->GetWeakReference(&weak)==S_OK && weak,
                                    "weak references validate outputs and retain their own control");
                                source->Release();
                            }
                            if(weak) {
                                check(weak->Resolve(iid_watcher,&same)==S_OK && same==observer,"live weak reference resolves the original watcher");
                                if(same)same->Release();
                                same=reinterpret_cast<IInspectable*>(1);
                                check(weak->Resolve(GUID_NULL,&same)==E_NOINTERFACE && !same,"weak resolution preserves unsupported IID failures");
                            }
                            set_title_package_root({});
                            IInspectable* absent=reinterpret_cast<IInspectable*>(1);
                            check(create(watcher_factory,current,&absent)==HRESULT_FROM_WIN32(APPMODEL_ERROR_NO_PACKAGE) && !absent,
                                "watcher cannot create an observer after session unbinding");
                            observer->Release();
                            check(handler.refs==1 && handler.calls==0,"watcher teardown releases pending handlers without completion callbacks");
                            if(weak) {
                                same=reinterpret_cast<IInspectable*>(1);
                                check(weak->Resolve(iid_watcher,&same)==S_OK && !same,"expired weak reference cannot resurrect a destroyed watcher");
                                weak->Release();
                            }
                            set_title_package_root(root.wstring(),"owned.exe",hash);
                        }
                        watcher_factory->Release();
                    }
                    IInspectable* second=nullptr;
                    check(get(factory,&second)==S_OK && second==current,"Current retains one COM identity for an immutable package revision");
                    if(second)second->Release();
                    xboxone::appmodel::Manifest metadata;std::wstring location;std::string revision;
                    check(title_package_metadata(metadata,location,revision)==S_OK && metadata.name==expected.name &&
                        metadata.version==expected.version && location==root.wstring() && revision==manifest_hash+hash,
                        "Current context preserves the real SDK metadata and both hashes");
                    set_title_package_root({});second=reinterpret_cast<IInspectable*>(1);
                    HSTRING value=nullptr;
                    check(get(factory,&second)==HRESULT_FROM_WIN32(APPMODEL_ERROR_NO_PACKAGE) && !second &&
                        current->GetRuntimeClassName(&value)==S_OK && text(value)==L"Windows.ApplicationModel.Package",
                        "rebinding invalidates Current while retained snapshots remain alive");
                    retained=current;
                }
                set_title_package_root(root.wstring(),"../owned.exe",hash);
                check(get(factory,&current)==E_INVALIDARG && !current,"package executable traversal is refused");
                set_title_package_root(root.wstring(),"owned.exe",std::string(64,'0'));
                check(get(factory,&current)==HRESULT_FROM_WIN32(ERROR_CRC) && !current,"selected executable hash mismatch cannot produce a package");
                set_title_package_root(root.wstring(),"absent.exe",hash);
                check(FAILED(get(factory,&current)) && !current,"missing selected executable cannot produce a package");
                const auto other=root/L"other.exe";std::filesystem::copy_file(exe,other);
                set_title_package_root(root.wstring(),"other.exe",hash);
                check(get(factory,&current)==HRESULT_FROM_WIN32(ERROR_BAD_EXE_FORMAT) && !current,
                    "a valid hash cannot bind an executable absent from the manifest");
                std::filesystem::remove(other);
                set_title_package_root(root.wstring(),"owned.exe",hash);
                {std::ofstream changed(xml,std::ios::app);changed<<' ';}
                check(get(factory,&current)==HRESULT_FROM_WIN32(ERROR_INVALID_DATA) && !current,"manifest mutation after binding invalidates the snapshot");
                {std::ofstream changed(xml);changed<<"<!DOCTYPE Package [<!ENTITY x SYSTEM 'file:///outside'>]><Package>&x;</Package>";}
                set_title_package_root(root.wstring(),"owned.exe",hash);
                check(FAILED(get(factory,&current)) && !current,"malformed or external-entity manifest cannot manufacture a package");
                {std::ofstream legacy(xml);legacy<<
                    "<Package xmlns=\"http://schemas.microsoft.com/appx/2010/manifest\">"
                    "<Identity Name=\"Owned.Legacy\" Publisher=\"CN=Owned\" Version=\"1.2.3.4\" ProcessorArchitecture=\"x64\"/>"
                    "<Properties><DisplayName>Owned</DisplayName><PublisherDisplayName>Owned</PublisherDisplayName><Logo>logo.png</Logo></Properties>"
                    "<Prerequisites><OSMinVersion>6.2</OSMinVersion><OSMaxVersionTested>6.2</OSMaxVersionTested></Prerequisites>"
                    "<Resources><Resource Language=\"en-US\"/></Resources><Applications>"
                    "<Application Id=\"App\" Executable=\"owned.exe\" EntryPoint=\"Owned.App\">"
                    "<VisualElements DisplayName=\"Owned\" Logo=\"logo.png\" SmallLogo=\"small.png\" Description=\"Owned\" ForegroundText=\"light\" BackgroundColor=\"#222222\"><SplashScreen Image=\"splash.png\"/></VisualElements>"
                    "</Application></Applications></Package>";}
                check(xboxone::inventory::fingerprint(xml,manifest_hash,error) &&
                    xboxone::appmodel::read_manifest(xml,manifest_hash,expected,error) && expected.device_families.empty() && !expected.unsupported_contracts.empty(),
                    "Appx 2010 metadata preserves absent device families and explicit unsupported activation contracts");
                set_title_package_root(root.wstring(),"owned.exe",hash);
                check(get(factory,&current)==S_OK && current,"Current supports verified legacy metadata without host deployment");
                check(expected.version==0x0001000200030004ull,"legacy metadata preserves all four version components");
                if(current)current->Release();
                if(retained) {
                    HSTRING value=nullptr;
                    check(retained->GetRuntimeClassName(&value)==S_OK && text(value)==L"Windows.ApplicationModel.Package",
                        "a retained snapshot survives cache replacement by another package revision");
                    retained->Release();
                }
                factory->Release();
            }
            set_title_package_root(package);
            std::filesystem::remove(exe);std::filesystem::remove(xml);std::filesystem::remove(root);
        }
        Statics* statics = nullptr;
        const HMODULE legacy_cx = GetModuleHandleW(L"vccorlib110.dll");
        if (legacy_cx) {
            check(hook_winrt_activation(legacy_cx), "legacy CX activation import hooked");
            check(hook_winrt_activation(legacy_cx), "legacy CX activation hook is idempotent");
            using LegacyFactory = HRESULT (*)(const wchar_t*, const GUID&, void**);
            const auto legacy_factory = reinterpret_cast<LegacyFactory>(resolve_host_module({
                "vccorlib110.dll", "?GetActivationFactoryByPCWSTR@@YAJPEAXAEAVGuid@Platform@@PEAPEAX@Z", 0}));
            check(legacy_factory != nullptr, "legacy CX factory export resolved");
            if (legacy_factory) {
                Statics* via_cx = nullptr;
                check(legacy_factory(L"Windows.Xbox.Services.XboxLiveConfiguration", __uuidof(Statics),
                                     reinterpret_cast<void**>(&via_cx)) == S_OK && via_cx,
                      "legacy CX reaches the title activation boundary");
                if (via_cx) via_cx->Release();
            }
        }
        check(activate(L"Windows.Xbox.Services.XboxLiveConfiguration", __uuidof(Statics),
                       reinterpret_cast<void**>(&statics)) == S_OK && statics, "XboxLiveConfiguration served by title layer");
        if (statics) {
            HSTRING value = nullptr;
            check(statics->get_TitleId(&value) == S_OK && text(value) == L"0000F00D", "TitleId read from package manifest");
            check(statics->get_PrimaryServiceConfigId(&value) == S_OK &&
                  text(value) == L"00000000-1111-2222-3333-44440000f00d", "PrimaryServiceConfigId read from package manifest");
            check(statics->get_SandboxId(&value) == S_OK && text(value) == L"RETAIL", "retail sandbox");
            check(statics->GetRuntimeClassName(&value) == S_OK &&
                  text(value) == L"Windows.Xbox.Services.XboxLiveConfiguration", "runtime class name");
            IActivationFactory* factory = nullptr;
            check(statics->QueryInterface(__uuidof(IActivationFactory), reinterpret_cast<void**>(&factory)) == S_OK,
                  "factory answers IActivationFactory");
            IInspectable* instance = reinterpret_cast<IInspectable*>(1);
            check(factory && factory->ActivateInstance(&instance) == E_NOTIMPL && !instance, "static class has no instances");
            statics->Release();
            if (factory) factory->Release();
        }
        IActivationFactory* host = nullptr;
        check(activate(L"Windows.Foundation.Uri", __uuidof(IActivationFactory), reinterpret_cast<void**>(&host)) == S_OK && host,
              "host class forwarded to Windows");
        void* missing = &missing;
        {
            const auto previous_input_root = title_storage_root();
            wchar_t input_temp[MAX_PATH]{}; GetTempPathW(MAX_PATH, input_temp);
            const auto input_root = std::filesystem::path(input_temp) / (L"xone-input-profile-" + std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()));
            set_title_storage_root(input_root.wstring());
            constexpr GUID controller_statics{0x7400624f,0x144b,0x47fc,{0x9f,0x3b,0x1d,0xca,0xfb,0x48,0x28,0x73}};
            constexpr GUID gamepad_statics{0x8c4c1bcd,0xfdab,0x4bf8,{0xbd,0xe9,0xab,0xb2,0xb1,0xd6,0xfd,0xd3}};
            IInspectable* input = nullptr;
            check(activate(L"Windows.Xbox.Input.Controller",controller_statics,reinterpret_cast<void**>(&input)) == S_OK && input,
                "observed Controller statics are implemented in the common runtime");
            using GetView = HRESULT(STDMETHODCALLTYPE*)(void*, IInspectable**);
            using Size = HRESULT(STDMETHODCALLTYPE*)(void*, UINT*);
            using Get = HRESULT(STDMETHODCALLTYPE*)(void*, UINT, IUnknown**);
            IInspectable* view = nullptr;
            if (input) {
                const auto get = reinterpret_cast<GetView>((*reinterpret_cast<void***>(input))[6]);
                check(get(input,nullptr) == E_POINTER && get(input,&view) == S_OK && view, "real XInput enumeration validates outputs");
            }
            if (view) {
                const auto vt = *reinterpret_cast<void***>(view);
                UINT count = 99;
                check(reinterpret_cast<Size>(vt[7])(view,&count) == S_OK && count <= 4, "controller snapshot reflects the bounded host API");
                IUnknown* item = reinterpret_cast<IUnknown*>(1);
                check(reinterpret_cast<Get>(vt[6])(view,count,&item) == E_BOUNDS && !item, "snapshot rejects an out-of-range controller");
                if (count) {
                    check(reinterpret_cast<Get>(vt[6])(view,0,&item) == S_OK && item, "connected host controller has a retained interface");
                    if (item) {
                        IUnknown* identity = nullptr;
                        check(item->QueryInterface(__uuidof(IUnknown),reinterpret_cast<void**>(&identity)) == S_OK && identity == item,
                            "controller COM identity is preserved");
                        if (identity) identity->Release();
                        using GetUser = HRESULT(STDMETHODCALLTYPE*)(void*, IInspectable**);
                        const auto get_user = reinterpret_cast<GetUser>((*reinterpret_cast<void***>(item))[8]);
                        IInspectable* paired = nullptr; IInspectable* selected = nullptr;
                        check(get_user(item, nullptr) == E_POINTER, "Controller.User validates its output");
                        check(get_user(item, &paired) == S_OK && paired && title_local_user(reinterpret_cast<void**>(&selected)) == S_OK && paired == selected,
                            "physical controller retains the same local profile identity as users and saves");
                        if (paired) {
                            UINT id = 0; GUID profile{}; std::uint32_t expected = 0;
                            const auto user_id = reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*, UINT*)>((*reinterpret_cast<void***>(paired))[6]);
                            check(title_local_user_identity(&profile, &expected) == S_OK && user_id(paired, &id) == S_OK && id == expected,
                                "Controller.User exposes the persisted local user ID without an online identity");
                            stop_title_input();
                            IInspectable* retained_user = nullptr;
                            check(user_id(paired, &id) == S_OK && id == expected && get_user(item, &retained_user) == S_OK && retained_user == paired,
                                "retained controller and user snapshots survive input registry teardown");
                            if (retained_user) retained_user->Release();
                            paired->Release();
                        }
                        if (selected) selected->Release();
                        constexpr GUID gamepad_iid{0x082e402c,0x07df,0x45e1,{0xa5,0xab,0xa3,0x12,0x7a,0xf1,0x97,0xb5}};
                        IUnknown* gamepad=nullptr;
                        check(item->QueryInterface(gamepad_iid,reinterpret_cast<void**>(&gamepad))==S_OK && gamepad,
                            "host controller shares its gamepad identity");
                        if (gamepad) {
                            using Vibrate = HRESULT(STDMETHODCALLTYPE*)(void*, const std::array<float,4>*);
                            const auto vibrate = reinterpret_cast<Vibrate>((*reinterpret_cast<void***>(gamepad))[6]);
                            std::array<float,4> vibration{};
                            check(vibrate(gamepad,nullptr) == E_INVALIDARG && vibrate(gamepad,reinterpret_cast<const std::array<float,4>*>(1)) == E_INVALIDARG,
                                  "vibration validates its four-float x64 ABI input");
                            const HRESULT stopped = vibrate(gamepad,&vibration);
                            check(stopped == S_OK || stopped == HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED), "zero vibration reaches the real XInput device");
                            for (float invalid : {-1.f, 1.01f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
                                vibration[0] = invalid;
                                check(vibrate(gamepad,&vibration) == E_INVALIDARG, "invalid motor levels cannot reach the physical controller");
                            }
                            vibration = {0,0,1,0};
                            check(vibrate(gamepad,&vibration) == HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED), "trigger motors remain explicit when the host transport cannot execute them");
                            using ReadObject = HRESULT(STDMETHODCALLTYPE*)(void*, IInspectable**);
                            const auto current_reading = reinterpret_cast<ReadObject>((*reinterpret_cast<void***>(gamepad))[7]);
                            IInspectable* snapshot = nullptr;
                            FILETIME before_time{}, after_time{}; GetSystemTimePreciseAsFileTime(&before_time);
                            check(current_reading(gamepad, nullptr) == E_POINTER, "Gamepad.GetCurrentReading validates its output");
                            const HRESULT snapshot_hr = current_reading(gamepad, &snapshot);
                            GetSystemTimePreciseAsFileTime(&after_time);
                            check(snapshot_hr == HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED) || (snapshot_hr == S_OK && snapshot),
                                "connected physical gamepad returns a real reading snapshot or its disconnection error");
                            if (snapshot) {
                                auto** reading_table = *reinterpret_cast<void***>(snapshot);
                                UINT64 timestamp = 0; UINT buttons = 0;
                                check(reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*,UINT64*)>(reading_table[6])(snapshot,&timestamp) == S_OK &&
                                      timestamp >= ((UINT64(before_time.dwHighDateTime)<<32)|before_time.dwLowDateTime) &&
                                      timestamp <= ((UINT64(after_time.dwHighDateTime)<<32)|after_time.dwLowDateTime), "reading DateTime uses the actual UTC FILETIME clock");
                                check(reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*,UINT*)>(reading_table[7])(snapshot,&buttons) == S_OK,
                                      "reading snapshot exposes its captured buttons");
                                const UINT masks[]{0x100,0x200,0x400,0x800,4,8,0x4000,0x8000,0x1000,0x2000,0x10,0x20,0x40,0x80};
                                for (UINT i = 0; i < std::size(masks); ++i) {
                                    struct { boolean value = false; BYTE guard = 0xa5; } pressed;
                                    const auto get = reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*,boolean*)>(reading_table[8+i]);
                                    check(get(snapshot,nullptr) == E_POINTER && get(snapshot,&pressed.value) == S_OK && bool(pressed.value) == bool(buttons & masks[i]) && pressed.guard == 0xa5,
                                          "reading button getter matches the same snapshot and preserves its one-byte ABI");
                                }
                                for (UINT i = 0; i < 6; ++i) {
                                    float value = 9, repeated = 10;
                                    const auto get = reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*,float*)>(reading_table[22+i]);
                                    check(get(snapshot,nullptr) == E_POINTER && get(snapshot,&value) == S_OK && get(snapshot,&repeated) == S_OK &&
                                          value == repeated && value >= (i < 2 ? 0.f : -1.f) && value <= 1.f, "reading axes preserve normalized immutable values");
                                }
                                IUnknown* canonical = nullptr;
                                check(snapshot->QueryInterface(__uuidof(IUnknown),reinterpret_cast<void**>(&canonical)) == S_OK && canonical == snapshot,
                                      "reading snapshot retains canonical COM identity");
                                if (canonical) canonical->Release();
                                snapshot->Release();
                            }
                            using Read=HRESULT(STDMETHODCALLTYPE*)(void*,RawGamepadReading*);
                            const auto read=reinterpret_cast<Read>((*reinterpret_cast<void***>(gamepad))[8]);
                            struct { RawGamepadReading value; UINT64 guard=0x1122334455667788ull; } sample{};
                            check(read(gamepad,nullptr)==E_POINTER,"raw gamepad reading validates its output");
                            const HRESULT result=read(gamepad,&sample.value);
                            check((result==HRESULT_FROM_WIN32(ERROR_DEVICE_NOT_CONNECTED) || (result==S_OK && sample.value.timestamp &&
                                sample.value.left_trigger>=0 && sample.value.left_trigger<=1 && sample.value.right_trigger>=0 && sample.value.right_trigger<=1 &&
                                sample.value.left_x>=-1 && sample.value.left_x<=1 && sample.value.right_y>=-1 && sample.value.right_y<=1)) &&
                                sample.guard==0x1122334455667788ull,"real gamepad read respects normalized ranges, disconnection and its 40-byte ABI");
                            gamepad->Release();
                        }
                        item->Release();
                    }
                }
                view->Release();
            }
            struct InputHandler : IUnknown {
                std::atomic<ULONG> refs{1};bool agile=true;
                HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out) override {
                    if(!out)return E_POINTER;*out=nullptr;
                    if(iid!=__uuidof(IUnknown) && !(agile && iid==__uuidof(IAgileObject)))return E_NOINTERFACE;
                    *out=this;AddRef();return S_OK;
                }
                ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
                ULONG STDMETHODCALLTYPE Release() override { return --refs; }
                virtual HRESULT STDMETHODCALLTYPE Invoke(IInspectable*,IInspectable*) { return S_OK; }
            } handler;
            using AddInput = HRESULT(STDMETHODCALLTYPE*)(void*, IUnknown*, INT64*);
            using RemoveInput = HRESULT(STDMETHODCALLTYPE*)(void*, INT64);
            if (input) {
                auto vt = *reinterpret_cast<void***>(input); INT64 token = 0;
                check(reinterpret_cast<AddInput>(vt[7])(input,&handler,&token) == S_OK && token && handler.refs == 2,
                    "input subscription retains the real handler");
                check(reinterpret_cast<RemoveInput>(vt[8])(input,token) == S_OK && handler.refs == 1,
                    "input unsubscription releases handler ownership");
                HRESULT worker_add=E_FAIL,worker_poll=S_OK,nonagile_add=S_OK;INT64 worker_token=0,rejected_token=99;
                InputHandler nonagile;nonagile.agile=false;
                std::thread input_worker([&] {
                    worker_add=reinterpret_cast<AddInput>(vt[7])(input,&handler,&worker_token);
                    worker_poll=poll_title_input();
                    nonagile_add=reinterpret_cast<AddInput>(vt[7])(input,&nonagile,&rejected_token);
                });input_worker.join();
                check(worker_add==S_OK && worker_token && handler.refs==2 && worker_poll==RPC_E_WRONG_THREAD,
                    "worker subscription of an agile delegate preserves dispatcher polling ownership");
                check(nonagile_add==RPC_E_WRONG_THREAD && rejected_token==0 && nonagile.refs==1,
                    "non-agile worker delegate is refused without a marshaling path or leaked reference");
                check(reinterpret_cast<RemoveInput>(vt[8])(input,worker_token)==S_OK && handler.refs==1 && poll_title_input()==S_OK,
                    "worker event token can be removed while polling remains on the dispatcher");
                check(reinterpret_cast<AddInput>(vt[11])(input,&handler,&token) == S_OK && handler.refs == 2,
                    "observed pairing subscription preserves its callback");
                stop_title_input();
                check(handler.refs == 1, "session teardown releases subscriptions and stops the input timer");
                input->Release();
            }
            IInspectable* pads = nullptr;
            check(activate(L"Windows.Xbox.Input.Gamepad",gamepad_statics,reinterpret_cast<void**>(&pads)) == S_OK && pads,
                "Gamepad statics use the same host-backed controller registry");
            if (pads) pads->Release();
            stop_title_input();
            set_title_storage_root(previous_input_root);
            std::filesystem::remove_all(input_root);
        }
        check(activate(L"Windows.Xbox.Input.BodyController", __uuidof(IActivationFactory), &missing) == REGDB_E_CLASSNOTREG && !missing,
              "unimplemented Xbox class keeps the real host error");
        {
            constexpr GUID iid_transfer_statics{0xd6cf4cae, 0x9eb1, 0x4cbe, {0xb4,0x26,0x7e,0x75,0xd4,0x29,0xf8,0x46}};
            constexpr GUID iid_transfer{0xc2d4b94b, 0x0413, 0x4380, {0xb2,0xf2,0xab,0x98,0x72,0xd0,0x82,0x85}};
            IInspectable* transfer = nullptr;
            check(activate(L"Windows.Xbox.Management.Deployment.PackageTransferManager", iid_transfer_statics,
                reinterpret_cast<void**>(&transfer)) == S_OK && transfer, "package transfer statics available");
            if (transfer) {
                using Current = HRESULT(STDMETHODCALLTYPE*)(void*, IInspectable**);
                const auto get_current = reinterpret_cast<Current>((*reinterpret_cast<void***>(transfer))[6]);
                check(get_current(transfer, nullptr) == E_POINTER, "transfer Current validates its output");
                set_title_package_root({});
                IInspectable* instance = reinterpret_cast<IInspectable*>(1);
                check(FAILED(get_current(transfer, &instance)) && !instance, "transfer Current requires an active package");
                set_title_package_root(package);
                check(get_current(transfer, &instance) == S_OK && instance, "transfer Current binds the active package");
                if (instance) {
                    using Installed = HRESULT(STDMETHODCALLTYPE*)(void*, UINT32, boolean*);
                    const auto installed = reinterpret_cast<Installed>((*reinterpret_cast<void***>(instance))[7]);
                    const auto batch=reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*,IInspectable*,boolean*)>(
                        (*reinterpret_cast<void***>(instance))[8]);
                    struct Sequence : WinrtObject {
                        struct Cursor : WinrtObject {Sequence* owner=nullptr;} cursor;
                        std::vector<UINT32> values;size_t position=0;ULONG cursor_refs=0;
                        HRESULT failure=S_OK;bool null_cursor=false;
                        static HRESULT STDMETHODCALLTYPE first(Sequence* self,void** out) {
                            *out=nullptr;self->position=0;
                            if(self->null_cursor)return S_OK;
                            ++self->cursor_refs;*out=&self->cursor;return S_OK;
                        }
                        static ULONG STDMETHODCALLTYPE retain(Cursor* self) {return ++self->owner->cursor_refs;}
                        static ULONG STDMETHODCALLTYPE release(Cursor* self) {return --self->owner->cursor_refs;}
                        static HRESULT STDMETHODCALLTYPE current(Cursor* self,UINT32* out) {
                            if(FAILED(self->owner->failure))return self->owner->failure;
                            if(self->owner->position>=self->owner->values.size())return E_BOUNDS;
                            *out=self->owner->values[self->owner->position];return S_OK;
                        }
                        static HRESULT STDMETHODCALLTYPE has(Cursor* self,boolean* out) {
                            *out=self->owner->position<self->owner->values.size();return S_OK;
                        }
                        static HRESULT STDMETHODCALLTYPE next(Cursor* self,boolean* out) {++self->owner->position;return has(self,out);}
                        Sequence() {
                            static const auto collection=[] {auto t=winrt_vtable();t[6]=reinterpret_cast<const void*>(&first);return t;}();
                            static const auto iterator=[] {auto t=winrt_vtable();t[1]=reinterpret_cast<const void*>(&retain);t[2]=reinterpret_cast<const void*>(&release);
                                t[6]=reinterpret_cast<const void*>(&current);t[7]=reinterpret_cast<const void*>(&has);t[8]=reinterpret_cast<const void*>(&next);return t;}();
                            static_cast<WinrtObject&>(*this)={collection.data(),L"Owned.Chunks",
                                winrt_pinterface_iid("pinterface({faa585ea-6214-4217-afda-7f46de5869b3};u4)"),nullptr};
                            static_cast<WinrtObject&>(cursor)={iterator.data(),L"Owned.ChunkIterator",GUID_NULL,nullptr};cursor.owner=this;
                        }
                    } sequence;
                    auto* iterable=reinterpret_cast<IInspectable*>(&sequence);
                    check(installed(instance, 69, nullptr) == E_POINTER, "chunk query validates its output");
                    boolean present = true;
                    check(batch(instance,iterable,nullptr)==E_POINTER && batch(instance,nullptr,&present)==E_INVALIDARG && !present,
                        "batch chunk query validates arguments and clears failed outputs");
                    check(FAILED(batch(instance,iterable,&present)) && !present && !sequence.cursor_refs,
                        "empty batch cannot hide a missing catalog or leak its iterator");
                    check(FAILED(installed(instance, 69, &present)) && !present, "missing catalog is an error, never installed");
                    const auto catalog = package + L"\\hash.manifest";
                    {
                        std::ofstream out(catalog, std::ios::binary);
                        const char truncated = 0;
                        out.write(&truncated, 1);
                    }
                    present = true;
                    check(installed(instance, 69, &present) == HRESULT_FROM_WIN32(ERROR_INVALID_DATA) && !present,
                          "malformed local catalog rejected");
                    check(batch(instance,iterable,&present)==HRESULT_FROM_WIN32(ERROR_INVALID_DATA) && !present && !sequence.cursor_refs,
                        "empty batch preserves malformed catalog failures");
                    {
                        std::ofstream out(catalog, std::ios::binary | std::ios::trunc);
                        const unsigned char record[12]{0,0,0,0,0,0,0,0,69,0,0,0};
                        out.write(reinterpret_cast<const char*>(record), sizeof record);
                    }
                    check(installed(instance, 69, &present) == S_OK && present, "local catalog supplies installed chunk");
                    check(installed(instance, 70, &present) == S_OK && !present, "absent chunk is not invented");
                    check(installed(instance, 0xffffffff, &present) == S_OK && !present, "reserved chunk never installed");
                    check(batch(instance,iterable,&present)==S_OK && present && !sequence.cursor_refs,"empty batch is true only for a valid catalog");
                    sequence.values={69,69};
                    check(batch(instance,iterable,&present)==S_OK && present && !sequence.cursor_refs,"batch checks every requested local chunk, including duplicates");
                    sequence.values={69,70};
                    check(batch(instance,iterable,&present)==S_OK && !present,"a missing batch member is never reported installed");
                    sequence.values={0xffffffff};
                    check(batch(instance,iterable,&present)==S_OK && !present,"reserved batch chunk is never invented");
                    sequence.failure=E_ABORT;
                    check(batch(instance,iterable,&present)==E_ABORT && !present && !sequence.cursor_refs,"iterator failure propagates without publishing partial success");
                    sequence.failure=S_OK;sequence.null_cursor=true;
                    check(batch(instance,iterable,&present)==E_UNEXPECTED && !present,"malformed iterable cannot return a null successful iterator");
                    sequence.null_cursor=false;
                    check(batch(instance,instance,&present)==E_NOINTERFACE && !present,"batch refuses an object without IIterable<UInt32>");
                    set_title_package_root(package);
                    DeleteFileW(catalog.c_str());
                    check(FAILED(installed(instance, 69, &present)), "package rebinding invalidates cached chunk state");
                    check(FAILED(batch(instance,iterable,&present)) && !present,"batch shares catalog invalidation with single-chunk queries");
                    IInspectable* same = nullptr;
                    check(instance->QueryInterface(iid_transfer, reinterpret_cast<void**>(&same)) == S_OK && same == instance,
                          "transfer manager has the declared default interface");
                    if (same) same->Release();
                    instance->Release();
                }
                transfer->Release();
            }
        }
        // Windows.Xbox.System.User statics: slots 12/13 (UserRemoved add/remove) observed from the title.
        constexpr GUID iid_user_statics{0xB4F23A93, 0x7FC6, 0x4A50, {0xB1, 0x53, 0x95, 0x00, 0x16, 0x38, 0x9E, 0x50}};
        IActivationFactory* user = nullptr;
        check(activate(L"Windows.Xbox.System.User", __uuidof(IActivationFactory), reinterpret_cast<void**>(&user)) == S_OK && user,
              "User served by title layer");
        IInspectable* user_statics = nullptr;
        if (user) {
            check(user->QueryInterface(iid_user_statics, reinterpret_cast<void**>(&user_statics)) == S_OK && user_statics,
                  "User factory answers IUserStatics");
        }
        if (user_statics && host) {
            {
                using Users=HRESULT(STDMETHODCALLTYPE*)(void*,void**);
                const auto users=reinterpret_cast<Users>((*reinterpret_cast<void***>(user_statics))[9]);
                using UserString=HRESULT(STDMETHODCALLTYPE*)(void*,HSTRING*);
                const auto offline_id=[&](void* user) {
                    HSTRING value=nullptr;
                    check(reinterpret_cast<UserString>((*reinterpret_cast<void***>(user))[15])(user,&value)==S_OK,
                          "XboxUserId returns the approved emulated offline identity");
                    const auto id=text(value); WindowsDeleteString(value);
                    check(!id.empty() && id.size()<=20 && id.find_first_not_of(L"0123456789")==std::wstring::npos && id!=L"0",
                          "emulated identity preserves the nonzero decimal uint64 string ABI");
                    return id;
                };
                std::wstring first_offline_id;
                const auto previous_root=xboxone::era::title_storage_root();
                wchar_t temporary[MAX_PATH]{}; GetTempPathW(MAX_PATH,temporary);
                const auto root=std::filesystem::path(temporary)/(L"xone-profile-test-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
                const auto second=std::filesystem::path(root.wstring()+L"-second");
                xboxone::era::set_title_storage_root(L"");
                void* missing=reinterpret_cast<void*>(1);
                check(users(user_statics,&missing)==HRESULT_FROM_WIN32(ERROR_NOT_READY) && !missing,
                      "missing profile storage remains explicit rather than returning an invented user or empty success");
                xboxone::era::set_title_storage_root(root.wstring());
                GUID identity{};UINT32 local_id=0;
                check(xboxone::era::title_local_user_identity(&identity,&local_id)==S_OK && identity!=GUID{} && local_id==1,
                      "offline profile persists a genuine local identity in the session storage root");
                const auto profile=root/L".xone-local-profile";
                check(std::filesystem::file_size(profile)==32,"profile publication is bounded to its versioned record");
                IInspectable* view=nullptr;
                check(users(user_statics,reinterpret_cast<void**>(&view))==S_OK && view,"Users returns an owning snapshot of the configured profile");
                if (view) {
                    auto** table=*reinterpret_cast<void***>(view);
                    using Get=HRESULT(STDMETHODCALLTYPE*)(void*,UINT,void**);
                    using Size=HRESULT(STDMETHODCALLTYPE*)(void*,UINT*);
                    using Many=HRESULT(STDMETHODCALLTYPE*)(void*,UINT,UINT,void**,UINT*);
                    using Index=HRESULT(STDMETHODCALLTYPE*)(void*,void*,UINT*,boolean*);
                    IInspectable* item=nullptr; UINT size=0;
                    check(reinterpret_cast<Size>(table[7])(view,&size)==S_OK && size==1,"snapshot count matches the persisted local profile");
                    check(reinterpret_cast<Get>(table[6])(view,0,reinterpret_cast<void**>(&item))==S_OK && item,"snapshot retains its user object");
                    void* invalid=reinterpret_cast<void*>(1);
                    check(reinterpret_cast<Get>(table[6])(view,1,&invalid)==E_BOUNDS && !invalid,"User GetAt rejects an out-of-range index");
                    UINT actual=99; void* values[2]{nullptr,reinterpret_cast<void*>(1)};
                    check(reinterpret_cast<Many>(table[9])(view,0,2,values,&actual)==S_OK && actual==1 && values[0]==item && values[1]==reinterpret_cast<void*>(1),
                          "User GetMany retains exactly the returned items and preserves output guards");
                    if (values[0]) static_cast<IUnknown*>(values[0])->Release();
                    check(reinterpret_cast<Many>(table[9])(view,1,0,nullptr,&actual)==S_OK && actual==0 &&
                          reinterpret_cast<Many>(table[9])(view,2,0,nullptr,&actual)==E_BOUNDS && actual==0,
                          "User GetMany distinguishes the end of a snapshot from invalid bounds");
                    UINT index=99;boolean found=false;
                    check(reinterpret_cast<Index>(table[8])(view,item,&index,&found)==S_OK && found && index==0,"User IndexOf preserves logical identity");
                    if (item) {
                        UINT id=0;auto** it=*reinterpret_cast<void***>(item);
                        check(reinterpret_cast<Size>(it[6])(item,&id)==S_OK && id==local_id,"IUser local ID comes from the shared profile record");
                        using UserBoolean=HRESULT(STDMETHODCALLTYPE*)(void*,boolean*);
                        boolean guest=true;
                        check(reinterpret_cast<UserBoolean>(it[10])(item,&guest)==S_OK && !guest &&
                              reinterpret_cast<UserBoolean>(it[10])(item,nullptr)==E_POINTER,
                              "persisted local profile is not a sponsored guest session");
                        boolean signed_in=false;UINT location=99;void* sponsor=reinterpret_cast<void*>(1);
                        check(reinterpret_cast<UserBoolean>(it[11])(item,&signed_in)==S_OK && signed_in &&
                              reinterpret_cast<Size>(it[12])(item,&location)==S_OK && location==0 &&
                              reinterpret_cast<Users>(it[13])(item,&sponsor)==S_OK && !sponsor,
                              "selected persistent user is locally signed in and has no guest sponsor");
                        first_offline_id=offline_id(item);
                        check(offline_id(item)==first_offline_id && reinterpret_cast<UserString>(it[15])(item,nullptr)==E_POINTER,
                              "offline identity is stable across calls and rejects a null output");
                        using TokenHeaders=HRESULT(STDMETHODCALLTYPE*)(void*,HSTRING,HSTRING,HSTRING,void**);
                        using TokenBytes=HRESULT(STDMETHODCALLTYPE*)(void*,HSTRING,HSTRING,HSTRING,UINT,const BYTE*,void**);
                        using Token=HRESULT(STDMETHODCALLTYPE*)(void*,HSTRING,HSTRING,HSTRING,HSTRING,void**);
                        using PutCompleted=HRESULT(STDMETHODCALLTYPE*)(void*,IUnknown*);
                        using GetResults=HRESULT(STDMETHODCALLTYPE*)(void*,void**);
                        struct TokenHandler : IUnknown {
                            ULONG refs=1;UINT calls=0;INT32 status=-1;bool drop_operation=false;
                            HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out) override {
                                if(!out)return E_POINTER;*out=nullptr;if(iid!=__uuidof(IUnknown))return E_NOINTERFACE;*out=this;AddRef();return S_OK;
                            }
                            ULONG STDMETHODCALLTYPE AddRef() override{return ++refs;}
                            ULONG STDMETHODCALLTYPE Release() override{return --refs;}
                            virtual HRESULT STDMETHODCALLTYPE Invoke(IUnknown* operation,INT32 value) {
                                ++calls;status=value;if(drop_operation)operation->Release();return S_OK;
                            }
                        };
                        constexpr GUID token_iid{0xe6c6e9b1,0x363d,0x54fe,{0x80,0x8b,0x89,0xda,0x6b,0xcf,0x78,0xbf}};
                        for(UINT slot:{16u,17u,18u}) {
                            const auto request=[&](void** out) {
                                if(slot==16)return reinterpret_cast<TokenHeaders>(it[slot])(item,nullptr,nullptr,nullptr,out);
                                if(slot==17)return reinterpret_cast<TokenBytes>(it[slot])(item,nullptr,nullptr,nullptr,0,nullptr,out);
                                return reinterpret_cast<Token>(it[slot])(item,nullptr,nullptr,nullptr,nullptr,out);
                            };
                            struct {IInspectable* value=reinterpret_cast<IInspectable*>(1);UINT64 guard=0x123456789abcdef0;} authentication;
                            check(request(nullptr)==E_POINTER && request(reinterpret_cast<void**>(&authentication.value))==S_OK &&
                                  authentication.value && authentication.guard==0x123456789abcdef0,
                                  "all original token overloads return owned asynchronous offline operations with intact ABI guards");
                            auto* operation=authentication.value;if(!operation || operation==reinterpret_cast<IInspectable*>(1))continue;
                            IInspectable* typed=nullptr;IAsyncInfo* info=nullptr;
                            check(operation->QueryInterface(token_iid,reinterpret_cast<void**>(&typed))==S_OK && typed==operation,
                                  "offline token operation answers the reference's parameterized IID");
                            if(typed)typed->Release();
                            check(operation->QueryInterface(__uuidof(IAsyncInfo),reinterpret_cast<void**>(&info))==S_OK && info,
                                  "offline token operation exposes its real error status");
                            const auto table=*reinterpret_cast<void***>(operation);TokenHandler completion;
                            check(reinterpret_cast<PutCompleted>(table[6])(operation,&completion)==S_OK && completion.calls==1 && completion.status==3 && completion.refs==2,
                                  "offline token completion immediately delivers Error instead of success or a fabricated token");
                            check(reinterpret_cast<PutCompleted>(table[6])(operation,&completion)==E_ILLEGAL_DELEGATE_ASSIGNMENT && completion.calls==1,
                                  "terminal operation rejects a second completion assignment");
                            void* result=reinterpret_cast<void*>(1);
                            check(reinterpret_cast<GetResults>(table[8])(operation,&result)==HRESULT_FROM_WIN32(ERROR_NETWORK_UNREACHABLE) && !result,
                                  "GetResults returns the reference network error and never an authentication result");
                            if(info) {
                                AsyncStatus status=Started;HRESULT error=S_OK;
                                check(info->get_Status(&status)==S_OK && status==Error && info->get_ErrorCode(&error)==S_OK &&
                                      error==HRESULT_FROM_WIN32(ERROR_NETWORK_UNREACHABLE) && info->Cancel()==S_OK &&
                                      info->get_Status(&status)==S_OK && status==Error,
                                      "async error metadata and cancellation preserve the settled offline failure");
                                check(info->Close()==S_OK && reinterpret_cast<GetResults>(table[8])(operation,&result)==E_ILLEGAL_METHOD_CALL && !result,
                                      "closed token operations reject result access without producing a token");
                                info->Release();
                            }
                            operation->Release();check(completion.refs==1,"offline operation retires its retained completion delegate");
                        }
                        void* invalid_body=reinterpret_cast<void*>(1);
                        check(reinterpret_cast<TokenBytes>(it[17])(item,nullptr,nullptr,nullptr,1,nullptr,&invalid_body)==E_INVALIDARG && !invalid_body,
                              "byte-array token overload rejects a missing nonempty body before creating an operation");
                        IInspectable* reentrant=nullptr;
                        if(reinterpret_cast<TokenHeaders>(it[16])(item,nullptr,nullptr,nullptr,reinterpret_cast<void**>(&reentrant))==S_OK && reentrant) {
                            TokenHandler completion;completion.drop_operation=true;
                            check(reinterpret_cast<PutCompleted>((*reinterpret_cast<void***>(reentrant))[6])(reentrant,&completion)==S_OK &&
                                  completion.calls==1 && completion.refs==1,
                                  "completion can release the caller's last operation reference safely during Invoke");
                        } else check(false,"reentrant offline token fixture created");
                        IInspectable* display=nullptr;
                        check(reinterpret_cast<Users>(it[9])(item,reinterpret_cast<void**>(&display))==S_OK && display,
                              "User.DisplayInfo retains the same local profile owner");
                        if (display) {
                            HSTRING label=nullptr;
                            using Name=HRESULT(STDMETHODCALLTYPE*)(void*,HSTRING*);
                            check(reinterpret_cast<Name>((*reinterpret_cast<void***>(display))[6])(display,&label)==S_OK && text(label)==L"Local Player",
                                  "offline display label uses the reference's HSTRING ABI without claiming an online identity");
                            WindowsDeleteString(label);label=nullptr;
                            check(reinterpret_cast<Name>((*reinterpret_cast<void***>(display))[9])(display,&label)==S_OK && text(label)==L"Local Player",
                                  "GameDisplayName slot 9 answers the same local profile name");
                            WindowsDeleteString(label);
                            using Age=HRESULT(STDMETHODCALLTYPE*)(void*,INT32*);
                            INT32 age=-1;
                            check(reinterpret_cast<Age>((*reinterpret_cast<void***>(display))[11])(display,&age)==S_OK && age==0 &&
                                  reinterpret_cast<Age>((*reinterpret_cast<void***>(display))[11])(display,nullptr)==E_POINTER,
                                  "AgeGroup slot 11 reports Unknown for a local profile without recorded age");
                            display->Release();
                        }
                    }
                    constexpr GUID iterable_iid{0x1e1813fd,0xf62e,0x5198,{0xb2,0x07,0x9a,0xa8,0x77,0x14,0x7c,0x08}};
                    IInspectable* iterable=nullptr;
                    check(view->QueryInterface(iterable_iid,reinterpret_cast<void**>(&iterable))==S_OK && iterable,"user snapshot exposes the reference's class-derived iterable IID");
                    if (iterable) {
                        IInspectable* iterator=nullptr;auto** vt=*reinterpret_cast<void***>(iterable);
                        check(reinterpret_cast<Users>(vt[6])(iterable,reinterpret_cast<void**>(&iterator))==S_OK && iterator,"First creates an independently owned iterator");
                        iterable->Release();
                        if (iterator) {
                            auto** iv=*reinterpret_cast<void***>(iterator);IInspectable* current=nullptr;
                            check(reinterpret_cast<Users>(iv[6])(iterator,reinterpret_cast<void**>(&current))==S_OK && current==item,"iterator retains the same profile identity");
                            if (current) current->Release();
                            using Advance=HRESULT(STDMETHODCALLTYPE*)(void*,boolean*);
                            boolean more=true;
                            check(reinterpret_cast<Advance>(iv[8])(iterator,&more)==S_OK && !more &&
                                  reinterpret_cast<Users>(iv[6])(iterator,reinterpret_cast<void**>(&current))==E_BOUNDS && !current,
                                  "iterator end never fabricates another user");
                            iterator->Release();
                        }
                    }
                    xboxone::era::set_title_storage_root(second.wstring());
                    GUID second_identity{};
                    check(xboxone::era::title_local_user_identity(&second_identity,&local_id)==S_OK && second_identity!=identity,
                          "separate session storage cannot alias a different persisted profile");
                    IInspectable* second_view=nullptr;
                    check(users(user_statics,reinterpret_cast<void**>(&second_view))==S_OK && second_view,"second profile snapshot");
                    if (second_view) {
                        IInspectable* second_user=nullptr;
                        check(reinterpret_cast<Get>((*reinterpret_cast<void***>(second_view))[6])(second_view,0,reinterpret_cast<void**>(&second_user))==S_OK && second_user,
                              "second profile user");
                        if (second_user) { check(offline_id(second_user)!=first_offline_id,"different profiles have different emulated identities");second_user->Release(); }
                        second_view->Release();
                    }
                    if (item) {
                        UINT id=0;check(reinterpret_cast<Size>((*reinterpret_cast<void***>(item))[6])(item,&id)==S_OK && id==1,
                            "existing user snapshots remain alive after storage-root replacement");
                        check(offline_id(item)==first_offline_id,"retained user cannot acquire the next session's offline identity");item->Release();
                    }
                    view->Release();
                }
                xboxone::era::set_title_storage_root(root.wstring());GUID reloaded{};
                check(xboxone::era::title_local_user_identity(&reloaded,&local_id)==S_OK && reloaded==identity,
                      "profile identity survives cache reset and reopening its persistent record");
                IInspectable* reloaded_view=nullptr;
                check(users(user_statics,reinterpret_cast<void**>(&reloaded_view))==S_OK && reloaded_view,"reloaded profile snapshot");
                if (reloaded_view) {
                    IInspectable* reloaded_user=nullptr;
                    using Get=HRESULT(STDMETHODCALLTYPE*)(void*,UINT,void**);
                    check(reinterpret_cast<Get>((*reinterpret_cast<void***>(reloaded_view))[6])(reloaded_view,0,reinterpret_cast<void**>(&reloaded_user))==S_OK && reloaded_user,
                          "reloaded profile user");
                    if (reloaded_user) { check(offline_id(reloaded_user)==first_offline_id,"offline identity survives reloading the persisted profile");reloaded_user->Release(); }
                    reloaded_view->Release();
                }
                { std::ofstream corrupt(profile,std::ios::binary|std::ios::trunc);corrupt<<"invalid"; }
                xboxone::era::set_title_storage_root(root.wstring());
                check(users(user_statics,&missing)==HRESULT_FROM_WIN32(ERROR_INVALID_DATA) && !missing && std::filesystem::file_size(profile)==7,
                      "corrupt profile is refused and preserved rather than regenerated over existing identity");
                xboxone::era::set_title_storage_root(previous_root);
                const auto remove_profile=[](const std::filesystem::path& directory) {
                    const auto file=directory/L".xone-local-profile";bool deleted=false;
                    for (UINT attempt=0;attempt<20;++attempt) {
                        if (!deleted) deleted=DeleteFileW(file.c_str())!=FALSE;
                        if (deleted && RemoveDirectoryW(directory.c_str())) return true;
                        Sleep(10); // A scanner can keep a just-deleted file pending briefly.
                    }
                    std::cerr<<"profile fixture cleanup Win32 "<<GetLastError()<<'\n';return false;
                };
                check(remove_profile(root),"profile fixture removes only its owned record and directory");
                const auto second_profile=second/L".xone-local-profile";
                if (std::filesystem::exists(second_profile)) check(remove_profile(second),"second profile fixture removes its own record");
            }
            using Add = HRESULT(STDMETHODCALLTYPE*)(void*, IUnknown*, std::int64_t*);
            using Remove = HRESULT(STDMETHODCALLTYPE*)(void*, std::int64_t);
            auto** vt = *reinterpret_cast<void***>(user_statics);
            HSTRING value = nullptr;
            check(user_statics->GetRuntimeClassName(&value) == S_OK && text(value) == L"Windows.Xbox.System.User",
                  "User runtime class name");
            std::int64_t token = 0;
            check(reinterpret_cast<Add>(vt[12])(user_statics, host, &token) == S_OK && token &&
                  xboxone::era::xbox_user_removed_handlers() == 1, "UserRemoved handler stored");
            check(reinterpret_cast<Add>(vt[12])(user_statics, nullptr, &token) == E_INVALIDARG, "null handler rejected");
            check(reinterpret_cast<Remove>(vt[13])(user_statics, token) == S_OK &&
                  xboxone::era::xbox_user_removed_handlers() == 0, "UserRemoved handler removed");
        }
        // DownloadableContentPackageManager: no DLC on this host, FindPackages is empty.
        constexpr GUID vector_view_string{0x2f13c006, 0xa03a, 0x5f69, {0xb0, 0x90, 0x75, 0xa4, 0x3e, 0x33, 0x42, 0x3e}};
        check(xboxone::era::winrt_pinterface_iid("pinterface({bbe1fa4c-b0e3-4583-baef-1f1b2e483e56};string)") ==
                  vector_view_string, "pinterface IID of IVectorView<String>");
        constexpr GUID iid_dlc_manager{0x971FEAAF, 0x98F8, 0x4038, {0xB8, 0xC1, 0x3A, 0x5F, 0x1A, 0x04, 0x3B, 0x97}};
        IActivationFactory* dlc = nullptr;
        check(activate(L"Windows.Xbox.Management.Deployment.DownloadableContentPackageManager", __uuidof(IActivationFactory),
                       reinterpret_cast<void**>(&dlc)) == S_OK && dlc, "DLC manager served by title layer");
        IInspectable* dlc_instance = nullptr;
        IInspectable* manager = nullptr;
        if (dlc) check(dlc->ActivateInstance(&dlc_instance) == S_OK && dlc_instance, "DLC manager instance");
        if (dlc_instance)
            check(dlc_instance->QueryInterface(iid_dlc_manager, reinterpret_cast<void**>(&manager)) == S_OK && manager,
                  "DLC manager default interface");
        if (manager && host) {
            auto** vt = *reinterpret_cast<void***>(manager);
            using Find = HRESULT(STDMETHODCALLTYPE*)(void*, INT32, IInspectable**);
            using Size = HRESULT(STDMETHODCALLTYPE*)(void*, UINT32*);
            using GetAt = HRESULT(STDMETHODCALLTYPE*)(void*, UINT32, void**);
            using Add = HRESULT(STDMETHODCALLTYPE*)(void*, IUnknown*, std::int64_t*);
            using Remove = HRESULT(STDMETHODCALLTYPE*)(void*, std::int64_t);
            IInspectable* packages = nullptr;
            check(reinterpret_cast<Find>(vt[6])(manager, 0, &packages) == S_OK && packages, "FindPackages returns a view");
            if (packages) {
                auto** pv = *reinterpret_cast<void***>(packages);
                UINT32 size = 7;
                void* item = &item;
                check(reinterpret_cast<Size>(pv[7])(packages, &size) == S_OK && size == 0, "no DLC packages");
                check(reinterpret_cast<GetAt>(pv[6])(packages, 0, &item) == E_BOUNDS && !item, "empty view is bounded");
                IUnknown* typed = nullptr;
                check(packages->QueryInterface(xboxone::era::winrt_pinterface_iid(xboxone::era::dlc_package_view_signature),
                                               reinterpret_cast<void**>(&typed)) == S_OK && typed == packages,
                      "view answers IVectorView<DownloadableContentPackage>");
            }
            std::int64_t token = 0;
            check(reinterpret_cast<Add>(vt[7])(manager, host, &token) == S_OK && token &&
                  reinterpret_cast<Remove>(vt[8])(manager, token) == S_OK, "install-completed event registration");
        }
        if (manager) manager->Release();
        if (dlc_instance) dlc_instance->Release();
        if (dlc) dlc->Release();
        // CoreApplication: the title layer drives the view lifecycle in contract order.
        {
            static std::string lifecycle;
            static std::wstring loaded;
            static bool fail_load = false;
            static HWND owned_window = nullptr;
            static IInspectable* runtime_window=nullptr;
            static UINT visibility_changes=0;
            static bool last_visibility=false;
            static IInspectable* retained_visibility=nullptr;
            static INT32 activation_state=-1;
            static UINT key_events=0;
            static bool expected_key_up=false;
            struct WindowActivationHandler : IUnknown {
                HRESULT STDMETHODCALLTYPE QueryInterface(REFIID,void** out) override {*out=this;return S_OK;}
                ULONG STDMETHODCALLTYPE AddRef() override {return 2;}
                ULONG STDMETHODCALLTYPE Release() override {return 1;}
                virtual HRESULT STDMETHODCALLTYPE Invoke(IInspectable*,IInspectable* args) {
                    const auto get=reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*,INT32*)>((*reinterpret_cast<void***>(args))[6]);
                    check(get(args,nullptr)==E_POINTER && get(args,&activation_state)==S_OK,"window activation args expose the native activation state");
                    return S_OK;
                }
            };
            static WindowActivationHandler window_activation_handler;
            struct KeyHandler : IUnknown {
                HRESULT STDMETHODCALLTYPE QueryInterface(REFIID,void** out) override {*out=this;return S_OK;}
                ULONG STDMETHODCALLTYPE AddRef() override {return 2;}
                ULONG STDMETHODCALLTYPE Release() override {return 1;}
                virtual HRESULT STDMETHODCALLTYPE Invoke(IInspectable*,IInspectable* args) {
                    const auto table=*reinterpret_cast<void***>(args);
                    INT32 key=0;
                    struct Status {UINT32 repeat,scan;boolean extended,menu,was_down,released;} status{};
                    const auto get_key=reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*,INT32*)>(table[6]);
                    const auto get_status=reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*,Status*)>(table[7]);
                    check(get_key(args,nullptr)==E_POINTER && get_status(args,nullptr)==E_POINTER && get_key(args,&key)==S_OK &&
                        get_status(args,&status)==S_OK && key=='A' && status.scan==0x1e && status.was_down &&
                        status.repeat==(expected_key_up ? 1u : 2u) && bool(status.released)==expected_key_up,
                        "CoreWindow keyboard events preserve virtual key, scan code, repetition and release bits");
                    ++key_events;return S_OK;
                }
            };
            static KeyHandler key_handler;
            struct VisibilityHandler : IUnknown {
                HRESULT STDMETHODCALLTYPE QueryInterface(REFIID,void** out) override {*out=this;return S_OK;}
                ULONG STDMETHODCALLTYPE AddRef() override {return 2;}
                ULONG STDMETHODCALLTYPE Release() override {return 1;}
                virtual HRESULT STDMETHODCALLTYPE Invoke(IInspectable*,IInspectable* args) {
                    boolean value=FALSE;
                    const auto get=reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*,boolean*)>((*reinterpret_cast<void***>(args))[6]);
                    check(get(args,nullptr)==E_POINTER && get(args,&value)==S_OK,"visibility event exposes its real snapshot");
                    last_visibility=value!=FALSE;++visibility_changes;
                    if(value && visibility_changes==1){retained_visibility=args;args->AddRef();}
                    return S_OK;
                }
            };
            static VisibilityHandler visibility_handler;
            struct Handler : IUnknown {
                HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override { *out = this; return S_OK; }
                ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
                ULONG STDMETHODCALLTYPE Release() override { return 1; }
                virtual HRESULT STDMETHODCALLTYPE Invoke(IInspectable* sender, IInspectable* args) {
                    lifecycle += sender && args ? "A" : "a";
                    if (args) {
                        using Kind = HRESULT(STDMETHODCALLTYPE*)(void*, INT32*);
                        const auto kind = reinterpret_cast<Kind>((*reinterpret_cast<void***>(args))[6]);
                        INT32 value = -1;
                        check(kind(args, nullptr) == E_POINTER, "activation kind validates its output");
                        check(kind(args, &value) == S_OK && value == 0, "Run dispatches an actual Launch activation");
                        check(reinterpret_cast<Kind>((*reinterpret_cast<void***>(args))[7])(args,&value)==S_OK && value==0,
                              "fresh process reports NotRunning rather than a fabricated resumed state");
                        constexpr GUID launch_iid{0xfbc93e26,0xa14a,0x4b4f,{0x82,0xb0,0x33,0xbe,0xd9,0x20,0xaf,0x52}};
                        IInspectable* launch=nullptr;
                        check(args->QueryInterface(launch_iid,reinterpret_cast<void**>(&launch))==S_OK && launch,
                              "LaunchActivatedEventArgs exposes its standard launch interface");
                        if(launch) {
                            IUnknown *first=nullptr,*second=nullptr;
                            args->QueryInterface(__uuidof(IUnknown),reinterpret_cast<void**>(&first));
                            launch->QueryInterface(__uuidof(IUnknown),reinterpret_cast<void**>(&second));
                            check(first && first==second,"launch interfaces share one COM identity");
                            if(first)first->Release();if(second)second->Release();
                            for(UINT slot:{6u,7u}) {
                                const auto get=reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*,HSTRING*)>((*reinterpret_cast<void***>(launch))[slot]);
                                HSTRING text=nullptr;
                                check(get(launch,nullptr)==E_POINTER && get(launch,&text)==S_OK && WindowsGetStringLen(text)==0,
                                      "ordinary launch has no extra guest arguments or pinned-tile ID");
                                WindowsDeleteString(text);
                            }
                            launch->Release();
                        }
                    }
                    return S_OK;
                }
            };
            static Handler handler;
            struct View : IInspectable {
                HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override { *out = this; return S_OK; }
                ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
                ULONG STDMETHODCALLTYPE Release() override { return 1; }
                HRESULT STDMETHODCALLTYPE GetIids(ULONG*, IID**) override { return E_NOTIMPL; }
                HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING*) override { return E_NOTIMPL; }
                HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel*) override { return E_NOTIMPL; }
                virtual HRESULT STDMETHODCALLTYPE Initialize(IInspectable* view) {
                    std::int64_t token = 0;
                    auto** vt = *reinterpret_cast<void***>(view);
                    lifecycle += "I";
                    return reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*, IUnknown*, std::int64_t*)>(vt[7])(view, &handler, &token);
                }
                virtual HRESULT STDMETHODCALLTYPE SetWindow(IInspectable* window) {
                    runtime_window=window;
                    owned_window = core_window_hwnd(window);
                    visibility_changes=0;last_visibility=false;
                    std::int64_t visibility_token=0;
                    check(reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*,IUnknown*,std::int64_t*)>((*reinterpret_cast<void***>(window))[35])(
                        window,&visibility_handler,&visibility_token)==S_OK,"CoreWindow visibility registration uses the Xbox slots");
                    const auto table=*reinterpret_cast<void***>(window);
                    using AddWindowEvent=HRESULT(STDMETHODCALLTYPE*)(void*,IUnknown*,std::int64_t*);
                    check(reinterpret_cast<AddWindowEvent>(table[15])(window,&window_activation_handler,&visibility_token)==S_OK &&
                        reinterpret_cast<AddWindowEvent>(table[23])(window,&key_handler,&visibility_token)==S_OK &&
                        reinterpret_cast<AddWindowEvent>(table[25])(window,&key_handler,&visibility_token)==S_OK,
                        "activation and keyboard event registrations use their observed Xbox slots");
                    check(!IsWindowVisible(core_window_hwnd(window)), "window stays hidden during view setup");
                    IAgileObject* agile=nullptr;
                    check(window->QueryInterface(__uuidof(IAgileObject),reinterpret_cast<void**>(&agile))==S_OK && agile,
                        "CoreWindow exposes the standard COM agility marker without inventing an Xbox interface");
                    if(agile) {
                        std::thread worker([&] {
                            constexpr GUID iid_window{0x79b9d5f2,0x879e,0x4b89,{0xb7,0x98,0x79,0xe4,0x75,0x98,0x03,0x0c}};
                            IInspectable* same=nullptr;
                            check(agile->QueryInterface(iid_window,reinterpret_cast<void**>(&same))==S_OK && same==window,
                                "agile window queries preserve the one session identity across threads");
                            if(same) {
                                auto** table=*reinterpret_cast<void***>(same);
                                check(reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*)>(table[11])(same)==RPC_E_WRONG_THREAD,
                                    "the agility marker does not remove Activate thread affinity");
                                same->Release();
                            }
                        });worker.join();agile->Release();
                    }
                    check(core_window_hwnd(nullptr) && core_window_hwnd(nullptr) == core_window_hwnd(window),
                        "Xbox null-window swapchain binding selects the same owned local window");
                    constexpr GUID iid_window_static{0x4d239005,0x3c2a,0x41b1,{0x90,0x22,0x53,0x6b,0xb9,0xcf,0x93,0xb1}};
                    IInspectable* statics = nullptr;
                    check(core_window_factory(iid_window_static,reinterpret_cast<void**>(&statics)) == S_OK && statics,
                        "CoreWindow static factory is served by the same runtime");
                    if (statics) {
                        using Current = HRESULT(STDMETHODCALLTYPE*)(void*,IInspectable**);
                        const auto get = reinterpret_cast<Current>((*reinterpret_cast<void***>(statics))[6]);
                        IInspectable* current = nullptr;
                        check(get(statics,nullptr) == E_POINTER && get(statics,&current) == S_OK && current == window,
                            "GetForCurrentThread returns the exact SetWindow identity");
                        if (current) current->Release();
                        std::thread foreign([&] {
                            check(!core_window_hwnd(nullptr), "null-window binding never selects another thread's window");
                            IInspectable* other = reinterpret_cast<IInspectable*>(1);
                            check(get(statics,&other) == S_OK && !other, "foreign thread has no runtime CoreWindow");
                        });
                        foreign.join(); statics->Release();
                    }
                    HSTRING name = nullptr;
                    window->GetRuntimeClassName(&name);
                    lifecycle += std::wstring(WindowsGetStringRawBuffer(name, nullptr)) == L"Windows.UI.Core.CoreWindow" ? "S" : "s";
                    WindowsDeleteString(name);
                    std::int64_t token = 0;
                    auto** vt = *reinterpret_cast<void***>(window);
                    IInspectable* dispatcher = nullptr;
                    using GetDispatcher = HRESULT (STDMETHODCALLTYPE*)(void*, IInspectable**);
                    check(reinterpret_cast<GetDispatcher>(vt[7])(window, nullptr) == E_POINTER, "dispatcher output required");
                    check(reinterpret_cast<GetDispatcher>(vt[7])(window, &dispatcher) == S_OK && dispatcher, "window dispatcher");
                    if (dispatcher) {
                        const auto dt = *reinterpret_cast<void***>(dispatcher);
                        using Access = HRESULT (STDMETHODCALLTYPE*)(void*, boolean*);
                        using Pump = HRESULT (STDMETHODCALLTYPE*)(void*, INT32);
                        const auto access = reinterpret_cast<Access>(dt[6]);
                        const auto pump = reinterpret_cast<Pump>(dt[7]);
                        boolean owns = false;
                        check(access(dispatcher, &owns) == S_OK && owns, "dispatcher owns window thread");
                        check(pump(dispatcher, 4) == E_INVALIDARG, "unknown dispatcher option rejected");
                        std::thread other([&] {
                            boolean owns = true;
                            check(access(dispatcher, &owns) == S_OK && !owns && pump(dispatcher, 3) == RPC_E_WRONG_THREAD,
                                  "dispatcher rejects pumping from another thread");
                        });
                        other.join();
                        check(pump(dispatcher, 3) == S_OK, "drain initial window messages");
                        PostThreadMessageW(GetCurrentThreadId(), WM_APP, 1, 0);
                        PostThreadMessageW(GetCurrentThreadId(), WM_APP, 2, 0);
                        check(pump(dispatcher, 1) == S_OK, "process one queued message");
                        MSG pending{};
                        check(PeekMessageW(&pending, nullptr, WM_APP, WM_APP, PM_NOREMOVE) && pending.wParam == 2,
                              "process one leaves the second message pending");
                        check(pump(dispatcher, 0) == S_OK && !PeekMessageW(&pending, nullptr, WM_APP, WM_APP, PM_NOREMOVE),
                              "process one and remaining queued messages");
                        dispatcher->Release();
                    }
                    return reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*, IUnknown*, std::int64_t*)>(vt[19])(window, &handler, &token);
                }
                virtual HRESULT STDMETHODCALLTYPE Load(HSTRING entry) {
                    if (fail_load) { lifecycle += "l"; return E_ABORT; }
                    loaded = WindowsGetStringRawBuffer(entry, nullptr);
                    lifecycle += "L";
                    return S_OK;
                }
                virtual HRESULT STDMETHODCALLTYPE Run() {
                    IInspectable* dispatcher=nullptr;
                    check(reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*,IInspectable**)>(
                        (*reinterpret_cast<void***>(runtime_window))[7])(runtime_window,&dispatcher)==S_OK && dispatcher,
                        "Run uses the same window dispatcher");
                    const auto pump=reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*,INT32)>(
                        (*reinterpret_cast<void***>(dispatcher))[7]);
                    check(IsWindowVisible(core_window_hwnd(nullptr)), "session shows the title before Run without a guest Activate call");
                    check(visibility_changes==0,"showing the window does not reenter title initialization");
                    check(pump(dispatcher,3)==S_OK && visibility_changes==1 && last_visibility,"dispatcher publishes the real initial visibility");
                    ShowWindow(owned_window,SW_HIDE);
                    check(visibility_changes==1 && pump(dispatcher,3)==S_OK && visibility_changes==2 && !last_visibility,"hiding the real CoreWindow queues its visibility snapshot");
                    if(retained_visibility) {
                        boolean value=FALSE;
                        const auto get=reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*,boolean*)>((*reinterpret_cast<void***>(retained_visibility))[6]);
                        check(get(retained_visibility,&value)==S_OK && value,"retained visibility args preserve their earlier snapshot after Hide");
                        retained_visibility->Release();retained_visibility=nullptr;
                    }
                    ShowWindow(owned_window,SW_SHOW);
                    check(pump(dispatcher,3)==S_OK,"show notification retires before activation checks");
                    SendMessageW(owned_window,WM_ACTIVATE,WA_INACTIVE,0);
                    check(pump(dispatcher,3)==S_OK && activation_state==1,"native deactivation message reaches CoreWindow through its dispatcher");
                    SendMessageW(owned_window,WM_ACTIVATE,WA_ACTIVE,0);
                    check(pump(dispatcher,3)==S_OK && activation_state==0,"native activation message reaches CoreWindow through its dispatcher");
                    key_events=0;expected_key_up=false;
                    SendMessageW(owned_window,WM_KEYDOWN,'A',2|(0x1eLL<<16)|(1LL<<30));
                    expected_key_up=true;
                    SendMessageW(owned_window,WM_KEYUP,'A',1|(0x1eLL<<16)|(1LL<<30)|(1LL<<31));
                    check(key_events==2,"both native key messages reach the registered title handlers");
                    PostQuitMessage(0);
                    check(pump(dispatcher,2)==S_OK,"process until quit consumes real quit event");
                    dispatcher->Release();
                    lifecycle += "R"; return S_OK;
                }
                virtual HRESULT STDMETHODCALLTYPE Uninitialize() { lifecycle += "U"; return S_OK; }
            };
            static View fake_view;
            // IFrameworkViewSource: CreateView at slot 6.
            struct SourceShim : IInspectable {
                HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override { *out = this; return S_OK; }
                ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
                ULONG STDMETHODCALLTYPE Release() override { return 1; }
                HRESULT STDMETHODCALLTYPE GetIids(ULONG*, IID**) override { return E_NOTIMPL; }
                HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING*) override { return E_NOTIMPL; }
                HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel*) override { return E_NOTIMPL; }
                virtual HRESULT STDMETHODCALLTYPE CreateView(IInspectable** out) { *out = &fake_view; return S_OK; }
            };
            static SourceShim source;
            constexpr GUID iid_core_application{0x0aacf7a4, 0x5e1d, 0x49df, {0x80, 0x34, 0xfb, 0x6a, 0x68, 0xbc, 0x5e, 0xd1}};
            constexpr GUID iid_resources{0x9ff3799e, 0x6e83, 0x468c, {0xa5, 0xb0, 0x50, 0x1c, 0xe6, 0xbb, 0x0f, 0xa1}};
            IActivationFactory* core = nullptr;
            check(activate(L"Windows.ApplicationModel.Core.CoreApplication", __uuidof(IActivationFactory),
                           reinterpret_cast<void**>(&core)) == S_OK && core, "CoreApplication served by title layer");
            IInspectable* app = nullptr;
            IInspectable* resources = nullptr;
            if (core) {
                constexpr GUID iid_gpu_policy{0x42a6a529,0x3cc6,0x4867,{0xb5,0xb7,0x6d,0x1f,0xaf,0x53,0x25,0xf9}};
                IInspectable* policy=nullptr;
                check(core->QueryInterface(iid_gpu_policy,reinterpret_cast<void**>(&policy))==S_OK && policy,
                      "CoreApplication exposes the requested GPU reservation policy");
                if(policy) {
                    const auto table=*reinterpret_cast<void***>(policy);
                    const auto get=reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*,boolean*)>(table[6]);
                    const auto set=reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*,boolean)>(table[7]);
                    boolean disabled=FALSE;
                    check(get(policy,nullptr)==E_POINTER && set(policy,TRUE)==S_OK && get(policy,&disabled)==S_OK && disabled &&
                        set(policy,FALSE)==S_OK && get(policy,&disabled)==S_OK && !disabled,
                        "Kinect reservation policy retains both requested values without asserting physical processing");
                    policy->Release();
                }
                check(core->QueryInterface(iid_core_application, reinterpret_cast<void**>(&app)) == S_OK && app, "ICoreApplication");
                check(core->QueryInterface(iid_resources, reinterpret_cast<void**>(&resources)) == S_OK && resources,
                      "Xbox resource availability interface");
            }
            if (app) {
                auto** vt = *reinterpret_cast<void***>(app);
                std::int64_t token = 0;
                check(reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*, IUnknown*, std::int64_t*)>(vt[7])(app, &handler, &token) == S_OK &&
                      reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*, std::int64_t)>(vt[8])(app, token) == S_OK,
                      "Suspending registration");
                const HRESULT hr = reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*, IUnknown*)>(vt[13])(app, &source);
                check(hr == S_OK, "CoreApplication::Run succeeds");
                check(lifecycle == "ISLARU", "view lifecycle Initialize, SetWindow, Load, Activated, Run, Uninitialize");
                check(loaded == L"Title.App", "Load receives the manifest entry point");
                check(!IsWindow(owned_window), "window destroyed after Run");
                lifecycle.clear();
                fail_load = true;
                check(reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*, IUnknown*)>(vt[13])(app, &source) == E_ABORT &&
                          lifecycle == "ISlU",
                      "failed Load returns its HRESULT and still uninitializes");
                check(!IsWindow(owned_window), "window destroyed after a failed Run");
                fail_load = false;
                check(reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*, IUnknown*)>(vt[13])(app, nullptr) == E_INVALIDARG,
                      "Run without a view source");
            }
            if (resources) resources->Release();
            if (app) app->Release();
            if (core) core->Release();
        }
        if (user_statics) user_statics->Release();
        if (user) user->Release();
        if (host) host->Release();
        DeleteFileW(manifest.c_str());
        RemoveDirectoryW(package.c_str());
        RoUninitialize();
    }
    std::cout << (failures ? "era_loader: FAILED\n" : "era_loader: all checks passed\n");
    return failures ? 1 : 0;
}
