#!/system/bin/sh
MODDIR=${0%/*}
SOCK=/data/local/tmp/display_daemon.sock
rm -f "$SOCK"

# Ecosystem Hygiene: Clean stale/orphaned anland sockets in Droidspaces Net directory
if [ -d "/data/local/Droidspaces/Net" ]; then
    find /data/local/Droidspaces/Net -maxdepth 1 -name "anland-*.sock" -type s -mmin +60 -exec rm -f {} + 2>/dev/null
fi

"$MODDIR/display_daemon" "$SOCK" &
