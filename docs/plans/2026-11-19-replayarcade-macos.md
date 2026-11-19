# ReplayArcade for macOS (first step: a build that runs on macsrv)

2026-11-19, user request ("ArcadeのMac版もビルドして"; confirmed: make a new ReplayArcade Mac app from
ReplayNES's Mac app, as the iPhone app was made). Distribution (signing, notarization, Sparkle feed,
release.sh) is a later decision and is **not** part of this step.

## Goal

`products/ReplayArcade/scripts/build-macos.sh` builds `ReplayArcade.app` (arm64, macOS 14+) from the
monorepo: ReplayNES's Mac app (apps/macos) with ReplayArcade's product part, the same way
`products/ReplayArcade/packaging/ios` + `scripts/build-ios.sh` + `scripts/build-ios-engine.sh` make the
iPhone app from apps/ios. The app opens an arcade ROM set (FinalBurn Neo .zip), plays it, records, rewinds,
saves and reopens a ReplayArcade project (.arcrec), on macsrv.

## Requirements

1. **ReplayNES's Mac app is unchanged in behaviour.** Its tests (`scripts/test-macos.sh` /
   the ReplayNES scheme's tests) keep passing; its bundle id, name, file types and Sparkle feed stay.
2. **Engine**: like build-ios-engine.sh, one static library `libreplaynes_all.a` built by CMake from
   products/ReplayArcade (`replaynes_engine replaynes_frontend replayarcade_core replayarcade_startup
   fbneo_build fbneo_libretro_common`), for macOS arm64, deployment target 14.0, Release always. Output under
   `products/ReplayArcade/build-macos/`. The generated `product.h` (generated/replaynes) is on the header path.
3. **Xcode project**: `products/ReplayArcade/packaging/macos/project.yml` (XcodeGen). Prefer reusing
   apps/macos/project.yml by splitting it the way apps/ios is split (`common.yml` shared + `project.yml`
   ReplayNES's product part), so ReplayArcade's spec only overrides the product part. If a split is needed,
   ReplayNES's generated project must stay equivalent (same targets, settings, sources).
   ReplayArcade overrides: PRODUCT_NAME ReplayArcade, bundle id `io.github.kathoc.replayarcade.mac`,
   display name, copyright ("ReplayArcade - personal build. Includes ReplayNES (GPL-2.0-or-later) and
   FinalBurn Neo (non-commercial licence)."), icon (reuse packaging/ios/Assets.xcassets' AppIcon, or a
   macOS AppIcon made from the same image), document type `io.github.kathoc.replayarcade.arcrec` (package,
   extension arcrec, as the iPhone Info.plist), link `-lreplaynes_all -lc++`, `SWIFT_ACTIVE_COMPILATION_CONDITIONS`
   adds `RN_PRODUCT_HAS_STARTUP`, **no Sparkle feed** (SUFeedURL absent; the updater must not offer
   ReplayNES's updates). Version: MARKETING_VERSION from products/ReplayArcade/cmake/product.cmake, build
   number = `git rev-list --count HEAD` (as build-ios.sh).
4. **App code (apps/macos/Sources)**: where the Mac app hard-codes the NES, use the product profile the
   iPhone app already uses (`RNProduct` in apps/macos/Sources/Core/RNProduct.swift: name, isNES,
   romExtensions, isROMFile, projectExtension, isProject, dataDirectory, startup). Known places:
   AppModel.swift `selectedCore` ("nestopia" / "replaynescore"; "Unknown core" otherwise), open panels with
   `.nesROM` / `.nesrec`, session creation with RN_CORE_NESTOPIA / RN_CORE_REPLAYNESCORE, the Quick Menu's
   core choice (QuickMenuPages.swift ~510), window titles / menu names "ReplayNES", the data directory.
   Follow how apps/ios/Sources/App/AppModel.swift does each (it runs ReplayArcade on the iPhone today).
   For ReplayNES (RNProduct.isNES) every behaviour stays as it is now.
5. **Script**: `products/ReplayArcade/scripts/build-macos.sh [--run]`: generate the project, build Release,
   put `ReplayArcade.app` in `products/ReplayArcade/build-macos/`; `--run` opens it. Ad-hoc signing is
   enough (no team, no notarization).

## Done when

- `products/ReplayArcade/scripts/build-macos.sh` succeeds on macsrv from a clean `build-macos/`.
- The app starts, its window / About say ReplayArcade and its version (0.3.19 now), its Info.plist has no
  SUFeedURL and has the arcrec document type.
- A FinalBurn Neo ROM set opens and plays (picture, sound, input), and a project saves and reopens.
  ROMs are not in the repository: the user's ROM folder is used for the manual check; for an automated
  check use the app's test hooks if one can load a file, else report that the run check needs a ROM.
- ReplayNES's Mac app: builds, and its unit tests pass as before (2 known failures exist only in the
  iPhone's QuickMenuModelTests; report any macOS test failure with its name).
- The engine tests (`cmake --build build && ctest`) still pass if the frontend / engine was touched.

## Not in this step

Sparkle feed and signing for ReplayArcade, notarization, GitHub release assets, the Mac App Store, any
change to the iPhone, Windows, Linux or Android apps.

## Implementation decisions

- Reuse the macOS app sources and shared XcodeGen settings; keep ReplayNES product settings in its own project specification. This minimizes divergence and preserves its existing behavior.
- Build the Arcade engine as a Release arm64 static archive and use the existing product profile for app differences. Separate app copies or runtime name detection were considered but rejected because they duplicate behavior or obscure product selection.
- Verify generated project settings and bundle metadata, build the Arcade app and ReplayNES tests, then exercise the app and a ROM where the environment permits. Record any environment or ROM limitations in the review.

## Result (2026-11-19)

Implemented by Codex (reports docs/reviews/replayarcade-macos-1.md, -2.md: it could not build in its sandbox),
built, fixed and checked by Claude on macsrv:

- Fixed after the review: packaging/macos's header path was `generated/replaynes`; frontend.h includes
  `"replaynes/product.h"`, so `__has_include` failed and the Swift side was compiled with the defaults
  (the app called itself ReplayNES and "the selected core is not in this build"). Now `generated`.
- Fixed: five new UI strings had no Japanese and the window title's "ReplayNES" key went unused
  (scripts/check-l10n.py, run by scripts/test-macos.sh): translations added, the key removed.
- `products/ReplayArcade/scripts/build-macos.sh`: ReplayArcade.app 0.3.19 (build 508), bundle id
  io.github.kathoc.replayarcade.mac, no SUFeedURL, document type io.github.kathoc.replayarcade.arcrec.
- Run (`--rom gradius.zip --autoplay --snapshot-at`): the boot grid at 24 s, the demo at 60 s (pictures
  differ: it runs); the session folder has `current.arcrec` (journal, states, timeline). Library screen:
  "ReplayArcade", no Test Cartridge button, Japanese text.
- ReplayNES's Mac app: scripts/test-macos.sh 128 tests, 0 failures (2 skipped), l10n 0 problems; its
  library screen unchanged (ReplayNES, .nes text, Test Cartridge button).

Not verified: sound and controller input on the Mac (no one at macsrv's speakers / pad), Save As of a
project and reopening it through the Open panel (the autosaved session was written; reopening by hand
is left to the user).
