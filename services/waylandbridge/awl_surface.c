/* awl_surface.c — wl_compositor / wl_surface (double-buffered) / wl_region (v2)
 *
 * commit flow: pending→current → first frame (window not yet created)
 * triggers window_created + migration of the client to a dedicated event
 * thread; each commit calls window_dirty directly (adapter implementations
 * must be thread safe: internally these are renderer requests with
 * built-in coalescing).
 *
 * Buffer hand-off (v3, awl_bufferqueue.h): a dmabuf commit does not hand
 * the renderer a pointer to "the current buffer" — it pushes the frame
 * (dup'd dmabuf fd + acquire fence) into the surface's queue and the
 * renderer pulls the newest complete one. wl_buffer.release and the
 * explicit-sync release go out when a frame leaves the queue (bq_release_cb
 * below), from whichever thread drained it: the render thread at frame
 * time, or this dispatch thread right at commit (opportunistic trylock
 * drain — over-speed clients get their buffers back without waiting for
 * vsync). A wl_shm commit is not queued and not copied: it only records the
 * source + accumulates damage; the backend reads the client's pool in place
 * at frame time (awl_surface_shm_begin/end, section at the end) and the
 * release goes out when that read is done.
 *
 * Locks: list topology = g_srv.rwl (dispatch thread wr, others rd);
 *        surface fields/sends = s->ev_lock (recursive); order rwl → ev_lock
 *        → g_bufref_lock (leaf, buffer liveness).
 */
#include "awl_internal.h"

#include <fcntl.h>
#include <string.h>
#include <unistd.h>   /* dup */

pthread_mutex_t g_bufref_lock = PTHREAD_MUTEX_INITIALIZER;

/* Renderer request invoked directly (no idle marshaling: the callback is
 * itself thread safe, the renderer side coalesces requests).
 * A subsurface has no window of its own — dirty the "root" (chain walk-up
 * under rwl, mutually exclusive with topology writes). Any thread (the shm
 * converter calls it too) with no logic-layer lock held. */
void awl_surface_schedule_render(struct awl_surface* s) {
    pthread_rwlock_rdlock(&g_srv.rwl);
    uint64_t root_id;
    if (s->role == AWL_ROLE_CURSOR) {
        /* cursor image (wl_pointer.set_cursor): no window of its own — redraw
         * the window currently compositing it (0 = not shown anywhere; its
         * queue still drains at every commit, see awl_surface_commit_drain) */
        root_id = awl_input_cursor_window(s);
    } else {
        root_id = awl_subsurface_root(s)->id;
    }
    pthread_rwlock_unlock(&g_srv.rwl);
    if (root_id && g_srv.cbs.window_dirty)
        g_srv.cbs.window_dirty(g_srv.cbs.user, root_id);
}
#define schedule_render awl_surface_schedule_render

/* ---------------- buffer wrappers / queue cookies ---------------- */

struct awl_buffer* awl_buffer_ref(struct awl_buffer* b) {
    if (b) atomic_fetch_add(&b->refs, 1);
    return b;
}

void awl_buffer_unref(struct awl_buffer* b) {
    if (!b || atomic_fetch_sub(&b->refs, 1) != 1) return;
    if (b->dmabuf_fd >= 0) close(b->dmabuf_fd);
    free(b);
}

void awl_surface_discard_sync(int acquire_fd, struct wl_resource* release_res) {
    if (acquire_fd >= 0) close(acquire_fd);
    if (release_res) {
        struct wl_client* c = wl_resource_get_client(release_res);
        pthread_mutex_lock(&g_bufref_lock);
        awl_esync_release_locked(release_res, -1);
        pthread_mutex_unlock(&g_bufref_lock);
        wl_client_flush(c);
    }
}

/* A frame left a surface's queue (drained by the render thread, by a commit,
 * or flushed at teardown) — hand the buffer back to its producer. Any
 * thread; the queue's head lock may be held by the caller (never re-enter
 * the queue here). e's fds are still open (the queue closes them after). */
static void bq_release_cb(struct awl_bq_buffer* e, void* ctx) {
    struct awl_bufref* r = e->user;
    (void)ctx;
    if (!r) return;   /* NULL-buffer marker */
    struct wl_client* c = NULL;
    pthread_mutex_lock(&g_bufref_lock);
    if (r->release_res) {
        /* explicit: the client waits on our render fence (fenced_release);
         * none = we never sampled it (immediate) */
        c = wl_resource_get_client(r->release_res);
        awl_esync_release_locked(r->release_res, e->release_fd);
    } else if (e->release_fd >= 0 && e->dmabuf_fd >= 0) {
        /* implicit: park our read fence in the dma-buf's reservation so the
         * client's next write (implicit-sync driver) waits for the composite
         * that sampled it — independent of what the GPU driver attaches */
        awl_dmabuf_import_sync_file(e->dmabuf_fd, e->release_fd, 1);
    }
    if (r->b && r->b->resource) {   /* still a live wl_buffer: protocol release (always, explicit or not) */
        wl_buffer_send_release(r->b->resource);
        c = wl_resource_get_client(r->b->resource);
    }
    if (c) wl_client_flush(c);
    pthread_mutex_unlock(&g_bufref_lock);
    awl_buffer_unref(r->b);
    free(r);
}

/* "We are done reading the shm source": wl_buffer.release for the buffer a
 * release is owed for + immediate_release of that commit's explicit-sync
 * object. Fired when a backend finished its upload, when the client
 * superseded / detached the buffer before any backend read it, and at
 * surface death. Caller holds ev_lock (shm_res is cleared under it by
 * awl_surface_buffer_gone, so a non-NULL value is a live resource). */
static void shm_release_locked(struct awl_surface* s) {
    if (s->shm_release_pending && s->shm_res) {
        wl_buffer_send_release(s->shm_res);
        wl_client_flush(wl_resource_get_client(s->shm_res));
    }
    s->shm_release_pending = 0;
    if (s->shm_release_res) {
        struct wl_resource* rr = s->shm_release_res;
        s->shm_release_res = NULL;
        awl_surface_discard_sync(-1, rr);
    }
}

/* NULL marker into the frame queue: "no dmabuf frame from here on" (detach,
 * or a shm commit after dmabuf ones). Unmapping must not be lost: a full
 * ring is flushed (bounded by one frame) and marked. Caller holds ev_lock. */
static void q_push_null(struct awl_surface* s) {
    struct awl_bq_buffer e;
    memset(&e, 0, sizeof(e));
    e.dmabuf_fd = e.acquire_fd = e.release_fd = -1;
    if (!awl_bufferqueue_push(s->q, &e)) {
        awl_bufferqueue_lock(s->q);
        awl_bufferqueue_flush(s->q);
        awl_bufferqueue_push(s->q, &e);
        awl_bufferqueue_unlock(s->q);
    }
    s->q_last_dmabuf = 0;
}

/* The double-buffered buffer state just became `res` (caller holds ev_lock).
 * dmabuf → one queue element (acquire = the client's explicit fence, else a
 * sync_file exported from the dma-buf's current write fences, else the
 * queue polls the dma-buf itself); shm → the source is recorded (+ a NULL
 * marker if the queue's head is a dmabuf frame); NULL → NULL marker.
 * A push into a full queue hands the buffer straight back (frame dropped —
 * the only way a client can get here is by outrunning a stalled/minimized
 * window with more buffers than the ring holds). */
void awl_surface_apply_buffer(struct awl_surface* s, struct wl_resource* res,
                              int acquire_fd, struct wl_resource* release_res) {
    if (!res) {
        awl_surface_discard_sync(acquire_fd, release_res);   /* commit_check already refused these; defensive */
        s->shm_live = 0;
        shm_release_locked(s);   /* an unread shm source goes back with the unmap */
        s->shm_res = NULL;
        q_push_null(s);
        return;
    }
    if (wl_shm_buffer_get(res)) {
        if (acquire_fd >= 0) close(acquire_fd);   /* unsupported_buffer was posted by commit_check */
        if (s->shm_res != res) {
            shm_release_locked(s);   /* superseded before any backend read it (detached window) */
        } else if (s->shm_release_res && s->shm_release_res != release_res) {
            /* same buffer re-committed before it was read: one wl_buffer.release
             * covers both cycles, the older release object is done now */
            struct wl_resource* rr = s->shm_release_res;
            s->shm_release_res = NULL;
            awl_surface_discard_sync(-1, rr);
        }
        s->shm_res = res;
        s->shm_release_pending = 1;
        s->shm_release_res = release_res;
        s->shm_live = 1;
        s->shm_serial++;
        if (s->q_last_dmabuf) q_push_null(s);   /* the consumer must drop the dmabuf head */
        return;
    }
    /* dmabuf: a shm source (if any) is superseded */
    s->shm_live = 0;
    shm_release_locked(s);
    s->shm_res = NULL;
    struct awl_buffer* b = wl_resource_get_user_data(res);
    if (!b || b->dmabuf_fd < 0) {
        awl_surface_discard_sync(acquire_fd, release_res);
        return;
    }
    struct awl_bufref* r = calloc(1, sizeof(*r));
    if (!r) {
        awl_surface_discard_sync(acquire_fd, release_res);
        return;
    }
    struct awl_bq_buffer e;
    memset(&e, 0, sizeof(e));
    e.dmabuf_fd = fcntl(b->dmabuf_fd, F_DUPFD_CLOEXEC, 0);
    e.acquire_fd = acquire_fd >= 0 ? acquire_fd : awl_dmabuf_export_sync_file(b->dmabuf_fd);
    e.release_fd = -1;
    e.ino = b->ino;
    e.width = b->width;
    e.height = b->height;
    e.stride = b->stride;
    e.format = b->drm_format;
    r->b = awl_buffer_ref(b);
    r->release_res = release_res;
    if (release_res) awl_esync_bind_ref(release_res, r);
    e.user = r;
    if (e.dmabuf_fd < 0 || !awl_bufferqueue_push(s->q, &e)) {
        LOGD("surface %llu: queue full — frame returned immediately", (unsigned long long)s->id);
        bq_release_cb(&e, NULL);
        if (e.dmabuf_fd >= 0) close(e.dmabuf_fd);
        if (e.acquire_fd >= 0) close(e.acquire_fd);
    } else {
        s->q_last_dmabuf = 1;
    }
}

void awl_surface_shm_damaged_locked(struct awl_surface* s) {
    if (s->shm_live) s->shm_serial++;   /* in-place redraw: the consumer re-uploads the rect */
}

void awl_surface_shm_release_gone(struct awl_surface* s, struct wl_resource* release_res) {
    pthread_mutex_lock(&s->ev_lock);
    if (s->shm_release_res == release_res) s->shm_release_res = NULL;
    pthread_mutex_unlock(&s->ev_lock);
}

/* Opportunistic drain at commit time (dispatch thread / shm converter, no
 * lock held): with the renderer idle we hand superseded frames back right
 * here — the client never waits for a frame to get its buffer. Skipped
 * while the root's Android window is detached: nothing will ever consume the
 * queue, so letting it fill starves the client of buffers = a minimized
 * window's client parks instead of spinning (throttle). Cursor images have
 * no window of their own and always drain (an unshown animated cursor must
 * not eat the theme's buffers). */
void awl_surface_commit_drain(struct awl_surface* s) {
    if (!s->q) return;
    int go;
    pthread_rwlock_rdlock(&g_srv.rwl);
    if (s->role == AWL_ROLE_CURSOR) go = 1;
    else go = atomic_load(&awl_subsurface_root(s)->attached);
    pthread_rwlock_unlock(&g_srv.rwl);
    if (!go) return;
    if (awl_bufferqueue_trylock(s->q)) {
        awl_bufferqueue_drain(s->q);
        awl_bufferqueue_arm(s->q);   /* next frame still on the GPU: release the head when it lands */
        awl_bufferqueue_unlock(s->q);
    }
}

/* Adapter → logic layer: the Android window of root `id` (renderer) came /
 * went. Any thread. */
void awl_window_attached(uint64_t id, int attached) {
    pthread_rwlock_rdlock(&g_srv.rwl);
    struct awl_surface* s = awl_surface_by_id(id);
    if (s) atomic_store(&s->attached, attached ? 1 : 0);
    pthread_rwlock_unlock(&g_srv.rwl);
}

struct awl_bufferqueue* awl_surface_queue_ref(uint64_t id) {
    pthread_rwlock_rdlock(&g_srv.rwl);
    struct awl_surface* s = awl_surface_by_id(id);
    struct awl_bufferqueue* q = s ? s->q : NULL;   /* immutable while the surface is listed */
    if (q) awl_bufferqueue_ref(q);
    pthread_rwlock_unlock(&g_srv.rwl);
    return q;
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

/* A wl_buffer resource is dying (shm: destroy listener below; dmabuf:
 * awl_dmabuf.c destroy handler): the client may destroy buffer + pool
 * without waiting for release (verified by relcross resize). Strip every
 * reference to it from each surface (pending/current/latched + the shm
 * source), otherwise a later commit dereferences a dangling resource.
 * Queued dmabuf frames are unaffected (an element owns its own fd dup + a
 * wrapper reference). A shm source's content persists in whatever a backend
 * last uploaded (shm_live stays; kwin likewise keeps showing a destroyed
 * shm buffer). Runs on that client's dispatch thread. */
void awl_surface_buffer_gone(struct wl_resource* res) {
    pthread_rwlock_rdlock(&g_srv.rwl);
    struct awl_surface* s;
    wl_list_for_each(s, &g_srv.surfaces, link) {
        if (s->pending_buffer_res != res && s->current_buffer_res != res &&
            !(s->role == AWL_ROLE_SUBSURFACE && s->u.sub.latched_buffer_res == res) &&
            s->shm_res != res)
            continue;   /* unrelated window: skip taking ev_lock (commit unaffected) */
        pthread_mutex_lock(&s->ev_lock);
        if (s->pending_buffer_res == res) s->pending_buffer_res = NULL;
        if (s->current_buffer_res == res) s->current_buffer_res = NULL;
        /* union branch gate: latched_* only exists while role == SUBSURFACE */
        if (s->role == AWL_ROLE_SUBSURFACE && s->u.sub.latched_buffer_res == res) {
            s->u.sub.latched_buffer_res = NULL;
            s->latched_attach = 0;
            s->sub_latched = 0;
            if (s->u.sub.latched_acquire_fd >= 0) { close(s->u.sub.latched_acquire_fd); s->u.sub.latched_acquire_fd = -1; }
            /* the latched release object stays: it is delivered (immediate) when the latch resolves */
        }
        if (s->shm_res == res) {
            /* nothing left to release to; the commit's release object is
             * delivered now (its buffer cannot be read any more) */
            s->shm_res = NULL;
            s->shm_release_pending = 0;
            if (s->shm_release_res) {
                struct wl_resource* rr = s->shm_release_res;
                s->shm_release_res = NULL;
                awl_surface_discard_sync(-1, rr);
            }
        }
        pthread_mutex_unlock(&s->ev_lock);
    }
    pthread_rwlock_unlock(&g_srv.rwl);
}

static void shm_buffer_gone(struct wl_listener* l, void* data) {
    free(l);   /* watcher self-recycles when the resource dies (removes itself within emit's safe iteration) */
    awl_surface_buffer_gone(data);
}

/* wl_callback(frame) resource destroyed → removed from the surface list.
 * Resource-map mutation is dispatch-thread-only.  The render thread only
 * marks a callback delivered; surface_frame() retires it on the client's
 * next request (or surface teardown retires it during client destruction). */
static void frame_cb_res_destroy(struct wl_resource* res) {
    struct awl_frame_cb* cb = wl_resource_get_user_data(res);
    if (cb) {
        struct awl_surface* s = cb->s;
        pthread_mutex_lock(&s->ev_lock);
        wl_list_remove(&cb->link);
        pthread_mutex_unlock(&s->ev_lock);
        free(cb);
    }
}

/* ---------------- wl_region (ordered rectangle ops, not part of layout) ----------------
 * A region is the SEQUENCE of add/subtract rectangles the client issued —
 * no region algebra: a point test replays the list and the last rectangle
 * covering the point decides (add = inside, subtract = outside). That is
 * exact for every wl_region a client can build, costs O(ops) per test, and
 * the lists are tiny in practice (Firefox: one add, or nothing at all for
 * the "no input" render child — #85). Consumers never hold the wl_region
 * resource: pointer constraints read a bounding box at request time
 * (awl_region_bbox), surface regions take a deep copy (awl_region_snapshot)
 * that the commit promotes. Empty rectangles are dropped on entry. */
struct awl_region_op {
    int32_t x, y, w, h;
    bool add;   /* 0 = subtract */
};
struct awl_region {
    struct awl_region_op* ops;
    int n, cap;
};

static void region_res_destroy(struct wl_resource* res) {
    awl_region_free(wl_resource_get_user_data(res));
}
static void region_destroy(struct wl_client* client, struct wl_resource* res) {
    wl_resource_destroy(res);
}
static void region_op(struct wl_resource* res, int32_t x, int32_t y,
                      int32_t w, int32_t h, bool add) {
    struct awl_region* r = wl_resource_get_user_data(res);
    if (!r || w <= 0 || h <= 0) return;
    if (!add && r->n == 0) return;   /* subtracting from nothing changes nothing */
    if (r->n == r->cap) {
        int cap = r->cap ? r->cap * 2 : 4;
        struct awl_region_op* ops = realloc(r->ops, (size_t)cap * sizeof(*ops));
        if (!ops) { wl_resource_post_no_memory(res); return; }
        r->ops = ops;
        r->cap = cap;
    }
    r->ops[r->n++] = (struct awl_region_op){ x, y, w, h, add };
}
static void region_add(struct wl_client* client, struct wl_resource* res,
                       int32_t x, int32_t y, int32_t w, int32_t h) {
    region_op(res, x, y, w, h, true);
}
static void region_subtract(struct wl_client* client, struct wl_resource* res,
                            int32_t x, int32_t y, int32_t w, int32_t h) {
    region_op(res, x, y, w, h, false);
}

static const struct wl_region_interface region_iface = {
    .destroy = region_destroy,
    .add = region_add,
    .subtract = region_subtract,
};

int awl_region_bbox(struct wl_resource* region, int32_t* x, int32_t* y,
                    int32_t* w, int32_t* h) {
    struct awl_region* r = region ? wl_resource_get_user_data(region) : NULL;
    if (!r) return 0;
    int set = 0;
    int32_t x1 = 0, y1 = 0, x2 = 0, y2 = 0;
    for (int i = 0; i < r->n; i++) {
        const struct awl_region_op* o = &r->ops[i];
        if (!o->add) continue;
        if (!set) {
            x1 = o->x; y1 = o->y; x2 = o->x + o->w; y2 = o->y + o->h;
            set = 1;
            continue;
        }
        if (o->x < x1) x1 = o->x;
        if (o->y < y1) y1 = o->y;
        if (o->x + o->w > x2) x2 = o->x + o->w;
        if (o->y + o->h > y2) y2 = o->y + o->h;
    }
    if (!set) return 0;
    *x = x1; *y = y1; *w = x2 - x1; *h = y2 - y1;
    return 1;
}

struct awl_region* awl_region_snapshot(struct wl_resource* region) {
    struct awl_region* src = region ? wl_resource_get_user_data(region) : NULL;
    if (!src) return NULL;
    struct awl_region* r = calloc(1, sizeof(*r));
    if (!r) return NULL;
    if (src->n) {
        r->ops = malloc((size_t)src->n * sizeof(*r->ops));
        if (!r->ops) { free(r); return NULL; }
        memcpy(r->ops, src->ops, (size_t)src->n * sizeof(*r->ops));
        r->n = r->cap = src->n;
    }
    return r;
}

void awl_region_free(struct awl_region* r) {
    if (!r) return;
    free(r->ops);
    free(r);
}

int awl_region_contains(const struct awl_region* r, int32_t x, int32_t y) {
    if (!r) return 0;
    for (int i = r->n - 1; i >= 0; i--) {
        const struct awl_region_op* o = &r->ops[i];
        if (x >= o->x && y >= o->y && x < o->x + o->w && y < o->y + o->h)
            return o->add;
    }
    return 0;
}

int awl_surface_accepts_input(struct awl_surface* s, float x, float y) {
    pthread_mutex_lock(&s->ev_lock);
    /* x,y are layer-local and non-negative here (the caller already bounded
     * them by the layer size), so truncation == floor */
    int hit = !s->input_region ||
              awl_region_contains(s->input_region, (int32_t)x, (int32_t)y);
    pthread_mutex_unlock(&s->ev_lock);
    return hit;
}

/* Move a snapshot between slots: the destination's old contents die. */
static void region_move(struct awl_region** dst, struct awl_region** src) {
    awl_region_free(*dst);
    *dst = *src;
    *src = NULL;
}

void awl_surface_apply_regions_locked(struct awl_surface* s) {
    if (s->pend_input) {
        region_move(&s->input_region, &s->pend_input_region);
        s->pend_input = 0;
    }
    if (s->pend_opaque) {
        region_move(&s->opaque_region, &s->pend_opaque_region);
        s->pend_opaque = 0;
    }
}

/* ---------------- wl_surface ---------------- */

static void surface_destroy_impl(struct wl_resource* res) {
    struct awl_surface* s = wl_resource_get_user_data(res);
    if (!s) return;

    LOGI("surface %llu destroyed (mapped=%d)",
            (unsigned long long)s->id, s->mapped);

    /* The window this surface created (if the role object did not already
     * take it down — awl_xdg.c toplevel_res_destroy): backend detach (joins
     * the GL render thread / retires the SCs — its snapshots hold rd; only
     * once they finish naturally can the wrlock be acquired → nothing in
     * flight, teardown is safe) + the Activity CLOSE + the lifecycle event.
     * Keyed on window_live, never on the role: by the time a client destroys
     * the wl_surface its xdg_toplevel is usually gone (role NONE) — and a
     * cursor/subsurface never had a window (mapped=1 says nothing). */
    if (s->window_live && g_srv.cbs.window_destroyed) {
        s->window_live = 0;
        g_srv.cbs.window_destroyed(g_srv.cbs.user, s->id);
    }

    /* Frame stream: hand every queued frame back (release events go out
     * here, on the dispatch thread). Waits for a render thread that still
     * has this layer's queue locked (a child layer of a live window) — that
     * frame ends without rwl, so no lock is held here yet. Producers are
     * gone (converter detached, this thread is the committer), so the queue
     * stays empty; the renderer's own reference keeps the object alive
     * until its frame ends. */
    if (s->q) {
        awl_bufferqueue_lock(s->q);
        awl_bufferqueue_flush(s->q);
        awl_bufferqueue_unlock(s->q);
    }
    /* shm source: the release still owed for its buffer goes out with the
     * surface (the client outlives it; kwin drops the buffer reference at
     * surface death). A backend mid-upload holds ev_lock — wait it out. */
    pthread_mutex_lock(&s->ev_lock);
    s->shm_live = 0;
    shm_release_locked(s);
    s->shm_res = NULL;
    pthread_mutex_unlock(&s->ev_lock);

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
     * the later destroy handlers lock the already-freed ev_lock.
     * The role-object pointers live in the union: each is non-NULL only
     * while role == its branch (the role-object destroy handlers clear
     * them before role → NONE), so gate each read on role. xdg_surface_res
     * is a plain field precisely because it stays set through the
     * role == NONE window (toplevel dead, xdg_surface object alive). */
    if (s->xdg_surface_res)
        wl_resource_set_user_data(s->xdg_surface_res, NULL);
    if (s->role == AWL_ROLE_TOPLEVEL || s->role == AWL_ROLE_POPUP) {
        /* role == xdg ⇒ role_res is live: the role-object destroy handlers
         * clear it BEFORE role → NONE (and role flips run on this same
         * dispatch thread) — the old NULL guard was unreachable */
        AWL_ASSERT(s->u.xdg.role_res);
        wl_resource_set_user_data(s->u.xdg.role_res, NULL);
    } else if (s->role == AWL_ROLE_SUBSURFACE) {
        AWL_ASSERT(s->u.sub.subsurface_res);   /* same invariant */
        wl_resource_set_user_data(s->u.sub.subsurface_res, NULL);
    } else if (s->role == AWL_ROLE_XWAYLAND) {
        /* NOT an over-guard, unlike the two above: xsurf_res_destroy clears
         * u.xway.res but the XWAYLAND role is permanent (never → NONE), so
         * role == XWAYLAND with res == NULL is a normal long-lived state */
        if (s->u.xway.res) {
            wl_resource_set_user_data(s->u.xway.res, NULL);
            s->u.xway.res = NULL;
        }
    }
    if (s->viewport_res)   /* #31: viewport handler holds a surface pointer — break the link */
        wl_resource_set_user_data(s->viewport_res, NULL);
    if (s->frac_res)       /* fractional_scale handler does not touch surface, just clear the flag */
        s->frac_res = NULL;

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
        awl_subsurface_unlink_locked(s);
    }
    if (s->sub_latched && s->latched_attach) {   /* latched buffer never presented — release directly */
        latched_drop = s->u.sub.latched_buffer_res;
        s->u.sub.latched_buffer_res = NULL;
    }
    s->sub_latched = 0;
    s->latched_attach = 0;
    /* explicit-sync objects die with the surface (queue already flushed →
     * every delivered release object is parked in esync_gc; the rest get an
     * immediate_release) */
    awl_esync_surface_gone(s);
    {
        struct awl_surface* ch;
        struct awl_surface* ctmp;
        wl_list_for_each_safe(ch, ctmp, &s->sub_children, sub_child_link) {
            awl_subsurface_unlink_locked(ch);
        }
    }
    wl_list_remove(&s->link);
    struct awl_bufferqueue* q = s->q;   /* renderer may still hold its own ref: unref after unlink */
    s->q = NULL;
    free(s->title);
    awl_region_free(s->input_region);
    awl_region_free(s->pend_input_region);
    awl_region_free(s->opaque_region);
    awl_region_free(s->pend_opaque_region);
    pthread_mutex_destroy(&s->ev_lock);
    free(s);
    pthread_rwlock_unlock(&g_srv.rwl);
    awl_bufferqueue_unref(q);
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
}

/* set_input_region / set_opaque_region share one shape: snapshot the
 * wl_region NOW (protocol: the region object may be destroyed right after,
 * its contents must not change what was requested), park the copy in the
 * pending slot; NULL region = reset to the protocol default (a NULL
 * snapshot). Returns 0 = allocation failed (no_memory posted, request
 * dropped). The pending flag itself is set by the caller — W1 word, dispatch
 * thread only. */
static int region_pend(struct awl_surface* s, struct wl_resource* surface_res,
                       struct wl_resource* region, struct awl_region** slot) {
    struct awl_region* snap = awl_region_snapshot(region);
    if (region && !snap) {
        wl_resource_post_no_memory(surface_res);
        return 0;
    }
    pthread_mutex_lock(&s->ev_lock);
    region_move(slot, &snap);
    pthread_mutex_unlock(&s->ev_lock);
    return 1;
}
/* Opaque region: recorded as protocol state (double-buffered, applied on
 * commit); no backend consumes it yet — both renderers blend every layer. */
static void surface_set_opaque_region(struct wl_client* c, struct wl_resource* r,
                                      struct wl_resource* region) {
    struct awl_surface* s = wl_resource_get_user_data(r);
    if (s && region_pend(s, r, region, &s->pend_opaque_region))
        s->pend_opaque = 1;
}
/* Input region (#85): consumed by awl_subsurface_hit through
 * awl_surface_accepts_input once the commit promoted it. */
static void surface_set_input_region(struct wl_client* c, struct wl_resource* r,
                                     struct wl_resource* region) {
    struct awl_surface* s = wl_resource_get_user_data(r);
    if (s && region_pend(s, r, region, &s->pend_input_region))
        s->pend_input = 1;
}
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

    /* wl_resource_destroy mutates the client's object map and is unsafe from
     * the render thread while this dispatch thread may be creating another
     * callback with a freshly reusable id.  Retire callbacks whose done was
     * sent by awl_surface_presented() here, before creating the replacement.
     * The client allocates a different id until delete_id is emitted, so the
     * one-request delay is protocol-safe and bounds the parked set. */
    pthread_mutex_lock(&s->ev_lock);
    struct awl_frame_cb* old;
    struct awl_frame_cb* old_tmp;
    wl_list_for_each_safe(old, old_tmp, &s->frame_callbacks, link) {
        if (old->detached)
            wl_resource_destroy(old->resource); /* recursive ev_lock listener */
    }
    pthread_mutex_unlock(&s->ev_lock);

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

    /* explicit sync: retire delivered release objects (dispatch thread), then
     * validate this cycle's fence/release against the attached buffer */
    awl_esync_gc(s);
    if (awl_esync_commit_check(s))
        return;

    /* KWin alignment: commit on an effective sync child layer latches — state not applied, waits for the parent commit */
    if (awl_subsurface_maybe_latch(s))
        return;

    /* Apply buffer state — only touch current if attached this cycle
     * (protocol: pending persists across commits; the empty commit for
     * ack_configure must not detach). The presented frame goes into the
     * surface's queue (awl_surface_apply_buffer): the render thread pulls
     * from there and the buffer is handed back when its element leaves the
     * queue — never here. ev_lock: the converter / layer snapshots read the
     * current_buffer_res-derived size concurrently */
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
    awl_surface_apply_regions_locked(s);   /* set_input_region / set_opaque_region (#85) */
    int attached = s->pending_attached;
    int32_t off_x = 0, off_y = 0;   /* attach dx,dy of this cycle (cursor role: moves the hotspot) */
    int acquire_fd = -1;
    struct wl_resource* release_res = NULL;
    if (s->pending_attached) {
        s->current_buffer_res = s->pending_buffer_res;
        s->pending_buffer_res = NULL;
        s->pending_attached = 0;
        off_x = s->pending_offset_x;
        off_y = s->pending_offset_y;
        s->pending_offset_x = s->pending_offset_y = 0;   /* consumed with the attach */
        acquire_fd = s->pend_acquire_fd;                 /* explicit-sync state rides with the attach */
        s->pend_acquire_fd = -1;
        release_res = s->pend_release_res;
        s->pend_release_res = NULL;
    }
    /* damage applies on EVERY commit (not just attach-commits): damage+commit
     * without re-attach is the standard in-place redraw pattern */
    awl_damage_merge_pending(s, attached);
    if (attached)
        awl_surface_apply_buffer(s, s->current_buffer_res, acquire_fd, release_res);
    else if (s->cd_state != AWL_DMG_NONE)
        awl_surface_shm_damaged_locked(s);   /* in-place redraw of a shm source: re-upload the rect */
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
    pthread_mutex_unlock(&s->ev_lock);

    /* mailbox: superseded frames the renderer never got to go back to the
     * client right now (trylock — never wait for a frame in flight;
     * skipped while the window is detached, see awl_surface_commit_drain) */
    if (attached) awl_surface_commit_drain(s);

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
        /* child state changed → root window redraw (walk-up inside); a buffer
         * detached (attach NULL) → the NULL marker must reach the screen: the
         * window drops the layer */
        if (children_applied || attached)
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
            s->window_live = 1;   /* window_destroyed owed: role death or surface death, whichever first */
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

/* Toplevel title storage: heap, owned by the surface (client dispatch
 * thread only — set_title / role setup / destroy; the render thread never
 * reads it). Full string kept, no truncation; empty/NULL and strdup failure
 * all mean "no title" (consumers treat NULL like ""). */
void awl_surface_set_title(struct awl_surface* s, const char* title) {
    free(s->title);
    s->title = (title && title[0]) ? strdup(title) : NULL;
}

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
    s->pend_acquire_fd = -1;
    /* u.sub.latched_acquire_fd: initialized at role assignment (get_subsurface) */
    wl_list_init(&s->esync_all);
    wl_list_init(&s->esync_gc);
    /* frame stream: created up front so the pointer is immutable for the
     * surface's listed lifetime (the renderer resolves it under rwl.rd) */
    s->q = awl_bufferqueue_create(bq_release_cb, NULL);
    if (!s->q) { free(s); wl_resource_destroy(sres); wl_resource_post_no_memory(res); return; }
    {
        pthread_mutexattr_t attr;
        pthread_mutexattr_init(&attr);
        pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(&s->ev_lock, &attr);   /* recursive: presented destroys cb while holding the lock */
        pthread_mutexattr_destroy(&attr);
    }
    wl_list_init(&s->frame_callbacks);
    wl_list_init(&s->sub_children);
    wl_list_init(&s->sub_below);
    wl_list_init(&s->sub_above);
    wl_list_init(&s->pend_sub_below);
    wl_list_init(&s->pend_sub_above);
    wl_list_init(&s->sub_child_link);
    wl_list_init(&s->sub_link);
    wl_list_init(&s->sub_pend_link);
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
    struct awl_region* r = calloc(1, sizeof(*r));
    if (!r) { wl_resource_destroy(rres); wl_resource_post_no_memory(res); return; }
    wl_resource_set_implementation(rres, &region_iface, r, region_res_destroy);
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
    if (!wl_global_create(g_srv.display,
                          &wl_compositor_interface, 4,
                          NULL, compositor_bind))
        LOGE("wl_compositor global create failed");
}

/* ---- adapter render-pull interfaces (called on the render thread) ----
 * The frame itself comes through the surface's queue (awl_surface_queue_ref
 * above + awl_bufferqueue.h); the snapshots below are geometry only. */

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

/* ---- wl_shm frame source (awl.h) ----
 * begin resolves the surface (rwl.rd) and takes its ev_lock; both stay held
 * until end so the buffer resource, the pool mapping and the damage words
 * are stable across the upload. That is the same rwl.rd every render-side
 * snapshot holds (writers = create/destroy wait a copy's worth) and the
 * ev_lock this surface's commit takes (its client waits — kwin copies shm
 * synchronously at commit time on the main thread; here it is the backend's
 * frame time instead). */
static uint32_t shm_fourcc(uint32_t wl_fmt) {
    /* wl_shm.format: ARGB8888 = 0 and XRGB8888 = 1 are the only codes that
     * differ from the DRM fourcc; every other value IS the fourcc */
    if (wl_fmt == WL_SHM_FORMAT_ARGB8888) return AWL_FORMAT_ARGB8888;
    if (wl_fmt == WL_SHM_FORMAT_XRGB8888) return AWL_FORMAT_XRGB8888;
    return wl_fmt;
}

int awl_surface_shm_begin(uint64_t id, uint64_t have_serial, awl_shm_frame_t* f) {
    pthread_rwlock_rdlock(&g_srv.rwl);
    struct awl_surface* s = awl_surface_by_id(id);
    if (!s) {
        pthread_rwlock_unlock(&g_srv.rwl);
        return 0;
    }
    pthread_mutex_lock(&s->ev_lock);
    if (!s->shm_live) {
        pthread_mutex_unlock(&s->ev_lock);
        pthread_rwlock_unlock(&g_srv.rwl);
        return 0;
    }
    struct wl_shm_buffer* shm =
        s->current_buffer_res ? wl_shm_buffer_get(s->current_buffer_res) : NULL;
    if (!shm || s->shm_serial == have_serial) {   /* nothing new / buffer already destroyed */
        pthread_mutex_unlock(&s->ev_lock);
        pthread_rwlock_unlock(&g_srv.rwl);
        return 2;
    }
    wl_shm_buffer_begin_access(shm);
    f->width = (uint32_t)wl_shm_buffer_get_width(shm);
    f->height = (uint32_t)wl_shm_buffer_get_height(shm);
    f->stride = (uint32_t)wl_shm_buffer_get_stride(shm);
    f->format = shm_fourcc(wl_shm_buffer_get_format(shm));
    f->pixels = wl_shm_buffer_get_data(shm);
    f->dmg_full = s->cd_state == AWL_DMG_FULL;
    if (s->cd_state == AWL_DMG_RECT) {
        f->dmg_x = s->cur_damage_x; f->dmg_y = s->cur_damage_y;
        f->dmg_w = s->cur_damage_w; f->dmg_h = s->cur_damage_h;
    } else {
        f->dmg_x = f->dmg_y = f->dmg_w = f->dmg_h = 0;
    }
    f->serial = s->shm_serial;
    f->priv = s;
    return 1;   /* rwl.rd + ev_lock held until end */
}

void awl_surface_shm_end(awl_shm_frame_t* f, int consumed) {
    struct awl_surface* s = f->priv;
    struct wl_shm_buffer* shm =
        s->current_buffer_res ? wl_shm_buffer_get(s->current_buffer_res) : NULL;
    if (shm) wl_shm_buffer_end_access(shm);   /* unchanged since begin: both locks held */
    if (consumed) {
        s->cd_state = AWL_DMG_NONE;   /* the consumer holds the pixels now */
        shm_release_locked(s);        /* wl_buffer.release: done reading */
    }
    pthread_mutex_unlock(&s->ev_lock);
    pthread_rwlock_unlock(&g_srv.rwl);
    f->priv = NULL;
    f->pixels = NULL;
}

/* Render thread sends done directly under ev_lock, but does not destroy the
 * callback resource here: wl_resource_destroy mutates the client object map
 * and can race its dispatch thread creating the next callback.  Delivered
 * callbacks are parked in the list and retired by surface_frame() on that
 * dispatch thread (or by surface teardown). */
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
        if (cb->detached) continue;   /* done sent; waiting for dispatch-thread GC */
        wl_callback_send_done(cb->resource, awl_now_ms());
        cb->detached = 1;
    }
    /* (buffer releases are not tied to presentation any more: they go out
     * when the frame leaves the surface's queue — bq_release_cb) */
    wl_client_flush(wl_resource_get_client(s->resource));
    pthread_mutex_unlock(&s->ev_lock);
    pthread_rwlock_unlock(&g_srv.rwl);
}
