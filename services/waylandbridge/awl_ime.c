/* awl_ime.c — zwp_text_input_v1 + v3 (Android IME bridge, wayland side)
 *
 * Pass-through model (same as input events): every event carries its window
 * id; no routing/focus hub. Client activate (v1) / enable+commit (v3) with
 * text-input focus present → cb_ime_show → the corresponding Activity pops
 * the Android soft keyboard; text from the Activity's InputConnection is sent
 * via awl_ime_text straight to that window client's text_input objects (v1/v3
 * dual protocol sent to both; the client receives whichever version it uses).
 *
 * Full IME context: client set_surrounding_text/set_content_type/
 * set_cursor_rectangle → cb_ime_state → cached by the Activity, answers IME
 * queries (getTextBeforeCursor/getCursorCapsMode/…) and drives
 * updateSelection/updateCursorAnchorInfo (candidate window follows the
 * cursor).
 *
 * Protocol semantics (isomorphic to kwin-6.6.5 src/wayland/textinput_v3.cpp,
 * verified against the chromium and GTK clients on 2026-09-10):
 *   v3 — enter/leave follow KEYBOARD focus, independent of enable (GTK only
 *        sends enable after it has received enter; chromium resolves the
 *        cursor-rectangle window from enter). A text_input created while
 *        its client already holds focus gets enter at creation.
 *        enable/disable reset the pending state; set_* are double-buffered
 *        and applied on commit; pending.enabled is NOT reset by commit.
 *        done(serial = number of commit requests) MUST follow EVERY commit,
 *        re-sending the current preedit if non-empty: chromium refuses to
 *        send any further state (cursor rect / surrounding text) until it
 *        has seen done == its commit count (ZwpTextInputV3Impl::
 *        DoneSerialEqualsCommitCount), and a done without preedit_string
 *        resets the preedit on the client (double-buffered event state).
 *        Text events (preedit/commit/delete) are each followed by done.
 *   v1 — activate(seat,surface) is the enter itself; set_surrounding_text /
 *        set_content_type / set_cursor_rectangle take effect immediately
 *        (kwin textinput_v1: surroundingTextChanged emitted at once; chromium
 *        never sends commit_state); commit_state(serial)'s serial is echoed
 *        in events; preedit_cursor precedes preedit_string; cursor_position /
 *        delete_surrounding_text are conveyed by the commit_string that
 *        follows.
 *
 * Locks (g_srv.rwl topology, same as the input object table):
 *   g_ime list = rwl (request threads create/destroy under wr, send/
 *   iterate under rd);
 *   event send = target window s->ev_lock (group atomicity, ordered with
 *   that window's other events under the same lock);
 *   g_ime_lock = only the keyboard focus snapshot (written by the awl_input
 *   focus hook, read at get_text_input / v3 commit) — always held briefly,
 *   never nested with rwl.
 */
#include "awl_internal.h"

#include <string.h>

#include "text-input-unstable-v1-server-protocol.h"
#include "text-input-unstable-v3-server-protocol.h"

#define AWL_IME_TEXT_MAX 4000

/* ---------------- object table ---------------- */

struct awl_ime_obj {
    struct wl_resource* res;      /* zwp_text_input_v1 / v3 */
    struct wl_list link;          /* g_ime (rwl) */
    uint32_t ver;                 /* 1 | 3 */

    /* effective state */
    bool enabled;                 /* v1: activated; v3: enable applied by commit (client state,
                                     survives focus changes — kwin state->enabled) */
    bool entered;                 /* enter sent, no leave yet (v3: follows keyboard focus; v1: activate) */
    struct wl_resource* surface_res;   /* associated surface (v1: activate arg; v3: focus surface at enter) */

    /* v3 double-buffered pending (applied on commit) */
    bool pend_enabled;            /* pending.enabled: set by enable/disable, not reset by commit (kwin) */
    bool pend_enable_req;         /* an enable request arrived since the last commit (re-enable while enabled → re-show the panel) */
    char pend_text[AWL_IME_TEXT_MAX + 1];
    uint32_t pend_cursor, pend_anchor;
    uint32_t pend_hint, pend_purpose;
    int32_t pend_cx, pend_cy, pend_cw, pend_ch;

    /* effective content (source for cb_ime_state push) */
    char text[AWL_IME_TEXT_MAX + 1];
    uint32_t cursor, anchor;
    uint32_t hint, purpose;
    int32_t cx, cy, cw, ch;

    /* v3 current preedit (re-sent with every done, kwin) */
    char pre_text[AWL_IME_TEXT_MAX + 1];
    int32_t pre_cb, pre_ce;

    uint32_t serial;              /* v1: last commit_state serial (echoed in events); v3: commit count (done serial) */
};

static struct wl_list g_ime;
static pthread_mutex_t g_ime_lock = PTHREAD_MUTEX_INITIALIZER;

/* keyboard focus snapshot (written by awl_input.c tr_kbd_enter/leave; read at get_text_input / v3 commit) */
static struct wl_client* g_focus_client;
static uint64_t g_focus_win;

void awl_ime_set_focus(struct wl_client* c, uint64_t win) {
    pthread_mutex_lock(&g_ime_lock);
    g_focus_client = c;
    g_focus_win = win;
    pthread_mutex_unlock(&g_ime_lock);
}

static void focus_snapshot(struct wl_client** c, uint64_t* win) {
    pthread_mutex_lock(&g_ime_lock);
    *c = g_focus_client;
    *win = g_focus_win;
    pthread_mutex_unlock(&g_ime_lock);
}

/* ---------------- state push / show-hide (callbacks) ---------------- */

/* resource destruction (client disconnect / v3 destroy request) → unlink */
static void ime_obj_destroy(struct wl_resource* res) {
    struct awl_ime_obj* o = wl_resource_get_user_data(res);
    if (!o) return;
    pthread_rwlock_wrlock(&g_srv.rwl);
    wl_list_remove(&o->link);
    free(o);
    pthread_rwlock_unlock(&g_srv.rwl);
}

/* caller holds rwl.rd; NULL when not associated with a live surface */
static struct awl_surface* obj_surface(struct awl_ime_obj* o) {
    return o->surface_res ? awl_surface_from_res(o->surface_res) : NULL;
}

/* caller holds rwl.rd; o already associated with a valid surface */
static void push_state_impl(struct awl_ime_obj* o, uint32_t flags) {
    struct awl_surface* s = obj_surface(o);
    if (!s || !g_srv.cbs.ime_state) return;
    /* #31/#34 scaling: cursor rectangle is client logical coords → Activity
     * window view px through the root's view mapping (awl_surface_view_map,
     * view = logical×s + o — 1:1 at Z for content following the configure;
     * chrome dst includes shadow margins → content basis = geometry rect).
     * The geometry origin IS subtracted (same anchor as the render dst and
     * input mapping — the old code skipped it and scaled both axes by the x
     * ratio, which sat off by the margin and mis-scaled y under anisotropic
     * stretch). Caller holds rwl.rd — read the root surface directly. */
    struct awl_surface* root = awl_subsurface_root(s);
    pthread_mutex_lock(&root->ev_lock);
    double sx, sy, ox, oy;
    awl_surface_view_map(root, &sx, &sy, &ox, &oy);
    double gx = root->geom_valid ? (double)root->geom_x : 0.0;
    double gy = root->geom_valid ? (double)root->geom_y : 0.0;
    pthread_mutex_unlock(&root->ev_lock);
    int32_t cx = (int32_t)(((double)o->cx - gx) * sx + ox);
    int32_t cy = (int32_t)(((double)o->cy - gy) * sy + oy);
    int32_t cw = (int32_t)((double)o->cw * sx);
    int32_t ch = (int32_t)((double)o->ch * sy);
    g_srv.cbs.ime_state(g_srv.cbs.user, s->id, o->text,
                        (int32_t)o->cursor, (int32_t)o->anchor,
                        o->hint, o->purpose, cx, cy, cw, ch,
                        flags);
}

static void push_state(struct awl_ime_obj* o) {
    push_state_impl(o, 0);
}

static void push_show(struct awl_ime_obj* o) {
    struct awl_surface* s = obj_surface(o);
    if (s && g_srv.cbs.ime_show)
        g_srv.cbs.ime_show(g_srv.cbs.user, s->id, o->hint, o->purpose);
}

static void push_hide(struct awl_ime_obj* o) {
    struct awl_surface* s = obj_surface(o);
    if (s && g_srv.cbs.ime_hide)
        g_srv.cbs.ime_hide(g_srv.cbs.user, s->id);
}

/* ---------------- zwp_text_input_v3 ---------------- */

static void ti3_reset_pending(struct awl_ime_obj* o) {
    o->pend_text[0] = 0;
    o->pend_cursor = o->pend_anchor = 0;
    o->pend_hint = o->pend_purpose = 0;
    o->pend_cx = o->pend_cy = o->pend_cw = o->pend_ch = 0;
}

/* done group tail (caller holds rwl.rd + the target surface ev_lock when
 * entered): current preedit (if any) + done(commit count). kwin
 * zwp_text_input_v3_commit: "Gtk text input implementation expect done to be
 * sent after every commit"; the preedit is re-sent because event state is
 * double-buffered on the client — a done without preedit_string resets it. */
static void ti3_send_done(struct awl_ime_obj* o) {
    if (o->pre_text[0] || o->pre_cb || o->pre_ce)
        zwp_text_input_v3_send_preedit_string(o->res, o->pre_text, o->pre_cb, o->pre_ce);
    zwp_text_input_v3_send_done(o->res, o->serial);
}

/* enter (caller holds rwl.rd + s->ev_lock). No done: kwin sendEnter sends only enter. */
static void ti3_enter(struct awl_ime_obj* o, struct awl_surface* s) {
    zwp_text_input_v3_send_enter(o->res, s->resource);
    o->entered = true;
    o->surface_res = s->resource;
}

static void ti3_destroy(struct wl_client* c, struct wl_resource* res) {
    wl_resource_destroy(res);
}

static void ti3_enable(struct wl_client* c, struct wl_resource* res) {
    struct awl_ime_obj* o = wl_resource_get_user_data(res);
    if (!o) return;
    ti3_reset_pending(o);   /* kwin: enable resets the pending state to defaults */
    o->pend_enabled = true;
    o->pend_enable_req = true;
}

static void ti3_disable(struct wl_client* c, struct wl_resource* res) {
    struct awl_ime_obj* o = wl_resource_get_user_data(res);
    if (!o) return;
    ti3_reset_pending(o);
    o->pend_enabled = false;
}

/* set_* are no-ops unless an enable is pending (kwin) */
static void ti3_set_surrounding_text(struct wl_client* c, struct wl_resource* res,
                                     const char* text, int32_t cursor, int32_t anchor) {
    struct awl_ime_obj* o = wl_resource_get_user_data(res);
    if (!o || !o->pend_enabled) return;
    snprintf(o->pend_text, sizeof(o->pend_text), "%s", text ? text : "");
    o->pend_cursor = cursor > 0 ? (uint32_t)cursor : 0;
    o->pend_anchor = anchor > 0 ? (uint32_t)anchor : o->pend_cursor;
}

static void ti3_set_text_change_cause(struct wl_client* c, struct wl_resource* res,
                                      uint32_t cause) {
}

static void ti3_set_content_type(struct wl_client* c, struct wl_resource* res,
                                 uint32_t hint, uint32_t purpose) {
    struct awl_ime_obj* o = wl_resource_get_user_data(res);
    if (!o || !o->pend_enabled) return;
    o->pend_hint = hint;
    o->pend_purpose = purpose;
}

static void ti3_set_cursor_rectangle(struct wl_client* c, struct wl_resource* res,
                                     int32_t x, int32_t y, int32_t w, int32_t h) {
    struct awl_ime_obj* o = wl_resource_get_user_data(res);
    if (!o || !o->pend_enabled) return;
    o->pend_cx = x; o->pend_cy = y; o->pend_cw = w; o->pend_ch = h;
}

static void ti3_commit(struct wl_client* c, struct wl_resource* res) {
    struct awl_ime_obj* o = wl_resource_get_user_data(res);
    if (!o) return;
    o->serial++;   /* commit count = done event serial */

    bool was = o->enabled;
    bool enable_req = o->pend_enable_req;
    o->pend_enable_req = false;
    o->enabled = o->pend_enabled;   /* pending.enabled persists across commits (kwin) */

    /* apply the double-buffered content (only meaningful while enabled;
     * copying unconditionally is harmless — pushes happen only when enabled) */
    memcpy(o->text, o->pend_text, sizeof(o->text));
    o->cursor = o->pend_cursor; o->anchor = o->pend_anchor;
    o->hint = o->pend_hint; o->purpose = o->pend_purpose;
    o->cx = o->pend_cx; o->cy = o->pend_cy; o->cw = o->pend_cw; o->ch = o->pend_ch;

    pthread_rwlock_rdlock(&g_srv.rwl);
    struct awl_surface* s = o->entered ? obj_surface(o) : NULL;
    if (s) pthread_mutex_lock(&s->ev_lock);
    if (!o->enabled) {   /* disable drops the composition */
        o->pre_text[0] = 0;
        o->pre_cb = o->pre_ce = 0;
    }
    ti3_send_done(o);   /* done after EVERY commit (serial handshake, see file header) */
    wl_client_flush(c);
    if (s) pthread_mutex_unlock(&s->ev_lock);

    if (s) {   /* text-input focus present → drive the Android IME */
        if (o->enabled && (!was || enable_req)) {
            push_state(o);
            push_show(o);
        } else if (!o->enabled && was) {
            push_hide(o);
        } else if (o->enabled) {
            push_state(o);
        }
    }
    pthread_rwlock_unlock(&g_srv.rwl);
}

static const struct zwp_text_input_v3_interface ti3_iface = {
    .destroy = ti3_destroy,
    .enable = ti3_enable,
    .disable = ti3_disable,
    .set_surrounding_text = ti3_set_surrounding_text,
    .set_text_change_cause = ti3_set_text_change_cause,
    .set_content_type = ti3_set_content_type,
    .set_cursor_rectangle = ti3_set_cursor_rectangle,
    .commit = ti3_commit,
};

static void mgr3_get_text_input(struct wl_client* c, struct wl_resource* res,
                                uint32_t id, struct wl_resource* seat) {
    struct wl_resource* t = wl_resource_create(
            c, &zwp_text_input_v3_interface, 1, id);
    if (!t) { wl_resource_post_no_memory(res); return; }
    struct awl_ime_obj* o = calloc(1, sizeof(*o));
    if (!o) { wl_resource_destroy(t); wl_resource_post_no_memory(res); return; }
    o->res = t;
    o->ver = 3;
    pthread_rwlock_wrlock(&g_srv.rwl);
    wl_list_insert(g_ime.prev, &o->link);
    pthread_rwlock_unlock(&g_srv.rwl);
    wl_resource_set_implementation(t, &ti3_iface, o, ime_obj_destroy);

    /* created while this client already holds keyboard focus (chromium
     * creates the object lazily per window) → it learns the focus now */
    struct wl_client* fc;
    uint64_t fw;
    focus_snapshot(&fc, &fw);
    if (fc != c) return;
    pthread_rwlock_rdlock(&g_srv.rwl);
    struct awl_surface* s = awl_surface_by_id(fw);
    if (s && s->resource && wl_resource_get_client(s->resource) == c) {
        pthread_mutex_lock(&s->ev_lock);
        ti3_enter(o, s);
        wl_client_flush(c);
        pthread_mutex_unlock(&s->ev_lock);
    }
    pthread_rwlock_unlock(&g_srv.rwl);
}

static const struct zwp_text_input_manager_v3_interface mgr3_iface = {
    .destroy = ti3_destroy,
    .get_text_input = mgr3_get_text_input,
};

static void mgr3_bind(struct wl_client* client, void* data,
                      uint32_t version, uint32_t id) {
    struct wl_resource* res = wl_resource_create(
            client, &zwp_text_input_manager_v3_interface, 1, id);
    if (!res) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(res, &mgr3_iface, NULL, NULL);
}

/* ---------------- zwp_text_input_v1 ---------------- */

static void ti1_activate(struct wl_client* c, struct wl_resource* res,
                         struct wl_resource* seat, struct wl_resource* surface) {
    struct awl_ime_obj* o = wl_resource_get_user_data(res);
    if (!o) return;
    pthread_rwlock_rdlock(&g_srv.rwl);
    struct awl_surface* s = surface ? awl_surface_from_res(surface) : NULL;
    o->enabled = true;
    if (s && s->resource) {
        if (o->entered && o->surface_res != surface) {   /* re-activated on another surface: leave the old one first */
            struct awl_surface* old = obj_surface(o);
            if (old) {
                pthread_mutex_lock(&old->ev_lock);
                zwp_text_input_v1_send_leave(o->res);
                wl_client_flush(c);
                pthread_mutex_unlock(&old->ev_lock);
            }
            o->entered = false;
        }
        o->surface_res = surface;
        if (!o->entered) {
            pthread_mutex_lock(&s->ev_lock);
            zwp_text_input_v1_send_enter(o->res, s->resource);
            zwp_text_input_v1_send_input_panel_state(
                    o->res, 1 /* VISIBLE */);
            wl_client_flush(c);
            pthread_mutex_unlock(&s->ev_lock);
            o->entered = true;
        }
    }
    push_show(o);
    pthread_rwlock_unlock(&g_srv.rwl);
}

static void ti1_deactivate(struct wl_client* c, struct wl_resource* res,
                           struct wl_resource* seat) {
    struct awl_ime_obj* o = wl_resource_get_user_data(res);
    if (!o) return;
    pthread_rwlock_rdlock(&g_srv.rwl);
    o->enabled = false;
    if (o->entered) {
        struct awl_surface* s = obj_surface(o);
        if (s) {
            pthread_mutex_lock(&s->ev_lock);
            zwp_text_input_v1_send_leave(o->res);
            zwp_text_input_v1_send_input_panel_state(
                    o->res, 0 /* HIDDEN */);
            wl_client_flush(c);
            pthread_mutex_unlock(&s->ev_lock);
        }
        o->entered = false;
    }
    push_hide(o);
    o->surface_res = NULL;
    pthread_rwlock_unlock(&g_srv.rwl);
}

static void ti1_show_input_panel(struct wl_client* c, struct wl_resource* res) {
    struct awl_ime_obj* o = wl_resource_get_user_data(res);
    if (!o) return;
    pthread_rwlock_rdlock(&g_srv.rwl);
    push_show(o);
    pthread_rwlock_unlock(&g_srv.rwl);
}

static void ti1_hide_input_panel(struct wl_client* c, struct wl_resource* res) {
    struct awl_ime_obj* o = wl_resource_get_user_data(res);
    if (!o) return;
    pthread_rwlock_rdlock(&g_srv.rwl);
    push_hide(o);
    pthread_rwlock_unlock(&g_srv.rwl);
}

static void ti1_reset(struct wl_client* c, struct wl_resource* res) {
    /* editor state changed externally (client asks IME to reset composition
     * state) — re-push current state + RESET flag → Activity clears
     * composition state + restartInput */
    struct awl_ime_obj* o = wl_resource_get_user_data(res);
    if (!o) return;
    pthread_rwlock_rdlock(&g_srv.rwl);
    push_state_impl(o, AWL_IME_STATE_RESET);
    pthread_rwlock_unlock(&g_srv.rwl);
}

/* v1 state requests take effect immediately (no commit_state needed — chromium never sends one) */
static void ti1_push_now(struct awl_ime_obj* o) {
    if (!o->enabled) return;
    pthread_rwlock_rdlock(&g_srv.rwl);
    push_state(o);
    pthread_rwlock_unlock(&g_srv.rwl);
}

static void ti1_set_surrounding_text(struct wl_client* c, struct wl_resource* res,
                                     const char* text, uint32_t cursor, uint32_t anchor) {
    struct awl_ime_obj* o = wl_resource_get_user_data(res);
    if (!o) return;
    snprintf(o->text, sizeof(o->text), "%s", text ? text : "");
    o->cursor = cursor;
    o->anchor = anchor;
    ti1_push_now(o);
}

static void ti1_set_content_type(struct wl_client* c, struct wl_resource* res,
                                 uint32_t hint, uint32_t purpose) {
    struct awl_ime_obj* o = wl_resource_get_user_data(res);
    if (!o) return;
    o->hint = hint;
    o->purpose = purpose;
    ti1_push_now(o);
}

static void ti1_set_cursor_rectangle(struct wl_client* c, struct wl_resource* res,
                                     int32_t x, int32_t y, int32_t w, int32_t h) {
    struct awl_ime_obj* o = wl_resource_get_user_data(res);
    if (!o) return;
    o->cx = x; o->cy = y; o->cw = w; o->ch = h;
    ti1_push_now(o);
}

static void ti1_set_preferred_language(struct wl_client* c, struct wl_resource* res,
                                       const char* language) {
}

static void ti1_commit_state(struct wl_client* c, struct wl_resource* res,
                             uint32_t serial) {
    struct awl_ime_obj* o = wl_resource_get_user_data(res);
    if (!o) return;
    o->serial = serial;
    pthread_rwlock_rdlock(&g_srv.rwl);
    push_state(o);
    pthread_rwlock_unlock(&g_srv.rwl);
}

static void ti1_invoke_action(struct wl_client* c, struct wl_resource* res,
                              uint32_t button, uint32_t index) {
}

static const struct zwp_text_input_v1_interface ti1_iface = {
    .activate = ti1_activate,
    .deactivate = ti1_deactivate,
    .show_input_panel = ti1_show_input_panel,
    .hide_input_panel = ti1_hide_input_panel,
    .reset = ti1_reset,
    .set_surrounding_text = ti1_set_surrounding_text,
    .set_content_type = ti1_set_content_type,
    .set_cursor_rectangle = ti1_set_cursor_rectangle,
    .set_preferred_language = ti1_set_preferred_language,
    .commit_state = ti1_commit_state,
    .invoke_action = ti1_invoke_action,
};

/* modifiers_map: standard name order (matches wl_keyboard modifiers group bit order) */
static const char k_mod_names[] =
        "Shift\0Lock\0Control\0Mod1\0Mod2\0Mod3\0Mod4\0Mod5\0";

static void mgr1_create_text_input(struct wl_client* c, struct wl_resource* res,
                                   uint32_t id) {
    struct wl_resource* t = wl_resource_create(
            c, &zwp_text_input_v1_interface, 1, id);
    if (!t) { wl_resource_post_no_memory(res); return; }
    struct awl_ime_obj* o = calloc(1, sizeof(*o));
    if (!o) { wl_resource_destroy(t); wl_resource_post_no_memory(res); return; }
    o->res = t;
    o->ver = 1;
    pthread_rwlock_wrlock(&g_srv.rwl);
    wl_list_insert(g_ime.prev, &o->link);
    pthread_rwlock_unlock(&g_srv.rwl);
    wl_resource_set_implementation(t, &ti1_iface, o, ime_obj_destroy);
    /* modifiers_map (for keysym events — this bridge sends no keysym, notify by convention) */
    struct wl_array map;
    map.data = (void*)k_mod_names;
    map.size = sizeof(k_mod_names);
    map.alloc = 0;
    zwp_text_input_v1_send_modifiers_map(t, &map);
    wl_client_flush(c);
}

static const struct zwp_text_input_manager_v1_interface mgr1_iface = {
    .create_text_input = mgr1_create_text_input,
};

static void mgr1_bind(struct wl_client* client, void* data,
                      uint32_t version, uint32_t id) {
    struct wl_resource* res = wl_resource_create(
            client, &zwp_text_input_manager_v1_interface, 1, id);
    if (!res) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(res, &mgr1_iface, NULL, NULL);
}

/* ---------------- focus hooks (called from awl_input.c, holds rwl.rd) ---------------- */

/* Keyboard focus gained by window win: v3 → enter on every text_input of
 * that client (text-input focus = keyboard focus, kwin sendEnter), and an
 * already-enabled one re-opens the Android IME (re-attach / window switch);
 * v1 → an activate that preceded keyboard focus gets its enter now. */
void awl_ime_focus_enter(uint64_t win) {
    struct awl_surface* s = awl_surface_by_id(win);
    if (!s || !s->resource) return;
    struct wl_client* c = wl_resource_get_client(s->resource);
    struct awl_ime_obj* o;
    wl_list_for_each(o, &g_ime, link) {
        if (wl_resource_get_client(o->res) != c || o->entered) continue;
        if (o->ver == 3) {
            pthread_mutex_lock(&s->ev_lock);
            ti3_enter(o, s);
            wl_client_flush(c);
            pthread_mutex_unlock(&s->ev_lock);
            if (o->enabled) {
                push_state(o);
                push_show(o);
            }
        } else {
            /* activate arrived before keyboard focus → send enter now */
            if (!o->enabled || !o->surface_res) continue;
            struct awl_surface* ts = obj_surface(o);
            if (!ts) continue;
            pthread_mutex_lock(&ts->ev_lock);
            zwp_text_input_v1_send_enter(o->res, ts->resource);
            zwp_text_input_v1_send_input_panel_state(o->res, 1);
            wl_client_flush(c);
            pthread_mutex_unlock(&ts->ev_lock);
            o->entered = true;
            push_show(o);
        }
    }
}

/* Keyboard focus lost: leave on every entered text_input of that client;
 * an enabled one closes the Android IME (re-opened by the next focus_enter).
 * The client's enabled state is kept (kwin: effective = enabled && focused). */
void awl_ime_focus_leave(uint64_t win) {
    struct awl_surface* s = awl_surface_by_id(win);
    if (!s || !s->resource) return;
    struct wl_client* c = wl_resource_get_client(s->resource);
    struct awl_ime_obj* o;
    wl_list_for_each(o, &g_ime, link) {
        if (wl_resource_get_client(o->res) != c || !o->entered) continue;
        struct awl_surface* ts = obj_surface(o);
        if (!ts) ts = s;
        pthread_mutex_lock(&ts->ev_lock);
        if (o->ver == 3)
            zwp_text_input_v3_send_leave(o->res, o->surface_res ? o->surface_res : s->resource);
        else
            zwp_text_input_v1_send_leave(o->res);
        wl_client_flush(c);
        pthread_mutex_unlock(&ts->ev_lock);
        o->entered = false;
        if (o->enabled) push_hide(o);
        if (o->ver == 3) o->surface_res = NULL;
    }
}

/* Surface death (caller holds rwl.wr, from awl_surface.c surface_destroy_impl):
 * drop dangling associations — no leave is sent for a surface that no longer exists */
void awl_ime_surface_gone(struct awl_surface* s) {
    struct awl_ime_obj* o;
    wl_list_for_each(o, &g_ime, link) {
        if (o->surface_res == s->resource) {
            o->entered = false;
            o->surface_res = NULL;
        }
    }
}

/* ---------------- IME text direct send (binder thread, any time) ---------------- */

void awl_ime_text(uint64_t id, uint32_t op, const char* text, int32_t a, int32_t b) {
    if (!text) text = "";
    pthread_rwlock_rdlock(&g_srv.rwl);
    struct awl_surface* s = awl_surface_by_id(id);
    if (!s || !s->resource) {
        pthread_rwlock_unlock(&g_srv.rwl);
        return;
    }
    struct wl_client* c = wl_resource_get_client(s->resource);
    struct awl_ime_obj* o;
    wl_list_for_each(o, &g_ime, link) {
        if (wl_resource_get_client(o->res) != c) continue;
        if (!o->enabled || !o->entered) continue;
        struct awl_surface* ts = obj_surface(o);
        if (!ts) ts = s;
        pthread_mutex_lock(&ts->ev_lock);
        if (o->ver == 3) {
            switch (op) {
            case AWL_IME_COMMIT:
                /* commit replaces the composition: no preedit in this done group */
                o->pre_text[0] = 0;
                o->pre_cb = o->pre_ce = 0;
                zwp_text_input_v3_send_commit_string(o->res, text);
                zwp_text_input_v3_send_done(o->res, o->serial);
                break;
            case AWL_IME_PREEDIT:
                snprintf(o->pre_text, sizeof(o->pre_text), "%s", text);
                o->pre_cb = a;
                o->pre_ce = b;
                zwp_text_input_v3_send_preedit_string(o->res, o->pre_text, a, b);
                zwp_text_input_v3_send_done(o->res, o->serial);
                break;
            case AWL_IME_DELETE:
                zwp_text_input_v3_send_delete_surrounding_text(
                        o->res, (uint32_t)(a > 0 ? a : 0),
                        (uint32_t)(b > 0 ? b : 0));
                ti3_send_done(o);   /* keeps the current preedit (deleted around it) */
                break;
            case AWL_IME_CURSOR:
                break;   /* v3 has no cursor positioning event */
            }
        } else {
            switch (op) {
            case AWL_IME_COMMIT:
                zwp_text_input_v1_send_commit_string(o->res, o->serial, text);
                break;
            case AWL_IME_PREEDIT:
                zwp_text_input_v1_send_preedit_cursor(o->res, a);
                zwp_text_input_v1_send_preedit_string(o->res, o->serial,
                                                      text, "");
                break;
            case AWL_IME_DELETE:
                zwp_text_input_v1_send_delete_surrounding_text(
                        o->res, -(a > 0 ? a : 0),
                        (uint32_t)((a > 0 ? a : 0) + (b > 0 ? b : 0)));
                zwp_text_input_v1_send_commit_string(o->res, o->serial, "");
                break;
            case AWL_IME_CURSOR:
                zwp_text_input_v1_send_cursor_position(o->res, a, b);
                zwp_text_input_v1_send_commit_string(o->res, o->serial, "");
                break;
            }
        }
        wl_client_flush(c);
        pthread_mutex_unlock(&ts->ev_lock);
    }
    pthread_rwlock_unlock(&g_srv.rwl);
}

/* ---------------- setup ---------------- */

void awl_ime_setup(void) {
    wl_list_init(&g_ime);
    g_focus_client = NULL;
    g_focus_win = 0;
    if (!wl_global_create(g_srv.display, &zwp_text_input_manager_v1_interface,
                          1, NULL, mgr1_bind))
        LOGE("zwp_text_input_manager_v1 global create failed");
    if (!wl_global_create(g_srv.display, &zwp_text_input_manager_v3_interface,
                          1, NULL, mgr3_bind))
        LOGE("zwp_text_input_manager_v3 global create failed");
}
