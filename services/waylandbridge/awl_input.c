/* awl_input.c — wl_seat input implementation (per-event literal translation model)
 *
 * Routing authority = Android: whichever Activity an event goes to is that
 * window's input; the event carries its window id — this file keeps zero
 * routing state, only translating Android events → wayland protocol:
 *   keyboard enter/leave ← onWindowFocusChanged (FOCUS transact)
 *   pointer enter/leave  ← ACTION_HOVER_ENTER/EXIT (Java side converts to PTR_ENTER/LEAVE)
 *   all other events     ← literal translation, delivered to the window whose id the event carries
 * Ordering guarantee: the binder driver delivers oneway transactions to the
 * same node strictly in serial (while one async is pending the next is not
 * delivered) → dispatch has a natural total order, no ordering lock needed.
 *
 * Thread safety relies on the wayland-src awl patches: wl_connection
 * recursive mutex + atomicized wl_display_next_serial — send+flush from any
 * thread. Locks: rwl(rd) to resolve the window → s->ev_lock to send (mutual
 * exclusion with commit/configure/presented under the same lock).
 * g_input_lock only protects the key bitmap/modifier bits (enter array
 * consistency).
 *
 * keymap: embedded evdev+qwerty xkb (third_party/keymap_evdev.h, memfd transfer).
 *
 * Cursor (wl_pointer.set_cursor, section at the end of this file): the
 * client's cursor image is composited by the renderer as the topmost layer
 * of the window the pointer is in (awl_pointer_cursor_layer) while the
 * adaptation layer hides the Android system pointer (cbs.pointer_cursor).
 * Android is the position authority: every enter/motion stores the pointer
 * position in one atomic word, the render thread reads it lock-free.
 *
 * Pointer constraints (zwp_pointer_constraints_v1, section at the end of
 * this file): pure state sync — the forwarding path above is untouched, no
 * constraint branch anywhere. Request → C_CAPTURE (mode + confine rect) →
 * the Activity requestPointerCapture()s, reports PTR_REL
 * (AXIS_RELATIVE_X/Y) and synthesizes any clamped absolute motion itself;
 * object/surface destruction → C_CAPTURE none → release. The daemon never
 * filters or synthesizes pointer events: the APK is the authority.
 */
#define _GNU_SOURCE   /* bionic: memfd_create */
#include "awl_internal.h"

#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>          /* memfd: keymap */

#include "keymap_evdev.h"
#include "relative-pointer-unstable-v1-server-protocol.h"
#include "pointer-constraints-unstable-v1-server-protocol.h"

/* ---------------- Input proxy table (topology: rwl protected) ---------------- */

struct awl_in_obj {           /* one per client (pointer/kbd/touch isomorphic) */
    struct wl_resource* res;
    struct wl_list link;
};

static struct wl_list g_ptrs, g_kbds, g_tchs;

/* Keyboard derived state (single-threaded access under binder serial
 * delivery; g_input_lock only defensively protects the bitmap and modifier
 * bits, used to restore the keys/modifiers arrays on enter) */
static uint8_t  g_keys_down[256 / 8];   /* bitmap of pressed evdev codes (keymap range 8..255) */
static uint32_t g_mods_depressed, g_mods_locked;   /* last known xkb mask */
static pthread_mutex_t g_input_lock = PTHREAD_MUTEX_INITIALIZER;

/* Input proxy of this window's client (caller holds rwl.rd) */
static struct wl_resource* res_for(struct wl_list* list, uint64_t win_id) {
    struct awl_surface* s = awl_surface_by_id(win_id);
    if (!s || !s->resource) return NULL;
    struct wl_client* c = wl_resource_get_client(s->resource);
    struct awl_in_obj* it;
    wl_list_for_each(it, list, link) {
        if (wl_resource_get_client(it->res) == c) return it->res;
    }
    return NULL;
}

/* Window view (Android physical pixels) → root logical coordinates
 * (#31 zoom, #34 scale_mode): the inverse of the root's view mapping
 * (awl_surface_view_map — view = (logical − geom origin) × s + o; s = Z,
 * o = 0 for content following the configure, scale_mode placement
 * otherwise), logical = geom origin + (view − o)/s. Degenerate size →
 * identity map (inside the mapping). The geometry origin is the same anchor
 * the render dst uses (chrome-like clients' viewport dst carries shadow
 * margins around the geometry rectangle). Caller holds s->ev_lock. */
static void view_to_surface(struct awl_surface* s, float* x, float* y) {
    double sx, sy, ox, oy;
    awl_surface_view_map(s, &sx, &sy, &ox, &oy);
    *x = (float)(((double)*x - ox) / sx);
    *y = (float)(((double)*y - oy) / sy);
    if (s->geom_valid) {
        *x += (float)s->geom_x;
        *y += (float)s->geom_y;
    }
}

/* ---------------- wl_pointer / wl_keyboard / wl_touch client-side interface ---- */

/* wl_pointer.set_cursor — see the cursor section at the end of this file */
static void pointer_set_cursor(struct wl_client* c, struct wl_resource* res,
                               uint32_t serial, struct wl_resource* surface,
                               int32_t hot_x, int32_t hot_y);
static void input_obj_release(struct wl_client* c, struct wl_resource* res) {
    wl_resource_destroy(res);
}
static const struct wl_pointer_interface pointer_iface = {
    .set_cursor = pointer_set_cursor,
    .release = input_obj_release,
};
static const struct wl_keyboard_interface keyboard_iface = {
    .release = input_obj_release,
};
static const struct wl_touch_interface touch_iface = {
    .release = input_obj_release,
};

static void in_obj_destroy(struct wl_list* list, struct wl_resource* res) {
    pthread_rwlock_wrlock(&g_srv.rwl);
    struct awl_in_obj* it;
    wl_list_for_each(it, list, link)
        if (it->res == res) { wl_list_remove(&it->link); free(it); break; }
    pthread_rwlock_unlock(&g_srv.rwl);
}
static void ptr_obj_destroy(struct wl_resource* res) {
    in_obj_destroy(&g_ptrs, res);
}
static void kbd_obj_destroy(struct wl_resource* res) {
    in_obj_destroy(&g_kbds, res);
}
static void tch_obj_destroy(struct wl_resource* res) {
    in_obj_destroy(&g_tchs, res);
}

/* keymap: write the embedded xkb text into a memfd, once per client (on keyboard proxy creation) */
static void send_keymap(struct wl_resource* kbd) {
    size_t len = strlen(k_keymap_evdev);
    int fd = memfd_create("awl-keymap", MFD_CLOEXEC);
    if (fd < 0) {
        LOGE("memfd_create: %s", strerror(errno));
        return;
    }
    if (write(fd, k_keymap_evdev, len) != (ssize_t)len) {
        close(fd);
        return;
    }
    lseek(fd, 0, SEEK_SET);
    wl_keyboard_send_keymap(kbd, 1 /* WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 */,
                            fd, (uint32_t)len);
    close(fd);   /* protocol stack already dup'ed */
    wl_client_flush(wl_resource_get_client(kbd));
}

/* ---------------- wl_seat ---------------- */

static void seat_get_pointer(struct wl_client* c, struct wl_resource* res,
                             uint32_t id) {
    struct wl_resource* p = wl_resource_create(
            c, &wl_pointer_interface, wl_resource_get_version(res), id);
    if (!p) { wl_resource_post_no_memory(res); return; }
    struct awl_in_obj* o = calloc(1, sizeof(*o));
    if (!o) { wl_resource_destroy(p); wl_resource_post_no_memory(res); return; }
    o->res = p;
    pthread_rwlock_wrlock(&g_srv.rwl);
    wl_list_insert(g_ptrs.prev, &o->link);
    pthread_rwlock_unlock(&g_srv.rwl);
    wl_resource_set_implementation(p, &pointer_iface, NULL, ptr_obj_destroy);
}
static void seat_get_keyboard(struct wl_client* c, struct wl_resource* res,
                              uint32_t id) {
    struct wl_resource* k = wl_resource_create(
            c, &wl_keyboard_interface, wl_resource_get_version(res), id);
    if (!k) { wl_resource_post_no_memory(res); return; }
    struct awl_in_obj* o = calloc(1, sizeof(*o));
    if (!o) { wl_resource_destroy(k); wl_resource_post_no_memory(res); return; }
    o->res = k;
    pthread_rwlock_wrlock(&g_srv.rwl);
    wl_list_insert(g_kbds.prev, &o->link);
    pthread_rwlock_unlock(&g_srv.rwl);
    wl_resource_set_implementation(k, &keyboard_iface, NULL, kbd_obj_destroy);
    send_keymap(k);
    /* enter is not made up here: in the normal order bind precedes the
     * window gaining focus (connect → bind seat → map → FOCUS → KBD_ENTER) */
}
static void seat_get_touch(struct wl_client* c, struct wl_resource* res,
                           uint32_t id) {
    struct wl_resource* t = wl_resource_create(
            c, &wl_touch_interface, wl_resource_get_version(res), id);
    if (!t) { wl_resource_post_no_memory(res); return; }
    struct awl_in_obj* o = calloc(1, sizeof(*o));
    if (!o) { wl_resource_destroy(t); wl_resource_post_no_memory(res); return; }
    o->res = t;
    pthread_rwlock_wrlock(&g_srv.rwl);
    wl_list_insert(g_tchs.prev, &o->link);
    pthread_rwlock_unlock(&g_srv.rwl);
    wl_resource_set_implementation(t, &touch_iface, NULL, tch_obj_destroy);
}
static void seat_release(struct wl_client* c, struct wl_resource* res) {
    wl_resource_destroy(res);
}
static const struct wl_seat_interface seat_iface = {
    .get_pointer = seat_get_pointer,
    .get_keyboard = seat_get_keyboard,
    .get_touch = seat_get_touch,
    .release = seat_release,
};

static void seat_bind(struct wl_client* client, void* data,
                      uint32_t version, uint32_t id) {
    struct wl_resource* res = wl_resource_create(
            client, &wl_seat_interface, version < 5 ? version : 5, id);
    if (!res) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(res, &seat_iface, NULL, NULL);
    if (wl_resource_get_version(res) >= WL_SEAT_NAME_SINCE_VERSION)
        wl_seat_send_name(res, "awl-seat-0");
    wl_seat_send_capabilities(res, WL_SEAT_CAPABILITY_KEYBOARD
                                   | WL_SEAT_CAPABILITY_POINTER
                                   | WL_SEAT_CAPABILITY_TOUCH);
    wl_client_flush(client);
}

/* ---------------- Relative motion reporting (zwp_relative_pointer_v1) ----------------
 * Pure event forwarding: PTR_REL → relative_motion (the Xwayland warp
 * emulator converts this into X relative MotionNotify, game mouse-look).
 * dx is computed on the APK side: absolute-position diff normally, or
 * AXIS_RELATIVE_X/Y while a pointer-constraint capture is active (see the
 * constraints section at the end of this file); this side keeps zero state
 * and broadcasts per client. The list is built/destroyed on the client
 * dispatch thread and read on the binder input thread — guarded by its own
 * g_rel_lock. */
static struct wl_list g_relptrs;
static pthread_mutex_t g_rel_lock = PTHREAD_MUTEX_INITIALIZER;

struct awl_relptr {
    struct wl_resource* res;   /* zwp_relative_pointer_v1 */
    struct wl_list link;
};

static void relptr_res_destroy(struct wl_resource* res) {
    pthread_mutex_lock(&g_rel_lock);
    struct awl_relptr* it;
    wl_list_for_each(it, &g_relptrs, link) {
        if (it->res == res) {
            wl_list_remove(&it->link);
            free(it);
            break;
        }
    }
    pthread_mutex_unlock(&g_rel_lock);
}

static void relptr_destroy(struct wl_client* c, struct wl_resource* res) {
    wl_resource_destroy(res);
}
static const struct zwp_relative_pointer_v1_interface relptr_iface = {
    .destroy = relptr_destroy,
};

static void relmgr_get_relative_pointer(struct wl_client* c,
                                        struct wl_resource* res, uint32_t id,
                                        struct wl_resource* pointer) {
    struct wl_resource* r = wl_resource_create(
            c, &zwp_relative_pointer_v1_interface, 1, id);
    if (!r) { wl_resource_post_no_memory(res); return; }
    struct awl_relptr* rp = calloc(1, sizeof(*rp));
    if (!rp) { wl_resource_destroy(r); wl_resource_post_no_memory(res); return; }
    rp->res = r;
    pthread_mutex_lock(&g_rel_lock);
    wl_list_insert(g_relptrs.prev, &rp->link);
    pthread_mutex_unlock(&g_rel_lock);
    wl_resource_set_implementation(r, &relptr_iface, NULL, relptr_res_destroy);
}
static const struct zwp_relative_pointer_manager_v1_interface relmgr_iface = {
    .destroy = relptr_destroy,
    .get_relative_pointer = relmgr_get_relative_pointer,
};
static void relmgr_bind(struct wl_client* client, void* data,
                        uint32_t version, uint32_t id) {
    struct wl_resource* res = wl_resource_create(
            client, &zwp_relative_pointer_manager_v1_interface, 1, id);
    if (!res) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(res, &relmgr_iface, NULL, NULL);
}

/* ---------------- Pointer constraints state (zwp_pointer_constraints_v1) ----------------
 * Sync-only model (design principle): the input translation path never
 * consults this state. The list exists for the already_constrained check
 * and surface-death cleanup. Locking: rwl(rd) → g_constr_lock → ev_lock;
 * the resource destroy handler takes g_constr_lock only (it may run inside
 * wl_map teardown, when another thread holds rwl — see the section at the
 * end of this file). */
static struct wl_list g_constrs;
static pthread_mutex_t g_constr_lock = PTHREAD_MUTEX_INITIALIZER;

static void cursor_reset_state(void);   /* cursor section */
static void pcmgr_bind(struct wl_client* client, void* data,
                       uint32_t version, uint32_t id);   /* constraints section */

void awl_input_setup(void) {
    wl_list_init(&g_ptrs);
    wl_list_init(&g_kbds);
    wl_list_init(&g_tchs);
    wl_list_init(&g_relptrs);
    wl_list_init(&g_constrs);
    g_mods_depressed = g_mods_locked = 0;
    memset(g_keys_down, 0, sizeof(g_keys_down));
    cursor_reset_state();
    g_srv.g_seat = wl_global_create(g_srv.display, &wl_seat_interface, 5,
                                    NULL, seat_bind);
    if (!g_srv.g_seat) LOGE("wl_seat global create failed");
    if (!wl_global_create(g_srv.display,
                          &zwp_relative_pointer_manager_v1_interface,
                          1, NULL, relmgr_bind))
        LOGE("zwp_relative_pointer_manager_v1 global create failed");
    if (!wl_global_create(g_srv.display,
                          &zwp_pointer_constraints_v1_interface,
                          1, NULL, pcmgr_bind))
        LOGE("zwp_pointer_constraints_v1 global create failed");
}


/* ---------------- Event translation (per-event) ----------------
 * Routing authority is still Android (events carry their window id); layer
 * hit-testing belongs here:
 *   view coordinates → root buffer coordinates (geometry offset, same as
 *   the rendering side) → top-down layer hit over the render stack order
 *   (awl_subsurface_hit) → sent in layer-local coordinates.
 * The only routing state in this file = pointer focus layer + button grab +
 * touch point grab table (binder serial oneway delivery to the same node =
 * single-threaded access; g_input_lock is defensive).
 * Window dead / client has no matching proxy → silently dropped (the event
 * was addressed to a nonexistent target anyway). */

static struct wl_resource* resolve(struct wl_list* list, uint64_t win,
                                   struct awl_surface** out_s) {
    struct awl_surface* s = awl_surface_by_id(win);
    struct wl_resource* r = s ? res_for(list, win) : NULL;
    *out_s = s;
    return r;
}

/* Pointer focus layer + button bitmap (nonzero = grab: protocol pointer
 * focus pinned until release); g_ptr_grabbed = presses consumed by a popup
 * grab (the matching release is not delivered either) */
static _Atomic uint64_t g_ptr_focus;         /* written on the input thread; set_cursor (dispatch thread) reads it */
static _Atomic uint32_t g_ptr_enter_serial;  /* serial of the enter that set g_ptr_focus (kwin focusedSerial: set_cursor must echo it) */
static uint32_t g_ptr_buttons;
static uint32_t g_ptr_grabbed;

/* Touch point grab table: down fixes the layer, subsequent events reuse it until up/cancel (protocol touch focus is pinned) */
static struct { int32_t tid; uint64_t sid; } g_touches[16];

/* Window view coordinates → event target layer + layer-local buffer
 * coordinates (caller holds rwl.rd).
 * prefer>0: force that layer during a grab (coordinate translation only;
 * falls back to a hit automatically if the layer disappears).
 * rx/ry (optional): the root logical coordinates before the layer hit —
 * the basis of the cursor image position (same space as the layer stack). */
static struct awl_surface* input_target(struct awl_surface* root, uint64_t prefer,
                                        float* x, float* y, float* rx, float* ry) {
    pthread_mutex_lock(&root->ev_lock);
    view_to_surface(root, x, y);
    pthread_mutex_unlock(&root->ev_lock);
    if (rx) *rx = *x;
    if (ry) *ry = *y;
    return awl_subsurface_hit(root, *x, *y, prefer, 0, x, y);
}

/* Cursor hooks (section at the end of the file). Return values = window
 * whose Android pointer must be restored / redrawn once rwl is released
 * (0 = nothing); callbacks never run under a logic-layer lock. */
static uint64_t cursor_pointer_entered(uint64_t win, float rx, float ry);
static uint64_t cursor_pointer_moved(uint64_t win, float rx, float ry);
static uint64_t cursor_pointer_left(void);
static void cursor_restore(uint64_t win);
static void cursor_dirty(uint64_t win);

/* Drag-phase motion resolution (caller holds no rwl): view → root buffer →
 * layer hit (excluding the icon layer, no prefer — the DnD target is
 * hit-tested live per event) → handed to the drag machine (data_device
 * takes rwl.wr itself internally and rebuilds references by id). */
static void drag_deliver_motion(uint64_t win, float x, float y) {
    pthread_rwlock_rdlock(&g_srv.rwl);
    struct awl_surface* s = awl_surface_by_id(win);
    if (s) {
        pthread_mutex_lock(&s->ev_lock);
        view_to_surface(s, &x, &y);
        pthread_mutex_unlock(&s->ev_lock);
        float bx = x, by = y;
        struct awl_surface* hit = awl_subsurface_hit(
                s, bx, by, 0, awl_datadev_drag_icon_id(), &x, &y);
        uint64_t hit_id = hit ? hit->id : 0;
        pthread_rwlock_unlock(&g_srv.rwl);
        awl_datadev_drag_motion(win, hit_id, bx, by, x, y);
        return;
    }
    pthread_rwlock_unlock(&g_srv.rwl);
}

/* Pointer leaves the current focus layer (caller holds rwl.rd; strictly paired with enter) */
static void ptr_leave_focus(void) {
    if (!g_ptr_focus) return;
    struct awl_surface* s = awl_surface_by_id(g_ptr_focus);
    struct wl_resource* ptr = s ? res_for(&g_ptrs, s->id) : NULL;
    if (ptr && s->resource) {
        pthread_mutex_lock(&s->ev_lock);
        wl_pointer_send_leave(ptr, wl_display_next_serial(g_srv.display),
                              s->resource);
        wl_client_flush(wl_resource_get_client(ptr));
        pthread_mutex_unlock(&s->ev_lock);
    }
    g_ptr_focus = 0;
}

/* Send enter to `hit` and make it the pointer focus (caller holds rwl.rd);
 * the enter serial is what a following set_cursor must echo. */
static void ptr_enter_focus(struct wl_resource* ptr, struct awl_surface* hit,
                            float x, float y) {
    pthread_mutex_lock(&hit->ev_lock);
    uint32_t serial = wl_display_next_serial(g_srv.display);
    wl_pointer_send_enter(ptr, serial, hit->resource,
                          wl_fixed_from_double(x), wl_fixed_from_double(y));
    wl_client_flush(wl_resource_get_client(ptr));
    pthread_mutex_unlock(&hit->ev_lock);
    g_ptr_enter_serial = serial;
    g_ptr_focus = hit->id;
}

static void tr_ptr_enter(uint64_t win, float x, float y) {
    if (awl_datadev_drag_active()) return;   /* drag-phase hover belongs to the drag machine */
    uint64_t restore = 0;
    pthread_rwlock_rdlock(&g_srv.rwl);
    struct awl_surface* s;
    struct wl_resource* ptr = resolve(&g_ptrs, win, &s);
    if (ptr && s) {
        float rx, ry;
        struct awl_surface* hit = input_target(s, 0, &x, &y, &rx, &ry);
        if (g_ptr_focus) ptr_leave_focus();   /* a focus still held = its leave got lost (Activity died): pair it (protocol: one enter per focus) */
        ptr_enter_focus(ptr, hit, x, y);
        restore = cursor_pointer_entered(win, rx, ry);
    }
    pthread_rwlock_unlock(&g_srv.rwl);
    cursor_restore(restore);
}

static void tr_ptr_leave(uint64_t win) {
    if (awl_datadev_drag_active()) return;
    pthread_rwlock_rdlock(&g_srv.rwl);
    ptr_leave_focus();
    uint64_t restore = cursor_pointer_left();
    pthread_rwlock_unlock(&g_srv.rwl);
    cursor_restore(restore);
}

static void tr_ptr_motion(uint64_t win, float x, float y) {
    if (awl_datadev_drag_active()) {
        drag_deliver_motion(win, x, y);
        return;
    }
    uint64_t dirty = 0;
    pthread_rwlock_rdlock(&g_srv.rwl);
    struct awl_surface* s;
    struct wl_resource* ptr = resolve(&g_ptrs, win, &s);
    if (ptr && s) {
        uint64_t prefer = g_ptr_buttons ? g_ptr_focus : 0;
        float rx, ry;
        struct awl_surface* hit = input_target(s, prefer, &x, &y, &rx, &ry);
        if (!g_ptr_buttons && hit->id != g_ptr_focus) {
            /* cross-layer switch: old-layer leave paired with the new enter
             * (the client cursor persists across it — kwin keeps the server
             * cursor over focusedSurfaceChanged; the client re-sets it with
             * the new serial) */
            ptr_leave_focus();
            ptr_enter_focus(ptr, hit, x, y);
        }
        pthread_mutex_lock(&hit->ev_lock);
        wl_pointer_send_motion(ptr, awl_now_ms(),
                               wl_fixed_from_double(x),
                               wl_fixed_from_double(y));
        wl_client_flush(wl_resource_get_client(ptr));
        pthread_mutex_unlock(&hit->ev_lock);
        dirty = cursor_pointer_moved(win, rx, ry);   /* position → atomic; redraw if a cursor image is shown */
    }
    pthread_rwlock_unlock(&g_srv.rwl);
    cursor_dirty(dirty);
}

/* Relative motion (x/y = computed APK-side without capture: AXIS_RELATIVE_X/Y
 * or absolute delta, view pixels) →
 * zwp_relative_pointer_v1.relative_motion (all proxies of that client) + frame.
 * The delta is converted to logical coordinates (same basis as motion): just
 * multiply by the view_to_surface scale factor f = content base / physical
 * size — the translation term cancels in a delta; without the conversion, at
 * zoom≠100% the relative stream is 1/f of the absolute stream (measured: the
 * red crosshair moved at 2x speed at 200%). */
static void tr_ptr_rel(uint64_t win, double dx, double dy) {
    pthread_rwlock_rdlock(&g_srv.rwl);
    struct awl_surface* s;
    struct wl_resource* ptr = resolve(&g_ptrs, win, &s);
    if (ptr && s) {
        pthread_mutex_lock(&s->ev_lock);
        double sx, sy, ox, oy;
        awl_surface_view_map(s, &sx, &sy, &ox, &oy);
        pthread_mutex_unlock(&s->ev_lock);
        dx /= sx;   /* #34: same basis as view_to_surface — the translation term cancels in a delta */
        dy /= sy;
        struct wl_client* c = wl_resource_get_client(ptr);
        uint64_t utime = (uint64_t)awl_now_ms() * 1000;
        pthread_mutex_lock(&g_rel_lock);
        struct awl_relptr* it;
        wl_list_for_each(it, &g_relptrs, link) {
            if (wl_resource_get_client(it->res) == c)
                zwp_relative_pointer_v1_send_relative_motion(
                        it->res, (uint32_t)(utime >> 32), (uint32_t)utime,
                        wl_fixed_from_double(dx), wl_fixed_from_double(dy),
                        wl_fixed_from_double(dx), wl_fixed_from_double(dy));
        }
        pthread_mutex_unlock(&g_rel_lock);
        if (wl_resource_get_version(ptr) >= WL_POINTER_FRAME_SINCE_VERSION)
            wl_pointer_send_frame(ptr);
        wl_client_flush(c);
    }
    pthread_rwlock_unlock(&g_srv.rwl);
}

static void tr_ptr_button(uint64_t win, uint32_t btn, uint32_t state) {
    if (awl_datadev_drag_active()) {
        uint32_t bit = 1u << (btn & 31);
        if (!state) {
            g_ptr_buttons &= ~bit;
            g_ptr_grabbed &= ~bit;
            awl_datadev_drag_end();   /* release = drop (KWin endDrag) */
        }
        return;   /* presses during a drag are all consumed (implicit grab) */
    }
    pthread_rwlock_rdlock(&g_srv.rwl);
    struct awl_surface* s;
    struct wl_resource* ptr = resolve(&g_ptrs, win, &s);
    if (ptr && s) {
        if (!g_ptr_focus) g_ptr_focus = s->id;   /* missing enter: fall back to root */
        struct awl_surface* hit = awl_surface_by_id(g_ptr_focus);
        if (!hit) hit = s;
        uint32_t bit = 1u << (btn & 31);
        if (state && awl_popup_input_grab(hit)) {
            g_ptr_grabbed |= bit;   /* the matching release is consumed too */
        } else if (!state && (g_ptr_grabbed & bit)) {
            g_ptr_grabbed &= ~bit;
        } else if (hit->resource) {
            pthread_mutex_lock(&hit->ev_lock);
            wl_pointer_send_button(ptr, wl_display_next_serial(g_srv.display),
                                   awl_now_ms(), btn, state);
            wl_client_flush(wl_resource_get_client(ptr));
            pthread_mutex_unlock(&hit->ev_lock);
            if (state) g_ptr_buttons |= bit;
            else       g_ptr_buttons &= ~bit;
        }
    }
    pthread_rwlock_unlock(&g_srv.rwl);
}

/* Wheel / touchpad scroll → one wl_pointer frame:
 *   [motion] → axis_source → [axis_discrete] → axis (per non-zero axis) → frame
 * and, for a finger gesture end (v == 0 && h == 0, finger source):
 *   axis_source → axis_stop(vertical) → axis_stop(horizontal) → frame.
 *
 * axis_stop is NEVER sent in the same frame as an axis carrying a delta
 * (kwin-6.6.5 PointerInterface::sendAxis: axis_stop only when delta == 0).
 * Verified against chromium ui/ozone/platform/wayland/host/
 * wayland_event_source.cc: OnPointerAxisStopEvent zeroes dx/dy of that axis
 * before the frame is processed, so axis+axis_stop in one frame = the delta
 * is discarded and only a (zero-velocity) fling start is dispatched — that
 * was the 2026-09-10 "wheel and touchpad never scroll" bug.
 *
 * Value semantics (wayland.xml: axis value is a vector in the motion
 * coordinate space, i.e. positive = down / right):
 *   finger=0 (wheel): v/h are notches (Android AXIS_VSCROLL/HSCROLL already
 *     sign-converted by the APK) → axis = ×10 (10 units per click, the
 *     convention chromium/GTK divide by) + axis_discrete = accumulated whole
 *     notches (hi-res wheels report fractions; kwin accumulates too);
 *   finger=1 (touchpad two-finger, #30): raw pixel distance, source=finger,
 *     no discrete (continuous scrolling).
 * Protocol semantics: during axis the pointer "is deemed stationary" — the
 * optional position (v1/v2 = view coords; 0,0 = none) is sent as a motion
 * first so the client scrolls what is under the pointer. */
static float g_wheel_acc_v, g_wheel_acc_h;   /* fractional-notch accumulators for axis_discrete (binder serial = single-threaded) */

static int32_t wheel_discrete(float* acc, float v) {
    *acc += v;
    int32_t d = (int32_t)*acc;   /* trunc toward zero: whole notches accumulated so far */
    *acc -= (float)d;
    return d;
}

static void tr_ptr_axis(uint64_t win, float v, float h, uint32_t finger,
                        float px, float py) {
    pthread_rwlock_rdlock(&g_srv.rwl);
    struct awl_surface* s;
    struct wl_resource* ptr = resolve(&g_ptrs, win, &s);
    if (ptr && s) {
        int v5 = wl_resource_get_version(ptr) >= WL_POINTER_AXIS_SOURCE_SINCE_VERSION;
        struct awl_surface* hit = g_ptr_focus ? awl_surface_by_id(g_ptr_focus) : s;
        float lx = 0, ly = 0;
        if ((px != 0 || py != 0)) {
            lx = px; ly = py;
            struct awl_surface* lay =
                    input_target(s, g_ptr_buttons ? g_ptr_focus : 0, &lx, &ly, NULL, NULL);
            if (lay) hit = lay;   /* the coordinates are already that layer's local system (same source as tr_ptr_motion) */
        }
        if (hit) {
        pthread_mutex_lock(&hit->ev_lock);
        uint32_t t = awl_now_ms();
        if (px != 0 || py != 0)
            wl_pointer_send_motion(ptr, t,
                                   wl_fixed_from_double(lx),
                                   wl_fixed_from_double(ly));
        if (finger && v == 0 && h == 0) {
            /* gesture end: finger lifted → axis_stop (client starts its fling / kinetic scroll) */
            if (v5) {
                wl_pointer_send_axis_source(ptr, WL_POINTER_AXIS_SOURCE_FINGER);
                wl_pointer_send_axis_stop(ptr, t, WL_POINTER_AXIS_VERTICAL_SCROLL);
                wl_pointer_send_axis_stop(ptr, t, WL_POINTER_AXIS_HORIZONTAL_SCROLL);
                wl_pointer_send_frame(ptr);
                wl_client_flush(wl_resource_get_client(ptr));
            }
            pthread_mutex_unlock(&hit->ev_lock);
            pthread_rwlock_unlock(&g_srv.rwl);
            return;
        }
        if (v5)
            wl_pointer_send_axis_source(ptr, finger ? WL_POINTER_AXIS_SOURCE_FINGER
                                                    : WL_POINTER_AXIS_SOURCE_WHEEL);
        if (v != 0) {
            if (!finger && v5) {
                int32_t d = wheel_discrete(&g_wheel_acc_v, v);
                if (d) wl_pointer_send_axis_discrete(ptr, WL_POINTER_AXIS_VERTICAL_SCROLL, d);
            }
            wl_pointer_send_axis(ptr, t, WL_POINTER_AXIS_VERTICAL_SCROLL,
                                 wl_fixed_from_double(finger ? v : v * 10.0));
        }
        if (h != 0) {
            if (!finger && v5) {
                int32_t d = wheel_discrete(&g_wheel_acc_h, h);
                if (d) wl_pointer_send_axis_discrete(ptr, WL_POINTER_AXIS_HORIZONTAL_SCROLL, d);
            }
            wl_pointer_send_axis(ptr, t, WL_POINTER_AXIS_HORIZONTAL_SCROLL,
                                 wl_fixed_from_double(finger ? h : h * 10.0));
        }
        if (v5) wl_pointer_send_frame(ptr);
        wl_client_flush(wl_resource_get_client(ptr));
        pthread_mutex_unlock(&hit->ev_lock);
        }
    }
    pthread_rwlock_unlock(&g_srv.rwl);
}

/* Android meta → xkb modifier mask (standard bit order in the evdev keymap:
 * Shift=0 Lock=1 Control=2 Mod1(Alt)=3 Mod2(Num)=4 Mod3=5 Mod4(Logo)=6 Mod5=7) */
#define AM_SHIFT  0x000000C1u
#define AM_ALT    0x00000302u
#define AM_CTRL   0x00007000u
#define AM_META   0x00070000u
#define AM_CAPS   0x00100000u
#define AM_NUM    0x00200000u
#define AM_SCROLL 0x00400000u

static void meta_to_masks(uint32_t meta, uint32_t* dep, uint32_t* lck) {
    uint32_t d = 0, l = 0;
    if (meta & AM_SHIFT)  d |= 1u << 0;
    if (meta & AM_CTRL)   d |= 1u << 2;
    if (meta & AM_ALT)    d |= 1u << 3;
    if (meta & AM_META)   d |= 1u << 6;
    if (meta & AM_CAPS)   l |= 1u << 1;
    if (meta & AM_NUM)    l |= 1u << 4;
    if (meta & AM_SCROLL) l |= 1u << 7;
    *dep = d;
    *lck = l;
}

/* Keyboard focus gained (= Android window focus): enter + the currently
 * pressed key set + modifiers. Under binder serial delivery the old window's
 * KBD_LEAVE is guaranteed to arrive before this. IME: focus snapshot (the
 * enter target at v3 enable-commit time) + text-input enter made up. */
static void tr_kbd_enter(uint64_t win) {
    pthread_rwlock_rdlock(&g_srv.rwl);
    struct awl_surface* s;
    struct wl_resource* kbd = resolve(&g_kbds, win, &s);
    if (kbd) {
        awl_ime_set_focus(wl_resource_get_client(kbd), win);
        pthread_mutex_lock(&s->ev_lock);
        struct wl_array keys;
        wl_array_init(&keys);
        pthread_mutex_lock(&g_input_lock);
        for (uint32_t kc = 8; kc < 256; kc++)
            if (g_keys_down[kc / 8] & (1u << (kc % 8))) {
                uint32_t* slot = wl_array_add(&keys, sizeof(uint32_t));
                if (slot) *slot = kc;
            }
        uint32_t dep = g_mods_depressed, lck = g_mods_locked;
        pthread_mutex_unlock(&g_input_lock);
        wl_keyboard_send_enter(kbd, wl_display_next_serial(g_srv.display),
                               s->resource, &keys);
        wl_array_release(&keys);
        wl_keyboard_send_modifiers(kbd, wl_display_next_serial(g_srv.display),
                                   dep, 0, lck, 0);
        wl_client_flush(wl_resource_get_client(kbd));
        pthread_mutex_unlock(&s->ev_lock);
        awl_ime_focus_enter(win);   /* make up enter for an already-enabled text_input */
        pthread_rwlock_unlock(&g_srv.rwl);
        awl_datadev_focus_enter(wl_resource_get_client(kbd));   /* selection */
        return;
    }
    pthread_rwlock_unlock(&g_srv.rwl);
    awl_datadev_focus_enter(NULL);
}

static void tr_kbd_leave(uint64_t win) {
    pthread_rwlock_rdlock(&g_srv.rwl);
    struct awl_surface* s;
    struct wl_resource* kbd = resolve(&g_kbds, win, &s);
    if (kbd) {
        awl_ime_set_focus(NULL, 0);
        pthread_mutex_lock(&s->ev_lock);
        wl_keyboard_send_leave(kbd, wl_display_next_serial(g_srv.display),
                               s->resource);
        wl_client_flush(wl_resource_get_client(kbd));
        pthread_mutex_unlock(&s->ev_lock);
        awl_ime_focus_leave(win);   /* the leave symmetric to the text-input enter */
    }
    pthread_rwlock_unlock(&g_srv.rwl);
    awl_datadev_focus_leave();
}

/* Key: update the bitmap/modifiers (consistent under the lock) → send key + modifiers directly */
static void tr_key(uint64_t win, uint32_t code, uint32_t state, uint32_t meta) {
    uint32_t dep = 0, lck = 0;
    int mods_changed = 0;
    pthread_mutex_lock(&g_input_lock);
    if (code >= 8 && code < 256) {
        if (state) g_keys_down[code / 8] |= 1u << (code % 8);
        else       g_keys_down[code / 8] &= ~(1u << (code % 8));
    }
    meta_to_masks(meta, &dep, &lck);
    if (dep != g_mods_depressed || lck != g_mods_locked) {
        g_mods_depressed = dep;
        g_mods_locked = lck;
        mods_changed = 1;
    }
    pthread_mutex_unlock(&g_input_lock);

    pthread_rwlock_rdlock(&g_srv.rwl);
    struct awl_surface* s;
    struct wl_resource* kbd = resolve(&g_kbds, win, &s);
    if (kbd) {
        pthread_mutex_lock(&s->ev_lock);
        if (mods_changed)
            wl_keyboard_send_modifiers(kbd,
                    wl_display_next_serial(g_srv.display), dep, 0, lck, 0);
        wl_keyboard_send_key(kbd, wl_display_next_serial(g_srv.display),
                             awl_now_ms(), code, state);
        wl_client_flush(wl_resource_get_client(kbd));
        pthread_mutex_unlock(&s->ev_lock);
    }
    pthread_rwlock_unlock(&g_srv.rwl);
    uint32_t dm = 0;   /* DnD action negotiation modifiers (Ctrl=copy/Shift=move) */
    if (meta & AM_CTRL)  dm |= AWL_DMOD_CTRL;
    if (meta & AM_SHIFT) dm |= AWL_DMOD_SHIFT;
    awl_datadev_key_mods(dm);
}

/* Touch point grab table operations (g_input_lock; sid=0 = free slot — surface ids start at 1) */
static uint64_t touch_target(int32_t tid) {
    pthread_mutex_lock(&g_input_lock);
    uint64_t sid = 0;
    for (int i = 0; i < 16 && !sid; i++)
        if (g_touches[i].tid == tid && g_touches[i].sid) sid = g_touches[i].sid;
    pthread_mutex_unlock(&g_input_lock);
    return sid;
}
static void touch_remember(int32_t tid, uint64_t sid) {
    pthread_mutex_lock(&g_input_lock);
    int free_slot = -1;
    for (int i = 0; i < 16; i++) {
        if (g_touches[i].tid == tid && g_touches[i].sid) {
            g_touches[i].sid = sid;
            pthread_mutex_unlock(&g_input_lock);
            return;
        }
        if (!g_touches[i].sid && free_slot < 0) free_slot = i;
    }
    if (free_slot < 0) free_slot = 0;   /* overflow overwrites (>16 touch points is unrealistic) */
    g_touches[free_slot].tid = tid;
    g_touches[free_slot].sid = sid;
    pthread_mutex_unlock(&g_input_lock);
}
static void touch_forget(int32_t tid) {
    pthread_mutex_lock(&g_input_lock);
    for (int i = 0; i < 16; i++)
        if (g_touches[i].tid == tid && g_touches[i].sid) {
            g_touches[i].sid = 0;
            break;
        }
    pthread_mutex_unlock(&g_input_lock);
}

/* Touch (touch point id in code; down hit-tests the layer and grabs it,
 * motion/up keep that layer, coordinates translated to layer-local; frame
 * to finish) */
static void tr_touch(uint64_t win, const awl_input_ev_t* ev) {
    if (awl_datadev_drag_active()) {
        switch (ev->type) {
        case AWL_IN_TOUCH_MOTION:
            drag_deliver_motion(win, ev->x, ev->y);
            break;
        case AWL_IN_TOUCH_UP:
            touch_forget((int32_t)ev->code);
            awl_datadev_drag_end();
            break;
        case AWL_IN_TOUCH_CANCEL:
            touch_forget((int32_t)ev->code);
            awl_datadev_drag_cancel();
            break;
        default:   /* new presses during a drag are consumed */
            break;
        }
        return;
    }
    pthread_rwlock_rdlock(&g_srv.rwl);
    struct awl_surface* s;
    struct wl_resource* t = resolve(&g_tchs, win, &s);
    if (t && s) {
        float x = ev->x, y = ev->y;
        int32_t tid = (int32_t)ev->code;
        struct awl_surface* hit = input_target(s, touch_target(tid), &x, &y, NULL, NULL);
        if (ev->type == AWL_IN_TOUCH_DOWN && awl_popup_input_grab(hit)) {
            pthread_rwlock_unlock(&g_srv.rwl);
            return;   /* popup grab: the press is consumed (menu closed), not delivered */
        }
        pthread_mutex_lock(&hit->ev_lock);
        switch (ev->type) {
        case AWL_IN_TOUCH_DOWN:
            touch_remember(tid, hit->id);
            wl_touch_send_down(t, wl_display_next_serial(g_srv.display),
                               awl_now_ms(), hit->resource, tid,
                               wl_fixed_from_double(x),
                               wl_fixed_from_double(y));
            break;
        case AWL_IN_TOUCH_MOTION:
            wl_touch_send_motion(t, awl_now_ms(), tid,
                                 wl_fixed_from_double(x),
                                 wl_fixed_from_double(y));
            break;
        case AWL_IN_TOUCH_UP:
            touch_forget(tid);
            wl_touch_send_up(t, wl_display_next_serial(g_srv.display),
                             awl_now_ms(), tid);
            break;
        case AWL_IN_TOUCH_CANCEL:
            touch_forget(tid);
            wl_touch_send_cancel(t);
            break;
        }
        wl_touch_send_frame(t);
        wl_client_flush(wl_resource_get_client(t));
        pthread_mutex_unlock(&hit->ev_lock);
    }
    pthread_rwlock_unlock(&g_srv.rwl);
}

/* ---------------- Unified entry (binder pool threads; driver-side same-node oneway serial) --- */

void awl_input_dispatch(const awl_input_ev_t* ev) {
    switch (ev->type) {
    case AWL_IN_PTR_ENTER:  tr_ptr_enter(ev->id, ev->x, ev->y); break;
    case AWL_IN_PTR_LEAVE:  tr_ptr_leave(ev->id); break;
    case AWL_IN_PTR_MOTION: tr_ptr_motion(ev->id, ev->x, ev->y); break;
    case AWL_IN_PTR_BUTTON: tr_ptr_button(ev->id, ev->code,
                                          (uint32_t)ev->v1); break;
    case AWL_IN_PTR_AXIS:   tr_ptr_axis(ev->id, ev->x, ev->y, ev->code,
                                        ev->v1, ev->v2); break;
    case AWL_IN_PTR_REL:    tr_ptr_rel(ev->id, ev->x, ev->y); break;
    case AWL_IN_KBD_ENTER:  tr_kbd_enter(ev->id); break;
    case AWL_IN_KBD_LEAVE:  tr_kbd_leave(ev->id); break;
    case AWL_IN_KEY:        tr_key(ev->id, ev->code, (uint32_t)ev->v1,
                                   ev->meta); break;
    case AWL_IN_TOUCH_DOWN:
    case AWL_IN_TOUCH_MOTION:
    case AWL_IN_TOUCH_UP:
    case AWL_IN_TOUCH_CANCEL:
        tr_touch(ev->id, ev);
        break;
    default:
        LOGE("input ev type=%u?", ev->type);
        break;
    }
}

/* ---------------- Cursor: wl_pointer.set_cursor ----------------
 * Protocol side isomorphic to kwin-6.6.5 PointerInterfacePrivate::
 * pointer_set_cursor (src/wayland/pointer.cpp): accepted only from the
 * pointer-focused client with the serial of the enter that set the focus,
 * anything else silently ignored; the surface takes the "cursor" role (any
 * other role → wl_pointer.error.role); NULL = invisible pointer; a commit
 * with an attach offset moves the hotspot (SurfaceCursorSource::refresh).
 *
 * awl model (Android owns pointer position and the system pointer):
 *   - position: the Activity reports every enter/motion; the input thread
 *     stores the root-logical position in ONE atomic word (x:y packed as
 *     two wl_fixed_t) and the render thread reads it lock-free — no
 *     position state machine; image = position − hotspot;
 *   - "set" = the client called set_cursor since the pointer entered the
 *     window → the Android pointer of that window is hidden
 *     (cbs.pointer_cursor 1) and the renderer draws the cursor surface on
 *     top of the window (awl_pointer_cursor_layer; never hit-tested). Not
 *     set → the Android pointer stays visible (the fallback cursor);
 *   - cleared (pointer restored, cbs.pointer_cursor 0) when the pointer
 *     leaves the window, when the pointer (re)enters a window while a stale
 *     set is pending (the previous window's leave got lost — its Activity
 *     died), when the focus layer / the window / the cursor surface dies
 *     (kwin turns a dead cursor surface into an invisible one; here the
 *     system pointer is the better fallback, the client did not ask to hide
 *     it). A same-window layer switch (subsurface/popup) keeps it.
 * g_cursor is written by the input thread and by client dispatch threads
 * (set_cursor / commit / surface death under rwl.wr), read by render threads
 * → g_cursor_lock. Order: rwl → g_cursor_lock → ev_lock. */
static struct {
    int set;                      /* client took over the cursor since the pointer entered → Android pointer hidden */
    uint64_t win;                 /* window the pointer is in (= where the cursor image renders); 0 = outside */
    struct awl_surface* surface;  /* cursor image (NULL while set = invisible pointer); valid under rwl */
    int32_t hot_x, hot_y;         /* hotspot, cursor-surface logical coordinates */
} g_cursor;
static pthread_mutex_t g_cursor_lock = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t g_cursor_pos;   /* pointer position, root logical: (wl_fixed x << 32) | wl_fixed y */

static inline uint64_t pos_pack(float x, float y) {
    return ((uint64_t)(uint32_t)wl_fixed_from_double(x) << 32) |
           (uint32_t)wl_fixed_from_double(y);
}

static void cursor_reset_state(void) {
    pthread_mutex_lock(&g_cursor_lock);
    memset(&g_cursor, 0, sizeof(g_cursor));
    pthread_mutex_unlock(&g_cursor_lock);
    atomic_store(&g_cursor_pos, 0);
}

static void cursor_dirty(uint64_t win) {
    if (win && g_srv.cbs.window_dirty)
        g_srv.cbs.window_dirty(g_srv.cbs.user, win);
}

/* The client cursor of `win` ended → redraw without the cursor layer + give
 * the Android pointer back (no lock held) */
static void cursor_restore(uint64_t win) {
    if (!win) return;
    cursor_dirty(win);
    if (g_srv.cbs.pointer_cursor)
        g_srv.cbs.pointer_cursor(g_srv.cbs.user, win, 0);
    LOGI("window %llu: client cursor ended → android pointer restored", (unsigned long long)win);
}

/* Drop the set state (caller holds g_cursor_lock); returns the window to restore */
static uint64_t cursor_clear_locked(void) {
    uint64_t restore = g_cursor.set ? g_cursor.win : 0;
    g_cursor.set = 0;
    g_cursor.surface = NULL;
    return restore;
}

/* Android pointer entered `win` (rx,ry root logical): the cursor state
 * starts fresh — a set still pending belongs to a window whose leave never
 * arrived (its Activity died) → restore that one. */
static uint64_t cursor_pointer_entered(uint64_t win, float rx, float ry) {
    atomic_store(&g_cursor_pos, pos_pack(rx, ry));
    pthread_mutex_lock(&g_cursor_lock);
    uint64_t restore = cursor_clear_locked();
    g_cursor.win = win;
    pthread_mutex_unlock(&g_cursor_lock);
    return restore;
}

/* Pointer moved inside `win`: store the position; redraw when a cursor image is shown there */
static uint64_t cursor_pointer_moved(uint64_t win, float rx, float ry) {
    atomic_store(&g_cursor_pos, pos_pack(rx, ry));
    pthread_mutex_lock(&g_cursor_lock);
    g_cursor.win = win;   /* tolerate a motion without enter */
    uint64_t dirty = (g_cursor.set && g_cursor.surface) ? win : 0;
    pthread_mutex_unlock(&g_cursor_lock);
    return dirty;
}

static uint64_t cursor_pointer_left(void) {
    pthread_mutex_lock(&g_cursor_lock);
    uint64_t restore = cursor_clear_locked();
    g_cursor.win = 0;
    pthread_mutex_unlock(&g_cursor_lock);
    return restore;
}

static void pointer_set_cursor(struct wl_client* c, struct wl_resource* res,
                               uint32_t serial, struct wl_resource* surface_res,
                               int32_t hot_x, int32_t hot_y) {
    struct awl_surface* s = surface_res ? wl_resource_get_user_data(surface_res) : NULL;
    if (surface_res && !s) return;   /* surface already torn down */
    pthread_rwlock_rdlock(&g_srv.rwl);
    /* kwin: only the focused client, only with the focus enter's serial */
    struct awl_surface* focus = awl_surface_by_id(g_ptr_focus);
    if (!focus || !focus->resource || wl_resource_get_client(focus->resource) != c) {
        pthread_rwlock_unlock(&g_srv.rwl);
        LOGD("set_cursor from unfocused client ignored");
        return;
    }
    if (serial != g_ptr_enter_serial) {
        pthread_rwlock_unlock(&g_srv.rwl);
        LOGD("set_cursor serial %u != enter serial %u ignored", serial, (uint32_t)g_ptr_enter_serial);
        return;
    }
    if (s) {
        pthread_mutex_lock(&s->ev_lock);
        if ((s->role != AWL_ROLE_NONE && s->role != AWL_ROLE_CURSOR) || s->sub_parent) {
            int role = s->role;
            pthread_mutex_unlock(&s->ev_lock);
            pthread_rwlock_unlock(&g_srv.rwl);
            wl_resource_post_error(res, WL_POINTER_ERROR_ROLE,
                                   "the wl_surface already has a role assigned (%d)", role);
            return;
        }
        s->role = AWL_ROLE_CURSOR;   /* permanent: protocol roles are never reassigned */
        pthread_mutex_unlock(&s->ev_lock);
    }
    pthread_mutex_lock(&g_cursor_lock);
    int was_set = g_cursor.set;
    uint64_t win = g_cursor.win;
    g_cursor.set = 1;
    g_cursor.surface = s;
    g_cursor.hot_x = hot_x;
    g_cursor.hot_y = hot_y;
    pthread_mutex_unlock(&g_cursor_lock);
    pthread_rwlock_unlock(&g_srv.rwl);
    LOGD("set_cursor win=%llu surface=%llu hot=%d,%d", (unsigned long long)win,
         (unsigned long long)(s ? s->id : 0), hot_x, hot_y);
    if (!was_set) {
        if (g_srv.cbs.pointer_cursor)
            g_srv.cbs.pointer_cursor(g_srv.cbs.user, win, 1);
        LOGI("window %llu: client cursor %s → android pointer hidden",
             (unsigned long long)win, s ? "surface" : "NULL (invisible)");
    }
    cursor_dirty(win);   /* add / swap / remove the cursor layer */
}

/* ---- awl.h / awl_internal.h entry points ---- */

int awl_pointer_cursor_layer(uint64_t root_id, awl_layer_info_t* out) {
    int ok = 0;
    pthread_rwlock_rdlock(&g_srv.rwl);
    pthread_mutex_lock(&g_cursor_lock);
    struct awl_surface* cs = g_cursor.surface;   /* valid under rwl: cleared under rwl.wr before the surface is freed */
    if (g_cursor.set && cs && g_cursor.win == root_id) {
        pthread_mutex_lock(&cs->ev_lock);
        float w = 0, h = 0;
        awl_surface_logical_size(cs, &w, &h);   /* viewport dst | source | buffer/scale, logical px */
        float u0, v0, su, sv;
        awl_surface_layer_uv(cs, &u0, &v0, &su, &sv);
        int32_t xform = cs->buf_transform;
        pthread_mutex_unlock(&cs->ev_lock);
        if (w > 0.5f && h > 0.5f) {   /* no buffer yet → nothing to draw (the commit re-dirties) */
            uint64_t pos = atomic_load(&g_cursor_pos);   /* one word: x and y from the same event */
            memset(out, 0, sizeof(*out));
            out->surface_id = cs->id;
            out->x = (float)wl_fixed_to_double((wl_fixed_t)(uint32_t)(pos >> 32)) - (float)g_cursor.hot_x;
            out->y = (float)wl_fixed_to_double((wl_fixed_t)(uint32_t)pos) - (float)g_cursor.hot_y;
            out->w = w;
            out->h = h;
            out->u0 = u0; out->v0 = v0;
            out->su = su; out->sv = sv;
            out->transform = xform;
            ok = 1;
        }
    }
    pthread_mutex_unlock(&g_cursor_lock);
    pthread_rwlock_unlock(&g_srv.rwl);
    return ok;
}

uint64_t awl_input_cursor_window(struct awl_surface* s) {
    pthread_mutex_lock(&g_cursor_lock);
    uint64_t win = (g_cursor.set && g_cursor.surface == s) ? g_cursor.win : 0;
    pthread_mutex_unlock(&g_cursor_lock);
    return win;
}

void awl_input_cursor_commit(struct awl_surface* s, int32_t off_x, int32_t off_y) {
    pthread_mutex_lock(&g_cursor_lock);
    if (g_cursor.surface == s) {   /* kwin refresh(): m_hotspot -= m_surface->offset() */
        g_cursor.hot_x -= off_x;
        g_cursor.hot_y -= off_y;
    }
    pthread_mutex_unlock(&g_cursor_lock);
}

uint64_t awl_input_surface_gone(struct awl_surface* s) {
    /* focus layer, its window, or the cursor image died → the client cursor
     * ends (a dangling g_ptr_focus is re-entered by the next motion, as before) */
    int is_focus = s->id == g_ptr_focus;
    pthread_mutex_lock(&g_cursor_lock);
    uint64_t win = 0;
    if (is_focus || s->id == g_cursor.win || s == g_cursor.surface)
        win = cursor_clear_locked();
    if (s->id == g_cursor.win) g_cursor.win = 0;
    pthread_mutex_unlock(&g_cursor_lock);
    return win;
}

void awl_input_cursor_gone_notify(uint64_t win) {
    cursor_restore(win);
}

/* ---------------- Pointer constraints (zwp_pointer_constraints_v1) ----------------
 * Pure state sync (design principle: the input translation path in this
 * file is untouched — no constraint branch anywhere; the daemon never
 * filters or synthesizes pointer events). The compositor-side job is
 * exactly:
 *   lock_pointer / confine_pointer request → send locked/confined once,
 *     then cbs.pointer_lock → C_CAPTURE (mode + confine rect, view px) →
 *     the Activity requestPointerCapture()s and becomes the authority
 *     (PTR_REL + clamped absolute motion are all APK-side)
 *   object destroy / client gone / surface gone → cbs.pointer_lock(none)
 *     → releasePointerCapture
 *   set_region on a live constraint → re-convert + re-send C_CAPTURE
 * Activation gating (pointer focus), oneshot-vs-persistent transitions and
 * region clamping are all APK-side; the lifetime enum is therefore not
 * tracked (the APK re-requests capture per focus, re-attach re-pushes the
 * mirror — a oneshot client that needs out destroys the object itself).
 * already_constrained stays protocol-exact (kwin: one constraint per
 * surface, the object keeps blocking new requests until destroyed).
 *
 * Region coordinates: interpreted in root-local logical px (the same space
 * input_target produces), converted to Activity view px once at
 * request/set_region time (inverse of view_to_surface; the APK intersects
 * the rect with the live window on every event, so a stale rect is safe).
 * The wl_region resource is copied (bbox), never held. */
struct awl_constr {
    struct wl_resource* res;     /* zwp_locked/confined_pointer_v1 */
    struct wl_client* client;
    uint64_t surface_id;         /* requesting surface (already_constrained key) */
    uint64_t root_id;            /* root window (C_CAPTURE address) */
    int mode;                    /* AWL_CAPTURE_CONFINE / _LOCK */
    int dead;                    /* surface gone: object stays until the client destroys it */
    int has_region;
    int32_t rg_x, rg_y, rg_w, rg_h;   /* confine region, root-local logical */
    int32_t r[4];                /* last C_CAPTURE rect, view px (cached: the
                                  * destroy path may not take rwl to re-convert) */
    struct wl_list link;
};

/* Region (root-local logical) → Activity view px, the inverse of
 * view_to_surface: view = (rg − geometry origin) × s + o through the root's
 * view mapping (awl_surface_view_map: 1:1 at Z, or the scale_mode letterbox
 * offsets for content ignoring the configure).
 * Caller holds rwl.rd + root ev_lock. No region / unknown window size →
 * the whole window (zeros are the APK's "whole window" convention). */
static void constr_app_rect(struct awl_surface* root, int has_region,
                            int32_t rx, int32_t ry, int32_t rw, int32_t rh,
                            int32_t* out) {
    float cw = 0, ch = 0;
    awl_surface_content_size(root, &cw, &ch);
    if (!has_region || root->phys_w <= 0 || root->phys_h <= 0 ||
        cw <= 0.5f || ch <= 0.5f) {
        out[0] = 0; out[1] = 0;
        out[2] = root->phys_w; out[3] = root->phys_h;
        return;
    }
    double sx, sy, ox, oy;
    awl_surface_view_map(root, &sx, &sy, &ox, &oy);
    double x = rx, y = ry, w = rw, h = rh;
    if (root->geom_valid) { x -= root->geom_x; y -= root->geom_y; }
    x = x * sx + ox;
    y = y * sy + oy;
    w *= sx;
    h *= sy;
    out[0] = x < 0 ? 0 : (int32_t)x;
    out[1] = y < 0 ? 0 : (int32_t)y;
    out[2] = w < 0 ? 0 : (int32_t)w;
    out[3] = h < 0 ? 0 : (int32_t)h;
}

static void constr_destroy(struct wl_client* client, struct wl_resource* res) {
    wl_resource_destroy(res);
}

/* Resource teardown (destroy request / client disconnect / wl_map teardown):
 * takes g_constr_lock ONLY — never rwl (teardown may run while another
 * thread holds it). A live constraint ending → C_CAPTURE none, unless a
 * sibling constraint of the same root survives (its cached state is
 * re-pushed — the r[] cache makes this rwl-free). */
static void constr_res_destroy(struct wl_resource* res) {
    struct awl_constr* c = wl_resource_get_user_data(res);
    if (!c) return;
    pthread_mutex_lock(&g_constr_lock);
    wl_list_remove(&c->link);
    struct awl_constr* keep = NULL;
    struct awl_constr* it;
    wl_list_for_each(it, &g_constrs, link) {
        if (it->root_id == c->root_id && !it->dead) { keep = it; break; }
    }
    int dead = c->dead;
    uint64_t root = c->root_id;
    int keep_mode = keep ? keep->mode : 0;
    int32_t r[4] = {0, 0, 0, 0};
    if (keep) memcpy(r, keep->r, sizeof(r));
    pthread_mutex_unlock(&g_constr_lock);
    free(c);
    if (dead || !g_srv.cbs.pointer_lock) return;   /* surface already gone: none was pushed then */
    if (keep)
        g_srv.cbs.pointer_lock(g_srv.cbs.user, root, keep_mode,
                               r[0], r[1], r[2], r[3]);
    else
        g_srv.cbs.pointer_lock(g_srv.cbs.user, root, AWL_CAPTURE_NONE, 0, 0, 0, 0);
}

/* Android owns the pointer position: the hint (kwin warps the cursor here)
 * cannot be applied — log only */
static void locked_set_cursor_position_hint(struct wl_client* client,
                                            struct wl_resource* res,
                                            wl_fixed_t sx, wl_fixed_t sy) {
    LOGD("locked_pointer cursor position hint %.1f,%.1f ignored",
         wl_fixed_to_double(sx), wl_fixed_to_double(sy));
}

/* set_region (both object types): applies immediately (no commit
 * double-buffer — the APK intersects the rect with the live window on
 * every event anyway) and re-pushes C_CAPTURE with the re-converted rect */
static void constr_set_region(struct wl_client* client, struct wl_resource* res,
                              struct wl_resource* region) {
    struct awl_constr* c = wl_resource_get_user_data(res);
    if (!c || c->dead) return;
    int32_t x = 0, y = 0, w = 0, h = 0;
    int has = awl_region_bbox(region, &x, &y, &w, &h);
    pthread_rwlock_rdlock(&g_srv.rwl);
    struct awl_surface* root = awl_surface_by_id(c->root_id);
    if (!root) {
        pthread_rwlock_unlock(&g_srv.rwl);
        return;
    }
    pthread_mutex_lock(&g_constr_lock);
    pthread_mutex_lock(&root->ev_lock);
    c->has_region = has;
    c->rg_x = x; c->rg_y = y; c->rg_w = w; c->rg_h = h;
    constr_app_rect(root, has, x, y, w, h, c->r);
    pthread_mutex_unlock(&root->ev_lock);
    int mode = c->mode;
    int32_t r[4];
    memcpy(r, c->r, sizeof(r));
    pthread_mutex_unlock(&g_constr_lock);
    pthread_rwlock_unlock(&g_srv.rwl);
    if (g_srv.cbs.pointer_lock)
        g_srv.cbs.pointer_lock(g_srv.cbs.user, c->root_id, mode,
                               r[0], r[1], r[2], r[3]);
}

static const struct zwp_locked_pointer_v1_interface locked_iface = {
    .destroy = constr_destroy,
    .set_cursor_position_hint = locked_set_cursor_position_hint,
    .set_region = constr_set_region,
};
static const struct zwp_confined_pointer_v1_interface confined_iface = {
    .destroy = constr_destroy,
    .set_region = constr_set_region,
};

/* lock_pointer / confine_pointer common path (client dispatch thread):
 * already_constrained check → create → locked/confined once → C_CAPTURE
 * push after rwl release (callbacks never run under a logic-layer lock). */
static void constr_create(struct wl_client* client, struct wl_resource* mgr,
                          uint32_t id, struct wl_resource* surface_res,
                          struct wl_resource* region, int mode) {
    int32_t x = 0, y = 0, w = 0, h = 0;
    int has = awl_region_bbox(region, &x, &y, &w, &h);

    pthread_rwlock_rdlock(&g_srv.rwl);
    struct awl_surface* s = surface_res ? wl_resource_get_user_data(surface_res) : NULL;
    if (s) {   /* kwin: one constraint per surface until the object is destroyed */
        pthread_mutex_lock(&g_constr_lock);
        struct awl_constr* it;
        int clash = 0;
        wl_list_for_each(it, &g_constrs, link) {
            if (it->surface_id == s->id) { clash = 1; break; }
        }
        pthread_mutex_unlock(&g_constr_lock);
        if (clash) {
            pthread_rwlock_unlock(&g_srv.rwl);
            wl_resource_post_error(mgr,
                    ZWP_POINTER_CONSTRAINTS_V1_ERROR_ALREADY_CONSTRAINED,
                    "a pointer constraint already exists on that surface");
            return;
        }
    }

    struct wl_resource* obj = wl_resource_create(
            client,
            mode == AWL_CAPTURE_LOCK ? &zwp_locked_pointer_v1_interface
                                     : &zwp_confined_pointer_v1_interface,
            1, id);
    struct awl_constr* c = calloc(1, sizeof(*c));
    if (!obj || !c) {
        if (obj) wl_resource_destroy(obj);
        free(c);
        pthread_rwlock_unlock(&g_srv.rwl);
        wl_resource_post_no_memory(mgr);
        return;
    }
    c->res = obj;
    c->client = client;
    c->mode = mode;
    c->has_region = has;
    c->rg_x = x; c->rg_y = y; c->rg_w = w; c->rg_h = h;
    if (!s) {
        c->dead = 1;   /* surface already destroyed (unreachable in practice: same-client requests serialize) */
    } else {
        c->surface_id = s->id;
        c->root_id = awl_subsurface_root(s)->id;
        struct awl_surface* root = awl_surface_by_id(c->root_id);
        pthread_mutex_lock(&g_constr_lock);
        if (root) {
            pthread_mutex_lock(&root->ev_lock);
            constr_app_rect(root, has, x, y, w, h, c->r);
            pthread_mutex_unlock(&root->ev_lock);
        }
        wl_list_insert(g_constrs.prev, &c->link);
        pthread_mutex_unlock(&g_constr_lock);
        /* activation = creation (sync model): the event once, in the same
         * ev_lock group as the pointer stream of that window */
        if (root) {
            pthread_mutex_lock(&root->ev_lock);
            if (mode == AWL_CAPTURE_LOCK)
                zwp_locked_pointer_v1_send_locked(obj);
            else
                zwp_confined_pointer_v1_send_confined(obj);
            wl_client_flush(client);
            pthread_mutex_unlock(&root->ev_lock);
        }
    }
    wl_resource_set_implementation(
            obj,
            mode == AWL_CAPTURE_LOCK ? (const void*)&locked_iface
                                     : (const void*)&confined_iface,
            c, constr_res_destroy);
    uint64_t root_id = c->root_id;
    int dead = c->dead;
    int32_t r[4];
    memcpy(r, c->r, sizeof(r));
    pthread_rwlock_unlock(&g_srv.rwl);
    LOGI("%s constraint: window %llu region(view px)=%d,%d %dx%d",
         mode == AWL_CAPTURE_LOCK ? "lock" : "confine",
         (unsigned long long)root_id, r[0], r[1], r[2], r[3]);
    if (!dead && g_srv.cbs.pointer_lock)
        g_srv.cbs.pointer_lock(g_srv.cbs.user, root_id, mode,
                               r[0], r[1], r[2], r[3]);
}

static void pcmgr_lock_pointer(struct wl_client* client, struct wl_resource* res,
                               uint32_t id, struct wl_resource* surface,
                               struct wl_resource* pointer,
                               struct wl_resource* region, uint32_t lifetime) {
    constr_create(client, res, id, surface, region, AWL_CAPTURE_LOCK);
}
static void pcmgr_confine_pointer(struct wl_client* client, struct wl_resource* res,
                                  uint32_t id, struct wl_resource* surface,
                                  struct wl_resource* pointer,
                                  struct wl_resource* region, uint32_t lifetime) {
    constr_create(client, res, id, surface, region, AWL_CAPTURE_CONFINE);
}
static const struct zwp_pointer_constraints_v1_interface pcmgr_iface = {
    .destroy = constr_destroy,
    .lock_pointer = pcmgr_lock_pointer,
    .confine_pointer = pcmgr_confine_pointer,
};
static void pcmgr_bind(struct wl_client* client, void* data,
                       uint32_t version, uint32_t id) {
    struct wl_resource* res = wl_resource_create(
            client, &zwp_pointer_constraints_v1_interface, 1, id);
    if (!res) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(res, &pcmgr_iface, NULL, NULL);
}

/* ---- surface death (awl_surface.c destroy path; caller holds rwl.wr) ----
 * Every constraint on this surface / its root dies with it:
 * unlocked/unconfined is still sent (the client usually outlives its
 * surface); the object itself stays (destroying it here would recurse into
 * constr_res_destroy under rwl.wr — the client destroys it, or its
 * disconnect does). Returns the root window whose capture state changed
 * (0 = none) for awl_input_constr_gone_notify AFTER the rwl release. */
uint64_t awl_input_constr_surface_gone(struct awl_surface* s) {
    pthread_mutex_lock(&g_constr_lock);
    uint64_t root = 0;
    struct awl_constr* c;
    struct awl_constr* tmp;
    wl_list_for_each_safe(c, tmp, &g_constrs, link) {
        if (c->dead || (c->surface_id != s->id && c->root_id != s->id)) continue;
        c->dead = 1;
        root = c->root_id;
        if (c->mode == AWL_CAPTURE_LOCK)
            zwp_locked_pointer_v1_send_unlocked(c->res);
        else
            zwp_confined_pointer_v1_send_unconfined(c->res);
        wl_client_flush(c->client);
    }
    pthread_mutex_unlock(&g_constr_lock);
    return root;
}

void awl_input_constr_gone_notify(uint64_t win) {
    if (!win || !g_srv.cbs.pointer_lock) return;
    g_srv.cbs.pointer_lock(g_srv.cbs.user, win, AWL_CAPTURE_NONE, 0, 0, 0, 0);
}

/* Re-convert + re-push the confine rects (view px) of the live constraints
 * (#34): the view mapping moved underneath them — a scale_mode switch
 * (awl_viewport.c) or a window resize (awl_xdg.c tail; the letterbox offset
 * and stretch ratio both derive from phys). Without this the APK clamp box
 * drifts onto the black bars. root_id 0 = every root.
 * Lock order rwl.rd → g_constr_lock → root ev_lock (same as
 * constr_set_region); callbacks fire after every lock is released. Before
 * each push the constraint is re-checked to still be linked (narrows the
 * race against constr_res_destroy, which unlinks under g_constr_lock only —
 * a residual window remains, same class as the APK's documented
 * C_CAPTURE×input cross-node ordering). Dead constraints are skipped
 * (surface_gone already pushed NONE); LOCK rects are ignored by the APK —
 * re-pushing is harmless. */
void awl_input_constr_remap(uint64_t root_id) {
    if (!g_srv.cbs.pointer_lock) return;
    struct remap_snap {
        uint64_t root;
        struct awl_constr* c;
        int mode;
        int32_t r[4];
    };
    struct remap_snap snap[16];
    int n;
    do {   /* full batch = maybe more entries: another idempotent pass */
        n = 0;
        pthread_rwlock_rdlock(&g_srv.rwl);
        pthread_mutex_lock(&g_constr_lock);
        struct awl_constr* c;
        wl_list_for_each(c, &g_constrs, link) {
            if (c->dead || (root_id && c->root_id != root_id)) continue;
            struct awl_surface* root = awl_surface_by_id(c->root_id);
            if (!root) continue;
            pthread_mutex_lock(&root->ev_lock);
            constr_app_rect(root, c->has_region,
                            c->rg_x, c->rg_y, c->rg_w, c->rg_h, c->r);
            pthread_mutex_unlock(&root->ev_lock);
            if (n < (int)(sizeof(snap) / sizeof(snap[0]))) {
                snap[n].root = c->root_id;
                snap[n].c = c;
                snap[n].mode = c->mode;
                memcpy(snap[n].r, c->r, sizeof(snap[n].r));
                n++;
            }
        }
        pthread_mutex_unlock(&g_constr_lock);
        pthread_rwlock_unlock(&g_srv.rwl);
        for (int i = 0; i < n; i++) {
            int alive = 0;
            pthread_mutex_lock(&g_constr_lock);
            struct awl_constr* it;
            wl_list_for_each(it, &g_constrs, link)
                if (it == snap[i].c) { alive = 1; break; }
            pthread_mutex_unlock(&g_constr_lock);
            if (!alive) continue;   /* destroyed while the locks were down */
            g_srv.cbs.pointer_lock(g_srv.cbs.user, snap[i].root, snap[i].mode,
                                   snap[i].r[0], snap[i].r[1],
                                   snap[i].r[2], snap[i].r[3]);
        }
    } while (n == (int)(sizeof(snap) / sizeof(snap[0])));
}
