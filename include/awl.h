/*
 * awl.h — AndroidWayland logic-layer public API (v2, window-driven)
 *
 * Logic layer: the Wayland protocol state machines (compositor/xdg_shell/
 * shm/dmabuf/output). Each xdg_toplevel maps to one Android window
 * (window_id); rendering is done by the adaptation layer (GPU) — the logic
 * layer never touches pixels.
 */
#ifndef AWL_H
#define AWL_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>   /* pid_t */

#ifdef __cplusplus
extern "C" {
#endif

#define AWL_ABI_VERSION 2

struct wl_shm_buffer;

/* ---- Window callbacks (implemented by the adaptation layer, invoked on the
 *      client's protocol dispatch thread: the main event thread or its
 *      dedicated sub-event thread; the adaptation layer must provide its own
 *      thread safety) ---- */
typedef struct awl_window_callbacks {
    void* user;

    /* xdg_surface first-frame map → create the Android-side window
     * (Activity). pref_w/h = first-frame buffer size; is_popup: popup-type
     * (menus etc.) */
    void (*window_created)(void* user, uint64_t id, int32_t pref_w, int32_t pref_h,
                           const char* title, int is_popup);
    void (*window_destroyed)(void* user, uint64_t id);
    void (*window_title)(void* user, uint64_t id, const char* title);

    /* Pointer constraint state change (zwp_pointer_constraints_v1
     * lock/confine request, set_region on a live constraint, or object /
     * surface destruction — pure state sync, always on the client's
     * dispatch thread; the input forwarding path never branches on this) →
     * the adaptation layer tells the Activity over C_CAPTURE: mode =
     * CONFINE/LOCK → requestPointerCapture (captured motion arrives as
     * AWL_IN_PTR_REL, the Activity synthesizes any clamped absolute motion
     * itself), NONE → releasePointerCapture. rx,ry,rw,rh = confine region
     * in Activity view pixels (zeros = whole window for CONFINE, ignored
     * for NONE/LOCK). */
    void (*pointer_lock)(void* user, uint64_t id, int mode,
                         int32_t rx, int32_t ry, int32_t rw, int32_t rh);

    /* surface commit (new buffer ready) → the adaptation layer pulls and renders */
    void (*window_dirty)(void* user, uint64_t id);

    /* ---- IME bridge (text-input protocol ↔ Android IME, see awl_ime.c) ----
     * Passthrough model: everything carries its window id, no routing state;
     * invoked on the client's protocol dispatch thread. */

    /* Client text field gained/lost focus (v1 activate / v3 enable+commit ↔
     * kbd focus) → notify the matching Activity to show/hide the soft
     * keyboard. hint/purpose = zwp_text_input content type (the Activity
     * maps it to InputType; keyboard panel selection for segmentation /
     * prediction). */
    void (*ime_show)(void* user, uint64_t id, uint32_t hint, uint32_t purpose);
    void (*ime_hide)(void* user, uint64_t id);

    /* Client-reported editor state (set_surrounding_text/set_cursor_rectangle
     * take effect with commit_state / v3 commit) → the Activity caches it to
     * answer IME queries (getTextBeforeCursor/getCursorCapsMode/... =
     * segmentation/prediction context) + updateSelection/
     * updateCursorAnchorInfo drive the candidate window to follow the
     * cursor. text = UTF-8 surrounding; cursor/anchor = byte offsets into
     * text; flags: see AWL_IME_STATE_* (e.g. v1 reset → Activity
     * restartInput). */
    void (*ime_state)(void* user, uint64_t id, const char* text,
                      int32_t cursor, int32_t anchor,
                      uint32_t hint, uint32_t purpose,
                      int32_t cx, int32_t cy, int32_t cw, int32_t ch,
                      uint32_t flags);

    /* ---- Clipboard bridge (wl_data_device set_selection → Android
     * clipboard) ----
     * Text already fetched from the source client's pipe (dedicated thread,
     * callback on that thread; ≤256KB UTF-8). win_id = the owning toplevel
     * of the source client (0 = none/clear); an empty utf8 string clears
     * the clipboard. The adaptation layer has the Activity write to
     * ClipboardManager over the ctrl channel (background reads/writes are
     * restricted on Android 10+, the foreground Activity does it on behalf). */
    void (*clipboard_text)(void* user, uint64_t win_id, const char* utf8);

    /* ---- Cursor (wl_pointer.set_cursor, semantics of kwin-6.6.5
     *      PointerInterface::pointer_set_cursor + CursorImage) ----
     * hidden=1: the pointer-focused client took over the cursor for that
     * window (set_cursor with a cursor surface, or NULL = invisible pointer)
     * → the adaptation layer must hide the Android system pointer for that
     * window (View.setPointerIcon(TYPE_NULL)); the cursor image itself is
     * composited by the renderer as the topmost layer of the window
     * (awl_pointer_cursor_layer, positioned from the pointer position the
     * Activity reports with every motion). hidden=0: pointer left the
     * window / (re)entered a window / focus layer or cursor surface
     * destroyed / client gone → restore the system pointer. Only state
     * transitions are reported. Invoked on the client's protocol dispatch
     * thread (set_cursor, surface death) or the input thread (leave/enter). */
    void (*pointer_cursor)(void* user, uint64_t id, int hidden);

    /* ---- Idle inhibitor (zwp_idle_inhibit_manager_v1, awl_idle.c) ----
     * Aggregate inhibitor state of the window flipped (first inhibitor
     * created on one of its surfaces ↔ last one gone, incl. surface
     * death) — pure state sync on the client's dispatch thread, only
     * transitions are reported → the adaptation layer tells the Activity
     * over C_KEEPON to set/clear FLAG_KEEP_SCREEN_ON. The window flag's
     * native Android semantics (honored only while the window is visible)
     * already matches the protocol's "inhibitor honored on a visible
     * surface" requirement, so no daemon-side visibility state exists. */
    void (*idle_inhibit)(void* user, uint64_t id, int on);
} awl_window_callbacks_t;

typedef struct awl_display_info {
    uint32_t width;        /* wl_output logical size */
    uint32_t height;
    int32_t  refresh_hz;
    int32_t  dpi;
    int32_t  scale;        /* wl_output scale factor (1) */
} awl_display_info_t;

/*
 * Start the server (listen_fd already bound+listening). Returns 0 on
 * success. The event loop owns a dedicated thread.
 */
int  awl_server_start(int listen_fd,
                      const awl_display_info_t* info,
                      const awl_window_callbacks_t* cbs);
void awl_server_stop(void);
int  awl_server_is_running(void);

/* ---- Android → logic layer ----
 * Direct send from any thread (libwayland carries the awl patches:
 * connection mutex + atomic serial): resolve→protocol send→flush all happen
 * inside the binder thread, bypassing the event thread. */
void awl_window_resize(uint64_t id, int32_t w, int32_t h);   /* xdg configure */
void awl_window_close(uint64_t id);                          /* xdg close */
int  awl_xwayland_window_serial(uint64_t id, uint64_t* serial);  /* Xwayland
    * window association serial (WL_SURFACE_SERIAL; 1 = an Xwayland window
    * that has been associated, 0 = not) — used to pair X-side operations
    * (resize/close) over the mini-wm control channel */
void awl_window_set_activated(uint64_t id, int activated);   /* ACTIVATED state */
pid_t awl_window_client_pid(uint64_t id);   /* window id → client host pid, read
    * fresh from the connect-time cached credentials (0 = unknown/destroyed) */
/* Foreground scheduling (awl_sched.c): on = move pid's whole /proc subtree
 * into Android's top-app cgroups, off = back to the root groups. Stateless
 * and synchronous — the adapter calls it on window attach/detach and with
 * getpid() for its own boost. Any thread, no daemon locks held. */
void awl_sched_set(pid_t pid, int on);
void awl_display_set_zoom(int pct);   /* zoom = 100×Z (50..300; dynamic, #31) */
int awl_display_zoom(void);           /* current zoom pct (daemon config reads) */

/* View mapping mode (#34, daemon config scale_mode): how the content-base
 * rectangle is placed inside the Android window. Presentation-layer only —
 * configure sizes are unaffected (a client that fills the window's aspect
 * renders 1:1 in every mode; one that keeps a fixed size gets letterboxed
 * instead of stretched). */
enum {
    AWL_SCALE_STRETCH = 0,   /* fill each axis independently (legacy behavior) */
    AWL_SCALE_FIT = 1,       /* uniform scale to fit inside, centered, letterbox */
    AWL_SCALE_CENTER = 2,    /* 1:1, centered (content larger than the window is cropped) */
};

/* Unified view mapping, the single source of truth shared by every
 * coordinate conversion between the client's content base (logical px) and
 * the Android window (view px):
 *   view = logical × s + o        (render dst, confine region, IME cursor rect)
 *   logical = (view − o) / s      (input, relative deltas)
 * Degenerate input (pw/ph ≤ 0 — no resize recorded yet — or cw/ch ≤ 0.5)
 * yields the identity map; an unknown mode falls back to stretch. Pure math,
 * any thread, no locks. */
void awl_view_map(int mode, float pw, float ph, float cw, float ch,
                  float* sx, float* sy, float* ox, float* oy);
void awl_display_set_scale_mode(int mode);   /* dynamic; invalid → ignored + LOGE */
int awl_display_scale_mode(void);            /* current mode (daemon config reads) */
/* Initial-configure placeholder size (#33, daemon config init_w/init_h — the
 * size sent before the Android window exists; the real size follows via
 * awl_window_resize once the Activity surface is ready). Applies to new
 * windows only. Any thread. */
void awl_display_set_init_size(int32_t w, int32_t h);
void awl_display_init_size(int32_t* w, int32_t* h);   /* current value (config reads) */

/* ---- Input (Activity → binder → straight to the client, bypassing the
 *      event thread) ----
 * Each event carries its target window id (input reaching an Activity goes
 * to that window). */

/* pointer_lock modes (mirror of the Activity's CAPTURE_* in
 * WlWindowActivity.java — do not renumber, they cross the ctrl parcel) */
enum {
    AWL_CAPTURE_NONE    = 0,
    AWL_CAPTURE_CONFINE = 1,
    AWL_CAPTURE_LOCK    = 2,
};

enum {
    AWL_IN_PTR_ENTER = 1,    /* x,y view coordinates */
    AWL_IN_PTR_LEAVE = 2,
    AWL_IN_PTR_MOTION = 3,   /* x,y view coordinates */
    AWL_IN_PTR_BUTTON = 4,   /* code=BTN_*, v1=state */
    AWL_IN_PTR_AXIS = 5,     /* x=vertical, y=horizontal; code=0 wheel notches
                               (×10+discrete), code=1 touchpad finger pixel
                               distance (source=finger, raw value, no
                               discrete) */
    AWL_IN_PTR_REL = 6,      /* x,y relative motion (captured state: while a
                               lock/confine constraint is active the Activity
                               reports AXIS_RELATIVE_X/Y, otherwise the
                               absolute-position diff) */
    AWL_IN_KBD_ENTER = 7,    /* id gained keyboard focus */
    AWL_IN_KBD_LEAVE = 8,
    AWL_IN_KEY = 9,          /* code=evdev, v1=state(1/0) */
    AWL_IN_MODIFIERS = 10,   /* meta=Android meta bits */
    AWL_IN_TOUCH_DOWN = 11,  /* code=touch id, x,y */
    AWL_IN_TOUCH_MOTION = 12,
    AWL_IN_TOUCH_UP = 13,
    AWL_IN_TOUCH_CANCEL = 14,
    AWL_IN_TABLET_PROX_IN = 20,  /* zwp_tablet_v2 (stylus todo) */
    AWL_IN_TABLET_PROX_OUT = 21,
    AWL_IN_TABLET_MOTION = 22,   /* x,y v1=pressure tilt uses v2 */
    AWL_IN_TABLET_DOWN = 23,
    AWL_IN_TABLET_UP = 24,
    AWL_IN_TABLET_BUTTON = 25,
};

typedef struct awl_input_ev {
    uint64_t id;      /* target window (input reaching an Activity goes to that window) */
    uint32_t type;
    uint32_t code;    /* evdev code / touch id / tablet button */
    float x, y;       /* view coords | relative motion | wheel (v,h) */
    float v1, v2;     /* state / pressure / tilt… (per type) */
    uint32_t meta;    /* Android meta bits (for MODIFIERS) */
    uint32_t flags;   /* reserved (tool type etc.) */
} awl_input_ev_t;

void awl_input_dispatch(const awl_input_ev_t* ev);   /* any thread */

/* ---- IME text passthrough (Activity InputConnection → binder → client,
 *      any thread; same passthrough model as input: ops carry their window
 *      id, no routing state) ---- */

enum {
    AWL_IME_COMMIT = 1,     /* text: committed text (v3 commit_string; v1 same) */
    AWL_IME_PREEDIT = 2,    /* text: preedit; a/b = cursor_begin/end (byte offsets into text) */
    AWL_IME_DELETE = 3,     /* a/b = before/after (bytes; the client converts against its cache) */
    AWL_IME_CURSOR = 4,     /* a/b = index/anchor (v1 cursor_position; v3 ignores) */
};

/* ime_state flags */
#define AWL_IME_STATE_RESET 0x1u   /* v1 reset → Activity clears composing state + restartInput */

void awl_ime_text(uint64_t id, uint32_t op, const char* text, int32_t a, int32_t b);

/* ---- Android clipboard → wl selection (read by the focused Activity, then
 *      pushed over binder; any thread) ----
 * An internal source (text/plain;charset=utf-8) takes over the global
 * selection and notifies the focused client; an empty string clears the
 * selection. Echo suppression is the APK side's job (lastClipWritten). */
void awl_datadev_android_clip(const char* utf8);

/* ---- Adaptation layer → logic layer (render thread; locked snapshot
 *      inside) ---- */

enum {
    AWL_BUFFER_NONE = 0,
    AWL_BUFFER_SHM = 1,      /* wl_shm_buffer, accessed via libwayland API */
    AWL_BUFFER_DMABUF = 2,
};

typedef struct awl_buffer_info {
    int kind;
    void* token;             /* buffer identity (wayland resource pointer value, reuse check) */
    /* SHM (both shm/pool are references pinned at get: the mapping survives
     * the client destroying the buffer/pool — the upload is asynchronous to
     * commit; the caller must return them with wl_shm_buffer_unref +
     * wl_shm_pool_unref when done, otherwise the mapping leaks) */
    struct wl_shm_buffer* shm;    /* read-only access: wl_shm_buffer_get_data etc. */
    struct wl_shm_pool*   pool;   /* pool reference: also defers resize (mremap) */
    /* DMABUF (fd is dup'ed by this call — stays valid even if the buffer is
     * destroyed meanwhile; the caller must close it when done) */
    int      fd;
    uint64_t modifier;
    /* common */
    uint32_t width, height, stride;
    uint32_t drm_format;          /* DRM fourcc */
} awl_buffer_info_t;

/* Get the current buffer (rdlock+ev_lock snapshot + dup fd, render-thread
 * safe). Returns 0 = present, <0 = none */
int  awl_surface_get_buffer(uint64_t id, awl_buffer_info_t* out);

/* ---- Damage (shm render path; wl_surface.damage accumulation) ----
 * get_damage returns the damage accumulated since the renderer last consumed
 * it (read-only snapshot under rdlock+ev_lock):
 *   AWL_DMG_NONE  nothing changed — skip the upload entirely (re-render
 *                 triggered by a cursor/layer move)
 *   AWL_DMG_RECT  re-upload the bbox (x,y,w,h, buffer px, already scaled by
 *                 buffer_scale; clamp to the buffer before use)
 *   AWL_DMG_FULL  the client committed a buffer with no damage — upload in
 *                 full (protocol default)
 * token = the wl_buffer resource the damage applies to, gen = its revision.
 * Compare token with the awl_buffer_info_t.token from get_buffer: mismatch
 * (buffer swapped between the two calls) → upload in full and do NOT consume.
 * After a successful upload call awl_surface_damage_consumed with the same
 * token+gen — it clears the damage only when still current (a commit that
 * raced the upload keeps its damage for the next frame). Over-upload is
 * always safe, under-upload never. dmabuf layers ignore this (the texture
 * samples the memory in place). */
enum {
    AWL_DMG_NONE = 0,
    AWL_DMG_RECT = 1,
    AWL_DMG_FULL = 2,
};
int  awl_surface_get_damage(uint64_t id, int32_t* x, int32_t* y,
                            int32_t* w, int32_t* h, void** token, uint32_t* gen);
void awl_surface_damage_consumed(uint64_t id, void* token, uint32_t gen);

/* ---- Sublayer composition snapshot (wl_subsurface, render thread) ----
 * #31 zoom: coordinates/sizes are always logical px (viewport dst | source |
 * buffer/scale); the render side scales dst by window-physical /
 * root-logical (=out[0].w). */

#define AWL_MAX_LAYERS 16

typedef struct awl_layer_info {
    uint64_t surface_id;   /* layer surface (root is always the first element) */
    float x, y;            /* root logical coordinates (Y down; root=(0,0)) */
    float w, h;            /* layer logical size (input hit-testing; 0 = no buffer on this layer) */
    float u0, v0, su, sv;  /* normalized uv transform of the sample region (viewport source; default = whole image) */
    int32_t transform;     /* wl_surface.set_buffer_transform (wl_output.transform 0..7,
                            * applied on commit; 90/270 swap the logical size) */
} awl_layer_info_t;

/* Returns the layer count (root first, sublayers in stack order bottom→top;
 * nested sublayers follow their parent). No root / over the limit →
 * truncated (>0 is enough to render). */
int  awl_surface_get_layers(uint64_t root_id, awl_layer_info_t* out, int max);

/* Client cursor layer of this window (wl_pointer.set_cursor surface; render
 * thread). Returns 1 and fills *out when the pointer-focused client set a
 * cursor surface for this window: the renderer composites it ABOVE every
 * layer returned by awl_surface_get_layers (it is not part of that stack and
 * never hit-tests). x,y = pointer position − hotspot (root logical
 * coordinates, same basis as the layer stack), w,h = cursor surface logical
 * size. 0 = nothing to draw (no cursor set, set_cursor(NULL) = invisible
 * pointer, or the pointer is in another window). */
int  awl_pointer_cursor_layer(uint64_t root_id, awl_layer_info_t* out);

/* Root's xdg window geometry origin (buffer pixels; never set = 0,0).
 * Shared by the render dst and the input view→buffer mapping: view(0,0) ↔
 * geometry rectangle origin. */
void awl_surface_get_origin(uint64_t root_id, int32_t* ox, int32_t* oy,
                            float* cw, float* ch);   /* geometry origin + content base size */

/* This frame has been presented (rendering done; the render thread sends
 * the frame callbacks directly) */
void awl_surface_presented(uint64_t id);

/* Frame callbacks only (presented minus the release_q drain) — for layers
 * whose wl_buffer.release is signalled precisely per buffer instead
 * (SurfaceControl setBufferWithRelease, awl_hwc.cpp). Any thread, same
 * locking as presented. */
void awl_surface_frame_done(uint64_t id);

/* Release exactly this buffer (wl_buffer identity = get_buffer's token) if it
 * still sits in the surface's deferred release queue: the display pipeline
 * stopped sampling it (SurfaceControl OnBufferRelease). No-op when the
 * buffer is unknown/already released. Any thread, same locking as presented. */
void awl_surface_release_token(uint64_t id, void* token);

#ifdef __cplusplus
}
#endif
#endif /* AWL_H */
