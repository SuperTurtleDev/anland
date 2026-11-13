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

SERVICE_LOG=/data/local/tmp/awl_service.log
DAEMON_POLICY_RETRIES=90
KSU_BUSYBOX=/data/adb/ksu/bin/busybox

start_daemon() {
  echo "anland: service start (boot $(cat /proc/sys/kernel/random/boot_id 2>/dev/null))" >> "$SERVICE_LOG"
  chmod 755 "$MODDIR/waylandbridge" 2>/dev/null
  if [ ! -x "$KSU_BUSYBOX" ]; then
    echo "anland: daemon not started (KernelSU BusyBox unavailable)" >> "$SERVICE_LOG"
    return 1
  fi
  # ReSukiSU 35229 can invoke module service scripts before a module's custom
  # SELinux transition is usable. A retained awl_daemon_exec xattr can make
  # chcon succeed during that interval, so the file label is not a sufficient
  # readiness probe. Use a disposable BusyBox copy to test the actual process
  # transition without repeatedly initializing the full Binder/Wayland daemon.
  mkdir -p "$RT/.domain-probe" || return 1
  probe="$RT/.domain-probe/busybox"
  if ! cp "$KSU_BUSYBOX" "$probe" || ! chmod 700 "$probe"; then
    echo "anland: daemon not started (cannot prepare SELinux domain probe)" >> "$SERVICE_LOG"
    rm -f "$probe"
    rmdir "$RT/.domain-probe" 2>/dev/null || true
    return 1
  fi

  attempt=1
  while [ "$attempt" -le "$DAEMON_POLICY_RETRIES" ]; do
    probe_domain=""
    if chcon u:object_r:awl_daemon_exec:s0 "$probe" 2>/dev/null; then
      "$probe" sleep 2 &
      probe_pid=$!
      sleep 0.2
      probe_domain=$(cat "/proc/$probe_pid/attr/current" 2>/dev/null)
      kill "$probe_pid" 2>/dev/null || true
      wait "$probe_pid" 2>/dev/null || true
    fi
    if [ "$probe_domain" = "u:r:awl_daemon:s0" ]; then
      echo "anland: SELinux transition ready (probe $attempt/$DAEMON_POLICY_RETRIES)" >> "$SERVICE_LOG"
      # The bridge may already carry awl_daemon_exec on disk when Android
      # first reads it, before KernelSU has installed the custom type. SELinux
      # then leaves that inode context unmapped, and writing the identical
      # xattr later does not reliably refresh the cached inode security state.
      # Bounce through a platform context after the disposable probe proves
      # the custom transition is live, then restore the intended exec label.
      if chcon u:object_r:system_file:s0 "$MODDIR/waylandbridge" 2>/dev/null &&
         chcon u:object_r:awl_daemon_exec:s0 "$MODDIR/waylandbridge" 2>/dev/null; then
        rm -f "$RT/wayland-0"
        # Run the bridge in its supported foreground mode under BusyBox
        # nohup. Without -f the bridge daemonizes itself, so $! identifies
        # its exited parent rather than the process whose SELinux domain must
        # be verified.
        "$KSU_BUSYBOX" nohup "$MODDIR/waylandbridge" -f > /data/local/tmp/awl_daemon.log 2>&1 &
        daemon_pid=$!
        sleep 1
        daemon_domain=$(cat "/proc/$daemon_pid/attr/current" 2>/dev/null)
        if kill -0 "$daemon_pid" 2>/dev/null &&
           [ "$daemon_domain" = "u:r:awl_daemon:s0" ]; then
          echo "anland: daemon domain ready (attempt $attempt/$DAEMON_POLICY_RETRIES; pid $daemon_pid)" >> "$SERVICE_LOG"
          rm -f "$probe"
          rmdir "$RT/.domain-probe" 2>/dev/null || true
          return 0
        fi

        # The disposable probe can transition a fraction earlier than this
        # executable during cold boot. Reject and reap only the verified
        # foreground child, then keep the same bounded readiness loop going.
        echo "anland: rejecting daemon attempt $attempt/$DAEMON_POLICY_RETRIES (pid $daemon_pid; domain ${daemon_domain:-exited})" >> "$SERVICE_LOG"
        kill "$daemon_pid" 2>/dev/null || true
        wait "$daemon_pid" 2>/dev/null || true
      fi
    else
      case "$attempt" in
        1|10|20|30|40|50|60|70|80|90)
          echo "anland: waiting for SELinux transition (probe $attempt/$DAEMON_POLICY_RETRIES; domain ${probe_domain:-unavailable})" >> "$SERVICE_LOG"
          ;;
      esac
    fi
    [ "$attempt" -lt "$DAEMON_POLICY_RETRIES" ] && sleep 1
    attempt=$((attempt + 1))
  done
  rm -f "$probe"
  rmdir "$RT/.domain-probe" 2>/dev/null || true

  echo "anland: daemon not started (SELinux transition unavailable after $DAEMON_POLICY_RETRIES attempts)" >> "$SERVICE_LOG"
  return 1
}

# ---- PulseAudio (module pulse/: Termux OpenSL ES / AAudio sink) — the Linux
#      container's apps play through Android ----
# MUST run under the host APK's uid: Android 12+ AudioPolicyService rejects
# stream creation from a process whose uid resolves to no package (root →
# createTrack_l INVALID_OPERATION, OpenSL ES error 9 / AAudio -896; Termux's
# pulseaudio works the same way — as the app). KernelSU BusyBox setuidgid
# changes uid/gid while the exec label transitions into awl_daemon; the app uid
# needs no app running.
# /data/adb is 0700 root → the tree is copied out to $RT/pulse each boot
# (module search path + libs point there: PULSE_CONFIG_PATH + daemon.conf
# dl-search-path + LD_LIBRARY_PATH — all runtime-dir relative, no baked path).
# Socket: $RT/pulse.sock (container view /run/anland/pulse.sock) with
# anonymous auth (the per-user cookie cannot cross into the container).
# pulseaudio insists on a 0700 runtime/state dir of its own → $RT/pulse-home.
# Log: /data/local/tmp/awl_pulse.log (root-owned fd, inherited by the child).
# Every skip path writes its reason there — a silent skip is what made "no
# sound" undiagnosable after a reflash.
# Keep boot bounded: 2 attempts × (4 probes × 2s + 3 intervals) + 1s backoff
# is at most about 23s when the audio service is unavailable.
PULSE_PROBE_TIMEOUT=2
PULSE_PROBE_POLLS=4
PULSE_START_RETRIES=2

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
  if [ ! -x "$KSU_BUSYBOX" ] ||
     ! "$KSU_BUSYBOX" --list 2>/dev/null | grep -qx setuidgid; then
    echo "anland: pulse attempt $attempt failed (KernelSU BusyBox setuidgid unavailable)" >> "$LOG"
    return 1
  fi
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
  nohup "$KSU_BUSYBOX" setuidgid "$PAUID:$PAUID" /system/bin/sh -c \
    "export HOME='$PH' TMPDIR='$PH' PULSE_RUNTIME_PATH='$PH/run' \
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
  PAR="$RT/pulse"
  PH="$RT/pulse-home"
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
  *)     start_daemon; start_pulse ;;
esac
