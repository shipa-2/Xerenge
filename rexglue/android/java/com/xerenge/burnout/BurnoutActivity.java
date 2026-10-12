package com.xerenge.burnout;

import android.os.Bundle;
import android.os.Process;
import android.os.SystemClock;
import android.system.Os;
import android.widget.Toast;

import java.io.File;

import org.libsdl.app.SDLActivity;

/**
 * The game: SDL's activity with the recompiled title in libmain.so, started
 * the way the desktop launcher starts it - from xerenge.conf (Settings), which
 * the installer writes beside game/ in the app's external files directory
 * (/sdcard/Android/data/com.xerenge.burnout/files, reachable over adb):
 *   game/        the extracted disc
 *   user/        saves
 *   xerenge.conf the settings
 *   burnout.log  the log (with .1 and .2 before it)
 * and copies the logs into a folder the player picked, if any (LogExport).
 */
public class BurnoutActivity extends SDLActivity {
    private Settings settings;

    @Override
    protected void onCreate(Bundle state) {
        // Before SDL loads the libraries: the game reads the environment when
        // it starts, the renderer when it creates its Vulkan instance.
        settings = Settings.load(this);
        settings.applyEnvironment();
        applyDebugProperty();
        // The last run's logs into the player's folder, if one is picked: the
        // game appends, so a run that crashed or hung is in them (LogExport).
        LogExport.exportAsync(this);
        String driver = getSharedPreferences(InstallerActivity.PREFS, MODE_PRIVATE)
                .getString(InstallerActivity.PREF_DRIVER, null);
        try {
            // Where the in-game menu writes the settings it changes back to.
            Os.setenv("XERENGE_CONF", Settings.file(this).getAbsolutePath(), true);
            if (driver != null && new File(driver).isFile()) {
                Os.setenv("XERENGE_VULKAN_DRIVER", driver, true);
                Os.setenv("XERENGE_NATIVE_LIB_DIR", getApplicationInfo().nativeLibraryDir, true);
            } else {
                Os.unsetenv("XERENGE_VULKAN_DRIVER");
            }
        } catch (Exception ignored) {
        }
        super.onCreate(state);
        // The on-screen pad, over SDL's surface, with a menu button at its top right:
        // the game's settings menu (plume_menu.cpp), and the layout editor (drag
        // and resize every control; kept in touch_layout.conf). The pad and the
        // editor are from PredatorWoah's XerengePlus fork.
        if (settings.touchControls && mLayout != null) {
            final TouchControls[] pad = new TouchControls[1];
            pad[0] = new TouchControls(this, settings.touchOpacity, true, () ->
                    new android.app.AlertDialog.Builder(this)
                            .setItems(new String[] {getString(R.string.game_settings), getString(R.string.edit_layout)},
                                    (d, which) -> {
                                        if (which == 0) {
                                            TouchControls.nativeMenuToggle();
                                        } else {
                                            pad[0].beginEdit();
                                        }
                                    })
                            .setNegativeButton(R.string.close, null)
                            .show());
            mLayout.addView(pad[0],
                    new android.view.ViewGroup.LayoutParams(android.view.ViewGroup.LayoutParams.MATCH_PARENT,
                            android.view.ViewGroup.LayoutParams.MATCH_PARENT));
        }
    }

    // Variables for trying things out from a computer, without writing into
    // the app's folder (a file pushed there over adb is the shell's, and the
    // app loses it): adb shell setprop debug.xerenge.env "NAME=value;NAME2=value".
    // debug.* properties are the shell's to set and anyone's to read.
    private static void applyDebugProperty() {
        String value = "";
        try {
            java.lang.Process p = Runtime.getRuntime().exec(new String[] {"getprop", "debug.xerenge.env"});
            try (java.io.BufferedReader in = new java.io.BufferedReader(new java.io.InputStreamReader(p.getInputStream()))) {
                String line = in.readLine();
                value = line == null ? "" : line.trim();
            }
        } catch (Exception ignored) {
        }
        for (String pair : value.split(";")) {
            int equals = pair.indexOf('=');
            if (equals > 0) {
                try {
                    Os.setenv(pair.substring(0, equals).trim(), pair.substring(equals + 1).trim(), true);
                    android.util.Log.i("xerenge", "debug.xerenge.env: " + pair.trim());
                } catch (Exception ignored) {
                }
            }
        }
    }

    // And this run's, whenever it leaves the screen: the moment a player goes
    // to report something, or swipes a hung game away.
    @Override
    protected void onStop() {
        super.onStop();
        LogExport.exportAsync(this);
    }

    // Always landscape, either way up: SDL would otherwise allow any
    // orientation for a resizable window, and the game started in portrait.
    @Override
    public void setOrientationBis(int w, int h, boolean resizable, String hint) {
        setRequestedOrientation(android.content.pm.ActivityInfo.SCREEN_ORIENTATION_SENSOR_LANDSCAPE);
    }

    // Back (the gesture or the button) leaves the game only when given twice:
    // the first press says so, a second within EXIT_WINDOW_MS closes it for
    // good - not to the background, where it would keep its memory and its
    // sound, but out of the recent apps with the process gone.
    private static final long EXIT_WINDOW_MS = 2000;
    private long lastBackPress;
    private Toast exitToast;

    @Override
    public void onBackPressed() {
        long now = SystemClock.uptimeMillis();
        if (lastBackPress != 0 && now - lastBackPress < EXIT_WINDOW_MS) {
            if (exitToast != null) {
                exitToast.cancel();
            }
            finishAndRemoveTask();
            Process.killProcess(Process.myPid());
            return;
        }
        lastBackPress = now;
        if (exitToast != null) {
            exitToast.cancel();
        }
        exitToast = Toast.makeText(this, R.string.press_back_to_exit, Toast.LENGTH_SHORT);
        exitToast.show();
    }

    @Override
    protected String[] getLibraries() {
        // SDL itself is linked into librexruntime.so, which libmain.so needs;
        // libc++_shared.so first, as both of them do.
        return new String[] { "c++_shared", "rexruntime", "main" };
    }

    @Override
    protected String[] getArguments() {
        if (settings == null) {
            settings = Settings.load(this);
        }
        return settings.arguments(this).toArray(new String[0]);
    }
}
