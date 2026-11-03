#!/usr/bin/env bash
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
build=$(mktemp -d /tmp/anland-tablet-client.XXXXXX)
trap 'rm -rf "$build"' EXIT
cc -std=c11 -Wall -Wextra -Werror -I"$here" -I"$root/common" \
    -I"$root/libdisplay_producer" "$here/input_test.c" "$here/input.c" \
    -lm -o "$build/input-test"
"$build/input-test"
protocol=$(pkg-config --variable=pkgdatadir wayland-protocols)/stable/tablet/tablet-v2.xml
wayland-scanner client-header "$protocol" "$build/tablet-v2-client-protocol.h"
wayland-scanner private-code "$protocol" "$build/tablet-v2-protocol.c"
if [[ -n ${WLROOTS_SOURCE:-} && -n ${WLROOTS_BUILD:-} ]]; then
    includes=(-I"$WLROOTS_SOURCE/include" -I"$WLROOTS_BUILD/include")
    libs=("$WLROOTS_BUILD"/libwlroots-*.so -Wl,-rpath,"$WLROOTS_BUILD")
else
    package=$(pkg-config --list-all | awk '/^wlroots-[0-9]/{print $1;exit}')
    [[ -n $package ]] || { echo 'Set WLROOTS_SOURCE and WLROOTS_BUILD or install wlroots development files.' >&2; exit 1; }
    read -ra includes <<< "$(pkg-config --cflags "$package")"
    read -ra libs <<< "$(pkg-config --libs "$package")"
fi
read -ra system_cflags <<< "$(pkg-config --cflags wayland-client wayland-server pixman-1 xkbcommon)"
read -ra system_libs <<< "$(pkg-config --libs wayland-client wayland-server pixman-1 xkbcommon)"
cc -std=c11 -Wall -Wextra -Werror -DWLR_USE_UNSTABLE -I"$here" -I"$root/common" \
    -I"$root/libdisplay_producer" -I"$build" "${includes[@]}" "${system_cflags[@]}" \
    "$here/tablet_test.c" "$here/tablet.c" "$build/tablet-v2-protocol.c" \
    "${libs[@]}" "${system_libs[@]}" -o "$build/tablet-test"
"$build/tablet-test"
