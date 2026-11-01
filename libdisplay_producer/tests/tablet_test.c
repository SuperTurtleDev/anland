#include "anland_tablet.h"
#include <assert.h>
#include <stddef.h>
#include <stdio.h>

_Static_assert(sizeof(struct InputEvent) == 20, "wire ABI changed");
_Static_assert(sizeof(anland_device_input_t) == 20, "device ABI changed");
_Static_assert(offsetof(struct InputEvent, tablet_tilt.distance) ==
               offsetof(anland_device_input_t, tablet_tilt.distance), "axis layout");

static anland_device_input_t tool(uint32_t id, uint32_t type) {
    anland_device_input_t e = {.type = ANLAND_DEVICE_IN_TABLET_TOOL};
    e.tablet_tool.tool_id = id;
    e.tablet_tool.tool_type = type;
    e.tablet_tool.device_id = 42;
    e.tablet_tool.capabilities = TABLET_CAP_PRESSURE | TABLET_CAP_TILT | TABLET_CAP_DISTANCE;
    return e;
}
static anland_device_input_t position(uint32_t id, float x, float pressure) {
    anland_device_input_t e = {.type = ANLAND_DEVICE_IN_TABLET_POSITION};
    e.tablet_position.tool_id = id;
    e.tablet_position.x = x;
    e.tablet_position.y = 500;
    e.tablet_position.pressure = pressure;
    return e;
}
static anland_device_input_t frame(uint32_t id, uint32_t flags, uint32_t buttons) {
    anland_device_input_t e = {.type = ANLAND_DEVICE_IN_TABLET_FRAME};
    e.tablet_frame.tool_id = id;
    e.tablet_frame.flags = flags;
    e.tablet_frame.buttons = buttons;
    e.tablet_frame.time_msec = 1234;
    return e;
}
int main(void) {
    anland_tablet tablet = {0};
    anland_tablet_sample prev = {0}, sample = {0};
    anland_device_input_t e = tool(1, TABLET_TOOL_PEN);
    assert(anland_tablet_feed(&tablet, &e, &prev, &sample) == 0);
    e = frame(1, TABLET_FLAG_PROXIMITY, 0);
    assert(anland_tablet_feed(&tablet, &e, &prev, &sample) == -1);
    e = position(1, 320, .7f);
    assert(anland_tablet_feed(&tablet, &e, &prev, &sample) == 0);
    assert(tablet.tools[0].current.pressure == 0); /* no partial sample */
    e = tool(2, TABLET_TOOL_ERASER);
    assert(anland_tablet_feed(&tablet, &e, &prev, &sample) == 0);
    e = position(2, 640, .2f);
    assert(anland_tablet_feed(&tablet, &e, &prev, &sample) == 0);
    e = frame(1, TABLET_FLAG_PROXIMITY | TABLET_FLAG_TIP_DOWN, TABLET_BUTTON_PRIMARY);
    assert(anland_tablet_feed(&tablet, &e, &prev, &sample) == 1);
    assert(sample.x == 320 && sample.pressure == .7f && sample.buttons == 1);
    assert(prev.flags == 0 && sample.time_msec == 1234);
    e = frame(2, TABLET_FLAG_PROXIMITY, TABLET_BUTTON_SECONDARY);
    assert(anland_tablet_feed(&tablet, &e, &prev, &sample) == 1);
    assert(sample.tool_type == TABLET_TOOL_ERASER && sample.x == 640);
    e = position(1, NAN, .2f);
    assert(anland_tablet_feed(&tablet, &e, &prev, &sample) == -1);
    e = position(1, 0, 1.1f);
    assert(anland_tablet_feed(&tablet, &e, &prev, &sample) == -1);
    e = tool(1, TABLET_TOOL_ERASER);
    assert(anland_tablet_feed(&tablet, &e, &prev, &sample) == -1);
    e = frame(1, TABLET_FLAG_CANCELED, TABLET_BUTTON_PRIMARY);
    assert(anland_tablet_feed(&tablet, &e, &prev, &sample) == 1);
    assert((prev.flags & TABLET_FLAG_TIP_DOWN) && prev.buttons == 1);
    assert(sample.flags == TABLET_FLAG_CANCELED && !sample.buttons && !sample.pressure);
    unsigned cursor = 0;
    assert(anland_tablet_release_next(&tablet, &cursor, &prev, &sample) == 1);
    assert(prev.tool_id == 2 && prev.buttons == TABLET_BUTTON_SECONDARY);
    assert(!sample.buttons && !(sample.flags & TABLET_FLAG_PROXIMITY));
    assert(anland_tablet_release_next(&tablet, &cursor, &prev, &sample) == 0);
    assert(!tablet.tools[0].occupied && !tablet.tools[1].occupied);
    puts("tablet framing, identity, interleaving, validation and disconnect: passed");
}
