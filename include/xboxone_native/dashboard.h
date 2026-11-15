// SPDX-License-Identifier: BSD-3-Clause
#pragma once
// Locally recreated Xbox One-style dashboard (maintainer-approved interim system
// UI). Portable model: pages, focus, Guide, profiles, settings and library over
// the shared console state; titles start through the session supervisor. Its
// frames are FrameOrigin::local_recreation, never SystemOS output.
#include "xboxone_native/console_state.h"
#include "xboxone_native/session_supervisor.h"

#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace xboxone::dashboard {

enum class Button { up, down, left, right, a, b, x, y, menu, view, guide, lb, rb };
enum class Sound { move, select, back, error, open };

// Host adapter (Windows today). Every call performs a real host action.
class Platform {
public:
    virtual ~Platform() = default;
    virtual bool pick_folder(std::filesystem::path& folder) = 0; // false: cancelled/unavailable
    virtual void play(Sound) = 0;
    virtual void rumble() = 0;
    virtual bool apply_display(std::uint32_t width, std::uint32_t height, bool fullscreen, std::string& error) = 0;
    virtual void focus_dashboard() = 0;
    virtual bool open_folder(const std::filesystem::path& folder) = 0;
    virtual std::uint32_t controllers() = 0;
    virtual void request_exit() = 0;
    virtual std::int64_t now() = 0; // Unix seconds
};

struct Rect { int x = 0, y = 0, w = 0, h = 0; };

enum class Kind { hero, tile, action_tile, profile, menu, option, key, button, field };

// One focusable element in logical 1920x1080 coordinates. Layout, navigation
// and rendering share it, so nothing is drawn as interactive without an action.
struct Item {
    std::string key;
    Kind kind = Kind::tile;
    Rect rect;
    std::string label, detail, value, image, glyph;
    std::uint32_t color = 0;
    bool selected = false;
    std::function<void()> activate;
    std::function<void(int)> adjust; // left/right on options
    std::function<void()> secondary; // X button
    std::function<void()> manage;    // Menu button
};

enum class Page { sign_in, home, library, details, settings, keyboard, session };

struct Dialog {
    std::string title;
    std::vector<std::string> lines;
    bool monospace = false;
    std::vector<std::string> buttons;
    int focus = 0;
    std::function<void(int)> choose; // index; B chooses the last button
};

struct Toast { std::string text, glyph; double remaining = 0; };

class Dashboard {
public:
    Dashboard(std::filesystem::path root, std::filesystem::path runtime, Platform&, session::Host&);

    // Loads the console state; a malformed state is preserved and reported in a dialog.
    bool start(std::string& error);
    void input(Button);
    void text(char32_t); // physical keyboard typing for the on-screen keyboard
    void tick(double seconds);

    // View for renderers.
    [[nodiscard]] std::vector<Item> items() const;
    [[nodiscard]] std::vector<Item> guide_items() const;
    [[nodiscard]] const Item* focused(const std::vector<Item>&) const;
    [[nodiscard]] std::string translate(const char* english) const;
    [[nodiscard]] std::string header() const;

    Page page = Page::sign_in;
    bool guide_open = false;
    std::string guide_focus;
    std::vector<Dialog> dialogs; // stacked; back() is shown
    std::deque<Toast> toasts;
    std::uint64_t serial = 0; // increments on every visible change
    int scroll = 0;           // logical vertical scroll of the current page

    console::State state;
    std::string profile_id;          // signed-in profile
    std::string selected_title;      // details page
    std::string session_title;       // running title (run mode)
    std::string checking_title;      // running compatibility check
    int settings_category = 0;
    std::string keyboard_text;
    bool keyboard_shift = false;
    std::string keyboard_purpose;    // "create" or "rename"
    bool controller_hints = false;   // last input came from a controller

    [[nodiscard]] const console::Profile* profile() const;
    [[nodiscard]] const console::Title* title(const std::string& id) const;
    [[nodiscard]] std::vector<const console::Title*> sorted_titles() const;
    [[nodiscard]] const std::filesystem::path& root() const { return root_; }

    // Actions (also used by tests).
    void sign_in(const std::string& id);
    void launch(const std::string& id);
    void check(const std::string& id);
    void import_folder(const std::filesystem::path& folder);
    void rescan();
    void go(Page);
    void back();
    void quit_title();

private:
    void persist();
    void notify(const std::string& text, char32_t glyph = 0xE946);
    void error_dialog(const std::string& title, const std::string& summary, const std::vector<std::string>& tail = {});
    void confirm(const std::string& title, const std::string& line, const std::string& yes, std::function<void()> action);
    void open_keyboard(const std::string& purpose, const std::string& initial);
    void finish_keyboard();
    void move(Button);
    void finished(const session::Result&);
    void next_check();
    std::vector<Item> page_items(Page) const;
    std::vector<Item> settings_options() const;
    std::string& focus_key();

    std::filesystem::path root_, runtime_;
    Platform& platform_;
    session::Host& session_;
    std::vector<Page> history_;
    std::vector<std::string> focus_keys_ = std::vector<std::string>(7);
    std::deque<std::string> check_queue_;
    std::string pending_launch_;
    session::Mode session_mode_ = session::Mode::run;
};

// Windows x64 host (window, presenter, keyboard/XInput, sounds, folder picker,
// supervised xone-era sessions). Returns the process exit code; other hosts
// report the missing adapter explicitly.
int run_host();

} // namespace xboxone::dashboard
