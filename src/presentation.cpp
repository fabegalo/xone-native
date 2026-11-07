// SPDX-License-Identifier: BSD-3-Clause
#include "xboxone_native/presentation.h"
#include "presentation_backend.h"

#include <utility>

namespace xboxone::presentation {
namespace {

bool is_valid_sha256_hex(std::string_view hex) noexcept {
    if (hex.size() != 64) return false;
    for (const char c : hex) {
        const bool digit = (c >= '0' && c <= '9');
        const bool lower = (c >= 'a' && c <= 'f');
        const bool upper = (c >= 'A' && c <= 'F');
        if (!digit && !lower && !upper) return false;
    }
    return true;
}

bool valid_dimensions(std::uint32_t width, std::uint32_t height, std::string& error) {
    if (width == 0 || height == 0) {
        error = "Frame dimensions must be greater than zero";
        return false;
    }
    if (width > max_frame_dimension || height > max_frame_dimension) {
        error = "Frame dimensions exceed maximum supported size (16384x16384)";
        return false;
    }
    return true;
}

} // namespace

BackendSupport query_backend_support(BackendKind backend, bool allow_software_adapter) {
    switch (backend) {
        case BackendKind::d3d12:
#ifdef _WIN32
            return query_d3d12_backend_support(allow_software_adapter);
#else
            (void)allow_software_adapter;
            return BackendSupport{
                .backend = BackendKind::d3d12,
                .compiled_in = false,
                .runtime_available = false,
                .software_adapter = false,
                .adapter_name = {},
                .status_reason = "Direct3D 12 presentation backend is only available on Windows targets",
            };
#endif
        case BackendKind::vulkan:
            return BackendSupport{
                .backend = BackendKind::vulkan,
                .compiled_in = false,
                .runtime_available = false,
                .software_adapter = false,
                .adapter_name = {},
                .status_reason = "Vulkan presentation backend interface is prepared, but Vulkan swapchain execution is not yet implemented",
            };
        case BackendKind::metal:
            return BackendSupport{
                .backend = BackendKind::metal,
                .compiled_in = false,
                .runtime_available = false,
                .software_adapter = false,
                .adapter_name = {},
                .status_reason = "Metal presentation backend interface is prepared, but Metal swapchain execution is not yet implemented",
            };
    }
    return BackendSupport{
        .backend = backend,
        .compiled_in = false,
        .runtime_available = false,
        .software_adapter = false,
        .adapter_name = {},
        .status_reason = "Unknown presentation backend kind",
    };
}

bool validate_provenance(const FrameProvenance& provenance, std::string& error) {
    if (provenance.component_id.empty()) {
        error = "Frame provenance requires a non-empty component_id";
        return false;
    }
    switch (provenance.origin) {
        case FrameOrigin::synthetic_test:
        case FrameOrigin::external_host_buffer:
            return true;
        case FrameOrigin::systemos_component:
            if (!is_valid_sha256_hex(provenance.component_sha256)) {
                error = "SystemOS frame provenance requires a verified 64-character hex SHA-256 of the authorized component";
                return false;
            }
            if (provenance.system_version_raw == 0) {
                error = "SystemOS frame provenance requires a non-zero raw SystemOS revision";
                return false;
            }
            return true;
    }
    error = "Unknown frame provenance origin";
    return false;
}

bool validate_config(const PresenterConfig& config, std::string& error) {
    if (config.window_title.empty()) {
        error = "Presenter window_title must not be empty";
        return false;
    }
    if (!valid_dimensions(config.width, config.height, error)) {
        return false;
    }
    if (config.buffer_count < min_swapchain_buffers || config.buffer_count > max_swapchain_buffers) {
        error = "Swapchain buffer_count must be between 2 and 16";
        return false;
    }
    if (bytes_per_pixel(config.swapchain_format) == 0) {
        error = "Unsupported swapchain pixel format";
        return false;
    }
    return true;
}

bool validate_external_frame(const ExternalFrameView& frame, std::string& error) {
    if (!valid_dimensions(frame.width, frame.height, error)) {
        return false;
    }
    const std::uint32_t bpp = bytes_per_pixel(frame.format);
    if (bpp == 0) {
        error = "Unsupported external frame pixel format";
        return false;
    }
    const std::uint64_t min_row_bytes = static_cast<std::uint64_t>(frame.width) * bpp;
    if (frame.row_pitch_bytes < min_row_bytes) {
        error = "External frame row_pitch_bytes is smaller than width * bytes_per_pixel";
        return false;
    }
    const std::uint64_t required_bytes =
        static_cast<std::uint64_t>(frame.height - 1) * frame.row_pitch_bytes + min_row_bytes;
    if (frame.pixels.size() < required_bytes) {
        error = "External frame pixel buffer is smaller than required height * row_pitch_bytes";
        return false;
    }
    return validate_provenance(frame.provenance, error);
}

bool validate_systemos_surface_handoff(const SystemOsSurfaceHandoffDesc& desc, std::string& error) {
    if (desc.provenance.origin != FrameOrigin::systemos_component) {
        error = "SystemOS surface handoff requires FrameOrigin::systemos_component provenance";
        return false;
    }
    if (!validate_provenance(desc.provenance, error)) {
        return false;
    }
    if (!valid_dimensions(desc.width, desc.height, error)) {
        return false;
    }
    if (bytes_per_pixel(desc.format) == 0) {
        error = "SystemOS surface handoff uses an unsupported pixel format";
        return false;
    }
    if (desc.native_handle == nullptr) {
        error = "SystemOS surface handoff requires a non-null surface handle or buffer pointer";
        return false;
    }
    return true;
}

bool make_synthetic_test_image(std::uint32_t width, std::uint32_t height,
                               std::uint32_t frame_index, PixelFormat format,
                               SyntheticImage& out_image, std::string& error) {
    if (!valid_dimensions(width, height, error)) {
        return false;
    }
    const std::uint32_t bpp = bytes_per_pixel(format);
    if (bpp == 0) {
        error = "Unsupported pixel format for synthetic test image";
        return false;
    }

    const std::uint32_t row_pitch = width * bpp;
    const std::size_t total_bytes = static_cast<std::size_t>(row_pitch) * height;
    out_image.pixels.resize(total_bytes);
    out_image.width = width;
    out_image.height = height;
    out_image.row_pitch_bytes = row_pitch;
    out_image.format = format;
    out_image.provenance = FrameProvenance{
        .origin = FrameOrigin::synthetic_test,
        .component_id = "xboxone_native.synthetic_presentation_test",
        .component_sha256 = {},
        .system_version_raw = 0,
    };

    for (std::uint32_t y = 0; y < height; ++y) {
        std::uint8_t* row = out_image.pixels.data() + static_cast<std::size_t>(y) * row_pitch;
        for (std::uint32_t x = 0; x < width; ++x) {
            const bool checker = (((x / 8u) + (y / 8u) + frame_index) & 1u) != 0;
            const std::uint8_t r = static_cast<std::uint8_t>((x * 255u / width + frame_index * 17u) & 0xFFu);
            const std::uint8_t g = static_cast<std::uint8_t>((y * 255u / height + frame_index * 31u) & 0xFFu);
            const std::uint8_t b = checker ? 0xCCu : 0x33u;
            const std::uint8_t a = 0xFFu;

            std::uint8_t* px = row + static_cast<std::size_t>(x) * 4u;
            if (format == PixelFormat::rgba8_unorm) {
                px[0] = r;
                px[1] = g;
                px[2] = b;
                px[3] = a;
            } else {
                px[0] = b;
                px[1] = g;
                px[2] = r;
                px[3] = a;
            }
        }
    }
    return true;
}

Presenter::Presenter() = default;
Presenter::~Presenter() { shutdown(); }
Presenter::Presenter(Presenter&&) noexcept = default;
Presenter& Presenter::operator=(Presenter&&) noexcept = default;

bool Presenter::initialize(const PresenterConfig& config, std::string& error) {
    if (is_initialized()) {
        error = "Presenter is already initialized; call shutdown() before reinitializing";
        return false;
    }
    if (!validate_config(config, error)) {
        return false;
    }

    switch (config.backend) {
        case BackendKind::d3d12: {
#ifdef _WIN32
            auto instance = create_d3d12_backend();
            if (!instance->initialize(config, error)) {
                return false;
            }
            backend_ = std::move(instance);
            return true;
#else
            error = "Direct3D 12 presentation backend is only supported on Windows";
            return false;
#endif
        }
        case BackendKind::vulkan:
            error = "Vulkan presentation backend interface is prepared, but Vulkan swapchain execution is not yet implemented";
            return false;
        case BackendKind::metal:
            error = "Metal presentation backend interface is prepared, but Metal swapchain execution is not yet implemented";
            return false;
    }
    error = "Unsupported presentation backend kind";
    return false;
}

void Presenter::close_window() noexcept {
    if (backend_) {
        backend_->close_window();
    }
}

void Presenter::shutdown() noexcept {
    if (backend_) {
        backend_->shutdown();
        backend_.reset();
    }
}

bool Presenter::is_initialized() const noexcept {
    return backend_ && backend_->is_initialized();
}

PresenterStats Presenter::stats() const {
    if (!backend_) {
        return PresenterStats{};
    }
    return backend_->stats();
}

bool Presenter::pump_messages(bool& quit_requested, std::string& error) {
    if (!is_initialized()) {
        error = "Cannot pump messages: presenter is not initialized";
        return false;
    }
    return backend_->pump_messages(quit_requested, error);
}

bool Presenter::resize(std::uint32_t new_width, std::uint32_t new_height, std::string& error) {
    if (!is_initialized()) {
        error = "Cannot resize swapchain: presenter is not initialized";
        return false;
    }
    if (!valid_dimensions(new_width, new_height, error)) {
        return false;
    }
    return backend_->resize(new_width, new_height, error);
}

bool Presenter::present_frame(const ExternalFrameView& frame, std::string& error) {
    if (!is_initialized()) {
        error = "Cannot present frame: presenter is not initialized";
        return false;
    }
    if (!validate_external_frame(frame, error)) {
        return false;
    }
    return backend_->present_frame(frame, error);
}

bool Presenter::wait_for_gpu(std::string& error) {
    if (!is_initialized()) {
        error = "Cannot synchronize GPU: presenter is not initialized";
        return false;
    }
    return backend_->wait_for_gpu(error);
}

bool Presenter::recover_swapchain(std::string& error) {
    if (!is_initialized()) {
        error = "Cannot recover swapchain: presenter is not initialized";
        return false;
    }
    return backend_->recover_swapchain(error);
}

bool Presenter::readback_last_backbuffer(std::vector<std::uint8_t>& out_pixels,
                                         std::uint32_t& out_width,
                                         std::uint32_t& out_height,
                                         PixelFormat& out_format,
                                         std::string& error) {
    if (!is_initialized()) {
        error = "Cannot read back backbuffer: presenter is not initialized";
        return false;
    }
    return backend_->readback_last_backbuffer(out_pixels, out_width, out_height, out_format, error);
}

} // namespace xboxone::presentation
