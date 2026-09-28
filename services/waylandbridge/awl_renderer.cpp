/* awl_renderer.cpp — per-window GPU compositor (GL path, sc_enabled=0)
 *
 * Frame sources, per layer:
 *   dmabuf  the layer's buffer queue (awl_bufferqueue.h: dma-buf fd +
 *           acquire fence + geometry, whatever the client committed through
 *           zwp_linux_dmabuf). lock → drain (superseded frames go back to the
 *           client) → gethead (waits the acquire fence, takes a reference) →
 *           unlock → forged AHardwareBuffer → EGLImage → zero-copy texture
 *           (per-layer cache keyed by dma-buf inode: a client cycling its
 *           swapchain never re-imports) → draw → put(native fence of this
 *           composite) — the element goes back to the client when the ring
 *           has dropped it too, with the fence as its release fence.
 *   wl_shm  the logic layer's in-place source (awl.h awl_surface_shm_begin/
 *           end): the client's pool is read straight into a per-layer GL
 *           texture, damage rect only, and the buffer is released by the
 *           logic layer when that upload returned. No intermediate copy.
 * Composite = one textured quad per layer in render stack order (kwin
 * order: below-children, surface, above-children; the client's
 * wl_pointer.set_cursor image on top) sampled into dst rects →
 * eglSwapBuffers → BufferQueue/SurfaceFlinger present. blend = premultiplied
 * alpha (ONE, ONE_MINUS_SRC_ALPHA); the first layer drawn and XR24 layers
 * blend off. wl_surface.set_buffer_transform is applied per layer via the
 * sample matrix (awl_gl_xform).
 *
 * All rendering happens on the window's dedicated render thread
 * (render_thread_loop). EGL display / program / imports / fences are the
 * shared awl_gl module (the SurfaceControl backend's EGL-mode layers use the
 * same code — both backends must sample a client buffer identically). */
#include "awl_renderer.hpp"
#include "awl_ahb.hpp"
#include "awl_gl.hpp"
#include "awl_geom.h"
#include "awl_bufferqueue.h"

#include <android/log.h>
#include <android/native_window.h>
#include <linux/dma-buf.h>
#include <math.h>                 /* round: pixel-grid snap of the dst */
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>               /* close (release fence) */

#include <atomic>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#define AWL_TAG "anland-rd"
#include "awl_log.h"   /* LOGI/LOGE/LOGD (LOGD compiled out unless AWL_LOG_DEBUG) */

/* Per-layer sources: the forged-AHB cache for dmabuf frames (slot payload =
 * EGLImage + texture, awl_gl_slot_texture) and the wl_shm upload texture.
 * Render-thread only per window. */
struct wl_layer {
    struct awl_ahb_cache* cache = nullptr;
    awl_gl_shm_tex shm;
    bool cpu_dmabuf = false;      /* sticky after vendor AHB forging is incompatible */
};

struct wl_window {
    uint64_t id;
    ANativeWindow* nw;        /* self-held reference */
    EGLSurface surface = EGL_NO_SURFACE;
    EGLContext context = EGL_NO_CONTEXT;
    awl_gl_quad quad;
    int cur_vw = 0, cur_vh = 0;    /* ANativeWindow size cache — valid until
                                    * the logic layer reports a resize
                                    * (awl_window_resize → size_dirty;
                                    * small-window ↔ fullscreen resizes the
                                    * surface in place, no SURFACE re-attach) */
    std::atomic<bool> size_dirty{true};   /* render_frame re-queries once */
    bool logged_frame = false;     /* first-frame log (diagnostics) */
    uint64_t frame_no = 0;         /* LRU clock of the layer import caches */
    std::map<uint64_t, wl_layer> layers;   /* layer id → sources (includes root's own id) */

    /* dedicated render thread: context bound 1:1 to the thread, requests coalesced via condvar */
    std::thread th;
    std::mutex m;
    std::condition_variable cv;
    bool stop = false;
    bool render_req = false;
};

static std::mutex g_map_lock;
static std::map<uint64_t, wl_window*> g_windows;

/* current context */
static void layer_release(wl_layer* l) {
    if (l->cache) {
        awl_ahb_cache_destroy(l->cache);   /* releases the forged AHBs + GL payloads */
        l->cache = nullptr;
    }
    awl_gl_shm_release(&l->shm);
}

/* ---------------- per-window GL ---------------- */

static bool window_setup_gl(wl_window* w) {
    EGLDisplay dpy = awl_gl_display();
    w->context = awl_gl_create_context();
    if (w->context == EGL_NO_CONTEXT) return false;
    w->surface = eglCreateWindowSurface(dpy, awl_gl_config(),
                                        (EGLNativeWindowType)w->nw, NULL);
    if (w->surface == EGL_NO_SURFACE) {
        LOGE("eglCreateWindowSurface: 0x%x", eglGetError());
        return false;
    }
    if (!eglMakeCurrent(dpy, w->surface, w->surface, w->context)) {
        LOGE("eglMakeCurrent: 0x%x", eglGetError());
        return false;
    }
    bool ok = awl_gl_quad_create(&w->quad);
    if (ok) {
        glViewport(0, 0, ANativeWindow_getWidth(w->nw), ANativeWindow_getHeight(w->nw));
        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        const char* glv = (const char*)glGetString(GL_VERSION);
        LOGI("window %llu GL ready %dx%d (%s)", (unsigned long long)w->id,
             ANativeWindow_getWidth(w->nw), ANativeWindow_getHeight(w->nw), glv ? glv : "?");
    }
    /* setup runs on the calling thread (binder pool), rendering on the
     * window's dedicated thread — release current */
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    return ok;
}

/* After the render thread has joined (it unbound the context): bind it here
 * to free its objects, then destroy. */
static void window_teardown_gl(wl_window* w) {
    EGLDisplay dpy = awl_gl_display();
    if (w->context != EGL_NO_CONTEXT) {
        bool bound = w->surface != EGL_NO_SURFACE &&
                     eglMakeCurrent(dpy, w->surface, w->surface, w->context);
        for (auto& kv : w->layers) layer_release(&kv.second);   /* GL deletes are no-ops unbound; the context's death frees them */
        w->layers.clear();
        awl_gl_quad_destroy(&w->quad);
        if (bound) eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (w->surface != EGL_NO_SURFACE) eglDestroySurface(dpy, w->surface);
        eglDestroyContext(dpy, w->context);
        w->context = EGL_NO_CONTEXT;
        w->surface = EGL_NO_SURFACE;
    }
}

/* ---------------- public API ---------------- */

static void render_thread_loop(wl_window* w);   /* defined at end of file */
void awl_renderer_request_render(uint64_t id);  /* awl_renderer.hpp; used by render_frame's re-arm */

/* Compatibility path for vendor gralloc layouts that cannot describe Mesa's
 * linear KGSL pitch (alioth: client 2048px, Android allocator skips directly
 * from 1792 to 2304).  The queue has already waited the acquire fence before
 * this point.  Copy the linear AR24/XR24 dma-buf into the same GL texture
 * uploader used by wl_shm; release happens only after the upload has returned.
 * Full upload is deliberate: linux-dmabuf commits do not carry a stable damage
 * snapshot in the queue element. */
static GLuint dmabuf_cpu_texture(wl_layer* l, const struct awl_bq_buffer* b) {
    size_t bytes = (size_t)b->stride * b->height;
    if (!bytes || b->stride < b->width * 4) return 0;
    off_t alloc = lseek(b->dmabuf_fd, 0, SEEK_END);
    if (alloc > 0 && (uint64_t)alloc < bytes) {
        LOGE("dmabuf CPU fallback allocation too small: %lld < %zu",
             (long long)alloc, bytes);
        return 0;
    }
    struct dma_buf_sync sync = { .flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ };
    bool cpu_sync = ioctl(b->dmabuf_fd, DMA_BUF_IOCTL_SYNC, &sync) == 0;
    void* pixels = mmap(NULL, bytes, PROT_READ, MAP_SHARED, b->dmabuf_fd, 0);
    if (pixels == MAP_FAILED) {
        LOGE("dmabuf CPU fallback mmap %ux%u stride=%u failed: %s",
             b->width, b->height, b->stride, strerror(errno));
        if (cpu_sync) {
            sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
            ioctl(b->dmabuf_fd, DMA_BUF_IOCTL_SYNC, &sync);
        }
        return 0;
    }
    awl_shm_frame_t f = {};
    f.width = b->width;
    f.height = b->height;
    f.stride = b->stride;
    f.format = b->format;
    f.pixels = pixels;
    f.dmg_full = 1;
    f.serial = l->shm.serial + 1;
    bool ok = awl_gl_shm_update(&l->shm, &f);
    munmap(pixels, bytes);
    if (cpu_sync) {
        sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
        ioctl(b->dmabuf_fd, DMA_BUF_IOCTL_SYNC, &sync);
    }
    return ok ? l->shm.texture : 0;
}

/* Detach and reclaim a window entry (map removal inside g_map_lock, join/free
 * entirely outside the lock — join must not hold g_map_lock: it would stall
 * every window's request_render, which runs on the client dispatch thread).
 * Returns whether an entry was actually reclaimed. */
static bool renderer_teardown_locked_out(uint64_t id) {
    wl_window* w = NULL;
    {
        std::lock_guard<std::mutex> lk(g_map_lock);
        auto it = g_windows.find(id);
        if (it == g_windows.end()) return false;
        w = it->second;
        g_windows.erase(it);
    }
    {
        std::lock_guard<std::mutex> lk(w->m);
        w->stop = true;
        w->cv.notify_all();
    }
    if (w->th.joinable()) w->th.join();
    window_teardown_gl(w);
    ANativeWindow_release(w->nw);
    delete w;
    return true;
}

int awl_renderer_attach(uint64_t id, ANativeWindow* nw) {
    /* attach/detach for the same id fully serialized: prevents a g_windows[id]
     * overwrite leak when concurrent SURFACE (re-attach) and detach interleave
     * (old thread never joined). Lifecycle-level operation, low frequency —
     * serialization is harmless; request_render bypasses this lock. */
    static std::mutex attach_lock;
    std::lock_guard<std::mutex> alkg(attach_lock);

    /* Tear down any existing entry with the same id first (re-attach = old
     * render target necessarily stale/replaced): same semantics as the
     * detach branch below */
    renderer_teardown_locked_out(id);

    if (nw) {
        if (!awl_gl_init()) return -1;
        wl_window* w = new wl_window();
        w->id = id;
        ANativeWindow_acquire(nw);
        w->nw = nw;
        if (!window_setup_gl(w)) {      /* GL setup on the calling thread (binder pool, concurrent) */
            window_teardown_gl(w);
            ANativeWindow_release(nw);
            delete w;
            return -1;
        }
        w->cur_vw = ANativeWindow_getWidth(nw);
        w->cur_vh = ANativeWindow_getHeight(nw);
        w->th = std::thread(render_thread_loop, w);   /* dedicated render thread */
        std::lock_guard<std::mutex> lk(g_map_lock);
        g_windows[id] = w;
        return 0;
    }
    return 0;
}

/* ---------------- per-window render thread ----------------
 * context stays resident on this thread (MakeCurrent once); requests
 * coalesced/deduped; glFinish/swap block only this window — multi-window
 * parallelism */

static void render_frame(wl_window* w) {
    /* Layer snapshot in render stack order bottom→top (kwin traversal: a
     * surface's below-children, the surface, its above-children — the root is
     * the first element only when nothing is placed below it); layers that
     * have no content are skipped.
     * #31 zoom: coordinates/sizes are root logical pixels; dst = (logical −
     * geometry origin) × s + o with the root's view transform (1:1 at Z for
     * content following the configure, scale_mode placement otherwise),
     * snapped to the pixel grid (kwin snapToPixelGrid). The geometry-origin
     * alignment (chrome buffer carries 16/10px shadow margins) is shared
     * with the input mapping. */
    awl_layer_info_t lay[AWL_MAX_LAYERS + 1];   /* +1: client cursor image appended on top */
    int n = awl_surface_get_layers(w->id, lay, AWL_MAX_LAYERS);
    if (n <= 0) return;
    /* wl_pointer.set_cursor image of the pointer-focused client: composited
     * above every layer of this window (x,y = pointer − hotspot in the same
     * root logical coordinates as the stack; never part of hit-testing).
     * Drawn/presented like any layer → the cursor surface gets frame_done
     * (animated cursors) and deferred buffer release. */
    if (awl_pointer_cursor_layer(w->id, &lay[n])) n++;
    /* Root view transform (logic layer, the same snapshot the input inverse
     * uses): geometry origin + logical→view scale/offset. Degenerate → identity. */
    awl_view_xform_t xf;
    awl_surface_get_view_xform(w->id, &xf);

    /* size cache — dropped when the logic layer reports a resize
     * (awl_window_resize → awl_renderer_window_resized); re-query renders the
     * frame at the fresh viewport/u_view = the original full-screen flush */
    if (w->size_dirty.load(std::memory_order_relaxed) || w->cur_vw <= 0 ||
        w->cur_vh <= 0) {
        w->cur_vw = ANativeWindow_getWidth(w->nw);
        w->cur_vh = ANativeWindow_getHeight(w->nw);
        w->size_dirty.store(false, std::memory_order_relaxed);
    }
    int vw = w->cur_vw;
    int vh = w->cur_vh;
    LOGD("win %llu render: view=%dx%d n=%d bottom=%llu xf(s=%.3f,%.3f o=%.1f,%.1f go=%d,%d)",
         (unsigned long long)w->id, vw, vh, n,
         (unsigned long long)lay[0].surface_id,
         xf.sx, xf.sy, xf.ox, xf.oy, xf.gox, xf.goy);

    glViewport(0, 0, vw, vh);
    glClearColor(0, 0, 0, 1);   /* letterbox bars for fit/center modes */
    glClear(GL_COLOR_BUFFER_BIT);
    awl_gl_quad_begin(&w->quad, (float)vw, (float)vh, 1.0f);

    /* Per-layer dmabuf source: resolve the queue (own reference — a layer
     * dying mid-frame cannot free it), lock → drain superseded frames back
     * to the client → take a reference on the newest complete head (waits
     * its acquire fence, bounded) → arm the fence waiter for a still-
     * incomplete newer frame → unlock. Microseconds under the lock: the
     * element reference, not the lock, keeps the frame alive and its memory
     * open while we import and draw. NULL / NULL-marker = no dmabuf frame:
     * the layer's content, if any, is wl_shm (asked below). */
    struct awl_bq_buffer* head[AWL_MAX_LAYERS + 1];
    bool more = false;   /* a queue still holds a newer, not-yet-complete frame */
    for (int i = 0; i < n; i++) {
        head[i] = NULL;
        struct awl_bufferqueue* q = awl_surface_queue_ref(lay[i].surface_id);
        if (!q) continue;
        awl_bufferqueue_lock(q);
        awl_bufferqueue_drain(q);
        head[i] = awl_bufferqueue_gethead(q, 100);
        if (awl_bufferqueue_pending(q) > 0) more = true;
        awl_bufferqueue_arm(q);   /* pending incomplete frame: its fence, not the next vsync, releases the head */
        awl_bufferqueue_unlock(q);
        awl_bufferqueue_unref(q);
    }
    w->frame_no++;

    uint64_t seen[AWL_MAX_LAYERS + 1];
    int nseen = 0;
    bool drew = false;
    for (int i = 0; i < n; i++) {
        wl_layer& l = w->layers[lay[i].surface_id];
        const struct awl_bq_buffer* b = head[i];
        GLuint tex = 0;
        uint32_t bw = 0, bh = 0, fmt = 0;
        if (b && b->dmabuf_fd >= 0) {
            /* dmabuf frame → forged AHB → EGLImage texture (cache hit from the second lap on) */
            if (!l.cpu_dmabuf) {
                if (!l.cache) l.cache = awl_ahb_cache_create(awl_gl_tex_payload_destroy);
                struct awl_ahb_slot* s = l.cache
                    ? awl_ahb_cache_get(l.cache, b, AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE, w->frame_no)
                    : nullptr;
                if (s) tex = awl_gl_slot_texture(s);
            }
            if (tex) {
                if (l.shm.texture) awl_gl_shm_release(&l.shm);
            } else {
                tex = dmabuf_cpu_texture(&l, b);
                if (tex && !l.cpu_dmabuf) {
                    l.cpu_dmabuf = true;
                    LOGI("window %llu layer %llu: AHB unavailable — linear dmabuf CPU upload fallback",
                         (unsigned long long)w->id,
                         (unsigned long long)lay[i].surface_id);
                }
            }
            bw = b->width;
            bh = b->height;
            fmt = b->format;
        } else {
            /* no dmabuf frame: wl_shm content? 1 = upload the damage rect
             * from the client's pool (surface locked until end), 2 =
             * unchanged (keep the texture), 0 = no shm content */
            awl_shm_frame_t f;
            int r = awl_surface_shm_begin(lay[i].surface_id, l.shm.serial, &f);
            if (r == 1) {
                bool ok = awl_gl_shm_update(&l.shm, &f);
                awl_surface_shm_end(&f, ok);   /* consumed → wl_buffer.release goes out */
            } else if (r == 0 && l.shm.texture) {
                awl_gl_shm_release(&l.shm);    /* unmapped */
            }
            if (l.shm.texture) {
                tex = l.shm.texture;
                bw = l.shm.w;
                bh = l.shm.h;
                fmt = l.shm.format;
            }
        }
        if (!tex) continue;
        seen[nseen++] = lay[i].surface_id;

        /* The stack is kwin order (below children → surface → above children),
         * so the root is not necessarily first: the first layer drawn writes
         * with blend off (nothing beneath it), XR24 layers are opaque by
         * protocol (their alpha byte is undefined — never blend on it), every
         * other layer stacks with premultiplied alpha. */
        if (!drew || fmt == AWL_FOURCC_XRGB8888) glDisable(GL_BLEND);
        else {
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        }
        /* Pixel-grid snap (awl_snap_extent, awl_geom.h): integer origin,
         * size = the buffer's own pixel count when it is the Z-scaled rendition
         * of the logical size — GL_LINEAR then samples texel centers: lossless.
         * A fractional origin/size would resample the whole buffer (half-pixel
         * blur) even at scale 1. */
        double rsw, rsh;
        awl_layer_sampled(&lay[i], bw, bh, &rsw, &rsh);
        float dst[4] = {
            (float)round(((double)lay[i].x - (double)xf.gox) * xf.sx + xf.ox),
            (float)round(((double)lay[i].y - (double)xf.goy) * xf.sy + xf.oy),
            (float)awl_snap_extent(lay[i].w, xf.sx, rsw),
            (float)awl_snap_extent(lay[i].h, xf.sy, rsh),
        };
        float uv[4] = { lay[i].u0, lay[i].v0, lay[i].su, lay[i].sv };
        awl_gl_quad_draw(&w->quad, tex, dst, uv, lay[i].transform);
        drew = true;

        if (!w->logged_frame) {
            w->logged_frame = true;
            LOGI("window %llu frame: layers=%d [%d]=%llu %ux%u %s fmt=%c%c%c%c xform=%d (glerr=0x%x)",
                 (unsigned long long)w->id, n, i,
                 (unsigned long long)lay[i].surface_id, bw, bh,
                 (b && b->dmabuf_fd >= 0) ? "dmabuf" : "shm",
                 (char)(fmt & 0xff), (char)((fmt >> 8) & 0xff),
                 (char)((fmt >> 16) & 0xff), (char)((fmt >> 24) & 0xff),
                 lay[i].transform, glGetError());
        }
    }
    if (!drew) {   /* no layer has content — don't spin on a swap */
        for (int i = 0; i < n; i++) awl_bufferqueue_put(head[i], -1);   /* nothing sampled */
        if (more) awl_renderer_request_render(w->id);
        return;
    }

    /* vanished / contentless layers (bubble hidden/destroyed): reclaim textures and AHB references */
    for (auto it = w->layers.begin(); it != w->layers.end();) {
        bool found = false;
        for (int k = 0; k < nseen && !found; k++)
            found = (seen[k] == it->first);
        if (!found) {
            layer_release(&it->second);
            it = w->layers.erase(it);
        } else {
            ++it;
        }
    }

    /* Release fence for the client buffers this composite sampled: a native
     * fence inserted after the draws (signals when the GPU is done reading;
     * the dup flushes the command stream). put() merges it into every held
     * element and drops our reference — when the ring has already popped
     * the element (a newer frame superseded it while we drew) it goes back
     * to the client right here, with the fence: explicit-sync clients get it
     * as the fenced_release, implicit-sync clients find it as a read fence
     * in the dma-buf reservation (bq_release_cb). Without native fences the
     * only remaining guarantee is glFinish (GPU idle = reads done). Nothing
     * of this waits for the vsync-blocking swap below: the client's buffer
     * turnaround is bounded by the GPU, not by the display. (wl_shm layers
     * were released at upload time — the GPU owns a copy.) */
    glDisable(GL_BLEND);
    int rel = awl_gl_fence_fd();
    if (rel < 0) glFinish();
    for (int i = 0; i < n; i++) awl_bufferqueue_put(head[i], rel);
    if (rel >= 0) close(rel);
    /* a newer frame arrived while we drew but was not complete yet: present
     * it as soon as the swap returns (the waiter hands buffers back, this
     * re-arm keeps the screen current) */
    if (more) awl_renderer_request_render(w->id);
    if (!eglSwapBuffers(awl_gl_display(), w->surface)) {
        LOGE("eglSwapBuffers: 0x%x", eglGetError());
        return;
    }
#ifdef AWL_LOG_DEBUG   /* two driver queries per swap — debug builds only (LOGD args alone would not evaluate them) */
    {
        EGLint sw = 0, sh = 0;
        eglQuerySurface(awl_gl_display(), w->surface, EGL_WIDTH, &sw);
        eglQuerySurface(awl_gl_display(), w->surface, EGL_HEIGHT, &sh);
        LOGD("win %llu swapped: egl surface %dx%d (view %dx%d)",
             (unsigned long long)w->id, sw, sh, vw, vh);
    }
#endif

    /* frame_done for each layer (child layers piggyback on the parent window's presentation) */
    for (int k = 0; k < nseen; k++)
        awl_surface_presented(seen[k]);
}

static void render_thread_loop(wl_window* w) {
    EGLDisplay dpy = awl_gl_display();
    if (!eglMakeCurrent(dpy, w->surface, w->surface, w->context)) {
        LOGE("window %llu render thread MakeCurrent: 0x%x",
             (unsigned long long)w->id, eglGetError());
        std::lock_guard<std::mutex> lk(w->m);
        w->stop = true;
        return;
    }
    /* vsync throttling: high-frequency commits from desync child layers (bubble) must not become unthrottled frame swaps */
    eglSwapInterval(dpy, 1);
    for (;;) {
        std::unique_lock<std::mutex> lk(w->m);
        w->cv.wait(lk, [&] { return w->stop || w->render_req; });
        bool stopping = w->stop;
        w->render_req = false;
        lk.unlock();
        if (stopping) break;
        render_frame(w);
    }
    eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
}

void awl_renderer_request_render(uint64_t id) {
    /* access w under the map lock: mutually exclusive with detach's removal (no use-after-free) */
    std::lock_guard<std::mutex> lk(g_map_lock);
    auto it = g_windows.find(id);
    if (it == g_windows.end()) return;   /* Activity not ready yet; request re-issued after attach */
    wl_window* w = it->second;
    std::lock_guard<std::mutex> lw(w->m);
    w->render_req = true;
    w->cv.notify_all();
}

/* Logic layer (awl_window_resize, any thread): the Android window resized in
 * place — the cached ANativeWindow size is stale. Drop it and wake the render
 * thread: the next frame re-queries and presents at the new size (the
 * original full-screen resize path). Coalesced like a render request. */
void awl_renderer_window_resized(uint64_t id) {
    std::lock_guard<std::mutex> lk(g_map_lock);
    auto it = g_windows.find(id);
    if (it == g_windows.end()) return;
    wl_window* w = it->second;
    w->size_dirty.store(true, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lw(w->m);
    w->render_req = true;
    w->cv.notify_all();
}

void awl_renderer_shutdown(void) {
    std::vector<wl_window*> wins;
    {
        std::lock_guard<std::mutex> lk(g_map_lock);
        for (auto& kv : g_windows) wins.push_back(kv.second);
        g_windows.clear();
    }
    for (wl_window* w : wins) {
        {   /* same as detach: stop+join, then release */
            std::lock_guard<std::mutex> lk(w->m);
            w->stop = true;
            w->cv.notify_all();
        }
        if (w->th.joinable()) w->th.join();
        window_teardown_gl(w);
        ANativeWindow_release(w->nw);
        delete w;
    }
    /* the EGL display is shared with the SC backend (awl_gl) — it lives with the process */
}
