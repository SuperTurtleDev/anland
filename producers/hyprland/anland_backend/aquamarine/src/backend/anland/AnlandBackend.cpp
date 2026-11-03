#include <aquamarine/backend/AnlandBackend.hpp>
#include "AnlandOutput.hpp"
#include "AnlandInput.hpp"
#include "anland_audio.h"
#include "anland_camera.h"
#include <fcntl.h>
#include <sys/timerfd.h>
#include <unistd.h>
#include <cstdlib>
#include <algorithm>

using namespace Aquamarine;
using namespace Hyprutils::Memory;

CAnlandBackend::CAnlandBackend(CSharedPointer<CBackend> backend) : m_backend(backend) { ; }
CAnlandBackend::~CAnlandBackend() {
    m_ready = false;
    if (auto backend = m_backend.lock(); backend && m_readyIdle) backend->removeIdleEvent(m_readyIdle);
    m_readyIdle.reset();
    resetInput();
    anland_audio_stop(); anland_camera_stop();
    if (m_public) {
        anland_de_backend_drop_session(m_public); dispatchScene();
        m_output.reset(); anland_de_backend_destroy(m_public); m_public = nullptr;
    }
    m_watchers.clear();
    if (m_timerFD >= 0) close(m_timerFD);
    if (m_renderFD >= 0) close(m_renderFD);
}
eBackendType CAnlandBackend::type() { return AQ_BACKEND_ANLAND; }
int CAnlandBackend::drmFD() { return m_renderFD; }
int CAnlandBackend::drmRenderNodeFD() { return m_renderFD; }
uint32_t CAnlandBackend::capabilities() { return AQ_BACKEND_CAPABILITY_POINTER; }
CWeakPointer<IBackendImplementation> CAnlandBackend::getPrimary() { return {}; }
CSharedPointer<IAllocator> CAnlandBackend::preferredAllocator() { return m_allocator; }
std::vector<CSharedPointer<IAllocator>> CAnlandBackend::getAllocators() { return {m_allocator}; }
std::vector<SDRMFormat> CAnlandBackend::getCursorFormats() { return {}; } // software composited cursor
std::vector<SDRMFormat> CAnlandBackend::getRenderFormats() {
    if (m_allocator && !m_allocator->buffers.empty()) {
        const auto attrs = m_allocator->buffers[0]->dmabuf();
        return {{.drmFormat = attrs.format, .modifiers = {attrs.modifier}}};
    }
    return {{.drmFormat = DRM_FORMAT_ABGR8888, .modifiers = {DRM_FORMAT_MOD_LINEAR, DRM_FORMAT_MOD_INVALID}}};
}
bool CAnlandBackend::inFallback() const { return !m_public || !anland_device_is_connected(anland_de_backend_device(m_public)); }

bool CAnlandBackend::start() {
    const char* node = std::getenv("ANLAND_DRM_DEVICE");
    m_renderFD = open(node && *node ? node : "/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (m_renderFD < 0) return false; // no empty GPU device substitution
    m_timerFD = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC | TFD_NONBLOCK);
    if (m_timerFD < 0) return false;
    const itimerspec timer = {.it_interval = {.tv_sec = 0, .tv_nsec = 200000000}, .it_value = {.tv_sec = 0, .tv_nsec = 1}};
    if (timerfd_settime(m_timerFD, 0, &timer, nullptr) < 0) return false;
    m_allocator = makeShared<CAnlandAllocator>(self);
    m_backend->primaryAllocator = m_allocator;
    m_pointer = makeShared<CAnlandPointer>(); m_keyboard = makeShared<CAnlandKeyboard>(); m_touch = makeShared<CAnlandTouch>();
    m_tablet = makeShared<CAnlandTablet>();
    // Start engines once; borrowed resource fd detachment happens before transport release.
    const int audioResult = anland_audio_start();
    const int cameraResult = anland_camera_start();
    if (audioResult != 0) m_backend->log(AQ_LOG_WARNING, "anland: audio engine initialization failed");
    if (cameraResult != 0) m_backend->log(AQ_LOG_WARNING, "anland: camera engine initialization failed");
    service(); syncWatchers();
    return true;
}
void CAnlandBackend::onReady() {
    // CBackend::start calls onReady synchronously, before the WM has created
    // its InputManager/SeatManager. Publish only after it starts pumping AQ.
    if (m_ready || m_readyIdle) return;
    auto backend = m_backend.lock();
    if (!backend) return;
    m_readyIdle = makeShared<std::function<void()>>([weak = self] {
        auto owner = weak.lock();
        if (!owner) return;
        owner->m_readyIdle.reset();
        auto backend = owner->m_backend.lock();
        if (!backend || owner->m_ready) return;
        owner->m_ready = true;
        backend->log(AQ_LOG_DEBUG, "anland: WM event loop ready; publishing input devices before session service");
        backend->events.newPointer.emit(owner->m_pointer);
        backend->events.newKeyboard.emit(owner->m_keyboard);
        backend->events.newTouch.emit(owner->m_touch);
        backend->events.newTablet.emit(owner->m_tablet);
        if (owner->m_output) backend->events.newOutput.emit(owner->m_output);
        owner->service();
    });
    backend->addIdleEvent(m_readyIdle);
}
bool CAnlandBackend::createOutput(const std::string& requested) {
    if (m_output) return requested.empty() || requested == m_output->name;
    if (!m_public || !m_generation || m_allocator->buffers.empty()) return false;
    m_output = makeShared<CAnlandOutput>(self);
    if (!requested.empty()) m_output->name = requested;
    if (!m_output->resetForSession()) {
        m_output.reset();
        return false;
    }
    if (m_ready) m_backend->events.newOutput.emit(m_output);
    return true;
}
std::vector<CSharedPointer<SPollFD>> CAnlandBackend::pollFDs() { return m_watchers; }
bool CAnlandBackend::dispatchEvents() { service(); return true; }

bool CAnlandBackend::sendClipboardToConsumer(const std::string& text) {
    if (inFallback()) return false;
    auto device = anland_de_backend_device(m_public);
    if (anland_device_queue_clipboard(device, text.data(), text.size()) != 0) return false;
    const int result = anland_device_flush_output(device);
    if (result < 0) m_lost = true;
    syncWatchers(); return result >= 0;
}
void CAnlandBackend::updateMouseCapture(bool active) {
    m_capture = active;
    syncMouseCapture();
}
bool CAnlandBackend::notifyRenderImportFailure(CSharedPointer<IBuffer> buffer) {
    const auto imported = dynamicPointerCast<CAnlandBuffer>(buffer);
    if (!imported || !m_output || !m_generation || imported->m_generation != m_generation ||
        !m_output->m_rendering || m_output->m_renderTarget.generation != m_generation ||
        imported->m_slot != m_output->m_renderTarget.index)
        return false;

    if (m_backend) m_backend->log(AQ_LOG_ERROR, "anland: render-target GPU import failed; retiring consumer session");
    m_output->m_rendering = false;
    m_output->needsFrame = false;
    m_output->m_repaint = true;
    m_lost = true;
    return true;
}

size_t CAnlandBackend::renderBufferCount() const {
    return m_allocator ? m_allocator->buffers.size() : 0;
}

bool CAnlandBackend::effectiveMouseCapture() const {
    return m_capture && m_output && m_output->enabled && !m_lost && !m_output->hasReleaseFault();
}
void CAnlandBackend::syncMouseCapture() {
    if (inFallback() || m_lost) { m_captureKnown = false; return; }
    const bool active = effectiveMouseCapture();
    if (m_captureKnown && m_captureSent == active) return;
    if (anland_device_set_consumer_var(anland_de_backend_device(m_public), ANLAND_DEVICE_VAR_CAPTURE_MOUSE, active) != 0) {
        m_captureKnown = false;
        m_lost = true;
        return;
    }
    m_captureSent = active;
    m_captureKnown = true;
}
