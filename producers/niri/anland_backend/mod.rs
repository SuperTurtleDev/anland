//! Anland backend: renders niri directly into the Android consumer's dmabufs.
//!
//! The anland transport is a buffer-sharing protocol: the Android consumer owns a
//! set of dmabufs (plus a "buffer ready" eventfd, a data socket and a fence
//! socket). We render niri's scene into the consumer-selected dmabuf with EGL,
//! hand over a native fence and `trigger_refresh()`, then wait for the consumer's
//! buffer-ready signal before completing the frame.

pub mod client;
pub use client::ffi;
pub mod frame_state;
pub mod input;
use smithay::wayland::text_input::TextInputSeat;
use smithay::input::keyboard::xkb;

use std::mem;
use std::collections::VecDeque;
use std::os::fd::{FromRawFd, IntoRawFd, OwnedFd};
use std::sync::{Arc, Mutex};
use std::time::Duration;

use anyhow::Context as _;
use niri_config::OutputName;
use smithay::backend::allocator::dmabuf::{Dmabuf, DmabufFlags};
use smithay::backend::allocator::{Fourcc, Modifier};
use smithay::backend::egl::native::EGLSurfacelessDisplay;
use smithay::backend::egl::{EGLContext, EGLDevice, EGLDisplay};
use smithay::backend::renderer::damage::{OutputDamageTracker, RenderOutputResult};
use smithay::backend::renderer::gles::GlesRenderer;
use smithay::backend::renderer::{Bind, Color32F, ImportDma};
use smithay::desktop::utils::OutputPresentationFeedback;
use smithay::output::{Mode, Output, PhysicalProperties, Subpixel};
use smithay::reexports::calloop::timer::{TimeoutAction, Timer};
use smithay::reexports::wayland_protocols::wp::presentation_time::server::wp_presentation_feedback;
use smithay::utils::Size;
use smithay::wayland::dmabuf::{DmabufFeedbackBuilder, DmabufGlobal};
use smithay::wayland::presentation::Refresh;

use super::{IpcOutputMap, OutputId, RenderResult};
use crate::niri::{Niri, RedrawState};
use crate::render_helpers::{resources, shaders, RenderCtx, RenderTarget};
use crate::utils::{get_monotonic_time, logical_output};

/// How often we poll the consumer for input, buffer-ready and reconnects.
/// Kept at 1 ms so buffer-ready (our "vblank") is picked up with minimal
/// latency; the anland lockstep throughput is bound by this plus the render.
const POLL_INTERVAL: Duration = Duration::from_millis(1);

/// How long a new consumer buffer size must remain unchanged before it becomes
/// the output mode. Android can briefly alternate portrait and landscape
/// buffers during rotation, so changing the mode on the first frame causes the
/// compositor geometry to flap with it.
const SIZE_STABILITY_DURATION: Duration = Duration::from_millis(500);

/// Minimum interval between two output mode changes. This keeps a late buffer
/// from immediately undoing a size that just passed the stability window.
const SIZE_ADAPT_COOLDOWN: Duration = Duration::from_millis(500);

/// Converts the anland protocol pixel format into a DRM fourcc.
fn protocol_format_to_fourcc(format: u32) -> Fourcc {
    // Same as kwin's anland backend: 1 == DRM_FORMAT_ABGR8888, else XRGB8888.
    match format {
        1 => Fourcc::Abgr8888,
        _ => Fourcc::Xrgb8888,
    }
}

fn egl_device_for_env() -> Option<EGLDevice> {
    let env = std::env::var_os("ANLAND_DRM_DEVICE")
        .map(|p| p.to_string_lossy().into_owned())
        .filter(|p| !p.is_empty());

    if let Ok(devices) = EGLDevice::enumerate() {
        for dev in devices {
            let matches = if let Some(ref desired) = env {
                let drm_matches = dev
                    .drm_device_path()
                    .map(|p| p.to_string_lossy().into_owned() == *desired)
                    .unwrap_or(false);
                let render_matches = dev
                    .render_device_path()
                    .map(|p| p.to_string_lossy().into_owned() == *desired)
                    .unwrap_or(false);
                drm_matches || render_matches
            } else {
                true
            };
            if matches {
                return Some(dev);
            }
        }
    }

    None
}

fn create_egl_display() -> anyhow::Result<EGLDisplay> {
    if let Some(device) = egl_device_for_env() {
        match unsafe { EGLDisplay::new(device) } {
            Ok(display) => {
                info!("anland: created EGL display from a device");
                return Ok(display);
            }
            Err(err) => {
                warn!("anland: error creating EGL display from device, falling back: {err:?}");
            }
        }
    }
    let display = unsafe { EGLDisplay::new(EGLSurfacelessDisplay) }
        .context("error creating EGL display (surfaceless)")?;
    Ok(display)
}

fn duration_to_micros(duration: Duration) -> u64 {
    duration.as_micros().min(u128::from(u64::MAX)) as u64
}

/// Record an observed buffer size and report whether the same candidate has
/// remained stable for the requested duration.
fn pending_size_is_stable(
    pending: &mut Option<(i32, i32, u64)>,
    observed: (i32, i32),
    now_usec: u64,
    stability: Duration,
) -> bool {
    match pending {
        Some((width, height, since)) if (*width, *height) == observed => {
            now_usec.saturating_sub(*since) >= duration_to_micros(stability)
        }
        _ => {
            *pending = Some((observed.0, observed.1, now_usec));
            false
        }
    }
}

pub struct Anland {
    client: client::Client,
    socket_path: String,
    frame_state: frame_state::FrameState,
    session_generation: Option<u64>,
    pending_text_keysyms: VecDeque<Vec<xkb::Keysym>>,
    screen_w: u32,
    screen_h: u32,
    screen_refresh: u32,
    renderer: Option<GlesRenderer>,
    damage_tracker: Option<OutputDamageTracker>,
    dmabuf_global: Option<DmabufGlobal>,
    ipc_outputs: Arc<Mutex<IpcOutputMap>>,
    output: Option<Output>,
    input_backend: input::AnlandInputBackend,
    /// Candidate consumer size and the monotonic timestamp at which it was
    /// first observed continuously.
    pending_size: Option<(i32, i32, u64)>,
    size_adapt_cooldown_until: u64,
    /// Whether the consumer has presented the last frame (or just connected) and
    /// is ready for another one. Gates rendering so we never draw into a buffer
    /// the consumer is still displaying.

    pending_feedback: Option<OutputPresentationFeedback>,
    frame_seq: u64,
    frame_stats_count: u32,
    frame_stats_start: Duration,
    /// The frame number in which each consumer dmabuf was last rendered into.
    /// Used to compute the buffer `age` for the damage tracker. Passing age 0
    /// forces a full-output redraw every frame, which re-reads every client
    /// buffer; software (SHM/llvmpipe) clients rewrite their released buffers
    /// and that mid-write read shows up as random flicker. A real age makes the
    /// damage tracker redraw only what changed.
    buffer_last_rendered_frame: [u64; ffi::MAX_BUFS],
    /// The `Dmabuf` we render into per consumer buffer index. smithay's GL
    /// renderer caches the imported EGL image / texture / renderbuffer keyed by
    /// the `Dmabuf` object; if we built a fresh `Dmabuf` every frame that cache
    /// never hits, so each frame created and destroyed new EGL images, and on
    /// freedreno/KGSL the teardown freed the BO while the GPU still referenced
    /// it ("premature free" GPU page faults → flicker / corruption). Keeping the
    /// same `Dmabuf` alive makes the renderer reuse the cached resources.
    consumer_dmabufs: [Option<Dmabuf>; ffi::MAX_BUFS],
    /// Signalled by the selection handler when a client sets the clipboard; the
    /// poll timer reads the new selection and forwards it to the consumer.
    selection_tx: std::sync::mpsc::Sender<()>,
    selection_rx: std::sync::mpsc::Receiver<()>,
    /// Signalled by the clipboard reader thread with the compositor clipboard text.
    clipboard_tx: std::sync::mpsc::Sender<String>,
    clipboard_rx: std::sync::mpsc::Receiver<String>,
    disposed: bool,
}

unsafe impl Send for Anland {}

impl Anland {
    pub fn new() -> anyhow::Result<Self> {
        // NOTE: we used to unset GALLIUM_DRIVER=kgsl here because the stock Arch
        // mesa (26.1.x) couldn't create a surfaceless DRI screen with the KGSL
        // backend, forcing the compositor onto llvmpipe. With the
        // lfdevs/mesa-for-android-container build (26.2.0+, "fix KGSL
        // initialization for surfaceless and Wayland") the KGSL backend works,
        // and /etc/environment already exports GALLIUM_DRIVER/MESA_LOADER_
        // DRIVER_OVERRIDE=kgsl + FD_FORCE_KGSL=1 for native Adreno GL. Do NOT
        // touch them here.

        let socket_path =
            std::env::var("ANLAND_SOCKET").unwrap_or_else(|_| "/run/display.sock".to_string());
        let mut client = client::Client::open(&socket_path)
            .context("error opening common producer device")?;
        // Do not reconnect before the event loop exists. The first timer tick
        // establishes the session, records its generation, then dispatches
        // OUTPUT_CHANGED/RENDER_TARGET_READY without losing the first target.
        let initial = client.output().context("no output description from daemon")?;
        let (width, height, refresh) =
            (initial.width.max(1), initial.height.max(1), initial.refresh_mhz.max(1));
        info!("anland: common device output {width}x{height} {refresh} mHz");
        let ipc_outputs = Arc::new(Mutex::new(IpcOutputMap::new()));
        let input_backend = input::AnlandInputBackend {
            native_w: width,
            native_h: height,
            scale: 1.5,
            time_offset: 0,
        };
        let (selection_tx, selection_rx) = std::sync::mpsc::channel();
        let (clipboard_tx, clipboard_rx) = std::sync::mpsc::channel();

        Ok(Self {
            client,
            socket_path,
            frame_state: frame_state::FrameState::new(),
            session_generation: None,
            pending_text_keysyms: VecDeque::new(),
            screen_w: width,
            screen_h: height,
            screen_refresh: refresh,
            renderer: None,
            damage_tracker: None,
            dmabuf_global: None,
            ipc_outputs,
            output: None,
            input_backend,
            pending_size: None,
            size_adapt_cooldown_until: 0,

            pending_feedback: None,
            frame_seq: 0,
            frame_stats_count: 0,
            frame_stats_start: Duration::ZERO,
            buffer_last_rendered_frame: [u64::MAX; ffi::MAX_BUFS],
            consumer_dmabufs: [const { None }; ffi::MAX_BUFS],
            selection_tx,
            selection_rx,
            clipboard_tx,
            clipboard_rx,
            disposed: false,
        })
    }

    /// Sender for the selection handler to request a clipboard forward.
    pub fn clipboard_selection_tx(&self) -> std::sync::mpsc::Sender<()> {
        self.selection_tx.clone()
    }

    pub fn init(&mut self, niri: &mut Niri) {
        if let Err(err) = self.create_renderer() {
            error!("anland: error initializing renderer: {err:#}");
            return;
        }

        niri.update_shaders();

        self.create_dmabuf_global(niri);
        self.add_output(niri);
        // Android already provides committed UTF-8 text. Act as an input method
        // for the focused text-input-v3 client even when no Wayland IME is bound.
        // Enable before focus is established so Smithay sends enter on focus.
        niri.seat.text_input().set_compositor_input_method(true);
        #[cfg(have_anland_audio)]
        if unsafe { ffi::anland_audio_start() } != 0 {
            warn!("anland: failed to start audio engine");
        }
        #[cfg(have_anland_audio)]
        if unsafe { ffi::anland_camera_start() } != 0 {
            warn!("anland: failed to start camera engine");
        }

        let timer = Timer::from_duration(POLL_INTERVAL);
        niri.event_loop
            .insert_source(timer, move |_, _, state| {
                // Reconnect and buffer-ready handling.
                state.backend.anland().poll(&mut state.niri);

                // Drain pending input events from the consumer.
                while let Some(event) = state.backend.anland().poll_input(&mut state.niri) {
                    state.process_input_event(event);
                }

                // Deliver fallback text from the complete State, bypassing niri's
                // shortcut dispatcher while addressing only keyboard focus.
                while let Some(symbols) = state.backend.anland().pending_text_keysyms.pop_front() {
                    if let Some(keyboard) = state.niri.seat.get_keyboard() {
                        // Smithay assigns at most 247 temporary keycodes (9..=255)
                        // per keymap. Send every chunk rather than silently
                        // truncating long Android commitText() batches.
                        for chunk in symbols.chunks(247) {
                            keyboard.inject_text_keysyms(state, chunk);
                        }
                    }
                }
                state.refresh_and_flush_clients();
                TimeoutAction::ToDuration(POLL_INTERVAL)
            })
            .expect("failed to insert anland poll timer");
    }

    /// Advertises the DRM render node to wayland clients via the dmabuf
    /// feedback global, so that EGL clients (zink) can create a hardware
    /// DRI2 screen instead of falling back to llvmpipe.
    fn create_dmabuf_global(&mut self, niri: &mut Niri) {
        let Some(renderer) = self.renderer.as_mut() else {
            warn!("anland: no renderer, skipping dmabuf global");
            return;
        };

        let default_feedback = || {
            let display = renderer.egl_context().display();
            let device =
                EGLDevice::device_for_display(display).context("error getting EGL device")?;
            let node = device
                .try_get_render_node()
                .context("error getting EGL device render node")?
                .context("failed to query EGL device render node")?;

            let primary_formats = renderer.dmabuf_formats();
            DmabufFeedbackBuilder::new(node.dev_id(), primary_formats)
                .build()
                .context("error building dmabuf feedback")
        };

        let dmabuf_global = match default_feedback() {
            Ok(feedback) => niri
                .dmabuf_state
                .create_global_with_default_feedback::<crate::niri::State>(
                    &niri.display_handle,
                    &feedback,
                ),
            Err(err) => {
                warn!(
                    "anland: failed building default dmabuf feedback, falling back to v3: {err:?}"
                );
                let primary_formats = renderer.dmabuf_formats();
                niri.dmabuf_state
                    .create_global::<crate::niri::State>(&niri.display_handle, primary_formats)
            }
        };
        assert!(self.dmabuf_global.replace(dmabuf_global).is_none());
    }

    fn create_renderer(&mut self) -> anyhow::Result<()> {
        let display = create_egl_display()?;
        let context = EGLContext::new(&display).context("error creating EGL context")?;
        let mut renderer =
            unsafe { GlesRenderer::new(context) }.context("error creating GLES renderer")?;

        resources::init(&mut renderer);
        shaders::init(&mut renderer);

        self.renderer = Some(renderer);
        Ok(())
    }

    fn add_output(&mut self, niri: &mut Niri) {
        let connector = "anland-0".to_string();
        let make = "niri".to_string();
        let model = "anland".to_string();
        let serial = "0".to_string();

        let output = Output::new(
            connector.clone(),
            PhysicalProperties {
                size: (0, 0).into(),
                subpixel: Subpixel::Unknown,
                make: make.clone(),
                model: model.clone(),
                serial_number: serial.clone(),
            },
        );

        let mode = Mode {
            size: Size::from((self.screen_w as i32, self.screen_h as i32)),
            refresh: self.screen_refresh as i32,
        };
        output.change_current_state(Some(mode), None, None, None);
        output.set_preferred(mode);

        output.user_data().insert_if_missing(|| OutputName {
            connector,
            make: Some(make),
            model: Some(model),
            serial: Some(serial),
        });

        let physical_properties = output.physical_properties();
        self.ipc_outputs.lock().unwrap().insert(
            OutputId::next(),
            niri_ipc::Output {
                name: output.name(),
                make: physical_properties.make,
                model: physical_properties.model,
                serial: None,
                physical_size: None,
                modes: vec![niri_ipc::Mode {
                    width: self.screen_w as u16,
                    height: self.screen_h as u16,
                    refresh_rate: self.screen_refresh,
                    is_preferred: true,
                }],
                current_mode: Some(0),
                is_custom_mode: true,
                vrr_supported: false,
                vrr_enabled: false,
                logical: Some(logical_output(&output)),
            },
        );

        self.output = Some(output.clone());
        self.damage_tracker = Some(OutputDamageTracker::from_output(&output));
        niri.add_output(output.clone(), None, false);
        self.input_backend.scale = output.current_scale().fractional_scale();
    }

    /// The consumer switched resolution/orientation. Adopt the new buffer size as
    /// our output mode so rendering resumes instead of skipping every frame.
    fn adapt_to_size(&mut self, niri: &mut Niri, output: &Output, w: i32, h: i32) {
        info!("anland: consumer changed size to {w}x{h}, adapting output");
        self.screen_w = w as u32;
        self.screen_h = h as u32;
        self.input_backend.native_w = self.screen_w;
        self.input_backend.native_h = self.screen_h;

        let mode = Mode {
            size: Size::from((w, h)),
            refresh: self.screen_refresh as i32,
        };
        output.change_current_state(Some(mode), None, None, None);
        output.set_preferred(mode);
        self.input_backend.scale = output.current_scale().fractional_scale();

        {
            let mut ipc_outputs = self.ipc_outputs.lock().unwrap();
            if let Some(output) = ipc_outputs.values_mut().next() {
                let mode = &mut output.modes[0];
                mode.width = w.clamp(0, u16::MAX as i32) as u16;
                mode.height = h.clamp(0, u16::MAX as i32) as u16;
                if let Some(logical) = output.logical.as_mut() {
                    logical.width = w as u32;
                    logical.height = h as u32;
                }
            }
            niri.ipc_outputs_changed = true;
        }

        niri.output_resized(output);
    }

    pub fn seat_name(&self) -> String {
        "anland".to_owned()
    }

    pub fn with_primary_renderer<T>(
        &mut self,
        f: impl FnOnce(&mut GlesRenderer) -> T,
    ) -> Option<T> {
        self.renderer.as_mut().map(f)
    }

    /// Polls the consumer: drains input events, checks buffer-ready, and attempts
    /// a reconnect if we fell back. Called from the poll timer.
    pub fn poll(&mut self, niri: &mut Niri) {
        while let Ok(text) = self.clipboard_rx.try_recv() {
            self.send_clipboard_to_consumer(&text);
        }
        while let Ok(()) = self.selection_rx.try_recv() {
            self.read_and_forward_clipboard(niri);
        }
        if let Err(err) = self.client.pump() {
            warn!("anland: presentation pump: {err}");
        }
        if !self.client.connected() {
            self.on_session_lost(niri);
            if !self.client.daemon_alive() {
                let _ = self.client.reopen(&self.socket_path);
            }
            if self.client.reconnect().is_ok() {
                self.on_reconnect(niri);
            }
        }
        self.dispatch_scene(niri);
    }

    fn on_session_lost(&mut self, niri: &mut Niri) {
        if self.session_generation.take().is_none() { return; }
        #[cfg(have_anland_audio)] unsafe {
            ffi::anland_audio_set_fd(-1);
            ffi::anland_camera_clear();
        }
        self.frame_state.reset(0);
        self.pending_text_keysyms.clear();
        self.pending_size = None;
        self.size_adapt_cooldown_until = 0;
        self.pending_feedback = None; // discarded: no false presentation
        self.consumer_dmabufs = [const { None }; ffi::MAX_BUFS];
        self.buffer_last_rendered_frame = [u64::MAX; ffi::MAX_BUFS];
        if let Some(output) = self.output.as_ref() {
            if let Some(state) = niri.output_state.get_mut(output) {
                if matches!(state.redraw_state, RedrawState::WaitingForVBlank { .. }) {
                    state.redraw_state = RedrawState::Idle;
                }
            }
        }
    }

    fn dispatch_scene(&mut self, niri: &mut Niri) {
        let events = match self.client.events() {
            Ok(events) => events,
            Err(err) => { warn!("anland: scene events: {err}"); return; }
        };
        for event in events {
            let e = event.data;
            match e.type_ {
                client::PRESENTED => {
                    if self.frame_state.presented(e.commit_id) == frame_state::FrameAction::Complete(e.commit_id) {
                        self.on_frame_presented(niri);
                    }
                }
                client::COMMIT_DROPPED => {
                    if self.frame_state.dropped(e.commit_id) == frame_state::FrameAction::Cancel(e.commit_id) {
                        self.pending_feedback = None;
                        if let Some(output) = self.output.as_ref() {
                            if let Some(state) = niri.output_state.get_mut(output) {
                                if matches!(state.redraw_state, RedrawState::WaitingForVBlank { .. }) {
                                    state.redraw_state = RedrawState::Idle;
                                }
                            }
                        }
                    }
                }
                client::RENDER_TARGET_READY => {
                    if self.session_generation == Some(e.generation) &&
                        self.frame_state.target_ready(e.generation, e.index, e.count)
                            == frame_state::FrameAction::QueueRedraw {
                        if let Some(output) = self.output.as_ref() { niri.queue_redraw(output); }
                    }
                }
                client::OUTPUT_CHANGED => {
                    if let Some(output) = self.output.clone() {
                        if e.width > 0 && e.height > 0 &&
                            (self.screen_w != e.width || self.screen_h != e.height) {
                            self.adapt_to_size(niri, &output, e.width as i32, e.height as i32);
                        }
                    }
                }
                client::BUFFER_RELEASED => { /* Owned release fence closes on drop. */ }
                _ => {}
            }
        }
    }

    /// Ask the client that owns the current clipboard selection for its text/plain
    /// content and forward it to the consumer once it arrives.
    fn read_and_forward_clipboard(&mut self, niri: &mut Niri) {
        use std::os::fd::FromRawFd;

        use smithay::wayland::selection::data_device::request_data_device_client_selection;

        let mut fds = [0; 2];
        if unsafe { libc::pipe(fds.as_mut_ptr()) } != 0 {
            return;
        }
        let read_fd = fds[0];
        if request_data_device_client_selection(&niri.seat, "text/plain".to_string(), unsafe {
            OwnedFd::from_raw_fd(fds[1])
        })
        .is_err()
        {
            unsafe { libc::close(read_fd) };
            return;
        }

        let tx = self.clipboard_tx.clone();
        std::thread::spawn(move || {
            let mut bytes = Vec::new();
            let start = std::time::Instant::now();
            loop {
                if start.elapsed() > Duration::from_secs(2) || bytes.len() > 2 * 1024 * 1024 {
                    break;
                }
                let mut pfd = libc::pollfd {
                    fd: read_fd,
                    events: libc::POLLIN,
                    revents: 0,
                };
                let n = unsafe { libc::poll(&mut pfd, 1, 200) };
                if n <= 0 {
                    continue;
                }
                let mut buf = [0u8; 4096];
                let r = unsafe { libc::read(read_fd, buf.as_mut_ptr().cast(), buf.len()) };
                if r <= 0 {
                    break; // EOF / error
                }
                bytes.extend_from_slice(&buf[..r as usize]);
            }
            unsafe { libc::close(read_fd) };
            if let Ok(text) = String::from_utf8(bytes) {
                let _ = tx.send(text);
            }
        });
    }

    /// Send a clipboard text to the consumer as an OUTPUT_TYPE_CLIPBOARD event.
    fn send_clipboard_to_consumer(&mut self, text: &str) {
        let _ = self.client.send_clipboard(text.as_bytes());
    }

    fn on_reconnect(&mut self, niri: &mut Niri) {
        let generation = self.client.generation();
        if generation == 0 {
            self.client.drop_session();
            return;
        }
        info!("anland: consumer session generation {generation}");
        self.session_generation = Some(generation);
        self.frame_state.reset(generation);
        self.pending_text_keysyms.clear();
        self.buffer_last_rendered_frame = [u64::MAX; ffi::MAX_BUFS];
        self.consumer_dmabufs = [const { None }; ffi::MAX_BUFS];
        self.pending_size = None;
        self.size_adapt_cooldown_until = 0;
        #[cfg(have_anland_audio)] unsafe {
            ffi::anland_audio_set_fd(self.client.audio_fd());
        }
        #[cfg(have_anland_audio)] {
            let _ = self.client.request_resources(ffi::SERVICE_TYPE_CAMERA);
        }
    }

    /// Polls one input event from the consumer.
    fn poll_input(
        &mut self,
        niri: &mut Niri,
    ) -> Option<smithay::backend::input::InputEvent<input::AnlandInputBackend>> {
        let raw = match self.client.poll_input() {
            Ok(Some(event)) => event,
            Ok(None) => return None,
            Err(err) => {
                warn!("anland: input: {err}");
                self.client.drop_session();
                return None;
            }
        };
        match raw.type_ {
            ffi::INPUT_TYPE_CLIPBOARD | ffi::INPUT_TYPE_TEXT_INPUT => {
                match self.client.read_payload(raw.payload_size as usize) {
                    Ok(bytes) if raw.type_ == ffi::INPUT_TYPE_CLIPBOARD => {
                        if let Ok(text) = String::from_utf8(bytes) {
                            self.set_compositor_clipboard(niri, text);
                        }
                    }
                    Ok(bytes) if raw.type_ == ffi::INPUT_TYPE_TEXT_INPUT => {
                        if let Ok(text) = String::from_utf8(bytes) {
                            self.commit_android_text(niri, &text);
                        } else {
                            warn!("anland: invalid UTF-8 in text input");
                        }
                    }
                    Err(err) => {
                        warn!("anland: input payload: {err}");
                        self.client.drop_session();
                    },
                    _ => {}
                }
            }
            ffi::INPUT_TYPE_RESOURCE => {
                match self.client.read_fds() {
                    Ok(fds) => {
                        #[cfg(have_anland_audio)]
                        if raw.resource_type == ffi::SERVICE_TYPE_CAMERA && fds.len() >= 2 {
                            use std::os::fd::IntoRawFd;
                            let mut fds = fds.into_iter().map(|fd| fd.into_raw_fd()).collect::<Vec<_>>();
                            unsafe { ffi::anland_camera_set_resources(fds[0], fds.as_ptr().add(1), (fds.len() - 1) as i32) };
                            fds.clear(); // camera engine now owns all descriptors
                        }
                    }
                    Err(err) => {
                        warn!("anland: resource fds: {err}");
                        self.client.drop_session();
                    },
                }
            }
            _ => return input::translate(&self.input_backend, &raw),
        }
        None
    }

    /// Commit Android UTF-8 only to an active focused text-input-v3 client.
    fn commit_android_text(&mut self, niri: &mut Niri, text: &str) {
        if text.is_empty() || self.session_generation.is_none() {
            return;
        }
        let handle = niri.seat.text_input();
        let mut delivered = false;
        handle.with_active_text_input(|input, _surface| {
            input.commit_string(Some(text.to_owned()));
            delivered = true;
        });
        if delivered {
            handle.done(false);
        } else if niri.seat.get_keyboard().is_some() {
            // Only address the focused Wayland client, never niri's shortcut
            // dispatcher or the seat's real XKB modifier state.
            let symbols: Vec<_> = text.chars().map(|ch| match ch {
                '\n' | '\r' => xkb::keysyms::KEY_Return.into(),
                '\t' => xkb::keysyms::KEY_Tab.into(),
                '\u{8}' => xkb::keysyms::KEY_BackSpace.into(),
                _ => xkb::utf32_to_keysym(ch as u32),
            }).collect();
            if self.pending_text_keysyms.len() < 128 {
                self.pending_text_keysyms.push_back(symbols);
            } else {
                warn!("anland: text input queue full");
            }
        }
    }

    /// Publish text pushed by the consumer as the compositor's clipboard selection.
    fn set_compositor_clipboard(&mut self, niri: &mut Niri, text: String) {
        use smithay::wayland::selection::data_device::set_data_device_selection;
        if text.is_empty() {
            return;
        }
        set_data_device_selection(
            &niri.display_handle,
            &niri.seat,
            vec!["text/plain".to_string()],
            Arc::from(text.into_bytes()),
        );
    }

    fn on_frame_presented(&mut self, niri: &mut Niri) {
        let Some(output) = self.output.as_ref() else { return; };
        let Some(state) = niri.output_state.get_mut(output) else { return; };
        if !matches!(state.redraw_state, RedrawState::WaitingForVBlank { .. }) {
            return;
        }
        state.redraw_state = RedrawState::Idle;
        let now = get_monotonic_time();
        if let Some(mut feedback) = self.pending_feedback.take() {
            let refresh = state.frame_clock.refresh_interval().map(Refresh::Fixed).unwrap_or(Refresh::Unknown);
            // Legacy acknowledgement does not prove physical scanout.
            feedback.presented::<_, smithay::utils::Monotonic>(now, refresh,
                self.frame_seq, wp_presentation_feedback::Kind::Vsync);
        }
        state.frame_clock.presented(now);
    }
    pub fn render(&mut self, niri: &mut Niri, output: &Output) -> RenderResult {
        let span = tracy_client::span!("Anland::render");

        let Some((generation, index)) = self.frame_state.target() else {
            return RenderResult::Skipped;
        };
        let target = match self.client.target() {
            Ok(target) if target.generation == generation && target.index == index &&
                target.index < target.count && (target.index as usize) < ffi::MAX_BUFS => target,
            _ => return RenderResult::Skipped,
        };
        let idx = target.index as i32;
        let info = &target;
        span.emit_text(&format!(
            "buffer {idx} {}x{} stride {}",
            info.width, info.height, info.stride
        ));

        // The consumer may briefly alternate portrait and landscape buffers
        // while rotating. Only adopt a size that remains stable, but keep
        // rendering during the wait: the consumer's lockstep protocol requires
        // one render completion for every selected buffer.
        let size = output
            .current_mode()
            .map(|m| m.size)
            .unwrap_or(Size::from((0, 0)));
        let buffer_size = (info.width as i32, info.height as i32);
        if (size.w, size.h) == buffer_size {
            self.pending_size = None;
        } else {
            let now_usec = get_monotonic_time().as_micros() as u64;
            let candidate_stable = pending_size_is_stable(
                &mut self.pending_size,
                buffer_size,
                now_usec,
                SIZE_STABILITY_DURATION,
            );
            if candidate_stable && now_usec >= self.size_adapt_cooldown_until {
                self.pending_size = None;
                self.size_adapt_cooldown_until =
                    now_usec.saturating_add(duration_to_micros(SIZE_ADAPT_COOLDOWN));
                self.adapt_to_size(niri, &output, buffer_size.0, buffer_size.1);
            }
        }

        let Some(renderer) = self.renderer.as_mut() else {
            error!("anland: renderer not initialized");
            return RenderResult::Skipped;
        };

        let render_start = get_monotonic_time();

        let mut dmabuf = match self.consumer_dmabufs[idx as usize].take() {
            Some(dmabuf) => dmabuf,
            None => {
                let dup = match target.fd.try_clone() {
                    Ok(fd) => fd,
                    Err(err) => { warn!("anland: dup dmabuf fd: {err}"); return RenderResult::Skipped; }
                };
                let mut builder = Dmabuf::builder(
                    (info.width as i32, info.height as i32),
                    protocol_format_to_fourcc(info.format),
                    Modifier::from(info.modifier),
                    DmabufFlags::empty(),
                );
                builder.add_plane(
                    dup,
                    0, // Smithay pinned by niri v26.04 requires an explicit plane index.
                    info.offset,
                    info.stride,
                );
                match builder.build() {
                    Some(dmabuf) => dmabuf,
                    None => {
                        error!("anland: failed to build dmabuf");
                        return RenderResult::Skipped;
                    }
                }
            }
        };

        // Build the render elements for this output.
        let ctx = RenderCtx {
            renderer: &mut *renderer,
            target: RenderTarget::Output,
            xray: None,
        };
        let elements = niri.render_to_vec(ctx, output, true);

        // Buffer age for the damage tracker: how many consumer frames ago this
        // dmabuf was last rendered into. age 0 forces a full redraw (and a
        // re-read of every client buffer), which makes slow software (SHM)
        // clients flicker. A real age lets the damage tracker redraw only what
        // actually changed.
        let frame = self.frame_seq;
        let age = if self.buffer_last_rendered_frame[idx as usize] == u64::MAX {
            0
        } else {
            frame.wrapping_sub(self.buffer_last_rendered_frame[idx as usize]) as usize
        };
        // Do not advance the buffer age until a frame is actually submitted.
        // Failed binds/renders must not make the old pixels look fresh.

        // Render them into the consumer's dmabuf.
        let damage_tracker = self.damage_tracker.as_mut().unwrap();
        let mut target = match renderer.bind(&mut dmabuf) {
            Ok(target) => target,
            Err(err) => {
                warn!("anland: error binding dmabuf: {err}");
                // The bind result still borrows dmabuf in this match arm.
                // A later attempt must reconstruct the import from the owned fd.
                return RenderResult::Skipped;
            }
        };
        let res = match damage_tracker.render_output(
            renderer,
            &mut target,
            age,
            &elements,
            Color32F::TRANSPARENT,
        ) {
            Ok(res) => res,
            Err(err) => {
                warn!("anland: error rendering to dmabuf: {err}");
                self.buffer_last_rendered_frame[idx as usize] = u64::MAX;
                drop(target);
                self.consumer_dmabufs[idx as usize] = Some(dmabuf);
                return RenderResult::Skipped;
            }
        };
        let RenderOutputResult { sync, states, .. } = res;

        // The target borrows `dmabuf`; drop it now so we can store the dmabuf
        // back into the cache below.
        drop(target);

        let render_dur = get_monotonic_time().saturating_sub(render_start);
        if render_dur > Duration::from_millis(30) {
            info!("anland: SLOW render took {render_dur:?}");
        }

        niri.update_primary_scanout_output(output, &states);

        // Hand a native fence to the consumer so it can wait on the render
        // GPU-side. freedreno/KGSL advertises EGL_ANDROID_native_fence_sync but
        // eglDupNativeFenceFDANDROID fails at runtime (EGL_BAD_PARAMETER), so
        // export() comes up empty even when is_exportable() is true. Without a
        // real fence the consumer presents a half-rendered buffer → flicker /
        // corruption under complex composites. Work around it by blocking on the
        // render fence CPU-side before handing the buffer over.
        let fence = if sync.is_exportable() { sync.export() } else { None };
        if fence.is_none() {
            if let Err(err) = sync.wait() {
                warn!("anland: render fence wait failed: {err}");
                self.buffer_last_rendered_frame[idx as usize] = u64::MAX;
                self.consumer_dmabufs[idx as usize] = Some(dmabuf);
                return RenderResult::Skipped;
            }
        }
        let commit_id = match self.client.submit(generation, index, fence.as_ref()) {
            Ok(id) => id,
            Err(err) => {
                warn!("anland: frame submit: {err}");
                self.client.drop_session();
                self.on_session_lost(niri);
                return RenderResult::Skipped;
            }
        };
        if !self.frame_state.submitted(generation, index, commit_id) {
            self.client.drop_session();
            self.on_session_lost(niri);
            return RenderResult::Skipped;
        }
        // Collect presentation feedback and complete it when the consumer signals
        // buffer-ready.
        self.pending_feedback = Some(niri.take_presentation_feedbacks(output, &states));


        // Mark the frame as in flight. Buffer-ready completes it.
        let output_state = niri.output_state.get_mut(output).unwrap();
        match mem::replace(
            &mut output_state.redraw_state,
            RedrawState::WaitingForVBlank {
                redraw_needed: false,
            },
        ) {
            RedrawState::Idle => unreachable!(),
            RedrawState::Queued => (),
            RedrawState::WaitingForVBlank { .. } => unreachable!(),
            RedrawState::WaitingForEstimatedVBlank(_) => unreachable!(),
            RedrawState::WaitingForEstimatedVBlankAndQueued(token) => {
                niri.event_loop.remove(token);
            }
        }
        output_state.frame_callback_sequence = output_state.frame_callback_sequence.wrapping_add(1);
        self.frame_seq = self.frame_seq.wrapping_add(1);

        self.buffer_last_rendered_frame[idx as usize] = frame;
        // Keep the dmabuf (and thus the renderer's cached EGL image / texture /
        // renderbuffer for it) alive for the next time this buffer index is
        // selected, so we don't recreate EGL images every frame.
        self.consumer_dmabufs[idx as usize] = Some(dmabuf);

        RenderResult::Submitted
    }

    pub fn import_dmabuf(&mut self, dmabuf: &Dmabuf) -> bool {
        // Import client dmabufs through the GL renderer so the compositor can
        // texture them. (Direct scanout is still not possible: the consumer owns
        // all the buffers.) Returning false here made every client dmabuf buffer
        // fail with a fatal protocol error, forcing clients onto SHM buffers.
        let Some(renderer) = self.renderer.as_mut() else {
            return false;
        };
        match renderer.import_dmabuf(dmabuf, None) {
            Ok(_texture) => true,
            Err(err) => {
                debug!("anland: error importing dmabuf: {err:?}");
                false
            }
        }
    }

    pub fn ipc_outputs(&self) -> Arc<Mutex<IpcOutputMap>> {
        self.ipc_outputs.clone()
    }
}

impl Drop for Anland {
    fn drop(&mut self) {
        if self.disposed {
            return;
        }
        self.disposed = true;
        #[cfg(have_anland_audio)]
        unsafe {
            ffi::anland_audio_stop();
        }
        #[cfg(have_anland_audio)]
        unsafe {
            ffi::anland_camera_stop();
        }
    }
}