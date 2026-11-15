// SPDX-License-Identifier: BSD-3-Clause
#pragma once
// Windows drawing surface for the dashboard renderer: a top-down 32-bit DIB
// shared by the software canvas and GDI text, plus WIC image decoding.
#include "dashboard_render.h"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <map>
#include <memory>
#include <optional>
#include <tuple>

namespace xboxone::dashboard {

class GdiSurface final : public Resources {
public:
    GdiSurface();
    ~GdiSurface() override;
    GdiSurface(const GdiSurface&) = delete;
    GdiSurface& operator=(const GdiSurface&) = delete;
    bool resize(int width, int height);
    [[nodiscard]] Canvas canvas() const { return Canvas(bits_, width_, height_, width_); }
    [[nodiscard]] int width() const { return width_; }
    [[nodiscard]] int height() const { return height_; }
    [[nodiscard]] const std::uint32_t* bits() const { return bits_; }
    void text(Canvas&, const std::string& utf8, int x, int y, int w, int h, int size, Font, std::uint32_t rgb, Align) override;
    const Image* image(const std::string& utf8_path) override;
private:
    HFONT font(int size, Font);
    HDC dc_ = nullptr;
    HBITMAP bitmap_ = nullptr;
    HGDIOBJ previous_ = nullptr;
    std::uint32_t* bits_ = nullptr;
    int width_ = 0, height_ = 0;
    std::map<std::pair<int, int>, HFONT> fonts_;
    std::map<std::string, std::optional<Image>> images_;
};

} // namespace xboxone::dashboard
