#define _GNU_SOURCE
#include "anland_present_ipc.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* Write the remainder of a frame whose first sendmsg() was accepted only
 * partially.
 *
 * send() with MSG_NOSIGNAL, not write(): a peer that disconnects mid-frame must
 * produce EPIPE and a -1 return, never SIGPIPE. The first sendmsg() already uses
 * MSG_NOSIGNAL, so using write() here would reintroduce the default disposition
 * for exactly the frames that were too large to be queued in one call - a
 * compositor killed by a client that went away is far worse than a lost frame. */
static int send_remaining(int fd, const unsigned char *data, size_t size)
{
    while (size > 0) {
        ssize_t n = send(fd, data, size, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1;
        data += (size_t)n;
        size -= (size_t)n;
    }
    return 0;
}

int anland_present_ipc_send(int fd,
                            const anland_present_msg_header_t *header,
                            const void *payload,
                            size_t payload_size,
                            const int *fds,
                            size_t fd_count)
{
    if (fd < 0 || !header || payload_size > UINT32_MAX ||
        fd_count > ANLAND_PRESENT_MAX_FDS ||
        (payload_size && !payload) || (fd_count && !fds))
        return -1;
    if (header->size != sizeof(*header) + payload_size ||
        header->size < sizeof(*header))
        return -1;

    size_t total = sizeof(*header) + payload_size;
    unsigned char *frame = malloc(total);
    if (!frame)
        return -1;
    memcpy(frame, header, sizeof(*header));
    if (payload_size)
        memcpy(frame + sizeof(*header), payload, payload_size);

    struct iovec iov = { .iov_base = frame, .iov_len = total };
    union {
        struct cmsghdr hdr;
        unsigned char data[CMSG_SPACE(sizeof(int) * ANLAND_PRESENT_MAX_FDS)];
    } control = { 0 };
    struct msghdr msg = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
    };

    if (fd_count) {
        msg.msg_control = control.data;
        msg.msg_controllen = CMSG_SPACE(sizeof(int) * fd_count);
        struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(sizeof(int) * fd_count);
        memcpy(CMSG_DATA(cmsg), fds, sizeof(int) * fd_count);
    }

    ssize_t sent;
    do {
        sent = sendmsg(fd, &msg, MSG_NOSIGNAL);
    } while (sent < 0 && errno == EINTR);
    if (sent < 0) {
        free(frame);
        return -1;
    }

    /* SCM_RIGHTS is attached to the first byte of this stream frame. If the
     * kernel accepted only a prefix, continue without ancillary data. */
    int rc = 0;
    if ((size_t)sent < total)
        rc = send_remaining(fd, frame + sent, total - (size_t)sent);
    free(frame);
    return rc;
}

static int recv_exact(int fd, unsigned char *data, size_t size)
{
    while (size > 0) {
        ssize_t n = recv(fd, data, size, 0);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (n == 0)
            return -1;
        data += (size_t)n;
        size -= (size_t)n;
    }
    return 0;
}

int anland_present_ipc_recv(int fd,
                            anland_present_msg_header_t *header,
                            void *payload,
                            size_t payload_capacity,
                            size_t *payload_size,
                            int *fds,
                            size_t fd_capacity,
                            size_t *fd_count)
{
    if (fd < 0 || !header || !payload_size || !fd_count ||
        fd_capacity > ANLAND_PRESENT_MAX_FDS)
        return -1;

    *payload_size = 0;
    *fd_count = 0;
    int received_fds[ANLAND_PRESENT_MAX_FDS];
    size_t received_fd_count = 0;
    for (size_t i = 0; i < ANLAND_PRESENT_MAX_FDS; i++)
        received_fds[i] = -1;

    struct iovec iov = { .iov_base = header, .iov_len = sizeof(*header) };
    union {
        struct cmsghdr hdr;
        unsigned char data[CMSG_SPACE(sizeof(int) * ANLAND_PRESENT_MAX_FDS)];
    } control = { 0 };
    struct msghdr msg = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = control.data,
        .msg_controllen = sizeof(control.data),
    };

    ssize_t got;
    do {
        got = recvmsg(fd, &msg, MSG_WAITALL);
    } while (got < 0 && errno == EINTR);
    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg);
         cmsg;
         cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS)
            continue;
        if (cmsg->cmsg_len < CMSG_LEN(0))
            goto fail;
        size_t bytes = cmsg->cmsg_len - CMSG_LEN(0);
        size_t count = bytes / sizeof(int);
        if (count > ANLAND_PRESENT_MAX_FDS || received_fd_count + count > ANLAND_PRESENT_MAX_FDS)
            goto fail;
        memcpy(received_fds + received_fd_count, CMSG_DATA(cmsg), count * sizeof(int));
        received_fd_count += count;
    }

    if (got != (ssize_t)sizeof(*header) || (msg.msg_flags & MSG_CTRUNC) != 0 ||
        header->magic != ANLAND_PRESENT_PROTOCOL_MAGIC ||
        header->version != ANLAND_PRESENT_PROTOCOL_VERSION ||
        header->size < sizeof(*header))
        goto fail;

    size_t size = header->size - sizeof(*header);
    if (size > payload_capacity || (size && !payload) ||
        received_fd_count > fd_capacity || (received_fd_count && !fds))
        goto fail;
    if (size && recv_exact(fd, payload, size) != 0)
        goto fail;

    if (received_fd_count)
        memcpy(fds, received_fds, received_fd_count * sizeof(int));
    *fd_count = received_fd_count;
    *payload_size = size;
    return 0;

fail:
    anland_present_ipc_close_fds(received_fds, received_fd_count);
    return -1;
}

void anland_present_ipc_close_fds(int *fds, size_t fd_count)
{
    if (!fds)
        return;
    if (fd_count > ANLAND_PRESENT_MAX_FDS)
        fd_count = ANLAND_PRESENT_MAX_FDS;
    for (size_t i = 0; i < fd_count; i++)
        if (fds[i] >= 0)
            close(fds[i]);
}

/* ---- incremental receive: framing state machine ---- */

void anland_present_ipc_reader_init(anland_present_ipc_reader_t *reader)
{
    if (!reader)
        return;
    memset(reader, 0, sizeof(*reader));
    for (size_t i = 0; i < ANLAND_PRESENT_MAX_FDS; i++)
        reader->fds[i] = -1;
}

void anland_present_ipc_reader_reset(anland_present_ipc_reader_t *reader)
{
    if (!reader)
        return;
    anland_present_ipc_close_fds(reader->fds, reader->fd_count);
    memset(reader, 0, sizeof(*reader));
    for (size_t i = 0; i < ANLAND_PRESENT_MAX_FDS; i++)
        reader->fds[i] = -1;
}

/* Close (and forget) the fds collected so far, keeping the framing state. Used by
 * the failure paths inside fill_header(): the message is being rejected, but the
 * descriptors the kernel already delivered are open in THIS process and would
 * leak without this. The caller finishes the teardown with reader_reset(). */
static void reader_close_fds(anland_present_ipc_reader_t *reader)
{
    anland_present_ipc_close_fds(reader->fds, reader->fd_count);
    reader->fd_count = 0;
    for (size_t i = 0; i < ANLAND_PRESENT_MAX_FDS; i++)
        reader->fds[i] = -1;
}

bool anland_present_ipc_reader_idle(const anland_present_ipc_reader_t *reader)
{
    return reader ? reader->header_bytes == 0 && !reader->have_header : true;
}

int64_t anland_present_ipc_deadline_after(int timeout_ms)
{
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
        return 0;
    int64_t deadline = (int64_t)now.tv_sec * 1000000000LL + now.tv_nsec;
    if (timeout_ms > 0)
        deadline += (int64_t)timeout_ms * 1000000LL;
    return deadline;
}

static int64_t now_ns(void)
{
    return anland_present_ipc_deadline_after(0);
}

/* Close every descriptor carried by an already-received control message.
 *
 * recvmsg() delivers SCM_RIGHTS fds into THIS process as soon as they fit in the
 * control buffer. When the message is then rejected (truncated control data,
 * malformed header, too many fds), nobody else will ever own those descriptors,
 * so the rejection path must close them or leak one dmabuf per bad send. */
static void close_received_fds(const struct msghdr *msg)
{
    if (!msg)
        return;
    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(msg); cmsg;
         cmsg = CMSG_NXTHDR((struct msghdr *)msg, cmsg)) {
        if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS)
            continue;
        if (cmsg->cmsg_len < CMSG_LEN(0))
            continue;
        const size_t bytes = cmsg->cmsg_len - CMSG_LEN(0);
        const size_t count = bytes / sizeof(int);
        const int *fds = (const int *)CMSG_DATA(cmsg);
        for (size_t i = 0; i < count; i++) {
            if (fds[i] >= 0)
                close(fds[i]);
        }
    }
}

/* Remaining budget in whole milliseconds, for poll(); 0 means "do not block". */
static int remaining_ms(int64_t deadline_ns)
{
    const int64_t left = deadline_ns - now_ns();
    if (left <= 0)
        return 0;
    const int64_t ms = (left + 999999LL) / 1000000LL;
    return ms > INT32_MAX ? INT32_MAX : (int)ms;
}

/* Wait until the socket is readable, or the budget runs out. */
static int wait_readable(int fd, int64_t deadline_ns)
{
    for (;;) {
        struct pollfd pfd = {.fd = fd, .events = POLLIN};
        int rc = poll(&pfd, 1, remaining_ms(deadline_ns));
        if (rc < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (rc == 0)
            return 0;
        if (pfd.revents & POLLNVAL)
            return -1;
        if (!(pfd.revents & POLLIN) && (pfd.revents & (POLLERR | POLLHUP)))
            return -1;
        if (pfd.revents & POLLIN)
            return 1;
        return 0;
    }
}

/* Receive header bytes up to the point where the length is known. Returns the
 * number of bytes appended, or -1 on error. */
static ssize_t fill_header(int fd, anland_present_ipc_reader_t *reader)
{
    size_t space = sizeof(reader->header) - reader->header_bytes;
    if (space == 0)
        return 0;

    /* Only the header arrives without payload; ancillary data rides with it. */
    union {
        struct cmsghdr hdr;
        unsigned char data[CMSG_SPACE(sizeof(int) * ANLAND_PRESENT_MAX_FDS)];
    } control = { 0 };
    struct iovec iov = {
        .iov_base = (unsigned char *)&reader->header + reader->header_bytes,
        .iov_len = space,
    };
    struct msghdr msg = {
        .msg_iov = &iov,
        .msg_iovlen = 1,
        .msg_control = control.data,
        .msg_controllen = sizeof(control.data),
    };

    ssize_t got;
    do {
        got = recvmsg(fd, &msg, MSG_DONTWAIT);
    } while (got < 0 && errno == EINTR);
    if (got < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return 0;
        return -1;
    }
    if (got == 0)
        return -1; /* peer closed */

    /* Collect FDs. They are only accepted with the first bytes of the header,
     * and only when no message is already in progress that owns fd slots. */
    if ((msg.msg_flags & MSG_CTRUNC) != 0) {
        /* The control buffer was truncated: the kernel closed whatever did not
         * fit, but the fds that DID fit are already open in this process. They
         * belong to a message we are about to reject, so close them here instead
         * of leaking one dmabuf per malformed send. */
        close_received_fds(&msg);
        return -1;
    }
    for (struct cmsghdr *cmsg = CMSG_FIRSTHDR(&msg); cmsg;
         cmsg = CMSG_NXTHDR(&msg, cmsg)) {
        if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS)
            continue;
        if (cmsg->cmsg_len < CMSG_LEN(0)) {
            close_received_fds(&msg);
            return -1;
        }
        const size_t bytes = cmsg->cmsg_len - CMSG_LEN(0);
        const size_t count = bytes / sizeof(int);
        if (count > ANLAND_PRESENT_MAX_FDS ||
            reader->fd_count + count > ANLAND_PRESENT_MAX_FDS) {
            /* Rejecting the message must not leak the fds it carried, and the
             * fds accepted so far are equally unusable (the message is dead).
             * recvmsg() already consumed the bytes, so the frame can never be
             * completed: the caller must treat this as a protocol failure. */
            close_received_fds(&msg);
            reader_close_fds(reader);
            return -1;
        }
        memcpy(reader->fds + reader->fd_count, CMSG_DATA(cmsg), count * sizeof(int));
        reader->fd_count += count;
    }

    reader->header_bytes += (size_t)got;
    if (reader->header_bytes < sizeof(reader->header))
        return 0; /* keep waiting for the rest of the header */

    if (reader->header.magic != ANLAND_PRESENT_PROTOCOL_MAGIC ||
        reader->header.version != ANLAND_PRESENT_PROTOCOL_VERSION ||
        reader->header.size < sizeof(reader->header)) {
        /* Malformed frame: its descriptors are already open here, and the
         * message that would have owned them is being rejected. */
        reader_close_fds(reader);
        return -1;
    }

    reader->payload_size = reader->header.size - sizeof(reader->header);
    reader->have_header = true;
    return 0;
}

/* Receive payload bytes into the caller's persistent buffer. */
static int fill_payload(int fd, anland_present_ipc_reader_t *reader,
                        unsigned char *payload, size_t payload_capacity)
{
    while (reader->payload_bytes < reader->payload_size) {
        if (reader->payload_size > payload_capacity)
            return -1;
        ssize_t got = recv(fd, payload + reader->payload_bytes,
                           reader->payload_size - reader->payload_bytes,
                           MSG_DONTWAIT);
        if (got < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            return -1;
        }
        if (got == 0)
            return -1; /* peer closed mid-message */
        reader->payload_bytes += (size_t)got;
    }
    return 0;
}

static anland_present_ipc_status_t reader_fail(anland_present_ipc_reader_t *reader)
{
    /* A partial message is discarded, but the fds that arrived with it are
     * closed here: otherwise a half-sent BUFFER_SUBMIT would leak its dmabuf. */
    anland_present_ipc_reader_reset(reader);
    return ANLAND_PRESENT_IPC_ERROR;
}

anland_present_ipc_status_t anland_present_ipc_reader_poll(
    int fd,
    anland_present_ipc_reader_t *reader,
    void *payload,
    size_t payload_capacity,
    int64_t deadline_ns,
    anland_present_msg_header_t *out_header,
    size_t *out_payload_size,
    int *out_fds,
    size_t fd_capacity,
    size_t *out_fd_count)
{
    if (fd < 0 || !reader || !out_header || !out_payload_size || !out_fd_count ||
        fd_capacity > ANLAND_PRESENT_MAX_FDS || (fd_capacity && !out_fds))
        return ANLAND_PRESENT_IPC_ERROR;

    *out_payload_size = 0;
    *out_fd_count = 0;

    unsigned char *payload_bytes = payload;

    /* Bounded: every iteration either consumes bytes or waits for readability
     * with the remaining budget, so this cannot spin. The cap only guards
     * against a peer that keeps a partial message dribbling in forever. */
    for (int guard = 0; guard < 64; guard++) {
        const bool need_header = !reader->have_header;
        const bool need_payload = reader->have_header &&
                                  reader->payload_bytes < reader->payload_size;

        if (need_header || need_payload) {
            const int ready = wait_readable(fd, deadline_ns);
            if (ready < 0)
                return reader_fail(reader);
            if (ready == 0)
                return ANLAND_PRESENT_IPC_INCOMPLETE;
        }

        if (!reader->have_header) {
            if (fill_header(fd, reader) < 0)
                return reader_fail(reader);
            if (!reader->have_header)
                continue; /* partial header: wait for the rest */
            if (reader->payload_size > payload_capacity)
                return reader_fail(reader);
        }

        if (fill_payload(fd, reader, payload_bytes, payload_capacity) != 0)
            return reader_fail(reader);
        if (reader->payload_bytes < reader->payload_size)
            continue; /* partial payload: wait for the rest */

        /* Complete message: hand it over and reset to the idle state. */
        if (reader->fd_count > fd_capacity)
            return reader_fail(reader);
        *out_header = reader->header;
        *out_payload_size = reader->payload_size;
        for (size_t i = 0; i < reader->fd_count; i++)
            out_fds[i] = reader->fds[i];
        *out_fd_count = reader->fd_count;

        memset(reader, 0, sizeof(*reader));
        for (size_t i = 0; i < ANLAND_PRESENT_MAX_FDS; i++)
            reader->fds[i] = -1;
        return ANLAND_PRESENT_IPC_MESSAGE;
    }

    /* The peer is trickling one message without ever finishing it. */
    return ANLAND_PRESENT_IPC_INCOMPLETE;
}