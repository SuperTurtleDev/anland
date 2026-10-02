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

/* poll() timeout (ms) for the two reconnect handshake steps. Kept short so the
 * caller's reconnect loop stays responsive when no consumer is present yet. */
#define HANDSHAKE_TIMEOUT_MS 100

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

    int      dmabuf_fds[MAX_BUFS];
    struct buf_info dmabuf_infos[MAX_BUFS];
    int      buf_count;
    // Input framing state survives partial socket reads; owned by this context.
    unsigned char *input_bytes;
    size_t input_size, input_used;
    int input_kind;
    int input_fds[64], input_fd_count;
    int64_t input_deadline;


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
static void release_consumer_resources(display_ctx *ctx)
{
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

/*
 * Ask the daemon for the consumer-side fds and map the shm index. Polls ctrl_fd
 * with a short timeout so it returns promptly when no consumer is up yet. On
 * success the five fds and shm_ptr are installed on ctx; the caller releases them
 * via release_consumer_resources() on any later failure. Returns 0 / -1.
 */
static int pickup_fds(display_ctx *ctx)
{
    struct ctrl_msg hdr = { .type = CTRL_MSG_PICKUP_FDS, .size = 0 };
    if (send_all(ctx->ctrl_fd, &hdr, sizeof(hdr)) < 0)
        return -1;

    struct pollfd pfd = { .fd = ctx->ctrl_fd, .events = POLLIN };
    if (poll(&pfd, 1, HANDSHAKE_TIMEOUT_MS) <= 0)
        return -1;

    int fds[5];
    int fd_count = 0;
    struct ctrl_msg resp;
    int n = recv_fds(ctx->ctrl_fd, &resp, sizeof(resp), fds, 5, &fd_count);
    if (n < (int)sizeof(resp) || resp.type != CTRL_MSG_FDS_READY
        || resp.size != 0 || fd_count != 5) {
        for (int i = 0; i < fd_count; i++)
            close(fds[i]);
        return -1;
    }

    /* Slot order matches the consumer's send_hello_fds(): { buf_ready, fence, data, shm, audio }.
     * fence_fd is the write end of the dedicated producer->consumer render-done channel;
     * audio_fd is the full-duplex PCM socket (producer writes playback, reads mic). */
    ctx->buf_ready_efd    = fds[0];
    ctx->fence_fd         = fds[1];
    ctx->data_fd          = fds[2];
    ctx->shm_fd           = fds[3];
    ctx->audio_fd         = fds[4];
    struct timeval io_timeout = {.tv_sec=0, .tv_usec=100000};
    if (setsockopt(ctx->data_fd, SOL_SOCKET, SO_RCVTIMEO, &io_timeout, sizeof(io_timeout)) < 0)
        return -1;

    ctx->shm_ptr = mmap(NULL, sizeof(uint32_t), PROT_READ, MAP_SHARED, ctx->shm_fd, 0);
    if (ctx->shm_ptr == MAP_FAILED) {
        ctx->shm_ptr = NULL;
        return -1;
    }
    return 0;
}

/*
 * Receive the dmabuf set the consumer pushes onto the data channel right after the
 * fd handshake. Polls data_fd with a short timeout. On failure the caller releases
 * the partially-acquired consumer resources. Returns 0 / -1.
 */
static int receive_dmabufs(display_ctx *ctx)
{
    if (ctx->buf_count > 0)
        return 0;

    struct pollfd pfd = { .fd = ctx->data_fd, .events = POLLIN | POLLHUP | POLLERR };
    if (poll(&pfd, 1, HANDSHAKE_TIMEOUT_MS) <= 0)
        return -1;
    if (pfd.revents & (POLLHUP | POLLERR))
        return -1;

    struct data_msg dhdr;
    int fds[MAX_BUFS];
    int fd_count = 0;

    int n = recv_fds(ctx->data_fd, &dhdr, sizeof(dhdr), fds, MAX_BUFS, &fd_count);
    if (n < (int)sizeof(struct data_msg) || fd_count < 1) {
        for (int i = 0; i < fd_count; i++)
            close(fds[i]);
        return -1;
    }

    if (dhdr.type != DATA_MSG_BUFS_READY) {
        for (int i = 0; i < fd_count; i++)
            close(fds[i]);
        return -1;
    }

    if (dhdr.size == 0 || dhdr.size % sizeof(struct buf_info) != 0) {
        for (int i = 0; i < fd_count; i++)
            close(fds[i]);
        return -1;
    }

    int count = dhdr.size / sizeof(struct buf_info);
    if (count != fd_count || count > MAX_BUFS) {
        for (int i = 0; i < fd_count; i++)
            close(fds[i]);
        return -1;
    }

    struct buf_info infos[MAX_BUFS];
    if (recv_all(ctx->data_fd, infos, dhdr.size) < 0) {
        for (int i = 0; i < fd_count; i++)
            close(fds[i]);
        return -1;
    }

    for (int i = 0; i < count; i++) {
        ctx->dmabuf_fds[i] = fds[i];
        ctx->dmabuf_infos[i] = infos[i];
    }
    ctx->buf_count = count;
    return 0;
}

int connect_to_deamon(display_ctx **out, const char *socket_path)
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
    // Handshake operations also have a finite per-syscall timeout.
    struct timeval io_timeout = {.tv_sec=0, .tv_usec=100000};
    if (setsockopt(ctx->ctrl_fd, SOL_SOCKET, SO_RCVTIMEO, &io_timeout, sizeof(io_timeout)) < 0 ||
        setsockopt(ctx->ctrl_fd, SOL_SOCKET, SO_SNDTIMEO, &io_timeout, sizeof(io_timeout)) < 0)
        goto fail;

    struct ctrl_msg hdr = { .type = CTRL_MSG_PRODUCER_HELLO, .size = 0 };
    if (send_all(ctx->ctrl_fd, &hdr, sizeof(hdr)) < 0)
        goto fail;

    uint8_t buf[sizeof(struct ctrl_msg) + sizeof(struct screen_info)];
    if (recv_all(ctx->ctrl_fd, buf, sizeof(buf)) < 0)
        goto fail;

    struct ctrl_msg resp;
    memcpy(&resp, buf, sizeof(resp));
    if (resp.type != CTRL_MSG_SCREEN_INFO || resp.size != sizeof(struct screen_info))
        goto fail;

    struct screen_info si;
    memcpy(&si, buf + sizeof(struct ctrl_msg), sizeof(si));
    ctx->screen_w = si.width;
    ctx->screen_h = si.height;
    ctx->pixel_format = si.format;
    ctx->refresh = si.refresh;

    // Daemon handshake only: screen info is in hand, but the consumer fds and
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

int poll_input_event(display_ctx *ctx, struct InputEvent *event, int timeout_ms)
{
    uint8_t bytes[sizeof(struct data_msg) + sizeof(struct InputEvent)];
    int rc=input_read(ctx, bytes, sizeof(bytes), 1, timeout_ms);
    if (rc <= 0) return rc;
    struct data_msg hdr; memcpy(&hdr,bytes,sizeof(hdr));
    if (hdr.type != DATA_MSG_INPUT_EVENT || hdr.size != sizeof(*event)) {
        enter_fallback(ctx); return -1;
    }
    memcpy(event,bytes+sizeof(hdr),sizeof(*event)); return 1;
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

// A bounded synchronous send preserves the existing API's completed-write
// contract. On partial failure, detach: continuing would corrupt stream framing.
static int output_send(display_ctx *ctx, const void *bytes, size_t size)
{
    size_t offset=0;
    int64_t deadline=monotonic_ms()+10;
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

void force_fallback(display_ctx *ctx)
{
    if (!ctx || ctx->fallback)
        return;

    ctx->fallback = true;
    if (ctx->pre_release_cb)
        ctx->pre_release_cb(ctx->pre_release_userdata);
    release_consumer_resources(ctx);
}

int try_exit_fallback(display_ctx *ctx)
{
    if (!ctx->fallback)
        return 0;

    // Step 1: ask the daemon to hand over the consumer-side fds.
    if (pickup_fds(ctx) < 0) {
        release_consumer_resources(ctx);
        return -1;
    }

    // Step 2: immediately pull the dmabuf set the consumer pushes right after the
    // fd handshake. Only leave fallback once both fds and dmabufs are in hand, so
    // the backend can import straight away.
    if (receive_dmabufs(ctx) < 0) {
        release_consumer_resources(ctx);
        return -1;
    }

    ctx->fallback = false;
    return 0;
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