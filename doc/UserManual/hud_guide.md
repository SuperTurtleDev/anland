# HUD Controls User Guide

> [中文版](hud_guide_zh.md) · [Settings guide](anland_settings_guide.md)

The HUD provides touch buttons and pointer controls over the Linux desktop in Anland. This guide describes the current Android app. Shortcut results also depend on your Linux desktop and application bindings.

## Getting started

1. Open **Settings → Keyboard & Keys** in the Android app and enable **Show floating HUD controls**.
2. Tap **Edit floating control layout**. The editor opens on a black canvas; opening it also enables the HUD switch.
3. Tap **ADD** and choose a control. Drag it to move it; tap it to open its properties. Tap **SAVE**, then **EXIT** when finished.
4. Return to the desktop to use the controls. Rotate the device to edit and save the other orientation.

HUD layout edits require saving in the editor. Keyboard settings and positions are stored separately and do not depend on the layout Save button. Discarding on exit applies to unsaved HUD layout changes.

## Editor toolbar

| Button | Purpose |
| --- | --- |
| **SAVE** | Save the HUD layout profile and continue editing. |
| **ADD** | Open the control list and place the selected control on the canvas. |
| **SNAP** | Toggle grid snapping to help align controls while dragging. |
| **LAYOUT** | Open the copy, preset, and clear menu described below. |
| **EXIT** | Leave the editor. If the layout has unsaved changes, choose **SAVE & EXIT**, **DISCARD**, or **CANCEL**. Android Back also exits. |
| **Edit keyboard** | Switch to the virtual keyboard and its dedicated HUD keyboard layer. |
| **Back to HUD** | Return from the keyboard layer to the regular HUD layer. |
| **Keyboard settings** | Shown on the keyboard layer; opens split, appearance, and wing-key settings. |

### Layout menu

| Option | Purpose |
| --- | --- |
| **Copy Portrait Layout Here / Copy Landscape Layout Here** | Replace the current layer's controls with the other orientation's regular HUD layout. Even on the keyboard layer, the source is the regular HUD, not the other orientation's keyboard layer. |
| **Desktop / Productivity Preset** | Load a mouse TrackPoint, keyboard shortcuts, and window controls. |
| **User Portrait Hotkeys Layout** | Load the built-in portrait shortcut layout, including on a landscape canvas. |
| **Clear Active Layout** | Remove all controls in the current layer and orientation. |

These options immediately replace the current canvas controls; save afterwards to keep them. Before saving, you can exit and discard unsaved changes to restore the saved layout.

## Add menu and controls

| Control | How to use it on the desktop |
| --- | --- |
| **Standard Key Button** | Press to send its main action. Swipe up for a configured popup action. Repeating while held can be enabled. |
| **Super Gesture Nub** | Tap for the main action; swipe left, right, up, or down for the corresponding assigned action. |
| **TrackPoint (Mouse / Scroll)** | Hold your finger away from the center to continuously move the pointer or scroll; release to stop. A quick tap sends left click. |
| **Steam Deck Trackpad** | Slide one finger to move; tap for left click; hold or tap with two fingers for right click. Slide two fingers to scroll, or double-tap and drag to hold the left button while dragging. |
| **Scroll Bar** | Drag along the control to scroll; switch between vertical and horizontal in properties. |
| **D-Pad (Sliding Cross)** | Hold a direction or slide to another; diagonals are supported. Release to release the directions. Defaults to keyboard arrow keys; customize each direction independently in properties. |
| **Group container** | Automatically arrange standard keys together and drag the container to move the group. |

## Properties panel

Tap a control on the canvas to open its panel. Drag the panel header to move it, or tap its close button to dismiss it. Options vary by control.

| Item or button | Purpose |
| --- | --- |
| **Display Label** | Change the visible text without changing the action. |
| **Width, Height (dp)** | Adjust size with a slider or numeric input. dp is a density-related size unit. |
| **Corner (dp)** | Adjust corner roundness. |
| **Opacity (%)** | Adjust visibility; lower values are more transparent. |
| **Main Action** | Tap its action button to choose an action; hold the button to clear it. |
| **Popup Action (Swipe Up)** | Action for swiping up on a standard key; tap to choose or hold to clear. |
| **Swipe Actions: Left / Right / Up / Down** | Actions for each Super Gesture or sliding-cross direction; tap to choose or hold to clear. |
| **Repeat while held** | Repeat applicable key actions, useful for arrows, Backspace, or page keys. |
| **Mode** | Cycle TrackPoint between mouse and scroll; switch the scroll bar between vertical and horizontal. |
| **Sensitivity (×10)** | Adjust response speed for the relevant pointer or scrolling control. |
| **Deadzone (dp)** | Adjust the area around the center that does not trigger movement. |
| **Color Theme** | Choose a preset color scheme. |
| **Haptic Feedback** | Toggle feedback for the control; actual vibration also depends on device settings. |
| **Nudge Position** | Move the control in small steps with the direction buttons. |
| **DUP** | Copy the selected control, then drag the copy into place. |
| **DEL → CONFIRM?** | Tap Delete and tap again to confirm removal. |

### Group container

Select the container and use **Add key to group** under its children section to choose an action, or **Move existing key in** to choose a standalone standard key on the canvas. You can also drag a standalone standard key onto the container. Children are automatically arranged and the container grows to fit. Tap a child's action to reassign it; **Take out** returns it to the canvas, and **DEL** removes it from the group. Drag the container to move the whole group.

## Choosing keys and actions

Tap an action in the properties panel to open the picker. Switch categories at the top; the category bar scrolls horizontally. Use search to find keys, and **Back** to close the picker.

| Category | Contents |
| --- | --- |
| **Keys** | Ordinary keys including letters, digits, and function keys. |
| **MODS & SYS** | Modifiers such as Ctrl and Alt, plus Android soft keyboard, Anland virtual keyboard, Settings, mouse buttons, scroll, Steam Deck mode, and other system actions. |
| **Combo** | Build an action with up to three keys. |

In the combo builder, tap a slot and choose its key. **Clear All** empties the slots; **Apply Combo** applies the selection. For example, select the three slots and fill them with Ctrl, Shift, and C, then apply. A modifier action toggles its active state when tapped and automatically releases after the next key is sent.

## Buttons in the built-in presets

### User Portrait Hotkeys and Desktop / Productivity

| Control | Default tap / swipe action |
| --- | --- |
| Super Gesture Nub | Left: Super+Left; right: Super+Right; up: Super+F; down: toggle the Android soft keyboard. Window effects depend on desktop shortcuts. |
| **Q** | Tap: Super+Q; swipe up: open Anland Settings. |
| **OV** | Tap: Super+O; swipe up: Enter. |
| **CP** | Tap: Ctrl+Shift+V; swipe up: Ctrl+Shift+C, commonly paste and copy in terminals. |
| **MOUSE** | Mouse TrackPoint in the productivity preset. |
| **ESC, TAB, ENTER, BKSP** | Productivity preset keys sending Escape, Tab, Enter, and Backspace. |
| **CTRL, ALT** | Modifier keys in the productivity preset. |
| **KB** | In the productivity preset, toggle the Anland virtual keyboard; hold to hide or restore the HUD. |
| **⚙** | Open Settings in the productivity preset. |

The portrait hotkeys preset contains only the Super gesture, Q, OV, and CP. Add KB or other controls yourself, or use the productivity preset.


## Keyboard layer and settings

Regular HUD and keyboard layers are stored separately, with separate portrait and landscape layouts. Tap **Edit keyboard** to see the keyboard and its HUD layer. This layer starts empty and accepts the controls described above. It is used while the virtual keyboard is visible; closing the keyboard returns to the regular HUD layer.

Tap **Keyboard settings** on the keyboard layer:

- **Use split arc keyboard in landscape:** split the keyboard into two halves in landscape. In the editor, drag a blank area near each half to position it; positions are saved. Tap **HIDE** during use to dismiss it.
- **Keyboard handedness:** choose the left- or right-handed layout.
- **Key appearance:** choose filled, outlined, or transparent.
- **Vibrate on virtual key press:** toggle keyboard vibration.
- **Customize thumb wings…:** choose the left and right wing-key functions, restore defaults if desired, and tap **Done** to close.

Move the portrait floating keyboard using its drag handle. Keyboard settings and positions are saved separately; discarding HUD layout edits does not undo them.

## Backup, restore, and troubleshooting

Under **Settings → Keyboard & Keys**, choose **Export HUD layout…** and use the Android file picker to save JSON. If there is no layout to export, save one in the editor first. **Import HUD layout…** replaces the current HUD profile and enables the HUD. Export a backup first if you want to keep the existing layout.

- **Controls are missing:** check the display switch, current orientation, and layer. If hidden by holding KB, hold the still-visible KB again to restore them. In the editor, check whether the current layout is empty or its opacity is too low.
- **Controls do not respond:** check their bindings and whether the target supports the input. Under **Settings → Immersive Mode → Immersive input**, check that **Touch HUD controls in immersive mode** is enabled. When disabled while the touchscreen is grabbed, the HUD is display-only and touches go to the desktop.
- **Layout changes on rotation:** portrait and landscape are stored separately; rotate and edit that orientation, or use the layout menu to copy.
- **Controls disappear when the keyboard opens:** the keyboard uses a separate layer. Tap **Edit keyboard** in the editor and add controls to that layer.

Each sliding-cross direction can use its own key, combination, or other action. Keys and combinations stay held until you slide away or release. Loading an older profile removes virtual gamepad buttons and retains the cross with keyboard arrows by default; touchpads and other keyboard/mouse controls are preserved.
