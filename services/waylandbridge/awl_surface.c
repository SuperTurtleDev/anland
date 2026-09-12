/* awl_surface.c — wl_compositor / wl_surface (double-buffered) / wl_region (v2)
 *
 * commit flow: pending→current → first frame (window not yet created)
 * triggers window_created + migration of the client to a dedicated event
 * thread; each commit calls window_dirty directly (adapter implementations
 * must be thread safe: internally these are renderer requests with
 * built-in coalescing). The old current buffer gets wl_buffer.release
 * when replaced.
 *
 * Locks: list topology = g_srv.rwl (dispatch thread wr, others rd);
 *        surface fields/sends = s->ev_lock (recursive); order rwl → ev_lock.
 */
#include "awl_internal.h"

#include <string.h>
#include <unistd.h>   /* dup */

/* Renderer request invoked directly (no idle marshaling: the callback is
 * itself thread safe, the renderer side coalesces requests).
 * A subsurface has no window of its own — dirty the "root" (chain walk-up
 * under rwl, mutually exclusive with topology writes). */
static void schedule_render(struct awl_surface* s) {
    s->dirty = 1;
    pthread_rwlock_rdlock(&g_srv.rwl);
    uint64_t root_id;
    if (s->role == AWL_ROLE_CURSOR) {
        /* cursor image (wl_pointer.set_cursor): no window of its own — redraw
         * the window currently compositing it (0 = not shown anywhere) */
        root_id = awl_input_cursor_window(s);
        if (!root_id) {
            /* nothing will present it → the deferred release queue would never
             * drain: hand replaced buffers back now (cursor themes animate by
             * committing new frames while the pointer may be elsewhere) */
            pthread_mutex_lock(&s->ev_lock);
            for (int i = 0; i < s->release_q_n; i++)
                wl_buffer_send_release(s->release_q[i]);
            s->release_q_n = 0;
            wl_client_flush(wl_resource_get_client(s->resource));
            pthread_mutex_unlock(&s->ev_lock);
        }
    } else {
        root_id = awl_subsurface_root(s)->id;
    }
    pthread_rwlock_unlock(&g_srv.rwl);
    if (root_id && g_srv.cbs.window_dirty)
        g_srv.cbs.window_dirty(g_srv.cbs.user, root_id);
}

/* Caller holds rwl (rd or wr) */
struct awl_surface* awl_surface_by_id(uint64_t id) {
    struct awl_surface* s;
    wl_list_for_each(s, &g_srv.surfaces, link) {
        if (s->id == id) return s;
    }
    return NULL;
}

struct awl_surface* awl_surface_from_res(struct wl_resource* res) {
    return wl_resource_get_user_data(res);
}

/* Window id → client host pid, for foreground scheduling (awl_sched.c).
 * wl_client_get_credentials reads the pid libwayland cached from the socket
 * credentials at connect time — a plain field copy, valid at any point the
 * surface still resolves (the window_destroyed callback fires before the
 * surface is unlinked, so it works on the destroy path too; when a client
 * dies its resources are torn down before the wl_client itself). Callers
 * hold no daemon locks (rwl.rd is taken here). */
pid_t awl_window_client_pid(uint64_t id) {
    pthread_rwlock_rdlock(&g_srv.rwl);
    struct awl_surface* s = awl_surface_by_id(id);
    struct wl_client* c = (s && s->resource) ? wl_resource_get_client(s->resource) : NULL;
    pid_t pid = -1;
    if (c) wl_client_get_credentials(c, &pid, NULL, NULL);
    pthread_rwlock_unlock(&g_srv.rwl);
    return pid > 0 ? pid : 0;
}

/* Window id → wayland client uid, same credential source and locking as
 * awl_window_client_pid (binder SURFACE auth pass: the attaching app's uid
 * must equal the uid of the client that owns the window). (uid_t)-1 =
 * unknown/destroyed — never equals a real binder uid, so an unresolvable
 * window denies unlisted callers by itself. */
uid_t awl_window_client_uid(uint64_t id) {
    pthread_rwlock_rdlock(&g_srv.rwl);
    struct awl_surface* s = awl_surface_by_id(id);
    struct wl_client* c = (s && s->resource) ? wl_resource_get_client(s->resource) : NULL;
    uid_t uid = (uid_t)-1;
    if (c) wl_client_get_credentials(c, NULL, &uid, NULL);
    pthread_rwlock_unlock(&g_srv.rwl);
    return uid;
}

/* Enqueue a deferred release (caller holds this surface's ev_lock).
 * Full queue = rendering stalled: release the head immediately so the
 * client does not starve (old timing, better than deadlock). */
void awl_surface_release_defer(struct awl_surface* s, struct wl_resource* buf) {
    if (s->release_q_n == 4) {
        wl_buffer_send_release(s->release_q[0]);
        memmove(s->release_q, s->release_q + 1, 3 * sizeof(void*));
        s->release_q[3] = buf;
        return;
    }
    s->release_q[s->release_q_n++] = buf;
}

/* shm wl_buffer.destroy (libwayland-built resource, intercepted via a
 * destroy listener; dmabuf counterpart = awl_dmabuf.c
 * dmabuf_buffer_destroy_handler): the client may destroy buffer + pool
 * without waiting for release (verified by relcross resize) — the
 * server-side mapping is munmapped as the wl_shm_buffer is freed. Strip
 * every reference to it from each surface (pending/current/latched/
 * release_q), otherwise a later get_buffer/commit dereferences a
 * dangling resource. In-flight render snapshots are unaffected
 * (get_buffer already pinned references). This listener runs on that
 * client's dispatch thread — same thread as commit, the sole writer of
 * release_q; the render side only removes, never adds (same lock-free
 * pre-check as the dmabuf handler). */
static void shm_buffer_gone(struct wl_listener* l, void* data) {
    struct wl_resource* res = data;
    free(l);   /* watcher self-recycles when the resource dies (removes itself within emit's safe iteration) */
    pthread_rwlock_rdlock(&g_srv.rwl);
    struct awl_surface* s;
    wl_list_for_each(s, &g_srv.surfaces, link) {
        int queued = 0;
        for (int i = 0; i < s->release_q_n && !queued; i++)
            queued = (s->release_q[i] == res);
        if (s->pending_buffer_res != res && s->current_buffer_res != res &&
            s->latched_buffer_res != res && !queued)
            continue;   /* unrelated window: skip taking ev_lock (commit unaffected) */
        pthread_mutex_lock(&s->ev_lock);
        if (s->pending_buffer_res == res) s->pending_buffer_res = NULL;
        if (s->current_buffer_res == res) s->current_buffer_res = NULL;
        if (s->latched_buffer_res == res) {
            s->latched_buffer_res = NULL;
            s->latched_attach = 0;
            s->sub_latched = 0;
        }
        for (int i = 0; i < s->release_q_n; )   /* resource dying: drop it, no further release */
            if (s->release_q[i] == res) s->release_q[i] = s->release_q[--s->release_q_n];
            else i++;
        pthread_mutex_unlock(&s->ev_lock);
    }
    pthread_rwlock_unlock(&g_srv.rwl);
}

/* wl_callback(frame) resource destroyed → removed from the surface list.
 * Multi-threaded entry (presented direct send / client destroy / surface
 * destroy) → ev_lock */
static void frame_cb_res_destroy(struct wl_resource* res) {
    struct awl_frame_cb* cb = wl_resource_get_user_data(res);
    if (cb) {
        struct awl_surface* s = cb->s;
        pthread_mutex_lock(&s->ev_lock);
        if (!cb->detached) wl_list_remove(&cb->link);
        pthread_mutex_unlock(&s->ev_lock);
        free(cb);
    }
}

/* ---------------- wl_region (bounding box, not part of layout) ----------------
 * Consumers today only need a rectangle (pointer-constraints confine region).
 * The region records the union bounding box of the added rectangles;
 * subtract is ignored for the box (holes and true region algebra are out of
 * scope — real confine regions are single rects). Consumers copy the box at
 * request time; the wl_region resource itself is never held. */
struct awl_region_bb {
    int32_t x, y, w, h;
    int set;   /* any rectangle added */
};

static void region_res_destroy(struct wl_resource* res) {
    free(wl_resource_get_user_data(res));
}
static void region_destroy(struct wl_client* client, struct wl_resource* res) {
    wl_resource_destroy(res);
}
static void region_add(struct wl_client* client, struct wl_resource* res,
                       int32_t x, int32_t y, int32_t w, int32_t h) {
    struct awl_region_bb* bb = wl_resource_get_user_data(res);
    if (!bb || w <= 0 || h <= 0) return;
    if (!bb->set) {
        bb->x = x; bb->y = y; bb->w = w; bb->h = h;
        bb->set = 1;
    } else {
        int32_t x2 = bb->x + bb->w, y2 = bb->y + bb->h;
        if (x < bb->x) bb->x = x;
        if (y < bb->y) bb->y = y;
        if (x + w > x2) x2 = x + w;
        if (y + h > y2) y2 = y + h;
        bb->w = x2 - bb->x; bb->h = y2 - bb->y;
    }
}
static void region_subtract(struct wl_client* client, struct wl_resource* res,
                            int32_t x, int32_t y, int32_t w, int32_t h) {
    /* bbox ignores holes (see section comment) */
}

static const struct wl_region_interface region_iface = {
    .destroy = region_destroy,
    .add = region_add,
    .subtract = region_subtract,
};

/* Bounding-box snapshot for consumers (pointer-constraints confine region);
 * 0 = no rectangle was ever added */
int awl_region_bbox(struct wl_resource* region, int32_t* x, int32_t* y,
                    int32_t* w, int32_t* h) {
    struct awl_region_bb* bb = region ? wl_resource_get_user_data(region) : NULL;
    if (!bb || !bb->set) return 0;
    *x = bb->x; *y = bb->y; *w = bb->w; *h = bb->h;
    return 1;
}

/* ---------------- wl_surface ---------------- */

static void surface_destroy_impl(struct wl_resource* res) {
    struct awl_surface* s = wl_resource_get_user_data(res);
    if (!s) return;

    LOGI("surface %llu destroyed (mapped=%d)",
            (unsigned long long)s->id, s->mapped);

    /* renderer detach (joins the render thread) first — its
     * get_buffer/presented hold rd; only once they finish naturally can
     * the wrlock be acquired → nothing in flight, teardown is safe.
     * A cursor-role surface never owned a window (it may carry mapped=1 from
     * a buffer committed before set_cursor). */
    if (s->mapped && s->role != AWL_ROLE_CURSOR && g_srv.cbs.window_destroyed)
        g_srv.cbs.window_destroyed(g_srv.cbs.user, s->id);

    pthread_rwlock_wrlock(&g_srv.rwl);
    /* DnD state references (drag origin/target/icon layer died →
     * leave/cancel); before topology removal — the datadev side holds
     * dd_lock reading the pointer for one last decision */
    awl_datadev_surface_gone(s);
    awl_ime_surface_gone(s);   /* text_input associations to this surface */
    /* pointer focus layer / its window / the cursor image died → the client
     * cursor is dropped; the window is redrawn without it and its Android
     * pointer restored after the lock (callbacks never run under rwl.wr) */
    uint64_t cursor_win = awl_input_surface_gone(s);
    /* constraints on this surface / its root die with it: unlock + release
     * the Activity capture after the lock (same notify-after-unlock shape) */
    uint64_t constr_win = awl_input_constr_surface_gone(s);
    /* idle inhibitors on this surface / its root die with it: C_KEEPON off
     * after the lock, when the window's aggregate flipped to zero */
    uint64_t idle_win = awl_idle_surface_gone(s);
    /* the toplevel icon (pending + applied) dies with its surface */
    awl_icon_surface_gone(s);

    struct awl_frame_cb* cb;
    struct awl_frame_cb* tmp;
    wl_list_for_each_safe(cb, tmp, &s->frame_callbacks, link) {
        wl_resource_destroy(cb->resource);   /* listener removes+frees under ev_lock */
    }
    /* On disconnect wl_map destroys in id order: wl_surface before
     * xdg_surface/toplevel/popup — strip their back-references, otherwise
     * the later destroy handlers lock the already-freed ev_lock */
    if (s->xdg_surface_res)
        wl_resource_set_user_data(s->xdg_surface_res, NULL);
    if (s->xdg_role_res)
        wl_resource_set_user_data(s->xdg_role_res, NULL);
    if (s->subsurface_res)
        wl_resource_set_user_data(s->subsurface_res, NULL);
    if (s->viewport_res)   /* #31: viewport handler holds a surface pointer — break the link */
        wl_resource_set_user_data(s->viewport_res, NULL);
    if (s->frac_res)       /* fractional_scale handler does not touch surface, just clear the flag */
        s->frac_res = NULL;
    if (s->xwayland_res) { /* #32: xwayland_surface_v1 destroy handler holds a pointer */
        wl_resource_set_user_data(s->xwayland_res, NULL);
        s->xwayland_res = NULL;
    }

    /* subsurface topology teardown: this is a child layer → unlink +
     * root window redraws without the layer; this is a parent → orphan
     * the child layers (keep surface and role, the client will destroy
     * them on its own) */
    uint64_t sub_root_id = 0;
    int sub_dirty = 0;
    struct wl_resource* latched_drop = NULL;
    if (s->sub_parent) {
        struct awl_surface* root = awl_subsurface_root(s);
        sub_root_id = root->id;
        sub_dirty = root->mapped;
        wl_list_remove(&s->sub_link);
        s->sub_parent = NULL;
    }
    if (s->sub_latched && s->latched_attach) {   /* latched buffer never presented — release directly */
        latched_drop = s->latched_buffer_res;
        s->latched_buffer_res = NULL;
    }
    s->sub_latched = 0;
    s->latched_attach = 0;
    /* Drain the deferred release queue: surface is dying, there will be no present */
    for (int i = 0; i < s->release_q_n; i++)
        wl_buffer_send_release(s->release_q[i]);
    s->release_q_n = 0;
    {
        struct awl_surface* ch;
        struct awl_surface* ctmp;
        wl_list_for_each_safe(ch, ctmp, &s->sub_children, sub_link) {
            wl_list_remove(&ch->sub_link);
            ch->sub_parent = NULL;
        }
    }
    wl_list_remove(&s->link);
    pthread_mutex_destroy(&s->ev_lock);
    free(s);
    pthread_rwlock_unlock(&g_srv.rwl);
    if (latched_drop)
        wl_buffer_send_release(latched_drop);
    if (sub_dirty && g_srv.cbs.window_dirty)   /* child layer gone → root window redraw */
        g_srv.cbs.window_dirty(g_srv.cbs.user, sub_root_id);
    awl_input_cursor_gone_notify(cursor_win);   /* redraw without the cursor + restore the Android pointer */
    awl_input_constr_gone_notify(constr_win);   /* C_CAPTURE none: the Activity releases the capture */
    awl_idle_gone_notify(idle_win);             /* C_KEEPON off: the Activity clears FLAG_KEEP_SCREEN_ON */
    wl_resource_set_user_data(res, NULL);
}

static void surface_destroy(struct wl_client* client, struct wl_resource* res) {
    wl_resource_destroy(res);
}

static void surface_attach(struct wl_client* client, struct wl_resource* res,
                           struct wl_resource* buffer, int32_t x, int32_t y) {
    struct awl_surface* s = wl_resource_get_user_data(res);
    LOGD("surface attach buf=%p off=%d,%d", (void*)buffer, x, y);
    if (!s) return;
    if (x != 0 || y != 0)
        LOGD("surface %llu attach offset %d,%d (ignored)",
                (unsigned long long)s->id, x, y);
    s->pending_buffer_res = buffer;   /* NULL = detach */
    s->pending_attached = 1;         /* this commit cycle has an attach (empty commit leaves buffer unchanged) */
    s->pending_offset_x = x;
    s->pending_offset_y = y;
    /* shm buffer lifetime watch (dmabuf-built resources use their own
     * destroy handler): one watcher per resource, deduplicated by notify
     * (with multiple surfaces sharing a buffer the removal scan already
     * covers the whole table; duplicate registration is harmless but
     * redundant). */
    if (buffer && wl_shm_buffer_get(buffer) &&
        !wl_resource_get_destroy_listener(buffer, shm_buffer_gone)) {
        struct wl_listener* wl = calloc(1, sizeof(*wl));
        if (wl) {
            wl->notify = shm_buffer_gone;
            wl_resource_add_destroy_listener(buffer, wl);
        }
    }
}

static void surface_damage(struct wl_client* client, struct wl_resource* res,
                           int32_t x, int32_t y, int32_t w, int32_t h) {
    struct awl_surface* s = wl_resource_get_user_data(res);
    if (!s) return;
    /* Accumulate bounding box (surface coordinates; extendable to a rectangle list in production) */
    if (s->pending_damage_empty) {
        s->pd_x = x; s->pd_y = y; s->pd_w = w; s->pd_h = h;
        s->pending_damage_empty = 0;
    } else {
        int32_t x2 = s->pd_x + s->pd_w, y2 = s->pd_y + s->pd_h;
        if (x < s->pd_x) s->pd_x = x;
        if (y < s->pd_y) s->pd_y = y;
        if (x + w > x2) x2 = x + w;
        if (y + h > y2) y2 = y + h;
        s->pd_w = x2 - s->pd_x; s->pd_h = y2 - s->pd_y;
    }
}

static void surface_damage_buffer(struct wl_client* client,
                                  struct wl_resource* res,
                                  int32_t x, int32_t y, int32_t w, int32_t h) {
    /* buffer-coordinate damage (only differs when scale≠1, equivalent at scale=1) */
    surface_damage(client, res, x, y, w, h);
}

/* Pending damage merges into the renderer-visible damage at every commit
 * (damage is double-buffered surface state — it applies on commit regardless
 * of attach; clients that redraw a wl_buffer in place send damage+commit
 * WITHOUT re-attaching). has_attach: this commit presented a new buffer —
 * an attach without any damage then means FULL (protocol default: no
 * information = whole surface), while an empty commit (no attach, no damage)
 * changes nothing and keeps the state (otherwise every ack_configure would
 * force a full re-upload). Caller holds s->ev_lock (surface_commit immediate
 * path + sync-subsurface latch apply, awl_subsurface.c). */
void awl_damage_merge_pending(struct awl_surface* s, int has_attach) {
    if (s->pending_damage_empty) {
        if (!has_attach) return;   /* empty commit: nothing changed */
        s->cd_state = AWL_DMG_FULL;
    } else if (s->cd_state == AWL_DMG_NONE) {
        s->cur_damage_x = s->pd_x;
        s->cur_damage_y = s->pd_y;
        s->cur_damage_w = s->pd_w;
        s->cur_damage_h = s->pd_h;
        s->cd_state = AWL_DMG_RECT;
    } else if (s->cd_state == AWL_DMG_RECT) {   /* bbox union */
        int32_t x2 = s->cur_damage_x + s->cur_damage_w;
        int32_t y2 = s->cur_damage_y + s->cur_damage_h;
        if (s->pd_x < s->cur_damage_x) s->cur_damage_x = s->pd_x;
        if (s->pd_y < s->cur_damage_y) s->cur_damage_y = s->pd_y;
        if (s->pd_x + s->pd_w > x2) x2 = s->pd_x + s->pd_w;
        if (s->pd_y + s->pd_h > y2) y2 = s->pd_y + s->pd_h;
        s->cur_damage_w = x2 - s->cur_damage_x;
        s->cur_damage_h = y2 - s->cur_damage_y;
    }   /* FULL stays full (rect damage subsumed) */
    s->pending_damage_empty = 1;
    s->cd_gen++;
}

/* no-op requests (region/transform recording left for later, no protocol error sent to the client) */
static void surface_set_opaque_region(struct wl_client* c, struct wl_resource* r,
                                      struct wl_resource* region) {
    LOGD("set_opaque_region %p", (void*)region);
}
static void surface_set_input_region(struct wl_client* c, struct wl_resource* r,
                                     struct wl_resource* region) {}
static void surface_set_buffer_transform(struct wl_client* c, struct wl_resource* r,
                                         int32_t transform) {
    /* Wayland: buffer transform, one of wl_output.transform (0..7), applied on
     * commit (double-buffered like viewport state); invalid value = protocol
     * error. 90/270 swap the surface logical size (awl_viewport.c). */
    if (transform < 0 || transform > 7) {
        wl_resource_post_error(r, WL_SURFACE_ERROR_INVALID_TRANSFORM,
                               "invalid transform %d", transform);
        return;
    }
    struct awl_surface* s = wl_resource_get_user_data(r);
    if (!s) return;
    pthread_mutex_lock(&s->ev_lock);
    s->pend_buf_transform = transform;
    pthread_mutex_unlock(&s->ev_lock);
}
static void surface_set_buffer_scale(struct wl_client* c, struct wl_resource* r,
                                     int32_t scale) {
    /* #31: record integer buffer scale (logical size = buffer/scale; when
     * coexisting with viewport dst, dst wins — same shape as kwin
     * surfaceSize) */
    if (scale < 1) {
        wl_resource_post_error(r, WL_SURFACE_ERROR_INVALID_SCALE,
                               "scale must be positive");
        return;
    }
    struct awl_surface* s = wl_resource_get_user_data(r);
    if (!s) return;
    pthread_mutex_lock(&s->ev_lock);
    s->buf_scale = scale;
    pthread_mutex_unlock(&s->ev_lock);
}

static void surface_frame(struct wl_client* client, struct wl_resource* res,
                          uint32_t callback) {
    LOGD("frame cb id=%u", callback);
    struct awl_surface* s = wl_resource_get_user_data(res);
    if (!s) return;
    struct wl_resource* cb_res = wl_resource_create(
            client, &wl_callback_interface, 1, callback);
    struct awl_frame_cb* cb = calloc(1, sizeof(*cb));
    if (!cb || !cb_res) {
        if (cb_res) wl_resource_destroy(cb_res);
        wl_resource_post_no_memory(res);
        return;
    }
    cb->resource = cb_res;
    cb->s = s;
    wl_resource_set_implementation(cb_res, NULL, cb, frame_cb_res_destroy);
    pthread_mutex_lock(&s->ev_lock);   /* mutex against concurrent iteration by render-thread presented */
    wl_list_insert(s->frame_callbacks.prev, &cb->link);
    pthread_mutex_unlock(&s->ev_lock);
}

static void surface_commit(struct wl_client* client, struct wl_resource* res) {
    struct awl_surface* s = wl_resource_get_user_data(res);
    if (!s) return;

    /* xdg role state machine check: only a commit that attached a buffer
     * requires a prior ack (an empty commit is legal, clients use it to
     * request the initial configure — weston simple-egl pattern) */
    if ((s->role == AWL_ROLE_TOPLEVEL || s->role == AWL_ROLE_POPUP) &&
        !s->acked && s->pending_buffer_res) {
        wl_resource_post_error(s->xdg_surface_res,
            XDG_SURFACE_ERROR_UNCONFIGURED_BUFFER,
            "buffer committed before first ack_configure");
        return;
    }

    /* KWin alignment: commit on an effective sync child layer latches — state not applied, waits for the parent commit */
    if (awl_subsurface_maybe_latch(s))
        return;

    /* Apply buffer state — only touch current if attached this cycle
     * (protocol: pending persists across commits; the empty commit for
     * ack_configure must not detach). Every replaced current goes into
     * the deferred queue (release after presented): the upload happens on
     * the render thread asynchronously to this commit — releasing
     * immediately would let the client overwrite/destroy (same for shm
     * glTex(Sub)Image2D and dmabuf GPU sampling, tombstones 26/30).
     * ev_lock: the render thread's get_buffer concurrently reads
     * current_buffer_res */
    pthread_mutex_lock(&s->ev_lock);
    if (s->pend_geom) {          /* xdg double-buffered geometry takes effect in the same commit as the buffer */
        s->geom_x = s->pend_gx; s->geom_y = s->pend_gy;
        s->geom_w = s->pend_gw; s->geom_h = s->pend_gh;
        s->geom_valid = 1;
        s->pend_geom = 0;
    }
    if (s->pend_vpd) {           /* #31 viewport dst double-buffered (same as kwin) */
        s->vp_dst_w = s->pend_vpd_w;
        s->vp_dst_h = s->pend_vpd_h;
        s->vp_has_dst = s->vp_dst_w > 0;
        s->pend_vpd = 0;
    }
    if (s->pend_vps) {           /* viewport source (0 size = reset) */
        s->vp_sx = s->pend_vps_x; s->vp_sy = s->pend_vps_y;
        s->vp_sw = s->pend_vps_w; s->vp_sh = s->pend_vps_h;
        s->vp_has_src = s->vp_sw > 0 && s->vp_sh > 0;
        s->pend_vps = 0;
    }
    if (s->pend_buf_transform >= 0) {   /* set_buffer_transform (double-buffered) */
        s->buf_transform = s->pend_buf_transform;
        s->pend_buf_transform = -1;
    }
    struct wl_resource* old = s->current_buffer_res;
    int attached = s->pending_attached;
    int32_t off_x = 0, off_y = 0;   /* attach dx,dy of this cycle (cursor role: moves the hotspot) */
    if (s->pending_attached) {
        s->current_buffer_res = s->pending_buffer_res;
        s->pending_buffer_res = NULL;
        s->pending_attached = 0;
        off_x = s->pending_offset_x;
        off_y = s->pending_offset_y;
        s->pending_offset_x = s->pending_offset_y = 0;   /* consumed with the attach */
    }
    /* damage applies on EVERY commit (not just attach-commits): damage+commit
     * without re-attach is the standard in-place redraw pattern */
    awl_damage_merge_pending(s, attached);
    /* Extract the first-map window size inside the lock: window_created
     * below is a callback outside the lock, during which shm_buffer_gone
     * may strip current to NULL (dangling dereference) */
    int32_t map_bw = 0, map_bh = 0;
    if (!s->mapped && s->current_buffer_res) {
        struct wl_shm_buffer* mshm = wl_shm_buffer_get(s->current_buffer_res);
        if (mshm) {
            map_bw = wl_shm_buffer_get_width(mshm);
            map_bh = wl_shm_buffer_get_height(mshm);
        } else {
            struct awl_buffer* mb =
                    wl_resource_get_user_data(s->current_buffer_res);
            if (mb && mb->width) { map_bw = (int32_t)mb->width; map_bh = (int32_t)mb->height; }
        }
    }
    int replaced = old && old != s->current_buffer_res;
    if (replaced)
        awl_surface_release_defer(s, old);
    pthread_mutex_unlock(&s->ev_lock);

    /* xdg-toplevel-icon: pending icon state applies on every commit (empty
     * included); awl_icon_commit fires its callback itself, outside ev_lock */
    awl_icon_commit(s);

    /* cursor image committed with an attach offset → hotspot follows (kwin
     * SurfaceCursorSource::refresh: hotspot -= offset); outside ev_lock
     * (order g_cursor_lock → ev_lock) */
    if (s->role == AWL_ROLE_CURSOR && attached && (off_x || off_y))
        awl_input_cursor_commit(s, off_x, off_y);

        /* State application → child layer double-buffered positions take
     * effect + sync-latch cascade applies (KWin merge). Must come before
     * the empty-commit early return: the empty commit is precisely the
     * protocol moment when sync child state takes effect (the parent may
     * have no buffer — in chrome's primary subsurface mode the root
     * surface is always empty, main content all lives on the sync child
     * layer, verified by the 2026-09-09 black screen). */
    int children_applied = awl_subsurface_parent_applied(s);

    if (!s->current_buffer_res) {   /* empty commit (e.g. requesting configure / sync flush) */
        LOGD("surface %llu empty commit (children_applied=%d)",
             (unsigned long long)s->id, children_applied);
        /* child state changed → root window redraw (walk-up inside); a cursor
         * image detached (attach NULL) → the compositing window drops the layer */
        if (children_applied || (s->role == AWL_ROLE_CURSOR && attached))
            schedule_render(s);
        return;
    }

    /* First buffer → map → migrate to a dedicated event thread + create
     * the Android window. subsurface/popup have no window of their own:
     * they composite as layers on the parent window (get_subsurface/
     * get_popup already linked into the tree, schedule_render already
     * dirtied the root), frame callbacks are issued by the render side
     * per layer presented. */
    if (!s->mapped && s->role != AWL_ROLE_SUBSURFACE && s->role != AWL_ROLE_CURSOR) {
        s->mapped = 1;
        LOGI("surface %llu mapped (role=%d)",
                (unsigned long long)s->id, s->role);
        /* attach is the handover: from here on every request of this
         * client is dispatched by its child event thread (this thread is
         * exactly the old loop thread, satisfying the migration contract) */
        awl_client_maybe_migrate(client);
        if (s->role == AWL_ROLE_TOPLEVEL || s->role == AWL_ROLE_XWAYLAND) {
            if (g_srv.cbs.window_created)
                g_srv.cbs.window_created(g_srv.cbs.user, s->id,
                                         map_bw, map_bh, s->title, 0);
            /* Resend resizes that arrived before map (Android fully owns
             * window sizing); the XWAYLAND role has no xdg object — the
             * role check inside flush naturally skips it */
            if (s->role == AWL_ROLE_TOPLEVEL)
                awl_xdg_flush_pending(s->id);
        }
    }

    LOGD("surface %llu commit buf=%p",
         (unsigned long long)s->id, (void*)s->current_buffer_res);
    schedule_render(s);
}

static const struct wl_surface_interface surface_iface = {
    .destroy = surface_destroy,
    .attach = surface_attach,
    .damage = surface_damage,
    .frame = surface_frame,
    .set_opaque_region = surface_set_opaque_region,
    .set_input_region = surface_set_input_region,
    .commit = surface_commit,
    .set_buffer_transform = surface_set_buffer_transform,
    .set_buffer_scale = surface_set_buffer_scale,
    .damage_buffer = surface_damage_buffer,
};

static void compositor_create_surface(struct wl_client* client,
                                      struct wl_resource* res, uint32_t id) {
    struct wl_resource* sres = wl_resource_create(
            client, &wl_surface_interface,
            wl_resource_get_version(res), id);
    if (!sres) { wl_resource_post_no_memory(res); return; }

    struct awl_surface* s = calloc(1, sizeof(*s));
    if (!s) { wl_resource_destroy(sres); wl_resource_post_no_memory(res); return; }
    s->resource = sres;
    s->id = g_srv.next_surface_id++;   /* event thread is the sole writer, no lock */
    s->role = AWL_ROLE_NONE;
    s->buf_scale = 1;   /* #31: wl_surface.set_buffer_scale defaults to 1 */
    s->buf_transform = 0;   /* set_buffer_transform default = wl_output.transform.normal */
    s->pend_buf_transform = -1;   /* -1 = nothing pending */
    s->pending_damage_empty = 1;   /* damage accumulator starts empty (calloc 0 = "has rect") */
    {
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(&s->ev_lock, &attr);   /* recursive: presented destroys cb while holding the lock */
        pthread_mutexattr_destroy(&attr);
    }
    wl_list_init(&s->frame_callbacks);
    wl_list_init(&s->sub_children);   /* may serve as a subsurface parent (child layers link in) */
    pthread_rwlock_wrlock(&g_srv.rwl);   /* topology write */
    wl_list_insert(g_srv.surfaces.prev, &s->link);
    pthread_rwlock_unlock(&g_srv.rwl);
    wl_resource_set_implementation(sres, &surface_iface, s, surface_destroy_impl);
    LOGI("surface %llu created", (unsigned long long)s->id);
}

static void compositor_create_region(struct wl_client* client,
                                     struct wl_resource* res, uint32_t id) {
    struct wl_resource* rres = wl_resource_create(
            client, &wl_region_interface, wl_resource_get_version(res), id);
    if (!rres) { wl_resource_post_no_memory(res); return; }
    struct awl_region_bb* bb = calloc(1, sizeof(*bb));
    if (!bb) { wl_resource_destroy(rres); wl_resource_post_no_memory(res); return; }
    wl_resource_set_implementation(rres, &region_iface, bb, region_res_destroy);
}

static const struct wl_compositor_interface compositor_iface = {
    .create_surface = compositor_create_surface,
    .create_region = compositor_create_region,
};

static void compositor_bind(struct wl_client* client, void* data,
                            uint32_t version, uint32_t id) {
    struct wl_resource* res = wl_resource_create(
            client, &wl_compositor_interface,
            version < 4 ? version : 4, id);
    wl_resource_set_implementation(res, &compositor_iface, NULL, NULL);
}

void awl_surface_setup(void) {
    g_srv.g_compositor = wl_global_create(g_srv.display,
                                          &wl_compositor_interface, 4,
                                          NULL, compositor_bind);
}

/* ---- adapter render-pull interfaces (called on the render thread) ---- */

int awl_surface_get_buffer(uint64_t id, awl_buffer_info_t* out) {
    memset(out, 0, sizeof(*out));
    pthread_rwlock_rdlock(&g_srv.rwl);
    struct awl_surface* s = awl_surface_by_id(id);
    if (!s) {
        pthread_rwlock_unlock(&g_srv.rwl);
        return -1;
    }
    /* ev_lock throughout: mutually exclusive with commit swapping buffers — what gets dup'd is always the newest current */
    pthread_mutex_lock(&s->ev_lock);
    struct wl_resource* buf_res = s->current_buffer_res;
    if (!buf_res) {
        pthread_mutex_unlock(&s->ev_lock);
        pthread_rwlock_unlock(&g_srv.rwl);
        return -1;
    }

    struct wl_shm_buffer* shm = wl_shm_buffer_get(buf_res);
    if (shm) {
        /* wl_shm enum (0=argb, 1=xrgb) → uniformly convert to DRM fourcc */
        switch (wl_shm_buffer_get_format(shm)) {
        case WL_SHM_FORMAT_ARGB8888:
            out->drm_format = AWL_FORMAT_ARGB8888;
            break;
        case WL_SHM_FORMAT_XRGB8888:
            out->drm_format = AWL_FORMAT_XRGB8888;
            break;
        default:
            LOGE("unsupported shm format %d", wl_shm_buffer_get_format(shm));
            pthread_mutex_unlock(&s->ev_lock);
            pthread_rwlock_unlock(&g_srv.rwl);
            return -1;
        }
        out->kind = AWL_BUFFER_SHM;
        /* Pin references (upload is asynchronous to commit, during which
         * the client may destroy buffer/pool): the wl_shm_buffer struct +
         * pool mapping stay alive until returned, otherwise
         * glTex(Sub)Image2D reads munmapped memory → adreno copy thread
         * SIGSEGV (tombstones 26/28/30); the pool reference additionally
         * defers pool resize (mremap moves the mapping). The caller does
         * wl_shm_buffer_unref + wl_shm_pool_unref when done. */
        wl_shm_buffer_ref(shm);
        out->pool = wl_shm_buffer_ref_pool(shm);
        out->shm = shm;
        out->token = buf_res;
        out->width = (uint32_t)wl_shm_buffer_get_width(shm);
        out->height = (uint32_t)wl_shm_buffer_get_height(shm);
        out->stride = (uint32_t)wl_shm_buffer_get_stride(shm);
        pthread_mutex_unlock(&s->ev_lock);
        pthread_rwlock_unlock(&g_srv.rwl);
        return 0;
    }
    /* dmabuf wrapping: fd is dup'd inside the lock — the snapshot stays valid after buffer destruction (memory pinned) */
    struct awl_buffer* b = wl_resource_get_user_data(buf_res);
    if (b && b->width && b->dmabuf_fd >= 0) {
        int fd = dup(b->dmabuf_fd);
        pthread_mutex_unlock(&s->ev_lock);
        if (fd < 0) {
            pthread_rwlock_unlock(&g_srv.rwl);
            return -1;
        }
        out->kind = AWL_BUFFER_DMABUF;
        out->token = buf_res;
        out->fd = fd;
        out->ino = b->ino;
        out->width = b->width;
        out->height = b->height;
        out->stride = b->stride;
        out->drm_format = b->drm_format;
        out->modifier = b->modifier;
        pthread_rwlock_unlock(&g_srv.rwl);
        return 0;
    }
    pthread_mutex_unlock(&s->ev_lock);
    pthread_rwlock_unlock(&g_srv.rwl);
    return -1;   /* unknown buffer type */
}

/* Root's view transform for the render thread: geometry origin (view (0,0)
 * ↔ geometry rectangle origin; chrome dst shadow margins overflow the
 * bounds and get clipped) + the logical→view mapping of awl_surface_view_map
 * — one snapshot under ev_lock, the same numbers the input inverse reads.
 * Unknown root → identity. Any thread; rd resolution + ev_lock snapshot. */
void awl_surface_get_view_xform(uint64_t id, awl_view_xform_t* out) {
    out->gox = out->goy = 0;
    out->sx = out->sy = 1.0;
    out->ox = out->oy = 0.0;
    pthread_rwlock_rdlock(&g_srv.rwl);
    struct awl_surface* s = awl_surface_by_id(id);
    if (s) {
        pthread_mutex_lock(&s->ev_lock);
        if (s->geom_valid) { out->gox = s->geom_x; out->goy = s->geom_y; }
        awl_surface_view_map(s, &out->sx, &out->sy, &out->ox, &out->oy);
        pthread_mutex_unlock(&s->ev_lock);
    }
    pthread_rwlock_unlock(&g_srv.rwl);
}

/* Render thread sends directly (no longer marshaled through the event
 * thread): rd resolution → send frame_done under ev_lock. cb resources
 * are destroyed inside the lock — ev_lock is recursive, listener
 * re-entry is safe. */
void awl_surface_presented(uint64_t id) {
    pthread_rwlock_rdlock(&g_srv.rwl);
    struct awl_surface* s = awl_surface_by_id(id);
    if (!s || !s->resource) {
        pthread_rwlock_unlock(&g_srv.rwl);
        return;
    }
    pthread_mutex_lock(&s->ev_lock);
    struct awl_frame_cb* cb;
    struct awl_frame_cb* tmp;
    wl_list_for_each_safe(cb, tmp, &s->frame_callbacks, link) {
        wl_callback_send_done(cb->resource, awl_now_ms());
        cb->detached = 1;
        wl_list_remove(&cb->link);
        wl_resource_destroy(cb->resource);   /* listener: detached → free */
    }
    /* The frame that sampled this batch of buffers has swapped — the client may safely overwrite (deferred release) */
    for (int i = 0; i < s->release_q_n; i++)
        wl_buffer_send_release(s->release_q[i]);
    s->release_q_n = 0;
    wl_client_flush(wl_resource_get_client(s->resource));
    pthread_mutex_unlock(&s->ev_lock);
    pthread_rwlock_unlock(&g_srv.rwl);
}

/* ---- Damage snapshot / consume (shm render path, see awl.h) ---- */

int awl_surface_get_damage(uint64_t id, int32_t* x, int32_t* y,
                           int32_t* w, int32_t* h, void** token, uint32_t* gen) {
    *x = *y = *w = *h = 0;
    *token = NULL;
    *gen = 0;
    pthread_rwlock_rdlock(&g_srv.rwl);
    struct awl_surface* s = awl_surface_by_id(id);
    if (!s) {
        pthread_rwlock_unlock(&g_srv.rwl);
        return AWL_DMG_NONE;
    }
    pthread_mutex_lock(&s->ev_lock);
    int st = s->cd_state;
    if (st == AWL_DMG_RECT) {
        /* surface-local → buffer px (damage_buffer at scale 1 is the same;
         * buf_scale is de-facto always 1 here — zoom goes through viewport) */
        int32_t bs = s->buf_scale > 1 ? s->buf_scale : 1;
        *x = s->cur_damage_x * bs;
        *y = s->cur_damage_y * bs;
        *w = s->cur_damage_w * bs;
        *h = s->cur_damage_h * bs;
    }
    *token = s->current_buffer_res;   /* the rect describes this buffer's content */
    *gen = s->cd_gen;
    pthread_mutex_unlock(&s->ev_lock);
    pthread_rwlock_unlock(&g_srv.rwl);
    return st;
}

void awl_surface_damage_consumed(uint64_t id, void* token, uint32_t gen) {
    pthread_rwlock_rdlock(&g_srv.rwl);
    struct awl_surface* s = awl_surface_by_id(id);
    if (!s) {
        pthread_rwlock_unlock(&g_srv.rwl);
        return;
    }
    pthread_mutex_lock(&s->ev_lock);
    /* clear only when still current: a buffer swap or a newer commit that
     * raced the upload keeps its damage for the next frame */
    if (s->current_buffer_res == token && s->cd_gen == gen)
        s->cd_state = AWL_DMG_NONE;
    pthread_mutex_unlock(&s->ev_lock);
    pthread_rwlock_unlock(&g_srv.rwl);
}
