/* awl_renderer.cpp — per-window GPU rendering (v3 multi-layer, no CPU
 * per-pixel compositing)
 *
 * wayland buffer → window:
 *   dmabuf : eglCreateImageKHR(EGL_EXT_image_dma_buf_import) → zero-copy texture
 *   shm    : wl_shm_buffer → glTexSubImage2D upload (GPU, damage region)
 * Composite = multi-layer quads (root + wl_subsurface child layers, render
 * stack order bottom→top, then the client's wl_pointer.set_cursor image on
 * top) sampled into dst rect → eglSwapBuffers → BufferQueue/SurfaceFlinger
 * present.
 * Per-layer texture state cached by surface id (wl_tex); reclaimed when the
 * layer disappears.
 * blend = premultiplied alpha (ONE, ONE_MINUS_SRC_ALPHA); first layer (root)
 * blend off.
 *
 * All rendering happens on the window's dedicated render thread
 * (render_thread_loop).
 */
#include "awl_renderer.hpp"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>   /* GL_BGRA_EXT etc. (gl3.h must come first for base types) */
#include <android/hardware_buffer.h>
#include <android/log.h>
#include <android/native_window.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>               /* dup / getpid */
#include <sys/socket.h>           /* socketpair / sendmsg / SCM_RIGHTS */
#include <sys/mman.h>             /* donor blob patching */
#include <fcntl.h>
#include <errno.h>
#include <wayland-server-core.h>   /* wl_shm_buffer_* */

/* Official VNDK API (vndk/hardware_buffer.h); no header in the NDK sysroot,
 * symbol exported by libnativewindow.so (already linked via CMake) */
extern "C" const struct native_handle* AHardwareBuffer_getNativeHandle(
    const AHardwareBuffer* buffer);
struct native_handle { int version; int numFds; int numInts; int data[]; };

#include <map>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <atomic>
#include <vector>

#define AWL_TAG "anland-rd"
#include "awl_log.h"   /* LOGI/LOGE/LOGD (LOGD compiled out unless AWL_LOG_DEBUG) */

/* ---------------- dmabuf → AHardwareBuffer (AOSP construction logic ported) ---- */

static const char* k_vert_src =
    "#version 300 es\n"
    "in vec2 pos;\n"
    "uniform vec2 u_view;   /* window px */\n"
    "uniform vec4 u_dst;    /* dst rect px: x,y=top-left w,h=size (Y down) */\n"
    "uniform vec4 u_uv;     /* sample region transform: uv = u_uv.xy + uv*u_uv.zw (#31 source) */\n"
    "out vec2 uv;\n"
    "void main() {\n"
    "  vec2 px = vec2(u_dst.x + (pos.x * 0.5 + 0.5) * u_dst.z,\n"
    "                 u_dst.y + (0.5 - pos.y * 0.5) * u_dst.w);\n"
    "  gl_Position = vec4(px.x / u_view.x * 2.0 - 1.0,\n"
    "                     1.0 - px.y / u_view.y * 2.0, 0.0, 1.0);\n"  /* flip Y */
    "  uv = u_uv.xy + vec2(pos.x * 0.5 + 0.5, 0.5 - pos.y * 0.5) * u_uv.zw;\n"
    "}\n";

static const char* k_frag_src =
    "#version 300 es\n"
    "precision mediump float;\n"
    "in vec2 uv;\n"
    "out vec4 color;\n"
    "uniform sampler2D tex;\n"
    "uniform bool u_swap_rb;\n"
    "void main() {\n"
    "  vec4 c = texture(tex, uv);\n"
    "  color = u_swap_rb ? c.bgra : c;\n"   /* dmabuf sampled in DRM fourcc byte order */
    "}\n";

/* Per-layer texture state (one per root / each wl_subsurface child layer, keyed by surface id) */
struct wl_tex {
    GLuint texture = 0;
    uint32_t tex_w = 0, tex_h = 0;
    bool tex_is_image = false;     /* external-memory texture (dmabuf) */
    EGLImageKHR image = EGL_NO_IMAGE_KHR;
    AHardwareBuffer* ahb = NULL;   /* registered AHB (wraps container dmabuf, released on destroy) */
    AHardwareBuffer* donor = NULL; /* blob donor (stays resident until ahb release, see below) */
    void* buf_token = NULL;        /* imported buffer identity (wayland resource value) */
};

struct wl_window {
    uint64_t id;
    ANativeWindow* nw;        /* self-held reference */
    EGLSurface surface = EGL_NO_SURFACE;
    EGLContext context = EGL_NO_CONTEXT;
    GLuint program = 0;
    GLuint vbo = 0;
    bool logged_frame = false;     /* first-frame log (diagnostics) */
    bool logged_dmg = false;       /* first partial-damage upload log (diagnostics) */
    std::map<uint64_t, wl_tex> layers;   /* layer id → texture (includes root's own id) */

    /* dedicated render thread: context bound 1:1 to the thread, requests coalesced via condvar */
    std::thread th;
    std::mutex m;
    std::condition_variable cv;
    bool stop = false;
    bool render_req = false;
};

static struct {
    EGLDisplay display = EGL_NO_DISPLAY;
    EGLConfig config = 0;
    EGLContext share_ctx = EGL_NO_CONTEXT;   /* precompiled shared program resources */
    bool inited = false;
    PFNEGLCREATEIMAGEKHRPROC eglCreateImageKHR;
    PFNEGLDESTROYIMAGEKHRPROC eglDestroyImageKHR;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC glEGLImageTargetTexture2DOES;
    PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC eglGetNativeClientBuffer;
} g;

static std::mutex g_map_lock;
static std::map<uint64_t, wl_window*> g_windows;

static void destroy_dmabuf_texture(wl_tex* t);   /* forward reference for window_teardown_gl */

/* ---------------- EGL global init ---------------- */

static bool egl_init(void) {
    if (g.inited) return true;
    /* Double-checked mutex: SURFACE can arrive concurrently from multiple binder
     * threads; failure leaves the flag unset — retried on the next attach (no
     * call_once: a failure there would lock up rendering forever) */
    static std::mutex init_lock;
    std::lock_guard<std::mutex> lk(init_lock);
    if (g.inited) return true;
    g.display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    if (g.display == EGL_NO_DISPLAY) {
        LOGE("eglGetDisplay failed");
        return false;
    }
    if (!eglInitialize(g.display, NULL, NULL)) {
        LOGE("eglInitialize: 0x%x", eglGetError());
        return false;
    }
    const char* exts = eglQueryString(g.display, EGL_EXTENSIONS);
    LOGI("EGL extensions: %s", exts ? exts : "none");
    bool has_dmabuf = exts && strstr(exts, "EGL_EXT_image_dma_buf_import");
    bool has_img2d = exts && strstr(exts, "EGL_KHR_gl_texture_2D_image");
    if (!has_dmabuf || !has_img2d)
        LOGE("!! dmabuf import %s / image2d %s",
             has_dmabuf ? "ok" : "MISSING", has_img2d ? "ok" : "MISSING");

    g.eglCreateImageKHR = (PFNEGLCREATEIMAGEKHRPROC)
        eglGetProcAddress("eglCreateImageKHR");
    g.eglDestroyImageKHR = (PFNEGLDESTROYIMAGEKHRPROC)
        eglGetProcAddress("eglDestroyImageKHR");
    g.glEGLImageTargetTexture2DOES = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)
        eglGetProcAddress("glEGLImageTargetTexture2DOES");
    g.eglGetNativeClientBuffer = (PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC)
        eglGetProcAddress("eglGetNativeClientBufferANDROID");
    if (!g.eglCreateImageKHR || !g.eglDestroyImageKHR ||
        !g.glEGLImageTargetTexture2DOES || !g.eglGetNativeClientBuffer) {
        LOGE("EGLImage procs missing");
        return false;
    }

    EGLint cfg_attr[] = {
        EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 0,
        EGL_NONE,
    };
    EGLint n = 0;
    if (!eglChooseConfig(g.display, cfg_attr, &g.config, 1, &n) || n < 1) {
        LOGE("eglChooseConfig: 0x%x n=%d", eglGetError(), n);
        return false;
    }
    g.inited = true;
    return true;
}

static GLuint compile(GLenum type, const char* src) {
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(sh, sizeof(log), NULL, log);
        LOGE("shader: %s", log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static bool window_setup_gl(wl_window* w) {
    const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    w->context = eglCreateContext(g.display, g.config, EGL_NO_CONTEXT, ctx_attr);
    if (w->context == EGL_NO_CONTEXT) {
        LOGE("eglCreateContext: 0x%x", eglGetError());
        return false;
    }
    w->surface = eglCreateWindowSurface(g.display, g.config,
                                        (EGLNativeWindowType)w->nw, NULL);
    if (w->surface == EGL_NO_SURFACE) {
        LOGE("eglCreateWindowSurface: 0x%x", eglGetError());
        return false;
    }
    if (!eglMakeCurrent(g.display, w->surface, w->surface, w->context)) {
        LOGE("eglMakeCurrent: 0x%x", eglGetError());
        return false;
    }

    GLuint vs = compile(GL_VERTEX_SHADER, k_vert_src);
    GLuint fs = compile(GL_FRAGMENT_SHADER, k_frag_src);
    if (!vs || !fs) return false;
    w->program = glCreateProgram();
    glAttachShader(w->program, vs);
    glAttachShader(w->program, fs);
    glLinkProgram(w->program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(w->program, GL_LINK_STATUS, &ok);
    if (!ok) {
        LOGE("program link failed");
        return false;
    }

    static const float quad[] = {
        -1, -1,  1, -1,  -1, 1,
        -1,  1,  1, -1,   1, 1,
    };
    glGenBuffers(1, &w->vbo);
    glBindBuffer(GL_ARRAY_BUFFER, w->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);

    /* layer textures created on demand (render_thread_loop, see wl_tex) */
    glViewport(0, 0, ANativeWindow_getWidth(w->nw), ANativeWindow_getHeight(w->nw));
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_BLEND);

    /* setup runs on the calling thread (binder pool), rendering on the window's dedicated thread — release current */
    eglMakeCurrent(g.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);

    LOGI("window %llu GL ready %dx%d", (unsigned long long)w->id,
         ANativeWindow_getWidth(w->nw), ANativeWindow_getHeight(w->nw));
    {
        const char* gle = (const char*)glGetString(GL_EXTENSIONS);
        LOGI("GL ext has memory_object_fd: %d",
             gle && strstr(gle, "GL_EXT_memory_object_fd") ? 1 : 0);
        const char* glv = (const char*)glGetString(GL_VERSION);
        LOGI("GL version: %s", glv ? glv : "?");
    }
    return true;
}

static void window_teardown_gl(wl_window* w) {
    if (w->context != EGL_NO_CONTEXT) {
        eglMakeCurrent(g.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (w->program) glDeleteProgram(w->program);
        if (w->vbo) glDeleteBuffers(1, &w->vbo);
        for (auto& kv : w->layers) {
            destroy_dmabuf_texture(&kv.second);
            if (kv.second.texture) glDeleteTextures(1, &kv.second.texture);
        }
        w->layers.clear();
        if (w->surface != EGL_NO_SURFACE) eglDestroySurface(g.display, w->surface);
        eglDestroyContext(g.display, w->context);
        w->context = EGL_NO_CONTEXT;
        w->surface = EGL_NO_SURFACE;
        w->program = w->vbo = 0;
    }
}

/* ---------------- public API ---------------- */

static void render_thread_loop(wl_window* w);   /* defined at end of file */

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
        if (!egl_init()) return -1;
        wl_window* w = new wl_window();
        w->id = id;
        ANativeWindow_acquire(nw);
        w->nw = nw;
        if (!window_setup_gl(w)) {      /* GL setup on the calling thread (binder pool, concurrent) */
            ANativeWindow_release(nw);
            delete w;
            return -1;
        }
        w->th = std::thread(render_thread_loop, w);   /* dedicated render thread */
        std::lock_guard<std::mutex> lk(g_map_lock);
        g_windows[id] = w;
        return 0;
    }
    return 0;
}

/* dmabuf → registered AHardwareBuffer — donor-blob supplies metadata
 * (device-verified scheme, see memory snapalloc-ahb-construction)
 *
 * QCOM snapalloc's importBuffer requires the handle to carry a vendor
 * descriptor:
 *   fd[0] = pixel dmabuf
 *   fd[1] = 28KB metadata blob (geometry ground truth; validateBufferSize
 *   checks STRIDE against it)
 * The container kgsl dmabuf lacks this descriptor → plain GB01+fd rejected
 * (error 2 / rc=5).
 *
 * Verified on device (/tmp/hswap*.c, /tmp/gpuverify.c):
 *   - blob mmap RW is writable, no per-buffer checksum
 *   - Retain does not verify the kernel binding between pixel fd and blob
 *   - a 4x4 mini donor's blob, fully patched, serves any geometry
 *     (stride/height/size/width all forgeable; GPU samples strictly by the
 *     patched stride — checkerboard 182528/182528)
 *
 * All calls go through official abstraction layers: AHardwareBuffer_allocate
 * (NDK) / AHardwareBuffer_getNativeHandle (VNDK) /
 * AHardwareBuffer_recvHandleFromUnixSocket (NDK, libs/ui/GraphicBuffer.cpp
 * flatten wire convention) / EGL_ANDROID_image_native_buffer.
 * Blob offsets are not hardcoded — self-calibrated at startup by diffing
 * two-geometry donors; on failure, report and refuse. */

struct ahb_calib {
    bool ok = false;
    uint32_t calib_fmt = 0;        /* HAL format used for calibration (recalibrated per format) */
    int blob_size = 0;            /* donor fd[1] byte count (measured 28672) */
    int num_ints = 0;             /* handle numInts (measured 34) */
    int stride_px_off[8], n_stride_px = 0;   /* blob offset: stride (pixels) */
    int stride_b_off[8],  n_stride_b = 0;    /* blob offset: stride (bytes) */
    int height_off[8],    n_height = 0;      /* blob offset: height */
    int size_off[8],      n_size = 0;        /* blob offset: allocated size (aligned, = pixel dmabuf size) */
    int size_exact_off[8], n_size_exact = 0; /* blob offset: exact size (stride*h*4) */
    int extent_off[8],    n_extent = 0;      /* blob offset: size+constant */
    long extent_const = 0;
    int idx_stride_px = -1;       /* handle ints index */
    int idx_height[2] = {-1, -1}; /* height appears twice (measured [3]/[5]) */
    int n_idx_height = 0;
    int idx_width = -1;
    int idx_size = -1;
    int idx_stride_b = -1;
};
/* One slot per HAL format (concurrent multi-window mixed formats must not evict
 * and force recalibration): with a single cache not keyed by fmt, chrome
 * (AR24→RGBA_8888) and Xwayland (XR24→RGBX_8888) render threads importing
 * alternately → full recalibration every frame (logs measured alternating
 * calib ok every 50-100ms). Each format calibrated once, no mutual eviction. */
static struct ahb_calib k_calibs[8];
static int k_n_calibs = 0;
static struct ahb_calib* calib_slot(uint32_t fmt) {
    for (int i = 0; i < k_n_calibs; i++)
        if (k_calibs[i].calib_fmt == fmt) return &k_calibs[i];
    if (k_n_calibs >= (int)(sizeof(k_calibs) / sizeof(k_calibs[0])))
        return NULL;
    struct ahb_calib* c = &k_calibs[k_n_calibs++];
    memset(c, 0, sizeof(*c));
    c->calib_fmt = fmt;
    return c;
}

/* Get the donor's blob; returns fd (-1 on failure) */
static int donor_blob_fd(const AHardwareBuffer* ahb) {
    const native_handle* nh = AHardwareBuffer_getNativeHandle(ahb);
    if (!nh || nh->numFds != 2) {
        LOGE("donor handle layout unexpected (numFds=%d) — not a snapalloc layout, refusing",
             nh ? nh->numFds : -1);
        return -1;
    }
    return nh->data[1];
}
static int donor_pixel_fd(const AHardwareBuffer* ahb) {
    const native_handle* nh = AHardwareBuffer_getNativeHandle(ahb);
    return nh ? nh->data[0] : -1;
}

/* Locate blob/ints offsets by diffing two-geometry donors (expected values all
 * taken from the donors' own describe; no stride rule assumed) */
static void calib_collect(const uint32_t* ba, const uint32_t* bb, long sz,
                          uint32_t ea, uint32_t eb,
                          int* offs, int* n, int max) {
    *n = 0;
    for (long o = 0; o + 4 <= sz && *n < max; o += 4)
        if (ba[o/4] == ea && bb[o/4] == eb && ea != eb)
            offs[(*n)++] = (int)o;
}
/* Calibrate the given HAL format (returns its slot; already-calibrated hits return immediately) */
static struct ahb_calib* ahb_calibrate(uint32_t fmt) {
    struct ahb_calib* kc = calib_slot(fmt);
    if (!kc) {
        LOGE("calib slots full (concurrent HAL formats >8) — refusing");
        return NULL;
    }
    if (kc->ok) return kc;

    const uint32_t W[2] = {300, 1134}, H[2] = {300, 567};
    AHardwareBuffer* d[2] = {NULL, NULL};
    uint32_t stride_px[2] = {0, 0}, size[2] = {0, 0};
    uint32_t* map[2] = {NULL, NULL};
    bool mapped[2] = {false, false};
    int bfd[2] = {-1, -1};
    long bsz = 0;
    bool ok = false;

    for (int i = 0; i < 2; i++) {
        AHardwareBuffer_Desc dd = {};
        dd.width = W[i]; dd.height = H[i];
        dd.format = fmt;
        dd.layers = 1; dd.usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;
        if (AHardwareBuffer_allocate(&dd, &d[i]) != 0) {
            LOGE("calib donor %d allocation failed", i);
            goto out;
        }
        AHardwareBuffer_Desc got;
        AHardwareBuffer_describe(d[i], &got);
        stride_px[i] = got.stride;
        /* blob has two size semantics (probe-measured, e.g. 300x300: alloc
         * 0x5e000 / exact 0x5dc00): alloc form = pixel dmabuf size, exact
         * form = stride*h*4 */
        size[i] = (uint32_t)lseek(donor_pixel_fd(d[i]), 0, SEEK_END);
    }

    for (int i = 0; i < 2; i++) {
        bfd[i] = donor_blob_fd(d[i]);
        if (bfd[i] < 0) goto out;
    }
    bsz = lseek(bfd[0], 0, SEEK_END);
    if (bsz <= 0 || bsz != lseek(bfd[1], 0, SEEK_END)) {
        LOGE("calib: donor blob size abnormal %ld", bsz);
        goto out;
    }
    for (int i = 0; i < 2; i++) {
        map[i] = (uint32_t*)mmap(NULL, bsz, PROT_READ, MAP_SHARED, bfd[i], 0);
        if (map[i] == MAP_FAILED) { LOGE("calib: blob mmap %s", strerror(errno)); goto out; }
        mapped[i] = true;
    }
    {
    const native_handle* nh[2] = {AHardwareBuffer_getNativeHandle(d[0]),
                                  AHardwareBuffer_getNativeHandle(d[1])};
    if (nh[0]->numInts != nh[1]->numInts || nh[0]->numInts > 64) {
        LOGE("calib: numInts mismatch %d/%d", nh[0]->numInts, nh[1]->numInts);
        goto out;
    }
    kc->num_ints = nh[0]->numInts;
    const int* ints[2] = {&nh[0]->data[2], &nh[1]->data[2]};

    /* blob fields (byte offsets) */
    calib_collect(map[0], map[1], bsz, stride_px[0], stride_px[1],
                  kc->stride_px_off, &kc->n_stride_px, 8);
    calib_collect(map[0], map[1], bsz, stride_px[0]*4, stride_px[1]*4,
                  kc->stride_b_off, &kc->n_stride_b, 8);
    calib_collect(map[0], map[1], bsz, H[0], H[1],
                  kc->height_off, &kc->n_height, 8);
    calib_collect(map[0], map[1], bsz, size[0], size[1],
                  kc->size_off, &kc->n_size, 8);
    calib_collect(map[0], map[1], bsz,
                  stride_px[0]*H[0]*4, stride_px[1]*H[1]*4,
                  kc->size_exact_off, &kc->n_size_exact, 8);
    /* extent-form fields: blob[k] - size is the same constant */
    kc->n_extent = 0;
    kc->extent_const = 0;
    for (long o = 0; o + 4 <= bsz && kc->n_extent < 8; o += 4) {
        long da = (long)map[0][o/4] - size[0], db = (long)map[1][o/4] - size[1];
        if (da == db && da > 0 && da < 0x100000) {
            kc->extent_off[kc->n_extent++] = (int)o;
            kc->extent_const = da;
        }
    }

    /* handle ints indices */
    kc->idx_stride_px = -1;
    kc->n_idx_height = 0;
    kc->idx_width = kc->idx_size = kc->idx_stride_b = -1;
    for (int k = 0; k < kc->num_ints; k++) {
        if (ints[0][k] == (int)stride_px[0] && ints[1][k] == (int)stride_px[1])
            kc->idx_stride_px = k;
        if (ints[0][k] == (int)H[0] && ints[1][k] == (int)H[1]) {
            if (kc->n_idx_height < 2) kc->idx_height[kc->n_idx_height++] = k;
        }
        if (ints[0][k] == (int)W[0] && ints[1][k] == (int)W[1])
            kc->idx_width = k;
        if (ints[0][k] == (int)size[0] && ints[1][k] == (int)size[1])
            kc->idx_size = k;
        if (ints[0][k] == (int)(stride_px[0]*4) && ints[1][k] == (int)(stride_px[1]*4))
            kc->idx_stride_b = k;
    }

    if (!kc->n_stride_px || !kc->n_stride_b || !kc->n_height ||
        !kc->n_size || !kc->n_size_exact || !kc->n_extent ||
        kc->idx_stride_px < 0 || !kc->n_idx_height ||
        kc->idx_width < 0 || kc->idx_size < 0 || kc->idx_stride_b < 0) {
        LOGE("calib failed: stride_px=%d stride_b=%d height=%d size=%d/%d extent=%d "
             "idx(spx=%d h=%d w=%d size=%d sb=%d) — vendor layout changed, refusing to forge",
             kc->n_stride_px, kc->n_stride_b, kc->n_height,
             kc->n_size, kc->n_size_exact, kc->n_extent,
             kc->idx_stride_px, kc->n_idx_height, kc->idx_width,
             kc->idx_size, kc->idx_stride_b);
        goto out;
    }
    kc->blob_size = (int)bsz;
    kc->calib_fmt = fmt;
    kc->ok = true;
    ok = true;
    LOGI("snapalloc calib ok (fmt=%u): blob=%dB ints=%d "
         "stride_px@%d,%d stride_b@%d,%d h@%d,%d size@%d,%d ext+0x%lx "
         "idx(spx=%d h=[%d,%d] w=%d size=%d sb=%d)",
         fmt, kc->blob_size, kc->num_ints,
         kc->stride_px_off[0], kc->stride_px_off[1],
         kc->stride_b_off[0], kc->stride_b_off[1],
         kc->height_off[0], kc->height_off[1],
         kc->size_off[0], kc->size_off[1], kc->extent_const,
         kc->idx_stride_px, kc->idx_height[0],
         kc->n_idx_height > 1 ? kc->idx_height[1] : -1,
         kc->idx_width, kc->idx_size, kc->idx_stride_b);
    }
out:
    for (int i = 0; i < 2; i++) {
        if (mapped[i]) munmap(map[i], bsz);
        if (d[i]) AHardwareBuffer_release(d[i]);
    }
    return ok ? kc : NULL;
}

/* Patch donor blob fields (mmap RW; offset table = the calibration slot for that HAL format) */
static bool donor_patch_blob(const struct ahb_calib* kc, int blob_fd,
                             uint32_t stride_px, uint32_t height, long size) {
    uint32_t* b = (uint32_t*)mmap(NULL, kc->blob_size,
                                  PROT_READ | PROT_WRITE, MAP_SHARED, blob_fd, 0);
    if (b == MAP_FAILED) {
        LOGE("donor blob mmap: %s", strerror(errno));
        return false;
    }
    for (int i = 0; i < kc->n_stride_px; i++)
        *(uint32_t*)((char*)b + kc->stride_px_off[i]) = stride_px;
    for (int i = 0; i < kc->n_stride_b; i++)
        *(uint32_t*)((char*)b + kc->stride_b_off[i]) = stride_px * 4;
    for (int i = 0; i < kc->n_height; i++)
        *(uint32_t*)((char*)b + kc->height_off[i]) = height;
    for (int i = 0; i < kc->n_size; i++)
        *(uint32_t*)((char*)b + kc->size_off[i]) = (uint32_t)size;
    for (int i = 0; i < kc->n_size_exact; i++)
        *(uint32_t*)((char*)b + kc->size_exact_off[i]) = (uint32_t)size;
    for (int i = 0; i < kc->n_extent; i++)
        *(uint32_t*)((char*)b + kc->extent_off[i]) = (uint32_t)(size + kc->extent_const);
    munmap(b, kc->blob_size);
    return true;
}

static AHardwareBuffer* ahb_wrap_dmabuf(const awl_buffer_info_t* b,
                                        uint32_t hal, uint64_t usage,
                                        AHardwareBuffer** donor_out) {
    /* k_calibs global table shared by multiple render threads (first import per
     * format triggers one calibration) — hold the lock throughout: calibration
     * + patching read consistently. import includes gralloc calls (ms-scale)
     * but happens only on buffer replacement; serialization is acceptable */
    static std::mutex wrap_lock;
    std::lock_guard<std::mutex> wlk(wrap_lock);

    struct ahb_calib* kc = ahb_calibrate(hal);
    if (!kc) {
        LOGE("blob offsets not calibrated — dmabuf import refused (no fallback)");
        return NULL;
    }

    /* Small 4x4 donor: with all blob fields patched it serves any geometry
     * (EXP-B verified on device). Format matches the target → fmt fields
     * inside handle/blob are automatically correct, no patching needed */
    AHardwareBuffer_Desc dd = {};
    dd.width = 4; dd.height = 4;
    dd.format = hal; dd.layers = 1;
    dd.usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;
    AHardwareBuffer* donor = NULL;
    if (AHardwareBuffer_allocate(&dd, &donor) != 0) {
        LOGE("donor allocation failed");
        return NULL;
    }
    const native_handle* nd = AHardwareBuffer_getNativeHandle(donor);
    if (!nd || nd->numFds != 2 || nd->numInts != kc->num_ints) {
        LOGE("donor layout drift (numFds=%d numInts=%d/%d)",
             nd ? nd->numFds : -1, nd ? nd->numInts : -1, kc->num_ints);
        AHardwareBuffer_release(donor);
        return NULL;
    }
    if (!donor_patch_blob(kc, nd->data[1], b->stride / 4, b->height,
                          (long)b->stride * b->height)) {
        AHardwareBuffer_release(donor);
        return NULL;
    }

    /* handle ints: donor template + target geometry */
    int ints[64];
    memcpy(ints, &nd->data[2], (size_t)kc->num_ints * 4);
    ints[kc->idx_stride_px] = (int)(b->stride / 4);
    for (int i = 0; i < kc->n_idx_height; i++)
        ints[kc->idx_height[i]] = (int)b->height;
    ints[kc->idx_width] = (int)b->width;
    ints[kc->idx_size] = (int)((long)b->stride * b->height);
    ints[kc->idx_stride_b] = (int)b->stride;

    /* GraphicBuffer::flatten wire (libs/ui/GraphicBuffer.cpp):
     * 13-int header + handle ints in the stream, numFds fds via SCM_RIGHTS */
    static std::atomic<uint32_t> counter{0};
    uint64_t id = ((uint64_t)getpid() << 32) | (counter++ & 0xffffffffu);
    int32_t head[13];
    head[0] = 0x47423031;                 /* 'GB01' */
    head[1] = (int32_t)b->width;
    head[2] = (int32_t)b->height;
    head[3] = (int32_t)(b->stride / 4);   /* declared stride = container's true row pitch (px) */
    head[4] = (int32_t)hal;
    head[5] = 1;                          /* layerCount */
    head[6] = (int32_t)usage;
    head[7] = (int32_t)(id >> 32);
    head[8] = (int32_t)id;
    head[9] = 0;                          /* generationNumber */
    head[10] = 2;                         /* numFds: pixel + blob */
    head[11] = kc->num_ints;
    head[12] = (int32_t)(usage >> 32);

    int pix = dup(b->fd);                 /* container dmabuf */
    int blb = dup(nd->data[1]);           /* donor metadata blob */
    if (pix < 0 || blb < 0) {
        LOGE("dup: %s", strerror(errno));
        if (pix >= 0) close(pix);
        if (blb >= 0) close(blb);
        AHardwareBuffer_release(donor);
        return NULL;
    }

    size_t total = (13 + kc->num_ints) * 4;
    int32_t* wire = (int32_t*)malloc(total);
    memcpy(wire, head, sizeof(head));
    memcpy(wire + 13, ints, (size_t)kc->num_ints * 4);

    int sv[2];
    AHardwareBuffer* out = NULL;
    int rc = -1;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0) {
        char cbuf[CMSG_SPACE(2 * sizeof(int))];
        struct iovec iov = { wire, total };
        struct msghdr msg = {};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = cbuf;
        msg.msg_controllen = sizeof(cbuf);
        struct cmsghdr* cmsg = CMSG_FIRSTHDR(&msg);
        cmsg->cmsg_level = SOL_SOCKET;
        cmsg->cmsg_type = SCM_RIGHTS;
        cmsg->cmsg_len = CMSG_LEN(2 * sizeof(int));
        int fds[2] = { pix, blb };
        memcpy(CMSG_DATA(cmsg), fds, sizeof(fds));
        msg.msg_controllen = cmsg->cmsg_len;

        if (sendmsg(sv[0], &msg, 0) >= 0)
            rc = AHardwareBuffer_recvHandleFromUnixSocket(sv[1], &out);
        else
            LOGE("sendmsg: %s", strerror(errno));
        close(sv[0]);
        close(sv[1]);
    } else {
        LOGE("socketpair: %s", strerror(errno));
    }
    free(wire);
    close(pix);
    close(blb);                           /* kernel already handed over to the peer */

    if (rc != 0 || !out) {
        LOGE("recvHandleFromUnixSocket rc=%d — importBuffer refused "
             "(%ux%u stride=%u hal=%u)", rc, b->width, b->height, b->stride, hal);
        if (out) AHardwareBuffer_release(out);
        AHardwareBuffer_release(donor);
        return NULL;
    }
    *donor_out = donor;                   /* blob referenced by out; stays alive until out is released */
    return out;
}

static void destroy_dmabuf_texture(wl_tex* t) {
    if (t->image != EGL_NO_IMAGE_KHR) {
        g.eglDestroyImageKHR(g.display, t->image);
        t->image = EGL_NO_IMAGE_KHR;
    }
    if (t->ahb) {
        AHardwareBuffer_release(t->ahb);   /* releases the reference on the donor blob fd */
        t->ahb = NULL;
    }
    if (t->donor) {
        AHardwareBuffer_release(t->donor); /* blob reclaimable only after ahb release */
        t->donor = NULL;
    }
    t->buf_token = NULL;
}

static void tex_params_default(void) {
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
}

static bool import_dmabuf_texture(wl_tex* t, const awl_buffer_info_t* b) {
    /* Same buffer already imported (token = wayland resource identity; fd
     * numbers get recycled, unusable as a key) — the texture IS that memory;
     * client writes update it in place */
    if (t->tex_is_image && t->buf_token == b->token &&
        t->tex_w == b->width && t->tex_h == b->height)
        return true;

    destroy_dmabuf_texture(t);      /* destroy the EGLImage before releasing the AHB */

    /* HAL format: DRM ARGB8888→RGBA_8888(1), XR24→RGBX_8888(2);
     * sampled-only usage (texture) — GPU_FRAMEBUFFER not declared */
    uint32_t hal = b->drm_format == AWL_FOURCC_ARGB8888 ? 1 : 2;
    uint64_t usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;

    AHardwareBuffer* donor = NULL;
    AHardwareBuffer* ahb = ahb_wrap_dmabuf(b, hal, usage, &donor);
    if (!ahb) return false;

    /* eglGetNativeClientBufferANDROID: AHB → EGLClientBuffer (the sanctioned path) */
    EGLClientBuffer cb = g.eglGetNativeClientBuffer(ahb);
    if (!cb) {
        LOGE("eglGetNativeClientBuffer == NULL");
        AHardwareBuffer_release(ahb);
        return false;
    }
    EGLImageKHR img = g.eglCreateImageKHR(g.display, EGL_NO_CONTEXT,
                                          EGL_NATIVE_BUFFER_ANDROID, cb, NULL);
    if (img == EGL_NO_IMAGE_KHR) {
        LOGE("eglCreateImageKHR(native buffer): 0x%x (%ux%u stride=%u)",
             eglGetError(), b->width, b->height, b->stride);
        AHardwareBuffer_release(ahb);
        return false;
    }
    t->ahb = ahb;
    t->donor = donor;
    t->buf_token = b->token;
    LOGD("AHB import ok %ux%u stride=%u fd=%d",
         b->width, b->height, b->stride, b->fd);

    if (t->texture) glDeleteTextures(1, &t->texture);
    glGenTextures(1, &t->texture);
    glBindTexture(GL_TEXTURE_2D, t->texture);
    tex_params_default();
    g.glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, img);
    GLenum terr = glGetError();
    if (terr != GL_NO_ERROR) LOGE("EGLImageTargetTexture: 0x%x", terr);
    t->image = img;
    t->tex_is_image = true;
    t->tex_w = b->width;
    t->tex_h = b->height;
    return true;
}

/* shm → GL texture upload (GPU, BGRA passed through directly).
 * Damage dispatch (awl_surface_get_damage):
 *   NONE + same buffer          → skip the upload entirely (re-render caused
 *                                 by a cursor/layer move — texture is current)
 *   RECT + same buffer + gen    → glTexSubImage2D of the bbox only
 *   FULL / token mismatch / size change → full upload; token mismatch does
 *                                 NOT consume (the damage belongs to the
 *                                 newer buffer — the next frame uploads it) */
struct shm_damage {
    int state;
    int32_t x, y, w, h;
    void* token;
    uint32_t gen;
};

static bool upload_shm_texture(wl_window* win, uint64_t sid, wl_tex* t,
                               const awl_buffer_info_t* b,
                               const shm_damage* d) {
    if (b->drm_format != AWL_FOURCC_ARGB8888 && b->drm_format != AWL_FOURCC_XRGB8888) {
        LOGE("shm format 0x%08x unsupported", b->drm_format);
        return false;
    }
    bool same_buf = d->token == b->token;
    bool need_full = t->tex_is_image || t->tex_w != b->width || t->tex_h != b->height
                     || d->state == AWL_DMG_FULL || !same_buf;
    if (!need_full && d->state == AWL_DMG_NONE) {
        /* nothing changed on this layer since the last upload (the render was
         * requested by a cursor move / another layer) */
        return true;
    }
    void* data = wl_shm_buffer_get_data(b->shm);
    if (!data) return false;
    wl_shm_buffer_begin_access(b->shm);
    if (!t->texture) {
        glGenTextures(1, &t->texture);
        glBindTexture(GL_TEXTURE_2D, t->texture);
        tex_params_default();
    } else {
        glBindTexture(GL_TEXTURE_2D, t->texture);
    }
    /* row pitch from the wl_shm-reported stride (pixels) — also correct for non-tight layouts */
    glPixelStorei(GL_UNPACK_ROW_LENGTH, wl_shm_buffer_get_stride(b->shm) / 4);
    if (t->tex_is_image) {
        /* switch back from an external-memory texture to a regular uploaded texture */
        destroy_dmabuf_texture(t);
        t->tex_is_image = false;
        t->tex_w = 0;
    }
    if (t->tex_w != b->width || t->tex_h != b->height) {
        /* GL_EXT_texture_format_BGRA8888: internalformat must also be
         * GL_BGRA_EXT (RGBA+BGRA is absent from ES3 core's valid combination
         * table — yields GL_INVALID_OPERATION black screen) */
        glTexImage2D(GL_TEXTURE_2D, 0, GL_BGRA_EXT, (GLsizei)b->width, (GLsizei)b->height,
                     0, GL_BGRA_EXT, GL_UNSIGNED_BYTE, data);
        t->tex_w = b->width;
        t->tex_h = b->height;
    } else if (need_full) {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, (GLsizei)b->width, (GLsizei)b->height,
                        GL_BGRA_EXT, GL_UNSIGNED_BYTE, data);
    } else {
        /* damage bbox only: clamp to the buffer (clients may over-draw), then
         * upload the sub-rect via the row-length pitch + pointer offset */
        int32_t x = d->x < 0 ? 0 : d->x;
        int32_t y = d->y < 0 ? 0 : d->y;
        int32_t w = d->w, h = d->h;
        if (x > (int32_t)b->width) x = (int32_t)b->width;
        if (y > (int32_t)b->height) y = (int32_t)b->height;
        if (w > (int32_t)b->width - x) w = (int32_t)b->width - x;
        if (h > (int32_t)b->height - y) h = (int32_t)b->height - y;
        if (w > 0 && h > 0) {
            int32_t stride = wl_shm_buffer_get_stride(b->shm);
            const void* src = (const char*)data + (size_t)y * (size_t)stride
                            + (size_t)x * 4;
            glTexSubImage2D(GL_TEXTURE_2D, 0, x, y, w, h,
                            GL_BGRA_EXT, GL_UNSIGNED_BYTE, src);
            if (!win->logged_dmg && (w < (int32_t)b->width || h < (int32_t)b->height)) {
                win->logged_dmg = true;
                LOGI("window %llu layer %llu partial damage upload %dx%d@%d,%d (buffer %ux%u)",
                     (unsigned long long)win->id, (unsigned long long)sid,
                     w, h, x, y, b->width, b->height);
            }
        }
        /* empty rect after clamp: texture unchanged — damage still consumed */
    }
    GLenum up_err = glGetError();
    if (up_err != GL_NO_ERROR)
        LOGE("shm upload glerr=0x%x (%ux%u)", up_err, b->width, b->height);
    wl_shm_buffer_end_access(b->shm);
    /* consume only when the snapshot still describes the current buffer
     * (mismatch = a commit raced: keep its damage for the next frame) */
    if (same_buf)
        awl_surface_damage_consumed(sid, d->token, d->gen);
    return true;
}

/* ---------------- per-window render thread ----------------
 * context stays resident on this thread (MakeCurrent once); requests
 * coalesced/deduped; glFinish/swap block only this window — multi-window
 * parallelism */

static void render_frame(wl_window* w) {
    /* Layer snapshot (root first, child layers in render stack order bottom→top);
     * layers that fail to fetch / have no buffer are skipped.
     * #31 zoom: coordinates/sizes are root logical pixels. #34: dst goes
     * through the unified awl_view_map (×s + centered offset o per
     * scale_mode), keeping the geometry-origin alignment (chrome buffer
     * carries 16/10px shadow margins, sharing the same origin as input
     * mapping). */
    awl_layer_info_t lay[AWL_MAX_LAYERS + 1];   /* +1: client cursor image appended on top */
    int n = awl_surface_get_layers(w->id, lay, AWL_MAX_LAYERS);
    if (n <= 0) return;
    /* wl_pointer.set_cursor image of the pointer-focused client: composited
     * above every layer of this window (x,y = pointer − hotspot in the same
     * root logical coordinates as the stack; never part of hit-testing).
     * Drawn/presented like any layer → the cursor surface gets frame_done
     * (animated cursors) and deferred buffer release. */
    if (awl_pointer_cursor_layer(w->id, &lay[n])) n++;
    int32_t gox = 0, goy = 0;
    float gw = 0, gh = 0;
    awl_surface_get_origin(w->id, &gox, &goy, &gw, &gh);

    int vw = ANativeWindow_getWidth(w->nw);
    int vh = ANativeWindow_getHeight(w->nw);
    glViewport(0, 0, vw, vh);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);

    /* Content basis (geometry rectangle; chrome dst includes shadow margins →
     * out-of-bounds clipped; fixed-size clients have no valid geometry →
     * root logical size) → window view rect through the unified awl_view_map
     * (#34 scale_mode): stretch fills each axis, fit/center scale uniformly
     * with a centered offset — the bars around the content stay the clear
     * color above. Degenerate basis → identity map. */
    float bw = gw > 0.5f ? gw : (lay[0].w > 0.5f ? lay[0].w : 0.0f);
    float bh = gh > 0.5f ? gh : (lay[0].h > 0.5f ? lay[0].h : 0.0f);
    float rsx, rsy, rox, roy;
    awl_view_map(awl_display_scale_mode(), (float)vw, (float)vh, bw, bh,
                 &rsx, &rsy, &rox, &roy);

    glUseProgram(w->program);
    GLint loc = glGetAttribLocation(w->program, "pos");
    glBindBuffer(GL_ARRAY_BUFFER, w->vbo);
    glEnableVertexAttribArray((GLuint)loc);
    glVertexAttribPointer((GLuint)loc, 2, GL_FLOAT, GL_FALSE, 0, 0);
    glUniform2f(glGetUniformLocation(w->program, "u_view"), (float)vw, (float)vh);
    GLint dst_loc = glGetUniformLocation(w->program, "u_dst");
    GLint uv_loc = glGetUniformLocation(w->program, "u_uv");
    glUniform1i(glGetUniformLocation(w->program, "tex"), 0);
    GLint swap_loc = glGetUniformLocation(w->program, "u_swap_rb");
    glActiveTexture(GL_TEXTURE0);

    uint64_t seen[AWL_MAX_LAYERS + 1];
    int nseen = 0;
    bool drew = false;
    for (int i = 0; i < n; i++) {
        awl_buffer_info_t b;
        if (awl_surface_get_buffer(lay[i].surface_id, &b) != 0) continue;
        bool is_dmabuf = b.kind == AWL_BUFFER_DMABUF;
        wl_tex& t = w->layers[lay[i].surface_id];   /* layers seen this frame */
        seen[nseen++] = lay[i].surface_id;
        bool ok = false;
        if (is_dmabuf) {
            ok = import_dmabuf_texture(&t, &b);
        } else if (b.kind == AWL_BUFFER_SHM) {
            shm_damage d;
            d.state = awl_surface_get_damage(lay[i].surface_id,
                                             &d.x, &d.y, &d.w, &d.h,
                                             &d.token, &d.gen);
            ok = upload_shm_texture(w, lay[i].surface_id, &t, &b, &d);
        }
        if (is_dmabuf) close(b.fd);   /* import holds its own reference internally; return the dup when done */
        if (b.kind == AWL_BUFFER_SHM && b.shm) {
            /* Return the pinned references (taken by get_buffer) — after return
             * the shm/pool pointers are dead and must not be touched (safe even
             * if the client destroys the buffer during upload, see awl.h) */
            wl_shm_buffer_unref(b.shm);
            wl_shm_pool_unref(b.pool);
        }
        if (!ok) continue;

        /* first layer (root) writes directly with blend off; child layers stack on top with premultiplied alpha */
        if (i == 0 || !drew) glDisable(GL_BLEND);
        else {
            glEnable(GL_BLEND);
            glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        }
        /* dmabuf byte-order fix: DRM AR24/XR24 memory order B,G,R,(A|X) (fourcc
         * naming refers to the 32-bit little-endian word), but the AHB is sampled
         * as HAL RGBA_8888/RGBX_8888 (R,G,B,X memory order) → R/B swapped;
         * swizzle back after sampling. shm path: GL already converts to RGBA on
         * GL_BGRA_EXT upload, no swap (chrome+Xwayland showed inverted colors
         * on device, 2026-09-09) */
        glUniform1i(swap_loc, is_dmabuf ? 1 : 0);
        glUniform4f(dst_loc, ((float)lay[i].x - (float)gox) * rsx + rox,
                    ((float)lay[i].y - (float)goy) * rsy + roy,
                    lay[i].w * rsx, lay[i].h * rsy);
        glUniform4f(uv_loc, lay[i].u0, lay[i].v0, lay[i].su, lay[i].sv);
        glBindTexture(GL_TEXTURE_2D, t.texture);
        glDrawArrays(GL_TRIANGLES, 0, 6);
        drew = true;

        if (!w->logged_frame) {
            w->logged_frame = true;
            LOGI("window %llu frame: layers=%d [0]=%llu kind=%s %ux%u stride=%u "
                 "fmt=%c%c%c%c (glerr=0x%x)",
                 (unsigned long long)w->id, n,
                 (unsigned long long)lay[i].surface_id,
                 is_dmabuf ? "dmabuf" : "shm",
                 b.width, b.height, b.stride,
                 (char)(b.drm_format & 0xff), (char)((b.drm_format >> 8) & 0xff),
                 (char)((b.drm_format >> 16) & 0xff), (char)((b.drm_format >> 24) & 0xff),
                 glGetError());
        }
    }
    if (!drew) return;   /* not even root has a usable buffer — don't spin on a swap */

    /* vanished layers (bubble hidden/destroyed): reclaim texture and AHB references */
    for (auto it = w->layers.begin(); it != w->layers.end();) {
        bool found = false;
        for (int k = 0; k < nseen && !found; k++)
            found = (seen[k] == it->first);
        if (!found) {
            destroy_dmabuf_texture(&it->second);
            if (it->second.texture) glDeleteTextures(1, &it->second.texture);
            it = w->layers.erase(it);
        } else {
            ++it;
        }
    }

    /* ensure sampling finished before releasing the client buffer (correctness first; switch to a fence later) */
    glFinish();
    glDisable(GL_BLEND);

    if (!eglSwapBuffers(g.display, w->surface)) {
        LOGE("eglSwapBuffers: 0x%x", eglGetError());
        return;
    }

    /* frame_done for each layer (child layers piggyback on the parent window's presentation) */
    for (int k = 0; k < nseen; k++)
        awl_surface_presented(seen[k]);
}

static void render_thread_loop(wl_window* w) {
    if (!eglMakeCurrent(g.display, w->surface, w->surface, w->context)) {
        LOGE("window %llu render thread MakeCurrent: 0x%x",
             (unsigned long long)w->id, eglGetError());
        std::lock_guard<std::mutex> lk(w->m);
        w->stop = true;
        return;
    }
    /* vsync throttling: high-frequency commits from desync child layers (bubble) must not become unthrottled frame swaps */
    eglSwapInterval(g.display, 1);
    for (;;) {
        std::unique_lock<std::mutex> lk(w->m);
        w->cv.wait(lk, [&] { return w->stop || w->render_req; });
        bool stopping = w->stop;
        w->render_req = false;
        lk.unlock();
        if (stopping) break;
        render_frame(w);
    }
    eglMakeCurrent(g.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
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
    if (g.display != EGL_NO_DISPLAY) {
        eglTerminate(g.display);
        g.display = EGL_NO_DISPLAY;
    }
    g.inited = false;
}
