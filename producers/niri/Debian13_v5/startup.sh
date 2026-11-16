#!/usr/bin/env bash
# Explicit opt-in launch; packaging never runs this script or restarts a session.
set -uo pipefail
die() { printf 'niri-anland: %s\n' "$*" >&2; exit 1; }
BIN=${NIRI_ANLAND_BIN:-/usr/bin/niri-anland}
case ${1:-} in
    --help) printf '%s\n' 'ANLAND_START_NIRI=YES niri-anland-start [Niri options]' 'niri-anland-start --check|--version'; exit 0 ;;
    --version) exec "$BIN" --version ;;
esac
[[ -x $BIN ]] || die "missing compositor: $BIN"
command -v xwayland-satellite >/dev/null || die 'missing xwayland-satellite'
export ANLAND=1 ANLAND_SOCKET=${ANLAND_SOCKET:-/run/display.sock}
export ANLAND_DRM_DEVICE=${ANLAND_DRM_DEVICE:-/dev/dri/renderD128}
export XWAYLAND_GBM_DEVICE=${XWAYLAND_GBM_DEVICE:-$ANLAND_DRM_DEVICE}
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
[[ -d $XDG_RUNTIME_DIR && -O $XDG_RUNTIME_DIR && -w $XDG_RUNTIME_DIR ]] || die 'invalid runtime directory'
[[ -S $ANLAND_SOCKET ]] || die 'missing Anland socket'
if [[ ${1:-} == --check ]]; then
    linkage=$(env -u LD_LIBRARY_PATH ldd "$BIN") || die 'ldd failed'
    printf '%s\n' "$linkage"
    [[ $linkage != *'not found'* ]] || die 'runtime library missing'
    printf 'NIRI_ANLAND_PREFLIGHT_OK (not a handshake or graphical test)\n'; exit 0
fi
[[ ${ANLAND_START_NIRI:-} == YES ]] || die 'set ANLAND_START_NIRI=YES to launch'
exec 9>"$XDG_RUNTIME_DIR/anland-producer-session.lock" || die 'cannot open session lock'
flock -n 9 || die 'another producer owns the session'
for name in niri niri-anland kwin_wayland Hyprland gnome-shell mango gamescope weston; do
    if pgrep -u "$(id -u)" -x "$name" >/dev/null; then die "existing $name session; not taking it over"; fi
done
# Driver overrides are opt-in, unlike the old Arch launcher's hardcoded KGSL defaults.
"$BIN" "$@" 9>&- &
child=$!
trap 'kill -TERM "$child" 2>/dev/null || true' INT TERM HUP
rc=0
while :; do
    wait "$child" && { rc=0; break; }
    rc=$?
    kill -0 "$child" 2>/dev/null || break
done
exit "$rc"
