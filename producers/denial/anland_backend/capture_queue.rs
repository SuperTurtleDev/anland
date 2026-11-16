//! Short-lived pins for requested captures, never a second presentation queue.
//! A source is pinned at publication (before commit/present can release it), and
//! becomes readable only after its own ACK and acquire fence. No GPU read starts
//! here, so dropping an obsolete/unpresented pin cannot cancel in-flight work.
use std::collections::BTreeMap;
use std::io;
use std::os::fd::{AsFd, BorrowedFd, OwnedFd};
use std::time::{Duration, Instant};

const CAPTURE_FENCE_TIMEOUT: Duration = Duration::from_secs(5);

#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub(crate) struct CaptureSource {
    pub serial: u64,
    pub index: usize,
    pub revision: u64,
    pub screenshot_request_id: Option<u64>,
}

struct Captured<L> {
    source: CaptureSource,
    _lease: L,
    acquire: Option<OwnedFd>,
    published_at: Instant,
    presented: bool,
}

pub(crate) struct CaptureQueue<L> {
    sources: BTreeMap<u64, Captured<L>>,
    latest_presented: u64,
}

impl<L> Default for CaptureQueue<L> {
    fn default() -> Self {
        Self {
            sources: BTreeMap::new(),
            latest_presented: 0,
        }
    }
}

impl<L> CaptureQueue<L> {
    pub fn retain(
        &mut self,
        source: CaptureSource,
        lease: L,
        acquire: Option<OwnedFd>,
        now: Instant,
    ) -> io::Result<()> {
        if source.serial == 0 || self.sources.contains_key(&source.serial) {
            return Err(io::Error::other("duplicate/invalid Anland capture serial"));
        }
        self.sources.insert(
            source.serial,
            Captured {
                source,
                _lease: lease,
                acquire,
                published_at: now,
                presented: false,
            },
        );
        Ok(())
    }

    pub fn source(&self, serial: u64) -> Option<CaptureSource> {
        self.sources.get(&serial).map(|capture| capture.source)
    }

    pub fn complete(&mut self, serial: u64, accepted: bool) {
        if !accepted {
            self.sources.remove(&serial);
            return;
        }
        self.latest_presented = self.latest_presented.max(serial);
        if let Some(capture) = self.sources.get_mut(&serial) {
            capture.presented = true;
        }
        // Ordinary protocol requests read the newest visible frame. A tagged
        // selection must keep its exact pre-overlay frame until it is frozen.
        self.sources.retain(|serial, capture| {
            *serial >= self.latest_presented || capture.source.screenshot_request_id.is_some()
        });
    }

    pub fn ready(
        &self,
        now: Instant,
        mut signaled: impl FnMut(BorrowedFd<'_>) -> io::Result<bool>,
    ) -> io::Result<Vec<CaptureSource>> {
        let mut ready = Vec::new();
        for capture in self.sources.values().filter(|capture| capture.presented) {
            let readable = match capture.acquire.as_ref() {
                Some(fd) => signaled(fd.as_fd())?,
                None => true,
            };
            if readable {
                ready.push(capture.source);
            } else if now.saturating_duration_since(capture.published_at) >= CAPTURE_FENCE_TIMEOUT {
                // The caller retains this queue on exceptional teardown. Never
                // drop its lease as a side effect of a timeout or query error.
                return Err(io::Error::new(
                    io::ErrorKind::TimedOut,
                    "Anland capture acquire fence timed out; retain GPU owners",
                ));
            }
        }
        Ok(ready)
    }

    /// Call only after a read finished or transferred ownership to the shared
    /// capture manager's fence-owned source read. An error keeps this pin alive.
    pub fn finish(&mut self, serial: u64) {
        self.sources.remove(&serial);
    }

    pub fn fences(&self) -> Vec<BorrowedFd<'_>> {
        self.sources
            .values()
            .filter(|capture| capture.presented)
            .filter_map(|capture| capture.acquire.as_ref().map(AsFd::as_fd))
            .collect()
    }

    pub fn is_latest(&self, serial: u64) -> bool {
        serial == self.latest_presented
    }

    pub fn cancel_screenshot(&mut self, request_id: u64) {
        self.sources
            .retain(|_, capture| capture.source.screenshot_request_id != Some(request_id));
    }

    pub fn clear(&mut self) {
        self.sources.clear();
    }
}
