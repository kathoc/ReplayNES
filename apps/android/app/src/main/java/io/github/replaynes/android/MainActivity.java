// The game activity: SDL's activity with its two libraries (libSDL3.so, libmain.so = the app).
// SPDX-License-Identifier: GPL-2.0-or-later
package io.github.replaynes.android;

import android.content.ActivityNotFoundException;
import android.content.Intent;
import android.net.Uri;
import android.os.Build;
import android.os.Bundle;
import android.provider.DocumentsContract;
import android.util.Log;
import android.view.HapticFeedbackConstants;
import android.view.WindowInsets;
import android.view.WindowInsetsController;

import java.io.File;
import java.io.IOException;
import java.util.ArrayList;
import java.util.List;

import org.libsdl.app.SDLActivity;

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
