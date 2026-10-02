/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-License-Identifier: GPL-2.0-or-later
*/
#include "anland_input.h"
#include "anland_backend.h"

#include <chrono>

namespace KWin
{

static std::chrono::microseconds now()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch());
}

AnlandInputDevice::AnlandInputDevice(QObject *parent)
    : InputDevice(parent)
{
}

QString AnlandInputDevice::name() const
{
    return QStringLiteral("anland virtual input");
}

bool AnlandInputDevice::isEnabled() const
{
    return m_enabled;
}

void AnlandInputDevice::setEnabled(bool enabled)
{
    m_enabled = enabled;
}

LEDs AnlandInputDevice::leds() const
{
    return LEDs();
}

void AnlandInputDevice::setLeds(LEDs leds)
{
}

bool AnlandInputDevice::isKeyboard() const
{
    return true;
}

bool AnlandInputDevice::isPointer() const
{
    return true;
}

bool AnlandInputDevice::isTouchpad() const
{
    return false;
}

bool AnlandInputDevice::isTouch() const
{
    return true;
}

bool AnlandInputDevice::isTabletTool() const
{
    return false;
}

bool AnlandInputDevice::isTabletPad() const
{
    return false;
}

bool AnlandInputDevice::isTabletModeSwitch() const
{
    return false;
}

bool AnlandInputDevice::isLidSwitch() const
{
    return false;
}

// KWin only delivers pointer events to client apps on a wl_pointer.frame, so every
// pointer event must be followed by a pointerFrame() (exactly as the libinput
// backend does). Without it scroll is dropped entirely; cursor motion and focus
// still appear to work only because PointerInputRedirection handles those
// compositor-side, independent of client delivery.
void AnlandInputDevice::pointerMotionAbsolute(const QPointF &position)
{
    Q_EMIT InputDevice::pointerMotionAbsolute(position, now(), this);
    Q_EMIT InputDevice::pointerFrame(this);
}

void AnlandInputDevice::pointerMotion(const QPointF &position, const QPointF &delta, const QPointF &deltaNonAccelerated)
{
    Q_EMIT InputDevice::pointerMotionAbsolute(position, now(), this);
    Q_EMIT InputDevice::pointerMotion(delta, deltaNonAccelerated, now(), this);
    Q_EMIT InputDevice::pointerFrame(this);
}

void AnlandInputDevice::pointerButton(quint32 button, bool pressed)
{
    Q_EMIT pointerButtonChanged(button,
                                pressed ? PointerButtonState::Pressed : PointerButtonState::Released,
                                now(), this);
    Q_EMIT InputDevice::pointerFrame(this);
}

void AnlandInputDevice::pointerAxis(PointerAxis axis, qreal delta, qint32 deltaV120)
{
    const PointerAxisSource source =
        deltaV120 != 0 ? PointerAxisSource::Wheel : PointerAxisSource::Continuous;
    // 6.x pointerAxisChanged carries an explicit "inverted" (natural scroll) flag;
    // the daemon already delivers deltas in KWin's direction, so it is never set.
    Q_EMIT pointerAxisChanged(axis, delta, deltaV120, source, false, now(), this);
    Q_EMIT InputDevice::pointerFrame(this);
}

void AnlandInputDevice::keyboardKey(quint32 keycode, bool pressed)
{
    Q_EMIT keyChanged(keycode,
                      pressed ? KeyboardKeyState::Pressed : KeyboardKeyState::Released,
                      now(), this);
}

void AnlandInputDevice::touchDown(qint32 id, const QPointF &position)
{
    Q_EMIT InputDevice::touchDown(id, position, now(), this);
}

void AnlandInputDevice::touchMotion(qint32 id, const QPointF &position)
{
    Q_EMIT InputDevice::touchMotion(id, position, now(), this);
}

void AnlandInputDevice::touchUp(qint32 id)
{
    Q_EMIT InputDevice::touchUp(id, now(), this);
}

void AnlandInputDevice::touchFrame()
{
    Q_EMIT InputDevice::touchFrame(this);
}

void AnlandInputDevice::touchCancel()
{
    Q_EMIT touchCanceled(this);
}

AnlandInputBackend::AnlandInputBackend(AnlandBackend *backend)
    : m_backend(backend)
{
}

void AnlandInputBackend::initialize()
{
    if (AnlandInputDevice *device = m_backend->inputDevice()) {
        Q_EMIT deviceAdded(device);
    }
}

} // namespace KWin

#include "moc_anland_input.cpp"
