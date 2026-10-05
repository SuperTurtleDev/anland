/*
 * anland_present.h -- producer-side presentation lifecycle.
 *
 * This is an in-process C API, NOT another wire protocol. All WMs use the same
 * path: scene transactions -> device -> existing fullscreen producer transport.
 * The WM still composites the complete desktop into the consumer's output.
 * Layer identity and buffer lifetime do not imply per-window Android Surfaces.
 *
 * Future device transports belong behind this boundary, not in WM adapters.
 */
#ifndef ANLAND_PRESENT_H
#define ANLAND_PRESENT_H

#include <stdbool.h>
#include "anland_device.h"
#include "anland_scene.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct anland_present anland_present;

typedef struct anland_present_config {
    /* Daemon socket hint; NULL/empty retains anland_device's discovery rules. */
    const char *endpoint;
} anland_present_config_t;

/* Config is borrowed for this call. No service or backend selector is needed. */
anland_present *anland_present_create(const anland_present_config_t *config);
void anland_present_destroy(anland_present *present);

/* Borrowed objects remain stable until destroy, including across reconnects.
 * Frame transactions use the scene; input/audio integrations use the device. */
anland_scene *anland_present_scene(anland_present *present);
anland_device *anland_present_device(anland_present *present);
bool anland_present_connected(const anland_present *present);
/* Connection alone does not guarantee a writable target during release retry. */
bool anland_present_target_available(const anland_present *present);

/* Preserve scene/layer identity. reopen replaces a dead daemon connection;
 * reconnect picks up a consumer on the existing daemon connection. A new
 * consumer session requires the WM to re-import its framebuffer set. */
int anland_present_reconnect(anland_present *present);
int anland_present_reopen(anland_present *present, const char *endpoint);
void anland_present_drop_session(anland_present *present);

/* Handoff an accepted transaction, then process consumer acknowledgements.
 * Acceptance, presentation and buffer release remain separate scene facts. */
int anland_present_present(anland_present *present);
int anland_present_pump(anland_present *present, int timeout_ms);

#ifdef __cplusplus
}
#endif

#endif /* ANLAND_PRESENT_H */