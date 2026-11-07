// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <array>
#include <cstdint>
namespace xboxone::presentation::pipe_protocol {
// Little-endian Windows wire format, no pointers or compiler-dependent structs.
// magic, version, sequence lo/hi, width, height, pitch, format (1=RGBA,2=BGRA),
// payload bytes, reserved. Sender provenance is deliberately absent.
using Header = std::array<std::uint32_t, 10>;
constexpr std::uint32_t magic = 0x46504f58, version = 1;
constexpr std::uint32_t max_bytes = 64 * 1024 * 1024;
} // namespace xboxone::presentation::pipe_protocol
