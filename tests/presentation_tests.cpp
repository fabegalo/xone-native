// SPDX-License-Identifier: BSD-3-Clause
#include "xboxone_native/presentation.h"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <span>
#include <string>
#include <vector>

namespace {

using namespace xboxone::presentation;

int failures = 0;
int checks_run = 0;

void check(bool condition, const std::string& message) {
    ++checks_run;
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

void test_backend_contracts_and_validation() {
    const auto vk_support = query_backend_support(BackendKind::vulkan);
    check(!vk_support.compiled_in && !vk_support.runtime_available,
          "Vulkan backend reports not compiled/available until implemented");
    check(!vk_support.status_reason.empty(), "Vulkan backend provides explicit status reason");

    const auto mtl_support = query_backend_support(BackendKind::metal);
    check(!mtl_support.compiled_in && !mtl_support.runtime_available,
          "Metal backend reports not compiled/available until implemented");
    check(!mtl_support.status_reason.empty(), "Metal backend provides explicit status reason");

    Presenter unimplemented_presenter;
    std::string err;
    PresenterConfig vk_cfg{};
    vk_cfg.backend = BackendKind::vulkan;
    check(!unimplemented_presenter.initialize(vk_cfg, err) && !err.empty(),
          "Initializing Vulkan presenter fails with explicit error (no stub)");
    check(!unimplemented_presenter.is_initialized(), "Vulkan presenter remains uninitialized");

    PresenterConfig mtl_cfg{};
    mtl_cfg.backend = BackendKind::metal;
    check(!unimplemented_presenter.initialize(mtl_cfg, err) && !err.empty(),
          "Initializing Metal presenter fails with explicit error (no stub)");

    PresenterConfig bad_cfg{};
    bad_cfg.window_title.clear();
    check(!validate_config(bad_cfg, err), "Reject empty window title");

    bad_cfg = PresenterConfig{};
    bad_cfg.width = 0;
    check(!validate_config(bad_cfg, err), "Reject zero width config");

    bad_cfg = PresenterConfig{};
    bad_cfg.height = max_frame_dimension + 1;
    check(!validate_config(bad_cfg, err), "Reject oversized height config");

    bad_cfg = PresenterConfig{};
    bad_cfg.buffer_count = 1;
    check(!validate_config(bad_cfg, err), "Reject single-buffer flip swapchain config");

    // Provenance validation: SystemOS frames require verified SHA-256 and version.
    FrameProvenance fake_systemos{};
    fake_systemos.origin = FrameOrigin::systemos_component;
    fake_systemos.component_id = "shell.exe";
    check(!validate_provenance(fake_systemos, err),
          "Reject SystemOS provenance without 64-hex SHA-256");

    fake_systemos.component_sha256 =
        "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    fake_systemos.system_version_raw = 0;
    check(!validate_provenance(fake_systemos, err),
          "Reject SystemOS provenance without raw system version");

    fake_systemos.system_version_raw = 0x800a000065f425afULL;
    check(validate_provenance(fake_systemos, err),
          "Accept complete SystemOS provenance contract: " + err);

    // Future SystemOS GPU surface handoff contract validation.
    int dummy_surface = 1;
    SystemOsSurfaceHandoffDesc handoff{};
    handoff.handle_kind = SurfaceHandleKind::d3d12_resource;
    handoff.native_handle = &dummy_surface;
    handoff.width = 1920;
    handoff.height = 1080;
    handoff.format = PixelFormat::bgra8_unorm;
    handoff.provenance = FrameProvenance{}; // synthetic_test by default -> must fail
    check(!validate_systemos_surface_handoff(handoff, err),
          "Reject SystemOS surface handoff with synthetic provenance");

    handoff.provenance = fake_systemos;
    check(validate_systemos_surface_handoff(handoff, err),
          "Accept valid SystemOS surface handoff contract: " + err);

    handoff.native_handle = nullptr;
    check(!validate_systemos_surface_handoff(handoff, err),
          "Reject SystemOS surface handoff with null native handle");
}

#ifdef _WIN32
void test_d3d12_lifecycle_resize_and_recovery() {
    const auto d3d_support = query_backend_support(BackendKind::d3d12, true);
    check(d3d_support.compiled_in, "D3D12 backend compiled in on Windows");
    check(d3d_support.runtime_available,
          "D3D12 runtime available on Windows: " + d3d_support.status_reason);
    check(!d3d_support.adapter_name.empty(), "D3D12 adapter name populated");
    std::cout << "D3D12 adapter: " << d3d_support.adapter_name
              << " (software=" << (d3d_support.software_adapter ? "true" : "false") << ")\n";

    Presenter presenter;
    std::string err;

    // Operations before initialization must fail cleanly.
    bool quit = false;
    check(!presenter.pump_messages(quit, err), "Reject pump_messages before init");
    check(!presenter.resize(640, 480, err), "Reject resize before init");
    check(!presenter.wait_for_gpu(err), "Reject wait_for_gpu before init");
    check(!presenter.recover_swapchain(err), "Reject recover_swapchain before init");

    PresenterConfig cfg{};
    cfg.backend = BackendKind::d3d12;
    cfg.window_title = "D3D12 Synthetic Lifecycle Test";
    cfg.width = 256;
    cfg.height = 144;
    cfg.buffer_count = 2;
    cfg.swapchain_format = PixelFormat::rgba8_unorm;
    cfg.vsync = false;
    cfg.visible = false;
    cfg.allow_software_adapter = true;

    check(presenter.initialize(cfg, err), "Initialize Win32 + D3D12 presenter: " + err);
    check(presenter.is_initialized(), "Presenter reports initialized");

    // Double initialization must fail without breaking active presenter.
    check(!presenter.initialize(cfg, err), "Reject double initialize without shutdown");
    check(presenter.is_initialized(), "Presenter stays initialized after rejected double-init");

    const auto init_stats = presenter.stats();
    check(init_stats.window_open, "Win32 window is open");
    check(init_stats.width == 256 && init_stats.height == 144, "Initial swapchain dimensions match");
    check(init_stats.active_window_title.find("[SYNTHETIC TEST - NOT SYSTEMOS]") == 0,
          "Window title explicitly marks synthetic test and not SystemOS dashboard");
    check(init_stats.authentic_systemos_frames == 0, "Zero authentic SystemOS frames reported");

    // Readback before presenting any frame must fail cleanly.
    std::vector<std::uint8_t> readback_pixels;
    std::uint32_t rb_w = 0, rb_h = 0;
    PixelFormat rb_fmt = PixelFormat::rgba8_unorm;
    check(!presenter.readback_last_backbuffer(readback_pixels, rb_w, rb_h, rb_fmt, err),
          "Reject readback before any frame is presented");

    // Present synthetic test frames and verify GPU backbuffer readback matches byte-for-byte.
    for (std::uint32_t frame_idx = 0; frame_idx < 4; ++frame_idx) {
        SyntheticImage synth;
        check(make_synthetic_test_image(256, 144, frame_idx, PixelFormat::rgba8_unorm, synth, err),
              "Generate synthetic RGBA8 test image: " + err);
        check(synth.provenance.origin == FrameOrigin::synthetic_test,
              "Synthetic image carries synthetic_test origin");

        check(presenter.present_frame(synth.view(), err),
              "Present synthetic frame " + std::to_string(frame_idx) + ": " + err);
        check(presenter.pump_messages(quit, err) && !quit,
              "Pump Win32 messages during frame loop: " + err);

        if (frame_idx == 0 || frame_idx == 3) {
            check(presenter.readback_last_backbuffer(readback_pixels, rb_w, rb_h, rb_fmt, err),
                  "Read back D3D12 backbuffer for frame " + std::to_string(frame_idx) + ": " + err);
            check(rb_w == 256 && rb_h == 144 && rb_fmt == PixelFormat::rgba8_unorm,
                  "Readback metadata matches 256x144 RGBA8");
            check(readback_pixels == synth.pixels,
                  "GPU backbuffer pixels exactly match synthetic test image " + std::to_string(frame_idx));
        }
    }

    const auto after_four = presenter.stats();
    check(after_four.presented_frames == 4 && after_four.synthetic_frames == 4,
          "Frame counters record 4 synthetic frames");
    check(after_four.authentic_systemos_frames == 0,
          "Synthetic frames never increment authentic_systemos_frames");
    check(after_four.last_submitted_fence_value >= 4 &&
          after_four.last_completed_fence_value >= 4,
          "GPU fence values advance and complete");

    // Test cross-format presentation: submit BGRA8 external buffer to RGBA8 swapchain.
    {
        SyntheticImage bgra_img;
        check(make_synthetic_test_image(256, 144, 7, PixelFormat::bgra8_unorm, bgra_img, err),
              "Generate synthetic BGRA8 test image: " + err);
        bgra_img.provenance.origin = FrameOrigin::external_host_buffer;
        bgra_img.provenance.component_id = "external_bgra_test_buffer";

        SyntheticImage expected_rgba;
        check(make_synthetic_test_image(256, 144, 7, PixelFormat::rgba8_unorm, expected_rgba, err),
              "Generate reference RGBA8 image: " + err);

        check(presenter.present_frame(bgra_img.view(), err),
              "Present BGRA8 external buffer onto RGBA8 swapchain: " + err);
        check(presenter.readback_last_backbuffer(readback_pixels, rb_w, rb_h, rb_fmt, err),
              "Read back converted BGRA8->RGBA8 backbuffer: " + err);
        check(readback_pixels == expected_rgba.pixels,
              "BGRA8 external buffer swizzled accurately to RGBA8 swapchain backbuffer");
        check(presenter.stats().external_host_frames == 1,
              "external_host_frames counter incremented");
    }

    // Test external buffer with non-tight (padded) row_pitch_bytes.
    {
        SyntheticImage tight_ref;
        check(make_synthetic_test_image(256, 144, 11, PixelFormat::rgba8_unorm, tight_ref, err),
              "Generate reference tight RGBA8 image: " + err);

        const std::uint32_t padded_pitch = 256 * 4 + 76; // Non-multiple of 256 with trailing padding
        std::vector<std::uint8_t> padded_pixels(static_cast<std::size_t>(padded_pitch) * 144, 0xAAu);
        for (std::uint32_t y = 0; y < 144; ++y) {
            std::copy_n(
                tight_ref.pixels.data() + static_cast<std::size_t>(y) * (256 * 4),
                256 * 4,
                padded_pixels.data() + static_cast<std::size_t>(y) * padded_pitch);
        }

        ExternalFrameView padded_view{};
        padded_view.pixels = padded_pixels;
        padded_view.width = 256;
        padded_view.height = 144;
        padded_view.row_pitch_bytes = padded_pitch;
        padded_view.format = PixelFormat::rgba8_unorm;
        padded_view.provenance.origin = FrameOrigin::external_host_buffer;
        padded_view.provenance.component_id = "padded_pitch_external_buffer";

        check(presenter.present_frame(padded_view, err),
              "Present external buffer with custom row_pitch_bytes: " + err);
        check(presenter.readback_last_backbuffer(readback_pixels, rb_w, rb_h, rb_fmt, err) &&
              readback_pixels == tight_ref.pixels,
              "Padded external buffer uploaded without row-padding corruption");
    }

    // Test resize lifecycle across multiple resolutions.
    const std::pair<std::uint32_t, std::uint32_t> resolutions[] = {
        {320, 180},
        {192, 128},
    };
    for (const auto& [w, h] : resolutions) {
        check(presenter.resize(w, h, err),
              "Resize swapchain to " + std::to_string(w) + "x" + std::to_string(h) + ": " + err);
        const auto st = presenter.stats();
        check(st.width == w && st.height == h, "Swapchain stats reflect resized dimensions");

        SyntheticImage resized_img;
        check(make_synthetic_test_image(w, h, w + h, PixelFormat::rgba8_unorm, resized_img, err),
              "Generate synthetic image for resized swapchain: " + err);
        check(presenter.present_frame(resized_img.view(), err),
              "Present frame after resize: " + err);
        check(presenter.readback_last_backbuffer(readback_pixels, rb_w, rb_h, rb_fmt, err),
              "Readback after resize: " + err);
        check(rb_w == w && rb_h == h && readback_pixels == resized_img.pixels,
              "Resized backbuffer readback matches exact pixels at " +
              std::to_string(w) + "x" + std::to_string(h));
    }
    check(presenter.stats().resize_count == 2, "Resize counter equals 2");

    // Error recovery 1: invalid resize must fail and leave existing 192x128 swapchain operational.
    check(!presenter.resize(0, 128, err), "Reject zero-width resize");
    check(!presenter.resize(192, max_frame_dimension + 10, err), "Reject oversized resize");
    check(presenter.stats().width == 192 && presenter.stats().height == 128,
          "Swapchain dimensions preserved after rejected resize");

    // Error recovery 2: mismatched frame dimensions and truncated buffer must fail without corrupting GPU state.
    {
        SyntheticImage wrong_size;
        check(make_synthetic_test_image(64, 64, 1, PixelFormat::rgba8_unorm, wrong_size, err),
              "Create mismatched 64x64 frame");
        check(!presenter.present_frame(wrong_size.view(), err),
              "Reject frame whose dimensions do not match swapchain");

        ExternalFrameView truncated = wrong_size.view();
        truncated.width = 192;
        truncated.height = 128;
        truncated.row_pitch_bytes = 192 * 4;
        truncated.pixels = std::span<const std::uint8_t>(wrong_size.pixels.data(), 100);
        check(!presenter.present_frame(truncated, err),
              "Reject truncated external pixel buffer");

        // Immediately present a valid 192x128 frame to prove error recovery.
        SyntheticImage valid_after_err;
        check(make_synthetic_test_image(192, 128, 99, PixelFormat::rgba8_unorm, valid_after_err, err),
              "Create valid 192x128 recovery frame");
        check(presenter.present_frame(valid_after_err.view(), err),
              "Presenter recovers immediately after rejected frames: " + err);
        check(presenter.readback_last_backbuffer(readback_pixels, rb_w, rb_h, rb_fmt, err) &&
              readback_pixels == valid_after_err.pixels,
              "Backbuffer readback matches after frame validation recovery");
    }

    // Error recovery 3: swapchain/pipeline reconstruction via recover_swapchain().
    {
        check(presenter.recover_swapchain(err), "Recover swapchain and command pipeline: " + err);
        check(presenter.stats().recovered_error_count == 1,
              "recovered_error_count incremented after swapchain recovery");

        SyntheticImage post_recovery;
        check(make_synthetic_test_image(192, 128, 42, PixelFormat::rgba8_unorm, post_recovery, err),
              "Create post-recovery synthetic frame");
        check(presenter.present_frame(post_recovery.view(), err),
              "Present frame after swapchain recovery: " + err);
        check(presenter.readback_last_backbuffer(readback_pixels, rb_w, rb_h, rb_fmt, err) &&
              readback_pixels == post_recovery.pixels,
              "GPU readback matches after swapchain recovery");
    }

    // Clean shutdown and re-initialization with triple buffering and BGRA8 swapchain.
    presenter.shutdown();
    check(!presenter.is_initialized(), "Presenter reports uninitialized after shutdown");
    presenter.shutdown(); // Idempotent second shutdown.

    PresenterConfig reinit_cfg = cfg;
    reinit_cfg.width = 160;
    reinit_cfg.height = 96;
    reinit_cfg.buffer_count = 3;
    reinit_cfg.swapchain_format = PixelFormat::bgra8_unorm;
    check(presenter.initialize(reinit_cfg, err),
          "Re-initialize presenter after shutdown (triple-buffered BGRA8): " + err);

    SyntheticImage bgra_native;
    check(make_synthetic_test_image(160, 96, 5, PixelFormat::bgra8_unorm, bgra_native, err),
          "Create 160x96 BGRA8 synthetic image");
    check(presenter.present_frame(bgra_native.view(), err),
          "Present BGRA8 frame on re-initialized presenter: " + err);
    check(presenter.readback_last_backbuffer(readback_pixels, rb_w, rb_h, rb_fmt, err) &&
          rb_w == 160 && rb_h == 96 && rb_fmt == PixelFormat::bgra8_unorm &&
          readback_pixels == bgra_native.pixels,
          "GPU readback matches on re-initialized BGRA8 swapchain");

    // Window close detection and rejection of presentation on a closed window.
    presenter.close_window();
    check(presenter.pump_messages(quit, err) && quit,
          "pump_messages reports quit_requested after close_window()");
    check(!presenter.stats().window_open, "Stats report window_open == false after close_window()");
    check(!presenter.present_frame(bgra_native.view(), err),
          "Reject present_frame after Win32 window is closed");
    check(!presenter.resize(320, 240, err),
          "Reject resize after Win32 window is closed");

    presenter.shutdown();
    check(!presenter.is_initialized(), "Final shutdown leaves presenter uninitialized");

    // Verify WARP software adapter fallback path when prefer_software_adapter is enabled.
    PresenterConfig warp_cfg = cfg;
    warp_cfg.width = 128;
    warp_cfg.height = 72;
    warp_cfg.prefer_software_adapter = true;
    warp_cfg.allow_software_adapter = true;
    check(presenter.initialize(warp_cfg, err),
          "Initialize presenter on DXGI WARP software adapter: " + err);
    check(presenter.stats().using_software_adapter,
          "Stats report using_software_adapter == true on WARP");
    SyntheticImage warp_img;
    check(make_synthetic_test_image(128, 72, 3, PixelFormat::rgba8_unorm, warp_img, err),
          "Create 128x72 synthetic image for WARP");
    check(presenter.present_frame(warp_img.view(), err),
          "Present frame on WARP adapter: " + err);
    check(presenter.readback_last_backbuffer(readback_pixels, rb_w, rb_h, rb_fmt, err) &&
          readback_pixels == warp_img.pixels,
          "WARP backbuffer readback matches synthetic image");
    presenter.shutdown();
}
#endif

} // namespace

int main() {
    test_backend_contracts_and_validation();
#ifdef _WIN32
    test_d3d12_lifecycle_resize_and_recovery();
#endif
    if (failures != 0) {
        return 1;
    }
    std::cout << "All presentation backend regressions passed (" << checks_run << " checks)\n";
    return 0;
}
