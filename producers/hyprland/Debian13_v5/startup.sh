#!/usr/bin/env bash
# Explicit foreground session. Build never invokes this script.
set -euo pipefail
HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
WORKDIR=${WORKDIR:-${XDG_CACHE_HOME:-$HOME/.cache}/anland-hyprland}
# Installed launcher does not depend on dpkg-dev or the developer's cache.
ARCH=$(uname -m)-linux-gnu
if [[ ${BASH_SOURCE[0]##*/} == hyprland-anland ]]; then
    DEFAULT_BIN=/usr/bin/Hyprland
    DEFAULT_LIBDIR=/usr/lib/$ARCH
else
    DEFAULT_BIN=$WORKDIR/build/hyprland/Hyprland
    DEFAULT_LIBDIR=$WORKDIR/sysroot/usr/lib/$ARCH
fi
BIN=${HYPRLAND_ANLAND_BINARY:-$DEFAULT_BIN}
LIBDIR=${HYPRLAND_ANLAND_LIBDIR:-$DEFAULT_LIBDIR}
die() { printf 'hyprland-anland: %s\n' "$*" >&2; exit 1; }
[[ -x "$BIN" ]] || die 'build output missing; run build.sh first'
export LD_LIBRARY_PATH="$LIBDIR${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export ANLAND=1
export XDG_SESSION_TYPE=wayland XDG_CURRENT_DESKTOP=Hyprland
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
export ANLAND_DRM_DEVICE=${ANLAND_DRM_DEVICE:-/dev/dri/renderD128}
# Explicit session logging, without rewriting the user configuration.
if [[ ${1:-} == --diagnose ]]; then
    export HYPRLAND_ANLAND_DIAGNOSTICS=1
    shift
    printf 'hyprland-anland: diagnostic logs enabled; runtime logs: %s/hypr/*/hyprland.log
' "$XDG_RUNTIME_DIR" >&2
fi
case ${1:-} in
    --help) printf '%s\n' 'startup.sh [--check|--version|--diagnose] [Hyprland options]' 'Set ANLAND_SOCKET and ANLAND_DRM_DEVICE as needed.'; exit 0 ;;
    --version) exec "$BIN" --version ;;
    --check)
        [[ -d "$XDG_RUNTIME_DIR" && -O "$XDG_RUNTIME_DIR" && -w "$XDG_RUNTIME_DIR" ]] || die 'runtime directory must exist and be owned/writable'
        [[ -r "$ANLAND_DRM_DEVICE" && -w "$ANLAND_DRM_DEVICE" ]] || die 'render node unavailable'
        linkage=$(ldd "$BIN"); printf '%s\n' "$linkage"
        [[ "$linkage" != *'not found'* ]] || die 'runtime library missing'
        printf '%s\n' 'HYPRLAND_ANLAND_PREFLIGHT_OK (not a consumer handshake or graphical test)'; exit 0 ;;
esac
[[ -d "$XDG_RUNTIME_DIR" && -O "$XDG_RUNTIME_DIR" && -w "$XDG_RUNTIME_DIR" ]] || die 'runtime directory must exist and be owned/writable'
exec 9>"$XDG_RUNTIME_DIR/anland-producer-session.lock"
flock -n 9 || die 'another producer owns the session'
for name in Hyprland mango kwin_wayland gnome-shell niri gamescope weston; do
    if pgrep -u "$(id -u)" -x "$name" >/dev/null; then die "existing $name session; not taking it over"; fi
done
[[ -r "$ANLAND_DRM_DEVICE" && -w "$ANLAND_DRM_DEVICE" ]] || die 'render node unavailable'
# GPU overrides remain opt-in. Audio/session services are owned by the user, not this launcher.
unset WAYLAND_DISPLAY DISPLAY
exec "$BIN" "$@"