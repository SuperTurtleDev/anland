/* A real in-process Wayland client verifies native tablet-v2 delivery. No
 * compositor, GPU, root, Android device or external display is required. */
#define _POSIX_C_SOURCE 200809L
#include "tablet.h"
#include "tablet-v2-client-protocol.h"
#include <assert.h>
#include <poll.h>
#include <stdio.h>
#include <sys/socket.h>
#include <wayland-client.h>
#include <wayland-server.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_seat.h>

struct probe {
    struct wl_compositor *compositor;
    struct wl_seat *seat;
    struct zwp_tablet_manager_v2 *manager;
    unsigned tools, pen, eraser, pressure_caps, tilt_caps, distance_caps;
    unsigned in, out, down, up, press, release, frames, removed;
    double x, y, tilt_x, tilt_y;
    uint32_t pressure, distance, button;
};
static void type(void *data, struct zwp_tablet_tool_v2 *t, uint32_t value) {
    (void)t;
    struct probe *p=data;
    p->pen += value == ZWP_TABLET_TOOL_V2_TYPE_PEN;
    p->eraser += value == ZWP_TABLET_TOOL_V2_TYPE_ERASER;
}
static void pair(void *d, struct zwp_tablet_tool_v2 *t, uint32_t a, uint32_t b) { (void)d;(void)t;(void)a;(void)b; }
static void cap(void *data, struct zwp_tablet_tool_v2 *t, uint32_t value) {
    (void)t; struct probe *p=data;
    p->pressure_caps += value == ZWP_TABLET_TOOL_V2_CAPABILITY_PRESSURE;
    p->tilt_caps += value == ZWP_TABLET_TOOL_V2_CAPABILITY_TILT;
    p->distance_caps += value == ZWP_TABLET_TOOL_V2_CAPABILITY_DISTANCE;
}
static void done(void *d, struct zwp_tablet_tool_v2 *t) { (void)d;(void)t; }
static void removed(void *d, struct zwp_tablet_tool_v2 *t) { ((struct probe *)d)->removed++; zwp_tablet_tool_v2_destroy(t); }
static void proximity(void *d, struct zwp_tablet_tool_v2 *t, uint32_t s, struct zwp_tablet_v2 *tab, struct wl_surface *surf) {
    (void)t;(void)s;(void)tab;(void)surf; ((struct probe *)d)->in++;
}
static void out(void *d, struct zwp_tablet_tool_v2 *t) { (void)t; ((struct probe *)d)->out++; }
static void down(void *d, struct zwp_tablet_tool_v2 *t, uint32_t s) { (void)t;(void)s; ((struct probe *)d)->down++; }
static void up(void *d, struct zwp_tablet_tool_v2 *t) { (void)t; ((struct probe *)d)->up++; }
static void motion(void *d, struct zwp_tablet_tool_v2 *t, wl_fixed_t x, wl_fixed_t y) {
    (void)t; struct probe *p=d; p->x=wl_fixed_to_double(x);p->y=wl_fixed_to_double(y);
}
static void pressure(void *d, struct zwp_tablet_tool_v2 *t, uint32_t value) { (void)t; ((struct probe *)d)->pressure=value; }
static void distance(void *d, struct zwp_tablet_tool_v2 *t, uint32_t value) { (void)t; ((struct probe *)d)->distance=value; }
static void tilt(void *d, struct zwp_tablet_tool_v2 *t, wl_fixed_t x, wl_fixed_t y) {
    (void)t; struct probe *p=d;p->tilt_x=wl_fixed_to_double(x);p->tilt_y=wl_fixed_to_double(y);
}
static void button(void *d, struct zwp_tablet_tool_v2 *t, uint32_t s, uint32_t code, uint32_t state) {
    (void)t;(void)s; struct probe *p=d;p->button=code;
    if (state) p->press++; else p->release++;
}
static void frame(void *d, struct zwp_tablet_tool_v2 *t, uint32_t time) { (void)t;(void)time; ((struct probe *)d)->frames++; }
static const struct zwp_tablet_tool_v2_listener tool_listener = {
    .type=type,.hardware_serial=pair,.hardware_id_wacom=pair,.capability=cap,.done=done,.removed=removed,
    .proximity_in=proximity,.proximity_out=out,.down=down,.up=up,.motion=motion,
    .pressure=pressure,.distance=distance,.tilt=tilt,.button=button,.frame=frame,
};
static void tablet_name(void *d, struct zwp_tablet_v2 *t, const char *s) { (void)d;(void)t;(void)s; }
static void tablet_id(void *d, struct zwp_tablet_v2 *t, uint32_t a, uint32_t b) { (void)d;(void)t;(void)a;(void)b; }
static void tablet_done(void *d, struct zwp_tablet_v2 *t) { (void)d;(void)t; }
static void tablet_removed(void *d, struct zwp_tablet_v2 *t) { (void)d;zwp_tablet_v2_destroy(t); }
static const struct zwp_tablet_v2_listener tablet_listener = {
    .name=tablet_name,.id=tablet_id,.path=tablet_name,.done=tablet_done,.removed=tablet_removed,
};
static void tablet_added(void *d, struct zwp_tablet_seat_v2 *s, struct zwp_tablet_v2 *t) {
    (void)s;zwp_tablet_v2_add_listener(t,&tablet_listener,d);
}
static void tool_added(void *d, struct zwp_tablet_seat_v2 *s, struct zwp_tablet_tool_v2 *t) {
    (void)s;((struct probe *)d)->tools++;zwp_tablet_tool_v2_add_listener(t,&tool_listener,d);
}
static const struct zwp_tablet_seat_v2_listener tablet_seat_listener = {.tablet_added=tablet_added,.tool_added=tool_added};
static void global(void *data, struct wl_registry *registry, uint32_t name, const char *interface, uint32_t version) {
    struct probe *p=data;(void)version;
    if (!strcmp(interface,"wl_compositor")) p->compositor=wl_registry_bind(registry,name,&wl_compositor_interface,1);
    if (!strcmp(interface,"wl_seat")) p->seat=wl_registry_bind(registry,name,&wl_seat_interface,1);
    if (!strcmp(interface,"zwp_tablet_manager_v2")) p->manager=wl_registry_bind(registry,name,&zwp_tablet_manager_v2_interface,1);
}
static void global_removed(void *d, struct wl_registry *r, uint32_t n) { (void)d;(void)r;(void)n; }
static const struct wl_registry_listener registry_listener={.global=global,.global_remove=global_removed};
static void pump(struct wl_display *server, struct wl_display *client) {
    for (unsigned i=0;i<8;++i) {
        assert(wl_display_flush(client) >= 0);
        assert(wl_event_loop_dispatch(wl_display_get_event_loop(server),0) >= 0);
        wl_display_flush_clients(server);
        assert(wl_display_dispatch_pending(client) >= 0);
        struct pollfd fd={.fd=wl_display_get_fd(client),.events=POLLIN};
        if (poll(&fd,1,0)>0) assert(wl_display_dispatch(client) >= 0);
    }
}
int main(void) {
    struct wl_display *server=wl_display_create(); assert(server);
    assert(wlr_compositor_create(server,6,NULL));
    struct wlr_seat *seat=wlr_seat_create(server,"test");assert(seat);
    struct anland_gamescope_tablet *tablet=anland_gamescope_tablet_create(server,seat);assert(tablet);
    int sockets[2];assert(socketpair(AF_UNIX,SOCK_STREAM,0,sockets)==0);
    struct wl_client *server_client=wl_client_create(server,sockets[0]);
    if (!server_client) perror("wl_client_create");
    assert(server_client);
    struct wl_display *client=wl_display_connect_to_fd(sockets[1]);assert(client);
    struct probe p={0};
    struct wl_registry *registry=wl_display_get_registry(client);
    wl_registry_add_listener(registry,&registry_listener,&p);
    pump(server,client);assert(p.manager && p.seat && p.compositor);
    struct zwp_tablet_seat_v2 *tablet_seat=zwp_tablet_manager_v2_get_tablet_seat(p.manager,p.seat);
    zwp_tablet_seat_v2_add_listener(tablet_seat,&tablet_seat_listener,&p);
    struct wl_surface *surface=wl_compositor_create_surface(p.compositor);
    pump(server,client);
    struct wlr_surface *target=wlr_surface_from_resource(wl_client_get_object(server_client,wl_proxy_get_id((struct wl_proxy *)surface)));
    assert(target);
    anland_tablet_sample sample={.tool_id=1,.tool_type=TABLET_TOOL_PEN,.device_id=7,
        .capabilities=TABLET_CAP_PRESSURE|TABLET_CAP_TILT|TABLET_CAP_DISTANCE,
        .flags=TABLET_FLAG_PROXIMITY,.distance=.5f,.tilt_x=-30,.tilt_y=15};
    assert(anland_gamescope_tablet_frame(tablet,&sample,target,120,240));pump(server,client);
    assert(p.tools==1 && p.pen==1 && p.in==1 && p.down==0);
    assert(p.pressure_caps==1 && p.tilt_caps==1 && p.distance_caps==1);
    assert(p.x==120 && p.y==240 && p.tilt_x==-30 && p.tilt_y==15 && p.distance==32767);
    sample.flags|=TABLET_FLAG_TIP_DOWN;sample.pressure=.75f;sample.buttons=TABLET_BUTTON_SECONDARY;
    assert(anland_gamescope_tablet_frame(tablet,&sample,target,130,250));pump(server,client);
    assert(p.down==1 && p.press==1 && p.button==0x14c && p.pressure==49151);
    /* A non-tablet target must release the stroke and both button/focus state. */
    assert(anland_gamescope_tablet_frame(tablet,&sample,NULL,0,0));pump(server,client);
    assert(p.up==1 && p.release==1 && p.out==1);
    assert(anland_gamescope_tablet_frame(tablet,&sample,target,130,250));pump(server,client);
    assert(p.in==2 && p.down==2 && p.press==2);
    anland_gamescope_tablet_reset(tablet);pump(server,client);
    assert(p.up==2 && p.release==2 && p.out==2 && p.removed==1);
    sample.tool_id=2;sample.tool_type=TABLET_TOOL_ERASER;sample.buttons=0;
    assert(anland_gamescope_tablet_frame(tablet,&sample,target,15,20));pump(server,client);
    assert(p.eraser==1 && p.tools==2 && p.frames>0);
    anland_gamescope_tablet_destroy(tablet);pump(server,client);
    wl_display_disconnect(client);
    wl_display_destroy_clients(server);wl_display_destroy(server);
    puts("native tablet-v2 client: pen/eraser, axes, focus loss, buttons and disconnect passed");
}
