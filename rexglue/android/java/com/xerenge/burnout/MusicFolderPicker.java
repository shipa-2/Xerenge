package com.xerenge.burnout;

import android.content.ContentResolver;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.provider.DocumentsContract;
import android.util.Log;
import android.widget.Toast;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.util.Locale;

/** Imports a document tree picked in the game into a folder the native scanner can read. */
final class MusicFolderPicker {
    static final int REQUEST = 7311;
    private static native void nativeResult(String path);

    private MusicFolderPicker() {
    }

    static void pick(BurnoutActivity activity) {
        activity.runOnUiThread(() -> {
            try {
                activity.startActivityForResult(new Intent(Intent.ACTION_OPEN_DOCUMENT_TREE), REQUEST);
            } catch (Exception error) {
                Log.e("Xerenge", "music folder picker failed", error);
                nativeResult("");
            }
        });
    }

    static boolean onActivityResult(BurnoutActivity activity, int request, int result, Intent data) {
        if (request != REQUEST) {
            return false;
        }
        if (result != BurnoutActivity.RESULT_OK || data == null || data.getData() == null) {
            nativeResult("");
            return true;
        }
        Uri tree = data.getData();
        try {
            activity.getContentResolver().takePersistableUriPermission(tree, Intent.FLAG_GRANT_READ_URI_PERMISSION);
        } catch (Exception ignored) {
        }
        Toast.makeText(activity, "Importing music\u2026", Toast.LENGTH_LONG).show();
        new Thread(() -> {
            String path = "";
            try {
                path = importTree(activity, tree);
                Log.i("Xerenge", "music folder imported to " + path);
            } catch (Exception error) {
                Log.e("Xerenge", "music folder import failed", error);
                String message = error.getMessage();
                activity.runOnUiThread(() -> Toast.makeText(activity,
                        message == null || message.isEmpty() ? "Could not import the selected folder" : message,
                        Toast.LENGTH_LONG).show());
            } finally {
                try {
                    activity.getContentResolver().releasePersistableUriPermission(tree,
                            Intent.FLAG_GRANT_READ_URI_PERMISSION);
                } catch (Exception ignored) {
                }
            }
            nativeResult(path);
        }, "Xerenge music import").start();
        return true;
    }

    private static String importTree(BurnoutActivity activity, Uri tree) throws Exception {
        File parent = Settings.root(activity);
        File next = new File(parent, "imported-music.next");
        File destination = new File(parent, "imported-music");
        deleteTree(next);
        if (!next.mkdirs()) {
            throw new Exception("Could not create the imported music folder");
        }
        try {
            String root = DocumentsContract.getTreeDocumentId(tree);
            int count = copyTree(activity.getContentResolver(), tree, root, next);
            if (count == 0) {
                throw new Exception("No MP3 or WMA files found in the selected folder");
            }
            deleteTree(destination);
            if (!next.renameTo(destination)) {
                throw new Exception("Could not finish the music import");
            }
            Log.i("Xerenge", count + " music file(s) imported");
            return destination.getAbsolutePath();
        } catch (Exception error) {
            deleteTree(next);
            throw error;
        }
    }

    private static int copyTree(ContentResolver resolver, Uri tree, String documentId, File destination)
            throws Exception {
        Uri children = DocumentsContract.buildChildDocumentsUriUsingTree(tree, documentId);
        String[] columns = {DocumentsContract.Document.COLUMN_DOCUMENT_ID,
                DocumentsContract.Document.COLUMN_DISPLAY_NAME, DocumentsContract.Document.COLUMN_MIME_TYPE};
        int count = 0;
        try (Cursor cursor = resolver.query(children, columns, null, null, null)) {
            if (cursor == null) {
                throw new Exception("Could not read the selected folder");
            }
            while (cursor.moveToNext()) {
                String id = cursor.getString(0);
                String name = safeName(cursor.getString(1));
                String mime = cursor.getString(2);
                if (DocumentsContract.Document.MIME_TYPE_DIR.equals(mime)) {
                    File child = new File(destination, name);
                    if (!child.mkdirs() && !child.isDirectory()) {
                        throw new Exception("Could not create " + name);
                    }
                    count += copyTree(resolver, tree, id, child);
                    continue;
                }
                String lower = name.toLowerCase(Locale.ROOT);
                if (!lower.endsWith(".mp3") && !lower.endsWith(".wma")) {
                    continue;
                }
                Uri document = DocumentsContract.buildDocumentUriUsingTree(tree, id);
                try (InputStream in = resolver.openInputStream(document);
                     FileOutputStream out = new FileOutputStream(new File(destination, name))) {
                    if (in == null) {
                        throw new Exception("Could not open " + name);
                    }
                    byte[] buffer = new byte[64 * 1024];
                    for (int got; (got = in.read(buffer)) >= 0; ) {
                        out.write(buffer, 0, got);
                    }
                }
                ++count;
            }
        }
        return count;
    }

    private static String safeName(String name) {
        if (name == null || name.isEmpty() || name.equals(".") || name.equals("..")) {
            return "unnamed";
        }
        return name.replace('/', '_').replace('\\', '_');
    }

    private static void deleteTree(File file) {
        File[] children = file.listFiles();
        if (children != null) {
            for (File child : children) {
                deleteTree(child);
            }
        }
        file.delete();
    }
}
