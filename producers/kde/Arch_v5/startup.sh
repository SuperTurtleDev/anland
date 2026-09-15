#!/bin/bash
#
# startup.sh - start kwin_wayland through its D-Bus wrapper with the anland backend
# (Arch Linux / Arch Linux ARM, MediaTek primary-node DRM with llvmpipe).
# Usage:
#   ./startup.sh [socket path]          (default /run/display.sock)
# Env:
#   KWIN_BIN     path to kwin_wayland_wrapper (default: the one in PATH)
#   ANLAND_SOCKET, ANLAND_DRM_DEVICE, XWAYLAND_GBM_DEVICE
set -eu
if [ -z "${DBUS_SESSION_BUS_ADDRESS:-}" ]; then
    exec dbus-run-session -- "$0" "$@"
fi


SOCK="${1:-${ANLAND_SOCKET:-/run/display.sock}}"
KWIN_BIN="${KWIN_BIN:-kwin_wayland_wrapper}"

command -v "$KWIN_BIN" >/dev/null 2>&1 || {
    echo "kwin_wayland_wrapper not found in PATH; set KWIN_BIN explicitly" >&2
    exit 1
}

export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
if [ ! -d "$XDG_RUNTIME_DIR" ]; then
    # Fresh DroidSpaces containers commonly lack a logind-created /run/user
    # directory. Keep the Wayland runtime directory private without requiring
    # a password or widening the launcher privileges.
    XDG_RUNTIME_DIR="$HOME/.local/run/anland-$(id -u)"
    mkdir -p "$XDG_RUNTIME_DIR"
    chmod 0700 "$XDG_RUNTIME_DIR"
fi

# MediaTek exposes only the primary DRM node here. Prevent inherited Adreno
# overrides from selecting kgsl and make the software fallback explicit.
unset MESA_LOADER_DRIVER_OVERRIDE GALLIUM_DRIVER FD_FORCE_KGSL
unset MESA_VK_DEVICE_SELECT_FORCE_DEFAULT_DEVICE FD_DEV_FEATURES
export LIBGL_ALWAYS_SOFTWARE="${LIBGL_ALWAYS_SOFTWARE:-1}"
export ANLAND_SKIP_IMPLICIT_SYNC_WAIT="${ANLAND_SKIP_IMPLICIT_SYNC_WAIT:-1}"
export XCURSOR_THEME=breeze_cursors
export XCURSOR_SIZE=24
export QT_QPA_PLATFORM=wayland
export XDG_SESSION_TYPE=wayland
export XDG_CURRENT_DESKTOP=KDE
export KDE_FULL_SESSION=true
export KDE_SESSION_VERSION=6
export ANLAND_SOCKET="$SOCK"
export ANLAND_DRM_DEVICE="${ANLAND_DRM_DEVICE:-/dev/dri/card0}"
# Xwayland inherits the primary node for its software GBM fallback.
export XWAYLAND_GBM_DEVICE="${XWAYLAND_GBM_DEVICE:-$ANLAND_DRM_DEVICE}"
unset DISPLAY WAYLAND_DISPLAY
if pgrep -x kwin_wayland >/dev/null 2>&1; then
    echo "ERROR: an existing kwin_wayland must stop before starting this session" >&2
    exit 1
fi

# A dead compositor can leave a wayland-* socket behind. Timestamp this launch
# and accept only sockets created after KWin starts; never bind Plasma to stale
# state inherited through droidspaces run.
WAYLAND_START_MARKER="$XDG_RUNTIME_DIR/.anland-wayland-start-$$"
touch "$WAYLAND_START_MARKER"

echo "==> $KWIN_BIN --anland --xwayland (socket=$SOCK, drm=$ANLAND_DRM_DEVICE)"
"$KWIN_BIN" --anland --xwayland &
KWIN_PID=$!
KDED_PID=""
PLASMASHELL_PID=""
cleanup() {
    [ -z "$PLASMASHELL_PID" ] || kill "$PLASMASHELL_PID" 2>/dev/null || true
    [ -z "$KDED_PID" ] || kill "$KDED_PID" 2>/dev/null || true
    rm -f "$WAYLAND_START_MARKER"
    kill "$KWIN_PID" 2>/dev/null || true
}
terminate() {
    exit 0
}
trap cleanup EXIT
trap terminate INT TERM


WAYLAND_SOCKET=""
WAYLAND_INFO="$(command -v wayland-info || true)"
for _ in $(seq 1 30); do
    sleep 1
    kill -0 "$KWIN_PID" 2>/dev/null || break
    for wl in "$XDG_RUNTIME_DIR"/wayland-*; do
        [ -S "$wl" ] || continue
        [ "$wl" -nt "$WAYLAND_START_MARKER" ] || continue
        case "$wl" in *.lock) continue ;; esac
        candidate="$(basename "$wl")"
        if [ -n "$WAYLAND_INFO" ] &&
           ! WAYLAND_DISPLAY="$candidate" timeout 2 "$WAYLAND_INFO" >/dev/null 2>&1; then
            continue
        fi
        WAYLAND_SOCKET="$candidate"
        break 2
    done
done
rm -f "$WAYLAND_START_MARKER"

if [ -z "$WAYLAND_SOCKET" ]; then
    echo "ERROR: no connectable wayland socket found; kwin_wayland may have failed" >&2
    wait "$KWIN_PID"
    exit 1
fi

echo "==> wayland socket: $WAYLAND_SOCKET"
export WAYLAND_DISPLAY="$WAYLAND_SOCKET"

kcminit_startup || true

kded6 &
KDED_PID=$!
KDED_READY=""
for _ in $(seq 1 30); do
    if qdbus6 org.kde.kded6 >/dev/null 2>&1; then
        KDED_READY=1
        break
    fi
    kill -0 "$KDED_PID" 2>/dev/null || break
    sleep 1
done
if [ -z "$KDED_READY" ]; then
    echo "ERROR: kded6 did not register on D-Bus" >&2
    exit 1
fi

plasmashell --replace &
PLASMASHELL_PID=$!
PLASMA_READY=""
for _ in $(seq 1 30); do
    if qdbus6 org.kde.plasmashell /PlasmaShell >/dev/null 2>&1; then
        PLASMA_READY=1
        break
    fi
    kill -0 "$PLASMASHELL_PID" 2>/dev/null || break
    sleep 1
done
if [ -z "$PLASMA_READY" ]; then
    echo "ERROR: plasmashell did not register on D-Bus" >&2
    exit 1
fi

echo "==> plasmashell ready"
set +e
wait -n "$KWIN_PID" "$PLASMASHELL_PID"
SESSION_STATUS=$?
set -e
if [ "$SESSION_STATUS" -eq 0 ]; then
    SESSION_STATUS=1
fi
echo "ERROR: KWin wrapper or plasmashell exited; restarting session" >&2
exit "$SESSION_STATUS"
