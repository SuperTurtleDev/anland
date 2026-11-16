package com.anland.consumer;

import android.content.Context;
import android.content.SharedPreferences;
import android.os.Handler;
import android.os.Looper;
import android.view.KeyEvent;
import android.widget.Toast;

import java.security.SecureRandom;
import java.util.LinkedHashSet;
import java.util.Set;

/**
 * Owns immersive input: which source is configured, which one is running, and
 * making sure only one of them ever is.
 *
 * <p>Every source uses one root-helper session. Gold output is read as raw
 * evdev events after remapping, so Android's shortcut policy is not involved.
 */
final class ImmersiveInputController {
    private static final String PREFS_NAME = "anland_settings";

    /**
     * The file Gold watches to know this app wants its own profile. Its presence
     * is the whole message: no command, no restart, no signal.
     */
    private static final String PROFILE_REQUEST =
            "/data/adb/modules/Gold_Keyboardremaps/anland-profile";
    private static final long PROFILE_REQUEST_TIMEOUT_MS = 5000L;

    private final Context ctx;
    private final ImmersiveMode direct;
    private final GoldStatusCache goldStatus;
    private final SuCommand.CommandRunner runner = new SuCommand.SuRunner();
    private final Handler main = new Handler(Looper.getMainLooper());

    private ImmersiveInputSource runningSource = ImmersiveInputSource.DIRECT_EVENT_NODES;

    ImmersiveInputController(ImmersiveMode.Host host) {
        this.ctx = host.context();
        this.direct = new ImmersiveMode(host);
        this.goldStatus = new GoldStatusCache(new GoldInputStatusClient(new GoldController(
                new SuCommand.SuRunner(), GoldController.DEFAULT_CONTROLLER, new SecureRandom())));

        // A flag left behind by a crash would keep Gold on the Anland profile
        // with nothing running to ever take it off again.
        setAnlandProfileRequest(false);
    }

    /**
     * Tells Gold to use the Anland profile, or to stop.
     *
     * <p>Runs off the main thread. Gold switches mapping profiles on its own
     * heartbeat while retaining its physical grabs and virtual output nodes.
     */
    private void setAnlandProfileRequest(final boolean wanted) {
        synchronized (profileRequestLock) {
            profileRequestWanted = wanted;
            if (profileRequestRunning)
                return;
            profileRequestRunning = true;
        }
        Thread worker = new Thread(this::drainProfileRequests, "anland-profile-request");
        worker.setDaemon(true);
        worker.start();
    }

    /**
     * Applies profile requests one at a time, coalescing to the latest.
     *
     * <p>A thread per request would let them overlap, and these are a {@code
     * touch} and an {@code rm} on the same path: whichever loses the race does
     * so silently, and the bad outcome is concrete -- an {@code rm} that finishes
     * before an earlier {@code touch} leaves the flag behind, Gold stays on the
     * Anland profile with nothing running to ever take it off, which is exactly
     * the state the constructor clears on startup.
     *
     * <p>Only the final state matters, so requests are folded rather than
     * queued: the loop re-reads what is wanted after each command and stops once
     * it has applied the latest.
     */
    private void drainProfileRequests() {
        for (;;) {
            boolean target;
            synchronized (profileRequestLock) {
                target = profileRequestWanted;
            }
            runner.run((target ? "touch " : "rm -f ") + SuCommand.shellQuote(PROFILE_REQUEST),
                    PROFILE_REQUEST_TIMEOUT_MS);
            synchronized (profileRequestLock) {
                if (profileRequestWanted == target) {
                    profileRequestRunning = false;
                    return;
                }
            }
        }
    }

    /** Guards {@link #drainProfileRequests} against overlapping runs. */
    private final Object profileRequestLock = new Object();
    private boolean profileRequestWanted;
    private boolean profileRequestRunning;

    /**
     * Called by the host whenever a session starts or ends, whichever source it
     * was. Entering immersive mode is what puts Gold on the Anland profile, and
     * leaving it is what takes Gold off again.
     */
    void onSessionActiveChanged(boolean active) {
        setAnlandProfileRequest(active);
    }

    /** Whether the app enters immersive mode on its own when it comes back up. */
    static final String KEY_AUTO_ENTER = "immersive_auto_enter";

    /**
     * Attempts one automatic entry after the host has focus and a surface.
     * False asks the host to retry: Gold may still be publishing its output, or
     * the previous foreground session may still be releasing its helper.
     * A submitted start is attempted only once; helper failures keep their normal
     * diagnostics instead of producing a loop of root requests and toasts.
     */
    boolean enterAutomatically() {
        SharedPreferences prefs = prefs();
        ImmersiveInputSource current = ImmersiveInputSource.read(prefs);
        if (!AutomaticImmersiveEntry.enabledFor(
                prefs.getBoolean(ImmersiveMode.KEY_ENABLED, false),
                prefs.getBoolean(KEY_AUTO_ENTER, false), current))
            return true;
        if (direct.isStopping())
            return false;
        if (isActive())
            return true;
        if (!current.isAvailable(ctx))
            return false;
        runningSource = current;
        direct.startWith(null, null, current);
        return true;
    }

    ImmersiveInputSource source() {
        return ImmersiveInputSource.read(prefs());
    }

    boolean isActive() {
        return direct.isActive();
    }

    boolean ownsGoldKeyboard() {
        return isActive() && runningSource.listensToGoldKeyboard();
    }

    /** Controllers in physical modes are already visible through desktop evdev. */
    boolean usesDirectControllers() {
        return isActive() && runningSource.takesDevices();
    }

    // ---- key routing -------------------------------------------------------

    /**
     * Consumes the bound immersive key, whichever source is configured.
     *
     * @return true when the event was the toggle and must go no further
     */
    boolean handleKey(KeyEvent event) {
        ImmersiveInputSource current = source();
        if (current != runningSource) {
            // The source was changed underneath a live session. Whatever was
            // running ends before the new source can be toggled.
            stop();
            runningSource = current;
        }

        if (!direct.isBoundToggle(event))
            return false;
        if (direct.consumeSuppressedToggle(event))
            return true;

        if (event.getAction() == KeyEvent.ACTION_DOWN && event.getRepeatCount() == 0) {
            if (isActive()) {
                stopSession();
            } else {
                startSession(current, direct.toggleScanCodeFor(event));
            }
        }
        return true;
    }

    /**
     * Start one helper with the selected source, then announce the session.
     */
    private void startSession(ImmersiveInputSource source, int toggleScanCode) {
        // Checked again here, not just in the picker: the module can be removed
        // between choosing a source and using it, and a session that came up
        // with nothing to read from would look exactly like one that worked.
        if (!source.isAvailable(ctx)) {
            toast(R.string.immersive_source_unavailable);
            return;
        }
        boolean started = source.takesDevices() ? startDirectNow(source, toggleScanCode)
                : direct.startWith(null, null, source, toggleScanCode);
        if (!started)
            return;
        direct.announceWhenReady(getString(R.string.immersive_entering_mode, sourceLabel(source)));
    }

    /** The source's own name, the same words the picker lists it under. */
    private String sourceLabel(ImmersiveInputSource source) {
        String[] names = ctx.getResources().getStringArray(R.array.immersive_source_options);
        int index = source.menuIndex();
        return index < names.length ? names[index] : source.preferenceValue;
    }

    private String getString(int res, Object... args) {
        return ctx.getString(res, args);
    }

    private void stopSession() {
        // Leaves the toggle's own release swallowed, so the key that ended the
        // session does not then reach the desktop.
        direct.requestStop();
    }

    // ---- direct start -------------------------------------------------------

    /**
     * Starts from the cached occupancy, synchronously.
     *
     * <p>Nothing on this path may wait. The toggle is a key press, and asking
     * Gold costs an {@code su} round trip; a session that hesitates before
     * starting is worse than one that starts without knowing which nodes Gold
     * holds, because the only cost of not knowing is a grab that fails and moves
     * on. The cache is refilled in the background instead, so the next press has
     * an answer without ever having paid for one here.
     */
    private boolean startDirectNow(ImmersiveInputSource source, int toggleScanCode) {
        SharedPreferences prefs = prefs();
        boolean automatic = ImmersiveInputSource.isAutomatic(prefs);
        Set<String> saved = ImmersiveInputSource.savedNodes(prefs);

        // Only a verified snapshot may contribute exclusions. An unreadable Gold
        // is not an idle Gold, so it contributes none.
        //
        // The exclusions are Gold's whole claimed set, not just the node it holds
        // this second: a keyboard offered over two transports can be held over
        // either, and taking the other one would put this session and Gold on the
        // same keyboard from opposite sides.
        GoldInputStatusClient.Result cached = goldStatus.fresh(now());
        Set<String> occupied = null;
        if (cached != null && cached.state == GoldInputStatusClient.State.VERIFIED) {
            occupied = new LinkedHashSet<>(cached.snapshot.claimed);
        } else if (shouldWarnGoldUnknown(cached)) {
            // No cached answer, or an unreadable one, means Gold may still be
            // holding nodes, so warn before starting without exclusions. An
            // ABSENT answer is different: the controller proved that Gold is
            // not installed, so there is nothing to warn about.
            toast(R.string.immersive_gold_unknown);
        }

        ImmersiveDirectSelection.Result selection = ImmersiveDirectSelection.resolve(
                automatic, saved, occupied, InputGrab.MAX_SELECTED_NODES);

        if (selection.invalid) {
            toast(R.string.immersive_selection_invalid);
            return false;
        }
        if (selection.allFiltered && !source.listensToGoldKeyboard()) {
            // Every saved node is Gold's. Falling back to automatic here would
            // quietly take nodes the user explicitly did not choose.
            toast(R.string.immersive_selection_filtered);
            return false;
        }
        // If every selected physical node belongs to Gold, the combined source
        // can still take Gold output. Keep the saved nonempty selection so the
        // helper does not interpret it as permission to auto-grab other nodes.
        boolean started = direct.startWith(selection.allFiltered ? saved : selection.selectedNodes,
                selection.excludedNodes, source, toggleScanCode);

        // Gold's answer for the next press, and for the settings page. Started
        // after the session is up so it cannot delay it.
        refreshGoldStatus();
        return started;
    }

    /** Whether the occupancy answer is missing or cannot be trusted. */
    static boolean shouldWarnGoldUnknown(GoldInputStatusClient.Result result) {
        return result == null || result.state == GoldInputStatusClient.State.UNKNOWN;
    }

    /**
     * Refills the occupancy cache in the background. Cheap to call: a refresh
     * already in flight is left alone, and the answer is only kept if it is
     * still current when it lands.
     */
    void refreshGoldStatus() {
        if (!goldStatus.beginRefresh())
            return;
        final long requestedAt = now();
        Thread worker = new Thread(() -> {
            GoldInputStatusClient.Result result = goldStatus.query();
            main.post(() -> {
                goldStatus.endRefresh();
                goldStatus.store(result, requestedAt, now());
            });
        }, "anland-gold-status");
        worker.setDaemon(true);
        worker.start();
    }

    private static long now() {
        return android.os.SystemClock.uptimeMillis();
    }

    // ---- lifecycle ---------------------------------------------------------

    /**
     * Ends whichever source is running and invalidates anything in flight.
     * Called for focus loss, pause, destroy, surface loss, screen off and source
     * switches alike: a session must never outlive the foreground.
     */
    void stop() {
        direct.stop();
        // Warm the occupancy answer for the next press. Left to the resume
        // refresh, a session started more than a few seconds later finds the
        // cache cold, drops Gold's exclusions, and can take nodes Gold is
        // holding -- the contention the selection design exists to avoid.
        refreshGoldStatus();
    }

    private SharedPreferences prefs() {
        return ctx.getSharedPreferences(PREFS_NAME, Context.MODE_PRIVATE);
    }

    private void toast(int stringRes) {
        Toast.makeText(ctx, stringRes, Toast.LENGTH_SHORT).show();
    }

    /** For messages that need a formatted argument, such as the bound key's name. */
    private void toast(String message) {
        Toast.makeText(ctx, message, Toast.LENGTH_SHORT).show();
    }
}
