// SPDX-License-Identifier: BSD-3-Clause
#include "xboxone_native/inspection.h"
#include "file_path.h"

#include <algorithm>
#include <array>
#include <fstream>
#include <limits>
#include <optional>
#include <ostream>
#include <set>
#include <span>
#include <string_view>
#include <vector>

namespace xboxone::inspection {
namespace {

std::uint16_t u16(const std::uint8_t* p) {
    return static_cast<std::uint16_t>(p[0]) |
           static_cast<std::uint16_t>(static_cast<std::uint16_t>(p[1]) << 8);
}
std::uint32_t u32(const std::uint8_t* p) {
    return static_cast<std::uint32_t>(u16(p)) |
           (static_cast<std::uint32_t>(u16(p + 2)) << 16);
}
std::uint64_t u64(const std::uint8_t* p) {
    return static_cast<std::uint64_t>(u32(p)) |
           (static_cast<std::uint64_t>(u32(p + 4)) << 32);
}

class Reader {
public:
    explicit Reader(const std::filesystem::path& path) : file_(detail::file_path(path), std::ios::binary | std::ios::ate) {
        if (file_) {
            const auto end = file_.tellg();
            if (end >= 0) {
                size_ = static_cast<std::uint64_t>(end);
                valid_ = true;
            }
        }
    }
    bool valid() const { return valid_; }
    std::uint64_t size() const { return size_; }

    bool read(std::uint64_t at, std::span<std::uint8_t> out) {
        if (!valid_ || at > size_ || out.size() > size_ - at ||
            at > static_cast<std::uint64_t>(std::numeric_limits<std::streamoff>::max())) {
            return false;
        }
        file_.clear();
        file_.seekg(static_cast<std::streamoff>(at), std::ios::beg);
        if (!file_) return false;
        file_.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
        return file_.gcount() == static_cast<std::streamsize>(out.size());
    }

    std::optional<std::string> cstring(std::uint64_t at, std::size_t max_len = 256) {
        if (at >= size_) return std::nullopt;
        const auto available = static_cast<std::size_t>(std::min<std::uint64_t>(max_len, size_ - at));
        std::vector<std::uint8_t> bytes(available);
        if (!read(at, bytes)) return std::nullopt;
        const auto stop = std::find(bytes.begin(), bytes.end(), 0);
        if (stop == bytes.end()) return std::nullopt;
        return std::string(bytes.begin(), stop);
    }

private:
    std::ifstream file_;
    std::uint64_t size_ = 0;
    bool valid_ = false;
};

struct Section {
    std::uint32_t virtual_address;
    std::uint32_t virtual_size;
    std::uint32_t raw_offset;
    std::uint32_t raw_size;
    std::uint32_t characteristics;
};

class PeReader {
public:
    Reader& file;
    std::uint32_t headers;
    const std::vector<Section>& sections;

    std::optional<std::uint64_t> locate(std::uint64_t rva, std::uint64_t count) const {
        if (!rva || rva > UINT32_MAX || count > (1ULL << 32) - rva) return std::nullopt;
        if (rva < headers && count <= headers - rva && rva + count <= file.size()) return rva;
        for (const auto& s : sections) {
            if (rva < s.virtual_address) continue;
            const auto delta = rva - s.virtual_address;
            if (delta >= s.raw_size || count > s.raw_size - delta) continue;
            const auto at = static_cast<std::uint64_t>(s.raw_offset) + delta;
            if (at <= file.size() && count <= file.size() - at) return at;
        }
        return std::nullopt;
    }
    bool read(std::uint64_t rva, std::span<std::uint8_t> out) const {
        const auto at = locate(rva, out.size());
        return at && file.read(*at, out);
    }
    std::optional<std::string> string(std::uint64_t rva, std::uint64_t limit = 256) const {
        std::uint64_t available = rva < headers ? headers - rva : 0ULL;
        for (const auto& s : sections)
            if (rva >= s.virtual_address && rva - s.virtual_address < s.raw_size)
                available = s.raw_size - (rva - s.virtual_address);
        const auto count = static_cast<std::size_t>(std::min({limit, available, std::uint64_t{256}}));
        std::array<std::uint8_t, 256> bytes{};
        if (!count || !read(rva, std::span(bytes).first(count))) return std::nullopt;
        const auto end = bytes.begin() + count;
        const auto zero = std::find(bytes.begin(), end, 0);
        if (zero != end && std::all_of(bytes.begin(), zero, [](auto b) { return b >= 0x21 && b <= 0x7e; }))
            return std::string(bytes.begin(), zero);
        return std::nullopt;
    }
};

bool library_name(std::string_view name) {
    return !name.empty() && name != "." && name != ".." &&
        std::all_of(name.begin(), name.end(), [](char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
        });
}

bool forward_target(Export& e) {
    const auto dot = e.forwarder.rfind('.');
    if (dot == std::string::npos || dot + 1 == e.forwarder.size()) return false;
    e.forward_library = e.forwarder.substr(0, dot);
    e.forward_symbol = e.forwarder.substr(dot + 1);
    if (!library_name(e.forward_library)) return false;
    // The PE convention omits .dll; add only the extension, never find a file.
    if (e.forward_library.find('.') == std::string::npos) e.forward_library += ".dll";
    if (e.forward_symbol.front() != '#') return true;
    unsigned ordinal = 0;
    if (e.forward_symbol.size() < 2 || e.forward_symbol.size() > 6) return false;
    for (char c : std::string_view(e.forward_symbol).substr(1)) {
        if (c < '0' || c > '9') return false;
        ordinal = ordinal * 10 + static_cast<unsigned>(c - '0');
    }
    if (ordinal > 65535) return false;
    e.forward_symbol = "#" + std::to_string(ordinal);
    return true;
}

bool parse_pe(Reader& file, Report& report, std::string& error) {
    std::array<std::uint8_t, 64> dos{};
    if (!file.read(0, dos) || dos[0] != 'M' || dos[1] != 'Z') {
        error = "invalid DOS/PE header";
        return false;
    }
    const std::uint64_t pe_offset = u32(dos.data() + 0x3c);
    std::array<std::uint8_t, 24> coff{};
    if (!file.read(pe_offset, coff) || coff[0] != 'P' || coff[1] != 'E' || coff[2] || coff[3]) {
        error = "invalid PE signature or truncated COFF header";
        return false;
    }
    const auto machine = u16(coff.data() + 4);
    const auto nsections = u16(coff.data() + 6);
    const auto optional_size = u16(coff.data() + 20);
    if (machine != 0x8664 || nsections == 0 || nsections > 96 || optional_size < 0x70) {
        error = "unsupported PE machine, section count, or optional header";
        return false;
    }
    if (pe_offset > std::numeric_limits<std::uint64_t>::max() - 24) {
        error = "PE offset overflow";
        return false;
    }
    const auto optional_offset = pe_offset + 24;
    std::vector<std::uint8_t> optional(optional_size);
    if (!file.read(optional_offset, optional) || u16(optional.data()) != 0x20b) {
        error = "not a complete PE32+ image";
        return false;
    }
    if (optional_offset > std::numeric_limits<std::uint64_t>::max() - optional_size) {
        error = "optional header offset overflow";
        return false;
    }
    const auto section_offset = optional_offset + optional_size;
    std::vector<Section> sections;
    sections.reserve(nsections);
    for (std::uint32_t i = 0; i < nsections; ++i) {
        std::array<std::uint8_t, 40> sec{};
        if (!file.read(section_offset + i * 40, sec)) {
            error = "truncated section table";
            return false;
        }
        sections.push_back({u32(sec.data() + 12), u32(sec.data() + 8),
                            u32(sec.data() + 20), u32(sec.data() + 16), u32(sec.data() + 36)});
    }
    report.kind = Kind::pe64;
    report.pe.machine = machine;
    report.pe.characteristics = u16(coff.data() + 22);
    report.pe.sections = nsections;
    report.pe.subsystem = u16(optional.data() + 0x44);
    report.pe.image_base = u64(optional.data() + 0x18);
    report.pe.entry_point_rva = u32(optional.data() + 0x10);
    const auto directories = u32(optional.data() + 0x6c);
    if (directories > (optional_size - 0x70U) / 8U) {
        error = "data directories exceed optional header";
        return false;
    }
    const auto fail = [&](const char* reason) { error = reason; return false; };
    const auto headers = u32(optional.data() + 0x3c);
    if (headers < section_offset + nsections * 40ULL || headers > file.size()) return fail("invalid SizeOfHeaders");
    for (std::size_t i = 0; i < sections.size(); ++i) {
        const auto& s = sections[i];
        const auto extent = std::max(s.virtual_size, s.raw_size);
        if (static_cast<std::uint64_t>(s.virtual_address) + extent > (1ULL << 32) ||
            (extent && s.virtual_address < headers) ||
            (s.raw_size && (s.raw_offset < headers || static_cast<std::uint64_t>(s.raw_offset) + s.raw_size > file.size())))
            return fail("invalid section range");
        for (std::size_t j = 0; j < i; ++j) {
            const auto& t = sections[j];
            const auto te = std::max(t.virtual_size, t.raw_size);
            if ((extent && te && s.virtual_address < static_cast<std::uint64_t>(t.virtual_address) + te &&
                 t.virtual_address < static_cast<std::uint64_t>(s.virtual_address) + extent) ||
                (s.raw_size && t.raw_size && s.raw_offset < static_cast<std::uint64_t>(t.raw_offset) + t.raw_size &&
                 t.raw_offset < static_cast<std::uint64_t>(s.raw_offset) + s.raw_size)) return fail("ambiguous overlapping sections");
        }
    }
    PeReader view{file, headers, sections};
    const auto directory = [&](unsigned index) -> std::pair<std::uint32_t, std::uint32_t> {
        if (directories <= index) return {};
        return {u32(optional.data() + 0x70 + index * 8), u32(optional.data() + 0x74 + index * 8)};
    };
    for (unsigned index : {0U, 1U, 2U, 9U, 13U, 14U}) {
        const auto [rva, size] = directory(index);
        if ((rva == 0) != (size == 0) || (rva && !view.locate(rva, size))) return fail("invalid or unbacked data directory range");
    }
    report.pe.has_delay_imports = directory(13).first != 0;
    report.pe.has_clr_header = directory(14).first != 0;
    for (unsigned index : {9U, 14U}) {
        const auto [rva, size] = directory(index);
        if (rva) report.pe.activation_metadata.push_back({index == 9 ? "tls_directory" : "clr_directory", {}, rva, size, 0, {}});
    }
    const auto [tls_rva, tls_size] = directory(9);
    if (tls_rva) {
        std::array<std::uint8_t, 40> data{};
        if (tls_size < data.size() || !view.read(tls_rva, data)) return fail("truncated PE32+ TLS directory");
        const auto to_rva = [&](std::uint64_t va) -> std::optional<std::uint32_t> {
            if (va < report.pe.image_base || va - report.pe.image_base > UINT32_MAX) return std::nullopt;
            return static_cast<std::uint32_t>(va - report.pe.image_base);
        };
        const auto section_range = [&](std::uint32_t rva, std::uint32_t count, std::uint32_t permission) {
            return std::any_of(sections.begin(), sections.end(), [&](const auto& s) {
                const auto extent = std::max(s.virtual_size, s.raw_size);
                return (s.characteristics & permission) && rva >= s.virtual_address &&
                    rva - s.virtual_address < extent && count <= extent - (rva - s.virtual_address);
            });
        };
        TlsInfo tls;
        const auto start = u64(data.data()), end = u64(data.data() + 8);
        if (end < start || (start == 0) != (end == 0) || end - start > UINT32_MAX)
            return fail("invalid TLS template VA range");
        if (start) {
            const auto rva = to_rva(start);
            if (!rva || (end != start && !view.locate(*rva, end - start))) return fail("unbacked TLS template");
            tls.template_rva = *rva;
            tls.template_size = static_cast<std::uint32_t>(end - start);
        }
        const auto index = to_rva(u64(data.data() + 16));
        if (!index || !section_range(*index, 4, 0x80000000U)) return fail("TLS index must address writable image storage");
        tls.index_rva = *index;
        tls.zero_fill_size = u32(data.data() + 32);
        const auto alignment = (u32(data.data() + 36) >> 20) & 15;
        if (alignment == 15) return fail("invalid TLS alignment");
        tls.alignment_bytes = alignment ? 1U << (alignment - 1) : 0;
        const auto callbacks = u64(data.data() + 24);
        if (callbacks) {
            const auto table = to_rva(callbacks);
            if (!table) return fail("invalid TLS callback table VA");
            tls.callbacks_rva = *table;
            for (std::uint32_t i = 0; ; ++i) {
                std::array<std::uint8_t, 8> entry{};
                if (i == 1024 || !view.read(static_cast<std::uint64_t>(*table) + i * 8ULL, entry))
                    return fail("unterminated or oversized TLS callback table");
                const auto va = u64(entry.data());
                if (!va) break;
                const auto target = to_rva(va);
                if (!target || !view.locate(*target, 1) || !section_range(*target, 1, 0x20000000U))
                    return fail("TLS callback must address executable image data");
                tls.callback_rvas.push_back(*target);
            }
        }
        report.pe.tls = std::move(tls);
    }
    std::size_t symbol_count = 0;
    const auto thunks = [&](Import& imported, std::uint64_t table, std::uint64_t iat, bool va) {
        if (!table || !iat) return false;
        for (std::uint32_t t = 0; t < 8192; ++t) {
            std::array<std::uint8_t, 8> value{};
            if (!view.read(table + t * 8ULL, value) || !view.locate(iat + t * 8ULL, 8)) return false;
            const auto thunk = u64(value.data());
            if (!thunk) return true;
            if (++symbol_count > 65536) return false;
            if (thunk & (1ULL << 63)) {
                if (thunk & 0x7fffffffffff0000ULL) return false;
                imported.ordinal_indices.push_back(t);
                imported.functions.push_back("#" + std::to_string(thunk & 0xffff));
            } else {
                if (va && thunk < report.pe.image_base) return false;
                const auto name = va ? thunk - report.pe.image_base : thunk;
                if (!view.locate(name, 3)) return false;
                const auto function = view.string(name + 2);
                if (!function || function->empty()) return false;
                imported.functions.push_back(*function);
            }
        }
        return false;
    };
    for (bool delay : {false, true}) {
        const auto [rva, size] = directory(delay ? 13 : 1);
        if (!rva) continue;
        const unsigned width = delay ? 32 : 20;
        if (size > width * 1024 || size < width) return fail("import descriptor table exceeds bounds");
        bool terminated = false;
        for (std::uint32_t pos = 0; pos + width <= size; pos += width) {
            std::array<std::uint8_t, 32> descriptor{};
            if (!view.read(static_cast<std::uint64_t>(rva) + pos, std::span(descriptor).first(width))) return fail("invalid import descriptor range");
            if (std::all_of(descriptor.begin(), descriptor.end(), [](auto b) { return !b; })) { terminated = true; break; }
            const auto attributes = delay ? u32(descriptor.data()) : 1;
            if (delay && attributes > 1) return fail("unsupported delay import attributes");
            const bool va = delay && !attributes;
            const auto address = [&](unsigned at) -> std::uint64_t {
                const auto value = u32(descriptor.data() + at);
                if (!va || !value) return value;
                return value >= report.pe.image_base ? value - report.pe.image_base : (1ULL << 32);
            };
            const auto name = view.string(address(delay ? 4 : 12));
            if (!name || !library_name(*name)) return fail("invalid imported library name");
            Import imported{*name, {}, delay, is_api_set(*name), {}};
            auto table = address(delay ? 16 : 0);
            const auto iat = address(delay ? 12 : 16);
            if (!table && !delay) table = iat;
            if (!thunks(imported, table, iat, va)) return fail("invalid or unterminated import thunk table");
            report.pe.imports.push_back(std::move(imported));
        }
        if (!terminated) return fail("unterminated import directory");
    }
    const auto [export_rva, export_size] = directory(0);
    if (export_rva) {
        std::array<std::uint8_t, 40> d{};
        if (export_size < d.size() || !view.read(export_rva, d)) return fail("truncated export directory");
        const auto base = u32(d.data() + 16), count = u32(d.data() + 20), names = u32(d.data() + 24);
        const auto eat = u32(d.data() + 28), name_table = u32(d.data() + 32), ordinals = u32(d.data() + 36);
        const auto dll_name = view.string(u32(d.data() + 12));
        if (!dll_name || !library_name(*dll_name)) return fail("invalid export library name");
        if (count > 65536 || names > 65536 || (count && base > UINT32_MAX - (count - 1)) ||
            (count && !view.locate(eat, count * 4ULL)) ||
            (names && (!view.locate(name_table, names * 4ULL) || !view.locate(ordinals, names * 2ULL)))) return fail("export table limit or range invalid");
        std::vector<Export> slots(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            std::array<std::uint8_t, 4> b{};
            if (!view.read(eat + i * 4ULL, b)) return fail("truncated export address table");
            auto& e = slots[i]; e.ordinal = base + i; e.rva = u32(b.data());
            if (!e.rva) continue;
            if (e.rva >= export_rva && e.rva - export_rva < export_size) {
                const auto f = view.string(e.rva, static_cast<std::uint64_t>(export_rva) + export_size - e.rva);
                if (!f || f->empty()) return fail("invalid forwarder string range");
                e.forwarder = *f;
                if (!forward_target(e)) return fail("invalid forwarded export target");
            } else if (e.rva >= headers && std::none_of(sections.begin(), sections.end(), [&](const auto& s) {
                return e.rva >= s.virtual_address && e.rva - s.virtual_address < std::max(s.virtual_size, s.raw_size);
            })) return fail("unmapped export RVA"); // Exported data may live in zero-filled BSS.
        }
        std::set<std::string> unique_names;
        for (std::uint32_t i = 0; i < names; ++i) {
            std::array<std::uint8_t, 4> n{}; std::array<std::uint8_t, 2> o{};
            if (!view.read(name_table + i * 4ULL, n) || !view.read(ordinals + i * 2ULL, o)) return fail("truncated export names");
            const auto slot = u16(o.data()); const auto name = view.string(u32(n.data()));
            if (slot >= count || !slots[slot].rva || !name || name->empty() || !unique_names.insert(*name).second) return fail("invalid or duplicate export name/ordinal");
            slots[slot].names.push_back(*name);
        }
        for (auto& e : slots) if (e.rva) {
            std::sort(e.names.begin(), e.names.end()); report.pe.exports.push_back(std::move(e));
        }
    }
    const auto [resource_rva, resource_size] = directory(2);
    if (resource_rva) {
        // Preserve bounded resource bytes. No XML/entity resolution or activation.
        std::set<std::uint32_t> visited;
        std::size_t entries = 0, payload_bytes = 0;
        const auto resource_read = [&](std::uint64_t offset, std::span<std::uint8_t> out) {
            return offset <= resource_size && out.size() <= resource_size - offset && view.read(static_cast<std::uint64_t>(resource_rva) + offset, out);
        };
        const auto visit = [&](auto&& self, std::uint32_t offset, std::vector<std::string> path, unsigned type) -> bool {
            if (path.size() > 2 || !visited.insert(offset).second) return false;
            std::array<std::uint8_t, 16> header{};
            if (!resource_read(offset, header)) return false;
            const auto count = static_cast<unsigned>(u16(header.data() + 12)) + u16(header.data() + 14);
            if (count > 4096 - entries) return false;
            entries += count;
            for (unsigned i = 0; i < count; ++i) {
                std::array<std::uint8_t, 8> entry{};
                if (!resource_read(offset + 16ULL + i * 8ULL, entry)) return false;
                const auto name = u32(entry.data()), target = u32(entry.data() + 4);
                auto next = path;
                if (name & 0x80000000U) {
                    std::array<std::uint8_t, 2> length{};
                    const auto at = name & 0x7fffffffU;
                    if (!resource_read(at, length) || u16(length.data()) > 256) return false;
                    std::vector<std::uint8_t> text(u16(length.data()) * 2);
                    if (!resource_read(at + 2ULL, text)) return false;
                    std::string label = "utf16le:";
                    constexpr char hex[] = "0123456789abcdef";
                    for (auto b : text) { label += hex[b >> 4]; label += hex[b & 15]; }
                    next.push_back(std::move(label));
                } else next.push_back("#" + std::to_string(name));
                const auto next_type = path.empty() ? name : type;
                if (target & 0x80000000U) {
                    if (!self(self, target & 0x7fffffffU, std::move(next), next_type)) return false;
                } else {
                    if (next.size() != 3) return false;
                    std::array<std::uint8_t, 16> data{};
                    if (!resource_read(target, data)) return false;
                    const auto rva = u32(data.data()), size = u32(data.data() + 4);
                    if (size && !view.locate(rva, size)) return false;
                    if (next_type != 24 && next_type != 16) continue;
                    if (size > 1024 * 1024 - payload_bytes) return false;
                    payload_bytes += size;
                    ActivationMetadata m{next_type == 24 ? "rt_manifest" : "rt_version", std::move(next), rva, size, u32(data.data() + 8), {}};
                    std::vector<std::uint8_t> bytes(size);
                    if (size && !view.read(rva, bytes)) return false;
                    constexpr char hex[] = "0123456789abcdef";
                    for (auto b : bytes) { m.bytes_hex += hex[b >> 4]; m.bytes_hex += hex[b & 15]; }
                    report.pe.activation_metadata.push_back(std::move(m));
                }
            }
            return true;
        };
        if (!visit(visit, 0, {}, 0)) return fail("invalid resource tree or activation metadata limit");
    }
    return true;
}

bool parse_xvd(Reader& file, Report& report, std::string& error) {
    // XVD has a 0x200-byte signature area followed by the 'msft-xvd' header.
    // This is identification ONLY; an authentic signature is NOT verified.
    std::array<std::uint8_t, xvd_probe_size> prefix{};
    if (!file.read(0, prefix)) {
        error = "truncated XVD metadata prefix (requires at least 0xE00 bytes)";
        return false;
    }
    return inspect_xvd_prefix(prefix, file.size(), report, error);
}

} // namespace

bool is_api_set(std::string_view library) {
    std::string name(library);
    for (auto& c : name) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    return name.starts_with("api-") || name.starts_with("ext-");
}

void write_pe_json_fields(std::ostream& out, const PeInfo& p) {
    const auto quote = [&](std::string_view value) {
        constexpr char hex[] = "0123456789abcdef";
        out << '"';
        for (unsigned char c : value) {
            if (c == '"' || c == '\\') out << '\\' << static_cast<char>(c);
            else if (c < 32) out << "\\u00" << hex[c >> 4] << hex[c & 15];
            else out << static_cast<char>(c);
        }
        out << '"';
    };
    const auto strings = [&](const auto& values) {
        out << '['; bool first = true;
        for (const auto& v : values) { if (!first) out << ','; first = false; quote(v); }
        out << ']';
    };
    out << "\"machine\":" << p.machine << ",\"subsystem\":" << p.subsystem
        << ",\"characteristics\":" << p.characteristics << ",\"section_count\":" << p.sections
        << ",\"entry_point_rva\":" << p.entry_point_rva << ",\"image_base\":" << p.image_base
        << ",\"has_delay_imports\":" << (p.has_delay_imports ? "true" : "false")
        << ",\"has_clr_header\":" << (p.has_clr_header ? "true" : "false") << ",\"imports\":[";
    bool first = true;
    for (const auto& i : p.imports) {
        if (!first) out << ',';
        first = false;
        out << "{\"library\":"; quote(i.library);
        out << ",\"delay_load\":" << (i.delay_load ? "true" : "false") << ",\"api_set\":" << (i.api_set ? "true" : "false");
        out << ",\"symbols\":"; strings(i.functions);
        out << ",\"ordinal_indices\":[";
        bool first_ordinal = true;
        for (const auto index : i.ordinal_indices) {
            if (!first_ordinal) out << ',';
            first_ordinal = false;
            out << index;
        }
        out << "]}";
    }
    out << "],\"exports\":["; first = true;
    for (const auto& e : p.exports) {
        if (!first) out << ',';
        first = false;
        out << "{\"ordinal\":" << e.ordinal << ",\"rva\":" << e.rva << ",\"names\":"; strings(e.names);
        out << ",\"forwarder\":"; quote(e.forwarder);
        out << ",\"forward_library\":"; quote(e.forward_library);
        out << ",\"forward_symbol\":"; quote(e.forward_symbol); out << '}';
    }
    out << "],\"activation_metadata\":["; first = true;
    for (const auto& m : p.activation_metadata) {
        if (!first) out << ',';
        first = false;
        out << "{\"kind\":"; quote(m.kind);
        out << ",\"resource_path\":"; strings(m.resource_path);
        out << ",\"rva\":" << m.rva << ",\"size\":" << m.size << ",\"code_page\":" << m.code_page << ",\"bytes_hex\":";
        quote(m.bytes_hex); out << '}';
    }
    out << "],\"tls\":";
    if (!p.tls) out << "null";
    else {
        const auto& t = *p.tls;
        out << "{\"template_rva\":" << t.template_rva << ",\"template_size\":" << t.template_size
            << ",\"index_rva\":" << t.index_rva << ",\"callbacks_rva\":" << t.callbacks_rva
            << ",\"zero_fill_size\":" << t.zero_fill_size << ",\"alignment_bytes\":" << t.alignment_bytes
            << ",\"callback_rvas\":[";
        for (std::size_t i = 0; i < t.callback_rvas.size(); ++i) {
            if (i) out << ',';
            out << t.callback_rvas[i];
        }
        out << "]}";
    }
}

bool inspect_xvd_prefix(std::span<const std::uint8_t> prefix, std::uint64_t declared_file_size,
                        Report& report, std::string& error) {
    report = {};
    error.clear();
    if (prefix.size() < xvd_probe_size || declared_file_size < prefix.size()) {
        error = "truncated XVD metadata prefix or inconsistent declared file size";
        return false;
    }
    const auto* header = prefix.data() + 0x200;
    if (std::string_view(reinterpret_cast<const char*>(header), 8) != "msft-xvd") {
        error = "invalid XVD magic";
        return false;
    }
    report.kind = Kind::xvd;
    report.file_size = declared_file_size;
    report.xvd.volume_flags = u32(header + 0x08);
    report.xvd.format_version = u32(header + 0x0c);
    report.xvd.created_filetime_raw = u64(header + 0x10);
    report.xvd.declared_drive_size = u64(header + 0x18);
    report.xvd.content_type = u32(header + 0x84);
    report.xvd.block_size = u32(header + 0x98);
    report.xvd.package_version_raw = u64(header + 0x1bc);
    report.xvd.required_system_version = u64(header + 0x294);
    return true;
}

bool inspect_chunk_catalog(const std::filesystem::path& path, std::vector<std::uint32_t>& chunks,
                           std::string& error) {
    chunks.clear();
    error.clear();
    Reader file(path);
    if (!file.valid()) { error = "cannot open chunk catalog"; return false; }
    constexpr std::uint64_t max_bytes = 16u << 20;
    if (!file.size() || file.size() > max_bytes || file.size() % 12) {
        error = "invalid chunk catalog size (12-byte records, maximum 16 MiB)";
        return false;
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(file.size()));
    if (!file.read(0, bytes)) { error = "cannot read chunk catalog"; return false; }
    std::vector<std::uint32_t> candidate;
    for (std::size_t offset = 8; offset < bytes.size(); offset += 12) {
        const auto id = u32(bytes.data() + offset);
        if (id < 0x3fffffffu) candidate.push_back(id);
    }
    std::sort(candidate.begin(), candidate.end());
    candidate.erase(std::unique(candidate.begin(), candidate.end()), candidate.end());
    chunks = std::move(candidate);
    return true;
}

bool inspect(const std::filesystem::path& path, Report& report, std::string& error) {
    error.clear();
    report = {};
    Reader file(path);
    if (!file.valid()) {
        error = "cannot open file";
        return false;
    }
    report.file_size = file.size();
    std::array<std::uint8_t, 2> prefix{};
    if (file.read(0, prefix) && prefix[0] == 'M' && prefix[1] == 'Z') {
        return parse_pe(file, report, error);
    }
    std::array<std::uint8_t, 8> magic{};
    if (file.read(0x200, magic) &&
        std::string_view(reinterpret_cast<const char*>(magic.data()), 8) == "msft-xvd") {
        return parse_xvd(file, report, error);
    }
    error = "unsupported input (expected PE32+ x86-64 or XVD header)";
    return false;
}

} // namespace xboxone::inspection
