/* Gamescope has no upstream tablet input route. Register a native tablet-v2
 * device on its existing seat and target its selected game surface. */
#include "tablet.h"
#include <stdlib.h>
#include <wlr/interfaces/wlr_tablet_tool.h>
#include <wlr/types/wlr_tablet_v2.h>

static const struct wlr_tablet_impl impl = {.name = "anland-tablet"};
struct native_tool {
    uint32_t id;
    struct wlr_tablet_tool tool;
    struct wlr_tablet_v2_tablet_tool *v2;
};
struct anland_gamescope_tablet {
    struct wlr_tablet tablet;
    struct wlr_tablet_manager_v2 *manager; /* display-owned */
    struct wlr_tablet_v2_tablet *v2;
    struct wlr_seat *seat;
    struct native_tool tools[ANLAND_TABLET_MAX_TOOLS];
};
static void release_tool(struct native_tool *tool) {
    if (!tool->v2) return;
    if (tool->v2->is_down) wlr_tablet_v2_tablet_tool_notify_up(tool->v2);
    while (tool->v2->num_buttons)
        wlr_tablet_v2_tablet_tool_notify_button(tool->v2,
            tool->v2->pressed_buttons[tool->v2->num_buttons - 1], ZWP_TABLET_PAD_V2_BUTTON_STATE_RELEASED);
    wlr_tablet_v2_tablet_tool_notify_proximity_out(tool->v2);
}
struct anland_gamescope_tablet *anland_gamescope_tablet_create(struct wl_display *display, struct wlr_seat *seat) {
    struct anland_gamescope_tablet *tablet = calloc(1, sizeof(*tablet));
    if (!tablet) return NULL;
    tablet->seat = seat;
    tablet->manager = wlr_tablet_v2_create(display);
    if (!tablet->manager) { free(tablet); return NULL; }
    wlr_tablet_init(&tablet->tablet, &impl, "Anland tablet");
    tablet->v2 = wlr_tablet_create(tablet->manager, seat, &tablet->tablet.base);
    if (!tablet->v2) { wlr_tablet_finish(&tablet->tablet); free(tablet); return NULL; }
    return tablet;
}
bool anland_gamescope_tablet_frame(struct anland_gamescope_tablet *tablet,
        const anland_tablet_sample *sample, struct wlr_surface *surface, double x, double y) {
    struct native_tool *tool = NULL;
    for (unsigned i=0; i<ANLAND_TABLET_MAX_TOOLS; ++i)
        if (tablet->tools[i].id == sample->tool_id) { tool=&tablet->tools[i]; break; }
    if (!tool) for (unsigned i=0; i<ANLAND_TABLET_MAX_TOOLS; ++i) if (!tablet->tools[i].id) {
        tool = &tablet->tools[i];
        tool->id = sample->tool_id;
        tool->tool.type = sample->tool_type == TABLET_TOOL_ERASER ? WLR_TABLET_TOOL_TYPE_ERASER : WLR_TABLET_TOOL_TYPE_PEN;
        tool->tool.hardware_serial = ((uint64_t)sample->device_id << 32) | sample->tool_id;
        tool->tool.pressure = sample->capabilities & TABLET_CAP_PRESSURE;
        tool->tool.tilt = sample->capabilities & TABLET_CAP_TILT;
        tool->tool.distance = sample->capabilities & TABLET_CAP_DISTANCE;
        wl_signal_init(&tool->tool.events.destroy);
        tool->v2 = wlr_tablet_tool_create(tablet->manager, tablet->seat, &tool->tool);
        if (!tool->v2) { tool->id=0; return false; }
        break;
    }
    if (!tool) return false;
    if (!(sample->flags & TABLET_FLAG_PROXIMITY) || !surface ||
            !wlr_surface_accepts_tablet_v2(surface, tablet->v2)) {
        release_tool(tool);
        return true;
    }
    /* A game/focus change releases the old target before entering the new one.
     * wlr_tablet_v2 tracks surface destruction, so no borrowed surface survives. */
    if (tool->v2->focused_surface != surface) {
        release_tool(tool);
        wlr_tablet_v2_tablet_tool_notify_proximity_in(tool->v2, tablet->v2, surface);
    }
    wlr_tablet_v2_tablet_tool_notify_motion(tool->v2, x, y);
    if (tool->tool.pressure) wlr_tablet_v2_tablet_tool_notify_pressure(tool->v2, sample->pressure);
    if (tool->tool.tilt) wlr_tablet_v2_tablet_tool_notify_tilt(tool->v2, sample->tilt_x, sample->tilt_y);
    if (tool->tool.distance) wlr_tablet_v2_tablet_tool_notify_distance(tool->v2, sample->distance);
    bool down = sample->flags & TABLET_FLAG_TIP_DOWN;
    if (down != tool->v2->is_down) {
        if (down) wlr_tablet_v2_tablet_tool_notify_down(tool->v2);
        else wlr_tablet_v2_tablet_tool_notify_up(tool->v2);
    }
    for (unsigned i=0;i<2;++i) {
        uint32_t button = 0x14bu+i;
        bool was_down = false;
        for (size_t j=0;j<tool->v2->num_buttons;++j)
            if (tool->v2->pressed_buttons[j] == button) was_down = true;
        bool pressed = sample->buttons & (1u<<i);
        if (pressed != was_down) wlr_tablet_v2_tablet_tool_notify_button(tool->v2, button,
            pressed ? ZWP_TABLET_PAD_V2_BUTTON_STATE_PRESSED : ZWP_TABLET_PAD_V2_BUTTON_STATE_RELEASED);
    }
    return true;
}
void anland_gamescope_tablet_reset(struct anland_gamescope_tablet *tablet) {
    if (!tablet) return;
    for (unsigned i=0;i<ANLAND_TABLET_MAX_TOOLS;++i) if (tablet->tools[i].id) {
        release_tool(&tablet->tools[i]);
        wl_signal_emit_mutable(&tablet->tools[i].tool.events.destroy, &tablet->tools[i].tool);
        memset(&tablet->tools[i], 0, sizeof(tablet->tools[i]));
    }
}
void anland_gamescope_tablet_destroy(struct anland_gamescope_tablet *tablet) {
    if (!tablet) return;
    anland_gamescope_tablet_reset(tablet);
    wlr_tablet_finish(&tablet->tablet);
    free(tablet);
}
