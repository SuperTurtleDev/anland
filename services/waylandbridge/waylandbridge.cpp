/* waylandbridge.cpp — root ELF daemon (core of the new architecture)
 *
 * Launched by su -c / SukiSU module service.sh; a native root resident
 * process — out of reach of OPlus Hans-style app freezing (background
 * kills only target app uids).
 *
 * Responsibilities:
 *   - wayland host (logic layer) + GPU rendering (renderer; all resources
 *     held by the daemon)
 *   - window state table: alive (wayland window exists) / attached
 *     (Activity holds a surface)
 *   - Attach = am start WlWindowActivity(--el id) (bring-to-front if
 *     already present)
 *   - single-attach model (APK only reports facts; all decisions in the
 *     daemon):
 *       SURFACE  id,w,h,Surface parcel,death token[,host] (render target;
 *                re-attach auto-evicts the old holder → orders it to kill
 *                itself via the old ctrl; single foreground)
 *       PAUSE    id[,host] (onPause → daemon fully detaches, ONEWAY)
 *       RESIZE   id,w,h
 *       LIST     → window list (id,title,attached)
 *       BRING    id → am start (list tap; SURFACE re-sent on onResume is
 *                the re-attach)
 *       CLOSE    id → graceful client exit (list long-press menu, the
 *                sole window-close entry)
 *   - APP abnormal death: AIBinder_linkToDeath(death token) → kernel
 *     callback auto-detaches all windows of that process (minimize
 *     semantics, wayland window kept alive)
 *   - window_destroyed (client quit on its own) → ctrl/broadcast tells
 *     the Activity to finish
 */
#include "awl.h"
#include "awl_renderer.hpp"

#include <android/binder_ibinder.h>
#include <android/binder_parcel.h>
#include <android/native_window_aidl.h>
#include <android/log.h>

#include <dlfcn.h>

#define AWL_TAG "anland-daemon"
#include "awl_log.h"   /* LOGI/LOGE/LOGD (LOGD compiled out unless AWL_LOG_DEBUG) */

/* NDK r29 app stub lacks AServiceManager_addService / ABinderProcess_startThreadPool
 * (platform-only exports; the device's /system/lib64/libbinder_ndk.so does have
 * them — the root ELF resolves them at runtime via dlopen) */
typedef binder_status_t (*awl_asms_fn)(AIBinder*, const char*);
typedef void (*awl_bstp_fn)(void);
static awl_asms_fn g_addService;
static awl_bstp_fn g_startThreadPool;

static bool binder_plat_init(void) {
    void* dl = dlopen("libbinder_ndk.so", RTLD_NOW);
    if (!dl) { LOGE("dlopen libbinder_ndk: %s", dlerror()); return false; }
    g_addService = (awl_asms_fn)dlsym(dl, "AServiceManager_addService");
    g_startThreadPool = (awl_bstp_fn)dlsym(dl, "ABinderProcess_startThreadPool");
    if (!g_addService || !g_startThreadPool) {
        LOGE("dlsym platform binder symbols failed");
        return false;
    }
    return true;
}

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/system_properties.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#define AWL_BINDER_NAME "anland.host"
#define AWL_PKG  "com.anlandnext"
#define AWL_WIN_ACT AWL_PKG "/.WlWindowActivity"

/* binder transaction codes (agreed with the APP BinderProxy)
 * single-attach model: the APK only reports facts; detach/evict/close
 * decisions all live in the daemon.
 * host:i64 (Activity instance id) is a trailing optional field of
 * SURFACE/PAUSE. */
enum {
    AWL_T_SURFACE = 1,   /* (id:i64 w:i32 h:i32 SurfaceParcel CtrlBinder [host:i64])
                            → ok:i32; re-attach auto-evicts the old holder (C_CLOSE) */
    AWL_T_RESIZE  = 3,   /* (id:i64 w:i32 h:i32) */
    AWL_T_LIST    = 4,   /* () → count:i32 { id:i64 attached:i32 title:string16 } */
    AWL_T_BRING   = 5,   /* (id:i64) → ok:i32 */
    AWL_T_PAUSE   = 6,   /* (id:i64 [host:i64]) Activity onPause → daemon fully detaches
                            (ONEWAY; minimize semantics, wayland window kept alive) */
    AWL_T_FOCUS   = 8,   /* (id:i64 has:i32) focus change → configure ACTIVATED + keyboard focus */
    AWL_T_INPUT   = 9,   /* (id:i64 type:i32 code:i32 x:f y:f v1:f v2:f meta:i32 flags:i32)
                           → logic layer sends straight to the wayland input protocol (ONEWAY hot path) */
    AWL_T_IME     = 10,  /* (id:i64 op:i32 a:i32 b:i32 text:string16)
                           → text-input protocol events sent straight through (ONEWAY; op per awl.h AWL_IME_*) */
    AWL_T_CLIPBOARD = 11, /* (id:i64 text:string16) Android clipboard text → wl
                           selection (ONEWAY; pushed after the focused Activity reads it, #29) */
    AWL_T_CFG_GET  = 12, /* (key:string16) → val:i32 (daemon-owned config entry, #31) */
    AWL_T_CFG_SET  = 13, /* (key:string16 val:i32) → ok:i32; apply + atomically persist
                           config.json (daemon is the single source of truth; APK only reads/writes values) */
    AWL_T_CLOSE   = 14,  /* (id:i64) → ok:i32; list long-press "close": request graceful
                            client exit (xdg toplevel.close / X WM_DELETE_WINDOW) */
};

/* control channel (daemon → Activity, delivered over the binder object
 * reported with SURFACE) */
#define AWL_C_CLOSE 1        /* client window destroyed → Activity kills itself and exits */
#define AWL_C_TITLE 2         /* (title:string16) title update → Recents label sync */
#define AWL_C_IME_SHOW 3      /* (hint:i32 purpose:i32) client text field gained focus → show soft keyboard */
#define AWL_C_IME_HIDE 4      /* () text field lost focus → hide soft keyboard */
#define AWL_C_IME_STATE 5     /* (hint purpose cursor anchor cx cy cw ch flags:i32×8
                                 + text:string16) editor state snapshot → IME context/candidate window */
#define AWL_C_CLIP_WRITE 6    /* (text:string16) wl client set_selection → this Activity
                                 writes the Android clipboard (empty string = clear; echo suppressed by the APK) */
#define AWL_C_CAPTURE 7       /* (mode x y w h:i32×5) pointer-constraints
                                 activation (mode = 1 confine / 2 lock →
                                 requestPointerCapture; 0 none → release; x,y,w,h
                                 = confine region in view pixels, zeros = whole
                                 window). While captured the Activity delivers
                                 AWL_IN_PTR_REL and, for confine, synthesizes
                                 the clamped absolute motion itself */
#define AWL_C_CURSOR 8        /* (hidden:i32) client took over the cursor via
                                 wl_pointer.set_cursor (image composited by the
                                 renderer on top of the window, or NULL = invisible)
                                 → Activity hides the Android pointer
                                 (setPointerIcon TYPE_NULL); 0 = restore it */
#define AWL_C_KEEPON 9        /* (on:i32) zwp_idle_inhibit_manager_v1 aggregate
                                 flipped: 1 → Activity sets
                                 FLAG_KEEP_SCREEN_ON (the window flag is only
                                 honored while the window is visible = the
                                 protocol's visible-surface semantics), 0 →
                                 clears it */
#define AWL_CTRL_DESC "anland.ICtrl"

/* ---------------- window state table ---------------- */

struct awl_win_state {
    char title[256];
    bool attached;        /* Activity holds a surface (render target exists) */
    int64_t host = 0;     /* current holder Activity instance id (SURFACE-reported; 0=unknown) */
    bool kbd_focus = false;   /* window holds keyboard focus (mirror of T_FOCUS; used to synthesize leave on detach) */
    AIBinder* ctrl;       /* control channel binder (proxy of the Activity's CtrlBinder) */

    /* IME lifecycle (mirror of the client text_input state; kept alive
     * across detach):
     * on re-attach the input state is still there → re-send
     * C_IME_SHOW(+state snapshot) to reopen the input method. */
    bool ime_active;
    uint32_t ime_hint, ime_purpose;
    char ime_text[4001];  /* UTF-8 surrounding (client set_surrounding_text) */
    int32_t ime_cursor, ime_anchor;   /* byte offsets (Activity side converts to chars) */
    int32_t ime_cx, ime_cy, ime_cw, ime_ch;   /* cursor rectangle (surface coords) */

    /* Pointer constraint (mirror of the logic-layer zwp_pointer_constraints
     * state; kept alive across detach like the IME mirror): on re-attach the
     * constraint may still be active (persistent lifetime) → re-send
     * C_CAPTURE so the new Activity instance captures again. */
    int capture_mode = 0;   /* AWL_CAPTURE_* (0 = none) */
    int32_t cap_rect[4] = {0, 0, 0, 0};   /* confine region, view pixels */

    /* Idle inhibitor (mirror of the logic-layer zwp_idle_inhibit aggregate;
     * kept alive across detach like the capture mirror): on re-attach the
     * new Activity instance has no window flag yet → re-send C_KEEPON. */
    bool keep_on = false;
};

static std::mutex g_state_lock;
static std::map<uint64_t, awl_win_state> g_wins;

/* death token → associated windows (APP process → multiple windows) */
struct death_link {
    AIBinder* token;
    std::vector<uint64_t> ids;
};
static std::vector<death_link*> g_links;   /* guarded by g_state_lock */

/* Any window still attached? (caller holds g_state_lock) — foreground
 * scheduling keeps the daemon boosted while the attach count is non-zero */
static bool any_attached_locked(void) {
    for (auto& [id, ws] : g_wins)
        if (ws.attached) return true;
    return false;
}

/* Full detach (minimize semantics: wayland window kept alive, render
 * resources/control channel fully torn down).
 * pause / evict / process death / window destroy all take this path.
 * Order: renderer teardown (lock-free, join outside the lock) → state
 * reset → focus-loss event outside the lock (awl_window_set_activated
 * goes through rwl+ev_lock+socket flush; must not hold g_state_lock). */
static void detach_window(uint64_t id) {
    bool had_kbd = false;
    bool sched_drop = false;      /* was attached → restore the client's cgroups */
    bool sched_none_left = false; /* this detach emptied the attach set → self falls back */
    awl_renderer_attach(id, nullptr);      /* free GL resources (window and texture state stays inside the renderer) */
    {
        std::lock_guard<std::mutex> lk(g_state_lock);
        auto it = g_wins.find(id);
        if (it != g_wins.end()) {
            sched_drop = it->second.attached;
            it->second.attached = false;
            had_kbd = it->second.kbd_focus;
            it->second.kbd_focus = false;
            /* the control channel peer (CtrlBinder) dies with detach; drop the reference */
            if (it->second.ctrl) {
                AIBinder_decStrong(it->second.ctrl);
                it->second.ctrl = nullptr;
            }
        }
        sched_none_left = sched_drop && !any_attached_locked();   /* attach count hit 0 → self falls back */
    }
    /* foreground scheduling restore (cgroup IO + /proc walk: outside g_state_lock;
     * the wayland window is alive here, so the id still resolves to its client) */
    if (sched_drop) {
        pid_t p = awl_window_client_pid(id);
        if (p > 0) awl_sched_set(p, 0);
        if (sched_none_left) awl_sched_set(getpid(), 0);
    }
    /* only synthesize leave when it held keyboard focus: avoids a duplicate
     * leave after T_FOCUS(false) was already sent, and the side effect of
     * tr_kbd_leave clearing datadev focus even for an unmatched window */
    if (had_kbd) {
        awl_window_set_activated(id, 0);
        awl_input_ev_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.id = id;
        ev.type = AWL_IN_KBD_LEAVE;
        awl_input_dispatch(&ev);
    }
    LOGI("window %llu detached (minimized, wayland window kept alive)", (unsigned long long)id);
}

/* ---------------- control channel (daemon → Activity) ---------------- */

static AIBinder_Class* k_ctrl_class = nullptr;

static void* ctrl_on_create(void* args) { return args; }
static void ctrl_on_destroy(void* userData) {}
static binder_status_t ctrl_on_transact(AIBinder* b, transaction_code_t code,
                                        const AParcel* in, AParcel* out) {
    return STATUS_UNKNOWN_TRANSACTION;   /* daemon accepts no Activity-direction commands */
}

/* Send one control command (oneway, no waiting). ctrl must be the binder
 * proxy reported with SURFACE. If str is non-null, append a string16
 * payload (e.g. the title for C_TITLE). */
/* Failed control sends are logged (rate-limited per code): the channel failed
 * silently for weeks because the APK's CtrlBinder did not publish the
 * "anland.ICtrl" descriptor — AIBinder_associateClass() compares it against
 * the remote's INTERFACE_TRANSACTION answer and prepareTransaction refuses a
 * class-less proxy (2026-09-10). */
static void ctrl_fail(transaction_code_t code, const char* stage, binder_status_t st) {
    static std::atomic<int> n{0};
    int k = n.fetch_add(1);
    if (k < 20 || (k % 100) == 0)
        LOGE("ctrl code=%u: %s failed st=%d (APK CtrlBinder descriptor / dead Activity?) [#%d]",
             (unsigned)code, stage, (int)st, k + 1);
}

/* AIBinder_transact takes ownership of *in and REQUIRES a non-null out
 * parcel even for FLAG_ONEWAY (libbinder_ndk: "requires non-null parameters
 * binder, in, and out" → STATUS_UNEXPECTED_NULL, nothing is sent) — passing
 * nullptr was the second reason the control channel never delivered
 * anything (2026-09-10). The reply parcel is empty for oneway; delete it. */
static bool ctrl_transact(AIBinder* ctrl, transaction_code_t code, AParcel** in) {
    AParcel* out = nullptr;
    binder_status_t st = AIBinder_transact(ctrl, code, in, &out, FLAG_ONEWAY);
    if (out) AParcel_delete(out);
    if (st != STATUS_OK) { ctrl_fail(code, "transact", st); return false; }
    return true;
}

static bool ctrl_send(AIBinder* ctrl, transaction_code_t code, const char* str = nullptr) {
    if (!ctrl || !k_ctrl_class) return false;
    if (!AIBinder_associateClass(ctrl, k_ctrl_class)) {   /* required before prepareTransaction writes the token */
        ctrl_fail(code, "associateClass", STATUS_INVALID_OPERATION);
        return false;
    }
    AParcel* in = nullptr;
    binder_status_t st = AIBinder_prepareTransaction(ctrl, &in);
    if (st != STATUS_OK || !in) { ctrl_fail(code, "prepareTransaction", st); return false; }
    if (str && AParcel_writeString(in, str, (int32_t)strlen(str)) != STATUS_OK) {
        AParcel_delete(in);
        return false;
    }
    return ctrl_transact(ctrl, code, &in);
}

/* Control command (int payload + optional trailing string16 payload; field order matches what CtrlBinder reads) */
static bool ctrl_send_ints(AIBinder* ctrl, transaction_code_t code,
                           const int32_t* ints, size_t nints, const char* str = nullptr) {
    if (!ctrl || !k_ctrl_class) return false;
    if (!AIBinder_associateClass(ctrl, k_ctrl_class)) {
        ctrl_fail(code, "associateClass", STATUS_INVALID_OPERATION);
        return false;
    }
    AParcel* in = nullptr;
    binder_status_t st = AIBinder_prepareTransaction(ctrl, &in);
    if (st != STATUS_OK || !in) { ctrl_fail(code, "prepareTransaction", st); return false; }
    for (size_t i = 0; i < nints; i++)
        if (AParcel_writeInt32(in, ints[i]) != STATUS_OK) { AParcel_delete(in); return false; }
    if (str && AParcel_writeString(in, str, (int32_t)strlen(str)) != STATUS_OK) {
        AParcel_delete(in);
        return false;
    }
    return ctrl_transact(ctrl, code, &in);
}

static void on_token_died(void* cookie) {
    death_link* dl = (death_link*)cookie;
    std::vector<uint64_t> ids;
    {
        std::lock_guard<std::mutex> lk(g_state_lock);
        ids = dl->ids;
        for (auto it = g_links.begin(); it != g_links.end(); ++it) {
            if (*it == dl) { g_links.erase(it); break; }
        }
        /* only detach windows still held by this token: stale link members
         * whose ctrl was swapped on re-attach must not kill the current
         * holder (detach itself is idempotent; the guard is for semantic
         * correctness only) */
        for (auto it = ids.begin(); it != ids.end();) {
            auto w = g_wins.find(*it);
            if (w == g_wins.end() || w->second.ctrl != dl->token) it = ids.erase(it);
            else ++it;
        }
    }
    LOGE("APP process died (binder death): auto-detaching %zu windows", ids.size());
    for (uint64_t id : ids) detach_window(id);
    AIBinder_decStrong(dl->token);   /* return the reference held for linkToDeath */
    delete dl;
}

/* ---------------- subprocess execution (am commands) ---------------- */

static void run_am(const char* fmt, ...) {
    char cmd[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(cmd, sizeof(cmd), fmt, ap);
    va_end(ap);
    pid_t pid = fork();
    if (pid < 0) return;
    if (pid == 0) {
        for (int fd = 0; fd < 3; fd++) (void)!close(fd);
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) { dup2(nul, 0); dup2(nul, 1); dup2(nul, 2); }
        execl("/system/bin/sh", "sh", "-c", cmd, (char*)NULL);
        _exit(127);
    }
    int st;
    waitpid(pid, &st, 0);
}

/* title → shell single-quote safe */
static void shell_quote(const char* in, char* out, size_t n) {
    size_t o = 0;
    out[o++] = '\'';
    for (size_t i = 0; in[i] && o + 4 < n; i++) {
        if (in[i] == '\'') { out[o++] = '\''; out[o++] = '\\'; out[o++] = '\''; out[o++] = '\''; }
        else out[o++] = in[i];
    }
    out[o++] = '\'';
    out[o] = 0;
}

static void attach_activity(uint64_t id, const char* title) {
    char q[600];
    shell_quote(title ? title : "", q, sizeof(q));
    /* exactly one am start in document mode (a double start creates two
       instances for the same window, one of which lingers as a placeholder
       after the window is destroyed):
       documentLaunchMode="intoExisting" + per-id unique data URI →
       same id bring-to-fronts the existing task; different ids each get
       their own instance/task (--activity-new-document is not recognized
       by this device's am, use -f NEW_TASK|NEW_DOCUMENT=0x90000000) */
    run_am("am start -n %s -d 'anland://win/%llu' --el id %llu --es title %s "
           "-f 0x90000000 >/dev/null 2>&1",
           AWL_WIN_ACT, (unsigned long long)id, (unsigned long long)id, q);
    LOGI("Attach: am start WlWindowActivity id=%llu", (unsigned long long)id);
}

/* ---------------- wayland logic-layer callbacks (wayland event thread) ---------------- */

/* ---- mini-wm control channel (X-side operations on Xwayland windows, #32) ----
 * The in-container mini-wm listens on /host/data/local/tmp/anland-wm.sock
 * (= /data/local/tmp/anland-wm.sock on this side); one connection per
 * command, line-text protocol:
 *   S <serial> <w> <h>   resize the X window (serial = WL_SURFACE_SERIAL pairing value)
 *   C <serial>           request close (WM_DELETE_WINDOW, or XKillClient if unsupported)
 * With no mini-wm (pure wayland client scenario) the connect fails —
 * skip silently, warn only once. */
#define AWL_WM_SOCK "/data/local/tmp/anland-wm.sock"
static void xwm_send_cmd(const char* cmd, size_t len) {
    static std::atomic<time_t> warned{0};   /* reachable from multiple binder threads */
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return;
    struct sockaddr_un sa = {};
    sa.sun_family = AF_UNIX;
    strncpy(sa.sun_path, AWL_WM_SOCK, sizeof(sa.sun_path) - 1);
    if (connect(fd, (struct sockaddr*)&sa, sizeof(sa)) != 0) {
        time_t now = time(NULL);
        time_t last = warned.load(std::memory_order_relaxed);
        if (now - last > 60) {   /* no Xwayland session is the norm; don't spam */
            warned.store(now, std::memory_order_relaxed);
            LOGI("mini-wm channel unreachable (%s) — Xwayland window resize/close skipped",
                 strerror(errno));
        }
        close(fd);
        return;
    }
    if (write(fd, cmd, len) < 0) LOGE("xwm cmd write: %s", strerror(errno));
    close(fd);
}
/* Xwayland window: Android window size change → resize its X window to
 * the same size (passive model: a client that doesn't comply keeps its old
 * buffer; the renderer just stretches as usual) */
static void xwm_resize_window(uint64_t id, int32_t w, int32_t h) {
    uint64_t serial = 0;
    if (!awl_xwayland_window_serial(id, &serial)) return;
    char cmd[96];
    int n = snprintf(cmd, sizeof(cmd), "S %llu %d %d\n",
                     (unsigned long long)serial, w, h);
    xwm_send_cmd(cmd, (size_t)n);
}
static void xwm_close_window(uint64_t id) {
    uint64_t serial = 0;
    if (!awl_xwayland_window_serial(id, &serial)) return;
    char cmd[48];
    int n = snprintf(cmd, sizeof(cmd), "C %llu\n", (unsigned long long)serial);
    xwm_send_cmd(cmd, (size_t)n);
}

static void cb_window_created(void* user, uint64_t id, int32_t pref_w, int32_t pref_h,
                              const char* title, int is_popup) {
    LOGI("window %llu created %dx%d popup=%d '%s'",
         (unsigned long long)id, pref_w, pref_h, is_popup, title ? title : "");
    {
        std::lock_guard<std::mutex> lk(g_state_lock);
        awl_win_state& ws = g_wins[id];
        snprintf(ws.title, sizeof(ws.title), "%s", title ? title : "");
        ws.attached = false;
    }
    /* am start = fork+waitpid (hundreds of ms) — run on a detached thread
     * so the event thread doesn't stall for it; if the window dies right
     * after, SURFACE will be rejected (Activity kills itself) */
    std::string title_s(title ? title : "");
    std::thread([id, title_s] { attach_activity(id, title_s.c_str()); }).detach();
}

static void cb_window_destroyed(void* user, uint64_t id) {
    LOGI("window %llu destroyed", (unsigned long long)id);
    awl_renderer_attach(id, nullptr);
    AIBinder* ctrl = nullptr;
    bool sched_drop = false;      /* was attached → restore the client's cgroups */
    bool sched_none_left = false; /* the destroy emptied the attach set → self falls back */
    {
        std::lock_guard<std::mutex> lk(g_state_lock);
        auto it = g_wins.find(id);
        if (it != g_wins.end()) {
            ctrl = it->second.ctrl;
            it->second.ctrl = nullptr;    /* ownership transferred to this send */
            sched_drop = it->second.attached;
        }
        g_wins.erase(id);
        sched_none_left = sched_drop && !any_attached_locked();
        for (death_link* dl : g_links) {
            for (auto it2 = dl->ids.begin(); it2 != dl->ids.end(); ++it2)
                if (*it2 == id) { dl->ids.erase(it2); break; }
        }
    }
    /* foreground scheduling restore: this callback still runs before the
     * logic layer unlinks the surface, so the id resolves to its client even
     * on the client-death path (resources die before the wl_client) */
    if (sched_drop) {
        pid_t p = awl_window_client_pid(id);
        if (p > 0) awl_sched_set(p, 0);
        if (sched_none_left) awl_sched_set(getpid(), 0);
    }
    /* tell the Activity to finish over the control channel (client already
       closed the window); on failure fall back to an explicit broadcast
       (targetSdk>=26 manifest receivers miss implicit broadcasts, -n required) */
    bool sent = ctrl_send(ctrl, AWL_C_CLOSE);
    if (ctrl) AIBinder_decStrong(ctrl);
    if (!sent)
        run_am("am broadcast -a anland.WINDOW_GONE -n %s/.WindowGoneReceiver --el id %llu "
               ">/dev/null 2>&1",
               AWL_PKG, (unsigned long long)id);
    else
        LOGI("window %llu: CLOSE sent via ctrl channel", (unsigned long long)id);
}

static void cb_window_title(void* user, uint64_t id, const char* title) {
    AIBinder* ctrl = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_state_lock);
        auto it = g_wins.find(id);
        if (it == g_wins.end() || !title || !title[0]) return;
        snprintf(it->second.title, sizeof(it->second.title), "%s", title);
        if (it->second.ctrl) {
            ctrl = it->second.ctrl;
            AIBinder_incStrong(ctrl);   /* keep alive for the out-of-lock transact */
        }
    }
    /* clients that retitle dynamically (terminals/browsers) → Recents label follows */
    if (ctrl) {
        ctrl_send(ctrl, AWL_C_TITLE, title);
        AIBinder_decStrong(ctrl);
    }
}

/* ---------------- IME bridge (text-input protocol ↔ Android IME, passthrough model) ---- */

/* window ctrl snapshot (taken under g_state_lock, transacted outside the lock: incStrong keeps it alive) */
static AIBinder* ctrl_of(uint64_t id) {
    std::lock_guard<std::mutex> lk(g_state_lock);
    auto it = g_wins.find(id);
    if (it == g_wins.end() || !it->second.ctrl) return nullptr;
    AIBinder_incStrong(it->second.ctrl);
    return it->second.ctrl;
}

static void cb_ime_show(void* user, uint64_t id, uint32_t hint, uint32_t purpose) {
    LOGI("window %llu: ime show (hint=%u purpose=%u)",
         (unsigned long long)id, hint, purpose);
    {
        std::lock_guard<std::mutex> lk(g_state_lock);
        auto it = g_wins.find(id);
        if (it == g_wins.end()) return;
        it->second.ime_active = true;
        it->second.ime_hint = hint;
        it->second.ime_purpose = purpose;
    }
    AIBinder* ctrl = ctrl_of(id);
    if (ctrl) {
        int32_t args[2] = { (int32_t)hint, (int32_t)purpose };
        ctrl_send_ints(ctrl, AWL_C_IME_SHOW, args, 2);
        AIBinder_decStrong(ctrl);
    }
}

static void cb_ime_hide(void* user, uint64_t id) {
    LOGI("window %llu: ime hide", (unsigned long long)id);
    {
        std::lock_guard<std::mutex> lk(g_state_lock);
        auto it = g_wins.find(id);
        if (it == g_wins.end()) return;
        it->second.ime_active = false;
    }
    AIBinder* ctrl = ctrl_of(id);
    if (ctrl) {
        ctrl_send_ints(ctrl, AWL_C_IME_HIDE, nullptr, 0);
        AIBinder_decStrong(ctrl);
    }
}

static void cb_ime_state(void* user, uint64_t id, const char* text,
                         int32_t cursor, int32_t anchor,
                         uint32_t hint, uint32_t purpose,
                         int32_t cx, int32_t cy, int32_t cw, int32_t ch,
                         uint32_t flags) {
    {
        std::lock_guard<std::mutex> lk(g_state_lock);
        auto it = g_wins.find(id);
        if (it == g_wins.end()) return;
        snprintf(it->second.ime_text, sizeof(it->second.ime_text), "%s",
                 text ? text : "");
        it->second.ime_cursor = cursor;
        it->second.ime_anchor = anchor;
        it->second.ime_hint = hint;
        it->second.ime_purpose = purpose;
        it->second.ime_cx = cx; it->second.ime_cy = cy;
        it->second.ime_cw = cw; it->second.ime_ch = ch;
    }
    AIBinder* ctrl = ctrl_of(id);
    if (ctrl) {
        int32_t args[9] = { (int32_t)hint, (int32_t)purpose, cursor, anchor,
                            cx, cy, cw, ch, (int32_t)flags };
        ctrl_send_ints(ctrl, AWL_C_IME_STATE, args, 9, text ? text : "");
        AIBinder_decStrong(ctrl);
    }
}

/* Re-attach: input state kept alive (text_input still enabled at detach
 * time) → re-send show + state snapshot so the new Activity instance
 * restores the IME context instantly (segmentation/prediction).
 * Called at the end of SURFACE handling (ctrl already registered). */
static void ime_reopen_on_attach(uint64_t id) {
    AIBinder* ctrl = nullptr;
    awl_win_state snap;
    {
        std::lock_guard<std::mutex> lk(g_state_lock);
        auto it = g_wins.find(id);
        if (it == g_wins.end() || !it->second.ime_active || !it->second.ctrl)
            return;
        snap = it->second;
        ctrl = it->second.ctrl;
        AIBinder_incStrong(ctrl);
    }
    int32_t show[2] = { (int32_t)snap.ime_hint, (int32_t)snap.ime_purpose };
    ctrl_send_ints(ctrl, AWL_C_IME_SHOW, show, 2);
    int32_t state[9] = { (int32_t)snap.ime_hint, (int32_t)snap.ime_purpose,
                         snap.ime_cursor, snap.ime_anchor,
                         snap.ime_cx, snap.ime_cy, snap.ime_cw, snap.ime_ch, 0 };
    ctrl_send_ints(ctrl, AWL_C_IME_STATE, state, 9, snap.ime_text);
    AIBinder_decStrong(ctrl);
    LOGI("window %llu: ime reopened on attach (input state kept alive)",
         (unsigned long long)id);
}

/* Pointer constraint state (zwp_pointer_constraints_v1 activate/deactivate):
 * tell the matching Activity to requestPointerCapture/releasePointerCapture
 * with the mode + confine region (view pixels). The mirror is updated FIRST,
 * with or without a live ctrl — an unattached window (paused / app died)
 * keeps the state so a later attach re-pushes it. */
static void cb_pointer_lock(void* user, uint64_t id, int mode,
                            int32_t rx, int32_t ry, int32_t rw, int32_t rh) {
    {
        std::lock_guard<std::mutex> lk(g_state_lock);
        auto it = g_wins.find(id);
        if (it == g_wins.end()) return;
        it->second.capture_mode = mode;
        it->second.cap_rect[0] = rx; it->second.cap_rect[1] = ry;
        it->second.cap_rect[2] = rw; it->second.cap_rect[3] = rh;
    }
    AIBinder* ctrl = ctrl_of(id);
    if (!ctrl) return;   /* no Activity attached: mirror only (state re-sent on attach) */
    int32_t args[5] = { mode, rx, ry, rw, rh };
    ctrl_send_ints(ctrl, AWL_C_CAPTURE, args, 5);
    AIBinder_decStrong(ctrl);
    LOGI("window %llu pointer constraint mode=%d rect=%d,%d %dx%d",
         (unsigned long long)id, mode, rx, ry, rw, rh);
}

/* Re-attach: the logic-layer constraint outlived the detach (persistent
 * lifetime, or the app died while locked) → re-send the capture state so the
 * new Activity instance captures again before any motion is delivered.
 * Called at the end of SURFACE handling (ctrl already registered). */
static void capture_reopen_on_attach(uint64_t id) {
    AIBinder* ctrl = nullptr;
    int mode = 0;
    int32_t r[4] = {0, 0, 0, 0};
    {
        std::lock_guard<std::mutex> lk(g_state_lock);
        auto it = g_wins.find(id);
        if (it == g_wins.end() || it->second.capture_mode == 0 || !it->second.ctrl)
            return;
        mode = it->second.capture_mode;
        memcpy(r, it->second.cap_rect, sizeof(r));
        ctrl = it->second.ctrl;
        AIBinder_incStrong(ctrl);
    }
    int32_t args[5] = { mode, r[0], r[1], r[2], r[3] };
    ctrl_send_ints(ctrl, AWL_C_CAPTURE, args, 5);
    AIBinder_decStrong(ctrl);
    LOGI("window %llu: capture mode=%d re-pushed on attach",
         (unsigned long long)id, mode);
}

/* Idle inhibitor state (zwp_idle_inhibit aggregate flip): tell the matching
 * Activity to set/clear FLAG_KEEP_SCREEN_ON. Mirror updated FIRST, with or
 * without a live ctrl (an unattached window keeps the state so a later
 * attach re-pushes it — same shape as cb_pointer_lock). */
static void cb_idle_inhibit(void* user, uint64_t id, int on) {
    {
        std::lock_guard<std::mutex> lk(g_state_lock);
        auto it = g_wins.find(id);
        if (it == g_wins.end()) return;
        it->second.keep_on = on != 0;
    }
    AIBinder* ctrl = ctrl_of(id);
    if (!ctrl) return;   /* no Activity attached: mirror only (re-sent on attach) */
    int32_t args[1] = { on ? 1 : 0 };
    ctrl_send_ints(ctrl, AWL_C_KEEPON, args, 1);
    AIBinder_decStrong(ctrl);
    LOGI("window %llu keep-screen-on %s (idle inhibitor)",
         (unsigned long long)id, on ? "on" : "off");
}

/* Re-attach: an inhibitor outlived the detach (the client kept the object,
 * e.g. a paused player) → re-send C_KEEPON; the fresh Activity instance has
 * no window flag yet. Called at the end of SURFACE handling (ctrl already
 * registered). */
static void keep_on_reopen_on_attach(uint64_t id) {
    AIBinder* ctrl = nullptr;
    {
        std::lock_guard<std::mutex> lk(g_state_lock);
        auto it = g_wins.find(id);
        if (it == g_wins.end() || !it->second.keep_on || !it->second.ctrl)
            return;
        ctrl = it->second.ctrl;
        AIBinder_incStrong(ctrl);
    }
    int32_t args[1] = { 1 };
    ctrl_send_ints(ctrl, AWL_C_KEEPON, args, 1);
    AIBinder_decStrong(ctrl);
    LOGI("window %llu: keep-screen-on re-pushed on attach", (unsigned long long)id);
}

/* Client cursor (wl_pointer.set_cursor): hidden=1 → the renderer now draws
 * the client's cursor image (or the client wants an invisible pointer) →
 * the Activity hides the Android pointer for this window; 0 → restore. Only
 * transitions arrive. No Activity attached: drop — a fresh Activity starts
 * with the system pointer visible and the client re-sets its cursor on the
 * next enter. */
static void cb_pointer_cursor(void* user, uint64_t id, int hidden) {
    AIBinder* ctrl = ctrl_of(id);
    if (!ctrl) return;
    int32_t args[1] = { hidden ? 1 : 0 };
    ctrl_send_ints(ctrl, AWL_C_CURSOR, args, 1);
    AIBinder_decStrong(ctrl);
    LOGI("window %llu android pointer %s", (unsigned long long)id,
         hidden ? "hidden (client cursor)" : "restored");
}

static void cb_window_dirty(void* user, uint64_t id) {
    /* on detach (incl. pause) there is no render entry → no-op: send no
     * frame_done; the client naturally parks in eglSwapBuffers waiting —
     * zero-cost keep-alive */
    awl_renderer_request_render(id);   /* set flag + wake this window's render thread, returns immediately */
}

/* ---- clipboard bridge (#29; logic-layer data thread callback) ----
 * wl→Android: the owning window's Activity writes the clipboard on its
 * behalf (with no Activity attached, fall back to any live ctrl — writing
 * does not require that window to be foreground). */
static void cb_clipboard_text(void* user, uint64_t win, const char* utf8) {
    const char* t = utf8 ? utf8 : "";
    LOGI("clipboard wl→android: win=%llu %zu bytes '%s'",
         (unsigned long long)win, strlen(t),
         strlen(t) > 24 ? "(trunc)" : t);
    AIBinder* ctrl = win ? ctrl_of(win) : nullptr;
    if (!ctrl) {
        std::lock_guard<std::mutex> lk(g_state_lock);
        for (auto& [id, ws] : g_wins) {
            if (ws.ctrl) {
                ctrl = ws.ctrl;
                AIBinder_incStrong(ctrl);
                break;
            }
        }
    }
    if (ctrl) {
        ctrl_send(ctrl, AWL_C_CLIP_WRITE, t);
        AIBinder_decStrong(ctrl);
    } else {
        LOGE("clipboard wl→android: no attached Activity — dropped");
    }
}

static awl_window_callbacks_t k_cbs = {
    .user = nullptr,
    .window_created = cb_window_created,
    .window_destroyed = cb_window_destroyed,
    .window_title = cb_window_title,
    .pointer_lock = cb_pointer_lock,
    .window_dirty = cb_window_dirty,
    .ime_show = cb_ime_show,
    .ime_hide = cb_ime_hide,
    .ime_state = cb_ime_state,
    .clipboard_text = cb_clipboard_text,
    .pointer_cursor = cb_pointer_cursor,
    .idle_inhibit = cb_idle_inhibit,
};

/* ---------------- daemon config (config.json, #31) ----------------
 * Daemon-relevant config (zoom, initial-configure size) is daemon-owned:
 * read and applied at startup; on set, applied + atomically persisted
 * (tmp+rename). The APK stores nothing itself — it only reads/writes values
 * over binder.
 * MANUAL-ONLY key: "runtime_dir" (wayland socket dir, default
 * /data/local/tmp/awl). Never exposed over binder (cfg_set rejects it) —
 * hand-edited in config.json, read at daemon startup before the socket is
 * bound; a change needs a daemon restart. Daemon-driven saves preserve its
 * file value verbatim (re-read at save time). */

#define AWL_CFG_PATH "/data/adb/modules/anland-awl/config.json"

static std::mutex g_cfg_lock;
static int g_cfg_zoom = 100;       /* persisted mirror (real state lives in the logic layer's g_srv.zoom_pct) */
static int g_cfg_init_w = 800;     /* initial-configure placeholder (#33; mirror of g_srv.init_conf_*) */
static int g_cfg_init_h = 600;
static int g_cfg_scale_mode = 0;   /* view mapping mode (#34; mirror of g_srv.scale_mode, AWL_SCALE_*) */
static char g_sock_dir[256] = "/data/local/tmp/awl";   /* runtime_dir (startup-loaded; see above) */

/* known config keys → valid domain (under g_cfg_lock); unknown keys rejected */
static bool cfg_domain(const std::string& key, int* lo, int* hi) {
    if (key == "zoom") { *lo = 50; *hi = 300; return true; }
    if (key == "init_w") { *lo = 100; *hi = 7680; return true; }
    if (key == "init_h") { *lo = 100; *hi = 4320; return true; }
    if (key == "scale_mode") { *lo = 0; *hi = 2; return true; }
    return false;
}

/* single-key file scan: first integer after the "key" colon (no JSON dependency; the file is daemon-written only) */
static int cfg_parse_int(const char* buf, const char* key) {
    char pat[32];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char* p = strstr(buf, pat);
    if (!p) return -1;
    p = strchr(p + strlen(pat) - 1, ':');
    if (!p) return -1;
    return atoi(p + 1);
}

/* same, string value: contents of the first quoted token after the colon
 * (paths contain no escapes/quotes). Returns 0 on success. */
static int cfg_parse_str(const char* buf, const char* key, char* out, size_t n) {
    char pat[32];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char* p = strstr(buf, pat);
    if (!p) return -1;
    p = strchr(p + strlen(pat) - 1, ':');
    if (!p) return -1;
    p++;   /* past the colon */
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return -1;
    p++;
    const char* e = strchr(p, '"');
    if (!e || (size_t)(e - p) >= n) return -1;
    memcpy(out, p, (size_t)(e - p));
    out[e - p] = '\0';
    return 0;
}

static void cfg_save_locked(void) {
    /* runtime_dir is manual-only: carry the FILE's current value over verbatim
     * (a hand edit made while the daemon runs survives this rewrite), falling
     * back to the effective startup value when absent/unreadable */
    char rt[256];
    memcpy(rt, g_sock_dir, sizeof(rt));
    {
        FILE* f = fopen(AWL_CFG_PATH, "r");
        if (f) {
            char buf[512] = "";
            fread(buf, 1, sizeof(buf) - 1, f);
            fclose(f);
            char got[256];
            if (cfg_parse_str(buf, "runtime_dir", got, sizeof(got)) == 0 && got[0] == '/')
                memcpy(rt, got, sizeof(rt));
        }
    }
    char tmp[128];
    snprintf(tmp, sizeof(tmp), "%s.tmp", AWL_CFG_PATH);
    FILE* f = fopen(tmp, "w");
    if (!f) { LOGE("config save open %s: %s", tmp, strerror(errno)); return; }
    fprintf(f, "{\n  \"zoom\": %d,\n  \"init_w\": %d,\n  \"init_h\": %d,\n"
               "  \"scale_mode\": %d,\n  \"runtime_dir\": \"%s\"\n}\n",
            g_cfg_zoom, g_cfg_init_w, g_cfg_init_h, g_cfg_scale_mode, rt);
    if (fclose(f) != 0)
        LOGE("config save flush: %s", strerror(errno));
    if (rename(tmp, AWL_CFG_PATH) != 0)
        LOGE("config rename %s: %s", AWL_CFG_PATH, strerror(errno));
}

/* Startup socket-dir load (main, before mkdir/listen — runs once, no binder):
 * manual-only key "runtime_dir"; absolute path, parent must exist (single
 * mkdir, same as before). Invalid/absent → default. */
static void cfg_load_sock_dir(void) {
    FILE* f = fopen(AWL_CFG_PATH, "r");
    if (!f) return;
    char buf[512] = "";
    fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    char dir[256];
    if (cfg_parse_str(buf, "runtime_dir", dir, sizeof(dir)) != 0)
        return;   /* key absent → default */
    if (dir[0] != '/' || strlen(dir) + 16 >= sizeof(g_sock_dir)) {
        LOGE("config: runtime_dir '%s' invalid (need absolute, short) — using default %s",
             dir, g_sock_dir);
        return;
    }
    memcpy(g_sock_dir, dir, sizeof(g_sock_dir));
    LOGI("config: runtime_dir=%s", g_sock_dir);
}

/* startup load + apply (after awl_server_start; no windows at startup →
 * just set the values; each new window's first configure already uses them) */
static void cfg_load_and_apply(void) {
    FILE* f = fopen(AWL_CFG_PATH, "r");
    if (!f) return;   /* no config file → defaults (zoom 100%, 800x600) */
    char buf[512] = "";
    fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    int z = cfg_parse_int(buf, "zoom");
    if (z >= 50 && z <= 300) {
        {
            std::lock_guard<std::mutex> lk(g_cfg_lock);
            g_cfg_zoom = z;
        }
        awl_display_set_zoom(z);
        LOGI("config: zoom=%d%% (applied at startup)", z);
    } else if (z != -1) {
        LOGE("config: zoom=%d out of range (50..300), ignored", z);
    }
    int iw = cfg_parse_int(buf, "init_w");
    int ih = cfg_parse_int(buf, "init_h");
    int lo, hi;
    if (cfg_domain("init_w", &lo, &hi) && iw >= lo && iw <= hi &&
        cfg_domain("init_h", &lo, &hi) && ih >= lo && ih <= hi) {
        {
            std::lock_guard<std::mutex> lk(g_cfg_lock);
            g_cfg_init_w = iw;
            g_cfg_init_h = ih;
        }
        awl_display_set_init_size(iw, ih);
        LOGI("config: init size %dx%d (applied at startup)", iw, ih);
    } else if (iw != -1 || ih != -1) {
        LOGE("config: init size %dx%d out of range, ignored", iw, ih);
    }
    int sm = cfg_parse_int(buf, "scale_mode");
    if (sm >= AWL_SCALE_STRETCH && sm <= AWL_SCALE_CENTER) {
        awl_display_set_scale_mode(sm);   /* no windows at startup: remap is a no-op */
        {
            std::lock_guard<std::mutex> lk(g_cfg_lock);
            g_cfg_scale_mode = sm;
        }
        LOGI("config: scale_mode=%d (applied at startup)", sm);
    } else if (sm != -1) {
        LOGE("config: scale_mode=%d out of range (0..2), ignored", sm);
    }
}

/* set: apply → persist (apply first, write second; a write failure only warns — the live value stays in effect) */
static int cfg_set(const std::string& key, int32_t val) {
    int lo, hi;
    if (!cfg_domain(key, &lo, &hi) || val < lo || val > hi) {
        LOGE("config set: key='%s' val=%d rejected", key.c_str(), val);
        return -1;
    }
    if (key == "zoom") {
        awl_display_set_zoom(val);
        std::lock_guard<std::mutex> lk(g_cfg_lock);
        g_cfg_zoom = val;
        cfg_save_locked();
        LOGI("config set zoom=%d%% (applied + persisted)", val);
    } else if (key == "init_w" || key == "init_h") {
        /* placeholder pair lives in the logic layer as one value — combine
         * with the other key's current mirror. Applies to NEW windows only
         * (mapped windows are sized by Android SURFACE resizes). */
        std::lock_guard<std::mutex> lk(g_cfg_lock);
        if (key == "init_w") g_cfg_init_w = val; else g_cfg_init_h = val;
        awl_display_set_init_size(g_cfg_init_w, g_cfg_init_h);
        cfg_save_locked();
        LOGI("config set init size %dx%d (applied to new windows + persisted)",
             g_cfg_init_w, g_cfg_init_h);
    } else if (key == "scale_mode") {
        /* zoom shape: apply OUTSIDE g_cfg_lock (the remap chain fires
         * C_CAPTURE callbacks → binder transacts under g_state_lock); lock
         * only wraps the mirror + persist */
        awl_display_set_scale_mode(val);   /* re-pushes confine rects itself */
        {
            std::lock_guard<std::mutex> lk(g_cfg_lock);
            g_cfg_scale_mode = val;
            cfg_save_locked();
        }
        /* every attached window renders one fresh frame under the new dst
         * mapping (unattached ones draw with it on the next attach) */
        std::vector<uint64_t> ids;
        {
            std::lock_guard<std::mutex> lk(g_state_lock);
            for (auto& [id, ws] : g_wins)
                if (ws.attached) ids.push_back(id);
        }
        for (uint64_t id : ids) awl_renderer_request_render(id);
        LOGI("config set scale_mode=%d (applied + persisted)", val);
    }
    return 0;
}

/* ---------------- binder service ---------------- */

static AIBinder_Class* k_binder_class = nullptr;
static AIBinder_DeathRecipient* k_death = nullptr;

/* AParcel_readString allocator (string16 wire → UTF-8 std::string).
 * length includes the trailing NUL; a null string arrives as length=-1 +
 * buffer=nullptr. */
static bool wl_str_alloc(void* data, int32_t length, char** buffer) {
    std::string* str = static_cast<std::string*>(data);
    if (length <= 0 || buffer == nullptr) {
        str->clear();
        return true;
    }
    str->resize((size_t)length - 1);
    *buffer = str->data();
    return true;
}

static void* host_on_create(void* args) { return args; }
static void host_on_destroy(void* userData) {}

/* ---------------- Caller verification (uid allowlist) ----------------
 * The service is reachable by every app on the device (sepolicy grants
 * find/call to all untrusted domains — SELinux cannot split packages within
 * one domain) while the transactions are powerful: input injection, clipboard
 * push, config writes, root `am start`, and a caller-chosen death/ctrl
 * binder in SURFACE. uid is the only reliable identity visible to the NDK:
 * assigned by the binder driver (not spoofable), valid for oneway calls too
 * (getCallingPid returns 0 there; no getCallingSid in libbinder_ndk).
 * Allow: root(0)/self + AWL_PKG's uid. The uid is looked up FRESH on every
 * call from /data/system/packages.list (plain text, "pkg uid flag dataDir
 * …", ~70KB — PackageManager's own dump; packages.xml is ABX binary since
 * Android 16): no cache means a reinstall with a new uid is picked up by
 * the very next call, and the scan is µs-scale (safe on the input hot path).
 * Same appId in ANY user passes (uid % 100000 — the same installed package
 * under another user/work profile). */
static int lookup_app_uid(void) {
    static bool open_failed_logged = false;
    FILE* f = fopen("/data/system/packages.list", "re");
    if (!f) {
        if (!open_failed_logged) {   /* one line: this fires per call otherwise */
            LOGE("binder auth: open packages.list: %s", strerror(errno));
            open_failed_logged = true;
        }
        return 0;
    }
    char line[1024];
    int uid = 0;
    while (fgets(line, sizeof line, f)) {
        char pkg[256];
        int u;
        if (sscanf(line, "%255s %d", pkg, &u) == 2 && strcmp(pkg, AWL_PKG) == 0) {
            uid = u;
            break;
        }
    }
    fclose(f);
    return uid;
}

/* Entry gate for host_on_transact. Denials log one line per uid (an abusive
 * caller must not flood the log). */
static bool caller_ok(void) {
    uid_t u = AIBinder_getCallingUid();
    if (u == 0 || u == (uid_t)getuid()) return true;   /* root / self */

    int app_uid = lookup_app_uid();
    if (app_uid > 10000 &&
        (u == (uid_t)app_uid || u % 100000 == (uid_t)(app_uid % 100000)))
        return true;

    static std::mutex rej_lock;
    static std::vector<uid_t> rej_seen;
    {
        std::lock_guard<std::mutex> lk(rej_lock);
        if (std::find(rej_seen.begin(), rej_seen.end(), u) == rej_seen.end()) {
            if (rej_seen.size() >= 16) rej_seen.clear();   /* bounded, uids repeat anyway */
            rej_seen.push_back(u);
            LOGE("binder auth: uid=%u rejected (not %s), call dropped", u, AWL_PKG);
        }
    }
    return false;
}

static binder_status_t host_on_transact(AIBinder* binder, transaction_code_t code,
                                        const AParcel* in, AParcel* out) {
    if (!caller_ok()) return STATUS_PERMISSION_DENIED;
    switch (code) {
    case AWL_T_SURFACE: {
        int64_t id64; int32_t w, h;
        if (AParcel_readInt64(in, &id64) != STATUS_OK ||
            AParcel_readInt32(in, &w) != STATUS_OK ||
            AParcel_readInt32(in, &h) != STATUS_OK)
            return STATUS_BAD_VALUE;
        uint64_t id = (uint64_t)id64;

        /* window destroyed (or nonexistent) → reject; the client kills itself and exits, leaving no placeholder */
        {
            std::lock_guard<std::mutex> lk(g_state_lock);
            if (g_wins.find(id) == g_wins.end()) {
                LOGE("SURFACE %llu: no such window", (unsigned long long)id);
                AParcel_writeInt32(out, -1);
                return STATUS_OK;
            }
        }

        ANativeWindow* anw = nullptr;
        binder_status_t st = ANativeWindow_readFromParcel(in, &anw);
        if (st != STATUS_OK || !anw) {
            LOGE("SURFACE %llu: readFromParcel failed st=%d", (unsigned long long)id, st);
            return STATUS_BAD_VALUE;
        }
        /* death token (APP process killed abnormally → auto detach); the same binder doubles as control channel endpoint */
        AIBinder* token = nullptr;
        AParcel_readStrongBinder(in, &token);
        /* trailing optional host (Activity instance id; older APKs lack this field → 0 = takes no part in evict decisions) */
        int64_t host = 0;
        (void)AParcel_readInt64(in, &host);

        /* register the death token early (a crash mid-attach still gets a
         * death-notification backstop); stale members are neutralized by
         * on_token_died's ctrl guard */
        if (token) {
            std::lock_guard<std::mutex> lk(g_state_lock);
            death_link* dl = nullptr;
            for (death_link* l : g_links)
                if (l->token == token) { dl = l; break; }
            if (!dl) {
                dl = new death_link();
                dl->token = token;
                AIBinder_incStrong(token);   /* keep alive for the duration of linkToDeath */
                AIBinder_linkToDeath(token, k_death, dl);
                g_links.push_back(dl);
                LOGI("death token linked (%p)", (void*)token);
            }
            bool known = false;
            for (uint64_t x : dl->ids) if (x == id) { known = true; break; }
            if (!known) dl->ids.push_back(id);
        }

        /* re-attach: the renderer internally tears down the old entry with
         * the same id first (rebuilt after pause/evict; concurrent SURFACE
         * can't leak via overwrite either) */
        if (awl_renderer_attach(id, anw) != 0) {
            LOGE("SURFACE %llu: renderer attach failed", (unsigned long long)id);
            ANativeWindow_release(anw);
            AParcel_writeInt32(out, -1);
            return STATUS_OK;
        }
        ANativeWindow_release(anw);          /* renderer holds its own reference */

        /* single atomic decision point (mutually exclusive with cb_window_destroyed / concurrent SURFACE) */
        AIBinder* evicted = nullptr;
        bool gone = false;
        bool was_attached = true;   /* pre-decision value: false→true flip = first holder (fg-sched boost) */
        {
            std::lock_guard<std::mutex> lk(g_state_lock);
            auto it = g_wins.find(id);
            if (it == g_wins.end()) {
                gone = true;   /* window destroyed during attach → rollback */
            } else {
                awl_win_state& ws = it->second;
                was_attached = ws.attached;   /* read before the evict branch clears it */
                /* a different holder already exists → evict (single-attach
                 * invariant): detach the old ctrl, order it to kill itself
                 * over the old channel outside the lock; same host
                 * (re-attached after pause) already has ctrl null */
                if (host && ws.host && ws.host != host && ws.ctrl) {
                    evicted = ws.ctrl;
                    AIBinder_incStrong(evicted);   /* keep alive during the out-of-lock send */
                    AIBinder_decStrong(ws.ctrl);
                    ws.ctrl = nullptr;
                    ws.attached = false;
                }
                if (ws.ctrl && ws.ctrl != token) AIBinder_decStrong(ws.ctrl);
                if (token && ws.ctrl != token) {
                    ws.ctrl = token;
                    AIBinder_incStrong(token);
                }
                ws.attached = true;
                if (host) ws.host = host;
                /* link convergence: the id stays only on this token's chain (stale members pruned from other chains) */
                for (death_link* dl : g_links) {
                    if (token && dl->token == token) continue;
                    for (auto it2 = dl->ids.begin(); it2 != dl->ids.end(); ++it2)
                        if (*it2 == id) { dl->ids.erase(it2); break; }
                }
            }
        }
        if (evicted) {
            LOGI("SURFACE %llu: evicting old holder host=%lld", (unsigned long long)id,
                 (long long)host);
            ctrl_send(evicted, AWL_C_CLOSE);   /* old Activity kills itself (finish) */
            AIBinder_decStrong(evicted);
        }
        if (gone) {
            awl_renderer_attach(id, nullptr);   /* rollback (outside the lock; join holds no lock) */
            LOGE("SURFACE %llu: window destroyed during attach → rollback",
                 (unsigned long long)id);
            AParcel_writeInt32(out, -1);
            return STATUS_OK;
        }
        /* foreground scheduling: first holder of this window → boost the
         * client's process subtree (+ ourselves, idempotent). The flip guard
         * skips repeat SURFACE and the evict branch (no net attach change). */
        if (!was_attached) {
            pid_t cpid = awl_window_client_pid(id);
            if (cpid > 0) {
                awl_sched_set(cpid, 1);
                awl_sched_set(getpid(), 1);
            }
        }
        ime_reopen_on_attach(id);            /* input state kept alive: enabled during detach → reopen */
        capture_reopen_on_attach(id);        /* constraint still active (persistent) → re-capture */
        keep_on_reopen_on_attach(id);        /* idle inhibitor alive → re-set FLAG_KEEP_SCREEN_ON */
        awl_window_resize(id, w, h);         /* Android fully owns sizing (initial + subsequent) */
        xwm_resize_window(id, w, h);         /* Xwayland window: sync initial size to the X side */
        awl_renderer_request_render(id);     /* render a first frame */
        LOGI("SURFACE %llu %dx%d → attached (host=%lld)",
             (unsigned long long)id, w, h, (long long)host);
        AParcel_writeInt32(out, 0);
        return STATUS_OK;
    }
    case AWL_T_CLOSE: {   /* list long-press menu "close": daemon fully owns window close (graceful client exit) */
        int64_t id64;
        if (AParcel_readInt64(in, &id64) != STATUS_OK)
            return STATUS_BAD_VALUE;
        uint64_t id = (uint64_t)id64;
        xwm_close_window(id);   /* Xwayland: WM_DELETE_WINDOW on the X side (XKillClient otherwise) */
        awl_window_close(id);   /* xdg: toplevel.close → client closes the window → C_CLOSE wraps up */
        AParcel_writeInt32(out, 0);
        return STATUS_OK;
    }
    case AWL_T_RESIZE: {
        int64_t id64; int32_t w, h;
        AParcel_readInt64(in, &id64);
        AParcel_readInt32(in, &w);
        AParcel_readInt32(in, &h);
        awl_window_resize((uint64_t)id64, w, h);
        xwm_resize_window((uint64_t)id64, w, h);
        awl_renderer_request_render((uint64_t)id64);
        AParcel_writeInt32(out, 0);
        return STATUS_OK;
    }
    case AWL_T_LIST: {
        std::lock_guard<std::mutex> lk(g_state_lock);
        AParcel_writeInt32(out, (int32_t)g_wins.size());
        for (auto& [id, ws] : g_wins) {
            AParcel_writeInt64(out, (int64_t)id);
            AParcel_writeInt32(out, ws.attached ? 1 : 0);
            AParcel_writeString(out, ws.title, strlen(ws.title));
        }
        return STATUS_OK;
    }
    case AWL_T_BRING: {
        int64_t id64;
        AParcel_readInt64(in, &id64);
        {
            std::lock_guard<std::mutex> lk(g_state_lock);
            if (g_wins.find((uint64_t)id64) == g_wins.end()) {
                AParcel_writeInt32(out, -1);
                return STATUS_OK;
            }
        }
        /* am start = fork+waitpid (hundreds of ms) → detached thread; don't occupy the binder pool */
        std::thread([id = (uint64_t)id64] { attach_activity(id, ""); }).detach();
        AParcel_writeInt32(out, 0);
        return STATUS_OK;
    }
    case AWL_T_PAUSE: {   /* Activity onPause → daemon immediately fully detaches (minimize).
                           * ONEWAY: renderer teardown joins a thread; must not block
                           * the APK main thread's transact */
        int64_t id64;
        AParcel_readInt64(in, &id64);
        int64_t host = 0;
        (void)AParcel_readInt64(in, &host);   /* trailing optional (same as SURFACE) */
        {
            std::lock_guard<std::mutex> lk(g_state_lock);
            auto it = g_wins.find((uint64_t)id64);
            if (it == g_wins.end()) { AParcel_writeInt32(out, 0); return STATUS_OK; }
            /* a late pause from an evicted instance must not kill the current holder */
            if (host && it->second.host && host != it->second.host) {
                LOGI("window %lld stale pause (host=%lld != %lld) ignored",
                     (long long)id64, (long long)host, (long long)it->second.host);
                AParcel_writeInt32(out, 0);
                return STATUS_OK;
            }
        }
        detach_window((uint64_t)id64);
        AParcel_writeInt32(out, 0);
        LOGI("window %lld paused (→ detach, render resources released)", (long long)id64);
        return STATUS_OK;
    }
    case AWL_T_FOCUS: {   /* focus → configure ACTIVATED + keyboard enter/leave */
        int64_t id64; int32_t has;
        AParcel_readInt64(in, &id64);
        AParcel_readInt32(in, &has);
        {
            std::lock_guard<std::mutex> lk(g_state_lock);
            auto it = g_wins.find((uint64_t)id64);
            if (it != g_wins.end()) it->second.kbd_focus = has != 0;
        }
        awl_window_set_activated((uint64_t)id64, has);
        awl_input_ev_t ev;
        memset(&ev, 0, sizeof(ev));
        ev.id = (uint64_t)id64;
        ev.type = has ? AWL_IN_KBD_ENTER : AWL_IN_KBD_LEAVE;
        awl_input_dispatch(&ev);
        AParcel_writeInt32(out, 0);
        LOGI("window %lld focus=%d → ACTIVATED + kbd %s", (long long)id64, has,
             has ? "enter" : "leave");
        return STATUS_OK;
    }
    case AWL_T_INPUT: {   /* input events, per-event literal translation (Activity dispatch callback, ONEWAY) */
        awl_input_ev_t ev;
        memset(&ev, 0, sizeof(ev));
        int64_t id64;
        if (AParcel_readInt64(in, &id64) != STATUS_OK ||
            AParcel_readInt32(in, (int32_t*)&ev.type) != STATUS_OK ||
            AParcel_readInt32(in, (int32_t*)&ev.code) != STATUS_OK ||
            AParcel_readFloat(in, &ev.x) != STATUS_OK ||
            AParcel_readFloat(in, &ev.y) != STATUS_OK ||
            AParcel_readFloat(in, &ev.v1) != STATUS_OK ||
            AParcel_readFloat(in, &ev.v2) != STATUS_OK ||
            AParcel_readInt32(in, (int32_t*)&ev.meta) != STATUS_OK ||
            AParcel_readInt32(in, (int32_t*)&ev.flags) != STATUS_OK)
            return STATUS_BAD_VALUE;
        ev.id = (uint64_t)id64;
        awl_input_dispatch(&ev);
        return STATUS_OK;   /* oneway, no reply */
    }
    case AWL_T_IME: {   /* IME text sent straight through (InputConnection → text-input protocol, ONEWAY) */
        int64_t id64; int32_t op, a, b;
        if (AParcel_readInt64(in, &id64) != STATUS_OK ||
            AParcel_readInt32(in, &op) != STATUS_OK ||
            AParcel_readInt32(in, &a) != STATUS_OK ||
            AParcel_readInt32(in, &b) != STATUS_OK)
            return STATUS_BAD_VALUE;
        std::string text;
        if (AParcel_readString(in, &text, wl_str_alloc) != STATUS_OK)
            return STATUS_BAD_VALUE;
        awl_ime_text((uint64_t)id64, (uint32_t)op, text.c_str(), a, b);
        return STATUS_OK;   /* oneway, no reply */
    }
    case AWL_T_CLIPBOARD: {   /* Android clipboard text → wl selection (ONEWAY, #29) */
        int64_t id64;
        if (AParcel_readInt64(in, &id64) != STATUS_OK)
            return STATUS_BAD_VALUE;
        std::string text;
        if (AParcel_readString(in, &text, wl_str_alloc) != STATUS_OK)
            return STATUS_BAD_VALUE;
        awl_datadev_android_clip(text.c_str());
        return STATUS_OK;
    }
    case AWL_T_CFG_GET: {   /* config value read (daemon is the single source of truth; #31) */
        std::string key;
        if (AParcel_readString(in, &key, wl_str_alloc) != STATUS_OK)
            return STATUS_BAD_VALUE;
        int32_t v = -1;
        if (key == "zoom") v = awl_display_zoom();
        else if (key == "init_w" || key == "init_h") {
            int32_t iw = 0, ih = 0;
            awl_display_init_size(&iw, &ih);
            v = key == "init_w" ? iw : ih;
        }
        else if (key == "scale_mode") v = awl_display_scale_mode();
        else LOGE("config get: unknown key '%s'", key.c_str());
        AParcel_writeInt32(out, v);
        return STATUS_OK;
    }
    case AWL_T_CFG_SET: {   /* config value write: apply + persist config.json (#31) */
        std::string key;
        int32_t val;
        if (AParcel_readString(in, &key, wl_str_alloc) != STATUS_OK ||
            AParcel_readInt32(in, &val) != STATUS_OK)
            return STATUS_BAD_VALUE;
        AParcel_writeInt32(out, cfg_set(key, val));
        return STATUS_OK;
    }
    default:
        return STATUS_UNKNOWN_TRANSACTION;
    }
}

/* ---------------- display info (no Java API, parsed via exec) ---------------- */

static bool query_display(awl_display_info_t* info) {
    memset(info, 0, sizeof(*info));
    info->width = 1280; info->height = 720; info->refresh_hz = 60; info->dpi = 420;
    FILE* p = popen("/system/bin/wm size 2>/dev/null", "r");
    if (p) {
        char buf[256];
        uint32_t ow = 0, oh = 0;
        while (fgets(buf, sizeof(buf), p)) {
            unsigned int w, h;
            if (sscanf(buf, " Override size: %ux%u", &w, &h) == 2) { ow = w; oh = h; }
            else if (!ow && sscanf(buf, " Physical size: %ux%u", &w, &h) == 2) { ow = w; oh = h; }
        }
        pclose(p);
        if (ow) { info->width = ow; info->height = oh; }
    }
    char prop[92] = "";
    __system_property_get("ro.sf.lcd_density", prop);
    if (prop[0]) info->dpi = atoi(prop);
    info->scale = 1;
    LOGI("display %ux%u dpi=%d", info->width, info->height, info->dpi);
    return true;
}

/* ---------------- socket ---------------- */

static int create_listen_socket(const char* path) {
    unlink(path);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    if (strlen(path) >= sizeof(addr.sun_path)) {
        LOGE("socket path too long: %s", path);
        close(fd);
        return -1;
    }
    strcpy(addr.sun_path, path);
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) != 0 ||
        listen(fd, 8) != 0) {
        LOGE("bind/listen %s: %s", path, strerror(errno));
        close(fd);
        return -1;
    }
    chmod(path, 0666);   /* any uid inside the container may connect */
    return fd;
}

/* ---------------- main ---------------- */

int main(int argc, char** argv) {
    /* daemonize (return immediately after launch by su -c / service.sh) */
    bool foreground = argc > 1 && !strcmp(argv[1], "-f");
    if (!foreground) {
        pid_t pid = fork();
        if (pid < 0) { perror("fork"); return 1; }
        if (pid > 0) _exit(0);
        setsid();
        if (chdir("/") != 0) { /* ignore */ }
        int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) { dup2(nul, 0); dup2(nul, 1); dup2(nul, 2); if (nul > 2) close(nul); }
    }
    signal(SIGCHLD, SIG_DFL);

    LOGI("awl-daemon starting (pid=%d uid=%d)", getpid(), getuid());

    cfg_load_sock_dir();   /* runtime_dir before mkdir/bind (manual-only key) */
    mkdir(g_sock_dir, 0777);
    chmod(g_sock_dir, 0777);

    awl_display_info_t info;
    query_display(&info);

    char sock_path[288];
    snprintf(sock_path, sizeof(sock_path), "%s/wayland-0", g_sock_dir);
    int fd = create_listen_socket(sock_path);
    if (fd < 0) { LOGE("listen socket failed"); return 1; }
    LOGI("wayland socket: %s", sock_path);

    if (awl_server_start(fd, &info, &k_cbs) != 0) {
        LOGE("awl_server_start failed");
        return 1;
    }

    cfg_load_and_apply();   /* daemon config: restore zoom etc. at startup (#31) */

    /* binder service */
    if (!binder_plat_init()) return 1;
    k_death = AIBinder_DeathRecipient_new(on_token_died);
    if (!k_death) { LOGE("DeathRecipient alloc failed"); return 1; }
    k_binder_class = AIBinder_Class_define("anland.IHost",
                                           host_on_create, host_on_destroy,
                                           host_on_transact);
    if (!k_binder_class) { LOGE("AIBinder_Class_define failed"); return 1; }
    k_ctrl_class = AIBinder_Class_define(AWL_CTRL_DESC,
                                         ctrl_on_create, ctrl_on_destroy,
                                         ctrl_on_transact);
    if (!k_ctrl_class) { LOGE("AIBinder_Class_define(ctrl) failed"); return 1; }
    AIBinder* svc = AIBinder_new(k_binder_class, nullptr);
    binder_status_t st = g_addService(svc, AWL_BINDER_NAME);
    if (st != STATUS_OK) {
        /* clear diagnostics when root-domain addService is denied by SELinux */
        LOGE("addService(%s) st=%d — check SELinux (can verify in permissive)", AWL_BINDER_NAME, st);
        return 1;
    }
    g_startThreadPool();
    LOGI("binder service %s ready", AWL_BINDER_NAME);

    /* park the main thread (the wayland event thread + binder thread pool do the work) */
    while (awl_server_is_running()) sleep(60);
    LOGI("awl-daemon exit");
    return 0;
}
