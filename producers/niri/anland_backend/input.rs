//! Anland input backend: feeds smithay input events from the anland consumer.

use std::collections::{BTreeMap, BTreeSet, VecDeque};
use std::path::PathBuf;

use smithay::backend::input::{
    AbsolutePositionEvent, Axis, AxisRelativeDirection, AxisSource, ButtonState, Device,
    DeviceCapability, Event, InputBackend, KeyState, KeyboardKeyEvent, Keycode, PointerAxisEvent,
    PointerButtonEvent, PointerMotionAbsoluteEvent, PointerMotionEvent, ProximityState,
    TabletToolAxisEvent, TabletToolButtonEvent, TabletToolCapabilities, TabletToolDescriptor,
    TabletToolEvent, TabletToolProximityEvent, TabletToolTipEvent, TabletToolTipState,
    TabletToolType, TouchDownEvent, TouchEvent, TouchFrameEvent, TouchMotionEvent, TouchSlot,
    TouchUpEvent,
};
use smithay::output::Output;

use super::ffi;
use crate::input::backend_ext::NiriInputDevice;
use crate::niri::State;

/// The consumer's general input device or one of its graphics-tablet devices.
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
        self.tablet_id
            .map_or_else(|| "anland".to_string(), |id| format!("anland-tablet-{id}"))
    }

    fn name(&self) -> String {
        self.tablet_id.map_or_else(
            || "anland consumer input".to_string(),
            |id| format!("Anland graphics tablet {id}"),
        )
    }

    fn has_capability(&self, capability: DeviceCapability) -> bool {
        match self.tablet_id {
            Some(_) => capability == DeviceCapability::TabletTool,
            None => matches!(
                capability,
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

impl NiriInputDevice for AnlandDevice {
    fn output(&self, _state: &State) -> Option<Output> {
        // Single-output backend. Leave the cursor in *global* output coordinates,
        // same as the winit backend.
        None
    }
}

/// The consumer reports coordinates in its native (physical) pixel space, which may
/// differ from the buffer size (e.g. after rotation). We scale absolute positions
/// into the target coordinate space from the consumer's native extent.
#[derive(Debug, Clone, Copy)]
pub struct AnlandEventBase {
    pub time_usec: u64,
    pub native_w: u32,
    pub native_h: u32,
    pub x: f64,
    pub y: f64,
}

impl AnlandEventBase {
    fn x_transformed(&self, width: i32) -> f64 {
        if self.native_w == 0 {
            return self.x;
        }
        self.x * (width as f64) / (self.native_w as f64)
    }

    fn y_transformed(&self, height: i32) -> f64 {
        if self.native_h == 0 {
            return self.y;
        }
        self.y * (height as f64) / (self.native_h as f64)
    }
}

impl Event<AnlandInputBackend> for AnlandEventBase {
    fn time(&self) -> u64 {
        self.time_usec
    }

    fn device(&self) -> AnlandDevice {
        AnlandDevice::consumer()
    }
}

impl AbsolutePositionEvent<AnlandInputBackend> for AnlandEventBase {
    fn x(&self) -> f64 {
        self.x
    }

    fn y(&self) -> f64 {
        self.y
    }

    fn x_transformed(&self, width: i32) -> f64 {
        AnlandEventBase::x_transformed(self, width)
    }

    fn y_transformed(&self, height: i32) -> f64 {
        AnlandEventBase::y_transformed(self, height)
    }
}

#[derive(Debug, Clone, Copy)]
pub struct AnlandKeyboardKeyEvent {
    pub base: AnlandEventBase,
    pub keycode: u32,
    pub state: KeyState,
}

impl Event<AnlandInputBackend> for AnlandKeyboardKeyEvent {
    fn time(&self) -> u64 {
        self.base.time()
    }

    fn device(&self) -> AnlandDevice {
        AnlandDevice::consumer()
    }
}

impl KeyboardKeyEvent<AnlandInputBackend> for AnlandKeyboardKeyEvent {
    fn key_code(&self) -> Keycode {
        // smithay/libinput convert evdev keycodes into xkb keycodes by adding 8.
        Keycode::from(self.keycode + 8)
    }

    fn state(&self) -> KeyState {
        self.state
    }

    fn count(&self) -> u32 {
        1
    }
}

#[derive(Debug, Clone, Copy)]
pub struct AnlandPointerMotionEvent {
    pub base: AnlandEventBase,
    pub dx: f64,
    pub dy: f64,
}

impl Event<AnlandInputBackend> for AnlandPointerMotionEvent {
    fn time(&self) -> u64 {
        self.base.time()
    }

    fn device(&self) -> AnlandDevice {
        AnlandDevice::consumer()
    }
}

impl PointerMotionEvent<AnlandInputBackend> for AnlandPointerMotionEvent {
    fn delta_x(&self) -> f64 {
        self.dx
    }

    fn delta_y(&self) -> f64 {
        self.dy
    }

    fn delta_x_unaccel(&self) -> f64 {
        self.dx
    }

    fn delta_y_unaccel(&self) -> f64 {
        self.dy
    }
}

#[derive(Debug, Clone, Copy)]
pub struct AnlandPointerMotionAbsoluteEvent(pub AnlandEventBase);

impl Event<AnlandInputBackend> for AnlandPointerMotionAbsoluteEvent {
    fn time(&self) -> u64 {
        self.0.time()
    }

    fn device(&self) -> AnlandDevice {
        AnlandDevice::consumer()
    }
}

impl AbsolutePositionEvent<AnlandInputBackend> for AnlandPointerMotionAbsoluteEvent {
    fn x(&self) -> f64 {
        self.0.x()
    }

    fn y(&self) -> f64 {
        self.0.y()
    }

    fn x_transformed(&self, width: i32) -> f64 {
        self.0.x_transformed(width)
    }

    fn y_transformed(&self, height: i32) -> f64 {
        self.0.y_transformed(height)
    }
}

impl PointerMotionAbsoluteEvent<AnlandInputBackend> for AnlandPointerMotionAbsoluteEvent {}

#[derive(Debug, Clone, Copy)]
pub struct AnlandPointerButtonEvent {
    pub base: AnlandEventBase,
    pub button: u32,
    pub state: ButtonState,
}

impl Event<AnlandInputBackend> for AnlandPointerButtonEvent {
    fn time(&self) -> u64 {
        self.base.time()
    }

    fn device(&self) -> AnlandDevice {
        AnlandDevice::consumer()
    }
}

impl PointerButtonEvent<AnlandInputBackend> for AnlandPointerButtonEvent {
    fn button_code(&self) -> u32 {
        self.button
    }

    fn state(&self) -> ButtonState {
        self.state
    }
}

#[derive(Debug, Clone, Copy)]
pub struct AnlandPointerAxisEvent {
    pub base: AnlandEventBase,
    pub axis: u32,
    pub value: f64,
    pub discrete: i32,
}

impl Event<AnlandInputBackend> for AnlandPointerAxisEvent {
    fn time(&self) -> u64 {
        self.base.time()
    }

    fn device(&self) -> AnlandDevice {
        AnlandDevice::consumer()
    }
}

impl PointerAxisEvent<AnlandInputBackend> for AnlandPointerAxisEvent {
    fn amount(&self, axis: Axis) -> Option<f64> {
        match (self.axis, axis) {
            (0, Axis::Vertical) | (1, Axis::Horizontal) => Some(self.value),
            _ => None,
        }
    }

    fn amount_v120(&self, axis: Axis) -> Option<f64> {
        match (self.axis, axis) {
            (0, Axis::Vertical) | (1, Axis::Horizontal) => Some(f64::from(self.discrete) * 120.),
            _ => None,
        }
    }

    fn source(&self) -> AxisSource {
        AxisSource::Wheel
    }

    fn relative_direction(&self, _axis: Axis) -> AxisRelativeDirection {
        AxisRelativeDirection::Identical
    }
}

#[derive(Debug, Clone, Copy)]
pub struct AnlandTouchDownEvent {
    pub base: AnlandEventBase,
    pub slot: TouchSlot,
}

impl Event<AnlandInputBackend> for AnlandTouchDownEvent {
    fn time(&self) -> u64 {
        self.base.time()
    }

    fn device(&self) -> AnlandDevice {
        AnlandDevice::consumer()
    }
}

impl TouchEvent<AnlandInputBackend> for AnlandTouchDownEvent {
    fn slot(&self) -> TouchSlot {
        self.slot
    }
}

impl AbsolutePositionEvent<AnlandInputBackend> for AnlandTouchDownEvent {
    fn x(&self) -> f64 {
        self.base.x()
    }

    fn y(&self) -> f64 {
        self.base.y()
    }

    fn x_transformed(&self, width: i32) -> f64 {
        self.base.x_transformed(width)
    }

    fn y_transformed(&self, height: i32) -> f64 {
        self.base.y_transformed(height)
    }
}

impl TouchDownEvent<AnlandInputBackend> for AnlandTouchDownEvent {}

#[derive(Debug, Clone, Copy)]
pub struct AnlandTouchMotionEvent {
    pub base: AnlandEventBase,
    pub slot: TouchSlot,
}

impl Event<AnlandInputBackend> for AnlandTouchMotionEvent {
    fn time(&self) -> u64 {
        self.base.time()
    }

    fn device(&self) -> AnlandDevice {
        AnlandDevice::consumer()
    }
}

impl TouchEvent<AnlandInputBackend> for AnlandTouchMotionEvent {
    fn slot(&self) -> TouchSlot {
        self.slot
    }
}

impl AbsolutePositionEvent<AnlandInputBackend> for AnlandTouchMotionEvent {
    fn x(&self) -> f64 {
        self.base.x()
    }

    fn y(&self) -> f64 {
        self.base.y()
    }

    fn x_transformed(&self, width: i32) -> f64 {
        self.base.x_transformed(width)
    }

    fn y_transformed(&self, height: i32) -> f64 {
        self.base.y_transformed(height)
    }
}

impl TouchMotionEvent<AnlandInputBackend> for AnlandTouchMotionEvent {}

#[derive(Debug, Clone, Copy)]
pub struct AnlandTouchUpEvent {
    pub base: AnlandEventBase,
    pub slot: TouchSlot,
}

impl Event<AnlandInputBackend> for AnlandTouchUpEvent {
    fn time(&self) -> u64 {
        self.base.time()
    }

    fn device(&self) -> AnlandDevice {
        AnlandDevice::consumer()
    }
}

impl TouchEvent<AnlandInputBackend> for AnlandTouchUpEvent {
    fn slot(&self) -> TouchSlot {
        self.slot
    }
}

impl TouchUpEvent<AnlandInputBackend> for AnlandTouchUpEvent {}

#[derive(Debug, Clone, Copy)]
pub struct AnlandTouchFrameEvent {
    pub base: AnlandEventBase,
}

impl Event<AnlandInputBackend> for AnlandTouchFrameEvent {
    fn time(&self) -> u64 {
        self.base.time()
    }

    fn device(&self) -> AnlandDevice {
        AnlandDevice::consumer()
    }
}

impl TouchFrameEvent<AnlandInputBackend> for AnlandTouchFrameEvent {}

#[derive(Debug, Clone, Copy)]
enum AnlandTabletEventKind {
    Axis,
    Proximity(ProximityState),
    Tip(TabletToolTipState),
    Button { button: u32, state: ButtonState },
}

#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
struct AnlandTabletAxes {
    pressure: bool,
    tilt_x: bool,
    tilt_y: bool,
    distance: bool,
}

#[derive(Debug, Clone, Copy)]
pub struct AnlandTabletEvent {
    previous: ffi::TabletSample,
    current: ffi::TabletSample,
    extent: (u32, u32),
    kind: AnlandTabletEventKind,
    axes: AnlandTabletAxes,
    seat_button_count: u32,
}

impl Event<AnlandInputBackend> for AnlandTabletEvent {
    fn time(&self) -> u64 {
        u64::from(self.current.time_msec) * 1_000
    }

    fn device(&self) -> AnlandDevice {
        AnlandDevice::tablet(self.current.device_id)
    }
}

impl AbsolutePositionEvent<AnlandInputBackend> for AnlandTabletEvent {
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

impl TabletToolEvent<AnlandInputBackend> for AnlandTabletEvent {
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

impl TabletToolAxisEvent<AnlandInputBackend> for AnlandTabletEvent {}

impl TabletToolProximityEvent<AnlandInputBackend> for AnlandTabletEvent {
    fn state(&self) -> ProximityState {
        match self.kind {
            AnlandTabletEventKind::Proximity(state) => state,
            _ => unreachable!("non-proximity tablet event"),
        }
    }
}

impl TabletToolTipEvent<AnlandInputBackend> for AnlandTabletEvent {
    fn tip_state(&self) -> TabletToolTipState {
        match self.kind {
            AnlandTabletEventKind::Tip(state) => state,
            _ => unreachable!("non-tip tablet event"),
        }
    }
}

impl TabletToolButtonEvent<AnlandInputBackend> for AnlandTabletEvent {
    fn button(&self) -> u32 {
        match self.kind {
            AnlandTabletEventKind::Button { button, .. } => button,
            _ => unreachable!("non-button tablet event"),
        }
    }

    fn seat_button_count(&self) -> u32 {
        self.seat_button_count
    }

    fn button_state(&self) -> ButtonState {
        match self.kind {
            AnlandTabletEventKind::Button { state, .. } => state,
            _ => unreachable!("non-button tablet event"),
        }
    }
}

/// The anland input backend. It never produces events on its own; events are pushed
/// by `poll()` from the consumer.
#[derive(Debug)]
pub struct AnlandInputBackend {
    pub native_w: u32,
    pub native_h: u32,
    /// Scale from the consumer's physical pixel deltas to niri's logical
    /// coordinate space. The anland consumer reports pointer motion in its
    /// native (physical) pixel space, while smithay expects relative motion in
    /// logical output coordinates.
    pub scale: f64,
    pub time_offset: u64,
    pressed_keys: BTreeSet<i32>,
    pressed_buttons: BTreeSet<i32>,
    touch_slots: BTreeSet<i32>,
    tablet_devices: BTreeSet<u32>,
    tablet_tools: BTreeMap<u32, ffi::TabletSample>,
    pending_events: VecDeque<smithay::backend::input::InputEvent<AnlandInputBackend>>,
}

impl Default for AnlandInputBackend {
    fn default() -> Self {
        Self {
            native_w: 0,
            native_h: 0,
            scale: 1.5,
            time_offset: 0,
            pressed_keys: BTreeSet::new(),
            pressed_buttons: BTreeSet::new(),
            touch_slots: BTreeSet::new(),
            tablet_devices: BTreeSet::new(),
            tablet_tools: BTreeMap::new(),
            pending_events: VecDeque::new(),
        }
    }
}

impl AnlandInputBackend {
    pub fn new(native_w: u32, native_h: u32) -> Self {
        Self {
            native_w,
            native_h,
            ..Self::default()
        }
    }

    pub fn pop_pending(&mut self) -> Option<smithay::backend::input::InputEvent<Self>> {
        self.pending_events.pop_front()
    }

    /// Flush active tablet state before DeviceRemoved on consumer disconnect.
    pub fn queue_tablet_releases(&mut self) {
        let time_msec = crate::utils::get_monotonic_time().as_millis() as u32;
        let tools = self.tablet_tools.values().copied().collect::<Vec<_>>();
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
            self.pending_events
                .push_back(smithay::backend::input::InputEvent::DeviceRemoved {
                    device: AnlandDevice::tablet(device_id),
                });
        }
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
            self.pending_events
                .push_back(smithay::backend::input::InputEvent::DeviceAdded {
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
                AnlandTabletEventKind::Proximity(ProximityState::In),
                Self::tablet_axes(previous, current, true),
                0,
            );
        } else if is_in_proximity
            && axes_changed != AnlandTabletAxes::default()
            && was_tip_down == is_tip_down
        {
            self.push_tablet_event(
                previous,
                current,
                AnlandTabletEventKind::Axis,
                axes_changed,
                0,
            );
        }

        if was_tip_down != is_tip_down {
            let axes = if proximity_in {
                AnlandTabletAxes::default()
            } else {
                axes_changed
            };
            self.push_tablet_event(
                previous,
                current,
                AnlandTabletEventKind::Tip(if is_tip_down {
                    TabletToolTipState::Down
                } else {
                    TabletToolTipState::Up
                }),
                axes,
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
                    AnlandTabletEventKind::Button {
                        button,
                        state: if is_pressed {
                            ButtonState::Pressed
                        } else {
                            ButtonState::Released
                        },
                    },
                    AnlandTabletAxes::default(),
                    other_button_count + buttons_after.count_ones(),
                );
            }
        }

        if was_in_proximity && !is_in_proximity {
            self.push_tablet_event(
                previous,
                current,
                AnlandTabletEventKind::Proximity(ProximityState::Out),
                AnlandTabletAxes::default(),
                0,
            );
        }
        self.tablet_tools.insert(current.tool_id, current);
    }

    fn tablet_axes(
        previous: ffi::TabletSample,
        current: ffi::TabletSample,
        force: bool,
    ) -> AnlandTabletAxes {
        AnlandTabletAxes {
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
        kind: AnlandTabletEventKind,
        axes: AnlandTabletAxes,
        seat_button_count: u32,
    ) {
        let event = AnlandTabletEvent {
            previous,
            current,
            extent: (self.native_w, self.native_h),
            kind,
            axes,
            seat_button_count,
        };
        use smithay::backend::input::InputEvent as I;
        self.pending_events.push_back(match kind {
            AnlandTabletEventKind::Axis => I::TabletToolAxis { event },
            AnlandTabletEventKind::Proximity(_) => I::TabletToolProximity { event },
            AnlandTabletEventKind::Tip(_) => I::TabletToolTip { event },
            AnlandTabletEventKind::Button { .. } => I::TabletToolButton { event },
        });
    }

    /// Replay these through State::process_input_event before new-session input,
    /// so Smithay grabs/modifiers and niri's shortcut state see real releases.
    pub fn take_releases(&mut self) -> Vec<ffi::InputEvent> {
        let mut events = Vec::new();
        for code in std::mem::take(&mut self.pressed_keys) {
            events.push(ffi::InputEvent {
                type_: ffi::INPUT_TYPE_KEY,
                code,
                action: ffi::INPUT_ACTION_UP,
                ..Default::default()
            });
        }
        for code in std::mem::take(&mut self.pressed_buttons) {
            events.push(ffi::InputEvent {
                type_: ffi::INPUT_TYPE_POINTER_BUTTON,
                code,
                action: 0,
                ..Default::default()
            });
        }
        let touches = std::mem::take(&mut self.touch_slots);
        let had_touches = !touches.is_empty();
        for pointer_id in touches {
            events.push(ffi::InputEvent {
                type_: ffi::INPUT_TYPE_TOUCH,
                pointer_id,
                action: ffi::INPUT_ACTION_UP,
                ..Default::default()
            });
        }
        if had_touches {
            events.push(ffi::InputEvent {
                type_: ffi::INPUT_TYPE_TOUCH_FRAME,
                ..Default::default()
            });
        }
        events
    }

    fn base(&self, x: f64, y: f64) -> AnlandEventBase {
        let now = crate::utils::get_monotonic_time().as_micros() as u64;
        AnlandEventBase {
            time_usec: now.saturating_sub(self.time_offset),
            native_w: self.native_w,
            native_h: self.native_h,
            x,
            y,
        }
    }
}

impl InputBackend for AnlandInputBackend {
    type Device = AnlandDevice;
    type KeyboardKeyEvent = AnlandKeyboardKeyEvent;
    type PointerAxisEvent = AnlandPointerAxisEvent;
    type PointerButtonEvent = AnlandPointerButtonEvent;
    type PointerMotionEvent = AnlandPointerMotionEvent;
    type PointerMotionAbsoluteEvent = AnlandPointerMotionAbsoluteEvent;
    type GestureSwipeBeginEvent = smithay::backend::input::UnusedEvent;
    type GestureSwipeUpdateEvent = smithay::backend::input::UnusedEvent;
    type GestureSwipeEndEvent = smithay::backend::input::UnusedEvent;
    type GesturePinchBeginEvent = smithay::backend::input::UnusedEvent;
    type GesturePinchUpdateEvent = smithay::backend::input::UnusedEvent;
    type GesturePinchEndEvent = smithay::backend::input::UnusedEvent;
    type GestureHoldBeginEvent = smithay::backend::input::UnusedEvent;
    type GestureHoldEndEvent = smithay::backend::input::UnusedEvent;
    type TouchDownEvent = AnlandTouchDownEvent;
    type TouchUpEvent = AnlandTouchUpEvent;
    type TouchMotionEvent = AnlandTouchMotionEvent;
    type TouchCancelEvent = smithay::backend::input::UnusedEvent;
    type TouchFrameEvent = AnlandTouchFrameEvent;
    type TabletToolAxisEvent = AnlandTabletEvent;
    type TabletToolProximityEvent = AnlandTabletEvent;
    type TabletToolTipEvent = AnlandTabletEvent;
    type TabletToolButtonEvent = AnlandTabletEvent;
    type SwitchToggleEvent = smithay::backend::input::UnusedEvent;
    type SpecialEvent = ();
}

/// Translate a common producer input event into a Smithay input event.
pub fn translate(
    backend: &mut AnlandInputBackend,
    raw: &ffi::InputEvent,
) -> Option<smithay::backend::input::InputEvent<AnlandInputBackend>> {
    use smithay::backend::input::{ButtonState, InputEvent as I, KeyState};

    if raw.type_ == ffi::INPUT_TYPE_TABLET_FRAME {
        backend.enqueue_tablet_frame(raw.tablet_previous, raw.tablet_current);
        return backend.pop_pending();
    }

    match raw.type_ {
        ffi::INPUT_TYPE_KEY => {
            if raw.action == ffi::INPUT_ACTION_DOWN {
                backend.pressed_keys.insert(raw.code.max(0));
            } else {
                backend.pressed_keys.remove(&raw.code.max(0));
            }
        }
        ffi::INPUT_TYPE_POINTER_BUTTON => {
            if raw.action != 0 {
                backend.pressed_buttons.insert(raw.code);
            } else {
                backend.pressed_buttons.remove(&raw.code);
            }
        }
        ffi::INPUT_TYPE_TOUCH => {
            if raw.action == ffi::INPUT_ACTION_DOWN {
                backend.touch_slots.insert(raw.pointer_id.max(0));
            } else if raw.action == ffi::INPUT_ACTION_UP {
                backend.touch_slots.remove(&raw.pointer_id.max(0));
            }
        }
        _ => {}
    }
    let base = |x: f64, y: f64| backend.base(x, y);

    match raw.type_ {
        ffi::INPUT_TYPE_KEY => Some(I::Keyboard {
            event: AnlandKeyboardKeyEvent {
                base: base(0., 0.),
                keycode: raw.code.max(0) as u32,
                state: if raw.action == ffi::INPUT_ACTION_DOWN {
                    KeyState::Pressed
                } else {
                    KeyState::Released
                },
            },
        }),
        ffi::INPUT_TYPE_POINTER_MOTION => {
            // The consumer also includes an absolute position, but using it
            // here makes pointer-lock clients fight the compositor's warp.
            // Feed the relative deltas instead so both the desktop pointer
            // and zwp_relative_pointer follow the same path.
            let scale = if backend.scale.is_finite() && backend.scale > 0. {
                backend.scale
            } else {
                1.
            };
            Some(I::PointerMotion {
                event: AnlandPointerMotionEvent {
                    base: base(f64::from(raw.x), f64::from(raw.y)),
                    dx: f64::from(raw.dx) / scale,
                    dy: f64::from(raw.dy) / scale,
                },
            })
        }
        ffi::INPUT_TYPE_POINTER_BUTTON => Some(I::PointerButton {
            event: AnlandPointerButtonEvent {
                base: base(0., 0.),
                button: raw.code as u32,
                state: if raw.action != 0 {
                    ButtonState::Pressed
                } else {
                    ButtonState::Released
                },
            },
        }),
        ffi::INPUT_TYPE_POINTER_AXIS => Some(I::PointerAxis {
            event: AnlandPointerAxisEvent {
                base: base(0., 0.),
                axis: raw.code as u32,
                value: f64::from(raw.value),
                discrete: raw.discrete,
            },
        }),
        ffi::INPUT_TYPE_TOUCH => {
            let slot = TouchSlot::from(Some(raw.pointer_id.max(0) as u32));
            match raw.action {
                ffi::INPUT_ACTION_DOWN => Some(I::TouchDown {
                    event: AnlandTouchDownEvent {
                        base: base(f64::from(raw.x), f64::from(raw.y)),
                        slot,
                    },
                }),
                ffi::INPUT_ACTION_MOVE => Some(I::TouchMotion {
                    event: AnlandTouchMotionEvent {
                        base: base(f64::from(raw.x), f64::from(raw.y)),
                        slot,
                    },
                }),
                ffi::INPUT_ACTION_UP => Some(I::TouchUp {
                    event: AnlandTouchUpEvent {
                        base: base(f64::from(raw.x), f64::from(raw.y)),
                        slot,
                    },
                }),
                _ => None,
            }
        }
        ffi::INPUT_TYPE_TOUCH_FRAME => Some(I::TouchFrame {
            event: AnlandTouchFrameEvent { base: base(0., 0.) },
        }),
        _ => None,
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
            tool_id: 17,
            tool_type: ffi::TABLET_TOOL_PEN,
            capabilities: ffi::TABLET_CAP_PRESSURE
                | ffi::TABLET_CAP_TILT
                | ffi::TABLET_CAP_DISTANCE,
            device_id: 4,
            flags: ffi::TABLET_FLAG_PROXIMITY | ffi::TABLET_FLAG_TIP_DOWN,
            buttons: ffi::TABLET_BUTTON_PRIMARY | ffi::TABLET_BUTTON_SECONDARY,
            time_msec: 1234,
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
        let mut backend = AnlandInputBackend::new(1000, 500);
        let sample = active_pen();
        backend.enqueue_tablet_frame(ffi::TabletSample::default(), sample);
        let events = std::iter::from_fn(|| backend.pop_pending()).collect::<Vec<_>>();

        assert!(
            matches!(events.first(), Some(InputEvent::DeviceAdded { device })
            if device.id() == "anland-tablet-4" && device.has_capability(DeviceCapability::TabletTool))
        );
        assert!(
            matches!(events.get(1), Some(InputEvent::TabletToolProximity { event })
            if event.state() == ProximityState::In
                && event.tool().tool_type == TabletToolType::Pen
                && event.tool().hardware_serial == 17
                && event.time() == 1_234_000
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
        let mut backend = AnlandInputBackend::new(1000, 500);
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
        let mut backend = AnlandInputBackend::new(1000, 500);
        let mut sample = active_pen();
        sample.tool_id = 18;
        sample.tool_type = ffi::TABLET_TOOL_ERASER;
        sample.flags = ffi::TABLET_FLAG_PROXIMITY;
        sample.buttons = 0;
        backend.enqueue_tablet_frame(ffi::TabletSample::default(), sample);
        let events = std::iter::from_fn(|| backend.pop_pending()).collect::<Vec<_>>();
        assert!(
            matches!(events.get(1), Some(InputEvent::TabletToolProximity { event })
            if event.tool().tool_type == TabletToolType::Eraser
                && event.tool().hardware_serial == 18)
        );
    }

    #[test]
    fn disconnect_after_device_added_preserves_queued_frame_before_releases() {
        let mut backend = AnlandInputBackend::new(1000, 500);
        let sample = active_pen();
        backend.enqueue_tablet_frame(ffi::TabletSample::default(), sample);
        assert!(
            matches!(backend.pop_pending(), Some(InputEvent::DeviceAdded { device })
            if device.id() == "anland-tablet-4")
        );

        backend.queue_tablet_releases();
        let events = std::iter::from_fn(|| backend.pop_pending()).collect::<Vec<_>>();
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
            if device.id() == "anland-tablet-4")
        );
        assert_eq!(events.len(), 9);
    }
}
