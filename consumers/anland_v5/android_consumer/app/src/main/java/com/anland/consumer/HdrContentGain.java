package com.anland.consumer;

/** Gain for native HDR inputs, independent of SDR reference white. */
final class HdrContentGain {
    static final String KEY_PERCENT = "hdr_content_gain_percent";
    static final int MIN_PERCENT = 50;
    static final int MAX_PERCENT = 200;
    static final int DEFAULT_PERCENT = 100;
    static final int STEP_PERCENT = 5;

    private HdrContentGain() { }

    static int clamp(int percent) {
        return Math.max(MIN_PERCENT, Math.min(MAX_PERCENT, percent));
    }
}
