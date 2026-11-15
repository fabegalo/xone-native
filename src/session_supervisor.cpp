// SPDX-License-Identifier: BSD-3-Clause
#include "xboxone_native/session_supervisor.h"
#include "file_path.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <optional>
#include <sstream>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace xboxone::session {
namespace {
constexpr std::size_t tail_lines = 14;
constexpr std::streamoff tail_bytes = 32 * 1024;

std::string read_tail(const std::filesystem::path& log) {
    std::ifstream in(detail::file_path(log), std::ios::binary);
    if (!in) return {};
    in.seekg(0, std::ios::end);
    const auto size = static_cast<std::streamoff>(in.tellg());
    in.seekg(size > tail_bytes ? size - tail_bytes : 0);
    std::ostringstream text;
    text << in.rdbuf();
    return text.str();
}

std::string first_line_with(const std::vector<std::string>& lines, const char* needle, bool last = false) {
    std::string found;
    for (const auto& line : lines)
        if (line.find(needle) != std::string::npos) { found = line; if (!last) break; }
    return found;
}
} // namespace

Result interpret(int exit_code, bool terminated, const std::string& output) {
    Result r;
    r.exit_code = exit_code;
    r.terminated = terminated;
    std::vector<std::string> lines;
    std::istringstream in(output);
    for (std::string line; std::getline(in, line);) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty()) lines.push_back(line);
    }
    unsigned long resolved = 0, unresolved = 0;
    for (const auto& line : lines)
        if (std::sscanf(line.c_str(), "resolved %lu, unresolved %lu", &resolved, &unresolved) == 2) {
            r.resolved = static_cast<std::uint32_t>(resolved);
            r.unresolved = static_cast<std::uint32_t>(unresolved);
        }
    r.mapped = first_line_with(lines, "mapped: ready for entry").size() || first_line_with(lines, "running title").size();
    r.tail.assign(lines.size() > tail_lines ? lines.end() - tail_lines : lines.begin(), lines.end());
    const bool returned = !first_line_with(lines, "title entry returned").empty() || !first_line_with(lines, "title called exit").empty();
    char code[32];
    std::snprintf(code, sizeof code, "0x%08X", static_cast<unsigned>(exit_code));
    if (terminated) {
        r.summary = "Closed by the system UI";
        if (!first_line_with(lines, "xone-era: title suspended").empty()) r.summary += " after the title suspended";
        else if (!first_line_with(lines, "suspend deadline reached").empty()) r.summary += "; the title did not finish suspending in time";
        else if (!first_line_with(lines, "no Suspending handler").empty()) r.summary += "; the title registered no Suspending handler";
    }
    else if (exit_code == 0 && returned) { r.normal = true; r.summary = "Title exited normally"; }
    else if (exit_code == 0 && r.mapped) r.summary = std::to_string(r.resolved) + " imports resolved, 0 missing";
    else if (exit_code == 124) r.summary = "Time limit reached; the runtime ended the title";
    else if (exit_code == 125) r.summary = first_line_with(lines, "unimplemented");
    else if (exit_code == 1 && !first_line_with(lines, "not mapped").empty()) {
        r.summary = "Not mapped: " + std::to_string(r.unresolved) + " imports unresolved";
        const auto missing = first_line_with(lines, "missing ");
        if (!missing.empty()) r.summary += " (first: " + missing.substr(8) + ")";
        const auto blocked = first_line_with(lines, "blocked: ");
        if (!blocked.empty()) r.summary += "; " + blocked;
    } else if (static_cast<unsigned>(exit_code) >= 0xC0000000u) {
        const auto exception = first_line_with(lines, "exception 0x", true);
        r.summary = "Title stopped with exception " + std::string(code) + (exception.empty() ? "" : ": " + exception);
    }
    if (r.summary.empty()) {
        r.summary = "xone-era exited with code " + std::to_string(exit_code);
        if (!lines.empty()) r.summary += ": " + lines.back();
    }
    return r;
}

#ifdef _WIN32
namespace {
std::wstring quote(const std::wstring& argument) {
    // CommandLineToArgvW rules: backslashes are literal unless they precede a quote.
    std::wstring out = L"\"";
    std::size_t slashes = 0;
    for (const wchar_t c : argument) {
        if (c == L'\\') { ++slashes; continue; }
        out.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        slashes = 0;
        out += c;
    }
    out.append(slashes * 2, L'\\');
    return out + L"\"";
}

struct Handle {
    HANDLE value = nullptr;
    ~Handle() { reset(); }
    void reset(HANDLE next = nullptr) { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); value = next; }
};

class WindowsHost final : public Host {
public:
    bool start(const Request& input, std::string& error) override {
        if (active()) { error = "a title session is already active"; return false; }
        std::error_code ec;
        // The child runs in the runtime's folder, so every path it receives must be absolute.
        auto request = input;
        for (auto* path : {&request.runtime, &request.title, &request.storage, &request.log})
            if (!path->empty()) *path = std::filesystem::absolute(*path, ec);
        if (!std::filesystem::is_regular_file(request.runtime, ec)) {
            error = "ERA runtime not found: " + request.runtime.string(); return false;
        }
        std::filesystem::create_directories(request.log.parent_path(), ec);
        SECURITY_ATTRIBUTES inherit{sizeof inherit, nullptr, TRUE};
        Handle log;
        log.reset(CreateFileW(request.log.c_str(), GENERIC_WRITE, FILE_SHARE_READ, &inherit, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr));
        if (log.value == INVALID_HANDLE_VALUE) { error = "cannot create session log: Win32 " + std::to_string(GetLastError()); return false; }
        std::wstring command = quote(request.runtime.wstring()) + (request.mode == Mode::run ? L" run " : L" check ") +
                               quote(request.title.wstring());
        if (request.mode == Mode::run) {
            command += L" --storage " + quote(request.storage.wstring());
            command += request.time_limit_ms ? L" --timeout-ms " + std::to_wstring(request.time_limit_ms) : L" --no-deadline";
        }
        job_.reset(CreateJobObjectW(nullptr, nullptr));
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!job_.value || !SetInformationJobObject(job_.value, JobObjectExtendedLimitInformation, &limits, sizeof limits)) {
            error = "cannot create session job: Win32 " + std::to_string(GetLastError()); return false;
        }
        STARTUPINFOW startup{sizeof startup};
        startup.dwFlags = STARTF_USESTDHANDLES;
        startup.hStdInput = nullptr;
        startup.hStdOutput = log.value;
        startup.hStdError = log.value;
        PROCESS_INFORMATION info{};
        const auto directory = request.runtime.parent_path().wstring();
        if (!CreateProcessW(request.runtime.c_str(), command.data(), nullptr, nullptr, TRUE,
                            CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, directory.c_str(), &startup, &info)) {
            error = "cannot start ERA runtime: Win32 " + std::to_string(GetLastError()); return false;
        }
        Handle thread;
        thread.reset(info.hThread);
        process_.reset(info.hProcess);
        pid_ = info.dwProcessId;
        // Opened by xone-era's suspend watch (era_core_application.cpp) under this name.
        suspend_.reset(request.mode == Mode::run ?
            CreateEventW(nullptr, TRUE, FALSE, (L"Local\\xone-era-suspend-" + std::to_wstring(pid_)).c_str()) : nullptr);
        grace_ = std::chrono::milliseconds(request.suspend_grace_ms);
        kill_at_.reset();
        if (!AssignProcessToJobObject(job_.value, process_.value)) {
            const DWORD failure = GetLastError();
            TerminateProcess(process_.value, 1);
            process_.reset();
            error = "cannot supervise ERA runtime: Win32 " + std::to_string(failure); return false;
        }
        ResumeThread(thread.value);
        log_ = request.log;
        terminated_ = false;
        started_ = std::chrono::steady_clock::now();
        return true;
    }

    [[nodiscard]] bool active() const override { return process_.value != nullptr; }

    bool poll(Result& result) override {
        if (!active()) return false;
        if (kill_at_ && std::chrono::steady_clock::now() >= *kill_at_) { TerminateJobObject(job_.value, 1); kill_at_.reset(); }
        if (WaitForSingleObject(process_.value, 0) != WAIT_OBJECT_0) return false;
        DWORD code = 0;
        GetExitCodeProcess(process_.value, &code);
        process_.reset();
        job_.reset();
        suspend_.reset();
        kill_at_.reset();
        result = interpret(static_cast<int>(code), terminated_, read_tail(log_));
        result.duration_ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started_).count());
        return true;
    }

    void terminate() override {
        if (!active()) return;
        terminated_ = true;
        if (suspend_.value && !kill_at_ && SetEvent(suspend_.value)) {
            kill_at_ = std::chrono::steady_clock::now() + grace_;
            return;
        }
        kill_at_.reset();
        TerminateJobObject(job_.value, 1);
    }

    bool focus_title() override {
        if (!active()) return false;
        struct Search { DWORD pid; HWND found; } search{pid_, nullptr};
        EnumWindows([](HWND window, LPARAM data) -> BOOL {
            auto& s = *reinterpret_cast<Search*>(data);
            DWORD owner = 0;
            GetWindowThreadProcessId(window, &owner);
            if (owner != s.pid || !IsWindowVisible(window) || GetWindow(window, GW_OWNER)) return TRUE;
            s.found = window;
            return FALSE;
        }, reinterpret_cast<LPARAM>(&search));
        if (!search.found) return false;
        if (IsIconic(search.found)) ShowWindow(search.found, SW_RESTORE);
        return SetForegroundWindow(search.found) != FALSE;
    }

private:
    Handle process_, job_, suspend_;
    std::chrono::milliseconds grace_{0};
    std::optional<std::chrono::steady_clock::time_point> kill_at_;
    DWORD pid_ = 0;
    bool terminated_ = false;
    std::filesystem::path log_;
    std::chrono::steady_clock::time_point started_{};
};
} // namespace

std::unique_ptr<Host> create_host() { return std::make_unique<WindowsHost>(); }
#else
namespace {
class UnsupportedHost final : public Host {
public:
    bool start(const Request&, std::string& error) override {
        error = "ERA title execution is implemented for Windows x64 only on this revision";
        return false;
    }
    [[nodiscard]] bool active() const override { return false; }
    bool poll(Result&) override { return false; }
    void terminate() override {}
    bool focus_title() override { return false; }
};
} // namespace
std::unique_ptr<Host> create_host() { return std::make_unique<UnsupportedHost>(); }
#endif

} // namespace xboxone::session
