/* awl_subsurface.c — wl_subcompositor / wl_subsurface (core protocol, generator symbols)
 *
 * Motivation (2026-09-09 chrome SIGSEGV root cause): chrome's WaylandBubble
 * (tooltip / touch selection handles / drag hint) and WaylandSubsurface (overlay/
 * main content) attach to the parent window via wl_subsurface; when the registry
 * lacks the wl_subcompositor global, chrome release builds call inline marshal on
 * a NULL proxy → crash.
 * GTK4 popovers also go through subsurfaces.
 *
 * Semantics aligned with kwin-6.6.5 (src/wayland/subcompositor.cpp + surface.cpp +
 * transaction machinery, 2026-09-09 window flicker post-mortem):
 *   - A child layer's commit under "effective sync" is latched; the parent commit
 *     applies it in cascade — chrome's overlay (WaylandSubsurface::CreateSubsurface
 *     calls wl_subsurface_set_sync) relies on this atomicity; applying it
 *     immediately would tear its frame sequence from the parent's (flicker).
 *   - set_position / place_* are double-buffered to take effect on parent commit
 *     (parentApplyState; chrome's SetSubsurfacePosition already ends with an
 *     explicit commit of the parent surface).
 *   - Effective sync recurses along the ancestor chain
 *     (SubSurfaceInterface::isSynchronized).
 *   - set_desync immediately flushes the latched state of itself and of
 *     descendants whose effective sync has been released (parentDesynchronized →
 *     transaction->commit).
 *   - Release of a replaced buffer is deferred until "after the frame being
 *     sampled is presented" (KWin GraphicsBuffer reference semantics, see
 *     awl_surface.c release_q) — a client (desync bubble swapping frames at a
 *     high rate) overwriting a dmabuf under sampling is another source of
 *     tearing/flicker.
 *
 * Composition: child surface commit → dirty the "root" window (child layers get
 * no Activity); the render side snapshots via awl_surface_get_layers then
 * composites the layers on the GPU (awl_renderer.cpp).
 *
 * Locks: topology (sub_parent / sub_children render stack order) = g_srv.rwl
 * (readers and writers alike); sub_x / sub_y / latched / release_q = child
 * surface ev_lock. The sub_sync / sub_latched flags are read/written only by the
 * client dispatch thread (a wl_subsurface tree is always one client) — no lock.
 * Order: rwl → ev_lock (consistent with the existing layering).
 */
#include "awl_internal.h"

#include <string.h>

/* Caller holds rwl (rd or wr). Walk up the parent chain to the root (depth guard). */
struct awl_surface* awl_subsurface_root(struct awl_surface* s) {
    int d = 0;
    while (s->sub_parent && d++ < 32) s = s->sub_parent;
    return s;
}

/* Effective sync: self or any ancestor is in sync mode (called only from the
 * client dispatch thread — subtree is always one client, no cross-thread access).
 * KWin SubSurfaceInterface::isSynchronized */
static int sub_effective_sync(struct awl_surface* s) {
    for (int d = 0; s && d < 32; s = s->sub_parent, d++)
        if (s->sub_sync) return 1;
    return 0;
}

/* surface_commit entry: a commit of an effective-sync child layer is latched
 * (KWin subsurface.transaction). Return 1 = handled, caller returns directly. */
int awl_subsurface_maybe_latch(struct awl_surface* s) {
    if (s->role != AWL_ROLE_SUBSURFACE || !sub_effective_sync(s))
        return 0;
    pthread_mutex_lock(&s->ev_lock);
    struct wl_resource* drop = NULL;
    if (s->pending_attached) {   /* latch buffer state only for cycles that attached */
        if (s->sub_latched && s->latched_attach && s->latched_buffer_res &&
            s->latched_buffer_res != s->pending_buffer_res)
            drop = s->latched_buffer_res;   /* superseded by a newer latch, never presented */
        s->latched_buffer_res = s->pending_buffer_res;
        s->pending_buffer_res = NULL;
        s->pending_attached = 0;
        s->latched_attach = 1;
    }
    s->sub_latched = 1;
    /* damage stays pending and keeps accumulating; moved to current on apply */
    pthread_mutex_unlock(&s->ev_lock);
    if (drop && drop != s->current_buffer_res)
        wl_buffer_send_release(drop);   /* never sampled — release immediately */
    return 1;
}

/* Apply s's latched state (caller = client dispatch thread). */
static void sub_apply_state(struct awl_surface* ch) {
    pthread_mutex_lock(&ch->ev_lock);
    if (ch->latched_attach) {
        struct wl_resource* old = ch->current_buffer_res;
        ch->current_buffer_res = ch->latched_buffer_res;
        if (old && old != ch->current_buffer_res) {
            if (wl_shm_buffer_get(old))
                wl_buffer_send_release(old);
            else
                awl_surface_release_defer(ch, old);
        }
    }
    ch->latched_buffer_res = NULL;
    int had_attach = ch->latched_attach;
    ch->latched_attach = 0;
    ch->sub_latched = 0;
    /* latched state applies now — its damage with it (also covers a latched
     * damage-only commit: no attach, pd accumulated, function's empty-check
     * handles the "nothing changed" case) */
    awl_damage_merge_pending(ch, had_attach);
    pthread_mutex_unlock(&ch->ev_lock);
}

/* s's state was just applied (any commit): child double-buffered positions take
 * effect; latched state of effective-sync children applies in cascade — children
 * that applied keep descending into their own children (KWin: the parent
 * transaction merges direct child transactions; a desync child commit merges its
 * own child transactions immediately).
 * Return 1 = some child layer's buffer state was applied (caller uses this to
 * decide whether to trigger presentation). */
int awl_subsurface_parent_applied(struct awl_surface* s) {
    int applied = 0;
    struct awl_surface* ch;
    wl_list_for_each(ch, &s->sub_children, sub_link) {
        pthread_mutex_lock(&ch->ev_lock);
        int apply = ch->sub_latched;
        if (ch->sub_pos_pending) {          /* KWin parentApplyState */
            ch->sub_x = ch->pend_sub_x;
            ch->sub_y = ch->pend_sub_y;
            ch->sub_pos_pending = 0;
            applied = 1;   /* layer position change also needs redraw */
        }
        pthread_mutex_unlock(&ch->ev_lock);
        if (apply) {
            sub_apply_state(ch);
            applied = 1;
            applied |= awl_subsurface_parent_applied(ch);
        }
    }
    return applied;
}

/* Topology changed (rwl already unlocked) → root window redraw. Caller: protocol dispatch thread. */
static void dirty_root(struct awl_surface* s) {
    pthread_rwlock_rdlock(&g_srv.rwl);
    struct awl_surface* root = awl_subsurface_root(s);
    uint64_t id = root->id;
    int mapped = root->mapped;
    pthread_rwlock_unlock(&g_srv.rwl);
    if (mapped && g_srv.cbs.window_dirty)
        g_srv.cbs.window_dirty(g_srv.cbs.user, id);
}

/* ---------------- wl_subsurface ---------------- */

static void sub_destroy(struct wl_client* client, struct wl_resource* res) {
    wl_resource_destroy(res);
}

/* wl_subsurface object destruction: unlink the parent-child relation; the surface immediately leaves composition (protocol semantics) */
static void sub_res_destroy(struct wl_resource* res) {
    struct awl_surface* s = wl_resource_get_user_data(res);
    if (!s) return;   /* wl_surface died first (surface_destroy_impl already unlinked it) */
    uint64_t root_id = 0;
    int dirty = 0;
    pthread_rwlock_wrlock(&g_srv.rwl);
    s->subsurface_res = NULL;
    if (s->sub_parent) {
        struct awl_surface* root = awl_subsurface_root(s);
        root_id = root->id;
        dirty = root->mapped;
        wl_list_remove(&s->sub_link);
        s->sub_parent = NULL;
    }
    s->role = AWL_ROLE_NONE;
    pthread_rwlock_unlock(&g_srv.rwl);
    /* Latched state was never presented — release directly; pending position voided */
    struct wl_resource* latched_drop = NULL;
    pthread_mutex_lock(&s->ev_lock);
    if (s->sub_latched && s->latched_attach)
        latched_drop = s->latched_buffer_res;
    s->latched_buffer_res = NULL;
    s->sub_latched = 0;
    s->latched_attach = 0;
    s->sub_pos_pending = 0;
    pthread_mutex_unlock(&s->ev_lock);
    if (latched_drop)
        wl_buffer_send_release(latched_drop);
    LOGI("surface %llu un-role subsurface", (unsigned long long)s->id);
    if (dirty && g_srv.cbs.window_dirty)
        g_srv.cbs.window_dirty(g_srv.cbs.user, root_id);
}

/* Double-buffered: takes effect on parent commit (KWin: stored in parent
 * pending, applied by parentApplyState). chrome WaylandSubsurface/WaylandBubble's
 * SetSubsurfacePosition ends with an explicit commit of the parent surface, so
 * there is no need to trigger presentation here. */
static void sub_set_position(struct wl_client* client, struct wl_resource* res,
                             int32_t x, int32_t y) {
    struct awl_surface* s = wl_resource_get_user_data(res);
    if (!s || !s->sub_parent) return;
    pthread_mutex_lock(&s->ev_lock);
    s->pend_sub_x = x;
    s->pend_sub_y = y;
    s->sub_pos_pending = 1;
    pthread_mutex_unlock(&s->ev_lock);
}

/* Only same-parent siblings accepted; cross-parent silently ignored (protocol has no matching error enum, log it) */
static void sub_place_above(struct wl_client* client, struct wl_resource* res,
                            struct wl_resource* sibling_res) {
    struct awl_surface* s = wl_resource_get_user_data(res);
    struct awl_surface* sib = wl_resource_get_user_data(sibling_res);
    if (!s || !sib || s == sib || !s->sub_parent ||
        sib->sub_parent != s->sub_parent) {
        LOGI("place_above: non-sibling ignored");
        return;
    }
    pthread_rwlock_wrlock(&g_srv.rwl);
    wl_list_remove(&s->sub_link);
    wl_list_insert(&sib->sub_link, &s->sub_link);   /* s right after sib = above it */
    pthread_rwlock_unlock(&g_srv.rwl);
    dirty_root(s);
}

static void sub_place_below(struct wl_client* client, struct wl_resource* res,
                            struct wl_resource* sibling_res) {
    struct awl_surface* s = wl_resource_get_user_data(res);
    struct awl_surface* sib = wl_resource_get_user_data(sibling_res);
    if (!s || !sib || s == sib || !s->sub_parent ||
        sib->sub_parent != s->sub_parent) {
        LOGI("place_below: non-sibling ignored");
        return;
    }
    pthread_rwlock_wrlock(&g_srv.rwl);
    wl_list_remove(&s->sub_link);
    wl_list_insert(sib->sub_link.prev, &s->sub_link);   /* s right before sib = below it */
    pthread_rwlock_unlock(&g_srv.rwl);
    dirty_root(s);
}

/* set_desync cascade flush: the latched state of self + descendants whose
 * effective sync has been released applies immediately (KWin parentDesynchronized → transaction->commit) */
static void sub_desync_flush(struct awl_surface* s) {
    struct awl_surface* ch;
    wl_list_for_each(ch, &s->sub_children, sub_link) {
        if (ch->sub_latched && !sub_effective_sync(ch)) {
            sub_apply_state(ch);
            dirty_root(ch);
        }
        sub_desync_flush(ch);
    }
}

static void sub_set_sync(struct wl_client* client, struct wl_resource* res) {
    struct awl_surface* s = wl_resource_get_user_data(res);
    if (s) s->sub_sync = 1;   /* latch from the next commit on (already-queued latched state is kept) */
}
static void sub_set_desync(struct wl_client* client, struct wl_resource* res) {
    struct awl_surface* s = wl_resource_get_user_data(res);
    if (!s) return;
    s->sub_sync = 0;
    if (s->sub_latched && !sub_effective_sync(s)) {
        sub_apply_state(s);
        dirty_root(s);
    }
    sub_desync_flush(s);
}

static const struct wl_subsurface_interface subsurface_iface = {
    .destroy = sub_destroy,
    .set_position = sub_set_position,
    .place_above = sub_place_above,
    .place_below = sub_place_below,
    .set_sync = sub_set_sync,
    .set_desync = sub_set_desync,
};

/* ---------------- wl_subcompositor ---------------- */

static void subcompositor_destroy(struct wl_client* client,
                                  struct wl_resource* res) {
    wl_resource_destroy(res);
}

static void subcompositor_get_subsurface(struct wl_client* client,
                                         struct wl_resource* res, uint32_t id,
                                         struct wl_resource* surface_res,
                                         struct wl_resource* parent_res) {
    struct awl_surface* s = wl_resource_get_user_data(surface_res);
    struct awl_surface* parent = wl_resource_get_user_data(parent_res);
    if (!s || !parent) {
        wl_resource_post_error(res, WL_SUBCOMPOSITOR_ERROR_BAD_SURFACE,
                               "invalid surface argument");
        return;
    }
    if (s->role != AWL_ROLE_NONE || s->subsurface_res) {
        wl_resource_post_error(res, WL_SUBCOMPOSITOR_ERROR_BAD_SURFACE,
                               "surface already has a role");
        return;
    }
    if (s == parent) {
        wl_resource_post_error(res, WL_SUBCOMPOSITOR_ERROR_BAD_PARENT,
                               "parent is the surface itself");
        return;
    }
    /* parent must not be a descendant of s (KWin mainSurface ancestor check) —
     * a surface can keep its child chain after un-role; a cycle would corrupt
     * root/layers traversal */
    for (struct awl_surface* a = parent; a; a = a->sub_parent) {
        if (a == s) {
            wl_resource_post_error(res, WL_SUBCOMPOSITOR_ERROR_BAD_PARENT,
                                   "parent is a descendant of the surface");
            return;
        }
    }

    struct wl_resource* ss = wl_resource_create(
            client, &wl_subsurface_interface,
            wl_resource_get_version(res), id);
    if (!ss) { wl_resource_post_no_memory(res); return; }
    wl_resource_set_implementation(ss, &subsurface_iface, s, sub_res_destroy);

    pthread_rwlock_wrlock(&g_srv.rwl);
    s->role = AWL_ROLE_SUBSURFACE;
    s->subsurface_res = ss;
    s->sub_parent = parent;
    s->sub_sync = 1;   /* protocol default: sync */
    s->sub_latched = 0;
    s->latched_buffer_res = NULL;
    s->latched_attach = 0;
    s->sub_x = 0;
    s->sub_y = 0;
    s->sub_pos_pending = 0;
    wl_list_insert(parent->sub_children.prev, &s->sub_link);   /* stack top */
    pthread_rwlock_unlock(&g_srv.rwl);
    LOGI("surface %llu -> subsurface of %llu (stack top)",
            (unsigned long long)s->id, (unsigned long long)parent->id);
    dirty_root(s);
}

static const struct wl_subcompositor_interface subcompositor_iface = {
    .destroy = subcompositor_destroy,
    .get_subsurface = subcompositor_get_subsurface,
};

static void subcompositor_bind(struct wl_client* client, void* data,
                               uint32_t version, uint32_t id) {
    struct wl_resource* res = wl_resource_create(
            client, &wl_subcompositor_interface, 1, id);
    if (!res) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(res, &subcompositor_iface, NULL, NULL);
}

void awl_subsurface_setup(void) {
    g_srv.g_subcompositor = wl_global_create(
            g_srv.display, &wl_subcompositor_interface, 1,
            NULL, subcompositor_bind);
}

/* ---- Render layer snapshot (awl.h public API, called by render/input threads) ----
 * The wl_pointer.set_cursor image is NOT part of this stack (it never
 * hit-tests): the renderer appends it on top via awl_pointer_cursor_layer
 * (awl_input.c).
 * #31 scaling: coordinates/sizes are always logical px (viewport dst | source |
 * buffer/scale; see awl_viewport.c awl_surface_logical_size). The render side
 * scales by the window-physical / root-logical ratio; input hit-testing uses the
 * same stack. */

static void layers_collect(struct awl_surface* parent, float bx, float by,
                           awl_layer_info_t* out, int* n, int max, int depth) {
    if (depth > 8) return;
    struct awl_surface* ch;
    wl_list_for_each(ch, &parent->sub_children, sub_link) {
        if (*n >= max) return;
        pthread_mutex_lock(&ch->ev_lock);   /* serialize against set_position / buffer swap */
        float x = bx + (float)ch->sub_x;
        float y = by + (float)ch->sub_y;
        /* A popup's sub_* is the window origin (geometry semantics, product of
         * the anchor computation) — the position then subtracts the popup's own
         * geometry margin (chrome menus carry shadow margins on all sides).
         * A subsurface has no geometry and is unaffected. All logical coords. */
        if (ch->geom_valid) {
            x -= (float)ch->geom_x;
            y -= (float)ch->geom_y;
        }
        float w = 0, h = 0;
        awl_surface_logical_size(ch, &w, &h);
        float u0, v0, su, sv;   /* sample region (viewport source) → normalized uv, awl_viewport.c */
        awl_surface_layer_uv(ch, &u0, &v0, &su, &sv);
        int32_t xform = ch->buf_transform;
        pthread_mutex_unlock(&ch->ev_lock);
        out[*n].surface_id = ch->id;
        out[*n].x = x;
        out[*n].y = y;
        out[*n].w = w;
        out[*n].h = h;
        out[*n].u0 = u0;
        out[*n].v0 = v0;
        out[*n].su = su;
        out[*n].sv = sv;
        out[*n].transform = xform;
        (*n)++;
        layers_collect(ch, x, y, out, n, max, depth + 1);
    }
}

int awl_surface_get_layers(uint64_t root_id, awl_layer_info_t* out, int max) {
    if (max < 1) return 0;
    pthread_rwlock_rdlock(&g_srv.rwl);
    struct awl_surface* root = awl_surface_by_id(root_id);
    if (!root) {
        pthread_rwlock_unlock(&g_srv.rwl);
        return 0;
    }
    root = awl_subsurface_root(root);   /* fault tolerance: a child layer id maps back to the root */
    int n = 0;
    if (root->role != AWL_ROLE_SUBSURFACE) {
        out[0].surface_id = root->id;
        out[0].x = 0;
        out[0].y = 0;
        pthread_mutex_lock(&root->ev_lock);
        float w = 0, h = 0;
        awl_surface_logical_size(root, &w, &h);
        awl_surface_layer_uv(root, &out[0].u0, &out[0].v0, &out[0].su, &out[0].sv);
        out[0].w = w;
        out[0].h = h;
        out[0].transform = root->buf_transform;
        pthread_mutex_unlock(&root->ev_lock);
        n = 1;
        layers_collect(root, 0, 0, out, &n, max, 1);
    }
    pthread_rwlock_unlock(&g_srv.rwl);
    return n;
}

/* Input hit-testing: scan backward from the last entry of the render draw order
 * (get_layers: bottom → top) — the visually topmost layer wins, so hit-testing
 * matches the screen exactly. prefer>0: force that layer while the touch/pointer
 * grab is held (protocol: focus fixed after down/press); if the layer is no
 * longer in the tree, fall back to normal hit-testing.
 * exclude>0: skip that layer when hit-testing (drag icon). Never NULL: on no hit
 * (shadow margin / layer without buffer) fall back to the root. */
struct awl_surface* awl_subsurface_hit(struct awl_surface* root, float bx, float by,
                                       uint64_t prefer, uint64_t exclude,
                                       float* lx, float* ly) {
    awl_layer_info_t lay[AWL_MAX_LAYERS];
    int n = awl_surface_get_layers(root->id, lay, AWL_MAX_LAYERS);
    if (prefer && prefer != exclude) {
        for (int i = 0; i < n; i++) {
            if (lay[i].surface_id != prefer) continue;
            struct awl_surface* s = awl_surface_by_id(prefer);
            if (!s) break;   /* layer destroyed → normal hit-testing */
            *lx = bx - (float)lay[i].x;
            *ly = by - (float)lay[i].y;
            return s;
        }
    }
    for (int i = n - 1; i >= 0; i--) {
        if (exclude && lay[i].surface_id == exclude) continue;   /* drag icon */
        if (!lay[i].w || !lay[i].h) continue;   /* layers without a buffer never hit */
        float rx = bx - (float)lay[i].x, ry = by - (float)lay[i].y;
        if (rx < 0.0f || ry < 0.0f ||
            rx >= (float)lay[i].w || ry >= (float)lay[i].h) continue;
        struct awl_surface* s = awl_surface_by_id(lay[i].surface_id);
        if (!s) continue;
        *lx = rx;
        *ly = ry;
        return s;
    }
    *lx = bx;
    *ly = by;
    return root;
}
