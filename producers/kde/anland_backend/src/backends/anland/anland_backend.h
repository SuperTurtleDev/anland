/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-License-Identifier: GPL-2.0-or-later

    Native KWin output+input backend that talks to the Android display daemon
    (via the unified anland_device layer), instead of running nested inside the
    weston "anland" compositor. Port of weston/libweston/backend-anland/anland.c
    to KWin's OutputBackend architecture. Target: Debian 13 trixie (KWin 6.3.6).
*/
#pragma once

#include "core/outputbackend.h"

#include <QByteArray>
#include <QPointer>
#include <QPointF>
#include <QVector>
#include <cstdint>
#include <memory>
#include <sys/types.h>

extern "C" {
#include "libdisplay_producer/anland_device.h"
#include "libdisplay_producer/anland_scene.h"
#include "libdisplay_producer/anland_present.h"
#include "libdisplay_producer/anland_de_backend.h"
}

#include "libdisplay_producer/anland_audio.h"
#include "libdisplay_producer/anland_camera.h"

#include "wayland/pointerconstraints_v1.h"
#include "wayland/surface.h"

class QSocketNotifier;
class QTimer;

namespace KWin
{

class AnlandOutput;
class AnlandInputDevice;
class AbstractDataSource;
class DrmDevice;
class EglDisplay;
#ifdef ANLAND_KWIN_67
class RenderDevice;
#endif
class InputBackend;
class OpenGLBackend;
class EglBackend;
class BackendOutput;

class KWIN_EXPORT AnlandBackend : public OutputBackend
{
    Q_OBJECT

public:
    explicit AnlandBackend(const QString &socketPath = QString(), QObject *parent = nullptr);
    ~AnlandBackend() override;

    bool initialize() override;

#ifdef ANLAND_KWIN_66
    std::unique_ptr<EglBackend> createOpenGLBackend() override;
#else
    std::unique_ptr<OpenGLBackend> createOpenGLBackend() override;
#endif
    std::unique_ptr<InputBackend> createInputBackend() override;
    QList<CompositingType> supportedCompositors() const override;
#ifdef ANLAND_KWIN_66
    QList<BackendOutput *> outputs() const override;
#else
    Outputs outputs() const override;
#endif

    EglDisplay *sceneEglDisplayObject() const override;
#ifdef ANLAND_KWIN_67
    // KWin 6.7 groups the DRM node and its EGL display in RenderDevice.
    RenderDevice *renderDevice() const;
#else
    // Older KWin owns the surfaceless EGL display separately.
    void setEglDisplay(std::unique_ptr<EglDisplay> &&display);
#endif

    anland_device *device() const
    {
        return m_device;
    }
    bool consumerReadyForImport() const
    {
        return m_device && !m_inFallback && anland_device_fb_count(m_device) > 0;
    }
    /**
     * The shared presentation facade (owns the scene, device and presentation
     * backend). DE code uses it for frame transactions and target queries instead
     * of reaching into the transport.
     */
    anland_de_backend *presentBackend() const
    {
        return m_presentBackend;
    }
    /** Generation of the consumer session this backend is currently serving. */
    uint64_t sessionGeneration() const
    {
        return m_sessionGeneration;
    }
    /**
     * DRM render device backing GL/EGL. KWin dereferences this during OpenGL
     * compositor setup (syncobj-timeline / dmabuf-feedback probing), so it must
     * be non-null; AnlandEglBackend::drmDevice() forwards to it.
     */
#ifdef ANLAND_KWIN_67
    DrmDevice *drmDevice() const;
#else
    DrmDevice *drmDevice() const
    {
        return m_drmDevice.get();
    }
#endif
    AnlandInputDevice *inputDevice() const
    {
        return m_inputDevice.get();
    }

    /**
     * Called by AnlandEglBackend::present(): submit the freshly painted frame
     * through the shared commit contract and hand it to the consumer.
     *
     * fenceFd is the layer's render fence for the frame (ownership taken, may be
     * -1); it rides along as the commit's acquire fence so SurfaceFlinger can wait
     * GPU-side. Returns true if the frame was handed over, false otherwise — the
     * caller uses this to decide how to complete the RenderLoop frame.
     *
     * The commit is FLATTENED onto the single legacy output: KWin composites every
     * window into the one output buffer, so the commit describes one layer covering
     * the output. See anland_scene_legacy.h for what that does and does not carry.
     */
    bool presentFrame(int fenceFd);

    /** Re-run the Workspace output layout after an output changed its mode at
     *  runtime (AnlandOutput::resize). The backend mutates the mode directly via
     *  setState() instead of going through OutputConfiguration, so it must emit
     *  outputsQueried() itself; the mode-changed signal alone does not trigger a
     *  relayout. */
    void handleBufferImportFailure();

    void notifyOutputsChanged()
    {
        Q_EMIT outputsQueried();
    }

private:
    void setupNotifiers();
    void teardownNotifiers();
    void onInputReadable();
    void onBufferReady();
    /** Drain the scene's event queue and act on each event (frame completion,
     *  drops). Called after servicing the transport in onBufferReady(). */
    void dispatchSceneEvents();
    void processInputEvent(const anland_device_input_t &ev);
    QPointF mapInputToLogical(const QPointF &devicePoint) const;
    QPointF mapInputDeltaToLogical(const QPointF &deviceDelta) const;
    void onReconnectTimer();
    void enterFallback();

    void onClipboardChanged();
    void sendClipboardToConsumer(const QByteArray &text);
    void sendClipboardToKWin(const QByteArray &text);
    void sendTextInputToKWin(const QByteArray &text);

    // Consumer-var bridge: force the Android app into pointer-capture (relative
    // mouse) mode while a Wayland client holds an active pointer constraint --
    // either a zwp_locked_pointer_v1 (native game pointer lock, and Xwayland's
    // hidden-cursor+confine emulation of it) or a zwp_confined_pointer_v1
    // (Xwayland visible-cursor confine, and native confinement). This overrides
    // the user's pointer_capture setting. The var is resent on every reconnect
    // because the consumer resets it to 0 on fallback.
    void setupMouseCaptureTracking();
    void updateMouseCaptureVar();
    void sendConsumerVar(uint32_t var, uint32_t value);

    // Foreground scheduling: the compositor subtree is boosted once per
    // connection; focus changes restore the previous client before boosting
    // the next one with a self-contained event.
    void setupSchedulingTracking();
    void updateActiveScheduling(bool force = false);
    void sendSchedulingEvent(pid_t pid, uint8_t flags);

    static void fallbackTrampoline(void *data);

    QString m_socketPath;
    /**
     * The shared commit contract, over the legacy fullscreen transport. Owns the
     * device (m_device is borrowed from it). Frame presentation goes through this
     * rather than the raw device, so this backend speaks the same contract as the
     * other DE adapters and only the presentation backend underneath changes.
     */
    anland_de_backend *m_presentBackend = nullptr;
    anland_scene *m_scene = nullptr;
    anland_device *m_device = nullptr;
    /** The one layer representing this backend's output. The legacy transport
     *  flattens the scene, so there is exactly one (see anland_scene_legacy.h). */
    anland_layer_id m_outputLayer = 0;

    /**
     * Session generation this backend is currently serving, and the render target
     * the shared layer published for it. KWin must not read the legacy
     * consumer-selected slot itself: the shared layer validates the slot and
     * stamps it with the session it belongs to, so a target from a session this
     * backend has not imported buffers for can be recognised and ignored.
     */
    uint64_t m_sessionGeneration = 0;
    int m_currentBuffer = -1;
    uint32_t m_bufferCount = 0;

#ifdef ANLAND_KWIN_67
    std::unique_ptr<RenderDevice> m_renderDevice;
#else
    std::unique_ptr<DrmDevice> m_drmDevice;
    std::unique_ptr<EglDisplay> m_eglDisplay;
#endif
    QVector<AnlandOutput *> m_outputs;
    std::unique_ptr<AnlandInputDevice> m_inputDevice;

    QSocketNotifier *m_inputNotifier = nullptr;
    QSocketNotifier *m_bufReadyNotifier = nullptr;
    QTimer *m_reconnectTimer = nullptr;

    bool m_inFallback = false;
    QByteArray m_clipboardText;
    std::unique_ptr<AbstractDataSource> m_clipboardSource;

    // Active pointer-constraint tracking for CONSUMER_VAR_CAPTURE_MOUSE. We mirror
    // KWin's own updatePointerConstraints() triggers (window activation + the
    // focused surface's pointerConstraintsChanged + each constraint's own
    // lockedChanged/confinedChanged) to observe zwp_locked_pointer_v1 and
    // zwp_confined_pointer_v1 enable/disable without touching core input code.
    // The lock covers native pointer lock plus Xwayland's hidden-cursor+confine
    // emulation; the confine covers Xwayland visible-cursor confine and native
    // confinement. The QPointers auto-null when the KWin objects are destroyed,
    // and Qt auto-clears the connections to a destroyed QObject, so re-derivation
    // stays safe.
    QPointer<SurfaceInterface> m_captureMouseSurface;
    QPointer<LockedPointerV1Interface> m_captureMouseLock;
    QPointer<ConfinedPointerV1Interface> m_captureMouseConfined;
    QMetaObject::Connection m_captureMouseSurfaceConn;
    QMetaObject::Connection m_captureMouseLockConn;
    QMetaObject::Connection m_captureMouseConfinedConn;
    bool m_captureMouseActive = false; // last CONSUMER_VAR_CAPTURE_MOUSE value sent

    pid_t m_activeSchedulingPid = -1;
};

} // namespace KWin