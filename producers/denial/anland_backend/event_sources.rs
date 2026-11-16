//! calloop watches only wake the outer iteration. Public pump/dispatch/input calls remain
//! serialized in the compositor's outer iteration, outside source callbacks.
use super::client::{Client, Fds};
use crate::RuntimeState;
use smithay::reexports::calloop::generic::Generic;
use smithay::reexports::calloop::timer::{TimeoutAction, Timer};
use smithay::reexports::calloop::{Interest, LoopHandle, Mode, PostAction, RegistrationToken};
use std::io;
use std::os::fd::BorrowedFd;
use std::time::Duration;

const MAINTENANCE_INTERVAL: Duration = Duration::from_millis(100);

pub(crate) struct EventSources {
    handle: LoopHandle<'static, RuntimeState>,
    sources: Vec<RegistrationToken>,
    timer: Option<RegistrationToken>,
    fences: Vec<RegistrationToken>,
    installed: Option<(Fds, bool)>,
    watch_input: bool,
}

impl EventSources {
    pub fn watch_input(&mut self, watch: bool) {
        if self.watch_input != watch {
            self.watch_input = watch;
            self.invalidate();
        }
    }
    pub fn new(handle: LoopHandle<'static, RuntimeState>) -> io::Result<Self> {
        let timer = handle
            .insert_source(
                Timer::from_duration(MAINTENANCE_INTERVAL),
                move |_, _, _| {
                    // Also run when a peer stops halfway through an input header or
                    // payload: fd readiness alone cannot enforce the public deadline.
                    TimeoutAction::ToDuration(MAINTENANCE_INTERVAL)
                },
            )
            .map_err(|error| io::Error::other(error.to_string()))?;
        Ok(Self {
            handle,
            sources: Vec::new(),
            timer: Some(timer),
            fences: Vec::new(),
            installed: None,
            watch_input: false,
        })
    }

    /// Duplicate live owners each iteration. Removal precedes reinstall, so fd
    /// number reuse can never retain an old fence callback/owner.
    pub fn refresh_fences<'a>(
        &mut self,
        fds: impl IntoIterator<Item = BorrowedFd<'a>>,
    ) -> io::Result<()> {
        for token in self.fences.drain(..) {
            self.handle.remove(token);
        }
        for fd in fds {
            let owned = fd.try_clone_to_owned()?;
            let token = self
                .handle
                .insert_source(
                    Generic::new(owned, Interest::READ, Mode::Level),
                    |_, _, _: &mut RuntimeState| Ok(PostAction::Continue),
                )
                .map_err(|error| io::Error::other(error.to_string()))?;
            self.fences.push(token);
        }
        Ok(())
    }

    /// Mandatory BEFORE drop/reconnect/reopen, even if new fd numbers will match.
    pub fn invalidate(&mut self) {
        for token in self.sources.drain(..) {
            self.handle.remove(token);
        }
        self.installed = None;
    }

    pub fn refresh(&mut self, client: &Client) -> io::Result<()> {
        let fds = client.fds()?;
        let pending = client.pending_output() != 0;
        if self.installed == Some((fds, pending)) {
            return Ok(());
        }
        self.invalidate();
        let result = (|| {
            for (fd, interest) in [
                (fds.scene, Interest::READ),
                (fds.ready, Interest::READ),
                (
                    if self.watch_input { fds.input } else { -1 },
                    Interest::READ,
                ),
                (if pending { fds.input } else { -1 }, Interest::WRITE),
            ] {
                if fd < 0 {
                    continue;
                }
                // SAFETY: the client owns these borrowed fds, and no session
                // mutation/callback occurs during this synchronous duplication.
                // The duplicate belongs to Generic and is closed on removal.
                let owned = unsafe { BorrowedFd::borrow_raw(fd) }.try_clone_to_owned()?;
                let token = self
                    .handle
                    .insert_source(
                        Generic::new(owned, interest, Mode::Level),
                        move |_, _, _: &mut RuntimeState| Ok(PostAction::Continue),
                    )
                    .map_err(|error| io::Error::other(error.to_string()))?;
                self.sources.push(token);
            }
            Ok(())
        })();
        if result.is_err() {
            self.invalidate();
        } else {
            self.installed = Some((fds, pending));
        }
        result
    }
}

impl Drop for EventSources {
    fn drop(&mut self) {
        self.invalidate();
        for token in self.fences.drain(..) {
            self.handle.remove(token);
        }
        if let Some(token) = self.timer.take() {
            self.handle.remove(token);
        }
    }
}
