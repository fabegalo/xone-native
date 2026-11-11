// SPDX-License-Identifier: BSD-3-Clause
// Windows.Xbox.Management.Deployment.DownloadableContentPackageManager.
// Only calls observed from titles are implemented; every other slot traps.
// Slot order of IDownloadableContentPackageManager per WinDurango's public IDL,
// whose IID matches the one the title requested: 6 FindPackages,
// 7/8 DownloadableContentPackageInstallCompleted, 9/10 ...WithDetails.
// No package is ever installed while a title runs on this host, so install
// handlers are stored and never raised.
#include "era_winrt_classes.h"
#include "era_winrt_object.h"

#include <activation.h>

namespace xboxone::era {
namespace {

constexpr wchar_t class_name[] = L"Windows.Xbox.Management.Deployment.DownloadableContentPackageManager";
// Default interface, observed from the title's QueryInterface after ActivateInstance.
constexpr GUID iid_manager{0x971FEAAF, 0x98F8, 0x4038, {0xB8, 0xC1, 0x3A, 0x5F, 0x1A, 0x04, 0x3B, 0x97}};

WinrtEvent install_completed;
WinrtEvent install_completed_details;

HRESULT STDMETHODCALLTYPE add_completed(WinrtObject*, IUnknown* h, std::int64_t* t) { return install_completed.add(h, t); }
HRESULT STDMETHODCALLTYPE remove_completed(WinrtObject*, std::int64_t t) { return install_completed.remove(t); }
HRESULT STDMETHODCALLTYPE add_details(WinrtObject*, IUnknown* h, std::int64_t* t) {
    return install_completed_details.add(h, t);
}
HRESULT STDMETHODCALLTYPE remove_details(WinrtObject*, std::int64_t t) { return install_completed_details.remove(t); }

// FindPackages(InstalledPackagesFilter, IVectorView<DownloadableContentPackage>**):
// no downloadable content is mounted on this host, so the view is empty.
HRESULT STDMETHODCALLTYPE find_packages(WinrtObject*, INT32, IInspectable** packages) {
    if (!packages) return E_POINTER;
    static WinrtObject* const empty = winrt_empty_vector_view(
        L"Windows.Foundation.Collections.IVectorView`1<Windows.Xbox.Management.Deployment.DownloadableContentPackage>",
        winrt_pinterface_iid(dlc_package_view_signature));
    *packages = reinterpret_cast<IInspectable*>(empty);
    return S_OK;
}

HRESULT query(WinrtObject*, REFIID iid, void** out);
HRESULT instance_query(WinrtObject*, REFIID iid, void** out);

// The manager holds no state of its own, so one process-wide instance serves
// every activation.
WinrtObject& instance() {
    static const auto vt = [] {
        auto t = winrt_vtable();
        t[6] = reinterpret_cast<const void*>(&find_packages);
        t[7] = reinterpret_cast<const void*>(&add_completed);
        t[8] = reinterpret_cast<const void*>(&remove_completed);
        t[9] = reinterpret_cast<const void*>(&add_details);
        t[10] = reinterpret_cast<const void*>(&remove_details);
        return t;
    }();
    static WinrtObject object{vt.data(), class_name, iid_manager, &instance_query};
    return object;
}

HRESULT instance_query(WinrtObject*, REFIID iid, void** out) {
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IInspectable)) { *out = &instance(); return S_OK; }
    return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE activate_instance(WinrtObject*, IInspectable** out) {
    if (!out) return E_POINTER;
    *out = reinterpret_cast<IInspectable*>(&instance());
    return S_OK;
}

WinrtObject& activation() {
    static const auto vt = [] {
        auto t = winrt_vtable();
        t[6] = reinterpret_cast<const void*>(&activate_instance);
        return t;
    }();
    static WinrtObject object{vt.data(), class_name, __uuidof(IActivationFactory), &query};
    return object;
}

HRESULT query(WinrtObject*, REFIID iid, void** out) {
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IInspectable) || iid == __uuidof(IActivationFactory)) {
        *out = &activation();
        return S_OK;
    }
    return E_NOINTERFACE;
}

} // namespace

HRESULT dlc_package_manager_factory(REFIID iid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    WinrtObject& self = activation();
    return reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(WinrtObject*, REFIID, void**)>(
        const_cast<void*>(self.vtable[0]))(&self, iid, out);
}

namespace transfer {
// ABI facts from the public deployment IDL; local availability uses the active
// package's chunk index. Other deployment operations still trap explicitly.
constexpr wchar_t name[] = L"Windows.Xbox.Management.Deployment.PackageTransferManager";
constexpr GUID iid_manager{0xc2d4b94b, 0x0413, 0x4380, {0xb2, 0xf2, 0xab, 0x98, 0x72, 0xd0, 0x82, 0x85}};
constexpr GUID iid_statics{0xd6cf4cae, 0x9eb1, 0x4cbe, {0xb4, 0x26, 0x7e, 0x75, 0xd4, 0x29, 0xf8, 0x46}};

HRESULT self_query(WinrtObject* self, REFIID iid, void** out) {
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IInspectable)) { *out = self; return S_OK; }
    return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE is_chunk_installed(WinrtObject*, UINT32 chunk, boolean* out) {
    if (!out) return E_POINTER;
    *out = false;
    bool installed = false;
    const HRESULT hr = title_chunk_installed(chunk, installed);
    if (SUCCEEDED(hr)) *out = installed;
    return hr;
}

WinrtObject& current() {
    static const auto vt = [] {
        auto t = winrt_vtable();
        t[7] = reinterpret_cast<const void*>(&is_chunk_installed);
        return t;
    }();
    static WinrtObject object{vt.data(), name, iid_manager, &self_query};
    return object;
}

HRESULT STDMETHODCALLTYPE get_current(WinrtObject*, IInspectable** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    std::wstring entry_point;
    const HRESULT hr = title_entry_point(entry_point);
    if (FAILED(hr)) return hr;
    *out = reinterpret_cast<IInspectable*>(&current());
    return S_OK;
}

WinrtObject& statics() {
    static const auto vt = [] {
        auto t = winrt_vtable();
        t[6] = reinterpret_cast<const void*>(&get_current);
        return t;
    }();
    static WinrtObject object{vt.data(), name, iid_statics, &self_query};
    return object;
}

HRESULT STDMETHODCALLTYPE activate(WinrtObject*, IInspectable** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    return E_NOTIMPL; // No public constructor; Current binds the active package.
}

HRESULT query(WinrtObject* self, REFIID iid, void** out) {
    if (iid == iid_statics) { *out = &statics(); return S_OK; }
    return self_query(self, iid, out);
}

WinrtObject& activation() {
    static const auto vt = [] {
        auto t = winrt_vtable();
        t[6] = reinterpret_cast<const void*>(&activate);
        return t;
    }();
    static WinrtObject object{vt.data(), name, __uuidof(IActivationFactory), &query};
    return object;
}
} // namespace transfer

HRESULT package_transfer_manager_factory(REFIID iid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    auto& self = transfer::activation();
    return reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(WinrtObject*, REFIID, void**)>(
        const_cast<void*>(self.vtable[0]))(&self, iid, out);
}

} // namespace xboxone::era
