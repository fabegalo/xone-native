// SPDX-License-Identifier: BSD-3-Clause
#include "../src/era_audio.h"
#include <audioclient.h>
#include <xaudio2.h>
#include <array>
#include <cstring>
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
int main(int argc,char** argv) {
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
