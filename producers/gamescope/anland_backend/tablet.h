#ifndef ANLAND_GAMESCOPE_TABLET_H
#define ANLAND_GAMESCOPE_TABLET_H
#include "anland_tablet.h"
struct wl_display;
struct wlr_seat;
struct wlr_surface;
struct anland_gamescope_tablet;
#ifdef __cplusplus
extern "C" {
#endif
/* All calls are serialized by the existing Gamescope Wayland seat lock. */
struct anland_gamescope_tablet *anland_gamescope_tablet_create(struct wl_display *, struct wlr_seat *);
bool anland_gamescope_tablet_frame(struct anland_gamescope_tablet *,
    const anland_tablet_sample *, struct wlr_surface *, double x, double y);
void anland_gamescope_tablet_reset(struct anland_gamescope_tablet *);
void anland_gamescope_tablet_destroy(struct anland_gamescope_tablet *);
#ifdef __cplusplus
}
#endif
#endif
