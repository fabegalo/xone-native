// SPDX-License-Identifier: BSD-3-Clause
// Windows x64: the dashboard's real path through the existing ERA runtime.
// Imports an owned fixture package, checks it with `xone-era check`, launches it
// with `xone-era run`, observes its exit and returns; also renders a dashboard
// frame through the GDI surface. Optional: --capture DIR writes BMP frames.
#include "dashboard_windows.h"
#include "xboxone_native/title_catalog.h"
#include "sha256.h"
#include "xboxone_native/presentation.h"

#include <objbase.h>

#include <chrono>
#include <cstdio>
#include <cwchar>
#include <fstream>
#include <iostream>
#include <random>
#include <thread>

namespace fs = std::filesystem;
using namespace xboxone;

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++failures; std::cerr << "FAIL: " << what << '\n'; }
}

struct Platform : dashboard::Platform {
    HWND parent = nullptr;
    bool pick_folder(fs::path&) override { return false; }
    void play(dashboard::Sound) override {}
    void rumble() override {}
    bool apply_display(std::uint32_t, std::uint32_t, bool, std::string&) override { return true; }
    void focus_dashboard() override { if (parent) { SetForegroundWindow(parent); SetFocus(parent); } }
    std::uintptr_t session_window() override { return reinterpret_cast<std::uintptr_t>(parent); }
    bool open_folder(const fs::path&) override { return true; }
    std::uint32_t controllers() override { return 0; }
    void request_exit() override {}
    std::int64_t now() override { return 1700000000; }
};

bool wait(session::Host& host, session::Result& result) {
    for (int i = 0; i < 400; ++i) {
        if (host.poll(result)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    host.terminate();
    return false;
}

// Stand-in for xone-era's suspend watch, used to test the supervisor's side of the
// contract: the event named after this process, then the line xone-era prints.
int fake_runtime(const std::wstring& title) {
    const auto name = L"Local\\xone-era-suspend-" + std::to_wstring(GetCurrentProcessId());
    HANDLE event = OpenEventW(SYNCHRONIZE, FALSE, name.c_str());
    if (!event) { std::puts("fake-era: no suspend event"); return 3; }
    if (title.find(L"hang") != std::wstring::npos) Sleep(INFINITE);
    if (WaitForSingleObject(event, 20000) != WAIT_OBJECT_0) return 4;
    std::puts("xone-era: system UI requested suspend\nxone-era: title suspended");
    return 0;
}

long long elapsed_ms(std::chrono::steady_clock::time_point since) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - since).count();
}

std::vector<char> read(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}

void write_bmp(const fs::path& path, const dashboard::GdiSurface& surface) {
    const int w = surface.width(), h = surface.height();
    const std::uint32_t size = static_cast<std::uint32_t>(w * h * 4);
    BITMAPFILEHEADER file{0x4D42, 54 + size, 0, 0, 54};
    BITMAPINFOHEADER info{sizeof info, w, -h, 1, 32, BI_RGB, size, 0, 0, 0, 0};
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char*>(&file), sizeof file);
    out.write(reinterpret_cast<const char*>(&info), sizeof info);
    out.write(reinterpret_cast<const char*>(surface.bits()), size);
}

// Opt-in original input: exercise the same catalog/profile/launch/Guide path,
// without treating a live process or a successful return as proof of gameplay.
int original_lifecycle(const fs::path& runtime, const fs::path& package, const fs::path& root, unsigned duration_ms) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    auto host = session::create_host();
    Platform platform;
    std::string error;
    presentation::Presenter presenter;
    presentation::PresenterConfig config;
    config.width = 1920; config.height = 1080; config.visible = true;
    config.swapchain_format = presentation::PixelFormat::bgra8_unorm;
    config.window_title = "xone-native session integration (local_recreation UI)";
    if (!presenter.initialize(config, error)) { std::cerr << error << '\n'; CoUninitialize(); return 1; }
    platform.parent = static_cast<HWND>(presenter.native_window());
    SetWindowLongPtrW(platform.parent, GWL_STYLE, GetWindowLongPtrW(platform.parent, GWL_STYLE) | WS_CLIPCHILDREN);
    SetWindowPos(platform.parent, nullptr, 40, 40, 1280, 720, SWP_NOZORDER | SWP_NOACTIVATE);
    dashboard::Dashboard ui(root, runtime, platform, *host);
    if (!ui.start(error)) { std::cerr << error << '\n'; CoUninitialize(); return 1; }
    if (ui.state.profiles.empty()) {
        ui.input(dashboard::Button::a);
        for (const char c : std::string("Local Player")) ui.text(static_cast<char32_t>(c));
        ui.input(dashboard::Button::menu);
    } else ui.sign_in(ui.state.profiles.front().id);
    ui.import_folder(package);
    if (!ui.profile() || ui.state.titles.size() != 1) {
        std::cerr << "expected one title and a signed-in local profile\n";
        for (const auto& dialog : ui.dialogs)
            for (const auto& line : dialog.lines) std::cerr << line << '\n';
        CoUninitialize(); return 1;
    }
    const auto id = ui.state.titles.front().id;
    for (int i = 0; i < 400 && ui.title(id)->readiness == console::Readiness::unchecked; ++i) {
        ui.tick(0.025); Sleep(25);
    }
    check(ui.title(id)->readiness == console::Readiness::ready, "original imports checked by ERA");
    std::cout << "original SHA256 " << id << '\n' << std::flush;
    dashboard::GdiSurface surface;
    dashboard::Renderer renderer;
    check(surface.resize(1920,1080), "local dashboard surface allocated");
    const auto capture = [&](const char* name) {
        auto canvas=surface.canvas();
        for(int i=0;i<30;++i) renderer.draw(ui,canvas,surface,0.05);
        write_bmp(root/name,surface);
        presentation::ExternalFrameView frame;
        frame.width = 1920; frame.height = 1080; frame.row_pitch_bytes = 1920 * 4;
        frame.format = presentation::PixelFormat::bgra8_unorm;
        frame.provenance = {presentation::FrameOrigin::local_recreation, "xone_local_dashboard", "", 0};
        frame.pixels = {reinterpret_cast<const std::uint8_t*>(surface.bits()), 1920 * 1080 * 4};
        check(presenter.present_frame(frame, error), "dashboard frame reaches persistent session window");
    };
    capture("home.bmp");
    // Exercise the finite session setting as well as a Guide quit before it expires.
    ui.state.settings.time_limit_minutes = 2;
    ui.launch(id);
    check(ui.session_title == id, "original launched by dashboard");
    const auto start = std::chrono::steady_clock::now();
    while (!ui.session_title.empty() && elapsed_ms(start) < duration_ms) {
        bool quit = false; check(presenter.pump_messages(quit, error) && !quit, "session window message pump");
        ui.tick(0.025); Sleep(25);
        std::error_code ec;
        if (fs::file_size(root / "logs/last-session.log", ec) > 2 * 1024 * 1024 && !ec) {
            check(false, "original session log exceeded 2 MiB diagnostic budget");
            ui.quit_title(); ui.quit_title(); break;
        }
    }
    check(!ui.session_title.empty(), "original remains active until Guide quit");
    ui.input(dashboard::Button::guide);
    check(ui.guide_open && ui.profile(), "Guide retains the session profile");
    capture("guide.bmp");
    dashboard::Renderer overlay_renderer;
    auto overlay = surface.canvas();
    overlay_renderer.draw(ui, overlay, surface, 1.0, true);
    GdiFlush();
    check(host->present_system_ui({ui.active_surface(), 1920, 1080,
          std::span<const std::uint32_t>(surface.bits(), 1920 * 1080)}), "Guide frame published to original title presentation");
    bool composed = false;
    for (int i = 0; i < 100 && !composed && !ui.session_title.empty(); ++i) {
        bool quit = false; presenter.pump_messages(quit, error);
        ui.tick(0.025); Sleep(25);
        const auto output = read(root / "logs/last-session.log");
        composed = std::string(output.begin(), output.end()).find("local_recreation Guide composited on title output") != std::string::npos;
    }
    check(composed, "original title presented a frame with the local Guide composed on its output");
    const auto child = FindWindowExW(platform.parent, nullptr, L"xone-era-core-window", nullptr);
    const auto profile_id = ui.profile() ? ui.profile()->id : std::string{};
    check(child != nullptr, "original title shares the persistent session window");
    ui.input(dashboard::Button::b);
    check(host->present_system_ui({ui.active_surface(), 1920, 1080,
          std::span<const std::uint32_t>(surface.bits(), 1920 * 1080)}), "close Guide on original title");
    ui.input(dashboard::Button::guide);
    for (const auto& item : ui.guide_items()) if (item.key == "guide:home") item.activate();
    check(host->present_system_ui({ui.active_surface(), 1920, 1080,
          std::span<const std::uint32_t>(surface.bits(), 1920 * 1080)}) && !IsWindowVisible(child),
          "Home takes presentation without ending original title");
    capture("home.bmp");
    ui.launch(id);
    check(host->present_system_ui({ui.active_surface(), 1920, 1080,
          std::span<const std::uint32_t>(surface.bits(), 1920 * 1080)}) && IsWindowVisible(child) &&
          FindWindowExW(platform.parent, nullptr, L"xone-era-core-window", nullptr) == child &&
          ui.profile() && ui.profile()->id == profile_id,
          "return from Home keeps the same original title window and profile");
    ui.quit_title();
    for (int i = 0; i < 400 && !ui.session_title.empty(); ++i) {
        bool quit = false; presenter.pump_messages(quit, error);
        ui.tick(0.025); Sleep(25);
    }
    check(ui.session_title.empty() && !host->active() && ui.page == dashboard::Page::home,
          "original returns to Home after Guide quit");
    ui.input(dashboard::Button::b);
    capture("returned-home.bmp");
    if (host->active()) { host->terminate(); session::Result result; wait(*host, result); }
    const auto bytes = read(root / "logs/last-session.log");
    const std::string log(bytes.begin(), bytes.end());
    check(log.find("xone-era: system UI requested suspend") != std::string::npos,
          "Guide request reached original runtime with a finite deadline");
    check(log.find("xone-era: title suspended") != std::string::npos,
          "original completed its Suspending handlers");
    console::State persisted;
    check(console::load(root, persisted, error) && persisted.profiles.size() == 1 && persisted.titles.size() == 1 &&
          persisted.titles.front().play_count > 0, "original profile, catalog and play history persisted");
    std::cout << ui.title(id)->last_result << '\n';
    CoUninitialize();
    return failures ? 1 : 0;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc >= 3 && std::wstring(argv[1]) == L"run") return fake_runtime(argv[2]);
    if ((argc == 5 || argc == 6) && std::wstring(argv[2]) == L"--original") {
        wchar_t* end=nullptr;
        const auto seconds=argc==6 ? std::wcstoul(argv[5],&end,10) : 15ul;
        if(!seconds || seconds>90 || (argc==6 && (!*argv[5] || *end))) {
            std::cerr << "original probe duration must be 1..90 seconds\n"; return 2;
        }
        return original_lifecycle(argv[1], argv[3], argv[4],static_cast<unsigned>(seconds*1000));
    }
    if (argc != 5 && argc != 7) { std::cerr << "usage: dashboard_era_tests <xone-era> <era_component> <AppxManifest.xml> <era_suspend_component> [--capture DIR]\n"
        "       dashboard_era_tests <xone-era> --original <package> <owned-console-root> [seconds: 1..90, default 15]\n"; return 2; }
    const fs::path runtime = argv[1], component = argv[2], manifest = argv[3];
    const fs::path capture = argc == 7 && std::wstring(argv[5]) == L"--capture" ? fs::path(argv[6]) : fs::path{};
    std::random_device device;
    const auto scratch = fs::canonical(fs::temp_directory_path()) / ("xone-dashboard-era-" + std::to_string(device()));
    fs::create_directories(scratch / "library" / "Owned Package");
    struct Cleanup { fs::path p; ~Cleanup() { std::error_code ec; fs::remove_all(p, ec); } } cleanup{scratch};
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // Supervisor over the real launcher.
    auto host = session::create_host();
    std::string error;
    session::Request run{session::Mode::run, runtime, component, scratch / "storage", scratch / "logs" / "run.log", 0};
    session::Result result;
    check(host->start(run, error), "supervisor starts xone-era run");
    check(wait(*host, result) && result.normal && result.exit_code == 0 && result.mapped, "fixture title runs and returns");

    // A deadline must not disable the Guide's suspend channel in the real launcher.
    for (const auto deadline : {0u, 60000u}) {
        auto suspend = run;
        suspend.title = argv[4];
        suspend.time_limit_ms = deadline;
        suspend.suspend_grace_ms = 1000;
        check(host->start(suspend, error), "start waiting fixture through ERA");
        bool entered = false;
        for (int i = 0; i < 400 && !entered; ++i) {
            const auto output = read(suspend.log);
            entered = std::string(output.begin(), output.end()).find("running title") != std::string::npos;
            if (!entered) Sleep(10);
        }
        check(entered, "waiting fixture reached entry");
        host->terminate();
        check(wait(*host, result) && result.terminated &&
              result.summary.find("no Suspending handler") != std::string::npos,
              "ERA receives Guide suspend with and without a deadline");
    }
    session::Request probe{session::Mode::check, runtime, component, {}, scratch / "logs" / "check.log", 0};
    check(host->start(probe, error) && wait(*host, result) && result.mapped && result.resolved == 2 && result.unresolved == 0,
          "check reports a ready fixture");
    auto bytes = read(component);
    const std::string needle("kernelx.dll\0", 12), replacement("missing.dll\0", 12);
    const auto at = std::search(bytes.begin(), bytes.end(), needle.begin(), needle.end());
    check(at != bytes.end(), "fixture imports kernelx.dll");
    if (at != bytes.end()) std::copy(replacement.begin(), replacement.end(), at);
    std::ofstream(scratch / "missing.exe", std::ios::binary).write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    probe.title = scratch / "missing.exe";
    check(host->start(probe, error) && wait(*host, result) && result.exit_code == 1 && result.unresolved == 2 &&
          result.summary.find("missing.dll") != std::string::npos, "unresolved imports reported with their real names");
    probe.mode = session::Mode::run;
    probe.storage = scratch / "storage";
    check(host->start(probe, error) && wait(*host, result) && !result.normal && result.exit_code == 1,
          "a title that cannot map is never reported as running");

    // Guide quit: the title is asked to suspend before the supervisor ends it.
    wchar_t self[MAX_PATH]{};
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    session::Request quit{session::Mode::run, self, scratch / "suspend.exe", scratch / "storage", scratch / "logs" / "quit.log", 0};
    check(host->start(quit, error), "supervisor starts a run session");
    auto since = std::chrono::steady_clock::now();
    host->terminate();
    check(wait(*host, result) && result.terminated && result.summary == "Closed by the system UI after the title suspended" &&
          elapsed_ms(since) < 5000, "quit lets the title suspend, then reports it");
    quit.title = scratch / "hang.exe";
    quit.suspend_grace_ms = 300;
    check(host->start(quit, error), "supervisor starts a title that ignores suspension");
    since = std::chrono::steady_clock::now();
    host->terminate();
    check(wait(*host, result) && result.terminated && result.summary == "Closed by the system UI" && elapsed_ms(since) >= 300,
          "a title that never suspends is ended after the grace period");
    quit.suspend_grace_ms = 20000;
    check(host->start(quit, error), "supervisor starts a title for forced quit");
    since = std::chrono::steady_clock::now();
    host->terminate();
    host->terminate();
    check(wait(*host, result) && result.terminated && elapsed_ms(since) < 5000, "a second quit ends the title at once");

    // Catalog identification from the package's own metadata.
    const auto package = scratch / "library" / "Owned Package";
    fs::copy_file(manifest, package / "AppxManifest.xml");
    fs::copy_file(component, package / "owned.exe");
    const auto scan = catalog::scan(scratch / "library");
    check(scan.titles.size() == 1 && scan.errors.empty(), "package identified");
    if (!scan.titles.empty()) {
        const auto& t = scan.titles[0];
        detail::Sha256 sha;
        const auto exe = read(package / "owned.exe");
        sha.update(std::span(reinterpret_cast<const std::uint8_t*>(exe.data()), exe.size()));
        check(t.name == "Owned activation test" && t.publisher == "XboxOneNative owned test" && t.version == "1.0.0.0" &&
              t.package_name == "XboxOneNative.OwnedLifecycleTest" && t.application_id == "OwnedLifecycle" && t.id == sha.finish(),
              "title metadata comes from the manifest and executable hash");
        check(catalog::verify(t, error), "unchanged title verifies");
    }
    console::State state;
    catalog::merge(state, scan, 1);
    check(catalog::merge(state, scan, 2) == 0 && state.titles.size() == 1, "rescan does not duplicate titles");

    // Dashboard -> ERA title -> dashboard.
    Platform platform;
    const auto root = scratch / "console";
    {
        dashboard::Dashboard ui(root, runtime, platform, *host);
        check(ui.start(error), "dashboard starts");
        ui.input(dashboard::Button::a);
        for (const char c : std::string("Player")) ui.text(static_cast<char32_t>(c));
        ui.input(dashboard::Button::menu);
        ui.import_folder(scratch / "library");
        check(ui.state.titles.size() == 1, "import adds the title");
        const auto id = ui.state.titles.empty() ? std::string{} : ui.state.titles[0].id;
        for (int i = 0; i < 400 && (!ui.checking_title.empty() || ui.title(id)->readiness == console::Readiness::unchecked); ++i) {
            ui.tick(0.025);
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        check(ui.title(id) && ui.title(id)->readiness == console::Readiness::ready, "imported title checked as ready by xone-era");

        dashboard::GdiSurface surface;
        dashboard::Renderer renderer;
        check(surface.resize(1920, 1080), "surface allocated");
        auto canvas = surface.canvas();
        for (int i = 0; i < 30; ++i) renderer.draw(ui, canvas, surface, 0.05);
        check((surface.bits()[10] & 0xFFFFFF) == 0x107C10, "accent bar rendered");
        std::size_t lit = 0;
        for (int y = 150; y < 630; ++y) for (int x = 96; x < 948; ++x) lit += (surface.bits()[y * 1920 + x] & 0xFFFFFF) != 0;
        check(lit > 100000, "hero tile rendered");
        if (!capture.empty()) {
            fs::create_directories(capture);
            write_bmp(capture / "home.bmp", surface);
            ui.input(dashboard::Button::rb);
            for (int i = 0; i < 30; ++i) renderer.draw(ui, canvas, surface, 0.05);
            write_bmp(capture / "library.bmp", surface);
            ui.input(dashboard::Button::guide);
            for (int i = 0; i < 30; ++i) renderer.draw(ui, canvas, surface, 0.05);
            write_bmp(capture / "guide.bmp", surface);
            ui.input(dashboard::Button::b);
            ui.input(dashboard::Button::rb);
            for (int i = 0; i < 30; ++i) renderer.draw(ui, canvas, surface, 0.05);
            write_bmp(capture / "settings.bmp", surface);
            ui.input(dashboard::Button::lb); ui.input(dashboard::Button::lb);
        }

        ui.launch(id);
        check(ui.page == dashboard::Page::session && ui.session_title == id, "title launched from the dashboard");
        if (!capture.empty()) {
            for (int i = 0; i < 30; ++i) renderer.draw(ui, canvas, surface, 0.05);
            write_bmp(capture / "session.bmp", surface);
        }
        for (int i = 0; i < 400 && !ui.session_title.empty(); ++i) {
            ui.tick(0.025);
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }
        check(ui.session_title.empty() && ui.page == dashboard::Page::home && ui.profile(), "returned to Home after the title exited");
        check(ui.dialogs.empty() && ui.title(id)->last_result.find("Title exited normally") == 0, "normal exit recorded");
        const auto storage = console::title_storage(root, *ui.profile(), *ui.title(id));
        check(fs::file_size(storage / ".xone-local-profile") == 32, "title ran against the profile's storage root");
    }
    console::State persisted;
    check(console::load(root, persisted, error) && persisted.profiles.size() == 1 && persisted.titles.size() == 1 &&
          persisted.titles[0].play_count == 1, "profile, library and play history persisted");
    CoUninitialize();
    if (failures) { std::cerr << failures << " dashboard ERA check(s) failed\n"; return 1; }
    std::cout << "dashboard ERA lifecycle: passed\n";
    return 0;
}
