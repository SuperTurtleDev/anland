#!/usr/bin/env python3
"""Test service decisions in private fixtures and the actual resume function."""
import os
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / 'module/service.sh').read_text()


def function(name):
    return re.search(r'^' + name + r'\(\) \{.*?^}', SOURCE, re.M | re.S).group()


class RecoveryTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix='anland-audio-test-')
        self.addCleanup(self.temp.cleanup)
        self.base = Path(self.temp.name)
        self.pidfile = self.base / 'worker.pid'

    def run_shell(self, names, command, mocks=''):
        env = dict(os.environ, FIXTURE=str(self.base), MODDIR=str(self.base),
                   PULSE_RECOVERY_PID=str(self.pidfile), LOG=str(self.base / 'audio.log'),
                   PULSE_RECOVERY_RETRIES='3', PULSE_RECOVERY_INTERVAL='0')
        definitions = '\n'.join(function(name) for name in names)
        definitions = definitions.replace('/proc/$recovery_pid/cmdline',
                                          '$FIXTURE/proc/$recovery_pid/cmdline')
        script = definitions + '\n' + mocks + '\n' + command + '; echo RESULT=$?'
        result = subprocess.run(['sh', '-c', script], env=env, text=True,
                                capture_output=True, timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout

    def test_stale_pid_is_not_signalled(self):
        self.pidfile.write_text('1234')
        path = self.base / 'proc/1234'
        path.mkdir(parents=True)
        (path / 'cmdline').write_bytes(b'/system/bin/system_server\0')
        self.run_shell(['stop_pulse_recovery'], 'stop_pulse_recovery',
                       'kill() { echo "$*" > "$FIXTURE/killed"; }')
        self.assertFalse((self.base / 'killed').exists())
        self.assertFalse(self.pidfile.exists())

    def test_owned_worker_is_signalled(self):
        self.pidfile.write_text('1234')
        path = self.base / 'proc/1234'
        path.mkdir(parents=True)
        (path / 'cmdline').write_bytes(b'sh\0' + str(self.base / 'service.sh').encode() + b'\0pulse-recovery\0')
        self.run_shell(['stop_pulse_recovery'], 'stop_pulse_recovery',
                       'kill() { echo "$*" > "$FIXTURE/killed"; }')
        self.assertEqual((self.base / 'killed').read_text().strip(), '1234')

    def test_invalid_pid_is_discarded(self):
        self.pidfile.write_text('not-a-pid')
        self.run_shell(['stop_pulse_recovery'], 'stop_pulse_recovery',
                       'kill() { echo "$*" > "$FIXTURE/killed"; }')
        self.assertFalse((self.base / 'killed').exists())

    def test_ready_sink_avoids_module_reload(self):
        out = self.run_shell(['load_aaudio_sink'], 'load_aaudio_sink',
                            'pulse_is_ready() { return 0; }; pulse_query() { return 99; }')
        self.assertIn('RESULT=0', out)

    def test_failed_module_load_returns_failure(self):
        out = self.run_shell(['load_aaudio_sink'], 'load_aaudio_sink',
                            'pulse_is_ready() { return 1; }; pulse_query() { return 1; }')
        self.assertIn('RESULT=1', out)

    def test_successful_load_selects_default_sink(self):
        mocks = '''
pulse_is_ready() { test -e "$FIXTURE/ready"; }
pulse_query() {
  echo "$*" >> "$FIXTURE/queries"
  case "$1" in load-module) touch "$FIXTURE/ready"; echo 7 ;; esac
}
'''
        out = self.run_shell(['load_aaudio_sink'], 'load_aaudio_sink', mocks)
        self.assertIn('RESULT=0', out)
        self.assertIn('set-default-sink android', (self.base / 'queries').read_text())

    def test_worker_retry_budget(self):
        mocks = '''
sleep() { :; }
pulse_server_is_ready() { return 0; }
load_aaudio_sink() { echo attempt >> "$FIXTURE/attempts"; return 1; }
'''
        out = self.run_shell(['cleanup_pulse_recovery_pid', 'run_pulse_recovery'],
                             'run_pulse_recovery', mocks)
        self.assertIn('RESULT=1', out)
        self.assertEqual(len((self.base / 'attempts').read_text().splitlines()), 3)

    def test_worker_stops_after_server_loss(self):
        mocks = '''
sleep() { :; }
pulse_server_is_ready() { return 1; }
load_aaudio_sink() { echo bad > "$FIXTURE/loaded"; return 0; }
'''
        out = self.run_shell(['cleanup_pulse_recovery_pid', 'run_pulse_recovery'],
                             'run_pulse_recovery', mocks)
        self.assertIn('RESULT=1', out)
        self.assertFalse((self.base / 'loaded').exists())

    def test_pid_cleanup_preserves_replacement_owner(self):
        self.pidfile.write_text('999999')
        self.run_shell(['cleanup_pulse_recovery_pid'], 'cleanup_pulse_recovery_pid')
        self.assertTrue(self.pidfile.exists())

    def test_resume_state_matrix(self):
        module = (ROOT / 'pulse/termux/module-aaudio-sink.c').read_text()
        body = re.search(r'static void resume_stream\(.*?^}', module, re.M | re.S).group()
        harness = r'''
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
typedef int aaudio_result_t;
typedef int aaudio_stream_state_t;
#define AAUDIO_ERROR_DISCONNECTED -899
#define AAUDIO_STREAM_STATE_DISCONNECTED 13
#define SINK_MESSAGE_STREAM_REOPENED 1
#define PA_MSGOBJECT(x) (x)
struct rate { unsigned rate; };
struct sink { struct rate sample_spec; };
struct userdata { void *stream; struct sink *sink; int no_close; struct rate ss; struct {void *inq;} thread_mq; };
static int result, state, opens, stops, closes, posts, open_result, second_start;
static int AAudioStream_requestStart(void *p) { (void)p; return opens ? second_start : result; }
static int AAudioStream_getState(void *p) { (void)p; return state; }
static void AAudioStream_requestStop(void *p) { (void)p; stops++; }
static void AAudioStream_close(void *p) { (void)p; closes++; }
static int pa_open_aaudio_stream(struct userdata *u, unsigned rate) { opens++; u->stream=(void*)2; u->ss.rate=rate; return open_result; }
#define pa_log(...) ((void)0)
#define pa_log_warn(...) ((void)0)
#define pa_asyncmsgq_post(...) (posts++)
'''
        harness += body + r'''
int main(void) {
 struct sink sink={{48000}};
 for (int test=0; test<8; test++) {
  struct userdata u={.stream=(void*)1,.sink=&sink,.ss={48000}};
  result=0; state=1; opens=stops=closes=posts=open_result=second_start=0;
  if(test==1) result=-1;
  if(test==2 || test==3 || test==5 || test==6 || test==7) state=AAUDIO_STREAM_STATE_DISCONNECTED;
  if(test==3) result=AAUDIO_ERROR_DISCONNECTED;
  if(test==4) u.stream=NULL;
  if(test==5) open_result=-1;
  if(test==6) second_start=-1;
  if(test==7) u.no_close=1;
  resume_stream(&u);
  if(test<2) { assert(opens==0 && posts==0); }
  else { assert(opens==1); assert(posts==((test==5 || test==6)?0:1)); }
  if(test==7) assert(stops==1 && closes==0);
 }
 return 0;
}
'''
        source = self.base / 'resume.c'
        source.write_text(harness)
        binary = self.base / 'resume'
        subprocess.run(['cc', '-std=c11', '-Wall', '-Werror', str(source), '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main()
