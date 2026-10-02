/*
    KWin - the KDE window manager
    This file is part of the KDE project.

    SPDX-License-Identifier: GPL-2.0-or-later

    Input for the anland backend. A single combined InputDevice (pointer +
    keyboard + touch) fed from the display daemon's data fd. Mirrors the single
    weston_seat that weston's backend-anland sets up.
*/
#pragma once

#include "core/inputbackend.h"
#include "core/inputdevice.h"

namespace KWin
{

class AnlandBackend;

class AnlandInputDevice : public InputDevice
{
    Q_OBJECT

public:
    explicit AnlandInputDevice(QObject *parent = nullptr);

    QString name() const override;
    bool isEnabled() const override;
    void setEnabled(bool enabled) override;
    LEDs leds() const override;
    void setLeds(LEDs leds) override;

    bool isKeyboard() const override;
    bool isPointer() const override;
    bool isTouchpad() const override;
    bool isTouch() const override;
    bool isTabletTool() const override;
    bool isTabletPad() const override;
    bool isTabletModeSwitch() const override;
    bool isLidSwitch() const override;

    // Event injection helpers (called by AnlandBackend from the data-fd reader).
    void pointerMotionAbsolute(const QPointF &position);
    void pointerMotion(const QPointF &position, const QPointF &delta, const QPointF &deltaNonAccelerated);
    void pointerButton(quint32 button, bool pressed);
    void pointerAxis(PointerAxis axis, qreal delta, qint32 deltaV120);
    void keyboardKey(quint32 keycode, bool pressed);
    void touchDown(qint32 id, const QPointF &position);
    void touchMotion(qint32 id, const QPointF &position);
    void touchUp(qint32 id);
    void touchFrame();
    void touchCancel();

private:
    bool m_enabled = true;
};

class AnlandInputBackend : public InputBackend
{
    Q_OBJECT

public:
    explicit AnlandInputBackend(AnlandBackend *backend);

    void initialize() override;

private:
    AnlandBackend *m_backend;
};

} // namespace KWin
