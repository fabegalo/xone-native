// SPDX-License-Identifier: BSD-3-Clause
// Native diagnostic front-end over the existing session log, independent of the GPU.
#include "diagnostic_console.h"
#include "file_path.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <array>
#include <cwctype>
#include <cstring>
#include <vector>

namespace xboxone::diagnostics {
namespace {
constexpr wchar_t window_class[]=L"xone-native-diagnostic-console";
enum { search_id=101, category_id, pause_id, follow_id, clear_id, copy_id, command_id, run_id };
std::wstring wide(std::string_view text) {
    if(text.empty())return {};
    const int count=MultiByteToWideChar(CP_UTF8,0,text.data(),int(text.size()),nullptr,0);
    std::wstring result(count,L'\0');MultiByteToWideChar(CP_UTF8,0,text.data(),int(text.size()),result.data(),count);return result;
}
std::wstring text(HWND control) {
    std::wstring value(GetWindowTextLengthW(control)+1,L'\0');
    value.resize(GetWindowTextW(control,value.data(),int(value.size())));return value;
}
std::wstring lower(std::wstring value) {for(auto& c:value)c=wchar_t(std::towlower(c));return value;}
void copy_text(HWND owner,const std::wstring& value) {
    if(!OpenClipboard(owner))return;
    HGLOBAL memory=GlobalAlloc(GMEM_MOVEABLE,(value.size()+1)*sizeof(wchar_t));
    if(memory) {
        if(void* target=GlobalLock(memory)) {
            memcpy(target,value.c_str(),(value.size()+1)*sizeof(wchar_t));GlobalUnlock(memory);
            if(EmptyClipboard() && SetClipboardData(CF_UNICODETEXT,memory))memory=nullptr;
        }
        if(memory)GlobalFree(memory);
    }
    CloseClipboard();
}
}
struct Console::Impl {
    HWND window=nullptr,search=nullptr,category=nullptr,pause=nullptr,follow=nullptr,output=nullptr,command=nullptr,status=nullptr;
    HFONT font=nullptr;
    WNDPROC edit_proc=nullptr;
    std::filesystem::path source;
    std::string session_marker;
    LogBuffer buffer;
    std::uint64_t offset=0;
    BY_HANDLE_FILE_INFORMATION identity{};
    bool paused=false,dirty=true;
    std::vector<std::wstring> history;
    std::size_t history_at=0;
    ~Impl(){if(window)DestroyWindow(window);if(font)DeleteObject(font);}
    HWND control(const wchar_t* kind,const wchar_t* label,DWORD style,int id) {
        HWND child=CreateWindowExW(0,kind,label,WS_CHILD|WS_VISIBLE|WS_TABSTOP|style,0,0,0,0,window,
            reinterpret_cast<HMENU>(INT_PTR(id)),GetModuleHandleW(nullptr),nullptr);
        SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);return child;
    }
    void layout() {
        RECT r{};GetClientRect(window,&r);const int width=r.right,height=r.bottom;
        MoveWindow(GetDlgItem(window,201),10,14,65,24,TRUE);
        MoveWindow(search,80,10,std::max(100,width-710),26,TRUE);
        MoveWindow(category,std::max(120,width-620),10,180,260,TRUE);
        MoveWindow(pause,width-430,10,90,26,TRUE);MoveWindow(follow,width-330,10,100,26,TRUE);
        MoveWindow(GetDlgItem(window,clear_id),width-220,10,95,26,TRUE);MoveWindow(GetDlgItem(window,copy_id),width-115,10,105,26,TRUE);
        MoveWindow(output,10,46,std::max(100,width-20),std::max(60,height-124),TRUE);
        MoveWindow(GetDlgItem(window,202),10,height-64,65,24,TRUE);
        MoveWindow(command,80,height-68,width-180,28,TRUE);MoveWindow(GetDlgItem(window,run_id),width-90,height-68,80,28,TRUE);
        MoveWindow(status,10,height-32,width-20,24,TRUE);
    }
    bool matches(const std::wstring& line,const std::wstring& needle,int group) {
        const auto value=lower(line);
        if(value.find(needle)==std::wstring::npos)return false;
        static constexpr std::array<std::wstring_view,5> gpu{L"draw",L"shader",L"texture",L"d3d",L"gpu"};
        const auto any=[&](auto tokens){return std::any_of(tokens.begin(),tokens.end(),[&](auto token){return value.find(token)!=std::wstring::npos;});};
        switch(group) {
            case 1:return any(std::array{L"failed",L"error",L"exception",L"not implemented",L"missing"});
            case 2:return any(std::array{L"warning",L"refused",L"unverified",L"unsupported"});
            case 3:return any(gpu);
            case 4:return any(std::array{L"audio",L"acp",L"shape",L"wasapi",L"xaudio"});
            case 5:return any(std::array{L"input",L"controller",L"gamepad"});
            case 6:return any(std::array{L"winrt",L"activation",L"interface"});
            case 7:return any(std::array{L"memory",L"alloc",L"esram",L"mapped"});
            default:return true;
        }
    }
    void refresh() {
        if(!dirty)return;
        dirty=false;std::wstring visible;
        const auto needle=lower(text(search));const int group=int(SendMessageW(category,CB_GETCURSEL,0,0));
        std::size_t shown=0;
        for(const auto& raw:buffer.lines) {
            auto line=wide(raw);if(!matches(line,needle,group))continue;
            visible+=line;visible+=L"\r\n";++shown;
        }
        const auto position=GetScrollPos(output,SB_VERT);
        DWORD first=0,last=0;SendMessageW(output,EM_GETSEL,reinterpret_cast<WPARAM>(&first),reinterpret_cast<LPARAM>(&last));
        SetWindowTextW(output,visible.c_str());
        if(SendMessageW(follow,BM_GETCHECK,0,0)==BST_CHECKED) {
            const auto end=GetWindowTextLengthW(output);SendMessageW(output,EM_SETSEL,end,end);SendMessageW(output,EM_SCROLLCARET,0,0);
        } else {
            SendMessageW(output,EM_SETSEL,first,last);SendMessageW(output,EM_LINESCROLL,0,position);
        }
        const auto message=(paused ? L"Paused | " : L"Live | ")+std::to_wstring(shown)+L" visible / "+std::to_wstring(buffer.lines.size())+L" retained; "+
            std::to_wstring(buffer.omitted)+L" older lines omitted | Type help for commands | "+source.wstring();
        SetWindowTextW(status,message.c_str());
    }
    void poll() {
        if(source.empty() || paused)return;
        HANDLE file=CreateFileW(detail::file_path(source).c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
            nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(file==INVALID_HANDLE_VALUE) {
            SetWindowTextW(status,(L"Waiting for session output: "+source.wstring()).c_str());return;
        }
        BY_HANDLE_FILE_INFORMATION info{};LARGE_INTEGER size{};
        if(GetFileInformationByHandle(file,&info) && GetFileSizeEx(file,&size)) {
            std::array<char,128> first{};DWORD first_count=0;
            const bool has_marker=ReadFile(file,first.data(),DWORD(first.size()),&first_count,nullptr) &&
                std::string_view(first.data(),first_count).starts_with("xone-native: session ");
            const std::string marker=has_marker ? std::string(first.data(),first_count).substr(0,std::string_view(first.data(),first_count).find('\n')) : std::string{};
            if(info.dwVolumeSerialNumber!=identity.dwVolumeSerialNumber || info.nFileIndexHigh!=identity.nFileIndexHigh ||
                info.nFileIndexLow!=identity.nFileIndexLow || std::uint64_t(size.QuadPart)<offset || marker!=session_marker) {
                buffer.clear();offset=0;dirty=true;identity=info;session_marker=marker;
            }
            if(std::uint64_t(size.QuadPart)>offset+LogBuffer::byte_limit) {
                offset=std::uint64_t(size.QuadPart)-LogBuffer::byte_limit;
                buffer.clear();buffer.append("[console] Earlier file bytes omitted: showing the bounded tail.\n");dirty=true;
            }
            LARGE_INTEGER at{};at.QuadPart=offset;
            if(SetFilePointerEx(file,at,nullptr,FILE_BEGIN)) {
                std::array<char,65536> bytes{};DWORD count=0;
                if(ReadFile(file,bytes.data(),DWORD(bytes.size()),&count,nullptr) && count) {
                    buffer.append(std::string_view(bytes.data(),count));offset+=count;dirty=true;
                }
            }
        }
        CloseHandle(file);refresh();
    }
    void execute() {
        const auto input=text(command);if(input.empty())return;
        if(history.empty() || history.back()!=input)history.push_back(input);
        if(history.size()>100)history.erase(history.begin());history_at=history.size();
        const auto value=lower(input);
        if(value==L"clear") {buffer.clear();dirty=true;}
        else if(value==L"pause" || value==L"resume") {paused=value==L"pause";SetWindowTextW(pause,paused ? L"Resume" : L"Pause");dirty=true;}
        else if(value.starts_with(L"filter ")) {SetWindowTextW(search,input.substr(7).c_str());dirty=true;}
        else if(value==L"filter") {SetWindowTextW(search,L"");dirty=true;}
        else if(value==L"follow on" || value==L"follow off")SendMessageW(follow,BM_SETCHECK,value==L"follow on" ? BST_CHECKED : BST_UNCHECKED,0);
        else if(value==L"copy")copy_text(window,text(output));
        else {
            buffer.append(value==L"help" ? "[console] help | clear | pause | resume | filter TEXT | filter | follow on/off | copy\n" :
                "[console] Unknown command. Type help. Commands affect the diagnostic view only.\n");dirty=true;
        }
        SetWindowTextW(command,L"");refresh();
    }
    static LRESULT CALLBACK edit(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) {
        auto* self=reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
        if(msg==WM_KEYDOWN) {
            if(wp==VK_RETURN){self->execute();return 0;}
            if(wp==VK_ESCAPE || wp==VK_F8){ShowWindow(self->window,SW_HIDE);return 0;}
            if(wp==VK_UP || wp==VK_DOWN) {
                if(wp==VK_UP && self->history_at) --self->history_at;
                if(wp==VK_DOWN && self->history_at<self->history.size())++self->history_at;
                SetWindowTextW(hwnd,self->history_at<self->history.size() ? self->history[self->history_at].c_str() : L"");
                SendMessageW(hwnd,EM_SETSEL,WPARAM(-1),LPARAM(-1));return 0;
            }
            if(wp==VK_TAB) {
                const auto prefix=lower(text(hwnd));
                for(auto candidate:{L"help",L"clear",L"pause",L"resume",L"filter ",L"follow on",L"follow off",L"copy"})
                    if(std::wstring_view(candidate).starts_with(prefix)){SetWindowTextW(hwnd,candidate);SendMessageW(hwnd,EM_SETSEL,WPARAM(-1),LPARAM(-1));break;}
                return 0;
            }
        }
        if(msg==WM_CHAR && (wp==L'\r' || wp==L'\t' || wp==27))return 0;
        return CallWindowProcW(self->edit_proc,hwnd,msg,wp,lp);
    }
    static LRESULT CALLBACK procedure(HWND hwnd,UINT msg,WPARAM wp,LPARAM lp) {
        auto* self=reinterpret_cast<Impl*>(GetWindowLongPtrW(hwnd,GWLP_USERDATA));
        if(msg==WM_NCCREATE) {
            self=static_cast<Impl*>(reinterpret_cast<CREATESTRUCTW*>(lp)->lpCreateParams);
            self->window=hwnd;SetWindowLongPtrW(hwnd,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));
        }
        if(!self)return DefWindowProcW(hwnd,msg,wp,lp);
        switch(msg) {
            case WM_CREATE: {
                self->font=CreateFontW(-16,0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,0,0,CLEARTYPE_QUALITY,FIXED_PITCH,L"Consolas");
                self->control(L"STATIC",L"Search",SS_LEFT,201);
                self->search=self->control(L"EDIT",L"",WS_BORDER|ES_AUTOHSCROLL,search_id);
                SendMessageW(self->search,0x1501,TRUE,reinterpret_cast<LPARAM>(L"Search log text"));
                self->category=self->control(L"COMBOBOX",L"",CBS_DROPDOWNLIST|WS_VSCROLL,category_id);
                for(auto label:{L"All output",L"Errors (text)",L"Warnings (text)",L"GPU",L"Audio",L"Input",L"WinRT",L"Memory"})
                    SendMessageW(self->category,CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label));
                SendMessageW(self->category,CB_SETCURSEL,0,0);
                self->pause=self->control(L"BUTTON",L"Pause",BS_PUSHBUTTON,pause_id);
                self->follow=self->control(L"BUTTON",L"Follow",BS_AUTOCHECKBOX,follow_id);SendMessageW(self->follow,BM_SETCHECK,BST_CHECKED,0);
                self->control(L"BUTTON",L"Clear view",BS_PUSHBUTTON,clear_id);self->control(L"BUTTON",L"Copy view",BS_PUSHBUTTON,copy_id);
                self->output=self->control(L"EDIT",L"",WS_BORDER|WS_VSCROLL|WS_HSCROLL|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL|ES_AUTOHSCROLL,0);
                SendMessageW(self->output,EM_SETLIMITTEXT,4*1024*1024,0);
                self->control(L"STATIC",L"Command",SS_LEFT,202);
                self->command=self->control(L"EDIT",L"",WS_BORDER|ES_AUTOHSCROLL,command_id);
                SetWindowLongPtrW(self->command,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));
                self->edit_proc=reinterpret_cast<WNDPROC>(SetWindowLongPtrW(self->command,GWLP_WNDPROC,reinterpret_cast<LONG_PTR>(&edit)));
                self->control(L"BUTTON",L"Run",BS_DEFPUSHBUTTON,run_id);self->status=self->control(L"STATIC",L"",SS_LEFTNOWORDWRAP|SS_ENDELLIPSIS|SS_NOPREFIX,0);
                SetTimer(hwnd,1,200,nullptr);return 0;
            }
            case WM_SIZE:self->layout();return 0;
            case WM_GETMINMAXINFO:reinterpret_cast<MINMAXINFO*>(lp)->ptMinTrackSize={850,300};return 0;
            case WM_TIMER:if(IsWindowVisible(hwnd))self->poll();return 0;
            case WM_CTLCOLORSTATIC:
                if(reinterpret_cast<HWND>(lp)==self->output) {
                    SetTextColor(reinterpret_cast<HDC>(wp),RGB(225,225,225));SetBkColor(reinterpret_cast<HDC>(wp),RGB(0,0,0));
                    return reinterpret_cast<LRESULT>(GetStockObject(BLACK_BRUSH));
                }
                break;
            case WM_CLOSE:ShowWindow(hwnd,SW_HIDE);return 0;
            case WM_KEYDOWN:if(wp==VK_F8){ShowWindow(hwnd,SW_HIDE);return 0;}break;
            case WM_COMMAND:
                switch(LOWORD(wp)) {
                    case search_id:if(HIWORD(wp)==EN_CHANGE)self->dirty=true;break;
                    case category_id:if(HIWORD(wp)==CBN_SELCHANGE)self->dirty=true;break;
                    case pause_id:self->paused=!self->paused;SetWindowTextW(self->pause,self->paused ? L"Resume" : L"Pause");self->dirty=true;break;
                    case follow_id:self->dirty=true;break;
                    case clear_id:self->buffer.clear();self->dirty=true;break;
                    case copy_id:copy_text(hwnd,text(self->output));break;
                    case run_id:self->execute();break;
                }
                self->refresh();return 0;
            case WM_NCDESTROY:self->window=nullptr;break;
        }
        return DefWindowProcW(hwnd,msg,wp,lp);
    }
};
Console::Console():impl_(std::make_unique<Impl>()){}
Console::~Console()=default;
bool Console::show(const std::filesystem::path& source,std::uintptr_t owner) {
    if(impl_->source!=source){impl_->source=source;impl_->offset=0;impl_->identity={};impl_->session_marker.clear();impl_->buffer.clear();impl_->dirty=true;}
    if(!impl_->window) {
        WNDCLASSW cls{};cls.lpfnWndProc=&Impl::procedure;cls.hInstance=GetModuleHandleW(nullptr);
        cls.hCursor=LoadCursorW(nullptr,MAKEINTRESOURCEW(32512));cls.hbrBackground=GetSysColorBrush(COLOR_BTNFACE);cls.lpszClassName=window_class;
        if(!RegisterClassW(&cls) && GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)return false;
        if(!CreateWindowExW(0,window_class,L"xone-native — Diagnostic console (F8)",WS_OVERLAPPEDWINDOW,
            CW_USEDEFAULT,CW_USEDEFAULT,1200,650,reinterpret_cast<HWND>(owner),nullptr,cls.hInstance,impl_.get()))return false;
    }
    ShowWindow(impl_->window,SW_SHOWNORMAL);impl_->dirty=true;impl_->poll();impl_->refresh();
    SetForegroundWindow(impl_->window);SetFocus(impl_->command);return true;
}
void Console::hide(){if(impl_->window)ShowWindow(impl_->window,SW_HIDE);}
int run_console(const std::filesystem::path& source) {
    Console console;if(!console.show(source))return 1;
    MSG message{};
    while(GetMessageW(&message,nullptr,0,0)>0) {
        TranslateMessage(&message);DispatchMessageW(&message);
        if(!console.impl_->window || !IsWindowVisible(console.impl_->window))break;
    }
    return 0;
}
}
