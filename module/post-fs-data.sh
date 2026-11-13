#!/system/bin/sh
# KernelSU runs module post-fs-data before the metamodule mount stage.
# A complete service-context overlay is tied to its source ROM. Reject it
# before it can shadow new Android service labels after an OTA.
MODDIR=${0%/*}
SOURCE=/system/etc/selinux/plat_service_contexts
EXPECTED_FILE="$MODDIR/plat_service_contexts.source.sha256"
LOG=/data/local/tmp/awl_post_fs_data.log

EXPECTED=$(cat "$EXPECTED_FILE" 2>/dev/null)
ACTUAL=$(
  if [ -r "$MODDIR/service-contexts.sh" ]; then
    . "$MODDIR/service-contexts.sh" && anland_context_fingerprint "$SOURCE"
  fi
)
case "$EXPECTED" in
  ''|*[!0-9a-f]*) EXPECTED=invalid ;;
esac
if [ "${#EXPECTED}" -ne 64 ] || [ -z "$ACTUAL" ] || [ "$ACTUAL" != "$EXPECTED" ]; then
  touch "$MODDIR/disable" "$MODDIR/skip_mount" || exit 1
  echo "anland: disabled incompatible service-context overlay; reinstall for the current ROM" > "$LOG"
  exit 0
fi
# Do not clear disable/skip_mount here: re-enabling needs a fresh installation.
rm -f "$LOG"
