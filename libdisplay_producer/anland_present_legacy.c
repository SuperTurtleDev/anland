#include "anland_present_internal.h"
#include "anland_scene_legacy.h"

#include <stdlib.h>

typedef struct anland_present_legacy {
    anland_present base;
    anland_scene_legacy *legacy;
} anland_present_legacy_t;

static anland_present_legacy_t *legacy_cast(anland_present *present)
{
    return (anland_present_legacy_t *)present;
}

static const anland_present_legacy_t *legacy_const_cast(const anland_present *present)
{
    return (const anland_present_legacy_t *)present;
}

static void legacy_destroy(anland_present *present)
{
    anland_present_legacy_t *legacy = legacy_cast(present);
    anland_scene_legacy_destroy(legacy->legacy);
    free(legacy);
}

static anland_scene *legacy_scene(anland_present *present)
{
    return anland_scene_legacy_scene(legacy_cast(present)->legacy);
}
static int legacy_window_create(anland_present *present,
                                 uint64_t window_id,
                                 const anland_layer_desc_t *desc,
                                 uint64_t *out_layer_id)
{
    (void)present;
    (void)window_id;
    (void)desc;
    (void)out_layer_id;
    return -1;
}
static int legacy_window_update(anland_present *present,
                                 uint64_t window_id,
                                 const anland_layer_desc_t *desc)
{
    (void)present;
    (void)window_id;
    (void)desc;
    return -1;
}
static int legacy_window_destroy(anland_present *present, uint64_t window_id)
{
    (void)present;
    (void)window_id;
    return -1;
}
static int legacy_lookup_window_layer(const anland_present *present,
                                      uint64_t window_id,
                                      uint64_t *out_layer_id)
{
    (void)present;
    (void)window_id;
    (void)out_layer_id;
    return -1;
}
static int legacy_get_buffer(const anland_present *present,
                             uint64_t buffer_id,
                             anland_buffer_desc_t *out)
{
    (void)present;
    (void)buffer_id;
    (void)out;
    return -1;
}
static int legacy_dup_buffer_fd(const anland_present *present, uint64_t buffer_id)
{
    (void)present;
    (void)buffer_id;
    return -1;
}
static anland_device *legacy_device(anland_present *present)
{
    return anland_scene_legacy_device(legacy_cast(present)->legacy);
}

static bool legacy_connected(const anland_present *present)
{
    anland_present_legacy_t *legacy =
        (anland_present_legacy_t *)legacy_const_cast(present);
    return anland_scene_legacy_connected(legacy->legacy);
}

static int legacy_reconnect(anland_present *present)
{
    return anland_scene_legacy_reconnect(legacy_cast(present)->legacy);
}

static int legacy_reopen(anland_present *present, const char *endpoint)
{
    return anland_scene_legacy_reopen(legacy_cast(present)->legacy, endpoint);
}

static void legacy_drop_session(anland_present *present)
{
    anland_scene_legacy_drop_session(legacy_cast(present)->legacy);
}

static int legacy_present(anland_present *present)
{
    return anland_scene_legacy_present(legacy_cast(present)->legacy);
}

static int legacy_pump(anland_present *present, int timeout_ms)
{
    return anland_scene_legacy_pump(legacy_cast(present)->legacy, timeout_ms);
}

static const anland_present_ops_t s_legacy_ops = {
    .destroy = legacy_destroy,
    .scene = legacy_scene,
    .lookup_window_layer = legacy_lookup_window_layer,
    .window_create = legacy_window_create,
    .window_update = legacy_window_update,
    .window_destroy = legacy_window_destroy,
    .get_buffer = legacy_get_buffer,
    .dup_buffer_fd = legacy_dup_buffer_fd,
    .device = legacy_device,
    .connected = legacy_connected,
    .reconnect = legacy_reconnect,
    .reopen = legacy_reopen,
    .drop_session = legacy_drop_session,
    .present = legacy_present,
    .pump = legacy_pump,
};

anland_present *anland_present_legacy_create(const anland_present_config_t *config)
{
    if (!config || config->runtime_dir)
        return NULL;

    anland_scene_legacy *scene = anland_scene_legacy_create(config->endpoint);
    if (!scene)
        return NULL;

    anland_present_legacy_t *legacy = calloc(1, sizeof(*legacy));
    if (!legacy) {
        anland_scene_legacy_destroy(scene);
        return NULL;
    }

    legacy->base.backend = ANLAND_PRESENT_BACKEND_LEGACY;
    legacy->base.ops = &s_legacy_ops;
    legacy->legacy = scene;
    return &legacy->base;
}