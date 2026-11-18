package com.anland.consumer;

import org.junit.Test;

import static org.junit.Assert.assertFalse;
import static org.junit.Assert.assertTrue;

public class KeyChordTest {
    @Test
    public void rejectsMalformedOrOversizedBindings() {
        for (String codes : new String[]{"30", "30,30", "30,0", "30,768", "30,48,46,32",
                "30,", "30,+48", "30,048"}) {
            assertFalse(codes, KeyChord.parse(codes).isBound());
        }
    }

    @Test
    public void requiresSimultaneousKeysFromOneDeviceInEitherOrder() {
        KeyChord chord = KeyChord.parse("30,48");
        assertFalse(chord.update(1, 48, true));
        assertFalse(chord.update(2, 30, true));
        assertTrue(chord.update(1, 30, true));
        assertFalse(chord.update(1, 30, true));
        assertFalse(chord.update(1, 48, false));
        assertTrue(chord.update(1, 48, true));
    }

    @Test
    public void clearingStatePreventsStaleKeysAcrossSessions() {
        KeyChord chord = KeyChord.parse("30,48,46");
        assertFalse(chord.update(1, 46, true));
        assertFalse(chord.update(1, 48, true));
        chord.clearHeld();
        assertFalse(chord.update(1, 30, true));
        assertFalse(chord.update(1, 46, true));
        assertTrue(chord.update(1, 48, true));
    }
}
