LoopZ Windows preview

Extract this entire archive, then open LoopZ.exe.
Targets Windows 10 22H2 x64 and Windows 11 x64; physical acceptance currently
covers Windows 10. Microsoft Edge WebView2 Runtime displays the interface.

This preview captures microphones and selected applications, mixes their audio,
and sends mixes to selected playback outputs. A separate LoopZ driver provides
one playback/microphone pair per mix. Desktop-only ZIPs omit the driver; combined
bundles include driver\LoopZAudio.inf and its signed catalog. After creating
mixes, choose Settings -> Install or repair microphones. Windows asks for
administrator approval. Extracting the archive alone installs nothing.

With driver 0.3 or newer, add a Pass-through source to receive applications that
select the mix's playback device as their speaker. Incoming playback passes
through the saved mix before reaching its microphone. Hardware listening outputs
remain independent. The microphone follows the mix's selected sample rate and
channel count. Validation across every format and physical device is ongoing.

Source options include "Mute original app playback". This routes the selected
application away from its original speakers while keeping its audio in the mix.
LoopZ restores the application's prior playback choices when routing is released.
Installed tests cover two selected processes, exclusion and mute/unmute. First-use
setup, continuous handoff and restart/recovery still need acceptance testing.

Development bundles use a test-signed driver for an explicitly configured
workbench. Production distribution requires Microsoft driver signing. The setup
helper never enables test signing or changes Windows boot security. Driver
upgrades may still change Windows endpoint IDs; selection preservation remains
under validation.

Closing the window leaves routing running. Open LoopZ again to reconnect.
Choose Settings -> Stop routing and quit to stop the engine and exit.
Saved mixes remain available next time you open the app. Settings can enable
start at Windows sign-in. A background supervisor retries engine crashes;
explicitly stopping routing also stops that supervisor.
Before replacing preview files, stop routing and quit.

Developer diagnostics are included as loopz-windows-*.exe. Ordinary use does
not require Node.js, Python, Visual Studio, or these diagnostics.
