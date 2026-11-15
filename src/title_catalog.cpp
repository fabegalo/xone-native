// SPDX-License-Identifier: BSD-3-Clause
#include "xboxone_native/title_catalog.h"
#include "xboxone_native/appmodel.h"
#include "xboxone_native/inspection.h"
#include "xboxone_native/inventory.h"
#include "file_path.h"

#include <algorithm>
#include <cwctype>
#include <initializer_list>
#include <string_view>
#include <system_error>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace xboxone::catalog {
namespace fs = std::filesystem;
namespace {
constexpr std::uint64_t max_executable_bytes = 16ULL << 30;
constexpr unsigned max_depth = 6;
constexpr std::size_t max_entries = 200000;

// Stored/displayed paths drop the \\?\ I/O prefix; detail::file_path re-adds it for I/O.
std::string utf8(const fs::path& path) {
    const auto u8 = path.u8string();
    std::string text(u8.begin(), u8.end());
    if (text.rfind("\\\\?\\UNC\\", 0) == 0) return "\\\\" + text.substr(8);
    if (text.rfind("\\\\?\\", 0) == 0) return text.substr(4);
    return text;
}

bool same_text(std::wstring_view a, std::wstring_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](wchar_t x, wchar_t y) {
        return std::towlower(static_cast<wint_t>(x)) == std::towlower(static_cast<wint_t>(y));
    });
}

bool resource_reference(const std::string& value) { return value.rfind("ms-resource:", 0) == 0; }

std::string first_plain(std::initializer_list<const std::string*> values) {
    for (const auto* v : values) if (!v->empty() && !resource_reference(*v)) return *v;
    return {};
}

std::string version_text(std::uint64_t v) {
    return std::to_string(v >> 48) + '.' + std::to_string((v >> 32) & 0xffff) + '.' +
           std::to_string((v >> 16) & 0xffff) + '.' + std::to_string(v & 0xffff);
}

std::int64_t mtime(const fs::path& path, std::error_code& ec) {
    return static_cast<std::int64_t>(fs::last_write_time(detail::file_path(path), ec).time_since_epoch().count());
}

// Package resources may carry qualifiers (Logo.scale-200.png); take the largest variant.
std::string resolve_logo(const fs::path& package, const std::string& reference) {
    fs::path relative;
    if (reference.empty() || !appmodel::package_relative_path(reference, relative)) return {};
    const auto exact = package / relative;
    std::error_code ec;
    if (fs::is_regular_file(detail::file_path(exact), ec)) return utf8(exact);
    const auto stem = relative.stem().wstring() + L".", extension = relative.extension().wstring();
    fs::path best;
    std::uintmax_t best_size = 0;
    for (fs::directory_iterator it(detail::file_path(exact.parent_path()), ec), end; !ec && it != end; it.increment(ec)) {
        const auto name = it->path().filename().wstring();
        if (!it->is_regular_file(ec) || name.size() <= stem.size() + extension.size() ||
            !same_text(std::wstring_view(name).substr(0, stem.size()), stem) ||
            !same_text(std::wstring_view(name).substr(name.size() - extension.size()), extension)) continue;
        const auto size = it->file_size(ec);
        if (!ec && size > best_size) { best_size = size; best = exact.parent_path() / it->path().filename(); }
    }
    return best.empty() ? std::string{} : utf8(best);
}

void identify(const fs::path& manifest_path, ScanResult& out) {
    const auto package = manifest_path.parent_path();
    std::string manifest_hash, error;
    appmodel::Manifest manifest;
    if (!inventory::fingerprint(manifest_path, manifest_hash, error, 1u << 20) ||
        !appmodel::read_manifest(manifest_path, manifest_hash, manifest, error)) {
        out.errors.push_back(utf8(manifest_path) + ": " + error);
        return;
    }
    if (manifest.architecture != 9) { // APPX_PACKAGE_ARCHITECTURE_X64: the only ERA execution target
        out.errors.push_back(utf8(manifest_path) + ": package is not x64");
        return;
    }
    for (const auto& app : manifest.applications) {
        fs::path relative;
        if (!appmodel::package_relative_path(app.executable, relative)) {
            out.errors.push_back(utf8(manifest_path) + ": unsafe Executable " + app.executable);
            continue;
        }
        const auto executable = package / relative;
        console::Title t;
        std::error_code ec;
        inspection::Report report;
        if (!inventory::fingerprint(executable, t.id, error, max_executable_bytes)) {
            out.errors.push_back(utf8(executable) + ": " + error);
            continue;
        }
        if (!inspection::inspect(detail::file_path(executable), report, error) || report.kind != inspection::Kind::pe64) {
            out.errors.push_back(utf8(executable) + ": not a PE32+ x86-64 image" + (error.empty() ? "" : " (" + error + ")"));
            continue;
        }
        t.executable = utf8(fs::absolute(executable));
        t.executable_size = fs::file_size(detail::file_path(executable), ec);
        t.executable_mtime = mtime(executable, ec);
        t.name = first_plain({&manifest.display_name, &app.display_name});
        if (t.name.empty()) t.name = manifest.name; // shortcut: ms-resource names need resources.pri; identity name shown until then
        t.publisher = first_plain({&manifest.publisher_display_name});
        if (t.publisher.empty()) t.publisher = manifest.publisher;
        t.package_name = manifest.name;
        t.version = version_text(manifest.version);
        t.application_id = app.id;
        t.logo = resolve_logo(package, app.logo);
        if (t.logo.empty() && !manifest.resource_paths.empty()) t.logo = resolve_logo(package, manifest.resource_paths.front());
        t.background_color = app.background_color;
        t.manifest_sha256 = manifest_hash;
        out.titles.push_back(std::move(t));
    }
}
} // namespace

ScanResult scan(const fs::path& folder) {
    ScanResult out;
    std::error_code ec;
    const auto root = detail::file_path(fs::absolute(folder, ec));
    if (ec || !fs::is_directory(root, ec)) { out.errors.push_back(utf8(folder) + ": not a readable folder"); return out; }
    std::size_t entries = 0;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
    for (; !ec && it != end; it.increment(ec)) {
        if (++entries > max_entries) { out.errors.push_back(utf8(folder) + ": entry limit reached, scan incomplete"); break; }
        const auto status = it->symlink_status(ec);
        if (ec) break;
        if (fs::is_symlink(status) || (it->is_directory(ec) && it.depth() >= static_cast<int>(max_depth))) {
            it.disable_recursion_pending();
            continue;
        }
#ifdef _WIN32
        if (it->is_directory(ec)) {
            // Junctions are not symlinks to std::filesystem; never follow them out of the library.
            const DWORD attributes = GetFileAttributesW(it->path().c_str());
            if (attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_REPARSE_POINT)) it.disable_recursion_pending();
            continue;
        }
#endif
        if (fs::is_regular_file(status) && same_text(it->path().filename().wstring(), L"AppxManifest.xml"))
            identify(it->path(), out);
    }
    if (ec) out.errors.push_back(utf8(folder) + ": " + ec.message());
    return out;
}

std::size_t merge(console::State& state, const ScanResult& result, std::int64_t now) {
    std::size_t added = 0;
    for (const auto& found : result.titles) {
        if (auto* known = console::find_title(state, found.id)) {
            auto refreshed = found;
            refreshed.added = known->added; refreshed.last_played = known->last_played; refreshed.play_count = known->play_count;
            refreshed.pinned = known->pinned; refreshed.readiness = known->readiness; refreshed.resolved = known->resolved;
            refreshed.unresolved = known->unresolved; refreshed.check_detail = known->check_detail;
            refreshed.last_result = known->last_result;
            *known = std::move(refreshed);
            continue;
        }
        auto title = found;
        title.added = now;
        state.titles.push_back(std::move(title));
        ++added;
    }
    return added;
}

bool verify(const console::Title& title, std::string& error) {
    const auto path = fs::path(std::u8string(title.executable.begin(), title.executable.end()));
    std::error_code ec;
    const auto size = fs::file_size(detail::file_path(path), ec);
    if (ec) { error = "title executable is not available: " + ec.message(); return false; }
    const auto time = mtime(path, ec);
    if (!ec && size == title.executable_size && time == title.executable_mtime) return true;
    std::string hash;
    if (!inventory::fingerprint(path, hash, error, max_executable_bytes)) return false;
    if (hash != title.id) { error = "title executable changed since import; rescan the library"; return false; }
    return true;
}

} // namespace xboxone::catalog
