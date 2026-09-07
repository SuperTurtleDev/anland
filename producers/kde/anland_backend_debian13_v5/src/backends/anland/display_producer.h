#ifndef DISPLAY_PRODUCER_H
#define DISPLAY_PRODUCER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "protocol.h"

typedef struct display_ctx display_ctx;

/*
 * Producer-side state machine
 * ----------------------------
 * connect_to_deamon() performs only the daemon handshake: it fetches the screen
 * info and leaves the context in *fallback* (no consumer fds, no dmabufs). The
 * backend then drives a reconnect loop that polls try_exit_fallback(); once that
 * call succeeds the context owns the consumer fds and the dmabuf set, and
 * is_fallback() returns false. Losing the consumer drops the context back to
 * fallback (see set_fallback_callback) and the loop resumes.
 */

/* Connect to the daemon and fetch screen info only. Leaves the context in
 * fallback: it does NOT pick up consumer fds or dmabufs. Returns 0 / -1. */
int  connect_to_deamon(display_ctx **ctx, const char *socket_path);

/* Tear down everything and disconnect from the daemon. */
void disconnect(display_ctx *ctx);

int  get_screen_info(display_ctx *ctx, uint32_t *width, uint32_t *height, uint32_t *format, uint32_t *refresh);

/* Stash the native fence for the current rendered frame. trigger_refresh() sends
 * it to the consumer; ownership of @p fence_fd transfers to display_ctx. */
void set_render_fence(display_ctx *ctx, int fence_fd);

/* Signal the consumer that the current frame is done. No-op in fallback. */
int  trigger_refresh(display_ctx *ctx);

/* Pull one pending input event. Returns 1 if an event was written, 0 if none was
 * available, -1 on consumer loss. No-op (returns 0) in fallback. */
int  poll_input_event(display_ctx *ctx, struct InputEvent *event, int timeout_ms);
int  poll_input_event_extend_data(display_ctx *ctx, void *payload, size_t size, int timeout_ms);

/* Receive the fds that follow an INPUT_TYPE_RESOURCE event (a DATA_MSG_INPUT_EXTEND_FDS
 * message carrying the service's fds as SCM_RIGHTS). Call this right after
 * poll_input_event() returns an INPUT_TYPE_RESOURCE event, using event.resource.fdnum
 * as the expected count. Writes up to max_fds received fds into fds[] and the actual
 * count into *fd_count. Returns 1 on success, 0 if nothing was pending, -1 on error
 * (consumer loss). The caller owns the received fds and must close them. */
int  poll_input_event_extend_fds(display_ctx *ctx, int *fds, int max_fds,
                                 int *fd_count, int timeout_ms);

/* Ask the consumer for a service's resources (e.g. SERVICE_TYPE_CAMERA). Sends an
 * OUTPUT_TYPE_RESOURCES_REQUEST; the consumer replies asynchronously with an
 * INPUT_TYPE_RESOURCE event + fds on the data channel. args may be NULL (treated as
 * three zeros). No-op (returns 0) in fallback. */
int  push_resources_request(display_ctx *ctx, uint32_t service_type, const uint32_t *args);

int  push_output_event(display_ctx *ctx, const struct OutputEvent *event);
int  push_output_event_with_length(display_ctx *ctx, const struct OutputEvent *event, void *payload, size_t size);

/* Register a callback invoked after fallback is set but before consumer-owned fds
 * are closed. It must not re-enter display_producer. */
int  set_pre_release_callback(display_ctx *ctx, void (*on_pre_release)(void *), void *userdata);

/* Register a callback invoked when the consumer is lost and the context drops
 * back to fallback, after its consumer-owned resources are released. */
int  set_fallback_callback(display_ctx *ctx, void (*on_fallback)(void *), void *userdata);

bool is_fallback(display_ctx *ctx);

/* Reject the currently acquired consumer generation after a producer-side import
 * or validation failure. Moves the transport back to fallback, releases all
 * consumer-owned resources and fires fallback callbacks just like a disconnect. */
void reject_consumer_resources(display_ctx *ctx);

/* Attempt to leave fallback: pick up the consumer fds and immediately receive the
 * dmabuf set (the consumer sends the dmabufs right after the fd handshake). Clears
 * fallback only when BOTH the fds and the dmabufs are in hand, so on a 0 return the
 * dmabufs are ready to import right away. Returns 0 on success, -1 if there is
 * still no consumer (stays in fallback; safe to retry on the next tick). */
int  try_exit_fallback(display_ctx *ctx);

int  get_data_fd(display_ctx *ctx);
/* Current local end of the audio socketpair, or -1 in fallback. Changes across
 * reconnects; re-point the audio engine on each exit/enter fallback. */
int  get_audio_fd(display_ctx *ctx);
int  get_buffer_ready_fd(display_ctx *ctx);
int  get_buf_count(display_ctx *ctx);
int  get_selected_idx(display_ctx *ctx);
int  get_dmabuf_fd(display_ctx *ctx);
int  get_dmabuf_fd_at(display_ctx *ctx, int idx);
int  get_dmabuf_info(display_ctx *ctx, struct buf_info *info);
int  get_dmabuf_info_at(display_ctx *ctx, int idx, struct buf_info *info);

#endif
