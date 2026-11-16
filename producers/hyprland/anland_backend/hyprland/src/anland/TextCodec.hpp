#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>
#include <xkbcommon/xkbcommon.h>
#include <xkbcommon/xkbcommon-keysyms.h>
namespace Anland {
inline constexpr size_t TEXT_QUEUE_MAX = 65536, TEXT_BATCH_MAX = 32, TEXT_WIRE_MAX = 4000;
// Validate the entire packet before sending any event. No replacement characters.
inline bool decodeText(const char* bytes, size_t length, std::vector<xkb_keysym_t>& symbols) {
    symbols.clear();
    if (!bytes || !length || length > TEXT_QUEUE_MAX) return false;
    for (size_t i = 0; i < length;) {
        uint32_t cp = static_cast<unsigned char>(bytes[i++]);
        unsigned n;
        if (cp < 0x80) n = 0;
        else if (cp >= 0xc2 && cp <= 0xdf) { cp &= 0x1f; n = 1; }
        else if (cp >= 0xe0 && cp <= 0xef) { cp &= 0x0f; n = 2; }
        else if (cp >= 0xf0 && cp <= 0xf4) { cp &= 7; n = 3; }
        else return false;
        if (i + n > length) return false;
        for (unsigned j = 0; j < n; ++j) {
            const auto c = static_cast<unsigned char>(bytes[i++]);
            if ((c & 0xc0) != 0x80) return false;
            cp = (cp << 6) | (c & 0x3f);
        }
        if (!cp || (n == 1 && cp < 0x80) || (n == 2 && cp < 0x800) || (n == 3 && cp < 0x10000) ||
            cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return false;
        const auto sym = cp == '\n' || cp == '\r' ? XKB_KEY_Return :
            cp == '\t' ? XKB_KEY_Tab : cp == '\b' ? XKB_KEY_BackSpace : xkb_utf32_to_keysym(cp);
        if (sym == XKB_KEY_NoSymbol) return false;
        symbols.push_back(sym);
    }
    return true;
}
}
