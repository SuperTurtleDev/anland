#!/usr/bin/env bash
# Only three pinned native color-helper files and host GLM/libdrm are needed.
set -euo pipefail
HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
REPO=$(cd -- "$HERE/../../.." && pwd -P)
SOURCE=${GAMESCOPE_SOURCE:-${XDG_CACHE_HOME:-$HOME/.cache}/anland-gamescope-armada/source}
[[ -f "$SOURCE/src/color_helpers.cpp" && -f "$SOURCE/src/color_helpers_impl.h" ]] || {
    echo 'Set GAMESCOPE_SOURCE to the patched pinned Gamescope source.' >&2; exit 1;
}
BUILD=$(mktemp -d "${TMPDIR:-/tmp}/anland-gamescope-hdr-gain.XXXXXX")
trap 'rm -rf -- "$BUILD"' EXIT
read -ra DRM_CFLAGS <<< "$(pkg-config --cflags libdrm)"
"${CXX:-c++}" -std=c++20 -O2 -DGLM_ENABLE_EXPERIMENTAL "${DRM_CFLAGS[@]}" \
    -I"$SOURCE/src" -I"$REPO/libdisplay_producer" -I"$REPO/producers/gamescope/anland_backend" \
    "$HERE/hdr_gain_test.cpp" "$HERE/native_lut.cpp" -o "$BUILD/hdr_gain_test"
"$BUILD/hdr_gain_test"
