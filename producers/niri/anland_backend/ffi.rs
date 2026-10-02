//! Layout-free Rust declarations for the single producer-side niri C adapter.
#![allow(dead_code)]
use std::ffi::{c_char, c_int, c_void};

pub const MAX_BUFS: usize = 8;
pub const EVENT_PRESENTED: u32 = 1;
pub const EVENT_BUFFER_RELEASED: u32 = 2;
pub const EVENT_COMMIT_DROPPED: u32 = 3;
pub const EVENT_OUTPUT_CHANGED: u32 = 4;
pub const EVENT_RENDER_TARGET_READY: u32 = 5;
pub const INPUT_TYPE_TOUCH: u32 = 1;
pub const INPUT_TYPE_KEY: u32 = 2;
pub const INPUT_TYPE_POINTER_MOTION: u32 = 3;
pub const INPUT_TYPE_POINTER_BUTTON: u32 = 4;
pub const INPUT_TYPE_POINTER_AXIS: u32 = 5;
pub const INPUT_TYPE_TOUCH_FRAME: u32 = 6;
pub const INPUT_TYPE_CLIPBOARD: u32 = 8;
pub const INPUT_TYPE_TEXT_INPUT: u32 = 9;
pub const INPUT_TYPE_RESOURCE: u32 = 11;
pub const INPUT_ACTION_DOWN: i32 = 0;
pub const INPUT_ACTION_UP: i32 = 1;
pub const INPUT_ACTION_MOVE: i32 = 2;
pub const SERVICE_TYPE_CAMERA: u32 = 1;

#[repr(C)]
pub struct Bridge {
    _private: [u8; 0],
}
#[repr(C)]
#[derive(Default, Clone, Copy)]
pub struct Output {
    pub width: u32,
    pub height: u32,
    pub refresh_mhz: u32,
    pub format: u32,
}
#[repr(C)]
#[derive(Default)]
pub struct Target {
    pub generation: u64,
    pub index: u32,
    pub count: u32,
    pub width: u32,
    pub height: u32,
    pub format: u32,
    pub stride: u32,
    pub offset: u32,
    pub modifier: u64,
    pub fd: c_int,
}
#[repr(C)]
#[derive(Default, Clone, Copy)]
pub struct SceneEvent {
    pub type_: u32,
    pub generation: u64,
    pub commit_id: u64,
    pub index: u32,
    pub count: u32,
    pub width: u32,
    pub height: u32,
    pub refresh_mhz: u32,
    pub presentation_ns: u64,
    pub buffer_id: u64,
    pub release_fence_fd: c_int,
}
#[repr(C)]
#[derive(Default, Clone, Copy)]
pub struct InputEvent {
    pub type_: u32,
    pub action: i32,
    pub code: i32,
    pub pointer_id: i32,
    pub discrete: i32,
    pub x: f32,
    pub y: f32,
    pub dx: f32,
    pub dy: f32,
    pub value: f32,
    pub payload_size: u32,
    pub resource_type: u32,
    pub fd_count: u32,
}
extern "C" {
    pub fn anland_niri_open(endpoint: *const c_char) -> *mut Bridge;
    pub fn anland_niri_close(bridge: *mut Bridge);
    pub fn anland_niri_pump(bridge: *mut Bridge, timeout_ms: c_int) -> c_int;
    pub fn anland_niri_reconnect(bridge: *mut Bridge) -> c_int;
    pub fn anland_niri_reopen(bridge: *mut Bridge, endpoint: *const c_char) -> c_int;
    pub fn anland_niri_connected(bridge: *mut Bridge) -> c_int;
    pub fn anland_niri_daemon_alive(bridge: *mut Bridge) -> c_int;
    pub fn anland_niri_generation(bridge: *mut Bridge) -> u64;
    pub fn anland_niri_drop_session(bridge: *mut Bridge);
    pub fn anland_niri_output_info(bridge: *mut Bridge, out: *mut Output) -> c_int;
    pub fn anland_niri_get_target(bridge: *mut Bridge, out: *mut Target) -> c_int;
    pub fn anland_niri_submit(
        bridge: *mut Bridge,
        generation: u64,
        index: u32,
        fence: c_int,
        commit_id: *mut u64,
    ) -> c_int;
    pub fn anland_niri_dispatch(
        bridge: *mut Bridge,
        events: *mut SceneEvent,
        capacity: usize,
        count: *mut usize,
    ) -> c_int;
    pub fn anland_niri_poll_input(bridge: *mut Bridge, out: *mut InputEvent) -> c_int;
    pub fn anland_niri_read_payload(
        bridge: *mut Bridge,
        buf: *mut c_void,
        size: usize,
        timeout_ms: c_int,
    ) -> c_int;
    pub fn anland_niri_read_fds(
        bridge: *mut Bridge,
        fds: *mut c_int,
        max: c_int,
        count: *mut c_int,
        timeout_ms: c_int,
    ) -> c_int;
    pub fn anland_niri_send_clipboard(
        bridge: *mut Bridge,
        buf: *const c_void,
        size: usize,
    ) -> c_int;
    pub fn anland_niri_request_resources(bridge: *mut Bridge, service: u32) -> c_int;
    pub fn anland_niri_audio_fd(bridge: *mut Bridge) -> c_int;
    #[cfg(have_anland_audio)]
    pub fn anland_audio_start() -> c_int;
    #[cfg(have_anland_audio)]
    pub fn anland_audio_stop();
    #[cfg(have_anland_audio)]
    pub fn anland_audio_set_fd(fd: c_int);
    #[cfg(have_anland_audio)]
    pub fn anland_camera_start() -> c_int;
    #[cfg(have_anland_audio)]
    pub fn anland_camera_stop();
    #[cfg(have_anland_audio)]
    pub fn anland_camera_set_resources(ctrl_fd: c_int, stream_fds: *const c_int, count: c_int);
    #[cfg(have_anland_audio)]
    pub fn anland_camera_clear();
}