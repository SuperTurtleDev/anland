package com.anland.consumer;

import android.content.Context;
import android.view.KeyEvent;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

final class KeyChord {
    static final int MAX_KEYS = 3;
    static final int MAX_SCAN_CODE = 0x2ff; // Linux KEY_MAX

    final String codes;
    private final int[] keys;
    private final Map<Integer, Integer> heldByDevice = new HashMap<>();

    static KeyChord parse(String codes) {
        String[] parts = codes.split(",", -1);
        if (parts.length < 2 || parts.length > MAX_KEYS)
            return new KeyChord("", new int[0]);
        int[] keys = new int[parts.length];
        for (int i = 0; i < parts.length; i++) {
            try {
                keys[i] = Integer.parseInt(parts[i]);
            } catch (NumberFormatException e) {
                return new KeyChord("", new int[0]);
            }
            if (keys[i] <= 0 || keys[i] > MAX_SCAN_CODE || !parts[i].equals(Integer.toString(keys[i])))
                return new KeyChord("", new int[0]);
            for (int j = 0; j < i; j++)
                if (keys[j] == keys[i])
                    return new KeyChord("", new int[0]);
        }
        return new KeyChord(codes, keys);
    }

    private KeyChord(String codes, int[] keys) {
        this.codes = codes;
        this.keys = keys;
    }

    boolean isBound() {
        return keys.length != 0;
    }

    int firstKey() {
        return keys[0];
    }

    int allKeys() {
        return (1 << keys.length) - 1;
    }

    int keyBit(int scanCode) {
        for (int i = 0; i < keys.length; i++)
            if (keys[i] == scanCode)
                return 1 << i;
        return 0;
    }

    void clearHeld() {
        heldByDevice.clear();
    }

    boolean update(KeyEvent event) {
        if (event.getAction() == KeyEvent.ACTION_UP)
            return update(event.getDeviceId(), event.getScanCode(), false);
        if (event.getAction() == KeyEvent.ACTION_DOWN && event.getRepeatCount() == 0)
            return update(event.getDeviceId(), event.getScanCode(), true);
        return false;
    }

    boolean update(int device, int scanCode, boolean down) {
        int bit = keyBit(scanCode);
        if (bit == 0)
            return false;
        int before = heldByDevice.getOrDefault(device, 0);
        if (!down) {
            int after = before & ~bit;
            if (after == 0)
                heldByDevice.remove(device);
            else
                heldByDevice.put(device, after);
            return false;
        }
        int after = before | bit;
        heldByDevice.put(device, after);
        return before != after && after == allKeys();
    }

    static String label(Iterable<KeyEvent> events) {
        List<String> names = new ArrayList<>(MAX_KEYS);
        for (KeyEvent event : events)
            names.add(keyName(event.getKeyCode(), event.getScanCode()));
        return String.join(" + ", names);
    }

    String statusLabel(Context context, String savedLabel) {
        // Previous versions already saved one Android key name per scan code.
        String[] savedNames = savedLabel.split(" \\+ ", -1);
        List<String> codes = new ArrayList<>(keys.length);
        List<String> names = new ArrayList<>(keys.length);
        for (int i = 0; i < keys.length; i++) {
            int code = savedNames.length == keys.length && !savedNames[i].startsWith("#")
                    ? KeyEvent.keyCodeFromString(savedNames[i]) : KeyEvent.KEYCODE_UNKNOWN;
            codes.add(context.getString(R.string.immersive_status_keycode, code));
            names.add(statusKeyName(context, code, keys[i]));
        }
        return String.join("+", codes) + "(" + String.join("+", names) + ")";
    }

    static String statusLabel(Context context, int keyCode, int scanCode) {
        int code = Math.max(KeyEvent.KEYCODE_UNKNOWN, keyCode);
        return context.getString(R.string.immersive_status_keycode, code)
                + "(" + statusKeyName(context, code, scanCode) + ")";
    }

    private static String statusKeyName(Context context, int keyCode, int scanCode) {
        if (keyCode == KeyEvent.KEYCODE_UNKNOWN && scanCode > 0)
            return context.getString(R.string.scancode_unknown, scanCode);
        return keyName(keyCode, scanCode);
    }

    private static String keyName(int keyCode, int scanCode) {
        if (keyCode == KeyEvent.KEYCODE_UNKNOWN && scanCode > 0)
            return "#" + scanCode;
        String name = KeyEvent.keyCodeToString(keyCode);
        return name.startsWith("KEYCODE_") ? name.substring("KEYCODE_".length()) : name;
    }
}
