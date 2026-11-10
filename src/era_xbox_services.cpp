// SPDX-License-Identifier: BSD-3-Clause
// Windows.Xbox.Services.XboxLiveConfiguration: the title's Xbox Live identity.
// Values come from the title package's own AppxManifest.xml (<mx:XboxLive
// TitleId PrimaryServiceConfigId>). This is local configuration only; it does
// not create or claim any Xbox Live connection. Interface (name, IID, getter
// order) follows WinDurango's public IDL; the implementation is ours.
#include "xboxone_native/era_loader.h"
#include "era_winrt_classes.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <activation.h>
#include <inspectable.h>
#include <objidl.h>
#include <shlwapi.h>
#include <winstring.h>
#include <xmllite.h>

#include <mutex>
#include <string>

namespace xboxone::era {
namespace {

struct __declspec(uuid("00193376-6a84-4c67-84a5-3b8a8aaea712")) IXboxLiveConfigurationStatics : IInspectable {
    virtual HRESULT STDMETHODCALLTYPE get_TitleId(HSTRING* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_PrimaryServiceConfigId(HSTRING* value) = 0;
    virtual HRESULT STDMETHODCALLTYPE get_SandboxId(HSTRING* value) = 0;
};

constexpr wchar_t class_name[] = L"Windows.Xbox.Services.XboxLiveConfiguration";
// Retail sandbox: the sandbox of retail consoles and store packages such as the
// title's. Absent from the manifest, it is still local configuration.
constexpr wchar_t retail_sandbox[] = L"RETAIL";

std::mutex package_mutex;
std::wstring package_root;

struct Identity {
    HRESULT status = E_FAIL;
    std::wstring title_id, service_config_id;
    std::wstring entry_point; // First Application element's EntryPoint.
};

Identity read_identity() {
    std::wstring root;
    {
        std::lock_guard lock(package_mutex);
        root = package_root;
    }
    Identity id;
    if (root.empty()) { id.status = HRESULT_FROM_WIN32(ERROR_NOT_READY); return id; }
    const std::wstring manifest = root + L"\\AppxManifest.xml";
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExW(manifest.c_str(), GetFileExInfoStandard, &info)) {
        id.status = HRESULT_FROM_WIN32(GetLastError());
        return id;
    }
    if (info.nFileSizeHigh || info.nFileSizeLow > (1u << 20)) { id.status = HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE); return id; }
    IStream* stream = nullptr;
    IXmlReader* reader = nullptr;
    HRESULT hr = SHCreateStreamOnFileEx(manifest.c_str(), STGM_READ | STGM_SHARE_DENY_WRITE, 0, FALSE, nullptr, &stream);
    if (SUCCEEDED(hr)) hr = CreateXmlReader(__uuidof(IXmlReader), reinterpret_cast<void**>(&reader), nullptr);
    if (SUCCEEDED(hr)) hr = reader->SetProperty(XmlReaderProperty_DtdProcessing, DtdProcessing_Prohibit);
    if (SUCCEEDED(hr)) hr = reader->SetProperty(XmlReaderProperty_XmlResolver, 0);
    if (SUCCEEDED(hr)) hr = reader->SetInput(stream);
    XmlNodeType type{};
    bool found = false;
    while (SUCCEEDED(hr) && (hr = reader->Read(&type)) == S_OK) {
        if (type != XmlNodeType_Element) continue;
        const wchar_t* local = nullptr;
        if (FAILED(reader->GetLocalName(&local, nullptr))) continue;
        const bool xbox_live = !found && !lstrcmpW(local, L"XboxLive");
        const bool application = id.entry_point.empty() && !lstrcmpW(local, L"Application");
        if (!xbox_live && !application) continue;
        found = found || xbox_live;
        for (HRESULT a = reader->MoveToFirstAttribute(); a == S_OK; a = reader->MoveToNextAttribute()) {
            const wchar_t* attribute = nullptr;
            const wchar_t* value = nullptr;
            if (FAILED(reader->GetLocalName(&attribute, nullptr)) || FAILED(reader->GetValue(&value, nullptr))) continue;
            if (xbox_live && !lstrcmpW(attribute, L"TitleId")) id.title_id = value;
            else if (xbox_live && !lstrcmpW(attribute, L"PrimaryServiceConfigId")) id.service_config_id = value;
            else if (application && !lstrcmpW(attribute, L"EntryPoint")) id.entry_point = value;
        }
    }
    if (hr == S_FALSE) hr = S_OK; // End of document.
    if (reader) reader->Release();
    if (stream) stream->Release();
    if (FAILED(hr)) id.status = hr;
    else id.status = found && !id.title_id.empty() && !id.service_config_id.empty() ? S_OK : HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    return id;
}

HRESULT make_string(const std::wstring& text, HSTRING* out) {
    if (!out) return E_POINTER;
    return WindowsCreateString(text.c_str(), static_cast<UINT32>(text.size()), out);
}

// Static-class factory: one process-lifetime object, two interfaces.
class Factory final : public IActivationFactory, public IXboxLiveConfigurationStatics {
public:
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) override {
        if (!out) return E_POINTER;
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IInspectable) || iid == __uuidof(IActivationFactory))
            *out = static_cast<IActivationFactory*>(this);
        else if (iid == __uuidof(IXboxLiveConfigurationStatics))
            *out = static_cast<IXboxLiveConfigurationStatics*>(this);
        else { *out = nullptr; return E_NOINTERFACE; }
        return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }  // Never destroyed.
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE GetIids(ULONG* count, IID** iids) override {
        if (!count || !iids) return E_POINTER;
        *iids = static_cast<IID*>(CoTaskMemAlloc(sizeof(IID)));
        if (!*iids) return E_OUTOFMEMORY;
        **iids = __uuidof(IXboxLiveConfigurationStatics);
        *count = 1;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING* name) override {
        return make_string(class_name, name);
    }
    HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel* level) override {
        if (!level) return E_POINTER;
        *level = BaseTrust;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE ActivateInstance(IInspectable** instance) override {
        if (instance) *instance = nullptr;
        return E_NOTIMPL; // Static class: no instances, as for WinRT static-only classes.
    }
    HRESULT STDMETHODCALLTYPE get_TitleId(HSTRING* value) override {
        const Identity& id = identity();
        return FAILED(id.status) ? id.status : make_string(id.title_id, value);
    }
    HRESULT STDMETHODCALLTYPE get_PrimaryServiceConfigId(HSTRING* value) override {
        const Identity& id = identity();
        return FAILED(id.status) ? id.status : make_string(id.service_config_id, value);
    }
    HRESULT STDMETHODCALLTYPE get_SandboxId(HSTRING* value) override { return make_string(retail_sandbox, value); }

private:
    static const Identity& identity() {
        static const Identity id = read_identity(); // Package identity is fixed for the process.
        return id;
    }
};

} // namespace

void set_title_package_root(const std::wstring& root) {
    std::lock_guard lock(package_mutex);
    package_root = root;
}

HRESULT title_entry_point(std::wstring& entry_point) {
    Identity id = read_identity();
    if (FAILED(id.status) && id.status != HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) return id.status;
    if (id.entry_point.empty()) return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
    entry_point = id.entry_point;
    return S_OK;
}

HRESULT xbox_live_configuration_factory(REFIID iid, void** factory) {
    static Factory instance;
    return instance.QueryInterface(iid, factory);
}

} // namespace xboxone::era
