#!/usr/bin/env bash
# Start the Denial Anland desktop inside the Android-side Ubuntu 26.04 userland.
#
# Building and installing never start a session. Run this script explicitly.
#
#   startup.sh [SOCKET]        start the full desktop on SOCKET
#   startup.sh --wm-only       compositor + embedded shell, no desktop services
#   startup.sh --check         validate files, devices and permissions only
#   startup.sh --version       print the compositor version
#
# Environment:
#   ANLAND_SOCKET             daemon socket (default /run/display.sock)
#   ANLAND_DRM_DEVICE         render node (default /dev/dri/renderD128)
#   DENIAL_ANLAND_XWAYLAND    1 to start Xwayland (default: full desktop only)
#   DENIAL_PREFIX             installed prefix when this file is not in bin/
#   DENIAL_SHELL_PROFILE      desktop|mobile (default desktop)
#   DENIAL_SUPER_MODIFIER     super|alt (default super)
#   DENIAL_ANLAND_OUTPUT_CONFIG, XDG_CONFIG_HOME, XDG_RUNTIME_DIR
#
# Existing Mesa/GBM settings are preserved: this launcher does not choose or
# override a GPU driver. The consumer environment must provide a working
# EGL/GBM stack for the selected render node.
set -euo pipefail

fail() { printf 'denial-anland: %s\n' "$*" >&2; exit 1; }
log() { printf 'denial-anland: %s\n' "$*" >&2; }

usage() {
    printf '%s\n' \
        'Usage: startup.sh [SOCKET] [--check|--wm-only|--version|--help]' \
        '' \
        'Run the Denial Anland desktop. The compositor retries the daemon' \
        'socket when the consumer is not ready yet.' \
        '  --check     validate files, devices and permissions; start nothing' \
        '  --wm-only   compositor and embedded shell only' \
        '  --version   print the deniald version' \
        '' \
        'Environment: ANLAND_SOCKET, ANLAND_DRM_DEVICE, DENIAL_ANLAND_XWAYLAND,' \
        'DENIAL_PREFIX, DENIAL_SHELL_PROFILE, DENIAL_SUPER_MODIFIER,' \
        'DENIAL_ANLAND_OUTPUT_CONFIG, XDG_CONFIG_HOME, XDG_RUNTIME_DIR.'
}

launcher="$(readlink -f -- "${BASH_SOURCE[0]}")" || fail 'cannot resolve the launcher path'
launcher_dir="$(dirname -- "$launcher")"
original_args=("$@")

mode=run
full=1
socket_arg=''
extra_args=()
while (($#)); do
    case "$1" in
        --check) mode=check ;;
        --wm-only) full=0 ;;
        --version | -V) mode=version ;;
        --help | -h) usage; exit 0 ;;
        --) shift; extra_args=("$@"); break ;;
        -*) fail "unsupported argument: $1" ;;
        *)
            [[ -z "$socket_arg" ]] || fail 'only one socket path may be given'
            socket_arg=$1
            ;;
    esac
    shift
done

if [[ -n "${DENIAL_PREFIX:-}" ]]; then
    [[ "$DENIAL_PREFIX" == /* ]] || fail 'DENIAL_PREFIX must be absolute'
    prefix=$DENIAL_PREFIX
elif [[ -d "$launcher_dir/../lib/denial/flutter" ]]; then
    prefix="$(cd -- "$launcher_dir/.." && pwd)"
else
    prefix=/usr
fi
base="$prefix/bin"
bundle="$prefix/lib/denial/flutter"
deniald="$base/deniald"

[[ -x "$deniald" ]] || fail "missing compositor: $deniald"
if [[ "$mode" == version ]]; then
    exec "$deniald" --version
fi

for file in \
    "$deniald" \
    "$prefix/lib/libanland_denial_bridge.so" \
    "$prefix/lib/libdisplay_producer.so" \
    "$bundle/lib/libapp.so" \
    "$bundle/lib/libflutter_engine.so" \
    "$bundle/data/icudtl.dat"; do
    [[ -r "$file" ]] || fail "missing runtime file: $file"
done
[[ -d "$bundle/data/flutter_assets" ]] || fail "missing shell assets: $bundle/data/flutter_assets"

if ((full)); then
    for file in \
        "$base/denial-settings" \
        "$base/denial-polkit-agent" \
        "$base/denial-portal"; do
        [[ -x "$file" ]] || fail "the full desktop needs $file; use --wm-only otherwise"
    done
fi

for file in "$deniald" "$prefix/lib/libanland_denial_bridge.so" "$prefix/lib/libdisplay_producer.so"; do
    deps="$(ldd "$file" 2>&1)" || fail "cannot inspect dependencies of $file"
    [[ "$deps" != *'not found'* ]] || fail "missing dependency for $file: $deps"
done

if [[ -n "$socket_arg" ]]; then
    [[ -z "${ANLAND_SOCKET:-}" || "${ANLAND_SOCKET:-}" == "$socket_arg" ]] \
        || fail 'socket argument conflicts with ANLAND_SOCKET'
    export ANLAND_SOCKET="$socket_arg"
fi
export ANLAND_SOCKET="${ANLAND_SOCKET:-/run/display.sock}"

render="${ANLAND_DRM_DEVICE:-/dev/dri/renderD128}"
[[ -c "$render" && -r "$render" && -w "$render" ]] || fail "render node is not accessible: $render"
export ANLAND_DRM_DEVICE="$render"

xwayland=${DENIAL_ANLAND_XWAYLAND:-$full}
[[ "$xwayland" == 0 || "$xwayland" == 1 ]] || fail 'DENIAL_ANLAND_XWAYLAND must be 0 or 1'
if [[ "$xwayland" == 1 ]]; then
    command -v Xwayland >/dev/null 2>&1 \
        || fail 'Xwayland is not installed; set DENIAL_ANLAND_XWAYLAND=0 to skip it'
    export XWAYLAND_GBM_DEVICE="${XWAYLAND_GBM_DEVICE:-$render}"
fi

if [[ "$mode" == check ]]; then
    [[ -S "$ANLAND_SOCKET" ]] || fail "missing daemon socket: $ANLAND_SOCKET"
else
    [[ -S "$ANLAND_SOCKET" ]] \
        || log "daemon socket is not ready yet ($ANLAND_SOCKET); the compositor will keep retrying"
fi

for process in deniald Hyprland kwin_wayland gnome-shell mango sway weston gamescope; do
    if pgrep -u "$(id -u)" -x "$process" >/dev/null 2>&1; then
        fail "an existing $process session is running; refusing to take over"
    fi
done

export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
[[ -d "$XDG_RUNTIME_DIR" && -O "$XDG_RUNTIME_DIR" && ! -L "$XDG_RUNTIME_DIR" ]] \
    || fail "invalid runtime directory: $XDG_RUNTIME_DIR"
[[ "$(stat -c %a "$XDG_RUNTIME_DIR")" == 700 ]] || fail 'XDG_RUNTIME_DIR must have mode 0700'

export XDG_CURRENT_DESKTOP=Denial XDG_SESSION_DESKTOP=Denial DESKTOP_SESSION=Denial
export XDG_SESSION_TYPE=wayland
export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-wayland}"
export QT_QPA_PLATFORMTHEME="${QT_QPA_PLATFORMTHEME-xdgdesktopportal}"
export GDK_BACKEND="${GDK_BACKEND:-wayland}"
export DENIAL_SHELL_PROFILE="${DENIAL_SHELL_PROFILE:-desktop}"
export DENIAL_SUPER_MODIFIER="${DENIAL_SUPER_MODIFIER:-super}"
[[ "$DENIAL_SUPER_MODIFIER" == super || "$DENIAL_SUPER_MODIFIER" == alt ]] \
    || fail 'DENIAL_SUPER_MODIFIER must be super or alt'
export DENIAL_ANLAND_DESKTOP=$full

export DENIAL_COMPOSITOR_BINARY="${DENIAL_COMPOSITOR_BINARY:-$deniald}"
export DENIAL_CONTROL_TOOL="${DENIAL_CONTROL_TOOL:-$base/denialctl}"
export DENIAL_SETTINGS_BINARY="${DENIAL_SETTINGS_BINARY:-$base/denial-settings}"
export DENIAL_POLKIT_BINARY="${DENIAL_POLKIT_BINARY:-$base/denial-polkit-agent}"
export DENIAL_PLUGINS_BINARY="${DENIAL_PLUGINS_BINARY:-$base/denial-plugins}"

output_config="${DENIAL_ANLAND_OUTPUT_CONFIG:-${XDG_CONFIG_HOME:-$HOME/.config}/denial/anland-outputs.conf}"
[[ "$output_config" == /* ]] || fail 'the output configuration path must be absolute'
[[ ! -L "$output_config" ]] || fail 'the output configuration must not be a symlink'

if [[ "$mode" == check ]]; then
    printf 'PASS: runtime files, libraries, render node and session inputs.\n'
    printf 'No consumer handshake, rendering or desktop service was started.\n'
    exit 0
fi

if [[ ! -e "$output_config" ]]; then
    (umask 077 && mkdir -p -- "$(dirname -- "$output_config")" &&
        set -o noclobber && printf '# Denial Anland logical output preferences\n' > "$output_config") \
        || fail "cannot create output configuration: $output_config"
fi
[[ -f "$output_config" && -O "$output_config" && -r "$output_config" && -w "$output_config" ]] \
    || fail 'the output configuration must be an owned, readable and writable file'

command -v dbus-run-session >/dev/null 2>&1 || fail 'dbus-run-session is required'
if [[ -z "${DBUS_SESSION_BUS_ADDRESS:-}" ]]; then
    if [[ -S "$XDG_RUNTIME_DIR/bus" ]]; then
        export DBUS_SESSION_BUS_ADDRESS="unix:path=$XDG_RUNTIME_DIR/bus"
    else
        exec dbus-run-session -- "$launcher" "${original_args[@]}"
    fi
fi

command -v flock >/dev/null 2>&1 || fail 'flock is required'
exec 9>"$XDG_RUNTIME_DIR/anland-producer-session.lock"
flock -n 9 || fail 'another producer owns the Anland session lock'

args=(
    --backend anland
    --wayland
    --render-device "$render"
    --flutter-bundle "$bundle"
    --output-config "$output_config"
)
if ((full)); then
    log 'starting the Denial desktop: shell, Polkit, portal and applications'
else
    args+=(--no-polkit-agent)
    log 'starting the Denial compositor and embedded shell only'
fi
[[ "$xwayland" == 1 ]] || args+=(--no-xwayland)
[[ ${#extra_args[@]} -eq 0 ]] || args+=("${extra_args[@]}")

exec "$deniald" "${args[@]}"
