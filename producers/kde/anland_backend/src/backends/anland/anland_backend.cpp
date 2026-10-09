/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "anland_backend.h"
#include "anland_device.h"
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
#include "opengl/egldisplay.h"
#ifdef ANLAND_KWIN_67
#include "core/renderdevice.h"
#endif
#include "utils/filedescriptor.h"
#include "wayland/abstract_data_source.h"
#include "wayland/display.h"
#include "wayland/seat.h"
#include "wayland_server.h"
#include "window.h"
#include "workspace.h"

#include <QElapsedTimer>
#include <QMimeData>
#include <QScopeGuard>
#include <QSocketNotifier>
#include <QThread>
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

/* Socket resolution: an explicit path (constructor argument, i.e. --anland or the
 * value main_wayland.cpp read from $ANLAND_SOCKET) wins; otherwise $ANLAND_SOCKET is
 * consulted here, and an empty result is handed to anland_device_open(), which probes
 * its well-known locations. A container that mounts the daemon at /run/display.sock
 * therefore needs no environment setup at all. */
static QString resolveSocketPath(const QString &explicitPath)
{
    if (!explicitPath.isEmpty()) {
        return explicitPath;
    }
    return qEnvironmentVariable("ANLAND_SOCKET");
}
static const int s_reconnectIntervalMs = 200;

/*
 * KWin needs a DRM render device for the GL/EGL path (syncobj timelines, dmabuf
 * feedback). The anland backend renders surfaceless and imports the daemon's
 * dmabufs, so any usable render node works. Prefer an explicit override
 * ($ANLAND_DRM_DEVICE), then the first enumerated render node (as the virtual
 * backend does), then the standard render node — which on the kgsl/turnip stack
 * is the msm node exposed at /dev/dri/renderD128.
 */
#ifdef ANLAND_KWIN_67
using AnlandRenderDevice = RenderDevice;
#else
using AnlandRenderDevice = DrmDevice;
#endif
static std::unique_ptr<AnlandRenderDevice> openRenderDevice()
{
    const QString override = qEnvironmentVariable("ANLAND_DRM_DEVICE");
    if (!override.isEmpty()) {
        if (auto dev = AnlandRenderDevice::open(override)) {
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
                    if (auto dev = AnlandRenderDevice::open(QString::fromUtf8(device->nodes[DRM_NODE_RENDER]))) {
                        return dev;
                    }
                }
            }
        }
    }

    return AnlandRenderDevice::open(QStringLiteral("/dev/dri/renderD128"));
}

static void detachAudioBeforeConsumerRelease(void *)
{
    anland_audio_set_fd(-1);
}

AnlandBackend::AnlandBackend(const QString &socketPath, QObject *parent)
    : OutputBackend(parent)
    , m_socketPath(resolveSocketPath(socketPath))
{
}

AnlandBackend::~AnlandBackend()
{
    teardownNotifiers();
    // Stop the audio engine before disconnect() closes the audio fd it borrows.
    anland_audio_stop();
    // Stop the camera engine (it owns its own resource fds; closes them itself).
    anland_camera_stop();
    if (m_reconnectTimer) {
        m_reconnectTimer->stop();
    }
    // m_outputs are QObject children of this backend; ~QObject deletes them.
    m_outputs.clear();
    m_inputDevice.reset();
    // The presentation facade owns the scene/device. DE code only borrows them.
    if (m_presentBackend) {
        anland_de_backend_destroy(m_presentBackend);
        m_presentBackend = nullptr;
        m_scene = nullptr;
        m_device = nullptr;
        m_outputLayer = 0;
    }
    // The EGL display is released by its owner (RenderDevice on 6.7, or
    // m_eglDisplay on older versions); no manual eglTerminate is needed.
}

bool AnlandBackend::initialize()
{
    anland_de_backend_config_t backendConfig{};
    // Presentation uses the shared scene/device lifecycle and existing transport.
    /* The endpoint must outlive the create() call: toLocal8Bit() returns a
     * temporary QByteArray, so binding .constData() directly would leave a
     * dangling pointer here. Keep the storage in this scope. */
    const QByteArray presentEndpoint = m_socketPath.toLocal8Bit();
    backendConfig.present.endpoint = presentEndpoint.constData();
    // No separate runtime presentation endpoint.
    backendConfig.name = "kwin";
    QElapsedTimer retryLogTimer;
    bool waitingForDaemon = false;
    while (!m_presentBackend) {
        m_presentBackend = anland_de_backend_create(&backendConfig);
        if (m_presentBackend) {
            break;
        }

        if (!waitingForDaemon) {
            qCWarning(KWIN_ANLAND) << "display daemon is not available at"
                                   << (m_socketPath.isEmpty() ? QStringLiteral("(auto discovery)")
                                                              : m_socketPath)
                                   << "; waiting for it to start";
            waitingForDaemon = true;
            retryLogTimer.start();
        } else if (retryLogTimer.elapsed() >= 5000) {
            qCWarning(KWIN_ANLAND) << "still waiting for display daemon at"
                                   << (m_socketPath.isEmpty() ? QStringLiteral("(auto discovery)")
                                                              : m_socketPath);
            retryLogTimer.restart();
        }
        QThread::msleep(s_reconnectIntervalMs);
    }
    m_scene = anland_de_backend_scene(m_presentBackend);
    m_device = anland_de_backend_device(m_presentBackend);
    // One layer represents this output. The legacy presentation flattens the
    // scene, so KWin keeps compositing all windows into the output buffer.
    const anland_layer_desc_t layerDesc{
        .parent_id = 0,
        .kind = ANLAND_LAYER_NORMAL,
        .name = "kwin-output",
        .opacity = 1.0f,
        .visible = true,
    };
    if (anland_de_backend_add_window_desc(m_presentBackend, 1, &layerDesc,
                                          &m_outputLayer) != 0) {
        qCWarning(KWIN_ANLAND) << "failed to create the output layer";
        anland_de_backend_destroy(m_presentBackend);
        m_presentBackend = nullptr;
        m_scene = nullptr;
        m_device = nullptr;
        return false;
    }

    anland_device_set_pre_release_cb(m_device, detachAudioBeforeConsumerRelease, nullptr);
    anland_device_set_fallback_cb(m_device, &AnlandBackend::fallbackTrampoline, this);

    anland_device_output_t out{};
    const int nout = anland_device_get_outputs(m_device, &out, 1);
    qCInfo(KWIN_ANLAND) << "connected to daemon at"
                        << QString::fromLocal8Bit(anland_device_socket_path(m_device))
                        << "outputs" << nout << "screen" << out.width << "x" << out.height
                        << "refresh" << out.refresh_mhz << "mHz";

    if (nout < 1 || out.width == 0 || out.height == 0) {
        qCWarning(KWIN_ANLAND) << "daemon reported invalid screen size";
        return false;
    }

    // KWin dereferences renderBackend->drmDevice() during OpenGL compositor
    // setup; without a real device it segfaults. Open one up front.
#ifdef ANLAND_KWIN_67
    m_renderDevice = openRenderDevice();
    if (!m_renderDevice) {
#else
    m_drmDevice = openRenderDevice();
    if (!m_drmDevice) {
#endif
        qCWarning(KWIN_ANLAND) << "no usable DRM render device; cannot bring up OpenGL compositing";
        return false;
    }

    m_inputDevice = std::make_unique<AnlandInputDevice>();

    auto *output = new AnlandOutput(this, QStringLiteral("anland-1"));
    output->init(QSize(out.width, out.height), static_cast<int>(out.refresh_mhz), 1.0);
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
    // goes. The socket fd is attached later (onReconnectTimer) and detached in
    // enterFallback(). Audio is non-critical, so a failure here is not fatal.
    //
    // ANLAND_DISABLE_AUDIO=1 skips the audio engine entirely: the anland-audio
    // PipeWire thread-loop (and the anland-speaker/anland-mic virtual devices) is
    // never created. Used to sidestep a busy-loop on this platform until the
    // underlying cause is fixed upstream. The backend simply reports no audio.
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
    // requests ANLAND_DEVICE_SERVICE_CAMERA on every reconnect (onReconnectTimer); a consumer
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

    // Pointer-lock tracking for ANLAND_DEVICE_VAR_CAPTURE_MOUSE. The Workspace is created
    // after the backend initializes (see ApplicationWayland::performStartup), so wire
    // the window-activation trigger when workspaceCreated fires; if the workspace is
    // already up (defensive), start immediately.
    if (workspace()) {
        setupMouseCaptureTracking();
        setupSchedulingTracking();
    } else {
        connect(kwinApp(), &Application::workspaceCreated, this, [this]() {
            setupMouseCaptureTracking();
            setupSchedulingTracking();
        });
    }

    return true;
}

#ifdef ANLAND_KWIN_66
std::unique_ptr<EglBackend> AnlandBackend::createOpenGLBackend()
#else
std::unique_ptr<OpenGLBackend> AnlandBackend::createOpenGLBackend()
#endif
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

#ifdef ANLAND_KWIN_66
QList<BackendOutput *> AnlandBackend::outputs() const
{
    QList<BackendOutput *> ret;
#else
Outputs AnlandBackend::outputs() const
{
    Outputs ret;
#endif
    ret.reserve(m_outputs.size());
    for (AnlandOutput *output : m_outputs) {
        ret.append(output);
    }
    return ret;
}

EglDisplay *AnlandBackend::sceneEglDisplayObject() const
{
#ifdef ANLAND_KWIN_67
    return m_renderDevice ? m_renderDevice->eglDisplay() : nullptr;
#else
    return m_eglDisplay.get();
#endif
}
#ifdef ANLAND_KWIN_67
DrmDevice *AnlandBackend::drmDevice() const
{
    return m_renderDevice ? m_renderDevice->drmDevice() : nullptr;
}
RenderDevice *AnlandBackend::renderDevice() const
{
    return m_renderDevice.get();
}
#else
void AnlandBackend::setEglDisplay(std::unique_ptr<EglDisplay> &&display)
{
    m_eglDisplay = std::move(display);
}
#endif

bool AnlandBackend::presentFrame(int fenceFd)
{
    if (m_inFallback || !m_scene || m_outputLayer == 0) {
        if (fenceFd >= 0) {
            close(fenceFd);
        }
        return false;
    }

    // The output layer covers the whole output: the legacy transport flattens the
    // scene, so the commit describes one full-output layer regardless of how many
    // windows KWin composited into it.
    anland_device_output_t out{};
    if (anland_device_get_outputs(m_device, &out, 1) != 1 ||
        out.width == 0 || out.height == 0) {
        if (fenceFd >= 0) {
            close(fenceFd);
        }
        return false;
    }

    anland_layer_state_t state;
    memset(&state, 0, sizeof(state));
    state.layer_id = m_outputLayer;
    // The render target comes from the shared layer, which validated the slot and
    // stamped it with the session it belongs to. Reading the legacy consumer slot
    // here would let a stale selection from a dead session be presented.
    anland_de_target_t target{};
    if (anland_de_backend_get_target(m_presentBackend, &target) != 0 ||
        target.generation != m_sessionGeneration ||
        target.index >= target.count) {
        qCWarning(KWIN_ANLAND) << "no usable render target for this session";
        if (fenceFd >= 0) {
            close(fenceFd);
        }
        return false;
    }
    state.buffer_id = static_cast<uint64_t>(target.index) + 1;
    state.destination.width = out.width;
    state.destination.height = out.height;
    state.opacity = 1.0f;
    state.visible = true;
    state.acquire_fence_fd = fenceFd;
    // The whole committed buffer is treated as damaged: KWin's own damage tracking
    // lives in the layer's per-buffer accumDamage, and the legacy transport does
    // not carry damage to the consumer.
    state.damage = nullptr;
    state.damage_count = 0;

    // SUBMITTED is not PRESENTED: this only accepts the transaction. Completion
    // arrives later through the scene's event queue (see dispatchSceneEvents),
    // which is what AnlandOutput::onConsumerReady() reacts to.
    if (anland_de_backend_commit(m_presentBackend, &state, 1, nullptr) != 0) {
        if (fenceFd >= 0) {
            close(fenceFd); // rejected: the commit did not take the fence
        }
        return false;
    }

    // The contract treats acquire_fence_fd as BORROWED and the adapter dups what it
    // needs for the flip, so this frame's fd is still ours. Release it here or every
    // presented frame leaks one descriptor.
    if (fenceFd >= 0) {
        close(fenceFd);
    }

    return anland_de_backend_present(m_presentBackend) == 0;
}

void AnlandBackend::handleBufferImportFailure()
{
    // A ready consumer without imported buffers cannot ever begin a frame.
    // Drop its session so the reconnect timer can retry the import.
    anland_de_backend_drop_session(m_presentBackend);
    enterFallback();
}

void AnlandBackend::dispatchSceneEvents()
{
    if (!m_scene) {
        return;
    }

    anland_scene_event_t events[ANLAND_SCENE_EVENT_QUEUE];
    size_t count = 0;
    if (anland_de_backend_dispatch(m_presentBackend,
                                   events,
                                   ANLAND_SCENE_EVENT_QUEUE,
                                   &count) != 0) {
        return;
    }

    for (size_t i = 0; i < count; i++) {
        switch (events[i].type) {
        case ANLAND_SCENE_EVENT_PRESENTED:
            // The frame reached the consumer: the RenderLoop frame may complete.
            if (!m_outputs.isEmpty()) {
                m_outputs[0]->onFramePresented();
            }
            break;
        case ANLAND_SCENE_EVENT_COMMIT_DROPPED:
            qCWarning(KWIN_ANLAND) << "frame commit dropped, reason" << int(events[i].u.dropped.reason);
            if (!m_outputs.isEmpty()) {
                m_outputs[0]->onFrameDropped();
            }
            break;
        case ANLAND_SCENE_EVENT_BUFFER_RELEASED:
            if (events[i].u.released.release_fence_fd >= 0) {
                close(events[i].u.released.release_fence_fd);
            }
            break;
        case ANLAND_SCENE_EVENT_OUTPUT_CHANGED:
            if (!m_outputs.isEmpty()) {
                m_outputs[0]->onOutputGeometryChanged(QSize(events[i].u.output.width,
                                                            events[i].u.output.height),
                                                      static_cast<int>(events[i].u.output.refresh_mhz));
            }
            break;
        case ANLAND_SCENE_EVENT_RENDER_TARGET_READY:
            // The shared layer published the buffer slot to draw into next, and
            // anland_de_backend_get_target() reports it from here on. Track it so
            // presentFrame() commits the same slot, and reject a target that
            // belongs to a session this backend is not serving.
            if (events[i].u.target_ready.generation != m_sessionGeneration) {
                qCWarning(KWIN_ANLAND) << "ignoring render target from stale session"
                                       << events[i].u.target_ready.generation;
                break;
            }
            if (events[i].u.target_ready.index >= events[i].u.target_ready.count) {
                qCWarning(KWIN_ANLAND) << "invalid render target index"
                                       << events[i].u.target_ready.index;
                enterFallback();
                break;
            }
            m_currentBuffer = int(events[i].u.target_ready.index);
            m_bufferCount = events[i].u.target_ready.count;
            // Wake only if this slot owes content from earlier frames. The
            // layer supplies its accumulated repaint in beginFrame(); do not
            // turn buffer catch-up into new full-screen scene damage.
            if (!m_outputs.isEmpty()) {
                if (AnlandEglLayer *layer = m_outputs[0]->eglLayer()) {
                    layer->scheduleBufferCatchUp(m_currentBuffer);
                }
            }
            break;
        }
    }
}

void AnlandBackend::setupNotifiers()
{
    teardownNotifiers();

    const int dataFd = anland_device_data_fd(m_device);
    if (dataFd >= 0) {
        m_inputNotifier = new QSocketNotifier(dataFd, QSocketNotifier::Read, this);
        connect(m_inputNotifier, &QSocketNotifier::activated, this, [this]() {
            onInputReadable();
        });
    }

    const int bufReadyFd = anland_device_buffer_ready_fd(m_device);
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

    anland_device_input_t ev;
    while (anland_device_poll_input(m_device, &ev, 0) > 0) {
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

void AnlandBackend::processInputEvent(const anland_device_input_t &ev)
{
    if (!m_inputDevice) {
        return;
    }

    switch (ev.type) {
    case ANLAND_DEVICE_IN_PTR_MOTION: {
        const QPointF pos = mapInputToLogical(QPointF(ev.pointer_motion.x, ev.pointer_motion.y));
        const QPointF delta = mapInputDeltaToLogical(QPointF(ev.pointer_motion.dx, ev.pointer_motion.dy));
        m_inputDevice->pointerMotion(pos, delta, delta);
        break;
    }
    case ANLAND_DEVICE_IN_PTR_BUTTON:
        m_inputDevice->pointerButton(ev.pointer_button.button, ev.pointer_button.pressed != 0);
        break;
    case ANLAND_DEVICE_IN_PTR_AXIS: {
        // protocol axis: 0 = vertical scroll, 1 = horizontal scroll (wayland order)
        const PointerAxis axis =
            ev.pointer_axis.axis == 0 ? PointerAxis::Vertical : PointerAxis::Horizontal;
        m_inputDevice->pointerAxis(axis, ev.pointer_axis.value, ev.pointer_axis.discrete * 120);
        break;
    }
    case ANLAND_DEVICE_IN_KEY:
        m_inputDevice->keyboardKey(ev.key.keycode, ev.key.action == ANLAND_DEVICE_ACTION_DOWN);
        break;
    case ANLAND_DEVICE_IN_TOUCH: {
        const QPointF pos = mapInputToLogical(QPointF(ev.touch.x, ev.touch.y));
        switch (ev.touch.action) {
        case ANLAND_DEVICE_ACTION_DOWN:
            m_inputDevice->touchDown(ev.touch.pointer_id, pos);
            break;
        case ANLAND_DEVICE_ACTION_UP:
            m_inputDevice->touchUp(ev.touch.pointer_id);
            break;
        case ANLAND_DEVICE_ACTION_MOVE:
            m_inputDevice->touchMotion(ev.touch.pointer_id, pos);
            break;
        default:
            break;
        }
        break;
    }
    case ANLAND_DEVICE_IN_TOUCH_FRAME:
        m_inputDevice->touchFrame();
        break;
    case ANLAND_DEVICE_IN_DISPLAY_REFRESH:
        // Not an input event: the consumer reports its live display refresh rate
        // (mHz) so we can repace the RenderLoop.
        m_outputs[0]->setRefreshRate(static_cast<int>(ev.display.refresh_mhz));
        break;
    case ANLAND_DEVICE_IN_CLIPBOARD: {
        const uint32_t size = ev.clipboard.size;
        QByteArray text(static_cast<int>(size), Qt::Uninitialized);
        if (size == 0 || anland_device_read_input(m_device, text.data(), size, 5000) == 1) {
            sendClipboardToKWin(text);
        }
        break;
    }
    case ANLAND_DEVICE_IN_TEXT_INPUT: {
        const uint32_t size = ev.text_input.size;
        if (size == 0) {
            break;
        }
        QByteArray text(static_cast<int>(size), Qt::Uninitialized);
        if (anland_device_read_input(m_device, text.data(), size, 5000) == 1) {
            sendTextInputToKWin(text);
        }
        break;
    }
    case ANLAND_DEVICE_IN_RESOURCE: {
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
        if (anland_device_read_fds(m_device, fds, static_cast<int>(fdnum), &got, 5000) != 1
            || got < static_cast<int>(fdnum)) {
            for (int i = 0; i < got; i++) {
                ::close(fds[i]);
            }
            break;
        }
        if (service == ANLAND_DEVICE_SERVICE_CAMERA) {
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
    if (m_inFallback) {
        return;
    }

    // The consumer consumed the frame we handed over and selected the next buffer.
    // pump() turns that acknowledgement into a PRESENTED event and publishes the
    // next render target; the QSocketNotifier fired for this same fd, so a zero
    // timeout is enough.
    if (anland_de_backend_pump(m_presentBackend, 0) != 0) {
        // The session proved unusable (corrupt buffer selection): the shared layer
        // already dropped it, so stop rendering until the reconnect timer brings a
        // fresh session up and the buffers are re-imported.
        enterFallback();
        return;
    }
    dispatchSceneEvents();
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
    m_outputs[0]->stopRendering();

    teardownNotifiers();

    // Renderer is stopped: drop the imported dmabuf set now that the producer's fds
    // are gone. The layer is null at startup (no GL backend attached yet).
    if (AnlandEglLayer *layer = m_outputs[0]->eglLayer()) {
        layer->releaseBuffers();
    }

    m_inFallback = true;

    // No target is usable until a fresh session publishes one: the slot index
    // belongs to the consumer that just went away.
    m_currentBuffer = -1;
    m_bufferCount = 0;

    // Any commit still in flight belongs to the consumer we just lost; drop it so
    // the DE gets COMMIT_DROPPED + BUFFER_RELEASED instead of waiting forever.
    // (The device-level fallback has already fired at this point.)
    if (m_scene) {
        dispatchSceneEvents();
    }

    // Audio is already detached: startup has no consumer socket, and a consumer
    // loss detaches the source before display_producer closes its borrowed fd.

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

    // A dead daemon invalidates the transport context, but not the public
    // device/scene/layer objects. Reopen the device in place so every owner keeps
    // the same stable identity and callbacks remain registered.
    if (m_scene && !anland_device_is_daemon_alive(m_device)) {
        qCWarning(KWIN_ANLAND) << "display daemon lost; reopening transport";

        teardownNotifiers();
        if (AnlandEglLayer *layer = m_outputs[0]->eglLayer()) {
            layer->releaseBuffers();
        }
        anland_audio_set_fd(-1);
        anland_camera_clear();
        anland_de_backend_drop_session(m_presentBackend);

        if (anland_de_backend_reopen(m_presentBackend,
                                      m_socketPath.toLocal8Bit().constData()) != 0) {
            qCWarning(KWIN_ANLAND) << "failed to reopen anland transport; will retry";
            return;
        }
        qCInfo(KWIN_ANLAND) << "transport reopened at"
                            << QString::fromLocal8Bit(anland_device_socket_path(m_device));
    }

    if (!m_presentBackend || !m_scene || !m_device || m_outputLayer == 0) {
        qCWarning(KWIN_ANLAND) << "presentation backend is unavailable; will retry";
        return;
    }

    // Old-session retirement can exceed one dispatch batch or need a retry.
    // Keep delivering those events while rendering is inhibited; reconnect
    // deliberately refuses to reuse buffer identities before they are drained.
    dispatchSceneEvents();
    if (anland_de_backend_reconnect(m_presentBackend) != 0) {
        return; // still down; the next tick drains any remaining retirement debt
    }

    // try_exit_fallback() received a fresh dmabuf set. Do not leave fallback
    // until that set is usable: otherwise KWin resumes without a render target
    // and has no later path that retries the import.
    AnlandEglLayer *layer = m_outputs[0]->eglLayer();
    if (layer && !layer->importBuffers(anland_device_fb_count(m_device))) {
        qCWarning(KWIN_ANLAND) << "failed to import consumer buffers; retrying session";
        anland_de_backend_drop_session(m_presentBackend);
        return;
    }

    qCInfo(KWIN_ANLAND) << "consumer reconnected";
    m_inFallback = false;
    m_reconnectTimer->stop();

    // This backend now serves a NEW consumer session. Capture its generation so
    // targets published for it are accepted and stale ones are refused.
    m_sessionGeneration = anland_scene_generation(m_scene);

    // The adapter dropped any commit that belonged to the previous consumer and
    // reported the new output geometry; surface those events now.
    dispatchSceneEvents();

    setupNotifiers();
    // Attach the fresh audio socket (a new socketpair was installed by pickup_fds).
    anland_audio_set_fd(anland_device_audio_fd(m_device));
    // Render loop is resuming and the data channel is live: immediately request the
    // camera service. The consumer replies asynchronously with an ANLAND_DEVICE_IN_RESOURCE
    // event whose fds onInputReadable() hands to the camera engine. If the consumer
    // has the camera disabled it simply never registers the service and never replies,
    // so this is a harmless no-op in that case.
    anland_device_request_resources(m_device, ANLAND_DEVICE_SERVICE_CAMERA, nullptr);
    m_outputs[0]->resumeRendering();
    if (layer) {
        #ifdef ANLAND_KWIN_66
                    layer->addDeviceRepaint(Region::infinite());
#else
                    layer->addRepaint(infiniteRegion());
#endif
    }

    // The consumer regresses every var to 0 on its own fallback, so the data
    // channel coming back up means we must re-assert the current pointer-capture
    // override (a game may still hold its pointer lock across the reconnect).
    sendConsumerVar(ANLAND_DEVICE_VAR_CAPTURE_MOUSE, m_captureMouseActive ? 1 : 0);

    // Re-assert the compositor's permanent subtree boost, then the focused
    // client's (the consumer regresses every boost on its own fallback).
    sendSchedulingEvent(getpid(), ANLAND_DEVICE_SCHED_FLAG_SETTREE | ANLAND_DEVICE_SCHED_FLAG_ON);
    updateActiveScheduling(true);
}

void AnlandBackend::setupMouseCaptureTracking()
{
    // Window activation is the cross-window trigger: a newly focused window may
    // carry (or release) its own pointer lock. Per-surface and per-lock signals
    // are wired lazily inside updateMouseCaptureVar() as the focused surface and
    // its lock identity change.
    if (auto *ws = workspace()) {
        connect(ws, &Workspace::windowActivated, this, &AnlandBackend::updateMouseCaptureVar);
        updateMouseCaptureVar();
    }
}

void AnlandBackend::updateMouseCaptureVar()
{
    // Re-derive the active pointer constraint from the focused window, rewiring
    // signals only when the surface / constraint identity changes. This mirrors
    // the triggers KWin itself uses in PointerInputRedirection::updatePointerConstraints()
    // (window activation + SurfaceInterface::pointerConstraintsChanged + each
    // constraint's own lockedChanged/confinedChanged), so we observe
    // zwp_locked_pointer_v1 and zwp_confined_pointer_v1 enable/disable without
    // touching core input code. The lock covers native pointer lock plus Xwayland's
    // hidden-cursor+confine emulation; the confine covers Xwayland visible-cursor
    // confine and native confinement. The QPointers auto-null on destruction and Qt
    // auto-clears connections to a destroyed QObject, so re-derivation stays safe.
    SurfaceInterface *surface = nullptr;
    if (auto *window = workspace()->activeWindow()) {
        surface = window->surface();
    }

    if (surface != m_captureMouseSurface.data()) {
        QObject::disconnect(m_captureMouseSurfaceConn);
        m_captureMouseSurfaceConn = QMetaObject::Connection();
        m_captureMouseSurface = surface;
        if (surface) {
            // Fires when a lock is installed on / removed from this surface, so we
            // pick up a freshly created zwp_locked_pointer_v1 (and its later removal).
            m_captureMouseSurfaceConn = connect(surface, &SurfaceInterface::pointerConstraintsChanged,
                                                this, &AnlandBackend::updateMouseCaptureVar);
        }
    }

    LockedPointerV1Interface *lock = surface ? surface->lockedPointer() : nullptr;
    if (lock != m_captureMouseLock.data()) {
        QObject::disconnect(m_captureMouseLockConn);
        m_captureMouseLockConn = QMetaObject::Connection();
        m_captureMouseLock = lock;
        if (lock) {
            // Fires when KWin activates/deactivates the lock (setLocked(true/false)).
            m_captureMouseLockConn = connect(lock, &LockedPointerV1Interface::lockedChanged,
                                             this, &AnlandBackend::updateMouseCaptureVar);
        }
    }

    // Xwayland exposes an X11 confine_to grab as a confined pointer (visible
    // cursor) or, when the cursor is hidden, upgrades it to a lock above; native
    // clients may also confine. Either an active lock or an active confinement
    // means the focused client has trapped the pointer, so both force capture.
    ConfinedPointerV1Interface *confined = surface ? surface->confinedPointer() : nullptr;
    if (confined != m_captureMouseConfined.data()) {
        QObject::disconnect(m_captureMouseConfinedConn);
        m_captureMouseConfinedConn = QMetaObject::Connection();
        m_captureMouseConfined = confined;
        if (confined) {
            // Fires when KWin activates/deactivates the confinement
            // (setConfined(true/false)).
            m_captureMouseConfinedConn = connect(confined, &ConfinedPointerV1Interface::confinedChanged,
                                                 this, &AnlandBackend::updateMouseCaptureVar);
        }
    }

    const bool captureActive = (lock && lock->isLocked())
                            || (confined && confined->isConfined());
    if (captureActive != m_captureMouseActive) {
        m_captureMouseActive = captureActive;
        sendConsumerVar(ANLAND_DEVICE_VAR_CAPTURE_MOUSE, captureActive ? 1 : 0);
    }
}

void AnlandBackend::sendConsumerVar(uint32_t var, uint32_t value)
{
    if (m_inFallback) {
        // No consumer connected. m_captureMouseActive still tracks the desired
        // state; the value is force-resent from onReconnectTimer() once the data
        // channel comes back up.
        return;
    }

    anland_device_set_consumer_var(m_device, var, value);
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
            sendSchedulingEvent(getpid(), ANLAND_DEVICE_SCHED_FLAG_SETTREE | ANLAND_DEVICE_SCHED_FLAG_ON);
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
        sendSchedulingEvent(m_activeSchedulingPid, ANLAND_DEVICE_SCHED_FLAG_SETTREE);
    }
    m_activeSchedulingPid = pid;
    if (pid > 0) {
        sendSchedulingEvent(pid, ANLAND_DEVICE_SCHED_FLAG_SETTREE | ANLAND_DEVICE_SCHED_FLAG_ON);
    }
}

void AnlandBackend::sendSchedulingEvent(pid_t pid, uint8_t flags)
{
    if (m_inFallback) {
        return;
    }

    anland_device_scheduling(m_device, pid, flags);
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

    #ifdef ANLAND_KWIN_66
    source->requestData(mimeType, FileDescriptor(pipeFds[1]));
#else
    source->requestData(mimeType, pipeFds[1]);
    close(pipeFds[1]);
#endif
    waylandServer()->display()->flush();
    return readDataFromFd(FileDescriptor(pipeFds[0]));
}

void AnlandBackend::onClipboardChanged()
{
    if (m_inFallback) {
        return;
    }

    AbstractDataSource *source = waylandServer()->seat()->selection();
    if (source == m_clipboardSource.get()) {
        return;
    }

    const QByteArray text = requestClipboardText(source);
    if (text == m_clipboardText) {
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

    anland_device_set_clipboard(m_device, text.constData(),
                                static_cast<size_t>(text.size()));
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

#ifdef ANLAND_KWIN_66
        void requestData(const QString &mimeType, FileDescriptor fd) override
        {
            const QByteArray data = m_data->data(mimeType);
            QThreadPool::globalInstance()->start([data, fd = std::move(fd)]() mutable {
                writeDataToFd(fd.take(), data);
            });
        }
#else
        void requestData(const QString &mimeType, qint32 fd) override
        {
            const QByteArray data = m_data->data(mimeType);
            QThreadPool::globalInstance()->start([data, fd]() {
                writeDataToFd(fd, data);
            });
        }
#endif

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
        // Route Android's committed UTF-8 text through KWin's normal input-method
        // path. It submits through text-input v1/v2/v3 when available, and falls
        // back to keysym -> virtual keyboard events when the client has no
        // text-input protocol enabled.
        inputMethod->commitText(string);
    }
}

} // namespace KWin

#include "moc_anland_backend.cpp"
