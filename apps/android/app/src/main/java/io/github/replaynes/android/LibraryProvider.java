// The app's files folder (Android/data/<app id>/files: the library, ROM/ and Projects/, and the Trash) as a
// documents provider (docs/design/ANDROID.md, "Opening a folder"). Since Android 11 other apps cannot list
// Android/data, and a file:// URI handed to another app throws FileUriExposedException, so a file manager
// reaches the library only through a provider: the system Files app and the document picker show it as a
// root, and MainActivity.openFolder opens one of its folders there. Read, write, create, rename, delete.
// SPDX-License-Identifier: GPL-2.0-or-later
package io.github.replaynes.android;

import android.content.Context;
import android.content.res.AssetFileDescriptor;
import android.database.Cursor;
import android.database.MatrixCursor;
import android.graphics.Point;
import android.os.CancellationSignal;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import android.provider.DocumentsContract.Document;
import android.provider.DocumentsContract.Root;
import android.provider.DocumentsProvider;
import android.webkit.MimeTypeMap;

import java.io.File;
import java.io.FileNotFoundException;
import java.io.IOException;

public class LibraryProvider extends DocumentsProvider {
    static final String ROOT_ID = "files";
    // A document id is the path below the files folder ("" for the folder itself) behind this prefix.
    private static final String DOC_PREFIX = "files:";

    private static final String[] ROOT_COLUMNS = {
        Root.COLUMN_ROOT_ID, Root.COLUMN_FLAGS, Root.COLUMN_TITLE, Root.COLUMN_DOCUMENT_ID,
        Root.COLUMN_ICON, Root.COLUMN_AVAILABLE_BYTES, Root.COLUMN_MIME_TYPES
    };
    private static final String[] DOCUMENT_COLUMNS = {
        Document.COLUMN_DOCUMENT_ID, Document.COLUMN_DISPLAY_NAME, Document.COLUMN_MIME_TYPE,
        Document.COLUMN_FLAGS, Document.COLUMN_SIZE, Document.COLUMN_LAST_MODIFIED
    };

    static String authority(Context c) { return c.getPackageName() + ".documents"; }

    static File base(Context c) {
        File ext = c.getExternalFilesDir(null);
        return ext != null ? ext : c.getFilesDir();  // the same choice as platform_android.cpp
    }

    /** The document id of a file under the files folder, or null outside it. */
    static String documentId(Context c, File f) throws IOException {
        String b = base(c).getCanonicalPath();
        String p = f.getCanonicalPath();
        if (p.equals(b)) return DOC_PREFIX;
        if (!p.startsWith(b + "/")) return null;
        return DOC_PREFIX + p.substring(b.length() + 1);
    }

    private File fileFor(String docId) throws FileNotFoundException {
        if (docId == null || !docId.startsWith(DOC_PREFIX)) throw new FileNotFoundException(docId);
        File b = base(getContext());
        String rel = docId.substring(DOC_PREFIX.length());
        File f = rel.isEmpty() ? b : new File(b, rel);
        try {
            if (documentId(getContext(), f) == null) throw new FileNotFoundException(docId);  // "..": outside
        } catch (IOException e) {
            throw new FileNotFoundException(docId);
        }
        return f;
    }

    private String idFor(File f) throws FileNotFoundException {
        try {
            String id = documentId(getContext(), f);
            if (id != null) return id;
        } catch (IOException e) {
            // below
        }
        throw new FileNotFoundException(f.getPath());
    }

    private static String mimeType(File f) {
        if (f.isDirectory()) return Document.MIME_TYPE_DIR;
        String name = f.getName();
        int dot = name.lastIndexOf('.');
        if (dot >= 0) {
            String t = MimeTypeMap.getSingleton().getMimeTypeFromExtension(name.substring(dot + 1).toLowerCase());
            if (t != null) return t;
        }
        return "application/octet-stream";
    }

    private void addRow(MatrixCursor c, File f) throws FileNotFoundException {
        String id = idFor(f);
        int flags = Document.FLAG_SUPPORTS_DELETE | Document.FLAG_SUPPORTS_RENAME;
        if (f.isDirectory()) flags |= Document.FLAG_DIR_SUPPORTS_CREATE;
        else flags |= Document.FLAG_SUPPORTS_WRITE;
        if (id.equals(DOC_PREFIX)) flags &= ~(Document.FLAG_SUPPORTS_DELETE | Document.FLAG_SUPPORTS_RENAME);
        MatrixCursor.RowBuilder row = c.newRow();
        row.add(Document.COLUMN_DOCUMENT_ID, id);
        row.add(Document.COLUMN_DISPLAY_NAME, id.equals(DOC_PREFIX) ? appName() : f.getName());
        row.add(Document.COLUMN_MIME_TYPE, mimeType(f));
        row.add(Document.COLUMN_FLAGS, flags);
        row.add(Document.COLUMN_SIZE, f.isDirectory() ? null : f.length());
        row.add(Document.COLUMN_LAST_MODIFIED, f.lastModified());
    }

    private String appName() {
        return getContext().getApplicationInfo().loadLabel(getContext().getPackageManager()).toString();
    }

    private void notifyChange(File dir) {
        try {
            String id = documentId(getContext(), dir);
            if (id == null) return;
            getContext().getContentResolver().notifyChange(
                DocumentsContract.buildChildDocumentsUri(authority(getContext()), id), null, false);
        } catch (IOException e) {
            // nothing to refresh
        }
    }

    @Override
    public boolean onCreate() { return true; }

    @Override
    public Cursor queryRoots(String[] projection) {
        MatrixCursor c = new MatrixCursor(projection != null ? projection : ROOT_COLUMNS);
        File b = base(getContext());
        b.mkdirs();
        MatrixCursor.RowBuilder row = c.newRow();
        row.add(Root.COLUMN_ROOT_ID, ROOT_ID);
        row.add(Root.COLUMN_FLAGS, Root.FLAG_SUPPORTS_CREATE | Root.FLAG_SUPPORTS_IS_CHILD | Root.FLAG_LOCAL_ONLY);
        row.add(Root.COLUMN_TITLE, appName());
        row.add(Root.COLUMN_DOCUMENT_ID, DOC_PREFIX);
        row.add(Root.COLUMN_ICON, getContext().getApplicationInfo().icon);
        row.add(Root.COLUMN_AVAILABLE_BYTES, b.getUsableSpace());
        row.add(Root.COLUMN_MIME_TYPES, "*/*");
        return c;
    }

    @Override
    public Cursor queryDocument(String documentId, String[] projection) throws FileNotFoundException {
        MatrixCursor c = new MatrixCursor(projection != null ? projection : DOCUMENT_COLUMNS);
        File f = fileFor(documentId);
        if (!f.exists()) throw new FileNotFoundException(documentId);
        addRow(c, f);
        return c;
    }

    @Override
    public Cursor queryChildDocuments(String parentDocumentId, String[] projection, String sortOrder)
            throws FileNotFoundException {
        MatrixCursor c = new MatrixCursor(projection != null ? projection : DOCUMENT_COLUMNS);
        File dir = fileFor(parentDocumentId);
        File[] children = dir.listFiles();
        if (children != null)
            for (File f : children) addRow(c, f);
        c.setNotificationUri(getContext().getContentResolver(),
            DocumentsContract.buildChildDocumentsUri(authority(getContext()), parentDocumentId));
        return c;
    }

    @Override
    public boolean isChildDocument(String parentDocumentId, String documentId) {
        if (!parentDocumentId.startsWith(DOC_PREFIX) || !documentId.startsWith(DOC_PREFIX)) return false;
        if (parentDocumentId.equals(DOC_PREFIX)) return true;
        return documentId.startsWith(parentDocumentId + "/");
    }

    // The Files app asks for the path from the root before it shows a folder it was asked to open
    // (MainActivity.openFolder); without this it falls back to its default location.
    @Override
    public DocumentsContract.Path findDocumentPath(String parentDocumentId, String childDocumentId)
            throws FileNotFoundException {
        fileFor(childDocumentId);
        String from = parentDocumentId != null ? parentDocumentId : DOC_PREFIX;
        if (!from.equals(childDocumentId) && !isChildDocument(from, childDocumentId))
            throw new FileNotFoundException(childDocumentId + " is not below " + from);
        java.util.ArrayList<String> ids = new java.util.ArrayList<>();
        ids.add(from);
        String rest = childDocumentId.substring(from.length());
        if (rest.startsWith("/")) rest = rest.substring(1);
        String at = from;
        if (!rest.isEmpty())
            for (String part : rest.split("/")) {
                at = at.equals(DOC_PREFIX) ? DOC_PREFIX + part : at + "/" + part;
                ids.add(at);
            }
        return new DocumentsContract.Path(parentDocumentId == null ? ROOT_ID : null, ids);
    }

    @Override
    public ParcelFileDescriptor openDocument(String documentId, String mode, CancellationSignal signal)
            throws FileNotFoundException {
        File f = fileFor(documentId);
        int m = ParcelFileDescriptor.parseMode(mode);
        if ((m & ParcelFileDescriptor.MODE_READ_ONLY) == m) return ParcelFileDescriptor.open(f, m);
        File parent = f.getParentFile();
        try {
            return ParcelFileDescriptor.open(f, m, new android.os.Handler(android.os.Looper.getMainLooper()),
                e -> notifyChange(parent));
        } catch (IOException e) {
            throw new FileNotFoundException(f + ": " + e.getMessage());
        }
    }

    @Override
    public AssetFileDescriptor openDocumentThumbnail(String documentId, Point sizeHint, CancellationSignal signal)
            throws FileNotFoundException {
        throw new FileNotFoundException(documentId);  // no thumbnails
    }

    @Override
    public String createDocument(String parentDocumentId, String mimeType, String displayName)
            throws FileNotFoundException {
        File dir = fileFor(parentDocumentId);
        File f = uniqueChild(dir, displayName);
        try {
            boolean made = Document.MIME_TYPE_DIR.equals(mimeType) ? f.mkdir() : f.createNewFile();
            if (!made) throw new FileNotFoundException("cannot create " + f);
        } catch (IOException e) {
            throw new FileNotFoundException("cannot create " + f + ": " + e.getMessage());
        }
        notifyChange(dir);
        return idFor(f);
    }

    // "name.zip", then "name (1).zip", ... as the Files app expects of a provider that does not overwrite.
    private static File uniqueChild(File dir, String name) {
        File f = new File(dir, name);
        if (!f.exists()) return f;
        int dot = name.lastIndexOf('.');
        String stem = dot > 0 ? name.substring(0, dot) : name;
        String ext = dot > 0 ? name.substring(dot) : "";
        for (int i = 1;; ++i) {
            f = new File(dir, stem + " (" + i + ")" + ext);
            if (!f.exists()) return f;
        }
    }

    @Override
    public String renameDocument(String documentId, String displayName) throws FileNotFoundException {
        File f = fileFor(documentId);
        File to = new File(f.getParentFile(), displayName);
        if (to.exists() || !f.renameTo(to)) throw new FileNotFoundException("cannot rename " + f);
        notifyChange(f.getParentFile());
        return idFor(to);
    }

    @Override
    public void deleteDocument(String documentId) throws FileNotFoundException {
        File f = fileFor(documentId);
        if (documentId.equals(DOC_PREFIX) || !deleteTree(f)) throw new FileNotFoundException("cannot delete " + f);
        notifyChange(f.getParentFile());
    }

    @Override
    public void removeDocument(String documentId, String parentDocumentId) throws FileNotFoundException {
        deleteDocument(documentId);
    }

    private static boolean deleteTree(File f) {
        File[] children = f.isDirectory() ? f.listFiles() : null;
        if (children != null)
            for (File c : children)
                if (!deleteTree(c)) return false;
        return f.delete();
    }
}
