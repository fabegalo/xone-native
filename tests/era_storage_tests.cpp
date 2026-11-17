// SPDX-License-Identifier: BSD-3-Clause
#include "../src/era_winrt_classes.h"
#include "xboxone_native/era_loader.h"
#include "../src/file_path.h"
#include "../src/sha256.h"
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
#include <cstdlib>

using Microsoft::WRL::ComPtr;
namespace fs=std::filesystem;
using winrt::Windows::Foundation::IAsyncAction;
using winrt::Windows::Foundation::IAsyncOperation;
using namespace winrt::Windows::Foundation::Collections;
using namespace winrt::Windows::Storage::Streams;
using Map=IMap<winrt::hstring,IBuffer>;
int failures=0;
bool wait_for_disk=true;
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
HRESULT status(IAsyncAction action){try{action.GetResults();return wait_for_disk?xboxone::era::flush_connected_storage():S_OK;}catch(const winrt::hresult_error& error){return error.code();}}
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
void write(const fs::path& file,std::string_view value){
    // External corruption is observed on a new session, as in the reference's
    // in-memory container model. No guest buffer is being submitted here.
    winrt::check_hresult(xboxone::era::close_connected_storage());
    winrt::check_hresult(xboxone::era::begin_connected_storage(!xboxone::era::title_storage_root().empty()));
    std::ofstream out(file,std::ios::binary|std::ios::trunc);out.write(value.data(),value.size());if(!out)throw std::runtime_error("owned fixture write failed");
}
std::wstring digest(std::wstring_view value){xboxone::detail::Sha256 hash;hash.update({reinterpret_cast<const std::uint8_t*>(value.data()),value.size()*sizeof(wchar_t)});auto text=hash.finish();return {text.begin(),text.end()};}
IAsyncAction erase(void* space,const winrt::hstring& name){void* raw=nullptr;winrt::check_hresult(method<Named>(space,10)(space,abi(name),&raw));return {raw,winrt::take_ownership_from_abi};}
int wmain(int argc,wchar_t** argv)try{
    winrt::init_apartment(winrt::apartment_type::multi_threaded);
    const auto temp=xboxone::detail::file_path(fs::canonical(fs::temp_directory_path()));
    const bool exit_child=argc==3&&(std::wstring_view(argv[1])==L"--native-exit"||std::wstring_view(argv[1])==L"--crt-exit");
    const auto root=exit_child?xboxone::detail::file_path(fs::absolute(argv[2]).lexically_normal()):temp/(L"xone-storage-test-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64()));
    if(exit_child && (root.parent_path().parent_path()!=temp||!root.parent_path().filename().wstring().starts_with(L"xone-storage-test-")||!root.filename().wstring().starts_with(L"exit-")))return 2;
    fs::create_directory(root);
    struct Cleanup{fs::path root,temp;~Cleanup(){if(root.parent_path()==temp && root.filename().wstring().starts_with(L"xone-storage-test-")){std::error_code error;fs::remove_all(root,error);}}}cleanup{root,temp};
    winrt::check_hresult(xboxone::era::set_title_storage_root(root.wstring()));
    const auto create=reinterpret_cast<decltype(&CreateFileW)>(xboxone::era::resolve_kernelx({"kernelx.dll","CreateFileW",0}));
    check(create!=nullptr,"storage uses the existing kernel file boundary");
    if(create){
        const auto missing_path=root/L"not-created"/L"parent"/L"blob";
        const auto handle=create(missing_path.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        const auto error=GetLastError();if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);
        check(handle==INVALID_HANDLE_VALUE && error==ERROR_PATH_NOT_FOUND && !fs::exists(root/L"not-created"),
            "a missing parent under the owned root reports the real Windows path error without creating anything");
        const auto outside=temp/(root.filename().wstring()+L"-outside")/L"missing"/L"blob";
        const auto denied=create(outside.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        const auto denied_error=GetLastError();if(denied!=INVALID_HANDLE_VALUE)CloseHandle(denied);
        check(denied==INVALID_HANDLE_VALUE && denied_error==ERROR_ACCESS_DENIED,"missing outside parents cannot bypass the profile root");
    }
    ComPtr<IInspectable> user,statics;
    winrt::check_hresult(xboxone::era::title_local_user(reinterpret_cast<void**>(user.GetAddressOf())));
    constexpr GUID iid{0x07020414,0xb3e9,0x47c8,{0xa3,0x6f,0xf2,0x4e,0x2d,0xc6,0x55,0x73}};
    winrt::check_hresult(xboxone::era::connected_storage_factory(iid,reinterpret_cast<void**>(statics.GetAddressOf())));
    auto first=space(statics.Get(),user.Get(),L"owned-test-scid");
    if(exit_child){
        wait_for_disk=false;auto save=container(first.Get(),L"Saves");auto values=winrt::single_threaded_map<winrt::hstring,IBuffer>();
        for(unsigned i=0;i<16;++i){values.Insert(L"state",blob({std::uint8_t(i)}));if(FAILED(status(update(save.Get(),values))))return 3;}
        if(std::wstring_view(argv[1])==L"--crt-exit")std::exit(24);
        const auto exit=reinterpret_cast<decltype(&ExitProcess)>(xboxone::era::resolve_kernelx({"kernelx.dll","ExitProcess",0}));
        if(!exit||exit==&ExitProcess)return 4;exit(23);
    }
    if(argc==2 && std::wstring_view(argv[1])==L"--worker-failure"){
        wait_for_disk=false;auto save=container(first.Get(),L"Saves");auto reads=winrt::single_threaded_map<winrt::hstring,IBuffer>();Buffer target(1);reads.Insert(L"state",target);
        check(status(read(save.Get(),reads))==HRESULT(0x80830008),"failure probe starts with a genuinely empty container");
        const auto saved=root/L"connected"/(digest(L"owned-test-scid")+L"-"+digest(L"Saves")+L".save");
        fs::create_directories(saved/L"revisions");{std::ofstream blocker(saved/L"revisions"/L"0000000000000001");blocker<<"owned blocker";}
        auto values=winrt::single_threaded_map<winrt::hstring,IBuffer>();values.Insert(L"state",blob({7}));
        check(status(update(save.Get(),values))==S_OK,"submission completion records acceptance before the disk outcome");
        check(status(read(save.Get(),reads))==S_OK && bytes(target)[0]==7,"accepted state remains coherent when the writer fails");
        const HRESULT failed=xboxone::era::flush_connected_storage();
        check(FAILED(failed) && xboxone::era::flush_connected_storage()==failed,"flush preserves the actual deferred failure");
        check(status(update(save.Get(),values))==failed,"a failed writer cannot accept another apparently successful submission");
        check(contents(saved/L"revisions"/L"0000000000000001")=="owned blocker","failed publication preserves the occupied destination");
        check(xboxone::era::close_connected_storage()==failed,"close joins the failed worker and reports undurable submissions");
        const auto previous=xboxone::era::title_storage_root();
        check(xboxone::era::set_title_storage_root((root/L"another-profile").wstring())==failed && xboxone::era::title_storage_root()==previous,
            "a failed close cannot silently discard pending state while changing profiles");
        std::cout<<"worker failure checks "<<failures<<'\n';return failures?1:0;
    }
    if(argc==2 && std::wstring_view(argv[1])==L"--worker"){
        wait_for_disk=false;auto save=container(first.Get(),L"Saves");auto second=container(first.Get(),L"Saves");
        auto values=winrt::single_threaded_map<winrt::hstring,IBuffer>();auto input=blob({7});values.Insert(L"state",input);values.Insert(L"old",blob({4}));
        check(status(update(save.Get(),values))==S_OK,"the runtime accepts a snapshot for its save worker");
        bytes(input)[0]=99;values.Clear();save.Reset();
        auto reads=winrt::single_threaded_map<winrt::hstring,IBuffer>();Buffer target(1);reads.Insert(L"state",target);
        check(status(read(second.Get(),reads))==S_OK&&bytes(target)[0]==7,"another container handle sees the accepted snapshot after caller buffers/objects change");
        query_info(first.Get(),L"Sa",1,2);
        winrt::check_hresult(xboxone::era::flush_connected_storage());
        winrt::check_hresult(xboxone::era::set_title_storage_root(root.wstring()));
        check(status(read(second.Get(),reads))==S_OK&&bytes(target)[0]==7,"a cold session observes the durable owned snapshot");
        check(status(erase(first.Get(),L"Saves"))==S_OK,"deletion is ordered through the same worker");
        values.Insert(L"state",blob({3}));check(status(update(second.Get(),values))==S_OK,"immediate recreation follows the deletion barrier");
        winrt::check_hresult(xboxone::era::close_connected_storage());
        check(status(update(second.Get(),values))==E_FAIL,"shutdown refuses new submissions with the reference's closed-save failure");
        winrt::check_hresult(xboxone::era::set_title_storage_root(root.wstring()));
        reads.Clear();reads.Insert(L"old",target);
        check(status(read(second.Get(),reads))==HRESULT(0x80830008),"deletion/recreation cannot resurrect an old blob or checkpoint");
        reads.Clear();reads.Insert(L"state",target);check(status(read(second.Get(),reads))==S_OK&&bytes(target)[0]==3,"close persisted the latest accepted recreation");
        wchar_t executable[32768]{};if(!GetModuleFileNameW(nullptr,executable,32768))return 1;
        for(const auto& native:{true,false}){
            const auto child=root/(native?L"exit-native":L"exit-crt");
            std::wstring command=L"\""+std::wstring(executable)+L"\" "+(native?L"--native-exit":L"--crt-exit")+L" \""+child.wstring()+L"\"";
            STARTUPINFOW startup{};startup.cb=sizeof startup;PROCESS_INFORMATION process{};
            const BOOL launched=CreateProcessW(executable,command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&startup,&process);
            check(launched!=FALSE,"owned exit fixture starts");if(!launched)continue;
            CloseHandle(process.hThread);const auto wait=WaitForSingleObject(process.hProcess,10000);
            if(wait!=WAIT_OBJECT_0){TerminateProcess(process.hProcess,99);WaitForSingleObject(process.hProcess,5000);}
            DWORD code=0;GetExitCodeProcess(process.hProcess,&code);CloseHandle(process.hProcess);
            check(wait==WAIT_OBJECT_0&&code==(native?23:24),"normal native/CRT exit preserves its requested code after joining saves");
            winrt::check_hresult(xboxone::era::set_title_storage_root(child.wstring()));
            ComPtr<IInspectable> child_user;winrt::check_hresult(xboxone::era::title_local_user(reinterpret_cast<void**>(child_user.GetAddressOf())));
            auto child_space=space(statics.Get(),child_user.Get(),L"owned-test-scid");auto child_save=container(child_space.Get(),L"Saves");
            check(status(read(child_save.Get(),reads))==S_OK&&bytes(target)[0]==15,"the child exit drained its last accepted save to actual disk");
        }
        winrt::check_hresult(xboxone::era::set_title_storage_root(L""));
        std::cout<<"worker checks "<<failures<<'\n';return failures?1:0;
    }
    if(argc==2 && std::wstring_view(argv[1])==L"--quota"){
        constexpr UINT block_size=65u*1024u*1024u;
        Buffer payload(block_size);payload.Length(block_size);std::memset(bytes(payload),0xa5,block_size);
        auto values=winrt::single_threaded_map<winrt::hstring,IBuffer>();values.Insert(L"state",payload);
        std::vector<ComPtr<IInspectable>> containers;
        for(const wchar_t* name:{L"quota-a",L"quota-b",L"quota-c"}){
            containers.push_back(container(first.Get(),name));
            check(status(update(containers.back().Get(),values))==S_OK,"a real 65 MiB blob exceeds the former parser bound but fits the reference quota");
        }
        auto fourth=container(first.Get(),L"quota-d");
        check(FAILED(status(update(fourth.Get(),values))),"four 65 MiB containers cannot exceed the shared 256 MiB space quota");
        unsigned published=0;for(const auto& entry:fs::directory_iterator(root/L"connected"))if(entry.path().extension()==L".save")++published;
        check(published==3,"quota rejection publishes no fourth container or partial revision");
        struct Oversized:winrt::implements<Oversized,IBuffer>{
            UINT Capacity()const{return 0x10000001;}UINT Length()const{return 0x10000001;}
            void Length(UINT){winrt::throw_hresult(E_NOTIMPL);}
        };
        values.Clear();values.Insert(L"oversized",winrt::make<Oversized>());
        // This object deliberately has no IBufferByteAccess. The reference's
        // size check must reject it before asking for or allocating its payload.
        check(status(update(fourth.Get(),values))==E_FAIL,"an oversized blob is rejected by quota before accessing its bytes");
        bytes(payload)[0]=0;bytes(payload)[block_size-1]=0;
        auto reads=winrt::single_threaded_map<winrt::hstring,IBuffer>();reads.Insert(L"state",payload);
        check(status(read(containers.front().Get(),reads))==S_OK && payload.Length()==block_size && bytes(payload)[0]==0xa5 && bytes(payload)[block_size-1]==0xa5,
            "a rejected submission leaves the previously committed large payload intact");
        check(status(update(containers.front().Get(),reads))==S_OK,"replacement counts its current container once instead of double charging the quota");
        xboxone::era::set_title_storage_root(L"");
        std::cout<<"quota failures "<<failures<<'\n';return failures?1:0;
    }
    auto save=container(first.Get(),L"Saves");
    query_info(first.Get(),L"",0);
    ComPtr<IInspectable> owner;winrt::check_hresult(method<Get>(save.Get(),7)(save.Get(),reinterpret_cast<void**>(owner.GetAddressOf())));
    check(owner.Get()==first.Get(),"container retains its original owning space");
    ComPtr<IInspectable> saved_user;winrt::check_hresult(method<Get>(first.Get(),6)(first.Get(),reinterpret_cast<void**>(saved_user.GetAddressOf())));
    check(saved_user.Get()==user.Get(),"storage retains the selected profile");
    void* invalid=reinterpret_cast<void*>(1);
    check(method<Named>(first.Get(),9)(first.Get(),abi(L"../escape"),&invalid)==E_INVALIDARG && !invalid,"container traversal is rejected");
    auto writes=winrt::single_threaded_map<winrt::hstring,IBuffer>();writes.Insert(L"state",blob({1,2,3,4}));writes.Insert(L"settings",blob({5,6}));
    const auto initial=status(update(save.Get(),writes));check(initial==S_OK,"several blobs commit together");winrt::check_hresult(initial);
    query_info(first.Get(),L"Sa",1,6);query_info(first.Get(),L"Missing",0);
    fs::path saved;for(const auto& entry:fs::directory_iterator(root/L"connected"))if(entry.path().extension()==L".save")saved=entry.path();
    check(!saved.empty(),"the existing profile owns the real revision container");
    const auto first_revision=saved/L"revisions"/L"0000000000000001";
    const auto file=first_revision/L"data"/L"state";const auto original=contents(file);const auto marker=contents(first_revision/L"commit");
    check(marker=="Azimuth complete save revision v2\nblobs 2\n"
        "c42522128b49193de8cd45d8f7589cd7e085e65f138640d57d4482e5f7189623 2 settings\n"
        "9f64a747e1b97f131fabb6b447296c9b6f0201e79fb3c5356e6c77e89b6a806a 4 state\n",
        "published marker matches the recovered reference format and independently computed SHA-256 values");
    check(!fs::exists(first_revision/L"pending") && contents(first_revision/L"data"/L"settings")==std::string("\5\6",2),
        "pending becomes commit only after every exact blob payload is written");
    // Recreate user/space/container snapshots to exercise on-disk persistence.
    xboxone::era::set_title_storage_root(root.wstring());
    winrt::check_hresult(xboxone::era::title_local_user(reinterpret_cast<void**>(user.ReleaseAndGetAddressOf())));
    auto reopened=space(statics.Get(),user.Get(),L"owned-test-scid");auto reopened_save=container(reopened.Get(),L"Saves");
    auto reads=winrt::single_threaded_map<winrt::hstring,IBuffer>();Buffer target(8);target.Length(8);std::memset(bytes(target),0xcc,8);reads.Insert(L"state",target);
    check(status(read(reopened_save.Get(),reads))==S_OK && target.Length()==4 && bytes(target)[0]==1 && bytes(target)[3]==4 && bytes(target)[4]==0xcc,"reopened save reads exact bytes/length and preserves guards");
    auto damaged=original;damaged.back()^=1;
    write(file,damaged);
    std::memset(bytes(target),0xcc,8);target.Length(8);
    check(status(read(reopened_save.Get(),reads))==HRESULT_FROM_WIN32(ERROR_INVALID_DATA) && target.Length()==8 && bytes(target)[0]==0xcc,
        "a same-size corrupted blob is rejected before modifying any output buffer");
    check(status(update(save.Get(),writes))==HRESULT_FROM_WIN32(ERROR_INVALID_DATA) && contents(file)==damaged,
        "a failed integrity check preserves the damaged revision without publishing over it");
    damaged=original;damaged.pop_back();
    write(file,damaged);
    check(status(read(save.Get(),reads))==HRESULT_FROM_WIN32(ERROR_INVALID_DATA) && target.Length()==8 && bytes(target)[0]==0xcc,
        "a truncated committed blob is refused without a partial read");
    write(file,original);
    const std::string state_line="9f64a747e1b97f131fabb6b447296c9b6f0201e79fb3c5356e6c77e89b6a806a 4 state\n";
    const std::string header="Azimuth complete save revision v2\n";
    for(const auto& invalid_marker:std::vector<std::string>{
        "unknown revision\nblobs 0\n", marker+"extra\n", header+"blobs 2\n"+state_line+state_line,
        header+"blobs 18446744073709551616\n", header+"blobs 1\n"+state_line.substr(0,65)+"18446744073709551616 state\n",
        header+"blobs 1\n"+state_line.substr(0,67)+std::string("\xff\n",2)}){
        write(first_revision/L"commit",invalid_marker);
        check(status(read(save.Get(),reads))==HRESULT_FROM_WIN32(ERROR_INVALID_DATA) && target.Length()==8 && bytes(target)[0]==0xcc,
            "unknown/trailing/duplicate/overflow/invalid-UTF8 reference markers cannot produce partial data");
    }
    write(first_revision/L"commit",marker);
    fs::rename(first_revision/L"data",first_revision/L"owned-test-data");
    check(status(read(save.Get(),reads))==HRESULT_FROM_WIN32(ERROR_INVALID_DATA),"a committed revision missing its data directory is damaged, not an empty save");
    fs::rename(first_revision/L"owned-test-data",first_revision/L"data");
    write(first_revision/L"data"/L"unlisted","owned extra");
    check(status(read(save.Get(),reads))==S_OK && target.Length()==4 && bytes(target)[0]==1,
        "as in the reference, unlisted data files are ignored without modifying them");
    fs::remove(first_revision/L"data"/L"unlisted");
    const auto alias=root/L"owned-reparse-control";
    if(CreateSymbolicLinkW(alias.c_str(),(first_revision/L"data").c_str(),SYMBOLIC_LINK_FLAG_DIRECTORY|SYMBOLIC_LINK_FLAG_ALLOW_UNPRIVILEGED_CREATE)){
        check(!xboxone::era::title_writable_path((alias/L"not-created"/L"blob").wstring()),
            "a reparse ancestor is refused even when later path components do not exist");
        check(RemoveDirectoryW(alias.c_str())!=FALSE,"owned reparse control is removed without following its target");
    }else std::cout<<"reparse control unavailable, Win32 "<<GetLastError()<<'\n';
    Buffer tiny(2);tiny.Length(2);bytes(tiny)[0]=0xcc;reads.Clear();reads.Insert(L"state",tiny);
    check(status(read(save.Get(),reads))==HRESULT(0x80830007) && bytes(tiny)[0]==0xcc && tiny.Length()==2,"short output is rejected without partial copy");
    writes.Insert(L"state",blob({9}));writes.Insert(L"zz/invalid",blob({0}));
    check(status(update(save.Get(),writes))==E_INVALIDARG && contents(file)==original,"failed batch leaves every blob and the previous container unchanged");
    writes.Remove(L"zz/invalid");writes.Insert(L"STATE",blob({7}));
    check(status(update(save.Get(),writes))==E_INVALIDARG && contents(first_revision/L"commit")==marker,
        "names colliding under Windows ordinal case folding cannot partially publish a revision");
    writes.Remove(L"STATE");writes.Insert(winrt::hstring(std::wstring(129,L'x')),blob({7}));
    check(status(update(save.Get(),writes))==E_INVALIDARG,"reference blob names longer than 128 UTF-16 units are refused");
    writes.Remove(winrt::hstring(std::wstring(129,L'x')));
    writes.Insert(L"NUL.txt",blob({7}));
    check(status(update(save.Get(),writes))==E_INVALIDARG,"Windows device aliases cannot become save blob paths");writes.Remove(L"NUL.txt");
    writes.Remove(L"settings"); // This batch deletes it; same-name precedence is tested below.
    auto deletions=winrt::single_threaded_vector<winrt::hstring>({L"settings"});
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
    const auto latest=saved/L"revisions"/L"0000000000000002";write(latest/L"data"/L"state",std::string("\x0a",1));
    check(status(read(save.Get(),reads))==S_OK && target.Length()==4 && bytes(target)[0]==1 && bytes(target)[3]==4,
        "a corrupted newest revision falls back to the last complete, hash-verified revision");
    check(contents(latest/L"data"/L"state")==std::string("\x0a",1),"recovery does not overwrite or remove the damaged revision");
    query_info(first.Get(),L"Sa",1,6);
    const auto pending=saved/L"revisions"/L"0000000000000003";fs::create_directories(pending/L"data");write(pending/L"pending","interrupted publication");
    check(status(read(save.Get(),reads))==S_OK && target.Length()==4 && bytes(target)[0]==1,
        "a newer pending-only revision cannot supersede the last verified commit");
    check(status(update(save.Get(),writes))==S_OK && fs::exists(saved/L"revisions"/L"0000000000000004"/L"commit"),
        "recovery publishes a new increasing revision without reusing a damaged or interrupted one");
    check(status(read(save.Get(),reads))==S_OK && target.Length()==1 && bytes(target)[0]==9,"new reads select the recovered publication");
    query_info(first.Get(),L"Sa",1,3);
    // Exercise old xone formats separately; the reference-format writer must
    // preserve these original bytes while moving subsequent writes to revisions.
    auto legacy_space=space(statics.Get(),user.Get(),L"legacy-test-scid");auto legacy_save=container(legacy_space.Get(),L"Saves");
    const auto legacy_file=root/L"connected"/(digest(L"legacy-test-scid")+L"-"+digest(L"Saves")+L".bin");
    constexpr char legacy[]="XONECS01\x05\0\0\0S\0a\0v\0e\0s\0\x01\0\0\0\x05\0\0\0s\0t\0a\0t\0e\0\x01\0\0\0\x09";
    write(legacy_file,{legacy,sizeof legacy-1});
    check(status(read(legacy_save.Get(),reads))==S_OK && target.Length()==1 && bytes(target)[0]==9 && contents(legacy_file)==std::string(legacy,sizeof legacy-1),
        "existing v1 saves remain readable without an implicit rewrite");
    std::string duplicate(legacy,sizeof legacy-1);duplicate[22]=2;duplicate.append(legacy+26,sizeof legacy-1-26);
    write(legacy_file,duplicate);
    check(status(read(legacy_save.Get(),reads))==HRESULT_FROM_WIN32(ERROR_INVALID_DATA) && contents(legacy_file)==duplicate,
        "duplicate committed blob names are refused and preserved as in the reference");
    std::string hashed(legacy,sizeof legacy-1);hashed[7]='2';hashed.insert(44,"2b4c342f5433ebe591a1da77e013d1b72475562d48578dca8b84bac6651c3cb9");
    write(legacy_file,hashed);
    check(status(read(legacy_save.Get(),reads))==S_OK && target.Length()==1 && bytes(target)[0]==9,"existing v2 binary containers remain readable");
    writes.Clear();
    check(status(update(legacy_save.Get(),writes))==S_OK && contents(legacy_file)==hashed && status(read(legacy_save.Get(),reads))==S_OK && bytes(target)[0]==9,
        "legacy migration commits reference-format revisions without modifying the old container");
    query_info(legacy_space.Get(),L"Sa",1,1);
    check(status(erase(legacy_space.Get(),L"Saves"))==S_OK && status(read(legacy_save.Get(),reads))==HRESULT(0x80830008),
        "deleting a healthy container removes its revisions and legacy fallback without resurrection");
    query_info(legacy_space.Get(),L"",0);
    auto retained_space=space(statics.Get(),user.Get(),L"retained-test-scid");auto retained_save=container(retained_space.Get(),L"Saves");
    auto bounded=winrt::single_threaded_map<winrt::hstring,IBuffer>();
    const winrt::hstring unicode_name=L"perfil-\u00e7-\u65e5";
    const winrt::hstring boundary_name(std::wstring(128,L'x'));bounded.Insert(boundary_name,blob({}));
    for(unsigned i=0;i<4;++i){bounded.Insert(unicode_name,blob({std::uint8_t(i)}));bounded.Insert(L"empty",blob({}));
        check(status(update(retained_save.Get(),bounded))==S_OK,"sequential Unicode/empty-blob submissions commit");}
    const auto retained_root=root/L"connected"/(digest(L"retained-test-scid")+L"-"+digest(L"Saves")+L".save");
    unsigned retained_count=0;for(const auto& entry:fs::directory_iterator(retained_root/L"revisions"))++retained_count;
    check(retained_count==2 && fs::exists(retained_root/L"revisions"/L"0000000000000003"/L"commit") && fs::exists(retained_root/L"revisions"/L"0000000000000004"/L"commit"),
        "normal repeated saves retain two verified revisions instead of accumulating full snapshots");
    auto unicode_reads=winrt::single_threaded_map<winrt::hstring,IBuffer>();Buffer unicode_target(1);unicode_reads.Insert(unicode_name,unicode_target);Buffer empty(0u);unicode_reads.Insert(L"empty",empty);Buffer boundary(0u);unicode_reads.Insert(boundary_name,boundary);
    check(status(read(retained_save.Get(),unicode_reads))==S_OK && unicode_target.Length()==1 && bytes(unicode_target)[0]==3 && empty.Length()==0,
        "Unicode, 128-unit long-path names and empty blobs roundtrip through actual reference-format files");
    auto same_name=winrt::single_threaded_vector<winrt::hstring>({L"state"});bounded.Insert(L"state",blob({9}));
    check(status(update(retained_save.Get(),bounded,same_name))==S_OK,"an updated name can also appear in the deletion batch");
    check(status(read(retained_save.Get(),reads))==S_OK && target.Length()==1 && bytes(target)[0]==9,
        "the reference applies deletions before updates of the same blob name");
    check(status(erase(retained_space.Get(),L"Saves"))==S_OK,"owned retention fixture is deleted through the real storage API");
    auto second=root/L"second";fs::create_directory(second);xboxone::era::set_title_storage_root(second.wstring());
    ComPtr<IInspectable> second_user;winrt::check_hresult(xboxone::era::title_local_user(reinterpret_cast<void**>(second_user.GetAddressOf())));
    invalid=reinterpret_cast<void*>(1);
    check(method<ForUser>(statics.Get(),7)(statics.Get(),user.Get(),abi(L"owned-test-scid"),&invalid)==E_INVALIDARG && !invalid,"a retained user from another session cannot bind the current storage");
    check(status(read(save.Get(),reads))==E_ACCESSDENIED,"stale storage cannot be redirected to the next profile");
    xboxone::era::set_title_storage_root(root.wstring());
    const auto fourth=saved/L"revisions"/L"0000000000000004";write(fourth/L"commit","invalid");write(first_revision/L"commit","invalid");
    check(status(update(save.Get(),writes))==HRESULT_FROM_WIN32(ERROR_INVALID_DATA) && contents(fourth/L"commit")=="invalid",
        "if every complete revision is damaged, updates fail without replacing the damaged data");
    check(status(erase(first.Get(),L"Saves"))==S_OK && !fs::exists(saved),"deleting an unverifiable container archives it instead of destroying it");
    fs::path archive;for(const auto& entry:fs::directory_iterator(root/L"connected"))if(entry.path().filename().wstring().starts_with(L"damaged-saves-"))archive=entry.path();
    check(!archive.empty() && contents(archive/L"container"/L"revisions"/L"0000000000000004"/L"commit")=="invalid",
        "the archived damaged container retains the actual failed commit for recovery");
    xboxone::era::set_title_storage_root(L"");
    std::cout<<"storage failures "<<failures<<'\n';return failures?1:0;
}catch(const winrt::hresult_error& error){std::cerr<<"storage HRESULT "<<std::hex<<unsigned(error.code().value)<<'\n';return 1;}
