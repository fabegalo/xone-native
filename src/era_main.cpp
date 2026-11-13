// SPDX-License-Identifier: BSD-3-Clause
// xone-era check <title.exe>: maps and binds a title image without running it,
// then lists every import that still lacks a host implementation.
// xone-era run <title.exe> [--timeout-ms N] [--storage DIR]: maps the title and
// runs its entry point natively on this thread, with the package directory as
// the working directory. A watchdog ends the process at the deadline; faults
// are reported with title-relative addresses.
#include "xboxone_native/era_loader.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <string>
#include <utility>

namespace {

std::uintptr_t title_base = 0, title_end = 0;
volatile LONG reported = 0;
std::array<xboxone::era::ImportRequest, 256> stopped_imports;
std::size_t stopped_count = 0;

void report(const char* text) {
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), text, static_cast<DWORD>(std::strlen(text)), &written, nullptr);
}

template<std::size_t Index>
[[noreturn]] __declspec(noinline) void stop_import(std::uintptr_t a, std::uintptr_t b, std::uintptr_t c, std::uintptr_t d) {
    const auto& request = stopped_imports[Index];
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    char line[1024];
    std::snprintf(line, sizeof line, "xone-era: unimplemented %s!%s ordinal=%u at title+0x%llX; stopped\n",
                  request.library.c_str(), request.name.c_str(), request.ordinal,
                  static_cast<unsigned long long>(caller - title_base));
    report(line);
    std::snprintf(line, sizeof line, "xone-era: raw integer args 0x%llX 0x%llX 0x%llX 0x%llX\n",
                  static_cast<unsigned long long>(a), static_cast<unsigned long long>(b),
                  static_cast<unsigned long long>(c), static_cast<unsigned long long>(d));
    report(line);
    if (request.name == "D3D11XCreateDeviceX" && xboxone::era::same_library(request.library, "d3d11_x.dll")) {
        std::array<DWORD, 11> descriptor{}; // Extent verified at the reference ABI entry.
        SIZE_T read = 0;
        if (ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(a), descriptor.data(),
                              sizeof descriptor, &read) && read == sizeof descriptor) {
            std::snprintf(line, sizeof line, "xone-era: raw device descriptor %lX %lX %lX %lX %lX %lX %lX %lX %lX %lX %lX\n",
                descriptor[0], descriptor[1], descriptor[2], descriptor[3], descriptor[4], descriptor[5],
                descriptor[6], descriptor[7], descriptor[8], descriptor[9], descriptor[10]);
            report(line);
        }
    }
    TerminateProcess(GetCurrentProcess(), 125);
    std::abort();
}

template<std::size_t... Index>
auto import_stops(std::index_sequence<Index...>) {
    using Stop = void(*)(std::uintptr_t, std::uintptr_t, std::uintptr_t, std::uintptr_t);
    return std::array<Stop, sizeof...(Index)>{&stop_import<Index>...};
}
const auto stops = import_stops(std::make_index_sequence<256>{});

LONG CALLBACK on_exception(EXCEPTION_POINTERS* info) {
    const DWORD code = info->ExceptionRecord->ExceptionCode;
    if (code == DBG_PRINTEXCEPTION_C || code == DBG_PRINTEXCEPTION_WIDE_C || code == 0x406D1388)
        return EXCEPTION_CONTINUE_SEARCH; // Debug print and thread naming are not faults.
    if (InterlockedIncrement(&reported) > 16) return EXCEPTION_CONTINUE_SEARCH;
    const auto at = reinterpret_cast<std::uintptr_t>(info->ExceptionRecord->ExceptionAddress);
    char line[256];
    if (at >= title_base && at < title_end)
        std::snprintf(line, sizeof line, "xone-era: exception 0x%08lX at title+0x%llX\n", code,
                      static_cast<unsigned long long>(at - title_base));
    else {
        HMODULE module = nullptr;
        char name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCSTR>(at), &module))
            GetModuleFileNameA(module, name, MAX_PATH);
        std::snprintf(line, sizeof line, "xone-era: exception 0x%08lX at %s+0x%llX\n", code, name,
                      static_cast<unsigned long long>(at - reinterpret_cast<std::uintptr_t>(module)));
    }
    report(line);
    if (code == EXCEPTION_ACCESS_VIOLATION && info->ExceptionRecord->NumberParameters >= 2) {
        std::snprintf(line, sizeof line, "xone-era:   %s 0x%llX\n",
                      info->ExceptionRecord->ExceptionInformation[0] ? "write to" : "read from",
                      static_cast<unsigned long long>(info->ExceptionRecord->ExceptionInformation[1]));
        report(line);
        const auto& c = *info->ContextRecord;
        std::snprintf(line, sizeof line, "xone-era:   rax=%llX rbx=%llX rcx=%llX rdx=%llX rsi=%llX rdi=%llX r8=%llX r9=%llX\n",
                      c.Rax, c.Rbx, c.Rcx, c.Rdx, c.Rsi, c.Rdi, c.R8, c.R9);
        report(line);
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

DWORD WINAPI watchdog(void* timeout) {
    Sleep(static_cast<DWORD>(reinterpret_cast<std::uintptr_t>(timeout)));
    report("xone-era: deadline reached; terminating title process\n");
    TerminateProcess(GetCurrentProcess(), 124);
    return 0;
}

void on_exit() { report("xone-era: title called exit\n"); }

int usage() {
    std::cerr << "Usage: xone-era check <title.exe>\n"
                 "       xone-era run <title.exe> [--timeout-ms N] [--storage DIR] [--stop-on-missing]\n"
                 "check maps the image and binds imports without calling the entry point.\n"
                 "run also executes the title natively in this process (default deadline 15000 ms).\n"
                 "--stop-on-missing diagnoses the first unimplemented import and terminates with code 125.\n";
    return 2;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) return usage();
    const std::wstring command = argv[1];
    const bool run = command == L"run";
    if (!run && (command != L"check" || argc != 3)) return usage();
    DWORD timeout = 15000;
    std::wstring storage;
    bool stop_on_missing = false;
    for (int i = 3; i < argc; ++i) {
        const std::wstring option = argv[i];
        if (option == L"--timeout-ms" && i + 1 < argc) timeout = static_cast<DWORD>(std::wcstoul(argv[++i], nullptr, 10));
        else if (option == L"--storage" && i + 1 < argc) storage = argv[++i];
        else if (option == L"--stop-on-missing") stop_on_missing = true;
        else return usage();
    }
    if (run && (timeout == 0 || timeout > 600000)) { std::cerr << "timeout must be 1..600000 ms\n"; return 2; }

    const std::filesystem::path title = std::filesystem::absolute(argv[2]);
    std::ifstream in(title, std::ios::binary);
    if (!in) { std::cerr << "cannot read input\n"; return 2; }
    const std::vector<std::uint8_t> file{std::istreambuf_iterator<char>(in), {}};
    // The extracted package layout keeps the original system revision here.
    // A missing/unknown module leaves revision-specific imports unresolved.
    (void)xboxone::era::bind_d3d11x_abi((title.parent_path().parent_path() /
        L"EmbeddedXvd/Windows/System32/combase.dll").wstring());
    const HRESULT xg_binding = xboxone::era::bind_xg_module((title.parent_path().parent_path() /
        L"EmbeddedXvd/Windows/System32/xg_x.dll").wstring());
    if (FAILED(xg_binding) && xg_binding != HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) &&
        xg_binding != HRESULT_FROM_WIN32(ERROR_PATH_NOT_FOUND))
        std::fprintf(stderr, "xone-era: original XG binding refused: 0x%08lX\n", xg_binding);

    xboxone::era::Image image;
    xboxone::era::LoadResult result;
    const bool ok = image.load(file, [&](const xboxone::era::ImportRequest& request) -> void* {
        if (void* address = xboxone::era::resolve_title_import(request)) return address;
        if (!stop_on_missing || stopped_count == stopped_imports.size()) return nullptr;
        stopped_imports[stopped_count] = request;
        return reinterpret_cast<void*>(stops[stopped_count++]);
    }, result);
    std::map<std::string, std::size_t> per_library;
    for (const auto& u : result.unresolved) ++per_library[u.library];
    std::cout << "resolved " << result.resolved - stopped_count << ", unresolved " << result.unresolved.size() + stopped_count << '\n';
    if (stopped_count) std::cout << "diagnostic only: " << stopped_count << " imports stop on call (not implemented)\n";
    for (const auto& [library, n] : per_library) std::cout << "  " << library << ": " << n << " missing\n";
    for (const auto& u : result.unresolved)
        std::cout << "missing " << u.library << '!' << (u.name.empty() ? "#" + std::to_string(u.ordinal) : u.name) << '\n';
    for (const auto& e : result.errors) std::cout << "blocked: " << e << '\n';
    if (!ok) { std::cout << "not mapped\n"; return 1; }
    if (!run) { std::cout << "mapped: ready for entry (not executed)\n"; return 0; }
    if (!image.entry()) { std::cout << "image has no entry point\n"; return 1; }

    // Title package root is the working directory, as on console.
    if (!SetCurrentDirectoryW(title.parent_path().c_str())) {
        std::cerr << "cannot enter package directory: Win32 " << GetLastError() << '\n'; return 1;
    }
    xboxone::era::set_title_storage_root(storage);
    xboxone::era::set_title_package_root(title.parent_path().wstring());
    xboxone::era::set_stop_on_missing_activation(stop_on_missing);
    for (const auto* runtime : {L"vccorlib110.dll", L"vccorlib140.dll"}) {
        const HMODULE module = GetModuleHandleW(runtime);
        if (module && !xboxone::era::hook_winrt_activation(module))
            report("xone-era: warning: WinRT activation boundary not installed\n");
    }
    title_base = reinterpret_cast<std::uintptr_t>(image.base());
    title_end = title_base + image.size();
    AddVectoredExceptionHandler(1, on_exception);
    std::atexit(on_exit);
    HANDLE dog = CreateThread(nullptr, 0, watchdog, reinterpret_cast<void*>(static_cast<std::uintptr_t>(timeout)), 0, nullptr);
    if (!dog) { std::cerr << "cannot start watchdog\n"; return 1; }
    CloseHandle(dog);

    std::cout << "running title at 0x" << std::hex << title_base << std::dec << " (deadline " << timeout << " ms)\n"
              << std::flush;
    if (FAILED(xboxone::era::initialize_xg_module())) { report("xone-era: XG module initialization failed\n"); return 1; }
    image.process_attach();
    reinterpret_cast<void (*)()>(image.entry())();
    report("xone-era: title entry returned\n");
    return 0;
}
