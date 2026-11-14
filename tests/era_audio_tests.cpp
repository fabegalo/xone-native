// SPDX-License-Identifier: BSD-3-Clause
#include "../src/era_audio.h"
#include "xboxone_native/era_loader.h"
#include <audioclient.h>
#include <xaudio2.h>
#include <array>
#include <cstring>
#include <wrl/client.h>
#include <atomic>
#include <iostream>
#include <memory>
#include <mutex>
#include <filesystem>
#include <thread>
using Microsoft::WRL::ComPtr;
using namespace xboxone::era;
namespace {
int failures = 0;
void check(bool ok, const char* text) { if (!ok) { ++failures; std::cerr << "FAIL: " << text << '\n'; } }
using AllocateMemory = void* (WINAPI*)(SIZE_T, ULONGLONG);
using FreeMemory = void (WINAPI*)(void*, ULONGLONG);
AllocateMemory default_allocate;
FreeMemory default_free;
unsigned allocation_calls = 0, free_calls = 0, fail_allocation = 0;
bool foreign_audio_backing = false;
void* WINAPI audio_allocate(SIZE_T bytes, ULONGLONG attributes) {
    if (++allocation_calls == fail_allocation) return nullptr;
    return default_allocate(bytes, foreign_audio_backing ? 6ull << 29 : attributes);
}
void WINAPI audio_free(void* address, ULONGLONG attributes) { ++free_calls; default_free(address, foreign_audio_backing ? 6ull << 29 : attributes); }
int test_acp(const std::wstring& original) {
    using Create = HRESULT(WINAPI*)(UINT, UINT);
    using Allocate = HRESULT(WINAPI*)(void**, UINT*, UINT, UINT, UINT);
    using Free = void(WINAPI*)(void*);
    using Map = UINT(WINAPI*)(const void*);
    using Reverse = void*(WINAPI*)(UINT);
    using Valid = bool(WINAPI*)(const void*, UINT);
    using State = HRESULT(WINAPI*)(std::array<UINT, 5>*, UINT);
    using Hooks = void(WINAPI*)(AllocateMemory, FreeMemory);
    struct MemoryStatus { DWORD length = 64, reserved = 0; UINT64 total, available, legacy[3], used, title_available; };
    static_assert(sizeof(MemoryStatus) == 64);
    const auto memory_status = reinterpret_cast<BOOL(WINAPI*)(MemoryStatus*)>(resolve_kernelx({"kernelx.dll", "TitleMemoryStatus"}));
    const auto used = [&] { MemoryStatus status{}; check(memory_status(&status), "real title memory status"); return status.used; };
    check(!resolve_title_import({"AcpHal.dll", "ApuCreateHeap"}), "ACP ABI is not exposed before identity binding");
    check(FAILED(bind_acp_module(L"C:\\Windows\\System32\\kernel32.dll")) && !resolve_acp({"AcpHal.dll", "ApuAlloc"}),
          "foreign ACP identity remains unbound");
    check(bind_acp_module(original) == S_OK, "original ACP identity binds without executing the DLL");
    const auto create = reinterpret_cast<Create>(resolve_title_import({"ACPHAL.DLL", "ApuCreateHeap"}));
    const auto allocate = reinterpret_cast<Allocate>(resolve_acp({"AcpHal.dll", "ApuAlloc"}));
    const auto release = reinterpret_cast<Free>(resolve_acp({"AcpHal.dll", "ApuFree"}));
    const auto map = reinterpret_cast<Map>(resolve_acp({"AcpHal.dll", "ApuMapVirtualAddress"}));
    const auto reverse = reinterpret_cast<Reverse>(resolve_acp({"AcpHal.dll", "ApuMapApuAddress"}));
    const auto valid = reinterpret_cast<Valid>(resolve_acp({"AcpHal.dll", "ApuIsVirtualAddressValid"}));
    const auto state = reinterpret_cast<State>(resolve_acp({"AcpHal.dll", "ApuHeapGetState"}));
    check(create && allocate && release && map && reverse && valid && state, "audited ACP memory exports resolve");
    if (!create || !allocate || !release || !map || !reverse || !valid || !state) return 1;
    check(!resolve_acp({"AcpHal.dll", "", 5}) && !resolve_acp({"AcpHal.dll", "UnknownExport"}) &&
          !resolve_acp({"other.dll", "ApuAlloc"}), "ordinals, foreign modules and unknown exports remain explicit");
    std::array<UINT, 5> snapshot{};
    check(state(&snapshot, 1) == HRESULT(0x8aca0002u), "heap query before creation reports no real heap");
    check(create(0, 1) == E_INVALIDARG && create(UINT_MAX, 1) == E_INVALIDARG, "invalid heap requests fail");
    const auto initial = used();
    default_allocate = reinterpret_cast<AllocateMemory>(resolve_kernelx({"kernelx.dll", "XMemAllocDefault"}));
    default_free = reinterpret_cast<FreeMemory>(resolve_kernelx({"kernelx.dll", "XMemFreeDefault"}));
    const auto hooks = reinterpret_cast<Hooks>(resolve_kernelx({"kernelx.dll", "XMemSetAllocationHooks"}));
    foreign_audio_backing = true; hooks(audio_allocate, audio_free);
    check(create(65536, 4096) == E_INVALIDARG && allocation_calls == 1 && free_calls == 1 && used() == initial,
          "incompatible hook backing is rejected and freed through its owner without replacement");
    foreign_audio_backing = false; allocation_calls = free_calls = 0; fail_allocation = 2;
    check(create(65536, 4096) == E_OUTOFMEMORY && allocation_calls == 2 && free_calls == 1 && used() == initial,
          "second backing allocation failure rolls back the first through the real hooks");
    check(state(&snapshot, 1) == HRESULT(0x8aca0002u), "failed creation publishes no partial heap");
    fail_allocation = 0; allocation_calls = free_calls = 0;
    check(create(65536, 4096) == S_OK && used() == initial + 131072 && allocation_calls == 2,
          "two ACP heaps use registered XMem backing charged once at 64-KiB granularity");
    check(create(65536, 4096) == HRESULT(0x8aca0001u) && used() == initial + 131072,
          "duplicate creation preserves the original heaps");
    check(bind_acp_module(original) == HRESULT_FROM_WIN32(ERROR_BUSY), "live ACP backing prevents ABI rebinding");
    check(state(&snapshot, 1) == S_OK && snapshot == std::array<UINT, 5>{65536, 0, 0, 65536, 0}, "data heap statistics derive from its blocks");
    check(state(&snapshot, 2) == S_OK && snapshot[0] == 12288, "control heap includes the original 8-KiB extension");
    void* a = nullptr; UINT apu = 0;
    check(allocate(&a, &apu, 1, 4, 1) == S_OK && a && ULONG_PTR(a) == apu && !(apu & 255),
          "small allocations round to 256 bytes in a real 32-bit address space");
    if (a) {
        std::memset(a, 0x4d, 256);
        check(reverse(apu + 255) == static_cast<char*>(a) + 255 && map(static_cast<char*>(a) + 255) == apu + 255 &&
              *static_cast<unsigned char*>(reverse(apu + 17)) == 0x4d, "interior CPU/APU maps address the same real bytes");
        check(!map(static_cast<char*>(a) + 256) && !reverse(apu + 256) && valid(a, 256) &&
              !valid(static_cast<char*>(a) + 1, 256) && valid(a, 0), "maps enforce live allocation extents and alignment");
    }
    void* b = nullptr; UINT b_apu = 0;
    check(allocate(&b, &b_apu, 1024, 4096, 1) == S_OK && b && !(b_apu & 4095), "aligned best-fit allocation retains padding");
    check(state(&snapshot, 1) == S_OK && snapshot[1] == 4352 && snapshot[2] == 3840 && snapshot[4] == 2,
          "statistics account for actual rounded payloads and alignment waste");
    release(a); release(a); release(b);
    check(!map(a) && !reverse(apu) && state(&snapshot, 1) == S_OK &&
          snapshot == std::array<UINT, 5>{65536, 0, 0, 65536, 0}, "free coalesces neighbours and invalidates mappings; duplicate free is inert");
    for (auto parameters : {std::array<UINT, 3>{1, 0, 1}, {1, 6, 1}, {1, 256, 3}, {0, 256, 1}}) {
        a = reinterpret_cast<void*>(1); apu = 1;
        check(allocate(&a, &apu, parameters[0], parameters[1], parameters[2]) == E_INVALIDARG && !a && !apu,
              "invalid allocation clears outputs without consuming memory");
    }
    check(allocate(&a, &apu, UINT_MAX, 256, 1) == E_OUTOFMEMORY && !a && !apu,
          "allocation rounding overflow fails transactionally");
    check(allocate(&a, &apu, 65537, 256, 1) == E_OUTOFMEMORY && used() == initial + 131072,
          "heap exhaustion does not grow a second memory pool");
    check(allocate(&a, reinterpret_cast<UINT*>(1), 1, 256, 1) == E_INVALIDARG && !a &&
          state(&snapshot, 1) == S_OK && snapshot[4] == 0, "unwritable output cannot publish an allocation");
    check(allocate(&a, &apu, 64, 2048, 2) == S_OK && a && !(apu & 2047), "control heap uses the same allocator and address registry");
    if (a) std::memset(a, 0xb7, 2048);
    check(state(&snapshot, 2) == S_OK && snapshot[1] == 2048 && snapshot[4] == 1, "control heap tracks its own live payload");
    check(state(nullptr, 1) == E_INVALIDARG && state(&snapshot, 3) == E_INVALIDARG &&
          !map(reinterpret_cast<void*>(1)) && !reverse(UINT_MAX), "foreign pointers and invalid queries fail");
    reset_acp_heaps();
    check(free_calls == 2 && used() == initial && !map(a) && !reverse(apu), "session teardown releases both backings through the hooks and invalidates maps");
    hooks(nullptr, nullptr);
    check(bind_acp_module(original) == S_OK, "a new session rebinds the verified ACP identity");
    check(create(65536, 4096) == S_OK && allocate(&a, &apu, 256, 256, 1) == S_OK, "heap state can be recreated after session teardown");
    if (a) {
        default_free(a, 0xec810000ull);
        check(used() == initial + 131072 && map(a) == apu, "an external free keeps pinned ACP backing alive and charged");
    }
    reset_acp_heaps();
    check(used() == initial, "last audio pin retires a pending backing exactly once");
    check(bind_acp_module(original) == S_OK && create(65536, 0) == S_OK &&
          state(&snapshot, 2) == S_OK && snapshot[0] == 0x116000 && used() == initial + 0x130000,
          "zero control size retains the original singleton default plus its 8-KiB extension");
    reset_acp_heaps();
    check(bind_acp_module(original) == S_OK && allocate(&a, &apu, 256, 256, 1) == S_OK &&
          state(&snapshot, 1) == S_OK && snapshot[0] + snapshot[1] == 0x4000000 && used() == initial + 0x4120000,
          "allocation before explicit creation uses the audited default sizes and real title budget");
    reset_acp_heaps();
    check(used() == initial && !resolve_acp({"AcpHal.dll", "ApuAlloc"}), "session teardown clears backing, configuration and ABI binding");
    struct Contexts { std::array<UINT, 6> counts{}; std::array<void*, 6> cpu{}; std::array<UINT, 6> apu{}; };
    static_assert(sizeof(Contexts) == 96 && offsetof(Contexts, cpu) == 24 && offsetof(Contexts, apu) == 72);
    check(bind_acp_module(original) == S_OK, "SHAPE uses the same verified session");
    const auto shape = reinterpret_cast<HRESULT(WINAPI*)(Contexts*)>(resolve_acp({"AcpHal.dll", "AcpHalAllocateShapeContexts"}));
    const auto shape_release = reinterpret_cast<HRESULT(WINAPI*)()>(resolve_acp({"AcpHal.dll", "AcpHalReleaseShapeContexts"}));
    check(shape && shape_release, "audited SHAPE allocation and release exports resolve");
    if (!shape || !shape_release) return 1;
    check(create(65536, 4096) == S_OK && shape_release() == HRESULT(0x8ac80002u), "release before allocation reports missing contexts");
    check(shape(nullptr) == E_INVALIDARG && shape(reinterpret_cast<Contexts*>(1)) == E_INVALIDARG, "invalid SHAPE structure pointers fail");
    for (unsigned index = 0; index < 6; ++index) {
        Contexts bad{}; bad.counts[index] = index == 4 ? 513 : index == 5 ? 3585 : 4097;
        check(shape(&bad) == E_INVALIDARG, "each SHAPE count has its original upper bound");
    }
    Contexts mismatch{}; mismatch.counts = {2, 0, 0, 0, 0, 1};
    check(shape(&mismatch) == E_INVALIDARG, "SRC count must match XMA plus PCM");
    Contexts exhausted{}; exhausted.counts = {1, 100, 0, 0, 0, 1};
    check(shape(&exhausted) == E_OUTOFMEMORY && state(&snapshot, 2) == S_OK && snapshot[4] == 0 && !exhausted.cpu[0],
          "partial SHAPE allocation rolls back the control heap without publishing outputs");
    reset_acp_heaps();
    check(bind_acp_module(original) == S_OK && create(65536, 65536) == S_OK, "SHAPE session can be recreated");
    Contexts contexts{}; contexts.counts = {3, 2, 4, 5, 1, 2};
    struct Guarded { UINT64 before = 0x12345678; Contexts value; UINT64 after = 0xabcdef; } guarded{0x12345678, contexts, 0xabcdef};
    check(shape(&guarded.value) == S_OK && guarded.before == 0x12345678 && guarded.after == 0xabcdef && guarded.value.counts == contexts.counts,
          "96-byte SHAPE ABI preserves counts and adjacent guards");
    contexts = guarded.value;
    for (unsigned i = 0; i < 5; ++i) {
        check(contexts.cpu[i] && map(contexts.cpu[i]) == contexts.apu[i] && !(contexts.apu[i] & 255), "context arrays use live aligned CPU/APU control allocations");
        if (contexts.cpu[i]) std::memset(contexts.cpu[i], 0x31 + i, 32);
    }
    check(contexts.cpu[5] == static_cast<char*>(contexts.cpu[4]) + 64 && contexts.apu[5] == contexts.apu[4] + 1 &&
          *static_cast<unsigned char*>(reverse(contexts.apu[4])) == 0x35, "shared XMA/PCM CPU layout and original encoded APU field are preserved");
    check(state(&snapshot, 2) == S_OK && snapshot[4] == 5 && snapshot[1] == 1536 && state(&snapshot, 1) == S_OK && snapshot[4] == 0,
          "five actual context allocations charge only the existing control heap");
    check(shape(&guarded.value) == HRESULT(0x8ac80001u) && !map(contexts.cpu[0]) && shape_release() == HRESULT(0x8ac80002u),
          "duplicate allocation follows original error cleanup and invalidates old maps");
    Contexts empty{};
    check(shape(&empty) == S_OK && shape_release() == S_OK, "empty context set registers and releases without allocations");
    Contexts pcm{}; pcm.counts = {1, 0, 0, 0, 0, 1};
    check(shape(&pcm) == S_OK && !pcm.cpu[4] && pcm.cpu[5] && map(pcm.cpu[5]) == pcm.apu[5], "PCM-only contexts own their shared backing");
    std::atomic<unsigned> released{0}, missing{0};
    std::array<std::thread, 4> workers;
    for (auto& worker : workers) worker = std::thread([&] {
        const auto hr = shape_release();
        if (hr == S_OK) ++released; else if (hr == HRESULT(0x8ac80002u)) ++missing;
    });
    for (auto& worker : workers) worker.join();
    check(released == 1 && missing == 3 && !map(pcm.cpu[5]) && state(&snapshot, 2) == S_OK && snapshot[4] == 0,
          "concurrent release retires each allocation exactly once");
    auto* readonly = static_cast<Contexts*>(VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (readonly) {
        *readonly = pcm; DWORD protection = 0;
        check(VirtualProtect(readonly, 4096, PAGE_READONLY, &protection) && shape(readonly) == E_INVALIDARG &&
              state(&snapshot, 2) == S_OK && snapshot[4] == 0, "read-only output rolls back all allocations");
        VirtualFree(readonly, 0, MEM_RELEASE);
    } else check(false, "read-only ABI fixture allocation");
    check(shape(&pcm) == S_OK, "contexts can be reallocated after output failure");
    reset_acp_heaps();
    check(used() == initial && !map(pcm.cpu[5]) && !resolve_acp({"AcpHal.dll", "AcpHalAllocateShapeContexts"}),
          "session teardown releases live contexts before backings and clears binding");
    check(bind_acp_module(original) == S_OK && create(65536, 0) == S_OK, "SHAPE maximum-count fixture uses original control default");
    Contexts maximum{}; maximum.counts = {4096, 4096, 4096, 4096, 512, 3584};
    check(shape(&maximum) == S_OK && state(&snapshot, 2) == S_OK && snapshot[4] == 5 && snapshot[1] == 1130496,
          "all inclusive original count limits fit the real default control heap");
    check(shape_release() == S_OK && state(&snapshot, 2) == S_OK && snapshot[4] == 0 && snapshot[0] == 0x116000,
          "maximum context release coalesces the shared allocation and all four arrays");
    reset_acp_heaps();
    check(used() == initial, "maximum context fixture retains no title memory");
    check(bind_acp_module(original) == S_OK && create(65536, 65536) == S_OK, "ACP client fixture uses existing heaps");
    const auto client_create = reinterpret_cast<HRESULT(WINAPI*)(void**)>(resolve_acp({"AcpHal.dll", "AcpHalCreate"}));
    const auto affinity = reinterpret_cast<DWORD_PTR(WINAPI*)(DWORD_PTR)>(resolve_title_import({"MMDevAPI.dll", "SetWasapiThreadAffinityMask"}));
    if (affinity) {
        DWORD_PTR process = 0, system = 0;
        check(GetProcessAffinityMask(GetCurrentProcess(), &process, &system), "host process affinity is observable");
        check(affinity(process) == 0 && GetLastError() == ERROR_NOT_SUPPORTED,
              "unavailable WASAPI worker affinity returns a real failure, not a successful configuration");
        if (~process) check(affinity(~process) == 0 && GetLastError() == ERROR_INVALID_PARAMETER, "foreign CPU mask retains the original parameter error");
    } else check(false, "WASAPI affinity ABI resolves");
    if (!client_create) { check(false, "ACP client ABI resolves"); return 1; }
    void* client = nullptr;
    check(client_create && client_create(nullptr) == E_INVALIDARG && client_create(reinterpret_cast<void**>(1)) == E_INVALIDARG &&
          client_create(&client) == S_OK && client, "ACP client creation validates outputs and returns a real owned instance");
    if (client) {
        auto** table = *static_cast<void***>(client);
        const auto connect = reinterpret_cast<HRESULT(WINAPI*)(void*, UINT, UINT)>(table[0]);
        const auto disconnect = reinterpret_cast<HRESULT(WINAPI*)(void*)>(table[1]);
        const auto submit = reinterpret_cast<HRESULT(WINAPI*)(void*, UINT, UINT64, UINT, const void*, UINT)>(table[2]);
        const auto message = reinterpret_cast<bool(WINAPI*)(void*, void*)>(table[3]);
        const auto messages = reinterpret_cast<UINT(WINAPI*)(void*)>(table[4]);
        UINT mask = 0xfe;
        check(submit(client, 2, 0, UINT_MAX, &mask, 0) == HRESULT(0x8000000eu) && disconnect(client) == HRESULT(0x8ac8000au),
              "disconnected client preserves original command and disconnect errors");
        check(connect(client, 0, 1) == E_INVALIDARG && connect(client, UINT_MAX, 1) == E_INVALIDARG &&
              connect(client, 1, UINT_MAX) == E_INVALIDARG, "queue counts validate zero and byte-size overflow");
        check(connect(client, 1024, 1024) == E_OUTOFMEMORY && state(&snapshot, 1) == S_OK && snapshot[4] == 0,
              "queue exhaustion publishes no partial connection or allocation");
        check(shape(&empty) == S_OK && connect(client, 4, 4) == S_OK && connect(client, 4, 4) == HRESULT(0x8ac80009u) &&
              shape_release() == HRESULT(0x8ac80010u), "connected client owns its queues and prevents live context release");
        check(state(&snapshot, 1) == S_OK && snapshot[4] == 3 && snapshot[1] == 2304, "client queues reuse the charged data heap");
        check(submit(client, 2, 0, UINT_MAX, &mask, 0) == S_OK && submit(client, 3, 0, UINT_MAX, &mask, 0) == S_OK,
              "register and unregister execute the client subscription changes");
        std::array<UINT, 6> received{}; received.fill(0x12345678);
        check(!message(client, &received) && received[0] == 0x12345678 && messages(client) == 0,
              "subscriptions do not fabricate audio frames, completed commands or overwrite an empty read");
        check(submit(client, 1, 0, UINT_MAX, &mask, 0) == E_NOTIMPL && submit(client, 4, 0, UINT_MAX, nullptr, 0) == E_NOTIMPL &&
              submit(client, 2, 0, 123, &mask, 0) == E_NOTIMPL && submit(client, 2, 0, UINT_MAX, &mask, 1) == E_NOTIMPL,
              "untranslated DSP, future-frame work and notifications cannot complete successfully");
        check(submit(client, 2, 0, UINT_MAX, reinterpret_cast<void*>(1), 0) == E_INVALIDARG &&
              submit(client, 2, 0, UINT_MAX, nullptr, 0) == E_INVALIDARG && submit(client, 28, 0, UINT_MAX, &mask, 0) == E_INVALIDARG,
              "command bounds and parameter pointers preserve errors");
        check(disconnect(client) == S_OK && connect(client, 4, 4) == S_OK && state(&snapshot, 1) == S_OK && snapshot[4] == 3 &&
              disconnect(client) == S_OK && shape_release() == S_OK, "reconnect reuses the same queues and disconnect relinquishes context ownership");
        reinterpret_cast<void(WINAPI*)(void*)>(table[5])(client);
        check(state(&snapshot, 1) == S_OK && snapshot[4] == 0, "client release retires all three queues");
    }
    std::array<void*, 5> clients{};
    for (auto& item : clients) check(client_create(&item) == S_OK && item, "independent ACP client instance");
    for (unsigned i = 0; i < clients.size(); ++i) if (clients[i]) {
        const auto connect = reinterpret_cast<HRESULT(WINAPI*)(void*, UINT, UINT)>((*static_cast<void***>(clients[i]))[0]);
        check(connect(clients[i], 1, 0) == (i < 4 ? S_OK : HRESULT(0x8000ffffu)), "four connected ACP slots enforce the original bound");
    }
    reset_acp_heaps();
    check(used() == initial, "session teardown retires live clients and their queues before freeing heaps");
    std::cout << "ACP memory ABI: " << (failures ? "failed" : "passed") << '\n';
    return failures ? 1 : 0;
}
template<class Interface> class FakeCom : public Interface {
    std::atomic<ULONG> refs{1};
public:
    virtual ~FakeCom() = default;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER; *out = nullptr;
        if (iid != __uuidof(IUnknown) && iid != __uuidof(Interface)) return E_NOINTERFACE;
        *out = static_cast<Interface*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { auto n = --refs; if (!n) delete this; return n; }
};
class Client : public FakeCom<IAudioClient> {
    std::shared_ptr<std::atomic<UINT32>> channels;
public:
    explicit Client(std::shared_ptr<std::atomic<UINT32>> c) : channels(std::move(c)) {}
    HRESULT STDMETHODCALLTYPE GetMixFormat(WAVEFORMATEX** out) override {
        if (!out) return E_POINTER;
        *out = static_cast<WAVEFORMATEX*>(CoTaskMemAlloc(sizeof(WAVEFORMATEX)));
        if (!*out) return E_OUTOFMEMORY;
        **out = {}; (*out)->wFormatTag = WAVE_FORMAT_PCM; (*out)->nChannels = static_cast<WORD>(channels->load());
        (*out)->nSamplesPerSec = 48000; (*out)->wBitsPerSample = 16;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Initialize(AUDCLNT_SHAREMODE, DWORD, REFERENCE_TIME, REFERENCE_TIME, const WAVEFORMATEX*, LPCGUID) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetBufferSize(UINT32*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetStreamLatency(REFERENCE_TIME*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetCurrentPadding(UINT32*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE IsFormatSupported(AUDCLNT_SHAREMODE, const WAVEFORMATEX*, WAVEFORMATEX**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetDevicePeriod(REFERENCE_TIME*, REFERENCE_TIME*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Start() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Stop() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Reset() override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE SetEventHandle(HANDLE) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetService(REFIID, void**) override { return E_NOTIMPL; }
};
class Device : public FakeCom<IMMDevice> {
    std::shared_ptr<std::atomic<UINT32>> channels;
public:
    explicit Device(std::shared_ptr<std::atomic<UINT32>> c) : channels(std::move(c)) {}
    HRESULT STDMETHODCALLTYPE Activate(REFIID iid, DWORD, PROPVARIANT*, void** out) override {
        if (!out) return E_POINTER; *out = nullptr;
        if (iid != __uuidof(IAudioClient)) return E_NOINTERFACE;
        *out = static_cast<IAudioClient*>(new Client(channels)); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OpenPropertyStore(DWORD, IPropertyStore**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetId(LPWSTR*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetState(DWORD*) override { return E_NOTIMPL; }
};
class Backend : public FakeCom<IMMDeviceEnumerator> {
    std::mutex mutex;
    ComPtr<IMMNotificationClient> notification;
public:
    std::shared_ptr<std::atomic<UINT32>> channels = std::make_shared<std::atomic<UINT32>>(2);
    HRESULT failure = S_OK, registration_failure = S_OK, unregistration_failure = S_OK;
    HRESULT STDMETHODCALLTYPE EnumAudioEndpoints(EDataFlow, DWORD, IMMDeviceCollection**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetDefaultAudioEndpoint(EDataFlow flow, ERole role, IMMDevice** out) override {
        if (!out) return E_POINTER; *out = nullptr;
        if (FAILED(failure)) return failure;
        if (flow != eRender || role != eConsole) return E_INVALIDARG;
        *out = new Device(channels); return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetDevice(LPCWSTR, IMMDevice**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE RegisterEndpointNotificationCallback(IMMNotificationClient* callback) override {
        if (FAILED(registration_failure)) return registration_failure;
        std::lock_guard lock(mutex); notification = callback; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE UnregisterEndpointNotificationCallback(IMMNotificationClient* callback) override {
        if (FAILED(unregistration_failure)) return unregistration_failure;
        std::lock_guard lock(mutex);
        if (notification.Get() != callback) return E_INVALIDARG;
        notification.Reset(); return S_OK;
    }
    void change(UINT32 count) {
        *channels = count;
        ComPtr<IMMNotificationClient> callback;
        { std::lock_guard lock(mutex); callback = notification; }
        if (callback) callback->OnDefaultDeviceChanged(eRender, eConsole, L"owned-fixture");
    }
};
class Handler : public ChannelCountNotification {
    std::atomic<ULONG> refs{1};
public:
    std::atomic<UINT32> first{0}, second{0};
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ~Handler() { CloseHandle(event); }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER; *out = nullptr;
        if (iid != __uuidof(IUnknown)) return E_NOINTERFACE;
        *out = static_cast<IUnknown*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { auto n = --refs; if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE OnChannelCountChanged(UINT32 a, UINT32 b) override {
        first = a; second = b; SetEvent(event); return S_OK;
    }
};
}
int main(int argc,char** argv) {
    if (argc == 3 && std::strcmp(argv[1], "--acp") == 0) return test_acp(std::filesystem::path(argv[2]).wstring());
    if (argc==2 && std::strcmp(argv[1],"--xaudio")==0) {
        CoInitializeEx(nullptr,COINIT_MULTITHREADED);
        const HMODULE module=LoadLibraryExW(L"xaudio2_9.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
        using Create=HRESULT(WINAPI*)(IXAudio2**,UINT32,UINT32,void*);
        const auto create=reinterpret_cast<Create>(resolve_xaudio2_export(module,"CreateXAudio2Object"));
        check(create && !resolve_xaudio2_export(GetModuleHandleW(L"kernel32.dll"),"CreateXAudio2Object") &&
            !resolve_xaudio2_export(module,MAKEINTRESOURCEA(1)),"audio factory binding rejects a foreign module and ordinal");
        ComPtr<IXAudio2> engine;
        if (!create || FAILED(create(&engine,0,0x10,nullptr))) return 1;
        IXAudio2* rejected=engine.Get();
        check(create(&rejected,0x80000000,0x10,nullptr)==E_NOTIMPL && !rejected,"unknown Xbox audio flags fail without an engine");
        for(UINT32 processor : {0x10u,0xffffffffu}) {
            ComPtr<IXAudio2> software;
            check(create(&software,0x400,processor,nullptr)==S_OK && software,
                  "Xbox software audio flag accepts explicit and ANY logical processor with a real host engine");
        }
        check(create(&rejected,0x400,0,nullptr)==E_NOTIMPL && !rejected &&
              create(&rejected,0x400,0x10,reinterpret_cast<void*>(1))==E_NOTIMPL && !rejected,
              "software audio still rejects an empty processor mask and unimplemented device parameters");
        IXAudio2MasteringVoice* master=nullptr;
        HRESULT hr=engine->CreateMasteringVoice(&master,0,0,0,nullptr,nullptr,AudioCategory_GameEffects);
        check(SUCCEEDED(hr) && master,"real host endpoint accepts the Xbox mastering mix");
        if (!master) return 1;
        XAUDIO2_VOICE_DETAILS details{};
        master->GetVoiceDetails(&details);
        check(details.InputChannels==8 && details.InputSampleRate==48000,"Xbox default mix keeps eight channels at 48 kHz");
        struct Callback final : IXAudio2VoiceCallback {
            HANDLE done=CreateEventW(nullptr,FALSE,FALSE,nullptr);
            std::atomic<void*> context{nullptr};
            std::atomic<HRESULT> error{S_OK};
            ~Callback(){if(done) CloseHandle(done);}
            void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override {}
            void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
            void STDMETHODCALLTYPE OnStreamEnd() override {}
            void STDMETHODCALLTYPE OnBufferStart(void*) override {}
            void STDMETHODCALLTYPE OnBufferEnd(void* value) override {context=value;SetEvent(done);}
            void STDMETHODCALLTYPE OnLoopEnd(void*) override {}
            void STDMETHODCALLTYPE OnVoiceError(void*,HRESULT value) override {error=value;SetEvent(done);}
        } callback;
        WAVEFORMATEX format{WAVE_FORMAT_IEEE_FLOAT,8,44100,44100*32,32,32,0};
        IXAudio2SourceVoice* source=nullptr;
        hr=engine->CreateSourceVoice(&source,&format,0,2,&callback,nullptr,nullptr);
        check(SUCCEEDED(hr) && source,"Sonic eight-channel float source uses the real host mixer");
        if (source) {
            std::array<float,128*8> silence{};
            XAUDIO2_BUFFER buffer{};
            buffer.AudioBytes=sizeof silence; buffer.pAudioData=reinterpret_cast<const BYTE*>(silence.data()); buffer.pContext=&buffer;
            hr=source->SubmitSourceBuffer(&buffer,nullptr);
            if (SUCCEEDED(hr)) hr=source->Start(0,0);
            check(SUCCEEDED(hr) && WaitForSingleObject(callback.done,2000)==WAIT_OBJECT_0 &&
                callback.error==S_OK && callback.context==&buffer,"native processing completes with the original callback context");
            XAUDIO2_VOICE_STATE state{};
            source->GetState(&state,0);
            check(!state.BuffersQueued && state.SamplesPlayed==128,"real source sample clock consumes every queued frame");
            source->Stop(0,0); source->DestroyVoice();
        }
        master->DestroyVoice(); engine.Reset();
        if(module) FreeLibrary(module);
        CoUninitialize();
        return failures ? 1 : 0;
    }
    const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(initialized)) return 1;
    ComPtr<Backend> host; host.Attach(new Backend);
    ComPtr<TitleAudioEnumerator> title;
    check(wrap_audio_enumerator(host.Get(), __uuidof(TitleAudioEnumerator), reinterpret_cast<void**>(title.GetAddressOf())) == S_OK,
          "Xbox IID is supported by the host adapter");
    if (!title) return 1;
    UINT32 a = 0, b = 0;
    check(title->GetChannelCounts(&a, &b) == S_OK && a == 2 && b == 2, "channel counts come from the backend format");
    check(title->GetChannelCounts(nullptr, &b) == E_POINTER, "channel outputs validated");
    host->failure = E_ACCESSDENIED;
    check(title->GetChannelCounts(&a, &b) == E_ACCESSDENIED && !a && !b, "host errors preserved without fabricated channels");
    host->failure = S_OK;
    ComPtr<Handler> handler; handler.Attach(new Handler);
    if (!handler->event) return 1;
    host->registration_failure = E_ACCESSDENIED;
    check(title->RegisterChannelCallback(handler.Get()) == E_ACCESSDENIED, "native registration failure propagates");
    host->registration_failure = S_OK;
    check(title->RegisterChannelCallback(nullptr) == E_POINTER, "null channel handler rejected");
    check(title->RegisterChannelCallback(handler.Get()) == S_OK, "handler retained and notifications subscribed");
    check(title->RegisterChannelCallback(handler.Get()) == S_FALSE, "duplicate registration does not duplicate events");
    host->change(6);
    check(WaitForSingleObject(handler->event, 2000) == WAIT_OBJECT_0 && handler->first == 6 && handler->second == 6,
          "native topology change dispatches actual channel counts asynchronously");
    check(title->GetChannelCounts(&a, &b) == S_OK && a == 6 && b == 6, "query observes the changed device format");
    host->unregistration_failure = E_ACCESSDENIED;
    check(title->UnregisterChannelCallback(handler.Get()) == E_ACCESSDENIED, "native unregister failure propagates");
    host->change(8);
    check(WaitForSingleObject(handler->event, 2000) == WAIT_OBJECT_0 && handler->first == 8,
          "failed removal preserves the active handler for retry");
    host->unregistration_failure = S_OK;
    check(title->UnregisterChannelCallback(handler.Get()) == S_OK, "channel handler removed");
    check(title->UnregisterChannelCallback(handler.Get()) == S_FALSE, "unknown handler removal reports absence");
    host->change(2);
    check(WaitForSingleObject(handler->event, 100) == WAIT_TIMEOUT, "removed handler receives no later topology event");
    title.Reset(); handler.Reset(); host.Reset();
    CoUninitialize();
    return failures ? 1 : 0;
}
