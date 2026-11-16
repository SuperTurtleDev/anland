#pragma once

namespace Anland {
    // Independent of Hyprland's rule layout/version; native enum values are
    // supplied by the bridge. Reapply on reconnect and on configuration reload.
    template <typename Rule, typename CM, typename TF>
    void fixedOutputColorRule(Rule& rule, bool hdr, CM hdrCM, CM sdrCM, TF sdrTF, float whiteNits, float peakNits) {
        rule.m_enable10bit = hdr;
        rule.m_cmType = hdr ? hdrCM : sdrCM;
        rule.m_sdrEotf = sdrTF;
        rule.m_iccFile.clear();
        rule.m_supportsHDR = hdr ? 1 : -1;
        rule.m_supportsWideColor = hdr ? 1 : -1;
        rule.m_sdrSaturation = 1;
        rule.m_sdrBrightness = 1;
        rule.m_sdrMinLuminance = 0;
        rule.m_sdrMaxLuminance = whiteNits;
        rule.m_minLuminance = 0;
        rule.m_maxLuminance = hdr ? peakNits : -1;
        rule.m_maxAvgLuminance = hdr ? peakNits : -1;
    }
}
