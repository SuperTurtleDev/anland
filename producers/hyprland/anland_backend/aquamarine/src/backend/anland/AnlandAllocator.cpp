#include "AnlandAllocator.hpp"
#include "AnlandOutput.hpp"
#include "anland_hdr.h"
#include <cstdlib>
#include <unistd.h>
using namespace Aquamarine;
using namespace Hyprutils::Memory;
using namespace Hyprutils::Math;

bool CAnlandAllocator::import(const anland_de_target_t& target) {
    auto owner = m_owner.lock();
    if (!owner || !target.count || target.count > ANLAND_DEVICE_MAX_BUFS || target.index >= target.count)
        return false;
    std::vector<CSharedPointer<CAnlandBuffer>> imported;
    const bool allowHDR = !anland_hdr_environment_disabled(std::getenv("ANLAND_HDR"));
    anland_device_output_t output = {};
    auto device = anland_de_backend_device(owner->m_public);
    if (anland_de_backend_get_output(owner->m_public, &output) != 0)
        return false;
    const auto mode = anland_hdr_mode_for_hal(output.format, allowHDR);
    if (mode == ANLAND_HDR_REJECTED) {
        owner->m_backend->log(AQ_LOG_ERROR, "anland: consumer format rejected (HAL " + std::to_string(output.format) + "); HDR may be explicitly disabled by ANLAND_HDR");
        return false;
    }
    for (uint32_t slot = 0; slot < target.count; ++slot) {
        // Validate the Android-side contract before translating its format to a
        // native fourcc. A generic 10-bit image is not inherently PQ/HDR.
        anland_device_fb_t raw = {.fd = -1};
        if (anland_device_get_fb(device, slot, &raw) != 0)
            return false;
        if (raw.fd >= 0) close(raw.fd);
        if (raw.format != output.format)
            return false;
        anland_device_fb_t fb = {.fd = -1};
        if (anland_device_get_drm_fb(device, slot, &fb) != 0)
            return false;
        const bool validFormat = mode == ANLAND_HDR_HDR10 ? fb.format == DRM_FORMAT_ABGR2101010 :
            (fb.format == DRM_FORMAT_ABGR8888 || fb.format == DRM_FORMAT_XBGR8888 || fb.format == DRM_FORMAT_ARGB8888);
        const bool valid = fb.fd >= 0 && fb.width == target.width && fb.height == target.height && fb.width <= 16384 && fb.height <= 16384 && validFormat &&
            uint64_t(fb.stride) >= uint64_t(fb.width) * 4 && uint64_t(fb.stride) * fb.height + fb.offset <= UINT32_MAX;
        if (!valid) {
            if (fb.fd >= 0) close(fb.fd);
            return false;
        }
        imported.push_back(makeShared<CAnlandBuffer>(fb, target.generation, slot));
    }
    buffers = std::move(imported);
    generation = target.generation;
    hdr = mode == ANLAND_HDR_HDR10;
    m_cursor = 0;
    owner->m_backend->log(AQ_LOG_DEBUG, hdr ? "anland: consumer HDR10, BT.2020/PQ, 10-bit output" : "anland: consumer SDR, sRGB output");
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
