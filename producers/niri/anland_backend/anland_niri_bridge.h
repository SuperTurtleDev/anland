/* Stable, layout-free C ABI for the niri/Smithay adapter. No Smithay types. */
#ifndef ANLAND_NIRI_BRIDGE_H
#define ANLAND_NIRI_BRIDGE_H
#include <stdint.h>
#include <stddef.h>

typedef struct anland_niri_bridge anland_niri_bridge;
typedef struct anland_niri_output {
    uint32_t width, height, refresh_mhz, format;
} anland_niri_output;
typedef struct anland_niri_target {
    uint64_t generation;
    uint32_t index, count, width, height, format, stride, offset;
    uint64_t modifier;
    int fd; /* dup'ed; caller owns and closes; -1 if not available */
} anland_niri_target;
typedef struct anland_niri_event {
    uint32_t type; /* ANLAND_SCENE_EVENT_* from anland_scene.h */
    uint64_t generation, commit_id;
    uint32_t index, count, width, height, refresh_mhz;
    uint64_t presentation_ns, buffer_id;
    int release_fence_fd; /* owned, -1 if none; caller must close */
} anland_niri_event;
typedef struct anland_niri_input {
    uint32_t type;
    int32_t action, code, pointer_id, discrete;
    float x, y, dx, dy, value;
    uint32_t payload_size, resource_type, fd_count;
} anland_niri_input;

/* Explicit legacy-only entry. A requested AWL session is rejected, not silently
 * changed to legacy. May exist while no consumer is connected. */
anland_niri_bridge *anland_niri_open(const char *endpoint);
void anland_niri_close(anland_niri_bridge *bridge);
int anland_niri_pump(anland_niri_bridge *bridge, int timeout_ms);
int anland_niri_reconnect(anland_niri_bridge *bridge);
int anland_niri_reopen(anland_niri_bridge *bridge, const char *endpoint);
int anland_niri_connected(anland_niri_bridge *bridge);
int anland_niri_daemon_alive(anland_niri_bridge *bridge);
uint64_t anland_niri_generation(anland_niri_bridge *bridge);
void anland_niri_drop_session(anland_niri_bridge *bridge);
int anland_niri_output_info(anland_niri_bridge *bridge, anland_niri_output *out);
/* The target fd belongs to the caller; generation must be rechecked at submit. */
int anland_niri_get_target(anland_niri_bridge *bridge, anland_niri_target *out);
/* The fence is BORROWED; submit duplicates it when necessary. */
int anland_niri_submit(anland_niri_bridge *bridge, uint64_t generation,
                       uint32_t index, int acquire_fence_fd, uint64_t *commit_id);
/* Drain up to capacity events; any release_fence_fd is caller-owned. */
int anland_niri_dispatch(anland_niri_bridge *bridge, anland_niri_event *events,
                         size_t capacity, size_t *count);
/* Poll one input event: 1 event, 0 none, negative disconnected/error.
 * For clipboard/text/resource events, read the announced payload/fds before
 * polling the next event. */
int anland_niri_poll_input(anland_niri_bridge *bridge, anland_niri_input *out);
int anland_niri_read_payload(anland_niri_bridge *bridge, void *buf, size_t size,
                              int timeout_ms);
int anland_niri_read_fds(anland_niri_bridge *bridge, int *fds, int max_fds,
                         int *count, int timeout_ms);
int anland_niri_send_clipboard(anland_niri_bridge *bridge, const void *buf, size_t size);
int anland_niri_request_resources(anland_niri_bridge *bridge, uint32_t service);
int anland_niri_audio_fd(anland_niri_bridge *bridge);
#endif