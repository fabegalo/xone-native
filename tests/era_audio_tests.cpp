// SPDX-License-Identifier: BSD-3-Clause
#include "../src/era_audio.h"
#include <audioclient.h>
#include <wrl/client.h>
#include <atomic>
#include <iostream>
#include <memory>
#include <mutex>
using Microsoft::WRL::ComPtr;
using namespace xboxone::era;
namespace {
int failures = 0;
void check(bool ok, const char* text) { if (!ok) { ++failures; std::cerr << "FAIL: " << text << '\n'; } }
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
int main() {
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
