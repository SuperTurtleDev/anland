#include "../anland_backend/hyprland/src/anland/OutputColor.hpp"
#include "../../../libdisplay_producer/anland_hdr.h"
#include <cassert>
#include <string>

enum class CM { SRGB, HDR, ICC };
enum class TF { SRGB, GAMMA22 };
struct Rule {
    bool m_enable10bit = true;
    CM m_cmType = CM::ICC;
    TF m_sdrEotf = TF::GAMMA22;
    std::string m_iccFile = "conflicting-profile.icc";
    int m_supportsHDR = 1, m_supportsWideColor = 1;
    float m_sdrSaturation = 2, m_sdrBrightness = 3, m_sdrMinLuminance = 4;
    int m_sdrMaxLuminance = 80;
    float m_minLuminance = -1;
    int m_maxLuminance = 500, m_maxAvgLuminance = 200;
};

static void apply(Rule& rule, anland_hdr_mode mode) {
    assert(mode != ANLAND_HDR_REJECTED);
    Anland::fixedOutputColorRule(rule, mode == ANLAND_HDR_HDR10, CM::HDR, CM::SRGB, TF::SRGB,
                                ANLAND_HDR_SDR_WHITE_NITS, ANLAND_HDR_PEAK_NITS);
}

int main() {
    // A monitor ICC/HDR rule cannot override an SDR consumer, even with env=1.
    for (auto hal : {1u, 2u, 5u}) {
        Rule rule;
        apply(rule, anland_hdr_mode_for_hal(hal, !anland_hdr_environment_disabled("1")));
        assert(!rule.m_enable10bit && rule.m_cmType == CM::SRGB && rule.m_sdrEotf == TF::SRGB);
        assert(rule.m_iccFile.empty() && rule.m_supportsHDR == -1 && rule.m_supportsWideColor == -1);
    }
    Rule rule;
    // Same-size reconnects may repeatedly alternate the fixed wire encoding.
    for (int i = 0; i < 8; ++i) {
        apply(rule, anland_hdr_mode_for_hal(43, true));
        assert(rule.m_enable10bit && rule.m_cmType == CM::HDR && rule.m_supportsHDR == 1);
        assert(rule.m_sdrMaxLuminance == 203 && rule.m_maxLuminance == 1000 && rule.m_maxAvgLuminance == 1000);
        assert(rule.m_iccFile.empty() && rule.m_sdrSaturation == 1 && rule.m_sdrBrightness == 1);
        apply(rule, anland_hdr_mode_for_hal(1, true));
        assert(!rule.m_enable10bit && rule.m_cmType == CM::SRGB && rule.m_maxLuminance == -1);
    }
    for (const auto* denied : {"0", "FALSE", " no ", "\tOff\n"})
        assert(anland_hdr_mode_for_hal(43, !anland_hdr_environment_disabled(denied)) == ANLAND_HDR_REJECTED);
    for (const auto* automatic : {static_cast<const char*>(nullptr), "", "1", "auto", "unknown"})
        assert(anland_hdr_mode_for_hal(43, !anland_hdr_environment_disabled(automatic)) == ANLAND_HDR_HDR10);
}
