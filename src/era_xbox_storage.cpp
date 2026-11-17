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
#include <charconv>
#include <limits>
#include <set>
#include <stdexcept>
#include <thread>
#include <condition_variable>
#include <cstdio>

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
constexpr std::size_t metadata_limit=64*1024*1024;
// Reference DAT_1407dd528 (verified in the supplied executable) and FUN_1404e5370.
constexpr std::uint64_t storage_quota=0x10000000;
constexpr std::uint64_t disk_reserve=0x200;
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
struct StoredContainer {std::wstring name;std::filesystem::path file;};
struct Container:WinrtObject,StoredContainer {
    std::atomic<ULONG> refs{1};Space* space=nullptr;
    ~Container(){if(space)release(space);}
};

// One immutable snapshot per container is replaced atomically, so updates and
// deletions of several blobs either all persist or leave the previous save.
void append(std::vector<std::uint8_t>& bytes,const void* p,std::size_t n) {
    if(n>metadata_limit-bytes.size())winrt::throw_hresult(E_OUTOFMEMORY);
    if(n){auto start=static_cast<const std::uint8_t*>(p);bytes.insert(bytes.end(),start,start+n);}
}
void number(std::vector<std::uint8_t>& bytes,UINT value){append(bytes,&value,sizeof(value));}
void name_bytes(std::vector<std::uint8_t>& bytes,const std::wstring& name){number(bytes,UINT(name.size()));append(bytes,name.data(),name.size()*sizeof(wchar_t));}
std::vector<std::uint8_t> read_file(const std::filesystem::path& path,std::size_t maximum=storage_quota) {
    allowed(path);
    const DWORD attributes=GetFileAttributesW(path.c_str());
    if(attributes!=INVALID_FILE_ATTRIBUTES && (attributes&FILE_ATTRIBUTE_DIRECTORY))winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
    FileHandle file(CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    if(!file.IsValid())winrt::throw_last_error();
    BY_HANDLE_FILE_INFORMATION info{};LARGE_INTEGER size{};
    winrt::check_bool(GetFileInformationByHandle(file.Get(),&info));winrt::check_bool(GetFileSizeEx(file.Get(),&size));
    if((info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT)) || size.QuadPart<0 || std::uint64_t(size.QuadPart)>maximum)
        winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size.QuadPart));DWORD read=0;
    winrt::check_bool(ReadFile(file.Get(),bytes.data(),DWORD(bytes.size()),&read,nullptr));
    if(read!=bytes.size())winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
    return bytes;
}
bool missing(HRESULT error) {return error==HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) || error==HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND);}
Blobs load_legacy(StoredContainer* self,std::wstring* loaded_name=nullptr,const std::filesystem::path* path=nullptr) {
    auto bytes=read_file(path?*path:self->file,storage_quota+metadata_limit);
    if(bytes.size()<16)winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
    const bool hashed=std::memcmp(bytes.data(),"XONECS02",8)==0;
    if(!hashed && std::memcmp(bytes.data(),"XONECS01",8))winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
    std::size_t at=8;
    auto take=[&](std::size_t n){if(n>bytes.size()-at)winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));auto p=bytes.data()+at;at+=n;return p;};
    auto u32=[&](){UINT value;std::memcpy(&value,take(4),4);return value;};
    auto name=[&](){auto n=u32();if(n>255)winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));std::wstring value(n,L'\0');auto p=take(n*sizeof(wchar_t));std::memcpy(value.data(),p,n*sizeof(wchar_t));if(!valid_name(value))winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));return value;};
    auto container_name=name();
    if(loaded_name)*loaded_name=std::move(container_name);
    else if(container_name!=self->name)winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
    const auto count=u32();if(count>(bytes.size()-at)/8)winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
    Blobs blobs;std::uint64_t total=0;
    for(UINT i=0;i<count;++i){
        auto key=name();auto n=u32();if(n>storage_quota-total)winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));total+=n;
        const auto* expected=hashed?take(64):nullptr;auto p=take(n);
        if(hashed){detail::Sha256 hash;hash.update({p,n});const auto actual=hash.finish();
            if(std::memcmp(expected,actual.data(),64))winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));}
        if(!blobs.emplace(std::move(key),std::vector<std::uint8_t>(p,p+n)).second)winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
    }
    if(at!=bytes.size()||bytes.size()-total>metadata_limit)winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
    return blobs;
}
std::filesystem::path revision_root(const StoredContainer* self) {auto path=self->file;return path.replace_extension(L".save");}
void ensure_directory(const std::filesystem::path& path) {
    allowed(path);
    if(!CreateDirectoryW(path.c_str(),nullptr) && GetLastError()!=ERROR_ALREADY_EXISTS)winrt::throw_last_error();
    const DWORD attributes=GetFileAttributesW(path.c_str());
    if(attributes==INVALID_FILE_ATTRIBUTES)winrt::throw_last_error();
    if(!(attributes&FILE_ATTRIBUTE_DIRECTORY) || (attributes&FILE_ATTRIBUTE_REPARSE_POINT))winrt::throw_hresult(E_ACCESSDENIED);
}
std::wstring revision_name(std::uint64_t revision) {
    constexpr wchar_t hex[]=L"0123456789abcdef";std::wstring name(16,L'0');
    for(unsigned i=0;i<16;++i){name[15-i]=hex[revision&15];revision>>=4;}return name;
}
// Reference FUN_1404d9b00: only nonzero, 16-digit lowercase hexadecimal directories.
std::vector<std::uint64_t> revisions(const std::filesystem::path& path) {
    allowed(path);std::vector<std::uint64_t> values;
    const DWORD attributes=GetFileAttributesW(path.c_str());
    if(attributes==INVALID_FILE_ATTRIBUTES){const auto error=GetLastError();if(missing(HRESULT_FROM_WIN32(error)))return values;winrt::throw_hresult(HRESULT_FROM_WIN32(error));}
    if(!(attributes&FILE_ATTRIBUTE_DIRECTORY))winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
    for(const auto& entry:std::filesystem::directory_iterator(path)){
        const auto name=entry.path().filename().wstring();if(name.size()!=16)continue;
        std::uint64_t revision=0;bool valid=true;
        for(const auto c:name){if(!((c>=L'0'&&c<=L'9')||(c>=L'a'&&c<=L'f'))){valid=false;break;}revision=(revision<<4)|(c<=L'9'?c-L'0':c-L'a'+10);}
        if(!valid || !revision)continue;allowed(entry.path());
        if(entry.is_directory())values.push_back(revision);
    }
    std::sort(values.begin(),values.end(),std::greater<>());return values;
}
bool reference_blob_name(const std::wstring& name) {
    // Reference FUN_1404d9f40 limits blob names to 128 UTF-16 code units.
    if(!valid_name(name)||name.size()>128||name.back()==L'.'||name.back()==L' ')return false;
    const auto stem=name.substr(0,name.find(L'.'));
    for(const wchar_t* device:{L"CON",L"PRN",L"AUX",L"NUL",L"CONIN$",L"CONOUT$"})
        if(CompareStringOrdinal(stem.c_str(),int(stem.size()),device,-1,TRUE)==CSTR_EQUAL)return false;
    if(stem.size()==4 && ((stem[3]>=L'1'&&stem[3]<=L'9')||stem[3]==L'\u00b9'||stem[3]==L'\u00b2'||stem[3]==L'\u00b3'))
        if(CompareStringOrdinal(stem.c_str(),3,L"COM",3,TRUE)==CSTR_EQUAL||CompareStringOrdinal(stem.c_str(),3,L"LPT",3,TRUE)==CSTR_EQUAL)return false;
    return WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,name.data(),int(name.size()),nullptr,0,nullptr,nullptr)!=0;
}
template<class Values> void unique_names(const Values& blobs) {
    auto less=[](const std::wstring& a,const std::wstring& b){
        const int result=CompareStringOrdinal(a.data(),int(a.size()),b.data(),int(b.size()),TRUE);
        if(!result)winrt::throw_last_error();return result==CSTR_LESS_THAN;
    };
    std::set<std::wstring,decltype(less)> names(less);
    for(const auto& [name,bytes]:blobs)if(!reference_blob_name(name)||!names.insert(name).second)winrt::throw_hresult(E_INVALIDARG);
}
// Exact v2 marker and blob layout recovered from FUN_1404dc380/FUN_1404de590.
Blobs load_revision(const std::filesystem::path& directory) {
    const auto raw=read_file(directory/L"commit",metadata_limit);
    std::string_view text(reinterpret_cast<const char*>(raw.data()),raw.size());
    const auto line=[&](){const auto end=text.find('\n');if(end==text.npos)winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
        const auto value=text.substr(0,end);text.remove_prefix(end+1);return value;};
    const auto number=[](std::string_view value){std::uint64_t n=0;const auto result=std::from_chars(value.data(),value.data()+value.size(),n);
        if(result.ec!=std::errc{}||result.ptr!=value.data()+value.size())winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));return n;};
    if(line()!="Azimuth complete save revision v2")winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
    const auto count_line=line();if(!count_line.starts_with("blobs "))winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
    const auto count=number(count_line.substr(6));if(count>text.size()/68)winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
    struct Blob{std::string checksum;std::uint64_t size;};std::map<std::wstring,Blob> expected;std::uint64_t total=0;
    for(std::uint64_t i=0;i<count;++i){
        const auto entry=line();if(entry.size()<68||entry[64]!=' ')winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
        const auto hash=entry.substr(0,64);for(const auto c:hash)if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
        const auto split=entry.find(' ',65);if(split==entry.npos)winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
        const auto size=number(entry.substr(65,split-65));if(size>storage_quota-total)winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));total+=size;
        std::wstring name;
        try{name=winrt::to_hstring(entry.substr(split+1));}
        catch(const winrt::hresult_error& error){if(error.code()!=HRESULT_FROM_WIN32(ERROR_NO_UNICODE_TRANSLATION))throw;winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));}
        if(!reference_blob_name(name)||!expected.emplace(name,Blob{std::string(hash),size}).second)winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
    }
    if(!text.empty())winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
    Blobs blobs;
    for(const auto& [name,entry]:expected){std::vector<std::uint8_t> data;
        try{data=read_file(directory/L"data"/name,entry.size);}catch(const winrt::hresult_error& error){if(!missing(error.code()))throw;winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));}
        detail::Sha256 hash;hash.update(data);if(data.size()!=entry.size||hash.finish()!=entry.checksum)winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
        blobs.emplace(name,std::move(data));}
    try{unique_names(blobs);}catch(const winrt::hresult_error& error){if(error.code()!=E_INVALIDARG)throw;winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));}return blobs;
}
Blobs load_from_disk(StoredContainer* self,std::wstring* loaded_name=nullptr,bool* published=nullptr) {
    if(published)*published=false;
    const auto root=revision_root(self);const auto catalog=root/L"name";
    allowed(root);const DWORD attributes=GetFileAttributesW(root.c_str());
    if(attributes!=INVALID_FILE_ATTRIBUTES && !(attributes&FILE_ATTRIBUTE_DIRECTORY))winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
    try {
        std::wstring name;const auto metadata=load_legacy(self,&name,&catalog);
        if(!metadata.empty()||(!self->name.empty()&&name!=self->name))winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
        if(loaded_name){*loaded_name=name;self->name=std::move(name);}
    }catch(const winrt::hresult_error& error){if(!missing(error.code()))throw;}
    bool damaged=false;
    // Reference FUN_1404e02a0: unpublished/corrupt newer revisions never hide
    // an older complete revision. Permission/path failures stay explicit.
    for(const auto revision:revisions(root/L"revisions"))try{
        auto blobs=load_revision(root/L"revisions"/revision_name(revision));if(published)*published=true;return blobs;
    }catch(const winrt::hresult_error& error){
        if(error.code()==HRESULT_FROM_WIN32(ERROR_INVALID_DATA))damaged=true;
        else if(!missing(error.code()))throw;
    }
    try{auto blobs=load_legacy(self,loaded_name);if(published)*published=true;return blobs;}
    catch(const winrt::hresult_error& error){if(!missing(error.code()))throw;}
    if(damaged)winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
    return {};
}
void write_file(const std::filesystem::path& path,std::span<const std::uint8_t> bytes) {
    allowed(path);
    FileHandle file(CreateFileW(path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
    if(!file.IsValid())winrt::throw_last_error();
    try {
        DWORD written=0;winrt::check_bool(WriteFile(file.Get(),bytes.data(),DWORD(bytes.size()),&written,nullptr));
        if(written!=bytes.size())winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_WRITE_FAULT));
        winrt::check_bool(FlushFileBuffers(file.Get()));
    }catch(...){file.Close();DeleteFileW(path.c_str());throw;}
}
void retain_recovery_revisions(const std::filesystem::path& directory) {
    // Reference FUN_1404e2b40 retains a prior completed revision. Keep the newest
    // two verified revisions; damaged/unpublished or unrecognized files remain.
    unsigned retained=0;
    for(const auto value:revisions(directory)){
        const auto revision=directory/revision_name(value);Blobs blobs;
        try{blobs=load_revision(revision);}catch(const winrt::hresult_error& error){
            if(missing(error.code())||error.code()==HRESULT_FROM_WIN32(ERROR_INVALID_DATA))continue;throw;}
        if(++retained<=2)continue;
        std::set<std::filesystem::path> owned{revision/L"data",revision/L"commit"};
        for(const auto& [name,data]:blobs)owned.insert(revision/L"data"/name);
        bool known=true;
        for(const auto& entry:std::filesystem::recursive_directory_iterator(revision)){
            allowed(entry.path());if(!owned.contains(entry.path()))known=false;
        }
        if(known)std::filesystem::remove_all(revision);
    }
}
void store(StoredContainer* self,const Blobs& blobs) {
    unique_names(blobs);
    std::uint64_t total=0;for(const auto& [name,data]:blobs){if(data.size()>storage_quota-total)throw std::runtime_error("Local save exceeds quota");total+=data.size();}
    const auto root=revision_root(self);const auto directory=root/L"revisions";
    ensure_directory(self->file.parent_path());ensure_directory(root);ensure_directory(directory);
    const auto catalog=root/L"name";
    std::vector<std::uint8_t> metadata;append(metadata,"XONECS01",8);name_bytes(metadata,self->name);number(metadata,0);
    const DWORD attributes=GetFileAttributesW(catalog.c_str());
    if(attributes==INVALID_FILE_ATTRIBUTES){
        const auto error=GetLastError();if(!missing(HRESULT_FROM_WIN32(error)))winrt::throw_hresult(HRESULT_FROM_WIN32(error));
        GUID id{};winrt::check_hresult(CoCreateGuid(&id));wchar_t label[40]{};StringFromGUID2(id,label,40);
        const auto temporary=root/(std::wstring(label)+L".tmp");write_file(temporary,metadata);
        try{allowed(catalog);winrt::check_bool(MoveFileExW(temporary.c_str(),catalog.c_str(),MOVEFILE_WRITE_THROUGH));}
        catch(...){DeleteFileW(temporary.c_str());throw;}
    }
    else {std::wstring name;const auto old=load_legacy(self,&name,&catalog);if(!old.empty()||name!=self->name)winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));}
    const auto previous=revisions(directory);
    if(!previous.empty()&&previous.front()==(std::numeric_limits<std::uint64_t>::max)())winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW));
    std::vector<std::filesystem::path> created;created.reserve(blobs.size()+1);
    const auto revision=directory/revision_name(previous.empty()?1:previous.front()+1);
    allowed(revision);winrt::check_bool(CreateDirectoryW(revision.c_str(),nullptr));
    // Reference FUN_1404e34e0: flush every blob before publishing pending->commit.
    std::string marker="Azimuth complete save revision v2\nblobs "+std::to_string(blobs.size())+"\n";
    try{
        ensure_directory(revision/L"data");
        for(const auto& [name,data]:blobs){
            const auto file=revision/L"data"/name;write_file(file,data);created.push_back(file);
            detail::Sha256 hash;hash.update(data);marker+=hash.finish()+" "+std::to_string(data.size())+" "+winrt::to_string(name)+"\n";
            if(marker.size()>metadata_limit)winrt::throw_hresult(E_OUTOFMEMORY);
        }
        const auto pending=revision/L"pending";write_file(pending,{reinterpret_cast<const std::uint8_t*>(marker.data()),marker.size()});created.push_back(pending);
        const auto commit=revision/L"commit";allowed(commit);
        winrt::check_bool(MoveFileExW(pending.c_str(),commit.c_str(),MOVEFILE_WRITE_THROUGH));
    }catch(...){
        for(const auto& file:created)DeleteFileW(file.c_str());
        RemoveDirectoryW((revision/L"data").c_str());RemoveDirectoryW(revision.c_str());throw;
    }
    // The guest write is already committed. Failure to retire older checkpoints
    // preserves them and cannot retroactively turn that committed write into failure.
    try{retain_recovery_revisions(directory);}catch(...){OutputDebugStringW(L"xone-era: older save revisions retained after cleanup failure\n");}
}
void remove_from_disk(StoredContainer* self);

// Reference FUN_1404e4340 coalesces dirty container snapshots on one save worker.
// Guest buffers/COM objects never cross this boundary; only owned host data does.
struct StorageService {
    struct Entry {
        StoredContainer container;
        std::shared_ptr<const Blobs> snapshot;
        std::uint64_t accepted=0,durable=0;
        bool published=false,dirty=false,reset_disk=false;
    };
    std::map<std::filesystem::path,Entry> entries;
    std::condition_variable changed;
    std::thread worker;
    DWORD worker_id=0;
    HRESULT failure=S_OK;
    bool closed=true,joining=false,in_flight=false;

    ~StorageService(){(void)close();}
    void require_open(bool writing=false) {
        if(closed)throw std::runtime_error("Local saves are closed: the session ends");
        if(writing)winrt::check_hresult(failure);
    }
    Entry& entry(StoredContainer* container) {
        require_open();allowed(container->file);
        const auto found=entries.find(container->file);
        if(found!=entries.end()){
            if(!container->name.empty() && found->second.container.name!=container->name)winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
            return found->second;
        }
        StoredContainer descriptor=*container;bool published=false;std::wstring name;
        auto blobs=load_from_disk(&descriptor,descriptor.name.empty()?&name:nullptr,&published);
        if(descriptor.name.empty())descriptor.name=std::move(name);
        Entry value;value.container=std::move(descriptor);value.snapshot=std::make_shared<const Blobs>(std::move(blobs));value.published=published;
        return entries.emplace(container->file,std::move(value)).first->second;
    }
    void submit(StoredContainer* container,Blobs blobs) {
        require_open(true);auto& value=entry(container);
        auto snapshot=std::make_shared<const Blobs>(std::move(blobs));
        if(value.accepted==(std::numeric_limits<std::uint64_t>::max)())winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW));
        value.snapshot=std::move(snapshot);value.published=true;value.dirty=true;++value.accepted;changed.notify_all();
    }
    void erase(StoredContainer container) {
        require_open(true);allowed(container.file);
        auto snapshot=std::make_shared<const Blobs>();
        auto& value=entries[container.file];
        if(value.accepted==(std::numeric_limits<std::uint64_t>::max)())winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_ARITHMETIC_OVERFLOW));
        value.container=std::move(container);value.snapshot=std::move(snapshot);value.published=false;
        // This barrier survives coalescing with a subsequent recreation, so an
        // old checkpoint cannot resurrect data deleted by the title.
        value.reset_disk=true;value.dirty=true;++value.accepted;changed.notify_all();
    }
    auto pending() {return std::find_if(entries.begin(),entries.end(),[](const auto& item){return item.second.dirty;});}
    void run() noexcept {
        std::unique_lock lock(storage_mutex);worker_id=GetCurrentThreadId();
        SetThreadDescription(GetCurrentThread(),L"xone-native saves");
        try{for(;;){
            changed.wait(lock,[&]{return closed||pending()!=entries.end();});
            const auto next=pending();if(next==entries.end()){if(closed)break;continue;}
            auto descriptor=next->second.container;const auto snapshot=next->second.snapshot;
            const auto sequence=next->second.accepted;const bool reset=next->second.reset_disk,publish=next->second.published;
            next->second.dirty=false;next->second.reset_disk=false;in_flight=true;
            lock.unlock();HRESULT result=S_OK;
            try{if(reset)remove_from_disk(&descriptor);if(publish)store(&descriptor,*snapshot);}
            catch(...){result=winrt::to_hresult();}
            lock.lock();auto& completed=entries.at(descriptor.file);
            if(SUCCEEDED(result))completed.durable=sequence;
            else if(SUCCEEDED(failure))failure=result;
            in_flight=false;changed.notify_all();
        }}catch(...){
            const HRESULT result=winrt::to_hresult();if(!lock.owns_lock())lock.lock();
            if(SUCCEEDED(failure))failure=result;
            in_flight=false;for(auto& [file,value]:entries)value.dirty=false;
        }
        changed.notify_all();
    }
    HRESULT begin(bool enabled) noexcept {
        try{
            std::lock_guard lock(storage_mutex);
            if(worker.joinable()||joining)return HRESULT_FROM_WIN32(ERROR_BUSY);
            entries.clear();failure=S_OK;worker_id=0;closed=!enabled;
            if(enabled)worker=std::thread([this]{run();});
            return S_OK;
        }catch(...){std::lock_guard lock(storage_mutex);closed=true;failure=winrt::to_hresult();return failure;}
    }
    HRESULT flush() noexcept {
        try{
            std::unique_lock lock(storage_mutex);
            if(worker_id==GetCurrentThreadId())return HRESULT_FROM_WIN32(ERROR_POSSIBLE_DEADLOCK);
            std::vector<std::pair<std::filesystem::path,std::uint64_t>> target;
            for(const auto& [file,value]:entries)target.emplace_back(file,value.accepted);
            changed.wait(lock,[&]{
                bool covered=true;
                for(const auto& [file,sequence]:target){const auto found=entries.find(file);if(found!=entries.end()&&found->second.durable<sequence)covered=false;}
                return covered||(FAILED(failure)&&!in_flight&&pending()==entries.end());
            });
            return failure;
        }catch(...){return winrt::to_hresult();}
    }
    HRESULT close() noexcept {
        std::thread joining_worker;
        {
            std::unique_lock lock(storage_mutex);
            if(worker_id==GetCurrentThreadId())return HRESULT_FROM_WIN32(ERROR_POSSIBLE_DEADLOCK);
            changed.wait(lock,[&]{return !joining;});
            closed=true;changed.notify_all();
            if(!worker.joinable())return failure;
            joining=true;joining_worker=std::move(worker);
        }
        // No service lock across join: native TLS detach may call title code.
        joining_worker.join();
        std::lock_guard lock(storage_mutex);joining=false;worker_id=0;changed.notify_all();
        if(FAILED(failure)){
            std::uint64_t unwritten=0;for(const auto& [file,value]:entries)unwritten+=value.accepted-value.durable;
            std::fprintf(stderr,"xone-era: %llu accepted save submission(s) not written, HRESULT 0x%08lX\n",unwritten,static_cast<unsigned long>(failure));
        }
        return failure;
    }
};
StorageService& storage_service(){static StorageService service;return service;}
Blobs load(StoredContainer* self,std::wstring* loaded_name=nullptr,bool* published=nullptr) {
    auto& value=storage_service().entry(self);
    if(loaded_name)*loaded_name=value.container.name;if(published)*published=value.published;
    return *value.snapshot;
}

std::uint8_t* data(const IBuffer& buffer) {
    std::uint8_t* p=nullptr;winrt::check_hresult(buffer.as<::Windows::Storage::Streams::IBufferByteAccess>()->Buffer(&p));
    if(!p && buffer.Capacity())winrt::throw_hresult(E_POINTER);return p;
}
std::set<std::filesystem::path> container_files(Space* space) {
    allowed(space->directory/L"enumeration");const auto prefix=digest(space->scid)+L"-";std::set<std::filesystem::path> files;
    for(const auto& entry:std::filesystem::directory_iterator(space->directory)){
        if((entry.path().extension()!=L".bin"&&entry.path().extension()!=L".save")||!entry.path().filename().wstring().starts_with(prefix))continue;
        auto file=entry.path();files.insert(file.replace_extension(L".bin"));
    }
    for(const auto& [file,value]:storage_service().entries)
        if(file.parent_path()==space->directory&&file.filename().wstring().starts_with(prefix))files.insert(file);
    return files;
}
void check_quota(Container* self,std::uint64_t next) {
    if(next>storage_quota)throw std::runtime_error("Local save exceeds quota");
    std::uint64_t used=0;const auto prefix=digest(self->space->scid)+L"-";
    for(const auto& file:container_files(self->space)){
        if(file==self->file)continue;
        Container other;other.file=file;std::wstring name;bool published=false;const auto blobs=load(&other,&name,&published);
        if(!published)continue;
        if(file.filename()!=prefix+digest(name)+L".bin")winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
        for(const auto& [key,bytes]:blobs){if(bytes.size()>storage_quota-used)throw std::runtime_error("Local save quota or disk space exhausted");used+=bytes.size();}
    }
    ULARGE_INTEGER available{};winrt::check_bool(GetDiskFreeSpaceExW(self->space->directory.c_str(),&available,nullptr,nullptr));
    // Like FUN_1404e5370, count the replacement once and require space for its
    // entire new revision while retaining the old one, plus the observed reserve.
    if(next>storage_quota-used||available.QuadPart<next+disk_reserve)throw std::runtime_error("Local save quota or disk space exhausted");
}
IAsyncAction update(Container* self,BufferMap updates,Names deletions) {
    // Completion means the runtime accepted an owned snapshot. The worker's
    // durable completion/failure is observed separately by flush/close.
    std::lock_guard lock(storage_mutex);storage_service().require_open(true);auto blobs=load(self);
    // Reference applies deletions before updates, so an update of the same name wins.
    if(deletions)for(const auto& key:deletions){std::wstring name(key);if(!reference_blob_name(name))winrt::throw_hresult(E_INVALIDARG);blobs.erase(name);}
    std::map<std::wstring,std::size_t> sizes;for(const auto& [name,bytes]:blobs)sizes.emplace(name,bytes.size());
    struct Update{std::wstring name;IBuffer buffer;UINT length;};std::vector<Update> submitted;
    if(updates)for(const auto& pair:updates){std::wstring name(pair.Key());if(!reference_blob_name(name))winrt::throw_hresult(E_INVALIDARG);
        auto value=pair.Value();if(!value)winrt::throw_hresult(E_POINTER);const auto n=value.Length();if(n>value.Capacity())winrt::throw_hresult(E_INVALIDARG);
        if(n>storage_quota)throw std::runtime_error("Save blob exceeds quota");
        sizes[name]=n;submitted.push_back({std::move(name),value,n});}
    unique_names(sizes);std::uint64_t total=0;
    for(const auto& [name,n]:sizes){if(n>storage_quota-total)throw std::runtime_error("Local save exceeds quota");total+=n;}
    check_quota(self,total);
    for(const auto& update:submitted){auto p=data(update.buffer);std::vector<std::uint8_t> bytes;if(update.length)bytes.assign(p,p+update.length);blobs[update.name]=std::move(bytes);}
    storage_service().submit(self,std::move(blobs));co_return;
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
void remove_from_disk(StoredContainer* self) {
    const auto file=self->file;allowed(file);const auto root=revision_root(self);allowed(root);
    const auto exists=[](const std::filesystem::path& path){const DWORD attributes=GetFileAttributesW(path.c_str());
        if(attributes!=INVALID_FILE_ATTRIBUTES)return true;const auto error=GetLastError();if(missing(HRESULT_FROM_WIN32(error)))return false;winrt::throw_hresult(HRESULT_FROM_WIN32(error));};
    bool damaged=false;
    try{load_from_disk(self);}catch(const winrt::hresult_error& error){if(error.code()!=HRESULT_FROM_WIN32(ERROR_INVALID_DATA))throw;damaged=true;}
    if(damaged){
        // Reference FUN_1404e1d50 preserves an unverifiable container on deletion.
        GUID id{};winrt::check_hresult(CoCreateGuid(&id));wchar_t label[40]{};StringFromGUID2(id,label,40);
        const auto archive=file.parent_path()/(L"damaged-saves-"+std::wstring(label));allowed(archive);winrt::check_bool(CreateDirectoryW(archive.c_str(),nullptr));
        bool moved=false;
        try{
            if(exists(root)){winrt::check_bool(MoveFileExW(root.c_str(),(archive/L"container").c_str(),MOVEFILE_WRITE_THROUGH));moved=true;}
            if(exists(file))winrt::check_bool(MoveFileExW(file.c_str(),(archive/L"legacy.bin").c_str(),MOVEFILE_WRITE_THROUGH));
        }catch(...){if(moved)MoveFileExW((archive/L"container").c_str(),root.c_str(),MOVEFILE_WRITE_THROUGH);RemoveDirectoryW(archive.c_str());throw;}
        return;
    }
    if(exists(root)){
        // Only the computed container tree in this writable profile is removed;
        // validate every descendant first, without following reparse points.
        for(const auto& entry:std::filesystem::recursive_directory_iterator(root))allowed(entry.path());
        std::filesystem::remove_all(root);
    }
    if(!DeleteFileW(file.c_str())){const auto error=GetLastError();if(error!=ERROR_FILE_NOT_FOUND && error!=ERROR_PATH_NOT_FOUND)winrt::throw_hresult(HRESULT_FROM_WIN32(error));}
}
IAsyncAction remove_container(Space* self,winrt::hstring name) {
    std::lock_guard lock(storage_mutex);if(!valid_name(std::wstring(name)))winrt::throw_hresult(E_INVALIDARG);
    StoredContainer container;container.name=std::wstring(name);container.file=self->directory/(digest(self->scid)+L"-"+digest(container.name)+L".bin");
    storage_service().erase(std::move(container));co_return;
}
HRESULT STDMETHODCALLTYPE space_delete(Space* self,HSTRING name,void** out) try {
    if(!out)return E_POINTER;*out=nullptr;*out=winrt::detach_abi(remove_container(self,winrt::hstring(string(name))));return S_OK;
}catch(...){return winrt::to_hresult();}

constexpr GUID iid_info_query{0x4f937563,0xbae3,0x4062,{0xa6,0x7f,0xcb,0xea,0x61,0xa2,0xed,0xc3}};
struct InfoQuery:WinrtObject{std::atomic<ULONG> refs{1};Space* space=nullptr;std::wstring prefix;~InfoQuery(){if(space)release(space);}};
std::vector<ContainerInfo> enumerate(InfoQuery* self) {
    std::lock_guard lock(storage_mutex);allowed(self->space->directory/L"enumeration");
    const auto file_prefix=digest(self->space->scid)+L"-";std::vector<ContainerInfo> values;
    for(const auto& file:container_files(self->space)){
        Container container;container.file=file;std::wstring name;bool published=false;const auto blobs=load(&container,&name,&published);
        if(!published)continue;
        if(file.filename()!=file_prefix+digest(name)+L".bin")winrt::throw_hresult(HRESULT_FROM_WIN32(ERROR_INVALID_DATA));
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
HRESULT begin_connected_storage(bool enabled){return storage_service().begin(enabled);}
HRESULT flush_connected_storage(){return storage_service().flush();}
HRESULT close_connected_storage(){return storage_service().close();}
} // namespace xboxone::era
