#!/usr/bin/env bash
# Requires installed KWin 6.8 headers/library, Qt 6, moc, epoxy, EGL and libdrm.
# Set KWIN_SOURCE_DIR to matching upstream source if private EGL headers are
# missing. The local anland-build-repair KWin 6.8 cache is used when available.
# Compiles actual production AnlandOutput; unused transport entry points abort.
set -euo pipefail

anland_test_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
anland_backend_dir="${anland_test_dir}/../src/backends/anland"
anland_build_dir="$(mktemp -d "${TMPDIR:-/tmp}/anland-hdr-mode-test.XXXXXX")"
trap 'rm -rf -- "$anland_build_dir"' EXIT

read -r -a anland_cxx <<< "${CXX:-c++}"
read -r -a anland_cflags <<< "$(pkg-config --cflags Qt6Core Qt6Gui Qt6Qml egl epoxy libdrm)"
read -r -a anland_libs <<< "$(pkg-config --libs Qt6Core Qt6Gui Qt6Qml egl epoxy)"
anland_defines=(-DANLAND_KWIN_64=1 -DANLAND_KWIN_66=1 -DANLAND_KWIN_67=1 -DANLAND_KWIN_68=1)
anland_includes=(-I"${KWIN_INCLUDE_DIR:-/usr/include/kwin}" -I"$anland_backend_dir"
    -I"$anland_backend_dir/libdisplay_producer" -I"$anland_backend_dir/common" -I"$anland_build_dir")
# Some distributions omit private EGL backend headers from their KWin package.
# Use matching upstream source headers for those declarations when necessary.
anland_source_dir="${KWIN_SOURCE_DIR:-${HOME}/.cache/anland-build-repair/kwin-6.8-verify}"
if [[ -f "$anland_source_dir/src/opengl/eglbackend.h" ]]; then
    anland_includes+=(-I"$anland_source_dir/src")
elif [[ ! -f "${KWIN_INCLUDE_DIR:-/usr/include/kwin}/opengl/eglbackend.h" ]]; then
    printf '%s\n' 'Missing private KWin EGL header; set KWIN_SOURCE_DIR to matching KWin 6.8 source.' >&2
    exit 2
fi

anland_moc="${MOC:-$(pkg-config --variable=libexecdir Qt6Core)/moc}"
"$anland_moc" "${anland_defines[@]}" "${anland_includes[@]}" "${anland_cflags[@]}" \
    "$anland_backend_dir/anland_output.h" -o "$anland_build_dir/moc_anland_output.cpp"
"${anland_cxx[@]}" -std=c++23 "${anland_defines[@]}" "${anland_includes[@]}" "${anland_cflags[@]}" \
    "$anland_test_dir/hdr_mode_test.cpp" "$anland_backend_dir/anland_output.cpp" \
    "$anland_backend_dir/anland_logging.cpp" -o "$anland_build_dir/hdr_mode_test" \
    -lkwin "${anland_libs[@]}"

"$anland_build_dir/hdr_mode_test"
