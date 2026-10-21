# Native audio maintenance acceptance

Validated on physical Apple silicon and Intel Macs on 21 October 2026.

| Check | Apple silicon, macOS 27.0.1 | Intel, macOS 15.7.9 |
| --- | --- | --- |
| Install through signed SMAppService helper | Passed | Passed |
| Activate without restarting the Mac | Passed | Passed |
| Publish a public two-channel input | Passed | Passed |
| Native update and saved graph restoration | Passed | Passed |
| Virtual output at 64 frames, three seconds | Passed | Passed |

The virtual audio checks reported no timestamp errors or render failures and
opened no physical devices. They validate callback operation, not total
microphone-to-speaker latency.

On Apple silicon, removing only the installed Engine and choosing native repair
restored it, retained the saved routing intent and selected virtual input, and
preserved both the driver inode and Core Audio process. Internal driver-format
receipts may be refreshed when the restored Engine republishes its devices.

The first driver activation explicitly refreshed the audio service. Subsequent
updates reloaded only LoopZ's isolated driver host. Neither Mac rebooted. Intel
macOS uses a distinct Apple driver-host identity; both driver and broker now
accept that verified identity alongside the newer host.

Automated validation included all 121 native tests, the focused maintenance and
session tests after final changes, the isolated setup/approval/update/repair UI
test, interface tests and release-distribution checks. The Intel native test
harness also runs directly on Intel hardware.

Maintenance refuses running LoopZ clients and active aggregate routes. The app
asks users to close those clients before trying again. Microphone consent and
macOS's initial background-helper approval remain system-controlled choices.
