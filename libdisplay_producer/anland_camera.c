#define _GNU_SOURCE
#include "anland_camera.h"
#include "protocol.h"

#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <sys/mman.h>

#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/buffers.h>
#include <spa/pod/builder.h>
#include <spa/utils/hook.h>

/*
 * Control-channel protocol + stream/shm protocol. MUST stay in sync with the
 * consumer's camera_service.h.
 */
struct camera_ctrl_msg {
    uint8_t  type;
    uint8_t  reserved;
    uint16_t len;
    uint8_t  payload[];
} __attribute__((packed));

#define CAMERA_CTRL_GET_INFO     0x01
#define CAMERA_CTRL_START_RECORD 0x02   /* payload: id(1B) w(2B,LE) h(2B,LE) */
#define CAMERA_CTRL_STOP_RECORD  0x03   /* payload: id(1B) */
#define CAMERA_CTRL_INFO_REPLY   0x81

/* stream_fd control protocol (SEQPACKET). Frame pixels travel through shared memory;
 * this only carries the shm hand-off and the READY/DONE pacing. */
#define CAMERA_SLOTS 2
struct cam_stream_msg {
    uint8_t  type;
    uint8_t  slot;
    uint16_t fmt;   /* READY: CAM_FMT_*; else 0 */
    uint32_t a;     /* SHM_OFFER: slot_bytes; READY: width  */
    uint32_t b;     /* READY: height                        */
} __attribute__((packed));

#define CAM_STREAM_GET_SHM   1
#define CAM_STREAM_SHM_OFFER 2
#define CAM_STREAM_READY     3
#define CAM_STREAM_DONE      4

/* Pixel layout in a slot (matches consumer camera_service.h). All are w*h*3/2 bytes,
 * single block, Y-stride = w, so only the announced SPA format differs. */
#define CAM_FMT_I420 0
#define CAM_FMT_NV12 1
#define CAM_FMT_NV21 2

/* Default resolution we advertise + request before anything is negotiated. The node
 * format then FOLLOWS whatever the consumer actually delivers (every READY carries the
 * real w/h): if the device's CameraX picks a different size, the first frame triggers a
 * re-negotiation so the node always matches the real frames -- no garbage, no truncation. */
#define CAM_DEF_W   1280
#define CAM_DEF_H   720
#define CAM_FPS     30

#define MAX_CAMERAS 8
#define RECONNECT_SECS 1

struct cam {
    struct anland_camera *owner;
    int                   index;        /* camera id, for ctrl messages */

    /* Persistent virtual webcam node -- created when a camera first appears, kept
     * alive across consumer disconnects (blank frames keep flowing), destroyed only
     * when the camera count shrinks or the engine stops. */
    struct pw_stream     *node;
    struct spa_hook       listener;
    bool                  streaming;    /* an app is consuming this node */
    bool                  recording;    /* we've told the consumer to START on ctrl */
    bool                  process_seen; /* one-shot debug: on_process was called */

    /* Current negotiated node format. The node always advertises exactly this, and we
     * re-negotiate it to match the resolution + pixel layout the consumer actually sends. */
    uint32_t              fmt_w, fmt_h;
    int                   fmt_code;   /* CAM_FMT_* currently announced */

    /* Hot-swapped per (re)connect. -1 while detached -> the node emits blank frames. */
    int                   stream_fd;
    struct spa_source    *io;

    /* Per-camera shared-memory double buffer, offered by the consumer over stream_fd.
     * On READY we copy the named slot out into our own `frame` (so the consumer can
     * reuse the slot immediately) and DONE it; on_process emits `frame` at app pace. */
    int                   shm_fd;       /* owned dup of the consumer's ashmem, -1 none */
    uint8_t              *shm;          /* mmap (PROT_READ), CAMERA_SLOTS*slot_bytes    */
    size_t                shm_bytes;
    size_t                slot_bytes;
    bool                  shm_requested;

    /* Producer-owned copy of the latest frame (loop thread only). */
    uint8_t              *frame;
    size_t                frame_cap;
    size_t                frame_size;
    bool                  have_frame;
};

struct anland_camera {
    struct pw_thread_loop *loop;
    struct pw_context     *context;
    struct pw_core        *core;
    struct spa_hook        core_listener;
    struct spa_source     *reconnect_timer;
    bool                   pw_connected;

    int                    ctrl_fd;     /* owned; shared control socket, -1 when none */
    int                    num_cameras; /* number of live nodes */
    struct cam             cams[MAX_CAMERAS];
};

static struct anland_camera *g_cam = NULL;

static int  build_pw(struct anland_camera *c);
static void teardown_pw(struct anland_camera *c);
static void create_nodes(struct anland_camera *c);
static void destroy_nodes(struct anland_camera *c);
static void attach_fd(struct cam *cam, int fd);
static void detach_fd(struct cam *cam);
static const struct spa_pod *build_video_format(struct spa_pod_builder *b,
                                                uint32_t w, uint32_t h, int fmt_code);

/* ---- control channel ---- */

static void send_ctrl(struct anland_camera *c, uint8_t type,
                      const uint8_t *payload, uint16_t len)
{
    if (c->ctrl_fd < 0)
        return;
    uint8_t buf[sizeof(struct camera_ctrl_msg) + 8];
    if (len > sizeof(buf) - sizeof(struct camera_ctrl_msg))
        return;
    struct camera_ctrl_msg *h = (struct camera_ctrl_msg *)buf;
    h->type = type;
    h->reserved = 0;
    h->len = len;
    if (len)
        memcpy(buf + sizeof(*h), payload, len);
    send(c->ctrl_fd, buf, sizeof(*h) + len, MSG_NOSIGNAL | MSG_DONTWAIT);
}

/* Send a fixed stream-control message (GET_SHM / DONE) on a camera's stream socket. */
static void send_stream(struct cam *cam, uint8_t type, uint8_t slot)
{
    if (cam->stream_fd < 0)
        return;
    struct cam_stream_msg m = { .type = type, .slot = slot };
    send(cam->stream_fd, &m, sizeof(m), MSG_NOSIGNAL | MSG_DONTWAIT);
}

/* Ask the consumer to hand over the shm fd (once per (re)attach, while streaming). */
static void request_shm(struct cam *cam)
{
    if (!cam->streaming || cam->stream_fd < 0 || cam->shm || cam->shm_requested)
        return;
    cam->shm_requested = true;
    send_stream(cam, CAM_STREAM_GET_SHM, 0);
}

/* Reconcile the "should the consumer be recording?" state with what we've told it.
 * Recording is wanted when an app is consuming the node AND we have a live control
 * socket + stream fd. Requests the consumer at the node's current format. */
static void update_recording(struct cam *cam)
{
    const bool want = cam->streaming && cam->stream_fd >= 0 && cam->owner->ctrl_fd >= 0;
    if (want == cam->recording)
        return;
    if (want) {
        uint8_t p[5];
        uint16_t w = (uint16_t)cam->fmt_w, h = (uint16_t)cam->fmt_h;
        p[0] = (uint8_t)cam->index;
        memcpy(p + 1, &w, sizeof(w));
        memcpy(p + 3, &h, sizeof(h));
        send_ctrl(cam->owner, CAMERA_CTRL_START_RECORD, p, sizeof(p));
        fprintf(stderr, "anland-camera: START_RECORD cam=%d %ux%u\n",
                cam->index, cam->fmt_w, cam->fmt_h);
        request_shm(cam);   /* pull the shm fd so frames can flow */
    } else if (cam->owner->ctrl_fd >= 0) {
        uint8_t id = (uint8_t)cam->index;
        send_ctrl(cam->owner, CAMERA_CTRL_STOP_RECORD, &id, 1);
        fprintf(stderr, "anland-camera: STOP_RECORD cam=%d\n", cam->index);
    }
    cam->recording = want;
}

/* ---- frame emission ---- */

/* The graph calls this whenever the consuming app needs a frame. Emit the latest
 * received frame, or neutral gray while detached / before the first frame arrives, so
 * a consuming app always gets a feed. Runs on the loop thread, same as the stream-
 * socket reader, so cam->frame needs no lock. */
static void on_process(void *data)
{
    struct cam *cam = data;
    if (!cam->node)
        return;

    struct pw_buffer *pwb = pw_stream_dequeue_buffer(cam->node);
    if (!cam->process_seen) {
        fprintf(stderr, "anland-camera: cam=%d on_process called, buffer=%s\n",
                cam->index, pwb ? "ok" : "NULL(out of buffers)");
        cam->process_seen = true;
    }
    if (!pwb)
        return;
    struct spa_data *d = &pwb->buffer->datas[0];
    if (d->data) {
        const bool live = cam->have_frame && cam->stream_fd >= 0;
        uint32_t fmt_bytes = cam->fmt_w * cam->fmt_h * 3 / 2;
        uint32_t avail = live ? (uint32_t)cam->frame_size : fmt_bytes;
        uint32_t copy = avail < d->maxsize ? avail : d->maxsize;
        if (live)
            memcpy(d->data, cam->frame, copy);
        else
            memset(d->data, 128, copy);   /* neutral gray: valid I420 at any size */
        d->chunk->offset = 0;
        d->chunk->stride = cam->fmt_w;   /* I420 Y-plane stride */
        d->chunk->size = copy;
    }
    pw_stream_queue_buffer(cam->node, pwb);
}

/* Re-announce the node's format to match what the consumer is actually sending (size
 * AND pixel layout), so the advertised caps always equal the real frames. The app
 * renegotiates and on_param_changed resizes the buffer pool. fmt_* are updated
 * optimistically so this doesn't re-fire on every subsequent frame. */
static void renegotiate_format(struct cam *cam, uint32_t w, uint32_t h, int fmt_code)
{
    if (!cam->node || w == 0 || h == 0)
        return;
    fprintf(stderr, "anland-camera: cam=%d renegotiate %ux%u/fmt%d -> %ux%u/fmt%d\n",
            cam->index, cam->fmt_w, cam->fmt_h, cam->fmt_code, w, h, fmt_code);
    cam->fmt_w = w;
    cam->fmt_h = h;
    cam->fmt_code = fmt_code;
    uint8_t buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const struct spa_pod *params[1] = { build_video_format(&b, w, h, fmt_code) };
    pw_stream_update_params(cam->node, params, 1);
}

/* ---- stream socket: shm hand-off + READY/DONE pacing ---- */

static void unmap_shm(struct cam *cam)
{
    if (cam->shm) {
        munmap(cam->shm, cam->shm_bytes);
        cam->shm = NULL;
    }
    if (cam->shm_fd >= 0) {
        close(cam->shm_fd);
        cam->shm_fd = -1;
    }
    cam->shm_bytes = cam->slot_bytes = 0;
    cam->shm_requested = false;
}

static void handle_stream_msg(struct cam *cam, const struct cam_stream_msg *m, int rfd)
{
    switch (m->type) {
    case CAM_STREAM_SHM_OFFER: {
        if (rfd < 0)
            return;
        size_t slot_bytes = m->a;
        size_t total = (size_t)CAMERA_SLOTS * slot_bytes;
        if (cam->shm)
            munmap(cam->shm, cam->shm_bytes);
        if (cam->shm_fd >= 0)
            close(cam->shm_fd);
        cam->shm_fd = rfd;
        cam->slot_bytes = slot_bytes;
        cam->shm_bytes = total;
        cam->shm = mmap(NULL, total, PROT_READ, MAP_SHARED, rfd, 0);
        if (cam->shm == MAP_FAILED) {
            cam->shm = NULL;
            close(rfd);
            cam->shm_fd = -1;
            cam->shm_bytes = cam->slot_bytes = 0;
            return;
        }
        fprintf(stderr, "anland-camera: cam=%d shm mapped %zu B/slot\n",
                cam->index, slot_bytes);
        break;
    }
    case CAM_STREAM_READY: {
        if (rfd >= 0)
            close(rfd);   /* READY carries no fd */
        uint8_t slot = m->slot;
        uint32_t w = m->a, h = m->b;
        int fmt = m->fmt;
        if (cam->shm && slot < CAMERA_SLOTS) {
            size_t bytes = (size_t)w * h * 3 / 2;
            if (bytes > cam->slot_bytes)
                bytes = cam->slot_bytes;
            if (cam->frame_cap < bytes) {
                uint8_t *nb = realloc(cam->frame, bytes);
                if (nb) {
                    cam->frame = nb;
                    cam->frame_cap = bytes;
                }
            }
            if (cam->frame_cap >= bytes && bytes > 0) {
                /* Copy out of the slot promptly so the consumer can reuse it. */
                memcpy(cam->frame, cam->shm + (size_t)slot * cam->slot_bytes, bytes);
                cam->frame_size = bytes;
                if (!cam->have_frame)
                    fprintf(stderr, "anland-camera: cam=%d first frame %ux%u\n",
                            cam->index, w, h);
                cam->have_frame = true;
            }
        }
        /* Release the slot (even if we couldn't read, so the consumer never wedges). */
        send_stream(cam, CAM_STREAM_DONE, slot);
        if (w && h && (w != cam->fmt_w || h != cam->fmt_h || fmt != cam->fmt_code))
            renegotiate_format(cam, w, h, fmt);
        break;
    }
    default:
        if (rfd >= 0)
            close(rfd);
        break;
    }
}

static void on_stream_readable(void *data, int fd, uint32_t mask)
{
    struct cam *cam = data;
    if (mask & (SPA_IO_ERR | SPA_IO_HUP))
        return;
    if (!(mask & SPA_IO_IN))
        return;

    for (;;) {
        struct cam_stream_msg m;
        int rfd = -1;
        struct iovec iov = { .iov_base = &m, .iov_len = sizeof(m) };
        union {
            char buf[CMSG_SPACE(sizeof(int))];
            struct cmsghdr align;
        } cmsg;
        struct msghdr msg = { .msg_iov = &iov, .msg_iovlen = 1,
                              .msg_control = cmsg.buf, .msg_controllen = sizeof(cmsg.buf) };
        ssize_t n = recvmsg(fd, &msg, MSG_DONTWAIT);
        if (n <= 0)
            break;
        struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
        if (c && c->cmsg_type == SCM_RIGHTS)
            memcpy(&rfd, CMSG_DATA(c), sizeof(int));
        if (n >= (ssize_t)sizeof(m))
            handle_stream_msg(cam, &m, rfd);
        else if (rfd >= 0)
            close(rfd);
    }
}

/* Point a camera at a fresh stream socket (or -1 to detach). Drops the old shm mapping
 * (the new connection re-offers it) so a stale region never bleeds into the new one. */
static void attach_fd(struct cam *cam, int fd)
{
    struct pw_loop *loop = pw_thread_loop_get_loop(cam->owner->loop);
    if (cam->io) {
        pw_loop_destroy_source(loop, cam->io);
        cam->io = NULL;
    }
    if (cam->stream_fd >= 0)
        close(cam->stream_fd);
    cam->stream_fd = fd;
    unmap_shm(cam);                   /* re-requested on the new connection */
    cam->have_frame = false;          /* show blank until a fresh frame arrives */
    if (fd >= 0)
        cam->io = pw_loop_add_io(loop, fd, SPA_IO_IN, false, on_stream_readable, cam);
    update_recording(cam);            /* START + GET_SHM if an app is consuming */
    request_shm(cam);                 /* also (re)request if already streaming */
}

static void detach_fd(struct cam *cam)
{
    attach_fd(cam, -1);
}

/* ---- Video/Source node ---- */

static uint32_t spa_video_fmt(int code)
{
    switch (code) {
    case CAM_FMT_NV12: return SPA_VIDEO_FORMAT_NV12;
    case CAM_FMT_NV21: return SPA_VIDEO_FORMAT_NV21;
    default:           return SPA_VIDEO_FORMAT_I420;
    }
}

static const struct spa_pod *build_video_format(struct spa_pod_builder *b,
                                                uint32_t w, uint32_t h, int fmt_code)
{
    struct spa_video_info_raw info;
    spa_zero(info);
    info.format = spa_video_fmt(fmt_code);
    info.size = SPA_RECTANGLE(w, h);
    info.framerate = SPA_FRACTION(CAM_FPS, 1);
    return spa_format_video_raw_build(b, SPA_PARAM_EnumFormat, &info);
}

static void on_stream_state_changed(void *data, enum pw_stream_state old,
                                    enum pw_stream_state state, const char *error)
{
    struct cam *cam = data;
    (void)error;
    fprintf(stderr, "anland-camera: cam=%d state %s -> %s\n", cam->index,
            pw_stream_state_as_string(old), pw_stream_state_as_string(state));
    const bool streaming = (state == PW_STREAM_STATE_STREAMING);
    if (streaming == cam->streaming)
        return;
    cam->streaming = streaming;
    update_recording(cam);
}

/* The format was negotiated: parse it and declare a buffer pool of the matching size.
 * Without this PipeWire never allocates correctly-sized buffers and
 * pw_stream_dequeue_buffer() returns NULL forever, so on_process can never emit. */
static void on_param_changed(void *data, uint32_t id, const struct spa_pod *param)
{
    struct cam *cam = data;
    if (!param || id != SPA_PARAM_Format)
        return;

    uint32_t mtype, msubtype;
    if (spa_format_parse(param, &mtype, &msubtype) < 0 ||
        mtype != SPA_MEDIA_TYPE_video || msubtype != SPA_MEDIA_SUBTYPE_raw)
        return;

    struct spa_video_info_raw raw;
    spa_zero(raw);
    if (spa_format_video_raw_parse(param, &raw) < 0)
        return;
    if (raw.size.width)
        cam->fmt_w = raw.size.width;
    if (raw.size.height)
        cam->fmt_h = raw.size.height;

    uint32_t frame_bytes = cam->fmt_w * cam->fmt_h * 3 / 2;
    fprintf(stderr, "anland-camera: cam=%d format negotiated %ux%u (%ub), declaring buffers\n",
            cam->index, cam->fmt_w, cam->fmt_h, frame_bytes);

    uint8_t buffer[512];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const struct spa_pod *params[1];
    params[0] = spa_pod_builder_add_object(&b,
        SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
        SPA_PARAM_BUFFERS_buffers,  SPA_POD_CHOICE_RANGE_Int(4, 2, 8),
        SPA_PARAM_BUFFERS_blocks,   SPA_POD_Int(1),
        SPA_PARAM_BUFFERS_size,     SPA_POD_Int((int)frame_bytes),
        SPA_PARAM_BUFFERS_stride,   SPA_POD_Int((int)cam->fmt_w),
        SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(
            (1 << SPA_DATA_MemPtr) | (1 << SPA_DATA_MemFd)));
    pw_stream_update_params(cam->node, params, 1);
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = on_stream_state_changed,
    .param_changed = on_param_changed,
    .process = on_process,
};

static int connect_node(struct cam *cam)
{
    char name[32], desc[48];
    snprintf(name, sizeof(name), "anland-camera-%d", cam->index);
    snprintf(desc, sizeof(desc), "Anland remote camera %d", cam->index);

    cam->node = pw_stream_new(cam->owner->core, name,
        pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Video",
            PW_KEY_MEDIA_CATEGORY, "Capture",
            PW_KEY_MEDIA_CLASS, "Video/Source",
            PW_KEY_MEDIA_ROLE, "Camera",
            PW_KEY_NODE_NAME, name,
            PW_KEY_NODE_DESCRIPTION, desc,
            NULL));
    if (!cam->node)
        return -1;
    pw_stream_add_listener(cam->node, &cam->listener, &stream_events, cam);

    uint8_t buffer[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const struct spa_pod *params[1] = {
        build_video_format(&b, cam->fmt_w, cam->fmt_h, cam->fmt_code) };

    /* No DRIVER flag: the consuming app (graph) drives, so PipeWire calls on_process
     * on demand. No RT_PROCESS either, so on_process runs on the same loop thread as
     * the stream-socket reader and shares cam->frame without a lock. */
    return pw_stream_connect(cam->node, PW_DIRECTION_OUTPUT, PW_ID_ANY,
                             PW_STREAM_FLAG_MAP_BUFFERS,
                             params, 1);
}

/* Create the node for one camera slot (no fd attached yet). PipeWire drives its
 * .process whenever an app consumes it. */
static int create_node(struct cam *cam)
{
    if (cam->node)
        return 0;
    /* PipeWire may be unreachable (no daemon, or a sandboxed XDG_RUNTIME_DIR).
     * pw_stream_new() dereferences the core, so building a node without one would
     * crash; defer instead and let on_reconnect_timer() -> create_nodes() create it
     * once build_pw() succeeds. The caller still attaches the stream fd, so the
     * camera shows up late rather than not at all. */
    if (!cam->owner || !cam->owner->pw_connected || !cam->owner->core)
        return -1;
    cam->streaming = false;
    cam->recording = false;
    cam->process_seen = false;
    if (cam->fmt_w == 0 || cam->fmt_h == 0) {
        cam->fmt_w = CAM_DEF_W;
        cam->fmt_h = CAM_DEF_H;
    }
    /* The consumer always delivers NV21, so advertise it from the start -- no live
     * format switch (which crashes downstream gst). Only resolution ever renegotiates. */
    cam->fmt_code = CAM_FMT_NV21;
    return connect_node(cam);
}

/* (Re)create nodes for the current camera count, e.g. after a PipeWire reconnect.
 * The stream fds in cam->stream_fd are preserved, so readers are re-armed. */
static void create_nodes(struct anland_camera *c)
{
    if (!c->pw_connected || !c->core)
        return;
    struct pw_loop *loop = pw_thread_loop_get_loop(c->loop);
    for (int i = 0; i < c->num_cameras; i++) {
        struct cam *cam = &c->cams[i];
        if (cam->node)
            continue;
        create_node(cam);
        if (cam->stream_fd >= 0 && !cam->io)
            cam->io = pw_loop_add_io(loop, cam->stream_fd, SPA_IO_IN, false,
                                     on_stream_readable, cam);
    }
}

/* Destroy one camera's node and reader. Keeps stream_fd (caller decides). */
static void destroy_node(struct cam *cam)
{
    struct pw_loop *loop = cam->owner && cam->owner->loop
                               ? pw_thread_loop_get_loop(cam->owner->loop) : NULL;
    if (cam->node) {
        spa_hook_remove(&cam->listener);
        pw_stream_destroy(cam->node);
        cam->node = NULL;
    }
    if (cam->io && loop) {
        pw_loop_destroy_source(loop, cam->io);
        cam->io = NULL;
    }
    cam->streaming = false;
    cam->recording = false;
}

static void destroy_nodes(struct anland_camera *c)
{
    for (int i = 0; i < MAX_CAMERAS; i++)
        destroy_node(&c->cams[i]);
}

/* ---- PipeWire connection lifecycle (mirrors the audio engine) ---- */

static void arm_reconnect(struct anland_camera *c)
{
    struct timespec val = { .tv_sec = RECONNECT_SECS, .tv_nsec = 0 };
    pw_loop_update_timer(pw_thread_loop_get_loop(c->loop), c->reconnect_timer,
                         &val, NULL, false);
}

static void on_core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
    struct anland_camera *c = data;
    (void)seq;
    (void)message;
    if (id == PW_ID_CORE && res == -EPIPE) {
        c->pw_connected = false;
        arm_reconnect(c);
    }
}

static const struct pw_core_events core_events = {
    PW_VERSION_CORE_EVENTS,
    .error = on_core_error,
};

static int build_pw(struct anland_camera *c)
{
    c->core = pw_context_connect(c->context, NULL, 0);
    if (!c->core)
        return -1;
    pw_core_add_listener(c->core, &c->core_listener, &core_events, c);
    return 0;
}

static void teardown_pw(struct anland_camera *c)
{
    destroy_nodes(c);
    if (c->core) {
        spa_hook_remove(&c->core_listener);
        pw_core_disconnect(c->core);
        c->core = NULL;
    }
}

static void on_reconnect_timer(void *data, uint64_t expirations)
{
    struct anland_camera *c = data;
    (void)expirations;
    if (c->pw_connected)
        return;

    teardown_pw(c);
    if (build_pw(c) == 0) {
        c->pw_connected = true;
        create_nodes(c);   /* rebuild nodes from the still-valid stream fds */
    } else {
        teardown_pw(c);
        arm_reconnect(c);
    }
}

/* Round-trip GET_INFO over the control socket to learn each camera's max sensor
 * resolution, used to seed the initial node format. Blocking with a short timeout;
 * called off the PipeWire loop thread (so it never stalls the RT loop). Fills
 * w[i]/h[i], left at 0 when unknown. */
static void query_max_res(int ctrl_fd, uint32_t *w, uint32_t *h, int n)
{
    struct camera_ctrl_msg req = { .type = CAMERA_CTRL_GET_INFO, .reserved = 0, .len = 0 };
    if (send(ctrl_fd, &req, sizeof(req), MSG_NOSIGNAL) < 0)
        return;

    struct pollfd pfd = { .fd = ctrl_fd, .events = POLLIN };
    if (poll(&pfd, 1, 300) <= 0)
        return;

    uint8_t buf[sizeof(struct camera_ctrl_msg) + 1 + MAX_CAMERAS * 4];
    ssize_t r = recv(ctrl_fd, buf, sizeof(buf), 0);
    if (r < (ssize_t)sizeof(struct camera_ctrl_msg))
        return;
    struct camera_ctrl_msg *hdr = (struct camera_ctrl_msg *)buf;
    if (hdr->type != CAMERA_CTRL_INFO_REPLY)
        return;

    uint8_t *pl = buf + sizeof(struct camera_ctrl_msg);
    size_t avail = (size_t)r - sizeof(struct camera_ctrl_msg);
    if (avail < 1)
        return;
    int num = pl[0];
    size_t off = 1;
    for (int i = 0; i < num && i < n; i++) {
        if (off + 4 > avail)
            break;
        uint16_t w16, h16;
        memcpy(&w16, pl + off, sizeof(w16));
        memcpy(&h16, pl + off + 2, sizeof(h16));
        off += 4;
        w[i] = w16;
        h[i] = h16;
    }
}

/* ---- public API ---- */

void anland_camera_set_resources(int ctrl_fd, const int *stream_fds, int num_cameras)
{
    /* Lazily bring the engine up on the first resource delivery: the PipeWire
     * thread-loop is only started once a consumer actually hands over a camera,
     * never just because the backend is running. */
    if (!g_cam && anland_camera_start() < 0) {
        if (ctrl_fd >= 0)
            close(ctrl_fd);
        for (int i = 0; i < num_cameras; i++)
            if (stream_fds[i] >= 0)
                close(stream_fds[i]);
        return;
    }
    struct anland_camera *c = g_cam;
    if (num_cameras > MAX_CAMERAS)
        num_cameras = MAX_CAMERAS;

    /* Only worth the blocking GET_INFO round-trip when we'll actually create a NEW
     * node: existing (persistent) nodes keep their negotiated format across reconnects,
     * so re-querying for them would just waste a round-trip and discard the result. */
    bool need_query = false;
    for (int i = 0; i < num_cameras; i++) {
        if (!c->cams[i].node) {
            need_query = true;
            break;
        }
    }

    /* Done BEFORE taking the loop lock so the blocking round-trip never stalls the
     * RT loop. New nodes are then created at the real sensor resolution. */
    uint32_t qw[MAX_CAMERAS] = {0}, qh[MAX_CAMERAS] = {0};
    if (ctrl_fd >= 0 && need_query)
        query_max_res(ctrl_fd, qw, qh, num_cameras);

    pw_thread_loop_lock(c->loop);

    /* Swap in the new control socket. */
    if (c->ctrl_fd >= 0)
        close(c->ctrl_fd);
    c->ctrl_fd = ctrl_fd;

    /* Pop excess cameras if the new connection exposes fewer than before. */
    for (int i = num_cameras; i < c->num_cameras; i++) {
        destroy_node(&c->cams[i]);
        detach_fd(&c->cams[i]);   /* closes the now-stale stream fd */
    }

    /* Create any newly-appeared cameras, then (re)attach every live one's stream fd.
     * Existing nodes are kept (apply-in-place, like the audio engine hot-swaps its
     * socket) so consuming apps never see the webcam disappear across a reconnect. */
    c->num_cameras = num_cameras;
    for (int i = 0; i < num_cameras; i++) {
        struct cam *cam = &c->cams[i];
        cam->owner = c;
        cam->index = i;
        /* Seed a NEW node's format from the reported sensor max. For an existing node we
         * must not touch fmt_w/h -- it would desync on_process's stride from the node's
         * live negotiated format. (No upper cap: we use the camera's real max; only a
         * missing/zero reply leaves create_node's 720p default in place.) */
        if (!cam->node && qw[i] >= 64 && qh[i] >= 64) {
            cam->fmt_w = qw[i];
            cam->fmt_h = qh[i];
        }
        create_node(cam);
        attach_fd(cam, stream_fds[i]);
    }

    pw_thread_loop_unlock(c->loop);
}

void anland_camera_clear(void)
{
    struct anland_camera *c = g_cam;
    if (!c)
        return;

    pw_thread_loop_lock(c->loop);
    /* Consumer is gone: drop the control socket and detach every camera's stream fd.
     * The nodes stay alive and keep emitting blank frames, so PipeWire and any
     * capturing app never see the cameras disappear -- the audio engine's detach
     * behaviour. */
    if (c->ctrl_fd >= 0) {
        close(c->ctrl_fd);
        c->ctrl_fd = -1;
    }
    for (int i = 0; i < c->num_cameras; i++) {
        c->cams[i].recording = false;   /* the consumer that we told to record is gone */
        detach_fd(&c->cams[i]);
    }
    pw_thread_loop_unlock(c->loop);
}

int anland_camera_start(void)
{
    if (g_cam)
        return 0;

    pw_init(NULL, NULL);

    struct anland_camera *c = calloc(1, sizeof(*c));
    if (!c)
        return -1;
    c->ctrl_fd = -1;
    for (int i = 0; i < MAX_CAMERAS; i++) {
        c->cams[i].stream_fd = -1;
        c->cams[i].shm_fd = -1;
    }

    c->loop = pw_thread_loop_new("anland-camera", NULL);
    if (!c->loop)
        goto fail;

    c->context = pw_context_new(pw_thread_loop_get_loop(c->loop), NULL, 0);
    if (!c->context)
        goto fail;

    c->reconnect_timer = pw_loop_add_timer(pw_thread_loop_get_loop(c->loop),
                                           on_reconnect_timer, c);
    if (!c->reconnect_timer)
        goto fail;

    if (pw_thread_loop_start(c->loop) < 0)
        goto fail;

    pw_thread_loop_lock(c->loop);
    if (build_pw(c) == 0) {
        c->pw_connected = true;
    } else {
        teardown_pw(c);
        arm_reconnect(c);
    }
    pw_thread_loop_unlock(c->loop);

    g_cam = c;
    return 0;

fail:
    if (c->reconnect_timer)
        pw_loop_destroy_source(pw_thread_loop_get_loop(c->loop), c->reconnect_timer);
    if (c->context)
        pw_context_destroy(c->context);
    if (c->loop)
        pw_thread_loop_destroy(c->loop);
    free(c);
    pw_deinit();
    return -1;
}

void anland_camera_stop(void)
{
    struct anland_camera *c = g_cam;
    if (!c)
        return;
    g_cam = NULL;

    if (c->loop)
        pw_thread_loop_lock(c->loop);
    teardown_pw(c);
    for (int i = 0; i < MAX_CAMERAS; i++) {
        struct cam *cam = &c->cams[i];
        if (cam->stream_fd >= 0) {
            close(cam->stream_fd);
            cam->stream_fd = -1;
        }
        unmap_shm(cam);
        free(cam->frame);
        cam->frame = NULL;
    }
    if (c->ctrl_fd >= 0) {
        close(c->ctrl_fd);
        c->ctrl_fd = -1;
    }
    if (c->reconnect_timer)
        pw_loop_destroy_source(pw_thread_loop_get_loop(c->loop), c->reconnect_timer);
    if (c->loop)
        pw_thread_loop_unlock(c->loop);

    if (c->loop)
        pw_thread_loop_stop(c->loop);
    if (c->context)
        pw_context_destroy(c->context);
    if (c->loop)
        pw_thread_loop_destroy(c->loop);
    free(c);
    pw_deinit();
}