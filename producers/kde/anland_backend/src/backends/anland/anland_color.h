/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "core/colorspace.h"
#include <QRegion>
#include <memory>

namespace KWin
{

class GLFramebuffer;
class GLTexture;
class GLShader;

#ifdef ANLAND_KWIN_66
using AnlandColorDescription = std::shared_ptr<ColorDescription>;
#else
using AnlandColorDescription = ColorDescription;
#endif

// Own this with the layer, and create/destroy its GL resources with that layer's
// context current. The fixed session uses BT.2020 gamma22 at 1000 nits and PQ.
class AnlandHdrEncoder
{
public:
    AnlandHdrEncoder();
    ~AnlandHdrEncoder();

    bool init();
    // Physical buffer pixels, with OpenGL's bottom-left origin. Empty is a no-op.
    bool encode(GLTexture *sceneTexture, GLFramebuffer *fbo, const QRegion &bufferDamage);

private:
    std::unique_ptr<GLShader> m_shader;
    uint32_t m_lut = 0;
};

bool anlandFramebufferHasColorPrecision(GLFramebuffer *fbo, int minimumBits, bool floatingPoint);
bool anlandHdrShaderAvailable();
bool anlandEncodeHdrFrame(GLTexture *sceneTexture, GLFramebuffer *fbo,
                          const AnlandColorDescription &renderColor,
                          const AnlandColorDescription &wireColor);

} // namespace KWin
