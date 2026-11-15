// SPDX-License-Identifier: BSD-3-Clause
#pragma once
// Software renderer for the local dashboard. Draws into a CPU BGRA frame that
// the host presents through xboxone::presentation with local_recreation origin.
#include "xboxone_native/dashboard.h"

#include <cstdint>
#include <string>
#include <vector>

namespace xboxone::dashboard {

struct Image { int width = 0, height = 0; std::vector<std::uint32_t> pixels; }; // premultiplied 0xAARRGGBB

class Canvas {
public:
    Canvas(std::uint32_t* pixels, int width, int height, int stride) : px_(pixels), w_(width), h_(height), stride_(stride) {}
    [[nodiscard]] int width() const { return w_; }
    [[nodiscard]] int height() const { return h_; }
    void fill(int x, int y, int w, int h, std::uint32_t argb);
    void gradient(int x, int y, int w, int h, std::uint32_t top, std::uint32_t bottom);
    void border(int x, int y, int w, int h, int thickness, std::uint32_t argb);
    void circle(float cx, float cy, float r, std::uint32_t argb);
    void ring(float cx, float cy, float r, float thickness, std::uint32_t argb);
    void image(const Image&, int x, int y, int w, int h, float opacity = 1.0f);
    [[nodiscard]] std::uint32_t* pixels() const { return px_; }
    [[nodiscard]] int stride() const { return stride_; }
private:
    void blend(int x, int y, std::uint32_t argb, float coverage);
    std::uint32_t* px_;
    int w_, h_, stride_;
};

enum class Font { regular, light, semibold, icons, mono };
enum class Align { left, center, right };

class Resources {
public:
    virtual ~Resources() = default;
    // Physical pixels; single line, vertically centred, ellipsis when too long.
    virtual void text(Canvas&, const std::string& utf8, int x, int y, int w, int h, int size, Font, std::uint32_t rgb, Align) = 0;
    virtual const Image* image(const std::string& utf8_path) = 0; // null when unreadable
};

class Renderer {
public:
    // Returns true while an animation still needs frames.
    bool draw(const Dashboard&, Canvas&, Resources&, double seconds, bool overlay = false);
private:
    struct Box { float x = 0, y = 0, w = 0, h = 0; };
    Box focus_{};
    bool focus_valid_ = false;
    float scroll_ = 0, guide_ = 0, page_ = 1, dialog_ = 0;
    Page last_page_ = Page::sign_in;
    std::string last_focus_;
    double clock_ = 0;
};

} // namespace xboxone::dashboard
