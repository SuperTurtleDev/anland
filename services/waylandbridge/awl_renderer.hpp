/* awl_renderer.hpp — GPU renderer (per-window EGL) */
#ifndef AWL_RENDERER_HPP
#define AWL_RENDERER_HPP

#include "awl.h"

#include <android/native_window.h>
#include <stdint.h>

/* DRM fourcc ('AR24' little-endian = memory order B,G,R,A) */
#define AWL_FOURCC_CODE(a, b, c, d) \
    ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | \
     ((uint32_t)(d) << 24))
#define AWL_FOURCC_ARGB8888 AWL_FOURCC_CODE('A', 'R', '2', '4')
#define AWL_FOURCC_XRGB8888 AWL_FOURCC_CODE('X', 'R', '2', '4')

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
