#!/usr/bin/env bash
# Foreground Mango session; never stops another WM or changes services.
# Usage: ./startup.sh [--check|--version|--help]
set -uo pipefail
PREFIX=${MANGO_PREFIX:-/usr/lib/mango-anland}
die() { printf 'mango-start: %s\n' "$*" >&2; exit 1; }
[[ $# -le 1 ]] || die 'usage: startup.sh [--check|--version|--help]'
case "${1:-start}" in
    --help|-h) sed -n '2,3p' "$0"; exit 0 ;;
    start|run|--check|--version) ;;
    *) die 'usage: startup.sh [--check|--version|--help]' ;;
esac
[[ ${1:-} != --help ]] || { printf '%s\n' 'startup.sh [--check|--version]' 'Set ANLAND_SOCKET and ANLAND_DRM_DEVICE for your deployment.'; exit 0; }
[[ -x "$PREFIX/bin/mango" ]] || die "build output unavailable: $PREFIX"
export PATH="$PREFIX/bin:$PATH"
# The private prefix stays out of the inherited environment. The compositor
# resolves its own private libraries through RUNPATH (verified by --check), and
# only a private panel helper is given the prefix explicitly. Exporting it here
# would shadow same-named system libraries (e.g. libdisplay_producer.so) inside
# every Xwayland, panel and client process started by this session.
private_lib=$PREFIX/lib
export ANLAND=1 ANLAND_SOCKET=${ANLAND_SOCKET:-/run/display.sock}
export ANLAND_DRM_DEVICE=${ANLAND_DRM_DEVICE:-/dev/dri/renderD128}
export WLR_RENDER_DRM_DEVICE=${WLR_RENDER_DRM_DEVICE:-$ANLAND_DRM_DEVICE}
export XWAYLAND_GBM_DEVICE=${XWAYLAND_GBM_DEVICE:-$ANLAND_DRM_DEVICE}
# The kgsl Mesa EGLDevice may advertise EGL_MESA_device_software even while
# GL_RENDERER is FD750/freedreno. Preserve the established Anland opt-in for
# this explicitly selected driver only; never force a GPU or disable sync.
if [[ ${MESA_LOADER_DRIVER_OVERRIDE:-} == kgsl || ${GALLIUM_DRIVER:-} == kgsl ]]; then
    export WLR_RENDERER_ALLOW_SOFTWARE=${WLR_RENDERER_ALLOW_SOFTWARE:-1}
fi
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
export PULSE_SERVER=${PULSE_SERVER:-unix:$XDG_RUNTIME_DIR/pulse/native}
export XDG_SESSION_TYPE=wayland XDG_CURRENT_DESKTOP=mango
export MANGO_ANLAND_MANAGE_SESSION=${MANGO_ANLAND_MANAGE_SESSION:-0}
[[ "$MANGO_ANLAND_MANAGE_SESSION" =~ ^[01]$ ]] || die 'MANGO_ANLAND_MANAGE_SESSION must be 0 or 1'
[[ "${1:-}" != --version ]] || exec "$PREFIX/bin/mango" -v
config=${MANGO_ANLAND_CONFIG:-$HOME/.config/mango/config.conf}
if [[ -z ${MANGO_ANLAND_CONFIG:-} && ! -f "$config" ]]; then
    if [[ "$PREFIX" == /usr/lib/mango-anland ]]; then config=/etc/mango-anland/config.conf;
    else config="$PREFIX/etc/mango/config.conf"; fi
fi
[[ -r "$config" ]] || die "configuration is not readable: $config"
[[ -d "$XDG_RUNTIME_DIR" && -w "$XDG_RUNTIME_DIR" && -O "$XDG_RUNTIME_DIR" ]] || die 'runtime directory must exist, be owned and writable'
if [[ "${1:-}" == --check ]]; then
    [[ -S "$ANLAND_SOCKET" && -r "$ANLAND_DRM_DEVICE" && -w "$ANLAND_DRM_DEVICE" ]] || die 'socket or DRM device unavailable'
    # The private prefix must be self-contained through RUNPATH alone, otherwise
    # a launcher-level LD_LIBRARY_PATH would be hiding a packaging defect.
    linkage=$(env -u LD_LIBRARY_PATH ldd "$PREFIX/bin/mango") || die 'ldd failed'
    printf '%s\n' "$linkage"
    [[ "$linkage" != *'not found'* ]] || die 'runtime library missing'
    printf 'MANGO_START_PREFLIGHT_OK (filesystem/link checks only, not a handshake)\n'
    exit 0
fi
for tool in flock pgrep; do command -v "$tool" >/dev/null || die "missing tool: $tool"; done
exec 9>"$XDG_RUNTIME_DIR/anland-producer-session.lock" || die "cannot open session lock"
flock -n 9 || die 'session is already running'
for name in kwin_wayland kwin_x11 gamescope gnome-shell niri Hyprland weston mango; do
    if pgrep -u "$(id -u)" -x "$name" >/dev/null; then die "existing $name session detected; explicitly exit it first"; fi
done
for ((attempt=0; attempt<60; attempt++)); do
    [[ ! -S "$ANLAND_SOCKET" || ! -e "$ANLAND_DRM_DEVICE" ]] || break
    sleep 0.5
done
[[ -S "$ANLAND_SOCKET" && -r "$ANLAND_DRM_DEVICE" && -w "$ANLAND_DRM_DEVICE" ]] || die 'Anland socket or DRM device is unavailable'
args=(-c "$config")
# Only a panel taken from the private prefix needs the private library path; a
# system panel keeps the ordinary search path.
panel_command() {
    local bar
    bar=$(command -v mangobar) || return 1
    if [[ $bar == "$PREFIX/"* ]]; then
        # Quote the resolved helper and prefix for the shell that Mango uses.
        # Keep the variable local to the panel process, not its restart loop.
        printf 'while :; do env LD_LIBRARY_PATH=%q%s %q; sleep 1; done' "$private_lib" '${LD_LIBRARY_PATH:+:"$LD_LIBRARY_PATH"}' "$bar"
    else
        printf 'while :; do mangobar; sleep 1; done'
    fi
}
case "${MANGO_ANLAND_PANEL:-auto}" in
    auto) if command -v mangobar >/dev/null; then args+=(-s "$(panel_command)"); fi ;;
    1) command -v mangobar >/dev/null || die 'explicitly requested mangobar is unavailable'; args+=(-s "$(panel_command)") ;;
    0) ;;
    *) die 'MANGO_ANLAND_PANEL must be auto, 0 or 1' ;;
esac
unset DISPLAY WAYLAND_DISPLAY
# Supervise the compositor instead of exec'ing it: the session lock then belongs
# to this launcher, so panels, Xwayland and clients spawned by the compositor
# never inherit fd 9 and cannot keep the session locked after it exits. Signals
# are forwarded explicitly because a background child ignores SIGINT/SIGQUIT.
env --default-signal=INT,QUIT "$PREFIX/bin/mango" "${args[@]}" <&0 9>&- &
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