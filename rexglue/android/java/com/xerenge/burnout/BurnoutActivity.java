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
 */
public class BurnoutActivity extends SDLActivity {
    private Settings settings;

    @Override
    protected void onCreate(Bundle state) {
        // Before SDL loads the libraries: the game reads the environment when
        // it starts, the renderer when it creates its Vulkan instance.
        settings = Settings.load(this);
        settings.applyEnvironment();
        String driver = getSharedPreferences(InstallerActivity.PREFS, MODE_PRIVATE)
                .getString(InstallerActivity.PREF_DRIVER, null);
        try {
            if (driver != null && new File(driver).isFile()) {
                Os.setenv("XERENGE_VULKAN_DRIVER", driver, true);
                Os.setenv("XERENGE_NATIVE_LIB_DIR", getApplicationInfo().nativeLibraryDir, true);
            } else {
                Os.unsetenv("XERENGE_VULKAN_DRIVER");
            }
        } catch (Exception ignored) {
        }
        super.onCreate(state);
        // The on-screen pad, over SDL's surface.
        if (settings.touchControls && mLayout != null) {
            mLayout.addView(new TouchControls(this, settings.touchOpacity),
                    new android.view.ViewGroup.LayoutParams(android.view.ViewGroup.LayoutParams.MATCH_PARENT,
                            android.view.ViewGroup.LayoutParams.MATCH_PARENT));
        }
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
