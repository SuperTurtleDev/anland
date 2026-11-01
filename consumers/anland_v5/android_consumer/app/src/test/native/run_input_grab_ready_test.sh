#!/bin/sh
set -eu
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_dir=$(CDPATH= cd -- "$script_dir/../../../../../../.." && pwd)
: "${CC:=clang}"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT INT TERM
mkdir -p "$work/android"
cat > "$work/android/log.h" <<'HEADER'
#define ANDROID_LOG_ERROR 6
#define ANDROID_LOG_INFO 4
static inline int __android_log_print(int priority, const char *tag, const char *format, ...) {
    (void)priority; (void)tag; (void)format;
    return 0;
}
HEADER
"$CC" -std=c11 -O1 -ffunction-sections -fdata-sections -Wl,--gc-sections \
    -I "$work" -I "$repo_dir/common" "$script_dir/input_grab_ready_test.c" \
    "$repo_dir/common/socket_utils.c" -o "$work/test_ready"
"$work/test_ready"
