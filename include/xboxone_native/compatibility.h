// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <string>
#include <string_view>

namespace xboxone::compatibility {

enum class Domain { hostos, systemos, gameos_era, gamecore };
enum class ServiceKind { nt_kernel, xbox_device, xbox_broker, winrt_activation, compositor, storage };

struct ServiceRequirement {
    std::string id;
    ServiceKind kind = ServiceKind::xbox_broker;
    std::string evidence; // Observed dependency, not an inferred implementation.
};

struct ServiceDiagnostic {
    std::string code;
    std::string detail;
};

bool parse_service_kind(std::string_view name, ServiceKind& kind);
// No Xbox service providers are implemented. Keep every requirement blocked;
// a kernel/device dependency alone does not select HLE, firmware or a VM.
ServiceDiagnostic service_blocker(Domain domain, const ServiceRequirement& requirement);

} // namespace xboxone::compatibility
