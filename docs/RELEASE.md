# Release procedure (macOS)

1. Bump the version by hand: `MARKETING_VERSION` / `CURRENT_PROJECT_VERSION` (must increase every
   release; Sparkle compares it as `sparkle:version`) in `apps/macos/project.yml`, and
   `project(ReplayNES VERSION ...)` in `CMakeLists.txt`.
2. Run engine tests: `cmake -G Ninja -S . -B build -DCMAKE_BUILD_TYPE=Release && ninja -C build && ctest --test-dir build`
3. Run app tests: `scripts/test-macos.sh`
4. Build and prepare the update feed: `scripts/release.sh <version>`
   - runs `scripts/build-macos.sh` → `build/ReplayNES.app`, `dist/ReplayNES-<version>-macOS-arm64.zip`
     (ad-hoc signed; zip contains the app, README, LICENSE, THIRD_PARTY_NOTICES)
   - signs the zip with Sparkle's `sign_update` (EdDSA key from the login keychain) and writes
     `dist/appcast.xml` (one item; enclosure = this release's zip asset URL; release notes link =
     the GitHub release page; minimum macOS = `LSMinimumSystemVersion`)
   - prints the commands below; it does not tag, push or upload anything.
5. Tag and publish (the release must be the **latest** release, and must carry `appcast.xml`):
   ```
   git tag v<version> && git push origin v<version>
   gh release create v<version> dist/ReplayNES-<version>-macOS-arm64.zip dist/appcast.xml \
     --repo kathoc/ReplayNES --title "ReplayNES <version>" --notes-file <notes>
   ```
6. Check: `curl -sL https://github.com/kathoc/ReplayNES/releases/latest/download/appcast.xml` shows the
   new version, and an installed previous version finds it via 「アップデートを確認…」.

## Automatic updates (Sparkle 2)

- Feed: `SUFeedURL` = `https://github.com/kathoc/ReplayNES/releases/latest/download/appcast.xml`
  (set in `apps/macos/project.yml`). Every release uploads its own `appcast.xml`, so "latest" always
  serves the newest item. Do not mark a release as pre-release/draft if it should be offered.
- Signing key: EdDSA (ed25519). The **private key is only in the release machine's login keychain**
  (Keychain Access → item "https://sparkle-project.org", account `ReplayNES`); the public key is
  `SUPublicEDKey` in `project.yml`. `scripts/release.sh` aborts if the two do not match.
  Back it up (`.../Sparkle/bin/generate_keys --account ReplayNES -x <file>`, store offline, then delete
  the file); losing it means existing installs can never be updated automatically again. Restore on a
  new machine with `generate_keys --account ReplayNES -f <file>`.
  Sparkle tools live in `build/DerivedData-release/SourcePackages/artifacts/sparkle/Sparkle/bin/`
  after a build.
- Ad-hoc code signing: Sparkle accepts an update if its EdDSA signature verifies against the
  *installed* app's `SUPublicEDKey`; the Apple code signature only has to be valid (the ad-hoc,
  cdhash-based identity is allowed to change between versions). It refuses an update that is unsigned
  when the installed app is signed, so every release must stay (at least) ad-hoc signed.
  `scripts/build-macos.sh` signs inside-out per component (Sparkle's XPC services, `Autoupdate`,
  `Updater.app`, the framework, then the app) instead of `codesign --deep`.
  If a Developer ID becomes available, keep the same EdDSA key; switching the code-signing identity is
  then allowed by Sparkle.
- Never remove `SUPublicEDKey` or change it without a key-rotation release (Sparkle rejects updates
  that drop the key).
- Users are asked for permission before the first automatic check (Sparkle's standard prompt on the
  second launch). No system profile is sent.

### Local end-to-end update test (no GitHub)

```
# "new" build: e.g. bump to a test version, scripts/build-macos.sh, then
SKIP_BUILD=1 ZIP=<new zip> APP=<new app> DOWNLOAD_URL=http://127.0.0.1:8765/<zip name> \
  APPCAST=<dir>/appcast.xml scripts/release.sh <new version>
cp <new zip> <dir>/ && (cd <dir> && python3 -m http.server 8765 --bind 127.0.0.1) &
# run a copy of the *old* app (not /Applications) against the local feed and install immediately
<old>/ReplayNES.app/Contents/MacOS/ReplayNES -SUFeedURL http://127.0.0.1:8765/appcast.xml \
  -SUAutomaticallyUpdate YES -SUEnableAutomaticChecks YES --update-check-now
/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' <old>/ReplayNES.app/Contents/Info.plist
```
The `-SU...` arguments live only in the argument domain (not persisted). Sparkle still writes
`SUHasLaunchedBefore` / `SULastCheckTime` to the `io.github.replaynes.ReplayNES` defaults and caches
to `~/Library/Caches/io.github.replaynes.ReplayNES`; remove them afterwards if needed.

## Developer ID signing + notarization (when a Developer ID is available)

```
SIGN_IDENTITY="Developer ID Application: <NAME> (<TEAMID>)" scripts/build-macos.sh
# SIGN_IDENTITY is used for every component (Sparkle helpers/framework get hardened runtime).
# Not automated yet for notarization: the app itself also needs --options runtime and a secure
# --timestamp (build-macos.sh currently passes --timestamp=none).
ditto -c -k --keepParent build/ReplayNES.app ReplayNES.zip
xcrun notarytool submit ReplayNES.zip --keychain-profile <profile> --wait
xcrun stapler staple build/ReplayNES.app
spctl --assess --type execute -vv build/ReplayNES.app
```
Re-zip the stapled app for distribution. The app needs no special entitlements (it is not sandboxed; network access is only used by Sparkle).
After re-zipping, re-run `sign_update` / `scripts/release.sh` steps so the appcast signature matches the final zip.

## License compliance checklist

- Release zip includes `LICENSE` (GPL-2.0) and `THIRD_PARTY_NOTICES.md`.
- The tag must contain the submodule commit used for the build (corresponding source).
- Never attach ROMs or projects containing copyrighted material to a release.
