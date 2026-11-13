#!/system/bin/sh
# service.sh — module late_start service: the waylandbridge daemon + the
# PulseAudio bridge.
#   sh service.sh          boot (ksud): start both
#   sh service.sh pulse    (re)start only PulseAudio — dev/repair path after a
#                          module reflash or a host-APK reinstall (uid change),
#                          without touching the running daemon and its windows
#   sh service.sh pulse-recover
#                          retry AAudio in the existing PulseAudio server
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
# Keep the service-stage work bounded.  If PulseAudio itself starts but Android
# audio is not ready, retain that server/socket and let one background worker
# retry only the AAudio module.  Existing clients can use auto_null meanwhile
# and migrate when the Android sink appears.
PULSE_PROBE_TIMEOUT=2
PULSE_PROBE_POLLS=4
PULSE_START_RETRIES=2
PULSE_RECOVERY_RETRIES=24
PULSE_RECOVERY_INTERVAL=5
PULSE_RECOVERY_PID="$RT/pulse-recovery.pid"

init_pulse_paths() {
  PA="$MODDIR/pulse"
  LOG=/data/local/tmp/awl_pulse.log
  PAR="$RT/pulse"
  PH="$RT/pulse-home"
}

stop_pulse_recovery() {
  [ -f "$PULSE_RECOVERY_PID" ] || return 0

  recovery_pid=$(cat "$PULSE_RECOVERY_PID" 2>/dev/null)
  case "$recovery_pid" in
    ''|*[!0-9]*) ;;
    *)
      # Runtime storage survives reboot. A stale PID must not signal an
      # unrelated Android process after PID reuse.
      if [ -r "/proc/$recovery_pid/cmdline" ] &&
         tr '\000' '\n' < "/proc/$recovery_pid/cmdline" |
         awk -v script="$MODDIR/service.sh" '
           NR == 1 { owned = ($0 ~ /(^|\/)sh$/) }
           NR == 2 { owned = owned && ($0 == script) }
           NR == 3 { owned = owned && ($0 == "pulse-recovery") }
           END { exit !(owned && NR == 3) }
         '; then
        kill "$recovery_pid" 2>/dev/null || true
      fi
      ;;
  esac
  rm -f "$PULSE_RECOVERY_PID"
}

stop_pulse() {
  # The runtime copy is disposable, but the process must be stopped before it
  # is replaced.  Otherwise an old instance can keep the old UID/audio state
  # alive across a module update or an APK reinstall.
  stop_pulse_recovery
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
  # use the same runtime environment as the server.  The binary is relabelled
  # as awl_daemon_exec below, so timeout's child transitions out of ksu into
  # the same domain as pulseaudio before it opens the restricted socket.
  # timeout is provided by Android toybox and prevents a half-created socket
  # from blocking boot.
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
  # both Android output modules reject the stream.  SUSPENDED is valid here
  # when idle.  Accept either backend because default.pa documents both.
  printf '%s\n' "$SINKS" | awk \
    '$3 == "module-sles-sink.c" || $3 == "module-aaudio-sink.c" { found = 1 }
     END { exit(found ? 0 : 1) }'
}

pulse_server_is_ready() {
  [ -S "$RT/pulse.sock" ] || return 1
  pulse_query info >/dev/null 2>>"$LOG"
}

load_aaudio_sink() {
  if pulse_is_ready; then
    return 0
  fi

  module_id=$(pulse_query load-module module-aaudio-sink \
    sink_name=android sink_properties=device.description=Android \
    no_close_hack=1 2>>"$LOG")
  status=$?
  if [ "$status" -ne 0 ]; then
    echo "anland: delayed AAudio load failed (pactl status $status)" >> "$LOG"
    return 1
  fi

  echo "anland: delayed AAudio module loaded (index $module_id)" >> "$LOG"
  if ! pulse_query set-default-sink android >/dev/null 2>>"$LOG"; then
    echo "anland: delayed AAudio default-sink selection failed" >> "$LOG"
    return 1
  fi
  pulse_is_ready
}

cleanup_pulse_recovery_pid() {
  current_pid=$(cat "$PULSE_RECOVERY_PID" 2>/dev/null)
  if [ "$current_pid" = "$$" ]; then
    rm -f "$PULSE_RECOVERY_PID"
  fi
}

run_pulse_recovery() {
  trap 'cleanup_pulse_recovery_pid; exit 0' HUP INT TERM
  trap 'cleanup_pulse_recovery_pid' EXIT

  attempt=1
  while [ "$attempt" -le "$PULSE_RECOVERY_RETRIES" ]; do
    sleep "$PULSE_RECOVERY_INTERVAL"

    if ! pulse_server_is_ready; then
      echo "anland: delayed AAudio recovery stopped (PulseAudio unavailable)" >> "$LOG"
      return 1
    fi
    if load_aaudio_sink; then
      echo "anland: delayed AAudio recovery ready (attempt $attempt/$PULSE_RECOVERY_RETRIES)" >> "$LOG"
      return 0
    fi

    echo "anland: delayed AAudio recovery attempt $attempt/$PULSE_RECOVERY_RETRIES failed" >> "$LOG"
    attempt=$((attempt + 1))
  done

  echo "anland: delayed AAudio recovery exhausted after $PULSE_RECOVERY_RETRIES attempts" >> "$LOG"
  return 1
}

start_pulse_recovery() {
  stop_pulse_recovery
  nohup sh "$MODDIR/service.sh" pulse-recovery >> "$LOG" 2>&1 &
  recovery_pid=$!
  echo "$recovery_pid" > "$PULSE_RECOVERY_PID"
  echo "anland: delayed AAudio recovery started (pid $recovery_pid; $PULSE_RECOVERY_RETRIES attempts every ${PULSE_RECOVERY_INTERVAL}s)" >> "$LOG"
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
  if ! rm -rf "$PH" || ! mkdir -p "$PH/run" "$PH/state"; then
    echo "anland: pulse attempt $attempt failed (cannot create $PH)" >> "$LOG"
    return 1
  fi
  if ! chown -R "$PAUID:$PAUID" "$PH" ||
     ! chmod 700 "$PH" "$PH/run" "$PH/state"; then
    echo "anland: pulse attempt $attempt failed (cannot prepare $PH)" >> "$LOG"
    return 1
  fi
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
  init_pulse_paths
  : > "$LOG"
  if [ ! -f "$PA/bin/pulseaudio" ]; then
    stop_pulse
    echo "anland: pulse skipped ($PA/bin/pulseaudio missing — module built without pulse/)" >> "$LOG"
    return
  fi
  chmod 755 "$PA/bin"/* 2>/dev/null
  PAUID=$(awk '$1=="com.anlandnext"{print $2; exit}' /data/system/packages.list 2>/dev/null)
  if [ -z "$PAUID" ]; then
    stop_pulse
    echo "anland: pulse skipped (com.anlandnext not installed)" >> "$LOG"
    return
  fi
  # a previous instance (repair path, or an app reinstall that changed the
  # uid) still holds the old tree and socket — replace it whole
  stop_pulse
  rm -rf "$PAR"
  if ! cp -r "$PA" "$PAR"; then
    echo "anland: pulse failed (cannot copy $PA to $PAR)" >> "$LOG"
    return 1
  fi
  chmod -R 755 "$PAR"
  if ! chcon u:object_r:awl_daemon_exec:s0 "$PAR/bin/pulseaudio" 2>/dev/null; then
    echo "anland: pulse failed (cannot label $PAR/bin/pulseaudio)" >> "$LOG"
    return 1
  fi
  # service.sh itself runs in ksu, but sepolicy deliberately denies ksu access
  # to awl_runtime_sock.  Relabelling pactl makes the child of timeout follow
  # the existing ksu → awl_daemon transition before connecting to pulse.sock.
  if [ -x "$PAR/bin/pactl" ] &&
     ! chcon u:object_r:awl_daemon_exec:s0 "$PAR/bin/pactl" 2>/dev/null; then
    echo "anland: pulse failed (cannot label $PAR/bin/pactl)" >> "$LOG"
    return 1
  fi
  # module lookup: daemon.conf is read from PULSE_CONFIG_PATH (this copy)
  echo "dl-search-path = $PAR/lib/pulseaudio/modules" >> "$PAR/etc/pulse/daemon.conf"

  attempt=1
  while [ "$attempt" -le "$PULSE_START_RETRIES" ]; do
    stop_pulse
    if launch_pulse "$attempt" && wait_for_pulse; then
      echo "anland: pulse ready (Android sink)" >> "$LOG"
      return 0
    fi

    echo "anland: pulse attempt $attempt/$PULSE_START_RETRIES did not produce an Android sink" >> "$LOG"
    if pulse_server_is_ready; then
      echo "anland: pulse retained with fallback sink; deferring AAudio load" >> "$LOG"
      start_pulse_recovery
      return 0
    fi

    stop_pulse
    if [ "$attempt" -lt "$PULSE_START_RETRIES" ]; then
      sleep 1
    fi
    attempt=$((attempt + 1))
  done

  rm -f "$RT/pulse.sock"
  echo "anland: pulse failed after $PULSE_START_RETRIES attempts (no Android sink)" >> "$LOG"
  return 1
}

case "${1:-}" in
  pulse) start_pulse ;;
  pulse-recover)
    init_pulse_paths
    if pulse_server_is_ready; then
      start_pulse_recovery
    else
      echo "anland: delayed AAudio recovery not started (PulseAudio unavailable)" >> "$LOG"
      exit 1
    fi
    ;;
  pulse-recovery)
    init_pulse_paths
    run_pulse_recovery
    ;;
  *)     start_daemon; start_pulse ;;
esac
