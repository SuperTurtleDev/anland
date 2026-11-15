/* Tests the production adapter and the exact Gamescope native LUT generator.
 * No compositor, Android device or Vulkan driver is mocked as a working GPU. */
#include "rendervulkan.hpp"
#include "hdr.hpp"
#include <cstdio>
#include <cstdlib>
#include <cmath>

glm::vec3 anland_test_native_luts(const lut1d_t &, const lut3d_t &, glm::vec3);

static void require(bool condition, const char *message)
{
    if (!condition) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
static double pq(double nits)
{
    constexpr double m1 = 2610.0 / 16384.0, m2 = 2523.0 / 32.0;
    const double p = std::pow(std::clamp(nits / 10000.0, 0.0, 1.0), m1);
    return std::pow((3424.0 / 4096.0 + 2413.0 / 128.0 * p) /
        (1.0 + 2392.0 / 128.0 * p), m2);
}
static double code_error(const glm::vec3 &actual, const glm::vec3 &expected)
{
    double error = 0;
    for (int c = 0; c < 3; ++c) {
        require(std::isfinite(actual[c]), "non-finite LUT output");
        error = std::max(error, std::abs(double(actual[c]) - expected[c]) * 1023.0);
    }
    return error;
}
static glm::vec3 encode(const lut1d_t &shaper, const lut3d_t &lut, glm::vec3 value)
{
    // The native composite shader uses linear 1D + tetrahedral 3D sampling.
    return anland_test_native_luts(shaper, lut, value);
}
int main()
{
    struct Env { const char *value; bool disabled; } environments[] = {
        {nullptr, false}, {"", false}, {"1", false}, {"auto", false},
        {"0", true}, {"false", true}, {"no", true}, {"off", true},
        {" \tFaLsE\n", true}, {" OFF ", true}, {"\nNo\t", true},
        {" 1 ", false}, {"enabled", false}, {"unknown", false}, {"00", false},
    };
    for (const auto &env : environments) {
        require(anland_hdr_environment_disabled(env.value) == env.disabled, "environment policy");
        for (uint32_t format : {1u, 2u, 5u}) {
            require(anland_hdr_mode_for_hal(format, !env.disabled) == ANLAND_HDR_SDR,
                "8-bit must remain SDR for every environment value");
            require(anland_gamescope_drm_format(format) != DRM_FORMAT_INVALID, "8-bit DRM format");
        }
        require(anland_hdr_mode_for_hal(43, !env.disabled) ==
            (env.disabled ? ANLAND_HDR_REJECTED : ANLAND_HDR_HDR10), "HAL43 automatic HDR policy");
        require(anland_hdr_mode_for_hal(999, !env.disabled) == ANLAND_HDR_REJECTED, "unknown HAL rejection");
    }
    require(anland_gamescope_drm_format(43) == DRM_FORMAT_ABGR2101010, "HAL43 channel packing");
    require(anland_gamescope_drm_format(2) == DRM_FORMAT_XBGR8888, "RGBX channel packing");
    require(anland_gamescope_drm_format(5) == DRM_FORMAT_ARGB8888, "BGRA channel packing");
    require(anland_gamescope_drm_format(999) == DRM_FORMAT_INVALID, "unknown DRM rejection");

    displaycolorimetry_t display{}, output{};
    EOTF displayTF{}, outputTF{};
    for (bool hdr : {false, true, false, true}) {
        gamescope::AnlandNativeColorimetry(hdr, hdr, &display, &displayTF, &output, &outputTF);
        require(display == (hdr ? displaycolorimetry_2020 : displaycolorimetry_709), "native gamut transition");
        require(output == display, "output gamut transition");
        require(displayTF == EOTF_Gamma22, "native panel transfer function");
        require(outputTF == (hdr ? EOTF_PQ : EOTF_Gamma22), "native output transfer function transition");
    }
    gamescope_color_mgmt_t settings{};
    gamescope::AnlandConfigureHDRColorMgmt(settings);
    require(settings.enabled && settings.flSDROnHDRBrightness == 203.f, "native PQ pipeline and SDR white");
    require(settings.sdrGamutWideness == 0.f, "automatic SDR must preserve Rec.709");
    settings.sdrGamutWideness = 0.4f;
    gamescope::AnlandConfigureHDRColorMgmt(settings);
    require(settings.sdrGamutWideness == 0.4f, "explicit native gamut preference preserved");
    settings.sdrGamutWideness = 0.f;

    displaycolorimetry_t input{};
    colormapping_t mapping{};
    buildSDRColorimetry(&input, &mapping, settings.sdrGamutWideness, display);
    require(glm::length(input.primaries.r - displaycolorimetry_709.primaries.r) < 1e-6f &&
        glm::length(input.primaries.g - displaycolorimetry_709.primaries.g) < 1e-6f &&
        glm::length(input.primaries.b - displaycolorimetry_709.primaries.b) < 1e-6f &&
        glm::length(input.white - display.white) < 1e-6f,
        "SDR source gamut is Rec.709 at the native D65 white inside BT.2020 container");
    tonemapping_t tone{};
    tone.bNormalizeShaper = true;
    tone.g22_luminance = settings.flSDROnHDRBrightness;
    lut1d_t shaper;
    lut3d_t lut;
    calcColorTransform<rendervulkan::s_nLutEdgeSize3d>(&shaper, rendervulkan::s_nLutSize1d, &lut,
        input, EOTF_Gamma22, output, outputTF, display, glm::vec2(0.f),
        k_EChromaticAdapatationMethod_Bradford, mapping, nightmode_t{}, tone, nullptr, 1.f);
    const auto white = encode(shaper, lut, glm::vec3(1.f));
    std::printf("native SDR white PQ=(%.7f, %.7f, %.7f), expected %.7f, shaper end %.7f\n",
        white.r, white.g, white.b, pq(203), shaper.dataR.back());
    require(code_error(encode(shaper, lut, glm::vec3(1.f)), glm::vec3(pq(203))) < 0.5,
        "SDR white must become 203-nit PQ");
    require(code_error(encode(shaper, lut, glm::vec3(0.f)), glm::vec3(pq(0))) < 0.5,
        "SDR black must remain black");
    double grayMax = 0;
    for (int i = 0; i <= 4096; ++i) {
        const double value = i / 4096.0;
        grayMax = std::max(grayMax, code_error(encode(shaper, lut, glm::vec3(value)),
            glm::vec3(pq(std::pow(value, 2.2) * 203.0))));
    }
    require(grayMax < 0.5, "native SDR gray ramp PQ accuracy");
    const glm::vec3 sdrPrimaries[] = {{1,0,0}, {0,1,0}, {0,0,1}};
    const glm::vec3 nits2020[] = {
        {0.6274039f*203, 0.0690973f*203, 0.0163914f*203},
        {0.3292830f*203, 0.9195404f*203, 0.0880132f*203},
        {0.0433131f*203, 0.0113624f*203, 0.8955954f*203},
    };
    for (int i = 0; i < 3; ++i) {
        const auto n = nits2020[i];
        require(code_error(encode(shaper, lut, sdrPrimaries[i]),
            glm::vec3(pq(n.r), pq(n.g), pq(n.b))) < 0.5,
            "native LUT must convert Rec.709 primaries into BT.2020");
    }

    buildPQColorimetry(&input, &mapping, display);
    tone.g22_luminance = 1.f;
    calcColorTransform<rendervulkan::s_nLutEdgeSize3d>(&shaper, rendervulkan::s_nLutSize1d, &lut,
        input, EOTF_PQ, output, outputTF, display, glm::vec2(0.f),
        k_EChromaticAdapatationMethod_Bradford, mapping, nightmode_t{}, tone, nullptr, 1.f);
    double hdrMax = 0;
    for (double nits : {0., 0.001, 0.005, 1., 80., 203., 1000.}) {
        const float encoded = pq(nits);
        hdrMax = std::max(hdrMax, code_error(encode(shaper, lut, glm::vec3(encoded)), glm::vec3(encoded)));
        for (int c = 0; c < 3; ++c) {
            glm::vec3 primary(0.f); primary[c] = encoded;
            hdrMax = std::max(hdrMax, code_error(encode(shaper, lut, primary), primary));
        }
    }
    std::printf("native HDR passthrough worst error %.4f codes\n", hdrMax);
    require(hdrMax < 0.5, "native BT.2020/PQ HDR must preserve near-black and highlights");
    std::printf("PASS: environment/format policy, native SDR→HDR→SDR→HDR, Rec.709→BT.2020 primaries; "
        "native LUT max gray %.4f / HDR %.4f codes (CPU, before GPU quantization)\n", grayMax, hdrMax);
}
