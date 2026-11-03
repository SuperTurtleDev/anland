package com.anland.consumer;

import android.accessibilityservice.AccessibilityService;
import android.accessibilityservice.AccessibilityServiceInfo;
import android.content.Context;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
import android.view.KeyEvent;
import android.view.accessibility.AccessibilityEvent;

import java.util.LinkedHashSet;

public class KeyInterceptor extends AccessibilityService {

    /**
     * Keys this service consumed, keyed by device as well as key code. A plain
     * key-code set would let a release from one keyboard lift the same key held
     * on another, and would mistake a different device's press for a repeat.
     */
    LinkedHashSet<KeyIdentity> pressedKeys = new LinkedHashSet<>();

    private static final class KeyIdentity {
        final int deviceId, keyCode, scanCode;

        KeyIdentity(int deviceId, int keyCode, int scanCode) {
            this.deviceId = deviceId;
            this.keyCode = keyCode;
            this.scanCode = scanCode;
        }

        @Override
        public boolean equals(Object other) {
            if (!(other instanceof KeyIdentity))
                return false;
            KeyIdentity key = (KeyIdentity) other;
            return deviceId == key.deviceId && keyCode == key.keyCode && scanCode == key.scanCode;
        }

        @Override
        public int hashCode() {
            return (deviceId * 31 + keyCode) * 31 + scanCode;
        }
    }

    private static final Handler handler = new Handler(Looper.getMainLooper());
    private static KeyInterceptor self;
    private static boolean launchedAutomatically = false;
    private boolean enabled = false;

    public KeyInterceptor() {
        self = this;
    }

    public static void launch(Context ctx) {
        try {
            String service = "com.anland.consumer/.KeyInterceptor";
            String enabled = Settings.Secure.getString(ctx.getContentResolver(),
                    Settings.Secure.ENABLED_ACCESSIBILITY_SERVICES);

            if (enabled == null || enabled.isEmpty())
                enabled = service;
            else if (!enabled.contains(service))
                enabled += ":" + service;

            Settings.Secure.putString(ctx.getContentResolver(),
                    Settings.Secure.ENABLED_ACCESSIBILITY_SERVICES, enabled);
            Settings.Secure.putString(ctx.getContentResolver(),
                    Settings.Secure.ACCESSIBILITY_ENABLED, "1");
            launchedAutomatically = true;
        } catch (SecurityException e) {
            android.util.Log.w("KeyInterceptor", "No WRITE_SECURE_SETTINGS permission", e);
            // User must enable via system Settings > Accessibility manually
        }
    }

    public static void shutdown(boolean onlyIfEnabledAutomatically) {
        if (onlyIfEnabledAutomatically && !launchedAutomatically)
            return;

        // Before dropping the record of what was pressed: the keys themselves
        // are held on the desktop, and this is the last chance to lift them.
        releaseHeldKeys();

        if (self != null) {
            self.disableSelf();
            self.pressedKeys.clear();
            self = null;
        }
    }

    /**
     * Asks the window to release whatever it forwarded on this service's behalf.
     *
     * <p>The service tracks which keys it consumed, but the evdev codes it
     * forwarded live with the window, which is the only place that can send the
     * releases back.
     */
    private static void releaseHeldKeys() {
        MainActivity instance = getMainActivity();
        if (instance != null)
            instance.releaseForwardedKeys();
    }

    public static boolean isLaunched() {
        AccessibilityServiceInfo info = self == null ? null : self.getServiceInfo();
        return info != null && info.getId() != null;
    }

    private static final Runnable disableImmediatelyCallback = KeyInterceptor::disableImmediately;
    private static void disableImmediately() {
        if (self == null) return;
        android.util.Log.d("KeyInterceptor", "disabling interception service");
        // Turning the filter flag off stops further events arriving, so any key
        // still held has to be let go now rather than on its release, which will
        // never come through here.
        releaseHeldKeys();
        AccessibilityServiceInfo info = self.getServiceInfo();
        info.flags &= ~AccessibilityServiceInfo.FLAG_REQUEST_FILTER_KEY_EVENTS;
        self.setServiceInfo(info);
        self.enabled = false;
    }

    @Override
    protected void onServiceConnected() {
        super.onServiceConnected();
        self = this;
        recheck();
    }

    public static void recheck() {
        MainActivity a = getMainActivity();
        boolean shouldBeEnabled = (a != null && self != null) && a.isAccessibilityInterceptEnabled();
        if (self != null && shouldBeEnabled != self.enabled) {
            if (shouldBeEnabled) {
                handler.removeCallbacks(disableImmediatelyCallback);
                android.util.Log.d("KeyInterceptor", "enabling interception service");
                AccessibilityServiceInfo info = self.getServiceInfo();
                info.flags |= AccessibilityServiceInfo.FLAG_REQUEST_FILTER_KEY_EVENTS;
                self.setServiceInfo(info);
                self.enabled = true;
            } else {
                handler.postDelayed(disableImmediatelyCallback, 120000);
            }
        }
    }

    @Override
    public boolean onKeyEvent(KeyEvent event) {
        MainActivity instance = getMainActivity();

        if (instance == null)
            return false;

        KeyIdentity identity = identityOf(event);
        boolean releaseTrackedKey = event.getAction() == KeyEvent.ACTION_UP
                && pressedKeys.contains(identity);

        // Keys are only intercepted while the activity is in front and focused.
        // A release for a key already consumed is the exception: the press was
        // forwarded, so dropping the release would leave that key held on the
        // desktop with nothing left to lift it.
        if (!releaseTrackedKey && !instance.hasWindowFocus())
            return false;
        boolean intercept = instance.isAccessibilityInterceptEnabled();

        boolean ret = false;
        // A key whose DOWN was consumed still gets its UP routed, even if
        // interception has been switched off in the meantime: otherwise the
        // release would be lost and the desktop would hold the key down.
        if (intercept || releaseTrackedKey)
            ret = instance.handleAccessibilityKey(event);

        if (intercept && ret && event.getAction() == KeyEvent.ACTION_DOWN)
            pressedKeys.add(identity);
        else if (event.getAction() == KeyEvent.ACTION_UP)
            pressedKeys.remove(identity);

        recheck();

        return ret;
    }

    @Override
    public void onAccessibilityEvent(AccessibilityEvent e) {}

    @Override
    public void onInterrupt() {}

    private static KeyIdentity identityOf(KeyEvent event) {
        return new KeyIdentity(event.getDeviceId(), event.getKeyCode(),
                event.getScanCode());
    }

    private static MainActivity getMainActivity() {
        // MainActivity is the only activity; we can locate it via the global
        // reference set in onCreate. Since we don't have a static getInstance()
        // on MainActivity, we use the singleton from the launcher's assumption.
        return MainActivity.sInstance;
    }
}
