#ifndef MANGO_ANLAND_TEXT_H
#define MANGO_ANLAND_TEXT_H
#include <stdbool.h>
#include <stddef.h>
struct wl_event_loop;
struct wlr_seat;
struct wlr_keyboard;
struct wlr_keyboard_key_event;
struct mango_input_method_relay;
struct mango_anland_text;
/* Private input translation, not a transport or public producer API. */
struct mango_anland_text *mango_anland_text_create(struct wl_event_loop *loop,
	struct wlr_seat *seat);
bool mango_anland_text_submit(struct mango_anland_text *text,
	struct mango_input_method_relay *relay, const char *utf8, size_t length);
/* The backend keyboard must outlive this text adapter; destruction cancels FIFO. */
bool mango_anland_text_handle_key(struct mango_anland_text *text,
	struct wlr_keyboard *keyboard, const struct wlr_keyboard_key_event *event);
void mango_anland_text_set_ime_grab(struct mango_anland_text *text, bool grabbed);
void mango_anland_text_reset(struct mango_anland_text *text);
void mango_anland_text_destroy(struct mango_anland_text *text);
#endif
