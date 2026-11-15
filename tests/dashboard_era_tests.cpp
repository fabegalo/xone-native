// SPDX-License-Identifier: BSD-3-Clause
// Windows x64: the dashboard's real path through the existing ERA runtime.
// Imports an owned fixture package, checks it with `xone-era check`, launches it
// with `xone-era run`, observes its exit and returns; also renders a dashboard
// frame through the GDI surface. Optional: --capture DIR writes BMP frames.
#include "dashboard_windows.h"
#include "xboxone_native/title_catalog.h"
#include "sha256.h"

#include <objbase.h>

#include <chrono>
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
    bool pick_folder(fs::path&) override { return false; }
    void play(dashboard::Sound) override {}
    void rumble() override {}
    bool apply_display(std::uint32_t, std::uint32_t, bool, std::string&) override { return true; }
    void focus_dashboard() override {}
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
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 4) { std::cerr << "usage: dashboard_era_tests <xone-era> <era_component> <AppxManifest.xml> [--capture DIR]\n"; return 2; }
    const fs::path runtime = argv[1], component = argv[2], manifest = argv[3];
    const fs::path capture = argc == 6 && std::wstring(argv[4]) == L"--capture" ? fs::path(argv[5]) : fs::path{};
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
