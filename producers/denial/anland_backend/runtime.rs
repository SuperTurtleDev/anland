//! Non-KMS desktop owner; transport stays in the public facade.
use super::capture::{CaptureState, CapturingRenderer};
use super::client::{Client, fence_signaled};
use super::display::OutputDriver;
use super::event_sources::EventSources;
use super::ffi;
use super::graphics::RenderDevice;
use super::input::{InputStream, Message};
use super::lifetime::{Action, Lifetime};
use super::native::{ImportedPool, deliver_feedback, prepare_output};
use super::pointer_capture::CaptureSync;
use super::scale_control::ScaleControl;
use super::scheduling::SchedulingSync;
use super::seat_input::AnlandInputBackend;
use super::services::ClipboardSync;
use super::topology::{OUTPUT_NAME, OutputPlan};
#[cfg(feature = "anland-runtime-v2")]
use crate::flutter_launcher::{FlutterLaunchConfiguration, FlutterLauncher};
use crate::wayland_frontend::WaylandFrontend;
use crate::*;
use smithay::reexports::calloop::channel::Event as ChannelEvent;
use std::ffi::CString;
use std::io;

// Ubuntu follows the refactored upstream runtime; Arch still uses cad6's flat
// queues. Expand to a field so existing borrows and queue ordering stay intact.
#[cfg(feature = "anland-runtime-v2")]
macro_rules! control_queue {
    ($state:expr, $legacy:ident, $current:ident) => {
        ($state).control.$current
    };
}
#[cfg(not(feature = "anland-runtime-v2"))]
macro_rules! control_queue {
    ($state:expr, $legacy:ident, $current:ident) => {
        ($state).$legacy
    };
}

/// Only the service send boundary is abstracted for fault injection. Encoding,
/// partial writes, backpressure and fallback remain in the public producer.
trait ServiceOutput {
    fn connected(&self) -> bool;
    fn pending_output(&self) -> usize;
    fn queue_clipboard(&mut self, bytes: &[u8]) -> io::Result<()>;
    fn flush_output(&mut self) -> io::Result<bool>;
}
impl ServiceOutput for Client {
    fn connected(&self) -> bool {
        Client::connected(self)
    }
    fn pending_output(&self) -> usize {
        Client::pending_output(self)
    }
    fn queue_clipboard(&mut self, bytes: &[u8]) -> io::Result<()> {
        Client::clipboard(self, bytes)
    }
    fn flush_output(&mut self) -> io::Result<bool> {
        Client::flush(self)
    }
}

/// Confirm disconnection AFTER the failed public call. A live-session error
/// (bad payload/allocation/etc.) is not transport loss and must still propagate.
/// Retirement does not drop an import, reset a fence, or reopen the session.
fn send_service_output<S: ServiceOutput>(
    session: &mut S,
    clipboard: &mut ClipboardSync,
    export: Option<(u64, &str)>,
    lifetime: &mut Lifetime,
    now: Instant,
    retire: impl FnOnce(&mut S, &io::Error),
) -> io::Result<()> {
    let sent = (|| {
        if let Some((epoch, text)) = export {
            clipboard.export(epoch, text, |bytes| session.queue_clipboard(bytes))?;
        }
        if session.pending_output() != 0 {
            let _ = session.flush_output()?;
        }
        Ok(())
    })();
    match sent {
        Err(error) if !session.connected() => {
            retire(session, &error);
            lifetime.stop(now);
            Ok(())
        }
        other => other,
    }
}

/// A reload cannot change branches while the raster callback owns imports.
/// Keep it requested; once the result is consumed, normal safe cold retirement
/// will honor it. Incompatible topology is never made compatible by a reload.
fn retain_runtime(
    replacement_rejected: bool,
    reload_requested: bool,
    replacement_outstanding: bool,
    compatible: bool,
) -> bool {
    !replacement_rejected && compatible && (!reload_requested || replacement_outstanding)
}

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
enum ReplacementProgress {
    Completed,
    Busy,
    Rejected,
    Pending,
}

/// Poll first: a queued completion is lifetime proof, even at the deadline.
/// Empty/Busy do not prove cancellation; uncertain/disconnected channels and
/// the original five-second deadline must continue to fail closed.
fn poll_replacement(
    receiver: Option<&std::sync::mpsc::Receiver<denial_flutter_engine::RenderOperationResult>>,
    since: Instant,
    now: Instant,
) -> io::Result<ReplacementProgress> {
    use denial_flutter_engine::RenderOperationResult;
    use std::sync::mpsc::TryRecvError;
    let expired = now.saturating_duration_since(since) >= Duration::from_secs(5);
    match receiver.map(|receiver| receiver.try_recv()) {
        Some(Ok(RenderOperationResult::Completed)) => Ok(ReplacementProgress::Completed),
        Some(Ok(RenderOperationResult::Rejected)) => Ok(ReplacementProgress::Rejected),
        Some(Ok(RenderOperationResult::Busy)) if !expired => Ok(ReplacementProgress::Busy),
        Some(Ok(RenderOperationResult::Busy)) => Err(io::Error::new(
            io::ErrorKind::TimedOut,
            "Anland replacement retirement deadline; retain borrowed backing stores",
        )),
        Some(Ok(RenderOperationResult::Uncertain)) | Some(Err(TryRecvError::Disconnected)) => Err(
            io::Error::other("Anland render replacement lost lifetime proof; retain all owners"),
        ),
        None | Some(Err(TryRecvError::Empty)) if !expired => Ok(ReplacementProgress::Pending),
        None | Some(Err(TryRecvError::Empty)) => Err(io::Error::new(
            io::ErrorKind::TimedOut,
            "Anland render replacement deadline (not cancellation); retain all owners",
        )),
    }
}

/// Guards for sampled Wayland client buffers must outlive their GPU fence even
/// during shutdown/reconnect. Keep them in the owner, not a callback destroyed
/// with the loop. Deadline/query failure is fail-closed, never early release.
struct SampledReleases {
    pending: Vec<(
        std::os::fd::OwnedFd,
        flutter_runtime::SampledBufferHoldBatch,
        Instant,
    )>,
}
impl SampledReleases {
    fn drain(&mut self, events: &mut RuntimeState, now: Instant) -> io::Result<()> {
        for (fence, batch) in events.sampled_buffer_releases.drain(..) {
            if let Some(fence) = fence {
                self.pending.push((fence, batch, now));
            }
        }
        let mut index = 0;
        while index < self.pending.len() {
            use std::os::fd::AsFd;
            if fence_signaled(self.pending[index].0.as_fd())? {
                self.pending.swap_remove(index);
            } else if now.saturating_duration_since(self.pending[index].2) >= Duration::from_secs(5)
            {
                return Err(io::Error::new(
                    io::ErrorKind::TimedOut,
                    "sampled Wayland buffer fence timed out",
                ));
            } else {
                index += 1;
            }
        }
        Ok(())
    }
}

struct Active {
    captures: CaptureState,
    driver: OutputDriver,
    runtime: flutter_runtime::FlutterRuntime,
    _imports: ImportedPool,
    plan: OutputPlan,
}

fn synchronize_refresh(
    active: &mut Active,
    events: &mut RuntimeState,
    publisher: &output_control::OutputControlPublisher,
    refresh: u32,
    now: Instant,
    scale: &ScaleControl,
) -> Result<(), Box<dyn Error>> {
    if active.plan.set_refresh(refresh)? {
        active.driver.set_refresh(refresh, now)?;
        active
            .runtime
            .update_external_refresh(refresh, active.plan.atlas.pixel_size)?;
        if let Some(frontend) = events.wayland.as_mut() {
            frontend.update_external_refresh(active.plan.pool.output, refresh)?;
        }
        publisher.publish(scale.state(Some(&active.plan)));
    }
    Ok(())
}

/// Resident geometry-only transaction. Reject cleanly after restoring the old
/// projection; an unprovable restore is fatal and retains GPU owners in run().
fn install_scale_plan(
    active: &mut Active,
    events: &mut RuntimeState,
    topology: &mut TopologyManager,
    plan: &OutputPlan,
) -> Result<Result<(), OutputControlFailure>, Box<dyn Error>> {
    let install = (|| -> Result<(), Box<dyn Error>> {
        active.runtime.reconfigure_output_geometry(
            &plan.snapshot,
            &plan.atlas,
            flutter_runtime::OutputGeometryTransition::Immediate,
        )?;
        events
            .wayland
            .as_mut()
            .ok_or("Anland frontend missing")?
            .update_topology(&plan.snapshot)?;
        events.flutter_input.resize(plan.atlas.pixel_size);
        events.synchronize_flutter_pointer_position();
        events.install_workspace_snapshot(&mut active.runtime)?;
        Ok(())
    })();
    if let Err(error) = install {
        active.runtime.reconfigure_output_geometry(
            &active.plan.snapshot,
            &active.plan.atlas,
            flutter_runtime::OutputGeometryTransition::Immediate,
        )?;
        events
            .wayland
            .as_mut()
            .ok_or("Anland frontend missing")?
            .update_topology(&active.plan.snapshot)?;
        events.flutter_input.resize(active.plan.atlas.pixel_size);
        events.synchronize_flutter_pointer_position();
        events.install_workspace_snapshot(&mut active.runtime)?;
        return Ok(Err(OutputControlFailure::new(
            "apply_failed",
            error.to_string(),
        )));
    }
    events.scene_sync.mark_dirty();
    active.plan = plan.clone();
    *topology = plan.topology.clone();
    Ok(Ok(()))
}

fn process_scale_controls(
    active: &mut Active,
    events: &mut RuntimeState,
    publisher: &output_control::OutputControlPublisher,
    topology: &mut TopologyManager,
    scale: &mut ScaleControl,
    now: Instant,
) -> Result<(), Box<dyn Error>> {
    use output_control::*;
    let due = scale.rollback_due(now);
    let pending = due.is_some()
        || !control_queue!(events, pending_output_applies, output_applies).is_empty()
        || !control_queue!(events, pending_output_confirmations, output_confirmations).is_empty();
    active.driver.set_geometry_pending(pending);
    if !pending || !active.driver.geometry_ready(&active.runtime, now) {
        return Ok(());
    }
    if let Some(previous) = due {
        let plan = active.plan.with_scale(previous)?;
        install_scale_plan(active, events, topology, &plan)?.map_err(|error| error.message)?;
        scale.cancel_preview();
        publisher.publish(scale.state(Some(&active.plan)));
    }
    while let Some(request) =
        control_queue!(events, pending_output_confirmations, output_confirmations).pop_front()
    {
        let result = if events.secure_session_locked() {
            Err(OutputControlFailure::new("locked", "session locked"))
        } else {
            match request.action {
                OutputConfirmationAction::Keep => scale.keep(request.token),
                OutputConfirmationAction::Rollback => match scale.rollback_scale(request.token) {
                    Ok(previous) => {
                        let plan = active.plan.with_scale(previous)?;
                        let result = install_scale_plan(active, events, topology, &plan)?;
                        if result.is_ok() {
                            scale.cancel_preview();
                        }
                        result
                    }
                    Err(error) => Err(error),
                },
            }
        };
        publisher.publish(scale.state(Some(&active.plan)));
        request.reply(result);
    }
    if let Some(request) =
        control_queue!(events, pending_output_applies, output_applies).pop_front()
    {
        let current = publisher.snapshot();
        let prepared = if events.secure_session_locked() {
            Err(OutputControlFailure::new("locked", "session locked"))
        } else {
            scale.prepare(&request.configuration, &current, &active.plan)
        };
        let result = match prepared {
            Err(error) => Err(error),
            Ok(prepared) => match install_scale_plan(active, events, topology, &prepared.plan)? {
                Err(error) => Err(error),
                Ok(()) => {
                    let result =
                        scale.applied(prepared, output_control::next_serial(current.serial), now);
                    let snapshot = publisher.publish(scale.state(Some(&active.plan)));
                    result.map(|_| snapshot)
                }
            },
        };
        request.reply(result);
    }
    active.driver.set_geometry_pending(
        !control_queue!(events, pending_output_applies, output_applies).is_empty(),
    );
    Ok(())
}

fn reject_unavailable_scale_requests(events: &mut RuntimeState) {
    for request in control_queue!(events, pending_output_applies, output_applies).drain(..) {
        request.reply(Err(OutputControlFailure::new(
            "output_unavailable",
            "Anland output is not active",
        )));
    }
    for request in
        control_queue!(events, pending_output_confirmations, output_confirmations).drain(..)
    {
        request.reply(Err(OutputControlFailure::new(
            "output_unavailable",
            "Anland output is not active",
        )));
    }
}

fn synchronize_pointer_capture(
    events: &RuntimeState,
    client: &mut Client,
    capture: &mut CaptureSync,
    output_running: bool,
) -> io::Result<()> {
    let locked = events.secure_session_locked();
    let requested = output_running
        && !locked
        && events.wayland.as_ref().is_some_and(|frontend| {
            frontend.consumer_pointer_capture_requested(events.flutter_input.pointer_captured())
        });
    capture.synchronize(
        client.connected(),
        output_running,
        locked,
        requested,
        |desired| client.capture_pointer(desired),
    )
}

fn synchronize_scheduling(
    events: &RuntimeState,
    client: &mut Client,
    scheduling: &mut SchedulingSync,
    output_running: bool,
) -> io::Result<()> {
    let wanted_pid = if output_running && !events.secure_session_locked() {
        Some(
            events
                .wayland
                .as_ref()
                .and_then(WaylandFrontend::focused_client_pid)
                .unwrap_or_else(|| std::process::id() as i32),
        )
    } else {
        None
    };
    scheduling.synchronize(client.connected(), wanted_pid, |pid, flags| {
        client.scheduling(pid, flags)
    })
}

fn stop(
    active: &mut Active,
    events: &mut RuntimeState,
    stream: &mut InputStream,
    seat: &mut AnlandInputBackend,
    client: &mut Client,
    capture: &mut CaptureSync,
) {
    release_tablet_devices(events, seat);
    if let Err(error) = synchronize_pointer_capture(events, client, capture, false) {
        warn!(%error, "Anland capture release failed; session will be detached");
    }
    if let Some(frontend) = events.wayland.as_mut() {
        frontend.release_consumer_pointer_constraint();
    }
    active.captures.stop(&mut active.runtime, events);
    for outcome in active.driver.stop(&active.runtime) {
        if let Some(frontend) = events.wayland.as_mut() {
            let _ = deliver_feedback(frontend, &outcome);
        }
    }
    stream.reset();
    seat.reset();
    wayland_frontend::reset_all_input_devices(events);
    if let Some(frontend) = events.wayland.as_mut() {
        let _ = frontend.display_handle.flush_clients();
    }
}

fn release_tablet_devices(events: &mut RuntimeState, seat: &mut AnlandInputBackend) {
    for event in seat.take_tablet_releases() {
        wayland_frontend::process_routed_input_event(events, event, "anland-consumer");
    }
}

pub(crate) fn run(options: Options) -> Result<(), Box<dyn Error>> {
    let bundle = options
        .flutter_bundle
        .as_deref()
        .ok_or("Anland requires --flutter-bundle")?;
    let render_path = options
        .render_device
        .as_deref()
        .ok_or("Anland requires an explicit --render-device; --device is KMS-only")?;
    if options.software_rendering {
        return Err("Anland direct consumer targets require the native GLES renderer".into());
    }
    let limit = options.runtime_limit();
    let mut desktop = flutter_desktop::Startup::new(
        limit,
        options.polkit_agent,
        std::env::var("DENIAL_ANLAND_DESKTOP").ok().as_deref() != Some("0"),
    );
    // Mask before EGL or any desktop worker is created. Never acquire a VT.
    let signals = Signals::new(&[
        Signal::SIGINT,
        Signal::SIGTERM,
        Signal::SIGUSR1,
        Signal::SIGUSR2,
    ])?;
    let mut event_loop: EventLoop<'static, RuntimeState> = EventLoop::try_new()?;
    event_loop
        .handle()
        .insert_source(signals, |event, _, state| match event.signal() {
            Signal::SIGINT => state.lifecycle.request_shutdown(ShutdownReason::Interrupt),
            Signal::SIGTERM => state.lifecycle.request_shutdown(ShutdownReason::Terminate),
            Signal::SIGUSR1 => state.flutter_reload_requested = true,
            Signal::SIGUSR2 => {
                warn!("Anland geometry is owned by the consumer; KMS reconfiguration ignored")
            }
            _ => {}
        })?;
    let mut settings = Some(settings::SettingsManager::load()?);
    if let Some(settings) = settings.as_mut()
        && let Err(error) = settings.keyboard().compiled_layout_names()
    {
        warn!(%error, "using safe US keymap without overwriting settings");
        settings.replace_invalid_keyboard_with_default();
    }
    let mut shortcuts = Some(ShortcutManager::load()?);
    let mut scale = ScaleControl::new(
        options
            .scales_120
            .get(OUTPUT_NAME)
            .copied()
            .unwrap_or(denial_core::topology::SCALE_BASE),
        options.output_config.clone(),
    )?;
    let portal = PortalIpcServer::start(settings.as_ref().unwrap().theme_snapshot())?;
    let (control, source) = OutputControlServer::start(
        scale.state(None),
        settings.as_ref().unwrap().revision(),
        settings.as_ref().unwrap().document_json()?,
    )?;
    event_loop
        .handle()
        .insert_source(source, |event, _, state: &mut RuntimeState| {
            if let ChannelEvent::Msg(request) = event {
                match request {
                    ControlEvent::OutputApply(request) => {
                        control_queue!(state, pending_output_applies, output_applies)
                            .push_back(request)
                    }
                    ControlEvent::OutputConfirmation(request) => {
                        control_queue!(state, pending_output_confirmations, output_confirmations)
                            .push_back(request)
                    }
                    ControlEvent::Settings(request) => {
                        control_queue!(state, pending_settings_controls, settings)
                            .push_back(request)
                    }
                    ControlEvent::SystemControl(request) => {
                        control_queue!(state, pending_system_controls, system).push_back(request)
                    }
                    ControlEvent::UiDevelopment(request) => {
                        request.reply(Err(OutputControlFailure::new(
                            "unsupported",
                            "Anland UI development control is not wired",
                        )))
                    }
                    ControlEvent::SoftwareDimming(request) => {
                        let (_, reply) = request.into_parts();
                        let _ = reply.send(Err(OutputControlFailure::new(
                            "unsupported",
                            "Anland software dimming is not wired",
                        )));
                    }
                    ControlEvent::Shell(request) => {
                        if state.secure_session_locked() {
                            request
                                .reply(Err(OutputControlFailure::new("locked", "session locked")));
                        } else {
                            match request.command {
                                ShellControlCommand::OpenWallpaper => {
                                    state.queue_shell_action(wire::ShellAction::Wallpaper, None)
                                }
                            }
                            request.reply(Ok(()));
                        }
                    }
                    #[cfg(feature = "anland-runtime-v2")]
                    ControlEvent::RepaintOutputs => {
                        // The consumer clock already renders every output tick.
                    }
                    #[cfg(feature = "anland-runtime-v2")]
                    ControlEvent::RepaintApp(app_id) => state.control.repaint_apps.push(app_id),
                    #[cfg(feature = "anland-runtime-v2")]
                    ControlEvent::RebuildOutputs => {
                        // Recreate renderer resources at the existing safe retirement
                        // boundary; buffer formats remain owned by the consumer.
                        state.flutter_reload_requested = true;
                    }
                    #[cfg(feature = "anland-runtime-v2")]
                    event @ (ControlEvent::OpenColorTuning | ControlEvent::ReopenColorTuning) => {
                        state.receive_control(event);
                    }
                    #[cfg(feature = "anland-runtime-v2")]
                    event @ ControlEvent::SaveColor(_) => {
                        if state.wayland.is_some() {
                            state.receive_control(event);
                        } else {
                            warn!("cannot save color settings before the Anland output is active");
                        }
                    }
                }
            }
        })?;
    let publisher = control.publisher();
    let sender = flutter_desktop::install_runtime_events(&mut event_loop)?;
    let mut graphics = RenderDevice::open(render_path)?;
    let endpoint = std::env::var("ANLAND_SOCKET")
        .ok()
        .map(CString::new)
        .transpose()?;
    let mut client: Option<Client> = None;
    let mut sources = EventSources::new(event_loop.handle())?;
    let mut input_stream = InputStream::default();
    let mut input_seat = AnlandInputBackend::default();
    let mut clipboard_sync = ClipboardSync::default();
    let mut pointer_capture = CaptureSync::default();
    let mut scheduling = SchedulingSync::default();
    let mut camera_requested = false;
    let mut pending_service_context = None;
    let mut waiting_initial_refresh = false;
    let mut launcher: Option<FlutterLauncher> = None;
    let mut active: Option<Active> = None;
    let mut parked: Option<Active> = None;
    // Guards live outside the unwind boundary: startup itself can fail after
    // giving EGL targets to an engine whose shutdown cannot be proven.
    let mut staged_imports: Option<ImportedPool> = None;
    let mut staged_plan: Option<OutputPlan> = None;
    let mut replacing: Option<
        std::sync::mpsc::Receiver<denial_flutter_engine::RenderOperationResult>,
    > = None;
    let mut replacement_started: Option<Instant> = None;
    let mut replacement_retry = Instant::now();
    let mut replacement_rejected = false;
    let mut staged_frontend: Option<WaylandFrontend> = None;
    let mut events = RuntimeState::default();
    let mut topology = TopologyManager::default();
    let mut lifetime = Lifetime::new(Instant::now());
    let mut finishing = false;
    let mut fail_closed = false;
    let mut failure: Option<String> = None;
    let mut sampled = SampledReleases {
        pending: Vec::new(),
    };
    let started = Instant::now();
    let mut frames = 0u64;
    let mut immediate = true;
    let audit_frames = std::env::var("DENIAL_RENDER_AUDIT").ok().as_deref() == Some("1");
    let mut audit_since = started;
    let mut audit_ticks = 0u64;
    let mut audit_renders = 0u64;
    let mut audit_vsync = 0u64;
    let mut audit_presented = 0u64;

    // Direct GLES replacement uses the resident renderer policy (including
    // Impeller stencil). Offscreen storage, changed topology and reload stay cold.
    let warm_supported = !options.flutter_offscreen_blit;
    // Anland always composites client textures with EGL. Explicit acquire
    // points and exact-frame targets still retain CPU readiness monitoring.
    let defer_implicit_dmabuf_wait =
        std::env::var("ANLAND_SKIP_IMPLICIT_SYNC_WAIT").map_or(true, |value| value == "1");
    let coalesce_buffer_commits = std::env::var("DENIAL_ANLAND_COALESCE_BUFFER_COMMITS")
        .ok()
        .as_deref()
        != Some("0");
    info!(
        defer_implicit_dmabuf_wait,
        coalesce_buffer_commits,
        "Anland deferred startup: waiting for daemon, consumer pool and real target; no arrival timeout"
    );
    let outcome = catch_unwind(AssertUnwindSafe(|| -> Result<(), Box<dyn Error>> {
        loop {
            let timeout = if immediate {
                Duration::ZERO
            } else {
                active
                    .as_ref()
                    .or(parked.as_ref())
                    .map_or(Duration::from_millis(100), |a| {
                        let now = Instant::now();
                        #[cfg(feature = "anland-runtime-v2")]
                        let timeout = a.runtime.next_dispatch_timeout(now);
                        #[cfg(not(feature = "anland-runtime-v2"))]
                        let timeout = a.runtime.next_dispatch_timeout();
                        timeout.min(a.driver.wait_timeout(&a.runtime, now))
                    })
                    .min(Duration::from_millis(100))
            };
            let timeout = sampled
                .pending
                .iter()
                .fold(timeout, |timeout, (_, _, since)| {
                    timeout.min(
                        (*since + Duration::from_secs(5)).saturating_duration_since(Instant::now()),
                    )
                });
            // Asynchronous buffer retirement can queue wl_buffer.release even
            // without frame callbacks or presentation feedback. Flush every
            // iteration before waiting, so mailbox clients can reuse buffers.
            if let Some(frontend) = events.wayland.as_mut() {
                frontend.display_handle.flush_clients()?;
            }
            event_loop.dispatch(timeout, &mut events)?;
            let now = Instant::now();
            sampled.drain(&mut events, now)?;
            // A stopped output must still reap shared capture leases; otherwise
            // a completed snapshot could block retirement/reconnect forever.
            if let Some(frontend) = events.wayland.as_mut() {
                frontend.reap_capture_source_reads();
            }
            immediate = false;
            if events.lifecycle.shutdown_reason().is_some()
                || matches!(limit, RuntimeLimit::Duration(d) if now.duration_since(started) >= d)
                || matches!(limit, RuntimeLimit::Frames(n) if frames >= n)
            {
                finishing = true;
            }
            if let Some(a) = parked.as_mut() {
                if let Err(error) =
                    flutter_desktop::process_flutter_event_batch(&mut a.runtime, &mut events)
                {
                    failure.get_or_insert(error.to_string());
                    finishing = true;
                }
                sampled.drain(&mut events, now)?;
                synchronize_authentication_boundary(&mut events);
                if a.runtime.take_logout_requested() {
                    finishing = true;
                }
            }
            if let Some(a) = active.as_mut() {
                // Runtime callback processing continues during retirement: a late
                // raster completion must reach the driver, never a replacement pool.
                if let Err(error) =
                    flutter_desktop::process_flutter_event_batch(&mut a.runtime, &mut events)
                {
                    failure.get_or_insert(error.to_string());
                    finishing = true;
                }
                sampled.drain(&mut events, now)?;
                if a.runtime.take_logout_requested() {
                    finishing = true;
                }
                if finishing
                    || events.flutter_reload_requested
                    || !client.as_ref().unwrap().connected()
                {
                    stop(
                        a,
                        &mut events,
                        &mut input_stream,
                        &mut input_seat,
                        client.as_mut().unwrap(),
                        &mut pointer_capture,
                    );
                }
                if !a.driver.stopped() && client.as_ref().unwrap().connected() {
                    #[cfg(feature = "anland-runtime-v2")]
                    for app_id in std::mem::take(&mut events.control.repaint_apps) {
                        a.driver.update_textures(a.runtime.app_texture_ids(&app_id));
                    }
                    // Poll every bounded outer iteration, including maintenance:
                    // partial headers/payloads must expire even without a new edge.
                    let c = client.as_mut().unwrap();
                    let output = c.output()?;
                    let mut flush_clients = false;
                    let locked = events.secure_session_locked();
                    events.clipboard.set_locked(locked);
                    if locked {
                        pending_service_context = None;
                    }
                    for index in 0..64 {
                        // Capture BEFORE the first header byte. Both public
                        // partial headers and local payloads retain this context;
                        // a genuinely idle poll allows a fresh target next time.
                        if !input_stream.has_pending(c) {
                            let target = if events.secure_session_locked() {
                                None
                            } else {
                                events
                                    .wayland
                                    .as_ref()
                                    .and_then(|frontend| frontend.consumer_text_target())
                            };
                            pending_service_context = if events.secure_session_locked() {
                                None
                            } else {
                                Some((
                                    target,
                                    events.clipboard.selection_epoch(),
                                    events.clipboard.revision(),
                                ))
                            };
                        }
                        match input_stream.next(c, now) {
                            Ok(Some(Message::Refresh(refresh))) => {
                                synchronize_refresh(
                                    a,
                                    &mut events,
                                    &publisher,
                                    refresh,
                                    now,
                                    &scale,
                                )?;
                            }
                            Ok(Some(Message::Input(raw))) => {
                                let time = now
                                    .duration_since(started)
                                    .as_micros()
                                    .min(u64::MAX as u128)
                                    as u64;
                                if let Some(event) =
                                    input_seat.translate(raw, (output.width, output.height), time)
                                {
                                    flush_clients |= wayland_frontend::process_routed_input_event(
                                        &mut events,
                                        event,
                                        "anland-consumer",
                                    );
                                }
                                while let Some(event) = input_seat.pop_pending() {
                                    flush_clients |= wayland_frontend::process_routed_input_event(
                                        &mut events,
                                        event,
                                        "anland-consumer",
                                    );
                                }
                            }
                            Ok(Some(Message::Clipboard(text))) => {
                                let current = events.clipboard.selection_epoch();
                                if !events.secure_session_locked()
                                    && pending_service_context.as_ref().is_some_and(
                                        |(_, epoch, revision)| {
                                            *epoch == current
                                                && *revision == events.clipboard.revision()
                                        },
                                    )
                                    && !clipboard_sync.is_echo(current, &text)
                                {
                                    match events.clipboard.set_consumer_text(&text) {
                                        Ok(epoch) => {
                                            clipboard_sync.imported(epoch);
                                            let actions = events.clipboard.take_actions();
                                            wayland_frontend::apply_clipboard_actions(
                                                &mut events,
                                                actions,
                                            );
                                            flush_clients = true;
                                        }
                                        Err(error) => debug!(?error, "consumer clipboard rejected"),
                                    }
                                }
                            }
                            Ok(Some(Message::Text(text))) => {
                                if !events.secure_session_locked()
                                    && let Some((Some(target), _, _)) =
                                        pending_service_context.as_ref()
                                    && let Some(delivery) =
                                        events.wayland.as_mut().and_then(|frontend| {
                                            frontend.commit_consumer_text(target, &text)
                                        })
                                {
                                    use crate::wayland_frontend::ConsumerTextDelivery;
                                    match delivery {
                                        ConsumerTextDelivery::Wayland => flush_clients = true,
                                        ConsumerTextDelivery::Keyboard => {
                                            flush_clients |=
                                                wayland_frontend::dispatch_consumer_keyboard_text(
                                                    &mut events,
                                                    target,
                                                    &text,
                                                );
                                        }
                                        ConsumerTextDelivery::Flutter {
                                            generation,
                                            lifecycle,
                                            client_id,
                                        } => {
                                            let _ = a.runtime.dispatch_consumer_text_to_flutter(
                                                generation, lifecycle, client_id, &text,
                                            )?;
                                        }
                                    }
                                } else {
                                    warn!(
                                        bytes = text.len(),
                                        "consumer text rejected: no eligible target, stale focus, IME ownership, grab or secure session"
                                    );
                                }
                            }
                            Ok(Some(Message::Resources { kind, invalid, fds })) => {
                                if let Err(error) = c.resources(kind, invalid, fds) {
                                    warn!(%error, "Anland resource handoff failed; retiring output");
                                    sources.invalidate();
                                    stop(
                                        a,
                                        &mut events,
                                        &mut input_stream,
                                        &mut input_seat,
                                        c,
                                        &mut pointer_capture,
                                    );
                                    lifetime.stop(now);
                                    break;
                                }
                            }
                            Ok(None) => break,
                            Err(error) => {
                                warn!(%error, "Anland input session invalid; retiring output");
                                sources.invalidate();
                                stop(
                                    a,
                                    &mut events,
                                    &mut input_stream,
                                    &mut input_seat,
                                    c,
                                    &mut pointer_capture,
                                );
                                lifetime.stop(now);
                                break;
                            }
                        }
                        // Capture changes can occur in this input batch (escape,
                        // focus transfer, client click). Send before polling the
                        // next message; do not wait for a render/ACK boundary.
                        if let Err(error) = synchronize_pointer_capture(
                            &events,
                            c,
                            &mut pointer_capture,
                            !a.driver.stopped(),
                        ) {
                            warn!(%error, "Anland capture send failed; retiring output");
                            sources.invalidate();
                            stop(
                                a,
                                &mut events,
                                &mut input_stream,
                                &mut input_seat,
                                c,
                                &mut pointer_capture,
                            );
                            lifetime.stop(now);
                            break;
                        }
                        if index == 63 {
                            immediate = true;
                        }
                    }
                    if flush_clients && let Some(frontend) = events.wayland.as_mut() {
                        frontend.display_handle.flush_clients()?;
                    }
                    // Input/capture failure may already have stopped this driver.
                    // Continue draining output only while this session is active.
                    if !a.driver.stopped() {
                        send_service_output(
                            c,
                            &mut clipboard_sync,
                            None,
                            &mut lifetime,
                            now,
                            |c, error| {
                                warn!(%error, "Anland queued output send failed on detached session; retiring output");
                                sources.invalidate();
                                stop(
                                    a,
                                    &mut events,
                                    &mut input_stream,
                                    &mut input_seat,
                                    c,
                                    &mut pointer_capture,
                                );
                            },
                        )?;
                    }
                }
                let mut drive_now = Instant::now();
                if !a.driver.stopped() && client.as_ref().unwrap().connected() {
                    // Consumer routing above may have just queued shell input.
                    // Use the runtime's bounded batch before scene synchronization/render.
                    if events.flutter_input.has_pending() {
                        a.runtime.process_input_batch(&mut events.flutter_input)?;
                    }
                    synchronize_authentication_boundary(&mut events);
                    let capture_allowed = !events.secure_session_locked();
                    if let Some(frontend) = events.wayland.as_mut() {
                        frontend.process_pending_dmabufs(&mut graphics.renderer)?;
                        if capture_allowed {
                            frontend.process_toplevel_screencopies(&mut graphics.renderer)?;
                        }
                    }
                    // Work-area changes are interactive state; apply them
                    // before periodic services can switch native presentation.
                    super::super::flutter_settings_sync::synchronize_system_bar_configuration(
                        &mut a.runtime,
                        &mut events,
                        launcher.as_mut(),
                    );
                    flutter_desktop::synchronize_periodic_shell_services(
                        &mut a.runtime,
                        &mut events,
                    )?;
                    if launcher
                        .as_mut()
                        .unwrap()
                        .synchronize_ui_development(&mut a.runtime)?
                    {
                        events.flutter_reload_requested = true;
                    }
                    process_scale_controls(
                        a,
                        &mut events,
                        &publisher,
                        &mut topology,
                        &mut scale,
                        now,
                    )?;
                    synchronize_flutter_window_management(&mut a.runtime, &mut events)?;
                    // Coalesce mailbox pixels until an output tick instead of
                    // cloning buffers/locking the raster texture map per commit.
                    // Metadata/unmaps, captures and exact-frame clients keep
                    // their existing immediate ingest and deadline semantics.
                    let buffer_sync_due = !coalesce_buffer_commits
                        || a.driver.buffer_sync_due(Instant::now())
                        || a.captures.requested(&events, a.plan.pool.output)
                        || events
                            .wayland
                            .as_ref()
                            .is_some_and(|frontend| frontend.has_frame_timeline_clients());
                    if buffer_sync_due || events.scene_sync.pending_metadata_revision().is_some() {
                        synchronize_flutter_scene(&publisher, &mut a.runtime, &mut events)?;
                    }
                    synchronize_flutter_input_layout(&mut a.runtime, &mut events)?;
                    synchronize_wayland_cursor(&mut a.runtime, &mut events)?;
                    a.captures.service_requests(
                        &mut a.runtime,
                        &a._imports,
                        &a.plan,
                        &mut graphics,
                        &mut events,
                    )?;
                    // Unmapped windows legitimately remain unpublished until a
                    // first buffer arrives. They cannot veto the existing desktop.
                    #[cfg(feature = "anland-runtime-v2")]
                    let window_events_empty = events.window_events.pending.is_empty();
                    #[cfg(not(feature = "anland-runtime-v2"))]
                    let window_events_empty = events.pending_window_events.is_empty();
                    let scene_ready = !events.replacement_flutter_rehydration_pending
                        && events.scene_sync.pending_metadata_revision().is_none()
                        && events.pending_shell_actions.is_empty()
                        && window_events_empty;
                    a.runtime.synchronize_scene_recovery(
                        events.scene_sync.metadata_revision(),
                        scene_ready,
                    )?;
                    let export = if !events.secure_session_locked() {
                        let epoch = events.clipboard.selection_epoch();
                        if clipboard_sync.needs_export(epoch) {
                            events
                                .clipboard
                                .consumer_export_text()
                                .map(|text| (epoch, text))
                        } else {
                            None
                        }
                    } else {
                        None
                    };
                    // queue success is NOT delivery. Public partial-output state
                    // controls the writable watcher installed below. A confirmed
                    // send fallback schedules retirement instead of leaving run().
                    send_service_output(
                        client.as_mut().unwrap(),
                        &mut clipboard_sync,
                        export.as_ref().map(|(epoch, text)| (*epoch, text.as_str())),
                        &mut lifetime,
                        now,
                        |c, error| {
                            warn!(%error, "Anland clipboard/output send failed on detached session; retiring output");
                            sources.invalidate();
                            stop(
                                a,
                                &mut events,
                                &mut input_stream,
                                &mut input_seat,
                                c,
                                &mut pointer_capture,
                            );
                        },
                    )?;
                    // Services above may cross an output edge after the early
                    // scene check. Latch one time for the last buffer ingest
                    // and drive, so that edge cannot render an avoidably stale
                    // texture and postpone the newest pixels by another tick.
                    drive_now = Instant::now();
                    if coalesce_buffer_commits
                        && a.driver.buffer_sync_due(drive_now)
                        && events.scene_sync.pending_buffer_revision().is_some()
                    {
                        synchronize_flutter_scene(&publisher, &mut a.runtime, &mut events)?;
                    }
                    let updates = a.runtime.take_output_updates();
                    for (_, textures) in &updates {
                        a.driver.update_textures(textures.iter().copied());
                    }
                    a.runtime.recycle_output_updates(updates);
                }
                let capture_requested =
                    !a.driver.stopped() && a.captures.requested(&events, a.plan.pool.output);
                let capture_running = !a.driver.stopped();
                // Shared with the last ingest above; do not observe a later
                // edge here without first ingesting its coalesced client pixels.
                let now = drive_now;
                let progress = a.driver.drive(
                    &mut CapturingRenderer {
                        runtime: &mut a.runtime,
                        captures: &mut a.captures,
                        requested: capture_requested,
                        running: capture_running,
                    },
                    client.as_mut().unwrap(),
                    now,
                    64,
                    fence_signaled,
                );
                if audit_frames {
                    audit_ticks += u64::from(progress.frame_tick.is_some());
                    audit_renders += u64::from(progress.render_requested);
                    audit_vsync += u64::from(progress.vsync_dispatched);
                    audit_presented += progress
                        .outcomes
                        .iter()
                        .filter(|outcome| {
                            matches!(outcome, super::submission::Outcome::Presented { .. })
                        })
                        .count() as u64;
                    if now.saturating_duration_since(audit_since) >= Duration::from_secs(1) {
                        info!(
                            clock_ticks = audit_ticks,
                            raster_requests = audit_renders,
                            flutter_vsync = audit_vsync,
                            consumer_ack = audit_presented,
                            elapsed_ms = now.saturating_duration_since(audit_since).as_millis(),
                            "Anland frame statistics"
                        );
                        audit_since = now;
                        audit_ticks = 0;
                        audit_renders = 0;
                        audit_vsync = 0;
                        audit_presented = 0;
                    }
                }
                // The facade may also publish OUTPUT_CHANGED without a seat notification.
                for change in &progress.state_changes {
                    if let super::client::Event::Output(output) = change
                        && !a.driver.stopped()
                        && output.width == a.plan.target.width
                        && output.height == a.plan.target.height
                    {
                        synchronize_refresh(
                            a,
                            &mut events,
                            &publisher,
                            output.refresh_mhz,
                            now,
                            &scale,
                        )?;
                    }
                }
                if let Some(frontend) = events.wayland.as_mut() {
                    for tick in &progress.submitted_ticks {
                        frontend.external_frame_submitted(tick.output, tick.sequence);
                    }
                }
                // Complete already-drained ACK/drop outcomes before advancing
                // local cadence, so a delayed ACK cannot lose its grant.
                for item in &progress.outcomes {
                    if matches!(item, super::submission::Outcome::Presented { .. }) {
                        frames += 1;
                    }
                    if let Some(frontend) = events.wayland.as_mut() {
                        deliver_feedback(frontend, item)?;
                    }
                    a.captures
                        .complete(item, &mut a.runtime, !a.driver.stopped())?;
                }
                if !finishing && !a.driver.stopped() && client.as_ref().unwrap().connected() {
                    a.captures.process_ready(
                        &mut a.runtime,
                        &a._imports,
                        &a.plan,
                        &mut graphics,
                        &mut events,
                        now,
                    )?;
                }
                if !finishing
                    && !a.driver.stopped()
                    && client.as_ref().unwrap().connected()
                    && let Some(frontend) = events.wayland.as_ref()
                {
                    desktop.activate(
                        progress.outcomes.iter().any(|item| {
                            matches!(item, super::submission::Outcome::Presented { .. })
                        }),
                        &a.runtime,
                        frontend,
                        Some(control.socket_path()),
                        launcher.as_ref().unwrap(),
                    );
                }
                if let Some(tick) = progress.frame_tick
                    && let Some(frontend) = events.wayland.as_mut()
                {
                    frontend.frame_tick(tick)?;
                }
                for error in progress.errors {
                    warn!(%error, "Anland output stopped; retiring native pool");
                }
                immediate |= progress.more && !a.driver.stopped();
                if a.driver.stopped() || !client.as_ref().unwrap().connected() {
                    stop(
                        a,
                        &mut events,
                        &mut input_stream,
                        &mut input_seat,
                        client.as_mut().unwrap(),
                        &mut pointer_capture,
                    );
                    lifetime.stop(now);
                }
            } else if finishing {
                if replacing.is_some() {
                    fail_closed = true;
                    return Err(
                        "shutdown requested during outstanding raster replacement; retain owners"
                            .into(),
                    );
                }
                if !sampled.pending.is_empty() {
                    continue;
                }
                if let Some(mut a) = parked.take() {
                    if let Err(error) = a.runtime.shutdown() {
                        std::mem::forget(a.captures);
                        std::mem::forget(a.driver);
                        std::mem::forget(a._imports);
                        fail_closed = true;
                        return Err(format!("parked Flutter shutdown failed: {error}").into());
                    }
                }
                break;
            }

            if active.as_ref().is_none_or(|a| a.driver.stopped()) {
                reject_unavailable_scale_requests(&mut events);
                events.pending_screenshot_selection = None;
                if let Some(frontend) = events.wayland.as_mut() {
                    frontend.fail_all_screencopies();
                }
            }
            // Wayland requests, shell/lock changes and constraint removal can
            // happen without input or rendering. Synchronize every bounded loop.
            if let Some(c) = client.as_mut() {
                let mut running = active.as_ref().is_some_and(|a| !a.driver.stopped()) && !finishing;
                if !c.connected() {
                    camera_requested = false;
                } else if running && !camera_requested {
                    match c.request_resources(ffi::SERVICE_TYPE_CAMERA) {
                        Ok(()) => camera_requested = true,
                        Err(error) => {
                            warn!(%error, "Anland camera resource request failed; retiring output");
                            sources.invalidate();
                            camera_requested = false;
                            if let Some(a) = active.as_mut() {
                                stop(
                                    a,
                                    &mut events,
                                    &mut input_stream,
                                    &mut input_seat,
                                    c,
                                    &mut pointer_capture,
                                );
                                lifetime.stop(now);
                            } else {
                                c.drop_session();
                                pointer_capture.reset_session();
                                scheduling.reset_session();
                            }
                            running = false;
                        }
                    }
                }
                if let Err(error) =
                    synchronize_pointer_capture(&events, c, &mut pointer_capture, running)
                {
                    warn!(%error, "Anland capture send failed; retiring output");
                    sources.invalidate();
                    if let Some(a) = active.as_mut() {
                        stop(
                            a,
                            &mut events,
                            &mut input_stream,
                            &mut input_seat,
                            c,
                            &mut pointer_capture,
                        );
                        lifetime.stop(now);
                    } else {
                        c.drop_session();
                        pointer_capture.reset_session();
                    }
                    running = false;
                }
                if let Err(error) = synchronize_scheduling(&events, c, &mut scheduling, running) {
                    warn!(%error, "Anland scheduling update failed; retiring output");
                    sources.invalidate();
                    if let Some(a) = active.as_mut() {
                        stop(
                            a,
                            &mut events,
                            &mut input_stream,
                            &mut input_seat,
                            c,
                            &mut pointer_capture,
                        );
                        lifetime.stop(now);
                    } else {
                        c.drop_session();
                        pointer_capture.reset_session();
                        scheduling.reset_session();
                    }
                }
            }
            let (daemon, connected, retired, empty) =
                client.as_ref().map_or((false, false, true, true), |c| {
                    (
                        c.daemon_alive(),
                        c.connected(),
                        active.as_ref().is_none_or(|a| {
                            a.captures.can_retire() && a.driver.retired(&a.runtime, now)
                        }),
                        c.events_pending() == 0 && sampled.pending.is_empty(),
                    )
                });
            // Never reopen the public session under a staged raster swap.
            let action = if staged_plan.is_some() || replacing.is_some() {
                Action::Wait
            } else {
                lifetime.poll(now, daemon, connected, retired, empty)
            };
            match action {
                Action::Drop => {
                    sources.invalidate();
                    client.as_mut().unwrap().drop_session();
                    pointer_capture.reset_session();
                    scheduling.reset_session();
                    camera_requested = false;
                    scale.cancel_preview();
                    publisher.publish(scale.state(None));
                }
                Action::Retired => {
                    sources.invalidate();
                    if warm_supported && !finishing && !events.flutter_reload_requested {
                        // Retired proves public events, consumer and sampled fences
                        // are drained. Raster swap will supply the final thread proof.
                        let a = active.as_mut().unwrap();
                        a.runtime.set_outputs_visible(false)?;
                        parked = active.take();
                        info!("Anland output retired; retaining Flutter engine and scene");
                    } else {
                        if let Some(mut a) = active.take() {
                            if let Err(error) = a.runtime.shutdown() {
                                std::mem::forget(a.captures);
                                std::mem::forget(a.driver);
                                std::mem::forget(a._imports);
                                fail_closed = true;
                                return Err(format!(
                                    "Flutter shutdown failed; retaining GPU owners: {error}"
                                )
                                .into());
                            }
                        }
                        events.flutter_events.clear();
                        events.flutter_reload_requested = false;
                    }
                    lifetime.restart(now);
                    if finishing {
                        break;
                    }
                }
                Action::FailClosed => {
                    fail_closed = true;
                    return Err("Anland native pool could not retire safely; output remains stopped, no reconnect".into());
                }
                Action::Reconnect | Action::Reopen => {
                    sources.invalidate();
                    input_stream.reset();
                    release_tablet_devices(&mut events, &mut input_seat);
                    input_seat.reset();
                    clipboard_sync.reset_session();
                    pointer_capture.reset_session();
                    scheduling.reset_session();
                    camera_requested = false;
                    pending_service_context = None;
                    if let Some(c) = client.as_mut() {
                        let result = if daemon {
                            c.reconnect()
                        } else {
                            c.reopen(endpoint.as_deref())
                        };
                        if let Err(error) = result {
                            debug!(%error, "waiting for Anland daemon/consumer");
                        }
                    } else {
                        match Client::open(endpoint.as_deref()) {
                            Ok(c) => client = Some(c),
                            Err(error) => debug!(%error, "waiting for Anland daemon"),
                        }
                    }
                }
                Action::Wait => {}
            }
            if active.is_none()
                && sampled.pending.is_empty()
                && events.sampled_buffer_releases.is_empty()
                && let Some(c) = client.as_mut()
            {
                // There can be target/output events but never old pool releases:
                // the old driver drained every event before it was destroyed.
                let _ = c.pump();
                for _ in 0..64 {
                    if c.next_event()?.is_none() && c.events_pending() == 0 {
                        break;
                    }
                }
                // Consumer HELLO can contain refresh=0 until Java reports the
                // display rate on the input stream. Waiting for an Active output
                // before reading that stream would make output creation depend
                // on itself. Drain only while metadata/partial framing is pending.
                let needs_initial_refresh =
                    c.connected() && c.output().is_ok_and(|output| output.refresh_mhz == 0);
                if needs_initial_refresh && !waiting_initial_refresh {
                    info!(
                        "Anland consumer connected; waiting for initial refresh rate without dropping session"
                    );
                }
                waiting_initial_refresh = needs_initial_refresh;
                if c.connected() && (input_stream.has_pending(c) || needs_initial_refresh) {
                    pending_service_context = None;
                    match input_stream.bootstrap_refresh(c, now) {
                        Ok(Some(refresh_mhz)) => {
                            info!(refresh_mhz, "Anland initial consumer refresh received");
                        }
                        Ok(None) => {}
                        Err(error) => {
                            // next() already retired the failed transport; do not
                            // forward any partially decoded input after reconnect.
                            sources.invalidate();
                            input_seat.reset();
                            warn!(%error, "Anland startup metadata stream failed; retrying session");
                        }
                    }
                }
                if (c.connected() && c.events_pending() == 0 && !input_stream.has_pending(c))
                    || staged_plan.is_some()
                    || replacing.is_some()
                {
                    'prepare: {
                        let prepared = if let Some(plan) = staged_plan.take() {
                            Ok((plan, staged_imports.take().unwrap()))
                        } else {
                            prepare_output(c, &topology, scale.scale_120)
                        };
                        match prepared {
                            Ok((plan, imports)) => {
                                staged_imports = Some(imports);
                                if launcher.is_none() {
                                    #[cfg(feature = "anland-runtime-v2")]
                                    let mut frontend = WaylandFrontend::new(
                                        &mut event_loop,
                                        &plan.snapshot,
                                        None,
                                        "anland",
                                        Some(graphics.fd()),
                                        graphics.fd(),
                                        crate::wayland_frontend::FrontendConfig {
                                            xwayland_enabled: options.xwayland,
                                            work_area: options.work_area.clone(),
                                            scrolling_layout_axes: options
                                                .scrolling_layout_axes
                                                .clone(),
                                            settings: settings.take().unwrap(),
                                            shortcuts: shortcuts.take().unwrap(),
                                        },
                                    )?;
                                    #[cfg(not(feature = "anland-runtime-v2"))]
                                    let mut frontend = WaylandFrontend::new(
                                        &mut event_loop,
                                        &plan.snapshot,
                                        None,
                                        "anland",
                                        Some(graphics.fd()),
                                        graphics.fd(),
                                        options.xwayland,
                                        options.work_area.clone(),
                                        options.scrolling_layout_axes.clone(),
                                        settings.take().unwrap(),
                                        shortcuts.take().unwrap(),
                                    )?;
                                    frontend.defer_implicit_dmabuf_wait =
                                        defer_implicit_dmabuf_wait;
                                    frontend.init_renderer(&mut graphics.renderer)?;
                                    launcher = Some(FlutterLauncher::new(
                                        FlutterLaunchConfiguration {
                                            bundle,
                                            #[cfg(not(feature = "anland-runtime-v2"))]
                                            renderer_backend: options.flutter_renderer,
                                            offscreen_blit: options.flutter_offscreen_blit,
                                            debug_bundle: options.flutter_debug_bundle.clone(),
                                            ui_workspace: options.flutter_ui_workspace.clone(),
                                        },
                                        sender.clone(),
                                        Some(frontend.socket_name().to_os_string()),
                                        frontend.xdisplay_name(),
                                        Some(control.socket_path_os_string()),
                                        options.work_area.clone(),
                                        options.start_locked,
                                    )?);
                                    staged_frontend = Some(frontend);
                                }
                                let retained = parked.as_ref().is_some_and(|old| {
                                    retain_runtime(
                                        replacement_rejected,
                                        events.flutter_reload_requested,
                                        replacing.is_some(),
                                        old.plan.pool.output == plan.pool.output
                                            && old.plan.pool.view == plan.pool.view
                                            && old.plan.pool.configuration
                                                == plan.pool.configuration
                                            && old.plan.pool.size == plan.pool.size
                                            && old.plan.atlas.pixel_size == plan.atlas.pixel_size,
                                    )
                                });
                                // A retained engine also retains screenshot request IDs,
                                // so delayed shell replies cannot alias a new selection.
                                let mut captures = CaptureState::default();
                                let driver = if retained {
                                    parked
                                        .as_ref()
                                        .unwrap()
                                        .driver
                                        .replacement(plan.pool, plan.refresh_millihz)?
                                } else {
                                    OutputDriver::new(plan.pool, plan.refresh_millihz)?
                                };
                                let runtime = if retained {
                                    let since = *replacement_started.get_or_insert(Instant::now());
                                    if replacing.is_none() && Instant::now() >= replacement_retry {
                                        replacing = Some(
                                            staged_imports.as_ref().unwrap().begin_rebind_runtime(
                                                &parked.as_ref().unwrap().runtime,
                                            )?,
                                        );
                                    }
                                    let status = match poll_replacement(
                                        replacing.as_ref(),
                                        since,
                                        Instant::now(),
                                    ) {
                                        Ok(status) => status,
                                        Err(error) => {
                                            fail_closed = true;
                                            return Err(error.into());
                                        }
                                    };
                                    match status {
                                        ReplacementProgress::Completed => {
                                            replacing = None;
                                            parked
                                                .as_mut()
                                                .unwrap()
                                                .runtime
                                                .finish_external_pool_replacement(
                                                    &plan.snapshot,
                                                    &plan.atlas,
                                                    plan.refresh_millihz,
                                                )?;
                                            replacement_started = None;
                                            info!(
                                                generation = plan.pool.generation,
                                                "Anland pool asynchronously replaced on retained Flutter engine"
                                            );
                                            let old = parked.take().unwrap();
                                            captures = old.captures;
                                            old.runtime
                                        }
                                        ReplacementProgress::Rejected => {
                                            replacing = None;
                                            replacement_rejected = true;
                                            warn!(
                                                "Anland replacement rejected before swap; cold recovery after completed raster callback"
                                            );
                                            staged_plan = Some(plan);
                                            immediate = true;
                                            break 'prepare;
                                        }
                                        ReplacementProgress::Busy => {
                                            replacing = None;
                                            replacement_retry =
                                                Instant::now() + Duration::from_millis(50);
                                            staged_plan = Some(plan);
                                            break 'prepare;
                                        }
                                        ReplacementProgress::Pending => {
                                            staged_plan = Some(plan);
                                            break 'prepare;
                                        }
                                    }
                                } else {
                                    if replacing.is_some() {
                                        fail_closed = true;
                                        return Err("cannot cold-recover while raster replacement is outstanding".into());
                                    }
                                    if let Some(mut old) = parked.take() {
                                        if let Err(error) = old.runtime.shutdown() {
                                            std::mem::forget(old.captures);
                                            std::mem::forget(old.driver);
                                            std::mem::forget(old._imports);
                                            fail_closed = true;
                                            return Err(format!(
                                                "cold fallback shutdown failed: {error}"
                                            )
                                            .into());
                                        }
                                        events.flutter_events.clear();
                                        events.flutter_reload_requested = false;
                                    }
                                    replacement_rejected = false;
                                    replacement_started = None;
                                    staged_imports.as_ref().unwrap().start_runtime(
                                        launcher.as_mut().unwrap(),
                                        graphics.renderer.egl_context(),
                                        &plan.snapshot,
                                        &plan.atlas,
                                        plan.refresh_millihz,
                                    )?
                                };
                                active = Some(Active {
                                    captures,
                                    driver,
                                    runtime,
                                    _imports: staged_imports.take().unwrap(),
                                    plan,
                                });
                                let plan = &active.as_ref().unwrap().plan;
                                if let Some(frontend) = staged_frontend.take() {
                                    let lifecycle = std::mem::take(&mut events.lifecycle);
                                    #[cfg(feature = "anland-runtime-v2")]
                                    let reopen_color_tuning = events.reopen_color_tuning;
                                    events = flutter_desktop::initialize(
                                        &mut event_loop,
                                        &active.as_ref().unwrap().runtime,
                                        Some(frontend),
                                        plan.atlas.pixel_size,
                                        Arc::new(Mutex::new(
                                            gamma_control::GammaController::default(),
                                        )),
                                        Some(publisher.clone()),
                                        Some(portal.publisher()),
                                    )?;
                                    events.lifecycle = lifecycle;
                                    #[cfg(feature = "anland-runtime-v2")]
                                    {
                                        events.reopen_color_tuning |= reopen_color_tuning;
                                    }
                                    events.replacement_flutter_rehydration_pending = true;
                                } else {
                                    events
                                        .wayland
                                        .as_mut()
                                        .unwrap()
                                        .update_topology(&plan.snapshot)?;
                                    if retained {
                                        // Keep Flutter focus/client IDs, widget state and
                                        // scene leases. Only refresh the native projection.
                                        events.flutter_input.resize(plan.atlas.pixel_size);
                                        events.synchronize_flutter_pointer_position();
                                    } else {
                                        events.begin_replacement_flutter_generation(
                                            plan.atlas.pixel_size,
                                        );
                                    }
                                }
                                publisher.publish(scale.state(Some(plan)));
                                topology = plan.topology.clone();
                                info!(
                                    generation = plan.pool.generation,
                                    slots = plan.pool.count,
                                    width = plan.target.width,
                                    height = plan.target.height,
                                    "Anland consumer target pool activated"
                                );
                                events.install_workspace_snapshot(
                                    &mut active.as_mut().unwrap().runtime,
                                )?;
                                active
                                    .as_mut()
                                    .unwrap()
                                    .runtime
                                    .begin_scene_recovery(Instant::now())?;
                                if retained {
                                    active.as_mut().unwrap().runtime.set_outputs_visible(true)?;
                                }
                                immediate = true;
                            }
                            Err(error) if error.kind() == io::ErrorKind::WouldBlock => {}
                            Err(error) => {
                                warn!(%error, "Anland target import rejected; retrying fresh session");
                                sources.invalidate();
                                c.drop_session();
                            }
                        }
                    }
                }
            }
            let bootstrap_input = active.is_none()
                && client.as_ref().is_some_and(|c| {
                    c.connected()
                        && (input_stream.has_pending(c)
                            || c.output().is_ok_and(|output| output.refresh_mhz == 0))
                });
            sources.watch_input(
                active.as_ref().is_some_and(|a| !a.driver.stopped()) || bootstrap_input,
            );
            if let Some(c) = client.as_ref() {
                sources.refresh(c)?;
            }
            // Ensure callbacks cannot starve on a large bounded batch.
            immediate |= !events.flutter_events.is_empty() || events.flutter_input.has_pending();
            use std::os::fd::AsFd;
            let mut fences: Vec<_> = sampled
                .pending
                .iter()
                .map(|(fd, _, _)| fd.as_fd())
                .collect();
            if let Some(a) = active.as_ref() {
                fences.extend(a.driver.release_fences());
            }
            let capture_fences = active
                .as_ref()
                .map(|a| a.captures.fences())
                .transpose()?
                .unwrap_or_default();
            fences.extend(capture_fences.iter().map(|fd| fd.as_fd()));
            sources.refresh_fences(fences)?;
        }
        Ok(())
    }));
    desktop.stop();
    sources.invalidate();
    // An exceptional exit may still have a raster worker or unsignaled fence.
    // Keep ALL reachable owners alive rather than destroy/reuse unproven buffers.
    if fail_closed || active.is_some() || !matches!(&outcome, Ok(Ok(()))) {
        if let Some(a) = active.as_mut() {
            stop(
                a,
                &mut events,
                &mut input_stream,
                &mut input_seat,
                client.as_mut().unwrap(),
                &mut pointer_capture,
            );
        }
        if let Some(c) = client.as_mut() {
            let _ = synchronize_pointer_capture(&events, c, &mut pointer_capture, false);
            c.drop_session();
        }
        std::mem::forget(sampled);
        std::mem::forget(staged_imports);
        std::mem::forget(staged_frontend);
        std::mem::forget(launcher);
        std::mem::forget(active);
        std::mem::forget(parked);
        std::mem::forget(graphics);
        std::mem::forget(events);
        std::mem::forget(event_loop);
        std::mem::forget(client);
        error!("Anland exceptional teardown retained native owners; no further output/reconnect");
    } else {
        // Sample-release callbacks retain client texture guards. Remove all
        // callbacks and stop frontend capture workers BEFORE EGL/GBM/device.
        drop(sources);
        drop(events);
        drop(event_loop);
        drop(graphics);
        drop(client);
    }
    match outcome {
        Ok(result) => result?,
        Err(payload) => resume_unwind(payload),
    }
    if let Some(error) = failure {
        return Err(error.into());
    }
    Ok(())
}
