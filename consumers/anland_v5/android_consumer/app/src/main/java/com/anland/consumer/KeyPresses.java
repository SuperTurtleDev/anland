package com.anland.consumer;

import android.view.KeyEvent;

import java.util.HashSet;
import java.util.Set;

/** Releases and repeats belong to the physical downs this handler swallowed. */
final class KeyPresses {
    private final Set<Long> consumed = new HashSet<>();

    static boolean isFreshDown(KeyEvent event) {
        return event.getAction() == KeyEvent.ACTION_DOWN && event.getRepeatCount() == 0;
    }

    static int key(KeyEvent event) {
        return event.getScanCode() > 0 ? event.getScanCode() : -1 - event.getKeyCode();
    }

    static long identity(KeyEvent event) {
        return ((long) event.getDeviceId() << 32) | (key(event) & 0xffffffffL);
    }

    void add(KeyEvent event) {
        add(identity(event));
    }

    void add(long identity) {
        consumed.add(identity);
    }

    boolean consumeTail(KeyEvent event) {
        long key = identity(event);
        if (isFreshDown(event)) {
            consumed.remove(key);
            return false;
        }
        if (event.getAction() == KeyEvent.ACTION_UP)
            return consumed.remove(key);
        return event.getAction() == KeyEvent.ACTION_DOWN && consumed.contains(key);
    }

    void clear() {
        consumed.clear();
    }
}
