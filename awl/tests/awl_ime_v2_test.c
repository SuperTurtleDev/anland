/* awl_ime_v2_test.c - protocol-level smoke test for input-method-v2. */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include "awl.h"
#include <wayland-client.h>
#include "input-method-unstable-v2-client-protocol.h"

#define SOCK "/tmp/awl-ime-v2-test.sock"

static int g_mgr_count;
static struct zwp_input_method_manager_v2 *g_mgr;
static struct wl_seat *g_seat;
static struct zwp_input_method_v2 *g_first;
static struct zwp_input_method_v2 *g_second;
static int g_first_activate;
static int g_first_deactivate;
static int g_first_done;
static int g_second_unavailable;

static int make_socket(void)
{
    unlink(SOCK);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un a = { .sun_family = AF_UNIX };
    strncpy(a.sun_path, SOCK, sizeof(a.sun_path) - 1);
    if (bind(fd, (struct sockaddr *)&a, sizeof(a)) < 0) return -1;
    if (listen(fd, 4) < 0) return -1;
    return fd;
}

static void cb_window(void *u, uint64_t id, int32_t w, int32_t h,
                      const char *title, int popup) { (void)u;(void)id;(void)w;(void)h;(void)title;(void)popup; }
static void cb_destroyed(void *u, uint64_t id) { (void)u;(void)id; }
static void cb_title(void *u, uint64_t id, const char *title) { (void)u;(void)id;(void)title; }
static void cb_lock(void *u, uint64_t id, int mode, int32_t x, int32_t y, int32_t w, int32_t h) { (void)u;(void)id;(void)mode;(void)x;(void)y;(void)w;(void)h; }
static void cb_dirty(void *u, uint64_t id) { (void)u;(void)id; }
static void cb_show(void *u, uint64_t id, uint32_t hint, uint32_t purpose) { (void)u;(void)id;(void)hint;(void)purpose; }
static void cb_hide(void *u, uint64_t id) { (void)u;(void)id; }
static void cb_state(void *u, uint64_t id, const char *text, int32_t c, int32_t a,
                     uint32_t hint, uint32_t purpose, int32_t x, int32_t y,
                     int32_t w, int32_t h, uint32_t flags) { (void)u;(void)id;(void)text;(void)c;(void)a;(void)hint;(void)purpose;(void)x;(void)y;(void)w;(void)h;(void)flags; }
static void cb_clip(void *u, uint64_t id, const char *text) { (void)u;(void)id;(void)text; }
static void cb_cursor(void *u, uint64_t id, int hidden) { (void)u;(void)id;(void)hidden; }
static void cb_idle(void *u, uint64_t id, int on) { (void)u;(void)id;(void)on; }

static const awl_window_callbacks_t cbs = {
    .window_created = cb_window,
    .window_destroyed = cb_destroyed,
    .window_title = cb_title,
    .pointer_lock = cb_lock,
    .window_dirty = cb_dirty,
    .ime_show = cb_show,
    .ime_hide = cb_hide,
    .ime_state = cb_state,
    .clipboard_text = cb_clip,
    .pointer_cursor = cb_cursor,
    .idle_inhibit = cb_idle,
};

static void ime_activate(void *d, struct zwp_input_method_v2 *ime) { (void)d;(void)ime; g_first_activate++; }
static void ime_deactivate(void *d, struct zwp_input_method_v2 *ime) { (void)d;(void)ime; g_first_deactivate++; }
static void ime_surrounding(void *d, struct zwp_input_method_v2 *ime, const char *t, uint32_t c, uint32_t a) { (void)d;(void)ime;(void)t;(void)c;(void)a; }
static void ime_cause(void *d, struct zwp_input_method_v2 *ime, uint32_t cause) { (void)d;(void)ime;(void)cause; }
static void ime_content(void *d, struct zwp_input_method_v2 *ime, uint32_t h, uint32_t p) { (void)d;(void)ime;(void)h;(void)p; }
static void ime_done(void *d, struct zwp_input_method_v2 *ime) { (void)d;(void)ime; g_first_done++; }
static void ime_unavailable(void *d, struct zwp_input_method_v2 *ime) { (void)d;(void)ime; g_second_unavailable++; }
static const struct zwp_input_method_v2_listener ime_listener = {
    .activate = ime_activate,
    .deactivate = ime_deactivate,
    .surrounding_text = ime_surrounding,
    .text_change_cause = ime_cause,
    .content_type = ime_content,
    .done = ime_done,
    .unavailable = ime_unavailable,
};

static void reg_global(void *d, struct wl_registry *r, uint32_t name,
                       const char *iface, uint32_t version)
{
    (void)d;
    if (!strcmp(iface, zwp_input_method_manager_v2_interface.name)) {
        g_mgr = wl_registry_bind(r, name, &zwp_input_method_manager_v2_interface,
                                  version < 1 ? version : 1);
        g_mgr_count++;
    } else if (!strcmp(iface, wl_seat_interface.name)) {
        g_seat = wl_registry_bind(r, name, &wl_seat_interface,
                                  version < 5 ? version : 5);
    }
}
static void reg_remove(void *d, struct wl_registry *r, uint32_t name) { (void)d;(void)r;(void)name; }
static const struct wl_registry_listener registry_listener = {
    .global = reg_global,
    .global_remove = reg_remove,
};

int main(void)
{
    int listen_fd = make_socket();
    if (listen_fd < 0) { perror("socket"); return 1; }
    awl_display_info_t info = { .width = 1280, .height = 720, .refresh_hz = 60, .dpi = 420, .scale = 1 };
    if (awl_server_start(listen_fd, &info, &cbs) != 0) return 2;

    struct wl_display *display = wl_display_connect(SOCK);
    if (!display) { fprintf(stderr, "connect failed\n"); return 3; }
    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);
    if (!g_mgr || g_mgr_count != 1 || !g_seat) { fprintf(stderr, "FAIL: v2 manager or seat missing\n"); return 4; }

    g_first = zwp_input_method_manager_v2_get_input_method(g_mgr, g_seat);
    if (!g_first) { fprintf(stderr, "FAIL: first IME create\n"); return 5; }
    zwp_input_method_v2_add_listener(g_first, &ime_listener, NULL);
    wl_display_roundtrip(display);

    g_second = zwp_input_method_manager_v2_get_input_method(g_mgr, g_seat);
    if (!g_second) { fprintf(stderr, "FAIL: second IME create\n"); return 6; }
    zwp_input_method_v2_add_listener(g_second, &ime_listener, NULL);
    wl_display_roundtrip(display);

    if (!g_second_unavailable) {
        fprintf(stderr, "FAIL: second IME did not receive unavailable\n");
        return 7;
    }
    printf("OK: manager + first IME + second unavailable\n");

    zwp_input_method_v2_destroy(g_second);
    zwp_input_method_v2_destroy(g_first);
    zwp_input_method_manager_v2_destroy(g_mgr);
    wl_display_disconnect(display);
    awl_server_stop();
    unlink(SOCK);
    printf("PASS\n");
    return 0;
}