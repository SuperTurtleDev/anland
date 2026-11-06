#!/usr/bin/env bash
# Direct Armada game mode; never stop/restart another compositor or consumer.
set -euo pipefail
HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
die() { printf 'armada-anland: %s\n' "$*" >&2; exit 1; }
usage() {
    printf '%s\n' 'Usage: startup.sh [--check] [-- COMMAND...]' \
      'Default client: native ARM64 Steam Big Picture (-gamepadui).' \
      'GAMESCOPE_BIN, STEAM_ROOT, ANLAND_SOCKET and ANLAND_DRM_DEVICE are configurable.' \
      '--check: files/backend/permissions only; no handshake, service or session startup.'
}
case ${1:-} in -h|--help) usage; exit 0 ;; esac
STEAM_ROOT=${STEAM_ROOT:-$HOME/.local/share/Steam}
STEAM_BIN=${STEAM_BIN:-$STEAM_ROOT/steamrtarm64/steam}
if [[ ${1:-} == --steam-client ]]; then
    shift
    [[ $(id -u) != 0 && $(uname -m) == aarch64 ]] || die 'Steam requires an ordinary ARM64 user'
    [[ -x $STEAM_BIN ]] || die "Steam missing: $STEAM_BIN"
    # Only the Steam child receives the previously validated KGSL/ANGLE workaround.
    if [[ ${ANLAND_STEAM_TIMESTAMP_WORKAROUND:-1} == 1 ]]; then
        case :${ANGLE_FEATURE_OVERRIDES_ENABLED:-}: in
            *:disableTimestampQueries:*) ;;
            *) export ANGLE_FEATURE_OVERRIDES_ENABLED="${ANGLE_FEATURE_OVERRIDES_ENABLED:+${ANGLE_FEATURE_OVERRIDES_ENABLED}:}disableTimestampQueries" ;;
        esac
    fi
    export STEAM_ROOT
    if [[ -x /usr/libexec/armada/launch-steam && ${ANLAND_STEAM_DIRECT:-0} != 1 ]]; then
        exec /usr/libexec/armada/launch-steam -gamepadui "$@"
    fi
    # Same library/runtime arrangement as Armada's launch-steam, without changing
    # official Steam files or bypassing its updater/verification.
    export LD_LIBRARY_PATH="$STEAM_ROOT/steamrtarm64:$STEAM_ROOT/lib/aarch64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
    if [[ -d $STEAM_ROOT/steam-runtime-steamrt-arm64/bin ]]; then
        export PATH="$STEAM_ROOT/steam-runtime-steamrt-arm64/bin:$PATH"
    fi
    export SYSTEM_PATH=$PATH SYSTEM_LD_LIBRARY_PATH=$LD_LIBRARY_PATH
    cd -- "$(dirname -- "$STEAM_BIN")"
    exec "$STEAM_BIN" -gamepadui "$@"
fi
CHECK=0
if [[ ${1:-} == --check ]]; then CHECK=1; shift; fi
STEAM_MODE=1
if [[ $# -gt 0 ]]; then
    [[ $1 == -- && $# -gt 1 ]] || die 'use -- before a client command'
    shift
    STEAM_MODE=0
fi
[[ $(id -u) != 0 && $(uname -m) == aarch64 ]] || die 'requires an ordinary ARM64 user'
GAMESCOPE_BIN=${GAMESCOPE_BIN:-/usr/bin/gamescope}
export ANLAND_SOCKET=${ANLAND_SOCKET:-/run/display.sock}
export ANLAND_DRM_DEVICE=${ANLAND_DRM_DEVICE:-/dev/dri/renderD128}
export XWAYLAND_GBM_DEVICE=${XWAYLAND_GBM_DEVICE:-$ANLAND_DRM_DEVICE}
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
[[ -x $GAMESCOPE_BIN ]] || die "Gamescope missing: $GAMESCOPE_BIN"
[[ -d $XDG_RUNTIME_DIR && -O $XDG_RUNTIME_DIR && -w $XDG_RUNTIME_DIR ]] || die 'runtime directory must be user-owned and writable'
[[ $(stat -c %a -- "$XDG_RUNTIME_DIR") == 700 ]] || die 'runtime directory must have mode 0700'
[[ -S $ANLAND_SOCKET ]] || die "daemon socket missing: $ANLAND_SOCKET"
[[ -c $ANLAND_DRM_DEVICE && -r $ANLAND_DRM_DEVICE && -w $ANLAND_DRM_DEVICE ]] || die "render device inaccessible: $ANLAND_DRM_DEVICE"
[[ -S $XDG_RUNTIME_DIR/bus ]] || die 'existing user D-Bus is required'
export DBUS_SESSION_BUS_ADDRESS=${DBUS_SESSION_BUS_ADDRESS:-unix:path=$XDG_RUNTIME_DIR/bus}
for tool in flock pgrep Xwayland; do command -v "$tool" >/dev/null || die "missing tool: $tool"; done
# This is a capability preflight, not a rendering test. Do not accept Fedora's
# unpatched Gamescope merely because it has the same binary name.
help=$("$GAMESCOPE_BIN" --help 2>&1) || die 'Gamescope --help failed'
[[ $help == *'anland =>'* ]] || die 'installed Gamescope does not expose the Anland backend'
if [[ $STEAM_MODE == 1 ]]; then
    [[ -x $STEAM_BIN ]] || die "Steam missing: $STEAM_BIN (download separately)"
fi
if [[ $CHECK == 1 ]]; then
    printf 'ARMADA_ANLAND_PREFLIGHT_OK binary=%s steam=%s\n' "$GAMESCOPE_BIN" "$STEAM_MODE"
    printf '%s\n' 'No handshake, rendering, input test, service start or configuration change performed.'
    exit 0
fi
exec 9>"$XDG_RUNTIME_DIR/anland-producer-session.lock"
flock -n 9 || die 'another Anland session entry is active'
for name in kwin_wayland gamescope gnome-shell niri Hyprland weston; do
    if pgrep -u "$(id -u)" -x "$name" >/dev/null; then die "existing $name session; exit it explicitly first"; fi
done
# Select the backend on Gamescope only, not on games or nested compositors.
unset ANLAND DISPLAY WAYLAND_DISPLAY QT_QPA_PLATFORM
if [[ ${ANLAND_USE_ZINK:-1} == 1 ]]; then
    export MESA_LOADER_DRIVER_OVERRIDE=${MESA_LOADER_DRIVER_OVERRIDE:-zink}
    export GALLIUM_DRIVER=${GALLIUM_DRIVER:-zink}
fi
# Preserve user-selected VK_DRIVER_FILES/VK_ICD_FILENAMES and the installed
# driver loader defaults. Do not rewrite the system Mesa/Turnip selection.
if [[ $STEAM_MODE == 1 ]]; then
    if pgrep -u "$(id -u)" -x steam >/dev/null; then die 'Steam already running; exit it explicitly first'; fi
    export GAMESCOPE_FALLBACK_APPID=${GAMESCOPE_FALLBACK_APPID:-769}
    exec "$GAMESCOPE_BIN" --backend anland -f -e -- bash "$HERE/$(basename -- "${BASH_SOURCE[0]}")" --steam-client
else
    exec "$GAMESCOPE_BIN" --backend anland -f -- "$@"
fi