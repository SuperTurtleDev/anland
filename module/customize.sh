# customize.sh — sourced by ksud when flashing the zip (env: MODPATH = install
# staging dir, ZIPFILE)
#
# plat_service_contexts is NOT snapshotted into the zip in full (a snapshot
# would shadow the system's original file stale after an OTA); instead it is
# generated at flash time from the device's current system original:
#   original file + the anland.host line (inserted before the wildcard
#   default line '*' — selabel lookup order)
# Idempotent: if the original already contains anland.host (module update
# install, old overlay still mounted) → used as-is, no duplicate
# generation/append.
SRC=/system/etc/selinux/plat_service_contexts
DST=$MODPATH/system/etc/selinux/plat_service_contexts
FRAG=$MODPATH/plat_service_contexts.anland

if [ -f "$SRC" ] && [ -f "$FRAG" ]; then
  mkdir -p "${DST%/*}"
  if grep -q '^anland\.host' "$SRC"; then
    cp -f "$SRC" "$DST"
    echo "anland: contexts already contains anland.host, using the live file (no regeneration)"
  elif n=$(grep -n '^\*' "$SRC" | head -1 | cut -d: -f1) && [ -n "$n" ]; then
    head -n $((n - 1)) "$SRC" > "$DST"
    cat "$FRAG" >> "$DST"
    tail -n +$n "$SRC" >> "$DST"
    echo "anland: contexts generated (inserted before the wildcard default line '*', line $n)"
  else
    cat "$SRC" "$FRAG" > "$DST"
    echo "anland: contexts generated (original has no wildcard line, appended)"
  fi
else
  echo "anland: skipping contexts generation (missing $SRC or FRAG; the module will not mount this file)"
fi
