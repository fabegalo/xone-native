// SPDX-License-Identifier: BSD-3-Clause
#include "xboxone_native/bootstrap.h"
#include "xboxone_native/inspection.h"
#include "xboxone_native/inventory.h"
#include "appmodel_internal.h"
#include "file_path.h"

#include <algorithm>
#include <fstream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <string_view>
#include <tuple>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace xboxone::systemos {
namespace {
namespace fs = std::filesystem;

std::string fold(std::string s) {
    for (auto& c : s) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    return s;
}

bool digest(std::string_view s) {
    return s.size() == 64 && std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    });
}

bool token(std::string_view s) {
    return !s.empty() && s.size() <= 256 && std::all_of(s.begin(), s.end(), [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
    });
}

void block(Report& r, std::string code, std::string subject, std::string detail) {
    r.diagnostics.push_back({std::move(code), std::move(subject), std::move(detail)});
    r.stage = Stage::blocked;
}

bool role_value(std::string_view s, Role& out) {
    if (s == "executable") out = Role::executable;
    else if (s == "module") out = Role::module;
    else if (s == "resource") out = Role::resource;
    else if (s == "container") out = Role::container;
    else return false;
    return true;
}

bool native_host() {
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
    // ARM64 emulation is a distinct, unvalidated target.
    SYSTEM_INFO info{};
    GetNativeSystemInfo(&info);
    return info.wProcessorArchitecture == PROCESSOR_ARCHITECTURE_AMD64;
#else
    return false;
#endif
}

bool host_symbol(const std::string& symbol) {
#ifdef _WIN32
    const auto module = GetModuleHandleW(L"kernel32.dll");
    if (!module || symbol.empty()) return false;
    if (symbol.front() == '#') {
        // Ordinal bindings require a separate audited contract, not a guessed ordinal.
        return false;
    }
    return GetProcAddress(module, symbol.c_str()) != nullptr;
#else
    (void)symbol;
    return false;
#endif
}

#ifdef _WIN32
struct Handle {
    HANDLE value = nullptr;
    ~Handle() { if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    Handle() = default;
    explicit Handle(HANDLE h) : value(h) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
};

std::string win_error(const char* api) {
    return std::string(api) + " failed; win32=" + std::to_string(GetLastError());
}
#endif
} // namespace

const char* stage_name(Stage stage) noexcept {
    switch (stage) {
        case Stage::empty: return "empty";
        case Stage::blocked: return "blocked";
        case Stage::prepared: return "prepared";
        case Stage::process_created: return "process_created";
        case Stage::running: return "running";
        case Stage::exited: return "exited";
        case Stage::timed_out: return "timed_out";
        case Stage::failed: return "failed";
    }
    return "unknown";
}

bool read_profile(const fs::path& path, Profile& profile, std::string& error) {
    profile = {};
    error.clear();
    std::error_code ec;
    if (fs::file_size(detail::file_path(path), ec) > 1024 * 1024 || ec) {
        error = "manifest inaccessible or exceeds 1 MiB";
        return false;
    }
    std::ifstream input(detail::file_path(path));
    std::string line;
    bool header = false, identity = false, entry = false, lifecycle = false;
    std::size_t number = 0, bytes = 0;
    Profile candidate;
    while (std::getline(input, line)) {
        ++number;
        bytes += line.size() + 1;
        if (number > 8192 || line.size() > 8192 || bytes > 1024 * 1024) { error = "manifest read limit exceeded"; return false; }
        std::istringstream row(line);
        std::string command;
        row >> command;
        if (command.empty() || command.front() == '#') continue;
        bool valid = true;
        if (!header) {
            valid = command == "xboxone-systemos-v1";
            header = valid;
        } else if (command == "profile" && !identity) {
            std::string origin;
            row >> std::quoted(candidate.id) >> std::quoted(candidate.version) >> origin
                >> std::quoted(candidate.source_sha256);
            valid = origin == "synthetic" || origin == "original";
            candidate.origin = origin == "original" ? Origin::original : Origin::synthetic;
            identity = true;
        } else if (command == "entry" && !entry) {
            row >> std::quoted(candidate.entry);
            entry = true;
        } else if (command == "artifact") {
            Artifact a;
            std::string file, role;
            row >> std::quoted(a.id) >> std::quoted(a.version) >> std::quoted(file)
                >> std::quoted(a.sha256) >> role >> std::quoted(a.provenance);
            valid = role_value(role, a.role);
            a.path = fs::path(std::u8string(file.begin(), file.end()));
            candidate.artifacts.push_back(std::move(a));
        } else if (command == "bind") {
            ImportBinding b;
            row >> std::quoted(b.requester) >> std::quoted(b.library)
                >> std::quoted(b.provider) >> std::quoted(b.evidence);
            candidate.imports.push_back(std::move(b));
        } else if (command == "service") {
            ServiceRequirement s;
            std::string kind;
            row >> std::quoted(s.id) >> kind >> std::quoted(s.evidence);
            valid = compatibility::parse_service_kind(kind, s.kind);
            candidate.services.push_back(std::move(s));
        } else if (command == "appmodel" && candidate.appmodel.manifest.empty()) {
            row >> std::quoted(candidate.appmodel.manifest) >> std::quoted(candidate.appmodel.application_id)
                >> std::quoted(candidate.appmodel.evidence);
            valid = !candidate.appmodel.manifest.empty();
        } else if (command == "appmodel_lifecycle" && !lifecycle) {
            std::string value;
            row >> value;
            valid = value == "process_exit" || value == "owned_fixture";
            candidate.appmodel.lifecycle = value == "owned_fixture" ?
                AppModelActivation::Lifecycle::owned_fixture : AppModelActivation::Lifecycle::process_exit;
            lifecycle = true;
        } else if (command == "package_bind") {
            PackageBinding b;
            row >> std::quoted(b.requester) >> std::quoted(b.name) >> std::quoted(b.provider) >> std::quoted(b.evidence);
            candidate.packages.push_back(std::move(b));
        } else valid = false;
        if (row.fail()) valid = false;
        std::string extra;
        if (row >> extra) valid = false;
        if (!valid) { error = "invalid manifest record at line " + std::to_string(number); return false; }
    }
    if (input.bad() || !header || !identity || !entry || (lifecycle && candidate.appmodel.manifest.empty())) {
        error = "incomplete manifest (header, profile and entry required)";
        return false;
    }
    profile = std::move(candidate);
    return true;
}

bool Session::prepare(const Profile& profile) {
    // A session has one identity and one attempt, even after failure.
    if (report_.stage != Stage::empty) {
        report_.diagnostics.push_back({"invalid_transition", "prepare", "create a new session for another attempt"});
        return false;
    }
    profile_ = profile;
    report_.profile_id = profile.id;
    report_.version = profile.version;
    report_.origin = profile.origin;
    report_.source_sha256 = profile.source_sha256;
    const bool packaged = !profile.appmodel.manifest.empty();
    report_.execution_model = packaged ? "appmodel" : "win32";
    if (packaged) report_.appmodel_lifecycle = profile.appmodel.lifecycle == AppModelActivation::Lifecycle::owned_fixture ?
        "owned_fixture" : "process_exit";
    if (!token(profile.id) || profile.version.empty() || !digest(profile.source_sha256) ||
        (profile.origin != Origin::synthetic && profile.origin != Origin::original))
        block(report_, "invalid_identity", profile.id, "profile id, exact revision and source SHA-256 are required");
    if (profile.artifacts.empty() || profile.artifacts.size() > 256 || profile.imports.size() > 4096 ||
        profile.services.size() > 256 || profile.packages.size() > 256) {
        block(report_, "manifest_limit", profile.id, "1..256 artifacts, <=4096 bindings and <=256 services required");
        return false;
    }
    std::map<std::string, const Artifact*> artifacts;
    std::map<std::string, inspection::Report> images;
    std::set<std::string> ambiguous_artifacts;
    std::set<fs::path> paths;
    std::uint64_t total_bytes = 0;
    std::size_t metadata_records = 0, metadata_bytes = 0;
    report_.nodes.reserve(profile.artifacts.size());
    for (const auto& a : profile.artifacts) {
        const auto identity_errors = report_.diagnostics.size();
        auto& node = report_.nodes.emplace_back(DependencyNode{a, "identity_rejected", {}});
        if (!token(a.id) || !artifacts.emplace(a.id, &a).second) {
            ambiguous_artifacts.insert(a.id);
            block(report_, "ambiguous_artifact", a.id, "invalid or duplicate artifact id");
            continue;
        }
        if (a.version != profile.version)
            block(report_, "version_mismatch", a.id, "artifact revision differs from the session; fallback forbidden");
        if (a.role != Role::executable && a.role != Role::module && a.role != Role::resource && a.role != Role::container)
            block(report_, "invalid_role", a.id, "unknown artifact role");
        if (!digest(a.sha256) || a.provenance.empty())
            block(report_, "missing_provenance", a.id, "per-artifact SHA-256 and access/source evidence required");
        if (!a.path.is_absolute()) {
            block(report_, "invalid_path", a.id, "artifact path must be absolute");
            continue;
        }
        std::error_code ec;
        const auto size = fs::file_size(detail::file_path(a.path), ec);
        if (ec || size > 1024ULL * 1024 * 1024 - total_bytes) {
            block(report_, "artifact_unreadable_or_total_limit", a.id, "input inaccessible or manifest exceeds 1 GiB");
            continue;
        }
        total_bytes += size;
        std::string hash, error;
        if (!inventory::fingerprint(a.path, hash, error)) {
            block(report_, "artifact_unreadable", a.id, error);
            continue;
        }
        const auto canonical = fs::canonical(detail::file_path(a.path), ec);
        const bool duplicate = !ec && std::any_of(paths.begin(), paths.end(), [&](const auto& previous) {
            std::error_code same_error;
            return fs::equivalent(detail::file_path(previous), detail::file_path(canonical), same_error) && !same_error;
        });
        if (ec || duplicate)
            block(report_, "ambiguous_path", a.id, "same artifact referenced more than once or canonical path unavailable");
        else paths.insert(canonical);
        if (hash != a.sha256) block(report_, "hash_mismatch", a.id, "content does not match pinned SHA-256");
        if (a.id == profile.entry) report_.entry_sha256 = hash;
        if (report_.diagnostics.size() != identity_errors) continue;
        node.status = "identity_verified";
        if (a.role == Role::resource) continue;
        inspection::Report image;
        if (!inspection::inspect(a.path, image, error)) {
            block(report_, "image_invalid", a.id, error);
            continue;
        }
        if (image.kind == inspection::Kind::xvd) {
            node.status = "identified_container";
            block(report_, (image.xvd.volume_flags & 2) ? "container_not_executable" : "encrypted_container",
                  a.id, "XVD metadata does not expose a readable executable; no decryption backend");
            continue;
        }
        if (a.role == Role::container) {
            block(report_, "role_mismatch", a.id, "PE cannot satisfy a declared container role");
            continue;
        }
        std::size_t records = image.pe.exports.size(), bytes = 0;
        if (image.pe.tls) records += image.pe.tls->callback_rvas.size();
        for (const auto& i : image.pe.imports) records += i.functions.size();
        for (const auto& e : image.pe.exports) records += e.names.size();
        for (const auto& m : image.pe.activation_metadata) bytes += m.bytes_hex.size() / 2;
        if (records > 65536 - metadata_records || bytes > 16 * 1024 * 1024 - metadata_bytes) {
            block(report_, "dependency_metadata_limit", a.id, "profile exceeds 65536 symbol records or 16 MiB activation resources");
            continue;
        }
        metadata_records += records; metadata_bytes += bytes;
        node.status = "inspected_pe";
        node.pe = image.pe;
        const auto& pe = image.pe;
        if (pe.subsystem == 1 || (pe.characteristics & 0x1000))
            block(report_, "needs_nt_kernel", a.id, "native subsystem/system image needs an unimplemented kernel contract; execution backend undecided");
        else if (pe.subsystem != 3 && !(packaged && pe.subsystem == 2))
            block(report_, "activation_model_unsupported", a.id, "only audited Win32 console activation is implemented");
        if (pe.has_delay_imports)
            block(report_, "delay_imports_unaudited", a.id, "delay imports were inspected; execution semantics remain unaudited");
        if (std::any_of(pe.activation_metadata.begin(), pe.activation_metadata.end(), [](const auto& m) {
                return m.kind == "rt_manifest";
            })) block(report_, "activation_manifest_unaudited", a.id, "embedded manifest identified; SxS/package activation contract not audited");
        if (pe.has_clr_header)
            block(report_, "clr_activation_unsupported", a.id, "managed activation is not implemented");
        if (a.id == profile.entry && (a.role != Role::executable || (pe.characteristics & 0x2000) ||
                                    !(pe.characteristics & 2) || pe.entry_point_rva == 0))
            block(report_, "entry_not_executable", a.id, "entry must be a PE executable with a real entry point, not a DLL");
        images.emplace(a.id, std::move(image));
    }
    // A duplicate ID invalidates every candidate, including the first record.
    // Never publish a selected export from an ambiguous manifest identity.
    for (const auto& id : ambiguous_artifacts) images.erase(id);
    for (auto& node : report_.nodes) if (ambiguous_artifacts.count(node.artifact.id)) {
        node.status = "ambiguous_artifact";
        node.pe = {};
    }
    const auto found_entry = artifacts.find(profile.entry);
    if (found_entry == artifacts.end() || found_entry->second->role != Role::executable || !images.count(profile.entry))
        block(report_, "missing_entry", profile.entry, "no audited readable PE executable for entry id");

    using Key = std::pair<std::string, std::string>;
    std::map<Key, const ImportBinding*> bindings;
    std::set<Key> ambiguous, used;
    for (const auto& b : profile.imports) {
        const Key key{b.requester, fold(b.library)};
        if (!bindings.emplace(key, &b).second) {
            ambiguous.insert(key);
            block(report_, "ambiguous_import", b.requester + ":" + b.library, "multiple explicit providers; no fallback selected");
        }
        if (b.evidence.empty()) block(report_, "binding_without_evidence", b.requester, "compatibility must be audited explicitly");
    }
    std::map<Key, const inspection::Export*> named_exports, ordinal_exports;
    for (const auto& [id, image] : images) for (const auto& e : image.pe.exports) {
        ordinal_exports.emplace(Key{id, "#" + std::to_string(e.ordinal)}, &e);
        for (const auto& name : e.names) named_exports.emplace(Key{id, name}, &e);
    }
    std::size_t traversed = 0;
    bool graph_limit = false;
    const auto resolve = [&](auto&& self, const std::string& id, const std::string& library,
                             const std::string& symbol, const std::string& kind, bool by_ordinal, std::set<Key>& active) -> void {
        if (++traversed > 65536 || active.size() >= 64) {
            if (!graph_limit) block(report_, "dependency_graph_limit", id, "<=65536 edges and forwarder depth <64 required");
            graph_limit = true;
            return;
        }
        const Key key{id, fold(library)};
        const auto binding = bindings.find(key);
        Dependency d{id, fold(library), "", "missing_binding", symbol, kind, inspection::is_api_set(library), by_ordinal};
        const auto subject = id + ":" + d.library + "!" + symbol;
        std::string next_id, next_library, next_symbol;
        if (binding == bindings.end()) {
            block(report_, d.api_set ? "api_set_mapping_missing" : "missing_import", subject,
                  "no explicit provider in this version-bound profile; no host/schema fallback");
        } else {
            used.insert(key);
            const auto& b = *binding->second;
            d.provider = b.provider;
            if (ambiguous.count(key) || b.evidence.empty()) d.status = "binding_rejected";
            else if (b.provider == "host:kernel32" && key.second == "kernel32.dll" && !d.api_set) {
                d.status = "host_exports_verified";
                if (!native_host()) {
                    d.status = "host_unavailable";
                    block(report_, "native_backend_unavailable", subject, "requires native Windows AMD64");
                } else if (!symbol.empty() && !host_symbol(symbol)) {
                    d.status = "host_symbol_missing";
                    block(report_, "host_symbol_missing", subject, "export unavailable or ordinal contract unaudited");
                }
            } else if (b.provider.starts_with("artifact:")) {
                const auto provider = b.provider.substr(9);
                const auto target = artifacts.find(provider);
                const auto image = images.find(provider);
                if (target == artifacts.end() || target->second->role != Role::module) {
                    d.status = "missing_private_module";
                    block(report_, "missing_private_module", subject, "exact module id required: " + provider);
                } else if (image == images.end()) {
                    d.status = "private_identity_unverified";
                    block(report_, "private_identity_unverified", subject, "provider version/hash/PE validation failed: " + provider);
                } else {
                    block(report_, "private_module_activation_unsupported", subject,
                          "static export analysis cannot authorize isolated DLL search or ABI execution: " + provider);
                    const auto& exports = by_ordinal ? ordinal_exports : named_exports;
                    const auto found = exports.find(Key{provider, symbol});
                    if (symbol.empty()) d.status = "private_module_present";
                    else if (found == exports.end()) {
                        d.status = "private_symbol_missing";
                        block(report_, "private_symbol_missing", subject, "symbol absent from pinned provider " + provider);
                    } else if (found->second->forwarder.empty()) d.status = "private_export_present";
                    else if (active.count({provider, symbol + (by_ordinal ? ":ordinal" : ":name")})) {
                        d.status = "forwarder_cycle";
                        block(report_, "forwarder_cycle", subject, "cycle at " + provider + "!" + symbol);
                    } else {
                        d.status = "private_forwarder_present";
                        next_id = provider; next_library = found->second->forward_library; next_symbol = found->second->forward_symbol;
                    }
                }
            } else {
                d.status = "unsupported_provider";
                block(report_, "unsupported_import_provider", subject, "no verified adapter for " + b.provider);
            }
        }
        report_.dependencies.push_back(std::move(d));
        if (!next_id.empty()) {
            const Key target{next_id, symbol + (by_ordinal ? ":ordinal" : ":name")};
            active.insert(target);
            self(self, next_id, next_library, next_symbol, "forwarder", next_symbol.starts_with("#"), active);
            active.erase(target);
        }
    };
    for (const auto& [id, image] : images) {
        for (const auto& imported : image.pe.imports) {
            std::set<Key> active;
            if (imported.functions.empty()) resolve(resolve, id, imported.library, "", imported.delay_load ? "delay_import" : "import", false, active);
            for (std::size_t index = 0; index < imported.functions.size(); ++index)
                resolve(resolve, id, imported.library, imported.functions[index], imported.delay_load ? "delay_import" : "import",
                        std::binary_search(imported.ordinal_indices.begin(), imported.ordinal_indices.end(), index), active);
        }
        // Include unused forwarded exports too; a static graph is conservative.
        for (const auto& e : image.pe.exports) if (!e.forwarder.empty()) {
            std::set<Key> active{{id, "#" + std::to_string(e.ordinal) + ":ordinal"}};
            resolve(resolve, id, e.forward_library, e.forward_symbol, "forwarder", e.forward_symbol.starts_with("#"), active);
        }
    }
    for (const auto& [key, binding] : bindings) {
        (void)binding;
        if (!used.count(key)) block(report_, "unused_binding", key.first + ":" + key.second, "binding does not correspond to an inspected dependency");
    }
    if (packaged) prepare_appmodel(profile, report_);
    else if (!profile.packages.empty() || !profile.appmodel.application_id.empty() || !profile.appmodel.evidence.empty())
        block(report_, "package_activation_missing", profile.id, "AppModel manifest selection required");
    std::set<std::string> service_ids;
    for (const auto& s : profile.services) {
        if (!token(s.id) || !service_ids.insert(s.id).second || s.evidence.empty())
            block(report_, "invalid_service", s.id, "unique service id and dependency evidence required");
        const auto diagnostic = compatibility::service_blocker(compatibility::Domain::systemos, s);
        block(report_, diagnostic.code, s.id, diagnostic.detail);
    }
    if (!native_host())
        block(report_, "native_backend_unavailable", profile.id, "execution implemented only for native Windows AMD64");
    std::sort(report_.nodes.begin(), report_.nodes.end(), [](const auto& a, const auto& b) {
        return std::tie(a.artifact.id, a.artifact.version, a.artifact.sha256, a.artifact.path, a.artifact.provenance, a.artifact.role, a.status) <
               std::tie(b.artifact.id, b.artifact.version, b.artifact.sha256, b.artifact.path, b.artifact.provenance, b.artifact.role, b.status);
    });
    const auto edge_key = [](const auto& d) { return std::tie(d.requester, d.kind, d.library, d.symbol, d.by_ordinal, d.provider, d.status); };
    std::sort(report_.dependencies.begin(), report_.dependencies.end(), [&](const auto& a, const auto& b) { return edge_key(a) < edge_key(b); });
    report_.dependencies.erase(std::unique(report_.dependencies.begin(), report_.dependencies.end(), [&](const auto& a, const auto& b) {
        return edge_key(a) == edge_key(b);
    }), report_.dependencies.end());
    const auto diagnostic_key = [](const auto& d) { return std::tie(d.code, d.subject, d.detail); };
    std::sort(report_.diagnostics.begin(), report_.diagnostics.end(), [&](const auto& a, const auto& b) { return diagnostic_key(a) < diagnostic_key(b); });
    report_.diagnostics.erase(std::unique(report_.diagnostics.begin(), report_.diagnostics.end(), [&](const auto& a, const auto& b) {
        return diagnostic_key(a) == diagnostic_key(b);
    }), report_.diagnostics.end());
    if (!report_.diagnostics.empty()) return false;
    report_.stage = Stage::prepared;
    return true;
}

bool Session::run(std::uint32_t timeout_ms) {
    if (report_.stage != Stage::prepared || timeout_ms == 0 || timeout_ms > 60000) {
        report_.diagnostics.push_back({"invalid_transition_or_timeout", "run", "prepared session and timeout 1..60000 ms required"});
        return false;
    }
    Session checked;
    if (!checked.prepare(profile_)) {
        report_ = checked.report();
        report_.diagnostics.push_back({"prelaunch_revalidation_failed", profile_.id, "input changed or dependency no longer available"});
        return false;
    }
    if (!profile_.appmodel.manifest.empty()) return run_appmodel(profile_, report_, timeout_ms);
#ifdef _WIN32
    const auto entry = std::find_if(profile_.artifacts.begin(), profile_.artifacts.end(),
                                  [&](const auto& a) { return a.id == profile_.entry; });
    const auto fail = [&](std::string detail) {
        report_.stage = Stage::failed;
        report_.diagnostics.push_back({"native_execution_failed", profile_.entry, std::move(detail)});
        return false;
    };
    Handle job(CreateJobObjectW(nullptr, nullptr));
    if (!job.value) return fail(win_error("CreateJobObjectW"));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
    limits.BasicLimitInformation.ActiveProcessLimit = 1;
    if (!SetInformationJobObject(job.value, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        return fail(win_error("SetInformationJobObject"));
    // Pin the entry against write/delete during the loader's open and run.
    const auto executable = detail::file_path(entry->path);
    Handle input(CreateFileW(executable.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                             OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (input.value == INVALID_HANDLE_VALUE) return fail(win_error("CreateFileW"));
    // Recheck after acquiring the deny-write handle; path checks assume stable ancestors.
    std::string hash, error;
    if (!inventory::fingerprint(entry->path, hash, error) || hash != entry->sha256)
        return fail("entry changed before locked launch: " + error);
    std::wstring command = L"\"" + executable.wstring() + L"\"";
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    // Explicit application path, no shell, no inherited handles, no visible window.
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                        CREATE_SUSPENDED | CREATE_NO_WINDOW, nullptr, executable.parent_path().c_str(),
                        &startup, &process)) {
        if (GetLastError() == ERROR_DIRECTORY && entry->path.parent_path().native().size() >= MAX_PATH)
            return fail("CreateProcessW rejected the long working directory; input remains in place and no alternate directory was selected; win32=267");
        return fail(win_error("CreateProcessW"));
    }
    Handle child(process.hProcess), thread(process.hThread);
    report_.stage = Stage::process_created;
    report_.process_id = process.dwProcessId;
    if (!AssignProcessToJobObject(job.value, child.value)) {
        const auto detail = win_error("AssignProcessToJobObject");
        TerminateProcess(child.value, 1);
        WaitForSingleObject(child.value, 5000);
        return fail(detail);
    }
    if (ResumeThread(thread.value) == static_cast<DWORD>(-1)) {
        const auto detail = win_error("ResumeThread");
        TerminateJobObject(job.value, 1);
        WaitForSingleObject(child.value, 5000);
        return fail(detail);
    }
    report_.stage = Stage::running;
    report_.primary_thread_resumed = true;
    const auto waited = WaitForSingleObject(child.value, timeout_ms);
    if (waited != WAIT_OBJECT_0) {
        const auto detail = waited == WAIT_TIMEOUT ? "execution deadline exceeded" : win_error("WaitForSingleObject");
        if (!TerminateJobObject(job.value, 1)) return fail(win_error("TerminateJobObject"));
        if (WaitForSingleObject(child.value, 5000) != WAIT_OBJECT_0) return fail("termination was not observed within 5 seconds");
        report_.stage = waited == WAIT_TIMEOUT ? Stage::timed_out : Stage::failed;
        report_.diagnostics.push_back({"execution_incomplete", profile_.entry, detail});
        return false;
    }
    DWORD exit_code = 0;
    if (!GetExitCodeProcess(child.value, &exit_code)) return fail(win_error("GetExitCodeProcess"));
    report_.exit_code = exit_code;
    report_.exit_code_available = true;
    if (exit_code != 0) return fail("component exited with code " + std::to_string(exit_code));
    report_.stage = Stage::exited;
    return true;
#else
    block(report_, "native_backend_unavailable", profile_.id, "requires native Windows AMD64");
    return false;
#endif
}

} // namespace xboxone::systemos
