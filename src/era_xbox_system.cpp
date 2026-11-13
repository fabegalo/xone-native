// SPDX-License-Identifier: BSD-3-Clause
// Windows.Xbox.System.User statics. IUserStatics IID observed from the title's
// own QueryInterface. Only slots whose calls were observed are implemented;
// the order hypothesis comes from WinDurango's public IDL (IInspectable 0-5,
// then OnlineState 6, OnlineStateChanged 7/8, Users 9, UserAdded 10/11,
// UserRemoved 12/13, ...). Everything else traps with its slot number.
//
// Observed (Sonic Mania 1.0.4.2): slot 12 called first with (handler, token*),
// then slot 10.
// Users enumerates the approved persistent local profile. Online identity and
// authentication remain explicit gaps; the reference's fixed XUID is not reused.
#include "era_winrt_classes.h"
#include "era_winrt_object.h"
#include "file_path.h"

#include <activation.h>
#include <winstring.h>
#include <wrl/wrappers/corewrappers.h>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <memory>
#include <cstdio>
#include <intrin.h>

namespace xboxone::era {
namespace {

constexpr wchar_t class_name[] = L"Windows.Xbox.System.User";
constexpr GUID iid_user_statics{0xB4F23A93, 0x7FC6, 0x4A50, {0xB1, 0x53, 0x95, 0x00, 0x16, 0x38, 0x9E, 0x50}};
constexpr GUID iid_user{0xa2c2223e,0x2f00,0x44e2,{0x89,0x7e,0x01,0x5b,0xce,0xb8,0x77,0x17}};
constexpr GUID iid_view{0x39cf85cc,0xa83f,0x5bcd,{0x9b,0x68,0xd4,0xd5,0xcc,0xcb,0x6a,0x9f}};
constexpr GUID iid_iterable{0x1e1813fd,0xf62e,0x5198,{0xb2,0x07,0x9a,0xa8,0x77,0x14,0x7c,0x08}};
constexpr GUID iid_iterator{0xc18e8fcb,0x754f,0x57ee,{0x9a,0xc8,0x52,0xf4,0xd6,0xeb,0xd9,0x50}};
constexpr GUID iid_display{0xb86dc0dc,0xcaef,0x4a9b,{0x91,0xc0,0x2d,0x1b,0x67,0xfc,0xcd,0xf1}};

struct ProfileRecord {
    char magic[8]={'X','O','N','E','U','S','R','1'};
    UINT local_id=1, reserved=0;
    GUID identity{};
};
static_assert(sizeof(ProfileRecord)==32);
HRESULT read_profile(const std::wstring& path,ProfileRecord& record) {
    Microsoft::WRL::Wrappers::FileHandle file(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    if (!file.IsValid()) return HRESULT_FROM_WIN32(GetLastError());
    BY_HANDLE_FILE_INFORMATION info{}; LARGE_INTEGER size{};
    if (!GetFileInformationByHandle(file.Get(),&info) || !GetFileSizeEx(file.Get(),&size)) return HRESULT_FROM_WIN32(GetLastError());
    if ((info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)) || size.QuadPart!=sizeof record) return HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
    DWORD read=0;
    if (!ReadFile(file.Get(),&record,sizeof record,&read,nullptr)) return HRESULT_FROM_WIN32(GetLastError());
    const ProfileRecord expected;
    return read==sizeof record && !std::memcmp(record.magic,expected.magic,8) && record.local_id==1 && !record.reserved && record.identity!=GUID{} ?
        S_OK : HRESULT_FROM_WIN32(ERROR_INVALID_DATA);
}
HRESULT load_profile(const std::wstring& root,ProfileRecord& record) {
    if (root.empty()) return HRESULT_FROM_WIN32(ERROR_NOT_READY);
    const auto directory=xboxone::detail::file_path(root);
    const auto path=(directory/L".xone-local-profile").wstring();
    HRESULT hr=read_profile(path,record);
    if (hr!=HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) && hr!=HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND)) return hr;
    std::error_code error;
    std::filesystem::create_directories(directory,error);
    if (error) return HRESULT_FROM_WIN32(error.value());
    record=ProfileRecord{};
    hr=CoCreateGuid(&record.identity); if (FAILED(hr)) return hr;
    wchar_t identity[40]{}; StringFromGUID2(record.identity,identity,40);
    const auto temporary=(directory/(std::wstring(L".xone-profile-")+identity+L".tmp")).wstring();
    Microsoft::WRL::Wrappers::FileHandle file(CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
    if (!file.IsValid()) return HRESULT_FROM_WIN32(GetLastError());
    DWORD written=0;
    if (!WriteFile(file.Get(),&record,sizeof record,&written,nullptr) || written!=sizeof record || !FlushFileBuffers(file.Get())) {
        const DWORD failure=GetLastError(); file.Close(); DeleteFileW(temporary.c_str());
        return HRESULT_FROM_WIN32(failure ? failure : ERROR_WRITE_FAULT);
    }
    file.Close();
    if (MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_WRITE_THROUGH)) return S_OK;
    const DWORD failure=GetLastError(); DeleteFileW(temporary.c_str());
    if (failure==ERROR_ALREADY_EXISTS || failure==ERROR_FILE_EXISTS) return read_profile(path,record);
    return HRESULT_FROM_WIN32(failure);
}
template<class T> ULONG STDMETHODCALLTYPE retain(T* self) { return ++self->refs; }
template<class T> ULONG STDMETHODCALLTYPE release(T* self) { const ULONG n=--self->refs; if (!n) delete self; return n; }
template<class T> HRESULT STDMETHODCALLTYPE dynamic_query(T* self,REFIID iid,void** out) {
    if (!out) return E_POINTER;
    *out=nullptr; return self->query(self,iid,out);
}
struct LocalUser;
struct DisplayInfo : WinrtObject { LocalUser* owner=nullptr; };
struct LocalUser : WinrtObject { std::atomic<ULONG> refs{1}; ProfileRecord profile; DisplayInfo display{}; };
HRESULT STDMETHODCALLTYPE display_query(DisplayInfo* self,REFIID iid,void** out) {
    if (!out) return E_POINTER; *out=nullptr;
    if (iid!=__uuidof(IUnknown) && iid!=__uuidof(IInspectable) && iid!=__uuidof(IAgileObject) && iid!=iid_display) return E_NOINTERFACE;
    *out=self;retain(self->owner);return S_OK;
}
ULONG STDMETHODCALLTYPE display_retain(DisplayInfo* self) { return retain(self->owner); }
ULONG STDMETHODCALLTYPE display_release(DisplayInfo* self) { return release(self->owner); }
HRESULT STDMETHODCALLTYPE display_name(DisplayInfo*,HSTRING* out) {
    if (!out) return E_POINTER;
    constexpr wchar_t name[]=L"Local Player";
    return WindowsCreateString(name,UINT(std::size(name)-1),out);
}
HRESULT STDMETHODCALLTYPE user_display(LocalUser* self,void** out) {
    if (!out) return E_POINTER;
    retain(self);*out=&self->display;return S_OK;
}
HRESULT user_query(WinrtObject* self,REFIID iid,void** out) {
    if (iid!=__uuidof(IUnknown) && iid!=__uuidof(IInspectable) && iid!=__uuidof(IAgileObject) && iid!=iid_user) return E_NOINTERFACE;
    *out=self; retain(static_cast<LocalUser*>(self)); return S_OK;
}
HRESULT STDMETHODCALLTYPE user_id(LocalUser* self,UINT* out) { if (!out) return E_POINTER; *out=self->profile.local_id; return S_OK; }
HRESULT STDMETHODCALLTYPE user_is_guest(LocalUser*,boolean* out) {
    if (!out) return E_POINTER;
    *out=false; // The persisted local profile is not a sponsored guest session.
    return S_OK;
}
HRESULT STDMETHODCALLTYPE user_is_signed_in(LocalUser*,boolean* out) {
    if (!out) return E_POINTER;
    *out=true; // Selected local profile; this does not authenticate Xbox services.
    return S_OK;
}
HRESULT STDMETHODCALLTYPE user_location(LocalUser*,UINT* out) { if (!out) return E_POINTER; *out=0; return S_OK; }
HRESULT STDMETHODCALLTYPE user_sponsor(LocalUser*,void** out) { if (!out) return E_POINTER; *out=nullptr; return S_OK; }
HRESULT STDMETHODCALLTYPE user_xbox_id(LocalUser*,HSTRING* out) {
    if (!out) return E_POINTER;
    *out=nullptr;
    // A local profile has no provisioned Xbox services identity.
    std::fprintf(stderr,"xone-era: XboxUserId requires a provisioned identity; local profile only (caller %p)\n",_ReturnAddress());
    return E_NOTIMPL;
}
const auto& user_table() {
    static const auto table=[] { auto t=winrt_vtable();
        t[0]=reinterpret_cast<const void*>(&dynamic_query<LocalUser>);
        t[1]=reinterpret_cast<const void*>(&retain<LocalUser>); t[2]=reinterpret_cast<const void*>(&release<LocalUser>);
        t[6]=reinterpret_cast<const void*>(&user_id);t[9]=reinterpret_cast<const void*>(&user_display);
        t[10]=reinterpret_cast<const void*>(&user_is_guest);
        t[11]=reinterpret_cast<const void*>(&user_is_signed_in);t[12]=reinterpret_cast<const void*>(&user_location);
        t[13]=reinterpret_cast<const void*>(&user_sponsor);
        t[15]=reinterpret_cast<const void*>(&user_xbox_id);return t; }();
    return table;
}
struct ProfileState {
    std::mutex mutex;
    std::wstring root;
    LocalUser* user=nullptr;
    ~ProfileState() { if (user) release(user); }
};
ProfileState& profile_state() { static ProfileState state; return state; }
// Caller owns a reference. A root change never redirects an existing snapshot.
HRESULT acquire_user(LocalUser** out) {
    *out=nullptr;
    const auto root=title_storage_root();
    auto& state=profile_state(); std::lock_guard lock(state.mutex);
    if (!state.user || state.root!=root) {
        ProfileRecord record;
        const HRESULT hr=load_profile(root,record); if (FAILED(hr)) return hr;
        auto user=std::make_unique<LocalUser>();
        static_cast<WinrtObject&>(*user)={user_table().data(),class_name,iid_user,&user_query}; user->profile=record;
        static const auto display=[] { auto t=winrt_vtable();t[0]=reinterpret_cast<const void*>(&display_query);
            t[1]=reinterpret_cast<const void*>(&display_retain);t[2]=reinterpret_cast<const void*>(&display_release);t[6]=reinterpret_cast<const void*>(&display_name);return t; }();
        static_cast<WinrtObject&>(user->display)={display.data(),L"Windows.Xbox.System.UserDisplayInfo",iid_display,nullptr};user->display.owner=user.get();
        state.root=root;
        auto* previous=state.user; state.user=user.release();
        if (previous) release(previous);
    }
    retain(state.user); *out=state.user; return S_OK;
}
struct UserView;
struct UserIterable : WinrtObject { UserView* owner=nullptr; };
struct UserView : WinrtObject {
    std::atomic<ULONG> refs{1}; LocalUser* user=nullptr; UserIterable iterable{};
    ~UserView() { if (user) release(user); }
};
struct UserIterator : WinrtObject {
    std::atomic<ULONG> refs{1}; LocalUser* user=nullptr; bool current=true;
    ~UserIterator() { if (user) release(user); }
};
HRESULT view_query(WinrtObject* self,REFIID iid,void** out) {
    auto* view=static_cast<UserView*>(self);
    if (iid==iid_iterable) *out=&view->iterable;
    else if (iid==__uuidof(IUnknown) || iid==__uuidof(IInspectable) || iid==__uuidof(IAgileObject) || iid==iid_view) *out=view;
    else return E_NOINTERFACE;
    retain(view); return S_OK;
}
HRESULT STDMETHODCALLTYPE view_get(UserView* self,UINT index,void** out) {
    if (!out) return E_POINTER; *out=nullptr;
    if (index) return E_BOUNDS;
    retain(self->user); *out=self->user; return S_OK;
}
HRESULT STDMETHODCALLTYPE view_size(UserView*,UINT* out) { if (!out) return E_POINTER; *out=1; return S_OK; }
HRESULT STDMETHODCALLTYPE view_index(UserView* self,void* user,UINT* index,boolean* found) {
    if (!index || !found) return E_POINTER; *index=0; *found=user==self->user; return S_OK;
}
HRESULT STDMETHODCALLTYPE view_many(UserView* self,UINT start,UINT capacity,void** items,UINT* actual) {
    if (!actual) return E_POINTER; *actual=0;
    if (capacity && !items) return E_POINTER;
    if (start>1) return E_BOUNDS;
    if (!start && capacity) { retain(self->user); items[0]=self->user; *actual=1; }
    return S_OK;
}
HRESULT iterator_query(WinrtObject* self,REFIID iid,void** out) {
    if (iid!=__uuidof(IUnknown) && iid!=__uuidof(IInspectable) && iid!=__uuidof(IAgileObject) && iid!=iid_iterator) return E_NOINTERFACE;
    *out=self; retain(static_cast<UserIterator*>(self)); return S_OK;
}
HRESULT STDMETHODCALLTYPE iterator_current(UserIterator* self,void** out) {
    if (!out) return E_POINTER; *out=nullptr;
    if (!self->current) return E_BOUNDS;
    retain(self->user); *out=self->user; return S_OK;
}
HRESULT STDMETHODCALLTYPE iterator_has(UserIterator* self,boolean* out) { if (!out) return E_POINTER; *out=self->current; return S_OK; }
HRESULT STDMETHODCALLTYPE iterator_next(UserIterator* self,boolean* out) { if (!out) return E_POINTER; self->current=false; *out=false; return S_OK; }
HRESULT STDMETHODCALLTYPE iterator_many(UserIterator* self,UINT capacity,void** items,UINT* actual) {
    if (!actual) return E_POINTER; *actual=0;
    if (capacity && !items) return E_POINTER;
    if (capacity && self->current) { retain(self->user); items[0]=self->user; *actual=1; self->current=false; }
    return S_OK;
}
HRESULT STDMETHODCALLTYPE iterable_query(UserIterable* self,REFIID iid,void** out) {
    if (!out) return E_POINTER; *out=nullptr; return view_query(self->owner,iid,out);
}
ULONG STDMETHODCALLTYPE iterable_retain(UserIterable* self) { return retain(self->owner); }
ULONG STDMETHODCALLTYPE iterable_release(UserIterable* self) { return release(self->owner); }
HRESULT STDMETHODCALLTYPE iterable_first(UserIterable* self,void** out) try {
    if (!out) return E_POINTER; *out=nullptr;
    static const auto table=[] { auto t=winrt_vtable();
        t[0]=reinterpret_cast<const void*>(&dynamic_query<UserIterator>);t[1]=reinterpret_cast<const void*>(&retain<UserIterator>);t[2]=reinterpret_cast<const void*>(&release<UserIterator>);
        t[6]=reinterpret_cast<const void*>(&iterator_current);t[7]=reinterpret_cast<const void*>(&iterator_has);t[8]=reinterpret_cast<const void*>(&iterator_next);t[9]=reinterpret_cast<const void*>(&iterator_many);return t; }();
    auto iterator=std::make_unique<UserIterator>();
    static_cast<WinrtObject&>(*iterator)={table.data(),class_name,iid_iterator,&iterator_query};
    iterator->user=self->owner->user;retain(iterator->user);*out=iterator.release();return S_OK;
} catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
HRESULT STDMETHODCALLTYPE users(WinrtObject*,void** out) try {
    if (!out) return E_POINTER; *out=nullptr;
    static const auto table=[] { auto t=winrt_vtable();
        t[0]=reinterpret_cast<const void*>(&dynamic_query<UserView>);t[1]=reinterpret_cast<const void*>(&retain<UserView>);t[2]=reinterpret_cast<const void*>(&release<UserView>);
        t[6]=reinterpret_cast<const void*>(&view_get);t[7]=reinterpret_cast<const void*>(&view_size);t[8]=reinterpret_cast<const void*>(&view_index);t[9]=reinterpret_cast<const void*>(&view_many);return t; }();
    static const auto iterable=[] { auto t=winrt_vtable();t[0]=reinterpret_cast<const void*>(&iterable_query);
        t[1]=reinterpret_cast<const void*>(&iterable_retain);t[2]=reinterpret_cast<const void*>(&iterable_release);t[6]=reinterpret_cast<const void*>(&iterable_first);return t; }();
    auto view=std::make_unique<UserView>();
    const HRESULT hr=acquire_user(&view->user); if (FAILED(hr)) return hr;
    static_cast<WinrtObject&>(*view)={table.data(),class_name,iid_view,&view_query};
    static_cast<WinrtObject&>(view->iterable)={iterable.data(),class_name,iid_iterable,nullptr};view->iterable.owner=view.get();
    *out=view.release();return S_OK;
} catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }

WinrtEvent user_added;
WinrtEvent user_removed;

HRESULT STDMETHODCALLTYPE add_UserAdded(WinrtObject*, IUnknown* handler, std::int64_t* token) {
    return user_added.add(handler, token);
}
HRESULT STDMETHODCALLTYPE remove_UserAdded(WinrtObject*, std::int64_t token) { return user_added.remove(token); }

HRESULT STDMETHODCALLTYPE add_UserRemoved(WinrtObject*, IUnknown* handler, std::int64_t* token) {
    return user_removed.add(handler, token);
}
HRESULT STDMETHODCALLTYPE remove_UserRemoved(WinrtObject*, std::int64_t token) { return user_removed.remove(token); }

struct UserFactory {
    WinrtObject activation;
    WinrtObject statics;
};
UserFactory& factory();

HRESULT query(WinrtObject*, REFIID iid, void** out) {
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IInspectable) || iid == __uuidof(IActivationFactory))
        *out = &factory().activation;
    else if (iid == iid_user_statics)
        *out = &factory().statics;
    else return E_NOINTERFACE;
    return S_OK;
}

UserFactory& factory() {
    static const auto activation_vt = winrt_vtable(); // Slot 6 ActivateInstance: not observed, traps.
    static const auto statics_vt = [] {
        auto t = winrt_vtable();
        t[9] = reinterpret_cast<const void*>(&users);
        t[10] = reinterpret_cast<const void*>(&add_UserAdded);
        t[11] = reinterpret_cast<const void*>(&remove_UserAdded);
        t[12] = reinterpret_cast<const void*>(&add_UserRemoved);
        t[13] = reinterpret_cast<const void*>(&remove_UserRemoved);
        return t;
    }();
    static UserFactory instance{{activation_vt.data(), class_name, __uuidof(IActivationFactory), &query},
                                {statics_vt.data(), class_name, iid_user_statics, &query}};
    return instance;
}

} // namespace

HRESULT xbox_user_factory(REFIID iid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    return query(nullptr, iid, out);
}

std::size_t xbox_user_removed_handlers() { return user_removed.size(); }
void reset_title_users() {
    auto& state=profile_state(); std::lock_guard lock(state.mutex);
    if (state.user) release(state.user);
    state.user=nullptr; state.root.clear();
}
HRESULT title_local_user_identity(GUID* identity,std::uint32_t* local_id) try {
    if (!identity || !local_id) return E_POINTER;
    *identity=GUID{};*local_id=0;LocalUser* user=nullptr;
    const HRESULT hr=acquire_user(&user);if (FAILED(hr)) return hr;
    *identity=user->profile.identity;*local_id=user->profile.local_id;release(user);return S_OK;
} catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }

} // namespace xboxone::era
