package com.xerenge.burnout;

import java.io.File;
import java.util.ArrayList;

import org.libsdl.app.SDLActivity;

/**
 * The game: SDL's activity with the recompiled title in libmain.so.
 *
 * Everything lives in the app's own external files directory
 * (/sdcard/Android/data/com.xerenge.burnout/files), which needs no storage
 * permission and is reachable over adb:
 *   game/        the extracted disc (default.xex and the rest)
 *   user/        saves and settings
 *   burnout.log  the log of the last run
 */
public class BurnoutActivity extends SDLActivity {
    @Override
    protected String[] getLibraries() {
        // SDL itself is linked into librexruntime.so, which libmain.so needs;
        // libc++_shared.so first, as both of them do.
        return new String[] { "c++_shared", "rexruntime", "main" };
    }

    @Override
    protected String[] getArguments() {
        File root = getExternalFilesDir(null);
        ArrayList<String> args = new ArrayList<>();
        args.add("--game_data_root=" + new File(root, "game").getAbsolutePath());
        args.add("--user_data_root=" + new File(root, "user").getAbsolutePath());
        args.add("--log_file=" + new File(root, "burnout.log").getAbsolutePath());
        args.add("--log_level=info");
        args.add("--gpu_plugin=plume");
        args.add("--gpu_backend=plume");
        args.add("--fullscreen");
        return args.toArray(new String[0]);
    }
}
