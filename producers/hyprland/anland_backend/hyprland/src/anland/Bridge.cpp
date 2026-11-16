#include "Bridge.hpp"
#include "BridgeInternal.hpp"
#include "OutputColor.hpp"
#include "../Compositor.hpp"
#include "../config/shared/monitor/MonitorRule.hpp"
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
        case DRM_FORMAT_ABGR2101010:
        case DRM_FORMAT_XRGB8888: return formats.front().drmFormat;
        default: return DRM_FORMAT_INVALID;
    }
}

std::optional<bool> Anland::outputHdrMode(SP<Aquamarine::IOutput> output) {
    if (!output) return std::nullopt;
    const auto implementation = output->getBackend();
    if (!implementation || implementation->type() != Aquamarine::AQ_BACKEND_ANLAND) return std::nullopt;
    const auto owner = Hyprutils::Memory::dynamicPointerCast<Aquamarine::CAnlandBackend>(implementation);
    return owner ? std::optional<bool>{owner->hdrEnabled()} : std::nullopt;
}

void Anland::configureColorRule(SP<Aquamarine::IOutput> output, Config::CMonitorRule& rule) {
    const auto hdr = outputHdrMode(output);
    if (!hdr.has_value()) return;
    const auto owner = Hyprutils::Memory::dynamicPointerCast<Aquamarine::CAnlandBackend>(output->getBackend());
    if (!owner) return;
    // Fixed consumer buffers determine both precision and encoding. Monitor
    // rules/ICC/auto-HDR must not silently put sRGB pixels into a PQ surface.
    fixedOutputColorRule(rule, *hdr, NCMType::CM_HDR, NCMType::CM_SRGB, NTransferFunction::TF_SRGB,
                         owner->hdrSdrWhiteNits(), Aquamarine::CAnlandBackend::hdrPeakNits());
}

float Anland::hdrContentGain(SP<Aquamarine::IOutput> output) {
    if (!output) return 1.0f;
    const auto implementation = output->getBackend();
    if (!implementation || implementation->type() != Aquamarine::AQ_BACKEND_ANLAND) return 1.0f;
    const auto owner = Hyprutils::Memory::dynamicPointerCast<Aquamarine::CAnlandBackend>(implementation);
    return owner ? owner->hdrContentGain() : 1.0f;
}
