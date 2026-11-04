//! Single-threaded owner of the common Anland presentation facade.
use super::ffi;
pub use ffi::{Fds, Input, Output, Target};
use std::ffi::CStr;
use std::io;
use std::marker::PhantomData;
use std::os::fd::{AsRawFd, BorrowedFd, FromRawFd, OwnedFd};
use std::ptr::{self, NonNull};
use std::rc::Rc;

pub struct Client {
    session: NonNull<ffi::Session>,
    _single_thread: PhantomData<Rc<()>>,
}
pub struct Image {
    pub fd: OwnedFd,
    pub width: u32,
    pub height: u32,
    pub stride: u32,
    pub offset: u32,
    pub fourcc: u32,
    pub modifier: u64,
}
#[derive(Debug)]
pub enum Event {
    Presented {
        commit: u64,
        software_time_ns: u64,
    },
    Released {
        commit: u64,
        buffer: u64,
        fence: Option<OwnedFd>,
    },
    Dropped {
        commit: u64,
        reason: u32,
    },
    Output(Output),
    Target {
        generation: u64,
        index: u32,
        count: u32,
    },
}
fn check(rc: i32) -> io::Result<i32> {
    if rc < 0 {
        Err(io::Error::from_raw_os_error(-rc))
    } else {
        Ok(rc)
    }
}
fn fd(raw: i32) -> Option<OwnedFd> {
    // SAFETY: the bridge transfers only newly owned descriptors in image/events.
    (raw >= 0).then(|| unsafe { OwnedFd::from_raw_fd(raw) })
}
pub fn fence_signaled(fence: BorrowedFd<'_>) -> io::Result<bool> {
    // SAFETY: borrowed descriptor is live; bridge only queries sync_file status.
    Ok(check(unsafe { ffi::ad_fence_signaled(fence.as_raw_fd()) })? == 1)
}
impl Client {
    pub fn open(endpoint: Option<&CStr>) -> io::Result<Self> {
        // SAFETY: endpoint is NUL terminated and borrowed only for this call.
        let session = unsafe { ffi::ad_open(endpoint.map_or(ptr::null(), CStr::as_ptr)) };
        Ok(Self {
            session: NonNull::new(session).ok_or_else(|| {
                io::Error::new(io::ErrorKind::NotConnected, "Anland daemon unavailable")
            })?,
            _single_thread: PhantomData,
        })
    }
    pub fn connected(&self) -> bool {
        // SAFETY: the live session is owned by self; !Send/!Sync serializes access.
        unsafe { ffi::ad_connected(self.session.as_ptr()) != 0 }
    }
    pub fn daemon_alive(&self) -> bool {
        // SAFETY: same live session ownership.
        unsafe { ffi::ad_daemon_alive(self.session.as_ptr()) != 0 }
    }
    pub fn reconnect(&mut self) -> io::Result<()> {
        // SAFETY: exclusive, live session; imports/watchers must be retired by caller.
        check(unsafe { ffi::ad_reconnect(self.session.as_ptr()) })?;
        Ok(())
    }
    pub fn reopen(&mut self, endpoint: Option<&CStr>) -> io::Result<()> {
        // SAFETY: exclusive session and valid borrowed C string.
        check(unsafe {
            ffi::ad_reopen(
                self.session.as_ptr(),
                endpoint.map_or(ptr::null(), CStr::as_ptr),
            )
        })?;
        Ok(())
    }
    pub fn drop_session(&mut self) {
        // SAFETY: exclusive live session; native cleanup is caller's responsibility.
        unsafe { ffi::ad_drop(self.session.as_ptr()) }
    }
    /// Borrowed fd numbers. Never close them; rebuild watchers on each lifecycle
    /// transition, even when the new owner receives the same descriptor number.
    pub fn fds(&self) -> io::Result<Fds> {
        let mut out = Fds::default();
        // SAFETY: initialized output storage and live session.
        check(unsafe { ffi::ad_get_fds(self.session.as_ptr(), &mut out) })?;
        Ok(out)
    }
    pub fn output(&self) -> io::Result<Output> {
        let mut out = Output::default();
        // SAFETY: initialized output storage and live session.
        check(unsafe { ffi::ad_get_output(self.session.as_ptr(), &mut out) })?;
        Ok(out)
    }
    pub fn writable(&self) -> io::Result<Option<Target>> {
        let mut out = Target::default();
        // SAFETY: initialized output storage and live session.
        match check(unsafe { ffi::ad_writable(self.session.as_ptr(), &mut out) }) {
            Ok(_) => Ok(Some(out)),
            Err(e) if e.kind() == io::ErrorKind::WouldBlock => Ok(None),
            Err(e) => Err(e),
        }
    }
    pub fn import(&mut self, generation: u64, index: u32) -> io::Result<Image> {
        let mut out = ffi::Image {
            fd: -1,
            ..Default::default()
        };
        // SAFETY: output storage valid; returned fd is a caller-owned duplicate.
        check(unsafe { ffi::ad_import(self.session.as_ptr(), generation, index, &mut out) })?;
        Ok(Image {
            fd: fd(out.fd).ok_or_else(|| io::Error::other("bridge returned no dmabuf"))?,
            width: out.width,
            height: out.height,
            stride: out.stride,
            offset: out.offset,
            fourcc: out.fourcc,
            modifier: out.modifier,
        })
    }
    /// Record this returned ID against the native frame BEFORE calling present.
    pub fn commit(&mut self, target: &Target, fence: Option<BorrowedFd<'_>>) -> io::Result<u64> {
        let mut id = 0;
        // SAFETY: all borrows live through the call; bridge never takes the fence.
        check(unsafe {
            ffi::ad_commit(
                self.session.as_ptr(),
                target,
                fence.map_or(-1, |f| f.as_raw_fd()),
                &mut id,
            )
        })?;
        Ok(id)
    }
    pub fn present(&mut self) -> io::Result<()> {
        // SAFETY: exclusive session. Even on failure caller must drain events.
        check(unsafe { ffi::ad_present(self.session.as_ptr()) })?;
        Ok(())
    }
    pub fn pump(&mut self) -> io::Result<()> {
        // SAFETY: exclusive session; bridge always uses zero timeout.
        check(unsafe { ffi::ad_pump(self.session.as_ptr()) })?;
        Ok(())
    }
    pub fn next_event(&mut self) -> io::Result<Option<Event>> {
        let mut out = ffi::Event {
            release_fd: -1,
            ..Default::default()
        };
        // SAFETY: one initialized event; any returned fence is immediately owned.
        let rc = unsafe { ffi::ad_next_event(self.session.as_ptr(), &mut out) };
        let fence = fd(out.release_fd);
        if check(rc)? == 0 {
            return Ok(None);
        }
        Ok(Some(match out.kind {
            1 => Event::Presented {
                commit: out.commit_id,
                software_time_ns: out.presentation_ns,
            },
            2 => Event::Released {
                commit: out.commit_id,
                buffer: out.buffer_id,
                fence,
            },
            3 => Event::Dropped {
                commit: out.commit_id,
                reason: out.reason,
            },
            4 => Event::Output(Output {
                width: out.width,
                height: out.height,
                refresh_mhz: out.refresh_mhz,
            }),
            5 => Event::Target {
                generation: out.generation,
                index: out.index,
                count: out.count,
            },
            _ => return Err(io::Error::other("unknown Anland bridge event")),
        }))
    }
    pub fn events_pending(&self) -> usize {
        // SAFETY: same live, single-threaded session; no ownership is transferred.
        unsafe { ffi::ad_events_pending(self.session.as_ptr()) }
    }
    pub fn input_header_pending(&self) -> bool {
        // SAFETY: read-only query on the same live, single-threaded session.
        unsafe { ffi::ad_input_header_pending(self.session.as_ptr()) != 0 }
    }
    pub fn poll_input(&mut self) -> io::Result<Option<Input>> {
        let mut out = Input::default();
        // SAFETY: initialized storage; caller must complete payload before repoll.
        Ok(
            (check(unsafe { ffi::ad_poll_input(self.session.as_ptr(), &mut out) })? == 1)
                .then_some(out),
        )
    }
    pub fn read_payload(&mut self, bytes: &mut [u8]) -> io::Result<bool> {
        // SAFETY: mutable slice supplies its full capacity; synchronous borrow.
        Ok(check(unsafe {
            ffi::ad_read_payload(
                self.session.as_ptr(),
                bytes.as_mut_ptr().cast(),
                bytes.len(),
            )
        })? == 1)
    }
    pub fn read_fds(&mut self) -> io::Result<Option<Vec<OwnedFd>>> {
        let mut raw = [-1; 64];
        let mut count = 0;
        // SAFETY: output array has the declared capacity; C transfers only on 1.
        if check(unsafe {
            ffi::ad_read_fds(self.session.as_ptr(), raw.as_mut_ptr(), 64, &mut count)
        })? == 0
        {
            return Ok(None);
        }
        Ok(Some(
            raw[..count as usize]
                .iter()
                .filter_map(|&n| fd(n))
                .collect(),
        ))
    }
    pub fn clipboard(&mut self, bytes: &[u8]) -> io::Result<()> {
        // SAFETY: valid borrowed bytes; common queue copies them before returning.
        check(unsafe {
            ffi::ad_clipboard(self.session.as_ptr(), bytes.as_ptr().cast(), bytes.len())
        })?;
        Ok(())
    }
    pub fn flush(&mut self) -> io::Result<bool> {
        // SAFETY: exclusive session; true means a writable watcher remains needed.
        Ok(check(unsafe { ffi::ad_flush(self.session.as_ptr()) })? != 0)
    }
    pub fn pending_output(&self) -> usize {
        // SAFETY: live session; query only, not an ownership reservation.
        unsafe { ffi::ad_pending_output(self.session.as_ptr()) }
    }
    pub fn capture_pointer(&mut self, capture: bool) -> io::Result<()> {
        // SAFETY: the bridge translates the boolean through the public device API.
        check(unsafe { ffi::ad_capture_pointer(self.session.as_ptr(), u32::from(capture)) })?;
        Ok(())
    }
    pub fn request_resources(&mut self, service_type: u32) -> io::Result<()> {
        check(unsafe { ffi::ad_request_resources(self.session.as_ptr(), service_type) })?;
        Ok(())
    }
    pub fn scheduling(&mut self, pid: i32, flags: u8) -> io::Result<()> {
        check(unsafe { ffi::ad_scheduling(self.session.as_ptr(), pid, flags) })?;
        Ok(())
    }
    pub fn resources(
        &mut self,
        service_type: u32,
        invalid: bool,
        fds: Vec<OwnedFd>,
    ) -> io::Result<()> {
        use std::os::fd::IntoRawFd;
        let raw_fds = fds.into_iter().map(IntoRawFd::into_raw_fd).collect::<Vec<_>>();
        check(unsafe {
            ffi::ad_resources(
                self.session.as_ptr(),
                service_type,
                u32::from(invalid),
                raw_fds.as_ptr(),
                raw_fds.len() as u32,
            )
        })?;
        Ok(())
    }
}
impl Drop for Client {
    fn drop(&mut self) {
        // SAFETY: sole owner; caller must stop borrowed-fd users before dropping.
        unsafe { ffi::ad_close(self.session.as_ptr()) }
    }
}
