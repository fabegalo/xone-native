// SPDX-License-Identifier: BSD-3-Clause
// Windows.Xbox.System.User statics. IUserStatics IID observed from the title's
// own QueryInterface. Only slots whose calls were observed are implemented;
// the order hypothesis comes from WinDurango's public IDL (IInspectable 0-5,
// then OnlineState 6, OnlineStateChanged 7/8, Users 9, UserAdded 10/11,
// UserRemoved 12/13, ...). Everything else traps with its slot number.
//
// Observed (Sonic Mania 1.0.4.2): slot 12 called first with (handler, token*),
// then slot 10.
// Event registration alone does not establish a local user. Enumeration,
// profile identity and sign-in state remain explicit implementation gaps.
#include "era_winrt_classes.h"
#include "era_winrt_object.h"

#include <activation.h>

namespace xboxone::era {
namespace {

constexpr wchar_t class_name[] = L"Windows.Xbox.System.User";
constexpr GUID iid_user_statics{0xB4F23A93, 0x7FC6, 0x4A50, {0xB1, 0x53, 0x95, 0x00, 0x16, 0x38, 0x9E, 0x50}};

WinrtEvent user_added;
WinrtEvent user_removed;

HRESULT STDMETHODCALLTYPE add_UserAdded(WinrtObject*, IUnknown* handler, std::int64_t* token) {
    return user_added.add(handler, token);
}
HRESULT STDMETHODCALLTYPE remove_UserAdded(WinrtObject*, std::int64_t token) { return user_added.remove(token); }

HRESULT STDMETHODCALLTYPE add_UserRemoved(WinrtObject*, IUnknown* handler, std::int64_t* token) {
    return user_removed.add(handler, token);
}
HRESULT STDMETHODCALLTYPE remove_UserRemoved(WinrtObject*, std::int64_t token) { return user_removed.remove(token); }

struct UserFactory {
    WinrtObject activation;
    WinrtObject statics;
};
UserFactory& factory();

HRESULT query(WinrtObject*, REFIID iid, void** out) {
    if (iid == __uuidof(IUnknown) || iid == __uuidof(IInspectable) || iid == __uuidof(IActivationFactory))
        *out = &factory().activation;
    else if (iid == iid_user_statics)
        *out = &factory().statics;
    else return E_NOINTERFACE;
    return S_OK;
}

UserFactory& factory() {
    static const auto activation_vt = winrt_vtable(); // Slot 6 ActivateInstance: not observed, traps.
    static const auto statics_vt = [] {
        auto t = winrt_vtable();
        t[10] = reinterpret_cast<const void*>(&add_UserAdded);
        t[11] = reinterpret_cast<const void*>(&remove_UserAdded);
        t[12] = reinterpret_cast<const void*>(&add_UserRemoved);
        t[13] = reinterpret_cast<const void*>(&remove_UserRemoved);
        return t;
    }();
    static UserFactory instance{{activation_vt.data(), class_name, __uuidof(IActivationFactory), &query},
                                {statics_vt.data(), class_name, iid_user_statics, &query}};
    return instance;
}

} // namespace

HRESULT xbox_user_factory(REFIID iid, void** out) {
    if (!out) return E_POINTER;
    *out = nullptr;
    return query(nullptr, iid, out);
}

std::size_t xbox_user_removed_handlers() { return user_removed.size(); }

} // namespace xboxone::era
