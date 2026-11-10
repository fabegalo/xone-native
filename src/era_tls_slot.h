// SPDX-License-Identifier: BSD-3-Clause
// Private ABI between the ERA loader and xone-era-tls.dll.
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#define ERA_TLS_MAX_BYTES (16u << 20)

struct EraTlsTitle {
    const void* raw;                 // Title TLS template (StartAddressOfRawData).
    SIZE_T raw_size;                 // Template bytes.
    SIZE_T size;                     // Template + SizeOfZeroFill.
    PIMAGE_TLS_CALLBACK* callbacks;  // Relocated, NULL-terminated, may be NULL.
    void* image;                     // Title image base passed to callbacks.
};

using EraTlsBind = BOOL(*)(const EraTlsTitle*, DWORD*);
using EraTlsUnbind = void(*)();
