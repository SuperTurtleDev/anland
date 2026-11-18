#!/usr/bin/env bash
# Run the full Anland desktop by default. Building/installing never starts it.
# Using DENIAL_SUPER_MODIFIER=alt variables allows you to switch the shortcut key from SUPER to ALT
set -euo pipefail
fail() { printf 'denial: %s\n' "$*" >&2; exit 1; }
launcher=$(readlink -f -- "${BASH_SOURCE[0]}") || fail 'Cannot resolve the launcher.'
bin=$(dirname -- "$launcher")
# A checkout's startup.sh uses the installed runtime; packaged launchers may
# also run from an extracted prefix for non-graphical validation.
[[ ${bin##*/} == bin ]] || bin=/usr/bin
prefix=$(dirname -- "$bin")
base="$prefix/lib/denial-anland/wm"
bundle="$base/shell"
desktop="$prefix/lib/denial-anland/desktop"
mode=run
full=1
original_args=("$@")
while (( $# )); do
    case "$1" in
        --check) mode=check ;;
        --wm-only) full=0 ;;
        --version | -V) exec "$base/deniald" --version ;;
        --help | -h)
            printf '%s\n' 'Usage: denial [--check] [--wm-only] [--version]' \
                'Run denial to start the full desktop (requires denial-desktop).' \
                '--wm-only starts just the compositor and embedded shell.' \
                '--check validates files/permissions without starting a session.' \
                'Shell profile: DENIAL_SHELL_PROFILE=desktop|mobile (default desktop).' \
                'Modifier: DENIAL_SUPER_MODIFIER=super|alt (default super; alt swaps Alt/Super for shortcuts and clients).' \
                'Display scale is saved in ~/.config/denial/anland-outputs.conf after Keep.' \
                'Optional environment: ANLAND_SOCKET, ANLAND_DRM_DEVICE, XDG_RUNTIME_DIR, DENIAL_ANLAND_XWAYLAND=0|1, DENIAL_ANLAND_OUTPUT_CONFIG.'
            exit 0 ;;
        *) fail "Unsupported argument: $1" ;;
    esac
    shift
done
for file in "$base/deniald" "$base/libdisplay_producer.so" "$base/libanland_denial_bridge.so" "$bundle/lib/libapp.so" "$bundle/lib/libflutter_engine.so" "$bundle/data/icudtl.dat"; do
    [[ -r "$file" ]] || fail "Missing runtime file: $file"
done
[[ -x "$base/deniald" && -d "$bundle/data/flutter_assets" ]] || fail 'Incomplete compositor/shell installation.'
if [[ $full == 1 ]]; then
    for file in "$bin/denial-settings" "$bin/denial-polkit-agent" "$bin/denial-portal" "$bin/denial-plugins" "$bin/denial-plugin-manager"; do
        [[ -x "$file" ]] || fail 'The full desktop requires denial-desktop; install it or use --wm-only.'
    done
    for app in settings polkit plugins; do
        for file in "$desktop/$app/lib/libapp.so" "$desktop/$app/lib/libflutter_engine.so" "$desktop/$app/data/icudtl.dat"; do
            [[ -r "$file" ]] || fail "Incomplete denial-desktop bundle: $file"
        done
        [[ -d "$desktop/$app/data/flutter_assets" ]] || fail "Missing desktop assets: $app"
    done
fi
for file in "$base/deniald" "$base/libanland_denial_bridge.so" "$bundle/lib/libflutter_engine.so"; do
    deps=$(ldd "$file" 2>&1) || fail "Dependency inspection failed: $file"
    [[ "$deps" != *'not found'* ]] || fail "Missing dependency for $file: $deps"
done
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
[[ -d "$XDG_RUNTIME_DIR" && -O "$XDG_RUNTIME_DIR" && ! -L "$XDG_RUNTIME_DIR" ]] || fail "Invalid runtime directory: $XDG_RUNTIME_DIR"
[[ "$(stat -c %a "$XDG_RUNTIME_DIR")" == 700 ]] || fail 'XDG_RUNTIME_DIR must have mode 0700.'
export ANLAND_SOCKET="${ANLAND_SOCKET:-/run/display.sock}"
# Boot ordering belongs to deniald's deferred connector, not the launcher.
# --check stays a strict, non-starting preflight; a real session waits/reopens
# in its signal-aware event loop when the daemon/consumer appears later.
if [[ $mode == check ]]; then
    [[ -S "$ANLAND_SOCKET" ]] || fail "Missing daemon socket: $ANLAND_SOCKET"
elif [[ ! -S "$ANLAND_SOCKET" ]]; then
    printf 'denial: daemon socket not ready (%s); the compositor will keep retrying.\n' "$ANLAND_SOCKET" >&2
fi
render="${ANLAND_DRM_DEVICE:-/dev/dri/renderD128}"
[[ -c "$render" && -r "$render" && -w "$render" ]] || fail "Render device is not accessible: $render"
# Preserve the caller's proven Mesa/GBM settings; never change clocks or drivers.
for process in deniald niri niri-anland Hyprland kwin_wayland gnome-shell mango sway gamescope weston; do
    if pgrep -u "$(id -u)" -x "$process" >/dev/null; then
        fail "Existing $process session; refusing to take over."
    fi
done
args=(--backend anland --render-device "$render" --wayland --flutter-bundle "$bundle")
export DENIAL_ANLAND_DESKTOP=$full
[[ $full == 1 ]] || args+=(--no-polkit-agent)
xwayland=${DENIAL_ANLAND_XWAYLAND:-$full}
[[ $xwayland == 0 || $xwayland == 1 ]] || fail 'DENIAL_ANLAND_XWAYLAND must be 0 or 1.'
if [[ $xwayland == 1 ]]; then
    command -v Xwayland >/dev/null || fail 'Install xorg-xwayland or set DENIAL_ANLAND_XWAYLAND=0.'
    export XWAYLAND_GBM_DEVICE="${XWAYLAND_GBM_DEVICE:-$render}"
else
    args+=(--no-xwayland)
fi
export XDG_CURRENT_DESKTOP=Denial XDG_SESSION_DESKTOP=Denial DESKTOP_SESSION=Denial
export XDG_SESSION_TYPE=wayland
export DENIAL_SHELL_PROFILE="${DENIAL_SHELL_PROFILE:-desktop}"
export DENIAL_SUPER_MODIFIER="${DENIAL_SUPER_MODIFIER:-super}"
[[ $DENIAL_SUPER_MODIFIER == super || $DENIAL_SUPER_MODIFIER == alt ]] || fail 'DENIAL_SUPER_MODIFIER must be super or alt.'
output_config="${DENIAL_ANLAND_OUTPUT_CONFIG:-${XDG_CONFIG_HOME:-$HOME/.config}/denial/anland-outputs.conf}"
args+=(--output-config "$output_config")
export DENIAL_COMPOSITOR_BINARY="${DENIAL_COMPOSITOR_BINARY:-$base/deniald}"
export DENIAL_CONTROL_TOOL="${DENIAL_CONTROL_TOOL:-$bin/denialctl}"
export DENIAL_SETTINGS_BINARY="${DENIAL_SETTINGS_BINARY:-$bin/denial-settings}"
export DENIAL_POLKIT_BINARY="${DENIAL_POLKIT_BINARY:-$bin/denial-polkit-agent}"
export DENIAL_PLUGINS_BINARY="${DENIAL_PLUGINS_BINARY:-$bin/denial-plugins}"
export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-wayland}"
export QT_QPA_PLATFORMTHEME="${QT_QPA_PLATFORMTHEME-xdgdesktopportal}"
export GDK_BACKEND="${GDK_BACKEND:-wayland}"
command -v dbus-run-session >/dev/null || fail 'Install dbus for session activation.'
if [[ "$mode" == check ]]; then
    printf 'PASS: runtime files, links, permissions and session exclusivity (desktop=%s).\n' "$full"
    printf '%s\n' 'No consumer handshake, D-Bus startup, rendering or desktop services were started.'
    exit 0
fi
# Create only on an explicitly requested real session, never build/install/--check.
[[ ! -L $output_config ]] || fail 'Output preferences must not be a symlink.'
if [[ ! -e $output_config ]]; then
    (umask 077; mkdir -p -- "$(dirname -- "$output_config")" &&
        set -o noclobber && printf '# Denial Anland logical output preferences\n' > "$output_config") || fail 'Cannot create output preferences.'
fi
[[ -f $output_config && -O $output_config && -r $output_config && -w $output_config ]] || fail 'Output preferences must be a readable/writable file owned by this user.'
# Reuse the user manager's bus when available; otherwise create a session bus.
# Re-enter before taking the lock, so the inherited descriptor cannot self-conflict.
if [[ -z "${DBUS_SESSION_BUS_ADDRESS:-}" ]]; then
    if [[ -S "$XDG_RUNTIME_DIR/bus" ]]; then
        export DBUS_SESSION_BUS_ADDRESS="unix:path=$XDG_RUNTIME_DIR/bus"
    else
        exec dbus-run-session -- "$launcher" "${original_args[@]}"
    fi
fi
exec 9>"$XDG_RUNTIME_DIR/anland-producer-session.lock"
flock -n 9 || fail 'Another producer owns the Anland session lock.'
if [[ $full == 1 ]]; then
    printf 'Starting Denial desktop: shell, Polkit, portals and desktop applications.\n' >&2
else
    printf 'Starting Denial compositor and embedded shell only.\n' >&2
fi
exec "$base/deniald" "${args[@]}"
