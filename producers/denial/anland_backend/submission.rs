//! Native output ownership and commit-to-frame correlation.
//!
//! The public producer decides acceptance, ACK, release and backpressure. This
//! module only pins Denial's native slots and translates those separate facts.
//! One instance belongs to one imported pool. Drain it completely before a new
//! session can reuse the public slot+1 buffer identities. Never reset a failed
//! fence by reconnecting: quarantine lasts until renderer teardown.
use super::client::{Client, Event, Target};
use crate::flutter_runtime::ReadyOutputFrame;
use crate::frame_request::OutputFrameRequest;
use crate::surface_feedback::SurfaceFeedback;
use denial_core::topology::{OutputId, PixelSize, RenderViewId};
use std::collections::BTreeMap;
use std::io;
use std::os::fd::{AsFd, BorrowedFd, OwnedFd};
use std::time::{Duration, Instant};

const RELEASE_TIMEOUT: Duration = Duration::from_secs(5);

#[derive(Clone, Copy, Debug)]
pub(crate) struct Pool {
    pub generation: u64,
    pub configuration: u64,
    pub output: OutputId,
    pub view: RenderViewId,
    pub size: PixelSize,
    pub count: usize,
}

/// Methods must preserve the broker's all-or-nothing publication/release contract.
pub(crate) trait NativeOutput {
    fn publish(&self, frame: &ReadyOutputFrame) -> io::Result<()>;
    fn release(&self, output: OutputId, index: usize) -> io::Result<()>;
}

pub(crate) trait Presentation {
    fn commit(&mut self, target: &Target, fence: Option<BorrowedFd<'_>>) -> io::Result<u64>;
    fn present(&mut self) -> io::Result<()>;
    fn next_event(&mut self) -> io::Result<Option<Event>>;
    fn events_pending(&self) -> usize;
}
impl Presentation for Client {
    fn commit(&mut self, target: &Target, fence: Option<BorrowedFd<'_>>) -> io::Result<u64> {
        Client::commit(self, target, fence)
    }
    fn present(&mut self) -> io::Result<()> {
        Client::present(self)
    }
    fn next_event(&mut self) -> io::Result<Option<Event>> {
        Client::next_event(self)
    }
    fn events_pending(&self) -> usize {
        Client::events_pending(self)
    }
}

#[derive(Debug)]
pub(crate) struct Completion {
    pub pool: Pool,
    pub request: OutputFrameRequest,
    pub capture_revision: u64,
    pub feedback: Vec<SurfaceFeedback>,
}

#[derive(Debug)]
pub(crate) enum Outcome {
    Presented {
        frame: Completion,
        observed_at: Instant,
        reported_ns: u64,
    },
    // Dropping a rendered output must not consume the shared surface feedback
    // latch: another output or a later repaint may still sample that source.
    Dropped {
        frame: Completion,
        reason: Option<u32>,
    },
}

#[derive(Debug)]
pub(crate) enum Submitted {
    Accepted {
        commit: u64,
        handoff_error: Option<io::Error>,
    },
    Rejected {
        error: io::Error,
        frame: Completion,
    },
}

/// Native broker still owns this Pending frame. Keep it alive and stop/drain the
/// renderer before dropping it; metadata failure never authorizes slot reuse.
#[derive(Debug)]
pub(crate) struct Unclaimed {
    pub error: io::Error,
    pub frame: ReadyOutputFrame,
}

struct Hold {
    acquire: Option<OwnedFd>,
    release: Option<OwnedFd>,
    released_at: Option<Instant>,
    // A failed commit may have queued a release without returning a commit ID.
    // Drain that queue before allowing this slot to acquire a new identity.
    needs_drain: bool,
    release_seen: bool,
    quarantined: bool,
}

#[derive(Default)]
pub(crate) struct Batch {
    pub outcomes: Vec<Outcome>,
    pub state_changes: Vec<Event>,
    pub more: bool,
    pub error: Option<io::Error>,
}

pub(crate) struct Submissions {
    pool: Pool,
    holds: Vec<Option<Hold>>,
    frames: BTreeMap<u64, Completion>,
    orphaned: Vec<Completion>,
    stopped: bool,
    unclaimed: bool,
}

impl Submissions {
    pub fn new(pool: Pool) -> io::Result<Self> {
        if pool.generation == 0
            || pool.configuration == 0
            || !(1..=8).contains(&pool.count)
            || pool.size.width == 0
            || pool.size.height == 0
        {
            return Err(io::Error::other("invalid Anland native pool"));
        }
        Ok(Self {
            pool,
            holds: (0..pool.count).map(|_| None).collect(),
            frames: BTreeMap::new(),
            orphaned: Vec::new(),
            stopped: false,
            unclaimed: false,
        })
    }

    pub fn submit(
        &mut self,
        native: &impl NativeOutput,
        session: &mut impl Presentation,
        target: Target,
        expected: OutputFrameRequest,
        mut frame: ReadyOutputFrame,
    ) -> Result<Submitted, Box<Unclaimed>> {
        let valid = !self.stopped
            && self.frames.len() < self.pool.count
            && target.generation == self.pool.generation
            && target.count as usize == self.pool.count
            && target.index < target.count
            && target.width == self.pool.size.width
            && target.height == self.pool.size.height
            && frame.output_id == self.pool.output
            && frame.render_view_id == self.pool.view
            && frame.configuration_generation == self.pool.configuration
            && frame.index == target.index as usize
            && self.holds[frame.index].is_none()
            && frame.damage.matches_size(target.width, target.height)
            && frame.request.tick == expected.tick
            && frame.request.dirty_serial == expected.dirty_serial
            && expected.dirty_serial != 0
            && expected.tick.output == self.pool.output;
        if !valid {
            self.stopped = true;
            self.unclaimed = true;
            return Err(Box::new(Unclaimed {
                error: io::Error::other("stale or unauthorized native output"),
                frame,
            }));
        }
        if let Err(error) = native.publish(&frame) {
            self.stopped = true;
            self.unclaimed = true;
            return Err(Box::new(Unclaimed { error, frame }));
        }
        let index = frame.index;
        self.holds[index] = Some(Hold {
            acquire: frame.fence.take(),
            release: None,
            released_at: None,
            needs_drain: false,
            release_seen: false,
            quarantined: false,
        });
        let completion = Completion {
            pool: self.pool,
            request: frame.request,
            capture_revision: frame.capture_revision,
            feedback: frame.feedback,
        };
        let hold = self.holds[index].as_mut().unwrap();
        let id = match session.commit(&target, hold.acquire.as_ref().map(AsFd::as_fd)) {
            Ok(id) => id,
            Err(error) => {
                hold.released_at = Some(Instant::now());
                hold.needs_drain = true;
                return Ok(Submitted::Rejected {
                    error,
                    frame: completion,
                });
            }
        };
        if id == 0 || self.frames.contains_key(&id) {
            self.stopped = true;
            self.orphaned.push(completion);
            return Ok(Submitted::Accepted {
                commit: id,
                handoff_error: Some(io::Error::other("public producer reused a commit identity")),
            });
        }
        // This MUST happen before present: handoff failure may immediately
        // produce drop/release events for the accepted transaction.
        self.frames.insert(id, completion);
        Ok(Submitted::Accepted {
            commit: id,
            handoff_error: session.present().err(),
        })
    }

    /// A raster callback may complete after revoke. Publish its native pin but
    /// never give it to the consumer. Wait for its acquire fence before release.
    pub fn discard_ready(
        &mut self,
        native: &impl NativeOutput,
        target: Target,
        expected: OutputFrameRequest,
        mut frame: ReadyOutputFrame,
        now: Instant,
    ) -> Result<(), Box<Unclaimed>> {
        if frame.output_id != self.pool.output
            || frame.render_view_id != self.pool.view
            || frame.configuration_generation != self.pool.configuration
            || target.generation != self.pool.generation
            || frame.index != target.index as usize
            || frame.index >= self.pool.count
            || frame.request.tick != expected.tick
            || frame.request.dirty_serial != expected.dirty_serial
            || self.holds[frame.index].is_some()
        {
            self.unclaimed = true;
            return Err(Box::new(Unclaimed {
                error: io::Error::other("invalid retiring raster frame"),
                frame,
            }));
        }
        if let Err(error) = native.publish(&frame) {
            self.unclaimed = true;
            return Err(Box::new(Unclaimed { error, frame }));
        }
        self.holds[frame.index] = Some(Hold {
            acquire: frame.fence.take(),
            release: None,
            released_at: Some(now),
            needs_drain: true,
            release_seen: false,
            quarantined: false,
        });
        Ok(())
    }

    pub fn drain(&mut self, session: &mut impl Presentation, budget: usize, now: Instant) -> Batch {
        let mut batch = Batch::default();
        for _ in 0..budget.min(256) {
            let event = match session.next_event() {
                Ok(event) => event,
                Err(error) => {
                    batch.error = Some(error);
                    break;
                }
            };
            match event {
                Some(Event::Presented {
                    commit,
                    software_time_ns,
                }) => {
                    if let Some(frame) = self.frames.remove(&commit) {
                        batch.outcomes.push(Outcome::Presented {
                            frame,
                            observed_at: now,
                            reported_ns: software_time_ns,
                        });
                    }
                }
                Some(Event::Dropped { commit, reason }) => {
                    if let Some(frame) = self.frames.remove(&commit) {
                        batch.outcomes.push(Outcome::Dropped {
                            frame,
                            reason: Some(reason),
                        });
                    }
                }
                Some(Event::Released { buffer, fence, .. }) => {
                    // Releases have commit_id=0. Never use it to look up a frame.
                    let hold = buffer
                        .checked_sub(1)
                        .and_then(|index| usize::try_from(index).ok())
                        .and_then(|index| self.holds.get_mut(index))
                        .and_then(Option::as_mut);
                    if let Some(hold) = hold {
                        if hold.release_seen {
                            // A duplicate release cannot replace a live fence.
                            hold.quarantined = true;
                            self.stopped = true;
                        } else {
                            hold.release_seen = true;
                            hold.release = fence;
                            hold.released_at.get_or_insert(now);
                        }
                    }
                }
                Some(event) => batch.state_changes.push(event),
                None if session.events_pending() == 0 => break,
                None => {} // facade may have filtered an unusable target
            }
        }
        batch.more = batch.error.is_some() || session.events_pending() != 0;
        if !batch.more {
            for hold in self.holds.iter_mut().flatten() {
                hold.needs_drain = false;
            }
        }
        batch
    }

    /// Poll only; never block the compositor on a GPU fence. Query failures and
    /// timeouts retain the native pin and the descriptor until renderer teardown.
    pub fn release_ready(
        &mut self,
        native: &impl NativeOutput,
        now: Instant,
        mut signaled: impl FnMut(BorrowedFd<'_>) -> io::Result<bool>,
    ) -> io::Result<Vec<usize>> {
        let mut released = Vec::new();
        for (index, entry) in self.holds.iter_mut().enumerate() {
            let Some(hold) = entry.as_mut() else {
                continue;
            };
            let Some(since) = hold.released_at else {
                continue;
            };
            if hold.quarantined || hold.needs_drain {
                continue;
            }
            let result = (|| {
                let mut ready = true;
                for fence in [&mut hold.acquire, &mut hold.release] {
                    if let Some(fd) = fence.as_ref() {
                        if signaled(fd.as_fd())? {
                            *fence = None;
                        } else {
                            ready = false;
                        }
                    }
                }
                if !ready && now.saturating_duration_since(since) >= RELEASE_TIMEOUT {
                    return Err(io::Error::new(
                        io::ErrorKind::TimedOut,
                        "Anland output fence timed out",
                    ));
                }
                if ready {
                    native.release(self.pool.output, index)?;
                }
                Ok(ready)
            })();
            match result {
                Ok(true) => {
                    *entry = None;
                    released.push(index);
                }
                Ok(false) => {}
                Err(error) => {
                    hold.quarantined = true;
                    self.stopped = true;
                    return Err(error);
                }
            }
        }
        Ok(released)
    }

    pub fn release_fences(&self) -> Vec<BorrowedFd<'_>> {
        self.holds
            .iter()
            .flatten()
            .filter(|hold| hold.released_at.is_some() && !hold.quarantined && !hold.needs_drain)
            .flat_map(|hold| [hold.acquire.as_ref(), hold.release.as_ref()])
            .flatten()
            .map(AsFd::as_fd)
            .collect()
    }

    pub fn release_deadline(&self) -> Option<Instant> {
        self.holds
            .iter()
            .flatten()
            .filter(|hold| !hold.quarantined)
            .filter_map(|hold| hold.released_at?.checked_add(RELEASE_TIMEOUT))
            .min()
    }

    pub fn slot_available(&self, index: usize) -> bool {
        !self.stopped && self.holds.get(index).is_some_and(Option::is_none)
    }
    pub fn can_render(&self, index: usize) -> bool {
        self.slot_available(index) && self.frames.len() < self.pool.count
    }
    pub fn stopped(&self) -> bool {
        self.stopped
    }
    pub fn retired(&self) -> bool {
        !self.unclaimed
            && self.holds.iter().all(Option::is_none)
            && self.frames.is_empty()
            && self.orphaned.is_empty()
    }
    /// Call after revoking raster authorizations. The caller must still drop the
    /// public session and drain releases; cancellation alone never unpins slots.
    pub fn cancel(&mut self) -> Vec<Outcome> {
        self.stopped = true;
        std::mem::take(&mut self.frames)
            .into_values()
            .chain(self.orphaned.drain(..))
            .map(|frame| Outcome::Dropped {
                frame,
                reason: None,
            })
            .collect()
    }
}
