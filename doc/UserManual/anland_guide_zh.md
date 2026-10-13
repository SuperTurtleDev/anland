<!--
title: Anland 使用指南
section: 指南
order: 3
desc: 在 Android 上部署 Anland，并连接 PRoot、chroot 或 Droidspaces 中支持 Anland 的 Wayland 桌面。
keywords: anland, wayland, android, proot, chroot, droidspaces, kwin, mutter, kde, gnome, gpu, adreno, snapdragon
-->

# Anland 使用指南

> [English](anland_guide.md)

Anland 是面向 Android 的 Wayland 显示方案，不是 Linux 容器管理器。

经过适配的 Linux 合成器会把画面渲染到共享 GPU 缓冲区。Android 端的 Anland 应用负责显示画面，并转发触摸、键盘、鼠标、剪贴板和音频等数据。

一条完整的 Anland 显示链路由三部分组成：

| 组件 | 运行位置 | 作用 |
| --- | --- | --- |
| Anland Android 应用（consumer） | Android | 分配并显示缓冲区，接收 Android 输入。 |
| `display_daemon` | Android 宿主 | 通过 Unix 域套接字连接 Android 应用与 Linux 合成器。 |
| 支持 Anland 的合成器（producer） | Linux 环境 | 使用 Anland 后端渲染 KDE Plasma、GNOME 等桌面。 |

PRoot、chroot 和 Droidspaces 是承载 Linux 用户空间的不同方式。无论选择哪一种，Android consumer 与 Linux producer 最终都要连接到同一个 `display_daemon`。

### 🧭 快速导航

- [🔀 选择运行方式](#runtime)
- [📋 前提条件](#requirements)
- [📱 第 1 步：安装 Android 端](#android-side)
- [🐧 第 2 步：接入 Linux 环境](#linux-environment)
  - [chroot](#chroot)
  - [Droidspaces](#droidspaces)
- [🧩 第 3 步：安装 Anland producer](#producer)
  - [从源码构建](#source-build)
  - [使用预编译包或 RootFS](#prebuilt)
- [🚀 第 4 步：启动桌面](#start-desktop)
- [🛠️ 常见问题](#troubleshooting)

---

<a id="runtime"></a>

## 🔀 选择运行方式

| 运行方式 | 是否需要 Root | 本指南如何处理 |
| --- | :---: | --- |
| PRoot / Termux 原生环境 | 否 | 使用独立的 Anland: Termux 项目，跳转到其外部指南。 |
| chroot | 是 | 本指南说明标准 Anland 应用、宿主守护进程、socket 映射和 producer 配置。 |
| Droidspaces | 是 | 使用 Droidspaces 内置的每容器 Anland daemon 和独立 **Anland Display** 开关，无需安装独立 daemon 模块或手动映射 socket。 |

### PRoot

PRoot 用户请阅读 [Anland: Termux 中文用户指南](https://github.com/lfdevs/anland-termux/blob/main/docs/user-guide_zh.md)。

该项目提供独立的 Android 应用、Termux 守护程序、PRoot 镜像和启动脚本，也支持无 Root 设备。

> [!IMPORTANT]
>
> Anland: Termux 与本页后续介绍的标准 Anland 路径使用不同的 APK、守护程序和 socket 布局。使用 PRoot 时请完整遵循外部指南，不要混用两套安装步骤。

本页余下内容只介绍标准 Anland 在 chroot 和 Droidspaces 中的部署。

---

<a id="requirements"></a>

## 📋 前提条件

开始前，逐项确认：

- **Android 11 或更高版本，并已取得 Root 权限。** chroot 使用独立 `display_daemon`；Droidspaces 由内置集成管理 daemon。应用通常仍需 Root 辅助程序连接 socket。

- **ARM64 设备与 Linux 环境。** 当前 APK、daemon 和 producer 构建面向 AArch64/ARM64。

- **支持 Anland 的合成器。** 普通发行版自带的 KWin 或 Mutter 不含 Anland 后端，需安装项目提供的补丁版本。

- **GPU 设备和驱动可用。** 推荐 Snapdragon / Adreno，并确保 Linux 环境能访问 `/dev/dri/renderD128` 及对应的 KGSL/Mesa 驱动。其他平台的支持取决于驱动，可能只能软件渲染或无法启动桌面。

- **组件版本兼容。** 协议更新时，Android 应用和 producer 可能需要一起更新。chroot 使用的独立 daemon 也应与其他组件版本接近。

> [!NOTE]
>
> Anland 只负责显示与输入等桥接能力，不负责创建 Linux 环境、安装完整桌面、配置 chroot 挂载或提供 GPU 驱动。

> [!IMPORTANT]
>
> 使用 Android 应用中的“设置 → 连接 → 启用前台调度（root）”时，设备内核必须启用：
>
> ```config
> CONFIG_UNIX_DIAG=y
> CONFIG_PROC_CHILDREN=y
> ```
>
> `CONFIG_UNIX_DIAG` 用于定位 Unix 域套接字对端进程；`CONFIG_PROC_CHILDREN` 用于遍历进程树。
>
> 这两项只供前台调度使用，不是基本显示链路的要求。

---

<a id="android-side"></a>

## 📱 第 1 步：安装 Android 端

chroot 和 Droidspaces 用户都需要先安装 [Anland 最新 Release](https://github.com/superturtlee/anland/releases/latest) 中的 `anland-v5.apk`，再按运行方式完成配置。

### chroot 使用的 Android 端

1. **下载并安装 daemon 模块。** 从最新成功的 [Build APK and Daemon](https://github.com/superturtlee/anland/actions/workflows/build.yml) 构建中下载 `anland-daemon`，再用支持 Magisk 模块的 Root 管理器安装 ZIP。

2. **重启并检查 socket。** 在 Root 终端运行：

   ```sh
   su -c 'test -S /data/local/tmp/display_daemon.sock && echo "Anland daemon is ready"'
   ```

3. **检查 Anland 设置。** 确认连接选项为：

   - **守护进程套接字路径**：保持默认的 `/data/local/tmp/display_daemon.sock`。
   - **使用 root 连接（辅助程序）**：保持开启，并在 Root 管理器中授予 Anland 权限。

> [!NOTE]
>
> 部分 Release 只提供 APK，不含 daemon 模块。此时从近期成功的 Actions 构建下载 `anland-daemon`；如果也使用构建 APK，优先从同一次构建下载两者。

### Droidspaces 使用的 Android 端

Droidspaces 的 [`anland` 分支](https://github.com/ravindu644/Droidspaces-OSS/tree/anland)已实现 Anland 内置集成，并计划合并到 `main`。

以下步骤适用于带独立 **Anland Display** 开关的构建；该功能合并后，也可使用包含它的 `main` 版本。

- **只安装 Anland APK。** 不需要单独安装 `anland-daemon`；Droidspaces 会为启用 Anland 的容器启动独立 daemon。

- **授予 Root 权限。** 不要手动填写 socket；**Launch Anland** 会把当前容器的动态路径传给应用。

- **从 Droidspaces 启动。** 不要点击 Anland 桌面图标，因为它使用全局默认 socket，而 Droidspaces 为每个容器使用独立 socket。

完整的应用设置说明见 [Anland 设置使用说明](anland_settings_guide_zh.md)。

---

<a id="linux-environment"></a>

## 🐧 第 2 步：接入 Linux 环境

无论使用哪种运行方式，Anland producer 都连接 Linux 环境中的同一路径：

```text
/run/display.sock
```

**chroot** 需要把 Android 宿主的默认 socket 映射到该路径：

```text
/data/local/tmp/display_daemon.sock -> /run/display.sock
```

Droidspaces 会在宿主的 `/data/local/tmp` 下为每个容器创建独立 socket（例如 `anland-<UUID>.sock`），并自动绑定到容器内的 `/run/display.sock`。

无需查看、填写或手动映射这个动态宿主路径。后续启动脚本统一通过 `ANLAND_SOCKET=/run/display.sock` 连接守护进程。

<a id="chroot"></a>

### chroot

开始前，请确认 chroot 已能正常进入，并已挂载 `/proc`、`/sys`、`/dev` 和 `/dev/pts`。

这些是 chroot 的基础挂载，Anland 不会替你配置。

1. **映射 daemon socket。** 在 Android Root 终端中，将 `CHROOT_ROOT` 改为实际 RootFS 路径，然后运行：

   ```sh
   CHROOT_ROOT=/data/local/chroot/debian

   test -S /data/local/tmp/display_daemon.sock || exit 1
   mkdir -p "$CHROOT_ROOT/run"
   touch "$CHROOT_ROOT/run/display.sock"
   mount --bind \
     /data/local/tmp/display_daemon.sock \
     "$CHROOT_ROOT/run/display.sock"
   ```

2. **提供 GPU 设备节点。** 确保 chroot 内可访问 GPU。Adreno 设备通常至少需要 `/dev/dri/renderD128` 和 `/dev/kgsl-3d0`；由 chroot 启动脚本处理具体挂载与权限。

3. **检查映射。** 进入 chroot 后确认 socket 和渲染节点存在：

   ```sh
   test -S /run/display.sock
   test -e /dev/dri/renderD128
   ```

4. **安装并启动 producer。** 按下一节安装 Anland producer，再以 Linux 普通用户启动桌面会话。

> [!NOTE]
>
> socket 是运行时文件。Android 重启或 daemon 重建 socket 后，chroot 启动脚本需要重新建立绑定挂载。

<a id="droidspaces"></a>

### Droidspaces

先安装 Anland APK，并使用包含独立 **Anland Display** 开关的 Droidspaces 构建：目前使用 `anland` 分支；功能合并后使用包含该开关的 `main` 版本。

然后编辑目标容器并完成以下设置：

1. **启用 GPU 访问。** 如果 RootFS 或设备明确要求完整硬件映射，再启用 **硬件访问**。

2. **启用 Anland Display。** 这是 Droidspaces 为该容器启动 Anland 的独立开关。

3. **关闭配置 Termux:X11。** Anland 和 Termux:X11 使用不同的显示链路；同时配置可能引起环境变量或服务冲突。

4. **启动容器并检查设备。** 确认 socket 和 GPU 节点已自动创建：

   ```sh
   test -S /run/display.sock
   test -e /dev/dri/renderD128
   ```

5. **打开 Anland 窗口。** 启动 producer 后，在运行中容器卡片或容器详情页点击 **Launch Anland**（部分版本显示为 `anland`）。Droidspaces 会传入该容器的动态 socket 路径并打开独立窗口。

> [!IMPORTANT]
>
> **Anland Display** 开关会启动 Droidspaces 内置的每容器 daemon，并自动将它绑定到容器内的 `/run/display.sock`。不要再添加 `/data/local/tmp/display_daemon.sock` 的手动绑定，也不要在 Anland 设置中把全局默认路径改成 Droidspaces 的动态 socket。

### Droidspaces 推荐设置

- **PulseAudio**

  关闭“配置 PulseAudio”。Anland producer 通过 PipeWire 接入 Anland 音频链路，不需要 Droidspaces 另外配置 PulseAudio。

  如果容器以前配置过 PulseAudio，也请删除已有的 `PULSE_SERVER` 环境变量。

- **SELinux**

  优先保持 enforcing。Droidspaces 的 Anland 集成已包含对应策略；只有确认 AVC 拒绝与 Anland 有关时，才临时使用宽容模式排障。

- **容器特权选项**

  按 RootFS 要求选择 `nocaps` 或 `noseccomp`。部分组合需要这些兼容设置，但它们不是 Anland 协议的要求。

- **Debian / Ubuntu 上的 KDE Plasma**

  确认内核启用了 User Namespace。若 Plasma 明显卡顿或操作延迟，再检查当前 Droidspaces 版本要求的 `noseccomp` 设置。

---

<a id="producer"></a>

## 🧩 第 3 步：安装 Anland producer

Linux 环境中的桌面必须使用带 Anland 后端的合成器。当前 Anland 源码树主要维护 KDE Plasma 的 KWin 后端和 GNOME 的 Mutter 后端。

<a id="source-build"></a>

### 从源码构建

这是 Anland 项目提供的安装方式，适合需要检查补丁、调试后端或自行构建软件包的用户。

1. **获取源码。** 在 chroot 或 Droidspaces 容器中运行：

   ```sh
   git clone https://github.com/superturtlee/anland.git
   cd anland
   ```

2. **选择构建目录。** 根据桌面和发行版选择对应目录：

   | 桌面 | 发行版 | 目录 |
   | --- | --- | --- |
   | KDE Plasma | Debian 13 | `producers/kde/Debian13_v5` |
   | KDE Plasma | Ubuntu 26.04 | `producers/kde/ubuntu2604_v5` |
   | KDE Plasma | Fedora 43 | `producers/kde/Fedora43_v5` |
   | KDE Plasma | Arch Linux | `producers/kde/Arch_v5` |
   | GNOME | Debian 13 | `producers/gnome/Debian13_v5` |
   | GNOME | Ubuntu 26.04 | `producers/gnome/Ubuntu2604_v5` |

3. **运行构建脚本。** 进入所选目录并执行 `build.sh`。例如 Debian 13 KDE：

   ```sh
   cd producers/kde/Debian13_v5
   ./build.sh
   ```

脚本会安装编译依赖、获取发行版源码、应用 Anland 补丁，并构建和安装软件包。

运行前请确认当前用户可以使用 `sudo`。构建 KWin、Mutter 和 Xwayland 需要较长时间，也需要足够的存储空间。

<a id="prebuilt"></a>

### 使用预编译包或 RootFS

如果不想在设备上编译，可以使用第三方维护的预编译包，或已集成 producer 的 RootFS。

[Droidspaces RootFS Desktop Builder](https://github.com/Goldzxcbug/Droidspaces-rootfs-Desktop-builder) 提供以下产物：

- **KDE 安装器**：支持 Debian 13、Ubuntu 26.04、Fedora 43/44 和 Arch Linux ARM64。

- **GNOME 安装器**：支持 Debian 13 和 Ubuntu 26.04 ARM64。

- **Droidspaces RootFS**：已集成 Anland producer，也可以通过 GitHub Actions 自行构建。

#### 直接使用已经打包好的 RootFS

这是最省事的方式：无需先准备 Linux 系统，也无需在设备上运行 `build.sh` 或 producer 安装器。

1. **下载 RootFS。** 打开项目的 [Releases](https://github.com/Goldzxcbug/Droidspaces-rootfs-Desktop-builder/releases)，选择带 `Wayland` 标记的 `.tar.xz` 文件，以及需要的 KDE、KDE Mobile 或 GNOME 桌面。

2. **导入 Droidspaces。** 将下载的 RootFS 导入应用。

3. **配置容器。** 按本指南的 [Droidspaces 配置](#droidspaces)启用 GPU 访问和 **Anland Display**。daemon 与 `/run/display.sock` 映射由 Droidspaces 自动管理。

4. **启动桌面。** 启动容器后直接进入[第 4 步：启动桌面](#start-desktop)。RootFS 已包含补丁版合成器、Anland 环境配置和桌面启动命令，无需重复安装 producer。

文件名类似：

```text
Ubuntu-26-kde-Wayland-Droidspaces-rootfs-aarch64-v20260702-120000.tar.xz
```

#### 在现有 Linux 环境中安装预编译包

在现有 Linux 环境中可以单独运行其安装器。先拉取项目：

```sh
git clone https://github.com/Goldzxcbug/Droidspaces-rootfs-Desktop-builder.git
cd Droidspaces-rootfs-Desktop-builder
```

KDE Plasma 用户运行：

```sh
sudo ./scripts/install-anland-kde.sh
```

GNOME 用户改为运行：

```sh
sudo ./scripts/install-anland-gnome.sh
```

这些安装器和 RootFS 由外部项目维护，不属于 Anland Release。使用前请核对发行版、版本和架构是否在其支持范围内。

---

<a id="start-desktop"></a>

## 🚀 第 4 步：启动桌面

**从源码构建的 producer：** 在所选 producer 目录中运行 `startup.sh`。脚本会设置 Anland 所需的主要环境变量；默认 socket 的启动命令如下：

```sh
./startup.sh /run/display.sock
```

脚本通常会设置以下变量：

```text
ANLAND=1
ANLAND_SOCKET=/run/display.sock
ANLAND_DRM_DEVICE=/dev/dri/renderD128
```

**使用预构建 RootFS：** 优先运行镜像提供的桌面启动命令。例如，Droidspaces RootFS Desktop Builder 的 KDE 环境可运行：

```sh
startplasma-wayland
```

producer 启动后，再打开 Android 端窗口：

- **chroot：** 打开或切回 Android 的 Anland 应用。

- **Droidspaces：** 在运行中容器卡片或容器详情页点击 **Launch Anland**。不要直接点击 Anland 桌面图标；只有 Droidspaces 操作会传入该容器的动态 socket 路径。

consumer 与 producer 连接成功后，桌面画面会自动出现。

> [!IMPORTANT]
>
> 不要用发行版自带、未经 Anland 补丁处理的 `kwin_wayland` 或 Mutter 启动会话。普通 Wayland 合成器即使成功启动，也不会向 Anland 提交画面。

---

<a id="troubleshooting"></a>

## 🛠️ 常见问题

### Anland 只打开设置页，或提示无法连接

**chroot：** 在 Android Root 终端检查默认宿主 socket：

```sh
su -c 'test -S /data/local/tmp/display_daemon.sock'
```

如果 socket 不存在，请检查 `anland-daemon` 模块是否启用，并确认设备已重启。

如果 socket 存在，请确认：

- Anland 设置中的 socket 路径正确。
- **使用 root 连接**已开启，且 Root 管理器没有拒绝授权。

**Droidspaces：** 不要检查或填写全局默认 socket。请逐项确认：

1. Droidspaces 使用带独立 **Anland Display** 开关的 `anland` 分支构建，或已合并该功能的 `main` 版本。

2. 目标容器已启用 **Anland Display**，并且正在运行。

3. 容器内存在 `/run/display.sock`。

4. 通过 Droidspaces 的 **Launch Anland** 或 `anland` 按钮打开窗口，而不是点击 Anland 桌面图标。

如果运行中容器没有 Anland 启动按钮，通常是当前 Droidspaces 版本尚未包含集成，或该容器的 daemon/socket 未能启动。

### 应用已连接，但没有桌面画面

在 Linux 环境中检查：

```sh
test -S /run/display.sock
printf '%s\n' "$ANLAND_SOCKET"
```

确认启动的是经过 Anland 补丁处理的 KWin 或 Mutter，并查看 producer 的终端日志。

常见原因包括：

- socket 没有映射进 Linux 环境。
- `ANLAND_SOCKET` 指向错误。
- 系统更新覆盖了补丁版软件包。

### 桌面启动失败或只能软件渲染

先确认 Linux 环境可以访问 `/dev/dri/renderD128`。Adreno 设备还需要可用的 KGSL 节点和匹配的 Mesa 驱动。

- **Droidspaces：** 复查 GPU/硬件访问设置。
- **chroot：** 检查设备节点挂载和用户权限。

### KDE Plasma 明显卡顿

按以下顺序检查：

1. Debian 或 Ubuntu：确认内核启用了 User Namespace。
2. Droidspaces：检查当前版本要求的 `noseccomp` 设置。
3. 如果怀疑 SELinux 拦截，先检查 AVC 日志并修复对应权限；不要长期保持 permissive 模式。

### 桌面正常，但应用没有声音

legacy 分支的 Anland producer 会创建 `anland-speaker` PipeWire 输出设备。WirePlumber 将应用播放流连接到该设备，再由 Android 应用播放收到的音频。请用启动桌面的 Linux 用户检查：

```sh
systemctl --user status pipewire.service wireplumber.service
wpctl status
```

确认两个服务均在运行、`anland-speaker` 已出现，且播放应用正在使用它。也请检查 Android 媒体音量和 Anland 的音频播放设置。若已有 `PULSE_SERVER` 指向其他音频服务器，应用可能会绕过 Anland 播放。

部分高通 Android 内核的 `/dev/video*` 包含名为 `cam_req_mgr` 或 `cam_sync` 的摄像头控制设备，以及 VIDC 编解码器设备，它们不是 Linux 摄像头。将宿主设备映射进 chroot 或 Droidspaces 容器后，WirePlumber 的 V4L2 监视器可能在探测这些设备时卡住，使音频策略无法继续连接应用播放流。此时即使能看到 `anland-speaker`，应用仍可能没有声音。

Anland 共享的 [Android V4L2 过滤配置](../../libdisplay_producer/wireplumber/50-anland-android-camera.conf) 在摄像头初始化前，根据 `device.sysfs.path` 匹配这些控制和编解码器设备。它不依赖重启后可能变化的 `/dev/videoN` 编号。使用同一套 PipeWire/WirePlumber 服务的 KDE、GNOME、Niri 等 producer 均适用。USB 摄像头和 Anland 直接在 PipeWire 中创建的远程摄像头来源仍可使用。

此配置使用 WirePlumber 0.5 语法。**未修补的 WirePlumber 0.5.16 和 0.5.17 必须升级，或使用发行版回移了[提交 65519a1](https://github.com/PipeWire/wireplumber/commit/65519a1ff7cecebc0622fc4a2badec830ee67d7d)的版本**：这些版本的异步 V4L2 设备 hook 在 `device.disabled` 跳过设备时没有继续推进，因此仅安装过滤配置仍可能卡住音频策略。建议使用 0.5.18 或更高版本。0.5.8、0.5.12、0.5.15 等较早的 0.5 版本使用同步 hook，没有这一特定回归，不需要仅为此问题升级。

本修复仅改变 Anland 仓库中的配置、producer 打包流程和文档；使用发行版官方 PipeWire 和 WirePlumber，不需要编译或替换它们的库。手动安装过滤配置无需编译；重新发布 producer 软件包时按原有脚本打包。Anland 已有的 PipeWire 自动重连逻辑保持不变。

包含此修复的 producer 软件包会安装过滤配置，并通过依赖约束 WirePlumber 版本。Arch 和 SteamOS 软件包要求 0.5.18 或更高版本；Debian、Ubuntu 和 Fedora 软件包允许较早的 0.5 版本，同时排除 0.5.16 和 0.5.17。若发行版回移修复后仍保留被排除的版本号，维护者需要在确认修复后调整软件包依赖。

为现有安装手动应用过滤配置时，请先确认上述 WirePlumber 版本要求，再在 Linux 环境内，从 Anland 仓库根目录运行：

```sh
sudo install -Dm644 \
  libdisplay_producer/wireplumber/50-anland-android-camera.conf \
  /usr/share/wireplumber/wireplumber.conf.d/50-anland-android-camera.conf
```

如果以前添加过设置 `hardware.video-capture = disabled` 的配置片段，请从自己创建的文件中移除这项设置，或将该配置片段移出配置目录。检查 `/etc/wireplumber/wireplumber.conf.d/` 和 `~/.config/wireplumber/wireplumber.conf.d/` 两处，保留其他无关设置和文件。关闭整个功能也会停止发现真正的 V4L2 摄像头，设备过滤配置用于替代这种全面禁用的临时方案。

然后以桌面用户身份重启 WirePlumber，不要加 `sudo`；必要时重新打开播放应用：

```sh
systemctl --user restart wireplumber.service
wpctl status
```

确认应用已连接到 `anland-speaker`，且 Android 实际播放了音频。过滤配置可避免这种控制设备探测故障；Android 音频路由、音量和 consumer 断连仍可能造成其他故障。能看到摄像头来源不等于已验证视频捕获：若需要使用摄像头，请先授予 Anland Android 摄像头权限，再用应用测试实际捕获。接入 USB 摄像头时，也请单独测试其视频捕获。

### 前台调度没有生效

逐项确认：

- Android 设置中的“连接 → 启用前台调度（root）”已开启。
- Root 管理器已向 Anland 授权。
- 设备内核启用了 `CONFIG_UNIX_DIAG=y` 和 `CONFIG_PROC_CHILDREN=y`。

缺少任一内核选项时，Anland 可能无法定位或遍历 Linux 桌面进程。调整内核配置后，请重新建立桌面连接再检查效果。

### 需要调整输入、分辨率、音频或窗口行为

参阅 [Anland 设置使用说明](anland_settings_guide_zh.md)。设置修改后通常会立即保存，但连接、音频和输出分辨率等项目需要在下一次连接时生效。
