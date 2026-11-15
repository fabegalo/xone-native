// SPDX-License-Identifier: BSD-3-Clause
#include "era_session.h"

namespace xboxone::era {
namespace {
session::SurfaceChannel channel;
HWND parent_window = nullptr;
}
bool bind_session_surface(HWND parent) {
    if (!channel.connect(GetCurrentProcessId(), parent, false)) return false;
    parent_window = parent;
    return true;
}
HWND session_parent() { return parent_window; }
bool session_title_input() {
    if (!parent_window) return true; // Standalone ERA retains its existing input contract.
    session::SurfaceMode mode{}; UINT width = 0, height = 0;
    return IsWindow(parent_window) && GetForegroundWindow() == parent_window &&
        channel.read(mode, width, height) && mode == session::SurfaceMode::title;
}
bool session_guide_frame(std::vector<std::uint32_t>& pixels, UINT& width, UINT& height) {
    if (!parent_window) return false;
    session::SurfaceMode mode{};
    return channel.read(mode, width, height, &pixels) && mode == session::SurfaceMode::guide && width && height;
}
} // namespace xboxone::era
