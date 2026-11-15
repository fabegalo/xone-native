// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace xboxone::appmodel {
struct Application {
    std::string id, executable, entry_point, aumid;
    // Presentation metadata only; may be "ms-resource:" references or empty.
    std::string display_name, logo, background_color;
};
struct PackageDependency {
    std::string name, publisher;
    std::uint64_t min_version = 0;
};
struct Capability { std::string name, namespace_uri; };
struct Manifest {
    std::string name, publisher, resource_id, full_name, family_name;
    std::string display_name, publisher_display_name; // presentation metadata only
    std::uint64_t version = 0;
    std::uint32_t architecture = 0;
    std::vector<Application> applications;
    std::vector<PackageDependency> dependencies;
    std::vector<Capability> capabilities;
    std::vector<std::string> device_families, resource_paths, unsupported_contracts;
};
// Windows SDK adapter. No registration, deployment, extraction or network I/O.
// Parses a <=1 MiB immutable snapshot matching the supplied SHA-256; rejects
// reparse ancestors, DTDs and external entities. Other hosts fail explicitly.
bool read_manifest(const std::filesystem::path&, const std::string& sha256,
                   Manifest&, std::string& error);
// Portable Windows-relative path policy, also used before touching any input.
bool package_relative_path(const std::string&, std::filesystem::path&);
} // namespace xboxone::appmodel
