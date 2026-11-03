#ifndef ANLAND_DENIAL_SESSION_H
#define ANLAND_DENIAL_SESSION_H
#include <stddef.h>
#include <stdint.h>

/* In-process ABI only. One compositor thread owns this opaque session. */
typedef struct ad_session ad_session;
typedef struct ad_target {
    uint64_t generation;
    uint32_t index, count, width, height;
} ad_target;
typedef struct ad_image {
    uint64_t modifier;
    uint32_t width, height, stride, offset, fourcc;
    int32_t fd; /* owned on success, -1 on failure */
} ad_image;
typedef struct ad_output { uint32_t width, height, refresh_mhz; } ad_output;
typedef struct ad_fds { int32_t scene, ready, input; } ad_fds;
enum ad_event_type { AD_PRESENTED = 1, AD_RELEASED, AD_DROPPED, AD_OUTPUT, AD_TARGET };
typedef struct ad_event {
    uint64_t commit_id, generation, buffer_id, presentation_ns;
    uint32_t type, reason, index, count, width, height, refresh_mhz;
    int32_t release_fd; /* owned by receiver, -1 if absent */
} ad_event;
typedef struct ad_tablet_sample {
    uint32_t tool_id, tool_type, capabilities, device_id;
    uint32_t flags, buttons, time_msec;
    float x, y, pressure, tilt_x, tilt_y, distance;
} ad_tablet_sample;
typedef struct ad_input {
    uint32_t type, size, resource_type, fd_count;
    int32_t action, code, pointer_id, discrete;
    float x, y, dx, dy, value;
    ad_tablet_sample tablet_previous, tablet_current;
} ad_input;

ad_session *ad_open(const char *endpoint);
void ad_close(ad_session *session);
int ad_connected(ad_session *session);
int ad_daemon_alive(ad_session *session);
int ad_reconnect(ad_session *session);
int ad_reopen(ad_session *session, const char *endpoint);
void ad_drop(ad_session *session);
int ad_get_fds(ad_session *session, ad_fds *out); /* borrowed; refresh on reconnect */
int ad_get_output(ad_session *session, ad_output *out);
int ad_writable(ad_session *session, ad_target *out);
int ad_import(ad_session *session, uint64_t generation, uint32_t index, ad_image *out);
/* Fence is borrowed. Acceptance and handoff MUST remain separate calls so the
 * caller records the commit-to-native-frame mapping before present can fail. */
int ad_commit(ad_session *session, const ad_target *rendered, int32_t fence, uint64_t *id);
int ad_present(ad_session *session);
int ad_pump(ad_session *session);
/* One event at a time prevents unconsumed release-fence array ownership. */
int ad_next_event(ad_session *session, ad_event *out); /* 1 event, 0 none, negative error */
size_t ad_events_pending(ad_session *session);
/* Borrowed Linux sync_file; 1 completed, 0 pending, negative error. Never waits. */
int ad_fence_signaled(int32_t fd);
int ad_poll_input(ad_session *session, ad_input *out);
/* 1 for a partially consumed event header, 0 for idle/completed/disconnected. */
int ad_input_header_pending(ad_session *session);
int ad_read_payload(ad_session *session, void *data, size_t size);
int ad_read_fds(ad_session *session, int32_t *fds, uint32_t capacity, uint32_t *count);
int ad_clipboard(ad_session *session, const void *data, size_t size);
int ad_flush(ad_session *session);
size_t ad_pending_output(ad_session *session);
int ad_capture_pointer(ad_session *session, uint32_t capture);
#endif
