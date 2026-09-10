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
 */
#define _GNU_SOURCE   /* bionic: memfd_create */
#include "awl_internal.h"

#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/mman.h>          /* memfd: keymap */

#include "keymap_evdev.h"
#include "relative-pointer-unstable-v1-server-protocol.h"

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

/* Window view (Android physical pixels) → root logical coordinates (#31 zoom).
 * f = content base size / window physical size (awl_surface_content_size /
 * phys_w) — chrome-like clients' viewport dst includes shadow margins, the
 * content base = the xdg geometry rectangle (= configure size): f = 1/Z
 * exactly; also holds when fixed-size clients are stretched.
 * The xdg geometry origin is aligned with the view origin (shared with the
 * rendering dst, both in logical coordinates):
 * with geometry → logical = geom origin + view×f; without → view×f.
 * Caller holds s->ev_lock. */
static void view_to_surface(struct awl_surface* s, float* x, float* y) {
    float lw = 0, lh = 0;
    awl_surface_content_size(s, &lw, &lh);
    float fx = (s->phys_w > 0 && lw > 0.5f) ? lw / (float)s->phys_w : 1.0f;
    float fy = (s->phys_h > 0 && lh > 0.5f) ? lh / (float)s->phys_h : 1.0f;
    if (s->geom_valid) {
        *x = (float)s->geom_x + *x * fx;
        *y = (float)s->geom_y + *y * fy;
    } else {
        *x *= fx;
        *y *= fy;
    }
}

/* ---------------- wl_pointer / wl_keyboard / wl_touch client-side interface ---- */

static void pointer_set_cursor(struct wl_client* c, struct wl_resource* res,
                               uint32_t serial, struct wl_resource* surface,
                               int32_t hot_x, int32_t hot_y) {
    /* cursor surface composited rendering (todo #22); during capture the client sends NULL — just recorded for now */
    LOGD("set_cursor serial=%u surface=%p hot=%d,%d", serial,
            (void*)surface, hot_x, hot_y);
}
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
 * No pointer-constraints — dx is computed on the APK side without capture
 * (AXIS_RELATIVE_X/Y or absolute delta, see the legacy equivalent); this
 * side keeps zero state and broadcasts per client. The list is built/
 * destroyed on the client dispatch thread and read on the binder input
 * thread — guarded by its own g_rel_lock. */
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

void awl_input_setup(void) {
    wl_list_init(&g_ptrs);
    wl_list_init(&g_kbds);
    wl_list_init(&g_tchs);
    wl_list_init(&g_relptrs);
    g_mods_depressed = g_mods_locked = 0;
    memset(g_keys_down, 0, sizeof(g_keys_down));
    g_srv.g_seat = wl_global_create(g_srv.display, &wl_seat_interface, 5,
                                    NULL, seat_bind);
    if (!g_srv.g_seat) LOGE("wl_seat global create failed");
    if (!wl_global_create(g_srv.display,
                          &zwp_relative_pointer_manager_v1_interface,
                          1, NULL, relmgr_bind))
        LOGE("zwp_relative_pointer_manager_v1 global create failed");
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
static uint64_t g_ptr_focus;
static uint32_t g_ptr_buttons;
static uint32_t g_ptr_grabbed;

/* Touch point grab table: down fixes the layer, subsequent events reuse it until up/cancel (protocol touch focus is pinned) */
static struct { int32_t tid; uint64_t sid; } g_touches[16];

/* Window view coordinates → event target layer + layer-local buffer
 * coordinates (caller holds rwl.rd).
 * prefer>0: force that layer during a grab (coordinate translation only;
 * falls back to a hit automatically if the layer disappears). */
static struct awl_surface* input_target(struct awl_surface* root, uint64_t prefer,
                                        float* x, float* y) {
    pthread_mutex_lock(&root->ev_lock);
    view_to_surface(root, x, y);
    pthread_mutex_unlock(&root->ev_lock);
    return awl_subsurface_hit(root, *x, *y, prefer, 0, x, y);
}

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

static void tr_ptr_enter(uint64_t win, float x, float y) {
    if (awl_datadev_drag_active()) return;   /* drag-phase hover belongs to the drag machine */
    pthread_rwlock_rdlock(&g_srv.rwl);
    struct awl_surface* s;
    struct wl_resource* ptr = resolve(&g_ptrs, win, &s);
    if (ptr && s) {
        struct awl_surface* hit = input_target(s, 0, &x, &y);
        pthread_mutex_lock(&hit->ev_lock);
        wl_pointer_send_enter(ptr, wl_display_next_serial(g_srv.display),
                              hit->resource, wl_fixed_from_double(x),
                              wl_fixed_from_double(y));
        wl_client_flush(wl_resource_get_client(ptr));
        pthread_mutex_unlock(&hit->ev_lock);
        g_ptr_focus = hit->id;
    }
    pthread_rwlock_unlock(&g_srv.rwl);
}

static void tr_ptr_leave(uint64_t win) {
    if (awl_datadev_drag_active()) return;
    pthread_rwlock_rdlock(&g_srv.rwl);
    ptr_leave_focus();
    pthread_rwlock_unlock(&g_srv.rwl);
}

static void tr_ptr_motion(uint64_t win, float x, float y) {
    if (awl_datadev_drag_active()) {
        drag_deliver_motion(win, x, y);
        return;
    }
    pthread_rwlock_rdlock(&g_srv.rwl);
    struct awl_surface* s;
    struct wl_resource* ptr = resolve(&g_ptrs, win, &s);
    if (ptr && s) {
        uint64_t prefer = g_ptr_buttons ? g_ptr_focus : 0;
        struct awl_surface* hit = input_target(s, prefer, &x, &y);
        if (!g_ptr_buttons && hit->id != g_ptr_focus) {
            ptr_leave_focus();   /* cross-layer switch: old-layer leave paired with the new enter */
            pthread_mutex_lock(&hit->ev_lock);
            wl_pointer_send_enter(ptr, wl_display_next_serial(g_srv.display),
                                  hit->resource, wl_fixed_from_double(x),
                                  wl_fixed_from_double(y));
            wl_client_flush(wl_resource_get_client(ptr));
            pthread_mutex_unlock(&hit->ev_lock);
            g_ptr_focus = hit->id;
        }
        pthread_mutex_lock(&hit->ev_lock);
        wl_pointer_send_motion(ptr, awl_now_ms(),
                               wl_fixed_from_double(x),
                               wl_fixed_from_double(y));
        wl_client_flush(wl_resource_get_client(ptr));
        pthread_mutex_unlock(&hit->ev_lock);
    }
    pthread_rwlock_unlock(&g_srv.rwl);
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
        float lw = 0, lh = 0;
        awl_surface_content_size(s, &lw, &lh);
        float fx = (s->phys_w > 0 && lw > 0.5f) ? lw / (float)s->phys_w : 1.0f;
        float fy = (s->phys_h > 0 && lh > 0.5f) ? lh / (float)s->phys_h : 1.0f;
        pthread_mutex_unlock(&s->ev_lock);
        dx *= fx;
        dy *= fy;
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
                    input_target(s, g_ptr_buttons ? g_ptr_focus : 0, &lx, &ly);
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
        struct awl_surface* hit = input_target(s, touch_target(tid), &x, &y);
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
