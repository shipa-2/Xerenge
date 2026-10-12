package com.xerenge.burnout;

import android.content.Context;
import android.media.AudioAttributes;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.SystemClock;
import android.os.VibrationAttributes;
import android.os.VibrationEffect;
import android.os.Vibrator;
import android.os.VibratorManager;
import android.util.Log;

/**
 * The game's rumble on the device's own vibrator: for a handheld whose
 * built-in pad has no motors, and for a phone played on the on-screen
 * controls. `vibration` in xerenge.conf says when: the hook on the title's rumble (android_vibration.cpp)
 * goes by it, and calls set() with one strength for the pad's two motors
 * whenever that changes, and every half second while it stays on.
 *
 * A strength is played as one long vibration, replaced by the next one: a
 * game's rumble changes many times a second, and nothing may queue up behind
 * what is playing. It is asked for again before it runs out for as long as the
 * game keeps asking, and it ends by itself, HOLD_MS at the latest, if the game
 * stops asking without saying 0. Leaving the game (pause) cuts it at once.
 */
final class DeviceVibration {
    private static final String TAG = "Xerenge";

    // How long one vibration lasts unless replaced, and how old it may be
    // before the game's next request starts it again.
    private static final long HOLD_MS = 5000, RENEW_MS = 4000;
    // A rumble shorter than this is played this long: the title's shortest
    // are a frame or two, which a motor does not get up to speed in.
    private static final long MIN_PULSE_MS = 40;
    // A strength playing is replaced by another no sooner than this: each
    // replacement starts the motor afresh.
    private static final long CHANGE_MS = 30;
    // Without amplitude control the motor is on or off: on from this strength.
    private static final int ON_FROM = 80;

    private static native void nativeStart();

    private static Vibrator vibrator;
    private static boolean amplitudeControl;
    private static Handler handler;

    // What the game last asked for, 0 to 255, and when (uptimeMillis).
    private static volatile int wanted;
    private static volatile long wantedAt;

    // Under the class's lock: the strength that is playing (0: none), when it
    // was started, when the motor went on, and whether the game is left.
    private static int playing;
    private static long playingSince, onSince;
    private static boolean paused;

    private static final Runnable APPLY = new Runnable() {
        @Override
        public void run() {
            apply();
        }
    };

    /** From the game's activity, once its libraries are loaded. */
    static synchronized void start(Context context) {
        if (handler != null) {
            return;
        }
        Vibrator found;
        if (Build.VERSION.SDK_INT >= 31) {
            VibratorManager manager =
                    (VibratorManager) context.getSystemService(Context.VIBRATOR_MANAGER_SERVICE);
            found = manager != null ? manager.getDefaultVibrator() : null;
        } else {
            found = (Vibrator) context.getSystemService(Context.VIBRATOR_SERVICE);
        }
        if (found == null || !found.hasVibrator()) {
            Log.i(TAG, "device vibration: this device has no vibrator");
            return;
        }
        vibrator = found;
        amplitudeControl = found.hasAmplitudeControl();
        Log.i(TAG, "device vibration: a vibrator " + (amplitudeControl ? "with" : "without") + " amplitude control");
        // The vibrator is a call to another process: not on the game's thread.
        HandlerThread thread = new HandlerThread("vibration");
        thread.start();
        handler = new Handler(thread.getLooper());
        nativeStart();
    }

    /** From the game, on its own thread: 0 stops, 1 to 255 is the strength. */
    static void set(int strength) {
        wanted = Math.max(0, Math.min(255, strength));
        wantedAt = SystemClock.uptimeMillis();
        handler.post(APPLY);
    }

    /** The game is left (paused, stopped, closed): nothing vibrates until it is back. */
    static synchronized void pause() {
        paused = true;
        // Not what it was at when the game comes back: what it asks for then.
        wanted = 0;
        stop("the game is left");
    }

    static synchronized void resume() {
        paused = false;
    }

    // Makes what is playing what the game last asked for.
    private static synchronized void apply() {
        handler.removeCallbacks(APPLY);
        long now = SystemClock.uptimeMillis();
        int want = paused ? 0 : wanted;
        if (!amplitudeControl && want != 0) {
            want = want >= ON_FROM ? 255 : 0;
        }
        if (want == playing) {
            // Still asked for, and about to run out.
            if (want != 0 && wantedAt - playingSince >= RENEW_MS) {
                play(want, now);
            }
            return;
        }
        if (want == 0) {
            if (now < onSince + MIN_PULSE_MS) {
                handler.postAtTime(APPLY, onSince + MIN_PULSE_MS);
                return;
            }
            stop("the game's rumble ended");
            return;
        }
        if (playing == 0) {
            onSince = now;
            Log.d(TAG, "device vibration: on, strength " + want);
        } else if (now < playingSince + CHANGE_MS) {
            handler.postAtTime(APPLY, playingSince + CHANGE_MS);
            return;
        }
        play(want, now);
    }

    private static void play(int strength, long now) {
        try {
            VibrationEffect effect = VibrationEffect.createOneShot(HOLD_MS,
                    amplitudeControl ? strength : VibrationEffect.DEFAULT_AMPLITUDE);
            // A game's rumble is media to Android: it follows the user's
            // setting for that, not the one for touch feedback.
            if (Build.VERSION.SDK_INT >= 33) {
                vibrator.vibrate(effect, VibrationAttributes.createForUsage(VibrationAttributes.USAGE_MEDIA));
            } else {
                vibrator.vibrate(effect,
                        new AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_GAME).build());
            }
        } catch (RuntimeException error) {
            Log.w(TAG, "device vibration: " + error);
        }
        playing = strength;
        playingSince = now;
    }

    private static void stop(String why) {
        if (playing == 0 || vibrator == null) {
            return;
        }
        try {
            vibrator.cancel();
        } catch (RuntimeException error) {
            Log.w(TAG, "device vibration: " + error);
        }
        playing = 0;
        Log.d(TAG, "device vibration: off after " + (SystemClock.uptimeMillis() - onSince) + " ms, " + why);
    }
}
