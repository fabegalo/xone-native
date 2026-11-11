// SPDX-License-Identifier: BSD-3-Clause
#pragma once

#include <cstdint>
#include <filesystem>
#include <iosfwd>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace xboxone::inspection {

enum class Kind { pe64, xvd };

struct Import {
    std::string library;
    std::vector<std::string> functions;
    bool delay_load = false;
    bool api_set = false; // Contract name, never an inferred host DLL mapping.
    std::vector<std::uint32_t> ordinal_indices; // Distinguish ordinal #7 from a literal named "#7".
};

struct Export {
    std::uint32_t ordinal = 0;
    std::uint32_t rva = 0;
    std::vector<std::string> names; // Aliases of the same EAT slot.
    std::string forwarder;
    std::string forward_library;
    std::string forward_symbol;
};

struct ActivationMetadata {
    std::string kind; // rt_manifest, rt_version, clr_directory, tls_directory.
    std::vector<std::string> resource_path; // Resource type/name/language.
    std::uint32_t rva = 0;
    std::uint32_t size = 0;
    std::uint32_t code_page = 0;
    std::string bytes_hex; // Exact bounded resource bytes; no XML/CLR activation.
};

struct TlsInfo {
    std::uint32_t template_rva = 0;
    std::uint32_t template_size = 0;
    std::uint32_t index_rva = 0;
    std::uint32_t callbacks_rva = 0;
    std::uint32_t zero_fill_size = 0;
    std::uint32_t alignment_bytes = 0; // Zero selects the loader's default.
    std::vector<std::uint32_t> callback_rvas; // In loader invocation order.
};

struct PeInfo {
    std::uint16_t machine = 0;
    std::uint16_t characteristics = 0;
    std::uint16_t subsystem = 0;
    std::uint16_t sections = 0;
    std::uint64_t image_base = 0;
    std::uint32_t entry_point_rva = 0;
    bool has_delay_imports = false;
    bool has_clr_header = false;
    std::vector<Import> imports;
    std::vector<Export> exports;
    std::vector<ActivationMetadata> activation_metadata;
    std::optional<TlsInfo> tls;
};

bool is_api_set(std::string_view library);
// Shared deterministic serialization for inspect and version-bound bootstrap.
void write_pe_json_fields(std::ostream& out, const PeInfo& pe);

struct XvdInfo {
    std::uint32_t volume_flags = 0;
    std::uint32_t format_version = 0;
    std::uint32_t content_type = 0;
    std::uint32_t block_size = 0;
    std::uint64_t declared_drive_size = 0;
    std::uint64_t required_system_version = 0;
    std::uint64_t package_version_raw = 0;
    std::uint64_t created_filetime_raw = 0;
};

struct Report {
    Kind kind = Kind::pe64;
    std::uint64_t file_size = 0;
    PeInfo pe;
    XvdInfo xvd;
};

// No execution, decryption, signature validation, or writes to the input.
// false means error contains a specific reason; report is not trustworthy then.
bool inspect(const std::filesystem::path& path, Report& report, std::string& error);

// Local chunk-index format: repeated little-endian 64-bit file key + 32-bit
// chunk ID. Reserved IDs >= 0x3fffffff are excluded. Not content authentication.
bool inspect_chunk_catalog(const std::filesystem::path& path, std::vector<std::uint32_t>& chunks,
                           std::string& error);

// Archive adapters can supply this bounded prefix without extracting the XVD.
// This probes metadata only, not the entire signed header or the payload. The
// original size is declared by the caller and has not been independently read.
inline constexpr std::size_t xvd_probe_size = 0xe00;
bool inspect_xvd_prefix(std::span<const std::uint8_t> prefix, std::uint64_t declared_file_size,
                        Report& report, std::string& error);

} // namespace xboxone::inspection
