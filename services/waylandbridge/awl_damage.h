#ifndef AWL_DAMAGE_H
#define AWL_DAMAGE_H

#include <stdint.h>

struct awl_damage_box {
    int64_t x1, y1, x2, y2;
};

struct awl_damage_rect {
    int32_t x, y, w, h;
};

struct awl_damage_map {
    uint32_t buffer_width;
    uint32_t buffer_height;
    int32_t buffer_scale;
    int32_t buffer_transform;
    int32_t viewport_dst_width;   /* 0 = unset */
    int32_t viewport_dst_height;  /* 0 = unset */
    int viewport_has_source;
    double viewport_src_x;
    double viewport_src_y;
    double viewport_src_width;
    double viewport_src_height;
};

enum awl_damage_result {
    AWL_DAMAGE_EMPTY = 0,
    AWL_DAMAGE_RECT = 1,
    AWL_DAMAGE_FULL = 2,
};

/* Add one protocol rectangle to a pending bbox. Non-positive extents are
 * empty. int64 endpoints prevent x+w/y+h overflow for int32 protocol args. */
void awl_damage_box_add(struct awl_damage_box* box, int* empty,
                        int32_t x, int32_t y, int32_t w, int32_t h);

/* Clip an already-buffer-coordinate pending bbox to the committed buffer. */
enum awl_damage_result awl_damage_buffer_box(
        const struct awl_damage_box* box, uint32_t width, uint32_t height,
        struct awl_damage_rect* out);

/* Convert surface-local damage through the committed viewport, scale and
 * transform into raw buffer coordinates. Rounds outwards: over-copy is safe,
 * under-copy corrupts incremental SHM uploads. */
enum awl_damage_result awl_damage_surface_box(
        const struct awl_damage_box* box, const struct awl_damage_map* map,
        struct awl_damage_rect* out);

void awl_damage_rect_union(struct awl_damage_rect* dst,
                           const struct awl_damage_rect* src);

#endif
