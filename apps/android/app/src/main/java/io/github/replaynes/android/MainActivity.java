// The game activity: SDL's activity with its two libraries (libSDL3.so, libmain.so = the app).
// SPDX-License-Identifier: GPL-2.0-or-later
package io.github.replaynes.android;

import android.Manifest;
import android.content.ActivityNotFoundException;
import android.content.Context;
import android.content.Intent;
import android.content.pm.ActivityInfo;
import android.content.pm.PackageManager;
import android.content.pm.ShortcutInfo;
import android.content.pm.ShortcutManager;
import android.graphics.drawable.Icon;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.os.Environment;
import android.provider.DocumentsContract;
import android.provider.Settings;
import android.util.Log;
import android.view.HapticFeedbackConstants;
import android.view.SurfaceHolder;
import android.view.WindowInsets;
import android.view.WindowInsetsController;

import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.Collections;
import java.util.List;

import org.libsdl.app.SDLActivity;
import org.libsdl.app.SDLSurface;

public class MainActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] {"SDL3", "main"};
    }

    // Add ROMs' picker (RomImport); clear of SDL's own request codes (its file dialog).
    private static final int REQUEST_IMPORT = 0x524f4d;
    // The library's ROM folder (setRomDir from the app's start-up); a Share or Open with that comes first waits.
    private volatile String romDir;
    private final List<Uri> pendingImport = new ArrayList<>();
    private String importDir;

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        hideSystemBars();
        chosenOrientation = orientationFor(getSharedPreferences(PREFS, MODE_PRIVATE).getString(ORIENTATION, "auto"));
        setRequestedOrientation(chosenOrientation);  // the last choice, before the first frame
        publishShareShortcut();
        receive(getIntent());
    }

    // Settings > App > Screen Orientation (docs/design/ANDROID.md "Screen orientation"). Automatic follows the
    // device and its rotation lock, as ReplayNES does; Landscape and Portrait keep that one (either way up).
    private static final String PREFS = "replaynes.screen";
    private static final String ORIENTATION = "orientation";
    private volatile int chosenOrientation = ActivityInfo.SCREEN_ORIENTATION_USER;

    private static int orientationFor(String which) {
        if ("landscape".equals(which)) return ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE;
        if ("portrait".equals(which)) return ActivityInfo.SCREEN_ORIENTATION_SENSOR_PORTRAIT;
        return ActivityInfo.SCREEN_ORIENTATION_USER;
    }

    /** setScreenOrientation in platform/shell.cpp through JNI (any thread): at start-up and when it changes. */
    public boolean setScreenOrientation(String which) {
        final int o = orientationFor(which);
        getSharedPreferences(PREFS, MODE_PRIVATE).edit().putString(ORIENTATION, which).apply();
        if (o == chosenOrientation && getRequestedOrientation() == o) return true;
        chosenOrientation = o;
        AppLog.line(this, "screen: orientation " + which);
        runOnUiThread(() -> setRequestedOrientation(o));
        return true;
    }

    /**
     * SDL asks for an orientation from the window's size when it makes the window: the person's choice wins
     * (it had taken a portrait phone's window for a portrait-only app, and the manifest's landscape was lost).
     */
    @Override
    public void setOrientationBis(int w, int h, boolean resizable, String hint) {
        setRequestedOrientation(chosenOrientation);
    }

    /**
     * Direct Share: "Add to the ROM folder" in the share sheet's top row for any file (res/xml/shortcuts.xml).
     * The app list below it may be cut short, or sort a new app far down: the shortcut is the entry a person
     * sees first. Long-lived, so the system keeps offering it.
     */
    private void publishShareShortcut() {
        try {
            ShortcutManager sm = getSystemService(ShortcutManager.class);
            if (sm == null) return;
            ShortcutInfo s = new ShortcutInfo.Builder(this, "rom-folder")
                .setShortLabel(getString(R.string.rn_share_target))
                .setLongLabel(getString(R.string.rn_share_target))
                .setIcon(Icon.createWithResource(this, R.mipmap.ic_launcher))
                .setCategories(Collections.singleton("io.github.replaynes.android.ROM_SHARE_TARGET"))
                .setLongLived(true)
                .setIntent(new Intent(Intent.ACTION_MAIN, null, this, MainActivity.class))
                .build();
            sm.addDynamicShortcuts(Collections.singletonList(s));
            AppLog.line(this, "share: direct share target published");
        } catch (RuntimeException e) {
            AppLog.line(this, "share: no direct share target: " + e);
        }
    }

    // The surface's life in the log too: Vulkan draws to it, and it goes when another app comes in front.
    @Override
    protected SDLSurface createSDLSurface(Context context) {
        return new SDLSurface(context) {
            @Override
            public void surfaceCreated(SurfaceHolder holder) {
                AppLog.line(context, "surface: created");
                super.surfaceCreated(holder);  // SDL takes the new window
                nativeSurfaceBack();
            }

            @Override
            public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
                AppLog.line(context, "surface: changed " + width + "x" + height);
                super.surfaceChanged(holder, format, width, height);
            }

            @Override
            public void surfaceDestroyed(SurfaceHolder holder) {
                AppLog.line(context, "surface: destroyed");
                // Before SDL lets the window go: no frame draws to it afterwards (Vulkan: SDL waits only for GL).
                nativeSurfaceGoing();
                super.surfaceDestroyed(holder);
            }
        };
    }

    private static native void nativeSurfaceGoing();
    private static native void nativeSurfaceBack();

    // The activity's life (another app in front, back again) in the log a user can send (docs/design/ANDROID.md,
    // "Logs"): the native side is paused then and cannot say it.
    @Override
    protected void onPause() {
        AppLog.line(this, "activity: pause");
        super.onPause();
    }

    @Override
    protected void onStop() {
        AppLog.line(this, "activity: stop");
        super.onStop();
    }

    @Override
    protected void onStart() {
        super.onStart();
        AppLog.line(this, "activity: start");
    }

    @Override
    protected void onResume() {
        super.onResume();
        AppLog.line(this, "activity: resume");
        if (awaitingAllFiles) {  // back from the "all files" page
            awaitingAllFiles = false;
            accessAnswered();
        }
    }

    @Override
    public void onTrimMemory(int level) {
        super.onTrimMemory(level);
        AppLog.line(this, "activity: trim memory " + level);
    }

    @Override
    protected void onNewIntent(Intent intent) {
        super.onNewIntent(intent);
        setIntent(intent);
        receive(intent);
    }

    // Share (SEND, SEND_MULTIPLE) or Open with (VIEW) from another app: the files go into the ROM folder.
    private void receive(Intent intent) {
        if (intent == null) return;
        String a = intent.getAction();
        if (!Intent.ACTION_SEND.equals(a) && !Intent.ACTION_SEND_MULTIPLE.equals(a) && !Intent.ACTION_VIEW.equals(a)) return;
        List<Uri> uris = RomImport.uris(intent);
        String dir = romDir;
        if (dir == null) {
            synchronized (pendingImport) { pendingImport.addAll(uris); }
            return;
        }
        RomImport.copyInto(this, uris, new File(dir));
    }

    /**
     * The library's ROM folder (setImportFolder in platform/shell.cpp through JNI: at start-up and when it is
     * changed in Settings): files shared before it was known go in now.
     */
    public boolean setRomDir(String dir) {
        romDir = dir;
        List<Uri> uris;
        synchronized (pendingImport) {
            uris = new ArrayList<>(pendingImport);
            pendingImport.clear();
        }
        if (!uris.isEmpty()) runOnUiThread(() -> RomImport.copyInto(this, uris, new File(dir)));
        return true;
    }

    /** Add ROMs (importFiles in platform/shell.cpp through JNI, any thread): the system file picker, several files at once. */
    public boolean importFiles(String dir) {
        AppLog.line(this, "import: open the picker for " + dir);
        runOnUiThread(() -> {
            importDir = dir;
            Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            i.addCategory(Intent.CATEGORY_OPENABLE);
            i.setType("*/*");
            i.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
            i.putExtra(Intent.EXTRA_MIME_TYPES, RomImport.ROM_TYPES);
            showDeviceStorage(i);
            // Where the picker starts: the folder of the last ROMs added, else the device's Download folder.
            // Without it the picker reopens its last place, and its "Documents" category lists documents only
            // (a .txt, never a .zip): the ROMs looked absent (2026-11-04).
            Uri startAt = RomImport.startFolder(this);
            i.putExtra(DocumentsContract.EXTRA_INITIAL_URI, startAt);
            AppLog.line(this, "import: the picker starts at " + startAt);
            try {
                startActivityForResult(i, REQUEST_IMPORT);
                AppLog.line(this, "import: picker started");
            } catch (ActivityNotFoundException e) {
                // ACTION_GET_CONTENT: a device without the documents UI (some TV boxes) still has a file chooser.
                Intent g = new Intent(Intent.ACTION_GET_CONTENT);
                g.addCategory(Intent.CATEGORY_OPENABLE);
                g.setType("*/*");
                g.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
                g.putExtra(Intent.EXTRA_MIME_TYPES, RomImport.ROM_TYPES);
                showDeviceStorage(g);
                try {
                    startActivityForResult(Intent.createChooser(g, null), REQUEST_IMPORT);
                } catch (ActivityNotFoundException e2) {
                    Log.w("ReplayNES", "importFiles: no file picker: " + e2);
                }
            }
        });
        return true;
    }

    /**
     * Share Log (shareLog in platform/shell.cpp through JNI, any thread): the log file and the one before it
     * to another app (mail, a chat), as documents of LibraryProvider with a read grant.
     */
    public boolean shareLog(String path) {
        ArrayList<Uri> uris = new ArrayList<>();
        File current = new File(path);
        File older = new File(current.getParentFile(), current.getName().replace(".log", ".1.log"));
        for (File f : new File[] {older, current}) {
            if (!f.isFile()) continue;
            try {
                String id = LibraryProvider.documentId(this, f);
                if (id != null) uris.add(DocumentsContract.buildDocumentUri(LibraryProvider.authority(this), id));
            } catch (IOException e) {
                Log.w("ReplayNES", "shareLog: " + f + ": " + e);
            }
        }
        if (uris.isEmpty()) return false;
        runOnUiThread(() -> {
            Intent send = new Intent(uris.size() == 1 ? Intent.ACTION_SEND : Intent.ACTION_SEND_MULTIPLE);
            send.setType("text/plain");
            if (uris.size() == 1) send.putExtra(Intent.EXTRA_STREAM, uris.get(0));
            else send.putParcelableArrayListExtra(Intent.EXTRA_STREAM, uris);
            send.putExtra(Intent.EXTRA_SUBJECT, getString(R.string.app_name) + " log");
            android.content.ClipData clip = android.content.ClipData.newRawUri(null, uris.get(0));
            for (int i = 1; i < uris.size(); ++i) clip.addItem(new android.content.ClipData.Item(uris.get(i)));
            send.setClipData(clip);  // the grant reaches every file
            send.addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION);
            try {
                startActivity(Intent.createChooser(send, null));
            } catch (ActivityNotFoundException e) {
                Log.w("ReplayNES", "shareLog: " + e);
            }
        });
        return true;
    }

    // Change… for the ROM / Projects folder (docs/design/ANDROID.md, "Choosing the folders"): all-files access
    // first (the library reads paths), then the system's folder picker; the answer to nativeFolderChosen as a
    // path, or "" with a reason the native side words ("no-access", "not-local", "no-picker"; both "": cancelled).
    private static final int REQUEST_FOLDER = 0x524f4e;
    private static final int REQUEST_STORAGE = 0x524f4f;
    private String folderStart;
    private boolean awaitingAllFiles;
    // What all-files access is asked for: the folder picker next, or Search the Device (its answer to native).
    private boolean accessForSearch;

    private static native void nativeFolderChosen(String path, String error);
    private static native void nativeStorageAccess(boolean granted);

    /** chooseFolder in platform/shell.cpp through JNI, any thread. */
    public boolean chooseFolder(String start) {
        runOnUiThread(() -> {
            folderStart = start;
            accessForSearch = false;
            if (hasStorageAccess()) pickFolder();
            else askStorageAccess();
        });
        return true;
    }

    /** Whether the app may read the whole storage now (storageAccess in platform/shell.cpp). */
    public boolean storageAccessGranted(String unused) {
        return hasStorageAccess();
    }

    /** Asks for all-files access for Search the Device; the answer to nativeStorageAccess (any thread). */
    public boolean requestStorageAccess(String unused) {
        runOnUiThread(() -> {
            accessForSearch = true;
            if (hasStorageAccess()) nativeStorageAccess(true);
            else askStorageAccess();
        });
        return true;
    }

    private void accessAnswered() {
        boolean granted = hasStorageAccess();
        AppLog.line(this, "storage: all-files access " + (granted ? "granted" : "refused"));
        if (accessForSearch) nativeStorageAccess(granted);
        else if (granted) pickFolder();
        else nativeFolderChosen("", "no-access");
    }

    private boolean hasStorageAccess() {
        if (Build.VERSION.SDK_INT >= 30) return Environment.isExternalStorageManager();
        return checkSelfPermission(Manifest.permission.WRITE_EXTERNAL_STORAGE) == PackageManager.PERMISSION_GRANTED;
    }

    private void askStorageAccess() {
        if (Build.VERSION.SDK_INT < 30) {  // Android 10: the legacy storage permission
            requestPermissions(new String[] {Manifest.permission.READ_EXTERNAL_STORAGE,
                                             Manifest.permission.WRITE_EXTERNAL_STORAGE}, REQUEST_STORAGE);
            return;
        }
        // The system's own page for this app's "all files" switch; back in onResume.
        Intent[] tries = {
            new Intent(Settings.ACTION_MANAGE_APP_ALL_FILES_ACCESS_PERMISSION, Uri.parse("package:" + getPackageName())),
            new Intent(Settings.ACTION_MANAGE_ALL_FILES_ACCESS_PERMISSION),
        };
        for (Intent i : tries) {
            try {
                awaitingAllFiles = true;
                startActivity(i);
                return;
            } catch (ActivityNotFoundException | SecurityException e) {
                awaitingAllFiles = false;
            }
        }
        // No page for it on this device (some TV boxes): the other routes remain (Choose Files, Share, Files).
        AppLog.line(this, "storage: no all-files access page");
        accessAnswered();
    }

    /**
     * The device's storage among the picker's places. Android 10 and earlier (and pickers built on that
     * documents UI) hide it until "Show internal storage" is chosen in the picker's menu: only Recent,
     * Downloads and apps are offered, so Documents or any other folder cannot be reached (found 2026-11-05 on
     * the Android 10 emulator). The documents UI's own extra (hidden API, both spellings it has used).
     */
    private static void showDeviceStorage(Intent i) {
        i.putExtra("android.provider.extra.SHOW_ADVANCED", true);
        i.putExtra("android.content.extra.SHOW_ADVANCED", true);
    }

    private void pickFolder() {
        Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE);
        showDeviceStorage(i);
        Uri start = folderStart == null ? null : StorageFolders.documentUri(folderStart);
        if (start != null) i.putExtra(DocumentsContract.EXTRA_INITIAL_URI, start);
        try {
            startActivityForResult(i, REQUEST_FOLDER);
        } catch (ActivityNotFoundException e) {
            nativeFolderChosen("", "no-picker");
        }
    }

    @Override
    public void onRequestPermissionsResult(int requestCode, String[] permissions, int[] grantResults) {
        if (requestCode != REQUEST_STORAGE) {
            super.onRequestPermissionsResult(requestCode, permissions, grantResults);
            return;
        }
        accessAnswered();
    }

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode == REQUEST_FOLDER) {
            if (resultCode != RESULT_OK || data == null || data.getData() == null) {
                nativeFolderChosen("", "");
                return;
            }
            String path = StorageFolders.treeToPath(data.getData());
            AppLog.line(this, "folder: chose " + data.getData() + " -> " + path);
            if (path == null) nativeFolderChosen("", "not-local");
            else nativeFolderChosen(path, "");
            return;
        }
        if (requestCode != REQUEST_IMPORT) {
            super.onActivityResult(requestCode, resultCode, data);
            return;
        }
        if (resultCode == RESULT_OK && importDir != null) {
            List<Uri> uris = RomImport.uris(data);
            if (!uris.isEmpty()) RomImport.rememberFolder(this, uris.get(0));
            RomImport.copyInto(this, uris, new File(importDir));
        }
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        AppLog.line(this, "activity: focus " + (hasFocus ? "gained" : "lost"));
        if (hasFocus) hideSystemBars();
    }

    /** A touch control's press (TouchControls through JNI, any thread): the view's haptic feedback. */
    public void haptic() {
        runOnUiThread(() -> getWindow().getDecorView().performHapticFeedback(HapticFeedbackConstants.VIRTUAL_KEY));
    }

    /**
     * A folder of the library in the system Files app (openFolder in platform/shell.cpp through JNI, any
     * thread): the folder as a document of LibraryProvider. False when it is outside the files folder or no
     * app opens it.
     */
    public boolean openFolder(String path) {
        String auth = LibraryProvider.authority(this);
        String docId;
        try {
            docId = LibraryProvider.documentId(this, new File(path));
        } catch (IOException e) {
            docId = null;
        }
        Intent[] tries;
        if (docId != null) {
            tries = new Intent[] {
                // The folder itself (DocumentsUI, Android 10+).
                new Intent(Intent.ACTION_VIEW).setDataAndType(
                    DocumentsContract.buildDocumentUri(auth, docId), DocumentsContract.Document.MIME_TYPE_DIR),
                // The provider's root (a Files app that does not open a folder).
                new Intent(Intent.ACTION_VIEW).setDataAndType(
                    DocumentsContract.buildRootUri(auth, LibraryProvider.ROOT_ID), "vnd.android.document/root"),
            };
        } else {
            // A folder chosen in Settings, outside the app's files: the external storage provider's document.
            Uri doc = StorageFolders.documentUri(path);
            if (doc == null) {
                Log.w("ReplayNES", "openFolder: no document for " + path);
                return false;
            }
            tries = new Intent[] {new Intent(Intent.ACTION_VIEW).setDataAndType(doc, DocumentsContract.Document.MIME_TYPE_DIR)};
        }
        for (Intent i : tries) {
            // CLEAR_TASK: a Files task left in the background would come back as it was, at its old folder,
            // instead of reading this intent (emulator, Android 16).
            i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_ACTIVITY_CLEAR_TASK
                       | Intent.FLAG_GRANT_READ_URI_PERMISSION | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
            try {
                startActivity(i);
                return true;
            } catch (ActivityNotFoundException | SecurityException e) {
                Log.w("ReplayNES", "openFolder: " + i + ": " + e);
            }
        }
        return false;
    }

    // The app is always full screen (docs/design/ANDROID.md). SDL hides the bars with the deprecated
    // system UI flags; on Android 11+ that request did not reach the window (Retroid Pocket 5, Android 13:
    // the window asked for both bars visible, they covered the top and bottom of the picture). The
    // window's own insets controller keeps the request; a swipe from an edge shows them for a moment.
    private void hideSystemBars() {
        if (Build.VERSION.SDK_INT < 30 /* Android 11 (R) */) return;
        WindowInsetsController c = getWindow().getInsetsController();
        if (c == null) return;
        c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
        c.hide(WindowInsets.Type.systemBars());
    }
}
