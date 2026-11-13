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
#include "xboxone_native/appmodel.h"
#include "appmodel_windows.h"

#include <activation.h>
#include <winstring.h>
#include <atomic>
#include <memory>

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

namespace package {
constexpr wchar_t name[]=L"Windows.ApplicationModel.Package";
// SDK IPackageStatics/IPackage/IPackageId; FH2 1411d3760 calls Current at slot 6.
constexpr GUID iid_statics{0x4e534bdf,0x2960,0x4878,{0x97,0xa4,0x96,0x24,0xde,0xb7,0x2f,0x2d}};
constexpr GUID iid_package{0x163c792f,0xbd75,0x413c,{0xbf,0x23,0xb1,0xfe,0x7b,0x95,0xd8,0x25}};
constexpr GUID iid_id{0x1adb665e,0x37c7,0x4790,{0x99,0x80,0xdd,0x7a,0xe7,0x4e,0x8b,0xb2}};
struct Snapshot;
struct Id : WinrtObject { Snapshot* owner=nullptr; };
struct Snapshot : WinrtObject {
    std::atomic<ULONG> refs{1};
    Id id{};
    std::array<std::wstring,6> strings; // Name, resource, publisher, publisher ID, full, family.
    UINT64 version=0;
};
ULONG STDMETHODCALLTYPE retain(Snapshot* self) {return ++self->refs;}
ULONG STDMETHODCALLTYPE release(Snapshot* self) {const ULONG refs=--self->refs;if(!refs)delete self;return refs;}
HRESULT STDMETHODCALLTYPE query(Snapshot* self,REFIID iid,void** out) {
    if(!out)return E_POINTER;*out=nullptr;
    if(iid!=iid_package && iid!=__uuidof(IUnknown) && iid!=__uuidof(IInspectable) && iid!=__uuidof(IAgileObject))return E_NOINTERFACE;
    *out=self;retain(self);return S_OK;
}
HRESULT STDMETHODCALLTYPE id_query(Id* self,REFIID iid,void** out) {
    if(!out)return E_POINTER;*out=nullptr;
    if(iid!=iid_id && iid!=__uuidof(IUnknown) && iid!=__uuidof(IInspectable) && iid!=__uuidof(IAgileObject))return E_NOINTERFACE;
    *out=self;retain(self->owner);return S_OK;
}
ULONG STDMETHODCALLTYPE id_retain(Id* self) {return retain(self->owner);}
ULONG STDMETHODCALLTYPE id_release(Id* self) {return release(self->owner);}
template<UINT Index> HRESULT STDMETHODCALLTYPE string(Id* self,HSTRING* out) {
    if(!out)return E_POINTER;
    const auto& value=self->owner->strings[Index];return WindowsCreateString(value.c_str(),UINT(value.size()),out);
}
HRESULT STDMETHODCALLTYPE version(Id* self,USHORT* out) {
    if(!out)return E_POINTER;
    // PackageVersion is four WORDs in Major/Minor/Build/Revision order.
    for(UINT i=0;i<4;++i)out[i]=USHORT(self->owner->version>>(48-16*i));return S_OK;
}
HRESULT STDMETHODCALLTYPE architecture(Id*,UINT* out) {if(!out)return E_POINTER;*out=1;return S_OK;} // Validated AMD64 -> WinRT X64.
HRESULT STDMETHODCALLTYPE get_id(Snapshot* self,void** out) {if(!out)return E_POINTER;retain(self);*out=&self->id;return S_OK;}
const auto& id_table() {
    static const auto table=[] {auto t=winrt_vtable();
        t[0]=reinterpret_cast<const void*>(&id_query);t[1]=reinterpret_cast<const void*>(&id_retain);t[2]=reinterpret_cast<const void*>(&id_release);
        t[6]=reinterpret_cast<const void*>(&string<0>);t[7]=reinterpret_cast<const void*>(&version);t[8]=reinterpret_cast<const void*>(&architecture);
        t[9]=reinterpret_cast<const void*>(&string<1>);t[10]=reinterpret_cast<const void*>(&string<2>);t[11]=reinterpret_cast<const void*>(&string<3>);
        t[12]=reinterpret_cast<const void*>(&string<4>);t[13]=reinterpret_cast<const void*>(&string<5>);return t;}();return table;
}
const auto& table() {
    static const auto table=[] {auto t=winrt_vtable();t[0]=reinterpret_cast<const void*>(&query);
        t[1]=reinterpret_cast<const void*>(&retain);t[2]=reinterpret_cast<const void*>(&release);t[6]=reinterpret_cast<const void*>(&get_id);return t;}();return table;
}
struct Cache {
    std::mutex mutex;Snapshot* value=nullptr;std::wstring root;std::string revision;
    ~Cache(){if(value)release(value);}
};
HRESULT STDMETHODCALLTYPE current(WinrtObject*,void** out) {
    if(!out)return E_POINTER;*out=nullptr;
    try {
        appmodel::Manifest manifest;std::wstring root;std::string revision;
        const HRESULT hr=title_package_metadata(manifest,root,revision);if(FAILED(hr))return hr;
        static Cache cache;std::lock_guard lock(cache.mutex);
        if(!cache.value || cache.root!=root || cache.revision!=revision) {
            auto next=std::make_unique<Snapshot>();
            next->vtable=table().data();next->runtime_class=name;next->iid=iid_package;next->query=nullptr;
            next->id.vtable=id_table().data();next->id.runtime_class=L"Windows.ApplicationModel.PackageId";next->id.iid=iid_id;next->id.owner=next.get();
            using appmodel::windows::wide;
            const auto at=manifest.family_name.rfind('_');
            if(at==std::string::npos)return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
            next->strings={wide(manifest.name),wide(manifest.resource_id),wide(manifest.publisher),wide(manifest.family_name.substr(at+1)),wide(manifest.full_name),wide(manifest.family_name)};
            next->version=manifest.version;
            cache.root=std::move(root);cache.revision=std::move(revision);
            auto* previous=cache.value;cache.value=next.release();if(previous)release(previous);
        }
        retain(cache.value);*out=cache.value;return S_OK;
    } catch(const std::bad_alloc&){return E_OUTOFMEMORY;}
      catch(const std::exception&){return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);}
}
WinrtObject& activation();
HRESULT factory_query(WinrtObject*,REFIID,void**);
WinrtObject& statics() {
    static const auto table=[] {auto t=winrt_vtable();t[6]=reinterpret_cast<const void*>(&current);return t;}();
    static WinrtObject object{table.data(),name,iid_statics,&factory_query};return object;
}
HRESULT factory_query(WinrtObject*,REFIID iid,void** out) {
    if(iid==iid_statics){*out=&statics();return S_OK;}
    if(iid==__uuidof(IUnknown) || iid==__uuidof(IInspectable) || iid==__uuidof(IActivationFactory)){*out=&activation();return S_OK;}
    return E_NOINTERFACE;
}
HRESULT STDMETHODCALLTYPE activate(WinrtObject*,void** out) {if(!out)return E_POINTER;*out=nullptr;return E_NOTIMPL;}
WinrtObject& activation() {
    static const auto table=[] {auto t=winrt_vtable();t[6]=reinterpret_cast<const void*>(&activate);return t;}();
    static WinrtObject object{table.data(),name,__uuidof(IActivationFactory),&factory_query};return object;
}
} // namespace package

HRESULT package_factory(REFIID iid,void** out) {
    if(!out)return E_POINTER;*out=nullptr;auto& object=package::activation();
    return reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(WinrtObject*,REFIID,void**)>(const_cast<void*>(object.vtable[0]))(&object,iid,out);
}

} // namespace xboxone::era
