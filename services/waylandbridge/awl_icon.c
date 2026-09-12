/* awl_icon.c — xdg_toplevel_icon_v1 (per-window icons for the Recents entry)
 *
 * Clients create an icon (manager.create_icon), fill it with square wl_shm
 * buffers (add_buffer; icon names are ignored — no icon-theme support) and
 * assign it to their toplevel (manager.set_icon, double-buffered: applied at
 * the toplevel's next wl_surface.commit, empty commits included; null or an
 * empty icon resets).
 *
 * Pixel data is COPIED at add_buffer time, so — exactly like the spec's
 * lifetime rules — the applied icon survives both the icon object and the
 * wl_buffer being destroyed afterwards:
 *   icon object destroyed → the icon stays set on its toplevel;
 *   wl_buffer destroyed while the icon object lives → no_buffer protocol
 *   error (one destroy listener per add_buffer; a wl_resource carries any
 *   number of destroy listeners, the surface-side shm_buffer_gone watcher
 *   is independent).
 *
 * State sync to Android: every apply/reset fires cbs.window_icon → the
 * adaptation layer sends C_ICON; the Activity re-fetches the best buffer
 * (largest width×scale) with awl_window_get_icon (AWL_T_ICON) and puts it
 * into its task description.
 *
 * Locking: g_icon_lock covers the per-toplevel entries, the icon objects
 * and their pixel copies (touched from client dispatch threads, the
 * rwl.wr destroy path and the binder thread). mgr_set_icon takes rwl.rd
 * first to resolve the toplevel's surface id (order rwl → g_icon_lock,
 * never the reverse); awl_icon_surface_gone is called with rwl.wr already
 * held (awl_surface.c destroy path), like awl_idle_surface_gone. */
#include "awl_internal.h"
#include "xdg-toplevel-icon-v1-server-protocol.h"

#include <stddef.h>
#include <string.h>

#define AWL_ICON_MAX_BUFFERS 16

/* One add_buffer: pixels copied out of the shm pool (memory order B,G,R,A) */
struct awl_icon_px {
    int32_t w, h, scale;
    int alpha;              /* buffer has a real alpha channel (ARGB8888) */
    uint8_t* px;            /* w*h*4 bytes */
};

struct awl_toplevel_icon {
    int ref;                /* 1 = the icon object; +1 per pend/cur assignment */
    struct wl_resource* res;/* NULL once the client destroyed the object */
    int immutable;          /* set_icon happened — no further changes allowed */
    int n_px;
    struct awl_icon_px px[AWL_ICON_MAX_BUFFERS];
    struct wl_list bufs;    /* buffer watchers (icon_buf_watch.link) */
};

/* Watcher on every referenced wl_buffer: destruction while the icon object
 * lives → no_buffer error (protocol lifetime contract). Self-recycles like
 * the dmabuf destroy handler. */
struct icon_buf_watch {
    struct wl_listener listener;    /* on the wl_buffer resource */
    struct awl_toplevel_icon* icon;
    struct wl_list link;            /* icon->bufs */
};

/* Pending/applied icon per toplevel, keyed by the toplevel's surface id */
struct awl_win_icon {
    uint64_t id;
    struct awl_toplevel_icon* pend; /* set_icon — applied at the next commit */
    int pend_reset;                 /* null/empty set_icon — reset at commit */
    struct awl_toplevel_icon* cur;  /* live icon (survives icon object death) */
    struct wl_list link;
};

static struct wl_list g_win_icons;
static pthread_mutex_t g_icon_lock = PTHREAD_MUTEX_INITIALIZER;

/* ---- internals (caller holds g_icon_lock) ---- */

static void icon_unref(struct awl_toplevel_icon* ic) {
    if (--ic->ref > 0) return;
    struct icon_buf_watch* w;
    struct icon_buf_watch* wt;
    wl_list_for_each_safe(w, wt, &ic->bufs, link) {
        wl_list_remove(&w->link);            /* icon side */
        wl_list_remove(&w->listener.link);   /* buffer side (buffer outlives) */
        free(w);
    }
    for (int i = 0; i < ic->n_px; i++) free(ic->px[i].px);
    free(ic);
}

static void icon_buf_gone(struct wl_listener* l, void* data);

static struct awl_win_icon* win_entry(uint64_t id, int create) {
    struct awl_win_icon* it;
    wl_list_for_each(it, &g_win_icons, link)
        if (it->id == id) return it;
    if (!create) return NULL;
    it = calloc(1, sizeof(*it));
    if (!it) return NULL;
    it->id = id;
    wl_list_insert(g_win_icons.prev, &it->link);
    return it;
}

/* ---- xdg_toplevel_icon_v1 ---- */

static void icon_destroy(struct wl_client* client, struct wl_resource* res) {
    wl_resource_destroy(res);
}

static void icon_res_destroy(struct wl_resource* res) {
    struct awl_toplevel_icon* ic = wl_resource_get_user_data(res);
    if (!ic) return;
    pthread_mutex_lock(&g_icon_lock);
    ic->res = NULL;
    icon_unref(ic);
    pthread_mutex_unlock(&g_icon_lock);
}

static void icon_set_name(struct wl_client* client, struct wl_resource* res,
                          const char* name) {
    pthread_mutex_lock(&g_icon_lock);
    struct awl_toplevel_icon* ic = wl_resource_get_user_data(res);
    int imm = ic ? ic->immutable : 1;
    pthread_mutex_unlock(&g_icon_lock);
    if (imm) {
        wl_resource_post_error(res, XDG_TOPLEVEL_ICON_V1_ERROR_IMMUTABLE,
                "icon has been assigned to a toplevel and must not change");
        return;
    }
    /* No icon-theme support: a name-only icon resolves to nothing → applying
     * it resets, exactly like the protocol's empty-icon rule. */
    LOGI("toplevel icon name '%s' ignored (no icon-theme support; "
         "pixel buffers only)", name ? name : "");
}

static void icon_add_buffer(struct wl_client* client, struct wl_resource* res,
                            struct wl_resource* buf, int32_t scale) {
    pthread_mutex_lock(&g_icon_lock);
    struct awl_toplevel_icon* ic = wl_resource_get_user_data(res);
    if (!ic) { pthread_mutex_unlock(&g_icon_lock); return; }
    if (ic->immutable) {
        pthread_mutex_unlock(&g_icon_lock);
        wl_resource_post_error(res, XDG_TOPLEVEL_ICON_V1_ERROR_IMMUTABLE,
                "icon has been assigned to a toplevel and must not change");
        return;
    }

    /* must be wl_shm and square */
    struct wl_shm_buffer* shm = wl_shm_buffer_get(buf);
    int32_t w = 0, h = 0;
    uint32_t fmt = WL_SHM_FORMAT_ARGB8888;
    if (shm) {
        w = wl_shm_buffer_get_width(shm);
        h = wl_shm_buffer_get_height(shm);
        fmt = wl_shm_buffer_get_format(shm);
    }
    if (!shm || w <= 0 || w != h || scale <= 0 ||
        (fmt != WL_SHM_FORMAT_ARGB8888 && fmt != WL_SHM_FORMAT_XRGB8888)) {
        pthread_mutex_unlock(&g_icon_lock);
        wl_resource_post_error(res, XDG_TOPLEVEL_ICON_V1_ERROR_INVALID_BUFFER,
                "icon buffer must be a square wl_shm ARGB8888/XRGB8888 buffer");
        return;
    }

    /* watcher first: the buffer must outlive the icon object (no_buffer) */
    struct icon_buf_watch* wch = calloc(1, sizeof(*wch));
    if (!wch) {
        pthread_mutex_unlock(&g_icon_lock);
        wl_resource_post_no_memory(res);
        return;
    }

    /* snapshot the pixels now (contents stay untouched afterwards; the
     * begin/end access pair guards against a truncated pool / SIGBUS) */
    uint8_t* copy = malloc((size_t)w * (size_t)h * 4);
    if (!copy) {
        free(wch);
        pthread_mutex_unlock(&g_icon_lock);
        wl_resource_post_no_memory(res);
        return;
    }
    wl_shm_buffer_begin_access(shm);
    const uint8_t* src = wl_shm_buffer_get_data(shm);
    int32_t stride = wl_shm_buffer_get_stride(shm);
    for (int32_t y = 0; y < h; y++)
        memcpy(copy + (size_t)y * w * 4, src + (size_t)y * stride, (size_t)w * 4);
    wl_shm_buffer_end_access(shm);   /* a faulted pool kills the buffer resource (libwayland SIGBUS guard) */

    /* install: same (w,scale) overrides, else append (bounded: the smallest
     * entry is recycled when a client adds more than sensible) */
    struct awl_icon_px* slot = NULL;
    for (int i = 0; i < ic->n_px; i++)
        if (ic->px[i].w == w && ic->px[i].scale == scale) { slot = &ic->px[i]; break; }
    if (!slot && ic->n_px < AWL_ICON_MAX_BUFFERS) slot = &ic->px[ic->n_px++];
    if (!slot) {
        slot = &ic->px[0];
        for (int i = 1; i < ic->n_px; i++)
            if ((int64_t)ic->px[i].w * ic->px[i].scale <
                (int64_t)slot->w * slot->scale) slot = &ic->px[i];
    }
    free(slot->px);
    slot->w = w;
    slot->h = h;
    slot->scale = scale;
    slot->alpha = (fmt == WL_SHM_FORMAT_ARGB8888);
    slot->px = copy;

    wch->icon = ic;
    wch->listener.notify = icon_buf_gone;
    wl_list_insert(ic->bufs.prev, &wch->link);
    wl_resource_add_destroy_listener(buf, &wch->listener);
    pthread_mutex_unlock(&g_icon_lock);
    LOGI("toplevel icon buffer %dx%d@%dx added (%d buffered)",
         w, h, scale, ic->n_px);
}

static const struct xdg_toplevel_icon_v1_interface icon_iface = {
    .destroy = icon_destroy,
    .set_name = icon_set_name,
    .add_buffer = icon_add_buffer,
};

/* wl_buffer destroyed while the icon object is alive → no_buffer. Runs on
 * the owning client's dispatch thread, same thread as every other mutation
 * of this icon — ic itself cannot be freed concurrently. */
static void icon_buf_gone(struct wl_listener* l, void* data) {
    struct icon_buf_watch* w = (struct icon_buf_watch*)(
            (char*)l - offsetof(struct icon_buf_watch, listener));
    pthread_mutex_lock(&g_icon_lock);
    struct awl_toplevel_icon* ic = w->icon;
    struct wl_resource* res = ic->res;   /* NULL = object gone first: impossible here */
    wl_list_remove(&w->link);
    free(w);
    pthread_mutex_unlock(&g_icon_lock);
    if (res)
        wl_resource_post_error(res, XDG_TOPLEVEL_ICON_V1_ERROR_NO_BUFFER,
                "wl_buffer destroyed before the toplevel icon");
}

/* ---- xdg_toplevel_icon_manager_v1 ---- */

static void mgr_destroy(struct wl_client* client, struct wl_resource* res) {
    wl_resource_destroy(res);
}

static void mgr_create_icon(struct wl_client* client, struct wl_resource* mgr,
                            uint32_t id) {
    struct wl_resource* obj = wl_resource_create(
            client, &xdg_toplevel_icon_v1_interface, 1, id);
    struct awl_toplevel_icon* ic = calloc(1, sizeof(*ic));
    if (!obj || !ic) {
        if (obj) wl_resource_destroy(obj);
        free(ic);
        wl_resource_post_no_memory(mgr);
        return;
    }
    ic->ref = 1;
    ic->res = obj;
    wl_list_init(&ic->bufs);
    wl_resource_set_implementation(obj, &icon_iface, ic, icon_res_destroy);
}

static void mgr_set_icon(struct wl_client* client, struct wl_resource* mgr,
                         struct wl_resource* toplevel,
                         struct wl_resource* icon_res) {
    /* resolve the surface id under rwl.rd (destroy path strips the role
     * resource's user_data under rwl.wr — same shape as awl_idle) */
    pthread_rwlock_rdlock(&g_srv.rwl);
    struct awl_surface* s = toplevel ? wl_resource_get_user_data(toplevel) : NULL;
    uint64_t id = s ? s->id : 0;
    pthread_rwlock_unlock(&g_srv.rwl);
    if (!id) return;   /* toplevel already gone */

    struct awl_toplevel_icon* ic = NULL;
    if (icon_res) {
        ic = wl_resource_get_user_data(icon_res);
        if (!ic) return;
    }

    pthread_mutex_lock(&g_icon_lock);
    if (ic) ic->immutable = 1;   /* set_icon makes the icon immutable, even an empty one */
    /* null icon, or an icon with neither buffers nor a usable name → reset */
    struct awl_toplevel_icon* pend = (ic && ic->n_px > 0) ? ic : NULL;
    struct awl_win_icon* e = win_entry(id, 1);
    if (!e) {
        pthread_mutex_unlock(&g_icon_lock);
        wl_resource_post_no_memory(mgr);
        return;
    }
    if (e->pend && e->pend != ic) icon_unref(e->pend);
    e->pend = pend;
    if (pend) pend->ref++;
    e->pend_reset = (pend == NULL);
    pthread_mutex_unlock(&g_icon_lock);
    LOGI("window %llu: set_icon %s", (unsigned long long)id,
         pend ? "pending (applies at next commit)" : "= reset (null/empty icon)");
}

static const struct xdg_toplevel_icon_manager_v1_interface mgr_iface = {
    .destroy = mgr_destroy,
    .create_icon = mgr_create_icon,
    .set_icon = mgr_set_icon,
};

static void mgr_bind(struct wl_client* client, void* data,
                     uint32_t version, uint32_t id) {
    struct wl_resource* res = wl_resource_create(
            client, &xdg_toplevel_icon_manager_v1_interface, 1, id);
    if (!res) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(res, &mgr_iface, NULL, NULL);
    /* preferred size hint + the mandatory done */
    xdg_toplevel_icon_manager_v1_send_icon_size(res, 64);
    xdg_toplevel_icon_manager_v1_send_done(res);
}

void awl_icon_setup(void) {
    wl_list_init(&g_win_icons);
    if (!wl_global_create(g_srv.display,
                          &xdg_toplevel_icon_manager_v1_interface,
                          1, NULL, mgr_bind))
        LOGE("xdg_toplevel_icon_manager_v1 global create failed");
}

/* ---- commit (awl_surface.c surface_commit; no logic lock held, may hold
 * that surface's ev_lock — the callback fires from here, outside it) ----
 * Consumes the pending state on every commit, empty ones included. */
void awl_icon_commit(struct awl_surface* s) {
    int changed = 0, has_icon = 0;
    pthread_mutex_lock(&g_icon_lock);
    struct awl_win_icon* e = win_entry(s->id, 0);
    if (e && (e->pend || e->pend_reset)) {
        if (e->cur) icon_unref(e->cur);
        e->cur = e->pend;          /* NULL = reset */
        e->pend = NULL;
        e->pend_reset = 0;
        changed = 1;
        has_icon = (e->cur != NULL);
    }
    pthread_mutex_unlock(&g_icon_lock);
    if (changed) {
        LOGI("window %llu: toplevel icon %s", (unsigned long long)s->id,
             has_icon ? "applied" : "reset");
        if (g_srv.cbs.window_icon)
            g_srv.cbs.window_icon(g_srv.cbs.user, s->id);
    }
}

/* ---- surface death (awl_surface.c destroy path; caller holds rwl.wr) ----
 * The window's pending + applied icon die with the toplevel surface. */
void awl_icon_surface_gone(struct awl_surface* s) {
    pthread_mutex_lock(&g_icon_lock);
    struct awl_win_icon* e = win_entry(s->id, 0);
    if (e) {
        if (e->pend) icon_unref(e->pend);
        if (e->cur) icon_unref(e->cur);
        wl_list_remove(&e->link);
        free(e);
    }
    pthread_mutex_unlock(&g_icon_lock);
}

/* ---- binder fetch (waylandbridge.cpp AWL_T_ICON, any thread) ----
 * Best buffer (largest width×scale) swizzled to RGBA (Android Bitmap
 * ARGB_8888 byte order); XRGB gets a forced opaque alpha. 0 = no icon. */
int awl_window_get_icon(uint64_t id, void** pixels, int32_t* w, int32_t* h) {
    if (pixels) *pixels = NULL;
    if (w) *w = 0;
    if (h) *h = 0;
    pthread_mutex_lock(&g_icon_lock);
    struct awl_win_icon* e = win_entry(id, 0);
    struct awl_toplevel_icon* ic = e ? e->cur : NULL;
    if (!ic || !ic->n_px) {
        pthread_mutex_unlock(&g_icon_lock);
        return 0;
    }
    struct awl_icon_px* best = &ic->px[0];
    for (int i = 1; i < ic->n_px; i++)
        if ((int64_t)ic->px[i].w * ic->px[i].scale >
            (int64_t)best->w * best->scale) best = &ic->px[i];
    uint8_t* out = malloc((size_t)best->w * (size_t)best->h * 4);
    if (!out) {
        pthread_mutex_unlock(&g_icon_lock);
        return 0;
    }
    const uint8_t* in = best->px;
    size_t n = (size_t)best->w * (size_t)best->h;
    for (size_t i = 0; i < n; i++) {
        out[i * 4 + 0] = in[i * 4 + 2];   /* R */
        out[i * 4 + 1] = in[i * 4 + 1];   /* G */
        out[i * 4 + 2] = in[i * 4 + 0];   /* B */
        out[i * 4 + 3] = best->alpha ? in[i * 4 + 3] : 0xFF;
    }
    int32_t bw = best->w, bh = best->h;
    pthread_mutex_unlock(&g_icon_lock);
    if (pixels) *pixels = out;
    if (w) *w = bw;
    if (h) *h = bh;
    return 1;
}
