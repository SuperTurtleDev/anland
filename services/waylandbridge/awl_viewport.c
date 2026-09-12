/* awl_viewport.c — wp_viewporter + zwp_fractional_scale_manager_v1 (#31
 * arbitrary-ratio zoom 1.5/1.75/2.5..., applied dynamically)
 *
 * Reference: kwin-6.6.5 src/wayland/{fractionalscale_v1,viewporter}.cpp:
 *   - wl_output.scale is always 1 (fractional ratios not expressible);
 *     clients (chrome/GTK) instead learn the zoom from the preferred_scale
 *     event of zwp_fractional_scale_v1 — the value is in 1/120 units (kwin:
 *     send_preferred_scale(round(z * 120))); the client picks its own integer
 *     buffer scale (usually ceil) and declares the logical size via
 *     wp_viewport set_destination, the buffer is sampled scaled into dst.
 *   - preferred_scale: current value sent on object creation (kwin ctor);
 *     resent on change (kwin setPreferredBufferScale sends only on change).
 *   - viewport state (source/destination) is double-buffered, effective on
 *     the same commit as the buffer (kwin pending→current); -1 resets;
 *     out-of-range values are the protocol error bad_value.
 *   - At most one viewport / one fractional_scale per surface (duplicate =
 *     protocol error).
 *
 * Coordinate model (repo-wide convention, see the surface note in
 * awl_internal.h): layer stack/geometry/popup/input event coordinates are
 * always logical px; the render side scales by the window-physical/
 * root-logical ratio.
 *
 * Zoom change (awl_display_set_zoom):
 *   preferred_scale broadcast to every fractional_scale object + re-configure
 *   every toplevel (phys/zoom) — after the client acks it re-lays out buffer/
 *   viewport at the new ratio, the render side adapts to the actual buffer,
 *   no window restart needed. */
#include "awl_internal.h"

#include <viewporter-server-protocol.h>
#include "fractional-scale-v1-server-protocol.h"

#include <math.h>
#include <string.h>

/* ---------------- logical size (public helpers, awl_internal.h) ---------------- */

/* Current buffer size in pixels (caller holds ev_lock; 0 = no buffer) */
static void vp_buf_size(struct awl_surface* s, uint32_t* w, uint32_t* h) {
    *w = *h = 0;
    if (!s->current_buffer_res) return;
    struct wl_shm_buffer* shm = wl_shm_buffer_get(s->current_buffer_res);
    if (shm) {
        *w = (uint32_t)wl_shm_buffer_get_width(shm);
        *h = (uint32_t)wl_shm_buffer_get_height(shm);
        return;
    }
    struct awl_buffer* b = wl_resource_get_user_data(s->current_buffer_res);
    if (b && b->width) { *w = b->width; *h = b->height; }
}

/* isomorphic to kwin SurfaceInterfacePrivate::applyState surfaceSize */
void awl_surface_logical_size(struct awl_surface* s, float* w, float* h) {
    if (s->vp_dst_w > 0 && s->vp_dst_h > 0) {
        *w = (float)s->vp_dst_w;
        *h = (float)s->vp_dst_h;
        return;
    }
    if (s->vp_has_src) {
        *w = s->vp_sw;
        *h = s->vp_sh;
        return;
    }
    uint32_t bw = 0, bh = 0;
    vp_buf_size(s, &bw, &bh);
    float sc = s->buf_scale > 0 ? (float)s->buf_scale : 1.0f;
    *w = (float)bw / sc;
    *h = (float)bh / sc;
    /* set_buffer_transform 90/270: the logical size swaps while the buffer
     * stays as-is (dst/src branches above are untouched — viewport source is
     * buffer-coordinate space, dst explicitly overrides size, kwin surfaceSize
     * shape). */
    if (s->buf_transform == 1 || s->buf_transform == 3) {
        float t = *w;
        *w = *h;
        *h = t;
    }
}

/* View mapping base size (#31 chrome shadow-margin findings: viewport dst =
 * configure + margins 16/10/16/32, real content = the xdg geometry
 * rectangle). Valid geometry → geometry size; otherwise the surface logical
 * size (for a pure fractional client dst is the content; for a fixed-size
 * client it is the buffer). Shared by render rs / input f / IME cursor ×r —
 * geometry rectangle ↔ window view, margins out of bounds are cropped.
 * Caller holds ev_lock. */
void awl_surface_content_size(struct awl_surface* s, float* w, float* h) {
    if (s->geom_valid && s->geom_w > 0 && s->geom_h > 0) {
        *w = (float)s->geom_w;
        *h = (float)s->geom_h;
        return;
    }
    awl_surface_logical_size(s, w, h);
}

/* ---------------- view mapping ----------------
 * Root content base (logical px) → Android window view px:
 *   view = logical × s + o
 * THE conversion shared by the render dst, input inverse, relative deltas,
 * confine rects and the IME cursor rect — every site reads
 * the same numbers from awl_surface_view_map, so they cannot drift.
 *
 * Two regimes, decided per root by awl_surface_view_map:
 *  1) content follows the configured size (kwin: the scene is drawn at the
 *     output scale, vertices snapped to the pixel grid) → s = Z exactly,
 *     o = 0. The client renders buffer = round(logical × Z) (fractional-
 *     scale-v1: toplevel size rounded half away from zero) with viewport
 *     dst = logical (Z = preferred_scale/120, the same quantized value on
 *     both sides), the consumer snaps the dst rect to integer px
 *     (awl_snap_extent, awl_renderer.hpp) → buffer px land 1:1 on view px.
 *     No stretch, nothing resampled, no lost pixels — whatever scale_mode
 *     says.
 *  2) content does NOT follow the configure (fixed-size client ignoring
 *     resize, X window the X side did not resize, the frames between a
 *     configure and its ack) → daemon scale_mode placement (awl_view_map). */

/* Buffer px per logical px of the root's current buffer, per axis — the
 * client's effective scale (1 for a scale-1 client, Z for a scale-aware one
 * rendering logical×Z into viewport dst = logical). Sampled region = viewport
 * source when set (surface orientation, same convention as
 * awl_surface_layer_uv), else the whole buffer (90/270 transform swaps its
 * axes into surface orientation). 1 when undetermined. Caller holds ev_lock. */
static void vp_buffer_ratio(struct awl_surface* s, double* rx, double* ry) {
    *rx = *ry = 1.0;
    float lw = 0, lh = 0;
    awl_surface_logical_size(s, &lw, &lh);
    if (lw <= 0.5f || lh <= 0.5f) return;
    double bw, bh;
    if (s->vp_has_src) {
        bw = s->vp_sw;
        bh = s->vp_sh;
    } else {
        uint32_t w = 0, h = 0;
        vp_buf_size(s, &w, &h);
        if (!w || !h) return;
        bw = (double)w;
        bh = (double)h;
        if (s->buf_transform == 1 || s->buf_transform == 3) {
            double t = bw;
            bw = bh;
            bh = t;
        }
    }
    *rx = bw / (double)lw;
    *ry = bh / (double)lh;
}

/* scale_mode placement (regime 2 above): pw/ph = window view px, cw/ch =
 * content base (logical), rx/ry = buffer px per logical px (vp_buffer_ratio).
 *   STRETCH  fill each axis independently (legacy)
 *   FIT      uniform scale to fit inside, centered
 *   CENTER   original size: s = rx/ry so 1 buffer px = 1 view px, centered —
 *            the buffer is shown at its own pixel size, never resampled
 *            (s = 1 here was the bug: at zoom≠100% a logical×Z buffer was
 *            squeezed to 1/Z in the middle of the window)
 * Degenerate input = identity (pw/ph ≤ 0: before the first resize; cw/ch ≤
 * 0.5: no buffer/geometry yet) — the inverse (division) never hits s = 0.
 * double throughout (kwin qreal), see awl_view_xform_t. */
void awl_view_map(int mode, double pw, double ph, double cw, double ch,
                  double rx, double ry,
                  double* sx, double* sy, double* ox, double* oy) {
    if (pw <= 0.0 || ph <= 0.0 || cw <= 0.5 || ch <= 0.5 || mode < 0) {
        *sx = *sy = 1.0;
        *ox = *oy = 0.0;
        return;
    }
    switch (mode) {
    case AWL_SCALE_FIT: {
        double kx = pw / cw, ky = ph / ch;
        *sx = *sy = kx < ky ? kx : ky;
        break;
    }
    case AWL_SCALE_CENTER:
        *sx = rx > 0.0 ? rx : 1.0;
        *sy = ry > 0.0 ? ry : 1.0;
        break;
    default:   /* AWL_SCALE_STRETCH (and any unknown value): legacy fill */
        *sx = pw / cw;
        *sy = ph / ch;
        *ox = *oy = 0.0;
        return;
    }
    /* round: an unrounded centering offset leaves a half-sampled edge
     * column/row that shimmers */
    *ox = round((pw - cw * *sx) * 0.5);
    *oy = round((ph - ch * *sy) * 0.5);
}

/* Per-root decision (regime 1 vs 2) — see the section comment. "Follows the
 * configure" = the committed content base (geometry rectangle, else surface
 * logical size) equals the last configure sent (conf_w/h); an X window has
 * no configure and always takes regime 2, where a buffer that matches phys
 * maps 1:1 in every mode anyway. Caller holds root ev_lock. */
void awl_surface_view_map(struct awl_surface* root,
                          double* sx, double* sy, double* ox, double* oy) {
    float cw = 0, ch = 0;
    awl_surface_content_size(root, &cw, &ch);
    if (root->role == AWL_ROLE_TOPLEVEL && root->conf_w > 0 && root->conf_h > 0 &&
        (int32_t)lroundf(cw) == root->conf_w &&
        (int32_t)lroundf(ch) == root->conf_h) {
        *sx = *sy = awl_zoom_scale();
        *ox = *oy = 0.0;
        return;
    }
    double rx, ry;
    vp_buffer_ratio(root, &rx, &ry);
    awl_view_map(g_srv.scale_mode, (double)root->phys_w, (double)root->phys_h,
                 (double)cw, (double)ch, rx, ry, sx, sy, ox, oy);
}

/* Sample region (viewport source; absent = whole buffer) → normalized uv
 * transform for the layer snapshot / cursor layer. The shader uv is already
 * Y-flipped (top-down) and the source rectangle is top-down too, so a plain
 * divide suffices. Caller holds ev_lock. */
void awl_surface_layer_uv(struct awl_surface* s, float* u0, float* v0,
                          float* su, float* sv) {
    *u0 = *v0 = 0.0f;
    *su = *sv = 1.0f;
    if (!s->vp_has_src) return;
    uint32_t bw = 0, bh = 0;
    vp_buf_size(s, &bw, &bh);
    if (!bw || !bh) return;
    *u0 = s->vp_sx / (float)bw;
    *v0 = s->vp_sy / (float)bh;
    *su = s->vp_sw / (float)bw;
    *sv = s->vp_sh / (float)bh;
}

/* ---------------- wp_viewport ---------------- */

static void vp_destroy(struct wl_client* c, struct wl_resource* res) {
    wl_resource_destroy(res);
}

static void vp_set_source(struct wl_client* c, struct wl_resource* res,
                          wl_fixed_t xf, wl_fixed_t yf,
                          wl_fixed_t wf, wl_fixed_t hf) {
    struct awl_surface* surf = wl_resource_get_user_data(res);
    if (!surf) {
        wl_resource_post_error(res, WP_VIEWPORT_ERROR_NO_SURFACE,
                               "the wl_surface for this viewport no longer exists");
        return;
    }
    double x = wl_fixed_to_double(xf), y = wl_fixed_to_double(yf);
    double w = wl_fixed_to_double(wf), h = wl_fixed_to_double(hf);
    pthread_mutex_lock(&surf->ev_lock);
    if (x == -1.0 && y == -1.0 && w == -1.0 && h == -1.0) {
        surf->pend_vps = 1;
        surf->pend_vps_x = surf->pend_vps_y = 0;
        surf->pend_vps_w = surf->pend_vps_h = 0;   /* 0 = reset */
    } else if (x < 0 || y < 0 || w <= 0 || h <= 0) {
        pthread_mutex_unlock(&surf->ev_lock);
        wl_resource_post_error(res, WP_VIEWPORT_ERROR_BAD_VALUE,
                               "invalid source geometry");
        return;
    } else {
        surf->pend_vps = 1;
        surf->pend_vps_x = (float)x;
        surf->pend_vps_y = (float)y;
        surf->pend_vps_w = (float)w;
        surf->pend_vps_h = (float)h;
    }
    pthread_mutex_unlock(&surf->ev_lock);
}

static void vp_set_destination(struct wl_client* c, struct wl_resource* res,
                               int32_t w, int32_t h) {
    struct awl_surface* s = wl_resource_get_user_data(res);
    if (!s) {
        wl_resource_post_error(res, WP_VIEWPORT_ERROR_NO_SURFACE,
                               "the wl_surface for this viewport no longer exists");
        return;
    }
    if (w == -1 && h == -1) {
        pthread_mutex_lock(&s->ev_lock);
        s->pend_vpd = 1;
        s->pend_vpd_w = s->pend_vpd_h = 0;   /* 0 = reset */
        pthread_mutex_unlock(&s->ev_lock);
        return;
    }
    if (w <= 0 || h <= 0) {
        wl_resource_post_error(res, WP_VIEWPORT_ERROR_BAD_VALUE,
                               "invalid destination size");
        return;
    }
    pthread_mutex_lock(&s->ev_lock);
    s->pend_vpd = 1;
    s->pend_vpd_w = w;
    s->pend_vpd_h = h;
    pthread_mutex_unlock(&s->ev_lock);
}

/* viewport resource destroy (client destroy / cleanup when the surface died
 * first): the state reset goes into pending — same as kwin, effective on the
 * next commit. The surface may already be cleared (user_data NULL). */
static void vp_res_destroy(struct wl_resource* res) {
    struct awl_surface* s = wl_resource_get_user_data(res);
    if (!s) return;
    pthread_mutex_lock(&s->ev_lock);
    if (s->viewport_res == res) s->viewport_res = NULL;
    s->pend_vpd = 1;
    s->pend_vpd_w = s->pend_vpd_h = 0;
    s->pend_vps = 1;
    s->pend_vps_x = s->pend_vps_y = s->pend_vps_w = s->pend_vps_h = 0;
    pthread_mutex_unlock(&s->ev_lock);
}

static const struct wp_viewport_interface viewport_iface = {
    .destroy = vp_destroy,
    .set_source = vp_set_source,
    .set_destination = vp_set_destination,
};

static void viewporter_get_viewport(struct wl_client* c, struct wl_resource* res,
                                    uint32_t id, struct wl_resource* surface_res) {
    struct awl_surface* s = surface_res ? wl_resource_get_user_data(surface_res) : NULL;
    if (!s) return;
    pthread_mutex_lock(&s->ev_lock);
    if (s->viewport_res) {
        pthread_mutex_unlock(&s->ev_lock);
        wl_resource_post_error(res, WP_VIEWPORTER_ERROR_VIEWPORT_EXISTS,
                               "the specified surface already has a viewport");
        return;
    }
    pthread_mutex_unlock(&s->ev_lock);
    struct wl_resource* vres = wl_resource_create(
            c, &wp_viewport_interface, wl_resource_get_version(res), id);
    if (!vres) { wl_resource_post_no_memory(res); return; }
    wl_resource_set_implementation(vres, &viewport_iface, s, vp_res_destroy);
    pthread_mutex_lock(&s->ev_lock);
    s->viewport_res = vres;
    pthread_mutex_unlock(&s->ev_lock);
}

static void viewporter_destroy(struct wl_client* c, struct wl_resource* res) {
    wl_resource_destroy(res);
}

static const struct wp_viewporter_interface viewporter_iface = {
    .destroy = viewporter_destroy,
    .get_viewport = viewporter_get_viewport,
};

/* ---------------- zwp_fractional_scale_v1 ---------------- */

struct awl_frac_scale {
    struct wl_resource* res;        /* zwp_fractional_scale_v1 */
    struct wl_list link;            /* g_srv.frac_scales (rwl topology) */
};

static uint32_t zoom_preferred_scale(void) {
    /* kwin: round(z × 120); integer zoom_pct avoids float drift */
    return (uint32_t)((g_srv.zoom_pct * 120 + 50) / 100);
}

uint32_t awl_zoom_preferred_scale(void) {
    return zoom_preferred_scale();
}

/* Effective display scale Z = preferred_scale/120 — the value the client
 * actually applies (kwin fractionalscale_v1 sends round(z×120); a percent
 * that is not a multiple of 1/120, e.g. 133% → 160/120 = 1.3333, is what the
 * client renders at). Every compositor-side use of Z (configure size, the
 * 1:1 view mapping) takes THIS number, never zoom_pct/100 — otherwise the
 * client's buffer = round(logical×Z_client) and our logical×Z disagree by a
 * few px and the buffer would have to be resampled. */
double awl_zoom_scale(void) {
    return (double)zoom_preferred_scale() / 120.0;
}

static void frac_destroy(struct wl_client* c, struct wl_resource* res) {
    wl_resource_destroy(res);
}

static void frac_res_destroy(struct wl_resource* res) {
    struct awl_frac_scale* fs = wl_resource_get_user_data(res);
    if (!fs) return;
    pthread_rwlock_wrlock(&g_srv.rwl);
    wl_list_remove(&fs->link);
    pthread_rwlock_unlock(&g_srv.rwl);
    free(fs);
}

static const struct wp_fractional_scale_v1_interface frac_iface = {
    .destroy = frac_destroy,
};

static void fsm_get_fractional_scale(struct wl_client* c, struct wl_resource* res,
                                     uint32_t id, struct wl_resource* surface_res) {
    struct awl_surface* s = surface_res ? wl_resource_get_user_data(surface_res) : NULL;
    if (!s) return;
    pthread_mutex_lock(&s->ev_lock);
    if (s->frac_res) {
        pthread_mutex_unlock(&s->ev_lock);
        wl_resource_post_error(res,
                WP_FRACTIONAL_SCALE_MANAGER_V1_ERROR_FRACTIONAL_SCALE_EXISTS,
                "the specified surface already has a fractional scale");
        return;
    }
    pthread_mutex_unlock(&s->ev_lock);
    struct wl_resource* fres = wl_resource_create(
            c, &wp_fractional_scale_v1_interface,
            wl_resource_get_version(res), id);
    if (!fres) { wl_resource_post_no_memory(res); return; }
    struct awl_frac_scale* fs = calloc(1, sizeof(*fs));
    if (!fs) { wl_resource_destroy(fres); wl_resource_post_no_memory(res); return; }
    fs->res = fres;
    pthread_rwlock_wrlock(&g_srv.rwl);
    wl_list_insert(g_srv.frac_scales.prev, &fs->link);
    pthread_rwlock_unlock(&g_srv.rwl);
    wl_resource_set_implementation(fres, &frac_iface, fs, frac_res_destroy);
    pthread_mutex_lock(&s->ev_lock);
    s->frac_res = fres;
    pthread_mutex_unlock(&s->ev_lock);
    /* kwin ctor: send the current preferred scale on creation (client lays out at Z from the first frame) */
    wp_fractional_scale_v1_send_preferred_scale(fres, zoom_preferred_scale());
    wl_client_flush(c);
}

static void fsm_destroy(struct wl_client* c, struct wl_resource* res) {
    wl_resource_destroy(res);
}

static const struct wp_fractional_scale_manager_v1_interface fsm_iface = {
    .destroy = fsm_destroy,
    .get_fractional_scale = fsm_get_fractional_scale,
};

static void viewporter_bind(struct wl_client* client, void* data,
                            uint32_t version, uint32_t id) {
    struct wl_resource* res = wl_resource_create(
            client, &wp_viewporter_interface, 1, id);
    if (!res) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(res, &viewporter_iface, NULL, NULL);
}

static void fsm_bind(struct wl_client* client, void* data,
                     uint32_t version, uint32_t id) {
    struct wl_resource* res = wl_resource_create(
            client, &wp_fractional_scale_manager_v1_interface, 1, id);
    if (!res) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(res, &fsm_iface, NULL, NULL);
}

void awl_viewport_setup(void) {
    wl_list_init(&g_srv.frac_scales);
    g_srv.zoom_pct = 100;
    g_srv.scale_mode = AWL_SCALE_STRETCH;   /* #34 default: legacy fill */
    g_srv.g_viewporter = wl_global_create(
            g_srv.display, &wp_viewporter_interface, 1, NULL, viewporter_bind);
    g_srv.g_frac_scale_mgr = wl_global_create(
            g_srv.display, &wp_fractional_scale_manager_v1_interface,
            1, NULL, fsm_bind);
    if (!g_srv.g_viewporter || !g_srv.g_frac_scale_mgr)
        LOGE("viewporter/fractional-scale global create failed");
}

/* ---------------- zoom change (daemon T_ZOOM → here; any thread) ----------------
 * pct = 100 × Z (integer 50..300). Applied dynamically:
 *   1) broadcast preferred_scale = pct×120/100 to all zwp_fractional_scale_v1
 *      objects;
 *   2) re-configure all toplevels with the remembered Android window size
 *      (phys×100/pct)
 * After receiving, the client (chrome) re-lays out buffer + viewport at the
 * new ratio; newly committed frames follow naturally. */
void awl_display_set_zoom(int pct) {
    if (pct < 50) pct = 50;
    if (pct > 300) pct = 300;
    if (pct == g_srv.zoom_pct) return;
    g_srv.zoom_pct = pct;
    LOGI("zoom → %d%% (preferred_scale=%u, effective Z=%.4f)", pct,
         zoom_preferred_scale(), awl_zoom_scale());

    /* Broadcast preferred_scale (kwin: resend on change; sending under rwl.rd is safe) */
    pthread_rwlock_rdlock(&g_srv.rwl);
    struct awl_frac_scale* fs;
    wl_list_for_each(fs, &g_srv.frac_scales, link) {
        wp_fractional_scale_v1_send_preferred_scale(fs->res, zoom_preferred_scale());
        wl_client_flush(wl_resource_get_client(fs->res));
    }
    /* Collect toplevel window sizes (re-configure after dropping the lock via
     * awl_window_resize — that API takes rwl.rd itself, no nested holding) */
    struct { uint64_t id; int32_t w, h; } wins[64];
    int nw = 0;
    struct awl_surface* s;
    wl_list_for_each(s, &g_srv.surfaces, link) {
        if (s->role == AWL_ROLE_TOPLEVEL && s->phys_w > 0 && nw < 64) {
            wins[nw].id = s->id;
            wins[nw].w = s->phys_w;
            wins[nw].h = s->phys_h;
            nw++;
        }
    }
    pthread_rwlock_unlock(&g_srv.rwl);

    for (int i = 0; i < nw; i++)
        awl_window_resize(wins[i].id, wins[i].w, wins[i].h);
    /* the 1:1 view mapping is Z itself → every cached confine rect (view px)
     * is stale right away, not only after the client acks the new size
     * (awl_window_resize skips the remap: phys did not move) */
    awl_input_constr_remap(0);
}

int awl_display_zoom(void) {
    return g_srv.zoom_pct;
}

/* ---------------- scale mode change (daemon T_CFG_SET → here; any thread) ----
 * #34: pure presentation-layer switch (render dst / input mapping / confine
 * rects / IME cursor rect all re-derive per frame or per event from
 * awl_view_map + this atom). No client re-configure (unlike zoom, the logical
 * size the client sees never changes — a client filling the window aspect
 * keeps rendering 1:1 in every mode). The only state cached across frames is
 * the confine rect in view px → remap + re-push every live constraint after
 * storing the mode. */
void awl_display_set_scale_mode(int mode) {
    if (mode < AWL_SCALE_STRETCH || mode > AWL_SCALE_CENTER) {
        LOGE("scale mode %d invalid (0..2), ignored", mode);
        return;
    }
    if (mode == g_srv.scale_mode) return;
    g_srv.scale_mode = mode;
    LOGI("scale mode → %d (%s)", mode,
         mode == AWL_SCALE_FIT ? "fit" : mode == AWL_SCALE_CENTER ? "center" : "stretch");
    /* view-px confine rects are stale under the new mapping → re-convert +
     * re-push C_CAPTURE (takes rwl itself: call with no logic-layer lock) */
    awl_input_constr_remap(0);
}

int awl_display_scale_mode(void) {
    return g_srv.scale_mode;
}
