// SPDX-License-Identifier: BSD-3-Clause
#include <initguid.h>
#include "era_audio.h"
#include <audioclient.h>
#include <wrl/client.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <memory>
#include <mutex>
#include <new>
#include <vector>

namespace xboxone::era {
namespace {
using Microsoft::WRL::ComPtr;
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
} // namespace xboxone::era
