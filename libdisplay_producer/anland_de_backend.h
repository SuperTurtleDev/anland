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

typedef struct anland_de_backend_buffer {
    anland_buffer_desc_t desc;
    int fd;
    void *native_image;
} anland_de_backend_buffer_t;

typedef struct anland_de_backend_ops {
    int (*import_buffer)(void *userdata,
                         const anland_buffer_desc_t *desc,
                         int fd,
                         anland_de_backend_buffer_t *out);
    void (*release_buffer)(void *userdata,
                           anland_de_backend_buffer_t *buffer);
    void *userdata;
} anland_de_backend_ops_t;

typedef struct anland_de_backend_config {
    anland_present_config_t present;
    const char *name;
    anland_de_backend_ops_t ops;
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

/* Complete current output description for a live session. Legacy returns the
 * device description; AWL does not advertise an output until it has one. */
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
/* Return the layer created by the AWL channel for an already announced window.
 * AWL owns WINDOW_CREATE/WINDOW_DESTROY layer lifetime; callers must not create
 * a second layer for the same window. */
int anland_de_backend_lookup_window_layer(const anland_de_backend *backend,
                                          uint64_t window_id,
                                          uint64_t *out_layer_id);

int anland_de_backend_commit(anland_de_backend *backend,
                             const anland_layer_state_t *states,
                             size_t count,
                             uint64_t *out_commit_id);
int anland_de_backend_present(anland_de_backend *backend);

/* Dispatch scene events and retire imported native buffers only after the scene
 * reports PRESENTED, BUFFER_RELEASED, or COMMIT_DROPPED. */
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