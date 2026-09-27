#ifndef ANLAND_PRESENT_INTERNAL_H
#define ANLAND_PRESENT_INTERNAL_H

#include "anland_present.h"

typedef struct anland_present_ops {
    void (*destroy)(anland_present *present);
    anland_scene *(*scene)(anland_present *present);
    int (*lookup_window_layer)(const anland_present *present,
                               uint64_t window_id,
                               uint64_t *out_layer_id);
    int (*window_create)(anland_present *present,
                         uint64_t window_id,
                         const anland_layer_desc_t *desc,
                         uint64_t *out_layer_id);
    int (*window_update)(anland_present *present,
                         uint64_t window_id,
                         const anland_layer_desc_t *desc);
    int (*window_destroy)(anland_present *present,
                          uint64_t window_id);
    int (*get_buffer)(const anland_present *present,
                      uint64_t buffer_id,
                      anland_buffer_desc_t *out);
    int (*dup_buffer_fd)(const anland_present *present,
                         uint64_t buffer_id);
    anland_device *(*device)(anland_present *present);
    bool (*connected)(const anland_present *present);
    int (*reconnect)(anland_present *present);
    int (*reopen)(anland_present *present, const char *endpoint);
    void (*drop_session)(anland_present *present);
    int (*present)(anland_present *present);
    int (*pump)(anland_present *present, int timeout_ms);
} anland_present_ops_t;

struct anland_present {
    anland_present_backend_t backend;
    const anland_present_ops_t *ops;
};

anland_present *anland_present_legacy_create(const anland_present_config_t *config);
anland_present *anland_present_awl_create(const anland_present_config_t *config);

#endif /* ANLAND_PRESENT_INTERNAL_H */