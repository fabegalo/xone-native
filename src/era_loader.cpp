// SPDX-License-Identifier: BSD-3-Clause
#include "xboxone_native/era_loader.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "era_tls_slot.h"
#include "era_audio.h"

#include <cstring>
#include <algorithm>
#include <string>

namespace xboxone::era {
namespace {

constexpr std::size_t max_image = std::size_t{1} << 30;
constexpr std::size_t max_imports = 1u << 16;

template <class T>
bool within(std::size_t offset, std::size_t count, std::size_t limit) {
    return offset <= limit && count <= (limit - offset) / sizeof(T);
}

DWORD protection(DWORD c) {
    const bool x = c & IMAGE_SCN_MEM_EXECUTE, r = c & IMAGE_SCN_MEM_READ, w = c & IMAGE_SCN_MEM_WRITE;
    if (x) return w ? PAGE_EXECUTE_READWRITE : (r ? PAGE_EXECUTE_READ : PAGE_EXECUTE);
    if (w) return PAGE_READWRITE;
    return r ? PAGE_READONLY : PAGE_NOACCESS;
}

// Returns a NUL-terminated string at rva inside the mapped image, or nullptr.
const char* image_string(const std::uint8_t* base, std::size_t size, DWORD rva) {
    if (rva == 0 || rva >= size) return nullptr;
    const auto* s = reinterpret_cast<const char*>(base + rva);
    return std::memchr(s, 0, size - rva) ? s : nullptr;
}

} // namespace

Image::~Image() { release(); }

void Image::process_attach() const {
    for (auto* cb = static_cast<PIMAGE_TLS_CALLBACK*>(tls_callbacks_); cb && *cb; ++cb)
        (*cb)(base_, DLL_PROCESS_ATTACH, nullptr);
}
bool Image::attach_module() {
    if (!base_ || kind_ != ImageKind::module) return false;
    if (attached_) return true;
    using DllEntry = BOOL (WINAPI*)(HINSTANCE, DWORD, LPVOID);
    if (entry_ && !reinterpret_cast<DllEntry>(entry_)(reinterpret_cast<HINSTANCE>(base_), DLL_PROCESS_ATTACH, nullptr)) return false;
    attached_ = true;
    return true;
}

void* Image::find_export(std::string_view name, std::uint16_t ordinal) const {
    if (!base_) return nullptr;
    DWORD rva = 0;
    if (name.empty()) {
        if (ordinal < export_base_ || ordinal - export_base_ >= export_rvas_.size()) return nullptr;
        rva = export_rvas_[ordinal - export_base_];
    } else {
        for (const auto& item : named_exports_) if (item.first == name) { rva = item.second; break; }
    }
    return rva ? base_ + rva : nullptr;
}

void Image::release() {
    if (attached_ && entry_)
        reinterpret_cast<BOOL (WINAPI*)(HINSTANCE, DWORD, LPVOID)>(entry_)(reinterpret_cast<HINSTANCE>(base_), DLL_PROCESS_DETACH, nullptr);
    attached_ = false;
    if (base_ && kind_ == ImageKind::title) reset_acp_heaps();
    if (tls_unbind_) reinterpret_cast<EraTlsUnbind>(tls_unbind_)();
    tls_unbind_ = nullptr; tls_callbacks_ = nullptr;
    if (function_table_) RtlDeleteFunctionTable(static_cast<PRUNTIME_FUNCTION>(function_table_));
    if (base_) { if (kind_ == ImageKind::title) set_title_module(nullptr); VirtualFree(base_, 0, MEM_RELEASE); }
    base_ = nullptr; size_ = 0; entry_ = nullptr; function_table_ = nullptr;
    export_rvas_.clear(); named_exports_.clear(); export_base_ = 0;
}

bool Image::load(const std::vector<std::uint8_t>& file, const Resolver& resolver, LoadResult& result, ImageKind kind) {
    result = {};
    release();
    kind_ = kind;
    const auto fail = [&](std::string error) { result.errors.push_back(std::move(error)); release(); return false; };

    if (file.size() < sizeof(IMAGE_DOS_HEADER)) return fail("truncated DOS header");
    IMAGE_DOS_HEADER dos;
    std::memcpy(&dos, file.data(), sizeof dos);
    if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew < 0 ||
        !within<IMAGE_NT_HEADERS64>(static_cast<std::size_t>(dos.e_lfanew), 1, file.size()))
        return fail("invalid DOS header");
    IMAGE_NT_HEADERS64 nt;
    std::memcpy(&nt, file.data() + dos.e_lfanew, sizeof nt);
    const auto& opt = nt.OptionalHeader;
    if (nt.Signature != IMAGE_NT_SIGNATURE || nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        opt.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return fail("not a PE32+ AMD64 image");
    const bool dll = (nt.FileHeader.Characteristics & IMAGE_FILE_DLL) != 0;
    if (dll != (kind == ImageKind::module))
        return fail(dll ? "DLL images are not title entry images" : "module binding requires a DLL image");
    if (opt.SizeOfImage == 0 || opt.SizeOfImage > max_image || opt.SizeOfHeaders > opt.SizeOfImage ||
        opt.SizeOfHeaders > file.size() || opt.NumberOfRvaAndSizes < IMAGE_DIRECTORY_ENTRY_TLS + 1)
        return fail("invalid image or header size");
    const std::size_t sections_at = static_cast<std::size_t>(dos.e_lfanew) + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) +
                                    nt.FileHeader.SizeOfOptionalHeader;
    const std::size_t count = nt.FileHeader.NumberOfSections;
    if (count == 0 || count > 96 || !within<IMAGE_SECTION_HEADER>(sections_at, count, opt.SizeOfHeaders))
        return fail("invalid section table");
    const auto& dirs = opt.DataDirectory;
    if (dirs[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT].VirtualAddress)
        return fail("delay-load imports not implemented");
    if (dirs[IMAGE_DIRECTORY_ENTRY_COM_DESCRIPTOR].VirtualAddress)
        return fail("CLR images not supported");
    if (kind == ImageKind::module && dirs[IMAGE_DIRECTORY_ENTRY_TLS].VirtualAddress)
        return fail("module static TLS is not implemented");

    size_ = opt.SizeOfImage;
    base_ = static_cast<std::uint8_t*>(VirtualAlloc(nullptr, size_, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
    if (!base_) return fail("image reservation failed: " + std::to_string(GetLastError()));
    std::memcpy(base_, file.data(), opt.SizeOfHeaders);
    std::vector<IMAGE_SECTION_HEADER> sections(count);
    std::memcpy(sections.data(), file.data() + sections_at, count * sizeof(IMAGE_SECTION_HEADER));
    for (const auto& s : sections) {
        const std::size_t span = (std::max)(s.Misc.VirtualSize, s.SizeOfRawData);
        if (s.VirtualAddress < opt.SizeOfHeaders || s.VirtualAddress > size_ || span > size_ - s.VirtualAddress)
            return fail("section outside image");
        const std::size_t raw = (std::min)(s.SizeOfRawData, s.Misc.VirtualSize ? s.Misc.VirtualSize : s.SizeOfRawData);
        if (raw && (s.PointerToRawData > file.size() || raw > file.size() - s.PointerToRawData))
            return fail("section raw data outside file");
        if (raw) std::memcpy(base_ + s.VirtualAddress, file.data() + s.PointerToRawData, raw);
    }
    const auto dir = [&](int index, std::size_t& rva, std::size_t& bytes) {
        rva = dirs[index].VirtualAddress; bytes = dirs[index].Size;
        return rva == 0 || (rva <= size_ && bytes <= size_ - rva);
    };

    // Base relocations: the title is never assumed to own its preferred address.
    const auto delta = static_cast<std::int64_t>(reinterpret_cast<std::uintptr_t>(base_) - opt.ImageBase);
    std::size_t rva = 0, bytes = 0;
    if (!dir(IMAGE_DIRECTORY_ENTRY_BASERELOC, rva, bytes)) return fail("relocation directory outside image");
    if (delta && (rva == 0 || (nt.FileHeader.Characteristics & IMAGE_FILE_RELOCS_STRIPPED)))
        return fail("image has no relocations and cannot be placed at its preferred base");
    for (std::size_t at = rva, end = rva + bytes; delta && at < end;) {
        IMAGE_BASE_RELOCATION block;
        if (end - at < sizeof block) return fail("truncated relocation block");
        std::memcpy(&block, base_ + at, sizeof block);
        if (block.SizeOfBlock < sizeof block || block.SizeOfBlock > end - at || (block.SizeOfBlock & 1))
            return fail("invalid relocation block size");
        for (std::size_t i = sizeof block; i < block.SizeOfBlock; i += 2) {
            std::uint16_t entry;
            std::memcpy(&entry, base_ + at + i, 2);
            const unsigned type = entry >> 12;
            const std::size_t target = static_cast<std::size_t>(block.VirtualAddress) + (entry & 0xfff);
            if (type == IMAGE_REL_BASED_ABSOLUTE) continue;
            if (type != IMAGE_REL_BASED_DIR64) return fail("unsupported relocation type " + std::to_string(type));
            if (target > size_ - 8) return fail("relocation target outside image");
            std::uint64_t value;
            std::memcpy(&value, base_ + target, 8);
            value += static_cast<std::uint64_t>(delta);
            std::memcpy(base_ + target, &value, 8);
        }
        at += block.SizeOfBlock;
    }

    // Imports: collect every unresolved symbol before failing.
    if (!dir(IMAGE_DIRECTORY_ENTRY_IMPORT, rva, bytes)) return fail("import directory outside image");
    std::size_t total = 0;
    for (std::size_t at = rva; rva; at += sizeof(IMAGE_IMPORT_DESCRIPTOR)) {
        if (at > size_ - sizeof(IMAGE_IMPORT_DESCRIPTOR)) return fail("unterminated import directory");
        IMAGE_IMPORT_DESCRIPTOR d;
        std::memcpy(&d, base_ + at, sizeof d);
        if (d.Name == 0 && d.FirstThunk == 0) break;
        const char* library = image_string(base_, size_, d.Name);
        if (!library) return fail("invalid import library name");
        const DWORD lookup = d.OriginalFirstThunk ? d.OriginalFirstThunk : d.FirstThunk;
        for (std::size_t i = 0;; ++i) {
            const std::size_t slot = static_cast<std::size_t>(d.FirstThunk) + i * 8, from = lookup + i * 8;
            if (slot > size_ - 8 || from > size_ - 8 || ++total > max_imports) return fail("invalid import thunk table");
            std::uint64_t thunk;
            std::memcpy(&thunk, base_ + from, 8);
            if (thunk == 0) break;
            ImportRequest request{library, {}, 0};
            if (thunk & IMAGE_ORDINAL_FLAG64) {
                if (thunk & ~(IMAGE_ORDINAL_FLAG64 | 0xffffull)) return fail("invalid import ordinal");
                request.ordinal = static_cast<std::uint16_t>(thunk & 0xffff);
            } else {
                if (thunk > size_ - sizeof(WORD)) return fail("import name outside image");
                const char* name = image_string(base_, size_, static_cast<DWORD>(thunk + sizeof(WORD)));
                if (!name) return fail("invalid import name");
                request.name = name;
            }
            void* address = resolver ? resolver(request) : nullptr;
            if (!address) { result.unresolved.push_back(std::move(request)); continue; }
            const auto value = reinterpret_cast<std::uint64_t>(address);
            std::memcpy(base_ + slot, &value, 8);
            ++result.resolved;
        }
    }
    if (!result.unresolved.empty())
        return fail(std::to_string(result.unresolved.size()) + " import(s) have no host implementation");

    // Retain validated export metadata before applying section protections.
    if (kind == ImageKind::module) {
        if (!dir(IMAGE_DIRECTORY_ENTRY_EXPORT, rva, bytes)) return fail("export directory outside image");
    } else { rva = 0; bytes = 0; }
    if (rva) {
        if (bytes < sizeof(IMAGE_EXPORT_DIRECTORY)) return fail("truncated export directory");
        IMAGE_EXPORT_DIRECTORY exports;
        std::memcpy(&exports, base_ + rva, sizeof exports);
        if (exports.NumberOfFunctions > max_imports || exports.NumberOfNames > max_imports || exports.Base > 65535 ||
            (exports.NumberOfFunctions && !exports.AddressOfFunctions) ||
            (exports.NumberOfNames && (!exports.AddressOfNames || !exports.AddressOfNameOrdinals)) ||
            (exports.NumberOfFunctions && exports.NumberOfFunctions - 1 > 65535 - exports.Base) ||
            !within<DWORD>(exports.AddressOfFunctions, exports.NumberOfFunctions, size_) ||
            !within<DWORD>(exports.AddressOfNames, exports.NumberOfNames, size_) ||
            !within<WORD>(exports.AddressOfNameOrdinals, exports.NumberOfNames, size_)) return fail("invalid export tables");
        export_base_ = exports.Base;
        export_rvas_.resize(exports.NumberOfFunctions);
        if (exports.NumberOfFunctions) std::memcpy(export_rvas_.data(), base_ + exports.AddressOfFunctions, exports.NumberOfFunctions * sizeof(DWORD));
        for (const auto target : export_rvas_) {
            if (!target) continue;
            if (target >= rva && target - rva < bytes) return fail("forwarded exports are not implemented");
            bool accessible = false;
            for (const auto& section : sections) {
                const auto span = (std::max)(section.Misc.VirtualSize, section.SizeOfRawData);
                if (target >= section.VirtualAddress && target - section.VirtualAddress < span &&
                    (section.Characteristics & (IMAGE_SCN_MEM_READ | IMAGE_SCN_MEM_EXECUTE))) accessible = true;
            }
            if (!accessible) return fail("export target outside accessible sections");
        }
        std::size_t name_bytes = 0;
        for (DWORD i = 0; i < exports.NumberOfNames; ++i) {
            DWORD name_rva; WORD index;
            std::memcpy(&name_rva, base_ + exports.AddressOfNames + i * sizeof(DWORD), sizeof name_rva);
            std::memcpy(&index, base_ + exports.AddressOfNameOrdinals + i * sizeof(WORD), sizeof index);
            if (!name_rva || name_rva >= size_ || index >= export_rvas_.size()) return fail("invalid export name/ordinal");
            const char* name = reinterpret_cast<const char*>(base_ + name_rva);
            const char* end = static_cast<const char*>(std::memchr(name, 0, (std::min)(size_ - name_rva, std::size_t{4096})));
            if (!end || end == name || (name_bytes += static_cast<std::size_t>(end - name)) > (1u << 20)) return fail("invalid or oversized export names");
            named_exports_.emplace_back(std::string(name, end), export_rvas_[index]);
        }
    }

    // Unwind data is required for exceptions, stack walks and C++ EH in title code.
    if (!dir(IMAGE_DIRECTORY_ENTRY_EXCEPTION, rva, bytes)) return fail("exception directory outside image");
    if (rva && bytes >= sizeof(RUNTIME_FUNCTION)) {
        auto* table = reinterpret_cast<PRUNTIME_FUNCTION>(base_ + rva);
        if (!RtlAddFunctionTable(table, static_cast<DWORD>(bytes / sizeof(RUNTIME_FUNCTION)),
                                 reinterpret_cast<DWORD64>(base_)))
            return fail("unwind table registration failed");
        function_table_ = table;
    }

    // Static TLS: VAs in the directory are already relocated to this mapping.
    if (!dir(IMAGE_DIRECTORY_ENTRY_TLS, rva, bytes)) return fail("TLS directory outside image");
    if (rva) {
        IMAGE_TLS_DIRECTORY64 tls;
        if (bytes < sizeof tls) return fail("truncated TLS directory");
        std::memcpy(&tls, base_ + rva, sizeof tls);
        const auto start = reinterpret_cast<std::uint64_t>(base_), end = start + size_;
        const auto inside = [&](std::uint64_t va, std::uint64_t n) { return va >= start && va <= end && n <= end - va; };
        const unsigned align = (tls.Characteristics >> 20) & 0xf; // 1 = 1 byte ... 5 = 16 bytes
        if (tls.EndAddressOfRawData < tls.StartAddressOfRawData ||
            !inside(tls.StartAddressOfRawData, tls.EndAddressOfRawData - tls.StartAddressOfRawData) ||
            !inside(tls.AddressOfIndex, sizeof(DWORD)) || (tls.AddressOfCallBacks && !inside(tls.AddressOfCallBacks, 8)))
            return fail("TLS directory references outside image");
        if (align > 5) return fail("TLS alignment above 16 bytes not supported");
        const std::uint64_t needed = tls.EndAddressOfRawData - tls.StartAddressOfRawData + tls.SizeOfZeroFill;
        if (needed > ERA_TLS_MAX_BYTES) return fail("title TLS block above 16 MiB");
        for (std::uint64_t at = tls.AddressOfCallBacks; at; at += 8) {
            if (!inside(at, 8)) return fail("unterminated TLS callback table");
            std::uint64_t cb;
            std::memcpy(&cb, reinterpret_cast<void*>(at), 8);
            if (!cb) break;
            if (!inside(cb, 1)) return fail("TLS callback outside image");
        }
        wchar_t module[MAX_PATH];
        const DWORD n = GetModuleFileNameW(nullptr, module, MAX_PATH);
        if (n == 0 || n == MAX_PATH) return fail("cannot locate launcher directory");
        std::wstring slot_path(module, n);
        slot_path.resize(slot_path.find_last_of(L"\\/") + 1);
        slot_path += L"xone-era-tls.dll";
        const HMODULE slot = LoadLibraryExW(slot_path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        const auto bind = slot ? reinterpret_cast<EraTlsBind>(GetProcAddress(slot, "era_tls_bind")) : nullptr;
        if (!bind) return fail("TLS slot module xone-era-tls.dll unavailable");
        EraTlsTitle title{reinterpret_cast<const void*>(tls.StartAddressOfRawData),
                          static_cast<SIZE_T>(tls.EndAddressOfRawData - tls.StartAddressOfRawData),
                          static_cast<SIZE_T>(needed), reinterpret_cast<PIMAGE_TLS_CALLBACK*>(tls.AddressOfCallBacks), base_};
        DWORD index = 0;
        if (!bind(&title, &index)) return fail("TLS slot already bound to another title in this process");
        tls_unbind_ = reinterpret_cast<void*>(GetProcAddress(slot, "era_tls_unbind"));
        *reinterpret_cast<DWORD*>(tls.AddressOfIndex) = index;
        tls_callbacks_ = reinterpret_cast<void*>(tls.AddressOfCallBacks);
    }

    DWORD old = 0;
    if (!VirtualProtect(base_, opt.SizeOfHeaders, PAGE_READONLY, &old)) return fail("header protection failed");
    for (const auto& s : sections) {
        const std::size_t span = (std::max)(s.Misc.VirtualSize, s.SizeOfRawData);
        if (span && !VirtualProtect(base_ + s.VirtualAddress, span, protection(s.Characteristics), &old))
            return fail("section protection failed");
    }
    FlushInstructionCache(GetCurrentProcess(), base_, size_);
    if (opt.AddressOfEntryPoint >= size_) return fail("entry point outside image");
    entry_ = opt.AddressOfEntryPoint ? base_ + opt.AddressOfEntryPoint : nullptr;
    if (kind == ImageKind::title) set_title_module(base_);
    return true;
}

} // namespace xboxone::era
