package com.anland.consumer;

import android.content.Context;
import android.util.SparseIntArray;
import android.view.KeyEvent;

public class KeyCodeMapper {
    private static final SparseIntArray MAP = new SparseIntArray();
    // Added in API 36; keep the stable Android keycode values so compileSdk 35
    // builds can still forward extended function keys received at runtime.
    private static final int KEYCODE_F13 = 326;
    private static final int KEYCODE_F24 = 337;


    /** Android keycode → localized name, for the keys worth binding to. */
    private static final SparseIntArray NAME_RES = new SparseIntArray();
    static {
        NAME_RES.put(KeyEvent.KEYCODE_VOLUME_UP, R.string.key_volume_up);
        NAME_RES.put(KeyEvent.KEYCODE_VOLUME_DOWN, R.string.key_volume_down);
        NAME_RES.put(KeyEvent.KEYCODE_VOLUME_MUTE, R.string.key_volume_mute);
        NAME_RES.put(KeyEvent.KEYCODE_POWER, R.string.key_power);
        NAME_RES.put(KeyEvent.KEYCODE_CAMERA, R.string.key_camera);
        NAME_RES.put(KeyEvent.KEYCODE_HEADSETHOOK, R.string.key_headset_hook);
        NAME_RES.put(KeyEvent.KEYCODE_MEDIA_PLAY_PAUSE, R.string.key_media_play_pause);
        NAME_RES.put(KeyEvent.KEYCODE_MEDIA_NEXT, R.string.key_media_next);
        NAME_RES.put(KeyEvent.KEYCODE_MEDIA_PREVIOUS, R.string.key_media_previous);
        NAME_RES.put(KeyEvent.KEYCODE_BRIGHTNESS_UP, R.string.key_brightness_up);
        NAME_RES.put(KeyEvent.KEYCODE_BRIGHTNESS_DOWN, R.string.key_brightness_down);
        NAME_RES.put(KeyEvent.KEYCODE_HOME, R.string.key_home);
        NAME_RES.put(KeyEvent.KEYCODE_BACK, R.string.key_back);
    }

    /**
     * A name for a bound key, for status lines and toasts. Falls back to the raw
     * key code, or to the evdev scan code when Android had no key code for it at
     * all (some vendor keys only report a scan code).
     */
    public static String keyName(Context ctx, int keyCode, int scanCode) {
        int nameRes = NAME_RES.get(keyCode);
        if (nameRes != 0)
            return ctx.getString(nameRes);
        if (keyCode != -1 && keyCode != KeyEvent.KEYCODE_UNKNOWN)
            return ctx.getString(R.string.keycode_unknown, keyCode);
        if (scanCode > 0)
            return ctx.getString(R.string.scancode_unknown, scanCode);
        return ctx.getString(R.string.status_current_none);
    }

    static {
        // 字母 A-Z
        MAP.put(KeyEvent.KEYCODE_A, 30);
        MAP.put(KeyEvent.KEYCODE_B, 48);
        MAP.put(KeyEvent.KEYCODE_C, 46);
        MAP.put(KeyEvent.KEYCODE_D, 32);
        MAP.put(KeyEvent.KEYCODE_E, 18);
        MAP.put(KeyEvent.KEYCODE_F, 33);
        MAP.put(KeyEvent.KEYCODE_G, 34);
        MAP.put(KeyEvent.KEYCODE_H, 35);
        MAP.put(KeyEvent.KEYCODE_I, 23);
        MAP.put(KeyEvent.KEYCODE_J, 36);
        MAP.put(KeyEvent.KEYCODE_K, 37);
        MAP.put(KeyEvent.KEYCODE_L, 38);
        MAP.put(KeyEvent.KEYCODE_M, 50);
        MAP.put(KeyEvent.KEYCODE_N, 49);
        MAP.put(KeyEvent.KEYCODE_O, 24);
        MAP.put(KeyEvent.KEYCODE_P, 25);
        MAP.put(KeyEvent.KEYCODE_Q, 16);
        MAP.put(KeyEvent.KEYCODE_R, 19);
        MAP.put(KeyEvent.KEYCODE_S, 31);
        MAP.put(KeyEvent.KEYCODE_T, 20);
        MAP.put(KeyEvent.KEYCODE_U, 22);
        MAP.put(KeyEvent.KEYCODE_V, 47);
        MAP.put(KeyEvent.KEYCODE_W, 17);
        MAP.put(KeyEvent.KEYCODE_X, 45);
        MAP.put(KeyEvent.KEYCODE_Y, 21);
        MAP.put(KeyEvent.KEYCODE_Z, 44);

        // 数字 0-9
        MAP.put(KeyEvent.KEYCODE_0, 11);
        MAP.put(KeyEvent.KEYCODE_1, 2);
        MAP.put(KeyEvent.KEYCODE_2, 3);
        MAP.put(KeyEvent.KEYCODE_3, 4);
        MAP.put(KeyEvent.KEYCODE_4, 5);
        MAP.put(KeyEvent.KEYCODE_5, 6);
        MAP.put(KeyEvent.KEYCODE_6, 7);
        MAP.put(KeyEvent.KEYCODE_7, 8);
        MAP.put(KeyEvent.KEYCODE_8, 9);
        MAP.put(KeyEvent.KEYCODE_9, 10);

        // 符号
        MAP.put(KeyEvent.KEYCODE_MINUS, 12);
        MAP.put(KeyEvent.KEYCODE_EQUALS, 13);
        MAP.put(KeyEvent.KEYCODE_LEFT_BRACKET, 26);
        MAP.put(KeyEvent.KEYCODE_RIGHT_BRACKET, 27);
        MAP.put(KeyEvent.KEYCODE_BACKSLASH, 43);
        MAP.put(KeyEvent.KEYCODE_SEMICOLON, 39);
        MAP.put(KeyEvent.KEYCODE_APOSTROPHE, 40);
        MAP.put(KeyEvent.KEYCODE_COMMA, 51);
        MAP.put(KeyEvent.KEYCODE_PERIOD, 52);
        MAP.put(KeyEvent.KEYCODE_SLASH, 53);
        MAP.put(KeyEvent.KEYCODE_GRAVE, 41);

        // 功能键
        MAP.put(KeyEvent.KEYCODE_SPACE, 57);
        MAP.put(KeyEvent.KEYCODE_ENTER, 28);
        MAP.put(KeyEvent.KEYCODE_DEL, 14);          // Backspace
        MAP.put(KeyEvent.KEYCODE_FORWARD_DEL, 111); // Delete
        MAP.put(KeyEvent.KEYCODE_TAB, 15);
        MAP.put(KeyEvent.KEYCODE_ESCAPE, 1);
        MAP.put(KeyEvent.KEYCODE_SHIFT_LEFT, 42);
        MAP.put(KeyEvent.KEYCODE_SHIFT_RIGHT, 54);
        MAP.put(KeyEvent.KEYCODE_CTRL_LEFT, 29);
        MAP.put(KeyEvent.KEYCODE_CTRL_RIGHT, 97);
        MAP.put(KeyEvent.KEYCODE_ALT_LEFT, 56);
        MAP.put(KeyEvent.KEYCODE_ALT_RIGHT, 100);
        MAP.put(KeyEvent.KEYCODE_META_LEFT, 125);
        MAP.put(KeyEvent.KEYCODE_META_RIGHT, 126);
        MAP.put(KeyEvent.KEYCODE_SEARCH, 125);
        MAP.put(KeyEvent.KEYCODE_ASSIST, 125);
        MAP.put(KeyEvent.KEYCODE_CAPS_LOCK, 58);

        // 方向键
        MAP.put(KeyEvent.KEYCODE_DPAD_UP, 103);
        MAP.put(KeyEvent.KEYCODE_DPAD_DOWN, 108);
        MAP.put(KeyEvent.KEYCODE_DPAD_LEFT, 105);
        MAP.put(KeyEvent.KEYCODE_DPAD_RIGHT, 106);

        // F1-F12（修正 F11/F12）
        MAP.put(KeyEvent.KEYCODE_F1, 59);
        MAP.put(KeyEvent.KEYCODE_F2, 60);
        MAP.put(KeyEvent.KEYCODE_F3, 61);
        MAP.put(KeyEvent.KEYCODE_F4, 62);
        MAP.put(KeyEvent.KEYCODE_F5, 63);
        MAP.put(KeyEvent.KEYCODE_F6, 64);
        MAP.put(KeyEvent.KEYCODE_F7, 65);
        MAP.put(KeyEvent.KEYCODE_F8, 66);
        MAP.put(KeyEvent.KEYCODE_F9, 67);
        MAP.put(KeyEvent.KEYCODE_F10, 68);
        MAP.put(KeyEvent.KEYCODE_F11, 87);  // 修正
        MAP.put(KeyEvent.KEYCODE_F12, 88);  // 修正
        for (int keyCode = KEYCODE_F13; keyCode <= KEYCODE_F24; keyCode++)
            MAP.put(keyCode, 183 + keyCode - KEYCODE_F13);

        // Home / End
        MAP.put(KeyEvent.KEYCODE_MOVE_HOME, 102);
        MAP.put(KeyEvent.KEYCODE_MOVE_END, 107);
    }

    static boolean isExtendedFunctionKey(int keyCode) {
        return keyCode >= KEYCODE_F13 && keyCode <= KEYCODE_F24;
    }

    public static int getScanCode(int keyCode) {
        return MAP.get(keyCode, -1);
    }
}
