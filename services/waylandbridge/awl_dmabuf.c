/* awl_dmabuf.c — zwp_linux_dmabuf_v1 v3 (v2: only registers the buffer; the
 * renderer gets the fd via awl_surface_get_buffer and imports the EGLImage itself) */
#include "awl_internal.h"

#include <string.h>
#include <sys/stat.h>   /* fstat once at buffer creation: dma-buf inode */
#include <unistd.h>

/* v3 event flow: params.created(buffer); create_immed builds the buffer directly */

struct awl_dmabuf_format {
    uint32_t format;
    uint64_t modifier;
};

static const struct awl_dmabuf_format k_supported[] = {
    { AWL_FORMAT_ARGB8888, DRM_FORMAT_MOD_INVALID },
    { AWL_FORMAT_ARGB8888, DRM_FORMAT_MOD_LINEAR },
    { AWL_FORMAT_XRGB8888, DRM_FORMAT_MOD_INVALID },
    { AWL_FORMAT_XRGB8888, DRM_FORMAT_MOD_LINEAR },
};

/* wl_buffer.destroy request: destroy the resource (triggers dmabuf_buffer_destroy_handler) */
static void dmabuf_wl_buffer_destroy(struct wl_client* c, struct wl_resource* res) {
    wl_resource_destroy(res);
}

static const struct wl_buffer_interface dmabuf_buffer_iface = {
    .destroy = dmabuf_wl_buffer_destroy,
};

static void dmabuf_buffer_destroy_handler(struct wl_resource* res) {
    struct awl_buffer* b = wl_resource_get_user_data(res);
    if (!b) return;
    /* Remove references from each surface's pending/current: rd + per-window
     * ev_lock (render thread get_buffer snapshots under the same lock — its
     * dup fd already pinned the memory, so close is safe).
     * This handler runs on that client's dispatch thread; the lock exists
     * for the render-thread readers. */
    pthread_rwlock_rdlock(&g_srv.rwl);
    struct awl_surface* s;
    wl_list_for_each(s, &g_srv.surfaces, link) {
        int queued = 0;
        for (int i = 0; i < s->release_q_n && !queued; i++)
            queued = (s->release_q[i] == res);   /* lock-free read of the count (this thread is the only writer + render side only clears, never adds) */
        if (s->pending_buffer_res != res && s->current_buffer_res != res &&
            s->latched_buffer_res != res && !queued)
            continue;   /* unrelated window: skip ev_lock (commit not affected) */
        pthread_mutex_lock(&s->ev_lock);
        if (s->pending_buffer_res == res) s->pending_buffer_res = NULL;
        if (s->current_buffer_res == res) s->current_buffer_res = NULL;
        if (s->latched_buffer_res == res) {
            s->latched_buffer_res = NULL;
            s->latched_attach = 0;
            s->sub_latched = 0;
        }
        for (int i = 0; i < s->release_q_n; )   /* resource is dying: remove it, stop sending release */
            if (s->release_q[i] == res) s->release_q[i] = s->release_q[--s->release_q_n];
            else i++;
        pthread_mutex_unlock(&s->ev_lock);
    }
    pthread_rwlock_unlock(&g_srv.rwl);
    pthread_rwlock_wrlock(&g_srv.rwl);
    wl_list_remove(&b->link);
    pthread_rwlock_unlock(&g_srv.rwl);
    if (b->dmabuf_fd >= 0) close(b->dmabuf_fd);
    free(b);
    wl_resource_set_user_data(res, NULL);
}

static struct awl_buffer* dmabuf_buffer_create(struct wl_client* client,
                                               uint32_t id, uint32_t version,
                                               int fd, uint32_t w, uint32_t h,
                                               uint32_t stride, uint32_t format,
                                               uint64_t modifier) {
    if (format != AWL_FORMAT_ARGB8888 && format != AWL_FORMAT_XRGB8888) {
        LOGE("dmabuf unsupported format 0x%08x", format);
        return NULL;
    }
    struct awl_buffer* b = calloc(1, sizeof(*b));
    if (!b) return NULL;
    b->dmabuf_fd = fd;                 /* take over the fd */
    struct stat st;                    /* identity once, here — the render side
                                        * compares awl_buffer_info_t.ino with
                                        * no per-frame fstat */
    b->ino = fstat(fd, &st) == 0 ? (uint64_t)st.st_ino : 0;
    b->width = w;
    b->height = h;
    b->stride = stride;
    b->drm_format = format;
    b->modifier = modifier;
    pthread_rwlock_wrlock(&g_srv.rwl);
    wl_list_insert(g_srv.buffers.prev, &b->link);
    pthread_rwlock_unlock(&g_srv.rwl);

    b->resource = wl_resource_create(client, &wl_buffer_interface,
                                     version, id);
    if (!b->resource) {
        pthread_rwlock_wrlock(&g_srv.rwl);
        wl_list_remove(&b->link);
        pthread_rwlock_unlock(&g_srv.rwl);
        close(fd);
        free(b);
        return NULL;
    }
    wl_resource_set_implementation(b->resource, &dmabuf_buffer_iface, b,
                                   dmabuf_buffer_destroy_handler);
    LOGD("dmabuf buffer %ux%u stride=%u fmt=%c%c%c%c mod=0x%llx fd=%d",
            w, h, stride,
            (char)(format & 0xff), (char)((format >> 8) & 0xff),
            (char)((format >> 16) & 0xff), (char)((format >> 24) & 0xff),
            (unsigned long long)modifier, fd);
    return b;
}

/* ---------------- zwp_linux_buffer_params_v1 ---------------- */

struct awl_params {
    struct wl_resource* resource;
    int fd;
    uint32_t width, height, stride;
    uint32_t format;
    uint64_t modifier;
    int has_fd, has_geometry, has_format;
};

static void params_destroy(struct wl_client* c, struct wl_resource* res) {
    struct awl_params* p = wl_resource_get_user_data(res);
    if (p && p->fd >= 0) close(p->fd);
    wl_resource_destroy(res);
}

static void params_add(struct wl_client* c, struct wl_resource* res,
                       int32_t fd, uint32_t plane_idx, uint32_t offset,
                       uint32_t stride, uint32_t modifier_hi, uint32_t modifier_lo) {
    struct awl_params* p = wl_resource_get_user_data(res);
    LOGD("params_add fd=%d plane=%u off=%u stride=%u", fd, plane_idx, offset, stride);
    if (!p) {
        close(fd);
        return;
    }
    if (plane_idx != 0) {
        LOGE("multi-plane dmabuf unsupported (plane %u)", plane_idx);
        close(fd);
        wl_resource_post_error(res,
                ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_PLANE_IDX,
                "only single plane supported");
        return;
    }
    if (p->fd >= 0) close(p->fd);   /* repeated add overwrites */
    p->fd = fd;
    p->has_fd = 1;
    (void)offset;
    p->stride = stride;
    p->modifier = ((uint64_t)modifier_hi << 32) | modifier_lo;
}

static void params_create(struct wl_client* c, struct wl_resource* res,
                          int32_t width, int32_t height, uint32_t format,
                          uint32_t flags) {
    struct awl_params* p = wl_resource_get_user_data(res);
    if (!p || !p->has_fd || width <= 0 || height <= 0) {
        wl_resource_post_error(res,
                ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_WL_BUFFER,
                "incomplete dmabuf params");
        return;
    }
    struct awl_buffer* b = dmabuf_buffer_create(c, 0, 1, p->fd, width, height,
                                                p->stride, format, p->modifier);
    if (!b) {
        zwp_linux_buffer_params_v1_send_failed(res);
        return;
    }
    p->fd = -1;   /* ownership transferred; params destroyed by the client afterwards */
    p->has_fd = 0;
    zwp_linux_buffer_params_v1_send_created(res, b->resource);
}

static void params_create_immed(struct wl_client* c, struct wl_resource* res,
                                uint32_t buffer_id, int32_t width, int32_t height,
                                uint32_t format, uint32_t flags) {
    struct awl_params* p = wl_resource_get_user_data(res);
    if (!p || !p->has_fd || width <= 0 || height <= 0) {
        wl_resource_post_error(res,
                ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_WL_BUFFER,
                "incomplete dmabuf params");
        return;
    }
    struct awl_buffer* b = dmabuf_buffer_create(c, buffer_id, 1, p->fd,
                                                width, height, p->stride,
                                                format, p->modifier);
    if (!b) {
        wl_resource_post_error(res,
                ZWP_LINUX_BUFFER_PARAMS_V1_ERROR_INVALID_WL_BUFFER,
                "unsupported format");
        close(p->fd);
        p->fd = -1;
        p->has_fd = 0;
        return;
    }
    p->fd = -1;
    p->has_fd = 0;
}

static const struct zwp_linux_buffer_params_v1_interface params_iface = {
    .destroy = params_destroy,
    .add = params_add,
    .create = params_create,
    .create_immed = params_create_immed,
};

static void params_res_destroy(struct wl_resource* res) {
    struct awl_params* p = wl_resource_get_user_data(res);
    if (p) {
        if (p->fd >= 0) close(p->fd);
        free(p);
    }
}

static void dmabuf_create_params(struct wl_client* c,
                                 struct wl_resource* dmabuf_res, uint32_t id) {
    LOGD("create_params id=%u", id);
    struct wl_resource* res = wl_resource_create(
            c, &zwp_linux_buffer_params_v1_interface,
            wl_resource_get_version(dmabuf_res), id);
    struct awl_params* p = calloc(1, sizeof(*p));
    if (!res || !p) {
        if (res) wl_resource_destroy(res);
        wl_resource_post_no_memory(dmabuf_res);
        return;
    }
    p->fd = -1;
    wl_resource_set_implementation(res, &params_iface, p, params_res_destroy);
}

static void dmabuf_destroy(struct wl_client* c, struct wl_resource* res) {
    wl_resource_destroy(res);
}

static const struct zwp_linux_dmabuf_v1_interface dmabuf_iface = {
    .destroy = dmabuf_destroy,
    .create_params = dmabuf_create_params,
};

static void dmabuf_bind(struct wl_client* client, void* data,
                        uint32_t version, uint32_t id) {
    uint32_t v = version < 3 ? version : 3;
    struct wl_resource* res = wl_resource_create(
            client, &zwp_linux_dmabuf_v1_interface, v, id);
    wl_resource_set_implementation(res, &dmabuf_iface, NULL, NULL);
    for (size_t i = 0; i < sizeof(k_supported) / sizeof(k_supported[0]); i++) {
        zwp_linux_dmabuf_v1_send_format(res, k_supported[i].format);
        if (v >= ZWP_LINUX_DMABUF_V1_MODIFIER_SINCE_VERSION)
            zwp_linux_dmabuf_v1_send_modifier(
                    res, k_supported[i].format,
                    (uint32_t)(k_supported[i].modifier >> 32),
                    (uint32_t)(k_supported[i].modifier & 0xffffffff));
    }
}

void awl_dmabuf_setup(void) {
    g_srv.g_dmabuf = wl_global_create(g_srv.display,
                                      &zwp_linux_dmabuf_v1_interface, 3,
                                      NULL, dmabuf_bind);
}
