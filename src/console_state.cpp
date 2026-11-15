// SPDX-License-Identifier: BSD-3-Clause
#include "xboxone_native/console_state.h"
#include "file_path.h"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>
#include <system_error>
#include <type_traits>

namespace xboxone::console {
namespace {
constexpr char header[] = "xone-console-state 1";
constexpr char state_file[] = "console-state.txt";

std::string escape(const std::string& value) {
    std::string out;
    for (const char c : value) {
        if (c == '\\') out += "\\\\";
        else if (c == '\t') out += "\\t";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else out += c;
    }
    return out;
}

bool unescape(const std::string& value, std::string& out) {
    out.clear();
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (value[i] != '\\') { out += value[i]; continue; }
        if (++i == value.size()) return false;
        switch (value[i]) {
            case '\\': out += '\\'; break;
            case 't': out += '\t'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            default: return false;
        }
    }
    return true;
}

bool split(const std::string& line, std::vector<std::string>& fields) {
    fields.clear();
    std::size_t start = 0;
    for (;;) {
        const auto end = line.find('\t', start);
        std::string field;
        if (!unescape(line.substr(start, end == std::string::npos ? std::string::npos : end - start), field)) return false;
        fields.push_back(std::move(field));
        if (end == std::string::npos) return true;
        start = end + 1;
    }
}

template<class T> bool number(const std::string& text, T& value) {
    if (text.empty() || text.size() > 20) return false;
    char* end = nullptr;
    errno = 0;
    if constexpr (std::is_signed_v<T>) {
        const long long parsed = std::strtoll(text.c_str(), &end, 10);
        value = static_cast<T>(parsed);
    } else {
        if (text[0] == '-') return false;
        const unsigned long long parsed = std::strtoull(text.c_str(), &end, 10);
        if (parsed > static_cast<unsigned long long>(T(~T(0)))) return false;
        value = static_cast<T>(parsed);
    }
    return errno == 0 && end && *end == 0;
}

bool hex_id(const std::string& id, std::size_t length) {
    if (id.size() != length) return false;
    for (const char c : id) if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

const char* readiness_name(Readiness r) {
    return r == Readiness::ready ? "ready" : r == Readiness::blocked ? "blocked" : "unchecked";
}

bool parse(std::istream& in, State& state, std::string& error) {
    std::string line;
    if (!std::getline(in, line) || line != header) { error = "unknown console state header"; return false; }
    std::vector<std::string> f;
    unsigned number_of_line = 1;
    while (std::getline(in, line)) {
        ++number_of_line;
        if (line.empty()) continue;
        if (!split(line, f)) { error = "bad escape on line " + std::to_string(number_of_line); return false; }
        bool ok = true;
        if (f[0] == "setting" && f.size() == 3) {
            auto& s = state.settings;
            const auto& k = f[1];
            const auto& v = f[2];
            if (k == "language") s.language = v == "pt-BR" ? "pt-BR" : "en";
            else if (k == "width") ok = number(v, s.width) && s.width >= 640 && s.width <= 7680;
            else if (k == "height") ok = number(v, s.height) && s.height >= 360 && s.height <= 4320;
            else if (k == "fullscreen") s.fullscreen = v == "1";
            else if (k == "ui_sounds") s.ui_sounds = v == "1";
            else if (k == "ui_volume") ok = number(v, s.ui_volume) && s.ui_volume <= 100;
            else if (k == "vibration") s.vibration = v == "1";
            else if (k == "stick_deadzone") ok = number(v, s.stick_deadzone) && s.stick_deadzone <= 90;
            else if (k == "time_limit_minutes") ok = number(v, s.time_limit_minutes) && s.time_limit_minutes <= 10;
            else if (k == "last_profile") s.last_profile = v;
            // Unknown keys come from newer revisions; ignoring them keeps the file usable.
        } else if (f[0] == "folder" && f.size() == 2) {
            state.settings.library_folders.push_back(f[1]);
        } else if (f[0] == "profile" && f.size() == 6) {
            Profile p;
            p.id = f[1]; p.name = f[2];
            ok = hex_id(p.id, 32) && !p.name.empty() && number(f[3], p.avatar) && number(f[4], p.created) &&
                 number(f[5], p.last_signin);
            if (ok) state.profiles.push_back(std::move(p));
        } else if (f[0] == "title" && f.size() == 23) {
            Title t;
            t.id = f[1]; t.name = f[2]; t.publisher = f[3]; t.package_name = f[4]; t.version = f[5];
            t.application_id = f[6]; t.executable = f[7]; t.logo = f[8]; t.background_color = f[9];
            t.manifest_sha256 = f[10];
            ok = hex_id(t.id, 64) && !t.executable.empty() && number(f[11], t.executable_size) &&
                 number(f[12], t.executable_mtime) && number(f[13], t.added) && number(f[14], t.last_played) &&
                 number(f[15], t.play_count) && (f[16] == "0" || f[16] == "1") &&
                 number(f[18], t.resolved) && number(f[19], t.unresolved);
            t.pinned = f[16] == "1";
            t.readiness = f[17] == "ready" ? Readiness::ready : f[17] == "blocked" ? Readiness::blocked : Readiness::unchecked;
            t.check_detail = f[20]; t.last_result = f[21];
            ok = ok && f[22] == "end";
            if (ok) state.titles.push_back(std::move(t));
        } else ok = false;
        if (!ok) { error = "invalid record on line " + std::to_string(number_of_line); return false; }
    }
    if (in.bad()) { error = "console state read failed"; return false; }
    return true;
}
} // namespace

std::filesystem::path default_root() {
    if (const char* own = std::getenv("XONE_NATIVE_HOME"); own && *own)
        return std::filesystem::path(std::u8string(own, own + std::strlen(own)));
#ifdef _WIN32
    if (const wchar_t* local = _wgetenv(L"LOCALAPPDATA"); local && *local)
        return std::filesystem::path(local) / L"xone-native";
    return std::filesystem::current_path() / L"xone-native-data";
#else
    const char* home = std::getenv("HOME");
#ifdef __APPLE__
    if (home && *home) return std::filesystem::path(home) / "Library/Application Support/xone-native";
#else
    if (const char* data = std::getenv("XDG_DATA_HOME"); data && *data) return std::filesystem::path(data) / "xone-native";
    if (home && *home) return std::filesystem::path(home) / ".local/share/xone-native";
#endif
    return std::filesystem::current_path() / "xone-native-data";
#endif
}

bool load(const std::filesystem::path& root, State& state, std::string& error) {
    state = State{};
    error.clear();
    const auto path = detail::file_path(root / state_file);
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        if (ec) { error = "console state not accessible: " + ec.message(); return false; }
        return true;
    }
    std::ifstream in(path, std::ios::binary);
    if (!in) { error = "console state cannot be opened"; return false; }
    State parsed;
    std::string reason;
    if (parse(in, parsed, reason)) { state = std::move(parsed); return true; }
    in.close();
    // Preserve the unreadable file for recovery instead of overwriting it.
    const auto stamp = std::to_string(std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count());
    const auto aside = detail::file_path(root / (std::string(state_file) + ".corrupt-" + stamp));
    std::filesystem::rename(path, aside, ec);
    if (ec) { error = "console state is malformed (" + reason + ") and could not be preserved: " + ec.message(); return false; }
    error = "console state was malformed (" + reason + "); kept as " + aside.filename().string();
    return true;
}

bool save(const std::filesystem::path& root, const State& state, std::string& error) {
    std::ostringstream out;
    out << header << '\n';
    const auto& s = state.settings;
    const auto setting = [&](const char* key, const std::string& value) { out << "setting\t" << key << '\t' << escape(value) << '\n'; };
    setting("language", s.language);
    setting("width", std::to_string(s.width));
    setting("height", std::to_string(s.height));
    setting("fullscreen", s.fullscreen ? "1" : "0");
    setting("ui_sounds", s.ui_sounds ? "1" : "0");
    setting("ui_volume", std::to_string(s.ui_volume));
    setting("vibration", s.vibration ? "1" : "0");
    setting("stick_deadzone", std::to_string(s.stick_deadzone));
    setting("time_limit_minutes", std::to_string(s.time_limit_minutes));
    setting("last_profile", s.last_profile);
    for (const auto& folder : s.library_folders) out << "folder\t" << escape(folder) << '\n';
    for (const auto& p : state.profiles)
        out << "profile\t" << p.id << '\t' << escape(p.name) << '\t' << p.avatar << '\t' << p.created << '\t' << p.last_signin << '\n';
    for (const auto& t : state.titles) {
        out << "title\t" << t.id;
        for (const auto* v : {&t.name, &t.publisher, &t.package_name, &t.version, &t.application_id, &t.executable,
                              &t.logo, &t.background_color, &t.manifest_sha256})
            out << '\t' << escape(*v);
        out << '\t' << t.executable_size << '\t' << t.executable_mtime << '\t' << t.added << '\t' << t.last_played << '\t'
            << t.play_count << '\t' << (t.pinned ? 1 : 0) << '\t' << readiness_name(t.readiness) << '\t' << t.resolved
            << '\t' << t.unresolved << '\t' << escape(t.check_detail) << '\t' << escape(t.last_result) << "\tend\n";
    }
    std::error_code ec;
    std::filesystem::create_directories(detail::file_path(root), ec);
    if (ec) { error = "cannot create console data folder: " + ec.message(); return false; }
    const auto final_path = detail::file_path(root / state_file);
    const auto temporary = detail::file_path(root / (std::string(state_file) + ".tmp"));
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        const auto text = out.str();
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
        file.flush();
        if (!file) { error = "cannot write console state"; file.close(); std::filesystem::remove(temporary, ec); return false; }
    }
    std::filesystem::rename(temporary, final_path, ec);
    if (ec) { error = "cannot replace console state: " + ec.message(); std::filesystem::remove(temporary, ec); return false; }
    return true;
}

std::string new_profile_id() {
    std::random_device device;
    std::array<std::uint8_t, 16> bytes{};
    for (auto& b : bytes) b = static_cast<std::uint8_t>(device());
    bytes[7] = static_cast<std::uint8_t>((bytes[7] & 0x0f) | 0x40); // GUID version 4 (Data3 high nibble, little endian)
    bytes[8] = static_cast<std::uint8_t>((bytes[8] & 0x3f) | 0x80);
    constexpr char digits[] = "0123456789abcdef";
    std::string id;
    for (const auto b : bytes) { id += digits[b >> 4]; id += digits[b & 15]; }
    return id;
}

std::filesystem::path profile_storage(const std::filesystem::path& root, const Profile& profile) {
    return root / "users" / profile.id;
}

std::filesystem::path title_storage(const std::filesystem::path& root, const Profile& profile, const Title& title) {
    return profile_storage(root, profile) / "titles" / title.id.substr(0, 16);
}

bool prepare_title_storage(const std::filesystem::path& storage, const Profile& profile, std::string& error) {
    if (!hex_id(profile.id, 32)) { error = "invalid profile identity"; return false; }
    std::error_code ec;
    std::filesystem::create_directories(detail::file_path(storage), ec);
    if (ec) { error = "cannot create title storage: " + ec.message(); return false; }
    // Layout read by ERA (src/era_xbox_system.cpp ProfileRecord): magic, local id 1, reserved 0, identity.
    std::array<char, 32> record{'X', 'O', 'N', 'E', 'U', 'S', 'R', '1', 1, 0, 0, 0, 0, 0, 0, 0};
    for (std::size_t i = 0; i < 16; ++i)
        record[16 + i] = static_cast<char>(std::stoi(profile.id.substr(i * 2, 2), nullptr, 16));
    const auto path = detail::file_path(storage / ".xone-local-profile");
    {
        std::ifstream existing(path, std::ios::binary);
        std::array<char, 33> current{};
        if (existing && existing.read(current.data(), 33).gcount() == 32 && std::memcmp(current.data(), record.data(), 32) == 0)
            return true;
    }
    const auto temporary = detail::file_path(storage / ".xone-local-profile.tmp");
    {
        std::ofstream file(temporary, std::ios::binary | std::ios::trunc);
        file.write(record.data(), record.size());
        file.flush();
        if (!file) { error = "cannot write ERA profile record"; return false; }
    }
    std::filesystem::rename(temporary, path, ec);
    if (ec) { error = "cannot install ERA profile record: " + ec.message(); return false; }
    return true;
}

Profile* find_profile(State& state, const std::string& id) {
    for (auto& p : state.profiles) if (p.id == id) return &p;
    return nullptr;
}

Title* find_title(State& state, const std::string& id) {
    for (auto& t : state.titles) if (t.id == id) return &t;
    return nullptr;
}

} // namespace xboxone::console
