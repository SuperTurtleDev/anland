#!/system/bin/sh
MODDIR=${0%/*}
chmod 755 "$MODDIR/waylandbridge" 2>/dev/null
# custom SELinux domain: relabel → exec transitions into awl_daemon
# (module sepolicy.rule is applied by ksud before service stage)
chcon u:object_r:awl_daemon_exec:s0 "$MODDIR/waylandbridge" 2>/dev/null
# wayland socket dir: manual-only config key runtime_dir (config.json);
# default /data/local/tmp/awl — keep in sync with waylandbridge.cpp cfg_load_sock_dir
RT=$(sed -n 's/.*"runtime_dir": *"\([^"]*\)".*/\1/p' "$MODDIR/config.json" 2>/dev/null | head -1)
case "$RT" in /*) ;; *) RT=/data/local/tmp/awl ;; esac
rm -f "$RT/wayland-0"
nohup "$MODDIR/waylandbridge" > /data/local/tmp/awl_daemon.log 2>&1 &
