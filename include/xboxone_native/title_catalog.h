// SPDX-License-Identifier: BSD-3-Clause
#pragma once
// Library import for the console state: identifies titles in place from their
// own package manifest (AppModel reader) and entry-executable SHA-256.
#include "xboxone_native/console_state.h"

#include <filesystem>
#include <string>
#include <vector>

namespace xboxone::catalog {

struct ScanResult {
    std::vector<console::Title> titles;
    std::vector<std::string> errors; // one line per rejected package/application
};

// Walks folder (no links/reparse points, bounded depth/entries) for AppxManifest.xml.
ScanResult scan(const std::filesystem::path& folder);
// Adds new titles and refreshes metadata/paths of known ones; returns the number added.
std::size_t merge(console::State& state, const ScanResult& result, std::int64_t now);
// Size/time check, re-hashing only when they changed. Fails if the content is gone or different.
bool verify(const console::Title& title, std::string& error);

} // namespace xboxone::catalog
