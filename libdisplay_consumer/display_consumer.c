#define _GNU_SOURCE
#include "display_consumer.h"
#include "../common/socket_utils.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <unistd.h>

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif

static inline int anland_memfd_create(const char *name, unsigned int flags)
{
#if defined(__NR_memfd_create)
    return (int)syscall(__NR_memfd_create, name, flags);
#else
    return -1;
#endif
}

struct display_ctx {
    int      ctrl_fd;
    int      data_fd;
    int      buf_ready_efd;
    int      fence_fd;        /* read end of the dedicated render-done fence channel */
    int      shm_fd;
    int      audio_fd;        /* local end of the bidirectional audio socketpair (hello slot 4) */
    volatile uint32_t *shm_ptr;
    uint32_t screen_w, screen_h;
    uint32_t pixel_format;
    bool     fallback;
    bool     buffer_pending;

    /* The display lib is called concurrently: the render thread (select_dmabuf /
     * refresh_done / push_dmabufs), the event thread (poll_output_event /
     * handle_resource_request) and JNI input threads (push_input_event*). Two locks
     * with a fixed order (state_lock -> data_lock) tame the resulting races:
     *   - state_lock guards `fallback`, every fd field, `shm_ptr` and `buffer_pending`
     *     (i.e. the whole connection lifecycle mutated by enter_fallback).
     *   - data_lock serialises concurrent WRITES to data_fd.
     * Invariant: never call enter_fallback() or a user callback while holding either
     * lock (they re-acquire / re-enter). */
    pthread_mutex_t state_lock;
    pthread_mutex_t data_lock;

    /* Hot-path gate: data_fd writes only need data_lock while the producer may still
     * request service fds (the two-part push_input_event_with_fds send is the only
     * writer that can interleave with input events). Once every registered service's
     * fds have gone out, no fd-carrying writer remains and input events go lockless.
     * Monotonic true->false per session; a stale `true` read just takes a harmless
     * extra lock, a `false` read is only reachable after the last fd write finished. */
    volatile bool data_needs_lock;
    uint32_t      services_sent_mask;

    int              stored_fds[MAX_BUFS];
    struct buf_info  stored_infos[MAX_BUFS];
    int              stored_count;

    void (*fallback_cb)(void *);
    void (*exit_fallback_cb)(void *);
    void  *fallback_userdata;
    void  *exit_fallback_userdata;
    struct service_info *services;
    int             num_services;
    struct resources *resources;
};

/* (Re)arm the data_fd write gate for a fresh connected session: writers take
 * data_lock until every service's fds have been sent. Callers either hold data_lock
 * (enter_fallback) or run single-threaded at (re)connect (allocate_services,
 * try_exit_fallback), so this needs no locking of its own. */
static void arm_data_lock(struct display_ctx *ctx)
{
    ctx->services_sent_mask = 0;
    ctx->data_needs_lock = (ctx->num_services > 0);
}

void allocate_services(struct display_ctx *ctx, struct service_info *services, int num_services){
    ctx->services = services;
    ctx->num_services = num_services;
    ctx->resources = (struct resources*)malloc(sizeof(struct resources) * num_services);
    if (!ctx->resources) {
        ctx->num_services = 0;
        return;
    }
    for(int i=0;i<num_services;i++){
        ctx->resources[i].service_type = services[i].type;
        ctx->resources[i].type = -1;//unallocated
        ctx->resources[i].num = 0;
        ctx->resources[i].fds = NULL;
    }
    arm_data_lock(ctx);
}
void push_input_event_with_fds(display_ctx *ctx, const struct InputEvent *event, int* fds, int fd_count);
void handle_resource_request(struct display_ctx *ctx, struct OutputEvent *event){
    uint32_t service_type = event->resources_request.type;
    uint8_t found = 0;
    int i;
    for(i=0;i<ctx->num_services;i++){
        if(ctx->services[i].type == service_type){
            //if failed fds=NULL, num=0
            struct resources res = ctx->services[i].allocate_resource(event->resources_request.args, ctx->services[i].userdata);
            if (ctx->resources[i].type != -1) {
                // free previous resource if it was allocated
                ctx->services[i].free_resource(ctx->resources[i], ctx->services[i].userdata);
            }
            ctx->resources[i] = res;
            found = 1;
            break;
        }
    }
    if(!found)
        return; //do nothing if the service type is not found, the producer will not enable the service if it not received the resources back
    //send resources back to producer
    struct InputEvent input_event;
    input_event.type = INPUT_TYPE_RESOURCE;
    input_event.resource.type = service_type;
    input_event.resource.fdnum = ctx->resources[i].num;
    push_input_event_with_fds(ctx, &input_event, ctx->resources[i].fds, ctx->resources[i].num);

    /* This service's fds are out. Once every service has been sent, no fd-carrying
     * writer remains, so drop the data_fd write lock (input goes lockless). Update
     * under data_lock so the flip-to-false lands after the fd send released it. */
    if (!ctx->fallback && ctx->num_services > 0 && ctx->num_services < 32) {
        pthread_mutex_lock(&ctx->data_lock);
        ctx->services_sent_mask |= (1u << i);
        uint32_t all = (1u << ctx->num_services) - 1u;
        if ((ctx->services_sent_mask & all) == all)
            ctx->data_needs_lock = false;
        pthread_mutex_unlock(&ctx->data_lock);
    }
}
void free_resources(struct display_ctx *ctx){//释放资源，保留服务信息
    for(int i=0;i<ctx->num_services;i++){
        if(ctx->resources[i].type != -1){
            ctx->services[i].free_resource(ctx->resources[i], ctx->services[i].userdata);
            ctx->resources[i].type = -1;
            ctx->resources[i].num = 0;
            ctx->resources[i].fds = NULL;
        }
    }
}
static int create_shm(display_ctx *ctx)
{
    ctx->shm_fd = anland_memfd_create("buf_select", MFD_CLOEXEC);
    if (ctx->shm_fd < 0)
        return -1;
    if (ftruncate(ctx->shm_fd, sizeof(uint32_t)) < 0) {
        close(ctx->shm_fd);
        ctx->shm_fd = -1;
        return -1;
    }
    ctx->shm_ptr = mmap(NULL, sizeof(uint32_t), PROT_READ | PROT_WRITE,
                        MAP_SHARED, ctx->shm_fd, 0);
    if (ctx->shm_ptr == MAP_FAILED) {
        ctx->shm_ptr = NULL;
        close(ctx->shm_fd);
        ctx->shm_fd = -1;
        return -1;
    }
    *ctx->shm_ptr = 0;
    return 0;
}

static int send_hello_fds(display_ctx *ctx)
{
    /* Three dedicated socketpairs:
     *   - data:  consumer->producer input/bufs (reverse direction reserved for future)
     *   - fence: producer->consumer render-done messages; the message itself is the
     *            "frame rendered" signal (no separate eventfd, no cross-channel ordering).
     *   - audio: full-duplex PCM -- producer writes playback, consumer writes mic.
     * We keep one end of each and hand the other to the producer. The fd slot order
     * must match the producer's pickup_fds(): { buf_ready, fence, data, shm, audio }. */
    int sv[2], fv[2], av[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
        return -1;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, fv) < 0) {
        close(sv[0]);
        close(sv[1]);
        return -1;
    }
    /* SEQPACKET: each PCM/format message is one atomic datagram, so neither end can
     * desync mid-frame the way a byte stream could on a partial send/recv. */
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, av) < 0) {
        close(sv[0]);
        close(sv[1]);
        close(fv[0]);
        close(fv[1]);
        return -1;
    }
    ctx->data_fd  = sv[0];
    ctx->fence_fd = fv[0];
    ctx->audio_fd = av[0];

    struct ctrl_msg hdr = { .type = CTRL_MSG_CONSUMER_HELLO, .size = 0 };
    int fds[5] = { ctx->buf_ready_efd, fv[1], sv[1], ctx->shm_fd, av[1] };
    int ret = send_fds(ctx->ctrl_fd, &hdr, sizeof(hdr), fds, 5);
    close(sv[1]);
    close(fv[1]);
    close(av[1]);
    return ret;
}

static void enter_fallback(display_ctx *ctx);

static int push_dmabufs_internal(display_ctx *ctx)
{
    if (ctx->stored_count <= 0)
        return 0;

    struct data_msg dhdr = {
        .type = DATA_MSG_BUFS_READY,
        .size = ctx->stored_count * sizeof(struct buf_info),
    };
    if (send_fds(ctx->data_fd, &dhdr, sizeof(dhdr),
                 ctx->stored_fds, ctx->stored_count) < 0) {
        enter_fallback(ctx);
        return -1;
    }
    if (send_all(ctx->data_fd, ctx->stored_infos,
                 ctx->stored_count * sizeof(struct buf_info)) < 0) {
        enter_fallback(ctx);
        return -1;
    }
    return 0;
}

/* Self-contained fallback->active transition: safe to call from any thread/site (the
 * state flip is under state_lock; only one caller wins). Currently driven by the
 * render thread via select_dmabuf, but the locking keeps future call sites correct. */
static bool try_exit_fallback(display_ctx *ctx)
{
    if (!ctx->fallback)
        return false;

    struct pollfd pfd = { .fd = ctx->ctrl_fd, .events = POLLIN };
    if (poll(&pfd, 1, 0) <= 0 || !(pfd.revents & POLLIN))
        return false;

    struct ctrl_msg hdr;
    if (recv_all(ctx->ctrl_fd, &hdr, sizeof(hdr)) != 0 ||
        hdr.type != CTRL_MSG_FDS_READY)
        return false;

    /* Publish the up-transition under state_lock so a concurrent enter_fallback (or a
     * future try_exit_fallback from another site) sees a consistent fallback/arm state.
     * The event thread is stopped while in fallback and only restarts in
     * exit_fallback_cb below, so no handle_resource_request races the re-arm. */
    pthread_mutex_lock(&ctx->state_lock);
    bool won = ctx->fallback;
    if (won) {
        ctx->fallback = false;
        arm_data_lock(ctx);
    }
    pthread_mutex_unlock(&ctx->state_lock);
    if (!won)
        return false;

    push_dmabufs_internal(ctx);   /* outside the lock: may enter_fallback on failure */
    if (ctx->exit_fallback_cb)
        ctx->exit_fallback_cb(ctx->exit_fallback_userdata);
    return true;
}

static void enter_fallback(display_ctx *ctx)
{
    /* Atomic test-and-set of `fallback` so two threads (render timeout + event/input
     * send failure) can't both tear the connection down (double close/munmap/cb). */
    pthread_mutex_lock(&ctx->state_lock);
    if (ctx->fallback) {
        pthread_mutex_unlock(&ctx->state_lock);
        return;
    }
    ctx->fallback = true;          /* set first: lockless hot-path writers bail early */
    free_resources(ctx);
    ctx->buffer_pending = false;

    /* Fence out data_fd writers while the fds are closed and replaced. Null each
     * pointer/fd before closing so a racing lockless writer reads -1 / NULL and fails
     * benignly (EBADF) rather than writing into a reused fd. */
    pthread_mutex_lock(&ctx->data_lock);
    if (ctx->data_fd >= 0)         { int fd = ctx->data_fd; ctx->data_fd = -1; close(fd); }
    if (ctx->buf_ready_efd >= 0)   { close(ctx->buf_ready_efd);   ctx->buf_ready_efd = -1; }
    if (ctx->fence_fd >= 0)        { close(ctx->fence_fd);        ctx->fence_fd = -1; }
    if (ctx->audio_fd >= 0)        { close(ctx->audio_fd);        ctx->audio_fd = -1; }
    if (ctx->shm_ptr) { volatile uint32_t *p = ctx->shm_ptr; ctx->shm_ptr = NULL; munmap((void *)p, sizeof(uint32_t)); }
    if (ctx->shm_fd >= 0)         { close(ctx->shm_fd);           ctx->shm_fd = -1; }

    /* buf_ready_efd stays an eventfd (consumer->producer pacing signal); fence_fd is
     * (re)created as a socketpair inside send_hello_fds(). */
    ctx->buf_ready_efd = eventfd(0, EFD_CLOEXEC);
    bool shm_ok = (create_shm(ctx) == 0);
    if (shm_ok)
        send_hello_fds(ctx);
    arm_data_lock(ctx);
    pthread_mutex_unlock(&ctx->data_lock);
    pthread_mutex_unlock(&ctx->state_lock);

    /* User callback (JNI, stops the event thread) runs OUTSIDE both locks. */
    if (ctx->fallback_cb)
        ctx->fallback_cb(ctx->fallback_userdata);
}
int connect_to_deamon(display_ctx **out, const char *socket_path){
    return connect_to_deamon_with_fd(out, connect_unix(socket_path));
}
int connect_to_deamon_with_fd(display_ctx **out, int ctrl_fd)
{
    display_ctx *ctx = calloc(1, sizeof(*ctx));
    if (!ctx)
        return -1;

    pthread_mutex_init(&ctx->state_lock, NULL);
    pthread_mutex_init(&ctx->data_lock, NULL);

    ctx->ctrl_fd = -1;
    ctx->data_fd = -1;
    ctx->buf_ready_efd = -1;
    ctx->fence_fd = -1;
    ctx->shm_fd = -1;
    ctx->audio_fd = -1;
    ctx->shm_ptr = NULL;
    ctx->fallback = true;

    ctx->ctrl_fd = ctrl_fd;
    if (ctx->ctrl_fd < 0)
        goto fail;

    /* buf_ready_efd is the consumer->producer pacing eventfd; fence_fd is created as a
     * socketpair inside send_hello_fds(). */
    ctx->buf_ready_efd = eventfd(0, EFD_CLOEXEC);
    if (ctx->buf_ready_efd < 0)
        goto fail;

    if (create_shm(ctx) < 0)
        goto fail;

    if (send_hello_fds(ctx) < 0)
        goto fail;

    *out = ctx;
    return 0;

fail:
    if (ctx->shm_ptr) munmap((void *)ctx->shm_ptr, sizeof(uint32_t));
    if (ctx->shm_fd >= 0)         close(ctx->shm_fd);
    if (ctx->ctrl_fd >= 0)         close(ctx->ctrl_fd);
    if (ctx->data_fd >= 0)         close(ctx->data_fd);
    if (ctx->buf_ready_efd >= 0)   close(ctx->buf_ready_efd);
    if (ctx->fence_fd >= 0)        close(ctx->fence_fd);
    if (ctx->audio_fd >= 0)        close(ctx->audio_fd);
    pthread_mutex_destroy(&ctx->state_lock);
    pthread_mutex_destroy(&ctx->data_lock);
    free(ctx);
    return -1;
}

void disconnect(display_ctx *ctx)
{
    if (!ctx)
        return;
    if (ctx->shm_ptr) munmap((void *)ctx->shm_ptr, sizeof(uint32_t));
    if (ctx->shm_fd >= 0)         close(ctx->shm_fd);
    if (ctx->ctrl_fd >= 0)         close(ctx->ctrl_fd);
    if (ctx->data_fd >= 0)         close(ctx->data_fd);
    if (ctx->buf_ready_efd >= 0)   close(ctx->buf_ready_efd);
    if (ctx->fence_fd >= 0)        close(ctx->fence_fd);
    if (ctx->audio_fd >= 0)        close(ctx->audio_fd);
    free_resources(ctx);
    pthread_mutex_destroy(&ctx->state_lock);
    pthread_mutex_destroy(&ctx->data_lock);
    free(ctx);
}

int set_screen_info(display_ctx *ctx, uint32_t width, uint32_t height, uint32_t format, uint32_t refresh)
{
    ctx->screen_w = width;
    ctx->screen_h = height;
    ctx->pixel_format = format;

    struct ctrl_msg hdr = { .type = CTRL_MSG_SCREEN_INFO, .size = sizeof(struct screen_info) };
    struct screen_info si = { .width = width, .height = height, .format = format, .refresh = refresh };
    uint8_t msg[sizeof(struct ctrl_msg) + sizeof(struct screen_info)];
    memcpy(msg, &hdr, sizeof(hdr));
    memcpy(msg + sizeof(hdr), &si, sizeof(si));
    return send_all(ctx->ctrl_fd, msg, sizeof(msg));
}

int push_dmabufs(display_ctx *ctx, const int *fds, const struct buf_info *infos, int count)
{
    if (count > MAX_BUFS) count = MAX_BUFS;
    memcpy(ctx->stored_fds, fds, count * sizeof(int));
    memcpy(ctx->stored_infos, infos, count * sizeof(struct buf_info));
    ctx->stored_count = count;

    if (ctx->fallback)
        return 0;

    int ret = push_dmabufs_internal(ctx);
    enter_fallback(ctx);
    return ret;
}

int select_dmabuf(display_ctx *ctx, int idx)
{
    if (ctx->fallback) {
        try_exit_fallback(ctx);   /* holds state_lock internally for the transition */
        if (ctx->fallback)
            return 0;
    }

    if (idx < 0 || idx >= ctx->stored_count)
        return -1;

    *ctx->shm_ptr = (uint32_t)idx;
    eventfd_t val = 1;
    eventfd_write(ctx->buf_ready_efd, val);
    ctx->buffer_pending = true;
    return 0;
}

/* Wait for the producer to finish the frame, then return its render-done fence so
 * the caller can hand it to ANativeWindow_queueBuffer (SurfaceFlinger waits on it
 * GPU-side before scanout). The producer sends exactly one message per frame on the
 * dedicated fence channel; the message itself is the "frame rendered" signal (no
 * separate eventfd, no cross-channel ordering) and the optional fence rides as
 * SCM_RIGHTS ancillary data. Returns the fence fd (caller owns it), or -1 if none /
 * on error. */
int refresh_done(display_ctx *ctx)
{
    if (!ctx->buffer_pending)
        return -1;

    /* Block (with a 5s safety timeout) on the fence channel: the arrival of the
     * producer's per-frame message is the render-done signal. Timeout / no POLLIN
     * (producer stalled or gone) -> fall back so the render thread never hangs. */
    struct pollfd pfd = { .fd = ctx->fence_fd, .events = POLLIN };
    int ret = poll(&pfd, 1, 5000);
    if (ret <= 0 || !(pfd.revents & POLLIN)) {
        enter_fallback(ctx);
        return -1;
    }
    ctx->buffer_pending = false;

    int rfence = -1;
    char b;
    struct iovec iov = { .iov_base = &b, .iov_len = 1 };
    union {
        char buf[CMSG_SPACE(sizeof(int))];
        struct cmsghdr align;
    } cmsg;
    struct msghdr msg = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = cmsg.buf,
        .msg_controllen = sizeof(cmsg.buf),
    };
    /* Non-blocking even though poll reported POLLIN: if a concurrent enter_fallback
     * (from a JNI input thread) swapped fence_fd between the poll and this recvmsg,
     * we get EAGAIN (n < 0) instead of reading a stale/foreign socket. A clean EOF
     * (n == 0) means the producer closed the channel -> fall back. No fence in the
     * message => queue with -1 ("ready now"). */
    ssize_t n = recvmsg(ctx->fence_fd, &msg, MSG_DONTWAIT);
    if (n == 0) {
        enter_fallback(ctx);
        return -1;
    }
    if (n > 0) {
        struct cmsghdr *c = CMSG_FIRSTHDR(&msg);
        if (c && c->cmsg_type == SCM_RIGHTS)
            memcpy(&rfence, CMSG_DATA(c), sizeof(int));
    }
    return rfence;
}

int push_input_event(display_ctx *ctx, const struct InputEvent *event)
{
    if (ctx->fallback)
        return 0;

    struct data_msg hdr = { .type = DATA_MSG_INPUT_EVENT, .size = sizeof(struct InputEvent) };
    uint8_t msg[sizeof(struct data_msg) + sizeof(struct InputEvent)];
    memcpy(msg, &hdr, sizeof(hdr));
    memcpy(msg + sizeof(hdr), event, sizeof(*event));

    /* While the producer may still request service fds, serialise against the event
     * thread's two-part fd send (push_input_event_with_fds); once all services are
     * out, data_needs_lock clears and input goes lock-free (hot path). */
    bool locked = ctx->data_needs_lock;
    if (locked)
        pthread_mutex_lock(&ctx->data_lock);
    int fd = ctx->data_fd;
    int r = (fd >= 0) ? send_all(fd, msg, sizeof(msg)) : -1;
    if (locked)
        pthread_mutex_unlock(&ctx->data_lock);

    if (r < 0) {
        enter_fallback(ctx);
        return -1;
    }
    return 0;
}
int push_input_event_with_length(display_ctx *ctx, const struct InputEvent *event, void* payload, size_t size)
{
    if (ctx->fallback)
        return 0;

    struct data_msg hdr = { .type = DATA_MSG_INPUT_EVENT, .size = sizeof(struct InputEvent) };
    size_t total = sizeof(struct data_msg) + sizeof(struct InputEvent) + size;
    uint8_t *msg = (uint8_t *)malloc(total);
    if (!msg)
        return -1;
    memcpy(msg, &hdr, sizeof(hdr));
    memcpy(msg + sizeof(hdr), event, sizeof(*event));
    memcpy(msg + sizeof(hdr) + sizeof(struct InputEvent), payload, size);

    bool locked = ctx->data_needs_lock;
    if (locked)
        pthread_mutex_lock(&ctx->data_lock);
    int fd = ctx->data_fd;
    int r = (fd >= 0) ? send_all(fd, msg, total) : -1;
    if (locked)
        pthread_mutex_unlock(&ctx->data_lock);

    free(msg);
    if (r < 0) {
        enter_fallback(ctx);
        return -1;
    }
    return 0;
}
int poll_output_event(display_ctx *ctx, struct OutputEvent *event, int timeout_ms)
{
    if (ctx->fallback)
        return 0;

    struct pollfd pfd = { .fd = ctx->data_fd, .events = POLLIN };
    int ret = poll(&pfd, 1, timeout_ms);
    if (ret <= 0)
        return 0;

    if (pfd.revents & (POLLHUP | POLLERR)) {
        enter_fallback(ctx);
        return -1;
    }

    /* Consume the message directly -- no MSG_PEEK pre-check. The producer ->
     * consumer direction only ever carries OUTPUT_EVENT (render-done moved to
     * the dedicated fence channel) and both push_output_event* variants frame
     * it at exactly sizeof(struct OutputEvent), so what to read next is fully
     * determined; the peek cost one extra syscall per event. A mismatched
     * header can only mean the stream desynced: fall back rather than leaving
     * bytes wedged in the socket. */
    uint8_t msg_buf[sizeof(struct data_msg) + sizeof(struct OutputEvent)];
    if (recv_all(ctx->data_fd, msg_buf, sizeof(msg_buf)) < 0)
        return -1;

    struct data_msg hdr;
    memcpy(&hdr, msg_buf, sizeof(hdr));
    if (hdr.type != DATA_MSG_OUTPUT_EVENT || hdr.size != sizeof(struct OutputEvent)) {
        enter_fallback(ctx);
        return -1;
    }

    memcpy(event, msg_buf + sizeof(struct data_msg), sizeof(*event));
    return 1;
}
int poll_output_event_extend_data(display_ctx *ctx, void* payload, size_t size, int timeout_ms)
{
    if (ctx->fallback)
        return 0;

    struct pollfd pfd = { .fd = ctx->data_fd, .events = POLLIN };
    int ret = poll(&pfd, 1, timeout_ms);
    if (ret <= 0)
        return 0;

    if (pfd.revents & (POLLHUP | POLLERR)) {
        enter_fallback(ctx);
        return -1;
    }
    if (recv_all(ctx->data_fd, payload, size) < 0)
        return -1;
    return 1;
}
int set_fallback_callback(display_ctx *ctx, void (*on_fallback)(void *), void *userdata)
{
    ctx->fallback_cb = on_fallback;
    ctx->fallback_userdata = userdata;
    return 0;
}

int set_exit_fallback_callback(display_ctx *ctx, void (*on_exit_fallback)(void *), void *userdata)
{
    ctx->exit_fallback_cb = on_exit_fallback;
    ctx->exit_fallback_userdata = userdata;
    return 0;
}
int get_data_fd(display_ctx *ctx)
{
    return ctx->data_fd;
}
/* Current local end of the audio socketpair, or -1 in fallback. The value changes
 * across reconnects (each hello creates a fresh socketpair), so callers must re-fetch
 * it rather than cache it. */
int get_audio_fd(display_ctx *ctx)
{
    return ctx->fallback ? -1 : ctx->audio_fd;
}
//用于处理未处理的变长payload事件
void handle_unhandled_event(display_ctx *ctx, const struct OutputEvent *event)
{
    switch (event->type)
    {
    case OUTPUT_TYPE_CLIPBOARD:
        //客户端发送了一个剪贴板事件，后续会有变长数据跟随，但是库调用者没有处理这个事件，所以我们需要把后续的变长数据读掉，避免阻塞
        if (event->clipboard.size > 0) {
            void* payload = malloc(event->clipboard.size);
            if (payload) {
                poll_output_event_extend_data(ctx, payload, event->clipboard.size, 1000);
                free(payload);
            }
        }
        break;
    default:
        break;
    }
}

void push_input_event_with_fds(display_ctx *ctx, const struct InputEvent *event, int* fds, int fd_count)
{
    if (ctx->fallback)
        return;

    /* This is the ONLY fd-carrying writer, and it is two framed sends (RESOURCE event
     * + EXTEND_FDS ancillary). Hold data_lock across BOTH so a concurrent input write
     * can't wedge between them and desync the producer's framed stream. Sent inline
     * here (not via push_input_event) to avoid re-locking data_lock. */
    struct data_msg hdr = { .type = DATA_MSG_INPUT_EVENT, .size = sizeof(struct InputEvent) };
    uint8_t msg[sizeof(struct data_msg) + sizeof(struct InputEvent)];
    memcpy(msg, &hdr, sizeof(hdr));
    memcpy(msg + sizeof(hdr), event, sizeof(*event));

    pthread_mutex_lock(&ctx->data_lock);
    int fd = ctx->data_fd;
    bool ok = (fd >= 0) && send_all(fd, msg, sizeof(msg)) == 0;
    if (ok && fd_count > 0) {
        struct data_msg fhdr = { .type = DATA_MSG_INPUT_EXTEND_FDS, .size = 0 };
        ok = send_fds(fd, &fhdr, sizeof(fhdr), fds, fd_count) >= 0;
    }
    pthread_mutex_unlock(&ctx->data_lock);

    if (!ok)
        enter_fallback(ctx);   /* outside the lock */
}
