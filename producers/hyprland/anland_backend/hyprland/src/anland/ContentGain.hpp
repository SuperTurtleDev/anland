#pragma once

namespace Anland {
    // Only the original client surface may receive gain. Internal FP16 images
    // (including finalMonitorCM) are also EXT_LINEAR and must never receive it.
    inline float clientHdrGain(bool hdrSurface, bool originalClientImage, bool finalPass, bool copyPass, float outputGain) {
        return hdrSurface && originalClientImage && !finalPass && !copyPass ? outputGain : 1.0f;
    }

    template<typename Settings>
    void applyHdrContentGain(Settings& settings, float gain) {
        if (gain == 1.0f)
            return;
        for (auto& row : settings.convertMatrix)
            for (auto& value : row)
                value *= gain;
        // Reuse native linear-nit clipping; keep intermediate tones linear and
        // the fixed 1000-nit destination. Unity retains native tone mapping.
        settings.needsTonemap = true;
        settings.maxLuminance = settings.dstMaxLuminance;
        if constexpr (requires { settings.tonemapMode; })
            settings.tonemapMode = 2;
    }
}
