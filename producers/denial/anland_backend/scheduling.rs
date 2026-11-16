//! Foreground process scheduling state for one consumer session.
use std::io;

const SETTREE: u8 = 0x01;
const ON: u8 = 0x02;

#[derive(Default)]
pub(crate) struct SchedulingSync {
    sent_pid: Option<i32>,
}

impl SchedulingSync {
    pub fn reset_session(&mut self) {
        self.sent_pid = None;
    }

    pub fn synchronize(
        &mut self,
        connected: bool,
        wanted_pid: Option<i32>,
        mut send: impl FnMut(i32, u8) -> io::Result<()>,
    ) -> io::Result<()> {
        if !connected {
            self.reset_session();
            return Ok(());
        }
        let wanted_pid = wanted_pid.filter(|pid| *pid > 0);
        if self.sent_pid == wanted_pid {
            return Ok(());
        }
        if let Some(previous_pid) = self.sent_pid {
            send(previous_pid, SETTREE)?;
            self.sent_pid = None;
        }
        if let Some(pid) = wanted_pid {
            send(pid, SETTREE | ON)?;
            self.sent_pid = Some(pid);
        }
        Ok(())
    }
}
