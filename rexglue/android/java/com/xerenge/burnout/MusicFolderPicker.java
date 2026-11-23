package com.xerenge.burnout;

import android.content.ContentResolver;
import android.content.Context;
import android.content.Intent;
import android.database.Cursor;
import android.net.Uri;
import android.os.ParcelFileDescriptor;
import android.provider.DocumentsContract;
import android.util.Log;
import android.widget.Toast;

import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.OutputStreamWriter;
import java.io.Writer;
import java.nio.charset.StandardCharsets;
import java.util.ArrayDeque;
import java.util.HashMap;
import java.util.Locale;
import java.util.Map;

/** Indexes a picked document tree and opens its songs for the native player on demand. */
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
        Toast.makeText(activity, "Listing music\u2026", Toast.LENGTH_LONG).show();
        new Thread(() -> {
            String path = "";
            try {
                path = indexTree(activity, tree);
                Log.i("Xerenge", "music folder indexed as " + path);
            } catch (Exception error) {
                Log.e("Xerenge", "music folder indexing failed", error);
                String message = error.getMessage();
                activity.runOnUiThread(() -> Toast.makeText(activity,
                        message == null || message.isEmpty() ? "Could not read the selected folder" : message,
                        Toast.LENGTH_LONG).show());
            }
            nativeResult(path);
        }, "Xerenge music index").start();
        return true;
    }

    /** Native takes ownership of the returned descriptor. */
    public static int openFile(Context context, String uri) {
        try {
            ParcelFileDescriptor file = context.getContentResolver().openFileDescriptor(Uri.parse(uri), "r");
            return file == null ? -1 : file.detachFd();
        } catch (Exception error) {
            Log.e("Xerenge", "could not open music document " + uri, error);
            return -1;
        }
    }

    private static final Map<String, String> PLAYBACK_CACHE = new HashMap<>();
    private static final ArrayDeque<String> PLAYBACK_ORDER = new ArrayDeque<>();
    private static int playbackSequence;

    static void resetPlaybackCache(Context context) {
        PLAYBACK_CACHE.clear();
        PLAYBACK_ORDER.clear();
        deleteTree(new File(context.getFilesDir(), "saf-playback"));
    }

    /** Only songs actually played are cached, six at most; the library itself is not imported. */
    public static synchronized String cacheFile(Context context, String uri) {
        String existing = PLAYBACK_CACHE.get(uri);
        if (existing != null && new File(context.getFilesDir(), "saf-playback/" + existing).isFile()) {
            return existing;
        }
        String extension = uri.toLowerCase(Locale.ROOT).contains(".wma") ? ".wma" : ".mp3";
        String name = "song-" + playbackSequence++ + extension;
        File directory = new File(context.getFilesDir(), "saf-playback");
        directory.mkdirs();
        File destination = new File(directory, name);
        File partial = new File(directory, name + ".part");
        try (InputStream in = context.getContentResolver().openInputStream(Uri.parse(uri));
             FileOutputStream out = new FileOutputStream(partial)) {
            if (in == null) {
                return null;
            }
            byte[] buffer = new byte[64 * 1024];
            for (int got; (got = in.read(buffer)) >= 0; ) {
                out.write(buffer, 0, got);
            }
        } catch (Exception error) {
            Log.e("Xerenge", "could not cache music document " + uri, error);
            partial.delete();
            return null;
        }
        if (!partial.renameTo(destination)) {
            partial.delete();
            return null;
        }
        if (PLAYBACK_ORDER.size() == 6) {
            String old = PLAYBACK_ORDER.removeFirst();
            String oldName = PLAYBACK_CACHE.remove(old);
            if (oldName != null) {
                new File(directory, oldName).delete();
            }
        }
        PLAYBACK_CACHE.put(uri, name);
        PLAYBACK_ORDER.addLast(uri);
        return name;
    }

    private static String indexTree(BurnoutActivity activity, Uri tree) throws Exception {
        File index = new File(activity.getFilesDir(), "music-index.tsv");
        File partial = new File(activity.getFilesDir(), "music-index.tsv.part");
        int count;
        try (Writer out = new OutputStreamWriter(new FileOutputStream(partial), StandardCharsets.UTF_8)) {
            String root = DocumentsContract.getTreeDocumentId(tree);
            count = writeTree(activity.getContentResolver(), tree, root, "", out);
        }
        if (count == 0) {
            partial.delete();
            throw new Exception("No MP3 or WMA files found in the selected folder");
        }
        if (index.exists() && !index.delete()) {
            throw new Exception("Could not replace the music index");
        }
        if (!partial.renameTo(index)) {
            throw new Exception("Could not save the music index");
        }
        // Remove copies made by the old importer; the index and descriptors
        // now refer to the provider's own files instead.
        deleteTree(new File(Settings.root(activity), "imported-music"));
        deleteTree(new File(Settings.root(activity), "imported-music.next"));
        Log.i("Xerenge", count + " music file(s) indexed without copying");
        return "saf:" + index.getAbsolutePath();
    }

    private static int writeTree(ContentResolver resolver, Uri tree, String documentId, String prefix, Writer out)
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
                    count += writeTree(resolver, tree, id, prefix + name + "/", out);
                    continue;
                }
                String lower = name.toLowerCase(Locale.ROOT);
                if (!lower.endsWith(".mp3") && !lower.endsWith(".wma")) {
                    continue;
                }
                Uri document = DocumentsContract.buildDocumentUriUsingTree(tree, id);
                out.write(prefix + name + "\t" + document + "\n");
                ++count;
            }
        }
        return count;
    }

    /** An opened URI, by native code for tags or the song being played. */
    private static String safeName(String name) {
        if (name == null || name.isEmpty() || name.equals(".") || name.equals("..")) {
            return "unnamed";
        }
        return name.replace('/', '_').replace('\\', '_').replace('\t', ' ')
                .replace('\r', ' ').replace('\n', ' ');
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
