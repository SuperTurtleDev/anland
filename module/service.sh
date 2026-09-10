#!/system/bin/sh
MODDIR=${0%/*}
chmod 755 "$MODDIR/waylandbridge" 2>/dev/null
rm -f /data/local/tmp/awl/wayland-0
nohup "$MODDIR/waylandbridge" > /data/local/tmp/awl_daemon.log 2>&1 &
