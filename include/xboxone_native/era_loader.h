// SPDX-License-Identifier: BSD-3-Clause
#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

// In-process GameOS/ERA image loader: the original AMD64 title image is mapped
// into the launcher process and its imports are bound to host implementations
// supplied by a resolver (kernelx, WinRT, D3D11.X, ...). Nothing is executed by
// load(); calling entry() is the caller's decision.
namespace xboxone::era {

struct ImportRequest {
    std::string library; // As written in the import descriptor, original case.
    std::string name;    // Empty when imported by ordinal.
    std::uint16_t ordinal = 0;
};

// Returns the host address bound to the import, or nullptr when no audited
// implementation exists. A nullptr is reported, never replaced by a stub.
using Resolver = std::function<void*(const ImportRequest&)>;

struct LoadResult {
    std::vector<std::string> errors;
    std::vector<ImportRequest> unresolved;
    std::size_t resolved = 0;
};

class Image {
public:
    Image() = default;
    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;
    ~Image();

    // Maps, relocates, binds and protects the image; registers .pdata unwind data.
    // On failure the image is released and result explains every blocker found.
    bool load(const std::vector<std::uint8_t>& file, const Resolver& resolver, LoadResult& result);
    // Runs the title's TLS callbacks for DLL_PROCESS_ATTACH on the calling thread.
    // Static TLS data is bound by load(); thread attach/detach is forwarded by
    // xone-era-tls.dll, which must sit next to the launcher executable.
    void process_attach() const;
    void* entry() const { return entry_; }
    std::uint8_t* base() const { return base_; }
    std::size_t size() const { return size_; }

private:
    void release();
    std::uint8_t* base_ = nullptr;
    std::size_t size_ = 0;
    void* entry_ = nullptr;
    void* function_table_ = nullptr;
    void* tls_callbacks_ = nullptr;
    void* tls_unbind_ = nullptr;
};

// kernelx.dll contract. Exports whose console semantics match the same-named
// Win32 API are forwarded; GetModuleHandleW(NULL) names the title image;
// title-memory, storage and Xbox-specific exports stay unresolved.
void* resolve_kernelx(const ImportRequest& request);
// d3d11_x.dll: D3D11.X objects with the title-visible Xbox layout over D3D12.
// Unimplemented vtable slots call the trap handler (interface, slot); without
// one they report and fail fast. A handler exists for tests only.
using D3D11XTrapHandler = std::uintptr_t (*)(const char* interface_name, unsigned slot);
void* resolve_d3d11x(const ImportRequest& request);
void set_d3d11x_trap_handler(D3D11XTrapHandler handler);
// EtwPlus.dll telemetry: offline semantics, events dropped, nothing uploaded.
void* resolve_etwplus(const ImportRequest& request);
// Writable title storage root for file mutation APIs (DeleteFileW). Empty (the
// default) means no writable storage: mutations fail with ERROR_ACCESS_DENIED,
// as for the read-only package. Paths outside the root fail the same way.
void set_title_storage_root(const std::wstring& root);
// PC builds with the title's ABI (VS2015+ CRT, vccorlib140, combase), System32 only.
void* resolve_host_module(const ImportRequest& request);
// Full title resolver: kernelx first, then audited host modules.
void* resolve_title_import(const ImportRequest& request);
// Records the mapped title image (one per process); set by Image::load.
void set_title_module(void* base);
bool same_library(std::string_view a, std::string_view b); // ASCII case-insensitive.

} // namespace xboxone::era
