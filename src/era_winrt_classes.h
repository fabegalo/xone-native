// SPDX-License-Identifier: BSD-3-Clause
// Title-layer WinRT class factories served by the activation boundary.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace xboxone::era {

// Windows.Xbox.Services.XboxLiveConfiguration (static class).
HRESULT xbox_live_configuration_factory(REFIID iid, void** factory);

} // namespace xboxone::era
