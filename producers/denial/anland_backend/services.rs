//! Clipboard export policy; all encoding and queueing stay in the public producer.
use std::io;

/// Selection epochs, not history/UI revisions, drive export. Backpressure never
/// stores a stale async result: the caller re-reads the current native selection.
#[derive(Default)]
pub struct ClipboardSync {
    inbound_epoch: Option<u64>,
    sent_epoch: Option<u64>,
    last_sent: Option<String>,
}
impl ClipboardSync {
    pub fn reset_session(&mut self) {
        self.sent_epoch = None;
        self.last_sent = None;
        // Do not replay the disconnected consumer's own clipboard back to it.
    }
    pub fn imported(&mut self, epoch: u64) {
        self.inbound_epoch = Some(epoch);
    }
    pub fn is_echo(&self, epoch: u64, text: &str) -> bool {
        self.sent_epoch == Some(epoch) && self.last_sent.as_deref() == Some(text)
    }
    pub fn needs_export(&self, epoch: u64) -> bool {
        self.inbound_epoch != Some(epoch) && self.sent_epoch != Some(epoch)
    }
    pub fn export(
        &mut self,
        epoch: u64,
        text: &str,
        mut queue: impl FnMut(&[u8]) -> io::Result<()>,
    ) -> io::Result<()> {
        if !self.needs_export(epoch) {
            return Ok(());
        }
        match queue(text.as_bytes()) {
            Ok(()) => {
                self.sent_epoch = Some(epoch);
                self.last_sent = Some(text.to_owned());
                Ok(())
            }
            // No success marker: next iteration tries the latest epoch, not this
            // text. Public queue owns accepted bytes and partial writes.
            Err(error)
                if matches!(error.kind(), io::ErrorKind::WouldBlock)
                    || error.raw_os_error() == Some(105) =>
            {
                Ok(())
            }
            Err(error) => Err(error),
        }
    }
}
