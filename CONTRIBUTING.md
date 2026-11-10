# Contributing to xone-native

This is an experimental research project, **not** a game-ready emulator. Small, measurable contributions are welcome: synthetic regression tests, read-only binary inspection, architecture research based on public sources, build fixes, documentation and evidence-backed compatibility improvements.

Read [STATUS.md](STATUS.md) and [docs/M0.md](docs/M0.md) first. Coding agents must follow [AGENTS.md](AGENTS.md) and [CLAUDE.md](CLAUDE.md), including scope restrictions and focused tests. The original Xbox One dashboard is a primary goal; synthetic screenshots or replacement shells never qualify.

## Build and test

Use CMake and a C++20 compiler. The current repository has no mandatory external runtime dependency:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build -C Release -R "inspection_regressions|cli_help" --output-on-failure
```

Documentation-only changes require link, language and consistency checks rather than recompiling the project. For C++ changes, use the affected tests, record the exact commands and results, and add a focused regression for any corrected behavior. Do not validate against stale binaries after a failed build. Do not run full visual/game suites without the maintainer's authorization.

## Report findings honestly

Include project commit, host OS/architecture, steps, expected/observed result and sanitized logs. When using legally obtained content, identify exact version and hashes **without attaching binaries**. `Identified`, `Loaded`, `Initialized`, `Rendering`, `Interactive`, `Gameplay`, `Playable` and `Fully Functional` are different states; report only observed ones. A compiled target, recognized header or synthetic frame is not a booted dashboard.

For an original system component, preserve SystemOS build, provenance, version and SHA-256 associations. Avoid cross-version fallback, untracked DLL copies and silently missing services. Keep original files outside the repository and unchanged. Never submit firmware, XVD/XVC content, games, Microsoft assets, keys, account data, saved games, circumvention code or proprietary dumps.

## Code structure and licensing

The shared C++ library is in `include/` and `src/`; regression tests belong in `tests/`; architecture findings belong in `docs/`. Do not create platform forks or duplicate content pipelines. Reuse code only after reviewing its license and compatibility; WinDurango is LGPL-3.0, while original project code is BSD-3-Clause. Azimuth is a public reference, not code available to import.

Use Conventional Commits (`test(inspect): reject truncated XVD metadata`). Never add artificial co-author signatures for coding assistants; preserve real upstream attributions. Maintainer direction for an authorized task may request a direct commit to `main`, but contributors should not rewrite the published history, force-push or publish Releases without explicit permission.

For potential vulnerabilities, read [SECURITY.md](SECURITY.md) before posting details publicly.
