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
#include <vector>

namespace xboxone::appmodel { struct Manifest; }
namespace xboxone::era {

// Windows.Xbox.Services.XboxLiveConfiguration (static class).
HRESULT xbox_live_configuration_factory(REFIID iid, void** factory);
// EntryPoint of the package manifest's first Application element.
HRESULT title_entry_point(std::wstring& entry_point);
HRESULT title_chunk_installed(std::uint32_t chunk, bool& installed);
HRESULT title_installed_chunks(std::vector<std::uint32_t>& chunks);
HRESULT title_package_metadata(appmodel::Manifest& manifest, std::wstring& root, std::string& revision);
HRESULT package_factory(REFIID iid, void** factory);
// HWND behind a CoreWindow handed to the title, or null if `window` is not ours.
HWND core_window_hwnd(const void* window);
// Windows.Xbox.System.User (statics; observed slots only).
HRESULT xbox_user_factory(REFIID iid, void** factory);
std::wstring title_storage_root();
void reset_title_users();
HRESULT title_local_user_identity(GUID* identity, std::uint32_t* local_id);
HRESULT title_local_user(void** user); // Retained snapshot of the selected session profile.
HRESULT xbox_controller_factory(REFIID iid, void** factory);
HRESULT xbox_gamepad_factory(REFIID iid, void** factory);
struct RawGamepadReading {
    std::uint64_t timestamp = 0; // Monotonic 100 ns units, not wall-clock time.
    std::uint32_t buttons = 0;
    float left_trigger = 0, right_trigger = 0;
    float left_x = 0, left_y = 0, right_x = 0, right_y = 0;
};
static_assert(sizeof(RawGamepadReading)==40 && offsetof(RawGamepadReading,left_trigger)==12);
HRESULT poll_title_input();
HRESULT start_title_input(); // Called on the session dispatcher before title initialization.
void stop_title_input();
std::size_t xbox_user_removed_handlers(); // Test observation.
// Windows.Xbox.Management.Deployment.DownloadableContentPackageManager.
HRESULT dlc_package_manager_factory(REFIID iid, void** factory);
HRESULT package_transfer_manager_factory(REFIID iid, void** factory);
HRESULT package_transfer_watcher_factory(REFIID iid, void** factory);
// Windows.ApplicationModel.Core.CoreApplication (view lifecycle driven here).
HRESULT core_application_factory(REFIID iid, void** factory);
HRESULT core_window_factory(REFIID iid, void** factory);
inline constexpr char dlc_package_view_signature[] =
    "pinterface({bbe1fa4c-b0e3-4583-baef-1f1b2e483e56};rc(Windows.Xbox.Management.Deployment.DownloadableContentPackage;"
    "{696996e9-f0f6-4497-a05e-514d915b7d05}))";

} // namespace xboxone::era
