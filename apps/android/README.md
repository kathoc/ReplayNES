# ReplayNES for Android (minimal)

A minimal Android frontend on the portable ReplayNES engine (`engine/`, C API
`engine/include/replaynes/replaynes.h`) and the Nestopia UE core. Kotlin, one Activity, plain Views.

- ROM library: **Open ROM** (Storage Access Framework, iNES header checked) copies the file into
  the app's `files/roms/`; each ROM gets a ReplayNES project in `files/projects/<name>.nesrec/`
  (reopened next time, saved when the game screen is left or the app goes to the background,
  journal autosave every 10 s). **Test Cartridge** writes the CC0 ReplayNES Test Cartridge.
- Game screen: SurfaceView (8:7 pixel aspect, letterboxed, nearest-neighbour integer pre-scale),
  touch pad (8-way D-pad, A, B, SELECT, START, hold-to-rewind), hardware gamepads and keyboards
  (arrows, X = A, Z = B, Enter = Start, Space = Select, L1 / Backspace = rewind).
- Native side (`app/src/main/cpp/replaynes_jni.cpp`): one emulation thread owns the session, paces
  `rn_step` to 39375000/655171 Hz, blits to the ANativeWindow and feeds AAudio (48 kHz mono s16)
  through a ring buffer. The engine is always compiled optimized (also in debug builds).
- Not yet: the shared frontend core (`frontend/`, needs Python at build time), timeline / takes /
  bookmarks / practice UI, settings, display-driven pacing with audio DRC (docs/FRAME_PACING.md),
  flash reduction.

## Build

Requirements: Android SDK (platform 35, build-tools 35), NDK 26.1.10909125, SDK CMake 3.22.1,
JDK 17 or 21, and the repo's submodules (`git submodule update --init`).

```sh
cd apps/android && JAVA_HOME=$(/usr/libexec/java_home -v 21) ./gradlew assembleDebug
```

The APK is `app/build/outputs/apk/debug/app-debug.apk` (application id `io.github.replaynes.dev`;
release builds use `io.github.replaynes`). Set `sdk.dir` in `local.properties` or `ANDROID_HOME`
if the SDK is not found. Versions: Gradle 8.9 (wrapper), AGP 8.7.3, Kotlin 2.0.21; ABI arm64-v8a.

## Release signing

`./gradlew assembleRelease` signs with the dedicated ReplayNES Android release key (created
2026-11-05; RSA 4096, alias `replaynes`, PKCS12). Neither the keystore nor its password is in the
repository: Gradle reads a properties file outside it,
`~/.config/replaynes/android-signing.properties` (override with `REPLAYNES_ANDROID_SIGNING=<file>`):

```properties
storeFile=/Users/<you>/.config/replaynes/android-release.jks
storePassword=...
keyAlias=replaynes
keyPassword=...
```

Both files live on the release Mac only (directory 700, files 600). Without the properties file
`assembleRelease` still builds, but the APK is unsigned and cannot be installed.

**Back up the keystore and the properties file offline** (encrypted USB / password manager).
Android only accepts an update signed with the same key as the installed app: losing the key means
every existing sideloaded install has to be uninstalled (losing its app data) before a new version
can be installed. Check the certificate with
`apksigner verify --print-certs app/build/outputs/apk/release/app-release.apk`.

## Run on an emulator / device

```sh
adb install -r app/build/outputs/apk/debug/app-debug.apk
# Debug builds accept a ROM for testing (copied into the library, then started):
adb shell am start -n io.github.replaynes.dev/io.github.replaynes.MainActivity --es rom testcart
adb shell am start -n io.github.replaynes.dev/io.github.replaynes.MainActivity --es rom /sdcard/Download/game.nes
adb logcat -s ReplayNES
```

## License

GPL-2.0-or-later, like the rest of ReplayNES (see `LICENSE`, `THIRD_PARTY_NOTICES.md`); the app's
About screen carries the notice, the Nestopia UE credit and the source URL.
