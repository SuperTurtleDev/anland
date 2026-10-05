/*
 * anland_window_map.h -- stable DE layer to presentation-window mapping.
 *
 * Layer ids belong to anland_scene; window ids belong to the WM adapter.
 * This producer-local map is independent of any WM or wire protocol.
 */
#ifndef ANLAND_WINDOW_MAP_H
#define ANLAND_WINDOW_MAP_H

#include <stddef.h>
#include <stdint.h>

#include "anland_scene.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct anland_window_map anland_window_map;

anland_window_map *anland_window_map_create(void);
void anland_window_map_destroy(anland_window_map *map);

/* Bind a live scene layer to a backend window. Rebinding either id is rejected;
 * callers must unbind first so stale backend windows cannot be reused silently. */
int anland_window_map_bind(anland_window_map *map,
                           anland_layer_id layer_id,
                           uint64_t window_id);
int anland_window_map_unbind_layer(anland_window_map *map,
                                   anland_layer_id layer_id,
                                   uint64_t *out_window_id);
int anland_window_map_unbind_window(anland_window_map *map,
                                    uint64_t window_id,
                                    anland_layer_id *out_layer_id);

int anland_window_map_lookup_layer(const anland_window_map *map,
                                   anland_layer_id layer_id,
                                   uint64_t *out_window_id);
int anland_window_map_lookup_window(const anland_window_map *map,
                                    uint64_t window_id,
                                    anland_layer_id *out_layer_id);

size_t anland_window_map_count(const anland_window_map *map);
void anland_window_map_clear(anland_window_map *map);

#ifdef __cplusplus
}
#endif

#endif /* ANLAND_WINDOW_MAP_H */