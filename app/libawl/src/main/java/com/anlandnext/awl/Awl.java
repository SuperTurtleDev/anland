package com.anlandnext.awl;

import android.content.Context;
import android.content.Intent;
import android.net.Uri;
import android.os.Binder;
import android.os.Handler;
import android.os.IBinder;
import android.os.Looper;
import android.os.Parcel;
import android.os.ParcelFileDescriptor;
import android.util.Log;

import java.io.FileDescriptor;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CopyOnWriteArrayList;

/**
 * anland client library — third-party apps host their own wayland windows on
 * the device's anland daemon (root-resident binder service "anland.host",
 * shipped by the anland-awl module).
 *
 * All calls hit the daemon's per-uid ownership scope: a normal app only ever
 * sees, subscribes to, attaches and closes the windows of wayland clients
 * running under its own uid. The identity chain: {@link #getWaylandFd}
 * creates the socketpair in YOUR process, so the daemon-side credentials of
 * the wayland connection are yours — attach/input/ime/close all check
 * against them.
 *
 * Typical flow:
 * <pre>
 *   fd = Awl.getWaylandFd();            // your wayland connection
 *   ... wl_display_connect_to_fd(fd) ...   // e.g. via JNI + libwayland-client
 *   Awl.registerCallback(cb);           // window events (main thread)
 *   Awl.attachWindow(ctx, id, title);   // show a toplevel as YOUR app's window
 * </pre>
 *
 * Events are live-edge only (never replayed); after resume call
 * {@link #getWindows()} for the current snapshot. The daemon disconnects a
 * paused (cached/frozen) subscriber by itself — call
 * {@link #ensureSubscribed()} from your Activity's onResume to re-arm.
 */
public final class Awl {
    private static final String TAG = "anland-awl";

    /** One daemon window (own-uid scope applied server-side). */
    public static final class WlWindow {
        public final long id;
        public final boolean attached;   /* an Activity currently holds its surface */
        public final String title;
        WlWindow(long id, boolean attached, String title) {
            this.id = id; this.attached = attached; this.title = title;
        }
    }

    /** Window lifecycle events (delivered on the main thread). */
    public interface Callback {
        void onWindowCreated(long id, String title);
        void onWindowDestroyed(long id);
        void onWindowAttached(long id);
        void onWindowDetached(long id);
    }

    /** Daemon reachable? */
    public static boolean available() { return AwlClient.available(); }

    /**
     * Wayland connection over binder: returns our end of a socketpair whose
     * other end the daemon wired into its wayland server. Use it with
     * wl_display_connect_to_fd() (JNI + libwayland-client, or any wayland
     * binding that accepts an fd). Caller owns the descriptor; close it to
     * disconnect. null = daemon unreachable.
     */
    public static FileDescriptor getWaylandFd() {
        return AwlClient.connect();
    }

    /** Current windows of THIS app's uid (null = daemon unreachable). */
    public static List<WlWindow> getWindows() {
        ArrayList<AwlClient.WinInfo> in = AwlClient.list();
        if (in == null) return null;
        ArrayList<WlWindow> out = new ArrayList<>(in.size());
        for (AwlClient.WinInfo w : in)
            out.add(new WlWindow(w.id, w.attached, w.title));
        return out;
    }

    /** Ask the wayland client to close this window gracefully (your own
     *  windows only, enforced server-side). 0 = requested. */
    public static int closeWindow(long id) {
        return AwlClient.close(id);
    }

    /* ---- event subscription (process-wide; main thread bookkeeping) ---- */

    private static final EventBinder BINDER = new EventBinder();
    private static final Handler MAIN = new Handler(Looper.getMainLooper());
    private static final CopyOnWriteArrayList<Callback> CALLBACKS = new CopyOnWriteArrayList<>();
    private static volatile boolean subscribed;
    private static int refs;   /* AwlWindowActivity resume/pause pairs; main thread only */

    /** daemon died → the next ensureSubscribed()/register re-subscribes */
    private static final IBinder.DeathRecipient daemonDeath = () -> subscribed = false;

    /** Register an event callback (subscribes on the first). Main thread. */
    public static void registerCallback(Callback cb) {
        CALLBACKS.addIfAbsent(cb);
        subscribe();
    }

    /** Remove a callback (unsubscribes when none remain). Main thread. */
    public static void unregisterCallback(Callback cb) {
        CALLBACKS.remove(cb);
        if (CALLBACKS.isEmpty() && refs == 0) unsubscribe();
    }

    /** Re-arm the subscription if the daemon disconnected us while paused
     *  (it auto-disconnects cached/frozen subscribers). Call from onResume;
     *  cheap when already subscribed. */
    public static void ensureSubscribed() {
        if (!CALLBACKS.isEmpty() || refs > 0) subscribe();
    }

    static void acquire() {   /* AwlWindowActivity onResume */
        if (refs++ == 0 || !subscribed) subscribe();
    }

    static void release() {   /* AwlWindowActivity onPause */
        if (--refs == 0 && CALLBACKS.isEmpty()) unsubscribe();
    }

    private static void subscribe() {
        if (subscribed && AwlClient.available()) return;
        int rc = AwlClient.subscribe(BINDER);
        subscribed = rc == 0;
        if (subscribed) AwlClient.monitorDeath(daemonDeath);
        Log.i(TAG, "subscribe rc=" + rc);
    }

    private static void unsubscribe() {
        if (!subscribed) return;
        subscribed = false;
        AwlClient.unsubscribe(BINDER);
    }

    private static void dispatch(int code, long id, String title) {
        MAIN.post(() -> {
            for (Callback cb : CALLBACKS) {
                if (code == AwlClient.E_CREATED) cb.onWindowCreated(id, title);
                else if (code == AwlClient.E_DESTROYED) cb.onWindowDestroyed(id);
                else if (code == AwlClient.E_ATTACHED) cb.onWindowAttached(id);
                else if (code == AwlClient.E_DETACHED) cb.onWindowDetached(id);
            }
        });
    }

    /** Daemon → app event endpoint (descriptor "anland.IEvents"; also the
     *  subscription identity — the daemon dedupes by binder pointer). */
    static final class EventBinder extends Binder {
        EventBinder() {
            attachInterface(null, "anland.IEvents");
        }

        @Override
        protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
                throws android.os.RemoteException {
            if (code < FIRST_CALL_TRANSACTION || code > LAST_CALL_TRANSACTION)
                return super.onTransact(code, data, reply, flags);
            try {
                data.enforceInterface("anland.IEvents");
            } catch (Exception e) {
                return false;   /* descriptor mismatch (not from the daemon) */
            }
            final long id = data.readLong();
            String title = null;
            if (code == AwlClient.E_CREATED) title = data.readString();
            dispatch(code, id, title);
            return true;
        }
    }

    /* ---- native client spawning ---- */

    /** A spawned wayland client: pid + its wired stdio (close them when
     *  done; the pipes EOF when the child exits). */
    public static final class ClientProcess {
        public final int pid;
        /** write end of the child's stdin (null only on a partial failure) */
        public final ParcelFileDescriptor stdin;
        /** read end of the child's stdout — its log */
        public final ParcelFileDescriptor stdout;
        /** read end of the child's stderr */
        public final ParcelFileDescriptor stderr;

        ClientProcess(int pid, ParcelFileDescriptor in, ParcelFileDescriptor out,
                      ParcelFileDescriptor err) {
            this.pid = pid; this.stdin = in; this.stdout = out; this.stderr = err;
        }
    }

    /**
     * Spawn a native wayland client with the connection fd inherited:
     * fork+exec of exePath with WAYLAND_SOCKET=&lt;fd&gt; in the child's
     * environment (wl_display_connect(NULL) consumes it). Java cannot pass
     * open fds to Runtime.exec children — the library's tiny native helper
     * does the fork and wires the child's stdio to pipes handed back here
     * (plain-printf clients stay loggable from the app). The wayland fd is
     * consumed, the child is reaped by the library. Returns null on
     * failure.
     */
    public static ClientProcess spawnClient(FileDescriptor fd, String exePath, String... args) {
        if (fd == null || exePath == null) return null;
        int raw = -1;
        try {
            ParcelFileDescriptor pfd = ParcelFileDescriptor.dup(fd);
            raw = pfd.detachFd();
        } catch (Exception e) {
            Log.e(TAG, "fd dup failed", e);
        }
        if (raw < 0) return null;
        int[] r = Spawn.nativeSpawn(raw, exePath, args);
        /* the child holds its forked reference; close the app's own copy (a
         * leaked copy would keep the connection alive after the child exits) */
        try { android.system.Os.close(fd); } catch (android.system.ErrnoException ignored) { }
        if (r == null || r.length < 4 || r[0] <= 0) return null;
        return new ClientProcess(r[0],
                ParcelFileDescriptor.adoptFd(r[1]),
                ParcelFileDescriptor.adoptFd(r[2]),
                ParcelFileDescriptor.adoptFd(r[3]));
    }

    /* ---- window hosting ---- */

    /**
     * Show a wayland toplevel as a window of THIS app: starts the library's
     * {@link AwlWindowActivity} (merged into your manifest) in its own task
     * — Recents identity, process and binder credentials are yours, never
     * the anland host APK's. Re-attaching an already-shown window brings its
     * task to the front. The daemon must have a window with this id created
     * by a wayland client of your uid (your {@link #getWaylandFd}
     * connection), otherwise the activity exits immediately.
     */
    public static void attachWindow(Context ctx, long id, String title) {
        Intent it = new Intent(ctx, AwlWindowActivity.class);
        it.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK
                | Intent.FLAG_ACTIVITY_NEW_DOCUMENT);
        it.setData(Uri.parse("anland://win/" + id));
        it.putExtra("id", id);
        if (title != null) it.putExtra("title", title);
        ctx.startActivity(it);
    }

    private Awl() { }
}
