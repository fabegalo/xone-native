// SPDX-License-Identifier: BSD-3-Clause
#include "xboxone_native/compatibility.h"

namespace xboxone::compatibility {

bool parse_service_kind(std::string_view name, ServiceKind& kind) {
    if (name == "nt_kernel") kind = ServiceKind::nt_kernel;
    else if (name == "xbox_device") kind = ServiceKind::xbox_device;
    else if (name == "xbox_broker") kind = ServiceKind::xbox_broker;
    else if (name == "winrt_activation") kind = ServiceKind::winrt_activation;
    else if (name == "compositor") kind = ServiceKind::compositor;
    else if (name == "storage") kind = ServiceKind::storage;
    else return false;
    return true;
}

ServiceDiagnostic service_blocker(Domain domain, const ServiceRequirement& requirement) {
    const char* name;
    switch (domain) {
        case Domain::hostos: name = "HostOS"; break;
        case Domain::systemos: name = "SystemOS"; break;
        case Domain::gameos_era: name = "GameOS/ERA"; break;
        case Domain::gamecore: name = "GameCore"; break;
        default: return {"invalid_service_domain", "unknown Xbox contract domain"};
    }
    switch (requirement.kind) {
        case ServiceKind::nt_kernel:
        case ServiceKind::xbox_device:
        case ServiceKind::xbox_broker:
        case ServiceKind::winrt_activation:
        case ServiceKind::compositor:
        case ServiceKind::storage:
            return {"service_adapter_missing", std::string(name) +
                " service implementation is missing; execution backend undecided: " + requirement.evidence};
        default: return {"invalid_service_kind", "unknown contract"};
    }
}

} // namespace xboxone::compatibility
