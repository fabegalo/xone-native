// SPDX-License-Identifier: BSD-3-Clause
// Title-layer WinRT objects with explicit vtables. Slots 0-5 implement
// IInspectable; every other slot traps, reporting runtime class, interface IID
// and slot, then fails fast. Slots are only filled in when the title's real
// calls have been observed, so a guessed method order never runs silently.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <inspectable.h>

#include <array>
#include <cstdint>
#include <mutex>
#include <vector>

namespace xboxone::era {

constexpr std::size_t winrt_slots = 64;

struct WinrtObject;
using WinrtQuery = HRESULT (*)(WinrtObject* self, REFIID iid, void** out);

// Every title-layer interface pointer points at one of these.
struct WinrtObject {
    const void* const* vtable;
    const wchar_t* runtime_class;
    GUID iid;          // Interface this vtable implements.
    WinrtQuery query;  // Resolves other interfaces of the same object.
};

// IInspectable slots 0-5 wired to the object's query/class/iid; the rest trap.
// Lifetime is process-wide for now (AddRef/Release do not free).
std::array<const void*, winrt_slots> winrt_vtable();

// Event registration storage for add_X(handler, token*) / remove_X(token).
class WinrtEvent {
public:
    HRESULT add(IUnknown* handler, std::int64_t* token);
    HRESULT remove(std::int64_t token);
    std::size_t size() const;

private:
    mutable std::mutex mutex_;
    std::vector<std::pair<std::int64_t, IUnknown*>> handlers_;
    std::int64_t next_ = 1;
};

} // namespace xboxone::era
