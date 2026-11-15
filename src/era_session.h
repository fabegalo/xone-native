// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include "session_surface_windows.h"

namespace xboxone::era {
bool bind_session_surface(HWND parent);
HWND session_parent();
bool session_title_input();
bool session_guide_frame(std::vector<std::uint32_t>& pixels, UINT& width, UINT& height);
} // namespace xboxone::era
