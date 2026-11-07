// SPDX-License-Identifier: BSD-3-Clause
// Real COM failure only; never registers a package or launches an installed app.
#include "appmodel_internal.h"
#include "appmodel_windows.h"
#include <iostream>
#include <filesystem>

int observation_tests(char** argv) {
    using namespace xboxone::systemos;
    for (int i = 1; i <= 3; ++i) {
        const auto path = std::filesystem::path(argv[i]).wstring();
        auto command = L"\"" + path + L"\"";
        STARTUPINFOW startup{sizeof(startup)};
        PROCESS_INFORMATION child{};
        if (!CreateProcessW(path.c_str(), command.data(), nullptr, nullptr, FALSE,
                            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &child)) return 1;
        CloseHandle(child.hThread);
        Report report;
        const auto ok = observe_appmodel_exit(child.hProcess, report, i == 2 ? 50 : 3000);
        bool passed = !report.activation_identity_verified && !report.owned_lifecycle_verified;
        if (i == 2) {
            passed = passed && !ok && report.stage == Stage::timed_out && !report.exit_code_available &&
                WaitForSingleObject(child.hProcess, 0) == WAIT_TIMEOUT;
        } else {
            passed = passed && ok == (i == 1) && report.stage == Stage::exited &&
                report.exit_code_available && report.exit_code == (i == 1 ? 0u : 27u);
        }
        // These are our CreateProcess fixtures, never broker-returned processes.
        if (WaitForSingleObject(child.hProcess, 0) != WAIT_OBJECT_0) TerminateProcess(child.hProcess, 99);
        passed = WaitForSingleObject(child.hProcess, 3000) == WAIT_OBJECT_0 && passed;
        CloseHandle(child.hProcess);
        if (!passed) return 1;
    }
    Report invalid;
    if (observe_appmodel_exit(nullptr, invalid, 1) || invalid.stage != Stage::failed) return 1;
    std::cout << "Process exit, nonzero, observation timeout without termination and invalid handle passed; no deployment/activation.\n";
    return 0;
}
int main(int argc, char** argv) {
    if (argc == 4) return observation_tests(argv);
    try {
        using namespace xboxone::appmodel::windows;
        Apartment apartment;
        GUID guid{};
        require(CoCreateGuid(&guid), "CoCreateGuid");
        wchar_t value[40]{};
        if (!StringFromGUID2(guid, value, 40)) return 1;
        std::string nonce;
        for (const auto c : utf8(value)) if (c != '{' && c != '}' && c != '-') nonce += c;
        const auto aumid = "XON.Missing." + nonce.substr(0, 24) + "_aaaaaaaaaaaaa!OwnedLifecycle";
        xboxone::systemos::Report report;
        const bool activated = xboxone::systemos::request_packaged_activation(aumid, L"", report);
        for (const auto& event : report.activation_events)
            std::cout << event.api << "; HRESULT=0x" << std::hex << event.result << '\n';
        if (activated || report.process_id || report.activation_events.empty()) return 1;
        // Require an actual failing API result; failure location stays visible.
        if (!(report.activation_events.back().result & 0x80000000U)) return 1;
        return 0;
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
