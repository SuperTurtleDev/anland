#include "awl_damage.h"

#include <limits.h>
#include <math.h>

void awl_damage_box_add(struct awl_damage_box* box, int* empty,
                        int32_t x, int32_t y, int32_t w, int32_t h) {
    if (w <= 0 || h <= 0) return;
    int64_t x1 = x, y1 = y;
    int64_t x2 = x1 + (int64_t)w, y2 = y1 + (int64_t)h;
    if (*empty) {
        box->x1 = x1; box->y1 = y1; box->x2 = x2; box->y2 = y2;
        *empty = 0;
        return;
    }
    if (x1 < box->x1) box->x1 = x1;
    if (y1 < box->y1) box->y1 = y1;
    if (x2 > box->x2) box->x2 = x2;
    if (y2 > box->y2) box->y2 = y2;
}

static enum awl_damage_result clip_box(double x1, double y1,
                                       double x2, double y2,
                                       uint32_t width, uint32_t height,
                                       struct awl_damage_rect* out) {
    if (!width || !height || width > INT32_MAX || height > INT32_MAX ||
        !isfinite(x1) || !isfinite(y1) || !isfinite(x2) || !isfinite(y2))
        return AWL_DAMAGE_FULL;
    if (x1 < 0.0) x1 = 0.0;
    if (y1 < 0.0) y1 = 0.0;
    if (x2 > (double)width) x2 = (double)width;
    if (y2 > (double)height) y2 = (double)height;
    if (x2 <= x1 || y2 <= y1) return AWL_DAMAGE_EMPTY;
    int64_t ix1 = (int64_t)floor(x1);
    int64_t iy1 = (int64_t)floor(y1);
    int64_t ix2 = (int64_t)ceil(x2);
    int64_t iy2 = (int64_t)ceil(y2);
    if (ix1 < 0) ix1 = 0;
    if (iy1 < 0) iy1 = 0;
    if (ix2 > (int64_t)width) ix2 = width;
    if (iy2 > (int64_t)height) iy2 = height;
    if (ix2 <= ix1 || iy2 <= iy1) return AWL_DAMAGE_EMPTY;
    out->x = (int32_t)ix1;
    out->y = (int32_t)iy1;
    out->w = (int32_t)(ix2 - ix1);
    out->h = (int32_t)(iy2 - iy1);
    return AWL_DAMAGE_RECT;
}

enum awl_damage_result awl_damage_buffer_box(
        const struct awl_damage_box* box, uint32_t width, uint32_t height,
        struct awl_damage_rect* out) {
    return clip_box((double)box->x1, (double)box->y1,
                    (double)box->x2, (double)box->y2,
                    width, height, out);
}

static void transformed_to_buffer(int transform, double qx, double qy,
                                  double bw, double bh,
                                  double* x, double* y) {
    switch (transform) {
    case 1: *x = qy * bw;         *y = (1.0 - qx) * bh; break;
    case 2: *x = (1.0 - qx) * bw; *y = (1.0 - qy) * bh; break;
    case 3: *x = (1.0 - qy) * bw; *y = qx * bh; break;
    case 4: *x = (1.0 - qx) * bw; *y = qy * bh; break;
    case 5: *x = qy * bw;         *y = qx * bh; break;
    case 6: *x = qx * bw;         *y = (1.0 - qy) * bh; break;
    case 7: *x = (1.0 - qy) * bw; *y = (1.0 - qx) * bh; break;
    default:*x = qx * bw;         *y = qy * bh; break;
    }
}

enum awl_damage_result awl_damage_surface_box(
        const struct awl_damage_box* box, const struct awl_damage_map* map,
        struct awl_damage_rect* out) {
    if (!map || !map->buffer_width || !map->buffer_height ||
        map->buffer_width > INT32_MAX || map->buffer_height > INT32_MAX ||
        map->buffer_scale < 1 || map->buffer_transform < 0 ||
        map->buffer_transform > 7)
        return AWL_DAMAGE_FULL;

    const int swap = map->buffer_transform == 1 || map->buffer_transform == 3 ||
                     map->buffer_transform == 5 || map->buffer_transform == 7;
    const double tw = swap ? map->buffer_height : map->buffer_width;
    const double th = swap ? map->buffer_width : map->buffer_height;
    const double scale = map->buffer_scale;
    const double base_w = tw / scale, base_h = th / scale;

    double sx = 0.0, sy = 0.0, sw = base_w, sh = base_h;
    if (map->viewport_has_source) {
        /* The renderer's existing transformed viewport-source sampling is not
         * yet trustworthy. Full upload preserves pixels until that separate
         * mapping path is corrected. Normal-transform cropping is exact. */
        if (map->buffer_transform != 0) return AWL_DAMAGE_FULL;
        sx = map->viewport_src_x;
        sy = map->viewport_src_y;
        sw = map->viewport_src_width;
        sh = map->viewport_src_height;
        if (!isfinite(sx) || !isfinite(sy) || !isfinite(sw) || !isfinite(sh) ||
            sx < 0.0 || sy < 0.0 || sw <= 0.0 || sh <= 0.0 ||
            sx + sw > base_w || sy + sh > base_h)
            return AWL_DAMAGE_FULL;
    }

    double dw, dh;
    if (map->viewport_dst_width > 0 && map->viewport_dst_height > 0) {
        dw = map->viewport_dst_width;
        dh = map->viewport_dst_height;
    } else if (map->viewport_dst_width == 0 && map->viewport_dst_height == 0) {
        dw = sw;
        dh = sh;
    } else {
        return AWL_DAMAGE_FULL;
    }
    if (!isfinite(dw) || !isfinite(dh) || dw <= 0.0 || dh <= 0.0)
        return AWL_DAMAGE_FULL;

    double rx1 = (double)box->x1, ry1 = (double)box->y1;
    double rx2 = (double)box->x2, ry2 = (double)box->y2;
    if (rx1 < 0.0) rx1 = 0.0;
    if (ry1 < 0.0) ry1 = 0.0;
    if (rx2 > dw) rx2 = dw;
    if (ry2 > dh) ry2 = dh;
    if (rx2 <= rx1 || ry2 <= ry1) return AWL_DAMAGE_EMPTY;

    const double px[2] = {
        (sx + rx1 * sw / dw) * scale,
        (sx + rx2 * sw / dw) * scale,
    };
    const double py[2] = {
        (sy + ry1 * sh / dh) * scale,
        (sy + ry2 * sh / dh) * scale,
    };
    double bx1 = INFINITY, by1 = INFINITY, bx2 = -INFINITY, by2 = -INFINITY;
    for (int ix = 0; ix < 2; ix++) {
        for (int iy = 0; iy < 2; iy++) {
            double bx, by;
            transformed_to_buffer(map->buffer_transform,
                                  px[ix] / tw, py[iy] / th,
                                  map->buffer_width, map->buffer_height,
                                  &bx, &by);
            if (bx < bx1) bx1 = bx;
            if (by < by1) by1 = by;
            if (bx > bx2) bx2 = bx;
            if (by > by2) by2 = by;
        }
    }
    return clip_box(bx1, by1, bx2, by2,
                    map->buffer_width, map->buffer_height, out);
}

void awl_damage_rect_union(struct awl_damage_rect* dst,
                           const struct awl_damage_rect* src) {
    int64_t x1 = dst->x < src->x ? dst->x : src->x;
    int64_t y1 = dst->y < src->y ? dst->y : src->y;
    int64_t dx2 = (int64_t)dst->x + dst->w;
    int64_t dy2 = (int64_t)dst->y + dst->h;
    int64_t sx2 = (int64_t)src->x + src->w;
    int64_t sy2 = (int64_t)src->y + src->h;
    int64_t x2 = dx2 > sx2 ? dx2 : sx2;
    int64_t y2 = dy2 > sy2 ? dy2 : sy2;
    dst->x = (int32_t)x1;
    dst->y = (int32_t)y1;
    dst->w = (int32_t)(x2 - x1);
    dst->h = (int32_t)(y2 - y1);
}
