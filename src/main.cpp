// SPDX-License-Identifier: BSD-3-Clause
#include "xboxone_native/inspection.h"
#include "xboxone_native/inventory.h"

#include <charconv>
#include <array>
#include <cstdio>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

namespace {
std::string escape_json(std::string_view s) {
    std::string result;
    result.reserve(s.size() + 8);
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { result += '\\'; result += static_cast<char>(c); }
        else if (c < 0x20) {
            result += "\\u00";
            result += hex[c >> 4];
            result += hex[c & 15];
        } else result += static_cast<char>(c);
    }
    return result;
}

void print_report(const xboxone::inspection::Report& report, bool json) {
    using xboxone::inspection::Kind;
    if (json) {
        std::cout << "{\"kind\":\"" << (report.kind == Kind::xvd ? "xvd" : "pe64")
                  << "\",\"file_size\":" << report.file_size;
        if (report.kind == Kind::xvd) {
            const auto& x = report.xvd;
            std::cout << ",\"format_version\":" << x.format_version
                      << ",\"volume_flags\":" << x.volume_flags
                      << ",\"content_type\":" << x.content_type
                      << ",\"block_size\":" << x.block_size
                      << ",\"declared_drive_size\":" << x.declared_drive_size
                      << ",\"required_system_version\":" << x.required_system_version
                      << ",\"package_version_raw\":" << x.package_version_raw
                      << ",\"created_filetime_raw\":" << x.created_filetime_raw
                      << ",\"signature_verified\":false,\"decrypted\":false";
        } else {
            std::cout << ',';
            xboxone::inspection::write_pe_json_fields(std::cout, report.pe);
        }
        std::cout << "}\n";
        return;
    }
    std::cout << "File size: " << report.file_size << " bytes\n";
    if (report.kind == Kind::xvd) {
        const auto& x = report.xvd;
        std::cout << "XVD header recognized (not authenticated/decrypted)\n"
                  << "Format version: " << x.format_version << "\n"
                  << "Content type: " << x.content_type << "\n"
                  << "Block size: " << x.block_size << "\n"
                  << "Declared drive size: " << x.declared_drive_size << "\n"
                  << "Required system version (raw): " << x.required_system_version << "\n";
        std::cout << "Volume flags (raw): " << x.volume_flags << "\n"
                  << "Package version (raw): " << x.package_version_raw << "\n"
                  << "Created FILETIME (raw): " << x.created_filetime_raw << "\n";
    } else {
        const auto& p = report.pe;
        std::cout << "PE32+ x86-64 image (not identified as an Xbox executable by itself)\n"
                  << "Machine: 0x" << std::hex << p.machine << std::dec << "\n"
                  << "Subsystem: " << p.subsystem << "\n"
                  << "Sections: " << p.sections << "\n"
                  << "Entry point RVA: 0x" << std::hex << p.entry_point_rva << std::dec << "\n";
        for (const auto& library : p.imports) {
            std::cout << "Import: " << library.library << "\n";
            for (const auto& symbol : library.functions) std::cout << "  " << symbol << "\n";
        }
    }
}

void help() {
    std::cout << "xone-native M0: read-only original content inspector\n"
              << "Usage: xone-native inspect <file> [--json]\n"
              << "       xone-native inventory <directory> [--json]\n"
              << "           [--max-file-bytes N] [--max-total-bytes N]\n"
              << "       xone-native inspect-xvd-header <declared-file-size> [--json]\n"
              << "           (archive adapter: reads exactly 0xE00 binary bytes from stdin)\n"
              << "Inventory: .exe/.dll/.sys/.xvd; SHA-256, imports, local candidates.\n"
              << "Defaults: 256 MiB/file, 1 GiB total, 10000 files, 100000 entries, depth 64.\n"
              << "No symlink traversal, execution, decryption or source copies.\n";
}

void print_strings(const std::vector<std::string>& strings) {
    std::cout << '[';
    bool first = true;
    for (const auto& value : strings) {
        if (!first) std::cout << ',';
        first = false;
        std::cout << '"' << escape_json(value) << '"';
    }
    std::cout << ']';
}

void print_inventory(const xboxone::inventory::Report& report, bool json) {
    if (!json) {
        std::cout << "Inventory: " << report.files.size() << " files, " << report.hashed_bytes
                  << " hashed bytes, complete: " << (report.complete ? "yes" : "no") << '\n';
        for (const auto& file : report.files) {
            std::cout << file.path << "  SHA256 " << file.sha256 << '\n';
            if (!file.error.empty()) std::cout << "  Inspection failed: " << file.error << '\n';
        }
        for (const auto& excluded : report.excluded)
            std::cout << "Excluded: " << excluded.path << " (" << excluded.reason << ")\n";
        for (const auto& dependency : report.dependencies) {
            std::cout << "Dependency: " << dependency.library << " [" << dependency.status << "]\n";
            for (const auto& path : dependency.candidates) std::cout << "  Candidate: " << path << '\n';
        }
        std::cout << "Candidates are unverified; no version, export, ABI or execution compatibility established.\n";
        return;
    }
    std::cout << "{\"schema_version\":1,\"complete\":" << (report.complete ? "true" : "false")
              << ",\"hashed_bytes\":" << report.hashed_bytes << ",\"files\":[";
    bool first = true;
    for (const auto& file : report.files) {
        if (!first) std::cout << ',';
        first = false;
        std::cout << "{\"path\":\"" << escape_json(file.path) << "\",\"sha256\":\"" << file.sha256 << '"';
        if (!file.error.empty()) {
            std::cout << ",\"error\":\"" << escape_json(file.error) << '"';
        } else {
            std::cout << ",\"inspection\":";
            print_report(file.image, true);
        }
        std::cout << '}';
    }
    std::cout << "],\"excluded\":[";
    first = true;
    for (const auto& excluded : report.excluded) {
        if (!first) std::cout << ',';
        first = false;
        std::cout << "{\"path\":\"" << escape_json(excluded.path) << "\",\"reason\":\""
                  << escape_json(excluded.reason) << "\"}";
    }
    std::cout << "],\"dependencies\":[";
    first = true;
    for (const auto& dependency : report.dependencies) {
        if (!first) std::cout << ',';
        first = false;
        std::cout << "{\"library\":\"" << escape_json(dependency.library) << "\",\"status\":\""
                  << dependency.status << "\",\"required_by\":";
        print_strings(dependency.required_by);
        std::cout << ",\"symbols\":";
        print_strings(dependency.symbols);
        std::cout << ",\"candidates\":";
        print_strings(dependency.candidates);
        std::cout << '}';
    }
    std::cout << "]}\n";
}

bool positive_integer(const auto* text, std::uint64_t& value) {
    std::string ascii;
    for (; *text; ++text) {
        if (*text < '0' || *text > '9') return false;
        ascii += static_cast<char>(*text);
    }
    if (ascii.empty()) return false;
    const auto result = std::from_chars(ascii.data(), ascii.data() + ascii.size(), value);
    return result.ec == std::errc{} && result.ptr == ascii.data() + ascii.size() && value > 0;
}

bool argument_is(const auto* text, std::string_view expected) {
    for (const auto c : expected) {
        if (*text == 0 || *text++ != c) return false;
    }
    return *text == 0;
}

int run(int argc, const auto* const* argv) {
    if (argc == 2 && (argument_is(argv[1], "--help") || argument_is(argv[1], "-h"))) {
        help(); return 0;
    }
    if ((argc == 3 || argc == 4) && argument_is(argv[1], "inspect-xvd-header")) {
        std::uint64_t declared_size = 0;
        const bool json = argc == 4;
        if (!positive_integer(argv[2], declared_size) || (json && !argument_is(argv[3], "--json"))) {
            help(); return 2;
        }
#ifdef _WIN32
        if (_setmode(_fileno(stdin), _O_BINARY) == -1) {
            std::cerr << "Cannot set binary stdin mode\n"; return 1;
        }
#endif
        std::array<std::uint8_t, xboxone::inspection::xvd_probe_size> prefix{};
        std::cin.read(reinterpret_cast<char*>(prefix.data()), static_cast<std::streamsize>(prefix.size()));
        xboxone::inspection::Report report;
        std::string error;
        if (std::cin.gcount() != static_cast<std::streamsize>(prefix.size()) ||
            !xboxone::inspection::inspect_xvd_prefix(prefix, declared_size, report, error)) {
            std::cerr << "XVD prefix inspection failed: " << (error.empty() ? "truncated stdin" : error) << '\n';
            return 1;
        }
        print_report(report, json);
        return 0;
    }
    if (argc >= 3 && argument_is(argv[1], "inventory")) {
        bool json = false, file_limit_seen = false, total_limit_seen = false;
        xboxone::inventory::Limits limits;
        for (int i = 3; i < argc; ++i) {
            if (argument_is(argv[i], "--json") && !json) { json = true; continue; }
            const bool file_limit = argument_is(argv[i], "--max-file-bytes");
            const bool total_limit = argument_is(argv[i], "--max-total-bytes");
            if ((!file_limit && !total_limit) || i + 1 >= argc ||
                (file_limit && file_limit_seen) || (total_limit && total_limit_seen)) {
                help(); return 2;
            }
            auto& limit = file_limit ? limits.max_file_bytes : limits.max_total_bytes;
            if (!positive_integer(argv[++i], limit)) { help(); return 2; }
            (file_limit ? file_limit_seen : total_limit_seen) = true;
        }
        xboxone::inventory::Report report;
        std::string error;
        if (!xboxone::inventory::scan(std::filesystem::path(argv[2]), report, error, limits)) {
            std::cerr << "Inventory failed: " << error << '\n'; return 1;
        }
        print_inventory(report, json);
        return report.complete ? 0 : 1;
    }
    if (argc != 3 && argc != 4) { help(); return 2; }
    if (!argument_is(argv[1], "inspect")) { help(); return 2; }
    const bool json = argc == 4;
    if (json && !argument_is(argv[3], "--json")) { help(); return 2; }
    xboxone::inspection::Report report;
    std::string error;
    if (!xboxone::inspection::inspect(std::filesystem::path(argv[2]), report, error)) {
        std::cerr << "Inspection failed: " << error << "\n";
        return 1;
    }
    print_report(report, json);
    return 0;
}
} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) { return run(argc, argv); }
#else
int main(int argc, char** argv) { return run(argc, argv); }
#endif
