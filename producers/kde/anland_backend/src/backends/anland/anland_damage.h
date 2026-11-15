/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "core/output.h"
#ifdef ANLAND_KWIN_66
#include "core/region.h"
#endif
#include <QRegion>

namespace KWin
{

#ifdef ANLAND_KWIN_66
inline QRegion anlandDeviceDamage(const Region &damage)
{
    QRegion result;
    for (const auto &rect : damage.rects()) {
        result |= QRect(rect);
    }
    return result;
}
#else
inline QRegion anlandDeviceDamage(const QRegion &damage)
{
    return damage;
}
#endif

// KWin supplies untransformed device damage. The final encoding copies raw
// physical pixels, so its scissor must match the scene's render transform,
// including the consumer's vertical flip. The result uses GL framebuffer
// coordinates (bottom-left origin), not Android's top-left buffer coordinates.
inline QRegion anlandHdrBufferDamage(const QRegion &deviceDamage, const QSize &bufferSize,
                                    const OutputTransform &contentTransform)
{
    const QSize deviceSize = contentTransform.map(bufferSize);
    const QRegion clipped = deviceDamage.intersected(QRect(QPoint(), deviceSize));
    QRegion result;
    for (const QRect &rect : clipped) {
#ifdef ANLAND_KWIN_66
        const QRect mapped = contentTransform.map(Rect(rect), deviceSize);
#else
        const QRect mapped = contentTransform.map(rect, deviceSize);
#endif
        result |= QRect(mapped.x(), bufferSize.height() - mapped.y() - mapped.height(),
                        mapped.width(), mapped.height());
    }
    return result;
}

} // namespace KWin
