# Android

User request 2026-11-01: "make an Android APK buildable; start the Android version".

## Goal

ReplayNES and ReplayArcade (a product profile, as on Windows and Linux) as Android apps, from the same
sources as the other platforms: the engine, the frontend core and the desktop frontend (SDL3 + Vulkan
+ Dear ImGui), with the same menus, library, recording, rewind and practice.

## Adopted

- apps/android: a Gradle project (AGP 9.5, Gradle 9.8, Java 21) whose native part is CMake
  (apps/android/CMakeLists.txt), like apps/linux with the Android pieces:
  - SDL3 from third_party/SDL as a shared library, its Java glue (org.libsdl.app, the same SDL version
    by construction: the source set points into third_party/SDL/android-project); the activity is a
    thin SDLActivity subclass that loads libSDL3.so and libmain.so.
  - The Vulkan renderer of apps/linux (vk_renderer, vk_backdrop, the CRT module) on the NDK's Vulkan
    (libvulkan.so, API 29+: Vulkan 1.1 on every 64-bit Android device that has Vulkan); shaders by the
    NDK's glslc.
  - main_android.cpp (the platform hooks), platform_android.cpp (folders: the library in the app's
    external files folder, visible over USB and to file managers; the session and settings in its
    internal files), fonts_android.cpp (/system/fonts: Roboto / Noto Sans CJK / symbols).
  - arm64-v8a only.
- The product: Gradle properties `rnProductFile` (a product.cmake, e.g. ReplayArcade's), `rnApplicationId`
  and `rnAppName`; without them ReplayNES. ReplayArcade's scripts/build-android.sh passes its profile
  (FBNeo and its startup registration come with it, as on Windows).
- First milestone (M1): the APK builds; it starts in the emulator and on a device; the library shows;
  a ROM put in the library folder plays with a controller. No MP4 export (RNL_HAVE_MP4_EXPORT off), no
  in-app updates, no Steam.

## Later

- M2: touch controls (the iOS app's layout and rules as the model), the system back button, the
  on-screen keyboard (SDL's), Android TV / handhelds (AYN Thor, Retroid) as controller-first devices.
- M3: MP4 export through MediaCodec; sharing; the CRT on capable GPUs (measured first).
- Signing for release (a keystore the user makes; never in the repository).

## Not adopted

- A separate Android UI (Compose / Views): two frontends to keep in step; the desktop frontend is
  already controller- and touch-aware through SDL.
- OpenGL ES: the renderer and the CRT are Vulkan; Android has Vulkan on every device that can run the
  emulation at speed.
- 32-bit ABIs: no current device needs them for this, and FBNeo is heavy.

## Done when (M1)

- `gradle assembleDebug` (apps/android) gives an APK; ReplayArcade's script gives its APK.
- Installed in the emulator: it starts, the library page shows (a screenshot).
