/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "anland_output.h"
#include "anland_backend.h"
// AnlandOutput::present() calls AnlandEglLayer::takePendingFence(), so the layer
// type must be complete here (the header only forward-declares it).
#include "anland_egl_backend.h"
#include "anland_format.h"
#include "anland_logging.h"
#include "libdisplay_producer/anland_hdr.h"

#include "core/renderbackend.h" // OutputFrame
#include "core/renderloop.h"

#include <chrono>

namespace KWin
{

// The legacy transport has no color negotiation. The Android app's buffer
// request selects a fixed local color mode for each consumer session.
static AnlandColorDescription makeColorDescription(bool hdr, bool wire, unsigned int sdrWhiteNits)
{
    if (!hdr) {
        return ColorDescription::sRGB;
    }
#ifdef ANLAND_KWIN_64
    const auto colorimetry = Colorimetry::BT2020;
#else
    const auto colorimetry = Colorimetry::fromName(NamedColorimetry::BT2020);
#endif
    // PQ always retains its absolute 0..10,000 nit transfer function. 1,000
    // nits is the content/output limit, not the luminance represented by code 1.
    const auto transfer = wire ? TransferFunction(TransferFunction::PerceptualQuantizer, 0, 10000)
                              : TransferFunction(TransferFunction::gamma22, 0, 1000);
#ifdef ANLAND_KWIN_66
    return std::make_shared<ColorDescription>(colorimetry, transfer, sdrWhiteNits, 0, 1000, 1000);
#else
    return ColorDescription(colorimetry, transfer, sdrWhiteNits, 0, 1000, 1000);
#endif
}

AnlandOutput::AnlandOutput(AnlandBackend *parent, const QString &name)
#ifdef ANLAND_KWIN_66
    : BackendOutput()
#else
    : Output(parent)
#endif
    , m_backend(parent)
    , m_hdrAllowed(!anlandHdrExplicitlyDisabled(qgetenv("ANLAND_HDR")))
    , m_wireColorDescription(ColorDescription::sRGB)
    , m_renderColorDescription(ColorDescription::sRGB)
#ifndef ANLAND_KWIN_68
    , m_renderLoop(std::make_unique<RenderLoop>(this))
#endif
{
#ifdef ANLAND_KWIN_66
    setParent(parent);
#endif
    Information info{
        .name = name,
        .manufacturer = QStringLiteral("anland"),
        .model = QStringLiteral("anland"),
        .capabilities = m_hdrEnabled ? Capability::HighDynamicRange | Capability::WideColorGamut : Capabilities{},
        .internal = true,
        .maxPeakBrightness = m_hdrEnabled ? std::optional<double>(1000) : std::nullopt,
        .maxAverageBrightness = m_hdrEnabled ? std::optional<double>(1000) : std::nullopt,
    };
#ifdef ANLAND_KWIN_64
    info.bitsPerColorRange = {.min = m_hdrEnabled ? 10u : 8u, .max = m_hdrEnabled ? 10u : 8u};
#endif
    setInformation(info);
    qCInfo(KWIN_ANLAND) << (m_hdrAllowed ? "output color mode follows the Android app's HDR setting"
                                       : "HDR disabled by ANLAND_HDR; only SDR consumer buffers are accepted");
}

AnlandOutput::~AnlandOutput()
{
}

bool AnlandOutput::hdrAllowed() const
{
    return m_hdrAllowed;
}

bool AnlandOutput::hdrEnabled() const
{
    return m_hdrEnabled;
}

void AnlandOutput::setConsumerHdrMode(bool hdr, unsigned int sdrWhiteNits)
{
    Q_ASSERT(!hdr || m_hdrAllowed);
    if ((hdr && !m_hdrAllowed) || sdrWhiteNits < ANLAND_HDR_SDR_WHITE_MIN_NITS
        || sdrWhiteNits > ANLAND_HDR_SDR_WHITE_MAX_NITS) {
        return;
    }
    const bool modeChanged = hdr != m_hdrEnabled;
    const bool referenceChanged = sdrWhiteNits != m_hdrSdrWhiteNits;
    m_hdrSdrWhiteNits = sdrWhiteNits;
    if (!modeChanged && (!hdr || !referenceChanged)) {
        return;
    }
    m_hdrEnabled = hdr;
    m_wireColorDescription = makeColorDescription(hdr, true, m_hdrSdrWhiteNits);
    m_renderColorDescription = makeColorDescription(hdr, false, m_hdrSdrWhiteNits);

    Information info = m_information;
    info.capabilities = hdr ? Capability::HighDynamicRange | Capability::WideColorGamut : Capabilities{};
    info.maxPeakBrightness = hdr ? std::optional<double>(1000) : std::nullopt;
    info.maxAverageBrightness = hdr ? std::optional<double>(1000) : std::nullopt;
#ifdef ANLAND_KWIN_64
    info.bitsPerColorRange = {.min = hdr ? 10u : 8u, .max = hdr ? 10u : 8u};
#endif
    setInformation(info);

    State next = m_state;
    next.highDynamicRange = hdr;
    next.wideColorGamut = hdr;
    next.referenceLuminance = hdr ? m_hdrSdrWhiteNits : 200;
    next.colorDescription = m_wireColorDescription;
#ifdef ANLAND_KWIN_64
    next.originalColorDescription = m_wireColorDescription;
    next.maxBitsPerColor = hdr ? 10 : 8;
#endif
#ifdef ANLAND_KWIN_66
    next.blendingColor = m_renderColorDescription;
    next.layerBlendingColor = m_wireColorDescription;
#endif
    setState(next);
    // setInformation() only notifies about capability changes. Refresh the
    // brightness metadata too when the consumer's local color mode changes.
    if (modeChanged) {
        Q_EMIT brightnessMetadataChanged();
    }
    // ColorDescription changes invalidate the persistent scene and every encoded
    // consumer slot through AnlandEglLayer's colorDescriptionChanged connection.
    m_renderLoop->scheduleRepaint();

    if (hdr) {
        qCInfo(KWIN_ANLAND) << "consumer selected HDR: BT.2020/PQ, 10bit, peak 1000 nits, SDR white"
                           << m_hdrSdrWhiteNits << "nits";
    } else {
        qCInfo(KWIN_ANLAND) << "consumer selected SDR: sRGB, 8bit (Android app HDR is off)";
    }
}

bool AnlandOutput::setHdrSdrWhiteNits(unsigned int nits)
{
    if (nits < ANLAND_HDR_SDR_WHITE_MIN_NITS || nits > ANLAND_HDR_SDR_WHITE_MAX_NITS) {
        return false;
    }
    setConsumerHdrMode(m_hdrEnabled, nits);
    return true;
}

unsigned int AnlandOutput::hdrSdrWhiteNits() const
{
    return m_hdrSdrWhiteNits;
}

const AnlandColorDescription &AnlandOutput::renderColorDescription() const
{
    return m_renderColorDescription;
}

const AnlandColorDescription &AnlandOutput::wireColorDescription() const
{
    return m_wireColorDescription;
}

#ifndef ANLAND_KWIN_68
RenderLoop *AnlandOutput::renderLoop() const
{
    return m_renderLoop.get();
}
#endif

#ifdef ANLAND_KWIN_68
std::expected<void, OutputError> AnlandOutput::testPresentation(const std::shared_ptr<OutputFrame> &frame)
{
    Q_UNUSED(frame)
    return {};
}
std::expected<void, OutputError> AnlandOutput::present(const QList<OutputLayer *> &layersToUpdate, const std::shared_ptr<OutputFrame> &frame)
{
    Q_UNUSED(layersToUpdate)
#elif defined(ANLAND_KWIN_66)
bool AnlandOutput::testPresentation(const std::shared_ptr<OutputFrame> &frame)
{
    Q_UNUSED(frame)
    return true;
}
bool AnlandOutput::present(const QList<OutputLayer *> &layersToUpdate, const std::shared_ptr<OutputFrame> &frame)
{
    Q_UNUSED(layersToUpdate)
#else
void AnlandOutput::present(const std::shared_ptr<OutputFrame> &frame)
{
#endif
    // The scene has already been rendered into the daemon's dmabuf by the layer
    // (AnlandEglLayer::doEndFrame). Hand it to the consumer now.
    m_frame = frame;
#ifndef ANLAND_KWIN_66
    Q_EMIT outputChange(frame->damage());
#endif

    // The layer created a fence for the GPU work it just submitted; it belongs to
    // this frame's commit, so collect it before the backend submits.
    int fence = -1;
    if (m_eglLayer) {
        fence = m_eglLayer->takePendingFence();
    }

    const bool handedToConsumer = m_backend->presentFrame(fence);
    if (handedToConsumer) {
        // The frame is now SUBMITTED, not presented. It completes when the scene
        // reports PRESENTED (see onFramePresented()).
        m_awaitingPresent = true;
    } else {
        // Nothing was handed to the consumer this frame, so no PRESENTED event will
        // arrive for it. Drop the OutputFrame; reporting it as presented would
        // fabricate VSync feedback for a frame the consumer never received.
        m_awaitingPresent = false;
        m_frame.reset();
        m_renderLoop->scheduleRepaint();
    }
#ifdef ANLAND_KWIN_68
    return {};
#elif defined(ANLAND_KWIN_66)
    return true;
#endif
}

void AnlandOutput::init(const QSize &pixelSize, int refresh, qreal scale)
{
    // refresh is in mHz, like RenderLoop/OutputMode expect.
    if (refresh <= 0) {
        refresh = 120000;
    }
    m_renderLoop->setRefreshRate(refresh);

    #ifdef ANLAND_KWIN_67
    auto mode = std::make_shared<OutputMode>(OutputModeline(pixelSize, refresh, OutputModeline::Flag::Preferred));
#else
    auto mode = std::make_shared<OutputMode>(pixelSize, refresh, OutputMode::Flag::Preferred);
#endif

    State state{
        .position = QPoint(0, 0),
        .scale = scale,
        .modes = {mode},
        .currentMode = mode,
    };
    state.highDynamicRange = m_hdrEnabled;
    state.wideColorGamut = m_hdrEnabled;
    state.referenceLuminance = m_hdrEnabled ? m_hdrSdrWhiteNits : 200;
    state.colorDescription = m_wireColorDescription;
#ifdef ANLAND_KWIN_64
    state.originalColorDescription = m_wireColorDescription;
    state.maxBitsPerColor = m_hdrEnabled ? 10 : 8;
#endif
#ifdef ANLAND_KWIN_66
    state.blendingColor = m_renderColorDescription;
    state.layerBlendingColor = m_wireColorDescription;
#endif
    // Base applyChanges() only changes layout on these KWin versions. Do not
    // implement a KScreen HDR toggle: Android cannot change encoding with it.
    setState(state);
}

void AnlandOutput::updateEnabled(bool enabled)
{
    State next = m_state;
    next.enabled = enabled;
    setState(next);
}

void AnlandOutput::setRefreshRate(int refresh)
{
    // refresh is in mHz. Ignore noise and no-op changes; RenderLoop::setRefreshRate
    // already guards the latter, but we also skip rebuilding the mode below.
    if (refresh <= 0 || refresh == m_renderLoop->refreshRate()) {
        return;
    }
    m_renderLoop->setRefreshRate(refresh);

    // Keep the OutputMode in lockstep with the RenderLoop, mirroring init(), so
    // currentMode()->refreshRate() and any mode-based logic see the new rate.
    #ifdef ANLAND_KWIN_67
    auto mode = std::make_shared<OutputMode>(OutputModeline(modeSize(), refresh, OutputModeline::Flag::Preferred));
#else
    auto mode = std::make_shared<OutputMode>(modeSize(), refresh, OutputMode::Flag::Preferred);
#endif
    State next = m_state;
    next.modes = {mode};
    next.currentMode = mode;
    setState(next);
}

void AnlandOutput::completeFrame()
{
    if (!m_frame) {
        return;
    }
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    // presented() -> RenderLoopPrivate::notifyFrameCompleted(): decrements the
    // pending-frame count, delivers presentation feedback and schedules the next
    // repaint through the standard path (the 6.x equivalent of the 5.27 backend's
    // RenderLoopPrivate::notifyFrameCompleted()).
    m_frame->presented(now, PresentationMode::VSync);
    m_frame.reset();
}

void AnlandOutput::onConsumerReady()
{
    if (m_awaitingPresent) {
        m_awaitingPresent = false;
        completeFrame();
    }
    // Render the next frame in lockstep with the consumer, exactly as the 5.27
    // backend did: the consumer's buffer-ready drives scheduleRepaint().
    m_renderLoop->scheduleRepaint();
}

void AnlandOutput::onFramePresented()
{
    // The scene confirmed the frame reached the consumer. This is the same
    // completion path onConsumerReady() uses; the difference is that it is now
    // driven by an explicit PRESENTED event rather than by the raw buffer-ready
    // signal, so a dropped or superseded commit can never complete a frame.
    onConsumerReady();
}

void AnlandOutput::onFrameDropped()
{
    if (m_awaitingPresent) {
        m_awaitingPresent = false;
        // The frame will never be presented, so it must NOT be reported as
        // presented. Releasing the OutputFrame makes its destructor call
        // notifyFrameDropped(), balancing the RenderLoop's pending-frame count so
        // the loop does not stall.
        m_frame.reset();
    }
    m_renderLoop->scheduleRepaint();
}

void AnlandOutput::onOutputGeometryChanged(const QSize &newSize, int refresh)
{
    if (refresh > 0) {
        setRefreshRate(refresh);
    }
    if (newSize.isValid() && !newSize.isEmpty()) {
        resize(newSize);
    }
}

void AnlandOutput::resize(const QSize &newSize)
{
    if (newSize == modeSize() || !newSize.isValid()) {
        return;
    }

    qCInfo(KWIN_ANLAND) << "resizing output to" << newSize;

    // Keep the same refresh rate; update both the OutputMode and the RenderLoop
    // pacing. Mirroring setRefreshRate() / init().
    const int refresh = m_renderLoop->refreshRate();
    #ifdef ANLAND_KWIN_67
    auto mode = std::make_shared<OutputMode>(OutputModeline(newSize, refresh, OutputModeline::Flag::Preferred));
#else
    auto mode = std::make_shared<OutputMode>(newSize, refresh, OutputMode::Flag::Preferred);
#endif
    State next = m_state;
    next.modes = {mode};
    next.currentMode = mode;
    setState(next);

    // setState() only emits currentModeChanged(); Workspace recomputes geometry
    // when OutputBackend::outputsQueried drives updateOutputs()/desktopResized().
    m_backend->notifyOutputsChanged();

    // Invalidate any in-flight frame: the mode just changed, so the buffer that
    // was being presented corresponds to a different layout.
    if (m_awaitingPresent) {
        m_awaitingPresent = false;
        m_frame.reset();
    }
}

#ifdef ANLAND_KWIN_66
void AnlandOutput::setEglLayer(std::unique_ptr<AnlandEglLayer> &&layer)
{
    m_eglLayer = std::move(layer);
}
#else
void AnlandOutput::setEglLayer(AnlandEglLayer *layer)
{
    m_eglLayer = layer;
}
#endif

AnlandEglLayer *AnlandOutput::eglLayer() const
{
#ifdef ANLAND_KWIN_66
    return m_eglLayer.get();
#else
    return m_eglLayer;
#endif
}

void AnlandOutput::stopRendering()
{
    if (m_awaitingPresent) {
        m_awaitingPresent = false;
        // No buffer-ready will arrive for the in-flight frame now. Releasing the
        // un-presented OutputFrame makes its destructor call notifyFrameDropped(),
        // balancing the RenderLoop's pending-frame count so it does not stall
        // (the 6.x equivalent of the 5.27 backend's notifyFrameFailed()).
        m_frame.reset();
    }

    // Pause compositing while the consumer is gone: there are no dmabufs to render
    // into. The bool keeps inhibit/uninhibit balanced (uninhibit() asserts > 0).
    if (!m_renderingInhibited) {
        m_renderLoop->inhibit();
        m_renderingInhibited = true;
    }
}

void AnlandOutput::resumeRendering()
{
    // Consumer is back and dmabufs are imported: resume compositing. uninhibit()
    // reschedules the next repaint internally; the backend then marks the layer
    // dirty (scheduleRepaint) so the first frame actually paints.
    if (m_renderingInhibited) {
        m_renderLoop->uninhibit();
        m_renderingInhibited = false;
    }
}

} // namespace KWin

#include "moc_anland_output.cpp"
