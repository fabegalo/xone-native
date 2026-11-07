// SPDX-License-Identifier: BSD-3-Clause
#include "xboxone_native/bootstrap.h"

#include <charconv>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>

namespace {
std::string escape(std::string_view s) {
    std::string out;
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += static_cast<char>(c); }
        else if (c < 32) { out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
        else out += static_cast<char>(c);
    }
    return out;
}

void print(const xboxone::systemos::Report& r) {
    std::cout << "{\"profile\":\"" << escape(r.profile_id) << "\",\"version\":\"" << escape(r.version)
              << "\",\"declared_origin\":\"" << (r.origin == xboxone::systemos::Origin::synthetic ? "synthetic" : "original")
              << "\",\"source_sha256\":\"" << escape(r.source_sha256) << "\",\"entry_sha256\":\"" << escape(r.entry_sha256)
              << "\",\"stage\":\"" << xboxone::systemos::stage_name(r.stage)
              << "\",\"process_id\":" << r.process_id << ",\"primary_thread_resumed\":" << (r.primary_thread_resumed ? "true" : "false")
              << ",\"exit_code\":";
    if (r.exit_code_available) std::cout << r.exit_code;
    else std::cout << "null";
    std::cout << ",\"execution_model\":\"" << escape(r.execution_model) << "\",\"activated_aumid\":\"" << escape(r.activated_aumid)
              << "\",\"appmodel_lifecycle\":\"" << escape(r.appmodel_lifecycle)
              << "\",\"activation_identity_verified\":" << (r.activation_identity_verified ? "true" : "false")
              << ",\"owned_lifecycle_verified\":" << (r.owned_lifecycle_verified ? "true" : "false")
              << ",\"activation_events\":[";
    bool event_first = true;
    for (const auto& e : r.activation_events) {
        if (!event_first) std::cout << ',';
        event_first = false;
        std::cout << "{\"api\":\"" << escape(e.api) << "\",\"result\":" << e.result << ",\"result_kind\":\"" << escape(e.result_kind) << "\"}";
    }
    std::cout << "],\"packages\":[";
    bool package_first = true;
    for (const auto& p : r.packages) {
        if (!package_first) std::cout << ',';
        package_first = false;
        const auto& m = p.manifest;
        std::cout << "{\"artifact\":\"" << escape(p.artifact) << "\",\"name\":\"" << escape(m.name)
                  << "\",\"publisher\":\"" << escape(m.publisher) << "\",\"version_raw\":" << m.version
                  << ",\"architecture\":" << m.architecture << ",\"resource_id\":\"" << escape(m.resource_id)
                  << "\",\"full_name\":\"" << escape(m.full_name) << "\",\"family_name\":\"" << escape(m.family_name) << "\",\"applications\":[";
        bool first_app = true;
        for (const auto& a : m.applications) {
            if (!first_app) std::cout << ',';
            first_app = false;
            std::cout << "{\"id\":\"" << escape(a.id) << "\",\"executable\":\"" << escape(a.executable)
                      << "\",\"entry_point\":\"" << escape(a.entry_point) << "\",\"aumid\":\"" << escape(a.aumid) << "\"}";
        }
        std::cout << "],\"capabilities\":[";
        bool first_capability = true;
        for (const auto& c : m.capabilities) {
            if (!first_capability) std::cout << ',';
            first_capability = false;
            std::cout << "{\"name\":\"" << escape(c.name) << "\",\"namespace\":\"" << escape(c.namespace_uri) << "\"}";
        }
        std::cout << "],\"dependencies\":[";
        bool first_dependency = true;
        for (const auto& d : m.dependencies) {
            if (!first_dependency) std::cout << ',';
            first_dependency = false;
            std::cout << "{\"name\":\"" << escape(d.name) << "\",\"publisher\":\"" << escape(d.publisher)
                      << "\",\"min_version_raw\":" << d.min_version << '}';
        }
        std::cout << "],\"device_families\":[";
        bool first_family = true;
        for (const auto& family : m.device_families) {
            if (!first_family) std::cout << ',';
            first_family = false;
            std::cout << '"' << escape(family) << '"';
        }
        std::cout << "]}";
    }
    std::cout << "],\"dependency_graph_version\":1,\"dashboard_boot_verified\":false,\"authentic_frames_observed\":0,\"dependencies\":[";
    bool first = true;
    for (const auto& d : r.dependencies) {
        if (!first) std::cout << ',';
        first = false;
        std::cout << "{\"requester\":\"" << escape(d.requester) << "\",\"library\":\"" << escape(d.library)
                  << "\",\"provider\":\"" << escape(d.provider) << "\",\"status\":\"" << escape(d.status) << "\",\"symbol\":\"" << escape(d.symbol)
                  << "\",\"kind\":\"" << escape(d.kind) << "\",\"api_set\":" << (d.api_set ? "true" : "false") << ",\"by_ordinal\":" << (d.by_ordinal ? "true" : "false") << '}';
    }
    std::cout << "],\"nodes\":[";
    first = true;
    for (const auto& n : r.nodes) {
        if (!first) std::cout << ',';
        first = false;
        const auto path = n.artifact.path.generic_u8string();
        const char* role = "invalid";
        switch (n.artifact.role) {
            case xboxone::systemos::Role::executable: role = "executable"; break;
            case xboxone::systemos::Role::module: role = "module"; break;
            case xboxone::systemos::Role::resource: role = "resource"; break;
            case xboxone::systemos::Role::container: role = "container"; break;
        }
        std::cout << "{\"id\":\"" << escape(n.artifact.id) << "\",\"version\":\"" << escape(n.artifact.version)
                  << "\",\"sha256\":\"" << escape(n.artifact.sha256) << "\",\"path\":\""
                  << escape({reinterpret_cast<const char*>(path.data()), path.size()})
                  << "\",\"provenance\":\"" << escape(n.artifact.provenance) << "\",\"role\":\"" << role
                  << "\",\"status\":\"" << escape(n.status) << "\",\"pe\":";
        if (n.status == "inspected_pe") {
            std::cout << '{';
            xboxone::inspection::write_pe_json_fields(std::cout, n.pe);
            std::cout << '}';
        } else std::cout << "null";
        std::cout << '}';
    }
    std::cout << "],\"diagnostics\":[";
    first = true;
    for (const auto& d : r.diagnostics) {
        if (!first) std::cout << ',';
        first = false;
        std::cout << "{\"code\":\"" << escape(d.code) << "\",\"subject\":\"" << escape(d.subject)
                  << "\",\"detail\":\"" << escape(d.detail) << "\"}";
    }
    std::cout << "]}\n";
}
} // namespace

#ifdef _WIN32
int wmain(int argc, wchar_t** argv) {
#else
int main(int argc, char** argv) {
#endif
    if (argc == 2 && std::filesystem::path(argv[1]) == "--help") {
        std::cout << "SystemOS bootstrap (no renderer)\n"
                     "  xboxone-systemos prepare <manifest>\n"
                     "  xboxone-systemos run <manifest> [timeout-ms: 1..60000, default 5000]\n"
                     "Win32 console or audited packaged desktop contract; no Xbox activation.\n"
                     "AppModel defaults to process_exit observation; appmodel_lifecycle owned_fixture selects the test handshake.\n"
                     "AppModel run never registers packages. Its timeout bounds observation after synchronous COM activation.\n";
        return 0;
    }
    if (argc < 3 || argc > 4) { std::cerr << "invalid arguments; use --help\n"; return 2; }
    const auto command = std::filesystem::path(argv[1]).string();
    if ((command != "prepare" && command != "run") || (command == "prepare" && argc != 3)) {
        std::cerr << "invalid command; use --help\n";
        return 2;
    }
    std::uint32_t timeout = 5000;
    if (argc == 4) {
        const auto value = std::filesystem::path(argv[3]).string();
        const auto parsed = std::from_chars(value.data(), value.data() + value.size(), timeout);
        if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || !timeout || timeout > 60000) {
            std::cerr << "invalid timeout (1..60000 ms required)\n";
            return 2;
        }
    }
    xboxone::systemos::Profile profile;
    std::string error;
    if (!xboxone::systemos::read_profile(argv[2], profile, error)) {
        std::cerr << "invalid manifest: " << error << '\n';
        return 2;
    }
    xboxone::systemos::Session session;
    bool ok = session.prepare(profile);
    if (ok && command == "run") ok = session.run(timeout);
    print(session.report());
    return ok ? 0 : 1;
}
