# SPDX-License-Identifier: BSD-3-Clause
"""Prepare owned test bytes only. Does not register or launch packages."""
import hashlib
from pathlib import Path
import shutil
import struct
import zlib

TEMPLATE = Path(__file__).parent / "appmodel" / "AppxManifest.xml"


def quote(value):
    return '"' + str(value).replace('\\', '\\\\').replace('"', '\\"') + '"'


def logo(size):
    def chunk(kind, value):
        return struct.pack(">I", len(value)) + kind + value + struct.pack(">I", zlib.crc32(kind + value))
    # One own solid PNG asset, generated locally; no copied logo or screenshot.
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress((b"\0" + b"\x30\x60\x90\xff" * size) * size)) + chunk(b"IEND", b""))


def layout(root, component=None, name=None):
    root.mkdir(parents=True, exist_ok=True)
    manifest = TEMPLATE.read_text(encoding="utf-8")
    if name:
        manifest = manifest.replace("XboxOneNative.OwnedLifecycleTest", name)
    (root / "AppxManifest.xml").write_text(manifest, encoding="utf-8")
    for file, size in [('logo.png', 50), ('square150.png', 150), ('square44.png', 44)]:
        (root / file).write_bytes(logo(size))
    if component:
        shutil.copyfile(component, root / "owned.exe")  # Own small fixture only.
    else:
        b = bytearray(1024)
        def put(at, value, size=4):
            b[at:at + size] = value.to_bytes(size, "little")
        b[:2] = b"MZ"; put(0x3c, 0x80); b[0x80:0x84] = b"PE\0\0"
        for at, value, size in [(0x84,0x8664,2),(0x86,1,2),(0x94,0xf0,2),(0x96,2,2),
                                (0x98,0x20b,2),(0xa8,0x2000,4),(0xb0,0x140000000,8),
                                (0xd4,0x200,4),(0xdc,3,2),(0x104,16,4),
                                (0x190,0x200,4),(0x194,0x2000,4),(0x198,0x200,4),(0x19c,0x200,4)]:
            put(at, value, size)
        (root / "owned.exe").write_bytes(b)


def profile(root, packages=(), extra=(), versions=None, hashes=None, reverse=False, native_imports=False, lifecycle="owned_fixture"):
    records = []
    files = [("entry", root / "owned.exe", "executable"),
             ("appx", root / "AppxManifest.xml", "resource"), ("logo", root / "logo.png", "resource"),
             ("icon150", root / "square150.png", "resource"), ("icon44", root / "square44.png", "resource")]
    files += [(id, path, "resource") for id, path in packages]
    for id, path, role in files:
        sha = (hashes or {}).get(id, hashlib.sha256(path.read_bytes()).hexdigest())
        revision = (versions or {}).get(id, "owned-revision-1")
        records.append(f'artifact {quote(id)} {quote(revision)} {quote(path.as_posix())} "{sha}" {role} "repository-owned fixture"')
    if native_imports:
        records.append('bind "entry" "kernel32.dll" "host:kernel32" "owned documented Windows lifecycle APIs"')
    rows = ['xboxone-systemos-v1', 'profile "owned-appmodel" "owned-revision-1" synthetic "' + "a" * 64 + '"',
            'entry "entry"', 'appmodel "appx" "OwnedLifecycle" "owned packaged desktop lifecycle contract v1"']
    rows += list(reversed(records)) if reverse else records
    if lifecycle is not None:
        rows.append('appmodel_lifecycle ' + lifecycle)
    rows += extra
    path = root / "profile.txt"
    path.write_text("\n".join(rows) + "\n", encoding="utf-8")
    return path
