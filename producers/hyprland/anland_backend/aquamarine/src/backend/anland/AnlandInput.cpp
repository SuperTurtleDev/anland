#include <aquamarine/backend/AnlandBackend.hpp>
#include "AnlandOutput.hpp"
#include "AnlandInput.hpp"
#include "anland_camera.h"
#include <algorithm>
#include <cmath>
#include <time.h>
#include <unistd.h>

using namespace Aquamarine;
using namespace Hyprutils::Math;
static uint32_t inputTime() { timespec now{}; clock_gettime(CLOCK_MONOTONIC, &now); return uint32_t(now.tv_sec * 1000 + now.tv_nsec / 1000000); }
void CAnlandBackend::tabletFrame(const anland_tablet_sample& previous, const anland_tablet_sample& sample) {
    if (!m_tablet || !m_width || !m_height) return;
    auto& tool = m_tablet->tools[sample.tool_id];
    if (!tool) {
        tool = Hyprutils::Memory::makeShared<CAnlandTabletTool>();
        tool->type = sample.tool_type == TABLET_TOOL_ERASER ? ITabletTool::AQ_TABLET_TOOL_TYPE_ERASER : ITabletTool::AQ_TABLET_TOOL_TYPE_PEN;
        tool->serial = (uint64_t(sample.device_id) << 32) | sample.tool_id;
        if (sample.capabilities & TABLET_CAP_PRESSURE) tool->capabilities |= ITabletTool::AQ_TABLET_TOOL_CAPABILITY_PRESSURE;
        if (sample.capabilities & TABLET_CAP_TILT) tool->capabilities |= ITabletTool::AQ_TABLET_TOOL_CAPABILITY_TILT;
        if (sample.capabilities & TABLET_CAP_DISTANCE) tool->capabilities |= ITabletTool::AQ_TABLET_TOOL_CAPABILITY_DISTANCE;
    }
    const Vector2D absolute{std::clamp(double(sample.x) / m_width, 0.0, 1.0), std::clamp(double(sample.y) / m_height, 0.0, 1.0)};
    const bool in = sample.flags & TABLET_FLAG_PROXIMITY, wasIn = previous.flags & TABLET_FLAG_PROXIMITY;
    const bool down = sample.flags & TABLET_FLAG_TIP_DOWN, wasDown = previous.flags & TABLET_FLAG_TIP_DOWN;
    const auto time = sample.time_msec;
    if (in && !wasIn)
        m_tablet->events.proximity.emit(ITablet::SProximityEvent{.tool=tool,.timeMs=time,.absolute=absolute,.in=true});
    if (wasDown && !down)
        m_tablet->events.tip.emit(ITablet::STipEvent{.tool=tool,.timeMs=time,.absolute=absolute,.down=false});
    if (in) {
        uint32_t axes = AQ_TABLET_TOOL_AXIS_X | AQ_TABLET_TOOL_AXIS_Y;
        if (sample.capabilities & TABLET_CAP_PRESSURE) axes |= AQ_TABLET_TOOL_AXIS_PRESSURE;
        if (sample.capabilities & TABLET_CAP_TILT) axes |= AQ_TABLET_TOOL_AXIS_TILT_X | AQ_TABLET_TOOL_AXIS_TILT_Y;
        if (sample.capabilities & TABLET_CAP_DISTANCE) axes |= AQ_TABLET_TOOL_AXIS_DISTANCE;
        m_tablet->events.axis.emit(ITablet::SAxisEvent{.tool=tool,.timeMs=time,.updatedAxes=axes,.absolute=absolute,
            .delta=wasIn ? Vector2D{(sample.x-previous.x)/m_width,(sample.y-previous.y)/m_height} : Vector2D{},
            .tilt={sample.tilt_x,sample.tilt_y},.pressure=sample.pressure,.distance=sample.distance});
    }
    if (down && !wasDown)
        m_tablet->events.tip.emit(ITablet::STipEvent{.tool=tool,.timeMs=time,.absolute=absolute,.down=true});
    for (unsigned i = 0; i < 2; ++i) if ((sample.buttons ^ previous.buttons) & (1u << i))
        m_tablet->events.button.emit(ITablet::SButtonEvent{.tool=tool,.timeMs=time,.button=0x14bu+i,.down=bool(sample.buttons & (1u<<i))});
    if (!in && wasIn)
        m_tablet->events.proximity.emit(ITablet::SProximityEvent{.tool=tool,.timeMs=time,.absolute=absolute,.in=false});
}
void CAnlandBackend::cancelTextQueuedKeys() { resetInput(); }
void CAnlandBackend::deliverTextQueuedKey(uint32_t key, bool pressed, uint32_t time) {
    if (pressed) m_keys.insert(key); else m_keys.erase(key);
    m_keyboard->events.key.emit(IKeyboard::SKeyEvent{.timeMs=time, .key=key, .pressed=pressed});
}
void CAnlandBackend::resetInput() {
    if (onInputReset) onInputReset();
    const auto time = inputTime();
    unsigned cursor = 0;
    anland_tablet_sample previous{}, sample{};
    while (m_tablet && anland_tablet_release_next(&m_tablet->state, &cursor, &previous, &sample)) {
        sample.time_msec = time;
        tabletFrame(previous, sample);
    }
    if (m_tablet) m_tablet->tools.clear();
    if (m_keyboard) for (auto key : m_keys) m_keyboard->events.key.emit(IKeyboard::SKeyEvent{.timeMs=time, .key=key, .pressed=false});
    if (m_pointer) {
        for (auto button : m_buttons) m_pointer->events.button.emit(IPointer::SButtonEvent{.timeMs=time, .button=button, .pressed=false});
        if (!m_buttons.empty()) m_pointer->events.frame.emit();
    }
    if (m_touch) {
        for (auto id : m_touches) m_touch->events.up.emit(ITouch::SUpEvent{.timeMs=time, .touchID=id});
        if (!m_touches.empty()) m_touch->events.frame.emit();
    }
    m_keys.clear(); m_buttons.clear(); m_touches.clear(); m_payload.clear(); m_payloadType = 0;
}
void CAnlandBackend::onInputReadable() {
    auto device = anland_de_backend_device(m_public);
    // Android history arrives as consecutive relative samples. Avoid repeating
    // the WM's focus/hit-test/frame path for every sample, without adding an idle
    // or timer delay. Only merge a short same-direction run from this drain.
    // Buttons/modifiers/axes are ordering barriers; locked pointers and drag
    // paths keep their individual samples. Nothing persists across dispatches.
    Vector2D motion;
    unsigned samples = 0;
    uint32_t motionTime = 0;
    const auto flushMotion = [&] {
        if (!samples) return;
        if (!m_lost) {
            m_pointer->events.move.emit(IPointer::SMoveEvent{.timeMs=motionTime, .delta=motion, .unaccel=motion});
            m_pointer->events.frame.emit();
        }
        motion = {}; samples = 0;
    };
    for (unsigned budget = 0; budget < 64 && !m_lost; ++budget) {
        if (m_payloadType) {
            if (std::chrono::steady_clock::now() > m_payloadDeadline) { m_lost = true; break; }
            int result = 0;
            if (m_payloadType == ANLAND_DEVICE_IN_RESOURCE) {
                int fds[16], count = 0;
                result = anland_device_read_fds(device, fds, 16, &count, 0);
                if (result == 1) {
                    if (uint32_t(count) == m_resourceCount && m_resourceType == ANLAND_DEVICE_SERVICE_CAMERA && count >= 2)
                        anland_camera_set_resources(fds[0], fds+1, count-1);
                    else { for (int i=0;i<count;++i) close(fds[i]); if (uint32_t(count) != m_resourceCount) m_lost = true; }
                }
            } else {
                result = anland_device_read_input(device, m_payload.data(), m_payload.size(), 0);
                if (result == 1) {
                    if (m_payloadType == ANLAND_DEVICE_IN_CLIPBOARD && onClipboardFromConsumer)
                        onClipboardFromConsumer(m_payload.data(), m_payload.size(), m_clipboardFromConsumerUserdata);
                    else if (m_payloadType == ANLAND_DEVICE_IN_TEXT_INPUT && onTextFromConsumer)
                        onTextFromConsumer(m_payload.data(), m_payload.size(), m_textFromConsumerUserdata);
                }
            }
            if (result < 0) m_lost = true;
            if (result <= 0) break;
            m_payloadType = 0; m_payload.clear();
        }
        anland_device_input_t event = {};
        const int result = anland_device_poll_input(device, &event, 0);
        if (result < 0) m_lost = true;
        if (result <= 0) break;
        const uint32_t time = inputTime();
        if (event.type != ANLAND_DEVICE_IN_PTR_MOTION) flushMotion();
        if (anland_tablet_is_event(event.type)) {
            anland_tablet_sample previous{}, sample{};
            const int tablet = anland_tablet_feed(&m_tablet->state, &event, &previous, &sample);
            if (tablet < 0) { m_lost = true; break; }
            if (tablet > 0) tabletFrame(previous, sample);
            continue;
        }
        switch (event.type) {
            case ANLAND_DEVICE_IN_PTR_MOTION: {
                const Vector2D delta{event.pointer_motion.dx, event.pointer_motion.dy};
                if (!std::isfinite(delta.x) || !std::isfinite(delta.y) || delta == Vector2D{}) break;
                const bool raw = m_capture || !m_buttons.empty();
                const bool reversal = (motion.x * delta.x < 0) || (motion.y * delta.y < 0);
                if (raw || reversal || (samples && time - motionTime >= 2)) flushMotion();
                if (!samples) motionTime = time;
                motion += delta; ++samples;
                if (raw || samples >= 8) flushMotion();
                break;
            }
            case ANLAND_DEVICE_IN_PTR_BUTTON: {
                const auto button = event.pointer_button.button;
                if (button < 0x110 || button > 0x11f) break;
                const bool pressed = event.pointer_button.pressed != 0;
                if (pressed) m_buttons.insert(button); else m_buttons.erase(button);
                m_pointer->events.button.emit(IPointer::SButtonEvent{.timeMs=time, .button=button, .pressed=pressed}); m_pointer->events.frame.emit(); break;
            }
            case ANLAND_DEVICE_IN_PTR_AXIS: {
                const double value = event.pointer_axis.value;
                if (!std::isfinite(value) || event.pointer_axis.axis > 1) break;
                m_pointer->events.axis.emit(IPointer::SAxisEvent{.timeMs=time,
                    .axis=event.pointer_axis.axis ? IPointer::AQ_POINTER_AXIS_HORIZONTAL : IPointer::AQ_POINTER_AXIS_VERTICAL,
                    // The wire has no source/finger metadata. Zero discrete
                    // means a continuous value, not a wheel event with v120=0:
                    // Hyprland's wheel emulation would quantize/drop this delta.
                    // Do not invent FINGER or swipe/pinch gestures from it.
                    .source=event.pointer_axis.discrete ? IPointer::AQ_POINTER_AXIS_SOURCE_WHEEL : IPointer::AQ_POINTER_AXIS_SOURCE_CONTINUOUS,
                    .delta=value, .discrete=double(event.pointer_axis.discrete) * 120.0});
                m_pointer->events.frame.emit(); break;
            }
            case ANLAND_DEVICE_IN_KEY: {
                if (event.key.keycode < 0 || event.key.keycode > 767 || (event.key.action != ANLAND_DEVICE_ACTION_DOWN && event.key.action != ANLAND_DEVICE_ACTION_UP)) break;
                const auto key = uint32_t(event.key.keycode); const bool pressed = event.key.action == ANLAND_DEVICE_ACTION_DOWN;
                if (!queueTextKey || !queueTextKey(key, pressed, time)) deliverTextQueuedKey(key, pressed, time);
                break;
            }
            case ANLAND_DEVICE_IN_TOUCH: {
                const auto id = event.touch.pointer_id;
                if (id < 0 || id > 31 || !m_width || !m_height || !std::isfinite(event.touch.x) || !std::isfinite(event.touch.y)) break;
                const Vector2D pos{std::clamp(double(event.touch.x)/m_width,0.0,1.0), std::clamp(double(event.touch.y)/m_height,0.0,1.0)};
                if (event.touch.action == ANLAND_DEVICE_ACTION_DOWN && m_touches.insert(id).second)
                    m_touch->events.down.emit(ITouch::SDownEvent{.timeMs=time,.touchID=id,.pos=pos});
                else if (event.touch.action == ANLAND_DEVICE_ACTION_UP && m_touches.erase(id))
                    m_touch->events.up.emit(ITouch::SUpEvent{.timeMs=time,.touchID=id});
                else if (event.touch.action == ANLAND_DEVICE_ACTION_MOVE && m_touches.contains(id))
                    m_touch->events.move.emit(ITouch::SMotionEvent{.timeMs=time,.touchID=id,.pos=pos});
                break;
            }
            case ANLAND_DEVICE_IN_TOUCH_FRAME: m_touch->events.frame.emit(); break;
            case ANLAND_DEVICE_IN_DISPLAY_REFRESH:
                if (event.display.refresh_mhz >= 1000 && event.display.refresh_mhz <= 1000000) {
                    m_refresh=event.display.refresh_mhz; if (m_output) m_output->updateMode();
                } break;
            case ANLAND_DEVICE_IN_CLIPBOARD:
            case ANLAND_DEVICE_IN_TEXT_INPUT: {
                const uint32_t size = event.type == ANLAND_DEVICE_IN_CLIPBOARD ? event.clipboard.size : event.text_input.size;
                if (size > ANLAND_DEVICE_MAX_PAYLOAD_SIZE) { m_lost=true; break; }
                m_payloadType=event.type; m_payload.assign(size,'\0'); m_payloadDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(2); break;
            }
            case ANLAND_DEVICE_IN_RESOURCE:
                if (!event.resource.fdnum || event.resource.fdnum > 16) { m_lost=true; break; }
                m_payloadType=event.type; m_resourceType=event.resource.type; m_resourceCount=event.resource.fdnum;
                m_payloadDeadline=std::chrono::steady_clock::now()+std::chrono::seconds(2); break;
            case ANLAND_DEVICE_IN_RESOURCE_INVALID: anland_camera_clear(); break;
            default: break;
        }
    }
    flushMotion();
}
