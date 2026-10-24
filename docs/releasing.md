# Releasing LoopZ

LoopZ follows SingZ's tag-triggered release model and disposable Developer ID
keychain mechanism. Its native AppKit/WebKit runtime does not use Electron's
Windows updater or electron-builder installer.

## Distribution

A DMG contains one signed LoopZ.app and an Applications shortcut. The app carries
its signed maintenance helper and sealed audio components. Choosing
Settings → Audio components registers its helper with Apple’s SMAppService;
macOS requires approval under Login Items & Extensions. The native helper verifies the app and payload, snapshots them into
protected storage, verifies the snapshot again, installs the audio components,
and starts only the LoopZ broker. Ordinary maintenance preserves Core Audio and
audio selections. Login startup remains a separate user choice, and the helper
never runs downloaded commands.

Install, update and repair share a journaled transaction. A failed update restores
the previous components; a later repair recovers an interrupted transaction.
The helper accepts only fixed operations from the signed LoopZ app and never
accepts caller-selected paths or commands. It reloads only a verified isolated
LoopZ driver host; first-time activation or a change to a driver hosted inside Core Audio uses an
explicit Finish audio setup action that briefly refreshes the audio service.
Ordinary install, update and repair requests never refresh that service
automatically. Saved routing resumes after verification.
The desktop app is copied by the user and ordinary app downloads are separate
from driver installation. Python is used only for builds, tests and release work.

## Credentials

Release signing currently runs locally; the tag workflow builds and validates
without receiving private signing credentials. Hosted signing is optional. To
enable it, set the repository variable `ENABLE_HOSTED_SIGNING=true` and configure
these GitHub Actions secrets, matching SingZ's existing names:

- APPLE_DEVELOPER_ID_CERTIFICATE_BASE64 — Developer ID Application P12.
- APPLE_DEVELOPER_ID_CERTIFICATE_PASSWORD — its export password.
- APP_STORE_CONNECT_API_KEY_BASE64 — Apple notary API P8, base64 encoded.
- APP_STORE_CONNECT_API_KEY_ID
- APP_STORE_CONNECT_API_ISSUER_ID

No credentials belong in source, release assets or job artifacts. The CI adapter
writes its credential files with restricted permissions in a temporary directory;
the signing code deletes its disposable keychain on exit.

Local builds use the existing ignored `.local/macos-signing.json`, whose
`certificateFile` and `passwordStore` refer to the SingZ certificate and SOPS
store. Alternatively, set LOOPZ_MAC_P12_PASSWORD for the certificate password.

## Build and verify

```sh
npm ci --ignore-scripts --prefix interface
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release --parallel 4
ctest --test-dir build/release --output-on-failure
python3 scripts/build_release.py --build build/release
```

The packager verifies every nested signature and runs the actual setup helper in
read-only `--verify-payload` mode before making the DMG. This is not a fresh-Mac
installation test. Perform that acceptance test before declaring general release
readiness; record actual observed third-party app compatibility too.

Submit an exact reviewed DMG with `scripts/notarize_image.py --record <json>
--sha256 <digest> --submit` inside the existing Apple credential wrapper. Follow
with the same arguments without `--submit` to check status. A saved submission ID
prevents accidental duplicate submissions. Accepted images are stapled, validated
and assessed with Gatekeeper. `scripts/release_ci.py` automates those build steps
on the CI runner.

## Publish

Update all bundle versions, `interface/package.json`, the CMake project and the
release script version together. Commit `docs/release-notes/v<version>.md` before
tagging; its first line is the release title, following SingZ's convention.
Push the exact tag to trigger `.github/workflows/release.yml`. The publish helper
requires notarized artifacts and a matching source commit. Preview releases
should be marked as prereleases until the acceptance gates pass.

The macOS updater checks GitHub's latest stable release after startup and every
six hours. Drafts and prereleases are not offered. It opens only LoopZ's fixed
release-download page, and never accepts executable commands or installation
paths from release JSON.

With hosted signing disabled, create and push the version tag after local
validation, then run `python3 scripts/publish_release.py --tag v0.2.3 --output
<notarized-artifact-directory>` to upload the exact notarized DMG and checksums.
