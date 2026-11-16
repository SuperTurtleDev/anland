#include "AnlandAllocator.hpp"
#include "AnlandOutput.hpp"
#include <unistd.h>
using namespace Aquamarine;
using namespace Hyprutils::Memory;
using namespace Hyprutils::Math;

bool CAnlandAllocator::import(const anland_de_target_t& target) {
    auto owner = m_owner.lock();
    if (!owner || !target.count || target.count > ANLAND_DEVICE_MAX_BUFS || target.index >= target.count)
        return false;
    std::vector<CSharedPointer<CAnlandBuffer>> imported;
    for (uint32_t slot = 0; slot < target.count; ++slot) {
        anland_device_fb_t fb = {.fd = -1};
        if (anland_device_get_drm_fb(anland_de_backend_device(owner->m_public), slot, &fb) != 0)
            return false;
        const bool valid = fb.fd >= 0 && fb.width == target.width && fb.height == target.height && fb.width <= 16384 && fb.height <= 16384 &&
            (fb.format == DRM_FORMAT_ABGR8888 || fb.format == DRM_FORMAT_XBGR8888 || fb.format == DRM_FORMAT_ARGB8888 || fb.format == DRM_FORMAT_XRGB8888) &&
            uint64_t(fb.stride) >= uint64_t(fb.width) * 4 && uint64_t(fb.stride) * fb.height + fb.offset <= UINT32_MAX;
        if (!valid) {
            if (fb.fd >= 0) close(fb.fd);
            return false;
        }
        imported.push_back(makeShared<CAnlandBuffer>(fb, target.generation, slot));
    }
    buffers = std::move(imported);
    generation = target.generation;
    m_cursor = 0;
    return true;
}

CSharedPointer<CBackend> CAnlandAllocator::getBackend() { auto owner = m_owner.lock(); return owner ? owner->m_backend.lock() : nullptr; }
CSharedPointer<IBuffer> CAnlandAllocator::acquire(const SAllocatorBufferParams& params, CSharedPointer<CSwapchain>) {
    if (buffers.empty() || params.size != buffers[0]->size || (params.format != DRM_FORMAT_INVALID && params.format != buffers[0]->dmabuf().format))
        return nullptr;
    return buffers[m_cursor++ % buffers.size()];
}
CSharedPointer<IBuffer> CAnlandAllocator::nextBuffer(int* age) {
    if (age) *age = 0;
    auto owner = m_owner.lock();
    if (!owner || owner->m_lost || !owner->m_output || !owner->m_output->m_rendering)
        return nullptr;
    const auto& target = owner->m_output->m_renderTarget;
    if (target.generation != generation || target.index >= buffers.size())
        return nullptr;
    const auto buffer = buffers[target.index];
    if (buffer->backendPinCount())
        return nullptr;
    return buffer;
}

