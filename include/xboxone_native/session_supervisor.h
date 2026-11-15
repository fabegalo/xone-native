// SPDX-License-Identifier: BSD-3-Clause
#pragma once
// System UI <-> title lifecycle. Starts the existing ERA launcher (xone-era) as
// a supervised child process, observes its end and reports its real result.
// Any system UI backend (local dashboard, future original SystemOS) uses it.
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include "session_surface.h"

namespace xboxone::session {

enum class Mode { check, run };

struct Request {
    Mode mode = Mode::run;
    std::filesystem::path runtime;  // xone-era executable
    std::filesystem::path title;    // entry executable inside its package
    std::filesystem::path storage;  // ERA --storage root (run only)
    std::filesystem::path log;      // child stdout+stderr, replaced per session
    std::uint32_t time_limit_ms = 0; // 0: no ERA deadline, the supervisor owns shutdown
    std::uint32_t suspend_grace_ms = 8000; // run: terminate() waits this long for Suspending, then kills
    std::uintptr_t session_window = 0; // Native persistent frame supplied by the host adapter.
};

struct Result {
    int exit_code = 0;
    bool terminated = false;   // ended by the supervisor owner (Guide "Quit")
    bool normal = false;       // title returned or called exit, code 0
    bool mapped = false;       // check: image mapped and ready for entry
    std::uint32_t resolved = 0, unresolved = 0;
    std::uint64_t duration_ms = 0;
    std::string summary;       // one line from the real output
    std::vector<std::string> tail; // last output lines (bounded)
};

// Portable interpretation of xone-era's exit code and output.
Result interpret(int exit_code, bool terminated, const std::string& output);

class Host {
public:
    virtual ~Host() = default;
    virtual bool start(const Request& request, std::string& error) = 0;
    [[nodiscard]] virtual bool active() const = 0;
    // True once, when the active session has ended; result then holds its outcome.
    virtual bool poll(Result& result) = 0;
    // Run sessions are asked to suspend first (the title's Suspending handlers run);
    // the process is killed after the grace period or on a second call.
    virtual void terminate() = 0;
    // Brings the title's window to the foreground; false when it has none.
    virtual bool focus_title() = 0;
    // System UI pixels and input ownership follow the same supervised session.
    virtual bool present_system_ui(const SystemUiFrame&) { return false; }
};

// Windows x64: process + kill-on-close job. Other hosts: start() fails explicitly.
std::unique_ptr<Host> create_host();

} // namespace xboxone::session
