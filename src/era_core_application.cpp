// SPDX-License-Identifier: BSD-3-Clause
// Windows.ApplicationModel.Core.CoreApplication served by the title layer.
// The host's CoreApplication::Run only works in a packaged, system-activated
// process; the launcher is neither, so the view lifecycle is driven here, in
// the order of the Windows contract: CreateView, Initialize, SetWindow, Load,
// Activated, Run, Uninitialize. The window is a real Win32 window.
//
// Interface layouts are the public Windows SDK ones. Only slots the title has
// been observed to call are filled; every other slot traps.
// Observed (Sonic Mania 1.0.4.2): ICoreApplication 13 Run, 7 add_Suspending;
// ICoreApplicationResourceAvailability 7 add_ResourceAvailabilityChanged;
// ICoreWindow (Xbox layout) 19 add_Closed;
// ICoreApplicationView 7 add_Activated.
#include "era_winrt_classes.h"
#include "era_winrt_object.h"

#include <activation.h>
#include <winstring.h>

#include <cstdio>
#include <atomic>

namespace xboxone::era {
namespace {

constexpr wchar_t class_name[] = L"Windows.ApplicationModel.Core.CoreApplication";
constexpr wchar_t view_class[] = L"Windows.ApplicationModel.Core.CoreApplicationView";
constexpr wchar_t window_class[] = L"Windows.UI.Core.CoreWindow";
constexpr wchar_t args_class[] = L"Windows.ApplicationModel.Activation.LaunchActivatedEventArgs";

constexpr GUID iid_core_application{0x0aacf7a4, 0x5e1d, 0x49df, {0x80, 0x34, 0xfb, 0x6a, 0x68, 0xbc, 0x5e, 0xd1}};
// Xbox extension of CoreApplication: 6 get_ResourceAvailability,
// 7/8 ResourceAvailabilityChanged (layout per WinDurango's public headers).
constexpr GUID iid_resource_availability{0x9ff3799e, 0x6e83, 0x468c, {0xa5, 0xb0, 0x50, 0x1c, 0xe6, 0xbb, 0x0f, 0xa1}};
constexpr GUID iid_view{0x638bb2db, 0x451d, 0x4661, {0xb0, 0x99, 0x41, 0x4f, 0x34, 0xff, 0xb9, 0xf1}};
constexpr GUID iid_view_source{0xcd770614, 0x65c4, 0x426c, {0x94, 0x94, 0x34, 0xfc, 0x43, 0x55, 0x48, 0x62}};
constexpr GUID iid_framework_view{0xfaab5cd0, 0x8924, 0x45ac, {0xad, 0x0f, 0xa0, 0x8f, 0xae, 0x5d, 0x03, 0x24}};
constexpr GUID iid_core_window{0x79b9d5f2, 0x879e, 0x4b89, {0xb7, 0x98, 0x79, 0xe4, 0x75, 0x98, 0x03, 0x0c}};
constexpr GUID iid_activated_args{0xcf651713, 0xcd08, 0x4fd8, {0xb6, 0x97, 0xa2, 0x81, 0xb6, 0x54, 0x4e, 0x2e}};

// Window client size: the 1080p output Xbox One titles render for.
constexpr int window_width = 1920;
constexpr int window_height = 1080;

void note(const char* text) {
    std::fputs(text, stderr);
    std::fflush(stderr);
}

template <typename R, typename... A>
R call(void* object, unsigned slot, A... args) {
    auto** vt = *static_cast<void***>(object);
    return reinterpret_cast<R(STDMETHODCALLTYPE*)(void*, A...)>(vt[slot])(object, args...);
}

// --- CoreApplicationView -----------------------------------------------------
WinrtEvent view_activated;

HRESULT self_query(WinrtObject* self, REFIID iid, void** out) {
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IInspectable)) { *out = self; return S_OK; }
    return E_NOINTERFACE;
}

HRESULT STDMETHODCALLTYPE add_activated(WinrtObject*, IUnknown* h, std::int64_t* t) { return view_activated.add(h, t); }
HRESULT STDMETHODCALLTYPE remove_activated(WinrtObject*, std::int64_t t) { return view_activated.remove(t); }

WinrtObject& view() {
    static const auto vt = [] {
        auto t = winrt_vtable();
        t[7] = reinterpret_cast<const void*>(&add_activated);
        t[8] = reinterpret_cast<const void*>(&remove_activated);
        return t;
    }();
    static WinrtObject object{vt.data(), view_class, iid_view, &self_query};
    return object;
}

// --- CoreWindow over a Win32 window -------------------------------------------
// The Xbox ICoreWindow keeps the desktop IID but has its own, shorter layout
// (per WinDurango's public headers; slot 19 add_Closed matches what SetWindow
// registers): 6 get_Bounds, 7 get_Dispatcher, 8/9 IsInputEnabled, 10 get_Visible,
// 11 Activate, 12 Close, 13 GetAsyncKeyState, 14 GetKeyState, then add/remove
// pairs from 15: Activated, CharacterReceived, Closed (19/20), InputEnabled,
// KeyDown, KeyUp, PointerMoved, PointerEntered, PointerExited, SizeChanged
// (33/34), VisibilityChanged (35/36).
HWND window = nullptr;
std::atomic<DWORD> window_thread{0};
bool dispatcher_quit = false; // Accessed only by the window thread.
WinrtEvent window_closed;

HRESULT STDMETHODCALLTYPE thread_access(WinrtObject*, boolean* value) {
    if (!value) return E_POINTER;
    *value = window_thread != 0 && GetCurrentThreadId() == window_thread;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE process_events(WinrtObject*, INT32 option) {
    if (option < 0 || option > 3) return E_INVALIDARG;
    if (!window_thread || GetCurrentThreadId() != window_thread) return RPC_E_WRONG_THREAD;
    if (dispatcher_quit) return S_OK;
    MSG message{};
    bool wait = option == 0 || option == 2;
    do {
        const HRESULT input = poll_title_input();
        if (FAILED(input)) return input;
        if (wait) {
            const BOOL result = GetMessageW(&message, nullptr, 0, 0);
            if (result == -1) return HRESULT_FROM_WIN32(GetLastError());
            if (!result) { dispatcher_quit = true; return S_OK; }
        } else if (!PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) return S_OK;
        if (message.message == WM_QUIT) { dispatcher_quit = true; return S_OK; }
        TranslateMessage(&message);
        DispatchMessageW(&message);
        if (option == 1) return S_OK;
        wait = option == 2;
    } while (!dispatcher_quit);
    return S_OK;
}
WinrtObject& dispatcher() {
    static const auto vt = [] {
        auto t = winrt_vtable();
        t[6] = reinterpret_cast<const void*>(&thread_access);
        t[7] = reinterpret_cast<const void*>(&process_events);
        return t;
    }();
    static constexpr GUID iid{0x60db2fa8, 0xb705, 0x4fde, {0xa7, 0xd6, 0xeb, 0xbb, 0x18, 0x91, 0xd3, 0x9e}};
    static WinrtObject object{vt.data(), L"Windows.UI.Core.CoreDispatcher", iid, &self_query};
    return object;
}
HRESULT STDMETHODCALLTYPE get_dispatcher(WinrtObject*, IInspectable** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!window_thread) return RO_E_CLOSED;
    *out = reinterpret_cast<IInspectable*>(&dispatcher());
    (*out)->AddRef();
    return S_OK;
}

HRESULT STDMETHODCALLTYPE add_closed(WinrtObject*, IUnknown* h, std::int64_t* t) { return window_closed.add(h, t); }
HRESULT STDMETHODCALLTYPE remove_closed(WinrtObject*, std::int64_t t) { return window_closed.remove(t); }

HRESULT STDMETHODCALLTYPE activate_window(WinrtObject*) {
    if (!window) return RO_E_CLOSED;
    if (GetCurrentThreadId() != window_thread) return RPC_E_WRONG_THREAD;
    ShowWindow(window, SW_SHOW);
    SetForegroundWindow(window);
    return S_OK;
}

LRESULT CALLBACK window_proc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_DESTROY && window_thread == GetCurrentThreadId()) PostQuitMessage(0);
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

HRESULT create_window() {
    WNDCLASSEXW wc{sizeof wc};
    wc.lpfnWndProc = window_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
    wc.lpszClassName = L"xone-era-core-window";
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return HRESULT_FROM_WIN32(GetLastError());
    RECT r{0, 0, window_width, window_height};
    const DWORD style = WS_OVERLAPPEDWINDOW;
    AdjustWindowRect(&r, style, FALSE);
    window = CreateWindowExW(0, wc.lpszClassName, L"xone-era", style, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left,
                             r.bottom - r.top, nullptr, nullptr, wc.hInstance, nullptr);
    window_thread = window ? GetCurrentThreadId() : 0;
    dispatcher_quit = false;
    return window ? S_OK : HRESULT_FROM_WIN32(GetLastError());
}

WinrtObject& core_window() {
    static const auto vt = [] {
        auto t = winrt_vtable();
        t[7] = reinterpret_cast<const void*>(&get_dispatcher);
        t[11] = reinterpret_cast<const void*>(&activate_window);
        t[19] = reinterpret_cast<const void*>(&add_closed);
        t[20] = reinterpret_cast<const void*>(&remove_closed);
        return t;
    }();
    static WinrtObject object{vt.data(), window_class, iid_core_window, &self_query};
    return object;
}

// --- Launch activation arguments --------------------------------------------
HRESULT STDMETHODCALLTYPE activation_kind(WinrtObject*, INT32* kind) {
    if (!kind) return E_POINTER;
    *kind = 0; // ActivationKind::Launch: this event is dispatched by CoreApplication::Run.
    return S_OK;
}

WinrtObject& launch_args() {
    static const auto vt = [] {
        auto t = winrt_vtable();
        t[6] = reinterpret_cast<const void*>(&activation_kind);
        return t;
    }();
    static WinrtObject object{vt.data(), args_class, iid_activated_args, &self_query};
    return object;
}

// --- ICoreApplication::Run(IFrameworkViewSource*) ------------------------------
HRESULT STDMETHODCALLTYPE run(WinrtObject*, IUnknown* source_unknown) {
    if (!source_unknown) return E_INVALIDARG;
    void* source = nullptr;
    HRESULT hr = source_unknown->QueryInterface(iid_view_source, &source);
    if (FAILED(hr)) return hr;
    IUnknown* framework_view = nullptr;
    hr = call<HRESULT>(source, 6, &framework_view); // CreateView
    static_cast<IUnknown*>(source)->Release();
    if (FAILED(hr)) return hr;
    void* fv = nullptr;
    hr = framework_view->QueryInterface(iid_framework_view, &fv);
    framework_view->Release();
    if (FAILED(hr)) return hr;

    std::wstring entry;
    HSTRING entry_point = nullptr;
    if (SUCCEEDED(hr)) hr = title_entry_point(entry);
    if (SUCCEEDED(hr)) hr = WindowsCreateString(entry.c_str(), static_cast<UINT32>(entry.size()), &entry_point);
    bool initialized = false;
    if (SUCCEEDED(hr)) {
        note("xone-era: view Initialize\n");
        hr = call<HRESULT>(fv, 6, &view());
        initialized = SUCCEEDED(hr);
    }
    if (SUCCEEDED(hr)) hr = create_window();
    if (SUCCEEDED(hr)) { note("xone-era: view SetWindow\n"); hr = call<HRESULT>(fv, 7, &core_window()); }
    if (SUCCEEDED(hr)) { note("xone-era: view Load\n"); hr = call<HRESULT>(fv, 8, entry_point); }
    if (SUCCEEDED(hr)) {
        note("xone-era: view Activated\n");
        for (IUnknown* handler : view_activated.snapshot()) {
            // TypedEventHandler<CoreApplicationView, IActivatedEventArgs>::Invoke
            const HRESULT h = call<HRESULT>(handler, 3, &view(), &launch_args());
            handler->Release();
            if (FAILED(h) && SUCCEEDED(hr)) hr = h;
        }
    }
    if (SUCCEEDED(hr)) { note("xone-era: view Run\n"); hr = call<HRESULT>(fv, 9); }
    if (initialized) { // Uninitialize pairs with a successful Initialize, also after a later failure.
        note("xone-era: view Uninitialize\n");
        const HRESULT un = call<HRESULT>(fv, 10);
        if (SUCCEEDED(hr)) hr = un;
    }
    static_cast<IUnknown*>(fv)->Release();
    stop_title_input();
    window_thread = 0;
    if (window) { DestroyWindow(window); window = nullptr; }
    WindowsDeleteString(entry_point);
    char line[100];
    std::snprintf(line, sizeof line, "xone-era: CoreApplication::Run -> 0x%08lX\n", static_cast<unsigned long>(hr));
    note(line);
    return hr;
}

// The launcher never suspends the title, so these are stored and never raised.
WinrtEvent suspending, resuming;
HRESULT STDMETHODCALLTYPE add_suspending(WinrtObject*, IUnknown* h, std::int64_t* t) { return suspending.add(h, t); }
HRESULT STDMETHODCALLTYPE remove_suspending(WinrtObject*, std::int64_t t) { return suspending.remove(t); }
HRESULT STDMETHODCALLTYPE add_resuming(WinrtObject*, IUnknown* h, std::int64_t* t) { return resuming.add(h, t); }
HRESULT STDMETHODCALLTYPE remove_resuming(WinrtObject*, std::int64_t t) { return resuming.remove(t); }

// --- Factory -------------------------------------------------------------------
HRESULT query(WinrtObject*, REFIID iid, void** out);

WinrtObject& activation() {
    static const auto vt = winrt_vtable();
    static WinrtObject object{vt.data(), class_name, __uuidof(IActivationFactory), &query};
    return object;
}

WinrtObject& statics() {
    static const auto vt = [] {
        auto t = winrt_vtable();
        t[7] = reinterpret_cast<const void*>(&add_suspending);
        t[8] = reinterpret_cast<const void*>(&remove_suspending);
        t[9] = reinterpret_cast<const void*>(&add_resuming);
        t[10] = reinterpret_cast<const void*>(&remove_resuming);
        t[13] = reinterpret_cast<const void*>(&run);
        return t;
    }();
    static WinrtObject object{vt.data(), class_name, iid_core_application, &query};
    return object;
}

// The title always runs in the foreground with full resources here, so the
// availability never changes and handlers are never raised.
WinrtEvent availability_changed;
HRESULT STDMETHODCALLTYPE add_availability(WinrtObject*, IUnknown* h, std::int64_t* t) { return availability_changed.add(h, t); }
HRESULT STDMETHODCALLTYPE remove_availability(WinrtObject*, std::int64_t t) { return availability_changed.remove(t); }

WinrtObject& resource_availability() {
    static const auto vt = [] {
        auto t = winrt_vtable();
        t[7] = reinterpret_cast<const void*>(&add_availability);
        t[8] = reinterpret_cast<const void*>(&remove_availability);
        return t;
    }();
    static WinrtObject object{vt.data(), class_name, iid_resource_availability, &query};
    return object;
}

HRESULT query(WinrtObject*, REFIID iid, void** out) {
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IInspectable) || iid == __uuidof(IActivationFactory))
        *out = &activation();
    else if (iid == iid_core_application)
        *out = &statics();
    else if (iid == iid_resource_availability)
        *out = &resource_availability();
    else
        return E_NOINTERFACE;
    return S_OK;
}

} // namespace

HWND core_window_hwnd(const void* object) { return object == &core_window() ? window : nullptr; }

static HRESULT window_factory_query(WinrtObject*, REFIID iid, void** out) { return core_window_factory(iid,out); }
static HRESULT STDMETHODCALLTYPE current_window(WinrtObject*, IInspectable** out) {
    if (!out) return E_POINTER;
    *out = window && GetCurrentThreadId() == window_thread ? reinterpret_cast<IInspectable*>(&core_window()) : nullptr;
    if (*out) (*out)->AddRef();
    return S_OK;
}
HRESULT core_window_factory(REFIID iid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    constexpr GUID iid_static{0x4d239005,0x3c2a,0x41b1,{0x90,0x22,0x53,0x6b,0xb9,0xcf,0x93,0xb1}};
    static const auto activation_table = winrt_vtable();
    static const auto static_table = [] {
        auto t = winrt_vtable(); t[6] = reinterpret_cast<const void*>(&current_window); return t;
    }();
    static WinrtObject activation{activation_table.data(), window_class, __uuidof(IActivationFactory), &window_factory_query};
    static WinrtObject statics{static_table.data(), window_class, iid_static, &window_factory_query};
    if (iid == iid_static) *out = &statics;
    else if (iid == __uuidof(IUnknown) || iid == __uuidof(IInspectable) || iid == __uuidof(IActivationFactory)) *out = &activation;
    else return E_NOINTERFACE;
    return S_OK;
}

HRESULT core_application_factory(REFIID iid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    return query(nullptr, iid, out);
}

} // namespace xboxone::era
