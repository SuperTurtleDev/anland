package com.anland.consumer;

import android.view.Surface;

/**
 * JNI transport surface for the display consumer. All native methods bind by name to
 * {@code Java_com_anland_consumer_Native_*} in {@code jni/native_consumer.c}.
 *
 * The shared library is loaded by {@link MainActivity}'s static initializer (a
 * single {@code .so} backs this class, MainActivity and CameraServices).
 *
 * Instance-based: each consumer window owns its own {@code Native} (a native
 * consumer_state handle behind {@link #handle}), so multiple independent pipelines
 * coexist in one process. Every native method takes the handle as its first
 * argument; the public wrappers below thread it in. Call {@link #destroy()} when the
 * window is torn down.
 */
public final class Native {
    private long handle;

    public Native() {
        handle = nativeCreate();
    }

    /** Release the native instance. Idempotent; the object is unusable afterwards. */
    public void destroy() {
        if (handle != 0) {
            nativeDestroy(handle);
            handle = 0;
        }
    }

    // ---- instance API (delegates to the handle-taking natives) ----

    public void configure(String socketPath, boolean useRoot, String helperPath, String bridgePath) {
        nativeConfigure(handle, socketPath, useRoot, helperPath, bridgePath, false, null, 1, null);
    }

    /** Full configure, including the foreground-scheduling (top-app) helper. */
    public void configure(String socketPath, boolean useRoot, String helperPath, String bridgePath,
                          boolean topAppEnable, String topAppPath, int topAppMode,
                          String topAppStops) {
        nativeConfigure(handle, socketPath, useRoot, helperPath, bridgePath,
                        topAppEnable, topAppPath, topAppMode, topAppStops);
    }
    public void start(Surface surface, Object clipboardTarget, Object activityTarget) { nativeStart(handle, surface, clipboardTarget, activityTarget); }
    public void stop() { nativeStop(handle); }
    /** Mark this instance focused: its camera client receives real frames, others blank. */
    public void setFocused(boolean focused) { nativeSetFocused(handle, focused); }
    public void setCustomResolution(int width, int height) { nativeSetCustomResolution(handle, width, height); }
    public void setScreenSize(int width, int height) { nativeSetScreenSize(handle, width, height); }
    public void sendTouch(int action, float x, float y, int pointerId) { nativeSendTouch(handle, action, x, y, pointerId); }
    public void sendTouchFrame() { nativeSendTouchFrame(handle); }
    public void sendKey(int action, int keycode) { nativeSendKey(handle, action, keycode); }
    public void sendMouseMotion(float x, float y, float dx, float dy) { nativeSendMouseMotion(handle, x, y, dx, dy); }
    public void sendMouseButton(int button, boolean pressed) { nativeSendMouseButton(handle, button, pressed); }
    public void sendMouseScroll(int axis, float value) { nativeSendMouseScroll(handle, axis, value); }
    public void setRefreshRate(float hz) { nativeSetRefreshRate(handle, hz); }
    public void sendClipboard(byte[] data) { nativeSendClipboard(handle, data); }
    public void sendTextInput(byte[] data) { nativeSendTextInput(handle, data); }
    public void setMicEnabled(boolean enabled) { nativeSetMicEnabled(handle, enabled); }
    public void setAudioLatency(int speakerMs, int micMs) { nativeSetAudioLatency(handle, speakerMs, micMs); }
    public void setAudioKeepalive(boolean enabled) { nativeSetAudioKeepalive(handle, enabled); }

    // ---- native handle lifecycle + handle-taking entry points ----

    private static native long nativeCreate();
    private static native void nativeDestroy(long handle);
    private static native void nativeSetFocused(long handle, boolean focused);

    private static native void nativeConfigure(long handle, String socketPath, boolean useRoot,
                                               String helperPath, String bridgePath,
                                               boolean topAppEnable, String topAppPath,
                                               int topAppMode, String topAppStops);

    // With static natives there is no `thiz`, so native is handed the object it
    // calls back into (the Clipboard hosting nativeSetClipboardText /
    // nativeClipListening / nativeClipboardSync). It is stored per-instance as the
    // global ref used by the event thread's clipboard callbacks.
    // activityTarget is the owning MainActivity; native calls its onFallback() when the
    // display lib drops the connection (see on_fallback in native_consumer.c).
    private static native void nativeStart(long handle, Surface surface, Object clipboardTarget,
                                           Object activityTarget);
    private static native void nativeStop(long handle);
    private static native void nativeSetCustomResolution(long handle, int width, int height);
    private static native void nativeSetScreenSize(long handle, int width, int height);
    private static native void nativeSendTouch(long handle, int action, float x, float y, int pointerId);
    private static native void nativeSendTouchFrame(long handle);
    private static native void nativeSendKey(long handle, int action, int keycode);
    private static native void nativeSendMouseMotion(long handle, float x, float y, float dx, float dy);
    private static native void nativeSendMouseButton(long handle, int button, boolean pressed);
    private static native void nativeSendMouseScroll(long handle, int axis, float value);
    private static native void nativeSetRefreshRate(long handle, float hz);
    private static native void nativeSendClipboard(long handle, byte[] data);
    private static native void nativeSendTextInput(long handle, byte[] data);
    private static native void nativeSetMicEnabled(long handle, boolean enabled);
    private static native void nativeSetAudioLatency(long handle, int speakerMs, int micMs);
    private static native void nativeSetAudioKeepalive(long handle, boolean enabled);
}
