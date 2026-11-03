//! Narrow C bridge ABI. No private public-library or Smithay layouts cross it.
use std::ffi::{c_char, c_void};
pub const INPUT_TYPE_TABLET_FRAME: u32 = 16;
pub const TABLET_TOOL_PEN: u32 = 1;
pub const TABLET_TOOL_ERASER: u32 = 2;
pub const TABLET_CAP_PRESSURE: u32 = 1;
pub const TABLET_CAP_TILT: u32 = 2;
pub const TABLET_CAP_DISTANCE: u32 = 4;
pub const TABLET_FLAG_PROXIMITY: u32 = 1;
pub const TABLET_FLAG_TIP_DOWN: u32 = 2;
pub const TABLET_FLAG_CANCELED: u32 = 4;
pub const TABLET_BUTTON_PRIMARY: u32 = 1;
pub const TABLET_BUTTON_SECONDARY: u32 = 2;
#[repr(C)]
pub struct Session {
    _opaque: [u8; 0],
}
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Target {
    pub generation: u64,
    pub index: u32,
    pub count: u32,
    pub width: u32,
    pub height: u32,
}
impl Target {
    /// Pool metadata is immutable within one public session generation. Slot
    /// selection may change without invalidating all imported images.
    pub fn same_pool(self, other: Self) -> bool {
        self.generation == other.generation
            && self.count == other.count
            && self.width == other.width
            && self.height == other.height
    }
}

#[repr(C)]
#[derive(Default)]
pub struct Image {
    pub modifier: u64,
    pub width: u32,
    pub height: u32,
    pub stride: u32,
    pub offset: u32,
    pub fourcc: u32,
    pub fd: i32,
}
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct Output {
    pub width: u32,
    pub height: u32,
    pub refresh_mhz: u32,
}
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub struct Fds {
    pub scene: i32,
    pub ready: i32,
    pub input: i32,
}
#[repr(C)]
#[derive(Default)]
pub struct Event {
    pub commit_id: u64,
    pub generation: u64,
    pub buffer_id: u64,
    pub presentation_ns: u64,
    pub kind: u32,
    pub reason: u32,
    pub index: u32,
    pub count: u32,
    pub width: u32,
    pub height: u32,
    pub refresh_mhz: u32,
    pub release_fd: i32,
}
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct Input {
    pub kind: u32,
    pub size: u32,
    pub resource_type: u32,
    pub fd_count: u32,
    pub action: i32,
    pub code: i32,
    pub pointer_id: i32,
    pub discrete: i32,
    pub x: f32,
    pub y: f32,
    pub dx: f32,
    pub dy: f32,
    pub value: f32,
    pub tablet_previous: TabletSample,
    pub tablet_current: TabletSample,
}
#[repr(C)]
#[derive(Clone, Copy, Debug, Default)]
pub struct TabletSample {
    pub tool_id: u32,
    pub tool_type: u32,
    pub capabilities: u32,
    pub device_id: u32,
    pub flags: u32,
    pub buttons: u32,
    pub time_msec: u32,
    pub x: f32,
    pub y: f32,
    pub pressure: f32,
    pub tilt_x: f32,
    pub tilt_y: f32,
    pub distance: f32,
}
#[link(name = "anland_denial_bridge")]
unsafe extern "C" {
    pub fn ad_open(endpoint: *const c_char) -> *mut Session;
    pub fn ad_close(session: *mut Session);
    pub fn ad_connected(session: *mut Session) -> i32;
    pub fn ad_daemon_alive(session: *mut Session) -> i32;
    pub fn ad_reconnect(session: *mut Session) -> i32;
    pub fn ad_reopen(session: *mut Session, endpoint: *const c_char) -> i32;
    pub fn ad_drop(session: *mut Session);
    pub fn ad_get_fds(session: *mut Session, out: *mut Fds) -> i32;
    pub fn ad_get_output(session: *mut Session, out: *mut Output) -> i32;
    pub fn ad_writable(session: *mut Session, out: *mut Target) -> i32;
    pub fn ad_import(session: *mut Session, generation: u64, index: u32, out: *mut Image) -> i32;
    pub fn ad_commit(session: *mut Session, target: *const Target, fence: i32, id: *mut u64)
    -> i32;
    pub fn ad_present(session: *mut Session) -> i32;
    pub fn ad_pump(session: *mut Session) -> i32;
    pub fn ad_next_event(session: *mut Session, event: *mut Event) -> i32;
    pub fn ad_events_pending(session: *mut Session) -> usize;
    pub fn ad_fence_signaled(fd: i32) -> i32;
    pub fn ad_poll_input(session: *mut Session, out: *mut Input) -> i32;
    pub fn ad_input_header_pending(session: *mut Session) -> i32;
    pub fn ad_read_payload(session: *mut Session, data: *mut c_void, size: usize) -> i32;
    pub fn ad_read_fds(session: *mut Session, fds: *mut i32, capacity: u32, count: *mut u32)
    -> i32;
    pub fn ad_clipboard(session: *mut Session, data: *const c_void, size: usize) -> i32;
    pub fn ad_flush(session: *mut Session) -> i32;
    pub fn ad_pending_output(session: *mut Session) -> usize;
    pub fn ad_capture_pointer(session: *mut Session, capture: u32) -> i32;
}
