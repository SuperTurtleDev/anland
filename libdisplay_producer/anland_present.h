/*
 * anland_present.h -- swappable presentation backend boundary.
 *
 * anland_scene owns layer transactions, buffer accounting and event delivery.
 * anland_present owns the connection from that scene to a concrete presentation
 * system. The legacy implementation wraps anland_scene_legacy; a future AWL
 * implementation will expose the same lifecycle without leaking AWL internals
 * into KWin, Mutter or another future DE.
 */
#ifndef ANLAND_PRESENT_H
#define ANLAND_PRESENT_H

#include <stdbool.h>

#include "anland_device.h"
#include "anland_scene.h"
#include "anland_buffer_registry.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct anland_present anland_present;

typedef enum anland_present_backend {
    ANLAND_PRESENT_BACKEND_LEGACY = 0,
    ANLAND_PRESENT_BACKEND_AWL = 1,
} anland_present_backend_t;

typedef struct anland_present_config {
    anland_present_backend_t backend;
    /* Backend-specific endpoint. For legacy this is the daemon socket hint. */
    const char *endpoint;
    /* AWL runtime directory. Must be NULL for legacy. */
    const char *runtime_dir;
} anland_present_config_t;

/* Parse explicit DE selection. Unknown values fail; there is no implicit
 * fallback from a requested AWL backend to legacy. */
int anland_present_config_from_environment(anland_present_config_t *out,
                                            const char *legacy_endpoint,
                                            const char *awl_runtime_dir);

/* Create a presentation backend. AWL requires a running presentation service;
 * it returns NULL when that service cannot be reached. There is no implicit
 * fallback to legacy, so callers cannot mistake fullscreen output for AWL. */
anland_present *anland_present_create(const anland_present_config_t *config);
void anland_present_destroy(anland_present *present);

anland_present_backend_t anland_present_backend(const anland_present *present);
const char *anland_present_backend_name(anland_present_backend_t backend);

/* Objects are owned by the presentation backend and remain stable until destroy.
 * DE-specific code may use device-only facilities such as input or audio, while
 * all frame transactions go through the scene contract. */
anland_scene *anland_present_scene(anland_present *present);
int anland_present_get_buffer(const anland_present *present,
                              uint64_t buffer_id,
                              anland_buffer_desc_t *out);
int anland_present_dup_buffer_fd(const anland_present *present,
                                 uint64_t buffer_id);
int anland_present_lookup_window_layer(const anland_present *present,
                                       uint64_t window_id,
                                       uint64_t *out_layer_id);
/* DE-side AWL window lifecycle. Legacy backends reject these operations. */
int anland_present_window_create(anland_present *present,
                                 uint64_t window_id,
                                 const anland_layer_desc_t *desc,
                                 uint64_t *out_layer_id);
int anland_present_window_update(anland_present *present,
                                 uint64_t window_id,
                                 const anland_layer_desc_t *desc);
int anland_present_window_destroy(anland_present *present,
                                  uint64_t window_id);
anland_device *anland_present_device(anland_present *present);
bool anland_present_connected(const anland_present *present);

/* Transport lifecycle. These preserve scene and layer identity across a consumer
 * reconnect. reopen is for a dead/restarted daemon; reconnect is for consumer
 * fallback while the daemon connection remains alive. */
int anland_present_reconnect(anland_present *present);
int anland_present_reopen(anland_present *present, const char *endpoint);
void anland_present_drop_session(anland_present *present);

/* Submit an already accepted scene transaction and service backend events. */
int anland_present_present(anland_present *present);
int anland_present_pump(anland_present *present, int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* ANLAND_PRESENT_H */