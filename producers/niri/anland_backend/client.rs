//! Owning Rust facade for the shared producer device and scene.
//! This module knows no niri or Smithay types; their adapter lives next to it.
#[path = "ffi.rs"]
pub mod ffi;

use std::ffi::CString;
use std::io;
use std::os::fd::{FromRawFd, OwnedFd};
use std::ptr::NonNull;

pub use ffi::{InputEvent, Output, SceneEvent};

pub const PRESENTED: u32 = ffi::EVENT_PRESENTED;
pub const BUFFER_RELEASED: u32 = ffi::EVENT_BUFFER_RELEASED;
pub const COMMIT_DROPPED: u32 = ffi::EVENT_COMMIT_DROPPED;
pub const OUTPUT_CHANGED: u32 = ffi::EVENT_OUTPUT_CHANGED;
pub const RENDER_TARGET_READY: u32 = ffi::EVENT_RENDER_TARGET_READY;
pub const INPUT_TYPE_CLIPBOARD: u32 = ffi::INPUT_TYPE_CLIPBOARD;
pub const INPUT_TYPE_TEXT_INPUT: u32 = ffi::INPUT_TYPE_TEXT_INPUT;
pub const INPUT_TYPE_RESOURCE: u32 = ffi::INPUT_TYPE_RESOURCE;

fn failed(action: &str) -> io::Error {
    io::Error::other(format!("anland: {action} failed"))
}

/// The bridge and its scene are single-thread owned by the compositor event loop.
pub struct Client(NonNull<ffi::Bridge>);

pub struct RenderTarget {
    pub generation: u64,
    pub index: u32,
    pub count: u32,
    pub width: u32,
    pub height: u32,
    pub format: u32,
    pub stride: u32,
    pub offset: u32,
    pub modifier: u64,
    pub fd: OwnedFd,
}

/// A scene event whose release fence, if any, closes on drop.
pub struct Event {
    pub data: SceneEvent,
    pub release_fence: Option<OwnedFd>,
}

impl Client {
    pub fn open(socket: &str) -> io::Result<Self> {
        let socket = CString::new(socket).map_err(|_| failed("invalid socket name"))?;
        let raw = unsafe { ffi::anland_niri_open(socket.as_ptr()) };
        NonNull::new(raw)
            .map(Self)
            .ok_or_else(|| failed("open (AWL is unsupported)"))
    }

    pub fn pump(&mut self) -> io::Result<()> {
        if unsafe { ffi::anland_niri_pump(self.0.as_ptr(), 0) } < 0 {
            return Err(failed("pump"));
        }
        Ok(())
    }

    pub fn reconnect(&mut self) -> io::Result<()> {
        if unsafe { ffi::anland_niri_reconnect(self.0.as_ptr()) } != 0 {
            return Err(failed("reconnect"));
        }
        Ok(())
    }

    pub fn reopen(&mut self, socket: &str) -> io::Result<()> {
        let socket = CString::new(socket).map_err(|_| failed("invalid socket name"))?;
        if unsafe { ffi::anland_niri_reopen(self.0.as_ptr(), socket.as_ptr()) } != 0 {
            return Err(failed("reopen"));
        }
        Ok(())
    }

    pub fn connected(&self) -> bool {
        unsafe { ffi::anland_niri_connected(self.0.as_ptr()) != 0 }
    }
    pub fn daemon_alive(&self) -> bool {
        unsafe { ffi::anland_niri_daemon_alive(self.0.as_ptr()) != 0 }
    }
    pub fn generation(&self) -> u64 {
        unsafe { ffi::anland_niri_generation(self.0.as_ptr()) }
    }
    pub fn drop_session(&mut self) {
        unsafe { ffi::anland_niri_drop_session(self.0.as_ptr()) };
    }

    pub fn output(&mut self) -> io::Result<Output> {
        let mut out = Output::default();
        if unsafe { ffi::anland_niri_output_info(self.0.as_ptr(), &mut out) } != 0 {
            return Err(failed("output unavailable"));
        }
        Ok(out)
    }

    pub fn target(&mut self) -> io::Result<RenderTarget> {
        let mut out = ffi::Target::default();
        if unsafe { ffi::anland_niri_get_target(self.0.as_ptr(), &mut out) } != 0 || out.fd < 0 {
            return Err(failed("render target unavailable"));
        }
        // The C API returned a fresh dup. From this point Rust alone owns it.
        let fd = unsafe { OwnedFd::from_raw_fd(out.fd) };
        Ok(RenderTarget {
            generation: out.generation,
            index: out.index,
            count: out.count,
            width: out.width,
            height: out.height,
            format: out.format,
            stride: out.stride,
            offset: out.offset,
            modifier: out.modifier,
            fd,
        })
    }

    /// Commit only the currently published target. `fence` is borrowed by the C
    /// call and duplicated by the legacy backend before this call returns.
    pub fn submit(
        &mut self,
        generation: u64,
        index: u32,
        fence: Option<&OwnedFd>,
    ) -> io::Result<u64> {
        use std::os::fd::AsRawFd;
        let mut id = 0;
        let fd = fence.map_or(-1, |f| f.as_raw_fd());
        if unsafe { ffi::anland_niri_submit(self.0.as_ptr(), generation, index, fd, &mut id) } != 0
        {
            return Err(failed("submit"));
        }
        Ok(id)
    }

    pub fn events(&mut self) -> io::Result<Vec<Event>> {
        let mut result = Vec::new();
        // Events are drained in fixed chunks: never drop a caller-owned fence
        // just because the scene has more than one chunk pending.
        loop {
            let mut raw = [SceneEvent::default(); 64];
            let mut count = 0usize;
            if unsafe {
                ffi::anland_niri_dispatch(self.0.as_ptr(), raw.as_mut_ptr(), raw.len(), &mut count)
            } != 0
            {
                return Err(failed("dispatch"));
            }
            if count > raw.len() {
                return Err(failed("invalid event count"));
            }
            for mut data in raw.into_iter().take(count) {
                let fd = std::mem::replace(&mut data.release_fence_fd, -1);
                let release_fence = (fd >= 0).then(|| unsafe { OwnedFd::from_raw_fd(fd) });
                result.push(Event {
                    data,
                    release_fence,
                });
            }
            if count < 64 {
                break;
            }
        }
        Ok(result)
    }

    /// A clipboard/text/resource event requires consuming its associated data
    /// via `read_payload` or `read_fds` before polling another input event.
    pub fn poll_input(&mut self) -> io::Result<Option<InputEvent>> {
        let mut event = InputEvent::default();
        match unsafe { ffi::anland_niri_poll_input(self.0.as_ptr(), &mut event) } {
            1 => Ok(Some(event)),
            0 => Ok(None),
            _ => Err(failed("input or session changed")),
        }
    }

    pub fn read_payload(&mut self, size: usize) -> io::Result<Vec<u8>> {
        const MAX_PAYLOAD: usize = 1024 * 1024;
        if size > MAX_PAYLOAD {
            return Err(failed("payload too large"));
        }
        let mut bytes = vec![0; size];
        if size > 0
            && unsafe {
                ffi::anland_niri_read_payload(self.0.as_ptr(), bytes.as_mut_ptr().cast(), size, 100)
            } != 1
        {
            return Err(failed("payload read"));
        }
        Ok(bytes)
    }

    pub fn read_fds(&mut self) -> io::Result<Vec<OwnedFd>> {
        let mut raw = [-1; 9];
        let mut count = 0;
        let rc = unsafe {
            ffi::anland_niri_read_fds(self.0.as_ptr(), raw.as_mut_ptr(), 9, &mut count, 100)
        };
        if count < 0 || count as usize > raw.len() {
            for fd in raw.into_iter().filter(|fd| *fd >= 0) {
                unsafe { libc_close(fd) };
            }
            return Err(failed("invalid fd count"));
        }
        let fds: Vec<OwnedFd> = raw
            .into_iter()
            .take(count as usize)
            .filter(|fd| *fd >= 0)
            .map(|fd| unsafe { OwnedFd::from_raw_fd(fd) })
            .collect();
        if rc <= 0 {
            return Err(failed("resource fd read"));
        }
        Ok(fds)
    }

    pub fn send_clipboard(&mut self, bytes: &[u8]) -> io::Result<()> {
        if unsafe {
            ffi::anland_niri_send_clipboard(self.0.as_ptr(), bytes.as_ptr().cast(), bytes.len())
        } < 0
        {
            return Err(failed("clipboard send"));
        }
        Ok(())
    }

    pub fn request_resources(&mut self, service: u32) -> io::Result<()> {
        if unsafe { ffi::anland_niri_request_resources(self.0.as_ptr(), service) } < 0 {
            return Err(failed("resource request"));
        }
        Ok(())
    }

    pub fn audio_fd(&mut self) -> i32 {
        unsafe { ffi::anland_niri_audio_fd(self.0.as_ptr()) }
    }
}

extern "C" {
    #[link_name = "close"]
    fn libc_close(fd: i32) -> i32;
}

impl Drop for Client {
    fn drop(&mut self) {
        unsafe { ffi::anland_niri_close(self.0.as_ptr()) };
    }
}