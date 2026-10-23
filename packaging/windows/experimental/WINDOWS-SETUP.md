# LoopZ 0.2.1 — experimental Windows preview

This release uses a **development-signed virtual microphone driver**, not a
Microsoft production-signed driver. Use a test PC you control. Windows 10 22H2
and Windows 11 on x64 are the preview targets; ARM64 is unsupported.

Windows Test Mode relaxes driver-signing trust for the whole PC, not just LoopZ.
It normally requires disabling Secure Boot and restarting. A Test Mode watermark
is expected. Do not proceed on a managed/work PC without its administrator's
approval. Keep your BitLocker recovery key available before changing firmware
settings; do not disable encryption or Memory Integrity to follow this guide.

## 1. Prepare Windows yourself

These are explicit administrator actions. LoopZ does not perform them silently.

1. Download the ZIP and `SHA256SUMS.txt` from the same GitHub release. In
   PowerShell, run `Get-FileHash .\LoopZ-0.2.1-windows-x64-experimental.zip` and
   compare its SHA-256 with the published checksum. Extract the entire ZIP to a
   permanent folder you own. Keep all files together.
2. Follow your PC manufacturer's instructions to disable Secure Boot in UEFI
   firmware settings, if enabled. If that is prohibited or unavailable, stop;
   this experimental driver is unsuitable for that PC.
3. Open **Terminal (Admin)** or **Command Prompt (Admin)** and run:

   ```cmd
   bcdedit /set testsigning on
   ```

4. Save your work and restart Windows. Enabling the setting without restarting
   is insufficient. LoopZ's setup checks the active kernel policy, not merely
   the setting for the next boot.

Microsoft documents these requirements in
[Loading test-signed code](https://learn.microsoft.com/en-us/windows-hardware/drivers/install/the-testsigning-boot-configuration-option).
If Windows still blocks the driver, keep the error for diagnosis. Do not disable
signature enforcement, install a random certificate or change additional
security settings to force it to load.

## 2. Install LoopZ's microphone components

1. Close LoopZ and apps recording from its microphones before updating.
2. Right-click **Install.cmd** and choose **Run as administrator**.
3. Read the notice and type `INSTALL EXPERIMENTAL LOOPZ` to consent. Setup checks
   the payload hashes and development certificate identity before importing the
   public certificate into this PC's Trusted Root and Trusted Publishers stores
   and calling LoopZ's native microphone installer. No private key is included.
4. If setup requests a restart, save your work and restart manually.
5. Open **LoopZ.exe normally**, without administrator rights. Windows may show an
   unknown-publisher or download warning because this preview has no public
   application-signing reputation. Only continue for your verified release.
6. If Microsoft WebView2 Runtime is missing, install it from
   [Microsoft's WebView2 page](https://developer.microsoft.com/microsoft-edge/webview2/)
   and reopen LoopZ. There is no Python or developer-tool requirement.
7. Create or open a mix and choose its LoopZ microphone in your recording app.
   Existing saved mixes are retained; automatic login/startup remains your choice.

The development certificate thumbprint is
`FEC5B498B1777655AF90E665A0856E2755212078`. Its current expiry is October 2027;
this package does not promise indefinite availability of development signing.
Keep this extracted folder: it contains the native repair/removal tool.

## Known preview limits

- On the Windows 11 VM, adding another microphone temporarily changes Windows'
  automatic default microphone. Check your input selection after creating mixes.
- High-channel format switching remains under investigation. Short 48 kHz stereo
  paths pass on the Windows 10 Dell and the Windows 11 VM console. Hardware and
  third-party app coverage is incomplete; no end-to-end latency is promised.
- Remote Desktop can hide local LoopZ audio devices behind Remote Audio. Use the
  local desktop or VM console for local microphone checks.
- Driver upgrades can require reselecting the microphone in recording apps.

## Remove LoopZ and restore normal security

1. Close LoopZ and apps using its microphones. Right-click **Remove.cmd**, choose
   **Run as administrator**, then type `REMOVE LOOPZ`. This calls the native
   uninstaller for LoopZ microphones and its device service; saved mixes remain.
   If removal fails, keep the folder and retry after closing recording apps.
2. After successful removal, remove this development certificate from both
   stores in **Terminal (Admin)** if no other LoopZ development build needs it:

   ```cmd
   certutil -delstore Root FEC5B498B1777655AF90E665A0856E2755212078
   certutil -delstore TrustedPublisher FEC5B498B1777655AF90E665A0856E2755212078
   bcdedit /set testsigning off
   ```

3. Save your work and restart Windows. Re-enable Secure Boot in firmware if it
   was enabled before this experiment. Other development drivers may also need
   Test Mode; check those dependencies first.
4. Delete the extracted LoopZ folder when no longer needed. Do not delete your
   saved mix folder unless you intend to erase your mixes.

Neither installation nor removal automatically changes boot settings, firmware,
encryption, login choices, system audio defaults or reboots the PC. Windows'
known automatic-default behavior above remains a preview limitation.
