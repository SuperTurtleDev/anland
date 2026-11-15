package com.anland.consumer;

/** SDR reference white inside HDR output; this never changes the display mode. */
final class HdrBrightness {
    static final String KEY_SDR_WHITE_NITS = "hdr_sdr_white_nits";
    static final int MIN_NITS = 80;
    static final int MAX_NITS = 500;
    static final int DEFAULT_NITS = 203;

    private HdrBrightness() { }

    static int clamp(int nits) {
        return Math.max(MIN_NITS, Math.min(MAX_NITS, nits));
    }
}
