// SPDX-License-Identifier: BSD-3-Clause
#include "era_winrt_object.h"
#include "xboxone_native/era_loader.h"

#include <winstring.h>
#include <bcrypt.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <utility>

namespace xboxone::era {
namespace {

[[noreturn]] void trap(const WinrtObject* self, unsigned slot, std::uintptr_t first, std::uintptr_t second) {
    wchar_t iid[40] = L"?";
    StringFromGUID2(self->iid, iid, 40);
    char line[300];
    std::snprintf(line, sizeof line, "xone-era: WinRT %ls interface %ls vtable slot %u is not implemented\n",
                  self->runtime_class, iid, slot);
    OutputDebugStringA(line);
    std::fputs(line, stderr);
    std::fflush(stderr);
    std::fprintf(stderr, "xone-era: raw WinRT integer args 0x%llX 0x%llX\n",
                 static_cast<unsigned long long>(first), static_cast<unsigned long long>(second));
    std::fflush(stderr);
    if (stop_on_missing_service()) TerminateProcess(GetCurrentProcess(), 125);
    __fastfail(FAST_FAIL_FATAL_APP_EXIT); // Never continue with a fabricated result.
}

template <unsigned Slot>
std::uintptr_t trap_slot(WinrtObject* self, std::uintptr_t first, std::uintptr_t second) {
    trap(self, Slot, first, second);
}

template <std::size_t... I>
std::array<const void*, winrt_slots> traps(std::index_sequence<I...>) {
    return {reinterpret_cast<const void*>(&trap_slot<static_cast<unsigned>(I)>)...};
}

HRESULT STDMETHODCALLTYPE query(WinrtObject* self, REFIID iid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    if (iid == self->iid) { *out = self; return S_OK; }
    const HRESULT hr = self->query ? self->query(self, iid, out) : E_NOINTERFACE;
    if (hr == E_NOINTERFACE) { // Evidence for the next interface the title needs.
        wchar_t requested[40] = L"?";
        StringFromGUID2(iid, requested, 40);
        char line[300];
        std::snprintf(line, sizeof line, "xone-era: WinRT %ls has no interface %ls\n", self->runtime_class, requested);
        std::fputs(line, stderr);
        std::fflush(stderr);
    }
    return hr;
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

HRESULT STDMETHODCALLTYPE view_get_at(WinrtObject*, UINT32, void** item) {
    if (!item) return E_POINTER;
    *item = nullptr;
    return E_BOUNDS;
}
HRESULT STDMETHODCALLTYPE view_size(WinrtObject*, UINT32* size) {
    if (!size) return E_POINTER;
    *size = 0;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE view_index_of(WinrtObject*, void*, UINT32* index, boolean* found) {
    if (!index || !found) return E_POINTER;
    *index = 0;
    *found = false;
    return S_OK;
}
HRESULT STDMETHODCALLTYPE view_get_many(WinrtObject*, UINT32 start, UINT32, void**, UINT32* actual) {
    if (!actual) return E_POINTER;
    *actual = 0;
    return start == 0 ? S_OK : E_BOUNDS;
}
HRESULT view_query(WinrtObject* self, REFIID iid, void** out) {
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IInspectable)) { *out = self; return S_OK; }
    return E_NOINTERFACE;
}

} // namespace

GUID winrt_pinterface_iid(const char* signature) {
    // UUIDv5 (SHA-1) in the WinRT pinterface namespace 11f47ad5-7b73-42c0-abae-878b1e16adee.
    static constexpr std::uint8_t ns[16]{0x11, 0xf4, 0x7a, 0xd5, 0x7b, 0x73, 0x42, 0xc0,
                                         0xab, 0xae, 0x87, 0x8b, 0x1e, 0x16, 0xad, 0xee};
    std::string data(reinterpret_cast<const char*>(ns), sizeof ns);
    data += signature;
    std::uint8_t hash[20]{};
    BCryptHash(BCRYPT_SHA1_ALG_HANDLE, nullptr, 0, reinterpret_cast<PUCHAR>(data.data()),
               static_cast<ULONG>(data.size()), hash, sizeof hash);
    hash[6] = static_cast<std::uint8_t>((hash[6] & 0x0f) | 0x50);
    hash[8] = static_cast<std::uint8_t>((hash[8] & 0x3f) | 0x80);
    GUID g{};
    g.Data1 = (static_cast<unsigned long>(hash[0]) << 24) | (hash[1] << 16) | (hash[2] << 8) | hash[3];
    g.Data2 = static_cast<unsigned short>((hash[4] << 8) | hash[5]);
    g.Data3 = static_cast<unsigned short>((hash[6] << 8) | hash[7]);
    std::memcpy(g.Data4, hash + 8, 8);
    return g;
}

WinrtObject* winrt_empty_vector_view(const wchar_t* runtime_class, const GUID& iid) {
    static const auto vt = [] {
        auto t = winrt_vtable();
        t[6] = reinterpret_cast<const void*>(&view_get_at);
        t[7] = reinterpret_cast<const void*>(&view_size);
        t[8] = reinterpret_cast<const void*>(&view_index_of);
        t[9] = reinterpret_cast<const void*>(&view_get_many);
        return t;
    }();
    // Lives for the process, like every title-layer object for now.
    return new WinrtObject{vt.data(), runtime_class, iid, &view_query};
}

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
    try { handlers_.emplace_back(next_, handler); }
    catch (const std::bad_alloc&) { return E_OUTOFMEMORY; }
    handler->AddRef();
    *token = next_++;
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

std::vector<IUnknown*> WinrtEvent::snapshot() const {
    std::lock_guard lock(mutex_);
    std::vector<IUnknown*> copy;
    copy.reserve(handlers_.size());
    for (const auto& h : handlers_) { h.second->AddRef(); copy.push_back(h.second); }
    return copy;
}

void WinrtEvent::clear() {
    std::vector<std::pair<std::int64_t, IUnknown*>> retired;
    { std::lock_guard lock(mutex_); retired.swap(handlers_); }
    for (const auto& entry : retired) entry.second->Release();
}

std::size_t WinrtEvent::size() const {
    std::lock_guard lock(mutex_);
    return handlers_.size();
}

} // namespace xboxone::era
