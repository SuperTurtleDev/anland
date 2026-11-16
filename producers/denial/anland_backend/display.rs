//! Consumer target -> Denial raster request -> public presentation.
//!
//! One driver belongs to one imported pool. This is native render scheduling,
//! not a second transport queue: public writable-target and release events are
//! the sole authority for consumer ownership. A stopped driver keeps its native
//! work until retirement; reconnect must never reset these holds in place.
use super::client::{Client, Event, Target};
use super::submission::{NativeOutput, Outcome, Pool, Presentation, Submissions, Submitted};
use crate::flutter_runtime::ReadyOutputFrame;
use crate::frame_request::{FrameTick, OutputFrameRequest, RenderDispatch};
use denial_core::topology::OutputId;
use std::collections::BTreeSet;
use std::io;
use std::os::fd::BorrowedFd;
use std::time::{Duration, Instant};

pub(crate) trait DisplaySession: Presentation {
    fn pump(&mut self) -> io::Result<()>;
    fn writable(&self) -> io::Result<Option<Target>>;
}
impl DisplaySession for Client {
    fn pump(&mut self) -> io::Result<()> {
        Client::pump(self)
    }
    fn writable(&self) -> io::Result<Option<Target>> {
        Client::writable(self)
    }
}

/// Production implementation delegates to FlutterRuntime's existing broker,
/// raster engine and frame-ready queue. Only the compositor thread calls this.
pub(crate) trait NativeRenderer: NativeOutput {
    fn select(&self, pool: Pool, target: Target) -> bool;
    fn revoke(&self);
    /// Atomically expire only unused authorizations, then inspect native work.
    fn work_pending(&self, output: OutputId, now: Instant) -> bool;
    /// Unlike request progress, this includes all references pinning the pool.
    fn retirement_pending(&self, output: OutputId, now: Instant) -> bool;
    fn authorization_deadline(&self, output: OutputId) -> Option<Instant>;
    fn render(
        &mut self,
        request: OutputFrameRequest,
        textures: &[i64],
    ) -> io::Result<RenderDispatch>;
    fn take_ready(&mut self, output: OutputId) -> Option<ReadyOutputFrame>;
    fn submission_allowed(&self, _frame: &ReadyOutputFrame) -> bool {
        true
    }
    fn recovery_expired(&self, _now: Instant) -> bool {
        false
    }
    fn frame_accepted(&mut self, _request: OutputFrameRequest) {}
}

struct Pending {
    target: Target,
    request: OutputFrameRequest,
    textures: BTreeSet<i64>,
}

#[derive(Default)]
pub(crate) struct Progress {
    pub outcomes: Vec<Outcome>,
    /// Accepted public commits pin the matching timeline grant until ACK/drop.
    pub submitted_ticks: Vec<FrameTick>,
    pub state_changes: Vec<Event>,
    /// Schedule another nonblocking iteration instead of waiting for a new edge.
    pub more: bool,
    pub render_requested: bool,
    pub vsync_dispatched: bool,
    /// Local output clock tick, independent of damage, raster completion or ACK.
    pub frame_tick: Option<FrameTick>,
    /// Reconfigure only after retiring this driver and the old native renderer.
    pub pool_changed: bool,
    pub errors: Vec<io::Error>,
}

pub(crate) struct OutputDriver {
    pool: Pool,
    interval: Duration,
    submissions: Submissions,
    pending: Option<Pending>,
    textures: BTreeSet<i64>,
    serial: u64,
    tick_serial: u64,
    next_tick: Option<Instant>,
    current_tick: Option<FrameTick>,
    stopped: bool,
    geometry_pending: bool,
    // Metadata failures leave the broker Pending. Retain the actual frame/fence
    // until the outer owner has stopped the renderer; never report retirement.
    unclaimed: Vec<ReadyOutputFrame>,
}

fn interval(refresh_millihz: u32) -> io::Result<Duration> {
    if refresh_millihz == 0 {
        return Err(io::Error::other("Anland output has no refresh rate"));
    }
    Ok(Duration::from_nanos(
        1_000_000_000_000 / u64::from(refresh_millihz),
    ))
}

impl OutputDriver {
    pub fn new(pool: Pool, refresh_millihz: u32) -> io::Result<Self> {
        Ok(Self {
            pool,
            interval: interval(refresh_millihz)?,
            submissions: Submissions::new(pool)?,
            pending: None,
            textures: BTreeSet::new(),
            serial: 0,
            tick_serial: 0,
            next_tick: None,
            current_tick: None,
            stopped: false,
            geometry_pending: false,
            unclaimed: Vec::new(),
        })
    }

    /// The retained Wayland timeline must not see frame/tick serials restart.
    pub fn replacement(&self, pool: Pool, refresh: u32) -> io::Result<Self> {
        if !self.stopped() {
            return Err(io::Error::other("replacement requires a stopped driver"));
        }
        let mut replacement = Self::new(pool, refresh)?;
        replacement.serial = self.serial;
        replacement.tick_serial = self.tick_serial;
        Ok(replacement)
    }

    pub fn set_refresh(&mut self, refresh: u32, now: Instant) -> io::Result<()> {
        let interval = interval(refresh)?;
        if self.interval != interval {
            self.interval = interval;
            self.current_tick = None;
            self.next_tick = Some(now);
        }
        Ok(())
    }

    /// Ingest ordinary buffer-only updates once per output grant, not at the
    /// mailbox client's rate. This is not a render/target-availability claim.
    pub fn buffer_sync_due(&self, now: Instant) -> bool {
        !self.stopped()
            && !self.geometry_pending
            && (self.current_tick.is_some() || self.next_tick.is_none_or(|at| now >= at))
    }
    /// Coalesce updates while the public producer or native raster is busy.
    pub fn update_textures(&mut self, textures: impl IntoIterator<Item = i64>) {
        self.textures.extend(textures);
    }

    /// Drain existing raster requests/ready frames while pausing new allocation.
    /// Published consumer buffers may remain pinned: no pool is replaced here.
    pub fn set_geometry_pending(&mut self, pending: bool) {
        self.geometry_pending = pending;
    }

    pub fn geometry_ready(&self, native: &impl NativeRenderer, now: Instant) -> bool {
        !self.stopped()
            && self.pending.is_none()
            && self.unclaimed.is_empty()
            && !native.work_pending(self.pool.output, now)
    }

    pub fn stopped(&self) -> bool {
        self.stopped || self.submissions.stopped()
    }

    pub fn stop(&mut self, native: &impl NativeRenderer) -> Vec<Outcome> {
        self.stopped = true;
        native.revoke();
        self.submissions.cancel()
    }

    /// Still requires an empty public event queue before installing another
    /// pool: release events use slot+1 IDs and carry no session generation.
    pub fn retired(&self, native: &impl NativeRenderer, now: Instant) -> bool {
        self.stopped()
            && self.unclaimed.is_empty()
            && self.submissions.retired()
            && !native.retirement_pending(self.pool.output, now)
    }

    pub fn wait_timeout(&self, native: &impl NativeRenderer, now: Instant) -> Duration {
        let mut timeout = Duration::from_millis(100);
        if !self.stopped() {
            timeout = self
                .next_tick
                .map_or(Duration::ZERO, |at| at.saturating_duration_since(now));
        }
        for at in [
            native.authorization_deadline(self.pool.output),
            self.submissions.release_deadline(),
        ]
        .into_iter()
        .flatten()
        {
            timeout = timeout.min(at.saturating_duration_since(now));
        }
        timeout
    }

    pub fn release_fences(&self) -> Vec<BorrowedFd<'_>> {
        self.submissions.release_fences()
    }

    fn advance_clock(&mut self, now: Instant) -> io::Result<Option<FrameTick>> {
        if self.stopped() || self.next_tick.is_some_and(|at| now < at) {
            return Ok(None);
        }
        // Keep the original clock phase when waking late. Work out the current
        // grid boundary without multiplying a potentially large missed count.
        let elapsed = now.duration_since(self.next_tick.unwrap_or(now)).as_nanos();
        let period = self.interval.as_nanos();
        let periods = u64::try_from(elapsed / period)
            .ok()
            .and_then(|missed| missed.checked_add(1))
            .ok_or_else(|| io::Error::other("Anland clock serial exhausted"))?;
        // interval() is nonzero and at most 1000 seconds, so the remainder fits.
        let remainder = Duration::from_nanos((elapsed % period) as u64);
        let render_deadline = now
            .checked_sub(remainder)
            .ok_or_else(|| io::Error::other("Anland clock deadline overflow"))?;
        let presentation_target = render_deadline
            .checked_add(self.interval)
            .ok_or_else(|| io::Error::other("Anland clock deadline overflow"))?;
        let sequence = self
            .tick_serial
            .checked_add(periods)
            .ok_or_else(|| io::Error::other("Anland clock serial exhausted"))?;
        // Publish only one tick after missed periods. Keep a full interval for
        // native authorization expiry, even when this boundary was observed late.
        let tick = FrameTick {
            output: self.pool.output,
            sequence,
            nominal_interval: self.interval,
            interval: self.interval,
            render_deadline,
            presentation_target,
        };
        self.tick_serial = sequence;
        self.next_tick = Some(tick.presentation_target);
        self.current_tick = Some(tick);
        Ok(Some(tick))
    }

    fn matches(&self, target: Target) -> bool {
        target.generation == self.pool.generation
            && target.count as usize == self.pool.count
            && target.index < target.count
            && target.width == self.pool.size.width
            && target.height == self.pool.size.height
    }

    fn fail(&mut self, native: &impl NativeRenderer, progress: &mut Progress, error: io::Error) {
        progress.errors.push(error);
        progress.outcomes.extend(self.stop(native));
    }

    /// Called after processing Flutter runtime events, on public fd readiness
    /// or the maintenance timer. It never waits for a fence or consumer ACK.
    /// Deliver every returned outcome even if the same batch also has errors.
    pub fn drive(
        &mut self,
        native: &mut impl NativeRenderer,
        session: &mut impl DisplaySession,
        now: Instant,
        budget: usize,
        signaled: impl FnMut(BorrowedFd<'_>) -> io::Result<bool>,
    ) -> Progress {
        let mut progress = Progress::default();
        let pump_error = session.pump().err();
        let batch = self.submissions.drain(session, budget, now);
        progress.outcomes = batch.outcomes;
        progress.more = batch.more;
        // Preserve feedback already queued before a transport failure, then
        // cancel only the native frames whose outcome is still unknown.
        if let Some(error) = pump_error {
            self.fail(native, &mut progress, error);
        }
        if let Some(error) = batch.error {
            self.fail(native, &mut progress, error);
        }
        for event in &batch.state_changes {
            match event {
                Event::Output(output) => {
                    if output.width != self.pool.size.width
                        || output.height != self.pool.size.height
                    {
                        progress.pool_changed = true;
                    } else if let Err(error) = self.set_refresh(output.refresh_mhz, now) {
                        self.fail(native, &mut progress, error);
                    }
                }
                Event::Target {
                    generation, count, ..
                } => {
                    if *generation != self.pool.generation || *count as usize != self.pool.count {
                        progress.pool_changed = true;
                    }
                }
                _ => {}
            }
        }
        progress.state_changes = batch.state_changes;
        if progress.pool_changed {
            progress.outcomes.extend(self.stop(native));
        }
        if let Err(error) = self.submissions.release_ready(native, now, signaled) {
            self.fail(native, &mut progress, error);
        }
        if self.submissions.stopped() && !self.stopped {
            self.fail(
                native,
                &mut progress,
                io::Error::other("Anland native pool is quarantined"),
            );
        }
        if !self.stopped() && native.recovery_expired(now) {
            self.fail(
                native,
                &mut progress,
                io::Error::new(
                    io::ErrorKind::TimedOut,
                    "Anland cold scene recovery exceeded 3s",
                ),
            );
        }
        if self.stopped() {
            if let Some(frame) = native.take_ready(self.pool.output) {
                if let Some(pending) = self.pending.take() {
                    if let Err(unclaimed) = self.submissions.discard_ready(
                        native,
                        pending.target,
                        pending.request,
                        frame,
                        now,
                    ) {
                        self.unclaimed.push(unclaimed.frame);
                        progress.errors.push(unclaimed.error);
                    }
                } else {
                    self.unclaimed.push(frame);
                    progress
                        .errors
                        .push(io::Error::other("unsolicited retiring raster frame"));
                }
                progress.more = true;
            }
            return progress;
        }
        match self.advance_clock(now) {
            Ok(tick) => progress.frame_tick = tick,
            Err(error) => {
                self.fail(native, &mut progress, error);
                return progress;
            }
        }
        if progress.more {
            return progress;
        }

        // At most one native request is outstanding for this output. A ready
        // handoff remains Pending in the broker until Submissions publishes it.
        if let Some(frame) = native.take_ready(self.pool.output) {
            let Some(pending) = self.pending.take() else {
                self.unclaimed.push(frame);
                self.fail(
                    native,
                    &mut progress,
                    io::Error::other("unsolicited Anland raster frame"),
                );
                return progress;
            };
            if !native.submission_allowed(&frame) {
                self.textures.extend(pending.textures);
                if let Err(unclaimed) = self.submissions.discard_ready(
                    native,
                    pending.target,
                    pending.request,
                    frame,
                    now,
                ) {
                    self.unclaimed.push(unclaimed.frame);
                    self.fail(native, &mut progress, unclaimed.error);
                }
                progress.more = true;
                return progress;
            }
            let accepted_request = frame.request;
            match self
                .submissions
                .submit(native, session, pending.target, pending.request, frame)
            {
                Ok(Submitted::Accepted { handoff_error, .. }) => {
                    progress.submitted_ticks.push(pending.request.tick);
                    native.frame_accepted(accepted_request);
                    if let Some(error) = handoff_error {
                        self.textures.extend(pending.textures);
                        self.fail(native, &mut progress, error);
                    }
                }
                Ok(Submitted::Rejected { error, frame }) => {
                    self.textures.extend(pending.textures);
                    progress.outcomes.push(Outcome::Dropped {
                        frame,
                        reason: None,
                    });
                    if error.kind() != io::ErrorKind::WouldBlock {
                        self.fail(native, &mut progress, error);
                    }
                }
                Err(unclaimed) => {
                    self.unclaimed.push(unclaimed.frame);
                    self.fail(native, &mut progress, unclaimed.error);
                }
            }
            // A rejection can queue a release without an ID. Always drain after
            // submission before reusing any slot, including for static content.
            progress.more = true;
            return progress;
        }
        if self.pending.is_some() {
            if native.work_pending(self.pool.output, now) {
                return progress;
            }
            // Framework consumed the VSync baton without acquiring a target.
            // Keep the original content updates for the next authorization.
            self.textures.extend(self.pending.take().unwrap().textures);
        }

        if self.geometry_pending {
            return progress;
        }
        let Some(tick) = self.current_tick else {
            return progress;
        };
        let target = match session.writable() {
            Ok(Some(target)) => target,
            Ok(None) => return progress,
            Err(error) => {
                self.fail(native, &mut progress, error);
                return progress;
            }
        };
        if !self.matches(target) {
            progress.pool_changed = true;
            progress.outcomes.extend(self.stop(native));
            return progress;
        }
        if !self.submissions.can_render(target.index as usize) || !native.select(self.pool, target)
        {
            return progress;
        }
        let Some(serial) = self.serial.checked_add(1) else {
            self.fail(
                native,
                &mut progress,
                io::Error::other("Anland render serial exhausted"),
            );
            return progress;
        };
        self.serial = serial;
        let request = OutputFrameRequest {
            tick,
            dirty_serial: serial,
            lock_frame_token: 0,
            fingerprint_epoch: 0,
            scene_recovery_token: 0,
        };
        let textures = std::mem::take(&mut self.textures);
        let texture_ids: Vec<_> = textures.iter().copied().collect();
        self.pending = Some(Pending {
            target,
            request,
            textures,
        });
        match native.render(request, &texture_ids) {
            Ok(dispatch) if dispatch.authorized => {
                // The display tick is a single-use raster grant, not a reusable
                // permission. A FrameReady/ACK/input wakeup before next_tick must
                // not render the same period again. New texture updates remain
                // coalesced until the next clock tick, independently of input.
                self.current_tick = None;
                progress.render_requested = true;
                progress.vsync_dispatched = dispatch.vsync_dispatched;
            }
            Ok(_) => {
                native.revoke();
                self.textures.extend(self.pending.take().unwrap().textures);
            }
            Err(error) => self.fail(native, &mut progress, error),
        }
        progress
    }
}
