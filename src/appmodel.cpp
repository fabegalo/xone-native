// SPDX-License-Identifier: BSD-3-Clause
#include "appmodel_internal.h"
#include <algorithm>
#include <map>
#include <set>

namespace xboxone::systemos {
namespace {
void reject(Report& r, std::string code, std::string subject, std::string detail) {
    r.stage = Stage::blocked;
    r.diagnostics.push_back({std::move(code), std::move(subject), std::move(detail)});
}
}
void prepare_appmodel(const Profile& profile, Report& report) {
    const auto& activation = profile.appmodel;
    if (activation.lifecycle != AppModelActivation::Lifecycle::process_exit &&
        activation.lifecycle != AppModelActivation::Lifecycle::owned_fixture)
        reject(report, "appmodel_lifecycle_invalid", activation.manifest, "unknown observation contract");
    if (activation.application_id.empty() || activation.evidence.empty())
        reject(report, "appmodel_selection_invalid", activation.manifest, "application ID and audited activation evidence required");
    std::map<std::string, const DependencyNode*> nodes;
    for (const auto& n : report.nodes) {
        if (n.status == "identity_verified" && n.artifact.role == Role::resource) nodes.emplace(n.artifact.id, &n);
    }
    using Key = std::pair<std::string, std::string>;
    std::map<Key, const PackageBinding*> bindings;
    std::set<Key> ambiguous, used;
    for (const auto& b : profile.packages) {
        const Key key{b.requester, b.name};
        if (!bindings.emplace(key, &b).second || b.evidence.empty()) {
            ambiguous.insert(key);
            reject(report, "ambiguous_package_binding", b.requester + ":" + b.name, "one explicit provider with evidence required");
        }
    }
    std::map<std::string, appmodel::Manifest> packages;
    std::set<std::string> failed;
    const auto load = [&](const std::string& id) -> const appmodel::Manifest* {
        if (const auto found = packages.find(id); found != packages.end()) return &found->second;
        if (failed.count(id)) return nullptr;
        const auto node = nodes.find(id);
        appmodel::Manifest manifest;
        std::string error;
        if (node == nodes.end() || node->second->artifact.path.filename() != "AppxManifest.xml")
            error = "exact identity-verified resource artifact named AppxManifest.xml required";
        else if (appmodel::read_manifest(node->second->artifact.path, node->second->artifact.sha256, manifest, error))
            return &packages.emplace(id, std::move(manifest)).first->second;
        failed.insert(id);
        reject(report, "appx_manifest_rejected", id, error);
        return nullptr;
    };
    std::set<std::string> active, visited;
    const auto walk = [&](auto&& self, const std::string& id) -> void {
        if (active.count(id)) {
            reject(report, "package_dependency_cycle", id, "cycle in explicit package graph"); return;
        }
        if (!visited.insert(id).second) return;
        if (active.size() >= 32) { reject(report, "package_graph_limit", id, "package depth must be <32"); return; }
        const auto* package = load(id);
        if (!package) return;
        active.insert(id);
        for (const auto& dep : package->dependencies) {
            const Key key{id, dep.name};
            Dependency edge{id, dep.name, "", "missing_package_binding", dep.publisher, "appx_package"};
            const auto binding = bindings.find(key);
            if (binding == bindings.end())
                reject(report, edge.status, id + ":" + dep.name, "no name/version/host fallback permitted");
            else {
                used.insert(key);
                edge.provider = binding->second->provider;
                edge.status = "package_binding_rejected";
                if (!ambiguous.count(key)) {
                    const auto* provider = load(edge.provider);
                    if (provider && provider->name == dep.name && provider->publisher == dep.publisher &&
                        provider->version >= dep.min_version &&
                        (provider->architecture == package->architecture || provider->architecture == 11)) {
                        edge.status = "pinned_package_identity_verified";
                        self(self, edge.provider);
                    } else reject(report, "package_identity_mismatch", id + ":" + dep.name,
                                  "explicit provider name/publisher/minimum version/architecture mismatch or invalid artifact");
                }
            }
            report.dependencies.push_back(std::move(edge));
        }
        active.erase(id);
    };
    walk(walk, activation.manifest);
    for (const auto& [key, b] : bindings) {
        (void)b;
        if (!used.count(key)) reject(report, "unused_package_binding", key.first + ":" + key.second, "not declared by a verified package");
    }
    const auto root = packages.find(activation.manifest);
    if (root != packages.end()) {
        const auto& package = root->second;
        const auto app = std::find_if(package.applications.begin(), package.applications.end(),
            [&](const auto& a) { return a.id == activation.application_id; });
        if (app == package.applications.end()) reject(report, "application_id_missing", activation.application_id, "no exact application in pinned package");
        else {
            std::filesystem::path relative;
            appmodel::package_relative_path(app->executable, relative);
            const auto expected = (nodes.at(activation.manifest)->artifact.path.parent_path() / relative).lexically_normal();
            const auto entry = std::find_if(report.nodes.begin(), report.nodes.end(), [&](const auto& n) {
                return n.artifact.id == profile.entry && n.status == "inspected_pe" && n.artifact.path.lexically_normal() == expected;
            });
            if (entry == report.nodes.end()) reject(report, "package_executable_mismatch", app->id, "Executable must be the exact pinned entry beneath its package root");
            report.dependencies.push_back({activation.manifest, app->executable, profile.entry,
                entry == report.nodes.end() ? "entry_rejected" : "pinned_application_verified", app->entry_point, "appx_application"});
            if (app->entry_point != "Windows.FullTrustApplication")
                reject(report, "appmodel_entrypoint_unsupported", app->id, "only owned packaged desktop lifecycle contract audited; UWP/WinRT classes need separate validation");
        }
        if (package.architecture != 9 || !package.resource_id.empty() || package.applications.size() != 1)
            reject(report, "appmodel_package_unsupported", activation.manifest, "one x64 application, no resource package required for current backend");
        if (package.device_families != std::vector<std::string>{"Windows.Desktop"})
            reject(report, "appmodel_device_family_unsupported", activation.manifest, "Xbox/Universal/other device contracts have no adapter");
        if (profile.origin != Origin::synthetic)
            reject(report, "original_appmodel_contract_unaudited", activation.manifest, "owned test contract does not authorize original SystemOS activation");
        if (package.capabilities.size() != 1 || package.capabilities[0].name != "runFullTrust" ||
            package.capabilities[0].namespace_uri != "http://schemas.microsoft.com/appx/manifest/foundation/windows10/restrictedcapabilities")
            reject(report, "appmodel_capability_unsupported", activation.manifest, "only runFullTrust for owned desktop test audited");
    }
    for (const auto& [id, manifest] : packages) {
        for (const auto& contract : manifest.unsupported_contracts)
            reject(report, "appmodel_contract_unsupported", id, contract);
        // Static package resolution is useful evidence, but host package-graph
        // selection has not been verified; never activate with implicit versions.
        if (!manifest.dependencies.empty())
            reject(report, "package_graph_activation_unsupported", id, "registered dependency graph verification not implemented; static graph only");
        for (const auto& name : manifest.resource_paths) {
            std::filesystem::path relative;
            appmodel::package_relative_path(name, relative);
            const auto expected = (nodes.at(id)->artifact.path.parent_path() / relative).lexically_normal();
            const auto found = std::find_if(report.nodes.begin(), report.nodes.end(), [&](const auto& node) {
                return node.status == "identity_verified" && node.artifact.role == Role::resource && node.artifact.path.lexically_normal() == expected;
            });
            report.dependencies.push_back({id, name, found == report.nodes.end() ? "" : found->artifact.id,
                found == report.nodes.end() ? "missing_pinned_resource" : "pinned_resource_verified", "", "appx_resource"});
            if (found == report.nodes.end()) reject(report, "missing_pinned_resource", id + ":" + name, "package asset needs version, SHA-256 and provenance");
        }
        report.packages.push_back({id, manifest});
    }
}
} // namespace xboxone::systemos
