/*
 * anland_device.c — DRM-like userspace display device interface
 *
 * Implementation: thin translation over the existing producer transport
 * (../libdisplay_producer/display_producer.[ch]), which already owns the
 * daemon handshake, fallback state machine, dmabuf set, shm index page and
 * the fence channel. Everything in this file is shape translation only — no
 * anland protocol knowledge beyond the public display_producer API.
 *
 * DRM semantics mapping:
 *   open()        → connect_to_deamon()           (master: single producer)
 *   connect()     → try_exit_fallback()           (hotplug / consumer appears)
 *   get_outputs() → get_screen_info()
 *   get_fb()      → get_dmabuf_info_at() + dup fd
 *   current_fb()  → get_selected_idx()
 *   commit()      → validate + remember fb index
 *   pageflip()    → set_render_fence() + trigger_refresh()
 *   poll_input()  → poll_input_event()
 *   read_input()  → poll_input_event_extend_data()
 *   read_fds()    → poll_input_event_extend_fds()
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* for dup(); KWin/mutter build systems already define it */
#endif
#include "anland_device.h"

#include "display_producer.h"
#include "protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* The public input struct must stay wire-compatible with the transport's
 * packed InputEvent so poll_input() can hand events through verbatim. */
_Static_assert(sizeof(anland_device_input_t) == sizeof(struct InputEvent),
               "anland_device_input_t must stay layout-compatible with InputEvent");

struct anland_device {
    display_ctx *ctx;        /* owned transport context */
    uint32_t     format;     /* screen_info.format (1 = ABGR8888) */
    int          pending_fb; /* committed fb index for the next pageflip */
    char         socket_path[108]; /* resolved daemon socket (for diagnostics) */
    anland_device_fallback_cb fallback_cb;
    void *fallback_userdata;
    anland_device_pre_release_cb pre_release_cb;
    void *pre_release_userdata;
};

/* Daemon socket discovery. The compile-time default is the Android path the magisk
 * module binds; desktop and containerized sessions usually mount the same daemon
 * socket elsewhere (e.g. /run/display.sock), which would otherwise force every
 * caller to pass $ANLAND_SOCKET by hand. Try the caller's hint first, then the
 * environment, then the well-known locations — the first one that answers wins. */
static const char *const s_socket_candidates[] = {
    "/run/display.sock",
    "/tmp/anland/display_daemon.sock",
    ANLAND_DEVICE_DEFAULT_SOCKET,
};

/* Try one candidate; on success the connection is live and its path is recorded. */
static display_ctx *try_socket(const char *path)
{
    if (!path || !*path)
        return NULL;
    display_ctx *ctx = NULL;
    if (connect_to_deamon(&ctx, path) == 0 && ctx)
        return ctx;
    return NULL;
}

anland_device *anland_device_open(const char *socket_path)
{
    const char *hint = (socket_path && *socket_path) ? socket_path : NULL;
    const char *env = getenv("ANLAND_SOCKET");
    if (env && !*env)
        env = NULL;

    const char *used = NULL;
    display_ctx *ctx = NULL;

    if (hint && (ctx = try_socket(hint)))
        used = hint;
    else if (env && (!hint || strcmp(env, hint) != 0) && (ctx = try_socket(env)))
        used = env;
    else {
        for (size_t i = 0; i < sizeof(s_socket_candidates) / sizeof(s_socket_candidates[0]); i++) {
            const char *path = s_socket_candidates[i];
            if (hint && strcmp(path, hint) == 0)
                continue;
            if (env && strcmp(path, env) == 0)
                continue;
            if ((ctx = try_socket(path))) {
                used = path;
                break;
            }
        }
    }
    if (!ctx)
        return NULL;

    anland_device *dev = calloc(1, sizeof(*dev));
    if (!dev) {
        disconnect(ctx);
        return NULL;
    }
    dev->ctx = ctx;
    dev->pending_fb = -1;
    /* Record which candidate answered so callers can log the real path. */
    snprintf(dev->socket_path, sizeof(dev->socket_path), "%s", used ? used : "");

    uint32_t w = 0, h = 0, f = 0, r = 0;
    if (get_screen_info(ctx, &w, &h, &f, &r) == 0)
        dev->format = f;

    return dev;
}

/* The daemon socket this device actually connected to ("" when unknown). Lets the
 * backend report the resolved path instead of the one it merely requested. */
const char *anland_device_socket_path(const anland_device *dev)
{
    return dev ? dev->socket_path : "";
}

void anland_device_close(anland_device *dev)
{
    if (!dev)
        return;
    disconnect(dev->ctx);
    free(dev);
}

bool anland_device_is_connected(const anland_device *dev)
{
    return dev && !is_fallback(dev->ctx);
}

int anland_device_connect(anland_device *dev)
{
    if (!dev)
        return -1;
    return try_exit_fallback(dev->ctx);
}

anland_device_connect_result_t anland_device_connect_result(anland_device *dev)
{
    if (!dev)
        return ANLAND_DEVICE_CONNECT_DAEMON_LOST;
    if (try_exit_fallback(dev->ctx) == 0)
        return ANLAND_DEVICE_CONNECT_READY;

    /* pickup can only fail for two reasons, and the return code alone cannot
     * tell them apart (both are -1). A dead control connection is permanent
     * until the device is reopened, so classify it explicitly instead of letting
     * the caller poll forever for a consumer that can never arrive. */
    return is_daemon_alive(dev->ctx) ?
           ANLAND_DEVICE_CONNECT_NO_CONSUMER :
           ANLAND_DEVICE_CONNECT_DAEMON_LOST;
}

void anland_device_force_fallback(anland_device *dev)
{
    if (!dev)
        return;
    force_fallback(dev->ctx);
}

bool anland_device_is_daemon_alive(const anland_device *dev)
{
    return dev && is_daemon_alive(dev->ctx);
}

int anland_device_reopen(anland_device *dev, const char *socket_path)
{
    if (!dev)
        return -1;

    const char *path = (socket_path && *socket_path) ? socket_path : dev->socket_path;
    display_ctx *ctx = try_socket(path);
    if (!ctx)
        return -1;

    disconnect(dev->ctx);
    dev->ctx = ctx;
    dev->pending_fb = -1;
    snprintf(dev->socket_path, sizeof(dev->socket_path), "%s", path ? path : "");

    uint32_t w = 0, h = 0, f = 0, r = 0;
    if (get_screen_info(ctx, &w, &h, &f, &r) == 0)
        dev->format = f;
    set_fallback_callback(dev->ctx, dev->fallback_cb, dev->fallback_userdata);
    set_pre_release_callback(dev->ctx, dev->pre_release_cb, dev->pre_release_userdata);
    return 0;
}

void anland_device_set_fallback_cb(anland_device *dev,
                                   anland_device_fallback_cb cb, void *userdata)
{
    if (!dev)
        return;
    dev->fallback_cb = cb;
    dev->fallback_userdata = userdata;
    set_fallback_callback(dev->ctx, cb, userdata);
}

void anland_device_set_pre_release_cb(anland_device *dev,
                                      anland_device_pre_release_cb cb,
                                      void *userdata)
{
    if (!dev)
        return;
    dev->pre_release_cb = cb;
    dev->pre_release_userdata = userdata;
    set_pre_release_callback(dev->ctx, cb, userdata);
}

int anland_device_data_fd(anland_device *dev)
{
    if (!dev)
        return -1;
    return get_data_fd(dev->ctx);
}

int anland_device_buffer_ready_fd(anland_device *dev)
{
    if (!dev)
        return -1;
    return get_buffer_ready_fd(dev->ctx);
}

int anland_device_audio_fd(anland_device *dev)
{
    if (!dev)
        return -1;
    return get_audio_fd(dev->ctx);
}

int anland_device_get_outputs(anland_device *dev,
                              anland_device_output_t *outs, int max)
{
    if (!dev || !outs || max < 1)
        return 0;

    uint32_t w = 0, h = 0, f = dev->format, r = 0;
    get_screen_info(dev->ctx, &w, &h, &f, &r);

    /* screen_info is cached from the producer HELLO. A consumer can reconnect
     * at a different orientation without renegotiating that HELLO, so the
     * connected buffer set is authoritative for output geometry. In fallback
     * retain screen_info for the initial KWin output before buffers exist. */
    if (anland_device_is_connected(dev)) {
        const int count = get_buf_count(dev->ctx);
        if (count <= 0)
            return 0;
        uint32_t buffer_w = 0, buffer_h = 0;
        for (int i = 0; i < count; i++) {
            struct buf_info info;
            if (get_dmabuf_info_at(dev->ctx, i, &info) < 0 ||
                info.width == 0 || info.height == 0 ||
                (i > 0 && (info.width != buffer_w || info.height != buffer_h)))
                return 0;
            buffer_w = info.width;
            buffer_h = info.height;
        }
        w = buffer_w;
        h = buffer_h;
    }

    anland_device_output_t *o = &outs[0];
    memset(o, 0, sizeof(*o));
    snprintf(o->name, sizeof(o->name), "ANLAND-1");
    o->width       = w;
    o->height      = h;
    o->format      = f;
    o->refresh_mhz = r;
    o->connected   = !is_fallback(dev->ctx);
    o->primary     = true;
    return 1;
}

int anland_device_fb_count(anland_device *dev)
{
    if (!dev)
        return 0;
    return get_buf_count(dev->ctx);
}

int anland_device_get_fb(anland_device *dev, int index, anland_device_fb_t *fb)
{
    if (!dev || !fb)
        return -1;
    if (index < 0 || index >= get_buf_count(dev->ctx))
        return -1;

    struct buf_info info;
    if (get_dmabuf_info_at(dev->ctx, index, &info) < 0)
        return -1;
    int fd = get_dmabuf_fd_at(dev->ctx, index);
    if (fd < 0)
        return -1;

    int dupfd = dup(fd);
    if (dupfd < 0)
        return -1;

    memset(fb, 0, sizeof(*fb));
    fb->fd       = dupfd;      /* caller-owned */
    fb->width    = info.width;
    fb->height   = info.height;
    fb->stride   = info.stride;
    fb->format   = info.format;
    fb->modifier = info.modifier;
    fb->offset   = info.offset;
    return 0;
}

int anland_device_current_fb(anland_device *dev)
{
    if (!dev)
        return 0;
    return get_selected_idx(dev->ctx);
}

int anland_device_current_fb_raw(anland_device *dev)
{
    if (!dev)
        return -1;
    return get_selected_idx_raw(dev->ctx);
}

int anland_device_commit(anland_device *dev, int fb_index)
{
    if (!dev)
        return -1;
    if (fb_index < 0 || fb_index >= get_buf_count(dev->ctx))
        return -1;
    dev->pending_fb = fb_index;
    return 0;
}

int anland_device_pageflip(anland_device *dev, int fence_fd,
                           void (*cb)(void *userdata, int fb_index),
                           void *userdata)
{
    if (!dev) {
        if (fence_fd >= 0)
            close(fence_fd);
        return -1;
    }

    int idx = dev->pending_fb;
    if (idx < 0)
        idx = get_selected_idx(dev->ctx);

    /* set_render_fence takes ownership of fence_fd (stashed until the next
     * trigger_refresh; a later set closes any unconsumed predecessor). */
    set_render_fence(dev->ctx, fence_fd);

    int rc = trigger_refresh(dev->ctx);
    if (rc == 0 && cb)
        cb(userdata, idx);
    return rc;
}

void anland_device_set_render_fence(anland_device *dev, int fence_fd)
{
    if (!dev) {
        if (fence_fd >= 0)
            close(fence_fd);
        return;
    }
    set_render_fence(dev->ctx, fence_fd);
}

int anland_device_trigger_refresh(anland_device *dev)
{
    if (!dev)
        return -1;
    return trigger_refresh(dev->ctx);
}

bool anland_device_notify_frame_done(anland_device *dev)
{
    if (!dev)
        return false;

    /* In fallback trigger_refresh() reports success after dropping the stashed
     * fence, because "no consumer" is not an error for the write path. A
     * compositor needs the opposite answer: the frame did NOT reach the consumer,
     * so it must fail the frame instead of claiming it was presented. */
    if (!anland_device_is_connected(dev))
    {
        trigger_refresh(dev->ctx);
        return false;
    }

    return trigger_refresh(dev->ctx) == 0;
}

int anland_device_poll_input(anland_device *dev, anland_device_input_t *ev,
                             int timeout_ms)
{
    if (!dev || !ev)
        return -1;

    struct InputEvent in;
    int rc = poll_input_event(dev->ctx, &in, timeout_ms);
    if (rc <= 0)
        return rc;

    /* Layout-compatible by the static assert above: hand the raw event
     * through verbatim (preserves pointer ids / tool types / axes). */
    memcpy(ev, &in, sizeof(in));
    return 1;
}

int anland_device_read_input(anland_device *dev, void *buf, size_t size,
                             int timeout_ms)
{
    if (!dev)
        return -1;
    return poll_input_event_extend_data(dev->ctx, buf, size, timeout_ms);
}

int anland_device_read_fds(anland_device *dev, int *fds, int max_fds,
                           int *fd_count, int timeout_ms)
{
    if (!dev || !fds || !fd_count)
        return -1;
    return poll_input_event_extend_fds(dev->ctx, fds, max_fds, fd_count,
                                       timeout_ms);
}

int anland_device_request_resources(anland_device *dev, uint32_t service_type,
                                    const uint32_t *args)
{
    if (!dev)
        return -1;
    return push_resources_request(dev->ctx, service_type, args);
}

int anland_device_set_consumer_var(anland_device *dev, uint32_t var,
                                   uint32_t value)
{
    if (!dev)
        return -1;
    const struct OutputEvent ev = {
        .type = OUTPUT_TYPE_SET_CONSUMER_VAR,
        .set_consumer_var = { .var = var, .value = value },
    };
    return push_output_event(dev->ctx, &ev);
}

int anland_device_scheduling(anland_device *dev, pid_t pid, uint8_t flags)
{
    if (!dev)
        return -1;
    const struct OutputEvent ev = {
        .type = OUTPUT_TYPE_SCHEDULING,
        .scheduling = { .pid = pid > 0 ? pid : 0, .flags = flags },
    };
    return push_output_event(dev->ctx, &ev);
}

int anland_device_set_clipboard(anland_device *dev, const void *text,
                                size_t size)
{
    if (!dev)
        return -1;
    const struct OutputEvent ev = {
        .type = OUTPUT_TYPE_CLIPBOARD,
        .clipboard = { .size = (uint32_t)size },
    };
    return push_output_event_with_length(dev->ctx, &ev,
                                         (void *)text, size);
}