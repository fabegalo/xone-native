// SPDX-License-Identifier: BSD-3-Clause
// xone-era check <title.exe>: maps and binds a title image without running it,
// then lists every import that still lacks a host implementation.
#include "xboxone_native/era_loader.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <string>

int wmain(int argc, wchar_t** argv) {
    if (argc != 3 || std::wstring(argv[1]) != L"check") {
        std::cerr << "Usage: xone-era check <title.exe>\n"
                     "Maps the image in-process, applies relocations and binds imports; never calls the entry point.\n";
        return 2;
    }
    std::ifstream in(std::filesystem::path(argv[2]), std::ios::binary);
    if (!in) { std::cerr << "cannot read input\n"; return 2; }
    const std::vector<std::uint8_t> file{std::istreambuf_iterator<char>(in), {}};

    xboxone::era::Image image;
    xboxone::era::LoadResult result;
    const bool ok = image.load(file, xboxone::era::resolve_kernelx, result);
    std::map<std::string, std::size_t> per_library;
    for (const auto& u : result.unresolved) ++per_library[u.library];
    std::cout << "resolved " << result.resolved << ", unresolved " << result.unresolved.size() << '\n';
    for (const auto& [library, n] : per_library) std::cout << "  " << library << ": " << n << " missing\n";
    for (const auto& u : result.unresolved)
        std::cout << "missing " << u.library << '!' << (u.name.empty() ? "#" + std::to_string(u.ordinal) : u.name) << '\n';
    for (const auto& e : result.errors) std::cout << "blocked: " << e << '\n';
    std::cout << (ok ? "mapped: ready for entry (not executed)\n" : "not mapped\n");
    return ok ? 0 : 1;
}
