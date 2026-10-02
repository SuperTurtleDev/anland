#include "session.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct anland_gamescope_session {
    anland_de_backend *backend;
    uint64_t layer;
};

anland_device *anland_gamescope_session_device(anland_gamescope_session *s)
{
    return s ? anland_de_backend_device(s->backend) : NULL;
}

anland_gamescope_session *anland_gamescope_session_open(const char *endpoint)
{
    anland_present_config_t present;
    if (anland_present_config_from_environment(&present, endpoint, NULL) != 0 ||
        present.backend != ANLAND_PRESENT_BACKEND_LEGACY)
        return NULL;
    anland_gamescope_session *s = calloc(1, sizeof(*s));
    if (!s)
        return NULL;
    const anland_de_backend_config_t config = {
        .present = present, .name = "gamescope"
    };
    s->backend = anland_de_backend_create(&config);
    const anland_layer_desc_t layer = {
        .kind = ANLAND_LAYER_NORMAL, .name = "Gamescope composite",
        .opacity = 1.0f, .visible = true
    };
    if (!s->backend ||
        anland_de_backend_add_window_desc(s->backend, 1, &layer, &s->layer) != 0) {
        anland_gamescope_session_close(s);
        return NULL;
    }
    return s;
}

void anland_gamescope_session_close(anland_gamescope_session *s)
{
    if (!s)
        return;
    anland_de_backend_destroy(s->backend);
    free(s);
}

int anland_gamescope_session_pump(anland_gamescope_session *s, int timeout_ms)
{
    return s ? anland_de_backend_pump(s->backend, timeout_ms) : -1;
}

int anland_gamescope_session_dispatch(anland_gamescope_session *s,
    anland_scene_event_t *events, size_t capacity, size_t *count)
{
    return s ? anland_de_backend_dispatch(s->backend, events, capacity, count) : -1;
}

int anland_gamescope_session_output(anland_gamescope_session *s,
    anland_device_output_t *output)
{
    return s ? anland_de_backend_get_output(s->backend, output) : -1;
}

int anland_gamescope_session_target(anland_gamescope_session *s,
    anland_gamescope_target *target)
{
    if (!target)
        return -1;
    memset(target, 0, sizeof(*target));
    target->buffer.fd = -1;
    if (!s || anland_de_backend_get_target(s->backend, &target->selection) != 0)
        return -1;
    /* Legacy exposes its consumer buffers through the public device API;
     * its presentation registry deliberately does not implement get_buffer(). */
    anland_device_fb_t fb = {.fd = -1};
    if (anland_device_get_fb(anland_gamescope_session_device(s),
            (int)target->selection.index, &fb) != 0)
        return -1;
    target->buffer = (anland_buffer_desc_t){
        .buffer_id = (uint64_t)target->selection.index + 1,
        .frame_id = target->selection.generation,
        .width = fb.width, .height = fb.height, .stride = fb.stride,
        .format = fb.format, .modifier = fb.modifier, .offset = fb.offset,
        .fd = fb.fd
    };
    if (fb.fd < 0 || !fb.width || !fb.height || !fb.stride) {
        if (fb.fd >= 0)
            close(fb.fd);
        target->buffer.fd = -1;
        return -1;
    }
    if (anland_gamescope_session_target_current(s, target) != 0) {
        close(target->buffer.fd);
        target->buffer.fd = -1;
        return -1;
    }
    return 0;
}

int anland_gamescope_session_target_current(anland_gamescope_session *s,
    const anland_gamescope_target *target)
{
    anland_de_target_t current;
    if (!s || !target ||
        anland_de_backend_get_target(s->backend, &current) != 0 ||
        current.generation != target->selection.generation ||
        current.index != target->selection.index ||
        current.count != target->selection.count ||
        current.index >= current.count)
        return -1;
    anland_device_fb_t fb = {.fd = -1};
    if (anland_device_get_fb(anland_gamescope_session_device(s),
            (int)current.index, &fb) != 0)
        return -1;
    if (fb.fd >= 0)
        close(fb.fd);
    const anland_buffer_desc_t *cached = &target->buffer;
    return (uint64_t)current.index + 1 == cached->buffer_id &&
           current.generation == cached->frame_id &&
           fb.width == cached->width && fb.height == cached->height &&
           fb.stride == cached->stride && fb.offset == cached->offset &&
           fb.format == cached->format && fb.modifier == cached->modifier ? 0 : -1;
}

int anland_gamescope_session_submit(anland_gamescope_session *s,
    const anland_gamescope_target *target, int fence, uint64_t *commit_id)
{
    if (commit_id)
        *commit_id = 0;
    anland_device_output_t output;
    if (anland_gamescope_session_target_current(s, target) != 0 ||
        anland_gamescope_session_output(s, &output) != 0 ||
        !output.width || !output.height)
        return -1;
    const anland_layer_state_t state = {
        .layer_id = s->layer, .buffer_id = target->buffer.buffer_id,
        .source = {0, 0, target->buffer.width, target->buffer.height},
        .destination = {0, 0, output.width, output.height},
        .opacity = 1.0f, .visible = true, .acquire_fence_fd = fence
    };
    if (anland_de_backend_commit(s->backend, &state, 1, commit_id) != 0)
        return -1;
    return anland_de_backend_present(s->backend);
}

int anland_gamescope_session_reconnect(anland_gamescope_session *s)
{
    return s ? anland_de_backend_reconnect(s->backend) : -1;
}
int anland_gamescope_session_reopen(anland_gamescope_session *s, const char *endpoint)
{
    return s ? anland_de_backend_reopen(s->backend, endpoint) : -1;
}
void anland_gamescope_session_drop(anland_gamescope_session *s)
{
    if (s)
        anland_de_backend_drop_session(s->backend);
}