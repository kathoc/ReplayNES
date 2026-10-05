# Release procedure (macOS)

1. Bump `MARKETING_VERSION` / `CURRENT_PROJECT_VERSION` in `apps/macos/project.yml`.
2. Run engine tests: `cmake -G Ninja -S . -B build -DCMAKE_BUILD_TYPE=Release && ninja -C build && ctest --test-dir build`
3. Run app tests: `scripts/test-macos.sh`
4. Build: `scripts/build-macos.sh` → `build/ReplayNES.app`, `dist/ReplayNES-<version>-macOS-arm64.zip`
   (ad-hoc signed; zip contains the app, README, LICENSE, THIRD_PARTY_NOTICES).
5. Tag `v<version>` and create a GitHub release with the zip attached:
   `gh release create v<version> dist/ReplayNES-<version>-macOS-arm64.zip --title "ReplayNES <version>" --notes-file <notes>`

## Developer ID signing + notarization (when a Developer ID is available)

```
codesign --force --deep --options runtime --timestamp \
  --sign "Developer ID Application: <NAME> (<TEAMID>)" build/ReplayNES.app
ditto -c -k --keepParent build/ReplayNES.app ReplayNES.zip
xcrun notarytool submit ReplayNES.zip --keychain-profile <profile> --wait
xcrun stapler staple build/ReplayNES.app
spctl --assess --type execute -vv build/ReplayNES.app
```
Re-zip the stapled app for distribution. The app needs no special entitlements (no network, no camera/mic).

## License compliance checklist

- Release zip includes `LICENSE` (GPL-2.0) and `THIRD_PARTY_NOTICES.md`.
- The tag must contain the submodule commit used for the build (corresponding source).
- Never attach ROMs or projects containing copyrighted material to a release.
