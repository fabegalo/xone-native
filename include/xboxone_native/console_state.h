// SPDX-License-Identifier: BSD-3-Clause
#pragma once
// One persistent console state shared by every system UI backend (the local
// dashboard now, original SystemOS later): local profiles, settings, the title
// library and the per-profile ERA storage roots that hold title saves.
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace xboxone::console {

struct Profile {
    std::string id;   // 32 lowercase hex: the 16 identity bytes ERA reads from its profile record
    std::string name; // UTF-8 display name
    std::uint32_t avatar = 0;
    std::int64_t created = 0, last_signin = 0; // Unix seconds
};

struct Settings {
    std::string language = "en"; // "en" or "pt-BR"
    std::uint32_t width = 1920, height = 1080;
    bool fullscreen = false;
    bool ui_sounds = true;
    std::uint32_t ui_volume = 70;      // percent
    bool vibration = true;
    std::uint32_t stick_deadzone = 40; // percent
    std::uint32_t time_limit_minutes = 0; // 0 = supervised without ERA deadline
    std::string last_profile;
    std::vector<std::string> library_folders; // UTF-8 absolute paths
};

enum class Readiness { unchecked, ready, blocked };

struct Title {
    std::string id;           // SHA-256 of the entry executable
    std::string name, publisher, package_name, version, application_id;
    std::string executable;   // UTF-8 absolute path, original content is never copied
    std::string logo, background_color, manifest_sha256;
    std::uint64_t executable_size = 0;
    std::int64_t executable_mtime = 0;
    std::int64_t added = 0, last_played = 0;
    std::uint32_t play_count = 0;
    bool pinned = false;
    Readiness readiness = Readiness::unchecked;
    std::uint32_t resolved = 0, unresolved = 0;
    std::string check_detail, last_result;
};

struct State {
    Settings settings;
    std::vector<Profile> profiles;
    std::vector<Title> titles;
};

// Missing file => defaults (true). A malformed file is moved aside, never
// overwritten, and reported in error while state receives defaults (true).
// false only when the file exists but cannot be read or preserved.
bool load(const std::filesystem::path& root, State& state, std::string& error);
// Atomic replace (temporary file + rename) of <root>/console-state.txt.
bool save(const std::filesystem::path& root, const State& state, std::string& error);

std::filesystem::path default_root();
std::string new_profile_id();
// <root>/users/<profile>/titles/<first 16 hex of title id>: the ERA --storage root.
std::filesystem::path title_storage(const std::filesystem::path& root, const Profile&, const Title&);
std::filesystem::path profile_storage(const std::filesystem::path& root, const Profile&);
// Creates the storage root and writes ERA's 32-byte local profile record so the
// title sees the same identity in every root of this profile.
bool prepare_title_storage(const std::filesystem::path& storage, const Profile&, std::string& error);

Profile* find_profile(State&, const std::string& id);
Title* find_title(State&, const std::string& id);

} // namespace xboxone::console
