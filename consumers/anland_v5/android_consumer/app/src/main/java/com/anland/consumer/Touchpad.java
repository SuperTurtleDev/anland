package com.anland.consumer;

import android.content.Context;
import android.graphics.Matrix;
import android.view.InputDevice;
import android.view.MotionEvent;
import android.view.ViewConfiguration;

/**
 * Laptop-style touchpad: interprets finger contacts as relative cursor motion,
 * taps/clicks, long-press drag and two-finger scroll.
 *
 * One class serves both touch devices the app has, because the gesture logic is
 * the same for both; they differ only in the coordinate space their contacts
 * arrive in, which {@link #setInputBounds} describes:
 *
 * <ul>
 *   <li>the on-screen touchpad — input is the screen, so the bounds are the view
 *       size; active while touchpad mode is on;</li>
 *   <li>the physical touchpad (and any physical mouse behind it) — input is the
 *       pad's own coordinate range; active while pointer capture is on.</li>
 * </ul>
 *
 * Only three gestures are implemented: one finger moves the cursor, two fingers
 * travelling together scroll, and a quick two-finger tap is a right click.
 * Anything else — a pinch, three or more fingers — is forwarded as touch, mapped
 * into a square around the cursor (see {@link #setGestureScale}).
 *
 * The cursor itself belongs to the host: this class only reports deltas through
 * {@link Output#onMotion} and reads the current position back through
 * {@link Output#cursorX}/{@link Output#cursorY}, so both instances share one
 * pointer.
 */
public final class Touchpad {

    /** Touch actions used by {@link Output#onTouch}, matching the wire protocol. */
    static final int TOUCH_DOWN = 0;
    static final int TOUCH_UP = 1;
    static final int TOUCH_MOVE = 2;

    /** Everything this class produces, and the cursor position it needs back. */
    interface Output {
        void onMotion(float dx, float dy);
        void onScroll(int axis, float value);
        void onButton(int button, boolean pressed);
        /** One contact of a gesture forwarded as touch, in output coordinates. */
        void onTouch(int action, int pointerId, float x, float y);
        /** End of a batch of {@link #onTouch} calls. */
        void onTouchFrame();
        float cursorX();
        float cursorY();
    }

    // 状态机
    private static final int STATE_IDLE = 0;
    private static final int STATE_ONE_FINGER = 1;
    private static final int STATE_TWO_FINGER = 2;
    private static final int STATE_DRAGGING = 3;
    private int currentState = STATE_IDLE;

    private float lastX1, lastY1;
    private float startX1, startY1;
    private float lastX2, lastY2;
    private long downTime1;
    // True while lastX1/lastY1 (and lastX2/lastY2 in a two-finger phase) hold a
    // live baseline for clickpadButton(). A physical clickpad press cancels the
    // gesture before recognize() sees it; cancel() intentionally does not clear
    // this, so the prior finger positions survive for the press to be judged.
    private boolean clickpadBaselineValid = false;
    private final float touchSlop;

    // Physical pads can disable synthesized taps while retaining real buttons
    // (those are delivered separately by ImmersiveMode / pointer capture).
    private boolean tapToClick = true;
    private int scrollGuardMs;
    // Absolute event-time deadline rather than a per-stream latch. Long-press
    // drag remains available once the guard has elapsed; a latched flag caused
    // a press started inside the guard to be suppressed for its whole lifetime.
    private long scrollGuardUntil = Long.MIN_VALUE / 2;
    private long gestureEventTime;
    private boolean scrollInStream;
    private boolean debug;

    void setTapFilter(boolean enabled, int guardMs, boolean debug) {
        tapToClick = enabled;
        scrollGuardMs = Math.max(0, Math.min(1000, guardMs));
        this.debug = debug;
    }

    /** True when taps may be synthesized at this event time. */
    private boolean tapAllowed(long time) {
        return tapToClick && !scrollInStream && time >= scrollGuardUntil;
    }

    /** Extend the post-scroll guard to the current event. */
    private void extendScrollGuard() {
        scrollGuardUntil = gestureEventTime + scrollGuardMs;
    }

    private void markScroll() {
        scrollInStream = true;
        extendScrollGuard();
        isSingleTapCandidate = false;
        isTwoFingerTapCandidate = false;
        isLongPressPossible = false;
    }

    private boolean isSingleTapCandidate = false;
    private boolean isTwoFingerTapCandidate = false;
    private boolean isDraggingActive = false;

    private long lastTapTime = 0;
    private float lastTapX, lastTapY;
    private boolean isDoubleTapPending = false;

    private static final long TOUCH_LONG_PRESS_TIMEOUT = 500;
    private boolean hasLongPressed = false;
    private boolean isLongPressPossible = false;
    private boolean isMultiFinger = false;

    // Two-finger classification, decided once per two-finger phase so a gesture
    // cannot oscillate between scrolling and being declined. ONE_MOVING is the
    // asynchronous-contact state: one finger has committed, but the other has
    // not yet moved far enough to say whether the gesture is a scroll or a pinch.
    private static final int TWO_FINGER_UNDECIDED = 0;
    private static final int TWO_FINGER_ONE_MOVING = 1;
    private static final int TWO_FINGER_SCROLL = 2;
    private static final int TWO_FINGER_NOT_SCROLL = 3;
    // Same phase classified as NOT_SCROLL (a pinch) while multi-finger gestures
    // are disabled: the whole phase is swallowed instead of being forwarded as
    // touch, so the gesture does nothing and lifting one finger resumes normal
    // one-finger cursor control.
    private static final int TWO_FINGER_IGNORED = 4;
    private int twoFingerMode = TWO_FINGER_UNDECIDED;
    // A two-finger tap may jitter slightly. Use a separate tolerance instead of
    // rejecting the tap as soon as one contact exceeds the one-finger touchSlop.
    private static final float TWO_FINGER_TAP_SLOP_FACTOR = 2.0f;
    private final float twoFingerTapSlop;
    private float twoFingerMaxTravel;
    private long streamDownTime;
    // True while three or more fingers are on the pad and multi-finger gestures
    // are disabled: swallow the stream (see recognize) until it drops to one.
    private boolean multiFingerIgnored = false;
    // Both fingers' positions when the two-finger phase began. The scroll test
    // measures displacement from here rather than frame to frame, matching AOSP.
    private float twoFingerStartX1, twoFingerStartY1;
    private float twoFingerStartX2, twoFingerStartY2;

    // Latched for the rest of the gesture once the recognizer declines it, so the
    // whole remaining stream (including the final UP) reaches the forwarding path
    // and the touches it emitted are released.
    private boolean gestureUnhandled = false;

    // AOSP's equivalents are 1.5mm and 7.0mm ("Two Finger Scroll/Move Distance
    // Thresh" in libgestures). Coordinates here are output pixels rather than
    // millimetres, so the physical values do not carry over; these are multiples of
    // touchSlop instead, and both multipliers are exposed in Settings because the
    // right values depend on the pad. Defaults keep AOSP's ~4.7:1 ratio.
    static final float DEFAULT_SCROLL_THRESHOLD_FACTOR = 0.5f;
    static final float DEFAULT_MOVE_THRESHOLD_FACTOR = 2.35f;
    private float scrollDistanceThreshold;
    private float moveDistanceThreshold;

    // Which axis a two-finger scroll is reported on: an axis wins when it exceeds
    // this fraction of the other, so a diagonal drag can report both.
    private static final float AXIS_DOMINANCE_RATIO = 0.5f;
    /** Scroll distance per pixel of finger travel. Configurable in Settings. */
    static final float DEFAULT_SCROLL_SPEED = 0.5f;
    private float scrollSpeed = DEFAULT_SCROLL_SPEED;
    private boolean scrollReversed = false;

    // The input coordinate space: where this device's contacts live. A zero range
    // means "not known yet", which leaves coordinates untouched.
    private float inputMinX = 0f;
    private float inputMinY = 0f;
    private float inputRangeX = 0f;
    private float inputRangeY = 0f;
    // The output coordinate space, i.e. the view the gestures act on.
    private int outputWidth = 0;
    private int outputHeight = 0;

    /** Side of the square, in output pixels, a forwarded gesture is mapped into. */
    static final float DEFAULT_GESTURE_SCALE = 800f;
    private float gestureScale = DEFAULT_GESTURE_SCALE;

    private float mouseAccelStrength = 1.0f; // 加速度强度，0.5 ~ 10.0

    // 禁用双指捏合/张开与三指及以上的触摸转发手势（缩放等）。开启后这些
    // 手势被吞掉（不转发为触摸），双指滚动与双指右键点击不受影响。
    private boolean multiFingerGesturesDisabled = false;

    // ===== 调整后的平滑/抗抖动参数（更灵敏、更连续） =====
    private static final float DEAD_ZONE = 0.3f;          // 死区从 0.5 降到 0.3
    private static final float SMOOTHING_FACTOR = 0.45f;   // 提高响应速度
    private static final float ACCUMULATED_THRESHOLD = 0.1f; // 从 0.8 大幅降低，让移动更连续

    private float smoothedDx = 0f;
    private float smoothedDy = 0f;
    private float accumulatedX = 0f;
    private float accumulatedY = 0f;
    private boolean smoothInitialized = false;

    // Contacts currently held down on the remote for a declined gesture, with the
    // last position each was sent at so they can be released without an event.
    private static final int MAX_FORWARDED = 10;
    private final int[] forwardedIds = new int[MAX_FORWARDED];
    private final float[] forwardedX = new float[MAX_FORWARDED];
    private final float[] forwardedY = new float[MAX_FORWARDED];
    private int forwardedCount = 0;

    private final Matrix normalizeTransform = new Matrix();
    private float gestureFactor = 1f;
    private float gestureOffsetX = 0f;
    private float gestureOffsetY = 0f;
    private final float[] mappedPoint = new float[2];

    private final Output output;
    // The on-screen touchpad emits an explicit double-click sequence. Captured
    // hardware taps already arrive as separate clicks, so emitting a second
    // synthetic pair there would turn two taps into three clicks.
    private final boolean synthesizeDoubleTap;

    Touchpad(Context context, Output output, boolean synthesizeDoubleTap) {
        this.output = output;
        this.synthesizeDoubleTap = synthesizeDoubleTap;
        touchSlop = ViewConfiguration.get(context).getScaledTouchSlop();
        twoFingerTapSlop = touchSlop * TWO_FINGER_TAP_SLOP_FACTOR;
        setGestureThresholds(DEFAULT_SCROLL_THRESHOLD_FACTOR,
                DEFAULT_MOVE_THRESHOLD_FACTOR);
    }

    /**
     * Describe the coordinate space this instance's events arrive in: the view size
     * for the on-screen touchpad, the pad's motion range for a physical one.
     */
    void setInputBounds(float minX, float minY, float rangeX, float rangeY) {
        inputMinX = minX;
        inputMinY = minY;
        inputRangeX = Math.max(0f, rangeX);
        inputRangeY = Math.max(0f, rangeY);
    }

    /** Set the view size gestures are interpreted and forwarded in. */
    void setOutputSize(int width, int height) {
        if (width == outputWidth && height == outputHeight)
            return;
        outputWidth = width;
        outputHeight = height;
        resetSmoothing();
    }

    /**
     * Set the side, in output pixels, of the square a forwarded gesture is mapped
     * into. This is the gesture's magnitude: the whole input area spans this many
     * pixels, so a larger value makes the same finger movement travel further.
     */
    void setGestureScale(float scale) {
        gestureScale = Math.max(50f, Math.min(4000f, scale));
    }

    /** Set the acceleration strength (clamped to 0.5 ~ 10.0). */
    void setAccelStrength(float strength) {
        mouseAccelStrength = Math.max(0.5f, Math.min(10.0f, strength));
    }

    /** Set the two-finger scroll distance per pixel of travel (0.05 ~ 5.0). */
    void setScrollSpeed(float speed) {
        scrollSpeed = Math.max(0.05f, Math.min(5.0f, speed));
    }

    /** Invert both scroll axes ("natural" scrolling). */
    void setScrollReversed(boolean reversed) {
        scrollReversed = reversed;
    }

    /** 是否禁用双指捏合/张开与三指及以上的触摸转发手势。 */
    void setMultiFingerGesturesDisabled(boolean disabled) {
        multiFingerGesturesDisabled = disabled;
        if (disabled) {
            // A partially-observed gesture must not stay latched as "forwarded".
            multiFingerIgnored = false;
            twoFingerMode = TWO_FINGER_UNDECIDED;
        }
    }

    /**
     * Set the two-finger classification thresholds, as multiples of touchSlop.
     *
     * scrollFactor is how far a finger must travel before it counts as moving at all;
     * lower it if a pinch is being mistaken for a scroll. moveFactor is how far the
     * leading finger must travel before an ambiguous phase is resolved; lower it if
     * scrolling feels slow to engage.
     */
    void setGestureThresholds(float scrollFactor, float moveFactor) {
        scrollDistanceThreshold = touchSlop * Math.max(0.05f, Math.min(5.0f, scrollFactor));
        moveDistanceThreshold = touchSlop * Math.max(0.1f, Math.min(10.0f, moveFactor));
    }

    /** True while a declined gesture is being forwarded as touch. */
    boolean isForwardingTouch() {
        return forwardedCount > 0;
    }

    /** Cancel an in-progress gesture, e.g. when the capture window loses focus. */
    void cancel() {
        releaseForwardedTouches();
        if (isDraggingActive)
            sendButton(0x110, false);
        resetTouchpadState();
        resetSmoothing();
        lastTapTime = 0L;
        lastTapX = 0f;
        lastTapY = 0f;
    }

    /**
     * Decide left vs right for a single-button (clickpad) physical press. The
     * pressing finger is the slowest contact -- it is held still to click -- so
     * each current contact is compared against the last position recognize()
     * recorded (lastX1 for the primary finger, lastX2 for the second) and the one
     * that travelled least wins. Left half of the pad => BTN_LEFT (0x110), right
     * half => BTN_RIGHT (0x111).
     *
     * Call on the ACTION_BUTTON_PRESS event. The press triggers cancel(), which
     * does not clear the baseline, so the prior positions are still live. Returns
     * BTN_LEFT when no baseline is available (a press with no contact on the pad).
     */
    int clickpadButton(MotionEvent event) {
        if (!clickpadBaselineValid || event.getPointerCount() <= 0)
            return 0x110; // BTN_LEFT

        // A press that arrives after a single contact has already been held
        // long enough for long-press drag is intentional drag/left-click, not a
        // right-click. This also covers the case where the first physical press
        // is what interrupts the hold before the recognizer sees a MOVE.
        boolean longHeldSingleContact = currentState == STATE_ONE_FINGER
                && !isMultiFinger && event.getPointerCount() == 1
                && event.getEventTime() - downTime1 >= TOUCH_LONG_PRESS_TIMEOUT;
        if (isDraggingActive || longHeldSingleContact) {
            if (debug)
                android.util.Log.d("AnlandTouchpad",
                        "clickpad press during long hold -> BTN_LEFT");
            return 0x110;
        }

        int limit = Math.min(event.getPointerCount(), 2);
        int slowestIndex = 0;
        float slowestSpeed = Float.MAX_VALUE;
        for (int i = 0; i < limit; i++) {
            float lastX = (i == 0) ? lastX1 : lastX2;
            float lastY = (i == 0) ? lastY1 : lastY2;
            float curX = toOutputX(event.getX(i));
            float curY = toOutputY(event.getY(i));
            float dx = curX - lastX;
            float dy = curY - lastY;
            float speed = dx * dx + dy * dy;
            if (speed < slowestSpeed) {
                slowestSpeed = speed;
                slowestIndex = i;
            }
        }
        float curX = toOutputX(event.getX(slowestIndex));
        float mid = outputWidth > 0 ? outputWidth / 2f : curX;
        return curX < mid ? 0x110 : 0x111; // BTN_LEFT / BTN_RIGHT
    }

    /** Map a pad (input-space) coordinate to the output space recognize() uses. */
    private float toOutputX(float x) {
        return (inputRangeX > 0f && outputWidth > 0)
                ? (x - inputMinX) * (outputWidth / inputRangeX) : x;
    }

    private float toOutputY(float y) {
        return (inputRangeY > 0f && outputHeight > 0)
                ? (y - inputMinY) * (outputHeight / inputRangeY) : y;
    }

    /**
     * Interpret one event from this device.
     *
     * Events belonging to a gesture this class does not implement are forwarded as
     * touch instead, so nothing is silently dropped.
     */
    void onTouch(MotionEvent event) {
        MotionEvent normalized = normalizeToOutput(event);
        boolean handled;
        try {
            handled = recognize(normalized != null ? normalized : event);
        } finally {
            if (normalized != null)
                normalized.recycle();
        }
        if (!handled)
            forwardAsTouch(event);
    }

    /**
     * Scale contacts from the input space into output pixels, which is what the
     * touchSlop-based thresholds are calibrated against.
     *
     * Returns null when the two spaces already coincide — the on-screen touchpad's
     * case — so no copy is made. SOURCE_CLASS_POSITION ignores MotionEvent
     * transforms on newer Android releases, hence the temporary touchscreen source.
     */
    private MotionEvent normalizeToOutput(MotionEvent event) {
        if (inputRangeX <= 0f || inputRangeY <= 0f
                || outputWidth <= 0 || outputHeight <= 0)
            return null;
        float sx = outputWidth / inputRangeX;
        float sy = outputHeight / inputRangeY;
        if (sx == 1f && sy == 1f && inputMinX == 0f && inputMinY == 0f)
            return null;

        MotionEvent copy = MotionEvent.obtain(event);
        copy.setSource(InputDevice.SOURCE_TOUCHSCREEN);
        normalizeTransform.setScale(sx, sy);
        normalizeTransform.postTranslate(-inputMinX * sx, -inputMinY * sy);
        copy.transform(normalizeTransform);
        return copy;
    }

    /** Larger of the two by magnitude, keeping its sign (libgestures MaxMag). */
    private static float maxMag(float a, float b) {
        return Math.abs(a) > Math.abs(b) ? a : b;
    }

    /** Smaller of the two by magnitude, keeping its sign (libgestures MinMag). */
    private static float minMag(float a, float b) {
        return Math.abs(a) < Math.abs(b) ? a : b;
    }

    /**
     * Classify a two-finger phase, following the rule AOSP's touchpad gesture
     * library uses (libgestures ImmediateInterpreter::GetTwoFingerGestureType):
     * take each finger's displacement since the phase began, pick whichever axis
     * dominates, and require both fingers to have travelled the same way along it.
     *
     * A pinch moves the fingers in opposite directions, so the sign product is
     * negative and it is not a scroll. If only one finger has moved enough to
     * commit while the other is still below the scroll threshold, the phase stays
     * ONE_MOVING. This is deliberately not timed out: contact updates can lag by
     * more than a frame under load, and the next event may still prove either
     * scroll (same direction) or pinch (opposite direction).
     */
    private int classifyTwoFinger(float x1, float y1, float x2, float y2) {
        float dx1 = x1 - twoFingerStartX1;
        float dy1 = y1 - twoFingerStartY1;
        float dx2 = x2 - twoFingerStartX2;
        float dy2 = y2 - twoFingerStartY2;

        float largeDx = maxMag(dx1, dx2);
        float largeDy = maxMag(dy1, dy2);
        float large;
        float small;
        if (Math.abs(largeDx) > Math.abs(largeDy)) {
            large = largeDx;
            small = minMag(dx1, dx2);
        } else {
            large = largeDy;
            small = minMag(dy1, dy2);
        }
        if (Math.abs(small) < scrollDistanceThreshold)
            small = 0f;

        // Both fingers already agree on a direction: this is a scroll as soon
        // as the leading finger passes the scroll threshold.
        if (small != 0f && large * small > 0f) {
            return Math.abs(large) < scrollDistanceThreshold
                    ? TWO_FINGER_UNDECIDED : TWO_FINGER_SCROLL;
        }

        // Not enough motion to classify yet.
        if (Math.abs(large) < moveDistanceThreshold)
            return TWO_FINGER_UNDECIDED;

        // One contact has committed while the other has not updated far enough.
        // Wait for the lagging contact rather than converting a slow event stream
        // into a permanent pinch decision.
        if (small == 0f)
            return TWO_FINGER_ONE_MOVING;

        // Both contacts have moved far enough in opposite directions: a pinch.
        return TWO_FINGER_NOT_SCROLL;
    }

    /**
     * Give up on the current gesture: release anything in progress and latch the
     * unhandled flag so every remaining event is forwarded, including the final UP.
     * The forwarding path needs that whole tail to release its touch points.
     */
    private boolean declineGesture() {
        if (isDraggingActive)
            sendButton(0x110, false);
        isDraggingActive = false;
        isSingleTapCandidate = false;
        isTwoFingerTapCandidate = false;
        isLongPressPossible = false;
        gestureUnhandled = true;
        resetSmoothing();
        return false;
    }

    private void sendButton(int button, boolean pressed) {
        if (debug)
            android.util.Log.d("AnlandTouchpad", "synthetic button=" + button
                    + " pressed=" + pressed + " t=" + gestureEventTime);
        if (output != null)
            output.onButton(button, pressed);
    }

    /**
     * Track the largest excursion made by either contact during a two-finger
     * tap. A dedicated tolerance avoids rejecting a normal tap because one
     * sample briefly exceeded the single-finger slop. The maximum is used rather
     * than the final displacement so a scroll that wanders back to its origin
     * still cannot become a right click.
     */
    private void updateTwoFingerTapTravel(MotionEvent event) {
        int action = event.getActionMasked();
        if (currentState != STATE_TWO_FINGER || event.getPointerCount() != 2
                || (action != MotionEvent.ACTION_MOVE
                    && action != MotionEvent.ACTION_POINTER_UP))
            return;
        float travel1 = (float) Math.hypot(event.getX(0) - twoFingerStartX1,
                event.getY(0) - twoFingerStartY1);
        float travel2 = (float) Math.hypot(event.getX(1) - twoFingerStartX2,
                event.getY(1) - twoFingerStartY2);
        twoFingerMaxTravel = Math.max(twoFingerMaxTravel,
                Math.max(travel1, travel2));
        if (twoFingerMaxTravel > twoFingerTapSlop)
            isTwoFingerTapCandidate = false;
        if (debug && twoFingerMaxTravel > twoFingerTapSlop)
            android.util.Log.d("AnlandTouchpad", "two-finger tap rejected: travel="
                    + twoFingerMaxTravel + " slop=" + twoFingerTapSlop);
    }

    private void sendScroll(int axis, float value) {
        if (output != null)
            output.onScroll(axis, value);
    }

    private void sendMotion(float dx, float dy) {
        if (output != null)
            output.onMotion(dx, dy);
    }

    // ==================== 触摸板手势及辅助方法 ====================
    /**
     * Run the gesture state machine over contacts already in output coordinates.
     *
     * @return true when this class consumed the event, false when the gesture is not
     *         one it implements. A false result latches for the remainder of the
     *         gesture, so the whole stream through the final UP is forwarded.
     */
    private boolean recognize(MotionEvent event) {
        int action = event.getActionMasked();
        int pointerCount = event.getPointerCount();
        gestureEventTime = event.getEventTime();
        if (action == MotionEvent.ACTION_DOWN) {
            scrollInStream = false;
            streamDownTime = gestureEventTime;
        }
        // Keep the guard active until the last scrolling contact lifts, even if
        // one finger lifts/rejoins or the hardware briefly reports no contacts.
        if (scrollInStream) extendScrollGuard();
        if (debug)
            android.util.Log.d("AnlandTouchpad", "action=" + action + " n=" + pointerCount
                    + " t=" + gestureEventTime + " scrolling=" + scrollInStream);

        // A moved two-finger gesture is never a right tap once it leaves the
        // tap tolerance. Inspect releases too: final positions can arrive with
        // the lift. Scroll/pinch classification has its own thresholds.
        updateTwoFingerTapTravel(event);

        // Keep clickpadBaselineValid current on every contact event: a physical
        // clickpad press cancels the gesture before recognize() sees it, so this
        // is the only place the flag is maintained.
        if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN
                || action == MotionEvent.ACTION_MOVE)
            clickpadBaselineValid = true;
        else if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL)
            clickpadBaselineValid = false;

        // Once declined, stay declined: the gesture is mid-way through being
        // forwarded as touch and its pointer-up events still have to get there.
        if (gestureUnhandled) {
            if (action == MotionEvent.ACTION_UP
                    || action == MotionEvent.ACTION_CANCEL) {
                resetTouchpadState();
                resetSmoothing();
            }
            return false;
        }

        // 三指及以上手势被禁用时，吞掉整个流，直到手指减到一根（此时落回
        // ACTION_POINTER_UP 的正常处理恢复单指控制）。
        if (multiFingerIgnored) {
            if (action == MotionEvent.ACTION_POINTER_UP
                    && (event.getPointerCount() - 1) <= 1) {
                multiFingerIgnored = false;
                // fall through to the ACTION_POINTER_UP branch below
            } else if (action == MotionEvent.ACTION_UP
                    || action == MotionEvent.ACTION_CANCEL) {
                multiFingerIgnored = false;
                resetTouchpadState();
                resetSmoothing();
                return true;
            } else {
                return true; // swallow
            }
        }

        switch (action) {
            case MotionEvent.ACTION_DOWN: {
                float x = event.getX();
                float y = event.getY();
                startX1 = lastX1 = x;
                startY1 = lastY1 = y;
                downTime1 = event.getEventTime();
                hasLongPressed = false;
                isLongPressPossible = true;
                isSingleTapCandidate = true;
                isTwoFingerTapCandidate = false;
                isDraggingActive = false;
                isMultiFinger = false;
                currentState = STATE_ONE_FINGER;
                twoFingerMode = TWO_FINGER_UNDECIDED;
                twoFingerMaxTravel = 0f;
                resetSmoothing();
                break;
            }
            case MotionEvent.ACTION_POINTER_DOWN: {
                isMultiFinger = true;
                isSingleTapCandidate = false;
                isLongPressPossible = false;
                if (currentState == STATE_DRAGGING) {
                    sendButton(0x110, false);
                    isDraggingActive = false;
                }
                if (pointerCount == 2) {
                    currentState = STATE_TWO_FINGER;
                    isTwoFingerTapCandidate = true;
                    lastX1 = twoFingerStartX1 = event.getX(0);
                    lastY1 = twoFingerStartY1 = event.getY(0);
                    lastX2 = twoFingerStartX2 = event.getX(1);
                    lastY2 = twoFingerStartY2 = event.getY(1);
                    twoFingerMode = TWO_FINGER_UNDECIDED;
                    twoFingerMaxTravel = 0f;
                } else if (pointerCount >= 3) {
                    if (multiFingerGesturesDisabled) {
                        // 三指及以上手势被禁用，吞掉。
                        multiFingerIgnored = true;
                        isMultiFinger = true;
                    } else {
                        // Three or more fingers is never one of ours.
                        return declineGesture();
                    }
                }
                break;
            }
            case MotionEvent.ACTION_MOVE: {
                if (pointerCount == 1 && !isMultiFinger) {
                    float x = event.getX();
                    float y = event.getY();
                    float rawDx = x - lastX1;
                    float rawDy = y - lastY1;
                    float dist = (float) Math.hypot(x - startX1, y - startY1);

                    // Check the hold timeout before the movement slop. A pad may
                    // not report any MOVE while the finger is stationary, so the
                    // first event after the 500 ms hold can already be the drag
                    // movement. Cancelling the candidate first made that first
                    // movement abort the long press. Once the hold has matured,
                    // the drag wins over the tap/motion classification.
                    if (tapToClick && isLongPressPossible && !hasLongPressed
                            && (event.getEventTime() - downTime1) >= TOUCH_LONG_PRESS_TIMEOUT) {
                        hasLongPressed = true;
                        currentState = STATE_DRAGGING;
                        isDraggingActive = true;
                        isSingleTapCandidate = false;
                        isTwoFingerTapCandidate = false;
                        // No motion is emitted with the press: the cursor belongs to
                        // the host and is already where the drag should start.
                        sendButton(0x110, true);
                        if (debug)
                            android.util.Log.d("AnlandTouchpad",
                                    "long-press drag down t=" + event.getEventTime());
                        resetSmoothing();
                        break;
                    }

                    if (dist > touchSlop) {
                        isLongPressPossible = false;
                        isSingleTapCandidate = false;
                        // Cleared once the remaining finger actually moves, so a
                        // scroll or drag whose second finger lifted early cannot
                        // finish as a right-click.
                        isTwoFingerTapCandidate = false;
                    }

                    float[] smoothed = applySmoothing(rawDx, rawDy);
                    float smoothDx = smoothed[0];
                    float smoothDy = smoothed[1];

                    if (smoothDx != 0f || smoothDy != 0f) {
                        // 计算移动距离（平滑后的欧式距离）
                        float distance = (float) Math.hypot(smoothDx, smoothDy);

                        // 改进的加速度曲线：以 10px 为参考阈值，使小位移也能获得明显加速
                        float speedFactor = distance / 10.0f;
                        // 使用 sigmoid-like 曲线：scale = 1 + (strength - 1) * (speed / (1 + speed))
                        float dynamicScale = 1.0f + (mouseAccelStrength - 1.0f) * (speedFactor / (1.0f + speedFactor));
                        // 限制范围，防止失控（最大不超过 10 倍）
                        dynamicScale = Math.max(0.3f, Math.min(10.0f, dynamicScale));

                        float moveX = smoothDx * dynamicScale;
                        float moveY = smoothDy * dynamicScale;
                        sendMotion(moveX, moveY);
                    }

                    lastX1 = x;
                    lastY1 = y;

                } else if (pointerCount == 2) {
                    if (currentState == STATE_TWO_FINGER) {
                        float x1 = event.getX(0);
                        float y1 = event.getY(0);
                        float x2 = event.getX(1);
                        float y2 = event.getY(1);

                        if (twoFingerMode == TWO_FINGER_UNDECIDED
                                || twoFingerMode == TWO_FINGER_ONE_MOVING) {
                            int classified = classifyTwoFinger(x1, y1, x2, y2);
                            if (classified == TWO_FINGER_NOT_SCROLL) {
                                if (multiFingerGesturesDisabled) {
                                    // 双指捏合/张开被禁用，吞掉这一阶段
                                    //（不滚动也不转发为触摸）。
                                    twoFingerMode = TWO_FINGER_IGNORED;
                                } else {
                                    // Both contacts have committed in opposite
                                    // directions. Hand the gesture to the
                                    // forwarding path.
                                    return declineGesture();
                                }
                            } else if (classified != TWO_FINGER_UNDECIDED) {
                                // SCROLL starts immediately; ONE_MOVING stays
                                // pending so a later contact update can still
                                // prove that the contacts agree.
                                twoFingerMode = classified;
                            }
                        }

                        if (twoFingerMode == TWO_FINGER_IGNORED) {
                            // Swallow the pinch: keep the anchors current so the
                            // next touch that resumes normal input does not see
                            // one giant delta.
                            lastX1 = x1;
                            lastY1 = y1;
                            lastX2 = x2;
                            lastY2 = y2;
                        } else if (twoFingerMode == TWO_FINGER_SCROLL) {
                            // Classification itself commits the stream to scroll;
                            // subpixel deltas must never leave a right-tap armed.
                            markScroll();
                            float avgDx = ((x1 - lastX1) + (x2 - lastX2)) / 2;
                            float avgDy = ((y1 - lastY1) + (y2 - lastY2)) / 2;

                            if (Math.abs(avgDx) > 1 || Math.abs(avgDy) > 1) {
                                isTwoFingerTapCandidate = false;
                                float scale = scrollReversed
                                        ? -scrollSpeed : scrollSpeed;
                                if (Math.abs(avgDy)
                                        > Math.abs(avgDx) * AXIS_DOMINANCE_RATIO) {
                                    sendScroll(0, -avgDy * scale);
                                }
                                if (Math.abs(avgDx)
                                        > Math.abs(avgDy) * AXIS_DOMINANCE_RATIO) {
                                    sendScroll(1, avgDx * scale);
                                }
                                lastX1 = x1;
                                lastY1 = y1;
                                lastX2 = x2;
                                lastY2 = y2;
                            }
                        } else {
                            // Still ambiguous, so emit nothing yet, but keep the
                            // anchors current: the first scroll delta after the
                            // decision must not be the whole accumulated drift.
                            lastX1 = x1;
                            lastY1 = y1;
                            lastX2 = x2;
                            lastY2 = y2;
                        }
                    }
                }
                break;
            }
            case MotionEvent.ACTION_POINTER_UP: {
                int remaining = pointerCount - 1;
                if (remaining == 1) {
                    isMultiFinger = false;
                    isSingleTapCandidate = false;
                    isLongPressPossible = false;
                    int idx = (event.getActionIndex() == 0) ? 1 : 0;
                    lastX1 = event.getX(idx);
                    lastY1 = event.getY(idx);
                    startX1 = lastX1;
                    startY1 = lastY1;
                    downTime1 = event.getEventTime();
                    hasLongPressed = false;
                    currentState = STATE_ONE_FINGER;
                    resetSmoothing();
                }
                break;
            }
            case MotionEvent.ACTION_UP: {
                long duration = event.getEventTime() - streamDownTime;
                boolean isQuickTap = duration < 300;

                if (isDraggingActive) {
                    sendButton(0x110, false);
                    isDraggingActive = false;
                    resetTouchpadState();
                    resetSmoothing();
                    return true;
                }

                if (tapAllowed(event.getEventTime()) && isTwoFingerTapCandidate
                        && twoFingerMaxTravel <= twoFingerTapSlop && isQuickTap) {
                    sendButton(0x111, true);
                    sendButton(0x111, false);
                    resetTouchpadState();
                    resetSmoothing();
                    return true;
                }

                if (tapAllowed(event.getEventTime()) && currentState == STATE_ONE_FINGER
                        && isSingleTapCandidate && isQuickTap) {
                    long gap = event.getEventTime() - lastTapTime;
                    float dist = (float) Math.hypot(lastX1 - lastTapX, lastY1 - lastTapY);
                    if (synthesizeDoubleTap && gap < 300 && dist < touchSlop
                            && !isDoubleTapPending) {
                        isDoubleTapPending = true;
                        sendButton(0x110, true);
                        sendButton(0x110, false);
                        sendButton(0x110, true);
                        sendButton(0x110, false);
                        isDoubleTapPending = false;
                        lastTapTime = 0;
                    } else {
                        sendButton(0x110, true);
                        sendButton(0x110, false);
                        lastTapTime = event.getEventTime();
                        lastTapX = lastX1;
                        lastTapY = lastY1;
                        isDoubleTapPending = false;
                    }
                    resetTouchpadState();
                    resetSmoothing();
                    return true;
                }
                resetTouchpadState();
                resetSmoothing();
                break;
            }
            case MotionEvent.ACTION_CANCEL: {
                if (isDraggingActive) {
                    sendButton(0x110, false);
                    isDraggingActive = false;
                }
                resetTouchpadState();
                resetSmoothing();
                break;
            }
        }
        return true;
    }

    // ==================== 未处理手势 → 触摸转发 ====================
    /**
     * Forward a declined gesture (a pinch, or three or more fingers) as touch.
     *
     * Contacts are mapped into a square of {@link #gestureScale} output pixels
     * centred on the cursor. Spreading the input area over the whole output would be
     * wrong for a gesture: the input's aspect ratio is not the output's, so the
     * gesture came out distorted, and it covered the entire output however small the
     * real finger movement was. A square around the cursor puts the gesture where
     * the user is looking and makes its magnitude one setting. The scale is uniform
     * on both axes, so the input's own aspect ratio survives inside that square and
     * a pinch behaves the same whichever way the fingers move.
     */
    private void forwardAsTouch(MotionEvent event) {
        if (output == null)
            return;
        int action = event.getActionMasked();
        if (action == MotionEvent.ACTION_CANCEL) {
            releaseForwardedTouches();
            return;
        }
        if (!computeGestureTransform())
            return;

        int upIndex = (action == MotionEvent.ACTION_UP
                || action == MotionEvent.ACTION_POINTER_UP)
                ? event.getActionIndex() : -1;
        boolean sent = false;

        for (int i = 0; i < event.getPointerCount(); i++) {
            if (i == upIndex)
                continue;
            int id = event.getPointerId(i);
            mapToCursorSquare(event.getX(i), event.getY(i));
            int slot = forwardedSlot(id);
            if (slot < 0) {
                // Catch-up down. The fingers already on the device were read as
                // cursor motion, or as a two-finger phase that had not been decided
                // yet, so the remote has never seen them go down — and a compositor
                // drops motion for a contact id it never saw, which would leave a
                // pinch showing a single finger.
                if (addForwarded(id, mappedPoint[0], mappedPoint[1])) {
                    output.onTouch(TOUCH_DOWN, id, mappedPoint[0], mappedPoint[1]);
                    sent = true;
                }
            } else if (action == MotionEvent.ACTION_MOVE) {
                forwardedX[slot] = mappedPoint[0];
                forwardedY[slot] = mappedPoint[1];
                output.onTouch(TOUCH_MOVE, id, mappedPoint[0], mappedPoint[1]);
                sent = true;
            }
        }

        if (upIndex >= 0) {
            int id = event.getPointerId(upIndex);
            mapToCursorSquare(event.getX(upIndex), event.getY(upIndex));
            if (removeForwarded(id)) {
                output.onTouch(TOUCH_UP, id, mappedPoint[0], mappedPoint[1]);
                sent = true;
            }
        }

        if (sent)
            output.onTouchFrame();
    }

    /**
     * Release every contact still held down for a forwarded gesture. Needed when the
     * gesture is cut short before its own UP arrives — a physical pad button, a
     * cancelled stream, or lost capture — otherwise they stay down on the remote for
     * good.
     */
    private void releaseForwardedTouches() {
        if (forwardedCount == 0 || output == null)
            return;
        for (int i = 0; i < forwardedCount; i++)
            output.onTouch(TOUCH_UP, forwardedIds[i], forwardedX[i], forwardedY[i]);
        forwardedCount = 0;
        output.onTouchFrame();
    }

    /** Centre the square on the cursor. False when there is nothing to map into. */
    private boolean computeGestureTransform() {
        if (inputRangeX <= 0f || inputRangeY <= 0f)
            return false;
        float cursorX = output.cursorX();
        float cursorY = output.cursorY();
        if (!Float.isFinite(cursorX) || !Float.isFinite(cursorY))
            return false;
        // The longer input axis spans the full square; the shorter keeps its ratio.
        gestureFactor = gestureScale / Math.max(inputRangeX, inputRangeY);
        gestureOffsetX = cursorX - (inputMinX + inputRangeX / 2f) * gestureFactor;
        gestureOffsetY = cursorY - (inputMinY + inputRangeY / 2f) * gestureFactor;
        return true;
    }

    private void mapToCursorSquare(float x, float y) {
        mappedPoint[0] = x * gestureFactor + gestureOffsetX;
        mappedPoint[1] = y * gestureFactor + gestureOffsetY;
    }

    private int forwardedSlot(int pointerId) {
        for (int i = 0; i < forwardedCount; i++) {
            if (forwardedIds[i] == pointerId)
                return i;
        }
        return -1;
    }

    private boolean addForwarded(int pointerId, float x, float y) {
        if (forwardedCount >= MAX_FORWARDED)
            return false;
        forwardedIds[forwardedCount] = pointerId;
        forwardedX[forwardedCount] = x;
        forwardedY[forwardedCount] = y;
        forwardedCount++;
        return true;
    }

    private boolean removeForwarded(int pointerId) {
        int slot = forwardedSlot(pointerId);
        if (slot < 0)
            return false;
        forwardedCount--;
        for (int i = slot; i < forwardedCount; i++) {
            forwardedIds[i] = forwardedIds[i + 1];
            forwardedX[i] = forwardedX[i + 1];
            forwardedY[i] = forwardedY[i + 1];
        }
        return true;
    }

    private void resetTouchpadState() {
        currentState = STATE_IDLE;
        isSingleTapCandidate = false;
        isTwoFingerTapCandidate = false;
        isDoubleTapPending = false;
        hasLongPressed = false;
        isDraggingActive = false;
        isLongPressPossible = false;
        isMultiFinger = false;
        twoFingerMode = TWO_FINGER_UNDECIDED;
        twoFingerMaxTravel = 0f;
        scrollInStream = false;
        streamDownTime = 0L;
        multiFingerIgnored = false;
        // Cleared here rather than in declineGesture: the latch has to outlive the
        // events that follow it and only lifts when the gesture itself ends.
        gestureUnhandled = false;
    }

    private void resetSmoothing() {
        smoothedDx = 0f;
        smoothedDy = 0f;
        accumulatedX = 0f;
        accumulatedY = 0f;
        smoothInitialized = false;
    }

    private float[] applySmoothing(float rawDx, float rawDy) {
        float deadDx = Math.abs(rawDx) < DEAD_ZONE ? 0f : rawDx;
        float deadDy = Math.abs(rawDy) < DEAD_ZONE ? 0f : rawDy;

        if (deadDx == 0f && deadDy == 0f) {
            return new float[]{0f, 0f};
        }

        if (!smoothInitialized) {
            smoothedDx = deadDx;
            smoothedDy = deadDy;
            smoothInitialized = true;
        } else {
            smoothedDx = SMOOTHING_FACTOR * deadDx + (1 - SMOOTHING_FACTOR) * smoothedDx;
            smoothedDy = SMOOTHING_FACTOR * deadDy + (1 - SMOOTHING_FACTOR) * smoothedDy;
        }

        // 累积阈值大幅降低，让移动更加连续
        accumulatedX += smoothedDx;
        accumulatedY += smoothedDy;

        float outX = 0f;
        float outY = 0f;
        if (Math.abs(accumulatedX) >= ACCUMULATED_THRESHOLD) {
            outX = accumulatedX;
            accumulatedX = 0f;
        }
        if (Math.abs(accumulatedY) >= ACCUMULATED_THRESHOLD) {
            outY = accumulatedY;
            accumulatedY = 0f;
        }
        return new float[]{outX, outY};
    }
}
