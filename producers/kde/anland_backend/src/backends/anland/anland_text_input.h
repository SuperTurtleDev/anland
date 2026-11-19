/*
    SPDX-License-Identifier: GPL-2.0-or-later
*/
#pragma once

#include <QByteArray>
#include <QList>
#include <QPointer>
#include <QQueue>
#include <QString>
#include <QTimer>

namespace KWin
{

class AnlandInputDevice;
class ClientConnection;
class SeatInterface;
class SurfaceInterface;

class AnlandTextInput : public QObject
{
public:
    explicit AnlandTextInput(AnlandInputDevice *device, SeatInterface *seat);

    void submit(const QString &text);
    void key(quint32 code, bool pressed);
    void reset();

private:
    struct Event {
        QString text{};
        QPointer<SurfaceInterface> focus{};
        quint64 focusGeneration = 0;
        quint32 key = 0;
        bool pressed = false;
    };

    bool stillFocused(const Event &event) const;
    void drain();
    bool prepareBatch(SurfaceInterface *focus);
    void sendBatch();
    void restoreKeymap();

    AnlandInputDevice *const m_device;
    const QPointer<SeatInterface> m_seat;
    QTimer m_timer;
    QQueue<Event> m_events;
    quint64 m_focusGeneration = 0;
    qsizetype m_queuedChars = 0;
    QPointer<ClientConnection> m_keymapClient;
    bool m_borrowedKey = false;
    QByteArray m_baseKeymap;
    QList<quint32> m_batchKeys;
    qsizetype m_batchChars = 0;
};

}
