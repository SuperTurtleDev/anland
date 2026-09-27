#!/usr/bin/env bash
# Build the Arch Linux ARM 50.5 Mutter port; installing the result requires
# ANLAND_INSTALL=1. All work and resulting packages stay in ~/.cache/anland/.
set -euo pipefail
here=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
repo=$(cd -- "$here/../../.." && pwd)
cache=${WORKDIR:-${XDG_CACHE_HOME:-$HOME/.cache}/anland/mutter-arch}
stage=$cache/package
src=$cache/sources
pkg=$cache/packages
overlay=$cache/overlay
[[ $(id -u) != 0 && $(uname -m) == aarch64 ]] || { echo 'Build as non-root on aarch64' >&2; exit 1; }
[[ -d $here/mutter/src/backends/anland && -f $here/mutter.patch ]] || { echo 'Shared backend or patch missing' >&2; exit 1; }
for tool in makepkg tar patch sha512sum; do command -v "$tool" >/dev/null || { echo "Missing $tool" >&2; exit 1; }; done
mkdir -p "$stage" "$src" "$pkg" "$overlay"
# No copied backend lives in the repository: resolve shared symlinks only in
# the build staging directory. Remove just this build's previous overlay.
rm -rf "$overlay/src"
mkdir -p "$overlay/src/backends/anland" "$overlay/src/tests"
cp -aL "$here/mutter/src/backends/anland/." "$overlay/src/backends/anland/"
cp -aL "$here/mutter/src/tests/." "$overlay/src/tests/"
[[ -f $overlay/src/backends/anland/libdisplay_producer/anland_device.c &&
   -f $overlay/src/backends/anland/common/anland_present_ipc.c &&
   -f $overlay/src/tests/anland-device-tests.c ]] || { echo 'Incomplete staged backend' >&2; exit 1; }
rm -f "$stage/mutter-overlay.tar.gz"
tar -czf "$stage/mutter-overlay.tar.gz" -C "$overlay" src
cp "$here/PKGBUILD" "$here/mutter.patch" "$stage/"
# Cached downloads are verified against the PKGBUILD SHA512, never skipped.
probe=${ANLAND_SOURCE_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/anland-review/arch-mutter}
[[ -f $probe/mutter-50.5.complete.tar.gz ]] && cp -f "$probe/mutter-50.5.complete.tar.gz" "$src/mutter-50.5.tar.gz"
[[ -f $probe/gvdb-b54bc5da.tar.gz ]] && cp -f "$probe/gvdb-b54bc5da.tar.gz" "$src/gvdb-b54bc5da.tar.gz"
cd "$stage"
export SRCDEST=$src PKGDEST=$pkg BUILDDIR=$cache/build
export MAKEFLAGS="-j${JOBS:-2}"
# Install only missing build dependencies; never install the generated Mutter
# packages without explicit opt-in. Do not bypass source checksum validation.
makepkg -s --needed --noconfirm --cleanbuild "$@"
echo "Packages: $pkg"
if [[ ${ANLAND_INSTALL:-0} == 1 ]]; then
  mapfile -t packages < <(find "$pkg" -maxdepth 1 -name 'mutter*50.5-1.1-aarch64.pkg.tar.*' -type f | sort)
  [[ ${#packages[@]} == 3 ]] || { echo "Expected three split packages, found ${#packages[@]}" >&2; exit 1; }
  sudo pacman -U -- "${packages[@]}"
else
  echo 'Not installed. Set ANLAND_INSTALL=1 for an explicit installation.'
fi
