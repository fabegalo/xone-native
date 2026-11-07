// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace xboxone::presentation {

enum class BackendKind {
    d3d12,
    vulkan,
    metal,
};

enum class PixelFormat {
    rgba8_unorm,
    bgra8_unorm,
};

// Tracks frame origin so synthetic lifecycle tests and host buffers are never
// misreported as authentic Xbox One SystemOS output.
enum class FrameOrigin {
    synthetic_test,
    external_host_buffer,
    systemos_component,
};

struct FrameProvenance {
    FrameOrigin origin = FrameOrigin::synthetic_test;
    std::string component_id = "synthetic_presentation_test";
    // Required (64 lowercase/uppercase hex chars) when origin == systemos_component.
    std::string component_sha256;
    // Required (non-zero raw revision) when origin == systemos_component.
    std::uint64_t system_version_raw = 0;
};

struct ExternalFrameView {
    std::span<const std::uint8_t> pixels;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t row_pitch_bytes = 0;
    PixelFormat format = PixelFormat::rgba8_unorm;
    FrameProvenance provenance{};
};

struct SyntheticImage {
    std::vector<std::uint8_t> pixels;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t row_pitch_bytes = 0;
    PixelFormat format = PixelFormat::rgba8_unorm;
    FrameProvenance provenance{};

    [[nodiscard]] ExternalFrameView view() const noexcept {
        return ExternalFrameView{
            .pixels = pixels,
            .width = width,
            .height = height,
            .row_pitch_bytes = row_pitch_bytes,
            .format = format,
            .provenance = provenance,
        };
    }
};

// Contract prepared for future SystemOS composition/DXGI surface handoff once
// an authorized, readable SystemOS shell component and its dependencies exist.
enum class SurfaceHandleKind {
    cpu_staging_buffer,
    d3d12_resource,
    dxgi_shared_handle,
};

struct SystemOsSurfaceHandoffDesc {
    SurfaceHandleKind handle_kind = SurfaceHandleKind::cpu_staging_buffer;
    void* native_handle = nullptr;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    PixelFormat format = PixelFormat::bgra8_unorm;
    FrameProvenance provenance{};
};

struct BackendSupport {
    BackendKind backend = BackendKind::d3d12;
    bool compiled_in = false;
    bool runtime_available = false;
    bool software_adapter = false;
    std::string adapter_name;
    std::string status_reason;
};

struct PresenterConfig {
    BackendKind backend = BackendKind::d3d12;
    std::string window_title = "xboxone-native presentation test";
    std::uint32_t width = 1280;
    std::uint32_t height = 720;
    std::uint32_t buffer_count = 2;
    PixelFormat swapchain_format = PixelFormat::rgba8_unorm;
    bool vsync = false;
    bool visible = false;
    bool allow_software_adapter = true;
    bool prefer_software_adapter = false;
};

struct PresenterStats {
    BackendKind backend = BackendKind::d3d12;
    bool initialized = false;
    bool window_open = false;
    bool using_software_adapter = false;
    std::string adapter_name;
    std::string active_window_title;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t buffer_count = 0;
    PixelFormat swapchain_format = PixelFormat::rgba8_unorm;
    std::uint64_t presented_frames = 0;
    std::uint64_t synthetic_frames = 0;
    std::uint64_t external_host_frames = 0;
    std::uint64_t authentic_systemos_frames = 0;
    std::uint64_t resize_count = 0;
    std::uint64_t recovered_error_count = 0;
    std::uint64_t last_submitted_fence_value = 0;
    std::uint64_t last_completed_fence_value = 0;
    std::uint32_t current_backbuffer_index = 0;
};

inline constexpr std::uint32_t max_frame_dimension = 16384;
inline constexpr std::uint32_t min_swapchain_buffers = 2;
inline constexpr std::uint32_t max_swapchain_buffers = 16;

[[nodiscard]] constexpr std::uint32_t bytes_per_pixel(PixelFormat format) noexcept {
    switch (format) {
        case PixelFormat::rgba8_unorm:
        case PixelFormat::bgra8_unorm:
            return 4;
    }
    return 0;
}

[[nodiscard]] constexpr std::string_view backend_name(BackendKind backend) noexcept {
    switch (backend) {
        case BackendKind::d3d12:
            return "d3d12";
        case BackendKind::vulkan:
            return "vulkan";
        case BackendKind::metal:
            return "metal";
    }
    return "unknown";
}

[[nodiscard]] constexpr std::string_view pixel_format_name(PixelFormat format) noexcept {
    switch (format) {
        case PixelFormat::rgba8_unorm:
            return "rgba8_unorm";
        case PixelFormat::bgra8_unorm:
            return "bgra8_unorm";
    }
    return "unknown";
}

[[nodiscard]] BackendSupport query_backend_support(BackendKind backend,
                                                   bool allow_software_adapter = true);

bool validate_provenance(const FrameProvenance& provenance, std::string& error);
bool validate_config(const PresenterConfig& config, std::string& error);
bool validate_external_frame(const ExternalFrameView& frame, std::string& error);
bool validate_systemos_surface_handoff(const SystemOsSurfaceHandoffDesc& desc, std::string& error);

// Generates a deterministic test pattern explicitly marked as FrameOrigin::synthetic_test.
// Never represents or substitutes an original Xbox One SystemOS frame.
bool make_synthetic_test_image(std::uint32_t width, std::uint32_t height,
                               std::uint32_t frame_index, PixelFormat format,
                               SyntheticImage& out_image, std::string& error);

class BackendInstance;

class Presenter {
public:
    Presenter();
    ~Presenter();

    Presenter(const Presenter&) = delete;
    Presenter& operator=(const Presenter&) = delete;
    Presenter(Presenter&&) noexcept;
    Presenter& operator=(Presenter&&) noexcept;

    bool initialize(const PresenterConfig& config, std::string& error);
    void close_window() noexcept;
    void shutdown() noexcept;

    [[nodiscard]] bool is_initialized() const noexcept;
    [[nodiscard]] PresenterStats stats() const;

    bool pump_messages(bool& quit_requested, std::string& error);
    bool resize(std::uint32_t new_width, std::uint32_t new_height, std::string& error);
    bool present_frame(const ExternalFrameView& frame, std::string& error);
    bool wait_for_gpu(std::string& error);

    // Rebuilds swapchain and command resources to recover from swapchain/pipeline faults
    // without destroying the host window or losing provenance counters.
    bool recover_swapchain(std::string& error);

    // Diagnostic GPU-to-CPU readback of the most recently presented backbuffer.
    // Optional and off the main presentation path; used by synthetic tests to prove
    // that pixels reached the GPU swapchain surface.
    bool readback_last_backbuffer(std::vector<std::uint8_t>& out_pixels,
                                  std::uint32_t& out_width,
                                  std::uint32_t& out_height,
                                  PixelFormat& out_format,
                                  std::string& error);

private:
    std::unique_ptr<BackendInstance> backend_;
};

} // namespace xboxone::presentation
