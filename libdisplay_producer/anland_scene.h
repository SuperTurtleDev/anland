/*
 * anland_scene.h — layer/commit interface for anland_device.
 *
 * The legacy device API (anland_device.h) models ONE full-screen output fed by a
 * set of dmabufs, and reports a frame as done the moment it is handed over. That
 * output model is the one the product uses (one container hosts one DE on one
 * Android Surface), but conflating submit, presentation and buffer release makes
 * it unable to report real presentation.
 *
 * This header adds the missing half: a layer/commit model that is independent of
 * any compositor. It deliberately contains NO Qt / GObject / Rust / Android types,
 * so every DE translates into the same contract.
 *
 * Layering:
 *
 *   DE adapter (KWin/Mutter/...)    window lifecycle + rendering
 *        │  anland_scene_*           ← this file: stable C ABI
 *   anland_scene                    state machine (atomic commit, in-flight
 *        │                          tracking, release/complete events)
 *   anland_scene_backend_ops        device presentation (legacy / future DRM)
 *
 * Three things are kept strictly separate, because conflating them is what made
 * the legacy path unable to report real presentation:
 *
 *   1. SUBMITTED  — anland_scene_commit_submit() accepted the transaction
 *   2. PRESENTED  — the backend reports the frame was acknowledged by the
 *      consumer (legacy: buffer rotation; not proof of physical scanout)
 *   3. RELEASED   — a buffer may be reused by the producer again
 *
 * A successful submit says nothing about (2) or (3).
 *
 * fd ownership (applies to every fd in this API):
 *   - fds passed IN  (acquire_fence_fd) are BORROWED. The caller keeps ownership
 *     and must keep them open for the duration of the call. The backend must
 *     dup() if it needs the fd after submit() returns.
 *   - fds passed OUT (release_fence_fd in events) are OWNED by the receiver.
 */
#ifndef ANLAND_SCENE_H
#define ANLAND_SCENE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ANLAND_SCENE_MAX_LAYERS 32
#define ANLAND_SCENE_MAX_DAMAGE 8
#define ANLAND_SCENE_MAX_NAME 64
#define ANLAND_SCENE_EVENT_QUEUE 64
/* Hard bound for owned release notifications; callers must drain before retry. */
#define ANLAND_SCENE_RELEASE_QUEUE_MAX 1024

/* Layer ids are opaque, stable for the lifetime of the layer, and never reused
 * within a scene. 0 is reserved to mean "none". */
typedef uint64_t anland_layer_id;

/* ---- geometry ---- */

typedef struct anland_rect {
    int32_t x;
    int32_t y;
    uint32_t width;
    uint32_t height;
} anland_rect_t;

/* ---- layer identity ---- */

typedef enum anland_layer_kind {
    ANLAND_LAYER_NORMAL = 0,  /* a toplevel window */
    ANLAND_LAYER_CURSOR = 1,  /* pointer sprite: composited above NORMAL */
    ANLAND_LAYER_OVERLAY = 2, /* always-on-top chrome (panels, OSD) */
} anland_layer_kind_t;

typedef struct anland_layer_desc {
    anland_layer_id parent_id; /* 0 = no parent (toplevel) */
    anland_layer_kind_t kind;
    const char *name;          /* optional, may be NULL; for diagnostics only */
    anland_rect_t geometry;    /* output-space placement and size */
    uint32_t scale;            /* 0/1 means compositor default */
    int32_t transform;
    int32_t z_order;
    float opacity;              /* clamped to [0,1] by the caller */
    bool visible;
} anland_layer_desc_t;

/* ---- per-layer state inside one commit ---- */

typedef struct anland_layer_state {
    anland_layer_id layer_id;

    /* 0 means "no buffer": the layer is drawn blank (or skipped when !visible).
     * buffer_id is chosen by the DE and must be unique among live buffers. */
    uint64_t buffer_id;

    /* Crop in buffer space. width==0 means "whole buffer". */
    anland_rect_t source;

    /* Placement in output space. width/height must be non-zero when the layer is
     * visible and has a buffer. */
    anland_rect_t destination;

    int32_t z_order;  /* higher paints on top; ties broken by array order */
    float opacity;    /* clamped to [0,1] by the caller; 0 is legal (invisible) */
    bool visible;

    /* Optional acquire fence: the backend must not sample the buffer until this
     * signals. BORROWED, -1 = already ready. */
    int acquire_fence_fd;

    /* Damage in buffer space, BORROWED. NULL/0 means "whole source rect". */
    const anland_rect_t *damage;
    size_t damage_count;
} anland_layer_state_t;

/* ---- events (backend → DE) ---- */

typedef enum anland_scene_event_type {
    /* The consumer has ACKNOWLEDGED this commit (it selected the next buffer /
     * requested more work). On the legacy transport this is the strongest signal
     * available; it does NOT prove the buffer was physically scanned out, so it
     * is deliberately named "presented" only by historical convention. A future
     * backend with real display feedback can report a stronger timestamp.
     *
     * DELIBERATELY NOT equivalent to BUFFER_RELEASED: a device may still scan out
     * or asynchronously composite the buffer it just acknowledged. The backend
     * owns the lifetime decision and reports it separately through
     * anland_scene_backend_release_buffer(). */
    ANLAND_SCENE_EVENT_PRESENTED = 1,
    /* The buffer is no longer referenced by any in-flight commit and may be
     * reused by the producer. Reported ONLY when the backend explicitly says so
     * (anland_scene_backend_release_buffer) or when a commit was dropped, so
     * "may be reused" is a statement the backend actually made.
     *
     * The backend reports once per distinct buffer used by a completed frame.
     * Buffer ids alone do not encode session identity: the backend must discard
     * stale session work before reporting releases. */
    ANLAND_SCENE_EVENT_BUFFER_RELEASED = 2,
    /* Commit will never be presented (superseded / invalidated / backend error). */
    ANLAND_SCENE_EVENT_COMMIT_DROPPED = 3,
    /* Size/refresh changed (hotplug). */
    ANLAND_SCENE_EVENT_OUTPUT_CHANGED = 4,
    /* The consumer published the buffer the producer should render into next.
     * This is a RENDER TARGET signal, not a completion signal: it says "you may
     * draw into buffer_id now", and it is what starts (or resumes) a DE's frame
     * loop. It is deliberately separate from PRESENTED, because the handshake
     * publishes a selection before any frame has ever been flipped — treating
     * that first signal as a presentation would report a frame that never
     * existed. Emitted on connect and again after each acknowledgement. */
    ANLAND_SCENE_EVENT_RENDER_TARGET_READY = 5,
} anland_scene_event_type_t;

typedef enum anland_scene_drop_reason {
    ANLAND_SCENE_DROP_SUPERSEDED = 1, /* a newer commit replaced it */
    ANLAND_SCENE_DROP_INVALIDATED = 2, /* reconnect/teardown dropped it */
    ANLAND_SCENE_DROP_BACKEND_ERROR = 3,
} anland_scene_drop_reason_t;

typedef struct anland_scene_event {
    anland_scene_event_type_t type;
    uint64_t commit_id; /* 0 when the event is not tied to a commit */
    union {
        struct {
            uint64_t presentation_ns; /* backend clock, 0 if unavailable */
        } presented;
        struct {
            uint64_t buffer_id;
            /* OWNED by the receiver; -1 = no fence (buffer already reusable). */
            int release_fence_fd;
        } released;
        struct {
            anland_scene_drop_reason_t reason;
        } dropped;
        struct {
            uint32_t width;
            uint32_t height;
            uint32_t refresh_mhz;
        } output;
        /* Payload of ANLAND_SCENE_EVENT_RENDER_TARGET_READY. `index` is a slot in
         * the producer-side buffer set (0 <= index < count); the DE renders into
         * the framebuffer it imported for that slot. Backends that own buffer
         * selection publish the slot here instead of making each DE decode their
         * own index convention. `generation` identifies the consumer session: a
         * DE that sees an older generation than its current one must ignore the
         * event rather than switch to a buffer from a dead session. */
        struct {
            uint64_t generation;
            uint32_t index;
            uint32_t count;
        } target_ready;
    } u;
} anland_scene_event_t;

/* ---- presentation backend ---- */

/* A self-contained copy of one commit. The backend may keep this (and the
 * acquire fds are only valid during submit(), so it must dup them if needed). */
typedef struct anland_scene_layer_snapshot {
    anland_layer_id layer_id;
    uint64_t buffer_id;
    anland_rect_t source;
    anland_rect_t destination;
    int32_t z_order;
    float opacity;
    bool visible;
    int acquire_fence_fd;
    size_t damage_count;
    anland_rect_t damage[ANLAND_SCENE_MAX_DAMAGE];
} anland_scene_layer_snapshot_t;

typedef struct anland_scene_snapshot {
    uint64_t commit_id;
    /* Bumped by anland_scene_invalidate(). A backend that sees a different
     * generation than the one it is presenting must discard the work. */
    uint64_t generation;
    size_t count;
    anland_scene_layer_snapshot_t layers[ANLAND_SCENE_MAX_LAYERS];
} anland_scene_snapshot_t;

typedef struct anland_scene_backend_ops {
    /* Accept a validated transaction. Return 0 if the backend took ownership of
     * presenting it, non-zero to reject it (the scene then reports
     * COMMIT_DROPPED with BACKEND_ERROR and re-releases its buffers).
     * The snapshot is only valid for the duration of this call. */
    int (*submit)(void *userdata, const anland_scene_snapshot_t *snapshot);

    /* Optional: called by anland_scene_destroy() so the backend can release
     * resources. May be NULL. */
    void (*destroy)(void *userdata);
} anland_scene_backend_ops_t;

/* ---- lifecycle ---- */

/* ops->submit is required; ops->destroy may be NULL. Returns NULL on failure.
 * userdata is passed back to the ops verbatim. */
struct anland_scene;
typedef struct anland_scene anland_scene;

anland_scene *anland_scene_create(const anland_scene_backend_ops_t *ops,
                                  void *userdata);
void anland_scene_destroy(anland_scene *scene);

/* Reconnect / teardown: any in-flight commit is dropped (the DE receives
 * COMMIT_DROPPED; unhanded buffers are released immediately) and the generation is bumped
 * so a backend holding stale work discards it. Layers survive: their identity is
 * the DE's, not the transport's. */
int anland_scene_invalidate(anland_scene *scene);

uint64_t anland_scene_generation(const anland_scene *scene);

/* Is this commit still in flight? A backend must check this before it reports
 * work to a peer: a commit may have been completed already (window destroyed,
 * invalidate(), reconnect) without the backend noticing, and reporting success
 * for it would tell the peer about a frame that no longer exists.
 *
 * Serialization: this is a point-in-time query, not a reservation. It is only
 * conclusive while scene calls are serialized by the presentation adapter.
 * A concurrent invalidate() can still
 * race the check; supporting cross-thread cancellation needs a state
 * transition primitive, not a query. */
bool anland_scene_commit_is_pending(anland_scene *scene, uint64_t commit_id);

/* ---- layers ---- */

/* Create a layer. On success *out_id receives a non-zero id. */
int anland_scene_layer_create(anland_scene *scene,
                              const anland_layer_desc_t *desc,
                              anland_layer_id *out_id);

/* Destroy a layer. Referencing commits are dropped. Already handed-off buffers
 * remain owned by the backend until explicit retirement. */
int anland_scene_layer_destroy(anland_scene *scene, anland_layer_id id);
/* Update mutable layer metadata without changing layer identity. */
int anland_scene_layer_update(anland_scene *scene,
                              anland_layer_id id,
                              const anland_layer_desc_t *desc);
int anland_scene_layer_count(const anland_scene *scene);

/* ---- commit ---- */

/* Atomically submit state for `count` layers.
 *
 * ALL-OR-NOTHING: every layer id must exist, none may repeat, and every state is
 * validated up front. On failure nothing changes and no event is queued, so a
 * partially-applied transaction can never be observed. Returns 0 on success and
 * writes the commit id to *out_commit_id (may be NULL).
 *
 * This only means the transaction was ACCEPTED. Presentation is reported later
 * through the event queue. */
int anland_scene_commit_submit(anland_scene *scene,
                               const anland_layer_state_t *states,
                               size_t count,
                               uint64_t *out_commit_id);

/* ---- backend → scene reporting ---- */

/* Mark successful transport handoff. A later cancellation reports the outcome
 * but does not release buffers still owned by the display backend. Serialized
 * with submit/present/cancellation by the adapter caller. */
int anland_scene_backend_handed_off(anland_scene *scene, uint64_t commit_id);

/* The consumer acknowledged the frame (legacy: buffer rotation; not proof of
 * physical scanout — see the event enum comment). Completes the in-flight commit
 * ONLY. The backend reports buffer release separately when reuse is safe. */
int anland_scene_backend_presented(anland_scene *scene, uint64_t commit_id,
                                   uint64_t presentation_ns);

/* The backend could not present the commit (GPU error, surface lost...). */
int anland_scene_backend_dropped(anland_scene *scene, uint64_t commit_id,
                                 anland_scene_drop_reason_t reason);

/* Report buffer reuse separately from presentation, including imported buffers
 * never committed. Rejects buffers still referenced by an in-flight commit.
 * Takes ownership of release_fence_fd on all paths. */
int anland_scene_backend_release_buffer(anland_scene *scene, uint64_t buffer_id,
                                        int release_fence_fd);

/* Output geometry/refresh changed (consumer reconnected at a new size). */
int anland_scene_backend_output_changed(anland_scene *scene, uint32_t width,
                                        uint32_t height, uint32_t refresh_mhz);

/* The backend published a new render target: the DE may now draw into buffer
 * slot `index` of `count`. Reported on connect (before any frame is flipped) and
 * after every acknowledgement, so it is the signal a DE's frame loop waits for.
 * Deliberately NOT a completion event — see ANLAND_SCENE_EVENT_RENDER_TARGET_READY. */
int anland_scene_backend_render_target_ready(anland_scene *scene,
                                             uint64_t generation,
                                             uint32_t index,
                                             uint32_t count);

/* ---- event delivery ---- */

/* fd that becomes readable when events are queued (eventfd). dispatch() drains
 * the counter before returning, so callers must not drain it separately. -1 if
 * the scene has no event fd. */
int anland_scene_event_fd(const anland_scene *scene);

/* Drain up to `max` events. *out_count receives how many were written. Returns 0
 * on success, -1 on bad arguments. */
int anland_scene_dispatch(anland_scene *scene, anland_scene_event_t *out,
                          size_t max, size_t *out_count);

/* Convenience for tests / poll-free callers: how many events are queued. */
size_t anland_scene_pending_events(const anland_scene *scene);

/* Human-readable names (diagnostics; never NULL). */
const char *anland_scene_event_type_name(anland_scene_event_type_t type);

#ifdef __cplusplus
}
#endif

#endif /* ANLAND_SCENE_H */