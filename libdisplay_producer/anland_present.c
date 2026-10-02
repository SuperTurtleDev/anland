#include "anland_present_internal.h"
#include <stdlib.h>
#include <string.h>

int anland_present_config_from_environment(anland_present_config_t *out,
                                            const char *legacy_endpoint,
                                            const char *awl_runtime_dir)
{
    if (!out)
        return -1;
    const char *mode = getenv("ANLAND_PRESENT_BACKEND");
    if (!mode || !*mode || strcmp(mode, "legacy") == 0) {
        out->backend = ANLAND_PRESENT_BACKEND_LEGACY;
        out->endpoint = legacy_endpoint;
        out->runtime_dir = NULL;
        return 0;
    }
    if (strcmp(mode, "awl") == 0) {
        if (!awl_runtime_dir || !*awl_runtime_dir)
            return -1;
        out->backend = ANLAND_PRESENT_BACKEND_AWL;
        out->endpoint = NULL;
        out->runtime_dir = awl_runtime_dir;
        return 0;
    }
    return -1;
}

anland_present *anland_present_create(const anland_present_config_t *config)
{
    if (!config)
        return NULL;

    switch (config->backend) {
    case ANLAND_PRESENT_BACKEND_LEGACY:
        return anland_present_legacy_create(config);
    case ANLAND_PRESENT_BACKEND_AWL:
        return anland_present_awl_create(config);
    default:
        return NULL;
    }
}

void anland_present_destroy(anland_present *present)
{
    if (present && present->ops && present->ops->destroy)
        present->ops->destroy(present);
}

anland_present_backend_t anland_present_backend(const anland_present *present)
{
    return present ? present->backend : ANLAND_PRESENT_BACKEND_LEGACY;
}

const char *anland_present_backend_name(anland_present_backend_t backend)
{
    switch (backend) {
    case ANLAND_PRESENT_BACKEND_LEGACY:
        return "legacy";
    case ANLAND_PRESENT_BACKEND_AWL:
        return "awl";
    default:
        return "unknown";
    }
}

anland_scene *anland_present_scene(anland_present *present)
{
    return present && present->ops && present->ops->scene ?
        present->ops->scene(present) : NULL;
}

int anland_present_get_buffer(const anland_present *present,
                              uint64_t buffer_id,
                              anland_buffer_desc_t *out)
{
    return present && present->ops && present->ops->get_buffer ?
        present->ops->get_buffer(present, buffer_id, out) : -1;
}
int anland_present_dup_buffer_fd(const anland_present *present,
                                 uint64_t buffer_id)
{
    return present && present->ops && present->ops->dup_buffer_fd ?
        present->ops->dup_buffer_fd(present, buffer_id) : -1;
}
int anland_present_lookup_window_layer(const anland_present *present,
                                       uint64_t window_id,
                                       uint64_t *out_layer_id)
{
    return present && present->ops && present->ops->lookup_window_layer ?
        present->ops->lookup_window_layer(present, window_id, out_layer_id) : -1;
}
int anland_present_window_create(anland_present *present,
                                 uint64_t window_id,
                                 const anland_layer_desc_t *desc,
                                 uint64_t *out_layer_id)
{
    return present && present->ops && present->ops->window_create ?
        present->ops->window_create(present, window_id, desc, out_layer_id) : -1;
}
int anland_present_window_update(anland_present *present,
                                 uint64_t window_id,
                                 const anland_layer_desc_t *desc)
{
    return present && present->ops && present->ops->window_update ?
        present->ops->window_update(present, window_id, desc) : -1;
}
int anland_present_window_destroy(anland_present *present, uint64_t window_id)
{
    return present && present->ops && present->ops->window_destroy ?
        present->ops->window_destroy(present, window_id) : -1;
}
anland_device *anland_present_device(anland_present *present)
{
    return present && present->ops && present->ops->device ?
        present->ops->device(present) : NULL;
}

bool anland_present_connected(const anland_present *present)
{
    return present && present->ops && present->ops->connected &&
        present->ops->connected(present);
}

int anland_present_reconnect(anland_present *present)
{
    return present && present->ops && present->ops->reconnect ?
        present->ops->reconnect(present) : -1;
}

int anland_present_reopen(anland_present *present, const char *endpoint)
{
    return present && present->ops && present->ops->reopen ?
        present->ops->reopen(present, endpoint) : -1;
}

void anland_present_drop_session(anland_present *present)
{
    if (present && present->ops && present->ops->drop_session)
        present->ops->drop_session(present);
}

int anland_present_present(anland_present *present)
{
    return present && present->ops && present->ops->present ?
        present->ops->present(present) : -1;
}

int anland_present_pump(anland_present *present, int timeout_ms)
{
    return present && present->ops && present->ops->pump ?
        present->ops->pump(present, timeout_ms) : -1;
}