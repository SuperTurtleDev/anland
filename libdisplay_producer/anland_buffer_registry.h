#ifndef ANLAND_BUFFER_REGISTRY_H
#define ANLAND_BUFFER_REGISTRY_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct anland_buffer_registry anland_buffer_registry;

typedef struct anland_buffer_desc {
    uint64_t buffer_id;
    uint64_t window_id;
    uint64_t frame_id;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
    uint64_t modifier;
    uint32_t offset;
    int fd;
} anland_buffer_desc_t;

anland_buffer_registry *anland_buffer_registry_create(void);
void anland_buffer_registry_destroy(anland_buffer_registry *registry);

/* Takes ownership of desc->fd on success and on replacement. */
int anland_buffer_registry_put(anland_buffer_registry *registry,
                               const anland_buffer_desc_t *desc);
int anland_buffer_registry_take(anland_buffer_registry *registry,
                                uint64_t buffer_id,
                                anland_buffer_desc_t *out);
int anland_buffer_registry_remove(anland_buffer_registry *registry,
                                  uint64_t buffer_id);
int anland_buffer_registry_contains(const anland_buffer_registry *registry,
                                    uint64_t buffer_id);
int anland_buffer_registry_frame_id(const anland_buffer_registry *registry,
                                    uint64_t buffer_id, uint64_t *out_frame_id);
int anland_buffer_registry_get(const anland_buffer_registry *registry,
                               uint64_t buffer_id,
                               anland_buffer_desc_t *out);
int anland_buffer_registry_dup_fd(const anland_buffer_registry *registry,
                                  uint64_t buffer_id);
size_t anland_buffer_registry_count(const anland_buffer_registry *registry);
void anland_buffer_registry_clear(anland_buffer_registry *registry);

#ifdef __cplusplus
}
#endif

#endif