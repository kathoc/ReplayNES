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

## Adding ROMs (2026-11-04)

Reported on 0.3.11: the folder opened in Files, but ROMs put there did not show up; a file manager could
not open the ROM folder at all. Asked for: ROMs must go in from any Android, by more than one route.

Found:

- **ROMs found but never shown (the cause).** The scan found the new file ("library: 1 ROMs in ...", a log
  line added for this) and the library stayed empty. The frame loop hands the scan's result to the library
  (library_->poll) only in the slack before the next sample (> 4 ms). On the emulator the loop had no
  slack in any frame (22 fps, measured over 2 s), so the result waited forever; the thumbnails, the resume
  record and the deferred checkpoints waited too. A touch of the file or a restart sometimes "fixed" it
  only because a frame happened to have slack. Fixed in app.cpp: the slack's tasks also run when they
  have not run for 0.25 s (each keeps its own limits: an autosave still needs 8 ms of slack unless paused).
- **A file written after it was created.** The library watched folder times only. A file manager or the
  documents provider creates the file and then writes it, which leaves the folder's time alone: a scan in
  between kept the empty file's size (and, for ReplayNES, the empty file's SHA-256). The watch now also
  compares every ROM file's time and size.
- **Files reopened at its old folder.** With a Files task in the background, "Open Folder" brought that
  task back as it was (a deleted folder) instead of reading the intent: FLAG_ACTIVITY_CLEAR_TASK.
- **Other file managers.** Since Android 11 they cannot list Android/data; only those that use the
  documents framework see the app's provider. So the app takes files in by itself:

Adopted routes (any one is enough):

1. **Add ROMs** (empty library, first button; Settings > App > Folders & License): the system file picker
   (ACTION_OPEN_DOCUMENT, several files; ACTION_GET_CONTENT where there is no documents UI); RomImport
   copies the files into the ROM folder.
2. **Share / Open with** from any app (zip, 7z, octet-stream, NES): MainActivity's intent filters, the same
   copy. A share that starts the app waits for the ROM folder (setRomDir from main_android.cpp).
3. **Files** (the documents provider, above).
4. **USB (MTP) from a computer**: Android/data/<id>/files/<product>/ROM, as before.

RomImport writes ".<name>.part" (hidden: the scan skips it) and renames it when complete, replacing a
file of the same name (a newer set), and says in a toast how many files went in or which failed.

Not adopted: a storage permission (MANAGE_EXTERNAL_STORAGE) to put the library in shared storage (a broad
permission, refused on Google Play, and still no standard way for a file manager to open a folder); a
ROM folder chosen with ACTION_OPEN_DOCUMENT_TREE (every file access through content URIs).

Checked on the emulator (Android 16): an empty library, Add ROMs, 1942.zip picked in Downloads: the toast,
"import: 1942.zip", "library: 1 ROMs" and the game in the library; Share from Files to ReplayArcade, with
the app running and with it stopped; Settings' ROM Folder with a Files task left at Downloads: ROM opens.
Before the frame-loop fix the same Add ROMs left the library empty although the scan had found the file.

## Black screen after another app (2026-11-04)

Reported from a friend's device (not at hand): back in the app after another one, the screen stayed black
and nothing reacted. On the emulator the picture always came back, so its log was read with the new log
lines (below): "surface: destroyed" at 20:38:37.546, then "vkAcquireNextImageKHR failed:
VK_ERROR_SURFACE_LOST_KHR" at 37.614. The frame loop drew to the window's surface after Android had taken
it down. SDL learns of the pause only in surfaceDestroyed (Java's UI thread) and releases the window at
once; with OpenGL it waits for the loop to let its EGL surface go, with Vulkan it does not wait at all, and
the loop goes on until its next event pump. Android's rule: after surfaceDestroyed returns, the surface is
not touched. The emulator's software Vulkan answers SURFACE_LOST at once (the loop recreated its surface and
went on); a device's driver can block on the abandoned window for good: the black, frozen screen.

Fixed (vk_renderer.cpp, MainActivity's SDLSurface): surfaceDestroyed calls VkRenderer::surfaceGoing before
SDL lets the window go. It closes a gate and waits for a frame in progress (at most 2 s); the next frame
releases the swapchain and the Vulkan surface (vkDeviceWaitIdle first) and draws nothing while the gate is
closed. surfaceCreated (after SDL took the new window) opens it again; if no frame ran in between (SDL had
paused the loop at once), the first frame releases the old surface before it draws, then recreateSurface
makes the new one. The renderer's thread does all the Vulkan work: the gate is the only thing shared.

Checked on the emulator: four trips to the home screen and back (library and a game running): each time
"surface: destroyed", "Vulkan surface released", "surface: created", "Vulkan surface recreated", and no
vkAcquireNextImageKHR failure; before the fix two of three trips touched the old surface. Not checked on the
reporter's device yet: the log (Share Log) from it will say.

## Logs (2026-11-04)

A device that is not at hand reports by its log. main_android.cpp writes what goes to logcat also to
<external files>/Logs/<product>.log, each line with the local time, after a header line (product, version,
manufacturer, model, Android version, SoC); past 1 MB the file becomes <product>.1.log. AppLog.java adds the
activity's life (pause, stop, start, resume, focus, trim memory) and the surface's (created, changed,
destroyed) from the Java side, which the paused native loop cannot log. The library logs each scan ("library:
N ROMs in ...", "skipped <file>: <reason>"). Settings > App > Folders & License > Share Log sends both files
through the share sheet (documents of LibraryProvider with a read grant): mail, a chat app.
