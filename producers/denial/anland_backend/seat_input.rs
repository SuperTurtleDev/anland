//! Smithay event objects only. No socket reads, wire decoding or new protocol.
use super::client::Input;
use super::ffi;
use smithay::backend::input::*;
use std::collections::{BTreeMap, BTreeSet, VecDeque};
use std::path::PathBuf;

#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub struct AnlandDevice {
    tablet_id: Option<u32>,
}
impl AnlandDevice {
    fn consumer() -> Self {
        Self { tablet_id: None }
    }
    fn tablet(device_id: u32) -> Self {
        Self {
            tablet_id: Some(device_id),
        }
    }
}
impl Device for AnlandDevice {
    fn id(&self) -> String {
        self.tablet_id.map_or_else(
            || "anland-consumer".into(),
            |id| format!("anland-tablet-{id}"),
        )
    }
    fn name(&self) -> String {
        self.tablet_id.map_or_else(
            || "Anland consumer input".into(),
            |id| format!("Anland graphics tablet {id}"),
        )
    }
    fn has_capability(&self, c: DeviceCapability) -> bool {
        match self.tablet_id {
            Some(_) => c == DeviceCapability::TabletTool,
            None => matches!(
                c,
                DeviceCapability::Keyboard | DeviceCapability::Pointer | DeviceCapability::Touch
            ),
        }
    }
    fn usb_id(&self) -> Option<(u32, u32)> {
        None
    }
    fn syspath(&self) -> Option<PathBuf> {
        None
    }
}

#[derive(Debug, Clone, Copy)]
pub struct NativeEvent {
    time: u64,
    extent: (u32, u32),
    raw: Input,
    key_count: u32,
}
impl Event<AnlandInputBackend> for NativeEvent {
    fn time(&self) -> InputTime {
        InputTime::from_micros(self.time)
    }
    fn device(&self) -> AnlandDevice {
        AnlandDevice::consumer()
    }
}
impl AbsolutePositionEvent<AnlandInputBackend> for NativeEvent {
    fn x(&self) -> f64 {
        self.raw.x.into()
    }
    fn y(&self) -> f64 {
        self.raw.y.into()
    }
    fn x_transformed(&self, width: i32) -> f64 {
        self.x() * f64::from(width) / f64::from(self.extent.0)
    }
    fn y_transformed(&self, height: i32) -> f64 {
        self.y() * f64::from(height) / f64::from(self.extent.1)
    }
}
impl KeyboardKeyEvent<AnlandInputBackend> for NativeEvent {
    // Public producer supplies evdev codes; Smithay's seat uses XKB (+8).
    fn key_code(&self) -> Keycode {
        Keycode::new(self.raw.code as u32 + 8)
    }
    fn state(&self) -> KeyState {
        if self.raw.action == 0 {
            KeyState::Pressed
        } else {
            KeyState::Released
        }
    }
    fn count(&self) -> u32 {
        self.key_count
    }
}
impl PointerMotionEvent<AnlandInputBackend> for NativeEvent {
    // Consumer deltas are physical pixels. The shared Flutter/Wayland route
    // divides by atlas_scale once; never apply a default scale/acceleration here.
    fn delta_x(&self) -> f64 {
        self.raw.dx.into()
    }
    fn delta_y(&self) -> f64 {
        self.raw.dy.into()
    }
    fn delta_x_unaccel(&self) -> f64 {
        self.delta_x()
    }
    fn delta_y_unaccel(&self) -> f64 {
        self.delta_y()
    }
}
impl PointerMotionAbsoluteEvent<AnlandInputBackend> for NativeEvent {}
impl PointerButtonEvent<AnlandInputBackend> for NativeEvent {
    fn button_code(&self) -> u32 {
        self.raw.code as u32
    }
    // Pointer pressed is boolean, NOT the touch/key DOWN=0 action enum.
    fn state(&self) -> ButtonState {
        if self.raw.action == 1 {
            ButtonState::Pressed
        } else {
            ButtonState::Released
        }
    }
}
impl PointerAxisEvent<AnlandInputBackend> for NativeEvent {
    fn amount(&self, axis: Axis) -> Option<f64> {
        self.matches_axis(axis).then_some(f64::from(self.raw.value))
    }
    fn amount_v120(&self, axis: Axis) -> Option<f64> {
        // Zero discrete means no wheel step metadata. Returning Some(0) would
        // discard smooth high-resolution scroll in Denial's existing route.
        (self.raw.discrete != 0 && self.matches_axis(axis))
            .then_some(f64::from(self.raw.discrete) * 120.)
    }
    fn source(&self) -> AxisSource {
        AxisSource::Wheel
    }
    fn relative_direction(&self, _: Axis) -> AxisRelativeDirection {
        AxisRelativeDirection::Identical
    }
}
impl NativeEvent {
    fn matches_axis(&self, axis: Axis) -> bool {
        matches!(
            (self.raw.code, axis),
            (0, Axis::Vertical) | (1, Axis::Horizontal)
        )
    }
}
impl TouchEvent<AnlandInputBackend> for NativeEvent {
    fn slot(&self) -> TouchSlot {
        TouchSlot::from(Some(self.raw.pointer_id as u32))
    }
}
impl TouchDownEvent<AnlandInputBackend> for NativeEvent {}
impl TouchMotionEvent<AnlandInputBackend> for NativeEvent {}
impl TouchUpEvent<AnlandInputBackend> for NativeEvent {}
impl TouchFrameEvent<AnlandInputBackend> for NativeEvent {}

#[derive(Debug, Clone, Copy)]
enum NativeTabletKind {
    Axis,
    Proximity(ProximityState),
    Tip(TabletToolTipState),
    Button { button: u32, state: ButtonState },
}

#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
struct TabletAxesChanged {
    pressure: bool,
    tilt_x: bool,
    tilt_y: bool,
    distance: bool,
}

#[derive(Debug, Clone, Copy)]
pub struct NativeTabletEvent {
    extent: (u32, u32),
    previous: ffi::TabletSample,
    current: ffi::TabletSample,
    kind: NativeTabletKind,
    axes: TabletAxesChanged,
    seat_button_count: u32,
}

impl Event<AnlandInputBackend> for NativeTabletEvent {
    fn time(&self) -> InputTime {
        InputTime::from_millis(self.current.time_msec)
    }
    fn device(&self) -> AnlandDevice {
        AnlandDevice::tablet(self.current.device_id)
    }
}
impl AbsolutePositionEvent<AnlandInputBackend> for NativeTabletEvent {
    fn x(&self) -> f64 {
        f64::from(self.current.x)
    }
    fn y(&self) -> f64 {
        f64::from(self.current.y)
    }
    fn x_transformed(&self, width: i32) -> f64 {
        if self.extent.0 == 0 {
            self.x()
        } else {
            self.x() * f64::from(width) / f64::from(self.extent.0)
        }
    }
    fn y_transformed(&self, height: i32) -> f64 {
        if self.extent.1 == 0 {
            self.y()
        } else {
            self.y() * f64::from(height) / f64::from(self.extent.1)
        }
    }
}
impl TabletToolEvent<AnlandInputBackend> for NativeTabletEvent {
    fn tool(&self) -> TabletToolDescriptor {
        let tool_type = match self.current.tool_type {
            ffi::TABLET_TOOL_PEN => TabletToolType::Pen,
            ffi::TABLET_TOOL_ERASER => TabletToolType::Eraser,
            _ => TabletToolType::Unknown,
        };
        let mut capabilities = TabletToolCapabilities::empty();
        if self.current.capabilities & ffi::TABLET_CAP_PRESSURE != 0 {
            capabilities |= TabletToolCapabilities::PRESSURE;
        }
        if self.current.capabilities & ffi::TABLET_CAP_TILT != 0 {
            capabilities |= TabletToolCapabilities::TILT;
        }
        if self.current.capabilities & ffi::TABLET_CAP_DISTANCE != 0 {
            capabilities |= TabletToolCapabilities::DISTANCE;
        }
        TabletToolDescriptor {
            tool_type,
            hardware_serial: u64::from(self.current.tool_id),
            hardware_id_wacom: 0,
            capabilities,
        }
    }
    fn delta_x(&self) -> f64 {
        f64::from(self.current.x - self.previous.x)
    }
    fn delta_y(&self) -> f64 {
        f64::from(self.current.y - self.previous.y)
    }
    fn distance(&self) -> f64 {
        f64::from(self.current.distance)
    }
    fn distance_has_changed(&self) -> bool {
        self.axes.distance
    }
    fn pressure(&self) -> f64 {
        f64::from(self.current.pressure)
    }
    fn pressure_has_changed(&self) -> bool {
        self.axes.pressure
    }
    fn slider_position(&self) -> f64 {
        0.
    }
    fn slider_has_changed(&self) -> bool {
        false
    }
    fn tilt_x(&self) -> f64 {
        f64::from(self.current.tilt_x)
    }
    fn tilt_x_has_changed(&self) -> bool {
        self.axes.tilt_x
    }
    fn tilt_y(&self) -> f64 {
        f64::from(self.current.tilt_y)
    }
    fn tilt_y_has_changed(&self) -> bool {
        self.axes.tilt_y
    }
    fn rotation(&self) -> f64 {
        0.
    }
    fn rotation_has_changed(&self) -> bool {
        false
    }
    fn wheel_delta(&self) -> f64 {
        0.
    }
    fn wheel_delta_discrete(&self) -> i32 {
        0
    }
    fn wheel_has_changed(&self) -> bool {
        false
    }
}
impl TabletToolAxisEvent<AnlandInputBackend> for NativeTabletEvent {}
impl TabletToolProximityEvent<AnlandInputBackend> for NativeTabletEvent {
    fn state(&self) -> ProximityState {
        match self.kind {
            NativeTabletKind::Proximity(state) => state,
            _ => unreachable!("non-proximity tablet event"),
        }
    }
}
impl TabletToolTipEvent<AnlandInputBackend> for NativeTabletEvent {
    fn tip_state(&self) -> TabletToolTipState {
        match self.kind {
            NativeTabletKind::Tip(state) => state,
            _ => unreachable!("non-tip tablet event"),
        }
    }
}
impl TabletToolButtonEvent<AnlandInputBackend> for NativeTabletEvent {
    fn button(&self) -> u32 {
        match self.kind {
            NativeTabletKind::Button { button, .. } => button,
            _ => unreachable!("non-button tablet event"),
        }
    }
    fn seat_button_count(&self) -> u32 {
        self.seat_button_count
    }
    fn button_state(&self) -> ButtonState {
        match self.kind {
            NativeTabletKind::Button { state, .. } => state,
            _ => unreachable!("non-button tablet event"),
        }
    }
}

#[derive(Debug, Default)]
pub struct AnlandInputBackend {
    keys: BTreeSet<i32>,
    buttons: BTreeSet<i32>,
    contacts: BTreeSet<i32>,
    tablet_devices: BTreeSet<u32>,
    tablet_tools: BTreeMap<u32, ffi::TabletSample>,
    pending: VecDeque<InputEvent<Self>>,
    extent: (u32, u32),
}
impl InputBackend for AnlandInputBackend {
    type Device = AnlandDevice;
    type KeyboardKeyEvent = NativeEvent;
    type PointerAxisEvent = NativeEvent;
    type PointerButtonEvent = NativeEvent;
    type PointerMotionEvent = NativeEvent;
    type PointerMotionAbsoluteEvent = NativeEvent;
    type GestureSwipeBeginEvent = UnusedEvent;
    type GestureSwipeUpdateEvent = UnusedEvent;
    type GestureSwipeEndEvent = UnusedEvent;
    type GesturePinchBeginEvent = UnusedEvent;
    type GesturePinchUpdateEvent = UnusedEvent;
    type GesturePinchEndEvent = UnusedEvent;
    type GestureHoldBeginEvent = UnusedEvent;
    type GestureHoldEndEvent = UnusedEvent;
    type TouchDownEvent = NativeEvent;
    type TouchUpEvent = NativeEvent;
    type TouchMotionEvent = NativeEvent;
    type TouchCancelEvent = UnusedEvent;
    type TouchFrameEvent = NativeEvent;
    type TabletToolAxisEvent = NativeTabletEvent;
    type TabletToolProximityEvent = NativeTabletEvent;
    type TabletToolTipEvent = NativeTabletEvent;
    type TabletToolButtonEvent = NativeTabletEvent;
    type SwitchToggleEvent = UnusedEvent;
    type SpecialEvent = ();
}
impl AnlandInputBackend {
    /// The native seat performs actual release/cancel before this local reset.
    pub fn reset(&mut self) {
        self.keys.clear();
        self.buttons.clear();
        self.contacts.clear();
        self.tablet_devices.clear();
        self.tablet_tools.clear();
        self.pending.clear();
    }

    pub fn pop_pending(&mut self) -> Option<InputEvent<Self>> {
        self.pending.pop_front()
    }

    /// Build native tablet-v2 release events before removing its devices.
    pub fn take_tablet_releases(&mut self) -> Vec<InputEvent<Self>> {
        let tools = self.tablet_tools.values().copied().collect::<Vec<_>>();
        let time_msec = tools.iter().map(|tool| tool.time_msec).max().unwrap_or(0);
        for previous in tools {
            if previous.flags & ffi::TABLET_FLAG_PROXIMITY == 0 {
                continue;
            }
            let mut current = previous;
            current.flags = ffi::TABLET_FLAG_CANCELED;
            current.buttons = 0;
            current.pressure = 0.;
            current.time_msec = time_msec;
            self.enqueue_tablet_frame(previous, current);
        }
        let devices = std::mem::take(&mut self.tablet_devices);
        self.tablet_tools.clear();
        for device_id in devices {
            self.pending.push_back(InputEvent::DeviceRemoved {
                device: AnlandDevice::tablet(device_id),
            });
        }
        self.pending.drain(..).collect()
    }

    fn enqueue_tablet_frame(
        &mut self,
        _assembler_previous: ffi::TabletSample,
        current: ffi::TabletSample,
    ) {
        let previous = self
            .tablet_tools
            .get(&current.tool_id)
            .copied()
            .unwrap_or(ffi::TabletSample {
                tool_id: current.tool_id,
                tool_type: current.tool_type,
                capabilities: current.capabilities,
                device_id: current.device_id,
                ..ffi::TabletSample::default()
            });
        let device_id = current.device_id;
        if self.tablet_devices.insert(device_id) {
            self.pending.push_back(InputEvent::DeviceAdded {
                device: AnlandDevice::tablet(device_id),
            });
        }
        let was_in_proximity = previous.flags & ffi::TABLET_FLAG_PROXIMITY != 0;
        let is_in_proximity = current.flags & ffi::TABLET_FLAG_PROXIMITY != 0;
        let was_tip_down = previous.flags & ffi::TABLET_FLAG_TIP_DOWN != 0;
        let is_tip_down = current.flags & ffi::TABLET_FLAG_TIP_DOWN != 0;
        let axes_changed = Self::tablet_axes(previous, current, false);
        let proximity_in = is_in_proximity && !was_in_proximity;

        if proximity_in {
            self.push_tablet_event(
                previous,
                current,
                NativeTabletKind::Proximity(ProximityState::In),
                Self::tablet_axes(previous, current, true),
                0,
            );
        } else if is_in_proximity
            && axes_changed != TabletAxesChanged::default()
            && was_tip_down == is_tip_down
        {
            self.push_tablet_event(previous, current, NativeTabletKind::Axis, axes_changed, 0);
        }

        if was_tip_down != is_tip_down {
            self.push_tablet_event(
                previous,
                current,
                NativeTabletKind::Tip(if is_tip_down {
                    TabletToolTipState::Down
                } else {
                    TabletToolTipState::Up
                }),
                if proximity_in {
                    TabletAxesChanged::default()
                } else {
                    axes_changed
                },
                0,
            );
        }

        let previous_buttons = previous.buttons;
        let current_buttons = if is_in_proximity { current.buttons } else { 0 };
        let other_button_count = self
            .tablet_tools
            .iter()
            .filter(|(tool_id, _)| **tool_id != current.tool_id)
            .map(|(_, sample)| sample.buttons.count_ones())
            .sum::<u32>();
        let mut buttons_after = previous_buttons;
        for (bit, button) in [
            (ffi::TABLET_BUTTON_PRIMARY, 0x14b),
            (ffi::TABLET_BUTTON_SECONDARY, 0x14c),
        ] {
            let was_pressed = previous_buttons & bit != 0;
            let is_pressed = current_buttons & bit != 0;
            if was_pressed != is_pressed {
                if is_pressed {
                    buttons_after |= bit;
                } else {
                    buttons_after &= !bit;
                }
                self.push_tablet_event(
                    previous,
                    current,
                    NativeTabletKind::Button {
                        button,
                        state: if is_pressed {
                            ButtonState::Pressed
                        } else {
                            ButtonState::Released
                        },
                    },
                    TabletAxesChanged::default(),
                    other_button_count + buttons_after.count_ones(),
                );
            }
        }

        if was_in_proximity && !is_in_proximity {
            self.push_tablet_event(
                previous,
                current,
                NativeTabletKind::Proximity(ProximityState::Out),
                TabletAxesChanged::default(),
                0,
            );
        }
        self.tablet_tools.insert(current.tool_id, current);
    }

    fn tablet_axes(
        previous: ffi::TabletSample,
        current: ffi::TabletSample,
        force: bool,
    ) -> TabletAxesChanged {
        TabletAxesChanged {
            pressure: current.capabilities & ffi::TABLET_CAP_PRESSURE != 0
                && (force || previous.pressure != current.pressure),
            tilt_x: current.capabilities & ffi::TABLET_CAP_TILT != 0
                && (force || previous.tilt_x != current.tilt_x),
            tilt_y: current.capabilities & ffi::TABLET_CAP_TILT != 0
                && (force || previous.tilt_y != current.tilt_y),
            distance: current.capabilities & ffi::TABLET_CAP_DISTANCE != 0
                && (force || previous.distance != current.distance),
        }
    }

    fn push_tablet_event(
        &mut self,
        previous: ffi::TabletSample,
        current: ffi::TabletSample,
        kind: NativeTabletKind,
        axes: TabletAxesChanged,
        seat_button_count: u32,
    ) {
        let event = NativeTabletEvent {
            extent: self.extent,
            previous,
            current,
            kind,
            axes,
            seat_button_count,
        };
        self.pending.push_back(match kind {
            NativeTabletKind::Axis => InputEvent::TabletToolAxis { event },
            NativeTabletKind::Proximity(_) => InputEvent::TabletToolProximity { event },
            NativeTabletKind::Tip(_) => InputEvent::TabletToolTip { event },
            NativeTabletKind::Button { .. } => InputEvent::TabletToolButton { event },
        });
    }
    /// Translation rejects invalid/orphan transitions rather than inventing an
    /// UP for an unknown key or turning a negative contact id into slot zero.
    pub fn translate(
        &mut self,
        raw: Input,
        extent: (u32, u32),
        time: u64,
    ) -> Option<InputEvent<Self>> {
        if extent.0 == 0 || extent.1 == 0 {
            return None;
        }
        self.extent = extent;
        if raw.kind == ffi::INPUT_TYPE_TABLET_FRAME {
            self.enqueue_tablet_frame(raw.tablet_previous, raw.tablet_current);
            return self.pop_pending();
        }
        match raw.kind {
            2 => {
                if !(1..=0x2ff).contains(&raw.code) || !matches!(raw.action, 0 | 1) {
                    return None;
                }
                let changed = if raw.action == 0 {
                    self.keys.insert(raw.code)
                } else {
                    self.keys.remove(&raw.code)
                };
                if !changed {
                    return None;
                } // Flutter/seat own key repeat.
            }
            3 => {
                if !raw.dx.is_finite() || !raw.dy.is_finite() {
                    return None;
                }
            }
            4 => {
                if !(0x110..=0x11f).contains(&raw.code) || !matches!(raw.action, 0 | 1) {
                    return None;
                }
                let changed = if raw.action == 1 {
                    self.buttons.insert(raw.code)
                } else {
                    self.buttons.remove(&raw.code)
                };
                if !changed {
                    return None;
                }
            }
            5 => {
                if !matches!(raw.code, 0 | 1) || !raw.value.is_finite() {
                    return None;
                }
            }
            1 => {
                if !(0..=65535).contains(&raw.pointer_id) {
                    return None;
                }
                if matches!(raw.action, 0 | 2) && (!raw.x.is_finite() || !raw.y.is_finite()) {
                    return None;
                }
                let changed = match raw.action {
                    0 => self.contacts.len() < 32 && self.contacts.insert(raw.pointer_id),
                    1 => self.contacts.remove(&raw.pointer_id),
                    2 => self.contacts.contains(&raw.pointer_id),
                    _ => false,
                };
                if !changed {
                    return None;
                }
            }
            6 => {}
            _ => return None,
        }
        let event = NativeEvent {
            time,
            extent,
            raw,
            key_count: self.keys.len() as u32,
        };
        Some(match raw.kind {
            2 => InputEvent::Keyboard { event },
            // Relative deltas preserve the original pointer constraints/locks.
            3 => InputEvent::PointerMotion { event },
            4 => InputEvent::PointerButton { event },
            5 => InputEvent::PointerAxis { event },
            1 => match raw.action {
                0 => InputEvent::TouchDown { event },
                1 => InputEvent::TouchUp { event },
                _ => InputEvent::TouchMotion { event },
            },
            _ => InputEvent::TouchFrame { event },
        })
    }
}

#[cfg(test)]
mod tablet_tests {
    use super::*;
    use smithay::backend::input::{
        AbsolutePositionEvent, Device, DeviceCapability, Event, InputEvent, TabletToolButtonEvent,
        TabletToolEvent, TabletToolProximityEvent, TabletToolTipEvent,
    };

    fn active_pen() -> ffi::TabletSample {
        ffi::TabletSample {
            tool_id: 29,
            tool_type: ffi::TABLET_TOOL_PEN,
            capabilities: ffi::TABLET_CAP_PRESSURE
                | ffi::TABLET_CAP_TILT
                | ffi::TABLET_CAP_DISTANCE,
            device_id: 7,
            flags: ffi::TABLET_FLAG_PROXIMITY | ffi::TABLET_FLAG_TIP_DOWN,
            buttons: ffi::TABLET_BUTTON_PRIMARY | ffi::TABLET_BUTTON_SECONDARY,
            time_msec: 2345,
            x: 500.,
            y: 250.,
            pressure: 0.6,
            tilt_x: -30.,
            tilt_y: 45.,
            distance: 0.25,
        }
    }

    #[test]
    fn tablet_frame_registers_native_tool_axes_and_button_transitions() {
        let mut backend = AnlandInputBackend::default();
        backend.extent = (1000, 500);
        let sample = active_pen();
        backend.enqueue_tablet_frame(ffi::TabletSample::default(), sample);
        let events = std::iter::from_fn(|| backend.pop_pending()).collect::<Vec<_>>();

        assert!(
            matches!(events.first(), Some(InputEvent::DeviceAdded { device })
            if device.id() == "anland-tablet-7" && device.has_capability(DeviceCapability::TabletTool))
        );
        assert!(
            matches!(events.get(1), Some(InputEvent::TabletToolProximity { event })
            if event.state() == ProximityState::In
                && event.tool().tool_type == TabletToolType::Pen
                && event.tool().hardware_serial == 29
                && event.time().micros() == 2_345_000
                && event.pressure_has_changed()
                && (event.pressure() - 0.6).abs() < 1e-6
                && event.tilt_x() == -30.0
                && event.tilt_y() == 45.0
                && (event.distance() - 0.25).abs() < 1e-6
                && event.x_transformed(2000) == 1000.0)
        );
        assert!(
            matches!(events.get(2), Some(InputEvent::TabletToolTip { event })
            if event.tip_state() == TabletToolTipState::Down)
        );
        let buttons = events
            .iter()
            .filter_map(|event| match event {
                InputEvent::TabletToolButton { event } => {
                    Some((event.button(), event.button_state()))
                }
                _ => None,
            })
            .collect::<Vec<_>>();
        assert_eq!(
            buttons,
            vec![(0x14b, ButtonState::Pressed), (0x14c, ButtonState::Pressed),]
        );
        assert_eq!(events.len(), 5);
    }

    #[test]
    fn first_queued_tool_ignores_assembler_previous_active_state() {
        let mut backend = AnlandInputBackend::default();
        backend.extent = (1000, 500);
        let sample = active_pen();
        // A bootstrap refresh can update the C assembler without forwarding
        // its frame, so Rust has not registered this active tool yet.
        backend.enqueue_tablet_frame(sample, sample);
        let events = std::iter::from_fn(|| backend.pop_pending()).collect::<Vec<_>>();

        assert!(matches!(events.first(), Some(InputEvent::DeviceAdded { .. })));
        assert!(matches!(events.get(1), Some(InputEvent::TabletToolProximity { event })
            if event.state() == ProximityState::In));
        assert!(matches!(events.get(2), Some(InputEvent::TabletToolTip { event })
            if event.tip_state() == TabletToolTipState::Down));
        assert_eq!(events.len(), 5);
    }

    #[test]
    fn eraser_identity_and_tool_serial_survive_native_event_conversion() {
        let mut backend = AnlandInputBackend::default();
        backend.extent = (1000, 500);
        let mut sample = active_pen();
        sample.tool_id = 31;
        sample.tool_type = ffi::TABLET_TOOL_ERASER;
        sample.flags = ffi::TABLET_FLAG_PROXIMITY;
        sample.buttons = 0;
        backend.enqueue_tablet_frame(ffi::TabletSample::default(), sample);
        let events = std::iter::from_fn(|| backend.pop_pending()).collect::<Vec<_>>();
        assert!(
            matches!(events.get(1), Some(InputEvent::TabletToolProximity { event })
            if event.tool().tool_type == TabletToolType::Eraser
                && event.tool().hardware_serial == 31)
        );
    }

    #[test]
    fn disconnect_after_device_added_preserves_queued_frame_before_releases() {
        let mut backend = AnlandInputBackend::default();
        backend.extent = (1000, 500);
        let sample = active_pen();
        backend.enqueue_tablet_frame(ffi::TabletSample::default(), sample);
        assert!(
            matches!(backend.pop_pending(), Some(InputEvent::DeviceAdded { device })
            if device.id() == "anland-tablet-7")
        );

        let events = backend.take_tablet_releases();
        assert!(
            matches!(events.first(), Some(InputEvent::TabletToolProximity { event })
            if event.state() == ProximityState::In)
        );
        assert!(
            matches!(events.get(1), Some(InputEvent::TabletToolTip { event })
            if event.tip_state() == TabletToolTipState::Down)
        );
        assert!(
            matches!(events.get(4), Some(InputEvent::TabletToolTip { event })
            if event.tip_state() == TabletToolTipState::Up)
        );
        let buttons = events
            .iter()
            .filter_map(|event| match event {
                InputEvent::TabletToolButton { event } => {
                    Some((event.button(), event.button_state()))
                }
                _ => None,
            })
            .collect::<Vec<_>>();
        assert_eq!(
            buttons,
            vec![
                (0x14b, ButtonState::Pressed),
                (0x14c, ButtonState::Pressed),
                (0x14b, ButtonState::Released),
                (0x14c, ButtonState::Released),
            ]
        );
        assert!(
            matches!(events.get(7), Some(InputEvent::TabletToolProximity { event })
            if event.state() == ProximityState::Out)
        );
        assert!(
            matches!(events.last(), Some(InputEvent::DeviceRemoved { device })
            if device.id() == "anland-tablet-7")
        );
        assert_eq!(events.len(), 9);
    }
}
