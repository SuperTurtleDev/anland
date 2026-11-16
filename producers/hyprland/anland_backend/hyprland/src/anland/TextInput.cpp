#include "Bridge.hpp"
#include "BridgeInternal.hpp"
#include "TextCodec.hpp"
#include "../managers/input/InputManager.hpp"
#include "../managers/input/TextInput.hpp"
#include "../managers/SeatManager.hpp"
#include "../managers/eventLoop/EventLoopManager.hpp"
#include "../desktop/state/FocusState.hpp"
#include "../protocols/core/Seat.hpp"
#include "../protocols/core/Compositor.hpp"
#include "../protocols/InputMethodV2.hpp"
#include "../protocols/IdleNotify.hpp"
#include "../devices/IKeyboard.hpp"
#include <deque>
#include <algorithm>
#include <cstdio>
#include <chrono>
#include <format>

namespace {
// Client-only keyboard: never registered with InputManager or WM bindings.
class CUnicodeKeyboard : public IKeyboard {
  public:
#if __has_include("../desktop/view/window/Window.hpp")
    bool isVirtual() const override { return true; }
#else
    bool isVirtual() override { return true; }
#endif
    SP<Aquamarine::IKeyboard> aq() override { return nullptr; }
    bool install(const std::vector<xkb_keysym_t>& symbols) {
        if (symbols.size() <= mapped.size() && std::equal(symbols.begin(), symbols.end(), mapped.begin())) return true;
        std::string map = "xkb_keymap { xkb_keycodes { minimum=8; maximum=255; ";
        for (size_t i = 0; i < symbols.size(); ++i) map += "<T" + std::to_string(i) + ">=" + std::to_string(200 + i) + "; ";
        map += "}; xkb_types { include \"complete\" }; xkb_compatibility { include \"complete\" }; xkb_symbols { ";
        for (size_t i = 0; i < symbols.size(); ++i) {
            char hex[16]; std::snprintf(hex, sizeof(hex), "0x%x", symbols[i]);
            map += "key <T" + std::to_string(i) + "> { [ " + hex + " ] }; ";
        }
        map += "}; };";
        auto context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
        auto keymap = context ? xkb_keymap_new_from_string(context, map.c_str(), XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS) : nullptr;
        if (context) xkb_context_unref(context);
        if (!keymap) return false;
        if (m_xkbKeymap) xkb_keymap_unref(m_xkbKeymap);
        mapped.clear(); m_xkbKeymap = keymap; updateKeymapFD();
        if (!m_xkbKeymapV1FD.isValid()) return false;
        mapped = symbols; return true;
    }
    std::vector<xkb_keysym_t> mapped;
};
struct SItem { xkb_keysym_t symbol = 0; uint32_t key = 0, time = 0; bool pressed = false; std::string nativeText; CTextInput* nativeInput = nullptr; };
struct STextState {
    WP<Aquamarine::CAnlandBackend> backend;
    WP<CWLSurfaceResource> focus;
    WP<CWLSeatResource> mappedSeat;
    SP<CUnicodeKeyboard> keyboard;
    SP<CEventLoopTimer> timer;
    CHyprSignalListener focusListener;
    std::deque<SItem> queue;
    size_t nativeBytes = 0;
    bool lease = false, holding = false, replaying = false;
};
STextState state;

bool usableFocus() {
    if (!g_pInputManager || !g_pSeatManager) return false;
    auto focus = g_pSeatManager->m_state.keyboardFocus.lock();
    return focus && focus == Desktop::focusState()->surface() && focus->m_mapped &&
        !g_pSeatManager->m_seatGrab && g_pInputManager->m_relay.m_inputMethod.expired();
}
void restoreMap() {
    state.lease = false; // Disable sendKeymap substitution before restoring.
    auto seat = state.mappedSeat.lock();
    auto keyboard = g_pSeatManager ? g_pSeatManager->m_keyboard.lock() : nullptr;
    if (seat && keyboard) for (auto& weak : seat->m_keyboards) if (auto resource = weak.lock()) {
        resource->sendKeymap(keyboard);
        const auto mods = keyboard->m_modifiersState;
        resource->sendMods(mods.depressed, mods.latched, mods.locked, mods.group);
        resource->repeatInfo(keyboard->m_repeatRate, keyboard->m_repeatDelay);
    }
    state.mappedSeat.reset();
}
void arm(int ms) { if (state.timer && g_pEventLoopManager) state.timer->updateTimeout(std::chrono::milliseconds(ms)); }
void tick() {
    if (state.queue.empty()) {
        state.holding = false;
        if (!usableFocus()) Anland::resetTextInput();
        return;
    }
    // Real releases retained by resetTextInput still pass after focus loss.
    if (!state.queue.front().symbol && state.queue.front().nativeText.empty()) {
        restoreMap(); state.holding = false;
        unsigned budget = 64;
        while (budget-- && !state.queue.empty() && !state.queue.front().symbol && state.queue.front().nativeText.empty()) {
            const auto item = state.queue.front(); state.queue.pop_front();
            if (auto backend = state.backend.lock()) {
                state.replaying = true; backend->deliverTextQueuedKey(item.key, item.pressed, item.time); state.replaying = false;
            }
        }
        state.holding = true; arm(100); return;
    }
    if (!usableFocus() || state.focus != g_pSeatManager->m_state.keyboardFocus) { Anland::resetTextInput(); return; }
    if (!state.queue.front().nativeText.empty()) {
        restoreMap();
        auto item = std::move(state.queue.front()); state.queue.pop_front(); state.nativeBytes -= item.nativeText.size();
        auto input = g_pInputManager->m_relay.getFocusedTextInput();
        if (input && input == item.nativeInput && input->isEnabled()) input->commitStringFromConsumer(item.nativeText);
        // Disabled/destroyed native editor is NOT permission to inject its old text.
        state.holding = false; if (!state.queue.empty()) arm(1); return;
    }
    std::vector<xkb_keysym_t> symbols;
    for (auto& item : state.queue) { if (!item.symbol || symbols.size() == Anland::TEXT_BATCH_MAX) break; symbols.push_back(item.symbol); }
    if (!state.keyboard) state.keyboard = makeShared<CUnicodeKeyboard>();
    if (!state.keyboard->install(symbols)) { Anland::resetTextInput(); return; }
    auto seat = g_pSeatManager->m_state.keyboardFocusResource.lock();
    if (!seat) { Anland::resetTextInput(); return; }
    state.mappedSeat = seat; state.lease = true;
    // Only this client's resources receive the Unicode map. Never switch global seat.
    for (auto& weak : seat->m_keyboards) if (auto resource = weak.lock()) {
        resource->sendKeymap(state.keyboard); resource->repeatInfo(0, 0); resource->sendMods(0, 0, 0, 0);
    }
    const auto ms = uint32_t(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
    for (size_t i = 0; i < symbols.size(); ++i) {
        g_pSeatManager->sendKeyboardKey(ms, 192 + i, WL_KEYBOARD_KEY_STATE_PRESSED);
        g_pSeatManager->sendKeyboardKey(ms, 192 + i, WL_KEYBOARD_KEY_STATE_RELEASED);
        state.queue.pop_front();
    }
    if (PROTO::idle) PROTO::idle->onActivity();
    state.holding = true; arm(100); // Xwayland must consume keys before another map.
}
void ensureTimer() {
    if (state.timer || !g_pEventLoopManager || !g_pSeatManager) return;
    state.timer = makeShared<CEventLoopTimer>(std::nullopt, [](auto, void*) { tick(); }, nullptr);
    g_pEventLoopManager->addTimer(state.timer);
    state.focusListener = g_pSeatManager->m_events.keyboardFocusChange.listen([] { Anland::resetTextInput(); });
}
}
void Anland::setTextBackend(SP<Aquamarine::CAnlandBackend> backend) { state.backend = backend; }
void Anland::resetTextInput() {
    restoreMap(); state.focus.reset(); state.holding = false; state.nativeBytes = 0;
    if (state.timer && g_pEventLoopManager) state.timer->updateTimeout(std::nullopt);
    // Cancel unforwarded presses/text; preserve real releases to avoid stuck keys.
    std::erase_if(state.queue, [](const SItem& item) { return item.symbol || item.pressed || !item.nativeText.empty(); });
    if (!state.queue.empty()) arm(1);
    if (state.keyboard) state.keyboard->mapped.clear();
}
void Anland::cancelTextInput() {
    resetTextInput(); state.queue.clear();
    if (state.timer && g_pEventLoopManager) state.timer->updateTimeout(std::nullopt); // AQ resetInput releases forwarded keys.
}
void Anland::beforeKeyboardEvent() {
    if (state.replaying) return;
    // Another keyboard takes over; Anland FIFO replay bypasses only this hook.
    if (state.lease || !state.queue.empty()) resetTextInput();
    if (state.timer) { state.holding = true; arm(100); }
}
bool Anland::queueKeyFromConsumer(uint32_t key, bool pressed, uint32_t time) {
    ensureTimer();
    if (!state.timer) return false;
    if (state.queue.empty() && (!state.holding || !state.lease)) {
        restoreMap(); state.holding = true; arm(100); return false;
    }
    if (state.queue.size() == TEXT_QUEUE_MAX) {
        Log::logger->log(Log::WARN, "anland: key FIFO overflow; cancelling text and releasing forwarded keys");
        if (auto backend = state.backend.lock()) backend->cancelTextQueuedKeys();
        else cancelTextInput();
        return false;
    }
    state.queue.push_back({.key = key, .time = time, .pressed = pressed}); return true;
}
SP<IKeyboard> Anland::keyboardForClient(SP<IKeyboard> keyboard, wl_client* client) {
    auto seat = state.mappedSeat.lock();
    return state.lease && seat && seat->client() == client ? state.keyboard : keyboard;
}
void Anland::textFromConsumer(const char* bytes, size_t count, void*) {
    std::vector<xkb_keysym_t> symbols;
    if (!decodeText(bytes, count, symbols)) { Log::logger->log(Log::WARN, std::string_view{std::format("anland: invalid/oversized committed UTF-8 ({} bytes)", count)}); return; }
    if (!usableFocus()) { resetTextInput(); Log::logger->log(Log::DEBUG, "anland: text rejected: focus/grab/IME ownership"); return; }
    auto input = g_pInputManager->m_relay.getFocusedTextInput();
    const bool native = input && input->isEnabled();
    if (native && count > TEXT_WIRE_MAX) { Log::logger->log(Log::WARN, std::string_view{std::format("anland: text-input exceeds wire limit ({} bytes)", count)}); return; }
    if (native && state.queue.empty() && !state.holding) {
        restoreMap(); input->commitStringFromConsumer(std::string(bytes, count));
        if (PROTO::idle) PROTO::idle->onActivity();
        return; // Never convert a rejected native commit into keyboard injection.
    }
    ensureTimer();
    if (!state.timer || symbols.size() > TEXT_QUEUE_MAX - state.queue.size() || count > TEXT_QUEUE_MAX - state.nativeBytes) {
        Log::logger->log(Log::WARN, "anland: local text FIFO full/unavailable"); return;
    }
    if (!state.focus) state.focus = g_pSeatManager->m_state.keyboardFocus;
    const bool idle = state.queue.empty() && !state.holding;
    if (native) { state.queue.push_back({.nativeText = std::string(bytes, count), .nativeInput = input}); state.nativeBytes += count; }
    else for (const auto symbol : symbols) state.queue.push_back({.symbol = symbol});
    if (idle) arm(1);
}
