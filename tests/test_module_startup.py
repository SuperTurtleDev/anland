#!/usr/bin/env python3
"""Run actual service functions against private process/domain fixtures."""
import os
from pathlib import Path
import re
import signal
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / 'module/service.sh').read_text()


def function(name):
    return re.search(r'^' + name + r'\(\) \{.*?^}', SOURCE, re.M | re.S).group()


class StartupTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='anland-startup-')
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.runtime = self.base / 'runtime'
        self.runtime.mkdir()
        self.daemon = self.base / 'waylandbridge'
        self.daemon.write_text('#!/bin/sh\nexec /bin/sleep 20\n')
        self.daemon.chmod(0o755)
        self.busybox = self.base / 'busybox'
        self.busybox.write_text('''#!/bin/sh
case "$1" in
  sleep) exec /bin/sleep 20 ;;
  nohup) shift; exec "$@" ;;
  --list) [ "${HAS_SETUIDGID:-1}" = 1 ] && echo setuidgid; exit 0 ;;
  setuidgid)
    shift
    printf '%s\\n' "$1" > "$FIXTURE/uid-request"
    shift
    [ "$1" = /system/bin/sh ] && shift
    exec /bin/sh "$@"
    ;;
esac
exit 1
''')
        self.busybox.chmod(0o755)

    def run_function(self, name, domains=(), missing=False, **variables):
        (self.base / 'domains').write_text('\n'.join(domains) + '\n')
        (self.base / 'reads').write_text('0')
        env = dict(os.environ, FIXTURE=str(self.base), MODDIR=str(self.base),
                   RT=str(self.runtime), SERVICE_LOG=str(self.base / 'service.log'),
                   KSU_BUSYBOX=str(self.busybox) if not missing else str(self.base / 'absent'),
                   DAEMON_POLICY_RETRIES='3', LOG=str(self.base / 'pulse.log'),
                   PH=str(self.base / 'pulse-home'), PAR=str(self.base / 'pulse'), PAUID='12345',
                   PULSE_START_RETRIES='2', **variables)
        pulse = self.base / 'pulse/bin'
        pulse.mkdir(parents=True)
        (pulse / 'pulseaudio').write_text('#!/bin/sh\nexit 0\n')
        (pulse / 'pulseaudio').chmod(0o755)
        mocks = '''
sleep() { /bin/sleep 0.02; }
chown() { :; }
chcon() {
  printf '%s\\n' "$*" >> "$FIXTURE/labels"
  [ "${FAIL_REAL_LABEL:-0}" != 1 ] || [ "$2" != "$MODDIR/waylandbridge" ]
}
cat() {
  case "$1" in
    /proc/*/attr/current)
      n=$(/bin/cat "$FIXTURE/reads")
      n=$((n+1))
      echo "$n" > "$FIXTURE/reads"
      /usr/bin/sed -n "${n}p" "$FIXTURE/domains"
      ;;
    *) /bin/cat "$@" ;;
  esac
}
kill() { echo "$*" >> "$FIXTURE/kills"; command kill "$@"; }
'''
        body = function(name).replace('/data/local/tmp/awl_daemon.log', str(self.base / 'daemon.log'))
        process = subprocess.Popen(['sh', '-c', mocks + body + '\n' + name +
                                    '; result=$?; /bin/sleep 0.05; echo RESULT=$result'],
                                   env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                   text=True, start_new_session=True)
        try:
            stdout, stderr = process.communicate(timeout=5)
        finally:
            try:
                os.killpg(process.pid, signal.SIGTERM)
            except ProcessLookupError:
                pass
        self.assertEqual(process.returncode, 0, stderr)
        return stdout

    def test_ready_domain_starts_foreground_child(self):
        self.assertIn('RESULT=0', self.run_function('start_daemon', ['u:r:awl_daemon:s0'] * 2))
        labels = (self.base / 'labels').read_text()
        self.assertIn('u:object_r:system_file:s0', labels)
        self.assertIn('u:object_r:awl_daemon_exec:s0', labels)
        self.assertFalse((self.runtime / '.domain-probe').exists())

    def test_probe_retry_then_success(self):
        values = ['u:r:ksu:s0', 'u:r:awl_daemon:s0', 'u:r:awl_daemon:s0']
        self.assertIn('RESULT=0', self.run_function('start_daemon', values))
        self.assertIn('attempt 2/3', (self.base / 'service.log').read_text())

    def test_wrong_daemon_is_reaped_before_retry(self):
        values = ['u:r:awl_daemon:s0', 'u:r:ksu:s0'] + ['u:r:awl_daemon:s0'] * 2
        self.assertIn('RESULT=0', self.run_function('start_daemon', values))
        self.assertIn('rejecting daemon attempt 1/3', (self.base / 'service.log').read_text())
        self.assertFalse((self.runtime / '.domain-probe').exists())

    def test_unavailable_probe_domain_is_bounded(self):
        self.assertIn('RESULT=1', self.run_function('start_daemon', ['u:r:ksu:s0'] * 3))
        self.assertEqual((self.base / 'reads').read_text().strip(), '3')
        self.assertFalse((self.runtime / '.domain-probe').exists())

    def test_real_domain_failure_is_bounded(self):
        values = ['u:r:awl_daemon:s0', 'u:r:ksu:s0'] * 3
        self.assertIn('RESULT=1', self.run_function('start_daemon', values))
        self.assertEqual((self.base / 'reads').read_text().strip(), '6')

    def test_label_failure_does_not_accept_daemon(self):
        self.assertIn('RESULT=1', self.run_function('start_daemon',
                      ['u:r:awl_daemon:s0'] * 3, FAIL_REAL_LABEL='1'))
        self.assertFalse((self.runtime / '.domain-probe').exists())

    def test_missing_busybox_fails_before_probe(self):
        self.assertIn('RESULT=1', self.run_function('start_daemon', missing=True))
        self.assertFalse((self.runtime / '.domain-probe').exists())

    def test_audio_uses_uid_gid_applet(self):
        self.assertIn('RESULT=0', self.run_function('launch_pulse'))
        self.assertEqual((self.base / 'uid-request').read_text().strip(), '12345:12345')

    def test_missing_uid_switch_applet_rejects_launch(self):
        self.assertIn('RESULT=1', self.run_function('launch_pulse', HAS_SETUIDGID='0'))
        self.assertFalse((self.base / 'uid-request').exists())


if __name__ == '__main__':
    unittest.main()
