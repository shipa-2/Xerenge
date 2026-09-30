package com.xerenge.burnout;

import android.app.Activity;
import android.content.Intent;
import android.content.SharedPreferences;
import android.content.pm.PackageManager;
import android.graphics.Color;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.TextView;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

import org.json.JSONObject;

/**
 * What the app opens with. When the game is installed and the GPU driver is
 * good enough it starts the game straight away; otherwise it is the installer:
 *
 *  - the GPU: the shaders need Vulkan 1.2 (64-bit integers, descriptor
 *    indexing). Stock Adreno 6xx drivers report 1.1; such a phone gets Mesa
 *    Turnip, from a driver package (.zip with meta.json, the format emulators
 *    use) the user picks, copied into internal storage and loaded by the game
 *    through libadrenotools.
 *  - the game: the retail disc image, picked with the system file picker,
 *    checked and extracted into the app's external files directory.
 */
public class InstallerActivity extends Activity {
    static final String PREFS = "installer";
    static final String PREF_DRIVER = "driver";  // absolute path of the driver .so, or absent

    private static final int PICK_DRIVER = 1;
    private static final int PICK_IMAGE = 2;
    // FEATURE_VULKAN_HARDWARE_VERSION encodes versions like VK_MAKE_VERSION.
    private static final int VULKAN_1_2 = (1 << 22) | (2 << 12);

    private TextView gpuStatus;
    private TextView gameStatus;
    private TextView progressText;
    private ProgressBar progressBar;
    private Button driverButton;
    private Button systemDriverButton;
    private Button imageButton;
    private Button startButton;
    private boolean busy;
    private final Handler ui = new Handler(Looper.getMainLooper());

    static {
        System.loadLibrary("xerenge_installer");
    }

    /** Checks and extracts the image; returns null or what went wrong. */
    private static native String nativeInstall(int fd, String destination, boolean verify, Progress progress);

    /** Called from the native side, on the worker thread. */
    public interface Progress {
        void onProgress(int phase, long done, long total);
    }

    static File gameDir(Activity activity) {
        return new File(activity.getExternalFilesDir(null), "game");
    }

    static boolean gameInstalled(Activity activity) {
        return new File(gameDir(activity), "default.xex").isFile();
    }

    private boolean vulkanIsEnough() {
        return getPackageManager().hasSystemFeature(PackageManager.FEATURE_VULKAN_HARDWARE_VERSION, VULKAN_1_2);
    }

    private String installedDriver() {
        String path = getSharedPreferences(PREFS, MODE_PRIVATE).getString(PREF_DRIVER, null);
        return path != null && new File(path).isFile() ? path : null;
    }

    private boolean ready() {
        return gameInstalled(this) && (vulkanIsEnough() || installedDriver() != null);
    }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        // The launcher shortcut passes "setup" as a string, an Intent as a boolean.
        boolean setup = getIntent().getBooleanExtra("setup", false) ||
                "true".equals(getIntent().getStringExtra("setup"));
        if (ready() && !setup) {
            startGame();
            return;
        }
        buildUi();
        refresh();
    }

    private void startGame() {
        startActivity(new Intent(this, BurnoutActivity.class));
        finish();
    }

    // --- the screen -------------------------------------------------------

    private TextView text(LinearLayout parent, String value, float size, boolean bold) {
        TextView view = new TextView(this);
        view.setText(value);
        view.setTextSize(size);
        view.setTextColor(Color.WHITE);
        if (bold) {
            view.setTypeface(Typeface.DEFAULT_BOLD);
        }
        view.setPadding(0, dp(6), 0, dp(6));
        parent.addView(view);
        return view;
    }

    private Button button(LinearLayout parent, int label, View.OnClickListener action) {
        Button view = new Button(this);
        view.setText(label);
        view.setAllCaps(false);
        view.setOnClickListener(action);
        parent.addView(view, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));
        return view;
    }

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    private void buildUi() {
        LinearLayout column = new LinearLayout(this);
        column.setOrientation(LinearLayout.VERTICAL);
        column.setPadding(dp(24), dp(16), dp(24), dp(16));
        column.setBackgroundColor(Color.rgb(18, 18, 22));

        text(column, getString(R.string.installer_title), 24, true);

        text(column, getString(R.string.section_gpu), 18, true);
        gpuStatus = text(column, "", 15, false);
        driverButton = button(column, R.string.pick_driver, v -> pick(PICK_DRIVER, "application/zip"));
        systemDriverButton = button(column, R.string.use_system_driver, v -> removeDriver());

        text(column, getString(R.string.section_game), 18, true);
        gameStatus = text(column, "", 15, false);
        imageButton = button(column, R.string.pick_image, v -> pick(PICK_IMAGE, "*/*"));
        progressBar = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        progressBar.setMax(1000);
        progressBar.setVisibility(View.GONE);
        column.addView(progressBar);
        progressText = text(column, "", 14, false);

        startButton = button(column, R.string.start_game, v -> startGame());
        startButton.setGravity(Gravity.CENTER);

        ScrollView scroll = new ScrollView(this);
        scroll.setBackgroundColor(Color.rgb(18, 18, 22));
        scroll.addView(column);
        setContentView(scroll);
    }

    private void refresh() {
        String driver = installedDriver();
        if (driver != null) {
            gpuStatus.setText(getString(R.string.gpu_custom, new File(driver).getName()));
        } else if (vulkanIsEnough()) {
            gpuStatus.setText(R.string.gpu_ok);
        } else {
            gpuStatus.setText(R.string.gpu_too_old);
        }
        systemDriverButton.setVisibility(driver != null ? View.VISIBLE : View.GONE);
        gameStatus.setText(gameInstalled(this) ? R.string.game_installed : R.string.game_missing);
        driverButton.setEnabled(!busy);
        systemDriverButton.setEnabled(!busy);
        imageButton.setEnabled(!busy);
        startButton.setEnabled(!busy && ready());
    }

    private void pick(int request, String type) {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType(type);
        startActivityForResult(intent, request);
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (result != RESULT_OK || data == null || data.getData() == null) {
            return;
        }
        Uri uri = data.getData();
        if (request == PICK_DRIVER) {
            installDriver(uri);
        } else if (request == PICK_IMAGE) {
            installGame(uri);
        }
    }

    // --- the driver -------------------------------------------------------

    private void removeDriver() {
        getSharedPreferences(PREFS, MODE_PRIVATE).edit().remove(PREF_DRIVER).apply();
        refresh();
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

    /** A driver package: a zip with meta.json naming the library, as the emulators' packages are. */
    private void installDriver(Uri uri) {
        File directory = new File(getFilesDir(), "driver");
        deleteTree(directory);
        directory.mkdirs();
        String library = null;
        try (InputStream in = getContentResolver().openInputStream(uri);
             ZipInputStream zip = new ZipInputStream(in)) {
            ZipEntry entry;
            byte[] buffer = new byte[1 << 16];
            while ((entry = zip.getNextEntry()) != null) {
                String name = new File(entry.getName()).getName();
                if (entry.isDirectory() || name.isEmpty()) {
                    continue;
                }
                if (name.equals("meta.json")) {
                    ByteArrayOutputStream text = new ByteArrayOutputStream();
                    int got;
                    while ((got = zip.read(buffer)) > 0) {
                        text.write(buffer, 0, got);
                    }
                    library = new JSONObject(text.toString("UTF-8")).optString("libraryName", null);
                    continue;
                }
                if (!name.endsWith(".so")) {
                    continue;
                }
                try (FileOutputStream out = new FileOutputStream(new File(directory, name))) {
                    int got;
                    while ((got = zip.read(buffer)) > 0) {
                        out.write(buffer, 0, got);
                    }
                }
            }
        } catch (Exception error) {
            progressText.setText(getString(R.string.driver_failed, String.valueOf(error.getMessage())));
            return;
        }
        if (library == null) {
            // No meta.json: the only library in the package, if there is just one.
            File[] libraries = directory.listFiles((dir, n) -> n.endsWith(".so"));
            if (libraries != null && libraries.length == 1) {
                library = libraries[0].getName();
            }
        }
        File driver = library != null ? new File(directory, library) : null;
        if (driver == null || !driver.isFile()) {
            progressText.setText(R.string.driver_not_a_package);
            return;
        }
        getSharedPreferences(PREFS, MODE_PRIVATE).edit().putString(PREF_DRIVER, driver.getAbsolutePath()).apply();
        progressText.setText(getString(R.string.driver_installed, driver.getName()));
        refresh();
    }

    // --- the game ---------------------------------------------------------

    private void installGame(Uri uri) {
        final ParcelFileDescriptor descriptor;
        try {
            descriptor = getContentResolver().openFileDescriptor(uri, "r");
        } catch (Exception error) {
            progressText.setText(getString(R.string.image_failed, String.valueOf(error.getMessage())));
            return;
        }
        if (descriptor == null) {
            return;
        }
        final File destination = gameDir(this);
        busy = true;
        progressBar.setVisibility(View.VISIBLE);
        progressBar.setProgress(0);
        refresh();
        new Thread(() -> {
            deleteTree(destination);
            destination.mkdirs();
            final long[] last = {0};
            String error = nativeInstall(descriptor.getFd(), destination.getAbsolutePath(), true,
                    (phase, done, total) -> {
                        long now = System.currentTimeMillis();
                        if (now - last[0] < 200) {
                            return;
                        }
                        last[0] = now;
                        ui.post(() -> showProgress(phase, done, total));
                    });
            try {
                descriptor.close();
            } catch (Exception ignored) {
            }
            ui.post(() -> {
                busy = false;
                progressBar.setVisibility(View.GONE);
                if (error != null) {
                    deleteTree(destination);
                    progressText.setText(getString(R.string.image_failed, error));
                } else {
                    progressText.setText(R.string.image_done);
                }
                refresh();
            });
        }, "installer").start();
    }

    // The retail image holds 832 files.
    private static final int IMAGE_FILES = 832;

    private void showProgress(int phase, long done, long total) {
        if (phase == 0) {
            int percent = total > 0 ? (int) (100 * done / total) : 0;
            progressBar.setProgress(percent * 5);
            progressText.setText(getString(R.string.checking, percent));
        } else {
            progressBar.setProgress(500 + (int) (500 * Math.min(done, IMAGE_FILES) / IMAGE_FILES));
            progressText.setText(getString(R.string.extracting, (int) done, IMAGE_FILES));
        }
    }
}
