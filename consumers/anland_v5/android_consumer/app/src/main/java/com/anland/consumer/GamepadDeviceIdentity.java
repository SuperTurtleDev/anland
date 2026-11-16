package com.anland.consumer;

import android.view.InputDevice;

/** Conservative identity match between an evdev gamepad node and Android's device list. */
final class GamepadDeviceIdentity {
    // Android also marks gamepad button events as keyboard-class input.
    private static final int ALLOWED_SOURCES = InputDevice.SOURCE_GAMEPAD
            | InputDevice.SOURCE_JOYSTICK | InputDevice.SOURCE_DPAD
            | InputDevice.SOURCE_KEYBOARD;

    static final class Candidate {
        final int id;
        final String name;
        final int vendor, product, sources, keyboardType;

        Candidate(int id, String name, int vendor, int product, int sources, int keyboardType) {
            this.id = id;
            this.name = name;
            this.vendor = vendor;
            this.product = product;
            this.sources = sources;
            this.keyboardType = keyboardType;
        }
    }

    private GamepadDeviceIdentity() {
    }

    static boolean sameIdentity(String expectedName, int expectedVendor, int expectedProduct,
                                String actualName, int actualVendor, int actualProduct) {
        return expectedName != null && !expectedName.isEmpty()
                && expectedVendor >= 0 && expectedVendor <= 0xffff
                && expectedProduct >= 0 && expectedProduct <= 0xffff
                && expectedName.equals(actualName)
                && expectedVendor == actualVendor && expectedProduct == actualProduct;
    }

    static boolean hasControllerSource(int sources) {
        return (sources & InputDevice.SOURCE_GAMEPAD) == InputDevice.SOURCE_GAMEPAD
                || (sources & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK;
    }

    static boolean matches(String expectedName, int expectedVendor, int expectedProduct,
                           InputDevice device) {
        return device != null && matches(expectedName, expectedVendor, expectedProduct,
                device.getName(), device.getVendorId(), device.getProductId(),
                device.getSources(), device.getKeyboardType());
    }

    static boolean matches(String expectedName, int expectedVendor, int expectedProduct,
                           String actualName, int actualVendor, int actualProduct,
                           int sources, int keyboardType) {
        if (!sameIdentity(expectedName, expectedVendor, expectedProduct,
                actualName, actualVendor, actualProduct))
            return false;
        boolean controller = (sources & InputDevice.SOURCE_GAMEPAD)
                == InputDevice.SOURCE_GAMEPAD
                || (sources & InputDevice.SOURCE_JOYSTICK)
                == InputDevice.SOURCE_JOYSTICK;
        boolean keyboardSource = (sources & InputDevice.SOURCE_KEYBOARD)
                == InputDevice.SOURCE_KEYBOARD;
        boolean safeKeyboardType = keyboardType == InputDevice.KEYBOARD_TYPE_NON_ALPHABETIC;
        // SOURCE_KEYBOARD can describe gamepad buttons; unknown keyboard type is not safe to infer.
        return controller && (!keyboardSource || safeKeyboardType)
                && (sources & ~ALLOWED_SOURCES) == 0;
    }

    static int findUniqueDeviceId(String name, int vendor, int product, Candidate[] devices) {
        if (devices == null)
            return -1;
        int match = -1;
        for (Candidate device : devices) {
            if (device == null || device.id < 0
                    || !sameIdentity(name, vendor, product, device.name,
                            device.vendor, device.product))
                continue;
            // A composite device with this identity must not be mistaken for a
            // different, pure controller having the same name/VID/PID.
            if (match >= 0 || !matches(name, vendor, product, device.name,
                    device.vendor, device.product, device.sources, device.keyboardType))
                return -1;
            match = device.id;
        }
        return match;
    }
}