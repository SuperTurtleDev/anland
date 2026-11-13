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
 *   - A replaced buffer is released when its frame leaves the child's buffer
 *     queue (awl_bufferqueue.h; superseded frames at the next drain, the
 *     sampled one with the composite's release fence) — a client (desync
 *     bubble swapping frames at a high rate) overwriting a dmabuf under
 *     sampling is another source of tearing/flicker.
 *
 * Stacking (kwin SurfaceInterfacePrivate below/above + SurfaceItemWayland z):
 * every parent keeps a CURRENT below stack and above stack around its own
 * content plus a PENDING copy of both; place_above/place_below edit the
 * pending copy (the parent itself is a legal reference: "just below the
 * first above child" / "just above the last below child") and the parent's
 * next commit promotes it atomically (sub_apply_stack). get_subsurface
 * appends to the top of both copies (kwin addChild), a popup / drag icon
 * enters the same way (awl_subsurface_link_immediate_above_locked). Render
 * order = recursive below-subtrees → surface → above-subtrees
 * (awl_surface_get_layers), the same list the hit-test walks top-down.
 *
 * Composition: child surface commit / topology change → dirty the "root"
 * window (child layers get no Activity); both backends snapshot via
 * awl_surface_get_layers — the GL renderer composites the layers on the GPU
 * (awl_renderer.cpp), the SurfaceControl backend reconciles its per-layer
 * SC set + z order on that same dirty (awl_sc.cpp awl_sc_sync) and latches
 * each layer's queue head at vsync.
 *
 * Locks: topology (sub_parent / active+pending below/above stacks) = g_srv.rwl
 * (readers and writers alike); sub_x / sub_y / latched = child
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

static void list_remove_init(struct wl_list* link) {
    if (link->next != link) wl_list_remove(link);
    wl_list_init(link);
}

/* KWin's SurfaceInterfacePrivate keeps current and pending below/above lists.
 * Keep the same representation here: wl_subsurface place requests mutate only
 * the pending order; a parent commit atomically promotes it to current. */
static void sub_pending_rebuild_locked(struct awl_surface* parent) {
    struct awl_surface* ch;
    wl_list_for_each(ch, &parent->sub_children, sub_child_link) {
        list_remove_init(&ch->sub_pend_link);
        ch->sub_pend_above = 0;
    }
    wl_list_for_each(ch, &parent->sub_below, sub_link) {
        wl_list_insert(parent->pend_sub_below.prev, &ch->sub_pend_link);
        ch->sub_pend_above = 0;
    }
    wl_list_for_each(ch, &parent->sub_above, sub_link) {
        wl_list_insert(parent->pend_sub_above.prev, &ch->sub_pend_link);
        ch->sub_pend_above = 1;
    }
}

void awl_subsurface_link_immediate_above_locked(struct awl_surface* child,
                                                struct awl_surface* parent) {
    if (child->sub_parent) awl_subsurface_unlink_locked(child);
    child->sub_parent = parent;
    wl_list_insert(parent->sub_children.prev, &child->sub_child_link);
    wl_list_insert(parent->sub_above.prev, &child->sub_link);
    /* KWin addChild appends to both current and pending stacks. This preserves
     * any ordered place_* requests already waiting on the parent. */
    wl_list_insert(parent->pend_sub_above.prev, &child->sub_pend_link);
    child->sub_pend_above = 1;
}

void awl_subsurface_unlink_locked(struct awl_surface* child) {
    if (!child->sub_parent) return;
    list_remove_init(&child->sub_link);
    list_remove_init(&child->sub_pend_link);
    list_remove_init(&child->sub_child_link);
    child->sub_parent = NULL;
    child->sub_pend_above = 0;
}

static int sub_apply_stack(struct awl_surface* parent) {
    int changed = 0;
    pthread_rwlock_wrlock(&g_srv.rwl);
    if (!parent->sub_stack_pending) {
        pthread_rwlock_unlock(&g_srv.rwl);
        return 0;
    }

    struct awl_surface* ch;
    struct awl_surface* tmp;
    wl_list_for_each(ch, &parent->sub_children, sub_child_link)
        list_remove_init(&ch->sub_link);
    wl_list_for_each_safe(ch, tmp, &parent->pend_sub_below, sub_pend_link) {
        list_remove_init(&ch->sub_pend_link);
        wl_list_insert(parent->sub_below.prev, &ch->sub_link);
    }
    wl_list_for_each_safe(ch, tmp, &parent->pend_sub_above, sub_pend_link) {
        list_remove_init(&ch->sub_pend_link);
        wl_list_insert(parent->sub_above.prev, &ch->sub_link);
    }
    parent->sub_stack_pending = 0;
    sub_pending_rebuild_locked(parent);
    changed = 1;
    pthread_rwlock_unlock(&g_srv.rwl);
    return changed;
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
    int drop_fd = -1;
    struct wl_resource* drop_rel = NULL;
    if (s->pending_attached) {   /* latch buffer state only for cycles that attached */
        if (s->sub_latched && s->latched_attach) {
            /* superseded by a newer latch, never presented: the buffer
             * (unless it is also the displayed one) and its explicit-sync
             * pair go straight back */
            if (s->u.sub.latched_buffer_res && s->u.sub.latched_buffer_res != s->pending_buffer_res)
                drop = s->u.sub.latched_buffer_res;
            drop_fd = s->u.sub.latched_acquire_fd;
            drop_rel = s->u.sub.latched_release_res;
        }
        s->u.sub.latched_buffer_res = s->pending_buffer_res;
        s->pending_buffer_res = NULL;
        s->pending_attached = 0;
        s->latched_attach = 1;
        s->u.sub.latched_acquire_fd = s->pend_acquire_fd;   /* explicit-sync state latches with the buffer */
        s->pend_acquire_fd = -1;
        s->u.sub.latched_release_res = s->pend_release_res;
        s->pend_release_res = NULL;
    }
    s->sub_latched = 1;
    /* damage stays pending and keeps accumulating; moved to current on apply */
    pthread_mutex_unlock(&s->ev_lock);
    if (drop && drop != s->current_buffer_res)
        wl_buffer_send_release(drop);   /* never sampled — release immediately */
    awl_surface_discard_sync(drop_fd, drop_rel);
    return 1;
}

/* Apply s's latched state (caller = client dispatch thread). */
static void sub_apply_state(struct awl_surface* ch) {
    /* reads the union's sub branch ungated — sound because a live latch
     * implies the subsurface role (maybe_latch only latches SUBSURFACE and
     * every role-exit path drops the latch first) */
    AWL_ASSERT(ch->role == AWL_ROLE_SUBSURFACE);
    pthread_mutex_lock(&ch->ev_lock);
    awl_surface_apply_mapping_state_locked(ch);
    int had_attach = ch->latched_attach;
    int acquire_fd = ch->u.sub.latched_acquire_fd;
    struct wl_resource* release_res = ch->u.sub.latched_release_res;
    if (ch->latched_attach)
        ch->current_buffer_res = ch->u.sub.latched_buffer_res;
    ch->u.sub.latched_buffer_res = NULL;
    ch->latched_attach = 0;
    ch->u.sub.latched_acquire_fd = -1;
    ch->u.sub.latched_release_res = NULL;
    ch->sub_latched = 0;
    /* latched state applies now — its damage with it (also covers a latched
     * damage-only commit: no attach, pd accumulated, function's empty-check
     * handles the "nothing changed" case) */
    awl_damage_merge_pending(ch, had_attach);
    /* set_input_region / set_opaque_region ride the same latch: their
     * pending slot stayed put at maybe_latch time (like damage) and is
     * promoted here — Firefox sets the render child's empty input region and
     * commits it in sync mode, the GTK parent's commit makes it live (#85) */
    awl_surface_apply_regions_locked(ch);
    /* the presented frame enters the child's queue exactly like a direct
     * commit (the old current is released when its element is drained) */
    if (had_attach)
        awl_surface_apply_buffer(ch, ch->current_buffer_res, acquire_fd, release_res);
    else if (ch->cd_state != AWL_DMG_NONE)
        awl_surface_shm_damaged_locked(ch);   /* in-place redraw of a shm source */
    pthread_mutex_unlock(&ch->ev_lock);
    if (had_attach) awl_surface_commit_drain(ch);
}

/* s's state was just applied (any commit): child double-buffered positions take
 * effect; latched state of effective-sync children applies in cascade — children
 * that applied keep descending into their own children (KWin: the parent
 * transaction merges direct child transactions; a desync child commit merges its
 * own child transactions immediately).
 * Return 1 = some child layer's buffer state was applied (caller uses this to
 * decide whether to trigger presentation). */
static int sub_parent_apply_list(struct wl_list* list) {
    int applied = 0;
    struct awl_surface* ch;
    wl_list_for_each(ch, list, sub_link) {
        pthread_mutex_lock(&ch->ev_lock);
        int apply = ch->sub_latched;
        if (ch->sub_pos_pending) {          /* KWin parentApplyState */
            ch->sub_x = ch->u.sub.pend_x;
            ch->sub_y = ch->u.sub.pend_y;
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

int awl_subsurface_parent_applied(struct awl_surface* s) {
    int applied = sub_apply_stack(s);
    applied |= sub_parent_apply_list(&s->sub_below);
    applied |= sub_parent_apply_list(&s->sub_above);
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
    s->u.sub.subsurface_res = NULL;
    if (s->sub_parent) {
        struct awl_surface* root = awl_subsurface_root(s);
        root_id = root->id;
        dirty = root->mapped;
        awl_subsurface_unlink_locked(s);
    }
    s->role = AWL_ROLE_NONE;
    pthread_rwlock_unlock(&g_srv.rwl);
    /* Latched state was never presented — release directly; pending position voided */
    struct wl_resource* latched_drop = NULL;
    int drop_fd = -1;
    struct wl_resource* drop_rel = NULL;
    pthread_mutex_lock(&s->ev_lock);
    if (s->sub_latched && s->latched_attach) {
        latched_drop = s->u.sub.latched_buffer_res;
        drop_fd = s->u.sub.latched_acquire_fd;
        drop_rel = s->u.sub.latched_release_res;
        s->u.sub.latched_acquire_fd = -1;
        s->u.sub.latched_release_res = NULL;
    }
    s->u.sub.latched_buffer_res = NULL;
    s->sub_latched = 0;
    s->latched_attach = 0;
    s->sub_pos_pending = 0;
    pthread_mutex_unlock(&s->ev_lock);
    if (latched_drop)
        wl_buffer_send_release(latched_drop);
    awl_surface_discard_sync(drop_fd, drop_rel);
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
    s->u.sub.pend_x = x;
    s->u.sub.pend_y = y;
    s->sub_pos_pending = 1;
    pthread_mutex_unlock(&s->ev_lock);
}

/* place_above / place_below (KWin SurfaceInterfacePrivate::raiseChild /
 * lowerChild): the reference may be a sibling OR the parent itself — the
 * parent counts as the element sitting between the below and above stacks.
 * Pending state only (parent->sub_stack_pending): the parent's next commit
 * promotes it (sub_apply_stack). Anything else is a protocol error, as in
 * kwin (error_bad_surface "incorrect sibling"). */
static void sub_place(struct wl_resource* res, struct wl_resource* sibling_res,
                      int above) {
    struct awl_surface* s = wl_resource_get_user_data(res);
    struct awl_surface* sib = sibling_res ? wl_resource_get_user_data(sibling_res) : NULL;
    pthread_rwlock_wrlock(&g_srv.rwl);
    struct awl_surface* parent = s ? s->sub_parent : NULL;
    if (!s || !sib || s == sib || !parent ||
        (sib != parent && sib->sub_parent != parent)) {
        pthread_rwlock_unlock(&g_srv.rwl);
        if (s)
            wl_resource_post_error(res, WL_SUBSURFACE_ERROR_BAD_SURFACE,
                                   "reference surface is not the parent or a sibling");
        return;
    }
    list_remove_init(&s->sub_pend_link);
    if (sib == parent) {
        if (above) {   /* parent = just before the first element of above */
            wl_list_insert(&parent->pend_sub_above, &s->sub_pend_link);
            s->sub_pend_above = 1;
        } else {       /* parent = just after the last element of below */
            wl_list_insert(parent->pend_sub_below.prev, &s->sub_pend_link);
            s->sub_pend_above = 0;
        }
    } else {           /* sibling: same stack (below/above) as the anchor, right after/before it */
        wl_list_insert(above ? &sib->sub_pend_link : sib->sub_pend_link.prev,
                       &s->sub_pend_link);
        s->sub_pend_above = sib->sub_pend_above;
    }
    parent->sub_stack_pending = 1;
    pthread_rwlock_unlock(&g_srv.rwl);
}

static void sub_place_above(struct wl_client* client, struct wl_resource* res,
                            struct wl_resource* sibling_res) {
    sub_place(res, sibling_res, 1);
}

static void sub_place_below(struct wl_client* client, struct wl_resource* res,
                            struct wl_resource* sibling_res) {
    sub_place(res, sibling_res, 0);
}

/* set_desync cascade flush: the latched state of self + descendants whose
 * effective sync has been released applies immediately (KWin parentDesynchronized → transaction->commit) */
static void sub_desync_flush(struct awl_surface* s) {
    struct awl_surface* ch;
    wl_list_for_each(ch, &s->sub_below, sub_link) {
        if (ch->sub_latched && !sub_effective_sync(ch)) {
            sub_apply_state(ch);
            dirty_root(ch);
        }
        sub_desync_flush(ch);
    }
    wl_list_for_each(ch, &s->sub_above, sub_link) {
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
    if (s->role != AWL_ROLE_NONE) {
        wl_resource_post_error(res, WL_SUBCOMPOSITOR_ERROR_BAD_SURFACE,
                               "surface already has a role");
        return;
    }
    /* role == NONE ⇒ the union's word 0 is NULL (every role-object destroy
     * handler clears its pointer before role → NONE): the extra
     * `|| s->u.sub.subsurface_res` clause this replaces was unreachable */
    AWL_ASSERT(!s->u.sub.subsurface_res);
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
    s->u.sub.subsurface_res = ss;
    awl_subsurface_link_immediate_above_locked(s, parent);
    s->sub_sync = 1;   /* protocol default: sync */
    s->sub_latched = 0;
    s->u.sub.latched_buffer_res = NULL;
    s->u.sub.latched_release_res = NULL;
    s->u.sub.latched_acquire_fd = -1;   /* fresh sub branch (role union) */
    s->latched_attach = 0;
    s->sub_x = 0;
    s->sub_y = 0;
    s->u.sub.pend_x = s->u.sub.pend_y = 0;
    s->sub_pos_pending = 0;
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
    if (!wl_global_create(g_srv.display,
                          &wl_subcompositor_interface, 1,
                          NULL, subcompositor_bind))
        LOGE("wl_subcompositor global create failed");
}

/* ---- Render layer snapshot (awl.h public API, called by render/input threads) ----
 * The wl_pointer.set_cursor image is NOT part of this stack (it never
 * hit-tests): the renderer appends it on top via awl_pointer_cursor_layer
 * (awl_input.c).
 * #31 scaling: coordinates/sizes are always logical px (viewport dst | source |
 * buffer/scale; see awl_viewport.c awl_surface_logical_size). The render side
 * scales by the window-physical / root-logical ratio; input hit-testing uses the
 * same stack. */

static void layers_collect(struct awl_surface* s, float x, float y,
                           awl_layer_info_t* out, int* n, int max, int depth);

static void layers_collect_children(struct wl_list* children, float bx, float by,
                                    awl_layer_info_t* out, int* n, int max, int depth) {
    struct awl_surface* ch;
    wl_list_for_each(ch, children, sub_link) {
        if (*n >= max) return;
        pthread_mutex_lock(&ch->ev_lock);
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
        pthread_mutex_unlock(&ch->ev_lock);
        layers_collect(ch, x, y, out, n, max, depth + 1);
    }
}

/* Render order is recursive below subtrees, this surface, then above
 * subtrees. This is the wl_subsurface stacking model: a child may be above or
 * below its parent, not merely above its siblings. */
static void layers_collect(struct awl_surface* s, float x, float y,
                           awl_layer_info_t* out, int* n, int max, int depth) {
    if (depth > 8 || *n >= max) return;
    layers_collect_children(&s->sub_below, x, y, out, n, max, depth);
    if (*n >= max) return;

    pthread_mutex_lock(&s->ev_lock);
    float w = 0, h = 0;
    awl_surface_logical_size(s, &w, &h);
    out[*n].surface_id = s->id;
    out[*n].x = x;
    out[*n].y = y;
    out[*n].w = w;
    out[*n].h = h;
    awl_surface_layer_uv(s, &out[*n].u0, &out[*n].v0, &out[*n].su, &out[*n].sv);
    out[*n].transform = s->buf_transform;
    pthread_mutex_unlock(&s->ev_lock);
    (*n)++;

    layers_collect_children(&s->sub_above, x, y, out, n, max, depth);
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
    if (root->role != AWL_ROLE_SUBSURFACE)
        layers_collect(root, 0, 0, out, &n, max, 1);
    pthread_rwlock_unlock(&g_srv.rwl);
    return n;
}

/* Input hit-testing: scan backward from the last entry of the render draw order
 * (get_layers: bottom → top) — the visually topmost layer wins, so hit-testing
 * matches the screen exactly. A layer whose committed input region excludes
 * the point is transparent to input and the scan continues underneath it
 * (wl_surface.set_input_region, #85: a GPU render subsurface with an empty
 * region must not steal the toolkit toplevel's clicks). prefer>0: force that
 * layer while the touch/pointer grab is held (protocol: focus fixed after
 * down/press, the region is not re-tested); if the layer is no longer in the
 * tree, fall back to normal hit-testing.
 * exclude>0: skip that layer when hit-testing (drag icon). Never NULL: on no hit
 * (shadow margin / layer without buffer / input-transparent everywhere) fall
 * back to the root — Android already routed the event to this window. */
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
        if (!awl_surface_accepts_input(s, rx, ry)) continue;   /* outside its input region: look through */
        *lx = rx;
        *ly = ry;
        return s;
    }
    *lx = bx;
    *ly = by;
    return root;
}
