package com.anlandnext.awl;

import android.os.IBinder;
import android.os.Parcel;
import android.os.RemoteException;
import android.util.Log;
import android.view.Surface;

import java.io.FileDescriptor;
import java.lang.reflect.Method;
import java.util.ArrayList;

/**
 * Raw binder client of the anland daemon service "anland.host" (internal to
 * the library — third-party code uses {@link Awl}). Reflects into
 * ServiceManager.getService (hidden API, light-grey).
 *
 * Protocol (matches the AWL_T_* codes in the daemon's waylandbridge.cpp; the
 * daemon scopes every transaction this library makes to the caller's own
 * windows — its uid must equal the wayland client's, which holds when the
 * connection came from {@link Awl#getWaylandFd}).
 */
final class AwlClient {
    private static final String TAG = "anland-awl";
    /** Descriptor from the daemon's AIBinder_Class_define — checkInterface requires it */
    private static final String DESCRIPTOR = "anland.IHost";

    static final int T_SURFACE = 1;
    static final int T_RESIZE = 3;
    static final int T_LIST = 4;
    static final int T_PAUSE = 6;     /* Activity onPause → daemon full detach (ONEWAY) */
    static final int T_FOCUS = 8;     /* focus change → notify the wayland client */
    static final int T_INPUT = 9;     /* input event literal translation (ONEWAY hot path) */
    static final int T_IME = 10;      /* IME text passthrough (ONEWAY) */
    static final int T_CLIPBOARD = 11; /* Android clipboard text → wl selection (ONEWAY) */
    static final int T_CLOSE = 14;    /* (id) → ok: ask the client to close the window */
    static final int T_ICON = 15;     /* (id) → w,h,bytes[RGBA] toplevel icon */
    static final int T_SUBSCRIBE = 16;   /* (eventBinder) → ok: window lifecycle events */
    static final int T_UNSUBSCRIBE = 17; /* (eventBinder) → ok: stop events */
    static final int T_CONNECT = 18;     /* (fd) → ok: wayland connection over binder */

    /* Event codes on the event binder (match the daemon's AWL_E_*) */
    static final int E_CREATED = 1;
    static final int E_DESTROYED = 2;
    static final int E_ATTACHED = 3;
    static final int E_DETACHED = 4;

    /* AWL_T_IME ops (match awl.h AWL_IME_*) */
    static final int IME_COMMIT = 1;
    static final int IME_PREEDIT = 2;
    static final int IME_DELETE = 3;
    static final int IME_CURSOR = 4;

    static final class WinInfo {
        long id;
        boolean attached;
        String title;
    }

    private static IBinder s;

    private static IBinder get() {
        if (s != null && s.pingBinder()) return s;
        s = null;
        try {
            Class<?> sm = Class.forName("android.os.ServiceManager");
            Method m = sm.getMethod("getService", String.class);
            IBinder b = (IBinder) m.invoke(null, "anland.host");
            if (b == null) Log.w(TAG, "getService(anland.host) = null");
            else if (b.pingBinder()) s = b;
            else Log.w(TAG, "anland.host pingBinder=false");
        } catch (Exception e) {
            Log.e(TAG, "getService failed", e);
        }
        return s;
    }

    static boolean available() { return get() != null; }

    static int surface(long id, int w, int h, Surface surface, IBinder deathToken,
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
            d.writeLong(host);
            b.transact(T_SURFACE, d, r, 0);
            return r.dataSize() >= 4 ? r.readInt() : Integer.MIN_VALUE;
        } catch (Exception e) {
            Log.e(TAG, "SURFACE transact failed", e);
            s = null;
            return -1;
        } finally {
            d.recycle();
            r.recycle();
        }
    }

    static int resize(long id, int w, int h) {
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
            return r.dataSize() >= 4 ? r.readInt() : -1;
        } catch (Exception e) {
            s = null;
            return -1;
        } finally {
            d.recycle();
            r.recycle();
        }
    }

    /** onPause → daemon full detach (minimize; ONEWAY — daemon teardown joins threads) */
    static void pause(long id, long host) {
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

    static int focus(long id, boolean hasFocus) {
        IBinder b = get();
        if (b == null) return -1;
        Parcel d = Parcel.obtain();
        Parcel r = Parcel.obtain();
        try {
            d.writeInterfaceToken(DESCRIPTOR);
            d.writeLong(id);
            d.writeInt(hasFocus ? 1 : 0);
            b.transact(T_FOCUS, d, r, 0);
            return r.dataSize() >= 4 ? r.readInt() : -1;
        } catch (Exception e) {
            s = null;
            return -1;
        } finally {
            d.recycle();
            r.recycle();
        }
    }

    static int close(long id) {
        IBinder b = get();
        if (b == null) return -1;
        Parcel d = Parcel.obtain();
        Parcel r = Parcel.obtain();
        try {
            d.writeInterfaceToken(DESCRIPTOR);
            d.writeLong(id);
            b.transact(T_CLOSE, d, r, 0);
            return r.dataSize() >= 4 ? r.readInt() : -1;
        } catch (Exception e) {
            s = null;
            return -1;
        } finally {
            d.recycle();
            r.recycle();
        }
    }

    /* Input event passthrough (ONEWAY hot path; field order matches daemon
     * AWL_T_INPUT: id:i64 type:i32 code:i32 x,y,v1,v2:f meta:i32 flags:i32) */
    static void input(long id, int type, int code,
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

    /* IME text passthrough (ONEWAY; id:i64 op:i32 a:i32 b:i32 text:string16) */
    static void ime(long id, int op, int a, int b, String text) {
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

    /* Android clipboard text push (ONEWAY; id:i64 text:string16) */
    static void clipboard(long id, String text) {
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

    /** Toplevel icon as RGBA bytes (Bitmap ARGB_8888 order); outWH[0]/[1] get
     *  the dimensions. null = no icon / daemon gone. */
    static byte[] icon(long id, int[] outWH) {
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
            s = null;
            return null;
        } finally {
            d.recycle();
            r.recycle();
        }
    }

    static ArrayList<WinInfo> list() {
        IBinder b = get();
        if (b == null) return null;
        Parcel d = Parcel.obtain();
        Parcel r = Parcel.obtain();
        try {
            d.writeInterfaceToken(DESCRIPTOR);
            b.transact(T_LIST, d, r, 0);
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

    static int subscribe(IBinder listener) {
        return transactBinder(T_SUBSCRIBE, listener);
    }

    static int unsubscribe(IBinder listener) {
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
            return r.dataSize() >= 4 ? r.readInt() : -1;
        } catch (Exception e) {
            Log.e(TAG, "transact " + code + " failed", e);
            s = null;
            return -1;
        } finally {
            d.recycle();
            r.recycle();
        }
    }

    /**
     * Wayland connection over binder: create a socketpair, hand one end to
     * the daemon (T_CONNECT), return the other for wl_display_connect_to_fd.
     * The daemon's wl_client_create reads SO_PEERCRED on its end — fixed at
     * socketpair creation to OUR creds — so its per-window ownership checks
     * see our real uid. With daemon config socket_listen=0 this is the only
     * connection path (no wayland-0 socket file exists).
     */
    static FileDescriptor connect() {
        IBinder b = get();
        if (b == null) return null;
        /* API-29 Os.socketpair is void — pass the two descriptors in to fill */
        FileDescriptor theirs = new FileDescriptor();
        FileDescriptor ours = new FileDescriptor();
        try {
            android.system.Os.socketpair(android.system.OsConstants.AF_UNIX,
                    android.system.OsConstants.SOCK_STREAM, 0, theirs, ours);
        } catch (android.system.ErrnoException e) {
            Log.e(TAG, "socketpair failed", e);
            return null;
        }
        boolean ok = false;
        Parcel d = Parcel.obtain();
        Parcel r = Parcel.obtain();
        try {
            d.writeInterfaceToken(DESCRIPTOR);
            /* Wire format of AParcel_readParcelFileDescriptor (= native
             * readParcelable<ParcelFileDescriptor>), three fields:
             *   int32(1)  readParcelable present marker (0 = null)
             *   int32(0)  ParcelFileDescriptor's commFd-present flag
             *             (ParcelFileDescriptor.java writeToParcel; 0 = no commFd)
             *   fd        flat fd (Parcel.writeFileDescriptor ≡ native
             *             writeDupFileDescriptor, wire-identical to
             *             writeDupParcelFileDescriptor's body)
             * Equivalently: writeInt(1) + ParcelFileDescriptor.writeToParcel. */
            d.writeInt(1);
            d.writeInt(0);
            d.writeFileDescriptor(theirs);
            b.transact(T_CONNECT, d, r, 0);
            ok = r.dataSize() >= 4 && r.readInt() == 0;
            if (!ok) Log.e(TAG, "CONNECT refused (daemon down / server off)");
        } catch (Exception e) {
            Log.e(TAG, "CONNECT failed", e);
            s = null;
        } finally {
            /* our copy of the daemon's end MUST close: the kernel dup'ed it
             * into the daemon at transact time, and with our copy still open
             * the daemon's death would never EOF our end */
            try { android.system.Os.close(theirs); } catch (android.system.ErrnoException ignored) { }
            if (!ok)
                try { android.system.Os.close(ours); } catch (android.system.ErrnoException ignored) { }
            d.recycle();
            r.recycle();
        }
        return ok ? ours : null;
    }

    static boolean monitorDeath(IBinder.DeathRecipient dr) {
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

    static void unmonitorDeath(IBinder.DeathRecipient dr) {
        IBinder cur = s;
        if (cur == null) return;
        try {
            cur.unlinkToDeath(dr, 0);
        } catch (Exception ignored) { /* already dead / never linked */ }
    }

    private AwlClient() { }
}
