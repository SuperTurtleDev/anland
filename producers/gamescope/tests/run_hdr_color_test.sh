#!/usr/bin/env bash
# Uses the build recipe's real Gamescope source and its matching wlroots headers.
# No downloads, package installation or device access.
set -euo pipefail
HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)
REPO=$(cd -- "$HERE/../../.." && pwd -P)
SOURCE=${GAMESCOPE_SOURCE:-${XDG_CACHE_HOME:-$HOME/.cache}/anland-gamescope-armada/source}
WLR_SOURCE=${WLR_SOURCE:-$SOURCE/subprojects/wlroots}
[[ -f "$SOURCE/src/color_helpers.cpp" && -f "$WLR_SOURCE/include/wlr/render/interface.h" ]] || {
    echo 'Set GAMESCOPE_SOURCE to the pinned Gamescope tree and WLR_SOURCE to its wlroots source.' >&2; exit 1;
}
BUILD=$(mktemp -d "${TMPDIR:-/tmp}/anland-gamescope-hdr-test.XXXXXX")
trap 'rm -rf -- "$BUILD"' EXIT
read -ra DRM_CFLAGS <<< "$(pkg-config --cflags libdrm pixman-1)"
COMPAT=()
# New host libdrm exposes the structs also vendored by this pinned Gamescope.
# Distinct type names let this CPU-only test compile both native declarations.
if printf '#include <xf86drm.h>\nhdr_metadata_infoframe value;\n' |
    "${CXX:-c++}" -std=c++20 "${DRM_CFLAGS[@]}" -x c++ -fsyntax-only - >/dev/null 2>&1; then
    cat > "$BUILD/libdrm-compat.h" <<'HEADER'
#include <xf86drm.h>
#define hdr_metadata_infoframe gamescope_vendored_hdr_metadata_infoframe
#define hdr_output_metadata gamescope_vendored_hdr_output_metadata
HEADER
    COMPAT=(-include "$BUILD/libdrm-compat.h")
fi
"${CXX:-c++}" -std=c++20 -O2 -DGLM_ENABLE_EXPERIMENTAL -DWLR_USE_UNSTABLE \
    "${COMPAT[@]}" "${DRM_CFLAGS[@]}" \
    -I"$SOURCE/src" -I"$WLR_SOURCE/include" \
    -I"$REPO/libdisplay_producer" -I"$REPO/producers/gamescope/anland_backend" \
    "$HERE/hdr_color_test.cpp" "$HERE/native_lut.cpp" -o "$BUILD/hdr_color_test"
"$BUILD/hdr_color_test"
