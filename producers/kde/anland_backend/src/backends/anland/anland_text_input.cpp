/*
    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "anland_text_input.h"

#include "anland_input.h"
#include "anland_logging.h"
#include "input.h"
#include "inputmethod.h"
#include "keyboard_input.h"
#include "main.h"
#include "utils/ramfile.h"
#include "wayland/clientconnection.h"
#include "wayland/display.h"
#include "wayland/seat.h"
#include "wayland/surface.h"
#if __has_include("wayland/textinput_v1.h")
#include "wayland/textinput_v1.h"
#endif
#include "wayland/textinput_v2.h"
#include "wayland/textinput_v3.h"
#include "wayland_server.h"
#include "xkb.h"

#include <QHash>
#include <QLoggingCategory>
#include <algorithm>
#include <cstring>
#include <utility>
#include <wayland-server-core.h>
#include <wayland-server-protocol.h>
#include <xkbcommon/xkbcommon.h>

namespace KWin
{
namespace
{
constexpr int KeycodeOffset = 8;
constexpr int LastX11Keycode = 255;
constexpr qsizetype MaxBatchKeys = 64;
constexpr qsizetype MaxQueuedChars = 4096;
constexpr qsizetype MaxQueuedEvents = 4096;
// Xwayland clients process MapNotify and query the map asynchronously. Give
// them time to load its new symbols and finish handling the resulting keys.
constexpr int KeymapSettleMs = 75;
constexpr int KeymapHoldMs = 100;
constexpr int EditingKeySettleMs = 75;

Xkb *seatXkb()
{
    auto keyboard = input() ? input()->keyboard() : nullptr;
    return keyboard ? keyboard->xkb() : nullptr;
}

bool sendKeymap(ClientConnection *client, const QByteArray &contents)
{
    RamFile file("anland-xwayland-keymap", contents.constData(), contents.size() + 1, RamFile::Flag::SealWrite);
    if (!file.isValid()) {
        return false;
    }
    wl_client_for_each_resource(client->client(), [](wl_resource *resource, void *data) {
        if (std::strcmp(wl_resource_get_class(resource), "wl_keyboard") == 0) {
            auto *keymap = static_cast<RamFile *>(data);
            wl_keyboard_send_keymap(resource, WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1, keymap->fd(), keymap->size());
        }
        return WL_ITERATOR_CONTINUE;
    }, &file);
    wl_client_flush(client->client());
    return true;
}

template<typename Seat>
void sendKey(Seat *seat, quint32 key, KeyboardKeyState state)
{
    if constexpr (requires { seat->notifyKeyboardKey(key, state, 0); }) {
        seat->notifyKeyboardKey(key, state, seat->display()->nextSerial());
    } else {
        seat->notifyKeyboardKey(key, state);
    }
}

bool hasSymbols(xkb_keymap *keymap, xkb_keycode_t code)
{
    for (xkb_layout_index_t group = 0; group < xkb_keymap_num_layouts_for_key(keymap, code); ++group) {
        for (xkb_level_index_t level = 0; level < xkb_keymap_num_levels_for_key(keymap, code, group); ++level) {
            const xkb_keysym_t *symbols;
            if (xkb_keymap_key_get_syms_by_level(keymap, code, group, level, &symbols) > 0) {
                return true;
            }
        }
    }
    return false;
}

bool usesXwaylandFallback(SeatInterface *seat, SurfaceInterface *surface)
{
    if (!surface || surface->client() != waylandServer()->xWaylandConnection()) {
        return false;
    }
#if __has_include("wayland/textinput_v1.h")
    if (const auto input = seat->textInputV1(); input && input->isEnabled()) {
        return false;
    }
#endif
    const auto v2 = seat->textInputV2();
    const auto v3 = seat->textInputV3();
    return !(v2 && v2->isEnabled()) && !(v3 && v3->isEnabled());
}

bool needsUnicodeKeymap(const QString &text)
{
    auto *xkb = seatXkb();
    return xkb && std::ranges::any_of(text.toUcs4(), [xkb](char32_t point) {
        const auto symbol = xkb_utf32_to_keysym(point);
        return symbol != XKB_KEY_NoSymbol && !xkb->keycodeFromKeysym(symbol);
    });
}

void commitText(const QString &text)
{
    if (auto method = kwinApp()->inputMethod()) {
#ifdef ANLAND_KWIN_68
        method->sendText(text);
#else
        method->commitText(text);
#endif
    }
}
}

AnlandTextInput::AnlandTextInput(AnlandInputDevice *device, SeatInterface *seat)
    : m_device(device)
    , m_seat(seat)
{
    m_timer.setTimerType(Qt::PreciseTimer);
    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, [this] {
        if (m_batchChars) {
            sendBatch();
        } else {
            drain();
        }
    });
    connect(seat, &SeatInterface::focusedKeyboardSurfaceAboutToChange, this, [this] {
        ++m_focusGeneration;
    });
}

void AnlandTextInput::submit(const QString &text)
{
    if (!m_seat || text.isEmpty() || waylandServer()->isScreenLocked()) {
        return;
    }
    auto *focus = m_seat->focusedKeyboardSurface();
    if (!m_timer.isActive() && (!usesXwaylandFallback(m_seat, focus) || !needsUnicodeKeymap(text))) {
        commitText(text);
        return;
    }
    if (text.size() > MaxQueuedChars - m_queuedChars || m_events.size() >= MaxQueuedEvents) {
        qCWarning(KWIN_ANLAND) << "Android IME text queue is full";
        return;
    }
    m_queuedChars += text.size();
    m_events.enqueue({.text = text, .focus = focus, .focusGeneration = m_focusGeneration});
    drain();
}

void AnlandTextInput::key(quint32 code, bool pressed)
{
    if (!m_timer.isActive()) {
        m_device->keyboardKey(code, pressed);
        return;
    }
    if (m_events.size() >= MaxQueuedEvents) {
        reset();
        m_device->keyboardKey(code, pressed);
        return;
    }
    m_events.enqueue({.key = code, .pressed = pressed});
}

void AnlandTextInput::reset()
{
    m_timer.stop();
    m_batchKeys.clear();
    m_batchChars = 0;
    m_queuedChars = 0;
    restoreKeymap();
    const auto events = std::exchange(m_events, {});
    for (const auto &event : events) {
        if (event.text.isEmpty() && !event.pressed) {
            m_device->keyboardKey(event.key, false);
        }
    }
}

void AnlandTextInput::drain()
{
    bool replayedKeys = false;
    while (!m_timer.isActive() && !m_events.isEmpty()) {
        const auto &event = m_events.head();
        const bool focused = stillFocused(event);
        const bool needsKeymap = !event.text.isEmpty() && focused
            && usesXwaylandFallback(m_seat, event.focus) && needsUnicodeKeymap(event.text);
        if (needsKeymap) {
            // Xwayland queues real key events, including edits between text
            // commits. Leave their map in place until those events drain.
            if (replayedKeys) {
                m_timer.start(EditingKeySettleMs);
                return;
            }
            if (prepareBatch(event.focus)) {
                return;
            }
        }
        if (m_keymapClient && m_borrowedKey && (event.text.isEmpty() || (focused && !needsKeymap))) {
            restoreKeymap();
            m_timer.start(KeymapSettleMs);
            return;
        }
        const auto next = m_events.dequeue();
        m_queuedChars -= next.text.size();
        if (next.text.isEmpty()) {
            m_device->keyboardKey(next.key, next.pressed);
            replayedKeys = true;
        } else if (focused && !needsKeymap) {
            commitText(next.text);
            replayedKeys = true;
        }
    }
    if (!m_timer.isActive()) {
        if (m_keymapClient && replayedKeys) {
            m_timer.start(EditingKeySettleMs);
        } else {
            restoreKeymap();
        }
    }
}

bool AnlandTextInput::stillFocused(const Event &event) const
{
    return m_seat && event.focusGeneration == m_focusGeneration
        && event.focus == m_seat->focusedKeyboardSurface() && !waylandServer()->isScreenLocked();
}

bool AnlandTextInput::prepareBatch(SurfaceInterface *focus)
{
    auto *xkb = seatXkb();
    const auto base = xkb ? xkb->keymapContents() : QByteArray();
    auto *map = xkb ? xkb->keymap() : nullptr;
    const auto symbolSection = base.indexOf("xkb_symbols ");
    const auto symbolEnd = symbolSection < 0 ? -1 : base.indexOf("\n};", symbolSection);
    if (!map || symbolEnd < 0) {
        qCWarning(KWIN_ANLAND) << "cannot read the Xwayland keyboard map for Android IME text";
        return false;
    }

    // Keep the user's normal mappings. A fully occupied layout can still use
    // the same fallback key that KWin borrows for unmappable Unicode symbols.
    QList<xkb_keycode_t> spare;
    for (xkb_keycode_t code = KeycodeOffset; code <= LastX11Keycode; ++code) {
        if (xkb_keymap_key_get_name(map, code) && !hasSymbols(map, code)) {
            spare.append(code);
        }
    }
    const bool borrowedKey = spare.isEmpty();
    if (borrowedKey && xkb_keymap_key_get_name(map, LastX11Keycode)) {
        spare.append(LastX11Keycode);
    }

    QHash<xkb_keysym_t, quint32> assigned;
    QByteArray symbols;
    QList<quint32> keycodes;
    qsizetype chars = 0;
    for (const char32_t point : m_events.head().text.toUcs4()) {
        if (keycodes.size() == MaxBatchKeys) {
            break;
        }
        const auto symbol = xkb_utf32_to_keysym(point);
        if (symbol == XKB_KEY_NoSymbol) {
            chars += point > 0xffff ? 2 : 1;
            continue;
        }
        auto found = assigned.constFind(symbol);
        if (found == assigned.constEnd()) {
            if (assigned.size() == spare.size()) {
                break;
            }
            const auto code = spare[assigned.size()];
            symbols += "\n\treplace key <";
            symbols += xkb_keymap_key_get_name(map, code);
            symbols += "> { [ 0x";
            symbols += QByteArray::number(symbol, 16);
            symbols += " ] };";
            assigned.insert(symbol, code - KeycodeOffset);
        }
        keycodes.append(assigned.value(symbol));
        chars += point > 0xffff ? 2 : 1;
    }
    if (keycodes.isEmpty()) {
        return false;
    }
    QByteArray augmented = base;
    augmented.insert(symbolEnd, symbols);
    if (m_keymapClient && m_keymapClient != focus->client()) {
        restoreKeymap();
    }
    if (!sendKeymap(focus->client(), augmented)) {
        qCWarning(KWIN_ANLAND) << "cannot publish the Xwayland keyboard map for Android IME text";
        return false;
    }
    m_keymapClient = focus->client();
    m_borrowedKey = borrowedKey;
    m_baseKeymap = base;
    m_batchKeys = std::move(keycodes);
    m_batchChars = chars;
    m_timer.start(KeymapSettleMs);
    return true;
}

void AnlandTextInput::sendBatch()
{
    auto *xkb = seatXkb();
    auto &event = m_events.head();
    if (!xkb || !stillFocused(event) || !usesXwaylandFallback(m_seat, event.focus)
        || xkb->keymapContents() != m_baseKeymap) {
        m_batchKeys.clear();
        m_batchChars = 0;
        restoreKeymap();
        drain();
        return;
    }

    m_seat->notifyKeyboardModifiers(0, 0, 0, 0);
    for (const auto code : std::as_const(m_batchKeys)) {
        sendKey(m_seat.data(), code, KeyboardKeyState::Pressed);
        sendKey(m_seat.data(), code, KeyboardKeyState::Released);
    }
    xkb->forwardModifiers();
    m_timer.start(KeymapHoldMs);
    event.text.remove(0, m_batchChars);
    m_queuedChars -= m_batchChars;
    if (event.text.isEmpty()) {
        m_events.dequeue();
    }
    m_batchKeys.clear();
    m_batchChars = 0;
}

void AnlandTextInput::restoreKeymap()
{
    if (m_keymapClient) {
        if (auto *xkb = seatXkb()) {
            const auto base = xkb->keymapContents();
            if (!base.isEmpty()) {
                sendKeymap(m_keymapClient, base);
            }
        }
        m_keymapClient.clear();
    }
    m_borrowedKey = false;
    m_baseKeymap.clear();
}

}
