/*
 * anland_scene_legacy.h — anland_scene backend over the LEGACY fullscreen transport.
 *
 * Bridges the layer/commit contract (anland_scene.h) onto the existing
 * anland_device fullscreen path, so a DE gets honest submit / presented /
 * released accounting on top of the transport that actually exists.
 *
 * ============================ SCOPE / LIMITS ============================
 * This adapter FLATTENS the scene. The legacy transport has exactly one output
 * fed by a rotating set of consumer-owned dmabufs; it has no notion of a window.
 * The DE composites its own layers into the output framebuffer (it already does)
 * and this adapter performs the flip.
 *
 * This is the INTENDED shape for the product, not a stopgap: one container hosts
 * one DE on one Android Surface, so the consumer needs exactly one output and
 * there is no per-window Surface to hand it.
 *
 * Therefore:
 *   - layer placement, z-order, opacity and damage are NOT transmitted to the
 *     consumer; they are validated and recorded by anland_scene only;
 *   - per-window Android Surfaces are OUT OF SCOPE for this adapter and for the
 *     product: the scene is a single full-output layer in practice, and no
 *     per-window Surface behaviour may be built on top of this adapter;
 *   - buffer_id is bookkeeping: it identifies the producer-side buffer the DE
 *     rendered into, and is what gets released. It is not an Android Surface id.
 *
 * What it DOES give the DE:
 *   - one stable contract (submit / presented / released) that every DE adapter
 *     speaks, so the presentation backend underneath can be swapped (AWL, a
 *     future virtual DRM device) without touching any DE adapter;
 *   - honest frame accounting: submit, flip and consumer acknowledgement are
 *     three distinct events, instead of one pageflip() call that claims success
 *     before the consumer has done anything;
 *   - reconnect handling that drops in-flight work and releases buffers.
 *
 * One frame may be in flight at a time (the transport's own back-pressure). A
 * submit while a frame is outstanding is rejected, which surfaces as
 * COMMIT_DROPPED(BACKEND_ERROR) to the DE.
 *
 * GPU FENCES
 *   The commit's acquire_fence_fd is forwarded to the consumer so SurfaceFlinger
 *   can wait GPU-side. Because the scene is flattened, every layer is composited
 *   into the SAME output buffer, so the adapter takes the first non-negative
 *   acquire fence it finds; attach the output buffer's fence to any one layer.
 *
 *   Do NOT also call anland_device_set_render_fence() directly while using this
 *   adapter: present() passes its own fence to the flip, which replaces whatever
 *   was stashed and would silently discard the direct one.
 * ========================================================================
 */
#ifndef ANLAND_SCENE_LEGACY_H
#define ANLAND_SCENE_LEGACY_H

#include "anland_device.h"
#include "anland_scene.h"

#ifdef __cplusplus
extern "C" {
#endif

struct anland_scene_legacy;
typedef struct anland_scene_legacy anland_scene_legacy;

/* Open the daemon connection (socket_path is a hint; NULL/"" probes the
 * well-known locations) and create the bound scene. Returns NULL on failure. */
anland_scene_legacy *anland_scene_legacy_create(const char *socket_path);

void anland_scene_legacy_destroy(anland_scene_legacy *b);

/* The commit contract this adapter presents. Owned by the adapter. */
anland_scene *anland_scene_legacy_scene(anland_scene_legacy *b);

/* The underlying device, for the DE-specific features the commit contract does
 * not model yet (input polling, clipboard, audio/camera resource requests).
 * Owned by the adapter. */
anland_device *anland_scene_legacy_device(anland_scene_legacy *b);

/* True once a consumer session is up. */
bool anland_scene_legacy_connected(anland_scene_legacy *b);

/* Attempt to leave fallback; returns 0 when connected. Safe to call repeatedly
 * (this is what a DE reconnect timer drives). On a successful transition any
 * in-flight commit is invalidated, OUTPUT_CHANGED is reported, and the first
 * RENDER_TARGET_READY is published so the DE can start its frame loop.
 * Returns -1 when the session cannot be used; the caller retries. */
int anland_scene_legacy_reconnect(anland_scene_legacy *b);

/* Replace a dead/restarted daemon connection without replacing the scene or its
 * layers. Pending and in-flight work is invalidated before the old connection is
 * discarded, so no stale fence or commit can survive into the new session. */
int anland_scene_legacy_reopen(anland_scene_legacy *b, const char *socket_path);

/* Abandon the current consumer session on purpose: drop any accepted / in-flight
 * frame (the DE receives COMMIT_DROPPED + BUFFER_RELEASED for its buffers) and
 * detach from the consumer, so the next anland_scene_legacy_reconnect() runs a
 * fresh handshake. Use it when the session itself is unusable (a dmabuf/EGL
 * import failure), NOT for ordinary fallback — the adapter detects that itself.
 * The daemon connection is kept. Idempotent. */
void anland_scene_legacy_drop_session(anland_scene_legacy *b);

/* Hand the accepted commit to the consumer: selects the buffer the consumer
 * asked for, commits it and flips. Returns 0 when the frame was handed over.
 *
 * This does NOT mean the frame reached the display — that is reported as
 * PRESENTED through the scene once the consumer acknowledges it. Returns -1 when
 * there is no accepted commit or the transport is in fallback. */
int anland_scene_legacy_present(anland_scene_legacy *b);

/* Service the transport for the CURRENT session: turn the consumer's
 * buffer-ready signal into PRESENTED for the frame in flight, then publish the
 * next RENDER_TARGET_READY. timeout_ms < 0 blocks until something happens; 0
 * polls.
 *
 * It does NOT reconnect. When the session is gone the in-flight work is dropped
 * and the call returns 0; the DE re-establishes the session with
 * anland_scene_legacy_reconnect() so it can re-import the new dmabuf set and
 * rebuild its event sources before rendering resumes. Returns -1 when the
 * current session proved unusable (the caller must reconnect).
 *
 * The DE then drains the scene with anland_scene_dispatch(). */
int anland_scene_legacy_pump(anland_scene_legacy *b, int timeout_ms);

/* Last output geometry seen by the adapter (zeroed before the first connect). */
void anland_scene_legacy_output(anland_scene_legacy *b,
                                anland_device_output_t *out);

/* ---- diagnostics ---- */
uint64_t anland_scene_legacy_frames_flipped(const anland_scene_legacy *b);
uint64_t anland_scene_legacy_frames_acked(const anland_scene_legacy *b);
uint64_t anland_scene_legacy_reconnects(const anland_scene_legacy *b);

#ifdef __cplusplus
}
#endif

#endif /* ANLAND_SCENE_LEGACY_H */