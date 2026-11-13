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

#include <activation.h>
#include <atomic>
#include <memory>
#include <weakreference.h>
#include <wrl/client.h>
#include <algorithm>

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

HRESULT STDMETHODCALLTYPE are_chunks_installed(WinrtObject*,IInspectable* chunks,boolean* out) {
    if(!out)return E_POINTER;*out=false;if(!chunks)return E_INVALIDARG;
    Microsoft::WRL::ComPtr<IInspectable> iterable,iterator;
    const auto iid=winrt_pinterface_iid("pinterface({faa585ea-6214-4217-afda-7f46de5869b3};u4)");
    HRESULT hr=chunks->QueryInterface(iid,reinterpret_cast<void**>(iterable.GetAddressOf()));
    if(FAILED(hr))return hr;if(!iterable)return E_UNEXPECTED;
    auto** vt=*reinterpret_cast<void***>(iterable.Get());
    hr=reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*,IInspectable**)>(vt[6])(iterable.Get(),iterator.GetAddressOf());
    if(FAILED(hr))return hr;if(!iterator)return E_UNEXPECTED;
    vt=*reinterpret_cast<void***>(iterator.Get());
    boolean more=false;
    hr=reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*,boolean*)>(vt[7])(iterator.Get(),&more);
    bool all=true;
    std::vector<std::uint32_t> catalog;
    // Even an empty request must not mask an absent/malformed package catalog.
    if(SUCCEEDED(hr))hr=title_installed_chunks(catalog);
    while(SUCCEEDED(hr) && more) {
        UINT32 chunk=0;
        hr=reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*,UINT32*)>(vt[6])(iterator.Get(),&chunk);
        if(SUCCEEDED(hr)) {
            all=all && std::binary_search(catalog.begin(),catalog.end(),chunk);
            hr=reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*,boolean*)>(vt[8])(iterator.Get(),&more);
        }
    }
    if(SUCCEEDED(hr))*out=all;return hr;
}

WinrtObject& current() {
    static const auto vt = [] {
        auto t = winrt_vtable();
        t[7] = reinterpret_cast<const void*>(&is_chunk_installed);
        t[8] = reinterpret_cast<const void*>(&are_chunks_installed);
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
// FH2 1411d3760 calls IPackageStatics.Current at slot 6.
constexpr GUID iid_statics{0x4e534bdf,0x2960,0x4878,{0x97,0xa4,0x96,0x24,0xde,0xb7,0x2f,0x2d}};
constexpr GUID iid_package{0x163c792f,0xbd75,0x413c,{0xbf,0x23,0xb1,0xfe,0x7b,0x95,0xd8,0x25}};
struct Snapshot : WinrtObject {
    std::atomic<ULONG> refs{1};
    appmodel::Manifest metadata;
};
ULONG STDMETHODCALLTYPE retain(Snapshot* self) {return ++self->refs;}
ULONG STDMETHODCALLTYPE release(Snapshot* self) {const ULONG refs=--self->refs;if(!refs)delete self;return refs;}
HRESULT STDMETHODCALLTYPE query(Snapshot* self,REFIID iid,void** out) {
    if(!out)return E_POINTER;*out=nullptr;
    if(iid!=iid_package && iid!=__uuidof(IUnknown) && iid!=__uuidof(IInspectable) && iid!=__uuidof(IAgileObject))return E_NOINTERFACE;
    *out=self;retain(self);return S_OK;
}
const auto& table() {
    static const auto table=[] {auto t=winrt_vtable();t[0]=reinterpret_cast<const void*>(&query);
        t[1]=reinterpret_cast<const void*>(&retain);t[2]=reinterpret_cast<const void*>(&release);return t;}();return table;
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
            next->metadata=std::move(manifest);
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

namespace watcher {
constexpr wchar_t name[]=L"Windows.Xbox.Management.Deployment.PackageTransferWatcher";
// FH2 1411cf610/1411cf6d0 and the public deployment IDL agree on these contracts.
constexpr GUID iid_statics{0xa306e506,0x12f9,0x460a,{0xb0,0xae,0x50,0xba,0x32,0x8c,0x78,0x6c}};
constexpr GUID iid_watcher{0xe74b8e43,0xba51,0x4b09,{0xad,0x3f,0x01,0xba,0xff,0x31,0xf9,0x34}};
struct Watcher;
HRESULT STDMETHODCALLTYPE query(Watcher*,REFIID,void**);
ULONG STDMETHODCALLTYPE retain(Watcher*);
ULONG STDMETHODCALLTYPE release(Watcher*);

// The weak control outlives the observer. A nonzero strong count pins the
// observer during Resolve; zero cannot be resurrected by a racing callback.
struct Weak final : IWeakReference {
    std::atomic<ULONG> refs{1},strong{1};
    Watcher* owner=nullptr;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out) override {
        if(!out)return E_POINTER;*out=nullptr;
        if(iid!=__uuidof(IUnknown) && iid!=__uuidof(IWeakReference) && iid!=__uuidof(IAgileObject))return E_NOINTERFACE;
        *out=static_cast<IWeakReference*>(this);AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {return ++refs;}
    ULONG STDMETHODCALLTYPE Release() override {const auto n=--refs;if(!n)delete this;return n;}
    HRESULT STDMETHODCALLTYPE Resolve(REFIID iid,IInspectable** out) override {
        if(!out)return E_POINTER;*out=nullptr;
        auto count=strong.load();
        while(count && !strong.compare_exchange_weak(count,count+1)) {}
        if(!count)return S_OK;
        const auto hr=query(owner,iid,reinterpret_cast<void**>(out));
        release(owner);return hr;
    }
};
struct Source final : IWeakReferenceSource {
    Watcher* owner=nullptr;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out) override {return query(owner,iid,out);}
    ULONG STDMETHODCALLTYPE AddRef() override {return retain(owner);}
    ULONG STDMETHODCALLTYPE Release() override {return release(owner);}
    HRESULT STDMETHODCALLTYPE GetWeakReference(IWeakReference** out) override;
};
struct Watcher : WinrtObject {
    Weak* weak=nullptr;
    Source source;
    IInspectable* package=nullptr;
    WinrtEvent chunk,progress,completed;
    ~Watcher() {
        chunk.clear();progress.clear();completed.clear();
        if(package)package->Release();
        if(weak)weak->Release();
    }
};
HRESULT STDMETHODCALLTYPE Source::GetWeakReference(IWeakReference** out) {
    if(!out)return E_POINTER;*out=owner->weak;owner->weak->AddRef();return S_OK;
}
ULONG STDMETHODCALLTYPE retain(Watcher* self) {return ++self->weak->strong;}
ULONG STDMETHODCALLTYPE release(Watcher* self) {const auto n=--self->weak->strong;if(!n)delete self;return n;}
HRESULT STDMETHODCALLTYPE query(Watcher* self,REFIID iid,void** out) {
    if(!out)return E_POINTER;*out=nullptr;
    if(iid==__uuidof(IWeakReferenceSource))*out=&self->source;
    else if(iid==iid_watcher || iid==__uuidof(IUnknown) || iid==__uuidof(IInspectable) || iid==__uuidof(IAgileObject))*out=self;
    else return E_NOINTERFACE;
    retain(self);return S_OK;
}
template<WinrtEvent Watcher::* Event>
HRESULT STDMETHODCALLTYPE add(Watcher* self,IUnknown* handler,std::int64_t* token) {return (self->*Event).add(handler,token);}
template<WinrtEvent Watcher::* Event>
HRESULT STDMETHODCALLTYPE remove(Watcher* self,std::int64_t token) {return (self->*Event).remove(token);}
const auto& table() {
    static const auto methods=[] {auto t=winrt_vtable();
        t[0]=reinterpret_cast<const void*>(&query);t[1]=reinterpret_cast<const void*>(&retain);t[2]=reinterpret_cast<const void*>(&release);
        t[7]=reinterpret_cast<const void*>(&add<&Watcher::chunk>);t[8]=reinterpret_cast<const void*>(&remove<&Watcher::chunk>);
        t[9]=reinterpret_cast<const void*>(&add<&Watcher::progress>);t[10]=reinterpret_cast<const void*>(&remove<&Watcher::progress>);
        t[11]=reinterpret_cast<const void*>(&add<&Watcher::completed>);t[12]=reinterpret_cast<const void*>(&remove<&Watcher::completed>);
        return t;}();return methods;
}
HRESULT STDMETHODCALLTYPE create(WinrtObject*,IInspectable* selected,void** out) {
    if(!out)return E_POINTER;*out=nullptr;if(!selected)return E_INVALIDARG;
    IInspectable* current=nullptr;
    const auto hr=package::current(nullptr,reinterpret_cast<void**>(&current));if(FAILED(hr))return hr;
    const bool matches=selected==current;current->Release();if(!matches)return E_INVALIDARG;
    try {
        auto value=std::make_unique<Watcher>();
        value->vtable=table().data();value->runtime_class=name;value->iid=iid_watcher;value->query=nullptr;
        value->weak=new Weak();value->weak->owner=value.get();value->source.owner=value.get();
        selected->AddRef();value->package=selected;
        // A read-only session has no transfer producer. Registration never
        // synthesizes chunk/progress/completion events or installation success.
        *out=value.release();return S_OK;
    } catch(const std::bad_alloc&){return E_OUTOFMEMORY;}
}
HRESULT STDMETHODCALLTYPE create_chunks(WinrtObject*,IInspectable*,IInspectable*,void** out) {
    if(!out)return E_POINTER;*out=nullptr;return E_NOTIMPL;
}
WinrtObject& activation();
HRESULT factory_query(WinrtObject*,REFIID,void**);
WinrtObject& statics() {
    static const auto methods=[] {auto t=winrt_vtable();t[6]=reinterpret_cast<const void*>(&create_chunks);t[7]=reinterpret_cast<const void*>(&create);return t;}();
    static WinrtObject object{methods.data(),name,iid_statics,&factory_query};return object;
}
HRESULT factory_query(WinrtObject*,REFIID iid,void** out) {
    if(iid==iid_statics){*out=&statics();return S_OK;}
    if(iid==__uuidof(IUnknown) || iid==__uuidof(IInspectable) || iid==__uuidof(IActivationFactory)){*out=&activation();return S_OK;}
    return E_NOINTERFACE;
}
WinrtObject& activation() {
    static const auto methods=[] {auto t=winrt_vtable();t[6]=reinterpret_cast<const void*>(&package::activate);return t;}();
    static WinrtObject object{methods.data(),name,__uuidof(IActivationFactory),&factory_query};return object;
}
} // namespace watcher

HRESULT package_transfer_watcher_factory(REFIID iid,void** out) {
    if(!out)return E_POINTER;*out=nullptr;auto& object=watcher::activation();
    return reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(WinrtObject*,REFIID,void**)>(const_cast<void*>(object.vtable[0]))(&object,iid,out);
}

} // namespace xboxone::era
