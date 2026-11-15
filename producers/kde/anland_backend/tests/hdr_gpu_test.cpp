// SPDX-License-Identifier: GPL-2.0-or-later
// Offscreen integration test against the installed KWin 6.8 library and
// production helpers.
#include "anland_color.h"
#include "anland_damage.h"
#include "anland_format.h"
#include "core/rendertarget.h"
#include "core/renderviewport.h"
#include <QCoreApplication>
#include <config-kwin.h>
#include <opengl/eglcontext.h>
#include <opengl/egldisplay.h>
#include <opengl/glutils.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <memory>
#include <vector>

static_assert(PROJECT_VERSION_MAJOR == 6 && PROJECT_VERSION_MINOR == 8,
    "This integration test targets installed KWin 6.8 headers and library.");

using namespace KWin;
static int failures = 0;
static void check(bool result, const char* message)
{
    std::printf("%s %s\n", result ? "PASS" : "FAIL", message);
    failures += !result;
}
static double pq(double nits)
{
    constexpr double m1 = 2610.0 / 16384, m2 = 2523.0 / 32;
    constexpr double c1 = 3424.0 / 4096, c2 = 2413.0 / 128, c3 = 2392.0 / 128;
    double l = std::pow(nits / 10000.0, m1);
    return std::pow((c1 + c2 * l) / (1 + c3 * l), m2);
}
struct Texture {
    GLuint id = 0;
    std::unique_ptr<GLTexture> texture;
    std::unique_ptr<GLFramebuffer> fbo;
    Texture(GLenum format, QSize size, const void* pixels = nullptr)
    {
        glGenTextures(1, &id);
        glBindTexture(GL_TEXTURE_2D, id);
        glTexImage2D(GL_TEXTURE_2D, 0, format, size.width(), size.height(), 0, GL_RGBA,
            (format == GL_RGBA16F || format == GL_RGBA32F)
                ? GL_FLOAT
                : (format == GL_RGB10_A2 ? GL_UNSIGNED_INT_2_10_10_10_REV : GL_UNSIGNED_BYTE),
            pixels);
        texture = GLTexture::createNonOwningWrapper(id, format, size);
        texture->setFilter(GL_NEAREST);
        texture->setWrapMode(GL_CLAMP_TO_EDGE);
        std::printf("allocate format=0x%x error=0x%x\n", format, glGetError());
        fbo = std::make_unique<GLFramebuffer>(texture.get());
        std::printf("FBO format=0x%x valid=%d error=0x%x\n", format, fbo->valid(), glGetError());
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    ~Texture()
    {
        fbo.reset();
        texture.reset();
        if (id) {
            glDeleteTextures(1, &id);
        }
    }
};

static void clear(Texture& texture, float red, float green, float blue)
{
    GLFramebuffer::pushFramebuffer(texture.fbo.get());
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(red, green, blue, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    GLFramebuffer::popFramebuffer();
}

static std::vector<float> readFloat(Texture& texture)
{
    GLFramebuffer::pushFramebuffer(texture.fbo.get());
    std::vector<float> result(texture.texture->width() * texture.texture->height() * 4);
    glReadPixels(0, 0, texture.texture->width(), texture.texture->height(), GL_RGBA, GL_FLOAT, result.data());
    GLFramebuffer::popFramebuffer();
    return result;
}

static void testLutPrecision()
{
    // Exhaust all finite positive/negative half values, not just uniform samples.
    const QSize size(256, 256);
    std::vector<float> input(65536 * 4);
    std::vector<double> expected(65536);
    for (unsigned bits = 0; bits < 65536; ++bits) {
        // Decode binary16 without a compiler-specific half-float type.
        const int sign = bits & 0x8000 ? -1 : 1;
        const unsigned exponent = (bits >> 10) & 31;
        const unsigned mantissa = bits & 1023;
        double value = exponent == 0 ? std::ldexp(double(mantissa), -24)
                                     : std::ldexp(1.0 + double(mantissa) / 1024, int(exponent) - 15);
        value *= sign;
        if (exponent == 31) {
            value = sign * 2.0; // NaN/Inf are not meaningful scene pixels.
        }
        for (int c = 0; c < 3; ++c) {
            input[bits * 4 + c] = value;
        }
        input[bits * 4 + 3] = 1;
        expected[bits] = pq(std::pow(std::clamp(value, 0.0, 1.0), 2.2) * 1000);
    }
    Texture scene(GL_RGBA16F, size, input.data()), output(GL_RGBA32F, size);
    AnlandHdrEncoder encoder;
    check(output.fbo->valid() && encoder.init(), "FP32 precision-test target and LUT encoder initialized");
    check(encoder.encode(scene.texture.get(), output.fbo.get(), QRegion(QRect(QPoint(), size))),
        "LUT finite-half sweep encoded");
    const auto pixels = readFloat(output);
    double worst = 0;
    for (size_t i = 0; i < expected.size(); ++i) {
        worst = std::max(worst, std::abs(double(pixels[i * 4]) - expected[i]) * 1023);
    }
    char label[160];
    std::snprintf(
        label, sizeof(label), "all finite half inputs, including negative and >1: max LUT error %.7f code <0.5", worst);
    check(glGetError() == GL_NO_ERROR && worst < 0.5, label);
    Texture quantized(GL_RGB10_A2, size);
    check(encoder.encode(scene.texture.get(), quantized.fbo.get(), QRegion(QRect(QPoint(), size))),
        "LUT finite-half sweep encoded to actual 10-bit output");
    GLFramebuffer::pushFramebuffer(quantized.fbo.get());
    std::vector<uint32_t> packed(expected.size());
    glReadPixels(0, 0, size.width(), size.height(), GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV, packed.data());
    GLFramebuffer::popFramebuffer();
    double quantizedWorst = 0;
    for (size_t i = 0; i < expected.size(); ++i) {
        quantizedWorst = std::max(quantizedWorst, std::abs(double(packed[i] & 1023) - expected[i] * 1023));
    }
    std::snprintf(
        label, sizeof(label), "all finite half inputs: final 10-bit output max error %.7f code <1", quantizedWorst);
    check(glGetError() == GL_NO_ERROR && quantizedWorst < 1, label);
}

static void testPremultipliedAlpha()
{
    const QSize size(64, 32);
    std::vector<float> input(size.width() * size.height() * 4);
    const std::array<float, 8> alpha = { 0, 0.0001, 0.0009, 0.001, 0.01, 0.1, 0.5, 0.999 };
    for (int i = 0; i < size.width() * size.height(); ++i) {
        input[i * 4 + 3] = alpha[i % alpha.size()];
        input[i * 4] = (i % 16) / 8.0; // Also exercises clipped, noncanonical premultiplied values.
        input[i * 4 + 1] = -0.1;
        input[i * 4 + 2] = (i % 13) / 13.0 * input[i * 4 + 3];
    }
    Texture scene(GL_RGBA16F, size, input.data()), reference(GL_RGBA32F, size), actual(GL_RGBA32F, size);
    auto renderColor = std::make_shared<ColorDescription>(
        Colorimetry::BT2020, TransferFunction(TransferFunction::gamma22, 0, 1000), 203, 0, 1000, 1000);
    auto wireColor = std::make_shared<ColorDescription>(
        Colorimetry::BT2020, TransferFunction(TransferFunction::PerceptualQuantizer, 0, 10000), 203, 0, 1000, 1000);
    AnlandHdrEncoder encoder;
    check(encoder.init() && anlandEncodeHdrFrame(scene.texture.get(), reference.fbo.get(), renderColor, wireColor)
            && encoder.encode(scene.texture.get(), actual.fbo.get(), QRegion(QRect(QPoint(), size))),
        "premultiplied-alpha reference and optimized paths encoded");
    const auto ref = readFloat(reference);
    const auto result = readFloat(actual);
    const auto roundedInput = readFloat(scene);
    double genericDifference = 0;
    double mathematicalError = 0;
    for (size_t i = 0; i < result.size(); ++i) {
        genericDifference = std::max(genericDifference, std::abs(double(result[i]) - ref[i]) * 1023);
        const double alphaValue = roundedInput[i / 4 * 4 + 3];
        double expected = alphaValue;
        if (i % 4 != 3) {
            const double divisor = std::max(alphaValue, 0.001);
            const double nits = std::pow(std::max(double(roundedInput[i]) / divisor, 0.0), 2.2) * 1000 * alphaValue;
            expected = pq(std::min(std::clamp(nits, 0.0, 1000.0) / divisor, 10000.0)) * alphaValue;
        }
        mathematicalError = std::max(mathematicalError, std::abs(double(result[i]) - expected) * 1023);
    }
    char label[200];
    std::snprintf(label, sizeof(label),
        "alpha 0..0.999 and clipping: CPU-rule error %.7f code, generic difference %.7f code", mathematicalError,
        genericDifference);
    // Generic BT.2020->BT.2020 matrix arithmetic leaves small off-diagonal
    // roundoff that PQ magnifies near zero. Check the exact scalar rule too.
    check(glGetError() == GL_NO_ERROR && mathematicalError < 0.05 && genericDifference < 0.5, label);
}

static void testDamageMapping()
{
    const QSize size(8, 4);
    const float encodedWhite = std::pow(203.0 / 1000, 1.0 / 2.2);
    const std::array<float, 4> white = { encodedWhite, encodedWhite, encodedWhite, 1 };
    Texture scene(GL_RGBA16F, size), output(GL_RGB10_A2, size), solid(GL_RGBA16F, QSize(1, 1), white.data());
    solid.texture->setContentTransform(OutputTransform::FlipY);
    auto renderColor = std::make_shared<ColorDescription>(
        Colorimetry::BT2020, TransferFunction(TransferFunction::gamma22, 0, 1000), 203, 0, 1000, 1000);
    AnlandHdrEncoder encoder;
    check(encoder.init(), "damage-test LUT encoder initialized");
    for (int tr = 0; tr < 8; ++tr) {
        const OutputTransform transform(static_cast<OutputTransform::Kind>(tr));
        scene.texture->setContentTransform(transform);
        clear(scene, 0, 0, 0);
        clear(output, 1, 0, 1);
        const QSize deviceSize = transform.map(size);
        const QRegion deviceDamage = QRegion(QRect(1, 1, 2, 1)) | QRect(0, deviceSize.height() - 1, 1, 1);
        // Draw actual scene pixels using KWin's real projection. This is an
        // independent check of the damage mapper, not an inverse of its formula.
        RenderTarget target(scene.fbo.get(), renderColor);
        RenderViewport viewport(RectF(QPoint(), deviceSize), 1, target, QPoint());
        GLFramebuffer::pushFramebuffer(scene.fbo.get());
        glDisable(GL_BLEND);
        {
            ShaderBinder shader(ShaderTrait::MapTexture);
            shader.shader()->setUniform(GLShader::IntUniform::Sampler, 0);
            solid.texture->bind();
            for (const QRect& rect : deviceDamage) {
                QMatrix4x4 matrix = viewport.projectionMatrix();
                matrix.translate(rect.x(), rect.y());
                shader.shader()->setUniform(GLShader::Mat4Uniform::ModelViewProjectionMatrix, matrix);
                solid.texture->render(QSizeF(rect.size()));
            }
            solid.texture->unbind();
        }
        GLFramebuffer::popFramebuffer();
        const auto scenePixels = readFloat(scene);
        const QRegion bufferDamage = anlandHdrBufferDamage(deviceDamage, size, transform);
        bool mappingMatchesPixels = true;
        for (int y = 0; y < size.height(); ++y) {
            for (int x = 0; x < size.width(); ++x) {
                const bool painted = scenePixels[(x + y * size.width()) * 4] > 0.1;
                mappingMatchesPixels &= painted == bufferDamage.contains(QPoint(x, y));
            }
        }
        glEnable(GL_SCISSOR_TEST);
        glScissor(1, 2, 3, 1);
        const bool encoded = encoder.encode(scene.texture.get(), output.fbo.get(), bufferDamage);
        GLint box[4];
        glGetIntegerv(GL_SCISSOR_BOX, box);
        const bool restored = glIsEnabled(GL_SCISSOR_TEST) && box[0] == 1 && box[1] == 2 && box[2] == 3 && box[3] == 1;
        glDisable(GL_SCISSOR_TEST);
        GLFramebuffer::pushFramebuffer(output.fbo.get());
        std::vector<uint32_t> pixels(size.width() * size.height());
        glReadPixels(0, 0, size.width(), size.height(), GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV, pixels.data());
        GLFramebuffer::popFramebuffer();
        bool correct = true;
        for (int y = 0; y < size.height(); ++y) {
            for (int x = 0; x < size.width(); ++x) {
                const uint32_t pixel = pixels[x + y * size.width()];
                if (bufferDamage.contains(QPoint(x, y))) {
                    for (int c = 0; c < 3; ++c) {
                        correct &= std::abs(double((pixel >> (c * 10)) & 1023) / 1023 - pq(203)) < 1.2 / 1023;
                    }
                } else {
                    correct &= pixel == 0xfff003ff; // Untouched magenta poison.
                }
            }
        }
        char label[160];
        std::snprintf(label, sizeof(label),
            "transform %d real RenderViewport paint matches partial damage, untouched pixels and scissor state", tr);
        check(encoded && mappingMatchesPixels && restored && correct && glGetError() == GL_NO_ERROR, label);
    }
}

int main(int argc, char** argv)
{
    QCoreApplication application(argc, argv);
    auto raw = eglGetPlatformDisplayEXT(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, nullptr);
    auto display = EglDisplay::create(raw, nullptr);
    check(bool(display), "KWin EglDisplay created");
    if (!display) {
        return 2;
    }
    auto context = EglContext::create(display.get(), nullptr, nullptr);
    const bool current = context && context->makeCurrent();
    check(current, "KWin EglContext current");
    if (!current) {
        return 2;
    }
    std::printf("KWin: %s\nRenderer: %s\nGL: %s\n", KWIN_PLUGIN_VERSION_STRING, glGetString(GL_RENDERER),
        glGetString(GL_VERSION));
    std::printf("PQ(0.001)=%.9f PQ(0.005)=%.9f PQ(203)=%.9f PQ(1000)=%.9f\n", pq(.001), pq(.005), pq(203), pq(1000));
    check(anlandProtocolFormatToDrm(1) == DRM_FORMAT_ABGR8888 && anlandProtocolFormatToDrm(2) == DRM_FORMAT_XBGR8888
            && anlandProtocolFormatToDrm(5) == DRM_FORMAT_ARGB8888
            && anlandProtocolFormatToDrm(43) == DRM_FORMAT_ABGR2101010
            && anlandProtocolFormatToDrm(999) == DRM_FORMAT_INVALID,
        "storage mappings include HAL43 and reject unknown formats");
    check(anlandProtocolFormatSupportsMode(43, true) && !anlandProtocolFormatSupportsMode(43, false)
            && !anlandProtocolFormatSupportsMode(1, true) && !anlandProtocolFormatSupportsMode(2, true)
            && !anlandProtocolFormatSupportsMode(5, true) && anlandProtocolFormatSupportsMode(1, false)
            && anlandProtocolFormatSupportsMode(2, false) && anlandProtocolFormatSupportsMode(5, false)
            && !anlandProtocolFormatSupportsMode(999, true) && !anlandProtocolFormatSupportsMode(999, false),
        "both HDR/8bit and SDR/10bit session mismatches are rejected");
    {
        const QSize size(8, 4);
        std::vector<float> data(size.width() * size.height() * 4);
        std::vector<double> expected(data.size());
        // Eight asymmetric patches exercise channel order, absolute luminance, and
        // near-black preservation. Data and readback use OpenGL pixel order, so the
        // expected final copy is transform-independent.
        const std::array<std::array<double, 3>, 8> patches
            = { { { 0, 0, 0 }, { 203, 203, 203 }, { 1000, 0, 50 }, { 0, 1000, 1000 }, { 0.001, 0.001, 0.001 },
                { 0.005, 0.005, 0.005 }, { 0.001, 0.005, 0 }, { 1000, 1000, 1000 } } };
        for (int y = 0; y < size.height(); ++y) {
            for (int x = 0; x < size.width(); ++x) {
                int patch = x / 2 + 4 * (y / 2);
                int offset = 4 * (x + y * size.width());
                for (int c = 0; c < 3; ++c) {
                    data[offset + c] = std::pow(patches[patch][c] / 1000., 1. / 2.2);
                    expected[offset + c] = pq(patches[patch][c]);
                }
                data[offset + 3] = 1;
                expected[offset + 3] = 1;
            }
        }
        Texture scene(GL_RGBA16F, size, data.data()), out(GL_RGB10_A2, size), eight(GL_RGBA8, QSize(3, 5));
        check(scene.fbo->valid() && out.fbo->valid() && eight.fbo->valid(), "FP16, RGB10_A2 and RGBA8 FBOs complete");
        check(anlandFramebufferHasColorPrecision(scene.fbo.get(), 16, true), "actual FP16 framebuffer accepted");
        check(anlandFramebufferHasColorPrecision(out.fbo.get(), 10, false), "actual RGB10_A2 framebuffer accepted");
        check(!anlandFramebufferHasColorPrecision(eight.fbo.get(), 10, false),
            "actual RGBA8 framebuffer rejected for 10bit");
        check(!anlandFramebufferHasColorPrecision(out.fbo.get(), 16, true), "RGB10_A2 framebuffer rejected for FP16");
        auto renderColor = std::make_shared<ColorDescription>(
            Colorimetry::BT2020, TransferFunction(TransferFunction::gamma22, 0, 1000), 203, 0, 1000, 1000);
        auto wireColor = std::make_shared<ColorDescription>(
            Colorimetry::BT2020, TransferFunction(TransferFunction::PerceptualQuantizer, 0, 10000), 203, 0, 1000, 1000);
        check(anlandHdrShaderAvailable(), "real KWin color shader compiled successfully");
        AnlandHdrEncoder encoder;
        check(encoder.init(), "fixed LUT encoder initializes");
        check(encoder.encode(nullptr, nullptr, QRegion()), "empty damage performs no GL work");
        GLuint dummy[3];
        glGenTextures(3, dummy);
        ShaderBinder existingShader(ShaderTrait::MapTexture);
        for (int tr = 0; tr < 8; ++tr) {
            for (int flags = 0; flags < 4; ++flags) {
                auto transform = OutputTransform(static_cast<OutputTransform::Kind>(tr));
                scene.texture->setContentTransform(transform);
                // Poison the destination every time: a skipped or scissored copy must
                // fail.
                GLFramebuffer::pushFramebuffer(out.fbo.get());
                glDisable(GL_SCISSOR_TEST);
                glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                glClearColor(1, 0, 1, 1);
                glClear(GL_COLOR_BUFFER_BIT);
                GLFramebuffer::popFramebuffer();
                GLFramebuffer::pushFramebuffer(eight.fbo.get());
                flags & 1 ? glEnable(GL_BLEND) : glDisable(GL_BLEND);
                flags & 2 ? glEnable(GL_SCISSOR_TEST) : glDisable(GL_SCISSOR_TEST);
                glScissor(1, 1, 1, 1);
                glColorMask(GL_FALSE, GL_TRUE, GL_FALSE, GL_TRUE);
                glActiveTexture(GL_TEXTURE0);
                glBindTexture(GL_TEXTURE_2D, dummy[0]);
                glActiveTexture(GL_TEXTURE1);
                glBindTexture(GL_TEXTURE_2D, dummy[1]);
                glActiveTexture(GL_TEXTURE3);
                glBindTexture(GL_TEXTURE_2D, dummy[2]);
                while (glGetError() != GL_NO_ERROR) { }
                GLint previousProgram = 0;
                glGetIntegerv(GL_CURRENT_PROGRAM, &previousProgram);
                bool encoded = encoder.encode(scene.texture.get(), out.fbo.get(), QRegion(QRect(QPoint(), size)));
                GLint active = 0, bound0 = 0, bound1 = 0, bound3 = 0, fbo = 0, program = 0, viewport[4], scissor[4];
                GLboolean mask[4];
                glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
                glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound3);
                glActiveTexture(GL_TEXTURE0);
                glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound0);
                glActiveTexture(GL_TEXTURE1);
                glGetIntegerv(GL_TEXTURE_BINDING_2D, &bound1);
                glActiveTexture(active);
                glGetIntegerv(GL_CURRENT_PROGRAM, &program);
                glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fbo);
                glGetIntegerv(GL_VIEWPORT, viewport);
                glGetIntegerv(GL_SCISSOR_BOX, scissor);
                glGetBooleanv(GL_COLOR_WRITEMASK, mask);
                bool restored = program == previousProgram && active == GL_TEXTURE3 && GLuint(bound0) == dummy[0]
                    && GLuint(bound1) == dummy[1] && GLuint(bound3) == dummy[2]
                    && scene.texture->contentTransform() == transform
                    && GLFramebuffer::currentFramebuffer() == eight.fbo.get() && GLuint(fbo) == eight.fbo->handle()
                    && viewport[0] == 0 && viewport[1] == 0 && viewport[2] == 3 && viewport[3] == 5 && scissor[0] == 1
                    && scissor[1] == 1 && scissor[2] == 1 && scissor[3] == 1 && glIsEnabled(GL_BLEND) == bool(flags & 1)
                    && glIsEnabled(GL_SCISSOR_TEST) == bool(flags & 2) && !mask[0] && mask[1] && !mask[2] && mask[3];
                GLFramebuffer::popFramebuffer();
                glDisable(GL_BLEND);
                glDisable(GL_SCISSOR_TEST);
                glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                GLFramebuffer::pushFramebuffer(out.fbo.get());
                std::vector<uint32_t> packed(size.width() * size.height());
                glReadPixels(0, 0, size.width(), size.height(), GL_RGBA, GL_UNSIGNED_INT_2_10_10_10_REV, packed.data());
                GLenum readError = glGetError();
                GLFramebuffer::popFramebuffer();
                double maximumError = 0;
                int badPixels = 0;
                for (size_t i = 0; i < packed.size(); ++i) {
                    for (int c = 0; c < 3; ++c) {
                        double decoded = ((packed[i] >> (10 * c)) & 1023) / 1023.;
                        double error = std::abs(decoded - expected[4 * i + c]);
                        maximumError = std::max(maximumError, error);
                        badPixels += error > 1.2 / 1023;
                    }
                }
                for (const uint32_t pixel : packed) {
                    badPixels += (pixel >> 30) != 3;
                }
                char label[240];
                std::snprintf(label, sizeof(label),
                    "transform=%d blend=%d scissor=%d encoded=%d GLstate=%d "
                    "readError=0x%x pixelErrors=%d maxPQError=%.7f",
                    tr, !!(flags & 1), !!(flags & 2), encoded, restored, readError, badPixels, maximumError);
                check(encoded && restored && readError == GL_NO_ERROR && badPixels == 0, label);
                if (tr == 0 && flags == 0) {
                    for (int y = 0; y < size.height(); y += 2) {
                        for (int x = 0; x < size.width(); x += 2) {
                            auto i = x + y * size.width();
                            std::printf("sample(%d,%d) PQ RGB=[%.8f, %.8f, %.8f]\n", x, y, (packed[i] & 1023) / 1023.,
                                ((packed[i] >> 10) & 1023) / 1023., ((packed[i] >> 20) & 1023) / 1023.);
                        }
                    }
                }
            }
        }
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE3);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE0);
        glDeleteTextures(3, dummy);
    }
    testLutPrecision();
    testPremultipliedAlpha();
    testDamageMapping();
    std::printf("RESULT %s failures=%d\n", failures ? "FAIL" : "PASS", failures);
    context->doneCurrent();
    return failures ? 1 : 0;
}
