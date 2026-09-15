#define _GNU_SOURCE
#include "display_consumer.h"
#include "../common/socket_utils.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

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
    uint64_t connection_generation;
    uint64_t pending_generation;
    int      output_payload_fd;
    size_t   output_payload_size;
    uint64_t output_payload_generation;

    /* The display lib is called concurrently by render, event, and JNI input
     * threads:
     *   - state_lock guards fallback, generations, fd fields, shm_ptr,
     *     pending-frame/payload state, resources, and the stored buffer set.
     *   - data_lock serialises complete data_fd frames.
     * Writers acquire data_lock before briefly inspecting state_lock, then release
     * state_lock before any blocking send. Fallback marks the generation dead and
     * shutdown()s its socket under state_lock before waiting for data_lock. */
    pthread_mutex_t state_lock;
    pthread_mutex_t data_lock;


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
    uint64_t        services_generation;
};
static void enter_fallback_for_generation(display_ctx *ctx, uint64_t generation);



struct resource_release {
    struct resources resource;
    struct service_info service;
};

/* Caller holds state_lock. Detach one resource so its user callback can run
 * after the lock is released. */
static bool detach_one_resource_locked(display_ctx *ctx,
                                       struct resource_release *release)
{
    for (int i = 0; i < ctx->num_services; i++) {
        if (ctx->resources[i].type == -1)
            continue;
        release->resource = ctx->resources[i];
        release->service = ctx->services[i];
        ctx->resources[i].type = -1;
        ctx->resources[i].num = 0;
        ctx->resources[i].fds = NULL;
        return true;
    }
    return false;
}

void free_resources(display_ctx *ctx)
{
    if (!ctx)
        return;

    pthread_mutex_lock(&ctx->state_lock);
    const uint64_t services_generation = ctx->services_generation;
    pthread_mutex_unlock(&ctx->state_lock);

    for (;;) {
        struct resource_release release;
        pthread_mutex_lock(&ctx->state_lock);
        const bool found =
            ctx->services_generation == services_generation &&
            detach_one_resource_locked(ctx, &release);
        pthread_mutex_unlock(&ctx->state_lock);
        if (!found)
            return;
        if (release.service.free_resource)
            release.service.free_resource(release.resource,
                                          release.service.userdata);
    }
}

void allocate_services(display_ctx *ctx, struct service_info *services,
                       int num_services)
{
    if (!ctx || num_services < 0 || (num_services > 0 && !services))
        return;

    struct resources *resources =
        num_services > 0 ? calloc((size_t)num_services, sizeof(*resources)) : NULL;
    if (num_services > 0 && !resources)
        return;
    for (int i = 0; i < num_services; i++) {
        resources[i].service_type = services[i].type;
        resources[i].type = -1;
    }

    pthread_mutex_lock(&ctx->state_lock);
    struct service_info *old_services = ctx->services;
    struct resources *old_resources = ctx->resources;
    const int old_count = ctx->num_services;
    ctx->services = services;
    ctx->num_services = num_services;
    ctx->resources = resources;
    ctx->services_generation++;
    pthread_mutex_unlock(&ctx->state_lock);

    for (int i = 0; i < old_count; i++) {
        if (old_resources[i].type != -1 && old_services[i].free_resource)
            old_services[i].free_resource(old_resources[i],
                                          old_services[i].userdata);
    }
    free(old_resources);
}

/* On success data_lock remains held until the caller completes one whole frame.
 * No thread waits for data_lock while holding state_lock. */
static int claim_data_write(display_ctx *ctx, bool require_generation,
                            uint64_t required_generation,
                            uint64_t *generation, int *data_fd)
{
    pthread_mutex_lock(&ctx->data_lock);
    pthread_mutex_lock(&ctx->state_lock);
    if (ctx->fallback ||
        (require_generation &&
         ctx->connection_generation != required_generation)) {
        pthread_mutex_unlock(&ctx->state_lock);
        pthread_mutex_unlock(&ctx->data_lock);
        return 0;
    }
    *generation = ctx->connection_generation;
    *data_fd = ctx->data_fd;
    pthread_mutex_unlock(&ctx->state_lock);
    if (*data_fd < 0) {
        pthread_mutex_unlock(&ctx->data_lock);
        return -1;
    }
    return 1;
}

/* Caller holds data_lock and owns the lifetime of every supplied fd. */
static bool send_input_event_with_fds_locked(int data_fd,
                                             const struct InputEvent *event,
                                             const int *fds, int fd_count)
{
    struct data_msg hdr = {
        .type = DATA_MSG_INPUT_EVENT,
        .size = sizeof(struct InputEvent),
    };
    uint8_t msg[sizeof(struct data_msg) + sizeof(struct InputEvent)];
    memcpy(msg, &hdr, sizeof(hdr));
    memcpy(msg + sizeof(hdr), event, sizeof(*event));

    bool ok = send_all(data_fd, msg, sizeof(msg)) == 0;
    if (ok && fd_count > 0) {
        struct data_msg fhdr = { .type = DATA_MSG_INPUT_EXTEND_FDS, .size = 0 };
        ok = send_fds(data_fd, &fhdr, sizeof(fhdr), fds, fd_count) >= 0;
    }
    return ok;
}

static int send_input_event_with_fds_for_generation(
    display_ctx *ctx, const struct InputEvent *event, const int *fds,
    int fd_count, uint64_t required_generation)
{
    uint64_t generation = 0;
    int data_fd = -1;
    const int claimed = claim_data_write(ctx, true, required_generation,
                                         &generation, &data_fd);
    if (claimed <= 0)
        return claimed;

    const bool ok =
        send_input_event_with_fds_locked(data_fd, event, fds, fd_count);
    pthread_mutex_unlock(&ctx->data_lock);
    if (!ok) {
        enter_fallback_for_generation(ctx, generation);
        return -1;
    }
    return 1;
}

void handle_resource_request(display_ctx *ctx, struct OutputEvent *event)
{
    if (!ctx || !event)
        return;

    const uint32_t service_type = event->resources_request.type;
    uint32_t args[3];
    memcpy(args, event->resources_request.args, sizeof(args));

    pthread_mutex_lock(&ctx->state_lock);
    int index = -1;
    for (int i = 0; !ctx->fallback && i < ctx->num_services; i++) {
        if (ctx->services[i].type == service_type &&
            ctx->services[i].allocate_resource) {
            index = i;
            break;
        }
    }
    if (index < 0) {
        pthread_mutex_unlock(&ctx->state_lock);
        return;
    }
    const struct service_info service = ctx->services[index];
    const uint64_t services_generation = ctx->services_generation;
    const uint64_t connection_generation = ctx->connection_generation;
    pthread_mutex_unlock(&ctx->state_lock);

    struct resources resource =
        service.allocate_resource(args, service.userdata);
    int *owned_fds = NULL;
    bool valid = resource.num <= INT_MAX &&
        (resource.num == 0 || resource.fds != NULL);
    if (valid && resource.num > 0) {
        owned_fds = malloc((size_t)resource.num * sizeof(*owned_fds));
        valid = owned_fds != NULL;
        for (uint32_t i = 0; valid && i < resource.num; i++) {
            owned_fds[i] = fcntl(resource.fds[i], F_DUPFD_CLOEXEC, 3);
            if (owned_fds[i] < 0) {
                for (uint32_t j = 0; j < i; j++)
                    close(owned_fds[j]);
                free(owned_fds);
                owned_fds = NULL;
                valid = false;
            }
        }
    }

    struct resource_release previous = {0};
    bool committed = false;
    if (valid) {
        pthread_mutex_lock(&ctx->state_lock);
        if (!ctx->fallback &&
            ctx->connection_generation == connection_generation &&
            ctx->services_generation == services_generation &&
            index < ctx->num_services &&
            ctx->services[index].type == service_type) {
            previous.resource = ctx->resources[index];
            previous.service = ctx->services[index];
            ctx->resources[index] = resource;
            committed = true;
        }
        pthread_mutex_unlock(&ctx->state_lock);
    }

    if (!committed) {
        if (service.free_resource)
            service.free_resource(resource, service.userdata);
        if (owned_fds) {
            for (uint32_t i = 0; i < resource.num; i++)
                close(owned_fds[i]);
            free(owned_fds);
        }
        return;
    }
    if (previous.resource.type != -1 && previous.service.free_resource)
        previous.service.free_resource(previous.resource,
                                       previous.service.userdata);

    struct InputEvent input_event = {0};
    input_event.type = INPUT_TYPE_RESOURCE;
    input_event.resource.type = service_type;
    input_event.resource.fdnum = resource.num;
    send_input_event_with_fds_for_generation(
        ctx, &input_event, owned_fds, (int)resource.num,
        connection_generation);

    for (uint32_t i = 0; i < resource.num; i++)
        close(owned_fds[i]);
    free(owned_fds);
}
static int create_shm(display_ctx *ctx)
{
    ctx->shm_fd = memfd_create("buf_select", MFD_CLOEXEC);
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


/* Caller holds data_lock. BUFS_READY is a two-part stream frame; the lock spans
 * both sends so no input frame can split the header from its buf_info payload. */
static int send_dmabufs_locked(int data_fd, const int *fds,
                               const struct buf_info *infos, int count)
{
    if (count <= 0)
        return 0;
    if (data_fd < 0)
        return -1;

    struct data_msg header = {
        .type = DATA_MSG_BUFS_READY,
        .size = count * sizeof(struct buf_info),
    };
    if (send_fds(data_fd, &header, sizeof(header), fds, count) < 0)
        return -1;
    return send_all(data_fd, infos, count * sizeof(struct buf_info));
}

/* Self-contained fallback->active transition. data_lock keeps the initial
 * BUFS_READY frame ahead of every writer, while state_lock is released before
 * the potentially blocking send so fallback can shutdown() the socket. */
static bool try_exit_fallback(display_ctx *ctx)
{
    pthread_mutex_lock(&ctx->state_lock);
    const bool in_fallback = ctx->fallback;
    const int ctrl_fd = ctx->ctrl_fd;
    pthread_mutex_unlock(&ctx->state_lock);
    if (!in_fallback || ctrl_fd < 0)
        return false;

    struct pollfd pfd = { .fd = ctrl_fd, .events = POLLIN };
    if (poll(&pfd, 1, 0) <= 0 || !(pfd.revents & POLLIN))
        return false;

    struct ctrl_msg header;
    if (recv_all(ctrl_fd, &header, sizeof(header)) != 0 ||
        header.type != CTRL_MSG_FDS_READY)
        return false;

    int stored_fds[MAX_BUFS];
    struct buf_info stored_infos[MAX_BUFS];
    int stored_count = 0;
    int data_fd = -1;
    uint64_t generation = 0;

    pthread_mutex_lock(&ctx->data_lock);
    pthread_mutex_lock(&ctx->state_lock);
    const bool won = ctx->fallback && ctx->data_fd >= 0;
    if (won) {
        ctx->fallback = false;
        ctx->connection_generation++;
        generation = ctx->connection_generation;
        ctx->buffer_pending = false;
        ctx->pending_generation = 0;
        data_fd = ctx->data_fd;
        stored_count = ctx->stored_count;
        if (stored_count > 0) {
            memcpy(stored_fds, ctx->stored_fds,
                   (size_t)stored_count * sizeof(*stored_fds));
            memcpy(stored_infos, ctx->stored_infos,
                   (size_t)stored_count * sizeof(*stored_infos));
        }
    }
    pthread_mutex_unlock(&ctx->state_lock);

    const int push_result =
        won ? send_dmabufs_locked(data_fd, stored_fds, stored_infos,
                                  stored_count)
            : -1;
    pthread_mutex_unlock(&ctx->data_lock);
    if (!won)
        return false;
    if (push_result < 0) {
        enter_fallback_for_generation(ctx, generation);
        return false;
    }

    pthread_mutex_lock(&ctx->state_lock);
    const bool still_active =
        !ctx->fallback && ctx->connection_generation == generation;
    void (*callback)(void *) = still_active ? ctx->exit_fallback_cb : NULL;
    void *userdata = ctx->exit_fallback_userdata;
    pthread_mutex_unlock(&ctx->state_lock);
    if (!still_active)
        return false;
    if (callback)
        callback(userdata);
    return true;
}

static void enter_fallback_internal(display_ctx *ctx,
                                    bool require_generation,
                                    uint64_t generation)
{
    int data_fd;
    int ready_fd;
    int fence_fd;
    int audio_fd;
    int shm_fd;
    int payload_fd;
    volatile uint32_t *shm_ptr;
    uint64_t transition_generation;
    void (*callback)(void *);
    void *userdata;

    /* Publish fallback first, then shutdown the old sockets while holding only
     * state_lock. shutdown() wakes a writer blocked in send while data_lock stays
     * free of lock inversion; descriptor close/reuse waits for that writer below. */
    pthread_mutex_lock(&ctx->state_lock);
    if (ctx->fallback ||
        (require_generation && ctx->connection_generation != generation)) {
        pthread_mutex_unlock(&ctx->state_lock);
        return;
    }
    ctx->fallback = true;
    ctx->buffer_pending = false;
    ctx->pending_generation = 0;
    transition_generation = ctx->connection_generation;

    data_fd = ctx->data_fd;
    ready_fd = ctx->buf_ready_efd;
    fence_fd = ctx->fence_fd;
    audio_fd = ctx->audio_fd;
    shm_fd = ctx->shm_fd;
    shm_ptr = ctx->shm_ptr;
    payload_fd = ctx->output_payload_fd;
    ctx->data_fd = -1;
    ctx->buf_ready_efd = -1;
    ctx->fence_fd = -1;
    ctx->audio_fd = -1;
    ctx->shm_fd = -1;
    ctx->shm_ptr = NULL;
    ctx->output_payload_fd = -1;
    ctx->output_payload_size = 0;
    ctx->output_payload_generation = 0;

    if (data_fd >= 0)
        shutdown(data_fd, SHUT_RDWR);
    if (fence_fd >= 0)
        shutdown(fence_fd, SHUT_RDWR);
    if (audio_fd >= 0)
        shutdown(audio_fd, SHUT_RDWR);
    if (payload_fd >= 0)
        shutdown(payload_fd, SHUT_RDWR);
    callback = ctx->fallback_cb;
    userdata = ctx->fallback_userdata;
    pthread_mutex_unlock(&ctx->state_lock);

    /* Resource callbacks are user code and must never run under an internal lock. */
    free_resources(ctx);
    if (payload_fd >= 0)
        close(payload_fd);
    /* Complete the consumer-side detach before publishing replacement fds, so
     * fallback and exit callbacks cannot be observed out of order. */
    if (callback)
        callback(userdata);

    pthread_mutex_lock(&ctx->data_lock);
    if (data_fd >= 0)
        close(data_fd);
    if (ready_fd >= 0)
        close(ready_fd);
    if (fence_fd >= 0)
        close(fence_fd);
    if (audio_fd >= 0)
        close(audio_fd);
    if (shm_ptr)
        munmap((void *)shm_ptr, sizeof(uint32_t));
    if (shm_fd >= 0)
        close(shm_fd);

    pthread_mutex_lock(&ctx->state_lock);
    const bool still_fallback =
        ctx->fallback &&
        ctx->connection_generation == transition_generation;
    if (still_fallback) {
        ctx->buf_ready_efd = eventfd(0, EFD_CLOEXEC);
        const bool shm_ok =
            ctx->buf_ready_efd >= 0 && create_shm(ctx) == 0;
        if (shm_ok)
            send_hello_fds(ctx);
    }
    pthread_mutex_unlock(&ctx->state_lock);
    pthread_mutex_unlock(&ctx->data_lock);

}


static void enter_fallback_for_generation(display_ctx *ctx, uint64_t generation)
{
    enter_fallback_internal(ctx, true, generation);
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
    ctx->output_payload_fd = -1;
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
    if (ctx->output_payload_fd >= 0)
        close(ctx->output_payload_fd);
    free_resources(ctx);
    free(ctx->resources);
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
    if (!ctx || count < 0 || (count > 0 && (!fds || !infos)))
        return -1;
    if (count > MAX_BUFS)
        count = MAX_BUFS;

    int local_fds[MAX_BUFS];
    struct buf_info local_infos[MAX_BUFS];
    if (count > 0) {
        memcpy(local_fds, fds, (size_t)count * sizeof(*local_fds));
        memcpy(local_infos, infos, (size_t)count * sizeof(*local_infos));
    }

    pthread_mutex_lock(&ctx->data_lock);
    pthread_mutex_lock(&ctx->state_lock);
    if (count > 0) {
        memcpy(ctx->stored_fds, local_fds,
               (size_t)count * sizeof(*local_fds));
        memcpy(ctx->stored_infos, local_infos,
               (size_t)count * sizeof(*local_infos));
    }
    ctx->stored_count = count;
    if (ctx->fallback) {
        pthread_mutex_unlock(&ctx->state_lock);
        pthread_mutex_unlock(&ctx->data_lock);
        return 0;
    }
    const uint64_t generation = ctx->connection_generation;
    const int data_fd = ctx->data_fd;
    pthread_mutex_unlock(&ctx->state_lock);

    const int result =
        send_dmabufs_locked(data_fd, local_fds, local_infos, count);
    pthread_mutex_unlock(&ctx->data_lock);
    if (result < 0)
        enter_fallback_for_generation(ctx, generation);
    return result;
}

int select_dmabuf(display_ctx *ctx, int idx)
{
    pthread_mutex_lock(&ctx->state_lock);
    const bool in_fallback = ctx->fallback;
    pthread_mutex_unlock(&ctx->state_lock);
    if (in_fallback)
        try_exit_fallback(ctx);

    pthread_mutex_lock(&ctx->state_lock);
    if (ctx->fallback || idx < 0 || idx >= ctx->stored_count ||
        !ctx->shm_ptr || ctx->buf_ready_efd < 0 || ctx->buffer_pending) {
        pthread_mutex_unlock(&ctx->state_lock);
        return -1;
    }

    const uint64_t generation = ctx->connection_generation;
    *ctx->shm_ptr = (uint32_t)idx;
    eventfd_t val = 1;
    const int result = eventfd_write(ctx->buf_ready_efd, val);
    if (result == 0) {
        ctx->buffer_pending = true;
        ctx->pending_generation = generation;
    }
    pthread_mutex_unlock(&ctx->state_lock);

    if (result < 0) {
        enter_fallback_for_generation(ctx, generation);
        return -1;
    }
    return 0;
}

/* Wait for the producer to finish the frame. A successful completion may carry no
 * fence; status and optional fence are therefore separate. On success, out_fence is
 * either an owned render fence or -1 ("ready now"). On failure no completion was
 * observed, out_fence remains -1, and the caller must not present the buffer. */
int refresh_done(display_ctx *ctx, int *out_fence)
{
    if (!out_fence)
        return -1;
    *out_fence = -1;

    pthread_mutex_lock(&ctx->state_lock);
    if (ctx->fallback || !ctx->buffer_pending || ctx->fence_fd < 0 ||
        ctx->pending_generation != ctx->connection_generation) {
        pthread_mutex_unlock(&ctx->state_lock);
        return -1;
    }
    const uint64_t generation = ctx->connection_generation;
    const int fence_channel = fcntl(ctx->fence_fd, F_DUPFD_CLOEXEC, 3);
    pthread_mutex_unlock(&ctx->state_lock);
    if (fence_channel < 0) {
        enter_fallback_for_generation(ctx, generation);
        return -1;
    }

    struct pollfd pfd = { .fd = fence_channel, .events = POLLIN };
    int poll_result;
    do {
        pfd.revents = 0;
        poll_result = poll(&pfd, 1, 5000);
    } while (poll_result < 0 && errno == EINTR);
    if (poll_result <= 0 || !(pfd.revents & POLLIN)) {
        close(fence_channel);
        enter_fallback_for_generation(ctx, generation);
        return -1;
    }

    int render_fence = -1;
    char byte;
    struct iovec iov = { .iov_base = &byte, .iov_len = 1 };
    union {
        char buf[CMSG_SPACE(sizeof(int))];
        struct cmsghdr align;
    } cmsg;
    memset(&cmsg, 0, sizeof(cmsg));
    struct msghdr msg = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = cmsg.buf,
        .msg_controllen = sizeof(cmsg.buf),
    };
    const ssize_t received =
        recvmsg(fence_channel, &msg, MSG_DONTWAIT | MSG_CMSG_CLOEXEC);
    close(fence_channel);

    bool valid_message = received == 1 &&
        !(msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC));
    struct cmsghdr *control = CMSG_FIRSTHDR(&msg);
    if (control) {
        if (control->cmsg_level != SOL_SOCKET ||
            control->cmsg_type != SCM_RIGHTS ||
            control->cmsg_len != CMSG_LEN(sizeof(int)) ||
            CMSG_NXTHDR(&msg, control) != NULL) {
            valid_message = false;
        } else {
            memcpy(&render_fence, CMSG_DATA(control), sizeof(render_fence));
        }
    }

    pthread_mutex_lock(&ctx->state_lock);
    const bool current_completion =
        valid_message && !ctx->fallback && ctx->buffer_pending &&
        ctx->connection_generation == generation &&
        ctx->pending_generation == generation;
    if (current_completion) {
        ctx->buffer_pending = false;
        ctx->pending_generation = 0;
    }
    pthread_mutex_unlock(&ctx->state_lock);

    if (!current_completion) {
        if (render_fence >= 0)
            close(render_fence);
        if (!valid_message)
            enter_fallback_for_generation(ctx, generation);
        return -1;
    }

    *out_fence = render_fence;
    return 0;
}

int push_input_event(display_ctx *ctx, const struct InputEvent *event)
{
    if (!ctx || !event)
        return -1;

    struct data_msg header = {
        .type = DATA_MSG_INPUT_EVENT,
        .size = sizeof(struct InputEvent),
    };
    uint8_t msg[sizeof(struct data_msg) + sizeof(struct InputEvent)];
    memcpy(msg, &header, sizeof(header));
    memcpy(msg + sizeof(header), event, sizeof(*event));

    uint64_t generation = 0;
    int data_fd = -1;
    const int claimed =
        claim_data_write(ctx, false, 0, &generation, &data_fd);
    if (claimed == 0)
        return 0;
    if (claimed < 0) {
        enter_fallback_for_generation(ctx, generation);
        return -1;
    }

    const int result = send_all(data_fd, msg, sizeof(msg));
    pthread_mutex_unlock(&ctx->data_lock);
    if (result < 0) {
        enter_fallback_for_generation(ctx, generation);
        return -1;
    }
    return 0;
}
int push_input_event_with_length(display_ctx *ctx, const struct InputEvent *event,
                                 void *payload, size_t size)
{
    if (!ctx || !event || (size > 0 && !payload) ||
        size > SIZE_MAX - sizeof(struct data_msg) - sizeof(struct InputEvent))
        return -1;

    struct data_msg header = {
        .type = DATA_MSG_INPUT_EVENT,
        .size = sizeof(struct InputEvent),
    };
    const size_t total =
        sizeof(struct data_msg) + sizeof(struct InputEvent) + size;
    uint8_t *msg = malloc(total);
    if (!msg)
        return -1;
    memcpy(msg, &header, sizeof(header));
    memcpy(msg + sizeof(header), event, sizeof(*event));
    if (size > 0)
        memcpy(msg + sizeof(header) + sizeof(struct InputEvent), payload, size);

    uint64_t generation = 0;
    int data_fd = -1;
    const int claimed =
        claim_data_write(ctx, false, 0, &generation, &data_fd);
    if (claimed <= 0) {
        free(msg);
        if (claimed < 0)
            enter_fallback_for_generation(ctx, generation);
        return claimed;
    }

    const int result = send_all(data_fd, msg, total);
    pthread_mutex_unlock(&ctx->data_lock);
    free(msg);
    if (result < 0) {
        enter_fallback_for_generation(ctx, generation);
        return -1;
    }
    return 0;
}
/* Duplicate the current data channel while data_lock prevents descriptor reuse.
 * The duplicate remains tied to that connection after ctx->data_fd is replaced. */
static int dup_active_data_fd(display_ctx *ctx, uint64_t *generation, bool *active)
{
    *active = false;
    if (!ctx)
        return -1;

    pthread_mutex_lock(&ctx->data_lock);
    pthread_mutex_lock(&ctx->state_lock);
    if (ctx->fallback || ctx->data_fd < 0) {
        pthread_mutex_unlock(&ctx->state_lock);
        pthread_mutex_unlock(&ctx->data_lock);
        return -1;
    }
    *generation = ctx->connection_generation;
    *active = true;
    const int fd = fcntl(ctx->data_fd, F_DUPFD_CLOEXEC, 3);
    pthread_mutex_unlock(&ctx->state_lock);
    pthread_mutex_unlock(&ctx->data_lock);
    return fd;
}

static bool data_generation_is_active(display_ctx *ctx, uint64_t generation)
{
    pthread_mutex_lock(&ctx->state_lock);
    const bool active =
        !ctx->fallback && ctx->connection_generation == generation;
    pthread_mutex_unlock(&ctx->state_lock);
    return active;
}

int poll_output_event(display_ctx *ctx, struct OutputEvent *event, int timeout_ms)
{
    if (!ctx || !event)
        return -1;

    /* Starting another header before draining the prior payload would parse payload
     * bytes as a data_msg. Tear down only that generation instead. */
    int abandoned_fd = -1;
    uint64_t abandoned_generation = 0;
    pthread_mutex_lock(&ctx->state_lock);
    if (ctx->output_payload_fd >= 0) {
        abandoned_fd = ctx->output_payload_fd;
        abandoned_generation = ctx->output_payload_generation;
        ctx->output_payload_fd = -1;
        ctx->output_payload_size = 0;
        ctx->output_payload_generation = 0;
    }
    pthread_mutex_unlock(&ctx->state_lock);
    if (abandoned_fd >= 0) {
        shutdown(abandoned_fd, SHUT_RDWR);
        close(abandoned_fd);
        enter_fallback_for_generation(ctx, abandoned_generation);
        return -1;
    }

    uint64_t generation = 0;
    bool active = false;
    int data_fd = dup_active_data_fd(ctx, &generation, &active);
    if (data_fd < 0) {
        if (active)
            enter_fallback_for_generation(ctx, generation);
        return active ? -1 : 0;
    }

    struct pollfd pfd = { .fd = data_fd, .events = POLLIN };
    int result;
    do {
        pfd.revents = 0;
        result = poll(&pfd, 1, timeout_ms);
    } while (result < 0 && errno == EINTR);
    if (result == 0) {
        close(data_fd);
        return 0;
    }
    if (result < 0 || !(pfd.revents & POLLIN)) {
        close(data_fd);
        enter_fallback_for_generation(ctx, generation);
        return -1;
    }

    uint8_t msg_buf[sizeof(struct data_msg) + sizeof(struct OutputEvent)];
    if (recv_all(data_fd, msg_buf, sizeof(msg_buf)) < 0) {
        close(data_fd);
        enter_fallback_for_generation(ctx, generation);
        return -1;
    }

    struct data_msg header;
    memcpy(&header, msg_buf, sizeof(header));
    if (header.type != DATA_MSG_OUTPUT_EVENT ||
        header.size != sizeof(struct OutputEvent)) {
        close(data_fd);
        enter_fallback_for_generation(ctx, generation);
        return -1;
    }

    struct OutputEvent decoded;
    memcpy(&decoded, msg_buf + sizeof(struct data_msg), sizeof(decoded));
    const size_t payload_size =
        decoded.type == OUTPUT_TYPE_CLIPBOARD ? decoded.clipboard.size : 0;

    bool current;
    bool payload_collision = false;
    pthread_mutex_lock(&ctx->state_lock);
    current = !ctx->fallback &&
        ctx->connection_generation == generation;
    if (current && payload_size > 0) {
        if (ctx->output_payload_fd >= 0) {
            payload_collision = true;
            current = false;
        } else {
            ctx->output_payload_fd = data_fd;
            ctx->output_payload_size = payload_size;
            ctx->output_payload_generation = generation;
            data_fd = -1;
        }
    }
    pthread_mutex_unlock(&ctx->state_lock);
    if (data_fd >= 0)
        close(data_fd);
    if (payload_collision)
        enter_fallback_for_generation(ctx, generation);
    if (!current)
        return payload_collision ? -1 : 0;

    *event = decoded;
    return 1;
}

int poll_output_event_extend_data(display_ctx *ctx, void *payload, size_t size,
                                  int timeout_ms)
{
    if (!ctx || (size > 0 && !payload))
        return -1;
    if (size == 0)
        return 1;

    pthread_mutex_lock(&ctx->state_lock);
    if (ctx->fallback) {
        pthread_mutex_unlock(&ctx->state_lock);
        return 0;
    }
    if (ctx->output_payload_fd < 0) {
        pthread_mutex_unlock(&ctx->state_lock);
        return -1;
    }

    const int data_fd = ctx->output_payload_fd;
    const size_t expected_size = ctx->output_payload_size;
    const uint64_t generation = ctx->output_payload_generation;
    const bool current =
        ctx->connection_generation == generation && size == expected_size;
    ctx->output_payload_fd = -1;
    ctx->output_payload_size = 0;
    ctx->output_payload_generation = 0;
    pthread_mutex_unlock(&ctx->state_lock);

    if (!current) {
        shutdown(data_fd, SHUT_RDWR);
        close(data_fd);
        enter_fallback_for_generation(ctx, generation);
        return -1;
    }

    struct pollfd pfd = { .fd = data_fd, .events = POLLIN };
    int result;
    do {
        pfd.revents = 0;
        result = poll(&pfd, 1, timeout_ms);
    } while (result < 0 && errno == EINTR);
    if (result <= 0 || !(pfd.revents & POLLIN)) {
        close(data_fd);
        enter_fallback_for_generation(ctx, generation);
        return result == 0 ? 0 : -1;
    }

    const int recv_result = recv_all(data_fd, payload, size);
    close(data_fd);
    if (recv_result < 0) {
        enter_fallback_for_generation(ctx, generation);
        return -1;
    }
    return data_generation_is_active(ctx, generation) ? 1 : 0;
}

int set_fallback_callback(display_ctx *ctx, void (*on_fallback)(void *), void *userdata)
{
    if (!ctx)
        return -1;
    pthread_mutex_lock(&ctx->state_lock);
    ctx->fallback_cb = on_fallback;
    ctx->fallback_userdata = userdata;
    pthread_mutex_unlock(&ctx->state_lock);
    return 0;
}

int set_exit_fallback_callback(display_ctx *ctx, void (*on_exit_fallback)(void *),
                               void *userdata)
{
    if (!ctx)
        return -1;
    pthread_mutex_lock(&ctx->state_lock);
    ctx->exit_fallback_cb = on_exit_fallback;
    ctx->exit_fallback_userdata = userdata;
    pthread_mutex_unlock(&ctx->state_lock);
    return 0;
}

/* Returns an owned duplicate of the active data channel; the caller must close it. */
int get_data_fd(display_ctx *ctx)
{
    uint64_t generation = 0;
    bool active = false;
    return dup_active_data_fd(ctx, &generation, &active);
}

/* Current local end of the audio socketpair, or -1 in fallback. The value changes
 * across reconnects, so callers must re-fetch it rather than cache it. */
int get_audio_fd(display_ctx *ctx)
{
    if (!ctx)
        return -1;
    pthread_mutex_lock(&ctx->state_lock);
    const int fd = ctx->fallback ? -1 : ctx->audio_fd;
    pthread_mutex_unlock(&ctx->state_lock);
    return fd;
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

void push_input_event_with_fds(display_ctx *ctx, const struct InputEvent *event,
                               int *fds, int fd_count)
{
    if (!ctx || !event || fd_count < 0 || (fd_count > 0 && !fds))
        return;

    uint64_t generation = 0;
    int data_fd = -1;
    const int claimed =
        claim_data_write(ctx, false, 0, &generation, &data_fd);
    if (claimed <= 0) {
        if (claimed < 0)
            enter_fallback_for_generation(ctx, generation);
        return;
    }

    const bool ok =
        send_input_event_with_fds_locked(data_fd, event, fds, fd_count);
    pthread_mutex_unlock(&ctx->data_lock);
    if (!ok)
        enter_fallback_for_generation(ctx, generation);
}
