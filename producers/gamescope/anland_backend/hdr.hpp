/* Anland's fixed consumer color contract, separate from Gamescope's HDR toggle. */
#pragma once
#include "anland_hdr.h"
#include "color_helpers.h"
#include <drm_fourcc.h>
#include <optional>

static inline uint32_t anland_gamescope_drm_format(uint32_t format)
{
    switch (format) {
    case 1: return DRM_FORMAT_ABGR8888;
    case 2: return DRM_FORMAT_XBGR8888;
    case 5: return DRM_FORMAT_ARGB8888;
    case 43: return DRM_FORMAT_ABGR2101010;
    default: return DRM_FORMAT_INVALID;
    }
}

namespace gamescope {
template<typename ColorMgmt>
inline void AnlandConfigureHDRColorMgmt(ColorMgmt &color, float sdrWhiteNits = ANLAND_HDR_SDR_WHITE_NITS, float contentGain = 1.f)
{
    color.enabled = true;
    color.flSDROnHDRBrightness = sdrWhiteNits;
    color.flHDRInputGain = contentGain;
    // Gamescope's automatic setting expands SDR to the panel gamut. This
    // virtual BT.2020 container has no panel primaries; preserve Rec.709 SDR.
    if (color.sdrGamutWideness < 0.f)
        color.sdrGamutWideness = 0.f;
}

inline void AnlandNativeColorimetry(bool hdr, bool hdr10,
    displaycolorimetry_t *displayColorimetry, EOTF *displayEOTF,
    displaycolorimetry_t *outputColorimetry, EOTF *outputEOTF)
{
    *displayColorimetry = hdr ? displaycolorimetry_2020 : displaycolorimetry_709;
    *displayEOTF = EOTF_Gamma22;
    *outputColorimetry = *displayColorimetry;
    *outputEOTF = hdr && hdr10 ? EOTF_PQ : EOTF_Gamma22;
}

/* A consumer owns its output encoding. Other backends retain the native toggle. */
std::optional<bool> AnlandOutputHDRMode();
float AnlandOutputSDRWhiteNits();
float AnlandOutputHDRContentGain();
}
