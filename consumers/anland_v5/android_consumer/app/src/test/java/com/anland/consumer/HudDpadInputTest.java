package com.anland.consumer;

import com.anland.consumer.hud.HudAction;
import com.anland.consumer.hud.HudButton;
import com.anland.consumer.hud.HudDpadInput;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import org.junit.Test;
import static org.junit.Assert.*;

public class HudDpadInputTest {
    private final List<String> events = new ArrayList<>();
    private final HudDpadInput input = new HudDpadInput((action, code) -> events.add(action + ":" + code));

    @Test public void defaultCrossUsesIndependentKeyboardArrows() {
        HudButton b = new HudButton();
        b.widgetType = HudButton.WIDGET_DPAD;
        assertEquals(103, b.directionalAction(4).code);
        assertEquals(108, b.directionalAction(5).code);
        assertEquals(105, b.directionalAction(2).code);
        assertEquals(106, b.directionalAction(3).code);
        b.setDirectionalAction(4, HudAction.key(17)); // W
        assertEquals(17, b.duplicate().directionalAction(4).code);
        assertEquals(108, b.directionalAction(5).code);
    }

    @Test public void diagonalAndOppositeSlideReleaseThePreviousKeys() {
        input.update(1, HudAction.key(103), true);
        input.update(4, HudAction.key(106), true);
        input.update(1, null, false);
        input.update(2, HudAction.key(108), true);
        input.update(2, null, false);
        input.update(4, null, false);
        assertEquals(Arrays.asList("0:103", "0:106", "1:103", "0:108", "1:108", "1:106"), events);
    }

    @Test public void sharedComboModifierStaysHeldUntilBothDirectionsRelease() {
        input.update(1, HudAction.combo(29, 17), true);
        input.update(4, HudAction.combo(29, 32), true);
        input.update(1, null, false);
        assertEquals(Arrays.asList("0:29", "0:17", "0:32", "1:17"), events);
        input.update(4, null, false);
        assertEquals(Arrays.asList("0:29", "0:17", "0:32", "1:17", "1:32", "1:29"), events);
    }

    @Test public void duplicateDirectionsAndBindingChangesDoNotLeaveKeysHeld() {
        HudAction action = HudAction.key(17);
        input.update(1, action, true);
        input.update(1, action, true);
        action.code = 30;
        input.update(1, action, false);
        input.update(1, action, false);
        assertEquals(Arrays.asList("0:17", "1:17"), events);
    }

    @Test public void clearedAndNonKeyboardActionsNeverGenerateKeyZero() {
        input.update(1, null, true);
        input.update(1, null, false);
        input.update(2, HudAction.key(0), true);
        input.update(2, null, false);
        input.update(3, HudAction.system("open_settings"), true);
        input.update(3, null, false);
        assertTrue(events.isEmpty());
    }

    @Test public void clearingDirectionSurvivesDuplication() {
        HudButton b = new HudButton();
        b.widgetType = HudButton.WIDGET_DPAD;
        b.setDirectionalAction(4, null);
        assertNull(b.duplicate().directionalAction(4));
    }
}
