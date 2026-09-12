package com.anlandnext;

import android.app.Activity;
import android.content.ClipData;
import android.content.ClipDescription;
import android.content.ClipboardManager;
import android.os.Binder;
import android.os.Bundle;
import android.os.IBinder;
import android.os.Parcel;
import android.text.InputType;
import android.text.TextUtils;
import android.util.Log;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.PointerIcon;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.View;
import android.view.WindowInsets;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.CompletionInfo;
import android.view.inputmethod.CorrectionInfo;
import android.view.inputmethod.CursorAnchorInfo;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.ExtractedText;
import android.view.inputmethod.ExtractedTextRequest;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputMethodManager;
import android.view.inputmethod.SurroundingText;
import android.view.inputmethod.TextAttribute;
import android.widget.EditText;
import android.widget.FrameLayout;

import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.atomic.AtomicLong;

/**
 * One wayland window = one Activity instance (id passed as extra).
 * The Surface is reported to the daemon over binder (the daemon holds the
 * rendering resources and renders straight into this window).
 *
 * Lifecycle (single-attach model: the APK only reports facts, all decisions
 * live in the daemon):
 *   onPause → PAUSE(id,host): daemon full detach (minimize: the wayland
 *   window stays alive); onResume / surfaceChanged → re-send SURFACE to
 *   re-attach (the daemon evicts the previous holder).
 *   Killed-from-recents / swiped away = minimize-and-keep-alive (onPause +
 *   binder death already cover it, no onDestroy report).
 *   The only close entry = long-press menu in the MainActivity window list
 *   (daemon T_CLOSE). Client closed its own window / evicted by a newer
 *   instance → daemon ctrl C_CLOSE → finish.
 *
 * IME bridge (text-input protocol ↔ Android IME, one direct pipe per
 * Activity, no hub):
 *   display side = legacy hidden EditText (1x1 invisible, disabled by
 *   default) — on C_IME_SHOW enable+focus+showSoftInput; WlInputConnection
 *   bridges the full InputConnection table to the text-input protocol.
 *   The daemon pushes the client's editor state (surrounding/cursor rect) →
 *   cached locally to answer IME queries (tokenization/prediction context) +
 *   updateSelection/updateCursorAnchorInfo.
 *   Display mode: inset (surface yields to the keyboard, client reflows) /
 *   overlay (floating over the surface).
 */
public class WlWindowActivity extends Activity {
    private static final String TAG = "anland-win";
    private static final ConcurrentHashMap<Long, WlWindowActivity> LIVE = new ConcurrentHashMap<>();

    /** Control-channel descriptor (daemon AIBinder_Class_define "anland.ICtrl") */
    private static final String CTRL_DESC = "anland.ICtrl";
    /** daemon → Activity command codes */
    private static final int C_CLOSE = 1;      /* wayland window destroyed → exit */
    private static final int C_TITLE = 2;      /* (title:string16) title update → Recents label */
    private static final int C_IME_SHOW = 3;   /* (hint purpose:i32) text field focused → show soft keyboard */
    private static final int C_IME_HIDE = 4;   /* () text field unfocused → hide soft keyboard */
    private static final int C_IME_STATE = 5;  /* (hint purpose cursor anchor cx cy cw ch flags
                                                  + text:string16) editor state snapshot */
    private static final int C_CLIP_WRITE = 6; /* (text:string16) wl set_selection → write
                                                  the Android clipboard (empty = clear) */
    private static final int C_CAPTURE = 7;    /* (mode x y w h:i32×5)
                                                  pointer-constraints state
                                                  sync (zwp lock/confine):
                                                  mode = 1 confine / 2 lock →
                                                  requestPointerCapture
                                                  (x,y,w,h = confine region
                                                  in view px, zeros = whole
                                                  window), 0 → release;
                                                  re-sent on set_region and
                                                  re-attach */
    private static final int C_CURSOR = 8;     /* (hidden:i32) wl client took over the
                                                  cursor (wl_pointer.set_cursor: image
                                                  composited by the daemon renderer on
                                                  top of the window, or NULL = invisible)
                                                  → hide the Android pointer; 0 = restore */
    private static final int C_KEEPON = 9;     /* (on:i32) zwp_idle_inhibit_manager_v1
                                                  aggregate flipped (inhibitor created/
                                                  destroyed on a surface of this window):
                                                  1 → FLAG_KEEP_SCREEN_ON (only honored
                                                  while the window is visible = the
                                                  protocol's visible-surface semantics),
                                                  0 → clear; re-sent on re-attach */
    private static final int C_ICON = 10;      /* (has:i32) xdg-toplevel-icon-v1 icon
                                                  applied/reset on this window's
                                                  toplevel → re-fetch the pixels
                                                  (WlBinder.icon) and re-apply the
                                                  task description */
    private static final int STATE_RESET = 0x1;   /* v1 reset → clear composing state + restartInput */
    private static final int CAPTURE_NONE = 0;
    private static final int CAPTURE_CONFINE = 1;
    private static final int CAPTURE_LOCK = 2;

    /** Unique Activity instance id (trailing host field of SURFACE/PAUSE: daemon eviction criterion) */
    private static final AtomicLong HOST_SEQ = new AtomicLong();

    private long id;
    private long host;
    private SurfaceView sv;
    private FrameLayout root;
    private EditText hiddenInput;
    private InputMethodManager imm;
    private CtrlBinder ctrl;
    private int lastW, lastH;
    private int lastImeMargin = -1;
    private boolean attached;
    private boolean finishingByGone;   /* client closed the window / evicted, nothing left to report */
    private boolean deathLinked;       /* daemon death monitoring attached */
    private String taskTitle;          /* last known client title (Recents label) */
    private android.graphics.Bitmap taskIcon;   /* last fetched toplevel icon (xdg-toplevel-icon-v1) */

    /* ---- Clipboard bridge (#29; static shared = consistent across windows,
     *      single process) ----
     * lastClipWritten: text we wrote via C_CLIP_WRITE — setPrimaryClip fires
     * the listener too, so compare-and-suppress the echo push (otherwise
     * wl↔Android loops forever).
     * lastClipPushed:  text last pushed to the daemon (repeated focus changes
     * don't re-push). */
    private static String sLastClipWritten;
    private static String sLastClipPushed;
    private ClipboardManager clipMgr;

    /* ---- IME state (pushed by daemon ctrl + written by InputConnection; all on the UI thread) ---- */

    private String surText = "";        /* client surrounding (no preedit) */
    private int surCursor, surAnchor;   /* char indices into surText */
    private int imeHint, imePurpose;    /* zwp_text_input content type */
    private final int[] imeRect = new int[4];   /* cursor rect (surface coords) */
    private boolean imeWanted;          /* input wanted (kept across detach, basis for reopening) */
    private String compText = "";       /* mirror of the preedit we sent (inserted at surCursor) */
    private int compCursor;             /* char cursor inside the preedit */

    /**
     * Local end of the control channel: reported to the daemon with SURFACE —
     * it is both the death token (app killed → the daemon's binder death
     * detaches automatically) and the endpoint the daemon sends commands to
     * (CLOSE / IME).
     */
    class CtrlBinder extends Binder {
        CtrlBinder() {
            /* Publish the descriptor: the daemon's libbinder_ndk
             * AIBinder_associateClass() queries it (INTERFACE_TRANSACTION)
             * and AIBinder_prepareTransaction() refuses any transaction on a
             * mismatch — without this every daemon→Activity command (IME
             * show/hide/state, clipboard write, title, close) failed silently
             * ("Expecting binder to have class 'anland.ICtrl' but descriptor
             * is actually ''", 2026-09-10 logcat). */
            attachInterface(null, CTRL_DESC);
        }

        @Override
        protected boolean onTransact(int code, Parcel data, Parcel reply, int flags)
                throws android.os.RemoteException {
            if (code < FIRST_CALL_TRANSACTION || code > LAST_CALL_TRANSACTION)
                return super.onTransact(code, data, reply, flags);   /* INTERFACE_TRANSACTION / PING / DUMP: no interface token in the parcel */
            try {
                data.enforceInterface(CTRL_DESC);
            } catch (Exception e) {
                return false;   /* descriptor mismatch (not from the daemon) — reject */
            }
            if (code == C_CLOSE) {
                Log.i(TAG, "win " + id + ": CLOSE via ctrl channel");
                finishingByGone = true;
                runOnUiThread(() -> { if (!isFinishing()) finish(); });
                return true;
            }
            if (code == C_TITLE) {
                String t = data.readString();
                if (t != null && !t.isEmpty()) {
                    taskTitle = t;
                    runOnUiThread(() -> applyTaskDescription());
                }
                return true;
            }
            if (code == C_IME_SHOW) {
                int hint = data.readInt();
                int purpose = data.readInt();
                runOnUiThread(() -> onImeShow(hint, purpose));
                return true;
            }
            if (code == C_IME_HIDE) {
                runOnUiThread(this::onImeHideCmd);
                return true;
            }
            if (code == C_IME_STATE) {
                int hint = data.readInt(), purpose = data.readInt();
                int curB = data.readInt(), ancB = data.readInt();
                int cx = data.readInt(), cy = data.readInt();
                int cw = data.readInt(), ch = data.readInt();
                int fl = data.readInt();
                String text = data.readString();
                runOnUiThread(() -> onImeState(text, curB, ancB, hint, purpose,
                                              cx, cy, cw, ch, fl));
                return true;
            }
            if (code == C_CLIP_WRITE) {
                String t = data.readString();
                final String text = t == null ? "" : t;
                runOnUiThread(() -> writeClipboard(text));
                return true;
            }
            if (code == C_CAPTURE) {
                final int mode = data.readInt();
                final int rx = data.readInt(), ry = data.readInt();
                final int rw = data.readInt(), rh = data.readInt();
                runOnUiThread(() -> setPointerCaptureMode(mode, rx, ry, rw, rh));
                return true;
            }
            if (code == C_CURSOR) {
                final boolean hidden = data.readInt() != 0;
                runOnUiThread(() -> setPointerHidden(hidden));
                return true;
            }
            if (code == C_KEEPON) {
                final boolean on = data.readInt() != 0;
                runOnUiThread(() -> applyKeepOn(on));
                return true;
            }
            if (code == C_ICON) {
                data.readInt();   /* has flag — always re-fetch (reset ⇒ fetch returns none) */
                applyTaskIconAsync();
                return true;
            }
            return super.onTransact(code, data, reply, flags);
        }

        private void onImeHideCmd() {
            onImeHide();
        }
    }

    /** Daemon (peer of the long-lived binder connection) death → this window
     *  lost its host, exit */
    private final IBinder.DeathRecipient daemonDeath = new IBinder.DeathRecipient() {
        @Override
        public void binderDied() {
            Log.e(TAG, "win " + id + ": daemon gone (binder death) -> finish");
            finishingByGone = true;   /* DETACH has nowhere to go now */
            /* the wl-side selection died with the daemon — clear the push
             * bookkeeping; once a new daemon is up, the focused window
             * re-pushes the current clipboard */
            sLastClipPushed = null;
            sLastClipWritten = null;
            runOnUiThread(() -> { if (!isFinishing()) finish(); });
        }
    };

    public static void finishById(long id) {
        WlWindowActivity a = LIVE.get(id);
        if (a != null) {
            a.finishingByGone = true;
            a.finish();
            Log.i(TAG, "win " + id + " finished (client gone)");
        }
    }

    @Override
    protected void onNewIntent(android.content.Intent intent) {
        super.onNewIntent(intent);
        /* defensive id switch when documentLaunchMode reuses this instance for the same data URI */
        long nid = intent.getLongExtra("id", id);
        if (nid != id) {
            if (attached) WlBinder.pause(id, host);   /* daemon detaches the old id */
            setPointerCaptureMode(CAPTURE_NONE, 0, 0, 0, 0);   /* the old window's constraint does not carry over */
            LIVE.remove(id);
            id = nid;
            host = HOST_SEQ.incrementAndGet();
            ctrl = new CtrlBinder();
            attached = false;
            lastW = lastH = 0;
            surText = ""; surCursor = surAnchor = 0;
            compText = ""; compCursor = 0;
            imeWanted = false;
            LIVE.put(id, this);
            String nt = intent.getStringExtra("title");
            if (nt != null && !nt.isEmpty()) {
                taskTitle = nt;
                applyTaskDescription();
            }
            Log.i(TAG, "win re-bound to id=" + id);
        }
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        id = getIntent().getLongExtra("id", -1);
        if (id < 0) { finish(); return; }
        host = HOST_SEQ.incrementAndGet();

        ctrl = new CtrlBinder();
        LIVE.put(id, this);
        imm = getSystemService(InputMethodManager.class);
        clipMgr = getSystemService(ClipboardManager.class);

        /* Recents shows the wayland window's real title (the UI itself has no title bar, only the task label) */
        String title = getIntent().getStringExtra("title");
        if (title != null && !title.isEmpty()) {
            taskTitle = title;
            applyTaskDescription();
        }

        /* Long-lived binder death monitoring: daemon gone (module restart / killed) → exit, no dead windows left behind */
        deathLinked = WlBinder.monitorDeath(daemonDeath);
        if (!deathLinked && !WlBinder.available()) {
            Log.e(TAG, "win " + id + ": daemon unreachable -> finish");
            finish();
            return;
        }

        sv = new SurfaceView(this);
        sv.getHolder().setFormat(android.graphics.PixelFormat.RGBX_8888);
        sv.getHolder().addCallback(new SurfaceHolder.Callback() {
            @Override public void surfaceCreated(SurfaceHolder holder) { }
            @Override public void surfaceChanged(SurfaceHolder holder, int format,
                                                 int width, int height) {
                Log.i(TAG, "win " + id + " surface " + width + "x" + height);
                if (!attached)
                    sendSurface(holder, width, height);
                else if (width != lastW || height != lastH) {
                    /* window resize (rotation/split-screen/IME inset) → configure the client to follow */
                    WlBinder.resize(id, width, height);
                    lastW = width;
                    lastH = height;
                }
            }
            @Override public void surfaceDestroyed(SurfaceHolder holder) {
                /* the daemon already detached at PAUSE; here we only clear the fact flag */
                attached = false;
            }
        });

        initHiddenInput();

        /* 小窗 (ColorOS freeform) reports the bottom resize bar as a content inset;
         * with decor-fits enabled the DecorView pads the content up and the bar
         * strip exposes the black window background. Take over inset handling
         * (same call as the legacy v5 consumer) so the surface draws under the
         * bar and the bar stays translucent over the app content. */
        if (android.os.Build.VERSION.SDK_INT >= 30)
            getWindow().setDecorFitsSystemWindows(false);

        root = new FrameLayout(this);
        root.setFitsSystemWindows(false);
        root.addView(sv, new FrameLayout.LayoutParams(
                FrameLayout.LayoutParams.MATCH_PARENT, FrameLayout.LayoutParams.MATCH_PARENT));
        root.addView(hiddenInput, new FrameLayout.LayoutParams(1, 1));
        /* IME inset: in inset mode the surface yields (client reflows); in overlay mode the keyboard floats above */
        root.setOnApplyWindowInsetsListener((v, insets) -> {
            applyImeInset(insets);
            return insets;
        });
        setContentView(root);

        setupFullscreen();   /* immersive (mimics legacy) */
    }

    /* Immersive fullscreen — ported line-by-line from ~/anland legacy: hide
     * status bar + navigation bar, swipe-revealed as transient overlays,
     * extend into the display cutout area. */
    private void setupFullscreen() {
        android.view.WindowInsetsController ic = getWindow().getInsetsController();
        if (ic != null) {
            ic.hide(android.view.WindowInsets.Type.statusBars()
                  | android.view.WindowInsets.Type.navigationBars());
            ic.setSystemBarsBehavior(
                android.view.WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
        }
        getWindow().getAttributes().layoutInDisplayCutoutMode =
            android.view.WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS;
    }

    /** Recents entry: last known client title + last fetched toplevel icon.
     *  TaskDescription is atomic — every label update must re-carry the icon
     *  or it is silently dropped. */
    private void applyTaskDescription() {
        if (taskTitle == null && taskIcon == null) return;
        android.app.ActivityManager.TaskDescription td = taskIcon != null
                ? new android.app.ActivityManager.TaskDescription(taskTitle, taskIcon)
                : new android.app.ActivityManager.TaskDescription(taskTitle);
        setTaskDescription(td);
    }

    /** Pull the daemon's stored toplevel icon (xdg-toplevel-icon-v1 pixels,
     *  AWL_T_ICON) off the UI thread, then swap the task description. */
    private void applyTaskIconAsync() {
        final long fid = id;
        new Thread(() -> {
            int[] wh = new int[2];
            byte[] px = WlBinder.icon(fid, wh);
            android.graphics.Bitmap bmp = null;
            if (px != null && wh[0] > 0 && wh[1] > 0) {
                try {
                    bmp = android.graphics.Bitmap.createBitmap(
                            wh[0], wh[1], android.graphics.Bitmap.Config.ARGB_8888);
                    bmp.copyPixelsFromBuffer(java.nio.ByteBuffer.wrap(px));
                } catch (Exception e) {
                    Log.w(TAG, "toplevel icon decode failed", e);
                    bmp = null;
                }
            }
            final android.graphics.Bitmap fb = bmp;
            runOnUiThread(() -> { taskIcon = fb; applyTaskDescription(); });
        }, "awl-icon").start();
    }

    /** Report the Surface to the daemon (shared by first attach / resume re-attach).
     *  rc != 0 (service gone / window missing / attach failed) → exit, no
     *  placeholder instance left behind */
    private void sendSurface(SurfaceHolder holder, int w, int h) {
        int rc = WlBinder.surface(id, w, h, holder.getSurface(), ctrl, host);
        attached = rc == 0;
        lastW = w;
        lastH = h;
        if (!attached) {
            Log.e(TAG, "win " + id + " surface binder rc=" + rc + " -> finish");
            finish();
            return;
        }
        applyTaskIconAsync();   /* the daemon may already hold an icon (re-attach / set before map) */
    }

    /* ---- Clipboard bridge (#29) ----
     * Android→wl: read while focused (background reads restricted on
     * Android 10+) → T_CLIPBOARD push; three entry points — listener +
     * focus-gained + onResume (the latter two cover "listener missed the
     * change" cases such as a daemon restart). Plain-text bridge only:
     * non-text/plain clipboards are ignored. */

    private final ClipboardManager.OnPrimaryClipChangedListener clipListener =
            new ClipboardManager.OnPrimaryClipChangedListener() {
        @Override
        public void onPrimaryClipChanged() {
            pushClipboard();
        }
    };

    /** Read the current clipboard as text; null when absent / not coercible
     *  to text / read restricted. Any item is coerced (coerceToText handles
     *  HTML/URI/Intent items): apps such as OnePlus Notes label their clips
     *  text/html only, a strict text/plain mime check dropped them
     *  (2026-09-10: clipboard read logged, nothing pushed). */
    private String readClipText() {
        if (clipMgr == null) return null;
        try {
            ClipData cd = clipMgr.getPrimaryClip();
            if (cd == null || cd.getItemCount() == 0) return null;
            ClipDescription d = cd.getDescription();
            if (d != null && d.hasMimeType("image/*") && !d.hasMimeType("text/*"))
                return null;   /* pure image clip: nothing to bridge */
            CharSequence cs = cd.getItemAt(0).coerceToText(this);
            if (cs == null || cs.length() == 0) return null;
            return cs.toString();
        } catch (SecurityException e) {
            Log.w(TAG, "win " + id + ": clipboard read denied");
            return null;
        }
    }

    private void pushClipboard() {
        if (!hasWindowFocus()) return;   /* background reads restricted; re-pushed on focus-gained */
        String t = readClipText();
        if (t == null) { Log.i(TAG, "win " + id + ": clipboard→wl skipped (no text clip)"); return; }
        if (t.length() > 256 * 1024) t = t.substring(0, 256 * 1024);
        if (t.equals(sLastClipWritten) || t.equals(sLastClipPushed)) {
            Log.i(TAG, "win " + id + ": clipboard→wl skipped (unchanged, " + t.length() + " chars)");
            return;
        }
        sLastClipPushed = t;
        WlBinder.clipboard(id, t);
        Log.i(TAG, "win " + id + ": clipboard→wl " + t.length() + " chars");
    }

    /** wl client set_selection → write the Android clipboard (UI thread; sent by daemon ctrl) */
    private void writeClipboard(String t) {
        sLastClipWritten = t;   /* record first: setPrimaryClip fires our own listener */
        if (clipMgr == null) return;
        if (t.isEmpty()) clipMgr.clearPrimaryClip();
        else clipMgr.setPrimaryClip(ClipData.newPlainText("anland", t));
        Log.i(TAG, "win " + id + ": wl→clipboard " + t.length() + " chars");
    }

    /* ---- Lifecycle → control-channel reports ---- */

    @Override
    protected void onResume() {
        super.onResume();
        setupFullscreen();   /* the system may reset immersive mode (legacy re-asserts in onResume too) */
        if (clipMgr != null)
            clipMgr.addPrimaryClipChangedListener(clipListener);
        /* pause without stop (quick round-trip) → the surface survived and
         * surfaceChanged won't fire again: re-sending SURFACE here is the
         * re-attach. If the surface was rebuilt, surfaceChanged handles it */
        if (!attached && lastW > 0 && sv != null
                && sv.getHolder().getSurface() != null
                && sv.getHolder().getSurface().isValid())
            sendSurface(sv.getHolder(), lastW, lastH);
        Log.i(TAG, "win " + id + " RESUME");
        /* capture state is daemon-owned: the SURFACE re-attach re-pushes
         * C_CAPTURE (a persistent constraint survives the pause) */
    }

    @Override
    protected void onPause() {
        if (clipMgr != null)
            clipMgr.removePrimaryClipChangedListener(clipListener);
        setPointerCaptureMode(CAPTURE_NONE, 0, 0, 0, 0);   /* release + local mode reset (the daemon mirror survives; re-pushed on re-attach) */
        endPadStream();   /* the touchpad pointer stream may be interrupted by lifecycle: make up leave/button releases */
        /* treat as minimize: daemon full detach (rendering resources freed,
         * wayland window kept alive). Clear attached locally too —
         * onResume/surfaceChanged re-attach from there */
        WlBinder.pause(id, host);
        attached = false;
        Log.i(TAG, "win " + id + " PAUSE");
        super.onPause();
    }

    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        WlBinder.focus(id, hasFocus);   /* focus notifies the wayland client (configure ACTIVATED) */
        if (hasFocus) {
            tryShowIme();        /* C_IME_SHOW may arrive before focus does (input state kept across re-attach) */
            pushClipboard();     /* daemon restart / listener missed the change → re-push while focused */
            applyPointerCapture();   /* the system broke the capture silently on focus loss — re-request (mode unchanged) */
        }
        Log.i(TAG, "win " + id + " focus=" + hasFocus);
    }

    /* ---- IME bridge ----
     * legacy hidden-EditText mode: 1x1 invisible, disabled by default;
     * enable+focus+show when needed. WlInputConnection bridges the full
     * table (text → text-input protocol; queries ← state cache). */

    private void initHiddenInput() {
        hiddenInput = new EditText(this) {
            @Override
            public InputConnection onCreateInputConnection(EditorInfo outAttrs) {
                super.onCreateInputConnection(outAttrs);
                int sel = editorSelStart();
                outAttrs.initialSelStart = sel;
                outAttrs.initialSelEnd = sel;
                outAttrs.initialCapsMode = TextUtils.getCapsMode(editorText(), sel,
                        TextUtils.CAP_MODE_SENTENCES | TextUtils.CAP_MODE_WORDS
                                | TextUtils.CAP_MODE_CHARACTERS);
                return new WlInputConnection(this);
            }
        };
        hiddenInput.setBackgroundColor(android.graphics.Color.TRANSPARENT);
        hiddenInput.setCursorVisible(false);
        hiddenInput.setAlpha(0f);
        hiddenInput.setEnabled(false);
        hiddenInput.setFocusable(false);
        hiddenInput.setFocusableInTouchMode(false);
        hiddenInput.setClickable(false);
        hiddenInput.setLongClickable(false);
        hiddenInput.setImeOptions(EditorInfo.IME_ACTION_GO
                | EditorInfo.IME_FLAG_NO_EXTRACT_UI
                | EditorInfo.IME_FLAG_NO_FULLSCREEN);
        hiddenInput.setInputType(InputType.TYPE_CLASS_TEXT
                | InputType.TYPE_TEXT_VARIATION_NORMAL);
    }

    /** zwp_text_input content hint/purpose → Android InputType (keyboard layout / segmentation style) */
    private int imeInputType() {
        int t;
        switch (imePurpose) {
            case 2: case 3: case 10: case 11: case 12:   /* digits number date time datetime */
                t = InputType.TYPE_CLASS_NUMBER; break;
            case 9:                                      /* pin */
                t = InputType.TYPE_CLASS_NUMBER
                        | InputType.TYPE_NUMBER_VARIATION_PASSWORD; break;
            case 4:
                t = InputType.TYPE_CLASS_PHONE; break;
            case 5:
                t = InputType.TYPE_CLASS_TEXT | InputType.TYPE_TEXT_VARIATION_URI; break;
            case 6:
                t = InputType.TYPE_CLASS_TEXT
                        | InputType.TYPE_TEXT_VARIATION_EMAIL_ADDRESS; break;
            case 7:
                t = InputType.TYPE_CLASS_TEXT
                        | InputType.TYPE_TEXT_VARIATION_PERSON_NAME; break;
            case 8:                                      /* password */
                t = InputType.TYPE_CLASS_TEXT
                        | InputType.TYPE_TEXT_VARIATION_PASSWORD; break;
            default:
                t = InputType.TYPE_CLASS_TEXT; break;
        }
        if ((imeHint & 0x200) != 0) t |= InputType.TYPE_TEXT_FLAG_MULTI_LINE;
        if ((imeHint & 0x1) != 0) t |= InputType.TYPE_TEXT_FLAG_AUTO_COMPLETE;
        if ((imeHint & 0x2) != 0) t |= InputType.TYPE_TEXT_FLAG_AUTO_CORRECT;
        if ((imeHint & 0x4) != 0) t |= InputType.TYPE_TEXT_FLAG_CAP_SENTENCES;
        if ((imeHint & 0x10) != 0) t |= InputType.TYPE_TEXT_FLAG_CAP_CHARACTERS;
        if ((imeHint & 0x40) != 0
                && (t & InputType.TYPE_MASK_CLASS) == InputType.TYPE_CLASS_TEXT
                && imePurpose != 8)
            t = (t & ~InputType.TYPE_MASK_VARIATION)
                    | InputType.TYPE_TEXT_VARIATION_PASSWORD;   /* hidden_text */
        return t;
    }

    /** IME display mode: 0 = inset (surface yields), 1 = overlay (floating above) */
    private boolean imeOverlayMode() {
        return getSharedPreferences("awl", MODE_PRIVATE).getInt("ime_mode", 0) != 0;
    }

    private void applyImeInset(WindowInsets insets) {
        int imeBottom = insets.getInsets(WindowInsets.Type.ime()).bottom;
        int margin = imeOverlayMode() ? 0 : imeBottom;
        if (margin == lastImeMargin) return;
        lastImeMargin = margin;
        FrameLayout.LayoutParams lp = (FrameLayout.LayoutParams) sv.getLayoutParams();
        lp.bottomMargin = margin;
        sv.setLayoutParams(lp);   /* surface resize → configure the client to reflow */
    }

    private void onImeShow(int hint, int purpose) {
        imeWanted = true;
        if (hint != imeHint || purpose != imePurpose) {
            imeHint = hint;
            imePurpose = purpose;
            hiddenInput.setInputType(imeInputType());
        }
        tryShowIme();
    }

    /** Focus may arrive late (surfaceChanged precedes onWindowFocusChanged) → show as soon as ready */
    private void tryShowIme() {
        if (!imeWanted || !hasWindowFocus() || imm == null) {
            Log.i(TAG, "win " + id + ": ime show deferred (wanted=" + imeWanted
                    + " focus=" + hasWindowFocus() + ")");
            return;
        }
        hiddenInput.setEnabled(true);
        hiddenInput.setFocusable(true);
        hiddenInput.setFocusableInTouchMode(true);
        boolean focused = hiddenInput.requestFocus();
        /* explicit (flags=0), not SHOW_IMPLICIT: the wl client asked for the
         * panel (activate/show_input_panel/enable = the user tapped a text
         * field); InputMethodService.onShowInputRequested refuses implicit
         * requests while a hard keyboard is attached (unless "show virtual
         * keyboard" is on) — this device runs with a pogo keyboard. */
        boolean shown = imm.showSoftInput(hiddenInput, 0);
        Log.i(TAG, "win " + id + ": ime show requestFocus=" + focused + " showSoftInput=" + shown
                + " type=0x" + Integer.toHexString(hiddenInput.getInputType()));
    }

    private void onImeHide() {
        Log.i(TAG, "win " + id + ": ime hide");
        imeWanted = false;
        if (imm != null)
            imm.hideSoftInputFromWindow(hiddenInput.getWindowToken(), 0);
        if (hiddenInput.isEnabled()) {
            hiddenInput.clearFocus();
            hiddenInput.setFocusable(false);
            hiddenInput.setFocusableInTouchMode(false);
            hiddenInput.setEnabled(false);
        }
    }

    /** Editor state pushed by the daemon (the client's set_surrounding/cursor_rect take effect with the commit) */
    private void onImeState(String text, int curB, int ancB, int hint, int purpose,
                            int cx, int cy, int cw, int ch, int flags) {
        boolean typeChanged = hint != imeHint || purpose != imePurpose;
        surText = text == null ? "" : text;
        surCursor = Math.min(surText.length(), byteToChar(surText, curB));
        surAnchor = Math.min(surText.length(), byteToChar(surText, ancB));
        imeHint = hint;
        imePurpose = purpose;
        imeRect[0] = cx; imeRect[1] = cy; imeRect[2] = cw; imeRect[3] = ch;
        if ((flags & STATE_RESET) != 0) {
            compText = "";
            compCursor = 0;
            if (imm != null) imm.restartInput(hiddenInput);
            return;
        }
        if (typeChanged) hiddenInput.setInputType(imeInputType());
        notifyImeState();
    }

    /** Push selection/composing region/cursor anchor (IME candidate window follows the cursor, context stays in sync) */
    private void notifyImeState() {
        if (imm == null) return;
        int sel = editorSelStart();
        int candStart = -1, candEnd = -1;
        int compAt = Math.min(surCursor, surText.length());
        if (!compText.isEmpty()) {
            candStart = compAt;
            candEnd = compAt + compText.length();
        }
        imm.updateSelection(hiddenInput, sel, sel, candStart, candEnd);
        if (imeRect[2] > 0 && imeRect[3] > 0 && sv != null) {
            /* Positional parameters require a local→screen matrix
             * (CursorAnchorInfo.Builder.build throws IllegalArgumentException
             * otherwise — crashed the process on the first real C_IME_STATE,
             * 2026-09-10). The daemon's rect is in surface-view pixels; the
             * IME wants screen coordinates. */
            int[] loc = new int[2];
            sv.getLocationOnScreen(loc);
            android.graphics.Matrix m = new android.graphics.Matrix();
            m.setTranslate(loc[0], loc[1]);
            CursorAnchorInfo.Builder b = new CursorAnchorInfo.Builder()
                    .setMatrix(m)
                    .setInsertionMarkerLocation(imeRect[0], imeRect[1],
                                                imeRect[1] + imeRect[3],
                                                imeRect[1] + imeRect[3],
                                                CursorAnchorInfo.FLAG_HAS_VISIBLE_REGION);
            if (!compText.isEmpty()) {
                b.setComposingText(compAt, compText);
                b.setSelectionRange(compCursor, compCursor);
            }
            try {
                imm.updateCursorAnchorInfo(hiddenInput, b.build());
            } catch (IllegalArgumentException e) {
                Log.w(TAG, "win " + id + ": cursor anchor info rejected: " + e.getMessage());
            }
        }
    }

    private void clearComposing() {
        compText = "";
        compCursor = 0;
        notifyImeState();
    }

    /* ---- Virtual editor (IME's view: surText + compText inserted at surCursor) ---- */

    private String editorText() {
        int c = Math.min(surCursor, surText.length());
        if (compText.isEmpty()) return surText;
        return surText.substring(0, c) + compText + surText.substring(c);
    }

    private int editorSelStart() {
        return Math.min(surCursor, surText.length()) + compCursor;
    }

    /** Editor index → client surrounding index (subtract the composing-length offset) */
    private int toSurroundingIndex(int editorIdx) {
        int c = Math.min(surCursor, surText.length());
        if (editorIdx <= c) return editorIdx;
        return Math.max(c, editorIdx - compText.length());
    }

    /* ---- UTF-16 ↔ UTF-8 conversion (protocol byte offsets ↔ Java char indices) ---- */

    private static int utf8Len(String s) {
        int n = 0;
        for (int i = 0; i < s.length(); ) {
            int cp = s.codePointAt(i);
            n += cp <= 0x7F ? 1 : cp <= 0x7FF ? 2 : cp <= 0xFFFF ? 3 : 4;
            i += Character.charCount(cp);
        }
        return n;
    }

    /** Byte offset → char index (stops on a code point boundary) */
    private static int byteToChar(String s, int byteOff) {
        if (byteOff <= 0) return 0;
        int chars = 0, bytes = 0;
        for (int i = 0; i < s.length(); ) {
            int cp = s.codePointAt(i);
            int b = cp <= 0x7F ? 1 : cp <= 0x7FF ? 2 : cp <= 0xFFFF ? 3 : 4;
            if (bytes + b > byteOff) return chars;
            bytes += b;
            int c = Character.charCount(cp);
            chars += c;
            i += c;
        }
        return chars;
    }

    /** Preedit cursor byte offset (newCp is relative to the end of the composing text, 1 = past the end; counted in code points) */
    private static int preeditCursorBytes(String text, int newCp) {
        int total = text.codePointCount(0, text.length());
        int pos = Math.max(0, Math.min(total, total + 1 - newCp));
        int bytes = 0, seen = 0;
        for (int i = 0; i < text.length() && seen < pos; ) {
            int cp = text.codePointAt(i);
            bytes += cp <= 0x7F ? 1 : cp <= 0x7FF ? 2 : cp <= 0xFFFF ? 3 : 4;
            seen++;
            i += Character.charCount(cp);
        }
        return bytes;
    }

    /** Preedit cursor char offset (for the local mirror) */
    private static int composingCursorChars(String text, int newCp) {
        int total = text.codePointCount(0, text.length());
        int pos = Math.max(0, Math.min(total, total + 1 - newCp));
        int chars = 0, seen = 0;
        for (int i = 0; i < text.length() && seen < pos; ) {
            int cp = text.codePointAt(i);
            int c = Character.charCount(cp);
            chars += c;
            seen++;
            i += c;
        }
        return chars;
    }

    /** Snap an index to a code point boundary (forward: absorb the second half of a surrogate pair) */
    private static int snap(String s, int idx) {
        if (idx <= 0) return 0;
        if (idx >= s.length()) return s.length();
        if (Character.isHighSurrogate(s.charAt(idx - 1))
                && Character.isLowSurrogate(s.charAt(idx)))
            return idx + 1;
        return idx;
    }

    /** Snap an index to a code point boundary (backward: retreat to the first half of a surrogate pair) */
    private static int snapBack(String s, int idx) {
        if (idx <= 0) return 0;
        if (idx >= s.length()) return s.length();
        if (Character.isHighSurrogate(s.charAt(idx - 1))
                && Character.isLowSurrogate(s.charAt(idx)))
            return idx - 1;
        return idx;
    }

    /* chars / code points around the cursor → bytes (converted on the client's surrounding) */
    private int bytesBefore(int units) {
        if (units <= 0) return 0;
        int start = snapBack(surText, Math.max(0, surCursor - units));
        return utf8Len(surText.substring(start, surCursor));
    }
    private int bytesAfter(int units) {
        if (units <= 0) return 0;
        int end = snap(surText, Math.min(surText.length(), surCursor + units));
        return utf8Len(surText.substring(surCursor, end));
    }
    private int bytesBeforeCp(int cps) {
        if (cps <= 0) return 0;
        int i = surCursor, cnt = 0;
        while (i > 0 && cnt < cps) {
            i = snapBack(surText, i - 1);
            cnt++;
        }
        return utf8Len(surText.substring(i, surCursor));
    }
    private int bytesAfterCp(int cps) {
        if (cps <= 0) return 0;
        int i = surCursor, cnt = 0;
        while (i < surText.length() && cnt < cps) {
            i = snap(surText, i + Character.charCount(surText.codePointAt(i)));
            cnt++;
        }
        return utf8Len(surText.substring(surCursor, i));
    }

    /**
     * Full-table InputConnection bridge (Android IME ↔ binder passthrough to
     * the text-input protocol). Output: commitText/setComposingText/
     * deleteSurrounding… → WlBinder.ime ONEWAY; queries:
     * getTextBeforeCursor/getCursorCapsMode/… ← state cache
     * (segmentation/prediction context).
     */
    private class WlInputConnection extends BaseInputConnection {
        WlInputConnection(View target) {
            super(target, false);
        }

        /* --- text output --- */

        @Override
        public boolean commitText(CharSequence text, int newCursorPosition) {
            String t = text == null ? "" : text.toString();
            WlBinder.ime(id, WlBinder.IME_COMMIT, 0, 0, t);
            clearComposing();
            return true;
        }

        @Override
        public boolean setComposingText(CharSequence text, int newCursorPosition) {
            String t = text == null ? "" : text.toString();
            int cb = preeditCursorBytes(t, newCursorPosition);
            WlBinder.ime(id, WlBinder.IME_PREEDIT, cb, cb, t);
            compText = t;
            compCursor = composingCursorChars(t, newCursorPosition);
            notifyImeState();
            return true;
        }

        @Override
        public boolean finishComposingText() {
            if (!compText.isEmpty()) {
                WlBinder.ime(id, WlBinder.IME_PREEDIT, 0, 0, "");
                clearComposing();
            }
            return true;
        }

        @Override
        public boolean deleteSurroundingText(int beforeLength, int afterLength) {
            WlBinder.ime(id, WlBinder.IME_DELETE,
                    bytesBefore(beforeLength), bytesAfter(afterLength), "");
            return true;
        }

        @Override
        public boolean deleteSurroundingTextInCodePoints(int beforeLength, int afterLength) {
            WlBinder.ime(id, WlBinder.IME_DELETE,
                    bytesBeforeCp(beforeLength), bytesAfterCp(afterLength), "");
            return true;
        }

        @Override
        public boolean commitCompletion(CompletionInfo text) {
            if (text != null) return commitText(text.getText(), 1);
            return true;
        }

        @Override
        public boolean commitCorrection(CorrectionInfo correctionInfo) {
            if (correctionInfo == null) return true;
            /* offset = start of the old text (editor coordinates); delete the old text + commit the new */
            int sel = editorSelStart();
            int off = correctionInfo.getOffset();
            String old = String.valueOf(correctionInfo.getOldText());
            String et = editorText();
            int a = snap(et, Math.max(0, Math.min(off, off + old.length())));
            int b = snap(et, Math.min(et.length(), off + old.length()));
            if (b > a) {
                WlBinder.ime(id, WlBinder.IME_DELETE,
                        utf8Len(et.substring(Math.min(a, sel), Math.min(b, sel))),
                        utf8Len(et.substring(Math.max(a, sel), Math.max(b, sel))), "");
            }
            WlBinder.ime(id, WlBinder.IME_COMMIT, 0, 0,
                    String.valueOf(correctionInfo.getNewText()));
            clearComposing();
            return true;
        }

        @Override
        public boolean setComposingRegion(int start, int end) {
            if (!compText.isEmpty()) {
                WlBinder.ime(id, WlBinder.IME_PREEDIT, 0, 0, "");
                compText = "";
                compCursor = 0;
            }
            /* the region becomes preedit (long-press re-segmentation / transform); the deletion is computed around the cursor */
            String et = editorText();
            int a = snap(et, Math.max(0, Math.min(start, end)));
            int b = snap(et, Math.min(et.length(), Math.max(start, end)));
            if (a >= b) return true;
            String region = et.substring(a, b);
            int c = Math.min(surCursor, surText.length());
            WlBinder.ime(id, WlBinder.IME_DELETE,
                    utf8Len(et.substring(Math.min(a, c), Math.min(b, c))),
                    utf8Len(et.substring(Math.max(a, c), Math.max(b, c))), "");
            WlBinder.ime(id, WlBinder.IME_PREEDIT,
                    utf8Len(region), utf8Len(region), region);
            surCursor = surAnchor = toSurroundingIndex(a);
            compText = region;
            compCursor = region.length();
            notifyImeState();
            return true;
        }

        @Override
        public boolean setSelection(int start, int end) {
            /* v1 cursor_position; v3 has no matching event (the client's state push self-heals) */
            String et = editorText();
            int a = toSurroundingIndex(snap(et, Math.max(0, Math.min(start, end))));
            int b = toSurroundingIndex(snap(et, Math.min(et.length(), Math.max(start, end))));
            WlBinder.ime(id, WlBinder.IME_CURSOR, a, b, "");
            surCursor = a;
            surAnchor = b;
            notifyImeState();
            return true;
        }

        @Override
        public boolean replaceText(int start, int end, CharSequence text,
                                   int newCursorPosition, TextAttribute textAttribute) {
            finishComposingText();
            String et = editorText();
            int a = snap(et, Math.max(0, Math.min(start, end)));
            int b = snap(et, Math.min(et.length(), Math.max(start, end)));
            int c = Math.min(surCursor, surText.length());
            if (b > a) {
                WlBinder.ime(id, WlBinder.IME_DELETE,
                        utf8Len(et.substring(Math.min(a, c), Math.min(b, c))),
                        utf8Len(et.substring(Math.max(a, c), Math.max(b, c))), "");
                surCursor = surAnchor = toSurroundingIndex(a);
            }
            return commitText(text, newCursorPosition);
        }

        /* --- context queries (data source for IME segmentation/prediction; answered locally) --- */

        @Override
        public CharSequence getTextBeforeCursor(int length, int flags) {
            String et = editorText();
            int sel = editorSelStart();
            int start = snapBack(et, Math.max(0, sel - Math.max(0, length)));
            return et.subSequence(start, sel);
        }

        @Override
        public CharSequence getTextAfterCursor(int length, int flags) {
            String et = editorText();
            int sel = editorSelStart();
            int end = snap(et, Math.min(et.length(), sel + Math.max(0, length)));
            return et.subSequence(sel, end);
        }

        @Override
        public CharSequence getSelectedText(int flags) {
            if (surCursor == surAnchor || surText.isEmpty()) return null;
            int a = Math.min(surCursor, surAnchor), b = Math.max(surCursor, surAnchor);
            return surText.substring(a, b);
        }

        @Override
        public SurroundingText getSurroundingText(int beforeLength, int afterLength, int flags) {
            String et = editorText();
            int sel = editorSelStart();
            int startPos = snapBack(et, Math.max(0, sel - Math.max(0, beforeLength)));
            int endPos = snap(et, Math.min(et.length(), sel + Math.max(0, afterLength)));
            return new SurroundingText(et.substring(startPos, endPos),
                                       sel - startPos, sel - startPos, startPos);
        }

        @Override
        public ExtractedText getExtractedText(ExtractedTextRequest request, int flags) {
            ExtractedText t = new ExtractedText();
            t.text = editorText();
            t.startOffset = 0;
            t.selectionStart = editorSelStart();
            t.selectionEnd = editorSelStart();
            return t;
        }

        @Override
        public int getCursorCapsMode(int reqModes) {
            return TextUtils.getCapsMode(editorText(), editorSelStart(), reqModes);
        }

        /* --- misc --- */

        @Override
        public boolean requestCursorUpdates(int cursorUpdateMode) {
            return true;   /* every state push already does updateCursorAnchorInfo */
        }

        @Override
        public void closeConnection() {
            compText = "";
            compCursor = 0;
            super.closeConnection();
        }
    }

    /* ---- Input passthrough: Android dispatch callbacks → binder literal
     *      translation into wayland events ----
     * Routing authority = Android (an event reaching this Activity belongs to
     * this window); the daemon keeps zero routing state.
     * Event type values = awl.h AWL_IN_* (do not change); BTN_* = evdev codes. */

    private static final int PTR_ENTER = 1, PTR_LEAVE = 2, PTR_MOTION = 3,
            PTR_BUTTON = 4, PTR_AXIS = 5, PTR_REL = 6,
            KEY = 9, TOUCH_DOWN = 11, TOUCH_MOTION = 12, TOUCH_UP = 13,
            TOUCH_CANCEL = 14;

    /* ---- Pointer capture (zwp_pointer_constraints_v1 state sync) ----
     * The daemon only mirrors the client's constraint state (C_CAPTURE:
     * mode + confine rect in view px — see the daemon's pure-sync design);
     * activation and limiting are entirely local:
     *   NONE    — normal mode: absolute positions, rel = absolute-position
     *             diff (the uncaptured AXIS_RELATIVE_* is always 0);
     *   CONFINE — requestPointerCapture + a virtual clamped position:
     *             confinex/y accumulates AXIS_RELATIVE_* inside the rect and
     *             the clamped point is sent as absolute motion;
     *   LOCK    — requestPointerCapture, relative-only: AXIS_RELATIVE_*
     *             deltas, no absolute motion (the client cursor stays frozen
     *             where capture started; buttons anchor at that point).
     * The system silently breaks capture on focus loss; onWindowFocusChanged
     * re-requests it (the daemon never re-sends C_CAPTURE for that — the
     * constraint object lives on, its mirror is unchanged).
     * Benign race: C_CAPTURE (ctrl node) and input events (input node) are
     * unordered across binder nodes — for at most one event around a mode
     * switch the old mode's classification applies. */
    private int captureMode = CAPTURE_NONE;
    private final int[] capRect = new int[4];   /* confine region, view px (zeros = whole window) */
    private float confinex, confiney;           /* virtual clamped position (CONFINE) */

    /* confine box = capRect ∩ live window (invalid/zero rect = whole window;
     * a degenerate intersection collapses to its lower edge) */
    private float confLoX() { return capRect[2] > 0 ? Math.max(0, capRect[0]) : 0f; }
    private float confLoY() { return capRect[3] > 0 ? Math.max(0, capRect[1]) : 0f; }
    private float confHiX() {
        return capRect[2] > 0 ? Math.max(confLoX(),
                Math.min(lastW - 1f, capRect[0] + capRect[2] - 1f)) : lastW - 1f;
    }
    private float confHiY() {
        return capRect[3] > 0 ? Math.max(confLoY(),
                Math.min(lastH - 1f, capRect[1] + capRect[3] - 1f)) : lastH - 1f;
    }

    /** New constraint state from the daemon (request / set_region / destroy):
     *  adopt the mode + rect, anchor the virtual position, (re)capture. */
    private void setPointerCaptureMode(int mode, int rx, int ry, int rw, int rh) {
        captureMode = mode;
        capRect[0] = rx; capRect[1] = ry; capRect[2] = rw; capRect[3] = rh;
        if (mode == CAPTURE_CONFINE) {
            /* anchor inside the box: the current pointer when known, else the box center */
            float ax = Float.isNaN(lastMouseX) ? (confLoX() + confHiX()) / 2f : lastMouseX;
            float ay = Float.isNaN(lastMouseY) ? (confLoY() + confHiY()) / 2f : lastMouseY;
            confinex = limitRange(confLoX(), confHiX(), ax);
            confiney = limitRange(confLoY(), confHiY(), ay);
        } else if (mode == CAPTURE_LOCK) {
            /* buttons anchor at the frozen position (window center if unknown) */
            if (Float.isNaN(lastMouseX)) lastMouseX = lastW / 2f;
            if (Float.isNaN(lastMouseY)) lastMouseY = lastH / 2f;
        }
        applyPointerCapture();
        Log.i(TAG, "win " + id + ": pointer capture mode=" + mode
                + " rect=" + rx + "," + ry + " " + rw + "x" + rh);
    }

    /** (Re-)request/release the system capture without touching the anchor
     *  (focus regained: capture broke silently, the mode is unchanged). */
    private void applyPointerCapture() {
        android.view.View v = getWindow().getDecorView();
        if (captureMode != CAPTURE_NONE) v.requestPointerCapture();
        else v.releasePointerCapture();
    }

    @Override
    public void onPointerCaptureChanged(boolean hasCapture) {
        super.onPointerCaptureChanged(hasCapture);
        Log.i(TAG, "win " + id + ": pointer capture " + (hasCapture ? "granted" : "lost")
                + " (mode=" + captureMode + ")");   /* log-only: the daemon owns the mode */
    }

    /* ---- Client cursor (wl_pointer.set_cursor) ----
     * While the wl client drives the cursor the daemon composites the
     * client's cursor image itself (topmost layer of this window, following
     * the pointer) — or the client asked for an invisible pointer — so the
     * Android system pointer must not be drawn on top of it. The icon is
     * resolved by ViewRootImpl.updatePointerIcon through the view hierarchy
     * on every mouse event (View.onResolvePointerIcon → mMousePointerIcon)
     * and re-resolved on HOVER_ENTER/EXIT (mResolvedPointerIcon reset), so
     * setting a persistent TYPE_NULL icon on the views under the pointer is
     * enough; setPointerIcon also triggers refreshPointerIcon() at once.
     * PointerIcon.TYPE_NULL = "Null icon. It has no bitmap" (PointerIcon.java)
     * = the documented way to hide the mouse pointer. Restored (null =
     * default arrow) when the daemon reports the client cursor gone (leave /
     * client exit). Per Activity instance — a re-attached window starts
     * visible and the client re-sets its cursor on the next enter. */
    private boolean ptrHidden;

    private void setPointerHidden(boolean hidden) {
        if (ptrHidden == hidden) return;
        ptrHidden = hidden;
        PointerIcon icon = hidden ? PointerIcon.getSystemIcon(this, PointerIcon.TYPE_NULL) : null;
        if (sv != null) sv.setPointerIcon(icon);
        if (root != null) root.setPointerIcon(icon);
        Log.i(TAG, "win " + id + ": android pointer " + (hidden ? "hidden (client cursor)" : "restored"));
    }

    /* ---- Idle inhibitor (zwp_idle_inhibit_manager_v1, C_KEEPON) ----
     * daemon-owned state (pure mirror sync, same model as pointer capture):
     * keep the screen on while a wl client holds an idle inhibitor on a
     * surface of this window. The window flag is the whole implementation
     * here: the system honors it only while the window is visible
     * (minimized = window gone = flag inert; occluded = not adopted) —
     * exactly the protocol's "inhibitor honored on a visible surface"
     * semantics, so there is no visibility bookkeeping on this side. No
     * permission needed (unlike PowerManager.WakeLock) and the flag dies
     * with the window automatically; a fresh Activity instance gets the
     * state re-pushed by the daemon on re-attach. */
    private void applyKeepOn(boolean on) {
        if (isFinishing()) return;
        if (on) getWindow().addFlags(
                android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        else getWindow().clearFlags(
                android.view.WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON);
        Log.i(TAG, "win " + id + ": keep-screen-on " + (on ? "on (idle inhibitor)" : "off"));
    }

    private static final int BTN_LEFT = 0x110, BTN_RIGHT = 0x111,
            BTN_MIDDLE = 0x112, BTN_FORWARD = 0x115, BTN_BACK = 0x116;

    /* Mouse detection (verbatim from legacy isMouseEvent: pure source bits,
     * no tool check) — this device's mouse hover events report toolType
     * FINGER (measured: src=0x2002 tool=1); filtering by TOOL_TYPE_MOUSE
     * would drop them and swallow them. Physical mouse and touchpad are one
     * inseparable stream here (both src=0x2002+FINGER on this device) — they
     * are split by gesture classification instead (two-finger = scroll,
     * multi-finger/pinch = touch passthrough, single contact = virtual
     * mouse), exactly the legacy structure. */
    private static boolean isMouse(MotionEvent ev) {
        return (ev.getSource() & InputDevice.SOURCE_CLASS_POINTER) != 0
                && ev.getSource() != InputDevice.SOURCE_TOUCHSCREEN;
    }

    /* ---- Touchpad gestures (#30, classified within the shared pointer
     *      stream — legacy structure) ----
     * System gesture classification (MotionEvent.getClassification, device
     * on SDK 36), dispatched BEFORE the virtual-mouse default so a
     * classified gesture never falls into pointer emulation:
     *   two-finger swipe (3) → scroll: AXIS_GESTURE_SCROLL_*_DISTANCE pixel
     *                distances, code=1, finger source (the daemon forwards
     *                raw values; no notches×10/discrete conversion);
     *   multi-finger swipe (4) / pinch (5) → touch passthrough (the original
     *                touch path; three-finger swipe/pinch belong to the client);
     *   rest (single contact / physical mouse) → virtual mouse: enter/motion
     *                + buttonState diffing — the system synthesizes
     *                two-finger taps into BUTTON_SECONDARY (right click) /
     *                single tap = PRIMARY.
     * Scroll sign: see handleTouchpadScroll / onGenericMotionEvent (AOSP
     * axis definitions vs wayland axis convention). */
    private static final int CLS_TWO_FINGER_SWIPE = 3;
    private static final int CLS_MULTI_FINGER_SWIPE = 4;
    private static final int CLS_PINCH = 5;

    private boolean padInWin;      /* pointer enter/leave pairing (protocol requires enter first) */

    private static boolean isTouchpad(MotionEvent ev) {
        return (ev.getSource() & InputDevice.SOURCE_CLASS_POINTER) != 0
                && ev.getSource() != InputDevice.SOURCE_TOUCHSCREEN
                && ev.getToolType(0) == MotionEvent.TOOL_TYPE_FINGER;
    }

    /** Pointer stream teardown: release leftover buttons + leave (shared by
     *  classification switches to touch passthrough, gesture end, hover
     *  exit, and lifecycle interruptions). */
    private void endPadStream() {
        if (mouseSavedBS != 0) {
            int diff = mouseSavedBS;
            mouseSavedBS = 0;
            for (int[] b : PAD_BUTTON_MAP)
                if ((diff & b[0]) != 0)
                    WlBinder.input(id, PTR_BUTTON, b[1], lastMouseX, lastMouseY, 0, 0, 0);
        }
        if (padInWin) {
            padInWin = false;
            WlBinder.input(id, PTR_LEAVE, 0, 0, 0, 0, 0, 0);
        }
        lastMouseX = lastMouseY = Float.NaN;   /* no rel diff across a leave */
    }

    /** Make up the pointer enter at the mode-valid position (NONE = raw event
     *  position, CONFINE = the virtual clamped position, LOCK = the frozen
     *  anchor, window center when unknown) — protocol requires enter before
     *  motion/button/axis. */
    private void padEnter(MotionEvent ev) {
        padInWin = true;
        switch (captureMode) {
        case CAPTURE_LOCK:
            if (Float.isNaN(lastMouseX)) lastMouseX = lastW / 2f;
            if (Float.isNaN(lastMouseY)) lastMouseY = lastH / 2f;
            break;
        case CAPTURE_CONFINE:
            lastMouseX = confinex;
            lastMouseY = confiney;
            break;
        default:
            lastMouseX = ev.getX();
            lastMouseY = ev.getY();
            break;
        }
        WlBinder.input(id, PTR_ENTER, 0, lastMouseX, lastMouseY, 0, 0, 0);
    }

    /* ---- Main pointer handling (legacy handleMouseEvent = the virtual
     *      mouse for physical-mouse events AND single-finger touchpad
     *      contact) ----
     * dx/dy = this event's endpoint - the last delivered endpoint (diff over
     * the sent event stream) — the telescoping sum ≡ total displacement,
     * independent of how Android batches/coalesces samples.
     * (In-batch history diffing measurably loses increments on this device:
     * history excludes the last delivered point; anchoring on either the
     * first or last history point made the red crosshair slow.)
     * Enter pairing: enters on the first event (a touchpad tap has no
     * preceding hover-enter) and stays after lift — leave happens only on
     * hover-exit / classification switches / teardown, legacy's
     * always-inside pointer model.
     * Buttons = buttonState diff (savedBS XOR current) — the touch stream
     * (drag/tap) and the generic stream (BUTTON_PRESS/RELEASE) are two input
     * paths sharing one differ: whoever arrives first raises the edge,
     * deduplicated naturally. */
    private int mouseSavedBS;
    private float lastMouseX = Float.NaN, lastMouseY = Float.NaN;

    private float limitRange(float a, float b, float val){
        if(val<a) return a;
        if(val>b) return b;
        return val;
    }

    /* Three-mode dispatch (see the capture block above): NONE = absolute +
     * abs-diff rel; LOCK = AXIS_RELATIVE_* only, position frozen at the
     * capture point; CONFINE = AXIS_RELATIVE_* accumulated into the clamped
     * virtual position, sent as absolute motion. lastMouseX/Y always hold
     * the position the client believes the pointer is at. */
    private void handleMouseEvent(MotionEvent ev) {
        float ex = ev.getX(), ey = ev.getY();
        float dx, dy;
        boolean sendabs;
        float x, y;
        switch (captureMode) {
        case CAPTURE_LOCK:
            /* captured: only the relative axes are populated (sum over the
             * batch); no absolute motion — the client cursor stays frozen */
            dx = sumAxis(ev, MotionEvent.AXIS_RELATIVE_X);
            dy = sumAxis(ev, MotionEvent.AXIS_RELATIVE_Y);
            if (Float.isNaN(lastMouseX)) lastMouseX = lastW / 2f;
            if (Float.isNaN(lastMouseY)) lastMouseY = lastH / 2f;
            sendabs = false;
            x = lastMouseX; y = lastMouseY;
            break;
        case CAPTURE_CONFINE:
            dx = sumAxis(ev, MotionEvent.AXIS_RELATIVE_X);
            dy = sumAxis(ev, MotionEvent.AXIS_RELATIVE_Y);
            confinex = limitRange(confLoX(), confHiX(), confinex + dx);
            confiney = limitRange(confLoY(), confHiY(), confiney + dy);
            sendabs = true;
            x = lastMouseX = confinex;
            y = lastMouseY = confiney;
            break;
        default:   /* CAPTURE_NONE — uncaptured AXIS_RELATIVE_* is always 0 */
            dx = Float.isNaN(lastMouseX) ? 0f : ex - lastMouseX;
            dy = Float.isNaN(lastMouseY) ? 0f : ey - lastMouseY;
            sendabs = true;
            x = lastMouseX = ex;
            y = lastMouseY = ey;
            break;
        }
        if (!padInWin)
            padEnter(ev);   /* no prior hover (touchpad tap without hover) → make up the enter */
        if (dx != 0f || dy != 0f)
            WlBinder.input(id, PTR_REL, 0, dx, dy, 0, 0, 0);
        if (sendabs)
            WlBinder.input(id, PTR_MOTION, 0, x, y, 0, 0, 0);
        int bs = ev.getButtonState();
        int diff = mouseSavedBS ^ bs;
        if (diff != 0) {
            for (int[] b : PAD_BUTTON_MAP)
                if ((diff & b[0]) != 0)
                    WlBinder.input(id, PTR_BUTTON, b[1], x, y,
                            (bs & b[0]) != 0 ? 1 : 0, 0, 0);
            mouseSavedBS = bs;
        }
    }

    private static final int[][] PAD_BUTTON_MAP = {   /* {android bit, evdev code} */
        {MotionEvent.BUTTON_PRIMARY,   BTN_LEFT},
        {MotionEvent.BUTTON_SECONDARY, BTN_RIGHT},
        {MotionEvent.BUTTON_TERTIARY,  BTN_MIDDLE},
        {MotionEvent.BUTTON_BACK,      BTN_BACK},
        {MotionEvent.BUTTON_FORWARD,   BTN_FORWARD},
    };

    /** Sum of an axis over the batched historical samples + the current one.
     *  AXIS_GESTURE_SCROLL_*_DISTANCE is per-sample, not accumulated
     *  (MotionEvent.java: "developers should make sure to process this axis
     *  value for all batched historical samples") — reading only the current
     *  sample drops distance whenever Android coalesces MOVEs. */
    private static float sumAxis(MotionEvent ev, int axis) {
        float s = 0f;
        for (int i = 0; i < ev.getHistorySize(); i++)
            s += ev.getHistoricalAxisValue(axis, 0, i);
        return s + ev.getAxisValue(axis);
    }

    /** Two-finger scroll (classification 3): MOVE yields pixel distances;
     *  UP/CANCEL = gesture end → a zero-delta finger axis event (the daemon
     *  turns it into axis_stop = fling start on the client). The pointer
     *  stream is NOT torn down: kwin sends no leave around a scroll gesture,
     *  and Android follows the UP with a HOVER_ENTER at the real cursor
     *  position anyway.
     *  Sign: AXIS_GESTURE_SCROLL_*_DISTANCE is "the distance that should be
     *  scrolled" (GestureConverter::handleScroll stores -finger delta, i.e. a
     *  scroll-position delta: positive = page scrolls down/right) — identical
     *  to the wayland axis convention (positive = down/right), so it is
     *  forwarded verbatim. Position is NOT passed: the fake finger the gesture
     *  converter synthesizes drifts from the cursor by the accumulated delta
     *  (PointerChoreographer adds the cursor position to the gesture offset),
     *  so using it as a motion would move the client's pointer during the
     *  scroll; the client keeps scrolling what is under the pointer.
     *  The classifier may tag cls=3 from the very first event (skipping the
     *  first finger DOWN's enter) → make up an enter before the axis
     *  (protocol requires axis to follow enter). */
    private void handleTouchpadScroll(MotionEvent ev) {
        int a = ev.getActionMasked();
        if (a == MotionEvent.ACTION_UP || a == MotionEvent.ACTION_CANCEL) {
            if (padInWin)
                WlBinder.input(id, PTR_AXIS, 1, 0, 0, 0, 0, 0);   /* v=h=0 finger → axis_stop */
            return;
        }
        if (a != MotionEvent.ACTION_MOVE) return;
        if (!padInWin)
            padEnter(ev);   /* the classifier may tag cls=3 from the first event → enter before the axis */
        float v = sumAxis(ev, MotionEvent.AXIS_GESTURE_SCROLL_Y_DISTANCE);
        float h = sumAxis(ev, MotionEvent.AXIS_GESTURE_SCROLL_X_DISTANCE);
        if (v != 0 || h != 0)
            WlBinder.input(id, PTR_AXIS, 1, v, h, 0, 0, 0);
    }

    /** Touchscreen (touch id = code; coordinates = SurfaceView-local,
     *  fullscreen means window-local). Pointer-class non-touchscreen
     *  (mouse/touchpad, one stream on this device): dispatch by gesture
     *  classification FIRST, virtual mouse as the default — two-finger =
     *  scroll, multi-finger/pinch = touch passthrough, else (single
     *  contact / physical mouse) = handleMouseEvent. */
    @Override
    public boolean dispatchTouchEvent(MotionEvent ev) {
        if (isMouse(ev)) {
            int cls = ev.getClassification();
            if (cls == CLS_TWO_FINGER_SWIPE) {
                handleTouchpadScroll(ev);
                return true;
            }
            if (cls != CLS_MULTI_FINGER_SWIPE && cls != CLS_PINCH) {
                handleMouseEvent(ev);   /* whole drag/tap: motion + rel diff + button diff */
                return true;
            }
            /* multi-finger swipe / pinch → touch passthrough. The first finger
             * already went through pointer emulation's enter/motion: send its
             * touch down (the client gets a complete touch stream) and then
             * close the pointer stream. */
            if (padInWin) {
                WlBinder.input(id, TOUCH_DOWN, ev.getPointerId(0),
                        ev.getX(0), ev.getY(0), 0, 0, 0);
                endPadStream();
            }
        }
        switch (ev.getActionMasked()) {
        case MotionEvent.ACTION_DOWN:
            sendTouch(TOUCH_DOWN, ev, ev.getActionIndex());
            return true;
        case MotionEvent.ACTION_POINTER_DOWN:
            sendTouch(TOUCH_DOWN, ev, ev.getActionIndex());
            return true;
        case MotionEvent.ACTION_MOVE:
            for (int i = 0; i < ev.getPointerCount(); i++)
                sendTouch(TOUCH_MOTION, ev, i);
            return true;
        case MotionEvent.ACTION_POINTER_UP:
            sendTouch(TOUCH_UP, ev, ev.getActionIndex());
            return true;
        case MotionEvent.ACTION_UP:
            sendTouch(TOUCH_UP, ev, ev.getActionIndex());
            return true;
        case MotionEvent.ACTION_CANCEL:
            for (int i = 0; i < ev.getPointerCount(); i++)
                WlBinder.input(id, TOUCH_CANCEL, ev.getPointerId(i), 0, 0, 0, 0, 0);
            return true;
        default:
            return super.dispatchTouchEvent(ev);
        }
    }

    private void sendTouch(int type, MotionEvent ev, int idx) {
        WlBinder.input(id, type, ev.getPointerId(idx),
                       ev.getX(idx), ev.getY(idx), 0, 0, 0);
    }

    /** Mouse/touchpad generic events: hover enter/move/exit + all buttons
     *  (BUTTON_* incl. right/middle) + wheel/finger scroll.
     *  Wheel (physical mouse): Android AXIS_VSCROLL is normalized -1.0 (down)
     *  .. 1.0 (up) and AXIS_HSCROLL -1.0 (left) .. 1.0 (right)
     *  (MotionEvent.java); wayland axis values live in the motion coordinate
     *  space (positive = down / right, wayland.xml wl_pointer.axis) → the
     *  vertical notch is negated, the horizontal one forwarded verbatim
     *  (code=0, notches; the daemon does ×10 + axis_discrete). This matches
     *  the legacy anland pipeline, which negated wheel v as well.
     *  An ACTION_SCROLL carrying AXIS_GESTURE_SCROLL_*_DISTANCE (touchpad
     *  scroll semantics on this input path) is forwarded as raw pixel
     *  distances (code=1, same sign rule as the classification-3 path).
     *  System-synthesized touchpad clicks may arrive as BUTTON_PRESS/
     *  RELEASE — through the same savedBS differ as the touch stream (if
     *  the touch stream already reported them, diff=0, deduplicated
     *  naturally). */
    @Override
    public boolean onGenericMotionEvent(MotionEvent ev) {
        Log.d(TAG, "generic act=" + ev.getActionMasked() + " src=0x"
                + Integer.toHexString(ev.getSource()) + " tool=" + ev.getToolType(0)
                + " hist=" + ev.getHistorySize());
        /* Capture 模式下事件源不同:requestPointerCapture 后系统停掉 hover 合成
         * (PointerChoreographer 不再产出 0x2002 鼠标流),触摸板原始流以
         * ACTION_MOVE + SOURCE_TOUCHPAD(0x100008) 到达,相对量在
         * AXIS_RELATIVE_X/Y — isMouse() 的 0x2002 判定不再命中,按捕获态放行
         * (NONE 模式照旧忽略,两条流不会重复处理)。 */
        boolean capturedPad = captureMode != CAPTURE_NONE
                && ev.isFromSource(InputDevice.SOURCE_TOUCHPAD);
        if (!isMouse(ev) && !capturedPad)
            return super.onGenericMotionEvent(ev);
        switch (ev.getActionMasked()) {
        case MotionEvent.ACTION_MOVE:
            /* captured 原始触摸板流(单指移动 = 虚拟鼠标;两指 = 滚动) */
            if (ev.getClassification() == CLS_TWO_FINGER_SWIPE) {
                handleTouchpadScroll(ev);
                return true;
            }
            if (ev.getClassification() == CLS_MULTI_FINGER_SWIPE
                    || ev.getClassification() == CLS_PINCH)
                return true;   /* captured: no touch-passthrough stream, swallow */
            handleMouseEvent(ev);
            return true;
        case MotionEvent.ACTION_HOVER_ENTER:
            if (padInWin) {   /* pointer already inside (made up by a scroll/tap) → protocol forbids a 2nd enter, treat as motion */
                handleMouseEvent(ev);
                return true;
            }
            padEnter(ev);
            return true;
        case MotionEvent.ACTION_HOVER_MOVE:
            handleMouseEvent(ev);   /* motion + rel diff + button diff (enter already paired by HOVER_ENTER) */
            return true;
        case MotionEvent.ACTION_HOVER_EXIT:
            if (captureMode != CAPTURE_NONE)
                return true;   /* captured: the pointer cannot leave the window (this exit is capture-start churn) */
            endPadStream();   /* leave + release leftovers + reset the rel anchor */
            return true;
        case MotionEvent.ACTION_SCROLL: {
            if (!padInWin)   /* same as the scroll-classification path: enter before the axis */
                padEnter(ev);
            float fv = sumAxis(ev, MotionEvent.AXIS_GESTURE_SCROLL_Y_DISTANCE);
            float fh = sumAxis(ev, MotionEvent.AXIS_GESTURE_SCROLL_X_DISTANCE);
            if (fv != 0 || fh != 0) {   /* touchpad pixel distances: finger source, no position (see handleTouchpadScroll) */
                WlBinder.input(id, PTR_AXIS, 1, fv, fh, 0, 0, 0);
                return true;
            }
            float v = ev.getAxisValue(MotionEvent.AXIS_VSCROLL);
            float h = ev.getAxisValue(MotionEvent.AXIS_HSCROLL);
            if (v != 0 || h != 0) {
                /* wheel notches; v1/v2 = pointer position for the synthesized
                 * surface-coord motion in the axis frame: LOCK = 0,0 (no
                 * motion — the client cursor is frozen), CONFINE = the
                 * clamped virtual position, NONE = raw */
                float px = 0f, py = 0f;
                if (captureMode == CAPTURE_CONFINE) { px = confinex; py = confiney; }
                else if (captureMode == CAPTURE_NONE) { px = ev.getX(); py = ev.getY(); }
                WlBinder.input(id, PTR_AXIS, 0, -v, h, px, py, 0);
            }
            return true;
        }
        case MotionEvent.ACTION_BUTTON_PRESS:
        case MotionEvent.ACTION_BUTTON_RELEASE:
            handleMouseEvent(ev);   /* ensure-enter + savedBS differ (dedups with the touch stream) */
            return true;
        default:
            return super.onGenericMotionEvent(ev);
        }
    }

    /** Keyboard: evdev scan code forwarded verbatim (matches the embedded
     *  keymap), meta as-is (the daemon decodes the Android meta bits).
     *  Volume keys stay with the system. Android's synthetic key repeats
     *  are swallowed (legacy behavior: repeat is the client's own job under
     *  wayland; forwarding them would re-press a held key).
     *  Soft-keyboard/IME synthesized keys have scanCode=0 → map the keycode
     *  to an evdev code. */
    @Override
    public boolean dispatchKeyEvent(KeyEvent ev) {
        int kc = ev.getKeyCode();
        if (kc == KeyEvent.KEYCODE_VOLUME_UP || kc == KeyEvent.KEYCODE_VOLUME_DOWN
                || kc == KeyEvent.KEYCODE_VOLUME_MUTE)
            return super.dispatchKeyEvent(ev);
        if (ev.getAction() == KeyEvent.ACTION_DOWN && ev.getRepeatCount() > 0)
            return true;   /* synthetic repeat — swallow (see above) */
        int sc = ev.getScanCode();
        if (sc == 0) sc = fallbackSc(kc);
        if (sc > 0) {
            WlBinder.input(id, KEY, sc, 0, 0,
                           ev.getAction() == KeyEvent.ACTION_DOWN ? 1 : 0, 0,
                           ev.getMetaState());
            return true;
        }
        return super.dispatchKeyEvent(ev);
    }

    /** IME-synthesized key (scanCode=0) → evdev code (matches the embedded keymap) */
    private static int fallbackSc(int keyCode) {
        switch (keyCode) {
        case KeyEvent.KEYCODE_ENTER:
        case KeyEvent.KEYCODE_NUMPAD_ENTER: return 0x1c;
        case KeyEvent.KEYCODE_DEL:          return 0x0e;   /* Android DEL = backspace */
        case KeyEvent.KEYCODE_FORWARD_DEL:  return 0x6f;
        case KeyEvent.KEYCODE_TAB:          return 0x0f;
        case KeyEvent.KEYCODE_ESCAPE:       return 0x01;
        case KeyEvent.KEYCODE_SPACE:        return 0x39;
        case KeyEvent.KEYCODE_DPAD_UP:      return 0x67;
        case KeyEvent.KEYCODE_DPAD_DOWN:    return 0x6c;
        case KeyEvent.KEYCODE_DPAD_LEFT:    return 0x69;
        case KeyEvent.KEYCODE_DPAD_RIGHT:   return 0x6a;
        case KeyEvent.KEYCODE_PAGE_UP:      return 0x68;
        case KeyEvent.KEYCODE_PAGE_DOWN:    return 0x6d;
        case KeyEvent.KEYCODE_MOVE_HOME:    return 0x66;
        case KeyEvent.KEYCODE_MOVE_END:     return 0x6b;
        default:                            return 0;
        }
    }

    @Override
    protected void onDestroy() {
        LIVE.remove(id);
        if (deathLinked) {
            WlBinder.unmonitorDeath(daemonDeath);
            deathLinked = false;
        }
        /* No detach report: swiping away / killing the background always
         * minimizes and keeps the window alive (onPause already sent PAUSE;
         * process death is backstopped by the daemon-side binder death).
         * The only close entry = the window-list long-press menu (daemon
         * T_CLOSE) → client closes the window → C_CLOSE → finish */
        attached = false;
        super.onDestroy();
    }
}
