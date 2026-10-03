#include "anland_niri_bridge.h"
#include "anland_de_backend.h"
#include "bounded_input_read.h"
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct anland_niri_bridge {
    anland_de_backend *backend;
    uint64_t layer;
};

static anland_device *device(anland_niri_bridge *b)
{
    return b ? anland_de_backend_device(b->backend) : NULL;
}

anland_niri_bridge *anland_niri_open(const char *endpoint)
{
    /* Arch_v5 niri is the legacy producer. Do not let an inherited global
     * backend selection redirect it to AWL or make daemon startup fail. */
    anland_present_config_t present = {
        .backend = ANLAND_PRESENT_BACKEND_LEGACY,
        .endpoint = endpoint,
        .runtime_dir = NULL,
    };
    anland_de_backend_config_t cfg = { .present = present, .name = "niri" };
    anland_niri_bridge *b = calloc(1, sizeof(*b));
    if (!b)
        return NULL;
    b->backend = anland_de_backend_create(&cfg);
    if (!b->backend) {
        free(b);
        return NULL;
    }
    anland_layer_desc_t layer = {
        .kind = ANLAND_LAYER_NORMAL, .name = "niri composite",
        .opacity = 1.0f, .visible = true
    };
    if (anland_de_backend_add_window_desc(b->backend, 1, &layer, &b->layer) != 0) {
        anland_niri_close(b);
        return NULL;
    }
    return b;
}
void anland_niri_close(anland_niri_bridge *b)
{
    if (!b) return;
    anland_de_backend_destroy(b->backend);
    free(b);
}
int anland_niri_pump(anland_niri_bridge *b, int timeout_ms)
{
    return b ? anland_de_backend_pump(b->backend, timeout_ms) : -1;
}
int anland_niri_reconnect(anland_niri_bridge *b)
{
    return b ? anland_de_backend_reconnect(b->backend) : -1;
}
int anland_niri_reopen(anland_niri_bridge *b, const char *endpoint)
{
    return b ? anland_de_backend_reopen(b->backend, endpoint) : -1;
}
int anland_niri_connected(anland_niri_bridge *b)
{
    return device(b) && anland_device_is_connected(device(b));
}
int anland_niri_daemon_alive(anland_niri_bridge *b)
{
    return device(b) && anland_device_is_daemon_alive(device(b));
}
uint64_t anland_niri_generation(anland_niri_bridge *b)
{
    return b ? anland_scene_generation(anland_de_backend_scene(b->backend)) : 0;
}
void anland_niri_drop_session(anland_niri_bridge *b)
{
    if (b) anland_de_backend_drop_session(b->backend);
}
int anland_niri_output_info(anland_niri_bridge *b, anland_niri_output *out)
{
    if (!out || !b) return -1;
    anland_device_output_t raw;
    /* During daemon HELLO the consumer may not yet exist. Allow initial niri
     * output setup from the device's cached screen_info without claiming it is
     * connected. Live sessions must use the validated DE output description. */
    if (anland_de_backend_get_output(b->backend, &raw) != 0 &&
        (!device(b) || anland_device_get_outputs(device(b), &raw, 1) != 1))
        return -1;
    *out = (anland_niri_output){raw.width, raw.height, raw.refresh_mhz, raw.format};
    return 0;
}
int anland_niri_get_target(anland_niri_bridge *b, anland_niri_target *out)
{
    if (!out || !b) return -1;
    memset(out, 0, sizeof(*out));
    out->fd = -1;
    anland_de_target_t target;
    if (anland_de_backend_get_target(b->backend, &target) != 0) return -1;
    anland_device_fb_t fb = {.fd = -1};
    if (!device(b) || anland_device_get_fb(device(b), (int)target.index, &fb) != 0)
        return -1;
    out->generation = target.generation;
    out->index = target.index;
    out->count = target.count;
    out->width = fb.width;
    out->height = fb.height;
    out->format = fb.format;
    out->stride = fb.stride;
    out->offset = fb.offset;
    out->modifier = fb.modifier;
    out->fd = fb.fd;
    return 0;
}
int anland_niri_submit(anland_niri_bridge *b, uint64_t generation,
                       uint32_t index, int fence, uint64_t *commit_id)
{
    if (!b || !b->layer) return -1;
    anland_de_target_t target;
    anland_device_output_t output;
    if (anland_de_backend_get_target(b->backend, &target) != 0 ||
        anland_de_backend_get_output(b->backend, &output) != 0 ||
        target.generation != generation || target.index != index ||
        index >= target.count || !output.width || !output.height)
        return -1;
    anland_layer_state_t state = {
        .layer_id = b->layer, .buffer_id = (uint64_t)index + 1,
        .destination = {0, 0, output.width, output.height},
        .opacity = 1.0f, .visible = true, .acquire_fence_fd = fence
    };
    if (anland_de_backend_commit(b->backend, &state, 1, commit_id) != 0) return -1;
    return anland_de_backend_present(b->backend); /* any failure is surfaced by dispatch */
}
int anland_niri_dispatch(anland_niri_bridge *b, anland_niri_event *out,
                         size_t capacity, size_t *count)
{
    if (!b || !out || !count) return -1;
    *count = 0;
    anland_scene_event_t events[ANLAND_SCENE_EVENT_QUEUE];
    if (capacity > ANLAND_SCENE_EVENT_QUEUE) capacity = ANLAND_SCENE_EVENT_QUEUE;
    size_t n = 0;
    if (!capacity || anland_de_backend_dispatch(b->backend, events, capacity, &n) != 0)
        return -1;
    for (size_t i = 0; i < n; i++) {
        anland_niri_event *e = &out[i];
        memset(e, 0, sizeof(*e));
        e->release_fence_fd = -1;
        e->type = events[i].type;
        e->commit_id = events[i].commit_id;
        switch (events[i].type) {
        case ANLAND_SCENE_EVENT_RENDER_TARGET_READY:
            e->generation = events[i].u.target_ready.generation;
            e->index = events[i].u.target_ready.index;
            e->count = events[i].u.target_ready.count;
            break;
        case ANLAND_SCENE_EVENT_OUTPUT_CHANGED:
            e->width = events[i].u.output.width;
            e->height = events[i].u.output.height;
            e->refresh_mhz = events[i].u.output.refresh_mhz;
            break;
        case ANLAND_SCENE_EVENT_PRESENTED:
            e->presentation_ns = events[i].u.presented.presentation_ns;
            break;
        case ANLAND_SCENE_EVENT_BUFFER_RELEASED:
            e->buffer_id = events[i].u.released.buffer_id;
            e->release_fence_fd = events[i].u.released.release_fence_fd;
            break;
        default: break;
        }
    }
    *count = n;
    return 0;
}
int anland_niri_poll_input(anland_niri_bridge *b, anland_niri_input *out)
{
    if (!device(b) || !out) return -1;
    anland_device_input_t ev;
    int rc = anland_device_poll_input(device(b), &ev, 0);
    if (rc != 1) return rc;
    memset(out, 0, sizeof(*out));
    out->type = ev.type;
    switch (ev.type) {
    case ANLAND_DEVICE_IN_KEY:
        out->action = ev.key.action; out->code = ev.key.keycode; break;
    case ANLAND_DEVICE_IN_TOUCH:
        out->action = ev.touch.action; out->x = ev.touch.x;
        out->y = ev.touch.y; out->pointer_id = ev.touch.pointer_id; break;
    case ANLAND_DEVICE_IN_PTR_MOTION:
        out->x = ev.pointer_motion.x; out->y = ev.pointer_motion.y;
        out->dx = ev.pointer_motion.dx; out->dy = ev.pointer_motion.dy; break;
    case ANLAND_DEVICE_IN_PTR_BUTTON:
        out->code = (int32_t)ev.pointer_button.button;
        out->action = ev.pointer_button.pressed; break;
    case ANLAND_DEVICE_IN_PTR_AXIS:
        out->code = (int32_t)ev.pointer_axis.axis;
        out->value = ev.pointer_axis.value;
        out->discrete = ev.pointer_axis.discrete; break;
    case ANLAND_DEVICE_IN_CLIPBOARD: out->payload_size = ev.clipboard.size; break;
    case ANLAND_DEVICE_IN_TEXT_INPUT: out->payload_size = ev.text_input.size; break;
    case ANLAND_DEVICE_IN_RESOURCE:
    case ANLAND_DEVICE_IN_RESOURCE_INVALID:
        out->resource_type = ev.resource.type; out->fd_count = ev.resource.fdnum; break;
    default: break;
    }
    return rc;
}
/* The common read_input() polls only before recv_all(), which can block forever
 * after a partial payload. Keep the compositor event loop bounded even when the
 * consumer stops mid-message. The caller drops this session on any error. */
int anland_niri_read_payload(anland_niri_bridge *b, void *buf, size_t size, int timeout_ms)
{
    if (!device(b)) return -1;
    return anland_niri_read_exact(anland_device_data_fd(device(b)), buf, size, timeout_ms);
}
int anland_niri_read_fds(anland_niri_bridge *b, int *fds, int max_fds, int *count, int timeout_ms)
{
    if (!device(b) || !fds || !count || max_fds <= 0) return -1;
    *count = 0;
    /* On a malformed frame, the transport closes received descriptors itself.
     * Never expose those now-invalid integers as owned fds to Rust. */
    const int rc = anland_device_read_fds(device(b), fds, max_fds, count, timeout_ms);
    if (rc != 1) {
        *count = 0;
        return rc;
    }
    return rc;
}
int anland_niri_send_clipboard(anland_niri_bridge *b, const void *buf, size_t size)
{
    return device(b) ? anland_device_set_clipboard(device(b), buf, size) : -1;
}
int anland_niri_request_resources(anland_niri_bridge *b, uint32_t service)
{
    return device(b) ? anland_device_request_resources(device(b), service, NULL) : -1;
}
int anland_niri_audio_fd(anland_niri_bridge *b)
{
    return device(b) ? anland_device_audio_fd(device(b)) : -1;
}
