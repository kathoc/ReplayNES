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
import android.content.Context;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.provider.DocumentsContract;
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

    /**
     * The picker's types (EXTRA_MIME_TYPES): ROM files only. Without them the picker offers Images, Audio, Videos
     * and Documents, views of some kinds of files only where no zip ever appears; with them it offers the device's
     * folders, Downloads and Recent, and every ROM there can be picked (.zip in any case, .7z, .nes, .fds, .unf:
     * checked on the Android 16 emulator 2026-11-06).
     */
    static final String[] ROM_TYPES = {
        "application/zip", "application/x-zip-compressed", "application/x-zip", "application/x-compressed",
        "application/x-zip-archive", "multipart/x-zip", "application/x-7z-compressed", "application/octet-stream",
        "application/x-nes-rom",
    };

    /** The library lists a file of this name (shell.cpp; the product's ROM extensions and filter, by name). */
    private static native boolean nativeIsRom(String name);

    private static boolean isRom(String name) {
        try {
            return nativeIsRom(name);
        } catch (UnsatisfiedLinkError e) {
            return true;  // no native library yet: the library's scan still lists ROMs only
        }
    }

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

    private static final String PREFS = "rom_import", LAST_FOLDER = "last_folder";

    /**
     * Where Add ROMs' picker starts (EXTRA_INITIAL_URI): the device storage folder the last ROMs came from, else
     * Download. A folder of the external storage provider lists every file; the picker's "Documents" and
     * "Recent" do not list zips.
     */
    static Uri startFolder(Context c) {
        String id = c.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getString(LAST_FOLDER, "primary:Download");
        return DocumentsContract.buildDocumentUri(StorageFolders.EXTERNAL_STORAGE, id);
    }

    /** The folder of a picked file, for the next startFolder (only a file of the external storage provider). */
    static void rememberFolder(Context c, Uri picked) {
        AppLog.line(c, "import: picked " + picked);
        if (!StorageFolders.EXTERNAL_STORAGE.equals(picked.getAuthority())) return;
        String id;
        try {
            id = DocumentsContract.getDocumentId(picked);
        } catch (IllegalArgumentException e) {
            return;
        }
        int slash = id.lastIndexOf('/');
        int colon = id.indexOf(':');
        if (colon < 0) return;
        String folder = slash > colon ? id.substring(0, slash) : id.substring(0, colon + 1);
        c.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit().putString(LAST_FOLDER, folder).apply();
        AppLog.line(c, "import: remembered " + folder);
    }

    /** Copies the URIs into dir on a worker thread and says how it went in a toast. */
    static void copyInto(Activity activity, List<Uri> uris, File dir) {
        if (uris.isEmpty()) return;
        final ContentResolver resolver = activity.getContentResolver();
        new Thread(() -> {
            int added = 0;
            List<String> failed = new ArrayList<>();
            List<String> notRom = new ArrayList<>();
            dir.mkdirs();
            for (Uri u : uris) {
                String name = displayName(resolver, u);
                if (!isRom(name)) {  // a share takes any file (AndroidManifest.xml): ROMs only into the folder
                    notRom.add(name);
                    Log.i(TAG, "import: " + name + ": not a ROM, not added");
                    continue;
                }
                try {
                    copy(resolver, u, dir, name);
                    added += 1;
                    Log.i(TAG, "import: " + name + " -> " + dir);
                } catch (IOException | SecurityException e) {
                    failed.add(name);
                    Log.w(TAG, "import: " + u + ": " + e);
                }
            }
            List<String> lines = new ArrayList<>();
            if (added > 0 || (failed.isEmpty() && notRom.isEmpty()))
                lines.add(activity.getResources().getQuantityString(R.plurals.rn_import_added, added, added));
            if (!failed.isEmpty()) lines.add(activity.getString(R.string.rn_import_failed, String.join(", ", failed)));
            if (!notRom.isEmpty()) lines.add(activity.getString(R.string.rn_import_not_rom, String.join(", ", notRom)));
            final String text = String.join("\n", lines);
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
