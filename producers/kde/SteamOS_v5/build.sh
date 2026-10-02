#!/usr/bin/env bash
# SteamOS aarch64 KWin 6.2.5 + Xwayland 24.1.9 pacman packages.
# Usage: ./build.sh [6.2.5] [--nocheck|--noconfirm|--log|--nosign]
# INSTALL=1 explicitly installs; default INSTALL=0, never restarts services.
# BUILD_XWAYLAND=0 builds KWin only. ANLAND_RESUME=1 reuses verified sources.
# Sources: cache first; missing archives are downloaded from official URLs.
# KWIN_ARCHIVE (or KWIN_TARBALL) overrides the ZIP path; XWAYLAND_TARBALL the tar.xz.
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VERSION=6.2.5
if [[ ${1:-} == -h || ${1:-} == --help ]]; then sed -n '2,7p' "$0"; exit 0; fi
if [[ $# -gt 0 && $1 != -* ]]; then VERSION="$1"; shift; fi
[[ "$VERSION" == 6.2.5 ]] || { echo 'SteamOS patch is pinned to KWin 6.2.5' >&2; exit 1; }
# Options cannot bypass INSTALL=1 or destroy an incremental source tree.
for arg in "$@"; do
    case "$arg" in --nocheck|--noconfirm|--log|--nosign) ;; *) echo "Unsupported option: $arg" >&2; exit 1 ;; esac
done
BACKEND="$HERE/kwin/src/backends/anland"
XPORT="$(cd "$HERE/../../xwayland/SteamOS_v5" && pwd)"
WORKDIR="${WORKDIR:-${XDG_CACHE_HOME:-$HOME/.cache}/anland-kwin625/package}"
ARCHIVE="${KWIN_ARCHIVE:-${KWIN_TARBALL:-${XDG_CACHE_HOME:-$HOME/.cache}/anland-kwin625/kwin-v6.2.5.zip}}"
XARCHIVE="${XWAYLAND_TARBALL:-${XDG_CACHE_HOME:-$HOME/.cache}/anland-kwin625/xwayland-24.1.9.tar.xz}"
INSTALL="${INSTALL:-0}"
BUILD_XWAYLAND="${BUILD_XWAYLAND:-1}"
ANLAND_RESUME="${ANLAND_RESUME:-0}"
for flag in "$INSTALL" "$BUILD_XWAYLAND" "$ANLAND_RESUME"; do
    [[ "$flag" == 0 || "$flag" == 1 ]] || { echo 'Flags must be 0 or 1' >&2; exit 1; }
done
export JOBS="${JOBS:-2}"
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || { echo 'JOBS must be a positive integer' >&2; exit 1; }
[[ $(id -u) != 0 && $(uname -m) == aarch64 ]] || { echo 'Requires an unprivileged aarch64 user' >&2; exit 1; }
[[ -d "$BACKEND" ]] || { echo 'Missing shared backend' >&2; exit 1; }
for tool in makepkg cmake ninja tar patch python3 bsdtar; do command -v "$tool" >/dev/null; done
# Download atomically, and verify existing caches as well as new downloads.
fetch_source() (
    set -euo pipefail
    local destination="$1" url="$2" algorithm="$3" expected="$4" temporary=''
    verify_source() { printf '%s  %s\n' "$expected" "$1" | "$algorithm" -c -; }
    if [[ -f "$destination" ]]; then
        verify_source "$destination"
        return
    fi
    command -v curl >/dev/null || { echo 'curl is required to download sources' >&2; exit 1; }
    mkdir -p "$(dirname "$destination")"
    temporary="$(mktemp "${destination}.part.XXXXXX")"
    trap '[[ -z "$temporary" ]] || rm -f -- "$temporary"' EXIT
    echo "Downloading: $url"
    curl --fail --location --retry 3 --connect-timeout 20 --max-time 600 \
        --output "$temporary" "$url"
    verify_source "$temporary"
    mv -- "$temporary" "$destination"
    temporary=''
)
fetch_source "$ARCHIVE" \
    'https://invent.kde.org/plasma/kwin/-/archive/v6.2.5/kwin-v6.2.5.zip?ref_type=tags' \
    sha256sum '9e9b8ec81f44c487aa6be141c0cd10f9980c0528186a819af55f4253aa1a4904'
if [[ "$BUILD_XWAYLAND" == 1 ]]; then
    fetch_source "$XARCHIVE" \
        'https://xorg.freedesktop.org/archive/individual/xserver/xwayland-24.1.9.tar.xz' \
        sha512sum '7438a572651dc77c1fd749879abccdc9a245c7b75143668d5561a8e99d41063f042a8eb3f9b931a2a12be1fc3cb9d197eee6794d0702a19e56c20f55acb35a26'
fi
[[ $(pacman -Q kwin | cut -d' ' -f2) == 6.2.5-* && $(qmake6 -query QT_VERSION) == 6.8.0 ]] || {
    echo 'Installed KWin/Qt differs from the verified 6.2.5/6.8.0 baseline' >&2; exit 1;
}
mkdir -p "$WORKDIR/stage" "$WORKDIR/srcdest" "$WORKDIR/packages"
# Content-only manifest follows the same shared-source links as tar -h.
# Include path names so additions/removals also invalidate resume.
backend_manifest() {
    python3 - "$BACKEND" <<'PY'
from pathlib import Path
import hashlib, os, sys
root = Path(sys.argv[1])
entries = []
for directory, dirs, files in os.walk(root, followlinks=True):
    dirs.sort()
    for name in sorted(files):
        p = Path(directory) / name
        entries.append((p.relative_to(root).as_posix(), hashlib.sha256(p.read_bytes()).hexdigest()))
for name, digest in sorted(entries):
    print(digest + '  ' + name)
PY
}
# Keep existing staging/cache paths so the validated KWin build remains reusable.
if [[ "$ANLAND_RESUME" == 1 ]]; then
    [[ -d "$WORKDIR/stage/src/build" && -f "$WORKDIR/stage/.prepared.sha256" ]] || {
        echo 'No verified KWin tree to resume' >&2; exit 1;
    }
    (cd "$WORKDIR/stage" && sha256sum -c .prepared.sha256)
    cmp "$HERE/PKGBUILD" "$WORKDIR/stage/PKGBUILD"
    cmp "$HERE/kwin.patch" "$WORKDIR/stage/kwin.patch"
    cmp "$ARCHIVE" "$WORKDIR/srcdest/kwin-v6.2.5.zip"
    [[ -f "$WORKDIR/stage/.backend.sha256" ]] || {
        echo 'Resume lacks shared-source manifest; run once with ANLAND_RESUME=0' >&2; exit 1;
    }
    current_manifest="$(backend_manifest)"
    [[ "$current_manifest" == "$(cat "$WORKDIR/stage/.backend.sha256")" ]] || {
        echo 'Shared backend/common/library sources changed; ANLAND_RESUME=0 required' >&2; exit 1;
    }
else
    cp "$HERE/PKGBUILD" "$HERE/kwin.patch" "$WORKDIR/stage/"
    cp "$ARCHIVE" "$WORKDIR/srcdest/kwin-v6.2.5.zip"
    backend_manifest > "$WORKDIR/stage/.backend.sha256"
    tar -chf "$WORKDIR/stage/anland-overlay.tar" -C "$HERE/kwin/src/backends" anland
fi
export LANG=C.UTF-8 LC_ALL=C.UTF-8
packages=()
# Exact package names/compression come from makepkg, not a hard-coded suffix.
# A package left by a previous run cannot satisfy this run's checks.
build_marker="$(mktemp "$WORKDIR/.build-start.XXXXXX")"
trap 'rm -f -- "$build_marker"' EXIT
collect_packages() {
    local stage="$1" list="$2"
    (cd "$stage" && PKGDEST="$WORKDIR/packages" makepkg --packagelist) > "$list"
    local -a outputs=()
    mapfile -t outputs < "$list"
    [[ ${#outputs[@]} -gt 0 ]] || { echo 'Empty package list' >&2; return 1; }
    packages+=("${outputs[@]}")
}
if [[ "$BUILD_XWAYLAND" == 1 ]]; then
    [[ $(pacman -Q xorg-xwayland | cut -d' ' -f2) == 24.1.9-* ]] || {
        echo 'Xwayland patch is pinned to the SteamOS 24.1.9 baseline' >&2; exit 1;
    }
    XSTAGE="$WORKDIR/xwayland-package"
    XSOURCES="$WORKDIR/xwayland-sources"
    mkdir -p "$XSTAGE" "$XSOURCES"
    if [[ "$ANLAND_RESUME" == 1 && -f "$XSTAGE/.prepared.sha256" ]]; then
        (cd "$XSTAGE" && sha256sum -c .prepared.sha256)
        cmp "$XPORT/xorg-xwayland.PKGBUILD" "$XSTAGE/PKGBUILD"
        cmp "$XPORT/xwayland.patch" "$XSTAGE/xwayland.patch"
        [[ -d "$XSTAGE/src/build" ]] || { echo 'Missing Xwayland build tree' >&2; exit 1; }
        xargs=(--force --noextract --noprepare --nocheck)
    else
        cp "$XPORT/xorg-xwayland.PKGBUILD" "$XSTAGE/PKGBUILD"
        cp "$XPORT/xwayland.patch" "$XSTAGE/xwayland.patch"
        if [[ -f "$XARCHIVE" ]]; then cp "$XARCHIVE" "$XSOURCES/xwayland-24.1.9.tar.xz"; fi
        xargs=(--force --cleanbuild --nocheck)
    fi
    (
        cd "$XSTAGE"
        export SRCDEST="$XSOURCES" PKGDEST="$WORKDIR/packages"
        makepkg "${xargs[@]}" "$@"
        sha256sum PKGBUILD xwayland.patch "$XSOURCES/xwayland-24.1.9.tar.xz" > .prepared.sha256
    )
    collect_packages "$XSTAGE" "$WORKDIR/.xwayland-packagelist"
fi
(
    cd "$WORKDIR/stage"
    export SRCDEST="$WORKDIR/srcdest" PKGDEST="$WORKDIR/packages"
    if [[ "$ANLAND_RESUME" == 1 ]]; then
        makepkg --force --noextract --noprepare --nocheck "$@"
    else
        sha256sum PKGBUILD kwin.patch anland-overlay.tar .backend.sha256 "$SRCDEST/kwin-v6.2.5.zip" > .prepared.sha256
        makepkg --force --cleanbuild --nocheck "$@"
    fi
)
collect_packages "$WORKDIR/stage" "$WORKDIR/.kwin-packagelist"
for pkg in "${packages[@]}"; do
    [[ -s "$pkg" && "$pkg" -nt "$build_marker" ]] || {
        echo "Missing or stale package from this build: $pkg" >&2; exit 1;
    }
    bsdtar -tf "$pkg" >/dev/null
    sha256sum "$pkg"
done
if [[ "$INSTALL" == 1 ]]; then
    # No signature-policy weakening, /etc/environment changes or service restarts.
    sudo pacman -U "${packages[@]}"
fi
printf 'Build finished (INSTALL=%s). Packages: %s\n' "$INSTALL" "$WORKDIR/packages"