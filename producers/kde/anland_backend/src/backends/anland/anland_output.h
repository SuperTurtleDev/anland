/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-License-Identifier: GPL-2.0-or-later

    Native output for the "anland" backend. The render cycle is paced by the
    display daemon's eventfds, not a software timer: present() hands the buffer
    to the consumer (render-done fence channel) and the consumer's buffer-ready signal
    (buf_ready_efd) completes the frame — mirroring how the DRM backend completes
    a frame on a page-flip event. This keeps KWin's compositing in lockstep with
    the frontend instead of free-running on a vsync timer.
*/
#pragma once

#ifdef ANLAND_KWIN_66
#include "core/backendoutput.h"
#else
#ifdef ANLAND_KWIN_66
#include "core/backendoutput.h"
#else
#ifdef ANLAND_KWIN_66
#include "core/backendoutput.h"
#else
#include "core/output.h"
#endif
#endif
#endif

#include <QObject>
#include <memory>

namespace KWin
{

class AnlandBackend;
class AnlandEglLayer;
class OutputFrame;
class OutputLayer;
class RenderLoop;

#ifdef ANLAND_KWIN_66
class AnlandOutput : public BackendOutput
#else
#ifdef ANLAND_KWIN_66
class AnlandOutput : public BackendOutput
#else
#ifdef ANLAND_KWIN_66
class AnlandOutput : public BackendOutput
#else
class AnlandOutput : public Output
#endif
#endif
#endif
{
    Q_OBJECT

public:
    AnlandOutput(AnlandBackend *parent, const QString &name);
    ~AnlandOutput() override;

    RenderLoop *renderLoop() const override;
#ifdef ANLAND_KWIN_66
    bool testPresentation(const std::shared_ptr<OutputFrame> &frame) override;
    bool present(const QList<OutputLayer *> &layersToUpdate, const std::shared_ptr<OutputFrame> &frame) override;
#else
#ifdef ANLAND_KWIN_66
    bool testPresentation(const std::shared_ptr<OutputFrame> &frame) override;
    bool present(const QList<OutputLayer *> &layersToUpdate, const std::shared_ptr<OutputFrame> &frame) override;
#else
#ifdef ANLAND_KWIN_66
    bool testPresentation(const std::shared_ptr<OutputFrame> &frame) override;
    bool present(const QList<OutputLayer *> &layersToUpdate, const std::shared_ptr<OutputFrame> &frame) override;
#else
    void present(const std::shared_ptr<OutputFrame> &frame);
#endif
#endif
#endif

    /** @p pixelSize and @p refresh (in mHz) come from the display daemon. */
    void init(const QSize &pixelSize, int refresh, qreal scale);
    void updateEnabled(bool enabled);

    /** Retune the render cycle when the consumer reports a new display refresh
     *  rate at runtime (INPUT_TYPE_DISPLAY_REFRESH). @p refresh is in mHz; values
     *  <= 0 or equal to the current rate are ignored. Updates both the RenderLoop
     *  pacing and the OutputMode so currentMode() stays consistent. */
    void setRefreshRate(int refresh);

    /** Consumer signalled buffer-ready (buf_ready_efd): complete the in-flight
     *  frame and schedule the next one, keeping the cycle consumer-paced. */
    void onConsumerReady();

    /** The scene reported the frame as PRESENTED: the frame reached the consumer,
     *  so it may complete and the next one may be scheduled. */
    void onFramePresented();

    /** The scene reported the commit as DROPPED (superseded / invalidated /
     *  backend error): the frame will never reach the consumer. Fail it without
     *  claiming it was presented, so the RenderLoop's frame accounting does not
     *  stall. */
    void onFrameDropped();

    /** The scene reported new output geometry (consumer reconnected at a
     *  different size or refresh). @p refresh is in mHz. */
    void onOutputGeometryChanged(const QSize &newSize, int refresh);

    /** Reconfigure the output when the consumer uses a different buffer size
     *  (e.g. screen rotation / resolution switch). Updates the OutputMode and asks
     *  the backend to emit outputsQueried() so the Workspace re-lays-out windows
     *  for the new size. */
    void resize(const QSize &newSize);

    /** Consumer went away (fallback): fail any in-flight frame and inhibit() the
     *  RenderLoop so the compositor stops trying to render into dmabufs that no
     *  longer exist. */
    void stopRendering();

    /** Consumer reconnected: uninhibit() the RenderLoop so compositing resumes. */
    void resumeRendering();

    /** The primary render layer for this output. Owned by AnlandEglBackend and
     *  wired up in AnlandEglBackend::addOutput(); null while no render backend is
     *  attached. Lets AnlandBackend drive the layer through its output — e.g.
     *  schedule a repaint when leaving fallback. */
#ifdef ANLAND_KWIN_66
    void setEglLayer(std::unique_ptr<AnlandEglLayer> &&layer);
#else
    void setEglLayer(AnlandEglLayer *layer);
#endif
    AnlandEglLayer *eglLayer() const;

private:
    void completeFrame();

    Q_DISABLE_COPY(AnlandOutput);

    AnlandBackend *m_backend;
    std::unique_ptr<RenderLoop> m_renderLoop;
    std::shared_ptr<OutputFrame> m_frame;
#ifdef ANLAND_KWIN_66
    std::unique_ptr<AnlandEglLayer> m_eglLayer;
#else
    AnlandEglLayer *m_eglLayer = nullptr;
#endif
    bool m_awaitingPresent = false;
    // Tracks our RenderLoop::inhibit() so inhibit/uninhibit stay balanced
    // (uninhibit() asserts the count is > 0). See stopRendering()/resumeRendering().
    bool m_renderingInhibited = false;
};

} // namespace KWin