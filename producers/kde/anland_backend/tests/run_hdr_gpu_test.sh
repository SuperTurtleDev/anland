#!/usr/bin/env bash
# Requires installed KWin 6.8 development headers/library, Qt 6, epoxy, EGL and libdrm.
# Exercises the real production helpers through surfaceless EGL; Mesa llvmpipe works.
set -euo pipefail

anland_test_dir="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
anland_backend_dir="${anland_test_dir}/../src/backends/anland"
anland_build_dir="$(mktemp -d "${TMPDIR:-/tmp}/anland-hdr-gpu-test.XXXXXX")"
trap 'rm -rf -- "$anland_build_dir"' EXIT

read -r -a anland_cxx <<< "${CXX:-c++}"
read -r -a anland_cflags <<< "$(pkg-config --cflags Qt6Core Qt6Gui Qt6Qml egl epoxy libdrm)"
read -r -a anland_libs <<< "$(pkg-config --libs Qt6Core Qt6Gui Qt6Qml egl epoxy)"

"${anland_cxx[@]}" -std=c++23 \
    -DANLAND_KWIN_64=1 -DANLAND_KWIN_66=1 -DANLAND_KWIN_67=1 -DANLAND_KWIN_68=1 \
    -I"${KWIN_INCLUDE_DIR:-/usr/include/kwin}" -I"$anland_backend_dir" \
    "${anland_cflags[@]}" \
    "$anland_test_dir/hdr_gpu_test.cpp" "$anland_backend_dir/anland_color.cpp" \
    -o "$anland_build_dir/hdr_gpu_test" -lkwin "${anland_libs[@]}"

"$anland_build_dir/hdr_gpu_test"
