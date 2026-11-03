package com.anland.consumer;

import java.util.List;
import org.junit.Test;
import static org.junit.Assert.*;

public class EvdevTabletStateTest {
    private static final int ABS_X = 0x00, ABS_Y = 0x01;
    private static final int ABS_TILT_X = 0x1a, ABS_TILT_Y = 0x1b;
    private static final int ABS_MT_SLOT = 0x2f;
    private static final int ABS_MT_POSITION_X = 0x35, ABS_MT_POSITION_Y = 0x36;
    private static final int ABS_MT_TOOL_TYPE = 0x37, ABS_MT_TRACKING_ID = 0x39;
    private static final int ABS_MT_PRESSURE = 0x3a, ABS_MT_DISTANCE = 0x3b;
    private static final int BTN_TOOL_PEN = 0x140, BTN_TOOL_RUBBER = 0x141;
    private static final int BTN_TOUCH = 0x14a, BTN_STYLUS = 0x14b, BTN_STYLUS2 = 0x14c;
    private static final int BTN_TOOL_FINGER = 0x145;

    @Test public void decodesMtPenAxesAndTreatsPositiveDistanceAsHover() {
        EvdevTabletState state = new EvdevTabletState();
        state.setRange(ABS_MT_PRESSURE, 0, 100);
        state.setRange(ABS_MT_DISTANCE, 0, 40);
        state.setRange(ABS_TILT_X, -60, 60);
        state.setRange(ABS_TILT_Y, -60, 60);
        state.key(BTN_TOOL_PEN, 1);
        state.key(BTN_TOUCH, 1);
        state.key(BTN_STYLUS, 1);
        state.key(BTN_STYLUS2, 1);
        state.abs(ABS_MT_SLOT, 0);
        state.abs(ABS_MT_TRACKING_ID, 12);
        state.abs(ABS_MT_TOOL_TYPE, 1);
        state.abs(ABS_MT_POSITION_X, 1000);
        state.abs(ABS_MT_POSITION_Y, 2000);
        state.abs(ABS_MT_PRESSURE, 50);
        state.abs(ABS_MT_DISTANCE, 20);
        state.abs(ABS_TILT_X, 12);
        state.abs(ABS_TILT_Y, -8);

        List<EvdevTabletState.Sample> samples = state.endFrame(1234);
        assertEquals(1, samples.size());
        EvdevTabletState.Sample pen = samples.get(0);
        assertEquals(0, pen.pointerId);
        assertEquals(TabletInput.TOOL_PEN, pen.toolType);
        assertEquals(TabletInput.CAP_PRESSURE | TabletInput.CAP_TILT
                | TabletInput.CAP_DISTANCE, pen.capabilities);
        assertEquals(1000f, pen.x, 0f);
        assertEquals(2000f, pen.y, 0f);
        assertEquals(0.5f, pen.pressure, 0.001f);
        assertEquals(0.5f, pen.distance, 0.001f);
        assertEquals(12f, pen.tiltX, 0f);
        assertEquals(-8f, pen.tiltY, 0f);
        assertTrue(pen.proximity);
        assertFalse(pen.tipDown);
        assertEquals(TabletInput.TIP_STATE_UP, pen.tipState);
        assertEquals(TabletInput.BUTTON_PRIMARY | TabletInput.BUTTON_SECONDARY, pen.buttons);
        assertEquals(1234, pen.timeMsec);
    }

    @Test public void sharedFingerBtnTouchDoesNotInventPenContactAtZeroPressure() {
        EvdevTabletState state = new EvdevTabletState();
        state.setRange(ABS_MT_PRESSURE, 0, 255);
        state.key(BTN_TOOL_PEN, 1);
        state.key(BTN_TOUCH, 1); // This BTN_TOUCH belongs to the finger contact.

        state.abs(ABS_MT_SLOT, 0);
        state.abs(ABS_MT_TOOL_TYPE, 1);
        state.abs(ABS_MT_POSITION_X, 500);
        state.abs(ABS_MT_POSITION_Y, 700);
        state.abs(ABS_MT_PRESSURE, 0);
        state.abs(ABS_MT_SLOT, 1);
        state.abs(ABS_MT_TRACKING_ID, 27);
        state.abs(ABS_MT_TOOL_TYPE, 0);
        state.abs(ABS_MT_POSITION_X, 200);
        state.abs(ABS_MT_POSITION_Y, 300);

        List<EvdevTabletState.Sample> samples = state.endFrame(20);
        assertEquals(1, samples.size());
        EvdevTabletState.Sample pen = samples.get(0);
        assertEquals(0, pen.pointerId);
        assertTrue(pen.proximity);
        assertFalse(pen.tipDown);
        assertEquals(TabletInput.TIP_STATE_UNKNOWN, pen.tipState);
        assertEquals(0f, pen.pressure, 0f);
    }

    @Test public void trackedPenContactSurvivesFingerReleaseAndZeroPressure() {
        EvdevTabletState state = new EvdevTabletState();
        state.setRange(ABS_MT_PRESSURE, 0, 100);
        state.key(BTN_TOOL_PEN, 1);
        state.key(BTN_TOUCH, 1);
        state.abs(ABS_MT_SLOT, 0);
        state.abs(ABS_MT_TRACKING_ID, 8);
        state.abs(ABS_MT_TOOL_TYPE, 1);
        state.abs(ABS_MT_POSITION_X, 100);
        state.abs(ABS_MT_POSITION_Y, 200);
        state.abs(ABS_MT_PRESSURE, 0);

        EvdevTabletState.Sample pen = state.endFrame(10).get(0);
        assertEquals(TabletInput.TIP_STATE_DOWN, pen.tipState);
        assertTrue(pen.tipDown);
        assertEquals(0f, pen.pressure, 0f);

        // A finger is added while the pen remains tracked; a device-wide BTN_TOUCH
        // release caused by that finger must not lift the pen's explicit MT slot.
        state.key(BTN_TOUCH, 0);
        state.abs(ABS_MT_SLOT, 1);
        state.abs(ABS_MT_TRACKING_ID, 9);
        state.abs(ABS_MT_TOOL_TYPE, 0);
        state.abs(ABS_MT_POSITION_X, 300);
        state.abs(ABS_MT_POSITION_Y, 400);
        pen = state.endFrame(11).get(0);
        assertEquals(TabletInput.TIP_STATE_DOWN, pen.tipState);
        assertTrue(pen.tipDown);
    }

    @Test public void trackedPenWithPositiveDistanceHoversBesideFingerContact() {
        EvdevTabletState penState = new EvdevTabletState();
        EvdevTouchState touchState = new EvdevTouchState();
        penState.setRange(ABS_MT_DISTANCE, 0, 40);
        penState.setRange(ABS_MT_PRESSURE, 0, 100);
        penState.key(BTN_TOOL_PEN, 1);
        penState.key(BTN_TOUCH, 1);
        touchState.key(BTN_TOUCH, true);

        int[] codes = { ABS_MT_SLOT, ABS_MT_TRACKING_ID, ABS_MT_TOOL_TYPE,
                ABS_MT_POSITION_X, ABS_MT_POSITION_Y, ABS_MT_PRESSURE, ABS_MT_DISTANCE };
        int[] penValues = { 0, 15, 1, 100, 200, 60, 10 };
        for (int i = 0; i < codes.length; i++) {
            penState.abs(codes[i], penValues[i]);
            touchState.abs(codes[i], penValues[i]);
        }
        int[] fingerValues = { 1, 22, 0, 300, 400 };
        int[] fingerCodes = { ABS_MT_SLOT, ABS_MT_TRACKING_ID, ABS_MT_TOOL_TYPE,
                ABS_MT_POSITION_X, ABS_MT_POSITION_Y };
        for (int i = 0; i < fingerCodes.length; i++) {
            penState.abs(fingerCodes[i], fingerValues[i]);
            touchState.abs(fingerCodes[i], fingerValues[i]);
        }

        List<EvdevTabletState.Sample> tools = penState.endFrame(33);
        assertEquals(1, tools.size());
        EvdevTabletState.Sample pen = tools.get(0);
        assertEquals(TabletInput.TOOL_PEN, pen.toolType);
        assertTrue(pen.proximity);
        assertFalse(pen.tipDown);
        assertEquals(TabletInput.TIP_STATE_UP, pen.tipState);
        assertEquals(0.25f, pen.distance, 0.001f);

        touchState.endFrame();
        int[] fingerSlots = new int[EvdevTouchState.MAX_CONTACTS];
        assertEquals(1, touchState.gather(fingerSlots));
        assertEquals(1, fingerSlots[0]);
    }

    @Test public void positiveDistanceWinsOverTrackedIdAndSharedBtnTouch() {
        EvdevTabletState state = new EvdevTabletState();
        state.setRange(ABS_MT_DISTANCE, 0, 100);
        state.setRange(ABS_MT_PRESSURE, 0, 100);
        state.key(BTN_TOOL_PEN, 1);
        state.key(BTN_TOOL_FINGER, 1);
        state.key(BTN_TOUCH, 1);
        state.abs(ABS_MT_SLOT, 0);
        state.abs(ABS_MT_TRACKING_ID, 3);
        state.abs(ABS_MT_TOOL_TYPE, 1);
        state.abs(ABS_MT_POSITION_X, 100);
        state.abs(ABS_MT_POSITION_Y, 200);
        state.abs(ABS_MT_PRESSURE, 75);
        state.abs(ABS_MT_DISTANCE, 25);
        state.abs(ABS_MT_SLOT, 1);
        state.abs(ABS_MT_TRACKING_ID, 4);
        state.abs(ABS_MT_TOOL_TYPE, 0);
        state.abs(ABS_MT_POSITION_X, 300);
        state.abs(ABS_MT_POSITION_Y, 400);

        EvdevTabletState.Sample pen = state.endFrame(44).get(0);
        assertTrue(pen.proximity);
        assertFalse(pen.tipDown);
        assertEquals(TabletInput.TIP_STATE_UP, pen.tipState);
        assertEquals(0.25f, pen.distance, 0.001f);
    }

    @Test public void legacyAbsCoordinatesMapIntoDifferentMtRange() {
        EvdevTabletState state = new EvdevTabletState();
        state.setRange(ABS_X, 0, 1000);
        state.setRange(ABS_Y, 0, 2000);
        state.setRange(ABS_MT_POSITION_X, 100, 2100);
        state.setRange(ABS_MT_POSITION_Y, 500, 2500);
        state.abs(ABS_X, 500);
        state.abs(ABS_Y, 1000);
        state.key(BTN_TOOL_PEN, 1);

        EvdevTabletState.Sample pen = state.endFrame(55).get(0);
        assertEquals(ABS_X, pen.coordinateAxisX);
        assertEquals(ABS_Y, pen.coordinateAxisY);
        assertEquals(1100f, EvdevTabletState.mapCoordinate(pen.x,
                state.rangeMin(pen.coordinateAxisX), state.rangeMax(pen.coordinateAxisX),
                100, 2100), 0.01f);
        assertEquals(1500f, EvdevTabletState.mapCoordinate(pen.y,
                state.rangeMin(pen.coordinateAxisY), state.rangeMax(pen.coordinateAxisY),
                500, 2500), 0.01f);
    }

    @Test public void proximityOutReleasesButtonAndEraserIsNativeToolType() {
        EvdevTabletState state = new EvdevTabletState();
        state.abs(ABS_X, 12);
        state.abs(ABS_Y, 34);
        state.key(BTN_TOOL_RUBBER, 1);
        state.key(BTN_STYLUS, 1);
        EvdevTabletState.Sample eraser = state.endFrame(50).get(0);
        assertEquals(TabletInput.TOOL_ERASER, eraser.toolType);
        assertEquals(TabletInput.BUTTON_PRIMARY, eraser.buttons);
        assertTrue(eraser.proximity);

        state.key(BTN_STYLUS, 0);
        state.key(BTN_TOOL_RUBBER, 0);
        EvdevTabletState.Sample out = state.endFrame(51).get(0);
        assertFalse(out.proximity);
        assertFalse(out.tipDown);
        assertEquals(0, out.buttons);
        assertEquals(TabletInput.TIP_STATE_UP, out.tipState);
    }
}
