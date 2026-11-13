#ifndef AWL_MODIFIER_STATE_H
#define AWL_MODIFIER_STATE_H
#include <stdint.h>

/* Android motion metadata can omit modifiers held on another input device.
 * Keep those backed by actual evdev key-down events. Cached metadata alone
 * is insufficient evidence of a genuinely held modifier. */
static inline uint32_t awl_held_modifiers(const uint8_t keys[32]) {
#define HELD(k) ((keys[(k) / 8] >> ((k) % 8)) & 1u)
    uint32_t mask = 0;
    if (HELD(42) || HELD(54)) mask |= 1u << 0;    /* Shift */
    if (HELD(29) || HELD(97)) mask |= 1u << 2;    /* Control */
    if (HELD(56) || HELD(100)) mask |= 1u << 3;   /* Alt */
    if (HELD(125) || HELD(126)) mask |= 1u << 6;  /* Logo */
#undef HELD
    return mask;
}
#endif
