/*
 * anland_present_protocol.h -- DE <-> AWL presentation IPC contract.
 *
 * This is deliberately separate from both anland_device.h and awl.h. The DE
 * side and the Android/WaylandBridge side are different processes, so an AWL
 * presentation backend cannot call awl_server_start() directly. Messages are
 * framed with a fixed header and may carry file descriptors through SCM_RIGHTS.
 *
 * All integer fields are little-endian, matching the Android arm64 deployment
 * and the existing display transport. The version and message size are checked
 * before any payload is used.
 */
#ifndef ANLAND_PRESENT_PROTOCOL_H
#define ANLAND_PRESENT_PROTOCOL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ANLAND_PRESENT_PROTOCOL_MAGIC 0x41575052u /* "AWPR" */
#define ANLAND_PRESENT_PROTOCOL_VERSION 1u
#define ANLAND_PRESENT_MAX_WINDOWS 64u
#define ANLAND_PRESENT_MAX_DAMAGE 8u
#define ANLAND_PRESENT_MAX_FDS 4u

/* Message direction is documented by the names, not encoded in the header. */
typedef enum anland_present_msg_type {
    ANLAND_PRESENT_HELLO = 1,          /* DE -> AWL */
    ANLAND_PRESENT_HELLO_REPLY = 2,    /* AWL -> DE */
    ANLAND_PRESENT_WINDOW_CREATE = 3,  /* AWL -> DE */
    ANLAND_PRESENT_WINDOW_DESTROY = 4, /* AWL -> DE */
    ANLAND_PRESENT_WINDOW_RESIZE = 5,  /* AWL -> DE */
    ANLAND_PRESENT_BUFFER_SUBMIT = 6,  /* AWL -> DE */
    ANLAND_PRESENT_FRAME_PRESENTED = 7,/* DE -> AWL */
    ANLAND_PRESENT_BUFFER_RELEASE = 8, /* DE -> AWL */
    ANLAND_PRESENT_REOPEN = 9,         /* DE -> AWL */
    ANLAND_PRESENT_ERROR = 10,         /* either direction */
    ANLAND_PRESENT_WINDOW_DIRTY = 11,  /* AWL -> DE; buffer export follows later */
    ANLAND_PRESENT_WINDOW_CONFIGURE = 12, /* DE -> AWL */
} anland_present_msg_type_t;

typedef struct anland_present_msg_header {
    uint32_t magic;
    uint16_t version;
    uint16_t type;
    uint32_t size;       /* header + payload, excluding passed fds */
    uint64_t generation; /* changes whenever the presentation session changes */
    uint64_t serial;     /* monotonically increasing message/frame serial */
} anland_present_msg_header_t;

typedef struct anland_present_hello {
    uint32_t flags;
    uint32_t max_windows;
    uint32_t max_damage;
    uint32_t reserved;
} anland_present_hello_t;

typedef struct anland_present_hello_reply {
    int32_t status;
    uint32_t flags;
    uint32_t max_windows;
    uint32_t max_damage;
} anland_present_hello_reply_t;

typedef struct anland_present_window_create {
    uint64_t window_id;
    int32_t width;
    int32_t height;
    uint32_t kind;       /* normal, popup, overlay */
    uint32_t flags;
} anland_present_window_create_t;

typedef struct anland_present_window_destroy {
    uint64_t window_id;
} anland_present_window_destroy_t;

typedef struct anland_present_window_resize {
    uint64_t window_id;
    int32_t width;
    int32_t height;
    uint32_t scale;
    int32_t transform;
} anland_present_window_resize_t;

typedef struct anland_present_rect {
    int32_t x;
    int32_t y;
    int32_t width;
    int32_t height;
} anland_present_rect_t;

typedef struct anland_present_buffer_submit {
    uint64_t window_id;
    uint64_t buffer_id;
    uint64_t frame_id;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
    uint64_t modifier;
    uint32_t offset;
    uint32_t damage_count;
    anland_present_rect_t damage[ANLAND_PRESENT_MAX_DAMAGE];
    uint32_t flags;      /* bit 0 = full damage, bit 1 = has acquire fence */
    uint32_t reserved;
} anland_present_buffer_submit_t;

typedef struct anland_present_frame_presented {
    uint64_t window_id;
    uint64_t frame_id;
    uint64_t presentation_ns;
} anland_present_frame_presented_t;

typedef struct anland_present_buffer_release {
    uint64_t window_id;
    uint64_t buffer_id;
    uint64_t frame_id;
    uint32_t flags;      /* bit 0 = has release fence */
    uint32_t reserved;
} anland_present_buffer_release_t;

typedef struct anland_present_reopen {
    uint64_t new_generation;
} anland_present_reopen_t;

typedef struct anland_present_error {
    int32_t status;
    uint32_t failed_type;
    uint64_t object_id;
} anland_present_error_t;

static inline int anland_present_protocol_header_valid(
    const anland_present_msg_header_t *header, uint32_t received_size)
{
    return header && header->magic == ANLAND_PRESENT_PROTOCOL_MAGIC &&
           header->version == ANLAND_PRESENT_PROTOCOL_VERSION &&
           header->size >= sizeof(*header) && header->size <= received_size;
}

#ifdef __cplusplus
}
#endif

#endif /* ANLAND_PRESENT_PROTOCOL_H */