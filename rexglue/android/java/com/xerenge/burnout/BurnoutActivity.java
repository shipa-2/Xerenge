package com.xerenge.burnout;

import android.os.Bundle;
import android.system.Os;

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
