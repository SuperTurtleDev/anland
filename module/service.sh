#!/system/bin/sh
# service.sh — module late_start service: the waylandbridge daemon + the
# PulseAudio bridge.
#   sh service.sh          boot (ksud): start both
#   sh service.sh pulse    (re)start only PulseAudio — dev/repair path after a
#                          module reflash or a host-APK reinstall (uid change),
#                          without touching the running daemon and its windows
#
# NOTE on file modes: ksud's installer runs `set_perm_recursive $MODPATH 0 0
# 0755 0644` on the extracted zip BEFORE customize.sh — every file lands 0644,
# executables included. customize.sh restores them at flash time; the chmods
# below repeat that at boot so a module dir refreshed by other means (deploy
# script, manual copy) still starts.
MODDIR=${0%/*}
# wayland socket dir: manual-only config key runtime_dir (config.json);
# default /data/local/tmp/awl — keep in sync with waylandbridge.cpp cfg_load_sock_dir
RT=$(sed -n 's/.*"runtime_dir": *"\([^"]*\)".*/\1/p' "$MODDIR/config.json" 2>/dev/null | head -1)
case "$RT" in /*) ;; *) RT=/data/local/tmp/awl ;; esac

start_daemon() {
  chmod 755 "$MODDIR/waylandbridge" 2>/dev/null
  # custom SELinux domain: relabel → exec transitions into awl_daemon
  # (module sepolicy.rule is applied by ksud before service stage)
  chcon u:object_r:awl_daemon_exec:s0 "$MODDIR/waylandbridge" 2>/dev/null
  rm -f "$RT/wayland-0"
  nohup "$MODDIR/waylandbridge" > /data/local/tmp/awl_daemon.log 2>&1 &
}

# ---- PulseAudio (module pulse/: Termux OpenSL ES / AAudio sink) — the Linux
#      container's apps play through Android ----
# MUST run under the host APK's uid: Android 12+ AudioPolicyService rejects
# stream creation from a process whose uid resolves to no package (root →
# createTrack_l INVALID_OPERATION, OpenSL ES error 9 / AAudio -896; Termux's
# pulseaudio works the same way — as the app). su <uid> keeps the awl_daemon
# domain (permissive) via the exec label; the app uid needs no app running.
# /data/adb is 0700 root → the tree is copied out to $RT/pulse each boot
# (module search path + libs point there: PULSE_CONFIG_PATH + daemon.conf
# dl-search-path + LD_LIBRARY_PATH — all runtime-dir relative, no baked path).
# Socket: $RT/pulse.sock (container view /run/anland/pulse.sock) with
# anonymous auth (the per-user cookie cannot cross into the container).
# pulseaudio insists on a 0700 runtime/state dir of its own → $RT/pulse-home.
# Log: /data/local/tmp/awl_pulse.log (root-owned fd, inherited by the child).
# Every skip path writes its reason there — a silent skip is what made "no
# sound" undiagnosable after a reflash.
PULSE_PROBE_TIMEOUT=3
PULSE_PROBE_POLLS=6
PULSE_START_RETRIES=3

stop_pulse() {
  # The runtime copy is disposable, but the process must be stopped before it
  # is replaced.  Otherwise an old instance can keep the old UID/audio state
  # alive across a module update or an APK reinstall.
  if pkill -f "$PAR/bin/pulseaudio" 2>/dev/null; then
    sleep 1
    # A stuck instance must not survive into the next attempt and race the
    # new server for the socket or AudioFlinger track.
    pkill -KILL -f "$PAR/bin/pulseaudio" 2>/dev/null || true
  fi
  rm -f "$RT/pulse.sock"
}

pulse_query() {
  # pactl in the staged tree is dynamically linked against the staged libpulse;
  # use the same runtime environment as the server.  timeout is provided by
  # Android toybox and prevents a half-created socket from blocking boot.
  PULSE_SERVER="unix:$RT/pulse.sock" \
  HOME="$PH" TMPDIR="$PH" \
  LD_LIBRARY_PATH="$PAR/lib:$PAR/lib/pulseaudio:$PAR/lib/pulseaudio/modules" \
  timeout "$PULSE_PROBE_TIMEOUT" "$PAR/bin/pactl" "$@"
}

pulse_is_ready() {
  [ -S "$RT/pulse.sock" ] || return 1
  [ -x "$PAR/bin/pactl" ] || {
    echo "anland: pulse probe unavailable ($PAR/bin/pactl missing)" >> "$LOG"
    return 1
  }

  SINKS=$(pulse_query list short sinks 2>>"$LOG")
  status=$?
  if [ "$status" -ne 0 ]; then
    echo "anland: pulse probe failed (pactl status $status)" >> "$LOG"
    return 1
  fi

  {
    echo "anland: pulse sinks:"
    printf '%s\n' "$SINKS"
  } >> "$LOG"

  # A native socket and a successful pactl connection are insufficient: the
  # daemon can otherwise fall back to module-always-sink/auto_null after
  # OpenSL ES rejects the stream.  SUSPENDED is valid here when idle.
  printf '%s\n' "$SINKS" | awk \
    '$2 == "android" && $3 == "module-sles-sink.c" { found = 1 }
     END { exit(found ? 0 : 1) }'
}

wait_for_pulse() {
  probe=1
  while [ "$probe" -le "$PULSE_PROBE_POLLS" ]; do
    if pulse_is_ready; then
      return 0
    fi
    if [ "$probe" -lt "$PULSE_PROBE_POLLS" ]; then
      sleep 1
    fi
    probe=$((probe + 1))
  done
  return 1
}

launch_pulse() {
  attempt="$1"
  rm -rf "$PH"
  mkdir -p "$PH/run" "$PH/state" || {
    echo "anland: pulse attempt $attempt failed (cannot create $PH)" >> "$LOG"
    return 1
  }
  chown -R "$PAUID:$PAUID" "$PH"
  chmod 700 "$PH" "$PH/run" "$PH/state"
  rm -f "$RT/pulse.sock"
  echo "anland: pulse starting as uid $PAUID (attempt $attempt/$PULSE_START_RETRIES; tree $PAR, socket $RT/pulse.sock)" >> "$LOG"
  nohup su "$PAUID" -c "export HOME='$PH' TMPDIR='$PH' PULSE_RUNTIME_PATH='$PH/run' \
PULSE_STATE_PATH='$PH/state' PULSE_CONFIG_PATH='$PAR/etc/pulse' \
LD_LIBRARY_PATH='$PAR/lib:$PAR/lib/pulseaudio:$PAR/lib/pulseaudio/modules'; \
exec '$PAR/bin/pulseaudio' --daemonize=no --exit-idle-time=-1 --disallow-exit \
--log-target=stderr -n -F '$PAR/etc/pulse/default.pa' \
-L 'module-native-protocol-unix auth-anonymous=1 socket=$RT/pulse.sock'" \
    >> "$LOG" 2>&1 &
}

start_pulse() {
  PA="$MODDIR/pulse"
  LOG=/data/local/tmp/awl_pulse.log
  if [ ! -f "$PA/bin/pulseaudio" ]; then
    echo "anland: pulse skipped ($PA/bin/pulseaudio missing — module built without pulse/)" > "$LOG"
    return
  fi
  chmod 755 "$PA/bin"/* 2>/dev/null
  PAUID=$(awk '$1=="com.anlandnext"{print $2; exit}' /data/system/packages.list 2>/dev/null)
  if [ -z "$PAUID" ]; then
    echo "anland: pulse skipped (com.anlandnext not installed)" > "$LOG"
    return
  fi
  PAR="$RT/pulse"
  # a previous instance (repair path, or an app reinstall that changed the
  # uid) still holds the old tree and socket — replace it whole
  PH="$RT/pulse-home"
  stop_pulse
  rm -rf "$PAR"
  cp -r "$PA" "$PAR" || {
    echo "anland: pulse failed (cannot copy $PA to $PAR)" > "$LOG"
    return 1
  }
  chmod -R 755 "$PAR"
  chcon u:object_r:awl_daemon_exec:s0 "$PAR/bin/pulseaudio" 2>/dev/null
  # module lookup: daemon.conf is read from PULSE_CONFIG_PATH (this copy)
  echo "dl-search-path = $PAR/lib/pulseaudio/modules" >> "$PAR/etc/pulse/daemon.conf"

  attempt=1
  while [ "$attempt" -le "$PULSE_START_RETRIES" ]; do
    stop_pulse
    launch_pulse "$attempt"
    if wait_for_pulse; then
      echo "anland: pulse ready (android/module-sles-sink.c)" >> "$LOG"
      return 0
    fi

    echo "anland: pulse attempt $attempt/$PULSE_START_RETRIES did not produce an Android sink" >> "$LOG"
    stop_pulse
    if [ "$attempt" -lt "$PULSE_START_RETRIES" ]; then
      case "$attempt" in
        1) delay=1 ;;
        2) delay=2 ;;
        *) delay=4 ;;
      esac
      sleep "$delay"
    fi
    attempt=$((attempt + 1))
  done

  rm -f "$RT/pulse.sock"
  echo "anland: pulse failed after $PULSE_START_RETRIES attempts (no android/module-sles-sink.c)" >> "$LOG"
  return 1
}

case "${1:-}" in
  pulse) start_pulse ;;
  *)     start_daemon; start_pulse ;;
esac
