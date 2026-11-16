/* SPDX-License-Identifier: MIT */
#include "anland_hdr.h"
#include <assert.h>
#include <stdio.h>
#include <limits.h>

int main(void)
{
    static const struct { const char *value; bool disabled; } cases[] = {
        {NULL, false}, {"", false}, {"1", false}, {"auto", false},
        {"0", true}, {"false", true}, {"no", true}, {"off", true},
        {" \tFaLsE\n", true}, {" OFF ", true}, {" nO\r", true},
        {" 0 ", true}, {"\t\n ", false}, {"unknown", false},
        {"falsex", false}, {"00", false}, {"true", false},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        const bool disabled = anland_hdr_environment_disabled(cases[i].value);
        assert(disabled == cases[i].disabled);
        assert(anland_hdr_mode_for_hal(1, !disabled) == ANLAND_HDR_SDR);
        assert(anland_hdr_mode_for_hal(2, !disabled) == ANLAND_HDR_SDR);
        assert(anland_hdr_mode_for_hal(5, !disabled) == ANLAND_HDR_SDR);
        assert(anland_hdr_mode_for_hal(43, !disabled) == (disabled ? ANLAND_HDR_REJECTED : ANLAND_HDR_HDR10));
        assert(anland_hdr_mode_for_hal(0, !disabled) == ANLAND_HDR_REJECTED);
        assert(anland_hdr_mode_for_hal(999, !disabled) == ANLAND_HDR_REJECTED);
    }
    for (int32_t value = 80; value <= 500; ++value) {
        uint32_t white = 203;
        assert(anland_hdr_sdr_white_from_action(ANLAND_HDR_ACTION_SDR_WHITE, value, &white));
        assert(white == (uint32_t)value);
    }
    const int32_t invalid[] = {INT32_MIN, -1, 0, 79, 501, INT32_MAX};
    for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        uint32_t white = 300;
        assert(!anland_hdr_sdr_white_from_action(ANLAND_HDR_ACTION_SDR_WHITE, invalid[i], &white));
        assert(white == 300);
    }
    uint32_t white = 400;
    assert(!anland_hdr_sdr_white_from_action(0, 300, &white));
    assert(white == 400);
    assert(!anland_hdr_sdr_white_from_action(ANLAND_HDR_ACTION_SDR_WHITE, 300, NULL));
    for (int32_t value = 50; value <= 200; ++value) {
        uint32_t gain = 100;
        assert(anland_hdr_content_gain_from_action(ANLAND_HDR_ACTION_CONTENT_GAIN, value, &gain));
        assert(gain == (uint32_t)value);
    }
    const int32_t invalid_gain[] = {INT32_MIN, -1, 0, 49, 201, INT32_MAX};
    for (size_t i = 0; i < sizeof(invalid_gain) / sizeof(invalid_gain[0]); ++i) {
        uint32_t gain = 150;
        assert(!anland_hdr_content_gain_from_action(ANLAND_HDR_ACTION_CONTENT_GAIN, invalid_gain[i], &gain));
        assert(gain == 150);
    }
    uint32_t gain = 150;
    assert(!anland_hdr_content_gain_from_action(ANLAND_HDR_ACTION_SDR_WHITE, 100, &gain));
    assert(gain == 150);
    assert(!anland_hdr_content_gain_from_action(ANLAND_HDR_ACTION_CONTENT_GAIN, 100, NULL));
    assert(!anland_hdr_sdr_white_from_action(ANLAND_HDR_ACTION_CONTENT_GAIN, 100, &white));
    assert(white == 400);
    puts("HDR policy: environment/format, independent SDR white and HDR gain validation passed");
    return 0;
}
