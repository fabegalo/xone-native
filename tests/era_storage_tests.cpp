// SPDX-License-Identifier: BSD-3-Clause
#include "../src/era_winrt_classes.h"
#include "xboxone_native/era_loader.h"
#include "../src/file_path.h"
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Storage.Streams.h>
#include <robuffer.h>
#include <winstring.h>
#include <wrl/client.h>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <algorithm>
#include <cstring>

using Microsoft::WRL::ComPtr;
namespace fs=std::filesystem;
using winrt::Windows::Foundation::IAsyncAction;
using winrt::Windows::Foundation::IAsyncOperation;
using namespace winrt::Windows::Foundation::Collections;
using namespace winrt::Windows::Storage::Streams;
using Map=IMap<winrt::hstring,IBuffer>;
int failures=0;
void check(bool ok,const char* message){if(!ok){++failures;std::cerr<<"FAIL: "<<message<<'\n';}}
void** table(void* p){return *reinterpret_cast<void***>(p);}
template<class T>T method(void* p,unsigned slot){return reinterpret_cast<T>(table(p)[slot]);}
using Get=HRESULT(STDMETHODCALLTYPE*)(void*,void**);
using Named=HRESULT(STDMETHODCALLTYPE*)(void*,HSTRING,void**);
using ForUser=HRESULT(STDMETHODCALLTYPE*)(void*,IInspectable*,HSTRING,void**);
using Update=HRESULT(STDMETHODCALLTYPE*)(void*,void*,void*,void**);
using Read=HRESULT(STDMETHODCALLTYPE*)(void*,void*,void**);
HSTRING abi(const winrt::hstring& s){return static_cast<HSTRING>(winrt::get_abi(s));}
std::uint8_t* bytes(const IBuffer& buffer){std::uint8_t* p=nullptr;winrt::check_hresult(buffer.as<::Windows::Storage::Streams::IBufferByteAccess>()->Buffer(&p));return p;}
Buffer blob(std::initializer_list<std::uint8_t> data){Buffer b(UINT(data.size()));b.Length(UINT(data.size()));std::copy(data.begin(),data.end(),bytes(b));return b;}
HRESULT status(IAsyncAction action){try{action.GetResults();return S_OK;}catch(const winrt::hresult_error& error){return error.code();}}
IAsyncAction update(void* container,const Map& map,const IVector<winrt::hstring>& remove=nullptr){
    void* raw=nullptr;auto view=map?map.GetView():nullptr;
    auto names=remove?remove.as<IIterable<winrt::hstring>>():nullptr;
    check(method<Update>(container,8)(container,winrt::get_abi(view),winrt::get_abi(names),&raw)==S_OK && raw,"SubmitUpdatesAsync returns an operation");
    return {raw,winrt::take_ownership_from_abi};
}
IAsyncAction read(void* container,const Map& map){void* raw=nullptr;auto view=map.GetView();
    check(method<Read>(container,9)(container,winrt::get_abi(view),&raw)==S_OK && raw,"ReadAsync returns an operation");return {raw,winrt::take_ownership_from_abi};}
ComPtr<IInspectable> space(void* statics,IInspectable* user,const winrt::hstring& scid){
    ComPtr<IInspectable> op,result;
    winrt::check_hresult(method<ForUser>(statics,7)(statics,user,abi(scid),reinterpret_cast<void**>(op.GetAddressOf())));
    winrt::check_hresult(method<Get>(op.Get(),8)(op.Get(),reinterpret_cast<void**>(result.GetAddressOf())));return result;
}
ComPtr<IInspectable> container(void* space,const winrt::hstring& name){ComPtr<IInspectable> result;
    winrt::check_hresult(method<Named>(space,9)(space,abi(name),reinterpret_cast<void**>(result.GetAddressOf())));return result;}
void query_info(void* space,const winrt::hstring& prefix,UINT expected_count,UINT expected_bytes=0){
    ComPtr<IInspectable> query,operation,view;
    winrt::check_hresult(method<Named>(space,11)(space,abi(prefix),reinterpret_cast<void**>(query.GetAddressOf())));
    winrt::check_hresult(method<Get>(query.Get(),8)(query.Get(),reinterpret_cast<void**>(operation.GetAddressOf())));
    using Count=HRESULT(STDMETHODCALLTYPE*)(void*,UINT*);UINT count=99;
    winrt::check_hresult(method<Count>(operation.Get(),8)(operation.Get(),&count));check(count==expected_count,"query counts the persisted containers matching its prefix");
    operation.Reset();winrt::check_hresult(method<Get>(query.Get(),7)(query.Get(),reinterpret_cast<void**>(operation.GetAddressOf())));
    winrt::check_hresult(method<Get>(operation.Get(),8)(operation.Get(),reinterpret_cast<void**>(view.GetAddressOf())));
    winrt::check_hresult(method<Count>(view.Get(),7)(view.Get(),&count));check(count==expected_count,"query returns a real container-info vector");
    if(expected_count){
        struct Entry{HSTRING name=nullptr;UINT size=0;};static_assert(sizeof(Entry)==16);
        using At=HRESULT(STDMETHODCALLTYPE*)(void*,UINT,Entry*);Entry info;
        winrt::check_hresult(method<At>(view.Get(),6)(view.Get(),0,&info));UINT length=0;auto name=WindowsGetStringRawBuffer(info.name,&length);
        check(std::wstring_view(name,length)==L"Saves" && info.size==expected_bytes,"container-info ABI returns exact name and summed blob bytes");WindowsDeleteString(info.name);
        check(method<At>(view.Get(),6)(view.Get(),count,&info)==E_BOUNDS,"container-info bounds are enforced");
    }
}
std::string contents(const fs::path& file){std::ifstream in(file,std::ios::binary);return {std::istreambuf_iterator<char>(in),{}};}
int main()try{
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    const auto temp=xboxone::detail::file_path(fs::canonical(fs::temp_directory_path()));
    const auto root=temp/(L"xone-storage-test-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    fs::create_directory(root);
    struct Cleanup{fs::path root,temp;~Cleanup(){if(root.parent_path()==temp && root.filename().wstring().starts_with(L"xone-storage-test-")){std::error_code error;fs::remove_all(root,error);}}}cleanup{root,temp};
    xboxone::era::set_title_storage_root(root.wstring());
    ComPtr<IInspectable> user,statics;
    winrt::check_hresult(xboxone::era::title_local_user(reinterpret_cast<void**>(user.GetAddressOf())));
    constexpr GUID iid{0x07020414,0xb3e9,0x47c8,{0xa3,0x6f,0xf2,0x4e,0x2d,0xc6,0x55,0x73}};
    winrt::check_hresult(xboxone::era::connected_storage_factory(iid,reinterpret_cast<void**>(statics.GetAddressOf())));
    auto first=space(statics.Get(),user.Get(),L"owned-test-scid");
    auto save=container(first.Get(),L"Saves");
    query_info(first.Get(),L"",0);
    ComPtr<IInspectable> owner;winrt::check_hresult(method<Get>(save.Get(),7)(save.Get(),reinterpret_cast<void**>(owner.GetAddressOf())));
    check(owner.Get()==first.Get(),"container retains its original owning space");
    ComPtr<IInspectable> saved_user;winrt::check_hresult(method<Get>(first.Get(),6)(first.Get(),reinterpret_cast<void**>(saved_user.GetAddressOf())));
    check(saved_user.Get()==user.Get(),"storage retains the selected profile");
    void* invalid=reinterpret_cast<void*>(1);
    check(method<Named>(first.Get(),9)(first.Get(),abi(L"../escape"),&invalid)==E_INVALIDARG && !invalid,"container traversal is rejected");
    auto writes=winrt::single_threaded_map<winrt::hstring,IBuffer>();writes.Insert(L"state",blob({1,2,3,4}));writes.Insert(L"settings",blob({5,6}));
    check(status(update(save.Get(),writes))==S_OK,"several blobs commit together");
    query_info(first.Get(),L"Sa",1,6);query_info(first.Get(),L"Missing",0);
    fs::path file;for(const auto& entry:fs::directory_iterator(root/L"connected"))if(entry.path().extension()==L".bin")file=entry.path();
    check(!file.empty(),"a real container file was published");const auto original=contents(file);
    check(original.starts_with("XONECS01"),"published file is a versioned local container");
    // Recreate user/space/container snapshots to exercise on-disk persistence.
    xboxone::era::set_title_storage_root(root.wstring());
    winrt::check_hresult(xboxone::era::title_local_user(reinterpret_cast<void**>(user.ReleaseAndGetAddressOf())));
    auto reopened=space(statics.Get(),user.Get(),L"owned-test-scid");auto reopened_save=container(reopened.Get(),L"Saves");
    auto reads=winrt::single_threaded_map<winrt::hstring,IBuffer>();Buffer target(8);target.Length(8);std::memset(bytes(target),0xcc,8);reads.Insert(L"state",target);
    check(status(read(reopened_save.Get(),reads))==S_OK && target.Length()==4 && bytes(target)[0]==1 && bytes(target)[3]==4 && bytes(target)[4]==0xcc,"reopened save reads exact bytes/length and preserves guards");
    Buffer tiny(2);tiny.Length(2);bytes(tiny)[0]=0xcc;reads.Clear();reads.Insert(L"state",tiny);
    check(status(read(save.Get(),reads))==HRESULT(0x80830007) && bytes(tiny)[0]==0xcc && tiny.Length()==2,"short output is rejected without partial copy");
    writes.Insert(L"state",blob({9}));writes.Insert(L"zz/invalid",blob({0}));
    check(status(update(save.Get(),writes))==E_INVALIDARG && contents(file)==original,"failed batch leaves every blob and the previous container unchanged");
    writes.Remove(L"zz/invalid");auto deletions=winrt::single_threaded_vector<winrt::hstring>({L"settings"});
    check(status(update(save.Get(),writes,deletions))==S_OK,"replacement and deletion commit atomically");
    query_info(first.Get(),L"",1,1);
    auto names=winrt::single_threaded_vector<winrt::hstring>({L"state"});void* raw=nullptr;
    winrt::check_hresult(method<Read>(save.Get(),10)(save.Get(),winrt::get_abi(names.as<IIterable<winrt::hstring>>()),&raw));
    IAsyncOperation<IMapView<winrt::hstring,IBuffer>> get{raw,winrt::take_ownership_from_abi};auto found=get.GetResults();
    check(found.Size()==1 && found.Lookup(L"state").Length()==1 && bytes(found.Lookup(L"state"))[0]==9,"GetAsync returns the real requested blob");
    reads.Clear();reads.Insert(L"settings",target);
    check(status(read(save.Get(),reads))==HRESULT(0x80830008),"deleted blob reports storage BLOB_NOT_FOUND");
    auto other=space(statics.Get(),user.Get(),L"another-scid");auto other_save=container(other.Get(),L"Saves");
    reads.Clear();reads.Insert(L"state",target);
    check(status(read(other_save.Get(),reads))==HRESULT(0x80830008),"service configurations cannot read each other's saves");
    auto second=root/L"second";fs::create_directory(second);xboxone::era::set_title_storage_root(second.wstring());
    ComPtr<IInspectable> second_user;winrt::check_hresult(xboxone::era::title_local_user(reinterpret_cast<void**>(second_user.GetAddressOf())));
    invalid=reinterpret_cast<void*>(1);
    check(method<ForUser>(statics.Get(),7)(statics.Get(),user.Get(),abi(L"owned-test-scid"),&invalid)==E_INVALIDARG && !invalid,"a retained user from another session cannot bind the current storage");
    check(status(read(save.Get(),reads))==E_ACCESSDENIED,"stale storage cannot be redirected to the next profile");
    xboxone::era::set_title_storage_root(root.wstring());
    {std::ofstream corrupt(file,std::ios::binary|std::ios::trunc);corrupt<<"invalid";}
    check(status(update(save.Get(),writes))==HRESULT_FROM_WIN32(ERROR_INVALID_DATA) && contents(file)=="invalid","corrupt saves are refused and preserved");
    xboxone::era::set_title_storage_root(L"");
    std::cout<<"storage failures "<<failures<<'\n';return failures?1:0;
}catch(const winrt::hresult_error& error){std::cerr<<"storage HRESULT "<<std::hex<<unsigned(error.code().value)<<'\n';return 1;}
