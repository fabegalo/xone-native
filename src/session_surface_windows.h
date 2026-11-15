// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include "xboxone_native/session_surface.h"
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <algorithm>
#include <string>
#include <vector>

namespace xboxone::session {
// Bounded IPC for the supervised ERA process. The same mutex protects pixels
// and input ownership, so the title never consumes a partially published frame.
class SurfaceChannel {
    struct Shared {
        std::uint32_t version, mode, width, height;
        std::uint64_t parent, sequence;
        std::uint32_t pixels[surface_max_width * surface_max_height];
    };
    HANDLE mapping_ = nullptr, mutex_ = nullptr;
    Shared* shared_ = nullptr;
public:
    SurfaceChannel() = default;
    SurfaceChannel(const SurfaceChannel&) = delete;
    SurfaceChannel& operator=(const SurfaceChannel&) = delete;
    ~SurfaceChannel() { reset(); }
    void reset() {
        if (shared_) UnmapViewOfFile(shared_);
        if (mapping_) CloseHandle(mapping_);
        if (mutex_) CloseHandle(mutex_);
        shared_ = nullptr; mapping_ = mutex_ = nullptr;
    }
    bool connect(DWORD pid, HWND parent, bool producer) {
        reset();
        if (!IsWindow(parent)) return false;
        const auto name = L"Local\\xone-session-surface-" + std::to_wstring(pid);
        if (producer) {
            mutex_ = CreateMutexW(nullptr, FALSE, (name + L"-lock").c_str());
            if (!mutex_ || GetLastError() == ERROR_ALREADY_EXISTS) { reset(); return false; }
            mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(Shared), name.c_str());
            if (!mapping_ || GetLastError() == ERROR_ALREADY_EXISTS) { reset(); return false; }
        } else {
            mutex_ = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, (name + L"-lock").c_str());
            mapping_ = OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
        }
        if (!mutex_ || !mapping_) { reset(); return false; }
        shared_ = static_cast<Shared*>(MapViewOfFile(mapping_, producer ? FILE_MAP_WRITE : FILE_MAP_READ, 0, 0, sizeof(Shared)));
        if (!shared_) { reset(); return false; }
        if (producer) {
            shared_->version = 1;
            shared_->parent = reinterpret_cast<std::uintptr_t>(parent);
            shared_->mode = static_cast<std::uint32_t>(SurfaceMode::title);
            shared_->width = shared_->height = 0;
            shared_->sequence = 0;
        } else if (shared_->version != 1 || shared_->parent != reinterpret_cast<std::uintptr_t>(parent)) {
            reset(); return false;
        }
        return true;
    }
    bool publish(const SystemUiFrame& frame) {
        if (!shared_ || !valid_frame(frame)) return false;
        const auto wait = WaitForSingleObject(mutex_, 100);
        if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) return false;
        std::copy(frame.pixels.begin(), frame.pixels.end(), shared_->pixels);
        shared_->width = frame.width; shared_->height = frame.height;
        shared_->mode = static_cast<std::uint32_t>(frame.mode);
        ++shared_->sequence;
        ReleaseMutex(mutex_);
        return true;
    }
    bool read(SurfaceMode& mode, std::uint32_t& width, std::uint32_t& height,
              std::vector<std::uint32_t>* pixels = nullptr) const {
        if (!shared_) return false;
        const auto wait = WaitForSingleObject(mutex_, 100);
        if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) return false;
        struct Unlock { HANDLE mutex; ~Unlock() { ReleaseMutex(mutex); } } unlock{mutex_};
        const auto w = shared_->width, h = shared_->height, m = shared_->mode;
        const bool valid = shared_->version == 1 && m <= static_cast<unsigned>(SurfaceMode::guide) &&
            w <= surface_max_width && h <= surface_max_height;
        if (valid) {
            mode = static_cast<SurfaceMode>(m); width = w; height = h;
            if (pixels && mode == SurfaceMode::guide) pixels->assign(shared_->pixels, shared_->pixels + std::size_t(w) * h);
        }
        return valid && wait != WAIT_ABANDONED;
    }
};
} // namespace xboxone::session
