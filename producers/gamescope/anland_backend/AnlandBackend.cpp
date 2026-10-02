// Direct output translation over the shared producer facade; see validation matrix.
#include "backend.h"
#include "rendervulkan.hpp"
#include "wlserver.hpp"
#include "refresh_rate.h"
#include "steamcompmgr.hpp"
#include "session.h"
#include "input.h"
#include "ime.hpp"
#include "clipboard.hpp"
#include <mutex>
#include "anland_audio.h"
#include "anland_camera.h"
#include <chrono>
#include <set>
#include <ctime>
#include <poll.h>
#include <cerrno>
#include <cstring>
#include <unistd.h>
#include "wlr_begin.hpp"
#include <wlr/render/dmabuf.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/interfaces/wlr_keyboard.h>
#include <wlr/types/wlr_keyboard_group.h>
#include "wlr_end.hpp"

extern int g_nPreferredOutputWidth;
extern int g_nPreferredOutputHeight;

namespace gamescope
{
    static anland_gamescope_session *s_session = nullptr;
    static uint64_t s_pending = 0;
    class CAnlandConnector final : public CBaseBackendConnector, public INestedHints
    {
    public:
        CAnlandConnector()
        {
        }
        virtual ~CAnlandConnector()
        {
        }

        virtual gamescope::GamescopeScreenType GetScreenType() const override
        {
            return GAMESCOPE_SCREEN_TYPE_INTERNAL;
        }
        virtual GamescopePanelOrientation GetCurrentOrientation() const override
        {
            return GAMESCOPE_PANEL_ORIENTATION_0;
        }
        virtual bool SupportsHDR() const override
        {
            return false;
        }
        virtual bool IsHDRActive() const override
        {
            return false;
        }
        virtual const BackendConnectorHDRInfo &GetHDRInfo() const override
        {
            return m_HDRInfo;
        }
		virtual bool IsVRRActive() const override
		{
			return false;
		}
        virtual std::span<const BackendMode> GetModes() const override
        {
            return std::span<const BackendMode>{};
        }

        virtual bool SupportsVRR() const override
        {
            return false;
        }

        virtual std::span<const uint8_t> GetRawEDID() const override
        {
            return std::span<const uint8_t>{};
        }
        virtual std::span<const uint32_t> GetValidDynamicRefreshRates() const override
        {
            return std::span<const uint32_t>{};
        }

        virtual void GetNativeColorimetry(
            bool bHDR10,
            displaycolorimetry_t *displayColorimetry, EOTF *displayEOTF,
            displaycolorimetry_t *outputEncodingColorimetry, EOTF *outputEncodingEOTF ) const override
        {
			*displayColorimetry = displaycolorimetry_709;
			*displayEOTF = EOTF_Gamma22;
			*outputEncodingColorimetry = displaycolorimetry_709;
			*outputEncodingEOTF = EOTF_Gamma22;
        }

        virtual const char *GetName() const override
        {
            return "Anland";
        }
        virtual const char *GetMake() const override
        {
            return "Gamescope";
        }
        virtual const char *GetModel() const override
        {
            return "Virtual Display";
        }

		virtual int Present( const FrameInfo_t *pFrameInfo, bool bAsync ) override
		{
            if (!s_session || s_pending)
                return -EAGAIN;
            anland_gamescope_target target{};
            target.buffer.fd = -1;
            if (anland_gamescope_session_target(s_session, &target) != 0)
                return -EAGAIN;
            // Device format is the legacy ABGR8888 convention, not a DRM fourcc.
            if (target.buffer.format != 1) {
                close(target.buffer.fd);
                return -EINVAL;
            }
            wlr_dmabuf_attributes dma{};
            dma.width = target.buffer.width;
            dma.height = target.buffer.height;
            dma.format = DRM_FORMAT_ABGR8888;
            dma.modifier = target.buffer.modifier;
            dma.n_planes = 1;
            dma.fd[0] = target.buffer.fd;
            dma.stride[0] = target.buffer.stride;
            dma.offset[0] = target.buffer.offset;
            OwningRc<CVulkanTexture> image = new CVulkanTexture();
            CVulkanTexture::createFlags flags{};
            flags.bStorage = true;
            flags.bOutputImage = true;
            bool imported = image->BInit(dma.width, dma.height, 1, dma.format, flags, &dma);
            close(target.buffer.fd);
            target.buffer.fd = -1;
            if (!imported) {
                fprintf(stderr, "Anland: consumer STORAGE dmabuf import failed\n");
                return -EINVAL;
            }
            if (anland_gamescope_session_target_current(s_session, &target) != 0)
                return -EAGAIN;
            auto sequence = vulkan_composite(const_cast<FrameInfo_t *>(pFrameInfo), nullptr,
                                             false, image, false);
            if (!sequence)
                return -EINVAL;
            // Synchronous baseline: -1 is not an exported fence, GPU is finished.
            vulkan_wait(*sequence, true);
            uint64_t commit = 0;
            int result = anland_gamescope_session_submit(s_session, &target, -1, &commit);
            if (commit) {
                s_pending = commit;
                ++m_PresentFeedback.m_uQueuedPresents;
                if (m_PresentFeedback.m_uQueuedPresents == 1)
                    fprintf(stderr, "Anland: first GPU-complete frame submitted, commit=%llu, result=%d\n",
                            (unsigned long long)commit, result);
            }
            return result == 0 ? 0 : -EIO;
		}

        INestedHints *GetNestedHints() override { return this; }
        bool ShouldPaintCursor() override { return true; } // No host cursor plane.
        void SetCursorImage(std::shared_ptr<INestedHints::CursorInfo>) override {}
        void SetRelativeMouseMode(bool) override {}
        void SetVisible(bool) override {}
        void SetTitle(std::shared_ptr<std::string>) override {}
        void SetIcon(std::shared_ptr<std::vector<uint32_t>>) override {}
        void SetSelection(std::shared_ptr<std::string> text, GamescopeSelection selection) override {
            if (!text) return;
            if (selection == GAMESCOPE_SELECTION_CLIPBOARD) {
                wlserver_lock();
                anland_clipboard_set(m_Clipboard, text->data(), text->size());
                wlserver_unlock();
                anland_clipboard_export(m_Clipboard, *text);
            }
            // Preserve Gamescope's cross-Xwayland selection fan-out.
            gamescope_set_selection(*text, selection);
        }
        void SetClipboard(AnlandClipboard *clipboard) { m_Clipboard = clipboard; }
    private:
        AnlandClipboard *m_Clipboard = nullptr;
        BackendConnectorHDRInfo m_HDRInfo{};
    };

	class CAnlandBackend final : public CBaseBackend
	{
	public:
		CAnlandBackend()
		{
		}

        virtual ~CAnlandBackend()
        {
            // XWM destroys the backend while the Wayland loop is still running.
            // Quiesce callbacks under the same lock used by event dispatch.
            wlserver_lock();
            if (m_Wakeup) wl_event_source_remove(m_Wakeup);
            m_Wakeup = nullptr;
            m_Connector.SetClipboard(nullptr);
            anland_clipboard_stop(m_Clipboard);
            m_Clipboard = nullptr;
            if (m_Ime) destroy_local_ime(m_Ime);
            m_Ime = nullptr;
            wlserver_unlock(false);
            anland_camera_stop();
            anland_audio_stop();
            if (s_session) {
                auto *device = anland_gamescope_session_device(s_session);
                anland_device_set_consumer_var(device, ANLAND_DEVICE_VAR_CAPTURE_MOUSE, 0);
                anland_device_scheduling(device, getpid(), ANLAND_DEVICE_SCHED_FLAG_SETTREE);
            }
            anland_gamescope_input_reset(&m_Input);
            anland_gamescope_session_close(s_session);
            s_session = nullptr;
            s_pending = 0;
        }

		virtual bool Init() override
		{
            s_session = anland_gamescope_session_open(getenv("ANLAND_SOCKET"));
            // A cold daemon may have no screen_info until its first consumer.
            // Public session open can therefore time out too, not just reconnect.
            // Bootstrap like the headless backend; never fabricate a live target.
            // PollState retries the public API while IsPaused keeps Present idle.
            anland_device_output_t output{};
            auto *device = anland_gamescope_session_device(s_session);
            bool cached = device &&
                anland_device_get_outputs(device, &output, 1) == 1 &&
                output.width && output.height;
            if (!cached) {
                output.height = g_nPreferredOutputHeight > 0 ? g_nPreferredOutputHeight : 720;
                output.width = g_nPreferredOutputWidth > 0 ? g_nPreferredOutputWidth : output.height * 16 / 9;
            }
            fprintf(stderr, "Anland: fallback waiting for consumer, %s output %ux%u @ %u mHz\n",
                    cached ? "cached" : "bootstrap", output.width, output.height, output.refresh_mhz);
            g_nOutputWidth = output.width;
            g_nOutputHeight = output.height;
            // Vblank is armed before DISPLAY_REFRESH input can be dispatched.
            g_nOutputRefresh = output.refresh_mhz ? output.refresh_mhz : 60000;

			if ( !vulkan_init( vulkan_get_instance(), VK_NULL_HANDLE ) )
			{
				return false;
			}

			if ( !wlsession_init() )
			{
				fprintf( stderr, "Failed to initialize Wayland session\n" );
				return false;
			}

			return true;
		}

        virtual bool PostInit() override
        {
            const char *disable = getenv("ANLAND_DISABLE_AUDIO");
            if (!(disable && strcmp(disable, "1") == 0)) {
                m_AudioStarted = anland_audio_start() == 0;
                if (!m_AudioStarted)
                    fprintf(stderr, "Anland: public audio engine unavailable\n");
            }
            const char *disableCamera = getenv("ANLAND_DISABLE_CAMERA");
            if (!(disableCamera && strcmp(disableCamera, "1") == 0))
                m_CameraStarted = anland_camera_start() == 0;
            wlserver_lock();
            m_Ime = create_local_ime();
            m_Clipboard = anland_clipboard_start();
            m_Connector.SetClipboard(m_Clipboard);
            wlserver_unlock();
            m_Input.text = &Text;
            m_Input.text_userdata = this;
            m_Input.resources = &Resources;
            m_Input.resources_userdata = this;
            AttachSession();
            // Native Wayland timer wakes the existing compositor loop, including
            // when no client is repainting or the consumer has disappeared.
            wlserver_lock();
            m_Wakeup = wl_event_loop_add_timer(wlserver.event_loop, &Wakeup, this);
            if (m_Wakeup) wl_event_source_timer_update(m_Wakeup, 16);
            wlserver_unlock();
            return m_Wakeup != nullptr && m_Clipboard != nullptr && m_Ime != nullptr;
        }

        virtual std::span<const char *const> GetInstanceExtensions() const override
		{
			return std::span<const char *const>{};
		}
        virtual std::span<const char *const> GetDeviceExtensions( VkPhysicalDevice pVkPhysicalDevice ) const override
		{
			return std::span<const char *const>{};
		}
        virtual VkImageLayout GetPresentLayout() const override
		{
			return VK_IMAGE_LAYOUT_GENERAL;
		}
		virtual void GetPreferredOutputFormat( uint32_t *pPrimaryPlaneFormat, uint32_t *pOverlayPlaneFormat ) const override
		{
			*pPrimaryPlaneFormat = DRM_FORMAT_ABGR8888;
			*pOverlayPlaneFormat = VulkanFormatToDRM( VK_FORMAT_B8G8R8A8_UNORM );
		}
		virtual bool ValidPhysicalDevice( VkPhysicalDevice pVkPhysicalDevice ) const override
		{
			return true;
		}

		virtual void DirtyState( bool bForce, bool bForceModeset ) override
		{
		}

        virtual bool PollState() override
        {
            std::lock_guard<std::mutex> deviceGuard(m_DeviceMutex);
            if (!s_session) {
                auto now = std::chrono::steady_clock::now();
                if (now < m_NextReconnect)
                    return false;
                m_NextReconnect = now + std::chrono::milliseconds(200);
                s_session = anland_gamescope_session_open(getenv("ANLAND_SOCKET"));
                if (!s_session)
                    return false;
                fprintf(stderr, "Anland: public session opened, waiting for consumer\n");
            }
            auto *device = anland_gamescope_session_device(s_session);
            bool dirty = false;
            if (!anland_device_is_connected(device)) {
                if (m_WasConnected) {
                    ReleaseInput();
                    anland_audio_set_fd(-1);
                    anland_camera_clear();
                    anland_gamescope_input_reset(&m_Input);
                    anland_gamescope_session_drop(s_session);
                    dirty |= DrainEvents();
                    m_WasConnected = false;
                }
                auto now = std::chrono::steady_clock::now();
                if (now < m_NextReconnect)
                    return dirty;
                m_NextReconnect = now + std::chrono::milliseconds(200);
                if (!anland_device_is_daemon_alive(device) &&
                    anland_gamescope_session_reopen(s_session, getenv("ANLAND_SOCKET")) != 0)
                    return dirty;
                if (anland_gamescope_session_reconnect(s_session) != 0)
                    return dirty;
                AttachSession();
                dirty = true;
                fprintf(stderr, "Anland: session reconnected\n");
            }
            anland_gamescope_session_pump(s_session, 0);
            dirty |= DrainEvents();
            int rc = anland_gamescope_input_pump(&m_Input, device, g_nOutputWidth,
                g_nOutputHeight, &InputSink, this);
            if (rc < 0) {
                ReleaseInput();
                anland_gamescope_input_reset(&m_Input);
                anland_gamescope_session_drop(s_session);
                dirty |= DrainEvents();
            }
            wlserver_lock();
            bool capture = wlserver.GetCursorConstraint() != nullptr;
            wlserver_unlock();
            if (capture != m_Capture) {
                m_Capture = capture;
                anland_device_set_consumer_var(device, ANLAND_DEVICE_VAR_CAPTURE_MOUSE, capture);
            }
            if (anland_device_is_connected(device)) {
                if (auto text = anland_clipboard_take(m_Clipboard)) {
                    gamescope_set_selection(*text, GAMESCOPE_SELECTION_CLIPBOARD);
                    anland_device_set_clipboard(device, text->data(), text->size());
                }
            }
            return dirty || rc > 0;
        }

        bool DrainEvents()
        {
            anland_scene_event_t events[ANLAND_SCENE_EVENT_QUEUE];
            size_t count = 0;
            bool dirty = false;
            if (anland_gamescope_session_dispatch(s_session, events,
                    ANLAND_SCENE_EVENT_QUEUE, &count) != 0)
                return false;
            for (size_t i = 0; i < count; ++i) {
                auto &event = events[i];
                if (event.type == ANLAND_SCENE_EVENT_PRESENTED ||
                    event.type == ANLAND_SCENE_EVENT_COMMIT_DROPPED) {
                    if (s_pending && event.commit_id == s_pending) {
                        ++m_Connector.PresentationFeedback().m_uCompletedPresents;
                        if (m_Connector.PresentationFeedback().m_uCompletedPresents == 1)
                            fprintf(stderr, "Anland: first commit completed, commit=%llu, event=%d\n",
                                    (unsigned long long)event.commit_id, (int)event.type);
                        s_pending = 0;
                        dirty = true;
                    }
                }
                if (event.type == ANLAND_SCENE_EVENT_BUFFER_RELEASED &&
                    event.u.released.release_fence_fd >= 0)
                    close(event.u.released.release_fence_fd);
                if (event.type == ANLAND_SCENE_EVENT_OUTPUT_CHANGED) {
                    anland_device_output_t output{};
                    if (anland_gamescope_session_output(s_session, &output) == 0 &&
                        output.width && output.height) {
                        g_nOutputWidth = output.width;
                        g_nOutputHeight = output.height;
                        g_nOutputRefresh = output.refresh_mhz ? output.refresh_mhz : 60000;
                    }
                }
                dirty |= event.type == ANLAND_SCENE_EVENT_RENDER_TARGET_READY ||
                         event.type == ANLAND_SCENE_EVENT_OUTPUT_CHANGED;
            }
            return dirty;
        }
        bool RequiresDrmPrimaryNode() const override { return false; }
        const char *GetRenderNodeOverride() const override {
            const char *path = getenv("ANLAND_DRM_DEVICE");
            return path && *path ? path : nullptr;
        }

		virtual std::shared_ptr<BackendBlob> CreateBackendBlob( const std::type_info &type, std::span<const uint8_t> data ) override
		{
			return std::make_shared<BackendBlob>( data );
		}

		virtual OwningRc<IBackendFb> ImportDmabufToBackend( wlr_dmabuf_attributes *pDmaBuf ) override
		{
			return new CBaseBackendFb();
		}

		virtual bool UsesModifiers() const override
		{
			return false;
		}
		virtual std::span<const uint64_t> GetSupportedModifiers( uint32_t uDrmFormat ) const override
		{
			return std::span<const uint64_t>{};
		}

		virtual IBackendConnector *GetCurrentConnector() override
		{
			return &m_Connector;
		}
		virtual IBackendConnector *GetConnector( GamescopeScreenType eScreenType ) override
		{
			if ( eScreenType == GAMESCOPE_SCREEN_TYPE_INTERNAL )
				return &m_Connector;

			return nullptr;
		}

		virtual bool SupportsPlaneHardwareCursor() const override
		{
			return false;
		}

		virtual bool SupportsTearing() const override
		{
			return false;
		}

		virtual bool UsesVulkanSwapchain() const override
		{
			return false;
		}

        virtual bool IsSessionBased() const override
		{
			return false;
		}

		virtual bool SupportsExplicitSync() const override
		{
			return true;
		}

        virtual bool IsPaused() const override
        {
            anland_gamescope_target target{};
            target.buffer.fd = -1;
            if (!s_session || s_pending ||
                anland_gamescope_session_target(s_session, &target) != 0)
                return true;
            close(target.buffer.fd);
            return false;
        }

		virtual bool IsVisible() const override
		{
			return true;
		}

		virtual glm::uvec2 CursorSurfaceSize( glm::uvec2 uvecSize ) const override
		{
			return uvecSize;
		}

		virtual bool HackTemporarySetDynamicRefresh( int nRefresh ) override
		{
			return false;
		}

		virtual void HackUpdatePatchedEdid() override
		{
		}

	protected:

		virtual void OnBackendBlobDestroyed( BackendBlob *pBlob ) override
		{
		}

	private:

        static void InputSink(void *userdata, const anland_gamescope_input_event *ev)
        {
            auto *self = static_cast<CAnlandBackend *>(userdata);
            timespec now{};
            clock_gettime(CLOCK_MONOTONIC, &now);
            uint32_t time = uint32_t(uint64_t(now.tv_sec) * 1000 + now.tv_nsec / 1000000);
            if (ev->kind == AG_REFRESH) {
                g_nOutputRefresh = ev->code;
                return;
            }
            wlserver_lock();
            switch (ev->kind) {
            case AG_KEY:
                if (ev->pressed) self->m_Keys.insert(ev->code); else self->m_Keys.erase(ev->code);
                // virtual_keyboard_device is registered with keyboard_group:
                // notify_key updates pressed keys/XKB and forwards exactly once.
                {
                    wlr_keyboard_key_event key{};
                    key.time_msec = time; key.keycode = ev->code; key.update_state = true;
                    key.state = ev->pressed ? WL_KEYBOARD_KEY_STATE_PRESSED : WL_KEYBOARD_KEY_STATE_RELEASED;
                    wlr_keyboard_notify_key(wlserver.wlr.virtual_keyboard_device, &key);
                }
                break;
            case AG_MOTION:
                if (wlserver.GetCursorConstraint()) {
                    wlserver_mousemotion(ev->x * focusedWindowScaleX,
                                         ev->y * focusedWindowScaleY, time);
                } else {
                    wlserver_mousewarp((ev->absolute_x + focusedWindowOffsetX) * focusedWindowScaleX,
                                       (ev->absolute_y + focusedWindowOffsetY) * focusedWindowScaleY,
                                       time, false);
                }
                break;
            case AG_BUTTON:
                if (ev->pressed) self->m_Buttons.insert(ev->code); else self->m_Buttons.erase(ev->code);
                wlserver_mousebutton(ev->code, ev->pressed, time); break;
            case AG_AXIS:
                wlr_seat_pointer_notify_axis(wlserver.wlr.seat, time,
                    ev->code == 0 ? WL_POINTER_AXIS_VERTICAL_SCROLL : WL_POINTER_AXIS_HORIZONTAL_SCROLL,
                    ev->x, ev->discrete * WLR_POINTER_AXIS_DISCRETE_STEP,
                    WL_POINTER_AXIS_SOURCE_WHEEL, WL_POINTER_AXIS_RELATIVE_DIRECTION_IDENTICAL);
                wlr_seat_pointer_notify_frame(wlserver.wlr.seat);
                break;
            case AG_TOUCH_DOWN:
                self->m_Touches.insert(ev->code);
                wlserver_touchdown(ev->x, ev->y, ev->code, time, &self->m_Connector); break;
            case AG_TOUCH_MOVE:
                wlserver_touchmotion(ev->x, ev->y, ev->code, time, false, &self->m_Connector); break;
            case AG_TOUCH_UP: self->m_Touches.erase(ev->code); wlserver_touchup(ev->code, time); break;
            case AG_TOUCH_FRAME: wlr_seat_touch_notify_frame(wlserver.wlr.seat); break;
            case AG_RESOURCE_INVALID:
                if (ev->code == ANLAND_DEVICE_SERVICE_CAMERA) anland_camera_clear();
                break;
            default: break;
            }
            wlserver_unlock();
        }
        void AttachSession()
        {
            if (!s_session)
                return;
            auto *device = anland_gamescope_session_device(s_session);
            m_WasConnected = anland_device_is_connected(device);
            if (m_AudioStarted) anland_audio_set_fd(anland_device_audio_fd(device));
            anland_device_scheduling(device, getpid(),
                ANLAND_DEVICE_SCHED_FLAG_SETTREE | ANLAND_DEVICE_SCHED_FLAG_ON);
            anland_device_set_consumer_var(device, ANLAND_DEVICE_VAR_CAPTURE_MOUSE, m_Capture);
            if (m_CameraStarted)
                anland_device_request_resources(device, ANLAND_DEVICE_SERVICE_CAMERA, nullptr);
        }
        void ReleaseInput()
        {
            timespec now{};
            clock_gettime(CLOCK_MONOTONIC, &now);
            uint32_t time = uint32_t(uint64_t(now.tv_sec) * 1000 + now.tv_nsec / 1000000);
            wlserver_lock();
            for (auto code : m_Keys) {
                wlr_keyboard_key_event key{};
                key.time_msec = time; key.keycode = code; key.update_state = true;
                key.state = WL_KEYBOARD_KEY_STATE_RELEASED;
                wlr_keyboard_notify_key(wlserver.wlr.virtual_keyboard_device, &key);
            }
            for (auto button : m_Buttons) wlserver_mousebutton(button, false, time);
            for (auto touch : m_Touches) wlserver_touchup(touch, time);
            wlserver_unlock();
            m_Keys.clear(); m_Buttons.clear(); m_Touches.clear();
        }
        static int Wakeup(void *userdata)
        {
            auto *self = static_cast<CAnlandBackend *>(userdata);
            // Wake only for device work. Blindly nudging every 16ms disturbs the
            // upstream vblank/repaint scheduling and must not drive rendering.
            // Wayland dispatch already owns the seat lock: never block waiting
            // for PollState, which may be waiting to acquire that same lock.
            std::unique_lock<std::mutex> deviceGuard(self->m_DeviceMutex, std::try_to_lock);
            if (!deviceGuard.owns_lock()) {
                wl_event_source_timer_update(self->m_Wakeup, 16);
                return 0;
            }
            auto *device = anland_gamescope_session_device(s_session);
            if (!anland_device_is_connected(device)) {
                nudge_steamcompmgr();
            } else {
                pollfd fds[2] = {
                    {anland_device_data_fd(device), POLLIN, 0},
                    {anland_device_buffer_ready_fd(device), POLLIN, 0}
                };
                if (poll(fds, 2, 0) > 0) nudge_steamcompmgr();
            }
            wl_event_source_timer_update(self->m_Wakeup, 16);
            return 0;
        }
        wl_event_source *m_Wakeup = nullptr;
        bool m_WasConnected = false;
        static bool Resources(void *userdata, uint32_t service, int *fds, int count)
        {
            auto *self = static_cast<CAnlandBackend *>(userdata);
            if (!self->m_CameraStarted || service != ANLAND_DEVICE_SERVICE_CAMERA || count < 2)
                return false;
            anland_camera_set_resources(fds[0], &fds[1], count - 1);
            return true; // engine adopted every fd
        }
        static void Text(void *userdata, uint32_t type, const char *text, size_t size)
        {
            auto *self = static_cast<CAnlandBackend *>(userdata);
            if (type == ANLAND_DEVICE_IN_CLIPBOARD) {
                // This callback runs on XWM, the owner of all Xlib selections.
                gamescope_set_selection(std::string(text, size), GAMESCOPE_SELECTION_CLIPBOARD);
                wlserver_lock();
                anland_clipboard_set(self->m_Clipboard, text, size);
                wlserver_unlock();
                return;
            }
            if (type != ANLAND_DEVICE_IN_TEXT_INPUT || !size || !self->m_Ime) return;
            // Existing Gamescope IME expects valid UTF-8 and a terminated string.
            for (size_t i = 0; i < size;) {
                unsigned char c = text[i];
                if (!c) return;
                unsigned n = c < 0x80 ? 1 : c >= 0xC2 && c <= 0xDF ? 2 :
                    c >= 0xE0 && c <= 0xEF ? 3 : c >= 0xF0 && c <= 0xF4 ? 4 : 0;
                if (!n || i + n > size) return;
                for (unsigned j = 1; j < n; ++j)
                    if ((static_cast<unsigned char>(text[i+j]) & 0xC0) != 0x80) return;
                if (n == 3 && ((c == 0xE0 && (unsigned char)text[i+1] < 0xA0) ||
                               (c == 0xED && (unsigned char)text[i+1] >= 0xA0))) return;
                if (n == 4 && ((c == 0xF0 && (unsigned char)text[i+1] < 0x90) ||
                               (c == 0xF4 && (unsigned char)text[i+1] >= 0x90))) return;
                i += n;
            }
            wlserver_lock();
            if (!type_local_text(self->m_Ime, text))
                fprintf(stderr, "Anland: text input rejected (local IME queue limit)\n");
            wlserver_unlock();
        }
        AnlandClipboard *m_Clipboard = nullptr;
        wlserver_input_method *m_Ime = nullptr;
        std::mutex m_DeviceMutex;
        bool m_CameraStarted = false;
        bool m_AudioStarted = false;
        bool m_Capture = false;
        std::chrono::steady_clock::time_point m_NextReconnect{};
        std::set<int32_t> m_Keys, m_Buttons, m_Touches;
        anland_gamescope_input m_Input{};
        CAnlandConnector m_Connector;
	};

	/////////////////////////
	// Backend Instantiator
	/////////////////////////

	template <>
	bool IBackend::Set<CAnlandBackend>()
	{
		return Set( new CAnlandBackend{} );
	}

}