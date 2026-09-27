/* awl_host_test.c — host smoke test for the awl logic layer.
 *
 * Spawns the awl Wayland server on a Unix socket, connects a real client
 * (system libwayland-client), binds wl_seat + zwp_tablet_manager_v2 and
 * verifies:
 *   1. server starts and the tablet manager global is announced;
 *   2. get_tablet_seat delivers tablet_added + tool_added;
 *   3. injecting AWL_IN_TABLET_* through awl_input_dispatch reaches the
 *      client as proximity_in / motion / down / up / frame.
 *
 * Uses the awl side purely through its public API (awl.h + the generated
 * tablet protocol headers) — the same surface a compositor backend sees.
 */
#define _GNU_SOURCE
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "awl.h"

#include <wayland-client.h>
#include "tablet-unstable-v2-client-protocol.h"
#include <wayland-client-protocol.h>

#define SOCK "/tmp/awl-host-test.sock"
#define W   1280u
#define H   720u

/* ---- server side ---- */

static int g_server_listen_fd = -1;

static int make_listen_socket(void)
{
    unlink(SOCK);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    strncpy(addr.sun_path, SOCK, sizeof(addr.sun_path) - 1);
    if (bind(fd, (struct sockaddr*)&addr, sizeof(addr)) < 0) return -1;
    if (listen(fd, 4) < 0) return -1;
    return fd;
}

static void cb_win(void* u, uint64_t id, int32_t w, int32_t h,
                   const char* title, int popup) { (void)u; (void)id; (void)w; (void)h; (void)title; (void)popup; }
static void cb_win_destroyed(void* u, uint64_t id) { (void)u; (void)id; }
static void cb_dirty(void* u, uint64_t id) { (void)u; (void)id; }
static void cb_lock(void* u, uint64_t id, int m, int32_t a, int32_t b, int32_t c, int32_t d) { (void)u;(void)id;(void)m;(void)a;(void)b;(void)c;(void)d; }
static void cb_title(void* u, uint64_t id, const char* t) { (void)u;(void)id;(void)t; }
static void cb_ime_show(void* u, uint64_t id, uint32_t h, uint32_t p) { (void)u;(void)id;(void)h;(void)p; }
static void cb_ime_hide(void* u, uint64_t id) { (void)u;(void)id; }
static void cb_ime_state(void* u, uint64_t id, const char* t, int32_t a, int32_t b,
                         uint32_t c, uint32_t d, int32_t e, int32_t f, int32_t g, int32_t h, uint32_t i) { (void)u;(void)id;(void)t;(void)a;(void)b;(void)c;(void)d;(void)e;(void)f;(void)g;(void)h;(void)i; }
static void cb_clip(void* u, uint64_t id, const char* t) { (void)u;(void)id;(void)t; }
static void cb_cur(void* u, uint64_t id, int hidden) { (void)u;(void)id;(void)hidden; }
static void cb_idle_inhibit(void* u, uint64_t id, int on) { (void)u;(void)id;(void)on; }

static awl_window_callbacks_t k_cbs = {
    .window_created  = cb_win,
    .window_destroyed= cb_win_destroyed,
    .window_title    = cb_title,
    .pointer_lock    = cb_lock,
    .window_dirty    = cb_dirty,
    .ime_show        = cb_ime_show,
    .ime_hide        = cb_ime_hide,
    .ime_state       = cb_ime_state,
    .clipboard_text  = cb_clip,
    .pointer_cursor  = cb_cur,
    .idle_inhibit    = cb_idle_inhibit,
};

/* ---- client side ---- */

static struct zwp_tablet_manager_v2* g_mgr;
static struct zwp_tablet_seat_v2*   g_tab_seat;
static struct zwp_tablet_v2*        g_tablet;
static struct zwp_tablet_tool_v2*   g_tool;
static struct zwp_tablet_pad_v2*    g_pad;
static struct zwp_tablet_pad_group_v2* g_group;
static struct zwp_tablet_pad_ring_v2* g_ring;
static struct zwp_tablet_pad_strip_v2* g_strip;
static struct wl_seat*              g_seat;
static struct wl_surface*           g_surf;
static struct wl_compositor*        g_comp;
static int g_tablet_added = 0, g_tool_added = 0, g_pad_added = 0;
static int g_prox_in = 0, g_motion = 0, g_down = 0, g_up = 0, g_frame = 0;
static int g_pad_button = 0, g_ring_angle = 0, g_strip_position = 0, g_mode_switch = 0;

static void tablet_handle_name(void* d, struct zwp_tablet_v2* t, const char* n) { (void)d;(void)t;(void)n; }
static void tablet_handle_id(void* d, struct zwp_tablet_v2* t, uint32_t a, uint32_t b) { (void)d;(void)t;(void)a;(void)b; }
static void tablet_handle_path(void* d, struct zwp_tablet_v2* t, const char* p) { (void)d;(void)t;(void)p; }
static void tablet_handle_done(void* d, struct zwp_tablet_v2* t) { (void)d;(void)t; }
static void tablet_handle_removed(void* d, struct zwp_tablet_v2* t) { (void)d;(void)t; }
static const struct zwp_tablet_v2_listener tablet_listener = {
    .name = tablet_handle_name, .id = tablet_handle_id, .path = tablet_handle_path,
    .done = tablet_handle_done, .removed = tablet_handle_removed,
};

static void tool_handle_type(void* d, struct zwp_tablet_tool_v2* t, uint32_t type) { (void)d;(void)t;(void)type; }
static void tool_handle_serial(void* d, struct zwp_tablet_tool_v2* t, uint32_t a, uint32_t b) { (void)d;(void)t;(void)a;(void)b; }
static void tool_handle_wacom(void* d, struct zwp_tablet_tool_v2* t, uint32_t a, uint32_t b) { (void)d;(void)t;(void)a;(void)b; }
static void tool_handle_cap(void* d, struct zwp_tablet_tool_v2* t, uint32_t c) { (void)d;(void)t;(void)c; }
static void tool_handle_done(void* d, struct zwp_tablet_tool_v2* t) { (void)d;(void)t; }
static void tool_handle_removed(void* d, struct zwp_tablet_tool_v2* t) { (void)d;(void)t; }
static void tool_handle_proximity_in(void* d, struct zwp_tablet_tool_v2* t, uint32_t s,
                                     struct zwp_tablet_v2* tab, struct wl_surface* surf) { (void)d;(void)t;(void)s;(void)tab;(void)surf; g_prox_in++; }
static void tool_handle_proximity_out(void* d, struct zwp_tablet_tool_v2* t) { (void)d;(void)t; }
static void tool_handle_down(void* d, struct zwp_tablet_tool_v2* t, uint32_t s) { (void)d;(void)t;(void)s; g_down++; }
static void tool_handle_up(void* d, struct zwp_tablet_tool_v2* t) { (void)d;(void)t; g_up++; }
static void tool_handle_motion(void* d, struct zwp_tablet_tool_v2* t, wl_fixed_t x, wl_fixed_t y) { (void)d;(void)t;(void)x;(void)y; g_motion++; }
static void tool_handle_pressure(void* d, struct zwp_tablet_tool_v2* t, uint32_t p) { (void)d;(void)t;(void)p; }
static void tool_handle_distance(void* d, struct zwp_tablet_tool_v2* t, uint32_t ds) { (void)d;(void)t;(void)ds; }
static void tool_handle_tilt(void* d, struct zwp_tablet_tool_v2* t, wl_fixed_t x, wl_fixed_t y) { (void)d;(void)t;(void)x;(void)y; }
static void tool_handle_rotation(void* d, struct zwp_tablet_tool_v2* t, wl_fixed_t r) { (void)d;(void)t;(void)r; }
static void tool_handle_slider(void* d, struct zwp_tablet_tool_v2* t, int32_t p) { (void)d;(void)t;(void)p; }
static void tool_handle_wheel(void* d, struct zwp_tablet_tool_v2* t, wl_fixed_t a, int32_t c) { (void)d;(void)t;(void)a;(void)c; }
static void tool_handle_button(void* d, struct zwp_tablet_tool_v2* t, uint32_t s, uint32_t b, uint32_t state) { (void)d;(void)t;(void)s;(void)b;(void)state; }
static void tool_handle_frame(void* d, struct zwp_tablet_tool_v2* t, uint32_t time) { (void)d;(void)t;(void)time; g_frame++; }
static const struct zwp_tablet_tool_v2_listener tool_listener = {
    .type = tool_handle_type, .hardware_serial = tool_handle_serial,
    .hardware_id_wacom = tool_handle_wacom, .capability = tool_handle_cap,
    .done = tool_handle_done, .removed = tool_handle_removed,
    .proximity_in = tool_handle_proximity_in, .proximity_out = tool_handle_proximity_out,
    .down = tool_handle_down, .up = tool_handle_up, .motion = tool_handle_motion,
    .pressure = tool_handle_pressure, .distance = tool_handle_distance,
    .tilt = tool_handle_tilt, .rotation = tool_handle_rotation,
    .slider = tool_handle_slider, .wheel = tool_handle_wheel,
    .button = tool_handle_button, .frame = tool_handle_frame,
};

static const struct zwp_tablet_pad_group_v2_listener group_listener;
static const struct zwp_tablet_pad_ring_v2_listener ring_listener;
static const struct zwp_tablet_pad_strip_v2_listener strip_listener;
static void pad_handle_group(void* d, struct zwp_tablet_pad_v2* p,
                             struct zwp_tablet_pad_group_v2* g) {
    (void)d; (void)p; g_group = g;
    zwp_tablet_pad_group_v2_add_listener(g, &group_listener, NULL);
}
static void pad_handle_path(void* d, struct zwp_tablet_pad_v2* p, const char* path) { (void)d; (void)p; (void)path; }
static void pad_handle_buttons(void* d, struct zwp_tablet_pad_v2* p, uint32_t buttons) { (void)d; (void)p; (void)buttons; }
static void pad_handle_done(void* d, struct zwp_tablet_pad_v2* p) { (void)d; (void)p; }
static void pad_handle_button(void* d, struct zwp_tablet_pad_v2* p, uint32_t t, uint32_t b, uint32_t s) { (void)d; (void)p; (void)t; (void)b; (void)s; g_pad_button++; }
static void pad_handle_enter(void* d, struct zwp_tablet_pad_v2* p, uint32_t s, struct zwp_tablet_v2* t, struct wl_surface* surf) { (void)d; (void)p; (void)s; (void)t; (void)surf; }
static void pad_handle_leave(void* d, struct zwp_tablet_pad_v2* p, uint32_t s, struct wl_surface* surf) { (void)d; (void)p; (void)s; (void)surf; }
static void pad_handle_removed(void* d, struct zwp_tablet_pad_v2* p) { (void)d; (void)p; }
static const struct zwp_tablet_pad_v2_listener pad_listener = {
    .group = pad_handle_group, .path = pad_handle_path, .buttons = pad_handle_buttons,
    .done = pad_handle_done, .button = pad_handle_button, .enter = pad_handle_enter,
    .leave = pad_handle_leave, .removed = pad_handle_removed,
};
static void group_handle_buttons(void* d, struct zwp_tablet_pad_group_v2* g, struct wl_array* b) { (void)d; (void)g; (void)b; }
static void group_handle_ring(void* d, struct zwp_tablet_pad_group_v2* g, struct zwp_tablet_pad_ring_v2* r) {
    (void)d; (void)g; g_ring = r;
    zwp_tablet_pad_ring_v2_add_listener(r, &ring_listener, NULL);
}
static void group_handle_strip(void* d, struct zwp_tablet_pad_group_v2* g, struct zwp_tablet_pad_strip_v2* s) {
    (void)d; (void)g; g_strip = s;
    zwp_tablet_pad_strip_v2_add_listener(s, &strip_listener, NULL);
}
static void group_handle_modes(void* d, struct zwp_tablet_pad_group_v2* g, uint32_t m) { (void)d; (void)g; (void)m; }
static void group_handle_done(void* d, struct zwp_tablet_pad_group_v2* g) { (void)d; (void)g; }
static void group_handle_mode_switch(void* d, struct zwp_tablet_pad_group_v2* g, uint32_t t, uint32_t s, uint32_t m) { (void)d; (void)g; (void)t; (void)s; (void)m; g_mode_switch++; }
static const struct zwp_tablet_pad_group_v2_listener group_listener = {
    .buttons = group_handle_buttons, .ring = group_handle_ring, .strip = group_handle_strip,
    .modes = group_handle_modes, .done = group_handle_done, .mode_switch = group_handle_mode_switch,
};
static void ring_handle_source(void* d, struct zwp_tablet_pad_ring_v2* r, uint32_t s) { (void)d; (void)r; (void)s; }
static void ring_handle_angle(void* d, struct zwp_tablet_pad_ring_v2* r, wl_fixed_t a) { (void)d; (void)r; (void)a; g_ring_angle++; }
static void ring_handle_stop(void* d, struct zwp_tablet_pad_ring_v2* r) { (void)d; (void)r; }
static void ring_handle_frame(void* d, struct zwp_tablet_pad_ring_v2* r, uint32_t t) { (void)d; (void)r; (void)t; }
static const struct zwp_tablet_pad_ring_v2_listener ring_listener = {
    .source = ring_handle_source, .angle = ring_handle_angle,
    .stop = ring_handle_stop, .frame = ring_handle_frame,
};
static void strip_handle_source(void* d, struct zwp_tablet_pad_strip_v2* s, uint32_t src) { (void)d; (void)s; (void)src; }
static void strip_handle_position(void* d, struct zwp_tablet_pad_strip_v2* s, uint32_t p) { (void)d; (void)s; (void)p; g_strip_position++; }
static void strip_handle_stop(void* d, struct zwp_tablet_pad_strip_v2* s) { (void)d; (void)s; }
static void strip_handle_frame(void* d, struct zwp_tablet_pad_strip_v2* s, uint32_t t) { (void)d; (void)s; (void)t; }
static const struct zwp_tablet_pad_strip_v2_listener strip_listener = {
    .source = strip_handle_source, .position = strip_handle_position,
    .stop = strip_handle_stop, .frame = strip_handle_frame,
};
static void seat_seat_tablet_added(void* d, struct zwp_tablet_seat_v2* s, struct zwp_tablet_v2* t) { (void)d;(void)s; g_tablet = t; g_tablet_added++; zwp_tablet_v2_add_listener(t, &tablet_listener, NULL); }
static void seat_seat_tool_added(void* d, struct zwp_tablet_seat_v2* s, struct zwp_tablet_tool_v2* t) { (void)d;(void)s; g_tool = t; g_tool_added++; zwp_tablet_tool_v2_add_listener(t, &tool_listener, NULL); }
static void seat_seat_pad_added(void* d, struct zwp_tablet_seat_v2* s, struct zwp_tablet_pad_v2* p) {
    (void)d; (void)s; g_pad = p; g_pad_added++;
    zwp_tablet_pad_v2_add_listener(p, &pad_listener, NULL);
}
static const struct zwp_tablet_seat_v2_listener seat_listener = {
    .tablet_added = seat_seat_tablet_added,
    .tool_added = seat_seat_tool_added,
    .pad_added = seat_seat_pad_added,
};

static void reg_global(void* d, struct wl_registry* r, uint32_t name,
                       const char* iface, uint32_t ver)
{
    (void)d;
    if (!strcmp(iface, zwp_tablet_manager_v2_interface.name)) {
        g_mgr = wl_registry_bind(r, name, &zwp_tablet_manager_v2_interface, ver);
    } else if (!strcmp(iface, wl_seat_interface.name)) {
        g_seat = wl_registry_bind(r, name, &wl_seat_interface, ver);
    } else if (!strcmp(iface, wl_compositor_interface.name)) {
        g_comp = wl_registry_bind(r, name, &wl_compositor_interface, ver);
    }
}
static void reg_global_remove(void* d, struct wl_registry* r, uint32_t name) { (void)d;(void)r;(void)name; }
static const struct wl_registry_listener reg_listener = {
    .global = reg_global, .global_remove = reg_global_remove,
};

static int inject_and_dispatch(void)
{
    /* A window id must exist for the translation to resolve. The smoke test
     * uses the server's own surfaces list directly — id 1 with an activity
     * target is expected on real Android. Here we just verify the client-side
     * events arrive; coordinate resolution tolerates a missing surface. */
    awl_input_ev_t ev;

    memset(&ev, 0, sizeof(ev));
    ev.id = 1; ev.type = AWL_IN_TABLET_PROX_IN; ev.x = 640; ev.y = 360; ev.v1 = 0.5f;
    awl_input_dispatch(&ev);

    memset(&ev, 0, sizeof(ev));
    ev.id = 1; ev.type = AWL_IN_TABLET_MOTION; ev.x = 700; ev.y = 400; ev.v1 = 0.8f;
    awl_input_dispatch(&ev);

    memset(&ev, 0, sizeof(ev));
    ev.id = 1; ev.type = AWL_IN_TABLET_DOWN;
    awl_input_dispatch(&ev);

    memset(&ev, 0, sizeof(ev));
    ev.id = 1; ev.type = AWL_IN_TABLET_UP;
    awl_input_dispatch(&ev);

    memset(&ev, 0, sizeof(ev));
    ev.id = 1; ev.type = AWL_IN_TABLET_PROX_OUT;
    awl_input_dispatch(&ev);

    memset(&ev, 0, sizeof(ev));
    ev.id = 1; ev.type = AWL_IN_TABLET_PAD_BUTTON; ev.code = 2; ev.v1 = 1;
    awl_input_dispatch(&ev);
    memset(&ev, 0, sizeof(ev));
    ev.id = 1; ev.type = AWL_IN_TABLET_PAD_RING; ev.v1 = 90.0f;
    awl_input_dispatch(&ev);
    memset(&ev, 0, sizeof(ev));
    ev.id = 1; ev.type = AWL_IN_TABLET_PAD_STRIP; ev.v1 = 32768.0f;
    awl_input_dispatch(&ev);
    memset(&ev, 0, sizeof(ev));
    ev.id = 1; ev.type = AWL_IN_TABLET_PAD_MODE; ev.v1 = 0.0f;
    awl_input_dispatch(&ev);

    return 0;
}

int main(void)
{
    g_server_listen_fd = make_listen_socket();
    if (g_server_listen_fd < 0) { fprintf(stderr, "listen socket failed\n"); return 1; }

    awl_display_info_t info = { .width = W, .height = H, .refresh_hz = 60, .dpi = 420, .scale = 1 };
    if (awl_server_start(g_server_listen_fd, &info, &k_cbs) != 0) {
        fprintf(stderr, "awl_server_start failed\n");
        return 2;
    }

    struct wl_display* disp = wl_display_connect(SOCK);
    if (!disp) { fprintf(stderr, "wl_display_connect failed\n"); return 3; }

    struct wl_registry* reg = wl_display_get_registry(disp);
    wl_registry_add_listener(reg, &reg_listener, NULL);
    wl_display_roundtrip(disp);   /* discover globals */

    if (!g_mgr) {
        fprintf(stderr, "FAIL: zwp_tablet_manager_v2 global not advertised\n");
        return 4;
    }
    printf("OK: tablet manager global present\n");

    /* The dispatch path resolves the tool seat through the owning surface
     * (awl_surface_by_id → client → seat). A real wl_surface must exist
     * server-side, otherwise AWL_IN_TABLET_* events are dropped. */
    if (!g_comp) {
        fprintf(stderr, "FAIL: wl_compositor global not advertised\n");
        return 4;
    }
    g_surf = wl_compositor_create_surface(g_comp);
    if (!g_surf) {
        fprintf(stderr, "FAIL: wl_compositor_create_surface\n");
        return 4;
    }
    wl_display_roundtrip(disp);   /* register surface server-side */
    printf("OK: wl_surface created (server-side id registered)\n");

    g_tab_seat = zwp_tablet_manager_v2_get_tablet_seat(g_mgr, g_seat);
    zwp_tablet_seat_v2_add_listener(g_tab_seat, &seat_listener, NULL);
    wl_display_roundtrip(disp);   /* tablet_added + tool_added + pad_added */
    wl_display_roundtrip(disp);   /* pad/group/ring/strip description burst */

    if (!g_tablet_added || !g_tool_added || !g_pad_added || !g_tablet || !g_tool || !g_pad || !g_group || !g_ring || !g_strip) {
        fprintf(stderr, "FAIL: tablet=%d tool=%d pad=%d objects=%p/%p/%p/%p\n",
                g_tablet_added, g_tool_added, g_pad_added,
                (void*)g_pad, (void*)g_group, (void*)g_ring, (void*)g_strip);
        return 5;
    }
    printf("OK: tablet + tool + pad/group/ring/strip delivered\n");

    inject_and_dispatch();
    wl_display_roundtrip(disp);

    printf("client saw: prox_in=%d motion=%d down=%d up=%d frame=%d pad_button=%d ring_angle=%d strip_position=%d mode_switch=%d\n",
           g_prox_in, g_motion, g_down, g_up, g_frame,
           g_pad_button, g_ring_angle, g_strip_position, g_mode_switch);
    if (g_prox_in != 1 || g_motion < 2 || g_down != 1 || g_up != 1 ||
        g_pad_button != 1 || g_ring_angle != 1 || g_strip_position != 1 ||
        g_mode_switch != 1) {
        fprintf(stderr, "FAIL: tablet/pad event counters do not match\n");
        return 6;
    }

    zwp_tablet_seat_v2_destroy(g_tab_seat);
    zwp_tablet_manager_v2_destroy(g_mgr);
    wl_display_disconnect(disp);
    awl_server_stop();
    unlink(SOCK);
    printf("PASS\n");
    return 0;
}