/* awl_shm_test.c — pure SHM wayland test client (third-party-app scenario)
 *
 * PORTABLE CLIENT ONLY: no binder, no Android glue beyond logging — the
 * wayland connection arrives as a fork-inherited fd named by the
 * WAYLAND_SOCKET environment variable (the libwayland-client convention:
 * wl_display_connect(NULL) consumes it and takes fd ownership). Whoever
 * spawns this process owns the handoff: the libawl test APK does
 * Awl.getWaylandFd() (socketpair + binder) + Awl.spawnClient (fork+exec
 * with the fd inherited). A future glibc/proot build keeps this file
 * unchanged.
 *
 * Binds wl_compositor / wl_shm / xdg_wm_base and runs an animated ARGB8888
 * window (moving color bands + a blinking liveness block), honoring xdg
 * configure/close; two buffers from one memfd pool, redraw driven by
 * wl_surface.frame callbacks. Exit path check: xdg_toplevel.close must end
 * the process cleanly.
 */
#define _GNU_SOURCE   /* memfd_create: __USE_GNU (bionic and glibc alike) */
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"

/* Logging = plain printf to stdout/stderr: the spawning side (libawl's
 * fork+exec helper) wires our stdio to pipes it hands to the app — no
 * Android glue needed here, a future glibc/proot build keeps this file
 * unchanged. (Plain C99 variadic function — no GNU ##__VA_ARGS__ comma
 * elision, strict ISO C builds stay clean.) */
#include <stdarg.h>

static void awl_logline(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fputs("awlshm: ", stdout);
    vfprintf(stdout, fmt, ap);
    fputc('\n', stdout);
    va_end(ap);
}
#define LOG(...) awl_logline(__VA_ARGS__)

#define TEST_W 800
#define TEST_H 600

static struct wl_display* g_disp;
static struct wl_compositor* g_comp;
static struct wl_shm* g_shm;
static struct xdg_wm_base* g_wm;
static struct wl_surface* g_surf;
static struct xdg_surface* g_xsurf;

static int g_configured;            /* first configure seen → buffer commit maps the window */
static int g_done;                  /* close requested / display gone */
static uint64_t g_frames;

static void* g_map;                 /* pool mapping: 2 buffers */
static struct wl_buffer* g_bufs[2];
static int g_cur;                   /* buffer being painted next */
static uint32_t g_paint_ms;


static void frame_done(void* data, struct wl_callback* cb, uint32_t ms);

static void paint(uint32_t* px, int w, int h) {
    /* cycling color test: full-screen red → green → blue, one second each —
     * exercises the whole per-frame path continuously (frame callback →
     * damage → upload → render → screen) and makes any channel swap or
     * stale frame immediately obvious */
    const uint32_t cyc[3] = { 0xffff0000, 0xff00ff00, 0xff0000ff };
    uint32_t c = cyc[(g_paint_ms / 1000) % 3];
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            px[y * w + x] = c;
}

static void redraw(void) {
    int w = TEST_W, h = TEST_H;
    uint32_t* px = (uint32_t*)((char*)g_map + (size_t)g_cur * w * h * 4);
    paint(px, w, h);
    wl_surface_attach(g_surf, g_bufs[g_cur], 0, 0);
    wl_surface_damage_buffer(g_surf, 0, 0, INT32_MAX, INT32_MAX);
    wl_surface_commit(g_surf);
    g_cur ^= 1;
}

/* wl_proxy_add_listener STORES the struct pointer (no copy) — listeners
 * must outlive the call: static storage, never block-scoped compound
 * literals (a dangling one crashed the client via ffi at the next event) */
static const struct wl_callback_listener k_frame_l = { frame_done };

/* frame callback → next frame (self-rearming) */
static void frame_done(void* data, struct wl_callback* cb, uint32_t ms) {
    wl_callback_destroy(cb);
    g_paint_ms = ms;
    if (g_done) return;
    struct wl_callback* next = wl_surface_frame(g_surf);
    wl_callback_add_listener(next, &k_frame_l, NULL);
    redraw();
    g_frames++;
    if (g_frames % 60 == 0)
        LOG("frame #%llu", (unsigned long long)g_frames);
    wl_display_flush(g_disp);
}

static void xdg_configure(void* data, struct xdg_surface* s, uint32_t serial) {
    xdg_surface_ack_configure(s, serial);
    if (!g_configured) {
        g_configured = 1;
        struct wl_callback* cb = wl_surface_frame(g_surf);
        wl_callback_add_listener(cb, &k_frame_l, NULL);
        redraw();   /* first buffer commit = map (the daemon creates the window here) */
        wl_display_flush(g_disp);
        LOG("mapped %dx%d (configure serial %u)", TEST_W, TEST_H, serial);
    }
}

static void toplevel_configure(void* data, struct xdg_toplevel* t,
                               int32_t w, int32_t h, struct wl_array* states) {
    /* sizes are daemon-owned (Android window); this test keeps its fixed
     * buffer — scale_mode letterboxes it */
    if (w || h)
        LOG("toplevel configure %dx%d (fixed %dx%d buffer)", w, h, TEST_W, TEST_H);
}

static void toplevel_close(void* data, struct xdg_toplevel* t) {
    LOG("toplevel close → exiting");
    g_done = 1;
}

static void wm_ping(void* data, struct xdg_wm_base* wm, uint32_t serial) {
    xdg_wm_base_pong(wm, serial);
}

static void registry_global(void* data, struct wl_registry* r, uint32_t name,
                            const char* iface, uint32_t ver) {
    if (strcmp(iface, "wl_compositor") == 0) {
        /* v4: wl_surface.damage_buffer (request 9) needs it — the daemon
         * advertises 4; an older server falls back to the offered version */
        g_comp = wl_registry_bind(r, name, &wl_compositor_interface, ver < 4 ? ver : 4);
    } else if (strcmp(iface, "wl_shm") == 0) {
        g_shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    } else if (strcmp(iface, "xdg_wm_base") == 0) {
        g_wm = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
        static const struct xdg_wm_base_listener wm_l = { wm_ping };
        xdg_wm_base_add_listener(g_wm, &wm_l, NULL);
    }
}

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    const char* env = getenv("WAYLAND_SOCKET");
    LOG("client pid=%d uid=%d WAYLAND_SOCKET=%s",
        getpid(), getuid(), env ? env : "(unset, connecting via WAYLAND_DISPLAY)");

    g_disp = wl_display_connect(NULL);   /* consumes WAYLAND_SOCKET, takes fd ownership */
    if (!g_disp) {
        fprintf(stderr, "awlshm: wl_display_connect failed: %s\n", strerror(errno));
        return 1;
    }
    LOG("display connected");

    static const struct wl_registry_listener reg_l = { registry_global, NULL };
    struct wl_registry* reg = wl_display_get_registry(g_disp);
    wl_registry_add_listener(reg, &reg_l, NULL);
    wl_display_roundtrip(g_disp);
    if (!g_comp || !g_shm || !g_wm) {
        fprintf(stderr, "awlshm: missing globals (compositor=%p shm=%p wm=%p)\n",
                (void*)g_comp, (void*)g_shm, (void*)g_wm);
        return 1;
    }

    /* two ARGB8888 buffers from one memfd pool */
    size_t bufsz = (size_t)TEST_W * TEST_H * 4;
    int fd = memfd_create("awlshm", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, bufsz * 2) != 0) {
        fprintf(stderr, "awlshm: memfd: %s\n", strerror(errno));
        return 1;
    }
    g_map = mmap(NULL, bufsz * 2, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (g_map == MAP_FAILED) {
        fprintf(stderr, "awlshm: mmap: %s\n", strerror(errno));
        return 1;
    }
    struct wl_shm_pool* pool = wl_shm_create_pool(g_shm, fd, bufsz * 2);
    g_bufs[0] = wl_shm_pool_create_buffer(pool, 0, TEST_W, TEST_H,
                                          TEST_W * 4, WL_SHM_FORMAT_ARGB8888);
    g_bufs[1] = wl_shm_pool_create_buffer(pool, bufsz, TEST_W, TEST_H,
                                          TEST_W * 4, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);   /* buffers hold their own references */
    close(fd);

    g_surf = wl_compositor_create_surface(g_comp);
    static const struct xdg_surface_listener xsurf_l = { xdg_configure };
    static const struct xdg_toplevel_listener tlp_l = { toplevel_configure, toplevel_close };
    g_xsurf = xdg_wm_base_get_xdg_surface(g_wm, g_surf);
    xdg_surface_add_listener(g_xsurf, &xsurf_l, NULL);
    struct xdg_toplevel* tlp = xdg_surface_get_toplevel(g_xsurf);
    xdg_toplevel_add_listener(tlp, &tlp_l, NULL);
    xdg_toplevel_set_title(tlp, "awl shm test");
    xdg_toplevel_set_app_id(tlp, "com.anlandtest.shm");

    wl_surface_commit(g_surf);   /* commit the roles → server answers with configure */
    wl_display_flush(g_disp);

    while (!g_done && wl_display_dispatch(g_disp) >= 0) {}
    LOG("exiting after %llu frames", (unsigned long long)g_frames);
    wl_display_disconnect(g_disp);
    return 0;
}
