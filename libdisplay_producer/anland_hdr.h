/* SPDX-License-Identifier: MIT */
#ifndef ANLAND_HDR_H
#define ANLAND_HDR_H

#include <ctype.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#define ANLAND_HDR_SDR_WHITE_NITS 203.0f
#define ANLAND_HDR_PEAK_NITS 1000.0f
#define ANLAND_HDR_PQ_SCALE_NITS 10000.0f

enum anland_hdr_mode {
    ANLAND_HDR_REJECTED = -1,
    ANLAND_HDR_SDR = 0,
    ANLAND_HDR_HDR10 = 1,
};

/* Producer-side policy only. No wire structure or protocol version changes. */
static inline bool anland_hdr_environment_disabled(const char *value)
{
    if (!value)
        return false;
    while (isspace((unsigned char)*value))
        ++value;
    size_t length = strlen(value);
    while (length && isspace((unsigned char)value[length - 1]))
        --length;
    static const char *const disabled[] = {"0", "false", "no", "off"};
    for (size_t i = 0; i < sizeof(disabled) / sizeof(disabled[0]); ++i) {
        if (strlen(disabled[i]) != length)
            continue;
        size_t j = 0;
        while (j < length && tolower((unsigned char)value[j]) == disabled[i][j])
            ++j;
        if (j == length)
            return true;
    }
    return false;
}

static inline enum anland_hdr_mode anland_hdr_mode_for_hal(uint32_t format, bool allow_hdr)
{
    /* In Anland's Android consumer HAL 43 is requested only with BT.2020/PQ.
     * This local contract does not make arbitrary 10-bit buffers HDR. */
    switch (format) {
    case 1:
    case 2:
    case 5:
        return ANLAND_HDR_SDR;
    case 43:
        return allow_hdr ? ANLAND_HDR_HDR10 : ANLAND_HDR_REJECTED;
    default:
        return ANLAND_HDR_REJECTED;
    }
}

#endif
