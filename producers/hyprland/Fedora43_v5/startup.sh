#!/usr/bin/env bash
# Fedora private prefix; keeps system Hyprland/Mesa/SDK untouched.
set -uo pipefail
base=${HYPRLAND_ANLAND_PREFIX:-/usr/lib64/hyprland-anland}
export HYPRLAND_ANLAND_BINARY=${HYPRLAND_ANLAND_BINARY:-$base/bin/Hyprland}
export HYPRLAND_ANLAND_LIBDIR=${HYPRLAND_ANLAND_LIBDIR:-$base/lib}
export LD_LIBRARY_PATH="$HYPRLAND_ANLAND_LIBDIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export PATH="$base/bin:$PATH"
export ANLAND=1 ANLAND_SOCKET=${ANLAND_SOCKET:-/run/display.sock}
export ANLAND_DRM_DEVICE=${ANLAND_DRM_DEVICE:-/dev/dri/renderD128}
export XWAYLAND_GBM_DEVICE=${XWAYLAND_GBM_DEVICE:-$ANLAND_DRM_DEVICE}
export XDG_SESSION_TYPE=wayland XDG_CURRENT_DESKTOP=Hyprland
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
die() { printf 'hyprland-anland: %s\n' "$*" >&2; exit 1; }
case ${1:-} in
    --help) printf '%s\n' 'hyprland-anland [--check|--version|--diagnose] [Hyprland options]'; exit 0 ;;
    --version) exec "$HYPRLAND_ANLAND_BINARY" --version ;;
    --diagnose) export HYPRLAND_ANLAND_DIAGNOSTICS=1; shift ;;
esac
[[ -x $HYPRLAND_ANLAND_BINARY ]] || die 'missing Fedora runtime'
[[ -d $XDG_RUNTIME_DIR && ! -L $XDG_RUNTIME_DIR && -O $XDG_RUNTIME_DIR && $(stat -c %a "$XDG_RUNTIME_DIR") == 700 ]] || die 'runtime directory must be owned, mode 0700'
[[ -c $ANLAND_DRM_DEVICE && -r $ANLAND_DRM_DEVICE && -w $ANLAND_DRM_DEVICE ]] || die 'render device unavailable'
if [[ ${1:-} == --check ]]; then
    linkage=$(ldd "$HYPRLAND_ANLAND_BINARY") || die 'ldd failed'
    printf '%s\n' "$linkage"
    [[ $linkage != *'not found'* ]] || die 'runtime library missing'
    printf 'HYPRLAND_ANLAND_PREFLIGHT_OK (no consumer handshake or graphical test)\n'; exit 0
fi
exec 9>"$XDG_RUNTIME_DIR/anland-producer-session.lock" || die 'cannot open session lock'
flock -n 9 || die 'another producer owns the session'
for name in Hyprland mango kwin_wayland gnome-shell niri niri-anland deniald gamescope weston; do
    if pgrep -u "$(id -u)" -x "$name" >/dev/null; then die "existing $name session; refusing takeover"; fi
done
unset WAYLAND_DISPLAY DISPLAY
exec "$HYPRLAND_ANLAND_BINARY" "$@"