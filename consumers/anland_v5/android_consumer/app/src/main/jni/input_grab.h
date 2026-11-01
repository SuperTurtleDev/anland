/*
 * Wire format between the root input-grab helper (input_grab.c, shipped as
 * libinputgrab.so) and the app's InputGrab.java.
 *
 * One fixed-size 32-byte record for everything, little-endian, no padding
 * surprises: every field is naturally aligned inside the struct, so the Java
 * side can parse it with a plain ByteBuffer. Fixed framing matters because the
 * helper drops records under back-pressure instead of blocking -- a variable
 * length would desync the stream the moment one is dropped.
 *
 * Keep IGRAB_* in sync with the constants at the top of InputGrab.java.
 */
#ifndef ANLAND_INPUT_GRAB_H
#define ANLAND_INPUT_GRAB_H

#include <stdint.h>

#define IGRAB_PROTO_VERSION 3

/* rtype */
#define IGRAB_REC_HELLO   1   /* value=version, aux[0]=grabbed count, aux[1]=pid */
#define IGRAB_REC_DEVICE  2   /* opened device, also on hotplug/state change */
#define IGRAB_REC_READY    3  /* initial setup settled; aux[0]=controlled count */
#define IGRAB_REC_EVENT   4   /* etype/code/value = evdev type/code/value */
#define IGRAB_REC_BYE     5   /* value = IGRAB_BYE_*; helper is exiting */
#define IGRAB_REC_AXIS_RANGE 6 /* code=EV_ABS axis, value=min, aux[0]=max */

/* DEVICE.etype -- what the app should do with this device's events. */
#define IGRAB_CLASS_TOUCHSCREEN 1
#define IGRAB_CLASS_TOUCHPAD    2
#define IGRAB_CLASS_MOUSE       3
#define IGRAB_CLASS_KEYBOARD    4
#define IGRAB_CLASS_GAMEPAD     5

/* EVENT.aux: evdev sample time in microseconds, monotonic for mice. */
#define IGRAB_EVENT_TIME_LO 0
#define IGRAB_EVENT_TIME_HI 1

/* DEVICE.aux layout */
#define IGRAB_AUX_MIN_X 0
#define IGRAB_AUX_MAX_X 1
#define IGRAB_AUX_MIN_Y 2
#define IGRAB_AUX_MAX_Y 3
#define IGRAB_AUX_FLAGS 4

/* DEVICE.aux[IGRAB_AUX_FLAGS] bits */
#define IGRAB_DEV_GRABBED    (1 << 0)  /* exclusive evdev grab (non-controllers) */
#define IGRAB_DEV_MULTITOUCH (1 << 1)  /* ABS_MT_POSITION_X/Y (protocol B) present */
#define IGRAB_DEV_CLICKPAD   (1 << 2)  /* one physical button under the whole pad */
#define IGRAB_DEV_PASSTHROUGH (1 << 3) /* Android reader suppressed; desktop evdev direct */

/* BYE.value */
#define IGRAB_BYE_TOGGLE    1  /* the toggle key was pressed */
#define IGRAB_BYE_PEER_GONE 2  /* app closed the socket */
#define IGRAB_BYE_STALLED   3  /* app stopped heartbeating / would not read */
#define IGRAB_BYE_ERROR     4  /* nothing grabbable, or a fatal device error */
/* 5..7 are Java-local reasons; keep controller failure distinct from them. */
#define IGRAB_BYE_CONTROLLER 8 /* selected controller could not acquire/retain suppression */

/* Broadcast device id, used by the resync marker after dropped records. */
#define IGRAB_DEV_ALL 0xFFFF

#define IGRAB_MAX_DEVICES 32
#define IGRAB_REC_SIZE    32

struct igrab_rec {
    uint16_t rtype;
    uint16_t dev;
    uint16_t etype;
    uint16_t code;
    int32_t  value;
    int32_t  aux[5];
} __attribute__((packed));

#endif /* ANLAND_INPUT_GRAB_H */
