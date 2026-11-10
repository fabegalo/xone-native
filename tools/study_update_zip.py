#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Inventory an OSU ZIP in place, using the project's C++ XVD prefix parser.

No extraction, mounting, decryption or execution of archive members. Only the
explicitly supplied, locally built inspector is executed. Stdout contains JSON.
Optional streaming hashes read whole archives/members; prefix-only is the default.
"""

import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import stat
import subprocess
import sys
import zipfile


PROBE_SIZE = 0xE00  # inspection::xvd_probe_size, not a complete signed header.
MAX_ENTRIES = 4096


def digest(stream):
    result = hashlib.sha256()
    while block := stream.read(1024 * 1024):
        result.update(block)
    return result.hexdigest()


def study(path, inspector, hash_archive=False, hash_members=False):
    before = path.stat()
    result = {
        "source_name": path.name,
        "zip_size": before.st_size,
        "archive_sha256": None,
        "payloads_readable": "not_tested",
        "signature_verified": False,
        "files": [],
    }
    if hash_archive:
        with path.open("rb") as stream:
            result["archive_sha256"] = digest(stream)
    with zipfile.ZipFile(path) as archive:
        entries = archive.infolist()
        if len(entries) > MAX_ENTRIES:
            raise ValueError("ZIP entry limit exceeded")
        keys = set()
        for entry in sorted(entries, key=lambda value: value.filename):
            member = PurePosixPath(entry.filename)
            if (member.is_absolute() or ".." in member.parts or "\\" in entry.filename
                    or ":" in entry.filename or not member.parts):
                raise ValueError("unsafe archive member path")
            key = entry.filename.casefold()
            if key in keys:
                raise ValueError("duplicate or case-colliding archive member path")
            keys.add(key)
            if stat.S_ISLNK(entry.external_attr >> 16):
                raise ValueError("archive symlink is not an update component")
            if entry.is_dir():
                continue
            row = {
                "path": entry.filename,
                "size": entry.file_size,
                "compressed_size": entry.compress_size,
                "zip_crc32": f"{entry.CRC:08x}",
                "zip_crc_verified": False,
                "sha256": None,
            }
            if member.suffix.lower() == ".xvd":
                with archive.open(entry) as stream:
                    prefix = stream.read(PROBE_SIZE)
                row["prefix_bytes"] = len(prefix)
                row["prefix_sha256"] = hashlib.sha256(prefix).hexdigest()
                parsed = subprocess.run(
                    [str(inspector), "inspect-xvd-header", str(entry.file_size), "--json"],
                    input=prefix, capture_output=True, timeout=15, check=False,
                )
                if parsed.returncode:
                    row["error"] = parsed.stderr.decode("utf-8", errors="replace").strip()
                    result["complete"] = False
                else:
                    row["inspection"] = json.loads(parsed.stdout)
            if hash_members:
                with archive.open(entry) as stream:
                    row["sha256"] = digest(stream)
                row["zip_crc_verified"] = True  # ZipExtFile checks CRC at EOF.
            result["files"].append(row)
    after = path.stat()
    if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
        raise ValueError("archive changed during study")
    result.setdefault("complete", True)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archives", nargs="+", type=Path)
    parser.add_argument("--inspector", required=True, type=Path, help="locally built xone-native executable")
    parser.add_argument("--hash-archives", action="store_true", help="stream full ZIPs for SHA-256")
    parser.add_argument("--hash-members", action="store_true", help="stream full members for SHA-256 and ZIP CRC checks")
    args = parser.parse_args()
    try:
        inspector = args.inspector.resolve(strict=True)
        sources = [study(path, inspector, args.hash_archives, args.hash_members) for path in args.archives]
    except (OSError, ValueError, zipfile.BadZipFile, RuntimeError, subprocess.SubprocessError) as error:
        print(f"Update ZIP study failed: {error}", file=sys.stderr)
        return 1
    print(json.dumps({"schema_version": 1, "sources": sources}, indent=2, ensure_ascii=True))
    return 0 if all(source["complete"] for source in sources) else 1


if __name__ == "__main__":
    sys.exit(main())
