/* awl_renderer.hpp — GPU renderer (per-window EGL) */
#ifndef AWL_RENDERER_HPP
#define AWL_RENDERER_HPP

#include "awl.h"

#include <android/native_window.h>
#include <math.h>
#include <stdint.h>

/* ---- pixel-grid snap of a layer's dst extent (GL composite) ----
 * kwin snapToPixelGridF shape: the size is rounded on its own, never derived
 * from two independently rounded corners (off by one for half the positions).
 * One rule on top of kwin: when the layer's own sampled buffer extent
 * (viewport source | whole buffer, surface orientation) is within 1 px of
 * logical × s, the client rendered this layer at the display scale and only
 * its rounding differs from ours — fractional-scale-v1 pins toplevel buffers
 * to round-half-away-from-zero (= round() here) but leaves subsurface size
 * rounding undefined — so present exactly its pixel count: the buffer lands
 * 1:1 instead of being stretched by one pixel across the whole layer. */
static inline int32_t awl_snap_extent(double logical, double s, double sampled) {
    double want = logical * s;
    if (sampled > 0.0 && fabs(sampled - want) < 1.0) return (int32_t)lround(sampled);
    return (int32_t)lround(want);
}
/* Sampled buffer extent of a layer in surface orientation (viewport source
 * region or the whole buffer; 90/270 transforms swap the buffer axes). */
static inline void awl_layer_sampled(const awl_layer_info_t* li, uint32_t bw,
                                     uint32_t bh, double* sw, double* sh) {
    double w = (double)li->su * (double)bw, h = (double)li->sv * (double)bh;
    if (li->transform & 1) { double t = w; w = h; h = t; }
    *sw = w;
    *sh = h;
}

/* DRM fourcc ('AR24' little-endian = memory order B,G,R,A) */
#define AWL_FOURCC_CODE(a, b, c, d) \
    ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | \
     ((uint32_t)(d) << 24))
#define AWL_FOURCC_ARGB8888 AWL_FOURCC_CODE('A', 'R', '2', '4')
#define AWL_FOURCC_XRGB8888 AWL_FOURCC_CODE('X', 'R', '2', '4')

/* HAL_PIXEL_FORMAT_BGRA_8888 (=5): platform graphics.h value — the NDK
 * AHardwareBuffer_Format enum skips it (defines 1..4 then jumps to 0x16) */
#define AWL_HAL_BGRA_8888 5u

/* attach: create GL resources (calling thread, concurrency-safe) + spawn the
 * window's dedicated render thread; detach(NULL): stop+join the render thread
 * then release GL — strictly ordered with that window's rendering */
int  awl_renderer_attach(uint64_t id, ANativeWindow* nw);   /* NULL=detach */

/* Request a render (any thread, returns fast): sets the window's render
 * request and wakes its render thread. The actual GL runs on each window's
 * own thread — windows render in parallel, a frame submit only blocks itself */
void awl_renderer_request_render(uint64_t id);

void awl_renderer_shutdown(void);

#endif
