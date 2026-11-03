/* Exercise the real input pump with a clipboard payload split across reads. */
#include "input.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct anland_device {
    const anland_device_input_t *events;
    size_t count, next;
    bool wait_payload;
};

int anland_device_poll_input(anland_device *device, anland_device_input_t *event,
                             int timeout_ms)
{
    (void)timeout_ms;
    if (device->next == device->count) return 0;
    *event = device->events[device->next++];
    return 1;
}

int anland_device_read_input(anland_device *device, void *data, size_t size,
                             int timeout_ms)
{
    (void)timeout_ms;
    if (device->wait_payload) {
        device->wait_payload = false;
        return 0;
    }
    assert(size == 3);
    memcpy(data, "abc", size);
    return 1;
}

int anland_device_read_fds(anland_device *device, int *fds, int max_fds,
                           int *count, int timeout_ms)
{
    (void)device; (void)fds; (void)max_fds; (void)count; (void)timeout_ms;
    assert(!"unexpected fd payload");
    return -1;
}

struct probe {
    unsigned frames, texts;
    anland_tablet_sample last;
};

static void input(void *data, const anland_gamescope_input_event *event)
{
    struct probe *probe = data;
    assert(event->kind == AG_TABLET);
    probe->last = event->tablet;
    ++probe->frames;
}

static void text(void *data, uint32_t type, const char *bytes, size_t size)
{
    struct probe *probe = data;
    assert(type == ANLAND_DEVICE_IN_CLIPBOARD && size == 3);
    assert(!memcmp(bytes, "abc", size));
    ++probe->texts;
}

int main(void)
{
    const uint32_t flags = TABLET_FLAG_PROXIMITY | TABLET_FLAG_TIP_DOWN;
    const anland_device_input_t events[] = {
        {.type = ANLAND_DEVICE_IN_TABLET_TOOL,
         .tablet_tool = {1, TABLET_TOOL_PEN, TABLET_CAP_PRESSURE | TABLET_CAP_TILT, 42}},
        {.type = ANLAND_DEVICE_IN_TABLET_POSITION, .tablet_position = {1, 10, 20, .4f}},
        {.type = ANLAND_DEVICE_IN_TABLET_FRAME, .tablet_frame = {1, flags, TABLET_BUTTON_PRIMARY, 100}},
        {.type = ANLAND_DEVICE_IN_TABLET_POSITION, .tablet_position = {1, 30, 40, .7f}},
        {.type = ANLAND_DEVICE_IN_CLIPBOARD, .clipboard = {3}},
        {.type = ANLAND_DEVICE_IN_TABLET_TILT, .tablet_tilt = {1, -15, 25, 0}},
        {.type = ANLAND_DEVICE_IN_TABLET_FRAME, .tablet_frame = {1, flags, TABLET_BUTTON_PRIMARY, 101}},
        /* Metadata must remain immutable after receiving a clipboard payload. */
        {.type = ANLAND_DEVICE_IN_TABLET_TOOL,
         .tablet_tool = {1, TABLET_TOOL_ERASER, TABLET_CAP_PRESSURE, 42}},
    };
    struct anland_device device = {events, 7, 0, true};
    struct probe probe = {0};
    anland_gamescope_input state = {.text = text, .text_userdata = &probe};
    assert(anland_gamescope_input_pump(&state, &device, 100, 100, input, &probe) == 1);
    assert(probe.frames == 1 && probe.texts == 0);
    assert(probe.last.x == 10 && probe.last.flags == flags);
    assert(state.pending_type == ANLAND_DEVICE_IN_CLIPBOARD);
    assert(anland_gamescope_input_pump(&state, &device, 100, 100, input, &probe) == 1);
    assert(probe.frames == 2 && probe.texts == 1);
    assert(probe.last.tool_id == 1 && probe.last.device_id == 42);
    assert(probe.last.x == 30 && probe.last.y == 40 && probe.last.pressure == .7f);
    assert(probe.last.tilt_x == -15 && probe.last.tilt_y == 25);
    assert(probe.last.flags == flags && probe.last.buttons == TABLET_BUTTON_PRIMARY);
    assert(!state.payload && !state.pending_type);
    device.count = sizeof(events) / sizeof(events[0]);
    assert(anland_gamescope_input_pump(&state, &device, 100, 100, input, &probe) == -1);
    anland_gamescope_input_reset(&state);
    assert(!state.tablet_state.tools[0].occupied);
    puts("tablet state survives a split clipboard payload: passed");
}
