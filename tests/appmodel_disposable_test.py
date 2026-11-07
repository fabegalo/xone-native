# SPDX-License-Identifier: BSD-3-Clause
"""Explicit opt-in ONLY, outside default CTest. Requires a disposable Windows
Sandbox guest or GitHub-hosted VM, non-elevated account, Python and official
development-package registration available. Never enables host features or
developer mode, installs certificates or requests elevation.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile
import uuid
from appmodel_fixture import layout, profile


def powershell(code, env):
    return subprocess.run(["powershell.exe", "-NoProfile", "-NonInteractive", "-Command",
                           "$ErrorActionPreference='Stop'; " + code], env=env,
                          capture_output=True, encoding="utf-8", errors="replace", timeout=30)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--disposable", action="store_true", required=True)
    parser.add_argument("--bootstrap", type=Path, required=True)
    parser.add_argument("--component", type=Path, required=True)
    parser.add_argument("--evidence", type=Path, required=True)
    args = parser.parse_args()
    env = os.environ.copy()
    evidence = {"activation_verified": False, "registration_attempted": False, "cleanup_verified": False,
                "dashboard_boot_verified": False, "authentic_frames_observed": 0}
    registered = False
    try:
        hosted = env.get("GITHUB_ACTIONS") == "true" and env.get("RUNNER_ENVIRONMENT") == "github-hosted"
        sandbox = env.get("USERNAME", "").lower() == "wdagutilityaccount"
        if os.name != "nt" or not (hosted or sandbox):
            raise RuntimeError("requires explicit disposable Sandbox guest or GitHub-hosted VM; host registration refused")
        guard = powershell("$p=[Security.Principal.WindowsPrincipal][Security.Principal.WindowsIdentity]::GetCurrent(); "
                           "if ($p.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)) { throw 'non-elevated test account required' }", env)
        if guard.returncode:
            raise RuntimeError(guard.stderr)
        with tempfile.TemporaryDirectory(prefix="xboxone_owned_package_") as directory:
            root = Path(directory).resolve()
            name = "XboxOneNative.Test." + uuid.uuid4().hex[:24]
            layout(root, args.component.resolve(), name)
            path = profile(root, native_imports=True)
            env["XON_TEST_PACKAGE_NAME"] = name
            env["XON_TEST_PACKAGE_MANIFEST"] = str(root / "AppxManifest.xml")
            env["XON_TEST_PACKAGE_ROOT"] = str(root)
            existing = powershell("if (Get-AppxPackage -Name $env:XON_TEST_PACKAGE_NAME) { throw 'unexpected existing identity; refusing to modify it' }", env)
            if existing.returncode:
                raise RuntimeError(existing.stderr)
            try:
                # Cleanup must run even if the deployment subprocess times out.
                registered = True
                evidence["registration_attempted"] = True
                result = powershell("Add-AppxPackage -Register $env:XON_TEST_PACKAGE_MANIFEST; "
                                    "(Get-AppxPackage -Name $env:XON_TEST_PACKAGE_NAME).PackageFullName", env)
                evidence["deployment"] = {"exit_code": result.returncode, "stdout": result.stdout, "stderr": result.stderr}
                if result.returncode:
                    raise RuntimeError("official development registration failed; see deployment diagnostics")
                run = subprocess.run([str(args.bootstrap.resolve()), "run", str(path), "5000"],
                                     capture_output=True, encoding="utf-8", timeout=20)
                evidence["bootstrap"] = json.loads(run.stdout)
                r = evidence["bootstrap"]
                calls = [e for e in r["activation_events"] if e["api"] == "IApplicationActivationManager::ActivateApplication"]
                if (run.returncode or r["stage"] != "exited" or not r["process_id"] or r["exit_code"] != 0
                        or not r['activation_identity_verified'] or not r['owned_lifecycle_verified']
                        or r["primary_thread_resumed"] or len(calls) != 1 or calls[0]["result"] != 0):
                    raise RuntimeError("packaged lifecycle was not verified")
                evidence["activation_verified"] = True
                path = profile(root, native_imports=True, lifecycle=None)
                observed = subprocess.run([str(args.bootstrap.resolve()), 'run', str(path), '5000'],
                                          capture_output=True, encoding='utf-8', timeout=20)
                r = evidence['process_exit_bootstrap'] = json.loads(observed.stdout)
                if (observed.returncode or r['stage'] != 'exited' or r['exit_code'] != 0
                        or not r['activation_identity_verified'] or r['owned_lifecycle_verified']
                        or r['appmodel_lifecycle'] != 'process_exit'
                        or any(e['api'].startswith('owned_fixture') for e in r['activation_events'])):
                    raise RuntimeError('normal packaged process observation was not verified')
                evidence['process_exit_verified'] = True
            finally:
                if registered:
                    clean = powershell("$p=Get-AppxPackage -Name $env:XON_TEST_PACKAGE_NAME; "
                                       "if ($p | Where-Object { $_.InstallLocation -ne $env:XON_TEST_PACKAGE_ROOT }) { throw 'different installation root; refusing cleanup' }; "
                                       "$p | ForEach-Object { Remove-AppxPackage -Package $_.PackageFullName }; "
                                       "if (Get-AppxPackage -Name $env:XON_TEST_PACKAGE_NAME) { throw 'package cleanup incomplete' }", env)
                    evidence["cleanup_verified"] = clean.returncode == 0
                    evidence["cleanup"] = {"exit_code": clean.returncode, "stderr": clean.stderr}
                    if clean.returncode:
                        raise RuntimeError("test package cleanup failed in disposable guest")
    except (OSError, RuntimeError, subprocess.SubprocessError, ValueError) as exception:
        evidence["blocker"] = str(exception)
    args.evidence.resolve().write_text(json.dumps(evidence, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(evidence))
    return 0 if evidence["activation_verified"] and evidence.get('process_exit_verified') and evidence["cleanup_verified"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
