#!/usr/bin/env bash
#
# Build the Arch Linux ARM anland KWin and Xwayland ports as pacman packages.
#
# Usage:
#   ./build.sh [additional makepkg options]
#
# KWin uses the verified 6.8.0 release; its patch must apply without fuzz.
# BUILD_XWAYLAND=0 reuses an already installed patched Xwayland.
# Xwayland keeps its existing upstream source policy.
# KWIN_PATCH and XWAYLAND_PATCH override the maintained patches.
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

BACKEND_SRC="$SCRIPT_DIR/kwin"
KWIN_PATCH="${KWIN_PATCH:-$SCRIPT_DIR/kwin.patch}"
XWAYLAND_PATCH="${XWAYLAND_PATCH:-$SCRIPT_DIR/xwayland.patch}"
XWAYLAND_PKGFILE="$SCRIPT_DIR/xorg-xwayland.PKGBUILD"
[[ -f "$KWIN_PATCH" ]] || die "KWin patch not found: $KWIN_PATCH"
[[ -f "$XWAYLAND_PATCH" ]] || die "Xwayland patch not found: $XWAYLAND_PATCH"
[[ -f "$XWAYLAND_PKGFILE" ]] || die "Xwayland PKGBUILD not found: $XWAYLAND_PKGFILE"
[[ -d "$BACKEND_SRC/src/backends/anland" ]] || die "backend sources not found: $BACKEND_SRC"

CACHE_ROOT="${XDG_CACHE_HOME:-$HOME/.cache}/anland/kwin-arch"
WORKDIR="${WORKDIR:-$CACHE_ROOT}"
KWIN_STAGE="$WORKDIR/kwin-package"
XWAYLAND_STAGE="$WORKDIR/xwayland-package"
KWIN_OVERLAY_ROOT="$WORKDIR/kwin-overlay"
KWIN_SRCDEST_DIR="$WORKDIR/kwin-sources"
XWAYLAND_SRCDEST_DIR="$WORKDIR/xwayland-sources"
PKGDEST_DIR="$WORKDIR/packages"
BUILD_XWAYLAND="${BUILD_XWAYLAND:-1}"
[[ "$BUILD_XWAYLAND" == '0' || "$BUILD_XWAYLAND" == '1' ]] || die 'BUILD_XWAYLAND must be 0 or 1'
mkdir -p "$WORKDIR"
exec 9>"$WORKDIR/.build.lock"
flock -n 9 || die "another KWin Arch build is using $WORKDIR"
KWIN_BUILDDIR="$WORKDIR/kwin-build"
XWAYLAND_BUILDDIR="$WORKDIR/xwayland-build"
PACMAN_LOCAL_CONFIG="$WORKDIR/pacman-local.conf"

prepare_xwayland_stage() {
    log 'Preparing Xwayland makepkg staging directory'
    rm -rf "$XWAYLAND_STAGE"
    mkdir -p "$XWAYLAND_STAGE" "$XWAYLAND_SRCDEST_DIR" "$PKGDEST_DIR" "$XWAYLAND_BUILDDIR"
    install -m644 "$XWAYLAND_PKGFILE" "$XWAYLAND_STAGE/PKGBUILD"
    install -m644 "$XWAYLAND_PATCH" "$XWAYLAND_STAGE/xwayland.patch"

    log 'makepkg will fetch the current Xwayland upstream default branch'
}

prepare_kwin_stage() {
    log 'Preparing KWin makepkg staging directory'
    rm -rf "$KWIN_STAGE" "$KWIN_OVERLAY_ROOT"
    mkdir -p "$KWIN_STAGE" "$KWIN_OVERLAY_ROOT/kwin" "$KWIN_SRCDEST_DIR" "$PKGDEST_DIR" "$KWIN_BUILDDIR"
    install -m644 "$SCRIPT_DIR/PKGBUILD" "$KWIN_STAGE/PKGBUILD"
    install -m644 "$KWIN_PATCH" "$KWIN_STAGE/kwin.patch"

    # Match the upstream Git checkout path for the self-contained overlay.
    # -L/-h dereference the libdisplay_producer symlinks: the packaged overlay
    # must be self-contained (relative links only resolve inside the checkout).
    cp -aL "$BACKEND_SRC/src" "$KWIN_OVERLAY_ROOT/kwin/"
    tar -chf "$KWIN_STAGE/anland-overlay.tar" -C "$KWIN_OVERLAY_ROOT" "kwin/src/backends/anland"
    # Freeze all local inputs in the staged makepkg recipe, not just upstream.
    local release_sha patch_sha overlay_sha
    release_sha="$(source "$KWIN_STAGE/PKGBUILD"; printf '%s' "${sha256sums[0]}")"
    patch_sha="$(sha256sum "$KWIN_STAGE/kwin.patch" | cut -d' ' -f1)"
    overlay_sha="$(sha256sum "$KWIN_STAGE/anland-overlay.tar" | cut -d' ' -f1)"
    printf '\n# Generated local input checksums.\nsha256sums=(\x27%s\x27 \x27%s\x27 \x27%s\x27)\n' \
        "$release_sha" "$patch_sha" "$overlay_sha" >> "$KWIN_STAGE/PKGBUILD"
}

export JOBS="${JOBS:-$(nproc)}"
INSTALL="${ANLAND_INSTALL:-${INSTALL:-0}}"
[[ "$INSTALL" == '0' || "$INSTALL" == '1' ]] || die 'INSTALL must be 0 or 1'
MAKEPKG_ARGS=(-f -s)
if [[ "$INSTALL" == '1' ]]; then
    log "Building and installing tracked xorg-xwayland and kwin packages (-j$JOBS)"
else
    log "Building tracked xorg-xwayland and kwin packages without installing them (-j$JOBS)"
fi

run_makepkg() {
    local stage="$1"
    local srcdest="$2"
    local builddir="$3"
    shift 3
    (
        cd "$stage"
        PKGDEST="$PKGDEST_DIR" SRCDEST="$srcdest" BUILDDIR="$builddir" \
            makepkg "${MAKEPKG_ARGS[@]}" "$@"
    )
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

shopt -s nullglob
if [[ "$BUILD_XWAYLAND" == '1' ]]; then
    prepare_xwayland_stage
    run_makepkg "$XWAYLAND_STAGE" "$XWAYLAND_SRCDEST_DIR" "$XWAYLAND_BUILDDIR" "$@"

    mapfile -t xwayland_packages < <(cd "$XWAYLAND_STAGE" && PKGDEST="$PKGDEST_DIR" makepkg --packagelist)
    (( ${#xwayland_packages[@]} > 0 )) || die 'makepkg returned no xwayland package names'
    for pkg in "${xwayland_packages[@]}"; do [[ -s "$pkg" ]] || die "missing package: $pkg"; done
    if [[ "$INSTALL" == '1' ]]; then
        log 'Installing the freshly built patched Xwayland package'
        install_packages "${xwayland_packages[@]}"
    fi
else
    log 'Reusing installed Xwayland (BUILD_XWAYLAND=0); no Xwayland rebuild or installation'
fi

prepare_kwin_stage
run_makepkg "$KWIN_STAGE" "$KWIN_SRCDEST_DIR" "$KWIN_BUILDDIR" "$@"

mapfile -t kwin_packages < <(cd "$KWIN_STAGE" && PKGDEST="$PKGDEST_DIR" makepkg --packagelist)
(( ${#kwin_packages[@]} > 0 )) || die 'makepkg returned no kwin package names'
for pkg in "${kwin_packages[@]}"; do [[ -s "$pkg" ]] || die "missing package: $pkg"; done
if [[ "$INSTALL" == '1' ]]; then
    log 'Installing the freshly built anland KWin package'
    install_packages "${kwin_packages[@]}"

    # Anland-only runtime switches belong to startup.sh, not /etc/environment.
fi
shopt -u nullglob

cat <<EOF

Done. KWin 6.8.0 was built for aarch64 (BUILD_XWAYLAND=$BUILD_XWAYLAND).
Package artifacts: $PKGDEST_DIR
The packages keep the names 'xorg-xwayland' and 'kwin'; KWin uses pkgrel 1.1.
EOF

if [[ "$INSTALL" == '1' ]]; then
    cat <<EOF
They are installed and tracked by pacman. A later repository KWin or Xwayland
upgrade may replace them, so rerun this script after either version changes.
EOF
fi
