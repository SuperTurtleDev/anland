#!/usr/bin/env bash
#
# Build the Arch Linux ARM anland niri and Xwayland ports as pacman packages.
#
# Usage:
#   ./build.sh [additional makepkg options]
#
# Niri and Smithay are fetched from their current upstream default branches.
# NIRI_PATCH and XWAYLAND_PATCH override the integration patches.
# BUILD_XWAYLAND=0 explicitly skips rebuilding an already patched Xwayland.
# Builds do not install compositor packages by default. ANLAND_INSTALL=1 opts
# into installation; INSTALL remains supported as an explicit legacy override.
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [[ "$#" -gt 0 && ( "$1" == '-h' || "$1" == '--help' ) ]]; then
    sed -n '4,9p' "$0"
    exit 0
fi
log()  { printf '\n\033[1;34m==> %s\033[0m\n' "$*"; }
die()  { printf '\033[1;31m[error] %s\033[0m\n' "$*" >&2; exit 1; }

# Extra arguments cannot bypass explicit installation or source verification.
for arg in "$@"; do
    case "$arg" in
        --nocheck|--noconfirm|--log|--nosign|--needed|--force|--syncdeps|--clean|--cleanbuild|-f|-s|-c|-C) ;;
        *) die "Unsupported makepkg option: $arg (use ANLAND_INSTALL=1 to install)" ;;
    esac
done
[[ "${JOBS:-$(nproc)}" =~ ^[1-9][0-9]*$ ]] || die 'JOBS must be a positive integer'

[[ "$(id -u)" -ne 0 ]] || die 'makepkg must run as an unprivileged user'
command -v makepkg >/dev/null 2>&1 || die 'makepkg is required (install base-devel)'
[[ "$(uname -m)" == 'aarch64' ]] || die 'this package must be built natively on Arch Linux ARM (aarch64)'

ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"
# Same layout as KDE's Arch_v5/kwin: a version-local entry to the one shared backend.
BACKEND_SRC="$SCRIPT_DIR/niri"
NIRI_PATCH="${NIRI_PATCH:-$SCRIPT_DIR/niri.patch}"
XWAYLAND_PATCH="${XWAYLAND_PATCH:-$SCRIPT_DIR/xwayland.patch}"
XWAYLAND_PKGFILE="$SCRIPT_DIR/xorg-xwayland.PKGBUILD"
[[ -f "$NIRI_PATCH" ]] || die "niri patch not found: $NIRI_PATCH"
[[ -f "$XWAYLAND_PATCH" ]] || die "Xwayland patch not found: $XWAYLAND_PATCH"
[[ -f "$XWAYLAND_PKGFILE" ]] || die "Xwayland PKGBUILD not found: $XWAYLAND_PKGFILE"
[[ -f "$BACKEND_SRC/mod.rs" ]] || die "backend sources not found: $BACKEND_SRC"

CACHE_ROOT="${XDG_CACHE_HOME:-$HOME/.cache}/anland/niri-arch"
WORKDIR="${WORKDIR:-${ANLAND_NIRI_CACHE:-$CACHE_ROOT}}"
NIRI_STAGE="$WORKDIR/niri-package"
XWAYLAND_STAGE="$WORKDIR/xwayland-package"
NIRI_OVERLAY_ROOT="$WORKDIR/niri-overlay"
NIRI_SRCDEST_DIR="$WORKDIR/niri-sources"
XWAYLAND_SRCDEST_DIR="$WORKDIR/xwayland-sources"
PKGDEST_DIR="$WORKDIR/packages"
BUILD_PKGDEST_DIR="$WORKDIR/package-build"
NIRI_BUILDDIR="$WORKDIR/niri-build"
XWAYLAND_BUILDDIR="$WORKDIR/xwayland-build"
PACMAN_LOCAL_CONFIG="$WORKDIR/pacman-local.conf"

prepare_xwayland_stage() {
    log 'Preparing Xwayland makepkg staging directory'
    rm -rf "$XWAYLAND_STAGE"
    mkdir -p "$XWAYLAND_STAGE" "$XWAYLAND_SRCDEST_DIR" "$PKGDEST_DIR" "$XWAYLAND_BUILDDIR"
    install -m644 "$XWAYLAND_PKGFILE" "$XWAYLAND_STAGE/PKGBUILD"
    install -m644 "$XWAYLAND_PATCH" "$XWAYLAND_STAGE/xwayland.patch"


}

prepare_niri_stage() {
    log 'Preparing niri makepkg staging directory'
    rm -rf "$NIRI_STAGE" "$NIRI_OVERLAY_ROOT"
    mkdir -p "$NIRI_STAGE" "$NIRI_OVERLAY_ROOT/anland-overlay/producers/niri" "$NIRI_SRCDEST_DIR" "$PKGDEST_DIR" "$NIRI_BUILDDIR"
    install -m644 "$SCRIPT_DIR/PKGBUILD" "$NIRI_STAGE/PKGBUILD"
    install -m644 "$NIRI_PATCH" "$NIRI_STAGE/niri.patch"
    install -m644 "$SCRIPT_DIR/smithay.patch" "$NIRI_STAGE/smithay.patch"

    # Match legacy staging: dereference the one shared backend only in the cache.
    # The bridge's CMake root also needs these common producer build inputs.
    cp -aL "$BACKEND_SRC" "$NIRI_OVERLAY_ROOT/anland-overlay/producers/niri/anland_backend"
    cp -aL "$ROOT/CMakeLists.txt" "$ROOT/common" "$ROOT/daemon" \
        "$ROOT/libdisplay_daemon" "$ROOT/libdisplay_consumer" "$ROOT/libdisplay_producer" \
        "$NIRI_OVERLAY_ROOT/anland-overlay/"
    tar --sort=name --mtime=@0 --owner=0 --group=0 --numeric-owner \
        --exclude='__pycache__' -chf "$NIRI_STAGE/anland-overlay.tar" \
        -C "$NIRI_OVERLAY_ROOT" anland-overlay

    log 'makepkg will resolve the current Niri and Smithay default branches'
}

export JOBS="${JOBS:-$(nproc)}"
export ANLAND_NIRI_CARGO_HOME="${ANLAND_NIRI_CARGO_HOME:-$WORKDIR/cargo}"
export ANLAND_NIRI_TARGET_DIR="${ANLAND_NIRI_TARGET_DIR:-$WORKDIR/target}"
BUILD_XWAYLAND="${BUILD_XWAYLAND:-1}"
[[ "$BUILD_XWAYLAND" == 0 || "$BUILD_XWAYLAND" == 1 ]] || die 'BUILD_XWAYLAND must be 0 or 1'
INSTALL="${ANLAND_INSTALL:-${INSTALL:-0}}"
[[ "$INSTALL" == '0' || "$INSTALL" == '1' ]] || die 'INSTALL must be 0 or 1'
MAKEPKG_ARGS=(-C -f -s --clean)
[[ ${ANLAND_OFFLINE:-0} != 1 ]] || MAKEPKG_ARGS+=(--holdver)
if [[ "$INSTALL" == '1' ]]; then
    log "Building and installing tracked xorg-xwayland and niri packages (-j$JOBS)"
else
    log "Building tracked xorg-xwayland and niri packages without installing them (-j$JOBS)"
fi

run_makepkg() {
    local stage="$1"
    local srcdest="$2"
    local builddir="$3"
    shift 3
    (
        cd "$stage"
        PKGDEST="$BUILD_PKGDEST_DIR" SRCDEST="$srcdest" BUILDDIR="$builddir" \
            makepkg "${MAKEPKG_ARGS[@]}" "$@"
    )
}

collect_packages() {
    local stage="$1" output="$2" listing pkg metadata name arch destination
    local -n selected="$output"
    listing=$(cd "$stage" && PKGDEST="$BUILD_PKGDEST_DIR" makepkg --packagelist) || die 'cannot query package list'
    selected=()
    while IFS= read -r pkg; do
        [[ -s "$pkg" && "$pkg" -nt "$build_marker" ]] || die "missing or stale package: $pkg"
        metadata=$(bsdtar -xOf "$pkg" .PKGINFO) || die "cannot read package metadata: $pkg"
        name=$(awk -F' = ' '$1 == "pkgname" {print $2; exit}' <<<"$metadata")
        [[ -n "$name" ]] || die "missing package name: $pkg"
        case "$name" in *-debug|*-dbg|*-dbgsym) continue ;; esac
        arch=$(awk -F' = ' '$1 == "arch" {print $2; exit}' <<<"$metadata")
        [[ "$arch" == aarch64 || "$arch" == any ]] || die "wrong package architecture: $pkg"
        bsdtar -tf "$pkg" >/dev/null
        destination="$PKGDEST_DIR/${pkg##*/}"
        if [[ -e "$destination.sig" || -L "$destination.sig" ]]; then
            cmp -s "$pkg" "$destination" || die "existing signature preserved; move $destination.sig aside before replacing its package"
        fi
        cp -p "$pkg" "$destination"
        selected+=("$destination")
    done <<<"$listing"
    (( ${#selected[@]} > 0 )) || die 'no binary packages returned'
    sha256sum "${selected[@]}"
}

install_packages() {
    # makepkg -i cannot override local package signature policy. Pacman 7
    # removed its old --nosignature switch, so use a per-build config copy
    # that accepts only unsigned local artifacts. Repository verification in
    # the system pacman.conf remains unchanged.
    cp /etc/pacman.conf "$PACMAN_LOCAL_CONFIG"
    if grep -q '^#LocalFileSigLevel = Optional$' "$PACMAN_LOCAL_CONFIG"; then
        sed -i 's/^#LocalFileSigLevel = Optional$/LocalFileSigLevel = Optional/' "$PACMAN_LOCAL_CONFIG"
    else
        printf '\n[options]\nLocalFileSigLevel = Optional\n' >> "$PACMAN_LOCAL_CONFIG"
    fi
    sudo pacman --config "$PACMAN_LOCAL_CONFIG" -U --noconfirm "$@"
}

mkdir -p "$BUILD_PKGDEST_DIR" "$PKGDEST_DIR"
build_marker=$(mktemp "$WORKDIR/.build-start.XXXXXX")
trap 'rm -f -- "$build_marker"' EXIT
if [[ "$BUILD_XWAYLAND" == 1 ]]; then
    prepare_xwayland_stage
    run_makepkg "$XWAYLAND_STAGE" "$XWAYLAND_SRCDEST_DIR" "$XWAYLAND_BUILDDIR" "$@"
    collect_packages "$XWAYLAND_STAGE" xwayland_packages
    if [[ "$INSTALL" == 1 ]]; then
        log 'Installing the freshly built patched Xwayland package'
        install_packages "${xwayland_packages[@]}"
    fi
else
    log 'Skipping Xwayland by explicit BUILD_XWAYLAND=0 (requires an already patched Xwayland)'
fi

prepare_niri_stage
run_makepkg "$NIRI_STAGE" "$NIRI_SRCDEST_DIR" "$NIRI_BUILDDIR" "$@"

collect_packages "$NIRI_STAGE" niri_packages
if [[ "$INSTALL" == 1 ]]; then
    log 'Installing the freshly built Anland niri package'
    install_packages "${niri_packages[@]}"
fi

cat <<EOF

Done. latest upstream niri source built; Xwayland build enabled: $BUILD_XWAYLAND.
Package artifacts: $PKGDEST_DIR
Packages: niri-anland and current patched Xwayland when enabled.
EOF

if [[ "$INSTALL" == '1' ]]; then
    cat <<EOF
They are installed and tracked by pacman. A later repository niri or Xwayland
upgrade may replace them, so rerun this script after either version changes.
EOF
fi
