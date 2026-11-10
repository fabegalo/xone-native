// SPDX-License-Identifier: BSD-3-Clause
#include "xboxone_native/era_loader.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <fstream>
#include <iostream>
#include <iterator>

namespace {
int failures = 0;
void check(bool ok, const char* what) {
    if (!ok) { ++failures; std::cerr << "FAIL: " << what << '\n'; }
}
std::vector<std::uint8_t> read(const char* path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), {}};
}
} // namespace

int main(int argc, char** argv) {
    using namespace xboxone::era;
    if (argc != 2) { std::cerr << "usage: era_loader_tests <era_component.exe>\n"; return 2; }
    const auto file = read(argv[1]);
    check(file.size() > 512, "fixture readable");

    {
        Image image;
        LoadResult result;
        check(image.load(file, resolve_kernelx, result), "fixture loads with kernelx contract");
        for (const auto& e : result.errors) std::cerr << "  " << e << '\n';
        check(result.resolved == 2 && result.unresolved.empty(), "both kernelx imports bound");
        check(image.entry() != nullptr, "entry point mapped");
        if (image.entry()) {
            MEMORY_BASIC_INFORMATION info{};
            VirtualQuery(image.entry(), &info, sizeof info);
            check(info.Protect == PAGE_EXECUTE_READ, "code section is execute-read, not writable");
            DWORD64 unwind_base = 0;
            check(RtlLookupFunctionEntry(reinterpret_cast<DWORD64>(image.entry()), &unwind_base, nullptr) &&
                  unwind_base == reinterpret_cast<DWORD64>(image.base()), "unwind data registered for title code");
            const auto entry = reinterpret_cast<unsigned long long (*)()>(image.entry());
            const auto value = entry();
            check((value & 0xffffffffull) == GetCurrentProcessId(), "title called host through kernelx binding");
            check((value >> 32) == 42, "DIR64 relocation applied at non-preferred base");
        }
    }
    {
        Image image;
        LoadResult result;
        check(!image.load(file, [](const ImportRequest&) -> void* { return nullptr; }, result), "missing contract blocks load");
        check(result.unresolved.size() == 2 && result.unresolved[0].library == "kernelx.dll",
              "every unresolved import reported with its library");
        check(image.base() == nullptr, "failed load releases the image");
    }
    {
        Image image;
        LoadResult result;
        auto truncated = file;
        truncated.resize(256);
        check(!image.load(truncated, resolve_kernelx, result) && !result.errors.empty(), "truncated image rejected");
        auto wrong = file;
        const auto lfanew = *reinterpret_cast<const LONG*>(wrong.data() + 0x3c);
        wrong[lfanew + 4] = 0x4c; wrong[lfanew + 5] = 0x01; // IMAGE_FILE_MACHINE_I386
        check(!image.load(wrong, resolve_kernelx, result), "non-AMD64 image rejected");
    }
    check(resolve_kernelx({"KERNELX.DLL", "GetTickCount64", 0}) != nullptr, "library name is case-insensitive");
    check(resolve_kernelx({"kernelx.dll", "AllocateTitlePhysicalPages", 0}) == nullptr,
          "Xbox-specific export is not faked by a host forward");
    check(resolve_kernelx({"kernelx.dll", "VirtualAlloc", 0}) == nullptr, "title memory API is not forwarded blindly");
    check(resolve_kernelx({"kernel32.dll", "Sleep", 0}) == nullptr, "only kernelx.dll is handled");

    std::cout << (failures ? "era_loader: FAILED\n" : "era_loader: all checks passed\n");
    return failures ? 1 : 0;
}
