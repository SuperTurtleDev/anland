#!/usr/bin/env bash
# Explicit foreground session. Build never invokes this script.
set -uo pipefail
PREFIX=${HYPRLAND_ANLAND_PREFIX:-/usr}
BIN=${HYPRLAND_ANLAND_BINARY:-$PREFIX/bin/Hyprland}
# The packaged /usr layout uses the system loader paths. Custom prefixes must
# carry their own RUNPATH; never inject WM libraries into Xwayland and clients.
die() { printf 'hyprland-anland: %s\n' "$*" >&2; exit 1; }
[[ ${1:-} != --help ]] || { printf '%s\n' 'startup.sh [--check|--version|--diagnose]' 'Set ANLAND_SOCKET and ANLAND_DRM_DEVICE for your deployment.'; exit 0; }
[[ -x "$BIN" ]] || die 'build output missing; run build.sh first'
export PATH="$PREFIX/bin:$PATH"
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
        linkage=$(env -u LD_LIBRARY_PATH ldd "$BIN") || die "ldd failed"; printf '%s\n' "$linkage"
        [[ "$linkage" != *'not found'* ]] || die 'runtime library missing'
        printf '%s\n' 'HYPRLAND_ANLAND_PREFLIGHT_OK (not a consumer handshake or graphical test)'; exit 0 ;;
esac
[[ -d "$XDG_RUNTIME_DIR" && -O "$XDG_RUNTIME_DIR" && -w "$XDG_RUNTIME_DIR" ]] || die 'runtime directory must exist and be owned/writable'
exec 9>"$XDG_RUNTIME_DIR/anland-producer-session.lock" || die "cannot open session lock"
flock -n 9 || die 'another producer owns the session'
for name in Hyprland mango kwin_wayland gnome-shell niri gamescope weston; do
    if pgrep -u "$(id -u)" -x "$name" >/dev/null; then die "existing $name session; not taking it over"; fi
done
[[ -r "$ANLAND_DRM_DEVICE" && -w "$ANLAND_DRM_DEVICE" ]] || die 'render node unavailable'
# GPU overrides remain opt-in. Audio/session services are owned by the user, not this launcher.
unset WAYLAND_DISPLAY DISPLAY
# Supervise the compositor instead of exec'ing it: the session lock then belongs
# to this launcher, so clients and helpers spawned by the compositor never
# inherit fd 9 and cannot keep the session locked after it exits. Signals are
# forwarded explicitly because a background child ignores SIGINT/SIGQUIT.
env --default-signal=INT,QUIT "$BIN" "$@" <&0 9>&- &
child=$!
forward() { kill -TERM "$child" 2>/dev/null || true; }
trap forward INT QUIT TERM HUP
rc=0
while :; do
    wait "$child" && { rc=0; break; }
    rc=$?
    kill -0 "$child" 2>/dev/null || break
done
trap - INT QUIT TERM HUP
exit "$rc"