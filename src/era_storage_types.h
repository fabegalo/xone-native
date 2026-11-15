// SPDX-License-Identifier: BSD-3-Clause
// Public ERA value-type ABI, projected through the installed Windows SDK's
// standard collection/async implementations. No reference implementation code.
#pragma once
#include <winrt/Windows.Foundation.h>
namespace winrt::Windows::Xbox::Storage {
struct ContainerInfo {
    hstring Name;
    std::uint32_t TotalSize=0;
    bool operator==(const ContainerInfo&)const=default;
};
}
namespace winrt::impl {
template<>struct category<Windows::Xbox::Storage::ContainerInfo>{using type=struct_category<hstring,std::uint32_t>;};
template<>inline constexpr auto& name_v<Windows::Xbox::Storage::ContainerInfo> = L"Windows.Xbox.Storage.ContainerInfo";
template<>struct abi<Windows::Xbox::Storage::ContainerInfo>{struct type{void* Name;std::uint32_t TotalSize;};};
}
