/* Niri-only bounded payload read. Do not change the common producer transport
 * while the other desktop backends still depend on its current API. */
#ifndef ANLAND_NIRI_BOUNDED_INPUT_READ_H
#define ANLAND_NIRI_BOUNDED_INPUT_READ_H
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/socket.h>
#include <time.h>

static inline int anland_niri_read_exact(int fd, void *buf, size_t size, int timeout_ms)
{
    if (fd < 0 || (!buf && size) || timeout_ms < 0) return -1;
    if (!size) return 1;
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
    int64_t deadline = (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000 + timeout_ms;
    size_t done = 0;
    while (done < size) {
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) return -1;
        int64_t remaining = deadline - ((int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000);
        if (remaining <= 0) return -1;
        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int rc = poll(&pfd, 1, remaining > INT_MAX ? INT_MAX : (int)remaining);
        if (rc < 0 && errno == EINTR) continue;
        if (rc <= 0 || !(pfd.revents & POLLIN)) return -1;
        ssize_t got = recv(fd, (char *)buf + done, size - done, MSG_DONTWAIT);
        if (got < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
        if (got <= 0) return -1;
        done += (size_t)got;
    }
    return 1;
}
#endif
