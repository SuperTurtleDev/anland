/* awl_idle.c — zwp_idle_inhibit_manager_v1 (idle-inhibit-unstable-v1)
 *
 * Pure state sync (same design as the pointer constraints in awl_input.c):
 * the inhibitor is recorded per surface and aggregated per root window; only
 * transitions (0→any / any→0 live inhibitors on a root) invoke
 * cbs.idle_inhibit → the adaptation layer tells the Activity over C_KEEPON
 * to set/clear FLAG_KEEP_SCREEN_ON.
 *
 * Visibility ("the inhibitor is honored only while the surface is visible",
 * idle-inhibit-unstable-v1.xml) is NOT tracked here: the Android window flag
 * carries exactly that semantics natively (a minimized Activity has no
 * window → the flag is gone; an occluded window's flag is not adopted by
 * PowerManager), so the daemon needs no occlusion/mirror state.
 *
 * The protocol itself is tiny: manager (destroy/create_inhibitor) +
 * inhibitor object with a single destroy request and no events.
 *
 * Locking (mirror of the constraint section in awl_input.c):
 *   create_inhibitor     rwl(rd) → g_inhib_lock; the callback fires after the
 *                        rwl release (callbacks never run under a logic lock)
 *   resource destroy     g_inhib_lock ONLY — never rwl (teardown may run
 *                        inside wl_map destruction while another thread
 *                        holds rwl)
 *   awl_idle_surface_gone  caller holds rwl.wr (awl_surface.c destroy path);
 *                        the returned root is notified AFTER the rwl release
 */
#include "awl_internal.h"
#include "idle-inhibit-unstable-v1-server-protocol.h"

struct awl_inhib {
    struct wl_resource* res;     /* zwp_idle_inhibitor_v1 */
    struct wl_client* client;
    uint64_t surface_id;         /* inhibiting surface (bookkeeping) */
    uint64_t root_id;            /* root window (C_KEEPON address) */
    int dead;                    /* surface gone: object stays until the client destroys it */
    struct wl_list link;
};

static struct wl_list g_inhibs;
static pthread_mutex_t g_inhib_lock = PTHREAD_MUTEX_INITIALIZER;

/* Live-inhibitor count of a root window (caller holds g_inhib_lock) */
static int inhib_count_live(uint64_t root_id) {
    int n = 0;
    struct awl_inhib* it;
    wl_list_for_each(it, &g_inhibs, link) {
        if (!it->dead && it->root_id == root_id) n++;
    }
    return n;
}

static void inhib_destroy(struct wl_client* client, struct wl_resource* res) {
    wl_resource_destroy(res);
}

/* Resource teardown (destroy request / client disconnect / wl_map teardown):
 * takes g_inhib_lock ONLY. Live aggregate any→0 → idle_inhibit off (dead
 * objects never counted: their off was pushed at surface-gone time). */
static void inhib_res_destroy(struct wl_resource* res) {
    struct awl_inhib* in = wl_resource_get_user_data(res);
    if (!in) return;
    pthread_mutex_lock(&g_inhib_lock);
    wl_list_remove(&in->link);
    int dead = in->dead;
    uint64_t root = in->root_id;
    int still_live = dead ? 1 : inhib_count_live(root);
    pthread_mutex_unlock(&g_inhib_lock);
    free(in);
    if (dead || still_live || !g_srv.cbs.idle_inhibit) return;
    g_srv.cbs.idle_inhibit(g_srv.cbs.user, root, 0);
    LOGI("idle inhibitor gone: window %llu keep-screen-on off",
         (unsigned long long)root);
}

static const struct zwp_idle_inhibitor_v1_interface inhib_iface = {
    .destroy = inhib_destroy,
};

static void iimgr_create_inhibitor(struct wl_client* client,
                                   struct wl_resource* mgr, uint32_t id,
                                   struct wl_resource* surface_res) {
    pthread_rwlock_rdlock(&g_srv.rwl);
    struct awl_surface* s = surface_res ? wl_resource_get_user_data(surface_res) : NULL;

    /* aggregate before insertion (rwl.rd held; g_inhib_lock inner, like constr) */
    uint64_t root = 0;
    int first = 0;
    if (s) {
        root = awl_subsurface_root(s)->id;
        pthread_mutex_lock(&g_inhib_lock);
        first = inhib_count_live(root) == 0;
        pthread_mutex_unlock(&g_inhib_lock);
    }

    struct wl_resource* obj = wl_resource_create(
            client, &zwp_idle_inhibitor_v1_interface, 1, id);
    struct awl_inhib* in = calloc(1, sizeof(*in));
    if (!obj || !in) {
        if (obj) wl_resource_destroy(obj);
        free(in);
        pthread_rwlock_unlock(&g_srv.rwl);
        wl_resource_post_no_memory(mgr);
        return;
    }
    in->res = obj;
    in->client = client;
    if (!s) {
        in->dead = 1;   /* surface already destroyed (unreachable in practice) */
    } else {
        in->surface_id = s->id;
        in->root_id = root;
    }
    /* always linked (dead ones too): inhib_res_destroy removes
     * unconditionally — an unlinked entry would crash the teardown */
    pthread_mutex_lock(&g_inhib_lock);
    wl_list_insert(g_inhibs.prev, &in->link);
    pthread_mutex_unlock(&g_inhib_lock);
    wl_resource_set_implementation(obj, &inhib_iface, in, inhib_res_destroy);
    pthread_rwlock_unlock(&g_srv.rwl);
    LOGI("idle inhibitor: window %llu keep-screen-on %s",
         (unsigned long long)root,
         !s ? "(dead surface, ignored)" : (first ? "on" : "(already on)"));
    if (first && g_srv.cbs.idle_inhibit)
        g_srv.cbs.idle_inhibit(g_srv.cbs.user, root, 1);
}

static void iimgr_destroy(struct wl_client* client, struct wl_resource* res) {
    wl_resource_destroy(res);
}

static const struct zwp_idle_inhibit_manager_v1_interface iimgr_iface = {
    .destroy = iimgr_destroy,
    .create_inhibitor = iimgr_create_inhibitor,
};

static void iimgr_bind(struct wl_client* client, void* data,
                       uint32_t version, uint32_t id) {
    struct wl_resource* res = wl_resource_create(
            client, &zwp_idle_inhibit_manager_v1_interface, 1, id);
    if (!res) { wl_client_post_no_memory(client); return; }
    wl_resource_set_implementation(res, &iimgr_iface, NULL, NULL);
}

void awl_idle_setup(void) {
    wl_list_init(&g_inhibs);
    if (!wl_global_create(g_srv.display,
                          &zwp_idle_inhibit_manager_v1_interface,
                          1, NULL, iimgr_bind))
        LOGE("zwp_idle_inhibit_manager_v1 global create failed");
}

/* ---- surface death (awl_surface.c destroy path; caller holds rwl.wr) ----
 * Inhibitors on this surface / its root die with it (the object stays until
 * the client destroys it — destroying it here would recurse into
 * inhib_res_destroy under rwl.wr). Returns the root window whose aggregate
 * flipped to 0 (0 = none) for awl_idle_gone_notify AFTER the rwl release. */
uint64_t awl_idle_surface_gone(struct awl_surface* s) {
    pthread_mutex_lock(&g_inhib_lock);
    uint64_t root = 0;
    struct awl_inhib* in;
    struct awl_inhib* tmp;
    wl_list_for_each_safe(in, tmp, &g_inhibs, link) {
        if (in->dead || (in->surface_id != s->id && in->root_id != s->id)) continue;
        in->dead = 1;
        root = in->root_id;
    }
    if (root && inhib_count_live(root) > 0)
        root = 0;   /* sibling inhibitors keep the window's aggregate on */
    pthread_mutex_unlock(&g_inhib_lock);
    return root;
}

void awl_idle_gone_notify(uint64_t win) {
    if (!win || !g_srv.cbs.idle_inhibit) return;
    g_srv.cbs.idle_inhibit(g_srv.cbs.user, win, 0);
    LOGI("idle inhibitor gone (surface destroyed): window %llu keep-screen-on off",
         (unsigned long long)win);
}
