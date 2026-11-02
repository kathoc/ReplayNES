// The game activity: SDL's activity with its two libraries (libSDL3.so, libmain.so = the app).
// SPDX-License-Identifier: GPL-2.0-or-later
package io.github.replaynes.android;

import android.os.Build;
import android.os.Bundle;
import android.view.HapticFeedbackConstants;
import android.view.WindowInsets;
import android.view.WindowInsetsController;

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
