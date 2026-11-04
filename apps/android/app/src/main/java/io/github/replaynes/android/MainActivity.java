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

import org.libsdl.app.SDLActivity;

public class MainActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        return new String[] {"SDL3", "main"};
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        hideSystemBars();
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
            i.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK | Intent.FLAG_GRANT_READ_URI_PERMISSION
                       | Intent.FLAG_GRANT_WRITE_URI_PERMISSION);
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
