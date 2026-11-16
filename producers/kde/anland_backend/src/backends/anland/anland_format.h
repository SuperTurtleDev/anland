/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include <QByteArray>
#include <cstdint>
#include <drm_fourcc.h>
#include <optional>

namespace KWin
{

// These are Android HAL pixel format values carried by the unchanged legacy
// protocol. They describe storage only, never a transfer function or HDR mode.
inline uint32_t anlandProtocolFormatToDrm(uint32_t format)
{
    switch (format) {
    case 1: // HAL_PIXEL_FORMAT_RGBA_8888
        return DRM_FORMAT_ABGR8888;
    case 2: // HAL_PIXEL_FORMAT_RGBX_8888
        return DRM_FORMAT_XBGR8888;
    case 5: // HAL_PIXEL_FORMAT_BGRA_8888
        return DRM_FORMAT_ARGB8888;
    case 43: // HAL_PIXEL_FORMAT_RGBA_1010102
        return DRM_FORMAT_ABGR2101010;
    default:
        return DRM_FORMAT_INVALID;
    }
}

inline bool anlandProtocolFormatSupportsMode(uint32_t format, bool hdr)
{
    // In this fixed deployment the app allocates 43 only for its HDR setting.
    // Refuse either mismatch, while keeping storage mapping independent above.
    return hdr ? format == 43 : format == 1 || format == 2 || format == 5;
}

inline bool anlandHdrExplicitlyDisabled(const QByteArray &environment)
{
    const QByteArray value = environment.trimmed().toLower();
    return value == "0" || value == "false" || value == "no" || value == "off";
}

inline std::optional<bool> anlandConsumerHdrMode(uint32_t format, bool hdrAllowed)
{
    // Local contract with the Android consumer: only its HDR setting requests
    // HAL 43, paired with BT.2020/PQ. This is not general HDR format detection
    // or protocol color negotiation. An environment value can forbid HDR but
    // never force it onto a consumer's SDR buffers.
    if (format == 43) {
        return hdrAllowed ? std::optional<bool>(true) : std::nullopt;
    }
    if (anlandProtocolFormatSupportsMode(format, false)) {
        return false;
    }
    return std::nullopt;
}

} // namespace KWin
