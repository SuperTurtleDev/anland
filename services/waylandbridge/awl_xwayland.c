/* awl_xwayland.c — xwayland_shell_v1 (Xwayland rootless window association, #32)
 *
 * Xwayland 24.1 rootless does not go through xdg_shell (xdg_surface creation is
 * only the rootful path, hw/xwayland/xwayland-window.c xwl_create_root_surface):
 *   - A WM client must exist on the X side and call XCompositeRedirectWindow(Manual)
 *     on top-level X windows; only then does ensure_surface_for_window create the
 *     wl_surface at realize/set_window_pixmap time (redirectDraw != Manual returns
 *     NULL directly; MapWindow does not check realize failure — the window is
 *     viewable but never surfaces);
 *   - The surfaced wl_surface takes xwayland_surface_v1 as its role:
 *     get_xwayland_surface then set_serial (the association serial carrying the
 *     same value as the X-side WL_SURFACE_SERIAL ClientMessage), effective with an
 *     immediate empty commit; content buffers commit normally afterwards.
 *
 * awl semantics: xwayland_surface_v1 is the toplevel role — no configure/ack state
 * machine (the X window size is decided X-side; awl sends no configure); the first
 * buffer commit means map → window_created; window size = buffer size (X pixels,
 * scale 1). O-R popups (menus/tooltips) get no WM MapRequest → not redirected →
 * never surface (follow-up task).
 */
#include "awl_internal.h"

#include <string.h>
#include "xwayland-shell-v1-server-protocol.h"

/* ---------------- xwayland_surface_v1 ---------------- */

static void xsurf_destroy(struct wl_client* client, struct wl_resource* res) {
    wl_resource_destroy(res);
}

/* set_serial: association state is double-buffered, effective on commit; this
 * implementation has no per-commit state, so just record it. serial = the same
 * 64-bit value as the X-side WL_SURFACE_SERIAL ClientMessage (l[0]=lo, l[1]=hi)
 * — mini-wm pairs the X window by it; the adapt layer sends resize/close through
 * its control channel. Once per surface. */
static void xsurf_set_serial(struct wl_client* client, struct wl_resource* res,
                             uint32_t lo, uint32_t hi) {
    struct awl_surface* s = wl_resource_get_user_data(res);
    if (!s) return;
    pthread_mutex_lock(&s->ev_lock);
    s->xwayland_serial = ((uint64_t)hi << 32) | lo;
    pthread_mutex_unlock(&s->ev_lock);
    LOGI("xwayland surface %llu associated (serial=%llu)",
            (unsigned long long)s->id, (unsigned long long)s->xwayland_serial);
    snprintf(s->title, sizeof(s->title), "Xwayland");
}

static const struct xwayland_surface_v1_interface xsurf_iface = {
    .destroy = xsurf_destroy,
    .set_serial = xsurf_set_serial,
};

/* Resource destruction (client destroy / disconnect): drop the surface back
 * reference. Protocol semantics: destroy does not undo an already-effective
 * association (the role stays until the surface dies). */
static void xsurf_res_destroy(struct wl_resource* res) {
    struct awl_surface* s = wl_resource_get_user_data(res);
    if (s && s->xwayland_res == res)
        s->xwayland_res = NULL;
}

/* ---------------- xwayland_shell_v1 ---------------- */

static void shell_destroy(struct wl_client* client, struct wl_resource* res) {
    wl_resource_destroy(res);
}

static void shell_get_xwayland_surface(struct wl_client* client,
                                       struct wl_resource* res, uint32_t id,
                                       struct wl_resource* surface_res) {
    struct awl_surface* s = surface_res ? awl_surface_from_res(surface_res)
                                        : NULL;
    if (!s) return;
    if (s->role != AWL_ROLE_NONE) {
        wl_resource_post_error(res, XWAYLAND_SHELL_V1_ERROR_ROLE,
                               "wl_surface already has role %d", s->role);
        return;
    }
    struct wl_resource* xs = wl_resource_create(
            client, &xwayland_surface_v1_interface, 1, id);
    if (!xs) { wl_resource_post_no_memory(res); return; }
    s->role = AWL_ROLE_XWAYLAND;
    s->xwayland_res = xs;
    wl_resource_set_implementation(xs, &xsurf_iface, s, xsurf_res_destroy);
}

static const struct xwayland_shell_v1_interface shell_iface = {
    .destroy = shell_destroy,
    .get_xwayland_surface = shell_get_xwayland_surface,
};

static void shell_bind(struct wl_client* client, void* data,
                       uint32_t version, uint32_t id) {
    struct wl_resource* res = wl_resource_create(
            client, &xwayland_shell_v1_interface, 1, id);
    if (!res) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(res, &shell_iface, NULL, NULL);
}

void awl_xwayland_setup(void) {
    if (!wl_global_create(g_srv.display, &xwayland_shell_v1_interface,
                          1, NULL, shell_bind))
        LOGE("xwayland_shell_v1 global create failed");
}

/* adapt-layer query: an Xwayland window's association serial (0 = not an Xwayland window / not associated) */
int awl_xwayland_window_serial(uint64_t id, uint64_t* serial) {
    pthread_rwlock_rdlock(&g_srv.rwl);
    struct awl_surface* s = awl_surface_by_id(id);
    int ok = 0;
    if (s && s->role == AWL_ROLE_XWAYLAND) {
        pthread_mutex_lock(&s->ev_lock);
        if (s->xwayland_serial) {
            *serial = s->xwayland_serial;
            ok = 1;
        }
        pthread_mutex_unlock(&s->ev_lock);
    }
    pthread_rwlock_unlock(&g_srv.rwl);
    return ok;
}
