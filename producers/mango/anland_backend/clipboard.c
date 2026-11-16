#include "internal.h"
#ifdef HAVE_ANLAND
struct anland_clipboard_source {
	struct wlr_data_source source;
	struct anland_backend *backend;
	char *data;
	size_t size;
};

struct anland_clipboard_write {
	struct anland_backend *backend;
	struct wl_list link;
	struct wl_event_source *source;
	int fd;
	char *data;
	size_t size, offset;
};

static void clipboard_write_destroy(struct anland_clipboard_write *transfer) {
	if (transfer->source) wl_event_source_remove(transfer->source);
	if (transfer->fd >= 0) close(transfer->fd);
	wl_list_remove(&transfer->link);
	free(transfer->data);
	free(transfer);
}

void mango_private_clear_clipboard_writes(struct anland_backend *backend) {
	struct anland_clipboard_write *transfer, *tmp;
	wl_list_for_each_safe(transfer, tmp, &backend->clipboard_writes, link)
		clipboard_write_destroy(transfer);
}

static int handle_clipboard_write(int fd, uint32_t mask, void *data) {
	struct anland_clipboard_write *transfer = data;
	if (mask & (WL_EVENT_ERROR | WL_EVENT_HANGUP)) {
		clipboard_write_destroy(transfer);
		return 0;
	}
	while (transfer->offset < transfer->size) {
		ssize_t n = write(fd, transfer->data + transfer->offset,
			transfer->size - transfer->offset);
		if (n > 0) { transfer->offset += (size_t)n; continue; }
		if (n < 0 && errno == EINTR) continue;
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return 0;
		break;
	}
	clipboard_write_destroy(transfer);
	return 0;
}

static void clipboard_source_send(struct wlr_data_source *source,
		const char *mime_type, int32_t fd) {
	struct anland_clipboard_source *clip = wl_container_of(source, clip, source);
	if (strcmp(mime_type, CLIPBOARD_MIME_UTF8) != 0 &&
			strcmp(mime_type, CLIPBOARD_MIME_TEXT) != 0) {
		close(fd);
		return;
	}
	struct anland_backend *backend = clip->backend;
	struct anland_clipboard_write *write = calloc(1, sizeof(*write));
	if (!write) { close(fd); return; }
	write->data = malloc(clip->size);
	if (!write->data) { close(fd); free(write); return; }
	memcpy(write->data, clip->data, clip->size);
	write->backend = backend;
	write->fd = fd;
	write->size = clip->size;
	if (fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) < 0) {
		close(fd); free(write->data); free(write); return;
	}
	wl_list_insert(&backend->clipboard_writes, &write->link);
	write->source = wl_event_loop_add_fd(backend->loop, fd, WL_EVENT_WRITABLE,
		handle_clipboard_write, write);
	if (!write->source) clipboard_write_destroy(write);
}

static void clipboard_source_destroy(struct wlr_data_source *source) {
	struct anland_clipboard_source *clip = wl_container_of(source, clip, source);
	free(clip->data);
	free(clip);
}

static const struct wlr_data_source_impl clipboard_source_impl = {
	.send = clipboard_source_send,
	.destroy = clipboard_source_destroy,
};

static bool clipboard_source_add_mime(struct wlr_data_source *source,
		const char *mime) {
	char *copy = strdup(mime);
	if (!copy)
		return false;
	char **slot = wl_array_add(&source->mime_types, sizeof(*slot));
	if (!slot) {
		free(copy);
		return false;
	}
	*slot = copy;
	return true;
}

void mango_private_set_clipboard_from_consumer(struct anland_backend *backend,
		const char *payload, size_t size) {
	if (!server.seat || size == 0)
		return;
	struct anland_clipboard_source *clip = calloc(1, sizeof(*clip));
	if (!clip)
		return;
	clip->backend = backend;
	clip->data = malloc(size);
	if (!clip->data) {
		free(clip);
		return;
	}
	memcpy(clip->data, payload, size);
	clip->size = size;
	wlr_data_source_init(&clip->source, &clipboard_source_impl);
	if (!clipboard_source_add_mime(&clip->source, CLIPBOARD_MIME_UTF8) ||
			!clipboard_source_add_mime(&clip->source, CLIPBOARD_MIME_TEXT)) {
		wlr_data_source_destroy(&clip->source);
		return;
	}
	wlr_seat_set_selection(server.seat, &clip->source, wl_display_next_serial(server.display));
}

static const char *pick_text_mime(struct wlr_data_source *source) {
	const char *fallback = NULL;
	char **mime;
	wl_array_for_each(mime, &source->mime_types) {
		if (strcmp(*mime, CLIPBOARD_MIME_UTF8) == 0)
			return CLIPBOARD_MIME_UTF8;
		if (strcmp(*mime, CLIPBOARD_MIME_TEXT) == 0)
			fallback = CLIPBOARD_MIME_TEXT;
	}
	return fallback;
}

void mango_private_clear_output_messages(struct anland_backend *backend) {
    if (backend->output_timer) wl_event_source_remove(backend->output_timer);
    backend->output_timer = NULL;
    /* The public context clears its queue on session drop/disconnect. */
}

static int flush_output_messages(void *data) {
    struct anland_backend *backend = data;
    int rc = anland_device_flush_output(backend->device);
    if (rc < 0) { mango_private_drop_session(backend); return 0; }
    if (rc > 0) {
        wl_event_source_timer_update(backend->output_timer, 10);
        return 0;
    }
    mango_private_clear_output_messages(backend);
    return 0;
}

static bool queue_clipboard_text(struct anland_backend *backend,
        const void *payload, size_t size) {
    if (!anland_device_is_connected(backend->device)) return false;
    if (!backend->output_timer) {
        backend->output_timer = wl_event_loop_add_timer(backend->loop,
            flush_output_messages, backend);
        if (!backend->output_timer) return false;
    }
    if (anland_device_queue_clipboard(backend->device, payload, size) < 0) {
        if (anland_device_pending_output(backend->device) == 0)
            mango_private_clear_output_messages(backend);
        return false;
    }
    /* Wayland 0ms disarms a timer. Retry even after a budget-limited flush. */
    if (wl_event_source_timer_update(backend->output_timer, 1) < 0) {
        mango_private_drop_session(backend);
        return false;
    }
    return true;
}

struct anland_clipboard_read {
	struct anland_backend *backend;
	struct wl_event_source *source, *timer;
	int fd;
	char *data;
	size_t size, capacity;
};

static void clipboard_read_destroy(struct anland_clipboard_read *transfer) {
	if (transfer->source) wl_event_source_remove(transfer->source);
	if (transfer->timer) wl_event_source_remove(transfer->timer);
	if (transfer->fd >= 0) close(transfer->fd);
	transfer->backend->clipboard_read = NULL;
	free(transfer->data);
	free(transfer);
}

static void finish_clipboard_read(struct anland_clipboard_read *transfer) {
    /* Queue/timer failure may drop the session. The current callback owns this
     * transfer until return; detach it before pre-release can cancel it again. */
    transfer->backend->clipboard_read = NULL;
	if (transfer->size > 0 && anland_device_is_connected(transfer->backend->device)) {
		queue_clipboard_text(transfer->backend, transfer->data, transfer->size);
	}
	clipboard_read_destroy(transfer);
}

static int handle_clipboard_timeout(void *data) {
	clipboard_read_destroy(data);
	return 0;
}

static int handle_clipboard_read(int fd, uint32_t mask, void *data) {
	struct anland_clipboard_read *transfer = data;
	if (mask & WL_EVENT_ERROR) {
		clipboard_read_destroy(transfer);
		return 0;
	}
	for (;;) {
		if (transfer->size == transfer->capacity) {
			size_t next = transfer->capacity ? transfer->capacity * 2 : 4096;
			if (next > INPUT_PAYLOAD_MAX) next = INPUT_PAYLOAD_MAX;
			if (next == transfer->capacity) {
                char extra;
                ssize_t n = read(fd, &extra, 1);
                if (n == 0) finish_clipboard_read(transfer);
                else if (n > 0) {
                    wlr_log(WLR_ERROR, "Anland clipboard rejected: payload exceeds %u bytes",
                        (unsigned)INPUT_PAYLOAD_MAX);
                    clipboard_read_destroy(transfer);
                } else if (errno == EINTR) continue;
                else if (errno != EAGAIN && errno != EWOULDBLOCK)
                    clipboard_read_destroy(transfer);
                else if (mask & WL_EVENT_HANGUP) finish_clipboard_read(transfer);
                return 0;
            }
			char *tmp = realloc(transfer->data, next);
			if (!tmp) { clipboard_read_destroy(transfer); return 0; }
			transfer->data = tmp;
			transfer->capacity = next;
		}
		ssize_t n = read(fd, transfer->data + transfer->size,
			transfer->capacity - transfer->size);
		if (n > 0) { transfer->size += (size_t)n; continue; }
		if (n == 0) { finish_clipboard_read(transfer); return 0; }
		if (errno == EINTR) continue;
		if (errno == EAGAIN || errno == EWOULDBLOCK) break;
		clipboard_read_destroy(transfer);
		return 0;
	}
	if (mask & WL_EVENT_HANGUP) finish_clipboard_read(transfer);
	return 0;
}

void mango_private_cancel_clipboard_read(struct anland_backend *backend) {
	if (backend->clipboard_read)
		clipboard_read_destroy(backend->clipboard_read);
}

static void send_selection_to_consumer(struct anland_backend *backend,
		struct wlr_data_source *source) {
	mango_private_cancel_clipboard_read(backend);
	if (!source || source->impl == &clipboard_source_impl ||
			!anland_device_is_connected(backend->device)) return;
	const char *mime = pick_text_mime(source);
	if (!mime) return;
	int pipefd[2];
	if (pipe2(pipefd, O_CLOEXEC | O_NONBLOCK) < 0) return;
	struct anland_clipboard_read *transfer = calloc(1, sizeof(*transfer));
	if (!transfer) { close(pipefd[0]); close(pipefd[1]); return; }
	transfer->backend = backend;
	transfer->fd = pipefd[0];
	backend->clipboard_read = transfer;
	transfer->source = wl_event_loop_add_fd(backend->loop, pipefd[0],
		WL_EVENT_READABLE, handle_clipboard_read, transfer);
	transfer->timer = wl_event_loop_add_timer(backend->loop,
		handle_clipboard_timeout, transfer);
	if (!transfer->source || !transfer->timer) {
		if (transfer->source) wl_event_source_remove(transfer->source);
		if (transfer->timer) wl_event_source_remove(transfer->timer);
		backend->clipboard_read = NULL;
		close(pipefd[0]); close(pipefd[1]); free(transfer); return;
	}
	wl_event_source_timer_update(transfer->timer, CLIPBOARD_TRANSFER_TIMEOUT_MS);
	wlr_data_source_send(source, mime, pipefd[1]);
	wl_display_flush_clients(server.display);
}

void mango_private_handle_seat_set_selection(struct wl_listener *listener, void *data) {
	struct anland_backend *backend = wl_container_of(listener, backend,
		selection_listener);
	send_selection_to_consumer(backend,
		server.seat ? server.seat->selection_source : NULL);
}
#endif
