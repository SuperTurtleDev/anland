/* Native pen sample assembler shared by compositor adapters. Header-only so
 * backends can stage fixed-size wire records without another library ABI. */
#ifndef ANLAND_TABLET_H
#define ANLAND_TABLET_H
#include "anland_device.h"
#include "protocol.h"
#include <math.h>
#include <string.h>

#define ANLAND_TABLET_MAX_TOOLS 16

typedef struct anland_tablet_sample {
    uint32_t tool_id, tool_type, capabilities, device_id;
    uint32_t flags, buttons, time_msec;
    float x, y, pressure, tilt_x, tilt_y, distance;
} anland_tablet_sample;

typedef struct anland_tablet_slot {
    bool occupied, has_position;
    anland_tablet_sample pending, current;
} anland_tablet_slot;

typedef struct anland_tablet {
    anland_tablet_slot tools[ANLAND_TABLET_MAX_TOOLS];
} anland_tablet;

static inline bool anland_tablet_is_event(uint32_t type)
{
    return type >= ANLAND_DEVICE_IN_TABLET_TOOL && type <= ANLAND_DEVICE_IN_TABLET_FRAME;
}

/* Returns 1 for a complete frame, 0 for a staged/non-tablet record, -1 for a
 * malformed sample. Callers must invalidate/reset the session on -1, releasing
 * tools before discarding this assembler. No axes are published before FRAME. */
static inline int anland_tablet_feed(anland_tablet *tablet,
                                    const anland_device_input_t *event,
                                    anland_tablet_sample *previous,
                                    anland_tablet_sample *sample)
{
    anland_tablet_slot *slot = NULL, *vacant = NULL;
    uint32_t id;
    unsigned i;
    if (!anland_tablet_is_event(event->type)) return 0;
    /* Every tablet record starts with its tool_id; copy avoids union aliasing. */
    memcpy(&id, &event->tablet_tool.tool_id, sizeof(id));
    if (!id) return -1;
    for (i = 0; i < ANLAND_TABLET_MAX_TOOLS; ++i) {
        if (tablet->tools[i].occupied && tablet->tools[i].current.tool_id == id)
            slot = &tablet->tools[i];
        if (!tablet->tools[i].occupied && !vacant) vacant = &tablet->tools[i];
    }
    if (event->type == ANLAND_DEVICE_IN_TABLET_TOOL) {
        const uint32_t type = event->tablet_tool.tool_type;
        const uint32_t caps = event->tablet_tool.capabilities;
        if ((type != TABLET_TOOL_PEN && type != TABLET_TOOL_ERASER) ||
            (caps & ~(TABLET_CAP_PRESSURE | TABLET_CAP_TILT | TABLET_CAP_DISTANCE))) return -1;
        if (!slot) {
            slot = vacant;
            /* Tool descriptors remain stable until session reset. Exhaustion
             * forces a clean reconnect, never silent reuse of a live native
             * object (whose clients may retain its identity after proximity). */
            if (!slot) return -1;
            memset(slot, 0, sizeof(*slot));
            slot->occupied = true;
            slot->current.tool_id = id;
            slot->current.tool_type = type;
            slot->current.capabilities = caps;
            slot->current.device_id = event->tablet_tool.device_id;
            slot->pending = slot->current;
        } else if (slot->current.tool_type != type || slot->current.capabilities != caps ||
                   slot->current.device_id != event->tablet_tool.device_id) {
            return -1; /* tool identity is stable for the life of a session */
        }
        return 0;
    }
    if (!slot) return -1;
    if (event->type == ANLAND_DEVICE_IN_TABLET_POSITION) {
        if (!isfinite(event->tablet_position.x) || !isfinite(event->tablet_position.y) ||
            !isfinite(event->tablet_position.pressure) || event->tablet_position.pressure < 0 ||
            event->tablet_position.pressure > 1) return -1;
        slot->pending.x = event->tablet_position.x;
        slot->pending.y = event->tablet_position.y;
        slot->pending.pressure = (slot->current.capabilities & TABLET_CAP_PRESSURE)
            ? event->tablet_position.pressure : 0;
        slot->has_position = true;
        return 0;
    }
    if (event->type == ANLAND_DEVICE_IN_TABLET_TILT) {
        if (!isfinite(event->tablet_tilt.tilt_x) || !isfinite(event->tablet_tilt.tilt_y) ||
            !isfinite(event->tablet_tilt.distance) || fabsf(event->tablet_tilt.tilt_x) > 90 ||
            fabsf(event->tablet_tilt.tilt_y) > 90 || event->tablet_tilt.distance < 0 ||
            event->tablet_tilt.distance > 1) return -1;
        if (slot->current.capabilities & TABLET_CAP_TILT) {
            slot->pending.tilt_x = event->tablet_tilt.tilt_x;
            slot->pending.tilt_y = event->tablet_tilt.tilt_y;
        }
        if (slot->current.capabilities & TABLET_CAP_DISTANCE)
            slot->pending.distance = event->tablet_tilt.distance;
        return 0;
    }
    if ((event->tablet_frame.flags & ~(TABLET_FLAG_PROXIMITY | TABLET_FLAG_TIP_DOWN | TABLET_FLAG_CANCELED)) ||
        (event->tablet_frame.buttons & ~(TABLET_BUTTON_PRIMARY | TABLET_BUTTON_SECONDARY))) return -1;
    *previous = slot->current;
    *sample = slot->pending;
    sample->flags = event->tablet_frame.flags;
    sample->buttons = event->tablet_frame.buttons;
    sample->time_msec = event->tablet_frame.time_msec;
    if (sample->flags & TABLET_FLAG_CANCELED) {
        sample->flags = TABLET_FLAG_CANCELED;
        sample->buttons = 0;
    }
    if (!(sample->flags & TABLET_FLAG_PROXIMITY)) {
        if (sample->flags & TABLET_FLAG_TIP_DOWN) return -1;
        sample->buttons = 0;
        sample->pressure = 0;
    } else if (!slot->has_position) return -1;
    slot->current = slot->pending = *sample;
    return 1;
}

/* Enumerate synthetic release frames on disconnect/teardown, including only
 * active tools. Start cursor at zero; call until 0, then the assembler is empty.
 * Backend owns device lifetime and must emit tip-up/buttons-up before out. */
static inline int anland_tablet_release_next(anland_tablet *tablet, unsigned *cursor,
                                            anland_tablet_sample *previous,
                                            anland_tablet_sample *sample)
{
    while (*cursor < ANLAND_TABLET_MAX_TOOLS) {
        anland_tablet_slot *slot = &tablet->tools[(*cursor)++];
        bool active = slot->occupied && (slot->current.flags & TABLET_FLAG_PROXIMITY);
        *previous = slot->current;
        *sample = slot->current;
        memset(slot, 0, sizeof(*slot));
        if (active) {
            sample->flags = TABLET_FLAG_CANCELED;
            sample->buttons = 0;
            sample->pressure = 0;
            return 1;
        }
    }
    return 0;
}
#endif
