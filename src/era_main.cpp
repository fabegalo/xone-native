// SPDX-License-Identifier: BSD-3-Clause
// xone-era check <title.exe>: maps and binds a title image without running it,
// then lists every import that still lacks a host implementation.
// xone-era run <title.exe> [--timeout-ms N] [--storage DIR]: maps the title and
// runs its entry point natively on this thread, with the package directory as
// the working directory. A watchdog ends the process at the deadline; faults
// are reported with title-relative addresses.
#include "xboxone_native/era_loader.h"
#include "era_display.h"
#include "sha256.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>

#include <array>
#include <atomic>
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
std::atomic<bool> diagnostic_exceptions{false};

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

void diagnose_callers(const CONTEXT& original) {
    CONTEXT context=original;
    __try {
        for (UINT i=0;i<48 && context.Rip;++i) {
            if (context.Rip>=title_base && context.Rip<title_end) {
                char line[96];
                std::snprintf(line,sizeof line,"xone-era:   C++ caller title+0x%llX\n",context.Rip-title_base);
                report(line);
            }
            DWORD64 base=0;
            auto* entry=RtlLookupFunctionEntry(context.Rip,&base,nullptr);
            if (entry) {
                void* data=nullptr;DWORD64 frame=0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER,base,context.Rip,entry,&context,&data,&frame,nullptr);
            } else {
                SIZE_T read=0;
                if (!ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<const void*>(context.Rsp),&context.Rip,sizeof context.Rip,&read) || read!=sizeof context.Rip) break;
                context.Rsp+=sizeof context.Rip;
            }
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) { report("xone-era:   C++ caller unwind incomplete\n"); }
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
    if (code==0xE06D7363 && info->ExceptionRecord->NumberParameters==4 &&
        (xboxone::era::stop_on_missing_service() || diagnostic_exceptions.load())) {
        // Diagnostic only: MSVC x64 EH metadata uses image-relative offsets.
        // https://devblogs.microsoft.com/oldnewthing/20100730-00/?p=13273
        const auto* data=info->ExceptionRecord->ExceptionInformation;
        const auto relative=[&](std::uintptr_t address,UINT offset,std::uintptr_t& result) {
            DWORD rva=0;SIZE_T read=0;
            if(!ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<const void*>(address+offset),&rva,sizeof rva,&read) || read!=sizeof rva) return false;
            result=data[3]+rva;return rva!=0;
        };
        std::uintptr_t types=0,type=0,name=0;
        char type_name[96]{};SIZE_T read=0;
        if(relative(data[2],12,types) && relative(types,4,type) && relative(type,4,name) &&
            ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<const void*>(name+16),type_name,sizeof type_name-1,&read)) {
            std::snprintf(line,sizeof line,"xone-era:   C++ type %.95s metadata image 0x%llX\n",type_name,static_cast<unsigned long long>(data[3]));
            report(line);
        }
        diagnose_callers(*info->ContextRecord);
    }
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
                 "       xone-era run <title.exe> [--timeout-ms N] [--storage DIR] [--warp] [--stop-on-missing | --stop-on-missing-imports]\n"
                 "check maps the image and binds imports without calling the entry point.\n"
                 "run also executes the title natively in this process (default deadline 15000 ms).\n"
                 "--stop-on-missing diagnoses missing imports, activations and service calls (exit 125).\n"
                 "--stop-on-missing-imports traps missing imports but returns real activation failures to the title.\n";
    std::cerr << "--warp selects the software GPU explicitly and enables bounded graphics diagnostics.\n";
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
    bool stop_on_activation = false;
    bool warp = false;
    for (int i = 3; i < argc; ++i) {
        const std::wstring option = argv[i];
        if (option == L"--timeout-ms" && i + 1 < argc) timeout = static_cast<DWORD>(std::wcstoul(argv[++i], nullptr, 10));
        else if (option == L"--storage" && i + 1 < argc) storage = argv[++i];
        else if (option == L"--stop-on-missing") { stop_on_missing = true; stop_on_activation = true; }
        else if (option == L"--stop-on-missing-imports") stop_on_missing = true;
        else if (option == L"--warp") warp = true;
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
    xboxone::detail::Sha256 title_hash;title_hash.update(file);
    const auto selected=title.filename().u8string();
    xboxone::era::set_title_package_root(title.parent_path().wstring(),std::string(selected.begin(),selected.end()),title_hash.finish());
    xboxone::era::set_stop_on_missing_activation(stop_on_activation);
    xboxone::era::set_graphics_diagnostics(stop_on_missing || warp, warp);
    diagnostic_exceptions.store(stop_on_missing);
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
