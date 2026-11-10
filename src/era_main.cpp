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

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <string>

namespace {

std::uintptr_t title_base = 0, title_end = 0;
volatile LONG reported = 0;

void report(const char* text) {
    DWORD written = 0;
    WriteFile(GetStdHandle(STD_ERROR_HANDLE), text, static_cast<DWORD>(std::strlen(text)), &written, nullptr);
}

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
                 "       xone-era run <title.exe> [--timeout-ms N] [--storage DIR]\n"
                 "check maps the image and binds imports without calling the entry point.\n"
                 "run also executes the title natively in this process (default deadline 15000 ms).\n";
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
    for (int i = 3; i < argc; ++i) {
        const std::wstring option = argv[i];
        if (option == L"--timeout-ms" && i + 1 < argc) timeout = static_cast<DWORD>(std::wcstoul(argv[++i], nullptr, 10));
        else if (option == L"--storage" && i + 1 < argc) storage = argv[++i];
        else return usage();
    }
    if (run && (timeout == 0 || timeout > 600000)) { std::cerr << "timeout must be 1..600000 ms\n"; return 2; }

    const std::filesystem::path title = std::filesystem::absolute(argv[2]);
    std::ifstream in(title, std::ios::binary);
    if (!in) { std::cerr << "cannot read input\n"; return 2; }
    const std::vector<std::uint8_t> file{std::istreambuf_iterator<char>(in), {}};

    xboxone::era::Image image;
    xboxone::era::LoadResult result;
    const bool ok = image.load(file, xboxone::era::resolve_title_import, result);
    std::map<std::string, std::size_t> per_library;
    for (const auto& u : result.unresolved) ++per_library[u.library];
    std::cout << "resolved " << result.resolved << ", unresolved " << result.unresolved.size() << '\n';
    for (const auto& [library, n] : per_library) std::cout << "  " << library << ": " << n << " missing\n";
    for (const auto& u : result.unresolved)
        std::cout << "missing " << u.library << '!' << (u.name.empty() ? "#" + std::to_string(u.ordinal) : u.name) << '\n';
    for (const auto& e : result.errors) std::cout << "blocked: " << e << '\n';
    if (!ok) { std::cout << "not mapped\n"; return 1; }
    if (!run) { std::cout << "mapped: ready for entry (not executed)\n"; return 0; }
    if (!image.entry()) { std::cout << "image has no entry point\n"; return 1; }

    // Title package root is the working directory, as on console.
    if (!SetCurrentDirectoryW(title.parent_path().c_str())) { std::cerr << "cannot enter package directory\n"; return 1; }
    xboxone::era::set_title_storage_root(storage);
    xboxone::era::set_title_package_root(title.parent_path().wstring());
    if (!xboxone::era::hook_winrt_activation(GetModuleHandleW(L"vccorlib140.dll")))
        report("xone-era: warning: WinRT activation boundary not installed\n");
    title_base = reinterpret_cast<std::uintptr_t>(image.base());
    title_end = title_base + image.size();
    AddVectoredExceptionHandler(1, on_exception);
    std::atexit(on_exit);
    HANDLE dog = CreateThread(nullptr, 0, watchdog, reinterpret_cast<void*>(static_cast<std::uintptr_t>(timeout)), 0, nullptr);
    if (!dog) { std::cerr << "cannot start watchdog\n"; return 1; }
    CloseHandle(dog);

    std::cout << "running title at 0x" << std::hex << title_base << std::dec << " (deadline " << timeout << " ms)\n"
              << std::flush;
    image.process_attach();
    reinterpret_cast<void (*)()>(image.entry())();
    report("xone-era: title entry returned\n");
    return 0;
}
