// SPDX-License-Identifier: BSD-3-Clause
// d3d11_x.dll: D3D11.X device objects with the title-visible Xbox layout,
// translated onto Direct3D 12. Layout facts (IGraphicsUnknown data and IIDs)
// follow WinDurango's public interface definitions; the implementation is ours.
//
// The original D3D11CreateDevice route describes combase 10.0.14393.2152
// up to (excluding) 10.0.19041.3453, where every object starts with
//   [vtable][ULONG DeviceIndex:3, PrivateDataPresent:1, Reserved:28][ULONG RefCount]
// Titles may touch RefCount inline, so it is a plain field, not hidden state.
// Every vtable slot not implemented yet traps and reports interface + slot.
// D3D11XCreateDeviceX additionally supports the explicitly bound 6.2.11295.0
// prefix (RefCount at +8); it never aliases the modern object prefix.
#include "xboxone_native/era_loader.h"
#include "era_winrt_classes.h"
#include "era_memory.h"
#include "era_display.h"
#include "file_path.h"
#include "sha256.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>
#include <d3d12.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <d3dcompiler.h>
#include <d3d11shader.h>
#include <wrl/client.h>
#include <wrl/wrappers/corewrappers.h>

#include <array>
#include <bit>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <utility>
#include <type_traits>
#include <mutex>
#include <memory>
#include <cstring>
#include <string>
#include <map>
#include <cmath>

namespace xboxone::era {
namespace {

constexpr GUID iid_graphics_unknown{0xACEEEA63, 0xE0A9, 0x4A1C, {0xBB, 0xEC, 0x71, 0xB2, 0xF4, 0x85, 0xF7, 0x58}};
constexpr GUID iid_unknown{0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}};
constexpr GUID iid_device{0xDB6F6DDB, 0xAC77, 0x4E88, {0x82, 0x53, 0x81, 0x9D, 0xF9, 0xBB, 0xF1, 0x40}};
constexpr GUID iid_device_x{0x177700F9, 0x876A, 0x4436, {0xB3, 0x68, 0x36, 0xA6, 0x04, 0xF8, 0x2C, 0xEF}};
constexpr GUID iid_performance_device{0x88671610, 0x712E, 0x4F1E, {0x84, 0xAB, 0x01, 0xB5, 0x94, 0x8B, 0xD3, 0x73}};
constexpr GUID iid_context{0xC0BFA96C, 0xE089, 0x44FB, {0x8E, 0xAF, 0x26, 0xF8, 0x79, 0x61, 0x90, 0xDA}};
constexpr GUID iid_context_x{0x48800095, 0x7134, 0x4BE7, {0x91, 0x86, 0xB8, 0x6B, 0xEC, 0xB2, 0x64, 0x77}};
constexpr GUID iid_performance_context{0x9458FE06, 0xC78D, 0x47F7, {0x96, 0xA0, 0xEC, 0x7B, 0x72, 0x7B, 0xE1, 0xE9}};
constexpr GUID iid_dma{0xA6332DDB, 0x8E02, 0x427D, {0xB0, 0xB7, 0x34, 0xA1, 0xE6, 0x1A, 0x64, 0x88}};
constexpr GUID iid_device_child{0x1841E5C8, 0x16B0, 0x489B, {0xBC, 0xC8, 0x44, 0xCF, 0xB0, 0xD5, 0xDE, 0xAE}};

// Xbox DXGI (ABI >= 6.2.13004): IDXGIObject 3 SetPrivateData, 4 SetPrivateDataInterface,
// 5 GetPrivateData, 6 GetParent; IDXGIDevice 7 GetAdapter (observed: the title queries
// IDXGIDevice1 from the device and calls slot 7).
constexpr GUID iid_dxgi_object{0xAEC22FB8, 0x76F3, 0x4639, {0x9B, 0xE0, 0x28, 0xEB, 0x43, 0xA6, 0x7A, 0x2E}};
constexpr GUID iid_dxgi_device{0x54EC77FA, 0x1377, 0x44E6, {0x8C, 0x32, 0x88, 0xFD, 0x5F, 0x44, 0xC8, 0x4C}};
constexpr GUID iid_dxgi_device1{0x77DB970F, 0x6276, 0x48BA, {0xBA, 0x28, 0x07, 0x01, 0x43, 0xB4, 0x39, 0x2C}};
// Xbox IDXGIFactory2 (title asks the adapter's parent for the desktop IID): 7 EnumAdapters,
// 8 MakeWindowAssociation, 9 GetWindowAssociation, 10 CreateSwapChain, 11 CreateSoftwareAdapter,
// 12 EnumAdapters1, 13 IsCurrent, 14 IsWindowedStereoEnabled, 15 CreateSwapChainForHwnd,
// 16 CreateSwapChainForCoreWindow.
constexpr GUID iid_dxgi_factory2{0x50C83A1C, 0xE072, 0x4C48, {0x87, 0xB0, 0x36, 0x30, 0xFA, 0x36, 0xA6, 0xD0}};
// Desktop IDXGISwapChain1 IID; replaced by the title's observed IID if it differs.
constexpr GUID iid_dxgi_swapchain1{0x790A45F7, 0x0D42, 0x4876, {0x98, 0x3A, 0x0A, 0x55, 0xCF, 0xE6, 0xF4, 0xAA}};
constexpr GUID iid_dxgi_adapter{0x2411E7E1, 0x12AC, 0x4CCF, {0xBD, 0x14, 0x97, 0x98, 0xE8, 0x53, 0x4D, 0xC0}};

constexpr std::size_t vtable_slots = 512; // Above any D3D11.X interface; extra slots only trap.

struct GraphicsObject {
    const void* const* vtable;
    ULONG flags;    // DeviceIndex:3, PrivateDataPresent:1, Reserved:28 (zero here).
    ULONG refcount; // Title-visible.
};
static_assert(offsetof(GraphicsObject, flags) == 8 && offsetof(GraphicsObject, refcount) == 12);

struct Context;
struct DepthState;
struct Sampler;
struct BlendState;
struct RasterState;
struct DmaEngine {
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> queue;
    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    std::atomic<UINT64> issued{0}, submitted{0};
    bool claimed = false; // Device ownership mutex protects engine binding.
};
struct Device : GraphicsObject {
    ID3D12Device* d3d12 = nullptr;
    ID3D12CommandQueue* queue = nullptr;
    D3D_FEATURE_LEVEL level = D3D_FEATURE_LEVEL_11_0;
    UINT creation_flags = 0;
    bool legacy = false;
    ID3D12Fence* retirement = nullptr;
    HANDLE retired = nullptr;
    std::mutex ownership;
    ULONG legacy_dependents = 0; // Keeps storage alive without altering the public device count.
    Context* immediate = nullptr; // Legacy context storage belongs to the device.
    std::array<std::shared_ptr<DmaEngine>, 3> dma; // The observed engine selectors are 1 and 2.
    std::map<std::array<UINT32, 13>, DepthState*> depth_states; // Live immutable states, not extra references.
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> sampler_heap;
    std::array<Sampler*, D3D11_REQ_SAMPLER_OBJECT_COUNT_PER_DEVICE> sampler_slots{};
    std::map<std::array<UINT32, 13>, Sampler*> samplers;
    std::map<std::array<UINT32, 66>, BlendState*> blend_states;
    std::map<std::array<UINT32, 10>, RasterState*> raster_states;
};
struct alignas(64) Context : GraphicsObject {
    Device* device = nullptr; // Owns one device reference.
    void* private_data = nullptr;
    // 6.2.11294+ public data: TinyDevice (0x148), shader user data (0x228),
    // then 0x40 bytes of out-of-line dispatch flags/reserved state.
    std::array<std::uint8_t, 0x3b0> inline_state{};
    std::array<const void*, vtable_slots> functions{};
    bool deferred = false;
    bool automatic_gpu_flush = false;
};
static_assert(offsetof(Context, private_data) == 24 && offsetof(Context, functions) == 0x3d0);
struct DmaOperation { ID3D12Fence* fence; UINT64 value; bool wait; };
struct Shader : GraphicsObject {
    Device* device = nullptr;
    void* private_data = nullptr;
    UINT native_flags = 0; // Original GS factory 180029ef0 clears public +0x20.
    UINT stage = 0;
    // D3D12 consumes this original DXBC when the complete pipeline is created.
    std::vector<std::uint8_t> code;
};
static_assert(offsetof(Shader, device) == 16 && offsetof(Shader, private_data) == 24 && offsetof(Shader, native_flags) == 32);
struct InputLayout : GraphicsObject {
    Device* device = nullptr;
    void* private_data = nullptr;
    std::vector<std::string> semantics;
    std::vector<D3D12_INPUT_ELEMENT_DESC> elements;
    std::vector<std::uint8_t> signature;
};
struct DepthState : GraphicsObject {
    Device* device = nullptr;
    void* private_data = nullptr;
    D3D12_DEPTH_STENCIL_DESC desc{};
    std::array<UINT32, 13> key{};
};
static_assert(sizeof(D3D12_DEPTH_STENCIL_DESC) == 52);
struct alignas(16) Sampler : GraphicsObject {
    Device* device = nullptr;
    void* private_data = nullptr;
    std::array<UINT32, 4> descriptor{};
    D3D12_SAMPLER_DESC desc{};
    UINT slot = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE host{};
    std::array<UINT32, 13> key{};
};
static_assert(offsetof(Sampler, descriptor) == 32 && offsetof(Sampler, desc) == 48);
struct BlendState : GraphicsObject {
    Device* device = nullptr;
    void* private_data = nullptr;
    D3D11_BLEND_DESC desc{};
    D3D12_BLEND_DESC host{};
    std::array<UINT32, 66> key{};
};
static_assert(sizeof(D3D11_BLEND_DESC) == 264);
struct RasterState : GraphicsObject {
    Device* device = nullptr;
    void* private_data = nullptr;
    D3D11_RASTERIZER_DESC desc{};
    D3D12_RASTERIZER_DESC host{};
    std::array<UINT32, 10> key{};
};
static_assert(sizeof(D3D11_RASTERIZER_DESC) == 40);
struct LegacyBufferDesc {
    D3D11_BUFFER_DESC standard;
    UINT64 extension;
};
struct LegacyTexture2DDesc {
    D3D11_TEXTURE2D_DESC standard;
    std::array<UINT, 2> extension{};
};
struct LegacyTexture3DDesc {
    D3D11_TEXTURE3D_DESC standard;
    std::array<UINT, 2> extension{};
};
static_assert(sizeof(LegacyTexture3DDesc) == 44);
static_assert(sizeof(LegacyTexture2DDesc) == 52);
static_assert(sizeof(LegacyBufferDesc) == 32);
struct alignas(16) Buffer : GraphicsObject {
    Device* device = nullptr;
    void* private_data = nullptr;
    std::array<UINT32, 4> descriptor{};
    void* allocation_start = nullptr;
    std::array<UINT32, 2> native_tags{0xffffffff, 0xffff};
    LegacyBufferDesc desc{};
    TitleGpuResource gpu;
    Microsoft::WRL::ComPtr<ID3D12Fence> creation_fence;
    ~Buffer();
};
static_assert(offsetof(Buffer, descriptor) == 32 && offsetof(Buffer, allocation_start) == 48);
struct alignas(16) Texture : GraphicsObject {
    Device* device = nullptr;
    void* private_data = nullptr;
    std::array<UINT32, 4> buffer_only_descriptor{};
    void* buffer_only_allocation = nullptr;
    std::array<UINT32, 2> native_tags{0xffffffff, 0xffff};
    LegacyTexture2DDesc desc{};
    LegacyTexture3DDesc volume_desc{};
    bool volume = false;
    UINT creation_misc_flags = 0;
    UINT64 allocation_attributes = 0; // Zero for borrowed placement storage.
    void* canonical = nullptr; // Borrowed placement memory; the registry pins its backing.
    std::array<UINT32, 8> descriptor{};
    TitleGpuResource gpu;
    Microsoft::WRL::ComPtr<ID3D12Fence> creation_fence;
    ~Texture();
};
static_assert(offsetof(Texture, device) == 16 && offsetof(Texture, native_tags) == 56);
struct alignas(16) BufferView : GraphicsObject {
    Device* device = nullptr;
    void* private_data = nullptr;
    GraphicsObject* resource = nullptr;
    UINT view_type = 3, reserved = 0;
    std::array<UINT32, 8> descriptor{};
    void* allocation_start = nullptr;
    D3D11_UNORDERED_ACCESS_VIEW_DESC desc{};
    D3D11_SHADER_RESOURCE_VIEW_DESC srv_desc{}; // Same native view prefix, type 2.
    D3D11_RENDER_TARGET_VIEW_DESC rtv_desc{}; // Type 0 has no public SRD payload.
    Texture* metadata_texture = nullptr; // Derived FMASK for the real expanded sample topology.
    Buffer* counter_buffer = nullptr;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> heap;
    D3D12_CPU_DESCRIPTOR_HANDLE host{};
    ~BufferView();
};
static_assert(offsetof(BufferView, resource) == 32 && offsetof(BufferView, view_type) == 40 &&
              offsetof(BufferView, descriptor) == 48 && offsetof(BufferView, allocation_start) == 80);
struct Dma : GraphicsObject {
    Device* device = nullptr;
    void* private_data = nullptr; // Old ID3D11DeviceChild prefix, at +24.
    std::shared_ptr<DmaEngine> engine;
    UINT selector = 0, ring_bytes = 0;
    std::mutex mutex;
    std::vector<DmaOperation> pending;
};
// DXGI objects of a device. private_data mirrors IDXGIObject's data member.
struct DxgiObject : GraphicsObject {
    void* private_data = nullptr;
    Device* device = nullptr; // Owns one device reference.
    IUnknown* host = nullptr; // Owned host DXGI object, if any (swapchain).
    std::unique_ptr<DisplayTransfer> display;
    DXGI_SWAP_CHAIN_DESC1 requested_desc{}; // Includes consumed Xbox display flags.
    std::array<Texture*, 16> backbuffers{}; // Owned logical surfaces; host buffers are presentation targets.
    std::mutex swapchain_mutex;
};

std::atomic<D3D11XTrapHandler> trap_handler{nullptr};
std::atomic<bool> legacy_abi_bound{false};
std::atomic<bool> modern_display_abi_bound{false};
std::atomic<std::shared_ptr<Image>> xg_module;
std::atomic<bool> xg_initialized{false};
std::mutex xg_module_mutex;
std::atomic_flag diagnostic_gpu_stop = ATOMIC_FLAG_INIT;

[[noreturn]] void default_trap(const char* interface_name, unsigned slot) {
    char message[160];
    std::snprintf(message, sizeof message, "xone-era: D3D11.X %s vtable slot %u is not implemented\n",
                  interface_name, slot);
    OutputDebugStringA(message);
    std::fputs(message, stderr);
    std::fflush(stderr);
    if (stop_on_missing_service()) TerminateProcess(GetCurrentProcess(), 125);
    __fastfail(FAST_FAIL_FATAL_APP_EXIT); // Never continue with a fabricated result.
}

enum Kind : unsigned { kind_device, kind_context, kind_dxgi_device, kind_adapter, kind_factory, kind_swapchain, kind_dma, kind_shader, kind_buffer, kind_uav, kind_srv, kind_texture, kind_rtv };
constexpr const char* kind_names[]{"ID3D11DeviceX", "ID3D11DeviceContextX", "IDXGIDevice1", "IDXGIAdapter", "IDXGIFactory", "IDXGISwapChain1", "ID3D11DmaEngineContextX", "ID3D11Shader", "ID3D11Buffer", "ID3D11UnorderedAccessView", "ID3D11ShaderResourceView", "ID3D11Texture2D", "ID3D11RenderTargetView"};

void diagnose_shader(std::uintptr_t address, SIZE_T bytes) {
    if (!address || bytes < 32 || bytes > (16u << 20)) return;
    try {
        std::vector<std::uint8_t> code(bytes);
        SIZE_T read = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), code.data(), bytes, &read) || read != bytes)
            return;
        const HMODULE compiler = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!compiler) {
            std::fprintf(stderr, "xone-era: host shader validator unavailable, Win32 %lu\n", GetLastError());
            return;
        }
        const auto reflect = reinterpret_cast<decltype(&D3DReflect)>(GetProcAddress(compiler, "D3DReflect"));
        const auto disassemble = reinterpret_cast<decltype(&D3DDisassemble)>(GetProcAddress(compiler, "D3DDisassemble"));
        ID3D11ShaderReflection* reflection = nullptr;
        const HRESULT hr = reflect ? reflect(code.data(), code.size(), __uuidof(ID3D11ShaderReflection),
            reinterpret_cast<void**>(&reflection)) : E_NOINTERFACE;
        std::fprintf(stderr, "xone-era: native shader reflection 0x%08lX\n", hr);
        if (reflection) {
            D3D11_SHADER_DESC desc{};
            if (SUCCEEDED(reflection->GetDesc(&desc)))
                std::fprintf(stderr, "xone-era: native shader version 0x%X instructions %u resources %u inputs %u outputs %u\n",
                    desc.Version, desc.InstructionCount, desc.BoundResources, desc.InputParameters, desc.OutputParameters);
            reflection->Release();
        }
        ID3DBlob* text = nullptr;
        const HRESULT decoded = disassemble ? disassemble(code.data(), code.size(), 0, nullptr, &text) : E_NOINTERFACE;
        std::fprintf(stderr, "xone-era: native shader disassembly 0x%08lX (not saved)\n", decoded);
        if (text) text->Release();
        FreeLibrary(compiler);
    } catch (const std::bad_alloc&) {
        std::fputs("xone-era: shader inspection allocation failed\n", stderr);
    }
}

template <unsigned K, unsigned Slot>
std::uintptr_t trap_slot(void*, std::uintptr_t a = 0, std::uintptr_t b = 0, std::uintptr_t c = 0, std::uintptr_t d = 0) {
    if (auto handler = trap_handler.load()) return handler(kind_names[K], Slot);
    if (stop_on_missing_service()) {
        // Keep the first fatal GPU diagnostic intact when worker threads hit
        // missing methods together. The reporting thread terminates this process.
        if (diagnostic_gpu_stop.test_and_set()) Sleep(INFINITE);
        std::fprintf(stderr, "xone-era: graphics caller %p\n", _ReturnAddress());
        std::fprintf(stderr, "xone-era: raw graphics integer args 0x%llX 0x%llX 0x%llX 0x%llX\n",
                     static_cast<unsigned long long>(a), static_cast<unsigned long long>(b), static_cast<unsigned long long>(c),
                     static_cast<unsigned long long>(d));
        if constexpr (K == kind_device && (Slot == 5 || Slot == 64)) {
            std::array<UINT32, 13> desc{}; // 18002b460/18002d720 read the original 52-byte texture descriptor.
            SIZE_T read = 0;
            if (a && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(a), desc.data(), sizeof desc, &read) && read == sizeof desc)
                std::fprintf(stderr, "xone-era: placement texture %ux%u mips %u array %u format %u samples %u quality %u usage %u bind 0x%X cpu 0x%X misc 0x%X extra 0x%X 0x%X; tile %llu flags 0x%llX base 0x%llX\n",
                    desc[0], desc[1], desc[2], desc[3], desc[4], desc[5], desc[6], desc[7], desc[8], desc[9], desc[10], desc[11], desc[12],
                    static_cast<unsigned long long>(b), static_cast<unsigned long long>(c), static_cast<unsigned long long>(d));
        }
        if constexpr (K == kind_device && Slot == 3) {
            D3D11_BUFFER_DESC desc{};
            SIZE_T read = 0;
            if (a && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(a), &desc, sizeof desc, &read) && read == sizeof desc)
                std::fprintf(stderr, "xone-era: buffer bytes %u usage %u bind 0x%X cpu 0x%X misc 0x%X stride %u\n",
                    desc.ByteWidth, desc.Usage, desc.BindFlags, desc.CPUAccessFlags, desc.MiscFlags, desc.StructureByteStride);
            UINT64 tail = 0;
            if (a <= UINTPTR_MAX - 32 && a && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(a + 24), &tail, sizeof tail, &read) && read == sizeof tail)
                std::fprintf(stderr, "xone-era: legacy buffer descriptor tail 0x%llX\n", static_cast<unsigned long long>(tail));
        }
        if constexpr (K == kind_device && (Slot == 6 || Slot == 65)) {
            D3D11_TEXTURE3D_DESC desc{};
            SIZE_T read = 0;
            if (a && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(a), &desc, sizeof desc, &read) && read == sizeof desc)
                std::fprintf(stderr, "xone-era: texture3D %ux%ux%u mips %u format %u usage %u bind 0x%X cpu 0x%X misc 0x%X\n",
                    desc.Width, desc.Height, desc.Depth, desc.MipLevels, desc.Format, desc.Usage, desc.BindFlags, desc.CPUAccessFlags, desc.MiscFlags);
        }
        if constexpr (K == kind_swapchain && Slot == 10) {
            GUID iid{}; SIZE_T read = 0;
            if (b && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(b), &iid, sizeof iid, &read) && read == sizeof iid) {
                wchar_t value[40]{}; StringFromGUID2(iid, value, 40);
                std::fprintf(stderr, "xone-era: swapchain GetBuffer index %llu IID %ls\n", static_cast<unsigned long long>(a), value);
            }
        }
        if constexpr (K == kind_device && Slot == 8) {
            std::array<UINT32, 5> desc{};
            SIZE_T read = 0;
            if (b && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(b), desc.data(), sizeof desc, &read) && read == sizeof desc)
                std::fprintf(stderr, "xone-era: view format %u dimension %u union %u %u 0x%X\n", desc[0], desc[1], desc[2], desc[3], desc[4]);
        }
        if constexpr (K == kind_device && Slot == 7) {
            std::array<UINT32, 6> desc{};
            SIZE_T read = 0;
            if (b && ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(b), desc.data(), sizeof desc, &read) && read == sizeof desc)
                std::fprintf(stderr, "xone-era: SRV format %u dimension %u union %u %u %u 0x%X\n", desc[0], desc[1], desc[2], desc[3], desc[4], desc[5]);
        }
        if constexpr (K == kind_device && (Slot == 11 || Slot == 12 || Slot == 13 || Slot == 15 || Slot == 18)) {
            const auto address = Slot == 11 ? c : a;
            const auto input_bytes = Slot == 11 ? d : b;
            std::array<DWORD, 8> header{};
            SIZE_T read = 0;
            if (input_bytes >= sizeof header && input_bytes <= (16u << 20) &&
                ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address), header.data(), sizeof header, &read) &&
                read == sizeof header) {
                std::fprintf(stderr, "xone-era: shader/container header %08lX %08lX %08lX %08lX %08lX %08lX %08lX %08lX\n",
                    header[0], header[1], header[2], header[3], header[4], header[5], header[6], header[7]);
                const SIZE_T bytes = header[6], count = header[7];
                std::array<DWORD, 16> offsets{};
                if (header[0] == 0x43425844 && count && count <= offsets.size() && bytes <= input_bytes &&
                    bytes >= 32 + 4 * count && address <= UINTPTR_MAX - bytes &&
                    ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address + 32), offsets.data(),
                                      count * sizeof(DWORD), &read) && read == count * sizeof(DWORD)) {
                    for (SIZE_T i = 0; i < count; ++i) {
                        std::array<DWORD, 2> part{};
                        const SIZE_T offset = offsets[i];
                        if (offset < 32 + 4 * count || offset > bytes - sizeof part ||
                            !ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address + offset),
                                               part.data(), sizeof part, &read) || read != sizeof part ||
                            part[1] > bytes - offset - sizeof part) {
                            std::fprintf(stderr, "xone-era: shader/container part %llu is invalid or unreadable; inspection incomplete\n",
                                         static_cast<unsigned long long>(i));
                            break;
                        }
                        char id[5]{};
                        for (unsigned j = 0; j < 4; ++j) {
                            const auto ch = (part[0] >> (j * 8)) & 255;
                            id[j] = ch >= 32 && ch <= 126 ? static_cast<char>(ch) : '.';
                        }
                        std::fprintf(stderr, "xone-era: shader/container part %s bytes %lu\n", id, part[1]);
                    }
                }
                diagnose_shader(address, input_bytes);
            }
        }
    }
    default_trap(kind_names[K], Slot);
}

template <unsigned K, std::size_t... I>
std::array<const void*, vtable_slots> traps(std::index_sequence<I...>) {
    return {reinterpret_cast<const void*>(&trap_slot<K, static_cast<unsigned>(I)>)...};
}
template <unsigned K>
std::array<const void*, vtable_slots> traps() { return traps<K>(std::make_index_sequence<vtable_slots>{}); }

template<class T> volatile LONG* counter(T* self) {
    Device* device;
    if constexpr (std::is_same_v<T, Device>) device = self;
    else device = self->device;
    return reinterpret_cast<volatile LONG*>(device->legacy ? &self->flags : &self->refcount);
}
void retain_device(Device* device);
template<class T> ULONG add_ref(T* self) {
    const ULONG count = InterlockedIncrement(counter(self));
    if constexpr (std::is_same_v<T, Context>)
        if (self->device->legacy && !self->deferred && count == 1) retain_device(self->device);
    return count;
}

template<class T> HRESULT query(T* self, REFIID riid, void** out, std::initializer_list<GUID> accepted) {
    if (!out) return E_POINTER;
    *out = nullptr;
    for (const auto& iid : accepted)
        if (IsEqualGUID(riid, iid)) {
            add_ref(self);
            *out = self;
            return S_OK;
        }
    wchar_t requested[40] = L"?";
    StringFromGUID2(riid, requested, 40);
    std::fprintf(stderr, "xone-era: D3D11.X object has no interface %ls\n", requested); // Evidence for the next one.
    return E_NOINTERFACE;
}

const std::array<const void*, vtable_slots>& dxgi_device_vtable();
const std::array<const void*, vtable_slots>& adapter_vtable();
const std::array<const void*, vtable_slots>& factory_vtable();
const void* const* legacy_dxgi_vtable(const void* const* modern);
HRESULT new_dxgi(const void* const* vtable, Device* device, void** out);
HRESULT STDMETHODCALLTYPE device_query(Device* self, REFIID riid, void** out) {
    // The 6.2.11295 performance interface extends DeviceX without new slots/data.
    // Unsupported methods still stop at their exact DeviceX slot.
    if (self->legacy && IsEqualGUID(riid, iid_performance_device))
        return query(self, riid, out, {iid_performance_device});
    // The DXGI device is a separate object (its own IGraphicsUnknown) holding the device.
    if (out && (IsEqualGUID(riid, iid_dxgi_object) || IsEqualGUID(riid, iid_dxgi_device) || IsEqualGUID(riid, iid_dxgi_device1))) {
        *out = nullptr;
        return new_dxgi(dxgi_device_vtable().data(), self, out);
    }
    return query(self, riid, out, {iid_unknown, iid_graphics_unknown, iid_device, iid_device_x});
}
ULONG STDMETHODCALLTYPE device_add_ref(Device* self) { return add_ref(self); }
ULONG STDMETHODCALLTYPE device_release(Device* self) {
    std::unique_lock lock(self->ownership);
    const ULONG left = InterlockedDecrement(counter(self));
    const bool destroy = left == 0 && self->legacy_dependents == 0;
    lock.unlock();
    if (destroy) {
        // 6.2.11295 creation bit 26 disables the deferred-deletion worker.
        // Do not release GPU ownership until the queue has retired synchronously.
        if (self->retirement) {
            HRESULT hr = self->queue->Signal(self->retirement, 1);
            if (SUCCEEDED(hr)) hr = self->retirement->SetEventOnCompletion(1, self->retired);
            if (FAILED(hr) || WaitForSingleObject(self->retired, 5000) != WAIT_OBJECT_0)
                default_trap("synchronous GPU retirement", 0);
            self->retirement->Release();
            CloseHandle(self->retired);
        }
        for (auto& engine : self->dma) engine.reset();
        if (self->queue) self->queue->Release();
        if (self->d3d12) self->d3d12->Release();
        delete self->immediate;
        delete self;
    }
    return left;
}
void retain_device(Device* device) {
    if (!device->legacy) { add_ref(device); return; }
    std::lock_guard lock(device->ownership);
    ++device->legacy_dependents;
}
void release_device(Device* device) {
    if (!device->legacy) { device_release(device); return; }
    std::unique_lock lock(device->ownership);
    const bool last = --device->legacy_dependents == 0 && *counter(device) == 0;
    // Transfer the final dependency to an API reference so destruction uses
    // the same synchronous retirement path; inline public counts remain visible.
    if (last) InterlockedIncrement(counter(device));
    lock.unlock();
    if (last) device_release(device);
}

constexpr GUID shader_iids[]{
    {0xEA82E40D,0x51DC,0x4F33,{0x93,0xD4,0xDB,0x7C,0x91,0x25,0xAE,0x8C}}, // Pixel
    {0x3B301D64,0xD678,0x4289,{0x88,0x97,0x22,0xF8,0x92,0x8B,0x72,0xF3}}, // Vertex
    __uuidof(ID3D11GeometryShader), {}, {},
    {0x4F5B196E,0xC2BD,0x495E,{0xBD,0x01,0x1F,0xDE,0xD3,0x8E,0x49,0x69}} // Compute
};
HRESULT STDMETHODCALLTYPE shader_query(Shader* self, REFIID iid, void** out) {
    return query(self, iid, out, {iid_unknown, iid_graphics_unknown, iid_device_child, shader_iids[self->stage]});
}
ULONG STDMETHODCALLTYPE shader_add_ref(Shader* self) { return add_ref(self); }
ULONG STDMETHODCALLTYPE shader_release(Shader* self) {
    const ULONG left = InterlockedDecrement(counter(self));
    if (!left) { release_device(self->device); delete self; }
    return left;
}
void STDMETHODCALLTYPE shader_get_device(Shader* self, void** out) {
    if (!out) return;
    add_ref(self->device);
    *out = self->device;
}
const std::array<const void*, vtable_slots>& shader_vtable() {
    static const auto table = [] {
        auto t = traps<kind_shader>();
        t[0] = reinterpret_cast<const void*>(&shader_query);
        t[1] = reinterpret_cast<const void*>(&shader_add_ref);
        t[2] = reinterpret_cast<const void*>(&shader_release);
        t[3] = reinterpret_cast<const void*>(&shader_get_device);
        return t;
    }();
    return table;
}

struct ValidatedShader {
    HMODULE compiler = nullptr;
    Microsoft::WRL::ComPtr<ID3D11ShaderReflection> reflection;
    D3D11_SHADER_DESC desc{};
    std::vector<std::uint8_t> code;
    ~ValidatedShader() { reflection.Reset(); if (compiler) FreeLibrary(compiler); }
    HRESULT load(const void* bytecode, SIZE_T bytes, UINT stage) {
        if (!bytecode || bytes < 32 || bytes > (16u << 20)) return E_INVALIDARG;
        code.resize(bytes);
        SIZE_T read = 0;
        if (!ReadProcessMemory(GetCurrentProcess(), bytecode, code.data(), bytes, &read) || read != bytes)
            return E_INVALIDARG;
        compiler = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!compiler) return HRESULT_FROM_WIN32(GetLastError());
        const auto reflect = reinterpret_cast<decltype(&D3DReflect)>(GetProcAddress(compiler, "D3DReflect"));
        const auto disassemble = reinterpret_cast<decltype(&D3DDisassemble)>(GetProcAddress(compiler, "D3DDisassemble"));
        HRESULT hr = reflect ? reflect(code.data(), bytes, IID_PPV_ARGS(&reflection)) : E_NOINTERFACE;
        if (SUCCEEDED(hr)) hr = reflection->GetDesc(&desc);
        if (SUCCEEDED(hr) && ((desc.Version >> 16) != stage || (desc.Version & 0xffff) != 0x50))
            hr = E_INVALIDARG;
        ID3DBlob* decoded = nullptr;
        if (SUCCEEDED(hr)) hr = disassemble ? disassemble(code.data(), bytes, 0, nullptr, &decoded) : E_NOINTERFACE;
        if (decoded) decoded->Release();
        return hr;
    }
};
HRESULT validate_native_compute(Device* device, const ValidatedShader& shader) {
    std::array<UINT, 4> counts{}; // SRV, UAV, CBV, sampler register ranges.
    for (UINT i = 0; i < shader.desc.BoundResources; ++i) {
        D3D11_SHADER_INPUT_BIND_DESC binding{};
        HRESULT hr = shader.reflection->GetResourceBindingDesc(i, &binding);
        if (FAILED(hr)) return hr;
        UINT table = 0;
        switch (binding.Type) {
        case D3D_SIT_CBUFFER: table = 2; break;
        case D3D_SIT_SAMPLER: table = 3; break;
        case D3D_SIT_TBUFFER: case D3D_SIT_TEXTURE: case D3D_SIT_STRUCTURED: case D3D_SIT_BYTEADDRESS: break;
        case D3D_SIT_UAV_RWTYPED: case D3D_SIT_UAV_RWSTRUCTURED: case D3D_SIT_UAV_RWBYTEADDRESS:
        case D3D_SIT_UAV_APPEND_STRUCTURED: case D3D_SIT_UAV_CONSUME_STRUCTURED: case D3D_SIT_UAV_RWSTRUCTURED_WITH_COUNTER:
            table = 1; break;
        default: return DXGI_ERROR_UNSUPPORTED;
        }
        if (!binding.BindCount || binding.BindPoint > 4096 || binding.BindCount > 4096 - binding.BindPoint)
            return DXGI_ERROR_UNSUPPORTED;
        counts[table] = (std::max)(counts[table], binding.BindPoint + binding.BindCount);
    }
    std::array<D3D12_DESCRIPTOR_RANGE, 4> ranges{};
    std::array<D3D12_ROOT_PARAMETER, 4> parameters{};
    UINT count = 0;
    for (UINT i = 0; i < counts.size(); ++i) if (counts[i]) {
        ranges[count] = {static_cast<D3D12_DESCRIPTOR_RANGE_TYPE>(i), counts[i], 0, 0, 0};
        parameters[count].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        parameters[count].DescriptorTable = {1, &ranges[count]};
        parameters[count].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        ++count;
    }
    const D3D12_ROOT_SIGNATURE_DESC desc{count, parameters.data(), 0, nullptr, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    Microsoft::WRL::ComPtr<ID3DBlob> serialized;
    HRESULT hr = D3D12SerializeRootSignature(&desc, D3D_ROOT_SIGNATURE_VERSION_1, &serialized, nullptr);
    Microsoft::WRL::ComPtr<ID3D12RootSignature> root;
    if (SUCCEEDED(hr)) hr = device->d3d12->CreateRootSignature(0, serialized->GetBufferPointer(), serialized->GetBufferSize(), IID_PPV_ARGS(&root));
    if (FAILED(hr)) return hr;
    D3D12_COMPUTE_PIPELINE_STATE_DESC pipeline{};
    pipeline.pRootSignature = root.Get(); pipeline.CS = {shader.code.data(), shader.code.size()};
    Microsoft::WRL::ComPtr<ID3D12PipelineState> compiled;
    return device->d3d12->CreateComputePipelineState(&pipeline, IID_PPV_ARGS(&compiled));
}
template<UINT Stage>
HRESULT STDMETHODCALLTYPE device_create_shader(Device* self, const void* bytecode, SIZE_T bytes, void* linkage, void** out) {
    if (out) *out = nullptr;
    if (linkage) return DXGI_ERROR_UNSUPPORTED;
    try {
        ValidatedShader validated;
        const HRESULT hr = validated.load(bytecode, bytes, Stage);
        if (FAILED(hr)) return hr;
        if (!out) return S_FALSE;
        if constexpr (Stage == 5) {
            static std::atomic<UINT> diagnostic_checks{0};
            if (stop_on_missing_service() && diagnostic_checks.fetch_add(1) < 4) {
                const HRESULT compiled = validate_native_compute(self, validated);
                std::fprintf(stderr, "xone-era: original CS native D3D12 pipeline check 0x%08lX bytes %zu resources %u instructions %u\n",
                    compiled, validated.code.size(), validated.desc.BoundResources, validated.desc.InstructionCount);
                if (FAILED(compiled)) return static_cast<HRESULT>(trap_slot<kind_device, 18>(self,
                    reinterpret_cast<std::uintptr_t>(bytecode), bytes, reinterpret_cast<std::uintptr_t>(linkage), reinterpret_cast<std::uintptr_t>(out)));
            }
        }
        auto shader = std::make_unique<Shader>();
        shader->code = std::move(validated.code);
        shader->vtable = shader_vtable().data();
        shader->flags = 1; shader->refcount = 0;
        shader->device = self; shader->stage = Stage;
        retain_device(self);
        *out = shader.release();
        return S_OK;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}

constexpr GUID iid_input_layout{0xE4819DDC,0x4CF0,0x4025,{0xBD,0x26,0x5D,0xE8,0x2A,0x3E,0x07,0xB7}};
HRESULT STDMETHODCALLTYPE input_layout_query(InputLayout* self, REFIID iid, void** out) {
    return query(self, iid, out, {iid_unknown, iid_graphics_unknown, iid_device_child, iid_input_layout});
}
ULONG STDMETHODCALLTYPE input_layout_add_ref(InputLayout* self) { return add_ref(self); }
ULONG STDMETHODCALLTYPE input_layout_release(InputLayout* self) {
    const ULONG left = InterlockedDecrement(counter(self));
    if (!left) { release_device(self->device); delete self; }
    return left;
}
void STDMETHODCALLTYPE input_layout_get_device(InputLayout* self, void** out) {
    if (!out) return;
    add_ref(self->device); *out = self->device;
}
const std::array<const void*, vtable_slots>& input_layout_vtable() {
    static const auto table = [] {
        auto t = traps<kind_shader>();
        t[0] = reinterpret_cast<const void*>(&input_layout_query);
        t[1] = reinterpret_cast<const void*>(&input_layout_add_ref);
        t[2] = reinterpret_cast<const void*>(&input_layout_release);
        t[3] = reinterpret_cast<const void*>(&input_layout_get_device);
        return t;
    }();
    return table;
}
HRESULT STDMETHODCALLTYPE device_create_input_layout(Device* self, const D3D12_INPUT_ELEMENT_DESC* elements,
        UINT count, const void* bytecode, SIZE_T bytes, void** out) {
    if (out) *out = nullptr;
    if (count > 32 || (count && !elements)) return E_INVALIDARG;
    try {
        ValidatedShader shader;
        HRESULT hr = shader.load(bytecode, bytes, 1);
        if (FAILED(hr)) return hr;
        auto layout = std::make_unique<InputLayout>();
        layout->elements.resize(count); layout->semantics.resize(count);
        SIZE_T read = 0;
        if (count && (!ReadProcessMemory(GetCurrentProcess(), elements, layout->elements.data(), count * sizeof(*elements), &read) ||
                      read != count * sizeof(*elements))) return E_INVALIDARG;
        for (UINT i = 0; i < count; ++i) {
            auto& e = layout->elements[i];
            if (!e.SemanticName || e.InputSlot >= 32 ||
                (e.InputSlotClass != D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA && e.InputSlotClass != D3D12_INPUT_CLASSIFICATION_PER_INSTANCE_DATA) ||
                (e.InputSlotClass == D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA && e.InstanceDataStepRate) ||
                (e.AlignedByteOffset != D3D12_APPEND_ALIGNED_ELEMENT && e.AlignedByteOffset > 2047)) return E_INVALIDARG;
            for (UINT n = 0; n != 256; ++n) {
                char ch = 0;
                if (!ReadProcessMemory(GetCurrentProcess(), e.SemanticName + n, &ch, 1, &read) || read != 1) return E_INVALIDARG;
                if (!ch) break;
                layout->semantics[i].push_back(ch);
            }
            if (layout->semantics[i].empty() || layout->semantics[i].size() == 256) return E_INVALIDARG;
            e.SemanticName = layout->semantics[i].c_str();
            D3D12_FEATURE_DATA_FORMAT_SUPPORT format{e.Format};
            hr = self->d3d12->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &format, sizeof format);
            if (FAILED(hr)) return hr;
            if (!(format.Support1 & D3D12_FORMAT_SUPPORT1_IA_VERTEX_BUFFER)) return E_INVALIDARG;
            for (UINT j = 0; j < i; ++j) {
                const auto& other = layout->elements[j];
                if (e.SemanticIndex == other.SemanticIndex && !_stricmp(e.SemanticName, other.SemanticName)) return E_INVALIDARG;
                if (e.InputSlot == other.InputSlot && (e.InputSlotClass != other.InputSlotClass ||
                    e.InstanceDataStepRate != other.InstanceDataStepRate)) return E_INVALIDARG;
            }
        }
        for (UINT i = 0; i < shader.desc.InputParameters; ++i) {
            D3D11_SIGNATURE_PARAMETER_DESC parameter{};
            hr = shader.reflection->GetInputParameterDesc(i, &parameter);
            if (FAILED(hr)) return hr;
            if (parameter.SystemValueType == D3D_NAME_VERTEX_ID || parameter.SystemValueType == D3D_NAME_INSTANCE_ID) continue;
            bool found = false;
            for (const auto& e : layout->elements)
                if (e.SemanticIndex == parameter.SemanticIndex && !_stricmp(e.SemanticName, parameter.SemanticName)) found = true;
            if (!found) return E_INVALIDARG;
        }
        const auto signature = reinterpret_cast<decltype(&D3DGetInputSignatureBlob)>(GetProcAddress(shader.compiler, "D3DGetInputSignatureBlob"));
        Microsoft::WRL::ComPtr<ID3DBlob> blob;
        hr = signature ? signature(shader.code.data(), bytes, &blob) : E_NOINTERFACE;
        if (FAILED(hr)) return hr;
        const auto* data = static_cast<const std::uint8_t*>(blob->GetBufferPointer());
        layout->signature.assign(data, data + blob->GetBufferSize());
        if (!out) return S_FALSE;
        layout->vtable = input_layout_vtable().data();
        layout->flags = 1; layout->refcount = 0; layout->device = self;
        retain_device(self); *out = layout.release();
        return S_OK;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}

constexpr GUID iid_depth_state{0x03823EFB,0x8D8F,0x4E1C,{0x9A,0xA2,0xF6,0x4B,0xB2,0xCB,0xFD,0xF1}};
HRESULT STDMETHODCALLTYPE depth_state_query(DepthState* self, REFIID iid, void** out) {
    return query(self, iid, out, {iid_unknown, iid_graphics_unknown, iid_device_child, iid_depth_state});
}
ULONG STDMETHODCALLTYPE depth_state_add_ref(DepthState* self) { return add_ref(self); }
ULONG STDMETHODCALLTYPE depth_state_release(DepthState* self) {
    auto* device = self->device;
    std::unique_lock lock(device->ownership);
    const ULONG left = InterlockedDecrement(counter(self));
    if (!left) device->depth_states.erase(self->key);
    lock.unlock();
    if (!left) { release_device(device); delete self; }
    return left;
}
void STDMETHODCALLTYPE depth_state_get_device(DepthState* self, void** out) {
    if (!out) return;
    add_ref(self->device); *out = self->device;
}
void STDMETHODCALLTYPE depth_state_get_desc(DepthState* self, D3D12_DEPTH_STENCIL_DESC* out) {
    if (out) *out = self->desc;
}
const std::array<const void*, vtable_slots>& depth_state_vtable() {
    static const auto table = [] {
        auto t = traps<kind_shader>();
        t[0] = reinterpret_cast<const void*>(&depth_state_query);
        t[1] = reinterpret_cast<const void*>(&depth_state_add_ref);
        t[2] = reinterpret_cast<const void*>(&depth_state_release);
        t[3] = reinterpret_cast<const void*>(&depth_state_get_device);
        t[8] = reinterpret_cast<const void*>(&depth_state_get_desc);
        return t;
    }();
    return table;
}
HRESULT STDMETHODCALLTYPE device_create_depth_state(Device* self, const D3D12_DEPTH_STENCIL_DESC* input, void** out) {
    if (out) *out = nullptr;
    D3D12_DEPTH_STENCIL_DESC desc{};
    SIZE_T read = 0;
    if (!input || !ReadProcessMemory(GetCurrentProcess(), input, &desc, sizeof desc, &read) || read != sizeof desc) return E_INVALIDARG;
    const auto valid_face = [](const D3D12_DEPTH_STENCILOP_DESC& face) {
        return face.StencilFailOp >= D3D12_STENCIL_OP_KEEP && face.StencilFailOp <= D3D12_STENCIL_OP_DECR &&
            face.StencilDepthFailOp >= D3D12_STENCIL_OP_KEEP && face.StencilDepthFailOp <= D3D12_STENCIL_OP_DECR &&
            face.StencilPassOp >= D3D12_STENCIL_OP_KEEP && face.StencilPassOp <= D3D12_STENCIL_OP_DECR &&
            face.StencilFunc >= D3D12_COMPARISON_FUNC_NEVER && face.StencilFunc <= D3D12_COMPARISON_FUNC_ALWAYS;
    };
    if ((desc.DepthWriteMask != D3D12_DEPTH_WRITE_MASK_ZERO && desc.DepthWriteMask != D3D12_DEPTH_WRITE_MASK_ALL) ||
        desc.DepthFunc < D3D12_COMPARISON_FUNC_NEVER || desc.DepthFunc > D3D12_COMPARISON_FUNC_ALWAYS ||
        !valid_face(desc.FrontFace) || !valid_face(desc.BackFace)) return E_INVALIDARG;
    if (!out) return S_FALSE;
    try {
        desc.DepthEnable = desc.DepthEnable != FALSE; desc.StencilEnable = desc.StencilEnable != FALSE;
        std::array<UINT32, 13> key;
        std::memcpy(key.data(), &desc, sizeof desc);
        key[4] &= 0xffff; // Padding is not part of state identity.
        std::unique_lock lock(self->ownership);
        if (const auto existing = self->depth_states.find(key); existing != self->depth_states.end()) {
            add_ref(existing->second); *out = existing->second; return S_OK;
        }
        if (self->depth_states.size() == 4096) return D3D11_ERROR_TOO_MANY_UNIQUE_STATE_OBJECTS;
        auto state = std::make_unique<DepthState>();
        state->vtable = depth_state_vtable().data(); state->flags = 1; state->refcount = 0;
        state->device = self; state->desc = desc; state->key = key;
        self->depth_states.emplace(key, state.get());
        ++self->legacy_dependents;
        *out = state.release();
        return S_OK;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}

constexpr GUID iid_sampler{0xDA6FEA51,0x564C,0x4487,{0x98,0x10,0xF0,0xD0,0xF9,0xB4,0xE3,0xA5}};
HRESULT STDMETHODCALLTYPE sampler_query(Sampler* self, REFIID iid, void** out) {
    return query(self, iid, out, {iid_unknown, iid_graphics_unknown, iid_device_child, iid_sampler});
}
ULONG STDMETHODCALLTYPE sampler_add_ref(Sampler* self) { return add_ref(self); }
ULONG STDMETHODCALLTYPE sampler_release(Sampler* self) {
    auto* device = self->device;
    std::unique_lock lock(device->ownership);
    const ULONG left = InterlockedDecrement(counter(self));
    if (!left) { device->samplers.erase(self->key); device->sampler_slots[self->slot] = nullptr; }
    lock.unlock();
    if (!left) { release_device(device); delete self; }
    return left;
}
void STDMETHODCALLTYPE sampler_get_device(Sampler* self, void** out) {
    if (!out) return;
    add_ref(self->device); *out = self->device;
}
void STDMETHODCALLTYPE sampler_get_desc(Sampler* self, D3D12_SAMPLER_DESC* out) { if (out) *out = self->desc; }
void STDMETHODCALLTYPE sampler_get_desc_x(Sampler* self, UINT32* out) {
    if (out) std::memcpy(out, self->descriptor.data(), sizeof self->descriptor);
}
const std::array<const void*, vtable_slots>& sampler_vtable() {
    static const auto table = [] {
        auto t = traps<kind_shader>();
        t[0] = reinterpret_cast<const void*>(&sampler_query);
        t[1] = reinterpret_cast<const void*>(&sampler_add_ref);
        t[2] = reinterpret_cast<const void*>(&sampler_release);
        t[3] = reinterpret_cast<const void*>(&sampler_get_device);
        t[8] = reinterpret_cast<const void*>(&sampler_get_desc);
        t[9] = reinterpret_cast<const void*>(&sampler_get_desc_x);
        return t;
    }();
    return table;
}
HRESULT STDMETHODCALLTYPE device_create_sampler(Device* self, const D3D12_SAMPLER_DESC* input, void** out) {
    if (out) *out = nullptr;
    D3D12_SAMPLER_DESC desc{};
    SIZE_T read = 0;
    if (!input || !ReadProcessMemory(GetCurrentProcess(), input, &desc, sizeof desc, &read) || read != sizeof desc) return E_INVALIDARG;
    const UINT filter = static_cast<UINT>(desc.Filter), base = filter & ~0x180u;
    if (base != 0 && base != 1 && base != 4 && base != 5 && base != 0x10 && base != 0x11 && base != 0x14 && base != 0x15 && base != 0x55)
        return E_INVALIDARG;
    const auto address_valid = [](D3D12_TEXTURE_ADDRESS_MODE mode) {
        return mode >= D3D12_TEXTURE_ADDRESS_MODE_WRAP && mode <= D3D12_TEXTURE_ADDRESS_MODE_MIRROR_ONCE;
    };
    if (!address_valid(desc.AddressU) || !address_valid(desc.AddressV) || !address_valid(desc.AddressW) ||
        !std::isfinite(desc.MipLODBias) || desc.MipLODBias < D3D12_MIP_LOD_BIAS_MIN || desc.MipLODBias > D3D12_MIP_LOD_BIAS_MAX ||
        !std::isfinite(desc.MinLOD) || !std::isfinite(desc.MaxLOD) || desc.MinLOD > desc.MaxLOD || desc.MaxAnisotropy > 16 ||
        (base == 0x55 && !desc.MaxAnisotropy) ||
        (filter == (base | 0x80) && (desc.ComparisonFunc < D3D12_COMPARISON_FUNC_NEVER || desc.ComparisonFunc > D3D12_COMPARISON_FUNC_ALWAYS)) ||
        static_cast<UINT>(desc.ComparisonFunc) > 8) return E_INVALIDARG;
    // The observed native constructor emits border type zero. Custom border
    // tables need their own console contract; do not silently substitute black.
    for (float color : desc.BorderColor) if (color != 0) return DXGI_ERROR_UNSUPPORTED;
    if (!out) return S_FALSE;
    try {
        auto sampler = std::make_unique<Sampler>();
        sampler->vtable = sampler_vtable().data(); sampler->flags = 1; sampler->refcount = 0;
        sampler->device = self; sampler->desc = desc;
        std::memcpy(sampler->key.data(), &desc, sizeof desc);
        // 6.2.11295 sampler SRD fields verified at 1800b1e70/18009ef70.
        constexpr UINT clamp[]{0, 0, 1, 2, 6, 3};
        UINT ratio = 0;
        if (base == 0x55) for (UINT n = desc.MaxAnisotropy; n > 1; n >>= 1) ++ratio;
        const UINT compare = (static_cast<UINT>(desc.ComparisonFunc) - 1) & 7;
        const UINT reduction = filter >> 7;
        sampler->descriptor[0] = clamp[desc.AddressU] | (clamp[desc.AddressV] << 3) | (clamp[desc.AddressW] << 6) |
            (ratio << 9) | (compare << 12) | ((reduction >= 2 ? reduction - 1 : 0) << 29);
        const auto lod = [](float value) -> UINT { return value <= 0 ? 0 : value >= 4095.f / 256 ? 4095 : static_cast<UINT>(value * 256); };
        const UINT minimum = lod(desc.MinLOD), maximum = lod(desc.MaxLOD);
        sampler->descriptor[1] = minimum | (maximum << 12);
        const auto bias = static_cast<INT>(desc.MipLODBias * 256);
        const UINT xy_min = base == 0x55 ? 3 : (base >> 4) & 1;
        const UINT xy_mag = base == 0x55 ? 3 : (base >> 2) & 1;
        const UINT mip = base == 0x55 ? 2 : (base & 1) + 1;
        sampler->descriptor[2] = (static_cast<UINT>(bias) & 0x3fff) | (xy_mag << 20) | (xy_min << 22) | (mip << 26) | 0x70000000;
        // Host sampling uses the effective quantized console values, while
        // GetDesc preserves the original request and GetDescX exposes the SRD.
        D3D12_SAMPLER_DESC host = desc;
        host.MipLODBias = bias / 256.f; host.MinLOD = minimum / 256.f; host.MaxLOD = maximum / 256.f;
        host.MaxAnisotropy = 1u << ratio;
        host.ComparisonFunc = static_cast<D3D12_COMPARISON_FUNC>(compare + 1);
        std::unique_lock lock(self->ownership);
        if (const auto existing = self->samplers.find(sampler->key); existing != self->samplers.end()) {
            add_ref(existing->second); *out = existing->second; return S_OK;
        }
        UINT slot = 0;
        while (slot < self->sampler_slots.size() && self->sampler_slots[slot]) ++slot;
        if (slot == self->sampler_slots.size()) return D3D11_ERROR_TOO_MANY_UNIQUE_STATE_OBJECTS;
        if (!self->sampler_heap) {
            const D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, D3D11_REQ_SAMPLER_OBJECT_COUNT_PER_DEVICE, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
            const HRESULT hr = self->d3d12->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&self->sampler_heap));
            if (FAILED(hr)) return hr;
        }
        sampler->slot = slot;
        sampler->host = self->sampler_heap->GetCPUDescriptorHandleForHeapStart();
        sampler->host.ptr += SIZE_T(slot) * self->d3d12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        self->samplers.emplace(sampler->key, sampler.get());
        self->d3d12->CreateSampler(&host, sampler->host);
        self->sampler_slots[slot] = sampler.get();
        ++self->legacy_dependents;
        *out = sampler.release();
        return S_OK;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}

constexpr GUID iid_blend_state{0x75B68FAA,0x347D,0x4159,{0x8F,0x45,0xA0,0x64,0x0F,0x01,0xCD,0x9A}};
HRESULT STDMETHODCALLTYPE blend_state_query(BlendState* self, REFIID iid, void** out) {
    return query(self, iid, out, {iid_unknown, iid_graphics_unknown, iid_device_child, iid_blend_state});
}
ULONG STDMETHODCALLTYPE blend_state_add_ref(BlendState* self) { return add_ref(self); }
ULONG STDMETHODCALLTYPE blend_state_release(BlendState* self) {
    auto* device = self->device;
    std::unique_lock lock(device->ownership);
    const ULONG left = InterlockedDecrement(counter(self));
    if (!left) device->blend_states.erase(self->key);
    lock.unlock();
    if (!left) { release_device(device); delete self; }
    return left;
}
void STDMETHODCALLTYPE blend_state_get_device(BlendState* self, void** out) {
    if (!out) return;
    add_ref(self->device); *out = self->device;
}
void STDMETHODCALLTYPE blend_state_get_desc(BlendState* self, D3D11_BLEND_DESC* out) { if (out) *out = self->desc; }
const std::array<const void*, vtable_slots>& blend_state_vtable() {
    static const auto table = [] {
        auto t = traps<kind_shader>();
        t[0] = reinterpret_cast<const void*>(&blend_state_query);
        t[1] = reinterpret_cast<const void*>(&blend_state_add_ref);
        t[2] = reinterpret_cast<const void*>(&blend_state_release);
        t[3] = reinterpret_cast<const void*>(&blend_state_get_device);
        t[8] = reinterpret_cast<const void*>(&blend_state_get_desc);
        return t;
    }();
    return table;
}
HRESULT STDMETHODCALLTYPE device_create_blend_state(Device* self, const D3D11_BLEND_DESC* input, void** out) {
    if (out) *out = nullptr;
    D3D11_BLEND_DESC desc{};
    SIZE_T read = 0;
    if (!input || !ReadProcessMemory(GetCurrentProcess(), input, &desc, sizeof desc, &read) || read != sizeof desc) return E_INVALIDARG;
    const auto valid_blend = [](D3D11_BLEND value) { return (value >= D3D11_BLEND_ZERO && value <= D3D11_BLEND_SRC_ALPHA_SAT) ||
        (value >= D3D11_BLEND_BLEND_FACTOR && value <= D3D11_BLEND_INV_SRC1_ALPHA); };
    const auto valid_op = [](D3D11_BLEND_OP value) { return value >= D3D11_BLEND_OP_ADD && value <= D3D11_BLEND_OP_MAX; };
    const auto valid_alpha = [&](D3D11_BLEND value) { return valid_blend(value) && value != D3D11_BLEND_SRC_COLOR &&
        value != D3D11_BLEND_INV_SRC_COLOR && value != D3D11_BLEND_DEST_COLOR && value != D3D11_BLEND_INV_DEST_COLOR &&
        value != D3D11_BLEND_SRC1_COLOR && value != D3D11_BLEND_INV_SRC1_COLOR; };
    D3D12_BLEND_DESC host{};
    host.AlphaToCoverageEnable = desc.AlphaToCoverageEnable != FALSE;
    host.IndependentBlendEnable = desc.IndependentBlendEnable != FALSE;
    for (UINT i = 0; i < 8; ++i) {
        const auto& source = desc.RenderTarget[host.IndependentBlendEnable ? i : 0];
        if ((source.RenderTargetWriteMask & ~D3D11_COLOR_WRITE_ENABLE_ALL) || (source.BlendEnable &&
            (!valid_blend(source.SrcBlend) || !valid_blend(source.DestBlend) || !valid_alpha(source.SrcBlendAlpha) ||
             !valid_alpha(source.DestBlendAlpha) || !valid_op(source.BlendOp) || !valid_op(source.BlendOpAlpha)))) return E_INVALIDARG;
        auto& target = host.RenderTarget[i];
        target.BlendEnable = source.BlendEnable != FALSE;
        target.LogicOpEnable = FALSE; target.LogicOp = D3D12_LOGIC_OP_NOOP;
        target.RenderTargetWriteMask = source.RenderTargetWriteMask;
        // Disabled blending ignores factors/operations; provide valid host
        // values without changing the guest description or write mask.
        target.SrcBlend = target.BlendEnable ? static_cast<D3D12_BLEND>(source.SrcBlend) : D3D12_BLEND_ONE;
        target.DestBlend = target.BlendEnable ? static_cast<D3D12_BLEND>(source.DestBlend) : D3D12_BLEND_ZERO;
        target.BlendOp = target.BlendEnable ? static_cast<D3D12_BLEND_OP>(source.BlendOp) : D3D12_BLEND_OP_ADD;
        target.SrcBlendAlpha = target.BlendEnable ? static_cast<D3D12_BLEND>(source.SrcBlendAlpha) : D3D12_BLEND_ONE;
        target.DestBlendAlpha = target.BlendEnable ? static_cast<D3D12_BLEND>(source.DestBlendAlpha) : D3D12_BLEND_ZERO;
        target.BlendOpAlpha = target.BlendEnable ? static_cast<D3D12_BLEND_OP>(source.BlendOpAlpha) : D3D12_BLEND_OP_ADD;
    }
    if (!out) return S_FALSE;
    try {
        auto state = std::make_unique<BlendState>();
        state->vtable = blend_state_vtable().data(); state->flags = 1; state->refcount = 0;
        state->device = self; state->desc = desc; state->host = host;
        state->key[0] = host.AlphaToCoverageEnable; state->key[1] = host.IndependentBlendEnable;
        for (UINT i = 0; i < 8; ++i) {
            const auto& r = desc.RenderTarget[i];
            const UINT32 values[]{r.BlendEnable != FALSE, static_cast<UINT32>(r.SrcBlend), static_cast<UINT32>(r.DestBlend),
                static_cast<UINT32>(r.BlendOp), static_cast<UINT32>(r.SrcBlendAlpha), static_cast<UINT32>(r.DestBlendAlpha),
                static_cast<UINT32>(r.BlendOpAlpha), r.RenderTargetWriteMask};
            std::memcpy(state->key.data() + 2 + i * 8, values, sizeof values);
        }
        std::unique_lock lock(self->ownership);
        if (const auto existing = self->blend_states.find(state->key); existing != self->blend_states.end()) {
            add_ref(existing->second); *out = existing->second; return S_OK;
        }
        if (self->blend_states.size() == 4096) return D3D11_ERROR_TOO_MANY_UNIQUE_STATE_OBJECTS;
        self->blend_states.emplace(state->key, state.get()); ++self->legacy_dependents;
        *out = state.release();
        return S_OK;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}

constexpr GUID iid_raster_state{0x9BB4AB81,0xAB1A,0x4D8F,{0xB5,0x06,0xFC,0x04,0x20,0x0B,0x6E,0xE7}};
HRESULT STDMETHODCALLTYPE raster_state_query(RasterState* self, REFIID iid, void** out) {
    return query(self, iid, out, {iid_unknown, iid_graphics_unknown, iid_device_child, iid_raster_state});
}
ULONG STDMETHODCALLTYPE raster_state_add_ref(RasterState* self) { return add_ref(self); }
ULONG STDMETHODCALLTYPE raster_state_release(RasterState* self) {
    auto* device = self->device;
    std::unique_lock lock(device->ownership);
    const ULONG left = InterlockedDecrement(counter(self));
    if (!left) device->raster_states.erase(self->key);
    lock.unlock();
    if (!left) { release_device(device); delete self; }
    return left;
}
void STDMETHODCALLTYPE raster_state_get_device(RasterState* self, void** out) {
    if (!out) return;
    add_ref(self->device); *out = self->device;
}
void STDMETHODCALLTYPE raster_state_get_desc(RasterState* self, D3D11_RASTERIZER_DESC* out) { if (out) *out = self->desc; }
const std::array<const void*, vtable_slots>& raster_state_vtable() {
    static const auto table = [] {
        auto t = traps<kind_shader>();
        t[0] = reinterpret_cast<const void*>(&raster_state_query);
        t[1] = reinterpret_cast<const void*>(&raster_state_add_ref);
        t[2] = reinterpret_cast<const void*>(&raster_state_release);
        t[3] = reinterpret_cast<const void*>(&raster_state_get_device);
        t[8] = reinterpret_cast<const void*>(&raster_state_get_desc);
        return t;
    }();
    return table;
}
HRESULT STDMETHODCALLTYPE device_create_raster_state(Device* self, const D3D11_RASTERIZER_DESC* input, void** out) {
    if (out) *out = nullptr;
    D3D11_RASTERIZER_DESC desc{};
    SIZE_T read = 0;
    if (!input || !ReadProcessMemory(GetCurrentProcess(), input, &desc, sizeof desc, &read) || read != sizeof desc) return E_INVALIDARG;
    if ((desc.FillMode != D3D11_FILL_WIREFRAME && desc.FillMode != D3D11_FILL_SOLID) ||
        desc.CullMode < D3D11_CULL_NONE || desc.CullMode > D3D11_CULL_BACK ||
        !std::isfinite(desc.DepthBiasClamp) || !std::isfinite(desc.SlopeScaledDepthBias)) return E_INVALIDARG;
    if (!out) return S_FALSE;
    try {
        auto state = std::make_unique<RasterState>();
        state->vtable = raster_state_vtable().data(); state->flags = 1; state->refcount = 0; state->device = self;
        desc.FrontCounterClockwise = desc.FrontCounterClockwise != FALSE; desc.DepthClipEnable = desc.DepthClipEnable != FALSE;
        desc.ScissorEnable = desc.ScissorEnable != FALSE; desc.MultisampleEnable = desc.MultisampleEnable != FALSE;
        desc.AntialiasedLineEnable = desc.AntialiasedLineEnable != FALSE;
        state->desc = desc;
        std::memcpy(state->key.data(), &desc, sizeof desc);
        state->host.FillMode = static_cast<D3D12_FILL_MODE>(desc.FillMode);
        state->host.CullMode = static_cast<D3D12_CULL_MODE>(desc.CullMode);
        state->host.FrontCounterClockwise = desc.FrontCounterClockwise;
        state->host.DepthBias = desc.DepthBias; state->host.DepthBiasClamp = desc.DepthBiasClamp;
        state->host.SlopeScaledDepthBias = desc.SlopeScaledDepthBias; state->host.DepthClipEnable = desc.DepthClipEnable;
        state->host.MultisampleEnable = desc.MultisampleEnable; state->host.AntialiasedLineEnable = desc.AntialiasedLineEnable;
        state->host.ForcedSampleCount = 0; state->host.ConservativeRaster = D3D12_CONSERVATIVE_RASTERIZATION_MODE_OFF;
        // ScissorEnable remains in guest state; D3D12 needs the corresponding
        // dynamic scissor rectangles when draw translation is implemented.
        std::unique_lock lock(self->ownership);
        if (const auto existing = self->raster_states.find(state->key); existing != self->raster_states.end()) {
            add_ref(existing->second); *out = existing->second; return S_OK;
        }
        if (self->raster_states.size() == 4096) return D3D11_ERROR_TOO_MANY_UNIQUE_STATE_OBJECTS;
        self->raster_states.emplace(state->key, state.get()); ++self->legacy_dependents;
        *out = state.release();
        return S_OK;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}

// The native graphics allocator (1800c3930/1800c53f4) marks internal
// allocations with system ID 0x80. App ID 3 can prepend a guest heap header.
constexpr UINT64 buffer_graphics_attributes = 0x64804000;
// 180031570/180051fc0 suppress an auxiliary decompression bit for this
// creation flag; expanded samples need no such step. 180053f20 consumes it.
constexpr UINT legacy_expanded_auxiliary = 0x40000;
using AllocateBufferMemory = void* (WINAPI*)(SIZE_T, UINT64);
using FreeBufferMemory = void (WINAPI*)(void*, UINT64);
Buffer::~Buffer() {
    // Draw/copy entry points are still traps: these buffers have no submitted
    // GPU work. Retain their registry pin until all object references are gone.
    if (gpu.resource && FAILED(release_title_gpu_resource(gpu, creation_fence.Get(), 0)))
        default_trap("buffer allocation retirement", 2);
    if (allocation_start) {
        const auto release = reinterpret_cast<FreeBufferMemory>(resolve_kernelx({"kernelx.dll", "XMemFree", 0}));
        if (!release) default_trap("buffer memory service", 2);
        release(allocation_start, buffer_graphics_attributes);
    }
}
constexpr GUID iid_buffer{0x48570B85,0xD1EE,0x4FCD,{0xA2,0x50,0xEB,0x35,0x07,0x22,0xB0,0x37}};
constexpr GUID iid_resource{0xDC8E63F3,0xD12B,0x4952,{0xB4,0x7B,0x5E,0x45,0x02,0x6A,0x86,0x2D}};
HRESULT STDMETHODCALLTYPE buffer_query(Buffer* self, REFIID iid, void** out) {
    return query(self, iid, out, {iid_unknown, iid_graphics_unknown, iid_device_child, iid_resource, iid_buffer});
}
ULONG STDMETHODCALLTYPE buffer_add_ref(Buffer* self) { return add_ref(self); }
ULONG STDMETHODCALLTYPE buffer_release(Buffer* self) {
    const ULONG left = InterlockedDecrement(counter(self));
    if (!left) { auto* device = self->device; delete self; release_device(device); }
    return left;
}
void STDMETHODCALLTYPE buffer_get_device(Buffer* self, void** out) {
    if (!out) return;
    add_ref(self->device); *out = self->device;
}
void STDMETHODCALLTYPE buffer_get_type(Buffer*, D3D11_RESOURCE_DIMENSION* out) { if (out) *out = D3D11_RESOURCE_DIMENSION_BUFFER; }
void STDMETHODCALLTYPE buffer_get_descriptor(Buffer* self, void** out) { if (out) *out = self; } // Native descriptor is a resource reference.
void STDMETHODCALLTYPE buffer_get_desc(Buffer* self, LegacyBufferDesc* out) { if (out) *out = self->desc; }
const std::array<const void*, vtable_slots>& buffer_vtable() {
    static const auto table = [] {
        auto t = traps<kind_buffer>();
        t[0] = reinterpret_cast<const void*>(&buffer_query);
        t[1] = reinterpret_cast<const void*>(&buffer_add_ref);
        t[2] = reinterpret_cast<const void*>(&buffer_release);
        t[3] = reinterpret_cast<const void*>(&buffer_get_device);
        t[8] = reinterpret_cast<const void*>(&buffer_get_type);
        t[11] = reinterpret_cast<const void*>(&buffer_get_descriptor);
        t[12] = reinterpret_cast<const void*>(&buffer_get_desc);
        return t;
    }();
    return table;
}
HRESULT STDMETHODCALLTYPE device_create_buffer(Device* self, const LegacyBufferDesc* input, const D3D11_SUBRESOURCE_DATA* initial, void** out) {
    if (out) *out = nullptr;
    if (!self->legacy) return DXGI_ERROR_UNSUPPORTED;
    LegacyBufferDesc desc{};
    D3D11_SUBRESOURCE_DATA data{};
    SIZE_T read = 0;
    if (!input || !ReadProcessMemory(GetCurrentProcess(), input, &desc, sizeof desc, &read) || read != sizeof desc) return E_INVALIDARG;
    const auto& d = desc.standard;
    if (!d.ByteWidth || static_cast<UINT>(d.Usage) > D3D11_USAGE_STAGING) return E_INVALIDARG;
    const bool structured = (d.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) != 0;
    const bool dynamic = d.Usage == D3D11_USAGE_DYNAMIC;
    if (structured && ((d.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS) || (d.BindFlags & D3D11_BIND_CONSTANT_BUFFER))) return E_INVALIDARG;
    if (structured && (!d.StructureByteStride || d.StructureByteStride > D3D11_REQ_MULTI_ELEMENT_STRUCTURE_SIZE_IN_BYTES ||
                       d.ByteWidth % d.StructureByteStride)) return E_INVALIDARG;
    constexpr UINT supported_misc = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED | D3D11_RESOURCE_MISC_BUFFER_ALLOW_RAW_VIEWS | D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
    if (desc.extension || d.Usage > D3D11_USAGE_DYNAMIC ||
        (dynamic ? d.CPUAccessFlags != D3D11_CPU_ACCESS_WRITE ||
            (d.MiscFlags & ~D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) || (d.BindFlags & D3D11_BIND_UNORDERED_ACCESS) : d.CPUAccessFlags != 0) ||
        (d.MiscFlags & ~supported_misc) ||
        (!structured && d.StructureByteStride) ||
        (d.BindFlags & ~(D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER | D3D11_BIND_CONSTANT_BUFFER | D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS))) {
        if (stop_on_missing_service()) return static_cast<HRESULT>(trap_slot<kind_device, 3>(self,
            reinterpret_cast<std::uintptr_t>(input), reinterpret_cast<std::uintptr_t>(initial), reinterpret_cast<std::uintptr_t>(out)));
        return DXGI_ERROR_UNSUPPORTED;
    }
    if ((d.BindFlags & D3D11_BIND_CONSTANT_BUFFER) && (d.BindFlags != D3D11_BIND_CONSTANT_BUFFER || (d.ByteWidth & 15) || d.ByteWidth > 65536))
        return E_INVALIDARG;
    if (initial && (!ReadProcessMemory(GetCurrentProcess(), initial, &data, sizeof data, &read) || read != sizeof data || !data.pSysMem))
        return E_INVALIDARG;
    if (d.Usage == D3D11_USAGE_IMMUTABLE && !initial) return E_INVALIDARG;
    if (!out) return S_FALSE;
    try {
        auto buffer = std::make_unique<Buffer>();
        buffer->vtable = buffer_vtable().data(); buffer->flags = 1; buffer->refcount = 0;
        buffer->device = self; buffer->desc = desc;
        HRESULT hr = self->d3d12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&buffer->creation_fence));
        if (FAILED(hr)) return hr;
        const auto allocate = reinterpret_cast<AllocateBufferMemory>(resolve_kernelx({"kernelx.dll", "XMemAlloc", 0}));
        if (!allocate) return E_NOINTERFACE;
        buffer->allocation_start = allocate(d.ByteWidth, buffer_graphics_attributes);
        if (!buffer->allocation_start) return E_OUTOFMEMORY;
        if (initial && (!ReadProcessMemory(GetCurrentProcess(), data.pSysMem, buffer->allocation_start, d.ByteWidth, &read) || read != d.ByteWidth))
            return E_INVALIDARG;
        hr = acquire_title_gpu_buffer(buffer->allocation_start, d.ByteWidth, self->d3d12, buffer->gpu,
            d.BindFlags & D3D11_BIND_UNORDERED_ACCESS ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE, true);
        if (FAILED(hr)) return hr;
        const UINT64 address = reinterpret_cast<std::uintptr_t>(buffer->allocation_start);
        if (address > 0xffffffffffull) return DXGI_ERROR_UNSUPPORTED;
        const bool constant = (d.BindFlags & D3D11_BIND_CONSTANT_BUFFER) != 0;
        const UINT stride = structured ? d.StructureByteStride : constant ? 16 : 0;
        buffer->descriptor = {static_cast<UINT32>(address), static_cast<UINT32>(address >> 32) | (stride << 16),
            stride ? d.ByteWidth / stride : d.ByteWidth, 0x24fac}; // Native 1800af450/1800536d0 SRD.
        retain_device(self); *out = buffer.release();
        return S_OK;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}

Texture::~Texture() {
    // Title GPU commands still trap. This binding has no submitted title work.
    if (gpu.resource && FAILED(release_title_gpu_resource(gpu, creation_fence.Get(), 0)))
        default_trap("placed texture retirement", 2);
    if (allocation_attributes && canonical) {
        const auto release = reinterpret_cast<FreeBufferMemory>(resolve_kernelx({"kernelx.dll", "XMemFree", 0}));
        if (!release) default_trap("texture memory service", 2);
        release(canonical, allocation_attributes);
    }
}
HRESULT STDMETHODCALLTYPE texture_query(Texture* self, REFIID iid, void** out) {
    return query(self, iid, out, {iid_unknown, iid_graphics_unknown, iid_device_child, iid_resource,
        self->volume ? __uuidof(ID3D11Texture3D) : __uuidof(ID3D11Texture2D)});
}
ULONG STDMETHODCALLTYPE texture_add_ref(Texture* self) { return add_ref(self); }
ULONG STDMETHODCALLTYPE texture_release(Texture* self) {
    const ULONG left = InterlockedDecrement(counter(self));
    if (!left) { auto* device = self->device; delete self; release_device(device); }
    return left;
}
void STDMETHODCALLTYPE texture_get_device(Texture* self, void** out) { if (out) { add_ref(self->device); *out = self->device; } }
void STDMETHODCALLTYPE texture_get_type(Texture* self, D3D11_RESOURCE_DIMENSION* out) {
    if (out) *out = self->volume ? D3D11_RESOURCE_DIMENSION_TEXTURE3D : D3D11_RESOURCE_DIMENSION_TEXTURE2D;
}
void STDMETHODCALLTYPE texture_get_descriptor(Texture* self, void** out) { if (out) *out = self; }
void STDMETHODCALLTYPE texture_get_desc(Texture* self, void* out) {
    if (out) {
        if (self->volume) std::memcpy(out, &self->volume_desc, sizeof self->volume_desc); // Original 180054230.
        else std::memcpy(out, &self->desc, sizeof self->desc);
    }
}
const std::array<const void*, vtable_slots>& texture_vtable() {
    static const auto table = [] {
        auto t = traps<kind_texture>();
        t[0] = reinterpret_cast<const void*>(&texture_query);
        t[1] = reinterpret_cast<const void*>(&texture_add_ref);
        t[2] = reinterpret_cast<const void*>(&texture_release);
        t[3] = reinterpret_cast<const void*>(&texture_get_device);
        t[8] = reinterpret_cast<const void*>(&texture_get_type);
        t[11] = reinterpret_cast<const void*>(&texture_get_descriptor);
        t[12] = reinterpret_cast<const void*>(&texture_get_desc); // Original 180054130 writes 52 bytes.
        return t;
    }();
    return table;
}
HRESULT make_texture2d(Device* self, const LegacyTexture2DDesc& desc, const TitleTextureLayout& layout, void* base, void** out) {
    if (!out) return S_FALSE;
    try {
        auto texture = std::make_unique<Texture>();
        texture->vtable = texture_vtable().data(); texture->flags = 1; texture->refcount = 0;
        texture->device = self; texture->desc = desc; texture->desc.standard.MipLevels = layout.resource_desc.MipLevels;
        texture->creation_misc_flags = desc.standard.MiscFlags;
        texture->desc.standard.MiscFlags &= ~legacy_expanded_auxiliary;
        HRESULT hr = self->d3d12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&texture->creation_fence));
        if (FAILED(hr)) return hr;
        texture->canonical = base;
        if (!base) {
            const auto allocate = reinterpret_cast<AllocateBufferMemory>(resolve_kernelx({"kernelx.dll", "XMemAlloc", 0}));
            if (!allocate) return E_NOINTERFACE;
            texture->allocation_attributes = (buffer_graphics_attributes & ~(31ull << 24)) |
                (UINT64(std::countr_zero(layout.alignment)) << 24);
            texture->canonical = allocate(layout.bytes, texture->allocation_attributes);
            if (!texture->canonical) return E_OUTOFMEMORY;
        }
        if (reinterpret_cast<std::uintptr_t>(texture->canonical) % layout.alignment ||
            reinterpret_cast<std::uintptr_t>(texture->canonical) > 0xffffffffffull) return DXGI_ERROR_UNSUPPORTED;
        texture->descriptor = layout.guest_descriptor;
        texture->descriptor[0] = static_cast<UINT32>(reinterpret_cast<std::uintptr_t>(texture->canonical) >> 8);
        hr = acquire_title_gpu_texture(texture->canonical, layout, self->d3d12, texture->gpu);
        if (FAILED(hr)) return hr;
        retain_device(self); *out = texture.release();
        return S_OK;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}
HRESULT STDMETHODCALLTYPE device_create_texture2d(Device* self, const LegacyTexture2DDesc* input,
        const D3D11_SUBRESOURCE_DATA* initial, void** out) {
    if (out) *out = nullptr;
    LegacyTexture2DDesc desc{}; SIZE_T read = 0;
    if (!input || !ReadProcessMemory(GetCurrentProcess(), input, &desc, sizeof desc, &read) || read != sizeof desc) return E_INVALIDARG;
    const auto unsupported = [&]() -> HRESULT {
        if (stop_on_missing_service()) return static_cast<HRESULT>(trap_slot<kind_device, 5>(self,
            reinterpret_cast<std::uintptr_t>(input), reinterpret_cast<std::uintptr_t>(initial), reinterpret_cast<std::uintptr_t>(out)));
        return DXGI_ERROR_UNSUPPORTED;
    };
    const auto& d = desc.standard;
    if (!d.Width || !d.Height || !d.ArraySize) return E_INVALIDARG;
    const bool multisample = d.SampleDesc.Count > 1;
    if (!d.SampleDesc.Count || d.SampleDesc.Count > 8 || (d.SampleDesc.Count & (d.SampleDesc.Count - 1))) return E_INVALIDARG;
    if (!self->legacy || initial || desc.extension[0] || desc.extension[1] || d.Usage != D3D11_USAGE_DEFAULT ||
        d.SampleDesc.Quality || d.CPUAccessFlags || (d.MiscFlags & ~legacy_expanded_auxiliary) || !d.BindFlags ||
        ((d.MiscFlags & legacy_expanded_auxiliary) && (!multisample || !(d.BindFlags & D3D11_BIND_RENDER_TARGET))) ||
        (multisample && (d.ArraySize != 1 || d.MipLevels != 1 || !(d.BindFlags & D3D11_BIND_RENDER_TARGET) || (d.BindFlags & D3D11_BIND_UNORDERED_ACCESS))) ||
        (d.BindFlags & ~(D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET | D3D11_BIND_UNORDERED_ACCESS))) return unsupported();
    TitleTextureLayout layout;
    HRESULT hr = create_xg_texture_layout(d.Width, d.Height, d.MipLevels, d.ArraySize, d.Format, UINT_MAX, layout, d.BindFlags, d.SampleDesc.Count);
    if (hr == DXGI_ERROR_UNSUPPORTED || hr == E_UNEXPECTED) return unsupported();
    if (FAILED(hr)) return hr;
    return make_texture2d(self, desc, layout, nullptr, out);
}
HRESULT STDMETHODCALLTYPE device_create_placement_texture2d(Device* self, const LegacyTexture2DDesc* input,
        UINT tile_mode, UINT flags, void* base, void** out) {
    if (out) *out = nullptr;
    LegacyTexture2DDesc desc{};
    SIZE_T read = 0;
    if (!input || !ReadProcessMemory(GetCurrentProcess(), input, &desc, sizeof desc, &read) || read != sizeof desc) return E_INVALIDARG;
    const auto unsupported = [&]() -> HRESULT {
        if (stop_on_missing_service()) return static_cast<HRESULT>(trap_slot<kind_device, 64>(self,
            reinterpret_cast<std::uintptr_t>(input), tile_mode, flags, reinterpret_cast<std::uintptr_t>(base)));
        return DXGI_ERROR_UNSUPPORTED;
    };
    const auto& d = desc.standard;
    if (!d.Width || !d.Height || !d.ArraySize) return E_INVALIDARG;
    if (!self->legacy || flags || desc.extension[0] || desc.extension[1] || !base ||
        d.Usage != D3D11_USAGE_DEFAULT || d.BindFlags != D3D11_BIND_SHADER_RESOURCE || d.CPUAccessFlags || d.MiscFlags ||
        d.SampleDesc.Count != 1 || d.SampleDesc.Quality || reinterpret_cast<std::uintptr_t>(base) > 0xffffffffffull) return unsupported();
    try {
        TitleTextureLayout layout;
        HRESULT hr = create_xg_texture_layout(d.Width, d.Height, d.MipLevels, d.ArraySize, d.Format, tile_mode, layout);
        if (hr == DXGI_ERROR_UNSUPPORTED || hr == E_UNEXPECTED) return unsupported();
        if (FAILED(hr)) return hr;
        hr = make_texture2d(self, desc, layout, base, out);
        if (FAILED(hr)) {
            if (stop_on_missing_service()) {
                std::fprintf(stderr, "xone-era: placement texture canonical binding failed 0x%08lX; span %zu alignment %zu\n", hr, layout.bytes, layout.alignment);
                return unsupported();
            }
            return hr;
        }
        return hr;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}

template<unsigned Slot>
HRESULT create_texture3d(Device* self, const LegacyTexture3DDesc* input,
        const D3D11_SUBRESOURCE_DATA* initial, UINT tile_mode, UINT flags, void* base, void** out) {
    if (out) *out = nullptr;
    LegacyTexture3DDesc desc{};
    SIZE_T read = 0;
    if (!input || !ReadProcessMemory(GetCurrentProcess(), input, &desc, sizeof desc, &read) || read != sizeof desc)
        return E_INVALIDARG;
    const auto unsupported = [&]() -> HRESULT {
        if (stop_on_missing_service()) {
            if constexpr (Slot == 6) return static_cast<HRESULT>(trap_slot<kind_device, Slot>(self,
                reinterpret_cast<std::uintptr_t>(input), reinterpret_cast<std::uintptr_t>(initial), reinterpret_cast<std::uintptr_t>(out)));
            else return static_cast<HRESULT>(trap_slot<kind_device, Slot>(self,
                reinterpret_cast<std::uintptr_t>(input), tile_mode, flags, reinterpret_cast<std::uintptr_t>(base)));
        }
        return DXGI_ERROR_UNSUPPORTED;
    };
    const auto& d = desc.standard;
    if (!d.Width || !d.Height || !d.Depth) return E_INVALIDARG;
    if (!self->legacy || initial || flags || (Slot == 65 && !base) || desc.extension[0] || desc.extension[1] || d.Usage != D3D11_USAGE_DEFAULT ||
        d.CPUAccessFlags || d.MiscFlags || !d.BindFlags ||
        (d.BindFlags & ~(D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS))) return unsupported();
    TitleTextureLayout layout;
    HRESULT hr = create_xg_texture3d_layout(d.Width, d.Height, d.Depth, d.MipLevels, d.Format, d.BindFlags, layout, tile_mode);
    if (hr == DXGI_ERROR_UNSUPPORTED) return unsupported();
    if (FAILED(hr)) return hr;
    if (base && (reinterpret_cast<std::uintptr_t>(base) % layout.alignment ||
        reinterpret_cast<std::uintptr_t>(base) > 0xffffffffffull)) return unsupported();
    if (!out) return S_FALSE;
    try {
        auto texture = std::make_unique<Texture>();
        texture->vtable = texture_vtable().data(); texture->flags = 1; texture->refcount = 0;
        texture->device = self; texture->volume = true; texture->volume_desc = desc;
        texture->volume_desc.standard.MipLevels = layout.resource_desc.MipLevels;
        hr = self->d3d12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&texture->creation_fence));
        if (FAILED(hr)) return hr;
        texture->canonical = base;
        if (!base) {
            const auto allocate = reinterpret_cast<AllocateBufferMemory>(resolve_kernelx({"kernelx.dll", "XMemAlloc", 0}));
            if (!allocate) return E_NOINTERFACE;
            texture->allocation_attributes = (buffer_graphics_attributes & ~(31ull << 24)) |
                (UINT64(std::countr_zero(layout.alignment)) << 24);
            texture->canonical = allocate(layout.bytes, texture->allocation_attributes);
            if (!texture->canonical) return E_OUTOFMEMORY;
        }
        if (reinterpret_cast<std::uintptr_t>(texture->canonical) % layout.alignment ||
            reinterpret_cast<std::uintptr_t>(texture->canonical) > 0xffffffffffull) return unsupported();
        texture->descriptor = layout.guest_descriptor;
        texture->descriptor[0] = static_cast<UINT32>(reinterpret_cast<std::uintptr_t>(texture->canonical) >> 8);
        hr = acquire_title_gpu_texture(texture->canonical, layout, self->d3d12, texture->gpu);
        if (FAILED(hr)) return hr;
        retain_device(self); *out = texture.release();
        return S_OK;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}
HRESULT STDMETHODCALLTYPE device_create_texture3d(Device* self, const LegacyTexture3DDesc* input,
        const D3D11_SUBRESOURCE_DATA* initial, void** out) {
    return create_texture3d<6>(self, input, initial, UINT_MAX, 0, nullptr, out);
}
HRESULT STDMETHODCALLTYPE device_create_placement_texture3d(Device* self, const LegacyTexture3DDesc* input,
        UINT tile_mode, UINT flags, void* base, void** out) {
    return create_texture3d<65>(self, input, nullptr, tile_mode, flags, base, out);
}

BufferView::~BufferView() {
    if (metadata_texture) texture_release(metadata_texture);
    if (counter_buffer) buffer_release(counter_buffer);
    if (resource) reinterpret_cast<ULONG (STDMETHODCALLTYPE*)(GraphicsObject*)>(resource->vtable[2])(resource);
}
HRESULT STDMETHODCALLTYPE view_query(BufferView* self, REFIID iid, void** out) {
    if (self->view_type == 0)
        return query(self, iid, out, {iid_unknown, iid_graphics_unknown, iid_device_child,
            __uuidof(ID3D11View), __uuidof(ID3D11RenderTargetView)});
    if (self->view_type == 2)
        return query(self, iid, out, {iid_unknown, iid_graphics_unknown, iid_device_child,
            __uuidof(ID3D11View), __uuidof(ID3D11ShaderResourceView)});
    // The standard UAV IID is present in the original QI table at 1801192c8.
    return query(self, iid, out, {iid_unknown, iid_graphics_unknown, iid_device_child,
        __uuidof(ID3D11View), __uuidof(ID3D11UnorderedAccessView)});
}
ULONG STDMETHODCALLTYPE view_add_ref(BufferView* self) { return add_ref(self); }
ULONG STDMETHODCALLTYPE view_release(BufferView* self) {
    const ULONG left = InterlockedDecrement(counter(self));
    if (!left) { auto* device = self->device; delete self; release_device(device); }
    return left;
}
void STDMETHODCALLTYPE view_get_device(BufferView* self, void** out) {
    if (!out) return;
    add_ref(self->device); *out = self->device;
}
void STDMETHODCALLTYPE view_get_resource(BufferView* self, void** out) {
    if (!out) return;
    reinterpret_cast<ULONG (STDMETHODCALLTYPE*)(GraphicsObject*)>(self->resource->vtable[1])(self->resource); *out = self->resource;
}
void STDMETHODCALLTYPE uav_get_desc(BufferView* self, D3D11_UNORDERED_ACCESS_VIEW_DESC* out) { if (out) *out = self->desc; }
void STDMETHODCALLTYPE srv_get_desc(BufferView* self, D3D11_SHADER_RESOURCE_VIEW_DESC* out) { if (out) *out = self->srv_desc; }
void STDMETHODCALLTYPE rtv_get_desc(BufferView* self, D3D11_RENDER_TARGET_VIEW_DESC* out) { if (out) *out = self->rtv_desc; }
const std::array<const void*, vtable_slots>& rtv_vtable() {
    static const auto table = [] {
        auto t = traps<kind_rtv>();
        t[0] = reinterpret_cast<const void*>(&view_query);
        t[1] = reinterpret_cast<const void*>(&view_add_ref);
        t[2] = reinterpret_cast<const void*>(&view_release);
        t[3] = reinterpret_cast<const void*>(&view_get_device);
        t[8] = reinterpret_cast<const void*>(&view_get_resource);
        t[9] = reinterpret_cast<const void*>(&rtv_get_desc);
        return t;
    }();
    return table;
}
HRESULT STDMETHODCALLTYPE device_create_rtv(Device* self, GraphicsObject* resource, const D3D11_RENDER_TARGET_VIEW_DESC* input, void** out) {
    if (out) *out = nullptr;
    if (!resource || resource->vtable != texture_vtable().data()) return E_INVALIDARG;
    auto* texture = static_cast<Texture*>(resource);
    if (texture->device != self) return E_INVALIDARG;
    const auto unsupported = [&]() -> HRESULT {
        if (stop_on_missing_service()) return static_cast<HRESULT>(trap_slot<kind_device, 9>(self,
            reinterpret_cast<std::uintptr_t>(resource), reinterpret_cast<std::uintptr_t>(input), reinterpret_cast<std::uintptr_t>(out)));
        return DXGI_ERROR_UNSUPPORTED;
    };
    const auto& d = texture->desc.standard;
    if (texture->volume || d.ArraySize != 1 || !(d.BindFlags & D3D11_BIND_RENDER_TARGET)) return unsupported();
    const bool multisample = d.SampleDesc.Count > 1;
    const auto dimension = multisample ? D3D11_RTV_DIMENSION_TEXTURE2DMS : D3D11_RTV_DIMENSION_TEXTURE2D;
    D3D11_RENDER_TARGET_VIEW_DESC desc{}; SIZE_T read = 0;
    if (input && (!ReadProcessMemory(GetCurrentProcess(), input, &desc, sizeof desc, &read) || read != sizeof desc)) return E_INVALIDARG;
    if (!input) { desc.Format = d.Format; desc.ViewDimension = dimension; }
    if (desc.Format == DXGI_FORMAT_UNKNOWN) desc.Format = d.Format;
    if (desc.Format != d.Format || desc.ViewDimension != dimension) return unsupported();
    if (!multisample && desc.Texture2D.MipSlice >= d.MipLevels) return E_INVALIDARG;
    if (!out) return S_FALSE;
    try {
        auto view = std::make_unique<BufferView>();
        view->vtable = rtv_vtable().data(); view->flags = 1; view->refcount = 0;
        view->device = self; view->view_type = 0;
        view->rtv_desc.Format = desc.Format; view->rtv_desc.ViewDimension = desc.ViewDimension;
        if (!multisample) view->rtv_desc.Texture2D = desc.Texture2D;
        const D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        const HRESULT hr = self->d3d12->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&view->heap));
        if (FAILED(hr)) return hr;
        view->host = view->heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_RENDER_TARGET_VIEW_DESC native{};
        native.Format = desc.Format;
        native.ViewDimension = multisample ? D3D12_RTV_DIMENSION_TEXTURE2DMS : D3D12_RTV_DIMENSION_TEXTURE2D;
        if (!multisample) native.Texture2D.MipSlice = desc.Texture2D.MipSlice;
        self->d3d12->CreateRenderTargetView(texture->gpu.resource, &native, view->host);
        texture_add_ref(texture); view->resource = texture;
        retain_device(self); *out = view.release();
        return S_OK;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}
const std::array<const void*, vtable_slots>& srv_vtable() {
    static const auto table = [] {
        auto t = traps<kind_srv>();
        t[0] = reinterpret_cast<const void*>(&view_query);
        t[1] = reinterpret_cast<const void*>(&view_add_ref);
        t[2] = reinterpret_cast<const void*>(&view_release);
        t[3] = reinterpret_cast<const void*>(&view_get_device);
        t[8] = reinterpret_cast<const void*>(&view_get_resource);
        t[9] = reinterpret_cast<const void*>(&srv_get_desc);
        return t;
    }();
    return table;
}
const std::array<const void*, vtable_slots>& uav_vtable() {
    static const auto table = [] {
        auto t = traps<kind_uav>();
        t[0] = reinterpret_cast<const void*>(&view_query);
        t[1] = reinterpret_cast<const void*>(&view_add_ref);
        t[2] = reinterpret_cast<const void*>(&view_release);
        t[3] = reinterpret_cast<const void*>(&view_get_device);
        t[8] = reinterpret_cast<const void*>(&view_get_resource);
        t[9] = reinterpret_cast<const void*>(&uav_get_desc);
        return t;
    }();
    return table;
}
HRESULT STDMETHODCALLTYPE device_create_uav(Device* self, Buffer* resource, const D3D11_UNORDERED_ACCESS_VIEW_DESC* input, void** out) {
    if (out) *out = nullptr;
    if (!resource || resource->vtable != buffer_vtable().data() || resource->device != self ||
        !(resource->desc.standard.BindFlags & D3D11_BIND_UNORDERED_ACCESS)) return E_INVALIDARG;
    D3D11_UNORDERED_ACCESS_VIEW_DESC desc{};
    SIZE_T read = 0;
    const auto& source = resource->desc.standard;
    const bool structured = (source.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED) != 0;
    if (input && (!ReadProcessMemory(GetCurrentProcess(), input, &desc, sizeof desc, &read) || read != sizeof desc)) return E_INVALIDARG;
    const auto unsupported = [&](const char* reason) -> HRESULT {
        if (stop_on_missing_service()) {
            std::fprintf(stderr, "xone-era: UAV unsupported: %s; shared offset 0x%llX stride %u\n", reason,
                static_cast<unsigned long long>(resource->gpu.offset), source.StructureByteStride);
            return static_cast<HRESULT>(trap_slot<kind_device, 8>(self,
                reinterpret_cast<std::uintptr_t>(resource), reinterpret_cast<std::uintptr_t>(input), reinterpret_cast<std::uintptr_t>(out)));
        }
        return DXGI_ERROR_UNSUPPORTED;
    };
    if (!structured) return unsupported("non-structured view");
    const UINT records = source.ByteWidth / source.StructureByteStride;
    if (!input) { desc.Format = DXGI_FORMAT_UNKNOWN; desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER; desc.Buffer.NumElements = records; }
    if (desc.ViewDimension != D3D11_UAV_DIMENSION_BUFFER) return E_INVALIDARG;
    if (desc.Format != DXGI_FORMAT_UNKNOWN || (desc.Buffer.Flags & ~(D3D11_BUFFER_UAV_FLAG_APPEND | D3D11_BUFFER_UAV_FLAG_COUNTER))) return unsupported("format or flags");
    if (desc.Buffer.Flags == (D3D11_BUFFER_UAV_FLAG_APPEND | D3D11_BUFFER_UAV_FLAG_COUNTER)) return E_INVALIDARG;
    const UINT count = !desc.Buffer.FirstElement && !desc.Buffer.NumElements ? records : desc.Buffer.NumElements;
    if (!count || desc.Buffer.FirstElement >= records || count > records - desc.Buffer.FirstElement) return E_INVALIDARG;
    if (resource->gpu.offset % source.StructureByteStride) return unsupported("suballocation offset is not divisible by stride");
    if (!out) return S_FALSE;
    try {
        auto view = std::make_unique<BufferView>();
        view->vtable = uav_vtable().data(); view->flags = 1; view->refcount = 0;
        view->device = self; view->desc = desc; view->allocation_start = resource->allocation_start;
        const UINT64 address = reinterpret_cast<std::uintptr_t>(resource->allocation_start) + UINT64(desc.Buffer.FirstElement) * source.StructureByteStride;
        if (address > 0xffffffffffull) return unsupported("GPU address exceeds native descriptor range");
        std::copy(resource->descriptor.begin(), resource->descriptor.end(), view->descriptor.begin());
        view->descriptor[0] = static_cast<UINT32>(address);
        view->descriptor[1] = (view->descriptor[1] & 0xffffff00) | static_cast<UINT32>(address >> 32);
        view->descriptor[2] = count; // Native 1800af700; the upper sixteen SRD bytes stay zero.
        HRESULT hr = S_OK;
        if (desc.Buffer.Flags) {
            const UINT zero = 0;
            const D3D11_SUBRESOURCE_DATA initial{&zero, 0, 0};
            const LegacyBufferDesc counter_desc{{4, D3D11_USAGE_DEFAULT, D3D11_BIND_UNORDERED_ACCESS, 0, 0, 0}, 0};
            void* counter_object = nullptr;
            hr = device_create_buffer(self, &counter_desc, &initial, &counter_object);
            if (FAILED(hr)) return hr;
            view->counter_buffer = static_cast<Buffer*>(counter_object);
            if (view->counter_buffer->gpu.offset % D3D12_UAV_COUNTER_PLACEMENT_ALIGNMENT) return unsupported("counter suballocation alignment");
        }
        const D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        hr = self->d3d12->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&view->heap));
        if (FAILED(hr)) return hr;
        view->host = view->heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_UNORDERED_ACCESS_VIEW_DESC native{};
        native.Format = DXGI_FORMAT_UNKNOWN; native.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
        native.Buffer.FirstElement = resource->gpu.offset / source.StructureByteStride + desc.Buffer.FirstElement;
        native.Buffer.NumElements = count; native.Buffer.StructureByteStride = source.StructureByteStride;
        if (view->counter_buffer) native.Buffer.CounterOffsetInBytes = view->counter_buffer->gpu.offset;
        self->d3d12->CreateUnorderedAccessView(resource->gpu.resource,
            view->counter_buffer ? view->counter_buffer->gpu.resource : nullptr, &native, view->host);
        buffer_add_ref(resource); view->resource = resource;
        retain_device(self); *out = view.release();
        return S_OK;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}

HRESULT STDMETHODCALLTYPE device_create_buffer_srv(Device* self, Buffer* resource, const D3D11_SHADER_RESOURCE_VIEW_DESC* input, void** out) {
    if (out) *out = nullptr;
    if (!resource || resource->vtable != buffer_vtable().data() || resource->device != self ||
        !(resource->desc.standard.BindFlags & D3D11_BIND_SHADER_RESOURCE)) return E_INVALIDARG;
    D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
    SIZE_T read = 0;
    const auto& source = resource->desc.standard;
    if (input && (!ReadProcessMemory(GetCurrentProcess(), input, &desc, sizeof desc, &read) || read != sizeof desc)) return E_INVALIDARG;
    const auto unsupported = [&]() -> HRESULT {
        if (stop_on_missing_service()) return static_cast<HRESULT>(trap_slot<kind_device, 7>(self,
            reinterpret_cast<std::uintptr_t>(resource), reinterpret_cast<std::uintptr_t>(input), reinterpret_cast<std::uintptr_t>(out)));
        return DXGI_ERROR_UNSUPPORTED;
    };
    if (!(source.MiscFlags & D3D11_RESOURCE_MISC_BUFFER_STRUCTURED)) return unsupported();
    const UINT records = source.ByteWidth / source.StructureByteStride;
    if (!input) { desc.Format = DXGI_FORMAT_UNKNOWN; desc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER; desc.Buffer.NumElements = records; }
    if (desc.ViewDimension != D3D11_SRV_DIMENSION_BUFFER || desc.Format != DXGI_FORMAT_UNKNOWN) return unsupported();
    const UINT count = !desc.Buffer.FirstElement && !desc.Buffer.NumElements ? records : desc.Buffer.NumElements;
    if (!count || desc.Buffer.FirstElement >= records || count > records - desc.Buffer.FirstElement) return E_INVALIDARG;
    if (resource->gpu.offset % source.StructureByteStride) return unsupported();
    if (!out) return S_FALSE;
    try {
        auto view = std::make_unique<BufferView>();
        view->vtable = srv_vtable().data(); view->flags = 1; view->refcount = 0;
        view->view_type = 2; view->device = self; view->allocation_start = resource->allocation_start;
        view->srv_desc.Format = desc.Format; view->srv_desc.ViewDimension = desc.ViewDimension;
        view->srv_desc.Buffer = desc.Buffer;
        const UINT64 address = reinterpret_cast<std::uintptr_t>(resource->allocation_start) + UINT64(desc.Buffer.FirstElement) * source.StructureByteStride;
        if (address > 0xffffffffffull) return unsupported();
        std::copy(resource->descriptor.begin(), resource->descriptor.end(), view->descriptor.begin());
        view->descriptor[0] = static_cast<UINT32>(address);
        view->descriptor[1] = (view->descriptor[1] & 0xffffff00) | static_cast<UINT32>(address >> 32);
        view->descriptor[2] = count; // Native 180050660/1800af540 buffer SRD; upper half stays zero.
        const D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
        const HRESULT hr = self->d3d12->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&view->heap));
        if (FAILED(hr)) return hr;
        view->host = view->heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_SHADER_RESOURCE_VIEW_DESC native{};
        native.Format = DXGI_FORMAT_UNKNOWN; native.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
        native.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        native.Buffer.FirstElement = resource->gpu.offset / source.StructureByteStride + desc.Buffer.FirstElement;
        native.Buffer.NumElements = count; native.Buffer.StructureByteStride = source.StructureByteStride;
        self->d3d12->CreateShaderResourceView(resource->gpu.resource, &native, view->host);
        buffer_add_ref(resource); view->resource = resource;
        retain_device(self); *out = view.release();
        return S_OK;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}

HRESULT STDMETHODCALLTYPE device_create_srv(Device* self, GraphicsObject* resource, const D3D11_SHADER_RESOURCE_VIEW_DESC* input, void** out) {
    if (out) *out = nullptr;
    if (resource && resource->vtable == buffer_vtable().data())
        return device_create_buffer_srv(self, static_cast<Buffer*>(resource), input, out);
    if (resource && resource->vtable == texture_vtable().data()) {
        auto* texture = static_cast<Texture*>(resource);
        if (texture->device != self) return E_INVALIDARG;
        const auto unsupported = [&]() -> HRESULT {
            if (stop_on_missing_service()) {
                std::fprintf(stderr, "xone-era: SRV source %s %ux%u format %u mips %u samples %u\n", texture->volume ? "3D" : "2D",
                    texture->volume ? texture->volume_desc.standard.Width : texture->desc.standard.Width,
                    texture->volume ? texture->volume_desc.standard.Height : texture->desc.standard.Height,
                    texture->volume ? texture->volume_desc.standard.Format : texture->desc.standard.Format,
                    texture->volume ? texture->volume_desc.standard.MipLevels : texture->desc.standard.MipLevels,
                    texture->volume ? 1 : texture->desc.standard.SampleDesc.Count);
                return static_cast<HRESULT>(trap_slot<kind_device, 7>(self,
                    reinterpret_cast<std::uintptr_t>(resource), reinterpret_cast<std::uintptr_t>(input), reinterpret_cast<std::uintptr_t>(out)));
            }
            return DXGI_ERROR_UNSUPPORTED;
        };
        const auto format = texture->volume ? texture->volume_desc.standard.Format : texture->desc.standard.Format;
        const UINT mips = texture->volume ? texture->volume_desc.standard.MipLevels : texture->desc.standard.MipLevels;
        const UINT binds = texture->volume ? texture->volume_desc.standard.BindFlags : texture->desc.standard.BindFlags;
        const bool multisample = !texture->volume && texture->desc.standard.SampleDesc.Count > 1;
        const auto dimension = texture->volume ? D3D11_SRV_DIMENSION_TEXTURE3D :
            multisample ? D3D11_SRV_DIMENSION_TEXTURE2DMS : D3D11_SRV_DIMENSION_TEXTURE2D;
        if (!(binds & D3D11_BIND_SHADER_RESOURCE) || (!texture->volume && texture->desc.standard.ArraySize != 1) ||
            format == DXGI_FORMAT_BC7_TYPELESS) return unsupported();
        D3D11_SHADER_RESOURCE_VIEW_DESC desc{};
        SIZE_T read = 0;
        if (input && (!ReadProcessMemory(GetCurrentProcess(), input, &desc, sizeof desc, &read) || read != sizeof desc)) return E_INVALIDARG;
        if (!input) { desc.Format = format; desc.ViewDimension = dimension; if (!multisample) desc.Texture2D.MipLevels = mips; }
        if (desc.Format == DXGI_FORMAT_UNKNOWN) desc.Format = format;
        if (multisample && desc.Format == DXGI_FORMAT_R8_UINT && desc.ViewDimension == D3D11_SRV_DIMENSION_TEXTURE2D) {
            const auto& d = texture->desc.standard;
            if (d.SampleDesc.Count > 4 || d.SampleDesc.Quality || desc.Texture2D.MostDetailedMip ||
                (desc.Texture2D.MipLevels != 1 && desc.Texture2D.MipLevels != UINT_MAX)) return unsupported();
            if (!out) return S_FALSE;
            try {
                LegacyTexture2DDesc meta{};
                meta.standard = {d.Width, d.Height, 1, 1, DXGI_FORMAT_R8_UINT, {1, 0}, D3D11_USAGE_DEFAULT, D3D11_BIND_SHADER_RESOURCE, 0, 0};
                TitleTextureLayout layout;
                HRESULT hr = create_xg_texture_layout(d.Width, d.Height, 1, 1, meta.standard.Format, UINT_MAX, layout, meta.standard.BindFlags);
                if (FAILED(hr)) return hr;
                void* object = nullptr;
                hr = make_texture2d(self, meta, layout, nullptr, &object);
                if (FAILED(hr)) return hr;
                std::unique_ptr<Texture, decltype(&texture_release)> metadata(static_cast<Texture*>(object), &texture_release);
                // Every fragment exists independently in our expanded MSAA resource.
                // FMASK therefore maps sample i to fragment i; quality/EQAA cases above are explicit.
                const UINT bits = std::bit_width(d.SampleDesc.Count - 1);
                UINT identity = 0;
                for (UINT sample = 0; sample < d.SampleDesc.Count; ++sample) identity |= sample << (bits * sample);
                std::vector<unsigned char> linear(SIZE_T(d.Width) * d.Height, static_cast<unsigned char>(identity));
                void* canonical = nullptr;
                hr = map_title_gpu_resource(metadata->gpu, &canonical);
                if (FAILED(hr)) return hr;
                hr = layout.copy(layout.context.get(), true, linear.data(), canonical, 0, d.Width, d.Width * d.Height);
                const HRESULT unmapped = unmap_title_gpu_resource(metadata->gpu);
                if (FAILED(hr)) return hr;
                if (FAILED(unmapped)) return unmapped;
                auto view = std::make_unique<BufferView>();
                view->vtable = srv_vtable().data(); view->flags = 1; view->refcount = 0;
                view->device = self; view->view_type = 2;
                view->srv_desc.Format = desc.Format; view->srv_desc.ViewDimension = desc.ViewDimension; view->srv_desc.Texture2D.MipLevels = 1;
                view->descriptor = metadata->descriptor; view->allocation_start = metadata->canonical;
                const D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
                hr = self->d3d12->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&view->heap));
                if (FAILED(hr)) return hr;
                view->host = view->heap->GetCPUDescriptorHandleForHeapStart();
                D3D12_SHADER_RESOURCE_VIEW_DESC native{};
                native.Format = DXGI_FORMAT_R8_UINT; native.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                native.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; native.Texture2D.MipLevels = 1;
                self->d3d12->CreateShaderResourceView(metadata->gpu.resource, &native, view->host);
                view->metadata_texture = metadata.release();
                texture_add_ref(texture); view->resource = texture;
                retain_device(self); *out = view.release();
                return S_OK;
            } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
        }
        if (desc.ViewDimension != dimension || desc.Format != format) return unsupported();
        // Texture2D/Texture3D share the same two-UINT mip range in this ABI.
        if (!multisample) {
            if (desc.Texture2D.MostDetailedMip >= mips) return E_INVALIDARG;
            if (desc.Texture2D.MipLevels == UINT_MAX) desc.Texture2D.MipLevels = mips - desc.Texture2D.MostDetailedMip;
            if (!desc.Texture2D.MipLevels || desc.Texture2D.MipLevels > mips - desc.Texture2D.MostDetailedMip) return E_INVALIDARG;
            if (desc.Texture2D.MostDetailedMip || desc.Texture2D.MipLevels != mips) return unsupported();
        }
        if (!out) return S_FALSE;
        try {
            auto view = std::make_unique<BufferView>();
            view->vtable = srv_vtable().data(); view->flags = 1; view->refcount = 0;
            view->view_type = 2; view->device = self;
            view->srv_desc.Format = desc.Format; view->srv_desc.ViewDimension = desc.ViewDimension;
            if (!multisample) view->srv_desc.Texture2D = desc.Texture2D;
            view->allocation_start = texture->canonical; view->descriptor = texture->descriptor;
            // 18009b0c0: base/last mip in word 3; first/last array slice in word 5.
            if (multisample) view->descriptor[3] &= ~0xf000u; // Preserve native log2(samples) at bits 16..19.
            else view->descriptor[3] = (view->descriptor[3] & ~0xff000u) | ((mips - 1) << 16);
            if (!texture->volume) view->descriptor[5] &= ~0x3ffffffu;
            const D3D12_DESCRIPTOR_HEAP_DESC heap{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE, 0};
            const HRESULT hr = self->d3d12->CreateDescriptorHeap(&heap, IID_PPV_ARGS(&view->heap));
            if (FAILED(hr)) return hr;
            view->host = view->heap->GetCPUDescriptorHandleForHeapStart();
            D3D12_SHADER_RESOURCE_VIEW_DESC native{};
            native.Format = format;
            native.ViewDimension = texture->volume ? D3D12_SRV_DIMENSION_TEXTURE3D :
                multisample ? D3D12_SRV_DIMENSION_TEXTURE2DMS : D3D12_SRV_DIMENSION_TEXTURE2D;
            native.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            if (texture->volume) native.Texture3D.MipLevels = mips;
            else if (!multisample) native.Texture2D.MipLevels = mips;
            self->d3d12->CreateShaderResourceView(texture->gpu.resource, &native, view->host);
            texture_add_ref(texture); view->resource = texture;
            retain_device(self); *out = view.release();
            return S_OK;
        } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
    }
    return E_INVALIDARG;
}

constexpr UINT64 dma_sequence_mask = (UINT64{1} << 48) - 1;
std::shared_ptr<DmaEngine> dma_fence_engine(Device* device, UINT64 token, UINT64& value) {
    const UINT64 selector = token >> 48;
    value = token & dma_sequence_mask;
    std::lock_guard lock(device->ownership);
    if (!selector || selector >= device->dma.size() || !value || !device->dma[selector] ||
        value > device->dma[selector]->issued.load()) default_trap("invalid DMA fence", 60);
    return device->dma[selector];
}
HRESULT dma_submit_locked(Dma* self) {
    std::size_t completed = 0;
    HRESULT hr = S_OK;
    for (const auto& operation : self->pending) {
        hr = operation.wait ? self->engine->queue->Wait(operation.fence, operation.value) :
                             self->engine->queue->Signal(operation.fence, operation.value);
        if (FAILED(hr)) break;
        if (!operation.wait) self->engine->submitted = operation.value;
        ++completed;
    }
    self->pending.erase(self->pending.begin(), self->pending.begin() + completed);
    return hr;
}
[[noreturn]] void dma_error(unsigned slot, HRESULT hr) {
    std::fprintf(stderr, "xone-era: DMA operation failed 0x%08lX\n", hr);
    default_trap("ID3D11DmaEngineContextX", slot);
}
void dma_append_locked(Dma* self, DmaOperation operation, unsigned slot) {
    // Bound pending fence/wait records by the supplied ring budget. GPU work
    // is never dropped when that budget fills: submit before appending.
    if (self->pending.size() == self->ring_bytes / sizeof(DmaOperation)) {
        const HRESULT hr = dma_submit_locked(self);
        if (FAILED(hr)) dma_error(slot, hr);
    }
    self->pending.push_back(operation); // Creation reserved the entire bounded capacity.
}
HRESULT STDMETHODCALLTYPE dma_query(Dma* self, REFIID iid, void** out) {
    return query(self, iid, out, {iid_unknown, iid_graphics_unknown, iid_device_child, iid_dma});
}
ULONG STDMETHODCALLTYPE dma_add_ref(Dma* self) { return add_ref(self); }
ULONG STDMETHODCALLTYPE dma_release(Dma* self) {
    const ULONG left = InterlockedDecrement(counter(self));
    if (left) return left;
    {
        std::lock_guard lock(self->mutex);
        const HRESULT hr = dma_submit_locked(self);
        if (FAILED(hr)) dma_error(2, hr);
    }
    const UINT64 last = self->engine->submitted.load();
    const UINT64 completed = self->engine->fence->GetCompletedValue();
    if (completed == UINT64_MAX) dma_error(2, DXGI_ERROR_DEVICE_REMOVED);
    if (completed < last) {
        HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!event) dma_error(2, HRESULT_FROM_WIN32(GetLastError()));
        const HRESULT hr = self->engine->fence->SetEventOnCompletion(last, event);
        const DWORD wait = SUCCEEDED(hr) ? WaitForSingleObject(event, 5000) : WAIT_FAILED;
        CloseHandle(event);
        if (FAILED(hr) || wait != WAIT_OBJECT_0) dma_error(2, FAILED(hr) ? hr : HRESULT_FROM_WIN32(ERROR_TIMEOUT));
    }
    Device* device = self->device;
    { std::lock_guard lock(device->ownership); self->engine->claimed = false; }
    delete self;
    release_device(device);
    return 0;
}
void STDMETHODCALLTYPE dma_get_device(Dma* self, void** out) {
    if (out) { add_ref(self->device); *out = self->device; }
}
UINT STDMETHODCALLTYPE dma_get_type(Dma*) { return 2; } // Original 6.2.11295 slot 8.
UINT64 STDMETHODCALLTYPE dma_insert_fence(Dma* self, UINT flags) {
    if (flags & ~1u) dma_error(16, DXGI_ERROR_UNSUPPORTED);
    std::lock_guard lock(self->mutex);
    const UINT64 value = self->engine->issued.fetch_add(1) + 1;
    if (value > dma_sequence_mask) dma_error(16, HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW));
    dma_append_locked(self, {self->engine->fence.Get(), value, false}, 16);
    if (!(flags & 1)) { // Original bit 0 defers submission.
        const HRESULT hr = dma_submit_locked(self);
        if (FAILED(hr)) dma_error(16, hr);
    }
    return (UINT64{self->selector} << 48) | value;
}
void STDMETHODCALLTYPE dma_wait_fence(Dma* self, UINT flags, UINT64 token) {
    if (flags) dma_error(17, DXGI_ERROR_UNSUPPORTED);
    if (!token) return;
    UINT64 value = 0;
    auto producer = dma_fence_engine(self->device, token, value);
    std::lock_guard lock(self->mutex);
    dma_append_locked(self, {producer->fence.Get(), value, true}, 17);
}
HRESULT STDMETHODCALLTYPE dma_submit(Dma* self) {
    std::lock_guard lock(self->mutex);
    return dma_submit_locked(self);
}
const std::array<const void*, vtable_slots>& dma_vtable() {
    static const auto table = [] {
        auto t = traps<kind_dma>();
        t[0] = reinterpret_cast<const void*>(&dma_query);
        t[1] = reinterpret_cast<const void*>(&dma_add_ref);
        t[2] = reinterpret_cast<const void*>(&dma_release);
        t[3] = reinterpret_cast<const void*>(&dma_get_device);
        t[8] = reinterpret_cast<const void*>(&dma_get_type);
        t[16] = reinterpret_cast<const void*>(&dma_insert_fence);
        t[17] = reinterpret_cast<const void*>(&dma_wait_fence);
        t[18] = reinterpret_cast<const void*>(&dma_submit);
        return t;
    }();
    return table;
}
struct DmaDescription { UINT flags, ring_bytes, segment_bytes; };
HRESULT STDMETHODCALLTYPE device_create_dma(Device* self, const DmaDescription* description, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!description) return E_INVALIDARG;
    const auto desc = *description;
    if (!self->legacy || (desc.flags != 1 && desc.flags != 2) || desc.segment_bytes ||
        (desc.ring_bytes != 0 && desc.ring_bytes != 65536)) return DXGI_ERROR_UNSUPPORTED;
    try {
        auto object = std::make_unique<Dma>();
        object->ring_bytes = desc.ring_bytes ? desc.ring_bytes : 0x80000; // Original default.
        object->pending.reserve(object->ring_bytes / sizeof(DmaOperation));
        std::lock_guard lock(self->ownership);
        auto engine = self->dma[desc.flags];
        if (engine && engine->claimed) return DXGI_ERROR_INVALID_CALL;
        if (!engine) {
            engine = std::make_shared<DmaEngine>();
            D3D12_COMMAND_QUEUE_DESC q{D3D12_COMMAND_LIST_TYPE_COPY};
            HRESULT hr = self->d3d12->CreateCommandQueue(&q, IID_PPV_ARGS(&engine->queue));
            if (SUCCEEDED(hr)) hr = self->d3d12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&engine->fence));
            if (FAILED(hr)) return hr;
            self->dma[desc.flags] = engine;
        }
        object->vtable = dma_vtable().data();
        object->flags = 1; object->refcount = 0;
        object->device = self; object->engine = engine; object->selector = desc.flags;
        engine->claimed = true;
        ++self->legacy_dependents;
        *out = object.release();
        std::fprintf(stderr, "xone-era: DMA engine %u created on a separate D3D12 copy queue\n", desc.flags);
        return S_OK;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}
BOOL STDMETHODCALLTYPE device_fence_pending(Device* self, UINT64 token) {
    if (!token) return FALSE;
    UINT64 value = 0;
    auto engine = dma_fence_engine(self, token, value);
    const UINT64 completed = engine->fence->GetCompletedValue();
    if (completed == UINT64_MAX) dma_error(60, DXGI_ERROR_DEVICE_REMOVED);
    return completed < value;
}

ULONG STDMETHODCALLTYPE dxgi_add_ref(DxgiObject* self) { return add_ref(self); }
ULONG STDMETHODCALLTYPE dxgi_release(DxgiObject* self) {
    const ULONG left = InterlockedDecrement(counter(self));
    if (left == 0) {
        for (auto* texture : self->backbuffers) if (texture) texture_release(texture);
        if (self->host) self->host->Release();
        release_device(self->device);
        delete self;
    }
    return left;
}
HRESULT new_dxgi(const void* const* vtable, Device* device, void** out) {
    auto* object = new (std::nothrow) DxgiObject;
    if (!object) return E_OUTOFMEMORY;
    object->vtable = device->legacy ? legacy_dxgi_vtable(vtable) : vtable;
    object->flags = device->legacy ? 1 : 0;
    object->refcount = device->legacy ? 0 : 1;
    object->device = device;
    retain_device(device);
    *out = object;
    return S_OK;
}
const std::array<const void*, vtable_slots>& dxgi_device_vtable();
const std::array<const void*, vtable_slots>& adapter_vtable();

HRESULT STDMETHODCALLTYPE dxgi_device_query(DxgiObject* self, REFIID riid, void** out) {
    return query(self, riid, out, {iid_unknown, iid_graphics_unknown, iid_dxgi_object, iid_dxgi_device, iid_dxgi_device1});
}
HRESULT STDMETHODCALLTYPE dxgi_device_get_adapter(DxgiObject* self, void** adapter) {
    if (!adapter) return E_INVALIDARG;
    *adapter = nullptr;
    return new_dxgi(adapter_vtable().data(), self->device, adapter);
}
HRESULT STDMETHODCALLTYPE adapter_query(DxgiObject* self, REFIID riid, void** out) {
    return query(self, riid, out, {iid_unknown, iid_graphics_unknown, iid_dxgi_object, iid_dxgi_adapter});
}
const std::array<const void*, vtable_slots>& factory_vtable();
HRESULT STDMETHODCALLTYPE factory_query(DxgiObject* self, REFIID riid, void** out) {
    return query(self, riid, out, {iid_unknown, iid_graphics_unknown, iid_dxgi_object, iid_dxgi_factory2});
}
HRESULT STDMETHODCALLTYPE adapter_get_parent(DxgiObject* self, REFIID riid, void** parent) {
    if (!parent) return E_INVALIDARG;
    *parent = nullptr;
    if (IsEqualGUID(riid, iid_dxgi_factory2)) return new_dxgi(factory_vtable().data(), self->device, parent);
    wchar_t requested[40] = L"?";
    StringFromGUID2(riid, requested, 40);
    std::fprintf(stderr, "xone-era: IDXGIAdapter::GetParent %ls is not implemented\n", requested);
    return E_NOINTERFACE;
}
HRESULT STDMETHODCALLTYPE adapter_enum_outputs(DxgiObject* self, UINT index, IDXGIOutput** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    IDXGIFactory4* factory = nullptr;
    IDXGIAdapter1* adapter = nullptr;
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (SUCCEEDED(hr)) hr = factory->EnumAdapterByLuid(self->device->d3d12->GetAdapterLuid(), IID_PPV_ARGS(&adapter));
    // IDXGIOutput retains the standard COM contract in this ABI.
    if (SUCCEEDED(hr)) hr = adapter->EnumOutputs(index, out);
    if (adapter) adapter->Release();
    if (factory) factory->Release();
    return hr;
}

const std::array<const void*, vtable_slots>& dxgi_device_vtable() {
    static const auto table = [] {
        auto t = traps<kind_dxgi_device>();
        t[0] = reinterpret_cast<const void*>(&dxgi_device_query);
        t[1] = reinterpret_cast<const void*>(&dxgi_add_ref);
        t[2] = reinterpret_cast<const void*>(&dxgi_release);
        t[7] = reinterpret_cast<const void*>(&dxgi_device_get_adapter);
        return t;
    }();
    return table;
}
const std::array<const void*, vtable_slots>& swapchain_vtable();
HRESULT STDMETHODCALLTYPE swapchain_query(DxgiObject* self, REFIID riid, void** out) {
    return query(self, riid, out, {iid_unknown, iid_graphics_unknown, iid_dxgi_object, iid_dxgi_swapchain1});
}
HRESULT STDMETHODCALLTYPE swapchain_get_buffer(DxgiObject* self, UINT index, REFIID iid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!self->host || index >= self->requested_desc.BufferCount || index >= self->backbuffers.size()) return DXGI_ERROR_INVALID_CALL;
    std::lock_guard lock(self->swapchain_mutex);
    auto*& texture = self->backbuffers[index];
    if (!texture) {
        Microsoft::WRL::ComPtr<ID3D12Resource> native;
        HRESULT hr = static_cast<IDXGISwapChain1*>(self->host)->GetBuffer(index, IID_PPV_ARGS(&native));
        if (FAILED(hr)) return hr;
        const auto d = native->GetDesc();
        if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.Width > 16384 || d.Height > 16384 ||
            d.SampleDesc.Count != 1 || d.DepthOrArraySize != 1) return DXGI_ERROR_UNSUPPORTED;
        LegacyTexture2DDesc desc{};
        desc.standard = {UINT(d.Width), d.Height, 1, 1, self->requested_desc.Format, {1, 0}, D3D11_USAGE_DEFAULT,
            D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, 0, 0};
        TitleTextureLayout layout;
        hr = create_xg_texture_layout(desc.standard.Width, desc.standard.Height, 1, 1, desc.standard.Format,
            UINT_MAX, layout, desc.standard.BindFlags);
        if (FAILED(hr)) return hr;
        void* object = nullptr;
        hr = make_texture2d(self->device, desc, layout, nullptr, &object);
        if (FAILED(hr)) return hr;
        texture = static_cast<Texture*>(object);
    }
    return texture_query(texture, iid, out);
}

// The swapchain presents straight onto the CoreWindow's HWND through a D3D12
// flip-model swapchain on the device queue (maintainer decision: no Presenter copy).
DXGI_FORMAT strip_srgb(DXGI_FORMAT f) {
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
    default: return f;
    }
}
HRESULT STDMETHODCALLTYPE factory_create_for_core_window(DxgiObject* self, void* device, IUnknown* window,
                                                         const DXGI_SWAP_CHAIN_DESC1* desc, void* restrict_output,
                                                         void** out) {
    if (!out) return DXGI_ERROR_INVALID_CALL;
    *out = nullptr;
    if (!desc || !device) return DXGI_ERROR_INVALID_CALL;
    std::fprintf(stderr, "xone-era: CreateSwapChainForCoreWindow %ux%u format %d buffers %u effect %d scaling %d alpha %d "
                 "usage 0x%X flags 0x%X samples %u\n", desc->Width, desc->Height, static_cast<int>(desc->Format),
                 desc->BufferCount, static_cast<int>(desc->SwapEffect), static_cast<int>(desc->Scaling),
                 static_cast<int>(desc->AlphaMode), desc->BufferUsage, desc->Flags, desc->SampleDesc.Count);
    if (device != self->device || restrict_output) {
        if (stop_on_missing_service()) std::fprintf(stderr,"xone-era: swapchain binding refused device %p expected %p restrict %p\n",device,self->device,restrict_output);
        return DXGI_ERROR_INVALID_CALL;
    }
    if ((self->device->legacy || modern_display_abi_bound) && (desc->Flags & 0x100000)) {
        if (stop_on_missing_service()) {
            std::fputs("xone-era: Xbox swapchain display flag 0x100000 remains unsupported\n", stderr);
            return static_cast<HRESULT>(trap_slot<kind_factory, 17>(self,
                reinterpret_cast<std::uintptr_t>(device), reinterpret_cast<std::uintptr_t>(window), reinterpret_cast<std::uintptr_t>(desc)));
        }
        return DXGI_ERROR_UNSUPPORTED;
    }
    HWND hwnd = core_window_hwnd(window);
    if (!hwnd) {
        if (stop_on_missing_service()) std::fprintf(stderr,"xone-era: swapchain window identity refused %p\n",window);
        return DXGI_ERROR_INVALID_CALL;
    }
    std::unique_ptr<DisplayTransfer> display;
    if ((self->device->legacy || modern_display_abi_bound) && (desc->Flags & 0x200000)) {
        display.reset(new (std::nothrow) DisplayTransfer);
        if (!display) return E_OUTOFMEMORY;
        const HRESULT hr = display->initialize(self->device->d3d12, strip_srgb(desc->Format));
        if (FAILED(hr)) return hr;
    }
    DXGI_SWAP_CHAIN_DESC1 host = *desc;
    if (display) host.Flags &= ~0x200000u; // Consumed by the retained GPU transfer, not forwarded to desktop DXGI.
    host.Format = strip_srgb(desc->Format); // Flip-model buffers cannot be sRGB; views keep the title's format.
    if (host.BufferCount < 2) host.BufferCount = 2;
    if (host.SwapEffect != DXGI_SWAP_EFFECT_FLIP_DISCARD) host.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    if (host.Scaling == DXGI_SCALING_ASPECT_RATIO_STRETCH) host.Scaling = DXGI_SCALING_STRETCH; // HWND swapchains reject it.
    host.AlphaMode = DXGI_ALPHA_MODE_UNSPECIFIED;
    IDXGIFactory2* factory = nullptr;
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    IDXGISwapChain1* chain = nullptr;
    if (SUCCEEDED(hr)) hr = factory->CreateSwapChainForHwnd(self->device->queue, hwnd, &host, nullptr, nullptr, &chain);
    if (SUCCEEDED(hr)) factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    if (factory) factory->Release();
    if (FAILED(hr)) {
        std::fprintf(stderr, "xone-era: host CreateSwapChainForHwnd -> 0x%08lX\n", static_cast<unsigned long>(hr));
        return hr;
    }
    hr = new_dxgi(swapchain_vtable().data(), self->device, out);
    if (FAILED(hr)) { chain->Release(); return hr; }
    static_cast<DxgiObject*>(*out)->host = chain;
    static_cast<DxgiObject*>(*out)->display = std::move(display);
    static_cast<DxgiObject*>(*out)->requested_desc = *desc;
    if (stop_on_missing_service()) std::fprintf(stderr,"xone-era: native swapchain created %p\n",*out);
    if (static_cast<DxgiObject*>(*out)->display)
        std::fputs("xone-era: Xbox swapchain 0x200000 prepared with native sRGB-to-BT.709 transfer\n", stderr);
    return S_OK;
}

const std::array<const void*, vtable_slots>& swapchain_vtable() {
    static const auto table = [] {
        auto t = traps<kind_swapchain>();
        t[0] = reinterpret_cast<const void*>(&swapchain_query);
        t[1] = reinterpret_cast<const void*>(&dxgi_add_ref);
        t[2] = reinterpret_cast<const void*>(&dxgi_release);
        return t;
    }();
    return table;
}
const std::array<const void*, vtable_slots>& factory_vtable() {
    static const auto table = [] {
        auto t = traps<kind_factory>();
        t[0] = reinterpret_cast<const void*>(&factory_query);
        t[1] = reinterpret_cast<const void*>(&dxgi_add_ref);
        t[2] = reinterpret_cast<const void*>(&dxgi_release);
        t[16] = reinterpret_cast<const void*>(&factory_create_for_core_window);
        return t;
    }();
    return table;
}
const std::array<const void*, vtable_slots>& adapter_vtable() {
    static const auto table = [] {
        auto t = traps<kind_adapter>();
        t[0] = reinterpret_cast<const void*>(&adapter_query);
        t[1] = reinterpret_cast<const void*>(&dxgi_add_ref);
        t[2] = reinterpret_cast<const void*>(&dxgi_release);
        t[6] = reinterpret_cast<const void*>(&adapter_get_parent);
        return t;
    }();
    return table;
}

const void* const* legacy_dxgi_vtable(const void* const* modern) {
    // Before 6.2.13004, SetPrivateDataInterfaceGraphics occupies slot 5.
    // Keep every other unsupported slot fatal instead of shifting modern traps.
    static const auto device = [] {
        auto t = traps<kind_dxgi_device>();
        t[0] = reinterpret_cast<const void*>(&dxgi_device_query);
        t[1] = reinterpret_cast<const void*>(&dxgi_add_ref);
        t[2] = reinterpret_cast<const void*>(&dxgi_release);
        t[8] = reinterpret_cast<const void*>(&dxgi_device_get_adapter);
        return t;
    }();
    static const auto adapter = [] {
        auto t = traps<kind_adapter>();
        t[0] = reinterpret_cast<const void*>(&adapter_query);
        t[1] = reinterpret_cast<const void*>(&dxgi_add_ref);
        t[2] = reinterpret_cast<const void*>(&dxgi_release);
        t[7] = reinterpret_cast<const void*>(&adapter_get_parent);
        t[8] = reinterpret_cast<const void*>(&adapter_enum_outputs);
        return t;
    }();
    static const auto factory = [] {
        auto t = traps<kind_factory>();
        t[0] = reinterpret_cast<const void*>(&factory_query);
        t[1] = reinterpret_cast<const void*>(&dxgi_add_ref);
        t[2] = reinterpret_cast<const void*>(&dxgi_release);
        t[17] = reinterpret_cast<const void*>(&factory_create_for_core_window);
        return t;
    }();
    static const auto swapchain = [] {
        auto t = traps<kind_swapchain>();
        t[0] = reinterpret_cast<const void*>(&swapchain_query);
        t[1] = reinterpret_cast<const void*>(&dxgi_add_ref);
        t[2] = reinterpret_cast<const void*>(&dxgi_release);
        t[10] = reinterpret_cast<const void*>(&swapchain_get_buffer);
        return t;
    }();
    if (modern == dxgi_device_vtable().data()) return device.data();
    if (modern == adapter_vtable().data()) return adapter.data();
    if (modern == factory_vtable().data()) return factory.data();
    if (modern == swapchain_vtable().data()) return swapchain.data();
    default_trap("unbound legacy DXGI object", 0);
}

HRESULT STDMETHODCALLTYPE context_query(Context* self, REFIID riid, void** out) {
    if (self->device->legacy && IsEqualGUID(riid, iid_performance_context))
        return query(self, riid, out, {iid_performance_context});
    return query(self, riid, out, {iid_unknown, iid_graphics_unknown, iid_context, iid_context_x});
}
ULONG STDMETHODCALLTYPE context_add_ref(Context* self) { return add_ref(self); }
ULONG STDMETHODCALLTYPE context_release(Context* self) {
    const ULONG left = InterlockedDecrement(counter(self));
    if (left == 0) {
        if (self->device->legacy && !self->deferred) release_device(self->device);
        else { release_device(self->device); delete self; }
    }
    return left;
}
void STDMETHODCALLTYPE context_get_device(Context* self, void** out) {
    if (!out) return;
    add_ref(self->device);
    *out = self->device;
}
void STDMETHODCALLTYPE device_get_context(Device* self, void** out) {
    if (!out) return;
    context_add_ref(self->immediate);
    *out = self->immediate;
}
const std::array<const void*, vtable_slots>& legacy_context_vtable();
void initialize_legacy_context(Context* context) {
    context->functions = legacy_context_vtable();
    context->vtable = context->functions.data();
    // Use the documented out-of-line resource setters instead of handing the
    // title an unimplemented native GPU command ring. Those setters still trap.
    context->inline_state[0x370] = 1; // Absolute public offset 0x390.
}
HRESULT STDMETHODCALLTYPE device_create_deferred(Device* self, UINT flags, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!self->legacy || flags) return DXGI_ERROR_UNSUPPORTED;
    auto* context = new (std::nothrow) Context;
    if (!context) return E_OUTOFMEMORY;
    context->device = self;
    context->flags = 1; context->refcount = 0; context->deferred = true;
    initialize_legacy_context(context);
    retain_device(self);
    *out = context;
    return S_OK;
}

// ID3D11DeviceX slot 85 (layout from 55: GetImmediateContextX ... 85 SetGpuMemoryPriority).
// shortcut: priority has no host effect, the title's memory does not compete with a
// system partition here; upgrade when D3D12 residency management (MakeResident/Evict) exists.
HRESULT STDMETHODCALLTYPE device_set_gpu_memory_priority(Device*, UINT) { return S_OK; }

const std::array<const void*, vtable_slots>& device_vtable() {
    static const auto table = [] {
        auto t = traps<kind_device>();
        t[0] = reinterpret_cast<const void*>(&device_query);
        t[1] = reinterpret_cast<const void*>(&device_add_ref);
        t[2] = reinterpret_cast<const void*>(&device_release);
        t[85] = reinterpret_cast<const void*>(&device_set_gpu_memory_priority);
        return t;
    }();
    return table;
}
UINT STDMETHODCALLTYPE device_creation_flags(Device* self) { return self->creation_flags; }
D3D_FEATURE_LEVEL STDMETHODCALLTYPE device_feature_level(Device* self) { return self->level; }
HRESULT STDMETHODCALLTYPE device_removed_reason(Device* self) { return self->d3d12->GetDeviceRemovedReason(); }
const std::array<const void*, vtable_slots>& legacy_device_vtable() {
    static const auto table = [] {
        auto t = traps<kind_device>();
        t[0] = reinterpret_cast<const void*>(&device_query);
        t[1] = reinterpret_cast<const void*>(&device_add_ref);
        t[2] = reinterpret_cast<const void*>(&device_release);
        t[3] = reinterpret_cast<const void*>(&device_create_buffer);
        t[5] = reinterpret_cast<const void*>(&device_create_texture2d);
        t[6] = reinterpret_cast<const void*>(&device_create_texture3d);
        t[7] = reinterpret_cast<const void*>(&device_create_srv);
        t[64] = reinterpret_cast<const void*>(&device_create_placement_texture2d);
        t[65] = reinterpret_cast<const void*>(&device_create_placement_texture3d);
        t[8] = reinterpret_cast<const void*>(&device_create_uav);
        t[9] = reinterpret_cast<const void*>(&device_create_rtv);
        t[12] = reinterpret_cast<const void*>(&device_create_shader<1>);
        t[13] = reinterpret_cast<const void*>(&device_create_shader<2>);
        t[11] = reinterpret_cast<const void*>(&device_create_input_layout);
        t[15] = reinterpret_cast<const void*>(&device_create_shader<0>);
        t[18] = reinterpret_cast<const void*>(&device_create_shader<5>);
        t[20] = reinterpret_cast<const void*>(&device_create_blend_state);
        t[21] = reinterpret_cast<const void*>(&device_create_depth_state);
        t[22] = reinterpret_cast<const void*>(&device_create_raster_state);
        t[23] = reinterpret_cast<const void*>(&device_create_sampler);
        t[27] = reinterpret_cast<const void*>(&device_create_deferred);
        t[38] = reinterpret_cast<const void*>(&device_feature_level);
        t[39] = reinterpret_cast<const void*>(&device_creation_flags);
        t[40] = reinterpret_cast<const void*>(&device_removed_reason);
        t[41] = reinterpret_cast<const void*>(&device_get_context);
        t[44] = reinterpret_cast<const void*>(&device_get_context);
        t[45] = reinterpret_cast<const void*>(&device_create_deferred);
        t[51] = reinterpret_cast<const void*>(&device_get_context);
        t[52] = reinterpret_cast<const void*>(&device_create_deferred);
        t[55] = reinterpret_cast<const void*>(&device_get_context);
        t[59] = reinterpret_cast<const void*>(&device_create_dma);
        t[60] = reinterpret_cast<const void*>(&device_fence_pending);
        t[68] = reinterpret_cast<const void*>(&device_create_deferred);
        return t;
    }();
    return table;
}
const std::array<const void*, vtable_slots>& context_vtable() {
    static const auto table = [] {
        auto t = traps<kind_context>();
        t[0] = reinterpret_cast<const void*>(&context_query);
        t[1] = reinterpret_cast<const void*>(&context_add_ref);
        t[2] = reinterpret_cast<const void*>(&context_release);
        t[3] = reinterpret_cast<const void*>(&context_get_device);
        return t;
    }();
    return table;
}
UINT STDMETHODCALLTYPE context_get_type(Context* self) { return self->deferred ? 1 : 0; }
UINT STDMETHODCALLTYPE context_get_flags(Context*) { return 0; } // Only creation flags zero are supported.
void STDMETHODCALLTYPE context_automatic_gpu_flush(Context* self, BOOL enabled) { self->automatic_gpu_flush = enabled != FALSE; }
template<class T, std::size_t Offset> T context_state(Context* self) {
    static_assert(Offset >= 32 && Offset + sizeof(T) <= 0x3d0);
    T value;
    std::memcpy(&value, self->inline_state.data() + Offset - 32, sizeof value);
    return value;
}
template<class T, std::size_t Offset> void context_state(Context* self, T value) {
    static_assert(Offset >= 32 && Offset + sizeof(T) <= 0x3d0);
    std::memcpy(self->inline_state.data() + Offset - 32, &value, sizeof value);
}
template<unsigned Slot, std::size_t Offset, UINT32 DirtyMask, UINT64 StageMask>
void STDMETHODCALLTYPE context_unbind_shader_stage(Context* self, void* shader) {
    if (shader) default_trap("shader translation", Slot);
    // Native 6.2.11295 unbinding updates the public shader data/dirty
    // state. A non-null program needs a translated shader, never a dummy shader.
    if (context_state<UINT64, Offset>(self)) {
        context_state<UINT64, Offset>(self, 0);
        context_state<UINT32, 0x168>(self, context_state<UINT32, 0x168>(self) | DirtyMask);
    }
    context_state<UINT64, 0x218>(self, context_state<UINT64, 0x218>(self) | StageMask);
}
void STDMETHODCALLTYPE context_unbind_geometry_shader(Context* self, void* shader) {
    context_unbind_shader_stage<24, 0x1d0, 0x2800, 8>(self, shader);
    context_unbind_shader_stage<24, 0x1d8, 0x3000, 8>(self, shader);
}
HRESULT STDMETHODCALLTYPE context_map_buffer(Context* self, Buffer* resource, UINT subresource,
        D3D11_MAP mode, UINT flags, D3D11_MAPPED_SUBRESOURCE* out) {
    if (!out) return E_INVALIDARG;
    *out = {};
    if (!resource || resource->vtable != buffer_vtable().data() || resource->device != self->device || subresource ||
        resource->desc.standard.Usage != D3D11_USAGE_DYNAMIC || resource->desc.standard.CPUAccessFlags != D3D11_CPU_ACCESS_WRITE)
        return E_INVALIDARG;
    const auto unsupported = [&]() -> HRESULT {
        if (stop_on_missing_service()) return static_cast<HRESULT>(trap_slot<kind_context, 15>(self,
            reinterpret_cast<std::uintptr_t>(resource), subresource, mode, flags));
        return DXGI_ERROR_UNSUPPORTED;
    };
    if (self->deferred || flags) return unsupported();
    if (mode != D3D11_MAP_WRITE_DISCARD && mode != D3D11_MAP_WRITE_NO_OVERWRITE) return E_INVALIDARG;
    if (mode == D3D11_MAP_WRITE_NO_OVERWRITE &&
        (!(resource->desc.standard.BindFlags & (D3D11_BIND_VERTEX_BUFFER | D3D11_BIND_INDEX_BUFFER)) ||
         (resource->desc.standard.BindFlags & D3D11_BIND_SHADER_RESOURCE))) return unsupported();
    void* address = nullptr;
    const HRESULT hr = map_title_gpu_resource(resource->gpu, &address);
    if (hr == E_PENDING) {
        return unsupported(); // Blocking maps need the future command submission's fence ownership.
    }
    if (FAILED(hr)) return hr;
    out->pData = address;
    out->RowPitch = out->DepthPitch = resource->desc.standard.ByteWidth; // Original 180051690, buffer span from SRD stride/count.
    return S_OK;
}
void STDMETHODCALLTYPE context_unmap_buffer(Context* self, Buffer* resource, UINT subresource) {
    if (!resource || resource->vtable != buffer_vtable().data() || resource->device != self->device || subresource ||
        FAILED(unmap_title_gpu_resource(resource->gpu))) default_trap("ID3D11DeviceContextX", 16);
}
const std::array<const void*, vtable_slots>& legacy_context_vtable() {
    static const auto table = [] {
        auto t = context_vtable();
        t[15] = reinterpret_cast<const void*>(&context_map_buffer); // Original 18000ff10, not desktop slot 14.
        t[16] = reinterpret_cast<const void*>(&context_unmap_buffer); // Original 1800104f0.
        t[24] = reinterpret_cast<const void*>(&context_unbind_geometry_shader);
        t[60] = reinterpret_cast<const void*>(&context_unbind_shader_stage<60, 0x1c0, 0x2200, 2>);
        t[64] = reinterpret_cast<const void*>(&context_unbind_shader_stage<64, 0x1c8, 0x2400, 4>);
        // Native 6.2.11295 table: public reference headers differ by one here.
        t[112] = reinterpret_cast<const void*>(&context_get_type);
        t[113] = reinterpret_cast<const void*>(&context_get_flags);
        t[196] = reinterpret_cast<const void*>(&context_automatic_gpu_flush);
        return t;
    }();
    return table;
}

// Highest requested level the D3D12 device supports, capped at 11_1 (the
// Xbox One D3D11.X level); the default list matches D3D11's when none is given.
bool select_level(ID3D12Device* device, const D3D_FEATURE_LEVEL* levels, UINT count, D3D_FEATURE_LEVEL& out) {
    static constexpr D3D_FEATURE_LEVEL defaults[]{D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0,
                                                  D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0};
    if (!levels) { levels = defaults; count = static_cast<UINT>(std::size(defaults)); }
    for (UINT i = 0; i < count; ++i) {
        if (levels[i] > D3D_FEATURE_LEVEL_11_1) continue;
        D3D12_FEATURE_DATA_FEATURE_LEVELS query{1, &levels[i], D3D_FEATURE_LEVEL_9_1};
        if (SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FEATURE_LEVELS, &query, sizeof query)) &&
            query.MaxSupportedFeatureLevel >= levels[i]) {
            out = levels[i];
            return true;
        }
    }
    return false;
}

HRESULT create_d3d12(D3D_DRIVER_TYPE type, ID3D12Device** out) {
    if (type == D3D_DRIVER_TYPE_HARDWARE || type == D3D_DRIVER_TYPE_UNKNOWN)
        return D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(out));
    if (type != D3D_DRIVER_TYPE_WARP) return DXGI_ERROR_UNSUPPORTED;
    IDXGIFactory4* factory = nullptr;
    HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) return hr;
    IDXGIAdapter* warp = nullptr;
    hr = factory->EnumWarpAdapter(IID_PPV_ARGS(&warp));
    factory->Release();
    if (FAILED(hr)) return hr;
    hr = D3D12CreateDevice(warp, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(out));
    warp->Release();
    return hr;
}

HRESULT WINAPI D3D11CreateDevice(void* adapter, D3D_DRIVER_TYPE type, HMODULE software, UINT, const D3D_FEATURE_LEVEL* levels,
                                 UINT level_count, UINT, void** device_out, D3D_FEATURE_LEVEL* level_out, void** context_out) {
    if (device_out) *device_out = nullptr;
    if (context_out) *context_out = nullptr;
    if (adapter || software || (levels && level_count == 0)) return E_INVALIDARG; // Not observed on the title path.
    ID3D12Device* d3d12 = nullptr;
    HRESULT hr = create_d3d12(type, &d3d12);
    if (FAILED(hr)) return hr;
    D3D_FEATURE_LEVEL level{};
    if (!select_level(d3d12, levels, levels ? level_count : 0, level)) {
        d3d12->Release();
        return E_INVALIDARG;
    }
    if (level_out) *level_out = level;
    if (!device_out && !context_out) { d3d12->Release(); return S_FALSE; }

    D3D12_COMMAND_QUEUE_DESC queue_desc{D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0};
    ID3D12CommandQueue* queue = nullptr;
    hr = d3d12->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue));
    if (FAILED(hr)) { d3d12->Release(); return hr; }

    auto* device = new (std::nothrow) Device;
    if (!device) { queue->Release(); d3d12->Release(); return E_OUTOFMEMORY; }
    device->vtable = device_vtable().data();
    device->flags = 0;
    device->refcount = 1;
    device->d3d12 = d3d12;
    device->queue = queue;
    device->level = level;
    if (context_out) {
        auto* context = new (std::nothrow) Context;
        if (!context) { device_release(device); return E_OUTOFMEMORY; }
        context->vtable = context_vtable().data();
        context->flags = 0;
        context->refcount = 1;
        context->device = device;
        add_ref(device);
        *context_out = context;
    }
    if (device_out) *device_out = device;
    else device_release(device);
    return S_OK;
}

// Layout checked against the original 6.2.11295 D3D11.X creation boundary.
struct CreateParameters {
    UINT version, flags;
    void* tessellation_buffer;
    void* tessellation_factors;
    UINT deletion_affinity, de_ring_bytes, ce_ring_bytes, de_segment_bytes, ce_segment_bytes;
};
static_assert(sizeof(CreateParameters) == 48 && offsetof(CreateParameters, deletion_affinity) == 24);
HRESULT WINAPI D3D11XCreateDeviceX(const CreateParameters* parameters, void** device_out, void** context_out) {
    if (device_out) *device_out = nullptr;
    if (context_out) *context_out = nullptr;
    if (!parameters || !device_out || !context_out || device_out == context_out) return E_INVALIDARG;
    const auto p = *parameters;
    std::fprintf(stderr, "xone-era: D3D11XCreateDeviceX version 0x%X flags 0x%X affinity 0x%X\n",
                 p.version, p.flags, p.deletion_affinity);
    if (p.version != 0x1000b) return E_INVALIDARG;
    // Only synchronous deletion and default command/tessellation storage have
    // a translated contract. Other flags and caller-owned GPU rings are explicit gaps.
    if (!legacy_abi_bound || p.flags != 0x04000000 || p.tessellation_buffer || p.tessellation_factors ||
        p.de_ring_bytes || p.ce_ring_bytes || p.de_segment_bytes || p.ce_segment_bytes ||
        (p.deletion_affinity & ~0x3fu)) {
        std::fputs("xone-era: unsupported D3D11.X creation parameters\n", stderr);
        return DXGI_ERROR_UNSUPPORTED;
    }
    void* raw_device = nullptr;
    void* raw_context = nullptr;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, 0,
                                   &raw_device, nullptr, &raw_context);
    if (FAILED(hr)) {
        std::fprintf(stderr, "xone-era: D3D11.X host device creation failed 0x%08lX\n", hr);
        return hr;
    }
    auto* device = static_cast<Device*>(raw_device);
    auto* context = static_cast<Context*>(raw_context);
    ID3D12Fence* fence = nullptr;
    hr = device->d3d12->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    HANDLE completed = SUCCEEDED(hr) ? CreateEventW(nullptr, FALSE, FALSE, nullptr) : nullptr;
    if (SUCCEEDED(hr) && !completed) hr = HRESULT_FROM_WIN32(GetLastError());
    if (FAILED(hr)) {
        if (fence) fence->Release();
        context_release(context);
        device_release(device);
        return hr;
    }
    device->legacy = true;
    device->creation_flags = p.flags;
    device->vtable = legacy_device_vtable().data();
    device->flags = 1;
    device->refcount = 0;
    device->legacy_dependents = 1;
    device->immediate = context;
    device->retirement = fence;
    device->retired = completed;
    context->flags = context->refcount;
    context->refcount = 0;
    initialize_legacy_context(context);
    *device_out = device;
    *context_out = context;
    std::fputs("xone-era: D3D11.X ABI 6.2.11295.0 device created; synchronous deletion\n", stderr);
    return S_OK;
}

} // namespace

void set_d3d11x_trap_handler(D3D11XTrapHandler handler) { trap_handler.store(handler); }

HRESULT bind_d3d11x_abi(const std::wstring& original_combase) {
    legacy_abi_bound = false;
    modern_display_abi_bound = false;
    DWORD ignored = 0;
    const auto path = detail::file_path(std::filesystem::path(original_combase));
    const DWORD bytes = GetFileVersionInfoSizeW(path.c_str(), &ignored);
    if (!bytes) return HRESULT_FROM_WIN32(GetLastError());
    if (bytes > (1u << 20)) return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
    try {
        std::vector<std::uint8_t> info(bytes);
        if (!GetFileVersionInfoW(path.c_str(), 0, bytes, info.data())) return HRESULT_FROM_WIN32(GetLastError());
        VS_FIXEDFILEINFO* version = nullptr;
        UINT size = 0;
        if (!VerQueryValueW(info.data(), L"\\", reinterpret_cast<void**>(&version), &size) ||
            !version || size < sizeof(*version) || version->dwSignature != VS_FFI_SIGNATURE)
            return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        if (version->dwFileVersionMS == MAKELONG(0,10) && version->dwFileVersionLS == MAKELONG(5104,16299)) {
            // The display flag is independent of the modern object's prefix;
            // other original module/texture ABIs remain deliberately unbound.
            Microsoft::WRL::Wrappers::FileHandle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
            if (!file.IsValid()) return HRESULT_FROM_WIN32(GetLastError());
            LARGE_INTEGER length{};
            if (!GetFileSizeEx(file.Get(),&length)) return HRESULT_FROM_WIN32(GetLastError());
            if (length.QuadPart != 896000) return HRESULT_FROM_WIN32(ERROR_CRC);
            detail::Sha256 hash;
            std::array<std::uint8_t,16384> block{};
            DWORD read = 0;
            do {
                if (!ReadFile(file.Get(),block.data(),static_cast<DWORD>(block.size()),&read,nullptr)) return HRESULT_FROM_WIN32(GetLastError());
                hash.update(std::span<const std::uint8_t>(block.data(),read));
            } while (read);
            if (hash.finish() != "c22ac3a822ff2a421a995949ff36ee84a922f74f2998d084556e9d92b48cf3e5")
                return HRESULT_FROM_WIN32(ERROR_CRC);
            modern_display_abi_bound = true;
            return S_OK;
        }
        if (version->dwFileVersionMS != MAKELONG(2, 6) || version->dwFileVersionLS != MAKELONG(0, 11295))
            return DXGI_ERROR_UNSUPPORTED;
        legacy_abi_bound = true;
        return S_OK;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}

HRESULT bind_xg_module(const std::wstring& original_xg) {
    std::lock_guard lock(xg_module_mutex);
    try {
        const auto path = detail::file_path(std::filesystem::path(original_xg));
        Microsoft::WRL::Wrappers::FileHandle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
        if (!file.IsValid()) return HRESULT_FROM_WIN32(GetLastError());
        LARGE_INTEGER size{};
        if (!GetFileSizeEx(file.Get(), &size)) return HRESULT_FROM_WIN32(GetLastError());
        if (size.QuadPart <= 0 || size.QuadPart > (1u << 20)) return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
        std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size.QuadPart));
        DWORD read = 0;
        if (!ReadFile(file.Get(), bytes.data(), static_cast<DWORD>(bytes.size()), &read, nullptr)) return HRESULT_FROM_WIN32(GetLastError());
        if (read != bytes.size()) return HRESULT_FROM_WIN32(ERROR_HANDLE_EOF);
        detail::Sha256 hash;
        hash.update(bytes);
        if (hash.finish() != "9e3a06d02784699c252bea271090b9e2a04ef2cb29b5b0647c6ce6e73b19db67")
            return HRESULT_FROM_WIN32(ERROR_CRC);
        if (!legacy_abi_bound) return DXGI_ERROR_UNSUPPORTED;
        if (xg_module.load()) return S_FALSE;
        auto module = std::make_shared<Image>();
        LoadResult result;
        if (!module->load(bytes, resolve_title_import, result, ImageKind::module)) {
            for (const auto& missing : result.unresolved)
                std::fprintf(stderr, "xone-era: XG unresolved %s!%s ordinal=%u\n", missing.library.c_str(), missing.name.c_str(), missing.ordinal);
            for (const auto& error : result.errors) std::fprintf(stderr, "xone-era: XG module blocked: %s\n", error.c_str());
            return HRESULT_FROM_WIN32(ERROR_BAD_EXE_FORMAT);
        }
        for (const auto* name : {"XGCreateTexture2DComputer", "XGComputeTexture2DLayout", "XGComputeOptimalTileMode"})
            if (!module->find_export(name)) return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
        xg_module.store(std::move(module));
        return S_OK;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}

HRESULT initialize_xg_module() {
    std::lock_guard lock(xg_module_mutex);
    const auto module = xg_module.load();
    if (!module) return S_FALSE;
    if (!module->attach_module()) return HRESULT_FROM_WIN32(ERROR_DLL_INIT_FAILED);
    xg_initialized = true;
    std::fputs("xone-era: original XG 6.2.11295.0 CPU layout module initialized (pinned SHA-256)\n", stderr);
    return S_OK;
}

void* resolve_xg_module(const ImportRequest& request) {
    if (!same_library(request.library, "xg_x.dll")) return nullptr;
    const auto module = xg_module.load();
    if (!module) return nullptr;
    void* address = module->find_export(request.name, request.ordinal);
    if (!address) return nullptr;
    for (const auto* name : {"XGCreateTexture1DComputer", "XGCreateTexture2DComputer", "XGCreateTexture3DComputer",
        "XGComputeOptimalDepthStencilTileModes", "XGComputeOptimalTileMode", "XGComputeTexture2DLayout", "XGComputeTexture3DLayout"})
        if (address == module->find_export(name)) return address;
    return nullptr;
}

namespace {
struct XgTextureContext {
    std::shared_ptr<Image> module;
    void* computer = nullptr;
    UINT width = 0, height = 0, mips = 0, arrays = 0, block = 0, bytes_per_block = 0, samples = 1;
    SIZE_T bytes = 0;
    ~XgTextureContext() {
        if (computer) reinterpret_cast<ULONG (*)(void*)>((*static_cast<void***>(computer))[1])(computer);
    }
};
HRESULT copy_xg_texture(void* opaque, bool into_tiled, void* linear, void* tiled,
                       UINT subresource, UINT row_pitch, UINT slice_pitch) {
    auto* context = static_cast<XgTextureContext*>(opaque);
    if (!context || !linear || !tiled || subresource >= context->mips * context->arrays) return E_INVALIDARG;
    const UINT mip = subresource % context->mips;
    const UINT width = (std::max)(1u, context->width >> mip), height = (std::max)(1u, context->height >> mip);
    const UINT row_bytes = ((width + context->block - 1) / context->block) * context->bytes_per_block * context->samples;
    const UINT rows = (height + context->block - 1) / context->block;
    if (row_pitch < row_bytes || UINT64(row_pitch) * rows > slice_pitch) return E_INVALIDARG;
    if (context->samples > 1) {
        // Original CopyInto/FromSubresource at 2b40/2fa0 hardcodes sample 0.
        // Slot 4 (2a70) accepts the sample index and returns its exact byte offset.
        using Address = UINT64 (*)(void*, UINT, UINT, UINT64, UINT, UINT, UINT);
        const auto address = reinterpret_cast<Address>((*static_cast<void***>(context->computer))[4]);
        for (UINT y = 0; y < height; ++y) for (UINT x = 0; x < width; ++x) for (UINT sample = 0; sample < context->samples; ++sample) {
            const UINT64 offset = address(context->computer, 0, mip, x, y, subresource / context->mips, sample);
            if (offset > context->bytes || context->bytes - offset < context->bytes_per_block) return E_INVALIDARG;
            auto* a = static_cast<unsigned char*>(linear) + SIZE_T(y) * row_pitch + (SIZE_T(x) * context->samples + sample) * context->bytes_per_block;
            auto* b = static_cast<unsigned char*>(tiled) + offset;
            std::memcpy(into_tiled ? b : a, into_tiled ? a : b, context->bytes_per_block);
        }
        return S_OK;
    }
    // Slots 5/6 at 180002b40/180002fa0: tiled destination / linear destination.
    using Copy = HRESULT (*)(void*, void*, UINT, UINT, const void*, UINT, UINT);
    const auto functions = *static_cast<void***>(context->computer);
    const auto copy = reinterpret_cast<Copy>(functions[into_tiled ? 5 : 6]);
    return copy(context->computer, into_tiled ? tiled : linear, 0, subresource,
                into_tiled ? linear : tiled, row_pitch, slice_pitch);
}
}

static HRESULT create_xg_texture_layout_impl(UINT width, UINT height, UINT depth, UINT mips, UINT arrays, DXGI_FORMAT format,
                                             UINT tile_mode, UINT bind_flags, UINT samples, TitleTextureLayout& out) {
    out = {};
    const auto module = xg_module.load();
    if (!module || !xg_initialized) return E_UNEXPECTED;
    if (!width || !height || width > 16384 || height > 16384 || !arrays || arrays > 2048) return E_INVALIDARG;
    if (depth > 2048 || (depth && (width > 2048 || height > 2048 || arrays != 1))) return E_INVALIDARG;
    const UINT levels = std::bit_width((std::max)((std::max)(width, height), depth));
    if (!mips) mips = levels;
    if (mips > levels || mips > 15) return E_INVALIDARG;
    if (!samples || samples > 8 || (samples & (samples - 1)) || (samples > 1 && (mips != 1 || depth))) return E_INVALIDARG;
    const bool bc7 = format == DXGI_FORMAT_BC7_TYPELESS || format == DXGI_FORMAT_BC7_UNORM || format == DXGI_FORMAT_BC7_UNORM_SRGB;
    if ((!bc7 && format != DXGI_FORMAT_R8G8B8A8_UNORM && format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB &&
         format != DXGI_FORMAT_R10G10B10A2_UNORM && format != DXGI_FORMAT_R11G11B10_FLOAT && format != DXGI_FORMAT_R8_UINT &&
         format != DXGI_FORMAT_R32G8X24_TYPELESS) ||
        ((depth || samples > 1) && bc7)) return DXGI_ERROR_UNSUPPORTED;
    if (samples > 1 && format == DXGI_FORMAT_R8_UINT) return DXGI_ERROR_UNSUPPORTED;
    try {
        auto context = std::make_shared<XgTextureContext>();
        context->module = module; context->width = width; context->height = height;
        context->mips = mips; context->arrays = arrays; context->block = bc7 ? 4 : 1; context->bytes_per_block = bc7 ? 16 : 4;
        if (format == DXGI_FORMAT_R8_UINT) context->bytes_per_block = 1;
        if (format == DXGI_FORMAT_R32G8X24_TYPELESS) context->bytes_per_block = 8;
        context->samples = samples;
        // 180052210 constructs the 60-byte XG descriptor independently of the
        // 52-byte public texture descriptor. Single-sample, default SRV storage.
        if (!depth && tile_mode == UINT_MAX) {
            using Optimal = UINT (*)(UINT, DXGI_FORMAT, UINT, UINT, UINT, UINT, UINT, UINT);
            const auto optimal = reinterpret_cast<Optimal>(module->find_export("XGComputeOptimalTileMode"));
            if (!optimal) return E_NOINTERFACE;
            tile_mode = optimal(3, format, width, height, 1, samples, bind_flags, 0);
            if (tile_mode == UINT_MAX) return DXGI_ERROR_UNSUPPORTED;
        }
        // Approved expanded canonical image: host render-target capabilities
        // remain below; Xbox auxiliary compression planes are not fabricated.
        std::array<UINT, 15> desc{width, height, mips, arrays, static_cast<UINT>(format), samples, 0, 0, 0, 0, 0, 0, 0, tile_mode, 0};
        // Original 180052210 carries the depth bind into XG. Omitting it yields
        // E_INVALIDARG and conceals the real three-plane depth/stencil contract.
        if (format == DXGI_FORMAT_R32G8X24_TYPELESS) desc[8] = D3D11_BIND_DEPTH_STENCIL;
        if (depth) {
            // Original 180052210: 52-byte volume descriptor; tile mode at +44.
            using Optimal = UINT (*)(UINT, DXGI_FORMAT, UINT, UINT, UINT, UINT, UINT, UINT);
            const auto optimal = reinterpret_cast<Optimal>(module->find_export("XGComputeOptimalTileMode"));
            if (!optimal) return E_NOINTERFACE;
            if (tile_mode == UINT_MAX) tile_mode = optimal(4, format, width, height, depth, 1, bind_flags, 0);
            if (tile_mode == UINT_MAX) return DXGI_ERROR_UNSUPPORTED;
            desc = {width, height, depth, mips, static_cast<UINT>(format), 0, 0, 0, 0, 0, 0, tile_mode, 0, 0, 0};
        }
        using Create = HRESULT (*)(const void*, void**);
        const auto create = reinterpret_cast<Create>(module->find_export(depth ? "XGCreateTexture3DComputer" : "XGCreateTexture2DComputer"));
        if (!create) return E_NOINTERFACE;
        HRESULT hr = create(desc.data(), &context->computer);
        if (FAILED(hr)) return hr;
        if (!context->computer) return E_UNEXPECTED;
        struct { alignas(16) std::array<std::uint8_t, 0xd40> bytes{}; UINT64 guard = 0xaabbccdd11223344ull; } layout;
        using GetLayout = HRESULT (*)(void*, void*);
        const auto get_layout = reinterpret_cast<GetLayout>((*static_cast<void***>(context->computer))[2]);
        hr = get_layout(context->computer, layout.bytes.data());
        if (FAILED(hr)) return hr;
        if (layout.guard != 0xaabbccdd11223344ull) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
        UINT64 bytes = 0, alignment = 0;
        UINT mip_count = 0, plane_count = 0;
        std::memcpy(&bytes, layout.bytes.data(), 8); std::memcpy(&alignment, layout.bytes.data() + 8, 8);
        std::memcpy(&mip_count, layout.bytes.data() + 16, 4); std::memcpy(&plane_count, layout.bytes.data() + 20, 4);
        if (!bytes || bytes > (1ull << 30) || !alignment || alignment > (1u << 21) || (alignment & (alignment - 1)) ||
            mip_count != mips || plane_count != 1) {
            if (stop_on_missing_service()) std::fprintf(stderr, "xone-era: unsupported XG layout header size %llu alignment %llu mips %u planes %u\n",
                static_cast<unsigned long long>(bytes), static_cast<unsigned long long>(alignment), mip_count, plane_count);
            return DXGI_ERROR_UNSUPPORTED;
        }
        out.bytes = static_cast<SIZE_T>(bytes); out.alignment = static_cast<SIZE_T>(alignment);
        context->bytes = out.bytes;
        out.resource_desc.Dimension = depth ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        out.resource_desc.Width = width; out.resource_desc.Height = height;
        out.resource_desc.DepthOrArraySize = static_cast<UINT16>(depth ? depth : arrays); out.resource_desc.MipLevels = static_cast<UINT16>(mips);
        out.resource_desc.Format = format; out.resource_desc.SampleDesc.Count = samples;
        if (bind_flags & D3D11_BIND_UNORDERED_ACCESS) out.resource_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (bind_flags & D3D11_BIND_RENDER_TARGET) out.resource_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        if (bind_flags & D3D11_BIND_DEPTH_STENCIL) out.resource_desc.Flags |= D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        std::memcpy(out.guest_descriptor.data(), static_cast<unsigned char*>(context->computer) + 0x10, sizeof out.guest_descriptor);
        out.context = std::move(context); out.copy = &copy_xg_texture;
        return S_OK;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}

HRESULT create_xg_texture_layout(UINT width, UINT height, UINT mips, UINT arrays, DXGI_FORMAT format,
                                 UINT tile_mode, TitleTextureLayout& out, UINT bind_flags, UINT samples) {
    if (bind_flags & ~(D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL)) return DXGI_ERROR_UNSUPPORTED;
    return create_xg_texture_layout_impl(width, height, 0, mips, arrays, format, tile_mode, bind_flags, samples, out);
}
HRESULT create_xg_texture3d_layout(UINT width, UINT height, UINT depth, UINT mips, DXGI_FORMAT format,
                                  UINT bind_flags, TitleTextureLayout& out, UINT tile_mode) {
    if (!depth) return E_INVALIDARG;
    if (bind_flags & ~(D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS)) return DXGI_ERROR_UNSUPPORTED;
    return create_xg_texture_layout_impl(width, height, depth, mips, 1, format, tile_mode, bind_flags, 1, out);
}

void* resolve_d3d11x(const ImportRequest& request) {
    if (!same_library(request.library, "d3d11_x.dll")) return nullptr;
    if (request.name == "D3D11CreateDevice") return reinterpret_cast<void*>(&D3D11CreateDevice);
    if (request.name == "D3D11XCreateDeviceX" && legacy_abi_bound) return reinterpret_cast<void*>(&D3D11XCreateDeviceX);
    if (request.name == "D3DMapEsramMemory" && legacy_abi_bound) return reinterpret_cast<void*>(&map_title_esram);
    return nullptr;
}

} // namespace xboxone::era
