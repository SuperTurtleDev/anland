#!/bin/sh
set -eu
hdr_test_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
hdr_test_binary=$(mktemp /tmp/anland-hyprland-hdr-test.XXXXXX)
trap 'rm -f "$hdr_test_binary"' EXIT HUP INT TERM
"${CXX:-c++}" -std=c++20 -Wall -Wextra -Werror -pedantic \
    "$hdr_test_dir/hdr_color_test.cpp" -o "$hdr_test_binary"
"$hdr_test_binary"
printf '%s\n' 'Hyprland fixed HDR/SDR rule and reconnect tests passed'
