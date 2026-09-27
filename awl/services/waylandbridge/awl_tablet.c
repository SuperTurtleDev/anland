/* awl_tablet.c — zwp_tablet_v2 (tablet-unstable-v2) server implementation
 *
 * Android stylus/pad input arrives through the existing per-event literal
 * translation model (Activity → binder → awl_input_dispatch → this file),
 * exactly like pointer/keyboard/touch:
 *
 *   AWL_IN_TABLET_PROX_IN   → proximity_in + motion + frame
 *   AWL_IN_TABLET_PROX_OUT  → proximity_out            + frame
 *   AWL_IN_TABLET_MOTION    → motion                   + frame  (x,y + v1=pressure)
 *   AWL_IN_TABLET_DOWN      → down                     + frame
 *   AWL_IN_TABLET_UP        → up                       + frame
 *   AWL_IN_TABLET_BUTTON    → button                   + frame  (code=BTN_*, v1=state)
 *
 * Topology (mirrors awl_input.c):
 *   g_tab_seats = list of per-client zwp_tablet_seat_v2 resources, each of
 *   which lazily owns one zwp_tablet_v2 + one zwp_tablet_tool_v2 resource
 *   (awl exposes a single virtual tablet + pen-shaped tool). List topology is
 *   guarded by g_srv.rwl; event delivery to a client takes that client's
 *   surface s->ev_lock first (same order as input: rwl → ev_lock).
 *
 * This is the protocol-completing side of the "awl 协议完善" milestone: the
 * XML was already in protocols/, the input event enum already in awl.h —
 * only the server-side state machine was missing.
 */
#include "awl_internal.h"

#include <stdlib.h>
#include <string.h>

#include "tablet-unstable-v2-server-protocol.h"

/* The one virtual tablet this compositor owns (sent once per seat bind). */
#define AWL_TABLET_NAME  "anland virtual tablet"
#define AWL_TABLET_PATH  "anland:tablet:0"
#define AWL_TABLET_VID   0x0000
#define AWL_TABLET_PID   0x0000

#define AWL_TOOL_SERIAL_HI 0x00000000u
#define AWL_TOOL_SERIAL_LO 0x52414e44u   /* "ANLD" — stable virtual serial */
#define AWL_TOOL_WACOM_HI  0x00000000u
#define AWL_TOOL_WACOM_LO  0x00000000u

/* ---- per-client seat object ---- */

struct awl_tab_seat {
    struct wl_resource* res;      /* zwp_tablet_seat_v2 */
    struct wl_resource* tablet;   /* zwp_tablet_v2 (one per client) */
    struct wl_resource* tool;     /* zwp_tablet_tool_v2 (one per client) */
    struct wl_resource* pad;      /* zwp_tablet_pad_v2 (one per client) */
    struct wl_resource* group;    /* one pad group */
    struct wl_resource* ring;     /* one ring */
    struct wl_resource* strip;    /* one strip */
    struct wl_list link;          /* g_tab_seats */
};

static struct wl_list g_tab_seats;

/* ---- object teardown ---- */

static void tab_seat_destroy(struct wl_resource* res)
{
    pthread_rwlock_wrlock(&g_srv.rwl);
    struct awl_tab_seat* it;
    struct awl_tab_seat* tmp;
    wl_list_for_each_safe(it, tmp, &g_tab_seats, link) {
        if (it->res == res) {
            wl_list_remove(&it->link);
            free(it);
            break;
        }
    }
    pthread_rwlock_unlock(&g_srv.rwl);
}

static void seat_req_destroy(struct wl_client* c, struct wl_resource* res)
{
    (void)c;
    wl_resource_destroy(res);
}

static const struct zwp_tablet_seat_v2_interface tab_seat_iface = {
    .destroy = seat_req_destroy,
};

static void tablet_req_destroy(struct wl_client* c, struct wl_resource* res)
{
    (void)c;
    wl_resource_destroy(res);
}

static const struct zwp_tablet_v2_interface tab_iface = {
    .destroy = tablet_req_destroy,
};

static void tool_req_destroy(struct wl_client* c, struct wl_resource* res)
{
    (void)c;
    wl_resource_destroy(res);
}

static const struct zwp_tablet_tool_v2_interface tool_iface = {
    .set_cursor = NULL,   /* tool cursors: not composited yet */
    .destroy    = tool_req_destroy,
};

static void pad_feedback(struct wl_client* c, struct wl_resource* res,
                         uint32_t button, const char* description, uint32_t serial) {
    (void)c; (void)res; (void)button; (void)description; (void)serial;
}
static void axis_feedback(struct wl_client* c, struct wl_resource* res,
                          const char* description, uint32_t serial) {
    (void)c; (void)res; (void)description; (void)serial;
}
static const struct zwp_tablet_pad_v2_interface pad_iface = {
    .set_feedback = pad_feedback,
    .destroy = tablet_req_destroy,
};
static const struct zwp_tablet_pad_group_v2_interface group_iface = {
    .destroy = tablet_req_destroy,
};
static const struct zwp_tablet_pad_ring_v2_interface ring_iface = {
    .set_feedback = axis_feedback,
    .destroy = tablet_req_destroy,
};
static const struct zwp_tablet_pad_strip_v2_interface strip_iface = {
    .set_feedback = axis_feedback,
    .destroy = tablet_req_destroy,
};

/* resource-level destroy funcs (wl_resource_set_implementation dtor) */
static void tablet_res_destroy(struct wl_resource* res) { (void)res; }
static void tool_res_destroy(struct wl_resource* res) { (void)res; }
static void pad_res_destroy(struct wl_resource* res) { (void)res; }
static void group_res_destroy(struct wl_resource* res) { (void)res; }
static void ring_res_destroy(struct wl_resource* res) { (void)res; }
static void strip_res_destroy(struct wl_resource* res) { (void)res; }

static void tab_seat_res_destroy(struct wl_resource* res)
{
    tab_seat_destroy(res);
}

/* Create the tablet + tool resources owned by a seat (idempotent per seat). */
static int tab_seat_ensure_objects(struct awl_tab_seat* s)
{
    struct wl_client* client = wl_resource_get_client(s->res);

    if (!s->tablet) {
        s->tablet = wl_resource_create(client, &zwp_tablet_v2_interface,
                                       1, 0);
        if (!s->tablet)
            return -1;
        wl_resource_set_implementation(s->tablet, &tab_iface, NULL,
                                       tablet_res_destroy);
        zwp_tablet_v2_send_name(s->tablet, AWL_TABLET_NAME);
        zwp_tablet_v2_send_id(s->tablet, AWL_TABLET_VID, AWL_TABLET_PID);
        zwp_tablet_v2_send_path(s->tablet, AWL_TABLET_PATH);
        zwp_tablet_v2_send_done(s->tablet);
    }
    if (!s->tool) {
        s->tool = wl_resource_create(client, &zwp_tablet_tool_v2_interface,
                                     1, 0);
        if (!s->tool)
            return -1;
        wl_resource_set_implementation(s->tool, &tool_iface, NULL,
                                       tool_res_destroy);
        zwp_tablet_tool_v2_send_type(s->tool, ZWP_TABLET_TOOL_V2_TYPE_PEN);
        zwp_tablet_tool_v2_send_hardware_serial(
                s->tool, AWL_TOOL_SERIAL_HI, AWL_TOOL_SERIAL_LO);
        zwp_tablet_tool_v2_send_hardware_id_wacom(
                s->tool, AWL_TOOL_WACOM_HI, AWL_TOOL_WACOM_LO);
        zwp_tablet_tool_v2_send_capability(
                s->tool, ZWP_TABLET_TOOL_V2_CAPABILITY_PRESSURE);
        zwp_tablet_tool_v2_send_done(s->tool);
    }
    if (!s->pad) {
        s->pad = wl_resource_create(client, &zwp_tablet_pad_v2_interface, 1, 0);
        s->group = wl_resource_create(client, &zwp_tablet_pad_group_v2_interface, 1, 0);
        s->ring = wl_resource_create(client, &zwp_tablet_pad_ring_v2_interface, 1, 0);
        s->strip = wl_resource_create(client, &zwp_tablet_pad_strip_v2_interface, 1, 0);
        if (!s->pad || !s->group || !s->ring || !s->strip)
            return -1;
        wl_resource_set_implementation(s->pad, &pad_iface, NULL, pad_res_destroy);
        wl_resource_set_implementation(s->group, &group_iface, NULL, group_res_destroy);
        wl_resource_set_implementation(s->ring, &ring_iface, NULL, ring_res_destroy);
        wl_resource_set_implementation(s->strip, &strip_iface, NULL, strip_res_destroy);
    }
    return 0;
}

/* The pad description burst must follow pad_added.  Keeping it separate from
 * object allocation is important: clients install the pad listener from their
 * pad_added callback, then discover the group/ring/strip objects in order. */
static void tab_pad_announce(struct awl_tab_seat* s)
{
    zwp_tablet_pad_v2_send_path(s->pad, "anland:tablet-pad:0");
    zwp_tablet_pad_v2_send_buttons(s->pad, 8);
    zwp_tablet_pad_v2_send_group(s->pad, s->group);
    zwp_tablet_pad_v2_send_done(s->pad);

    struct wl_array buttons;
    wl_array_init(&buttons);
    for (uint32_t i = 0; i < 8; i++) {
        uint32_t* b = wl_array_add(&buttons, sizeof(*b));
        if (b) *b = i;
    }
    zwp_tablet_pad_group_v2_send_buttons(s->group, &buttons);
    wl_array_release(&buttons);
    zwp_tablet_pad_group_v2_send_ring(s->group, s->ring);
    zwp_tablet_pad_group_v2_send_strip(s->group, s->strip);
    /* One mode means no mode-switch capability event is required. */
    zwp_tablet_pad_group_v2_send_done(s->group);
}

/* manager.get_tablet_seat → create the per-client seat resource and announce
 * the virtual tablet/tool. */
static void mgr_get_tablet_seat(struct wl_client* c, struct wl_resource* res,
                                uint32_t id, struct wl_resource* seat)
{
    (void)seat;
    struct awl_tab_seat* ts = calloc(1, sizeof(*ts));
    if (!ts) {
        wl_resource_post_no_memory(res);
        return;
    }
    ts->res = wl_resource_create(c, &zwp_tablet_seat_v2_interface, 1, id);
    if (!ts->res) {
        free(ts);
        wl_resource_post_no_memory(res);
        return;
    }
    wl_resource_set_implementation(ts->res, &tab_seat_iface, ts,
                                   tab_seat_res_destroy);

    pthread_rwlock_wrlock(&g_srv.rwl);
    wl_list_insert(g_tab_seats.prev, &ts->link);
    if (tab_seat_ensure_objects(ts) < 0) {
        wl_list_remove(&ts->link);
        pthread_rwlock_unlock(&g_srv.rwl);
        wl_resource_destroy(ts->res);
        return;
    }
    pthread_rwlock_unlock(&g_srv.rwl);

    zwp_tablet_seat_v2_send_tablet_added(ts->res, ts->tablet);
    zwp_tablet_seat_v2_send_tool_added(ts->res, ts->tool);
    zwp_tablet_seat_v2_send_pad_added(ts->res, ts->pad);
    tab_pad_announce(ts);
    wl_client_flush(c);
}

static void mgr_destroy(struct wl_client* c, struct wl_resource* res)
{
    (void)c;
    wl_resource_destroy(res);
}

static const struct zwp_tablet_manager_v2_interface mgr_iface = {
    .get_tablet_seat = mgr_get_tablet_seat,
    .destroy         = mgr_destroy,
};

static void mgr_bind(struct wl_client* client, void* data,
                     uint32_t version, uint32_t id)
{
    (void)data;
    struct wl_resource* res = wl_resource_create(
            client, &zwp_tablet_manager_v2_interface, version < 1 ? version : 1, id);
    if (!res) {
        wl_client_post_no_memory(client);
        return;
    }
    wl_resource_set_implementation(res, &mgr_iface, NULL, NULL);
}

void awl_tablet_setup(void)
{
    wl_list_init(&g_tab_seats);
    if (!wl_global_create(g_srv.display,
                          &zwp_tablet_manager_v2_interface,
                          1, NULL, mgr_bind))
        LOGE("zwp_tablet_manager_v2 global create failed");
}

/* ---- Android → protocol translation (called from awl_input_dispatch) ----
 * Event carries the target window id; the tool object lives on that window's
 * client. Locks: rwl(rd) to resolve seat → s->ev_lock to send.
 */

/* Window view → surface-local logical coords (same math as
 * awl_input.c view_to_surface; kept local to keep the interface thin). */
static void tab_view_to_surface(struct awl_surface* s, float* x, float* y)
{
    float lw = 0, lh = 0;
    awl_surface_content_size(s, &lw, &lh);
    float sx, sy, ox, oy;
    awl_view_map(g_srv.scale_mode,
                 (float)s->phys_w, (float)s->phys_h, lw, lh,
                 &sx, &sy, &ox, &oy);
    *x = (*x - ox) / sx;
    *y = (*y - oy) / sy;
    if (s->geom_valid) {
        *x += (float)s->geom_x;
        *y += (float)s->geom_y;
    }
}

/* Resolve the tablet-seat object of the client that owns `win`, and the
 * surface, under rwl.rd. Returns the seat (tool/tablet inside) or NULL. */
static struct awl_tab_seat* resolve_seat(uint64_t win,
                                         struct awl_surface** out_s)
{
    struct awl_surface* s = awl_surface_by_id(win);
    *out_s = s;
    if (!s || !s->resource)
        return NULL;
    struct wl_client* c = wl_resource_get_client(s->resource);
    struct awl_tab_seat* it;
    wl_list_for_each(it, &g_tab_seats, link)
        if (wl_resource_get_client(it->res) == c)
            return it;
    return NULL;
}

void awl_tablet_dispatch(const awl_input_ev_t* ev)
{
    if (ev->type >= AWL_IN_TABLET_PAD_BUTTON && ev->type <= AWL_IN_TABLET_PAD_MODE) {
        pthread_rwlock_rdlock(&g_srv.rwl);
        struct awl_surface* s = NULL;
        struct awl_tab_seat* ts = resolve_seat(ev->id, &s);
        if (ts && ts->pad && s) {
            uint32_t now = awl_now_ms();
            if (ev->type == AWL_IN_TABLET_PAD_BUTTON)
                zwp_tablet_pad_v2_send_button(ts->pad, now, ev->code,
                    ev->v1 ? ZWP_TABLET_PAD_V2_BUTTON_STATE_PRESSED : ZWP_TABLET_PAD_V2_BUTTON_STATE_RELEASED);
            else if (ev->type == AWL_IN_TABLET_PAD_RING) {
                zwp_tablet_pad_ring_v2_send_source(ts->ring, ZWP_TABLET_PAD_RING_V2_SOURCE_FINGER);
                zwp_tablet_pad_ring_v2_send_angle(ts->ring, wl_fixed_from_double(ev->v1));
                zwp_tablet_pad_ring_v2_send_frame(ts->ring, now);
            } else if (ev->type == AWL_IN_TABLET_PAD_STRIP) {
                zwp_tablet_pad_strip_v2_send_source(ts->strip, ZWP_TABLET_PAD_STRIP_V2_SOURCE_FINGER);
                uint32_t pos = ev->v1 < 0 ? 0 : ev->v1 > 65535 ? 65535 : (uint32_t)ev->v1;
                zwp_tablet_pad_strip_v2_send_position(ts->strip, pos);
                zwp_tablet_pad_strip_v2_send_frame(ts->strip, now);
            } else {
                uint32_t serial = wl_display_next_serial(g_srv.display);
                zwp_tablet_pad_group_v2_send_mode_switch(ts->group, now, serial, (uint32_t)ev->v1);
            }
            wl_client_flush(wl_resource_get_client(ts->pad));
        }
        pthread_rwlock_unlock(&g_srv.rwl);
        return;
    }
    pthread_rwlock_rdlock(&g_srv.rwl);
    struct awl_surface* s = NULL;
    struct awl_tab_seat* ts = resolve_seat(ev->id, &s);
    if (!ts || !ts->tool || !s) {
        pthread_rwlock_unlock(&g_srv.rwl);
        return;
    }

    struct wl_resource* tool = ts->tool;
    struct wl_resource* tablet = ts->tablet;
    struct wl_resource* surf = s->resource;
    pthread_mutex_lock(&s->ev_lock);

    switch (ev->type) {
    case AWL_IN_TABLET_PROX_IN: {
        float x = ev->x, y = ev->y;
        tab_view_to_surface(s, &x, &y);
        zwp_tablet_tool_v2_send_proximity_in(
                tool, wl_display_next_serial(g_srv.display), tablet, surf);
        if (ev->v1 > 0.f)
            zwp_tablet_tool_v2_send_pressure(
                    tool, (uint32_t)(ev->v1 * 65535.f));
        zwp_tablet_tool_v2_send_motion(
                tool, wl_fixed_from_double(x), wl_fixed_from_double(y));
        zwp_tablet_tool_v2_send_frame(tool, awl_now_ms());
        break;
    }
    case AWL_IN_TABLET_PROX_OUT:
        zwp_tablet_tool_v2_send_proximity_out(tool);
        zwp_tablet_tool_v2_send_frame(tool, awl_now_ms());
        break;
    case AWL_IN_TABLET_MOTION: {
        float x = ev->x, y = ev->y;
        tab_view_to_surface(s, &x, &y);
        zwp_tablet_tool_v2_send_motion(
                tool, wl_fixed_from_double(x), wl_fixed_from_double(y));
        if (ev->v1 > 0.f)
            zwp_tablet_tool_v2_send_pressure(
                    tool, (uint32_t)(ev->v1 * 65535.f));
        /* v2 = tilt-x (degrees, +right); -y tilt not carried by awl_input_ev */
        if (ev->v2 != 0.f)
            zwp_tablet_tool_v2_send_tilt(
                    tool, wl_fixed_from_double(ev->v2), 0);
        zwp_tablet_tool_v2_send_frame(tool, awl_now_ms());
        break;
    }
    case AWL_IN_TABLET_DOWN:
        zwp_tablet_tool_v2_send_down(
                tool, wl_display_next_serial(g_srv.display));
        zwp_tablet_tool_v2_send_frame(tool, awl_now_ms());
        break;
    case AWL_IN_TABLET_UP:
        zwp_tablet_tool_v2_send_up(tool);
        zwp_tablet_tool_v2_send_frame(tool, awl_now_ms());
        break;
    case AWL_IN_TABLET_BUTTON:
        zwp_tablet_tool_v2_send_button(
                tool, wl_display_next_serial(g_srv.display),
                (uint32_t)ev->code, (uint32_t)ev->v1 ? 1 : 0);
        zwp_tablet_tool_v2_send_frame(tool, awl_now_ms());
        break;
    default:
        break;
    }

    wl_client_flush(wl_resource_get_client(tool));
    pthread_mutex_unlock(&s->ev_lock);
    pthread_rwlock_unlock(&g_srv.rwl);
}