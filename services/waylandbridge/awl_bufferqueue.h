/* awl_bufferqueue.h — per-surface buffer ring queue: the mediator between
 * the protocol dispatch thread (producer: commits) and the window's render
 * thread (consumer: frames).
 *
 * Pure Linux (dma-buf fd + sync_file fd + plain integers): no libwayland, no
 * Android objects — the logic layer feeds it, the adaptation layer drains it,
 * neither side sees the other's types. This is the seam a hardware-composer
 * backend plugs into later (it consumes the same elements).
 *
 * Model (mailbox + per-element references):
 *   push    lock-free, any thread. Fails when the ring is full (the caller
 *           must hand the buffer straight back to its owner).
 *   drain   caller holds the head lock. Pops the head while the element
 *           behind it is already complete (its acquire fence signaled —
 *           completion is assumed FIFO: an element is complete only if every
 *           earlier one is). Afterwards the head is the newest complete
 *           element (or the oldest incomplete one when none is complete yet).
 *           A popped element drops the ring's reference; it goes back to the
 *           producer (release callback) when the last reference is gone.
 *   gethead caller holds the head lock. Waits (bounded) for the head's
 *           acquire fence, takes a reference on it and returns it — the
 *           caller may UNLOCK IMMEDIATELY: the element stays valid (and its
 *           dma-buf open) until the caller's awl_bufferqueue_put, however far
 *           the ring moves meanwhile. The lock is held for microseconds; a
 *           commit-time drain almost never misses.
 *   put     any thread, no lock: the consumer is done with an element it
 *           got from gethead. fence_fd (sync_file, may be -1) = when the
 *           consumer's reads of that memory are finished — merged into the
 *           element's release fence, handed to the producer when the last
 *           reference drops (explicit-sync fenced_release / implicit-sync
 *           read fence in the dma-buf reservation).
 *   arm     caller holds the lock, after drain: when the frame behind the
 *           head is still incomplete, a shared fence-waiter thread drains
 *           the queue the moment its acquire fence signals — a producer
 *           parked on buffer starvation is unblocked when its own GPU work
 *           finishes, not at the next commit or the next vsync.
 *   lock/trylock/unlock  head lock. The render thread holds it across
 *           drain+gethead only; the dispatch thread trylocks at commit for
 *           an opportunistic drain and passes when it is busy.
 *
 * Elements own their fds: the queue closes dmabuf/acquire/release fds once the
 * release callback returned. `user` is an opaque producer cookie handed to the
 * callback (the logic layer keeps its wl_buffer/release-object bookkeeping
 * there). A dmabuf_fd of -1 is a NULL-buffer marker ("no dmabuf frame from
 * here on": wl_surface.attach(NULL) commit, or a wl_shm commit following
 * dmabuf ones): always "complete", the consumer shows no dmabuf for that
 * layer and asks the logic layer for shm content instead (awl.h
 * awl_surface_shm_begin). wl_shm frames never enter a queue.
 *
 * Lifetime: refcounted. The owning surface holds one reference; every live
 * element holds one (so the queue outlives its owner while a consumer still
 * holds a frame); a consumer resolving the queue takes its own for the span
 * it touches the ring. */
#ifndef AWL_BUFFERQUEUE_H
#define AWL_BUFFERQUEUE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct awl_bq_buffer {
    int dmabuf_fd;         /* owned; -1 = NULL-buffer marker */
    int acquire_fd;        /* sync_file the producer's writes signal; owned;
                            * -1 = none → implicit: the dma-buf's own write
                            * fences (poll POLLIN) gate readiness */
    int release_fd;        /* merged consumer fences (awl_bufferqueue_put);
                            * valid inside the release callback only */
    uint64_t ino;          /* dma-buf inode: render-side identity (0 = unknown) */
    uint64_t modifier;     /* DRM format modifier; CPU readers must require LINEAR */
    uint32_t width, height, stride;   /* stride in bytes */
    uint32_t format;       /* DRM fourcc */
    uint64_t seq;          /* process-wide frame sequence, stamped by push:
                            * identity of a frame that survives the consumer's
                            * put (an element pointer may be recycled) */
    void* user;            /* producer cookie (opaque to the queue) */
};

struct awl_bufferqueue;

/* Called once per element, when its last reference drops (ring popped it
 * AND every consumer put it back), on whichever thread dropped it. The head
 * lock may be held by that thread — the callback must never touch the same
 * queue. fds are still open during the callback and closed by the queue
 * right after; the callee owns `user`. */
typedef void (*awl_bq_release_fn)(struct awl_bq_buffer* e, void* ctx);

struct awl_bufferqueue* awl_bufferqueue_create(awl_bq_release_fn fn, void* ctx);
void awl_bufferqueue_ref(struct awl_bufferqueue* q);
void awl_bufferqueue_unref(struct awl_bufferqueue* q);

/* Lock-free. 1 = queued (the queue now owns the fds + user), 0 = full
 * (caller keeps everything and must return the buffer itself). */
int awl_bufferqueue_push(struct awl_bufferqueue* q, const struct awl_bq_buffer* e);

int  awl_bufferqueue_trylock(struct awl_bufferqueue* q);   /* 1 = locked */
void awl_bufferqueue_lock(struct awl_bufferqueue* q);
void awl_bufferqueue_unlock(struct awl_bufferqueue* q);

/* Caller holds the lock. Returns the number of elements popped. */
int awl_bufferqueue_drain(struct awl_bufferqueue* q);
/* Caller holds the lock. Waits up to timeout_ms for the head's acquire fence,
 * then BLOCKS until it signals (the timeout only tunes the rate-limited
 * warning) — the returned head is always a complete frame (dead fds end the
 * poll immediately; only a real wait error gives up). NULL = empty. The
 * returned element is referenced: pair with awl_bufferqueue_put. */
struct awl_bq_buffer* awl_bufferqueue_gethead(struct awl_bufferqueue* q, int timeout_ms);
/* Caller holds the lock. gethead without the wait: NULL when the ring is
 * empty OR the head's acquire fence has not signaled yet — the consumer keeps
 * presenting what it already holds and re-checks at its own cadence. For a
 * consumer that serves every window from one thread (the SurfaceControl
 * compositor's vsync loop) a blocking head would stall every other window's
 * frames behind one client's GPU. Referenced like gethead: pair with put. */
struct awl_bq_buffer* awl_bufferqueue_tryhead(struct awl_bufferqueue* q);
/* Any thread, no lock. fence_fd is dup'd/merged (caller keeps its fd). */
void awl_bufferqueue_put(struct awl_bq_buffer* e, int fence_fd);
/* Caller holds the lock, after drain. Readiness fd (own dup, CLOEXEC) of the
 * oldest incomplete frame — the head tryhead refused, or the frame behind a
 * complete head — for a consumer that wants to be woken when a frame it
 * could not latch becomes latchable (poll POLLIN; POLLERR/HUP = dead fence,
 * treat as signaled). -1 = every published frame is complete. */
int awl_bufferqueue_incomplete_fd(struct awl_bufferqueue* q);
/* Caller holds the lock, after drain. No-op when nothing incomplete is
 * pending or a watch is already registered. */
void awl_bufferqueue_arm(struct awl_bufferqueue* q);
/* Caller holds the lock. Pop everything (surface teardown). Elements a
 * consumer still holds are released by its put. */
void awl_bufferqueue_flush(struct awl_bufferqueue* q);

/* Lock-free snapshots (any thread; advisory). */
int awl_bufferqueue_count(struct awl_bufferqueue* q);     /* elements incl. head */
int awl_bufferqueue_pending(struct awl_bufferqueue* q);   /* elements behind the head */
/* Frames drain has popped without their ever being the presented head,
 * monotonic since creation (wraps). A consumer diffing it per frame sees
 * how many frames the client discarded in the interval: 0 = the client is
 * paced to the consumer (frame callbacks / FIFO), ≥1 sustained = it runs
 * ahead (mailbox / immediate) — a pacing hint, never a correctness input. */
unsigned awl_bufferqueue_superseded(struct awl_bufferqueue* q);

/* ---- sync helpers (Linux dma-buf / sync_file ioctls; no Android) ----
 * fence_signaled: 1 = signaled (or fd < 0), 0 = pending, <0 = error.
 * fence_wait:     0 = signaled, 1 = timeout, <0 = error.
 * dmabuf_export_sync_file: sync_file of the dma-buf's current write fences
 *   (what a reader must wait for); -1 = unsupported/failed (caller falls back
 *   to polling the dma-buf itself).
 * dmabuf_import_sync_file: add a fence to the dma-buf's reservation (read
 *   usage when read=1): an implicit-sync writer then waits for it.
 * fence_merge: sync_file merge of a and b (either may be -1); returns a new
 *   fd (caller owns) or -1. Inputs are NOT closed. */
int awl_fence_signaled(int fd);
int awl_fence_wait(int fd, int timeout_ms);
int awl_dmabuf_export_sync_file(int dmabuf_fd);
int awl_dmabuf_import_sync_file(int dmabuf_fd, int fence_fd, int read);
int awl_fence_merge(int a, int b);
int awl_fence_is_sync_file(int fd);   /* 1 = a sync_file (SYNC_IOC_FILE_INFO answers) */

#ifdef __cplusplus
}
#endif
#endif
