// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <Windows.h>
#include <cstdint>
#include <vector>

namespace xboxone::era {
// Reference 1405a07a0 / 140494bc0: arithmetic compatibility before native submission.
HRESULT translate_reference_shader(std::vector<std::uint8_t>& code, const char* creator);
}
