package com.anland.consumer;

import android.content.Context;

import java.util.Collection;

/**
 * One immersive input-capture transport. This is the seam between the session
 * controller and the root helper which grabs physical nodes or Gold output.
 *
 * <p>Implementations are single-use. Callers create a fresh transport for every
 * attempt and wait for {@link Listener#onEnded} before replacing it.
 * Implementations serialize callbacks on the main thread. A successful
 * {@code start} must eventually report exactly one {@code onEnded}; a failed
 * start reports no callbacks and needs no {@link #stop}.
 */
interface InputGrabTransport {
    int EVENT_STRIDE = 6;
    interface Listener {
        /** Opened device, initially or on hotplug/state changes. Ranges are raw ABS units. */
        void onDevice(int dev, int cls, int minX, int maxX, int minY, int maxY, int flags);

        /** Range for an advertised EV_ABS axis, following its DEVICE record. */
        void onAxisRange(int dev, int axis, int min, int max);

        /** Initial setup settled. Hotplug and device-state records may still follow. */
        void onReady(int grabbedCount);

        /**
         * Flat {dev, type, code, value, timeLow, timeHigh} records. The last two
         * ints contain the evdev timestamp in microseconds, low word first.
         * Batched per SYN_REPORT so a multi-touch frame is applied in one piece.
         */
        void onEvents(int[] batch, int count);

        /** The session ended, for any reason, exactly once per {@code start}. */
        void onEnded(int reason);
    }

    interface Factory {
        InputGrabTransport create(Context context, Listener listener);
    }

    boolean isRunning();

    /**
     * @param toggleScanCode evdev code of the key that ends the session; the root
     *                       helper refuses to grab anything without one.
     */
    boolean start(int toggleScanCode);

    /**
     * @param selectedNodes nodes to take, or {@code null}/empty to auto-select.
     *                      Uses the legacy grammar, so a saved {@code event01}
     *                      is still honoured.
     */
    boolean start(int toggleScanCode, Collection<String> selectedNodes);

    /**
     * @param excludedNodes nodes to leave strictly alone — normally the ones
     *                      Gold has confirmed it holds. Uses the strict grammar,
     *                      and a list containing anything else is refused.
     *
     * <p>The default forwards to the two-argument form. Every production
     * transport overrides this: a wrapper that inherited the default would
     * silently drop the exclusions and start grabbing nodes out from under
     * Gold, which is the one mistake this whole path exists to prevent.
     */
    default boolean start(int toggleScanCode, Collection<String> selectedNodes,
                          Collection<String> excludedNodes) {
        return start(toggleScanCode, selectedNodes);
    }

    default boolean start(int toggleScanCode, Collection<String> selectedNodes,
                          Collection<String> excludedNodes, ImmersiveInputSource source) {
        return source == ImmersiveInputSource.DIRECT_EVENT_NODES
                && start(toggleScanCode, selectedNodes, excludedNodes);
    }

    void stop();
}
