//! Pure lifecycle policy; retirement requires native and public ownership proofs.
use std::time::{Duration, Instant};
// This deadline protects borrowed GPU owners AFTER stop; never consumer arrival.
const RETIRE_LIMIT: Duration = Duration::from_secs(5);
// Retry pacing only. Waiting for the daemon/consumer has no total deadline.
const RETRY_INTERVAL: Duration = Duration::from_millis(200);

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) enum Action {
    Wait,
    Drop,
    Reconnect,
    Reopen,
    Retired,
    FailClosed,
}

pub(crate) struct Lifetime {
    retry: Instant,
    retiring: Option<Instant>,
    dropped: bool,
}
impl Lifetime {
    pub fn new(now: Instant) -> Self {
        Self {
            retry: now,
            retiring: None,
            dropped: false,
        }
    }
    pub fn stop(&mut self, now: Instant) {
        self.retiring.get_or_insert(now);
    }
    pub fn poll(
        &mut self,
        now: Instant,
        daemon: bool,
        connected: bool,
        native_retired: bool,
        public_empty: bool,
    ) -> Action {
        if let Some(start) = self.retiring {
            if !self.dropped {
                self.dropped = true;
                return Action::Drop;
            }
            if native_retired && public_empty {
                return Action::Retired;
            }
            if now.saturating_duration_since(start) >= RETIRE_LIMIT {
                return Action::FailClosed;
            }
            return Action::Wait;
        }
        if connected || now < self.retry {
            return Action::Wait;
        }
        self.retry = now + RETRY_INTERVAL;
        if daemon {
            Action::Reconnect
        } else {
            Action::Reopen
        }
    }
    pub fn restart(&mut self, now: Instant) {
        *self = Self::new(now);
    }
}
