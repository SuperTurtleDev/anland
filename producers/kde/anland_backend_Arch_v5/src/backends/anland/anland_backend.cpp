/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "anland_backend.h"
#include "anland_audio.h"
#include "anland_camera.h"
#include "anland_egl_backend.h"
#include "anland_input.h"
#include "anland_logging.h"
#include "anland_output.h"

#include "core/drmdevice.h"
#include "core/renderloop.h"
#include "inputmethod.h"
#include "main.h"
#include "core/renderdevice.h"
#include "opengl/egldisplay.h"

#include <epoxy/egl.h>

#ifndef EGL_PLATFORM_SURFACELESS_MESA
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#endif
#include "utils/filedescriptor.h"
#include "wayland/abstract_data_source.h"
#include "wayland/display.h"
#include "wayland/seat.h"
#include "wayland_server.h"
#include "window.h"
#include "workspace.h"

#include <QMimeData>
#include <QScopeGuard>
#include <QSocketNotifier>
#include <QThreadPool>
#include <QTimer>

#include <xf86drm.h>

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

namespace KWin
{

static const QString s_defaultSocketPath = QStringLiteral("/tmp/display_daemon.sock");
static const int s_reconnectIntervalMs = 200;

/*
 * KWin needs a DRM render device for the GL/EGL path (syncobj timelines, dmabuf
 * feedback). The anland backend renders surfaceless and imports the daemon's
 * dmabufs, so any usable render node works. Prefer an explicit override
 * ($ANLAND_DRM_DEVICE), then the first enumerated render node (as the virtual
 * backend does), then the standard render node — which on the kgsl/turnip stack
 * is the msm node exposed at /dev/dri/renderD128.
 */
static std::unique_ptr<DrmDevice> openRenderDevice()
{
    const QString override = qEnvironmentVariable("ANLAND_DRM_DEVICE");
    if (!override.isEmpty()) {
        if (auto dev = DrmDevice::open(override)) {
            return dev;
        }
        qCWarning(KWIN_ANLAND) << "ANLAND_DRM_DEVICE" << override << "could not be opened";
    }

    const int count = drmGetDevices2(0, nullptr, 0);
    if (count > 0) {
        QList<drmDevice *> devices(count);
        if (drmGetDevices2(0, devices.data(), devices.size()) >= 0) {
            auto guard = qScopeGuard([&] {
                drmFreeDevices(devices.data(), devices.size());
            });
            for (drmDevice *device : std::as_const(devices)) {
                if (device->available_nodes & (1 << DRM_NODE_RENDER)) {
                    if (auto dev = DrmDevice::open(QString::fromUtf8(device->nodes[DRM_NODE_RENDER]))) {
                        return dev;
                    }
                }
            }
        }
    }

    return DrmDevice::open(QStringLiteral("/dev/dri/renderD128"));
}

static void detachAudioBeforeConsumerRelease(void *)
{
    anland_audio_set_fd(-1);
}

AnlandBackend::AnlandBackend(const QString &socketPath, QObject *parent)
    : OutputBackend(parent)
    , m_socketPath(socketPath.isEmpty() ? s_defaultSocketPath : socketPath)
{
}

AnlandBackend::~AnlandBackend()
{
    teardownNotifiers();
    // Stop the audio engine before disconnect() releases the consumer audio fd.
    anland_audio_stop();
    // Stop the camera engine (it owns its own resource fds; closes them itself).
    anland_camera_stop();
    if (m_reconnectTimer) {
        m_reconnectTimer->stop();
    }
    // Drop EGL resources while the render device is still alive. The output
    // objects remain QObject children and are deleted by ~QObject afterwards.
    for (AnlandOutput *output : std::as_const(m_outputs)) {
        output->setEglLayer(std::unique_ptr<AnlandEglLayer>{});
    }
    m_outputs.clear();
    m_inputDevice.reset();
    if (m_display) {
        ::disconnect(m_display); // C producer API, not QObject::disconnect
        m_display = nullptr;
    }
    // m_eglDisplay (EglDisplay) tears down the EGLDisplay in its destructor; the
    // 5.27-era manual eglTerminate(sceneEglDisplay()) is no longer needed.
}

bool AnlandBackend::initialize()
{
    if (connect_to_deamon(&m_display, m_socketPath.toLocal8Bit().constData()) < 0) {
        qCWarning(KWIN_ANLAND) << "failed to connect to display daemon at" << m_socketPath;
        return false;
    }

    set_pre_release_callback(m_display, detachAudioBeforeConsumerRelease, nullptr);
    set_fallback_callback(m_display, &AnlandBackend::fallbackTrampoline, this);

    uint32_t w = 0, h = 0, fmt = 0, refresh = 0;
    get_screen_info(m_display, &w, &h, &fmt, &refresh);
    qCInfo(KWIN_ANLAND) << "connected to daemon at" << m_socketPath
                        << "screen" << w << "x" << h << "refresh" << refresh << "mHz";

    if (w == 0 || h == 0) {
        qCWarning(KWIN_ANLAND) << "daemon reported invalid screen size";
        return false;
    }

    // KWin dereferences renderBackend->drmDevice() during OpenGL compositor
    // setup; without a real device it segfaults. Open one up front, then wrap
    // it together with a surfaceless EGL display (anland imports the daemon's
    // dmabufs directly rather than allocating through the DRM device) into a
    // RenderDevice, mirroring how VirtualBackend owns its RenderDevice.
    auto drmDevice = openRenderDevice();
    if (!drmDevice) {
        qCWarning(KWIN_ANLAND) << "no usable DRM render device; cannot bring up OpenGL compositing";
        return false;
    }

    auto eglDisplay = EglDisplay::create(eglGetPlatformDisplayEXT(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr), drmDevice.get());
    if (!eglDisplay) {
        qCWarning(KWIN_ANLAND) << "failed to create surfaceless EGL display";
        return false;
    }

    m_renderDevice = std::make_unique<RenderDevice>(std::move(drmDevice), std::move(eglDisplay));

    m_inputDevice = std::make_unique<AnlandInputDevice>();

    auto *output = new AnlandOutput(this, QStringLiteral("anland-1"));
    output->setParent(this); // AnlandOutput no longer inherits Qt-parent ctor; wire it up manually so ~QObject cleans it up.
    output->init(QSize(w, h), static_cast<int>(refresh), 1.0);
    m_outputs.append(output);
    Q_EMIT outputAdded(output);
    output->updateEnabled(true);
    Q_EMIT outputsQueried();

    m_reconnectTimer = new QTimer(this);
    m_reconnectTimer->setInterval(s_reconnectIntervalMs);
    connect(m_reconnectTimer, &QTimer::timeout, this, &AnlandBackend::onReconnectTimer);

    // Bring up the audio engine up front: its PipeWire sink-monitor capture and
    // virtual mic Source live for the whole session, independent of the consumer,
    // so Linux apps never see the devices appear/disappear as the consumer comes and
    // goes. The socket fd is attached later (onReconnectTimer) and detached by
    // the transport before it closes a consumer fd. Audio is non-critical, so a
    // failure here is not fatal.
    const bool disableAudio = qEnvironmentVariableIsSet("ANLAND_DISABLE_AUDIO")
        && qEnvironmentVariableIntValue("ANLAND_DISABLE_AUDIO") != 0;
    if (!disableAudio && anland_audio_start() < 0) {
        qCWarning(KWIN_ANLAND) << "failed to start audio engine; continuing without audio";
    } else if (disableAudio) {
        qCInfo(KWIN_ANLAND) << "anland audio engine disabled by ANLAND_DISABLE_AUDIO";
    }

    // The camera engine is NOT started here: its PipeWire thread-loop is brought up
    // lazily on the first resource delivery (anland_camera_set_resources), i.e. only
    // once a consumer with the camera enabled actually hands over its fds. The backend
    // requests SERVICE_TYPE_CAMERA on every reconnect (onReconnectTimer); a consumer
    // that has the camera disabled never replies, so the engine never starts.

    // connect_to_deamon() only fetched screen info; the context is still in
    // fallback with no consumer fds or dmabufs. Enter fallback explicitly so the
    // reconnect timer starts and discovers the consumer via try_exit_fallback().
    enterFallback();

    if (SeatInterface *seat = waylandServer()->seat()) {
        connect(seat, &SeatInterface::selectionChanged, this, [this](AbstractDataSource *) {
            onClipboardChanged();
        });
    }

    // Workspace is created after backend initialization on KWin 6.7.
    if (workspace()) {
        setupSchedulingTracking();
    } else {
        connect(kwinApp(), &Application::workspaceCreated, this, &AnlandBackend::setupSchedulingTracking);
    }

    return true;
}

std::unique_ptr<EglBackend> AnlandBackend::createOpenGLBackend()
{
    return std::make_unique<AnlandEglBackend>(this);
}

std::unique_ptr<InputBackend> AnlandBackend::createInputBackend()
{
    return std::make_unique<AnlandInputBackend>(this);
}

QList<CompositingType> AnlandBackend::supportedCompositors() const
{
    return QList<CompositingType>{OpenGLCompositing};
}

QList<BackendOutput *> AnlandBackend::outputs() const
{
    QList<BackendOutput *> ret;
    ret.reserve(m_outputs.size());
    for (AnlandOutput *output : m_outputs) {
        ret.append(output);
    }
    return ret;
}

EglDisplay *AnlandBackend::sceneEglDisplayObject() const
{
    return m_renderDevice ? m_renderDevice->eglDisplay() : nullptr;
}

DrmDevice *AnlandBackend::drmDevice() const
{
    return m_renderDevice ? m_renderDevice->drmDevice() : nullptr;
}

bool AnlandBackend::notifyFramePresented()
{
    if (m_consumerReady && m_display) {
        m_consumerReady = false;
        if (trigger_refresh(m_display) < 0) {
            return false;
        }
        return true;
    }
    return false;
}

bool AnlandBackend::isConsumerConnected() const
{
    return m_display && !m_inFallback && !is_fallback(m_display);
}

bool AnlandBackend::importBuffers(AnlandEglLayer *layer)
{
    if (!layer) {
        return true;
    }
    if (!isConsumerConnected()) {
        return false;
    }

    const int count = get_buf_count(m_display);
    if (count <= 0 || count > MAX_BUFS) {
        qCWarning(KWIN_ANLAND) << "consumer reported invalid dmabuf count" << count;
    } else if (layer->importBuffers(count)) {
        return true;
    } else {
        qCWarning(KWIN_ANLAND) << "failed to import the consumer dmabuf set";
    }

    // Keep the C protocol state and KWin's fallback state in sync. Without
    // this, the reconnect timer would stop while the stale fds stayed live.
    force_fallback(m_display);
    m_inFallback = false;
    enterFallback();
    return false;
}

void AnlandBackend::setupNotifiers()
{
    teardownNotifiers();

    const int dataFd = get_data_fd(m_display);
    if (dataFd >= 0) {
        m_inputNotifier = new QSocketNotifier(dataFd, QSocketNotifier::Read, this);
        connect(m_inputNotifier, &QSocketNotifier::activated, this, [this]() {
            onInputReadable();
        });
    }

    const int bufReadyFd = get_buffer_ready_fd(m_display);
    if (bufReadyFd >= 0) {
        m_bufReadyNotifier = new QSocketNotifier(bufReadyFd, QSocketNotifier::Read, this);
        connect(m_bufReadyNotifier, &QSocketNotifier::activated, this, [this]() {
            onBufferReady();
        });
    }
}

void AnlandBackend::teardownNotifiers()
{
    if (m_inputNotifier) {
        m_inputNotifier->setEnabled(false);
        m_inputNotifier->deleteLater();
        m_inputNotifier = nullptr;
    }
    if (m_bufReadyNotifier) {
        m_bufReadyNotifier->setEnabled(false);
        m_bufReadyNotifier->deleteLater();
        m_bufReadyNotifier = nullptr;
    }
}

void AnlandBackend::onInputReadable()
{
    if (m_inFallback) return;

    InputEvent ev;
    while (poll_input_event(m_display, &ev, 0) > 0) {
        processInputEvent(ev);
        if (m_inFallback) break; // poll_input_event may have triggered fallback
    }
}

QPointF AnlandBackend::mapInputToLogical(const QPointF &devicePoint) const
{
    AnlandOutput *output = m_outputs[0];
    // The compositor maps logical -> device with transform().map(p, pixelSize()),
    // so the inverse maps device -> logical with the inverted transform bounded by
    // the device-space size (modeSize, the buffer's native landscape extent).
    // Then divide out the output scale to land in global logical coordinates.
    const QPointF logical = output->transform().inverted().map(devicePoint, QSizeF(output->modeSize()));
    return logical / output->scale();
}

QPointF AnlandBackend::mapInputDeltaToLogical(const QPointF &deviceDelta) const
{
    AnlandOutput *output = m_outputs[0];
    const OutputTransform transform = output->transform().inverted();
    const QSizeF bounds = QSizeF(output->modeSize());
    const QPointF mappedDelta = transform.map(deviceDelta, bounds) - transform.map(QPointF(0, 0), bounds);
    return mappedDelta / output->scale();
}

void AnlandBackend::processInputEvent(const InputEvent &ev)
{
    if (!m_inputDevice) {
        return;
    }

    switch (ev.type) {
    case INPUT_TYPE_POINTER_MOTION: {
        const QPointF pos = mapInputToLogical(QPointF(ev.pointer_motion.x, ev.pointer_motion.y));
        const QPointF delta = mapInputDeltaToLogical(QPointF(ev.pointer_motion.dx, ev.pointer_motion.dy));
        m_inputDevice->pointerMotion(pos, delta, delta);
        break;
    }
    case INPUT_TYPE_POINTER_BUTTON:
        m_inputDevice->pointerButton(ev.pointer_button.button, ev.pointer_button.pressed != 0);
        break;
    case INPUT_TYPE_POINTER_AXIS: {
        // protocol axis: 0 = vertical scroll, 1 = horizontal scroll (wayland order)
        const PointerAxis axis =
            ev.pointer_axis.axis == 0 ? PointerAxis::Vertical : PointerAxis::Horizontal;
        m_inputDevice->pointerAxis(axis, ev.pointer_axis.value, ev.pointer_axis.discrete * 120);
        break;
    }
    case INPUT_TYPE_KEY:
        m_inputDevice->keyboardKey(ev.key.keycode, ev.key.action == INPUT_ACTION_DOWN);
        break;
    case INPUT_TYPE_TOUCH: {
        const QPointF pos = mapInputToLogical(QPointF(ev.touch.x, ev.touch.y));
        switch (ev.touch.action) {
        case INPUT_ACTION_DOWN:
            m_inputDevice->touchDown(ev.touch.pointer_id, pos);
            break;
        case INPUT_ACTION_UP:
            m_inputDevice->touchUp(ev.touch.pointer_id);
            break;
        case INPUT_ACTION_MOVE:
            m_inputDevice->touchMotion(ev.touch.pointer_id, pos);
            break;
        default:
            break;
        }
        break;
    }
    case INPUT_TYPE_TOUCH_FRAME:
        m_inputDevice->touchFrame();
        break;
    case INPUT_TYPE_DISPLAY_REFRESH:
        // Not an input event: the consumer reports its live display refresh rate
        // (mHz) so we can repace the RenderLoop.
        if (!m_outputs.isEmpty()) {
            m_outputs[0]->setRefreshRate(static_cast<int>(ev.display.refresh_mhz));
        }
        break;
    case INPUT_TYPE_CLIPBOARD: {
        const uint32_t size = ev.clipboard.size;
        QByteArray text(static_cast<int>(size), Qt::Uninitialized);
        if (size == 0 || poll_input_event_extend_data(m_display, text.data(), size, 5000) == 1) {
            sendClipboardToKWin(text);
        }
        break;
    }
    case INPUT_TYPE_TEXT_INPUT: {
        const uint32_t size = ev.text_input.size;
        if (size == 0) {
            break;
        }
        QByteArray text(static_cast<int>(size), Qt::Uninitialized);
        if (poll_input_event_extend_data(m_display, text.data(), size, 5000) == 1) {
            sendTextInputToKWin(text);
        }
        break;
    }
    case INPUT_TYPE_RESOURCE: {
        // The consumer is handing back the fds for a service we requested. The fdnum
        // fds follow as a separate DATA_MSG_INPUT_EXTEND_FDS message; receive them now
        // before the next poll_input_event() and route them to the owning engine.
        const uint32_t service = ev.resource.type;
        const uint32_t fdnum = ev.resource.fdnum;
        constexpr int kMaxFds = 1 + 8; // ctrl + up to MAX_CAMERAS streams
        if (fdnum == 0 || fdnum > kMaxFds) {
            break;
        }
        int fds[kMaxFds];
        int got = 0;
        if (poll_input_event_extend_fds(m_display, fds, static_cast<int>(fdnum), &got, 5000) != 1
            || got < static_cast<int>(fdnum)) {
            for (int i = 0; i < got; i++) {
                ::close(fds[i]);
            }
            break;
        }
        if (service == SERVICE_TYPE_CAMERA) {
            // fds[0] = shared control socket, fds[1..] = per-camera stream sockets.
            anland_camera_set_resources(fds[0], &fds[1], got - 1);
        } else {
            for (int i = 0; i < got; i++) {
                ::close(fds[i]);
            }
        }
        break;
    }
    default:
        break;
    }
}

void AnlandBackend::onBufferReady()
{
    if (m_inFallback || m_outputs.isEmpty()) {
        return;
    }

    // The dmabuf set is already in hand (try_exit_fallback() received it when we
    // left fallback), so buffer-ready is purely a frame-completion signal: drain
    // the eventfd and pace the render cycle.
    const int fd = get_buffer_ready_fd(m_display);
    if (fd >= 0) {
        eventfd_t val;
        eventfd_read(fd, &val);
    }

    m_consumerReady = true;

    // Buffer-ready is our frame-completion signal: complete the in-flight frame
    // (if any) and schedule the next one, keeping the render cycle paced by the
    // consumer rather than a timer.
    if (!m_outputs.isEmpty()) {
        m_outputs[0]->onConsumerReady();
    }
}

void AnlandBackend::fallbackTrampoline(void *data)
{
    static_cast<AnlandBackend *>(data)->enterFallback();
}

void AnlandBackend::enterFallback()
{
    if (m_inFallback) {
        return;
    }
    qCWarning(KWIN_ANLAND) << "consumer disconnected, entering fallback";

    if (m_inputDevice) {
        m_inputDevice->touchCancel();
    }

    // A frame may be in flight awaiting a buffer-ready that will never come now;
    // fail it so the RenderLoop's frame accounting does not stall.
    if (!m_outputs.isEmpty()) {
        m_outputs[0]->stopRendering();
    }

    teardownNotifiers();

    // Renderer is stopped: drop the imported dmabuf set now that the producer's fds
    // are gone. The layer is null at startup (no GL backend attached yet).
    if (!m_outputs.isEmpty()) {
        if (AnlandEglLayer *layer = m_outputs[0]->eglLayer()) {
            layer->releaseBuffers();
        }
    }

    m_consumerReady = false;
    m_inFallback = true;

    // The transport detaches audio before closing its borrowed consumer fd. At
    // startup no consumer fd exists yet, so nothing needs to be detached here.

    // The consumer's camera fds are now dead: stop recording, detach from the
    // resource fds and let the virtual camera nodes emit blank frames.
    anland_camera_clear();

    if (m_reconnectTimer) {
        m_reconnectTimer->start();
    }
}

void AnlandBackend::onReconnectTimer()
{
    if (!m_inFallback) {
        m_reconnectTimer->stop();
        return;
    }

    if (try_exit_fallback(m_display) != 0) {
        return; // still down, keep retrying
    }

    AnlandEglLayer *layer = m_outputs.isEmpty() ? nullptr : m_outputs[0]->eglLayer();
    if (layer && !importBuffers(layer)) {
        enter_fallback(m_display);
        return;
    }

    qCInfo(KWIN_ANLAND) << "consumer reconnected";
    m_inFallback = false;
    m_consumerReady = false;
    m_reconnectTimer->stop();
    setupNotifiers();
    // Attach the fresh audio socket (a new socketpair was installed by pickup_fds).
    anland_audio_set_fd(get_audio_fd(m_display));
    // Render loop is resuming and the data channel is live: immediately request the
    // camera service. The consumer replies asynchronously with an INPUT_TYPE_RESOURCE
    // event whose fds onInputReadable() hands to the camera engine. If the consumer
    // has the camera disabled it simply never registers the service and never replies,
    // so this is a harmless no-op in that case.
    if (push_resources_request(m_display, SERVICE_TYPE_CAMERA, nullptr) < 0 || m_inFallback) {
        return;
    }
    // Selection changes may have happened while the consumer was away. Force a
    // fresh read even when the current source is the remote source we created.
    onClipboardChanged(true);
    if (m_inFallback) {
        return;
    }
    if (!m_outputs.isEmpty()) {
        m_outputs[0]->resumeRendering();
    }
    if (layer) {
        layer->addDeviceRepaint(Region::infinite());
    }

    // Re-assert the compositor's permanent subtree boost, then the focused
    // client's (the consumer regresses every boost on its own fallback).
    sendSchedulingEvent(getpid(), SCHEDULING_FLAG_SETTREE | SCHEDULING_FLAG_ON);
    updateActiveScheduling(true);
}

// ---------------------------------------------------------------------------
// Foreground scheduling: compositor + focused client
// ---------------------------------------------------------------------------

void AnlandBackend::setupSchedulingTracking()
{
    if (auto *ws = workspace()) {
        connect(ws, &Workspace::windowActivated, this, [this]() {
            updateActiveScheduling();
        });
        if (!m_inFallback) {
            sendSchedulingEvent(getpid(), SCHEDULING_FLAG_SETTREE | SCHEDULING_FLAG_ON);
        }
        updateActiveScheduling(true);
    }
}

void AnlandBackend::updateActiveScheduling(bool force)
{
    pid_t pid = 0;
    if (auto *ws = workspace()) {
        if (Window *window = ws->activeWindow()) {
            pid = window->pid();
        }
    }

    if (!force && pid == m_activeSchedulingPid) {
        return;
    }
    // The off carries SETTREE too: promotion moved the whole subtree in, so
    // restoration must move the whole subtree back out, including children
    // forked while the client was focused.
    if (m_activeSchedulingPid > 0) {
        sendSchedulingEvent(m_activeSchedulingPid, SCHEDULING_FLAG_SETTREE);
    }
    m_activeSchedulingPid = pid;
    if (pid > 0) {
        sendSchedulingEvent(pid, SCHEDULING_FLAG_SETTREE | SCHEDULING_FLAG_ON);
    }
}

void AnlandBackend::sendSchedulingEvent(pid_t pid, uint8_t flags)
{
    if (m_inFallback) {
        return;
    }

    const OutputEvent ev = {
        .type = OUTPUT_TYPE_SCHEDULING,
        .scheduling = {
            .pid = pid > 0 ? pid : 0,
            .flags = flags,
        },
    };
    push_output_event(m_display, &ev);
}

static QByteArray readDataFromFd(FileDescriptor fd)
{
    QByteArray buffer;
    pollfd pfd{};
    pfd.fd = fd.get();
    pfd.events = POLLIN;

    while (true) {
        const int ready = poll(&pfd, 1, 1000);
        if (ready < 0) {
            if (errno != EINTR) {
                return QByteArray();
            }
        } else if (ready == 0) {
            return QByteArray();
        } else {
            char chunk[4096];
            const ssize_t n = read(fd.get(), chunk, sizeof(chunk));
            if (n < 0) {
                return QByteArray();
            } else if (n == 0) {
                return buffer;
            } else {
                buffer.append(chunk, n);
            }
        }
    }
}

static void writeDataToFd(qint32 rawFd, const QByteArray &buffer)
{
    FileDescriptor fd(rawFd);
    size_t remaining = buffer.size();
    pollfd pfd{};
    pfd.fd = fd.get();
    pfd.events = POLLOUT;

    while (remaining > 0) {
        const int ready = poll(&pfd, 1, 5000);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            return;
        }
        if (ready == 0 || !(pfd.revents & POLLOUT)) {
            return;
        }

        const char *chunk = buffer.constData() + (buffer.size() - remaining);
        const ssize_t n = write(fd.get(), chunk, remaining);
        if (n <= 0) {
            return;
        }
        remaining -= n;
    }
}

static QByteArray requestClipboardText(AbstractDataSource *source)
{
    if (!source) {
        return QByteArray();
    }

    const QStringList mimeTypes = source->mimeTypes();
    QString mimeType;
    if (mimeTypes.contains(QStringLiteral("text/plain;charset=utf-8"))) {
        mimeType = QStringLiteral("text/plain;charset=utf-8");
    } else if (mimeTypes.contains(QStringLiteral("text/plain"))) {
        mimeType = QStringLiteral("text/plain");
    } else {
        return QByteArray();
    }

    int pipeFds[2];
    if (pipe2(pipeFds, O_CLOEXEC) != 0) {
        return QByteArray();
    }

    source->requestData(mimeType, FileDescriptor(pipeFds[1]));
    waylandServer()->display()->flush();
    return readDataFromFd(FileDescriptor(pipeFds[0]));
}

void AnlandBackend::onClipboardChanged(bool force)
{
    if (m_inFallback) {
        return;
    }

    SeatInterface *seat = waylandServer() ? waylandServer()->seat() : nullptr;
    if (!seat) {
        return;
    }

    AbstractDataSource *source = seat->selection();
    if (!force && source == m_clipboardSource.get()) {
        return;
    }

    const QByteArray text = requestClipboardText(source);
    if (!force && text == m_clipboardText) {
        return;
    }

    m_clipboardText = text;
    sendClipboardToConsumer(text);
}

void AnlandBackend::sendClipboardToConsumer(const QByteArray &text)
{
    if (m_inFallback) {
        return;
    }

    const uint32_t len = static_cast<uint32_t>(text.size());
    const OutputEvent ev = {
        .type = OUTPUT_TYPE_CLIPBOARD,
        .clipboard = { .size = len },
    };
    push_output_event_with_length(m_display, &ev, const_cast<char *>(text.constData()), len);
}

void AnlandBackend::sendClipboardToKWin(const QByteArray &text)
{
    if (text == m_clipboardText) {
        return;
    }

    m_clipboardText = text;

    if (!waylandServer()) {
        return;
    }

    SeatInterface *seat = waylandServer()->seat();
    if (!seat) {
        return;
    }

    if (text.isEmpty()) {
        seat->setSelection(nullptr, waylandServer()->display()->nextSerial());
        m_clipboardSource.reset();
        return;
    }

    class AnlandClipboardSource : public AbstractDataSource
    {
    public:
        explicit AnlandClipboardSource(std::unique_ptr<QMimeData> data, QObject *parent = nullptr)
            : AbstractDataSource(parent)
            , m_data(std::move(data))
        {
        }

        void requestData(const QString &mimeType, FileDescriptor fd) override
        {
            const QByteArray data = m_data->data(mimeType);
            const int rawFd = fd.take();
            QThreadPool::globalInstance()->start([data, rawFd]() {
                writeDataToFd(rawFd, data);
            });
        }

        void cancel() override
        {
        }

        QStringList mimeTypes() const override
        {
            return m_data->formats();
        }

    private:
        std::unique_ptr<QMimeData> m_data;
    };

    auto mimeData = std::make_unique<QMimeData>();
    mimeData->setData(QStringLiteral("text/plain;charset=utf-8"), text);
    mimeData->setData(QStringLiteral("text/plain"), text);

    auto oldSource = std::move(m_clipboardSource);
    m_clipboardSource = std::make_unique<AnlandClipboardSource>(std::move(mimeData));
    seat->setSelection(m_clipboardSource.get(), waylandServer()->display()->nextSerial());
    oldSource.reset();
}

void AnlandBackend::sendTextInputToKWin(const QByteArray &text)
{
    if (m_inFallback) {
        return;
    }

    const QString string = QString::fromUtf8(text);
    if (string.isEmpty()) {
        return;
    }

    if (InputMethod *inputMethod = kwinApp()->inputMethod()) {
        inputMethod->commitText(string);
    }
}

} // namespace KWin

#include "moc_anland_backend.cpp"
