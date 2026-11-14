# SPDX-License-Identifier: BSD-3-Clause
"""A missing import must stop diagnostics, never return fabricated success."""
import pathlib
import subprocess
import sys
import tempfile

launcher, fixture, loader_tests = map(pathlib.Path, sys.argv[1:])
result = subprocess.run([launcher, "run", fixture, "--warp", "--timeout-ms", "2000"],
                        capture_output=True, text=True, timeout=5)
assert result.returncode == 0 and "title entry returned" in result.stderr, result
data = fixture.read_bytes()
assert data.count(b"kernelx.dll\0") == 1
with tempfile.TemporaryDirectory(prefix="xone-era-cli-") as scratch:
    title = pathlib.Path(scratch) / "missing.exe"
    title.write_bytes(data.replace(b"kernelx.dll\0", b"missing.dll\0"))
    for command in ("check", "run"):
        result = subprocess.run([launcher, command, title], capture_output=True, text=True, timeout=5)
        assert result.returncode == 1, result
        assert "unresolved 2" in result.stdout and "running title" not in result.stdout, result
    for option in ("--stop-on-missing", "--stop-on-missing-imports"):
        result = subprocess.run([launcher, "run", title, option, "--timeout-ms", "2000"],
                                capture_output=True, text=True, timeout=5)
        assert result.returncode == 125, result
        assert "resolved 0, unresolved 2" in result.stdout, result
        assert "unimplemented missing.dll!Sleep" in result.stderr and "; stopped" in result.stderr, result
        assert "raw integer registers (not an ABI signature)" in result.stderr and "thread=" in result.stderr, result
        assert "title entry returned" not in result.stderr, result
result = subprocess.run([loader_tests, "--missing-activation"], capture_output=True, text=True, timeout=5)
assert result.returncode == 125, result
assert "unimplemented activation Xone.Tests.MissingActivation; stopped" in result.stderr, result
result = subprocess.run([loader_tests, "--missing-slot"], capture_output=True, text=True, timeout=5)
assert result.returncode == 125, result
assert "Xone.Tests.MissingSlot" in result.stderr and "raw WinRT integer args 0x11 0x1D" in result.stderr, result
print("era diagnostic CLI: passed")
