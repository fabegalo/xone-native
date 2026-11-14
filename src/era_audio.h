// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <mmdeviceapi.h>
#include <string>

namespace xboxone::era {
struct ImportRequest;
// Read-only identity binding: audited ACP heaps, SHAPE arrays and client ABI.
HRESULT bind_acp_module(const std::wstring& path);
void* resolve_acp(const ImportRequest& request);
void* resolve_audio_device(const ImportRequest& request);
void reset_acp_heaps(); // Before unloading title allocation callbacks.
void set_audio_diagnostics(bool enabled);
// Observed Xbox extension: desktop enumerator slots 0-7, then channel counts
// and channel-count callback registration/removal. Callback slot 3 receives both counts.
struct __declspec(uuid("8B557ADC-555E-47A0-B223-43477E481DAD")) TitleAudioEnumerator : IMMDeviceEnumerator {
    virtual HRESULT STDMETHODCALLTYPE GetChannelCounts(UINT32*, UINT32*) = 0;
    virtual HRESULT STDMETHODCALLTYPE RegisterChannelCallback(IUnknown*) = 0;
    virtual HRESULT STDMETHODCALLTYPE UnregisterChannelCallback(IUnknown*) = 0;
};
struct ChannelCountNotification : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE OnChannelCountChanged(UINT32, UINT32) = 0;
};
// Takes its own reference; no host device or mixer state is changed.
HRESULT wrap_audio_enumerator(IMMDeviceEnumerator* host, REFIID iid, void** out);
// Only substitutes the Xbox factory on the audited System32 XAudio2 module.
FARPROC resolve_xaudio2_export(HMODULE module, LPCSTR name);
}
