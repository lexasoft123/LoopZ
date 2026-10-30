# Pull-request DSP performance gate

Mac and Windows CI compare the complete pull request against its base commit,
using the PR merge result checked out by GitHub Actions. There is no commit hook,
no automatic local build and no performance workflow on ordinary branch pushes.
Superseded PR runs are cancelled when a newer update arrives.

The job reports success without compiling when the full PR changes only interface
or documentation files. Native code, CMake, pinned dependencies and gate changes
trigger the silent offline benchmark. Python 3.9+, Git, CMake 3.24+ and a C++20
toolchain are required for manual runs; CI supplies the platform toolchains.

The gate freezes exact Git trees and checks out their pinned dependency revisions.
For an optional staged comparison, the actual Git index is used; unstaged files
and dirty submodule files never enter either snapshot. Missing pinned submodule
objects fail the check; initialize/fetch dependencies before running manually.
The gate itself performs no network operations.

Both trees receive the same candidate benchmark workload and separate Release builds.
The workloads exercise 2/64 channels, 32/64/256-frame blocks, mixing and limiting,
with the limiter both enabled and disabled. Every workload verifies its expected
audio output; output hashes must also match across builds and repetitions.

After a discarded warm-up pair, five process pairs run in alternating order.
A profile fails the PR check when its median is more than **15% and 0.25 µs slower**
and at least **four of five matched pairs** exceed both limits. One timing outlier
does not hide a repeatable slowdown or create a failure by itself. Run on a quiet
machine; these thresholds do not eliminate all scheduler or thermal noise.

Run the gate manually, or compare committed revisions:

```sh
python3 scripts/performance_gate.py --force
python3 scripts/performance_gate.py --baseline HEAD^ --candidate HEAD --force
python3 -m unittest discover -s tests -p performance_gate_tests.py
```

Build failures, malformed/incomplete measurements, changed audio output and
reproducible slowdowns return a nonzero exit status. Inspect the printed
`artifacts/performance-gate/<run>/report.json` and raw JSONL results, fix the issue,
then retry. There is no automatic baseline acceptance or success-marker bypass.
Reports retain the two tree IDs, exact workload/binary hashes, per-run timings,
source snapshots and build logs. Retained runs can consume disk space; remove old
ignored artifact directories when their evidence is no longer needed.

Mark the Mac and Windows **DSP performance gate** jobs required in branch
protection if mandatory merge enforcement is desired. This workflow adds the
checks; repository branch-protection settings are managed separately.

This is an offline core/DSP CPU gate. It opens no audio endpoints, requests no
permissions, changes no buffers and installs nothing. It does not measure graphics,
installed Engine/system-service CPU, physical dropouts or end-to-end latency.
Existing correctness, hardware continuity and release acceptance checks still apply.
