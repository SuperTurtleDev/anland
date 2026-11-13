#include "awl_damage.h"

#include <assert.h>
#include <stdio.h>

static struct awl_damage_box box(int32_t x, int32_t y, int32_t w, int32_t h) {
    struct awl_damage_box b = {0};
    int empty = 1;
    awl_damage_box_add(&b, &empty, x, y, w, h);
    assert(!empty);
    return b;
}

static void expect_rect(enum awl_damage_result got,
                        const struct awl_damage_rect* r,
                        int x, int y, int w, int h) {
    assert(got == AWL_DAMAGE_RECT);
    if (r->x != x || r->y != y || r->w != w || r->h != h)
        fprintf(stderr, "got %d,%d %dx%d expected %d,%d %dx%d\n",
                r->x, r->y, r->w, r->h, x, y, w, h);
    assert(r->x == x && r->y == y && r->w == w && r->h == h);
}

int main(void) {
    struct awl_damage_rect r;
    struct awl_damage_map m = {
        .buffer_width = 200, .buffer_height = 100,
        .buffer_scale = 2, .buffer_transform = 0,
    };
    struct awl_damage_box b = box(10, 5, 20, 10);
    expect_rect(awl_damage_surface_box(&b, &m, &r), &r, 20, 10, 40, 20);

    m.viewport_dst_width = 100;
    m.viewport_dst_height = 50;
    expect_rect(awl_damage_surface_box(&b, &m, &r), &r, 20, 10, 40, 20);

    m.buffer_width = 400; m.buffer_height = 300; m.buffer_scale = 1;
    m.viewport_dst_width = 100; m.viewport_dst_height = 50;
    m.viewport_has_source = 1;
    m.viewport_src_x = 50; m.viewport_src_y = 20;
    m.viewport_src_width = 200; m.viewport_src_height = 100;
    /* Floating viewport math rounds conservatively outwards; one extra pixel
     * is valid, while one missing pixel would corrupt an incremental upload. */
    expect_rect(awl_damage_surface_box(&b, &m, &r), &r, 70, 30, 41, 20);

    m = (struct awl_damage_map) {
        .buffer_width = 200, .buffer_height = 100,
        .buffer_scale = 1, .buffer_transform = 1,
    };
    b = box(10, 20, 30, 40);
    expect_rect(awl_damage_surface_box(&b, &m, &r), &r, 20, 60, 40, 30);

    m.viewport_has_source = 1;
    m.viewport_src_width = 100; m.viewport_src_height = 200;
    assert(awl_damage_surface_box(&b, &m, &r) == AWL_DAMAGE_FULL);

    b = box(-10, -20, INT32_MAX, INT32_MAX);
    m = (struct awl_damage_map) {
        .buffer_width = 200, .buffer_height = 100,
        .buffer_scale = 1, .buffer_transform = 0,
    };
    expect_rect(awl_damage_surface_box(&b, &m, &r), &r, 0, 0, 200, 100);

    b = box(150, 70, 100, 100);
    expect_rect(awl_damage_buffer_box(&b, 200, 100, &r), &r, 150, 70, 50, 30);

    struct awl_damage_rect a = {10, 20, 30, 40};
    struct awl_damage_rect c = {20, 10, 50, 20};
    awl_damage_rect_union(&a, &c);
    assert(a.x == 10 && a.y == 10 && a.w == 60 && a.h == 50);

    puts("awl_damage_test: PASS");
    return 0;
}
