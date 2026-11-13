package com.anlandnext.awl;

import android.view.KeyEvent;

/** Linux evdev fallback for Android key events which do not carry a scan code. */
final class AndroidKeyCode {
    private AndroidKeyCode() {}

    static int toEvdev(int keyCode) {
        /* Android's digit keycodes are 0, 1..9; evdev's are 1..9, 0. */
        if (keyCode >= KeyEvent.KEYCODE_1 && keyCode <= KeyEvent.KEYCODE_9)
            return 2 + keyCode - KeyEvent.KEYCODE_1;
        if (keyCode == KeyEvent.KEYCODE_0) return 11;

        /* F1..F10 and F13..F24 are contiguous in both APIs. */
        if (keyCode >= KeyEvent.KEYCODE_F1 && keyCode <= KeyEvent.KEYCODE_F10)
            return 59 + keyCode - KeyEvent.KEYCODE_F1;
        if (keyCode >= KeyEvent.KEYCODE_F13 && keyCode <= KeyEvent.KEYCODE_F24)
            return 183 + keyCode - KeyEvent.KEYCODE_F13;

        switch (keyCode) {
        /* Alphabetic keys. Android is alphabetical; evdev follows QWERTY. */
        case KeyEvent.KEYCODE_A: return 30;
        case KeyEvent.KEYCODE_B: return 48;
        case KeyEvent.KEYCODE_C: return 46;
        case KeyEvent.KEYCODE_D: return 32;
        case KeyEvent.KEYCODE_E: return 18;
        case KeyEvent.KEYCODE_F: return 33;
        case KeyEvent.KEYCODE_G: return 34;
        case KeyEvent.KEYCODE_H: return 35;
        case KeyEvent.KEYCODE_I: return 23;
        case KeyEvent.KEYCODE_J: return 36;
        case KeyEvent.KEYCODE_K: return 37;
        case KeyEvent.KEYCODE_L: return 38;
        case KeyEvent.KEYCODE_M: return 50;
        case KeyEvent.KEYCODE_N: return 49;
        case KeyEvent.KEYCODE_O: return 24;
        case KeyEvent.KEYCODE_P: return 25;
        case KeyEvent.KEYCODE_Q: return 16;
        case KeyEvent.KEYCODE_R: return 19;
        case KeyEvent.KEYCODE_S: return 31;
        case KeyEvent.KEYCODE_T: return 20;
        case KeyEvent.KEYCODE_U: return 22;
        case KeyEvent.KEYCODE_V: return 47;
        case KeyEvent.KEYCODE_W: return 17;
        case KeyEvent.KEYCODE_X: return 45;
        case KeyEvent.KEYCODE_Y: return 21;
        case KeyEvent.KEYCODE_Z: return 44;

        /* Punctuation and the main typing block. */
        case KeyEvent.KEYCODE_MINUS:         return 12;
        case KeyEvent.KEYCODE_EQUALS:        return 13;
        case KeyEvent.KEYCODE_DEL:           return 14;  /* Android DEL = Backspace */
        case KeyEvent.KEYCODE_TAB:           return 15;
        case KeyEvent.KEYCODE_LEFT_BRACKET:  return 26;
        case KeyEvent.KEYCODE_RIGHT_BRACKET: return 27;
        case KeyEvent.KEYCODE_ENTER:         return 28;
        case KeyEvent.KEYCODE_SEMICOLON:     return 39;
        case KeyEvent.KEYCODE_APOSTROPHE:    return 40;
        case KeyEvent.KEYCODE_GRAVE:         return 41;
        case KeyEvent.KEYCODE_BACKSLASH:     return 43;
        case KeyEvent.KEYCODE_COMMA:         return 51;
        case KeyEvent.KEYCODE_PERIOD:        return 52;
        case KeyEvent.KEYCODE_SLASH:         return 53;
        case KeyEvent.KEYCODE_SPACE:         return 57;

        /* Modifiers and locks. Modifier masks still come from metaState. */
        case KeyEvent.KEYCODE_CTRL_LEFT:   return 29;
        case KeyEvent.KEYCODE_SHIFT_LEFT:  return 42;
        case KeyEvent.KEYCODE_SHIFT_RIGHT: return 54;
        case KeyEvent.KEYCODE_ALT_LEFT:    return 56;
        case KeyEvent.KEYCODE_CAPS_LOCK:   return 58;
        case KeyEvent.KEYCODE_NUM_LOCK:    return 69;
        case KeyEvent.KEYCODE_SCROLL_LOCK: return 70;
        case KeyEvent.KEYCODE_CTRL_RIGHT:  return 97;
        case KeyEvent.KEYCODE_ALT_RIGHT:   return 100;
        case KeyEvent.KEYCODE_META_LEFT:   return 125;
        case KeyEvent.KEYCODE_META_RIGHT:  return 126;

        /* Navigation/editing. DPAD_CENTER is keyboard Enter in this path. */
        case KeyEvent.KEYCODE_DPAD_CENTER: return 28;
        case KeyEvent.KEYCODE_ESCAPE:      return 1;
        case KeyEvent.KEYCODE_MOVE_HOME:   return 102;
        case KeyEvent.KEYCODE_DPAD_UP:     return 103;
        case KeyEvent.KEYCODE_PAGE_UP:     return 104;
        case KeyEvent.KEYCODE_DPAD_LEFT:   return 105;
        case KeyEvent.KEYCODE_DPAD_RIGHT:  return 106;
        case KeyEvent.KEYCODE_MOVE_END:    return 107;
        case KeyEvent.KEYCODE_DPAD_DOWN:   return 108;
        case KeyEvent.KEYCODE_PAGE_DOWN:   return 109;
        case KeyEvent.KEYCODE_INSERT:      return 110;
        case KeyEvent.KEYCODE_FORWARD_DEL: return 111;
        case KeyEvent.KEYCODE_SYSRQ:       return 99;
        case KeyEvent.KEYCODE_BREAK:       return 119;
        case KeyEvent.KEYCODE_F11:         return 87;
        case KeyEvent.KEYCODE_F12:         return 88;

        /* Numeric keypad. */
        case KeyEvent.KEYCODE_NUMPAD_7:           return 71;
        case KeyEvent.KEYCODE_NUMPAD_8:           return 72;
        case KeyEvent.KEYCODE_NUMPAD_9:           return 73;
        case KeyEvent.KEYCODE_NUMPAD_SUBTRACT:    return 74;
        case KeyEvent.KEYCODE_NUMPAD_4:           return 75;
        case KeyEvent.KEYCODE_NUMPAD_5:           return 76;
        case KeyEvent.KEYCODE_NUMPAD_6:           return 77;
        case KeyEvent.KEYCODE_NUMPAD_ADD:         return 78;
        case KeyEvent.KEYCODE_NUMPAD_1:           return 79;
        case KeyEvent.KEYCODE_NUMPAD_2:           return 80;
        case KeyEvent.KEYCODE_NUMPAD_3:           return 81;
        case KeyEvent.KEYCODE_NUMPAD_0:           return 82;
        case KeyEvent.KEYCODE_NUMPAD_DOT:         return 83;
        case KeyEvent.KEYCODE_NUMPAD_COMMA:       return 121;
        case KeyEvent.KEYCODE_NUMPAD_ENTER:       return 96;
        case KeyEvent.KEYCODE_NUMPAD_DIVIDE:      return 98;
        case KeyEvent.KEYCODE_NUMPAD_MULTIPLY:    return 55;
        case KeyEvent.KEYCODE_NUMPAD_EQUALS:      return 117;
        case KeyEvent.KEYCODE_NUMPAD_LEFT_PAREN:  return 179;
        case KeyEvent.KEYCODE_NUMPAD_RIGHT_PAREN: return 180;

        /* Desktop/application and media keys useful through scrcpy. */
        case KeyEvent.KEYCODE_MENU:               return 139;
        case KeyEvent.KEYCODE_SEARCH:             return 136;
        case KeyEvent.KEYCODE_HELP:               return 138;
        case KeyEvent.KEYCODE_CALCULATOR:         return 140;
        case KeyEvent.KEYCODE_SLEEP:              return 142;
        case KeyEvent.KEYCODE_WAKEUP:             return 143;
        case KeyEvent.KEYCODE_FORWARD:            return 159;
        case KeyEvent.KEYCODE_REFRESH:            return 173;
        case KeyEvent.KEYCODE_COPY:               return 133;
        case KeyEvent.KEYCODE_PASTE:              return 135;
        case KeyEvent.KEYCODE_CUT:                return 137;
        case KeyEvent.KEYCODE_MEDIA_STOP:         return 128;
        case KeyEvent.KEYCODE_MEDIA_NEXT:         return 163;
        case KeyEvent.KEYCODE_MEDIA_PLAY_PAUSE:   return 164;
        case KeyEvent.KEYCODE_MEDIA_PREVIOUS:     return 165;
        case KeyEvent.KEYCODE_MEDIA_REWIND:       return 168;
        case KeyEvent.KEYCODE_MEDIA_FAST_FORWARD: return 208;
        default:                                  return 0;
        }
    }
}
