// SPDX-License-Identifier: BSD-3-Clause
// Two owned desktop processes, hidden WARP output; no Xbox code or deployment.
#include "xboxone_native/frame_pipe.h"
#include "xboxone_native/inventory.h"
#include "frame_pipe_protocol.h"
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <iostream>

using namespace xboxone::presentation;
using namespace xboxone::presentation::pipe_protocol;
namespace {
int failures = 0, checks = 0;
void check(bool ok, const std::string& why) {
    ++checks; if (!ok) { ++failures; std::cerr << "FAIL: " << why << '\n'; }
}
std::vector<std::uint8_t> expected() {
    std::vector<std::uint8_t> out(4 * 3 * 4);
    for (std::size_t i = 0; i < out.size(); i += 4) {
        out[i] = static_cast<std::uint8_t>(i + 1); out[i + 1] = 70;
        out[i + 2] = 190; out[i + 3] = 255;
    }
    return out;
}
int producer(const std::wstring& endpoint, DWORD parent, const std::wstring& mode) {
    std::string error;
    if (mode == L"normal" || mode == L"sender_timeout" || mode == L"sender_backpressure") {
        FramePipe pipe;
        if (!pipe.connect(endpoint, parent, error)) { std::cerr << error; return 2; }
        auto pixels = expected();
        ExternalFrameView frame{pixels, 4, 3, 16, PixelFormat::rgba8_unorm,
            {FrameOrigin::synthetic_test, "owned_pipe_test", "", 0}};
        if (mode == L"sender_backpressure") {
            pixels.resize(1024 * 1024);
            frame = {pixels, 512, 512, 2048, PixelFormat::rgba8_unorm, frame.provenance};
        }
        if (mode != L"normal") {
            if (pipe.send(frame, 100, error) || error.find("timeout") == std::string::npos) return 6;
            if (pipe.send(frame, 100, error)) return 7;
            return 0;
        }
        if (pipe.send(frame, 0, error) || pipe.send(frame, 60001, error)) return 8;
        auto bad = frame;
        bad.row_pitch_bytes = 1;
        if (pipe.send(bad, 100, error)) return 9;
        bad = frame;
        bad.provenance = {FrameOrigin::systemos_component, "unverified", std::string(64, 'a'), 1};
        if (pipe.send(bad, 100, error)) return 10;
        if (!pipe.send(frame, 5000, error)) { std::cerr << error; return 3; }
        std::vector<std::uint8_t> padded(24 * 3, 0x55);
        for (std::size_t y = 0; y < 3; ++y) for (std::size_t x = 0; x < 4; ++x) {
            const auto s = (y * 4 + x) * 4, d = y * 24 + x * 4;
            padded[d] = pixels[s + 2]; padded[d + 1] = pixels[s + 1];
            padded[d + 2] = pixels[s]; padded[d + 3] = pixels[s + 3];
        }
        frame = {padded, 4, 3, 24, PixelFormat::bgra8_unorm, frame.provenance};
        if (!pipe.send(frame, 5000, error)) { std::cerr << error; return 4; }
        return 0;
    }
    // Deliberately malformed peer, independent of the production serializer.
    HANDLE raw = CreateFileW(endpoint.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (raw == INVALID_HANDLE_VALUE) return 5;
    Header h{magic, version, 1, 0, 4, 3, 16, 1, 48, 0};
    if (mode == L"version") h[1] = 99;
    if (mode == L"oversized") { h[4] = 16384; h[5] = 16384; h[6] = 65536; h[8] = 1073741824; }
    if (mode == L"pitch") h[6] = 1;
    if (mode == L"format") h[7] = 99;
    if (mode == L"sequence") h[2] = 2;
    if (mode == L"reserved") h[9] = 1;
    DWORD count = 0;
    // Leave a live connected peer with no bytes, to test cancellation.
    if (mode == L"stall" || mode == L"identity" || mode == L"hash") Sleep(1500);
    else {
        WriteFile(raw, h.data(), mode == L"truncated" ? 8 : sizeof(h), &count, nullptr);
        if (mode == L"payload_stall") Sleep(1500);
        else if (mode == L"replay") {
            auto pixels = expected();
            WriteFile(raw, pixels.data(), static_cast<DWORD>(pixels.size()), &count, nullptr);
            std::uint64_t ack = 0;
            if (ReadFile(raw, &ack, sizeof(ack), &count, nullptr)) WriteFile(raw, h.data(), sizeof(h), &count, nullptr);
        }
        // Keep the process object alive through receiver identity validation.
        if (mode != L"payload_stall") Sleep(250);
    }
    CloseHandle(raw);
    return 0;
}
struct Child {
    PROCESS_INFORMATION info{};
    bool start(const std::filesystem::path& exe, const std::wstring& endpoint, const std::wstring& mode) {
        auto command = L"\"" + exe.wstring() + L"\" --producer \"" + endpoint + L"\" " +
            std::to_wstring(GetCurrentProcessId()) + L" " + mode;
        STARTUPINFOW startup{sizeof(startup)};
        return CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, FALSE,
            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &info) != FALSE;
    }
    bool exited_ok() {
        DWORD code = 99;
        return WaitForSingleObject(info.hProcess, 6000) == WAIT_OBJECT_0 &&
            GetExitCodeProcess(info.hProcess, &code) && code == 0;
    }
    ~Child() {
        if (info.hProcess) {
            if (WaitForSingleObject(info.hProcess, 0) != WAIT_OBJECT_0) {
                TerminateProcess(info.hProcess, 99); WaitForSingleObject(info.hProcess, 3000);
            }
            CloseHandle(info.hProcess);
        }
        if (info.hThread) CloseHandle(info.hThread);
    }
};
void run(const std::filesystem::path& exe, const std::string& hash, Presenter& presenter, const std::wstring& mode) {
    FramePipe receiver;
    std::string error;
    check(receiver.create(error), "create local pipe: " + error);
    const auto old_endpoint = receiver.endpoint();
    Child child;
    if (!child.start(exe, receiver.endpoint(), mode)) { check(false, "start owned producer"); return; }
    const auto accepted = receiver.accept(mode == L"identity" ? GetCurrentProcess() : child.info.hProcess,
        exe, mode == L"hash" ? std::string(64, '0') : hash, 3000, error);
    if (mode == L"identity" || mode == L"hash") {
        check(!accepted, "reject producer identity/hash mismatch");
        check(!error.empty(), "identity rejection has diagnostic");
        return;
    }
    if (!accepted) { check(false, "accept pinned producer: " + error); return; }
    ExternalFrameView frame;
    if (mode == L"sender_timeout" || mode == L"sender_backpressure") {
        check(child.exited_ok(), "producer I/O and backpressure timeout cancel safely");
        check(!receiver.receive(frame, 500, error) && frame.pixels.empty(), "aborted sender cannot publish a frame");
    } else if (mode == L"normal") {
        for (int i = 0; i < 2; ++i) {
            const bool got = receiver.receive(frame, 3000, error);
            check(got, "receive owned pixels: " + error);
            if (!got) break;
            check(frame.width == 4 && frame.height == 3 && frame.row_pitch_bytes == (i ? 24u : 16u), "frame layout preserved");
            check(frame.provenance.origin == FrameOrigin::external_host_buffer, "producer cannot claim authentic origin");
            check(presenter.present_frame(frame, error), "production Presenter accepts IPC frame: " + error);
            std::vector<std::uint8_t> pixels; std::uint32_t w = 0, h = 0; PixelFormat format{};
            check(presenter.readback_last_backbuffer(pixels, w, h, format, error) && pixels == expected() && w == 4 && h == 3,
                "WARP readback equals independent RGBA expectation, including padded BGRA");
        }
        check(child.exited_ok(), "producer receives acknowledgements and exits zero");
        check(!receiver.receive(frame, 500, error) && frame.pixels.empty(), "producer exit returns no stale frame");
    } else {
        if (mode == L"replay") check(receiver.receive(frame, 3000, error), "first frame before replay accepted");
        const auto began = GetTickCount64();
        const auto timeout = mode == L"stall" || mode == L"payload_stall" ? 100u : 3000u;
        check(!receiver.receive(frame, timeout, error) && frame.pixels.empty() && !error.empty(), "reject malformed, stalled or truncated frame");
        if (timeout == 100) check(error.find("timeout") != std::string::npos && GetTickCount64() - began < 1000,
                                  "bounded I/O timeout cancelled and drained");
        check(!receiver.receive(frame, 1, error), "failed stream cannot resume partial data");
    }
    receiver.close();
    FramePipe late;
    check(!late.connect(old_endpoint, GetCurrentProcessId(), error), "closed session cannot be reconnected");
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc == 5 && std::wstring(argv[1]) == L"--producer")
        return producer(argv[2], static_cast<DWORD>(std::stoul(argv[3])), argv[4]);
    wchar_t image[32768]{};
    if (!GetModuleFileNameW(nullptr, image, 32768)) return 1;
    std::filesystem::path exe = image;
    std::string hash, error;
    if (!xboxone::inventory::fingerprint(exe, hash, error)) return 1;
    Presenter presenter;
    PresenterConfig config;
    config.width = 4; config.height = 3; config.prefer_software_adapter = true; config.visible = false;
    if (!presenter.initialize(config, error)) { std::cerr << error; return 1; }
    for (const wchar_t* mode : {L"normal", L"version", L"oversized", L"pitch", L"format", L"sequence", L"reserved",
                               L"truncated", L"stall", L"payload_stall", L"replay", L"identity", L"hash",
                               L"sender_timeout", L"sender_backpressure", L"normal"})
        run(exe, hash, presenter, mode);
    check(presenter.stats().authentic_systemos_frames == 0 && presenter.stats().external_host_frames == 4,
          "two restarted sessions produce four external frames and zero authentic frames");
    FramePipe unconnected;
    ExternalFrameView frame;
    check(!unconnected.receive(frame, 1, error) && !unconnected.send(frame, 1, error), "unconnected endpoints rejected");
    check(unconnected.create(error), "create empty session");
    check(!unconnected.create(error), "duplicate create refused");
    check(!unconnected.accept(GetCurrentProcess(), exe, hash, 50, error) && error.find("timeout") != std::string::npos,
          "connect timeout closes empty session");
    check(!unconnected.connect(L"\\\\remote\\pipe\\bad", GetCurrentProcessId(), error), "remote endpoint refused");
    presenter.shutdown();
    std::cout << checks << " IPC checks; failures=" << failures << "; WARP; original components=0; authentic frames=0\n";
    return failures ? 1 : 0;
}
