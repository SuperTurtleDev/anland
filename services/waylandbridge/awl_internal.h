/* awl_internal.h — internal shared definitions for the logic layer (v2 window-driven) */
#ifndef AWL_INTERNAL_H
#define AWL_INTERNAL_H

#include "awl.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <wayland-server-core.h>
#include "wayland-server-protocol-core.h"
#include "linux-dmabuf-unstable-v1-server-protocol.h"
#include "xdg-shell-server-protocol.h"
#include "awl_bufferqueue.h"
#include "awl_log.h"   /* AWL_TAG "anland-wl" + LOGI/LOGE/LOGD (see awl_log.h) */

/* Damage state of a surface since a composition backend last uploaded its
 * wl_shm content (awl_surface.cd_state; NONE = calloc default) */
enum {
    AWL_DMG_NONE = 0,
    AWL_DMG_RECT = 1,
    AWL_DMG_FULL = 2,
};

#define awl_fourcc(a, b, c, d) \
    ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | \
     ((uint32_t)(d) << 24))
#define AWL_FORMAT_ARGB8888 awl_fourcc('A', 'R', '2', '4')  /* [B,G,R,A] */
#define AWL_FORMAT_XRGB8888 awl_fourcc('X', 'R', '2', '4')  /* [B,G,R,X] */

/* drm_fourcc.h is missing (bionic); define the modifier constants ourselves */
#ifndef DRM_FORMAT_MOD_INVALID
#define DRM_FORMAT_MOD_INVALID 0x00ffffff00000000ULL
#endif
#ifndef DRM_FORMAT_MOD_LINEAR
#define DRM_FORMAT_MOD_LINEAR 0ULL
#endif

static inline uint32_t awl_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

/* ---- surface roles / xdg state ---- */
enum awl_role {
    AWL_ROLE_NONE = 0,
    AWL_ROLE_TOPLEVEL,
    AWL_ROLE_POPUP,
    AWL_ROLE_SUBSURFACE,   /* wl_subcompositor.get_subsurface — attached to the
                              parent window for compositing */
    AWL_ROLE_XWAYLAND,     /* xwayland_surface_v1 (Xwayland rootless window,
                              no configure state machine, #32) */
    AWL_ROLE_CURSOR,       /* wl_pointer.set_cursor cursor image (kwin
                              "cursor" SurfaceRole): never maps a window, never
                              enters the layer stack / hit-testing; composited
                              by the renderer above all layers of the
                              pointer-focused window (awl_input.c) */
};

/* wl_buffer wrapper for the dmabuf side (shm buffers are self-managed by
 * libwayland and not wrapped). Refcounted: the resource holds one reference,
 * every queued frame (awl_bufferqueue element cookie) holds another — the
 * wrapper outlives a client-side wl_buffer.destroy while its frames are
 * still in flight. `resource` is cleared by the destroy handler under
 * g_bufref_lock; wl_buffer.release is only ever sent under that lock. */
struct awl_buffer {
    struct wl_resource* resource;    /* wl_buffer (created by us); NULL = destroyed (g_bufref_lock) */
    atomic_int refs;
    int dmabuf_fd;                   /* owned after dup */
    uint64_t ino;                    /* dma-buf inode at creation — render-side
                                      * identity without a per-frame fstat
                                      * (0 = fstat failed here, consumers
                                      * fall back to their own) */
    uint32_t width, height, stride;  /* stride: bytes */
    uint32_t drm_format;
};

struct awl_frame_cb {
    struct wl_resource* resource;
    struct awl_surface* s;           /* owner for backfill (destroy listener takes lock via it) */
    int detached;                    /* done sent; parked until dispatch-thread destruction */
    struct wl_list link;
};

/* Field order = alignment groups: 8-byte members (serials/pointers/lists/
 * ev_lock/role union) first, then 4-byte state, then the flags, the plain
 * int fds/atomic, and the 1-bit tail word — zero internal padding
 * (pahole-verified arm64/bionic: 560B; history: 976B original → 688B after
 * the alignment-group pass → 560B after dead-field removal, the role union
 * and the two flag words; +32B since for the four wl_region slots, #85).
 * Rules for new fields:
 *   - role-exclusive state → the union below (init at role assignment);
 *   - a 1-bit flag → the W1/W2 word matching its writer threads;
 *   - everything else → the group matching its size. */
struct awl_surface {
    uint64_t id;                     /* window id (globally unique; same id on the Java side) */
    struct wl_resource* resource;
    struct wl_list link;             /* server.surfaces */
    pthread_mutex_t ev_lock;         /* per-window event send lock (recursive): fields + send order for that client */

    struct wl_resource* xdg_surface_res;    /* associated xdg_surface. Deliberately NOT in the
                                              * role union below: it stays set while role == NONE
                                              * (toplevel destroyed, xdg_surface object alive) and
                                              * the surface-destroy strip reads it in that window. */
    char* title;                     /* heap: toplevel/Xwayland title (awl_surface_set_title; NULL = none —
                                      * client dispatch thread only, the render thread never reads it) */

    /* ---- zoom (#31: wp_viewporter + wp_fractional_scale_v1, kwin-isomorphic) ----
     * Coordinate model: all layer-stack/geometry/popup/input coords = logical
     * pixels (surface-local). Layer logical size = viewport dst | source |
     * buffer/buf_scale (isomorphic to kwin SurfaceInterfacePrivate::applyState
     * surfaceSize); toplevel logical size = phys/zoom (the configure-issued
     * value). viewport state is double-buffered, applied on the same commit
     * as the buffer (kwin pending→current). Sizes/uv floats: 4-byte group. */
    struct wl_resource* viewport_res;/* wp_viewport (at most 1 per surface; NULL=none) */
    struct wl_resource* frac_res;    /* zwp_fractional_scale_v1 (at most 1 per surface) */

    /* subsurface role (chrome WaylandBubble=tooltip/selection handles, GTK4 popover):
     * Topology is owned by g_srv.rwl. Like KWin, each parent has current
     * below/above stacks around its own content and a pending copy for
     * place_above/place_below. `sub_children` owns every direct child; it is
     * independent of z order.
     * sub_x/sub_y are owned by this surface's ev_lock (render snapshot reads,
     * set_position writes) — positions/flags live in the 4-byte group below.
     *
     * sync semantics aligned with kwin-6.6.5 (src/wayland/{subcompositor,surface}.cpp):
     *   - sub_sync defaults to 1 (protocol default sync); the effective value
     *     recurses up the ancestor chain (KWin SubSurfaceInterface::isSynchronized);
     *   - a commit of an effectively-sync child latches into the latched slot;
     *     the parent commit cascades the application (KWin subsurface.transaction + merge);
     *   - set_position is double-buffered, applied on parent commit (KWin parentApplyState).
     * These flags/slots are read/written only on the client's dispatch thread
     * (a subtree always belongs to one client) — no lock; latched_buffer_res
     * and current are read concurrently (render thread) under ev_lock. */
    struct awl_surface* sub_parent;
    struct wl_list sub_children;          /* all direct children, ownership only */
    struct wl_list sub_below, sub_above;  /* current stack around this surface */
    struct wl_list pend_sub_below, pend_sub_above; /* pending parent state */
    struct wl_list sub_child_link;        /* linked into sub_parent->sub_children */
    struct wl_list sub_link;              /* linked into current below/above */
    struct wl_list sub_pend_link;         /* linked into pending below/above */

    /* Double-buffered state. Protocol semantics (2026-09-09 black-screen
     * deadlock, verified): pending state persists across commits — a commit
     * without attach does not change the buffer; only an explicit
     * attach(NULL)+commit detaches. pending_attached marks whether this
     * cycle attached (empty commit / ack commit must not clear current). */
    struct wl_resource* pending_buffer_res;
    struct wl_resource* current_buffer_res;

    struct wl_list frame_callbacks;

    /* ---- wl_surface.set_input_region / set_opaque_region (#85) ----
     * Double-buffered: pend_* is the wl_region snapshot taken at request time
     * (the client may destroy the wl_region right after), promoted to the
     * current slot on commit (direct commit: surface_commit; sync subsurface:
     * the parent-commit apply in awl_subsurface.c — the pending slot keeps
     * accumulating until then, same shape as damage). NULL current = protocol
     * default (input: infinite → the whole layer hits; opaque: nothing). A
     * non-NULL snapshot with zero rectangles = EXPLICITLY EMPTY: an input
     * region like that makes the layer transparent to hit-testing — the case
     * Firefox/LibreWolf's WebRender subsurface relies on (2026-09-19 #85: the
     * clicks landed on the render child instead of the GTK toplevel).
     * Slots owned by ev_lock (hit-test reads on the input thread). */
    struct awl_region* input_region;
    struct awl_region* pend_input_region;
    struct awl_region* opaque_region;
    struct awl_region* pend_opaque_region;

    /* ---- frame stream (awl_bufferqueue.h) ----
     * Every presented dmabuf state change of this surface becomes one queue
     * element: dmabuf commits push the client's fd + acquire fence directly
     * (commit path, awl_surface_apply_buffer); attach(NULL) pushes a NULL
     * marker, and so does the first shm commit after a dmabuf one (the
     * consumer must stop showing the stale dmabuf head). The backend is the
     * consumer (lock → drain → gethead → GL/SC → unlock). wl_buffer.release
     * / zwp_linux_buffer_release_v1 go out when the element leaves the queue
     * (bq_release_cb in awl_surface.c) — never from a "presented" hook. */
    struct awl_bufferqueue* q;

    /* ---- wl_shm frame source ----
     * A wl_shm commit never enters the queue and is never copied here: the
     * backend reads the client's pool at frame time (awl_surface_shm_begin/
     * end, this ev_lock held across the upload — the damage rect only, into
     * its own GPU texture) and wl_buffer.release goes out when that read is
     * done. All ev_lock. */
    uint64_t shm_serial;             /* bumps on every commit that changed shm content (attach or damage); a consumer skips unchanged */
    struct wl_resource* shm_res;     /* the shm wl_buffer whose release is owed (NULL = none / destroyed) */
    struct wl_resource* shm_release_res;   /* zwp_linux_buffer_release_v1 of that commit (immediate_release once read; cleared by awl_surface_shm_release_gone) */

    /* ---- zwp_linux_explicit_synchronization_v1 (awl_esync.c) ----
     * Double-buffered like the buffer itself: pend_* is applied on the commit
     * that carries the attach (sync-subsurface latching moves it to
     * latched_*). Ownership of an acquire fd moves into the queue element. */
    struct wl_resource* sync_res;            /* zwp_linux_surface_synchronization_v1 (≤ 1) */
    struct wl_resource* pend_release_res;    /* zwp_linux_buffer_release_v1 for this cycle */
    struct wl_list esync_all;                /* every live release object of this surface
                                              * (awl_esync_release::all_link; dispatch thread) */
    struct wl_list esync_gc;                 /* delivered ones awaiting wl_resource_destroy on
                                              * the dispatch thread (gc_link; g_bufref_lock) */

    /* ---- role-exclusive state (discriminated by `role` below) ----
     * Exactly one branch is meaningful. A branch is (re-)initialized at the
     * moment its role is assigned (xdg get_toplevel/get_popup,
     * get_xwayland_surface, get_subsurface); between a role-object destroy
     * (role → NONE) and the next role assignment the words may still hold
     * the previous branch, so cross-object cleanup paths MUST gate on
     * role == <branch's role> before reading or writing a branch (see
     * awl_esync.c release/destroy, awl_surface_buffer_gone,
     * surface_destroy_impl). The role-object destroy handlers clear their
     * branch's pointers before role → NONE. */
    union {
        struct {                            /* TOPLEVEL | POPUP */
            struct wl_resource* role_res;   /* xdg_toplevel / xdg_popup */
            int32_t conf_w, conf_h;         /* most recent configure contents */
            int32_t pend_w, pend_h;         /* cached when resize precedes map (Android owns sizing entirely) */
        } xdg;
        struct {                            /* XWAYLAND */
            struct wl_resource* res;        /* xwayland_surface_v1 (#32) */
            uint64_t serial;                /* association serial from set_serial (= the X-side
                                              * WL_SURFACE_SERIAL ClientMessage; mini-wm pairs
                                              * the X window by this; 0=not associated) */
        } xway;
        struct {                            /* SUBSURFACE */
            struct wl_resource* subsurface_res;    /* wl_subsurface object (drop the reference
                                                    * if the surface dies first) */
            struct wl_resource* latched_buffer_res;   /* latched buffer (never sampled) */
            struct wl_resource* latched_release_res;  /* esync release object latched with it */
            int32_t latched_acquire_fd;     /* -1 = none */
            int32_t pend_x, pend_y;         /* set_position double-buffered value (applied on parent commit) */
        } sub;
    } u;

    /* ---- 4-byte state: xdg configure / geometry / zoom sizes / uv / damage ---- */
    enum awl_role role;
    /* xdg window geometry (buffer coords, double-buffered, applied on commit)
     * = the window's visible content region (the chrome buffer carries
     * 16/10px shadow margins; geometry states where the content sits). The
     * render dst and the input view→buffer mapping share this origin —
     * ignoring it misplaces content to bottom-right and systematically
     * offsets input coords (2026-09-09 restore-bubble unclickable, verified). */
    int32_t geom_x, geom_y, geom_w, geom_h;
    int32_t pend_gx, pend_gy, pend_gw, pend_gh;
    int32_t phys_w, phys_h;          /* Android window size (recorded by awl_window_resize; 0=unknown) */
    int32_t buf_scale;               /* wl_surface.set_buffer_scale (default 1; bookkeeping only) */
    int32_t buf_transform;           /* wl_surface.set_buffer_transform, current (wl_output.transform
                                      * 0..7; 90/270 swap the logical size). Applied on commit like
                                      * viewport state — the render side reads it per frame. */
    int32_t pend_buf_transform;      /* -1 = nothing pending */
    int32_t vp_dst_w, vp_dst_h;      /* viewport dst logical size (0=unset) */
    int32_t pend_vpd_w, pend_vpd_h;
    float vp_sx, vp_sy, vp_sw, vp_sh;      /* source rectangle (buffer×buf_scale coords) */
    float pend_vps_x, pend_vps_y, pend_vps_w, pend_vps_h;
    int32_t sub_x, sub_y;                 /* applied position (buffer pixels, Y down) */
    int32_t pending_offset_x, pending_offset_y;
    /* damage (wl_surface.damage/damage_buffer accumulated in pending —
     * surface-local px, bbox merge; moved to cur on the commit/latch-apply).
     * cur_* = damage since a backend last uploaded the wl_shm content
     * (awl_surface_shm_begin/end) — dmabuf layers never read it (the GPU
     * samples the memory in place):
     *   NONE    nothing changed since the last upload
     *   RECT    the bbox rect needs re-uploading
     *   FULL    a commit attached a buffer with NO damage — protocol
     *           default: whole surface (client gave no information)
     * Over-copy is always safe, under-copy never. All owned by ev_lock. */
    int32_t pd_x, pd_y, pd_w, pd_h;
    int32_t cur_damage_x, cur_damage_y, cur_damage_w, cur_damage_h;

    /* ---- flags: two bit-field words, split BY WRITER THREAD ----
     * A bit-field write is a word-wide read-modify-write, so all writers of
     * one word must be serialized with each other (readers may be loose:
     * they see the whole word atomically). The two groups MUST stay separated
     * by the plain (non-bit-field) members below: compilers DO merge adjacent
     * bit-fields of different base types into one allocation unit (verified:
     * aarch64 gcc and clang both fold unsigned:1+bool:1 neighbors into one
     * 32-bit word), and only a non-bit-field member between them ends the
     * unit. W2 (unsigned): writers on several threads — binder render/resize
     * callbacks and the client dispatch thread — but EVERY access under
     * ev_lock.
     *
     * Cacheline note (false-sharing audit): this tail region (damage rect +
     * flag words, ~[512,576)) is written cross-core on the shm frame path —
     * the dispatch thread merges damage here under ev_lock and the render
     * thread resets cd_state/cur_damage at upload (also under ev_lock), plus
     * the lock-free W1 bits ride the same line. That is TRUE sharing of the
     * damage-retire state (a per-frame producer→consumer handoff), not false
     * sharing: separating the words into different lines would not remove
     * the bounce, it would only add bytes. The dmabuf frame path never
     * touches this region from the render side (all render traffic goes
     * through awl_bufferqueue, whose OWN lines are writer-grouped — see
     * awl_bufferqueue.c). */
    unsigned configured : 1;            /* a configure has been sent */
    unsigned has_pending : 1;           /* resize cached before map */
    unsigned activated : 1;             /* xdg ACTIVATED (Android foreground focus) */
    unsigned shm_live : 1;              /* committed content is a shm buffer (cleared by
                                          * attach(NULL) / a dmabuf attach) */
    unsigned shm_release_pending : 1;   /* wl_buffer.release for shm_res not sent yet */
    unsigned q_last_dmabuf : 1;         /* the newest push was a dmabuf frame */
    unsigned cd_state : 2;              /* AWL_DMG_* (NONE = 0, calloc-init) */
    /* plain ints (real values / addressable — never bit-fields). These also
     * physically separate the two flag words — do not move them. */
    int pend_acquire_fd;                /* -1 = none */
    atomic_int attached;                /* root only: an Android window is attached (renderer
                                          * alive). Commit-time drain is skipped while 0 so a
                                          * minimized window's client parks on buffer starvation
                                          * instead of spinning. Adapter writes (awl_window_attached). */
    /* W1 (bool): written ONLY by the client's own dispatch thread (its
     * request handlers/commit), with or without ev_lock — one writer thread
     * ⇒ serialized. */
    bool geom_valid : 1;
    bool pend_geom : 1;
    bool sub_pend_above : 1;            /* pending link belongs to parent's above stack */
    bool sub_stack_pending : 1;         /* this parent has a pending z order */
    bool sub_sync : 1;                  /* 1=sync mode (protocol default) */
    bool sub_latched : 1;               /* latched (sync) pending state exists */
    bool latched_attach : 1;            /* latched cycle contains an attach (without one,
                                          * applying leaves current alone) */
    bool sub_pos_pending : 1;
    bool pending_attached : 1;          /* this commit cycle attached */
    bool pending_damage_empty : 1;      /* damage accumulator empty */
    bool pend_vpd : 1;
    bool vp_has_src : 1;
    bool pend_vps : 1;
    bool pend_input : 1;                /* set_input_region this cycle (pend_input_region valid, NULL = reset to default) */
    bool pend_opaque : 1;               /* set_opaque_region this cycle (same shape) */
    bool acked : 1;                     /* client has acked (set after the first configure) */
    bool mapped : 1;                    /* first frame buffer committed */
    bool window_live : 1;               /* window_created went out for this id and
                                          * window_destroyed is still owed. Decoupled
                                          * from `role`: a client closes a window by
                                          * destroying the xdg_toplevel (role → NONE)
                                          * before the wl_surface — a role test at
                                          * surface death never fires and the window
                                          * table keeps a zombie (2026-09-17). */
};

/* Buffer / release-object liveness lock (awl_surface.c): guards
 * awl_buffer.resource, awl_bufref.release_res, esync_gc — the words a
 * client-side destroy (dispatch thread) races against a release being sent
 * from a render thread. Leaf lock: taken with nothing else held
 * except a bufferqueue head lock (drain callback) or rwl/ev_lock (destroy
 * handlers); never take rwl/ev_lock inside it. */
extern pthread_mutex_t g_bufref_lock;
struct awl_buffer* awl_buffer_ref(struct awl_buffer* b);
void awl_buffer_unref(struct awl_buffer* b);

/* Queue element cookie (awl_bq_buffer.user): what the release callback needs
 * to hand the frame back to whoever produced it. */
struct awl_bufref {
    struct awl_buffer* b;                /* ref'd dmabuf wrapper */
    struct wl_resource* release_res;     /* zwp_linux_buffer_release_v1 of the commit (g_bufref_lock; NULL = none/destroyed) */
};
/* Discard an uncommitted/unpresented explicit-sync pair: close the fence,
 * immediate_release the object (no lock held by the caller). */
void awl_surface_discard_sync(int acquire_fd, struct wl_resource* release_res);

/* Commit-path glue (awl_surface.c). apply_buffer: the double-buffered buffer
 * state of `s` just became `res` (NULL = detach); acquire_fd (owned, moved)
 * and release_res (may be NULL) are the explicit-sync state of that cycle.
 * Pushes the frame (dmabuf) / records the shm source (shm) / pushes a NULL
 * marker (detach). Caller holds s->ev_lock. shm_damaged: a commit without
 * attach carried damage for a live shm source (caller holds ev_lock).
 * commit_drain: the opportunistic trylock+drain (throttle rule inside).
 * schedule_render: root window dirty. */
void awl_surface_apply_buffer(struct awl_surface* s, struct wl_resource* res,
                              int acquire_fd, struct wl_resource* release_res);
void awl_surface_shm_damaged_locked(struct awl_surface* s);
void awl_surface_commit_drain(struct awl_surface* s);
void awl_surface_schedule_render(struct awl_surface* s);
/* The explicit-sync release object of the shm source died before delivery
 * (awl_esync.c destroy handler, dispatch thread, no lock held). */
void awl_surface_shm_release_gone(struct awl_surface* s, struct wl_resource* release_res);

/* awl_esync.c — zwp_linux_explicit_synchronization_v1 v2 */
void awl_esync_setup(void);
/* Deliver the release event for a commit's release object (fence_fd ≥ 0 →
 * fenced_release, else immediate_release) and park the resource for
 * destruction on its dispatch thread. Caller holds g_bufref_lock; `res` must
 * be live (the caller read it from a guarded word). The caller flushes. */
void awl_esync_release_locked(struct wl_resource* res, int fence_fd);
/* A queued frame now carries this release object (its cookie is `ref`):
 * the object's destroy handler clears ref->release_res. Dispatch thread. */
void awl_esync_bind_ref(struct wl_resource* res, struct awl_bufref* ref);
void awl_esync_gc(struct awl_surface* s);           /* dispatch thread: destroy parked release objects */
void awl_esync_surface_gone(struct awl_surface* s); /* dispatch thread, rwl.wr held, queue already flushed, before free */
/* Commit-time protocol validation (dispatch thread, before latching/apply):
 * 1 = a protocol error was posted, the commit must be abandoned. */
int awl_esync_commit_check(struct awl_surface* s);

/* ---- wl_data_device_manager (awl_data_device.c; KWin semantics) ----
 * Topology (three lists) is owned by g_srv.rwl: create/destroy and offer
 * creation = wr, iteration = rd; g_selection / g_drag / g_dd_focus_client /
 * source mimes are owned by g_srv.dd_lock.
 * Order: rwl → dd_lock (inner) → ev_lock. */
struct awl_mime {
    char name[64];
    struct wl_list link;            /* awl_data_source::mimes */
};

struct awl_data_source {
    struct wl_resource* res;        /* wl_data_source; NULL = internal source (clipboard bridge) */
    struct wl_client* client;
    void (*fill_fd)(struct awl_data_source*, const char*, int);   /* internal-source data
                                       callback (caller already holds rwl+dd — must not
                                       take locks inside; fd self-managed incl. close) */
    struct wl_list mimes;
    uint32_t dnd_actions;
    int is_dnd_actions;             /* set_actions was called (set_selection refused) */
    int accepted;                   /* target has accepted some mime */
    int drop_performed;             /* drop happened (precondition for finish) */
    struct wl_list link;            /* server.data_sources */
};

struct awl_data_offer {
    struct wl_resource* res;
    struct awl_data_source* src;    /* weak reference: cleared to NULL when the source dies / on finish */
    uint32_t supported_actions;     /* target-side action set (set_actions) */
    uint32_t preferred_action;
    int has_actions;
    int dnd;                        /* 1 = DnD offer; 0 = selection */
    struct wl_list link;            /* server.data_offers */
};

struct awl_data_device {
    struct wl_resource* res;        /* wl_data_device */
    struct wl_client* client;
    struct wl_list link;            /* server.data_devices */
};

/* DnD action negotiation modifiers (translated from Android meta in awl_input.c) */
#define AWL_DMOD_CTRL  1u
#define AWL_DMOD_SHIFT 2u

/* Server singleton (defined in awl_server.c)
 *
 * Threading model: main event thread (accept + not-yet-migrated clients)
 * + one dedicated per-client sub event thread (from map until disconnect)
 * + render thread/window + binder pool threads (input/window commands sent
 * directly).
 *
 * Lock hierarchy (libwayland carries the awl patch: connection mutex + atomic
 * serial — cross-thread direct send is safe):
 *   g_srv.rwl (rwlock)  guards only list topology (surfaces / input object
 *                       tables / clients migration table). Writers =
 *                       the protocol dispatch threads (create/destroy, wrlock).
 *                       Readers = the sending threads (binder/render), rdlock
 *                       held across the whole resolve→send — acquiring wrlock
 *                       proves no send is in flight.
 *   s->ev_lock (recursive) one per window: that surface's fields
 *                       (buffer/conf/frame_cb) + atomicity and ordering of
 *                       message groups sent to that client.
 *   g_input_lock        keyboard-derived state only (key bitmap / modifier
 *                       bits — consistency of kbd.enter's keys/modifiers
 *                       arrays). Input event order is guaranteed by binder
 *                       oneway serial delivery on the same node; no routing/focus state.
 * Fixed order: g_input_lock → rwl(rd) → ev_lock; never nested the other way. */
struct awl_server {
    atomic_int running;   /* main thread sleep loop ↔ stop thread writes, atomic */
    pthread_t thread;
    struct wl_display* display;
    struct wl_event_loop* loop;
    pthread_rwlock_t rwl;            /* list-topology rwlock (see above) */

    struct wl_list data_devices;   /* struct awl_data_device::link */
    struct wl_list data_sources;   /* struct awl_data_source::link */
    struct wl_list data_offers;    /* struct awl_data_offer::link */
    pthread_mutex_t dd_lock;       /* selection/drag state (inner, below rwl) */

    awl_display_info_t info;
    awl_window_callbacks_t cbs;

    /* Zoom (#31): zoom_pct = 100 × Z (integer percent, any ratio 50..300).
     * wl_output.scale is always 1 — clients receive preferred_scale =
     * zoom_pct×120/100 via wp_fractional_scale_v1 (kwin round(z×120)). */
    atomic_int zoom_pct;   /* binder thread set_zoom ↔ protocol dispatch threads read, atomic */

    /* View mapping mode (#34, daemon config scale_mode — AWL_SCALE_* in awl.h):
     * how the content-base rectangle maps into the Android window. Pure
     * presentation-layer state: render dst / input / confine / IME-rect all
     * convert through awl_view_map(); no configure size changes. Binder config
     * thread writes, dispatch/render threads read — atomic, no lock. */
    atomic_int scale_mode;

    /* Initial-configure placeholder size (#33, daemon config init_w/init_h):
     * sent before the Android window exists (get_toplevel initial configure +
     * set_maximized/fullscreen placeholders). Set from the binder config
     * thread, read on client dispatch threads — atomics, no lock. Applies to
     * NEW windows only; mapped windows are resized by awl_window_resize. */
    atomic_int init_conf_w, init_conf_h;
    struct wl_list frac_scales;      /* struct awl_frac_scale::link (awl_viewport.c) */

    struct wl_list surfaces;   /* struct awl_surface::link */
    struct wl_list clients;    /* struct awl_client_ctx::link (awl_server.c) */
    uint64_t next_surface_id;
};

extern struct awl_server g_srv;

/* awl_surface.c — wl_compositor/wl_surface/wl_region/wl_shm buffer lifecycle */
void awl_surface_setup(void);
struct awl_surface* awl_surface_by_id(uint64_t id);
struct awl_surface* awl_surface_from_res(struct wl_resource* res);
/* Replace the toplevel title (heap; NULL/"" = none). Client dispatch thread
 * only; the adapter copies synchronously inside window_created/
 * window_title, so the pointer never outlives the callback. */
void awl_surface_set_title(struct awl_surface* s, const char* title);
/* wl_region contents: the ordered add/subtract rectangle list the client
 * built on the object (awl_surface.c). Consumers never hold the wl_region
 * resource — they either read a bounding box at request time (pointer
 * constraints) or take a snapshot (surface regions, applied on commit). */
struct awl_region;
/* Bounding box of the ADDED rectangles (subtract ignored — holes do not
 * grow a confine box; returns 1 = at least one rectangle was added; 0 =
 * empty/NULL region — callers treat it as "unconstrained/whole") */
int awl_region_bbox(struct wl_resource* region, int32_t* x, int32_t* y,
                    int32_t* w, int32_t* h);
/* Deep copy of a wl_region's rectangle list (NULL region → NULL; a region
 * with no rectangles → a valid, empty snapshot — the two are distinct: see
 * awl_surface.input_region). Returns NULL on allocation failure too, so a
 * caller with a non-NULL region must treat NULL as no_memory. */
struct awl_region* awl_region_snapshot(struct wl_resource* region);
void awl_region_free(struct awl_region* r);
/* Point test with full add/subtract semantics: the LAST rectangle covering
 * (x,y) decides. NULL region = empty. */
int awl_region_contains(const struct awl_region* r, int32_t x, int32_t y);
/* Input hit filter (#85): does surface-local logical point (x,y) fall inside
 * s's committed input region? No region set = the whole surface. Takes
 * s->ev_lock; caller holds rwl (rd). */
int awl_surface_accepts_input(struct awl_surface* s, float x, float y);
/* Pending → current promotion of both region slots (caller holds s->ev_lock;
 * surface_commit direct path + sync-subsurface apply in awl_subsurface.c). */
void awl_surface_apply_regions_locked(struct awl_surface* s);
/* Pending → current damage merge at every commit / sync-subsurface latch
 * apply (caller holds s->ev_lock; awl_surface.c + awl_subsurface.c).
 * has_attach = this commit presented a new buffer — attach without damage
 * then means FULL; an empty commit (neither) is a no-op. */
void awl_damage_merge_pending(struct awl_surface* s, int has_attach);

/* Shared by dmabuf/shm: on buffer destroy, unlink it from pending/current/
 * latched of every surface (rwl.rd + ev_lock inside; dispatch thread) */
void awl_surface_buffer_gone(struct wl_resource* buffer_res);

/* awl_input.c — wl_seat input (per-event literal translation: Android is the
 * routing authority, events carry the window id; event types in awl.h) */
void awl_input_setup(void);          /* creates the seat global (called by server_start) */
/* Cursor hooks (set_cursor state lives in awl_input.c, own g_cursor_lock;
 * order rwl → g_cursor_lock → ev_lock):
 *  - surface_gone: caller holds rwl.wr (awl_surface.c destroy path). Drops
 *    pointer focus / cursor references to s. Returns the window whose cursor
 *    display changed (0 = none) — the caller must pass it to
 *    awl_input_cursor_gone_notify AFTER releasing rwl (redraw + restore the
 *    Android pointer; callbacks must not run under the topology write lock).
 *  - cursor_window: window currently compositing s as its cursor (0 = s is
 *    not the displayed cursor) — schedule_render of a CURSOR-role surface
 *    dirties that window instead of "its own" (it has none).
 *  - cursor_commit: a cursor surface committed with an attach offset —
 *    hotspot -= offset (kwin SurfaceCursorSource::refresh). */
uint64_t awl_input_surface_gone(struct awl_surface* s);
void awl_input_cursor_gone_notify(uint64_t win);
uint64_t awl_input_cursor_window(struct awl_surface* s);   /* caller holds rwl (rd/wr) */
void awl_input_cursor_commit(struct awl_surface* s, int32_t off_x, int32_t off_y);

/* Pointer-constraint hooks (zwp_pointer_constraints_v1 state lives in
 * awl_input.c under g_constr_lock — pure state sync, the input translation
 * path never reads it; activation/clamping are APK-side):
 *  - constr_surface_gone: caller holds rwl.wr (awl_surface.c destroy path).
 *    Constraints on this surface / its root die: unlocked/unconfined is
 *    sent to the still-live client, the objects stay until the client
 *    destroys them. Returns the root window whose capture state changed
 *    (0 = none) — pass it to awl_input_constr_gone_notify AFTER releasing
 *    rwl (the C_CAPTURE callback must not run under the topology write
 *    lock). */
uint64_t awl_input_constr_surface_gone(struct awl_surface* s);
void awl_input_constr_gone_notify(uint64_t win);
/* Re-convert + re-push the confine rects (view px) of this root's live
 * constraints — the view mapping changed underneath them (scale_mode switch
 * or a window resize that moved the letterbox offset / stretch ratio; without
 * this the APK clamp box sits over the black bars). root_id 0 = every root.
 * Takes rwl.rd itself: call with no logic-layer lock held; callbacks fire
 * after release (awl_xdg.c awl_window_resize tail / awl_viewport.c
 * set_scale_mode + set_zoom). */
void awl_input_constr_remap(uint64_t root_id);

/* awl_idle.c — zwp_idle_inhibit_manager_v1 (inhibitor state under
 * g_inhib_lock — pure state sync like the constraints above; the Activity
 * sets FLAG_KEEP_SCREEN_ON per C_KEEPON, window visibility governs whether
 * it is honored):
 *  - idle_surface_gone: caller holds rwl.wr (awl_surface.c destroy path).
 *    Inhibitors on this surface / its root die (the objects stay until the
 *    client destroys them). Returns the root window whose aggregate flipped
 *    to zero live inhibitors (0 = none) — pass it to awl_idle_gone_notify
 *    AFTER releasing rwl (the C_KEEPON callback must not run under the
 *    topology write lock). */
void awl_idle_setup(void);
uint64_t awl_idle_surface_gone(struct awl_surface* s);
void awl_idle_gone_notify(uint64_t win);

/* awl_icon.c — xdg_toplevel_icon_v1 (per-window Recents icons; pixels are
 * copied at add_buffer, so the applied icon survives icon-object and buffer
 * destruction like the spec's lifetime rules; entries under g_icon_lock,
 * keyed by the toplevel's surface id):
 *  - commit: called from surface_commit (no logic lock held; that surface's
 *    ev_lock may be held — the C_ICON callback fires from awl_icon_commit
 *    itself, outside ev_lock).
 *  - surface_gone: caller holds rwl.wr (awl_surface.c destroy path); drops
 *    the window's pending + applied icon with the surface. */
void awl_icon_setup(void);
void awl_icon_commit(struct awl_surface* s);
void awl_icon_surface_gone(struct awl_surface* s);

/* awl_data_device.c — wl_data_device_manager v3 (full selection + DnD state
 * machine, semantics aligned with kwin-6.6.5; see the file-header lock note).
 * Input hooks must not be entered holding rwl (they take it internally; motion
 * needs the caller to resolve the layer hit under rwl.rd first, then drop it). */
void awl_datadev_setup(void);
int  awl_datadev_drag_active(void);
uint64_t awl_datadev_drag_icon_id(void);   /* excluded from hit-testing (0=no icon) */
void awl_datadev_drag_motion(uint64_t win, uint64_t hit_id,
                             float bx, float by, float lx, float ly);
void awl_datadev_drag_end(void);
void awl_datadev_drag_cancel(void);
void awl_datadev_key_mods(uint32_t mods);            /* AWL_DMOD_* */
void awl_datadev_focus_enter(struct wl_client* c);   /* keyboard focus → selection receiver */
void awl_datadev_focus_leave(void);
void awl_datadev_surface_gone(struct awl_surface* s);   /* caller holds rwl.wr */
/* Internal source (Android clipboard bridge only; res=NULL) */
struct awl_data_source* awl_datadev_internal_source(
        void (*fill_fd)(struct awl_data_source*, const char*, int));
void awl_datadev_internal_mime(struct awl_data_source* src, const char* mime);
void awl_datadev_internal_gone(struct awl_data_source* src);

/* awl_ime.c — zwp_text_input_v1+v3 (Android IME bridge; pass-through model in awl.h)
 * kbd focus hooks: tr_kbd_enter/leave are called with rwl(rd) held — v3 enter/
 * leave follow keyboard focus (independent of enable, kwin semantics); v1
 * activate is its own enter (a make-up enter follows a late focus). */
void awl_ime_setup(void);                /* creates the manager globals (called by server_start) */
void awl_ime_focus_enter(uint64_t win);   /* caller holds rwl(rd) */
void awl_ime_focus_leave(uint64_t win);   /* caller holds rwl(rd) */
void awl_ime_surface_gone(struct awl_surface* s);   /* caller holds rwl.wr (drops dangling associations) */
void awl_ime_set_focus(struct wl_client* c, uint64_t win);   /* tr_kbd_* writes the focus snapshot */

/* awl_xdg.c — xdg_wm_base/xdg_surface/xdg_toplevel/xdg_popup
 * The three Android → client window commands are sent directly from any
 * thread (rdlock+ev_lock, no marshalling) */
void awl_xdg_setup(void);
void awl_xdg_flush_pending(uint64_t id);   /* after map, re-sends the cached resize (dispatch thread) */
/* popup grab check (press event; caller holds rwl.rd). Returns 1 = the press
 * was consumed by the popup grab (popup_done already sent, event must not be delivered). */
int awl_popup_input_grab(struct awl_surface* hit);

/* awl_subsurface.c — wl_subcompositor / wl_subsurface (child layers attached to the parent window for compositing) */
void awl_subsurface_setup(void);
struct awl_surface* awl_subsurface_root(struct awl_surface* s);   /* caller holds rwl */
/* Called from surface_commit: a commit of an effectively-sync child layer is
 * latched (not applied, not presented); returns 1 = the caller should return
 * directly (this commit is fully handled) */
int awl_subsurface_maybe_latch(struct awl_surface* s);
/* Called after state application completes (any commit, including empty ones
 * — the moment sync children take effect): child double-buffered positions
 * apply + latched sync state cascades; returns 1 = some child buffer applied */
int awl_subsurface_parent_applied(struct awl_surface* s);
/* Caller holds g_srv.rwl for write. Popup and drag layers intentionally enter
 * the current top stack immediately; wl_subsurface place requests instead
 * modify the parent's pending stack. */
void awl_subsurface_link_immediate_above_locked(struct awl_surface* child,
                                                struct awl_surface* parent);
void awl_subsurface_unlink_locked(struct awl_surface* child);
/* Input hit test (caller holds rwl.rd): root buffer coords → first layer
 * containing the point AND accepting input there (wl_surface.set_input_region,
 * #85), top-down in render stack order, coords translated to layer-local;
 * prefer>0 = touch/pointer grab forces that layer (translation only, the
 * region is not re-tested — protocol focus is pinned after down/press);
 * exclude>0 = skip that layer (the drag icon never hit-tests, its events
 * belong to the drag machine). Never NULL — no hit falls back to the root
 * (Android already routed the event to that window). */
struct awl_surface* awl_subsurface_hit(struct awl_surface* root, float bx, float by,
                                       uint64_t prefer, uint64_t exclude,
                                       float* lx, float* ly);

/* awl_dmabuf.c */
void awl_dmabuf_setup(void);

/* awl_xwayland.c — xwayland_shell_v1 (Xwayland rootless window association,
 * #32; role = toplevel, no configure state machine, first buffer commit maps) */
void awl_xwayland_setup(void);
/* Xwayland window association serial (written when non-0 and the role is
 * XWAYLAND; 0=no such window / not associated) — the adapt layer uses it to
 * operate the matching X window (resize/close) via the mini-wm control channel */
int awl_xwayland_window_serial(uint64_t id, uint64_t* serial);

/* awl_viewport.c — wp_viewporter + zwp_fractional_scale_manager_v1 (#31
 * arbitrary-ratio zoom, isomorphic to kwin-6.6.5 fractionalscale_v1/viewporter) */
void awl_viewport_setup(void);
/* Surface logical size (caller holds that surface's ev_lock; 0=undetermined,
 * no buffer). viewport dst | source | buffer/buf_scale — shared by the render
 * dst and input hit-testing. */
void awl_surface_logical_size(struct awl_surface* s, float* w, float* h);
/* Content base size of a root (logical px; caller holds its ev_lock): the
 * xdg geometry rectangle when valid (chrome-like clients' viewport dst
 * carries shadow margins around it), else the surface logical size. Shared
 * by the view mapping, render dst and input inverse. */
void awl_surface_content_size(struct awl_surface* s, float* w, float* h);
/* Root → window view mapping, view = (logical − geometry origin) × s + o —
 * THE conversion shared by render dst / input inverse / relative deltas /
 * confine rects / IME cursor rect. Content following the
 * configured size → exactly s = Z, o = 0 (kwin: scene at the output scale;
 * the client's logical×Z buffer lands 1:1, nothing resampled, whatever
 * scale_mode says). Otherwise → scale_mode placement (awl_view_map). Caller
 * holds root ev_lock. */
void awl_surface_view_map(struct awl_surface* root,
                          double* sx, double* sy, double* ox, double* oy);
/* zoom: preferred_scale (1/120 units, kwin round(z×120)) and the effective
 * scale Z = preferred_scale/120 the client renders at — the only Z the
 * compositor side may use (configure size, 1:1 mapping). Any thread. */
uint32_t awl_zoom_preferred_scale(void);
double awl_zoom_scale(void);
/* Sample-region uv transform of the current buffer (viewport source →
 * normalized; whole buffer when unset/no buffer). Caller holds ev_lock. */
void awl_surface_layer_uv(struct awl_surface* s, float* u0, float* v0,
                          float* su, float* sv);

/* awl_server.c — dedicated per-client event thread
 * Called at map (first buffer commit, on the dispatch thread that owns the
 * client at that moment): the first call migrates that client's fd source
 * into a new loop and starts the sub-thread — from then on all of that
 * client's requests are dispatched by the sub-thread, and disconnect destroys
 * it there too. Thread contract: wl_client_set_event_loop in wayland-server.c. */
void awl_client_maybe_migrate(struct wl_client* client);

#endif
