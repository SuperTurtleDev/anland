#include "internal.h"
#ifdef HAVE_ANLAND
static void buffer_destroy(struct wlr_buffer *base) {
	struct anland_buffer *buffer = wl_container_of(base, buffer, base);
	if (buffer->attrs.fd[0] >= 0) close(buffer->attrs.fd[0]);
	free(buffer);
}
static bool buffer_get_dmabuf(struct wlr_buffer *base,
		struct wlr_dmabuf_attributes *attrs) {
	struct anland_buffer *buffer = wl_container_of(base, buffer, base);
	*attrs = buffer->attrs;
	return true;
}
static const struct wlr_buffer_impl buffer_impl = {
	.destroy = buffer_destroy,
	.get_dmabuf = buffer_get_dmabuf,
};
static size_t select_slot(void *data) {
    struct anland_backend *backend = data;
    anland_de_target_t target;
    if (anland_de_backend_get_writable_target(backend->de, &target) != 0 ||
        target.generation != backend->output.generation ||
        target.count != backend->output.buffer_count || target.index >= target.count)
        return SIZE_MAX;
    struct anland_buffer *buffer = backend->output.buffers[target.index];
    return !buffer || buffer->submitted ? SIZE_MAX : target.index;
}

void mango_private_drop_buffers(struct anland_backend *backend) {
    struct anland_output *output = &backend->output;
    /* Session already invalidated and outcomes dispatched by the owner. Native
     * resources may be retired now, including a failed release-fence import. */
    for (uint32_t i = 0; i < output->buffer_count; ++i) {
        struct anland_buffer *buffer = output->buffers[i];
        if (buffer && buffer->submitted) {
            buffer->submitted = false;
            wlr_buffer_unlock(&buffer->base);
        }
    }
    wlr_swapchain_destroy(output->swapchain);
    output->swapchain = NULL;
    memset(output->buffers, 0, sizeof(output->buffers));
    output->buffer_count = 0;
    output->generation = 0;
}

bool mango_private_import_buffers(struct anland_backend *backend) {
    anland_de_target_t target;
    if (anland_de_backend_get_writable_target(backend->de, &target) != 0 ||
        target.count == 0 || target.count > ANLAND_DEVICE_MAX_BUFS ||
        target.count > WLR_SWAPCHAIN_CAP) return false;
    const uint32_t count = target.count;
    struct wlr_buffer *buffers[ANLAND_DEVICE_MAX_BUFS] = {0};
    struct wlr_drm_format_set formats = {0};
    uint32_t format_code = 0;
    for (uint32_t i = 0; i < count; ++i) {
        anland_device_fb_t fb = {.fd = -1};
        if (anland_device_get_drm_fb(backend->device, (int)i, &fb) < 0) goto fail;
        if (fb.width != backend->width || fb.height != backend->height ||
            !fb.stride || (i && fb.format != format_code)) {
            close(fb.fd); goto fail;
        }
        format_code = fb.format;
        struct anland_buffer *buffer = calloc(1, sizeof(*buffer));
        if (!buffer) { close(fb.fd); goto fail; }
        buffer->attrs = (struct wlr_dmabuf_attributes){.width=fb.width,
            .height=fb.height, .format=fb.format, .modifier=fb.modifier,
            .n_planes=1, .offset={fb.offset}, .stride={fb.stride}, .fd={fb.fd}};
        wlr_buffer_init(&buffer->base, &buffer_impl, fb.width, fb.height);
        backend->output.buffers[i] = buffer;
        buffers[i] = &buffer->base;
        if (!wlr_drm_format_set_add(&formats, fb.format, fb.modifier)) goto fail;
    }
    const struct wlr_drm_format *format = wlr_drm_format_set_get(&formats, format_code);
    if (!format) goto fail;
    backend->output.swapchain = wlr_swapchain_create_external(backend->width,
        backend->height, format, buffers, count, select_slot, backend);
    if (!backend->output.swapchain) goto fail;
    backend->output.buffer_count = count;
    backend->output.generation = target.generation;
    wlr_drm_format_set_finish(&formats);
    return true;
fail:
    wlr_drm_format_set_finish(&formats);
    for (uint32_t i = 0; i < count; ++i) if (buffers[i]) wlr_buffer_drop(buffers[i]);
    memset(backend->output.buffers, 0, sizeof(backend->output.buffers));
    return false;
}
#endif
