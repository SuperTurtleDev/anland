/* awl_hwc.hpp — subsurface-family child layers composed by
 * SurfaceFlinger/HWC (ASurfaceControl), no GL fallback */
#ifndef AWL_HWC_HPP
#define AWL_HWC_HPP

#include "awl.h"

#include <android/native_window.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque per-window SC state (defined in awl_hwc.cpp) */
typedef struct awl_hwc_window awl_hwc_window;

/* Called on window attach (children of the window's own Surface; SF composes
 * them above the window buffer, matching the Wayland stack where the root is
 * the bottom layer). Returns NULL only on allocation failure — a window
 * without hwc state simply shows no child layers. Never fatal: a failing
 * single layer only HIDEs that layer. */
awl_hwc_window* awl_hwc_create(ANativeWindow* nw);
void awl_hwc_destroy(awl_hwc_window* h);

/* One render pass for every CHILD layer of the snapshot (lay[1..n-1]:
 * wl_subsurface stack + xdg_popup riding the same tree + the client cursor
 * image; lay[0] root is drawn by the GL path into the window's own buffer and
 * is skipped here). gox/goy = root xdg geometry origin; sx/sy/ox/oy =
 * awl_view_map scale/offset (dst = ((x − go) × s + o), same math as the GL
 * path). Single atomic transaction: z order, source crop (viewport), dst
 * position/scale, buffer transform, buffer, blending mode, visibility; layers
 * absent from the snapshot are hidden and reclaimed. Frame callbacks /
 * wl_buffer releases fire from the SF callbacks (see file header). Runs on the
 * window's render thread only. */
void awl_hwc_frame(awl_hwc_window* h, const awl_layer_info_t* lay, int n,
                   int32_t gox, int32_t goy,
                   double sx, double sy, double ox, double oy);

#ifdef __cplusplus
}
#endif
#endif /* AWL_HWC_HPP */
