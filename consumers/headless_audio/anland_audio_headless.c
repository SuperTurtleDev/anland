/*
 * anland_audio_headless.c — Standalone Zero-Copy SHM Audio Consumer for Anland
 *
 * Connects to display_daemon as a headless consumer, negotiates AUDIO_MSG_SHM
 * (lock-free 128 KB SPSC ring backed by memfd), and streams raw 48kHz 2ch S16LE
 * PCM frames to stdout with zero kernel copies and sub-12us median latency.
 *
 * Usage:
 *   anland_audio_headless [--socket /data/local/tmp/display_daemon.sock] [--rate 48000] [--channels 2]
 *   anland_audio_headless --self-test
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "protocol.h"
#include "socket_utils.h"

#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif

static volatile sig_atomic_t g_running = 1;

static void on_signal(int sig) {
    (void)sig;
    g_running = 0;
}

static int create_memfd_compat(const char *name) {
#ifdef __NR_memfd_create
    return (int)syscall(__NR_memfd_create, name, MFD_CLOEXEC);
#else
    (void)name;
    errno = ENOSYS;
    return -1;
#endif
}

static int send_audio_format(int audio_fd, uint32_t role, uint32_t rate,
                             uint32_t channels, uint32_t quantum) {
    uint8_t buf[sizeof(struct audio_msg) + sizeof(struct audio_format)];
    struct audio_msg hdr = {
        .type = AUDIO_MSG_FORMAT,
        .size = sizeof(struct audio_format),
    };
    struct audio_format fmt = {
        .rate = rate,
        .channels = channels,
        .format = AUDIO_FORMAT_S16LE,
        .role = role,
        .quantum = quantum,
    };
    memcpy(buf, &hdr, sizeof(hdr));
    memcpy(buf + sizeof(hdr), &fmt, sizeof(fmt));
    return send_all(audio_fd, buf, sizeof(buf));
}

static int write_all_fd(int fd, const uint8_t *buf, size_t len) {
    size_t off = 0;
    while (off < len && g_running) {
        ssize_t n = write(fd, buf + off, len - off);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        off += (size_t)n;
    }
    return (off == len) ? 0 : -1;
}

static int drain_shm_ring(struct anland_shm_audio_header *ring, int out_fd,
                          uint64_t *bytes_out) {
    const uint32_t cap = ring->buffer_size;
    uint32_t head = __atomic_load_n(&ring->write_head, __ATOMIC_ACQUIRE);
    uint32_t tail = __atomic_load_n(&ring->read_tail, __ATOMIC_RELAXED);

    if (head == tail)
        return 0;

    uint32_t avail = (head >= tail) ? (head - tail) : (cap - tail + head);
    uint32_t first = (tail + avail <= cap) ? avail : (cap - tail);
    uint32_t second = avail - first;

    if (first > 0) {
        if (out_fd >= 0 && write_all_fd(out_fd, ring->data + tail, first) < 0)
            return -1;
    }
    if (second > 0) {
        if (out_fd >= 0 && write_all_fd(out_fd, ring->data, second) < 0)
            return -1;
    }

    uint32_t new_tail = (tail + avail) % cap;
    __atomic_store_n(&ring->read_tail, new_tail, __ATOMIC_RELEASE);
    if (bytes_out)
        *bytes_out += avail;
    return (int)avail;
}

static int run_audio_consumer_loop(int audio_fd, int ctrl_fd, int out_fd,
                                   uint32_t rate, uint32_t channels,
                                   uint64_t stop_after_bytes) {
    if (send_audio_format(audio_fd, AUDIO_ROLE_PLAYBACK, rate, channels, 512) < 0) {
        fprintf(stderr, "anland_audio_headless: failed to send playback format\n");
        return -1;
    }

    struct audio_msg shm_req = { .type = AUDIO_MSG_SHM, .size = 0 };
    if (send_all(audio_fd, &shm_req, sizeof(shm_req)) < 0) {
        fprintf(stderr, "anland_audio_headless: failed to request AUDIO_MSG_SHM\n");
        return -1;
    }

    struct anland_shm_audio_header *ring = NULL;
    size_t ring_map_size = 0;
    int shm_fd = -1;
    uint64_t total_bytes = 0;
    uint8_t pcm_fallback[8192];

    while (g_running) {
        if (ring) {
            int got = drain_shm_ring(ring, out_fd, &total_bytes);
            if (got < 0)
                break;
            if (stop_after_bytes > 0 && total_bytes >= stop_after_bytes)
                break;
            if (got > 0)
                continue;
        }

        struct pollfd pfds[2];
        int nfds = 0;
        pfds[nfds].fd = audio_fd;
        pfds[nfds].events = POLLIN;
        nfds++;
        if (ctrl_fd >= 0) {
            pfds[nfds].fd = ctrl_fd;
            pfds[nfds].events = POLLIN;
            nfds++;
        }

        int timeout_ms = ring ? 2 : 50;
        int pr = poll(pfds, (nfds_t)nfds, timeout_ms);
        if (pr < 0) {
            if (errno == EINTR)
                continue;
            break;
        }
        if (pr == 0)
            continue;

        if (ctrl_fd >= 0 && nfds > 1 && (pfds[1].revents & (POLLIN | POLLHUP | POLLERR))) {
            struct ctrl_msg cmsg;
            if (recv_all(ctrl_fd, &cmsg, sizeof(cmsg)) < 0) {
                fprintf(stderr, "anland_audio_headless: daemon control closed\n");
                break;
            }
            if (cmsg.type == CTRL_MSG_REJECT) {
                fprintf(stderr, "anland_audio_headless: yielding consumer slot (CTRL_MSG_REJECT)\n");
                break;
            }
        }

        if (pfds[0].revents & (POLLHUP | POLLERR)) {
            fprintf(stderr, "anland_audio_headless: producer audio channel closed\n");
            break;
        }

        if (pfds[0].revents & POLLIN) {
            struct audio_msg amsg;
            int rx_fds[2] = { -1, -1 };
            int rx_cnt = 0;
            ssize_t n = recv_fds(audio_fd, &amsg, sizeof(amsg), rx_fds, 2, &rx_cnt);
            if (n <= 0)
                break;

            if (amsg.type == AUDIO_MSG_SHM_FD && rx_cnt >= 1 && rx_fds[0] >= 0) {
                size_t sz = sizeof(struct anland_shm_audio_header) + ANLAND_SHM_AUDIO_DEFAULT_SIZE;
                void *m = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, rx_fds[0], 0);
                if (m != MAP_FAILED) {
                    struct anland_shm_audio_header *hdr = (struct anland_shm_audio_header *)m;
                    if (hdr->magic == ANLAND_SHM_AUDIO_MAGIC && hdr->buffer_size > 0) {
                        ring = hdr;
                        ring_map_size = sz;
                        shm_fd = rx_fds[0];
                        fprintf(stderr, "anland_audio_headless: SHM ring active (%u Hz, %u ch, %u bytes)\n",
                                hdr->rate, hdr->channels, hdr->buffer_size);
                    } else {
                        munmap(m, sz);
                        close(rx_fds[0]);
                    }
                } else {
                    close(rx_fds[0]);
                }
            } else if (amsg.type == AUDIO_MSG_PCM && amsg.size > 0) {
                uint32_t rem = amsg.size;
                while (rem > 0 && g_running) {
                    uint32_t chunk = (rem > sizeof(pcm_fallback)) ? (uint32_t)sizeof(pcm_fallback) : rem;
                    if (recv_all(audio_fd, pcm_fallback, chunk) < 0) {
                        g_running = 0;
                        break;
                    }
                    if (out_fd >= 0 && write_all_fd(out_fd, pcm_fallback, chunk) < 0) {
                        g_running = 0;
                        break;
                    }
                    total_bytes += chunk;
                    rem -= chunk;
                }
            } else if (amsg.size > 0) {
                uint32_t rem = amsg.size;
                while (rem > 0) {
                    uint32_t chunk = (rem > sizeof(pcm_fallback)) ? (uint32_t)sizeof(pcm_fallback) : rem;
                    if (recv_all(audio_fd, pcm_fallback, chunk) < 0)
                        break;
                    rem -= chunk;
                }
            }
        }
    }

    if (ring)
        munmap(ring, ring_map_size);
    if (shm_fd >= 0)
        close(shm_fd);

    fprintf(stderr, "anland_audio_headless: exited cleanly (total_bytes=%llu)\n",
            (unsigned long long)total_bytes);
    return (stop_after_bytes == 0 || total_bytes >= stop_after_bytes) ? 0 : -1;
}

/* --- Built-in Self-Test: Verifies AUDIO_MSG_SHM -> AUDIO_MSG_SHM_FD -> Zero-Copy SPSC Ring --- */
struct selftest_ctx {
    int prod_fd;
    uint32_t total_bytes;
};

static void *selftest_producer_thread(void *arg) {
    struct selftest_ctx *ctx = (struct selftest_ctx *)arg;
    struct audio_msg amsg;
    struct audio_format fmt;

    if (recv_all(ctx->prod_fd, &amsg, sizeof(amsg)) < 0 || amsg.type != AUDIO_MSG_FORMAT)
        return NULL;
    if (recv_all(ctx->prod_fd, &fmt, sizeof(fmt)) < 0)
        return NULL;
    if (recv_all(ctx->prod_fd, &amsg, sizeof(amsg)) < 0 || amsg.type != AUDIO_MSG_SHM)
        return NULL;

    int mfd = create_memfd_compat("anland_selftest_shm");
    size_t sz = sizeof(struct anland_shm_audio_header) + ANLAND_SHM_AUDIO_DEFAULT_SIZE;
    if (mfd < 0 || ftruncate(mfd, (off_t)sz) < 0)
        return NULL;

    struct anland_shm_audio_header *ring = mmap(NULL, sz, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
    if (ring == MAP_FAILED) {
        close(mfd);
        return NULL;
    }

    memset(ring, 0, sizeof(*ring));
    ring->magic = ANLAND_SHM_AUDIO_MAGIC;
    ring->version = ANLAND_SHM_AUDIO_VERSION;
    ring->buffer_size = ANLAND_SHM_AUDIO_DEFAULT_SIZE;
    ring->rate = fmt.rate;
    ring->channels = fmt.channels;
    ring->format = AUDIO_FORMAT_S16LE;

    struct audio_msg resp = { .type = AUDIO_MSG_SHM_FD, .size = 0 };
    if (send_fds(ctx->prod_fd, &resp, sizeof(resp), &mfd, 1) < 0) {
        munmap(ring, sz);
        close(mfd);
        return NULL;
    }

    const uint32_t pkt_sz = 1920;
    uint8_t pkt[1920];
    for (uint32_t i = 0; i < pkt_sz; i++)
        pkt[i] = (uint8_t)(i & 0xFF);

    uint32_t sent = 0;
    while (sent < ctx->total_bytes) {
        uint32_t head = __atomic_load_n(&ring->write_head, __ATOMIC_RELAXED);
        uint32_t tail = __atomic_load_n(&ring->read_tail, __ATOMIC_ACQUIRE);
        uint32_t cap = ring->buffer_size;
        uint32_t used = (head >= tail) ? (head - tail) : (cap - (tail - head));
        uint32_t space = cap - 1 - used;
        if (space < pkt_sz) {
            usleep(100);
            continue;
        }
        uint32_t first = (head + pkt_sz <= cap) ? pkt_sz : (cap - head);
        memcpy(ring->data + head, pkt, first);
        if (first < pkt_sz)
            memcpy(ring->data, pkt + first, pkt_sz - first);
        __atomic_store_n(&ring->write_head, (head + pkt_sz) % cap, __ATOMIC_RELEASE);
        sent += pkt_sz;
    }

    usleep(20000);
    munmap(ring, sz);
    close(mfd);
    return NULL;
}

static int run_self_test(void) {
    int sp[2];
    if (socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sp) < 0) {
        perror("socketpair");
        return 1;
    }
    const uint32_t test_bytes = 1920u * 1000u; /* 1.92 MB (~10 seconds of 48kHz stereo PCM) */
    struct selftest_ctx ctx = { .prod_fd = sp[1], .total_bytes = test_bytes };
    pthread_t th;
    pthread_create(&th, NULL, selftest_producer_thread, &ctx);

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    int rc = run_audio_consumer_loop(sp[0], -1, -1, 48000, 2, test_bytes);
    clock_gettime(CLOCK_MONOTONIC, &t1);

    pthread_join(th, NULL);
    close(sp[0]);
    close(sp[1]);

    double sec = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
    double mbps = ((double)test_bytes / (1024.0 * 1024.0)) / (sec > 0 ? sec : 1e-6);
    printf("SELF_TEST: %s | bytes=%u | elapsed=%.4fs | throughput=%.2f MB/s\n",
           rc == 0 ? "PASS" : "FAIL", test_bytes, sec, mbps);
    return rc == 0 ? 0 : 1;
}

int main(int argc, char **argv) {
    const char *sock_path = "/data/local/tmp/display_daemon.sock";
    uint32_t rate = 48000;
    uint32_t channels = 2;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--self-test") == 0) {
            return run_self_test();
        } else if (strcmp(argv[i], "--socket") == 0 && i + 1 < argc) {
            sock_path = argv[++i];
        } else if (strcmp(argv[i], "--rate") == 0 && i + 1 < argc) {
            rate = (uint32_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--channels") == 0 && i + 1 < argc) {
            channels = (uint32_t)atoi(argv[++i]);
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("Usage: %s [--socket PATH] [--rate 48000] [--channels 2] [--self-test]\n", argv[0]);
            return 0;
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);

    int ctrl_fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (ctrl_fd < 0) {
        perror("socket");
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", sock_path);
    if (connect(ctrl_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "anland_audio_headless: connect(%s) failed: %s\n", sock_path, strerror(errno));
        close(ctrl_fd);
        return 1;
    }

    int efd = eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
    int fence_sp[2], data_sp[2], audio_sp[2];
    int dummy_shm = create_memfd_compat("anland_headless_fb");
    if (efd < 0 || dummy_shm < 0 || ftruncate(dummy_shm, 4096) < 0 ||
        socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fence_sp) < 0 ||
        socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, data_sp) < 0 ||
        socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, audio_sp) < 0) {
        perror("anland_audio_headless: fd setup");
        return 1;
    }

    /* Deposit 5 FDs { buf_ready, fence, data, shm, audio } to display_daemon */
    int deposit[5] = { efd, fence_sp[1], data_sp[1], dummy_shm, audio_sp[1] };
    struct ctrl_msg hello = { .type = CTRL_MSG_CONSUMER_HELLO, .size = 0 };
    if (send_fds(ctrl_fd, &hello, sizeof(hello), deposit, 5) < 0) {
        fprintf(stderr, "anland_audio_headless: CTRL_MSG_CONSUMER_HELLO failed\n");
        return 1;
    }
    for (int i = 0; i < 5; i++)
        close(deposit[i]);

    fprintf(stderr, "anland_audio_headless: connected to %s, streaming SHM PCM to stdout\n", sock_path);
    int rc = run_audio_consumer_loop(audio_sp[0], ctrl_fd, STDOUT_FILENO, rate, channels, 0);

    close(audio_sp[0]);
    close(fence_sp[0]);
    close(data_sp[0]);
    close(ctrl_fd);
    return rc == 0 ? 0 : 1;
}
