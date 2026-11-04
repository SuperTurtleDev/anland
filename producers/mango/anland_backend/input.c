#include "internal.h"
#ifdef HAVE_ANLAND
const struct wlr_keyboard_impl mango_private_keyboard_impl = {.name = "anland-keyboard"};
const struct wlr_pointer_impl mango_private_pointer_impl = {.name = "anland-pointer"};
const struct wlr_touch_impl mango_private_touch_impl = {.name = "anland-touch"};
const struct wlr_tablet_impl mango_private_tablet_impl = {.name = "anland-tablet"};
bool mango_anland_tablet_is(struct wlr_tablet *tablet) {
    return tablet && tablet->impl == &mango_private_tablet_impl;
}

uint32_t mango_private_now_msec(void) {
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000ULL + ts.tv_nsec / 1000000ULL);
}
static double normalized(float value, uint32_t extent) {
	if (extent == 0 || value <= 0) return 0;
	if (value >= extent) return 1;
	return value / extent;
}
static void emit_pointer_frame(struct anland_backend *b) {
	wl_signal_emit_mutable(&b->pointer.events.frame, &b->pointer);
}
static void emit_tablet_frame(struct anland_backend *b,
        const anland_tablet_sample *previous, const anland_tablet_sample *sample) {
    struct wlr_tablet_tool *tool = NULL;
    unsigned index;
    for (index = 0; index < ANLAND_TABLET_MAX_TOOLS; ++index)
        if (b->tablet_tool_ids[index] == sample->tool_id) { tool = &b->tablet_tools[index]; break; }
    if (!tool) for (index = 0; index < ANLAND_TABLET_MAX_TOOLS; ++index) {
        if (!b->tablet_tool_ids[index]) {
            tool = &b->tablet_tools[index];
            b->tablet_tool_ids[index] = sample->tool_id;
            tool->type = sample->tool_type == TABLET_TOOL_ERASER ? WLR_TABLET_TOOL_TYPE_ERASER : WLR_TABLET_TOOL_TYPE_PEN;
            tool->hardware_serial = ((uint64_t)sample->device_id << 32) | sample->tool_id;
            tool->pressure = sample->capabilities & TABLET_CAP_PRESSURE;
            tool->tilt = sample->capabilities & TABLET_CAP_TILT;
            tool->distance = sample->capabilities & TABLET_CAP_DISTANCE;
            wl_signal_init(&tool->events.destroy);
            break;
        }
    }
    if (!tool) return;
    bool in = sample->flags & TABLET_FLAG_PROXIMITY, was_in = previous->flags & TABLET_FLAG_PROXIMITY;
    bool down = sample->flags & TABLET_FLAG_TIP_DOWN, was_down = previous->flags & TABLET_FLAG_TIP_DOWN;
    double x = normalized(sample->x, b->width), y = normalized(sample->y, b->height);
    if (in && !was_in) {
        struct wlr_tablet_tool_proximity_event e = {.tablet=&b->tablet,.tool=tool,.time_msec=sample->time_msec,
            .x=x,.y=y,.state=WLR_TABLET_TOOL_PROXIMITY_IN};
        wl_signal_emit_mutable(&b->tablet.events.proximity, &e);
    }
    if (was_down && !down) {
        struct wlr_tablet_tool_tip_event e = {.tablet=&b->tablet,.tool=tool,.time_msec=sample->time_msec,
            .x=x,.y=y,.state=WLR_TABLET_TOOL_TIP_UP};
        wl_signal_emit_mutable(&b->tablet.events.tip, &e);
    }
    if (in) {
        uint32_t axes = WLR_TABLET_TOOL_AXIS_X | WLR_TABLET_TOOL_AXIS_Y;
        if (tool->pressure) axes |= WLR_TABLET_TOOL_AXIS_PRESSURE;
        if (tool->tilt) axes |= WLR_TABLET_TOOL_AXIS_TILT_X | WLR_TABLET_TOOL_AXIS_TILT_Y;
        if (tool->distance) axes |= WLR_TABLET_TOOL_AXIS_DISTANCE;
        struct wlr_tablet_tool_axis_event e = {.tablet=&b->tablet,.tool=tool,.time_msec=sample->time_msec,
            .updated_axes=axes,.x=x,.y=y,
            .dx=was_in ? x-normalized(previous->x,b->width) : 0,
            .dy=was_in ? y-normalized(previous->y,b->height) : 0,
            .pressure=sample->pressure,.distance=sample->distance,.tilt_x=sample->tilt_x,.tilt_y=sample->tilt_y};
        wl_signal_emit_mutable(&b->tablet.events.axis, &e);
    }
    if (down && !was_down) {
        struct wlr_tablet_tool_tip_event e = {.tablet=&b->tablet,.tool=tool,.time_msec=sample->time_msec,
            .x=x,.y=y,.state=WLR_TABLET_TOOL_TIP_DOWN};
        wl_signal_emit_mutable(&b->tablet.events.tip, &e);
    }
    for (unsigned bit=0;bit<2;++bit) if ((previous->buttons ^ sample->buttons) & (1u<<bit)) {
        struct wlr_tablet_tool_button_event e = {.tablet=&b->tablet,.tool=tool,.time_msec=sample->time_msec,
            .button=0x14bu+bit,.state=(sample->buttons & (1u<<bit)) ? WLR_BUTTON_PRESSED : WLR_BUTTON_RELEASED};
        wl_signal_emit_mutable(&b->tablet.events.button, &e);
    }
    if (!in && was_in) {
        struct wlr_tablet_tool_proximity_event e = {.tablet=&b->tablet,.tool=tool,.time_msec=sample->time_msec,
            .x=x,.y=y,.state=WLR_TABLET_TOOL_PROXIMITY_OUT};
        wl_signal_emit_mutable(&b->tablet.events.proximity, &e);
    }
}
void mango_private_clear_input_payload(struct anland_backend *backend) {
	free(backend->input_payload);
	backend->input_payload = NULL;
	backend->input_payload_size = 0;
	backend->input_fds_pending = false;
	backend->input_fd_count = 0;
	backend->input_payload_type = 0;
	backend->input_resource_type = 0;
	backend->input_resource_invalid = false;
}

static bool track_input_id(uint32_t *ids, uint32_t *count, uint32_t id, bool down) {
    for (uint32_t i = 0; i < *count; ++i) {
        if (ids[i] != id) continue;
        if (!down) ids[i] = ids[--*count];
        return true;
    }
    if (!down) return true;
    if (*count == INPUT_PRESSED_MAX) return false;
    ids[(*count)++] = id;
    return true;
}

static pid_t focused_client_pid(void) {
	struct wlr_surface *surface = server.seat ?
		server.seat->keyboard_state.focused_surface : NULL;
	pid_t pid = 0;
	uid_t uid = 0;
	gid_t gid = 0;
	if (surface && surface->resource) {
		struct wl_client *client = wl_resource_get_client(surface->resource);
		if (client)
			wl_client_get_credentials(client, &pid, &uid, &gid);
	}
	return pid > 0 ? pid : getpid();
}

void mango_private_sync_scheduling(struct anland_backend *backend, bool force) {
	if (!backend || !backend->device) return;
	pid_t wanted = focused_client_pid();
	if (!force && wanted == backend->active_scheduling_pid) return;
	if (!force && !backend->session_lost &&
			anland_device_is_connected(backend->device) &&
			backend->active_scheduling_pid > 0) {
		if (anland_device_scheduling(backend->device,
				backend->active_scheduling_pid,
				ANLAND_DEVICE_SCHED_FLAG_SETTREE) != 0) {
			wlr_log(WLR_ERROR, "anland: failed to restore previous scheduling target");
			return;
		}
	}
	if (!backend->session_lost && anland_device_is_connected(backend->device) &&
			anland_device_scheduling(backend->device, wanted,
			ANLAND_DEVICE_SCHED_FLAG_SETTREE | ANLAND_DEVICE_SCHED_FLAG_ON) != 0) {
		backend->active_scheduling_pid = 0;
		wlr_log(WLR_ERROR, "anland: failed to promote focused process");
		return;
	}
	if (!backend->session_lost && anland_device_is_connected(backend->device))
		backend->active_scheduling_pid = wanted;
}

void mango_private_sync_consumer_var(struct anland_backend *backend, bool force) {
	if (!backend || !backend->device || backend->session_lost ||
			!anland_device_is_connected(backend->device)) {
		if (backend) backend->capture_mouse_known = false;
		return;
	}
	bool capture = server.active_constraint != NULL;
	if (!force && backend->capture_mouse_known &&
			backend->capture_mouse_sent == capture) return;
	if (anland_device_set_consumer_var(backend->device,
			ANLAND_DEVICE_VAR_CAPTURE_MOUSE, capture ? 1 : 0) == 0) {
		backend->capture_mouse_sent = capture;
		backend->capture_mouse_known = true;
	} else {
		backend->capture_mouse_known = false;
		wlr_log(WLR_ERROR, "anland: failed to update pointer capture state");
	}
}

void mango_private_handle_focus_change(struct wl_listener *listener, void *data) {
	(void)data;
	struct anland_backend *backend = wl_container_of(listener, backend, focus_change);
	mango_private_sync_scheduling(backend, false);
	mango_private_sync_consumer_var(backend, false);
}

void mango_private_replay_tablets(struct anland_backend *b) {
    /* Input can arrive after fd attachment but before the first output target.
     * Register the native device before publishing any cached proximity/tip. */
    for (unsigned i=0;i<ANLAND_TABLET_MAX_TOOLS;++i) {
        const anland_tablet_slot *slot = &b->tablet_state.tools[i];
        if (!slot->occupied || !(slot->current.flags & TABLET_FLAG_PROXIMITY)) continue;
        anland_tablet_sample previous = slot->current;
        previous.flags = previous.buttons = 0;
        previous.pressure = 0;
        emit_tablet_frame(b, &previous, &slot->current);
    }
}

void mango_private_reset_input(struct anland_backend *b) {
    if (!b->announced) {
        memset(&b->tablet_state, 0, sizeof(b->tablet_state));
        return;
    }
    uint32_t time = mango_private_now_msec();
    unsigned cursor = 0;
    anland_tablet_sample previous, sample;
    while (anland_tablet_release_next(&b->tablet_state, &cursor, &previous, &sample)) {
        sample.time_msec = time;
        emit_tablet_frame(b, &previous, &sample);
    }
    for (unsigned i=0;i<ANLAND_TABLET_MAX_TOOLS;++i) if (b->tablet_tool_ids[i]) {
        wl_signal_emit_mutable(&b->tablet_tools[i].events.destroy, &b->tablet_tools[i]);
        memset(&b->tablet_tools[i], 0, sizeof(b->tablet_tools[i]));
        b->tablet_tool_ids[i] = 0;
    }
    while (b->keyboard.num_keycodes) {
        struct wlr_keyboard_key_event e = {.time_msec=time,
            .keycode=b->keyboard.keycodes[b->keyboard.num_keycodes - 1],
            .update_state=true, .state=WL_KEYBOARD_KEY_STATE_RELEASED};
        wlr_keyboard_notify_key(&b->keyboard, &e);
    }
    bool buttons = b->button_count != 0;
    while (b->button_count) {
        struct wlr_pointer_button_event e = {.pointer=&b->pointer, .time_msec=time,
            .button=b->pressed_buttons[--b->button_count],
            .state=WL_POINTER_BUTTON_STATE_RELEASED};
        wlr_pointer_notify_button(&b->pointer, &e);
    }
    if (buttons) emit_pointer_frame(b);
    bool touches = b->touch_count != 0;
    while (b->touch_count) {
        struct wlr_touch_cancel_event e = {.touch=&b->touch, .time_msec=time,
            .touch_id=(int32_t)b->touch_ids[--b->touch_count]};
        wl_signal_emit_mutable(&b->touch.events.cancel, &e);
    }
    if (touches) wl_signal_emit_mutable(&b->touch.events.frame, &b->touch);
}

/* The public library owns partial socket reads, framing checks and deadlines.
 * This adapter owns only the completed payload and its wlroots translation. */
static bool drain_input_payload(struct anland_backend *backend) {
	int rc = anland_device_read_input(backend->device, backend->input_payload,
		backend->input_payload_size, 0);
	if (rc <= 0) return false;
	backend->input_payload[backend->input_payload_size] = '\0';
	if (backend->input_payload_type == ANLAND_DEVICE_IN_TEXT_INPUT) {
		if (!mango_anland_text_submit(backend->text, server.input_method_relay,
				backend->input_payload, backend->input_payload_size))
			wlr_log(WLR_ERROR, "Anland text input not queued (%zu bytes): check focus, UTF-8, v3 size or local keyboard queue",
				backend->input_payload_size);
	} else
		mango_private_set_clipboard_from_consumer(backend, backend->input_payload,
			backend->input_payload_size);
	mango_private_clear_input_payload(backend);
	return true;
}

/* Consume every ancillary message, including invalidated/unsupported resources,
 * before polling the next input header. The camera engine adopts valid fds. */
static bool drain_input_fds(struct anland_backend *backend) {
	int fds[64], count = 0;
	int rc = anland_device_read_fds(backend->device, fds, 64, &count, 0);
	if (rc <= 0) return false;
	bool valid = (uint32_t)count == backend->input_fd_count;
	uint32_t resource_type = backend->input_resource_type;
	bool invalid = backend->input_resource_invalid;
	backend->input_fds_pending = false;
	backend->input_fd_count = 0;
	backend->input_resource_type = 0;
	backend->input_resource_invalid = false;
	if (valid && !invalid && resource_type == ANLAND_DEVICE_SERVICE_CAMERA &&
			backend->camera_started && count >= 2) {
		anland_camera_set_resources(fds[0], &fds[1], count - 1);
	} else {
		for (int i = 0; i < count; ++i) close(fds[i]);
		if (valid && resource_type == ANLAND_DEVICE_SERVICE_CAMERA)
			anland_camera_clear();
	}
	if (!valid) mango_private_drop_session(backend);
	return valid;
}

int mango_private_handle_input(int fd, uint32_t mask, void *data) {
	(void)fd;
	struct anland_backend *b = data;
	if (mask & (WL_EVENT_ERROR | WL_EVENT_HANGUP)) {
		mango_private_drop_session(b); return 0;
	}
	if (b->input_payload && !drain_input_payload(b)) return 0;
	if (b->input_fds_pending && !drain_input_fds(b)) return 0;
	anland_device_input_t ev;
	int ret;
	while ((ret = anland_device_poll_input(b->device, &ev, 0)) > 0) {
		uint32_t time = mango_private_now_msec();
		if (anland_tablet_is_event(ev.type)) {
            anland_tablet_sample previous, sample;
            int status = anland_tablet_feed(&b->tablet_state, &ev, &previous, &sample);
            if (status < 0) { mango_private_drop_session(b); return 0; }
            if (status > 0 && b->announced) emit_tablet_frame(b, &previous, &sample);
            continue;
        }
		switch (ev.type) {
		case ANLAND_DEVICE_IN_KEY: {
			struct wlr_keyboard_key_event e = {.time_msec=time, .keycode=ev.key.keycode,
				.update_state=true, .state=ev.key.action == ANLAND_DEVICE_ACTION_DOWN};
			if (!mango_anland_text_handle_key(b->text, &b->keyboard, &e)) {
				mango_private_drop_session(b); return 0;
			}
			break;
		}
		case ANLAND_DEVICE_IN_PTR_MOTION: {
			/* One packet must move the cursor only once. Use raw deltas for a
			 * locked client; normal desktop motion uses authoritative x/y. */
			if (server.active_constraint &&
					server.active_constraint->type == WLR_POINTER_CONSTRAINT_V1_LOCKED) {
				struct wlr_pointer_motion_event e = {.pointer=&b->pointer,
					.time_msec=time, .delta_x=ev.pointer_motion.dx,
					.delta_y=ev.pointer_motion.dy, .unaccel_dx=ev.pointer_motion.dx,
					.unaccel_dy=ev.pointer_motion.dy};
				wl_signal_emit_mutable(&b->pointer.events.motion, &e);
			} else {
				struct wlr_pointer_motion_absolute_event e = {.pointer=&b->pointer,
					.time_msec=time, .x=normalized(ev.pointer_motion.x,b->width),
					.y=normalized(ev.pointer_motion.y,b->height)};
				wl_signal_emit_mutable(&b->pointer.events.motion_absolute, &e);
			}
			emit_pointer_frame(b); break;
		}
		case ANLAND_DEVICE_IN_PTR_BUTTON: {
            if (!track_input_id(b->pressed_buttons, &b->button_count,
                    ev.pointer_button.button, ev.pointer_button.pressed)) {
                mango_private_drop_session(b); return 0;
            }
			struct wlr_pointer_button_event e = {.pointer=&b->pointer,.time_msec=time,
				.button=ev.pointer_button.button,.state=ev.pointer_button.pressed};
			wlr_pointer_notify_button(&b->pointer,&e); emit_pointer_frame(b); break;
		}
		case ANLAND_DEVICE_IN_PTR_AXIS: {
			struct wlr_pointer_axis_event e = {.pointer=&b->pointer,.time_msec=time,
				.source=WL_POINTER_AXIS_SOURCE_WHEEL,
				.orientation=ev.pointer_axis.axis == 0 ? WL_POINTER_AXIS_VERTICAL_SCROLL : WL_POINTER_AXIS_HORIZONTAL_SCROLL,
				.relative_direction=WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL,
				.delta=ev.pointer_axis.value,
				.delta_discrete=ev.pointer_axis.discrete * WLR_POINTER_AXIS_DISCRETE_STEP};
			wl_signal_emit_mutable(&b->pointer.events.axis,&e); emit_pointer_frame(b); break;
		}
		case ANLAND_DEVICE_IN_TOUCH: {
            if (ev.touch.action != ANLAND_DEVICE_ACTION_MOVE &&
                    !track_input_id(b->touch_ids, &b->touch_count,
                        (uint32_t)ev.touch.pointer_id, ev.touch.action == ANLAND_DEVICE_ACTION_DOWN)) {
                mango_private_drop_session(b); return 0;
            }
			double x=normalized(ev.touch.x,b->width), y=normalized(ev.touch.y,b->height);
			if (ev.touch.action == ANLAND_DEVICE_ACTION_DOWN) { struct wlr_touch_down_event e={.touch=&b->touch,.time_msec=time,.touch_id=ev.touch.pointer_id,.x=x,.y=y}; wl_signal_emit_mutable(&b->touch.events.down,&e); }
			else if (ev.touch.action == ANLAND_DEVICE_ACTION_MOVE) { struct wlr_touch_motion_event e={.touch=&b->touch,.time_msec=time,.touch_id=ev.touch.pointer_id,.x=x,.y=y}; wl_signal_emit_mutable(&b->touch.events.motion,&e); }
			else { struct wlr_touch_up_event e={.touch=&b->touch,.time_msec=time,.touch_id=ev.touch.pointer_id}; wl_signal_emit_mutable(&b->touch.events.up,&e); } break;
		}
		case ANLAND_DEVICE_IN_TOUCH_FRAME: wl_signal_emit_mutable(&b->touch.events.frame,&b->touch); break;
		case ANLAND_DEVICE_IN_DISPLAY_REFRESH:
			if (ev.display.refresh_mhz && ev.display.refresh_mhz != b->refresh) {
				b->refresh=ev.display.refresh_mhz; struct wlr_output_state state;
				wlr_output_state_init(&state); wlr_output_state_set_custom_mode(&state,b->width,b->height,b->refresh);
				wlr_output_send_request_state(&b->output.base,&state); wlr_output_state_finish(&state);
			} break;
		case ANLAND_DEVICE_IN_CLIPBOARD: case ANLAND_DEVICE_IN_TEXT_INPUT: {
			size_t size = ev.type == ANLAND_DEVICE_IN_CLIPBOARD ? ev.clipboard.size : ev.text_input.size;
			if (size > INPUT_PAYLOAD_MAX) { mango_private_drop_session(b); return 0; }
			if (size == 0) break;
			b->input_payload = malloc(size + 1);
			if (!b->input_payload) { mango_private_drop_session(b); return 0; }
			b->input_payload_size = size;
			b->input_payload_type = ev.type;
			if (!drain_input_payload(b)) return 0;
			break;
		}
		case ANLAND_DEVICE_IN_RESOURCE:
		case ANLAND_DEVICE_IN_RESOURCE_INVALID:
			if (ev.resource.fdnum > 64) { mango_private_drop_session(b); return 0; }
			if (ev.resource.fdnum == 0) {
				if (ev.resource.type == ANLAND_DEVICE_SERVICE_CAMERA)
					anland_camera_clear();
				break;
			}
			b->input_fds_pending = true;
			b->input_fd_count = ev.resource.fdnum;
			b->input_resource_type = ev.resource.type;
			b->input_resource_invalid = ev.type == ANLAND_DEVICE_IN_RESOURCE_INVALID;
			if (!drain_input_fds(b)) return 0;
			break;
		default: break;
		}
	}
	if (ret < 0) mango_private_drop_session(b);
	return 0;
}
bool mango_anland_touch_is(struct wlr_touch *touch) {
	return touch && touch->impl == &mango_private_touch_impl;
}
#endif
