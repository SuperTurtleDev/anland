#include "Bridge.hpp"
#include "BridgeInternal.hpp"
#include "../Compositor.hpp"
#include <drm_fourcc.h>
#include <aquamarine/output/Output.hpp>

static WP<Aquamarine::CAnlandBackend> schedulingBackend;

void Anland::bind(SP<Aquamarine::CBackend> aq) {
    for (const auto& impl : aq->getImplementations()) {
        if (impl->type() != Aquamarine::AQ_BACKEND_ANLAND) continue;
        auto owner = Hyprutils::Memory::dynamicPointerCast<Aquamarine::CAnlandBackend>(impl);
        setClipboardBackend(owner);
        setTextBackend(owner);
        owner->queueTextKey = queueKeyFromConsumer;
        owner->onInputReset = cancelTextInput;
        setPointerCaptureBackend(owner);
        owner->queryMouseCapture = pointerCaptureRequested;
        schedulingBackend = owner;
        owner->onClipboardFromConsumer = clipboardFromConsumer;
        owner->onTextFromConsumer = textFromConsumer;
    }
}

void Anland::reportActiveClient(pid_t pid) {
    if (auto owner = schedulingBackend.lock())
        owner->updateActiveScheduling(pid);
}

void Anland::renderImportFailed(SP<Aquamarine::IOutput> output, SP<Aquamarine::IBuffer> buffer) {
    if (!output) return;
    const auto implementation = output->getBackend();
    if (!implementation || implementation->type() != Aquamarine::AQ_BACKEND_ANLAND) return;
    if (auto owner = Hyprutils::Memory::dynamicPointerCast<Aquamarine::CAnlandBackend>(implementation))
        owner->notifyRenderImportFailure(buffer);
}

size_t Anland::swapchainLength(SP<Aquamarine::IOutput> output, size_t nativeLength) {
    if (!output) return nativeLength;
    const auto implementation = output->getBackend();
    if (!implementation || implementation->type() != Aquamarine::AQ_BACKEND_ANLAND) return nativeLength;
    const auto owner = Hyprutils::Memory::dynamicPointerCast<Aquamarine::CAnlandBackend>(implementation);
    return owner ? owner->renderBufferCount() : 0;
}

uint32_t Anland::renderTargetFormat(SP<Aquamarine::IOutput> output) {
    if (!output)
        return DRM_FORMAT_INVALID;
    const auto implementation = output->getBackend();
    if (!implementation || implementation->type() != Aquamarine::AQ_BACKEND_ANLAND)
        return DRM_FORMAT_INVALID;
    const auto owner = Hyprutils::Memory::dynamicPointerCast<Aquamarine::CAnlandBackend>(implementation);
    if (!owner || !owner->renderBufferCount())
        return DRM_FORMAT_INVALID;
    const auto formats = output->getRenderFormats();
    if (formats.size() != 1)
        return DRM_FORMAT_INVALID;
    switch (formats.front().drmFormat) {
        case DRM_FORMAT_ABGR8888:
        case DRM_FORMAT_XBGR8888:
        case DRM_FORMAT_ARGB8888:
        case DRM_FORMAT_XRGB8888: return formats.front().drmFormat;
        default: return DRM_FORMAT_INVALID;
    }
}
