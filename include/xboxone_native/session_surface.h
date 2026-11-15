// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <cstdint>
#include <span>

namespace xboxone::session {
// One system UI and one active title share this presentation/input decision.
// Host adapters transport local_recreation pixels; these are never game frames.
enum class SurfaceMode : std::uint32_t { system_ui, title, guide };
struct SystemUiFrame {
    SurfaceMode mode = SurfaceMode::system_ui;
    std::uint32_t width = 0, height = 0;
    std::span<const std::uint32_t> pixels; // premultiplied BGRA, tightly packed
};
inline constexpr std::uint32_t surface_max_width = 3840, surface_max_height = 2160;
inline bool valid_frame(const SystemUiFrame& frame) {
    return frame.mode <= SurfaceMode::guide && frame.width && frame.height &&
        frame.width <= surface_max_width && frame.height <= surface_max_height &&
        frame.pixels.size() == std::size_t(frame.width) * frame.height;
}
} // namespace xboxone::session
