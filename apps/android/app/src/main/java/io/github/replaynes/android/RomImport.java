// Copying ROMs into the library's ROM folder from any app (docs/design/ANDROID.md, "Adding ROMs"): the
// system file picker (Add ROMs), another app's Share or Open with, all as content:// (or file://) URIs that
// this app may read. Android/data, where the library lives, is closed to other apps since Android 11, so
// these routes do not depend on a file manager reaching it. Each file is written under a hidden name and
// renamed when complete: the library never lists a half-copied ROM.
// SPDX-License-Identifier: GPL-2.0-or-later
package io.github.replaynes.android;

import android.app.Activity;
import android.content.ClipData;
import android.content.ContentResolver;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.provider.OpenableColumns;
import android.util.Log;
import android.widget.Toast;

import java.io.File;
import java.io.FileOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.List;

final class RomImport {
    private static final String TAG = "ReplayNES";

    private RomImport() {}

    /** The URIs an intent carries: a picker's result (one or several), Share (SEND / SEND_MULTIPLE), Open with (VIEW). */
    static List<Uri> uris(Intent intent) {
        List<Uri> out = new ArrayList<>();
        if (intent == null) return out;
        ClipData clip = intent.getClipData();
        if (clip != null)
            for (int i = 0; i < clip.getItemCount(); ++i) {
                Uri u = clip.getItemAt(i).getUri();
                if (u != null && !out.contains(u)) out.add(u);
            }
        String action = intent.getAction();
        if (Intent.ACTION_SEND.equals(action)) {
            Uri u = intent.getParcelableExtra(Intent.EXTRA_STREAM);
            if (u != null && !out.contains(u)) out.add(u);
        } else if (Intent.ACTION_SEND_MULTIPLE.equals(action)) {
            ArrayList<Uri> list = intent.getParcelableArrayListExtra(Intent.EXTRA_STREAM);
            if (list != null)
                for (Uri u : list)
                    if (u != null && !out.contains(u)) out.add(u);
        }
        Uri data = intent.getData();
        if (data != null && !out.contains(data)) out.add(data);
        return out;
    }

    /** Copies the URIs into dir on a worker thread and says how it went in a toast. */
    static void copyInto(Activity activity, List<Uri> uris, File dir) {
        if (uris.isEmpty()) return;
        final ContentResolver resolver = activity.getContentResolver();
        new Thread(() -> {
            int added = 0;
            List<String> failed = new ArrayList<>();
            dir.mkdirs();
            for (Uri u : uris) {
                String name = displayName(resolver, u);
                try {
                    copy(resolver, u, dir, name);
                    added += 1;
                    Log.i(TAG, "import: " + name + " -> " + dir);
                } catch (IOException | SecurityException e) {
                    failed.add(name);
                    Log.w(TAG, "import: " + u + ": " + e);
                }
            }
            final String text = failed.isEmpty()
                ? activity.getResources().getQuantityString(R.plurals.rn_import_added, added, added)
                : activity.getString(R.string.rn_import_failed, String.join(", ", failed));
            activity.runOnUiThread(() -> Toast.makeText(activity, text, Toast.LENGTH_LONG).show());
        }, "rom-import").start();
    }

    private static String displayName(ContentResolver resolver, Uri u) {
        String name = null;
        if ("content".equals(u.getScheme())) {
            try (Cursor c = resolver.query(u, new String[] {OpenableColumns.DISPLAY_NAME}, null, null, null)) {
                if (c != null && c.moveToFirst() && !c.isNull(0)) name = c.getString(0);
            } catch (RuntimeException e) {
                // no name column: the path's last segment below
            }
        }
        if (name == null) name = u.getLastPathSegment();
        if (name == null) name = "rom";
        // Only the name: a provider's "a/b.zip" or a file URI's path must not leave the folder.
        name = name.substring(name.lastIndexOf('/') + 1).replace('\u0000', '_').trim();
        while (name.startsWith(".")) name = name.substring(1);  // hidden names are the library's half-copied files
        return name.isEmpty() ? "rom" : name;
    }

    // Same name: replaced (a newer set of the same game), as a copy over USB or with a file manager does.
    private static void copy(ContentResolver resolver, Uri u, File dir, String name) throws IOException {
        File part = new File(dir, "." + name + ".part");
        File dest = new File(dir, name);
        try (InputStream in = resolver.openInputStream(u)) {
            if (in == null) throw new IOException("cannot open");
            try (OutputStream out = new FileOutputStream(part)) {
                byte[] buf = new byte[1 << 16];
                for (int n; (n = in.read(buf)) > 0;) out.write(buf, 0, n);
            }
            if (dest.exists() && !dest.delete()) throw new IOException("cannot replace " + dest);
            if (!part.renameTo(dest)) throw new IOException("cannot rename " + part);
        } finally {
            if (part.exists()) part.delete();
        }
    }
}
