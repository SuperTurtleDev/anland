/*
 * anland_scene_legacy.c — anland_scene backend over the legacy fullscreen transport.
 *
 * Read the SCOPE / LIMITS block in anland_scene_legacy.h first: this adapter
 * FLATTENS the scene onto the single-output legacy transport. That is the shape
 * the product needs (one container, one DE, one Android Surface); per-window
 * Surfaces are out of scope.
 *
 * Frame lifecycle mapped onto the legacy transport:
 *
 *   submit()   → accepted only while connected and with no frame outstanding;
 *                records the commit as PENDING. Never touches the device, and
 *                never calls back into the scene: submit() runs while the scene
 *                holds its lock, so re-entering would deadlock.
 *   present()  → drains any stale buffer-ready signal, then commits + flips the
 *                buffer the consumer selected. The commit becomes IN FLIGHT.
 *   pump()     → when the consumer raises buffer-ready again (it consumed the
 *                frame and wants the next buffer), the in-flight commit is
 *                reported PRESENTED. That is the only point where the frame is
 *                known to have reached the consumer.
 *
 * The stale-signal drain in present() is what keeps the accounting honest: the
 * consumer's handshake raises buffer-ready before the producer has ever flipped,
 * so without draining, the first pump() would report a frame as presented that
 * the consumer had not yet consumed.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* for dup()/eventfd(); KWin's build already defines it */
#endif
#include "anland_scene_legacy.h"

#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <unistd.h>

struct anland_scene_legacy {
    anland_device *dev;
    anland_scene *scene;

    /* Accepted by submit(), not yet flipped. */
    uint64_t pending_commit;

    /* Distinct buffers the pending commit referenced. In the flattened model every
     * layer is composited into the output buffer, but a commit may legitimately
     * name several buffer ids (a DE that tracks per-layer buffers), and each one
     * is in use until the frame completes — so each one owes a release. */
    uint64_t pending_buffers[ANLAND_SCENE_MAX_LAYERS];
    size_t pending_buffer_count;

    /* Acquire fence carried by the pending commit, or -1.
     *
     * The scene contract states the fd passed to submit() is BORROWED and only
     * valid for that call, so the adapter dups it to keep it usable until
     * present(). Ownership then transfers to the transport. */
    int pending_fence;

    /* Flipped, awaiting the consumer's acknowledgement. */
    uint64_t inflight_commit;
    /* Buffer ids retained per physical slot until that slot is selected again.
     * A new target proves only that selected slot is writable, not the old one. */
    uint64_t retained[ANLAND_DEVICE_MAX_BUFS][ANLAND_SCENE_MAX_LAYERS];
    size_t retained_count[ANLAND_DEVICE_MAX_BUFS];
    /* Buffers the in-flight frame used; released once the consumer rotated away
     * from them (see pump()). */
    uint64_t inflight_buffers[ANLAND_SCENE_MAX_LAYERS];
    size_t inflight_buffer_count;

    /* Generation of the consumer session this adapter is serving. Captured when
     * the session is established and used for every publication, so a render
     * target can never be attributed to a session it did not come from. */
    uint64_t session_generation;
    /* True between a successful connect and the next session teardown. Keeps
     * drop_session() from invalidating an already-dead session over and over. */
    bool session_valid;
    /* Old identities/events must retire before a handshake can reuse buffer ids. */
    bool retirement_pending;
    /* eventfd ACK was consumed, but slot release/publication may need a retry. */
    bool ready_pending;
    int ready_slot;

    anland_device_output_t output;
    bool have_output;

    uint64_t frames_flipped;
    uint64_t frames_acked;
    uint64_t reconnects;
};

/* ---- scene backend ops (called with the scene lock held) ---- */

static int legacy_submit(void *ud, const anland_scene_snapshot_t *snap)
{
    struct anland_scene_legacy *b = ud;

    /* No consumer: there is nobody to present to. Rejecting here is what turns
     * into COMMIT_DROPPED(BACKEND_ERROR) for the DE. */
    if (!b->session_valid || b->ready_pending || b->retirement_pending ||
        !anland_device_is_connected(b->dev))
        return -1;

    /* One frame at a time — the transport's own back-pressure. */
    if (b->pending_commit != 0 || b->inflight_commit != 0)
        return -1;

    /* Forward the commit's acquire fence. In the flattened model every layer is
     * composited into the SAME output buffer, so one fence guards the frame; the
     * adapter takes the first one present. The contract only guarantees the fd
     * for the duration of this call, so dup it now.
     *
     * Rejecting on dup failure (rather than silently dropping the fence) keeps
     * GPU synchronisation honest: presenting a frame whose fence was lost could
     * hand the consumer a buffer that is still being written. */
    int fence = -1;
    size_t buffer_count = 0;
    for (size_t i = 0; i < snap->count; i++) {
        if (fence < 0 && snap->layers[i].acquire_fence_fd >= 0) {
            fence = dup(snap->layers[i].acquire_fence_fd);
            if (fence < 0)
                return -1;
        }

        /* Remember every DISTINCT buffer this frame uses: each one is in use
         * until the frame completes, so each one owes exactly one release. A
         * commit that names no buffer simply has nothing to release. */
        const uint64_t buffer_id = snap->layers[i].buffer_id;
        if (buffer_id == 0)
            continue;
        bool seen = false;
        for (size_t j = 0; j < buffer_count; j++) {
            if (b->pending_buffers[j] == buffer_id) {
                seen = true;
                break;
            }
        }
        if (seen)
            continue;
        if (buffer_count >= ANLAND_SCENE_MAX_LAYERS)
            break; /* cannot happen: count <= ANLAND_SCENE_MAX_LAYERS */
        b->pending_buffers[buffer_count++] = buffer_id;
    }

    /* NOTE: only bookkeeping. Do NOT call anland_scene_* from here. */
    b->pending_commit = snap->commit_id;
    b->pending_buffer_count = buffer_count;
    b->pending_fence = fence;
    return 0;
}

static void legacy_ops_destroy(void *ud)
{
    (void)ud;
}

/* ---- helpers ---- */

/* Report the current output geometry, emitting OUTPUT_CHANGED when it differs
 * from what the DE was last told. */
static bool refresh_output(struct anland_scene_legacy *b)
{
    anland_device_output_t out;
    memset(&out, 0, sizeof(out));
    /* Connected output geometry comes from the validated consumer dmabuf set
     * (anland_device_get_outputs), not the producer HELLO's cached screen_info. */
    if (anland_device_get_outputs(b->dev, &out, 1) != 1 ||
        (anland_device_is_connected(b->dev) && (!out.width || !out.height)))
        return false;

    const bool changed = !b->have_output
        || out.width != b->output.width
        || out.height != b->output.height
        || out.refresh_mhz != b->output.refresh_mhz;

    b->output = out;
    b->have_output = true;

    if (changed)
        anland_scene_backend_output_changed(b->scene, out.width, out.height,
                                            out.refresh_mhz);
    return true;
}

/* Publish the buffer slot the consumer wants the producer to render into next.
 *
 * This is the RENDER TARGET signal, and it is what a DE's frame loop waits for:
 * the handshake publishes a selection before any frame has been flipped, so the
 * first publication starts rendering without ever claiming a presentation.
 *
 * An index outside the consumer's own buffer set means the session is corrupt;
 * report that instead of publishing a slot the DE cannot map to an imported
 * framebuffer. */
static int publish_target_ready(struct anland_scene_legacy *b)
{
    const int count = anland_device_fb_count(b->dev);
    if (count <= 0)
        return -1;

    const int index = anland_device_current_fb_raw(b->dev);
    if (index < 0 || index >= count)
        return -1;

    /* Publish under the SESSION's generation, not the scene's current one. If the
     * session was torn down in between, the scene has moved on and this
     * publication must be refused rather than attributed to the new session. */
    return anland_scene_backend_render_target_ready(b->scene,
                                                    b->session_generation,
                                                    (uint32_t) index,
                                                    (uint32_t) count);
}

/* Discard any buffer-ready signal the consumer raised before we flipped.
 *
 * The transport's eventfd is NOT non-blocking, so poll first — a bare
 * eventfd_read() would block forever when nothing is pending. One read is enough:
 * an eventfd read returns the accumulated counter and resets it to zero. */
static void drain_buffer_ready(struct anland_scene_legacy *b)
{
    const int fd = anland_device_buffer_ready_fd(b->dev);
    if (fd < 0)
        return;

    struct pollfd p = { .fd = fd, .events = POLLIN, .revents = 0 };
    if (poll(&p, 1, 0) <= 0 || !(p.revents & POLLIN))
        return;

    eventfd_t v;
    (void)eventfd_read(fd, &v);
}

/* Abandon the accepted-but-not-flipped frame: close its fence so the fd does not
 * leak, and let the scene report the drop + release the buffers. */
static void drop_pending(struct anland_scene_legacy *b)
{
    if (b->pending_fence >= 0) {
        close(b->pending_fence);
        b->pending_fence = -1;
    }
    b->pending_commit = 0;
    b->pending_buffer_count = 0;
}

/* Remove all aliases only after a release was queued successfully (or the
 * scene itself retires a live commit during invalidation). */
static void forget_retained(struct anland_scene_legacy *b, uint64_t buffer)
{
    for (size_t slot = 0; slot < ANLAND_DEVICE_MAX_BUFS; ++slot) {
        size_t n = 0;
        for (size_t i = 0; i < b->retained_count[slot]; ++i)
            if (b->retained[slot][i] != buffer)
                b->retained[slot][n++] = b->retained[slot][i];
        b->retained_count[slot] = n;
    }
}

/* Failed publications leave their identities intact. No allocations are needed
 * to remember this debt: retained[] already owns every handed-off buffer. */
static int retry_retirement(struct anland_scene_legacy *b)
{
    for (size_t slot = 0; slot < ANLAND_DEVICE_MAX_BUFS; ++slot) {
        while (b->retained_count[slot]) {
            uint64_t buffer = b->retained[slot][b->retained_count[slot] - 1];
            if (anland_scene_backend_release_buffer(b->scene, buffer, -1) != 0)
                return -1;
            forget_retained(b, buffer);
        }
    }
    return 0;
}

/* End a session exactly once. Submission outcome and buffer retirement are
 * separate: a cancelled handed-off commit no longer exists in the scene but
 * its retained[] entry still owes a release. */
static void drop_session(struct anland_scene_legacy *b)
{
    if (b->session_valid || b->pending_commit || b->inflight_commit) {
        /* Only commits that STILL exist will be released by invalidate(). A
         * cancelled handed-off commit must remain in retained[] for retry. */
        if (anland_scene_commit_is_pending(b->scene, b->pending_commit)) {
            for (size_t i = 0; i < b->pending_buffer_count; ++i)
                forget_retained(b, b->pending_buffers[i]);
        }
        if (anland_scene_commit_is_pending(b->scene, b->inflight_commit)) {
            for (size_t i = 0; i < b->inflight_buffer_count; ++i)
                forget_retained(b, b->inflight_buffers[i]);
        }

        /* invalidate() uses the headroom reserved for the live commit, including
         * handed-off commits. It must run only once, not once per retry. */
        anland_scene_invalidate(b->scene);
        drop_pending(b);
        b->inflight_commit = 0;
        b->inflight_buffer_count = 0;
        b->session_valid = false;
        b->retirement_pending = true;
        b->ready_pending = false;
        b->have_output = false;
        memset(&b->output, 0, sizeof(b->output));
    }
    if (b->retirement_pending)
        (void)retry_retirement(b);
}

/* ---- lifecycle ---- */

anland_scene_legacy *anland_scene_legacy_create(const char *socket_path)
{
    struct anland_scene_legacy *b = calloc(1, sizeof(*b));
    if (!b)
        return NULL;

    /* calloc() leaves 0, which is a VALID fd number (stdin). Every optional fd in
     * this adapter must start as -1 or a drop path would close someone else's fd. */
    b->pending_fence = -1;

    b->dev = anland_device_open(socket_path);
    if (!b->dev) {
        free(b);
        return NULL;
    }

    const anland_scene_backend_ops_t ops = {
        .submit = legacy_submit,
        .destroy = legacy_ops_destroy,
    };
    b->scene = anland_scene_create(&ops, b);
    if (!b->scene) {
        anland_device_close(b->dev);
        free(b);
        return NULL;
    }
    return b;
}

void anland_scene_legacy_destroy(anland_scene_legacy *b)
{
    if (!b)
        return;
    /* A frame may have been accepted but never flipped; its fence is ours to close. */
    drop_pending(b);
    /* Scene first: its destroy hook must not observe a freed device. */
    anland_scene_destroy(b->scene);
    anland_device_close(b->dev);
    free(b);
}

anland_scene *anland_scene_legacy_scene(anland_scene_legacy *b)
{
    return b ? b->scene : NULL;
}

anland_device *anland_scene_legacy_device(anland_scene_legacy *b)
{
    return b ? b->dev : NULL;
}

bool anland_scene_legacy_connected(anland_scene_legacy *b)
{
    return b && anland_device_is_connected(b->dev);
}

bool anland_scene_legacy_target_available(anland_scene_legacy *b)
{
    return b && b->session_valid && !b->ready_pending &&
           !b->retirement_pending && anland_device_is_connected(b->dev);
}

int anland_scene_legacy_reconnect(anland_scene_legacy *b)
{
    if (!b)
        return -1;

    if (anland_device_is_connected(b->dev)) {
        if (!refresh_output(b)) {
            drop_session(b);
            anland_device_force_fallback(b->dev);
            return -1;
        }
        return 0;
    }

    /* Retire the old session BEFORE connecting. Even successfully queued
     * releases must be delivered before a new session may reuse the same ids. */
    drop_session(b);
    if (b->retirement_pending) {
        if (retry_retirement(b) != 0 || anland_scene_pending_events(b->scene) != 0)
            return -1;
        b->retirement_pending = false;
    }
    if (anland_device_connect(b->dev) != 0)
        return -1;
    b->reconnects++;

    /* This session's identity: every publication below is stamped with it, and a
     * publication from a session that has already ended is refused by the scene. */
    b->session_generation = anland_scene_generation(b->scene);
    b->session_valid = true;

    if (!refresh_output(b)) {
        drop_session(b);
        anland_device_force_fallback(b->dev);
        return -1;
    }

    /* The handshake already published which buffer the consumer wants first, so
     * announce the render target now: this is what lets a DE start (or resume)
     * its frame loop. It is not a presentation — nothing has been flipped yet. */
    if (publish_target_ready(b) != 0) {
        /* An unusable selection is not a usable session: detach so the caller's
         * next reconnect() runs a fresh handshake instead of rendering blind. */
        drop_session(b);
        anland_device_force_fallback(b->dev);
        return -1;
    }
    return 0;
}

int anland_scene_legacy_reopen(anland_scene_legacy *b, const char *socket_path)
{
    if (!b)
        return -1;

    /* Close any adapter-owned fence and invalidate scene work before replacing
     * the transport context. The scene/layer objects deliberately survive. */
    drop_session(b);
    anland_device_force_fallback(b->dev);
    if (anland_device_reopen(b->dev, socket_path) != 0)
        return -1;

    b->have_output = false;
    return 0;
}

void anland_scene_legacy_drop_session(anland_scene_legacy *b)
{
    if (!b)
        return;

    drop_session(b);

    /* Detaching is what makes the next reconnect() run the handshake again: while
     * the device still reports "connected", reconnect() takes the
     * already-connected shortcut and the DE would keep using a session it has
     * just declared unusable. */
    anland_device_force_fallback(b->dev);
}

/* ---- frame path ---- */

int anland_scene_legacy_present(anland_scene_legacy *b)
{
    if (!b)
        return -1;
    if (b->pending_commit == 0)
        return -1; /* nothing accepted, or already flipped */
    if (!b->session_valid || b->ready_pending ||
        !anland_device_is_connected(b->dev))
        return -1;

    /* Scene operations are serialized with this adapter by its caller. A layer
     * destroyed before handoff has already dropped and released this commit. */
    if (!anland_scene_commit_is_pending(b->scene, b->pending_commit)) {
        drop_pending(b);
        return -1;
    }

    const int slot = anland_device_current_fb_raw(b->dev);
    if (slot < 0 || slot >= anland_device_fb_count(b->dev) ||
        slot >= ANLAND_DEVICE_MAX_BUFS || b->retained_count[slot] != 0)
        return -1;

    /* Clear the consumer's pre-flip signal so the NEXT one is unambiguously the
     * acknowledgement of this frame. */
    drain_buffer_ready(b);

    /* Flip with THIS commit's fence. pageflip() stashes the fence and triggers the
     * refresh; it takes ownership unconditionally (even on the error path), so the
     * adapter must not close it afterwards. Passing -1 when the commit carried no
     * fence correctly clears any predecessor.
     *
     * Do not replace this with a bare trigger_refresh(): that would send whatever
     * fence a previous frame left stashed and silently drop this one. */
    const int fence = b->pending_fence;
    b->pending_fence = -1;

    if (anland_device_pageflip(b->dev, fence, NULL, NULL) != 0) {
        const uint64_t failed = b->pending_commit;
        drop_pending(b);
        anland_scene_backend_dropped(b->scene, failed, ANLAND_SCENE_DROP_BACKEND_ERROR);
        return -1;
    }

    anland_scene_backend_handed_off(b->scene, b->pending_commit);
    memcpy(b->retained[slot], b->pending_buffers,
           b->pending_buffer_count * sizeof(uint64_t));
    b->retained_count[slot] = b->pending_buffer_count;
    b->inflight_commit = b->pending_commit;
    memcpy(b->inflight_buffers, b->pending_buffers,
           b->pending_buffer_count * sizeof(b->inflight_buffers[0]));
    b->inflight_buffer_count = b->pending_buffer_count;
    b->pending_commit = 0;
    b->pending_buffer_count = 0;
    b->frames_flipped++;
    return 0;
}

int anland_scene_legacy_pump(anland_scene_legacy *b, int timeout_ms)
{
    if (!b)
        return -1;

    if (!anland_device_is_connected(b->dev)) {
        /* Consumer gone: nothing in flight can complete. Drop it so the DE is not
         * left waiting on a dead frame. This does NOT reconnect: establishing a new
         * session changes the fds, so the DE must re-import buffers and rebuild its
         * event sources first. Reconnecting here would hand it a live session it is
         * not wired to. Call anland_scene_legacy_reconnect() explicitly. */
        drop_session(b);
        return 0;
    }

    if (!b->ready_pending) {
        const int fd = anland_device_buffer_ready_fd(b->dev);
        if (fd < 0)
            return 0;
        struct pollfd p = { .fd = fd, .events = POLLIN, .revents = 0 };
        if (poll(&p, 1, timeout_ms) <= 0 || !(p.revents & POLLIN))
            return 0;
        eventfd_t v;
        if (eventfd_read(fd, &v) != 0)
            return 0;

        /* Remember the consumed ACK before any fallible release publication.
         * Subsequent pump() calls retry without waiting for another eventfd. */
        b->ready_pending = true;
        b->ready_slot = anland_device_current_fb_raw(b->dev);
        if (b->inflight_commit != 0) {
            const uint64_t done = b->inflight_commit;
            b->inflight_commit = 0;
            b->inflight_buffer_count = 0;
            if (anland_scene_backend_presented(b->scene, done, 0) == 0)
                b->frames_acked++;
        }
    }
    const int slot = b->ready_slot;
    if (slot < 0 || slot >= anland_device_fb_count(b->dev) ||
        slot >= ANLAND_DEVICE_MAX_BUFS ||
        slot != anland_device_current_fb_raw(b->dev)) {
        drop_session(b);
        anland_device_force_fallback(b->dev);
        return -1;
    }
    while (b->retained_count[slot] != 0) {
        size_t i = b->retained_count[slot] - 1;
        if (anland_scene_backend_release_buffer(b->scene, b->retained[slot][i], -1) != 0)
            return -1;
        b->retained_count[slot]--;
    }
    if (publish_target_ready(b) != 0) {
        drop_session(b);
        anland_device_force_fallback(b->dev);
        return -1;
    }
    b->ready_pending = false;
    return 0;
}

void anland_scene_legacy_output(anland_scene_legacy *b,
                                anland_device_output_t *out)
{
    if (!out)
        return;
    if (!b) {
        memset(out, 0, sizeof(*out));
        return;
    }
    if (b->have_output)
        *out = b->output;
    else
        memset(out, 0, sizeof(*out));
}

uint64_t anland_scene_legacy_frames_flipped(const anland_scene_legacy *b)
{
    return b ? b->frames_flipped : 0;
}

uint64_t anland_scene_legacy_frames_acked(const anland_scene_legacy *b)
{
    return b ? b->frames_acked : 0;
}

uint64_t anland_scene_legacy_reconnects(const anland_scene_legacy *b)
{
    return b ? b->reconnects : 0;
}