/*
 * anland_scene.c — layer/commit state machine for anland_device.
 *
 * Responsibilities (and deliberately nothing else):
 *   - own layer identity (the DE's window → layer mapping stays in the DE)
 *   - validate a transaction ALL-OR-NOTHING before anything changes
 *   - track in-flight commits and report completion/release as events
 *   - survive reconnects without losing buffer-release accounting
 *
 * It knows nothing about Qt/GObject/Rust, dmabuf formats, Wayland resources or
 * Android. Presentation is delegated to anland_scene_backend_ops.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* for dup()/eventfd(); KWin's build already defines it */
#endif
#include "anland_scene.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

/* The legacy presentation backend is strictly one-frame-in-flight: it accepts a
 * commit only while no other commit is pending or outstanding, and it has no
 * way to cancel a commit that a presentation backend has already started using.
 * Keep the scene contract at the same depth so the two never disagree. A wider
 * pipeline can be added when a backend with an explicit retire/cancel path
 * exists (e.g. a DRM device); until then, a second frame is backpressure
 * (the caller keeps its buffer and retries after PRESENTED/COMMIT_DROPPED). */
#define ANLAND_SCENE_INFLIGHT_MAX 1

struct layer_slot {
    bool used;
    anland_layer_id id;
    anland_layer_id parent_id;
    anland_layer_kind_t kind;
    char name[ANLAND_SCENE_MAX_NAME];
    anland_rect_t geometry;
    uint32_t scale;
    int32_t transform;
    int32_t z_order;
    float opacity;
    bool visible;
    uint64_t buffer_id; /* last accepted, for teardown accounting */
};

struct inflight {
    bool used;
    bool handed_off;
    anland_scene_snapshot_t snapshot;
};

struct anland_scene {
    anland_scene_backend_ops_t ops;
    void *userdata;

    pthread_mutex_t lock;


    struct layer_slot layers[ANLAND_SCENE_MAX_LAYERS];
    anland_layer_id next_layer_id;

    uint64_t next_commit_id;
    uint64_t generation;

    struct inflight inflight[ANLAND_SCENE_INFLIGHT_MAX];

    anland_scene_event_t events[ANLAND_SCENE_EVENT_QUEUE];
    size_t qhead;
    size_t qcount;
    /* Release notifications are resource-lifetime events and must not be
     * discarded when diagnostic/output events fill the main queue. This queue
     * grows on demand because release events carry ownership of caller fds. */
    anland_scene_event_t *release_events;
    size_t release_count;
    size_t release_capacity;


    int event_fd;
};

/* ---- event queue (caller holds the lock) ---- */

static void signal_event_fd_locked(anland_scene *s)
{
    if (s->event_fd >= 0) {
        const uint64_t one = 1;
        ssize_t n = write(s->event_fd, &one, sizeof(one));
        (void)n; /* EAGAIN just means the counter is already non-zero */
    }
}

/* Reserve before accepting work; teardown must not allocate to release it. */
static int reserve_releases_locked(anland_scene *s, size_t extra)
{
    if (extra > SIZE_MAX - s->release_count)
        return -1;
    size_t needed = s->release_count + extra;
    if (needed > ANLAND_SCENE_RELEASE_QUEUE_MAX)
        return -1;
    if (needed <= s->release_capacity)
        return 0;
    size_t capacity = needed > ANLAND_SCENE_EVENT_QUEUE ? needed : ANLAND_SCENE_EVENT_QUEUE;
    if (capacity > SIZE_MAX / sizeof(*s->release_events))
        return -1;
    void *events = realloc(s->release_events, capacity * sizeof(*s->release_events));
    if (!events)
        return -1;
    s->release_events = events;
    s->release_capacity = capacity;
    return 0;
}

static void queue_event_locked(anland_scene *s, const anland_scene_event_t *ev)
{
    if (ev->type == ANLAND_SCENE_EVENT_BUFFER_RELEASED) {
        /* Capacity was reserved by submit() or explicit release reporting. */
        s->release_events[s->release_count++] = *ev;
        signal_event_fd_locked(s);
        return;
    }

    /* A render-target publication is superseded state, not a history entry: only
     * the newest one is meaningful, so an undelivered older one is removed before
     * queueing. This keeps a DE that polls slowly from switching to a stale slot,
     * and keeps this event from crowding out commit outcomes.
     *
     * The compaction runs through a scratch buffer because the queue is a ring:
     * with a non-zero head, writing compacted entries back in place would overwrite
     * slots that have not been read yet. */
    if ((ev->type == ANLAND_SCENE_EVENT_RENDER_TARGET_READY ||
         ev->type == ANLAND_SCENE_EVENT_OUTPUT_CHANGED) && s->qcount > 0) {
        anland_scene_event_t kept[ANLAND_SCENE_EVENT_QUEUE];
        size_t n = 0;
        for (size_t i = 0; i < s->qcount; i++) {
            const anland_scene_event_t *src =
                &s->events[(s->qhead + i) % ANLAND_SCENE_EVENT_QUEUE];
            if (src->type == ev->type)
                continue;
            kept[n++] = *src;
        }
        s->qhead = 0;
        s->qcount = n;
        memcpy(s->events, kept, n * sizeof(s->events[0]));
    }

    /* State publications cannot consume the slot reserved for a live outcome. */
    if (ev->type == ANLAND_SCENE_EVENT_OUTPUT_CHANGED ||
        ev->type == ANLAND_SCENE_EVENT_RENDER_TARGET_READY) {
        bool live = false;
        for (size_t i = 0; i < ANLAND_SCENE_INFLIGHT_MAX; i++)
            live |= s->inflight[i].used;
        if (live && s->qcount >= ANLAND_SCENE_EVENT_QUEUE - 1)
            return;
    }

    if (s->qcount == ANLAND_SCENE_EVENT_QUEUE) {
        /* Non-resource events are lossy by design under notification pressure.
         * Resource release events use the separate lossless queue above. Both
         * evictable types are pure state (geometry, current render target) whose
         * newest value is the only one that matters; commit outcomes are not
         * evictable. */
        anland_scene_event_t *oldest = &s->events[s->qhead];
        if (oldest->type == ANLAND_SCENE_EVENT_OUTPUT_CHANGED ||
            oldest->type == ANLAND_SCENE_EVENT_RENDER_TARGET_READY)
            s->qhead = (s->qhead + 1) % ANLAND_SCENE_EVENT_QUEUE;
        else
            return;
        s->qcount--;
    }
    const size_t tail = (s->qhead + s->qcount) % ANLAND_SCENE_EVENT_QUEUE;
    s->events[tail] = *ev;
    s->qcount++;
    signal_event_fd_locked(s);
}

static void queue_presented_locked(anland_scene *s, uint64_t commit_id,
                                   uint64_t presentation_ns)
{
    anland_scene_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = ANLAND_SCENE_EVENT_PRESENTED;
    ev.commit_id = commit_id;
    ev.u.presented.presentation_ns = presentation_ns;
    queue_event_locked(s, &ev);
}

static void queue_dropped_locked(anland_scene *s, uint64_t commit_id,
                                 anland_scene_drop_reason_t reason)
{
    anland_scene_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = ANLAND_SCENE_EVENT_COMMIT_DROPPED;
    ev.commit_id = commit_id;
    ev.u.dropped.reason = reason;
    queue_event_locked(s, &ev);
}

static void queue_released_locked(anland_scene *s, uint64_t buffer_id,
                                  int release_fence_fd)
{
    anland_scene_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = ANLAND_SCENE_EVENT_BUFFER_RELEASED;
    ev.commit_id = 0;
    ev.u.released.buffer_id = buffer_id;
    ev.u.released.release_fence_fd = release_fence_fd;
    queue_event_locked(s, &ev);
}

static void queue_target_ready_locked(anland_scene *s, uint64_t generation,
                                      uint32_t index, uint32_t count)
{
    anland_scene_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = ANLAND_SCENE_EVENT_RENDER_TARGET_READY;
    ev.commit_id = 0;
    ev.u.target_ready.generation = generation;
    ev.u.target_ready.index = index;
    ev.u.target_ready.count = count;
    queue_event_locked(s, &ev);
}

/* ---- layer lookup (caller holds the lock) ---- */

static struct layer_slot *find_layer_locked(anland_scene *s, anland_layer_id id)
{
    if (id == 0)
        return NULL;
    for (size_t i = 0; i < ANLAND_SCENE_MAX_LAYERS; i++) {
        if (s->layers[i].used && s->layers[i].id == id)
            return &s->layers[i];
    }
    return NULL;
}

/* Is this buffer still needed by a commit other than `exclude`? A buffer may be
 * referenced by several queued frames, and must only be released once the last
 * one is done. */
static bool buffer_still_referenced_locked(const anland_scene *s,
                                           const struct inflight *exclude,
                                           uint64_t buffer_id)
{
    if (buffer_id == 0)
        return false;
    for (size_t i = 0; i < ANLAND_SCENE_INFLIGHT_MAX; i++) {
        const struct inflight *fl = &s->inflight[i];
        if (!fl->used || fl == exclude)
            continue;
        for (size_t j = 0; j < fl->snapshot.count; j++) {
            if (fl->snapshot.layers[j].buffer_id == buffer_id)
                return true;
        }
    }
    return false;
}

/* Release every DISTINCT buffer a snapshot references, once each, skipping any
 * buffer another live commit still needs. Caller holds the lock. */
static void release_snapshot_buffers_locked(anland_scene *s,
                                            const struct inflight *fl)
{
    for (size_t i = 0; i < fl->snapshot.count; i++) {
        const uint64_t buf = fl->snapshot.layers[i].buffer_id;
        if (buf == 0)
            continue;
        bool dup = false;
        for (size_t j = 0; j < i; j++) {
            if (fl->snapshot.layers[j].buffer_id == buf) {
                dup = true;
                break;
            }
        }
        if (dup)
            continue;
        if (buffer_still_referenced_locked(s, fl, buf))
            continue;
        queue_released_locked(s, buf, -1);
    }
}

/* Finish an in-flight commit: report the outcome, then retire its buffers ONLY
 * when the outcome means nobody can still be using them.
 *
 * PRESENTED deliberately does NOT imply BUFFER_RELEASED. A DRM-like device may
 * still be scanning out — or asynchronously compositing — the very buffer it just
 * reported as presented, so the commit's outcome and the buffer's lifetime are
 * separate facts. The backend owns the second one: it calls
 * anland_scene_backend_release_buffer() once the buffer is really free (legacy:
 * when the consumer rotated away from it; a future DRM backend: on the retire or
 * release fence). A dropped commit is different — nothing was handed over — so
 * its buffers are retired here. Caller holds the lock. */
static void complete_commit_locked(anland_scene *s, struct inflight *fl,
                                   bool presented, uint64_t presentation_ns,
                                   anland_scene_drop_reason_t reason)
{
    const uint64_t commit_id = fl->snapshot.commit_id;

    if (presented)
        queue_presented_locked(s, commit_id, presentation_ns);
    else
        queue_dropped_locked(s, commit_id, reason);

    if (!presented && !fl->handed_off) {
        release_snapshot_buffers_locked(s, fl);
    }

    fl->used = false;
    fl->handed_off = false;
    memset(&fl->snapshot, 0, sizeof(fl->snapshot));
}

static struct inflight *find_inflight_locked(anland_scene *s, uint64_t commit_id)
{
    if (commit_id == 0)
        return NULL;
    for (size_t i = 0; i < ANLAND_SCENE_INFLIGHT_MAX; i++) {
        if (s->inflight[i].used && s->inflight[i].snapshot.commit_id == commit_id)
            return &s->inflight[i];
    }
    return NULL;
}

static struct inflight *alloc_inflight_locked(anland_scene *s)
{
    for (size_t i = 0; i < ANLAND_SCENE_INFLIGHT_MAX; i++) {
        if (!s->inflight[i].used)
            return &s->inflight[i];
    }
    return NULL;
}

/* ---- validation ---- */

/* Validate ONE layer's state. Pure: no mutation. */
static int validate_state_locked(const anland_scene *s,
                                 const anland_layer_state_t *st)
{
    if (!find_layer_locked((anland_scene *)s, st->layer_id))
        return -1;
    if (st->damage_count > ANLAND_SCENE_MAX_DAMAGE)
        return -1;
    if (st->damage_count > 0 && !st->damage)
        return -1;
    if (st->opacity < 0.0f || st->opacity > 1.0f)
        return -1;
    /* A visible layer with content must land somewhere. A layer without a buffer
     * (buffer_id 0) is a blank/hidden layer and needs no placement. */
    if (st->visible && st->buffer_id != 0) {
        if (st->destination.width == 0 || st->destination.height == 0)
            return -1;
    }
    return 0;
}

/* ---- public API ---- */

anland_scene *anland_scene_create(const anland_scene_backend_ops_t *ops,
                                  void *userdata)
{
    if (!ops || !ops->submit)
        return NULL;

    anland_scene *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;

    s->ops = *ops;
    s->userdata = userdata;
    s->next_layer_id = 1;
    s->next_commit_id = 1;
    s->generation = 1;
    s->event_fd = -1;

    if (pthread_mutex_init(&s->lock, NULL) != 0) {
        free(s);
        return NULL;
    }

    s->event_fd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    if (s->event_fd < 0) {
        pthread_mutex_destroy(&s->lock);
        free(s);
        return NULL;
    }
    return s;
}

void anland_scene_destroy(anland_scene *s)
{
    if (!s)
        return;

    /* Release fences in events that were queued but never dispatched. */
    for (size_t i = 0; i < s->qcount; i++) {
        const size_t pos = (s->qhead + i) % ANLAND_SCENE_EVENT_QUEUE;
        anland_scene_event_t *ev = &s->events[pos];
        if (ev->type == ANLAND_SCENE_EVENT_BUFFER_RELEASED
            && ev->u.released.release_fence_fd >= 0) {
            close(ev->u.released.release_fence_fd);
            ev->u.released.release_fence_fd = -1;
        }
    }
    for (size_t i = 0; i < s->release_count; i++) {
        anland_scene_event_t *ev = &s->release_events[i];
        if (ev->u.released.release_fence_fd >= 0) {
            close(ev->u.released.release_fence_fd);
            ev->u.released.release_fence_fd = -1;
        }
    }

    free(s->release_events);

    if (s->ops.destroy)
        s->ops.destroy(s->userdata);

    if (s->event_fd >= 0)
        close(s->event_fd);
    pthread_mutex_destroy(&s->lock);
    free(s);
}

uint64_t anland_scene_generation(const anland_scene *s)
{
    if (!s)
        return 0;

    pthread_mutex_lock((pthread_mutex_t *)&s->lock);
    const uint64_t generation = s->generation;
    pthread_mutex_unlock((pthread_mutex_t *)&s->lock);
    return generation;
}

bool anland_scene_commit_is_pending(anland_scene *s, uint64_t commit_id)
{
    if (!s || commit_id == 0)
        return false;

    pthread_mutex_lock(&s->lock);
    const bool pending = find_inflight_locked(s, commit_id) != NULL;
    pthread_mutex_unlock(&s->lock);
    return pending;
}

/* Drop queued render-target publications. They describe a buffer slot in the
 * session that is ending, and a later session must not act on it — not even when
 * it happens to reuse the same slot index. Caller holds the lock. */
static void remove_queued_render_target_events_locked(anland_scene *s)
{
    anland_scene_event_t kept[ANLAND_SCENE_EVENT_QUEUE];
    size_t n = 0;

    for (size_t i = 0; i < s->qcount; i++) {
        const anland_scene_event_t *src =
            &s->events[(s->qhead + i) % ANLAND_SCENE_EVENT_QUEUE];
        if (src->type == ANLAND_SCENE_EVENT_RENDER_TARGET_READY)
            continue;
        kept[n++] = *src;
    }

    s->qhead = 0;
    s->qcount = n;
    if (n > 0)
        memcpy(s->events, kept, n * sizeof(s->events[0]));
}

int anland_scene_invalidate(anland_scene *s)
{
    if (!s)
        return -1;

    pthread_mutex_lock(&s->lock);
    for (size_t i = 0; i < ANLAND_SCENE_INFLIGHT_MAX; i++) {
        if (s->inflight[i].used) {
            /* A handed-off commit is normally retired by the backend when the
             * consumer rotates the slot. A session teardown removes the
             * consumer entirely: nothing can still be scanning out, so the
             * buffers must be retired HERE. The legacy adapter leaves live
             * commits to this path and retries only identities no longer owned
             * by the scene. Releasing them before complete_commit_locked()
             * keeps exactly one BUFFER_RELEASED per
             * distinct buffer, because complete_commit_locked() skips its own
             * release for handed-off commits. */
            if (s->inflight[i].handed_off)
                release_snapshot_buffers_locked(s, &s->inflight[i]);
            complete_commit_locked(s, &s->inflight[i], false, 0,
                                   ANLAND_SCENE_DROP_INVALIDATED);
        }
    }

    /* The generation identifies the session, so it MUST advance on every
     * invalidation — not only when a frame happened to be in flight. A
     * disconnect while idle still ends one session and starts another, and a
     * stale event from the old one has to be recognisable as stale. */
    s->generation++;

    /* Anything the old session published is meaningless now: its target slot index
     * refers to buffers this scene will re-import. */
    remove_queued_render_target_events_locked(s);

    pthread_mutex_unlock(&s->lock);
    return 0;
}

int anland_scene_layer_create(anland_scene *s,
                              const anland_layer_desc_t *desc,
                              anland_layer_id *out_id)
{
    if (!s || !desc || !out_id)
        return -1;

    pthread_mutex_lock(&s->lock);

    struct layer_slot *slot = NULL;
    for (size_t i = 0; i < ANLAND_SCENE_MAX_LAYERS; i++) {
        if (!s->layers[i].used) {
            slot = &s->layers[i];
            break;
        }
    }
    if (!slot) {
        pthread_mutex_unlock(&s->lock);
        return -1;
    }

    /* A parent must already exist: layer identity is created top-down. */
    if (desc->parent_id != 0 && !find_layer_locked(s, desc->parent_id)) {
        pthread_mutex_unlock(&s->lock);
        return -1;
    }

    memset(slot, 0, sizeof(*slot));
    slot->used = true;
    slot->id = s->next_layer_id++;
    slot->parent_id = desc->parent_id;
    slot->kind = desc->kind;
    slot->geometry = desc->geometry;
    slot->scale = desc->scale;
    slot->transform = desc->transform;
    slot->z_order = desc->z_order;
    slot->opacity = desc->opacity;
    slot->visible = desc->visible;
    if (desc->name)
        snprintf(slot->name, sizeof(slot->name), "%s", desc->name);
    *out_id = slot->id;
    pthread_mutex_unlock(&s->lock);
    return 0;
}

int anland_scene_layer_destroy(anland_scene *s, anland_layer_id id)
{
    if (!s || id == 0)
        return -1;

    pthread_mutex_lock(&s->lock);

    struct layer_slot *slot = find_layer_locked(s, id);
    if (!slot) {
        pthread_mutex_unlock(&s->lock);
        return -1;
    }

    /* Drop any in-flight commit that references this layer, so the DE learns its
     * buffers were released instead of waiting forever for a frame that will
     * never be presented. */
    for (size_t i = 0; i < ANLAND_SCENE_INFLIGHT_MAX; i++) {
        struct inflight *fl = &s->inflight[i];
        if (!fl->used)
            continue;
        for (size_t j = 0; j < fl->snapshot.count; j++) {
            if (fl->snapshot.layers[j].layer_id == id) {
                complete_commit_locked(s, fl, false, 0,
                                       ANLAND_SCENE_DROP_INVALIDATED);
                break;
            }
        }
    }

    /* Reparent children to the root rather than orphaning them. */
    for (size_t i = 0; i < ANLAND_SCENE_MAX_LAYERS; i++) {
        if (s->layers[i].used && s->layers[i].parent_id == id)
            s->layers[i].parent_id = 0;
    }

    memset(slot, 0, sizeof(*slot));
    pthread_mutex_unlock(&s->lock);
    return 0;
}

int anland_scene_layer_update(anland_scene *s,
                              anland_layer_id id,
                              const anland_layer_desc_t *desc)
{
    if (!s || id == 0 || !desc)
        return -1;
    pthread_mutex_lock(&s->lock);
    struct layer_slot *slot = find_layer_locked(s, id);
    if (!slot || desc->parent_id != slot->parent_id || desc->kind != slot->kind ||
        desc->opacity < 0.0f || desc->opacity > 1.0f) {
        pthread_mutex_unlock(&s->lock);
        return -1;
    }
    slot->geometry = desc->geometry;
    slot->scale = desc->scale;
    slot->transform = desc->transform;
    slot->z_order = desc->z_order;
    slot->opacity = desc->opacity;
    slot->visible = desc->visible;
    if (desc->name) {
        memset(slot->name, 0, sizeof(slot->name));
        snprintf(slot->name, sizeof(slot->name), "%s", desc->name);
    }
    pthread_mutex_unlock(&s->lock);
    return 0;
}
int anland_scene_layer_count(const anland_scene *s)
{
    if (!s)
        return 0;
    int n = 0;
    for (size_t i = 0; i < ANLAND_SCENE_MAX_LAYERS; i++) {
        if (s->layers[i].used)
            n++;
    }
    return n;
}

int anland_scene_commit_submit(anland_scene *s,
                               const anland_layer_state_t *states,
                               size_t count,
                               uint64_t *out_commit_id)
{
    if (!s || (!states && count > 0))
        return -1;
    if (count > ANLAND_SCENE_MAX_LAYERS)
        return -1;

    pthread_mutex_lock(&s->lock);

    /* ---- phase 1: validate everything, mutate nothing ---- */
    for (size_t i = 0; i < count; i++) {
        if (validate_state_locked(s, &states[i]) != 0) {
            pthread_mutex_unlock(&s->lock);
            return -1;
        }
        /* A layer may appear at most once per transaction. */
        for (size_t j = 0; j < i; j++) {
            if (states[j].layer_id == states[i].layer_id) {
                pthread_mutex_unlock(&s->lock);
                return -1;
            }
        }
    }

    /* Each accepted or backend-rejected transaction owes one outcome. */
    if (s->qcount >= ANLAND_SCENE_EVENT_QUEUE) {
        pthread_mutex_unlock(&s->lock);
        return -1;
    }
    /* Keep capacity for all accepted buffers, including invalidation/drop. */
    if (reserve_releases_locked(s, ANLAND_SCENE_MAX_LAYERS) != 0) {
        pthread_mutex_unlock(&s->lock);
        return -1;
    }

    /* ---- phase 2: build the snapshot (still no visible change) ---- */
    struct inflight *fl = alloc_inflight_locked(s);
    if (!fl) {
        /* One commit in flight already: backpressure. Rejecting here leaves the
         * caller's buffers untouched and lets the DE retry after the in-flight
         * commit completes. Deliberately NOT superseding the oldest: the legacy
         * backend may already hold/use that buffer and there is no cancel path. */
        pthread_mutex_unlock(&s->lock);
        return -1;
    }

    memset(&fl->snapshot, 0, sizeof(fl->snapshot));
    /* Allocate the identity before calling the backend. Rejected commits still
     * consume an id, so a dropped event can never collide with a later commit. */
    fl->snapshot.commit_id = s->next_commit_id++;
    fl->snapshot.generation = s->generation;
    fl->snapshot.count = count;

    for (size_t i = 0; i < count; i++) {
        const anland_layer_state_t *src = &states[i];
        anland_scene_layer_snapshot_t *dst = &fl->snapshot.layers[i];
        dst->layer_id = src->layer_id;
        dst->buffer_id = src->buffer_id;
        dst->source = src->source;
        dst->destination = src->destination;
        dst->z_order = src->z_order;
        dst->opacity = src->opacity;
        dst->visible = src->visible;
        dst->acquire_fence_fd = src->acquire_fence_fd;
        dst->damage_count = src->damage_count;
        for (size_t d = 0; d < src->damage_count; d++)
            dst->damage[d] = src->damage[d];
    }

    /* ---- phase 3: hand to the backend ---- */
    const int rc = s->ops.submit(s->userdata, &fl->snapshot);
    if (rc != 0) {
        /* Rejected: report the drop and release the buffers immediately, since no
         * frame is in flight for them. */
        const uint64_t commit_id = fl->snapshot.commit_id;
        fl->used = false;
        queue_dropped_locked(s, commit_id, ANLAND_SCENE_DROP_BACKEND_ERROR);
        for (size_t i = 0; i < count; i++) {
            const uint64_t buf = fl->snapshot.layers[i].buffer_id;
            if (buf == 0)
                continue;
            bool dup = false;
            for (size_t j = 0; j < i; j++) {
                if (fl->snapshot.layers[j].buffer_id == buf) {
                    dup = true;
                    break;
                }
            }
            if (!dup && !buffer_still_referenced_locked(s, NULL, buf))
                queue_released_locked(s, buf, -1);
        }
        memset(&fl->snapshot, 0, sizeof(fl->snapshot));
        pthread_mutex_unlock(&s->lock);
        return -1;
    }

    fl->used = true;

    /* Remember the accepted state per layer (for teardown accounting). */
    for (size_t i = 0; i < count; i++) {
        struct layer_slot *slot = find_layer_locked(s, states[i].layer_id);
        if (slot)
            slot->buffer_id = states[i].buffer_id;
    }

    const uint64_t commit_id = fl->snapshot.commit_id;
    if (out_commit_id)
        *out_commit_id = commit_id;

    pthread_mutex_unlock(&s->lock);
    return 0;
}

/* Serialized adapter calls mark handoff before cancellation can occur. */
int anland_scene_backend_handed_off(anland_scene *s, uint64_t commit_id)
{
    if (!s) return -1;
    pthread_mutex_lock(&s->lock);
    struct inflight *fl = find_inflight_locked(s, commit_id);
    if (fl) fl->handed_off = true;
    pthread_mutex_unlock(&s->lock);
    return fl ? 0 : -1;
}

int anland_scene_backend_presented(anland_scene *s, uint64_t commit_id,
                                   uint64_t presentation_ns)
{
    if (!s || commit_id == 0)
        return -1;

    pthread_mutex_lock(&s->lock);
    struct inflight *fl = find_inflight_locked(s, commit_id);
    if (!fl) {
        pthread_mutex_unlock(&s->lock);
        return -1; /* already completed, or never known */
    }
    complete_commit_locked(s, fl, true, presentation_ns, 0);
    pthread_mutex_unlock(&s->lock);
    return 0;
}

int anland_scene_backend_dropped(anland_scene *s, uint64_t commit_id,
                                 anland_scene_drop_reason_t reason)
{
    if (!s || commit_id == 0)
        return -1;

    pthread_mutex_lock(&s->lock);
    struct inflight *fl = find_inflight_locked(s, commit_id);
    if (!fl) {
        pthread_mutex_unlock(&s->lock);
        return -1;
    }
    complete_commit_locked(s, fl, false, 0, reason);
    pthread_mutex_unlock(&s->lock);
    return 0;
}

int anland_scene_backend_release_buffer(anland_scene *s, uint64_t buffer_id,
                                        int release_fence_fd)
{
    if (!s || buffer_id == 0) {
        if (release_fence_fd >= 0)
            close(release_fence_fd);
        return -1;
    }

    pthread_mutex_lock(&s->lock);

    /* A buffer still named by a live commit is not free yet: releasing it now
     * would tell the DE it may overwrite something the backend is about to use.
     * This is the only condition: a buffer imported but never committed has no
     * live commit either, which is exactly the documented "imported but never
     * committed" release path. */
    if (buffer_still_referenced_locked(s, NULL, buffer_id)) {
        if (release_fence_fd >= 0)
            close(release_fence_fd);
        pthread_mutex_unlock(&s->lock);
        return -1;
    }

    /* Preserve space for the live commit's eventual drop as well. */
    if (reserve_releases_locked(s, ANLAND_SCENE_MAX_LAYERS + 1) != 0) {
        if (release_fence_fd >= 0)
            close(release_fence_fd);
        pthread_mutex_unlock(&s->lock);
        return -1;
    }
    queue_released_locked(s, buffer_id, release_fence_fd);
    pthread_mutex_unlock(&s->lock);
    return 0;
}

int anland_scene_backend_output_changed(anland_scene *s, uint32_t width,
                                        uint32_t height, uint32_t refresh_mhz)
{
    if (!s)
        return -1;

    anland_scene_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = ANLAND_SCENE_EVENT_OUTPUT_CHANGED;
    ev.commit_id = 0;
    ev.u.output.width = width;
    ev.u.output.height = height;
    ev.u.output.refresh_mhz = refresh_mhz;

    pthread_mutex_lock(&s->lock);
    queue_event_locked(s, &ev);
    pthread_mutex_unlock(&s->lock);
    return 0;
}

int anland_scene_backend_render_target_ready(anland_scene *s,
                                             uint64_t generation,
                                             uint32_t index,
                                             uint32_t count)
{
    if (!s || count == 0 || index >= count)
        return -1;

    pthread_mutex_lock(&s->lock);
    /* A publication from a session the scene has already moved past is stale by
     * definition: reporting it would point the DE at a buffer that no longer
     * belongs to the live consumer. Drop it instead of queueing. */
    if (generation != 0 && generation != s->generation) {
        pthread_mutex_unlock(&s->lock);
        return -1;
    }
    queue_target_ready_locked(s, generation ? generation : s->generation,
                              index, count);
    pthread_mutex_unlock(&s->lock);
    return 0;
}

int anland_scene_event_fd(const anland_scene *s)
{
    return s ? s->event_fd : -1;
}

static void drain_event_fd_locked(anland_scene *s)
{
    if (s->event_fd < 0)
        return;

    uint64_t value;
    while (read(s->event_fd, &value, sizeof(value)) == sizeof(value)) {
    }
}

int anland_scene_dispatch(anland_scene *s, anland_scene_event_t *out,
                          size_t max, size_t *out_count)
{
    if (!s || !out || !out_count)
        return -1;

    pthread_mutex_lock(&s->lock);
    drain_event_fd_locked(s);
    size_t n = 0;
    while (n < max && s->qcount > 0) {
        out[n++] = s->events[s->qhead];
        s->qhead = (s->qhead + 1) % ANLAND_SCENE_EVENT_QUEUE;
        s->qcount--;
    }
    while (n < max && s->release_count > 0) {
        out[n++] = s->release_events[0];
        memmove(&s->release_events[0], &s->release_events[1],
                (s->release_count - 1) * sizeof(s->release_events[0]));
        s->release_count--;
    }

    /* dispatch() drained the event counter above, so anything still queued needs
     * a fresh wakeup: a caller that waits on the event fd must not sleep through
     * releases that did not fit into this batch. */
    if (s->qcount != 0 || s->release_count != 0)
        signal_event_fd_locked(s);

    pthread_mutex_unlock(&s->lock);

    *out_count = n;
    return 0;
}

size_t anland_scene_pending_events(const anland_scene *s)
{
    if (!s)
        return 0;

    pthread_mutex_lock((pthread_mutex_t *)&s->lock);
    const size_t count = s->qcount + s->release_count;
    pthread_mutex_unlock((pthread_mutex_t *)&s->lock);
    return count;
}

const char *anland_scene_event_type_name(anland_scene_event_type_t type)
{
    switch (type) {
    case ANLAND_SCENE_EVENT_PRESENTED:
        return "PRESENTED";
    case ANLAND_SCENE_EVENT_BUFFER_RELEASED:
        return "BUFFER_RELEASED";
    case ANLAND_SCENE_EVENT_COMMIT_DROPPED:
        return "COMMIT_DROPPED";
    case ANLAND_SCENE_EVENT_OUTPUT_CHANGED:
        return "OUTPUT_CHANGED";
    case ANLAND_SCENE_EVENT_RENDER_TARGET_READY:
        return "RENDER_TARGET_READY";
    }
    return "UNKNOWN";
}