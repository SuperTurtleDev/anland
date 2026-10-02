#!/usr/bin/env bash
# Explicit opt-in only: never run from build/package scripts.
set -euo pipefail
[[ "${ANLAND_START_NIRI:-}" == YES ]] || { echo 'Set ANLAND_START_NIRI=YES to launch a separate niri session.' >&2; exit 1; }
SOCK="${ANLAND_SOCKET:-/run/display.sock}"
[[ -S "$SOCK" ]] || { echo "Missing display socket: $SOCK" >&2; exit 1; }
for process in niri niri-anland kwin_wayland Hyprland gnome-shell plasmashell startplasma-wayland; do
    if pgrep -u "$(id -u)" -f "(^|/)${process}([[:space:]]|$)" >/dev/null; then
        echo "Existing $process session; refusing to launch another." >&2
        exit 1
    fi
done
BIN="${NIRI_ANLAND_BIN:-/usr/bin/niri-anland}"
[[ -x "$BIN" ]] || { echo "Missing compositor: $BIN" >&2; exit 1; }
command -v xwayland-satellite >/dev/null 2>&1 || {
    echo 'Missing xwayland-satellite; install the xwayland-satellite package for X11 integration.' >&2
    exit 1
}
# Select the Anland backend explicitly; do not rely on a login-shell variable.
export ANLAND=1
export ANLAND_SOCKET="$SOCK"
# Same KGSL/Turnip defaults as the legacy Arch producer launcher.
export MESA_LOADER_DRIVER_OVERRIDE="${MESA_LOADER_DRIVER_OVERRIDE:-kgsl}"
export GALLIUM_DRIVER="${GALLIUM_DRIVER:-kgsl}"
export FD_FORCE_KGSL="${FD_FORCE_KGSL:-1}"
export MESA_VK_DEVICE_SELECT_FORCE_DEFAULT_DEVICE="${MESA_VK_DEVICE_SELECT_FORCE_DEFAULT_DEVICE:-1}"
export FD_DEV_FEATURES="${FD_DEV_FEATURES:-enable_tp_ubwc_flag_hint=1}"
export ANLAND_SKIP_IMPLICIT_SYNC_WAIT="${ANLAND_SKIP_IMPLICIT_SYNC_WAIT:-1}"
export ANLAND_DRM_DEVICE="${ANLAND_DRM_DEVICE:-/dev/dri/renderD128}"
# Inherited by xwayland-satellite and its patched Xwayland child.
export XWAYLAND_GBM_DEVICE="${XWAYLAND_GBM_DEVICE:-$ANLAND_DRM_DEVICE}"
export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-wayland}"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
[[ -d "$XDG_RUNTIME_DIR" && -O "$XDG_RUNTIME_DIR" ]] || { echo "Invalid runtime directory: $XDG_RUNTIME_DIR" >&2; exit 1; }
# No --session: do not change global D-Bus/systemd environment during opt-in tests.
exec "$BIN" "$@"