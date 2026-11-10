#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Verify pinned OSU ZIPs and plan Xbox One SystemOS inspection, without extraction.

Usage: python tools/systemos_preflight.py OSU1.zip OSU2.zip OSU3.zip
         --reference <previous-full-study.json> --inspector <built-xone-native>
Stdout is JSON. Exit 1 means a valid diagnostic with dashboard blockers; exit 2
means an invalid/mismatched input. This tool never mounts, launches or deletes
content, and never treats an unencrypted header flag as readable executable code.
"""

import argparse
import json
from pathlib import Path, PurePosixPath
import re
import subprocess
import sys
import zipfile

from study_update_zip import digest, study


HEX256 = re.compile(r"[0-9a-f]{64}\Z")
# Hints documented by Xbox One Research, not experimentally mounted here.
SYSTEM_VOLUMES = {
    "system.xvd": (2, "systemos_boot", "C:\\"),
    "systemaux.xvd": (20, "inbox_apps", "X:\\"),
    "systemauxf.xvd": (20, "inbox_apps", "Y:\\"),
    "systemmisc.xvd": (20, "system_boot_resources", "M:\\"),
}
LEGACY_LAYOUT = {
    "host.xvd", "settingstemplate.xvd", "sosinit.xvd", "sostmpl.xvd",
    "system.xvd", "systemaux.xvd", "systemtools.xvd", "updater.xvd",
}


def pinned_source(path, inspector, catalog):
    """Bind the fresh native probes to a previously fully hashed archive.

    Matching is by archive SHA-256, not the current basename. Member hashes are
    retained from the explicitly supplied reference; they are not rehashed here.
    The reference is an identity record, not a Microsoft authenticity certificate.
    """
    before = path.stat()
    with path.open("rb") as stream:
        archive_hash = digest(stream)
    if any(not isinstance(source, dict) for source in catalog["sources"]):
        raise ValueError("malformed reference source")
    matches = [source for source in catalog["sources"] if source.get("archive_sha256") == archive_hash]
    if len(matches) != 1:
        raise ValueError(f"archive has no unique pinned identity: {path.name}")
    reference = matches[0]
    if reference.get("complete") is not True or reference.get("zip_size") != before.st_size:
        raise ValueError("incomplete reference or archive size mismatch")
    if not isinstance(reference.get("source_name"), str):
        raise ValueError("missing reference source name")
    if not isinstance(reference.get("files"), list):
        raise ValueError("missing reference file list")
    fresh = study(path, inspector)
    expected = {}
    for entry in reference["files"]:
        if not isinstance(entry, dict) or not isinstance(entry.get("path"), str):
            raise ValueError("malformed reference member")
        sha256 = entry.get("sha256")
        if entry["path"] in expected or not isinstance(sha256, str) or not HEX256.fullmatch(sha256):
            raise ValueError("duplicate reference member or missing full member SHA-256")
        if entry.get("zip_crc_verified") is not True:
            raise ValueError("reference member was not fully verified")
        expected[entry["path"]] = entry
    if set(expected) != {entry["path"] for entry in fresh["files"]}:
        raise ValueError("archive member set differs from reference")
    for entry in fresh["files"]:
        recorded = expected[entry["path"]]
        for field in ("size", "compressed_size", "zip_crc32", "prefix_bytes", "prefix_sha256", "inspection"):
            if field not in entry or entry[field] != recorded.get(field):
                raise ValueError(f"fresh probe differs from reference: {entry['path']} ({field})")
        entry["sha256"] = recorded["sha256"]
    after = path.stat()
    if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
        raise ValueError("archive changed during identity verification")
    fresh["archive_sha256"] = archive_hash
    fresh["reference_source_name"] = reference["source_name"]
    return fresh


def assess(source):
    """Build one isolated profile; no search/fallback into another package."""
    names = {PurePosixPath(entry["path"]).name.lower() for entry in source["files"]}
    label = source["reference_source_name"].lower()
    if label == "osu1.zip" and {"system.xvd", "host.xvd", "scarletthost.xvd"} <= names:
        family = "xbox_one_and_series_osu1"
    elif label in {"osu2.zip", "osu3.zip"} and names == LEGACY_LAYOUT:
        family = "original_xbox_one_legacy_recovery"
    else:
        family = "unclassified"
    profile = {
        "profile_id": "xbox-one/" + source["archive_sha256"],
        "archive_name": source["source_name"],
        "reference_source_name": source["reference_source_name"],
        "archive_sha256": source["archive_sha256"],
        "archive_identity_verified": True,
        "member_hashes_recomputed": False,
        "member_hash_basis": "previous_full_study_bound_to_verified_archive",
        "publisher_signature_verified": False,
        "target": "xbox-one",
        "package_family": family,
        "classification_basis": "pinned_reference_OSU_label_and_observed_layout; not_vendor_authentication",
        "contains_xbox_one_candidates": True if family != "unclassified" else None,
        "systemos_revision_raw_hex": None,
        "systemos_inputs": [],
        "reference_only": [],
        "excluded_from_systemos": [],
        "blockers": [],
        "can_boot_original_dashboard": False,
    }

    def block(code, members=None):
        item = {"code": code}
        if members is not None:
            item["members"] = members
        profile["blockers"].append(item)

    if family == "unclassified":
        block("package_applicability_unclassified")
    for entry in source["files"]:
        member = PurePosixPath(entry["path"])
        if member.parent != PurePosixPath("$SystemUpdate"):
            block("unexpected_update_member_location", [entry["path"]])
            continue
        image = entry.get("inspection", {})
        if image.get("kind") != "xvd":
            block("unrecognized_update_member", [entry["path"]])
            continue
        name = member.name.lower()
        item = {
            "archive_member": entry["path"], "sha256": entry["sha256"], "size": entry["size"],
            "content_type": image["content_type"],
            "package_version_raw_hex": f"0x{image['package_version_raw']:016x}",
            "encryption": "disabled_by_header" if image["volume_flags"] & 2 else "enabled_by_header",
        }
        if name in SYSTEM_VOLUMES:
            expected_type, role, mount = SYSTEM_VOLUMES[name]
            item.update(role=role, documented_mount_hint=mount)
            profile["systemos_inputs"].append(item)
            if image["content_type"] != expected_type:
                block("component_content_type_mismatch", [entry["path"]])
            if name == "system.xvd":
                profile["systemos_revision_raw_hex"] = item["package_version_raw_hex"]
        elif image["content_type"] == 20:
            # Language/app resources have package-scoped mappings, not an
            # invented drive letter derived from their filename.
            item.update(role="inbox_or_language_resource", documented_mount_hint=None)
            profile["systemos_inputs"].append(item)
        elif name == "host.xvd":
            item["role"] = "xbox_one_host_reference"
            profile["reference_only"].append(item)
            if image["content_type"] != 7:
                block("component_content_type_mismatch", [entry["path"]])
        elif name in {"settingstemplate.xvd", "sosinit.xvd", "sostmpl.xvd"}:
            item["role"] = "settings_or_boot_template_reference"
            profile["reference_only"].append(item)
            if image["content_type"] != 13:
                block("component_content_type_mismatch", [entry["path"]])
        else:
            item["reason"] = (
                "scarlett_host_not_selected_for_xbox_one" if name == "scarletthost.xvd" else
                "gameos_is_a_separate_environment" if image["content_type"] == 3 else
                "developer_utilities_not_retail_shell" if image["content_type"] == 19 else
                "offline_updater_or_hardware_firmware" if image["content_type"] == 12 else
                "unclassified_not_automatically_selected"
            )
            profile["excluded_from_systemos"].append(item)
    revision = profile["systemos_revision_raw_hex"]
    if revision is None:
        block("missing_systemos_boot_volume")
    elif revision == "0x0000000000000000":
        block("systemos_revision_not_identified")
    mismatched = [item["archive_member"] for item in profile["systemos_inputs"]
                  if revision and item["package_version_raw_hex"] != revision]
    if mismatched:
        block("mixed_systemos_revisions", mismatched)
    encrypted = [item["archive_member"] for item in profile["systemos_inputs"]
                 if item["encryption"] == "enabled_by_header"]
    if encrypted:
        block("encrypted_systemos_inputs", encrypted)
    # No filesystem/shell is inferred from an encryption flag or an XVD name.
    block("original_shell_and_matching_resources_not_identified")
    block("pe_winrt_nt_dependency_graph_not_available")
    block("systemos_activation_backend_not_implemented")
    profile["runtime_storage_not_supplied"] = [
        name for name in ("settings-saved.xvd", "user.xvd") if name not in names
    ]
    profile["runtime_storage_note"] = "update_archives_are_not_an_initialized_console_disk; no_empty_volumes_created"
    return profile


def compare_profiles(profiles):
    duplicates = []
    for i, left in enumerate(profiles):
        left_hashes = {item["sha256"] for item in left["systemos_inputs"]}
        for right in profiles[i + 1:]:
            shared = sorted(left_hashes & {item["sha256"] for item in right["systemos_inputs"]})
            if shared:
                duplicates.append({"left": left["profile_id"], "right": right["profile_id"],
                                   "shared_systemos_member_sha256": shared,
                                   "archives_interchangeable": False})
    return duplicates


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archives", nargs="+", type=Path)
    parser.add_argument("--reference", required=True, type=Path)
    parser.add_argument("--inspector", required=True, type=Path)
    args = parser.parse_args()
    try:
        catalog = json.loads(args.reference.read_text(encoding="utf-8-sig"))
        if not isinstance(catalog, dict) or catalog.get("schema_version") != 1 or not isinstance(catalog.get("sources"), list):
            raise ValueError("unsupported reference study schema")
        inspector = args.inspector.resolve(strict=True)
        profiles = []
        identities = set()
        for path in args.archives:
            source = pinned_source(path, inspector, catalog)
            if source["archive_sha256"] in identities:
                raise ValueError("same archive supplied twice, even under different names")
            identities.add(source["archive_sha256"])
            profiles.append(assess(source))
        profiles.sort(key=lambda profile: profile["profile_id"])
    except (OSError, ValueError, KeyError, TypeError, zipfile.BadZipFile, RuntimeError, subprocess.SubprocessError) as error:
        print(f"SystemOS preflight failed: {error}", file=sys.stderr)
        return 2
    report = {
        "schema_version": 1,
        "diagnostic_completed": True,
        "can_boot_original_dashboard": False,
        "profiles": profiles,
        "shared_content": compare_profiles(profiles),
        "cleanup": {
            "delete_archives": [],
            "reason": "no_input_proven_exclusively_outside_xbox_one; shared_members_do_not_make_archives_identical",
            "exclusion_is_not_deletion": True,
            "archive_rewrites_performed": False,
        },
    }
    print(json.dumps(report, indent=2, ensure_ascii=True))
    return 1  # Diagnostic completed; original dashboard remains blocked.


if __name__ == "__main__":
    sys.exit(main())
