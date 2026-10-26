#pragma once

// Framing for RFC-7 messages over a QLocalSocket. One instance per end of a
// session. Reads are bounded (kMaxReadBuffer) and every header is validated
// before its payload is waited for; a violation closes the socket and is
// reported once through protocolError().

#include "SurfaceProtocol.h"
#include "SurfaceExport.h"

#include <QByteArray>
#include <QObject>

class QLocalSocket;

namespace logos::surface {

class LOGOS_VIEW_SURFACE_EXPORT SurfaceChannel : public QObject {
    Q_OBJECT
public:
    // Takes no ownership; the socket must outlive the channel or be reset.
    explicit SurfaceChannel(QLocalSocket* socket, QObject* parent = nullptr);

    QLocalSocket* socket() const { return m_socket; }
    bool isOpen() const;

    // Sends one message. Returns false (and does nothing) when the socket is
    // not connected or the payload exceeds kMaxPayload.
    bool send(MessageType type, const QByteArray& payload = {}, uint32_t generation = 0,
              uint32_t flags = 0);

    // Peer process id from the OS (SO_PEERCRED / LOCAL_PEERPID /
    // GetNamedPipeClientProcessId); 0 when unavailable.
    static qint64 peerProcessId(QLocalSocket* socket);

    void close(ByeReason reason);

signals:
    void message(const logos::surface::Header& header, const QByteArray& payload);
    void protocolError(const QString& what);
    void closed();

private:
    void onReadyRead();
    void fail(const QString& what);

    QLocalSocket* m_socket = nullptr;
    QByteArray m_buffer;
    uint32_t m_sequence = 0;
    bool m_failed = false;
};

} // namespace logos::surface
