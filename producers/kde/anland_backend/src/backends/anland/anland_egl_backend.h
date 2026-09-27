/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-License-Identifier: GPL-2.0-or-later

    OpenGL/EGL render backend for the anland backend. It renders the KWin scene
    directly into the dmabuf buffers provided by the display daemon (imported by
    fd) and tells the daemon to present them. Modeled on VirtualEglBackend, with
    the render target being one of the daemon's dmabufs instead of an internal
    FBO. The consumer rotates the buffer index externally (shared memory), so the
    layer keeps per-buffer accumulated damage (buffer-age equivalent), exactly
    like weston's backend-anland.
*/
#pragma once

#include "core/outputlayer.h"
#ifdef ANLAND_KWIN_66
#include "opengl/eglbackend.h"
#else
#include "platformsupport/scenes/opengl/abstract_egl_backend.h"
#endif

#ifndef ANLAND_KWIN_66
#include <QRegion>
#endif
#include <array>
#include <map>
#include <memory>

extern "C" {
#include "anland_device.h"
}

namespace KWin
{
class GLFramebuffer;
class GLTexture;
class DrmDevice;
class OutputFrame;
class Output;
class SurfacePixmap;
class SurfaceTexture;
class AnlandBackend;
class AnlandEglBackend;
class AnlandOutput;

class AnlandEglLayer : public OutputLayer
{
public:
    AnlandEglLayer(AnlandOutput *output, AnlandEglBackend *backend);
    ~AnlandEglLayer() override;

    std::optional<OutputLayerBeginFrameInfo> doBeginFrame() override;
#ifdef ANLAND_KWIN_66
    bool doEndFrame(const Region &renderedDeviceRegion, const Region &damagedDeviceRegion, OutputFrame *frame) override;
#else
    bool doEndFrame(const QRegion &renderedDeviceRegion, const QRegion &damagedDeviceRegion, OutputFrame *frame) override;
#endif

    // OutputLayer pure virtuals. anland imports the consumer's dmabufs and
    // never allocates or scans out through DRM itself, so these are stubs.
    DrmDevice *scanoutDevice() const override;
    #ifdef ANLAND_KWIN_67
    FormatModifierMap supportedDrmFormats() const override;
#else
    QHash<uint32_t, QList<uint64_t>> supportedDrmFormats() const override;
#endif

    // Buffer lifetime is driven by the backend state machine, not doBeginFrame():
    // importBuffers() imports the daemon's dmabuf set on (re)connect and arms a
    // full-output (infinite) repaint on every rotation buffer; releaseBuffers()
    // drops them on fallback. Both make
    // the GL context current, so they are safe to call from outside a frame.
    bool importBuffers(int count);
#ifdef ANLAND_KWIN_66
    void releaseBuffers() override;
#else
    void releaseBuffers();
#endif
    std::shared_ptr<GLTexture> texture() const;

    /**
     * The render fence for the GPU work doEndFrame() just submitted.
     *
     * The fence no longer goes straight to the device: it belongs to the frame's
     * commit (anland_layer_state_t::acquire_fence_fd), which the backend submits
     * when it presents. AnlandOutput::present() collects it via takePendingFence().
     * Ownership transfers with the fd.
     */
    void setPendingFence(int fd);
    int takePendingFence();

    /** Update the transport handle after the backend rebuilds its session. */
    void setDevice(anland_device *device)
    {
        m_device = device;
    }

    /** Which rotation buffer the current frame rendered into (0-based). */
    int currentBufferIndex() const
    {
        return m_currentIndex;
    }

    /**
     * The render target this layer must draw into, as published by the shared
     * presentation layer. Returns -1 when no target is usable (no session, or a
     * target from a session whose buffers are not imported here), in which case
     * the layer must not begin a frame.
     *
     * This is the ONLY source of the current buffer: doBeginFrame() uses it, and
     * the backend commits the same slot. Reading the transport's own selection
     * separately would let a frame be rendered into one buffer and submitted as
     * another.
     */
    int targetBufferIndex() const;

    /** Wake the compositor only when the newly selected buffer owes a repaint. */
    void scheduleBufferCatchUp(int index);

private:
    void onOutputTransformChanged();

    AnlandEglBackend *const m_backend;
    AnlandOutput *m_output;
    // Updated when the backend rebuilds its transport after daemon loss. The
    // layer owns no device state; the scene adapter remains the owner.
    anland_device *m_device;

    int m_bufCount = 0;
    int m_currentIndex = 0;
    /* Render fence for the frame being composed, or -1. Created in doEndFrame(),
     * consumed by the backend's commit when it presents. */
    int m_pendingFence = -1;
    std::array<std::shared_ptr<GLTexture>, ANLAND_DEVICE_MAX_BUFS> m_textures;
    std::array<std::unique_ptr<GLFramebuffer>, ANLAND_DEVICE_MAX_BUFS> m_fbos;
    /* Per-buffer accumulated damage: each buffer must remember everything that
     * changed since it was last rendered, because the consumer rotates the
     * selected index out from under us. */
#ifdef ANLAND_KWIN_66
    std::array<Region, ANLAND_DEVICE_MAX_BUFS> m_accumDamage;
#else
    std::array<QRegion, ANLAND_DEVICE_MAX_BUFS> m_accumDamage;
#endif
};

#ifdef ANLAND_KWIN_66
class AnlandEglBackend : public EglBackend
#else
class AnlandEglBackend : public AbstractEglBackend
#endif
{
    Q_OBJECT

public:
    AnlandEglBackend(AnlandBackend *b);
    ~AnlandEglBackend() override;

#ifdef ANLAND_KWIN_67
    bool init() override;
#else
    void init() override;
#endif
#ifdef ANLAND_KWIN_66
    QList<OutputLayer *> compatibleOutputLayers(BackendOutput *output) override;
#endif
#ifndef ANLAND_KWIN_66
    std::unique_ptr<SurfaceTexture> createSurfaceTextureWayland(SurfacePixmap *pixmap) override;
    std::pair<std::shared_ptr<KWin::GLTexture>, ColorDescription> textureForOutput(Output *output) const override;
    OutputLayer *primaryLayer(Output *output) override;
    bool present(Output *output, const std::shared_ptr<OutputFrame> &frame) override;
#endif
    DrmDevice *drmDevice() const override;

    AnlandBackend *backend() const
    {
        return m_backend;
    }
    anland_device *device() const;

private:
    bool initializeEgl();
    bool initRenderingContext();

#ifdef ANLAND_KWIN_66
    void addOutput(BackendOutput *output);
    void removeOutput(BackendOutput *output);
#else
    void addOutput(Output *output);
    void removeOutput(Output *output);
#endif

    AnlandBackend *m_backend;
#ifndef ANLAND_KWIN_66
    std::map<Output *, std::unique_ptr<AnlandEglLayer>> m_outputs;
#endif
};

} // namespace KWin