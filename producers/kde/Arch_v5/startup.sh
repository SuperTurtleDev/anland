#!/bin/bash
#
# Start Plasma with one KWin using the built-in Anland backend
# (Arch Linux / Arch Linux ARM, Adreno kgsl stack).
#
# Usage: ./startup.sh [socket path] (default /run/display.sock)
# START_PLASMA=0 starts only KWin; KWIN_BIN applies to that mode.
# ANLAND_SOCKET, ANLAND_DRM_DEVICE and XWAYLAND_GBM_DEVICE are configurable.
set -euo pipefail

SOCK="${1:-${ANLAND_SOCKET:-/run/display.sock}}"
KWIN_BIN="${KWIN_BIN:-kwin_wayland}"
START_PLASMA="${START_PLASMA:-1}"
[[ "$START_PLASMA" == 0 || "$START_PLASMA" == 1 ]] || {
    echo 'START_PLASMA must be 0 or 1' >&2; exit 1;
}

# Refuse a second desktop instead of attaching to an existing Wayland socket.
for process in kwin_wayland kwin_wayland_wrapper plasmashell startplasma-wayland; do
    if pgrep -u "$(id -u)" -f "(^|/)${process}([[:space:]]|$)" >/dev/null; then
        echo "An existing $process session is running; refusing to start another." >&2
        exit 1
    fi
done
[ -S "$SOCK" ] || { echo "Display socket not found: $SOCK" >&2; exit 1; }

if [[ "$START_PLASMA" == 1 ]]; then
    for program in startplasma-wayland kwin_wayland_wrapper kwin_wayland; do
        command -v "$program" >/dev/null 2>&1 || {
            echo "Required Plasma program not found: $program" >&2; exit 1;
        }
    done
    [[ "$KWIN_BIN" == kwin_wayland ]] || {
        echo 'Plasma uses kwin_wayland from PATH; KWIN_BIN requires START_PLASMA=0.' >&2
        exit 1
    }
    [[ -x /usr/lib/xdg-desktop-portal ]] || {
        echo 'Missing /usr/lib/xdg-desktop-portal; reinstall xdg-desktop-portal.' >&2
        exit 1
    }
else
    command -v "$KWIN_BIN" >/dev/null 2>&1 || {
        echo "KWin executable not found: $KWIN_BIN" >&2; exit 1;
    }
fi

export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
if [ ! -d "$XDG_RUNTIME_DIR" ]; then
    # Fresh DroidSpaces containers commonly lack a logind-created /run/user
    # directory. Keep the Wayland runtime directory private without requiring
    # a password or widening the launcher privileges.
    XDG_RUNTIME_DIR="$HOME/.local/run/anland-$(id -u)"
    # Do not chmod an existing path: it may be a symlink or unsafe directory.
    # mkdir -p leaves existing permissions unchanged; validate below.
    mkdir -p -m 0700 -- "$XDG_RUNTIME_DIR"
fi
[[ -d "$XDG_RUNTIME_DIR" && ! -L "$XDG_RUNTIME_DIR" && -O "$XDG_RUNTIME_DIR" &&
   "$(stat -c '%a' "$XDG_RUNTIME_DIR")" == 700 ]] || {
    echo "Runtime directory must be owned by this user with mode 0700: $XDG_RUNTIME_DIR" >&2
    exit 1
}
exec 9>"$XDG_RUNTIME_DIR/anland-producer-session.lock"
flock -n 9 || { echo 'Another Anland session entry is active' >&2; exit 1; }

# kgsl/turnip (Adreno) environment
export MESA_LOADER_DRIVER_OVERRIDE=kgsl
export GALLIUM_DRIVER=kgsl
export FD_FORCE_KGSL=1
export MESA_VK_DEVICE_SELECT_FORCE_DEFAULT_DEVICE=1
export FD_DEV_FEATURES=enable_tp_ubwc_flag_hint=1
export ANLAND_SKIP_IMPLICIT_SYNC_WAIT=1
export XCURSOR_THEME=breeze_cursors
export XCURSOR_SIZE=24
export QT_QPA_PLATFORM=wayland
export ANLAND_SOCKET="$SOCK"
export ANLAND_DRM_DEVICE="${ANLAND_DRM_DEVICE:-/dev/dri/renderD128}"
# KWin 6.8 moved render-device selection into GpuManager. An explicit real
# node avoids relying on udev enumeration inside DroidSpaces containers.
export KWIN_RENDER_NODES="${KWIN_RENDER_NODES:-$ANLAND_DRM_DEVICE}"
# Xwayland inherits this from KWin and uses it when linux-dmabuf feedback has
# no usable DRM render node on the kgsl/turnip stack.
export XWAYLAND_GBM_DEVICE="${XWAYLAND_GBM_DEVICE:-$ANLAND_DRM_DEVICE}"
[[ -r "$ANLAND_DRM_DEVICE" && -w "$ANLAND_DRM_DEVICE" ]] || {
    echo "Render node is not accessible: $ANLAND_DRM_DEVICE" >&2; exit 1;
}
unset DISPLAY WAYLAND_DISPLAY WAYLAND_SOCKET
export XDG_SESSION_TYPE=wayland
export XDG_CURRENT_DESKTOP=KDE
export XDG_SESSION_DESKTOP=KDE

if [[ "$START_PLASMA" == 1 ]]; then
    # plasma_session starts kwin_wayland_wrapper and publishes the display
    # environment on this same bus. The patched KWin selects Anland through
    # ANLAND_SOCKET; starting a standalone KWin here would create a second one.
    echo "==> Plasma / Anland (socket=$SOCK, drm=$ANLAND_DRM_DEVICE)"
    exec dbus-run-session -- startplasma-wayland
else
    echo "==> $KWIN_BIN --anland --xwayland (socket=$SOCK, drm=$ANLAND_DRM_DEVICE)"
    exec dbus-run-session -- "$KWIN_BIN" --anland --xwayland
fi