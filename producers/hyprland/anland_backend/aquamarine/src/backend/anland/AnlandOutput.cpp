#include "AnlandOutput.hpp"
#include "anland_de_backend.h"
#include "anland_present.h"
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES2/gl2.h>
#include <poll.h>
#include <unistd.h>
#include <cerrno>

using namespace Aquamarine;
using namespace Hyprutils::Memory;
using namespace Hyprutils::Math;

CAnlandOutput::CAnlandOutput(CWeakPointer<CAnlandBackend> backend) : m_backend(backend) {
    name = "ANLAND-1"; description = "Anland full desktop"; make = "Anland"; model = "Consumer";
    auto owner = m_backend.lock();
    if (owner && owner->m_public)
        anland_de_backend_add_window(owner->m_public, 1, "Hyprland desktop", &m_layer);
}
CAnlandOutput::~CAnlandOutput() {
    auto owner = m_backend.lock();
    if (owner && owner->m_backend && m_idle) owner->m_backend->removeIdleEvent(m_idle);
    for (auto& r : m_releases) { if (r.fd >= 0) close(r.fd); r.buffer->backendUnpin(); }
    for (auto& [id, buffer] : m_pinned) buffer->backendUnpin();
}
bool CAnlandOutput::destroy() { return false; }
CSharedPointer<IBackendImplementation> CAnlandOutput::getBackend() { return m_backend.lock(); }
std::vector<SDRMFormat> CAnlandOutput::getRenderFormats() { auto owner = m_backend.lock(); return owner ? owner->getRenderFormats() : std::vector<SDRMFormat>{}; }

bool CAnlandOutput::resetForSession() {
    auto owner = m_backend.lock();
    if (!owner || m_releaseFault || !owner->m_allocator || owner->m_allocator->buffers.empty() || !owner->m_width || !owner->m_height)
        return false;

    // A generation change replaces images even when output geometry is identical.
    // Configure before notifying the WM: its next render must never see length=0.
    auto replacement = CSwapchain::create(owner->m_allocator, owner);
    SSwapchainOptions options = {};
    options.size = {double(owner->m_width), double(owner->m_height)};
    options.format = owner->m_allocator->buffers.front()->dmabuf().format;
    options.length = owner->m_allocator->buffers.size();
    options.scanout = true;
    if (!replacement->reconfigure(options))
        return false;

    swapchain = replacement;
    m_renderTarget = {};
    m_rendering = false;
    needsFrame = false;
    m_repaint = true;
    // A failed old-frame commit may leave a buffer/fence in native pending state.
    // Do not discard pinned resources: public drop/release outcomes retire them.
    state->resetExplicitFences();
    state->setBuffer(nullptr);
    updateMode(true);
    owner->resendScheduling();
    return true;
}

void CAnlandOutput::updateMode(bool forceNotification) {
    auto owner = m_backend.lock();
    if (!owner || !owner->m_width || !owner->m_height) return;
    anland_present_note_refresh(anland_de_backend_present_object(owner->m_public),
                                owner->m_refresh);
    const Vector2D size{double(owner->m_width), double(owner->m_height)};
    if (!forceNotification && !modes.empty() && modes[0]->pixelSize == size && modes[0]->refreshRate == owner->m_refresh) return;
    modes = {makeShared<SOutputMode>()};
    modes[0]->pixelSize = size; modes[0]->refreshRate = owner->m_refresh; modes[0]->preferred = true;
    anland_layer_desc_t desc = {};
    desc.name = "Hyprland desktop"; desc.geometry = {0, 0, owner->m_width, owner->m_height}; desc.scale = 1; desc.opacity = 1; desc.visible = true;
    anland_de_backend_update_window(owner->m_public, 1, &desc);
    events.state.emit(SStateEvent{.size = size});
}

void CAnlandOutput::scheduleFrame(scheduleFrameReason reason) {
    auto owner = m_backend.lock();
    if (!owner || !owner->m_ready) return;
    // No unconditional render tail loop. Damage and animation retain repaint intent.
    m_repaint = true;
    if (!enabled || owner->m_lost || m_releaseFault || m_idle || m_rendering || m_commitID || !m_layer) return;
    anland_de_target_t target = {};
    if (anland_de_backend_get_writable_target(owner->m_public, &target) != 0 || !targetAvailable(target)) return;
    m_idle = makeShared<std::function<void()>>([weak = m_backend] {
        auto backend = weak.lock();
        if (!backend || !backend->m_output) return;
        auto output = backend->m_output;
        output->m_idle.reset();
        anland_de_target_t target = {};
        if (!output->enabled || backend->m_lost || output->m_releaseFault || !output->m_repaint || output->m_commitID ||
            anland_de_backend_get_writable_target(backend->m_public, &target) != 0 || !output->targetAvailable(target)) return;
        output->m_renderTarget = target; output->m_rendering = true; output->m_repaint = false; output->needsFrame = true;
        output->events.frame.emit();
        // Rendering may be skipped for an empty damage region. Do not reserve a slot forever.
        const bool skipped = output->m_rendering;
        if (skipped) { output->m_rendering = false; output->needsFrame = false; }
        // New damage during a skipped frame must not wait for the 200ms retry.
        // No new request means no unconditional idle render loop.
        if (skipped && output->m_repaint) output->scheduleFrame(AQ_SCHEDULE_NEEDS_FRAME);
    });
    owner->m_backend->addIdleEvent(m_idle);
}

static int exportFence() {
    auto display = eglGetCurrentDisplay();
    auto create = reinterpret_cast<PFNEGLCREATESYNCKHRPROC>(eglGetProcAddress("eglCreateSyncKHR"));
    auto destroy = reinterpret_cast<PFNEGLDESTROYSYNCKHRPROC>(eglGetProcAddress("eglDestroySyncKHR"));
    auto duplicate = reinterpret_cast<PFNEGLDUPNATIVEFENCEFDANDROIDPROC>(eglGetProcAddress("eglDupNativeFenceFDANDROID"));
    if (display != EGL_NO_DISPLAY && create && destroy && duplicate) {
        const auto sync = create(display, EGL_SYNC_NATIVE_FENCE_ANDROID, nullptr);
        if (sync != EGL_NO_SYNC_KHR) {
            glFlush(); const int fd = duplicate(display, sync); destroy(display, sync);
            if (fd >= 0) return fd;
        }
    }
    glFinish(); // Explicit synchronous fallback; -1 is now safe.
    return -1;
}

bool CAnlandOutput::test() {
    auto owner = m_backend.lock();
    const auto& pending = state->state();
    if (!owner) return false;
    if (pending.buffer && !dynamicPointerCast<CAnlandBuffer>(pending.buffer)) return false; // no client direct scanout
    const auto fmt = pending.drmFormat;
    return fmt == DRM_FORMAT_INVALID || (!owner->m_allocator->buffers.empty() && fmt == owner->m_allocator->buffers[0]->dmabuf().format);
}
bool CAnlandOutput::commit() {
    auto owner = m_backend.lock();
    if (!owner) return false;
    const auto snapshot = state->snapshot();
    if (snapshot.error()) return false;
    const auto& pending = snapshot.state();
    // Local enable/disable/modeset does not submit a desktop frame. It must work
    // even while disconnected or waiting for an older frame's ACK/release.
    if (!pending.enabled || !(pending.committed & COutputState::AQ_OUTPUT_STATE_BUFFER) || !pending.buffer) {
        if (pending.enabled && !test()) return false;
        applyEnabled(pending.enabled);
        events.commit.emit();
        state->consume(snapshot);
        if (enabled && m_repaint) scheduleFrame(AQ_SCHEDULE_NEEDS_FRAME);
        return true;
    }
    if (!owner->m_public || owner->m_lost || m_releaseFault || !test()) return false;
    auto buffer = dynamicPointerCast<CAnlandBuffer>(pending.buffer);
    anland_de_target_t current = {};
    if (!buffer || !m_rendering || m_commitID || buffer->m_generation != m_renderTarget.generation || buffer->m_slot != m_renderTarget.index ||
        anland_de_backend_get_writable_target(owner->m_public, &current) != 0 || current.generation != m_renderTarget.generation || current.index != m_renderTarget.index) {
        m_rendering = false; m_repaint = true; return false;
    }
    int fence = pending.explicitInFence;
    const bool ownFence = fence < 0;
    if (ownFence) fence = exportFence();
    anland_layer_state_t layer = {};
    layer.layer_id = m_layer; layer.buffer_id = uint64_t(buffer->m_slot) + 1;
    layer.destination = {0, 0, current.width, current.height}; layer.visible = true; layer.opacity = 1; layer.acquire_fence_fd = fence;
    uint64_t id = 0;
    const int accepted = anland_de_backend_commit(owner->m_public, &layer, 1, &id);
    if (ownFence && fence >= 0) close(fence);
    if (accepted != 0) { m_rendering = false; m_repaint = true; return false; }
    m_commitID = id; buffer->backendPin(); m_pinned[layer.buffer_id] = buffer;
    m_rendering = false; needsFrame = false;
    applyEnabled(pending.enabled);
    events.commit.emit(); state->consume(snapshot);
    // A failed handoff still has a commit ID: dispatch its dropped/released outcome.
    if (anland_de_backend_present(owner->m_public) != 0) owner->m_lost = true;
    return true;
}

void CAnlandOutput::applyEnabled(bool value) {
    const bool changed = enabled != value;
    enabled = value;
    auto owner = m_backend.lock();
    if (!enabled) {
        if (m_idle && owner && owner->m_backend) owner->m_backend->removeIdleEvent(m_idle);
        m_idle.reset();
        m_rendering = false;
        needsFrame = false;
        m_repaint = true;
    } else if (changed) {
        m_repaint = true;
    }
    if (owner) owner->syncMouseCapture();
}

bool CAnlandOutput::targetAvailable(const anland_de_target_t& target) const {
    const auto owner = m_backend.lock();
    if (!owner || !owner->m_allocator || m_releaseFault || target.generation != owner->m_generation ||
        target.index >= owner->m_allocator->buffers.size()) return false;
    const auto& buffer = owner->m_allocator->buffers[target.index];
    return buffer->m_generation == target.generation && buffer->backendPinCount() == 0;
}

std::vector<int> CAnlandOutput::releaseFDs() const {
    std::vector<int> fds;
    for (const auto& release : m_releases)
        if (!release.failed && release.fd >= 0) fds.push_back(release.fd);
    return fds;
}

void CAnlandOutput::retryReleases() {
    std::erase_if(m_releases, [this](SRelease& release) {
        if (release.failed) return false;
        pollfd pfd = {.fd = release.fd, .events = POLLIN};
        const int result = poll(&pfd, 1, 0);
        if (result < 0 && errno == EINTR) return false;
        const bool error = result < 0 || (pfd.revents & (POLLERR | POLLHUP | POLLNVAL));
        if (!error && (pfd.revents & POLLIN)) {
            close(release.fd);
            release.buffer->backendUnpin();
            return true;
        }
        if (!error && std::chrono::steady_clock::now() < release.deadline) return false;
        // No unpin on error/timeout: this image is NOT known to be reusable.
        if (release.fd >= 0 && !(pfd.revents & POLLNVAL)) close(release.fd);
        release.fd = -1;
        release.failed = true;
        m_releaseFault = true;
        m_rendering = false;
        needsFrame = false;
        m_repaint = true;
        if (auto owner = m_backend.lock()) {
            owner->m_lost = true;
            if (owner->m_backend) owner->m_backend->log(AQ_LOG_ERROR,
                "anland: release fence failed/timed out; output stopped, buffer quarantined until teardown");
        }
        return false;
    });
}
void CAnlandOutput::onEvent(const anland_scene_event_t& event) {
    // A writable target is a consumer frame request, not merely an opportunity
    // to paint damage. The existing consumer waits for every selected slot and
    // times out after 5s; leaving an idle request unanswered reconnects the
    // session and can queue stale slot contents. Service schedules only after
    // dispatching the batch's releases, with the usual ACK/pin/fence gates.
    if (event.type == ANLAND_SCENE_EVENT_RENDER_TARGET_READY) {
        const auto owner = m_backend.lock();
        if (owner && owner->m_allocator && event.u.target_ready.generation == owner->m_generation &&
            event.u.target_ready.count == owner->m_allocator->buffers.size() &&
            event.u.target_ready.index < event.u.target_ready.count)
            m_repaint = true;
        return;
    }
    if ((event.type == ANLAND_SCENE_EVENT_PRESENTED || event.type == ANLAND_SCENE_EVENT_COMMIT_DROPPED) && event.commit_id == m_commitID && m_commitID) {
        const auto id = m_commitID; m_commitID = 0;
        const bool success = event.type == ANLAND_SCENE_EVENT_PRESENTED;
        if (!success) m_repaint = true;
        const uint64_t ns = success ? event.u.presented.presentation_ns : 0;
        timespec when{.tv_sec = time_t(ns / 1000000000), .tv_nsec = long(ns % 1000000000)};
        events.present.emit(SPresentEvent{.presented = success, .when = success && event.u.presented.presentation_ns ? &when : nullptr,
            .seq = ++m_sequence, .refresh = 0, .flags = 0, .commitID = id}); // consumer ACK, never physical VSync
    } else if (event.type == ANLAND_SCENE_EVENT_BUFFER_RELEASED) {
        auto it = m_pinned.find(event.u.released.buffer_id);
        if (it == m_pinned.end()) { if (event.u.released.release_fence_fd >= 0) close(event.u.released.release_fence_fd); return; }
        if (event.u.released.release_fence_fd >= 0) m_releases.push_back({event.u.released.release_fence_fd, it->second});
        else it->second->backendUnpin();
        m_pinned.erase(it);
    }
}
