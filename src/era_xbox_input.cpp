// SPDX-License-Identifier: BSD-3-Clause
#include "era_winrt_classes.h"
#include "era_winrt_object.h"
#include <activation.h>
#include <xinput.h>
#include <winstring.h>
#include <atomic>
#include <memory>
#include <new>
#include <cstdio>
#include <cmath>

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
    decltype(&XInputSetState) set = module ? reinterpret_cast<decltype(set)>(GetProcAddress(module, "XInputSetState")) : nullptr;
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
struct Controller : WinrtObject {
    std::atomic<ULONG> refs{1}; DWORD index = 0; Gamepad gamepad;
    IInspectable* user = nullptr;
    bool vibrating = false;
    void stop_vibration() {
        if (vibrating && host().set) {
            XINPUT_VIBRATION off{}; const DWORD error = host().set(index, &off);
            if (!error || error == ERROR_DEVICE_NOT_CONNECTED) vibrating = false;
            else std::fprintf(stderr, "xone-era: controller %lu vibration teardown failed Win32 %lu\n", index, error);
        }
    }
    ~Controller() { stop_vibration(); if (user) user->Release(); }
};
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
HRESULT STDMETHODCALLTYPE controller_user(Controller* self, IInspectable** out) {
    if (!out) return E_POINTER;
    *out = self->user;
    self->user->AddRef();
    return S_OK;
}
HRESULT STDMETHODCALLTYPE gamepad_vibration(Gamepad* self, const std::array<float, 4>* input) {
    std::array<float, 4> value{}; SIZE_T read = 0;
    if (!input || !ReadProcessMemory(GetCurrentProcess(), input, value.data(), sizeof value, &read) || read != sizeof value)
        return E_INVALIDARG;
    for (float level : value) if (!std::isfinite(level) || level < 0 || level > 1) return E_INVALIDARG;
    // XInput exposes two motors; never pretend that trigger-motor commands were executed.
    if (value[2] || value[3]) return HRESULT_FROM_WIN32(ERROR_NOT_SUPPORTED);
    if (!host().set) return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    XINPUT_VIBRATION vibration{WORD(std::lround(value[0] * 65535.f)), WORD(std::lround(value[1] * 65535.f))};
    std::lock_guard lock(input_mutex);
    const DWORD error = host().set(self->owner->index, &vibration);
    if (error) return HRESULT_FROM_WIN32(error);
    self->owner->vibrating = vibration.wLeftMotorSpeed || vibration.wRightMotorSpeed;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE raw_gamepad_reading(Gamepad* self,RawGamepadReading* out) {
    if (!out) return E_POINTER;
    *out = {};
    if (!host().get) return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    XINPUT_STATE state{};
    const DWORD error=host().get(self->owner->index,&state);
    if (error) return HRESULT_FROM_WIN32(error);
    LARGE_INTEGER counter{},frequency{};
    if (!QueryPerformanceCounter(&counter) || !QueryPerformanceFrequency(&frequency) || frequency.QuadPart<=0)
        return E_FAIL;
    RawGamepadReading value{};
    value.timestamp=UINT64(counter.QuadPart/frequency.QuadPart)*10000000 +
        UINT64(counter.QuadPart%frequency.QuadPart)*10000000/UINT64(frequency.QuadPart);
    const UINT buttons=state.Gamepad.wButtons;
    value.buttons=((buttons&0xf)<<8) | ((buttons&0x30)>>2) | ((buttons&0xc0)<<8) |
        ((buttons&0x300)<<4) | ((buttons&0xf000)>>8);
    value.left_trigger=state.Gamepad.bLeftTrigger/255.f;
    value.right_trigger=state.Gamepad.bRightTrigger/255.f;
    const auto axis=[](SHORT v){return v/(v<0 ? 32768.f : 32767.f);};
    value.left_x=axis(state.Gamepad.sThumbLX); value.left_y=axis(state.Gamepad.sThumbLY);
    value.right_x=axis(state.Gamepad.sThumbRX); value.right_y=axis(state.Gamepad.sThumbRY);
    *out=value;
    return S_OK;
}
struct GamepadReading : WinrtObject {
    std::atomic<ULONG> refs{1};
    RawGamepadReading value{};
    UINT64 timestamp = 0; // Foundation.DateTime: UTC FILETIME units, unlike the raw monotonic timestamp.
};
HRESULT reading_query(WinrtObject* self, REFIID iid, void** out) {
    if (iid != __uuidof(IUnknown) && iid != __uuidof(IInspectable)) return E_NOINTERFACE;
    *out = self; retain(static_cast<GamepadReading*>(self)); return S_OK;
}
HRESULT STDMETHODCALLTYPE reading_iids(GamepadReading*, ULONG* count, IID** iids) {
    if (!count || !iids) return E_POINTER;
    *count = 0; *iids = nullptr; return S_OK; // Reference 14019a0a0; private reading IID remains unaudited.
}
HRESULT STDMETHODCALLTYPE reading_time(GamepadReading* self, UINT64* out) {
    if (!out) return E_POINTER; *out = self->timestamp; return S_OK;
}
HRESULT STDMETHODCALLTYPE reading_buttons(GamepadReading* self, UINT* out) {
    if (!out) return E_POINTER; *out = self->value.buttons; return S_OK;
}
template<UINT Mask> HRESULT STDMETHODCALLTYPE reading_button(GamepadReading* self, boolean* out) {
    if (!out) return E_POINTER; *out = (self->value.buttons & Mask) != 0; return S_OK;
}
template<float RawGamepadReading::*Member> HRESULT STDMETHODCALLTYPE reading_axis(GamepadReading* self, float* out) {
    if (!out) return E_POINTER; *out = self->value.*Member; return S_OK;
}
HRESULT STDMETHODCALLTYPE gamepad_reading(Gamepad* self, GamepadReading** out) try {
    if (!out) return E_POINTER;
    *out = nullptr;
    auto reading = std::make_unique<GamepadReading>();
    const HRESULT hr = raw_gamepad_reading(self, &reading->value);
    if (FAILED(hr)) return hr;
    FILETIME time{}; GetSystemTimePreciseAsFileTime(&time);
    reading->timestamp = (UINT64(time.dwHighDateTime) << 32) | time.dwLowDateTime;
    static const auto table = [] {
        auto t = winrt_vtable();
        t[0] = reinterpret_cast<const void*>(&dynamic_query<GamepadReading>);
        t[1] = reinterpret_cast<const void*>(&retain<GamepadReading>); t[2] = reinterpret_cast<const void*>(&release<GamepadReading>);
        t[3] = reinterpret_cast<const void*>(&reading_iids);
        t[6] = reinterpret_cast<const void*>(&reading_time); t[7] = reinterpret_cast<const void*>(&reading_buttons);
        t[8] = reinterpret_cast<const void*>(&reading_button<0x100>); t[9] = reinterpret_cast<const void*>(&reading_button<0x200>);
        t[10] = reinterpret_cast<const void*>(&reading_button<0x400>); t[11] = reinterpret_cast<const void*>(&reading_button<0x800>);
        t[12] = reinterpret_cast<const void*>(&reading_button<0x4>); t[13] = reinterpret_cast<const void*>(&reading_button<0x8>);
        t[14] = reinterpret_cast<const void*>(&reading_button<0x4000>); t[15] = reinterpret_cast<const void*>(&reading_button<0x8000>);
        t[16] = reinterpret_cast<const void*>(&reading_button<0x1000>); t[17] = reinterpret_cast<const void*>(&reading_button<0x2000>);
        t[18] = reinterpret_cast<const void*>(&reading_button<0x10>); t[19] = reinterpret_cast<const void*>(&reading_button<0x20>);
        t[20] = reinterpret_cast<const void*>(&reading_button<0x40>); t[21] = reinterpret_cast<const void*>(&reading_button<0x80>);
        t[22] = reinterpret_cast<const void*>(&reading_axis<&RawGamepadReading::left_trigger>);
        t[23] = reinterpret_cast<const void*>(&reading_axis<&RawGamepadReading::right_trigger>);
        t[24] = reinterpret_cast<const void*>(&reading_axis<&RawGamepadReading::left_x>);
        t[25] = reinterpret_cast<const void*>(&reading_axis<&RawGamepadReading::left_y>);
        t[26] = reinterpret_cast<const void*>(&reading_axis<&RawGamepadReading::right_x>);
        t[27] = reinterpret_cast<const void*>(&reading_axis<&RawGamepadReading::right_y>);
        return t;
    }();
    static_cast<WinrtObject&>(*reading) = {table.data(), L"Windows.Xbox.Input.GamepadReading", __uuidof(IInspectable), &reading_query};
    *out = reading.release(); return S_OK;
} catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
HRESULT make_controller(DWORD index, Controller** out) {
    static const auto table = [] {
        auto t = winrt_vtable();
        t[0] = reinterpret_cast<const void*>(&dynamic_query<Controller>);
        t[1] = reinterpret_cast<const void*>(&retain<Controller>); t[2] = reinterpret_cast<const void*>(&release<Controller>);
        t[6] = reinterpret_cast<const void*>(&controller_id); t[7] = reinterpret_cast<const void*>(&controller_type);
        t[8] = reinterpret_cast<const void*>(&controller_user); // IController.User, shared local session profile.
        return t;
    }();
    auto object = std::make_unique<Controller>();
    const HRESULT hr = title_local_user(reinterpret_cast<void**>(&object->user));
    if (FAILED(hr)) return hr;
    static_cast<WinrtObject&>(*object) = {table.data(), controller_class, iid_controller, &controller_query};
    object->index = index;
    static const auto pad_table = [] {
        auto t = winrt_vtable();
        t[0] = reinterpret_cast<const void*>(&dynamic_query<Gamepad>);
        t[1] = reinterpret_cast<const void*>(&gamepad_retain); t[2] = reinterpret_cast<const void*>(&gamepad_release);
        t[6] = reinterpret_cast<const void*>(&gamepad_vibration); // Reference 14019a500: four floats, by address on x64.
        t[7] = reinterpret_cast<const void*>(&gamepad_reading); // Reference 14019a5c0 returns an immutable reading object.
        t[8] = reinterpret_cast<const void*>(&raw_gamepad_reading); // Reference 14019a690 copies exactly 40 bytes.
        return t;
    }();
    static_cast<WinrtObject&>(object->gamepad) = {pad_table.data(), gamepad_class, iid_gamepad, &gamepad_query};
    object->gamepad.owner = object.get();
    *out = object.release();
    return S_OK;
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
        if (!controller) { const HRESULT hr = make_controller(index, &controller); if (FAILED(hr)) return hr; }
        auto* item = reinterpret_cast<IUnknown*>(pads ? static_cast<WinrtObject*>(&controller->gamepad) : static_cast<WinrtObject*>(controller));
        view->items.push_back(item); item->AddRef();
    }
    *out = view.release(); return S_OK;
} catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
HRESULT register_handler(WinrtEvent& event, IUnknown* handler, INT64* token) {
    if (!handler || !token) return E_INVALIDARG;
    *token=0;
    if (!host().get) return HRESULT_FROM_WIN32(ERROR_PROC_NOT_FOUND);
    if (!event_thread) {
        const HRESULT hr=start_title_input();if (FAILED(hr)) return hr;
    }
    if (event_thread != GetCurrentThreadId()) {
        // The dispatcher can invoke an agile delegate registered by a worker.
        // Non-agile delegates still require an apartment-aware marshaling path.
        IUnknown* agile=nullptr;
        const HRESULT hr=handler->QueryInterface(__uuidof(IAgileObject),reinterpret_cast<void**>(&agile));
        if (FAILED(hr)) return hr==E_NOINTERFACE ? RPC_E_WRONG_THREAD : hr;
        agile->Release();
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
    if (event_thread && event_thread != GetCurrentThreadId()) {
        std::fprintf(stderr,"xone-era: input poll thread %lu differs from owner %lu\n",GetCurrentThreadId(),event_thread.load());
        return RPC_E_WRONG_THREAD;
    }
    std::array<bool, XUSER_MAX_COUNT> current{};
    std::array<Controller*, XUSER_MAX_COUNT> changed{};
    {
        std::lock_guard lock(input_mutex);
        for (DWORD index = 0; index < XUSER_MAX_COUNT; ++index) {
            XINPUT_STATE state{}; const DWORD error = host().get(index, &state);
            if (error && error != ERROR_DEVICE_NOT_CONNECTED) return HRESULT_FROM_WIN32(error);
            current[index] = error == ERROR_SUCCESS;
            if (initialized && current[index] != connected[index]) {
                if (!controller_objects[index]) { const HRESULT hr = make_controller(index, &controller_objects[index]); if (FAILED(hr)) return hr; }
                changed[index] = controller_objects[index];
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
HRESULT start_title_input() {
    DWORD previous=0;
    if (!event_thread.compare_exchange_strong(previous,GetCurrentThreadId()))
        return previous==GetCurrentThreadId() ? S_OK : RPC_E_WRONG_THREAD;
    const HRESULT hr=poll_title_input();
    if (FAILED(hr)) { event_thread=0;return hr; }
    input_timer=SetTimer(nullptr,0,32,nullptr);
    if (!input_timer) {event_thread=0;return HRESULT_FROM_WIN32(GetLastError());}
    return S_OK;
}
void stop_title_input() {
    if (event_thread && event_thread != GetCurrentThreadId()) return;
    if (input_timer && event_thread == GetCurrentThreadId()) KillTimer(nullptr,input_timer);
    input_timer = 0; event_thread = 0; initialized = false;
    added.clear(); removed.clear(); gamepad_added.clear(); gamepad_removed.clear(); pairing_changed.clear();
    std::lock_guard lock(input_mutex);
    for (auto*& controller : controller_objects) { if (controller) { controller->stop_vibration(); release(controller); } controller = nullptr; }
    connected = {};
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
