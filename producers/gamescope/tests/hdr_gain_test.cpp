// The real native CPU LUT generator, not a replacement PQ implementation.
#include "color_helpers_impl.h"
#include "hdr.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>

glm::vec3 anland_test_native_luts(const lut1d_t &, const lut3d_t &, glm::vec3);

struct Settings {
    bool enabled = false;
    float flSDROnHDRBrightness = 203.f, flHDRInputGain = 1.f, flSDRInputGain = 1.f, sdrGamutWideness = 0.f;
};

static double pq(double nits) {
    const double p = std::pow(nits / 10000., 2610. / 16384.);
    return std::pow((3424. / 4096. + 2413. / 128. * p) / (1. + 2392. / 128. * p), 2523. / 32.);
}
static void check(bool ok, const char *why) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", why); std::exit(1); }
}
int main() {
    double worst = 0;
    for (float gain : {.5f, 1.f, 2.f, 1.f}) {
        Settings settings;
        gamescope::AnlandConfigureHDRColorMgmt(settings, 203.f, gain);
        check(settings.flHDRInputGain == gain && settings.flSDRInputGain == 1 && settings.flSDROnHDRBrightness == 203,
              "HDR gain does not change SDR gain/reference white");
        for (bool hdr : {false, true}) {
            displaycolorimetry_t input{};
            colormapping_t mapping{};
            if (hdr) buildPQColorimetry(&input, &mapping, displaycolorimetry_2020);
            else buildSDRColorimetry(&input, &mapping, 0.f, displaycolorimetry_2020);
            tonemapping_t tone{};
            tone.bNormalizeShaper = true;
            tone.g22_luminance = hdr ? 1.f : settings.flSDROnHDRBrightness;
            tone.contentGainPeakNits = hdr && gain != 1.f ? 1000.f : 0.f;
            lut1d_t shaper; lut3d_t lut;
            calcColorTransform<rendervulkan::s_nLutEdgeSize3d>(&shaper, rendervulkan::s_nLutSize1d, &lut,
                input, hdr ? EOTF_PQ : EOTF_Gamma22, displaycolorimetry_2020, EOTF_PQ, displaycolorimetry_2020,
                glm::vec2(0.f), k_EChromaticAdapatationMethod_Bradford, mapping, nightmode_t{}, tone, nullptr,
                hdr ? settings.flHDRInputGain : settings.flSDRInputGain);
            for (double nits : {0., .005, 1., 20., 80., 203., 400., 500., 600., 1000.}) {
                if (!hdr && nits > 203) continue;
                const float encoded = hdr ? float(pq(nits)) : float(std::pow(nits / 203., 1. / 2.2));
                const glm::vec3 actual = anland_test_native_luts(shaper, lut, glm::vec3(encoded));
                const double expected = pq(hdr ? std::min(nits * gain, 1000.) : nits);
                for (int c = 0; c < 3; ++c) {
                    const double error = std::abs(actual[c] - expected) * 1023;
                    worst = std::max(worst, error);
                    if (!std::isfinite(actual[c]) || error >= 1.2) {
                        std::fprintf(stderr, "gain %.2f HDR %d nits %.5f actual %.8f expected %.8f codes %.4f\n",
                                     gain, hdr, nits, actual[c], expected, error);
                        check(false, "native LUT: HDR linear gain/1000-nit cap, SDR invariant");
                    }
                }
            }
        }
    }
    std::printf("Gamescope native LUT gain .5/1/2/reset, black/SDR/1000-nit cap passed; max error %.4f codes\n", worst);
}
