//! Denial's single Anland adapter. Transport and session state stay in the
//! common producer; this module owns native conversions and borrowed watchers.
#![deny(unsafe_op_in_unsafe_fn)]
pub(crate) mod capture;
pub(crate) mod capture_queue;
pub mod client;
pub(crate) mod display;
pub(crate) mod event_sources;
mod ffi;
pub(crate) mod graphics;
pub mod input;
pub(crate) mod lifetime;
pub(crate) mod native;
pub(crate) mod pointer_capture;
pub(crate) mod runtime;
pub(crate) mod scale_control;
pub(crate) mod scheduling;
pub(crate) mod seat_input;
pub(crate) mod services;
pub(crate) mod submission;
pub(crate) mod topology;
