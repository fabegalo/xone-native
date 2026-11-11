// SPDX-License-Identifier: BSD-3-Clause
// Title-layer WinRT class factories served by the activation boundary.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <string>

namespace xboxone::era {

// Windows.Xbox.Services.XboxLiveConfiguration (static class).
HRESULT xbox_live_configuration_factory(REFIID iid, void** factory);
// EntryPoint of the package manifest's first Application element.
HRESULT title_entry_point(std::wstring& entry_point);
HRESULT title_chunk_installed(std::uint32_t chunk, bool& installed);
// HWND behind a CoreWindow handed to the title, or null if `window` is not ours.
HWND core_window_hwnd(const void* window);
// Windows.Xbox.System.User (statics; observed slots only).
HRESULT xbox_user_factory(REFIID iid, void** factory);
std::size_t xbox_user_removed_handlers(); // Test observation.
// Windows.Xbox.Management.Deployment.DownloadableContentPackageManager.
HRESULT dlc_package_manager_factory(REFIID iid, void** factory);
HRESULT package_transfer_manager_factory(REFIID iid, void** factory);
// Windows.ApplicationModel.Core.CoreApplication (view lifecycle driven here).
HRESULT core_application_factory(REFIID iid, void** factory);
inline constexpr char dlc_package_view_signature[] =
    "pinterface({bbe1fa4c-b0e3-4583-baef-1f1b2e483e56};rc(Windows.Xbox.Management.Deployment.DownloadableContentPackage;"
    "{696996e9-f0f6-4497-a05e-514d915b7d05}))";

} // namespace xboxone::era
