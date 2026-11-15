/* SPDX-License-Identifier: MIT */
#include "anland_hdr.h"
#include <assert.h>
#include <stdio.h>

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
    puts("HDR policy: 17 environment cases and all supported/unknown formats passed");
    return 0;
}
