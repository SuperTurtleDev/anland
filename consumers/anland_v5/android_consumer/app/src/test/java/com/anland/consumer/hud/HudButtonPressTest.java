package com.anland.consumer.hud;

import org.junit.Test;

import static org.junit.Assert.assertEquals;

public class HudButtonPressTest {
    @Test public void immediateModeSendsOnceWithoutRepeatTicks() {
        HudButtonPress press = new HudButtonPress();
        int[] taps = {0};
        Runnable tap = () -> taps[0]++;
        press.down(false, tap);
        assertEquals(1, taps[0]);
        press.up(tap);
        assertEquals(1, taps[0]);
    }

    @Test public void repeatOnlyRunsWhileImmediatePressIsActive() {
        HudButtonPress press = new HudButtonPress();
        int[] taps = {0};
        Runnable tap = () -> taps[0]++;
        press.down(false, tap);
        press.repeat(tap);
        press.repeat(tap);
        press.up(tap);
        press.repeat(tap);
        assertEquals(3, taps[0]);
    }

    @Test public void releaseModeSuppressesRepeatAndSendsOnRelease() {
        HudButtonPress press = new HudButtonPress();
        int[] taps = {0};
        Runnable tap = () -> taps[0]++;
        press.down(true, tap);
        press.repeat(tap);
        assertEquals(0, taps[0]);
        press.up(tap);
        press.up(tap);
        assertEquals(1, taps[0]);
    }

    @Test public void popupOrCancellationSuppressesDeferredPrimaryAction() {
        HudButtonPress press = new HudButtonPress();
        int[] taps = {0};
        Runnable tap = () -> taps[0]++;
        press.down(true, tap);
        press.cancel();
        press.up(tap);
        assertEquals(0, taps[0]);
    }
}
