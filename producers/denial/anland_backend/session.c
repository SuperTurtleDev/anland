#include "session.h"
#include "anland_de_backend.h"
#include "anland_tablet.h"
#include "anland_audio.h"
#include <stdio.h>
#include <errno.h>
#include <limits.h>
#include <linux/sync_file.h>
#include <sys/ioctl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
struct ad_session {
    anland_de_backend *backend;
    uint64_t layer;
    anland_tablet tablet;
    bool audio_started;
    int audio_fd; /* Borrowed identity only; the audio engine owns its duplicate. */
    uint64_t audio_generation;
};
static anland_device *device(ad_session *s) { return anland_de_backend_device(s->backend); }
static int status(int rc) { return rc < 0 ? -EIO : rc; }

static void sync_audio(ad_session *s)
{
    if (!s->audio_started) return;
    const int fd = anland_device_audio_fd(device(s));
    const uint64_t generation = anland_scene_generation(anland_de_backend_scene(s->backend));
    /* A new consumer may reuse the same descriptor number. Do not reset the
     * mic ring on every frame, but always rebind when the session changes. */
    if (fd != s->audio_fd || (fd >= 0 && generation != s->audio_generation)) {
        anland_audio_set_fd(fd);
        s->audio_fd = fd;
    }
    s->audio_generation = generation;
}

static void detach_audio(ad_session *s)
{
    if (s->audio_started && s->audio_fd >= 0)
        anland_audio_set_fd(-1);
    s->audio_fd = -1;
}


ad_session *ad_open(const char *endpoint)
{
    ad_session *s = calloc(1, sizeof(*s));
    if (!s) return NULL;
    anland_de_backend_config_t config = {
        .present = { .endpoint = endpoint, .defer_connector = true }, .name = "Denial",
    };
    s->backend = anland_de_backend_create(&config);
    anland_layer_desc_t desc = {
        .kind = ANLAND_LAYER_NORMAL, .name = "Denial desktop", .opacity = 1.f, .visible = true,
    };
    if (!s->backend || anland_de_backend_add_window_desc(s->backend, 1, &desc, &s->layer)) {
        ad_close(s);
        return NULL;
    }
    s->audio_fd = -1;
    s->audio_started = anland_audio_start() == 0;
    if (!s->audio_started)
        fprintf(stderr, "denial: cannot initialize Anland audio bridge; continuing without audio\n");
    sync_audio(s);
    return s;
}
void ad_close(ad_session *s)
{
    if (!s) return;
    /* Stop the audio thread before releasing the transport's borrowed fd. */
    if (s->audio_started) anland_audio_stop();
    anland_de_backend_destroy(s->backend);
    free(s);
}
int ad_connected(ad_session *s)
{
    if (!s) return 0;
    sync_audio(s);
    return anland_device_is_connected(device(s));
}
int ad_daemon_alive(ad_session *s) { return s && anland_device_is_daemon_alive(device(s)); }
int ad_reconnect(ad_session *s)
{
    if (!s) return -EINVAL;
    memset(&s->tablet, 0, sizeof(s->tablet));
    /* Observe fallback before a handshake can recycle descriptor numbers. */
    sync_audio(s);
    int rc = anland_de_backend_reconnect(s->backend);
    sync_audio(s);
    return status(rc);
}
int ad_reopen(ad_session *s, const char *endpoint)
{
    if (!s) return -EINVAL;
    memset(&s->tablet, 0, sizeof(s->tablet));
    detach_audio(s);
    int rc = anland_de_backend_reopen(s->backend, endpoint);
    sync_audio(s);
    return status(rc);
}
void ad_drop(ad_session *s)
{
    if (!s) return;
    memset(&s->tablet, 0, sizeof(s->tablet));
    /* Keep the PipeWire nodes alive; only the consumer socket is detached. */
    detach_audio(s);
    anland_de_backend_drop_session(s->backend);
}
int ad_pump(ad_session *s)
{
    if (!s) return -EINVAL;
    int rc = anland_de_backend_pump(s->backend, 0);
    sync_audio(s);
    return status(rc);
}
int ad_present(ad_session *s)
{
    if (!s) return -EINVAL;
    int rc = anland_de_backend_present(s->backend);
    sync_audio(s);
    return status(rc);
}

int ad_get_fds(ad_session *s, ad_fds *out)
{
    if (!out) return -EINVAL;
    *out = (ad_fds){ -1, -1, -1 };
    if (!s) return -EINVAL;
    out->scene = anland_scene_event_fd(anland_de_backend_scene(s->backend));
    out->ready = anland_device_buffer_ready_fd(device(s));
    out->input = anland_device_data_fd(device(s));
    return 0;
}
int ad_get_output(ad_session *s, ad_output *out)
{
    if (!out) return -EINVAL;
    *out = (ad_output){0};
    if (!s) return -EINVAL;
    anland_device_output_t raw;
    if (anland_de_backend_get_output(s->backend, &raw)) return -ENOTCONN;
    *out = (ad_output){raw.width, raw.height, raw.refresh_mhz};
    return 0;
}
static int valid_target(const anland_de_target_t *t)
{
    return t->generation && t->count && t->count <= ANLAND_DEVICE_MAX_BUFS &&
        t->index < t->count && t->width && t->height &&
        t->width <= INT32_MAX && t->height <= INT32_MAX;
}
int ad_writable(ad_session *s, ad_target *out)
{
    if (!out) return -EINVAL;
    *out = (ad_target){0};
    if (!s) return -EINVAL;
    anland_de_target_t t;
    if (anland_de_backend_get_writable_target(s->backend, &t)) return -EAGAIN;
    if (!valid_target(&t)) return -EPROTO;
    *out = (ad_target){t.generation, t.index, t.count, t.width, t.height};
    return 0;
}
int ad_import(ad_session *s, uint64_t generation, uint32_t index, ad_image *out)
{
    if (!out) return -EINVAL;
    *out = (ad_image){ .fd = -1 };
    if (!s) return -EINVAL;
    anland_de_target_t t;
    if (anland_de_backend_get_target(s->backend, &t) || !valid_target(&t) ||
        t.generation != generation || index >= t.count) return -ESTALE;
    anland_device_fb_t fb = {.fd = -1};
    if (anland_device_get_drm_fb(device(s), (int)index, &fb)) return -ENOTSUP;
    /* The public DRM view currently supports only the three 32-bit formats.
     * Bounds are checked in 64 bits before Smithay/EGL's signed dimensions. */
    uint64_t row = (uint64_t)fb.width * 4;
    if (fb.fd < 0 || fb.width != t.width || fb.height != t.height ||
        row > fb.stride || !fb.stride ||
        (uint64_t)fb.offset + (uint64_t)(fb.height - 1) * fb.stride + row > INT64_MAX) {
        if (fb.fd >= 0) close(fb.fd);
        return -EPROTO;
    }
    *out = (ad_image){fb.modifier, fb.width, fb.height, fb.stride, fb.offset, fb.format, fb.fd};
    return 0;
}
int ad_commit(ad_session *s, const ad_target *rendered, int32_t fence, uint64_t *id)
{
    if (id) *id = 0;
    if (!s || !rendered || !id || fence < -1) return -EINVAL;
    ad_target current;
    int rc = ad_writable(s, &current);
    if (rc) return rc;
    if (rendered->generation != current.generation || rendered->index != current.index ||
        rendered->count != current.count || rendered->width != current.width ||
        rendered->height != current.height) return -ESTALE;
    anland_layer_state_t state = {
        .layer_id = s->layer, .buffer_id = (uint64_t)rendered->index + 1,
        .destination = {.width = rendered->width, .height = rendered->height},
        .opacity = 1.f, .visible = true, .acquire_fence_fd = fence,
    };
    if (anland_de_backend_commit(s->backend, &state, 1, id)) return -EAGAIN;
    return 0;
}
int ad_next_event(ad_session *s, ad_event *out)
{
    if (!out) return -EINVAL;
    *out = (ad_event){ .release_fd = -1 };
    if (!s) return -EINVAL;
    anland_scene_event_t raw;
    size_t count = 0;
    int rc = anland_de_backend_dispatch(s->backend, &raw, 1, &count);
    sync_audio(s);
    if (rc) return -EIO;
    if (!count) return 0;
    out->commit_id = raw.commit_id;
    switch (raw.type) {
    case ANLAND_SCENE_EVENT_PRESENTED:
        out->type = AD_PRESENTED; out->presentation_ns = raw.u.presented.presentation_ns; break;
    case ANLAND_SCENE_EVENT_BUFFER_RELEASED:
        out->type = AD_RELEASED; out->buffer_id = raw.u.released.buffer_id;
        out->release_fd = raw.u.released.release_fence_fd; break;
    case ANLAND_SCENE_EVENT_COMMIT_DROPPED:
        out->type = AD_DROPPED; out->reason = raw.u.dropped.reason; break;
    case ANLAND_SCENE_EVENT_OUTPUT_CHANGED:
        out->type = AD_OUTPUT; out->width = raw.u.output.width; out->height = raw.u.output.height;
        out->refresh_mhz = raw.u.output.refresh_mhz; break;
    case ANLAND_SCENE_EVENT_RENDER_TARGET_READY:
        out->type = AD_TARGET; out->generation = raw.u.target_ready.generation;
        out->index = raw.u.target_ready.index; out->count = raw.u.target_ready.count; break;
    default: return -EPROTO;
    }
    return 1;
}
size_t ad_events_pending(ad_session *s)
{
    return s ? anland_scene_pending_events(anland_de_backend_scene(s->backend)) : 0;
}
int ad_fence_signaled(int32_t fd)
{
    if (fd < 0) return -EINVAL;
    /* SYNC_IOC_FILE_INFO is nonblocking and distinguishes a signaled error
     * fence from successful completion. Merely seeing POLLIN is insufficient. */
    struct sync_file_info info = {0};
    if (ioctl(fd, SYNC_IOC_FILE_INFO, &info) < 0) return -errno;
    return info.status < 0 ? -EIO : info.status > 0;
}
int ad_input_header_pending(ad_session *s)
{
    return s && anland_device_input_header_pending(device(s));
}
int ad_poll_input(ad_session *s, ad_input *out)
{
    if (!out) return -EINVAL;
    *out = (ad_input){0};
    if (!s) return -EINVAL;
    for (unsigned i = 0; i < 64; ++i) {
        anland_device_input_t raw;
        int rc = anland_device_poll_input(device(s), &raw, 0);
        sync_audio(s);
        if (rc <= 0) return status(rc);
        if (anland_tablet_is_event(raw.type)) {
            anland_tablet_sample previous, sample;
            rc = anland_tablet_feed(&s->tablet, &raw, &previous, &sample);
            if (rc < 0) {
                ad_drop(s);
                return -EPROTO;
            }
            if (rc == 0) continue;
            out->type = ANLAND_DEVICE_IN_TABLET_FRAME;
            memcpy(&out->tablet_previous, &previous, sizeof(previous));
            memcpy(&out->tablet_current, &sample, sizeof(sample));
            return 1;
        }
        out->type = raw.type;
        switch (raw.type) {
        case ANLAND_DEVICE_IN_KEY: out->action = raw.key.action; out->code = raw.key.keycode; break;
        case ANLAND_DEVICE_IN_TOUCH:
            out->action = raw.touch.action; out->pointer_id = raw.touch.pointer_id;
            out->x = raw.touch.x; out->y = raw.touch.y; break;
        case ANLAND_DEVICE_IN_PTR_MOTION:
            out->x = raw.pointer_motion.x; out->y = raw.pointer_motion.y;
            out->dx = raw.pointer_motion.dx; out->dy = raw.pointer_motion.dy; break;
        case ANLAND_DEVICE_IN_PTR_BUTTON:
            out->code = (int32_t)raw.pointer_button.button; out->action = raw.pointer_button.pressed; break;
        case ANLAND_DEVICE_IN_PTR_AXIS:
            out->code = (int32_t)raw.pointer_axis.axis; out->value = raw.pointer_axis.value;
            out->discrete = raw.pointer_axis.discrete; break;
        case ANLAND_DEVICE_IN_CLIPBOARD: out->size = raw.clipboard.size; break;
        case ANLAND_DEVICE_IN_TEXT_INPUT: out->size = raw.text_input.size; break;
        case ANLAND_DEVICE_IN_RESOURCE: case ANLAND_DEVICE_IN_RESOURCE_INVALID:
            out->resource_type = raw.resource.type; out->fd_count = raw.resource.fdnum; break;
        case ANLAND_DEVICE_IN_DISPLAY_REFRESH: out->size = raw.display.refresh_mhz; break;
        case ANLAND_DEVICE_IN_ACTION:
            out->code = (int32_t)raw.input_action.action; out->action = raw.input_action.value; break;
        case ANLAND_DEVICE_IN_TOUCH_FRAME: break;
        default: ad_drop(s); return -EPROTO;
        }
        if ((raw.type == ANLAND_DEVICE_IN_CLIPBOARD || raw.type == ANLAND_DEVICE_IN_TEXT_INPUT) &&
            out->size > ANLAND_DEVICE_MAX_PAYLOAD_SIZE) { ad_drop(s); return -EMSGSIZE; }
        if ((raw.type == ANLAND_DEVICE_IN_RESOURCE || raw.type == ANLAND_DEVICE_IN_RESOURCE_INVALID) &&
            out->fd_count > 64) { ad_drop(s); return -EMSGSIZE; }
        return 1;
    }
    return 0;
}
int ad_read_payload(ad_session *s, void *data, size_t size)
{
    if (!s || (!data && size) || size > ANLAND_DEVICE_MAX_PAYLOAD_SIZE) return -EINVAL;
    return status(anland_device_read_input(device(s), data, size, 0));
}
int ad_read_fds(ad_session *s, int32_t *fds, uint32_t capacity, uint32_t *count)
{
    if (count) *count = 0;
    if (!s || !fds || !count || capacity > 64) return -EINVAL;
    int n = 0;
    int rc = anland_device_read_fds(device(s), fds, (int)capacity, &n, 0);
    if (rc == 1) *count = (uint32_t)n;
    return status(rc);
}
int ad_clipboard(ad_session *s, const void *data, size_t size)
{
    if (!s || (!data && size) || size > ANLAND_DEVICE_MAX_PAYLOAD_SIZE) return -EINVAL;
    /* Unlike generic facade failures, queue rejection explicitly documents
     * errno: preserve ENOBUFS so the caller can retry without dropping input. */
    errno = 0;
    int rc = anland_device_queue_clipboard(device(s), data, size);
    return rc < 0 ? -(errno ? errno : EIO) : rc;
}
int ad_flush(ad_session *s) { return s ? status(anland_device_flush_output(device(s))) : -EINVAL; }
size_t ad_pending_output(ad_session *s) { return s ? anland_device_pending_output(device(s)) : 0; }
int ad_capture_pointer(ad_session *s, uint32_t capture)
{
    if (!s || capture > 1) return -EINVAL;
    return status(anland_device_set_consumer_var(device(s), ANLAND_DEVICE_VAR_CAPTURE_MOUSE, capture));
}
