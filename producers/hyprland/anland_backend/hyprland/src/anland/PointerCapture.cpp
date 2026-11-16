#include "Bridge.hpp"
#include "BridgeInternal.hpp"
#include "../Compositor.hpp"
#include "../managers/input/InputManager.hpp"
#include "../managers/SeatManager.hpp"
#include "../desktop/state/FocusState.hpp"
#include "../desktop/view/WLSurface.hpp"
#if __has_include("../desktop/view/window/Window.hpp")
#include "../desktop/view/window/Window.hpp"
#else
#include "../desktop/view/Window.hpp"
#endif
#include "../protocols/PointerConstraints.hpp"

static WP<Aquamarine::CAnlandBackend> captureBackend;

void Anland::setPointerCaptureBackend(SP<Aquamarine::CAnlandBackend> backend) {
    captureBackend = backend;
}

bool Anland::pointerCaptureRequested() {
    if (!g_pCompositor || !g_pCompositor->m_sessionActive || !g_pInputManager || !g_pSeatManager)
        return false;
    const auto focused = Desktop::focusState()->surface();
    if (!focused || g_pSeatManager->m_state.pointerFocus != focused) return false;
    const auto surface = Desktop::View::CWLSurface::fromResource(focused);
    if (!surface) return false;
    const auto constraint = surface->constraint();
    if (!constraint || !constraint->isActive() || !constraint->isLocked()) return false;
    const auto window = Desktop::View::CWindow::fromView(surface->view());
#if __has_include("../desktop/view/window/Window.hpp")
    return window && !window->cantLockCursor();
#else
    return window && !window->m_layoutFlags.cantLockCursor;
#endif
}

void Anland::syncPointerCapture() {
    if (auto backend = captureBackend.lock()) backend->updateMouseCapture(pointerCaptureRequested());
}
