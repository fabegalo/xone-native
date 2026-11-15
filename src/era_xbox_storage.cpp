// SPDX-License-Identifier: BSD-3-Clause
// Offline ConnectedStorage in the existing profile/title root. No cloud sync.
// Sonic 16299: 14026afb0 uses GetForUserAsync 6; 140279540 observes IAsyncInfo.
// Public interface names/IIDs checked against the reference IDL; independent code.
#include "era_winrt_classes.h"
#include "era_winrt_object.h"
#include "file_path.h"
#include "sha256.h"
#include "era_storage_types.h"
#include <activation.h>
#include <asyncinfo.h>
#include <robuffer.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.Streams.h>
#include <wrl/client.h>
#include <wrl/wrappers/corewrappers.h>
#include <atomic>
#include <algorithm>
#include <map>
#include <memory>
#include <cstring>

namespace xboxone::era {
namespace {
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Wrappers::FileHandle;
using winrt::Windows::Foundation::IAsyncAction;
using winrt::Windows::Foundation::IAsyncOperation;
using winrt::Windows::Foundation::Collections::IMapView;
using winrt::Windows::Foundation::Collections::IIterable;
using winrt::Windows::Storage::Streams::IBuffer;
using BufferMap=IMapView<winrt::hstring,IBuffer>;
using Names=IIterable<winrt::hstring>;
using ContainerInfo=winrt::Windows::Xbox::Storage::ContainerInfo;
using InfoView=winrt::Windows::Foundation::Collections::IVectorView<ContainerInfo>;
using Blobs=std::map<std::wstring,std::vector<std::uint8_t>>;
constexpr wchar_t class_name[]=L"Windows.Xbox.Storage.ConnectedStorageSpace";
constexpr GUID iid_statics{0x07020414,0xb3e9,0x47c8,{0xa3,0x6f,0xf2,0x4e,0x2d,0xc6,0x55,0x73}};
constexpr GUID iid_space{0xb48eea29,0x8323,0x41bc,{0xbb,0xad,0x2c,0x26,0xe2,0x7b,0xda,0x22}};
constexpr GUID iid_container{0x885c65fe,0xe884,0x4cff,{0xba,0x58,0xfb,0x82,0xcc,0x32,0xb5,0x52}};
constexpr HRESULT blob_missing=HRESULT(0x80830008), buffer_small=HRESULT(0x80830007);
// A bounded parser/transaction, not an advertised guest storage quota.
constexpr std::size_t transaction_limit=64*1024*1024;
std::mutex storage_mutex;

template<class T> ULONG STDMETHODCALLTYPE retain(T* self) {return ++self->refs;}
template<class T> ULONG STDMETHODCALLTYPE release(T* self) {auto n=--self->refs;if(!n)delete self;return n;}
template<class T> HRESULT STDMETHODCALLTYPE query(T* self,REFIID iid,void** out) {
    if(!out)return E_POINTER;*out=nullptr;
    if(iid!=__uuidof(IUnknown) && iid!=__uuidof(IInspectable) && iid!=__uuidof(IAgileObject) && iid!=self->iid)return E_NOINTERFACE;
    *out=self;retain(self);return S_OK;
}
template<class T> auto object_table() {
    auto t=winrt_vtable();t[0]=reinterpret_cast<const void*>(&query<T>);
    t[1]=reinterpret_cast<const void*>(&retain<T>);t[2]=reinterpret_cast<const void*>(&release<T>);return t;
}
std::wstring string(HSTRING value) {UINT n=0;auto p=WindowsGetStringRawBuffer(value,&n);return {p,n};}
bool valid_name(const std::wstring& name) {
    if(name.empty() || name.size()>255 || name==L"." || name==L"..")return false;
    return name.find_first_of(L"\\/:*?\"<>|")==std::wstring::npos &&
        std::none_of(name.begin(),name.end(),[](wchar_t c){return c<32 || c==127;});
}
std::wstring digest(const std::wstring& text) {
    detail::Sha256 hash;hash.update({reinterpret_cast<const std::uint8_t*>(text.data()),text.size()*sizeof(wchar_t)});
    auto hex=hash.finish();return {hex.begin(),hex.end()};
}
void allowed(const std::filesystem::path& path) {
    if(!title_writable_path(path.wstring()))winrt::throw_hresult(E_ACCESSDENIED);
}
struct Space:WinrtObject {std::atomic<ULONG> refs{1};ComPtr<IInspectable> user;std::filesystem::path directory;std::wstring scid;};
struct Container:WinrtObject {
    std::atomic<ULONG> refs{1};Space* space=nullptr;std::wstring name;std::filesystem::path file;
    ~Container(){if(space)release(space);}
};

// One immutable snapshot per container is replaced atomically, so updates and
// deletions of several blobs either all persist or leave the previous save.
void append(std::vector<std::uint8_t>& bytes,const void* p,std::size_t n) {
    if(n>transaction_limit-bytes.size())winrt::throw_hresult(E_OUTOFMEMORY);
    if(n){auto start=static_cast<const std::uint8_t*>(p);bytes.insert(bytes.end(),start,start+n);}
}
void number(std::vector<std::uint8_t>& bytes,UINT value){append(bytes,&value,sizeof(value));}
void name_bytes(std::vector<std::uint8_t>& bytes,const std::wstring& name){number(bytes,UINT(name.size()));append(bytes,name.data(),name.size()*sizeof(wchar_t));}
Blobs load(Container* self,std::wstring* loaded_name=nullptr) {
    allowed(self->file);
    FileHandle file(CreateFileW(self->file.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    if(!file.IsValid()) {
        auto error=GetLastError();if(error==ERROR_FILE_NOT_FOUND || error==ERROR_PATH_NOT_FOUND)return {};
        winrt::throw_hresult(HRESULT_FROM_WIN32(error));
    }
    BY_HANDLE_FILE_INFORMATION info{};LARGE_INTEGER size{};
    winrt::check_bool(GetFileInformationByHandle(file.Get(),&info));winrt::check_bool(GetFileSizeEx(file.Get(),&size));
    if((info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)) || size.QuadPart<16 || size.QuadPart>transaction_limit)
        winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size.QuadPart));DWORD read=0;
    winrt::check_bool(ReadFile(file.Get(),bytes.data(),DWORD(bytes.size()),&read,nullptr));
    if(read!=bytes.size() || std::memcmp(bytes.data(),"XONECS01",8))winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
    std::size_t at=8;
    auto take=[&](std::size_t n){if(n>bytes.size()-at)winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));auto p=bytes.data()+at;at+=n;return p;};
    auto u32=[&](){UINT value;std::memcpy(&value,take(4),4);return value;};
    auto name=[&](){auto n=u32();if(n>255)winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));std::wstring value(n,L'\0');auto p=take(n*sizeof(wchar_t));std::memcpy(value.data(),p,n*sizeof(wchar_t));if(!valid_name(value))winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));return value;};
    auto container_name=name();
    if(loaded_name)*loaded_name=std::move(container_name);
    else if(container_name!=self->name)winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
    const auto count=u32();if(count>(bytes.size()-at)/8)winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
    Blobs blobs;
    for(UINT i=0;i<count;++i){auto key=name();auto n=u32();auto p=take(n);if(!blobs.emplace(std::move(key),std::vector<std::uint8_t>(p,p+n)).second)winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));}
    if(at!=bytes.size())winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
    return blobs;
}
void store(Container* self,const Blobs& blobs) {
    std::vector<std::uint8_t> bytes;append(bytes,"XONECS01",8);name_bytes(bytes,self->name);number(bytes,UINT(blobs.size()));
    for(const auto& [name,data]:blobs){name_bytes(bytes,name);number(bytes,UINT(data.size()));append(bytes,data.data(),data.size());}
    allowed(self->space->directory);
    if(!CreateDirectoryW(self->space->directory.c_str(),nullptr) && GetLastError()!=ERROR_ALREADY_EXISTS)winrt::throw_last_error();
    allowed(self->file);
    GUID id{};winrt::check_hresult(CoCreateGuid(&id));wchar_t label[40]{};StringFromGUID2(id,label,40);
    const auto temporary=self->space->directory/(std::wstring(label)+L".tmp");allowed(temporary);
    FileHandle file(CreateFileW(temporary.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
    if(!file.IsValid())winrt::throw_last_error();
    try {
        DWORD written=0;winrt::check_bool(WriteFile(file.Get(),bytes.data(),DWORD(bytes.size()),&written,nullptr));
        if(written!=bytes.size())winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_WRITE_FAULT));
        winrt::check_bool(FlushFileBuffers(file.Get()));file.Close();allowed(self->file);
        winrt::check_bool(MoveFileExW(temporary.c_str(),self->file.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH));
    }catch(...){file.Close();DeleteFileW(temporary.c_str());throw;}
}
std::uint8_t* data(const IBuffer& buffer) {
    std::uint8_t* p=nullptr;winrt::check_hresult(buffer.as<::Windows::Storage::Streams::IBufferByteAccess>()->Buffer(&p));
    if(!p && buffer.Capacity())winrt::throw_hresult(E_POINTER);return p;
}
IAsyncAction update(Container* self,BufferMap updates,Names deletions) {
    // This coroutine has no suspension: snapshots and publication finish before
    // the operation completes, and errors remain on its IAsyncInfo.
    std::lock_guard lock(storage_mutex);auto blobs=load(self);
    if(updates)for(const auto& pair:updates){std::wstring name(pair.Key());if(!valid_name(name))winrt::throw_hresult(E_INVALIDARG);
        auto value=pair.Value();if(!value)winrt::throw_hresult(E_POINTER);const auto n=value.Length();if(n>value.Capacity() || n>transaction_limit)winrt::throw_hresult(E_INVALIDARG);
        auto p=data(value);std::vector<std::uint8_t> bytes;if(n)bytes.assign(p,p+n);blobs[name]=std::move(bytes);}
    if(deletions)for(const auto& key:deletions){std::wstring name(key);if(!valid_name(name))winrt::throw_hresult(E_INVALIDARG);blobs.erase(name);}
    store(self,blobs);co_return;
}
IAsyncAction read(Container* self,BufferMap targets) {
    if(!targets)winrt::throw_hresult(E_POINTER);
    std::lock_guard lock(storage_mutex);const auto blobs=load(self);
    struct Copy{IBuffer target;const std::vector<std::uint8_t>* source;std::uint8_t* destination;};std::vector<Copy> copies;
    for(const auto& pair:targets){std::wstring name(pair.Key());if(!valid_name(name))winrt::throw_hresult(E_INVALIDARG);
        auto found=blobs.find(name);if(found==blobs.end())winrt::throw_hresult(blob_missing);
        auto target=pair.Value();if(!target)winrt::throw_hresult(E_POINTER);if(target.Capacity()<found->second.size())winrt::throw_hresult(buffer_small);
        copies.push_back({target,&found->second,data(target)});}
    for(const auto& copy:copies){if(!copy.source->empty())std::memcpy(copy.destination,copy.source->data(),copy.source->size());copy.target.Length(UINT(copy.source->size()));}
    co_return;
}
IAsyncOperation<BufferMap> get(Container* self,Names names) {
    if(!names)winrt::throw_hresult(E_POINTER);
    std::lock_guard lock(storage_mutex);const auto blobs=load(self);auto map=winrt::single_threaded_map<winrt::hstring,IBuffer>();
    for(const auto& key:names){std::wstring name(key);if(!valid_name(name))winrt::throw_hresult(E_INVALIDARG);
        auto found=blobs.find(name);if(found==blobs.end())winrt::throw_hresult(blob_missing);
        winrt::Windows::Storage::Streams::Buffer value(UINT(found->second.size()));value.Length(UINT(found->second.size()));
        if(!found->second.empty())std::memcpy(data(value),found->second.data(),found->second.size());map.Insert(key,value);}
    co_return map.GetView();
}
template<class T> T projected(void* p){T result{nullptr};if(p)winrt::copy_from_abi(result,p);return result;}
HRESULT STDMETHODCALLTYPE container_name(Container* self,HSTRING* out){if(!out)return E_POINTER;return WindowsCreateString(self->name.c_str(),UINT(self->name.size()),out);}
HRESULT STDMETHODCALLTYPE container_space(Container* self,void** out){if(!out)return E_POINTER;retain(self->space);*out=self->space;return S_OK;}
HRESULT STDMETHODCALLTYPE container_update(Container* self,void* updates,void* deletions,void** out) try {
    if(!out)return E_POINTER;*out=nullptr;*out=winrt::detach_abi(update(self,projected<BufferMap>(updates),projected<Names>(deletions)));return S_OK;
}catch(...){return winrt::to_hresult();}
HRESULT STDMETHODCALLTYPE container_read(Container* self,void* targets,void** out) try {
    if(!out)return E_POINTER;*out=nullptr;*out=winrt::detach_abi(read(self,projected<BufferMap>(targets)));return S_OK;
}catch(...){return winrt::to_hresult();}
HRESULT STDMETHODCALLTYPE container_get(Container* self,void* names,void** out) try {
    if(!out)return E_POINTER;*out=nullptr;*out=winrt::detach_abi(get(self,projected<Names>(names)));return S_OK;
}catch(...){return winrt::to_hresult();}
HRESULT STDMETHODCALLTYPE create_container(Space* self,HSTRING value,void** out) try {
    if(!out)return E_POINTER;*out=nullptr;auto name=string(value);if(!valid_name(name))return E_INVALIDARG;
    static const auto table=[] {auto t=object_table<Container>();t[6]=reinterpret_cast<const void*>(&container_name);t[7]=reinterpret_cast<const void*>(&container_space);
        t[8]=reinterpret_cast<const void*>(&container_update);t[9]=reinterpret_cast<const void*>(&container_read);t[10]=reinterpret_cast<const void*>(&container_get);return t;}();
    auto c=std::make_unique<Container>();static_cast<WinrtObject&>(*c)={table.data(),L"Windows.Xbox.Storage.ConnectedStorageContainer",iid_container,nullptr};
    c->name=std::move(name);c->file=self->directory/(digest(self->scid)+L"-"+digest(c->name)+L".bin");retain(self);c->space=self;*out=c.release();return S_OK;
}catch(...){return winrt::to_hresult();}
HRESULT STDMETHODCALLTYPE space_user(Space* self,void** out){if(!out)return E_POINTER;return self->user.CopyTo(reinterpret_cast<IInspectable**>(out));}
HRESULT STDMETHODCALLTYPE space_scid(Space* self,HSTRING* out){if(!out)return E_POINTER;return WindowsCreateString(self->scid.c_str(),UINT(self->scid.size()),out);}
HRESULT STDMETHODCALLTYPE space_readonly(Space*,boolean* out){if(!out)return E_POINTER;*out=false;return S_OK;}
IAsyncAction remove_container(Space* self,winrt::hstring name) {
    std::lock_guard lock(storage_mutex);if(!valid_name(std::wstring(name)))winrt::throw_hresult(E_INVALIDARG);
    const auto file=self->directory/(digest(self->scid)+L"-"+digest(std::wstring(name))+L".bin");allowed(file);
    if(!DeleteFileW(file.c_str())){const auto error=GetLastError();if(error!=ERROR_FILE_NOT_FOUND && error!=ERROR_PATH_NOT_FOUND)winrt::throw_hresult(HRESULT_FROM_WIN32(error));}co_return;
}
HRESULT STDMETHODCALLTYPE space_delete(Space* self,HSTRING name,void** out) try {
    if(!out)return E_POINTER;*out=nullptr;*out=winrt::detach_abi(remove_container(self,winrt::hstring(string(name))));return S_OK;
}catch(...){return winrt::to_hresult();}

constexpr GUID iid_info_query{0x4f937563,0xbae3,0x4062,{0xa6,0x7f,0xcb,0xea,0x61,0xa2,0xed,0xc3}};
struct InfoQuery:WinrtObject{std::atomic<ULONG> refs{1};Space* space=nullptr;std::wstring prefix;~InfoQuery(){if(space)release(space);}};
std::vector<ContainerInfo> enumerate(InfoQuery* self) {
    std::lock_guard lock(storage_mutex);allowed(self->space->directory/L"enumeration");
    const auto file_prefix=digest(self->space->scid)+L"-";std::vector<ContainerInfo> values;
    for(const auto& entry:std::filesystem::directory_iterator(self->space->directory)) {
        if(entry.path().extension()!=L".bin" || !entry.path().filename().wstring().starts_with(file_prefix))continue;
        Container container;container.file=entry.path();std::wstring name;const auto blobs=load(&container,&name);
        if(entry.path().filename()!=file_prefix+digest(name)+L".bin")winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
        if(!name.starts_with(self->prefix))continue;
        std::uint64_t size=0;for(const auto& [key,data]:blobs)size+=data.size();
        if(size>UINT32_MAX)winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW));
        values.push_back({winrt::hstring(name),UINT(size)});
    }
    std::sort(values.begin(),values.end(),[](const auto& a,const auto& b){return a.Name<b.Name;});return values;
}
IAsyncOperation<InfoView> query_info(InfoQuery* self,UINT start,UINT count) {
    auto values=enumerate(self);if(start>values.size())winrt::throw_hresult(E_BOUNDS);
    const auto end=start+std::min<std::size_t>(count,values.size()-start);
    std::vector<ContainerInfo> selected(values.begin()+start,values.begin()+end);
    co_return winrt::single_threaded_vector<ContainerInfo>(std::move(selected)).GetView();
}
IAsyncOperation<UINT> query_count(InfoQuery* self){co_return UINT(enumerate(self).size());}
HRESULT STDMETHODCALLTYPE info_range(InfoQuery* self,UINT start,UINT count,void** out) try {
    if(!out)return E_POINTER;*out=nullptr;*out=winrt::detach_abi(query_info(self,start,count));return S_OK;
}catch(...){return winrt::to_hresult();}
HRESULT STDMETHODCALLTYPE info_all(InfoQuery* self,void** out){return info_range(self,0,UINT32_MAX,out);}
HRESULT STDMETHODCALLTYPE info_count(InfoQuery* self,void** out) try {
    if(!out)return E_POINTER;*out=nullptr;*out=winrt::detach_abi(query_count(self));return S_OK;
}catch(...){return winrt::to_hresult();}
HRESULT STDMETHODCALLTYPE create_info_query(Space* self,HSTRING prefix,void** out) try {
    if(!out)return E_POINTER;*out=nullptr;auto text=string(prefix);if(!text.empty()&&!valid_name(text))return E_INVALIDARG;
    static const auto table=[] {auto t=object_table<InfoQuery>();t[6]=reinterpret_cast<const void*>(&info_range);t[7]=reinterpret_cast<const void*>(&info_all);t[8]=reinterpret_cast<const void*>(&info_count);return t;}();
    auto value=std::make_unique<InfoQuery>();static_cast<WinrtObject&>(*value)={table.data(),L"Windows.Xbox.Storage.ContainerInfoQueryResult",iid_info_query,nullptr};
    value->prefix=std::move(text);retain(self);value->space=self;*out=value.release();return S_OK;
}catch(...){return winrt::to_hresult();}

// Completed IAsyncOperation<ConnectedStorageSpace> using the same ABI/lifetime
// pattern as AccountPicker. Standard SDK coroutines handle actions and buffers.
struct Operation;
struct Info:WinrtObject{Operation* owner=nullptr;};
struct Operation:WinrtObject{std::atomic<ULONG> refs{1};Info info;ComPtr<IInspectable> result;ComPtr<IUnknown> handler;std::mutex mutex;bool closed=false;};
const GUID& operation_iid(){static const auto iid=winrt_pinterface_iid("pinterface({9fc2b0bb-e446-44e2-aa61-9cab8f636af2};rc(Windows.Xbox.Storage.ConnectedStorageSpace;{b48eea29-8323-41bc-bbad-2c26e27bda22}))");return iid;}
HRESULT STDMETHODCALLTYPE operation_query(Operation* self,REFIID iid,void** out){
    if(!out)return E_POINTER;*out=nullptr;
    if(iid==__uuidof(IAsyncInfo))*out=&self->info;
    else if(iid==__uuidof(IUnknown)||iid==__uuidof(IInspectable)||iid==__uuidof(IAgileObject)||iid==operation_iid())*out=self;
    else return E_NOINTERFACE;retain(self);return S_OK;
}
HRESULT STDMETHODCALLTYPE info_query(Info* self,REFIID iid,void** out){return operation_query(self->owner,iid,out);}
ULONG STDMETHODCALLTYPE info_retain(Info* self){return retain(self->owner);}
ULONG STDMETHODCALLTYPE info_release(Info* self){return release(self->owner);}
HRESULT STDMETHODCALLTYPE info_id(Info*,UINT* out){if(!out)return E_POINTER;*out=1;return S_OK;}
HRESULT STDMETHODCALLTYPE info_status(Info* self,INT32* out){if(!out)return E_POINTER;std::lock_guard lock(self->owner->mutex);if(self->owner->closed)return RO_E_CLOSED;*out=1;return S_OK;}
HRESULT STDMETHODCALLTYPE info_error(Info* self,HRESULT* out){if(!out)return E_POINTER;std::lock_guard lock(self->owner->mutex);if(self->owner->closed)return RO_E_CLOSED;*out=S_OK;return S_OK;}
HRESULT STDMETHODCALLTYPE info_cancel(Info*){return S_OK;}
HRESULT STDMETHODCALLTYPE info_close(Info* self){std::lock_guard lock(self->owner->mutex);self->owner->closed=true;return S_OK;}
HRESULT STDMETHODCALLTYPE completed(Operation* self,IUnknown* handler){
    if(!handler)return E_POINTER;ComPtr<IUnknown> keep(handler);ComPtr<IInspectable> lifetime(reinterpret_cast<IInspectable*>(self));
    {std::lock_guard lock(self->mutex);if(self->closed)return RO_E_CLOSED;if(self->handler)return E_ILLEGAL_DELEGATE_ASSIGNMENT;self->handler=handler;}
    using Invoke=HRESULT(STDMETHODCALLTYPE*)(IUnknown*,void*,INT32);
    return reinterpret_cast<Invoke>((*reinterpret_cast<void***>(handler))[3])(handler,self,1);
}
HRESULT STDMETHODCALLTYPE get_completed(Operation* self,IUnknown** out){if(!out)return E_POINTER;std::lock_guard lock(self->mutex);*out=nullptr;if(self->closed)return RO_E_CLOSED;return self->handler.CopyTo(out);}
HRESULT STDMETHODCALLTYPE results(Operation* self,void** out){if(!out)return E_POINTER;*out=nullptr;std::lock_guard lock(self->mutex);if(self->closed)return RO_E_CLOSED;return self->result.CopyTo(reinterpret_cast<IInspectable**>(out));}
HRESULT STDMETHODCALLTYPE for_user_scid(WinrtObject*,IInspectable* user,HSTRING scid,void** out) try {
    if(!out)return E_POINTER;*out=nullptr;if(!user)return E_INVALIDARG;
    ComPtr<IUnknown> selected,requested;ComPtr<IInspectable> current;
    winrt::check_hresult(title_local_user(reinterpret_cast<void**>(current.GetAddressOf())));
    winrt::check_hresult(current.As(&selected));winrt::check_hresult(user->QueryInterface(IID_PPV_ARGS(&requested)));
    if(selected.Get()!=requested.Get())return E_INVALIDARG;
    auto id=string(scid);if(id.empty() || id.size()>255 || id.find(L'\0')!=std::wstring::npos)return E_INVALIDARG;
    static const auto table=[] {auto t=object_table<Space>();t[6]=reinterpret_cast<const void*>(&space_user);t[7]=reinterpret_cast<const void*>(&space_scid);
        t[8]=reinterpret_cast<const void*>(&space_readonly);t[9]=reinterpret_cast<const void*>(&create_container);t[10]=reinterpret_cast<const void*>(&space_delete);t[11]=reinterpret_cast<const void*>(&create_info_query);return t;}();
    auto space=std::make_unique<Space>();static_cast<WinrtObject&>(*space)={table.data(),class_name,iid_space,nullptr};space->user=user;
    space->scid=std::move(id);space->directory=detail::file_path(title_storage_root())/L"connected";allowed(space->directory);
    if(!CreateDirectoryW(space->directory.c_str(),nullptr) && GetLastError()!=ERROR_ALREADY_EXISTS)winrt::throw_last_error();
    static const auto op_table=[] {auto t=object_table<Operation>();t[0]=reinterpret_cast<const void*>(&operation_query);t[6]=reinterpret_cast<const void*>(&completed);t[7]=reinterpret_cast<const void*>(&get_completed);t[8]=reinterpret_cast<const void*>(&results);return t;}();
    static const auto info_table=[] {auto t=winrt_vtable();t[0]=reinterpret_cast<const void*>(&info_query);t[1]=reinterpret_cast<const void*>(&info_retain);t[2]=reinterpret_cast<const void*>(&info_release);
        t[6]=reinterpret_cast<const void*>(&info_id);t[7]=reinterpret_cast<const void*>(&info_status);t[8]=reinterpret_cast<const void*>(&info_error);t[9]=reinterpret_cast<const void*>(&info_cancel);t[10]=reinterpret_cast<const void*>(&info_close);return t;}();
    auto op=std::make_unique<Operation>();static_cast<WinrtObject&>(*op)={op_table.data(),class_name,operation_iid(),nullptr};
    static_cast<WinrtObject&>(op->info)={info_table.data(),class_name,__uuidof(IAsyncInfo),nullptr};op->info.owner=op.get();
    op->result.Attach(reinterpret_cast<IInspectable*>(space.release()));*out=op.release();return S_OK;
}catch(...){return winrt::to_hresult();}
HRESULT STDMETHODCALLTYPE for_user(WinrtObject* factory,IInspectable* user,void** out) try {
    if(!out)return E_POINTER;*out=nullptr;
    constexpr GUID iid_config{0x00193376,0x6a84,0x4c67,{0x84,0xa5,0x3b,0x8a,0x8a,0xae,0xa7,0x12}};
    ComPtr<IInspectable> config;winrt::check_hresult(xbox_live_configuration_factory(iid_config,reinterpret_cast<void**>(config.GetAddressOf())));
    winrt::hstring scid;using Getter=HRESULT(STDMETHODCALLTYPE*)(void*,HSTRING*);
    winrt::check_hresult(reinterpret_cast<Getter>((*reinterpret_cast<void***>(config.Get()))[7])(config.Get(),reinterpret_cast<HSTRING*>(winrt::put_abi(scid))));
    return for_user_scid(factory,user,static_cast<HSTRING>(winrt::get_abi(scid)),out);
}catch(...){return winrt::to_hresult();}
struct Factory{WinrtObject activation;WinrtObject statics;};
Factory& factory();
HRESULT factory_query(WinrtObject*,REFIID iid,void** out){if(!out)return E_POINTER;*out=nullptr;
    if(iid==__uuidof(IUnknown)||iid==__uuidof(IInspectable)||iid==__uuidof(IActivationFactory)||iid==__uuidof(IAgileObject))*out=&factory().activation;
    else if(iid==iid_statics)*out=&factory().statics;else return E_NOINTERFACE;return S_OK;}
Factory& factory(){static const auto activation=winrt_vtable();static const auto statics=[] {auto t=winrt_vtable();t[6]=reinterpret_cast<const void*>(&for_user);t[7]=reinterpret_cast<const void*>(&for_user_scid);return t;}();
    static Factory value{{activation.data(),class_name,__uuidof(IActivationFactory),&factory_query},{statics.data(),class_name,iid_statics,&factory_query}};return value;}
} // namespace
HRESULT connected_storage_factory(REFIID iid,void** out){return factory_query(nullptr,iid,out);}
} // namespace xboxone::era
