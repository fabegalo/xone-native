# SPDX-License-Identifier: BSD-3-Clause
"""Only repository-owned synthetic code; no OSUs, Microsoft payloads or GUI."""
import hashlib
import json
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

CLI, COMPONENT, EVIDENCE, INSPECTOR, TLS_COMPONENT = map(lambda s: Path(s).resolve(), sys.argv[1:6])
del sys.argv[1:]


def quote(text):
    return '"' + str(text).replace('\\', '\\\\').replace('"', '\\"') + '"'


def manifest(component):
    sha = hashlib.sha256(component.read_bytes()).hexdigest()
    return (f'xboxone-systemos-v1\nprofile "owned-synthetic" "synthetic-1" synthetic "{sha}"\n'
            'entry "entry"\n'
            f'artifact "entry" "synthetic-1" {quote(component.as_posix())} "{sha}" executable "repository-owned source"\n'
            'bind "entry" "kernel32.dll" "host:kernel32" "documented Windows API used by owned test"\n')


class BootstrapCLI(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="xboxone_bootstrap_cli_")
        self.root = Path(self.temp.name)
        self.profile = self.root / "profile.txt"
        self.profile.write_text(manifest(COMPONENT), encoding="utf-8")

    def tearDown(self):
        self.temp.cleanup()

    def run_cli(self, *args, **kwargs):
        return subprocess.run([str(CLI), *map(str, args)], capture_output=True, encoding="utf-8", timeout=10, **kwargs)

    def test_deep_unicode_content_path(self):
        # Explicit spelling lets the fixture work without host long-path policy.
        owned = Path("\\\\?\\" + str(self.root.resolve())) / "long-path-fixture"
        deep = owned
        while len(str(deep)) < 380:
            deep /= "biblioteca_própria_测试_" + "x" * 32
        try:
            deep.mkdir(parents=True)
            component = deep / "component.exe"
            shutil.copyfile(COMPONENT, component)
            ordinary = Path(str(component)[4:])
            path = deep / "profile.txt"
            text = manifest(component).replace(quote(component.as_posix()), quote(ordinary.as_posix()))
            path.write_text(text, encoding="utf-8")
            before = component.read_bytes()
            for spelling in (ordinary, component):
                with self.subTest(path=spelling):
                    inspect = subprocess.run([str(INSPECTOR), "inspect", str(spelling), "--json"],
                                             capture_output=True, encoding="utf-8", timeout=10)
                    self.assertEqual(inspect.returncode, 0, inspect.stderr)
                    self.assertEqual(json.loads(inspect.stdout)["kind"], "pe64")
                    inventory = subprocess.run([str(INSPECTOR), "inventory", str(spelling.parent), "--json"],
                                               capture_output=True, encoding="utf-8", timeout=10)
                    self.assertEqual(inventory.returncode, 0, inventory.stderr)
                    files = json.loads(inventory.stdout)["files"]
                    self.assertEqual(len(files), 1)
                    self.assertEqual(files[0]["sha256"], hashlib.sha256(before).hexdigest())
            ordinary_profile = Path(str(path)[4:])
            for contents in (text, manifest(component)):
                path.write_text(contents, encoding="utf-8")
                for spelling in (ordinary_profile, path, ordinary_profile.relative_to(self.root.resolve())):
                    with self.subTest(manifest=spelling, extended_artifact=contents != text):
                        run = self.run_cli("prepare", spelling, cwd=self.root)
                        self.assertEqual(run.returncode, 0, run.stderr + run.stdout)
                        report = json.loads(run.stdout)
                        self.assertEqual(report["stage"], "prepared")
                        self.assertEqual(report["authentic_frames_observed"], 0)
            path.write_text(text, encoding="utf-8")
            run = self.run_cli("run", ordinary_profile)
            report = json.loads(run.stdout)
            if run.returncode:
                self.assertEqual(report["stage"], "failed")
                self.assertEqual(report["process_id"], 0)
                self.assertTrue(any(d["code"] == "native_execution_failed" and
                                    "long working directory" in d["detail"] and "win32=267" in d["detail"]
                                    for d in report["diagnostics"]), report)
            else:
                self.assertEqual(report["stage"], "exited")
                self.assertEqual(report["exit_code"], 0)
            duplicate = next(line for line in manifest(component).splitlines() if line.startswith("artifact "))
            path.write_text(text + duplicate.replace('"entry"', '"alias"', 1) + "\n", encoding="utf-8")
            rejected = self.run_cli("prepare", ordinary_profile)
            self.assertEqual(rejected.returncode, 1, rejected.stderr)
            self.assertIn("ambiguous_path", [d["code"] for d in json.loads(rejected.stdout)["diagnostics"]])
            path.write_text(text.replace(hashlib.sha256(before).hexdigest(), "0" * 64), encoding="utf-8")
            rejected = self.run_cli("run", ordinary_profile)
            self.assertEqual(rejected.returncode, 1, rejected.stderr)
            self.assertEqual(json.loads(rejected.stdout)["process_id"], 0)
            self.assertIn("hash_mismatch", [d["code"] for d in json.loads(rejected.stdout)["diagnostics"]])
            self.assertEqual(component.read_bytes(), before)
        finally:
            if owned.exists():
                shutil.rmtree(owned)

    def test_long_executable_name_runs_in_pinned_directory(self):
        ordinary = self.root.resolve() / ("component_" + "x" * (270 - len(str(self.root.resolve()))) + ".exe")
        extended = Path("\\\\?\\" + str(ordinary))
        shutil.copyfile(COMPONENT, extended)
        try:
            text = manifest(extended).replace(quote(extended.as_posix()), quote(ordinary.as_posix()))
            self.profile.write_text(text, encoding="utf-8")
            result = self.run_cli("run", self.profile)
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            self.assertEqual(json.loads(result.stdout)["stage"], "exited")
        finally:
            extended.unlink()

    def test_run_and_record_owned_execution(self):
        result = self.run_cli("run", self.profile, "5000")
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        report = json.loads(result.stdout)
        self.assertEqual(report["stage"], "exited")
        self.assertGreater(report["process_id"], 0)
        self.assertTrue(report["primary_thread_resumed"])
        self.assertEqual(report["exit_code"], 0)
        self.assertEqual(report["declared_origin"], "synthetic")
        self.assertFalse(report["dashboard_boot_verified"])
        self.assertEqual(report["authentic_frames_observed"], 0)
        self.assertEqual(report["dependencies"][0]["status"], "host_exports_verified")
        self.assertEqual(report["entry_sha256"], hashlib.sha256(COMPONENT.read_bytes()).hexdigest())
        EVIDENCE.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        EVIDENCE.with_suffix(".manifest").write_text(manifest(COMPONENT), encoding="utf-8")

    def test_prepare_does_not_execute(self):
        result = self.run_cli("prepare", self.profile)
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(result.stdout)
        self.assertEqual(report["stage"], "prepared")
        self.assertEqual(report["process_id"], 0)
        self.assertIsNone(report["exit_code"])

    def test_native_static_tls_lifecycle(self):
        self.profile.write_text(manifest(TLS_COMPONENT), encoding="utf-8")
        prepared = self.run_cli("prepare", self.profile)
        self.assertEqual(prepared.returncode, 0, prepared.stderr + prepared.stdout)
        report = json.loads(prepared.stdout)
        tls = report["nodes"][0]["pe"]["tls"]
        self.assertGreater(tls["template_size"], 0)
        self.assertGreater(tls["index_rva"], 0)
        self.assertEqual(len(tls["callback_rvas"]), 1)
        self.assertEqual(report["process_id"], 0)
        for _ in range(2):  # Each fresh process must recreate TLS state.
            run = self.run_cli("run", self.profile)
            self.assertEqual(run.returncode, 0, run.stderr + run.stdout)
            report = json.loads(run.stdout)
            self.assertEqual(report["stage"], "exited")
            self.assertEqual(report["exit_code"], 0)
            self.assertTrue(report["primary_thread_resumed"])
            self.assertFalse(report["dashboard_boot_verified"])
            self.assertEqual(report["authentic_frames_observed"], 0)

    def test_hash_mismatch_is_blocked(self):
        text = manifest(COMPONENT).replace(hashlib.sha256(COMPONENT.read_bytes()).hexdigest(), "0" * 64)
        self.profile.write_text(text, encoding="utf-8")
        result = self.run_cli("run", self.profile)
        self.assertEqual(result.returncode, 1)
        report = json.loads(result.stdout)
        self.assertEqual(report["process_id"], 0)
        self.assertIn("hash_mismatch", [d["code"] for d in report["diagnostics"]])

    def test_unicode_path(self):
        own_copy = self.root / "componente_próprio_测试.exe"
        shutil.copyfile(COMPONENT, own_copy)  # Owned synthetic bytes only.
        self.profile.write_text(manifest(own_copy), encoding="utf-8")
        result = self.run_cli("run", self.profile)
        self.assertEqual(result.returncode, 0, result.stderr + result.stdout)

    def test_invalid_timeout(self):
        for value in ("0", "60001", "-1", "10x"):
            self.assertEqual(self.run_cli("run", self.profile, value).returncode, 2)

    def test_missing_and_malformed_manifest(self):
        self.assertEqual(self.run_cli("prepare", self.root / "missing").returncode, 2)
        self.profile.write_text('xboxone-systemos-v1\nprofile invalid\n', encoding="utf-8")
        self.assertEqual(self.run_cli("prepare", self.profile).returncode, 2)

    def test_unknown_service_does_not_succeed(self):
        original = self.profile.read_text(encoding="utf-8")
        for kind in ("nt_kernel", "xbox_device", "xbox_broker", "winrt_activation", "compositor", "storage"):
            with self.subTest(kind=kind):
                self.profile.write_text(original + f'service "test-service" {kind} "synthetic missing contract"\n', encoding="utf-8")
                result = self.run_cli("run", self.profile)
                self.assertEqual(result.returncode, 1)
                report = json.loads(result.stdout)
                self.assertEqual(report["process_id"], 0)
                diagnostic = next(d for d in report["diagnostics"] if d["code"] == "service_adapter_missing")
                self.assertIn("SystemOS", diagnostic["detail"])
                self.assertIn("synthetic missing contract", diagnostic["detail"])


if __name__ == "__main__":
    unittest.main()
