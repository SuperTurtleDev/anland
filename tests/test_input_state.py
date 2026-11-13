#!/usr/bin/env python3
"""Compile the actual seat transition functions against event-recording stubs."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]


class InputStateTest(unittest.TestCase):
    def test_seat_lifetime_and_motion_modifiers(self):
        source = (ROOT / 'services/waylandbridge/awl_input.c').read_text()
        functions = '\n'.join(re.search(r'static void ' + name + r'\(.*?^}', source, re.M | re.S).group()
                              for name in ['tr_kbd_leave', 'tr_modifiers'])
        harness = r'''
#define _POSIX_C_SOURCE 200809L
#include <stdint.h>
#include <string.h>
#include <assert.h>
#include <pthread.h>
#include "awl_modifier_state.h"
#define WL_KEYBOARD_KEY_STATE_RELEASED 0
#define AWL_DMOD_CTRL 1
#define AWL_DMOD_SHIFT 2
#define LOGI(...) ((void)0)
struct wl_resource { int value; } resource;
struct awl_surface { pthread_mutex_t ev_lock; struct wl_resource *resource; } surface={PTHREAD_MUTEX_INITIALIZER,&resource};
struct { pthread_rwlock_t rwl; void *display; } g_srv={PTHREAD_RWLOCK_INITIALIZER,0};
static pthread_mutex_t g_input_lock=PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_kbd_owner;
static uint8_t g_keys_down[32];
static uint32_t g_mods_depressed,g_mods_locked;
static int g_kbds, live=1, releases, leaves, modifiers, ime, data, dm;
static uint32_t sent_locked;
static struct wl_resource *resolve(int *unused,uint64_t win,struct awl_surface **s) { *s=&surface; return live?&resource:0; }
static int wl_display_next_serial(void *unused) { return 1; }
static int awl_now_ms(void) { return 1; }
static void *wl_resource_get_client(void *unused) { return 0; }
static void wl_client_flush(void *unused) {}
static void wl_keyboard_send_key(void *k,int serial,int time,uint32_t key,int state) { assert(state==0); releases++; }
static void wl_keyboard_send_modifiers(void *k,int serial,uint32_t dep,uint32_t lat,uint32_t lock,uint32_t group) { modifiers++; sent_locked=lock; }
static void wl_keyboard_send_leave(void *k,int serial,void *s) { leaves++; }
static void awl_ime_set_focus(void *s,int x) { ime++; }
static void awl_ime_focus_leave(uint64_t win) { ime++; }
static void awl_datadev_key_mods(uint32_t value) { dm=value; }
static void awl_datadev_focus_leave(void) { data++; }
static void meta_to_masks(uint32_t meta,uint32_t *dep,uint32_t *lock) { *dep=meta & 0xff; *lock=meta>>8; }
'''
        harness += functions + r'''
int main(void) {
 g_kbd_owner=1;g_keys_down[29/8]|=1u<<(29%8);g_keys_down[30/8]|=1u<<(30%8);
 g_mods_depressed=4;g_mods_locked=2;
 tr_kbd_leave(2);
 assert(g_kbd_owner==1 && releases==0 && leaves==0 && ime==0 && data==0);
 tr_kbd_leave(1);
 assert(g_kbd_owner==0 && releases==2 && leaves==1 && ime==2 && data==1);
 assert(g_mods_depressed==0 && g_mods_locked==2 && sent_locked==2);
 for(int i=0;i<32;i++) assert(g_keys_down[i]==0);
 live=0;g_kbd_owner=3;g_keys_down[1]=1;g_mods_depressed=4;
 tr_kbd_leave(3);
 assert(g_kbd_owner==0 && g_keys_down[1]==0 && releases==2 && data==2);
 live=1;g_mods_depressed=4;tr_modifiers(4,0);
 assert(g_mods_depressed==0 && dm==0);
 g_keys_down[29/8]|=1u<<(29%8);tr_modifiers(4,0);
 assert(g_mods_depressed==4 && dm==AWL_DMOD_CTRL);
 memset(g_keys_down,0,32);tr_modifiers(4,(2u<<8)|1u);
 assert(g_mods_depressed==1 && g_mods_locked==2 && dm==AWL_DMOD_SHIFT);
 int keys[]={42,54,29,97,56,100,125,126};
 int masks[]={1,1,4,4,8,8,64,64};
 for(unsigned i=0;i<8;i++){memset(g_keys_down,0,32);g_keys_down[keys[i]/8]=1u<<(keys[i]%8);assert(awl_held_modifiers(g_keys_down)==(unsigned)masks[i]);}
 return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='anland-seat-test-') as directory:
            path = Path(directory)
            (path / 'seat.c').write_text(harness)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Werror', '-Wno-unused-parameter',
                            '-fsanitize=address,undefined', '-pthread',
                            '-I' + str(ROOT / 'services/waylandbridge'), str(path / 'seat.c'),
                            '-o', str(path / 'seat')], check=True)
            subprocess.run([str(path / 'seat')], check=True)


if __name__ == '__main__':
    unittest.main()
