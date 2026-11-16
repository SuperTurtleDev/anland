#ifndef ANLAND_DE_BACKEND_H
#define ANLAND_DE_BACKEND_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include "anland_present.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct anland_de_backend anland_de_backend;

/* WM-specific native image imports stay in the renderer. The shared device
 * owns the consumer buffer set and exposes caller-owned duplicate fds. */

typedef struct anland_de_backend_config {
    anland_present_config_t present;
    const char *name;
} anland_de_backend_config_t;

/* The buffer the DE should render into next, as published by the presentation
 * backend. This is what a DE's frame loop waits for: it is a RENDER TARGET
 * signal, never a completion signal (see ANLAND_SCENE_EVENT_RENDER_TARGET_READY).
 *
 * `generation` identifies the consumer session. A DE that sees a generation other
 * than the one it is currently serving must ignore the target: the slot index
 * belongs to a buffer set it has not imported. */
typedef struct anland_de_target {
    uint64_t generation;
    uint32_t index;      /* slot in the producer-side buffer set */
    uint32_t count;      /* size of that set (index < count) */
    uint32_t width;
    uint32_t height;
    uint32_t format;
    uint64_t modifier;
} anland_de_target_t;

/* Current render target. Returns 0 only for a published target of the live
 * session, -1 if disconnected, stale or not yet announced. A session drop
 * invalidates a cached target immediately, even before events are dispatched.
 * The DE never needs to know how the backend learned about it. */
int anland_de_backend_get_target(const anland_de_backend *backend,
                                 anland_de_target_t *out);

/* Writable target for event-driven renderers. Unlike get_target(), refuses an
 * accepted/inflight frame, retained slot, release retry, or a cached target that
 * no longer matches the device's slot. Returns 0/-1; clears *out on failure.
 * Serialized with commit/present/pump by the event-loop caller. */
int anland_de_backend_get_writable_target(const anland_de_backend *backend,
                                          anland_de_target_t *out);

/* Complete current device output description for a live session. */
int anland_de_backend_get_output(const anland_de_backend *backend,
                                 anland_device_output_t *out);

/* dup'ed dmabuf fd for buffer slot `index`, or -1. Caller owns the fd and must
 * close it. The mapping from a slot to a real buffer handle (PRIME fd, GBM
 * buffer, drmModeGetFB2) is the presentation backend's business, not the DE's. */
int anland_de_backend_buffer_fd(anland_de_backend *backend, uint32_t index);

anland_de_backend *anland_de_backend_create(const anland_de_backend_config_t *config);
void anland_de_backend_destroy(anland_de_backend *backend);

int anland_de_backend_add_window(anland_de_backend *backend,
                                 uint64_t window_id,
                                 const char *name,
                                 uint64_t *out_layer_id);
/* Create a window with its complete initial display metadata. */
int anland_de_backend_add_window_desc(anland_de_backend *backend,
                                      uint64_t window_id,
                                      const anland_layer_desc_t *desc,
                                      uint64_t *out_layer_id);
int anland_de_backend_remove_window(anland_de_backend *backend,
                                    uint64_t window_id);
/* Update mutable geometry/display metadata while preserving the layer identity. */
int anland_de_backend_update_window(anland_de_backend *backend,
                                    uint64_t window_id,
                                    const anland_layer_desc_t *desc);
/* Look up the producer-local layer for a WM window. No wire window messages
 * or Android per-window Surface are involved. */
int anland_de_backend_lookup_window_layer(const anland_de_backend *backend,
                                          uint64_t window_id,
                                          uint64_t *out_layer_id);

int anland_de_backend_commit(anland_de_backend *backend,
                             const anland_layer_state_t *states,
                             size_t count,
                             uint64_t *out_commit_id);
int anland_de_backend_present(anland_de_backend *backend);

/* Dispatch scene events unchanged. WMs retain their native framebuffer set
 * for the session; BUFFER_RELEASED permits reuse, not framebuffer destruction.
 * Release-fence fds in returned events are owned by the caller. */
int anland_de_backend_dispatch(anland_de_backend *backend,
                               anland_scene_event_t *events,
                               size_t capacity,
                               size_t *out_count);
int anland_de_backend_pump(anland_de_backend *backend, int timeout_ms);
int anland_de_backend_reconnect(anland_de_backend *backend);
int anland_de_backend_reopen(anland_de_backend *backend, const char *endpoint);
void anland_de_backend_drop_session(anland_de_backend *backend);
/* Borrowed handles owned by the presentation backend. They remain valid until
 * anland_de_backend_destroy(); DE adapters must not destroy them directly. */
anland_scene *anland_de_backend_scene(anland_de_backend *backend);
anland_device *anland_de_backend_device(anland_de_backend *backend);
anland_present *anland_de_backend_present_object(anland_de_backend *backend);

#ifdef __cplusplus
}
#endif

#endif