package com.xerenge.burnout;

import android.content.Context;
import android.system.Os;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStreamReader;
import java.io.OutputStreamWriter;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;

/**
 * xerenge.conf, as the desktop installer writes it and its launcher reads it
 * (rexglue/installer/installer_window.cpp): the same keys, the same defaults,
 * the same meaning. It lives in the app's external files directory beside game/
 * and can be edited there by hand; the game reads it at every start.
 */
final class Settings {
    /** Language choices: label (null: ask at every start) and value. */
    static final String[][] LANGUAGES = {
        {null, ""},
        {"English", "0"},
        {"English (US)", "1"},
        {"Español", "5"},
        {"Nederlands", "10"},
        {"Svenska", "11"},
        {"Suomi", "12"},
    };

    /** Render resolutions: label (null: the device's screen) and value. */
    static final String[][] RESOLUTIONS = {
        {null, ""},
        {"1440p", "2560x1440"},
        {"1080p", "1920x1080"},
        {"720p", "1280x720"},
        {"540p", "960x540"},
        {"480p", "854x480"},
        {"360p", "640x360"},
    };

    /** Anisotropic filtering at most: label (null: off) and value. */
    static final String[][] ANISOTROPY = {
        {null, "off"},
        {"4x", "4"},
        {"8x", "8"},
    };

    static final String DEFAULT_SERVER = "94639.snk.wtf";

    // What the installer's controls hold.
    boolean bloom, motionBlur, xenia, windowed, debug;
    String language = "0";  // English, as the desktop installer preselects
    String gamertag = "";
    boolean online;
    String lobbyServer = DEFAULT_SERVER;
    boolean asyncPresent, earlySubmit = true, packedVertices = true, culling, fpsCounter;
    boolean fps30;  // two logic steps and two vblanks a frame: an even 30
    boolean frameGeneration;  // frames drawn between the game's, for a 120 Hz screen
    boolean reflectionSplit;  // two of the reflection cube's faces a frame
    String renderResolution = "";
    String anisotropy = "4";  // the title asks 8 of the road; 4 costs a phone less
    boolean aspect169 = true;  // Android: keep the frame 16:9, black bars on a wider screen
    // On-screen controls (Android only; the desktop has the keyboard instead).
    boolean touchControls = true;
    int touchOpacity = 40;  // percent
    /** The debug_* lines of an installed copy, kept as they are. */
    String preservedDebug = "";

    /** Every line of the file as read, for the launcher side. */
    final Map<String, String> values = new LinkedHashMap<>();

    static File root(Context context) {
        return context.getExternalFilesDir(null);
    }

    static File file(Context context) {
        return new File(root(context), "xerenge.conf");
    }

    static File gameDir(Context context) {
        return new File(root(context), "game");
    }

    static boolean installed(Context context) {
        return new File(gameDir(context), "default.xex").isFile() && file(context).isFile();
    }

    static Settings load(Context context) {
        Settings settings = new Settings();
        File file = file(context);
        if (!file.isFile()) {
            return settings;
        }
        StringBuilder debugLines = new StringBuilder();
        try (BufferedReader in = new BufferedReader(
                new InputStreamReader(new FileInputStream(file), StandardCharsets.UTF_8))) {
            String line;
            while ((line = in.readLine()) != null) {
                line = line.trim();
                if (line.isEmpty() || line.startsWith("#")) {
                    continue;
                }
                int equals = line.indexOf('=');
                if (equals > 0) {
                    settings.values.put(line.substring(0, equals).trim(), line.substring(equals + 1).trim());
                    if (line.startsWith("debug_") || line.startsWith("env.")) {
                        debugLines.append(line).append('\n');
                    }
                }
            }
        } catch (Exception ignored) {
        }
        Map<String, String> v = settings.values;
        settings.preservedDebug = debugLines.toString();
        settings.bloom = flag(v, "bloom", settings.bloom);
        settings.motionBlur = flag(v, "motion_blur", settings.motionBlur);
        if (v.containsKey("renderer")) {
            settings.xenia = "xenos".equals(v.get("renderer"));
        }
        settings.windowed = flag(v, "windowed", settings.windowed);
        settings.debug = flag(v, "debug", settings.debug);
        if (v.containsKey("language")) {
            settings.language = v.get("language");
        }
        if (v.containsKey("gamertag")) {
            settings.gamertag = v.get("gamertag");
        }
        settings.online = flag(v, "online", settings.online);
        if (v.containsKey("lobby_server")) {
            settings.lobbyServer = v.get("lobby_server");
        }
        settings.asyncPresent = flag(v, "async_present", settings.asyncPresent);
        settings.earlySubmit = flag(v, "early_submit", settings.earlySubmit);
        settings.packedVertices = flag(v, "packed_vertices", settings.packedVertices);
        settings.culling = flag(v, "culling", settings.culling);
        settings.reflectionSplit = flag(v, "reflection_split", settings.reflectionSplit);
        if (v.containsKey("render_resolution")) {
            settings.renderResolution = v.get("render_resolution");
        }
        if (v.containsKey("anisotropy") && !v.get("anisotropy").isEmpty()) {
            settings.anisotropy = v.get("anisotropy");
        }
        settings.aspect169 = flag(v, "aspect_16_9", settings.aspect169);
        settings.fpsCounter = flag(v, "fps_counter", settings.fpsCounter);
        settings.fps30 = flag(v, "fps_30", settings.fps30);
        settings.frameGeneration = flag(v, "frame_generation", settings.frameGeneration);
        settings.touchControls = flag(v, "touch_controls", settings.touchControls);
        try {
            if (v.containsKey("touch_opacity")) {
                settings.touchOpacity = Integer.parseInt(v.get("touch_opacity"));
            }
        } catch (NumberFormatException ignored) {
        }
        return settings;
    }

    private static boolean flag(Map<String, String> values, String key, boolean fallback) {
        return values.containsKey(key) ? "true".equals(values.get(key)) : fallback;
    }

    private static String tf(boolean value) {
        return value ? "true" : "false";
    }

    /** The file, line for line as the desktop installer writes it. */
    boolean save(Context context) {
        StringBuilder out = new StringBuilder();
        out.append("# Burnout Revenge (Xerenge) settings, read by the launcher at every start.\n")
           .append("# true or false\n")
           .append("bloom = ").append(tf(bloom)).append('\n')
           .append("motion_blur = ").append(tf(motionBlur)).append('\n')
           .append("# plume (this project's renderer) or xenos (Xenia's)\n")
           .append("renderer = ").append(xenia ? "xenos" : "plume").append('\n')
           .append("# Fullscreen unless true\n")
           .append("windowed = ").append(tf(windowed)).append('\n')
           .append("# The name shown to others online (empty: Player)\n")
           .append("gamertag = ").append(gamertag.trim()).append('\n')
           .append("# Local multiplayer: the copies on one network find each other's games\n")
           .append("online = ").append(tf(online)).append('\n')
           .append("# A server of your own (host or IP); set, it is the only lobby and local multiplayer is off\n")
           .append("lobby_server = ").append(lobbyServer.trim()).append('\n')
           .append("# The language set ahead of time (empty: the game asks at every start)\n")
           .append("language = ").append(language).append('\n')
           .append("# Hacks\n")
           .append("async_present = ").append(tf(asyncPresent)).append('\n')
           .append("early_submit = ").append(tf(earlySubmit)).append('\n')
           .append("packed_vertices = ").append(tf(packedVertices)).append('\n')
           .append("culling = ").append(tf(culling)).append('\n')
           .append("reflection_split = ").append(tf(reflectionSplit)).append('\n')
           .append("render_resolution = ").append(renderResolution).append('\n')
           .append("# Anisotropic filtering at most: off, 4 or 8\n")
           .append("anisotropy = ").append(anisotropy).append('\n')
           .append("# Keep the frame 16:9 (black bars on a wider screen)\n")
           .append("aspect_16_9 = ").append(tf(aspect169)).append('\n')
           .append("fps_counter = ").append(tf(fpsCounter)).append('\n')
           .append("fps_30 = ").append(tf(fps30)).append('\n')
           .append("frame_generation = ").append(tf(frameGeneration)).append('\n')
           .append("# On-screen controls: translucent pad buttons over the game, and how opaque (percent)\n")
           .append("touch_controls = ").append(tf(touchControls)).append('\n')
           .append("touch_opacity = ").append(touchOpacity).append('\n');
        out.append("# Debug mode: detailed logs for reporting a problem. They go to\n")
           .append("# burnout.log beside this file.\n")
           .append("debug = ").append(tf(debug)).append('\n')
           .append("# What debug mode logs. Edited here only; an update keeps these lines.\n")
           .append("#   debug_log_level: trace, debug, info, warn, error\n")
           .append("#   debug_gpu_trace: the renderer's per-draw and per-frame diagnostics (large)\n")
           .append("#   debug_movie_trace: the video player's states\n")
           .append("#   debug_pipeline_log: every render pipeline built\n")
           .append("#   debug_vertex_trace: vertex attributes that come out as NaN or absurd, and then who writes those vertices (on unless false)\n")
           .append("#   debug_constant_trace: shader constants (object and bone matrices) that are NaN or absurd, and who writes them (off unless true)\n")
           .append("#   debug_video_gpu: 0 turns menu video into RGB on the CPU, 3 (the default) on the GPU; empty: the default\n")
           .append("#   debug_noisy: the per-frame log lines as well (very large)\n")
           .append("#   env.NAME = value: sets that environment variable for the game, debug mode or not\n")
           .append("#     (experiments, e.g. env.XERENGE_PLUME_TIMING = 1)\n");
        if (!preservedDebug.isEmpty()) {
            out.append(preservedDebug);
        } else {
            out.append("debug_log_level = debug\n")
               .append("debug_gpu_trace = true\n")
               .append("debug_movie_trace = false\n")
               .append("debug_pipeline_log = false\n")
               .append("debug_vertex_trace = true\n")
               .append("debug_constant_trace = false\n")
               .append("debug_video_gpu = \n")
               .append("debug_noisy = false\n");
        }
        File file = file(context);
        file.getParentFile().mkdirs();
        try (OutputStreamWriter writer = new OutputStreamWriter(new FileOutputStream(file), StandardCharsets.UTF_8)) {
            writer.write(out.toString());
            return true;
        } catch (Exception error) {
            return false;
        }
    }

    // --- the launcher: what the desktop launcher script does with the file ---

    private String value(String key) {
        String value = values.get(key);
        return value == null ? "" : value;
    }

    private static void env(String name, String value) {
        try {
            Os.setenv(name, value, true);
        } catch (Exception ignored) {
        }
    }

    /** Sets the environment the game reads; call before its libraries load. */
    void applyEnvironment() {
        if (!"true".equals(value("bloom"))) env("XERENGE_NO_BLOOM", "1");
        if (!"true".equals(value("motion_blur"))) env("XERENGE_NO_MOTION_BLUR", "1");
        {  // plume, the only renderer here
            for (String name : new String[] {"XERENGE_D3D_TARGETS", "XERENGE_D3D_UI", "XERENGE_SKIP_LOGOS",
                                             "XERENGE_REAL_SHADERS", "XERENGE_D3D_DRAWS", "XERENGE_SECONDARY_TICKS"}) {
                env(name, "1");
            }
        }
        if (!value("language").isEmpty()) env("XERENGE_LANGUAGE", value("language"));
        if ("true".equals(value("async_present"))) env("XERENGE_ASYNC_PRESENT", "1");
        if ("false".equals(value("early_submit"))) env("XERENGE_ACQUIRE_FIRST", "1");
        if ("false".equals(value("packed_vertices"))) env("XERENGE_FULL_VERTICES", "1");
        if ("true".equals(value("culling"))) env("XERENGE_CULL", "1");
        if ("true".equals(value("reflection_split"))) env("XERENGE_REFLECTION_SPLIT", "1");
        if (!value("render_resolution").isEmpty()) env("XERENGE_RENDER_RESOLUTION", value("render_resolution"));
        if ("off".equals(value("anisotropy"))) env("XERENGE_MAX_ANISOTROPY", "1");
        else if (!value("anisotropy").isEmpty()) env("XERENGE_MAX_ANISOTROPY", value("anisotropy"));
        if (!"false".equals(value("aspect_16_9"))) env("XERENGE_ASPECT_16_9", "1");
        if ("true".equals(value("fps_counter"))) env("XERENGE_FPS_SHOW", "1");
        if ("true".equals(value("fps_30"))) env("XERENGE_FPS_30", "1");
        if ("true".equals(value("frame_generation"))) env("XERENGE_FRAME_GENERATION", "120");
        if ("true".equals(value("debug"))) {
            // The renderer's own counting and timing, per draw: debug mode only.
            env("XERENGE_DEBUG", "1");
            if ("true".equals(value("debug_gpu_trace"))) env("XERENGE_GPU_TRACE", "1");
            if ("true".equals(value("debug_movie_trace"))) env("XERENGE_MOVIE_TRACE", "1");
            if ("true".equals(value("debug_pipeline_log"))) env("XERENGE_PIPELINE_LOG", "1");
            if (!"false".equals(value("debug_vertex_trace"))) env("XERENGE_VERTEX_TRACE", "1");
            if ("true".equals(value("debug_constant_trace"))) env("XERENGE_CONSTANT_TRACE", "1");
            if (!value("debug_video_gpu").isEmpty()) env("XERENGE_VIDEO_GPU", value("debug_video_gpu"));
        }
        // env.NAME = value lines: any variable the game or the renderer reads, for trying
        // things out on the phone without a new APK.
        for (Map.Entry<String, String> e : values.entrySet()) {
            if (e.getKey().startsWith("env.") && e.getKey().length() > 4) env(e.getKey().substring(4), e.getValue());
        }
    }

    /** The game's command line, as the desktop launcher builds it. */
    List<String> arguments(Context context) {
        File root = root(context);
        // Always plume: the Xenos plugin is not shipped on Android.
        String renderer = "plume";
        String level = "info";
        if ("true".equals(value("debug"))) {
            level = value("debug_log_level").isEmpty() ? "debug" : value("debug_log_level");
        }
        List<String> args = new ArrayList<>();
        args.add("--game_data_root=" + gameDir(context).getAbsolutePath());
        args.add("--user_data_root=" + new File(root, "user").getAbsolutePath());
        args.add("--gpu_plugin=" + renderer);
        args.add("--gpu_backend=" + renderer);
        args.add("--no-vulkan_async_skip_incomplete_frames");
        args.add("--log_level=" + level);
        args.add("--log_file=" + new File(root, "burnout.log").getAbsolutePath());
        args.add("--log_max_file_size_mb=32");
        args.add("--log_max_files=3");
        args.add("--fullscreen");  // a phone has no windows to speak of
        if ("true".equals(value("debug")) && "true".equals(value("debug_noisy"))) {
            args.add("--log_noisy=true");
        }
        if (!value("gamertag").isEmpty()) {
            args.add("--gamertag=" + value("gamertag"));
        }
        // A server address means that server and nothing local; otherwise this
        // copy runs a lobby of its own when local multiplayer is on.
        if (!value("lobby_server").isEmpty()) {
            args.add("--online");
            args.add("--online_fake_lobby=false");
            args.add("--lobby_server=" + value("lobby_server"));
        } else if ("true".equals(value("online"))) {
            args.add("--online");
            args.add("--online_fake_lobby=false");
        }
        return args;
    }
}
