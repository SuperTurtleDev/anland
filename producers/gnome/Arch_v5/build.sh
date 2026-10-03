#!/usr/bin/env bash
#
# Build the Arch Linux ARM Anland Mutter and patched Xwayland packages.
#
# Usage:
#   ./build.sh [--nocheck|--noconfirm|--log|--nosign|--force]
#
# WORKDIR overrides the cache; ANLAND_SOURCE_CACHE supplies local archives.
# ANLAND_INSTALL=1 (or INSTALL=1) explicitly installs the resulting packages.
# Default: JOBS=2, build only, no session or global environment changes.
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ ${1:-} == -h || ${1:-} == --help ]]; then
    sed -n '3,10p' "$0"
    exit 0
fi

log()  { printf '\n\033[1;34m==> %s\033[0m\n' "$*"; }
die()  { printf '\033[1;31m[error] %s\033[0m\n' "$*" >&2; exit 1; }

CACHE_ROOT="${XDG_CACHE_HOME:-$HOME/.cache}/anland/mutter-arch"
WORKDIR="${WORKDIR:-$CACHE_ROOT}"
MUTTER_STAGE="$WORKDIR/package"
MUTTER_SRCDEST_DIR="$WORKDIR/sources"
PKGDEST_DIR="$WORKDIR/packages"
MUTTER_OVERLAY_ROOT="$WORKDIR/overlay"
MUTTER_BUILDDIR="$WORKDIR/build"
XWAYLAND_STAGE="$WORKDIR/xwayland-package"
XWAYLAND_SRCDEST_DIR="$WORKDIR/xwayland-sources"
XWAYLAND_BUILDDIR="$WORKDIR/xwayland-build"
SOURCE_CACHE="${ANLAND_SOURCE_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/anland-review/arch-mutter}"
INSTALL="${ANLAND_INSTALL:-${INSTALL:-0}}"
export JOBS="${JOBS:-2}"

validate_environment() {
    local arg tool
    for arg in "$@"; do
        case "$arg" in
            --nocheck|--noconfirm|--log|--nosign|--force) ;;
            *) die "Unsupported option: $arg" ;;
        esac
    done
    [[ "$INSTALL" == 0 || "$INSTALL" == 1 ]] || die 'INSTALL must be 0 or 1'
    [[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || die 'JOBS must be a positive integer'
    [[ $(id -u) != 0 && $(uname -m) == aarch64 ]] || die 'Build as non-root on aarch64'
    [[ -d "$SCRIPT_DIR/mutter/src/backends/anland" && -f "$SCRIPT_DIR/mutter.patch" ]] \
        || die 'Shared backend or patch missing'
    [[ -f "$SCRIPT_DIR/xorg-xwayland.PKGBUILD" && -f "$SCRIPT_DIR/xwayland.patch" ]] \
        || die 'Patched Xwayland PKGBUILD or patch missing'
    for tool in makepkg tar patch sha512sum sha256sum bsdtar; do
        command -v "$tool" >/dev/null || die "Missing tool: $tool"
    done
}

prepare_xwayland_stage() {
    log 'Preparing patched Xwayland makepkg staging directory'
    mkdir -p "$XWAYLAND_STAGE" "$XWAYLAND_SRCDEST_DIR" "$PKGDEST_DIR" "$XWAYLAND_BUILDDIR"
    cp "$SCRIPT_DIR/xorg-xwayland.PKGBUILD" "$XWAYLAND_STAGE/PKGBUILD"
    cp "$SCRIPT_DIR/xwayland.patch" "$XWAYLAND_STAGE/"
}

prepare_mutter_stage() {
    log 'Preparing Mutter makepkg staging directory'
    mkdir -p "$MUTTER_STAGE" "$MUTTER_SRCDEST_DIR" "$PKGDEST_DIR" "$MUTTER_OVERLAY_ROOT"
    # Dereference shared backend links only in the cache, never in the repository.
    rm -rf "${MUTTER_OVERLAY_ROOT:?}/src"
    mkdir -p "$MUTTER_OVERLAY_ROOT/src/backends/anland"
    cp -aL "$SCRIPT_DIR/mutter/src/backends/anland/." "$MUTTER_OVERLAY_ROOT/src/backends/anland/"
    [[ -f "$MUTTER_OVERLAY_ROOT/src/backends/anland/libdisplay_producer/anland_device.c" &&
       -f "$MUTTER_OVERLAY_ROOT/src/backends/anland/common/anland_present_ipc.c" ]] || die 'Incomplete staged backend'
    tar -czf "$MUTTER_STAGE/mutter-overlay.tar.gz" -C "$MUTTER_OVERLAY_ROOT" src
    cp "$SCRIPT_DIR/PKGBUILD" "$SCRIPT_DIR/mutter.patch" "$MUTTER_STAGE/"
    # makepkg verifies cached and downloaded archives against the PKGBUILD SHA512.
    if [[ -f "$SOURCE_CACHE/mutter-50.5.complete.tar.gz" ]]; then
        cp -f "$SOURCE_CACHE/mutter-50.5.complete.tar.gz" "$MUTTER_SRCDEST_DIR/mutter-50.5.tar.gz"
    fi
    if [[ -f "$SOURCE_CACHE/gvdb-b54bc5da.tar.gz" ]]; then
        cp -f "$SOURCE_CACHE/gvdb-b54bc5da.tar.gz" "$MUTTER_SRCDEST_DIR/gvdb-b54bc5da.tar.gz"
    fi
}

run_xwayland_makepkg() (
    cd "$XWAYLAND_STAGE"
    export SRCDEST="$XWAYLAND_SRCDEST_DIR" PKGDEST="$PKGDEST_DIR" BUILDDIR="$XWAYLAND_BUILDDIR"
    export MAKEFLAGS="-j$JOBS"
    makepkg -s --needed --noconfirm --cleanbuild "$@"
)

run_makepkg() (
    cd "$MUTTER_STAGE"
    export SRCDEST="$MUTTER_SRCDEST_DIR" PKGDEST="$PKGDEST_DIR" BUILDDIR="$MUTTER_BUILDDIR"
    export MAKEFLAGS="-j$JOBS"
    # Only build dependencies may be installed without ANLAND_INSTALL=1.
    makepkg -s --needed --noconfirm --cleanbuild "$@"
)

collect_xwayland_packages() {
    local stamp="$1" pkg
    mapfile -t xwayland_packages < <(cd "$XWAYLAND_STAGE" && PKGDEST="$PKGDEST_DIR" makepkg --packagelist)
    [[ ${#xwayland_packages[@]} == 1 ]] || die "Expected one patched Xwayland package, found ${#xwayland_packages[@]}"
    for pkg in "${xwayland_packages[@]}"; do
        [[ -s "$pkg" && "$pkg" -nt "$stamp" ]] || die "Missing or stale package: $pkg"
        bsdtar -tf "$pkg" >/dev/null
        sha256sum "$pkg"
    done
    rm -f -- "$stamp"
}

collect_packages() {
    local stamp="$1" pkg
    mapfile -t mutter_packages < <(cd "$MUTTER_STAGE" && PKGDEST="$PKGDEST_DIR" makepkg --packagelist)
    [[ ${#mutter_packages[@]} == 3 ]] || die "Expected three split Mutter packages, found ${#mutter_packages[@]}"
    for pkg in "${mutter_packages[@]}"; do
        [[ -s "$pkg" && "$pkg" -nt "$stamp" ]] || die "Missing or stale package: $pkg"
        bsdtar -tf "$pkg" >/dev/null
        sha256sum "$pkg"
    done
    rm -f -- "$stamp"
}

install_packages() {
    log 'Installing freshly built Mutter and Xwayland packages (explicitly requested)'
    local pacman_local_config="$WORKDIR/pacman-local.conf"
    cp /etc/pacman.conf "$pacman_local_config"
    if grep -q '^#LocalFileSigLevel = Optional$' "$pacman_local_config"; then
        sed -i 's/^#LocalFileSigLevel = Optional$/LocalFileSigLevel = Optional/' "$pacman_local_config"
    elif grep -qE '^[[:space:]]*LocalFileSigLevel[[:space:]]*=' "$pacman_local_config"; then
        sed -i -E 's/^[[:space:]]*LocalFileSigLevel[[:space:]]*=.*/LocalFileSigLevel = Optional/' "$pacman_local_config"
    elif grep -qE '^\[options\][[:space:]]*$' "$pacman_local_config"; then
        sed -i '/^\[options\][[:space:]]*$/a LocalFileSigLevel = Optional' "$pacman_local_config"
    else
        rm -f -- "$pacman_local_config"
        die 'pacman.conf has no [options] section'
    fi
    if ! sudo pacman --config "$pacman_local_config" -U --noconfirm "$@"; then
        rm -f -- "$pacman_local_config"
        die 'Failed to install the built Mutter/Xwayland packages'
    fi
    rm -f -- "$pacman_local_config"
}

main() {
    validate_environment "$@"
    prepare_xwayland_stage
    prepare_mutter_stage
    local mutter_stamp xwayland_stamp
    local -a mutter_packages xwayland_packages
    xwayland_stamp="$(mktemp "$XWAYLAND_STAGE/build-start.XXXXXX")"
    mutter_stamp="$(mktemp "$MUTTER_STAGE/build-start.XXXXXX")"
    run_xwayland_makepkg "$@"
    collect_xwayland_packages "$xwayland_stamp"
    run_makepkg "$@"
    collect_packages "$mutter_stamp"
    if [[ "$INSTALL" == 1 ]]; then
        install_packages "${xwayland_packages[@]}" "${mutter_packages[@]}"
    fi
    log "Done. Mutter and Xwayland package artifacts: $PKGDEST_DIR (INSTALL=$INSTALL)"
}

main "$@"
