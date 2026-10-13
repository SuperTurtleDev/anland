<!--
title: Anland User Guide
section: Guides
order: 3
desc: Deploy Anland on Android and connect to an Anland-enabled Wayland desktop in PRoot, chroot, or Droidspaces.
keywords: anland, wayland, android, proot, chroot, droidspaces, kwin, mutter, kde, gnome, gpu, adreno, snapdragon
-->

# Anland User Guide

> [中文版](anland_guide_zh.md)

Anland is a Wayland display solution for Android, not a Linux container manager.

An adapted Linux compositor renders into shared GPU buffers. The Anland Android app displays those buffers and forwards touch, keyboard, mouse, clipboard, audio, and other data.

A complete Anland display path has three components:

| Component | Runs on | Purpose |
| --- | --- | --- |
| Anland Android app (consumer) | Android | Allocates and displays buffers and receives Android input. |
| `display_daemon` | Android host | Connects the Android app and Linux compositor through a Unix domain socket. |
| Anland-enabled compositor (producer) | Linux environment | Uses the Anland backend to render desktops such as KDE Plasma and GNOME. |

PRoot, chroot, and Droidspaces are different ways to host a Linux user space. Whichever one you choose, the consumer and producer must ultimately connect to the same `display_daemon`.

### 🧭 Quick navigation

- [🔀 Choose a runtime](#runtime)
- [📋 Requirements](#requirements)
- [📱 Step 1: Install the Android side](#android-side)
- [🐧 Step 2: Connect the Linux environment](#linux-environment)
  - [chroot](#chroot)
  - [Droidspaces](#droidspaces)
- [🧩 Step 3: Install the Anland producer](#producer)
  - [Build from source](#source-build)
  - [Use prebuilt packages or a RootFS](#prebuilt)
- [🚀 Step 4: Start the desktop](#start-desktop)
- [🛠️ Troubleshooting](#troubleshooting)

---

<a id="runtime"></a>

## 🔀 Choose a runtime

| Runtime | Root required | Covered here |
| --- | :---: | --- |
| PRoot / native Termux environment | No | Uses the separate Anland: Termux project and its external guide. |
| chroot | Yes | This guide covers the standard Anland app, host daemon, socket mapping, and producer setup. |
| Droidspaces | Yes | Uses the built-in per-container Anland daemon and separate **Anland Display** switch, with no standalone daemon module or manual socket mapping. |

### PRoot

PRoot users should follow the [Anland: Termux User Guide](https://github.com/lfdevs/anland-termux/blob/main/docs/user-guide.md).

That project provides its own Android app, Termux daemon, PRoot image, and startup scripts. It also supports devices without root.

> [!IMPORTANT]
>
> Anland: Termux uses a different APK, daemon, and socket layout from the standard Anland path described below.
>
> Follow its external guide from start to finish for PRoot; do not mix the two installation procedures.

The rest of this page covers standard Anland deployments in chroot and Droidspaces only.

---

<a id="requirements"></a>

## 📋 Requirements

Before you begin, confirm the following:

- **Android 11 or later with root access.** chroot runs a standalone `display_daemon`; Droidspaces manages it through built-in integration. The Anland app normally still uses its root helper to connect to the socket.

- **An ARM64 device and Linux environment.** The current APK, daemon, and producer builds target AArch64/ARM64.

- **An Anland-enabled compositor.** Stock KWin and Mutter packages do not include the Anland backend. Install a patched version supplied by the project.

- **Working GPU devices and drivers.** Snapdragon/Adreno devices are recommended. Linux must expose `/dev/dri/renderD128` and the corresponding KGSL/Mesa drivers. Other platforms may be limited to software rendering or may not start the desktop, depending on driver support.

- **Compatible component versions.** A protocol update may require updating the Android app and producer together. The standalone chroot daemon should also be from a similar release period; avoid mixing components from widely separated versions.

> [!NOTE]
>
> Anland only bridges display, input, and related data. It does not create the Linux environment, install a complete desktop, configure chroot mounts, or provide GPU drivers.

> [!IMPORTANT]
>
> To use **Settings > Connection > Enable foreground scheduling (root)** in the Android app, the device kernel must enable:
>
> ```config
> CONFIG_UNIX_DIAG=y
> CONFIG_PROC_CHILDREN=y
> ```
>
> `CONFIG_UNIX_DIAG` lets Anland locate the Linux-side process through the Unix domain socket. `CONFIG_PROC_CHILDREN` lets it walk the process tree.
>
> These options are used only for foreground scheduling. They are not required for Anland's basic display path.

---

<a id="android-side"></a>

## 📱 Step 1: Install the Android side

Both chroot and Droidspaces users need the `anland-v5.apk` from the [latest Anland release](https://github.com/superturtlee/anland/releases/latest).

Install the APK, then complete the setup for your chosen runtime.

### Android side for chroot

1. **Download and install the daemon module.** Download `anland-daemon` from the latest successful [Build APK and Daemon](https://github.com/superturtlee/anland/actions/workflows/build.yml) run. Install the ZIP with a root manager that supports the Magisk module format.

2. **Restart and check the socket.** From a root shell, confirm that the default socket was created:

   ```sh
   su -c 'test -S /data/local/tmp/display_daemon.sock && echo "Anland daemon is ready"'
   ```

3. **Check Anland Settings.** Confirm these connection options:

   - **Daemon socket path**: keep the default `/data/local/tmp/display_daemon.sock`.
   - **Connect with root (helper)**: keep this enabled and grant Anland permission in your root manager.

> [!NOTE]
>
> Some releases include only the APK. In that case, download `anland-daemon` from a recent successful Actions build.
>
> If you also use a build APK, prefer getting `anland-v5` and `anland-daemon` from the same run.

### Android side for Droidspaces

The Droidspaces [`anland` branch](https://github.com/ravindu644/Droidspaces-OSS/tree/anland) includes built-in Anland integration and is planned for merging into `main`.

These instructions apply to builds with a separate **Anland Display** switch. After the feature is merged, use a `main` build that includes it.

- **Install the Anland APK only.** Do not install the separate `anland-daemon` module. Droidspaces starts an independent daemon for each container with Anland enabled.

- **Grant root access when asked.** Do not enter the container socket manually; **Launch Anland** passes its dynamic path to the app.

- **Open Anland from Droidspaces.** The Anland launcher icon uses the global default socket, while Droidspaces uses a separate socket for each container.

For a complete description of the app settings, see the [Anland Settings Guide](anland_settings_guide.md).

---

<a id="linux-environment"></a>

## 🐧 Step 2: Connect the Linux environment

Regardless of the runtime, the Anland producer connects to the same path inside the Linux environment:

```text
/run/display.sock
```

**chroot** maps the Android host's default socket to that path:

```text
/data/local/tmp/display_daemon.sock -> /run/display.sock
```

Droidspaces creates a separate socket for each container under the host's `/data/local/tmp` directory (for example, `anland-<UUID>.sock`) and automatically bind-mounts it at `/run/display.sock` inside the container.

You do not need to inspect, enter, or manually map this dynamic host path. The startup scripts below connect to the daemon through `ANLAND_SOCKET=/run/display.sock`.

<a id="chroot"></a>

### chroot

Before you begin, confirm that you can enter the chroot and that it mounts `/proc`, `/sys`, `/dev`, and `/dev/pts`.

These are basic chroot mounts; Anland does not configure them for you.

1. **Map the daemon socket.** In an Android root shell, change `CHROOT_ROOT` to the actual RootFS path, then run:

   ```sh
   CHROOT_ROOT=/data/local/chroot/debian

   test -S /data/local/tmp/display_daemon.sock || exit 1
   mkdir -p "$CHROOT_ROOT/run"
   touch "$CHROOT_ROOT/run/display.sock"
   mount --bind \
     /data/local/tmp/display_daemon.sock \
     "$CHROOT_ROOT/run/display.sock"
   ```

2. **Provide GPU device nodes.** Make sure the GPU is accessible inside the chroot. Adreno devices normally require at least `/dev/dri/renderD128` and `/dev/kgsl-3d0`; your chroot startup script should handle the exact mounts and permissions.

3. **Verify the mapping.** Enter the chroot and confirm that the socket and render node exist:

   ```sh
   test -S /run/display.sock
   test -e /dev/dri/renderD128
   ```

4. **Install and start the producer.** Follow the next section to install the Anland producer, then start the desktop session as a normal Linux user.

> [!NOTE]
>
> The socket is a runtime file. After Android restarts or the daemon recreates it, the chroot startup script must recreate the bind mount.

<a id="droidspaces"></a>

### Droidspaces

Install the Anland APK and use a Droidspaces build that includes the separate **Anland Display** switch. For now, use the `anland` branch; after the feature is merged, use a `main` build that includes the switch.

Then edit the target container and apply these settings:

1. **Enable GPU Access.** Also enable **Hardware Access** if your RootFS or device explicitly requires full hardware mapping.

2. **Enable Anland Display.** This is the per-container switch that starts Anland in Droidspaces.

3. **Disable Configure Termux:X11.** Anland and Termux:X11 use separate display paths; configuring both can cause environment-variable or service conflicts.

4. **Start the container and check the devices.** Confirm that the socket and GPU node were created automatically:

   ```sh
   test -S /run/display.sock
   test -e /dev/dri/renderD128
   ```

5. **Open the Anland window.** After starting the producer, select **Launch Anland** on the running-container card or container details page. Some versions label this button `anland`. Droidspaces passes the container's dynamic socket path to Anland and opens a separate window.

> [!IMPORTANT]
>
> The **Anland Display** switch starts Droidspaces' built-in per-container daemon and automatically bind-mounts it at `/run/display.sock` inside the container. Do not add a manual mapping for `/data/local/tmp/display_daemon.sock`, and do not change Anland's global default path to a Droidspaces dynamic socket.

### Recommended Droidspaces settings

- **PulseAudio**

  Keep **Configure PulseAudio** disabled. The Anland producer connects through PipeWire to Anland's audio path, so Droidspaces does not need to configure PulseAudio separately.

  If the container used PulseAudio before, remove the existing `PULSE_SERVER` environment variable too.

- **SELinux**

  Prefer to keep SELinux enforcing. Droidspaces' Anland integration includes the corresponding policy. Use permissive mode temporarily only after confirming that an AVC denial is related to Anland.

- **Container privilege options**

  Follow the RootFS requirements for `nocaps` or `noseccomp`. Some combinations need these compatibility settings, but they are not requirements of the Anland protocol.

- **KDE Plasma on Debian/Ubuntu**

  Confirm that the kernel has User Namespace support enabled. If Plasma is noticeably sluggish or input is delayed, check the `noseccomp` setting required by your Droidspaces version.

---

<a id="producer"></a>

## 🧩 Step 3: Install the Anland producer

The desktop in the Linux environment must use a compositor with an Anland backend. The current Anland source tree primarily maintains a KWin backend for KDE Plasma and a Mutter backend for GNOME.

<a id="source-build"></a>

### Build from source

This installation method is provided by the Anland project. Use it when you need to inspect patches, debug a backend, or build the packages yourself.

1. **Get the source.** Run these commands inside the chroot or Droidspaces container:

   ```sh
   git clone https://github.com/superturtlee/anland.git
   cd anland
   ```

2. **Choose a build directory.** Select the directory for your desktop and distribution:

   | Desktop | Distribution | Directory |
   | --- | --- | --- |
   | KDE Plasma | Debian 13 | `producers/kde/Debian13_v5` |
   | KDE Plasma | Ubuntu 26.04 | `producers/kde/ubuntu2604_v5` |
   | KDE Plasma | Fedora 43 | `producers/kde/Fedora43_v5` |
   | KDE Plasma | Arch Linux | `producers/kde/Arch_v5` |
   | GNOME | Debian 13 | `producers/gnome/Debian13_v5` |
   | GNOME | Ubuntu 26.04 | `producers/gnome/Ubuntu2604_v5` |

3. **Run the build script.** Enter the selected directory and run `build.sh`. For KDE on Debian 13:

   ```sh
   cd producers/kde/Debian13_v5
   ./build.sh
   ```

The script installs build dependencies, obtains the distribution sources, applies the Anland patches, and builds and installs the packages.

Make sure the current user can use `sudo` before running it. Building KWin, Mutter, and Xwayland takes considerable time and storage space.

<a id="prebuilt"></a>

### Use prebuilt packages or a RootFS

If you do not want to compile on the device, use third-party prebuilt packages or a RootFS with the producer already integrated.

[Droidspaces RootFS Desktop Builder](https://github.com/Goldzxcbug/Droidspaces-rootfs-Desktop-builder) provides:

- **KDE installers:** Debian 13, Ubuntu 26.04, Fedora 43/44, and Arch Linux ARM64.

- **GNOME installers:** Debian 13 and Ubuntu 26.04 ARM64.

- **Droidspaces RootFS images:** include the Anland producer and can also be built through GitHub Actions.

#### Use a packaged RootFS directly

This is the simplest option: you do not need to prepare a Linux system or run `build.sh` or a producer installer on the device.

1. **Download a RootFS.** Open the project's [Releases](https://github.com/Goldzxcbug/Droidspaces-rootfs-Desktop-builder/releases) and choose a `.tar.xz` image marked `Wayland`, with KDE, KDE Mobile, or GNOME as needed.

2. **Import it into Droidspaces.** Select the downloaded RootFS in the app.

3. **Configure the container.** Follow [Droidspaces configuration](#droidspaces) to enable GPU access and **Anland Display**. Droidspaces manages the daemon and `/run/display.sock` mapping automatically.

4. **Start the desktop.** Start the container, then continue to [Step 4: Start the desktop](#start-desktop). The RootFS already includes the patched compositor, Anland environment configuration, and desktop startup command, so you do not need to install the producer again.

An example filename is:

```text
Ubuntu-26-kde-Wayland-Droidspaces-rootfs-aarch64-v20260702-120000.tar.xz
```

#### Install prebuilt packages in an existing Linux environment

You can run the project's installers separately in an existing Linux environment. First clone the project:

```sh
git clone https://github.com/Goldzxcbug/Droidspaces-rootfs-Desktop-builder.git
cd Droidspaces-rootfs-Desktop-builder
```

KDE Plasma users should run:

```sh
sudo ./scripts/install-anland-kde.sh
```

GNOME users should run:

```sh
sudo ./scripts/install-anland-gnome.sh
```

These installers and RootFS images are maintained by an external project; they are not part of an Anland release. Check that your distribution, version, and architecture are in that project's support matrix before using them.

---

<a id="start-desktop"></a>

## 🚀 Step 4: Start the desktop

**For a producer built from source:** run `startup.sh` from the selected producer directory. It sets the main Anland environment variables. To use the default socket, run:

```sh
./startup.sh /run/display.sock
```

The script normally sets these variables:

```text
ANLAND=1
ANLAND_SOCKET=/run/display.sock
ANLAND_DRM_DEVICE=/dev/dri/renderD128
```

**For a prebuilt RootFS:** use the desktop startup command it provides. For example, start the KDE environment from Droidspaces RootFS Desktop Builder with:

```sh
startplasma-wayland
```

After the producer starts, open the Android-side window:

- **chroot:** open or switch back to the Anland Android app.

- **Droidspaces:** select **Launch Anland** on the running-container card or container details page. Do not open the Anland launcher icon; only the Droidspaces action passes the container's dynamic socket path to Anland.

The desktop appears automatically once the consumer and producer are connected.

> [!IMPORTANT]
>
> Do not start the session with your distribution's unpatched `kwin_wayland` or Mutter. A normal Wayland compositor will not submit frames to Anland even if it starts successfully.

---

<a id="troubleshooting"></a>

## 🛠️ Troubleshooting

### Anland opens only Settings or reports that it cannot connect

**chroot:** check the default host socket from an Android root shell:

```sh
su -c 'test -S /data/local/tmp/display_daemon.sock'
```

If the socket does not exist, check that the `anland-daemon` module is enabled and restart the device if needed.

If it exists, confirm that:

- The socket path in Anland Settings is correct.
- **Connect with root (helper)** is enabled and root permission has not been denied.

**Droidspaces:** do not check or enter the global default socket. Confirm each item:

1. The Droidspaces build includes the separate **Anland Display** switch, from the `anland` branch or a `main` build with the feature merged.

2. **Anland Display** is enabled for the target container, and the container is running.

3. `/run/display.sock` exists inside the container.

4. You opened the window with Droidspaces' **Launch Anland** action or `anland` button, not the Anland launcher icon.

If a running container has no Anland launch button, that Droidspaces version may not include the integration, or the container's daemon/socket may not have started.

### The app connects but no desktop appears

Check inside the Linux environment:

```sh
test -S /run/display.sock
printf '%s\n' "$ANLAND_SOCKET"
```

Confirm that you started an Anland-patched KWin or Mutter, then inspect the producer's terminal output.

Common causes include:

- The socket was not mapped into the Linux environment.
- `ANLAND_SOCKET` points to the wrong path.
- A system update replaced the patched packages.

### The desktop fails to start or uses only software rendering

First confirm that the Linux environment can access `/dev/dri/renderD128`. Adreno devices also need an available KGSL node and matching Mesa drivers.

- **Droidspaces:** recheck GPU/Hardware Access settings.
- **chroot:** check device-node mounts and user permissions.

### KDE Plasma is noticeably sluggish

Check these items in order:

1. On Debian or Ubuntu, confirm that User Namespace support is enabled in the kernel.
2. With Droidspaces, check the `noseccomp` setting required by your version.
3. If SELinux may be blocking access, inspect AVC logs and fix the specific permission. Do not leave SELinux in permissive mode.

### The desktop works but applications have no sound

The legacy Anland producer creates an `anland-speaker` PipeWire sink. WirePlumber connects application playback streams to that sink, and the Android app plays the received audio. Run these checks as the Linux user who started the desktop:

```sh
systemctl --user status pipewire.service wireplumber.service
wpctl status
```

Confirm that both services are running, `anland-speaker` is present, and the playback application is using it. Also check the Android media volume and Anland's playback setting. An existing `PULSE_SERVER` pointing to a different audio server can route applications away from Anland.

On some Qualcomm Android kernels, `/dev/video*` includes camera control devices named `cam_req_mgr` or `cam_sync`, and VIDC codec devices. These are not Linux webcams. When the host devices are exposed inside a chroot or Droidspaces container, WirePlumber's V4L2 monitor can stall while probing them. Its audio policy then stops connecting application streams, even though `anland-speaker` is visible.

Anland's shared [Android V4L2 filter](../../libdisplay_producer/wireplumber/50-anland-android-camera.conf) matches the control and codec devices by `device.sysfs.path` before camera initialization. It does not depend on their `/dev/videoN` numbers, which can change after a reboot. The filter applies to KDE, GNOME, Niri, and other producers using the same PipeWire/WirePlumber services. It leaves USB webcams and the remote camera sources created by Anland directly in PipeWire available.

The configuration uses WirePlumber 0.5 syntax. **Unpatched WirePlumber 0.5.16 and 0.5.17 require an upgrade or a distribution backport of [commit 65519a1](https://github.com/PipeWire/wireplumber/commit/65519a1ff7cecebc0622fc4a2badec830ee67d7d)**: their asynchronous V4L2 device hook does not advance when `device.disabled` skips a device, so installing the filter alone can still block audio policy. Prefer 0.5.18 or later. Older 0.5 releases such as 0.5.8, 0.5.12, and 0.5.15 use a synchronous hook and do not have this particular regression; they do not need an upgrade solely for this issue.

This fix changes only configuration, producer packaging, and documentation in the Anland repository. It uses official distribution PipeWire and WirePlumber libraries; neither library needs to be compiled or replaced. Installing the filter manually requires no compilation. New producer releases use the existing packaging scripts. Anland's existing PipeWire reconnection code is unchanged.

Producer packages built with this fix include the filter and enforce a WirePlumber version dependency. Arch and SteamOS packages require 0.5.18 or later. Debian, Ubuntu, and Fedora packages permit the earlier 0.5 releases while excluding 0.5.16 and 0.5.17. A distribution backport that retains an excluded version number still needs its package dependency adjusted by the maintainer after confirming the fix.

To apply the filter manually to an existing installation, first confirm the WirePlumber version requirements above, then run the following from the Anland repository root inside the Linux environment:

```sh
sudo install -Dm644 \
  libdisplay_producer/wireplumber/50-anland-android-camera.conf \
  /usr/share/wireplumber/wireplumber.conf.d/50-anland-android-camera.conf
```

If you previously added a drop-in that sets `hardware.video-capture = disabled`, remove that setting from the file you created, or move that specific drop-in outside the configuration directory. Check both `/etc/wireplumber/wireplumber.conf.d/` and `~/.config/wireplumber/wireplumber.conf.d/`. Keep unrelated settings and files. Disabling the whole feature also prevents real V4L2 webcams from being discovered; the device filter replaces that broad workaround.

Then restart WirePlumber as the desktop user, without `sudo`, and reopen the playback application if necessary:

```sh
systemctl --user restart wireplumber.service
wpctl status
```

Confirm that the application connects to `anland-speaker` and that Android plays its audio. The filter prevents this control-device discovery failure; Android audio routing, volume, and consumer disconnections can still cause separate faults. Camera sources remaining visible does not verify video capture: grant Anland Android camera permission and test capture with an application if you use that feature. Test USB webcam capture separately when a webcam is attached.

### Foreground scheduling has no effect

Check each requirement:

- **Enable foreground scheduling (root)** is on under **Settings > Connection**.
- Your root manager has granted Anland permission.
- The device kernel enables `CONFIG_UNIX_DIAG=y` and `CONFIG_PROC_CHILDREN=y`.

Without either kernel option, Anland may not locate or walk the Linux desktop processes. After changing the kernel configuration, establish a new desktop connection and check again.

### You need to adjust input, resolution, audio, or window behavior

See the [Anland Settings Guide](anland_settings_guide.md). Settings are normally saved immediately, but connection, audio, and output-resolution options take effect on the next connection.
