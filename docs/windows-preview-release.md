# Windows workbench preview

Prepared 2026-10-23. The development ZIP is ready for the explicitly configured
Windows 10 Dell and Windows 11 VM. It is not a production signed installer or a
stable public release. No exhaustive format tests or optional optimizations are
running.

## Frozen candidate

App/control/helper/broker R644 use frozen source R643, Release x64 with the MSVC
runtime linked statically. Driver 0.4.0.25 is built as R621 and development-signed
as R623; the interface remains R462. R643 differs from tested R620 only in the
CMake runtime setting. The clean VM exposed the earlier broker's dependency on
absent global Visual C++ runtime DLLs (exit C0000135); the self-contained build
fixes installation without adding a machine-wide runtime.

R644 passes all 48 selected checks. The four executable import tables contain no
dynamic Visual C++ runtime imports. R645 packages those exact binaries. R655
updates only the notes and acceptance metadata; all executable and driver bytes
remain unchanged. The dirty checkout and later unverified source changes are
excluded. A reviewed source commit is still required for a public release.

## Installed evidence

- Windows 10 22H2 Dell: R648 updates the native service and app without rebooting;
  saved routing, defaults and microphone identity are preserved. R649 passes a
  15-second 48 kHz stereo PCM16 recording and independent waveform/native-flow
  audit on the actual installed R644 build.
- Windows 11 VM: R646 native service installation and normal-user launch pass.
  After switching from Remote Desktop to the VM console, the saved microphone
  reconnects and the exact installed kernel image is verified. R654 passes a
  15-second 48 kHz stereo PCM16 recording through that existing microphone,
  with zero recording gaps, capture discontinuities, position gaps or timestamp
  errors. Temporary source/gain edits are restored; saved document bytes,
  microphone identity and system defaults are preserved. Its independent audit
  separates waveform results from the existing worker's lifetime counters:
  one deadline miss and two restarts already exist in the earlier console
  baseline and do not increase. The native packet-discontinuity counter grows
  from zero to one across the broader baseline window; no claim of all-zero
  lifetime counters or strict fresh-worker acceptance is made.

## Known limits

R653's temporary new microphone causes Windows in this VM to change its automatic
capture default during the test. Cleanup restores the original default and
saved document. This is a failed default-preservation check; R654's existing-mic
result does not supersede or accept new-endpoint behavior. Remote Desktop exposes
Remote Audio rather than validating console endpoint access.

The earlier .25 PCM16 matrix stops after 109 passing profiles while the
96 kHz / 28-channel endpoint remains in setup. Full current-driver coverage is
not accepted. Full float and five-minute continuity results apply to the older
.24 candidate. Hardware and third-party client compatibility remain incomplete;
no current-build end-to-end latency claim is included.

## Deliverable

`artifacts/windows/workbench-preview-r655/LoopZ-0.2.0-windows-x64-workbench.zip`
contains release notes, a hash manifest and `ACCEPTANCE.json`. SHA-256:
`521b10afe5e18db4c366fbf82d821da2ccb29e199cbb31ea646acec558b229e4`.
The previously delivered R652 ZIP is retained as historical evidence; use R655
for the updated acceptance record. Both have identical executable/driver bytes.

See [the detailed Windows record](windows.md) and
[preview release notes](release-notes/v0.2.0-windows-preview.md).

## Before public distribution

Obtain production application/installer signing and Microsoft driver signing;
keep the production packaging signature checks enabled. Verify the real signed
installer's clean install, upgrade/repair and uninstall behavior, resolve or
constrain the default-selection issue, and review the exact release source
commit. Keep component versions and release notes aligned. The user has subsequently authorized v0.2.1 as an experimental GitHub
prerelease with development signing. Follow the explicit setup guide and
experimental packager in docs/releasing.md; this does not authorize calling the
package production-signed or stable. Signing research is
available in the separate user-requested chat.
