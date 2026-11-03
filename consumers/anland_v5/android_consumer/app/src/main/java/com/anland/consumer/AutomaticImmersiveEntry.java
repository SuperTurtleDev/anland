package com.anland.consumer;

/** One automatic attempt window per foreground entry or lifecycle interruption. */
final class AutomaticImmersiveEntry {
    // Gold backs off for up to 30 seconds between device scans. Allow its
    // next scan, Android device publication, and previous-helper cleanup too.
    private static final long WAIT_MS = 45_000L;
    private boolean pending;
    private long deadline;

    static boolean enabledFor(boolean immersiveEnabled, boolean autoEnabled,
                              ImmersiveInputSource source) {
        return immersiveEnabled && autoEnabled && source == ImmersiveInputSource.EXISTING_UINPUT_BUS;
    }

    void arm(long now) {
        pending = true;
        deadline = now + WAIT_MS;
    }

    void cancel() {
        pending = false;
    }

    boolean canAttempt(boolean resumed, boolean surfaceReady, boolean focused) {
        return pending && resumed && surfaceReady && focused;
    }

    /** A submitted session consumes the attempt, so a manual exit stays exited. */
    boolean retry(boolean completed, long now) {
        if (completed || now >= deadline)
            pending = false;
        return pending;
    }
}
