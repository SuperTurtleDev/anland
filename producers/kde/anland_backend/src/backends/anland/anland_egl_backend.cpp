/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "anland_egl_backend.h"
#include "anland_backend.h"
#include "anland_logging.h"
#include "anland_output.h"

// kwin
#include "core/graphicsbuffer.h" // DmaBufAttributes
#include "core/output.h" // OutputTransform
#include "core/renderloop.h"
#include "opengl/eglcontext.h"
#include "opengl/egldisplay.h"
#include "opengl/eglnativefence.h"
#include "opengl/glutils.h"
#ifndef ANLAND_KWIN_66
#include "platformsupport/scenes/opengl/basiceglsurfacetexture_wayland.h"
#endif
#include "utils/filedescriptor.h"

#include <drm_fourcc.h>
#include <unistd.h>

#ifndef EGL_PLATFORM_SURFACELESS_MESA
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#endif

namespace KWin
{
#ifdef ANLAND_KWIN_66
#define ANLAND_FULL_REPAINT Region::infinite()
#else
#define ANLAND_FULL_REPAINT infiniteRegion()
#endif

/*
 * The daemon's screen_info.format / buf_info.format uses the consumer-side
 * pixel-format enum (see common/protocol.h). 1 == RGBA_8888 in Android memory
 * layout, which is ABGR8888 in DRM fourcc terms; everything else is treated as
 * XRGB8888. Mirrors protocol_format_to_drm() in weston's backend-anland.
 */
static uint32_t protocol_format_to_drm(uint32_t fmt)
{
    switch (fmt) {
    case 1:
        return DRM_FORMAT_ABGR8888;
    default:
        return DRM_FORMAT_XRGB8888;
    }
}

AnlandEglLayer::AnlandEglLayer(AnlandOutput *output, AnlandEglBackend *backend)
#ifdef ANLAND_KWIN_66
    : OutputLayer(output, OutputLayerType::Primary)
#else
    : OutputLayer(output)
#endif
    , m_backend(backend)
    , m_output(output)
    , m_device(backend->device())
{
    // React to runtime orientation changes through the output's transformChanged
    // signal instead of polling the transform in the per-frame render path.
#ifdef ANLAND_KWIN_66
    connect(m_output, &BackendOutput::transformChanged, this, &AnlandEglLayer::onOutputTransformChanged);
#else
    connect(m_output, &Output::transformChanged, this, &AnlandEglLayer::onOutputTransformChanged);
#endif
}

AnlandEglLayer::~AnlandEglLayer()
{
    // Avoid leaving a dangling pointer in the output when we're destroyed without a
    // removeOutput() call (e.g. ~AnlandEglBackend clearing m_outputs).
#ifndef ANLAND_KWIN_66
    if (m_output && m_output->eglLayer() == this) {
        m_output->setEglLayer(nullptr);
    }
#endif
    // A fence created for a frame that was never committed is ours to close.
    setPendingFence(-1);
    releaseBuffers();
}

void AnlandEglLayer::releaseBuffers()
{
    // Destroying the GL textures/framebuffers needs the context current. Callers
    // (backend state machine on fallback, ~AnlandEglLayer) may run outside a frame.
#ifdef ANLAND_KWIN_66
    m_backend->openglContext()->makeCurrent();
#else
    m_backend->makeCurrent();
#endif

    for (int i = 0; i < ANLAND_DEVICE_MAX_BUFS; i++) {
        m_fbos[i].reset();
        m_textures[i].reset();
        #ifdef ANLAND_KWIN_66
        m_accumDamage[i] = Region();
#else
        m_accumDamage[i] = QRegion();
#endif
    }
    m_bufCount = 0;
}

bool AnlandEglLayer::importBuffers(int count)
{
    if (!m_device || count <= 0 || count > ANLAND_DEVICE_MAX_BUFS) {
        qCWarning(KWIN_ANLAND) << "cannot import anland buffers without a valid device"
                                << "count" << count;
        releaseBuffers();
        return false;
    }

#ifdef ANLAND_KWIN_66
    m_backend->openglContext()->makeCurrent();
#else
    m_backend->makeCurrent();
#endif

    releaseBuffers();

    // The consumer reads this dmabuf top-down, while GL renders bottom-up, so the
    // content transform always carries a vertical flip. On top of that we fold in
    // the output's configured rotation, so the scene is rendered pre-rotated into
    // the consumer's fixed-size dmabuf.
    const OutputTransform contentTransform = m_output->transform().combine(OutputTransform::FlipY);

    for (int i = 0; i < count; i++) {
        anland_device_fb_t fb;
        if (anland_device_get_fb(m_device, i, &fb) < 0) {
            qCWarning(KWIN_ANLAND) << "failed to get dmabuf info for buffer" << i;
            releaseBuffers();
            return false;
        }

        /* The per-buffer width/height come from the consumer's native resolution
         * (fb, filled by collect_dmabufs). If it differs from the current
         * OutputMode, resize the output to match. All buffers in a set share the
         * same size, so we only need to check the first buffer. */
        if (i == 0) {
            const QSize bufSize(fb.width, fb.height);
            if (bufSize != m_output->modeSize() && bufSize.isValid()) {
                qCInfo(KWIN_ANLAND) << "dmabuf size changed, resizing output to" << bufSize;
                m_output->resize(bufSize);
            }
        }
        const QSize actual(fb.width, fb.height);
        if (!actual.isValid() || actual != m_output->modeSize()) {
            qCWarning(KWIN_ANLAND) << "dmabuf size disagrees with output mode" << i
                                   << actual << m_output->modeSize();
            close(fb.fd);
            releaseBuffers();
            return false;
        }
        DmaBufAttributes attrs;
        attrs.planeCount = 1;
        attrs.width = actual.width();
        attrs.height = actual.height();
        attrs.format = protocol_format_to_drm(fb.format);
        attrs.modifier = fb.modifier;
        // anland_device_get_fb() returns a caller-owned dup of the dmabuf fd;
        // DmaBufAttributes (and the EGLImage we hand the fd to) takes it over.
        attrs.fd[0] = FileDescriptor(fb.fd);
        attrs.offset[0] = static_cast<int>(fb.offset);
        attrs.pitch[0] = static_cast<int>(fb.stride);

        // EglBackend::importDmaBufAsTexture() builds the EGLImage and wraps it in
        // a GLTexture in one step (the 5.27-era manual EGLImageKHR +
        // EGLImageTexture(...) dance is gone in 6.x).
        std::shared_ptr<GLTexture> texture = m_backend->importDmaBufAsTexture(attrs);
        if (!texture) {
            qCWarning(KWIN_ANLAND) << "failed to import dmabuf" << i << "as texture";
            releaseBuffers();
            return false;
        }

        texture->setContentTransform(contentTransform);
        auto fbo = std::make_unique<GLFramebuffer>(texture.get());
        if (!fbo->valid()) {
            qCWarning(KWIN_ANLAND) << "framebuffer for dmabuf" << i << "is not complete";
            releaseBuffers();
            return false;
        }

        qCDebug(KWIN_ANLAND) << "imported buffer" << i << "fd" << fb.fd << actual
                             << "fmt" << Qt::hex << attrs.format << "mod" << attrs.modifier;

        m_textures[i] = std::move(texture);
        m_fbos[i] = std::move(fbo);
        // Freshly imported dmabuf has undefined contents: owe it a full repaint.
        m_accumDamage[i] = ANLAND_FULL_REPAINT;
    }

    m_bufCount = count;
    return true;
}

void AnlandEglLayer::onOutputTransformChanged()
{
    const OutputTransform contentTransform = m_output->transform().combine(OutputTransform::FlipY);
    for (int i = 0; i < m_bufCount; i++) {
        m_textures[i]->setContentTransform(contentTransform);
        m_accumDamage[i] = ANLAND_FULL_REPAINT;
    }
#ifdef ANLAND_KWIN_66
    addDeviceRepaint(ANLAND_FULL_REPAINT);
#else
    addRepaint(ANLAND_FULL_REPAINT);
#endif
}

std::optional<OutputLayerBeginFrameInfo> AnlandEglLayer::doBeginFrame()
{
#ifdef ANLAND_KWIN_66
    m_backend->openglContext()->makeCurrent();
#else
    m_backend->makeCurrent();
#endif

    // Buffer import/release is driven by the backend state machine (importBuffers()
    // on reconnect, releaseBuffers() on fallback), not here. With nothing imported
    // there is no dmabuf to render into.
    if (!m_device || m_bufCount == 0) {
        return std::nullopt;
    }

    // The shared presentation layer owns buffer selection: it validated the slot
    // and stamped it with the session it belongs to. Using it here AND in the
    // backend's commit keeps "what we rendered into" and "what we submitted" the
    // same buffer; reading the transport selection here instead would allow the
    // two to disagree after a rotation.
    const int index = targetBufferIndex();
    if (index < 0) {
        // No usable target for this session yet: starting a frame would render
        // into a buffer the consumer did not ask for.
        return std::nullopt;
    }
    m_currentIndex = index;

    return OutputLayerBeginFrameInfo{
        .renderTarget = RenderTarget(m_fbos[m_currentIndex].get()),
        .repaint = m_accumDamage[m_currentIndex],
    };
}

int AnlandEglLayer::targetBufferIndex() const
{
    if (!m_backend || m_bufCount == 0) {
        return -1;
    }

    AnlandBackend *backend = m_backend->backend();
    if (!backend) {
        return -1;
    }

    anland_de_target_t target{};
    if (anland_de_backend_get_target(backend->presentBackend(), &target) != 0) {
        return -1;
    }

    // A target from another session belongs to a buffer set this layer has not
    // imported, so it must not be rendered into.
    if (target.generation != backend->sessionGeneration() ||
        target.index >= target.count ||
        target.index >= static_cast<uint32_t>(m_bufCount)) {
        return -1;
    }

    return static_cast<int>(target.index);
}

void AnlandEglLayer::scheduleBufferCatchUp(int index)
{
    if (index >= 0 && index < m_bufCount && !m_accumDamage[index].isEmpty()) {
        // Only this slot's old content needs painting. Do not add new damage
        // to the other rotation buffers when asking the compositor to run.
#ifdef ANLAND_KWIN_66
        // KWin 6.6 added per-layer repaint scheduling.
        scheduleRepaint(nullptr);
#else
        // Older KWin versions schedule the compositor through the render loop.
        m_output->renderLoop()->scheduleRepaint();
#endif
    }
}

#ifdef ANLAND_KWIN_66
bool AnlandEglLayer::doEndFrame(const Region &renderedDeviceRegion, const Region &damagedDeviceRegion, OutputFrame *frame)
#else
bool AnlandEglLayer::doEndFrame(const QRegion &renderedDeviceRegion, const QRegion &damagedDeviceRegion, OutputFrame *frame)
#endif
{
    glFlush(); // flush pending rendering commands into the dmabuf.
    // Every buffer owes this frame's damage; the buffer we just painted is now
    // fully up to date, so clear its debt.
    for (int i = 0; i < m_bufCount; i++) {
        m_accumDamage[i] = m_accumDamage[i] + damagedDeviceRegion;
    }
    if (m_currentIndex < m_bufCount) {
        #ifdef ANLAND_KWIN_66
        m_accumDamage[m_currentIndex] = Region();
#else
        m_accumDamage[m_currentIndex] = QRegion();
#endif
    }

    // Wait until the consumer publishes its next target before scheduling a
    // catch-up repaint. The current slot may be selected again, so scheduling
    // here could repaint it forever while a different slot still owes damage.
    // A target change is handled in dispatchSceneEvents().

    // Instead of CPU-blocking on glFinish, create a fence for the just-submitted
    // GPU work and hand it to the consumer. The consumer passes it to
    // ANativeWindow_queueBuffer, so SurfaceFlinger waits on it GPU-side before
    // scanout.
    //
    // The fence is stashed on the layer rather than pushed to the device: it now
    // travels as the frame's commit acquire fence, which the backend submits when
    // it presents (see AnlandBackend::presentFrame).
    EGLNativeFence fence{m_backend->eglDisplayObject()};
    // Native fence support or export can fail on this EGL stack. If no sync fd
    // was produced, finish GPU work before making the dmabuf visible to the
    // consumer instead of submitting an unsynchronized frame.
    if (!fence.isValid()) {
        glFinish();
    }
    setPendingFence(fence.takeFileDescriptor().take());
    return true;
}

void AnlandEglLayer::setPendingFence(int fd)
{
    // Closing the predecessor mirrors set_render_fence()'s ownership rule: a fence
    // that was never consumed by a commit must not leak.
    if (m_pendingFence >= 0) {
        close(m_pendingFence);
    }
    m_pendingFence = fd;
}

int AnlandEglLayer::takePendingFence()
{
    const int fd = m_pendingFence;
    m_pendingFence = -1;
    return fd;
}

DrmDevice *AnlandEglLayer::scanoutDevice() const
{
    return m_backend->drmDevice();
}

#ifdef ANLAND_KWIN_67
FormatModifierMap AnlandEglLayer::supportedDrmFormats() const
#else
QHash<uint32_t, QList<uint64_t>> AnlandEglLayer::supportedDrmFormats() const
#endif
{
    return {};
}

std::shared_ptr<GLTexture> AnlandEglLayer::texture() const
{
    return m_textures[m_currentIndex];
}

AnlandEglBackend::AnlandEglBackend(AnlandBackend *b)
#ifdef ANLAND_KWIN_66
    : EglBackend()
#else
    : AbstractEglBackend()
#endif
    , m_backend(b)
{
}

AnlandEglBackend::~AnlandEglBackend()
{
#ifdef ANLAND_KWIN_66
    // Outputs own their layers in 6.6. Destroy them while our GL context
    // remains valid, before cleanup() and before this backend is destroyed.
    for (BackendOutput *output : m_backend->outputs()) {
        static_cast<AnlandOutput *>(output)->setEglLayer(nullptr);
    }
#else
    m_outputs.clear();
#endif
    cleanup();
}

anland_device *AnlandEglBackend::device() const
{
    return m_backend->device();
}

DrmDevice *AnlandEglBackend::drmDevice() const
{
    return m_backend->drmDevice();
}

bool AnlandEglBackend::initializeEgl()
{
#ifdef ANLAND_KWIN_67
    if (!initClientExtensions() || !m_backend->renderDevice()) {
        return false;
    }
    setRenderDevice(m_backend->renderDevice());
    return true;
#else
    initClientExtensions();
    if (!m_backend->sceneEglDisplayObject()) {
        if (!hasClientExtension(QByteArrayLiteral("EGL_MESA_platform_surfaceless"))) {
            qCWarning(KWIN_ANLAND) << "Extension EGL_MESA_platform_surfaceless not available";
            return false;
        }
        m_backend->setEglDisplay(EglDisplay::create(eglGetPlatformDisplayEXT(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr)));
    }

    auto display = m_backend->sceneEglDisplayObject();
    if (!display) {
        return false;
    }
    setEglDisplay(display);
    return true;
#endif
}

#ifdef ANLAND_KWIN_67
bool AnlandEglBackend::init()
#else
void AnlandEglBackend::init()
#endif
{
    if (!initializeEgl()) {
#ifdef ANLAND_KWIN_67
        qCWarning(KWIN_ANLAND) << "Could not initialize egl";
        return false;
#else
        setFailed("Could not initialize egl");
        return;
#endif
    }
    if (!initRenderingContext()) {
#ifdef ANLAND_KWIN_67
        qCWarning(KWIN_ANLAND) << "Could not initialize rendering context";
        return false;
#else
        setFailed("Could not initialize rendering context");
        return;
#endif
    }

    if (checkGLError("Init")) {
#ifdef ANLAND_KWIN_67
        qCWarning(KWIN_ANLAND) << "Error during init of AnlandEglBackend";
        return false;
#else
        setFailed("Error during init of AnlandEglBackend");
        return;
#endif
    }

#ifndef ANLAND_KWIN_66
    setSupportsBufferAge(false);
#endif
    initWayland();

    const auto outputs = m_backend->outputs();
    for (auto *output : outputs) {
        addOutput(output);
    }

    connect(m_backend, &AnlandBackend::outputAdded, this, &AnlandEglBackend::addOutput);
#ifndef ANLAND_KWIN_66
    connect(m_backend, &AnlandBackend::outputRemoved, this, &AnlandEglBackend::removeOutput);
#endif
#ifdef ANLAND_KWIN_67
    return true;
#endif
}

bool AnlandEglBackend::initRenderingContext()
{
#ifdef ANLAND_KWIN_67
    return createContext() && openglContext()->makeCurrent();
#elif defined(ANLAND_KWIN_66)
    return createContext(EGL_NO_CONFIG_KHR) && openglContext()->makeCurrent();
#else
    return createContext(EGL_NO_CONFIG_KHR) && makeCurrent();
#endif
}

#ifdef ANLAND_KWIN_66
void AnlandEglBackend::addOutput(BackendOutput *output)
#else
void AnlandEglBackend::addOutput(Output *output)
#endif
{
#ifdef ANLAND_KWIN_66
    openglContext()->makeCurrent();
#else
    makeCurrent();
#endif
    auto *anlandOutput = static_cast<AnlandOutput *>(output);
    auto layer = std::make_unique<AnlandEglLayer>(anlandOutput, this);
    // Let AnlandBackend reach this layer through its output (output->eglLayer()).
#ifdef ANLAND_KWIN_66
    anlandOutput->setEglLayer(std::move(layer));
    // If the session was ready before renderer initialization, import it now.
    if (m_backend->consumerReadyForImport()) {
        if (!anlandOutput->eglLayer()->importBuffers(anland_device_fb_count(m_backend->device()))) {
            m_backend->handleBufferImportFailure();
        }
    }
#else
    anlandOutput->setEglLayer(layer.get());
    m_outputs[output] = std::move(layer);
#endif
}

#ifdef ANLAND_KWIN_66
void AnlandEglBackend::removeOutput(BackendOutput *output)
#else
void AnlandEglBackend::removeOutput(Output *output)
#endif
{
#ifdef ANLAND_KWIN_66
    openglContext()->makeCurrent();
#else
    makeCurrent();
#endif
    static_cast<AnlandOutput *>(output)->setEglLayer(nullptr);
#ifndef ANLAND_KWIN_66
    m_outputs.erase(output);
#endif
}

#ifndef ANLAND_KWIN_66
std::unique_ptr<SurfaceTexture> AnlandEglBackend::createSurfaceTextureWayland(SurfacePixmap *pixmap)
{
    return std::make_unique<BasicEGLSurfaceTextureWayland>(this, pixmap);
}

OutputLayer *AnlandEglBackend::primaryLayer(Output *output)
{
    auto it = m_outputs.find(output);
    if (it == m_outputs.end()) {
        return nullptr;
    }
    return it->second.get();
}

bool AnlandEglBackend::present(Output *output, const std::shared_ptr<OutputFrame> &frame)
{
    static_cast<AnlandOutput *>(output)->present(frame);
    return true;
}

std::pair<std::shared_ptr<KWin::GLTexture>, ColorDescription> AnlandEglBackend::textureForOutput(Output *output) const
{
    auto it = m_outputs.find(output);
    if (it == m_outputs.end()) {
        return {nullptr, ColorDescription::sRGB};
    }
    return {it->second->texture(), ColorDescription::sRGB};
}

#endif
#ifdef ANLAND_KWIN_66
QList<OutputLayer *> AnlandEglBackend::compatibleOutputLayers(BackendOutput *output)
{
    auto *layer = static_cast<AnlandOutput *>(output)->eglLayer();
    return layer ? QList<OutputLayer *>{layer} : QList<OutputLayer *>{};
}
#endif
} // namespace KWin

#include "moc_anland_egl_backend.cpp"
