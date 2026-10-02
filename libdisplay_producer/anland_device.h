/*
 * anland_device.h — DRM-like userspace display device interface
 *
 * Unified foundation that every supported DE backend (KWin / Mutter / ...)
 * translates to, replacing the model where each backend carries its own
 * copy of the anland transport glue (and often its own protocol fork).
 *
 * It wraps the existing producer transport (display daemon +
 * libdisplay_producer) behind DRM semantics:
 *
 *   device            ↔ display_daemon connection (single producer session)
 *   connector/CRTC    ↔ screen_info (width/height/format/refresh_mhz)
 *   framebuffer/plane ↔ consumer-owned dmabuf set (MAX_BUFS = 8)
 *   atomic commit     ↔ consumer-driven selected index (shm) + buf_ready
 *   page flip         ↔ fence channel (one render-done message per frame)
 *   fence fd          ↔ set_render_fence + trigger_refresh (GPU-side sync)
 *   evdev             ↔ poll_input_event (touch/key/pointer/clipboard/...)
 *   DRM master        ↔ anland_device_open (single producer, daemon-enforced)
 *
 * The API deliberately mirrors drmMode* shapes so that:
 *   1. each DE backend shrinks to a thin shape-translation layer
 *      (no anland protocol / fallback / dmabuf knowledge needed);
 *   2. if a real virtual DRM device later becomes possible, backends keep
 *      working unchanged — only this interface's implementation swaps to a
 *      /dev/dri/cardN shim.
 *
 * Threading: same contract as the underlying transport. The frame path
 * (commit/pageflip/get_fb) and the input path (poll_input) may run on
 * different threads; the underlying libdisplay_producer already serializes.
 */
#ifndef ANLAND_DEVICE_H
#define ANLAND_DEVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h> /* pid_t */

#ifdef __cplusplus
extern "C" {
#endif

#define ANLAND_DEVICE_MAX_BUFS 8
#define ANLAND_DEVICE_MAX_NAME 64
#define ANLAND_DEVICE_DEFAULT_SOCKET "/data/local/tmp/display_daemon.sock"
/* Largest variable-length payload (clipboard / text input) a backend may
 * announce and read back in one anland_device_read_input() call. Matches the
 * daemon/consumer limit, so an oversized header is rejected before allocating. */
#define ANLAND_DEVICE_MAX_PAYLOAD_SIZE (1024 * 1024)

typedef struct anland_device anland_device;

/* ---- output / connector (mirrors drmModeConnector + modes) ---- */

typedef struct anland_device_output {
    char     name[ANLAND_DEVICE_MAX_NAME];
    uint32_t width;        /* logical (consumer native) resolution */
    uint32_t height;
    uint32_t format;       /* 1 = ABGR8888 (protocol convention) */
    uint32_t refresh_mhz;  /* milli-Hz */
    bool     connected;    /* false while consumer absent (fallback) */
    bool     primary;
} anland_device_output_t;

/* ---- framebuffer / plane ---- */

typedef struct anland_device_fb {
    int      fd;           /* dup'ed dmabuf fd; caller-owned, close when done */
    uint32_t width;
    uint32_t height;
    uint32_t stride;       /* bytes */
    uint32_t format;
    uint64_t modifier;
    uint32_t offset;
} anland_device_fb_t;

/* ---- input events (evdev-like subset, wire-compatible with InputEvent) ---- */

enum {
    ANLAND_DEVICE_IN_TOUCH           = 1,
    ANLAND_DEVICE_IN_KEY             = 2,
    ANLAND_DEVICE_IN_PTR_MOTION      = 3,
    ANLAND_DEVICE_IN_PTR_BUTTON      = 4,
    ANLAND_DEVICE_IN_PTR_AXIS        = 5,
    ANLAND_DEVICE_IN_TOUCH_FRAME     = 6,
    ANLAND_DEVICE_IN_DISPLAY_REFRESH = 7,
    ANLAND_DEVICE_IN_CLIPBOARD       = 8,
    ANLAND_DEVICE_IN_TEXT_INPUT      = 9,
    ANLAND_DEVICE_IN_ACTION          = 10,
    ANLAND_DEVICE_IN_RESOURCE        = 11,
    /* Consumer reports the requested service's fds were unusable. Same layout
     * as RESOURCE (type + fdnum) — matches protocol.h INPUT_TYPE_RESOURCE_INVALID. */
    ANLAND_DEVICE_IN_RESOURCE_INVALID = 12,
};

/* Touch/button action codes carried by ANLAND_DEVICE_IN_TOUCH /
 * ANLAND_DEVICE_IN_KEY (evdev-style, wire-compatible with the transport). */
enum {
    ANLAND_DEVICE_ACTION_DOWN = 0,
    ANLAND_DEVICE_ACTION_UP   = 1,
    ANLAND_DEVICE_ACTION_MOVE = 2,
};

/* Service identifiers for anland_device_request_resources(). */
enum {
    ANLAND_DEVICE_SERVICE_CAMERA = 1,
};

/* Flags for anland_device_scheduling(). */
enum {
    ANLAND_DEVICE_SCHED_FLAG_SETTREE = 0x01,  /* whole process subtree */
    ANLAND_DEVICE_SCHED_FLAG_ON      = 0x02,  /* 1 = top-app, 0 = restore */
};

/* Consumer vars for anland_device_set_consumer_var(). */
enum {
    /* Force-enable Android pointer capture while a Wayland client holds an
     * active pointer lock / confinement; 0 = release. */
    ANLAND_DEVICE_VAR_CAPTURE_MOUSE = 1,
};

typedef struct anland_device_input {
    uint32_t type;
    union {
        struct { int32_t action; float x, y; int32_t pointer_id; } touch;
        struct { int32_t action; int32_t keycode; } key;
        struct { float x, y, dx, dy; } pointer_motion;
        struct { uint32_t button; int32_t pressed; } pointer_button;
        struct { uint32_t axis; float value; int32_t discrete; } pointer_axis;
        struct { uint32_t refresh_mhz; } display;
        struct { uint32_t size; } clipboard;   /* payload via anland_device_read_input */
        struct { uint32_t size; } text_input;  /* payload via anland_device_read_input */
        struct { uint32_t action; int32_t value; } input_action;
        struct { uint32_t type; uint32_t fdnum; } resource; /* fds via anland_device_read_fds */
        struct { uint32_t padding[4]; };
    };
} anland_device_input_t;

/* ---- callbacks ---- */

/* Consumer lost/re-established. Same contract as the transport fallback
 * callback: fired from transport internals, must not re-enter anland_device. */
typedef void (*anland_device_fallback_cb)(void *userdata);

/* Fired right before the consumer's fds are released (on fallback /
 * teardown) so the backend can detach engines borrowing them (audio...).
 * Must not re-enter anland_device. */
typedef void (*anland_device_pre_release_cb)(void *userdata);

/* ---- device lifecycle (DRM master semantics) ---- */

/* Open the daemon connection and fetch screen info. Leaves the device in
 * fallback until anland_device_connect() succeeds.
 *
 * socket_path is a HINT, not a requirement: pass NULL or "" to rely on
 * $ANLAND_SOCKET and the built-in well-known locations (the Android default plus
 * the /run and /tmp paths a container or desktop session mounts the daemon on).
 * A non-empty path is tried first and only then falls back to those. Returns NULL
 * when no candidate answered. */
anland_device *anland_device_open(const char *socket_path);

/* The daemon socket the device actually connected to ("" when unknown). Callers
 * that pass a hint should log this instead of the path they requested. */
const char *anland_device_socket_path(const anland_device *dev);

void anland_device_close(anland_device *dev);

/* True once a consumer session is up (not in fallback). */
bool anland_device_is_connected(const anland_device *dev);

/* One attempt to leave fallback (pickup fds + receive dmabufs). Returns 0
 * when connected, -1 when the consumer is not (yet) available — safe to
 * retry on a timer (mirrors the transport's 200 ms reconnect loop). */
int anland_device_connect(anland_device *dev);
/* Same attempt as anland_device_connect(), reporting WHY it failed. A backend
 * reconnect loop must tell the two apart: NO_CONSUMER means keep polling, while
 * DAEMON_LOST means the control connection is dead and the device has to be
 * reopened (anland_device_reopen) before a consumer can ever appear again. */
typedef enum anland_device_connect_result {
    ANLAND_DEVICE_CONNECT_READY = 0,
    ANLAND_DEVICE_CONNECT_NO_CONSUMER = 1,
    ANLAND_DEVICE_CONNECT_DAEMON_LOST = 2,
} anland_device_connect_result_t;
/* Performs the same pickup as anland_device_connect() and classifies the
 * failure. READY leaves the device connected. */
anland_device_connect_result_t
anland_device_connect_result(anland_device *dev);

/* Drop the consumer session without dropping the daemon connection: the next
 * anland_device_connect() re-establishes it. Mirrors the legacy transport's
 * enter_fallback() and is what a backend calls when it cannot use the session
 * it was handed (e.g. an EGL/dmabuf import failure). Deliberately does NOT fire
 * the fallback callback — the caller already knows it dropped the session. */
void anland_device_force_fallback(anland_device *dev);

/* True while the daemon control connection is still usable. False means the daemon
 * died or restarted: the device can no longer reach a consumer and must be closed
 * and reopened (the reconnect loop is pointless until then). Mirrors the legacy
 * transport's DAEMON_LOST pickup result. */
bool anland_device_is_daemon_alive(const anland_device *dev);

/* Reopen the daemon connection without replacing the public device object. The
 * current consumer session is discarded, registered callbacks are preserved, and
 * the object remains valid for scene/layer owners. Returns 0 when the daemon
 * handshake succeeds; the device stays disconnected and remains retryable on -1. */
int anland_device_reopen(anland_device *dev, const char *socket_path);

void anland_device_set_fallback_cb(anland_device *dev,
                                   anland_device_fallback_cb cb, void *userdata);

/* Register the pre-release hook (see anland_device_pre_release_cb). */
void anland_device_set_pre_release_cb(anland_device *dev,
                                      anland_device_pre_release_cb cb,
                                      void *userdata);

/* ---- transport fd accessors (Qt event-loop integration) ----
 * These mirror the raw display_producer getters so a backend can hook the
 * data / buffer-ready / audio sockets into its own event loop (QSocketNotifier,
 * GSource, ...) instead of being forced into a polling model. FDs change on
 * reconnect; re-query after anland_device_connect() returns 0. Return -1 when
 * not connected. */

/* Data channel fd (consumer input / our output events). */
int anland_device_data_fd(anland_device *dev);
/* eventfd signalled when the consumer has presented a frame (buffer-ready). */
int anland_device_buffer_ready_fd(anland_device *dev);
/* Audio socketpair endpoint (playback+capture PCM), or -1 in fallback. */
int anland_device_audio_fd(anland_device *dev);

/* ---- connector / CRTC enum ---- */

/* Fill up to max outputs and return the count (currently always 1). */
int anland_device_get_outputs(anland_device *dev,
                              anland_device_output_t *outs, int max);

/* ---- framebuffers ---- */

/* Number of consumer-owned dmabufs (>= 1 once connected). */
int anland_device_fb_count(anland_device *dev);

/* Snapshot the consumer's fb at index into *fb (fd = dup of the transport's
 * fd; caller owns and must close it). Returns 0 / -1. */
int anland_device_get_fb(anland_device *dev, int index, anland_device_fb_t *fb);

/* Index the consumer selected for the current frame (shm page). Clamped into
 * [0, fb_count) so a backend can index its framebuffer array directly. */
int anland_device_current_fb(anland_device *dev);
/* The same shared-page index WITHOUT clamping: -1 when no session is mapped,
 * otherwise the raw value the consumer published. A backend that wants to
 * detect a corrupt or stale index (instead of silently presenting fb 0) checks
 * it against anland_device_fb_count() before switching framebuffers. */
int anland_device_current_fb_raw(anland_device *dev);

/* ---- atomic commit + page flip ---- */

/* Validate and select the fb to present on the next pageflip. Producer-side
 * bookkeeping only; handoff happens in anland_device_pageflip(). */
int anland_device_commit(anland_device *dev, int fb_index);

/* Submit the selected fb to the consumer (one render-done message on the
 * fence channel). fence_fd (may be -1) rides along via SCM_RIGHTS so
 * SurfaceFlinger can wait GPU-side. cb (optional) is invoked with fb_index
 * once the handoff message has been sent. Returns 0 / -1. */
int anland_device_pageflip(anland_device *dev, int fence_fd,
                           void (*cb)(void *userdata, int fb_index),
                           void *userdata);

/* Split-frame variants for backends that stash the fence in their render
 * pass and hand the frame over later (KWin: doEndFrame stashes the
 * EGLNativeFence, AnlandOutput::present triggers the handoff). Same
 * underlying transport as pageflip(): set_render_fence stashes the fd
 * (takes ownership), trigger_refresh sends one refresh-done message with
 * whatever fence is stashed. Use the pair OR pageflip(), never mixed in one
 * frame. */
void anland_device_set_render_fence(anland_device *dev, int fence_fd);
int  anland_device_trigger_refresh(anland_device *dev);
/* trigger_refresh() with the legacy transport's return contract: true when the
 * frame-done message (and the stashed fence, if any) actually reached the
 * consumer, false in fallback — where nothing was sent and the stashed fence was
 * released. A backend that must set its frame result to "idle" (rather than
 * "pending presented") when the frame never left uses this. */
bool anland_device_notify_frame_done(anland_device *dev);

/* ---- input (evdev-like) ---- */

/* Poll one input event. Returns 1 = event, 0 = none, -1 = consumer lost. */
int anland_device_poll_input(anland_device *dev, anland_device_input_t *ev,
                             int timeout_ms);

/* Read a variable-length payload (clipboard / text input) announced by the
 * previous poll_input() event. Returns 1 / 0 / -1. */
int anland_device_read_input(anland_device *dev, void *buf, size_t size,
                             int timeout_ms);

/* Receive the fds that follow an INPUT_TYPE_RESOURCE event. Caller owns them.
 * Returns 1 / 0 / -1. */
int anland_device_read_fds(anland_device *dev, int *fds, int max_fds,
                           int *fd_count, int timeout_ms);

/* ---- producer→consumer control channel (thin passthrough) ----
 * The underlying transport already carries a fixed set of non-frame output
 * events (clipboard, resources request, consumer vars, foreground
 * scheduling). Backends keep using them for DE-specific integrations; the
 * device layer merely forwards. All are no-ops (return 0) in fallback. */

/* Ask the consumer for a service's fds (e.g. camera). */
int anland_device_request_resources(anland_device *dev, uint32_t service_type,
                                    const uint32_t *args);
/* Set a transient Android runtime var (CONSUMER_VAR_*). */
int anland_device_set_consumer_var(anland_device *dev, uint32_t var,
                                   uint32_t value);
/* Foreground-scheduling switch (SCHEDULING_FLAG_*). */
int anland_device_scheduling(anland_device *dev, pid_t pid, uint8_t flags);
/* Push clipboard text (raw UTF-8) to the consumer. */
int anland_device_set_clipboard(anland_device *dev, const void *text,
                                size_t size);

#ifdef __cplusplus
}
#endif

#endif /* ANLAND_DEVICE_H */