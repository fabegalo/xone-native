// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include "xboxone_native/presentation.h"

#include <memory>
#include <string>
#include <vector>

namespace xboxone::presentation {

class BackendInstance {
public:
    virtual ~BackendInstance() = default;

    virtual bool initialize(const PresenterConfig& config, std::string& error) = 0;
    virtual void close_window() noexcept = 0;
    virtual void shutdown() noexcept = 0;
    [[nodiscard]] virtual bool is_initialized() const noexcept = 0;
    [[nodiscard]] virtual PresenterStats stats() const = 0;

    virtual bool pump_messages(bool& quit_requested, std::string& error) = 0;
    virtual bool resize(std::uint32_t new_width, std::uint32_t new_height, std::string& error) = 0;
    virtual bool present_frame(const ExternalFrameView& frame, std::string& error) = 0;
    virtual bool wait_for_gpu(std::string& error) = 0;
    virtual bool recover_swapchain(std::string& error) = 0;
    virtual bool readback_last_backbuffer(std::vector<std::uint8_t>& out_pixels,
                                          std::uint32_t& out_width,
                                          std::uint32_t& out_height,
                                          PixelFormat& out_format,
                                          std::string& error) = 0;
};

#ifdef _WIN32
BackendSupport query_d3d12_backend_support(bool allow_software_adapter);
std::unique_ptr<BackendInstance> create_d3d12_backend();
#endif

} // namespace xboxone::presentation
