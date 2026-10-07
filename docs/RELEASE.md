# Release procedure (macOS; Linux / Steam Deck in step 7; Windows in step 8)

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
   new version, and an installed previous version finds it via "Check for Updates…".
7. Linux / Steam Deck (same version; also bump `project(ReplayNESLinux VERSION ...)` in
   `apps/linux/CMakeLists.txt` and add a `<release>` to
   `apps/linux/flatpak/io.github.replaynes.ReplayNES.metainfo.xml`): `scripts/publish-flatpak-repo.sh`
   (see "Linux: Flatpak repository" below), then attach the bundle to the release:
   `gh release upload v<version> dist/io.github.replaynes.ReplayNES-<version>-x86_64.flatpak --repo kathoc/ReplayNES`.
8. Windows (same version, `project(ReplayNES VERSION ...)` already bumped in step 1): run the tests
   in the VM (`scripts/build-windows.sh && scripts/windows-vm/run-tests.sh`), then
   `scripts/release-windows.sh <version>` → `dist/ReplayNES-<version>-windows-{x64,arm64}.zip`
   (with `WinSparkle.dll`) and `dist/appcast-windows.xml`, signed with the same EdDSA key (see
   "Windows: WinSparkle feed" below). Upload all three to the release:
   `gh release upload v<version> dist/ReplayNES-<version>-windows-*.zip dist/appcast-windows.xml --repo kathoc/ReplayNES`.
   Check: `curl -sL https://github.com/kathoc/ReplayNES/releases/latest/download/appcast-windows.xml`.

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

## Windows: WinSparkle feed

- The Windows app (WinSparkle 0.9.4, `apps/windows/src/update_winsparkle.*`) reads
  `https://github.com/kathoc/ReplayNES/releases/latest/download/appcast-windows.xml`. **Every
  release that becomes "latest" must carry `appcast-windows.xml`** (also a macOS-only release: copy
  the previous Windows feed and assets, or Windows installs see no feed until the next one).
- Same format as `appcast.xml`; `sparkle:version` = the version string (`0.3.2`; WinSparkle compares
  it with the running `ReplayNES.exe`'s), one `<enclosure>` per architecture with
  `sparkle:os="windows-x64"` / `"windows-arm64"` and `sparkle:edSignature` = Sparkle's
  `sign_update` signature of the zip (the same EdDSA key as macOS; the Windows build embeds
  `SUPublicEDKey` from `apps/macos/project.yml`). WinSparkle refuses unsigned or wrongly signed
  updates ("The update is improperly signed").
- `scripts/release-windows.sh <version>` builds both zips (`scripts/build-windows.sh all`), checks
  that each contains `WinSparkle.dll`, signs them from the keychain (`SPARKLE_ACCOUNT`, checked
  against `SUPublicEDKey`) and writes the feed. The app installs a zip itself: WinSparkle verifies
  it, ReplayNES unpacks it and replaces its own folder after quitting (docs/WINDOWS.md).

### Local end-to-end update test (Windows VM, no GitHub)

```
# a throw-away key (never the release key) for both builds and the feed
python3 -c 'from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey as K; from cryptography.hazmat.primitives import serialization as s; import base64; k=K.generate(); open("build/update-test/seed.txt","w").write(base64.b64encode(k.private_bytes(s.Encoding.Raw,s.PrivateFormat.Raw,s.NoEncryption())).decode()); print(base64.b64encode(k.public_key().public_bytes(s.Encoding.Raw,s.PublicFormat.Raw)).decode())'
PUB=<printed public key>
# old version, then the new one + its feed served from the Mac (Parallels host address 10.211.55.2)
BUILD_BASE=$PWD/build/update-test/old DIST=$PWD/build/update-test/old-dist VERSION_OVERRIDE=0.4.0-test \
  EXTRA_CMAKE_ARGS="-DRNW_UPDATE_PUBLIC_KEY=$PUB -DREPLAYNES_BUILD_TESTS=OFF" scripts/build-windows.sh aarch64
VERSION_OVERRIDE=1 BUILD_BASE=$PWD/build/update-test/new DIST=$PWD/build/update-test/feed \
  ED_KEY_FILE=$PWD/build/update-test/seed.txt DOWNLOAD_BASE=http://10.211.55.2:8765 \
  EXTRA_CMAKE_ARGS="-DRNW_UPDATE_PUBLIC_KEY=$PUB -DREPLAYNES_BUILD_TESTS=OFF" scripts/release-windows.sh 0.3.2-test
(cd build/update-test/feed && python3 -m http.server 8765 --bind 10.211.55.2) &
# in the VM: unzip the old zip anywhere, then (PowerShell)
#   $env:REPLAYNES_APPCAST_URL='http://10.211.55.2:8765/appcast-windows.xml'
#   .\ReplayNES.exe --library-root <test library> --script "wait 5; update apply"
# ("update apply" = check and install without asking; "Check for Updates…" asks first)
```
Result: the old app quits, the folder holds 0.3.2-test (`ReplayNES.exe --version`), the new version
runs with the same arguments, `%LOCALAPPDATA%\ReplayNES\Update\update.log` lists the install. A
feed with a corrupted signature must end in WinSparkle's "The update is improperly signed" with
nothing installed. WinSparkle keeps its state in `HKCU\Software\ReplayNES\WinSparkle`.

## Linux: Flatpak repository (GitHub Pages)

Installs of the Linux Flatpak update from a signed OSTree repository served by GitHub Pages:
`https://kathoc.github.io/ReplayNES/flatpak/` (branch `gh-pages`, folder `flatpak/`; Pages source
`gh-pages` `/`). Users install with
`flatpak install --user https://kathoc.github.io/ReplayNES/flatpak/io.github.replaynes.ReplayNES.flatpakref`
or with the release bundle, which is built with `--repo-url` and the key, so it registers the same
remote. The app's update notice (Flatpak portal) and `flatpak update` both use it.

```
scripts/publish-flatpak-repo.sh            # build on the Deck, sign, update the repo, bundle, push gh-pages
NO_PUSH=1 scripts/publish-flatpak-repo.sh  # stop before the push (inspect build/gh-pages first)
SKIP_BUILD=1 ...                           # reuse the Deck's last build (~/ReplayNES-dev/repo)
TARGET=test scripts/publish-flatpak-repo.sh   # local test repo on the Deck (~/ReplayNES-dev/test-repo, file://)
```

What it does: builds with `scripts/build-linux-flatpak.sh` (no install, no bundle) on the Deck;
copies the published repository (gh-pages `flatpak/`, worktree `build/gh-pages`) to the Deck; adds
the build as a new commit with `flatpak build-commit-from --gpg-sign` (subject "ReplayNES
<version>"); `flatpak build-update-repo --gpg-sign --generate-static-deltas --prune
--prune-depth=KEEP-1` (signed summary, deltas, only the last `KEEP`=3 commits kept, the `.Debug`
extension is never published); writes `ReplayNES.flatpakrepo` and
`io.github.replaynes.ReplayNES.flatpakref` (base64 public key embedded, `RuntimeRepo` = Flathub);
builds `dist/io.github.replaynes.ReplayNES-<version>-x86_64.flatpak` from the signed commit with
`--repo-url` + `--gpg-keys`; copies the repository back and commits gh-pages as **one orphan commit,
force-pushed** (old repository objects never pile up in git history; the repo is ~10 MB). It enables
Pages on the first push (`gh api -X POST repos/kathoc/ReplayNES/pages`). Pages takes a minute or two
to deploy and its CDN caches files ~10 minutes, so clients can see the new summary a little later.

Check from a Linux machine / the Deck:
`flatpak remote-ls --user replaynes` (or the bundle's `replaynes-origin`), `flatpak update --user`.

Self-updates must not need new sandbox permissions: the portal refuses an update whose
`finish-args` add permissions ("requires new permissions"); such a release reaches users only through
`flatpak update` / Discover. Keep `finish-args` stable, or announce it.

### Signing key (GPG)

- Dedicated key "ReplayNES Flatpak Repo", RSA 4096, no expiry, fingerprint
  `5267770BF84CC0DA2FB9B8D093BCD9083B5DF142`. Public key: `apps/linux/flatpak/replaynes-repo.gpg`
  (also embedded in the `.flatpakrepo` / `.flatpakref` and in every bundle).
- Private key: **only on the Steam Deck**, GnuPG home `~/.local/share/replaynes-signing/gnupg`
  (mode 700), **without a passphrase** so that the publish script can sign unattended. Risk: anyone
  with access to that account (or a copy of the folder) can sign repository updates that every
  installed ReplayNES accepts. Keep the Deck's account protected, never copy the folder into a repo,
  backup or sync folder unencrypted, and **never commit the private key**.
- Backup (store it offline / in a password manager, then delete the file):
  ```
  ssh deck@steamdeck.local 'GNUPGHOME=~/.local/share/replaynes-signing/gnupg gpg --export-secret-keys --armor 5267770BF84CC0DA2FB9B8D093BCD9083B5DF142' > replaynes-repo-secret.asc
  ```
  The revocation certificate is in `~/.local/share/replaynes-signing/gnupg/openpgp-revocs.d/`; back it
  up the same way.
- Restore (new Deck / build host):
  ```
  mkdir -p ~/.local/share/replaynes-signing/gnupg && chmod 700 ~/.local/share/replaynes-signing ~/.local/share/replaynes-signing/gnupg
  GNUPGHOME=~/.local/share/replaynes-signing/gnupg gpg --batch --import replaynes-repo-secret.asc
  ```
  (`SIGN_HOME` / `GPG_KEY` override the location / key for `scripts/publish-flatpak-repo.sh`.)
- Losing the key: existing installs reject a repository signed by another key. Recovery = a new key,
  new `.flatpakrepo` / `.flatpakref`, and users re-add the remote
  (`flatpak remote-modify --user --gpg-import=<new key> replaynes`) or reinstall.

### Local end-to-end update test (Flatpak, on the Deck)

```
TARGET=test scripts/publish-flatpak-repo.sh            # version N into ~/ReplayNES-dev/test-repo (file://)
# on the Deck: install the test bundle (it points to the test repo)
flatpak install --user -y --bundle ~/ReplayNES-dev/test-io.github.replaynes.ReplayNES-<N>-x86_64.flatpak
# bump to N+1 locally (CMakeLists.txt, apps/linux/CMakeLists.txt, metainfo; do not commit), then
TARGET=test scripts/publish-flatpak-repo.sh
# portal check interval is 30 min: for the test run flatpak-portal with a short one (same ssh session)
systemctl --user stop flatpak-portal; /usr/lib/flatpak-portal --replace --poll-timeout=15 &
flatpak run io.github.replaynes.ReplayNES --script "updatewait available 120; update apply; updatewait installed 240; update restart"
flatpak ps --columns=pid,application,commit   # the restarted instance runs the new commit
```
Afterwards reinstall from the real repository (`.flatpakref` or the published bundle) and
`pkill -x flatpak-portal` (it is started again on demand).

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

- Release zips include `LICENSE` (GPL-2.0) and `THIRD_PARTY_NOTICES.md` (the Windows zips also
  WinSparkle's notices there, as it ships `WinSparkle.dll`).
- The tag must contain the submodule commit used for the build (corresponding source).
- Never attach ROMs or projects containing copyrighted material to a release.
