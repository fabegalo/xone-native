// SPDX-License-Identifier: BSD-3-Clause
// EtwPlus.dll: Xbox title telemetry, normally uploaded to Xbox services. There
// is no upload channel here (no fake online connection), so behaviour matches an
// offline console: providers register, events are accepted and dropped locally.
// Signatures follow the interface observed in WinDurango's public headers;
// the descriptor structures are never dereferenced.
#include "xboxone_native/era_loader.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <evntprov.h>

#include <atomic>

namespace xboxone::era {
namespace {

std::atomic<REGHANDLE> next_handle{1};

ULONG WINAPI EtxRegister(const void* provider, PREGHANDLE handle) {
    if (!provider || !handle) return ERROR_INVALID_PARAMETER;
    *handle = next_handle.fetch_add(1);
    return ERROR_SUCCESS;
}

ULONG WINAPI EtxEventWrite(const void* event, const void* provider, REGHANDLE, ULONG count,
                           PEVENT_DATA_DESCRIPTOR data) {
    if (!event || !provider || (count && !data)) return ERROR_INVALID_PARAMETER;
    return ERROR_SUCCESS; // Dropped: no telemetry upload exists.
}

// Common fields (device/session identity) are an empty payload offline.
void WINAPI EtxFillCommonFields_v7(PEVENT_DATA_DESCRIPTOR data, PUINT8, SIZE_T) {
    if (data) *data = {};
}

} // namespace

void* resolve_etwplus(const ImportRequest& request) {
    if (!same_library(request.library, "EtwPlus.dll")) return nullptr;
    if (request.name == "EtxRegister") return reinterpret_cast<void*>(&EtxRegister);
    if (request.name == "EtxEventWrite") return reinterpret_cast<void*>(&EtxEventWrite);
    if (request.name == "EtxFillCommonFields_v7") return reinterpret_cast<void*>(&EtxFillCommonFields_v7);
    return nullptr;
}

} // namespace xboxone::era
