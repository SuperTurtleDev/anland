#define _GNU_SOURCE
#include "../common/protocol.h"
#include "../common/socket_utils.h"
#include "../libdisplay_consumer/display_consumer.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define STRESS_ITERATIONS 4000
#define IO_TIMEOUT_SECONDS 10
#define BLOCKED_WRITE_BYTES (16U * 1024U * 1024U)

struct fixture {
    display_ctx *ctx;
    int ctrl_peer;
    int ready_fd;
    int fence_peer;
    int data_peer;
    int shm_fd;
    int audio_peer;
    int dmabuf_fd;
};

struct writer_args {
    display_ctx *ctx;
    int dmabuf_fd;
    const struct buf_info *info;
    int result;
};

struct reader_args {
    int data_fd;
    const struct buf_info *info;
    int result;
};

struct blocked_writer_args {
    display_ctx *ctx;
    const void *payload;
    size_t size;
    int result;
};

static void close_fd(int *fd)
{
    if (*fd >= 0) {
        close(*fd);
        *fd = -1;
    }
}

static int recv_header_with_fds(int fd, void *header, size_t header_size,
                                int *fds, int fd_capacity, int *fd_count)
{
    size_t received = 0;
    *fd_count = 0;
    while (received < header_size) {
        char control[CMSG_SPACE(sizeof(int) * MAX_BUFS)] = {0};
        struct iovec iov = {
            .iov_base = (uint8_t *)header + received,
            .iov_len = header_size - received,
        };
        struct msghdr msg = {
            .msg_iov = &iov,
            .msg_iovlen = 1,
            .msg_control = control,
            .msg_controllen = sizeof(control),
        };

        ssize_t n;
        do {
            n = recvmsg(fd, &msg, MSG_CMSG_CLOEXEC);
        } while (n < 0 && errno == EINTR);
        if (n <= 0 || (msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)))
            return -1;
        received += (size_t)n;

        for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg); cmsg;
             cmsg = CMSG_NXTHDR(&msg, cmsg)) {
            if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS ||
                cmsg->cmsg_len < CMSG_LEN(0))
                return -1;
            const int count = (int)((cmsg->cmsg_len - CMSG_LEN(0)) / sizeof(int));
            if (*fd_count + count > fd_capacity)
                return -1;
            memcpy(fds + *fd_count, CMSG_DATA(cmsg), (size_t)count * sizeof(int));
            *fd_count += count;
        }
    }
    return 0;
}

static void close_received_fds(int *fds, int count)
{
    for (int i = 0; i < count; i++)
        close(fds[i]);
}

static int receive_hello(struct fixture *fixture)
{
    struct ctrl_msg hello;
    int hello_fds[MAX_BUFS];
    int hello_fd_count = 0;
    if (recv_header_with_fds(fixture->ctrl_peer, &hello, sizeof(hello),
                             hello_fds, MAX_BUFS, &hello_fd_count) < 0)
        return -1;
    if (hello.type != CTRL_MSG_CONSUMER_HELLO || hello.size != 0 ||
        hello_fd_count != 5) {
        close_received_fds(hello_fds, hello_fd_count);
        return -1;
    }

    close_fd(&fixture->ready_fd);
    close_fd(&fixture->fence_peer);
    close_fd(&fixture->data_peer);
    close_fd(&fixture->shm_fd);
    close_fd(&fixture->audio_peer);
    fixture->ready_fd = hello_fds[0];
    fixture->fence_peer = hello_fds[1];
    fixture->data_peer = hello_fds[2];
    fixture->shm_fd = hello_fds[3];
    fixture->audio_peer = hello_fds[4];

    const struct timeval timeout = { .tv_sec = IO_TIMEOUT_SECONDS };
    return setsockopt(fixture->data_peer, SOL_SOCKET, SO_RCVTIMEO,
                      &timeout, sizeof(timeout));
}

static int send_output_event(int data_fd, const struct OutputEvent *event)
{
    struct data_msg header = {
        .type = DATA_MSG_OUTPUT_EVENT,
        .size = sizeof(*event),
    };
    uint8_t message[sizeof(header) + sizeof(*event)];
    memcpy(message, &header, sizeof(header));
    memcpy(message + sizeof(header), event, sizeof(*event));
    return send_all(data_fd, message, sizeof(message));
}

static int recv_buf_record(int data_fd, const struct buf_info *expected)
{
    struct data_msg header;
    int fds[MAX_BUFS];
    int fd_count = 0;
    if (recv_header_with_fds(data_fd, &header, sizeof(header),
                             fds, MAX_BUFS, &fd_count) < 0)
        return -1;

    if (header.type != DATA_MSG_BUFS_READY ||
        header.size != sizeof(struct buf_info) || fd_count != 1) {
        close_received_fds(fds, fd_count);
        return -1;
    }

    struct buf_info actual;
    const int result = recv_all(data_fd, &actual, sizeof(actual));
    close_received_fds(fds, fd_count);
    return result == 0 && memcmp(&actual, expected, sizeof(actual)) == 0 ? 0 : -1;
}

static int complete_frame(struct fixture *fixture)
{
    struct pollfd pfd = { .fd = fixture->ready_fd, .events = POLLIN };
    if (poll(&pfd, 1, 3000) != 1 || !(pfd.revents & POLLIN))
        return -1;

    eventfd_t value = 0;
    if (eventfd_read(fixture->ready_fd, &value) < 0 || value == 0)
        return -1;
    if (send_all(fixture->fence_peer, "x", 1) < 0)
        return -1;

    int fence = -1;
    if (refresh_done(fixture->ctx, &fence) < 0)
        return -1;
    if (fence >= 0)
        close(fence);
    return 0;
}

static void *input_writer(void *userdata)
{
    struct writer_args *args = userdata;
    struct InputEvent event = {0};
    event.type = INPUT_TYPE_POINTER_MOTION;
    event.pointer_motion.x = 17.0f;
    event.pointer_motion.y = 29.0f;
    event.pointer_motion.dx = 3.0f;
    event.pointer_motion.dy = -5.0f;

    args->result = 0;
    for (int i = 0; i < STRESS_ITERATIONS; i++) {
        if (push_input_event(args->ctx, &event) < 0) {
            args->result = -1;
            break;
        }
    }
    return NULL;
}

static void *dmabuf_writer(void *userdata)
{
    struct writer_args *args = userdata;
    args->result = 0;
    for (int i = 0; i < STRESS_ITERATIONS; i++) {
        if (push_dmabufs(args->ctx, &args->dmabuf_fd, args->info, 1) < 0) {
            args->result = -1;
            break;
        }
    }
    return NULL;
}

static void *blocked_writer(void *userdata)
{
    struct blocked_writer_args *args = userdata;
    struct InputEvent event = {0};
    event.type = INPUT_TYPE_CLIPBOARD;
    event.clipboard.size = (uint32_t)args->size;
    args->result =
        push_input_event_with_length(args->ctx, &event,
                                     (void *)args->payload, args->size);
    return NULL;
}

static void *data_reader(void *userdata)
{
    struct reader_args *args = userdata;
    int input_count = 0;
    int buf_count = 0;
    args->result = -1;

    while (input_count < STRESS_ITERATIONS || buf_count < STRESS_ITERATIONS) {
        struct data_msg header;
        int fds[MAX_BUFS];
        int fd_count = 0;
        if (recv_header_with_fds(args->data_fd, &header, sizeof(header),
                                 fds, MAX_BUFS, &fd_count) < 0)
            return NULL;

        if (header.type == DATA_MSG_INPUT_EVENT) {
            struct InputEvent event;
            if (header.size != sizeof(event) || fd_count != 0 ||
                recv_all(args->data_fd, &event, sizeof(event)) < 0 ||
                event.type != INPUT_TYPE_POINTER_MOTION)
                return NULL;
            input_count++;
            continue;
        }

        if (header.type == DATA_MSG_BUFS_READY) {
            struct buf_info info;
            if (header.size != sizeof(info) || fd_count != 1 ||
                recv_all(args->data_fd, &info, sizeof(info)) < 0) {
                close_received_fds(fds, fd_count);
                return NULL;
            }
            close_received_fds(fds, fd_count);
            if (memcmp(&info, args->info, sizeof(info)) != 0)
                return NULL;
            buf_count++;
            continue;
        }

        close_received_fds(fds, fd_count);
        return NULL;
    }

    args->result = 0;
    return NULL;
}

static void cleanup_fixture(struct fixture *fixture)
{
    if (fixture->ctx) {
        disconnect(fixture->ctx);
        fixture->ctx = NULL;
    }
    close_fd(&fixture->ctrl_peer);
    close_fd(&fixture->ready_fd);
    close_fd(&fixture->fence_peer);
    close_fd(&fixture->data_peer);
    close_fd(&fixture->shm_fd);
    close_fd(&fixture->audio_peer);
    close_fd(&fixture->dmabuf_fd);
}

int main(void)
{
    int status = 1;
    struct fixture fixture = {
        .ctrl_peer = -1,
        .ready_fd = -1,
        .fence_peer = -1,
        .data_peer = -1,
        .shm_fd = -1,
        .audio_peer = -1,
        .dmabuf_fd = -1,
    };
    int ctrl[2] = {-1, -1};

#define REQUIRE(condition, message) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "display_consumer_tests: %s\n", message); \
            goto cleanup; \
        } \
    } while (0)

    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, ctrl) == 0,
            "control socketpair failed");
    fixture.ctrl_peer = ctrl[1];
    REQUIRE(connect_to_deamon_with_fd(&fixture.ctx, ctrl[0]) == 0,
            "consumer connection failed");
    ctrl[0] = -1;

    struct timeval timeout = { .tv_sec = IO_TIMEOUT_SECONDS };
    REQUIRE(setsockopt(fixture.ctrl_peer, SOL_SOCKET, SO_RCVTIMEO,
                       &timeout, sizeof(timeout)) == 0,
            "control timeout setup failed");

    REQUIRE(receive_hello(&fixture) == 0,
            "consumer hello was malformed");

    fixture.dmabuf_fd = memfd_create("display-consumer-test", MFD_CLOEXEC);
    REQUIRE(fixture.dmabuf_fd >= 0, "test dma-buf fd creation failed");

    const struct buf_info first_info = {
        .stride = 640,
        .width = 160,
        .height = 90,
        .format = 0x34325241,
        .modifier = UINT64_C(0x0102030405060708),
        .offset = 64,
    };
    const struct buf_info active_info = {
        .stride = 1280,
        .width = 320,
        .height = 180,
        .format = 0x34325258,
        .modifier = UINT64_C(0x8877665544332211),
        .offset = 128,
    };

    REQUIRE(push_dmabufs(fixture.ctx, &fixture.dmabuf_fd, &first_info, 1) == 0,
            "initial dma-buf store failed");
    const struct ctrl_msg ready = { .type = CTRL_MSG_FDS_READY, .size = 0 };
    REQUIRE(send_all(fixture.ctrl_peer, &ready, sizeof(ready)) == 0,
            "FDS_READY send failed");
    REQUIRE(select_dmabuf(fixture.ctx, 0) == 0,
            "initial fallback exit failed");
    REQUIRE(recv_buf_record(fixture.data_peer, &first_info) == 0,
            "initial BUFS_READY frame was malformed");
    REQUIRE(complete_frame(&fixture) == 0, "initial frame completion failed");

    REQUIRE(push_dmabufs(fixture.ctx, &fixture.dmabuf_fd, &active_info, 1) == 0,
            "active dma-buf republish failed");
    REQUIRE(recv_buf_record(fixture.data_peer, &active_info) == 0,
            "active BUFS_READY frame was malformed");
    REQUIRE(select_dmabuf(fixture.ctx, 0) == 0,
            "active republish incorrectly entered fallback");
    REQUIRE(complete_frame(&fixture) == 0,
            "active republish frame completion failed");

    const int owned_data_fd = get_data_fd(fixture.ctx);
    REQUIRE(owned_data_fd >= 0, "active data fd duplicate failed");
    close(owned_data_fd);

    struct writer_args input_args = {
        .ctx = fixture.ctx,
        .dmabuf_fd = fixture.dmabuf_fd,
        .info = &active_info,
        .result = -1,
    };
    struct writer_args dmabuf_args = input_args;
    struct reader_args reader_args = {
        .data_fd = fixture.data_peer,
        .info = &active_info,
        .result = -1,
    };
    pthread_t input_thread;
    pthread_t dmabuf_thread;
    pthread_t reader_thread;

    REQUIRE(pthread_create(&reader_thread, NULL, data_reader, &reader_args) == 0,
            "reader thread creation failed");
    REQUIRE(pthread_create(&input_thread, NULL, input_writer, &input_args) == 0,
            "input writer creation failed");
    REQUIRE(pthread_create(&dmabuf_thread, NULL, dmabuf_writer, &dmabuf_args) == 0,
            "dma-buf writer creation failed");
    REQUIRE(pthread_join(input_thread, NULL) == 0, "input writer join failed");
    REQUIRE(pthread_join(dmabuf_thread, NULL) == 0, "dma-buf writer join failed");
    REQUIRE(pthread_join(reader_thread, NULL) == 0, "reader join failed");
    REQUIRE(input_args.result == 0 && dmabuf_args.result == 0 &&
            reader_args.result == 0,
            "concurrent data stream framing failed");

    REQUIRE(select_dmabuf(fixture.ctx, 0) == 0,
            "session fell back during concurrent writes");
    REQUIRE(complete_frame(&fixture) == 0,
            "post-stress frame completion failed");

    /* A clipboard header retains its old-generation socket. Reconnect before
     * draining it, queue a new-generation event, and prove the stale drain does
     * not consume any bytes from the new stream. */
    const struct OutputEvent old_clipboard = {
        .type = OUTPUT_TYPE_CLIPBOARD,
        .clipboard = { .size = 4 },
    };
    REQUIRE(send_output_event(fixture.data_peer, &old_clipboard) == 0,
            "old clipboard header send failed");
    struct OutputEvent received_event;
    REQUIRE(poll_output_event(fixture.ctx, &received_event, 1000) == 1 &&
            received_event.type == OUTPUT_TYPE_CLIPBOARD,
            "old clipboard header receive failed");

    close_fd(&fixture.data_peer);
    struct InputEvent reconnect_event = { .type = INPUT_TYPE_TOUCH_FRAME };
    REQUIRE(push_input_event(fixture.ctx, &reconnect_event) < 0,
            "closed data channel did not enter fallback");
    REQUIRE(receive_hello(&fixture) == 0,
            "reconnect hello was malformed");
    REQUIRE(send_all(fixture.ctrl_peer, &ready, sizeof(ready)) == 0,
            "reconnect FDS_READY send failed");
    REQUIRE(select_dmabuf(fixture.ctx, 0) == 0,
            "reconnect fallback exit failed");
    REQUIRE(recv_buf_record(fixture.data_peer, &active_info) == 0,
            "reconnect BUFS_READY frame was malformed");
    REQUIRE(complete_frame(&fixture) == 0,
            "reconnect frame completion failed");

    const struct OutputEvent new_event = {
        .type = OUTPUT_TYPE_SET_CONSUMER_VAR,
        .set_consumer_var = {
            .var = CONSUMER_VAR_CAPTURE_MOUSE,
            .value = 1,
        },
    };
    REQUIRE(send_output_event(fixture.data_peer, &new_event) == 0,
            "new-generation output event send failed");
    char stale_payload[4];
    REQUIRE(poll_output_event_extend_data(
                fixture.ctx, stale_payload, sizeof(stale_payload), 100) != 1,
            "stale payload drain consumed the new generation");
    REQUIRE(poll_output_event(fixture.ctx, &received_event, 1000) == 1 &&
            received_event.type == OUTPUT_TYPE_SET_CONSUMER_VAR &&
            received_event.set_consumer_var.var ==
                CONSUMER_VAR_CAPTURE_MOUSE &&
            received_event.set_consumer_var.value == 1,
            "new-generation event was corrupted by stale payload drain");

    /* A writer blocked on an undrained data socket must not hold state_lock.
     * Closing the fence peer forces fallback, whose shutdown wakes the writer. */
    const int send_fd = get_data_fd(fixture.ctx);
    REQUIRE(send_fd >= 0, "blocked-writer data fd duplicate failed");
    const int send_buffer_size = 4096;
    REQUIRE(setsockopt(send_fd, SOL_SOCKET, SO_SNDBUF,
                       &send_buffer_size, sizeof(send_buffer_size)) == 0,
            "blocked-writer send buffer setup failed");
    close(send_fd);
    REQUIRE(select_dmabuf(fixture.ctx, 0) == 0,
            "blocked-writer frame selection failed");

    void *blocked_payload = calloc(1, BLOCKED_WRITE_BYTES);
    REQUIRE(blocked_payload != NULL, "blocked-writer payload allocation failed");
    struct blocked_writer_args blocked_args = {
        .ctx = fixture.ctx,
        .payload = blocked_payload,
        .size = BLOCKED_WRITE_BYTES,
        .result = 0,
    };
    pthread_t blocked_thread;
    REQUIRE(pthread_create(&blocked_thread, NULL, blocked_writer,
                           &blocked_args) == 0,
            "blocked writer creation failed");
    usleep(200000);
    close_fd(&fixture.fence_peer);
    const int fallback_result = refresh_done(fixture.ctx, &(int){-1});
    const int blocked_join_result = pthread_join(blocked_thread, NULL);
    free(blocked_payload);
    REQUIRE(fallback_result < 0, "fence loss did not enter fallback");
    REQUIRE(blocked_join_result == 0 && blocked_args.result < 0,
            "fallback did not release blocked data writer");

    status = 0;
    printf("display_consumer_tests: active republish, concurrent framing, "
           "generation-safe payload, and blocked-writer recovery passed\n");

cleanup:
    if (ctrl[0] >= 0)
        close(ctrl[0]);
    cleanup_fixture(&fixture);
    return status;
}
