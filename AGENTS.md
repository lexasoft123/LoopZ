# LoopZ — development instructions

LoopZ routes application and hardware audio through a native background Engine
and virtual audio driver. The macOS desktop uses AppKit/WebKit with a React
interface and the SingZ design system. Keep the audio core portable; platform
capture, device management, permissions and installation belong in adapters.

This workflow adapts SingZ's `AGENTS.md` conventions and
`docs/DEVELOPMENT.md` sections on worktrees, verification and binary provenance.
Use LoopZ's commands and distribution model below, rather than SingZ's Electron
or mobile tooling. A sibling SingZ checkout is not required to build LoopZ.

## Feature workflow and worktrees

1. Inspect the branch, working tree and relevant documentation before editing.
   Establish what success means, including the affected platforms and checks.
2. Use a separate Git worktree and a `codex/<feature>` branch for each parallel
   feature. Reuse a suitable task-owned worktree when available; prefer the
   managed Codex worktree tools. Start new work from the appropriate current
   base, usually `origin/main`, after fetching it.
3. Never switch another task's branch, reset its checkout, overwrite its edits
   or sweep its changes into a commit. If a checkout already contains mixed
   work, stage only the intended changes and validate that staged version.
4. Bootstrap each worktree with its own interface dependencies and CMake build
   directory. Do not share mutable `build/`, `node_modules`, staged applications
   or generated native executables between worktrees. Tracked, pinned vendor
   inputs are dependencies, not a place for incidental fixes.
5. Implement a focused change, run appropriate checks, review and commit the
   exact staged result, then push within the user's authorized scope. Integrate
   the feature back into `main` after review and passing required checks; use a
   pull request when requested or required by repository rules. Publishing a
   feature branch does not mean it has been merged or released.

Do not copy SingZ's `scripts/worktree-setup.sh` commands into this workflow:
LoopZ does not currently have that bootstrap script. Use the setup commands
below in the selected worktree.

## Review before every commit

- Stage exactly what is intended to ship, in a separate operation from commit.
  Inspect `git diff --cached` and `git diff --cached --check`.
- Have an independent reviewer agent review the staged diff before committing.
  Give it the scope, validation results and relevant risks. It should review
  without changing files. Address findings, or record why a finding does not
  apply, before committing. If an independent reviewer is unavailable, report
  that limitation rather than claiming the review happened.
- Review applies to the exact staged tree. Record `git write-tree` with the
  review result; changing the index invalidates that review. Recheck and review
  the changed result before committing.
- Commit the index with plain `git commit`. Avoid `-a`, `-am`, `--all`,
  `--include`, `--only`, `--patch`, and commit path arguments: these can commit
  a different tree from the one reviewed. Do not combine staging and committing
  in one command.
- Review conflict resolutions before merge, rebase or cherry-pick continuation
  creates a commit. Do not force-push shared history unless explicitly authorized.

SingZ enforces its review workflow with a hook and a reviewed-tree marker.
Those hooks and reviewer definitions have **not** been installed in LoopZ.
This file establishes the workflow; do not claim an automated gate exists or
create a fake approval marker. A hook is a separate implementation task.

## Build and checks

Requirements: CMake 3.24+, C++20, Python 3, Node.js and the macOS toolchain
described in [README.md](README.md). Use pinned dependencies and lockfiles.

```sh
npm ci --ignore-scripts --prefix interface
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release --parallel 4
ctest --test-dir build/release --output-on-failure --parallel 2
npm test --prefix interface
npm run format:check --prefix interface
npm run build --prefix interface
```

For portable-core changes, also use the core-only configuration:

```sh
cmake -S . -B build/core -DLOOPZ_BUILD_MACOS_BACKEND=OFF -DCMAKE_BUILD_TYPE=Release
cmake --build build/core --config Release --parallel 4
ctest --test-dir build/core -C Release --output-on-failure
```

Native/Python formatting is defined by `scripts/format_code.py` and
`pyproject.toml`; native formatting currently pins clang-format 22.1.8.
Use `python3 scripts/format_code.py --check` with the required tools installed.
Frontend formatting uses the locked Prettier version. Format the files changed
by the task without rewriting unrelated concurrent work.

Run focused checks while iterating and the relevant broader checks before
delivery. Tests should catch behavior, failure cases or regressions rather than
repeat the implementation. A documentation-only change needs link/command and
diff checks, not a native rebuild. Consult the actual workflow files under
`.github/workflows/` for CI requirements. Check the selected backend's platform
documentation and current acceptance evidence before claiming platform support.

## Verify the actual application and binary

- Build before testing. A native source change requires a rebuilt native
  executable and, for installed acceptance, deployment of that exact signed
  build. A refreshed web bundle cannot validate a changed Engine or driver.
- Identify the tested executable, source revision or snapshot, architecture,
  build configuration and installed signature/hash. A version string alone
  cannot distinguish two builds from different worktrees. Never select a test
  target by whichever process or debugger endpoint answers first.
- Prefer the native editor/debug contract and existing automation harnesses to
  manual clicks. Use [docs/editor-control.md](docs/editor-control.md) for the
  actual contract. For interface changes, use the relevant browser tests and
  real WebKit harnesses in `interface/scripts/`; inspect screenshots and verify
  the visible result. Synthetic/browser checks do not prove installed audio.
- Keep scratch experiments, recordings, profiles and diagnostic reports in
  ignored `artifacts/`. Commit reusable regression tests in `tests/` or
  `interface/tests/`. Preserve the exact binaries and fixtures used by a run so
  another build cannot silently replace them during measurement.
- Report automated, installed, hardware and third-party-app verification
  separately. A successful build, mock graph or permission timeout is not proof
  of live capture, visible consent or release readiness.

## Audio safety and performance evidence

- Preserve saved mixes, default devices, login choices and existing consent.
  Ordinary app updates must remain separate from privileged driver/broker
  maintenance. Use the app's native install/update/repair flow for users;
  developer Python scripts are not an end-user installation requirement.
- Do not restart Core Audio, reboot, stop unrelated audio clients, or change
  shared hardware buffers/rates without authorization for that action. When a
  temporary experiment is authorized, record its original state and restore it
  even after failure. Keep signing identities stable; never bypass privacy
  controls or put signing credentials in source or logs.
- Automated audio tests should be silent by construction unless the user has
  requested an audible check. Do not mute the user's system output to silence a
  test, and do not substitute silent output when audible continuity is what the
  test must establish.
- Realtime callbacks must not allocate, block, log, query hardware properties,
  perform file/network operations or destroy shared resources. Prepare work on
  control threads and publish it safely. Recovery retains the desired full
  graph and retries lost sources and monitors independently.
- Compare CPU with the same graph, devices, rates, buffers, active clients and
  measurement duration. Separate Engine, graphics, Core Audio and USB audio
  usage; system-service CPU is shared across applications. Do not infer
  causality from one noisy sample or count sleeping stack samples as CPU work.
- Stack sampling can disturb very small audio deadlines. Validate continuity
  without an attached profiler after warm-up, using counter deltas for missing
  frames, discontinuities, timestamp errors and missed deadlines. A CPU saving
  is not a success if it introduces dropouts or additional audio buffering.
- Distinguish one hardware buffer, a configured source/history budget, reported
  device latency and measured end-to-end latency. Record the measurement path
  and exclusions; never present a buffer-size calculation as real round-trip
  latency. See [docs/latency-comparison.md](docs/latency-comparison.md) and
  [docs/validation.md](docs/validation.md).

## Interface and dependency conventions

- Use the pinned `singz-ui` package and existing design tokens/components.
  Keep novice-facing copy clear, sentence-case and focused on the action and
  its effect. Put technical diagnostic details in the appropriate details view.
- Maintain the existing English, Russian and Simplified Chinese localization
  structure when changing user-visible controls.
- Keep meters smooth without forcing whole-page renders. Gate visual updates
  on visible changes; pause unnecessary animation when hidden or obscured.
- IPC/editor operations must return useful structured errors. Validate requests
  at the native boundary and retain existing authentication and path checks.
- Change vendored SingZ code only through the repository's explicit dependency
  update/overlay mechanism, keeping provenance and hashes consistent.

## Releases

Follow [docs/releasing.md](docs/releasing.md). LoopZ uses native maintenance
components and DMG distribution; SingZ's Electron/NSIS/mobile release commands
are not interchangeable with this repository's tooling.

Prepare version changes and `docs/release-notes/v<version>.md` before tagging.
Review what changed since the preceding release; write notes for users, with
the first line serving as the release title. Keep bundle, interface, CMake and
release-script versions consistent. Publish only from the reviewed source
commit, with matching signed/notarized artifacts and checksums.

Release tags and publication require authorization within the release task.
Passing CI or preparing a package does not authorize an unrelated release.
Verify clean installation, update/repair, saved-graph recovery and promised
platform/client compatibility before calling a release stable. Record missing
acceptance evidence and keep preview builds labeled accordingly.
