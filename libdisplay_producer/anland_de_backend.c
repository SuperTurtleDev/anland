#include "anland_de_backend.h"
#include "anland_window_map.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
struct anland_de_backend {
    anland_present *present;
    anland_window_map *windows;
    /* Native framebuffer imports belong to each WM renderer, not another IPC. */
    char name[64];

    /* Latest render target and output geometry the presentation backend
     * published. Kept here so a DE reads one stable structure instead of
     * decoding backend-specific events (legacy: a consumer-selected slot in a
     * shared page; a future DRM backend: whatever it derives from a page-flip). */
    anland_de_target_t target;
    bool have_target;
    anland_device_output_t output;
    bool have_output;
};

/* Window/layer identity is local producer state, never consumer wire state. */

/* Published geometry/target is session state, not a property of the facade.
 * In particular, an old render target must never survive a disconnect. */
static void clear_display_cache(anland_de_backend *backend)
{
    backend->have_target = false;
    backend->have_output = false;
    memset(&backend->target, 0, sizeof(backend->target));
    memset(&backend->output, 0, sizeof(backend->output));
}

anland_de_backend *anland_de_backend_create(const anland_de_backend_config_t *config)
{
    if (!config)
        return NULL;
    anland_de_backend *backend = calloc(1, sizeof(*backend));
    if (!backend)
        return NULL;
    backend->windows = anland_window_map_create();
    backend->present = anland_present_create(&config->present);
    if (!backend->windows || !backend->present) {
        anland_present_destroy(backend->present);
        anland_window_map_destroy(backend->windows);
        free(backend);
        return NULL;
    }
    if (config->name)
        snprintf(backend->name, sizeof(backend->name), "%s", config->name);
    return backend;
}

void anland_de_backend_destroy(anland_de_backend *backend)
{
    if (!backend)
        return;
    anland_present_destroy(backend->present);
    anland_window_map_destroy(backend->windows);
    free(backend);
}

int anland_de_backend_add_window(anland_de_backend *backend,
                                 uint64_t window_id,
                                 const char *name,
                                 uint64_t *out_layer_id)
{
    anland_layer_desc_t desc = {
        .parent_id = 0,
        .kind = ANLAND_LAYER_NORMAL,
        .name = name,
        .opacity = 1.0f,
        .visible = true,
    };
    return anland_de_backend_add_window_desc(backend, window_id, &desc,
                                             out_layer_id);
}
int anland_de_backend_add_window_desc(anland_de_backend *backend,
                                      uint64_t window_id,
                                      const anland_layer_desc_t *desc,
                                      uint64_t *out_layer_id)
{
    if (!backend || window_id == 0 || !desc || !out_layer_id)
        return -1;
    anland_layer_id layer_id = 0;
    if (anland_scene_layer_create(anland_present_scene(backend->present),
                                  desc, &layer_id) != 0 ||
        anland_window_map_bind(backend->windows, layer_id, window_id) != 0) {
        if (layer_id)
            anland_scene_layer_destroy(anland_present_scene(backend->present), layer_id);
        return -1;
    }
    *out_layer_id = layer_id;
    return 0;
}

int anland_de_backend_remove_window(anland_de_backend *backend, uint64_t window_id)
{
    if (!backend || window_id == 0)
        return -1;

    anland_layer_id layer_id = 0;
    if (anland_window_map_unbind_window(backend->windows, window_id, &layer_id) != 0)
        return -1;
    return anland_scene_layer_destroy(anland_present_scene(backend->present), layer_id);
}

int anland_de_backend_update_window(anland_de_backend *backend,
                                    uint64_t window_id,
                                    const anland_layer_desc_t *desc)
{
    if (!backend || window_id == 0 || !desc)
        return -1;
    anland_layer_id layer_id = 0;
    if (anland_de_backend_lookup_window_layer(backend, window_id, &layer_id) != 0)
        return -1;
    return anland_scene_layer_update(anland_present_scene(backend->present),
                                     layer_id, desc);
}
int anland_de_backend_lookup_window_layer(const anland_de_backend *backend,
                                          uint64_t window_id,
                                          uint64_t *out_layer_id)
{
    if (!backend || window_id == 0 || !out_layer_id)
        return -1;
    return anland_window_map_lookup_window(backend->windows, window_id,
                                           out_layer_id);
}

int anland_de_backend_commit(anland_de_backend *backend,
                             const anland_layer_state_t *states,
                             size_t count,
                             uint64_t *out_commit_id)
{
    if (!backend || !states || count > ANLAND_SCENE_MAX_LAYERS)
        return -1;
    /* Retirement is backpressure, not a newly rejected transaction: avoid
     * enqueueing releases for ids still owned by the previous session. */
    if (!anland_present_target_available(backend->present))
        return -1;
    /* Validation, atomic acceptance and backpressure are implemented once by
     * the scene. Native framebuffer import remains the renderer's job. */
    return anland_scene_commit_submit(anland_present_scene(backend->present),
                                      states, count, out_commit_id);
}

int anland_de_backend_present(anland_de_backend *backend)
{
    return backend ? anland_present_present(backend->present) : -1;
}

int anland_de_backend_dispatch(anland_de_backend *backend,
                               anland_scene_event_t *events,
                               size_t capacity,
                               size_t *out_count)
{
    if (!backend || !events || !out_count)
        return -1;
    /* Teardown publishes drop/release events. Run it BEFORE dispatch so one
     * fallback drain also observes events generated by the dead session. */
    if (!anland_present_connected(backend->present)) {
        clear_display_cache(backend);
        (void)anland_present_pump(backend->present, 0);
    }
    if (anland_scene_dispatch(anland_present_scene(backend->present), events,
                              capacity, out_count) != 0)
        return -1;
    /* Do not forward unusable targets to WMs that consume events directly,
     * rather than reading get_target(). Keep outcome/release events untouched. */
    size_t kept = 0;
    const uint64_t current_generation =
        anland_scene_generation(anland_present_scene(backend->present));
    for (size_t i = 0; i < *out_count; ++i) {
        if (events[i].type == ANLAND_SCENE_EVENT_RENDER_TARGET_READY &&
            (!anland_present_target_available(backend->present) ||
             events[i].u.target_ready.generation != current_generation ||
             events[i].u.target_ready.count == 0 ||
             events[i].u.target_ready.index >= events[i].u.target_ready.count))
            continue;
        events[kept++] = events[i];
    }
    *out_count = kept;
    for (size_t i = 0; i < *out_count; i++) {
        switch (events[i].type) {
        /* Completion/release events pass through unchanged to the WM. */
        case ANLAND_SCENE_EVENT_RENDER_TARGET_READY: {
            /* Events queued before a disconnect may be dispatched afterwards.
             * They must not resurrect a target from a dead session. */
            anland_scene *scene = anland_present_scene(backend->present);
            const uint64_t generation = anland_scene_generation(scene);
            if (!anland_present_target_available(backend->present) ||
                events[i].u.target_ready.generation != generation ||
                events[i].u.target_ready.count == 0 ||
                events[i].u.target_ready.index >= events[i].u.target_ready.count)
                break;
            backend->target.generation = generation;
            backend->target.index = events[i].u.target_ready.index;
            backend->target.count = events[i].u.target_ready.count;
            backend->have_target = true;
            break;
        }
        case ANLAND_SCENE_EVENT_OUTPUT_CHANGED:
            if (!anland_present_connected(backend->present))
                break;
            backend->output.width = events[i].u.output.width;
            backend->output.height = events[i].u.output.height;
            backend->output.refresh_mhz = events[i].u.output.refresh_mhz;
            backend->have_output = true;
            break;
        default:
            break;
        }
    }
    /* No post-dispatch pump: newly queued retirement events must be visible
     * in this drain, not deferred until a successful reconnect. */
    return 0;
}

int anland_de_backend_get_target(const anland_de_backend *backend,
                                 anland_de_target_t *out)
{
    if (!out)
        return -1;
    memset(out, 0, sizeof(*out));
    if (!backend || !backend->have_target ||
        !anland_present_target_available(backend->present))
        return -1;
    anland_scene *scene = anland_present_scene(backend->present);
    if (!scene || backend->target.generation != anland_scene_generation(scene) ||
        backend->target.count == 0 ||
        backend->target.index >= backend->target.count)
        return -1;

    *out = backend->target;

    /* Fill in the buffer description when the backend can describe it. A slot
     * whose buffer is unknown to the presentation backend is still a valid
     * target for the DE, which imported it itself; the description is advisory. */
    anland_device *device = anland_present_device(backend->present);
    if (device) {
        anland_device_fb_t fb = { .fd = -1 };
        if (anland_device_get_fb(device, (int)out->index, &fb) == 0) {
            out->width = fb.width;
            out->height = fb.height;
            out->format = fb.format;
            out->modifier = fb.modifier;
            close(fb.fd);
        }
    }
    return 0;
}

int anland_de_backend_get_output(const anland_de_backend *backend,
                                 anland_device_output_t *out)
{
    if (!out)
        return -1;
    memset(out, 0, sizeof(*out));
    if (!backend || !backend->have_output ||
        !anland_present_connected(backend->present))
        return -1;

    /* Use the complete device description; geometry-only events do not contain
     * format, connector name or connection status. */
    anland_device *device = anland_present_device(backend->present);
    if (!device || anland_device_get_outputs(device, out, 1) != 1 ||
        !out->connected) {
        memset(out, 0, sizeof(*out));
        return -1;
    }
    return 0;
}

int anland_de_backend_buffer_fd(anland_de_backend *backend, uint32_t index)
{
    if (!backend)
        return -1;

    anland_device *device = anland_present_device(backend->present);
    if (!device)
        return -1;

    /* anland_device_get_fb() already returns a dup'ed fd owned by the caller. */
    anland_device_fb_t fb = { .fd = -1 };
    if (anland_device_get_fb(device, (int)index, &fb) != 0)
        return -1;
    return fb.fd;
}

int anland_de_backend_pump(anland_de_backend *backend, int timeout_ms)
{
    if (!backend)
        return -1;
    const int rc = anland_present_pump(backend->present, timeout_ms);
    if (!anland_present_connected(backend->present))
        clear_display_cache(backend);
    else if (rc != 0) {
        /* Release retry blocks rendering, not the live output's geometry. */
        backend->have_target = false;
        memset(&backend->target, 0, sizeof(backend->target));
    }
    return rc;
}
int anland_de_backend_reconnect(anland_de_backend *backend)
{
    if (!backend)
        return -1;
    const int rc = anland_present_reconnect(backend->present);
    if (!anland_present_connected(backend->present))
        clear_display_cache(backend);
    return rc;
}
int anland_de_backend_reopen(anland_de_backend *backend, const char *endpoint)
{
    if (!backend)
        return -1;
    anland_scene *scene = anland_present_scene(backend->present);
    const uint64_t before = anland_scene_generation(scene);
    const int rc = anland_present_reopen(backend->present, endpoint);
    /* A rejected endpoint may leave the old session intact. Only forget its
     * published state when the session actually changed or disconnected. */
    if (!anland_present_connected(backend->present) ||
        anland_scene_generation(scene) != before)
        clear_display_cache(backend);
    return rc;
}
void anland_de_backend_drop_session(anland_de_backend *backend)
{
    if (!backend)
        return;

    clear_display_cache(backend);
    /* The scene invalidates accepted work and the device releases its consumer
     * fds. WMs retire their native images through their existing fallback path. */
    anland_present_drop_session(backend->present);
}
anland_scene *anland_de_backend_scene(anland_de_backend *backend)
{
    return backend ? anland_present_scene(backend->present) : NULL;
}
anland_device *anland_de_backend_device(anland_de_backend *backend)
{
    return backend ? anland_present_device(backend->present) : NULL;
}
anland_present *anland_de_backend_present_object(anland_de_backend *backend)
{
    return backend ? backend->present : NULL;
}
int anland_de_backend_get_writable_target(const anland_de_backend *backend,
                                          anland_de_target_t *out)
{
    if (!out) return -1;
    memset(out, 0, sizeof(*out));
    if (!backend || !anland_present_renderable(backend->present) ||
        anland_de_backend_get_target(backend, out) != 0) return -1;
    anland_device *device = anland_present_device(backend->present);
    if (!device || anland_device_current_fb_raw(device) != (int)out->index) {
        memset(out, 0, sizeof(*out));
        return -1;
    }
    return 0;
}
