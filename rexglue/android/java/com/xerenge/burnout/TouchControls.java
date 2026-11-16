package com.xerenge.burnout;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.hardware.input.InputManager;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.util.SparseArray;
import android.view.InputDevice;
import android.view.MotionEvent;
import android.view.View;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileOutputStream;
import java.io.InputStreamReader;
import java.io.OutputStreamWriter;
import java.nio.charset.StandardCharsets;
import java.util.Locale;

/**
 * The on-screen controls: translucent buttons drawn over the game, working a
 * virtual gamepad (android_touch.cpp) the game takes like a plugged-in pad.
 *
 *   left half      a floating stick: where the thumb lands is its centre
 *   right, low     RT (accelerate) and LT (brake)
 *   right, middle  A B X Y
 *   top corners    LB and RB; top middle Back and Start (drawn as the Xbox
 *                  View and Menu glyphs)
 *   top right      the menu: edit layout
 *
 * Edit layout lets every button, the stick and the menu be dragged and resized;
 * the result is kept in touch_layout.conf beside xerenge.conf.
 *
 * With touch_controls = auto the pad is hidden while a gamepad is connected - a
 * handheld's built-in one included - and comes back when it is taken away; the
 * menu button stays.
 */
final class TouchControls extends View implements InputManager.InputDeviceListener {
    // SDL_GamepadButton and SDL_GamepadAxis.
    private static final int A = 0, B = 1, X = 2, Y = 3, BACK = 4, START = 6, LB = 9, RB = 10;
    private static final int DPAD_UP = 11, DPAD_DOWN = 12, DPAD_LEFT = 13, DPAD_RIGHT = 14;
    private static final int LEFT_X = 0, LEFT_Y = 1, LEFT_TRIGGER = 4, RIGHT_TRIGGER = 5;

    private static native boolean nativeAttach();
    private static native void nativeButton(int button, boolean down);
    private static native void nativeAxis(int axis, float value);

    /** A round control: a button, a trigger pressed all the way, the stick's place, a d-pad direction, or the menu. */
    private static final class Control {
        final String label;
        final int button;   // or -1
        final int trigger;  // or -1
        final boolean stick, menu, dpad;
        float x, y, radius;  // pixels; set in layout
        float fx, fy, fr;    // x of the width, y of the height, size in units; the player's layout
        final float dfx, dfy, dfr;  // the stock layout
        boolean down;

        Control(String label, int button, int trigger, float fx, float fy, float fr) {
            this.label = label;
            this.button = button;
            this.trigger = trigger;
            this.stick = label.equals("Stick");
            this.menu = label.equals("Menu");
            this.dpad = label.equals("Up") || label.equals("Down")
                    || label.equals("Left") || label.equals("Right");
            this.fx = dfx = fx;
            this.fy = dfy = fy;
            this.fr = dfr = fr;
        }

        void reset() {
            fx = dfx;
            fy = dfy;
            fr = dfr;
        }
    }

    /** What the menu button asks of the activity. */
    interface MenuListener {
        void onMenu();
    }

    private final Control[] controls = {
        new Control("RT", -1, RIGHT_TRIGGER, 0.90f, 0.76f, 1.00f),
        new Control("LT", -1, LEFT_TRIGGER, 0.75f, 0.84f, 0.80f),
        new Control("A", A, -1, 0.84f, 0.47f, 0.45f),
        new Control("B", B, -1, 0.92f, 0.33f, 0.45f),
        new Control("X", X, -1, 0.76f, 0.33f, 0.45f),
        new Control("Y", Y, -1, 0.84f, 0.19f, 0.45f),
        new Control("LB", LB, -1, 0.07f, 0.12f, 0.50f),
        new Control("RB", RB, -1, 0.66f, 0.12f, 0.50f),
        new Control("Back", BACK, -1, 0.40f, 0.09f, 0.38f),
        new Control("Start", START, -1, 0.52f, 0.09f, 0.38f),
        new Control("Stick", -1, -1, 0.17f, 0.66f, 1.15f),
        new Control("Menu", -1, -1, 0.96f, 0.08f, 0.32f),
        // The d-pad cross, where the stick's place is: shown in place of the
        // stick when the layout's D-pad switch is on (digital steering).
        new Control("Up", DPAD_UP, -1, 0.17f, 0.55f, 0.42f),
        new Control("Down", DPAD_DOWN, -1, 0.17f, 0.77f, 0.42f),
        new Control("Left", DPAD_LEFT, -1, 0.06f, 0.66f, 0.42f),
        new Control("Right", DPAD_RIGHT, -1, 0.28f, 0.66f, 0.42f),
    };
    private final Control stickControl = controls[10];
    private boolean dpadMode;  // the stick's place holds a d-pad cross instead

    private boolean padVisible;  // the pad itself; the menu button is always there
    // touch_controls = auto: the pad is hidden while a gamepad is connected.
    private final boolean autoHide;
    // The name of the gamepad connected, for the log, or null.
    private String gamepad;
    // Whether the virtual pad has been asked for: only once the pad is first
    // shown, so a game played on a real pad alone has no second one.
    private boolean attaching;
    private MenuListener menuListener;
    private boolean editing;
    private Control selected;
    private int dragPointer = -1;
    private float dragDx, dragDy;
    private final RectF[] editButtons = {new RectF(), new RectF(), new RectF(), new RectF(), new RectF()};
    private static final String[] EDIT_LABELS = {"-", "+", "Reset", "D-pad", "Done"};

    // The stick.
    private float stickRestX, stickRestY, stickRadius;
    private float stickCentreX, stickCentreY, stickX, stickY;
    private int stickPointer = -1;

    private final SparseArray<Control> pointers = new SparseArray<>();
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint ring = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint text = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final int alpha;
    private final Handler handler = new Handler(Looper.getMainLooper());

    TouchControls(Context context, int opacityPercent, boolean autoHide, MenuListener menuListener) {
        super(context);
        this.autoHide = autoHide;
        this.menuListener = menuListener;
        loadLayout();
        alpha = Math.max(10, Math.min(100, opacityPercent)) * 255 / 100;
        fill.setStyle(Paint.Style.FILL);
        ring.setStyle(Paint.Style.STROKE);
        text.setTextAlign(Paint.Align.CENTER);
        text.setFakeBoldText(true);
        findGamepad();
    }

    @Override
    protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        if (autoHide) {
            InputManager input = (InputManager) getContext().getSystemService(Context.INPUT_SERVICE);
            input.registerInputDeviceListener(this, handler);
            findGamepad();
        }
    }

    @Override
    protected void onDetachedFromWindow() {
        if (autoHide) {
            InputManager input = (InputManager) getContext().getSystemService(Context.INPUT_SERVICE);
            input.unregisterInputDeviceListener(this);
        }
        super.onDetachedFromWindow();
    }

    @Override
    public void onInputDeviceAdded(int id) {
        findGamepad();
    }

    @Override
    public void onInputDeviceRemoved(int id) {
        findGamepad();
    }

    @Override
    public void onInputDeviceChanged(int id) {
        findGamepad();
    }

    // Whether a gamepad is connected, for auto: any device Android calls a
    // gamepad or a joystick, which a handheld's built-in pad is. The pad these
    // controls work is SDL's own and not one of Android's devices; Android's
    // virtual devices are left out.
    private void findGamepad() {
        String found = null;
        if (autoHide) {
            for (int id : InputDevice.getDeviceIds()) {
                InputDevice device = InputDevice.getDevice(id);
                if (device == null || device.isVirtual()) {
                    continue;
                }
                int sources = device.getSources();
                if ((sources & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
                        || (sources & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK) {
                    found = device.getName();
                    break;
                }
            }
            if (found == null ? gamepad != null : !found.equals(gamepad)) {
                Log.i("Xerenge", found != null ? "touch controls: gamepad \"" + found + "\" connected, pad hidden"
                        : "touch controls: no gamepad connected, pad shown");
            }
        }
        gamepad = found;
        boolean shown = found == null;
        if (shown != padVisible) {
            if (!shown) {
                letGo();
            }
            padVisible = shown;
            invalidate();
        }
        // The pad appears once the game has brought SDL's joysticks up.
        if (padVisible && !attaching) {
            attaching = true;
            handler.post(new Runnable() {
                @Override
                public void run() {
                    if (!nativeAttach()) {
                        handler.postDelayed(this, 500);
                    }
                }
            });
        }
    }

    // Whatever is held is let go of, as the pad leaves the screen.
    private void letGo() {
        for (int i = 0; i < pointers.size(); ++i) {
            press(pointers.valueAt(i), false);
        }
        pointers.clear();
        if (stickPointer >= 0) {
            releaseStick();
        }
    }

    @Override
    protected void onSizeChanged(int width, int height, int oldWidth, int oldHeight) {
        layout();
        float unit = unit();
        text.setTextSize(0.36f * unit);
        ring.setStrokeWidth(0.06f * unit);
        float bw = 2.2f * unit, bh = 0.9f * unit, gap = 0.2f * unit;
        float left = width / 2f - (4 * bw + 3 * gap) / 2f;
        for (int i = 0; i < editButtons.length; ++i) {
            float l = left + i * (bw + gap);
            editButtons[i].set(l, height - bh - 1.2f * unit, l + bw, height - 1.2f * unit);
        }
    }

    private float unit() {
        return Math.min(getWidth(), getHeight()) / 7.0f;
    }

    /** Pixels from the fractions; the stick keeps its place while a thumb is on it. */
    private void layout() {
        float unit = unit();
        for (Control control : controls) {
            control.x = control.fx * getWidth();
            control.y = control.fy * getHeight();
            control.radius = control.fr * unit;
        }
        stickRestX = stickControl.x;
        stickRestY = stickControl.y;
        stickRadius = stickControl.radius;
        if (stickPointer < 0) {
            stickCentreX = stickX = stickRestX;
            stickCentreY = stickY = stickRestY;
        }
    }

    // --- the player's layout, touch_layout.conf: Label = x y size ---

    private File layoutFile() {
        return new File(Settings.root(getContext()), "touch_layout.conf");
    }

    private void loadLayout() {
        File file = layoutFile();
        if (!file.isFile()) {
            return;
        }
        try (BufferedReader in = new BufferedReader(
                new InputStreamReader(new FileInputStream(file), StandardCharsets.UTF_8))) {
            String line;
            while ((line = in.readLine()) != null) {
                int equals = line.indexOf('=');
                if (line.startsWith("#") || equals <= 0) {
                    continue;
                }
                String key = line.substring(0, equals).trim();
                if (key.equals("Dpad")) {
                    // The stick/d-pad switch: a d-pad cross in the stick's place.
                    dpadMode = line.substring(equals + 1).trim().equals("true");
                    continue;
                }
                String[] v = line.substring(equals + 1).trim().split("\\s+");
                for (Control control : controls) {
                    if (control.label.equals(key) && v.length == 3) {
                        try {
                            control.fx = clamp(Float.parseFloat(v[0]), 0f, 1f);
                            control.fy = clamp(Float.parseFloat(v[1]), 0f, 1f);
                            control.fr = clamp(Float.parseFloat(v[2]), control.dfr * 0.4f, control.dfr * 2.5f);
                        } catch (NumberFormatException ignored) {
                        }
                    }
                }
            }
        } catch (Exception ignored) {
        }
    }

    private void saveLayout() {
        StringBuilder out = new StringBuilder("# On-screen controls layout: label = x y size (x of the width, y of the height)\n");
        for (Control control : controls) {
            out.append(control.label).append(" = ")
               .append(String.format(Locale.US, "%.4f %.4f %.3f", control.fx, control.fy, control.fr)).append('\n');
        }
        out.append("Dpad = ").append(dpadMode).append('\n');
        try (OutputStreamWriter writer = new OutputStreamWriter(new FileOutputStream(layoutFile()), StandardCharsets.UTF_8)) {
            writer.write(out.toString());
        } catch (Exception ignored) {
        }
    }

    private static float clamp(float value, float low, float high) {
        return Math.max(low, Math.min(high, value));
    }

    /** Edit layout: buttons are dragged, sized with - and +, and kept with Done. */
    void beginEdit() {
        for (int i = 0; i < pointers.size(); ++i) {
            press(pointers.valueAt(i), false);
        }
        pointers.clear();
        releaseStick();
        editing = true;
        selected = null;
        invalidate();
    }

    @Override
    protected void onDraw(Canvas canvas) {
        if ((padVisible || editing) && !dpadMode) {
            // The stick: its base, and the knob where the thumb is.
            paintColor(false, 0.5f);
            canvas.drawCircle(stickCentreX, stickCentreY, stickRadius, fill);
            canvas.drawCircle(stickCentreX, stickCentreY, stickRadius, ring);
            paintColor(stickPointer >= 0, 1.0f);
            canvas.drawCircle(stickX, stickY, stickRadius * 0.45f, fill);
        }
        for (Control control : controls) {
            if ((control.stick && dpadMode) || (control.dpad && !dpadMode)
                    || (!padVisible && !editing && !control.menu)) {
                continue;
            }
            paintColor(control.down, 1.0f);
            if (control.trigger >= 0) {
                RectF box = new RectF(control.x - control.radius * 0.8f, control.y - control.radius,
                        control.x + control.radius * 0.8f, control.y + control.radius);
                canvas.drawRoundRect(box, control.radius * 0.3f, control.radius * 0.3f, fill);
                canvas.drawRoundRect(box, control.radius * 0.3f, control.radius * 0.3f, ring);
            } else {
                canvas.drawCircle(control.x, control.y, control.radius, fill);
                canvas.drawCircle(control.x, control.y, control.radius, ring);
            }
            if (control.menu) {
                drawDots(canvas, control);
            } else if (control.button == BACK) {
                drawViewGlyph(canvas, control);
            } else if (control.button == START) {
                drawMenuGlyph(canvas, control);
            } else {
                canvas.drawText(control.label, control.x, control.y - (text.ascent() + text.descent()) / 2, text);
            }
        }
        if (editing) {
            drawEditor(canvas);
        }
    }

    // The Xbox View button (two overlapping rectangles, the old Back) ...
    private void drawViewGlyph(Canvas canvas, Control c) {
        float s = c.radius * 0.30f;
        text.setStyle(Paint.Style.STROKE);
        text.setStrokeWidth(Math.max(2f, c.radius * 0.09f));
        canvas.drawRoundRect(new RectF(c.x - s * 1.3f, c.y - s * 0.7f, c.x + s * 0.5f, c.y + s * 1.1f), s * 0.2f, s * 0.2f, text);
        canvas.drawRoundRect(new RectF(c.x - s * 0.5f, c.y - s * 1.1f, c.x + s * 1.3f, c.y + s * 0.7f), s * 0.2f, s * 0.2f, text);
        text.setStyle(Paint.Style.FILL);
    }

    // ... and the Menu button (three bars, the old Start).
    private void drawMenuGlyph(Canvas canvas, Control c) {
        float w = c.radius * 0.42f, gap = c.radius * 0.26f;
        text.setStyle(Paint.Style.STROKE);
        text.setStrokeWidth(Math.max(2f, c.radius * 0.11f));
        text.setStrokeCap(Paint.Cap.ROUND);
        for (int i = -1; i <= 1; ++i) {
            canvas.drawLine(c.x - w, c.y + i * gap, c.x + w, c.y + i * gap, text);
        }
        text.setStyle(Paint.Style.FILL);
        text.setStrokeCap(Paint.Cap.BUTT);
    }

    private void drawDots(Canvas canvas, Control c) {
        for (int i = -1; i <= 1; ++i) {
            canvas.drawCircle(c.x + i * c.radius * 0.42f, c.y, c.radius * 0.13f, text);
        }
    }

    private void drawEditor(Canvas canvas) {
        Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
        p.setStyle(Paint.Style.FILL);
        p.setColor(Color.argb(110, 0, 0, 0));
        canvas.drawRect(0, 0, getWidth(), getHeight(), p);
        if (selected != null) {
            p.setStyle(Paint.Style.STROKE);
            p.setStrokeWidth(ring.getStrokeWidth() * 1.5f);
            p.setColor(Color.rgb(255, 200, 40));
            float r = selected.radius * (selected.trigger >= 0 ? 1.1f : 1.2f);
            canvas.drawCircle(selected.x, selected.y, r, p);
        }
        p.setTextAlign(Paint.Align.CENTER);
        p.setTextSize(0.4f * unit());
        p.setStyle(Paint.Style.FILL);
        p.setColor(Color.WHITE);
        canvas.drawText(selected == null ? "Drag a button. Tap one, then - or + to resize."
                : selected.label + ": drag to move, - and + to resize", getWidth() / 2f, 0.9f * unit() + 0.4f * unit(), p);
        for (int i = 0; i < editButtons.length; ++i) {
            p.setColor(Color.argb(210, 40, 40, 48));
            canvas.drawRoundRect(editButtons[i], 14f, 14f, p);
            p.setColor(i == 4 ? Color.rgb(120, 220, 120) : Color.WHITE);
            canvas.drawText(EDIT_LABELS[i], editButtons[i].centerX(),
                    editButtons[i].centerY() - (p.ascent() + p.descent()) / 2, p);
        }
    }

    private void paintColor(boolean pressed, float scale) {
        int a = (int) (alpha * scale);
        fill.setColor(Color.argb(pressed ? Math.min(255, a + 60) : a / 2, 255, 255, 255));
        ring.setColor(Color.argb(Math.min(255, a + 40), 255, 255, 255));
        text.setColor(Color.argb(Math.min(255, a + 80), 20, 20, 24));
    }

    private Control hit(float x, float y) {
        Control best = null;
        float bestDistance = Float.MAX_VALUE;
        for (Control control : controls) {
            if (!padVisible && !control.menu && !editing) {
                continue;
            }
            if ((control.stick && (dpadMode || !editing)) || (control.dpad && !dpadMode)) {
                continue;
            }
            float dx = x - control.x, dy = y - control.y;
            float distance = (float) Math.sqrt(dx * dx + dy * dy);
            float reach = Math.max(control.radius * 1.25f, 0.6f * unit());  // a small button is still easy to hit
            if (distance < reach && distance < bestDistance) {
                best = control;
                bestDistance = distance;
            }
        }
        return best;
    }

    private void press(Control control, boolean down) {
        control.down = down;
        if (control.trigger >= 0) {
            nativeAxis(control.trigger, down ? 1.0f : 0.0f);
        } else {
            nativeButton(control.button, down);
        }
    }

    private void moveStick(float x, float y) {
        float dx = x - stickCentreX, dy = y - stickCentreY;
        float length = (float) Math.sqrt(dx * dx + dy * dy);
        if (length > stickRadius) {
            dx *= stickRadius / length;
            dy *= stickRadius / length;
        }
        stickX = stickCentreX + dx;
        stickY = stickCentreY + dy;
        nativeAxis(LEFT_X, dx / stickRadius);
        nativeAxis(LEFT_Y, dy / stickRadius);
    }

    private boolean editTouch(MotionEvent event) {
        int action = event.getActionMasked();
        int index = event.getActionIndex();
        float x = event.getX(index), y = event.getY(index);
        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN:
                for (int i = 0; i < editButtons.length; ++i) {
                    if (editButtons[i].contains(x, y)) {
                        editButton(i);
                        invalidate();
                        return true;
                    }
                }
                Control control = hit(x, y);
                if (control != null) {
                    selected = control;
                    dragPointer = event.getPointerId(index);
                    dragDx = control.x - x;
                    dragDy = control.y - y;
                } else {
                    selected = null;
                }
                break;
            case MotionEvent.ACTION_MOVE:
                for (int i = 0; i < event.getPointerCount(); ++i) {
                    if (selected != null && event.getPointerId(i) == dragPointer) {
                        selected.fx = clamp((event.getX(i) + dragDx) / getWidth(), 0.02f, 0.98f);
                        selected.fy = clamp((event.getY(i) + dragDy) / getHeight(), 0.03f, 0.97f);
                        layout();
                    }
                }
                break;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP:
            case MotionEvent.ACTION_CANCEL:
                if (event.getPointerId(index) == dragPointer) {
                    dragPointer = -1;
                }
                break;
            default:
                break;
        }
        invalidate();
        return true;
    }

    private void editButton(int i) {
        if (i == 0 || i == 1) {
            if (selected != null) {
                selected.fr = clamp(selected.fr * (i == 1 ? 1.12f : 1f / 1.12f), selected.dfr * 0.4f, selected.dfr * 2.5f);
            }
        } else if (i == 2) {
            for (Control control : controls) {
                control.reset();
            }
        } else if (i == 3) {
            // The stick/d-pad switch: a d-pad cross in the stick's place.
            dpadMode = !dpadMode;
            if (dpadMode) {
                releaseStick();
            }
        } else {
            editing = false;
            selected = null;
            saveLayout();
        }
        layout();
    }

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        if (editing) {
            return editTouch(event);
        }
        if (!padVisible) {
            // Only the menu button takes touches; the rest belongs to the game below.
            int a = event.getActionMasked();
            if (a == MotionEvent.ACTION_DOWN) {
                Control c = hit(event.getX(), event.getY());
                if (c != null && c.menu) {
                    menuListener.onMenu();
                    return true;
                }
            }
            return false;
        }
        int action = event.getActionMasked();
        int index = event.getActionIndex();
        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN: {
                int id = event.getPointerId(index);
                float x = event.getX(index), y = event.getY(index);
                Control control = hit(x, y);
                if (control != null && control.menu) {
                    menuListener.onMenu();
                } else if (control != null) {
                    pointers.put(id, control);
                    press(control, true);
                } else if (!dpadMode && stickPointer < 0 && x < getWidth() * 0.5f) {
                    stickPointer = id;
                    stickCentreX = x;
                    stickCentreY = y;
                    moveStick(x, y);
                }
                break;
            }
            case MotionEvent.ACTION_MOVE:
                for (int i = 0; i < event.getPointerCount(); ++i) {
                    if (event.getPointerId(i) == stickPointer) {
                        moveStick(event.getX(i), event.getY(i));
                    }
                }
                break;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP:
            case MotionEvent.ACTION_CANCEL: {
                if (action == MotionEvent.ACTION_CANCEL) {
                    for (int i = 0; i < pointers.size(); ++i) {
                        press(pointers.valueAt(i), false);
                    }
                    pointers.clear();
                    releaseStick();
                    break;
                }
                int id = event.getPointerId(index);
                Control control = pointers.get(id);
                if (control != null) {
                    pointers.remove(id);
                    press(control, false);
                }
                if (id == stickPointer) {
                    releaseStick();
                }
                break;
            }
            default:
                break;
        }
        invalidate();
        return true;
    }

    private void releaseStick() {
        stickPointer = -1;
        stickCentreX = stickX = stickRestX;
        stickCentreY = stickY = stickRestY;
        nativeAxis(LEFT_X, 0);
        nativeAxis(LEFT_Y, 0);
    }
}
