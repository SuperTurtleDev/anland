#!/bin/bash
# Native path: kwin_wayland IS the top-level compositor, talking to the display
# daemon directly through its built-in "anland" backend (--anland). There is no
# weston layer and no nested kwin — kwin replaces both. The patched kwin_wayland
# must be installed (see kdefix/build.sh, which builds the .deb with the anland
# backend baked in).
SOCK="${1:-/run/display.sock}"

for process in kwin_wayland plasmashell startplasma-wayland; do
    if pgrep -u "$(id -u)" -f "(^|/)${process}([[:space:]]|$)" >/dev/null; then
        echo "An existing $process session is running; refusing to start another." >&2
        exit 1
    fi
done
[ -S "$SOCK" ] || { echo "Display socket not found: $SOCK" >&2; exit 1; }
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
mkdir -p "$XDG_RUNTIME_DIR"; chmod 0700 "$XDG_RUNTIME_DIR"
unset DISPLAY
export ANLAND_SOCKET="$SOCK"
export ANLAND=1
export ANLAND_DRM_DEVICE=/dev/dri/renderD128
export ANLAND_SKIP_IMPLICIT_SYNC_WAIT=1
export MESA_LOADER_DRIVER_OVERRIDE=kgsl GALLIUM_DRIVER=kgsl FD_FORCE_KGSL=1
export QT_QPA_PLATFORM=wayland
exec dbus-run-session startplasma-wayland
