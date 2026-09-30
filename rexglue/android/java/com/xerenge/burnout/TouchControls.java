package com.xerenge.burnout;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.RectF;
import android.os.Handler;
import android.os.Looper;
import android.util.SparseArray;
import android.view.MotionEvent;
import android.view.View;

/**
 * The on-screen controls: translucent buttons drawn over the game, working a
 * virtual gamepad (android_touch.cpp) the game takes like a plugged-in pad.
 *
 *   left half      a floating stick: where the thumb lands is its centre
 *   right, low     RT (accelerate) and LT (brake)
 *   right, middle  A B X Y
 *   top corners    LB and RB; top middle Back and Start
 */
final class TouchControls extends View {
    // SDL_GamepadButton and SDL_GamepadAxis.
    private static final int A = 0, B = 1, X = 2, Y = 3, BACK = 4, START = 6, LB = 9, RB = 10;
    private static final int LEFT_X = 0, LEFT_Y = 1, LEFT_TRIGGER = 4, RIGHT_TRIGGER = 5;

    private static native boolean nativeAttach();
    private static native void nativeButton(int button, boolean down);
    private static native void nativeAxis(int axis, float value);

    /** A round control: a button, or a trigger pressed all the way. */
    private static final class Control {
        final String label;
        final int button;   // or -1
        final int trigger;  // or -1
        float x, y, radius;  // in fractions of the height, x of the width; set in layout
        final float fx, fy, fr;
        boolean down;

        Control(String label, int button, int trigger, float fx, float fy, float fr) {
            this.label = label;
            this.button = button;
            this.trigger = trigger;
            this.fx = fx;
            this.fy = fy;
            this.fr = fr;
        }
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
    };

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

    TouchControls(Context context, int opacityPercent) {
        super(context);
        alpha = Math.max(10, Math.min(100, opacityPercent)) * 255 / 100;
        fill.setStyle(Paint.Style.FILL);
        ring.setStyle(Paint.Style.STROKE);
        text.setTextAlign(Paint.Align.CENTER);
        text.setFakeBoldText(true);
        // The pad appears once the game has brought SDL's joysticks up.
        handler.post(new Runnable() {
            @Override
            public void run() {
                if (!nativeAttach()) {
                    handler.postDelayed(this, 500);
                }
            }
        });
    }

    @Override
    protected void onSizeChanged(int width, int height, int oldWidth, int oldHeight) {
        float unit = Math.min(width, height) / 7.0f;
        for (Control control : controls) {
            control.x = control.fx * width;
            control.y = control.fy * height;
            control.radius = control.fr * unit;
        }
        stickRestX = 0.17f * width;
        stickRestY = 0.66f * height;
        stickRadius = 1.15f * unit;
        text.setTextSize(0.36f * unit);
        ring.setStrokeWidth(0.06f * unit);
        if (stickPointer < 0) {
            stickCentreX = stickX = stickRestX;
            stickCentreY = stickY = stickRestY;
        }
    }

    @Override
    protected void onDraw(Canvas canvas) {
        // The stick: its base, and the knob where the thumb is.
        paintColor(false, 0.5f);
        canvas.drawCircle(stickCentreX, stickCentreY, stickRadius, fill);
        canvas.drawCircle(stickCentreX, stickCentreY, stickRadius, ring);
        paintColor(stickPointer >= 0, 1.0f);
        canvas.drawCircle(stickX, stickY, stickRadius * 0.45f, fill);
        for (Control control : controls) {
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
            canvas.drawText(control.label, control.x, control.y - (text.ascent() + text.descent()) / 2, text);
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
            float dx = x - control.x, dy = y - control.y;
            float distance = (float) Math.sqrt(dx * dx + dy * dy);
            if (distance < control.radius * 1.25f && distance < bestDistance) {
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

    @Override
    public boolean onTouchEvent(MotionEvent event) {
        int action = event.getActionMasked();
        int index = event.getActionIndex();
        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN: {
                int id = event.getPointerId(index);
                float x = event.getX(index), y = event.getY(index);
                Control control = hit(x, y);
                if (control != null) {
                    pointers.put(id, control);
                    press(control, true);
                } else if (stickPointer < 0 && x < getWidth() * 0.5f) {
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
