#!/usr/bin/env bash
#
# Build the Arch Linux ARM anland KWin and Xwayland ports as pacman packages.
#
# Usage:
#   ./build.sh [6.7.4] [additional makepkg options]
#
# KWIN_TARBALL and XWAYLAND_TARBALL may point at locally cached source archives.
# Repository-level tarballs are used when available before makepkg downloads the
# pinned upstream sources. KWIN_PATCH and XWAYLAND_PATCH override the patches.
# By default, successful builds are installed through pacman; set INSTALL=0
# only when package artifacts are needed without changing the running system.
#
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VERSION='6.7.4'
XWAYLAND_VERSION='24.1.13'
if [[ "$#" -gt 0 && ( "$1" == '-h' || "$1" == '--help' ) ]]; then
    sed -n '4,9p' "$0"
    exit 0
fi
if [[ "$#" -gt 0 && "$1" != -* ]]; then
    VERSION="$1"
    shift
fi

log()  { printf '\n\033[1;34m==> %s\033[0m\n' "$*"; }
die()  { printf '\033[1;31m[error] %s\033[0m\n' "$*" >&2; exit 1; }

[[ "$VERSION" == '6.7.4' ]] || die "this Arch port is tied to KWin 6.7.4 (got $VERSION)"
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
KWIN_BUILDDIR="$WORKDIR/kwin-build"
XWAYLAND_BUILDDIR="$WORKDIR/xwayland-build"
PACMAN_LOCAL_CONFIG="$WORKDIR/pacman-local.conf"
LOCAL_KWIN_TARBALL="${KWIN_TARBALL:-}"
LOCAL_XWAYLAND_TARBALL="${XWAYLAND_TARBALL:-}"

find_local_tarball() {
    local name="$1"
    local version="$2"
    local candidate
    for candidate in \
        "$SCRIPT_DIR/../$name-$version.tar.xz" \
        "$SCRIPT_DIR/$name-$version.tar.xz"; do
        if [[ -f "$candidate" ]]; then
            printf '%s\n' "$candidate"
            return 0
        fi
    done
    return 1
}

if [[ -z "$LOCAL_KWIN_TARBALL" ]]; then
    LOCAL_KWIN_TARBALL="$(find_local_tarball kwin "$VERSION" || true)"
fi
if [[ -z "$LOCAL_XWAYLAND_TARBALL" ]]; then
    LOCAL_XWAYLAND_TARBALL="$(find_local_tarball xwayland "$XWAYLAND_VERSION" || true)"
fi

prepare_xwayland_stage() {
    log 'Preparing Xwayland makepkg staging directory'
    rm -rf "$XWAYLAND_STAGE"
    mkdir -p "$XWAYLAND_STAGE" "$XWAYLAND_SRCDEST_DIR" "$PKGDEST_DIR" "$XWAYLAND_BUILDDIR"
    install -m644 "$XWAYLAND_PKGFILE" "$XWAYLAND_STAGE/PKGBUILD"
    install -m644 "$XWAYLAND_PATCH" "$XWAYLAND_STAGE/xwayland.patch"

    if [[ -n "$LOCAL_XWAYLAND_TARBALL" ]]; then
        [[ -f "$LOCAL_XWAYLAND_TARBALL" ]] || die "XWAYLAND_TARBALL not found: $LOCAL_XWAYLAND_TARBALL"
        log "Using local Xwayland source tarball: $LOCAL_XWAYLAND_TARBALL"
        install -m644 "$LOCAL_XWAYLAND_TARBALL" "$XWAYLAND_SRCDEST_DIR/xwayland-$XWAYLAND_VERSION.tar.xz"
    else
        log "No local Xwayland tarball found; makepkg will download pinned $XWAYLAND_VERSION source"
    fi
}

prepare_kwin_stage() {
    log 'Preparing KWin makepkg staging directory'
    rm -rf "$KWIN_STAGE" "$KWIN_OVERLAY_ROOT"
    mkdir -p "$KWIN_STAGE" "$KWIN_OVERLAY_ROOT/kwin-$VERSION" "$KWIN_SRCDEST_DIR" "$PKGDEST_DIR" "$KWIN_BUILDDIR"
    install -m644 "$SCRIPT_DIR/PKGBUILD" "$KWIN_STAGE/PKGBUILD"
    install -m644 "$KWIN_PATCH" "$KWIN_STAGE/kwin.patch"

    # Keep the archive root aligned with the upstream source directory. The
    # PKGBUILD can therefore extract it without relying on the checkout's path.
    # -L dereferences symlinks so repo-relative links (libdisplay_producer/)
    # become real files instead of dangling symlinks inside the tarball.
    cp -aL "$BACKEND_SRC/src" "$KWIN_OVERLAY_ROOT/kwin-$VERSION/"
    tar -cf "$KWIN_STAGE/anland-overlay.tar" -C "$KWIN_OVERLAY_ROOT" "kwin-$VERSION/src/backends/anland"

    if [[ -n "$LOCAL_KWIN_TARBALL" ]]; then
        [[ -f "$LOCAL_KWIN_TARBALL" ]] || die "KWIN_TARBALL not found: $LOCAL_KWIN_TARBALL"
        log "Using local KWin source tarball: $LOCAL_KWIN_TARBALL"
        install -m644 "$LOCAL_KWIN_TARBALL" "$KWIN_SRCDEST_DIR/kwin-$VERSION.tar.xz"
    else
        log "No local KWin tarball found; makepkg will download pinned $VERSION source"
    fi
}

export JOBS="${JOBS:-$(nproc)}"
INSTALL="${INSTALL:-1}"
[[ "$INSTALL" == '0' || "$INSTALL" == '1' ]] || die 'INSTALL must be 0 or 1'
MAKEPKG_ARGS=(-C -f -s --clean)
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

prepare_xwayland_stage
run_makepkg "$XWAYLAND_STAGE" "$XWAYLAND_SRCDEST_DIR" "$XWAYLAND_BUILDDIR" "$@"

shopt -s nullglob
xwayland_packages=("$PKGDEST_DIR"/xorg-xwayland-"$XWAYLAND_VERSION"-*.pkg.tar.*)
(( ${#xwayland_packages[@]} > 0 )) || die 'makepkg finished without producing an aarch64 Xwayland package'
if [[ "$INSTALL" == '1' ]]; then
    log 'Installing the freshly built patched Xwayland package'
    install_packages "${xwayland_packages[@]}"
fi

prepare_kwin_stage
run_makepkg "$KWIN_STAGE" "$KWIN_SRCDEST_DIR" "$KWIN_BUILDDIR" "$@"

kwin_packages=("$PKGDEST_DIR"/kwin-"$VERSION"-*.pkg.tar.*)
(( ${#kwin_packages[@]} > 0 )) || die 'makepkg finished without producing an aarch64 KWin package'
if [[ "$INSTALL" == '1' ]]; then
    log 'Installing the freshly built anland KWin package'
    install_packages "${kwin_packages[@]}"

    sudo touch /etc/environment
    sudo sed -i '/^ANLAND_SKIP_IMPLICIT_SYNC_WAIT=/d' /etc/environment
    printf '%s\n' 'ANLAND_SKIP_IMPLICIT_SYNC_WAIT=1' | sudo tee -a /etc/environment >/dev/null
fi
shopt -u nullglob

cat <<EOF

Done. The patched aarch64 Xwayland $XWAYLAND_VERSION and KWin $VERSION packages were built.
Package artifacts: $PKGDEST_DIR
The packages keep the names 'xorg-xwayland' and 'kwin' and use pkgrel 1.1.
EOF

if [[ "$INSTALL" == '1' ]]; then
    cat <<EOF
They are installed and tracked by pacman. A later repository KWin or Xwayland
upgrade may replace them, so rerun this script after either version changes.
EOF
fi
