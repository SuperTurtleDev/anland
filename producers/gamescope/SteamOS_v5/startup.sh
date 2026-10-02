#!/usr/bin/env bash
# Direct Gamescope/Anland session; never kills an existing desktop or consumer.
# Default: the ARM64 Steam desktop client (-nobigpicture). Use its UI for Big Picture.
# Pass -- COMMAND... for another client; -nobigpicture auto-login is not assumed.
set -euo pipefail
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
STEAM_BIN=/home/steamos/.local/share/Steam/steamrtarm64/steam
if [[ ${1:-} == --steam-client ]]; then
    shift
    # Launch the ARM64 Steam client directly, without distro wrappers or helpers.
    [[ -x "$STEAM_BIN" ]] || { printf 'Steam executable missing: %s\n' "$STEAM_BIN" >&2; exit 1; }
    # Match Steam's normal data-root working directory, not the binary subdirectory.
    cd -- /home/steamos/.local/share/Steam
    # Steam's XRandR wrappers use RTLD_NEXT; make the real symbols global early.
    # Loader --preload applies only to this executable, not Steam's child processes.
    if [[ $(uname -m) == aarch64 && ${ANLAND_STEAM_XRANDR_PRELOAD:-1} == 1 ]]; then
        export LD_PRELOAD="libXrandr.so.2${LD_PRELOAD:+:$LD_PRELOAD}"
    fi
    exec steam -nobigpicture "$@"
fi
die() { printf 'gamescope-anland: %s\n' "$*" >&2; exit 1; }
if [[ ${1:-} == --help ]]; then
    printf '%s\n' 'Usage: bash startup.sh [-- COMMAND...]' \
      'Defaults to steamrtarm64/steam -nobigpicture. Explicitly exit the desktop before starting.' \
      'GAMESCOPE_BIN overrides the test build; ANLAND_SOCKET selects the daemon.'
    exit 0
fi
# Direct output is selected explicitly with --backend anland below.
# Do not leak the automatic backend selector into games and nested compositors.
unset ANLAND
export ANLAND_SOCKET="${ANLAND_SOCKET:-/run/display.sock}"
export ANLAND_DRM_DEVICE="${ANLAND_DRM_DEVICE:-/dev/dri/renderD128}"
export XWAYLAND_GBM_DEVICE="${XWAYLAND_GBM_DEVICE:-$ANLAND_DRM_DEVICE}"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
[[ -d "$XDG_RUNTIME_DIR" && -O "$XDG_RUNTIME_DIR" && -w "$XDG_RUNTIME_DIR" ]] || die 'runtime directory must exist, be owned and writable'
[[ -S "$ANLAND_SOCKET" ]] || die "daemon socket missing: $ANLAND_SOCKET"
[[ -r "$ANLAND_DRM_DEVICE" && -w "$ANLAND_DRM_DEVICE" ]] || die "render node not accessible: $ANLAND_DRM_DEVICE"
# Use the same advisory lock as the KDE entry, before the process check.
command -v flock >/dev/null || die 'flock is required'
command -v pgrep >/dev/null || die 'pgrep is required'
exec 9>"$XDG_RUNTIME_DIR/anland-producer-session.lock"
flock -n 9 || die 'another Anland session entry is active'
for name in kwin_wayland gamescope gnome-shell niri Hyprland weston; do
    if pgrep -u "$(id -u)" -x "$name" >/dev/null; then
        die "existing $name session detected; explicitly exit it first"
    fi
done
# Keep fd 9 open across exec to hold the shared session lock.
# Normal launches always use the tracked system package, including checkout entry.
# Testing another binary requires an explicit override.
GAMESCOPE_BIN="${GAMESCOPE_BIN:-/usr/bin/gamescope}"
printf 'gamescope-anland: using %s\n' "$GAMESCOPE_BIN" >&2
[[ -x "$GAMESCOPE_BIN" ]] || die "Gamescope build missing: $GAMESCOPE_BIN"
export MESA_LOADER_DRIVER_OVERRIDE="${MESA_LOADER_DRIVER_OVERRIDE:-zink}"
export GALLIUM_DRIVER="${GALLIUM_DRIVER:-zink}"
if [[ -z ${VK_DRIVER_FILES:-} && -z ${VK_ICD_FILENAMES:-} ]]; then
    [[ -f /usr/share/vulkan/icd.d/freedreno_icd.aarch64.json ]] || die 'Turnip ICD missing; specify VK_DRIVER_FILES'
    export VK_DRIVER_FILES=/usr/share/vulkan/icd.d/freedreno_icd.aarch64.json
fi
unset DISPLAY WAYLAND_DISPLAY QT_QPA_PLATFORM
STEAM_MODE=0
if [[ $# == 0 ]]; then
    STEAM_MODE=1
    [[ -x "$STEAM_BIN" ]] || die "Steam executable missing: $STEAM_BIN"
    # Never attach a pre-existing Steam instance to the wrong display/session.
    if pgrep -u "$(id -u)" -x steam >/dev/null; then
        die 'Steam already running; explicitly exit it before game mode'
    fi
    set -- bash "$SCRIPT_DIR/$(basename -- "${BASH_SOURCE[0]}")" --steam-client
elif [[ $1 == -- && $# -gt 1 ]]; then
    shift
else
    die 'use -- before a test/client command'
fi
# Reuse the existing user bus and PipeWire. No restart/logout or service stop.
if [[ $STEAM_MODE == 1 ]]; then
    # SteamControlled may not publish a focus choice during initial UI startup.
    # The existing patch uses this only when no normal focus was selected.
    export GAMESCOPE_FALLBACK_APPID="${GAMESCOPE_FALLBACK_APPID:-769}"
    exec "$GAMESCOPE_BIN" --backend anland -e -- "$@"
else
    exec "$GAMESCOPE_BIN" --backend anland -- "$@"
fi