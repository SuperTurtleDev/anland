#ifndef DISPLAY_PRODUCER_H
#define DISPLAY_PRODUCER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "../common/protocol.h"

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

/* Stash the render-done fence (created in doEndFrame) for the current frame. The
 * next trigger_refresh hands it to the consumer on the dedicated fence channel, so
 * SurfaceFlinger waits on it GPU-side instead of the producer CPU-blocking.
 * Takes ownership of fence_fd (-1 = none). */
void set_render_fence(display_ctx *ctx, int fence_fd);

/* Signal the consumer that the current frame is done by sending one message (with
 * the render fence, if any) on the dedicated fence channel. No-op in fallback. */
int  trigger_refresh(display_ctx *ctx);

/* Pull one pending input event. Returns 1 if an event was written, 0 if none was
 * available, -1 on consumer loss. No-op (returns 0) in fallback. */
int  poll_input_event(display_ctx *ctx, struct InputEvent *event, int timeout_ms);
int poll_input_event_extend_data(display_ctx *ctx, void* payload, size_t size, int timeout_ms);

int push_output_event(display_ctx *ctx, const struct OutputEvent *event);
//接收到输出事件时，可能会有额外的数据需要接收，所以增加一个带长度的版本
//但是发送方必须设置变长事件的size字段，表示随后数据的大小，而且必须紧跟事件发送数据
//变长事件不得使用push_output_event发送，必须使用push_output_event_with_length发送
//接收端使用标准事件接收后根据size字段知道后续数据的大小，务必使用socket手动接收变长数据（必须有超时，避免对端挂掉）
int push_output_event_with_length(display_ctx *ctx, const struct OutputEvent *event, void* payload, size_t size);
/* Register a callback invoked when the consumer is lost and the context drops
 * back to fallback. */
int  set_fallback_callback(display_ctx *ctx, void (*on_fallback)(void *), void *userdata);

bool is_fallback(display_ctx *ctx);
void enter_fallback(display_ctx *ctx);

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
