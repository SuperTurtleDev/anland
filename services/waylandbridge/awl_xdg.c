/* awl_xdg.c — xdg_wm_base v3 / xdg_surface / xdg_toplevel / xdg_popup
 *
 * Window lifecycle: get_toplevel → initial configure(0,0) (client picks size)
 * → client ack + first buffer commit → map → window_created callback (Android
 * creates the window) → Android resize → awl_window_resize → configure(w,h) →
 * client rebuilds buffer
 *
 * configure sending: under s->ev_lock (fields + messages atomic as a group,
 * per-client order); the binder-thread direct-send path also holds rdlock and
 * flushes explicitly.
 */
#include "awl_internal.h"

#include <math.h>
#include <string.h>

#define AWL_XDG_VERSION 3

/* caller holds s->ev_lock */
static void send_configure_locked(struct awl_surface* s,
                                  int32_t w, int32_t h,
                                  const uint32_t* states, size_t nstates) {
    struct wl_array arr;
    wl_array_init(&arr);
    if (nstates) {
        uint32_t* p = wl_array_add(&arr, nstates * sizeof(uint32_t));
        if (p) memcpy(p, states, nstates * sizeof(uint32_t));
    }
    s->configure_serial = wl_display_get_serial(g_srv.display);
    s->conf_w = w;
    s->conf_h = h;
    s->configured = 1;
    xdg_toplevel_send_configure(s->xdg_role_res, w, h, &arr);
    wl_array_release(&arr);
    xdg_surface_send_configure(s->xdg_surface_res, s->configure_serial);
    LOGD("surface %llu configure %dx%d serial=%u",
            (unsigned long long)s->id, w, h, s->configure_serial);
}

/* Event-thread convenience entry (takes the lock itself) */
static void send_toplevel_configure(struct awl_surface* s,
                                    int32_t w, int32_t h,
                                    const uint32_t* states, size_t nstates) {
    pthread_mutex_lock(&s->ev_lock);
    send_configure_locked(s, w, h, states, nstates);
    pthread_mutex_unlock(&s->ev_lock);
}

/* ---------------- xdg_toplevel ---------------- */

static void toplevel_destroy(struct wl_client* c, struct wl_resource* res) {
    wl_resource_destroy(res);
}

static void toplevel_set_parent(struct wl_client* c, struct wl_resource* res,
                                struct wl_resource* parent) {
    /* Android task stack manages parent/child relations itself */
}

static void toplevel_set_title(struct wl_client* c, struct wl_resource* res,
                               const char* title) {
    struct awl_surface* s = wl_resource_get_user_data(res);
    if (!s) return;
    snprintf(s->title, sizeof(s->title), "%s", title ? title : "");
    if (s->mapped && g_srv.cbs.window_title)
        g_srv.cbs.window_title(g_srv.cbs.user, s->id, s->title);
}

static void toplevel_set_app_id(struct wl_client* c, struct wl_resource* res,
                                const char* app_id) {
    LOGI("app_id=%s", app_id ? app_id : "");
}

static void toplevel_show_window_menu(struct wl_client* c, struct wl_resource* res,
                                      struct wl_resource* seat, uint32_t serial,
                                      int32_t x, int32_t y) {
    /* Input deferred */
}
static void toplevel_move(struct wl_client* c, struct wl_resource* res,
                          struct wl_resource* seat, uint32_t serial) {}
static void toplevel_resize(struct wl_client* c, struct wl_resource* res,
                            struct wl_resource* seat, uint32_t serial, uint32_t edges) {}

static void toplevel_set_max_size(struct wl_client* c, struct wl_resource* res,
                                  int32_t w, int32_t h) {}
static void toplevel_set_min_size(struct wl_client* c, struct wl_resource* res,
                                  int32_t w, int32_t h) { LOGD("set_min_size"); }

static void toplevel_set_maximized(struct wl_client* c, struct wl_resource* res) {
    struct awl_surface* s = wl_resource_get_user_data(res);
    if (!s) return;
    /* Placeholder from daemon config (never use conf_w/h — stale after orientation
     * switch, would crash; physical display values are meaningless in DeX-like
     * scenarios). The real size is only sent by awl_window_resize triggered by
     * Activity SURFACE changes. */
    uint32_t states[] = { XDG_TOPLEVEL_STATE_MAXIMIZED };
    send_toplevel_configure(s, g_srv.init_conf_w, g_srv.init_conf_h, states, 1);
}
static void toplevel_unset_maximized(struct wl_client* c, struct wl_resource* res) {
    struct awl_surface* s = wl_resource_get_user_data(res);
    if (s) send_toplevel_configure(s, 0, 0, NULL, 0);
}
static void toplevel_set_fullscreen(struct wl_client* c, struct wl_resource* res,
                                    struct wl_resource* output) {
    struct awl_surface* s = wl_resource_get_user_data(res);
    if (!s) return;
    /* Same as set_maximized: config placeholder, real size comes from SURFACE-triggered resize */
    uint32_t states[] = { XDG_TOPLEVEL_STATE_FULLSCREEN };
    send_toplevel_configure(s, g_srv.init_conf_w, g_srv.init_conf_h, states, 1);
}
static void toplevel_unset_fullscreen(struct wl_client* c, struct wl_resource* res) {
    struct awl_surface* s = wl_resource_get_user_data(res);
    if (s) send_toplevel_configure(s, 0, 0, NULL, 0);
}
static void toplevel_set_minimized(struct wl_client* c, struct wl_resource* res) {
    /* Window minimize on the Android side is user-driven, ignore */
}

static void toplevel_res_destroy(struct wl_resource* res) {
    struct awl_surface* s = wl_resource_get_user_data(res);
    if (s) {
        pthread_mutex_lock(&s->ev_lock);   /* binder thread reads fields concurrently */
        s->xdg_role_res = NULL;
        s->role = AWL_ROLE_NONE;
        pthread_mutex_unlock(&s->ev_lock);
    }
}

static const struct xdg_toplevel_interface toplevel_iface = {
    .destroy = toplevel_destroy,
    .set_parent = toplevel_set_parent,
    .set_title = toplevel_set_title,
    .set_app_id = toplevel_set_app_id,
    .show_window_menu = toplevel_show_window_menu,
    .move = toplevel_move,
    .resize = toplevel_resize,
    .set_max_size = toplevel_set_max_size,
    .set_min_size = toplevel_set_min_size,
    .set_maximized = toplevel_set_maximized,
    .unset_maximized = toplevel_unset_maximized,
    .set_fullscreen = toplevel_set_fullscreen,
    .unset_fullscreen = toplevel_unset_fullscreen,
    .set_minimized = toplevel_set_minimized,
};

/* ---------------- xdg_popup (composited as a layer of the parent window —
 * same pipeline as subsurface: render layer snapshot / input hit / frame_done
 * / deferred release all reused; no separate Android window. 2026-09-09 fix,
 * verified with three-dot menu popping over the whole window.) ---------------- */

static void popup_reposition(struct wl_client* c, struct wl_resource* res,
                             struct wl_resource* positioner, uint32_t token);

static void popup_destroy(struct wl_client* c, struct wl_resource* res) {
    wl_resource_destroy(res);
}
static void popup_grab(struct wl_client* c, struct wl_resource* res,
                       struct wl_resource* seat, uint32_t serial) {
    /* grab means map semantics (KWin): dismissal check in awl_popup_input_grab */
    LOGI("popup grab serial=%u", serial);
}
/* Menu close (xdg_popup destroy): detach layer, un-composite + redraw root
 * window. If the surface died first this handler leaves the tree alone
 * (surface_destroy_impl already detached it). */
static void popup_res_destroy(struct wl_resource* res) {
    struct awl_surface* s = wl_resource_get_user_data(res);
    if (!s) return;
    uint64_t root_id = 0;
    int dirty = 0;
    pthread_rwlock_wrlock(&g_srv.rwl);
    if (s->sub_parent) {
        struct awl_surface* root = awl_subsurface_root(s);
        root_id = root->id;
        dirty = root->mapped;
        wl_list_remove(&s->sub_link);
        s->sub_parent = NULL;
    }
    pthread_mutex_lock(&s->ev_lock);
    s->xdg_role_res = NULL;
    s->role = AWL_ROLE_NONE;
    pthread_mutex_unlock(&s->ev_lock);
    pthread_rwlock_unlock(&g_srv.rwl);
    if (dirty && g_srv.cbs.window_dirty)
        g_srv.cbs.window_dirty(g_srv.cbs.user, root_id);
}

static const struct xdg_popup_interface popup_iface = {
    .destroy = popup_destroy,
    .grab = popup_grab,
    .reposition = popup_reposition,
};

/* ---------------- xdg_positioner (records parameters, simple anchor math) ---------------- */

struct awl_positioner {
    int32_t size_w, size_h;
    int32_t anchor_x, anchor_y, anchor_w, anchor_h;
    uint32_t anchor, gravity, constraint;
    int32_t off_x, off_y;
};

static void pos_destroy(struct wl_client* c, struct wl_resource* res) {
    wl_resource_destroy(res);
}
static void pos_set_size(struct wl_client* c, struct wl_resource* res,
                         int32_t w, int32_t h) {
    struct awl_positioner* p = wl_resource_get_user_data(res);
    if (p) { p->size_w = w; p->size_h = h; }
}
static void pos_set_anchor_rect(struct wl_client* c, struct wl_resource* res,
                                int32_t x, int32_t y, int32_t w, int32_t h) {
    struct awl_positioner* p = wl_resource_get_user_data(res);
    if (p) { p->anchor_x = x; p->anchor_y = y; p->anchor_w = w; p->anchor_h = h; }
}
static void pos_set_anchor(struct wl_client* c, struct wl_resource* res, uint32_t anchor) {
    struct awl_positioner* p = wl_resource_get_user_data(res);
    if (p) p->anchor = anchor;
}
static void pos_set_gravity(struct wl_client* c, struct wl_resource* res, uint32_t gravity) {
    struct awl_positioner* p = wl_resource_get_user_data(res);
    if (p) p->gravity = gravity;
}
static void pos_set_constraint_adjustment(struct wl_client* c, struct wl_resource* res,
                                          uint32_t adjustment) {
    struct awl_positioner* p = wl_resource_get_user_data(res);
    if (p) p->constraint = adjustment;
}
static void pos_set_offset(struct wl_client* c, struct wl_resource* res,
                           int32_t x, int32_t y) {
    struct awl_positioner* p = wl_resource_get_user_data(res);
    if (p) { p->off_x = x; p->off_y = y; }
}

static const struct xdg_positioner_interface positioner_iface = {
    .destroy = pos_destroy,
    .set_size = pos_set_size,
    .set_anchor_rect = pos_set_anchor_rect,
    .set_anchor = pos_set_anchor,
    .set_gravity = pos_set_gravity,
    .set_constraint_adjustment = pos_set_constraint_adjustment,
    .set_offset = pos_set_offset,
};

static void positioner_res_destroy(struct wl_resource* res) {
    free(wl_resource_get_user_data(res));
}

/* xdg_positioner anchor math (aligned with KWin xdgshell.cpp popupOffset).
 * anchor/gravity are enum values, not bitmasks (top_left=5, bottom_right=8
 * ... combined values cannot be ANDed) — resolved by full-value switch per
 * axis:
 *   anchor point = edge/corner of anchor_rect per anchor; axis-irrelevant
 *   enums (none/top/bottom on the x axis) take the midpoint;
 *   gravity decides the growth direction from the anchor (LEFT = right edge
 *   on the anchor growing left ...), axis-irrelevant enums center (-size/2).
 * The return value is in parent window geometry space (protocol: anchor rect
 * and popup configure x/y are both relative to the parent geometry origin).
 * Out-of-window clamping in popup_constrain (slide in preserving length;
 * flip/resize deferred). */
static void positioner_place(const struct awl_positioner* p,
                             int32_t* x, int32_t* y) {
    int32_t ax = p->anchor_x + p->anchor_w / 2;
    switch (p->anchor) {
    case XDG_POSITIONER_ANCHOR_LEFT:
    case XDG_POSITIONER_ANCHOR_TOP_LEFT:
    case XDG_POSITIONER_ANCHOR_BOTTOM_LEFT:
        ax = p->anchor_x; break;
    case XDG_POSITIONER_ANCHOR_RIGHT:
    case XDG_POSITIONER_ANCHOR_TOP_RIGHT:
    case XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT:
        ax = p->anchor_x + p->anchor_w; break;
    default: break;   /* none/top/bottom: midpoint */
    }
    int32_t ay = p->anchor_y + p->anchor_h / 2;
    switch (p->anchor) {
    case XDG_POSITIONER_ANCHOR_TOP:
    case XDG_POSITIONER_ANCHOR_TOP_LEFT:
    case XDG_POSITIONER_ANCHOR_TOP_RIGHT:
        ay = p->anchor_y; break;
    case XDG_POSITIONER_ANCHOR_BOTTOM:
    case XDG_POSITIONER_ANCHOR_BOTTOM_LEFT:
    case XDG_POSITIONER_ANCHOR_BOTTOM_RIGHT:
        ay = p->anchor_y + p->anchor_h; break;
    default: break;
    }
    int32_t gx = -(p->size_w + 1) / 2;
    switch (p->gravity) {
    case XDG_POSITIONER_GRAVITY_LEFT:
    case XDG_POSITIONER_GRAVITY_TOP_LEFT:
    case XDG_POSITIONER_GRAVITY_BOTTOM_LEFT:
        gx = -p->size_w; break;
    case XDG_POSITIONER_GRAVITY_RIGHT:
    case XDG_POSITIONER_GRAVITY_TOP_RIGHT:
    case XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT:
        gx = 0; break;
    default: break;   /* none/top/bottom: centered */
    }
    int32_t gy = -(p->size_h + 1) / 2;
    switch (p->gravity) {
    case XDG_POSITIONER_GRAVITY_TOP:
    case XDG_POSITIONER_GRAVITY_TOP_LEFT:
    case XDG_POSITIONER_GRAVITY_TOP_RIGHT:
        gy = -p->size_h; break;
    case XDG_POSITIONER_GRAVITY_BOTTOM:
    case XDG_POSITIONER_GRAVITY_BOTTOM_LEFT:
    case XDG_POSITIONER_GRAVITY_BOTTOM_RIGHT:
        gy = 0; break;
    default: break;
    }
    *x = ax + gx + p->off_x;
    *y = ay + gy + p->off_y;
}

/* Out-of-bounds clamp (2026-09-10): a popup past the root window's (= Android
 * window) content area slides back preserving length — if x2>maxx then
 * len=x2-x1, x2=maxx, x1=x2-len; left/top overflow pinned to 0.
 * The view logical rectangle converts on the same basis as rendering
 * (layers_collect layer position − root content origin):
 *   V = L(parent) + protocol x/y + parent geom − root geom, visible area
 *   [0,gw]×[0,gh].
 * (Protocol x is relative to the parent geometry origin; sub_x is buffer
 * semantics → L(parent) already carries the −parent geom term, add it back.
 * positioner size = window geometry, logical px.)
 * After clamping, solve back for protocol x/y — configure echoes the actual
 * landing spot (required by protocol). */
static void popup_constrain(struct awl_surface* parent,
                            const struct awl_positioner* p,
                            int32_t* x, int32_t* y) {
    pthread_rwlock_rdlock(&g_srv.rwl);
    struct awl_surface* root = awl_subsurface_root(parent);
    pthread_mutex_lock(&root->ev_lock);
    float gw = 0, gh = 0;
    awl_surface_content_size(root, &gw, &gh);
    int32_t gox = root->geom_valid ? root->geom_x : 0;
    int32_t goy = root->geom_valid ? root->geom_y : 0;
    pthread_mutex_unlock(&root->ev_lock);
    if (gw < 1 || gh < 1 || p->size_w <= 0 || p->size_h <= 0) {
        pthread_rwlock_unlock(&g_srv.rwl);
        return;   /* root not committed (size unknown): no constraint */
    }
    /* L(parent) = Σ over the chain of (sub_x − geom) (same accumulation as layers_collect, per-node lock) */
    float pvx = -(float)gox, pvy = -(float)goy;
    for (struct awl_surface* a = parent; a->sub_parent; a = a->sub_parent) {
        pthread_mutex_lock(&a->ev_lock);
        float sx = (float)a->sub_x, sy = (float)a->sub_y;
        if (a->geom_valid) { sx -= (float)a->geom_x; sy -= (float)a->geom_y; }
        pthread_mutex_unlock(&a->ev_lock);
        pvx += sx;
        pvy += sy;
    }
    pthread_rwlock_unlock(&g_srv.rwl);
    pthread_mutex_lock(&parent->ev_lock);
    if (parent->geom_valid) { pvx += (float)parent->geom_x;
                              pvy += (float)parent->geom_y; }
    pthread_mutex_unlock(&parent->ev_lock);

    float w = (float)p->size_w, h = (float)p->size_h;
    float vx = pvx + (float)*x, vy = pvy + (float)*y;
    if (vx + w > gw) vx = gw - w;   /* right/bottom overflow: shift left preserving length (len unchanged) */
    if (vy + h > gh) vy = gh - h;
    if (vx < 0) vx = 0;             /* left/top overflow: pin to 0 */
    if (vy < 0) vy = 0;
    *x = (int32_t)lroundf(vx - pvx);
    *y = (int32_t)lroundf(vy - pvy);
}

/* v3: re-place an already mapped popup (menu size change / chrome submenu
 * cascade). Same anchor math as get_popup + parent geometry conversion;
 * replies repositioned(token) + configure. */
static void popup_reposition(struct wl_client* c, struct wl_resource* res,
                             struct wl_resource* positioner, uint32_t token) {
    struct awl_surface* s = wl_resource_get_user_data(res);
    struct awl_positioner* p =
            positioner ? wl_resource_get_user_data(positioner) : NULL;
    if (!s || s->role != AWL_ROLE_POPUP || !p || !s->sub_parent) return;
    if (wl_resource_get_version(res) < 3) return;

    int32_t x = 0, y = 0;
    positioner_place(p, &x, &y);
    popup_constrain(s->sub_parent, p, &x, &y);
    pthread_rwlock_rdlock(&g_srv.rwl);
    struct awl_surface* root = awl_subsurface_root(s);
    uint64_t root_id = root->id;
    int root_mapped = root->mapped;
    pthread_mutex_lock(&s->sub_parent->ev_lock);
    int32_t pgx = s->sub_parent->geom_valid ? s->sub_parent->geom_x : 0;
    int32_t pgy = s->sub_parent->geom_valid ? s->sub_parent->geom_y : 0;
    pthread_mutex_unlock(&s->sub_parent->ev_lock);
    pthread_rwlock_unlock(&g_srv.rwl);

    pthread_mutex_lock(&s->ev_lock);
    s->sub_x = x + pgx;
    s->sub_y = y + pgy;
    s->popup_x = x;
    s->popup_y = y;
    xdg_popup_send_repositioned(res, token);
    xdg_popup_send_configure(res, x, y, p->size_w, p->size_h);
    s->configure_serial = wl_display_next_serial(g_srv.display);
    xdg_surface_send_configure(s->xdg_surface_res, s->configure_serial);
    pthread_mutex_unlock(&s->ev_lock);
    if (root_mapped && g_srv.cbs.window_dirty)
        g_srv.cbs.window_dirty(g_srv.cbs.user, root_id);
    LOGI("popup %llu reposition -> %d,%d", (unsigned long long)s->id, x, y);
}

/* ---------------- xdg_surface ---------------- */

static void xdg_surface_destroy(struct wl_client* c, struct wl_resource* res) {
    wl_resource_destroy(res);
}

static void xdg_surface_get_toplevel(struct wl_client* c,
                                     struct wl_resource* res, uint32_t id) {
    struct awl_surface* s = wl_resource_get_user_data(res);
    if (!s) return;
    if (s->role != AWL_ROLE_NONE) {
        wl_resource_post_error(res, XDG_SURFACE_ERROR_ALREADY_CONSTRUCTED,
                               "surface already has a role");
        return;
    }
    struct wl_resource* t = wl_resource_create(
            c, &xdg_toplevel_interface, wl_resource_get_version(res), id);
    if (!t) { wl_resource_post_no_memory(res); return; }
    wl_resource_set_implementation(t, &toplevel_iface, s, toplevel_res_destroy);

    pthread_mutex_lock(&s->ev_lock);
    s->role = AWL_ROLE_TOPLEVEL;
    s->xdg_role_res = t;
    s->xdg_surface_res = res;
    s->title[0] = '\0';

    /* Initial configure: daemon-config placeholder init_w/init_h (#33, not the
     * display's physical size — in external-screen scenarios like Samsung DeX
     * the Activity is unrelated to the physical screen size, physical values
     * would be wrong). Once the Activity surface is ready, surfaceChanged →
     * awl_window_resize forces a configure with the exact window size. */
    send_configure_locked(s, g_srv.init_conf_w, g_srv.init_conf_h, NULL, 0);
    pthread_mutex_unlock(&s->ev_lock);
}

static void xdg_surface_get_popup(struct wl_client* c, struct wl_resource* res,
                                  uint32_t id, struct wl_resource* parent_res,
                                  struct wl_resource* pos_res) {
    struct awl_surface* s = wl_resource_get_user_data(res);
    struct awl_positioner* p = pos_res ? wl_resource_get_user_data(pos_res) : NULL;
    if (!s) return;
    if (s->role != AWL_ROLE_NONE) {
        wl_resource_post_error(res, XDG_SURFACE_ERROR_ALREADY_CONSTRUCTED,
                               "surface already has a role");
        return;
    }
    struct awl_surface* parent = parent_res ? wl_resource_get_user_data(parent_res)
                                            : NULL;
    if (!parent || parent == s) {
        wl_resource_post_error(res, XDG_WM_BASE_ERROR_INVALID_POPUP_PARENT,
                               "invalid popup parent");
        return;
    }
    struct wl_resource* pr = wl_resource_create(
            c, &xdg_popup_interface, wl_resource_get_version(res), id);
    if (!pr) { wl_resource_post_no_memory(res); return; }
    wl_resource_set_implementation(pr, &popup_iface, s, popup_res_destroy);

    /* Anchor math → (x,y) in parent window geometry space. */
    int32_t x = 0, y = 0;
    int32_t sw = p ? p->size_w : 0, sh = p ? p->size_h : 0;
    if (p) positioner_place(p, &x, &y);
    if (p) popup_constrain(parent, p, &x, &y);

    /* Attach to the parent layer tree (top of stack, above all siblings):
     * rendering/input hit/frame use the same pipeline as subsurface.
     * sub_x/sub_y = popup window origin (geometry semantics) relative to the
     * parent surface buffer origin — the anchor/configure coordinate space is
     * based on the parent geometry origin (protocol), so the conversion adds
     * the parent geometry offset (chrome's 16/10 shadow margins); the
     * configure echo keeps the protocol-space values. After
     * set_window_geometry the layer snapshot derives the buffer position. */
    pthread_rwlock_wrlock(&g_srv.rwl);
    s->role = AWL_ROLE_POPUP;
    s->xdg_role_res = pr;
    s->xdg_surface_res = res;
    s->sub_parent = parent;
    wl_list_insert(parent->sub_children.prev, &s->sub_link);
    pthread_rwlock_unlock(&g_srv.rwl);

    pthread_mutex_lock(&parent->ev_lock);
    int32_t pgx = parent->geom_valid ? parent->geom_x : 0;
    int32_t pgy = parent->geom_valid ? parent->geom_y : 0;
    pthread_mutex_unlock(&parent->ev_lock);

    pthread_mutex_lock(&s->ev_lock);
    s->sub_x = x + pgx;
    s->sub_y = y + pgy;
    s->popup_x = x;
    s->popup_y = y;
    s->configure_serial = wl_display_get_serial(g_srv.display);
    s->configured = 1;
    xdg_popup_send_configure(pr, x, y, sw, sh);
    xdg_surface_send_configure(res, s->configure_serial);
    pthread_mutex_unlock(&s->ev_lock);
    LOGI("popup %llu of %llu at %d,%d %dx%d",
            (unsigned long long)s->id, (unsigned long long)parent->id,
            x, y, sw, sh);
}

static void xdg_surface_set_window_geometry(struct wl_client* c,
                                            struct wl_resource* res,
                                            int32_t x, int32_t y,
                                            int32_t w, int32_t h) {
    /* Double-buffered, effective on commit (protocol semantics). Rendering/
     * input align to the geometry origin against the window view: chrome's
     * buffer carries 16/10px shadow margins, geometry marks the content area
     * — ignoring it shifts content down-right and systematically skews input
     * coordinates. w/h<=0 = client falls back to the default geometry (buffer
     * is the window) — keep as-is. */
    struct awl_surface* s = wl_resource_get_user_data(res);
    if (!s) return;
    if (w <= 0 || h <= 0) return;
    pthread_mutex_lock(&s->ev_lock);
    s->pend_gx = x; s->pend_gy = y;
    s->pend_gw = w; s->pend_gh = h;
    s->pend_geom = 1;
    pthread_mutex_unlock(&s->ev_lock);
}

static void xdg_surface_ack_configure(struct wl_client* c,
                                      struct wl_resource* res, uint32_t serial) {
    LOGD("ack_configure serial=%u", serial);
    struct awl_surface* s = wl_resource_get_user_data(res);
    if (!s) return;
    if (!s->configured) {
        wl_resource_post_error(res, XDG_SURFACE_ERROR_NOT_CONSTRUCTED,
                               "ack before configure");
        return;
    }
    s->acked = 1;
}

static const struct xdg_surface_interface xdg_surface_iface = {
    .destroy = xdg_surface_destroy,
    .get_toplevel = xdg_surface_get_toplevel,
    .get_popup = xdg_surface_get_popup,
    .set_window_geometry = xdg_surface_set_window_geometry,
    .ack_configure = xdg_surface_ack_configure,
};

static void xdg_surface_res_destroy(struct wl_resource* res) {
    struct awl_surface* s = wl_resource_get_user_data(res);
    if (s) {
        pthread_mutex_lock(&s->ev_lock);
        s->xdg_surface_res = NULL;
        pthread_mutex_unlock(&s->ev_lock);
    }
}

/* ---------------- xdg_wm_base ---------------- */

static void wm_base_destroy(struct wl_client* c, struct wl_resource* res) {
    wl_resource_destroy(res);
}

static void wm_base_create_positioner(struct wl_client* c,
                                      struct wl_resource* res, uint32_t id) {
    struct wl_resource* pr = wl_resource_create(
            c, &xdg_positioner_interface, wl_resource_get_version(res), id);
    struct awl_positioner* p = calloc(1, sizeof(*p));
    if (!pr || !p) {
        if (pr) wl_resource_destroy(pr);
        wl_resource_post_no_memory(res);
        return;
    }
    wl_resource_set_implementation(pr, &positioner_iface, p, positioner_res_destroy);
}

static void wm_base_get_xdg_surface(struct wl_client* c,
                                    struct wl_resource* res, uint32_t id,
                                    struct wl_resource* surface_res) {
    struct awl_surface* s = wl_resource_get_user_data(surface_res);
    LOGI("get_xdg_surface: surface_res=%p user_data=%p",
            (void*)surface_res, (void*)s);
    if (!s) {
        wl_resource_post_error(res, XDG_WM_BASE_ERROR_INVALID_POPUP_PARENT,
                               "surface not created by this compositor");
        return;
    }
    struct wl_resource* xres = wl_resource_create(
            c, &xdg_surface_interface, wl_resource_get_version(res), id);
    if (!xres) { wl_resource_post_no_memory(res); return; }
    wl_resource_set_implementation(xres, &xdg_surface_iface, s,
                                   xdg_surface_res_destroy);
    s->xdg_surface_res = xres;
}

static void wm_base_pong(struct wl_client* c, struct wl_resource* res,
                         uint32_t serial) {
    LOGI("pong %u", serial);
}

static const struct xdg_wm_base_interface wm_base_iface = {
    .destroy = wm_base_destroy,
    .create_positioner = wm_base_create_positioner,
    .get_xdg_surface = wm_base_get_xdg_surface,
    .pong = wm_base_pong,
};

static void wm_base_bind(struct wl_client* client, void* data,
                         uint32_t version, uint32_t id) {
    uint32_t v = version < AWL_XDG_VERSION ? version : AWL_XDG_VERSION;
    struct wl_resource* res = wl_resource_create(
            client, &xdg_wm_base_interface, v, id);
    wl_resource_set_implementation(res, &wm_base_iface, NULL, NULL);
}

void awl_xdg_setup(void) {
    g_srv.init_conf_w = 800;   /* #33 defaults; cfg_load_and_apply overrides at startup */
    g_srv.init_conf_h = 600;
    g_srv.g_xdg_wm_base = wl_global_create(g_srv.display,
                                           &xdg_wm_base_interface,
                                           AWL_XDG_VERSION, NULL, wm_base_bind);
}

/* ---------------- initial-configure placeholder size (#33, daemon config) ----------------
 * New windows only: the initial configure + maximized/fullscreen placeholders
 * read these; mapped windows are resized by awl_window_resize (Android owns
 * sizing entirely). Atomics → binder config thread writes, dispatch threads
 * read, same shape as zoom_pct. */
void awl_display_set_init_size(int32_t w, int32_t h) {
    if (w < 100) w = 100;
    if (h < 100) h = 100;
    g_srv.init_conf_w = w;
    g_srv.init_conf_h = h;
}

void awl_display_init_size(int32_t* w, int32_t* h) {
    *w = g_srv.init_conf_w;
    *h = g_srv.init_conf_h;
}

/* ---- Android → logical layer: window commands (sent directly from any
 * thread, rdlock + ev_lock) ----
 * Window size is decided entirely on the Android side (the Activity surface is
 * the single authority); every change forces a configure (#31 zoom: the
 * logical size sent = phys×100/zoom_pct, client renders buffer + viewport dst
 * per fractional_scale). When the client does not respond (fixed-size buffer),
 * the render side scales the display by the window/root logical ratio. */

/* phys → logical at the EFFECTIVE zoom Z = preferred_scale/120 (round to
 * nearest, integer math). The client renders at exactly that quantized Z
 * (kwin fractionalscale_v1: round(z×120)), so the buffer it commits is
 * round(logical×Z) px; dividing by zoom_pct/100 instead (133% vs the
 * client's 160/120) would make that buffer miss phys by a few px and the
 * 1:1 view mapping (awl_surface_view_map) would have to resample it. */
static int32_t phys_to_logical(int32_t v) {
    int64_t pref = awl_zoom_preferred_scale();
    return (int32_t)(((int64_t)v * 120 + pref / 2) / pref);
}

void awl_window_resize(uint64_t id, int32_t w, int32_t h) {
    int hit = 0, changed = 0;
    pthread_rwlock_rdlock(&g_srv.rwl);
    struct awl_surface* s = awl_surface_by_id(id);
    if (s && (s->role == AWL_ROLE_TOPLEVEL || s->role == AWL_ROLE_XWAYLAND)) {
        hit = 1;
        pthread_mutex_lock(&s->ev_lock);
        changed = s->phys_w != w || s->phys_h != h;
        LOGD("window %llu resize %dx%d (was %dx%d conf=%dx%d mapped=%d changed=%d)",
             (unsigned long long)s->id, w, h, s->phys_w, s->phys_h,
             s->conf_w, s->conf_h, s->mapped, changed);
        s->phys_w = w;   /* Android window size (for view→logical conversion / render ratio) */
        s->phys_h = h;
        if (s->role == AWL_ROLE_XWAYLAND) {
            /* X window size is decided on the X side (no configure channel;
             * HMCL etc. cannot resize): does not follow the Android window —
             * rendering stretches the buffer over the whole window, input
             * converts via view_to_surface by the phys/content ratio. Just
             * record the size. */
            LOGD("xwayland window %llu phys=%dx%d", (unsigned long long)s->id,
                    w, h);
        } else {
            int32_t lw = phys_to_logical(w);
            int32_t lh = phys_to_logical(h);
            if (!s->xdg_role_res || !s->mapped) {
                /* Window not ready (first buffer not committed / role not built):
                 * cache it; awl_xdg_flush_pending forces the send after map — the
                 * initial size signal is not lost */
                s->pend_w = w;
                s->pend_h = h;
                s->has_pending = 1;
            } else if (lw != s->conf_w || lh != s->conf_h) {
                s->has_pending = 0;   /* exact value arrived, invalidate the cache */
                send_configure_locked(s, lw, lh, NULL, 0);
                wl_client_flush(wl_resource_get_client(s->xdg_role_res));
            }   /* same size: prevent loops, no resend */
        }
        pthread_mutex_unlock(&s->ev_lock);
    }
    pthread_rwlock_unlock(&g_srv.rwl);
    /* #34: phys moved → the view mapping moved (letterbox offset / stretch
     * ratio) → this root's cached confine rects (view px) are stale. Remap
     * takes rwl itself, so it must run after the release above; skipped on a
     * no-op resize to keep duplicate SURFACE/RESIZE traffic quiet. */
    if (hit && changed) {
        awl_input_constr_remap(id);
        /* the surface resized in place — the renderer's cached ANativeWindow
         * size is stale; drop it (next frame = full-screen flush at the new
         * size, the original resize path) */
        awl_renderer_window_resized(id);
    }
}

/* Android foreground/focus change → xdg_toplevel ACTIVATED state (configure
 * resent). Size unchanged (conf_w/h as-is), only the state array carries
 * ACTIVATED — clients use it to sense foreground/background (e.g. pause
 * animations, restore input focus). */
void awl_window_set_activated(uint64_t id, int activated) {
    pthread_rwlock_rdlock(&g_srv.rwl);
    struct awl_surface* s = awl_surface_by_id(id);
    if (s && s->role == AWL_ROLE_TOPLEVEL) {
        pthread_mutex_lock(&s->ev_lock);
        if (s->xdg_role_res && s->mapped && (int)s->activated != !!activated) {
            s->activated = !!activated;
            uint32_t states[] = { XDG_TOPLEVEL_STATE_ACTIVATED };
            send_configure_locked(s, s->conf_w, s->conf_h, states,
                                  s->activated ? 1u : 0u);
            wl_client_flush(wl_resource_get_client(s->xdg_role_res));
        }
        pthread_mutex_unlock(&s->ev_lock);
    }
    pthread_rwlock_unlock(&g_srv.rwl);
}

/* Called after map (first buffer commit, event thread): re-sends resizes that arrived earlier */
void awl_xdg_flush_pending(uint64_t id) {
    pthread_rwlock_rdlock(&g_srv.rwl);
    struct awl_surface* s = awl_surface_by_id(id);
    if (s && s->role == AWL_ROLE_TOPLEVEL) {
        pthread_mutex_lock(&s->ev_lock);
        if (s->has_pending && s->xdg_role_res && s->mapped) {
            s->has_pending = 0;
            LOGI("surface %llu flush pending configure %dx%d (logical %dx%d)",
                    (unsigned long long)s->id, s->pend_w, s->pend_h,
                    phys_to_logical(s->pend_w), phys_to_logical(s->pend_h));
            send_configure_locked(s, phys_to_logical(s->pend_w),
                                  phys_to_logical(s->pend_h), NULL, 0);
            wl_client_flush(wl_resource_get_client(s->xdg_role_res));
        }
        pthread_mutex_unlock(&s->ev_lock);
    }
    pthread_rwlock_unlock(&g_srv.rwl);
}

/* Ask the client to close the window (xdg_toplevel.close — the client destroys the surface itself for a clean exit) */
void awl_window_close(uint64_t id) {
    pthread_rwlock_rdlock(&g_srv.rwl);
    struct awl_surface* s = awl_surface_by_id(id);
    if (s && s->role == AWL_ROLE_TOPLEVEL && s->xdg_role_res) {
        pthread_mutex_lock(&s->ev_lock);
        if (s->xdg_role_res) {   /* re-check under lock (destroy handler clears it) */
            xdg_toplevel_send_close(s->xdg_role_res);
            wl_client_flush(wl_resource_get_client(s->xdg_role_res));
            LOGI("surface %llu close requested", (unsigned long long)id);
        }
        pthread_mutex_unlock(&s->ev_lock);
    }
    pthread_rwlock_unlock(&g_srv.rwl);
}

/* xdg_popup input grab check (press event; caller holds rwl.rd, hit = the
 * layer hit by this press): if a mapped popup exists in the layer tree and
 * hit is not inside any popup subtree → KWin grab semantics — popup_done
 * bounces the topmost popup and consumes the press (not delivered).
 * Returns 1 = event consumed. */
int awl_popup_input_grab(struct awl_surface* hit) {
    for (struct awl_surface* a = hit; a && a != a->sub_parent; a = a->sub_parent)
        if (a->role == AWL_ROLE_POPUP && a->mapped) return 0;
    struct awl_surface* root = awl_subsurface_root(hit);
    awl_layer_info_t lay[AWL_MAX_LAYERS];
    int n = awl_surface_get_layers(root->id, lay, AWL_MAX_LAYERS);
    struct awl_surface* top = NULL;
    for (int i = 0; i < n; i++) {
        struct awl_surface* s = awl_surface_by_id(lay[i].surface_id);
        if (!s || s->role != AWL_ROLE_POPUP || !s->mapped) continue;
        if (!lay[i].w || !lay[i].h) continue;   /* popup without content does not intercept */
        top = s;   /* last in layer order = topmost */
    }
    if (!top) return 0;
    pthread_mutex_lock(&top->ev_lock);
    if (top->xdg_role_res) {   /* re-check under lock (destroy handler clears it) */
        xdg_popup_send_popup_done(top->xdg_role_res);
        wl_client_flush(wl_resource_get_client(top->xdg_role_res));
        LOGI("popup %llu dismissed (input grab)", (unsigned long long)top->id);
    }
    pthread_mutex_unlock(&top->ev_lock);
    return 1;
}
