# BlackHole timing and installer review

Reviewed upstream commit `561d934c77a1fa64987e01ccabe890fed30e181f` from the
project's official repository. The checkout was inspected in a temporary
research directory; BlackHole was neither installed nor executed, and none of
its source was copied into LoopZ's implementation.

## Timing findings

BlackHole's default added-latency setting is zero. It transfers application
output to input through a shared sample-clock ring, indexing both directions
by their respective Core Audio cycle sample times. Its ring holds 65536 frames
by default while its zero-timestamp period is 16384 frames. These values describe
storage and clock-anchor spacing; they do not imply an equally large queued
audio delay. Reducing LoopZ's clock-anchor period alone is therefore not a
justified latency fix. Explicit lookback and readback queues must be measured
and tuned independently.

BlackHole's advertised zero additional driver latency is for a virtual cable.
It does not establish zero end-to-end app latency and is not a measurement of
LoopZ's process tap, independent microphone clock, mixing graph or hardware
monitor. LoopZ's timestamped app-route results remain the comparison evidence.

## Installation findings

The package preinstall creates and secures the HAL directory; postinstall sets
root ownership and permissions on the driver bundles. The installer welcome
page explicitly calls for a system restart, and the developer instructions
restart Core Audio after copying a built driver. The uninstaller also invokes a
Core Audio reset. This does not supply a supported restart-free reload method.
LoopZ's isolated-host reload remains a separately reviewed macOS-specific
experiment with exact mapped-image identity, authenticated peer PID, precise
process generation, independent hardware continuity, and rollback checks.

## Integration boundary

The upstream project is GPL-3.0, and its authors request a separate license for
non-GPLv3 integration. This review adopts architectural observations, not their
implementation. LoopZ continues to use its independently adapted Apple driver
and the pinned SingZ zcore/zdsp code.

## Sources

- [Pinned driver source](https://github.com/ExistentialAudio/BlackHole/blob/561d934c77a1fa64987e01ccabe890fed30e181f/BlackHole/BlackHole.c)
- [Installer scripts](https://github.com/ExistentialAudio/BlackHole/tree/561d934c77a1fa64987e01ccabe890fed30e181f/Installer/scripts)
- [Installer welcome](https://github.com/ExistentialAudio/BlackHole/blob/561d934c77a1fa64987e01ccabe890fed30e181f/Installer/welcome.html)
- [Project and integration guidance](https://github.com/ExistentialAudio/BlackHole)
