//! Deduplicate pointer-capture requests for one public producer session.
use std::io;

/// Last successfully sent state for ONE live session. A successful public send
/// is not an acknowledgement that Android has granted pointer capture.
#[derive(Default)]
pub(crate) struct CaptureSync {
    sent: Option<bool>,
}
impl CaptureSync {
    pub fn reset_session(&mut self) {
        self.sent = None;
    }
    pub fn synchronize(
        &mut self,
        connected: bool,
        output_running: bool,
        session_locked: bool,
        requested: bool,
        mut send: impl FnMut(bool) -> io::Result<()>,
    ) -> io::Result<()> {
        if !connected {
            self.reset_session();
            return Ok(());
        }
        let desired = output_running && !session_locked && requested;
        if self.sent == Some(desired) {
            return Ok(());
        }
        // On failure do not record success or retry inside the frame path.
        // The caller retires the output; public transport owns partial failure.
        self.sent = None;
        send(desired)?;
        self.sent = Some(desired);
        Ok(())
    }
}
