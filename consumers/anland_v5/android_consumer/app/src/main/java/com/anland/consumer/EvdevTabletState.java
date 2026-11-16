package com.anland.consumer;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;

/** Decodes raw Linux tablet axes/buttons from a grabbed touchscreen device. */
final class EvdevTabletState {
    static final int ABS_X = 0x00, ABS_Y = 0x01;
    private static final int ABS_PRESSURE = 0x18, ABS_DISTANCE = 0x19;
    private static final int ABS_TILT_X = 0x1a, ABS_TILT_Y = 0x1b;
    private static final int ABS_MT_SLOT = 0x2f;
    static final int ABS_MT_POSITION_X = 0x35, ABS_MT_POSITION_Y = 0x36;
    private static final int ABS_MT_TOOL_TYPE = 0x37, ABS_MT_TRACKING_ID = 0x39;
    private static final int ABS_MT_PRESSURE = 0x3a, ABS_MT_DISTANCE = 0x3b;
    private static final int MT_TOOL_PEN = 1;

    private static final int BTN_TOOL_PEN = 0x140, BTN_TOOL_RUBBER = 0x141;
    private static final int BTN_TOUCH = 0x14a, BTN_STYLUS = 0x14b, BTN_STYLUS2 = 0x14c;
    private static final int BTN_TOOL_FINGER = 0x145;

    static final class Sample {
        final int pointerId;
        final int toolType;
        final int capabilities;
        final int coordinateAxisX, coordinateAxisY;
        final float x, y, pressure, tiltX, tiltY, distance;
        final boolean proximity, tipDown;
        final int tipState, buttons;
        final long timeMsec;

        Sample(int pointerId, int toolType, int capabilities,
               int coordinateAxisX, int coordinateAxisY, float x, float y,
               float pressure, float tiltX, float tiltY, float distance,
               boolean proximity, boolean tipDown, int tipState, int buttons,
               long timeMsec) {
            this.pointerId = pointerId;
            this.toolType = toolType;
            this.capabilities = capabilities;
            this.coordinateAxisX = coordinateAxisX;
            this.coordinateAxisY = coordinateAxisY;
            this.x = x;
            this.y = y;
            this.pressure = pressure;
            this.tiltX = tiltX;
            this.tiltY = tiltY;
            this.distance = distance;
            this.proximity = proximity;
            this.tipDown = tipDown;
            this.tipState = tipState;
            this.buttons = buttons;
            this.timeMsec = timeMsec;
        }

        Sample outOfProximity(long time) {
            return new Sample(pointerId, toolType, capabilities,
                    coordinateAxisX, coordinateAxisY, x, y, 0f,
                    tiltX, tiltY, distance, false, false,
                    TabletInput.TIP_STATE_UP, 0, time);
        }
    }

    private final int[] min = new int[0x40];
    private final int[] max = new int[0x40];
    private final boolean[] hasRange = new boolean[0x40];
    private final int[] trackingId = new int[EvdevTouchState.MAX_CONTACTS];
    private final int[] toolType = new int[EvdevTouchState.MAX_CONTACTS];
    private final int[] mtX = new int[EvdevTouchState.MAX_CONTACTS];
    private final int[] mtY = new int[EvdevTouchState.MAX_CONTACTS];
    private final int[] mtPressure = new int[EvdevTouchState.MAX_CONTACTS];
    private final int[] mtDistance = new int[EvdevTouchState.MAX_CONTACTS];
    private final boolean[] hasMtX = new boolean[EvdevTouchState.MAX_CONTACTS];
    private final boolean[] hasMtY = new boolean[EvdevTouchState.MAX_CONTACTS];
    private final boolean[] hasMtPressure = new boolean[EvdevTouchState.MAX_CONTACTS];
    private final boolean[] hasMtDistance = new boolean[EvdevTouchState.MAX_CONTACTS];
    private final Sample[] active = new Sample[EvdevTouchState.MAX_CONTACTS];

    private int slot;
    private int x, y, pressure, distance, tiltX, tiltY;
    private boolean hasX, hasY, hasPressure, hasDistance, hasTiltX, hasTiltY;
    private boolean penInProximity, eraserInProximity, fingerInProximity;
    private boolean touchKnown, touching, stylusButton, stylusButton2;

    EvdevTabletState() {
        Arrays.fill(trackingId, -1);
    }

    void setRange(int axis, int minimum, int maximum) {
        if (axis < 0 || axis >= hasRange.length || maximum <= minimum)
            return;
        min[axis] = minimum;
        max[axis] = maximum;
        hasRange[axis] = true;
    }

    boolean hasRange(int axis) {
        return axis >= 0 && axis < hasRange.length && hasRange[axis];
    }

    int rangeMin(int axis) {
        return axis >= 0 && axis < min.length ? min[axis] : 0;
    }

    int rangeMax(int axis) {
        return axis >= 0 && axis < max.length ? max[axis] : 0;
    }

    /** Map one absolute axis into another device coordinate range. */
    static float mapCoordinate(float value, int sourceMin, int sourceMax,
                               int targetMin, int targetMax) {
        if (sourceMax <= sourceMin || targetMax <= targetMin)
            return value;
        float normalized = (value - sourceMin) / (float) (sourceMax - sourceMin);
        normalized = Math.max(0f, Math.min(1f, normalized));
        return targetMin + normalized * (targetMax - targetMin);
    }

    boolean key(int code, int value) {
        boolean pressed = value != 0;
        switch (code) {
            case BTN_TOOL_PEN: penInProximity = pressed; return true;
            case BTN_TOOL_RUBBER: eraserInProximity = pressed; return true;
            case BTN_TOOL_FINGER: fingerInProximity = pressed; return true;
            case BTN_TOUCH: touchKnown = true; touching = pressed; return true;
            case BTN_STYLUS: stylusButton = pressed; return true;
            case BTN_STYLUS2: stylusButton2 = pressed; return true;
            default: return false;
        }
    }

    boolean abs(int code, int value) {
        switch (code) {
            case ABS_MT_SLOT:
                if (value >= 0 && value < trackingId.length)
                    slot = value;
                return true;
            case ABS_MT_TRACKING_ID: trackingId[slot] = value; return true;
            case ABS_MT_TOOL_TYPE: toolType[slot] = value; return true;
            case ABS_MT_POSITION_X: mtX[slot] = value; hasMtX[slot] = true; return true;
            case ABS_MT_POSITION_Y: mtY[slot] = value; hasMtY[slot] = true; return true;
            case ABS_MT_PRESSURE:
                mtPressure[slot] = value; hasMtPressure[slot] = true; return true;
            case ABS_MT_DISTANCE:
                mtDistance[slot] = value; hasMtDistance[slot] = true; return true;
            case ABS_X: x = value; hasX = true; return true;
            case ABS_Y: y = value; hasY = true; return true;
            case ABS_PRESSURE: pressure = value; hasPressure = true; return true;
            case ABS_DISTANCE: distance = value; hasDistance = true; return true;
            case ABS_TILT_X: tiltX = value; hasTiltX = true; return true;
            case ABS_TILT_Y: tiltY = value; hasTiltY = true; return true;
            default: return false;
        }
    }

    /** Commit all current pen slots and one explicit proximity-out per departed slot. */
    List<Sample> endFrame(long timeMsec) {
        Sample[] current = new Sample[active.length];
        int capabilities = capabilities();
        int buttons = (stylusButton ? TabletInput.BUTTON_PRIMARY : 0)
                | (stylusButton2 ? TabletInput.BUTTON_SECONDARY : 0);
        boolean foundMtPen = false;

        for (int i = 0; i < active.length; i++) {
            boolean trackedPen = toolType[i] == MT_TOOL_PEN && trackingId[i] >= 0;
            // The current MT slot may now be a finger slot while a pen remains
            // in proximity in another slot. Keep reporting the last pen slot
            // until the tool's BTN_TOOL_* proximity state goes up.
            boolean hoveringPenSlot = toolType[i] == MT_TOOL_PEN
                    && (penInProximity || eraserInProximity);
            if (!trackedPen && !hoveringPenSlot)
                continue;
            Sample sample = makeSample(i, true, trackedPen, capabilities, buttons, timeMsec);
            if (sample != null) {
                current[i] = sample;
                foundMtPen = true;
            }
        }

        if (!foundMtPen && (penInProximity || eraserInProximity)
                && hasX && hasY) {
            int pointer = slot >= 0 && slot < toolType.length
                    && toolType[slot] == MT_TOOL_PEN ? slot : 0;
            current[pointer] = makeSample(pointer, false, false,
                    capabilities, buttons, timeMsec);
        }

        List<Sample> result = new ArrayList<>();
        for (int i = 0; i < current.length; i++) {
            if (current[i] != null) {
                active[i] = current[i];
                result.add(current[i]);
            } else if (active[i] != null) {
                result.add(active[i].outOfProximity(timeMsec));
                active[i] = null;
            }
        }
        return result;
    }

    void clear() {
        Arrays.fill(trackingId, -1);
        Arrays.fill(toolType, 0);
        Arrays.fill(hasMtX, false);
        Arrays.fill(hasMtY, false);
        Arrays.fill(hasMtPressure, false);
        Arrays.fill(hasMtDistance, false);
        Arrays.fill(active, null);
        hasX = hasY = hasPressure = hasDistance = hasTiltX = hasTiltY = false;
        penInProximity = eraserInProximity = fingerInProximity = false;
        touchKnown = touching = false;
        stylusButton = stylusButton2 = false;
        slot = 0;
    }

    private int capabilities() {
        int result = 0;
        if (hasRange[ABS_PRESSURE] || hasRange[ABS_MT_PRESSURE])
            result |= TabletInput.CAP_PRESSURE;
        if (hasRange[ABS_TILT_X] && hasRange[ABS_TILT_Y])
            result |= TabletInput.CAP_TILT;
        if (hasRange[ABS_DISTANCE] || hasRange[ABS_MT_DISTANCE])
            result |= TabletInput.CAP_DISTANCE;
        return result;
    }

    private Sample makeSample(int pointer, boolean multitouch, boolean tracked,
                              int capabilities, int buttons, long timeMsec) {
        boolean useMtPosition = multitouch && hasMtX[pointer] && hasMtY[pointer];
        boolean positionAvailable = useMtPosition || hasX && hasY;
        if (!positionAvailable)
            return null;
        int rawX = useMtPosition ? mtX[pointer] : x;
        int rawY = useMtPosition ? mtY[pointer] : y;
        boolean hasRawPressure = multitouch && hasMtPressure[pointer] || hasPressure;
        int rawPressure = multitouch && hasMtPressure[pointer]
                ? mtPressure[pointer] : pressure;
        int pressureAxis = multitouch && hasMtPressure[pointer]
                ? ABS_MT_PRESSURE : ABS_PRESSURE;
        float normalizedPressure = (capabilities & TabletInput.CAP_PRESSURE) != 0
                && hasRawPressure ? normalize(pressureAxis, rawPressure) : 0f;

        int distanceAxis = multitouch && hasMtDistance[pointer]
                ? ABS_MT_DISTANCE : ABS_DISTANCE;
        boolean hasRawDistance = multitouch && hasMtDistance[pointer] || hasDistance;
        int rawDistance = multitouch && hasMtDistance[pointer] ? mtDistance[pointer] : distance;
        float normalizedDistance = (capabilities & TabletInput.CAP_DISTANCE) != 0
                && hasRawDistance ? normalize(distanceAxis, rawDistance) : 0f;
        boolean hoveringByDistance = (capabilities & TabletInput.CAP_DISTANCE) != 0
                && hasRawDistance && hasRange[distanceAxis]
                && rawDistance > min[distanceAxis];
        float tiltXValue = (capabilities & TabletInput.CAP_TILT) != 0 && hasTiltX
                ? tiltAngle(ABS_TILT_X, tiltX) : 0f;
        float tiltYValue = (capabilities & TabletInput.CAP_TILT) != 0 && hasTiltY
                ? tiltAngle(ABS_TILT_Y, tiltY) : 0f;

        boolean inRange = tracked || penInProximity || eraserInProximity;
        // BTN_TOUCH is device-wide. On a shared pen+finger node it can describe
        // a finger while the pen hovers, so use it for MT tools only when there
        // is no concurrent finger slot. A tracked MT pen slot is itself an
        // explicit per-pointer contact signal, including a zero-pressure first
        // sample.
        boolean useDeviceTouch = touchKnown && !fingerInProximity
                && (!multitouch || !hasFingerContact());
        int tipState;
        if (hoveringByDistance) {
            // Tracking ids can remain allocated while a stylus hovers. The
            // distance axis is the per-tool signal and takes precedence over
            // a device-wide BTN_TOUCH that may belong to a concurrent finger.
            tipState = TabletInput.TIP_STATE_UP;
        } else if (multitouch && tracked) {
            tipState = useDeviceTouch && !touching ? TabletInput.TIP_STATE_UP
                    : TabletInput.TIP_STATE_DOWN;
        } else if (useDeviceTouch) {
            tipState = touching ? TabletInput.TIP_STATE_DOWN : TabletInput.TIP_STATE_UP;
        } else {
            tipState = TabletInput.TIP_STATE_UNKNOWN;
        }
        boolean tipDown = tipState == TabletInput.TIP_STATE_DOWN
                || tipState == TabletInput.TIP_STATE_UNKNOWN && normalizedPressure > 0f;
        int type = eraserInProximity ? TabletInput.TOOL_ERASER : TabletInput.TOOL_PEN;
        return new Sample(pointer, type, capabilities,
                useMtPosition ? ABS_MT_POSITION_X : ABS_X,
                useMtPosition ? ABS_MT_POSITION_Y : ABS_Y, rawX, rawY,
                normalizedPressure, tiltXValue, tiltYValue, normalizedDistance,
                inRange, tipDown, tipState, buttons, timeMsec);
    }

    private float normalize(int axis, int value) {
        if (!hasRange[axis] || max[axis] <= min[axis])
            return 0f;
        float normalized = (value - min[axis]) / (float) (max[axis] - min[axis]);
        return Math.max(0f, Math.min(1f, normalized));
    }

    private float tiltAngle(int axis, int value) {
        // Android's evdev mapping specifies ABS_TILT_X/Y in degrees from
        // perpendicular, with zero at the upright position.
        return Math.max(-90f, Math.min(90f, value));
    }

    private boolean hasFingerContact() {
        if (fingerInProximity)
            return true;
        for (int i = 0; i < trackingId.length; i++) {
            if (trackingId[i] >= 0 && toolType[i] == 0)
                return true;
        }
        return false;
    }
}
