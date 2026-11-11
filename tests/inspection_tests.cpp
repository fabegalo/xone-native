// SPDX-License-Identifier: BSD-3-Clause
#include "xboxone_native/inspection.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace {
void put16(std::vector<std::uint8_t>& b, std::size_t at, std::uint16_t v) {
    b[at] = static_cast<std::uint8_t>(v);
    b[at + 1] = static_cast<std::uint8_t>(v >> 8);
}
void put32(std::vector<std::uint8_t>& b, std::size_t at, std::uint32_t v) {
    put16(b, at, static_cast<std::uint16_t>(v));
    put16(b, at + 2, static_cast<std::uint16_t>(v >> 16));
}
void put64(std::vector<std::uint8_t>& b, std::size_t at, std::uint64_t v) {
    put32(b, at, static_cast<std::uint32_t>(v));
    put32(b, at + 4, static_cast<std::uint32_t>(v >> 32));
}

bool write_file(const fs::path& path, const std::vector<std::uint8_t>& b) {
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(b.data()), static_cast<std::streamsize>(b.size()));
    return static_cast<bool>(f);
}

std::vector<std::uint8_t> pe64() {
    std::vector<std::uint8_t> b(0x400);
    b[0] = 'M'; b[1] = 'Z';
    put32(b, 0x3c, 0x80);
    b[0x80] = 'P'; b[0x81] = 'E';
    put16(b, 0x84, 0x8664); // AMD64
    put16(b, 0x86, 1);      // one section
    put16(b, 0x94, 0xf0);   // PE32+ optional header size
    const std::size_t opt = 0x98;
    put16(b, opt, 0x20b);
    put32(b, opt + 0x10, 0x2000);
    put64(b, opt + 0x18, 0x140000000ULL);
    put32(b, opt + 0x3c, 0x200); // headers size
    put16(b, opt + 0x44, 3);     // console subsystem
    put32(b, opt + 0x6c, 16);
    put32(b, opt + 0x78, 0x2000); // import directory RVA
    put32(b, opt + 0x7c, 40);     // two descriptors including terminator
    const std::size_t section = opt + 0xf0;
    b[section] = '.'; b[section + 1] = 'r'; b[section + 2] = 'd';
    put32(b, section + 8, 0x200);  // virtual size
    put32(b, section + 12, 0x2000); // virtual address
    put32(b, section + 16, 0x200); // raw size
    put32(b, section + 20, 0x200); // raw address
    put32(b, 0x200, 0x2060);      // original thunk RVA
    put32(b, 0x20c, 0x2080);      // library name RVA
    put32(b, 0x210, 0x2060);      // first thunk RVA
    put64(b, 0x260, 0x20a0);      // hint/name RVA (second thunk is zero)
    const std::string dll = "example.dll";
    for (std::size_t i = 0; i < dll.size(); ++i) b[0x280 + i] = static_cast<std::uint8_t>(dll[i]);
    const std::string symbol = "Start";
    for (std::size_t i = 0; i < symbol.size(); ++i) b[0x2a2 + i] = static_cast<std::uint8_t>(symbol[i]);
    return b;
}

std::vector<std::uint8_t> xvd() {
    std::vector<std::uint8_t> b(0xe00);
    constexpr char magic[] = "msft-xvd";
    for (std::size_t i = 0; i < 8; ++i) b[0x200 + i] = static_cast<std::uint8_t>(magic[i]);
    put32(b, 0x20c, 3);        // format version
    put32(b, 0x208, 9);        // read-only, legacy sector size; encryption not disabled
    put64(b, 0x210, 123456789); // synthetic FILETIME
    put64(b, 0x218, 0x40000);  // declared drive size
    put32(b, 0x284, 1);        // content type
    put32(b, 0x298, 0xaa000);  // block size
    put64(b, 0x494, 10586);    // raw required system version
    put64(b, 0x3bc, 0x800a000065f425afULL); // synthetic opaque package version
    return b;
}

int failures = 0;
void check(bool condition, std::string message) {
    if (!condition) { std::cerr << "FAIL: " << message << '\n'; ++failures; }
}
} // namespace

int main() {
    const fs::path dir = fs::temp_directory_path() /
        ("xboxone_native_m0_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    fs::create_directories(dir);
    const auto pe = pe64();
    const auto x = xvd();
    check(write_file(dir / "sample.exe", pe), "write PE fixture");
    check(write_file(dir / "system.xvd", x), "write XVD fixture");
    xboxone::inspection::Report report;
    std::string err;
    check(xboxone::inspection::inspect(dir / "sample.exe", report, err), "valid synthetic PE32+ parses: " + err);
    check(report.kind == xboxone::inspection::Kind::pe64, "PE classification");
    check(report.pe.machine == 0x8664 && report.pe.sections == 1, "PE machine and sections");
    check(report.pe.imports.size() == 1, "PE import library count");
    if (report.pe.imports.size() == 1) {
        check(report.pe.imports[0].library == "example.dll", "PE import name");
        check(report.pe.imports[0].functions.size() == 1 &&
              report.pe.imports[0].functions[0] == "Start", "PE imported symbol");
    }
    check(xboxone::inspection::inspect(dir / "system.xvd", report, err), "valid synthetic XVD parses: " + err);
    check(report.kind == xboxone::inspection::Kind::xvd, "XVD classification");
    check(report.xvd.block_size == 0xaa000 && report.xvd.format_version == 3, "XVD metadata");
    check(report.xvd.required_system_version == 10586, "XVD required version field");
    check(report.xvd.volume_flags == 9 && report.xvd.created_filetime_raw == 123456789 &&
          report.xvd.package_version_raw == 0x800a000065f425afULL, "XVD provenance metadata");
    check(xboxone::inspection::inspect_xvd_prefix(x, 0x100000000ULL, report, err) &&
          report.file_size == 0x100000000ULL, "in-memory prefix preserves caller-declared 64-bit size");
    check(!xboxone::inspection::inspect_xvd_prefix(x, 100, report, err), "reject inconsistent archive size");
    check(!xboxone::inspection::inspect_xvd_prefix(std::span(x).first(0x400), 0x1000, report, err), "reject short archive prefix");
    {
        auto invalid = x;
        const std::string uppercase = "MSFT-XVD";
        for (std::size_t i = 0; i < uppercase.size(); ++i) invalid[0x200 + i] = static_cast<std::uint8_t>(uppercase[i]);
        write_file(dir / "uppercase.xvd", invalid);
        check(!xboxone::inspection::inspect(dir / "uppercase.xvd", report, err), "reject incorrect uppercase XVD magic");
    }
    {
        auto invalid = pe;
        put16(invalid, 0x84, 0x14c);
        write_file(dir / "x86.exe", invalid);
        check(!xboxone::inspection::inspect(dir / "x86.exe", report, err), "reject 32-bit machine");
    }
    {
        auto invalid = pe;
        put64(invalid, 0x260, 0x70000000ULL);
        write_file(dir / "bad_import.exe", invalid);
        check(!xboxone::inspection::inspect(dir / "bad_import.exe", report, err), "reject invalid import RVA");
    }
    {
        auto invalid = x;
        invalid.resize(0x400);
        write_file(dir / "truncated.xvd", invalid);
        check(!xboxone::inspection::inspect(dir / "truncated.xvd", report, err), "reject truncated XVD");
    }
    {
        std::vector<std::uint8_t> unknown = {1, 2, 3, 4};
        write_file(dir / "unknown.dat", unknown);
        check(!xboxone::inspection::inspect(dir / "unknown.dat", report, err), "reject unknown format");
    }
    {
        std::vector<std::uint8_t> catalog(6 * 12);
        const std::uint32_t ids[]{69, 7, 69, 0x3fffffff, 0xffffffff, 0x01020304};
        for (std::size_t i = 0; i < 6; ++i) {
            put64(catalog, i * 12, 0x123456789abcdef0ull + i);
            put32(catalog, i * 12 + 8, ids[i]);
        }
        check(write_file(dir / "chunks.manifest", catalog), "write chunk catalog fixture");
        std::vector<std::uint32_t> chunks;
        check(xboxone::inspection::inspect_chunk_catalog(dir / "chunks.manifest", chunks, err) &&
              chunks == std::vector<std::uint32_t>{7, 69, 0x01020304}, "little-endian chunk IDs sorted, unique and reserved IDs excluded");
        catalog.pop_back();
        check(write_file(dir / "truncated.manifest", catalog), "write truncated chunk catalog");
        check(!xboxone::inspection::inspect_chunk_catalog(dir / "truncated.manifest", chunks, err) && chunks.empty(),
              "partial record rejected and stale results cleared");
        check(write_file(dir / "empty.manifest", {}), "write empty chunk catalog");
        check(!xboxone::inspection::inspect_chunk_catalog(dir / "empty.manifest", chunks, err), "empty catalog rejected");
        check(!xboxone::inspection::inspect_chunk_catalog(dir / "absent.manifest", chunks, err), "missing catalog rejected");
        {
            std::ofstream out(dir / "oversized.manifest", std::ios::binary);
            out.seekp((16u << 20) + 7);
            out.put('\0'); // Whole records beyond the cap must still be rejected.
        }
        check(!xboxone::inspection::inspect_chunk_catalog(dir / "oversized.manifest", chunks, err), "oversized catalog rejected");
    }
    check(!xboxone::inspection::inspect(dir / "missing.xvd", report, err), "reject missing file");
    std::vector<std::uint8_t> after(x.size());
    {
        std::ifstream in(dir / "system.xvd", std::ios::binary);
        in.read(reinterpret_cast<char*>(after.data()), static_cast<std::streamsize>(after.size()));
    }
    check(after == x, "inspector leaves input unchanged");
    fs::remove_all(dir);
    if (failures) return 1;
    std::cout << "All M0 inspection regressions passed\n";
    return 0;
}
