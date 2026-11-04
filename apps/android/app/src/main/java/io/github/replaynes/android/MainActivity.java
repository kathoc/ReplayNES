// The game activity: SDL's activity with its two libraries (libSDL3.so, libmain.so = the app).
// SPDX-License-Identifier: GPL-2.0-or-later
package io.github.replaynes.android;

import android.content.ActivityNotFoundException;
import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.provider.DocumentsContract;
import android.util.Log;
import android.view.HapticFeedbackConstants;
import android.view.SurfaceHolder;
import android.view.WindowInsets;
import android.view.WindowInsetsController;

import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
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
        receive(getIntent());
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

    /** The library's ROM folder (main_android.cpp through JNI, at start-up): files shared before it was known go in now. */
    public void setRomDir(String dir) {
        romDir = dir;
        List<Uri> uris;
        synchronized (pendingImport) {
            uris = new ArrayList<>(pendingImport);
            pendingImport.clear();
        }
        if (!uris.isEmpty()) runOnUiThread(() -> RomImport.copyInto(this, uris, new File(dir)));
    }

    /** Add ROMs (importFiles in platform/shell.cpp through JNI, any thread): the system file picker, several files at once. */
    public boolean importFiles(String dir) {
        runOnUiThread(() -> {
            importDir = dir;
            Intent i = new Intent(Intent.ACTION_OPEN_DOCUMENT);
            i.addCategory(Intent.CATEGORY_OPENABLE);
            i.setType("*/*");
            i.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
            try {
                startActivityForResult(i, REQUEST_IMPORT);
            } catch (ActivityNotFoundException e) {
                // ACTION_GET_CONTENT: a device without the documents UI (some TV boxes) still has a file chooser.
                Intent g = new Intent(Intent.ACTION_GET_CONTENT);
                g.addCategory(Intent.CATEGORY_OPENABLE);
                g.setType("*/*");
                g.putExtra(Intent.EXTRA_ALLOW_MULTIPLE, true);
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

    @Override
    protected void onActivityResult(int requestCode, int resultCode, Intent data) {
        if (requestCode != REQUEST_IMPORT) {
            super.onActivityResult(requestCode, resultCode, data);
            return;
        }
        if (resultCode == RESULT_OK && importDir != null) RomImport.copyInto(this, RomImport.uris(data), new File(importDir));
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
        if (docId == null) {
            Log.w("ReplayNES", "openFolder: outside the app's files folder: " + path);
            return false;
        }
        Intent[] tries = {
            // The folder itself (DocumentsUI, Android 10+).
            new Intent(Intent.ACTION_VIEW).setDataAndType(
                DocumentsContract.buildDocumentUri(auth, docId), DocumentsContract.Document.MIME_TYPE_DIR),
            // The provider's root (a Files app that does not open a folder).
            new Intent(Intent.ACTION_VIEW).setDataAndType(
                DocumentsContract.buildRootUri(auth, LibraryProvider.ROOT_ID), "vnd.android.document/root"),
        };
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
