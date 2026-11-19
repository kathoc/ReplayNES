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
