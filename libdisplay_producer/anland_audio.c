#define _GNU_SOURCE
#include "anland_audio.h"
#include "protocol.h"

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/pod/builder.h>
#include <spa/utils/hook.h>

/* Default formats until the consumer negotiates the real device formats (protocol.h):
 * playback stereo L,R, mic mono, both S16LE. */
#define DEFAULT_RATE          48000
#define DEFAULT_PLAY_CHANNELS 2
#define DEFAULT_CAP_CHANNELS  1
/* ~1s of stereo S16 mic audio; bounds added latency, old samples drop on overflow.
 * Sized for the worst case (stereo) so the ring never under-allocates. */
#define MIC_RING_BYTES        (48000 * 2 * (int)sizeof(int16_t))
#define MAX_DGRAM             (64 * 1024)
/* Retry cadence after the PipeWire connection is lost (sound service restart, etc). */
#define RECONNECT_SECS    1

struct anland_audio {
    struct pw_thread_loop *loop;
    struct pw_context     *context;
    struct pw_core        *core;
    struct spa_hook        core_listener;
    struct spa_source     *reconnect_timer;
    bool                   pw_connected;   /* core + streams are up */

    struct pw_stream      *capture;   /* virtual Audio/Sink  -> socket (playback) */
    struct spa_hook        capture_listener;
    struct pw_stream      *source;    /* virtual Audio/Source <- socket (capture) */
    struct spa_hook        source_listener;

    /* Negotiated formats (the consumer owns the hardware and dictates these via
     * AUDIO_MSG_FORMAT). Defaults stand until the consumer's formats arrive. */
    uint32_t               play_rate, play_channels;   /* the Audio/Sink format   */
    uint32_t               cap_rate, cap_channels;      /* the Audio/Source format */
    /* Requested buffer (frames) per stream from the consumer's latency preset;
     * 0 = let PipeWire choose the graph quantum. Applied as node.latency. */
    uint32_t               play_quantum, cap_quantum;

    int                    audio_fd;  /* owned duplicate; -1 when detached */
    struct spa_source     *io;        /* loop io source watching audio_fd for reads */

    /* Mic ring buffer. Only ever touched from the loop thread (the io read callback
     * fills it, the source process callback drains it), so it needs no lock. */
    uint8_t               *ring;
    size_t                 ring_size, ring_head, ring_tail, ring_fill;

    uint8_t                rx[MAX_DGRAM];
};

static struct anland_audio *g_audio = NULL;

static int connect_stream(struct pw_stream *stream, enum spa_direction direction,
                          uint32_t rate, uint32_t channels, uint32_t quantum);
static const struct spa_pod *build_format(struct spa_pod_builder *bld,
                                          uint32_t rate, uint32_t channels);
static void set_latency(struct pw_stream *stream, uint32_t quantum, uint32_t rate);

/* ---- mic ring buffer (single-threaded: loop thread only) ---- */

static void ring_reset(struct anland_audio *a)
{
    a->ring_head = a->ring_tail = a->ring_fill = 0;
}

static void ring_write(struct anland_audio *a, const uint8_t *p, size_t n)
{
    if (n > a->ring_size) {           /* keep only the newest ring_size bytes */
        p += n - a->ring_size;
        n = a->ring_size;
    }
    if (a->ring_fill + n > a->ring_size) {   /* drop oldest to make room */
        size_t drop = a->ring_fill + n - a->ring_size;
        a->ring_tail = (a->ring_tail + drop) % a->ring_size;
        a->ring_fill -= drop;
    }
    size_t first = a->ring_size - a->ring_head;
    if (first > n)
        first = n;
    memcpy(a->ring + a->ring_head, p, first);
    memcpy(a->ring, p + first, n - first);
    a->ring_head = (a->ring_head + n) % a->ring_size;
    a->ring_fill += n;
}

static size_t ring_read(struct anland_audio *a, uint8_t *p, size_t n)
{
    size_t got = n < a->ring_fill ? n : a->ring_fill;
    size_t first = a->ring_size - a->ring_tail;
    if (first > got)
        first = got;
    memcpy(p, a->ring + a->ring_tail, first);
    memcpy(p + first, a->ring, got - first);
    a->ring_tail = (a->ring_tail + got) % a->ring_size;
    a->ring_fill -= got;
    return got;
}

/* The caller holds the thread-loop lock, or is executing on that loop. The io
 * source owns audio_fd, so destroying it also closes the duplicated descriptor. */
static void detach_audio_fd_locked(struct anland_audio *a)
{
    struct spa_source *io = a->io;
    a->io = NULL;
    a->audio_fd = -1;
    ring_reset(a);
    if (io)
        pw_loop_destroy_source(pw_thread_loop_get_loop(a->loop), io);
}

/* ---- stream process callbacks (run on the PipeWire thread loop) ---- */

/* Desktop audio captured from the default sink's monitor -> push to the socket so
 * the consumer plays it. Dropped (still drained) while detached. */
static void on_capture_process(void *data)
{
    struct anland_audio *a = data;
    struct pw_buffer *b = pw_stream_dequeue_buffer(a->capture);
    if (!b)
        return;

    struct spa_data *d = &b->buffer->datas[0];
    if (d->data && d->chunk->size > 0 && a->audio_fd >= 0) {
        struct audio_msg h = { .type = AUDIO_MSG_PCM, .size = d->chunk->size };
        struct iovec iov[2] = {
            { .iov_base = &h, .iov_len = sizeof(h) },
            { .iov_base = (uint8_t *)d->data + d->chunk->offset, .iov_len = d->chunk->size },
        };
        struct msghdr m = { .msg_iov = iov, .msg_iovlen = 2 };
        /* Non-blocking: the loop thread must never stall on a slow/dead consumer.
         * One SEQPACKET datagram per period; drop on EAGAIN. */
        sendmsg(a->audio_fd, &m, MSG_DONTWAIT | MSG_NOSIGNAL);
    }
    pw_stream_queue_buffer(a->capture, b);
}

/* Fill the virtual mic source from the ring buffer (fed by Android). Silence-pads
 * when the ring underruns or while detached, so the source never glitches/vanishes. */
static void on_source_process(void *data)
{
    struct anland_audio *a = data;
    struct pw_buffer *b = pw_stream_dequeue_buffer(a->source);
    if (!b)
        return;

    struct spa_data *d = &b->buffer->datas[0];
    const uint32_t stride = sizeof(int16_t) * a->cap_channels;
    uint32_t frames = d->maxsize / stride;
    if (b->requested && b->requested < frames)
        frames = b->requested;
    uint32_t bytes = frames * stride;

    size_t got = ring_read(a, d->data, bytes);
    if (got < bytes)
        memset((uint8_t *)d->data + got, 0, bytes - got);

    d->chunk->offset = 0;
    d->chunk->stride = stride;
    d->chunk->size = bytes;
    pw_stream_queue_buffer(a->source, b);
}

static const struct pw_stream_events capture_events = {
    PW_VERSION_STREAM_EVENTS,
    .process = on_capture_process,
};

static const struct pw_stream_events source_events = {
    PW_VERSION_STREAM_EVENTS,
    .process = on_source_process,
};

/* Apply a consumer-announced format. The virtual device stays continuously online
 * and is hot-plugged (disconnect + reconnect) ONLY when the format actually changes
 * versus what the stream is currently running -- an unchanged announcement (the common
 * case, including every consumer reconnect that re-sends the same format) is a no-op,
 * so the node never churns and plasma-pa keeps resolving the default sink/source.
 *
 * Defaults are role-correct: a field left 0 means "device default", which must equal
 * the value the stream was built with, otherwise the comparison below would treat an
 * unset field as a change and re-plug on every announcement. (One bug was a CAPTURE
 * announce with channels==0 defaulting to 2, never matching the mono source.)
 *
 * Every change is applied IN PLACE -- the node object (anland-speaker / anland-mic) is
 * never destroyed, so WirePlumber/plasma-pa keep their default reference and never log
 * "No object for name anland-speaker". A rate/channels change renegotiates the port
 * format via pw_stream_update_params(); a quantum-only change (AAudio's framesPerBurst
 * legitimately varies between opens) just updates node.latency. Neither disconnects.
 *
 * Runs on the loop thread, so the pw_stream calls are safe. */
static void apply_format(struct anland_audio *a, const struct audio_format *f)
{
    const bool playback = (f->role == AUDIO_ROLE_PLAYBACK);
    const uint32_t rate = f->rate ? f->rate : DEFAULT_RATE;
    const uint32_t channels = f->channels ? f->channels
                                          : (playback ? DEFAULT_PLAY_CHANNELS : DEFAULT_CAP_CHANNELS);
    /* Validate before changing live stream state or doing PCM stride math. */
    if ((f->role != AUDIO_ROLE_PLAYBACK && f->role != AUDIO_ROLE_CAPTURE) ||
        f->format != AUDIO_FORMAT_S16LE || rate < 8000 || rate > 384000 ||
        channels > 2 || f->quantum > rate)
        return;

    uint32_t *cur_rate     = playback ? &a->play_rate : &a->cap_rate;
    uint32_t *cur_channels = playback ? &a->play_channels : &a->cap_channels;
    uint32_t *cur_quantum  = playback ? &a->play_quantum : &a->cap_quantum;
    struct pw_stream *stream = playback ? a->capture : a->source;

    const bool format_changed = (rate != *cur_rate || channels != *cur_channels);
    const bool quantum_changed = (f->quantum != *cur_quantum);
    if (!format_changed && !quantum_changed)
        return;   /* unchanged -> keep the device online, no hot-plug */

    *cur_rate = rate;
    *cur_channels = channels;
    *cur_quantum = f->quantum;

    if (!a->pw_connected || !stream)
        return;   /* build_pw() will pick up the new values when it (re)creates the stream */

    if (format_changed) {
        /* Renegotiate the port format on the LIVE stream. update_params keeps the node
         * object alive (same id) and re-runs format negotiation, so the default sink/
         * source reference is never lost -- no churn, no plasma-pa "No object" spam. */
        set_latency(stream, f->quantum, rate);
        uint8_t buffer[1024];
        struct spa_pod_builder bld = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
        const struct spa_pod *params[1] = { build_format(&bld, rate, channels) };
        pw_stream_update_params(stream, params, 1);
    } else if (f->quantum > 0) {
        /* Latency-only tweak: node.latency in place, nothing renegotiates. */
        set_latency(stream, f->quantum, rate);
    }
}

/* Mic PCM / format announcements arriving from the consumer. Runs on the loop thread. */
static void on_audio_readable(void *data, int fd, uint32_t mask)
{
    struct anland_audio *a = data;
    if (fd != a->audio_fd)
        return;
    if (mask & (SPA_IO_ERR | SPA_IO_HUP)) {
        detach_audio_fd_locked(a);
        return;
    }
    if (!(mask & SPA_IO_IN))
        return;

    for (;;) {
        ssize_t n = recv(fd, a->rx, sizeof(a->rx), MSG_DONTWAIT);
        if (n == 0) {
            detach_audio_fd_locked(a);
            break;
        }
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                detach_audio_fd_locked(a);
            break;
        }
        if ((size_t)n < sizeof(struct audio_msg))
            continue;
        struct audio_msg h;
        memcpy(&h, a->rx, sizeof(h));
        size_t avail = (size_t)n - sizeof(struct audio_msg);

        if (h.type == AUDIO_MSG_FORMAT) {
            if (h.size == sizeof(struct audio_format) && avail == sizeof(struct audio_format)) {
                struct audio_format f;
                memcpy(&f, a->rx + sizeof(struct audio_msg), sizeof(f));
                apply_format(a, &f);
            }
            continue;
        }
        if (h.type != AUDIO_MSG_PCM)
            continue;
        size_t size = h.size < avail ? h.size : avail;
        ring_write(a, a->rx + sizeof(struct audio_msg), size);
    }
}

/* ---- PipeWire connection lifecycle (build / teardown / auto-reconnect) ---- */

static void arm_reconnect(struct anland_audio *a)
{
    struct timespec val = { .tv_sec = RECONNECT_SECS, .tv_nsec = 0 };
    pw_loop_update_timer(pw_thread_loop_get_loop(a->loop), a->reconnect_timer,
                         &val, NULL, false);
}

/* Fatal, non-recoverable error on the core proxy means the sound service connection
 * was lost (e.g. pipewire/wireplumber restarted). Drop the dead core+streams and
 * schedule a rebuild; the audio socket / mic ring are untouched so the consumer side
 * keeps working and resumes the moment PipeWire is back. */
static void on_core_error(void *data, uint32_t id, int seq, int res, const char *message)
{
    struct anland_audio *a = data;
    (void)seq;
    (void)message;
    if (id == PW_ID_CORE && res == -EPIPE) {
        a->pw_connected = false;
        arm_reconnect(a);   /* teardown + rebuild happens in the timer, not here */
    }
}

static const struct pw_core_events core_events = {
    PW_VERSION_CORE_EVENTS,
    .error = on_core_error,
};

/* Build an S16LE EnumFormat POD for rate/channels into the caller's builder. Stereo is
 * L,R (FL,FR); anything else collapses to a plain mono channel. */
static const struct spa_pod *build_format(struct spa_pod_builder *bld,
                                          uint32_t rate, uint32_t channels)
{
    struct spa_audio_info_raw info = {
        .format = SPA_AUDIO_FORMAT_S16_LE,
        .rate = rate,
        .channels = channels,
    };
    if (channels >= 2) {
        info.position[0] = SPA_AUDIO_CHANNEL_FL;
        info.position[1] = SPA_AUDIO_CHANNEL_FR;
    } else {
        info.position[0] = SPA_AUDIO_CHANNEL_MONO;
    }
    return spa_format_audio_raw_build(bld, SPA_PARAM_EnumFormat, &info);
}

/* node.latency = "quantum/rate" asks PipeWire to run this node at that buffer size.
 * quantum == 0 leaves the graph default. Applied in place -- never re-plugs the node. */
static void set_latency(struct pw_stream *stream, uint32_t quantum, uint32_t rate)
{
    if (quantum == 0)
        return;
    char latency[32];
    snprintf(latency, sizeof(latency), "%u/%u", quantum, rate);
    struct spa_dict_item items[] = {
        SPA_DICT_ITEM_INIT(PW_KEY_NODE_LATENCY, latency),
    };
    struct spa_dict dict = SPA_DICT_INIT(items, 1);
    pw_stream_update_properties(stream, &dict);
}

static int connect_stream(struct pw_stream *stream, enum spa_direction direction,
                          uint32_t rate, uint32_t channels, uint32_t quantum)
{
    set_latency(stream, quantum, rate);

    uint8_t buffer[1024];
    struct spa_pod_builder bld = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
    const struct spa_pod *params[1] = { build_format(&bld, rate, channels) };

    return pw_stream_connect(stream, direction, PW_ID_ANY,
                             PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS,
                             params, 1);
}

/* Tear down the core proxy and both streams, leaving the loop, context, timer, mic
 * ring and audio-socket io source intact. Idempotent. */
static void teardown_pw(struct anland_audio *a)
{
    if (a->capture) {
        spa_hook_remove(&a->capture_listener);
        pw_stream_destroy(a->capture);
        a->capture = NULL;
    }
    if (a->source) {
        spa_hook_remove(&a->source_listener);
        pw_stream_destroy(a->source);
        a->source = NULL;
    }
    if (a->core) {
        spa_hook_remove(&a->core_listener);
        pw_core_disconnect(a->core);
        a->core = NULL;
    }
}

/* (Re)connect the core and (re)create both streams. Returns 0 on success. */
static int build_pw(struct anland_audio *a)
{
    a->core = pw_context_connect(a->context, NULL, 0);
    if (!a->core)
        return -1;
    pw_core_add_listener(a->core, &a->core_listener, &core_events, a);

    /* Own a virtual sink so the container has a real output device instead of only
     * the auto-null "Dummy Output": apps play into this Audio/Sink, WirePlumber makes
     * it the default (high priority beats auto_null), and on_capture_process receives
     * the mixed PCM directly -- no monitor capture, nothing bound to the dummy. */
    a->capture = pw_stream_new(a->core, "anland-speaker",
        pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Audio",
            PW_KEY_MEDIA_CLASS, "Audio/Sink",
            PW_KEY_NODE_NAME, "anland-speaker",
            PW_KEY_NODE_DESCRIPTION, "Anland remote speaker",
            PW_KEY_PRIORITY_SESSION, "1010",   /* outrank the auto-null dummy sink */
            PW_KEY_PRIORITY_DRIVER, "1010",
            NULL));
    if (!a->capture)
        return -1;
    pw_stream_add_listener(a->capture, &a->capture_listener, &capture_events, a);

    /* Expose the Android mic to Linux apps as a recordable source. */
    a->source = pw_stream_new(a->core, "anland-mic",
        pw_properties_new(
            PW_KEY_MEDIA_TYPE, "Audio",
            PW_KEY_MEDIA_CLASS, "Audio/Source",
            PW_KEY_NODE_NAME, "anland-mic",
            PW_KEY_NODE_DESCRIPTION, "Anland remote microphone",
            PW_KEY_PRIORITY_SESSION, "1010",   /* outrank the auto-null dummy source */
            PW_KEY_PRIORITY_DRIVER, "1010",
            NULL));
    if (!a->source)
        return -1;
    pw_stream_add_listener(a->source, &a->source_listener, &source_events, a);

    if (connect_stream(a->capture, PW_DIRECTION_INPUT, a->play_rate, a->play_channels,
                       a->play_quantum) < 0)
        return -1;
    if (connect_stream(a->source, PW_DIRECTION_OUTPUT, a->cap_rate, a->cap_channels,
                       a->cap_quantum) < 0)
        return -1;

    return 0;
}

static void on_reconnect_timer(void *data, uint64_t expirations)
{
    struct anland_audio *a = data;
    (void)expirations;
    if (a->pw_connected)
        return;

    teardown_pw(a);            /* clear any half-built state from a failed attempt */
    if (build_pw(a) == 0) {
        a->pw_connected = true;
    } else {
        teardown_pw(a);
        arm_reconnect(a);      /* sound service still down -- keep retrying */
    }
}

/* ---- public API ---- */

void anland_audio_set_fd(int audio_fd)
{
    struct anland_audio *a = g_audio;
    if (!a)
        return;

    pw_thread_loop_lock(a->loop);

    detach_audio_fd_locked(a);

    if (audio_fd >= 0) {
        /* display_producer retains the input fd. Keep an owned duplicate so source
         * teardown cannot race with producer fd reuse during fallback. The consumer
         * announces both device formats (AUDIO_MSG_FORMAT) right after
         * this socket comes up; on_audio_readable applies them and reconfigures the
         * PipeWire streams, so we don't dictate any format here. */
        int owned_fd = fcntl(audio_fd, F_DUPFD_CLOEXEC, 3);
        if (owned_fd >= 0) {
            a->io = pw_loop_add_io(pw_thread_loop_get_loop(a->loop), owned_fd,
                                   SPA_IO_IN, true, on_audio_readable, a);
            if (a->io) {
                a->audio_fd = owned_fd;
            } else {
                close(owned_fd);
            }
        }
    }

    pw_thread_loop_unlock(a->loop);
}

int anland_audio_start(void)
{
    if (g_audio)
        return 0;

    pw_init(NULL, NULL);

    struct anland_audio *a = calloc(1, sizeof(*a));
    if (!a)
        return -1;
    a->audio_fd = -1;
    a->play_rate = DEFAULT_RATE;
    a->play_channels = DEFAULT_PLAY_CHANNELS;
    a->cap_rate = DEFAULT_RATE;
    a->cap_channels = DEFAULT_CAP_CHANNELS;
    a->ring_size = MIC_RING_BYTES;
    a->ring = malloc(a->ring_size);
    if (!a->ring)
        goto fail;

    a->loop = pw_thread_loop_new("anland-audio", NULL);
    if (!a->loop)
        goto fail;

    a->context = pw_context_new(pw_thread_loop_get_loop(a->loop), NULL, 0);
    if (!a->context)
        goto fail;

    a->reconnect_timer = pw_loop_add_timer(pw_thread_loop_get_loop(a->loop),
                                           on_reconnect_timer, a);
    if (!a->reconnect_timer)
        goto fail;

    if (pw_thread_loop_start(a->loop) < 0)
        goto fail;

    /* First connection attempt under the loop lock. If PipeWire is not up yet, fall
     * back to the reconnect timer instead of failing -- audio will come up when the
     * sound service appears. Either way the engine object exists. */
    pw_thread_loop_lock(a->loop);
    if (build_pw(a) == 0) {
        a->pw_connected = true;
    } else {
        teardown_pw(a);
        arm_reconnect(a);
    }
    pw_thread_loop_unlock(a->loop);

    g_audio = a;
    return 0;

fail:
    if (a->reconnect_timer)
        pw_loop_destroy_source(pw_thread_loop_get_loop(a->loop), a->reconnect_timer);
    if (a->context)
        pw_context_destroy(a->context);
    if (a->loop)
        pw_thread_loop_destroy(a->loop);
    free(a->ring);
    free(a);
    pw_deinit();
    return -1;
}

void anland_audio_stop(void)
{
    struct anland_audio *a = g_audio;
    if (!a)
        return;
    g_audio = NULL;

    if (a->loop)
        pw_thread_loop_stop(a->loop);
    teardown_pw(a);
    if (a->io)
        detach_audio_fd_locked(a);
    if (a->reconnect_timer)
        pw_loop_destroy_source(pw_thread_loop_get_loop(a->loop), a->reconnect_timer);
    if (a->context)
        pw_context_destroy(a->context);
    if (a->loop)
        pw_thread_loop_destroy(a->loop);
    free(a->ring);
    free(a);
    pw_deinit();
}