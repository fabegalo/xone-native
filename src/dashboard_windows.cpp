// SPDX-License-Identifier: BSD-3-Clause
// Windows x64 host adapter for the local dashboard.
#include "dashboard_windows.h"
#include "xboxone_native/presentation.h"

#include <objbase.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <xinput.h>
#include <mmsystem.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <span>
#include <vector>

namespace xboxone::dashboard {
using Microsoft::WRL::ComPtr;
namespace {
std::wstring wide(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
    return out;
}
} // namespace

GdiSurface::GdiSurface() { dc_ = CreateCompatibleDC(nullptr); }

GdiSurface::~GdiSurface() {
    for (auto& [key, font] : fonts_) DeleteObject(font);
    if (previous_) SelectObject(dc_, previous_);
    if (bitmap_) DeleteObject(bitmap_);
    if (dc_) DeleteDC(dc_);
}

bool GdiSurface::resize(int width, int height) {
    if (width == width_ && height == height_ && bitmap_) return true;
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof info.bmiHeader;
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height; // top-down rows match the presenter's pitch
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP bitmap = CreateDIBSection(dc_, &info, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!bitmap) return false;
    if (previous_) SelectObject(dc_, previous_);
    if (bitmap_) DeleteObject(bitmap_);
    bitmap_ = bitmap;
    previous_ = SelectObject(dc_, bitmap_);
    bits_ = static_cast<std::uint32_t*>(bits);
    width_ = width;
    height_ = height;
    return true;
}

HFONT GdiSurface::font(int size, Font face) {
    const auto key = std::make_pair(size, static_cast<int>(face));
    if (const auto found = fonts_.find(key); found != fonts_.end()) return found->second;
    const wchar_t* family = L"Segoe UI";
    int weight = FW_NORMAL;
    switch (face) {
        case Font::light: family = L"Segoe UI Light"; weight = FW_LIGHT; break;
        case Font::semibold: family = L"Segoe UI Semibold"; weight = FW_SEMIBOLD; break;
        case Font::icons: family = L"Segoe MDL2 Assets"; break;
        case Font::mono: family = L"Consolas"; break;
        default: break;
    }
    HFONT font = CreateFontW(-size, 0, 0, 0, weight, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_TT_PRECIS, CLIP_DEFAULT_PRECIS,
                             CLEARTYPE_QUALITY, DEFAULT_PITCH, family);
    fonts_[key] = font;
    return font;
}

void GdiSurface::text(Canvas&, const std::string& utf8, int x, int y, int w, int h, int size, Font face, std::uint32_t rgb, Align align) {
    if (utf8.empty() || w <= 0 || h <= 0) return;
    GdiFlush(); // keep CPU canvas writes and GDI glyphs in painter's order
    const auto text = wide(utf8);
    SelectObject(dc_, font(size, face));
    SetTextColor(dc_, RGB((rgb >> 16) & 0xFF, (rgb >> 8) & 0xFF, rgb & 0xFF));
    SetBkMode(dc_, TRANSPARENT);
    RECT r{x, y, x + w, y + h};
    UINT flags = DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX | DT_END_ELLIPSIS;
    flags |= align == Align::center ? DT_CENTER : align == Align::right ? DT_RIGHT : DT_LEFT;
    DrawTextW(dc_, text.c_str(), static_cast<int>(text.size()), &r, flags);
    GdiFlush();
}

const Image* GdiSurface::image(const std::string& utf8_path) {
    if (utf8_path.empty()) return nullptr;
    if (const auto found = images_.find(utf8_path); found != images_.end()) return found->second ? &*found->second : nullptr;
    auto& slot = images_[utf8_path];
    ComPtr<IWICImagingFactory> factory;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICBitmapScaler> scaler;
    ComPtr<IWICFormatConverter> converter;
    UINT w = 0, h = 0;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) ||
        FAILED(factory->CreateDecoderFromFilename(wide(utf8_path).c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder)) ||
        FAILED(decoder->GetFrame(0, &frame)) || FAILED(frame->GetSize(&w, &h)) || !w || !h)
        return nullptr;
    IWICBitmapSource* source = frame.Get();
    if (w > 512 || h > 512) { // tiles never need more; bounds memory per cached image
        const double k = 512.0 / std::max(w, h);
        const UINT sw = std::max(1u, static_cast<UINT>(w * k)), sh = std::max(1u, static_cast<UINT>(h * k));
        if (FAILED(factory->CreateBitmapScaler(&scaler)) || FAILED(scaler->Initialize(frame.Get(), sw, sh, WICBitmapInterpolationModeFant)))
            return nullptr;
        source = scaler.Get();
        w = sw; h = sh;
    }
    if (FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(source, GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
        return nullptr;
    Image img;
    img.width = static_cast<int>(w);
    img.height = static_cast<int>(h);
    img.pixels.resize(static_cast<std::size_t>(w) * h);
    if (FAILED(converter->CopyPixels(nullptr, w * 4, w * h * 4, reinterpret_cast<BYTE*>(img.pixels.data())))) return nullptr;
    slot = std::move(img);
    return &*slot;
}

namespace {
using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
using XInputSetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_VIBRATION*);
constexpr WORD guide_button = 0x0400; // reported only by XInputGetStateEx (xinput1_4 ordinal 100)
constexpr int hotkey_guide = 0x584F;

struct Pads {
    HMODULE module = LoadLibraryExW(L"xinput1_4.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    XInputGetStateFn get = module ? reinterpret_cast<XInputGetStateFn>(GetProcAddress(module, reinterpret_cast<LPCSTR>(100))) : nullptr;
    XInputGetStateFn plain = module ? reinterpret_cast<XInputGetStateFn>(GetProcAddress(module, "XInputGetState")) : nullptr;
    XInputSetStateFn set = module ? reinterpret_cast<XInputSetStateFn>(GetProcAddress(module, "XInputSetState")) : nullptr;
    std::array<WORD, XUSER_MAX_COUNT> held{};
    std::array<bool, XUSER_MAX_COUNT> connected{};
    std::array<double, 4> repeat{}; // up, down, left, right
    double rumble_until = 0;
    ~Pads() { if (module) FreeLibrary(module); }
};

class WavBank {
public:
    void play(Sound sound, std::uint32_t volume) {
        auto& wav = sounds_[static_cast<int>(sound)];
        if (volumes_[static_cast<int>(sound)] != volume || wav.empty()) { build(sound, volume, wav); volumes_[static_cast<int>(sound)] = volume; }
        PlaySoundW(reinterpret_cast<LPCWSTR>(wav.data()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT);
    }
private:
    // Interface sounds are synthesized here: short shaped sine sweeps, no sampled assets.
    static void build(Sound sound, std::uint32_t volume, std::vector<std::uint8_t>& wav) {
        struct Tone { double f0, f1, seconds; };
        std::vector<Tone> tones;
        switch (sound) {
            case Sound::move: tones = {{1450, 1500, 0.028}}; break;
            case Sound::select: tones = {{880, 990, 0.05}, {1320, 1320, 0.07}}; break;
            case Sound::back: tones = {{990, 700, 0.07}}; break;
            case Sound::open: tones = {{520, 1040, 0.11}}; break;
            case Sound::error: tones = {{220, 200, 0.09}, {180, 160, 0.12}}; break;
        }
        constexpr int rate = 44100;
        std::vector<std::int16_t> samples;
        for (const auto& t : tones) {
            const int n = static_cast<int>(t.seconds * rate);
            double phase = 0;
            for (int i = 0; i < n; ++i) {
                const double k = static_cast<double>(i) / n;
                phase += 2 * 3.14159265358979 * (t.f0 + (t.f1 - t.f0) * k) / rate;
                const double envelope = std::min(1.0, i / 120.0) * (1 - k) * (1 - k);
                samples.push_back(static_cast<std::int16_t>(std::sin(phase) * envelope * 9000 * volume / 100.0));
            }
        }
        const std::uint32_t data = static_cast<std::uint32_t>(samples.size() * 2);
        wav.assign(44 + data, 0);
        const auto put32 = [&](std::size_t at, std::uint32_t v) { std::memcpy(&wav[at], &v, 4); };
        const auto put16 = [&](std::size_t at, std::uint16_t v) { std::memcpy(&wav[at], &v, 2); };
        std::memcpy(&wav[0], "RIFF", 4); put32(4, 36 + data); std::memcpy(&wav[8], "WAVEfmt ", 8);
        put32(16, 16); put16(20, 1); put16(22, 1); put32(24, rate); put32(28, rate * 2); put16(32, 2); put16(34, 16);
        std::memcpy(&wav[36], "data", 4); put32(40, data);
        std::memcpy(&wav[44], samples.data(), data);
    }
    std::array<std::vector<std::uint8_t>, 5> sounds_;
    std::array<std::uint32_t, 5> volumes_{};
};

class Host final : public Platform {
public:
    Host(presentation::Presenter& presenter, GdiSurface& surface) : presenter_(presenter), surface_(surface) {}
    Dashboard* ui = nullptr;
    bool exit = false;
    Pads pads;

    HWND window() const { return static_cast<HWND>(presenter_.native_window()); }

    bool pick_folder(std::filesystem::path& folder) override {
        ComPtr<IFileOpenDialog> dialog;
        if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return false;
        DWORD options = 0;
        dialog->GetOptions(&options);
        dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST);
        dialog->SetTitle(L"xone-native: choose a folder with extracted title packages");
        if (FAILED(dialog->Show(window()))) return false;
        ComPtr<IShellItem> item;
        PWSTR path = nullptr;
        if (FAILED(dialog->GetResult(&item)) || FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) return false;
        folder = path;
        CoTaskMemFree(path);
        return true;
    }

    void play(Sound sound) override {
        if (ui && ui->state.settings.ui_sounds && ui->state.settings.ui_volume) wavs_.play(sound, ui->state.settings.ui_volume);
    }

    void rumble() override {
        if (!pads.set || (ui && !ui->state.settings.vibration)) return;
        for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i)
            if (pads.connected[i]) { XINPUT_VIBRATION v{30000, 30000}; pads.set(i, &v); }
        pads.rumble_until = seconds() + 0.09;
    }

    void stop_rumble() {
        if (!pads.rumble_until || seconds() < pads.rumble_until) return;
        pads.rumble_until = 0;
        for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) if (pads.connected[i]) { XINPUT_VIBRATION off{}; pads.set(i, &off); }
    }

    bool apply_display(std::uint32_t width, std::uint32_t height, bool fullscreen, std::string& error) override {
        HWND hwnd = window();
        if (!hwnd) { error = "dashboard window is closed"; return false; }
        if (presenter_.stats().width != width || presenter_.stats().height != height) {
            if (!presenter_.resize(width, height, error)) return false;
        }
        if (!surface_.resize(static_cast<int>(width), static_cast<int>(height))) { error = "cannot allocate the dashboard surface"; return false; }
        MONITORINFO monitor{sizeof monitor};
        GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &monitor);
        if (fullscreen) {
            SetWindowLongPtrW(hwnd, GWL_STYLE, WS_POPUP | WS_VISIBLE);
            const auto& m = monitor.rcMonitor;
            SetWindowPos(hwnd, HWND_TOP, m.left, m.top, m.right - m.left, m.bottom - m.top, SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        } else {
            SetWindowLongPtrW(hwnd, GWL_STYLE, WS_OVERLAPPEDWINDOW | WS_VISIBLE);
            const auto& work = monitor.rcWork;
            // Client area keeps the frame aspect and fits the work area (DXGI scales the swapchain).
            int cw = static_cast<int>(width), ch = static_cast<int>(height);
            const int max_w = (work.right - work.left) * 9 / 10, max_h = (work.bottom - work.top) * 9 / 10;
            if (cw > max_w || ch > max_h) {
                const double k = std::min(static_cast<double>(max_w) / cw, static_cast<double>(max_h) / ch);
                cw = static_cast<int>(cw * k);
                ch = static_cast<int>(ch * k);
            }
            RECT r{0, 0, cw, ch};
            AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW, FALSE, 0);
            const int ww = r.right - r.left, wh = r.bottom - r.top;
            SetWindowPos(hwnd, nullptr, work.left + ((work.right - work.left) - ww) / 2, work.top + ((work.bottom - work.top) - wh) / 2,
                         ww, wh, SWP_NOZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);
        }
        if (ui) ++ui->serial;
        return true;
    }

    void focus_dashboard() override {
        HWND hwnd = window();
        if (!hwnd) return;
        if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
        if (!SetForegroundWindow(hwnd)) SwitchToThisWindow(hwnd, TRUE);
    }

    bool open_folder(const std::filesystem::path& folder) override {
        std::error_code ec;
        std::filesystem::create_directories(folder, ec);
        return reinterpret_cast<INT_PTR>(ShellExecuteW(window(), L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) > 32;
    }

    std::uint32_t controllers() override {
        std::uint32_t n = 0;
        for (const bool c : pads.connected) n += c;
        return n;
    }

    void request_exit() override { exit = true; }
    std::int64_t now() override { return static_cast<std::int64_t>(std::time(nullptr)); }

    static double seconds() {
        return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    }

    // Controller polling with edge detection and held-direction repeat.
    void poll_pads(bool foreground) {
        const auto read = pads.get ? pads.get : pads.plain;
        if (!read) return;
        const double t = seconds();
        const float zone = (ui ? ui->state.settings.stick_deadzone : 40) / 100.0f * 32767.0f;
        for (DWORD i = 0; i < XUSER_MAX_COUNT; ++i) {
            XINPUT_STATE state{};
            pads.connected[i] = read(i, &state) == ERROR_SUCCESS;
            if (!pads.connected[i]) { pads.held[i] = 0; continue; }
            WORD buttons = state.Gamepad.wButtons;
            const auto& g = state.Gamepad;
            if (g.sThumbLY > zone) buttons |= XINPUT_GAMEPAD_DPAD_UP;
            if (g.sThumbLY < -zone) buttons |= XINPUT_GAMEPAD_DPAD_DOWN;
            if (g.sThumbLX < -zone) buttons |= XINPUT_GAMEPAD_DPAD_LEFT;
            if (g.sThumbLX > zone) buttons |= XINPUT_GAMEPAD_DPAD_RIGHT;
            const WORD pressed = buttons & ~pads.held[i];
            pads.held[i] = buttons;
            if (!ui) continue;
            if (pressed & guide_button) { ui->controller_hints = true; ui->input(Button::guide); }
            if (!foreground) continue; // a running title owns the controller; only the Guide reaches the dashboard
            static constexpr std::pair<int, Button> map[] = {
                {XINPUT_GAMEPAD_A, Button::a}, {XINPUT_GAMEPAD_B, Button::b}, {XINPUT_GAMEPAD_X, Button::x},
                {XINPUT_GAMEPAD_Y, Button::y}, {XINPUT_GAMEPAD_START, Button::menu}, {XINPUT_GAMEPAD_BACK, Button::view},
                {XINPUT_GAMEPAD_LEFT_SHOULDER, Button::lb}, {XINPUT_GAMEPAD_RIGHT_SHOULDER, Button::rb}};
            for (const auto& [mask, button] : map) if (pressed & mask) { ui->controller_hints = true; ui->input(button); }
            static constexpr std::pair<int, Button> directions[] = {
                {XINPUT_GAMEPAD_DPAD_UP, Button::up}, {XINPUT_GAMEPAD_DPAD_DOWN, Button::down},
                {XINPUT_GAMEPAD_DPAD_LEFT, Button::left}, {XINPUT_GAMEPAD_DPAD_RIGHT, Button::right}};
            for (std::size_t d = 0; d < 4; ++d) {
                const auto [mask, button] = directions[d];
                if (pressed & mask) { pads.repeat[d] = t + 0.38; ui->controller_hints = true; ui->input(button); }
                else if ((buttons & mask) && t >= pads.repeat[d]) { pads.repeat[d] = t + 0.11; ui->input(button); }
            }
        }
    }

    void key(std::uint32_t vk) {
        if (!ui) return;
        ui->controller_hints = false;
        const bool typing = ui->page == Page::keyboard && ui->dialogs.empty() && !ui->guide_open;
        switch (vk) {
            case VK_UP: ui->input(Button::up); break;
            case VK_DOWN: ui->input(Button::down); break;
            case VK_LEFT: ui->input(Button::left); break;
            case VK_RIGHT: ui->input(Button::right); break;
            case VK_RETURN: typing ? ui->text(U'\r') : ui->input(Button::a); break;
            case VK_SPACE: if (!typing) ui->input(Button::a); break;
            case VK_BACK: typing ? ui->text(U'\b') : ui->input(Button::b); break;
            case VK_ESCAPE: ui->input(Button::b); break;
            case VK_DELETE: ui->input(Button::x); break;
            case VK_INSERT: ui->input(Button::y); break;
            case VK_F2: case VK_APPS: ui->input(Button::menu); break;
            case VK_F3: ui->input(Button::view); break;
            case VK_HOME: case VK_F1: ui->input(Button::guide); break;
            case VK_PRIOR: ui->input(Button::lb); break;
            case VK_NEXT: ui->input(Button::rb); break;
            default: break;
        }
    }

private:
    presentation::Presenter& presenter_;
    GdiSurface& surface_;
    WavBank wavs_;
};

std::filesystem::path executable_directory() {
    std::wstring path(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    path.resize(n);
    return std::filesystem::path(path).parent_path();
}
} // namespace

int run_host() {
    // A console created only for this launch (Explorer double-click) is not part of the UI.
    DWORD processes[2];
    if (GetConsoleProcessList(processes, 2) == 1) FreeConsole();
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) return 1;
    struct Com { ~Com() { CoUninitialize(); } } com;

    presentation::Presenter presenter;
    presentation::PresenterConfig config;
    config.window_title = "xone-native";
    config.width = 1920;
    config.height = 1080;
    config.swapchain_format = presentation::PixelFormat::bgra8_unorm;
    config.visible = true;
    config.vsync = true;
    std::string error;
    if (!presenter.initialize(config, error)) {
        MessageBoxW(nullptr, wide("Presentation failed: " + error).c_str(), L"xone-native", MB_ICONERROR);
        return 1;
    }
    GdiSurface surface;
    if (!surface.resize(1920, 1080)) return 1;
    Host host(presenter, surface);
    const auto session = session::create_host();
    Dashboard ui(console::default_root(), executable_directory() / L"xone-era.exe", host, *session);
    host.ui = &ui;
    if (!ui.start(error)) {
        MessageBoxW(host.window(), wide("Console state unavailable: " + error).c_str(), L"xone-native", MB_ICONERROR);
        return 1;
    }
    SetForegroundWindow(host.window());

    Renderer renderer;
    std::vector<presentation::WindowInputEvent> events;
    std::uint64_t drawn = ~0ULL;
    bool animating = true, hotkey = false;
    double last = Host::seconds(), last_clock = 0;
    for (;;) {
        bool quit = false;
        if (!presenter.pump_messages(quit, error) || quit || host.exit) break;
        presenter.take_input(events);
        for (const auto& e : events) {
            if (e.kind == presentation::WindowInputEvent::Kind::key_down) host.key(e.code);
            else if (e.kind == presentation::WindowInputEvent::Kind::character && e.code >= 0x20 && !(e.code >= 0xD800 && e.code <= 0xDFFF))
                ui.text(static_cast<char32_t>(e.code));
            else if (e.kind == presentation::WindowInputEvent::Kind::hotkey && e.code == hotkey_guide) ui.input(Button::guide);
        }
        host.poll_pads(GetForegroundWindow() == host.window());
        host.stop_rumble();
        const double now = Host::seconds();
        const double dt = std::min(0.1, now - last);
        last = now;
        ui.tick(dt);
        // Ctrl+Home reaches the Guide while a title window has the keyboard.
        const bool want_hotkey = !ui.session_title.empty();
        if (want_hotkey != hotkey) {
            // If another app owns Ctrl+Home, registration fails and the controller Guide still works.
            if (want_hotkey) RegisterHotKey(nullptr, hotkey_guide, MOD_CONTROL | MOD_NOREPEAT, VK_HOME);
            else UnregisterHotKey(nullptr, hotkey_guide);
            hotkey = want_hotkey;
        }
        if (drawn != ui.serial || animating || now - last_clock > 1.0) {
            auto canvas = surface.canvas();
            animating = renderer.draw(ui, canvas, surface, dt);
            GdiFlush();
            presentation::ExternalFrameView frame;
            frame.pixels = std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(surface.bits()),
                                                         static_cast<std::size_t>(surface.width()) * surface.height() * 4);
            frame.width = static_cast<std::uint32_t>(surface.width());
            frame.height = static_cast<std::uint32_t>(surface.height());
            frame.row_pitch_bytes = frame.width * 4;
            frame.format = presentation::PixelFormat::bgra8_unorm;
            frame.provenance = {presentation::FrameOrigin::local_recreation, "xone_local_dashboard", "", 0};
            if (!presenter.present_frame(frame, error) && !presenter.recover_swapchain(error)) {
                MessageBoxW(host.window(), wide("Presentation failed: " + error).c_str(), L"xone-native", MB_ICONERROR);
                break;
            }
            drawn = ui.serial;
            last_clock = now;
        } else {
            MsgWaitForMultipleObjects(0, nullptr, FALSE, 8, QS_ALLINPUT);
        }
    }
    if (hotkey) UnregisterHotKey(nullptr, hotkey_guide);
    ui.quit_title();
    presenter.shutdown();
    return 0;
}

} // namespace xboxone::dashboard
