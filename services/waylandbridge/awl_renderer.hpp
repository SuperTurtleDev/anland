/* awl_renderer.hpp — GPU renderer (per-window EGL root layer) + dmabuf→AHB
 * wrap export shared with awl_hwc.cpp */
#ifndef AWL_RENDERER_HPP
#define AWL_RENDERER_HPP

#include "awl.h"

#include <android/hardware_buffer.h>
#include <android/native_window.h>
#include <stdint.h>

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

/* Per-surface dmabuf slot: ONE AHardwareBuffer per surface, swapped per
 * arriving buffer (awl_renderer.cpp for the snapalloc donor scheme). When the
 * dmabuf changes the AHB is re-forged with a fresh identity (SF/kgsl bind the
 * memory at import — an in-place fd swap would leave consumers sampling the
 * old dmabuf); the re-forge uses the OLD AHB itself as the donor (its metadata
 * blob already carries this geometry — no allocation), and the old AHB's
 * release closes the swapped-out dmabuf fd. The blob stays alive through the
 * relay: each forged AHB's handle holds its own fd dup. After a swap the
 * consumer MUST re-import: setBuffer for SC layers, a new EGLImage for the GL
 * root. Callers treat the slot as opaque apart from swap/destroy.
 * Render-thread only per surface. */
struct awl_ahb_slot {
    AHardwareBuffer* ahb = NULL;   /* wraps the current dmabuf */
    uint64_t ino = 0;              /* dma-buf identity (fstat inode) */
    uint32_t w = 0, h = 0, stride = 0;
};

/* 1 = swapped (new memory — re-import required), 0 = same dmabuf (no-op),
 * -1 = import refused (old AHB kept). */
int  awl_renderer_ahb_swap(awl_ahb_slot* s, const awl_buffer_info_t* b);
void awl_renderer_ahb_slot_destroy(awl_ahb_slot* s);

#endif
