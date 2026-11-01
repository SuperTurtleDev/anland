package com.anland.consumer;

import java.util.ArrayList;
import java.util.List;
import org.junit.Test;
import static org.junit.Assert.*;

public class TabletInputTest {
    private final List<TabletInput.Frame> frames = new ArrayList<>();
    private final TabletInput input = new TabletInput(frames::add);

    private TabletInput.Sample sample(int device, int pointer, int type,
                                      boolean proximity, boolean tipDown,
                                      int buttons, long time) {
        return new TabletInput.Sample(device, pointer, type,
                TabletInput.CAP_PRESSURE | TabletInput.CAP_TILT | TabletInput.CAP_DISTANCE,
                100, 200, 0.6f, 12, -8, 0.25f, proximity, tipDown,
                tipDown ? TabletInput.TIP_STATE_DOWN : TabletInput.TIP_STATE_UP,
                false, buttons, time);
    }

    @Test public void hoverContactButtonAndExitKeepOneToolIdAndCommitStateLast() {
        input.update(sample(4, 2, TabletInput.TOOL_PEN, true, false,
                TabletInput.BUTTON_PRIMARY, 1000));
        input.update(sample(4, 2, TabletInput.TOOL_PEN, true, true,
                TabletInput.BUTTON_PRIMARY, 1016));
        input.update(sample(4, 2, TabletInput.TOOL_PEN, false, false, 0, 1032));

        assertEquals(3, frames.size());
        assertEquals(frames.get(0).toolId, frames.get(1).toolId);
        assertEquals(frames.get(1).toolId, frames.get(2).toolId);
        assertEquals(TabletInput.FLAG_PROXIMITY, frames.get(0).flags);
        assertEquals(TabletInput.FLAG_PROXIMITY | TabletInput.FLAG_TIP_DOWN,
                frames.get(1).flags);
        assertEquals(0, frames.get(2).flags);
        assertEquals(0, frames.get(2).buttons);
        assertEquals(1032, frames.get(2).timeMsec);
        assertFalse(input.hasActiveTools());
    }

    @Test public void devicesAndPointersRemainIndependentDuringMixedInput() {
        input.update(sample(3, 0, TabletInput.TOOL_PEN, true, true, 0, 10));
        input.update(sample(3, 1, TabletInput.TOOL_ERASER, true, false, 0, 11));
        input.update(sample(8, 0, TabletInput.TOOL_PEN, true, false, 0, 12));

        assertEquals(3, frames.size());
        assertNotEquals(frames.get(0).toolId, frames.get(1).toolId);
        assertNotEquals(frames.get(0).toolId, frames.get(2).toolId);
        assertEquals(TabletInput.TOOL_ERASER, frames.get(1).toolType);
        input.cancelDevice(3, 13);
        assertEquals(5, frames.size());
        assertEquals(TabletInput.FLAG_CANCELED, frames.get(3).flags);
        assertEquals(TabletInput.FLAG_CANCELED, frames.get(4).flags);
        assertEquals(0, frames.get(3).buttons);
        assertTrue(input.hasActiveTools());
    }

    @Test public void cancelAllReleasesEveryToolAndClampsInvalidAxes() {
        input.update(new TabletInput.Sample(1, 0, TabletInput.TOOL_PEN,
                TabletInput.CAP_PRESSURE, Float.NaN, 200, 1.5f, 120, -120, -1,
                true, true, false, TabletInput.BUTTON_SECONDARY, 20));
        input.update(sample(2, 0, TabletInput.TOOL_ERASER, true, false, 0, 21));
        assertEquals(0f, frames.get(0).x, 0f);
        assertEquals(1f, frames.get(0).pressure, 0f);
        assertEquals(90f, frames.get(0).tiltX, 0f);
        assertEquals(-90f, frames.get(0).tiltY, 0f);
        assertEquals(0f, frames.get(0).distance, 0f);

        input.cancelAll(22);
        assertEquals(4, frames.size());
        assertEquals(TabletInput.FLAG_CANCELED, frames.get(2).flags);
        assertEquals(TabletInput.FLAG_CANCELED, frames.get(3).flags);
        assertFalse(input.hasActiveTools());
    }

    @Test public void repeatedProximityCyclesReuseTheSameBackendToolId() {
        for (int cycle = 0; cycle < 24; cycle++) {
            long time = cycle * 2L;
            input.update(sample(5, 7, TabletInput.TOOL_PEN, true, false, 0, time));
            input.update(sample(5, 7, TabletInput.TOOL_PEN, false, false, 0, time + 1));
        }

        assertEquals(48, frames.size());
        int firstId = frames.get(0).toolId;
        assertTrue(firstId > 0 && firstId <= 16);
        for (TabletInput.Frame frame : frames)
            assertEquals(firstId, frame.toolId);
        assertFalse(input.hasActiveTools());
    }

    @Test public void explicitTipContactSurvivesZeroPressureMoveSamples() {
        TabletInput.Sample down = new TabletInput.Sample(2, 4, TabletInput.TOOL_PEN,
                TabletInput.CAP_PRESSURE, 10, 20, 0f, 0, 0, 0,
                true, false, TabletInput.TIP_STATE_DOWN, false, 0, 100);
        TabletInput.Sample move = new TabletInput.Sample(2, 4, TabletInput.TOOL_PEN,
                TabletInput.CAP_PRESSURE, 11, 21, 0f, 0, 0, 0,
                true, false, TabletInput.TIP_STATE_UNKNOWN, false, 0, 116);
        input.update(down);
        input.update(move);

        assertEquals(TabletInput.FLAG_PROXIMITY | TabletInput.FLAG_TIP_DOWN,
                frames.get(0).flags);
        assertEquals(TabletInput.FLAG_PROXIMITY | TabletInput.FLAG_TIP_DOWN,
                frames.get(1).flags);
        assertEquals(0f, frames.get(1).pressure, 0f);
    }

    @Test public void unknownInitialContactFallsBackToPressureUntilStateBecomesExplicit() {
        TabletInput.Sample pressurized = new TabletInput.Sample(2, 4,
                TabletInput.TOOL_PEN, TabletInput.CAP_PRESSURE,
                10, 20, 0.5f, 0, 0, 0,
                true, true, TabletInput.TIP_STATE_UNKNOWN, false, 0, 100);
        TabletInput.Sample pressureReleased = new TabletInput.Sample(2, 4,
                TabletInput.TOOL_PEN, TabletInput.CAP_PRESSURE,
                11, 21, 0f, 0, 0, 0,
                true, false, TabletInput.TIP_STATE_UNKNOWN, false, 0, 116);
        input.update(pressurized);
        input.update(pressureReleased);

        assertEquals(TabletInput.FLAG_PROXIMITY | TabletInput.FLAG_TIP_DOWN,
                frames.get(0).flags);
        assertEquals(TabletInput.FLAG_PROXIMITY, frames.get(1).flags);
    }

    @Test public void androidTiltAzimuthProjectsToSignedWaylandAxes() {
        float[] towardScreenBottom = TabletInput.projectTilt((float) (Math.PI / 4), 0f);
        float[] towardScreenRight = TabletInput.projectTilt((float) (Math.PI / 4),
                (float) (-Math.PI / 2));
        float[] towardScreenTop = TabletInput.projectTilt((float) (Math.PI / 4),
                (float) Math.PI);

        assertEquals(0f, towardScreenBottom[0], 0.001f);
        assertEquals(45f, towardScreenBottom[1], 0.001f);
        assertEquals(45f, towardScreenRight[0], 0.001f);
        assertEquals(0f, towardScreenRight[1], 0.001f);
        assertEquals(0f, towardScreenTop[0], 0.001f);
        assertEquals(-45f, towardScreenTop[1], 0.001f);
    }

    @Test public void cancelOfOnePointerDoesNotCancelAnotherOrReleaseTwice() {
        input.update(sample(9, 1, TabletInput.TOOL_PEN, true, true, 0, 10));
        input.update(sample(9, 2, TabletInput.TOOL_ERASER, true, true, 0, 11));
        TabletInput.Sample cancelFirst = new TabletInput.Sample(9, 1,
                TabletInput.TOOL_PEN, TabletInput.CAP_PRESSURE,
                100, 200, 0, 0, 0, 0, true, false,
                TabletInput.TIP_STATE_UNKNOWN, true, 0, 12);
        input.update(cancelFirst);

        assertEquals(3, frames.size());
        assertEquals(TabletInput.FLAG_CANCELED, frames.get(2).flags);
        assertTrue(input.hasActiveTools());
        input.cancelAll(13);
        assertEquals(4, frames.size());
        assertEquals(TabletInput.FLAG_CANCELED, frames.get(3).flags);
        assertEquals(frames.get(1).toolId, frames.get(3).toolId);
        assertFalse(input.hasActiveTools());
    }

    @Test public void capabilityChangesAllocateMetadataStableIdentity() {
        input.update(sample(6, 0, TabletInput.TOOL_PEN, true, false, 0, 1));
        TabletInput.Sample changed = new TabletInput.Sample(6, 0,
                TabletInput.TOOL_PEN, TabletInput.CAP_PRESSURE | TabletInput.CAP_TILT,
                100, 200, 0.2f, 10, 15, 0, true, false, false, 0, 2);
        input.update(changed);

        assertEquals(3, frames.size());
        assertEquals(TabletInput.FLAG_CANCELED, frames.get(1).flags);
        assertNotEquals(frames.get(0).toolId, frames.get(2).toolId);
        assertEquals(TabletInput.CAP_PRESSURE | TabletInput.CAP_TILT,
                frames.get(2).capabilities);
    }

    @Test public void identitiesBeyondSixteenReachProducerAndResetOnlyAfterCancel() {
        for (int device = 0; device < 20; device++)
            input.update(sample(device, 0, TabletInput.TOOL_PEN, true, false, 0, device));

        assertEquals(20, frames.size());
        for (int i = 0; i < 20; i++)
            assertEquals(i + 1, frames.get(i).toolId);

        input.cancelAll(30);
        assertEquals(40, frames.size());
        for (int i = 20; i < 40; i++)
            assertEquals(TabletInput.FLAG_CANCELED, frames.get(i).flags);
        input.resetSession();

        input.update(sample(50, 0, TabletInput.TOOL_PEN, true, false, 0, 31));
        assertEquals(41, frames.size());
        assertEquals(1, frames.get(40).toolId);
    }
}
