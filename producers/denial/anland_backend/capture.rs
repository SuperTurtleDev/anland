//! Connect the shared capture services to real Anland presentations. No KMS
//! completion, CPU framebuffer copy, or permanent display-buffer pin is invented.
use super::capture_queue::{CaptureQueue, CaptureSource};
use super::client::Target;
use super::display::NativeRenderer;
use super::graphics::RenderDevice;
use super::native::ImportedPool;
use super::submission::{NativeOutput, Outcome, Pool};
use super::topology::OutputPlan;
use crate::RuntimeState;
use crate::flutter_runtime::system_command::ScreenshotRequest;
use crate::flutter_runtime::{FlutterRuntime, OutputBufferLease, ReadyOutputFrame};
use crate::frame_request::{OutputFrameRequest, RenderDispatch};
use crate::screenshot::ScreenshotManager;
use crate::wayland_frontend::OutputCompositeSource;
use denial_core::topology::OutputId;
use smithay::backend::allocator::Buffer;
use std::cell::RefCell;
use std::error::Error;
use std::io;
use std::time::Instant;
use tracing::{debug, warn};

#[derive(Default)]
pub(crate) struct CaptureState {
    sources: RefCell<CaptureQueue<OutputBufferLease>>,
    manager: Option<ScreenshotManager>,
    live_request: Option<ScreenshotRequest>,
    teardown_failed: bool,
}

impl CaptureState {
    pub fn requested(&self, events: &RuntimeState, output: OutputId) -> bool {
        !events.secure_session_locked()
            && (self.live_request.is_some()
                || events
                    .wayland
                    .as_ref()
                    .is_some_and(|frontend| frontend.has_pending_screencopy_for_output(output)))
    }

    fn ensure_writer(&mut self, events: &RuntimeState) -> bool {
        if self.manager.is_none() {
            match ScreenshotManager::new(events.clipboard.clone()) {
                Ok(manager) => self.manager = Some(manager),
                Err(error) => {
                    warn!(%error, "Anland screenshot writer unavailable");
                    return false;
                }
            }
        }
        true
    }

    fn cancel_selection(
        &mut self,
        runtime: &mut FlutterRuntime,
        request_id: Option<u64>,
        notify: bool,
    ) -> Result<(), Box<dyn Error>> {
        if let Some(manager) = self.manager.as_mut()
            && let Some(request_id) = manager.cancel_selection(runtime, request_id)?
        {
            self.sources.get_mut().cancel_screenshot(request_id);
            if notify {
                runtime.send_screenshot_action(
                    crate::wire::ShellAction::ScreenshotDone,
                    request_id,
                    None,
                )?;
            }
        }
        Ok(())
    }

    /// Consume only actions the user actually requested. Merely constructing this
    /// owner neither creates UI state nor takes a screenshot.
    pub fn service_requests(
        &mut self,
        runtime: &mut FlutterRuntime,
        imports: &ImportedPool,
        plan: &OutputPlan,
        graphics: &mut RenderDevice,
        events: &mut RuntimeState,
    ) -> Result<(), Box<dyn Error>> {
        let prepared = runtime.take_screenshot_prepared();
        let cancelled = runtime.take_screenshot_cancelled();
        let request = runtime.take_screenshot_requested();
        if let Some(request_id) = cancelled {
            self.cancel_selection(runtime, Some(request_id.get()), true)?;
        }
        if events.secure_session_locked() {
            events.pending_screenshot_selection = None;
            self.live_request = None;
            self.cancel_selection(runtime, None, true)?;
            self.sources.get_mut().clear();
            if let Some(frontend) = events.wayland.as_mut() {
                frontend.fail_all_screencopies();
            }
            return Ok(());
        }
        if self.manager.as_ref().is_some_and(|manager| {
            manager.request_id().is_some()
                && (manager.topology_epoch() != Some(plan.atlas.topology_epoch)
                    || manager.target_output() != Some(plan.pool.output))
        }) {
            self.cancel_selection(runtime, None, true)?;
        }
        if let Some(output) = events.pending_screenshot_selection.take()
            && output == plan.pool.output
            && self.ensure_writer(events)
        {
            let modifier = imports.buffer(0)?.format().modifier;
            match self.manager.as_mut().unwrap().begin_selection(
                &mut graphics.capture_allocator,
                output,
                plan.atlas.clone(),
                modifier,
            ) {
                Ok(Some(request_id)) => {
                    if let Err(error) = runtime.send_screenshot_action(
                        crate::wire::ShellAction::ScreenshotRegion,
                        request_id,
                        None,
                    ) {
                        self.cancel_selection(runtime, Some(request_id), false)?;
                        return Err(error);
                    }
                }
                Ok(None) => debug!("ignored repeated Anland screenshot selection"),
                Err(error) => warn!(%error, "could not allocate Anland screenshot buffer"),
            }
        }
        if let Some(request_id) = prepared
            && let Some(manager) = self.manager.as_mut()
            && manager.prepared(request_id.get())
        {
            runtime.arm_screenshot_frame(plan.pool.output, request_id.get())?;
        }
        if let Some(request) = request {
            if let Some(request_id) = request.request_id {
                let request_id = request_id.get();
                if let Some(manager) = self.manager.as_mut()
                    && manager.request_id() == Some(request_id)
                {
                    if let Err(error) =
                        manager.finish_selection(&mut graphics.renderer, runtime, request)
                    {
                        warn!(%error, request_id, "Anland frozen screenshot failed");
                        self.cancel_selection(runtime, Some(request_id), false)?;
                    }
                    self.sources.get_mut().cancel_screenshot(request_id);
                    runtime.send_screenshot_action(
                        crate::wire::ShellAction::ScreenshotDone,
                        request_id,
                        None,
                    )?;
                }
            } else if self.ensure_writer(events) {
                // Read a subsequently ACKed frame, never an unowned last index.
                self.live_request = Some(request);
            }
        }
        Ok(())
    }

    pub fn complete(
        &mut self,
        outcome: &Outcome,
        runtime: &mut FlutterRuntime,
        running: bool,
    ) -> Result<(), Box<dyn Error>> {
        let (frame, accepted) = match outcome {
            Outcome::Presented { frame, .. } => (frame, true),
            Outcome::Dropped { frame, .. } => (frame, false),
        };
        let serial = frame.request.dirty_serial;
        let source = self.sources.get_mut().source(serial);
        self.sources.get_mut().complete(serial, accepted);
        if !accepted
            && running
            && let Some(request_id) = source.and_then(|source| source.screenshot_request_id)
            && self
                .manager
                .as_ref()
                .is_some_and(|manager| manager.request_id() == Some(request_id))
        {
            // A dropped tagged frame did not freeze anything. Cancel the old
            // broker tag before rearming the same selection on the next frame.
            runtime.cancel_screenshot_frame(request_id);
            runtime.arm_screenshot_frame(frame.pool.output, request_id)?;
        }
        Ok(())
    }

    pub fn process_ready(
        &mut self,
        runtime: &mut FlutterRuntime,
        imports: &ImportedPool,
        plan: &OutputPlan,
        graphics: &mut RenderDevice,
        events: &mut RuntimeState,
        now: Instant,
    ) -> Result<(), Box<dyn Error>> {
        if events.secure_session_locked() {
            return Ok(());
        }
        let sources = self
            .sources
            .get_mut()
            .ready(now, super::client::fence_signaled)?;
        for source in sources {
            let buffer = imports.buffer(source.index)?;
            if self.sources.get_mut().is_latest(source.serial) {
                if let Some(frontend) = events.wayland.as_mut()
                    && frontend.has_pending_screencopy_for_output(plan.pool.output)
                {
                    let timestamp = frontend.screencopy_clock_now();
                    frontend.screencopy_output_presented(plan.pool.output, source.revision);
                    frontend.process_screencopies(
                        &mut graphics.renderer,
                        buffer,
                        plan.pool.output,
                        timestamp,
                        || runtime.retain_output(plan.pool.output, source.index),
                    )?;
                }
                if let Some(request) = self.live_request.take()
                    && let Some(manager) = self.manager.as_ref()
                {
                    let mut composite = composite_source(plan, buffer)?;
                    // The common screenshot compositor waits for its GPU read.
                    // If that proof fails, retain this owner rather than drop
                    // the source pin and permit renderer teardown/reuse.
                    self.teardown_failed = true;
                    if let Err(error) = manager.capture_live(
                        &mut graphics.renderer,
                        &mut graphics.capture_allocator,
                        &plan.atlas,
                        buffer.format().modifier,
                        std::slice::from_mut(&mut composite),
                        request,
                    ) {
                        return Err(error);
                    }
                    self.teardown_failed = false;
                }
            }
            if let Some(request_id) = source.screenshot_request_id
                && let Some(manager) = self.manager.as_mut()
                && manager.request_id() == Some(request_id)
            {
                let mut composite = composite_source(plan, buffer)?;
                self.teardown_failed = true;
                let frozen = manager.capture_prepared_frame(
                    &mut graphics.renderer,
                    runtime,
                    plan.pool.output,
                    std::slice::from_mut(&mut composite),
                )?;
                self.teardown_failed = false;
                match frozen {
                    Some((request_id, texture_id)) => runtime.send_screenshot_action(
                        crate::wire::ShellAction::ScreenshotTextureReady,
                        request_id,
                        Some(texture_id),
                    )?,
                    None => {}
                }
            }
            // Protocol reads own another lease until their snapshot GPU fence;
            // ScreenshotManager's synchronous snapshot has finished its read.
            self.sources.get_mut().finish(source.serial);
        }
        Ok(())
    }

    pub fn stop(&mut self, runtime: &mut FlutterRuntime, events: &mut RuntimeState) {
        events.pending_screenshot_selection = None;
        self.live_request = None;
        if let Some(frontend) = events.wayland.as_mut() {
            frontend.fail_all_screencopies();
        }
        if self.teardown_failed {
            // An exceptional GPU read must keep its exact source/selection.
            // run() retains this whole owner, rather than treating cancellation
            // as proof that the GPU stopped accessing the backing store.
            return;
        }
        if let Err(error) = self.cancel_selection(runtime, None, false) {
            self.teardown_failed = true;
            warn!(%error, "Anland screenshot teardown failed; retain GPU owners");
            return;
        }
        self.sources.get_mut().clear();
    }

    pub fn can_retire(&self) -> bool {
        !self.teardown_failed
    }

    pub fn fences(&self) -> io::Result<Vec<std::os::fd::OwnedFd>> {
        self.sources
            .borrow()
            .fences()
            .into_iter()
            .map(|fd| fd.try_clone_to_owned())
            .collect()
    }
}

fn composite_source(
    plan: &OutputPlan,
    buffer: &smithay::backend::allocator::dmabuf::Dmabuf,
) -> Result<OutputCompositeSource, Box<dyn Error>> {
    if plan.atlas.outputs.len() != 1 {
        return Err("Anland screenshot requires the single native output atlas".into());
    }
    let output = &plan.atlas.outputs[0];
    if output.id != plan.pool.output {
        return Err("Anland screenshot atlas has a different output identity".into());
    }
    Ok(OutputCompositeSource {
        dmabuf: buffer.clone(),
        destination: crate::kms_render::physical_rect(output.source_rect)?,
        transform: crate::kms_render::smithay_output_transform(output.transform),
    })
}

/// Decorate only native publication. The driver and public producer retain sole
/// authority over commit, acceptance, ACK/drop, release and backpressure.
pub(crate) struct CapturingRenderer<'a> {
    pub runtime: &'a mut FlutterRuntime,
    pub captures: &'a mut CaptureState,
    pub requested: bool,
    pub running: bool,
}

impl NativeOutput for CapturingRenderer<'_> {
    fn publish(&self, frame: &ReadyOutputFrame) -> io::Result<()> {
        // Dup first: an allocation error must not occur after publication but
        // before the Submissions owner installs its native pin.
        let capture = self.running
            && self.runtime.scene_recovery_frame_allowed(frame)
            && (self.requested || frame.screenshot_request_id.is_some());
        let acquire = if capture {
            frame.fence.as_ref().map(|fd| fd.try_clone()).transpose()?
        } else {
            None
        };
        NativeOutput::publish(self.runtime, frame)?;
        if capture {
            let lease = self
                .runtime
                .retain_output(frame.output_id, frame.index)
                .map_err(|error| io::Error::other(error.to_string()))?;
            self.captures.sources.borrow_mut().retain(
                CaptureSource {
                    serial: frame.request.dirty_serial,
                    index: frame.index,
                    revision: frame.capture_revision,
                    screenshot_request_id: frame.screenshot_request_id,
                },
                lease,
                acquire,
                Instant::now(),
            )?;
        }
        Ok(())
    }
    fn release(&self, output: OutputId, index: usize) -> io::Result<()> {
        NativeOutput::release(self.runtime, output, index)
    }
}

impl NativeRenderer for CapturingRenderer<'_> {
    fn select(&self, pool: Pool, target: Target) -> bool {
        NativeRenderer::select(self.runtime, pool, target)
    }
    fn revoke(&self) {
        NativeRenderer::revoke(self.runtime);
    }
    fn work_pending(&self, output: OutputId, now: Instant) -> bool {
        NativeRenderer::work_pending(self.runtime, output, now)
    }
    fn retirement_pending(&self, output: OutputId, now: Instant) -> bool {
        !self.captures.can_retire() || NativeRenderer::retirement_pending(self.runtime, output, now)
    }
    fn authorization_deadline(&self, output: OutputId) -> Option<Instant> {
        NativeRenderer::authorization_deadline(self.runtime, output)
    }
    fn render(
        &mut self,
        request: OutputFrameRequest,
        textures: &[i64],
    ) -> io::Result<RenderDispatch> {
        NativeRenderer::render(self.runtime, request, textures)
    }
    fn take_ready(&mut self, output: OutputId) -> Option<ReadyOutputFrame> {
        NativeRenderer::take_ready(self.runtime, output)
    }
    fn submission_allowed(&self, frame: &ReadyOutputFrame) -> bool {
        NativeRenderer::submission_allowed(self.runtime, frame)
    }
    fn recovery_expired(&self, now: Instant) -> bool {
        NativeRenderer::recovery_expired(self.runtime, now)
    }
    fn frame_accepted(&mut self, request: OutputFrameRequest) {
        NativeRenderer::frame_accepted(self.runtime, request);
    }
}
