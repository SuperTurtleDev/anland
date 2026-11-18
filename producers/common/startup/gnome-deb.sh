#!/bin/bash
SOCK="${1:-/run/display.sock}"
# Never terminate an existing graphical session implicitly.
for process_name in gnome-shell gnome-session mutter; do
    if pgrep -u "$(id -u)" -f "(^|/)${process_name}([[:space:]]|$)" >/dev/null; then
        echo "An existing $process_name session is running; refusing to start another." >&2
        exit 1
    fi
done
[ -S "$SOCK" ] || { echo "Display socket not found: $SOCK" >&2; exit 1; }
export ANLAND=1
export ANLAND_SOCKET="$SOCK"
export QT_QPA_PLATFORM=wayland XDG_CURRENT_DESKTOP=GNOME XDG_SESSION_DESKTOP=gnome XDG_SESSION_TYPE=wayland GNOME_SHELL_SESSION_MODE=gnome
export WAYLAND_DISPLAY=wayland-anland GNOME_WAYLAND_DISPLAY=wayland-anland
export ANLAND_DRM_DEVICE=/dev/dri/renderD128
export MESA_LOADER_DRIVER_OVERRIDE=kgsl TURNIP_KMD=kgsl GALLIUM_DRIVER=freedreno FD_FORCE_KGSL=1
export XDG_RUNTIME_DIR=/run/user/$(id -u)
sudo mkdir -p $XDG_RUNTIME_DIR
sudo chown $(id -un):$(id -gn) $XDG_RUNTIME_DIR
chmod 700 $XDG_RUNTIME_DIR
sudo mkdir -p /tmp/.X11-unix
sudo chmod 1777 /tmp/.X11-unix
exec gnome-session
