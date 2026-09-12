package com.anlandnext;

import android.os.Binder;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.Parcel;
import android.util.Log;

import java.util.concurrent.CopyOnWriteArrayList;

/**
 * Window lifecycle events for the window list (daemon "anland.host"
 * T_SUBSCRIBE): created / destroyed / attached / detached are pushed by the
 * daemon onto our EventBinder and dispatched to listeners on the main
 * thread. Consumer = MainActivity (the window list); WlWindowActivity is
 * NOT a subscriber — its attach path is the separate SURFACE/ctrl binder
 * pair, do not mix the two channels.
 *
 * Scope: this APK is package-authenticated, so it receives every window's
 * events; a normal app receives only its own uid's windows (daemon-side uid
 * filter). The daemon disconnects a paused subscriber on its own (reported
 * pause / binder death / failed send / oom_score_adj watchdog) — after
 * resume the app re-subscribes and re-pulls LIST: events are live-edge
 * only, what happened while disconnected is never replayed.
 *
 * Main thread only: onResume → addListener + acquire, onPause → release +
 * removeListener. acquire() is reference-counted within the process; the
 * first acquire subscribes, the last release unsubscribes.
 */
public final class WindowEvents {
    private static final String TAG = "anland-events";
    /** Daemon-side AIBinder_Class_define — associateClass refuses a
     *  class-less proxy, so the descriptor must match exactly */
    private static final String EVT_DESC = "anland.IEvents";

    public interface Listener {
        void onWindowCreated(long id, String title);
        void onWindowDestroyed(long id);
        void onWindowAttached(long id);
        void onWindowDetached(long id);
    }

    /**
     * Local endpoint the daemon pushes events onto (also the subscription
     * identity — the daemon dedupes by binder pointer, so re-subscribing
     * this same object after a daemon restart is safe).
     */
    static final class EventBinder extends Binder {
        EventBinder() {
            /* publish the descriptor (same ICtrl lesson: the daemon's
             * associateClass compares it against INTERFACE_TRANSACTION) */
            attachInterface(null, EVT_DESC);
        }

        @Override
        protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
                throws android.os.RemoteException {
            if (code < FIRST_CALL_TRANSACTION || code > LAST_CALL_TRANSACTION)
                return super.onTransact(code, data, reply, flags);
            try {
                data.enforceInterface(EVT_DESC);
            } catch (Exception e) {
                return false;   /* descriptor mismatch (not from the daemon) */
            }
            final long id = data.readLong();
            String title = null;
            if (code == WlBinder.E_CREATED) title = data.readString();
            dispatch(code, id, title);
            return true;
        }
    }

    private static final EventBinder BINDER = new EventBinder();
    private static final Handler MAIN = new Handler(Looper.getMainLooper());
    private static final CopyOnWriteArrayList<Listener> LISTENERS = new CopyOnWriteArrayList<>();

    private static int refs;                  /* acquire/release pairs; main thread only */
    private static volatile boolean subscribed;

    /** daemon died → the next acquire()/ensure() re-subscribes (WlBinder re-fetches the service) */
    private static final IBinder.DeathRecipient daemonDeath = () -> subscribed = false;

    /** binder threads → main thread fan-out */
    private static void dispatch(int code, long id, String title) {
        MAIN.post(() -> {
            for (Listener l : LISTENERS) {
                if (code == WlBinder.E_CREATED) l.onWindowCreated(id, title);
                else if (code == WlBinder.E_DESTROYED) l.onWindowDestroyed(id);
                else if (code == WlBinder.E_ATTACHED) l.onWindowAttached(id);
                else if (code == WlBinder.E_DETACHED) l.onWindowDetached(id);
            }
        });
    }

    /* ---- refcounted subscription (main thread only) ---- */

    /** Activity onResume: 0→1 (or the daemon restarted since) → subscribe */
    public static void acquire() {
        if (refs++ == 0 || !subscribed) subscribe();
    }

    /** Activity onPause: 1→0 → unsubscribe (the daemon watchdog also
     *  disconnects a subscriber that went background without reporting) */
    public static void release() {
        if (--refs == 0) unsubscribe();
    }

    /** Re-subscribe if the daemon went away and came back while we held the
     *  reference (call after a successful LIST — the service is alive) */
    public static void ensure() {
        if (refs > 0 && !subscribed) subscribe();
    }

    private static void subscribe() {
        int rc = WlBinder.subscribe(BINDER);
        subscribed = rc == 0;
        if (subscribed) WlBinder.monitorDeath(daemonDeath);
        Log.i(TAG, "subscribe rc=" + rc);
    }

    private static void unsubscribe() {
        if (!subscribed) return;
        subscribed = false;
        WlBinder.unsubscribe(BINDER);
    }

    public static void addListener(Listener l) { LISTENERS.addIfAbsent(l); }
    public static void removeListener(Listener l) { LISTENERS.remove(l); }

    private WindowEvents() { }
}
