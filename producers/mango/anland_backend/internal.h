#ifndef MANGO_ANLAND_INTERNAL_H
#define MANGO_ANLAND_INTERNAL_H
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifdef HAVE_ANLAND
#include "anland.h"
#include "text.h"
#include "mango/common/server.h"
#include "mango/ext-protocol/text-input.h"
#include <anland_de_backend.h>
#include <anland_audio.h>
#include <anland_camera.h>
#include <anland_tablet.h>
#include <drm_fourcc.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/dma-buf.h>
#include <poll.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>
#include <wayland-server-core.h>
#include <wlr/backend/interface.h>
#include <wlr/interfaces/wlr_buffer.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/interfaces/wlr_output.h>
#include <wlr/interfaces/wlr_pointer.h>
#include <wlr/interfaces/wlr_touch.h>
#include <wlr/interfaces/wlr_tablet_tool.h>
#include <wlr/render/swapchain.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_pointer_constraints_v1.h>
#include <wlr/util/log.h>

#define RECONNECT_MS 200
#define INPUT_PRESSED_MAX 64
#define INPUT_PAYLOAD_MAX ANLAND_DEVICE_MAX_PAYLOAD_SIZE
#define CLIPBOARD_MIME_UTF8 "text/plain;charset=utf-8"
#define CLIPBOARD_MIME_TEXT "text/plain"
#define CLIPBOARD_TRANSFER_TIMEOUT_MS 1000

struct anland_clipboard_read;
struct anland_clipboard_write;
struct anland_buffer {
	struct wlr_buffer base;
	struct wlr_dmabuf_attributes attrs;
	bool submitted; /* native lock released ONLY on public BUFFER_RELEASED */
};
struct anland_backend;
struct anland_output {
	struct wlr_output base;
	struct anland_backend *backend;
	struct wlr_swapchain *swapchain;
	struct anland_buffer *buffers[ANLAND_DEVICE_MAX_BUFS];
	uint64_t active_commit;
	uint32_t active_seq;
	uint64_t generation;
	uint32_t buffer_count;
};

struct anland_backend {
	struct wlr_backend base;
	struct wl_event_loop *loop;
	anland_de_backend *de; /* owned */
	anland_device *device; /* borrowed, owned by de */
	uint64_t output_layer;
	char *socket_path;
	int drm_fd;
	struct anland_output output;
	struct wlr_keyboard keyboard;
	struct mango_anland_text *text;
	struct wlr_pointer pointer;
	struct wlr_touch touch;
    struct wlr_tablet tablet;
    struct wlr_tablet_tool tablet_tools[ANLAND_TABLET_MAX_TOOLS];
    uint32_t tablet_tool_ids[ANLAND_TABLET_MAX_TOOLS];
    anland_tablet tablet_state;
	struct wl_event_source *data_source, *ready_source, *reconnect_source;
	struct wl_event_source *scene_source, *frame_timer, *pump_timer;
	struct wl_event_source *controls_timer;
	struct wl_listener focus_change;
	bool destroying, session_lost;
	bool focus_listener_set;
	pid_t active_scheduling_pid;
	bool capture_mouse_sent, capture_mouse_known;
	bool camera_started;
	bool ime_grabbed;
    uint32_t pressed_buttons[INPUT_PRESSED_MAX], button_count;
    uint32_t touch_ids[INPUT_PRESSED_MAX], touch_count;
	uint32_t width, height, format, refresh;
	bool announced;
	bool output_initialized;
	bool selection_listener_set;
	struct wl_listener selection_listener;
	bool audio_started;
	struct anland_clipboard_read *clipboard_read;
	struct wl_list clipboard_writes;
	struct wl_event_source *output_timer;
	bool input_fds_pending;
	uint32_t input_fd_count;
	char *input_payload;
	size_t input_payload_size;
	uint32_t input_payload_type;
	uint32_t input_resource_type;
	bool input_resource_invalid;
};

extern const struct wlr_keyboard_impl mango_private_keyboard_impl;
extern const struct wlr_pointer_impl mango_private_pointer_impl;
extern const struct wlr_touch_impl mango_private_touch_impl;
extern const struct wlr_tablet_impl mango_private_tablet_impl;


extern const struct wlr_output_impl mango_private_output_impl;

/* Private shape translations, never a public producer ABI. */
void mango_private_set_clipboard_from_consumer(struct anland_backend *backend,
		const char *payload, size_t size);
uint32_t mango_private_now_msec(void);
void mango_private_send_frame_result(struct anland_output *output, uint32_t seq, bool presented);
void mango_private_drop_buffers(struct anland_backend *backend);
bool mango_private_import_buffers(struct anland_backend *backend);
int mango_private_handle_ready(int fd, uint32_t mask, void *data);
int mango_private_handle_input(int fd, uint32_t mask, void *data);
void mango_private_reset_input(struct anland_backend *backend);
void mango_private_replay_tablets(struct anland_backend *backend);
void mango_private_clear_input_payload(struct anland_backend *backend);
void mango_private_cancel_clipboard_read(struct anland_backend *backend);
void mango_private_clear_clipboard_writes(struct anland_backend *backend);
void mango_private_clear_output_messages(struct anland_backend *backend);
void mango_private_handle_seat_set_selection(struct wl_listener *listener, void *data);
int mango_private_handle_scene(int fd, uint32_t mask, void *data);
void mango_private_dispatch_scene(struct anland_backend *backend);
void mango_private_drop_session(struct anland_backend *backend);
void mango_private_arm_frame(struct anland_backend *backend);
void mango_private_retry_pump(struct anland_backend *backend);
void mango_private_sync_scheduling(struct anland_backend *backend, bool force);
void mango_private_sync_consumer_var(struct anland_backend *backend, bool force);
void mango_private_handle_focus_change(struct wl_listener *listener, void *data);

#endif
#endif
