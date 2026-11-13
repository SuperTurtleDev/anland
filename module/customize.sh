# customize.sh — sourced by ksud when flashing the zip (env: MODPATH = install
# staging dir, ZIPFILE)

# File modes: the installer has just run `set_perm_recursive $MODPATH 0 0 0755
# 0644` over the whole tree (before sourcing this script) — the zip's modes are
# gone and every binary is 0644. service.sh chmods again at boot, but the
# flashed module should be right on its own (and readable in the manager).
set_perm "$MODPATH/waylandbridge" 0 0 0755
set_perm "$MODPATH/service.sh" 0 0 0755
set_perm "$MODPATH/post-fs-data.sh" 0 0 0755

# KernelSU does not consume a standalone *.anland service-context fragment.
# Build a system overlay from the device's live platform file so
# servicemanager labels anland.host with anland_host_service. Without this
# entry root/su can see the service, but untrusted_app_29 cannot find it.
. "$MODPATH/service-contexts.sh"
PLAT_CTX_SRC=/system/etc/selinux/plat_service_contexts
PLAT_CTX_DST="$MODPATH/system/etc/selinux/plat_service_contexts"
PLAT_CTX_HASH="$MODPATH/plat_service_contexts.source.sha256"
PLAT_CTX_ENTRY=$(cat "$MODPATH/plat_service_contexts.anland") || abort "Missing Anland service-context fragment"
PLAT_CTX_EXPECTED=$(anland_context_fingerprint "$PLAT_CTX_SRC") ||
  abort "Cannot fingerprint platform service contexts; disable Anland, reboot and reinstall"
mkdir -p "${PLAT_CTX_DST%/*}" || abort "Cannot create service-context overlay directory"
# Remove our own entry before regeneration, including an entry from an active
# installation. Keep every other platform service and insert before fallback.
awk -v anland_entry="$PLAT_CTX_ENTRY" '
  $1 == "anland.host" { next }
  $1 == "*" && !inserted {
    print anland_entry
    inserted = 1
  }
  { print }
  END {
    if (!inserted)
      print anland_entry
  }
' "$PLAT_CTX_SRC" > "$PLAT_CTX_DST" || abort "Cannot generate platform service contexts"
printf '%s\n' "$PLAT_CTX_EXPECTED" > "$PLAT_CTX_HASH" || abort "Cannot save platform fingerprint"
set_perm "$PLAT_CTX_DST" 0 0 0644
set_perm "$PLAT_CTX_HASH" 0 0 0644
echo "anland: generated ROM-bound platform service-context overlay"

if [ -d "$MODPATH/pulse/bin" ]; then
  set_perm_recursive "$MODPATH/pulse/bin" 0 0 0755 0755
  echo "anland: pulse/bin marked executable"
fi
