/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "anland_color.h"
#include "anland_hdr_gain.h"
#include "opengl/glutils.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace KWin
{

void anlandApplyHdrContentGain(GLShader *shader, const AnlandColorDescription &source,
                               const AnlandColorDescription &target, RenderingIntent intent)
{
    const double gain = anlandHdrContentGainForSurface(source, target);
    if (gain == 1.0) {
        return; // Keep all native uniforms and the exact original 1x path.
    }
    const auto &src = anlandColorDescriptionValue(source);
    const auto &dst = anlandColorDescriptionValue(target);
    QMatrix4x4 multiplier;
    multiplier.scale(gain);
    shader->setUniform(GLShader::Mat4Uniform::ColorimetryTransformation, multiplier * src.toOther(dst, intent));

    // Gain belongs before tone mapping. Retune its source peak too, otherwise
    // the native source clamp can discard a gain above 1. Unknown HDR mastering
    // peaks use our existing output target, not the SDR reference white.
    const double destinationPeak = std::min<double>(dst.maxHdrLuminance().value_or(ANLAND_HDR_PEAK_NITS), ANLAND_HDR_PEAK_NITS);
    const double referenceScale = dst.referenceLuminance() / src.referenceLuminance();
    const double sourcePeak = src.maxHdrLuminance().value_or(destinationPeak / referenceScale) * referenceScale * gain;
    const bool tonemap = intent == RenderingIntent::Perceptual
        && qEnvironmentVariableIntValue("KWIN_DISABLE_TONEMAPPING") != 1;
    // Equal peaks select native clipping instead of the tone mapper. In both
    // cases the adjusted surface remains bounded by the unchanged output peak.
    shader->setUniform(GLShader::FloatUniform::MaxTonemappingLuminance,
        static_cast<float>(tonemap ? sourcePeak : destinationPeak));
    shader->setUniform(GLShader::FloatUniform::MaxDestinationLuminance, static_cast<float>(destinationPeak));
}

AnlandHdrEncoder::AnlandHdrEncoder() = default;

AnlandHdrEncoder::~AnlandHdrEncoder()
{
    m_shader.reset();
    if (m_lut) {
        glDeleteTextures(1, &m_lut);
    }
}

bool AnlandHdrEncoder::init()
{
    if (m_shader && m_lut) {
        return true;
    }
    // Mapping is fixed for the session. 2048 R16F entries take 4 KiB and keep
    // interpolation error below half a 10-bit code, including near black.
    // Sample texel centers so x=0 and x=1 address the actual endpoints.
    constexpr int size = 2048;
    std::array<float, size> values;
    for (int i = 0; i < size; ++i) {
        const double gamma = static_cast<double>(i) / (size - 1);
        const double normalizedNits = std::pow(gamma, 2.2) * 0.1;
        const double p = std::pow(normalizedNits, 2610.0 / 16384.0);
        values[i] = std::pow((3424.0 / 4096.0 + 2413.0 / 128.0 * p) / (1.0 + 2392.0 / 128.0 * p), 2523.0 / 32.0);
    }
    GLint activeTexture = 0, previousTexture = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
    glActiveTexture(GL_TEXTURE1);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
    glGenTextures(1, &m_lut);
    glBindTexture(GL_TEXTURE_2D, m_lut);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R16F, size, 1, 0, GL_RED, GL_FLOAT, values.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    const bool allocated = m_lut && glGetError() == GL_NO_ERROR;
    glBindTexture(GL_TEXTURE_2D, previousTexture);
    glActiveTexture(activeTexture);
    if (!allocated) {
        if (m_lut) {
            glDeleteTextures(1, &m_lut);
            m_lut = 0;
        }
        return false;
    }
    m_shader = ShaderManager::instance()->generateCustomShader(ShaderTrait::MapTexture, QByteArray(), QByteArrayLiteral(R"(
#version 140
uniform sampler2D sampler;
uniform sampler2D pqLut;
in vec2 texcoord0;
out vec4 fragColor;

void main()
{
    vec4 color = texture(sampler, texcoord0);
    if (color.a == 1.0) {
        vec3 coord = clamp(color.rgb, 0.0, 1.0) * (2047.0 / 2048.0) + vec3(0.5 / 2048.0);
        fragColor = vec4(texture(pqLut, vec2(coord.r, 0.5)).r,
                         texture(pqLut, vec2(coord.g, 0.5)).r,
                         texture(pqLut, vec2(coord.b, 0.5)).r, color.a);
    } else {
        // Match KWin's generic premultiplied-alpha handling exactly. The
        // 1000-nit clip happens before destination unpremultiplication.
        float alphaDivisor = max(color.a, 0.001);
        vec3 nits = pow(max(color.rgb / alphaDivisor, vec3(0.0)), vec3(2.2)) * (1000.0 * color.a);
        vec3 linear = clamp(nits, 0.0, 1000.0) / (10000.0 * alphaDivisor);
        vec3 p = pow(clamp(linear, 0.0, 1.0), vec3(0.1593017578125));
        vec3 pq = pow((vec3(0.8359375) + 18.8515625 * p) / (vec3(1.0) + 18.6875 * p), vec3(78.84375));
        fragColor = vec4(pq * color.a, color.a);
    }
}
)"));
#ifdef ANLAND_KWIN_67
    const bool compiled = m_shader != nullptr;
#else
    const bool compiled = m_shader && m_shader->isValid();
#endif
    if (!compiled) {
        m_shader.reset();
        glDeleteTextures(1, &m_lut);
        m_lut = 0;
    } else {
        ShaderBinder shader(m_shader.get());
        shader.shader()->setUniform("sampler", 0);
        shader.shader()->setUniform("pqLut", 1);
    }
    return compiled;
}

bool AnlandHdrEncoder::encode(GLTexture *sceneTexture, GLFramebuffer *fbo, const QRegion &bufferDamage)
{
    if (bufferDamage.isEmpty()) {
        return true;
    }
    if (!m_shader || !m_lut || !sceneTexture || !fbo) {
        return false;
    }
    const QRegion clippedDamage = bufferDamage.intersected(QRect(QPoint(), fbo->size()));
    if (clippedDamage.isEmpty()) {
        return true;
    }
    const GLboolean blend = glIsEnabled(GL_BLEND);
    const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    GLint scissorBox[4];
    glGetIntegerv(GL_SCISSOR_BOX, scissorBox);
    GLboolean colorMask[4];
    glGetBooleanv(GL_COLOR_WRITEMASK, colorMask);
    GLint activeTexture = 0, previousTexture[2];
    glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
    for (int unit = 0; unit < 2; ++unit) {
        glActiveTexture(GL_TEXTURE0 + unit);
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture[unit]);
    }
    glDisable(GL_BLEND);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBindTexture(GL_TEXTURE_2D, m_lut);
    glActiveTexture(GL_TEXTURE0);
    GLFramebuffer::pushFramebuffer(fbo);
    {
        ShaderBinder shader(m_shader.get());
        QMatrix4x4 matrix;
        matrix.scale(1, -1);
        matrix.ortho(QRectF(QPointF(), fbo->size()));
        shader.shader()->setUniform(GLShader::Mat4Uniform::ModelViewProjectionMatrix, matrix);
        const auto sceneTransform = sceneTexture->contentTransform();
        sceneTexture->setContentTransform(OutputTransform::FlipY);
        sceneTexture->bind();
        if (clippedDamage == QRegion(QRect(QPoint(), fbo->size()))) {
            glDisable(GL_SCISSOR_TEST);
            sceneTexture->render(QSizeF(fbo->size()));
        } else {
            glEnable(GL_SCISSOR_TEST);
            for (const QRect &rect : clippedDamage) {
                glScissor(rect.x(), rect.y(), rect.width(), rect.height());
                sceneTexture->render(QSizeF(fbo->size()));
            }
        }
        sceneTexture->unbind();
        sceneTexture->setContentTransform(sceneTransform);
    }
    const bool encoded = glGetError() == GL_NO_ERROR;
    GLFramebuffer::popFramebuffer();
    for (int unit = 0; unit < 2; ++unit) {
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(GL_TEXTURE_2D, previousTexture[unit]);
    }
    glActiveTexture(activeTexture);
    glColorMask(colorMask[0], colorMask[1], colorMask[2], colorMask[3]);
    glScissor(scissorBox[0], scissorBox[1], scissorBox[2], scissorBox[3]);
    blend ? glEnable(GL_BLEND) : glDisable(GL_BLEND);
    scissor ? glEnable(GL_SCISSOR_TEST) : glDisable(GL_SCISSOR_TEST);
    return encoded;
}

bool anlandHdrShaderAvailable()
{
    auto *shader = ShaderManager::instance()->shader(ShaderTrait::MapTexture | ShaderTrait::TransformColorspace);
    // 6.7 removed GLShader::isValid() and returns null on compile/link failure.
    // Older ShaderManager versions can retain an invalid shader object.
#ifdef ANLAND_KWIN_67
    return shader != nullptr;
#else
    return shader && shader->isValid();
#endif
}

bool anlandFramebufferHasColorPrecision(GLFramebuffer *fbo, int minimumBits, bool floatingPoint)
{
    GLFramebuffer::pushFramebuffer(fbo);
    GLint red = 0, green = 0, blue = 0, component = 0;
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_RED_SIZE, &red);
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_GREEN_SIZE, &green);
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_BLUE_SIZE, &blue);
    glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE, &component);
    const GLenum error = glGetError();
    GLFramebuffer::popFramebuffer();
    return error == GL_NO_ERROR && red >= minimumBits && green >= minimumBits && blue >= minimumBits
        && (!floatingPoint || component == GL_FLOAT);
}

bool anlandEncodeHdrFrame(GLTexture *sceneTexture, GLFramebuffer *fbo,
                          const AnlandColorDescription &renderColor,
                          const AnlandColorDescription &wireColor)
{
    if (!anlandHdrShaderAvailable()) {
        return false;
    }
    // Preserve scene GL state. In particular its scissor may cover only a small
    // damaged area, whereas every newly selected consumer slot needs a complete
    // copy of our single persistent scene image.
    const GLboolean blend = glIsEnabled(GL_BLEND);
    const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    GLboolean colorMask[4];
    glGetBooleanv(GL_COLOR_WRITEMASK, colorMask);
    GLint activeTexture = 0, previousTexture = 0;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &activeTexture);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previousTexture);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    GLFramebuffer::pushFramebuffer(fbo);
    bool encoded = false;
    {
        ShaderBinder shader(ShaderTrait::MapTexture | ShaderTrait::TransformColorspace);
        if (shader.shader()) {
#ifdef ANLAND_KWIN_66
            constexpr auto intent = RenderingIntent::AbsoluteColorimetricNoAdaptation;
#else
            constexpr auto intent = RenderingIntent::AbsoluteColorimetric;
#endif
            shader.shader()->setColorspaceUniforms(renderColor, wireColor, intent);
            shader.shader()->setUniform(GLShader::IntUniform::Sampler, 0);
            QMatrix4x4 matrix;
            matrix.scale(1, -1);
            matrix.ortho(QRectF(QPointF(), fbo->size()));
            shader.shader()->setUniform(GLShader::Mat4Uniform::ModelViewProjectionMatrix, matrix);

            // Scene rendering already applied the consumer's rotation/FlipY to
            // the FP16 pixels. GLTexture::render() adds its own Y flip and its
            // content transform to sampling coordinates. Temporarily FlipY
            // cancels that built-in flip, giving a raw pixel-for-pixel copy.
            const auto sceneTransform = sceneTexture->contentTransform();
            sceneTexture->setContentTransform(OutputTransform::FlipY);
            sceneTexture->bind();
            sceneTexture->render(QSizeF(fbo->size()));
            sceneTexture->unbind();
            sceneTexture->setContentTransform(sceneTransform);
            encoded = glGetError() == GL_NO_ERROR;
        }
    }
    GLFramebuffer::popFramebuffer();
    glBindTexture(GL_TEXTURE_2D, previousTexture);
    glActiveTexture(activeTexture);
    glColorMask(colorMask[0], colorMask[1], colorMask[2], colorMask[3]);
    if (blend) {
        glEnable(GL_BLEND);
    }
    if (scissor) {
        glEnable(GL_SCISSOR_TEST);
    }
    return encoded;
}

} // namespace KWin
