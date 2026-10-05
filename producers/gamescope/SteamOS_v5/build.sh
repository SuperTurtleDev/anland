#!/usr/bin/env bash
# SteamOS aarch64 Gamescope 3.16.28 + Xwayland 24.1.9 pacman packages.
# Usage: ./build.sh [3.16.28] [--nocheck|--noconfirm|--log|--nosign]
# INSTALL=1 explicitly installs; default INSTALL=0, never restarts services.
# BUILD_XWAYLAND=0 builds Gamescope only. JOBS=2 controls parallelism.
# Release ZIP via curl; pinned Git dependencies are fetched shallow (one commit).
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VERSION=3.16.28
if [[ ${1:-} == -h || ${1:-} == --help ]]; then sed -n '2,6p' "$0"; exit 0; fi
if [[ $# -gt 0 && $1 != -* ]]; then VERSION="$1"; shift; fi
[[ "$VERSION" == 3.16.28 ]] || { echo 'SteamOS patch is pinned to Gamescope 3.16.28' >&2; exit 1; }
for arg in "$@"; do
    case "$arg" in --nocheck|--noconfirm|--log|--nosign) ;; *) echo "Unsupported option: $arg" >&2; exit 1 ;; esac
done
BACKEND="$HERE/gamescope"
XPORT="$(cd "$HERE/../../xwayland/SteamOS_v5" && pwd)"
WORKDIR="${WORKDIR:-${XDG_CACHE_HOME:-$HOME/.cache}/anland-gamescope-build}"
INSTALL="${INSTALL:-${ANLAND_INSTALL:-0}}"
BUILD_XWAYLAND="${BUILD_XWAYLAND:-1}"
for flag in "$INSTALL" "$BUILD_XWAYLAND"; do
    [[ "$flag" == 0 || "$flag" == 1 ]] || { echo 'Flags must be 0 or 1' >&2; exit 1; }
done
export JOBS="${JOBS:-2}"
export MAKEFLAGS="-j$JOBS"
export LANG=C.UTF-8 LC_ALL=C.UTF-8 GIT_TERMINAL_PROMPT=0
[[ "$JOBS" =~ ^[1-9][0-9]*$ ]] || { echo 'JOBS must be a positive integer' >&2; exit 1; }
[[ $(id -u) != 0 && $(uname -m) == aarch64 ]] || { echo 'Requires an unprivileged aarch64 user' >&2; exit 1; }
[[ -d "$BACKEND" ]] || { echo 'Missing shared backend' >&2; exit 1; }
for tool in makepkg meson ninja tar patch python3 git bsdtar flock curl sha256sum; do
    command -v "$tool" >/dev/null || { echo "Missing tool: $tool" >&2; exit 1; }
done
[[ -x /usr/bin/steamos-session-shell && -d /usr/share/gamescope/reshade ]] || {
    echo 'Missing SteamOS Gamescope session/ReShade assets' >&2; exit 1;
}
if [[ "$BUILD_XWAYLAND" == 1 ]]; then
    [[ $(pacman -Q xorg-xwayland | cut -d' ' -f2) == 24.1.9-* ]] || {
        echo 'Xwayland patch requires the SteamOS 24.1.9 baseline' >&2; exit 1;
    }
fi
mkdir -p "$WORKDIR" "$HERE/artifacts"
WORKDIR="$(cd "$WORKDIR" && pwd)"
exec 9>"$WORKDIR/.build.lock"
flock -n 9 || { echo 'Another Gamescope build is active' >&2; exit 1; }
RUN="$(mktemp -d "$WORKDIR/build-3.16.28.XXXXXX")"
STAGE="$RUN/stage"
# Pinned Git dependencies live in a persistent shallow cache: one commit each,
# no history. The previous full mirror clone pulled ~600 MB (openvr alone was
# 533 MB of pure history) and repeated that download on every run.
DEPCACHE="${DEPCACHE:-$WORKDIR/depcache}"
mkdir -p "$STAGE" "$DEPCACHE"
DEPCACHE="$(cd "$DEPCACHE" && pwd)"
prefetch_dependencies() {
    local -a rows=()
    local line row path revision url name dir attempt ok cache_state index=0
    while IFS= read -r line || [[ -n "$line" ]]; do
        [[ -z "$line" || "$line" == \#* ]] && continue
        rows+=("$line")
    done < "$HERE/dependencies.lock"
    for row in "${rows[@]}"; do
        read -r path revision url _ <<< "$row"
        [[ "$path" == . ]] && continue
        name="anland-dep-$index"
        index=$((index + 1))
        dir="$DEPCACHE/$name"
        if git -C "$dir" cat-file -e "$revision^{commit}" 2>/dev/null; then
            cache_state=cached
        else
            rm -rf -- "$dir"
            git init -q --bare "$dir" || return 1
            git -C "$dir" remote add origin "$url" || return 1
            ok=0
            for attempt in 1 2 3 4 5; do
                if git -C "$dir" fetch -q --depth 1 origin "$revision" </dev/null &&
                    git -C "$dir" cat-file -e "$revision^{commit}" 2>/dev/null; then
                    ok=1
                    break
                fi
                printf '  -> retry %s for %s\n' "$attempt" "$name" >&2
                sleep 5
            done
            [[ "$ok" == 1 ]] || { echo "Failed to fetch $name from $url" >&2; return 1; }
            cache_state=fetched
        fi
        # A branch ref and HEAD keep makepkg's shared clone from seeing an empty repo.
        # Repair them on cache hits too, including an interrupted initial fetch.
        git -C "$dir" update-ref refs/heads/anland-pinned "$revision" || return 1
        git -C "$dir" symbolic-ref HEAD refs/heads/anland-pinned || return 1
        printf '  -> %s %s (%s)\n' "$name" "$cache_state" "$path"
    done
}
printf 'Resolving pinned dependencies into %s\n' "$DEPCACHE"
prefetch_dependencies || exit 1
printf 'Dependency cache size: %s\n' "$(du -sh "$DEPCACHE" | cut -f1)"
for name in PKGBUILD gamescope.patch dependencies.lock startup.sh; do
    cp "$HERE/$name" "$STAGE/"
done
# Follow the canonical shared-source links when preparing makepkg input.
tar -chf "$STAGE/anland-overlay.tar" --exclude='run_*' --exclude='*_test.*' -C "$BACKEND" .
PACKAGES="$HERE/artifacts"
packages=()
build_marker="$RUN/.build-start"
touch "$build_marker"
finish() {
    local rc=$?
    rm -f -- "$build_marker"
    if [[ "$rc" != 0 ]]; then
        printf 'Build failed (rc=%s). Diagnostic workspace: %s\n' "$rc" "$RUN" >&2
    fi
}
trap finish EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
# Fetch the release atomically before dependency installation or compilation.
fetch_source() (
    local destination="$1" url="$2" temporary
    if [[ -s "$destination" ]] && bsdtar -tf "$destination" >/dev/null 2>&1; then
        printf 'Reusing cached %s\n' "${destination##*/}"
        return 0
    fi
    temporary="$(mktemp "${destination}.part.XXXXXX")"
    trap 'rm -f -- "$temporary"' EXIT
    curl --fail --location --retry 3 --connect-timeout 20 --max-time 600 \
        --output "$temporary" "$url"
    bsdtar -tf "$temporary" >/dev/null
    mv -- "$temporary" "$destination"
)
fetch_source "$DEPCACHE/gamescope-$VERSION.zip" \
    "https://github.com/ValveSoftware/gamescope/archive/refs/tags/$VERSION.zip"
# Pin all non-VCS inputs in this run's staged recipe, not in the main repository.
python3 - "$STAGE" "$DEPCACHE/gamescope-$VERSION.zip" <<'PY'
from pathlib import Path
import hashlib, sys
stage, archive = map(Path, sys.argv[1:])
inputs = [archive] + [stage/name for name in ('gamescope.patch', 'anland-overlay.tar', 'dependencies.lock', 'startup.sh')]
sums = 'sha256sums=(' + ' '.join(repr(hashlib.sha256(p.read_bytes()).hexdigest()) for p in inputs) + ')'
recipe = stage/'PKGBUILD'
text = recipe.read_text()
old = "sha256sums=('SKIP' 'SKIP' 'SKIP' 'SKIP' 'SKIP')"
if text.count(old) != 1:
    raise ValueError('Unexpected source checksum declaration')
recipe.write_text(text.replace(old, sums))
PY
collect_packages() {
    local stage="$1" list="$2"
    (cd "$stage" && PKGDEST="$PACKAGES" makepkg --packagelist) > "$list"
    local -a outputs=()
    mapfile -t outputs < "$list"
    [[ ${#outputs[@]} -gt 0 ]] || { echo 'Empty package list' >&2; return 1; }
    packages+=("${outputs[@]}")
}
# Build Gamescope first, so an unavailable release download fails before Xwayland work.
(
    cd "$STAGE"
    # SRCDEST is the persistent shallow cache; --holdver stops makepkg from
    # "updating" those single-commit repos back into full-history fetches.
    export SRCDEST="$DEPCACHE" PKGDEST="$PACKAGES"
    makepkg --force --cleanbuild --syncdeps --needed --holdver "$@"
)
collect_packages "$STAGE" "$RUN/.gamescope-packagelist"
if [[ "$BUILD_XWAYLAND" == 1 ]]; then
    XSTAGE="$RUN/xwayland-package"
    mkdir -p "$XSTAGE" "$RUN/xwayland-sources"
    cp "$XPORT/xorg-xwayland.PKGBUILD" "$XSTAGE/PKGBUILD"
    cp "$XPORT/xwayland.patch" "$XSTAGE/xwayland.patch"
    (
        cd "$XSTAGE"
        export SRCDEST="$RUN/xwayland-sources" PKGDEST="$PACKAGES"
        makepkg --force --cleanbuild --syncdeps --needed --nocheck "$@"
    )
    collect_packages "$XSTAGE" "$RUN/.xwayland-packagelist"
fi
for pkg in "${packages[@]}"; do
    [[ -s "$pkg" && "$pkg" -nt "$build_marker" ]] || { echo "Missing or stale package: $pkg" >&2; exit 1; }
    bsdtar -tf "$pkg" >/dev/null
    sha256sum "$pkg"
done
names=()
for pkg in "${packages[@]}"; do names+=("${pkg##*/}"); done
(cd "$PACKAGES" && sha256sum "${names[@]}" > SHA256SUMS && sha256sum -c SHA256SUMS)
if [[ "$INSTALL" == 1 ]]; then
    sudo pacman -U "${packages[@]}"
fi
printf 'Build finished (INSTALL=%s). Packages: %s\n' "$INSTALL" "$PACKAGES"
# Keep failed trees for diagnosis; remove disposable sources/objects only after success.
rm -rf -- "$RUN"
