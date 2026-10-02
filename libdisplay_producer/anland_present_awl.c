#include "anland_present_internal.h"
#include "anland_buffer_registry.h"
#include "anland_window_map.h"
#include "../common/anland_present_ipc.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define ANLAND_PRESENT_AWL_MAX_PATH 108

typedef struct anland_present_awl {
    anland_present base;
    anland_scene *scene;
    int fd;
    char endpoint[ANLAND_PRESENT_AWL_MAX_PATH];
    uint64_t generation;
    uint64_t last_serial;
    bool connected;
    anland_buffer_registry *buffers;
    anland_window_map *windows;
    uint64_t tx_serial;
    uint64_t inflight_commit;
    size_t inflight_count;
    /* Partially received message state, kept across pump() calls so a peer that
     * sends a header and its payload in separate writes cannot block the caller. */
    anland_present_ipc_reader_t reader;
    unsigned char rx_payload[sizeof(anland_present_buffer_submit_t)];
    struct {
        uint64_t window_id;
        uint64_t buffer_id;
        uint64_t frame_id;
    } inflight[ANLAND_SCENE_MAX_LAYERS];
} anland_present_awl_t;

static anland_present_awl_t *awl_cast(anland_present *present)
{
    return (anland_present_awl_t *)present;
}

static const anland_present_awl_t *awl_const_cast(const anland_present *present)
{
    return (const anland_present_awl_t *)present;
}

static int awl_connect_socket(const char *endpoint)
{
    if (!endpoint || !*endpoint || strlen(endpoint) >= sizeof(((struct sockaddr_un *)0)->sun_path))
        return -1;

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;

    struct sockaddr_un address;
    memset(&address, 0, sizeof(address));
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, endpoint, strlen(endpoint) + 1);
    if (connect(fd, (const struct sockaddr *)&address, sizeof(address)) != 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static int awl_send_hello(int fd, uint64_t generation)
{
    anland_present_hello_t hello = {
        .flags = 0,
        .max_windows = ANLAND_PRESENT_MAX_WINDOWS,
        .max_damage = ANLAND_PRESENT_MAX_DAMAGE,
        .reserved = 0,
    };
    anland_present_msg_header_t header = {
        .magic = ANLAND_PRESENT_PROTOCOL_MAGIC,
        .version = ANLAND_PRESENT_PROTOCOL_VERSION,
        .type = ANLAND_PRESENT_HELLO,
        .size = sizeof(header) + sizeof(hello),
        .generation = generation,
        .serial = 1,
    };
    return anland_present_ipc_send(fd, &header, &hello, sizeof(hello), NULL, 0);
}

static int awl_recv_hello_reply(int fd, uint64_t generation)
{
    anland_present_ipc_reader_t reader;
    anland_present_ipc_reader_init(&reader);

    anland_present_hello_reply_t reply;
    anland_present_msg_header_t header;
    size_t payload_size = 0;
    size_t fd_count = 0;
    int fds[ANLAND_PRESENT_MAX_FDS] = {-1, -1, -1, -1};

    /* The handshake is synchronous but must not wait forever: a peer that accepts
     * the connection and then stalls would otherwise hang an entire compositor
     * startup. */
    const int64_t deadline = anland_present_ipc_deadline_after(5000);
    anland_present_ipc_status_t status;
    do {
        status = anland_present_ipc_reader_poll(fd, &reader, &reply, sizeof(reply),
                                                deadline, &header, &payload_size,
                                                fds, ANLAND_PRESENT_MAX_FDS,
                                                &fd_count);
    } while (status == ANLAND_PRESENT_IPC_INCOMPLETE &&
             anland_present_ipc_deadline_after(0) < deadline);

    if (status != ANLAND_PRESENT_IPC_MESSAGE) {
        anland_present_ipc_reader_reset(&reader);
        anland_present_ipc_close_fds(fds, fd_count);
        return -1;
    }
    anland_present_ipc_close_fds(fds, fd_count);
    if (header.type != ANLAND_PRESENT_HELLO_REPLY ||
        header.generation != generation || payload_size != sizeof(reply) ||
        reply.status != 0)
        return -1;
    return 0;
}

static int awl_handshake(anland_present_awl_t *awl)
{
    if (awl_send_hello(awl->fd, awl->generation) != 0 ||
        awl_recv_hello_reply(awl->fd, awl->generation) != 0)
        return -1;
    awl->last_serial = 1;
    awl->connected = true;
    return 0;
}

static void awl_close(anland_present_awl_t *awl)
{
    if (awl->fd >= 0)
        close(awl->fd);
    awl->fd = -1;
    awl->connected = false;
}

/* A transport loss invalidates the in-flight scene commit and its registry
 * entries before a later reconnect can accept new work. */
static void awl_drop_inflight(anland_present_awl_t *awl,
                              anland_scene_drop_reason_t reason)
{
    if (!awl || awl->inflight_commit == 0)
        return;

    (void)anland_scene_backend_dropped(awl->scene, awl->inflight_commit, reason);
    for (size_t i = 0; i < awl->inflight_count; i++)
        (void)anland_buffer_registry_remove(awl->buffers,
                                             awl->inflight[i].buffer_id);
    awl->inflight_commit = 0;
    awl->inflight_count = 0;
}

/* Tear down one consumer session: cancel the in-flight commit, close the socket
 * and drop EVERY registry entry.
 *
 * The registry holds buffers that were submitted but never committed as well as
 * the in-flight ones, and their fds stay valid (and lookups keep succeeding)
 * across a reconnect, so a session boundary must clear the whole registry rather
 * than only the inflight list. Layer identity deliberately survives: it belongs
 * to the DE, not to the transport.
 *
 * DE-side imported fds are independent dup()s and are retired through the scene
 * event path, so they must not be closed here. */
static void awl_end_session(anland_present_awl_t *awl,
                            anland_scene_drop_reason_t reason)
{
    if (!awl)
        return;
    awl_drop_inflight(awl, reason);
    awl_close(awl);
    /* A half-received message may still own fds; they belong to the session that
     * is ending and would otherwise leak or be delivered to the next one. */
    anland_present_ipc_reader_reset(&awl->reader);
    anland_buffer_registry_clear(awl->buffers);
}

static int awl_backend_submit(void *userdata, const anland_scene_snapshot_t *snapshot)
{
    anland_present_awl_t *awl = (anland_present_awl_t *)userdata;
    if (!awl || !snapshot || !awl->connected || awl->inflight_commit != 0 ||
        snapshot->count > ANLAND_SCENE_MAX_LAYERS)
        return -1;

    awl->inflight_count = 0;
    for (size_t i = 0; i < snapshot->count; i++) {
        const anland_scene_layer_snapshot_t *layer = &snapshot->layers[i];
        uint64_t window_id = 0;
        if (anland_window_map_lookup_layer(awl->windows, layer->layer_id,
                                           &window_id) != 0)
            return -1;
        if (layer->visible && layer->buffer_id != 0 &&
            !anland_buffer_registry_contains(awl->buffers, layer->buffer_id))
            return -1;
        if (!layer->visible || layer->buffer_id == 0)
            continue;
        uint64_t frame_id = 0;
        if (anland_buffer_registry_frame_id(awl->buffers, layer->buffer_id,
                                            &frame_id) != 0 || frame_id == 0)
            return -1;
        awl->inflight[awl->inflight_count].window_id = window_id;
        awl->inflight[awl->inflight_count].buffer_id = layer->buffer_id;
        awl->inflight[awl->inflight_count].frame_id = frame_id;
        awl->inflight_count++;
    }
    awl->inflight_commit = snapshot->commit_id;
    return 0;
}

static int awl_send_window_message(anland_present_awl_t *awl,
                                    uint16_t type,
                                    const void *payload,
                                    size_t payload_size)
{
    if (!awl || !awl->connected || !payload || payload_size == 0)
        return -1;
    anland_present_msg_header_t header = {
        .magic = ANLAND_PRESENT_PROTOCOL_MAGIC,
        .version = ANLAND_PRESENT_PROTOCOL_VERSION,
        .type = type,
        .size = (uint32_t)(sizeof(header) + payload_size),
        .generation = awl->generation,
        .serial = ++awl->tx_serial,
    };
    return anland_present_ipc_send(awl->fd, &header, payload, payload_size, NULL, 0);
}
static int awl_window_create(anland_present *present,
                             uint64_t window_id,
                             const anland_layer_desc_t *desc,
                             uint64_t *out_layer_id)
{
    anland_present_awl_t *awl = awl_cast(present);
    if (!awl || !desc || window_id == 0 || !out_layer_id ||
        anland_window_map_lookup_window(awl->windows, window_id, out_layer_id) == 0)
        return -1;
    anland_present_window_create_t create = {
        .window_id = window_id,
        .width = (int32_t)desc->geometry.width,
        .height = (int32_t)desc->geometry.height,
        .kind = (uint32_t)desc->kind,
        .flags = desc->visible ? 1u : 0u,
    };
    if (awl_send_window_message(awl, ANLAND_PRESENT_WINDOW_CREATE,
                                &create, sizeof(create)) != 0)
        return -1;
    anland_layer_id layer_id = 0;
    if (anland_scene_layer_create(awl->scene, desc, &layer_id) != 0 ||
        anland_window_map_bind(awl->windows, layer_id, window_id) != 0) {
        if (layer_id != 0)
            (void)anland_scene_layer_destroy(awl->scene, layer_id);
        /* The remote create was already accepted; undo it so a failed local
         * map cannot leave a window that only exists on the AWL side. */
        anland_present_window_destroy_t rollback = {.window_id = window_id};
        (void)awl_send_window_message(awl, ANLAND_PRESENT_WINDOW_DESTROY,
                                       &rollback, sizeof(rollback));
        return -1;
    }
    *out_layer_id = layer_id;
    return 0;
}
static int awl_window_update(anland_present *present,
                             uint64_t window_id,
                             const anland_layer_desc_t *desc)
{
    anland_present_awl_t *awl = awl_cast(present);
    if (!awl || !desc || window_id == 0 || !awl->connected)
        return -1;
    anland_layer_id layer_id = 0;
    if (anland_window_map_lookup_window(awl->windows, window_id, &layer_id) != 0)
        return -1;
    anland_present_window_resize_t configure = {
        .window_id = window_id,
        .width = (int32_t)desc->geometry.width,
        .height = (int32_t)desc->geometry.height,
        .scale = desc->scale ? desc->scale : 1,
        .transform = desc->transform,
    };
    if (awl_send_window_message(awl, ANLAND_PRESENT_WINDOW_CONFIGURE,
                                &configure, sizeof(configure)) != 0)
        return -1;
    return anland_scene_layer_update(awl->scene, layer_id, desc);
}
static int awl_window_destroy(anland_present *present, uint64_t window_id)
{
    anland_present_awl_t *awl = awl_cast(present);
    if (!awl || window_id == 0 || !awl->connected)
        return -1;
    anland_layer_id layer_id = 0;
    if (anland_window_map_lookup_window(awl->windows, window_id, &layer_id) != 0)
        return -1;
    anland_present_window_destroy_t destroy = {.window_id = window_id};
    if (awl_send_window_message(awl, ANLAND_PRESENT_WINDOW_DESTROY,
                                &destroy, sizeof(destroy)) != 0)
        return -1;
    if (anland_window_map_unbind_window(awl->windows, window_id, &layer_id) != 0)
        return -1;
    return anland_scene_layer_destroy(awl->scene, layer_id);
}
static void awl_scene_destroy(void *userdata)
{
    (void)userdata;
}
static void awl_destroy(anland_present *present)
{
    anland_present_awl_t *awl = awl_cast(present);
    awl_close(awl);
    /* A destroyed session must not leak fds held by a half-received message. */
    anland_present_ipc_reader_reset(&awl->reader);
    anland_buffer_registry_destroy(awl->buffers);
    anland_window_map_destroy(awl->windows);
    anland_scene_destroy(awl->scene);
    free(awl);
}

static anland_scene *awl_scene(anland_present *present)
{
    return awl_cast(present)->scene;
}
static int awl_lookup_window_layer(const anland_present *present,
                                   uint64_t window_id,
                                   uint64_t *out_layer_id)
{
    const anland_present_awl_t *awl = awl_const_cast(present);
    return awl && awl->windows ?
        anland_window_map_lookup_window(awl->windows, window_id, out_layer_id) : -1;
}
static int awl_get_buffer(const anland_present *present,
                          uint64_t buffer_id,
                          anland_buffer_desc_t *out)
{
    const anland_present_awl_t *awl = awl_const_cast(present);
    return awl && awl->buffers ?
        anland_buffer_registry_get(awl->buffers, buffer_id, out) : -1;
}
static int awl_dup_buffer_fd(const anland_present *present, uint64_t buffer_id)
{
    const anland_present_awl_t *awl = awl_const_cast(present);
    return awl && awl->buffers ?
        anland_buffer_registry_dup_fd(awl->buffers, buffer_id) : -1;
}
static anland_device *awl_device(anland_present *present)
{
    (void)present;
    return NULL;
}

static bool awl_connected(const anland_present *present)
{
    return awl_const_cast(present)->connected;
}

static int awl_reconnect(anland_present *present)
{
    anland_present_awl_t *awl = awl_cast(present);
    if (awl->connected)
        return 0;
    int fd = awl_connect_socket(awl->endpoint);
    if (fd < 0)
        return -1;
    awl->fd = fd;
    if (awl_handshake(awl) != 0) {
        awl_close(awl);
        return -1;
    }
    return 0;
}

static int awl_reopen(anland_present *present, const char *endpoint)
{
    anland_present_awl_t *awl = awl_cast(present);
    if (endpoint && *endpoint) {
        if (strlen(endpoint) >= sizeof(awl->endpoint))
            return -1;
        memcpy(awl->endpoint, endpoint, strlen(endpoint) + 1);
    }
    /* The previous consumer session is gone: its buffers are not valid for the
     * new one, so drop them together with the connection. */
    awl_end_session(awl, ANLAND_SCENE_DROP_INVALIDATED);
    awl->generation++;
    if (anland_scene_invalidate(awl->scene) != 0)
        return -1;
    return awl_reconnect(present);
}

static void awl_drop_session(anland_present *present)
{
    anland_present_awl_t *awl = awl_cast(present);
    awl_end_session(awl, ANLAND_SCENE_DROP_INVALIDATED);
    (void)anland_scene_invalidate(awl->scene);
}
static int awl_present_frame(anland_present *present)
{
    anland_present_awl_t *awl = awl_cast(present);
    if (!awl || !awl->connected || awl->inflight_commit == 0)
        return -1;

    /* The commit may already be gone without this adapter knowing: destroying a
     * layer drops any commit referencing it, and an explicit invalidate() does the
     * same. Reporting FRAME_PRESENTED / BUFFER_RELEASE for it would tell the peer
     * about a frame that no longer exists, so verify it is still in flight before
     * any success message leaves this process. The scene calls are serialized by
     * the adapter's single-threaded contract, which is what makes this check
     * conclusive. */
    if (!anland_scene_commit_is_pending(awl->scene, awl->inflight_commit))
        goto dropped;

    for (size_t i = 0; i < awl->inflight_count; i++) {
        anland_present_frame_presented_t presented = {
            .window_id = awl->inflight[i].window_id,
            .frame_id = awl->inflight[i].frame_id,
            .presentation_ns = 0,
        };
        anland_present_msg_header_t header = {
            .magic = ANLAND_PRESENT_PROTOCOL_MAGIC,
            .version = ANLAND_PRESENT_PROTOCOL_VERSION,
            .type = ANLAND_PRESENT_FRAME_PRESENTED,
            .size = sizeof(header) + sizeof(presented),
            .generation = awl->generation,
            .serial = ++awl->tx_serial,
        };
        if (anland_present_ipc_send(awl->fd, &header, &presented,
                                    sizeof(presented), NULL, 0) != 0)
            goto dropped;

        anland_present_buffer_release_t release = {
            .window_id = awl->inflight[i].window_id,
            .buffer_id = awl->inflight[i].buffer_id,
            .frame_id = awl->inflight[i].frame_id,
            .flags = 0,
            .reserved = 0,
        };
        header.type = ANLAND_PRESENT_BUFFER_RELEASE;
        header.size = sizeof(header) + sizeof(release);
        header.serial = ++awl->tx_serial;
        if (anland_present_ipc_send(awl->fd, &header, &release,
                                    sizeof(release), NULL, 0) != 0)
            goto dropped;
    }
    if (anland_scene_backend_presented(awl->scene, awl->inflight_commit, 0) != 0)
        goto dropped;

    /* The buffers of this frame are no longer in use by it, so the AWL backend
     * says so explicitly instead of relying on the scene to infer a release from
     * the presentation. Its registry entries are removed right after, because the
     * AWL peer will not submit them again. */
    for (size_t i = 0; i < awl->inflight_count; i++)
        (void)anland_scene_backend_release_buffer(awl->scene,
                                                  awl->inflight[i].buffer_id, -1);

    for (size_t i = 0; i < awl->inflight_count; i++)
        (void)anland_buffer_registry_remove(awl->buffers,
                                             awl->inflight[i].buffer_id);
    awl->inflight_commit = 0;
    awl->inflight_count = 0;
    return 0;

dropped:
    awl_end_session(awl, ANLAND_SCENE_DROP_BACKEND_ERROR);
    return -1;
}

static int awl_process_message(anland_present_awl_t *awl, int timeout_ms)
{
    /* The budget belongs to the caller: timeout_ms == 0 drains whatever has already
     * arrived without waiting, and a positive value bounds the wait for the rest
     * of a frame that is already in progress. Two invariants matter here:
     *   - a partial frame never blocks longer than the caller allowed, and
     *   - a partial frame is never abandoned just because the caller passed 0,
     *     because the reader keeps its state and the next call resumes it. */
    const int64_t deadline = anland_present_ipc_deadline_after(timeout_ms);

    anland_present_msg_header_t header;
    size_t payload_size = 0;
    size_t fd_count = 0;
    int fds[ANLAND_PRESENT_MAX_FDS] = {-1, -1, -1, -1};

    const anland_present_ipc_status_t status =
        anland_present_ipc_reader_poll(awl->fd, &awl->reader, awl->rx_payload,
                                       sizeof(awl->rx_payload), deadline,
                                       &header, &payload_size, fds,
                                       ANLAND_PRESENT_MAX_FDS, &fd_count);
    if (status == ANLAND_PRESENT_IPC_INCOMPLETE)
        return 0;
    if (status != ANLAND_PRESENT_IPC_MESSAGE) {
        /* The reader already discarded its partial state; the session cannot be
         * trusted any more. */
        awl_end_session(awl, ANLAND_SCENE_DROP_BACKEND_ERROR);
        return -1;
    }

    if (header.generation != awl->generation || header.serial <= awl->last_serial) {
        anland_present_ipc_close_fds(fds, fd_count);
        return 0;
    }
    awl->last_serial = header.serial;

    const unsigned char *payload = awl->rx_payload;
    switch (header.type) {
    case ANLAND_PRESENT_WINDOW_CREATE:
        if (fd_count != 0 || payload_size != sizeof(anland_present_window_create_t)) {
            anland_present_ipc_close_fds(fds, fd_count);
            return -1;
        }
        {
            const anland_present_window_create_t *window =
                (const anland_present_window_create_t *)payload;
            anland_layer_desc_t desc = {
                .parent_id = 0,
                .kind = window->kind == 2 ? ANLAND_LAYER_OVERLAY : ANLAND_LAYER_NORMAL,
                .name = NULL,
            };
            anland_layer_id layer_id = 0;
            if (window->window_id == 0 ||
                anland_scene_layer_create(awl->scene, &desc, &layer_id) != 0 ||
                anland_window_map_bind(awl->windows, layer_id, window->window_id) != 0) {
                if (layer_id != 0)
                    (void)anland_scene_layer_destroy(awl->scene, layer_id);
                return -1;
            }
        }
        return 0;
    case ANLAND_PRESENT_WINDOW_DESTROY:
        if (fd_count != 0 || payload_size != sizeof(anland_present_window_destroy_t)) {
            anland_present_ipc_close_fds(fds, fd_count);
            return -1;
        }
        {
            const anland_present_window_destroy_t *window =
                (const anland_present_window_destroy_t *)payload;
            anland_layer_id layer_id = 0;
            if (anland_window_map_unbind_window(awl->windows, window->window_id,
                                                &layer_id) != 0 ||
                anland_scene_layer_destroy(awl->scene, layer_id) != 0)
                return -1;
        }
        return 0;
    case ANLAND_PRESENT_WINDOW_DIRTY:
        if (fd_count != 0 || payload_size != sizeof(anland_present_window_destroy_t)) {
            anland_present_ipc_close_fds(fds, fd_count);
            return -1;
        }
        return 0;
    case ANLAND_PRESENT_BUFFER_SUBMIT:
        if (payload_size != sizeof(anland_present_buffer_submit_t) || fd_count != 1) {
            anland_present_ipc_close_fds(fds, fd_count);
            return -1;
        }
        {
            const anland_present_buffer_submit_t *submit =
                (const anland_present_buffer_submit_t *)payload;
            if (submit->window_id == 0 || submit->buffer_id == 0 ||
                submit->frame_id == 0 || submit->width == 0 || submit->height == 0 ||
                submit->stride < submit->width ||
                submit->damage_count > ANLAND_PRESENT_MAX_DAMAGE) {
                anland_present_ipc_close_fds(fds, fd_count);
                return -1;
            }
        }
        const anland_present_buffer_submit_t *submit =
            (const anland_present_buffer_submit_t *)payload;
        anland_buffer_desc_t desc = {
            .buffer_id = submit->buffer_id,
            .window_id = submit->window_id,
            .frame_id = submit->frame_id,
            .width = submit->width,
            .height = submit->height,
            .stride = submit->stride,
            .format = submit->format,
            .modifier = submit->modifier,
            .offset = submit->offset,
            .fd = fds[0],
        };
        if (anland_buffer_registry_put(awl->buffers, &desc) != 0) {
            anland_present_ipc_close_fds(fds, fd_count);
            return -1;
        }
        fds[0] = -1; /* registry now owns it */
        return 0;
    case ANLAND_PRESENT_ERROR:
        anland_present_ipc_close_fds(fds, fd_count);
        return -1;
    default:
        anland_present_ipc_close_fds(fds, fd_count);
        return 0;
    }
}

static int awl_pump(anland_present *present, int timeout_ms)
{
    anland_present_awl_t *awl = awl_cast(present);
    if (!awl)
        return -1;

    /* Same contract as the legacy adapter: pump() services the CURRENT session
     * only and never establishes a new one. A reconnect changes the session (and
     * therefore the buffer set the DE has imported), so the DE must re-import and
     * rebuild its event sources first — it does that through reconnect(), which
     * is what its own timer drives. Silently reconnecting here would hand the DE
     * a live session it is not wired to, and would hide the session change from
     * the event queue. */
    if (!awl->connected)
        return -1;

    return awl_process_message(awl, timeout_ms);
}

static const anland_present_ops_t s_awl_ops = {
    .destroy = awl_destroy,
    .scene = awl_scene,
    .lookup_window_layer = awl_lookup_window_layer,
    .window_create = awl_window_create,
    .window_update = awl_window_update,
    .window_destroy = awl_window_destroy,
    .get_buffer = awl_get_buffer,
    .dup_buffer_fd = awl_dup_buffer_fd,
    .device = awl_device,
    .connected = awl_connected,
    .reconnect = awl_reconnect,
    .reopen = awl_reopen,
    .drop_session = awl_drop_session,
    .present = awl_present_frame,
    .pump = awl_pump,
};

anland_present *anland_present_awl_create(const anland_present_config_t *config)
{
    if (!config || !config->runtime_dir || !*config->runtime_dir)
        return NULL;

    anland_present_awl_t *awl = calloc(1, sizeof(*awl));
    if (!awl)
        return NULL;
    awl->fd = -1;
    awl->generation = 1;
    /* calloc() leaves every fd slot 0, which is a VALID descriptor; the reader's
     * slots must start as -1 before anything can reset it. */
    anland_present_ipc_reader_init(&awl->reader);
    awl->buffers = anland_buffer_registry_create();
    awl->windows = anland_window_map_create();
    if (!awl->buffers || !awl->windows)
        goto fail;

    /* snprintf()'s return value is the length it WOULD have written, so it is the
     * only reliable truncation signal; strlen() of the truncated string cannot
     * detect an over-long runtime dir. */
    const int endpoint_len = snprintf(awl->endpoint, sizeof(awl->endpoint),
                                      "%s/presentation-0", config->runtime_dir);
    if (endpoint_len < 0 || (size_t)endpoint_len >= sizeof(awl->endpoint))
        goto fail;

    const anland_scene_backend_ops_t scene_ops = {
        .submit = awl_backend_submit,
        .destroy = awl_scene_destroy,
    };
    awl->scene = anland_scene_create(&scene_ops, awl);
    if (!awl->scene || awl_reconnect(&awl->base) != 0)
        goto fail;

    awl->base.backend = ANLAND_PRESENT_BACKEND_AWL;
    awl->base.ops = &s_awl_ops;
    return &awl->base;

fail:
    /* Every partially built member is released exactly once: close the socket,
     * then the scene, registry and window map, all null-tolerant. */
    awl_close(awl);
    anland_scene_destroy(awl->scene);
    anland_buffer_registry_destroy(awl->buffers);
    anland_window_map_destroy(awl->windows);
    free(awl);
    return NULL;
}