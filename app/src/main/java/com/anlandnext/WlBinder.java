package com.anlandnext;

import android.os.IBinder;
import android.os.Parcel;
import android.os.RemoteException;
import android.util.Log;
import android.view.Surface;

import java.lang.reflect.Method;
import java.util.ArrayList;

/**
 * Client of the daemon binder service "anland.host".
 * Reflects into ServiceManager.getService (hidden API, light-grey — usable at
 * targetSdk 29).
 *
 * Protocol (matches the AWL_T_* codes in awl_daemon.cpp; single-attach model —
 * the APK only reports facts, all detach/evict/close decisions live in the
 * daemon):
 *   SURFACE(id,w,h,SurfaceParcel,deathToken,host) / PAUSE(id,host) ONEWAY
 *   RESIZE(id,w,h) / LIST() → {id,attached,title}... / BRING(id) / CLOSE(id)
 *   SUBSCRIBE(eventBinder) / UNSUBSCRIBE(eventBinder) — window lifecycle
 *   events pushed to the binder (see WindowEvents; normal apps receive only
 *   their own uid's windows)
 * host = Activity instance id (trailing field of SURFACE/PAUSE; the daemon
 * decides eviction from it).
 */
public final class WlBinder {
    private static final String TAG = "anland-binder";
    /** Descriptor from the daemon's AIBinder_Class_define — the NDK wrapper's checkInterface requires transactions to start with it */
    private static final String DESCRIPTOR = "anland.IHost";
    public static final int T_SURFACE = 1;
    public static final int T_RESIZE = 3;
    public static final int T_LIST = 4;
    public static final int T_BRING = 5;
    public static final int T_PAUSE = 6;    /* Activity onPause → daemon full detach (ONEWAY) */
    public static final int T_FOCUS = 8;    /* focus change → notify the wayland client */
    public static final int T_INPUT = 9;    /* input event literal translation (ONEWAY hot path) */
    public static final int T_IME = 10;     /* IME text passthrough (ONEWAY) */
    public static final int T_CLIPBOARD = 11; /* Android clipboard text → wl selection (ONEWAY) */
    public static final int T_CFG_GET = 12;   /* (key) → val: read a daemon config (#31) */
    public static final int T_CFG_SET = 13;   /* (key,val) → ok: apply + persist in daemon (#31) */
    public static final int T_CLOSE = 14;     /* (id) → ok: long-press "Close" in window list (the only close entry) */
    public static final int T_ICON = 15;      /* (id) → w:i32 h:i32 bytes[RGBA]: toplevel icon (xdg-toplevel-icon-v1) */
    public static final int T_SUBSCRIBE = 16;   /* (eventBinder) → ok: window lifecycle events pushed to it (WindowEvents) */
    public static final int T_UNSUBSCRIBE = 17; /* (eventBinder) → ok: stop events (onPause; daemon watchdog backstops) */
    /* T_CONNECT = 18 (wayland fd over binder, #36) is third-party-app only —
     * this APK never uses it; the protocol lives in waylandbridge.cpp */

    /* Event codes on the event binder (match the daemon's AWL_E_*) */
    public static final int E_CREATED = 1;   /* (id:i64, title:string16) */
    public static final int E_DESTROYED = 2; /* (id:i64) */
    public static final int E_ATTACHED = 3;  /* (id:i64) */
    public static final int E_DETACHED = 4;  /* (id:i64) */

    /* AWL_T_IME ops (match awl.h AWL_IME_*) */
    public static final int IME_COMMIT = 1;   /* text: commit */
    public static final int IME_PREEDIT = 2;  /* text; a/b = cursor begin/end (bytes) */
    public static final int IME_DELETE = 3;   /* a/b = before/after (bytes) */
    public static final int IME_CURSOR = 4;   /* a/b = index/anchor (v1) */

    private static IBBinder s;

    /** wrapper: re-fetched automatically after death */
    private static final class IBBinder {
        final IBinder b;
        IBBinder(IBinder b) { this.b = b; }
    }

    private static IBinder get() {
        if (s != null && s.b.pingBinder()) return s.b;
        s = null;
        try {
            Class<?> sm = Class.forName("android.os.ServiceManager");
            Method m = sm.getMethod("getService", String.class);
            IBinder b = (IBinder) m.invoke(null, "anland.host");
            if (b == null) Log.w(TAG, "getService(anland.host) = null");
            else if (b.pingBinder()) s = new IBBinder(b);
            else Log.w(TAG, "anland.host pingBinder=false");
        } catch (Exception e) {
            Log.e(TAG, "getService failed", e);
        }
        return s != null ? s.b : null;
    }

    public static boolean available() { return get() != null; }

    /** Surface report (carries a death token: app killed → the daemon's binder
     *  death callback detaches automatically).
     *  host = Activity instance id (trailing field: the daemon's eviction
     *  criterion for the previous holder) */
    public static int surface(long id, int w, int h, Surface surface, IBinder deathToken,
                              long host) {
        IBinder b = get();
        if (b == null || surface == null) return -1;
        Parcel d = Parcel.obtain();
        Parcel r = Parcel.obtain();
        try {
            d.writeInterfaceToken(DESCRIPTOR);
            d.writeLong(id);
            d.writeInt(w);
            d.writeInt(h);
            surface.writeToParcel(d, 0);
            d.writeStrongBinder(deathToken);
            d.writeLong(host);   /* trailing: old daemons ignore extra bytes */
            boolean ok = b.transact(T_SURFACE, d, r, 0);
            int rc = r.dataSize() >= 4 ? r.readInt() : Integer.MIN_VALUE;
            Log.i(TAG, "SURFACE transact ok=" + ok + " replySize=" + r.dataSize() + " rc=" + rc);
            return rc;
        } catch (Exception e) {
            Log.e(TAG, "SURFACE transact failed", e);
            s = null;
            return -1;
        } finally {
            d.recycle();
            r.recycle();
        }
    }

    public static int resize(long id, int w, int h) {
        IBinder b = get();
        if (b == null) return -1;
        Parcel d = Parcel.obtain();
        Parcel r = Parcel.obtain();
        try {
            d.writeInterfaceToken(DESCRIPTOR);
            d.writeLong(id);
            d.writeInt(w);
            d.writeInt(h);
            b.transact(T_RESIZE, d, r, 0);
            return r.readInt();
        } catch (Exception e) {
            s = null;
            return -1;
        } finally {
            d.recycle();
            r.recycle();
        }
    }

    public static int bring(long id) {
        IBinder b = get();
        if (b == null) return -1;
        Parcel d = Parcel.obtain();
        Parcel r = Parcel.obtain();
        try {
            d.writeInterfaceToken(DESCRIPTOR);
            d.writeLong(id);
            b.transact(T_BRING, d, r, 0);
            return r.readInt();
        } catch (Exception e) {
            s = null;
            return -1;
        } finally {
            d.recycle();
            r.recycle();
        }
    }

    /* ---- Lifecycle reports (Activity → daemon, facts only; ONEWAY: daemon
     *      teardown joins the render thread, must not block the main-thread
     *      transact of onPause) ---- */

    /** onPause → daemon full detach (minimize: the wayland window stays alive).
     *  host trailing field: a late pause from an evicted instance must not
     *  kill the current holder */
    public static void pause(long id, long host) {
        IBinder b = get();
        if (b == null) return;
        Parcel d = Parcel.obtain();
        try {
            d.writeInterfaceToken(DESCRIPTOR);
            d.writeLong(id);
            d.writeLong(host);
            b.transact(T_PAUSE, d, null, IBinder.FLAG_ONEWAY);
        } catch (Exception e) {
            s = null;
        } finally {
            d.recycle();
        }
    }

    /** Long-press "Close" in the window list: ask the client to exit cleanly (the only close entry) */
    public static int close(long id) {
        return transactId(T_CLOSE, id);
    }

    public static int focus(long id, boolean hasFocus) {
        IBinder b = get();
        if (b == null) return -1;
        Parcel d = Parcel.obtain();
        Parcel r = Parcel.obtain();
        try {
            d.writeInterfaceToken(DESCRIPTOR);
            d.writeLong(id);
            d.writeInt(hasFocus ? 1 : 0);
            b.transact(T_FOCUS, d, r, 0);
            return r.readInt();
        } catch (Exception e) {
            s = null;
            return -1;
        } finally {
            d.recycle();
            r.recycle();
        }
    }

    private static int transactId(int code, long id) {
        IBinder b = get();
        if (b == null) return -1;
        Parcel d = Parcel.obtain();
        Parcel r = Parcel.obtain();
        try {
            d.writeInterfaceToken(DESCRIPTOR);
            d.writeLong(id);
            b.transact(code, d, r, 0);
            return r.readInt();
        } catch (Exception e) {
            s = null;
            return -1;
        } finally {
            d.recycle();
            r.recycle();
        }
    }

    /* ---- Input event passthrough (ONEWAY hot path; field order matches
     *      daemon AWL_T_INPUT: id:i64 type:i32 code:i32 x,y,v1,v2:f meta:i32 flags:i32) ---- */

    public static void input(long id, int type, int code,
                             float x, float y, float v1, float v2, int meta) {
        IBinder b = get();
        if (b == null) return;
        Parcel d = Parcel.obtain();
        try {
            d.writeInterfaceToken(DESCRIPTOR);
            d.writeLong(id);
            d.writeInt(type);
            d.writeInt(code);
            d.writeFloat(x);
            d.writeFloat(y);
            d.writeFloat(v1);
            d.writeFloat(v2);
            d.writeInt(meta);
            d.writeInt(0);   /* flags reserved */
            b.transact(T_INPUT, d, null, IBinder.FLAG_ONEWAY);
        } catch (Exception e) {
            s = null;
        } finally {
            d.recycle();
        }
    }

    /* ---- IME text passthrough (ONEWAY; field order matches daemon AWL_T_IME:
     *      id:i64 op:i32 a:i32 b:i32 text:string16) ---- */

    public static void ime(long id, int op, int a, int b, String text) {
        IBinder bd = get();
        if (bd == null) return;
        Parcel d = Parcel.obtain();
        try {
            d.writeInterfaceToken(DESCRIPTOR);
            d.writeLong(id);
            d.writeInt(op);
            d.writeInt(a);
            d.writeInt(b);
            d.writeString(text == null ? "" : text);
            bd.transact(T_IME, d, null, IBinder.FLAG_ONEWAY);
        } catch (Exception e) {
            s = null;
        } finally {
            d.recycle();
        }
    }

    /* ---- Android clipboard text push (ONEWAY; #29: read by the focused
     *      Activity → an internal daemon source takes over the wl selection;
     *      field order matches daemon AWL_T_CLIPBOARD: id:i64 text:string16) ---- */

    public static void clipboard(long id, String text) {
        IBinder b = get();
        if (b == null) return;
        Parcel d = Parcel.obtain();
        try {
            d.writeInterfaceToken(DESCRIPTOR);
            d.writeLong(id);
            d.writeString(text == null ? "" : text);
            b.transact(T_CLIPBOARD, d, null, IBinder.FLAG_ONEWAY);
        } catch (Exception e) {
            s = null;
        } finally {
            d.recycle();
        }
    }

    /* ---- Daemon config read/write (#31: the daemon is the numeric source
     *      of truth, the APK keeps no copy. Field order matches daemon
     *      AWL_T_CFG_GET/SET: key:string16 [val:i32]) ---- */

    /** Read a daemon config (e.g. "zoom"); returns -1 when the daemon is
     *  unavailable or the key is unknown */
    public static int configGet(String key) {
        IBinder b = get();
        if (b == null) return -1;
        Parcel d = Parcel.obtain();
        Parcel r = Parcel.obtain();
        try {
            d.writeInterfaceToken(DESCRIPTOR);
            d.writeString(key);
            b.transact(T_CFG_GET, d, r, 0);
            return r.readInt();
        } catch (Exception e) {
            Log.e(TAG, "CFG_GET failed", e);
            s = null;
            return -1;
        } finally {
            d.recycle();
            r.recycle();
        }
    }

    /** Write a daemon config (apply + persist to config.json); 0 = success */
    public static int configSet(String key, int val) {
        IBinder b = get();
        if (b == null) return -1;
        Parcel d = Parcel.obtain();
        Parcel r = Parcel.obtain();
        try {
            d.writeInterfaceToken(DESCRIPTOR);
            d.writeString(key);
            d.writeInt(val);
            b.transact(T_CFG_SET, d, r, 0);
            return r.readInt();
        } catch (Exception e) {
            Log.e(TAG, "CFG_SET failed", e);
            s = null;
            return -1;
        } finally {
            d.recycle();
            r.recycle();
        }
    }

    /* ---- Toplevel icon fetch (xdg-toplevel-icon-v1: the daemon keeps the
     *      pixels the client sent; field order matches daemon AWL_T_ICON:
     *      id:i64 → w:i32 h:i32 bytes[RGBA]) ---- */

    /** Current toplevel icon as RGBA bytes (Bitmap ARGB_8888 order);
     *  outWH[0]/[1] receive the dimensions. Returns null when the window
     *  has no icon (or the daemon is gone). */
    public static byte[] icon(long id, int[] outWH) {
        IBinder b = get();
        if (b == null) return null;
        Parcel d = Parcel.obtain();
        Parcel r = Parcel.obtain();
        try {
            d.writeInterfaceToken(DESCRIPTOR);
            d.writeLong(id);
            b.transact(T_ICON, d, r, 0);
            if (r.dataSize() < 8) return null;
            int w = r.readInt();
            int h = r.readInt();
            if (outWH != null && outWH.length >= 2) { outWH[0] = w; outWH[1] = h; }
            return w > 0 && h > 0 ? r.createByteArray() : null;
        } catch (Exception e) {
            Log.e(TAG, "ICON failed", e);
            s = null;
            return null;
        } finally {
            d.recycle();
            r.recycle();
        }
    }

    /* ---- Window lifecycle events (daemon pushes AWL_E_* onto the binder
     *      reported here; see WindowEvents. Field order matches daemon
     *      AWL_T_SUBSCRIBE/UNSUBSCRIBE: binder only) ---- */

    /** Subscribe the process event binder (idempotent server-side: the same
     *  binder re-subscribed keeps the existing registration). 0 = subscribed. */
    public static int subscribe(IBinder listener) {
        return transactBinder(T_SUBSCRIBE, listener);
    }

    /** Stop events (Activity onPause; the daemon also disconnects paused
     *  subscribers itself). 0 = unsubscribed (or nothing was registered). */
    public static int unsubscribe(IBinder listener) {
        return transactBinder(T_UNSUBSCRIBE, listener);
    }

    private static int transactBinder(int code, IBinder listener) {
        IBinder b = get();
        if (b == null) return -1;
        Parcel d = Parcel.obtain();
        Parcel r = Parcel.obtain();
        try {
            d.writeInterfaceToken(DESCRIPTOR);
            d.writeStrongBinder(listener);
            b.transact(code, d, r, 0);
            return r.readInt();
        } catch (Exception e) {
            Log.e(TAG, "transact " + code + " failed", e);
            s = null;
            return -1;
        } finally {
            d.recycle();
            r.recycle();
        }
    }

    /* ---- Daemon death monitoring (long-lived connection dropped → Activity exits) ---- */

    public static boolean monitorDeath(IBinder.DeathRecipient dr) {
        IBinder b = get();
        if (b == null) return false;
        try {
            b.linkToDeath(dr, 0);
            return true;
        } catch (Exception e) {
            Log.e(TAG, "linkToDeath failed", e);
            return false;
        }
    }

    public static void unmonitorDeath(IBinder.DeathRecipient dr) {
        IBBinder cur = s;
        if (cur == null) return;
        try {
            cur.b.unlinkToDeath(dr, 0);
        } catch (Exception ignored) { /* already dead / never linked */ }
    }

    public static class WinInfo {
        public long id;
        public boolean attached;
        public String title;
    }

    public static ArrayList<WinInfo> list() {
        IBinder b = get();
        if (b == null) return null;
        Parcel d = Parcel.obtain();
        Parcel r = Parcel.obtain();
        try {
            d.writeInterfaceToken(DESCRIPTOR);
            b.transact(T_LIST, d, r, 0);
            Log.i(TAG, "LIST transact ok, replySize=" + r.dataSize());
            int n = r.readInt();
            ArrayList<WinInfo> out = new ArrayList<>(Math.max(0, n));
            for (int i = 0; i < n; i++) {
                WinInfo w = new WinInfo();
                w.id = r.readLong();
                w.attached = r.readInt() != 0;
                w.title = r.readString();
                out.add(w);
            }
            return out;
        } catch (Exception e) {
            Log.e(TAG, "LIST transact failed", e);
            s = null;
            return null;
        } finally {
            d.recycle();
            r.recycle();
        }
    }

    private WlBinder() { }
}
