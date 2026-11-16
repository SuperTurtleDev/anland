//! Continuation of public input messages; the common library owns decoding and
//! partial socket reads. Call at fd readiness AND on a bounded maintenance timer.
use super::client::{Client, Input};
use std::io;
use std::os::fd::OwnedFd;
use std::time::{Duration, Instant};

const MAX_PAYLOAD: usize = 1024 * 1024;
const PAYLOAD_DEADLINE: Duration = Duration::from_secs(5);
#[derive(Debug)]
pub enum Message {
    Input(Input),
    Refresh(u32),
    Clipboard(String),
    Text(String),
    Resources {
        kind: u32,
        invalid: bool,
        fds: Vec<OwnedFd>,
    },
}
pub trait InputTransport {
    /// Already consumed header bytes, not merely an idle/readable socket.
    fn header_pending(&self) -> bool;
    fn poll(&mut self) -> io::Result<Option<Input>>;
    fn payload(&mut self, bytes: &mut [u8]) -> io::Result<bool>;
    fn descriptors(&mut self) -> io::Result<Option<Vec<OwnedFd>>>;
    fn disconnect(&mut self);
}
impl InputTransport for Client {
    fn header_pending(&self) -> bool {
        self.input_header_pending()
    }
    fn poll(&mut self) -> io::Result<Option<Input>> {
        self.poll_input()
    }
    fn payload(&mut self, bytes: &mut [u8]) -> io::Result<bool> {
        self.read_payload(bytes)
    }
    fn descriptors(&mut self) -> io::Result<Option<Vec<OwnedFd>>> {
        self.read_fds()
    }
    fn disconnect(&mut self) {
        self.drop_session();
    }
}
enum Pending {
    Payload(Input, Vec<u8>),
    Descriptors(Input),
}
#[derive(Default)]
pub struct InputStream {
    pending: Option<Pending>,
    deadline: Option<Instant>,
}
impl InputStream {
    /// Preserve the caller's target through both the public header read and
    /// the announced payload. A genuinely idle poll must not pin an old target.
    pub fn has_pending(&self, source: &impl InputTransport) -> bool {
        self.pending.is_some() || source.header_pending()
    }

    /// Mandatory before reconnect/reopen: old payload belongs to the old session.
    pub fn reset(&mut self) {
        self.pending = None;
        self.deadline = None;
    }
    pub fn next(
        &mut self,
        source: &mut impl InputTransport,
        now: Instant,
    ) -> io::Result<Option<Message>> {
        let result = self.step(source, now);
        if result.is_err() {
            self.reset();
            source.disconnect();
        }
        result
    }
    /// Before any output/focus exists, learn the consumer's real refresh through
    /// the same framed reader used by the running desktop. Unrouted user input
    /// is discarded, including complete text/clipboard payloads; OwnedFd replies
    /// close normally. Never replay these messages into a later focus/session.
    /// Stop at refresh so subsequent live-session events stay in wire order.
    pub fn bootstrap_refresh(
        &mut self,
        source: &mut impl InputTransport,
        now: Instant,
    ) -> io::Result<Option<u32>> {
        for _ in 0..64 {
            match self.next(source, now)? {
                Some(Message::Refresh(refresh)) => return Ok(Some(refresh)),
                Some(_) => {}
                None => break,
            }
        }
        Ok(None)
    }
    fn step(
        &mut self,
        source: &mut impl InputTransport,
        now: Instant,
    ) -> io::Result<Option<Message>> {
        if self.deadline.is_some_and(|deadline| now >= deadline) {
            return Err(io::Error::new(
                io::ErrorKind::TimedOut,
                "Anland input payload timed out",
            ));
        }
        if self.pending.is_none() {
            let Some(event) = source.poll()? else {
                return Ok(None);
            };
            if event.kind == 7 {
                if event.size == 0 || event.size > i32::MAX as u32 {
                    return Err(io::Error::new(
                        io::ErrorKind::InvalidData,
                        "invalid consumer refresh",
                    ));
                }
                return Ok(Some(Message::Refresh(event.size)));
            }
            self.pending = match event.kind {
                8 | 9 => {
                    if event.size as usize > MAX_PAYLOAD {
                        return Err(io::Error::other("oversized input payload"));
                    }
                    Some(Pending::Payload(event, vec![0; event.size as usize]))
                }
                11 | 12 if event.fd_count == 0 => {
                    return Ok(Some(Message::Resources {
                        kind: event.resource_type,
                        invalid: true,
                        fds: Vec::new(),
                    }));
                }
                11 | 12 => {
                    if event.fd_count > 64 {
                        return Err(io::Error::other("too many input descriptors"));
                    }
                    Some(Pending::Descriptors(event))
                }
                _ => return Ok(Some(Message::Input(event))),
            };
            self.deadline = Some(now + PAYLOAD_DEADLINE);
        }
        let message = match self
            .pending
            .as_mut()
            .expect("pending payload was just installed")
        {
            Pending::Payload(event, bytes) => {
                if !source.payload(bytes)? {
                    return Ok(None);
                }
                let text = std::str::from_utf8(bytes).map_err(|_| {
                    io::Error::new(io::ErrorKind::InvalidData, "invalid UTF-8 input")
                })?;
                if text.contains('\0') {
                    return Err(io::Error::new(io::ErrorKind::InvalidData, "NUL in text"));
                }
                if event.kind == 9 {
                    Message::Text(text.to_owned())
                } else {
                    Message::Clipboard(text.to_owned())
                }
            }
            Pending::Descriptors(event) => {
                let Some(fds) = source.descriptors()? else {
                    return Ok(None);
                };
                if fds.len() != event.fd_count as usize {
                    return Err(io::Error::new(
                        io::ErrorKind::InvalidData,
                        "resource descriptor count mismatch",
                    ));
                }
                Message::Resources {
                    kind: event.resource_type,
                    invalid: event.kind == 12 || event.fd_count == 0,
                    fds,
                }
            }
        };
        self.reset();
        Ok(Some(message))
    }
}
