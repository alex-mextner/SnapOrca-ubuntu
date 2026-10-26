# Repository Guidelines

## Project Structure & Module Organization
Snapmaker_Orca’s C++17 sources live in `src/`, split by feature modules and platform adapters. User assets, icons, and printer presets are in `resources/`; translations stay in `localization/`. Tests sit in `tests/`, grouped by domain (`libslic3r/`, `sla_print/`, etc.) with fixtures under `tests/data/`. CMake helpers reside in `cmake/`, and longer references in `doc/` and `SoftFever_doc/`. Automation scripts belong in `scripts/` and `tools/`. Treat everything in `deps/` and `deps_src/` as vendored snapshots—do not modify without mirroring upstream tags.

## Build, Test, and Development Commands
Use out-of-source builds:
- `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release` configures dependencies and generates build files.
- `cmake --build build --target Snapmaker_Orca --config Release` compiles the app; add `--parallel` to speed up.
- `cmake --build build --target tests` then `ctest --test-dir build --output-on-failure` runs automated suites.
Platform helpers such as `build_linux.sh`, `build_release_macos.sh`, and `build_release_vs2022.bat` wrap the same flow with toolchain flags. Use `build_release_macos.sh -sx` when reproducing macOS build issues, and `scripts/DockerBuild.sh` for reproducible container builds.

### Ubuntu 26.04 fork (branch `ubuntu-26.04`) — host rules
- Builds and heavy tests (compiles, full ctest runs, stress loops, AppImage builds, GUI test runs) run on the build server `65.21.40.51`, never on the laptop: `scripts/ubuntu2604/server-build.sh --name <worktree>` rsyncs the tree, builds incrementally and runs ctest (`--cmd 'SHELL CMD'` runs a command instead, `--no-build` only ctest, extra args go to ctest). The server has 16 cores and one heavy job at a time: runs queue behind `~/build.lock`, and every concurrent run needs a distinct `--name`. Toolchain setup is `scripts/ubuntu2604/server-env.sh` (rootless; `server-build.sh` runs it when missing); see `doc/developer-reference/How-to-build.md`, section "Ubuntu 26.04".
- Never start Snapmaker Orca (AppImage or binary, GUI or CLI) directly on the development laptop: USB/HID enumeration there has hung the machine. Run it only in containers without device access (`scripts/ubuntu2604/gui-test/update-test.sh`, the smoke slice in `scripts/fork-sync/release.sh`) or on the build server (`server-build.sh --cmd`; the headless GUI smoke that proves the 3D canvas is drawn is `scripts/ubuntu2604/server-gui-smoke.sh APPIMAGE LABEL`, see How-to-build.md); never with `SO_DIRECT=1` on the laptop.
- What the user sees when they start Orca from the desktop menu is checked with `scripts/ubuntu2604/launcher-path/launcher-path.sh` (the installed AppImage through its own `AppRun`, real session environment and display, USB/HID hidden by bubblewrap, composited screenshots; How-to-build.md, "Launcher path"). A sandboxed `--appimage-extract-and-run` with a fresh or copied config on a stripped environment is not the launcher path and proves nothing about it.
- No heavy work on the laptop: no local compiles, full test-suite runs or stress loops.
- Runpod (`scripts/ubuntu2604/remote-build.sh`, `scripts/ubuntu2604/gui-test/remote-test.sh`) only when the user explicitly asks for a pod; then one pod at a time, never parallel pods, and stop it when done (`--session stop`). Don't build OrcaSlicer PRs on the server or Runpod: their CI builds and tests them.
- Remotes: `origin` is the fork repository `alex-mextner/SnapOrca-ubuntu` (all work lands here: `ubuntu-26.04`, `fix/*`, PRs, tickets); `upstream` is `Snapmaker/OrcaSlicer`, used only for the draft pull requests that go to Snapmaker (push the `upstream/*` branches to `origin`, open the PR with `--repo Snapmaker/OrcaSlicer`). The worktree-removal guard checks `refs/remotes/origin/*`, so work merged into the fork counts as landed. Tracked `node_modules` files under `resources/web/guide/swiper` and `resources/web/include/swiper` that show as deleted make a worktree "dirty": run `git checkout -- resources/web/guide/swiper resources/web/include/swiper` first. The guard has no bypass: push the branch, merge the PR, remove the worktree through the guard, then `git branch -d`.

## Coding Style & Naming Conventions
`.clang-format` enforces 4-space indents, a 140-column limit, aligned initializers, and brace wrapping for classes and functions. Run `clang-format -i <file>` before committing; the CMake `clang-format` target is available when LLVM tools are on your PATH. Prefer `CamelCase` for classes, `snake_case` for functions and locals, and `SCREAMING_CASE` for constants, matching conventions in `src/`. Keep headers self-contained and align include order with the IWYU pragmas.

## Testing Guidelines
Unit tests rely on Catch2 (`tests/catch2/`). Name specs after the component under test—for example `tests/libslic3r/TestPlanarHole.cpp`—and tag long-running cases so `ctest -L fast` remains useful. Cover new algorithms with deterministic fixtures or sample G-code stored in `tests/data/`. Document manual printer validation or regression slicer checks in your PR when automated coverage is insufficient.

## Commit & Pull Request Guidelines
The history favors concise, sentence-style subject lines with optional issue references, e.g., `Fix grid lines origin for multiple plates (#10724)`. Squash fixups locally before opening a PR. Complete `.github/pull_request_template.md`, include reproduction steps or screenshots for UI changes, and mention impacted presets or translations. Link issues via `Closes #NNNN` when applicable, and call out dependency bumps or profile migrations for maintainer review.

## Security & Configuration Tips
Follow `SECURITY.md` for vulnerability reporting. Keep API tokens and printer credentials out of tracked configs; use `sandboxes/` for experimental settings. When touching third-party code in `deps_src/`, record the upstream commit or release in your PR description and run the relevant platform build script to confirm integration.
