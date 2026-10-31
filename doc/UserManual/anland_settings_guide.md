# Anland Settings Guide

> [中文版](anland_settings_guide_zh.md)

This guide corresponds to the **Settings** screen in the Anland Android app. Here, "desktop" means the Linux desktop displayed by Anland, not the Android home screen.

Settings are saved automatically; HUD layouts must be saved in the editor.

Some options apply only after the desktop reconnects. Returning from Settings to the Anland desktop normally triggers a reconnect.

- Input, HUD, and display layout settings take effect when you return to the desktop view.
- Connection, audio, and output resolution settings take effect on the next connection. Returning from Settings to the desktop normally triggers one.
- Screen orientation takes effect after you exit and reopen Anland.

<a id="settings-nav"></a>
## 🧭 Quick navigation

- [⌨️ Keyboard & Keys](#keyboard)
- [🖱️ Touchpad & Mouse](#touchpad-mouse)
- [🔌 Connection](#connection)
- [🖥️ Display Resolution](#resolution)
- [🎮 Immersive Mode](#immersive)
- [⚙️ General](#general)

The version number at the bottom of the Settings home screen is only for identifying the installed APK version and cannot be changed.

<a id="keyboard"></a>
## ⌨️ Keyboard & Keys

<a id="soft-keyboard"></a>
### Soft Keyboard Toggle Key

<a id="bind-soft-keyboard"></a>
#### Bind Soft Keyboard Toggle Key

**Bind a key:** tap the button, then press the target hardware key within five seconds. If you do not press a key before the countdown ends, the binding is cleared.

**Use the key:** while the Anland desktop has focus, press it to show or hide the Android system soft keyboard. The key is not sent to Linux, so choose one you do not normally need there.

<a id="raise-desktop"></a>
#### Raise desktop when soft keyboard opens

- **On:** the keyboard occupies space at the bottom. The desktop shrinks and moves up, keeping the typing area visible.
- **Off:** the desktop stays full size, with the keyboard overlaid on top. It may cover content near the bottom.

<a id="accessibility"></a>
### Accessibility Key Interception

When enabled, Anland uses an Android accessibility service to receive hardware keys that a normal window may not see, such as Fn combinations and F1-F12.

It also converts Esc keys that some tablet keyboards report as Back or Browser Back into a Linux Esc.

**When to use it:** try this option if function keys, Esc, or key combinations from an external keyboard do not reach the Linux desktop correctly.

**First use:** enable the `Anland KeyInterceptor` service under Android **Settings > Accessibility**. If the option has no effect, check this permission first.

**Scope:** the service intercepts keys only while the Anland desktop window is in the foreground and has focus. It does not take over keys in other apps.

<a id="hud"></a>
### Floating HUD controls

The HUD provides configurable touch controls over the Linux desktop for shortcuts, mouse operations, and game controls. The former custom extra keys bar has been removed; HUD buttons can replace it. See the [HUD Controls User Guide](hud_guide.md) for each button and gesture.

1. Open **Settings → Keyboard & Keys**, enable **Show floating HUD controls**, and tap **Edit floating control layout**. Opening the editor also enables the switch.
2. The editor opens on a separate black canvas. Tap **ADD** to choose a control, drag it into position, and tap it to open its properties. Available properties depend on the control and include size, opacity, color, and actions; the panel also offers duplicate, delete, and position adjustments.
3. Toggle grid snapping with **SNAP**. Use **LAYOUT** to apply a preset, copy the other orientation's regular HUD layout, or clear the active layout. Presets and clearing replace the current layout, so check before applying them.
4. Tap **SAVE** to save the layout. If there are unsaved layout changes when you tap **EXIT** or Back, choose to save and exit, discard, or cancel.

Portrait and landscape layouts are saved separately. Rotate the device to edit each orientation, or use **LAYOUT** to copy from the other orientation. The HUD and keyboard layers also have separate layouts; check each orientation and layer.

#### Add and configure controls

- **Standard Key Button:** assign a key, modifier, system action, or a combination of up to three keys. Configure a main action and a swipe-up popup action; enable **Repeat while held** when needed.
- **Super Gesture Nub:** assign actions to different swipe directions.
- **TrackPoint, trackpad, scroll bar, and D-pad:** control the pointer, scrolling, or direction; options depend on the control.
- **Group container:** arrange several keys together and move them as a group. Add keys in its properties, choose **Move existing key in**, or drag a standalone key into it. Child keys can be detached or deleted in the properties panel.

On the desktop, tap or operate a control to send its configured action. Swipe up on a button with a popup action to trigger it. Tap a **KB** key assigned to the virtual keyboard to toggle the keyboard; hold it to hide the HUD and hold it again to restore it. The hidden state is remembered.

#### Keyboard layer

In the editor, tap **Edit keyboard** to adjust the floating virtual keyboard and its dedicated HUD keyboard layer. This layer starts empty and accepts the same controls as the regular HUD. It is used while the virtual keyboard is visible; hiding the keyboard restores the regular HUD layer. Tap **Back to HUD** to return to the regular layer in the editor.

Tap **Keyboard settings** on the keyboard layer to configure landscape split mode, handedness, appearance, and wing keys. In landscape split mode, drag the blank area near each half separately in the editor; its position is saved. Tap **HIDE** during use to dismiss the keyboard. The portrait floating keyboard can be moved using its drag handle.

#### Export, import, and immersive mode

Under **Settings → Keyboard & Keys**, choose **Export HUD layout…** and use the Android file picker to save a JSON file. If there is no saved layout yet, save one in the editor first. **Import HUD layout…** replaces the current HUD profile and enables HUD controls; export a backup first if needed.

Under **Settings → Immersive Mode → Immersive Input**, **Touch HUD controls in immersive mode** is enabled by default. When disabled, touches go directly to the desktop while the touchscreen is grabbed and the HUD is display-only. If the HUD is missing or unresponsive, check the display switch, KB hidden state, current orientation and layer, and this immersive-mode option.

<a id="immersive"></a>
## 🎮 Immersive Mode

<a id="enable-immersive"></a>
### Enable Immersive Mode

This is the master switch for immersive mode. There are two ways to enter:

- **Manual toggle:** bind the key below, then press it once in the Anland desktop to enter.
- **Automatic entry:** select **Gold keyboard directly to desktop** and enable **Automatically enter immersive mode**. Anland enters when it returns to the foreground.

While active, the selected input source is sent to the Linux desktop, reducing Android's interception of those input events. This is useful for games, full-screen applications, and other cases that need complete keyboard and mouse behavior.

- **Input sources:** Anland can take selected Android input devices, forward Gold's remapped keyboard, or combine the two.

- **Requirements and exit behavior:** working root access and at least one usable input source are required. Anland exits automatically and returns input to Android when you switch apps, the window loses focus, or the screen is locked or turned off.

- **How input is handled:** for a direct device source, the root helper takes exclusive control of selected devices and forwards their raw events. For a Gold source, Anland reads Gold's remapped keyboard output. Immersive mode does more than hide the Android system bars.

<a id="bind-immersive"></a>
### Bind Immersive Mode Key

To enter and leave manually, bind a dedicated physical key: press it once in the desktop to enter, then press it again to exit.

Use a volume key or another hardware key that reports a scan code; avoid Android on-screen navigation gestures. Gold keyboard auto-entry does not need a bound key.

If the status says the key reports no scan code, bind a different physical key. Android does not receive normal key events in immersive mode, so the helper recognizes the toggle by its raw scan code.

<a id="oplus-refresh-rate"></a>
### Keep OnePlus refresh rate fixed

This option is off by default and works only on some OnePlus devices.

When enabled, Anland asks the system to stop the low-refresh-rate policy triggered by a lack of input events during immersive input. Try it if the refresh rate drops during immersive mode; ignore it on other devices.

<a id="immersive-input"></a>
### Immersive input

Choose the input source for immersive mode here. Every source requires working root access.

Sources that use Gold also require the Gold Keyboard Remaps module to be installed and running. Those choices are disabled when the module is unavailable.

<a id="immersive-input-source"></a>
#### Input source

- **Anland takes the devices (direct):** Anland takes the selected Android input devices and forwards their events to Linux. Android cannot use them while they are taken.

- **Gold keyboard directly to desktop:** sends Gold Keyboard Remaps' keyboard input to the desktop. Touch, mouse, and Android gestures remain available.

- **Direct touch and pointers + Gold keyboard:** Anland takes the touchscreen and pointer devices; Gold's remapped keyboard input goes to the desktop.

<a id="immersive-device-selection"></a>
#### Device selection

When the selected source requires Anland to take input devices, turn on **Choose devices automatically** to let the app select usable devices. Turn it off to choose event nodes individually.

Devices held by Gold cannot be selected. Power-button devices stay with Android so you retain power-button control.

<a id="immersive-auto-enter"></a>
#### Automatically enter immersive mode

This option appears with **Gold keyboard directly to desktop**.

When enabled, the Gold keyboard is sent to the desktop automatically when Anland returns to the foreground. Touch input and Android gestures remain available.

The immersive mode master switch must still be enabled, and the Gold Keyboard Remaps module must be available.

<a id="touchpad-mouse"></a>
## 🖱️ Touchpad & Mouse

<a id="touchpad-mode"></a>
### Touchpad Mode (relative movement)

- **On:** sliding a finger moves the pointer like a laptop touchpad. Use one finger to move, tap for left click, two fingers to scroll or tap for right click, and long-press before moving to drag.

- **Off:** screen touches go to Linux as absolute-position multitouch input. Use this for touch-optimized apps, drawing, or direct positioning.

<a id="pointer-capture"></a>
### Capture external pointer

When enabled, an external mouse or physical touchpad uses relative movement and stays locked to the Anland desktop. Reaching a screen edge will not reveal the Android system bars.

This is useful for games and continuous pointer movement. Press Android Back to release capture, then click the desktop to capture again. Screen touch is unaffected.

Linux apps can still request pointer lock temporarily when this option is off. Normal behavior resumes when the app releases the lock.

<a id="pointer-sensitivity"></a>
### Touchpad Pointer Sensitivity (Acceleration)

The range is 0.5x to 10x. Higher values move the pointer farther during fast movement; lower values help with precise positioning.

This affects the on-screen touchpad, captured physical touchpads, and external mice in relative mode. It does not affect absolute touch.

Acceleration varies with movement speed: slow adjustments stay controlled, while fast movement accelerates more.

<a id="reverse-scroll"></a>
### Reverse scroll direction

When enabled, scrolling is "natural": pushing your fingers up moves the content up. When disabled, it follows the traditional mouse-wheel direction.

This applies to both the on-screen and captured physical touchpads.

<a id="disable-multifinger"></a>
### Disable pinch and multi-finger gestures

- **On:** ignore pinch/spread and gestures with three or more fingers. Two-finger scrolling and two-finger tap for right click continue to work.

  Use this if Linux mistakes gestures for zoom or window switching.

- **Off:** forward multi-finger gestures other than scrolling to Linux as multitouch input.

<a id="scroll-speed"></a>
### Two-finger scroll speed

Controls how much scrolling two-finger movement produces. Raise the value if scrolling feels too slow; lower it if it feels too fast.

<a id="scroll-threshold"></a>
### Scroll detection threshold

Controls how far two fingers must travel before scrolling starts. Lower it if pinches are mistaken for scrolling; raise it if slight jitter triggers scrolling.

<a id="gesture-threshold"></a>
### Gesture commit threshold

Controls when Anland classifies an ambiguous two-finger movement as scrolling or another gesture. Lower it if scrolling starts too late; raise it if gestures are often misclassified.

<a id="gesture-scale"></a>
### Gesture magnitude

When multi-finger gestures are enabled, Anland forwards pinch/spread and three-or-more-finger gestures as Linux multitouch input. They map to a square centered on the pointer.

This setting controls the square's side length. A larger value makes the same finger movement travel farther in Linux.

It does not affect one-finger pointer movement or two-finger scrolling.

<a id="connection"></a>
## 🔌 Connection

<a id="socket-path"></a>
### Daemon socket path

This is the Unix domain socket path the Android app uses to connect to the display daemon. The default is:

```text
/data/local/tmp/display_daemon.sock
```

You normally do not need to change it. Enter another path only if the desktop startup script uses a different location or you run multiple independent daemons.

This is a local filesystem path, not an IP address or network URL.

If the path is wrong, the daemon is not running, or the target is not a Unix socket, Anland cannot open the desktop. On first launch, the app may open Settings directly so you can correct the path.

<a id="second-window"></a>
### Open a second window (own daemon & title)

Use this section to start another Anland window, for example in Android freeform or split-screen mode, or to view two independent Linux desktop sessions at the same time.

- **Window name** is used only as the Android window title in Recents or freeform mode, for example `work`. It does not create or rename a Linux daemon.

- **Socket path** selects the daemon for the new window. To get a truly independent second desktop, enter the socket of another running daemon.

- **Open second window** launches it with the values currently entered. The name and socket apply only to this launch and do not replace the main window's saved connection path.

If the selected socket is already open, Anland switches to the existing window instead of connecting to the same daemon twice.

If the path does not exist, the new window reports the failure and closes.

<a id="root-connection"></a>
### Connect with root (helper)

- **On:** Anland uses `su` to launch a root helper, which connects to the daemon and passes the connection back to the app.

- **Use it when:** the socket is in a location a normal app cannot access, such as a root-owned directory. Grant permission in your root manager.

- **Off:** Anland can connect only to sockets the app itself has permission to access.

This does not change the permissions of the Linux desktop itself. Root is used only for the restricted Android-side connection. Changes take effect on the next connection.

<a id="foreground-scheduling"></a>
### Enable foreground scheduling (root)

When enabled, Anland puts related Linux desktop processes in Android's foreground scheduling group to raise their CPU priority. Try it if the desktop stutters under system load.

This feature requires working `su` and increases power use. It adjusts CPU scheduling; it cannot fix issues caused by the GPU, network, or an application itself.

Leave it off if the desktop is already smooth. Anland restores the normal scheduling scope when the connection ends.

> [!IMPORTANT]
>
> `CONFIG_UNIX_DIAG` locates the process at the other end of the Unix domain socket. `CONFIG_PROC_CHILDREN` lets Anland walk the desktop session's process tree.
>
> Without either option, foreground scheduling may not handle the target processes correctly. The basic display connection is unaffected.

<a id="foreground-scope"></a>
#### Foreground scheduling scope

- **1 - Focused application:** raises the compositor and the Linux application in use. When focus changes, the previous application returns to normal priority. This is suitable for everyday use and has a smaller impact.

- **2 - Whole desktop session:** raises the entire Anland Linux process tree, including background applications and services, for the duration of the connection. This prioritizes performance but uses more power and affects other system tasks more.

<a id="scan-stop-processes"></a>
#### Scan stop process names (optional)

This advanced option is for nonstandard container or session structures. It does **not** stop processes.

The names tell the root helper where to stop while walking up the Linux desktop's parent processes, so it can find the session boundary.

Separate names with colons, for example `systemd:init`.

Leave the field empty to use the built-in list. A custom value replaces that list. Most users should leave it empty; incorrect names can make the boosted scope too broad, too narrow, or ineffective.

<a id="microphone"></a>
### Forward microphone to desktop

When enabled, the Android microphone becomes a recording input for the Linux desktop, for meetings, recording, and voice chat. Android requests microphone permission the first time you enable it.

Turning this off stops only the phone-to-Linux microphone path. Desktop audio will still play through the phone or tablet. Leave it off when you are not recording to limit access and save power.

<a id="camera"></a>
### Forward camera to desktop

When enabled, the Linux desktop can use the Android cameras as video sources. Android requests camera permission the first time you enable it.

A camera opens only while a Linux application requests a video stream.

If multiple Anland windows request the same camera, Android opens one capture stream and shares it with those windows.

<a id="audio-keepalive"></a>
### Audio keep-alive

- **On:** keep the phone's audio output active so short Linux sounds, such as volume ticks and key clicks, play immediately. This uses slightly more standby power.
- **Off:** after about 1.5 seconds of desktop silence, the audio path sleeps to save power. It wakes for the next sound, but short sounds may have a slight start delay.

<a id="audio-latency"></a>
### Audio latency

The speaker and microphone have separate target-buffer settings:

| Option | Audio direction | Affects |
| --- | --- | --- |
| **Speaker (desktop → phone)** | Linux desktop to Android speaker/headphones | Playback latency for games, video, and system sounds |
| **Microphone (phone → desktop)** | Android microphone to Linux desktop | Input latency for calls, recording, and voice chat |

Presets are **Auto (engine default)** and approximately 1, 3, 5, 10, or 20 milliseconds.

Smaller buffers usually reduce latency but can crackle or drop audio when the device is busy. Larger buffers are more stable. These are buffer targets, not guaranteed end-to-end latency.

<a id="resolution"></a>
## 🖥️ Display Resolution

<a id="resolution-values"></a>
### Presets, width, and height

This page sets the Linux desktop's logical output resolution; it does not change the Android panel's physical resolution.

Lower resolutions usually reduce rendering, memory, and bandwidth load. Higher resolutions show more detail and desktop space at a higher cost.

- **Presets:** Auto (`0×0`), 4K, 2K, 1080p, 720p, 480p, and **Screen × 1.0 / 0.8 / 0.75 / 0.5 / 0.25**. Choosing a preset fills in the width and height. **Preset…** itself does not change the values.

- **Screen ×:** calculates concrete pixel dimensions from the panel's long and short sides in landscape. It is not a scale factor that follows later screen changes.

- **Custom:** enter two positive integers. Use `0×0` to return to the Anland view's automatic native size.

The resolution is sent to Linux on the next connection.

<a id="auto-stretch"></a>
### Auto-stretch display

- **On:** fill the Anland window. The image may stretch if the aspect ratios differ.

- **Off:** preserve the image's aspect ratio and center it with black bars. Anland maps touch and mouse coordinates automatically.

Black bars normally do not appear with automatic native resolution.

<a id="general"></a>
## ⚙️ General

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

When enabled, a persistent **Anland — Tap to open Settings** notification appears while the desktop window is in the foreground. Tap it to open Settings. Turning this off hides the notification.

Android 13 and later also require notification permission. The notification will not appear if its system channel has been disabled.

This notification is only a Settings shortcut. It does not keep the daemon or Linux desktop running in the background, and Anland removes it when you leave the app.
