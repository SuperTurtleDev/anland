#define _GNU_SOURCE
#include "text.h"
#include "mango/ext-protocol/text-input.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <wayland-server-core.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

/* Gamescope-style local keyboard fallback. Never announced as a Mango input
 * device: generated presses go only through the seat, not WM key bindings. */
#define TEXT_QUEUE_MAX 65536
#define TEXT_BATCH_MAX 32
#define TEXT_KEYMAP_HOLD_MS 100
/* NoSymbol denotes a normal key event; validated text never uses NoSymbol. */
struct text_item {
	xkb_keysym_t symbol;
	struct wlr_keyboard_key_event key;
};
struct mango_anland_text {
	struct wlr_seat *seat;
	struct wlr_keyboard keyboard;
	struct wlr_keyboard *restore_keyboard;
	struct wl_listener focus_change, seat_destroy, keyboard_destroy, grab_begin;
	struct wl_listener key_keyboard_destroy;
	struct wlr_keyboard *key_keyboard;
	bool ime_grabbed;
	uint64_t reset_serial;
	struct wl_event_source *timer;
	struct text_item *queue;
	size_t count;
	/* Cache the installed map, not a map per packet. A shorter final batch
	 * can reuse a matching prefix of a previous batch's slots. */
	xkb_keysym_t mapped[TEXT_BATCH_MAX];
	size_t mapped_count;
	bool batch_active; /* Unicode keyboard owns the seat, including idle time. */
	bool holding; /* last presses need time to be consumed before v3 takes over */
};
static const struct wlr_keyboard_impl text_keyboard_impl = {.name = "anland-text"};

/* Validate the entire bounded payload before queuing anything. */
static bool decode_utf8(const char *s, size_t length, xkb_keysym_t *symbols,
		size_t *count) {
	*count = 0;
	for (size_t i = 0; i < length;) {
		uint32_t cp = (unsigned char)s[i++];
		unsigned n;
		if (cp < 0x80) n = 0;
		else if (cp >= 0xc2 && cp <= 0xdf) { cp &= 0x1f; n = 1; }
		else if (cp >= 0xe0 && cp <= 0xef) { cp &= 0x0f; n = 2; }
		else if (cp >= 0xf0 && cp <= 0xf4) { cp &= 7; n = 3; }
		else return false;
		if (i + n > length) return false;
		for (unsigned j = 0; j < n; j++) {
			unsigned char c = s[i++];
			if ((c & 0xc0) != 0x80) return false;
			cp = (cp << 6) | (c & 0x3f);
		}
		if (!cp || (n == 1 && cp < 0x80) || (n == 2 && cp < 0x800) ||
				(n == 3 && cp < 0x10000) || cp > 0x10ffff ||
				(cp >= 0xd800 && cp <= 0xdfff)) return false;
		xkb_keysym_t sym = cp == '\n' || cp == '\r' ? XKB_KEY_Return :
			cp == '\t' ? XKB_KEY_Tab : cp == '\b' ? XKB_KEY_BackSpace :
			xkb_utf32_to_keysym(cp);
		if (sym == XKB_KEY_NoSymbol) return false;
		symbols[(*count)++] = sym;
	}
	return true;
}
static void restore_keyboard(struct mango_anland_text *text) {
	if (!text->batch_active) return;
	if (text->seat && wlr_seat_get_keyboard(text->seat) == &text->keyboard)
		wlr_seat_set_keyboard(text->seat, text->restore_keyboard);
	wl_list_remove(&text->keyboard_destroy.link);
	wl_list_init(&text->keyboard_destroy.link);
	text->restore_keyboard = NULL;
	text->keyboard.keymap_client = NULL;
	text->keyboard.keymap_fallback = NULL;
	text->mapped_count = 0; /* Never reuse another focus/client's mapped suffix. */
	text->batch_active = false;
	text->holding = false;
}
void mango_anland_text_reset(struct mango_anland_text *text) {
	if (!text) return;
	++text->reset_serial;
	if (text->timer) wl_event_source_timer_update(text->timer, 0);
	/* A focus/grab boundary cancels unforwarded presses, but not releases
	 * of keys already forwarded. Replay those on the next timer (not inside
	 * a key callback: wlroots updates its xkb state after emitting key). */
	size_t keep = 0;
	for (size_t i = 0; text->key_keyboard && i < text->count; i++) {
		struct text_item item = text->queue[i];
		if (item.symbol || item.key.state != WL_KEYBOARD_KEY_STATE_RELEASED) continue;
		for (size_t j = 0; j < text->key_keyboard->num_keycodes; j++) {
			if (item.key.keycode == text->key_keyboard->keycodes[j]) {
				text->queue[keep++] = item;
				break;
			}
		}
	}
	text->count = keep;
	restore_keyboard(text);
	text->holding = false;
	if (keep) wl_event_source_timer_update(text->timer, 1);
}
void mango_anland_text_set_ime_grab(struct mango_anland_text *text, bool grabbed) {
	if (!text) return;
	/* input-method-v2 is not a wlr_seat keyboard grab. Restore before the
	 * relay selects its keyboard, and cancel text accepted before takeover. */
	if (grabbed) mango_anland_text_reset(text);
	text->ime_grabbed = grabbed;
}
static void handle_key_keyboard_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct mango_anland_text *text = wl_container_of(listener, text, key_keyboard_destroy);
	text->key_keyboard = NULL;
	mango_anland_text_reset(text);
	wl_list_remove(&text->key_keyboard_destroy.link);
	wl_list_init(&text->key_keyboard_destroy.link);
}
bool mango_anland_text_handle_key(struct mango_anland_text *text,
		struct wlr_keyboard *keyboard, const struct wlr_keyboard_key_event *event) {
	if (!keyboard || !event) return false;
	/* A normal-map hold blocks only the next Unicode map, not normal keys.
	 * Pending items still enforce FIFO, including reset-preserved releases. */
	if (!text || (!text->count && (!text->holding || !text->batch_active))) {
		if (text) restore_keyboard(text);
		uint64_t serial = text ? text->reset_serial : 0;
		struct wlr_keyboard_key_event copy = *event;
		wlr_keyboard_notify_key(keyboard, &copy);
		if (text && serial == text->reset_serial && text->holding && !text->batch_active)
			wl_event_source_timer_update(text->timer, TEXT_KEYMAP_HOLD_MS);
		return true;
	}
	if (text->count == TEXT_QUEUE_MAX) return false;
	if (text->key_keyboard != keyboard) {
		/* One Anland backend supplies one keyboard. Never replay queued
		 * events against a replacement device with a different key state. */
		if (text->key_keyboard) return false;
		text->key_keyboard = keyboard;
		wl_signal_add(&keyboard->base.events.destroy, &text->key_keyboard_destroy);
	}
	text->queue[text->count++] = (struct text_item){.key = *event};
	return true;
}
static void handle_keyboard_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct mango_anland_text *text = wl_container_of(listener, text, keyboard_destroy);
	wl_list_remove(&text->keyboard_destroy.link);
	wl_list_init(&text->keyboard_destroy.link);
	text->restore_keyboard = NULL;
	text->keyboard.keymap_fallback = NULL;
}
static void handle_focus_change(struct wl_listener *listener, void *data) {
	(void)data;
	struct mango_anland_text *text = wl_container_of(listener, text, focus_change);
	/* Includes surface destruction; queued characters must never cross focus. */
	mango_anland_text_reset(text);
}
static void handle_grab_begin(struct wl_listener *listener, void *data) {
	(void)data;
	struct mango_anland_text *text = wl_container_of(listener, text, grab_begin);
	/* An IME or other grab must see the real keyboard, never the idle map. */
	mango_anland_text_reset(text);
}
static void handle_seat_destroy(struct wl_listener *listener, void *data) {
	(void)data;
	struct mango_anland_text *text = wl_container_of(listener, text, seat_destroy);
	mango_anland_text_reset(text);
	wl_list_remove(&text->focus_change.link);
	wl_list_remove(&text->seat_destroy.link);
	wl_list_remove(&text->grab_begin.link);
	text->seat = NULL;
	text->count = 0;
	wl_event_source_timer_update(text->timer, 0);
}
static int text_tick(void *data) {
	struct mango_anland_text *text = data;
	if (!text->count) {
		/* Do not push the full evdev map back to Xwayland after every committed
		 * character. Keep this keyboard on the same focus until a real key,
		 * focus change, grab, disconnect, or v3 delivery needs to take over. */
		text->holding = false;
		if (text->seat && wlr_seat_get_keyboard(text->seat) != &text->keyboard)
			restore_keyboard(text);
		return 0;
	}
	/* A normal key is a FIFO barrier: all preceding Unicode batches and
	 * their delivery hold finish first. Dequeue before notify: WM bindings
	 * can change focus and synchronously reset this queue. */
	if (text->queue[0].symbol == XKB_KEY_NoSymbol) {
		text->holding = false;
		restore_keyboard(text);
		while (text->count && text->queue[0].symbol == XKB_KEY_NoSymbol) {
			struct wlr_keyboard_key_event event = text->queue[0].key;
			--text->count;
			memmove(text->queue, text->queue + 1, text->count * sizeof(*text->queue));
			uint64_t serial = text->reset_serial;
			if (text->key_keyboard) wlr_keyboard_notify_key(text->key_keyboard, &event);
			if (serial != text->reset_serial) return 0;
		}
		/* Also hold the normal map before a following Unicode batch. */
		text->holding = true;
		wl_event_source_timer_update(text->timer, TEXT_KEYMAP_HOLD_MS);
		return 0;
	}
	if (!text->seat || !text->seat->keyboard_state.focused_surface ||
			!text->seat->keyboard_state.focused_client ||
			(text->ime_grabbed || wlr_seat_keyboard_has_grab(text->seat))) {
		mango_anland_text_reset(text); return 0;
	}
	/* A different real/virtual keyboard may have taken the seat between
	 * batches. Never restore over it or retain a stale restore target. */
	if (text->batch_active && wlr_seat_get_keyboard(text->seat) != &text->keyboard) {
		restore_keyboard(text);
	}
	size_t n = 0;
	xkb_keysym_t symbols[TEXT_BATCH_MAX];
	while (n < text->count && n < TEXT_BATCH_MAX && text->queue[n].symbol) {
		symbols[n] = text->queue[n].symbol;
		++n;
	}
	if (n > text->mapped_count ||
			memcmp(text->mapped, symbols, n * sizeof(*symbols)) != 0) {
		char *map = NULL;
		size_t map_size = 0;
		FILE *f = open_memstream(&map, &map_size);
		if (!f) { mango_anland_text_reset(text); return 0; }
		/* Distinct keycodes per character, including repeats. Zero modifiers:
		 * held Shift/Ctrl must not transform committed text. */
		fprintf(f, "xkb_keymap { xkb_keycodes { minimum=8; maximum=255; ");
		for (size_t i = 0; i < n; i++) fprintf(f, "<T%zu>=%zu; ", i, 200 + i);
		fprintf(f, "}; xkb_types { include \"complete\" }; xkb_compatibility { include \"complete\" }; xkb_symbols { ");
		for (size_t i = 0; i < n; i++) fprintf(f, "key <T%zu> { [ 0x%x ] }; ", i, symbols[i]);
		fprintf(f, "}; };");
		if (fclose(f) != 0) { free(map); mango_anland_text_reset(text); return 0; }
		struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
		struct xkb_keymap *keymap = context ? xkb_keymap_new_from_string(context,
			map, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS) : NULL;
		free(map);
		if (context) xkb_context_unref(context);
		if (!keymap || !wlr_keyboard_set_keymap(&text->keyboard, keymap)) {
			if (keymap) xkb_keymap_unref(keymap);
			wlr_log(WLR_ERROR, "Anland text: failed to create local Unicode keymap");
			mango_anland_text_reset(text); return 0;
		}
		xkb_keymap_unref(keymap);
		memcpy(text->mapped, symbols, n * sizeof(*symbols));
		text->mapped_count = n;
	}
	if (!text->batch_active) {
		text->restore_keyboard = wlr_seat_get_keyboard(text->seat);
		if (text->restore_keyboard)
			wl_signal_add(&text->restore_keyboard->base.events.destroy, &text->keyboard_destroy);
		text->keyboard.keymap_client = text->seat->keyboard_state.focused_client->client;
		text->keyboard.keymap_fallback = text->restore_keyboard;
		text->batch_active = true;
		wlr_seat_set_keyboard(text->seat, &text->keyboard);
	}
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	uint32_t time = (uint32_t)(ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL);
	for (size_t i = 0; i < n; i++) {
		wlr_seat_keyboard_notify_key(text->seat, time, 192 + i, WL_KEYBOARD_KEY_STATE_PRESSED);
		wlr_seat_keyboard_notify_key(text->seat, time, 192 + i, WL_KEYBOARD_KEY_STATE_RELEASED);
	}
	text->count -= n;
	memmove(text->queue, text->queue + n, text->count * sizeof(*text->queue));
	/* Only finish the delivery hold on this timer, not the keyboard lease.
	 * Otherwise spaced Android commits cause evdev -> Unicode -> evdev churn. */
	text->holding = true;
	wl_event_source_timer_update(text->timer, TEXT_KEYMAP_HOLD_MS);
	return 0;
}
struct mango_anland_text *mango_anland_text_create(struct wl_event_loop *loop,
		struct wlr_seat *seat) {
	if (!loop || !seat) return NULL;
	struct mango_anland_text *text = calloc(1, sizeof(*text));
	if (!text) return NULL;
	text->queue = calloc(TEXT_QUEUE_MAX, sizeof(*text->queue));
	if (!text->queue) { free(text); return NULL; }
	text->seat = seat;
	wlr_keyboard_init(&text->keyboard, &text_keyboard_impl, "Anland Unicode text");
	text->keyboard.keymap_client_only = true;
	wlr_keyboard_set_repeat_info(&text->keyboard, 0, 0);
	wl_list_init(&text->key_keyboard_destroy.link);
	text->key_keyboard_destroy.notify = handle_key_keyboard_destroy;
	wl_list_init(&text->keyboard_destroy.link);
	text->keyboard_destroy.notify = handle_keyboard_destroy;
	text->timer = wl_event_loop_add_timer(loop, text_tick, text);
	if (!text->timer) { wlr_keyboard_finish(&text->keyboard); free(text->queue); free(text); return NULL; }
	text->focus_change.notify = handle_focus_change;
	wl_signal_add(&seat->keyboard_state.events.focus_change, &text->focus_change);
	text->seat_destroy.notify = handle_seat_destroy;
	wl_signal_add(&seat->events.destroy, &text->seat_destroy);
	text->grab_begin.notify = handle_grab_begin;
	wl_signal_add(&seat->events.keyboard_grab_begin, &text->grab_begin);
	return text;
}
bool mango_anland_text_submit(struct mango_anland_text *text,
		struct mango_input_method_relay *relay, const char *utf8, size_t length) {
	if (!text || !text->seat || !utf8 || !length || length > TEXT_QUEUE_MAX ||
			!text->seat->keyboard_state.focused_surface ||
			(text->ime_grabbed || wlr_seat_keyboard_has_grab(text->seat))) return false;
	xkb_keysym_t *symbols = malloc(length * sizeof(*symbols));
	size_t count;
	if (!symbols) return false;
	if (!decode_utf8(utf8, length, symbols, &count)) { free(symbols); return false; }
	/* If v3 is available, its failure (e.g. oversized text) is NOT a reason
	 * to inject keys into the same editor. Only missing/disabled v3 falls back. */
	/* v3 may take over after the delivery hold, even if the idle Unicode
	 * keyboard still owns the seat. Never overtake pending fallback text. */
	/* Do not bypass v3's wire-size policy just because fallback is draining. */
	if (relay && relay->active_text_input && length > 4000) { free(symbols); return false; }
	if (relay && relay->active_text_input && !text->count && !text->holding) {
		restore_keyboard(text);
		free(symbols);
		return mango_text_input_commit_utf8(relay, utf8, length);
	}
	if (count > TEXT_QUEUE_MAX - text->count) { free(symbols); return false; }
	bool idle = !text->count && !text->holding;
	for (size_t i = 0; i < count; i++)
		text->queue[text->count + i] = (struct text_item){.symbol = symbols[i]};
	text->count += count;
	free(symbols);
	if (idle) wl_event_source_timer_update(text->timer, 1);
	return true;
}
void mango_anland_text_destroy(struct mango_anland_text *text) {
	if (!text) return;
	mango_anland_text_reset(text);
	if (text->seat) {
		wl_list_remove(&text->focus_change.link);
		wl_list_remove(&text->seat_destroy.link);
		wl_list_remove(&text->grab_begin.link);
	}
	wl_list_remove(&text->key_keyboard_destroy.link);
	wl_event_source_remove(text->timer);
	wlr_keyboard_finish(&text->keyboard);
	free(text->queue);
	free(text);
}