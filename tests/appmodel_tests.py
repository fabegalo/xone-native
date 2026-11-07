# SPDX-License-Identifier: BSD-3-Clause
"""Windows SDK manifest/graph tests. No package deployment or activation."""
import hashlib
import json
import os
import shutil
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest
from appmodel_fixture import layout, profile, TEMPLATE

CLI = Path(sys.argv.pop(1)).resolve()
COMPONENT = Path(sys.argv.pop(1)).resolve() if len(sys.argv) > 1 else None
EVIDENCE = Path(sys.argv.pop(1)).resolve() if len(sys.argv) > 1 else None


class AppModel(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="xboxone_appmodel_")
        self.root = Path(self.temp.name).resolve()
        layout(self.root)
        self.manifest = self.root / "AppxManifest.xml"

    def tearDown(self):
        self.temp.cleanup()

    def edit(self, old, new):
        self.manifest.write_text(self.manifest.read_text(encoding="utf-8").replace(old, new), encoding="utf-8")

    def prepare(self, **kwargs):
        before = self.manifest.read_bytes()
        p = profile(self.root, **kwargs)
        result = subprocess.run([str(CLI), "prepare", str(p)], capture_output=True, encoding="utf-8", timeout=10)
        self.assertIn(result.returncode, (0, 1), result.stderr)
        report = json.loads(result.stdout)
        self.assertEqual(self.manifest.read_bytes(), before)
        self.assertEqual(report["process_id"], 0)
        self.assertFalse(report["dashboard_boot_verified"])
        self.assertEqual(report["authentic_frames_observed"], 0)
        return result.returncode, report

    def rejected(self, code, **kwargs):
        status, report = self.prepare(**kwargs)
        self.assertEqual(status, 1, report)
        self.assertIn(code, [d["code"] for d in report["diagnostics"]], report)
        return report

    def test_sdk_metadata_and_no_deployment(self):
        status, r = self.prepare()
        self.assertEqual(status, 0, r)
        self.assertEqual(r["execution_model"], "appmodel")
        self.assertEqual(r["stage"], "prepared")
        p = r["packages"][0]
        self.assertEqual(p["name"], "XboxOneNative.OwnedLifecycleTest")
        self.assertEqual(p["publisher"], "CN=XboxOneNative.OwnedTest")
        self.assertEqual(p["version_raw"], 1 << 48)
        self.assertEqual(p["architecture"], 9)
        self.assertEqual(p["applications"][0]["entry_point"], "Windows.FullTrustApplication")
        self.assertEqual(p["applications"][0]["aumid"], p["family_name"] + "!OwnedLifecycle")
        self.assertEqual(p["capabilities"][0]["name"], "runFullTrust")
        self.assertEqual(r["activation_events"], [])
        self.assertTrue(any(d["kind"] == "appx_resource" and d["status"] == "pinned_resource_verified" for d in r["dependencies"]))

    def test_explicit_and_default_observation_contracts(self):
        for lifecycle in (None, 'process_exit', 'owned_fixture'):
            status, r = self.prepare(lifecycle=lifecycle)
            self.assertEqual(status, 0, r)
            self.assertEqual(r['appmodel_lifecycle'], lifecycle or 'process_exit')
            self.assertFalse(r['activation_identity_verified'])
            self.assertFalse(r['owned_lifecycle_verified'])
        for extra in (['appmodel_lifecycle unknown'],
                      ['appmodel_lifecycle process_exit', 'appmodel_lifecycle owned_fixture']):
            p = profile(self.root, lifecycle=None, extra=extra)
            run = subprocess.run([str(CLI), 'prepare', str(p)], capture_output=True, encoding='utf-8', timeout=10)
            self.assertEqual(run.returncode, 2, run.stdout)
            self.assertIn('invalid manifest record', run.stderr)
        p = profile(self.root)
        text = '\n'.join(line for line in p.read_text(encoding='utf-8').splitlines() if not line.startswith('appmodel '))
        p.write_text(text + '\n', encoding='utf-8')
        run = subprocess.run([str(CLI), 'prepare', str(p)], capture_output=True, encoding='utf-8', timeout=10)
        self.assertEqual(run.returncode, 2)

    def test_deep_package_path_without_deployment(self):
        owned = Path("\\\\?\\" + str(self.root)) / "long-package-fixture"
        deep = owned
        while len(str(deep)) < 380:
            deep /= "pacote_próprio_测试_" + "x" * 32
        try:
            layout(deep)
            p = profile(deep)
            # Read fixture bytes with an extended path, then pin ordinary paths.
            text = p.read_text(encoding="utf-8").replace("//?/", "")
            p.write_text(text, encoding="utf-8")
            result = subprocess.run([str(CLI), "prepare", str(p)[4:]], capture_output=True,
                                    encoding="utf-8", timeout=10)
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
            r = json.loads(result.stdout)
            self.assertEqual(r["stage"], "prepared")
            self.assertEqual(len(r["packages"]), 1)
            self.assertEqual(r["process_id"], 0)
            self.assertEqual(r["activation_events"], [])
            self.assertEqual(p.read_text(encoding="utf-8"), text)
        finally:
            if owned.exists():
                shutil.rmtree(owned)

    def test_xml_security_and_structure(self):
        original = self.manifest.read_text(encoding="utf-8")
        for xml in (original.replace('<Package ', '<!DOCTYPE Package [<!ENTITY x SYSTEM "file:///C:/Windows/win.ini">]><Package ', 1),
                    original.replace('<Package ', '<?unsafe run="true"?><Package ', 1),
                    original.replace('Version="1.0.0.0"', 'Version="1.0.0.0" Version="2.0.0.0"'),
                    original.replace('</Package>', ''), original + '<Package/>',
                    original.replace('Version="1.0.0.0"', 'Version="65536.0.0.0"'),
                    original.replace('foundation/windows10"', 'foundation/spoof"', 1),
                    original.replace('</Applications>', original.split('<Applications>')[1].split('</Applications>')[0] + '</Applications>'),
                    '<Package>' + '<nested>' * 40 + '</nested>' * 40 + '</Package>',
                    ' ' * (1024 * 1024 + 1)):
            with self.subTest(xml=xml[:60]):
                self.manifest.write_text(xml, encoding="utf-8")
                # Oversized input may fail at the fingerprint or snapshot gate.
                self.rejected("appx_manifest_rejected")

    def test_unsafe_executable_paths(self):
        for path in ('../owned.exe', 'C:/owned.exe', '//server/share/owned.exe', 'owned.exe:stream', 'bin/../owned.exe', 'NUL.exe'):
            with self.subTest(path=path):
                self.manifest.write_text(TEMPLATE.read_text(encoding="utf-8").replace('owned.exe', path), encoding="utf-8")
                self.rejected("appx_manifest_rejected")

    def test_missing_identity_hash_and_version(self):
        self.rejected("hash_mismatch", hashes={"appx": "0" * 64})
        self.rejected("version_mismatch", versions={"appx": "other-revision"})
        self.rejected("ambiguous_artifact", extra=[
            f'artifact "appx" "owned-revision-1" "{self.manifest.as_posix()}" "' + hashlib.sha256(self.manifest.read_bytes()).hexdigest() + '" resource "duplicate"'])

    def test_exact_application_and_executable_selection(self):
        self.edit('Id="OwnedLifecycle"', 'Id="Other"')
        self.rejected("application_id_missing")
        self.manifest.write_text(TEMPLATE.read_text(encoding="utf-8").replace('owned.exe', 'different.exe'), encoding="utf-8")
        self.rejected("package_executable_mismatch")

    def test_unsupported_contracts_are_visible(self):
        original = TEMPLATE.read_text(encoding="utf-8")
        for old, new, code in [
            ('Windows.Desktop', 'Windows.Xbox', 'appmodel_device_family_unsupported'),
            ('Windows.FullTrustApplication', 'Owned.WinRT.App', 'appmodel_entrypoint_unsupported'),
            ('<Capabilities>', '<Capabilities><Capability Name="internetClient"/>', 'appmodel_capability_unsupported'),
            ('</Application>', '<Extensions/></Application>', 'appmodel_contract_unsupported')]:
            with self.subTest(code=code):
                self.manifest.write_text(original.replace(old, new), encoding="utf-8")
                self.rejected(code)
        self.manifest.write_text(original, encoding="utf-8")
        for kind in ("nt_kernel", "xbox_device", "xbox_broker", "winrt_activation", "compositor", "storage"):
            with self.subTest(service=kind):
                self.rejected("service_adapter_missing", extra=[f'service "xbox-contract" {kind} "no Xbox service adapter"'])

    def dependency(self, name="XboxOneNative.Dependency", version="1.0.0.0"):
        self.edit('</Dependencies>', f'<PackageDependency Name="{name}" Publisher="CN=XboxOneNative.OwnedTest" MinVersion="{version}"/></Dependencies>')

    def test_missing_and_ambiguous_package_bindings(self):
        self.dependency()
        self.rejected("missing_package_binding")
        binding = 'package_bind "appx" "XboxOneNative.Dependency" "appx" "own audited dependency"'
        self.rejected("ambiguous_package_binding", extra=[binding, binding])
        self.rejected("package_identity_mismatch", extra=[binding])

    def test_package_provider_identity_and_revision(self):
        dep = self.root / "dependency"
        layout(dep, name="XboxOneNative.Dependency")
        self.dependency()
        packages = [("dep", dep / "AppxManifest.xml"), ("dep-logo", dep / "logo.png")]
        binding = 'package_bind "appx" "XboxOneNative.Dependency" "dep" "explicit revision-bound provider"'
        r = self.rejected("package_graph_activation_unsupported", packages=packages, extra=[binding])
        self.assertTrue(any(d["status"] == "pinned_package_identity_verified" for d in r["dependencies"]))
        self.rejected("version_mismatch", packages=packages, extra=[binding], versions={"dep": "other"})
        self.edit('MinVersion="1.0.0.0"', 'MinVersion="2.0.0.0"')
        self.rejected("package_identity_mismatch", packages=packages, extra=[binding])

    def test_dependency_cycle_and_unused_binding(self):
        self.dependency("XboxOneNative.OwnedLifecycleTest")
        self.rejected("package_dependency_cycle", extra=[
            'package_bind "appx" "XboxOneNative.OwnedLifecycleTest" "appx" "explicit cycle fixture"'])
        self.manifest.write_text(TEMPLATE.read_text(encoding="utf-8"), encoding="utf-8")
        self.rejected("unused_package_binding", extra=['package_bind "appx" "absent" "appx" "explicit but unused"'])

    def test_profile_order_determinism_and_no_win32_fallback(self):
        _, a = self.prepare()
        _, b = self.prepare(reverse=True)
        self.assertEqual(a, b)
        p = profile(self.root)
        p.write_text(p.read_text(encoding="utf-8").replace('appmodel "appx" "OwnedLifecycle"', 'appmodel "appx" "Missing"'), encoding="utf-8")
        result = subprocess.run([str(CLI), 'run', str(p)], capture_output=True, encoding="utf-8", timeout=10)
        r = json.loads(result.stdout)
        self.assertEqual(result.returncode, 1)
        self.assertEqual(r["process_id"], 0)
        self.assertEqual(r["activation_events"], [])

    def test_original_declaration_does_not_authorize_xbox_activation(self):
        p = profile(self.root)
        p.write_text(p.read_text(encoding="utf-8").replace(' synthetic ', ' original '), encoding="utf-8")
        result = subprocess.run([str(CLI), 'run', str(p)], capture_output=True, encoding="utf-8", timeout=10)
        r = json.loads(result.stdout)
        self.assertEqual(result.returncode, 1)
        self.assertIn('original_appmodel_contract_unaudited', [d['code'] for d in r['diagnostics']])
        self.assertEqual(r['process_id'], 0)

    def test_assets_and_provider_publisher_are_pinned(self):
        self.rejected('hash_mismatch', hashes={'logo': '0' * 64})
        self.edit('logo.png', 'absent.png')
        self.rejected('missing_pinned_resource')
        self.manifest.write_text(TEMPLATE.read_text(encoding='utf-8'), encoding='utf-8')
        self.dependency()
        dep = self.root / 'provider'
        layout(dep, name='XboxOneNative.Dependency')
        p = dep / 'AppxManifest.xml'
        p.write_text(p.read_text(encoding='utf-8').replace('CN=XboxOneNative.OwnedTest', 'CN=OtherPublisher'), encoding='utf-8')
        self.rejected('package_identity_mismatch', packages=[('dep', p)], extra=[
            'package_bind "appx" "XboxOneNative.Dependency" "dep" "explicit provider"'])

    def test_utf16_and_unicode_package_root(self):
        text = self.manifest.read_text(encoding='utf-8').replace('encoding="utf-8"', 'encoding="utf-16"')
        self.manifest.write_bytes(text.encode('utf-16'))
        self.assertEqual(self.prepare()[0], 0)
        self.root = self.root / 'pacote_próprio_测试'
        layout(self.root)
        self.manifest = self.root / 'AppxManifest.xml'
        self.assertEqual(self.prepare()[0], 0)

    def test_symlink_manifest_is_rejected(self):
        target = self.root / "source.xml"
        self.manifest.rename(target)
        try:
            self.manifest.symlink_to(target)
        except OSError:
            if os.environ.get("XBOXONE_REQUIRE_SYMLINKS") == "1":
                self.fail("CI requires symlink coverage")
            self.skipTest("host lacks symlink privilege")
        self.rejected("artifact_unreadable")

    def test_absent_registration_is_real_failure(self):
        # A unique syntactic fixture cannot be registered. No fake activation.
        import uuid
        layout(self.root, name="XboxOneNative.Test." + uuid.uuid4().hex[:24])
        p = profile(self.root)
        result = subprocess.run([str(CLI), 'run', str(p)], capture_output=True, encoding="utf-8", timeout=10)
        r = json.loads(result.stdout)
        self.assertEqual(result.returncode, 1, r)
        self.assertEqual(r["process_id"], 0)
        self.assertIn('package_registration_unavailable', [d['code'] for d in r['diagnostics']])
        self.assertEqual(r['activation_events'][0]['api'], 'GetPackagePathByFullName(size)')
        self.assertNotEqual(r['activation_events'][0]['result'], 0)

    def test_owned_compiled_component_reaches_registration_gate(self):
        if COMPONENT is None:
            self.skipTest('compiled owned component was not supplied')
        import uuid
        # A single ignored layout/evidence path can be inspected after the test.
        root = EVIDENCE.with_suffix('.package') if EVIDENCE else self.root
        layout(root, COMPONENT, name='XboxOneNative.Test.' + uuid.uuid4().hex[:24])
        p = profile(root, native_imports=True)
        result = subprocess.run([str(CLI), 'run', str(p)], capture_output=True, encoding='utf-8', timeout=10)
        r = json.loads(result.stdout)
        self.assertEqual(result.returncode, 1, r)
        self.assertEqual(r['entry_sha256'], hashlib.sha256(COMPONENT.read_bytes()).hexdigest())
        self.assertEqual(r['execution_model'], 'appmodel')
        self.assertEqual(r['process_id'], 0)
        self.assertEqual([d['code'] for d in r['diagnostics']], ['package_registration_unavailable'])
        if EVIDENCE:
            EVIDENCE.write_text(json.dumps(r, indent=2) + '\n', encoding='utf-8')


if __name__ == "__main__":
    unittest.main()
