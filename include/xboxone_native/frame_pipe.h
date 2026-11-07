// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include "xboxone_native/presentation.h"
#include <filesystem>

namespace xboxone::presentation {
// Windows desktop CPU staging transport. One producer/consumer, one thread per
// endpoint. No deployment, injection, remote clients or SystemOS certification.
// shortcut: pixels cross a local pipe, replace transport with shared GPU resources
// when an observed compositor contract and measurements justify that change.
class FramePipe {
public:
    FramePipe();
    ~FramePipe();
    FramePipe(const FramePipe&) = delete;
    FramePipe& operator=(const FramePipe&) = delete;
    bool create(std::string& error);
    const std::wstring& endpoint() const;
    // Retains the exact process object and a read-only executable lock. Caller
    // supplies a process HANDLE obtained by activation or owned process creation.
    bool accept(void* producer_process, const std::filesystem::path& executable,
                const std::string& sha256, std::uint32_t timeout_ms, std::string& error);
    bool connect(const std::wstring& endpoint, std::uint32_t server_pid, std::string& error);
    bool send(const ExternalFrameView&, std::uint32_t timeout_ms, std::string& error);
    // Output owns no memory: pixels remain valid until next receive/close.
    // Always external_host_buffer, irrespective of sender declarations.
    bool receive(ExternalFrameView&, std::uint32_t timeout_ms, std::string& error);
    void close() noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace xboxone::presentation
