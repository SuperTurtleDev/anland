#include "anland_de_backend.h"
#include "anland_window_map.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
struct anland_de_backend {
    anland_present *present;
    anland_window_map *windows;
    anland_de_backend_ops_t ops;
    anland_de_backend_buffer_t imported[ANLAND_SCENE_MAX_LAYERS];
    size_t imported_count;
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

static int same_buffer(const anland_buffer_desc_t *a,
                       const anland_buffer_desc_t *b);
static int imported_index(const anland_de_backend *backend, uint64_t buffer_id);
static void release_buffer_id(anland_de_backend *backend, uint64_t buffer_id);
static void release_import_object(anland_de_backend *backend,
                                  anland_de_backend_buffer_t *buffer);

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
    backend->ops = config->ops;
    return backend;
}
/* Hand one imported object back to the DE adapter (or close its fd when the
 * adapter has no release hook). Used by every path that owns an import: the
 * committed array, the staged array during rollback, and destroy(). */
static void release_import_object(anland_de_backend *backend,
                                  anland_de_backend_buffer_t *buffer)
{
    if (!buffer)
        return;
    if (backend && backend->ops.release_buffer)
        backend->ops.release_buffer(backend->ops.userdata, buffer);
    else if (buffer->fd >= 0)
        close(buffer->fd);
    buffer->fd = -1;
    buffer->native_image = NULL;
}

static void release_imported_from(anland_de_backend *backend, size_t first)
{
    while (backend && backend->imported_count > first) {
        anland_de_backend_buffer_t *buffer =
            &backend->imported[--backend->imported_count];
        release_import_object(backend, buffer);
    }
}


void anland_de_backend_destroy(anland_de_backend *backend)
{
    if (!backend)
        return;
    release_imported_from(backend, 0);
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
    if (anland_present_backend(backend->present) == ANLAND_PRESENT_BACKEND_AWL) {
        /* A consumer may have announced the window first. Reuse that layer;
         * otherwise create the DE-owned window through the AWL backend. */
        if (anland_present_lookup_window_layer(backend->present, window_id,
                                               out_layer_id) == 0)
            return 0;
        return anland_present_window_create(backend->present, window_id, desc,
                                            out_layer_id);
    }
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
    if (anland_present_backend(backend->present) == ANLAND_PRESENT_BACKEND_AWL)
        return anland_present_window_destroy(backend->present, window_id);

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
    if (anland_present_backend(backend->present) == ANLAND_PRESENT_BACKEND_AWL)
        return anland_present_window_update(backend->present, window_id, desc);
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
    if (anland_present_backend(backend->present) == ANLAND_PRESENT_BACKEND_AWL)
        return anland_present_lookup_window_layer(backend->present, window_id,
                                                  out_layer_id);
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

    /* Importing is a transaction, not a sequence of side effects.
     *
     * The scene may still reject the commit (backpressure: another commit is in
     * flight, or validation fails), and it may do so AFTER the adapter already
     * imported buffers. Therefore:
     *   - every import is STAGED in `staged` and installed in `backend->imported`
     *     only once the scene accepted the commit;
     *   - an import that replaces an existing entry keeps the OLD object intact
     *     until the new one is installed, so a rejected commit cannot destroy a
     *     native image the in-flight commit is still sampling;
     *   - on any failure path exactly the objects created by this call are
     *     released, and every pre-existing entry is left exactly as it was.
     *
     * `imported_count` is therefore the rollback boundary: entries below it are
     * untouched by this call, and staged objects never enter the array early.
     */
    anland_de_backend_buffer_t staged[ANLAND_SCENE_MAX_LAYERS];
    size_t staged_count = 0;
    /* Entries that will ADD a slot (rather than replace one) when installed. The
     * array has a fixed capacity, so it must be reserved before anything is
     * imported: discovering the overflow after import_buffer() succeeded would
     * mean releasing an object the DE just built. */
    size_t staged_new = 0;

    if (anland_present_backend(backend->present) == ANLAND_PRESENT_BACKEND_AWL) {
        for (size_t i = 0; i < count; i++) {
            if (!states[i].visible || states[i].buffer_id == 0)
                continue;
            if (!backend->ops.import_buffer)
                goto import_failed;
            anland_buffer_desc_t desc;
            if (anland_present_get_buffer(backend->present, states[i].buffer_id,
                                          &desc) != 0)
                goto import_failed;

            const int existing = imported_index(backend, desc.buffer_id);
            if (existing >= 0 && same_buffer(&backend->imported[existing].desc,
                                             &desc))
                continue; /* already imported, nothing to stage */

            /* A buffer already staged by THIS commit must not be imported twice. */
            bool already_staged = false;
            for (size_t s = 0; s < staged_count; s++) {
                if (staged[s].desc.buffer_id == desc.buffer_id) {
                    already_staged = true;
                    break;
                }
            }
            if (already_staged)
                continue;
            if (staged_count >= ANLAND_SCENE_MAX_LAYERS)
                goto import_failed;
            if (existing < 0 &&
                backend->imported_count + staged_new >= ANLAND_SCENE_MAX_LAYERS)
                goto import_failed;

            const int fd = anland_present_dup_buffer_fd(backend->present,
                                                        states[i].buffer_id);
            if (fd < 0)
                goto import_failed;
            anland_de_backend_buffer_t *imported = &staged[staged_count];
            memset(imported, 0, sizeof(*imported));
            imported->desc = desc;
            imported->fd = fd;
            if (backend->ops.import_buffer(backend->ops.userdata, &desc, fd,
                                           imported) != 0) {
                close(fd);
                imported->fd = -1;
                goto import_failed;
            }
            if (existing < 0)
                staged_new++;
            staged_count++;
        }
    }

    if (anland_scene_commit_submit(anland_present_scene(backend->present),
                                   states, count, out_commit_id) != 0)
        goto import_failed;

    /* The commit is accepted: the staged imports are now the live ones. Only now
     * is it safe to destroy the objects they replace. Resolve by id rather than by
     * a remembered slot index, because release_buffer_id() compacts the array. */
    for (size_t s = 0; s < staged_count; s++) {
        if (imported_index(backend, staged[s].desc.buffer_id) >= 0)
            release_buffer_id(backend, staged[s].desc.buffer_id);
        if (backend->imported_count >= ANLAND_SCENE_MAX_LAYERS)
            break; /* unreachable: staged_count <= MAX_LAYERS */
        backend->imported[backend->imported_count++] = staged[s];
    }
    return 0;

import_failed:
    /* Release exactly what this call created; pre-existing imports are untouched. */
    for (size_t s = 0; s < staged_count; s++)
        release_import_object(backend, &staged[s]);
    return -1;
}

int anland_de_backend_present(anland_de_backend *backend)
{
    return backend ? anland_present_present(backend->present) : -1;
}

static int same_buffer(const anland_buffer_desc_t *a,
                       const anland_buffer_desc_t *b)
{
    return a->buffer_id == b->buffer_id &&
           a->frame_id == b->frame_id &&
           a->width == b->width && a->height == b->height &&
           a->stride == b->stride && a->format == b->format &&
           a->modifier == b->modifier && a->offset == b->offset;
}

static int imported_index(const anland_de_backend *backend, uint64_t buffer_id)
{
    for (size_t i = 0; i < backend->imported_count; i++)
        if (backend->imported[i].desc.buffer_id == buffer_id)
            return (int)i;
    return -1;
}

static void release_buffer_id(anland_de_backend *backend, uint64_t buffer_id)
{
    if (!backend || buffer_id == 0)
        return;
    for (size_t i = 0; i < backend->imported_count; i++) {
        if (backend->imported[i].desc.buffer_id != buffer_id)
            continue;
        anland_de_backend_buffer_t buffer = backend->imported[i];
        backend->imported[i] = backend->imported[--backend->imported_count];
        release_import_object(backend, &buffer);
        return;
    }
}

int anland_de_backend_dispatch(anland_de_backend *backend,
                               anland_scene_event_t *events,
                               size_t capacity,
                               size_t *out_count)
{
    if (!backend || !events || !out_count)
        return -1;
    if (anland_scene_dispatch(anland_present_scene(backend->present), events,
                              capacity, out_count) != 0)
        return -1;
    for (size_t i = 0; i < *out_count; i++) {
        switch (events[i].type) {
        case ANLAND_SCENE_EVENT_BUFFER_RELEASED:
            release_buffer_id(backend, events[i].u.released.buffer_id);
            break;
        case ANLAND_SCENE_EVENT_RENDER_TARGET_READY: {
            /* Events queued before a disconnect may be dispatched afterwards.
             * They must not resurrect a target from a dead session. */
            anland_scene *scene = anland_present_scene(backend->present);
            const uint64_t generation = anland_scene_generation(scene);
            if (!anland_present_connected(backend->present) ||
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
    return 0;
}

int anland_de_backend_get_target(const anland_de_backend *backend,
                                 anland_de_target_t *out)
{
    if (!out)
        return -1;
    memset(out, 0, sizeof(*out));
    if (!backend || !backend->have_target ||
        !anland_present_connected(backend->present))
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

    /* Legacy can provide a complete device description. Do not synthesize
     * format/name/connection status from a geometry-only scene event. AWL has
     * no output description today, so it must not advertise one. */
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
    /* Session-level teardown: every import belongs to the session that is ending,
     * so it is retired here. This is deliberately separate from the per-commit
     * release path (dispatch() releases a single buffer when the scene says it is
     * free): a dropped commit frees the buffers it referenced, while losing the
     * consumer invalidates the whole imported set. */
    release_imported_from(backend, 0);
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