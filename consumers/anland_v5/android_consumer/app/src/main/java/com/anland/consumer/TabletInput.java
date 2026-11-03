package com.anland.consumer;

import java.util.HashMap;
import java.util.Iterator;
import java.util.Map;

/** Tracks active pen tools and commits each sample as one native tablet frame. */
final class TabletInput {
    static final int TOOL_PEN = 1;
    static final int TOOL_ERASER = 2;

    static final int TIP_STATE_UNKNOWN = 0;
    static final int TIP_STATE_DOWN = 1;
    static final int TIP_STATE_UP = 2;

    static final int CAP_PRESSURE = 1;
    static final int CAP_TILT = 2;
    static final int CAP_DISTANCE = 4;

    static final int FLAG_PROXIMITY = 1;
    static final int FLAG_TIP_DOWN = 2;
    static final int FLAG_CANCELED = 4;

    static final int BUTTON_PRIMARY = 1;
    static final int BUTTON_SECONDARY = 2;

    interface Sender {
        void send(Frame frame);
    }

    /** One timestamped Android sample before assigning its stable active-tool id. */
    static final class Sample {
        final int deviceId;
        final int pointerId;
        final int toolType;
        final int capabilities;
        final float x;
        final float y;
        final float pressure;
        final float tiltX;
        final float tiltY;
        final float distance;
        final boolean proximity;
        final boolean tipDown;
        final int tipState;
        final boolean canceled;
        final int buttons;
        final long timeMsec;

        Sample(int deviceId, int pointerId, int toolType, int capabilities,
               float x, float y, float pressure, float tiltX, float tiltY,
               float distance, boolean proximity, boolean tipDown,
               boolean canceled, int buttons, long timeMsec) {
            this(deviceId, pointerId, toolType, capabilities, x, y, pressure,
                    tiltX, tiltY, distance, proximity, tipDown,
                    TIP_STATE_UNKNOWN, canceled, buttons, timeMsec);
        }

        Sample(int deviceId, int pointerId, int toolType, int capabilities,
               float x, float y, float pressure, float tiltX, float tiltY,
               float distance, boolean proximity, boolean tipDown, int tipState,
               boolean canceled, int buttons, long timeMsec) {
            this.deviceId = deviceId;
            this.pointerId = pointerId;
            this.toolType = toolType;
            this.capabilities = capabilities;
            this.x = finiteOrZero(x);
            this.y = finiteOrZero(y);
            this.pressure = clamp01(pressure);
            this.tiltX = clampTilt(tiltX);
            this.tiltY = clampTilt(tiltY);
            this.distance = clamp01(distance);
            this.proximity = proximity;
            this.tipDown = tipDown;
            this.tipState = tipState;
            this.canceled = canceled;
            this.buttons = buttons & (BUTTON_PRIMARY | BUTTON_SECONDARY);
            this.timeMsec = timeMsec;
        }

        private static float finiteOrZero(float value) {
            return Float.isFinite(value) ? value : 0f;
        }

        private static float clamp01(float value) {
            return Float.isFinite(value) ? Math.max(0f, Math.min(1f, value)) : 0f;
        }

        private static float clampTilt(float value) {
            return Float.isFinite(value) ? Math.max(-90f, Math.min(90f, value)) : 0f;
        }
    }

    /** Immutable native call; FRAME state is the final field group committed. */
    static final class Frame {
        final int toolId;
        final int toolType;
        final int capabilities;
        final int deviceId;
        final float x;
        final float y;
        final float pressure;
        final float tiltX;
        final float tiltY;
        final float distance;
        final int flags;
        final int buttons;
        final long timeMsec;

        Frame(int toolId, Sample sample, int flags, int buttons, long timeMsec) {
            this.toolId = toolId;
            this.toolType = sample.toolType;
            this.capabilities = sample.capabilities;
            this.deviceId = sample.deviceId;
            this.x = sample.x;
            this.y = sample.y;
            this.pressure = sample.pressure;
            this.tiltX = sample.tiltX;
            this.tiltY = sample.tiltY;
            this.distance = sample.distance;
            this.flags = flags;
            this.buttons = buttons;
            this.timeMsec = timeMsec;
        }
    }

    private static final class ActiveTool {
        final int id;
        Sample last;
        boolean tipDown;
        boolean tipStateKnown;

        ActiveTool(int id, Sample last) {
            this.id = id;
            this.last = last;
            this.tipDown = last.tipDown;
            this.tipStateKnown = last.tipState != TIP_STATE_UNKNOWN;
        }
    }

    private static final class Identity {
        final int deviceId;
        final int pointerId;
        final int toolType;
        final int capabilities;

        Identity(Sample sample) {
            deviceId = sample.deviceId;
            pointerId = sample.pointerId;
            toolType = sample.toolType;
            capabilities = sample.capabilities;
        }

        @Override public boolean equals(Object other) {
            if (!(other instanceof Identity))
                return false;
            Identity identity = (Identity) other;
            return deviceId == identity.deviceId && pointerId == identity.pointerId
                    && toolType == identity.toolType && capabilities == identity.capabilities;
        }

        @Override public int hashCode() {
            int result = deviceId;
            result = 31 * result + pointerId;
            result = 31 * result + toolType;
            result = 31 * result + capabilities;
            return result;
        }
    }

    private final Sender sender;
    private final Map<Long, ActiveTool> active = new HashMap<>();
    /* Keep ids across proximity exits and cancellations. Backends retain metadata
     * for each id for the life of the connection, so an id must never name a
     * different device/tool/capability tuple. */
    private final Map<Identity, Integer> identities = new HashMap<>();
    private int nextToolId = 1;

    TabletInput(Sender sender) {
        this.sender = sender;
    }

    void update(Sample sample) {
        if (sample == null)
            return;
        long key = key(sample.deviceId, sample.pointerId);
        ActiveTool tool = active.get(key);

        if (sample.canceled) {
            cancel(tool, sample.timeMsec);
            active.remove(key);
            return;
        }
        if (!sample.proximity) {
            if (tool != null) {
                tool.last = sample;
                sender.send(new Frame(tool.id, sample, 0, 0, sample.timeMsec));
                active.remove(key);
            }
            return;
        }

        if (tool == null || tool.last.toolType != sample.toolType
                || tool.last.capabilities != sample.capabilities) {
            if (tool != null) {
                cancel(tool, sample.timeMsec);
                active.remove(key);
            }
            int toolId = allocateToolId(sample);
            if (toolId == 0)
                return;
            tool = new ActiveTool(toolId, sample);
            active.put(key, tool);
        }

        if (sample.tipState == TIP_STATE_DOWN) {
            tool.tipDown = true;
            tool.tipStateKnown = true;
        } else if (sample.tipState == TIP_STATE_UP) {
            tool.tipDown = false;
            tool.tipStateKnown = true;
        } else if (!tool.tipStateKnown) {
            tool.tipDown = sample.tipDown;
        }
        tool.last = sample;
        int flags = FLAG_PROXIMITY | (tool.tipDown ? FLAG_TIP_DOWN : 0);
        sender.send(new Frame(tool.id, sample, flags, sample.buttons, sample.timeMsec));
    }

    void cancelDevice(int deviceId, long timeMsec) {
        Iterator<ActiveTool> it = active.values().iterator();
        while (it.hasNext()) {
            ActiveTool tool = it.next();
            if (tool.last.deviceId == deviceId) {
                cancel(tool, timeMsec);
                it.remove();
            }
        }
    }

    void cancelAll(long timeMsec) {
        for (ActiveTool tool : active.values())
            cancel(tool, timeMsec);
        active.clear();
    }

    /**
     * Forget identities only after the native producer session has been reset.
     * The caller must release active tools before disconnecting the transport.
     */
    void resetSession() {
        if (!active.isEmpty())
            throw new IllegalStateException("tablet tools must be canceled before session reset");
        identities.clear();
        nextToolId = 1;
    }

    boolean hasActiveTools() {
        return !active.isEmpty();
    }

    private void cancel(ActiveTool tool, long timeMsec) {
        if (tool != null)
            sender.send(new Frame(tool.id, tool.last, FLAG_CANCELED, 0, timeMsec));
    }

    private int allocateToolId(Sample sample) {
        Identity identity = new Identity(sample);
        Integer existing = identities.get(identity);
        if (existing != null)
            return existing;
        if (nextToolId <= 0)
            return 0;
        int id = nextToolId++;
        identities.put(identity, id);
        return id;
    }

    /** Project Android's tilt/azimuth pair into Wayland's signed X/Y tilt angles. */
    static float[] projectTilt(float tiltRadians, float azimuthRadians) {
        if (!Float.isFinite(tiltRadians) || !Float.isFinite(azimuthRadians))
            return new float[] {0f, 0f};
        double tilt = Math.max(0.0, Math.min(Math.PI / 2.0, tiltRadians));
        double tangent = Math.tan(tilt);
        double tiltX = -Math.atan(tangent * Math.sin(azimuthRadians));
        double tiltY = Math.atan(tangent * Math.cos(azimuthRadians));
        return new float[] {(float) Math.toDegrees(tiltX), (float) Math.toDegrees(tiltY)};
    }

    private static long key(int deviceId, int pointerId) {
        return ((long) deviceId << 32) | (pointerId & 0xffffffffL);
    }
}
