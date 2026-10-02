#!/usr/bin/env bash
# Launch an Anland GNOME session explicitly; never stop an existing session.
set -euo pipefail
sock=${1:-/run/display.sock}
[[ -S $sock ]] || { echo "Display socket missing: $sock" >&2; exit 1; }
for name in gnome-shell gnome-session mutter; do
  if pgrep -u "$(id -u)" -f "(^|/)${name}([[:space:]]|$)" >/dev/null; then
    echo "Existing $name session: refusing a second one" >&2
    exit 1
  fi
done
command -v gnome-session >/dev/null || { echo 'Install gnome-session and patched Mutter first' >&2; exit 1; }
if ! pacman -Q mutter >/dev/null 2>&1; then
  echo 'Mutter is not installed; install the built package explicitly first' >&2
  exit 1
fi
export ANLAND=1 ANLAND_SOCKET=$sock
export XDG_SESSION_TYPE=wayland XDG_SESSION_DESKTOP=gnome XDG_CURRENT_DESKTOP=GNOME
export GNOME_SHELL_SESSION_MODE=gnome WAYLAND_DISPLAY=wayland-anland GNOME_WAYLAND_DISPLAY=wayland-anland
export ANLAND_DRM_DEVICE=${ANLAND_DRM_DEVICE:-/dev/dri/renderD128}
export MESA_LOADER_DRIVER_OVERRIDE=${MESA_LOADER_DRIVER_OVERRIDE:-kgsl}
export TURNIP_KMD=${TURNIP_KMD:-kgsl}
export GALLIUM_DRIVER=${GALLIUM_DRIVER:-freedreno}
export FD_FORCE_KGSL=${FD_FORCE_KGSL:-1}
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/$(id -u)}
[[ -d $XDG_RUNTIME_DIR && -w $XDG_RUNTIME_DIR ]] || {
  echo "Usable XDG_RUNTIME_DIR required: $XDG_RUNTIME_DIR" >&2; exit 1;
}
# Arch's systemd-user GNOME units do not inherit ad-hoc shell environment.
# Environment import only when explicitly requested; avoid modifying another
# existing desktop session's activation environment by default.
if [[ ${ANLAND_IMPORT_SESSION_ENV:-0} == 1 ]]; then
  command -v dbus-update-activation-environment >/dev/null || exit 1
  dbus-update-activation-environment --systemd ANLAND ANLAND_SOCKET ANLAND_DRM_DEVICE \
    MESA_LOADER_DRIVER_OVERRIDE TURNIP_KMD GALLIUM_DRIVER FD_FORCE_KGSL \
    XDG_SESSION_TYPE XDG_SESSION_DESKTOP XDG_CURRENT_DESKTOP \
    GNOME_SHELL_SESSION_MODE WAYLAND_DISPLAY GNOME_WAYLAND_DISPLAY
fi
exec gnome-session
