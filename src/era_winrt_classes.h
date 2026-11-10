// SPDX-License-Identifier: BSD-3-Clause
// Title-layer WinRT class factories served by the activation boundary.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstddef>

namespace xboxone::era {

// Windows.Xbox.Services.XboxLiveConfiguration (static class).
HRESULT xbox_live_configuration_factory(REFIID iid, void** factory);
// Windows.Xbox.System.User (statics; observed slots only).
HRESULT xbox_user_factory(REFIID iid, void** factory);
std::size_t xbox_user_removed_handlers(); // Test observation.

} // namespace xboxone::era
