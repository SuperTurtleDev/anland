// SPDX-License-Identifier: GPL-2.0-or-later
// Policy and state integration test against real production AnlandOutput.
#include "anland_backend.h"
#include "anland_egl_backend.h"
#include "anland_format.h"
#include "anland_output.h"

#include <QCoreApplication>
#include <array>
#include <config-kwin.h>
#include <cstdio>
#include <cstdlib>

static_assert(PROJECT_VERSION_MAJOR == 6 && PROJECT_VERSION_MINOR == 8,
    "This integration test targets installed KWin 6.8 headers and library.");

namespace KWin {
// No transport or GL layer is constructed in this test. Link the actual output
// implementation while making any accidental use of those paths fail loudly.
bool AnlandBackend::presentFrame(int) { std::abort(); }
int AnlandEglLayer::takePendingFence() { std::abort(); }
}

using namespace KWin;
static int failures = 0;
static void check(bool result, const char* message)
{
    std::printf("%s %s\n", result ? "PASS" : "FAIL", message);
    failures += !result;
}

class EnvironmentRestore {
public:
    EnvironmentRestore()
        : m_wasSet(qEnvironmentVariableIsSet("ANLAND_HDR"))
        , m_value(qgetenv("ANLAND_HDR"))
    {
    }
    ~EnvironmentRestore()
    {
        if (m_wasSet) {
            qputenv("ANLAND_HDR", m_value);
        } else {
            qunsetenv("ANLAND_HDR");
        }
    }

private:
    bool m_wasSet;
    QByteArray m_value;
};

static void testPolicy()
{
    struct EnvironmentCase {
        const char* label;
        const char* value; // nullptr represents an unset variable.
        bool disabled;
    };
    const EnvironmentCase cases[] = {
        { "unset", nullptr, false },
        { "empty", "", false },
        { "1", "1", false },
        { "auto", "auto", false },
        { "0", "0", true },
        { "false", "false", true },
        { "no", "no", true },
        { "off", "off", true },
        { "mixed-case false", "FaLsE", true },
        { "whitespace/case off", "\t OfF \r\n", true },
        { "whitespace/case no", "  nO  ", true },
        { "whitespace zero", " \t0\n", true },
        { "whitespace only", " \t\n", false },
        { "unknown", "unknown", false },
        { "unknown zero prefix", "0x0", false },
        { "true", "true", false },
        { "yes", "YES", false },
    };
    for (const auto& test : cases) {
        if (test.value) {
            qputenv("ANLAND_HDR", test.value);
        } else {
            qunsetenv("ANLAND_HDR");
        }
        AnlandOutput output(nullptr, QStringLiteral("anland-policy-test"));
        output.init(QSize(1920, 1080), 120000, 1);
        const bool policyMatches = anlandHdrExplicitlyDisabled(qgetenv("ANLAND_HDR")) == test.disabled
            && output.hdrAllowed() == !test.disabled;
        const bool startsSdr = !output.hdrEnabled() && !output.highDynamicRange() && !output.wideColorGamut()
            && output.renderColorDescription().get() == ColorDescription::sRGB.get()
            && output.wireColorDescription().get() == ColorDescription::sRGB.get()
            && output.colorDescription().get() == ColorDescription::sRGB.get();
        char label[160];
        std::snprintf(label, sizeof(label), "environment %s: actual constructor policy correct and output remains SDR",
            test.label);
        check(policyMatches && startsSdr, label);
    }

    struct FormatCase {
        uint32_t format;
        int hdrAllowed; // -1 rejected; 0 SDR; 1 HDR.
        int hdrForbidden;
    };
    const FormatCase formats[] = {
        { 0, -1, -1 },
        { 1, 0, 0 },
        { 2, 0, 0 },
        { 5, 0, 0 },
        { 43, 1, -1 },
        { 999, -1, -1 },
    };
    for (const auto& test : formats) {
        for (bool allowed : { false, true }) {
            const auto actual = anlandConsumerHdrMode(test.format, allowed);
            const int result = actual ? int(*actual) : -1;
            char label[128];
            std::snprintf(label, sizeof(label), "consumer format %u, HDR allowed=%d selects expected mode or rejection",
                test.format, allowed);
            check(result == (allowed ? test.hdrAllowed : test.hdrForbidden), label);
        }
    }
}

static bool outputHasMode(const AnlandOutput& output, bool hdr, unsigned int referenceWhite = 203)
{
    const auto& render = output.renderColorDescription();
    const auto& wire = output.wireColorDescription();
    const auto capabilities = output.capabilities();
    const bool hasHdr = capabilities.testFlag(BackendOutput::Capability::HighDynamicRange);
    const bool hasWcg = capabilities.testFlag(BackendOutput::Capability::WideColorGamut);
    const auto range = output.bitsPerColorRange();
    bool result = output.hdrEnabled() == hdr && output.highDynamicRange() == hdr && output.wideColorGamut() == hdr
        && hasHdr == hdr && hasWcg == hdr && range.min == (hdr ? 10u : 8u) && range.max == range.min
        && output.maxBitsPerColor() == range.max && output.referenceLuminance() == (hdr ? referenceWhite : 200u)
        && output.colorDescription().get() == wire.get() && output.layerBlendingColor().get() == wire.get()
        && output.blendingColor().get() == render.get();
    if (hdr) {
        result &= render->containerColorimetry() == Colorimetry::BT2020
            && wire->containerColorimetry() == Colorimetry::BT2020
            && render->transferFunction().type == TransferFunction::gamma22
            && render->transferFunction().minLuminance == 0 && render->transferFunction().maxLuminance == 1000
            && wire->transferFunction().type == TransferFunction::PerceptualQuantizer
            && wire->transferFunction().minLuminance == 0 && wire->transferFunction().maxLuminance == 10000
            && render->referenceLuminance() == referenceWhite && wire->referenceLuminance() == referenceWhite
            && render->maxAverageLuminance() == 1000 && render->maxHdrLuminance() == 1000
            && wire->maxAverageLuminance() == 1000 && wire->maxHdrLuminance() == 1000
            && output.advertisedMaxPeakBrightness() == 1000 && output.advertisedMaxAverageBrightness() == 1000;
    } else {
        result &= render.get() == ColorDescription::sRGB.get() && wire.get() == ColorDescription::sRGB.get()
            && !output.advertisedMaxPeakBrightness() && !output.advertisedMaxAverageBrightness();
    }
    return result;
}

static void testStateTransitions()
{
    qunsetenv("ANLAND_HDR");
    AnlandOutput output(nullptr, QStringLiteral("anland-mode-test"));
    output.init(QSize(1920, 1080), 120000, 1);
    output.updateEnabled(true);
    check(
        outputHasMode(output, false), "actual output starts with coherent SDR state, colors, metadata and 8-bit range");
    const auto originalMode = output.currentMode();
    const auto originalModes = output.modes();

    struct Signals {
        int capabilities = 0, hdr = 0, wcg = 0, reference = 0, color = 0, blend = 0, brightness = 0, bpc = 0;
        int modes = 0, currentMode = 0, position = 0, enabled = 0;
    } counts;
    QObject::connect(&output, &BackendOutput::capabilitiesChanged, [&] { ++counts.capabilities; });
    QObject::connect(&output, &BackendOutput::highDynamicRangeChanged, [&] { ++counts.hdr; });
    QObject::connect(&output, &BackendOutput::wideColorGamutChanged, [&] { ++counts.wcg; });
    QObject::connect(&output, &BackendOutput::referenceLuminanceChanged, [&] { ++counts.reference; });
    QObject::connect(&output, &BackendOutput::colorDescriptionChanged, [&] { ++counts.color; });
    QObject::connect(&output, &BackendOutput::blendingColorChanged, [&] { ++counts.blend; });
    QObject::connect(&output, &BackendOutput::brightnessMetadataChanged, [&] { ++counts.brightness; });
    QObject::connect(&output, &BackendOutput::maxBitsPerColorChanged, [&] { ++counts.bpc; });
    QObject::connect(&output, &BackendOutput::modesChanged, [&] { ++counts.modes; });
    QObject::connect(&output, &BackendOutput::currentModeChanged, [&] { ++counts.currentMode; });
    QObject::connect(&output, &BackendOutput::positionChanged, [&] { ++counts.position; });
    QObject::connect(&output, &BackendOutput::enabledChanged, [&] { ++counts.enabled; });

    int transitions = 0;
    for (const bool hdr : { false, true, true, false, false, true }) {
        const bool changed = hdr != output.hdrEnabled();
        transitions += changed;
        output.setConsumerHdrMode(hdr);
        const bool notifications = counts.capabilities == transitions && counts.hdr == transitions
            && counts.wcg == transitions && counts.reference == transitions && counts.color == transitions
            && counts.blend == transitions && counts.brightness == transitions && counts.bpc == transitions;
        const bool geometryStable = output.currentMode() == originalMode && output.modes() == originalModes
            && output.modeSize() == QSize(1920, 1080) && output.refreshRate() == 120000 && output.scale() == 1
            && output.isEnabled() && !counts.modes && !counts.currentMode && !counts.position && !counts.enabled;
        char label[160];
        std::snprintf(label, sizeof(label),
            "same-size consumer %s (%s): coherent actual state and exactly one set of notifications",
            hdr ? "HDR" : "SDR", changed ? "transition" : "no-op");
        check(outputHasMode(output, hdr) && notifications && geometryStable, label);
    }

    // Explicit enabling is permission only: an SDR app session still wins.
    qputenv("ANLAND_HDR", "1");
    AnlandOutput requested(nullptr, QStringLiteral("anland-explicit-one"));
    requested.init(QSize(1920, 1080), 120000, 1);
    const auto sdrConsumer = anlandConsumerHdrMode(1, requested.hdrAllowed());
    if (sdrConsumer) {
        requested.setConsumerHdrMode(*sdrConsumer);
    }
    check(
        sdrConsumer == false && outputHasMode(requested, false), "ANLAND_HDR=1 with app 8-bit buffers stays fully SDR");
}

static void testReferenceWhite()
{
    qunsetenv("ANLAND_HDR");
    AnlandOutput output(nullptr, QStringLiteral("anland-white-test"));
    output.init(QSize(1920, 1080), 120000, 1);
    check(output.setHdrSdrWhiteNits(400) && output.hdrSdrWhiteNits() == 400
            && outputHasMode(output, false),
        "brightness request in SDR records the value without changing SDR color, brightness or HDR state");
    // A freshly connected session supplies its default 203, replacing the old
    // consumer's value even if the output and buffer size are retained.
    output.setConsumerHdrMode(true, 203);
    check(outputHasMode(output, true) && output.hdrSdrWhiteNits() == 203,
        "new HDR session uses its own 203 nit default rather than a stale session preference");
    const auto originalMode = output.currentMode();
    int references = 0, colors = 0, blends = 0, hdr = 0, bpc = 0;
    QObject::connect(&output, &BackendOutput::referenceLuminanceChanged, [&] { ++references; });
    QObject::connect(&output, &BackendOutput::colorDescriptionChanged, [&] { ++colors; });
    QObject::connect(&output, &BackendOutput::blendingColorChanged, [&] { ++blends; });
    QObject::connect(&output, &BackendOutput::highDynamicRangeChanged, [&] { ++hdr; });
    QObject::connect(&output, &BackendOutput::maxBitsPerColorChanged, [&] { ++bpc; });
    int changes = 0;
    for (unsigned int white : {80u, 203u, 300u, 300u, 400u, 500u, 203u}) {
        changes += output.hdrSdrWhiteNits() != white;
        const bool accepted = output.setHdrSdrWhiteNits(white);
        char label[180];
        std::snprintf(label, sizeof(label),
            "SDR white %u nits: coherent render/wire reference, fixed 0/1000-nit scene, absolute PQ, one notification", white);
        check(accepted && outputHasMode(output, true, white) && output.hdrSdrWhiteNits() == white
                && output.currentMode() == originalMode && references == changes && colors == changes
                && blends == changes && hdr == 0 && bpc == 0, label);
    }
    for (unsigned int invalid : {0u, 79u, 501u, 0xffffffffu}) {
        check(!output.setHdrSdrWhiteNits(invalid) && outputHasMode(output, true)
                && references == changes && colors == changes && blends == changes,
            "out-of-range white is rejected without state or notification changes");
    }
    output.setConsumerHdrMode(true, 400);
    output.setConsumerHdrMode(true, 203);
    check(outputHasMode(output, true) && output.hdrSdrWhiteNits() == 203,
        "same-size HDR reconnect resets the old 400 nit reference to the new session's 203 nits");
    output.setConsumerHdrMode(false, 500);
    check(outputHasMode(output, false), "HDR-to-SDR import ignores the reference white in its SDR output state");
    check(output.setHdrSdrWhiteNits(80) && outputHasMode(output, false),
        "live action in SDR cannot enable HDR or lower SDR brightness");
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);
    EnvironmentRestore restore;
    std::printf("KWin: %s\n", KWIN_PLUGIN_VERSION_STRING);
    testPolicy();
    testStateTransitions();
    testReferenceWhite();
    std::printf("RESULT %s failures=%d\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
