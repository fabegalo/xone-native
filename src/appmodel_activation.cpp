// SPDX-License-Identifier: BSD-3-Clause
#include "appmodel_internal.h"
#include "appmodel_windows.h"
#include "file_path.h"
#include "xboxone_native/inventory.h"
#include <algorithm>
#include <stdexcept>
#ifdef _WIN32
#include <appmodel.h>
#include <shobjidl.h>
#endif

namespace xboxone::systemos {
#ifdef _WIN32
bool observe_appmodel_exit(void* process, Report& report, std::uint32_t timeout_ms) {
    const auto waited = WaitForSingleObject(process, timeout_ms);
    report.activation_events.push_back({"WaitForSingleObject(process)", waited, "win32"});
    if (waited == WAIT_TIMEOUT) {
        report.stage = Stage::timed_out;
        report.diagnostics.push_back({"packaged_observation_timeout", report.activated_aumid,
            "process remains unmodified; observation timeout is not shutdown or initialization"});
        return false;
    }
    DWORD code = 0;
    if (waited != WAIT_OBJECT_0 || !GetExitCodeProcess(process, &code)) {
        report.stage = Stage::failed;
        report.diagnostics.push_back({"packaged_observation_failed", report.activated_aumid,
            "cannot observe process exit; Win32=" + std::to_string(GetLastError())});
        return false;
    }
    report.exit_code = code;
    report.exit_code_available = true;
    report.stage = Stage::exited;
    if (code != 0) report.diagnostics.push_back({"packaged_exit_nonzero", report.activated_aumid,
        "process exited with code " + std::to_string(code)});
    return code == 0;
}

bool request_packaged_activation(const std::string& aumid, const std::wstring& arguments, Report& report) {
    using namespace appmodel::windows;
    Apartment apartment;
    const auto event = [&](const char* api, HRESULT hr) {
        report.activation_events.push_back({api, static_cast<std::uint32_t>(hr), "hresult"});
        return SUCCEEDED(hr);
    };
    Com<IApplicationActivationManager> manager;
    const auto created = CoCreateInstance(CLSID_ApplicationActivationManager, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&manager));
    if (!event("CoCreateInstance(ApplicationActivationManager)", created)) return false;
    const auto foreground = CoAllowSetForegroundWindow(manager.Get(), nullptr);
    // Foreground permission is advisory, not proof of activation or focus.
    // Preserve its failure and let ActivateApplication return its actual result.
    event("CoAllowSetForegroundWindow", foreground);
    DWORD pid = 0;
    report.activated_aumid = aumid;
    const auto hr = manager->ActivateApplication(wide(aumid).c_str(), arguments.c_str(), AO_NOERRORUI, &pid);
    const bool success = event("IApplicationActivationManager::ActivateApplication", hr);
    if (success) report.process_id = pid;
    return success;
}
#endif
bool run_appmodel(const Profile& profile, Report& report, std::uint32_t timeout_ms) {
    const auto fail = [&](std::string code, std::string detail) {
        report.stage = Stage::failed;
        report.diagnostics.push_back({std::move(code), profile.appmodel.manifest, std::move(detail)});
        return false;
    };
#if !defined(_WIN32) || !(defined(_M_X64) || defined(__x86_64__))
    (void)timeout_ms;
    return fail("appmodel_backend_unavailable", "native Windows x64 required");
#else
    using namespace appmodel::windows;
    struct Handle {
        HANDLE value = nullptr;
        ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    };
    try {
        Apartment apartment;
        const auto package = std::find_if(report.packages.begin(), report.packages.end(),
            [&](const auto& n) { return n.artifact == profile.appmodel.manifest; });
        if (package == report.packages.end()) return fail("appmodel_package_missing", "prepared package required");
        const auto& manifest = package->manifest;
        const auto application = std::find_if(manifest.applications.begin(), manifest.applications.end(),
            [&](const auto& a) { return a.id == profile.appmodel.application_id; });
        if (application == manifest.applications.end()) return fail("appmodel_application_missing", "prepared application required");
        const auto entry = std::find_if(profile.artifacts.begin(), profile.artifacts.end(), [&](const auto& a) { return a.id == profile.entry; });
        const auto source = std::find_if(profile.artifacts.begin(), profile.artifacts.end(), [&](const auto& a) { return a.id == profile.appmodel.manifest; });
        if (entry == profile.artifacts.end() || source == profile.artifacts.end())
            return fail("appmodel_artifact_missing", "prepared entry and manifest required");
        const auto event = [&](const char* api, std::uint32_t result, const char* kind = "win32") {
            report.activation_events.push_back({api, result, kind});
        };
        const auto full_name = wide(manifest.full_name);
        UINT32 length = 0;
        auto rc = GetPackagePathByFullName(full_name.c_str(), &length, nullptr);
        event("GetPackagePathByFullName(size)", static_cast<std::uint32_t>(rc));
        if (rc != ERROR_INSUFFICIENT_BUFFER || length > 32768)
            return fail("package_registration_unavailable", "exact pinned package is not accessible; Win32=" + std::to_string(rc));
        std::wstring installed(length, L'\0');
        rc = GetPackagePathByFullName(full_name.c_str(), &length, installed.data());
        event("GetPackagePathByFullName", static_cast<std::uint32_t>(rc));
        if (rc != ERROR_SUCCESS) return fail("package_registration_unavailable", "package path query failed");
        installed.resize(length - 1);
        std::error_code ec;
        if (!std::filesystem::equivalent(detail::file_path(installed), detail::file_path(source->path.parent_path()), ec) || ec)
            return fail("registered_package_path_mismatch", "registered exact identity does not reference the pinned input root");
        // Prevent entry replacement while broker/loader opens it. Stable package
        // ancestors remain a precondition; this is not a hostile-FS snapshot.
        Handle input{CreateFileW(detail::file_path(entry->path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr)};
        std::string hash, error;
        if (input.value == INVALID_HANDLE_VALUE || !inventory::fingerprint(entry->path, hash, error) || hash != entry->sha256)
            return fail("package_entry_changed", "entry lock/hash verification failed: " + error);
        const bool fixture = profile.appmodel.lifecycle == AppModelActivation::Lifecycle::owned_fixture;
        Handle started, stop, finished;
        std::wstring args;
        if (fixture) {
            GUID guid{};
            require(CoCreateGuid(&guid), "CoCreateGuid");
            wchar_t token[40]{};
            if (!StringFromGUID2(guid, token, 40)) return fail("activation_nonce_failed", "GUID formatting failed");
            const std::wstring prefix = std::wstring(L"Local\\XON-AppModel-") + token;
            const auto make_event = [&](const wchar_t* suffix) {
                const auto h = CreateEventW(nullptr, TRUE, FALSE, (prefix + suffix).c_str());
                if (!h) throw std::runtime_error("CreateEventW; Win32=" + std::to_string(GetLastError()));
                if (GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(h); throw std::runtime_error("activation event collision"); }
                return h;
            };
            started.value = make_event(L"-started"); stop.value = make_event(L"-stop"); finished.value = make_event(L"-finished");
            args = std::wstring(L"--xboxone-appmodel-test=") + token;
        }
        // No CreateProcess fallback, no shell activation and no deployment.
        if (!request_packaged_activation(application->aumid, args, report))
            return fail("packaged_activation_failed", "official activation API failed; see HRESULT activation_events");
        report.stage = Stage::running;
        Handle process{OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, report.process_id)};
        if (!process.value) return fail("activated_process_unobservable", "OpenProcess; Win32=" + std::to_string(GetLastError()));
        const auto identity = [&](auto query, const std::string& expected, const char* api) {
            UINT32 count = 0;
            auto status = query(process.value, &count, nullptr);
            if (status != ERROR_INSUFFICIENT_BUFFER || count > 32768) { event(api, static_cast<std::uint32_t>(status)); return false; }
            std::wstring value(count, L'\0');
            status = query(process.value, &count, value.data());
            event(api, static_cast<std::uint32_t>(status));
            return status == ERROR_SUCCESS && utf8(value.c_str()) == expected;
        };
        if (!identity(GetPackageFullName, manifest.full_name, "GetPackageFullName(process)") ||
            !identity(GetApplicationUserModelId, application->aumid, "GetApplicationUserModelId(process)")) {
            if (stop.value) SetEvent(stop.value);
            return fail("activated_process_identity_mismatch", "returned PID is not the exact package/application");
        }
        wchar_t image[32768]{}; DWORD count = 32768;
        if (!QueryFullProcessImageNameW(process.value, 0, image, &count) ||
            !std::filesystem::equivalent(detail::file_path(image), detail::file_path(entry->path), ec) || ec) {
            if (stop.value) SetEvent(stop.value);
            return fail("activated_process_path_mismatch", "returned PID executable differs from pinned entry");
        }
        report.activation_identity_verified = true;
        if (!fixture) return observe_appmodel_exit(process.value, report, timeout_ms);
        const auto began = GetTickCount64();
        auto waited = WaitForSingleObject(started.value, timeout_ms);
        event("owned_fixture_started_event", waited);
        // Always request cooperative stop, including on missing handshake. Never
        // terminate an arbitrary reused PID returned by the activation broker.
        if (!SetEvent(stop.value)) return fail("fixture_stop_failed", "SetEvent; Win32=" + std::to_string(GetLastError()));
        const auto elapsed = GetTickCount64() - began;
        const auto remaining = elapsed >= timeout_ms ? 0 : timeout_ms - static_cast<DWORD>(elapsed);
        const auto exited = WaitForSingleObject(process.value, remaining);
        event("WaitForSingleObject(process)", exited);
        if (exited != WAIT_OBJECT_0) {
            report.stage = Stage::timed_out;
            report.diagnostics.push_back({"packaged_observation_timeout", application->id, "cooperative stop requested; process exit not observed within deadline"});
            return false;
        }
        DWORD code = 0;
        if (!GetExitCodeProcess(process.value, &code)) return fail("packaged_exit_query_failed", "GetExitCodeProcess failed");
        report.exit_code = code; report.exit_code_available = true;
        const auto completed = WaitForSingleObject(finished.value, 0);
        event("owned_fixture_finished_event", completed);
        if (waited != WAIT_OBJECT_0 || completed != WAIT_OBJECT_0 || code != 0)
            return fail("packaged_lifecycle_unverified", "owned lifecycle handshake/exit failed; exit=" + std::to_string(code));
        report.stage = Stage::exited;
        report.owned_lifecycle_verified = true;
        return true;
    } catch (const std::exception& e) { return fail("appmodel_backend_failed", e.what()); }
#endif
}
} // namespace xboxone::systemos
