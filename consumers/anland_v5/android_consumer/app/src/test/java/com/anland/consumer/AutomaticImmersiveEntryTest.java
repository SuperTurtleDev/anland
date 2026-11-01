package com.anland.consumer;

import org.junit.Test;

import static org.junit.Assert.*;

public class AutomaticImmersiveEntryTest {
    @Test
    public void onlyGoldDirectHonorsAutoEntry() {
        assertTrue(AutomaticImmersiveEntry.enabledFor(true, true,
                ImmersiveInputSource.EXISTING_UINPUT_BUS));
        assertFalse(AutomaticImmersiveEntry.enabledFor(true, true,
                ImmersiveInputSource.DIRECT_PLUS_GOLD_KEYBOARD));
        assertFalse(AutomaticImmersiveEntry.enabledFor(true, true,
                ImmersiveInputSource.DIRECT_EVENT_NODES));
        for (ImmersiveInputSource source : ImmersiveInputSource.values()) {
            assertFalse(AutomaticImmersiveEntry.enabledFor(false, true, source));
            assertFalse(AutomaticImmersiveEntry.enabledFor(true, false, source));
        }
    }

    @Test
    public void resumeBeforeSurfaceAndFocusDoesNotLoseTheAttempt() {
        AutomaticImmersiveEntry entry = new AutomaticImmersiveEntry();
        entry.arm(100);
        assertFalse(entry.canAttempt(true, false, false));
        assertFalse(entry.canAttempt(true, true, false));
        assertFalse(entry.canAttempt(false, true, true));
        assertTrue(entry.canAttempt(true, true, true));
    }

    @Test
    public void delayedGoldAndPreviousHelperTeardownCanRetryWithinBudget() {
        AutomaticImmersiveEntry entry = new AutomaticImmersiveEntry();
        entry.arm(100);
        assertTrue(entry.retry(false, 600));
        assertTrue(entry.canAttempt(true, true, true));
        assertTrue(entry.retry(false, 10_100));
        assertTrue(entry.retry(false, 30_100));
        assertTrue(entry.canAttempt(true, true, true));
        assertFalse(entry.retry(false, 45_100));
        assertFalse(entry.canAttempt(true, true, true));
    }

    @Test
    public void submittedSessionDoesNotReenterAfterManualExitOrFocusCallbacks() {
        AutomaticImmersiveEntry entry = new AutomaticImmersiveEntry();
        entry.arm(100);
        assertFalse(entry.retry(true, 200));
        assertFalse(entry.canAttempt(true, true, false));
        assertFalse(entry.canAttempt(true, true, true));
        // A fresh resume or lifecycle interruption explicitly re-arms it.
        entry.arm(20_000);
        assertTrue(entry.canAttempt(true, true, true));
        assertTrue(entry.retry(false, 20_500));
    }

    @Test
    public void pauseCancelsPendingEntry() {
        AutomaticImmersiveEntry entry = new AutomaticImmersiveEntry();
        entry.arm(100);
        entry.cancel();
        assertFalse(entry.canAttempt(true, true, true));
        assertFalse(entry.retry(false, 600));
    }
}
