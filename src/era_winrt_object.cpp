// SPDX-License-Identifier: BSD-3-Clause
#include "era_winrt_object.h"

#include <winstring.h>

#include <cstdio>
#include <utility>

namespace xboxone::era {
namespace {

[[noreturn]] void trap(const WinrtObject* self, unsigned slot) {
    wchar_t iid[40] = L"?";
    StringFromGUID2(self->iid, iid, 40);
    char line[300];
    std::snprintf(line, sizeof line, "xone-era: WinRT %ls interface %ls vtable slot %u is not implemented\n",
                  self->runtime_class, iid, slot);
    OutputDebugStringA(line);
    std::fputs(line, stderr);
    std::fflush(stderr);
    __fastfail(FAST_FAIL_FATAL_APP_EXIT); // Never continue with a fabricated result.
}

template <unsigned Slot>
std::uintptr_t trap_slot(WinrtObject* self) {
    trap(self, Slot);
}

template <std::size_t... I>
std::array<const void*, winrt_slots> traps(std::index_sequence<I...>) {
    return {reinterpret_cast<const void*>(&trap_slot<static_cast<unsigned>(I)>)...};
}

HRESULT STDMETHODCALLTYPE query(WinrtObject* self, REFIID iid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (iid == self->iid) { *out = self; return S_OK; }
    return self->query ? self->query(self, iid, out) : E_NOINTERFACE;
}
ULONG STDMETHODCALLTYPE add_ref(WinrtObject*) { return 2; }
ULONG STDMETHODCALLTYPE release(WinrtObject*) { return 1; }
HRESULT STDMETHODCALLTYPE get_iids(WinrtObject* self, ULONG* count, IID** iids) {
    if (!count || !iids) return E_POINTER;
    *iids = static_cast<IID*>(CoTaskMemAlloc(sizeof(IID)));
    if (!*iids) return E_OUTOFMEMORY;
    **iids = self->iid;
    *count = 1;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE get_class(WinrtObject* self, HSTRING* name) {
    if (!name) return E_POINTER;
    return WindowsCreateString(self->runtime_class, static_cast<UINT32>(lstrlenW(self->runtime_class)), name);
}
HRESULT STDMETHODCALLTYPE get_trust(WinrtObject*, TrustLevel* level) {
    if (!level) return E_POINTER;
    *level = BaseTrust;
    return S_OK;
}

} // namespace

std::array<const void*, winrt_slots> winrt_vtable() {
    auto table = traps(std::make_index_sequence<winrt_slots>{});
    table[0] = reinterpret_cast<const void*>(&query);
    table[1] = reinterpret_cast<const void*>(&add_ref);
    table[2] = reinterpret_cast<const void*>(&release);
    table[3] = reinterpret_cast<const void*>(&get_iids);
    table[4] = reinterpret_cast<const void*>(&get_class);
    table[5] = reinterpret_cast<const void*>(&get_trust);
    return table;
}

HRESULT WinrtEvent::add(IUnknown* handler, std::int64_t* token) {
    if (!handler || !token) return E_INVALIDARG;
    std::lock_guard lock(mutex_);
    handler->AddRef();
    *token = next_++;
    handlers_.emplace_back(*token, handler);
    return S_OK;
}

HRESULT WinrtEvent::remove(std::int64_t token) {
    IUnknown* handler = nullptr;
    {
        std::lock_guard lock(mutex_);
        for (auto it = handlers_.begin(); it != handlers_.end(); ++it)
            if (it->first == token) { handler = it->second; handlers_.erase(it); break; }
    }
    if (handler) handler->Release();
    return S_OK; // WinRT remove of an unknown token is not an error.
}

std::size_t WinrtEvent::size() const {
    std::lock_guard lock(mutex_);
    return handlers_.size();
}

} // namespace xboxone::era
