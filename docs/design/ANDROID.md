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

## Choosing the folders (2026-11-04)

Asked for: the ROM folder and the Projects folder can be changed, and set back to the defaults. On Android
the user chose all-files access (asked, over a picker giving content URIs, which would change every file
access in the library, the projects and the thumbnails; a Google Play sale would have to pass its review of
that permission).

- Settings keeps romFolder / projectsFolder ("": the library's own, <library>/ROM and <library>/Projects).
  LibraryModel takes them (setFolders: waits for a scan, makes a missing chosen folder, rescans); every user
  of the folders already went through romDir() / projectsDir(). The in-app choosers stay inside the
  library, or inside a chosen folder outside it (chooserRoot). Share / Open with follow the ROM folder
  (setImportFolder).
- Settings > App > Folders & License: a folder's row shows its path and opens a dialog: Open Folder,
  Change…, Back to the Default (only for a chosen one), Cancel. A chosen folder must take a file (a probe
  file written and removed). The files in the folder used before stay where they are; the notice says so.
- Change… picks with: the app's own chooser in folder mode, from "/" (Linux, the Steam Deck: a gamepad
  drives it; cards and USB drives under /run/media); SDL's folder dialog (Windows: other drives); on
  Android all-files access first (Android 11+: the system's "All files access" page for the app, back in
  onResume; Android 10: the storage permissions, requestLegacyExternalStorage), then the system's folder
  picker (ACTION_OPEN_DOCUMENT_TREE), whose tree URI becomes a path (StorageFolders: "primary:<path>" on the
  device, "<volume>:<path>" on a card as /storage/<volume>/<path>; a cloud folder is refused). The answer
  comes back to the frame loop through takeChosenFolder.
- Open Folder on a chosen folder outside the app: the external storage provider's document in Files.

Checked: on the emulator, Change… -> All files access -> the picker -> /sdcard/ROMs: the library read its
ROM ("library: 1 ROMs in /storage/emulated/0/ROMs"), Open Folder showed it in Files, the choice survived a
restart, Back to the Default returned to Android/data/... with its notice. Tests: the settings' round trip,
a chosen folder and back with the chooser's root, a missing chosen folder made. Linux (the offscreen UI
tool): the folder dialog and the chooser with Use This Folder (folders-dialog.png, folders-chooser.png).
Windows: built (x86_64), SDL's folder dialog not tried on Windows.

## Add ROMs: zips not listed, a second tap that did nothing (2026-11-04)

Reported on 0.3.12: Add ROMs' picker showed .txt files, never the zips. Reproduced: the picker opens where it
was last, and its "Documents" category (MediaProvider's documents root) lists documents only; once there, a
zip in the same Download folder is not listed. Fixed: the picker starts in a folder of the external storage
provider (every file listed): the folder the last ROMs came from (remembered when one is picked there), else
Download (EXTRA_INITIAL_URI, RomImport.startFolder / rememberFolder).

Found by the emulator test (scripts/test-android-emu.sh in ReplayArcade): after the picker was cancelled,
Add ROMs did nothing however often it was tapped, while a tap elsewhere worked. The press and release
reached the app; ImGui's SDL backend takes the pointer's position from motion events only, the window had
been left (ImGui's mouse "nowhere"), and a tap where the last one was makes no motion: the row under it
never saw the press. Fixed in app.cpp: a button event gives ImGui its position first. Every screen of the
app had it (any tap at the same spot after another app), on every platform where a window can be left.

The test (14 checks, emulator, Android 16): Add ROMs from the empty library and from Settings (5 cancels in
a row), the picker's start folder (Download, then the remembered one), the copy and the library listing it,
Open Folder in Files, home and back (surface released, no frame on a lost surface), Share from Files with the
app stopped and running.

## Finding ROMs anywhere on the device (2026-11-05)

Reported on 0.3.13: zips left in Documents and other folders were not listed at all. The device, its
Android version and its screens cannot be seen, and no log can be sent: the route must not depend on what a
picker of some maker and version chooses to list. Adopted: Add ROMs asks first, "Search the Device" /
"Choose Files" / Cancel. Search the Device asks for all-files access (Android 11+: the system's page;
Android 10: storage permission), walks every storage root (the device's storage and each card, Android/data
and Android/obb left out; hidden folders, any case, any name, any depth; links not followed; an unreadable
folder skipped and counted), keeps a file only when it is a ROM the product runs (rnf_library_is_rom_file:
extension, then the game database; a junk .zip is "unknown", not offered), one per name (the larger), and
shows what it found (up to 8 names, "Add All"). The copy goes through name.part and a rename
(apps/desktop/src/rom_finder.*, the walk off the UI thread). Choose Files stays as the second route; denied
access or a device without the settings page answers the dialog instead of leaving it waiting.

Found on the way, by the emulator matrix:
- FinalBurn Neo appended every load's missing-file list to one 2048-byte buffer and never cleared it: a few
  wrong sets in the library overran it, and the pointers after it, into an abort in realloc. Fixed in the
  core (patches/fbneo in ReplayArcade): cleared before each check, every write bounded.
- Android 13's emulator Vulkan: the present-id/present-wait features and the driver properties were asked
  for without their extension (a validation abort). Fixed: asked only when the device has them.

The test (scripts/test-android-emu.sh, scripts/test-android-matrix.sh in ReplayArcade): Android 10, 11, 13
and 16 (emulators, wiped), each bounded in time and naming its device. H: seven 1942 copies under other set
names in Download/deep/a/b/c/d/e, .hidden, Documents, a Japanese-named folder, upper case, the root, DCIM, and
a junk zip: all seven found and copied, the junk one not offered, the app alive with the wrong sets loaded
and after a cold start. Android 11's picker ignores `adb shell input tap` on its files (an event with no
input device); a finger works: the script taps through the emulator's touchscreen (sendevent, root).

Android 10's picker (and pickers built on that documents UI) hides the device's storage until "Show internal
storage" is chosen in its menu: its places are Recent, Downloads and apps, so Documents or any other folder
cannot be reached by hand. The script's D check had passed by picking Download's copy; made strict (the log
must say the ROMs folder was remembered), it failed there. Fixed: every picker the app opens (files, the
GET_CONTENT fallback, folders) asks for the device's storage (SHOW_ADVANCED, the documents UI's extra).

## Sharing, the picker's views, Open Folder, swiping the library (2026-11-06)

Reported: the share sheet sometimes had no ReplayArcade; from Open Folder, another folder showed no zips; with
many ROMs the library could not be scrolled by swiping.

- **Share.** `cmd package query-activities` on the Android 16 emulator: SEND / SEND_MULTIPLE with
  application/x-compressed, application/x-zip-archive or multipart/x-zip (names other apps give a zip) left the
  app out; the filters listed six types. Now any type (`*/*`); RomImport keeps only names the library lists
  (`rnf_library_is_rom_file` through `RomImport.nativeIsRom`: the product's extensions and its filter, which goes
  by the name) and says which files were not added. Being in the list is not being seen: a share sheet puts a
  new app far down or cuts the list short, and some apps show a short list of their own. So the app also
  publishes a Direct Share target (res/xml/shortcuts.xml, a long-lived dynamic shortcut "Add to the ROM
  folder"), the share sheet's top row on Android 10 and later; and the routes that need no share (Search the
  Device, Choose Files, the dialog below) stay the sure ones.
- **The Files app's and the picker's Documents, Images, Audio, Videos** (chips and drawer) are views of some
  kinds of files collected from the whole device, not folders: no zip ever appears there (reproduced: the real
  Documents folder lists 1943.zip, the Documents view only readme.txt). A person going from Open Folder to
  "Documents" saw no ROMs at all. The picker now asks for ROM types (EXTRA_MIME_TYPES, RomImport.ROM_TYPES):
  those views are gone from it, the device's folders, Downloads and Recent stay, and every ROM extension can
  be picked (.zip in any case, .7z, .nes, .fds, .unf). The Files app cannot be told: back from Open Folder
  with the ROM folder still empty, the app offers Search the Device / Choose Files (UI::openRomFolder,
  pollRomFolderVisit). It goes by the window's focus events (App -> UI::focusChanged): on Android no frame
  runs while another app is in front, so a frame loop never sees the focus gone.
- **The library** turns its pages (it does not scroll); a phone had no way to the next page but tapping the
  dimmed edge of the neighbouring one. A swipe turns the page now (right to left or bottom to top = next);
  the press it began is let go, so the card under the finger does not open.

The emulator test has checks for each (A3, E4, G direct, I1, I2, K1).

## Screen orientation (2026-11-06)

Settings > App > Screen Orientation: Automatic (default: the device's, with its rotation lock, as ReplayNES's
Android app does), Landscape or Portrait (either way up). The manifest asked for sensorLandscape, but SDL asks
again from the window's size when it makes the window (SDLActivity.setOrientationBis), so the app came up as
the window was. MainActivity applies the saved choice in onCreate (before the first frame), takes
setScreenOrientation from the settings (platform/shell.cpp, App::applySettings) and answers SDL's request with
the choice. Checked on the Android 16 emulator: Landscape 2424 x 1080, Portrait 1080 x 2424 after a restart
too, Automatic turning with the device (user_rotation 1 / 0). Emulator test O1-O3.
