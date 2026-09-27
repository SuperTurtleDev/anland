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
#include "anland_logging.h"

#include "core/renderbackend.h" // OutputFrame
#include "core/renderloop.h"

#include <chrono>

namespace KWin
{

AnlandOutput::AnlandOutput(AnlandBackend *parent, const QString &name)
#ifdef ANLAND_KWIN_66
    : BackendOutput()
#else
    : Output(parent)
#endif
    , m_backend(parent)
    , m_renderLoop(std::make_unique<RenderLoop>(this))
{
#ifdef ANLAND_KWIN_66
    setParent(parent);
#endif
    setInformation(Information{
        .name = name,
        .manufacturer = QStringLiteral("anland"),
        .model = QStringLiteral("anland"),
        .internal = true,
    });
}

AnlandOutput::~AnlandOutput()
{
}

RenderLoop *AnlandOutput::renderLoop() const
{
    return m_renderLoop.get();
}

#ifdef ANLAND_KWIN_66
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
#ifdef ANLAND_KWIN_66
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

    setState(State{
        .position = QPoint(0, 0),
        .scale = scale,
        .modes = {mode},
        .currentMode = mode,
    });
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