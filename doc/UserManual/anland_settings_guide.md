# Anland Settings Guide

> [中文版](anland_settings_guide_zh.md)

This guide corresponds to the **Settings** screen in the Anland Android app. Here, "desktop" means the Linux desktop displayed by Anland, not the Android home screen.

Changes are saved immediately; there is no Save button. Some options, however, are read only when a desktop connection is established. Returning from Settings to the Anland desktop normally reconnects and applies them.

- Input, extra keys bar, and display layout settings take effect when you return to the desktop view.
- Connection, audio, and output resolution settings take effect on the next connection. Returning from Settings to the desktop normally triggers one.
- Screen orientation takes effect after you exit and reopen Anland.

<a id="settings-nav"></a>
## Settings hierarchy

- [Keyboard & Keys](#keyboard)
  - [Soft Keyboard Toggle Key](#soft-keyboard)
    - [Bind Soft Keyboard Toggle Key](#bind-soft-keyboard)
    - [Raise desktop when soft keyboard opens](#raise-desktop)
  - [Immersive Mode (Experimental)](#immersive)
    - [Enable Immersive Mode](#enable-immersive)
    - [Bind Immersive Mode Key](#bind-immersive)
  - [Accessibility Key Interception](#accessibility)
  - [Extra Keys Bar](#extra-keys)
    - [Extra keys bar mode](#extra-keys-mode)
    - [Back key quickly toggles extra keys bar](#back-extra-keys)
  - [Custom Extra Keys Layout](#custom-extra-keys)
    - [Load default template](#load-default-layout)
    - [Load from file…](#load-layout-file)
    - [Layout format](#layout-format)
- [Touchpad & Mouse](#touchpad-mouse)
  - [Touchpad Mode (relative movement)](#touchpad-mode)
  - [Capture external pointer](#pointer-capture)
  - [Touchpad Pointer Sensitivity](#pointer-sensitivity)
  - [Reverse scroll direction](#reverse-scroll)
  - [Disable pinch and multi-finger gestures](#disable-multifinger)
  - [Two-finger scroll speed](#scroll-speed)
  - [Scroll detection threshold](#scroll-threshold)
  - [Gesture commit threshold](#gesture-threshold)
  - [Gesture magnitude](#gesture-scale)
- [Connection](#connection)
  - [Daemon socket path](#socket-path)
  - [Open a second window](#second-window)
  - [Connect with root](#root-connection)
  - [Enable foreground scheduling](#foreground-scheduling)
    - [Foreground scheduling scope](#foreground-scope)
    - [Scan stop process names](#scan-stop-processes)
  - [Forward microphone to desktop](#microphone)
  - [Forward camera to desktop](#camera)
  - [Audio keep-alive](#audio-keepalive)
  - [Audio latency](#audio-latency)
- [Display Resolution](#resolution)
  - [Presets, width, and height](#resolution-values)
  - [Auto-stretch display](#auto-stretch)
- [General](#general)
  - [Screen Orientation](#orientation)
    - [Force screen orientation](#force-orientation)
  - [Settings Notification](#notification)
    - [Show settings notification](#show-notification)

The version number at the bottom of the Settings home screen is only for identifying the installed APK version and cannot be changed.

<a id="keyboard"></a>
## Keyboard & Keys

<a id="soft-keyboard"></a>
### Soft Keyboard Toggle Key

<a id="bind-soft-keyboard"></a>
#### Bind Soft Keyboard Toggle Key

Assign a hardware key to the Android system soft keyboard. After you tap the button, Anland records the next key pressed within five seconds. If no key is pressed before the countdown ends, the binding is cleared.

When the Anland desktop has focus, pressing this key shows or hides the system soft keyboard. The key is no longer sent to the Linux desktop, so choose a spare key that you do not normally need in Linux.

<a id="raise-desktop"></a>
#### Raise desktop when soft keyboard opens

When enabled, the visible soft keyboard and extra keys bar occupy space at the bottom, and the Linux desktop shrinks above them so you can see where you are typing. When disabled, the keyboard and bar overlay the desktop at its original size and may cover content at the bottom.

<a id="immersive"></a>
### Immersive Mode (Experimental)

<a id="enable-immersive"></a>
#### Enable Immersive Mode (Experimental)

This is the master switch for immersive mode. Enabling it does not enter the mode immediately; first bind the toggle key below, then press that key once in the Anland desktop.

While active, physical input from the touchscreen, keyboard, mouse, and touchpad goes directly to the Linux desktop. Android's Back gesture, status bar, and navigation bar no longer take those inputs. This is useful for games, full-screen applications, and other cases that need complete keyboard and mouse behavior.

This feature requires working root access and at least one input device that can be taken over. Anland automatically leaves immersive mode and returns input to Android when you switch apps, the window loses focus, or the screen is locked or turned off, so the Android interface remains accessible.

Internally, Anland's root helper temporarily takes exclusive control of these input devices and forwards their raw events to the Linux desktop. It does more than hide the Android system bars.

<a id="bind-immersive"></a>
#### Bind Immersive Mode Key

Choose a physical key dedicated to entering and leaving immersive mode. Press it once in the desktop to enter, and press the same key again to exit. A volume key or another hardware key with a scan code is recommended; avoid Android on-screen navigation gestures.

If the binding status says that the key reports no scan code, it cannot be used for immersive mode. Bind a different physical key. Android no longer receives normal key events during immersive mode, so the helper must recognize the exit key by its raw scan code.

<a id="accessibility"></a>
### Accessibility Key Interception

When enabled, Anland uses an Android accessibility service to receive hardware keys that a normal window may not see, such as Fn combinations and F1-F12. It also converts Esc keys that some tablet keyboards report as Back or Browser Back into a Linux Esc.

Use this option when function keys, Esc, or key combinations from an external keyboard do not reach the Linux desktop correctly. On first use, enable the `Anland KeyInterceptor` service under Android **Settings > Accessibility**. If this option has no effect, confirm that permission first.

The service intercepts keys only while the Anland desktop window is in the foreground and has focus. It does not take over keys in other apps.

<a id="extra-keys"></a>
### Extra Keys Bar

The extra keys bar is a shortcut bar at the bottom of the desktop. By default, it includes ESC, TAB, CTRL, ALT, arrow keys, HOME, END, PgUp, PgDn, a soft-keyboard button, and a Settings button. It is useful for terminals, editors, and desktop shortcuts when you do not have a full physical keyboard.

In the default layout, tapping a modifier such as CTRL or ALT applies it only to the next key; long-press it to lock, then tap it again to unlock. Arrow, page, and other repeat-enabled keys can be held to send repeated presses. Swipe up on a key with a secondary action to invoke its popup key: for example, the default `-` pops up `|`, and `⌨` pops up the floating virtual keyboard.

<a id="extra-keys-mode"></a>
#### Extra keys bar mode

- **Always show**: keep the bar visible while the desktop is open.
- **Never show**: hide the bar by default.
- **Show with soft keyboard**: show the bar only while the Android system soft keyboard is visible.

When visible, the bar occupies space at the bottom of the desktop. If **Raise desktop when soft keyboard opens** is disabled, the bar overlays the desktop instead.

<a id="back-extra-keys"></a>
#### Back key quickly toggles extra keys bar

When enabled, pressing Android Back in the Anland desktop shows or hides the extra keys bar. This is a manual toggle, so it can temporarily show the bar even when **Never show** is selected.

If **Capture external pointer** is also enabled, the first Android Back press releases mouse/touchpad capture. Press Back again to toggle the bar.

<a id="custom-extra-keys"></a>
### Custom Extra Keys Layout

You can edit the JSON layout for the extra keys bar directly. The editor shows **Valid layout** or an error below it. An invalid or empty layout falls back to the built-in default when you return to the desktop.

<a id="load-default-layout"></a>
#### Load default template

Replace the editor contents with the built-in two-row default layout. You can then add, remove, or change keys.

<a id="load-layout-file"></a>
#### Load from file…

Open the Android file picker, read a JSON or text file, and replace the editor contents with it. The file is validated and saved in the same way as a manual edit.

<a id="layout-format"></a>
#### Layout format

The top-level `rows` value is an array of rows, and each row is an array of keys. This is a minimal example:

```json
{
  "rows": [
    [
      {"label": "ESC", "type": "key", "code": 1},
      {"label": "CTRL", "type": "modifier", "code": 29},
      {"label": "⌨", "type": "keyboard", "popup": {"label": "VK", "type": "vkeyboard"}}
    ]
  ]
}
```

| `type` | Action | Common fields |
| --- | --- | --- |
| `key` | Sends one key press and release to Linux | `code` is a Linux evdev key code; may include `"repeat": true` |
| `text` | Enters text directly | `text`; defaults to `label` when omitted |
| `modifier` | Toggles a modifier such as CTRL, ALT, or SHIFT | `code` is a Linux evdev key code |
| `keyboard` | Shows or hides the Android system soft keyboard | None |
| `vkeyboard` | Shows or hides Anland's floating virtual keyboard | None |
| `settings` | Opens Anland Settings | None |

Any key may include a `popup` object; swiping up from that key performs the popup action. The `code` for `key` and `modifier` is a Linux evdev key code, not an Android `KEYCODE`. A green **Valid layout** status means the JSON structure can be loaded; you should still confirm that the target Linux system supports the key codes themselves.

<a id="touchpad-mouse"></a>
## Touchpad & Mouse

<a id="touchpad-mode"></a>
### Touchpad Mode (relative movement)

When enabled, sliding a finger on the screen moves the mouse pointer like a laptop touchpad instead of selecting the corresponding point on the screen. Use one finger to move the pointer, tap for left click, use two fingers to scroll or tap for right click, and move after a long press to drag.

When disabled, screen touches are sent directly to the Linux desktop as absolute-position multitouch input. This is suitable for touch-optimized applications, drawing, and other uses that require direct positioning.

<a id="pointer-capture"></a>
### Capture external pointer

When enabled, an external mouse or physical touchpad uses relative movement and is locked to the Anland desktop. Moving the pointer to a screen edge does not reveal the Android status or navigation bar, which is useful for desktop games and continuous mouse movement.

Press Android Back to release capture temporarily, then click the Anland desktop to capture it again. Screen touch is unaffected.

Even when this option is disabled, a Linux application may temporarily request pointer lock when needed, as some games do. Normal behavior resumes automatically after the application releases the lock.

<a id="pointer-sensitivity"></a>
### Touchpad Pointer Sensitivity (Acceleration)

Adjusts relative pointer movement from 0.5x to 10x. Higher values move the pointer farther during a fast swipe or mouse movement; lower values make precise positioning easier. This primarily affects the on-screen touchpad, captured physical touchpads, and external mice in relative mode. It does not affect absolute touch that maps directly to the screen.

This setting uses acceleration that varies with movement speed, not a simple fixed multiplier. Slow adjustments remain controlled, while faster movement receives stronger acceleration.

<a id="reverse-scroll"></a>
### Reverse scroll direction

When enabled, scrolling is "natural": pushing your fingers upward moves the content in the same direction. When disabled, traditional mouse-wheel direction is used. This applies to two-finger scrolling on both the on-screen and captured physical touchpads.

<a id="disable-multifinger"></a>
### Disable pinch and multi-finger gestures

When enabled, two-finger pinch/spread and gestures with three or more fingers are ignored and never reach the Linux desktop. Two-finger scrolling and two-finger tap for right click continue to work.

Enable this if the Linux desktop or an application mistakes multi-finger input for zoom or switching gestures. When disabled, multi-finger gestures other than scrolling are forwarded to Linux as multitouch input.

<a id="scroll-speed"></a>
### Two-finger scroll speed

Adjusts how much scrolling is produced by two-finger movement. A higher value scrolls farther for the same finger travel; change it if scrolling feels too fast or too slow.

<a id="scroll-threshold"></a>
### Scroll detection threshold

Controls how far two fingers must travel before Anland considers them to be moving. Lower this value if pinch/spread is often mistaken for scrolling; raise it if slight jitter starts scrolling.

<a id="gesture-threshold"></a>
### Gesture commit threshold

Controls when Anland resolves an ambiguous two-finger movement as scrolling or another gesture. Lower this value if two-finger scrolling engages too late or requires excessive movement; raise it if gestures are often misclassified.

<a id="gesture-scale"></a>
### Gesture magnitude

When pinch/spread and gestures with three or more fingers are not disabled, Anland forwards them as Linux multitouch input mapped to a square centered on the current mouse pointer. This setting is the side length of that square.

A larger value makes the same finger movement travel farther in Linux. It affects only these forwarded multi-finger gestures, not normal one-finger pointer movement or two-finger scrolling.

<a id="connection"></a>
## Connection

<a id="socket-path"></a>
### Daemon socket path

This is the Unix domain socket path used by the Anland Android app to connect to the display daemon. The default is:

```text
/data/local/tmp/display_daemon.sock
```

You normally do not need to change it. Enter another path only when the desktop startup script places the daemon elsewhere or when you run multiple independent daemons. This is a local filesystem path, not an IP address or network URL.

If the path is wrong, the daemon is not running, or the target is not a Unix socket, Anland cannot open the desktop. On first launch, the app may open Settings directly so you can correct the path.

<a id="second-window"></a>
### Open a second window (own daemon & title)

Use this section to start another Anland window, for example in Android freeform or split-screen mode, or to view two independent Linux desktop sessions at the same time.

- **Window name** is used only as the Android window title in Recents or freeform mode, for example `work`. It does not create or rename a Linux daemon.
- **Socket path** selects the daemon for the new window. To get a truly independent second desktop, enter the socket of another running daemon.
- **Open second window** launches it with the values currently entered. The name and socket apply only to this launch and do not replace the main window's saved connection path.

If the selected socket is already open in another Anland window, the app switches to that window instead of connecting to the same daemon twice. If the path does not exist, the new window reports the failure and closes.

<a id="root-connection"></a>
### Connect with root (helper)

When enabled, Anland uses `su` to launch a small root helper. The helper opens the daemon socket and passes the established connection back to the app.

Enable this and grant permission in your root manager when the socket is in a location that a normal Android app cannot access, such as some root-owned directories. When disabled, Anland can connect only to sockets that the app itself has permission to access.

This does not change the permissions of the Linux desktop itself. Root is used only for the restricted Android-side connection. Changes take effect on the next connection.

<a id="foreground-scheduling"></a>
### Enable foreground scheduling (root)

When enabled, Anland puts the related Linux desktop processes in Android's foreground scheduling group, giving them higher CPU priority. Try it if the desktop stutters under system load or the active application responds slowly.

This feature requires working `su` and increases power consumption. It raises CPU scheduling priority but cannot fix every issue caused by the GPU, network, or an application itself. Leave it disabled if the desktop is already smooth. Anland restores the normal scheduling scope when the connection ends.

<a id="foreground-scope"></a>
#### Foreground scheduling scope

- **1 - Focused application** keeps the compositor and the Linux application currently in use at higher priority. When focus changes, it restores the previous application to normal priority. This is suitable for everyday desktop use and has a smaller impact.
- **2 - Whole desktop session** boosts the entire Anland Linux desktop process tree for the duration of the connection, including background applications and services. It prioritizes performance but uses more power and has a greater effect on other system tasks.

<a id="scan-stop-processes"></a>
#### Scan stop process names (optional)

This is an advanced option for nonstandard container or session structures. It does not mean "stop these processes." It tells the root helper which process names should stop its upward search through the Linux desktop session's parents, allowing it to determine the session boundary to boost.

Separate multiple names with colons, for example `systemd:init`. Leaving the field empty uses the built-in boundary list; entering a value replaces that list rather than appending to it. Most users should leave this empty. An incorrect list may make the boosted scope too broad, too narrow, or ineffective.

<a id="microphone"></a>
### Forward microphone to desktop

When enabled, the Android device's microphone becomes a recording input for the Linux desktop and can be used for meetings, recording, and voice chat. Android asks for microphone permission the first time you enable it.

Disabling this does not affect audio played from the Linux desktop through the phone or tablet speaker. It only disables the phone-to-Linux microphone direction. Leave it disabled when you are not recording to protect privacy and reduce power use.

<a id="camera"></a>
### Forward camera to desktop

When enabled, the Linux desktop can use the Android device's cameras as video sources. Android requests camera permission the first time you enable it; a camera opens only when a Linux application actively requests a video stream.

If multiple Anland windows request the same camera at once, Android opens one capture stream and shares the same image with those windows.

<a id="audio-keepalive"></a>
### Audio keep-alive

When enabled, Anland keeps the phone-side audio output path active so short Linux desktop sounds, such as volume ticks and key clicks, play immediately.

When disabled, the audio path sleeps after about 1.5 seconds of desktop silence to reduce standby power. It wakes automatically for the next sound, which may make short sounds more likely to have an audible start delay. Enabling this option uses slightly more standby power.

<a id="audio-latency"></a>
### Audio latency

These two options adjust the target buffer size for each audio path:

| Option | Audio direction | Affects |
| --- | --- | --- |
| **Speaker (desktop → phone)** | Linux desktop to Android speaker/headphones | Playback latency for games, video, and system sounds |
| **Microphone (phone → desktop)** | Android microphone to Linux desktop | Input latency for calls, recording, and voice chat |

Available presets are **Auto (engine default)** and approximately 1, 3, 5, 10, or 20 milliseconds. Smaller buffers normally reduce latency but are more likely to cause crackles, artifacts, or dropouts while the device is busy; larger buffers are more stable. These values are audio-buffer targets, not guaranteed end-to-end latency.

<a id="resolution"></a>
## Display Resolution

<a id="resolution-values"></a>
### Presets, width, and height

This page sets the logical render/output resolution of the Linux desktop. It does not change the physical resolution of the Android device's panel. Lower resolutions usually reduce rendering, video-memory, and bandwidth load; higher resolutions provide more desktop space and sharper detail at a higher cost.

Presets include Auto (`0×0`), 4K, 2K, 1080p, 720p, 480p, and **Screen × 1.0 / 0.8 / 0.75 / 0.5 / 0.25**. Selecting a normal preset fills in the width and height fields below. **Preset…** itself does not change the current values.

A **Screen ×** option calculates concrete pixel dimensions from the Android panel's long and short sides in landscape orientation, then writes those values into the width and height fields. It is not a persistent scale factor that follows future screen changes.

You can also enter the width and height directly. For a custom resolution, enter two positive integers; select `0×0` to use the native size of the Anland view automatically. The setting is sent to the Linux desktop on the next connection.

<a id="auto-stretch"></a>
### Auto-stretch display

When enabled, the Linux desktop fills the Anland window. If the custom resolution and device window have different aspect ratios, the image may be stretched.

When disabled, the image keeps the custom resolution's original aspect ratio and is centered with black bars (letterboxing). Anland maps touch and mouse coordinates automatically, so you do not need to compensate for the bars. Letterboxing normally does not occur when using the automatic native resolution.

<a id="general"></a>
## General

<a id="orientation"></a>
### Screen Orientation

<a id="force-orientation"></a>
#### Force screen orientation

- **Default (system)** lets Android auto-rotation and system settings decide.
- **Force landscape** keeps Anland in landscape; rotating the device can still switch between the two landscape orientations.
- **Force portrait** keeps Anland in portrait; rotating the device can still switch between the two portrait orientations.

This controls the orientation of the Anland Android window; it does not change the resolution configured inside the Linux desktop. Exit and reopen Anland after changing it.

<a id="notification"></a>
### Settings Notification

<a id="show-notification"></a>
#### Show settings notification

When enabled, a persistent notification titled **Anland** with the text **Tap to open Settings** appears while the Anland desktop window is in the foreground. Tap it to open Settings quickly. When disabled, the notification is not shown.

On Android 13 and later, you must also grant notification permission the first time. The notification will not appear if its system notification channel has been disabled manually. It is only a shortcut to Settings; it does not keep the daemon or Linux desktop running in the background. Anland removes it when you leave the app.
