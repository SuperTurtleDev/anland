#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* for recvmsg()/SCM_RIGHTS; DE builds already define it */
#endif
#include "display_producer.h"
#include "socket_utils.h"

#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>

/* Consumer discovery has no wall-clock deadline. Deferred callers keep every
 * partial record and descriptor, and advance it without blocking their loop. */
static int64_t monotonic_ms(void);

/* One bounded stream record; ancillary ownership survives short reads. */
struct handshake_record {
    unsigned char bytes[sizeof(struct data_msg) + MAX_BUFS * sizeof(struct buf_info)];
    size_t used;
    int fds[MAX_BUFS], fd_count;
};

#define OUTPUT_QUEUE_MAX (2U * 1024U * 1024U)
#define OUTPUT_PAYLOAD_MAX (1024U * 1024U)

/* One writer/order per data channel. A queued frame remains accounted until its
 * last byte is sent; on session teardown no suffix can survive into a new one. */
struct output_message {
    struct output_message *next;
    size_t size, offset;
    unsigned char bytes[];
};

struct display_ctx {
    int      ctrl_fd;
    int      data_fd;
    int      buf_ready_efd;
    int      fence_fd;        /* write end of the dedicated render-done fence channel */
    int      shm_fd;
    int      audio_fd;        /* local end of the bidirectional audio socketpair (hello slot 4) */
    int      pending_render_fence; /* render-done fence for the in-flight frame */
    volatile uint32_t *shm_ptr;
    uint32_t screen_w, screen_h;
    uint32_t pixel_format;
    uint32_t refresh;
    bool     fallback;
    bool     have_screen_info, pickup_requested;
    unsigned char screen_bytes[sizeof(struct ctrl_msg) + sizeof(struct screen_info)];
    size_t screen_used;
    size_t pickup_sent;
    struct handshake_record pickup, buffers;

    int      dmabuf_fds[MAX_BUFS];
    struct buf_info dmabuf_infos[MAX_BUFS];
    int      buf_count;
    // Input framing state survives partial socket reads; owned by this context.
    unsigned char *input_bytes;
    size_t input_size, input_used;
    int input_kind;
    int input_fds[64], input_fd_count;
    int64_t input_deadline;
    struct output_message *output_head, *output_tail;
    size_t output_bytes;


    void (*pre_release_cb)(void *);
    void  *pre_release_userdata;
    void (*fallback_cb)(void *);
    void  *fallback_userdata;
};

/*
 * Release every consumer-side resource (dmabuf fds, the five picked-up fds and the
 * shm mapping), leaving the context holding only the daemon ctrl_fd. Does NOT touch
 * the fallback flag or fire the fallback callback — callers decide that. Idempotent.
 */
static void clear_output_queue(display_ctx *ctx)
{
    while (ctx->output_head) {
        struct output_message *message = ctx->output_head;
        ctx->output_head = message->next;
        free(message);
    }
    ctx->output_tail = NULL;
    ctx->output_bytes = 0;
}

static void release_consumer_resources(display_ctx *ctx)
{
    clear_output_queue(ctx);
    for (int i = 0; i < ctx->pickup.fd_count; ++i) close(ctx->pickup.fds[i]);
    for (int i = 0; i < ctx->buffers.fd_count; ++i) close(ctx->buffers.fds[i]);
    memset(&ctx->pickup, 0, sizeof(ctx->pickup));
    memset(&ctx->buffers, 0, sizeof(ctx->buffers));
    ctx->pickup_requested = false;
    ctx->pickup_sent = 0;
    for (int i = 0; i < ctx->buf_count; i++) {
        if (ctx->dmabuf_fds[i] >= 0) {
            close(ctx->dmabuf_fds[i]);
            ctx->dmabuf_fds[i] = -1;
        }
    }
    ctx->buf_count = 0;
    free(ctx->input_bytes); ctx->input_bytes = NULL;
    ctx->input_size = ctx->input_used = 0; ctx->input_kind = 0;
    for (int i = 0; i < ctx->input_fd_count; ++i) close(ctx->input_fds[i]);
    ctx->input_fd_count = 0;


    if (ctx->data_fd >= 0)          { close(ctx->data_fd);          ctx->data_fd = -1; }
    if (ctx->buf_ready_efd >= 0)    { close(ctx->buf_ready_efd);    ctx->buf_ready_efd = -1; }
    if (ctx->fence_fd >= 0)         { close(ctx->fence_fd);         ctx->fence_fd = -1; }
    if (ctx->audio_fd >= 0)         { close(ctx->audio_fd);         ctx->audio_fd = -1; }
    if (ctx->pending_render_fence >= 0) { close(ctx->pending_render_fence); ctx->pending_render_fence = -1; }
    if (ctx->shm_ptr)               { munmap((void *)ctx->shm_ptr, sizeof(uint32_t)); ctx->shm_ptr = NULL; }
    if (ctx->shm_fd >= 0)           { close(ctx->shm_fd);           ctx->shm_fd = -1; }
}

static void enter_fallback(display_ctx *ctx)
{
    if (ctx->fallback)
        return;
    ctx->fallback = true;

    if (ctx->pre_release_cb)
        ctx->pre_release_cb(ctx->pre_release_userdata);

    release_consumer_resources(ctx);

    if (ctx->fallback_cb)
        ctx->fallback_cb(ctx->fallback_userdata);
}

/* 1 complete, 0 pending, -1 fatal. Never wait in a compositor callback.
 * recvmsg on every chunk catches split/extra SCM_RIGHTS, including on payload.
 * No prefix or descriptor is discarded on EAGAIN. Caller owns fatal cleanup. */
static int handshake_read(int fd, struct handshake_record *r, size_t total)
{
    for (unsigned budget = 0; budget < 16 && r->used < total; ++budget) {
        struct iovec iov = {r->bytes + r->used, total - r->used};
        union { struct cmsghdr align; char bytes[CMSG_SPACE(MAX_BUFS * sizeof(int))]; } control;
        struct msghdr msg = {.msg_iov=&iov, .msg_iovlen=1,
            .msg_control=control.bytes, .msg_controllen=sizeof(control.bytes)};
        ssize_t n = recvmsg(fd, &msg, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        if (n <= 0) { if (!n) errno=ECONNRESET; return -1; }
        bool bad = msg.msg_flags & (MSG_CTRUNC | MSG_TRUNC);
        for (struct cmsghdr *c=CMSG_FIRSTHDR(&msg); c; c=CMSG_NXTHDR(&msg,c)) {
            if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) {
                bad=true; continue;
            }
            size_t bytes=c->cmsg_len-CMSG_LEN(0);
            if (bytes % sizeof(int)) bad=true;
            int *fds=(int *)CMSG_DATA(c);
            for (size_t i=0; i<bytes/sizeof(int); ++i) {
                if (r->fd_count < MAX_BUFS) r->fds[r->fd_count++]=fds[i];
                else { close(fds[i]); bad=true; }
            }
        }
        r->used+=(size_t)n;
        if (bad) { errno=EPROTO; return -1; }
    }
    return r->used >= total;
}

static int pickup_fds(display_ctx *ctx)
{
    if (ctx->data_fd >= 0) return 1;
    struct ctrl_msg request = {.type=CTRL_MSG_PICKUP_FDS, .size=0};
    if (ctx->pickup_sent < sizeof(request)) {
        ssize_t n=send(ctx->ctrl_fd, (unsigned char *)&request+ctx->pickup_sent,
            sizeof(request)-ctx->pickup_sent, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return 0;
        if (n <= 0) return -1;
        ctx->pickup_sent+=(size_t)n;
        if (ctx->pickup_sent != sizeof(request)) return 0;
        ctx->pickup_requested=true;
    }
    int rc=handshake_read(ctx->ctrl_fd, &ctx->pickup, sizeof(struct ctrl_msg));
    if (rc != 1) return rc;
    struct ctrl_msg resp;
    memcpy(&resp,ctx->pickup.bytes,sizeof(resp));
    if (resp.type != CTRL_MSG_FDS_READY || resp.size != 0 || ctx->pickup.fd_count != 5) {
        errno=EPROTO; return -1;
    }
    /* { buf_ready, fence, data, shm, audio }; ownership transfers exactly once. */
    ctx->buf_ready_efd=ctx->pickup.fds[0];
    ctx->fence_fd=ctx->pickup.fds[1];
    ctx->data_fd=ctx->pickup.fds[2];
    ctx->shm_fd=ctx->pickup.fds[3];
    ctx->audio_fd=ctx->pickup.fds[4];
    ctx->pickup.fd_count=0;
    ctx->pickup_requested=false;
    ctx->shm_ptr=mmap(NULL,sizeof(uint32_t),PROT_READ,MAP_SHARED,ctx->shm_fd,0);
    if (ctx->shm_ptr == MAP_FAILED) { ctx->shm_ptr=NULL; return -1; }
    return 1;
}

static int receive_dmabufs(display_ctx *ctx)
{
    if (ctx->buf_count > 0) return 1;
    int rc=handshake_read(ctx->data_fd,&ctx->buffers,sizeof(struct data_msg));
    if (rc != 1) return rc;
    struct data_msg header;
    memcpy(&header,ctx->buffers.bytes,sizeof(header));
    if (header.type != DATA_MSG_BUFS_READY || header.size == 0 ||
        header.size % sizeof(struct buf_info) ||
        header.size > MAX_BUFS * sizeof(struct buf_info)) { errno=EPROTO; return -1; }
    rc=handshake_read(ctx->data_fd,&ctx->buffers,sizeof(header)+header.size);
    if (rc != 1) return rc;
    int count=(int)(header.size/sizeof(struct buf_info));
    if (count != ctx->buffers.fd_count) { errno=EPROTO; return -1; }
    for (int i=0; i<count; ++i) {
        ctx->dmabuf_fds[i]=ctx->buffers.fds[i];
        memcpy(&ctx->dmabuf_infos[i],ctx->buffers.bytes+sizeof(header)+i*sizeof(struct buf_info),
            sizeof(struct buf_info));
    }
    ctx->buffers.fd_count=0;
    ctx->buf_count=count;
    return 1;
}

/* Keep the daemon HELLO live while its display connector is absent. Partial
 * screen-info bytes are owned here, never discarded/reinterpreted by a WM. */
static int receive_screen_info(display_ctx *ctx, int timeout_ms)
{
    if (ctx->have_screen_info) return 1;
    struct pollfd pfd = {.fd=ctx->ctrl_fd, .events=POLLIN};
    int rc = poll(&pfd, 1, timeout_ms);
    if (rc < 0) return errno == EINTR ? 0 : -1;
    if (rc == 0) return 0;
    while (ctx->screen_used < sizeof(ctx->screen_bytes)) {
        ssize_t n = recv(ctx->ctrl_fd, ctx->screen_bytes + ctx->screen_used,
            sizeof(ctx->screen_bytes) - ctx->screen_used, MSG_DONTWAIT);
        if (n > 0) {
            ctx->screen_used += (size_t)n; continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
        return -1;
    }
    struct ctrl_msg hdr;
    struct screen_info si;
    memcpy(&hdr, ctx->screen_bytes, sizeof(hdr));
    memcpy(&si, ctx->screen_bytes + sizeof(hdr), sizeof(si));
    if (hdr.type != CTRL_MSG_SCREEN_INFO || hdr.size != sizeof(si) ||
        !si.width || !si.height) { errno=EPROTO; return -1; }
    ctx->screen_w=si.width; ctx->screen_h=si.height;
    ctx->pixel_format=si.format; ctx->refresh=si.refresh;
    ctx->have_screen_info=true;
    return 1;
}

static int connect_daemon(display_ctx **out, const char *socket_path, bool deferred)
{
    display_ctx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return -1;

    ctx->ctrl_fd = -1;
    ctx->data_fd = -1;
    ctx->buf_ready_efd = -1;
    ctx->fence_fd = -1;
    ctx->shm_fd = -1;
    ctx->audio_fd = -1;
    ctx->pending_render_fence = -1;
    ctx->shm_ptr = NULL;
    ctx->fallback = true; // stay in fallback until try_exit_fallback() succeeds
    for (int i = 0; i < MAX_BUFS; i++)
        ctx->dmabuf_fds[i] = -1;

    ctx->ctrl_fd = connect_unix(socket_path);
    if (ctx->ctrl_fd < 0)
        goto fail;
    // Bound HELLO writes only; readiness reads use poll + MSG_DONTWAIT.
    struct timeval io_timeout = {.tv_sec=0, .tv_usec=100000};
    if (setsockopt(ctx->ctrl_fd, SOL_SOCKET, SO_RCVTIMEO, &io_timeout, sizeof(io_timeout)) < 0 ||
        setsockopt(ctx->ctrl_fd, SOL_SOCKET, SO_SNDTIMEO, &io_timeout, sizeof(io_timeout)) < 0)
        goto fail;

    struct ctrl_msg hdr = { .type = CTRL_MSG_PRODUCER_HELLO, .size = 0 };
    if (send_all(ctx->ctrl_fd, &hdr, sizeof(hdr)) < 0)
        goto fail;

    int ready;
    do {
        ready = receive_screen_info(ctx, deferred ? 0 : -1);
    } while (!deferred && ready == 0);
    if (ready < 0) goto fail;

    // Daemon handshake only: geometry may be deferred; consumer fds and
    // dmabufs are deliberately left for try_exit_fallback() so the backend brings
    // the consumer up through the single reconnect path. Stay in fallback.
    *out = ctx;
    return 0;

fail:
    if (ctx->ctrl_fd >= 0)
        close(ctx->ctrl_fd);
    free(ctx);
    return -1;
}

int connect_to_deamon(display_ctx **out, const char *socket_path)
{
    return connect_daemon(out, socket_path, false);
}
int connect_to_deamon_deferred(display_ctx **out, const char *socket_path)
{
    return connect_daemon(out, socket_path, true);
}

void disconnect(display_ctx *ctx)
{
    if (!ctx)
        return;
    release_consumer_resources(ctx);
    if (ctx->ctrl_fd >= 0)
        close(ctx->ctrl_fd);
    free(ctx);
}

int get_screen_info(display_ctx *ctx, uint32_t *width, uint32_t *height, uint32_t *format, uint32_t *refresh)
{
    *width  = ctx->screen_w;
    *height = ctx->screen_h;
    *format = ctx->pixel_format;
    *refresh = ctx->refresh;
    return 0;
}

/* Stash the render-done fence for the in-flight frame (created in doEndFrame).
 * trigger_refresh sends it to the consumer. Closes any previous unconsumed stash. */
void set_render_fence(display_ctx *ctx, int fence_fd)
{
    if (ctx->pending_render_fence >= 0)
        close(ctx->pending_render_fence);
    ctx->pending_render_fence = fence_fd;
}

int trigger_refresh(display_ctx *ctx)
{
    if (ctx->fallback) {
        if (ctx->pending_render_fence >= 0) {
            close(ctx->pending_render_fence);
            ctx->pending_render_fence = -1;
        }
        return 0;
    }

    /* Send exactly one render-done message per frame on the dedicated fence channel
     * (producer->consumer). The message itself is the "frame rendered" signal -- no
     * separate eventfd, no cross-channel ordering. The render fence rides as
     * SCM_RIGHTS ancillary data when we have one; otherwise a bare byte is sent so
     * the consumer's per-frame recv always has exactly one message (it then queues
     * with -1). The consumer hands the fence to queueBuffer -> SurfaceFlinger waits
     * GPU-side. NON-BLOCKING: this runs on kwin's main thread, which must never block
     * on our socket. In lockstep the consumer always drains, so the one-message
     * buffer never fills; a momentary miss self-heals via the consumer's 5s
     * poll->fallback rather than freezing the compositor. data_fd's reverse direction
     * is intentionally left unused (reserved for future extension). */
    char b = 0;
    struct iovec iov = { .iov_base = &b, .iov_len = 1 };
    union {
        char buf[CMSG_SPACE(sizeof(int))];
        struct cmsghdr align;
    } cmsg;
    struct msghdr msg = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
    };
    if (ctx->pending_render_fence >= 0) {
        msg.msg_control = cmsg.buf;
        msg.msg_controllen = sizeof(cmsg.buf);
        struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int));
        memcpy(CMSG_DATA(c), &ctx->pending_render_fence, sizeof(int));
    }
    const ssize_t sent = sendmsg(ctx->fence_fd, &msg, MSG_NOSIGNAL | MSG_DONTWAIT);
    if (sent != (ssize_t)iov.iov_len) {
        enter_fallback(ctx);
        return -1;
    }
    if (ctx->pending_render_fence >= 0) {
        close(ctx->pending_render_fence);
        ctx->pending_render_fence = -1;
    }
    return 0;
}

static int64_t monotonic_ms(void)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

/* A zero timeout really is non-blocking. Partial bytes/fds remain context-owned
 * until the complete frame is available. A framing error ends the session rather
 * than ever parsing leftover payload as an event. Callers serialize the context. */
static int input_step(display_ctx *ctx, void *out, size_t size, int kind, int timeout_ms)
{
    if (ctx->fallback) return 0;
    if (!ctx->input_bytes) {
        if (!size || size > 1024u * 1024u) { enter_fallback(ctx); return -1; }
        ctx->input_bytes = malloc(size);
        if (!ctx->input_bytes) { enter_fallback(ctx); return -1; }
        ctx->input_size = size; ctx->input_used = 0; ctx->input_kind = kind;
        ctx->input_deadline = kind == 1 ? 0 : monotonic_ms()+5000;
        // An idle connection is not incomplete; an announced payload is.
    }
    if (ctx->input_size != size || ctx->input_kind != kind ||
        (ctx->input_deadline && monotonic_ms() >= ctx->input_deadline)) { enter_fallback(ctx); return -1; }
    struct pollfd pfd = { .fd=ctx->data_fd, .events=POLLIN };
    // Never extend the frame's absolute deadline, even for timeout=-1.
    int remaining = ctx->input_deadline ? (int)(ctx->input_deadline - monotonic_ms()) : 5000;
    int wait = timeout_ms < 0 || timeout_ms > remaining ? remaining : timeout_ms;
    int ready = poll(&pfd, 1, wait);
    if (ready < 0) {
        if (errno == EINTR) return 0;
        enter_fallback(ctx); return -1;
    }
    if (!ready) return 0;
    for (int budget = 0; budget < 16 && ctx->input_used < size; ++budget) {
        ssize_t n;
        if (kind == 3) {
            struct iovec iov = {ctx->input_bytes + ctx->input_used, size - ctx->input_used};
            union { struct cmsghdr align; char bytes[CMSG_SPACE(64 * sizeof(int))]; } control;
            struct msghdr msg = {.msg_iov=&iov, .msg_iovlen=1,
                .msg_control=control.bytes, .msg_controllen=sizeof(control.bytes)};
            n = recvmsg(ctx->data_fd, &msg, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
            bool bad = msg.msg_flags & (MSG_CTRUNC | MSG_TRUNC);
            if (n > 0) {
                for (struct cmsghdr *c=CMSG_FIRSTHDR(&msg); c; c=CMSG_NXTHDR(&msg,c)) {
                    if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
                    size_t count=(c->cmsg_len-CMSG_LEN(0))/sizeof(int);
                    int *fds=(int *)CMSG_DATA(c);
                    for (size_t i=0; i<count; ++i) {
                        if (ctx->input_fd_count < 64) ctx->input_fds[ctx->input_fd_count++]=fds[i];
                        else { close(fds[i]); bad=true; }
                    }
                }
                if (bad) { enter_fallback(ctx); return -1; }
            }
        } else {
            n = recv(ctx->data_fd, ctx->input_bytes + ctx->input_used,
                     size - ctx->input_used, MSG_DONTWAIT);
        }
        if (n > 0) {
            if (!ctx->input_deadline) ctx->input_deadline=monotonic_ms()+5000;
            ctx->input_used += (size_t)n;
        }
        else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) break;
        else { enter_fallback(ctx); return -1; }
    }
    if (ctx->input_used != size) return 0;
    memcpy(out, ctx->input_bytes, size);
    free(ctx->input_bytes); ctx->input_bytes=NULL;
    ctx->input_size=ctx->input_used=0; ctx->input_kind=0;
    return 1;
}

static int input_read(display_ctx *ctx, void *out, size_t size, int kind, int timeout_ms)
{
    if (ctx->fallback) return 0;
    int64_t deadline=monotonic_ms()+(timeout_ms < 0 ? 5000 : timeout_ms);
    do {
        int remaining=timeout_ms == 0 ? 0 : (int)(deadline-monotonic_ms());
        if (remaining < 0) remaining=0;
        int rc=input_step(ctx,out,size,kind,remaining);
        if (rc != 0 || timeout_ms == 0) return rc;
        if (monotonic_ms() >= deadline) {
            if (ctx->input_used) { enter_fallback(ctx); return -1; }
            return 0;
        }
    } while (true);
}

bool input_header_pending(const display_ctx *ctx)
{
    return ctx && !ctx->fallback && ctx->input_kind == 1 && ctx->input_used > 0;
}

int poll_input_event(display_ctx *ctx, struct InputEvent *event, int timeout_ms)
{
    uint8_t bytes[sizeof(struct data_msg) + sizeof(struct InputEvent)];
    int rc = input_read(ctx, bytes, sizeof(bytes), 1, timeout_ms);
    if (rc <= 0) return rc;
    struct data_msg hdr; memcpy(&hdr, bytes, sizeof(hdr));
    if (hdr.type != DATA_MSG_INPUT_EVENT || hdr.size != sizeof(*event)) {
        enter_fallback(ctx); return -1;
    }
    memcpy(event, bytes + sizeof(hdr), sizeof(*event));
    /* SCREEN_INFO can precede Android's first refresh callback. Keep the
     * consumer's later, validated display rate visible through get_screen_info
     * without swallowing/reordering its input notification or payloads. */
    if (event->type == INPUT_TYPE_DISPLAY_REFRESH &&
        event->display.refresh_mhz > 0 && event->display.refresh_mhz <= INT32_MAX)
        ctx->refresh = event->display.refresh_mhz;
    return 1;
}
int poll_input_event_extend_fds(display_ctx *ctx, int *fds, int max_fds,
                                int *fd_count, int timeout_ms)
{
    *fd_count=0;
    struct data_msg hdr;
    int rc=input_read(ctx, &hdr, sizeof(hdr), 3, timeout_ms);
    if (rc <= 0) return rc;
    if (hdr.type != DATA_MSG_INPUT_EXTEND_FDS || hdr.size != 0 || max_fds < 0 || ctx->input_fd_count > max_fds) {
        enter_fallback(ctx); return -1;
    }
    *fd_count=ctx->input_fd_count;
    memcpy(fds,ctx->input_fds,(size_t)*fd_count * sizeof(int));
    ctx->input_fd_count=0; return 1;
}

size_t pending_output_bytes(const display_ctx *ctx)
{
    return ctx && !ctx->fallback ? ctx->output_bytes : 0;
}

/* A work budget keeps even a constantly draining consumer from monopolising a
 * compositor callback. Pending does not mean failure and never drops a session. */
int flush_queued_output(display_ctx *ctx)
{
    if (!ctx || ctx->fallback) { errno = ENOTCONN; return -1; }
    for (unsigned budget = 0; budget < 16 && ctx->output_head; ++budget) {
        struct output_message *message = ctx->output_head;
        ssize_t n = send(ctx->data_fd, message->bytes + message->offset,
                         message->size - message->offset,
                         MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n > 0) {
            message->offset += (size_t)n;
            if (message->offset != message->size) continue;
            ctx->output_bytes -= message->size;
            ctx->output_head = message->next;
            if (!ctx->output_head) ctx->output_tail = NULL;
            free(message);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 1;
        enter_fallback(ctx);
        return -1;
    }
    return ctx->output_head ? 1 : 0;
}

int queue_output_event_with_length(display_ctx *ctx,
                                  const struct OutputEvent *event,
                                  const void *payload, size_t size)
{
    if (!ctx || ctx->fallback) { errno = ENOTCONN; return -1; }
    if (!event || size > OUTPUT_PAYLOAD_MAX || (size && !payload) ||
        (event->type == OUTPUT_TYPE_CLIPBOARD && event->clipboard.size != size) ||
        (event->type != OUTPUT_TYPE_CLIPBOARD && size != 0)) {
        errno = EINVAL;
        return -1;
    }
    const size_t total = sizeof(struct data_msg) + sizeof(*event) + size;
    if (total > OUTPUT_QUEUE_MAX - ctx->output_bytes) {
        errno = ENOBUFS; /* Reject atomically: the existing queue remains valid. */
        return -1;
    }
    struct output_message *message = malloc(sizeof(*message) + total);
    if (!message) return -1;
    message->next = NULL;
    message->size = total;
    message->offset = 0;
    struct data_msg header = {.type = DATA_MSG_OUTPUT_EVENT, .size = sizeof(*event)};
    memcpy(message->bytes, &header, sizeof(header));
    memcpy(message->bytes + sizeof(header), event, sizeof(*event));
    if (size) memcpy(message->bytes + sizeof(header) + sizeof(*event), payload, size);
    if (ctx->output_tail) ctx->output_tail->next = message;
    else ctx->output_head = message;
    ctx->output_tail = message;
    ctx->output_bytes += total;
    return 0;
}

int queue_clipboard(display_ctx *ctx, const void *text, size_t size)
{
    if (size > OUTPUT_PAYLOAD_MAX || (size && !text)) { errno = EINVAL; return -1; }
    struct OutputEvent event;
    memset(&event, 0, sizeof(event));
    event.type = OUTPUT_TYPE_CLIPBOARD;
    event.clipboard.size = (uint32_t)size;
    return queue_output_event_with_length(ctx, &event, text, size);
}

// A bounded synchronous send preserves the existing API's completed-write
// contract. On partial failure, detach: continuing would corrupt stream framing.
static int output_send(display_ctx *ctx, const void *bytes, size_t size)
{
    size_t offset=0;
    int64_t deadline=monotonic_ms()+10;
    /* Existing complete-write APIs must not overtake an async message's prefix.
     * Preserve their old deadline/error contract; async users drive flush alone. */
    while (ctx->output_head) {
        int rc = flush_queued_output(ctx);
        if (rc < 0) return -1;
        if (rc == 0) break;
        int remaining = (int)(deadline - monotonic_ms());
        struct pollfd pfd = {.fd = ctx->data_fd, .events = POLLOUT};
        if (remaining <= 0 || poll(&pfd, 1, remaining) <= 0 ||
            !(pfd.revents & POLLOUT)) {
            enter_fallback(ctx);
            return -1;
        }
    }
    while (offset < size) {
        if (monotonic_ms() >= deadline) { enter_fallback(ctx); return -1; }
        ssize_t n=send(ctx->data_fd,(const char *)bytes+offset,size-offset,MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n > 0) { offset+=(size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            int remaining=(int)(deadline-monotonic_ms());
            struct pollfd pfd={.fd=ctx->data_fd,.events=POLLOUT};
            if (remaining > 0 && poll(&pfd,1,remaining)>0 && (pfd.revents & POLLOUT)) continue;
        }
        enter_fallback(ctx); return -1;
    }
    return 0;
}

int push_resources_request(display_ctx *ctx, uint32_t service_type, const uint32_t *args)
{
    struct OutputEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.type = OUTPUT_TYPE_RESOURCES_REQUEST;
    ev.resources_request.type = service_type;
    if (args) {
        ev.resources_request.args[0] = args[0];
        ev.resources_request.args[1] = args[1];
        ev.resources_request.args[2] = args[2];
    }
    return push_output_event(ctx, &ev);
}

int push_output_event(display_ctx *ctx, const struct OutputEvent *event)
{
    if (ctx->fallback)
        return 0;

    struct data_msg hdr = { .type = DATA_MSG_OUTPUT_EVENT, .size = sizeof(struct OutputEvent) };
    uint8_t msg[sizeof(struct data_msg) + sizeof(struct OutputEvent)];
    memcpy(msg, &hdr, sizeof(hdr));
    memcpy(msg + sizeof(hdr), event, sizeof(*event));

    if (output_send(ctx, msg, sizeof(msg)) < 0) {
        enter_fallback(ctx);
        return -1;
    }
    return 0;
}
int push_output_event_with_length(display_ctx *ctx, const struct OutputEvent *event, void* payload, size_t size)
{
    if (ctx->fallback)
        return 0;

    struct data_msg hdr = { .type = DATA_MSG_OUTPUT_EVENT, .size = sizeof(struct OutputEvent) };
    const size_t msg_size = sizeof(struct data_msg) + sizeof(struct OutputEvent) + size;
    uint8_t *msg = (uint8_t *)malloc(msg_size);
    if (!msg)
        return -1;
    memcpy(msg, &hdr, sizeof(hdr));
    memcpy(msg + sizeof(hdr), event, sizeof(*event));
    if (size > 0)
        memcpy(msg + sizeof(hdr) + sizeof(struct OutputEvent), payload, size);

    if (output_send(ctx, msg, msg_size) < 0) {
        free(msg);
        enter_fallback(ctx);
        return -1;
    }
    free(msg);
    return 0;
}
int poll_input_event_extend_data(display_ctx *ctx, void *payload, size_t size, int timeout_ms)
{
    if (ctx->fallback)
        return 0;
    /* Empty text/clipboard events have no bytes to consume. Preserve the legacy
     * completed-read contract without allocating or detaching the session. */
    if (size == 0)
        return 1;
    return input_read(ctx, payload, size, 2, timeout_ms);
}
int set_pre_release_callback(display_ctx *ctx, void (*on_pre_release)(void *), void *userdata)
{
    ctx->pre_release_cb = on_pre_release;
    ctx->pre_release_userdata = userdata;
    return 0;
}

int set_fallback_callback(display_ctx *ctx, void (*on_fallback)(void *), void *userdata)
{
    ctx->fallback_cb = on_fallback;
    ctx->fallback_userdata = userdata;
    return 0;
}

bool is_fallback(display_ctx *ctx)
{
    return ctx->fallback;
}

bool is_daemon_alive(display_ctx *ctx)
{
    if (!ctx || ctx->ctrl_fd < 0)
        return false;

    /* Non-blocking liveness probe: a healthy idle daemon connection reports no
     * revents at all, a closed/restarted one reports POLLHUP (or POLLERR when the
     * peer went away mid-write). Nothing is consumed from the socket. */
    struct pollfd pfd = { .fd = ctx->ctrl_fd, .events = POLLIN };
    if (poll(&pfd, 1, 0) < 0)
        return false;

    return !(pfd.revents & (POLLHUP | POLLERR | POLLNVAL));
}

int poll_buffer_ready(display_ctx *ctx, int timeout_ms)
{
    if (!ctx) { errno = EINVAL; return -1; }
    if (ctx->fallback) return 0;
    /* One poll serves both ACK/initial-target readiness and peer health.
     * eventfd alone cannot report a consumer closing before its first target.
     * Only poll socket hangups: never steal input or ancillary bytes. */
    struct pollfd channels[] = {
        {.fd = ctx->buf_ready_efd, .events = POLLIN},
        {.fd = ctx->ctrl_fd, .events = POLLRDHUP},
        {.fd = ctx->data_fd, .events = POLLRDHUP},
    };
    int rc = poll(channels, sizeof(channels) / sizeof(channels[0]), timeout_ms);
    if (rc < 0 && errno == EINTR) return 0;
    int error = rc < 0 ? errno : ECONNRESET;
    bool lost = rc < 0;
    for (size_t i = 0; i < sizeof(channels) / sizeof(channels[0]); ++i) {
        lost |= channels[i].fd < 0 ||
            (channels[i].revents & (POLLRDHUP | POLLHUP | POLLERR | POLLNVAL)) != 0;
    }
    if (!lost) return (channels[0].revents & POLLIN) != 0;
    enter_fallback(ctx);
    errno = error;
    return -1;
}
void force_fallback(display_ctx *ctx)
{
    if (!ctx) return;
    if (ctx->fallback) {
        /* An abandoned outstanding control response cannot become a new attempt. */
        bool outstanding=ctx->data_fd < 0 && (ctx->pickup_sent || ctx->pickup.used);
        release_consumer_resources(ctx);
        if (outstanding && ctx->ctrl_fd >= 0) { close(ctx->ctrl_fd); ctx->ctrl_fd=-1; }
        return;
    }

    ctx->fallback = true;
    if (ctx->pre_release_cb)
        ctx->pre_release_cb(ctx->pre_release_userdata);
    release_consumer_resources(ctx);
}

int try_exit_fallback(display_ctx *ctx)
{
    if (!ctx || ctx->ctrl_fd < 0) { errno=ENOTCONN; return -1; }
    if (!ctx->fallback) return 0;
    int rc=receive_screen_info(ctx,0);
    if (rc < 0) goto control_failure;
    if (!rc) { errno=EAGAIN; return -1; }
    rc=pickup_fds(ctx);
    if (rc < 0) goto control_failure;
    if (!rc) { errno=EAGAIN; return -1; }
    rc=receive_dmabufs(ctx);
    if (rc < 0) goto consumer_failure;
    if (!rc) { errno=EAGAIN; return -1; }
    ctx->fallback=false;
    return 0;

control_failure:
    /* The protocol has no attempt ID. Closing an ambiguous control stream is
     * mandatory: late bytes/fds must never be interpreted as the next pickup. */
    if (ctx->ctrl_fd >= 0) { close(ctx->ctrl_fd); ctx->ctrl_fd=-1; }
consumer_failure: {
    int saved=errno;
    release_consumer_resources(ctx);
    errno=saved;
    return -1;
}
}

int get_data_fd(display_ctx *ctx)
{
    return ctx->data_fd;
}

/* Current local end of the audio socketpair, or -1 in fallback. The value changes
 * across reconnects (each pickup installs a fresh socket), so the audio engine must
 * be re-pointed via anland_audio_set_fd() rather than caching it. */
int get_audio_fd(display_ctx *ctx)
{
    return ctx->fallback ? -1 : ctx->audio_fd;
}

int get_buffer_ready_fd(display_ctx *ctx)
{
    return ctx->buf_ready_efd;
}

int get_buf_count(display_ctx *ctx)
{
    return ctx->buf_count;
}

int get_selected_idx(display_ctx *ctx)
{
    int raw = get_selected_idx_raw(ctx);
    return (raw >= 0 && raw < ctx->buf_count) ? raw : 0;
}

/* Unclamped view of the consumer's shared index page. The producer must not
 * trust this value: it is written by the consumer and read without a lock, so a
 * backend that switches framebuffers on it checks it against get_buf_count()
 * first and treats anything else as consumer loss. */
int get_selected_idx_raw(display_ctx *ctx)
{
    if (!ctx || !ctx->shm_ptr)
        return -1;
    return (int)(*ctx->shm_ptr);
}

int get_dmabuf_fd(display_ctx *ctx)
{
    return get_dmabuf_fd_at(ctx, get_selected_idx(ctx));
}

int get_dmabuf_fd_at(display_ctx *ctx, int idx)
{
    if (idx < 0 || idx >= ctx->buf_count)
        return -1;
    return ctx->dmabuf_fds[idx];
}

int get_dmabuf_info(display_ctx *ctx, struct buf_info *info)
{
    return get_dmabuf_info_at(ctx, get_selected_idx(ctx), info);
}

int get_dmabuf_info_at(display_ctx *ctx, int idx, struct buf_info *info)
{
    if (idx < 0 || idx >= ctx->buf_count)
        return -1;
    *info = ctx->dmabuf_infos[idx];
    return 0;
}
