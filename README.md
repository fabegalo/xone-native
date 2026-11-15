<div align="center">
  <img src="docs/assets/xone-native-banner.webp" alt="Concept art for xone-native showing a console-to-PC transition through compatibility layers; not a system screenshot" width="100%" />

  # xone-native

  **An open research project to run original Xbox One system components and games on PCs, with the authentic SystemOS dashboard as its central goal.**

  [![CI/CD](https://img.shields.io/github/actions/workflow/status/fabegalo/xone-native/build.yml?branch=main&event=push&label=CI%2FCD)](https://github.com/fabegalo/xone-native/actions/workflows/build.yml?query=branch%3Amain)
  [![Runtime research](https://img.shields.io/badge/milestone-runtime%20research-555)](#current-status)
  [![Experimental](https://img.shields.io/badge/status-experimental-orange)](#current-status)
  [![BSD-3-Clause](https://img.shields.io/badge/license-BSD--3--Clause-blue)](LICENSE)

  [English](README.md) · [Português (Brasil)](README.pt-BR.md) · [Documentation](docs/README.md) · [Contributing](CONTRIBUTING.md)
</div>

> **Early research software — no playable commercial games, original SystemOS boot or end-user release yet.** A locally recreated Xbox One-style dashboard now opens by default; it is **not** the original SystemOS and does not show Microsoft's dashboard. The banner above is concept art, not a capture of running Xbox One software. A successful build or XVD header inspection is not proof of Xbox One compatibility.

## See it running

| Sonic Mania · original Xbox One title screen | Local dashboard · Home after title return |
|---|---|
| ![Original Sonic Mania Xbox One title screen rendered by xone-era](docs/assets/screenshots/sonic-mania-title.png) | ![Locally recreated dashboard Home with Sonic Mania and the title suspension result](docs/assets/screenshots/local-dashboard-home.png) |

<sub>Local Windows x64 captures, 15 November 2026. Sonic Mania 1.0.4.2 reached its title screen through the ERA runtime; gameplay is not certified. Home is output from the locally recreated dashboard renderer (`local_recreation`), not original SystemOS. Game imagery belongs to its respective owners. [Session evidence and remaining limitations](docs/NEXT-TASK.md).</sub>

## What is xone-native?

Xbox One software targets **x86-64**, but its original environment consists of separate HostOS, SystemOS and GameOS/ERA components, with Xbox-specific virtualization, services, devices, graphics interfaces and security. Matching the CPU architecture alone does **not** make its executables ordinary desktop Windows programs.

The project's long-term objective is to launch the **original SystemOS shell and dashboard**, navigate it with a controller, access local profiles, Guide and library, launch a supported game **from that dashboard**, and return to the original shell.

While authentic SystemOS stays unreadable, the default front-end is a **locally recreated dashboard** inspired by the 2015 New Xbox One Experience (maintainer-approved, see [AGENTS.md](AGENTS.md)). It uses the same console state, catalog, ERA runtime and session supervisor that an original SystemOS backend will use later. Its frames are labelled `local_recreation`, its art is drawn by the project, and it offers no online services, achievements or licenses.

The preferred strategy is direct x86-64 execution **when technically demonstrated**, with a modular compatibility layer for Xbox services and a VM-based or hybrid SystemOS path only when required by real dependencies. GameOS/ERA and later GameCore need separate contracts. This is not a port of x360-native's PowerPC AOT/ReXGlue architecture.

<a id="current-status"></a>

## Current status

**Runtime research — original ERA title execution and a local dashboard on Windows x64.** Sonic Mania 1.0.4.2 has reached its original title screen. The local dashboard can launch a title, compose its Guide over the title output and return to Home with shared profile/storage state. Commercial gameplay and original SystemOS boot remain unproven; the read-only XVD/PE inspector is also available.

| Component | Actually implemented | Not demonstrated |
|---|---|---|
| **XVD inspection** | Recognizes the header at `0x200` and reports structural metadata | Signature verification, readable payload, decryption, SystemOS boot |
| **PE32+ x64 inspection** | Reports image properties, DLL imports and imported symbols | Xbox-specific provenance, NT/WinRT activation, executable launch |
| **Build and tests** | Portable CMake targets; local M0 tests previously passed | Runtime compatibility; functional platform validation |
| **Local dashboard (recreation, Windows x64)** | Sign-in and persistent local profiles, Home, My games & apps, Guide, settings (display, audio, controller, language, storage), keyboard and XInput navigation, UI sounds; folder import identified by manifest + SHA-256, `xone-era check`, launch through `xone-era run`, Guide quit, return with the real exit result | Commercial title gameplay through it; Linux/macOS host adapters; physical-controller validation |
| **SystemOS / original dashboard** | Architecture and obstacles documented | Original frame, boot, navigation, Guide, profiles |
| **GameOS / ERA / graphics** | Original x86-64 title execution, partial KernelX/Xbox WinRT services and D3D11.X → D3D12 translation; Sonic Mania startup/title screen; supervised return to the local dashboard | Certified commercial gameplay, audible original audio, complete API/graphics coverage |

A GitHub Actions workflow **is configured** for Windows, Linux and macOS host builds/tests. Its existence does not establish runtime support on those systems. Windows x64/ARM64, Linux x64/ARM64 (including Steam Deck on Linux x64), and macOS Intel/Apple Silicon are **long-term targets**, not supported downloads.

The documented SystemOS build `10.0.10586.1029` is only an **inspection reference**, not a proven easiest or bootable target. Selection of the first real dashboard requires legitimate, readable, version-consistent components. See [M0 findings](docs/M0.md) and [project status](STATUS.md).

## Try the current M0 inspector

The read-only inspector is a **developer tool** alongside the local dashboard. You need CMake and a C++20 compiler. From the repository root:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```

Run it on files that you lawfully possess and are authorized to analyze:

```sh
./build/xone-native inspect /path/to/system.xvd --json
./build/xone-native inspect /path/to/component.exe
```

With a Windows multi-config generator, the executable is typically `build\Release\xone-native.exe`; locate the executable in the selected generator's output directory. The inspector does **not** copy, unpack, decrypt, authenticate, modify or execute the input. A PE32+ result does not identify a binary as Xbox One software on its own.

### Open the local dashboard (Windows x64)

Run `xone-native` with no arguments (or `xone-native dashboard`). Create a local profile, then use **My games & apps → Import folder** to pick a folder with extracted title packages (each with its `AppxManifest.xml`). Titles start through `xone-era`, which must be built next to `xone-native` (MSVC x64). Keyboard: arrows, Enter = A, Esc = B, F2 = Menu, Del = X, Home = Guide (Ctrl+Home while a title has focus), PageUp/PageDown = bumpers. A title that cannot start shows the runtime's real error. Profiles, settings, the library and title saves live in `%LOCALAPPDATA%\xone-native` (override with `XONE_NATIVE_HOME`).

There are **no downloadable user releases** at this stage. Do not present the repository source archive as a playable application.

## Architecture and design rules

```text
Future desktop launcher (management and preparation only)
  └── Shared content inventory / version profiles / SHA-256
        ├── SystemOS session
        │     ├── Original component activation / compatibility boundary
        │     ├── Optional VM boundary if truly required
        │     └── Original shell, presentation, input, profiles, storage
        └── GameOS session
              ├── ERA x86-64 loader and KernelX / Xbox WinRT contracts
              ├── D3D11.X, audio, controller and storage adapters
              └── Return-to-SystemOS session coordination
```

- **Authenticity:** only original, legally obtained components may render the dashboard; no screenshots, mock shell or fake Guide may count as the real system.
- **Exact version isolation:** original modules and assets must match their SystemOS revision, provenance and hashes. No cross-version fallback or shared cache of version-specific originals.
- **Performance with correctness:** measure before/after against equivalent workloads; preserve graphics, timing, security boundaries and storage behavior. No fake-success stubs or per-game hacks in shared paths.
- **One shared codebase:** isolate OS-specific adapters without duplicating games, system components, projects or large caches. Keep original inputs unchanged and use version-aware, content-addressed caching only where justified.
- **Honest evidence:** distinguish identified, loaded, initialized, rendering, interactive, gameplay, playable and fully functional. Test fixtures and successful compilation alone are not runtime evidence.

The planned graphics directions are **Xbox** (original behavior and fidelity) and **Native** (measured PC enhancements without visual or timing regressions). The current Windows ERA backend translates a subset of D3D11.X to D3D12; complete renderer coverage remains unfinished. Further boundaries and blockers: [docs/NEXT-TASK.md](docs/NEXT-TASK.md).

## Roadmap

Goals, not compatibility claims:

1. **M0:** identify legally readable system inputs and map imports/dependencies (currently: read-only XVD/PE inspector).
2. **M1:** present the **first authentic frame** drawn by an original SystemOS component with recorded evidence.
3. **M2–M3:** original shell, navigation, Guide, local profiles and persistent storage.
4. **M4:** GameOS/ERA execution through verified KernelX, Xbox WinRT and graphics/audio/input contracts.
5. **M5:** dashboard → game → Guide/dashboard, lifecycle and save/restore. The interim local dashboard already exercises this loop with the ERA runtime; the original SystemOS remains a separate future backend on the same contracts.
6. **M6:** validated additional host architectures, optimized graphics/performance and user-friendly distributions.

## FAQ

<details><summary><strong>Does “native” mean Xbox One software already runs like a Windows app?</strong></summary>
No. The CPU architecture is similar, but the Xbox APIs, application model, VM services and original shell still need implementation or isolation. No original SystemOS code has been executed by xone-native yet.
</details>

<details><summary><strong>Can I boot my Xbox One dashboard or play games today?</strong></summary>
Not the original dashboard: authentic SystemOS has not been executed. `xone-native` opens a locally recreated Xbox One-style dashboard that imports, checks and launches titles through the ERA runtime, but commercial games are not yet demonstrated as playable.
</details>

<details><summary><strong>Why not just copy WinDurango or the Xbox 360 project?</strong></summary>
[WinDurango](https://github.com/WinDurango/WinDurango) targets Windows-hosted ERA game compatibility and uses LGPL-3.0. It does not provide a finished original SystemOS boot path. x360-native targets a different console and PowerPC AOT pipeline. Reuse requires a technical and license review, not an unverified source copy.
</details>

<details><summary><strong>Does the project ship firmware or bypass Xbox protections?</strong></summary>
No. Do not submit or distribute Microsoft firmware, game files, proprietary assets, keys, personal saves or extracted protected content. No DRM-bypass or key-extraction mechanism is part of this project.
</details>

## Contributing and documentation

Focused code changes, synthetic fixtures, reproducible reports, upstream research and documentation improvements are welcome. Start with [CONTRIBUTING.md](CONTRIBUTING.md); coding agents must follow [AGENTS.md](AGENTS.md) and [CLAUDE.md](CLAUDE.md). Do not submit original Xbox files or account identifiers in issues or logs.

| Document | Purpose |
|---|---|
| [STATUS.md](STATUS.md) | Dated, evidence-based progress and known limitations |
| [M0 technical report](docs/M0.md) | Verified facts, architecture choice, first-frame blockers and next increment |
| [Documentation index](docs/README.md) | Technical reading order and reference links |
| [Agent rules](AGENTS.md) / [persistent context](CLAUDE.md) | Permanent requirements for code and tests |
| [Security](SECURITY.md) | Responsible vulnerability reporting |

## License and acknowledgements

Original xone-native code is under [BSD-3-Clause](LICENSE). Third-party components retain their own terms; none are vendored at M0. See the public research in [Xbox One Research](https://github.com/xboxoneresearch/wiki), [WinDurango](https://github.com/WinDurango/WinDurango) and public [Azimuth](https://almost-there-games.itch.io/azimuth) documentation. Azimuth's source is not available for reuse here. [x360-native](https://github.com/fabegalo/x360-native) is a separate, inspirational project.

Not affiliated with, sponsored or endorsed by Microsoft. Xbox and related names are trademarks of their owners. The banner is original abstract concept art, **not** a system or gameplay capture.
