#define _GNU_SOURCE
#include "native_audio.h"
#include "protocol.h"
#include "tracy_zones.h"

#include <aaudio/AAudio.h>
#include <android/log.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>

#define TAG "AnlandAudio"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)

/* Channel-count preferences; the device may override (we read back the actuals).
 * Sample rate is never pinned -- AAudio picks the device-optimal rate and we honour
 * it, telling the producer so PipeWire matches. See protocol.h. */
#define WANT_PLAY_CHANNELS 2
#define WANT_CAP_CHANNELS  1
#define MAX_DGRAM          (64 * 1024)
#define MIC_MAX_FRAMES     1024   /* upper bound on frames per mic read */
#define PLAY_POLL_MS       100
#define PLAY_RING_MS       160
#define PLAY_RING_MIN      (32 * 1024)
#define PLAY_RING_MAX      (256 * 1024)
#define PLAY_KEEPALIVE_AMPLITUDE 1
#define PLAY_WAKE_FADE_MS  8
/* Keep the output stream running while the desktop is producing audio (short
 * UI sounds restart it instantly), but stop it once silence has lasted this
 * long so the audio path can idle instead of holding the codec/HAL on forever
 * (idle power). A fresh PCM datagram restarts it via ensure_play_stream_started(). */
#define PLAY_IDLE_STOP_MS  1500

struct audio_bridge {
    volatile bool running;
    volatile bool mic_enabled;

    /* The live connection. Set by audio_set_ctx() from the render thread, read by
     * the audio threads -- same lightweight convention as the event thread's
     * s->ctx. get_audio_fd() returns -1 in fallback, so a stale-but-valid ctx just
     * yields no fd rather than misbehaving. */
    display_ctx *volatile ctx;

    pthread_t play_thread;
    pthread_t cap_thread;

    AAudioStream *play;   /* output: desktop -> speaker */
    AAudioStream *rec;    /* input:  mic -> producer    */

    /* Actual device-chosen formats, read back after the streams open. */
    int play_rate, play_channels;
    int cap_rate, cap_channels;

    /* Latency presets in ms (0 = engine default), set from the settings UI. */
    volatile int play_latency_ms;
    volatile int cap_latency_ms;
    volatile bool resend_formats;   /* a preset/device change -> re-announce */

    /* User "audio keep-alive" toggle. On: keep the output stream running (fed
     * near-silent keepalive) for maximum burst reliability, at a little standby
     * power. Off (default): the play thread idle-stops the stream after silence
     * so the audio path can sleep. */
    volatile bool keepalive_enabled;

    /*
     * Playback is pull-driven by an AAudio data callback. The socket thread only
     * appends incoming PCM to this ring; the callback feeds an inaudible keepalive
     * on underrun.
     * That keeps Android's audio mixer awake across short Linux sound effects
     * without reconnecting the display pipeline.
     */
    pthread_mutex_t play_lock;
    uint8_t *play_ring;
    size_t play_ring_size;
    size_t play_ring_head;
    size_t play_ring_tail;
    size_t play_ring_fill;
    volatile bool play_error;
    bool play_idle;
    int play_keepalive_phase;
    int play_wake_fade_frames;

    uint8_t rx[MAX_DGRAM];
};

static int current_fd(struct audio_bridge *b)
{
    display_ctx *ctx = b->ctx;
    return ctx ? get_audio_fd(ctx) : -1;
}

static uint64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

/* ---- playback ring helpers ---- */

static size_t align_down(size_t n, size_t align)
{
    return align > 0 ? n - (n % align) : n;
}

static size_t align_up(size_t n, size_t align)
{
    if (align == 0)
        return n;
    size_t rem = n % align;
    return rem == 0 ? n : n + align - rem;
}

static size_t play_frame_bytes(struct audio_bridge *b)
{
    int channels = b && b->play_channels > 0 ? b->play_channels : WANT_PLAY_CHANNELS;
    return (size_t)channels * sizeof(int16_t);
}

static size_t playback_ring_bytes(int rate, int channels)
{
    if (rate <= 0)
        rate = 48000;
    if (channels <= 0)
        channels = WANT_PLAY_CHANNELS;

    size_t frame_bytes = (size_t)channels * sizeof(int16_t);
    size_t bytes = (size_t)rate * (size_t)channels * sizeof(int16_t) * PLAY_RING_MS / 1000;
    if (bytes < PLAY_RING_MIN)
        bytes = PLAY_RING_MIN;
    if (bytes > PLAY_RING_MAX)
        bytes = PLAY_RING_MAX;
    bytes = align_down(bytes, frame_bytes);
    if (bytes < frame_bytes)
        bytes = frame_bytes;
    return bytes;
}

static void play_ring_reset_locked(struct audio_bridge *b)
{
    b->play_ring_head = 0;
    b->play_ring_tail = 0;
    b->play_ring_fill = 0;
}

static bool play_ring_resize_locked(struct audio_bridge *b, size_t size, bool reset)
{
    if (b->play_ring && b->play_ring_size == size) {
        if (reset)
            play_ring_reset_locked(b);
        return true;
    }

    uint8_t *ring = malloc(size);
    if (!ring)
        return false;

    free(b->play_ring);
    b->play_ring = ring;
    b->play_ring_size = size;
    play_ring_reset_locked(b);
    return true;
}

static size_t play_ring_read_locked(struct audio_bridge *b, uint8_t *dst, size_t n)
{
    if (!b->play_ring || b->play_ring_fill == 0)
        return 0;

    n = align_down(n, play_frame_bytes(b));
    size_t got = n < b->play_ring_fill ? n : b->play_ring_fill;
    got = align_down(got, play_frame_bytes(b));
    if (got == 0)
        return 0;

    size_t first = b->play_ring_size - b->play_ring_tail;
    if (first > got)
        first = got;

    memcpy(dst, b->play_ring + b->play_ring_tail, first);
    memcpy(dst + first, b->play_ring, got - first);
    b->play_ring_tail = (b->play_ring_tail + got) % b->play_ring_size;
    b->play_ring_fill -= got;
    return got;
}

static void play_ring_write_locked(struct audio_bridge *b, const uint8_t *src, size_t n)
{
    if (!b->play_ring || b->play_ring_size == 0 || n == 0)
        return;

    size_t frame_bytes = play_frame_bytes(b);
    n = align_down(n, frame_bytes);
    if (n == 0)
        return;

    if (n > b->play_ring_size) {
        src += n - b->play_ring_size;
        n = b->play_ring_size;
    }

    if (b->play_ring_fill + n > b->play_ring_size) {
        size_t drop = b->play_ring_fill + n - b->play_ring_size;
        drop = align_up(drop, frame_bytes);
        if (drop > b->play_ring_fill)
            drop = b->play_ring_fill;
        b->play_ring_tail = (b->play_ring_tail + drop) % b->play_ring_size;
        b->play_ring_fill -= drop;
    }

    size_t first = b->play_ring_size - b->play_ring_head;
    if (first > n)
        first = n;

    memcpy(b->play_ring + b->play_ring_head, src, first);
    memcpy(b->play_ring, src + first, n - first);
    b->play_ring_head = (b->play_ring_head + n) % b->play_ring_size;
    b->play_ring_fill += n;
}

static void queue_playback_bytes(struct audio_bridge *b, const uint8_t *data, size_t bytes)
{
    if (!b || !data || bytes == 0)
        return;

    pthread_mutex_lock(&b->play_lock);
    play_ring_write_locked(b, data, bytes);
    pthread_mutex_unlock(&b->play_lock);
}

static int play_wake_fade_frame_count(struct audio_bridge *b)
{
    int rate = b && b->play_rate > 0 ? b->play_rate : 48000;
    int frames = rate * PLAY_WAKE_FADE_MS / 1000;
    return frames > 0 ? frames : 1;
}

static void fill_keepalive_frames(struct audio_bridge *b, int16_t *dst,
                                  int32_t frames, int channels)
{
    if (frames <= 0 || channels <= 0)
        return;

    for (int32_t f = 0; f < frames; ++f) {
        int16_t v = b->play_keepalive_phase ? PLAY_KEEPALIVE_AMPLITUDE
                                            : -PLAY_KEEPALIVE_AMPLITUDE;
        b->play_keepalive_phase ^= 1;
        for (int ch = 0; ch < channels; ++ch)
            *dst++ = v;
    }
}

static void apply_wake_fade(struct audio_bridge *b, int16_t *samples,
                            int32_t frames, int channels)
{
    if (frames <= 0 || channels <= 0 || b->play_wake_fade_frames <= 0)
        return;

    int total = play_wake_fade_frame_count(b);
    for (int32_t f = 0; f < frames && b->play_wake_fade_frames > 0; ++f) {
        int gain = total - b->play_wake_fade_frames + 1;
        for (int ch = 0; ch < channels; ++ch) {
            int32_t s = samples[f * channels + ch];
            samples[f * channels + ch] = (int16_t)((s * gain) / total);
        }
        b->play_wake_fade_frames--;
    }
}

static aaudio_data_callback_result_t play_data_cb(
    AAudioStream *stream, void *userdata, void *audio_data, int32_t num_frames)
{
    (void)stream;
    TracyCZoneN(zCb, "audio play callback", 1);
    struct audio_bridge *b = userdata;
    uint8_t *dst = audio_data;
    int16_t *samples = audio_data;
    int channels = b->play_channels > 0 ? b->play_channels : WANT_PLAY_CHANNELS;
    size_t need = (size_t)num_frames * (size_t)channels * sizeof(int16_t);
    size_t got = 0;
    int32_t got_frames = 0;

    if (pthread_mutex_trylock(&b->play_lock) == 0) {
        got = play_ring_read_locked(b, dst, need);
        pthread_mutex_unlock(&b->play_lock);
    }

    got_frames = (int32_t)(got / ((size_t)channels * sizeof(int16_t)));
    if (got_frames > 0) {
        if (b->play_idle)
            b->play_wake_fade_frames = play_wake_fade_frame_count(b);
        b->play_idle = false;
        apply_wake_fade(b, samples, got_frames, channels);
    }

    if (got < need) {
        int32_t keepalive_frames = num_frames - got_frames;
        fill_keepalive_frames(b, (int16_t *)(dst + got), keepalive_frames, channels);
        b->play_idle = true;
    }
    TracyCPlot("audio underrun frames", (float)(num_frames - got_frames));
    TracyCZoneEnd(zCb);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

static void play_error_cb(AAudioStream *stream, void *userdata, aaudio_result_t error)
{
    (void)stream;
    struct audio_bridge *b = userdata;
    if (b) {
        b->play_error = true;
        LOGE("output stream error: %s", AAudio_convertResultToText(error));
    }
}

/* ---- AAudio stream helpers ---- */

static AAudioStream *open_stream(struct audio_bridge *bridge,
                                 aaudio_direction_t dir, int channels)
{
    AAudioStreamBuilder *bld = NULL;
    if (AAudio_createStreamBuilder(&bld) != AAUDIO_OK || !bld)
        return NULL;

    AAudioStreamBuilder_setDirection(bld, dir);
    /* UNSPECIFIED rate: let the device pick its optimal/native rate; we read it back. */
    AAudioStreamBuilder_setSampleRate(bld, AAUDIO_UNSPECIFIED);
    AAudioStreamBuilder_setChannelCount(bld, channels);
    AAudioStreamBuilder_setFormat(bld, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setSharingMode(bld, AAUDIO_SHARING_MODE_SHARED);

    if (dir == AAUDIO_DIRECTION_OUTPUT) {
        AAudioStreamBuilder_setUsage(bld, AAUDIO_USAGE_MEDIA);
        AAudioStreamBuilder_setContentType(bld, AAUDIO_CONTENT_TYPE_MUSIC);
        AAudioStreamBuilder_setPerformanceMode(bld, AAUDIO_PERFORMANCE_MODE_NONE);
        AAudioStreamBuilder_setDataCallback(bld, play_data_cb, bridge);
        AAudioStreamBuilder_setErrorCallback(bld, play_error_cb, bridge);
    } else {
        AAudioStreamBuilder_setInputPreset(bld, AAUDIO_INPUT_PRESET_VOICE_RECOGNITION);
        AAudioStreamBuilder_setPerformanceMode(bld, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    }

    AAudioStream *stream = NULL;
    aaudio_result_t r = AAudioStreamBuilder_openStream(bld, &stream);
    AAudioStreamBuilder_delete(bld);
    if (r != AAUDIO_OK || !stream) {
        LOGE("open %s stream failed: %s",
             dir == AAUDIO_DIRECTION_OUTPUT ? "output" : "input",
             AAudio_convertResultToText(r));
        return NULL;
    }
    return stream;
}

static void close_play_stream(struct audio_bridge *b, bool reset_ring)
{
    if (!b || !b->play)
        return;
    AAudioStream_requestStop(b->play);
    AAudioStream_close(b->play);
    b->play = NULL;
    b->play_error = false;

    if (reset_ring) {
        pthread_mutex_lock(&b->play_lock);
        play_ring_reset_locked(b);
        pthread_mutex_unlock(&b->play_lock);
    }
}

static bool open_play_stream(struct audio_bridge *b, bool reset_ring)
{
    int old_rate = b->play_rate;
    int old_channels = b->play_channels;
    close_play_stream(b, reset_ring);

    if (b->play_rate <= 0)
        b->play_rate = 48000;
    if (b->play_channels <= 0)
        b->play_channels = WANT_PLAY_CHANNELS;

    b->play = open_stream(b, AAUDIO_DIRECTION_OUTPUT, WANT_PLAY_CHANNELS);
    if (!b->play)
        return false;

    int rate = AAudioStream_getSampleRate(b->play);
    int channels = AAudioStream_getChannelCount(b->play);
    if (rate > 0)
        b->play_rate = rate;
    if (channels > 0)
        b->play_channels = channels;

    pthread_mutex_lock(&b->play_lock);
    bool format_changed = old_rate != b->play_rate || old_channels != b->play_channels;
    bool ring_ok = play_ring_resize_locked(
        b, playback_ring_bytes(b->play_rate, b->play_channels),
        reset_ring || format_changed);
    pthread_mutex_unlock(&b->play_lock);
    if (!ring_ok) {
        LOGE("allocate playback ring failed");
        close_play_stream(b, true);
        return false;
    }

    b->play_idle = true;
    b->play_keepalive_phase = 0;
    b->play_wake_fade_frames = 0;

    aaudio_result_t r = AAudioStream_requestStart(b->play);
    if (r != AAUDIO_OK) {
        LOGE("start output stream failed: %s", AAudio_convertResultToText(r));
        close_play_stream(b, true);
        return false;
    }

    b->play_error = false;
    b->resend_formats = true;
    LOGI("output stream ready: %d Hz x%d ring=%zu",
         b->play_rate, b->play_channels, b->play_ring_size);
    return true;
}

static bool ensure_play_stream_started(struct audio_bridge *b)
{
    if (!b->play || b->play_error)
        return open_play_stream(b, false);

    aaudio_stream_state_t state = AAudioStream_getState(b->play);
    if (state == AAUDIO_STREAM_STATE_STARTED || state == AAUDIO_STREAM_STATE_STARTING)
        return true;

    if (state == AAUDIO_STREAM_STATE_DISCONNECTED) {
        LOGE("output stream disconnected, reopening");
        return open_play_stream(b, false);
    }

    aaudio_result_t r = AAudioStream_requestStart(b->play);
    if (r == AAUDIO_OK)
        return true;

    LOGE("restart output stream from %s failed: %s",
         AAudio_convertStreamStateToText(state),
         AAudio_convertResultToText(r));

    if (r == AAUDIO_ERROR_DISCONNECTED || r == AAUDIO_ERROR_INVALID_STATE)
        return open_play_stream(b, false);

    return false;
}

static void queue_playback_frames(struct audio_bridge *b, const uint8_t *data,
                                  int32_t frames, int packet_channels)
{
    if (!ensure_play_stream_started(b))
        return;

    int channels = b->play_channels > 0 ? b->play_channels : WANT_PLAY_CHANNELS;
    if (channels != packet_channels)
        return;

    size_t bytes = (size_t)frames * (size_t)packet_channels * sizeof(int16_t);
    queue_playback_bytes(b, data, bytes);
}

/* Convert a latency preset (ms) to a frame count at the given rate. 0 ms -> 0 (let
 * the producer pick the default quantum). */
static uint32_t ms_to_frames(int ms, int rate)
{
    if (ms <= 0 || rate <= 0)
        return 0;
    return (uint32_t)(((long)ms * rate) / 1000);
}

/* Tell the producer the device-chosen format + latency preset for one direction. */
static void send_format(int fd, uint32_t role, uint32_t rate, uint32_t channels,
                        uint32_t quantum)
{
    struct audio_format f = {
        .rate = rate,
        .channels = channels,
        .format = AUDIO_FORMAT_S16LE,
        .role = role,
        .quantum = quantum,
    };
    struct audio_msg h = { .type = AUDIO_MSG_FORMAT, .size = sizeof(f) };
    struct iovec iov[2] = {
        { .iov_base = &h, .iov_len = sizeof(h) },
        { .iov_base = &f, .iov_len = sizeof(f) },
    };
    struct msghdr m = { .msg_iov = iov, .msg_iovlen = 2 };
    sendmsg(fd, &m, MSG_DONTWAIT | MSG_NOSIGNAL);
}

/* ---- playback: socket -> speaker ---- */

static void *play_thread_func(void *arg)
{
    struct audio_bridge *b = arg;
    LOGI("playback thread started");

    bool had_fd = false;   /* drives a one-shot format handshake per connection */
    int last_fd = -1;
    /* Last time real PCM was queued; the idle-stop logic below uses it to let the
     * audio path sleep once the desktop has been silent for a while. */
    uint64_t last_pcm_ms = now_ms();

    while (b->running) {
        if (b->play_error)
            open_play_stream(b, false);

        /* Power: with the keepalive callback the output stream never naturally
         * idles, so once the desktop has been silent for PLAY_IDLE_STOP_MS we stop
         * it ourselves. The next PCM arrival restarts it in queue_playback_frames()
         * via ensure_play_stream_started(). requestStop is a no-op unless the stream
         * is actually running, so this is cheap even when already stopped.
         * Skipped when the user enabled "audio keep-alive" (keep the stream hot). */
        if (!b->keepalive_enabled && b->play
                && now_ms() - last_pcm_ms > PLAY_IDLE_STOP_MS) {
            aaudio_stream_state_t st = AAudioStream_getState(b->play);
            if (st == AAUDIO_STREAM_STATE_STARTED || st == AAUDIO_STREAM_STATE_STARTING)
                AAudioStream_requestStop(b->play);
        }

        int fd = current_fd(b);
        if (fd != last_fd) {
            pthread_mutex_lock(&b->play_lock);
            play_ring_reset_locked(b);
            pthread_mutex_unlock(&b->play_lock);
            had_fd = false;
            last_fd = fd;
        }

        if (fd < 0) {
            usleep(20000);
            continue;
        }

        /* Hand the producer the real device formats + latency presets for both
         * directions: once when the socket comes up (just left fallback), and again
         * whenever a preset/device change needs to re-size its PipeWire nodes live. */
        if (!had_fd || b->resend_formats) {
            ensure_play_stream_started(b);
            b->resend_formats = false;
            send_format(fd, AUDIO_ROLE_PLAYBACK, b->play_rate, b->play_channels,
                        ms_to_frames(b->play_latency_ms, b->play_rate));
            send_format(fd, AUDIO_ROLE_CAPTURE, b->cap_rate, b->cap_channels,
                        ms_to_frames(b->cap_latency_ms, b->cap_rate));
            had_fd = true;
        }

        struct pollfd pfd = { .fd = fd, .events = POLLIN };
        int poll_result = poll(&pfd, 1, PLAY_POLL_MS);
        if (poll_result == 0)
            continue;
        if (poll_result < 0)
            continue;
        if (pfd.revents & (POLLHUP | POLLERR)) {
            pthread_mutex_lock(&b->play_lock);
            play_ring_reset_locked(b);
            pthread_mutex_unlock(&b->play_lock);
            had_fd = false;
            last_fd = -1;
            usleep(20000);
            continue;
        }

        ssize_t n;
        TracyCZoneN(zRecv, "audio recv PCM", 1);
        n = recv(fd, b->rx, sizeof(b->rx), 0);
        TracyCZoneEnd(zRecv);
        if (n < (ssize_t)sizeof(struct audio_msg))
            continue;

        struct audio_msg h;
        memcpy(&h, b->rx, sizeof(h));
        if (h.type != AUDIO_MSG_PCM)
            continue;   /* the producer only sends PCM back; formats flow upstream */

        size_t avail = (size_t)n - sizeof(struct audio_msg);
        size_t bytes = h.size < avail ? h.size : avail;
        int play_channels = b->play_channels > 0 ? b->play_channels : WANT_PLAY_CHANNELS;
        size_t frame_bytes = sizeof(int16_t) * (size_t)play_channels;
        bytes -= bytes % frame_bytes;
        int32_t frames = (int32_t)(bytes / frame_bytes);
        if (frames <= 0)
            continue;

        TracyCZoneN(zQueue, "audio queue PCM", 1);
        queue_playback_frames(b, b->rx + sizeof(struct audio_msg), frames, play_channels);
        TracyCZoneEnd(zQueue);
        TracyCPlot("audio packet frames", (float)frames);
        last_pcm_ms = now_ms();
    }

    LOGI("playback thread stopped");
    return NULL;
}

/* ---- capture: mic -> socket ---- */

static void *cap_thread_func(void *arg)
{
    struct audio_bridge *b = arg;
    LOGI("capture thread started");

    bool started = false;
    int cap_channels = b->cap_channels > 0 ? b->cap_channels : WANT_CAP_CHANNELS;
    int16_t *buf = malloc((size_t)MIC_MAX_FRAMES * (size_t)cap_channels * sizeof(*buf));
    if (!buf) {
        LOGE("allocate capture buffer failed");
        return NULL;
    }
    /* ~10 ms per read at the device rate, capped to the buffer. */
    int32_t mic_frames = b->cap_rate / 100;
    if (mic_frames <= 0)
        mic_frames = 1;
    if (mic_frames > MIC_MAX_FRAMES)
        mic_frames = MIC_MAX_FRAMES;

    while (b->running) {
        int fd = current_fd(b);
        if (!b->mic_enabled || fd < 0) {
            if (started && b->rec) {
                AAudioStream_requestStop(b->rec);
                started = false;
            }
            usleep(20000);
            continue;
        }
        if (!b->rec) {
            usleep(20000);
            continue;
        }
        if (!started) {
            if (AAudioStream_requestStart(b->rec) != AAUDIO_OK) {
                usleep(50000);
                continue;
            }
            started = true;
        }

        TracyCZoneN(zMicRead, "mic read", 1);
        int32_t got = AAudioStream_read(b->rec, buf, mic_frames, 100 * 1000 * 1000L);
        TracyCZoneEnd(zMicRead);
        if (got <= 0)
            continue;

        uint32_t bytes = (uint32_t)got * sizeof(int16_t) * (uint32_t)cap_channels;
        struct audio_msg h = { .type = AUDIO_MSG_PCM, .size = bytes };
        struct iovec iov[2] = {
            { .iov_base = &h, .iov_len = sizeof(h) },
            { .iov_base = buf, .iov_len = bytes },
        };
        struct msghdr m = { .msg_iov = iov, .msg_iovlen = 2 };
        TracyCZoneN(zMicSend, "mic send", 1);
        sendmsg(fd, &m, MSG_DONTWAIT | MSG_NOSIGNAL);   /* drop if the socket is full */
        TracyCZoneEnd(zMicSend);
        TracyCFrameMark;
    }

    if (started && b->rec)
        AAudioStream_requestStop(b->rec);
    free(buf);
    LOGI("capture thread stopped");
    return NULL;
}

/* ---- public API ---- */

audio_bridge *audio_create(void)
{
    audio_bridge *b = calloc(1, sizeof(struct audio_bridge));
    if (!b)
        return NULL;
    pthread_mutex_init(&b->play_lock, NULL);
    return b;
}

void audio_destroy(audio_bridge *b)
{
    if (!b)
        return;
    audio_stop(b);
    pthread_mutex_destroy(&b->play_lock);
    free(b->play_ring);
    free(b);
}

void audio_start(audio_bridge *b)
{
    if (!b || b->running)
        return;

    /* Keep the output stream running and silence-fed. Short Linux UI sounds then
     * arrive into a hot Android mixer instead of waking a cold one-shot stream. */
    b->play_rate = 48000;
    b->play_channels = WANT_PLAY_CHANNELS;
    b->resend_formats = true;
    b->play_error = false;
    open_play_stream(b, true);

    /* Open the input stream even before the mic is enabled; it is started/stopped
     * by the capture thread. May be NULL if RECORD_AUDIO is not granted. */
    b->cap_rate = 48000;
    b->cap_channels = WANT_CAP_CHANNELS;
    b->rec = open_stream(b, AAUDIO_DIRECTION_INPUT, WANT_CAP_CHANNELS);
    if (b->rec) {
        b->cap_rate = AAudioStream_getSampleRate(b->rec);
        b->cap_channels = AAudioStream_getChannelCount(b->rec);
    }
    LOGI("device formats: playback %d Hz x%d, capture %d Hz x%d",
         b->play_rate, b->play_channels, b->cap_rate, b->cap_channels);

    b->running = true;
    pthread_create(&b->play_thread, NULL, play_thread_func, b);
    pthread_create(&b->cap_thread, NULL, cap_thread_func, b);
    LOGI("audio bridge started (play=%p rec=%p)", (void *)b->play, (void *)b->rec);
}

void audio_stop(audio_bridge *b)
{
    if (!b || !b->running)
        return;
    b->running = false;
    pthread_join(b->play_thread, NULL);
    pthread_join(b->cap_thread, NULL);

    close_play_stream(b, true);
    if (b->rec) {
        AAudioStream_requestStop(b->rec);
        AAudioStream_close(b->rec);
        b->rec = NULL;
    }
    pthread_mutex_lock(&b->play_lock);
    free(b->play_ring);
    b->play_ring = NULL;
    b->play_ring_size = 0;
    play_ring_reset_locked(b);
    pthread_mutex_unlock(&b->play_lock);
    b->ctx = NULL;
    LOGI("audio bridge stopped");
}

void audio_set_ctx(audio_bridge *b, display_ctx *ctx)
{
    if (!b)
        return;
    b->ctx = ctx;
    if (ctx)
        b->resend_formats = true;
}

void audio_set_mic_enabled(audio_bridge *b, int enabled)
{
    if (!b)
        return;
    b->mic_enabled = enabled != 0;
    LOGI("mic %s", b->mic_enabled ? "enabled" : "disabled");
}

void audio_set_latency(audio_bridge *b, int speaker_ms, int mic_ms)
{
    if (!b)
        return;
    b->play_latency_ms = speaker_ms;
    b->cap_latency_ms = mic_ms;
    b->resend_formats = true;   /* picked up by the playback thread on the live fd */
    LOGI("latency preset: speaker=%dms mic=%dms", speaker_ms, mic_ms);
}

void audio_set_keepalive(audio_bridge *b, int enabled)
{
    if (!b)
        return;
    b->keepalive_enabled = enabled != 0;
    LOGI("audio keep-alive %s", b->keepalive_enabled ? "enabled" : "disabled");
}
