// SPDX-License-Identifier: BSD-3-Clause
// Portable regressions for the console state and the local dashboard model:
// persistence, navigation, profiles, settings, launch requests, errors, Guide
// and return. The session host is a recording fake; real ERA sessions are
// covered by dashboard_era_tests on Windows x64.
#include "xboxone_native/console_state.h"
#include "xboxone_native/dashboard.h"
#include "xboxone_native/session_supervisor.h"
#include "sha256.h"
#include "dashboard_render.h"
#include "diagnostic_console.h"
#include <algorithm>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <thread>

namespace fs = std::filesystem;
using namespace xboxone;

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++failures; std::cerr << "FAIL: " << what << '\n'; }
}

struct Scratch {
    fs::path path;
    Scratch() {
        std::random_device device;
        path = fs::canonical(fs::temp_directory_path()) / ("xone-dashboard-test-" + std::to_string(device()));
        fs::create_directories(path);
    }
    ~Scratch() { std::error_code ec; fs::remove_all(path, ec); }
};

struct FakePlatform : dashboard::Platform {
    fs::path next_folder;
    int sounds = 0, rumbles = 0, focus = 0, displays = 0;
    bool exit = false;
    bool pick_folder(fs::path& folder) override { folder = next_folder; return !next_folder.empty(); }
    void play(dashboard::Sound) override { ++sounds; }
    void rumble() override { ++rumbles; }
    bool apply_display(std::uint32_t, std::uint32_t, bool, std::string&) override { ++displays; return true; }
    void focus_dashboard() override { ++focus; }
    bool open_folder(const fs::path&) override { return true; }
    std::uint32_t controllers() override { return 1; }
    void request_exit() override { exit = true; }
    std::int64_t now() override { return 1700000000; }
};

struct FakeSession : session::Host {
    std::vector<session::Request> requests;
    bool running = false, fail_start = false, terminated = false;
    int focus_requests = 0;
    std::optional<session::Result> finish;
    bool start(const session::Request& r, std::string& error) override {
        if (fail_start) { error = "cannot start ERA runtime: Win32 2"; return false; }
        requests.push_back(r);
        running = true;
        return true;
    }
    [[nodiscard]] bool active() const override { return running; }
    bool poll(session::Result& result) override {
        if (!running || !finish) return false;
        result = *finish;
        finish.reset();
        running = false;
        return true;
    }
    void terminate() override { terminated = true; finish = session::interpret(1, true, ""); }
    bool focus_title() override { ++focus_requests; return running; }
};

void test_overlay_pixels() {
    struct Resources : dashboard::Resources {
        void text(dashboard::Canvas&, const std::string&, int, int, int, int, int,
                  dashboard::Font, std::uint32_t, dashboard::Align) override {}
        const dashboard::Image* image(const std::string&) override { return nullptr; }
    } resources;
    FakePlatform platform;
    FakeSession session;
    dashboard::Dashboard ui({}, {}, platform, session);
    dashboard::Renderer renderer;
    std::vector<std::uint32_t> pixels(1920 * 1080, 0xFFFFFFFF);
    dashboard::Canvas canvas(pixels.data(), 1920, 1080, 1920);
    ui.guide_open = true;
    renderer.draw(ui, canvas, resources, 1.0, true);
    check(pixels[500 * 1920 + 20] == 0xFF202020 && pixels[500 * 1920 + 1900] == 0x99000000,
          "Guide-only rendering keeps its blade opaque and the game area translucent");
    ui.guide_open = false;
    renderer.draw(ui, canvas, resources, 1.0, true);
    check(pixels[500 * 1920 + 20] == 0 && pixels[500 * 1920 + 1900] == 0,
          "closing Guide clears its overlay without drawing dashboard content");
    canvas.fill(0, 0, 1, 1, 0x8000FF00);
    check(pixels[0] == 0x80008000, "UI blending preserves premultiplied alpha on transparent surfaces");
    ui.page = dashboard::Page::session;
    ui.toasts.push_back({"Starting title", "", 5.0});
    renderer.draw(ui, canvas, resources, 1.0);
    check(ui.items().empty() && std::all_of(pixels.begin(), pixels.end(), [](auto pixel) { return pixel == 0xFF000000; }),
          "launch hands presentation to the title without an invented Playing page, management buttons or launcher toast");
}

std::string sha_of(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::vector<std::uint8_t> bytes{std::istreambuf_iterator<char>(in), {}};
    detail::Sha256 sha;
    sha.update(bytes);
    return sha.finish();
}

console::Title make_title(const fs::path& dir, const std::string& name, std::int64_t played) {
    fs::create_directories(dir);
    const auto exe = dir / (name + ".exe");
    std::ofstream(exe, std::ios::binary) << "MZ fake title " << name;
    console::Title t;
    t.id = sha_of(exe);
    t.name = name;
    t.publisher = "Test Publisher";
    const auto u8 = exe.u8string();
    t.executable = std::string(u8.begin(), u8.end());
    t.executable_size = fs::file_size(exe);
    t.executable_mtime = static_cast<std::int64_t>(fs::last_write_time(exe).time_since_epoch().count());
    t.last_played = played;
    return t;
}

void type(dashboard::Dashboard& ui, const char* text) { for (; *text; ++text) ui.text(static_cast<char32_t>(*text)); }

void test_state_roundtrip() {
    Scratch scratch;
    console::State state;
    state.settings.language = "pt-BR";
    state.settings.ui_volume = 30;
    state.settings.library_folders = {"C:\\Jogos\\Pacotes\tcom tab", "/home/deck/Títulos"};
    state.profiles.push_back({console::new_profile_id(), "Ana\nLinha", 3, 10, 20});
    console::Title t;
    t.id = std::string(64, 'a');
    t.name = "Jogo \\ ção";
    t.executable = "D:\\x\\y.exe";
    t.readiness = console::Readiness::blocked;
    t.check_detail = "Not mapped: 2 imports unresolved";
    t.pinned = true;
    t.play_count = 4;
    state.titles.push_back(t);
    std::string error;
    check(console::save(scratch.path, state, error), "state saves");
    console::State loaded;
    check(console::load(scratch.path, loaded, error) && error.empty(), "state loads");
    check(loaded.settings.language == "pt-BR" && loaded.settings.ui_volume == 30, "settings persist");
    check(loaded.settings.library_folders == state.settings.library_folders, "folders with tabs/Unicode persist");
    check(loaded.profiles.size() == 1 && loaded.profiles[0].name == "Ana\nLinha" && loaded.profiles[0].avatar == 3, "profile persists");
    check(loaded.titles.size() == 1 && loaded.titles[0].name == t.name && loaded.titles[0].pinned &&
          loaded.titles[0].readiness == console::Readiness::blocked && loaded.titles[0].play_count == 4 &&
          loaded.titles[0].check_detail == t.check_detail, "title persists");

    std::ofstream(scratch.path / "console-state.txt", std::ios::trunc) << "xone-console-state 1\nprofile\tnot-hex\tX\t0\t0\t0\n";
    check(console::load(scratch.path, loaded, error) && !error.empty() && loaded.profiles.empty(), "malformed state reported, defaults used");
    bool preserved = false;
    for (const auto& entry : fs::directory_iterator(scratch.path))
        preserved |= entry.path().filename().string().rfind("console-state.txt.corrupt-", 0) == 0;
    check(preserved && !fs::exists(scratch.path / "console-state.txt"), "malformed state preserved, not overwritten");

    const auto& p = state.profiles[0];
    const auto storage = console::title_storage(scratch.path, p, t);
    check(storage == scratch.path / "users" / p.id / "titles" / std::string(16, 'a'), "per-profile title storage layout");
    check(console::prepare_title_storage(storage, p, error), "ERA profile record written");
    std::ifstream record(storage / ".xone-local-profile", std::ios::binary);
    std::vector<unsigned char> bytes{std::istreambuf_iterator<char>(record), {}};
    check(bytes.size() == 32 && std::string(bytes.begin(), bytes.begin() + 8) == "XONEUSR1" && bytes[8] == 1 && bytes[12] == 0,
          "ERA record header matches ProfileRecord");
    char hex[3];
    std::snprintf(hex, sizeof hex, "%02x", bytes.size() == 32 ? bytes[16] : 0);
    check(p.id.substr(0, 2) == hex, "ERA record identity is the profile id");
    const auto name = [&] {
        std::ifstream in(storage / ".xone-local-profile-name", std::ios::binary);
        return std::string{std::istreambuf_iterator<char>(in), {}};
    };
    check(name() == p.name, "profile name written for the title's GameDisplayName");
    auto renamed = p;
    renamed.name = "Jogadora Dois";
    check(console::prepare_title_storage(storage, renamed, error) && name() == "Jogadora Dois", "renamed profile updates the name");
}

void test_interpret() {
    auto r = session::interpret(0, false, "resolved 2, unresolved 0\nrunning title at 0x1 (deadline 2000 ms)\nxone-era: title entry returned\n");
    check(r.normal && r.mapped && r.resolved == 2, "normal title exit");
    r = session::interpret(0, false, "resolved 2, unresolved 0\nmapped: ready for entry (not executed)\n");
    check(!r.normal && r.mapped && r.unresolved == 0 && r.summary == "2 imports resolved, 0 missing", "check ready");
    r = session::interpret(1, false, "resolved 0, unresolved 2\n  missing.dll: 2 missing\nmissing missing.dll!Sleep\nnot mapped\n");
    check(!r.normal && r.unresolved == 2 && r.summary.find("missing.dll!Sleep") != std::string::npos, "unmapped title reports first missing import");
    r = session::interpret(125, false, "xone-era: unimplemented kernelx.dll!Foo ordinal=0 at title+0x10; stopped\n");
    check(r.summary.find("unimplemented kernelx.dll!Foo") != std::string::npos, "missing service stop");
    r = session::interpret(static_cast<int>(0xC0000005u), false, "xone-era: exception 0xC0000005 at title+0x20\n");
    check(r.summary.find("0xC0000005") != std::string::npos && r.summary.find("title+0x20") != std::string::npos, "crash keeps real exception");
    r = session::interpret(124, false, "");
    check(r.summary.find("Time limit") == 0, "deadline");
    r = session::interpret(1, true, "");
    check(r.terminated && !r.normal && r.summary == "Closed by the system UI", "terminated by system UI");
    r = session::interpret(0, true, "xone-era: system UI requested suspend\nxone-era: title suspended\n");
    check(r.terminated && r.summary == "Closed by the system UI after the title suspended", "suspended before quit");
    r = session::interpret(0, true, "xone-era: suspend deadline reached; terminating title\n");
    check(r.summary.find("did not finish suspending") != std::string::npos, "suspend deadline reported");
    r = session::interpret(0, true, "xone-era: title has no Suspending handler; terminating\n");
    check(r.summary.find("no Suspending handler") != std::string::npos, "title without handler reported");
}

void test_dashboard_flow() {
    Scratch scratch;
    FakePlatform platform;
    FakeSession session;
    std::string error;
    {
        dashboard::Dashboard ui(scratch.path, scratch.path / "xone-era.exe", platform, session);
        check(ui.start(error), "dashboard starts without SystemOS or titles");
        check(ui.page == dashboard::Page::sign_in && ui.items().size() == 1, "first boot offers only Add new");
        ui.input(dashboard::Button::a);
        check(ui.page == dashboard::Page::keyboard, "Add new opens on-screen keyboard");
        ui.input(dashboard::Button::a); // focused key "Q" (shift starts on)
        type(ui, "ui");
        ui.input(dashboard::Button::x); // controller backspace
        check(ui.keyboard_text == "Qu", "keyboard typing and delete");
        ui.input(dashboard::Button::menu); // Done
        check(ui.page == dashboard::Page::home && ui.profile() && ui.profile()->name == "Qu", "profile created and signed in");
        ui.input(dashboard::Button::guide);
        check(ui.guide_open, "Guide opens");
        ui.input(dashboard::Button::b);
        check(!ui.guide_open, "Guide closes");
        ui.input(dashboard::Button::left);
        check(ui.guide_open, "left edge of Home opens Guide");
        ui.input(dashboard::Button::down); ui.input(dashboard::Button::down); // Home -> My games & apps
        ui.input(dashboard::Button::a);
        check(!ui.guide_open && ui.page == dashboard::Page::library, "Guide navigates to My games & apps");
        ui.input(dashboard::Button::rb);
        check(ui.page == dashboard::Page::settings, "bumper switches to Settings");
        ui.input(dashboard::Button::down); ui.input(dashboard::Button::down); // Profile -> Display -> Audio
        check(ui.settings_category == 2, "focusing a category shows its options");
        ui.input(dashboard::Button::right);
        ui.input(dashboard::Button::a);
        check(!ui.state.settings.ui_sounds, "interface sounds toggled");
        ui.input(dashboard::Button::down);
        ui.input(dashboard::Button::left);
        check(ui.state.settings.ui_volume == 60, "volume adjusted with left");
        ui.input(dashboard::Button::b);
        for (int i = 0; i < 2; ++i) ui.input(dashboard::Button::down);
        ui.input(dashboard::Button::right);
        ui.input(dashboard::Button::down);
        ui.input(dashboard::Button::a);
        check(ui.state.settings.language == "pt-BR" && ui.translate("Home") == "Início", "language switch");
    }
    console::State reloaded;
    check(console::load(scratch.path, reloaded, error) && reloaded.profiles.size() == 1 && !reloaded.settings.ui_sounds &&
          reloaded.settings.ui_volume == 60 && reloaded.settings.language == "pt-BR" &&
          reloaded.settings.last_profile == reloaded.profiles[0].id, "profile and settings persist across restarts");

    // Library, launch, return.
    auto recent = make_title(scratch.path / "pkg" / "a", "Recent", 50);
    auto older = make_title(scratch.path / "pkg" / "b", "Older", 10);
    reloaded.titles = {older, recent};
    check(console::save(scratch.path, reloaded, error), "library saved");
    dashboard::Dashboard ui(scratch.path, scratch.path / "xone-era.exe", platform, session);
    check(ui.start(error), "dashboard restarts");
    check(ui.focused(ui.items())->key == "profile:" + reloaded.profiles[0].id, "last profile focused at sign-in");
    ui.input(dashboard::Button::a);
    // Startup queues compatibility checks of unchecked titles; finish them first.
    ui.tick(0.01);
    check(!session.requests.empty() && session.requests.back().mode == session::Mode::check, "unchecked title queued for compatibility check");
    while (session.running) {
        session.finish = session::interpret(0, false, "resolved 5, unresolved 0\nmapped: ready for entry (not executed)\n");
        ui.tick(0.01);
        ui.tick(0.01);
    }
    check(ui.title(recent.id)->readiness == console::Readiness::ready && ui.title(older.id)->resolved == 5, "checks recorded");
    const auto hero = ui.items().front();
    check(hero.kind == dashboard::Kind::hero && hero.label == "Recent", "hero shows last played title");
    ui.input(dashboard::Button::right);
    check(ui.focused(ui.items())->label == "Older", "right moves from hero to recent tile");
    ui.input(dashboard::Button::left);
    session.requests.clear();
    ui.input(dashboard::Button::a);
    check(session.requests.size() == 1 && ui.page == dashboard::Page::session, "A on hero launches through the supervisor");
    if (!session.requests.empty()) {
        const auto& r = session.requests[0];
        const auto expected = console::title_storage(scratch.path, *ui.profile(), *ui.title(recent.id));
        check(r.mode == session::Mode::run && r.storage == expected && r.time_limit_ms == 0 &&
              r.runtime == scratch.path / "xone-era.exe", "launch request uses ERA runtime and profile storage");
        check(fs::file_size(expected / ".xone-local-profile") == 32, "ERA profile record prepared before launch");
    }
    check(ui.title(recent.id)->play_count == 1, "play count recorded");
    check(ui.active_surface() == session::SurfaceMode::title, "title owns presentation and input after launch");
    const auto focus_before = session.focus_requests;
    ui.input(dashboard::Button::guide);
    check(ui.active_surface() == session::SurfaceMode::guide, "Guide takes presentation and input ownership");
    ui.input(dashboard::Button::b);
    check(!ui.guide_open && session.focus_requests == focus_before + 1 && session.running,
          "B closes Guide and returns focus to the same running title");
    ui.input(dashboard::Button::guide);
    ui.input(dashboard::Button::guide);
    check(!ui.guide_open && session.focus_requests == focus_before + 2 && session.running,
          "Xbox button closes Guide and returns focus to the same running title");
    ui.input(dashboard::Button::guide);
    for (const auto& item : ui.guide_items()) if (item.key == "guide:home") item.activate();
    check(ui.page == dashboard::Page::home && session.running && ui.active_surface() == session::SurfaceMode::system_ui,
          "Home owns presentation and input while preserving the active title");
    const auto launches = session.requests.size();
    ui.launch(recent.id);
    check(ui.page == dashboard::Page::session && session.requests.size() == launches,
          "returning from Home reuses the running title and restores its active surface");
    ui.input(dashboard::Button::guide);
    for (const auto& item : ui.guide_items()) if (item.key == "guide:home") item.activate();
    ui.input(dashboard::Button::guide);
    for (const auto& item : ui.guide_items()) if (item.key == "guide:return") item.activate();
    check(ui.page == dashboard::Page::session && !ui.guide_open && session.requests.size() == launches,
          "Guide return selects the title surface without starting another process");
    ui.input(dashboard::Button::guide);
    bool has_quit = false;
    for (const auto& item : ui.guide_items()) has_quit |= item.key == "guide:quit";
    check(ui.guide_open && has_quit, "Guide offers quit while a title runs");
    for (int i = 0; i < 4; ++i) ui.input(dashboard::Button::down);
    check(ui.focused(ui.guide_items())->key == "guide:quit", "Guide focus reaches quit");
    ui.input(dashboard::Button::a);
    check(session.terminated, "quit terminates the supervised title");
    ui.tick(0.01);
    check(ui.page == dashboard::Page::home && ui.session_title.empty() && ui.profile(), "returns to Home with profile");
    check(!ui.toasts.empty() && ui.dialogs.empty(), "quit reported as notification");

    // Real failure output is shown, not a simulated success.
    session.terminated = false;
    ui.input(dashboard::Button::a);
    session.finish = session::interpret(1, false, "resolved 1, unresolved 1\nmissing kernelx.dll!Foo\nnot mapped\n");
    ui.tick(0.01);
    check(ui.dialogs.size() == 1 && ui.dialogs[0].lines[0].find("kernelx.dll!Foo") != std::string::npos &&
          ui.dialogs[0].lines.size() > 2, "failed title shows its real error and output");
    check(ui.title(recent.id)->last_result.find("Not mapped") == 0, "failure recorded as last result");
    ui.input(dashboard::Button::a);
    check(ui.dialogs.empty(), "dialog dismissed");

    session.fail_start = true;
    const auto before = ui.page;
    ui.input(dashboard::Button::a);
    check(ui.page == before && ui.dialogs.size() == 1 && ui.dialogs[0].lines[0].find("Win32 2") != std::string::npos,
          "start failure is reported and nothing runs");
    ui.input(dashboard::Button::a);
    session.fail_start = false;

    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    std::ofstream(fs::path(std::u8string(recent.executable.begin(), recent.executable.end())), std::ios::app) << "changed";
    const auto count = session.requests.size();
    ui.input(dashboard::Button::a);
    check(session.requests.size() == count && ui.dialogs.size() == 1 &&
          ui.dialogs[0].lines[0].find("changed since import") != std::string::npos, "modified title refused before launch");
    ui.input(dashboard::Button::a);

    // Manage -> remove from library.
    ui.input(dashboard::Button::right);
    ui.input(dashboard::Button::menu);
    check(ui.page == dashboard::Page::details && ui.selected_title == older.id, "Menu opens Manage game");
    for (int i = 0; i < 3; ++i) ui.input(dashboard::Button::down);
    ui.input(dashboard::Button::a);
    ui.input(dashboard::Button::left);
    ui.input(dashboard::Button::a);
    check(!ui.title(older.id) && ui.page == dashboard::Page::home, "title removed after confirmation");
    console::State final_state;
    check(console::load(scratch.path, final_state, error) && final_state.titles.size() == 1, "removal persisted");
}
} // namespace

int main() {
    {
        diagnostics::LogBuffer log;
        log.append("original GPU ");log.append("bytes\r\npartial");
        check(log.lines.size()==1 && log.lines.front()=="original GPU bytes" && log.fragment=="partial",
              "live console retains incomplete writes until the original line is complete");
        log.append("\n");
        for(std::size_t i=0;i<diagnostics::LogBuffer::line_limit;++i)log.append("line\n");
        check(log.lines.size()==diagnostics::LogBuffer::line_limit && log.omitted==2,"console history reports bounded eviction");
        log.clear();log.append(std::string(diagnostics::LogBuffer::byte_limit+32,'x'));
        check(log.lines.size()<=diagnostics::LogBuffer::line_limit && log.fragment.size()<diagnostics::LogBuffer::fragment_limit,
              "unterminated guest output cannot grow the console without bound");
        log.clear();check(log.lines.empty() && log.fragment.empty() && !log.omitted,"clear view resets only the retained model");
    }
    test_overlay_pixels();
    test_state_roundtrip();
    test_interpret();
    test_dashboard_flow();
    if (failures) { std::cerr << failures << " dashboard check(s) failed\n"; return 1; }
    std::cout << "dashboard regressions: passed\n";
    return 0;
}
