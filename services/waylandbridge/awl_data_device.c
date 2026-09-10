/* awl_data_device.c — wl_data_device_manager v3 / wl_data_device /
 * wl_data_source / wl_data_offer (clipboard selection + drag and drop DnD,
 * full state machines)
 *
 * Motivation (2026-09-09 gdb symbolized evidence): chrome WaylandConnection::
 * CreateDataObjectsIfReady needs data_device_manager before it builds
 * window_drag_controller_; missing global → WaylandEventSource::OnWindowRemoved
 * unconditionally calls window_drag_controller()->drag_source() → null pointer
 * SIGSEGV (closing a bubble always crashes while any window holds touch
 * focus). GIMP/GTK also depend heavily on this protocol (clipboard + DnD).
 *
 * Semantics aligned with kwin-6.6.5 (datadevicemanager/datasource/datadevice/
 * dataoffer.cpp + seat.cpp startDrag/endDrag/cancelDrag + pointer implicit
 * grab):
 *   - selection: set_selection replaces the global clipboard source; notify
 *     every data device of the "keyboard focus client" (data_offer + all
 *     mimes + selection events); re-delivered on focus change / late device
 *     creation; source death → selection(NULL) notification; v3 rejects a
 *     DnD source carrying set_actions (invalid_source).
 *   - DnD: start_drag → input belongs to the drag machine while the drag is
 *     running (implicit grab, awl_input.c hooks): the origin is the first
 *     target (enter + data_offer + source_actions + action negotiation);
 *     motion → layer hit switches target (leave old + data_offer new + enter,
 *     icon layer excluded); release → if accepted, drop + dnd_drop_performed,
 *     else dnd_drop_performed + cancelled; touch cancel / source death /
 *     origin death → cancelled. offer.finish (drop must already have
 *     happened) → dnd_finished; offer.destroy after drop → supplemental
 *     dnd_finished (KWin ~offer).
 *   - action negotiation (simplified from KWin chooseDndAction: no exclusive
 *     action): modifier forced (Ctrl=copy/Shift=move) → target preferred →
 *     first action common to both sides (copy,move,ask order).
 *   - receive: fd passed straight through to wl_data_source.send (pipe
 *     between clients, the daemon never touches the data and closes its own
 *     copy of the fd); unknown mimes are not forwarded (KWin).
 *   - icon: validated as roleless (error role) → attached at the top of the
 *     origin root layer stack (reuses the subsurface rendering pipeline;
 *     stack tail = topmost, above all bubble layers); position updated
 *     immediately per motion (sub_pos double buffering only works for real
 *     subsurfaces, the icon is not governed by commit) + per-event
 *     window_dirty; unlinked when the drag ends; excluded from hit testing.
 *
 * Locks (follow the global layering rwl → dd_lock → ev_lock, never reverse):
 *   g_srv.rwl    data_devices/sources/offers list topology: create/destroy/
 *                offer creation = wr, traversal = rd. Every dereference-and-
 *                send of an external res/dev/src pointer must sit under rwl
 *                (the destroyer clears references under wr before free —
 *                rd/wr is the lifetime fence).
 *   g_srv.dd_lock g_selection / g_drag / g_dd_focus_client / source mimes.
 *   Every drag machine entry point (motion/end/cancel) takes rwl.wr
 *   internally — the caller (input thread) must not hold rwl; motion's layer
 *   hit is resolved by the caller under rwl.rd first, surface ids are passed
 *   in to rebuild references (destroyed during the rd→wr gap counts as no
 *   target).
 */
#define _GNU_SOURCE   /* bionic: pipe2 */
#include "awl_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <string.h>
#include <unistd.h>   /* close: receive fd */

#define AWL_DDM_VERSION 3

enum {
    AWL_DND_COPY = 1,
    AWL_DND_MOVE = 2,
    AWL_DND_ASK  = 4,
    AWL_DND_ALL  = 7,
};

/* ---------------- state ---------------- */

static struct {
    int active;
    struct awl_data_source* src;      /* NULL = sourceless drag (chrome window drag) */
    struct awl_surface* origin;       /* origin surface of start_drag */
    uint64_t origin_win;              /* id of the root window holding origin (Android routing domain) */
    struct awl_surface* icon;         /* drag icon layer attached at top of the root layer stack */
    struct awl_surface* target;       /* current DnD target layer */
    struct awl_data_device* target_dev;
    struct awl_data_offer* offer;
    uint32_t mods;                    /* AWL_DMOD_* */
} g_drag;

static struct awl_data_source* g_selection;
static struct wl_client* g_dd_focus_client;   /* keyboard focus client (selection receiver) */
static struct awl_data_source* g_clip_src;    /* resident internal source for the Android clipboard (res=NULL; created only by the T_CLIPBOARD thread) */
static char* g_clip_text;                     /* current Android clipboard text (dd_lock; snapshotted by fill_fd) */

static struct wl_client* client_of(struct wl_resource* res) {
    return wl_resource_get_client(res);
}

/* Actions the source supports: v3 = result of set_actions (if not called, KWin DataSourceInterface v<3 → copy) */
static uint32_t source_actions(const struct awl_data_source* src) {
    if (src->res &&
        wl_resource_get_version(src->res) < WL_DATA_SOURCE_ACTION_SINCE_VERSION)
        return AWL_DND_COPY;
    return src->dnd_actions ? src->dnd_actions : AWL_DND_COPY;
}

/* KWin DataSourceInterface::accept — the target picked a mime (NULL = none suitable) */
static void source_accept(struct awl_data_source* src, const char* mime) {
    src->accepted = mime != NULL;
    if (src->res) {
        wl_data_source_send_target(src->res, mime);
        wl_client_flush(client_of(src->res));
    }
}

/* KWin DataSourceInterface::requestData — fd passed straight to the source client (caller holds rwl+dd) */
static void source_request_data(struct awl_data_source* src, const char* mime, int fd) {
    if (src->res) {
        wl_data_source_send_send(src->res, mime, fd);
        wl_client_flush(client_of(src->res));
    } else if (src->fill_fd) {
        src->fill_fd(src, mime, fd);   /* internal source manages the fd itself (closing included) */
        return;
    }
    close(fd);   /* close our own copy of the fd; the receiver's fd is dup'ed by the protocol stack */
}

/* Append a mime offered by the source (dedup; offer event order = list order). Inside dd_lock. */
static void source_add_mime(struct awl_data_source* src, const char* mime) {
    struct awl_mime* m;
    wl_list_for_each(m, &src->mimes, link)
        if (!strcmp(m->name, mime)) return;
    struct awl_mime* nm = calloc(1, sizeof(*nm));
    if (!nm) return;
    snprintf(nm->name, sizeof(nm->name), "%s", mime);
    wl_list_insert(src->mimes.prev, &nm->link);
}

/* ---------------- wl_data_offer ---------------- */

static void offer_send_actions(struct awl_data_offer* off) {
    if (!off->src || !off->res) return;
    if (wl_resource_get_version(off->res) < WL_DATA_OFFER_SOURCE_ACTIONS_SINCE_VERSION)
        return;
    wl_data_offer_send_source_actions(off->res, source_actions(off->src));
}

static void offer_send_mimes(struct awl_data_offer* off) {
    if (!off->src) return;
    struct awl_mime* m;
    wl_list_for_each(m, &off->src->mimes, link)
        wl_data_offer_send_offer(off->res, m->name);
}

static void offer_set_action(struct awl_data_offer* off, uint32_t action) {
    if (!off->res ||
        wl_resource_get_version(off->res) < WL_DATA_OFFER_ACTION_SINCE_VERSION)
        return;
    wl_data_offer_send_action(off->res, action);
}

static void source_send_action(struct awl_data_source* src, uint32_t action) {
    src->selected_action = action;
    if (!src->res) return;
    if (wl_resource_get_version(src->res) < WL_DATA_SOURCE_ACTION_SINCE_VERSION)
        return;
    wl_data_source_send_action(src->res, action);
}

static void drag_match_actions_locked(void);
static void drag_update_target_locked(struct awl_surface* surface, float x, float y);
static void drag_cancel_locked(void);
static void selection_notify_focus(void);
static void selection_push_android_locked(struct awl_data_source* src);

/* Negotiation (simplified from KWin chooseDndAction: no exclusive action;
 * inside dd_lock): modifier forced → target preferred (only if the source
 * supports it) → first action common to both sides. */
static uint32_t negotiate_action(struct awl_data_source* src,
                                 struct awl_data_offer* off, uint32_t mods) {
    uint32_t sa = src ? source_actions(src) : AWL_DND_COPY;
    uint32_t ta = off->has_actions ? off->supported_actions : (AWL_DND_COPY | AWL_DND_MOVE);
    uint32_t pref = off->has_actions ? off->preferred_action : AWL_DND_COPY;
    if ((mods & AWL_DMOD_CTRL) &&
        (sa & AWL_DND_COPY) && (ta & AWL_DND_COPY)) return AWL_DND_COPY;
    if ((mods & AWL_DMOD_SHIFT) &&
        (sa & AWL_DND_MOVE) && (ta & AWL_DND_MOVE)) return AWL_DND_MOVE;
    if (pref && (sa & pref)) return pref;
    uint32_t common = sa & ta;
    if (common & AWL_DND_COPY) return AWL_DND_COPY;
    if (common & AWL_DND_MOVE) return AWL_DND_MOVE;
    if (common & AWL_DND_ASK)  return AWL_DND_ASK;
    return 0;
}

static void offer_accept(struct wl_client* c, struct wl_resource* res,
                         uint32_t serial, const char* mime) {
    struct awl_data_offer* off = wl_resource_get_user_data(res);
    pthread_rwlock_rdlock(&g_srv.rwl);
    pthread_mutex_lock(&g_srv.dd_lock);
    if (off->src && off->dnd && !off->src->drop_performed)
        source_accept(off->src, mime && *mime ? mime : NULL);   /* non-dnd / already dropped: ignore */
    pthread_mutex_unlock(&g_srv.dd_lock);
    pthread_rwlock_unlock(&g_srv.rwl);
}

static void offer_receive(struct wl_client* c, struct wl_resource* res,
                          const char* mime, int32_t fd) {
    struct awl_data_offer* off = wl_resource_get_user_data(res);
    if (fd < 0) return;
    pthread_rwlock_rdlock(&g_srv.rwl);
    pthread_mutex_lock(&g_srv.dd_lock);
    struct awl_data_source* src = off->src;
    int known = 0;
    if (src && mime) {
        struct awl_mime* m;
        wl_list_for_each(m, &src->mimes, link)
            if (!strcmp(m->name, mime)) { known = 1; break; }
    }
    LOGI("offer receive: mime=%s src=%s%s", mime ? mime : "(null)",
            src ? (src->res ? "client" : "android-clip") : "none",
            known ? "" : " (unknown mime, dropped)");
    if (known)
        source_request_data(src, mime, fd);   /* KWin: unknown mimes are not forwarded */
    else
        close(fd);
    pthread_mutex_unlock(&g_srv.dd_lock);
    pthread_rwlock_unlock(&g_srv.rwl);
}

static void offer_finish(struct wl_client* c, struct wl_resource* res) {
    struct awl_data_offer* off = wl_resource_get_user_data(res);
    pthread_rwlock_rdlock(&g_srv.rwl);
    pthread_mutex_lock(&g_srv.dd_lock);
    if (!off->dnd || !off->src || !off->src->drop_performed) {
        /* KWin: only a DnD offer that has already been dropped may finish */
        pthread_mutex_unlock(&g_srv.dd_lock);
        pthread_rwlock_unlock(&g_srv.rwl);
        wl_resource_post_error(res, WL_DATA_OFFER_ERROR_INVALID_FINISH,
                               "finish on non-dnd offer or before drop");
        return;
    }
    if (off->src->res &&
        wl_resource_get_version(off->src->res) >= WL_DATA_SOURCE_DND_FINISHED_SINCE_VERSION) {
        wl_data_source_send_dnd_finished(off->src->res);
        wl_client_flush(client_of(off->src->res));
    }
    LOGI("dnd finished");
    off->src = NULL;   /* later receive/accept stay silent */
    pthread_mutex_unlock(&g_srv.dd_lock);
    pthread_rwlock_unlock(&g_srv.rwl);
}

static void offer_set_actions(struct wl_client* c, struct wl_resource* res,
                              uint32_t dnd_actions, uint32_t preferred) {
    struct awl_data_offer* off = wl_resource_get_user_data(res);
    if (dnd_actions & ~AWL_DND_ALL) {
        wl_resource_post_error(res, WL_DATA_OFFER_ERROR_INVALID_ACTION_MASK,
                               "invalid action mask");
        return;
    }
    if (preferred != WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE &&
        (preferred & ~AWL_DND_ALL)) {
        wl_resource_post_error(res, WL_DATA_OFFER_ERROR_INVALID_ACTION,
                               "invalid preferred action");
        return;
    }
    pthread_rwlock_rdlock(&g_srv.rwl);
    pthread_mutex_lock(&g_srv.dd_lock);
    off->supported_actions = dnd_actions;
    off->preferred_action = preferred;
    off->has_actions = 1;
    drag_match_actions_locked();
    pthread_mutex_unlock(&g_srv.dd_lock);
    pthread_rwlock_unlock(&g_srv.rwl);
}

static void offer_destroy(struct wl_client* c, struct wl_resource* res) {
    wl_resource_destroy(res);
}

/* KWin ~DataOfferInterface: drop already happened but the target only destroys without finish → finish on its behalf */
static void offer_res_destroy(struct wl_resource* res) {
    struct awl_data_offer* off = wl_resource_get_user_data(res);
    if (!off) return;
    pthread_rwlock_wrlock(&g_srv.rwl);
    wl_list_remove(&off->link);
    pthread_mutex_lock(&g_srv.dd_lock);
    if (off->dnd && off->src && off->src->drop_performed && off->src->res &&
        wl_resource_get_version(off->src->res) >= WL_DATA_SOURCE_DND_FINISHED_SINCE_VERSION) {
        wl_data_source_send_dnd_finished(off->src->res);
        wl_client_flush(client_of(off->src->res));
    }
    pthread_mutex_unlock(&g_srv.dd_lock);
    pthread_rwlock_unlock(&g_srv.rwl);
    free(off);
}

static const struct wl_data_offer_interface offer_iface = {
    .accept = offer_accept,
    .receive = offer_receive,
    .destroy = offer_destroy,
    .finish = offer_finish,
    .set_actions = offer_set_actions,
};

/* KWin DataDeviceInterfacePrivate::createDataOffer — new offer resource +
 * inserted into the list. Caller holds rwl (topology insert: wr);
 * data_offer/mime events are sent by the caller. */
static struct awl_data_offer* offer_create(struct wl_client* client,
                                           uint32_t version,
                                           struct awl_data_source* src,
                                           int dnd) {
    struct wl_resource* res = wl_resource_create(
            client, &wl_data_offer_interface, version, 0);
    if (!res) return NULL;
    struct awl_data_offer* off = calloc(1, sizeof(*off));
    if (!off) { wl_resource_destroy(res); return NULL; }
    off->res = res;
    off->src = src;
    off->dnd = dnd;
    off->preferred_action = WL_DATA_DEVICE_MANAGER_DND_ACTION_NONE;
    wl_list_insert(g_srv.data_offers.prev, &off->link);
    wl_resource_set_implementation(res, &offer_iface, off, offer_res_destroy);
    return off;
}

/* ---------------- wl_data_source ---------------- */

static void ds_offer(struct wl_client* c, struct wl_resource* res,
                     const char* mime) {
    struct awl_data_source* src = wl_resource_get_user_data(res);
    if (!src || !mime || !*mime) return;
    pthread_mutex_lock(&g_srv.dd_lock);
    source_add_mime(src, mime);
    pthread_mutex_unlock(&g_srv.dd_lock);
}

static void ds_set_actions(struct wl_client* c, struct wl_resource* res,
                           uint32_t dnd_actions) {
    struct awl_data_source* src = wl_resource_get_user_data(res);
    if (!src) return;
    if (dnd_actions & ~AWL_DND_ALL) {
        wl_resource_post_error(res, WL_DATA_SOURCE_ERROR_INVALID_ACTION_MASK,
                               "invalid action mask");
        return;
    }
    pthread_rwlock_rdlock(&g_srv.rwl);
    pthread_mutex_lock(&g_srv.dd_lock);
    src->dnd_actions = dnd_actions;
    src->is_dnd_actions = 1;
    drag_match_actions_locked();
    pthread_mutex_unlock(&g_srv.dd_lock);
    pthread_rwlock_unlock(&g_srv.rwl);
}

static void ds_destroy(struct wl_client* c, struct wl_resource* res) {
    wl_resource_destroy(res);
}

/* In-use cleanup when a source dies (shared by ds_res_destroy and internal
 * sources; caller holds rwl.wr, dd taken inside): drag source death → cancel
 * the drag (source is dead, no further cancelled); selection source death →
 * clear it and notify the focus side; clear the source reference in all
 * offers. Unlinking is done by the caller. */
static void source_teardown_locked(struct awl_data_source* src, int* was_selection) {
    pthread_mutex_lock(&g_srv.dd_lock);
    *was_selection = g_selection == src;
    if (*was_selection) g_selection = NULL;
    if (g_drag.active && g_drag.src == src) {
        g_drag.src = NULL;
        drag_cancel_locked();
    }
    struct awl_data_offer* off;
    wl_list_for_each(off, &g_srv.data_offers, link)
        if (off->src == src) off->src = NULL;
    pthread_mutex_unlock(&g_srv.dd_lock);
}

static void source_free(struct awl_data_source* src) {
    struct awl_mime* m;
    struct awl_mime* tmp;
    wl_list_for_each_safe(m, tmp, &src->mimes, link) {
        wl_list_remove(&m->link);
        free(m);
    }
    free(src);
}

static void ds_res_destroy(struct wl_resource* res) {
    struct awl_data_source* src = wl_resource_get_user_data(res);
    if (!src) return;
    pthread_rwlock_wrlock(&g_srv.rwl);
    wl_list_remove(&src->link);
    int was_selection = 0;
    source_teardown_locked(src, &was_selection);
    pthread_rwlock_unlock(&g_srv.rwl);
    if (was_selection)
        selection_notify_focus();   /* rwl already released → the notify takes its own locks inside */
    source_free(src);
}

static const struct wl_data_source_interface source_iface = {
    .offer = ds_offer,
    .set_actions = ds_set_actions,
    .destroy = ds_destroy,
};

/* ---------------- wl_data_device ---------------- */

static void dd_start_drag(struct wl_client* c, struct wl_resource* res,
                          struct wl_resource* source_res,
                          struct wl_resource* origin_res,
                          struct wl_resource* icon_res, uint32_t serial) {
    struct awl_surface* origin = origin_res ? wl_resource_get_user_data(origin_res) : NULL;
    if (!origin) {
        LOGI("start_drag without origin (ignored)");
        return;
    }
    struct awl_data_source* src = source_res ? wl_resource_get_user_data(source_res) : NULL;
    struct awl_surface* icon = icon_res ? wl_resource_get_user_data(icon_res) : NULL;
    if (icon_res && (!icon || icon->role != AWL_ROLE_NONE || icon->sub_parent)) {
        wl_resource_post_error(res, WL_DATA_DEVICE_ERROR_ROLE,
                               "drag icon surface already has a role");
        return;
    }

    uint64_t origin_win = 0;
    struct awl_surface* root = NULL;
    pthread_rwlock_wrlock(&g_srv.rwl);
    pthread_mutex_lock(&g_srv.dd_lock);
    if (g_drag.active) {
        pthread_mutex_unlock(&g_srv.dd_lock);
        pthread_rwlock_unlock(&g_srv.rwl);
        LOGI("start_drag while drag active (ignored)");
        return;
    }
    root = awl_subsurface_root(origin);
    origin_win = root->id;
    g_drag.active = 1;
    g_drag.src = src;
    g_drag.origin = origin;
    g_drag.origin_win = origin_win;
    g_drag.target = NULL;
    g_drag.target_dev = NULL;
    g_drag.offer = NULL;
    g_drag.mods = 0;
    g_drag.icon = icon;
    if (icon) {   /* root layer stack tail = topmost (KWin: icon above all layers) */
        wl_list_insert(root->sub_children.prev, &icon->sub_link);
        icon->sub_parent = root;
        pthread_mutex_lock(&icon->ev_lock);
        icon->sub_x = 0;
        icon->sub_y = 0;
        icon->sub_pos_pending = 0;
        pthread_mutex_unlock(&icon->ev_lock);
    }
    drag_update_target_locked(origin, 0.0f, 0.0f);   /* first target = origin */
    pthread_mutex_unlock(&g_srv.dd_lock);
    pthread_rwlock_unlock(&g_srv.rwl);
    LOGI("drag start: src=%p origin=%llu(win %llu) icon=%s",
            (void*)src, (unsigned long long)origin->id,
            (unsigned long long)origin_win, icon ? "yes" : "no");
    if (root->mapped && g_srv.cbs.window_dirty)
        g_srv.cbs.window_dirty(g_srv.cbs.user, origin_win);
}

/* set_selection: replace the global clipboard source + notify the keyboard focus client (KWin allows NULL to clear) */
static void dd_set_selection(struct wl_client* c, struct wl_resource* res,
                             struct wl_resource* source_res, uint32_t serial) {
    struct awl_data_source* src = source_res ? wl_resource_get_user_data(source_res) : NULL;
    pthread_rwlock_rdlock(&g_srv.rwl);
    pthread_mutex_lock(&g_srv.dd_lock);
    if (src && src->is_dnd_actions &&
        wl_resource_get_version(src->res) >= WL_DATA_SOURCE_ACTION_SINCE_VERSION) {
        pthread_mutex_unlock(&g_srv.dd_lock);
        pthread_rwlock_unlock(&g_srv.rwl);
        wl_resource_post_error(res, WL_DATA_DEVICE_ERROR_USED_SOURCE,
                               "data source is for drag and drop");
        return;
    }
    struct awl_data_source* old = g_selection;
    g_selection = src;
    LOGI("selection set: src=%p mimes=%d", (void*)src,
            src ? wl_list_length(&src->mimes) : 0);
    /* wl→Android push: with a source → fetch text on a detached thread
     * (non-text sources stay silent); the empty-text callback for a NULL
     * source (clear) is deferred to outside dd (the adapt layer's binder
     * transact should not hold dd). */
    if (src)
        selection_push_android_locked(src);
    pthread_mutex_unlock(&g_srv.dd_lock);
    if (!src && g_srv.cbs.clipboard_text)
        g_srv.cbs.clipboard_text(g_srv.cbs.user, 0, "");
    if (old && old->res && old != g_clip_src) {   /* replaced source gets cancelled */
        wl_data_source_send_cancelled(old->res);
        wl_client_flush(client_of(old->res));
    }
    pthread_rwlock_unlock(&g_srv.rwl);
    selection_notify_focus();
}

static void dd_release(struct wl_client* c, struct wl_resource* res) {
    wl_resource_destroy(res);
}

static void dd_res_destroy(struct wl_resource* res) {
    struct awl_data_device* dev = wl_resource_get_user_data(res);
    if (!dev) return;
    pthread_rwlock_wrlock(&g_srv.rwl);
    wl_list_remove(&dev->link);
    pthread_mutex_lock(&g_srv.dd_lock);
    if (g_drag.active && g_drag.target_dev == dev) {
        if (g_drag.target_dev->res) {
            wl_data_device_send_leave(g_drag.target_dev->res);
            wl_client_flush(g_drag.target_dev->client);
        }
        g_drag.target = NULL;
        g_drag.target_dev = NULL;
        g_drag.offer = NULL;   /* offer resource belongs to the target client, its destroy cleans up */
    }
    pthread_mutex_unlock(&g_srv.dd_lock);
    pthread_rwlock_unlock(&g_srv.rwl);
    free(dev);
}

static const struct wl_data_device_interface device_iface = {
    .start_drag = dd_start_drag,
    .set_selection = dd_set_selection,
    .release = dd_release,
};

/* ---------------- internal source (Android clipboard bridge, #29) ---------------- */

struct awl_data_source* awl_datadev_internal_source(
        void (*fill_fd)(struct awl_data_source*, const char*, int)) {
    struct awl_data_source* src = calloc(1, sizeof(*src));
    if (!src) return NULL;
    wl_list_init(&src->mimes);
    src->fill_fd = fill_fd;
    pthread_rwlock_wrlock(&g_srv.rwl);
    wl_list_insert(g_srv.data_sources.prev, &src->link);
    pthread_rwlock_unlock(&g_srv.rwl);
    return src;
}

void awl_datadev_internal_mime(struct awl_data_source* src, const char* mime) {
    pthread_mutex_lock(&g_srv.dd_lock);
    source_add_mime(src, mime);
    pthread_mutex_unlock(&g_srv.dd_lock);
}

void awl_datadev_internal_gone(struct awl_data_source* src) {
    pthread_rwlock_wrlock(&g_srv.rwl);
    wl_list_remove(&src->link);
    int was_selection = 0;
    source_teardown_locked(src, &was_selection);
    pthread_rwlock_unlock(&g_srv.rwl);
    if (was_selection) selection_notify_focus();
    source_free(src);
}

/* ---------------- Android clipboard bridge (#29) ----------------
 *
 * Bidirectional text bridge; Android clipboard reads and writes both go
 * through an Activity (background reads are restricted on Android 10+,
 * writes take the same channel for symmetry); echo suppression lives on the
 * APK side (static lastClipWritten/lastPushed, shared by all windows — same
 * process):
 *   wl→Android: client set_selection → a dedicated thread reads text/plain
 *     from the pipe (the blocking read keeps the dispatch thread free) →
 *     cbs.clipboard_text → adapt ctrl C_CLIP_WRITE → the owner window's
 *     Activity ClipboardManager.setPrimaryClip.
 *   Android→wl: the Activity (reads the clipboard while focused) pushes text
 *     via T_CLIPBOARD → awl_datadev_android_clip → the resident internal
 *     source takes over the selection + notifies the focus side. */

/* Internal source data callback: write the cached text into the requester's
 * fd. Contract (awl_internal.h): the caller already holds rwl+dd — no locks
 * may be taken here; text ≤256KB, the requester (who initiated receive)
 * reads promptly, so the pipe write returns immediately. */
static void clip_fill_fd(struct awl_data_source* src, const char* mime, int fd) {
    (void)src; (void)mime;
    size_t len = g_clip_text ? strlen(g_clip_text) : 0;
    size_t off = 0;
    while (off < len) {
        ssize_t n = write(fd, g_clip_text + off, len - off);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            break;
        }
        off += (size_t)n;
    }
    close(fd);
}

/* Android clipboard text arriving (T_CLIPBOARD; binder thread):
 * empty string = clear the selection. Duplicate pushes of the same text
 * (including while our own source is in place) only update the cache. */
void awl_datadev_android_clip(const char* utf8) {
    if (!g_clip_src) {
        g_clip_src = awl_datadev_internal_source(clip_fill_fd);
        if (!g_clip_src) return;
        awl_datadev_internal_mime(g_clip_src, "text/plain;charset=utf-8");
        awl_datadev_internal_mime(g_clip_src, "text/plain");
    }
    if (!utf8) utf8 = "";
    char* nt = strdup(utf8);
    if (!nt) return;
    pthread_rwlock_rdlock(&g_srv.rwl);
    pthread_mutex_lock(&g_srv.dd_lock);
    int quiet = g_selection == g_clip_src && g_clip_text && !strcmp(g_clip_text, utf8);
    struct awl_data_source* old = g_selection;
    free(g_clip_text);
    g_clip_text = nt;
    if (!quiet)
        g_selection = *utf8 ? g_clip_src : NULL;
    pthread_mutex_unlock(&g_srv.dd_lock);
    if (!quiet && old && old != g_clip_src && old->res) {   /* external source being displaced */
        wl_data_source_send_cancelled(old->res);
        wl_client_flush(client_of(old->res));
    }
    pthread_rwlock_unlock(&g_srv.rwl);
    if (!quiet)
        selection_notify_focus();
    LOGI("android clip in: %zu bytes%s", strlen(utf8), quiet ? " (dup)" : "");
}

/* ---- wl→Android text extraction (dedicated thread, keeps the client dispatch thread free) ---- */

#define AWL_CLIP_MAX (256 * 1024)   /* within the binder oneway limit */
#define AWL_CLIP_TIMEOUT_MS 500

struct clip_fetch {
    int fd;
    uint64_t win;
};

static void* clip_fetch_thread(void* arg) {
    struct clip_fetch* cf = arg;
    size_t len = 0, cap = 8192;
    char* data = malloc(cap);
    int eof = 0;
    if (data) {
        uint32_t deadline = awl_now_ms() + AWL_CLIP_TIMEOUT_MS;
        for (;;) {
            uint32_t now = awl_now_ms();
            if ((int32_t)(now - deadline) >= 0) break;
            struct pollfd pfd = { cf->fd, POLLIN, 0 };
            int r = poll(&pfd, 1, (int32_t)(deadline - now));
            if (r <= 0) { if (r == 0) break; if (errno == EINTR) continue; break; }
            char chunk[4096];
            ssize_t n = read(cf->fd, chunk, sizeof chunk);
            if (n < 0) { if (errno == EINTR || errno == EAGAIN) continue; break; }
            if (n == 0) { eof = 1; break; }
            if (len + (size_t)n + 1 > cap) {
                if (len + (size_t)n + 1 > AWL_CLIP_MAX) break;   /* over the limit, give up */
                cap = (len + (size_t)n + 1) * 2;
                char* nd = realloc(data, cap);
                if (!nd) break;
                data = nd;
            }
            memcpy(data + len, chunk, (size_t)n);
            len += (size_t)n;
        }
    }
    close(cf->fd);
    if (data && eof && len) {
        data[len] = 0;
        if (g_srv.cbs.clipboard_text)
            g_srv.cbs.clipboard_text(g_srv.cbs.user, cf->win, data);
    } else {
        LOGE("clip fetch: incomplete (eof=%d len=%zu) — dropped", eof, len);
    }
    free(data);
    free(cf);
    return NULL;
}

/* After set_selection, fetch the source text and push it to Android
 * (caller holds rwl.rd + dd_lock): pick a mime (utf-8 preferred) →
 * send_send + flush (the fd is dup'ed into the connection by the protocol
 * stack, our copy of the write end is closed right after → once the client
 * finishes writing and closes its copy, it is EOF) → a detached thread
 * reads. The owner toplevel window id lets the adapt layer locate the
 * Activity that writes the clipboard. */
static void selection_push_android_locked(struct awl_data_source* src) {
    if (!src || !src->res) return;   /* NULL source (clear): the caller sends the empty text directly */
    const char* mime = NULL;
    struct awl_mime* m;
    wl_list_for_each(m, &src->mimes, link) {
        if (!strcmp(m->name, "text/plain;charset=utf-8")) { mime = m->name; break; }
    }
    if (!mime)
        wl_list_for_each(m, &src->mimes, link) {
            if (!strcmp(m->name, "text/plain")) { mime = m->name; break; }
        }
    if (!mime)
        wl_list_for_each(m, &src->mimes, link) {
            if (!strncmp(m->name, "text/plain;", 11)) { mime = m->name; break; }
        }
    if (!mime) return;   /* plain-text bridge: non-text sources leave the Android clipboard untouched */

    int pfd[2];
    if (pipe2(pfd, O_CLOEXEC) != 0) return;
    wl_data_source_send_send(src->res, mime, pfd[1]);
    wl_client_flush(client_of(src->res));

    uint64_t win = 0;   /* first mapped toplevel of the source client */
    struct awl_surface* s;
    wl_list_for_each(s, &g_srv.surfaces, link) {
        if (s->role == AWL_ROLE_TOPLEVEL && s->mapped && s->resource &&
            wl_resource_get_client(s->resource) == src->client) {
            win = s->id;
            break;
        }
    }
    struct clip_fetch* cf = calloc(1, sizeof(*cf));
    if (cf) {
        cf->fd = pfd[0];
        cf->win = win;
        pthread_t t;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&t, &attr, clip_fetch_thread, cf) == 0) {
            pthread_attr_destroy(&attr);
            close(pfd[1]);   /* our copy of the write end: SCM_RIGHTS already dup'ed, closing it lets EOF reach the client side */
            return;
        }
        pthread_attr_destroy(&attr);
        free(cf);
    }
    close(pfd[0]);
    close(pfd[1]);
}

/* ---------------- selection notification ---------------- */

/* Every data device of the focus client: data_offer + mimes + selection (or
 * NULL). Holds rwl.wr throughout — offer_create needs the list insert;
 * enters lockless (takes its own locks inside). */
static void selection_notify_focus(void) {
    pthread_rwlock_wrlock(&g_srv.rwl);
    pthread_mutex_lock(&g_srv.dd_lock);
    struct awl_data_source* src = g_selection;
    struct wl_client* focus = g_dd_focus_client;
    pthread_mutex_unlock(&g_srv.dd_lock);
    if (!focus) {
        pthread_rwlock_unlock(&g_srv.rwl);
        return;
    }
    struct awl_data_device* dev;
    wl_list_for_each(dev, &g_srv.data_devices, link) {
        if (dev->client != focus || !dev->res) continue;
        if (src) {
            struct awl_data_offer* off = offer_create(
                    focus, wl_resource_get_version(dev->res), src, 0);
            if (off) {
                wl_data_device_send_data_offer(dev->res, off->res);
                offer_send_mimes(off);
                wl_data_device_send_selection(dev->res, off->res);
            }
        } else {
            wl_data_device_send_selection(dev->res, NULL);
        }
        wl_client_flush(focus);
    }
    pthread_rwlock_unlock(&g_srv.rwl);
}

/* Keyboard focus switch hook (awl_input.c tr_kbd_enter; caller holds no rwl
 * — record the client first, then notify; each stage locks on its own) */
void awl_datadev_focus_enter(struct wl_client* c) {
    pthread_mutex_lock(&g_srv.dd_lock);
    g_dd_focus_client = c;
    pthread_mutex_unlock(&g_srv.dd_lock);
    selection_notify_focus();
}

void awl_datadev_focus_leave(void) {
    pthread_mutex_lock(&g_srv.dd_lock);
    g_dd_focus_client = NULL;
    pthread_mutex_unlock(&g_srv.dd_lock);
}

/* ---------------- drag state machine (input is routed here; see the file header lock notes) ---------------- */

/* Switch the target layer (KWin updateDragTarget; caller holds rwl.wr +
 * dd_lock): leave old → new layer gets data_offer + all mimes +
 * source_actions + enter + action negotiation. Target with no data device /
 * surface with no resource → leave only. */
static void drag_update_target_locked(struct awl_surface* surface,
                                      float x, float y) {
    if (g_drag.target_dev && g_drag.target_dev->res) {
        wl_data_device_send_leave(g_drag.target_dev->res);
        wl_client_flush(g_drag.target_dev->client);
    }
    g_drag.target = NULL;
    g_drag.target_dev = NULL;
    g_drag.offer = NULL;   /* old offer resource belongs to the old target client, its destroy cleans up */

    if (!surface || !surface->resource) return;
    struct wl_client* tc = wl_resource_get_client(surface->resource);
    struct awl_data_device* dev;
    wl_list_for_each(dev, &g_srv.data_devices, link)
        if (dev->client == tc && dev->res) {
            struct awl_data_source* src = g_drag.src;
            if (src) source_accept(src, NULL);   /* KWin: a new target gets target(NULL) first */
            struct awl_data_offer* off = offer_create(
                    tc, wl_resource_get_version(dev->res), src, 1);
            if (!off) return;
            wl_data_device_send_data_offer(dev->res, off->res);
            offer_send_mimes(off);
            offer_send_actions(off);
            g_drag.target = surface;
            g_drag.target_dev = dev;
            g_drag.offer = off;
            uint32_t action = negotiate_action(src, off, g_drag.mods);
            offer_set_action(off, action);
            if (src) source_send_action(src, action);
            wl_data_device_send_enter(dev->res, wl_display_next_serial(g_srv.display),
                                      surface->resource,
                                      wl_fixed_from_double(x), wl_fixed_from_double(y),
                                      off->res);
            wl_client_flush(tc);
            LOGI("drag enter: surface=%llu action=%u",
                    (unsigned long long)surface->id, action);
            return;
        }
}

/* Re-negotiate on input change (source/offer set_actions, modifiers; inside rwl + dd_lock) */
static void drag_match_actions_locked(void) {
    if (!g_drag.active || !g_drag.offer || !g_drag.target_dev) return;
    uint32_t action = negotiate_action(g_drag.src, g_drag.offer, g_drag.mods);
    offer_set_action(g_drag.offer, action);
    if (g_drag.src) source_send_action(g_drag.src, action);
}

static void drag_icon_move_locked(struct awl_surface* root, float bx, float by) {
    struct awl_surface* icon = g_drag.icon;
    if (!icon) return;
    pthread_mutex_lock(&icon->ev_lock);
    icon->sub_x = (int32_t)bx;   /* attached to the root layer: root buffer coords are the layer position */
    icon->sub_y = (int32_t)by;
    icon->sub_pos_pending = 0;
    pthread_mutex_unlock(&icon->ev_lock);
}

/* Drag teardown (leave + clear state; does not unlink the icon — topology belongs to the caller's wr section; inside dd_lock) */
static void drag_end_common_locked(void) {
    if (g_drag.target_dev && g_drag.target_dev->res) {
        wl_data_device_send_leave(g_drag.target_dev->res);
        wl_client_flush(g_drag.target_dev->client);
    }
    g_drag.active = 0;
    g_drag.target = NULL;
    g_drag.target_dev = NULL;
    g_drag.offer = NULL;
    g_drag.src = NULL;
    g_drag.origin = NULL;
    g_drag.origin_win = 0;
    g_drag.mods = 0;
}

/* Release (button/touch up; KWin endDrag; inside rwl.wr + dd_lock):
 * accepted → drop + dnd_drop_performed; else dnd_drop_performed + cancelled. */
static void drag_drop_locked(void) {
    struct awl_data_source* src = g_drag.src;
    struct awl_data_device* dev = g_drag.target_dev;
    if (src) {
        src->drop_performed = 1;
        if (src->res &&
            wl_resource_get_version(src->res) >= WL_DATA_SOURCE_DND_DROP_PERFORMED_SINCE_VERSION)
            wl_data_source_send_dnd_drop_performed(src->res);
        if (dev && dev->res && src->accepted) {
            wl_data_device_send_drop(dev->res);
            wl_client_flush(dev->client);
        } else if (src->res &&
                   wl_resource_get_version(src->res) >= WL_DATA_SOURCE_ACTION_SINCE_VERSION) {
            wl_data_source_send_cancelled(src->res);
        }
        if (src->res) wl_client_flush(client_of(src->res));
    }
    drag_end_common_locked();
}

/* Cancel (touch cancel / source death / origin death; KWin cancelDrag; inside dd_lock) */
static void drag_cancel_locked(void) {
    if (g_drag.src && g_drag.src->res) {
        wl_data_source_send_cancelled(g_drag.src->res);
        wl_client_flush(client_of(g_drag.src->res));
    }
    drag_end_common_locked();
}

/* ---- input hooks (called from awl_input.c; neither party holds rwl — wr taken inside) ---- */

int awl_datadev_drag_active(void) {
    return g_drag.active;   /* single read; a race only sends one event down the normal path, harmless */
}

/* icon layer id (for hit-test exclusion; 0 = none) */
uint64_t awl_datadev_drag_icon_id(void) {
    pthread_mutex_lock(&g_srv.dd_lock);
    uint64_t id = (g_drag.active && g_drag.icon) ? g_drag.icon->id : 0;
    pthread_mutex_unlock(&g_srv.dd_lock);
    return id;
}

/* motion: the caller resolved everything under rwl.rd (hit_id + root buffer
 * coords + layer-local coords) and calls with the lock released. References
 * are rebuilt by id under wr inside — destroyed during the rd→wr gap means
 * no target. */
void awl_datadev_drag_motion(uint64_t win, uint64_t hit_id,
                             float bx, float by, float lx, float ly) {
    uint64_t dirty_win = 0;
    pthread_rwlock_wrlock(&g_srv.rwl);
    pthread_mutex_lock(&g_srv.dd_lock);
    if (g_drag.active && win == g_drag.origin_win) {
        struct awl_surface* root = awl_surface_by_id(win);
        struct awl_surface* hit = hit_id ? awl_surface_by_id(hit_id) : NULL;
        if (root) {
            dirty_win = root->mapped ? win : 0;
            drag_icon_move_locked(root, bx, by);
            if (hit && hit != g_drag.target)
                drag_update_target_locked(hit, lx, ly);
            else if (g_drag.target_dev && g_drag.target_dev->res) {
                wl_data_device_send_motion(g_drag.target_dev->res, awl_now_ms(),
                                           wl_fixed_from_double(lx),
                                           wl_fixed_from_double(ly));
                wl_client_flush(g_drag.target_dev->client);
            }
        }
    }
    pthread_mutex_unlock(&g_srv.dd_lock);
    pthread_rwlock_unlock(&g_srv.rwl);
    if (dirty_win && g_srv.cbs.window_dirty)
        g_srv.cbs.window_dirty(g_srv.cbs.user, dirty_win);   /* icon tracks the pointer */
}

void awl_datadev_drag_end(void) {
    uint64_t dirty_win = 0;
    pthread_rwlock_wrlock(&g_srv.rwl);
    pthread_mutex_lock(&g_srv.dd_lock);
    if (g_drag.active) {
        if (g_drag.origin) {
            struct awl_surface* root = awl_subsurface_root(g_drag.origin);
            dirty_win = root->mapped ? root->id : 0;
        }
        struct awl_surface* icon = g_drag.icon;
        drag_drop_locked();
        g_drag.icon = NULL;
        if (icon && icon->sub_parent) {
            wl_list_remove(&icon->sub_link);
            icon->sub_parent = NULL;
        }
    }
    pthread_mutex_unlock(&g_srv.dd_lock);
    pthread_rwlock_unlock(&g_srv.rwl);
    if (dirty_win && g_srv.cbs.window_dirty)
        g_srv.cbs.window_dirty(g_srv.cbs.user, dirty_win);
    LOGI("drag end");
}

void awl_datadev_drag_cancel(void) {
    uint64_t dirty_win = 0;
    pthread_rwlock_wrlock(&g_srv.rwl);
    pthread_mutex_lock(&g_srv.dd_lock);
    if (g_drag.active) {
        if (g_drag.origin) {
            struct awl_surface* root = awl_subsurface_root(g_drag.origin);
            dirty_win = root->mapped ? root->id : 0;
        }
        struct awl_surface* icon = g_drag.icon;
        drag_cancel_locked();
        g_drag.icon = NULL;
        if (icon && icon->sub_parent) {
            wl_list_remove(&icon->sub_link);
            icon->sub_parent = NULL;
        }
        LOGI("drag cancelled");
    }
    pthread_mutex_unlock(&g_srv.dd_lock);
    pthread_rwlock_unlock(&g_srv.rwl);
    if (dirty_win && g_srv.cbs.window_dirty)
        g_srv.cbs.window_dirty(g_srv.cbs.user, dirty_win);
}

void awl_datadev_key_mods(uint32_t mods) {
    pthread_rwlock_rdlock(&g_srv.rwl);
    pthread_mutex_lock(&g_srv.dd_lock);
    if (g_drag.active && g_drag.mods != mods) {
        g_drag.mods = mods;
        drag_match_actions_locked();
    }
    pthread_mutex_unlock(&g_srv.dd_lock);
    pthread_rwlock_unlock(&g_srv.rwl);
}

/* Surface death (called from the rwl.wr section of awl_surface.c
 * surface_destroy_impl): clear the icon reference (unlinking is done by the
 * sub_parent path right after) / leave the target / origin death → cancel
 * the drag. */
void awl_datadev_surface_gone(struct awl_surface* s) {
    pthread_mutex_lock(&g_srv.dd_lock);
    if (g_drag.active) {
        if (g_drag.icon == s)
            g_drag.icon = NULL;
        if (g_drag.target == s) {
            if (g_drag.target_dev && g_drag.target_dev->res) {
                wl_data_device_send_leave(g_drag.target_dev->res);
                wl_client_flush(g_drag.target_dev->client);
            }
            g_drag.target = NULL;
            g_drag.target_dev = NULL;
            g_drag.offer = NULL;
        }
        if (g_drag.origin == s)
            drag_cancel_locked();
    }
    pthread_mutex_unlock(&g_srv.dd_lock);
}

/* ---------------- wl_data_device_manager ---------------- */

static void ddm_create_data_source(struct wl_client* c, struct wl_resource* res,
                                   uint32_t id) {
    struct wl_resource* sres = wl_resource_create(
            c, &wl_data_source_interface, wl_resource_get_version(res), id);
    if (!sres) { wl_resource_post_no_memory(res); return; }
    struct awl_data_source* src = calloc(1, sizeof(*src));
    if (!src) { wl_resource_destroy(sres); wl_resource_post_no_memory(res); return; }
    src->res = sres;
    src->client = c;
    wl_list_init(&src->mimes);
    pthread_rwlock_wrlock(&g_srv.rwl);
    wl_list_insert(g_srv.data_sources.prev, &src->link);
    pthread_rwlock_unlock(&g_srv.rwl);
    wl_resource_set_implementation(sres, &source_iface, src, ds_res_destroy);
}

static void ddm_get_data_device(struct wl_client* c, struct wl_resource* res,
                                uint32_t id, struct wl_resource* seat_res) {
    struct wl_resource* dres = wl_resource_create(
            c, &wl_data_device_interface, wl_resource_get_version(res), id);
    if (!dres) { wl_resource_post_no_memory(res); return; }
    struct awl_data_device* dev = calloc(1, sizeof(*dev));
    if (!dev) { wl_resource_destroy(dres); wl_resource_post_no_memory(res); return; }
    dev->res = dres;
    dev->client = c;
    pthread_rwlock_wrlock(&g_srv.rwl);
    wl_list_insert(g_srv.data_devices.prev, &dev->link);
    wl_resource_set_implementation(dres, &device_iface, dev, dd_res_destroy);
    /* KWin: focus client's device created late → re-deliver the current selection */
    pthread_mutex_lock(&g_srv.dd_lock);
    struct awl_data_source* src = g_selection;
    int focused = c == g_dd_focus_client;
    pthread_mutex_unlock(&g_srv.dd_lock);
    if (focused && src) {
        struct awl_data_offer* off = offer_create(
                c, wl_resource_get_version(dres), src, 0);
        if (off) {
            wl_data_device_send_data_offer(dres, off->res);
            offer_send_mimes(off);
            wl_data_device_send_selection(dres, off->res);
            wl_client_flush(c);
        }
    }
    pthread_rwlock_unlock(&g_srv.rwl);
}

static const struct wl_data_device_manager_interface ddm_iface = {
    .create_data_source = ddm_create_data_source,
    .get_data_device = ddm_get_data_device,
};

static void ddm_bind(struct wl_client* client, void* data,
                     uint32_t version, uint32_t id) {
    uint32_t v = version < AWL_DDM_VERSION ? version : AWL_DDM_VERSION;
    struct wl_resource* res = wl_resource_create(
            client, &wl_data_device_manager_interface, v, id);
    if (!res) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(res, &ddm_iface, NULL, NULL);
}

void awl_datadev_setup(void) {
    wl_list_init(&g_srv.data_devices);
    wl_list_init(&g_srv.data_sources);
    wl_list_init(&g_srv.data_offers);
    pthread_mutex_init(&g_srv.dd_lock, NULL);
    g_srv.g_data_device_manager = wl_global_create(
            g_srv.display, &wl_data_device_manager_interface,
            AWL_DDM_VERSION, NULL, ddm_bind);
    if (!g_srv.g_data_device_manager)
        LOGE("wl_data_device_manager global create failed");
}
