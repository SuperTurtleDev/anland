#include "internal.h"
#ifdef HAVE_ANLAND
static void remove_consumer_sources(struct anland_backend *backend) {
    if (backend->data_source) wl_event_source_remove(backend->data_source);
    if (backend->ready_source) wl_event_source_remove(backend->ready_source);
    backend->data_source = backend->ready_source = NULL;
}

/* Transport callback must not re-enter the public device/scene. Detach borrowed
 * fds and schedule the owner; public invalidation/events happen afterwards. */
static void producer_pre_release(void *data) {
    struct anland_backend *backend = data;
    backend->session_lost = true;
    remove_consumer_sources(backend);
    mango_private_cancel_clipboard_read(backend);
    mango_private_clear_clipboard_writes(backend);
    mango_private_clear_output_messages(backend);
    mango_private_clear_input_payload(backend);
    mango_anland_text_reset(backend->text);
    anland_audio_set_fd(-1);
    anland_camera_clear();
    backend->capture_mouse_known = false;
    if (backend->frame_timer) wl_event_source_timer_update(backend->frame_timer, 0);
    if (backend->pump_timer) wl_event_source_timer_update(backend->pump_timer, 0);
    if (backend->reconnect_source && !backend->destroying)
        wl_event_source_timer_update(backend->reconnect_source, RECONNECT_MS);
}
static void producer_fallback(void *data) {
    struct anland_backend *backend = data;
    if (backend->reconnect_source && !backend->destroying)
        wl_event_source_timer_update(backend->reconnect_source, RECONNECT_MS);
}

void mango_private_drop_session(struct anland_backend *backend) {
    producer_pre_release(backend);
    anland_de_backend_drop_session(backend->de);
    mango_private_dispatch_scene(backend);
    /* Signal only from the owner, never from transport pre-release. */
    mango_private_reset_input(backend);
    /* Retire native images only after public outcomes/releases have drained. */
    mango_private_drop_buffers(backend);
}

void mango_private_arm_frame(struct anland_backend *backend) {
    if (!backend->destroying && !backend->session_lost && backend->frame_timer)
        wl_event_source_timer_update(backend->frame_timer, 1);
}
static int frame_tick(void *data) {
    struct anland_backend *backend = data;
    anland_de_target_t target;
    struct anland_output *output = &backend->output;
    if (backend->destroying || backend->session_lost || !output->swapchain ||
        anland_de_backend_get_writable_target(backend->de, &target) != 0 ||
        target.generation != output->generation || target.count != output->buffer_count ||
        target.index >= target.count || output->active_commit ||
        output->buffers[target.index]->submitted) return 0;
    /* Includes static desktops: each requested target needs rendered content. */
    wlr_output_update_needs_frame(&output->base);
    wlr_output_send_frame(&output->base);
    return 0;
}

void mango_private_retry_pump(struct anland_backend *backend) {
    if (!backend->destroying && !backend->session_lost && backend->pump_timer)
        wl_event_source_timer_update(backend->pump_timer, 1);
}
static int pump_tick(void *data) {
    struct anland_backend *backend = data;
    mango_private_handle_ready(-1, WL_EVENT_READABLE, backend);
    return 0;
}

static int controls_tick(void *data) {
    struct anland_backend *backend = data;
    if (backend->destroying) return 0;
    if (!backend->session_lost && (backend->input_payload ||
            backend->input_fds_pending || anland_device_input_header_pending(backend->device)))
        mango_private_handle_input(-1, 0, backend);
    mango_private_sync_scheduling(backend, false);
    mango_private_sync_consumer_var(backend, false);
    if (backend->controls_timer)
        wl_event_source_timer_update(backend->controls_timer, 16);
    return 0;
}

static bool attach_sources(struct anland_backend *b) {
    int data_fd = anland_device_data_fd(b->device);
    int flags = fcntl(data_fd, F_GETFL);
    if (flags < 0 || fcntl(data_fd, F_SETFL, flags | O_NONBLOCK) < 0) return false;
    b->data_source=wl_event_loop_add_fd(b->loop,data_fd,WL_EVENT_READABLE,mango_private_handle_input,b);
    b->ready_source=wl_event_loop_add_fd(b->loop,anland_device_buffer_ready_fd(b->device),
        WL_EVENT_READABLE,mango_private_handle_ready,b);
    return b->data_source && b->ready_source;
}
static void announce_devices(struct anland_backend *b) {
    if (b->announced) return;
    b->announced = true;
    wl_signal_emit_mutable(&b->base.events.new_output,&b->output.base);
    wl_signal_emit_mutable(&b->base.events.new_input,&b->keyboard.base);
    wl_signal_emit_mutable(&b->base.events.new_input,&b->pointer.base);
    wl_signal_emit_mutable(&b->base.events.new_input,&b->touch.base);
    wl_signal_emit_mutable(&b->base.events.new_input,&b->tablet.base);
    mango_private_replay_tablets(b);
}
static int reconnect(void *data) {
    struct anland_backend *b=data;
    if (b->destroying) return 0;
    if (!b->session_lost && anland_device_is_connected(b->device)) return 0;
    /* Retire an old imported session once, not an in-progress handshake:
     * reconnect preserves partial replies and borrowed fds across EAGAIN. */
    if (!b->session_lost || b->output.swapchain)
        mango_private_drop_session(b);
    if (!anland_device_is_daemon_alive(b->device) &&
        anland_de_backend_reopen(b->de, b->socket_path) != 0) goto retry;
    if (anland_de_backend_reconnect(b->de) != 0) {
        mango_private_dispatch_scene(b); goto retry;
    }
    mango_private_dispatch_scene(b); /* publish validated target/output cache */
    anland_device_output_t info;
    if (anland_de_backend_get_output(b->de,&info) != 0) goto unusable;
    b->width=info.width; b->height=info.height; b->format=info.format; b->refresh=info.refresh_mhz;
    /* Pool arrival need not include the first buffer-ready notification.
     * Listen before waiting for a real target; keep this session on retries. */
    if (!b->ready_source && !attach_sources(b)) goto unusable;
    anland_de_target_t target;
    if (anland_de_backend_get_writable_target(b->de, &target) != 0) goto retry;
    if (!mango_private_import_buffers(b)) goto unusable;
    b->session_lost=false;
    anland_audio_set_fd(anland_device_audio_fd(b->device));
    mango_private_sync_scheduling(b, true);
    mango_private_sync_consumer_var(b, true);
    if (b->camera_started && anland_device_request_resources(b->device,
            ANLAND_DEVICE_SERVICE_CAMERA, NULL) != 0)
        goto unusable;
    if (b->session_lost || !anland_device_is_connected(b->device)) goto unusable;
    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_custom_mode(&state,b->width,b->height,b->refresh);
    if (b->announced)
        wlr_output_send_request_state(&b->output.base,&state);
    else {
        /* No mode existed during deferred discovery, so wlroots initialized
         * this unannounced output disabled. Mode + enable must be atomic:
         * a mode-only commit is rejected before our output_impl is called.
         * Renderer initialization still belongs to Mango's new_output handler. */
        wlr_output_state_set_enabled(&state, true);
        if (!wlr_output_commit_state(&b->output.base,&state)) {
            wlr_output_state_finish(&state); goto unusable;
        }
    }
    wlr_output_state_finish(&state);
    announce_devices(b);
    mango_private_arm_frame(b);
    return 0;
unusable:
    mango_private_drop_session(b);
retry:
    wl_event_source_timer_update(b->reconnect_source,RECONNECT_MS);
    return 0;
}

static bool backend_start(struct wlr_backend *base) {
	struct anland_backend *b=wl_container_of(base,b,base);
	if (anland_audio_start() == 0)
		b->audio_started = true;
	if (anland_camera_start() == 0)
		b->camera_started = true;
	else
		wlr_log(WLR_ERROR, "anland: failed to start camera engine");
	if (server.seat && !b->selection_listener_set) {
		b->selection_listener.notify = mango_private_handle_seat_set_selection;
		wl_signal_add(&server.seat->events.set_selection, &b->selection_listener);
		b->selection_listener_set = true;
	}
	if (server.seat && !b->focus_listener_set) {
		b->focus_change.notify = mango_private_handle_focus_change;
		wl_signal_add(&server.seat->keyboard_state.events.focus_change,
			&b->focus_change);
		b->focus_listener_set = true;
	}
	if (!b->text) {
		b->text = mango_anland_text_create(b->loop, server.seat);
		if (!b->text) return false;
		mango_anland_text_set_ime_grab(b->text, b->ime_grabbed);
	}
	if (b->controls_timer)
		wl_event_source_timer_update(b->controls_timer, 16);
	reconnect(b);
	return true;
}
static void backend_destroy(struct wlr_backend *base) {
    struct anland_backend *b = wl_container_of(base, b, base);
    b->destroying = true;
    if (b->selection_listener_set) wl_list_remove(&b->selection_listener.link);
    if (b->focus_listener_set) wl_list_remove(&b->focus_change.link);
    if (b->reconnect_source) wl_event_source_remove(b->reconnect_source);
    b->reconnect_source = NULL;
    mango_private_drop_session(b);
    mango_anland_text_destroy(b->text);
    b->text = NULL;
    if (b->scene_source) wl_event_source_remove(b->scene_source);
    if (b->frame_timer) wl_event_source_remove(b->frame_timer);
    if (b->pump_timer) wl_event_source_remove(b->pump_timer);
    if (b->controls_timer) wl_event_source_remove(b->controls_timer);
    b->scene_source = b->frame_timer = b->pump_timer = b->controls_timer = NULL;
    if (b->audio_started) anland_audio_stop();
    if (b->camera_started) anland_camera_stop();
    if (b->output_initialized) wlr_output_destroy(&b->output.base);
    anland_de_backend_destroy(b->de);
    b->de = NULL; b->device = NULL;
    if (b->drm_fd >= 0) close(b->drm_fd);
    wlr_keyboard_finish(&b->keyboard);
    wlr_pointer_finish(&b->pointer);
    wlr_touch_finish(&b->touch);
    wlr_tablet_finish(&b->tablet);
    wlr_backend_finish(base);
    free(b->socket_path);
    free(b);
}
static int backend_get_drm_fd(struct wlr_backend *base) {
	struct anland_backend *b=wl_container_of(base,b,base);
	return b->drm_fd;
}
static const struct wlr_backend_impl backend_impl={.start=backend_start,.destroy=backend_destroy,.get_drm_fd=backend_get_drm_fd};
void mango_anland_set_ime_grab(struct wlr_backend *base, bool grabbed) {
	if (!base || base->impl != &backend_impl) return;
	struct anland_backend *b = wl_container_of(base, b, base);
	b->ime_grabbed = grabbed;
	mango_anland_text_set_ime_grab(b->text, grabbed);
}
struct wlr_backend *mango_anland_backend_create(struct wl_event_loop *loop,const char *socket_path) {
	struct anland_backend *b=calloc(1,sizeof(*b)); if(!b) return NULL;
	b->socket_path = strdup(socket_path ? socket_path : "");
	if (!b->socket_path) { free(b); return NULL; }
	b->loop=loop; b->drm_fd=-1;
	wl_list_init(&b->clipboard_writes);
	const char *drm_path=getenv("ANLAND_DRM_DEVICE");
	if(!drm_path||!*drm_path) drm_path=getenv("WLR_RENDER_DRM_DEVICE");
	if(drm_path&&*drm_path) {
		b->drm_fd=open(drm_path,O_RDWR|O_CLOEXEC);
		if(b->drm_fd<0) {
			wlr_log_errno(WLR_ERROR, "anland: cannot open DRM device %s", drm_path);
			goto fail;
		}
	}
    const anland_de_backend_config_t config = {
        .present = {.endpoint = socket_path, .defer_connector = true}, .name = "mango",
    };
    b->de = anland_de_backend_create(&config);
    if (!b->de) {
        wlr_log_errno(WLR_ERROR, "anland: shared device handshake failed at %s", b->socket_path);
        goto fail;
    }
    b->device = anland_de_backend_device(b->de);
    anland_device_output_t info;
    if (anland_device_get_outputs(b->device, &info, 1) != 1)
        goto fail;
    b->width=info.width; b->height=info.height; b->format=info.format; b->refresh=info.refresh_mhz;
    b->session_lost=true;
    const anland_layer_desc_t layer = {
        .kind=ANLAND_LAYER_NORMAL, .name="Mango complete desktop", .opacity=1, .visible=true,
    };
    if (anland_de_backend_add_window_desc(b->de, 1, &layer, &b->output_layer) != 0)
        goto fail;
    wlr_log(WLR_INFO, "anland: shared device %s, screen %ux%u format=%u refresh=%u",
        anland_device_socket_path(b->device), b->width, b->height, b->format, b->refresh);
	wlr_backend_init(&b->base,&backend_impl);
	b->base.buffer_caps=WLR_BUFFER_CAP_DMABUF;
	struct wlr_output_state state;
	wlr_output_state_init(&state);
	if (b->width && b->height)
        wlr_output_state_set_custom_mode(&state,b->width,b->height,b->refresh);
	b->output.backend=b;
	wlr_output_init(&b->output.base,&b->base,&mango_private_output_impl,loop,&state);
	b->output_initialized = true;
	wlr_output_state_finish(&state);
	wlr_output_set_name(&b->output.base,"ANLAND-1");
	wlr_output_set_description(&b->output.base,"Anland 5 display");
	wlr_keyboard_init(&b->keyboard,&mango_private_keyboard_impl,"Anland keyboard");
	wlr_pointer_init(&b->pointer,&mango_private_pointer_impl,"Anland pointer");
	wlr_touch_init(&b->touch,&mango_private_touch_impl,"Anland touch");
    wlr_tablet_init(&b->tablet,&mango_private_tablet_impl,"Anland tablet");
    b->reconnect_source=wl_event_loop_add_timer(loop,reconnect,b);
    b->frame_timer=wl_event_loop_add_timer(loop,frame_tick,b);
    b->pump_timer=wl_event_loop_add_timer(loop,pump_tick,b);
    b->controls_timer=wl_event_loop_add_timer(loop,controls_tick,b);
    b->scene_source=wl_event_loop_add_fd(loop,
        anland_scene_event_fd(anland_de_backend_scene(b->de)), WL_EVENT_READABLE,
        mango_private_handle_scene,b);
    if (!b->reconnect_source || !b->frame_timer || !b->pump_timer ||
            !b->controls_timer || !b->scene_source)
        goto fail_initialized;
    anland_device_set_pre_release_cb(b->device, producer_pre_release, b);
    anland_device_set_fallback_cb(b->device, producer_fallback, b);
	return &b->base;
fail_initialized:
    if (b->reconnect_source) wl_event_source_remove(b->reconnect_source);
    if (b->frame_timer) wl_event_source_remove(b->frame_timer);
    if (b->pump_timer) wl_event_source_remove(b->pump_timer);
    if (b->controls_timer) wl_event_source_remove(b->controls_timer);
    if (b->scene_source) wl_event_source_remove(b->scene_source);
	b->scene_source=b->frame_timer=b->pump_timer=b->controls_timer=b->reconnect_source=NULL;
    b->destroying=true;
    wlr_output_destroy(&b->output.base);
	wlr_keyboard_finish(&b->keyboard);
	wlr_pointer_finish(&b->pointer);
	wlr_touch_finish(&b->touch);
    wlr_tablet_finish(&b->tablet);
	wlr_backend_finish(&b->base);
fail:
	if(b->de) anland_de_backend_destroy(b->de);
	if(b->drm_fd>=0) close(b->drm_fd);
	free(b->socket_path);
	free(b);
	return NULL;
}
#endif
