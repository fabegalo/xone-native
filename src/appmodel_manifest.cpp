// SPDX-License-Identifier: BSD-3-Clause
#include "xboxone_native/appmodel.h"
#include "xboxone_native/inventory.h"
#include "sha256.h"
#include "file_path.h"
#include "appmodel_windows.h"
#include <algorithm>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <string_view>
#ifdef _WIN32
#include <appxpackaging.h>
#include <shlwapi.h>
#include <xmllite.h>
#endif

namespace xboxone::appmodel {
bool package_relative_path(const std::string& input, std::filesystem::path& result) {
    result.clear();
    if (input.empty() || input.size() > 1024) return false;
    auto name = input;
    std::replace(name.begin(), name.end(), '\\', '/');
    std::size_t start = 0;
    while (start <= name.size()) {
        auto end = name.find('/', start);
        if (end == std::string::npos) end = name.size();
        auto part = name.substr(start, end - start);
        if (part.empty() || part == "." || part == ".." || part.back() == '.' || part.back() == ' ' ||
            std::any_of(part.begin(), part.end(), [](unsigned char c) {
                return c < 32 || c == 127 || std::string_view(":<>\"|?*%").find(static_cast<char>(c)) != std::string_view::npos;
            })) return false;
        auto base = part.substr(0, part.find('.'));
        for (auto& c : base) if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 32);
        while (!base.empty() && base.back() == ' ') base.pop_back();
        if (base == "CON" || base == "PRN" || base == "AUX" || base == "NUL" ||
            ((base.starts_with("COM") || base.starts_with("LPT")) &&
             ((base.size() == 4 && base[3] >= '0' && base[3] <= '9') ||
              base.substr(3) == "\xc2\xb9" || base.substr(3) == "\xc2\xb2" || base.substr(3) == "\xc2\xb3"))) return false;
        if (end == name.size()) break;
        start = end + 1;
    }
    result = std::filesystem::path(std::u8string(name.begin(), name.end()));
    return !result.is_absolute() && !result.has_root_path();
}

#ifdef _WIN32
namespace {
using namespace windows;
constexpr auto foundation = L"http://schemas.microsoft.com/appx/manifest/foundation/windows10";
constexpr auto uap = L"http://schemas.microsoft.com/appx/manifest/uap/windows10";
constexpr auto restricted = L"http://schemas.microsoft.com/appx/manifest/foundation/windows10/restrictedcapabilities";

// A security pass precedes the SDK schema reader over the SAME immutable bytes.
// Unknown/ignorable extensions are visible blockers, never silently authorized.
void xml_contracts(IStream* stream, Manifest& manifest) {
    Com<IXmlReader> xml;
    require(CreateXmlReader(__uuidof(IXmlReader), reinterpret_cast<void**>(xml.GetAddressOf()), nullptr), "CreateXmlReader");
    require(xml->SetProperty(XmlReaderProperty_DtdProcessing, DtdProcessing_Prohibit), "prohibit DTD");
    require(xml->SetProperty(XmlReaderProperty_XmlResolver, 0), "disable XML resolver");
    require(xml->SetProperty(XmlReaderProperty_MaxElementDepth, 32), "XML depth limit");
    require(xml->SetProperty(XmlReaderProperty_MaxEntityExpansion, 1024), "XML entity limit");
    require(xml->SetInput(stream), "XML input");
    const std::set<std::wstring> ordinary{L"Package", L"Identity", L"Properties", L"DisplayName", L"PublisherDisplayName",
        L"Logo", L"Description", L"Framework", L"Resources", L"Resource", L"Dependencies", L"TargetDeviceFamily",
        L"PackageDependency", L"Applications", L"Application", L"Capabilities", L"Capability"};
    const std::set<std::wstring> paths{L"Logo", L"Square150x150Logo", L"Square44x44Logo", L"Wide310x150Logo", L"Square310x310Logo", L"Square71x71Logo"};
    // Only these attributes have an audited interpretation for activation.
    const std::map<std::wstring, std::set<std::wstring>> attributes{
        {L"Package", {L"IgnorableNamespaces"}}, {L"Identity", {L"Name",L"Publisher",L"Version",L"ProcessorArchitecture",L"ResourceId"}},
        {L"Application", {L"Id",L"Executable",L"EntryPoint"}}, {L"PackageDependency", {L"Name",L"Publisher",L"MinVersion"}},
        {L"TargetDeviceFamily", {L"Name",L"MinVersion",L"MaxVersionTested"}}, {L"Capability", {L"Name"}},
        {L"Resource", {L"Language"}}, {L"VisualElements", {L"DisplayName",L"Description",L"BackgroundColor",L"Square150x150Logo",L"Square44x44Logo"}}};
    XmlNodeType type{};
    HRESULT hr;
    unsigned nodes = 0;
    bool logo_text = false;
    while ((hr = xml->Read(&type)) == S_OK) {
        if (++nodes > 8192) throw std::runtime_error("XML node limit exceeded");
        if (type == XmlNodeType_ProcessingInstruction || type == XmlNodeType_DocumentType)
            throw std::runtime_error("XML processing instructions/DTD prohibited");
        if (type == XmlNodeType_Element) {
            LPCWSTR raw = nullptr, uri_raw = nullptr;
            require(xml->GetLocalName(&raw, nullptr), "XML name");
            require(xml->GetNamespaceUri(&uri_raw, nullptr), "XML namespace");
            const std::wstring local(raw), uri(uri_raw);
            if (!((uri == foundation && ordinary.count(local)) || (uri == uap && local == L"VisualElements") ||
                  (uri == restricted && local == L"Capability")))
                manifest.unsupported_contracts.push_back(utf8(uri.c_str()) + ":" + utf8(local.c_str()));
            logo_text = local == L"Logo" && uri == foundation;
            unsigned count = 0;
            auto attr = xml->MoveToFirstAttribute();
            while (attr == S_OK) {
                if (++count > 64) throw std::runtime_error("XML attribute limit exceeded");
                LPCWSTR an = nullptr, au = nullptr, value = nullptr;
                require(xml->GetLocalName(&an, nullptr), "attribute name");
                require(xml->GetNamespaceUri(&au, nullptr), "attribute namespace");
                require(xml->GetValue(&value, nullptr), "attribute value");
                const auto allowed = attributes.find(local);
                if (std::wstring_view(au) != L"http://www.w3.org/2000/xmlns/" &&
                    (*au || allowed == attributes.end() || !allowed->second.count(an)))
                    manifest.unsupported_contracts.push_back(utf8(local.c_str()) + "@" + utf8(au) + ":" + utf8(an));
                if (paths.count(an)) manifest.resource_paths.push_back(utf8(value));
                if (local == L"Capability" && std::wstring_view(an) == L"Name")
                    manifest.capabilities.push_back({utf8(value), utf8(uri.c_str())});
                attr = xml->MoveToNextAttribute();
            }
            require(attr, "XML attributes");
            xml->MoveToElement();
        } else if (type == XmlNodeType_Text && logo_text) {
            LPCWSTR value = nullptr;
            require(xml->GetValue(&value, nullptr), "Logo text");
            manifest.resource_paths.push_back(utf8(value));
        } else if (type == XmlNodeType_EndElement) logo_text = false;
    }
    require(hr, "XML parse");
    LARGE_INTEGER zero{};
    require(stream->Seek(zero, STREAM_SEEK_SET, nullptr), "rewind manifest snapshot");
}
}
#endif

bool read_manifest(const std::filesystem::path& path, const std::string& sha256, Manifest& output, std::string& error) {
    output = {}; error.clear();
#ifndef _WIN32
    (void)path; (void)sha256;
    error = "Appx manifest SDK reader unavailable on this host (Windows required)";
    return false;
#else
    try {
        std::string hash;
        if (!inventory::fingerprint(path, hash, error, 1024 * 1024) || hash != sha256)
            throw std::runtime_error("manifest path/hash rejected: " + error);
        std::ifstream input(detail::file_path(path), std::ios::binary);
        std::vector<std::uint8_t> bytes(1024 * 1024 + 1);
        input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
        const auto size = input.gcount();
        if (input.bad() || size <= 0 || size > 1024 * 1024) throw std::runtime_error("manifest read limit/error");
        bytes.resize(static_cast<std::size_t>(size));
        detail::Sha256 snapshot;
        snapshot.update(bytes);
        if (snapshot.finish() != sha256) throw std::runtime_error("manifest snapshot SHA-256 mismatch");
        Apartment apartment;
        Com<IStream> stream;
        stream.Attach(SHCreateMemStream(bytes.data(), static_cast<UINT>(bytes.size())));
        if (!stream) throw std::runtime_error("manifest snapshot stream allocation failed");
        Manifest candidate;
        xml_contracts(stream.Get(), candidate);
        Com<IAppxFactory> factory;
        require(CoCreateInstance(__uuidof(AppxFactory), nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)), "AppxFactory");
        Com<IAppxManifestReader> reader;
        require(factory->CreateManifestReader(stream.Get(), &reader), "CreateManifestReader");
        Com<IAppxManifestPackageId> id;
        require(reader->GetPackageId(&id), "GetPackageId");
        candidate.name = string_value([&](auto p) { return id->GetName(p); });
        candidate.publisher = string_value([&](auto p) { return id->GetPublisher(p); });
        candidate.resource_id = string_value([&](auto p) { return id->GetResourceId(p); });
        candidate.full_name = string_value([&](auto p) { return id->GetPackageFullName(p); });
        candidate.family_name = string_value([&](auto p) { return id->GetPackageFamilyName(p); });
        require(id->GetVersion(&candidate.version), "GetVersion");
        APPX_PACKAGE_ARCHITECTURE arch{};
        require(id->GetArchitecture(&arch), "GetArchitecture");
        candidate.architecture = static_cast<std::uint32_t>(arch);
        Com<IAppxManifestApplicationsEnumerator> applications;
        require(reader->GetApplications(&applications), "GetApplications");
        each(applications.Get(), [&](auto e) {
            Com<IAppxManifestApplication> app;
            require(e->GetCurrent(&app), "application");
            Application a;
            a.id = string_value([&](auto p) { return app->GetStringValue(L"Id", p); });
            a.executable = string_value([&](auto p) { return app->GetStringValue(L"Executable", p); });
            a.entry_point = string_value([&](auto p) { return app->GetStringValue(L"EntryPoint", p); });
            a.aumid = string_value([&](auto p) { return app->GetAppUserModelId(p); });
            candidate.applications.push_back(std::move(a));
        });
        Com<IAppxManifestPackageDependenciesEnumerator> dependencies;
        require(reader->GetPackageDependencies(&dependencies), "GetPackageDependencies");
        each(dependencies.Get(), [&](auto e) {
            Com<IAppxManifestPackageDependency> dep;
            require(e->GetCurrent(&dep), "dependency");
            PackageDependency d;
            d.name = string_value([&](auto p) { return dep->GetName(p); });
            d.publisher = string_value([&](auto p) { return dep->GetPublisher(p); });
            require(dep->GetMinVersion(&d.min_version), "GetMinVersion");
            candidate.dependencies.push_back(std::move(d));
        });
        Com<IAppxManifestReader3> modern;
        require(reader.As(&modern), "IAppxManifestReader3");
        Com<IAppxManifestTargetDeviceFamiliesEnumerator> families;
        const HRESULT families_hr=modern->GetTargetDeviceFamilies(&families);
        // Appx 2010 uses Prerequisites instead. Absence is metadata, not desktop activation permission.
        if(families_hr!=HRESULT_FROM_WIN32(ERROR_NOT_FOUND)) require(families_hr,"GetTargetDeviceFamilies");
        if(families) each(families.Get(), [&](auto e) {
            Com<IAppxManifestTargetDeviceFamily> family;
            require(e->GetCurrent(&family), "device family");
            candidate.device_families.push_back(string_value([&](auto p) { return family->GetName(p); }));
        });
        std::set<std::string> ids, deps;
        for (const auto& a : candidate.applications) {
            std::filesystem::path relative;
            if (!ids.insert(a.id).second || !package_relative_path(a.executable, relative))
                throw std::runtime_error("ambiguous application or unsafe Executable path");
        }
        for (const auto& d : candidate.dependencies) if (!deps.insert(d.name).second)
            throw std::runtime_error("ambiguous package dependency");
        for (const auto& p : candidate.resource_paths) {
            std::filesystem::path relative;
            if (!package_relative_path(p, relative)) throw std::runtime_error("unsafe package resource path");
        }
        std::sort(candidate.resource_paths.begin(), candidate.resource_paths.end());
        candidate.resource_paths.erase(std::unique(candidate.resource_paths.begin(), candidate.resource_paths.end()), candidate.resource_paths.end());
        std::sort(candidate.unsupported_contracts.begin(), candidate.unsupported_contracts.end());
        output = std::move(candidate);
        return true;
    } catch (const std::exception& exception) { error = exception.what(); return false; }
#endif
}
} // namespace xboxone::appmodel
