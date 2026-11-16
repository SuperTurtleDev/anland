#include <aquamarine/backend/AnlandBackend.hpp>
#include "AnlandOutput.hpp"
#include "anland_audio.h"
#include "anland_camera.h"
#include <cstdlib>
#include <unistd.h>
using namespace Aquamarine;
using namespace Hyprutils::Memory;

void CAnlandBackend::syncWatchers() {
    std::vector<std::pair<int,uint32_t>> wanted;
    if (m_timerFD >= 0) wanted.emplace_back(m_timerFD, 1);
    if (m_public) {
        auto device = anland_de_backend_device(m_public);
        const int data = anland_device_data_fd(device), ready = anland_device_buffer_ready_fd(device), scene = anland_scene_event_fd(anland_de_backend_scene(m_public));
        if (data >= 0) wanted.emplace_back(data, 1 | (anland_device_pending_output(device) ? 2 : 0));
        if (ready >= 0) wanted.emplace_back(ready, 1);
        if (scene >= 0) wanted.emplace_back(scene, 1);
    }
    if (m_output) for (const int fd : m_output->releaseFDs()) wanted.emplace_back(fd, 1);
    bool changed = wanted.size() != m_watchers.size();
    for (size_t i = 0; !changed && i < wanted.size(); ++i) changed = wanted[i].first != m_watchers[i]->fd || wanted[i].second != m_watchers[i]->events;
    if (!changed) return;
    m_watchers.clear();
    for (auto [fd, mask] : wanted) {
        auto watch = makeShared<SPollFD>(); watch->fd = fd; watch->events = mask;
        watch->onSignal = [weak = self] { if (auto owner = weak.lock()) owner->service(); };
        m_watchers.push_back(watch);
    }
    if (m_ready && m_backend) m_backend->events.pollFDsChanged.emit();
}
void CAnlandBackend::loseSession() {
    resetInput(); m_generation = 0; m_captureKnown = false;
    if (m_output) { m_output->m_rendering = false; m_output->m_repaint = true; }
    if (m_public) anland_de_backend_drop_session(m_public);
    m_lost = false;
}
void CAnlandBackend::service() {
    // AQ start/onReady precedes the WM's input/seat/protocol initialization.
    // Leave the retry timer armed until the core idle publishes our devices.
    if (!m_ready || m_servicing) return;
    m_servicing = true;
    uint64_t ticks;
    const bool retry = m_timerFD >= 0 && read(m_timerFD, &ticks, sizeof(ticks)) == sizeof(ticks);
    if (m_lost) loseSession();
    const bool releaseFault = m_output && m_output->hasReleaseFault();
    if (!m_public && retry && !releaseFault) {
        anland_de_backend_config_t config = {};
        config.name = "Hyprland"; config.present.endpoint = std::getenv("ANLAND_SOCKET"); config.present.defer_connector = true;
        m_public = anland_de_backend_create(&config);
        if (m_public) {
            m_backend->log(AQ_LOG_DEBUG, "anland: daemon control connection established; waiting for consumer targets");
            auto device = anland_de_backend_device(m_public);
            anland_device_set_pre_release_cb(device, [](void*) { anland_audio_set_fd(-1); anland_camera_clear(); }, this);
            anland_device_set_fallback_cb(device, [](void* data) { static_cast<CAnlandBackend*>(data)->m_lost = true; }, this);
        }
    }
    if (m_public) {
        auto device = anland_de_backend_device(m_public);
        if (retry && !releaseFault && !anland_device_is_connected(device)) {
            if (!anland_device_is_daemon_alive(device)) anland_de_backend_reopen(m_public, std::getenv("ANLAND_SOCKET"));
            if (anland_de_backend_reconnect(m_public) == 0) {
                m_backend->log(AQ_LOG_DEBUG, "anland: consumer session connected; pumping target publication");
                anland_audio_set_fd(anland_device_audio_fd(device));
                anland_device_request_resources(device, ANLAND_DEVICE_SERVICE_CAMERA, nullptr);
                m_captureKnown = false; // replay effective state to the new consumer
            }
        }
        if (anland_device_is_connected(device)) {
            anland_de_backend_pump(m_public, 0);
            onInputReadable();
            if (anland_device_pending_output(device) && anland_device_flush_output(device) < 0) m_lost = true;
        }
        dispatchScene();
        if (m_lost) { loseSession(); dispatchScene(); }
    }
    if (m_output) { m_output->retryReleases(); if (m_output->m_repaint) m_output->scheduleFrame(IOutput::AQ_SCHEDULE_DAMAGE); }
    if (queryMouseCapture) updateMouseCapture(queryMouseCapture());
    syncMouseCapture();
    if (m_lost) { loseSession(); dispatchScene(); }
    syncWatchers(); m_servicing = false;
}
void CAnlandBackend::dispatchScene() {
    if (!m_public) return;
    anland_scene_event_t events[32]; size_t count = 0;
    for (unsigned budget = 0; budget < 4; ++budget) {
        if (anland_de_backend_dispatch(m_public, events, 32, &count) != 0 || !count) break;
        for (size_t i = 0; i < count; ++i) {
            const auto& event = events[i];
            if (event.type == ANLAND_SCENE_EVENT_RENDER_TARGET_READY) {
                anland_de_target_t target = {};
                if (anland_de_backend_get_target(m_public, &target) != 0 || target.generation != event.u.target_ready.generation) continue;
                if (m_generation != target.generation) {
                    m_backend->log(AQ_LOG_DEBUG, "anland: received target generation=" + std::to_string(target.generation) + " slots=" + std::to_string(target.count) + " size=" + std::to_string(target.width) + "x" + std::to_string(target.height));
                    if (!m_allocator->import(target)) { m_backend->log(AQ_LOG_ERROR, "anland: target descriptors rejected; retiring session"); m_lost = true; continue; }
                    m_generation = target.generation; m_width = target.width; m_height = target.height;
                    anland_device_output_t output = {};
                    if (anland_de_backend_get_output(m_public, &output) == 0 && output.refresh_mhz) m_refresh = output.refresh_mhz;
                    if (m_output) {
                        if (!m_output->resetForSession()) m_lost = true;
                    } else if (!createOutput()) m_lost = true;
                }
            } else if (event.type == ANLAND_SCENE_EVENT_OUTPUT_CHANGED) {
                m_width = event.u.output.width; m_height = event.u.output.height;
                if (event.u.output.refresh_mhz) m_refresh = event.u.output.refresh_mhz;
                if (m_output) m_output->updateMode();
            }
            if (m_output) m_output->onEvent(event);
            else if (event.type == ANLAND_SCENE_EVENT_BUFFER_RELEASED && event.u.released.release_fence_fd >= 0) close(event.u.released.release_fence_fd);
        }
    }
}