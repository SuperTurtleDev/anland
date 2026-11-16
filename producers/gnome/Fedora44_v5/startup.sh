#!/usr/bin/env bash
# Foreground Fedora GNOME session; never adjusts drivers or restarts services.
set -uo pipefail
die() { printf 'gnome-anland: %s\n' "$*" >&2; exit 1; }
case ${1:-} in
    --help) printf '%s\n' 'gnome-anland [--check] [Anland socket]' 'Uses Fedora GNOME50; caller owns Mesa and session-service configuration.'; exit 0 ;;
esac
mode=run
[[ ${1:-} != --check ]] || { mode=check; shift; }
[[ $# -le 1 ]] || die 'expected at most one Anland socket'
export ANLAND=1 ANLAND_SOCKET=${1:-${ANLAND_SOCKET:-/run/display.sock}}
export ANLAND_DRM_DEVICE=${ANLAND_DRM_DEVICE:-/dev/dri/renderD128}
export XWAYLAND_GBM_DEVICE=${XWAYLAND_GBM_DEVICE:-$ANLAND_DRM_DEVICE}
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
export XDG_SESSION_TYPE=wayland XDG_CURRENT_DESKTOP=GNOME XDG_SESSION_DESKTOP=gnome
export GNOME_SHELL_SESSION_MODE=gnome
[[ -d $XDG_RUNTIME_DIR && ! -L $XDG_RUNTIME_DIR && -O $XDG_RUNTIME_DIR && $(stat -c %a "$XDG_RUNTIME_DIR") == 700 ]] || die 'runtime directory must exist, owned, mode 0700'
[[ -S $ANLAND_SOCKET ]] || die "missing Anland socket: $ANLAND_SOCKET"
[[ -c $ANLAND_DRM_DEVICE && -r $ANLAND_DRM_DEVICE && -w $ANLAND_DRM_DEVICE ]] || die 'render device inaccessible'
for tool in gnome-shell gnome-session Xwayland; do command -v "$tool" >/dev/null || die "missing $tool"; done
if [[ $mode == check ]]; then
    gnome-shell --help 2>&1 | grep -q -- '--anland' || die 'Mutter lacks Anland support'
    printf 'GNOME_ANLAND_PREFLIGHT_OK (no consumer handshake or graphical test)\n'; exit 0
fi
exec 9>"$XDG_RUNTIME_DIR/anland-producer-session.lock" || die 'session lock'
flock -n 9 || die 'another producer owns the session'
for name in gnome-shell mutter kwin_wayland Hyprland mango niri niri-anland deniald gamescope weston; do
    if pgrep -u "$(id -u)" -x "$name" >/dev/null; then die "existing $name session; refusing takeover"; fi
done
unset DISPLAY WAYLAND_DISPLAY
# GNOME50's session manager may require an existing user systemd manager.
# A missing manager is not grounds to start/stop one or change the caller's bus.
exec gnome-session