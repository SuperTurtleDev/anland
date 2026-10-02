#!/usr/bin/env bash
#
# Build the Arch Linux ARM Anland Mutter 50.5 port as pacman split packages.
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
    for tool in makepkg tar patch sha512sum sha256sum bsdtar; do
        command -v "$tool" >/dev/null || die "Missing tool: $tool"
    done
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

run_makepkg() (
    cd "$MUTTER_STAGE"
    export SRCDEST="$MUTTER_SRCDEST_DIR" PKGDEST="$PKGDEST_DIR" BUILDDIR="$MUTTER_BUILDDIR"
    export MAKEFLAGS="-j$JOBS"
    # Only build dependencies may be installed without ANLAND_INSTALL=1.
    makepkg -s --needed --noconfirm --cleanbuild "$@"
)

collect_packages() {
    local stamp="$1" pkg
    mapfile -t packages < <(cd "$MUTTER_STAGE" && PKGDEST="$PKGDEST_DIR" makepkg --packagelist)
    [[ ${#packages[@]} == 3 ]] || die "Expected three split packages, found ${#packages[@]}"
    for pkg in "${packages[@]}"; do
        [[ -s "$pkg" && "$pkg" -nt "$stamp" ]] || die "Missing or stale package: $pkg"
        bsdtar -tf "$pkg" >/dev/null
        sha256sum "$pkg"
    done
    rm -f -- "$stamp"
}

install_packages() {
    log 'Installing freshly built Mutter packages (explicitly requested)'
    sudo pacman -U -- "$@"
}

main() {
    validate_environment "$@"
    prepare_mutter_stage
    local stamp
    local -a packages
    stamp="$(mktemp "$MUTTER_STAGE/build-start.XXXXXX")"
    run_makepkg "$@"
    collect_packages "$stamp"
    if [[ "$INSTALL" == 1 ]]; then
        install_packages "${packages[@]}"
    fi
    log "Done. Package artifacts: $PKGDEST_DIR (INSTALL=$INSTALL)"
}

main "$@"