// SPDX-License-Identifier: BSD-3-Clause
#include "xboxone_native/frame_pipe.h"
#include "xboxone_native/inventory.h"
#include "frame_pipe_protocol.h"
#include "file_path.h"
#include <algorithm>
#include <limits>
#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <objbase.h>
#endif

namespace xboxone::presentation {
using namespace pipe_protocol;
struct FramePipe::Impl {
    std::wstring name;
    std::vector<std::uint8_t> pixels;
    std::uint64_t sequence = 0;
    bool server = false, connected = false;
#ifdef _WIN32
    HANDLE pipe = INVALID_HANDLE_VALUE, process = nullptr, input = INVALID_HANDLE_VALUE;
    ~Impl() {
        if (pipe != INVALID_HANDLE_VALUE) CloseHandle(pipe);
        if (process) CloseHandle(process);
        if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
    }
    bool fail(std::string& error, const char* api) {
        error = std::string(api) + "; Win32=" + std::to_string(GetLastError());
        return false;
    }
    // Every pending operation is cancelled and drained before its stack storage
    // is released. A failed operation poisons the stream; no partial retry.
    bool finish(OVERLAPPED& op, BOOL immediate, ULONGLONG deadline, DWORD& bytes, std::string& error) {
        if (!immediate && GetLastError() != ERROR_IO_PENDING) return fail(error, "pipe I/O");
        const auto now = GetTickCount64();
        const DWORD remaining = deadline > now ? static_cast<DWORD>(deadline - now) : 0;
        const auto waited = WaitForSingleObject(op.hEvent, remaining);
        if (waited != WAIT_OBJECT_0) {
            CancelIoEx(pipe, &op);
            GetOverlappedResult(pipe, &op, &bytes, TRUE);
            error = waited == WAIT_TIMEOUT ? "frame pipe timeout; stream closed" : "frame pipe wait failed";
            return false;
        }
        if (!GetOverlappedResult(pipe, &op, &bytes, FALSE)) return fail(error, "GetOverlappedResult");
        return true;
    }
    bool transfer(void* data, std::uint32_t size, bool writing, ULONGLONG deadline, std::string& error) {
        auto* cursor = static_cast<std::uint8_t*>(data);
        while (size) {
            OVERLAPPED op{};
            op.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
            if (!op.hEvent) return fail(error, "CreateEventW");
            const auto ok = writing ? WriteFile(pipe, cursor, size, nullptr, &op) : ReadFile(pipe, cursor, size, nullptr, &op);
            DWORD count = 0;
            const bool done = finish(op, ok, deadline, count, error);
            CloseHandle(op.hEvent);
            if (!done) return false;
            if (!count || count > size) { error = "frame pipe closed or invalid transfer size"; return false; }
            cursor += count; size -= count;
            if (size && GetTickCount64() >= deadline) { error = "frame pipe timeout; stream closed"; return false; }
        }
        return true;
    }
#endif
};

namespace {
bool timeout_valid(std::uint32_t value, std::string& error) {
    if (value && value <= 60000) return true;
    error = "frame pipe timeout must be 1..60000 ms"; return false;
}
bool layout(const Header& h, std::string& error) {
    const auto required = h[5] ? static_cast<std::uint64_t>(h[5] - 1) * h[6] + static_cast<std::uint64_t>(h[4]) * 4 : 0;
    if (h[0] != magic || h[1] != version || h[9] || !h[4] || !h[5] ||
        h[4] > max_frame_dimension || h[5] > max_frame_dimension ||
        static_cast<std::uint64_t>(h[6]) < static_cast<std::uint64_t>(h[4]) * 4 ||
        (h[7] != 1 && h[7] != 2) || required > max_bytes || h[8] != required) {
        error = "invalid frame pipe header, layout or 64 MiB payload limit"; return false;
    }
    return true;
}
}
FramePipe::FramePipe() : impl_(std::make_unique<Impl>()) {}
FramePipe::~FramePipe() = default;
const std::wstring& FramePipe::endpoint() const { return impl_->name; }
void FramePipe::close() noexcept {
#ifdef _WIN32
    if (impl_->pipe != INVALID_HANDLE_VALUE) { CloseHandle(impl_->pipe); impl_->pipe = INVALID_HANDLE_VALUE; }
    if (impl_->process) { CloseHandle(impl_->process); impl_->process = nullptr; }
    if (impl_->input != INVALID_HANDLE_VALUE) { CloseHandle(impl_->input); impl_->input = INVALID_HANDLE_VALUE; }
#endif
    impl_->name.clear(); impl_->pixels.clear(); impl_->connected = impl_->server = false; impl_->sequence = 0;
}
bool FramePipe::create(std::string& error) {
    error.clear();
#ifdef _WIN32
    if (impl_->pipe != INVALID_HANDLE_VALUE) { error = "frame pipe already open"; return false; }
    GUID id{}; wchar_t nonce[40]{};
    if (FAILED(CoCreateGuid(&id)) || !StringFromGUID2(id, nonce, 40)) { error = "frame pipe nonce creation failed"; return false; }
    impl_->name = std::wstring(L"\\\\.\\pipe\\LOCAL\\XON-Frame-") + nonce;
    impl_->pipe = CreateNamedPipeW(impl_->name.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 0, nullptr);
    if (impl_->pipe == INVALID_HANDLE_VALUE) { impl_->name.clear(); return impl_->fail(error, "CreateNamedPipeW"); }
    impl_->server = true;
    return true;
#else
    error = "frame pipe requires Windows desktop"; return false;
#endif
}
bool FramePipe::accept(void* producer_process, const std::filesystem::path& executable,
                       const std::string& sha256, std::uint32_t timeout_ms, std::string& error) {
    error.clear();
#ifdef _WIN32
    if (!impl_->server || impl_->connected || !timeout_valid(timeout_ms, error)) { if (error.empty()) error = "frame pipe not awaiting a producer"; return false; }
    const auto deadline = GetTickCount64() + timeout_ms;
    const auto reject = [&] { close(); return false; };
    if (!producer_process || sha256.size() != 64 || !std::all_of(sha256.begin(), sha256.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); })) {
        error = "exact producer process and lowercase SHA-256 required"; return reject();
    }
    if (!DuplicateHandle(GetCurrentProcess(), producer_process, GetCurrentProcess(), &impl_->process,
                         SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, 0)) {
        impl_->fail(error, "DuplicateHandle(process)"); return reject();
    }
    // Lock before hashing; reuse the shared long-path and ancestor policy.
    impl_->input = CreateFileW(detail::file_path(executable).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    std::string actual;
    if (impl_->input == INVALID_HANDLE_VALUE || !inventory::fingerprint(executable, actual, error) || actual != sha256) {
        error = "producer executable lock/hash mismatch: " + error; return reject();
    }
    wchar_t image[32768]{}; DWORD length = 32768;
    std::error_code ec;
    if (!QueryFullProcessImageNameW(impl_->process, 0, image, &length) ||
        !std::filesystem::equivalent(detail::file_path(image), detail::file_path(executable), ec) || ec) {
        error = "producer process path differs from pinned executable"; return reject();
    }
    OVERLAPPED op{}; op.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!op.hEvent) { impl_->fail(error, "CreateEventW"); return reject(); }
    const auto connected = ConnectNamedPipe(impl_->pipe, &op);
    const auto code = connected ? ERROR_SUCCESS : GetLastError();
    DWORD transferred = 0;
    bool ok = code == ERROR_PIPE_CONNECTED;
    if (!ok) { SetLastError(code); ok = impl_->finish(op, connected, deadline, transferred, error); }
    CloseHandle(op.hEvent);
    ULONG pid = 0;
    if (!ok) return reject();
    if (!GetNamedPipeClientProcessId(impl_->pipe, &pid) || pid != GetProcessId(impl_->process) ||
        WaitForSingleObject(impl_->process, 0) != WAIT_TIMEOUT) {
        error = "frame pipe client is not the retained live producer process"; return reject();
    }
    impl_->connected = true;
    return true;
#else
    (void)producer_process; (void)executable; (void)sha256; (void)timeout_ms;
    error = "frame pipe requires Windows desktop"; return false;
#endif
}
bool FramePipe::connect(const std::wstring& name, std::uint32_t server_pid, std::string& error) {
    error.clear();
#ifdef _WIN32
    const std::wstring prefix = L"\\\\.\\pipe\\LOCAL\\XON-Frame-";
    if (impl_->pipe != INVALID_HANDLE_VALUE || !server_pid || !name.starts_with(prefix) ||
        name.size() != prefix.size() + 38 || name.substr(prefix.size()).find_first_not_of(L"{}-0123456789abcdefABCDEF") != std::wstring::npos) {
        error = "invalid or already open local frame endpoint"; return false;
    }
    impl_->pipe = CreateFileW(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
                             FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
    if (impl_->pipe == INVALID_HANDLE_VALUE) return impl_->fail(error, "CreateFileW(frame pipe)");
    ULONG pid = 0;
    if (!GetNamedPipeServerProcessId(impl_->pipe, &pid) || pid != server_pid) {
        error = "frame pipe server PID mismatch"; close(); return false;
    }
    impl_->name = name; impl_->connected = true;
    return true;
#else
    (void)name; (void)server_pid; error = "frame pipe requires Windows desktop"; return false;
#endif
}
bool FramePipe::send(const ExternalFrameView& frame, std::uint32_t timeout_ms, std::string& error) {
    error.clear();
#ifdef _WIN32
    if (!impl_->connected || impl_->server || !timeout_valid(timeout_ms, error)) { if (error.empty()) error = "frame producer not connected"; return false; }
    if (frame.provenance.origin == FrameOrigin::systemos_component) { error = "frame pipe does not certify SystemOS provenance"; return false; }
    if (!validate_external_frame(frame, error)) return false;
    const auto required = static_cast<std::uint64_t>(frame.height - 1) * frame.row_pitch_bytes + static_cast<std::uint64_t>(frame.width) * 4;
    if (required > max_bytes || impl_->sequence == std::numeric_limits<std::uint64_t>::max()) { error = "frame pipe payload/sequence limit"; return false; }
    const auto seq = impl_->sequence + 1;
    Header h{magic, version, static_cast<std::uint32_t>(seq), static_cast<std::uint32_t>(seq >> 32),
        frame.width, frame.height, frame.row_pitch_bytes, frame.format == PixelFormat::rgba8_unorm ? 1u : 2u,
        static_cast<std::uint32_t>(required), 0};
    const auto deadline = GetTickCount64() + timeout_ms;
    std::array<std::uint32_t, 2> ack{};
    if (!impl_->transfer(h.data(), sizeof(h), true, deadline, error) ||
        !impl_->transfer(const_cast<std::uint8_t*>(frame.pixels.data()), h[8], true, deadline, error) ||
        !impl_->transfer(ack.data(), sizeof(ack), false, deadline, error)) { close(); return false; }
    if (ack[0] != h[2] || ack[1] != h[3]) { error = "frame pipe acknowledgement mismatch"; close(); return false; }
    impl_->sequence = seq; return true;
#else
    (void)frame; (void)timeout_ms; error = "frame pipe requires Windows desktop"; return false;
#endif
}
bool FramePipe::receive(ExternalFrameView& frame, std::uint32_t timeout_ms, std::string& error) {
    frame = {}; error.clear();
#ifdef _WIN32
    if (!impl_->connected || !impl_->server || !timeout_valid(timeout_ms, error)) { if (error.empty()) error = "frame receiver not connected"; return false; }
    const auto deadline = GetTickCount64() + timeout_ms;
    Header h{};
    if (!impl_->transfer(h.data(), sizeof(h), false, deadline, error) || !layout(h, error)) { close(); return false; }
    const auto seq = static_cast<std::uint64_t>(h[2]) | (static_cast<std::uint64_t>(h[3]) << 32);
    if (impl_->sequence == std::numeric_limits<std::uint64_t>::max() || seq != impl_->sequence + 1) {
        error = "frame pipe sequence mismatch"; close(); return false;
    }
    impl_->pixels.resize(h[8]);
    std::array<std::uint32_t, 2> ack{h[2], h[3]};
    if (!impl_->transfer(impl_->pixels.data(), h[8], false, deadline, error) ||
        !impl_->transfer(ack.data(), sizeof(ack), true, deadline, error)) { close(); return false; }
    impl_->sequence = seq;
    frame = {impl_->pixels, h[4], h[5], h[6], h[7] == 1 ? PixelFormat::rgba8_unorm : PixelFormat::bgra8_unorm,
             {FrameOrigin::external_host_buffer, "ipc_cpu_producer", "", 0}};
    return true;
#else
    (void)timeout_ms; error = "frame pipe requires Windows desktop"; return false;
#endif
}
} // namespace xboxone::presentation
