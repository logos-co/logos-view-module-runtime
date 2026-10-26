#include "SurfaceChannel.h"

#include <QLocalSocket>
#include <QLoggingCategory>

#if defined(Q_OS_LINUX)
#include <sys/socket.h>
#include <sys/types.h>
#elif defined(Q_OS_MACOS)
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ucred.h>
#elif defined(Q_OS_WIN)
#include <windows.h>
#endif

Q_LOGGING_CATEGORY(lcSurfaceChannel, "logos.surface.channel")

namespace logos::surface {

SurfaceChannel::SurfaceChannel(QLocalSocket* socket, QObject* parent)
    : QObject(parent), m_socket(socket)
{
    connect(m_socket, &QLocalSocket::readyRead, this, &SurfaceChannel::onReadyRead);
    connect(m_socket, &QLocalSocket::disconnected, this, &SurfaceChannel::closed);
    // Anything already buffered before the connection was handed over.
    if (m_socket->bytesAvailable() > 0)
        QMetaObject::invokeMethod(this, &SurfaceChannel::onReadyRead, Qt::QueuedConnection);
}

bool SurfaceChannel::isOpen() const
{
    return m_socket && m_socket->state() == QLocalSocket::ConnectedState && !m_failed;
}

bool SurfaceChannel::send(MessageType type, const QByteArray& payload, uint32_t generation,
                          uint32_t flags)
{
    if (!isOpen()) return false;
    if (payload.size() > int(kMaxPayload)) {
        qCWarning(lcSurfaceChannel) << "refusing to send oversize payload" << payload.size();
        return false;
    }
    Header h;
    h.type = uint16_t(type);
    h.length = uint32_t(payload.size());
    h.generation = generation;
    h.sequence = ++m_sequence;
    h.flags = flags;
    QByteArray frame = encodeHeader(h);
    frame.append(payload);
    const qint64 n = m_socket->write(frame);
    if (n != frame.size()) {
        fail(QStringLiteral("short write"));
        return false;
    }
    return true;
}

void SurfaceChannel::onReadyRead()
{
    if (m_failed || !m_socket) return;
    // Bounded: never let a peer make us hold more than kMaxReadBuffer.
    const qint64 room = qint64(kMaxReadBuffer) - m_buffer.size();
    if (room <= 0) {
        fail(QStringLiteral("read buffer exhausted"));
        return;
    }
    m_buffer.append(m_socket->read(room));

    while (m_buffer.size() >= int(kHeaderSize)) {
        Header h;
        if (!decodeHeader(m_buffer, &h)) {
            fail(QStringLiteral("bad header"));
            return;
        }
        const int total = int(kHeaderSize) + int(h.length);
        if (m_buffer.size() < total) break;      // wait for the payload
        const QByteArray payload = m_buffer.mid(int(kHeaderSize), int(h.length));
        m_buffer.remove(0, total);
        emit message(h, payload);
        if (m_failed) return;                    // a handler closed us
    }
    if (m_socket && m_socket->bytesAvailable() > 0 && !m_failed)
        QMetaObject::invokeMethod(this, &SurfaceChannel::onReadyRead, Qt::QueuedConnection);
}

void SurfaceChannel::fail(const QString& what)
{
    if (m_failed) return;
    m_failed = true;
    qCWarning(lcSurfaceChannel) << "protocol error:" << what;
    emit protocolError(what);
    if (m_socket) m_socket->abort();
}

void SurfaceChannel::close(ByeReason reason)
{
    if (!m_socket) return;
    if (isOpen()) {
        send(MessageType::Bye, Bye{reason}.encode());
        m_socket->flush();
        m_socket->disconnectFromServer();
    }
    m_failed = true;
}

qint64 SurfaceChannel::peerProcessId(QLocalSocket* socket)
{
    if (!socket) return 0;
#if defined(Q_OS_LINUX)
    struct ucred cred {};
    socklen_t len = sizeof(cred);
    if (getsockopt(int(socket->socketDescriptor()), SOL_SOCKET, SO_PEERCRED, &cred, &len) == 0)
        return cred.pid;
    return 0;
#elif defined(Q_OS_MACOS)
    pid_t pid = 0;
    socklen_t len = sizeof(pid);
    if (getsockopt(int(socket->socketDescriptor()), SOL_LOCAL, LOCAL_PEERPID, &pid, &len) == 0)
        return pid;
    return 0;
#elif defined(Q_OS_WIN)
    ULONG pid = 0;
    if (GetNamedPipeClientProcessId(reinterpret_cast<HANDLE>(socket->socketDescriptor()), &pid))
        return pid;
    return 0;
#else
    return 0;
#endif
}

} // namespace logos::surface
