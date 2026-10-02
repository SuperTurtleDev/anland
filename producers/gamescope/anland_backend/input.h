/* Thin input shape adapter; protocol and fd reception stay in anland_device. */
#ifndef ANLAND_GAMESCOPE_INPUT_H
#define ANLAND_GAMESCOPE_INPUT_H
#include "anland_device.h"
#ifdef __cplusplus
extern "C" {
#endif
enum anland_gamescope_input_kind { AG_KEY, AG_MOTION, AG_BUTTON, AG_AXIS,
    AG_TOUCH_DOWN, AG_TOUCH_MOVE, AG_TOUCH_UP, AG_TOUCH_FRAME, AG_REFRESH, AG_RESOURCE_INVALID };
typedef struct anland_gamescope_input_event {
    enum anland_gamescope_input_kind kind;
    double x, y;
    double absolute_x, absolute_y;
    int32_t code, discrete;
    bool pressed;
} anland_gamescope_input_event;
typedef void (*anland_gamescope_input_sink)(void *, const anland_gamescope_input_event *);
typedef struct anland_gamescope_input {
    uint32_t pending_type, pending_size;
    void *payload;
    uint32_t pending_service;
    bool (*resources)(void *, uint32_t, int *, int);
    void *resources_userdata;
    void (*text)(void *, uint32_t, const char *, size_t);
    void *text_userdata;
} anland_gamescope_input;
void anland_gamescope_input_reset(anland_gamescope_input *input);
/* Returns emitted event count, or -1: caller must invalidate the session.
 * Pending variable data must finish before another header is polled. */
int anland_gamescope_input_pump(anland_gamescope_input *input, anland_device *device,
    uint32_t width, uint32_t height, anland_gamescope_input_sink sink, void *userdata);
#ifdef __cplusplus
}
#endif
#endif