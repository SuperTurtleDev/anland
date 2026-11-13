package com.anlandnext.awl;

import android.view.KeyEvent;

/** Execute with app_process and the candidate APK on CLASSPATH. */
public final class AndroidKeyCodeProbe {
    private static int count;
    private static void check(int androidCode, int evdevCode) {
        int actual = AndroidKeyCode.toEvdev(androidCode);
        if (actual != evdevCode)
            throw new AssertionError(androidCode + " mapped to " + actual + " expected " + evdevCode);
        count++;
    }
    public static void main(String[] args) {
        int[] letters = {30,48,46,32,18,33,34,35,23,36,37,38,50,49,24,25,16,19,31,20,22,47,17,45,21,44};
        for (int i = 0; i < letters.length; i++) check(KeyEvent.KEYCODE_A + i, letters[i]);
        for (int i = 1; i <= 9; i++) check(KeyEvent.KEYCODE_0 + i, i + 1);
        check(KeyEvent.KEYCODE_0, 11);
        for (int i = 0; i < 10; i++) check(KeyEvent.KEYCODE_F1 + i, 59 + i);
        for (int i = 0; i < 12; i++) check(KeyEvent.KEYCODE_F13 + i, 183 + i);
        check(KeyEvent.KEYCODE_F11, 87); check(KeyEvent.KEYCODE_F12, 88);
        check(KeyEvent.KEYCODE_ENTER, 28); check(KeyEvent.KEYCODE_DEL, 14);
        check(KeyEvent.KEYCODE_FORWARD_DEL, 111); check(KeyEvent.KEYCODE_TAB, 15);
        check(KeyEvent.KEYCODE_CTRL_LEFT, 29); check(KeyEvent.KEYCODE_CTRL_RIGHT, 97);
        check(KeyEvent.KEYCODE_ALT_LEFT, 56); check(KeyEvent.KEYCODE_ALT_RIGHT, 100);
        check(KeyEvent.KEYCODE_META_LEFT, 125); check(KeyEvent.KEYCODE_META_RIGHT, 126);
        check(KeyEvent.KEYCODE_DPAD_UP, 103); check(KeyEvent.KEYCODE_DPAD_DOWN, 108);
        check(KeyEvent.KEYCODE_DPAD_LEFT, 105); check(KeyEvent.KEYCODE_DPAD_RIGHT, 106);
        check(KeyEvent.KEYCODE_UNKNOWN, 0);
        System.out.println("PASS " + count + " Android key mapping checks");
    }
}
