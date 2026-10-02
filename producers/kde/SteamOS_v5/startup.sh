#!/usr/bin/env bash
# SteamOS 6.2.5 / aarch64: native Anland, Zink -> Turnip.
# Usage: ./startup.sh [socket path]
# START_PLASMA=0 starts only KWin; default starts the complete Plasma session.
# Does not stop/restart an existing compositor, daemon or consumer.
set -euo pipefail
if [[ ${1:-} == --help || ${1:-} == -h ]]; then
    sed -n '2,5p' "$0"
    exit 0
fi
export ANLAND=1
export ANLAND_SOCKET="${1:-${ANLAND_SOCKET:-/run/display.sock}}"
export LANG="${LANG:-C.UTF-8}"
[[ "$LANG" != C && "$LANG" != POSIX ]] || export LANG=C.UTF-8
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
if [[ ! -d "$XDG_RUNTIME_DIR" ]]; then
    export XDG_RUNTIME_DIR="$HOME/.local/run/anland-$(id -u)"
    mkdir -p "$XDG_RUNTIME_DIR"
    chmod 0700 "$XDG_RUNTIME_DIR"
fi
[[ -O "$XDG_RUNTIME_DIR" && -w "$XDG_RUNTIME_DIR" ]] || {
    echo 'Runtime directory must be owned and writable by the current user' >&2; exit 1;
}
# Keep explicit caller overrides. Do not copy the Arch kgsl GL override.
export MESA_LOADER_DRIVER_OVERRIDE="${MESA_LOADER_DRIVER_OVERRIDE:-zink}"
export GALLIUM_DRIVER="${GALLIUM_DRIVER:-zink}"
export QT_QPA_PLATFORM=wayland
export XCURSOR_THEME="${XCURSOR_THEME:-breeze_cursors}"
export XCURSOR_SIZE="${XCURSOR_SIZE:-24}"
export ANLAND_DRM_DEVICE="${ANLAND_DRM_DEVICE:-/dev/dri/renderD128}"
export XWAYLAND_GBM_DEVICE="${XWAYLAND_GBM_DEVICE:-$ANLAND_DRM_DEVICE}"
[[ -r "$ANLAND_DRM_DEVICE" && -w "$ANLAND_DRM_DEVICE" ]] || {
    echo "Cannot access $ANLAND_DRM_DEVICE; check effective droidspaces-gpu group membership" >&2; exit 1;
}
[[ -S "$ANLAND_SOCKET" ]] || { echo "Daemon socket missing: $ANLAND_SOCKET" >&2; exit 1; }
# Shared with the Gamescope entry; take the lock before checking processes.
# Third-party launchers are not covered by this advisory lock.
command -v flock >/dev/null
command -v pgrep >/dev/null
exec 9>"$XDG_RUNTIME_DIR/anland-producer-session.lock"
flock -n 9 || { echo 'Another Anland session entry is active' >&2; exit 1; }
for name in kwin_wayland gamescope gnome-shell niri Hyprland weston; do
    if pgrep -u "$(id -u)" -x "$name" >/dev/null; then
        echo "Existing $name session detected; explicitly exit it first" >&2; exit 1
    fi
done
unset DISPLAY WAYLAND_DISPLAY
# Preserve implicit/explicit synchronization; no ANLAND_SKIP_IMPLICIT_SYNC_WAIT override.
if [[ ${START_PLASMA:-1} == 0 ]]; then
    exec dbus-run-session -- "${KWIN_BIN:-kwin_wayland}" --anland --xwayland
elif [[ ${START_PLASMA:-1} == 1 ]]; then
    # Plasma starts its own KWin under the same D-Bus session. ANLAND=1 selects
    # the backend without creating an extra standalone compositor beforehand.
    exec dbus-run-session -- startplasma-wayland
else
    echo 'START_PLASMA must be 0 or 1' >&2; exit 1
fi