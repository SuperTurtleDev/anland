/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "anland_color.h"
#include "libdisplay_producer/anland_hdr.h"

#include <cmath>
#include <map>

namespace KWin
{

inline const ColorDescription &anlandColorDescriptionValue(const AnlandColorDescription &description)
{
#ifdef ANLAND_KWIN_66
    return *description;
#else
    return description;
#endif
}

// Classify the source's declared encoding, never its storage format or just its
// gamut. In particular our gamma22 FP16 scene is not another HDR client input.
inline bool anlandIsNativeHdrContent(const AnlandColorDescription &description)
{
    const auto &color = anlandColorDescriptionValue(description);
    const auto transfer = color.transferFunction();
    if (transfer.type == TransferFunction::PerceptualQuantizer) {
        return true;
    }
#ifdef ANLAND_KWIN_68
    if (transfer.type == TransferFunction::HLG) {
        return true;
    }
#endif
#ifdef ANLAND_KWIN_64
    const auto srgbPrimaries = Colorimetry::BT709;
#else
    const auto srgbPrimaries = Colorimetry::fromName(NamedColorimetry::BT709);
#endif
    // KWin's Windows scRGB description: code 1 is 80 nit and reference white
    // is 203 nit. An arbitrary linear SDR texture is not sufficient evidence.
    return transfer.type == TransferFunction::linear
        && std::abs(transfer.minLuminance) < 1e-6
        && std::abs(transfer.maxLuminance - 80.0) < 1e-6
        && std::abs(color.referenceLuminance() - 203.0) < 1e-6
        && color.containerColorimetry() == srgbPrimaries;
}

struct AnlandHdrGainTarget {
    AnlandColorDescription color;
    unsigned int percent;
};

// KWin's scene and output state run on the compositor thread. Registration is
// scoped to Anland outputs, so non-Anland rendering and SDR sessions keep the
// original path. All outputs of one transport share its consumer setting.
inline std::map<const void *, AnlandHdrGainTarget> &anlandHdrGainTargets()
{
    static std::map<const void *, AnlandHdrGainTarget> targets;
    return targets;
}

inline void anlandRegisterHdrGainTarget(const void *owner, const AnlandColorDescription &color,
                                       unsigned int percent, bool enabled)
{
    auto &targets = anlandHdrGainTargets();
    targets.erase(owner);
    if (enabled) {
        targets.emplace(owner, AnlandHdrGainTarget{color, percent});
    }
}

inline void anlandUnregisterHdrGainTarget(const void *owner)
{
    anlandHdrGainTargets().erase(owner);
}

inline double anlandHdrContentGainForSurface(const AnlandColorDescription &source,
                                            const AnlandColorDescription &target)
{
    if (!anlandIsNativeHdrContent(source)) {
        return 1.0;
    }
    for (const auto &[owner, entry] : anlandHdrGainTargets()) {
        if (anlandColorDescriptionValue(entry.color) == anlandColorDescriptionValue(target)) {
            return entry.percent / 100.0;
        }
    }
    return 1.0;
}

// Called immediately after native per-surface colorspace uniforms are set.
// Reuses the existing linear color matrix and tone mapper, without new GLSL.
void anlandApplyHdrContentGain(GLShader *shader, const AnlandColorDescription &source,
                               const AnlandColorDescription &target, RenderingIntent intent);

} // namespace KWin
