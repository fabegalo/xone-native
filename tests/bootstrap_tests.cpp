// SPDX-License-Identifier: BSD-3-Clause
#include "xboxone_native/bootstrap.h"
#include "xboxone_native/inventory.h"
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <utility>
#include <vector>
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif
namespace fs = std::filesystem;
using namespace xboxone::systemos;
namespace {
int failures = 0, assertions = 0;
void check(bool ok, const std::string& text) {
    ++assertions;
    if (!ok) { ++failures; std::cerr << "FAIL: " << text << '\n'; }
}
bool has(const Session& s, std::string_view code) {
    return std::any_of(s.report().diagnostics.begin(), s.report().diagnostics.end(),
                       [&](const auto& d) { return d.code == code; });
}
struct Temporary {
    fs::path root = fs::canonical(fs::temp_directory_path()) /
        ("xboxone_bootstrap_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Temporary() { fs::create_directory(root); }
    ~Temporary() {
        for (const char* name : {"entry.exe", "profile.txt", "resource.dat", "input.xvd"}) {
            std::error_code ec;
            fs::remove(root / name, ec);
        }
        std::error_code ec;
        fs::remove(root, ec); // No recursive delete or user content.
    }
};
void put(std::vector<unsigned char>& b, std::size_t at, std::uint64_t v, unsigned n = 4) {
    for (unsigned i = 0; i < n; ++i) b[at + i] = static_cast<unsigned char>(v >> (i * 8));
}
std::vector<unsigned char> image() {
    std::vector<unsigned char> b(0x400);
    b[0] = 'M'; b[1] = 'Z'; put(b, 0x3c, 0x80);
    b[0x80] = 'P'; b[0x81] = 'E'; put(b, 0x84, 0x8664, 2);
    put(b, 0x86, 1, 2); put(b, 0x94, 0xf0, 2); put(b, 0x96, 2, 2);
    put(b, 0x98, 0x20b, 2); put(b, 0xa8, 0x2000); put(b, 0xb0, 0x140000000ULL, 8);
    put(b, 0xd4, 0x200); put(b, 0xdc, 3, 2); put(b, 0x104, 16);
    put(b, 0x110, 0x2000); put(b, 0x114, 40);
    put(b, 0x190, 0x200); put(b, 0x194, 0x2000); put(b, 0x198, 0x200); put(b, 0x19c, 0x200);
    put(b, 0x200, 0x2060); put(b, 0x20c, 0x2080); put(b, 0x210, 0x2060); put(b, 0x260, 0x20a0, 8);
    const std::string dll = "KERNEL32.dll", symbol = "GetCurrentProcessId";
    std::copy(dll.begin(), dll.end(), b.begin() + 0x280);
    std::copy(symbol.begin(), symbol.end(), b.begin() + 0x2a2);
    return b;
}
void write(const fs::path& p, const std::vector<unsigned char>& bytes) {
    std::ofstream out(p, std::ios::binary);
    out.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
}
std::string hash(const fs::path& path) {
    std::string value, error;
    check(xboxone::inventory::fingerprint(path, value, error), "fingerprint: " + error);
    return value;
}
Profile profile(const fs::path& path) {
    Profile p;
    p.id = "owned-synthetic-v1"; p.version = "synthetic-1"; p.source_sha256 = hash(path); p.entry = "entry";
    p.artifacts.push_back({"entry", p.version, fs::absolute(path), p.source_sha256, Role::executable, "repository-owned synthetic test"});
    p.imports.push_back({"entry", "kernel32.dll", "host:kernel32", "owned test uses documented Windows API semantics"});
    return p;
}
void structural() {
    namespace compat = xboxone::compatibility;
    for (const auto& [domain, name] : std::vector<std::pair<compat::Domain, std::string>>{
             {compat::Domain::hostos, "HostOS"}, {compat::Domain::systemos, "SystemOS"},
             {compat::Domain::gameos_era, "GameOS/ERA"}, {compat::Domain::gamecore, "GameCore"}}) {
        const auto diagnostic = compat::service_blocker(domain,
            {"observed-service", ServiceKind::nt_kernel, "owned dependency evidence"});
        check(diagnostic.code == "service_adapter_missing", "no domain implies a working service provider");
        check(diagnostic.detail.starts_with(name + " service"), "contract domain retained in diagnostic");
        check(diagnostic.detail.ends_with("owned dependency evidence"), "dependency evidence retained");
    }
    check(compat::service_blocker(static_cast<compat::Domain>(100), {}).code == "invalid_service_domain",
          "unknown contract domain rejected");
    auto kind = ServiceKind::storage;
    check(!compat::parse_service_kind("unknown", kind) && kind == ServiceKind::storage,
          "unknown service name rejected without changing output");
    Temporary temp;
    const auto path = temp.root / "entry.exe";
    const auto bytes = image();
    write(path, bytes);
    const auto base = profile(path);
    const auto reject = [&](const Profile& p, const char* code) {
        Session s;
        check(!s.prepare(p), std::string("reject ") + code);
        check(has(s, code), std::string("diagnostic ") + code);
        check(s.report().process_id == 0, "rejection did not launch input");
    };
    auto p = base;
    p.artifacts[0].version = "other-version"; reject(p, "version_mismatch");
    p = base; p.artifacts[0].sha256 = std::string(64, '0'); reject(p, "hash_mismatch");
    p = base; p.artifacts[0].provenance.clear(); reject(p, "missing_provenance");
    p = base; p.artifacts.push_back(p.artifacts[0]); reject(p, "ambiguous_artifact");
    p = base; p.artifacts[0].path = "entry.exe"; reject(p, "invalid_path");
    p = base; p.entry = "absent"; reject(p, "missing_entry");
    p = base; p.imports.clear(); reject(p, "missing_import");
    p = base; p.imports.push_back(p.imports[0]); reject(p, "ambiguous_import");
    p = base; p.imports[0].provider = "artifact:absent"; reject(p, "missing_private_module");
    p = base; p.imports[0].provider = "host:xbox"; reject(p, "unsupported_import_provider");
    p = base; p.imports[0].evidence.clear(); reject(p, "binding_without_evidence");
    p = base; p.artifacts[0].path = temp.root / "absent.exe"; reject(p, "artifact_unreadable_or_total_limit");
    p = base; p.artifacts.push_back(p.artifacts[0]); p.artifacts.back().id = "duplicate-path";
    reject(p, "ambiguous_path");
    p = base; p.artifacts[0].role = static_cast<Role>(100); reject(p, "invalid_role");
    p = base; p.imports.push_back({"entry", "other.dll", "host:kernel32", "explicit"}); reject(p, "unused_binding");
    for (const auto kind : {ServiceKind::nt_kernel, ServiceKind::xbox_device,
                           ServiceKind::xbox_broker, ServiceKind::winrt_activation,
                           ServiceKind::compositor, ServiceKind::storage}) {
        p = base; p.services.push_back({"service-test", kind, "synthetic dependency requirement"});
        reject(p, "service_adapter_missing");
        Session blocked;
        blocked.prepare(p);
        check(!has(blocked, "isolated_backend_required_for_evaluation"),
              "a service requirement does not select a VM backend");
    }
    p = base; p.services.push_back({"invalid-kind", static_cast<ServiceKind>(100), "owned test"});
    reject(p, "invalid_service_kind");
    p = base; p.services.push_back({"missing-evidence", ServiceKind::storage, ""});
    reject(p, "invalid_service");
    p = base; p.services.push_back({"duplicate", ServiceKind::storage, "owned test"});
    p.services.push_back(p.services.front()); reject(p, "invalid_service");
    auto changed = bytes;
    put(changed, 0xdc, 1, 2); write(path, changed); reject(profile(path), "needs_nt_kernel");
    Session kernel_image;
    kernel_image.prepare(profile(path));
    check(std::any_of(kernel_image.report().diagnostics.begin(), kernel_image.report().diagnostics.end(),
                     [](const auto& d) { return d.code == "needs_nt_kernel" &&
                         d.detail.find("backend undecided") != std::string::npos; }),
          "native subsystem does not choose an isolated execution backend");
    changed = bytes; put(changed, 0x96, 0x2002, 2); write(path, changed); reject(profile(path), "entry_not_executable");
    changed = bytes; put(changed, 0x170, 0x20c0); put(changed, 0x174, 64);
    put(changed, 0x2c0, 1); put(changed, 0x2c4, 0x2080);
    put(changed, 0x2cc, 0x2060); put(changed, 0x2d0, 0x2060);
    write(path, changed); reject(profile(path), "delay_imports_unaudited");
    changed = bytes; put(changed, 0x178, 0x20c0); put(changed, 0x17c, 72);
    write(path, changed); reject(profile(path), "clr_activation_unsupported");
    changed = bytes; put(changed, 0x104, 17); write(path, changed); reject(profile(path), "image_invalid");
    changed = bytes; put(changed, 0x84, 0xaa64, 2); write(path, changed); reject(profile(path), "image_invalid");
    write(path, bytes);
    Session audited_only;
    if (audited_only.prepare(base)) {
        check(!audited_only.run(1000), "PE metadata alone does not guarantee OS loader acceptance");
        check(has(audited_only, "native_execution_failed"), "OS loader failure retained explicitly");
        changed = bytes;
        std::fill(changed.begin() + 0x2a2, changed.begin() + 0x2e0, 0);
        const std::string missing = "NonexistentExportM1";
        std::copy(missing.begin(), missing.end(), changed.begin() + 0x2a2);
        write(path, changed); reject(profile(path), "host_symbol_missing");
        write(path, bytes);
    }
    std::vector<unsigned char> xvd(0xe00);
    const std::string magic = "msft-xvd";
    std::copy(magic.begin(), magic.end(), xvd.begin() + 0x200); put(xvd, 0x208, 1);
    const auto container = temp.root / "input.xvd";
    write(container, xvd);
    p = base; p.artifacts.push_back({"container", p.version, container, hash(container), Role::container, "owned fixture"});
    reject(p, "encrypted_container");
    put(xvd, 0x208, 2); write(container, xvd); p.artifacts.back().sha256 = hash(container);
    reject(p, "container_not_executable");
    std::ofstream(temp.root / "resource.dat") << "synthetic resource";
    p = base; p.artifacts.push_back({"resource", "other-version", temp.root / "resource.dat", hash(temp.root / "resource.dat"), Role::resource, "owned asset"});
    reject(p, "version_mismatch");
    const auto manifest = temp.root / "profile.txt";
    { std::ofstream out(manifest); out << "xboxone-systemos-v1\nprofile \"test\" \"1\" synthetic \"" << base.source_sha256
        << "\"\nentry \"entry\"\nartifact \"entry\" \"1\" " << std::quoted(path.generic_string())
        << " \"" << base.artifacts[0].sha256 << "\" executable \"own source\"\n"
           "bind \"entry\" \"kernel32.dll\" \"host:kernel32\" \"documented Windows API\"\n"; }
    Profile parsed;
    std::string error;
    check(read_profile(manifest, parsed, error), "manifest roundtrip: " + error);
    check(parsed.artifacts.size() == 1 && parsed.artifacts[0].path == path, "manifest preserves exact path");
    { std::ofstream out(manifest, std::ios::app); out << "unknown-command success\n"; }
    check(!read_profile(manifest, parsed, error) && parsed.artifacts.empty(), "invalid manifest leaves no usable partial profile");
    { std::ofstream out(manifest); out << "xboxone-systemos-v1\nprofile incomplete\n"; }
    check(!read_profile(manifest, parsed, error), "truncated manifest rejected");
    Session empty;
    check(!empty.run(), "run before prepare rejected");
    Session ready;
    const bool prepared = ready.prepare(base);
    if (prepared) {
        check(ready.report().stage == Stage::prepared && !ready.report().process_id, "prepare does not execute input");
        check(!ready.run(0), "zero timeout rejected");
        changed = bytes; changed.back() = 1; write(path, changed);
        check(!ready.run(), "modified image rejected before launch");
        check(has(ready, "hash_mismatch") && has(ready, "prelaunch_revalidation_failed"), "identity checked again before run");
    } else check(has(ready, "native_backend_unavailable"), "unsupported host is explicit");
}
void native(const fs::path& ok, const fs::path& hang, const fs::path& fail) {
    auto p = profile(ok);
    Session s;
    check(s.prepare(p), "prepare owned AMD64 executable");
    check(s.run(5000), "native memory/thread self-test");
    check(s.report().stage == Stage::exited && s.report().process_id && s.report().primary_thread_resumed,
          "child created, primary thread resumed and exit observed");
    check(s.report().exit_code_available && !s.report().exit_code, "every owned native self-check completed");
    check(s.report().origin == Origin::synthetic, "synthetic execution never attributed to SystemOS");
    check(!s.run(), "session cannot launch twice");
    check(hash(ok) == p.artifacts[0].sha256, "executed input preserved");
    Session failed;
    check(failed.prepare(profile(fail)), "prepare owned failure executable");
    check(!failed.run(), "nonzero exit is failure");
    check(failed.report().stage == Stage::failed && failed.report().exit_code == 27, "exact exit error recorded");
    Session timed;
    check(timed.prepare(profile(hang)), "prepare owned timeout executable");
    const auto start = std::chrono::steady_clock::now();
    check(!timed.run(100), "deadline stops hanging executable");
    check(timed.report().stage == Stage::timed_out, "timeout distinguished from exit");
    check(std::chrono::steady_clock::now() - start < std::chrono::seconds(6), "bounded termination observed");
#ifdef _WIN32
    HANDLE child = OpenProcess(SYNCHRONIZE, FALSE, timed.report().process_id);
    check(!child || WaitForSingleObject(child, 0) == WAIT_OBJECT_0, "no live child left after timeout");
    if (child) CloseHandle(child);
#endif
}
}
int main(int argc, char** argv) {
    if (argc == 1) structural();
    else if (argc == 4) native(fs::u8path(argv[1]), fs::u8path(argv[2]), fs::u8path(argv[3]));
    else return 2;
    std::cout << assertions << " bootstrap assertions; " << failures << " failures\n";
    return failures ? 1 : 0;
}
