//! Bounded client-selection reader. Only complete, current selections are usable.
use std::os::fd::{AsRawFd, OwnedFd};
use std::sync::atomic::{AtomicU64, Ordering};
use std::time::{Duration, Instant};

pub(super) struct ReadResult {
    pub selection: u64,
    pub session: u64,
    pub text: Option<String>,
}

pub(super) fn read_selection(fd: OwnedFd, epoch: &AtomicU64, selection: u64) -> Option<String> {
    const MAX_BYTES: usize = 1024 * 1024;
    let deadline = Instant::now() + Duration::from_secs(2);
    let mut bytes = Vec::new();
    loop {
        if epoch.load(Ordering::Acquire) != selection || Instant::now() >= deadline {
            return None;
        }
        let mut pfd = libc::pollfd {
            fd: fd.as_raw_fd(), events: libc::POLLIN, revents: 0,
        };
        let timeout = deadline.saturating_duration_since(Instant::now()).as_millis().min(100) as i32;
        let ready = unsafe { libc::poll(&mut pfd, 1, timeout) };
        if ready < 0 {
            if std::io::Error::last_os_error().kind() == std::io::ErrorKind::Interrupted {
                continue;
            }
            return None;
        }
        if ready == 0 { continue; }
        if pfd.revents & (libc::POLLERR | libc::POLLNVAL) != 0 { return None; }
        let mut chunk = [0u8; 4096];
        let count = unsafe { libc::read(fd.as_raw_fd(), chunk.as_mut_ptr().cast(), chunk.len()) };
        if count == 0 {
            // POLLHUP alone is not EOF: drain its remaining bytes before accepting.
            if epoch.load(Ordering::Acquire) != selection || Instant::now() >= deadline {
                return None;
            }
            return String::from_utf8(bytes).ok();
        }
        if count < 0 {
            match std::io::Error::last_os_error().kind() {
                std::io::ErrorKind::Interrupted | std::io::ErrorKind::WouldBlock => continue,
                _ => return None,
            }
        }
        let count = count as usize;
        if count > MAX_BYTES - bytes.len() { return None; }
        bytes.extend_from_slice(&chunk[..count]);
    }
}
