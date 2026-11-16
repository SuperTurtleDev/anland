#include "internal.h"
#ifdef HAVE_ANLAND
/* Consumer acknowledgement is not physical scanout. No VSYNC/HW_CLOCK flags. */
void mango_private_send_frame_result(struct anland_output *output, uint32_t seq,
        bool presented) {
    struct wlr_output_event_present event = {.commit_seq=seq, .presented=presented};
    if (presented) clock_gettime(CLOCK_MONOTONIC, &event.when);
    wlr_output_send_present(&output->base, &event);
}

static bool release_native_buffer(struct anland_backend *backend,
        uint64_t id, int fence) {
    struct anland_output *output = &backend->output;
    struct anland_buffer *buffer = id && id <= output->buffer_count
        ? output->buffers[id - 1] : NULL;
    bool ok = true;
    if (fence >= 0) {
        if (buffer && buffer->submitted && !backend->session_lost) {
            /* Import the release sync_file into the dmabuf reservation. The
             * renderer's existing implicit synchronization then orders reuse
             * GPU-side without blocking the compositor main loop. */
            struct dma_buf_import_sync_file sync = {.flags=DMA_BUF_SYNC_WRITE, .fd=fence};
            ok = ioctl(buffer->attrs.fd[0], DMA_BUF_IOCTL_IMPORT_SYNC_FILE, &sync) == 0;
        }
        close(fence); /* event receiver owns the fd on every branch */
    }
    if (ok && buffer && buffer->submitted) {
        buffer->submitted = false;
        wlr_buffer_unlock(&buffer->base);
    }
    return ok;
}

void mango_private_dispatch_scene(struct anland_backend *backend) {
    bool frame = false, failed_release = false;
    anland_scene_event_t events[ANLAND_SCENE_EVENT_QUEUE];
    size_t count;
    do {
        count = 0;
        if (anland_de_backend_dispatch(backend->de, events,
                ANLAND_SCENE_EVENT_QUEUE, &count) != 0) break;
        for (size_t i = 0; i < count; ++i) {
            anland_scene_event_t *event = &events[i];
            struct anland_output *output = &backend->output;
            switch (event->type) {
            case ANLAND_SCENE_EVENT_PRESENTED:
            case ANLAND_SCENE_EVENT_COMMIT_DROPPED:
                if (event->commit_id == output->active_commit && output->active_commit) {
                    uint32_t seq = output->active_seq;
                    output->active_commit = 0; /* signal callbacks can re-enter */
                    mango_private_send_frame_result(output, seq,
                        event->type == ANLAND_SCENE_EVENT_PRESENTED);
                }
                break;
            case ANLAND_SCENE_EVENT_BUFFER_RELEASED:
                failed_release |= !release_native_buffer(backend,
                    event->u.released.buffer_id, event->u.released.release_fence_fd);
                break;
            case ANLAND_SCENE_EVENT_RENDER_TARGET_READY:
                if (event->u.target_ready.generation == output->generation &&
                    event->u.target_ready.count == output->buffer_count &&
                    event->u.target_ready.index < output->buffer_count)
                    frame = true;
                break;
            case ANLAND_SCENE_EVENT_OUTPUT_CHANGED:
                /* Connected geometry is applied before importing the new set.
                 * The refresh event path uses the same native mode request. */
                break;
            }
        }
    } while (count != 0);
    if (failed_release && !backend->session_lost) {
        mango_private_drop_session(backend);
        return;
    }
    /* dispatch returns outcomes before releases. Schedule only AFTER draining
     * releases, so external swapchain acquisition cannot race the native unlock. */
    if (frame && !backend->session_lost) mango_private_arm_frame(backend);
}

int mango_private_handle_scene(int fd, uint32_t mask, void *data) {
    (void)fd;
    struct anland_backend *backend = data;
    if (mask & (WL_EVENT_ERROR | WL_EVENT_HANGUP)) {
        mango_private_drop_session(backend); return 0;
    }
    mango_private_dispatch_scene(backend);
    return 0;
}

int mango_private_handle_ready(int fd, uint32_t mask, void *data) {
    (void)fd;
    struct anland_backend *backend = data;
    if (mask & (WL_EVENT_ERROR | WL_EVENT_HANGUP)) {
        mango_private_drop_session(backend); return 0;
    }
    int rc = anland_de_backend_pump(backend->de, 0); /* sole ready-fd reader */
    mango_private_dispatch_scene(backend);
    if (!anland_device_is_connected(backend->device))
        mango_private_drop_session(backend);
    else if (rc != 0)
        mango_private_retry_pump(backend); /* consumed ACK release-publication retry */
    return 0;
}

static const uint32_t output_states = WLR_OUTPUT_STATE_BACKEND_OPTIONAL |
	WLR_OUTPUT_STATE_BUFFER | WLR_OUTPUT_STATE_ENABLED | WLR_OUTPUT_STATE_MODE;
static bool output_test(struct wlr_output *base,
		const struct wlr_output_state *state) {
	struct anland_output *output = wl_container_of(base, output, base);
	if (state->committed & ~output_states) {
		return false;
	}
	if ((state->committed & WLR_OUTPUT_STATE_MODE) &&
			(state->mode_type != WLR_OUTPUT_STATE_MODE_CUSTOM ||
			state->custom_mode.width != (int32_t)output->backend->width ||
			state->custom_mode.height != (int32_t)output->backend->height ||
			state->custom_mode.refresh != (int32_t)output->backend->refresh)) {
		return false;
	}
	if ((state->committed & WLR_OUTPUT_STATE_BUFFER) &&
			(!output->swapchain || !state->buffer ||
			!wlr_swapchain_has_buffer(output->swapchain, state->buffer))) {
		return false;
	}
	return true;
}
static bool output_commit(struct wlr_output *base,
        const struct wlr_output_state *state) {
    struct anland_output *output = wl_container_of(base, output, base);
    if (!output_test(base, state)) return false;
    if (!(state->committed & WLR_OUTPUT_STATE_BUFFER)) return true;
    struct anland_backend *backend = output->backend;
    anland_de_target_t target;
    if (anland_de_backend_get_writable_target(backend->de, &target) != 0 ||
        target.generation != output->generation || target.count != output->buffer_count ||
        target.index >= target.count || output->active_commit) return false;
    struct anland_buffer *buffer = output->buffers[target.index];
    if (!buffer || buffer->submitted || state->buffer != &buffer->base) return false;
    struct dma_buf_export_sync_file sync = {.flags = DMA_BUF_SYNC_WRITE, .fd = -1};
    if (ioctl(buffer->attrs.fd[0], DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &sync) < 0) {
        /* Preserve the existing implicit-sync fallback only where export is
         * unsupported. Real export failures must not silently lose ordering. */
        if (errno != ENOTTY && errno != EINVAL && errno != ENOSYS && errno != EOPNOTSUPP) {
            mango_private_drop_session(backend);
            return false;
        }
        sync.fd = -1;
    }
    anland_layer_state_t layer = {
        .layer_id = backend->output_layer, .buffer_id = (uint64_t)target.index + 1,
        .destination = {0, 0, backend->width, backend->height},
        .opacity = 1.0f, .visible = true, .acquire_fence_fd = sync.fd,
    };
    uint64_t commit = 0;
    int rc = anland_de_backend_commit(backend->de, &layer, 1, &commit);
    if (sync.fd >= 0) close(sync.fd); /* scene borrows; transport duplicates */
    if (rc != 0) return false;
    output->active_commit = commit;
    output->active_seq = base->commit_seq + 1;
    buffer->submitted = true;
    wlr_buffer_lock(&buffer->base);
    if (anland_de_backend_present(backend->de) != 0) {
        /* A transient present failure cannot leave an accepted frame stranded.
         * Retire through the public scene; wlroots observes this commit failed. */
        output->active_commit = 0;
        mango_private_drop_session(backend);
        return false;
    }
    return true;
}
static void output_destroy(struct wlr_output *base) {
	struct anland_output *output=wl_container_of(base,output,base);
	mango_private_drop_session(output->backend); wlr_output_finish(base);
}
const struct wlr_output_impl mango_private_output_impl={.destroy=output_destroy,.test=output_test,.commit=output_commit};
bool mango_anland_output_is(struct wlr_output *output) { return output && output->impl==&mango_private_output_impl; }
struct wlr_swapchain *mango_anland_output_swapchain(struct wlr_output *output) {
	if (!mango_anland_output_is(output)) return NULL;
	struct anland_output *anland=wl_container_of(output,anland,base); return anland->swapchain;
}
#endif