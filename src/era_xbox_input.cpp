// SPDX-License-Identifier: BSD-3-Clause
#include "era_winrt_classes.h"
#include "era_winrt_object.h"
#include <activation.h>
#include <xinput.h>
#include <winstring.h>
#include <atomic>
#include <memory>
#include <new>

namespace xboxone::era {
namespace {
constexpr wchar_t controller_class[] = L"Windows.Xbox.Input.Controller";
constexpr GUID iid_statics{0x7400624f,0x144b,0x47fc,{0x9f,0x3b,0x1d,0xca,0xfb,0x48,0x28,0x73}};
constexpr GUID iid_controller{0x9776ff56,0x9bfd,0x4581,{0xad,0x45,0xb6,0x45,0xbb,0xa5,0x26,0xd6}};
WinrtEvent added, removed;
WinrtEvent gamepad_added, gamepad_removed;
WinrtEvent pairing_changed;
constexpr GUID iid_gamepad_statics{0x8c4c1bcd,0xfdab,0x4bf8,{0xbd,0xe9,0xab,0xb2,0xb1,0xd6,0xfd,0xd3}};
constexpr GUID iid_gamepad{0x082e402c,0x07df,0x45e1,{0xa5,0xab,0xa3,0x12,0x7a,0xf1,0x97,0xb5}};
constexpr wchar_t gamepad_class[] = L"Windows.Xbox.Input.Gamepad";
std::mutex input_mutex;
std::atomic<DWORD> event_thread{0};
UINT_PTR input_timer = 0;
bool initialized = false;
std::array<bool, XUSER_MAX_COUNT> connected{};

struct HostInput {
    HMODULE module = LoadLibraryExW(L"xinput1_4.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    decltype(&XInputGetState) get = module ? reinterpret_cast<decltype(get)>(GetProcAddress(module, "XInputGetState")) : nullptr;
    ~HostInput() { if (module) FreeLibrary(module); }
};
HostInput& host() { static HostInput input; return input; }

template<class T> ULONG STDMETHODCALLTYPE retain(T* self) { return ++self->refs; }
template<class T> ULONG STDMETHODCALLTYPE release(T* self) {
    const ULONG refs = --self->refs;
    if (!refs) delete self;
    return refs;
}
template<class T> HRESULT STDMETHODCALLTYPE dynamic_query(T* self, REFIID iid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    return self->query(self, iid, out);
}
struct Controller;
struct Gamepad : WinrtObject { Controller* owner = nullptr; };
struct Controller : WinrtObject { std::atomic<ULONG> refs{1}; DWORD index = 0; Gamepad gamepad; };
std::array<Controller*, XUSER_MAX_COUNT> controller_objects{};
HRESULT controller_query(WinrtObject* self, REFIID iid, void** out) {
    if (iid == iid_gamepad) {
        auto* controller = static_cast<Controller*>(self);
        *out = &controller->gamepad; retain(controller); return S_OK;
    }
    if (iid != __uuidof(IUnknown) && iid != __uuidof(IInspectable) && iid != iid_controller) return E_NOINTERFACE;
    *out = self; retain(static_cast<Controller*>(self)); return S_OK;
}
HRESULT gamepad_query(WinrtObject* self, REFIID iid, void** out) {
    return controller_query(static_cast<Gamepad*>(self)->owner, iid, out);
}
ULONG STDMETHODCALLTYPE gamepad_retain(Gamepad* self) { return retain(self->owner); }
ULONG STDMETHODCALLTYPE gamepad_release(Gamepad* self) { return release(self->owner); }
HRESULT STDMETHODCALLTYPE controller_id(Controller* self, UINT64* out) {
    if (!out) return E_POINTER;
    *out = UINT64(self->index) + 1; return S_OK;
}
HRESULT STDMETHODCALLTYPE controller_type(Controller*, HSTRING* out) {
    if (!out) return E_POINTER;
    return WindowsCreateString(L"Windows.Xbox.Input.Gamepad", 26, out);
}
Controller* make_controller(DWORD index) {
    static const auto table = [] {
        auto t = winrt_vtable();
        t[0] = reinterpret_cast<const void*>(&dynamic_query<Controller>);
        t[1] = reinterpret_cast<const void*>(&retain<Controller>); t[2] = reinterpret_cast<const void*>(&release<Controller>);
        t[6] = reinterpret_cast<const void*>(&controller_id); t[7] = reinterpret_cast<const void*>(&controller_type);
        return t;
    }();
    auto* object = new (std::nothrow) Controller;
    if (object) {
        static_cast<WinrtObject&>(*object) = {table.data(), controller_class, iid_controller, &controller_query};
        object->index = index;
        static const auto pad_table = [] {
            auto t = winrt_vtable();
            t[0] = reinterpret_cast<const void*>(&dynamic_query<Gamepad>);
            t[1] = reinterpret_cast<const void*>(&gamepad_retain); t[2] = reinterpret_cast<const void*>(&gamepad_release);
            return t;
        }();
        static_cast<WinrtObject&>(object->gamepad) = {pad_table.data(), gamepad_class, iid_gamepad, &gamepad_query};
        object->gamepad.owner = object;
    }
    return object;
}

struct ControllerView : WinrtObject {
    std::atomic<ULONG> refs{1};
    std::vector<IUnknown*> items;
    ~ControllerView() { for (auto* item : items) item->Release(); }
};
GUID view_iid() {
    static const GUID iid = winrt_pinterface_iid("pinterface({bbe1fa4c-b0e3-4583-baef-1f1b2e483e56};{9776ff56-9bfd-4581-ad45-b645bba526d6})");
    return iid;
}
HRESULT view_query(WinrtObject* self, REFIID iid, void** out) {
    if (iid != __uuidof(IUnknown) && iid != __uuidof(IInspectable) && iid != self->iid) return E_NOINTERFACE;
    *out = self; retain(static_cast<ControllerView*>(self)); return S_OK;
}
HRESULT STDMETHODCALLTYPE view_get(ControllerView* self, UINT index, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (index >= self->items.size()) return E_BOUNDS;
    self->items[index]->AddRef(); *out = self->items[index]; return S_OK;
}
HRESULT STDMETHODCALLTYPE view_size(ControllerView* self, UINT* out) {
    if (!out) return E_POINTER;
    *out = static_cast<UINT>(self->items.size()); return S_OK;
}
HRESULT STDMETHODCALLTYPE view_index(ControllerView* self, void* item, UINT* index, boolean* found) {
    if (!index || !found) return E_POINTER;
    *index = 0; *found = false;
    for (UINT i = 0; i < self->items.size(); ++i) if (self->items[i] == item) { *index = i; *found = true; break; }
    return S_OK;
}
HRESULT STDMETHODCALLTYPE view_many(ControllerView* self, UINT start, UINT capacity, void** items, UINT* actual) {
    if (!actual) return E_POINTER;
    *actual = 0;
    if (start > self->items.size()) return E_BOUNDS;
    if (capacity && !items) return E_POINTER;
    for (; *actual < capacity && start + *actual < self->items.size(); ++*actual) {
        auto* item = self->items[start + *actual]; item->AddRef(); items[*actual] = item;
    }
    return S_OK;
}
HRESULT STDMETHODCALLTYPE controllers(WinrtObject* self, void** out) try {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (!host().get) return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    static const auto table = [] {
        auto t = winrt_vtable();
        t[0] = reinterpret_cast<const void*>(&dynamic_query<ControllerView>);
        t[1] = reinterpret_cast<const void*>(&retain<ControllerView>); t[2] = reinterpret_cast<const void*>(&release<ControllerView>);
        t[6] = reinterpret_cast<const void*>(&view_get); t[7] = reinterpret_cast<const void*>(&view_size);
        t[8] = reinterpret_cast<const void*>(&view_index); t[9] = reinterpret_cast<const void*>(&view_many);
        return t;
    }();
    const bool pads = self->iid == iid_gamepad_statics;
    auto view = std::make_unique<ControllerView>();
    const GUID iid = pads ? winrt_pinterface_iid("pinterface({bbe1fa4c-b0e3-4583-baef-1f1b2e483e56};{082e402c-07df-45e1-a5ab-a3127af197b5})") : view_iid();
    static_cast<WinrtObject&>(*view) = {table.data(), pads ? gamepad_class : controller_class, iid, &view_query};
    std::lock_guard lock(input_mutex);
    for (DWORD index = 0; index < XUSER_MAX_COUNT; ++index) {
        XINPUT_STATE state{};
        const DWORD error = host().get(index, &state);
        if (error == ERROR_DEVICE_NOT_CONNECTED) continue;
        if (error) return HRESULT_FROM_WIN32(error);
        auto*& controller = controller_objects[index];
        if (!controller) controller = make_controller(index);
        if (!controller) return E_OUTOFMEMORY;
        auto* item = reinterpret_cast<IUnknown*>(pads ? static_cast<WinrtObject*>(&controller->gamepad) : static_cast<WinrtObject*>(controller));
        view->items.push_back(item); item->AddRef();
    }
    *out = view.release(); return S_OK;
} catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
HRESULT register_handler(WinrtEvent& event, IUnknown* handler, INT64* token) {
    if (!handler || !token) return E_INVALIDARG;
    if (!host().get) return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    DWORD previous = 0;
    const bool first = event_thread.compare_exchange_strong(previous, GetCurrentThreadId());
    if (!first && previous != GetCurrentThreadId()) return RPC_E_WRONG_THREAD;
    if (first) {
        const HRESULT hr = poll_title_input();
        if (FAILED(hr)) { event_thread = 0; return hr; }
        input_timer = SetTimer(nullptr, 0, 32, nullptr);
        if (!input_timer) { event_thread = 0; return HRESULT_FROM_WIN32(GetLastError()); }
    }
    return event.add(handler, token);
}
HRESULT STDMETHODCALLTYPE add_added(WinrtObject*, IUnknown* h, INT64* t) { return register_handler(added,h,t); }
HRESULT STDMETHODCALLTYPE remove_added(WinrtObject*, INT64 t) { return added.remove(t); }
HRESULT STDMETHODCALLTYPE add_removed(WinrtObject*, IUnknown* h, INT64* t) { return register_handler(removed,h,t); }
HRESULT STDMETHODCALLTYPE remove_removed(WinrtObject*, INT64 t) { return removed.remove(t); }
HRESULT STDMETHODCALLTYPE add_pairing(WinrtObject*, IUnknown* h, INT64* t) { return register_handler(pairing_changed,h,t); }
HRESULT STDMETHODCALLTYPE remove_pairing(WinrtObject*, INT64 t) { return pairing_changed.remove(t); }
HRESULT factory_query(WinrtObject*, REFIID iid, void** out) { return xbox_controller_factory(iid,out); }
HRESULT pad_factory_query(WinrtObject*, REFIID iid, void** out) { return xbox_gamepad_factory(iid,out); }
HRESULT STDMETHODCALLTYPE add_pad_added(WinrtObject*, IUnknown* h, INT64* t) { return register_handler(gamepad_added,h,t); }
HRESULT STDMETHODCALLTYPE remove_pad_added(WinrtObject*, INT64 t) { return gamepad_added.remove(t); }
HRESULT STDMETHODCALLTYPE add_pad_removed(WinrtObject*, IUnknown* h, INT64* t) { return register_handler(gamepad_removed,h,t); }
HRESULT STDMETHODCALLTYPE remove_pad_removed(WinrtObject*, INT64 t) { return gamepad_removed.remove(t); }

struct InputEventArgs : WinrtObject {
    std::atomic<ULONG> refs{1};
    IUnknown* item = nullptr;
    ~InputEventArgs() { if (item) item->Release(); }
};
HRESULT args_query(WinrtObject* self, REFIID iid, void** out) {
    if (iid != __uuidof(IUnknown) && iid != __uuidof(IInspectable) && iid != self->iid) return E_NOINTERFACE;
    *out = self; retain(static_cast<InputEventArgs*>(self)); return S_OK;
}
HRESULT STDMETHODCALLTYPE event_item(InputEventArgs* self, void** out) {
    if (!out) return E_POINTER;
    *out = self->item; self->item->AddRef(); return S_OK;
}
HRESULT STDMETHODCALLTYPE args_iids(InputEventArgs*, ULONG* count, IID** iids) {
    if (!count || !iids) return E_POINTER;
    *count = 0; *iids = nullptr; return S_OK;
}
HRESULT notify(WinrtEvent& event, Controller* controller, bool is_added, bool pads) {
    static const auto table = [] {
        auto t = winrt_vtable();
        t[0] = reinterpret_cast<const void*>(&dynamic_query<InputEventArgs>);
        t[1] = reinterpret_cast<const void*>(&retain<InputEventArgs>); t[2] = reinterpret_cast<const void*>(&release<InputEventArgs>);
        t[3] = reinterpret_cast<const void*>(&args_iids);
        t[6] = reinterpret_cast<const void*>(&event_item); return t;
    }();
    auto* args = new (std::nothrow) InputEventArgs;
    if (!args) return E_OUTOFMEMORY;
    // The reference's InputArgs (19b6d0/19b790) exposes base interfaces and an
    // empty IID list. Do not invent the unpublished gamepad event IIDs.
    const GUID iid = pads ? __uuidof(IInspectable) : is_added ? GUID{0x3dacf202,0x8c03,0x4db7,{0x9e,0x89,0x8f,0x2e,0x1c,0x53,0x74,0xbe}} :
        GUID{0xfd4af91e,0x40c3,0x4de0,{0x82,0x1b,0x0b,0xe1,0xbd,0x14,0x17,0xd1}};
    const wchar_t* name = pads ? (is_added ? L"Windows.Xbox.Input.GamepadAddedEventArgs" : L"Windows.Xbox.Input.GamepadRemovedEventArgs") :
        (is_added ? L"Windows.Xbox.Input.ControllerAddedEventArgs" : L"Windows.Xbox.Input.ControllerRemovedEventArgs");
    static_cast<WinrtObject&>(*args) = {table.data(), name, iid, &args_query};
    args->item = reinterpret_cast<IUnknown*>(pads ? static_cast<WinrtObject*>(&controller->gamepad) : static_cast<WinrtObject*>(controller));
    args->item->AddRef();
    HRESULT result = S_OK;
    std::vector<IUnknown*> handlers;
    try { handlers = event.snapshot(); }
    catch (const std::bad_alloc&) { release(args); return E_OUTOFMEMORY; }
    void* sender = nullptr;
    (pads ? xbox_gamepad_factory : xbox_controller_factory)(__uuidof(IInspectable), &sender);
    for (IUnknown* handler : handlers) {
        const auto invoke = reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(IUnknown*, IInspectable*, void*)>((*reinterpret_cast<void***>(handler))[3]);
        const HRESULT hr = invoke(handler, static_cast<IInspectable*>(sender), args);
        handler->Release(); if (FAILED(hr) && SUCCEEDED(result)) result = hr;
    }
    release(args); return result;
}
}

HRESULT poll_title_input() try {
    if (!event_thread) return S_OK;
    if (!host().get) return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    if (event_thread && event_thread != GetCurrentThreadId()) return RPC_E_WRONG_THREAD;
    std::array<bool, XUSER_MAX_COUNT> current{};
    std::array<Controller*, XUSER_MAX_COUNT> changed{};
    {
        std::lock_guard lock(input_mutex);
        for (DWORD index = 0; index < XUSER_MAX_COUNT; ++index) {
            XINPUT_STATE state{}; const DWORD error = host().get(index, &state);
            if (error && error != ERROR_DEVICE_NOT_CONNECTED) return HRESULT_FROM_WIN32(error);
            current[index] = error == ERROR_SUCCESS;
            if (initialized && current[index] != connected[index]) {
                if (!controller_objects[index]) controller_objects[index] = make_controller(index);
                if (!(changed[index] = controller_objects[index])) return E_OUTOFMEMORY;
            }
        }
        connected = current; initialized = true;
    }
    for (DWORD index = 0; index < XUSER_MAX_COUNT; ++index) if (changed[index]) {
        HRESULT hr = notify(current[index] ? added : removed, changed[index], current[index], false);
        if (SUCCEEDED(hr)) hr = notify(current[index] ? gamepad_added : gamepad_removed, changed[index], current[index], true);
        if (FAILED(hr)) return hr;
    }
    return S_OK;
} catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
void stop_title_input() {
    if (event_thread && event_thread != GetCurrentThreadId()) return;
    if (input_timer && event_thread == GetCurrentThreadId()) KillTimer(nullptr,input_timer);
    input_timer = 0; event_thread = 0; initialized = false;
    added.clear(); removed.clear(); gamepad_added.clear(); gamepad_removed.clear(); pairing_changed.clear();
}

HRESULT xbox_controller_factory(REFIID iid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    static const auto activation_table = winrt_vtable();
    static const auto statics_table = [] {
        auto t = winrt_vtable();
        t[6] = reinterpret_cast<const void*>(&controllers);
        t[7] = reinterpret_cast<const void*>(&add_added); t[8] = reinterpret_cast<const void*>(&remove_added);
        t[9] = reinterpret_cast<const void*>(&add_removed); t[10] = reinterpret_cast<const void*>(&remove_removed);
        t[11] = reinterpret_cast<const void*>(&add_pairing); t[12] = reinterpret_cast<const void*>(&remove_pairing);
        return t;
    }();
    static WinrtObject activation{activation_table.data(), controller_class, __uuidof(IActivationFactory), &factory_query};
    static WinrtObject statics{statics_table.data(), controller_class, iid_statics, &factory_query};
    if (iid == iid_statics) *out = &statics;
    else if (iid == __uuidof(IUnknown) || iid == __uuidof(IInspectable) || iid == __uuidof(IActivationFactory)) *out = &activation;
    else return E_NOINTERFACE;
    return S_OK;
}
HRESULT xbox_gamepad_factory(REFIID iid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    static const auto activation_table = winrt_vtable();
    static const auto statics_table = [] {
        auto t = winrt_vtable(); t[6] = reinterpret_cast<const void*>(&controllers);
        t[7] = reinterpret_cast<const void*>(&add_pad_added); t[8] = reinterpret_cast<const void*>(&remove_pad_added);
        t[9] = reinterpret_cast<const void*>(&add_pad_removed); t[10] = reinterpret_cast<const void*>(&remove_pad_removed); return t;
    }();
    static WinrtObject activation{activation_table.data(), gamepad_class, __uuidof(IActivationFactory), &pad_factory_query};
    static WinrtObject statics{statics_table.data(), gamepad_class, iid_gamepad_statics, &pad_factory_query};
    if (iid == iid_gamepad_statics) *out = &statics;
    else if (iid == __uuidof(IUnknown) || iid == __uuidof(IInspectable) || iid == __uuidof(IActivationFactory)) *out = &activation;
    else return E_NOINTERFACE;
    return S_OK;
}
}
