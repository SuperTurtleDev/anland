/* awl_hwc.cpp — subsurface-family child layers composed by
 * SurfaceFlinger/HWC (all-HWC by design: no GL fallback)
 *
 * One ASurfaceControl child per layer (wl_subsurface + xdg_popup riding the
 * same tree + the wl_pointer.set_cursor image), the buffer handed straight to
 * SF in a single atomic transaction:
 *   dmabuf : zero-copy — the forged AHB (awl_renderer_wrap_dmabuf_ahb) IS the
 *            client dmabuf; SF/HWC samples it directly
 *   shm    : copied into a locally allocated AHB (wl_shm B,G,R,A/X byte order
 *            == HAL BGRA_8888, plain row memcpy), 3-slot ring so the slot SF
 *            latched this frame is never the one being memcpy'd into
 * Root stays on the EGL path — the window Surface's BufferQueue IS the root
 * layer; SC children stack above the window buffer, same order as the
 * Wayland stack (root at the bottom).
 *
 * wl_buffer release, two runtime paths (a probe, not a fallback):
 *   WithRelease present (A36, dlsym — compile platform 35): dmabuf setBuffer
 *     carries OnBufferRelease (ctx = {surface id, buffer token} values, no
 *     pointers) → wait the release fence → awl_surface_release_token for that
 *     exact buffer; frame callbacks go through awl_surface_frame_done.
 *   absent (A12–A15) / shm layers: OnComplete → awl_surface_presented (frame
 *     callbacks + conservative release_q drain — the model the GL path runs
 *     on; per the header, buffers replaced by the transaction invoking
 *     OnComplete may be reused at that point).
 * Our AHB/donor refs of in-flight buffers may be dropped at any time: SF
 * holds its own strong refs and kernel fd refcounts pin the memory.
 *
 * Acquire fences are -1: the client's GPU writes to the dmabuf are ordered by
 * the dma-buf resv implicit sync (same model as the GL path's post-swap
 * release), which SF's GPU composition honors.
 *
 * Threading: entry points run on the window's render thread; SF callbacks
 * arrive on arbitrary binder threads and touch only ids/tokens through the
 * logic layer's any-thread APIs.
 */
#include "awl_hwc.hpp"

#include "awl_renderer.hpp"   /* awl_renderer_wrap_dmabuf_ahb + fourccs */

#include <android/hardware_buffer.h>
#include <android/native_window.h>
#include <android/rect.h>
#include <android/surface_control.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-server-core.h>   /* wl_shm_buffer_* */

#include <map>
#include <vector>

#define AWL_TAG "anland-rd"
#include "awl_log.h"

/* ---------------- WithRelease probe (A36; symbol not in the 35 platform) ---- */

typedef void (*awl_set_buf_release_fn)(ASurfaceTransaction*, ASurfaceControl*,
                                       AHardwareBuffer*, int, void*,
                                       ASurfaceTransaction_OnBufferRelease);
static awl_set_buf_release_fn g_set_buf_release;

/* wl_output.transform (surface set_buffer_transform, applied by the compositor
 * inverted — the buffer holds content rotated by T) → NATIVE_WINDOW_TRANSFORM
 * (native_window.h). wl rotates CCW; Android ROTATE_90 is clockwise, i.e. the
 * undo transform. Combined entries: 90° then mirror (wl applies flip first).
 * Direction verified against the spec; on-device check = weston-transformed —
 * if 90/270 come out swapped, flip 0x04↔0x06 / 0x05↔0x07 here. */
static const int32_t k_wl_xform[8] = {
    0,     /* 0 normal */
    0x04,  /* 1 90      → ROTATE_90 */
    0x03,  /* 2 180     → ROTATE_180 */
    0x06,  /* 3 270     → ROTATE_270 */
    0x01,  /* 4 flipped → MIRROR_HORIZONTAL */
    0x05,  /* 5 flipped_90 */
    0x02,  /* 6 flipped_180 → MIRROR_VERTICAL */
    0x07,  /* 7 flipped_270 */
};

/* ---------------- per-layer state (render thread only) ---------------- */

struct sc_layer {
    ASurfaceControl* sc = NULL;
    /* last applied state — unchanged layers emit no ops (steady-state renders
     * only touch whatever actually moved, e.g. the cursor) */
    void* last_token = NULL;
    int visible = 0;
    int32_t last_z = -1;
    int32_t last_xform = -1;
    int last_opaque = -1;
    ARect last_src = {-1, -1, -1, -1};
    ARect last_dst = {-1, -1, -1, -1};
    /* dmabuf token → forged AHB ring (client buffer rings reuse tokens; a
     * re-import is ms-scale). FIFO eviction — SF refs cover in-flight ones. */
    struct dmb_slot { void* token; AHardwareBuffer* ahb; AHardwareBuffer* donor; };
    dmb_slot dmb[4];
    int ndmb = 0;
    /* shm local AHBs, rotated per setBuffer (never overwrite the slot SF just
     * latched; 3 slots = 2 frames of slack) */
    AHardwareBuffer* sm_ahb[3] = {NULL, NULL, NULL};
    int sm_slot = 0;
    uint32_t sm_w = 0, sm_h = 0;
    int logged = 0;   /* first-present diagnostics */
};

struct awl_hwc_window {
    ANativeWindow* nw;
    std::map<uint64_t, sc_layer> layers;
};

static void layer_free(sc_layer* l) {
    if (l->sc) ASurfaceControl_release(l->sc);
    for (int k = 0; k < l->ndmb; k++) {
        AHardwareBuffer_release(l->dmb[k].ahb);
        if (l->dmb[k].donor) AHardwareBuffer_release(l->dmb[k].donor);
    }
    for (int k = 0; k < 3; k++)
        if (l->sm_ahb[k]) AHardwareBuffer_release(l->sm_ahb[k]);
}

/* ---------------- SF callbacks (arbitrary threads; ids/tokens only) -------- */

struct oncomplete_ctx {
    int n;
    struct { uint64_t id; int precise; } e[AWL_MAX_LAYERS];
};

static void on_complete(void* cctx_, ASurfaceTransactionStats* stats) {
    (void)stats;
    struct oncomplete_ctx* c = (struct oncomplete_ctx*)cctx_;
    if (!c) return;
    for (int i = 0; i < c->n; i++) {
        if (c->e[i].precise)
            awl_surface_frame_done(c->e[i].id);      /* release handled per buffer */
        else
            awl_surface_presented(c->e[i].id);       /* + conservative release_q drain */
    }
    free(c);
}

/* release-fence waiter: one daemon-wide thread + pipe. OnBufferRelease must
 * not block its SF binder thread, but wl_buffer.release may only go out once
 * the display stopped reading the buffer — the fence says when. poll() on a
 * sync_file fd is the classic libsync wait. */
struct rel_payload {
    uint64_t sid;
    void* token;
    int fd;
};

static int g_rel_rd = -1;
static int g_rel_wr = -1;
static pthread_mutex_t g_rel_lock = PTHREAD_MUTEX_INITIALIZER;

static void* rel_thread(void* arg) {
    (void)arg;
    int rd = g_rel_rd;
    for (;;) {
        struct rel_payload p;
        ssize_t r = read(rd, &p, sizeof(p));
        if (r != (ssize_t)sizeof(p)) {
            if (r < 0 && errno == EINTR) continue;
            break;
        }
        struct pollfd pf = { p.fd, POLLIN, 0 };
        while (poll(&pf, 1, 100) < 0 && errno == EINTR) {}
        close(p.fd);
        awl_surface_release_token(p.sid, p.token);
    }
    return NULL;
}

static bool queue_fence_wait(uint64_t sid, void* token, int fd) {
    pthread_mutex_lock(&g_rel_lock);
    if (g_rel_rd < 0) {
        int sv[2];
        if (pipe2(sv, O_CLOEXEC) != 0) {
            pthread_mutex_unlock(&g_rel_lock);
            return false;
        }
        g_rel_rd = sv[0];
        g_rel_wr = sv[1];
        pthread_t t;
        if (pthread_create(&t, NULL, rel_thread, NULL) != 0) {
            g_rel_rd = g_rel_wr = -1;
            close(sv[0]);
            close(sv[1]);
            pthread_mutex_unlock(&g_rel_lock);
            return false;
        }
        pthread_detach(t);
    }
    int wr = g_rel_wr;
    pthread_mutex_unlock(&g_rel_lock);
    struct rel_payload p = { sid, token, fd };
    return write(wr, &p, sizeof(p)) == (ssize_t)sizeof(p);
}

struct relbuf_ctx {
    uint64_t sid;
    void* token;
};

static void on_buffer_release(void* rctx_, int release_fence_fd) {
    struct relbuf_ctx* r = (struct relbuf_ctx*)rctx_;
    if (!r) return;
    uint64_t sid = r->sid;
    void* token = r->token;
    free(r);
    if (release_fence_fd < 0) {
        awl_surface_release_token(sid, token);
        return;
    }
    if (!queue_fence_wait(sid, token, release_fence_fd)) {
        close(release_fence_fd);
        awl_surface_release_token(sid, token);   /* degraded: no fence wait */
    }
}

/* ---------------- buffer materialization ---------------- */

static AHardwareBuffer* dmb_lookup(sc_layer* l, void* token) {
    for (int k = 0; k < l->ndmb; k++)
        if (l->dmb[k].token == token) return l->dmb[k].ahb;
    return NULL;
}

static void dmb_push(sc_layer* l, void* token, AHardwareBuffer* ahb,
                     AHardwareBuffer* donor) {
    if (l->ndmb < 4) {
        l->dmb[l->ndmb].token = token;
        l->dmb[l->ndmb].ahb = ahb;
        l->dmb[l->ndmb].donor = donor;
        l->ndmb++;
        return;
    }
    /* ring full (client buffer ring > 4): drop the oldest import */
    AHardwareBuffer_release(l->dmb[0].ahb);
    if (l->dmb[0].donor) AHardwareBuffer_release(l->dmb[0].donor);
    memmove(&l->dmb[0], &l->dmb[1], 3 * sizeof(l->dmb[0]));
    l->dmb[3].token = token;
    l->dmb[3].ahb = ahb;
    l->dmb[3].donor = donor;
}

/* shm wl_buffer → the next ring slot of a local AHB (full copy — the wl_shm
 * mapping is only valid while our pinned refs live). Returns the AHB now set
 * for setBuffer. */
static AHardwareBuffer* shm_copy(sc_layer* l, const awl_buffer_info_t* b) {
    if (l->sm_w != b->width || l->sm_h != b->height) {
        for (int k = 0; k < 3; k++) {
            if (l->sm_ahb[k]) AHardwareBuffer_release(l->sm_ahb[k]);
            l->sm_ahb[k] = NULL;
        }
        l->sm_w = b->width;
        l->sm_h = b->height;
        l->sm_slot = 0;
    }
    int slot = l->sm_slot;
    AHardwareBuffer* ahb = l->sm_ahb[slot];
    if (!ahb) {
        AHardwareBuffer_Desc d = {};
        d.width = b->width;
        d.height = b->height;
        d.format = AWL_HAL_BGRA_8888;
        d.layers = 1;
        d.usage = AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN |
                  AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE;
        if (AHardwareBuffer_allocate(&d, &ahb) != 0 || !ahb) {
            LOGE("shm AHB allocate failed (%ux%u)", b->width, b->height);
            return NULL;
        }
        l->sm_ahb[slot] = ahb;
    }
    l->sm_slot = (slot + 1) % 3;

    void* va = NULL;
    if (AHardwareBuffer_lock(ahb, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN,
                             -1, NULL, &va) != 0 || !va) {
        LOGE("shm AHB lock failed");
        return NULL;
    }
    AHardwareBuffer_Desc got;
    AHardwareBuffer_describe(ahb, &got);
    const void* src = wl_shm_buffer_get_data(b->shm);
    if (src) {
        wl_shm_buffer_begin_access(b->shm);
        uint32_t src_stride = (uint32_t)wl_shm_buffer_get_stride(b->shm);
        for (uint32_t y = 0; y < b->height; y++)
            memcpy((char*)va + (size_t)y * (size_t)got.stride * 4,
                   (const char*)src + (size_t)y * (size_t)src_stride,
                   (size_t)b->width * 4);
        wl_shm_buffer_end_access(b->shm);
    }
    AHardwareBuffer_unlock(ahb, NULL);
    return ahb;
}

/* viewport source (u0..sv normalized) → buffer-px rect, rounded outward,
 * clamped to the buffer, kept ≥1px (setGeometry requires non-empty source) */
static ARect layer_src_rect(const awl_layer_info_t* li,
                            const awl_buffer_info_t* b) {
    ARect r;
    int32_t bw = (int32_t)b->width;
    int32_t bh = (int32_t)b->height;
    r.left = (int32_t)floorf(li->u0 * (float)bw);
    r.top = (int32_t)floorf(li->v0 * (float)bh);
    r.right = (int32_t)ceilf((li->u0 + li->su) * (float)bw);
    r.bottom = (int32_t)ceilf((li->v0 + li->sv) * (float)bh);
    if (r.left < 0) r.left = 0;
    if (r.top < 0) r.top = 0;
    if (r.right > bw) r.right = bw;
    if (r.bottom > bh) r.bottom = bh;
    if (r.right <= r.left) r.right = r.left + 1;
    if (r.bottom <= r.top) r.bottom = r.top + 1;
    return r;
}

/* ---------------- public API ---------------- */

awl_hwc_window* awl_hwc_create(ANativeWindow* nw) {
    if (!nw) return NULL;
    /* WithRelease probe (once): precise per-buffer release on A16, OnComplete
     * model below A16 / for shm layers */
    static bool probed = false;
    if (!probed) {
        probed = true;
        g_set_buf_release = (awl_set_buf_release_fn)dlsym(
            RTLD_DEFAULT, "ASurfaceTransaction_setBufferWithRelease");
        LOGI("setBufferWithRelease %s",
             g_set_buf_release ? "available" : "absent (OnComplete release path)");
    }
    awl_hwc_window* h = new awl_hwc_window();
    ANativeWindow_acquire(nw);
    h->nw = nw;
    return h;
}

void awl_hwc_destroy(awl_hwc_window* h) {
    if (!h) return;
    for (auto& kv : h->layers) layer_free(&kv.second);
    ANativeWindow_release(h->nw);
    delete h;
}

/* Hide a snapshot layer that has nothing to show this frame (no buffer /
 * degenerate size / materialization failure) — keep the entry and its SF
 * state, just stop displaying the stale buffer. */
static void hide_layer(awl_hwc_window* h, ASurfaceTransaction* txn,
                       uint64_t sid, bool* any) {
    auto it = h->layers.find(sid);
    if (it == h->layers.end() || !it->second.visible) return;
    if (it->second.sc)
        ASurfaceTransaction_setVisibility(txn, it->second.sc,
                                          ASURFACE_TRANSACTION_VISIBILITY_HIDE);
    it->second.visible = 0;
    *any = true;
}

void awl_hwc_frame(awl_hwc_window* h, const awl_layer_info_t* lay, int n,
                   int32_t gox, int32_t goy,
                   float sx, float sy, float ox, float oy) {
    if (!h) return;
    ASurfaceTransaction* txn = ASurfaceTransaction_create();
    if (!txn) return;
    struct oncomplete_ctx* cctx =
        (struct oncomplete_ctx*)calloc(1, sizeof(*cctx));
    bool any = false;   /* any op added → transaction must be applied */
    std::vector<sc_layer> gone;   /* vanished layers, reclaimed after apply */

    for (int i = 1; i < n && i <= AWL_MAX_LAYERS; i++) {   /* lay[0] = root → GL path */
        uint64_t sid = lay[i].surface_id;
        awl_buffer_info_t b;
        if (awl_surface_get_buffer(sid, &b) != 0 ||
            b.width == 0 || b.height == 0 ||
            lay[i].w < 0.5f || lay[i].h < 0.5f) {
            hide_layer(h, txn, sid, &any);
            continue;
        }

        sc_layer* l = &h->layers[sid];
        if (!l->sc) {
            l->sc = ASurfaceControl_createFromWindow(h->nw, "awl-layer");
            if (!l->sc) {
                LOGE("child SC create failed (layer %llu)",
                     (unsigned long long)sid);
                h->layers.erase(sid);   /* fresh entry, nothing to reclaim */
                continue;
            }
        }

        /* materialize the buffer → AHB for setBuffer */
        AHardwareBuffer* ahb = NULL;
        bool precise = false;   /* release signalled per buffer (WithRelease) */
        bool buf_set;           /* setBuffer needed this pass */
        if (b.kind == AWL_BUFFER_DMABUF) {
            ahb = dmb_lookup(l, b.token);
            if (!ahb) {
                AHardwareBuffer* donor = NULL;
                ahb = awl_renderer_wrap_dmabuf_ahb(
                    &b, AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE, &donor);
                if (ahb) dmb_push(l, b.token, ahb, donor);
            }
            close(b.fd);   /* wrap dup'ed its own reference */
            if (!ahb) {
                LOGE("layer %llu dmabuf import refused — hidden this frame",
                     (unsigned long long)sid);
                hide_layer(h, txn, sid, &any);
                continue;
            }
            /* same token: SF keeps latching this memory, in-place client
             * writes show at the next latch — no setBuffer needed */
            buf_set = (l->last_token != b.token);
            precise = (g_set_buf_release != NULL);
        } else {   /* shm */
            int32_t dx, dy, dw, dh;
            void* dtok;
            uint32_t dgen;
            int st = awl_surface_get_damage(sid, &dx, &dy, &dw, &dh,
                                            &dtok, &dgen);
            bool unchanged = st == AWL_DMG_NONE && dtok == b.token &&
                             l->last_token == b.token && l->visible;
            if (unchanged) {
                buf_set = false;   /* content on screen already current */
            } else {
                ahb = shm_copy(l, &b);
                buf_set = (ahb != NULL);
                if (ahb && dtok == b.token)
                    awl_surface_damage_consumed(sid, dtok, dgen);
            }
            /* pinned refs from get_buffer — return them (copy is done) */
            wl_shm_buffer_unref(b.shm);
            wl_shm_pool_unref(b.pool);
            if (!ahb && !unchanged) {
                hide_layer(h, txn, sid, &any);
                continue;
            }
        }

        /* geometry: viewport crop (buffer px) + dst (view px, GL-path math) +
         * wl buffer transform; SF clips dst to the parent (window) itself */
        ARect src = layer_src_rect(&lay[i], &b);
        float dxf = ((float)lay[i].x - (float)gox) * sx + ox;
        float dyf = ((float)lay[i].y - (float)goy) * sy + oy;
        ARect dst;
        dst.left = (int32_t)lroundf(dxf);
        dst.top = (int32_t)lroundf(dyf);
        dst.right = (int32_t)lroundf(dxf + lay[i].w * sx);
        dst.bottom = (int32_t)lroundf(dyf + lay[i].h * sy);
        if (dst.right <= dst.left) dst.right = dst.left + 1;
        if (dst.bottom <= dst.top) dst.bottom = dst.top + 1;
        int32_t xf = k_wl_xform[lay[i].transform & 7];
        /* XR24/XRGB = opaque → SF can skip blending for the layer */
        int opaque = (b.drm_format == AWL_FOURCC_ARGB8888) ? 0 : 1;

        bool changed = buf_set || !l->visible || l->last_z != i ||
                       l->last_xform != xf || l->last_opaque != opaque ||
                       memcmp(&l->last_src, &src, sizeof(src)) != 0 ||
                       memcmp(&l->last_dst, &dst, sizeof(dst)) != 0;
        if (changed) {
            ASurfaceTransaction_setZOrder(txn, l->sc, i);
            ASurfaceTransaction_setGeometry(txn, l->sc, src, dst, xf);
            ASurfaceTransaction_setBufferTransparency(
                txn, l->sc,
                opaque ? ASURFACE_TRANSACTION_TRANSPARENCY_OPAQUE
                       : ASURFACE_TRANSACTION_TRANSPARENCY_TRANSLUCENT);
            if (buf_set) {
                if (precise) {
                    /* values only — no pointers, safe against window teardown */
                    struct relbuf_ctx* rctx =
                        (struct relbuf_ctx*)malloc(sizeof(*rctx));
                    if (rctx) {
                        rctx->sid = sid;
                        rctx->token = b.token;
                        g_set_buf_release(txn, l->sc, ahb, -1, rctx,
                                          on_buffer_release);
                    } else {
                        ASurfaceTransaction_setBuffer(txn, l->sc, ahb, -1);
                        precise = false;   /* this buffer releases via OnComplete */
                    }
                } else {
                    ASurfaceTransaction_setBuffer(txn, l->sc, ahb, -1);
                }
                l->last_token = b.token;
            }
            ASurfaceTransaction_setVisibility(txn, l->sc,
                                              ASURFACE_TRANSACTION_VISIBILITY_SHOW);
            l->last_z = i;
            l->last_xform = xf;
            l->last_opaque = opaque;
            l->last_src = src;
            l->last_dst = dst;
            l->visible = 1;
            any = true;
            if (!l->logged) {
                l->logged = 1;
                LOGI("layer %llu → SC z=%d %s %ux%u%s",
                     (unsigned long long)sid, i,
                     b.kind == AWL_BUFFER_DMABUF ? "dmabuf" : "shm",
                     b.width, b.height,
                     precise ? " (precise release)" : "");
            }
        }
        /* frame callbacks for every snapshot child, changed or not — a client
         * may frame() a layer whose state did not change this pass */
        if (cctx && cctx->n < AWL_MAX_LAYERS) {
            cctx->e[cctx->n].id = sid;
            cctx->e[cctx->n].precise = precise;
            cctx->n++;
        }
    }

    /* layers absent from the snapshot (destroyed / unmapped): hide + reclaim */
    for (auto it = h->layers.begin(); it != h->layers.end();) {
        bool found = false;
        for (int i = 1; i < n && !found; i++)
            found = (lay[i].surface_id == it->first);
        if (!found) {
            if (it->second.sc && it->second.visible) {
                ASurfaceTransaction_setVisibility(
                    txn, it->second.sc, ASURFACE_TRANSACTION_VISIBILITY_HIDE);
                any = true;
            }
            gone.push_back(it->second);
            it = h->layers.erase(it);
        } else {
            ++it;
        }
    }

    if (any) {
        ASurfaceTransaction_setOnComplete(txn, cctx, on_complete);
        ASurfaceTransaction_apply(txn);
    } else if (cctx) {
        /* steady state, nothing to submit — fire the frame callbacks at
         * composite time like the GL path does after swap (no SF roundtrip) */
        for (int i = 0; i < cctx->n; i++) {
            if (cctx->e[i].precise) awl_surface_frame_done(cctx->e[i].id);
            else awl_surface_presented(cctx->e[i].id);
        }
        free(cctx);
    }
    /* buffers referenced by the applied transaction stay alive through SF's
     * own refs — reclaiming our SC/AHB refs right after apply is safe */
    for (size_t k = 0; k < gone.size(); k++) layer_free(&gone[k]);
    ASurfaceTransaction_delete(txn);
}
