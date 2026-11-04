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

## Devices: orientation and size (2026-11-02, Retroid Pocket 5)

Android devices differ in their panel's orientation (a portrait panel used in landscape), how they are
turned, the window's size against the display (bars, cutouts) and in how a driver reports a turned
surface (the Retroid Pocket 5's Adreno 650: extent 1920 x 1080 with a 90 degree transform; others the
panel's 1080 x 1920). The renderer took the surface's transform as the swapchain's preTransform: the
compositor took the frames as pre-rotated and the UI showed sideways. render/surface_plan.h decides
for every device: an identity swapchain the size of the window (the compositor rotates), the reported
extent only orienting the limits; unit tested with both reporting styles, 90 / 180 / 270, a smaller
window, a surface without identity. The renderer logs what it found and chose whenever it changes
("video: surface ... turned ..., window ... -> swapchain ..."); stdout / stderr go to logcat (tag = the
product). Checked on a Retroid Pocket 5 (Android 13): landscape and upright; a ROM pushed into
Android/data/<id>/files/ReplayArcade/ROM shows in the library.

## Full screen (2026-11-02, Retroid Pocket 5)

The status and navigation bars stayed over the top and bottom of the picture (the window 1920 x 1080,
the bars 54 px each). SDL asks for full screen with the deprecated system UI flags
(setSystemUiVisibility FULLSCREEN | HIDE_NAVIGATION | IMMERSIVE_STICKY); on Android 13 the window kept
only the layout flags and its requested visibilities said both bars visible (dumpsys window), from the
launch on. MainActivity hides them with the window's insets controller (Android 11+; systemBars, shown
for a moment by a swipe from an edge), at creation and whenever the window gets the focus back.

## Fonts (2026-11-03)

On the Android 16 emulator the Japanese UI showed "?": the system's Noto Sans CJK is a variable font (OpenType with
CFF2, fvar), which Dear ImGui's stb_truetype cannot read; the Retroid Pocket 5 (Android 13) carries the older
static one and was fine. UI::loadFonts also took "found" for "loaded", so Japanese was kept with no glyphs.
Adopted (user: bundle a font with no licence problem, the app may later be sold with another core): Noto Sans JP
Regular 2.004, static OTF (CFF), SIL OFL 1.1, in the APK's assets/fonts with OFL.txt (third_party/fonts/noto-sans-jp);
fonts_android.cpp prefers it (SDL_LoadFile, AddFontFromMemoryTTF), the system fonts remain the fallback, and
loadFonts keeps Japanese only when the Japanese font was actually read. Not adopted: FreeType (reads CFF2, but a
second font rasteriser to build on every platform for one font). Checked on the Android 16 emulator: the settings
and the library in Japanese, the log names the bundled font.

## Opening a folder (2026-11-04)

"Open Folder" (the empty library) and the folders under Settings > App > Folders & License did nothing.
openFolder handed SDL_OpenURL a file:// URL; an app that targets Android 7+ may not pass one to another
app (StrictMode throws FileUriExposedException in startActivity), SDL's openURL catches every exception
and returns false, and the UI ignored the result, so the press left no trace (emulator, Android 16: the
app stayed in front, nothing in logcat). Even a working URL would not have helped: since Android 11 other
apps cannot list Android/data, where the library lives.

Adopted: the app is a documents provider for its files folder (LibraryProvider: the library and the
Trash, read / write / create / rename / delete), so the system Files app and the document picker show
"ReplayArcade" (the product) as a root, and ROMs can be copied in from any file manager that uses them.
On Android openFolder calls MainActivity.openFolder through JNI: ACTION_VIEW of the folder as a document
(vnd.android.document/directory), the provider's root as the fallback. DocumentsUI opens a folder it was
handed only after the provider answers findDocumentPath (the path from the root); without it, it falls
back to Downloads. The provider's intent filter is android.content.action.DOCUMENTS_PROVIDER
(DocumentsContract.PROVIDER_INTERFACE); a wrong action string leaves the provider registered but never
listed by DocumentsUI (queryIntentContentProviders), with no error anywhere.

Not adopted: a storage permission and a library in shared storage (MANAGE_EXTERNAL_STORAGE: a broad
permission for one folder, and a file manager still has no standard "open this folder" request); a
folder picked with ACTION_OPEN_DOCUMENT_TREE (the library reads paths, so every file access would change).

Checked on the emulator (Android 16, DocumentsUIGoogle): "Open Folder" with an empty library opens
ReplayArcade > ROM in Files; Settings > Folders & License opens ROM and Projects; a zip copied into ROM
with Files' "Copy to" lands there byte for byte (md5). Not yet on the Retroid Pocket 5 (Android 13).
