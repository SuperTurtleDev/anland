package com.anland.consumer;

import android.app.Activity;
import android.graphics.Color;
import android.view.KeyCharacterMap;
import android.view.KeyEvent;
import android.view.View;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.inputmethod.BaseInputConnection;
import android.view.inputmethod.EditorInfo;
import android.view.inputmethod.InputConnection;
import android.view.inputmethod.InputMethodManager;
import android.widget.EditText;

import java.nio.charset.StandardCharsets;

/**
 * Bridges the Android system soft keyboard to the remote compositor.
 *
 * Owns a 1x1 hidden EditText whose {@link ForwardingInputConnection} forwards
 * committed / composing text and editing keys to the remote in real time, plus
 * the show/hide toggle and IME-visibility query. Surface resizing stays in the
 * host; SystemIME reaches back through {@link Host} only to read the active
 * modifier latch (for modifier combos) and to report visibility changes.
 */
public final class SystemIME {

    // A newly focused view may need a few UI-loop turns before IMM considers it
    // the served editor. Keep the retry bounded so a failed request cannot leave
    // a permanent callback chain behind.
    private static final int SHOW_RETRY_LIMIT = 4;
    private static final long SHOW_RETRY_DELAY_MS = 50L;

    /** Callback surface SystemIME needs from its host (MainActivity). */
    public interface Host {
        /**
         * The modifier latch that soft-keyboard text input should combine with
         * (the HUD overlay), or null when no HUD is available.
         */
        com.anland.consumer.hud.IModifierProvider getModifierProvider();
        /** IME shown/hidden via the toggle; host re-applies the surface layout. */
        void onImeVisibilityChanged(boolean visible);
    }

    // evdev keycodes (linux/input-event-codes.h) for the editing keys a soft
    // keyboard emits as key events rather than text.
    private static final int EVDEV_ESC = 1;
    private static final int EVDEV_BACKSPACE = 14;
    private static final int EVDEV_TAB = 15;
    private static final int EVDEV_ENTER = 28;
    private static final int EVDEV_UP = 103;
    private static final int EVDEV_LEFT = 105;
    private static final int EVDEV_RIGHT = 106;
    private static final int EVDEV_DOWN = 108;
    private static final int EVDEV_DELETE = 111;

    private final Activity activity;
    private final Host host;
    private final Native mNative;
    private InputMethodManager imm;
    private EditText hiddenInput;
    /** Incremented whenever a pending show becomes obsolete (for example, hide). */
    private int showRequestId;

    SystemIME(Activity activity, Host host, Native n) {
        this.activity = activity;
        this.host = host;
        this.mNative = n;
        initHiddenInput();
    }

    /** The 1x1 hidden input the host adds to its content root. */
    View getInputView() {
        return hiddenInput;
    }

    private void initHiddenInput() {
        imm = activity.getSystemService(InputMethodManager.class);

        // Anonymous subclass so we can hand the IME our own InputConnection that
        // forwards text/keys to the remote in real time instead of buffering
        // them in an Editable that only flushes on Enter.
        hiddenInput = new EditText(activity) {
            @Override
            public InputConnection onCreateInputConnection(EditorInfo outAttrs) {
                super.onCreateInputConnection(outAttrs); // fills outAttrs only
                return new ForwardingInputConnection(this);
            }
        };
        hiddenInput.setBackgroundColor(Color.TRANSPARENT);
        hiddenInput.setCursorVisible(false);
        hiddenInput.setAlpha(0f);
        hiddenInput.setEnabled(false);          // 默认不拦截触摸
        hiddenInput.setFocusable(false);
        hiddenInput.setFocusableInTouchMode(false);
        hiddenInput.setClickable(false);
        hiddenInput.setLongClickable(false);
        // NO_ENTER_ACTION: deliver Enter as a key event we can forward, rather
        // than an editor action we'd have to swallow. NO_FULLSCREEN: never show
        // the landscape extract editor, which buffers text instead of sending it
        // live through our InputConnection.
        hiddenInput.setImeOptions(EditorInfo.IME_FLAG_NO_EXTRACT_UI
            | EditorInfo.IME_FLAG_NO_FULLSCREEN
            | EditorInfo.IME_FLAG_NO_ENTER_ACTION);
        hiddenInput.setInputType(android.text.InputType.TYPE_CLASS_TEXT
            | android.text.InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
            | android.text.InputType.TYPE_TEXT_VARIATION_NORMAL);
    }

    // Mirror of the text we have pushed to the remote via the IME path, with the
    // cursor implicitly at its end (we only ever append text or backspace from the
    // tail). Maintained at the sendText/tapKey choke points so it stays accurate no
    // matter which InputConnection method drove the change. Used to re-seed the
    // composing tracker when the IME reclaims already-sent text as a composing
    // region (see ForwardingInputConnection.setComposingRegion).
    private final StringBuilder mMirror = new StringBuilder();
    // Only the trailing text is ever needed (a composing region is at most a word);
    // drop the head past this bound so a long session can't grow the buffer forever.
    private static final int MIRROR_CAP = 4096;

    private void sendText(String text) {
        if (text.isEmpty()) return;
        mMirror.append(text);
        if (mMirror.length() > MIRROR_CAP) {
            mMirror.delete(0, mMirror.length() - MIRROR_CAP);
        }
        mNative.sendTextInput(text.getBytes(StandardCharsets.UTF_8));
    }

    private void tapKey(int evdevCode) {
        mNative.sendKey(0, evdevCode);
        mNative.sendKey(1, evdevCode);
    }

    // Maps soft-keyboard characters to Android key codes so a bar modifier can be
    // combined with them. Shared instance; KeyCharacterMap.getEvents is read-only.
    private final KeyCharacterMap mVirtualKcm =
        KeyCharacterMap.load(KeyCharacterMap.VIRTUAL_KEYBOARD);

    /*
     * If a latched modifier (CTRL/ALT/SHIFT) is active on the HUD overlay, take
     * the first key-mappable character of `s` and send it as a modifier combo
     * (e.g. Ctrl+C) through the overlay, which also clears the unlocked
     * modifiers. Returns true if the input was consumed this way, false to fall
     * back to plain text.
     */
    private boolean maybeSendModifierCombo(String s) {
        com.anland.consumer.hud.IModifierProvider modifiers = host.getModifierProvider();
        if (modifiers == null || !modifiers.hasActiveModifier()
                || s == null || s.isEmpty())
            return false;
        for (int i = 0; i < s.length(); i++) {
            int evdev = charToEvdev(s.charAt(i));
            if (evdev != -1) {
                modifiers.sendKeyComboFromExternal(evdev);
                return true;
            }
        }
        return false;
    }

    // Convert a character to an evdev scancode via the virtual key character map
    // and KeyCodeMapper. Returns -1 if it can't be expressed as a single key.
    private int charToEvdev(char ch) {
        KeyEvent[] events = mVirtualKcm.getEvents(new char[]{ch});
        if (events != null) {
            for (KeyEvent e : events) {
                if (e.getAction() == KeyEvent.ACTION_DOWN) {
                    int evdev = KeyCodeMapper.getScanCode(e.getKeyCode());
                    if (evdev != -1) return evdev;
                }
            }
        }
        return -1;
    }

    // Map the few Android key codes a soft keyboard delivers as key events to the
    // evdev keycodes KWin expects. Returns 0 for keys we don't forward.
    private static int toEvdevKey(int keyCode) {
        switch (keyCode) {
            case KeyEvent.KEYCODE_ENTER:
            case KeyEvent.KEYCODE_NUMPAD_ENTER: return EVDEV_ENTER;
            case KeyEvent.KEYCODE_DEL:          return EVDEV_BACKSPACE;
            case KeyEvent.KEYCODE_FORWARD_DEL:  return EVDEV_DELETE;
            case KeyEvent.KEYCODE_TAB:          return EVDEV_TAB;
            case KeyEvent.KEYCODE_ESCAPE:       return EVDEV_ESC;
            case KeyEvent.KEYCODE_DPAD_LEFT:    return EVDEV_LEFT;
            case KeyEvent.KEYCODE_DPAD_RIGHT:   return EVDEV_RIGHT;
            case KeyEvent.KEYCODE_DPAD_UP:      return EVDEV_UP;
            case KeyEvent.KEYCODE_DPAD_DOWN:    return EVDEV_DOWN;
            default:                            return 0;
        }
    }

    /*
     * Bridges the soft keyboard to the remote compositor in real time. Committed
     * text is forwarded as UTF-8 immediately; composing (preedit) text is
     * forwarded as it changes by diffing against what we already sent, so each
     * keystroke shows up live without waiting for Enter. Editing keys (Enter,
     * Backspace, ...) are forwarded as evdev key taps. We keep no Editable of our
     * own, so nothing accumulates between commits.
     */
    private final class ForwardingInputConnection extends BaseInputConnection {
        // What we have already forwarded for the in-progress composition.
        private final StringBuilder composing = new StringBuilder();

        ForwardingInputConnection(View target) {
            super(target, false);
        }

        @Override
        public boolean commitText(CharSequence text, int newCursorPosition) {
            final String s = text == null ? "" : text.toString();
            // If a bar modifier (CTRL/ALT/...) is held, combine it with the typed
            // character and send as a key combo instead of inserting text.
            if (maybeSendModifierCombo(s)) {
                composing.setLength(0);
                return true;
            }
            // Fast path: the commit just finalizes the current composition
            // unchanged — already forwarded, so only drop the tracker.
            if (composing.length() > 0 && composing.toString().equals(s)) {
                composing.setLength(0);
                return true;
            }
            eraseComposing();
            sendText(s);
            return true;
        }

        @Override
        public boolean setComposingText(CharSequence text, int newCursorPosition) {
            final String s = text == null ? "" : text.toString();
            if (maybeSendModifierCombo(s)) {
                composing.setLength(0);
                return true;
            }
            replaceComposing(s);
            return true;
        }

        @Override
        public boolean finishComposingText() {
            composing.setLength(0); // accepted as-is; keep what we forwarded
            return true;
        }

        @Override
        public boolean deleteSurroundingText(int beforeLength, int afterLength) {
            for (int i = 0; i < beforeLength; i++) {
                if (mMirror.length() > 0) {
                    mMirror.setLength(mMirror.offsetByCodePoints(mMirror.length(), -1));
                }
                tapKey(EVDEV_BACKSPACE);
            }
            for (int i = 0; i < afterLength; i++) {
                tapKey(EVDEV_DELETE);
            }
            return true;
        }

        @Override
        public boolean deleteSurroundingTextInCodePoints(int beforeLength, int afterLength) {
            for (int i = 0; i < beforeLength; i++) {
                if (mMirror.length() > 0) {
                    mMirror.setLength(mMirror.offsetByCodePoints(mMirror.length(), -1));
                }
                tapKey(EVDEV_BACKSPACE);
            }
            for (int i = 0; i < afterLength; i++) {
                tapKey(EVDEV_DELETE);
            }
            return true;
        }

        // The IME reclaims text it previously committed as a fresh composing region
        // (e.g. backspacing into a finished word: it deletes a char, then re-composes
        // the remainder before replacing it). We keep no Editable, so the base class
        // can't honour this — and because our composing tracker is empty at this
        // point, the follow-up setComposingText would diff against "" and *append*
        // the replacement instead of overwriting, turning "shado"+"shad" into
        // "shadoshad". Re-seed the tracker with the region's text so replaceComposing()
        // backspaces the difference. The cursor always sits at the tail of what we've
        // sent, so the region is the last (end - start) chars of the mirror — reading
        // it as a length keeps us correct whether the IME's indices are document- or
        // word-relative.
        @Override
        public boolean setComposingRegion(int start, int end) {
            final int len = end - start;
            if (len >= 0 && len <= mMirror.length()) {
                composing.setLength(0);
                composing.append(mMirror, mMirror.length() - len, mMirror.length());
            }
            return true;
        }

        @Override
        public boolean sendKeyEvent(KeyEvent event) {
            final int evdev = toEvdevKey(event.getKeyCode());
            if (evdev == 0) {
                return super.sendKeyEvent(event);
            }
            if (event.getAction() == KeyEvent.ACTION_DOWN) {
                mNative.sendKey(0, evdev);
            } else if (event.getAction() == KeyEvent.ACTION_UP) {
                mNative.sendKey(1, evdev);
                // Keep the mirror consistent with a raw key edit. A backspace pops the
                // tail; anything else (Enter, Tab, arrows, ...) moves the cursor or
                // inserts content our tail-only model can't track, so drop the mirror
                // and composing tracker rather than risk seeding a bad region later.
                if (evdev == EVDEV_BACKSPACE) {
                    if (mMirror.length() > 0)
                        mMirror.setLength(mMirror.offsetByCodePoints(mMirror.length(), -1));
                } else {
                    mMirror.setLength(0);
                    composing.setLength(0);
                }
            }
            return true;
        }

        // Forward only the delta between the previously-sent composition and the
        // new one: backspace the changed tail, then send the new tail.
        private void replaceComposing(String next) {
            final String prev = composing.toString();
            int prefix = 0;
            final int min = Math.min(prev.length(), next.length());
            while (prefix < min && prev.charAt(prefix) == next.charAt(prefix)) {
                prefix++;
            }
            if (prefix > 0 && Character.isHighSurrogate(prev.charAt(prefix - 1))) {
                prefix--; // never split a surrogate pair
            }
            final int erase = prev.codePointCount(prefix, prev.length());
            for (int i = 0; i < erase; i++) {
                if (mMirror.length() > 0) {
                    mMirror.setLength(mMirror.offsetByCodePoints(mMirror.length(), -1));
                }
                tapKey(EVDEV_BACKSPACE);
            }
            if (prefix < next.length()) {
                sendText(next.substring(prefix));
            }
            composing.setLength(0);
            composing.append(next);
        }

        private void eraseComposing() {
            final int erase = composing.codePointCount(0, composing.length());
            for (int i = 0; i < erase; i++) {
                if (mMirror.length() > 0) {
                    mMirror.setLength(mMirror.offsetByCodePoints(mMirror.length(), -1));
                }
                tapKey(EVDEV_BACKSPACE);
            }
            composing.setLength(0);
        }
    }

    boolean isImeVisible() {
        WindowInsets insets = activity.getWindow().getDecorView().getRootWindowInsets();
        return insets != null && insets.isVisible(WindowInsets.Type.ime());
    }

    // True while the soft keyboard is wanted: the hidden editor stays enabled
    // from the show request until releaseHiddenInput() runs (toggle-off, or the
    // host's insets listener when the IME hides by any means). Insets lag or
    // never fire in freeform mode, so the editor's enabled state is the closer
    // "IME wanted" latch — it must not go stale-open in the other direction,
    // and releaseHiddenInput keeps it shut on every hide path that reports.
    boolean isImeWanted() {
        return hiddenInput.isEnabled();
    }

    void releaseHiddenInput() {
        if (!hiddenInput.isEnabled()) return;  // already released
        hiddenInput.clearFocus();
        hiddenInput.setFocusable(false);
        hiddenInput.setEnabled(false);
    }

    private void retryShow(int requestId, int attempt) {
        if (attempt >= SHOW_RETRY_LIMIT || requestId != showRequestId
                || !hiddenInput.isEnabled())
            return;
        hiddenInput.postDelayed(() -> requestShow(requestId, attempt + 1),
                SHOW_RETRY_DELAY_MS);
    }

    private void requestShow(int requestId, int attempt) {
        if (requestId != showRequestId || !hiddenInput.isEnabled()) return;
        if (!hiddenInput.hasFocus()) hiddenInput.requestFocus();

        android.os.ResultReceiver receiver = new android.os.ResultReceiver(
                hiddenInput.getHandler()) {
            @Override
            protected void onReceiveResult(int resultCode, android.os.Bundle data) {
                if (resultCode == InputMethodManager.RESULT_UNCHANGED_HIDDEN
                        || resultCode == InputMethodManager.RESULT_HIDDEN) {
                    retryShow(requestId, attempt);
                }
            }
        };

        // The insets API is the explicit user-driven show path on Android 11+
        // and later. Keep IMM below as a compatibility/focus-registration
        // fallback for devices that do not hand the view a controller yet.
        WindowInsetsController controller = hiddenInput.getWindowInsetsController();
        if (controller == null) controller = activity.getWindow().getInsetsController();
        if (controller != null) controller.show(WindowInsets.Type.ime());

        // showSoftInput() returns false when the view has not reached IMM's
        // served-view state. In that case ResultReceiver is not guaranteed to
        // run, so retry from the return value as well as from a hidden result.
        if (!imm.showSoftInput(hiddenInput, InputMethodManager.SHOW_IMPLICIT, receiver))
            retryShow(requestId, attempt);
    }

    // Toggle the system IME (soft keyboard). Driven by the ⌨ bar key tap and the
    // user-bound hardware keycode.
    void toggleSystemKeyboard() {
        if (imm == null) imm = activity.getSystemService(InputMethodManager.class);
        if (imm == null) return;
        if (isImeVisible()) {
            showRequestId++;
            imm.hideSoftInputFromWindow(hiddenInput.getWindowToken(), 0);
            releaseHiddenInput();
            // In freeform mode the inset callback may not fire; hide the bar
            // explicitly so it tracks the IME state in all modes.
            host.onImeVisibilityChanged(false);
        } else {
            hiddenInput.setEnabled(true);
            hiddenInput.setFocusable(true);
            hiddenInput.setFocusableInTouchMode(true);
            hiddenInput.requestFocus();
            // A secondary window is a fresh freeform/multi-window task: the IMM
            // registers hiddenInput as its served view asynchronously on the focus
            // change, so an immediate showSoftInput() here races that and no-ops.
            // Post the request so it runs after focus settles, and retry briefly if
            // the IME wasn't actually shown — the system also drops some
            // SHOW_IMPLICIT requests for windows that aren't the current full input
            // target.
            final int requestId = ++showRequestId;
            hiddenInput.post(() -> {
                // Window focus only says that the Activity is foreground; it does
                // not mean this 1x1 editor owns input focus. Returning from the
                // Settings Activity commonly leaves the surface focused, so the
                // old check could skip the request and make the first bound-key
                // press a no-op.
                requestShow(requestId, 0);
            });
            // In freeform / small-window mode the IME appears as a floating
            // window that does NOT trigger window insets, so applyImeInset()
            // is never called and the extra-keys bar stays hidden.  Show it
            // explicitly here so the bar appears alongside the IME in all modes.
            host.onImeVisibilityChanged(true);
        }
    }
}
