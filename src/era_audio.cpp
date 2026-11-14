// SPDX-License-Identifier: BSD-3-Clause
#include <initguid.h>
#include "era_audio.h"
#include "era_memory.h"
#include "file_path.h"
#include "sha256.h"
#include "xboxone_native/era_loader.h"
#include <audioclient.h>
#include <xaudio2.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <vector>
#include <list>
#include <filesystem>
#include <wrl/wrappers/corewrappers.h>

namespace xboxone::era {
namespace {
using Microsoft::WRL::ComPtr;
constexpr ULONGLONG acp_attributes = 0xec810000ull;
constexpr HRESULT acp_already_created = HRESULT(0x8aca0001u), acp_not_created = HRESULT(0x8aca0002u);
using AudioAllocate = void* (WINAPI*)(SIZE_T, ULONGLONG);
using AudioFree = void (WINAPI*)(void*, ULONGLONG);
std::recursive_mutex acp_mutex;
bool acp_bound = false;
bool audio_diagnostics = false;
UINT acp_data_bytes = 0x4000000, acp_control_bytes = 0x114000; // Original singleton 180001a2c.
struct AudioBlock { UINT offset, span, used = 0, padding = 0; };
struct AcpHeap {
    void* memory = nullptr;
    UINT bytes = 0;
    bool pinned = false;
    std::list<AudioBlock> blocks;
    ~AcpHeap() {
        if (memory) {
            const auto release = reinterpret_cast<AudioFree>(resolve_kernelx({"kernelx.dll", "XMemFree"}));
            release(memory, acp_attributes);
            if (pinned) unpin_title_audio_memory(memory);
        }
    }
};
std::array<std::unique_ptr<AcpHeap>, 2> acp_heaps;

template<class T> bool audio_output(T* output, const T& value) {
    SIZE_T written = 0;
    return output && WriteProcessMemory(GetCurrentProcess(), output, &value, sizeof value, &written) && written == sizeof value;
}
HRESULT make_acp_heap(UINT bytes, std::unique_ptr<AcpHeap>& result) {
    auto heap = std::make_unique<AcpHeap>();
    heap->bytes = bytes;
    heap->blocks.push_back({0, bytes});
    const auto allocate = reinterpret_cast<AudioAllocate>(resolve_kernelx({"kernelx.dll", "XMemAlloc"}));
    heap->memory = allocate(bytes, acp_attributes);
    if (!heap->memory) return E_OUTOFMEMORY;
    const auto address = reinterpret_cast<ULONG_PTR>(heap->memory);
    if (!address || address > UINT32_MAX || bytes > UINT32_MAX - address || (address & 4095)) return E_INVALIDARG;
    const HRESULT hr = pin_title_audio_memory(heap->memory, bytes);
    if (FAILED(hr)) return hr;
    heap->pinned = true;
    result = std::move(heap);
    return S_OK;
}
HRESULT initialize_acp_heaps() { // Caller holds acp_mutex, including callbacks through XMem.
    if (!acp_bound) return E_NOTIMPL;
    if (acp_heaps[0]) return S_OK;
    const auto finish = [&](HRESULT hr) {
        if (audio_diagnostics) std::fprintf(stderr, "xone-era: ACP heap creation HRESULT 0x%08lX requested=0x%X/0x%X backing=%p/%p thread=%lu\n",
            hr, acp_data_bytes, acp_control_bytes, acp_heaps[0] ? acp_heaps[0]->memory : nullptr,
            acp_heaps[1] ? acp_heaps[1]->memory : nullptr, GetCurrentThreadId());
        return hr;
    };
    try {
        std::array<std::unique_ptr<AcpHeap>, 2> heaps;
        HRESULT hr = make_acp_heap((acp_data_bytes + 4095u) & ~4095u, heaps[0]);
        // The original adds 8 KiB to the control heap before page rounding.
        if (SUCCEEDED(hr)) hr = make_acp_heap((acp_control_bytes + 8192u + 4095u) & ~4095u, heaps[1]);
        if (FAILED(hr)) return finish(hr);
        acp_heaps = std::move(heaps);
        return finish(S_OK);
    } catch (const std::bad_alloc&) { return finish(E_OUTOFMEMORY); }
}
HRESULT WINAPI apu_create_heap(UINT data_bytes, UINT control_bytes) {
    std::lock_guard lock(acp_mutex);
    if (!data_bytes) return E_INVALIDARG;
    if (acp_heaps[0]) return acp_already_created;
    if (data_bytes > 0x1f400000u || control_bytes > 0x1f3fe000u) return E_INVALIDARG;
    acp_data_bytes = data_bytes;
    if (control_bytes) acp_control_bytes = control_bytes;
    return initialize_acp_heaps();
}
void free_audio_block(AcpHeap& heap, std::list<AudioBlock>::iterator block) {
    block->used = block->padding = 0;
    auto next = std::next(block);
    if (next != heap.blocks.end() && !next->used) { block->span += next->span; heap.blocks.erase(next); }
    if (block != heap.blocks.begin()) {
        auto previous = std::prev(block);
        if (!previous->used) { previous->span += block->span; heap.blocks.erase(block); }
    }
}
HRESULT WINAPI apu_alloc(void** output, UINT* apu_output, UINT bytes, UINT alignment, UINT type) {
    if (!audio_output(output, static_cast<void*>(nullptr)) || (apu_output && !audio_output(apu_output, UINT{0}))) return E_INVALIDARG;
    if (!bytes || !alignment || (alignment & 3) || (alignment & (alignment - 1)) || type < 1 || type > 2) return E_INVALIDARG;
    alignment = (std::max)(alignment, 256u);
    if (bytes > UINT_MAX - (alignment - 1)) return E_OUTOFMEMORY;
    bytes = (bytes + alignment - 1) & ~(alignment - 1);
    std::lock_guard lock(acp_mutex);
    const HRESULT initialized = initialize_acp_heaps();
    if (FAILED(initialized)) return initialized;
    auto& heap = *acp_heaps[type - 1];
    auto best = heap.blocks.end();
    UINT padding = 0;
    const auto base = reinterpret_cast<ULONG_PTR>(heap.memory);
    for (auto block = heap.blocks.begin(); block != heap.blocks.end(); ++block) {
        const UINT pad = (UINT{0} - UINT(base + block->offset)) & (alignment - 1);
        if (!block->used && pad <= block->span && bytes <= block->span - pad &&
            (best == heap.blocks.end() || block->span < best->span)) { best = block; padding = pad; }
    }
    if (best == heap.blocks.end()) return E_OUTOFMEMORY;
    const UINT span = padding + bytes;
    try {
        if (span < best->span) heap.blocks.insert(std::next(best), {best->offset + span, best->span - span});
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
    best->span = span; best->used = bytes; best->padding = padding;
    void* address = reinterpret_cast<void*>(base + best->offset + padding);
    if (!audio_output(output, address) || (apu_output && !audio_output(apu_output, UINT(reinterpret_cast<ULONG_PTR>(address))))) {
        free_audio_block(heap, best);
        (void)audio_output(output, static_cast<void*>(nullptr));
        return E_INVALIDARG;
    }
    return S_OK;
}
void WINAPI apu_free(void* address) {
    std::lock_guard lock(acp_mutex);
    if (FAILED(initialize_acp_heaps())) return;
    for (auto& heap : acp_heaps) if (heap) {
        for (auto block = heap->blocks.begin(); block != heap->blocks.end(); ++block)
            if (block->used && reinterpret_cast<ULONG_PTR>(heap->memory) + block->offset + block->padding == reinterpret_cast<ULONG_PTR>(address)) {
                free_audio_block(*heap, block); return;
            }
    }
}
UINT WINAPI apu_map_virtual_address(const void* address) {
    std::lock_guard lock(acp_mutex);
    if (FAILED(initialize_acp_heaps())) return 0;
    const auto value = reinterpret_cast<ULONG_PTR>(address);
    for (auto& heap : acp_heaps) if (heap) for (const auto& block : heap->blocks) if (block.used) {
        const auto start = reinterpret_cast<ULONG_PTR>(heap->memory) + block.offset + block.padding;
        if (value >= start && value - start < block.used) return UINT(value);
    }
    return 0;
}
void* WINAPI apu_map_apu_address(UINT address) {
    auto* pointer = reinterpret_cast<void*>(ULONG_PTR(address));
    return apu_map_virtual_address(pointer) ? pointer : nullptr;
}
bool WINAPI apu_is_virtual_address_valid(const void* address, UINT alignment) {
    const UINT mapped = apu_map_virtual_address(address);
    if (!alignment) alignment = 1;
    return mapped && !(mapped & (alignment - 1));
}
HRESULT WINAPI apu_heap_get_state(std::array<UINT, 5>* output, UINT type) {
    std::lock_guard lock(acp_mutex);
    if (!output || type < 1 || type > 2) return E_INVALIDARG;
    if (!acp_heaps[0]) return acp_not_created;
    std::array<UINT, 5> state{}; // free, used, alignment waste, largest free block, allocation count.
    for (const auto& block : acp_heaps[type - 1]->blocks) {
        if (block.used) { state[1] += block.used; state[2] += block.span - block.used; ++state[4]; }
        else { state[0] += block.span; state[3] = (std::max)(state[3], block.span); }
    }
    return audio_output(output, state) ? S_OK : E_INVALIDARG;
}
HMODULE host_xaudio2() {
    static const HMODULE module = [] {
        wchar_t system[MAX_PATH]{};
        const UINT length=GetSystemDirectoryW(system,MAX_PATH);
        if (!length || length>=MAX_PATH) return HMODULE{};
        const std::wstring path=std::wstring(system,length)+L"\\xaudio2_9.dll";
        return LoadLibraryExW(path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32);
    }();
    return module;
}
class XAudioEngine final : public IXAudio2 {
    std::atomic<ULONG> refs{1};
    ComPtr<IXAudio2> host;
public:
    explicit XAudioEngine(IXAudio2* engine) : host(engine) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != __uuidof(IUnknown) && iid != __uuidof(IXAudio2)) return E_NOINTERFACE;
        *out = static_cast<IXAudio2*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { auto left=--refs; if (!left) delete this; return left; }
    HRESULT STDMETHODCALLTYPE RegisterForCallbacks(IXAudio2EngineCallback* callback) override { return host->RegisterForCallbacks(callback); }
    void STDMETHODCALLTYPE UnregisterForCallbacks(IXAudio2EngineCallback* callback) override { host->UnregisterForCallbacks(callback); }
    HRESULT STDMETHODCALLTYPE CreateSourceVoice(IXAudio2SourceVoice** out,const WAVEFORMATEX* format,UINT32 flags,float ratio,
            IXAudio2VoiceCallback* callback,const XAUDIO2_VOICE_SENDS* sends,const XAUDIO2_EFFECT_CHAIN* effects) override {
        // PCM/float voice and callback slots match the observed Xbox ABI. Other
        // codecs and XAPO contracts need validation before reaching the host.
        if (!out) return E_POINTER;
        *out = nullptr;
        if (!format) return E_INVALIDARG;
        if ((format->wFormatTag != WAVE_FORMAT_PCM && format->wFormatTag != WAVE_FORMAT_IEEE_FLOAT) || format->cbSize || effects)
            return E_NOTIMPL;
        return host->CreateSourceVoice(out,format,flags,ratio,callback,sends,nullptr);
    }
    HRESULT STDMETHODCALLTYPE CreateSubmixVoice(IXAudio2SubmixVoice** out,UINT32 channels,UINT32 rate,UINT32 flags,UINT32 stage,
            const XAUDIO2_VOICE_SENDS* sends,const XAUDIO2_EFFECT_CHAIN* effects) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (effects) return E_NOTIMPL;
        return host->CreateSubmixVoice(out,channels,rate,flags,stage,sends,nullptr);
    }
    HRESULT STDMETHODCALLTYPE CreateMasteringVoice(IXAudio2MasteringVoice** out,UINT32 channels,UINT32 rate,UINT32 flags,
            LPCWSTR id,const XAUDIO2_EFFECT_CHAIN* effects,AUDIO_STREAM_CATEGORY category) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (id || effects) return E_NOTIMPL;
        // Xbox default is an eight-channel 48 kHz mix. The host audio engine
        // performs the real endpoint conversion; retain all logical channels.
        return host->CreateMasteringVoice(out,channels ? channels : 8,rate ? rate : 48000,flags,nullptr,nullptr,category);
    }
    HRESULT STDMETHODCALLTYPE StartEngine() override { return host->StartEngine(); }
    void STDMETHODCALLTYPE StopEngine() override { host->StopEngine(); }
    HRESULT STDMETHODCALLTYPE CommitChanges(UINT32 operation) override { return host->CommitChanges(operation); }
    void STDMETHODCALLTYPE GetPerformanceData(XAUDIO2_PERFORMANCE_DATA*) override { unsupported("GetPerformanceData layout"); }
    void STDMETHODCALLTYPE SetDebugConfiguration(const XAUDIO2_DEBUG_CONFIGURATION*,void*) override { unsupported("SetDebugConfiguration layout"); }
private:
    static void unsupported(const char* operation) {
        std::fprintf(stderr,"xone-era: XAudio2 %s is not implemented\n",operation);
        if (stop_on_missing_service()) ExitProcess(125);
        RaiseFailFastException(nullptr,nullptr,0);
    }
};
HRESULT WINAPI create_xaudio2_object(IXAudio2** out,UINT32 flags,UINT32 processor,void* reserved) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if ((flags & ~0x400u) || !processor || (processor!=0xffffffffu && (processor & ~0xffu)) || reserved) {
        std::fprintf(stderr,"xone-era: unsupported XAudio2 creation flags 0x%X processor 0x%X reserved %p\n",flags,processor,reserved);
        return E_NOTIMPL;
    }
    using Create = HRESULT(WINAPI*)(IXAudio2**,UINT32,XAUDIO2_PROCESSOR);
    const auto create = reinterpret_cast<Create>(GetProcAddress(host_xaudio2(),"XAudio2Create"));
    if (!create) return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    ComPtr<IXAudio2> engine;
    // Original 11295 initializer 180004684: bit 10 bypasses ACP hardware setup.
    // The host mixer is software; logical Xbox processors (including ANY) are not Windows CPU indices.
    HRESULT hr = create(&engine,0,XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr)) return hr;
    try { *out = new XAudioEngine(engine.Get()); return S_OK; }
    catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}
void audio_error(const char* operation, HRESULT hr) {
    static std::atomic<unsigned> count{0};
    const unsigned n = count.fetch_add(1);
    if (n < 8) std::fprintf(stderr, "xone-era: audio %s failed 0x%08lX\n", operation, hr);
    else if (n == 8) std::fputs("xone-era: further audio diagnostics suppressed\n", stderr);
}
HRESULT channel_count(IMMDeviceEnumerator* host, UINT32& count) {
    ComPtr<IMMDevice> device;
    ComPtr<IAudioClient> client;
    HRESULT hr = host->GetDefaultAudioEndpoint(eRender, eConsole, &device);
    if (SUCCEEDED(hr)) hr = device->Activate(__uuidof(IAudioClient), CLSCTX_INPROC_SERVER, nullptr,
                                            reinterpret_cast<void**>(client.GetAddressOf()));
    WAVEFORMATEX* format = nullptr;
    if (SUCCEEDED(hr)) hr = client->GetMixFormat(&format);
    if (SUCCEEDED(hr)) {
        if (!format || !format->nChannels) hr = AUDCLNT_E_UNSUPPORTED_FORMAT;
        else count = format->nChannels;
    }
    CoTaskMemFree(format);
    return hr;
}

struct ChannelState : std::enable_shared_from_this<ChannelState> {
    ComPtr<IMMDeviceEnumerator> host;
    std::mutex mutex;
    std::vector<ComPtr<IUnknown>> callbacks;
    UINT32 last_count = 0;
    std::atomic<bool> active{true}, pending{false};
    std::atomic<unsigned> version{0};

    static void CALLBACK update(PTP_CALLBACK_INSTANCE, void* context) {
        std::unique_ptr<std::shared_ptr<ChannelState>> holder(static_cast<std::shared_ptr<ChannelState>*>(context));
        auto state = *holder;
        const unsigned version = state->version.load();
        const HRESULT initialized = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (SUCCEEDED(initialized)) {
            try {
                UINT32 count = 0;
                const HRESULT queried = state->active ? channel_count(state->host.Get(), count) : S_FALSE;
                if (FAILED(queried)) audio_error("channel query", queried);
                if (state->active && queried == S_OK) {
                    std::vector<ComPtr<IUnknown>> snapshot;
                    {
                        std::lock_guard lock(state->mutex);
                        if (state->active && count != state->last_count) {
                            snapshot = state->callbacks;
                            state->last_count = count;
                        }
                    }
                    for (auto& handler : snapshot) {
                        if (!state->active) break;
                        const HRESULT hr = reinterpret_cast<ChannelCountNotification*>(handler.Get())->OnChannelCountChanged(count, count);
                        if (FAILED(hr)) audio_error("channel callback", hr);
                    }
                }
            } catch (const std::bad_alloc&) {
                audio_error("notification allocation", E_OUTOFMEMORY);
            }
            CoUninitialize();
        } else audio_error("notification COM initialization", initialized);
        state->pending = false;
        if (state->active && state->version.load() != version) state->queue();
    }
    void queue() {
        // MMDevice callbacks cannot wait or call guest code. A coalesced worker
        // performs the COM query and dispatch; the version prevents lost updates.
        if (!active || pending.exchange(true)) return;
        auto* context = new (std::nothrow) std::shared_ptr<ChannelState>(shared_from_this());
        if (!context || !TrySubmitThreadpoolCallback(&update, context, nullptr)) {
            const HRESULT error = context ? HRESULT_FROM_WIN32(GetLastError()) : E_OUTOFMEMORY;
            delete context;
            pending = false;
            audio_error("notification submission", error);
        }
    }
    void changed() { ++version; queue(); }
};

class Notifications final : public IMMNotificationClient {
    std::atomic<ULONG> refs{1};
    std::shared_ptr<ChannelState> state;
public:
    explicit Notifications(std::shared_ptr<ChannelState> s) : state(std::move(s)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != __uuidof(IUnknown) && iid != __uuidof(IMMNotificationClient)) return E_NOINTERFACE;
        *out = static_cast<IMMNotificationClient*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { const auto n = --refs; if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow flow, ERole role, LPCWSTR) override {
        if (flow == eRender && role == eConsole) state->changed();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { state->changed(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { state->changed(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { state->changed(); return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY key) override {
        if (key.fmtid == PKEY_AudioEngine_DeviceFormat.fmtid && key.pid == PKEY_AudioEngine_DeviceFormat.pid) state->changed();
        return S_OK;
    }
};

class Enumerator final : public TitleAudioEnumerator {
    std::atomic<ULONG> refs{1};
    std::shared_ptr<ChannelState> state;
    ComPtr<IMMNotificationClient> notifications;
    std::mutex registration;
    bool subscribed = false;
public:
    explicit Enumerator(IMMDeviceEnumerator* host) : state(std::make_shared<ChannelState>()) {
        state->host = host;
        notifications.Attach(new Notifications(state));
    }
    ~Enumerator() {
        state->active = false;
        if (subscribed) {
            const HRESULT hr = state->host->UnregisterEndpointNotificationCallback(notifications.Get());
            if (FAILED(hr)) {
                audio_error("notification shutdown", hr);
                // Keep the inactive target alive if the host may still call it.
                // Process termination owns recovery from a failed host unregister.
                (void)notifications.Detach();
            }
        }
        std::vector<ComPtr<IUnknown>> handlers;
        { std::lock_guard lock(state->mutex); handlers.swap(state->callbacks); }
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        *out = nullptr;
        if (iid != __uuidof(IUnknown) && iid != __uuidof(IMMDeviceEnumerator) && iid != __uuidof(TitleAudioEnumerator))
            return E_NOINTERFACE;
        *out = static_cast<TitleAudioEnumerator*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override { const auto n = --refs; if (!n) delete this; return n; }
    HRESULT STDMETHODCALLTYPE EnumAudioEndpoints(EDataFlow flow, DWORD mask, IMMDeviceCollection** out) override {
        return state->host->EnumAudioEndpoints(flow, mask, out);
    }
    HRESULT STDMETHODCALLTYPE GetDefaultAudioEndpoint(EDataFlow flow, ERole role, IMMDevice** out) override {
        return state->host->GetDefaultAudioEndpoint(flow, role, out);
    }
    HRESULT STDMETHODCALLTYPE GetDevice(LPCWSTR id, IMMDevice** out) override { return state->host->GetDevice(id, out); }
    HRESULT STDMETHODCALLTYPE RegisterEndpointNotificationCallback(IMMNotificationClient* callback) override {
        return state->host->RegisterEndpointNotificationCallback(callback);
    }
    HRESULT STDMETHODCALLTYPE UnregisterEndpointNotificationCallback(IMMNotificationClient* callback) override {
        return state->host->UnregisterEndpointNotificationCallback(callback);
    }
    HRESULT STDMETHODCALLTYPE GetChannelCounts(UINT32* first, UINT32* second) override {
        if (!first || !second) return E_POINTER;
        *first = *second = 0;
        UINT32 count = 0;
        const HRESULT hr = channel_count(state->host.Get(), count);
        if (SUCCEEDED(hr)) *first = *second = count;
        return hr;
    }
    HRESULT STDMETHODCALLTYPE RegisterChannelCallback(IUnknown* callback) override {
        if (!callback) return E_POINTER;
        std::lock_guard api_lock(registration);
        {
            std::lock_guard lock(state->mutex);
            if (std::any_of(state->callbacks.begin(), state->callbacks.end(), [&](const auto& h){return h.Get() == callback;})) return S_FALSE;
        }
        const bool was_subscribed = subscribed;
        if (!subscribed) {
            UINT32 count = 0;
            const HRESULT queried = channel_count(state->host.Get(), count);
            if (FAILED(queried)) return queried;
            { std::lock_guard lock(state->mutex); state->last_count = count; }
            const HRESULT hr = state->host->RegisterEndpointNotificationCallback(notifications.Get());
            if (FAILED(hr)) return hr;
            subscribed = true;
        }
        try {
            std::lock_guard lock(state->mutex);
            state->callbacks.emplace_back(callback);
        } catch (const std::bad_alloc&) {
            if (!was_subscribed) {
                const HRESULT hr = state->host->UnregisterEndpointNotificationCallback(notifications.Get());
                if (SUCCEEDED(hr)) subscribed = false;
                else audio_error("registration rollback", hr);
            }
            return E_OUTOFMEMORY;
        }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE UnregisterChannelCallback(IUnknown* callback) override {
        if (!callback) return E_POINTER;
        ComPtr<IUnknown> removed;
        std::lock_guard api_lock(registration);
        bool empty = false;
        {
            std::lock_guard lock(state->mutex);
            auto found = std::find_if(state->callbacks.begin(), state->callbacks.end(), [&](const auto& h){return h.Get() == callback;});
            if (found == state->callbacks.end()) return S_FALSE;
            removed = std::move(*found);
            state->callbacks.erase(found);
            empty = state->callbacks.empty();
        }
        if (empty && subscribed) {
            const HRESULT hr = state->host->UnregisterEndpointNotificationCallback(notifications.Get());
            if (FAILED(hr)) {
                std::lock_guard lock(state->mutex);
                state->callbacks.push_back(std::move(removed)); // Erase retained capacity; no new allocation.
                return hr;
            }
            subscribed = false;
        }
        return S_OK;
    }
};
} // namespace

HRESULT bind_acp_module(const std::wstring& original) {
    std::lock_guard lock(acp_mutex);
    if (acp_heaps[0]) return HRESULT_FROM_WIN32(ERROR_BUSY);
    acp_bound = false;
    try {
        const auto path = detail::file_path(std::filesystem::path(original));
        Microsoft::WRL::Wrappers::FileHandle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr));
        if (!file.IsValid()) return HRESULT_FROM_WIN32(GetLastError());
        LARGE_INTEGER bytes{};
        if (!GetFileSizeEx(file.Get(), &bytes)) return HRESULT_FROM_WIN32(GetLastError());
        if (bytes.QuadPart != 21504) return HRESULT_FROM_WIN32(ERROR_CRC);
        std::array<std::uint8_t, 21504> data{}; DWORD read = 0;
        if (!ReadFile(file.Get(), data.data(), DWORD(data.size()), &read, nullptr)) return HRESULT_FROM_WIN32(GetLastError());
        if (read != data.size()) return HRESULT_FROM_WIN32(ERROR_HANDLE_EOF);
        detail::Sha256 hash; hash.update(data);
        if (hash.finish() != "34e6beaf67f2900145ecfb9afb6ed6b48747eb63aafc2edb5974c38c480f99a1") return HRESULT_FROM_WIN32(ERROR_CRC);
        acp_bound = true;
        return S_OK;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}
void reset_acp_heaps() {
    std::lock_guard lock(acp_mutex);
    acp_bound = false;
    for (auto& heap : acp_heaps) heap.reset();
    acp_data_bytes = 0x4000000; acp_control_bytes = 0x114000;
}
void set_audio_diagnostics(bool enabled) {
    std::lock_guard lock(acp_mutex);
    audio_diagnostics = enabled;
}
void* resolve_acp(const ImportRequest& request) {
    std::lock_guard lock(acp_mutex);
    if (!acp_bound || !same_library(request.library, "AcpHal.dll") || request.name.empty()) return nullptr;
    if (request.name == "ApuCreateHeap") return reinterpret_cast<void*>(&apu_create_heap);
    if (request.name == "ApuAlloc") return reinterpret_cast<void*>(&apu_alloc);
    if (request.name == "ApuFree") return reinterpret_cast<void*>(&apu_free);
    if (request.name == "ApuMapVirtualAddress") return reinterpret_cast<void*>(&apu_map_virtual_address);
    if (request.name == "ApuMapApuAddress") return reinterpret_cast<void*>(&apu_map_apu_address);
    if (request.name == "ApuIsVirtualAddressValid") return reinterpret_cast<void*>(&apu_is_virtual_address_valid);
    if (request.name == "ApuHeapGetState") return reinterpret_cast<void*>(&apu_heap_get_state);
    return nullptr; // SHAPE/DSP has no translated executor yet.
}

HRESULT wrap_audio_enumerator(IMMDeviceEnumerator* host, REFIID iid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!host) return E_INVALIDARG;
    try {
        auto* object = new Enumerator(host);
        const HRESULT hr = object->QueryInterface(iid, out);
        object->Release();
        return hr;
    } catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
}
FARPROC resolve_xaudio2_export(HMODULE module,LPCSTR name) {
    if (reinterpret_cast<ULONG_PTR>(name) <= 0xffff || std::strcmp(name,"CreateXAudio2Object") != 0) return nullptr;
    if (!module || module != host_xaudio2()) return nullptr;
    return reinterpret_cast<FARPROC>(&create_xaudio2_object);
}
} // namespace xboxone::era
