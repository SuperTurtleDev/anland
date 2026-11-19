//! Committed UTF-8 for clients without an active Wayland text-input editor.
use std::io;
use std::os::fd::{AsFd, OwnedFd};
use std::time::{Duration, Instant};

use smithay::input::keyboard::{KeymapFile, SerializedMods, xkb};
use smithay::reexports::wayland_server::Resource;
use smithay::reexports::wayland_server::protocol::wl_keyboard::{
    KeyState, KeymapFormat, WlKeyboard,
};
use smithay::utils::SERIAL_COUNTER;

const FIRST_XKB_KEYCODE: u32 = 9;
const MAX_BATCH: usize = (256 - FIRST_XKB_KEYCODE) as usize;
const REAL_KEY_HOLD: Duration = Duration::from_millis(30);
const MAP_SETTLE: Duration = Duration::from_millis(75);
const KEY_HOLD: Duration = Duration::from_millis(100);

struct PreparedKeymap {
    fd: OwnedFd,
    size: u32,
}

impl PreparedKeymap {
    fn new(file: &KeymapFile) -> io::Result<Self> {
        let mut prepared = Err(io::Error::other("missing keymap file descriptor"));
        file.with_fd(false, |fd, size| {
            prepared = fd.try_clone_to_owned().map(|fd| Self {
                fd,
                size: size as u32,
            });
        })?;
        prepared
    }

    fn send(&self, keyboard: &WlKeyboard) {
        keyboard.keymap(KeymapFormat::XkbV1, self.fd.as_fd(), self.size);
    }
}

enum MapPhase {
    Pending,
    Settling,
    Holding,
    Restoring,
}

pub(crate) struct TextKeyboard {
    keyboards: Vec<WlKeyboard>,
    original: PreparedKeymap,
    original_mods: SerializedMods,
    batches: Vec<(PreparedKeymap, usize)>,
    batch: usize,
    deadline: Instant,
    phase: MapPhase,
}

impl TextKeyboard {
    pub(crate) fn start(
        keyboards: Vec<WlKeyboard>,
        original: &xkb::Keymap,
        mods: SerializedMods,
        text: &str,
        time_ms: u32,
    ) -> io::Result<Option<Self>> {
        let symbols = text
            .chars()
            .map(|ch| {
                let symbol = match ch {
                    '\n' | '\r' => xkb::keysyms::KEY_Return.into(),
                    '\t' => xkb::keysyms::KEY_Tab.into(),
                    '\u{8}' => xkb::keysyms::KEY_BackSpace.into(),
                    ch if !ch.is_control() => xkb::utf32_to_keysym(u32::from(ch)),
                    _ => xkb::keysyms::KEY_NoSymbol.into(),
                };
                if symbol.raw() == xkb::keysyms::KEY_NoSymbol {
                    Err(io::Error::new(
                        io::ErrorKind::InvalidData,
                        "unsupported text character",
                    ))
                } else {
                    Ok(symbol)
                }
            })
            .collect::<io::Result<Vec<_>>>()?;
        if keyboards.is_empty() || symbols.is_empty() {
            return Err(io::Error::new(
                io::ErrorKind::InvalidInput,
                "no target or text",
            ));
        }
        if mods.depressed == 0 && mods.latched == 0 && mods.locked == 0 {
            let min = original.min_keycode().raw().max(8);
            let max = original.max_keycode().raw().min(255);
            let codes = symbols
                .iter()
                .map(|symbol| {
                    (min..=max)
                        .find(|code| {
                            original.key_get_syms_by_level(
                                xkb::Keycode::new(*code),
                                mods.layout_effective,
                                0,
                            ) == [*symbol]
                        })
                        .map(|code| code - 8)
                })
                .collect::<Option<Vec<_>>>();
            if let Some(codes) = codes {
                // The normal map already represents the entire commit.
                send_keys(&keyboards, time_ms, codes);
                return Ok(None);
            }
        }
        // Prepare the original and every replacement first. An allocation/FD
        // failure must not leave a partial commit or an installed temporary map.
        let original = PreparedKeymap::new(&KeymapFile::new(original))?;
        let context = xkb::Context::new(xkb::CONTEXT_NO_FLAGS);
        let mut batches = Vec::new();
        for chunk in symbols.chunks(MAX_BATCH) {
            let source = keymap_source(chunk);
            let map = xkb::Keymap::new_from_string(
                &context,
                source,
                xkb::KEYMAP_FORMAT_TEXT_V1,
                xkb::KEYMAP_COMPILE_NO_FLAGS,
            )
            .ok_or_else(|| io::Error::other("cannot compile Unicode keymap"))?;
            batches.push((PreparedKeymap::new(&KeymapFile::new(&map))?, chunk.len()));
        }
        Ok(Some(Self {
            keyboards,
            original,
            original_mods: mods,
            batches,
            batch: 0,
            deadline: Instant::now() + REAL_KEY_HOLD,
            // Preceding real keys can still be queued for their original map.
            phase: MapPhase::Pending,
        }))
    }

    pub(crate) fn deadline(&self) -> Instant {
        self.deadline
    }

    fn install(&self) {
        let serial = SERIAL_COUNTER.next_serial().into();
        for keyboard in self.keyboards.iter().filter(|keyboard| keyboard.is_alive()) {
            self.batches[self.batch].0.send(keyboard);
            keyboard.modifiers(serial, 0, 0, 0, 0);
        }
    }

    /// Wayland queues preserve the order of events, but Xwayland clients look
    /// up keysyms against the current X11 map after reading their key events.
    /// Give them separate dispatch turns to see the map and consume the keys
    /// before replacing it. Return false when the caller should drop the lease.
    pub(crate) fn advance(&mut self, now: Instant, time_ms: u32) -> bool {
        if now < self.deadline {
            return true;
        }
        if matches!(self.phase, MapPhase::Restoring) {
            return false;
        }
        if matches!(self.phase, MapPhase::Pending) {
            self.install();
            self.phase = MapPhase::Settling;
            self.deadline = now + MAP_SETTLE;
            return true;
        }
        if matches!(self.phase, MapPhase::Settling) {
            let count = self.batches[self.batch].1;
            let first = FIRST_XKB_KEYCODE - 8;
            send_keys(&self.keyboards, time_ms, first..first + count as u32);
            self.phase = MapPhase::Holding;
            self.deadline = now + KEY_HOLD;
            return true;
        }
        if self.batch + 1 == self.batches.len() {
            return self.cancel(now);
        }
        self.batch += 1;
        self.install();
        self.phase = MapPhase::Settling;
        self.deadline = now + MAP_SETTLE;
        true
    }

    /// Drop undelivered text while leaving time for the restored map to apply.
    pub(crate) fn cancel(&mut self, now: Instant) -> bool {
        match self.phase {
            MapPhase::Pending => false,
            MapPhase::Restoring => now < self.deadline,
            MapPhase::Settling | MapPhase::Holding => {
                self.restore();
                self.phase = MapPhase::Restoring;
                self.deadline = now + MAP_SETTLE;
                true
            }
        }
    }

    fn restore(&self) {
        let serial = SERIAL_COUNTER.next_serial().into();
        let mods = self.original_mods;
        for keyboard in self.keyboards.iter().filter(|keyboard| keyboard.is_alive()) {
            self.original.send(keyboard);
            keyboard.modifiers(
                serial,
                mods.depressed,
                mods.latched,
                mods.locked,
                mods.layout_effective,
            );
        }
    }
}

fn send_keys(keyboards: &[WlKeyboard], time_ms: u32, codes: impl IntoIterator<Item = u32>) {
    for key in codes {
        let press = SERIAL_COUNTER.next_serial().into();
        let release = SERIAL_COUNTER.next_serial().into();
        for keyboard in keyboards.iter().filter(|keyboard| keyboard.is_alive()) {
            keyboard.key(press, time_ms, key, KeyState::Pressed);
            keyboard.key(release, time_ms, key, KeyState::Released);
        }
    }
}

impl Drop for TextKeyboard {
    fn drop(&mut self) {
        if matches!(self.phase, MapPhase::Settling | MapPhase::Holding) {
            self.restore();
        }
    }
}

fn keymap_source(symbols: &[xkb::Keysym]) -> String {
    use std::fmt::Write as _;
    let mut map = String::from("xkb_keymap { xkb_keycodes { minimum = 8; maximum = 255; ");
    for index in 0..symbols.len() {
        let _ = write!(
            map,
            "<K{index:03}> = {}; ",
            index as u32 + FIRST_XKB_KEYCODE
        );
    }
    map.push_str("}; xkb_types { include \"complete\" }; xkb_compatibility { include \"complete\" interpret Any+AnyOfOrNone(all) { action= NoAction(); }; }; xkb_symbols { ");
    for (index, symbol) in symbols.iter().enumerate() {
        let _ = write!(map, "key <K{index:03}> {{ [ 0x{:x} ] }}; ", symbol.raw());
    }
    map.push_str("}; };");
    map
}
