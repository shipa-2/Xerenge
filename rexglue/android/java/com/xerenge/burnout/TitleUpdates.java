package com.xerenge.burnout;

import android.util.Log;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.OutputStreamWriter;
import java.io.Writer;
import java.net.HttpURLConnection;
import java.net.URL;
import java.nio.charset.StandardCharsets;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

/** The retail title updates, beside default.xex, on Install and Update. */
final class TitleUpdates {
    interface Progress {
        void onProgress(int completed);
    }

    private static final String[] IDS = {"23567", "20954"};
    private static final String URL =
            "https://xboxunity.net/Resources/Lib/TitleUpdate.php?tuid=";
    private static final Pattern FILENAME = Pattern.compile("filename=([^;\\r\\n]+)");

    private TitleUpdates() {
    }

    /** Null when all are present or downloaded; otherwise the first error, after trying both. */
    static String installMissing(File game, Progress progress) {
        File record = new File(game, ".xerenge-title-updates");
        Map<String, String> installed = readRecord(record);
        String failure = null;
        for (int i = 0; i < IDS.length; ++i) {
            String id = IDS[i];
            File existing = new File(game, installed.getOrDefault(id, ""));
            if (!installed.containsKey(id) || !existing.isFile() || existing.length() == 0) {
                try {
                    String name = download(game, id);
                    installed.put(id, name);
                    writeRecord(record, installed);
                    Log.i("Xerenge", "title update " + id + " -> " + name);
                } catch (Exception error) {
                    Log.e("Xerenge", "title update " + id + " failed", error);
                    if (failure == null) {
                        failure = id + ": " + error.getMessage();
                    }
                }
            } else {
                Log.i("Xerenge", "title update " + id + " already installed: " + existing.getName());
            }
            if (progress != null) {
                progress.onProgress(i + 1);
            }
        }
        return failure;
    }

    private static String download(File game, String id) throws Exception {
        HttpURLConnection connection = (HttpURLConnection) new URL(URL + id).openConnection();
        connection.setConnectTimeout(20000);
        connection.setReadTimeout(60000);
        connection.setInstanceFollowRedirects(true);
        connection.setRequestProperty("User-Agent", "Xerenge-Android");
        File partial = new File(game, ".xerenge-tu-" + id + ".part");
        try {
            int status = connection.getResponseCode();
            if (status != HttpURLConnection.HTTP_OK) {
                throw new Exception("HTTP " + status);
            }
            String name = fileName(connection.getHeaderField("Content-Disposition"), id);
            File destination = new File(game, name);
            try (InputStream in = connection.getInputStream(); FileOutputStream out = new FileOutputStream(partial)) {
                byte[] buffer = new byte[64 * 1024];
                for (int got; (got = in.read(buffer)) >= 0; ) {
                    out.write(buffer, 0, got);
                }
            }
            if (partial.length() == 0) {
                throw new Exception("empty download");
            }
            if (destination.exists() && !destination.delete()) {
                throw new Exception("could not replace " + name);
            }
            if (!partial.renameTo(destination)) {
                throw new Exception("could not save " + name);
            }
            return name;
        } finally {
            partial.delete();
            connection.disconnect();
        }
    }

    private static String fileName(String disposition, String id) {
        if (disposition != null) {
            Matcher match = FILENAME.matcher(disposition);
            if (match.find()) {
                String name = match.group(1).trim().replace("\"", "");
                name = new File(name).getName().replace('\\', '_');
                if (!name.isEmpty()) {
                    return name;
                }
            }
        }
        return "tu_" + id + ".bin";
    }

    private static Map<String, String> readRecord(File file) {
        Map<String, String> result = new LinkedHashMap<>();
        try (BufferedReader in = new BufferedReader(new InputStreamReader(
                new FileInputStream(file), StandardCharsets.UTF_8))) {
            String line;
            while ((line = in.readLine()) != null) {
                int tab = line.indexOf('\t');
                if (tab > 0) {
                    result.put(line.substring(0, tab), line.substring(tab + 1).trim());
                }
            }
        } catch (Exception ignored) {
        }
        return result;
    }

    private static void writeRecord(File file, Map<String, String> installed) throws Exception {
        File partial = new File(file.getParentFile(), file.getName() + ".part");
        try (Writer out = new OutputStreamWriter(new FileOutputStream(partial), StandardCharsets.UTF_8)) {
            for (Map.Entry<String, String> entry : installed.entrySet()) {
                out.write(entry.getKey() + "\t" + entry.getValue() + "\n");
            }
        }
        if (file.exists() && !file.delete()) {
            throw new Exception("could not update the title-update record");
        }
        if (!partial.renameTo(file)) {
            throw new Exception("could not save the title-update record");
        }
    }
}
