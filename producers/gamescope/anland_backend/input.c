#include "input.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void clear_pending(anland_gamescope_input *input)
{
    free(input->payload);
    input->payload = NULL;
    input->pending_type = input->pending_size = input->pending_service = 0;
    input->pending_text_identity = input->header_text_identity = 0;
    input->header_identity_valid = false;
}

void anland_gamescope_input_reset(anland_gamescope_input *input)
{
    memset(&input->tablet_state, 0, sizeof(input->tablet_state));
    clear_pending(input);
}

static int consume_pending(anland_gamescope_input *input, anland_device *device)
{
    int rc;
    if (input->pending_type == ANLAND_DEVICE_IN_RESOURCE ||
        input->pending_type == ANLAND_DEVICE_IN_RESOURCE_INVALID) {
        int fds[64], count = 0;
        rc = anland_device_read_fds(device, fds, 64, &count, 0);
        if (rc > 0) {
            bool valid = (uint32_t)count == input->pending_size;
            bool adopted = valid && input->pending_type == ANLAND_DEVICE_IN_RESOURCE && input->resources &&
                input->resources(input->resources_userdata, input->pending_service, fds, count);
            if (!adopted)
                for (int i = 0; i < count; ++i) close(fds[i]);
            if (!valid) rc = -1;
        }
    } else {
        rc = anland_device_read_input(device, input->payload, input->pending_size, 0);
        if (rc > 0 && input->text) {
            ((char *)input->payload)[input->pending_size] = 0;
            input->text(input->text_userdata, input->pending_type,
                        input->payload, input->pending_size,
                        input->pending_text_identity);
        }
    }
    if (rc != 0)
        clear_pending(input);
    return rc;
}

int anland_gamescope_input_pump(anland_gamescope_input *input, anland_device *device,
    uint32_t width, uint32_t height, anland_gamescope_input_sink sink, void *userdata)
{
    int emitted = 0;
    for (unsigned budget = 0; budget < 64; ++budget) {
        if (input->pending_type) {
            int rc = consume_pending(input, device);
            if (rc < 0) return -1;
            if (!rc) break;
        }
        const uint64_t observed_identity = input->text_identity ?
            input->text_identity(input->text_identity_userdata) : 0;
        if (!input->header_identity_valid)
            input->header_text_identity = observed_identity;
        anland_device_input_t raw;
        int rc = anland_device_poll_input(device, &raw, 0);
        if (rc < 0) return -1;
        if (!rc) {
            input->header_identity_valid =
                anland_device_input_header_pending(device);
            if (!input->header_identity_valid)
                input->header_text_identity = 0;
            break;
        }
        const uint64_t event_identity = input->header_identity_valid ?
            input->header_text_identity : observed_identity;
        input->header_identity_valid = false;
        const bool resource = raw.type == ANLAND_DEVICE_IN_RESOURCE ||
            raw.type == ANLAND_DEVICE_IN_RESOURCE_INVALID;
        if (raw.type == ANLAND_DEVICE_IN_CLIPBOARD ||
            raw.type == ANLAND_DEVICE_IN_TEXT_INPUT ||
            resource) {
            uint32_t size = resource ?
                raw.resource.fdnum : raw.clipboard.size;
            if (size > (resource ? 64u : ANLAND_DEVICE_MAX_PAYLOAD_SIZE))
                return -1;
            if (resource && (!size || raw.type == ANLAND_DEVICE_IN_RESOURCE_INVALID)) {
                anland_gamescope_input_event event = {0};
                event.kind = AG_RESOURCE_INVALID;
                event.code = (int32_t)raw.resource.type;
                sink(userdata, &event);
                ++emitted;
            }
            // A zero-resource response carries no following SCM_RIGHTS message.
            if (!size) {
                if (!resource && input->text)
                    input->text(input->text_userdata, raw.type, "", 0, event_identity);
                continue;
            }
            input->pending_type = raw.type;
            input->pending_size = size;
            input->pending_service = resource ? raw.resource.type : 0;
            if (raw.type == ANLAND_DEVICE_IN_TEXT_INPUT) {
                input->pending_text_identity = event_identity;
            } else {
                input->pending_text_identity = 0;
                input->header_identity_valid = false;
            }
            if (!resource) {
                input->payload = calloc((size_t)size + 4, 1);
                if (!input->payload) return -1;
            }
            continue;
        }
        anland_gamescope_input_event event = {0};
        if (anland_tablet_is_event(raw.type)) {
            anland_tablet_sample previous;
            int status = anland_tablet_feed(&input->tablet_state, &raw, &previous, &event.tablet);
            if (status < 0) return -1;
            if (!status) continue;
            event.kind = AG_TABLET;
            sink(userdata, &event);
            ++emitted;
            continue;
        }
        switch (raw.type) {
        case ANLAND_DEVICE_IN_KEY:
            if (raw.key.keycode < 0 || raw.key.keycode > 767 ||
                (raw.key.action != ANLAND_DEVICE_ACTION_DOWN && raw.key.action != ANLAND_DEVICE_ACTION_UP))
                continue;
            event.kind = AG_KEY; event.code = raw.key.keycode;
            event.pressed = raw.key.action == ANLAND_DEVICE_ACTION_DOWN;
            break;
        case ANLAND_DEVICE_IN_PTR_MOTION:
            event.kind = AG_MOTION; event.x = raw.pointer_motion.dx; event.y = raw.pointer_motion.dy;
            event.absolute_x = raw.pointer_motion.x;
            event.absolute_y = raw.pointer_motion.y;
            if (!isfinite(event.x) || !isfinite(event.y) ||
                !isfinite(event.absolute_x) || !isfinite(event.absolute_y)) continue;
            break;
        case ANLAND_DEVICE_IN_PTR_BUTTON:
            if (raw.pointer_button.button > 767) continue;
            event.kind = AG_BUTTON; event.code = (int32_t)raw.pointer_button.button;
            event.pressed = raw.pointer_button.pressed != 0;
            break;
        case ANLAND_DEVICE_IN_PTR_AXIS:
            if (raw.pointer_axis.axis > 1 || !isfinite(raw.pointer_axis.value) ||
                raw.pointer_axis.discrete > 1000000 || raw.pointer_axis.discrete < -1000000) continue;
            event.kind = AG_AXIS; event.code = raw.pointer_axis.axis;
            event.x = raw.pointer_axis.value; event.discrete = raw.pointer_axis.discrete;
            break;
        case ANLAND_DEVICE_IN_TOUCH:
            if (!width || !height || raw.touch.pointer_id < 0) continue;
            event.code = raw.touch.pointer_id;
            if (raw.touch.action == ANLAND_DEVICE_ACTION_UP) {
                event.kind = AG_TOUCH_UP;
            } else {
                if (!isfinite(raw.touch.x) || !isfinite(raw.touch.y)) continue;
                event.x = fmax(0.0, fmin(1.0, raw.touch.x / width));
                event.y = fmax(0.0, fmin(1.0, raw.touch.y / height));
                if (raw.touch.action == ANLAND_DEVICE_ACTION_DOWN) event.kind = AG_TOUCH_DOWN;
                else if (raw.touch.action == ANLAND_DEVICE_ACTION_MOVE) event.kind = AG_TOUCH_MOVE;
                else continue;
            }
            break;
        case ANLAND_DEVICE_IN_TOUCH_FRAME:
            event.kind = AG_TOUCH_FRAME;
            break;
        case ANLAND_DEVICE_IN_DISPLAY_REFRESH:
            if (!raw.display.refresh_mhz || raw.display.refresh_mhz > 1000000) continue;
            event.kind = AG_REFRESH; event.code = raw.display.refresh_mhz;
            break;
        default:
            continue;
        }
        sink(userdata, &event);
        ++emitted;
    }
    return emitted;
}
