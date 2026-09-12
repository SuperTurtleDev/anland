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
#include "awl_log.h"   /* AWL_TAG "anland-wl" + LOGI/LOGE/LOGD (see awl_log.h) */

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
 * libwayland and not wrapped) */
struct awl_buffer {
    struct wl_resource* resource;    /* wl_buffer (created by us) */
    struct wl_list link;             /* server.buffers */
    int dmabuf_fd;                   /* owned after dup */
    uint64_t ino;                    /* dma-buf inode at creation — render-side
                                      * identity without a per-frame fstat
                                      * (0 = fstat failed here, consumers
                                      * fall back to their own) */
    uint32_t width, height, stride;  /* stride: bytes */
    uint32_t drm_format;
    uint64_t modifier;
};

struct awl_frame_cb {
    struct wl_resource* resource;
    struct awl_surface* s;           /* owner for backfill (destroy listener takes lock via it) */
    int detached;                    /* already unlinked from the list (presented path: unlink first, destroy later) */
    struct wl_list link;
};

struct awl_surface {
    uint64_t id;                     /* window id (globally unique; same id on the Java side) */
    struct wl_resource* resource;
    struct wl_list link;             /* server.surfaces */
    pthread_mutex_t ev_lock;         /* per-window event send lock (recursive): fields + send order for that client */

    enum awl_role role;
    struct wl_resource* xdg_surface_res;    /* associated xdg_surface */
    struct wl_resource* xdg_role_res;       /* xdg_toplevel / xdg_popup */
    struct wl_resource* xwayland_res;       /* xwayland_surface_v1 (#32) */
    uint64_t xwayland_serial;               /* association serial from set_serial (= the X-side
                                              * WL_SURFACE_SERIAL ClientMessage;
                                              * mini-wm pairs the X window by this; 0=not associated) */

    /* xdg state machine */
    bool configured;                 /* a configure has been sent */
    bool acked;                      /* client has acked (set after the first configure) */
    uint32_t configure_serial;       /* serial of the most recent configure */
    bool mapped;                     /* first frame buffer committed */
    int32_t conf_w, conf_h;          /* most recent configure contents */
    int32_t pend_w, pend_h;          /* cached when resize precedes map (Android owns sizing entirely) */
    bool has_pending;
    /* xdg window geometry (buffer coords, double-buffered, applied on commit)
     * = the window's visible content region (the chrome buffer carries
     * 16/10px shadow margins; geometry states where the content sits). The
     * render dst and the input view→buffer mapping share this origin —
     * ignoring it misplaces content to bottom-right and systematically
     * offsets input coords (2026-09-09 restore-bubble unclickable, verified). */
    int32_t geom_x, geom_y, geom_w, geom_h;
    int geom_valid;
    int32_t pend_gx, pend_gy, pend_gw, pend_gh;
    int pend_geom;
    bool activated;                  /* xdg ACTIVATED state (Android foreground focus) */
    int32_t popup_x, popup_y;        /* popup anchoring result */
    char title[256];

    /* ---- zoom (#31: wp_viewporter + wp_fractional_scale_v1, kwin-isomorphic) ----
     * Coordinate model: all layer-stack/geometry/popup/input coords = logical
     * pixels (surface-local). Layer logical size = viewport dst | source |
     * buffer/buf_scale (isomorphic to kwin SurfaceInterfacePrivate::applyState
     * surfaceSize); toplevel logical size = phys/zoom (the configure-issued
     * value). viewport state is double-buffered, applied on the same commit
     * as the buffer (kwin pending→current). */
    int32_t phys_w, phys_h;          /* Android window size (recorded by awl_window_resize; 0=unknown) */
    int32_t buf_scale;               /* wl_surface.set_buffer_scale (default 1; bookkeeping only) */
    int32_t buf_transform;           /* wl_surface.set_buffer_transform, current (wl_output.transform
                                      * 0..7; 90/270 swap the logical size). Applied on commit like
                                      * viewport state — the render side reads it per frame. */
    int32_t pend_buf_transform;      /* -1 = nothing pending */
    struct wl_resource* viewport_res;/* wp_viewport (at most 1 per surface; NULL=none) */
    struct wl_resource* frac_res;    /* zwp_fractional_scale_v1 (at most 1 per surface) */
    int32_t vp_dst_w, vp_dst_h;      /* viewport dst logical size (0=unset) */
    int32_t pend_vpd_w, pend_vpd_h;
    int vp_has_dst, pend_vpd;
    float vp_sx, vp_sy, vp_sw, vp_sh;      /* source rectangle (buffer×buf_scale coords) */
    float pend_vps_x, pend_vps_y, pend_vps_w, pend_vps_h;
    int vp_has_src, pend_vps;

    /* subsurface role (chrome WaylandBubble=tooltip/selection handles, GTK4 popover):
     * Topology (sub_parent / sub_children stack order) is owned by g_srv.rwl;
     * sub_x/sub_y are owned by this surface's ev_lock (render snapshot reads,
     * set_position writes).
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
    struct wl_resource* subsurface_res;   /* wl_subsurface object (drop the reference if the surface dies first) */
    struct awl_surface* sub_parent;
    struct wl_list sub_children;          /* child stack order: head=bottom, tail=top */
    struct wl_list sub_link;              /* linked into sub_parent->sub_children */
    int sub_sync;                         /* 1=sync mode (protocol default) */
    int sub_latched;                      /* latched (sync) pending state exists */
    struct wl_resource* latched_buffer_res;   /* latched buffer (never sampled) */
    int latched_attach;                   /* latched cycle contains an attach (without one, applying leaves current alone) */
    int32_t sub_x, sub_y;                 /* applied position (buffer pixels, Y down) */
    int32_t pend_sub_x, pend_sub_y;       /* set_position double-buffered value (applied on parent commit) */
    int sub_pos_pending;

    /* Double-buffered state. Protocol semantics (2026-09-09 black-screen
     * deadlock, verified): pending state persists across commits — a commit
     * without attach does not change the buffer; only an explicit
     * attach(NULL)+commit detaches. pending_attached marks whether this
     * cycle attached (empty commit / ack commit must not clear current). */
    struct wl_resource* pending_buffer_res;
    struct wl_resource* current_buffer_res;
    int pending_attached;
    int32_t pending_offset_x, pending_offset_y;

    /* damage (wl_surface.damage/damage_buffer accumulated in pending —
     * surface-local px, bbox merge; moved to cur on the commit/latch-apply
     * that presents a buffer). cur_* = damage since the renderer last
     * consumed it (awl_surface_get_damage / _damage_consumed):
     *   NONE    nothing changed since the last upload (cursor/layer move
     *           re-render → the renderer skips the upload entirely)
     *   RECT    the bbox rect needs re-uploading
     *   FULL    a commit attached a buffer with NO damage — protocol
     *           default: whole surface (client gave no information)
     * cd_gen increments on every change: the renderer consumes only when
     * token+gen still match (a commit racing the upload keeps its damage
     * for the next frame — over-upload is always safe, under-upload never).
     * All owned by this surface's ev_lock. */
    int32_t pd_x, pd_y, pd_w, pd_h;
    int pending_damage_empty;
    int32_t cur_damage_x, cur_damage_y, cur_damage_w, cur_damage_h;
    int cd_state;               /* AWL_DMG_* (awl.h; NONE = 0, calloc-init) */
    uint32_t cd_gen;

    struct wl_list frame_callbacks;
    bool dirty;                      /* awaiting render after commit */

    /* Deferred release queue (KWin GraphicsBuffer reference semantics:
     * wl_buffer.release is sent only after the frame that sampled the buffer
     * is presented) — keeps clients from overwriting a dmabuf mid-sampling.
     * Only dmabuf queues (shm upload copies at once, no concurrent sampling);
     * presented drains it; queue full (rendering stalled) degrades to freeing
     * the head immediately. wl_buffer destruction removes entries from it. */
    struct wl_resource* release_q[4];
    int release_q_n;
};

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
    uint32_t selected_action;
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
 *                       tables / buffers / clients migration table). Writers =
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

    struct wl_shm* shm;
    struct wl_global* g_compositor;
    struct wl_global* g_seat;
    struct wl_global* g_output;
    struct wl_global* g_dmabuf;
    struct wl_global* g_xdg_wm_base;
    struct wl_global* g_subcompositor;
    struct wl_global* g_data_device_manager;

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
    struct wl_global* g_viewporter;
    struct wl_global* g_frac_scale_mgr;
    struct wl_list frac_scales;      /* struct awl_frac_scale::link (awl_viewport.c) */

    struct wl_list surfaces;   /* struct awl_surface::link */
    struct wl_list buffers;    /* struct awl_buffer::link (dmabuf) */
    struct wl_list clients;    /* struct awl_client_ctx::link (awl_server.c) */
    uint64_t next_surface_id;
};

extern struct awl_server g_srv;

/* awl_surface.c — wl_compositor/wl_surface/wl_region/wl_shm buffer lifecycle */
void awl_surface_setup(void);
struct awl_surface* awl_surface_by_id(uint64_t id);
struct awl_surface* awl_surface_from_res(struct wl_resource* res);
/* wl_region bounding box snapshot (returns 1 = at least one rectangle was
 * added; 0 = empty region — callers treat it as "unconstrained/whole") */
int awl_region_bbox(struct wl_resource* region, int32_t* x, int32_t* y,
                    int32_t* w, int32_t* h);
/* Pending → current damage merge at every commit / sync-subsurface latch
 * apply (caller holds s->ev_lock; awl_surface.c + awl_subsurface.c).
 * has_attach = this commit presented a new buffer — attach without damage
 * then means FULL; an empty commit (neither) is a no-op. */
void awl_damage_merge_pending(struct awl_surface* s, int has_attach);

/* Shared by dmabuf/shm: on buffer destroy, unlink it from current/pending */
void awl_surface_detach_buffer(struct wl_resource* buffer_res);
/* Enqueue a deferred release (caller holds that surface's ev_lock; dmabuf only — shm releases immediately) */
void awl_surface_release_defer(struct awl_surface* s, struct wl_resource* buf);

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
/* Input hit test (caller holds rwl.rd): root buffer coords → first layer
 * containing the point, top-down in render stack order, coords translated to
 * layer-local; prefer>0 = touch/pointer grab forces that layer (translation
 * only); exclude>0 = skip that layer (the drag icon never hit-tests, its
 * events belong to the drag machine). Never NULL — out-of-bounds falls back
 * to the root (Android already routed the event to that window). */
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
