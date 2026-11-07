// SPDX-License-Identifier: BSD-3-Clause
#include "presentation_backend.h"

#ifdef _WIN32

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace xboxone::presentation {
namespace {

using Microsoft::WRL::ComPtr;

constexpr wchar_t kWindowClassName[] = L"XboxOneNativePresentationWindowClass";
constexpr std::string_view kSyntheticTitlePrefix = "[SYNTHETIC TEST - NOT SYSTEMOS] ";

std::string format_hresult(std::string_view context, HRESULT hr) {
    std::ostringstream oss;
    oss << context << " (HRESULT 0x" << std::hex << std::uppercase
        << std::setw(8) << std::setfill('0') << static_cast<std::uint32_t>(hr) << ")";
    return oss.str();
}

std::wstring utf8_to_wide(std::string_view utf8) {
    if (utf8.empty()) return {};
    const int needed = MultiByteToWideChar(
        CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring wide(static_cast<std::size_t>(needed), L'\0');
    MultiByteToWideChar(
        CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), needed);
    return wide;
}

std::string wide_to_utf8(std::wstring_view wide) {
    if (wide.empty()) return {};
    const int needed = WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string utf8(static_cast<std::size_t>(needed), '\0');
    WideCharToMultiByte(
        CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), needed, nullptr, nullptr);
    return utf8;
}

DXGI_FORMAT to_dxgi_format(PixelFormat format) noexcept {
    switch (format) {
        case PixelFormat::rgba8_unorm:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case PixelFormat::bgra8_unorm:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
    }
    return DXGI_FORMAT_R8G8B8A8_UNORM;
}

std::string build_safe_window_title(std::string_view requested_title, bool authentic_systemos) {
    if (authentic_systemos) {
        return std::string(requested_title);
    }
    if (requested_title.starts_with(kSyntheticTitlePrefix)) {
        return std::string(requested_title);
    }
    std::string title;
    title.reserve(kSyntheticTitlePrefix.size() + requested_title.size());
    title.append(kSyntheticTitlePrefix);
    title.append(requested_title);
    return title;
}

bool select_d3d12_adapter(IDXGIFactory4* factory,
                          bool allow_software_adapter,
                          bool prefer_software_adapter,
                          ComPtr<IDXGIAdapter1>& out_adapter,
                          ComPtr<ID3D12Device>& out_device,
                          std::string& out_adapter_name,
                          bool& out_is_software,
                          std::string& error) {
    out_adapter.Reset();
    out_device.Reset();
    out_adapter_name.clear();
    out_is_software = false;

    auto try_hardware = [&]() -> bool {
        for (UINT index = 0;; ++index) {
            ComPtr<IDXGIAdapter1> candidate;
            if (factory->EnumAdapters1(index, &candidate) == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            DXGI_ADAPTER_DESC1 desc{};
            if (FAILED(candidate->GetDesc1(&desc))) {
                continue;
            }
            if ((desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) != 0) {
                continue;
            }
            ComPtr<ID3D12Device> device;
            if (SUCCEEDED(D3D12CreateDevice(
                    candidate.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device)))) {
                out_adapter = candidate;
                out_device = device;
                out_adapter_name = wide_to_utf8(desc.Description);
                out_is_software = false;
                return true;
            }
        }
        return false;
    };

    auto try_warp = [&]() -> bool {
        if (!allow_software_adapter) {
            return false;
        }
        ComPtr<IDXGIAdapter> warp_base;
        const HRESULT hr_warp = factory->EnumWarpAdapter(IID_PPV_ARGS(&warp_base));
        if (FAILED(hr_warp)) {
            error = format_hresult("Failed to enumerate DXGI WARP adapter", hr_warp);
            return false;
        }
        ComPtr<IDXGIAdapter1> warp_adapter;
        if (FAILED(warp_base.As(&warp_adapter))) {
            error = "Failed to query IDXGIAdapter1 from WARP adapter";
            return false;
        }
        ComPtr<ID3D12Device> device;
        const HRESULT hr_dev = D3D12CreateDevice(
            warp_adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&device));
        if (FAILED(hr_dev)) {
            error = format_hresult("Failed to create D3D12 device on WARP adapter", hr_dev);
            return false;
        }
        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(warp_adapter->GetDesc1(&desc))) {
            out_adapter_name = wide_to_utf8(desc.Description);
        } else {
            out_adapter_name = "Microsoft Basic Render Driver (WARP)";
        }
        out_adapter = warp_adapter;
        out_device = device;
        out_is_software = true;
        return true;
    };

    if (prefer_software_adapter) {
        if (try_warp()) return true;
        if (try_hardware()) return true;
    } else {
        if (try_hardware()) return true;
        if (try_warp()) return true;
    }

    if (error.empty()) {
        error = "No compatible Direct3D 12 adapter found";
    }
    return false;
}

class D3D12Backend final : public BackendInstance {
public:
    D3D12Backend() = default;
    ~D3D12Backend() override { shutdown(); }

    bool initialize(const PresenterConfig& config, std::string& error) override {
        shutdown();
        config_ = config;
        stats_ = PresenterStats{};
        stats_.backend = BackendKind::d3d12;
        stats_.width = config.width;
        stats_.height = config.height;
        stats_.buffer_count = config.buffer_count;
        stats_.swapchain_format = config.swapchain_format;

        if (!create_window(config, error)) {
            shutdown();
            return false;
        }
        if (!create_device_and_swapchain(error)) {
            shutdown();
            return false;
        }

        stats_.initialized = true;
        return true;
    }

    void close_window() noexcept override {
        if (hwnd_ != nullptr && window_open_) {
            SendMessageW(hwnd_, WM_CLOSE, 0, 0);
        }
    }

    void shutdown() noexcept override {
        if (device_ && command_queue_ && fence_) {
            std::string ignored_error;
            (void)wait_for_gpu(ignored_error);
        }

        readback_buffer_.Reset();
        readback_buffer_size_ = 0;
        upload_buffer_.Reset();
        upload_buffer_size_ = 0;

        for (auto& buffer : backbuffers_) {
            buffer.Reset();
        }
        backbuffers_.clear();

        for (auto& allocator : frame_allocators_) {
            allocator.Reset();
        }
        frame_allocators_.clear();
        frame_fence_values_.clear();

        command_list_.Reset();
        rtv_heap_.Reset();
        swapchain_.Reset();
        fence_.Reset();
        command_queue_.Reset();
        device_.Reset();
        adapter_.Reset();
        factory_.Reset();

        if (fence_event_ != nullptr) {
            CloseHandle(fence_event_);
            fence_event_ = nullptr;
        }

        if (hwnd_ != nullptr) {
            SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }

        window_open_ = false;
        close_requested_ = false;
        has_presented_frame_ = false;
        last_presented_backbuffer_index_ = 0;
        next_fence_value_ = 0;
        stats_ = PresenterStats{};
    }

    [[nodiscard]] bool is_initialized() const noexcept override {
        return stats_.initialized;
    }

    [[nodiscard]] PresenterStats stats() const override {
        return stats_;
    }

    bool pump_messages(bool& quit_requested, std::string& error) override {
        if (!stats_.initialized) {
            error = "Presenter is not initialized";
            return false;
        }

        MSG msg{};
        while (hwnd_ != nullptr && PeekMessageW(&msg, hwnd_, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        while (PeekMessageW(&msg, reinterpret_cast<HWND>(-1), 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                close_requested_ = true;
            }
        }

        stats_.window_open = window_open_ && !close_requested_;
        quit_requested = !stats_.window_open;
        return true;
    }

    bool resize(std::uint32_t new_width, std::uint32_t new_height, std::string& error) override {
        if (!stats_.initialized || !swapchain_ || !device_) {
            error = "Cannot resize: Direct3D 12 presenter is not initialized";
            return false;
        }
        if (!window_open_ || close_requested_) {
            error = "Cannot resize: presentation window is closed";
            return false;
        }

        if (!wait_for_gpu(error)) {
            return false;
        }

        for (auto& buffer : backbuffers_) {
            buffer.Reset();
        }

        const HRESULT hr = swapchain_->ResizeBuffers(
            stats_.buffer_count,
            new_width,
            new_height,
            to_dxgi_format(stats_.swapchain_format),
            0);
        if (FAILED(hr)) {
            error = format_hresult("IDXGISwapChain3::ResizeBuffers failed", hr);
            // Attempt to restore previous buffers so presenter remains usable.
            std::string restore_error;
            (void)acquire_backbuffers_and_rtvs(restore_error);
            return false;
        }

        if (!acquire_backbuffers_and_rtvs(error)) {
            return false;
        }

        for (auto& fence_val : frame_fence_values_) {
            fence_val = stats_.last_completed_fence_value;
        }

        if (hwnd_ != nullptr) {
            RECT rect{0, 0, static_cast<LONG>(new_width), static_cast<LONG>(new_height)};
            const DWORD style = static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_STYLE));
            const DWORD ex_style = static_cast<DWORD>(GetWindowLongPtrW(hwnd_, GWL_EXSTYLE));
            AdjustWindowRectEx(&rect, style, FALSE, ex_style);
            SetWindowPos(
                hwnd_,
                nullptr,
                0,
                0,
                rect.right - rect.left,
                rect.bottom - rect.top,
                SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }

        config_.width = new_width;
        config_.height = new_height;
        stats_.width = new_width;
        stats_.height = new_height;
        stats_.current_backbuffer_index = swapchain_->GetCurrentBackBufferIndex();
        ++stats_.resize_count;
        has_presented_frame_ = false;
        return true;
    }

    bool present_frame(const ExternalFrameView& frame, std::string& error) override {
        if (!stats_.initialized || !swapchain_ || !device_ || !command_queue_) {
            error = "Cannot present frame: Direct3D 12 presenter is not initialized";
            return false;
        }
        if (!window_open_ || close_requested_) {
            error = "Cannot present frame: Win32 presentation window is closed";
            return false;
        }
        if (frame.width != stats_.width || frame.height != stats_.height) {
            std::ostringstream oss;
            oss << "External frame dimensions (" << frame.width << "x" << frame.height
                << ") do not match swapchain dimensions (" << stats_.width << "x" << stats_.height
                << "); call resize() before presenting";
            error = oss.str();
            return false;
        }

        const std::uint32_t backbuffer_index = swapchain_->GetCurrentBackBufferIndex();
        if (backbuffer_index >= backbuffers_.size()) {
            error = "Swapchain returned an out-of-range backbuffer index";
            return false;
        }

        if (!wait_for_fence_value(frame_fence_values_[backbuffer_index], error)) {
            return false;
        }

        const std::uint32_t row_bytes = frame.width * 4u;
        const std::uint32_t aligned_pitch =
            (row_bytes + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u) &
            ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u);
        const std::uint64_t required_upload_size =
            static_cast<std::uint64_t>(aligned_pitch) * frame.height;

        if (!ensure_upload_buffer(required_upload_size, error)) {
            return false;
        }

        void* mapped_ptr = nullptr;
        const D3D12_RANGE read_range{0, 0};
        const HRESULT hr_map = upload_buffer_->Map(0, &read_range, &mapped_ptr);
        if (FAILED(hr_map) || mapped_ptr == nullptr) {
            error = format_hresult("Failed to map D3D12 upload buffer", hr_map);
            return false;
        }

        auto* dst_base = static_cast<std::uint8_t*>(mapped_ptr);
        const bool same_format = (frame.format == stats_.swapchain_format);
        for (std::uint32_t y = 0; y < frame.height; ++y) {
            const std::uint8_t* src_row =
                frame.pixels.data() + static_cast<std::size_t>(y) * frame.row_pitch_bytes;
            std::uint8_t* dst_row =
                dst_base + static_cast<std::size_t>(y) * aligned_pitch;
            if (same_format) {
                std::memcpy(dst_row, src_row, row_bytes);
            } else {
                for (std::uint32_t x = 0; x < frame.width; ++x) {
                    const std::size_t offset = static_cast<std::size_t>(x) * 4u;
                    dst_row[offset + 0] = src_row[offset + 2];
                    dst_row[offset + 1] = src_row[offset + 1];
                    dst_row[offset + 2] = src_row[offset + 0];
                    dst_row[offset + 3] = src_row[offset + 3];
                }
            }
        }

        const D3D12_RANGE written_range{0, static_cast<SIZE_T>(required_upload_size)};
        upload_buffer_->Unmap(0, &written_range);

        ID3D12CommandAllocator* allocator = frame_allocators_[backbuffer_index].Get();
        HRESULT hr = allocator->Reset();
        if (FAILED(hr)) {
            error = format_hresult("ID3D12CommandAllocator::Reset failed", hr);
            return false;
        }

        hr = command_list_->Reset(allocator, nullptr);
        if (FAILED(hr)) {
            error = format_hresult("ID3D12GraphicsCommandList::Reset failed", hr);
            return false;
        }

        ID3D12Resource* backbuffer = backbuffers_[backbuffer_index].Get();

        D3D12_RESOURCE_BARRIER to_copy_dest{};
        to_copy_dest.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_copy_dest.Transition.pResource = backbuffer;
        to_copy_dest.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        to_copy_dest.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        to_copy_dest.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        command_list_->ResourceBarrier(1, &to_copy_dest);

        D3D12_TEXTURE_COPY_LOCATION dst_location{};
        dst_location.pResource = backbuffer;
        dst_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst_location.SubresourceIndex = 0;

        D3D12_TEXTURE_COPY_LOCATION src_location{};
        src_location.pResource = upload_buffer_.Get();
        src_location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src_location.PlacedFootprint.Offset = 0;
        src_location.PlacedFootprint.Footprint.Format = to_dxgi_format(stats_.swapchain_format);
        src_location.PlacedFootprint.Footprint.Width = frame.width;
        src_location.PlacedFootprint.Footprint.Height = frame.height;
        src_location.PlacedFootprint.Footprint.Depth = 1;
        src_location.PlacedFootprint.Footprint.RowPitch = aligned_pitch;

        command_list_->CopyTextureRegion(&dst_location, 0, 0, 0, &src_location, nullptr);

        D3D12_RESOURCE_BARRIER to_present{};
        to_present.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_present.Transition.pResource = backbuffer;
        to_present.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        to_present.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        to_present.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        command_list_->ResourceBarrier(1, &to_present);

        hr = command_list_->Close();
        if (FAILED(hr)) {
            error = format_hresult("ID3D12GraphicsCommandList::Close failed", hr);
            return false;
        }

        ID3D12CommandList* lists[] = {command_list_.Get()};
        command_queue_->ExecuteCommandLists(1, lists);

        hr = swapchain_->Present(config_.vsync ? 1u : 0u, 0u);
        if (FAILED(hr)) {
            error = format_hresult("IDXGISwapChain3::Present failed", hr);
            return false;
        }

        const std::uint64_t fence_value = ++next_fence_value_;
        hr = command_queue_->Signal(fence_.Get(), fence_value);
        if (FAILED(hr)) {
            error = format_hresult("ID3D12CommandQueue::Signal failed", hr);
            return false;
        }

        frame_fence_values_[backbuffer_index] = fence_value;
        stats_.last_submitted_fence_value = fence_value;
        stats_.last_completed_fence_value = fence_->GetCompletedValue();
        last_presented_backbuffer_index_ = backbuffer_index;
        has_presented_frame_ = true;
        stats_.current_backbuffer_index = swapchain_->GetCurrentBackBufferIndex();
        ++stats_.presented_frames;

        switch (frame.provenance.origin) {
            case FrameOrigin::synthetic_test:
                ++stats_.synthetic_frames;
                update_window_title_mode(false);
                break;
            case FrameOrigin::external_host_buffer:
                ++stats_.external_host_frames;
                update_window_title_mode(false);
                break;
            case FrameOrigin::systemos_component:
                ++stats_.authentic_systemos_frames;
                update_window_title_mode(true);
                break;
        }

        return true;
    }

    bool wait_for_gpu(std::string& error) override {
        if (!command_queue_ || !fence_ || fence_event_ == nullptr) {
            error = "Cannot synchronize GPU: command queue or fence is not initialized";
            return false;
        }
        const std::uint64_t fence_value = ++next_fence_value_;
        const HRESULT hr = command_queue_->Signal(fence_.Get(), fence_value);
        if (FAILED(hr)) {
            error = format_hresult("ID3D12CommandQueue::Signal failed during GPU sync", hr);
            return false;
        }
        stats_.last_submitted_fence_value = fence_value;
        return wait_for_fence_value(fence_value, error);
    }

    bool recover_swapchain(std::string& error) override {
        if (!stats_.initialized || hwnd_ == nullptr || !window_open_ || close_requested_) {
            error = "Cannot recover swapchain: presentation window is not active";
            return false;
        }

        if (device_ && command_queue_ && fence_) {
            std::string ignored;
            (void)wait_for_gpu(ignored);
        }

        readback_buffer_.Reset();
        readback_buffer_size_ = 0;
        upload_buffer_.Reset();
        upload_buffer_size_ = 0;

        for (auto& buffer : backbuffers_) {
            buffer.Reset();
        }
        backbuffers_.clear();

        for (auto& allocator : frame_allocators_) {
            allocator.Reset();
        }
        frame_allocators_.clear();
        frame_fence_values_.clear();

        command_list_.Reset();
        rtv_heap_.Reset();
        swapchain_.Reset();
        fence_.Reset();
        command_queue_.Reset();

        if (fence_event_ != nullptr) {
            CloseHandle(fence_event_);
            fence_event_ = nullptr;
        }

        if (device_ && FAILED(device_->GetDeviceRemovedReason())) {
            device_.Reset();
            adapter_.Reset();
            factory_.Reset();
        }

        if (!create_device_and_swapchain(error)) {
            return false;
        }

        has_presented_frame_ = false;
        ++stats_.recovered_error_count;
        return true;
    }

    bool readback_last_backbuffer(std::vector<std::uint8_t>& out_pixels,
                                  std::uint32_t& out_width,
                                  std::uint32_t& out_height,
                                  PixelFormat& out_format,
                                  std::string& error) override {
        if (!stats_.initialized || !has_presented_frame_) {
            error = "No presented frame is available for GPU readback";
            return false;
        }
        if (last_presented_backbuffer_index_ >= backbuffers_.size()) {
            error = "Last presented backbuffer index is invalid";
            return false;
        }
        if (!wait_for_gpu(error)) {
            return false;
        }

        const std::uint32_t width = stats_.width;
        const std::uint32_t height = stats_.height;
        const std::uint32_t row_bytes = width * 4u;
        const std::uint32_t aligned_pitch =
            (row_bytes + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u) &
            ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1u);
        const std::uint64_t required_readback_size =
            static_cast<std::uint64_t>(aligned_pitch) * height;

        if (!ensure_readback_buffer(required_readback_size, error)) {
            return false;
        }

        ID3D12CommandAllocator* allocator = frame_allocators_[last_presented_backbuffer_index_].Get();
        HRESULT hr = allocator->Reset();
        if (FAILED(hr)) {
            error = format_hresult("ID3D12CommandAllocator::Reset failed in readback", hr);
            return false;
        }
        hr = command_list_->Reset(allocator, nullptr);
        if (FAILED(hr)) {
            error = format_hresult("ID3D12GraphicsCommandList::Reset failed in readback", hr);
            return false;
        }

        ID3D12Resource* backbuffer = backbuffers_[last_presented_backbuffer_index_].Get();

        D3D12_RESOURCE_BARRIER to_copy_source{};
        to_copy_source.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_copy_source.Transition.pResource = backbuffer;
        to_copy_source.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
        to_copy_source.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        to_copy_source.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        command_list_->ResourceBarrier(1, &to_copy_source);

        D3D12_TEXTURE_COPY_LOCATION dst_location{};
        dst_location.pResource = readback_buffer_.Get();
        dst_location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst_location.PlacedFootprint.Offset = 0;
        dst_location.PlacedFootprint.Footprint.Format = to_dxgi_format(stats_.swapchain_format);
        dst_location.PlacedFootprint.Footprint.Width = width;
        dst_location.PlacedFootprint.Footprint.Height = height;
        dst_location.PlacedFootprint.Footprint.Depth = 1;
        dst_location.PlacedFootprint.Footprint.RowPitch = aligned_pitch;

        D3D12_TEXTURE_COPY_LOCATION src_location{};
        src_location.pResource = backbuffer;
        src_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src_location.SubresourceIndex = 0;

        command_list_->CopyTextureRegion(&dst_location, 0, 0, 0, &src_location, nullptr);

        D3D12_RESOURCE_BARRIER to_present{};
        to_present.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        to_present.Transition.pResource = backbuffer;
        to_present.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        to_present.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        to_present.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        command_list_->ResourceBarrier(1, &to_present);

        hr = command_list_->Close();
        if (FAILED(hr)) {
            error = format_hresult("ID3D12GraphicsCommandList::Close failed in readback", hr);
            return false;
        }

        ID3D12CommandList* lists[] = {command_list_.Get()};
        command_queue_->ExecuteCommandLists(1, lists);

        if (!wait_for_gpu(error)) {
            return false;
        }

        void* mapped_ptr = nullptr;
        const D3D12_RANGE read_range{0, static_cast<SIZE_T>(required_readback_size)};
        hr = readback_buffer_->Map(0, &read_range, &mapped_ptr);
        if (FAILED(hr) || mapped_ptr == nullptr) {
            error = format_hresult("Failed to map D3D12 readback buffer", hr);
            return false;
        }

        out_pixels.resize(static_cast<std::size_t>(row_bytes) * height);
        const auto* src_base = static_cast<const std::uint8_t*>(mapped_ptr);
        for (std::uint32_t y = 0; y < height; ++y) {
            std::memcpy(
                out_pixels.data() + static_cast<std::size_t>(y) * row_bytes,
                src_base + static_cast<std::size_t>(y) * aligned_pitch,
                row_bytes);
        }

        const D3D12_RANGE empty_write_range{0, 0};
        readback_buffer_->Unmap(0, &empty_write_range);

        out_width = width;
        out_height = height;
        out_format = stats_.swapchain_format;
        return true;
    }

private:
    static LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
        auto* self = reinterpret_cast<D3D12Backend*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        switch (msg) {
            case WM_CLOSE:
                if (self != nullptr) {
                    self->close_requested_ = true;
                    self->stats_.window_open = false;
                }
                ShowWindow(hwnd, SW_HIDE);
                return 0;
            case WM_DESTROY:
                if (self != nullptr) {
                    self->window_open_ = false;
                    self->stats_.window_open = false;
                }
                return 0;
            default:
                return DefWindowProcW(hwnd, msg, wparam, lparam);
        }
    }

    static bool ensure_window_class_registered(HINSTANCE instance, std::string& error) {
        WNDCLASSEXW existing{};
        if (GetClassInfoExW(instance, kWindowClassName, &existing) != 0) {
            return true;
        }
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = &D3D12Backend::window_proc;
        wc.hInstance = instance;
        wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
        wc.lpszClassName = kWindowClassName;
        if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            error = "RegisterClassExW failed for Win32 presentation window";
            return false;
        }
        return true;
    }

    bool create_window(const PresenterConfig& config, std::string& error) {
        const HINSTANCE instance = GetModuleHandleW(nullptr);
        if (!ensure_window_class_registered(instance, error)) {
            return false;
        }

        const std::string safe_title = build_safe_window_title(config.window_title, false);
        const std::wstring wide_title = utf8_to_wide(safe_title);
        if (wide_title.empty()) {
            error = "Failed to convert window_title to UTF-16";
            return false;
        }

        const DWORD style = WS_OVERLAPPEDWINDOW;
        const DWORD ex_style = 0;
        RECT rect{0, 0, static_cast<LONG>(config.width), static_cast<LONG>(config.height)};
        AdjustWindowRectEx(&rect, style, FALSE, ex_style);

        hwnd_ = CreateWindowExW(
            ex_style,
            kWindowClassName,
            wide_title.c_str(),
            style,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            rect.right - rect.left,
            rect.bottom - rect.top,
            nullptr,
            nullptr,
            instance,
            nullptr);
        if (hwnd_ == nullptr) {
            error = "CreateWindowExW failed for Win32 presentation window";
            return false;
        }

        SetWindowLongPtrW(hwnd_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
        if (config.visible) {
            ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        }

        window_open_ = true;
        close_requested_ = false;
        stats_.window_open = true;
        stats_.active_window_title = safe_title;
        return true;
    }

    void update_window_title_mode(bool authentic_systemos) {
        const std::string expected_title =
            build_safe_window_title(config_.window_title, authentic_systemos);
        if (expected_title == stats_.active_window_title) {
            return;
        }
        stats_.active_window_title = expected_title;
        if (hwnd_ != nullptr) {
            const std::wstring wide_title = utf8_to_wide(expected_title);
            if (!wide_title.empty()) {
                SetWindowTextW(hwnd_, wide_title.c_str());
            }
        }
    }

    bool create_device_and_swapchain(std::string& error) {
        if (!factory_) {
            const HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory_));
            if (FAILED(hr)) {
                error = format_hresult("CreateDXGIFactory2 failed", hr);
                return false;
            }
        }

        if (!device_) {
            std::string adapter_name;
            bool is_software = false;
            if (!select_d3d12_adapter(
                    factory_.Get(),
                    config_.allow_software_adapter,
                    config_.prefer_software_adapter,
                    adapter_,
                    device_,
                    adapter_name,
                    is_software,
                    error)) {
                return false;
            }
            stats_.adapter_name = std::move(adapter_name);
            stats_.using_software_adapter = is_software;
        }

        D3D12_COMMAND_QUEUE_DESC queue_desc{};
        queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        queue_desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
        queue_desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
        HRESULT hr = device_->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&command_queue_));
        if (FAILED(hr)) {
            error = format_hresult("ID3D12Device::CreateCommandQueue failed", hr);
            return false;
        }

        DXGI_SWAP_CHAIN_DESC1 sc_desc{};
        sc_desc.Width = config_.width;
        sc_desc.Height = config_.height;
        sc_desc.Format = to_dxgi_format(config_.swapchain_format);
        sc_desc.Stereo = FALSE;
        sc_desc.SampleDesc.Count = 1;
        sc_desc.SampleDesc.Quality = 0;
        sc_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sc_desc.BufferCount = config_.buffer_count;
        sc_desc.Scaling = DXGI_SCALING_STRETCH;
        sc_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
        sc_desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        sc_desc.Flags = 0;

        ComPtr<IDXGISwapChain1> swapchain1;
        hr = factory_->CreateSwapChainForHwnd(
            command_queue_.Get(),
            hwnd_,
            &sc_desc,
            nullptr,
            nullptr,
            &swapchain1);
        if (FAILED(hr)) {
            error = format_hresult("IDXGIFactory2::CreateSwapChainForHwnd failed", hr);
            return false;
        }

        (void)factory_->MakeWindowAssociation(hwnd_, DXGI_MWA_NO_ALT_ENTER);

        hr = swapchain1.As(&swapchain_);
        if (FAILED(hr)) {
            error = format_hresult("Failed to query IDXGISwapChain3", hr);
            return false;
        }

        D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc{};
        rtv_heap_desc.NumDescriptors = config_.buffer_count;
        rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
        hr = device_->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&rtv_heap_desc_heap_ref()));
        if (FAILED(hr)) {
            error = format_hresult("ID3D12Device::CreateDescriptorHeap (RTV) failed", hr);
            return false;
        }
        rtv_descriptor_size_ =
            device_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

        if (!acquire_backbuffers_and_rtvs(error)) {
            return false;
        }

        frame_allocators_.resize(config_.buffer_count);
        frame_fence_values_.assign(config_.buffer_count, 0);
        for (std::uint32_t i = 0; i < config_.buffer_count; ++i) {
            hr = device_->CreateCommandAllocator(
                D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&frame_allocators_[i]));
            if (FAILED(hr)) {
                error = format_hresult("ID3D12Device::CreateCommandAllocator failed", hr);
                return false;
            }
        }

        hr = device_->CreateCommandList(
            0,
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            frame_allocators_[0].Get(),
            nullptr,
            IID_PPV_ARGS(&command_list_));
        if (FAILED(hr)) {
            error = format_hresult("ID3D12Device::CreateCommandList failed", hr);
            return false;
        }
        (void)command_list_->Close();

        hr = device_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
        if (FAILED(hr)) {
            error = format_hresult("ID3D12Device::CreateFence failed", hr);
            return false;
        }

        fence_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (fence_event_ == nullptr) {
            error = "CreateEventW failed for D3D12 synchronization fence";
            return false;
        }

        next_fence_value_ = 0;
        stats_.last_submitted_fence_value = 0;
        stats_.last_completed_fence_value = 0;
        stats_.current_backbuffer_index = swapchain_->GetCurrentBackBufferIndex();
        return true;
    }

    ComPtr<ID3D12DescriptorHeap>& rtv_heap_desc_heap_ref() noexcept {
        return rtv_heap_;
    }

    bool acquire_backbuffers_and_rtvs(std::string& error) {
        backbuffers_.resize(stats_.buffer_count);
        D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = rtv_heap_->GetCPUDescriptorHandleForHeapStart();
        for (std::uint32_t i = 0; i < stats_.buffer_count; ++i) {
            const HRESULT hr = swapchain_->GetBuffer(i, IID_PPV_ARGS(&backbuffers_[i]));
            if (FAILED(hr)) {
                error = format_hresult("IDXGISwapChain3::GetBuffer failed", hr);
                return false;
            }
            device_->CreateRenderTargetView(backbuffers_[i].Get(), nullptr, rtv_handle);
            rtv_handle.ptr += rtv_descriptor_size_;
        }
        return true;
    }

    bool wait_for_fence_value(std::uint64_t target_value, std::string& error) {
        if (target_value == 0) {
            return true;
        }
        const std::uint64_t completed = fence_->GetCompletedValue();
        if (completed < target_value) {
            const HRESULT hr = fence_->SetEventOnCompletion(target_value, fence_event_);
            if (FAILED(hr)) {
                error = format_hresult("ID3D12Fence::SetEventOnCompletion failed", hr);
                return false;
            }
            const DWORD wait_res = WaitForSingleObject(fence_event_, 5000);
            if (wait_res != WAIT_OBJECT_0) {
                error = "Timed out or failed waiting for D3D12 GPU fence completion";
                return false;
            }
        }
        stats_.last_completed_fence_value = fence_->GetCompletedValue();
        return true;
    }

    bool ensure_upload_buffer(std::uint64_t required_size, std::string& error) {
        if (upload_buffer_ && upload_buffer_size_ >= required_size) {
            return true;
        }
        upload_buffer_.Reset();
        upload_buffer_size_ = 0;

        D3D12_HEAP_PROPERTIES heap_props{};
        heap_props.Type = D3D12_HEAP_TYPE_UPLOAD;
        heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

        D3D12_RESOURCE_DESC buf_desc{};
        buf_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buf_desc.Alignment = 0;
        buf_desc.Width = required_size;
        buf_desc.Height = 1;
        buf_desc.DepthOrArraySize = 1;
        buf_desc.MipLevels = 1;
        buf_desc.Format = DXGI_FORMAT_UNKNOWN;
        buf_desc.SampleDesc.Count = 1;
        buf_desc.SampleDesc.Quality = 0;
        buf_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        buf_desc.Flags = D3D12_RESOURCE_FLAG_NONE;

        const HRESULT hr = device_->CreateCommittedResource(
            &heap_props,
            D3D12_HEAP_FLAG_NONE,
            &buf_desc,
            D3D12_RESOURCE_STATE_GENERIC_READ,
            nullptr,
            IID_PPV_ARGS(&upload_buffer_));
        if (FAILED(hr)) {
            error = format_hresult("Failed to allocate D3D12 upload buffer", hr);
            return false;
        }
        upload_buffer_size_ = required_size;
        return true;
    }

    bool ensure_readback_buffer(std::uint64_t required_size, std::string& error) {
        if (readback_buffer_ && readback_buffer_size_ >= required_size) {
            return true;
        }
        readback_buffer_.Reset();
        readback_buffer_size_ = 0;

        D3D12_HEAP_PROPERTIES heap_props{};
        heap_props.Type = D3D12_HEAP_TYPE_READBACK;
        heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
        heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

        D3D12_RESOURCE_DESC buf_desc{};
        buf_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buf_desc.Alignment = 0;
        buf_desc.Width = required_size;
        buf_desc.Height = 1;
        buf_desc.DepthOrArraySize = 1;
        buf_desc.MipLevels = 1;
        buf_desc.Format = DXGI_FORMAT_UNKNOWN;
        buf_desc.SampleDesc.Count = 1;
        buf_desc.SampleDesc.Quality = 0;
        buf_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        buf_desc.Flags = D3D12_RESOURCE_FLAG_NONE;

        const HRESULT hr = device_->CreateCommittedResource(
            &heap_props,
            D3D12_HEAP_FLAG_NONE,
            &buf_desc,
            D3D12_RESOURCE_STATE_COPY_DEST,
            nullptr,
            IID_PPV_ARGS(&readback_buffer_));
        if (FAILED(hr)) {
            error = format_hresult("Failed to allocate D3D12 readback buffer", hr);
            return false;
        }
        readback_buffer_size_ = required_size;
        return true;
    }

    PresenterConfig config_{};
    PresenterStats stats_{};
    HWND hwnd_ = nullptr;
    bool window_open_ = false;
    bool close_requested_ = false;
    bool has_presented_frame_ = false;
    std::uint32_t last_presented_backbuffer_index_ = 0;

    ComPtr<IDXGIFactory4> factory_;
    ComPtr<IDXGIAdapter1> adapter_;
    ComPtr<ID3D12Device> device_;
    ComPtr<ID3D12CommandQueue> command_queue_;
    ComPtr<IDXGISwapChain3> swapchain_;
    ComPtr<ID3D12DescriptorHeap> rtv_heap_;
    std::uint32_t rtv_descriptor_size_ = 0;
    std::vector<ComPtr<ID3D12Resource>> backbuffers_;
    std::vector<ComPtr<ID3D12CommandAllocator>> frame_allocators_;
    std::vector<std::uint64_t> frame_fence_values_;
    ComPtr<ID3D12GraphicsCommandList> command_list_;
    ComPtr<ID3D12Fence> fence_;
    HANDLE fence_event_ = nullptr;
    std::uint64_t next_fence_value_ = 0;

    ComPtr<ID3D12Resource> upload_buffer_;
    std::uint64_t upload_buffer_size_ = 0;
    ComPtr<ID3D12Resource> readback_buffer_;
    std::uint64_t readback_buffer_size_ = 0;
};

} // namespace

BackendSupport query_d3d12_backend_support(bool allow_software_adapter) {
    BackendSupport support{};
    support.backend = BackendKind::d3d12;
    support.compiled_in = true;

    ComPtr<IDXGIFactory4> factory;
    const HRESULT hr = CreateDXGIFactory2(0, IID_PPV_ARGS(&factory));
    if (FAILED(hr)) {
        support.runtime_available = false;
        support.status_reason = format_hresult("CreateDXGIFactory2 failed", hr);
        return support;
    }

    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<ID3D12Device> device;
    std::string adapter_name;
    bool is_software = false;
    std::string error;
    if (!select_d3d12_adapter(
            factory.Get(),
            allow_software_adapter,
            false,
            adapter,
            device,
            adapter_name,
            is_software,
            error)) {
        support.runtime_available = false;
        support.status_reason = error;
        return support;
    }

    support.runtime_available = true;
    support.software_adapter = is_software;
    support.adapter_name = std::move(adapter_name);
    support.status_reason = is_software
        ? "Direct3D 12 runtime available via DXGI WARP software adapter"
        : "Direct3D 12 runtime available via hardware adapter";
    return support;
}

std::unique_ptr<BackendInstance> create_d3d12_backend() {
    return std::make_unique<D3D12Backend>();
}

} // namespace xboxone::presentation

#endif // _WIN32
