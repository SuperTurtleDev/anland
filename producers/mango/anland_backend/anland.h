#ifndef MANGO_BACKEND_ANLAND_H
#define MANGO_BACKEND_ANLAND_H

#include <stdbool.h>
#include <stddef.h>

struct wl_event_loop;
struct wlr_backend;
struct wlr_output;
struct wlr_swapchain;
struct wlr_touch;
struct wlr_tablet;

#ifdef HAVE_ANLAND
struct wlr_backend *mango_anland_backend_create(struct wl_event_loop *loop,
	const char *socket_path);
bool mango_anland_output_is(struct wlr_output *output);
struct wlr_swapchain *mango_anland_output_swapchain(struct wlr_output *output);
bool mango_anland_touch_is(struct wlr_touch *touch);
bool mango_anland_tablet_is(struct wlr_tablet *tablet);
void mango_anland_set_ime_grab(struct wlr_backend *backend, bool grabbed);
#else
static inline bool mango_anland_tablet_is(struct wlr_tablet *tablet) {
    (void)tablet;
    return false;
}
static inline void mango_anland_set_ime_grab(struct wlr_backend *backend, bool grabbed) {
	(void)backend; (void)grabbed;
}
static inline bool mango_anland_output_is(struct wlr_output *output) {
	(void)output;
	return false;
}
static inline struct wlr_swapchain *mango_anland_output_swapchain(
		struct wlr_output *output) {
	(void)output;
	return NULL;
}
static inline bool mango_anland_touch_is(struct wlr_touch *touch) {
	(void)touch;
	return false;
}
#endif

#endif
