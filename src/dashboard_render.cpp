// SPDX-License-Identifier: BSD-3-Clause
// Visual language inspired by the 2015 Xbox One dashboard (dark theme, flat
// tiles, accent focus, Guide blade); all shapes, colours and icons are drawn
// here or come from installed host fonts. No Microsoft image assets.
#include "dashboard_render.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

namespace xboxone::dashboard {
namespace {
constexpr std::uint32_t accent = 0xFF107C10;
constexpr std::uint32_t white = 0xFFFFFF, secondary = 0xB4B4B4, dim = 0x8A8A8A;
constexpr std::uint32_t avatar_colors[12] = {0xFF107C10, 0xFF0078D7, 0xFFE81123, 0xFF8764B8, 0xFFFF8C00, 0xFF00B7C3,
                                             0xFFC239B3, 0xFF498205, 0xFFCA5010, 0xFF6B69D6, 0xFF2D7D9A, 0xFF7A7574};

std::uint8_t channel(std::uint32_t c, int shift) { return static_cast<std::uint8_t>((c >> shift) & 0xFF); }

std::string clock_text() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char text[8];
    std::snprintf(text, sizeof text, "%02d:%02d", local.tm_hour, local.tm_min);
    return text;
}

std::string initial(const std::string& name) {
    if (name.empty()) return "?";
    std::size_t length = 1;
    const auto lead = static_cast<unsigned char>(name[0]);
    if (lead >= 0xF0) length = 4; else if (lead >= 0xE0) length = 3; else if (lead >= 0xC0) length = 2;
    auto first = name.substr(0, length);
    if (first.size() == 1 && first[0] >= 'a' && first[0] <= 'z') first[0] = static_cast<char>(first[0] - 32);
    return first;
}

float approach(float value, float target, double seconds, float rate) {
    const float k = 1.0f - static_cast<float>(std::exp(-rate * seconds));
    const float next = value + (target - value) * k;
    return std::fabs(next - target) < 0.25f ? target : next;
}

float ease(float t) { t = std::clamp(t, 0.0f, 1.0f); return 1 - (1 - t) * (1 - t) * (1 - t); }
} // namespace

void Canvas::blend(int x, int y, std::uint32_t argb, float coverage) {
    if (x < 0 || y < 0 || x >= w_ || y >= h_) return;
    const float a = (channel(argb, 24) / 255.0f) * coverage;
    if (a <= 0) return;
    auto& d = px_[static_cast<std::size_t>(y) * stride_ + x];
    const auto mix = [&](int shift) {
        return static_cast<std::uint32_t>(channel(argb, shift) * a + channel(d, shift) * (1 - a) + 0.5f) << shift;
    };
    const auto alpha = static_cast<std::uint32_t>(255 * a + channel(d, 24) * (1 - a) + 0.5f);
    d = (alpha << 24) | mix(16) | mix(8) | mix(0);
}

void Canvas::fill(int x, int y, int w, int h, std::uint32_t argb) {
    const int x0 = std::max(0, x), y0 = std::max(0, y), x1 = std::min(w_, x + w), y1 = std::min(h_, y + h);
    if (x0 >= x1 || y0 >= y1) return;
    if (channel(argb, 24) == 255) {
        for (int j = y0; j < y1; ++j) std::fill(px_ + static_cast<std::size_t>(j) * stride_ + x0, px_ + static_cast<std::size_t>(j) * stride_ + x1, argb);
        return;
    }
    for (int j = y0; j < y1; ++j) for (int i = x0; i < x1; ++i) blend(i, j, argb, 1);
}

void Canvas::gradient(int x, int y, int w, int h, std::uint32_t top, std::uint32_t bottom) {
    for (int j = 0; j < h; ++j) {
        const float t = h > 1 ? static_cast<float>(j) / (h - 1) : 0;
        std::uint32_t c = 0;
        for (int shift : {24, 16, 8, 0})
            c |= static_cast<std::uint32_t>(channel(top, shift) + (channel(bottom, shift) - channel(top, shift)) * t + 0.5f) << shift;
        fill(x, y + j, w, 1, c);
    }
}

void Canvas::border(int x, int y, int w, int h, int t, std::uint32_t argb) {
    fill(x, y, w, t, argb); fill(x, y + h - t, w, t, argb);
    fill(x, y + t, t, h - 2 * t, argb); fill(x + w - t, y + t, t, h - 2 * t, argb);
}

void Canvas::circle(float cx, float cy, float r, std::uint32_t argb) {
    for (int j = static_cast<int>(cy - r - 1); j <= static_cast<int>(cy + r + 1); ++j)
        for (int i = static_cast<int>(cx - r - 1); i <= static_cast<int>(cx + r + 1); ++i) {
            const float d = std::hypot(i + 0.5f - cx, j + 0.5f - cy);
            blend(i, j, argb, std::clamp(r - d + 0.5f, 0.0f, 1.0f));
        }
}

void Canvas::ring(float cx, float cy, float r, float t, std::uint32_t argb) {
    for (int j = static_cast<int>(cy - r - 1); j <= static_cast<int>(cy + r + 1); ++j)
        for (int i = static_cast<int>(cx - r - 1); i <= static_cast<int>(cx + r + 1); ++i) {
            const float d = std::hypot(i + 0.5f - cx, j + 0.5f - cy);
            blend(i, j, argb, std::clamp(std::min(r - d, d - (r - t)) + 0.5f, 0.0f, 1.0f));
        }
}

void Canvas::image(const Image& img, int x, int y, int w, int h, float opacity) {
    if (img.width <= 0 || img.height <= 0 || w <= 0 || h <= 0) return;
    const int x0 = std::max(0, x), y0 = std::max(0, y), x1 = std::min(w_, x + w), y1 = std::min(h_, y + h);
    for (int j = y0; j < y1; ++j) {
        const float v = ((j - y) + 0.5f) * img.height / h - 0.5f;
        const int v0 = std::clamp(static_cast<int>(std::floor(v)), 0, img.height - 1), v1 = std::min(v0 + 1, img.height - 1);
        const float fv = std::clamp(v - v0, 0.0f, 1.0f);
        for (int i = x0; i < x1; ++i) {
            const float u = ((i - x) + 0.5f) * img.width / w - 0.5f;
            const int u0 = std::clamp(static_cast<int>(std::floor(u)), 0, img.width - 1), u1 = std::min(u0 + 1, img.width - 1);
            const float fu = std::clamp(u - u0, 0.0f, 1.0f);
            const auto at = [&](int uu, int vv) { return img.pixels[static_cast<std::size_t>(vv) * img.width + uu]; };
            float c[4];
            for (int k = 0; k < 4; ++k) {
                const int shift = 24 - k * 8;
                const float top = channel(at(u0, v0), shift) * (1 - fu) + channel(at(u1, v0), shift) * fu;
                const float low = channel(at(u0, v1), shift) * (1 - fu) + channel(at(u1, v1), shift) * fu;
                c[k] = (top * (1 - fv) + low * fv) * opacity;
            }
            const float a = c[0] / 255.0f;
            if (a <= 0.001f) continue;
            auto& d = px_[static_cast<std::size_t>(j) * stride_ + i];
            const auto mix = [&](int k, int shift) {
                return static_cast<std::uint32_t>(std::clamp(c[k] + channel(d, shift) * (1 - a), 0.0f, 255.0f)) << shift;
            };
            d = 0xFF000000u | mix(1, 16) | mix(2, 8) | mix(3, 0);
        }
    }
}

bool Renderer::draw(const Dashboard& ui, Canvas& canvas, Resources& res, double seconds, bool overlay) {
    clock_ += seconds;
    const float s = std::min(canvas.width() / 1920.0f, canvas.height() / 1080.0f);
    const float ox = (canvas.width() - 1920 * s) / 2, oy = (canvas.height() - 1080 * s) / 2;
    const auto X = [&](float v) { return static_cast<int>(std::lround(ox + v * s)); };
    const auto Y = [&](float v) { return static_cast<int>(std::lround(oy + v * s)); };
    const auto S = [&](float v) { return std::max(1, static_cast<int>(std::lround(v * s))); };
    const auto text = [&](const std::string& t, float x, float y, float w, float h, float size, Font f, std::uint32_t rgb,
                          Align a = Align::left) { res.text(canvas, t, X(x), Y(y), S(w), S(h), S(size), f, rgb, a); };
    const auto fill = [&](float x, float y, float w, float h, std::uint32_t c) { canvas.fill(X(x), Y(y), S(w), S(h), c); };
    const auto avatar = [&](float cx, float cy, float r, std::uint32_t index, const std::string& name) {
        canvas.circle(ox + cx * s, oy + cy * s, r * s, avatar_colors[index % 12]);
        canvas.ring(ox + cx * s, oy + cy * s, r * s, std::max(1.5f, 2 * s), 0xA0FFFFFF);
        text(initial(name), cx - r, cy - r, r * 2, r * 2, r * 0.95f, Font::light, white, Align::center);
    };

    // Animation targets.
    if (ui.page != last_page_) { page_ = 0; last_page_ = ui.page; focus_valid_ = false; }
    page_ = std::min(1.0f, page_ + static_cast<float>(seconds / 0.28));
    guide_ = std::clamp(guide_ + static_cast<float>(seconds / 0.18) * (ui.guide_open ? 1 : -1), 0.0f, 1.0f);
    dialog_ = std::clamp(dialog_ + static_cast<float>(seconds / 0.15) * (ui.dialogs.empty() ? -1 : 1), 0.0f, 1.0f);
    scroll_ = approach(scroll_, static_cast<float>(ui.scroll), seconds, 16);
    const float slide = (1 - ease(page_)) * 48;

    const Item* focus = nullptr;
    auto items = ui.items();
    const float shift = -scroll_ + slide;
    const bool title_surface = ui.page == Page::session;
    if (overlay || title_surface) {
        for (int y = 0; y < canvas.height(); ++y)
            std::fill_n(canvas.pixels() + std::size_t(y) * canvas.stride(), canvas.width(), overlay ? 0u : 0xFF000000u);
    }
    if (!overlay && !title_surface) {
    // Background and header.
    canvas.gradient(0, 0, canvas.width(), canvas.height(), 0xFF1C1F1C, 0xFF0A0A0A);
    fill(0, 0, 1920, 6, accent);
    const auto* p = ui.profile();
    if (p) {
        avatar(120, 64, 26, p->avatar, p->name);
        text(p->name, 160, 40, 600, 48, 26, Font::regular, white);
    }
    text(clock_text(), 1500, 40, 324, 48, 26, Font::regular, white, Align::right);
    const auto heading = ui.header();
    if (!heading.empty()) {
        const bool centred = ui.page == Page::sign_in || ui.page == Page::keyboard;
        text(heading, centred ? 0.0f : 96.0f, (ui.page == Page::keyboard ? 170.0f : centred ? 230.0f : 104.0f) + slide, centred ? 1920.0f : 1200.0f, 72, centred ? 56.0f : 44.0f, Font::light, white,
             centred ? Align::center : Align::left);
    }

    focus = ui.guide_open || !ui.dialogs.empty() ? nullptr : ui.focused(items);

    // Page decorations.
    if (ui.page == Page::home) {
        bool pins = false;
        for (const auto& it : items) pins |= it.rect.y >= 700;
        if (pins) text(ui.translate("Pins"), 96, 658 + shift, 600, 44, 26, Font::regular, secondary);
    } else if (ui.page == Page::details) {
        if (const auto* t = ui.title(ui.selected_title)) {
            fill(96, 200 + shift, 400, 400, 0xFF2B2B2B);
            if (const auto* img = res.image(t->logo)) canvas.image(*img, X(116), Y(220 + shift), S(360), S(360));
            else text("\xEE\x9F\xBC", 96, 200 + shift, 400, 400, 120, Font::icons, dim, Align::center);
            text(t->name, 540, 190 + shift, 1300, 72, 48, Font::light, white);
            const char* readiness = t->readiness == console::Readiness::ready ? "Ready" :
                                    t->readiness == console::Readiness::blocked ? "Blocked" : "Not checked";
            const std::pair<std::string, std::string> rows[] = {
                {ui.translate("Publisher"), t->publisher},
                {ui.translate("Version"), t->version},
                {ui.translate("Package"), t->package_name},
                {ui.translate("Executable"), t->executable},
                {"SHA-256", t->id.substr(0, 16) + "..."},
                {ui.translate("Compatibility"), ui.translate(readiness) + (t->check_detail.empty() ? "" : " - " + t->check_detail)},
                {ui.translate("Last result"), t->last_result.empty() ? ui.translate("Never") : t->last_result},
                {ui.translate("Times played"), std::to_string(t->play_count)}};
            float y = 270;
            for (const auto& [label, value] : rows) {
                text(label, 540, y + shift, 260, 36, 21, Font::regular, dim);
                text(value, 800, y + shift, 1024, 36, 21, Font::regular, secondary);
                y += 38;
            }
        }
    } else if (ui.page == Page::keyboard) {
        fill(444, 290 + slide, 1032, 84, 0xFF2B2B2B);
        fill(444, 370 + slide, 1032, 4, accent);
        const bool caret = std::fmod(clock_, 1.0) < 0.55;
        text(ui.keyboard_text + (caret ? "|" : " "), 468, 290 + slide, 990, 84, 38, Font::regular, white);
    }

    // Animated focus box.
    if (focus) {
        const Box target{static_cast<float>(focus->rect.x), focus->rect.y + shift, static_cast<float>(focus->rect.w), static_cast<float>(focus->rect.h)};
        if (!focus_valid_ || last_focus_.empty()) focus_ = target;
        focus_.x = approach(focus_.x, target.x, seconds, 22); focus_.y = approach(focus_.y, target.y, seconds, 22);
        focus_.w = approach(focus_.w, target.w, seconds, 22); focus_.h = approach(focus_.h, target.h, seconds, 22);
        focus_valid_ = true;
        last_focus_ = focus->key;
    }
    const auto list_kind = [](Kind k) { return k == Kind::menu || k == Kind::option || k == Kind::button || k == Kind::key; };
    if (focus && list_kind(focus->kind)) fill(focus_.x, focus_.y, focus_.w, focus_.h, accent);

    for (const auto& it : items) {
        const bool focused = focus && &it == focus;
        const float x = static_cast<float>(it.rect.x), y = it.rect.y + shift, w = static_cast<float>(it.rect.w), h = static_cast<float>(it.rect.h);
        if (y + h < 0 || y > 1080) continue;
        switch (it.kind) {
        case Kind::hero: case Kind::tile: case Kind::action_tile: {
            const bool system_tile = it.color == 0xFF2B2B2B;
            fill(x, y, w, h, system_tile ? accent : it.color);
            const auto* img = it.image.empty() ? nullptr : res.image(it.image);
            const float label_h = it.kind == Kind::hero ? 110.0f : 44.0f;
            if (img) {
                const float box = std::min(w * 0.8f, h - label_h - 16);
                canvas.image(*img, X(x + (w - box) / 2), Y(y + (h - label_h - box) / 2 + 4), S(box), S(box));
            } else {
                const auto icon = it.glyph.empty() ? std::string("\xEE\x9F\xBC") : it.glyph;
                text(icon, x, y, w, h - label_h / 2, it.kind == Kind::hero ? 120.0f : 64.0f, Font::icons, system_tile ? white : 0xDDDDDD, Align::center);
            }
            if (it.kind == Kind::hero) {
                canvas.gradient(X(x), Y(y + h - 140), S(w), S(140), 0x00000000, 0xC0000000);
                text(it.detail, x + 28, y + h - 104, w - 56, 34, 22, Font::regular, secondary);
                text(it.label, x + 28, y + h - 70, w - 56, 56, 38, Font::light, white);
            } else {
                fill(x, y + h - label_h, w, label_h, 0x66000000);
                text(it.label, x + 12, y + h - label_h, w - 24, label_h, 19, Font::regular, white);
            }
            if (!it.value.empty() && it.kind == Kind::tile) {
                const std::uint32_t badge = it.value == ui.translate("Ready") ? 0xFF16C60C : it.value == ui.translate("Blocked") ? 0xFFFF8C00 : 0xFF6E6E6E;
                canvas.circle(ox + (x + w - 18) * s, oy + (y + 18) * s, 7 * s, badge);
            }
            if (it.selected) fill(x, y, w, 6, 0xFFFFFFFF);
            break;
        }
        case Kind::profile: {
            fill(x, y, w, h, focused ? 0xFF383838 : 0xFF262626);
            if (it.key == "profile:new") {
                canvas.ring(ox + (x + w / 2) * s, oy + (y + 110) * s, 72 * s, 3 * s, 0xFFB4B4B4);
                text(it.glyph, x, y + 38, w, 144, 48, Font::icons, white, Align::center);
            } else {
                avatar(x + w / 2, y + 110, 72, static_cast<std::uint32_t>(std::stoul(it.value.empty() ? "0" : it.value)), it.label);
            }
            text(it.label, x + 8, y + 210, w - 16, 48, 24, Font::regular, white, Align::center);
            break;
        }
        case Kind::menu: case Kind::button: {
            if (!focused) fill(x, y, w, h, it.kind == Kind::button ? 0xFF2B2B2B : 0x00000000);
            if (it.selected && it.kind == Kind::menu) fill(x, y, 6, h, focused ? 0xFFFFFFFF : accent);
            float tx = x + 24;
            if (!it.glyph.empty()) { text(it.glyph, x + 20, y, 40, h, 24, Font::icons, white, Align::center); tx = x + 76; }
            text(it.label, tx, y, w - (tx - x) - 16, h, 24, Font::regular, white);
            break;
        }
        case Kind::option: {
            if (!focused) fill(x, y, w, h, 0xFF1F1F1F);
            text(it.label, x + 24, y, w * 0.55f, h, 24, Font::regular, white);
            if (it.detail == "avatar") {
                const auto index = static_cast<std::uint32_t>(std::stoul(it.value.empty() ? "0" : it.value));
                avatar(x + w - 52, y + h / 2, 26, index, p ? p->name : "?");
                if (focused) text("\xEE\x9D\xAC          \xEE\x9D\xAD", x + w - 220, y, 140, h, 16, Font::icons, white, Align::center);
            } else {
                const bool icon = it.value.size() == 3 && static_cast<unsigned char>(it.value[0]) == 0xEE;
                text(it.value, x + w * 0.45f, y, w * 0.55f - 24, h, icon ? 24.0f : 22.0f, icon ? Font::icons : Font::regular,
                     focused ? white : secondary, Align::right);
            }
            break;
        }
        case Kind::key: {
            if (!focused) fill(x, y, w, h, it.selected ? 0xFF4A4A4A : 0xFF333333);
            if (!it.glyph.empty()) {
                text(it.glyph, x + 12, y, 36, h, 20, Font::icons, white, Align::center);
                text(it.label, x + 48, y, w - 56, h, 20, Font::regular, white, Align::center);
            } else text(it.label, x, y, w, h, it.label.size() == 1 ? 32.0f : 22.0f, Font::regular, white, Align::center);
            break;
        }
        case Kind::field: break;
        }
        if (focused && !list_kind(it.kind)) {
            const float grow = 6;
            canvas.border(X(focus_.x - grow), Y(focus_.y - grow), S(focus_.w + grow * 2), S(focus_.h + grow * 2), S(4), 0xFFFFFFFF);
        }
    }

    // Button hints.
    if (!ui.guide_open && ui.dialogs.empty()) {
        struct Hint { const char* pad; std::uint32_t color; const char* key; const char* label; };
        std::vector<Hint> hints{{"A", 0xFF16C60C, "Enter", "Select"}};
        if (ui.page != Page::home && ui.page != Page::sign_in) hints.push_back({"B", 0xFFE81123, "Esc", "Back"});
        if (focus && focus->manage) hints.push_back({"\xE2\x98\xB0", 0xFF555555, "F2", "Manage"});
        if (focus && focus->secondary) hints.push_back({"X", 0xFF0078D7, "Del", "Remove"});
        if (ui.page == Page::keyboard) hints = {{"A", 0xFF16C60C, "A-Z", "Select"}, {"X", 0xFF0078D7, "Bksp", "Delete"},
                                                {"Y", 0xFFFFB900, "Space", "Space"}, {"\xE2\x98\xB0", 0xFF555555, "Enter", "Done"}};
        if (ui.profile()) hints.push_back({"\xE2\x93\x8D", 0xFF107C10, "Home", "Guide"});
        float x = 1824;
        for (auto hint = hints.rbegin(); hint != hints.rend(); ++hint) {
            const auto label = ui.translate(hint->label);
            const float label_w = 22.0f + 11.0f * static_cast<float>(label.size());
            x -= label_w;
            text(label, x, 1012, label_w, 40, 20, Font::regular, secondary);
            if (ui.controller_hints) {
                x -= 40;
                canvas.circle(ox + (x + 16) * s, oy + 1032 * s, 15 * s, hint->color);
                text(hint->pad, x, 1012, 32, 40, 18, Font::semibold, white, Align::center);
            } else {
                const float key_w = 16.0f + 10.0f * static_cast<float>(std::char_traits<char>::length(hint->key));
                x -= key_w + 8;
                canvas.border(X(x), Y(1016), S(key_w), S(32), S(2), 0xFF8A8A8A);
                text(hint->key, x, 1016, key_w, 32, 16, Font::regular, white, Align::center);
            }
            x -= 28;
        }
        text(ui.translate("Locally recreated dashboard. Not Xbox One SystemOS."), 96, 1012, 900, 40, 16, Font::regular, 0x5E5E5E);
    }

    }
    // Toasts.
    float ty = 930;
    for (auto toast = ui.toasts.rbegin(); (!title_surface || ui.guide_open) && toast != ui.toasts.rend(); ++toast) {
        const float in = std::clamp(static_cast<float>(5.0 - toast->remaining) / 0.25f, 0.0f, 1.0f);
        const float x = 96 - (1 - ease(in)) * 80;
        fill(x, ty, 760, 64, 0xF0262626);
        fill(x, ty, 6, 64, accent);
        text(toast->glyph, x + 18, ty, 40, 64, 22, Font::icons, white, Align::center);
        text(toast->text, x + 70, ty, 670, 64, 21, Font::regular, white);
        ty -= 76;
    }

    // Guide blade.
    if (guide_ > 0) {
        const float g = ease(guide_);
        canvas.fill(0, 0, canvas.width(), canvas.height(), static_cast<std::uint32_t>(0x99 * g) << 24);
        const float left = -560 * (1 - g);
        fill(left, 0, 560, 1080, 0xFF202020);
        fill(left, 0, 560, 6, accent);
        text("\xEE\xA6\x90", left + 32, 40, 48, 56, 30, Font::icons, white);
        text(ui.translate("Guide"), left + 88, 40, 300, 56, 30, Font::light, white);
        text(clock_text(), left + 380, 40, 148, 56, 24, Font::regular, secondary, Align::right);
        const auto guide = ui.guide_items();
        const Item* gf = ui.dialogs.empty() ? ui.focused(guide) : nullptr;
        for (const auto& it : guide) {
            const float x = it.rect.x + left, y = static_cast<float>(it.rect.y), w = static_cast<float>(it.rect.w), h = static_cast<float>(it.rect.h);
            if (&it == gf) fill(x, y, w, h, accent);
            if (it.kind == Kind::profile) {
                avatar(x + 48, y + h / 2, 32, static_cast<std::uint32_t>(std::stoul(it.value)), it.label);
                text(it.label, x + 96, y + 14, w - 110, 36, 26, Font::regular, white);
                text(it.detail, x + 96, y + 50, w - 110, 30, 19, Font::regular, &it == gf ? white : secondary);
            } else {
                text(it.glyph, x + 16, y, 40, h, 22, Font::icons, white, Align::center);
                text(it.label, x + 72, y, w - 88, h, 23, Font::regular, white);
            }
        }
    }

    // Dialog.
    if (!ui.dialogs.empty()) {
        const auto& d = ui.dialogs.back();
        const float a = ease(dialog_);
        canvas.fill(0, 0, canvas.width(), canvas.height(), static_cast<std::uint32_t>(0xB0 * a) << 24);
        const float line_h = d.monospace ? 30.0f : 40.0f;
        const float h = 200 + line_h * static_cast<float>(d.lines.size());
        const float y = (1080 - h) / 2 + (1 - a) * 30;
        fill(260, y, 1400, h, 0xFF262626);
        fill(260, y, 1400, 6, accent);
        text(d.title, 300, y + 28, 1320, 56, 34, Font::light, white);
        float ly = y + 96;
        for (std::size_t i = 0; i < d.lines.size(); ++i) {
            const bool mono = d.monospace && i > 0;
            text(d.lines[i], 300, ly, 1320, line_h, mono ? 17.0f : 22.0f, mono ? Font::mono : Font::regular, i == 0 ? white : secondary);
            ly += line_h;
        }
        float bx = 1620;
        for (int i = static_cast<int>(d.buttons.size()) - 1; i >= 0; --i) {
            bx -= 260;
            fill(bx, y + h - 84, 240, 56, i == d.focus ? accent : 0xFF3A3A3A);
            text(d.buttons[static_cast<std::size_t>(i)], bx, y + h - 84, 240, 56, 22, Font::regular, white, Align::center);
            bx -= 16;
        }
    }

    // Page transition fade.
    if (!overlay && !title_surface && page_ < 1) canvas.fill(0, 0, canvas.width(), canvas.height(), static_cast<std::uint32_t>(0xC0 * (1 - ease(page_))) << 24 | 0x0A0A0A);

    const bool focus_moving = focus && (std::fabs(focus_.x - focus->rect.x) > 0.3f || std::fabs(focus_.y - (focus->rect.y + shift)) > 0.3f ||
                                        std::fabs(focus_.w - focus->rect.w) > 0.3f || std::fabs(focus_.h - focus->rect.h) > 0.3f);
    return page_ < 1 || (guide_ > 0 && guide_ < 1) || (dialog_ > 0 && dialog_ < 1) || focus_moving ||
           std::fabs(scroll_ - ui.scroll) > 0.3f || !ui.toasts.empty() || ui.page == Page::keyboard;
}

} // namespace xboxone::dashboard
