// Lines from the Java side into the log file main_android.cpp writes (<external files>/Logs/<app>.log),
// with the same time stamp: the activity's life while the native loop is paused (docs/design/ANDROID.md, "Logs").
// SPDX-License-Identifier: GPL-2.0-or-later
package io.github.replaynes.android;

import android.content.Context;
import android.util.Log;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.text.SimpleDateFormat;
import java.util.Date;
import java.util.Locale;

final class AppLog {
    private AppLog() {}

    static synchronized void line(Context c, String text) {
        Log.i("ReplayNES", text);
        File ext = c.getExternalFilesDir(null);
        if (ext == null) return;
        File log = new File(new File(ext, "Logs"), c.getString(R.string.app_name) + ".log");
        if (!log.getParentFile().isDirectory()) return;  // the native side makes it at start-up
        String stamp = new SimpleDateFormat("MM-dd HH:mm:ss.SSS", Locale.US).format(new Date());
        // Append mode (O_APPEND): whole lines between the native side's.
        try (FileOutputStream out = new FileOutputStream(log, true)) {
            out.write((stamp + " " + text + "\n").getBytes(StandardCharsets.UTF_8));
        } catch (IOException e) {
            // no log file: logcat has the line
        }
    }
}
