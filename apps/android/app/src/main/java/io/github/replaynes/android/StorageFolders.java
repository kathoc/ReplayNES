// A ROM or Projects folder outside the app's own files (docs/design/ANDROID.md, "Choosing the folders"):
// the system's folder picker answers with a tree URI of the external storage provider, the library reads
// paths (with all-files access), and Files opens such a folder as that provider's document.
// SPDX-License-Identifier: GPL-2.0-or-later
package io.github.replaynes.android;

import android.net.Uri;
import android.os.Environment;
import android.provider.DocumentsContract;

import java.io.File;

final class StorageFolders {
    static final String EXTERNAL_STORAGE = "com.android.externalstorage.documents";
    private static final String DOWNLOADS = "com.android.providers.downloads.documents";

    private StorageFolders() {}

    /** The path of a folder the picker chose; null for a folder that is no local path (a cloud drive). */
    static String treeToPath(Uri tree) {
        if (tree == null) return null;
        String id = DocumentsContract.getTreeDocumentId(tree);
        if (DOWNLOADS.equals(tree.getAuthority()) && id.startsWith("raw:")) return id.substring(4);
        if (!EXTERNAL_STORAGE.equals(tree.getAuthority())) return null;
        int colon = id.indexOf(':');
        if (colon < 0) return null;
        String volume = id.substring(0, colon), rel = id.substring(colon + 1);
        // "primary": the device's own storage; else a card or a USB drive, mounted as /storage/<volume id>.
        File base = "primary".equalsIgnoreCase(volume) ? Environment.getExternalStorageDirectory() : new File("/storage", volume);
        return rel.isEmpty() ? base.getPath() : new File(base, rel).getPath();
    }

    /** The external storage provider's document id of a path on the device or a card; null elsewhere. */
    static String documentId(String path) {
        String own = Environment.getExternalStorageDirectory().getPath();
        if (path.equals(own)) return "primary:";
        if (path.startsWith(own + "/")) return "primary:" + path.substring(own.length() + 1);
        if (!path.startsWith("/storage/")) return null;
        String rest = path.substring("/storage/".length());
        int slash = rest.indexOf('/');
        String volume = slash < 0 ? rest : rest.substring(0, slash);
        if (volume.isEmpty() || volume.equals("emulated") || volume.equals("self")) return null;
        return volume + ":" + (slash < 0 ? "" : rest.substring(slash + 1));
    }

    static Uri documentUri(String path) {
        String id = documentId(path);
        return id == null ? null : DocumentsContract.buildDocumentUri(EXTERNAL_STORAGE, id);
    }
}
