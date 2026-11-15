// SPDX-License-Identifier: BSD-3-Clause
// System UI answers to the title's own calls: GameDisplayName returns the
// profile the system UI signed in; a system UI quit reaches the title through
// its CoreApplication.Suspending registration, on its window thread, and waits
// for the deferral it took.
#include "xboxone_native/era_loader.h"
#include "era_winrt_classes.h"

#include <inspectable.h>
#include <winstring.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>

namespace {
namespace era = xboxone::era;
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++failures; std::cout << "FAIL: " << what << '\n'; }
}
template <typename R, typename... A>
R call(void* object, unsigned slot, A... args) {
    auto** vt = *static_cast<void***>(object);
    return reinterpret_cast<R(STDMETHODCALLTYPE*)(void*, A...)>(vt[slot])(object, args...);
}

std::atomic<int> invoked{0};
std::atomic<DWORD> handler_thread{0};
std::int64_t deadline = 0;
bool complete_deferral = true;

struct Handler : IUnknown { // EventHandler<SuspendingEventArgs>: Invoke at slot 3.
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override { *out = this; return S_OK; }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    virtual HRESULT STDMETHODCALLTYPE Invoke(IInspectable* sender, IInspectable* args) {
        ++invoked;
        handler_thread = GetCurrentThreadId();
        IInspectable* operation = nullptr;
        IInspectable* deferral = nullptr;
        if (!sender || !args || FAILED(call<HRESULT>(args, 6, &operation)) || !operation ||
            FAILED(call<HRESULT>(operation, 7, &deadline)) || FAILED(call<HRESULT>(operation, 6, &deferral)) || !deferral)
            return E_FAIL;
        if (complete_deferral) // Completed later on another thread, as titles save asynchronously.
            std::thread([deferral] { Sleep(100); call<HRESULT>(deferral, 6); call<HRESULT>(deferral, 6); }).detach();
        return S_OK;
    }
};
Handler handler;

DWORD window_thread = 0;
era::SuspendOutcome completed{}, abandoned{};
long long completed_ms = 0;

void pump_until(std::atomic<bool>& done) {
    while (!done) {
        MSG message{};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&message); DispatchMessageW(&message); }
        Sleep(1);
    }
}

struct View : IInspectable { // IFrameworkView: 6 Initialize, 7 SetWindow, 8 Load, 9 Run, 10 Uninitialize.
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override { *out = this; return S_OK; }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE GetIids(ULONG*, IID**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel*) override { return E_NOTIMPL; }
    virtual HRESULT STDMETHODCALLTYPE Initialize(IInspectable*) { return S_OK; }
    virtual HRESULT STDMETHODCALLTYPE SetWindow(IInspectable*) { return S_OK; }
    virtual HRESULT STDMETHODCALLTYPE Load(HSTRING) { return S_OK; }
    virtual HRESULT STDMETHODCALLTYPE Run() {
        window_thread = GetCurrentThreadId();
        std::atomic<bool> done{false};
        std::thread system_ui([&] {
            const auto start = std::chrono::steady_clock::now();
            completed = era::suspend_title(3000);
            completed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
            complete_deferral = false;
            abandoned = era::suspend_title(300);
            done = true;
        });
        pump_until(done);
        system_ui.join();
        return S_OK;
    }
    virtual HRESULT STDMETHODCALLTYPE Uninitialize() { return S_OK; }
};
View view;

struct Source : IInspectable { // IFrameworkViewSource: CreateView at slot 6.
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID, void** out) override { *out = this; return S_OK; }
    ULONG STDMETHODCALLTYPE AddRef() override { return 2; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }
    HRESULT STDMETHODCALLTYPE GetIids(ULONG*, IID**) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetRuntimeClassName(HSTRING*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE GetTrustLevel(TrustLevel*) override { return E_NOTIMPL; }
    virtual HRESULT STDMETHODCALLTYPE CreateView(IInspectable** out) { *out = &view; return S_OK; }
};
Source source;
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) { std::cerr << "usage: era_system_ui_tests <AppxManifest.xml>\n"; return 2; }
    namespace fs = std::filesystem;
    const auto package = fs::temp_directory_path() / ("xone-era-system-ui-" + std::to_string(GetCurrentProcessId()));
    fs::create_directories(package);
    fs::copy_file(argv[1], package / "AppxManifest.xml", fs::copy_options::overwrite_existing);
    era::set_title_package_root(package.wstring());

    // The record and name as console_state.cpp prepares them for a signed-in profile.
    const auto storage = package / "storage";
    fs::create_directories(storage);
    const unsigned char record[32]{'X', 'O', 'N', 'E', 'U', 'S', 'R', '1', 1, 0, 0, 0, 0, 0, 0, 0, 0x11, 0x22};
    std::ofstream(storage / ".xone-local-profile", std::ios::binary).write(reinterpret_cast<const char*>(record), 32);
    std::ofstream(storage / ".xone-local-profile-name", std::ios::binary) << "Jogadora \xC3\x9Anica";
    era::set_title_storage_root(storage.wstring());
    void* user = nullptr;
    check(era::title_local_user(&user) == S_OK && user, "signed-in profile served as Windows.Xbox.System.User");
    if (user) {
        IInspectable* display = nullptr;
        HSTRING name = nullptr;
        check(call<HRESULT>(user, 9, &display) == S_OK && display && call<HRESULT>(display, 6, &name) == S_OK &&
                  std::wstring(WindowsGetStringRawBuffer(name, nullptr)) == L"Jogadora Única",
              "GameDisplayName is the signed-in profile's name");
        WindowsDeleteString(name);
        if (display) display->Release();
        static_cast<IUnknown*>(user)->Release();
    }

    check(era::suspend_title(100) == era::SuspendOutcome::no_handler, "no window: nothing to suspend");
    check(!era::start_suspend_watch(), "unsupervised run has no suspend event");

    constexpr GUID iid_core_application{0x0aacf7a4, 0x5e1d, 0x49df, {0x80, 0x34, 0xfb, 0x6a, 0x68, 0xbc, 0x5e, 0xd1}};
    void* app = nullptr;
    check(era::core_application_factory(iid_core_application, &app) == S_OK && app, "ICoreApplication");
    if (app) {
        std::int64_t token = 0;
        check(call<HRESULT>(app, 7, static_cast<IUnknown*>(&handler), &token) == S_OK, "title registers Suspending");
        check(call<HRESULT>(app, 13, static_cast<IUnknown*>(&source)) == S_OK, "CoreApplication::Run");
        check(completed == era::SuspendOutcome::suspended && completed_ms >= 90, "suspension waits for the completed deferral");
        check(abandoned == era::SuspendOutcome::deadline, "an uncompleted deferral ends at the deadline");
        check(invoked == 2 && handler_thread == window_thread, "Suspending runs on the title's window thread");
        FILETIME now{};
        GetSystemTimeAsFileTime(&now);
        const auto ticks = (static_cast<std::int64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
        check(deadline > ticks - 50'000'000 && deadline < ticks + 50'000'000, "deadline is a current DateTime");
        call<HRESULT>(app, 8, token);
        check(era::suspend_title(100) == era::SuspendOutcome::no_handler, "window gone after Run");
    }
    std::error_code ec;
    fs::remove_all(package, ec);
    std::cout << (failures ? "era_system_ui: FAILED\n" : "era_system_ui: all checks passed\n");
    return failures ? 1 : 0;
}
