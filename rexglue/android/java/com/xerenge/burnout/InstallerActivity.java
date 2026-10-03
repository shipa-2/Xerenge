package com.xerenge.burnout;

import android.app.Activity;
import android.content.Intent;
import android.content.pm.PackageManager;
import android.content.res.ColorStateList;
import android.graphics.Color;
import android.graphics.Typeface;
import android.net.Uri;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.ParcelFileDescriptor;
import android.provider.OpenableColumns;
import android.database.Cursor;
import android.text.Editable;
import android.text.InputFilter;
import android.text.InputType;
import android.text.TextWatcher;
import android.view.Gravity;
import android.view.View;
import android.view.ViewGroup;
import android.widget.AdapterView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.CheckBox;
import android.widget.EditText;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ProgressBar;
import android.widget.ScrollView;
import android.widget.SeekBar;
import android.widget.Spinner;
import android.widget.TextView;

import java.io.ByteArrayOutputStream;
import java.io.File;
import java.io.FileOutputStream;
import java.io.InputStream;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

import org.json.JSONObject;

/**
 * What the app opens with: the desktop installer (rexglue/installer) on a
 * phone - the same tabs, settings and steps, writing the same xerenge.conf.
 * When the game is installed and the GPU driver will do, it starts the game
 * at once instead; a long press on the icon (Setup) brings it back.
 *
 * On top of the desktop one, the graphics driver: the game needs Vulkan 1.1,
 * which the stock drivers of the phones it targets report. A phone without it,
 * or a player who prefers it, can load Mesa Turnip from a driver package (a
 * .zip with meta.json, as the emulators use), copied into internal storage and
 * opened by the game through libadrenotools.
 */
public class InstallerActivity extends Activity {
    static final String PREFS = "installer";
    static final String PREF_DRIVER = "driver";  // absolute path of the driver .so, or absent

    private static final int PICK_DRIVER = 1;
    private static final int PICK_IMAGE = 2;
    // FEATURE_VULKAN_HARDWARE_VERSION encodes versions like VK_MAKE_VERSION.
    private static final int VULKAN_1_1 = (1 << 22) | (1 << 12);
    // Files the retail image holds, for the extraction's progress.
    private static final int IMAGE_FILES = 832;

    private static final int BACKGROUND = Color.rgb(18, 18, 22);
    private static final int NOTE = Color.rgb(150, 150, 160);
    private static final int CLASH = Color.rgb(0xd0, 0x30, 0x30);

    static {
        System.loadLibrary("xerenge_installer");
    }

    /** Checks and extracts the image; returns null or what went wrong. */
    private static native String nativeInstall(int fd, String destination, boolean verify, Progress progress);

    /** Called from the native side, on the worker thread. */
    public interface Progress {
        void onProgress(int phase, long done, long total);
    }

    private final Handler ui = new Handler(Looper.getMainLooper());

    // The install tab.
    private TextView imageField;
    private Button imageBrowse;
    private Uri imageUri;
    // No "Windowed" or "Xenia render" here: a phone runs full screen, and on plume.
    private CheckBox bloomBox, blurBox, debugBox;
    private Spinner languageSpinner;
    private CheckBox asyncBox, earlySubmitBox, packedVerticesBox, cullingBox, reflectionSplitBox, fpsBox, fps30Box,
            frameGenBox;
    private Spinner resolutionSpinner, anisotropySpinner;
    private CheckBox aspectBox;
    private TextView gpuStatus;
    private Button driverButton, systemDriverButton;
    // The network tab.
    private EditText gamertagEdit, serverEdit;
    private CheckBox onlineBox;
    private TextView serverNote;
    private boolean onlineSaved;
    // The controls tab.
    private CheckBox touchBox;
    private SeekBar opacityBar;
    private TextView opacityLabel;
    // Below the tabs.
    private ProgressBar progress;
    private TextView status;
    private Button installButton, startButton;

    private boolean busy;
    private boolean loadedSettings;
    private Settings settings = new Settings();

    // --- when the game starts at once ------------------------------------

    private boolean vulkanIsEnough() {
        return getPackageManager().hasSystemFeature(PackageManager.FEATURE_VULKAN_HARDWARE_VERSION, VULKAN_1_1);
    }

    private String installedDriver() {
        String path = getSharedPreferences(PREFS, MODE_PRIVATE).getString(PREF_DRIVER, null);
        return path != null && new File(path).isFile() ? path : null;
    }

    private boolean driverOk() {
        return vulkanIsEnough() || installedDriver() != null;
    }

    private boolean ready() {
        return Settings.installed(this) && driverOk();
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
        refreshInstallState();
    }

    private void startGame() {
        startActivity(new Intent(this, BurnoutActivity.class));
        finish();
    }

    // --- building blocks ---------------------------------------------------

    private int dp(int value) {
        return Math.round(value * getResources().getDisplayMetrics().density);
    }

    private TextView label(LinearLayout parent, CharSequence text, float size, boolean bold, int color) {
        TextView view = new TextView(this);
        view.setText(text);
        view.setTextSize(size);
        view.setTextColor(color);
        if (bold) {
            view.setTypeface(Typeface.DEFAULT_BOLD);
        }
        view.setPadding(0, dp(4), 0, dp(4));
        parent.addView(view);
        return view;
    }

    /** The desktop tooltip, as a line under its control. */
    private void note(LinearLayout parent, int text) {
        TextView view = label(parent, getString(text), 12, false, NOTE);
        view.setPadding(dp(32), 0, 0, dp(6));
    }

    private CheckBox check(LinearLayout parent, int text, int tooltip) {
        CheckBox box = new CheckBox(this);
        box.setText(text);
        box.setTextColor(Color.WHITE);
        box.setButtonTintList(ColorStateList.valueOf(Color.WHITE));
        parent.addView(box);
        if (tooltip != 0) {
            note(parent, tooltip);
        }
        return box;
    }

    private Button button(LinearLayout parent, CharSequence text, View.OnClickListener action) {
        Button view = new Button(this);
        view.setText(text);
        view.setAllCaps(false);
        view.setOnClickListener(action);
        parent.addView(view, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT));
        return view;
    }

    private LinearLayout row(LinearLayout parent) {
        LinearLayout row = new LinearLayout(this);
        row.setOrientation(LinearLayout.HORIZONTAL);
        row.setGravity(Gravity.CENTER_VERTICAL);
        parent.addView(row);
        return row;
    }

    private Spinner spinner(LinearLayout parent, String[][] choices, String first) {
        List<String> labels = new ArrayList<>();
        for (String[] choice : choices) {
            labels.add(choice[0] != null ? choice[0] : first);
        }
        Spinner spinner = new Spinner(this);
        ArrayAdapter<String> adapter = new ArrayAdapter<String>(this, android.R.layout.simple_spinner_item, labels) {
            @Override
            public View getView(int position, View convert, ViewGroup group) {
                TextView view = (TextView) super.getView(position, convert, group);
                view.setTextColor(Color.WHITE);
                return view;
            }
        };
        adapter.setDropDownViewResource(android.R.layout.simple_spinner_dropdown_item);
        spinner.setAdapter(adapter);
        parent.addView(spinner, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        return spinner;
    }

    private static int indexOf(String[][] choices, String value) {
        for (int i = 0; i < choices.length; ++i) {
            if (choices[i][1].equals(value)) {
                return i;
            }
        }
        return -1;
    }

    private EditText edit(LinearLayout parent, String hint, String allowed, int max) {
        EditText edit = new EditText(this);
        edit.setSingleLine(true);
        edit.setTextColor(Color.WHITE);
        edit.setHintTextColor(NOTE);
        edit.setHint(hint);
        edit.setInputType(InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS);
        List<InputFilter> filters = new ArrayList<>();
        if (max > 0) {
            filters.add(new InputFilter.LengthFilter(max));
        }
        filters.add((source, start, end, dest, dstart, dend) -> {
            StringBuilder kept = new StringBuilder();
            for (int i = start; i < end; ++i) {
                if (allowed.indexOf(source.charAt(i)) >= 0) {
                    kept.append(source.charAt(i));
                }
            }
            return kept.length() == end - start ? null : kept.toString();
        });
        edit.setFilters(filters.toArray(new InputFilter[0]));
        parent.addView(edit, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        return edit;
    }

    private static final String NAME_CHARS =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_-";

    private LinearLayout page() {
        LinearLayout page = new LinearLayout(this);
        page.setOrientation(LinearLayout.VERTICAL);
        page.setPadding(dp(4), dp(8), dp(4), dp(8));
        return page;
    }

    private ScrollView scroll(LinearLayout page) {
        ScrollView scroll = new ScrollView(this);
        scroll.addView(page);
        return scroll;
    }

    // --- the screen ----------------------------------------------------------

    private void buildUi() {
        LinearLayout outer = new LinearLayout(this);
        outer.setOrientation(LinearLayout.VERTICAL);
        outer.setBackgroundColor(BACKGROUND);
        outer.setPadding(dp(16), dp(12), dp(16), dp(12));
        label(outer, getString(R.string.window_title), 20, true, Color.WHITE);

        // Tabs: Install, Network, Controls - the desktop order.
        LinearLayout tabRow = row(outer);
        FrameLayout pages = new FrameLayout(this);
        outer.addView(pages, new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1));
        View[] tabPages = {scroll(buildInstallPage()), scroll(buildNetworkPage()), scroll(buildControlsPage())};
        int[] tabNames = {R.string.tab_install, R.string.tab_network, R.string.tab_controls};
        Button[] tabButtons = new Button[tabPages.length];
        for (int i = 0; i < tabPages.length; ++i) {
            pages.addView(tabPages[i]);
            Button tab = new Button(this);
            tab.setText(tabNames[i]);
            tab.setAllCaps(false);
            final int index = i;
            tab.setOnClickListener(v -> {
                for (int j = 0; j < tabPages.length; ++j) {
                    tabPages[j].setVisibility(j == index ? View.VISIBLE : View.GONE);
                    tabButtons[j].setAlpha(j == index ? 1.0f : 0.55f);
                }
            });
            tabButtons[i] = tab;
            tabRow.addView(tab, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        }
        tabButtons[0].performClick();

        progress = new ProgressBar(this, null, android.R.attr.progressBarStyleHorizontal);
        progress.setMax(1000);
        outer.addView(progress);
        status = label(outer, getString(R.string.ready), 14, false, Color.WHITE);
        status.setGravity(Gravity.CENTER);
        installButton = button(outer, getString(R.string.install), v -> startInstall());
        startButton = button(outer, getString(R.string.start_game), v -> startGame());
        setContentView(outer);
    }

    private LinearLayout buildInstallPage() {
        LinearLayout page = page();

        // The disc image.
        label(page, getString(R.string.select_iso), 15, false, Color.WHITE);
        LinearLayout imageRow = row(page);
        imageField = new TextView(this);
        imageField.setHint(R.string.iso_placeholder);
        imageField.setHintTextColor(NOTE);
        imageField.setTextColor(Color.WHITE);
        imageRow.addView(imageField, new LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1));
        imageBrowse = new Button(this);
        imageBrowse.setText("...");
        imageBrowse.setOnClickListener(v -> pick(PICK_IMAGE, "*/*"));
        imageRow.addView(imageBrowse);

        // Off unless ticked.
        bloomBox = check(page, R.string.bloom, R.string.bloom_tip);
        blurBox = check(page, R.string.blur, R.string.blur_tip);
        debugBox = check(page, R.string.debug, R.string.debug_tip);

        // The game asks for its language at every start unless one is set here.
        LinearLayout languageRow = row(page);
        label(languageRow, getString(R.string.language), 15, false, Color.WHITE).setPadding(0, 0, dp(12), 0);
        languageSpinner = spinner(languageRow, Settings.LANGUAGES, getString(R.string.language_ask));

        // Hacks: folded away, each trading something for speed on a slow machine.
        Button hacksToggle = new Button(this);
        hacksToggle.setAllCaps(false);
        hacksToggle.setText("▸ " + getString(R.string.hacks));
        hacksToggle.setBackgroundColor(Color.TRANSPARENT);
        hacksToggle.setTextColor(Color.WHITE);
        hacksToggle.setGravity(Gravity.START | Gravity.CENTER_VERTICAL);
        page.addView(hacksToggle);
        LinearLayout hacks = page();
        hacks.setPadding(dp(18), 0, 0, 0);
        asyncBox = check(hacks, R.string.async, R.string.async_tip);
        earlySubmitBox = check(hacks, R.string.early_submit, R.string.early_submit_tip);
        packedVerticesBox = check(hacks, R.string.packed_vertices, R.string.packed_vertices_tip);
        cullingBox = check(hacks, R.string.culling, R.string.culling_tip);
        reflectionSplitBox = check(hacks, R.string.reflection_split, R.string.reflection_split_tip);
        LinearLayout resolutionRow = row(hacks);
        label(resolutionRow, getString(R.string.render_resolution), 15, false, Color.WHITE).setPadding(0, 0, dp(12), 0);
        resolutionSpinner = spinner(resolutionRow, Settings.RESOLUTIONS, getString(R.string.resolution_window));
        note(hacks, R.string.render_resolution_tip);
        LinearLayout anisotropyRow = row(hacks);
        label(anisotropyRow, getString(R.string.anisotropy), 15, false, Color.WHITE).setPadding(0, 0, dp(12), 0);
        anisotropySpinner = spinner(anisotropyRow, Settings.ANISOTROPY, getString(R.string.anisotropy_off));
        note(hacks, R.string.anisotropy_tip);
        aspectBox = check(hacks, R.string.aspect_16_9, R.string.aspect_16_9_tip);
        fpsBox = check(hacks, R.string.fps_counter, R.string.fps_counter_tip);
        fps30Box = check(hacks, R.string.fps_30, R.string.fps_30_tip);
        frameGenBox = check(hacks, R.string.frame_generation, R.string.frame_generation_tip);
        hacks.setVisibility(View.GONE);
        page.addView(hacks);
        hacksToggle.setOnClickListener(v -> {
            boolean open = hacks.getVisibility() != View.VISIBLE;
            hacks.setVisibility(open ? View.VISIBLE : View.GONE);
            hacksToggle.setText((open ? "▾ " : "▸ ") + getString(R.string.hacks));
        });

        // Where it goes: fixed on Android, the app's own directory.
        label(page, getString(R.string.install_path), 15, false, Color.WHITE);
        label(page, Settings.root(this).getAbsolutePath(), 13, false, NOTE);

        // The graphics driver.
        label(page, getString(R.string.section_gpu), 16, true, Color.WHITE);
        gpuStatus = label(page, "", 14, false, Color.WHITE);
        driverButton = button(page, getString(R.string.pick_driver), v -> pick(PICK_DRIVER, "application/zip"));
        systemDriverButton = button(page, getString(R.string.use_system_driver), v -> {
            getSharedPreferences(PREFS, MODE_PRIVATE).edit().remove(PREF_DRIVER).apply();
            refreshInstallState();
        });

        showSettings(settings);
        return page;
    }

    private LinearLayout buildNetworkPage() {
        LinearLayout page = page();
        LinearLayout gamertagRow = row(page);
        label(gamertagRow, getString(R.string.gamertag), 15, false, Color.WHITE).setPadding(0, 0, dp(12), 0);
        gamertagEdit = edit(gamertagRow, getString(R.string.gamertag_placeholder), NAME_CHARS, 15);
        note(page, R.string.gamertag_tip);

        onlineBox = check(page, R.string.local_multiplayer, R.string.local_multiplayer_tip);

        LinearLayout serverRow = row(page);
        label(serverRow, getString(R.string.server_address), 15, false, Color.WHITE).setPadding(0, 0, dp(12), 0);
        serverEdit = edit(serverRow, getString(R.string.server_placeholder), NAME_CHARS + ".", 0);
        note(page, R.string.server_tip);

        serverNote = label(page, "", 14, false, Color.WHITE);
        // A server address replaces local multiplayer: the box is off and not to be ticked while it is set.
        serverEdit.addTextChangedListener(new TextWatcher() {
            @Override public void beforeTextChanged(CharSequence s, int a, int b, int c) {}
            @Override public void onTextChanged(CharSequence s, int a, int b, int c) {}
            @Override public void afterTextChanged(Editable s) { syncNetwork(); }
        });
        showNetwork(settings);
        return page;
    }

    private void syncNetwork() {
        String server = serverEdit.getText().toString().trim();
        boolean hasServer = !server.isEmpty();
        if (hasServer && onlineBox.isEnabled()) {
            onlineSaved = onlineBox.isChecked();
            onlineBox.setChecked(false);
        } else if (!hasServer && !onlineBox.isEnabled()) {
            onlineBox.setChecked(onlineSaved);
        }
        onlineBox.setEnabled(!hasServer);
        serverNote.setText(hasServer ? getString(R.string.server_note, server) : getString(R.string.local_note));
    }

    private LinearLayout buildControlsPage() {
        LinearLayout page = page();
        touchBox = check(page, R.string.touch_controls, R.string.touch_controls_tip);
        opacityLabel = label(page, "", 15, false, Color.WHITE);
        opacityBar = new SeekBar(this);
        opacityBar.setMax(90);  // 10..100 percent
        opacityBar.setOnSeekBarChangeListener(new SeekBar.OnSeekBarChangeListener() {
            @Override public void onProgressChanged(SeekBar bar, int value, boolean user) {
                opacityLabel.setText(getString(R.string.touch_opacity, value + 10));
            }
            @Override public void onStartTrackingTouch(SeekBar bar) {}
            @Override public void onStopTrackingTouch(SeekBar bar) {}
        });
        page.addView(opacityBar);
        note(page, R.string.touch_opacity_tip);
        touchBox.setOnCheckedChangeListener((b, on) -> opacityBar.setEnabled(on));
        showControls(settings);
        return page;
    }

    // --- settings in and out of the controls ----------------------------------

    private void showSettings(Settings s) {
        bloomBox.setChecked(s.bloom);
        blurBox.setChecked(s.motionBlur);
        debugBox.setChecked(s.debug);
        int language = indexOf(Settings.LANGUAGES, s.language);
        languageSpinner.setSelection(language >= 0 ? language : 1);
        asyncBox.setChecked(s.asyncPresent);
        earlySubmitBox.setChecked(s.earlySubmit);
        packedVerticesBox.setChecked(s.packedVertices);
        cullingBox.setChecked(s.culling);
        reflectionSplitBox.setChecked(s.reflectionSplit);
        int resolution = indexOf(Settings.RESOLUTIONS, s.renderResolution);
        resolutionSpinner.setSelection(Math.max(resolution, 0));
        int anisotropy = indexOf(Settings.ANISOTROPY, s.anisotropy);
        anisotropySpinner.setSelection(anisotropy >= 0 ? anisotropy : 1);
        aspectBox.setChecked(s.aspect169);
        fpsBox.setChecked(s.fpsCounter);
        fps30Box.setChecked(s.fps30);
        frameGenBox.setChecked(s.frameGeneration);
    }

    private void showNetwork(Settings s) {
        gamertagEdit.setText(s.gamertag);
        onlineBox.setEnabled(true);
        onlineBox.setChecked(s.online);
        onlineSaved = s.online;
        serverEdit.setText(s.lobbyServer);
        syncNetwork();
    }

    private void showControls(Settings s) {
        touchBox.setChecked(s.touchControls);
        opacityBar.setProgress(Math.max(0, Math.min(90, s.touchOpacity - 10)));
        opacityLabel.setText(getString(R.string.touch_opacity, opacityBar.getProgress() + 10));
        opacityBar.setEnabled(s.touchControls);
    }

    private Settings collectSettings() {
        Settings s = new Settings();
        s.preservedDebug = settings.preservedDebug;
        s.bloom = bloomBox.isChecked();
        s.motionBlur = blurBox.isChecked();
        s.xenia = false;
        s.windowed = false;
        s.debug = debugBox.isChecked();
        s.language = Settings.LANGUAGES[languageSpinner.getSelectedItemPosition()][1];
        s.gamertag = gamertagEdit.getText().toString().trim();
        String server = serverEdit.getText().toString().trim();
        s.lobbyServer = server;
        s.online = server.isEmpty() ? onlineBox.isChecked() : onlineSaved;
        s.asyncPresent = asyncBox.isChecked();
        s.earlySubmit = earlySubmitBox.isChecked();
        s.packedVertices = packedVerticesBox.isChecked();
        s.culling = cullingBox.isChecked();
        s.reflectionSplit = reflectionSplitBox.isChecked();
        s.renderResolution = Settings.RESOLUTIONS[resolutionSpinner.getSelectedItemPosition()][1];
        s.anisotropy = Settings.ANISOTROPY[anisotropySpinner.getSelectedItemPosition()][1];
        s.aspect169 = aspectBox.isChecked();
        s.fpsCounter = fpsBox.isChecked();
        s.fps30 = fps30Box.isChecked();
        s.frameGeneration = frameGenBox.isChecked();
        s.touchControls = touchBox.isChecked();
        s.touchOpacity = opacityBar.getProgress() + 10;
        return s;
    }

    // --- state ---------------------------------------------------------------

    private void refreshInstallState() {
        boolean installed = Settings.installed(this);
        if (installed && !loadedSettings) {
            // The settings of the installed copy, back into the controls, so an
            // update writes them out again as they were unless changed here.
            loadedSettings = true;
            settings = Settings.load(this);
            showSettings(settings);
            showNetwork(settings);
            showControls(settings);
        }
        installButton.setText(installed ? R.string.update : R.string.install);
        imageField.setEnabled(!installed && !busy);
        imageBrowse.setEnabled(!installed && !busy);
        if (!busy) {
            status.setText(installed ? R.string.installed_here : R.string.ready);
        }
        String driver = installedDriver();
        if (driver != null) {
            gpuStatus.setText(getString(R.string.gpu_custom, new File(driver).getName()));
        } else if (vulkanIsEnough()) {
            gpuStatus.setText(R.string.gpu_ok);
        } else {
            gpuStatus.setText(R.string.gpu_too_old);
        }
        systemDriverButton.setVisibility(driver != null ? View.VISIBLE : View.GONE);
        driverButton.setEnabled(!busy);
        systemDriverButton.setEnabled(!busy);
        installButton.setEnabled(!busy);
        startButton.setVisibility(installed ? View.VISIBLE : View.GONE);
        startButton.setEnabled(!busy && ready());
    }

    private void pick(int request, String type) {
        Intent intent = new Intent(Intent.ACTION_OPEN_DOCUMENT);
        intent.addCategory(Intent.CATEGORY_OPENABLE);
        intent.setType(type);
        startActivityForResult(intent, request);
    }

    private String displayName(Uri uri) {
        try (Cursor cursor = getContentResolver().query(uri, new String[] {OpenableColumns.DISPLAY_NAME},
                null, null, null)) {
            if (cursor != null && cursor.moveToFirst()) {
                return cursor.getString(0);
            }
        } catch (Exception ignored) {
        }
        return uri.getLastPathSegment();
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        if (result != RESULT_OK || data == null || data.getData() == null) {
            return;
        }
        if (request == PICK_DRIVER) {
            installDriver(data.getData());
        } else if (request == PICK_IMAGE) {
            imageUri = data.getData();
            imageField.setText(displayName(imageUri));
        }
    }

    // --- the driver ------------------------------------------------------------

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
            status.setText(getString(R.string.driver_failed, String.valueOf(error.getMessage())));
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
            status.setText(R.string.driver_not_a_package);
            return;
        }
        getSharedPreferences(PREFS, MODE_PRIVATE).edit().putString(PREF_DRIVER, driver.getAbsolutePath()).apply();
        refreshInstallState();
        status.setText(getString(R.string.driver_installed, driver.getName()));
    }

    // --- installing: the desktop steps, less the program copy and shortcuts ------

    private void setBusy(boolean value) {
        busy = value;
        for (View view : new View[] {bloomBox, blurBox, installButton}) {
            view.setEnabled(!value);
        }
        refreshInstallState();
    }

    private void startInstall() {
        final Settings chosen = collectSettings();
        final File game = Settings.gameDir(this);
        final boolean extracted = new File(game, "default.xex").isFile();
        if (!extracted && imageUri == null) {
            status.setText(R.string.select_image_first);
            return;
        }
        final ParcelFileDescriptor descriptor;
        if (!extracted) {
            try {
                descriptor = getContentResolver().openFileDescriptor(imageUri, "r");
            } catch (Exception error) {
                status.setText(getString(R.string.step_failed, getString(R.string.step_extract),
                        String.valueOf(error.getMessage())));
                return;
            }
        } else {
            descriptor = null;
        }
        // Step weights as on the desktop: extraction 30, the settings 1.
        final int extractWeight = extracted ? 0 : 30;
        final int total = extractWeight + 1;
        progress.setProgress(0);
        setBusy(true);
        new Thread(() -> {
            if (!extracted) {
                post(getString(R.string.step_extract) + "...", 0);
                deleteTree(game);
                game.mkdirs();
                final long[] last = {0};
                String error = nativeInstall(descriptor.getFd(), game.getAbsolutePath(), true,
                        (phase, done, all) -> {
                            long now = System.currentTimeMillis();
                            if (now - last[0] < 200) {
                                return;
                            }
                            last[0] = now;
                            double step;
                            String text;
                            if (phase == 0) {
                                int percent = all > 0 ? (int) (100 * done / all) : 0;
                                step = 0.5 * percent / 100.0;
                                text = getString(R.string.checking, percent);
                            } else {
                                step = 0.5 + 0.5 * Math.min(done, IMAGE_FILES) / (double) IMAGE_FILES;
                                text = getString(R.string.extracting, (int) done, IMAGE_FILES);
                            }
                            post(text, (int) (1000 * extractWeight * step / total));
                        });
                try {
                    descriptor.close();
                } catch (Exception ignored) {
                }
                if (error != null) {
                    deleteTree(game);
                    finishInstall(getString(R.string.step_failed, getString(R.string.step_extract), error));
                    return;
                }
            }
            post(getString(R.string.step_settings) + "...", 1000 * extractWeight / total);
            if (!chosen.save(this)) {
                finishInstall(getString(R.string.step_failed, getString(R.string.step_settings),
                        getString(R.string.cannot_write, Settings.file(this).getAbsolutePath())));
                return;
            }
            ui.post(() -> {
                progress.setProgress(1000);
                settings = Settings.load(this);
            });
            finishInstall(null);
        }, "installer").start();
    }

    private void post(String text, int value) {
        ui.post(() -> {
            status.setText(text);
            progress.setProgress(value);
        });
    }

    private void finishInstall(String error) {
        ui.post(() -> {
            setBusy(false);
            status.setText(error != null ? error
                    : getString(driverOk() ? R.string.installed : R.string.installed_needs_driver));
        });
    }
}
