#include "SurfaceClientSession.h"

#include "SurfaceChannel.h"
#include "SurfaceProducer.h"

#include <QCoreApplication>
#include <QLocalSocket>
#include <QLoggingCategory>
#include <QQuickWindow>

Q_LOGGING_CATEGORY(lcSurfaceClient, "logos.surface.client")

namespace logos::surface {

SurfaceClientSession::SurfaceClientSession(QObject* parent) : QObject(parent)
{
    m_producer = new SurfaceProducer(this);
    connect(m_producer, &SurfaceProducer::demand, this, &SurfaceClientSession::onDemand);
}

SurfaceClientSession::~SurfaceClientSession()
{
    if (m_channel && m_channel->isOpen()) m_channel->close(ByeNormal);
    resetSlots();
    // Slots hold QRhi resources of the producer's rhi: gone before the producer.
    delete m_producer;
    m_producer = nullptr;
}

bool SurfaceClientSession::connectToHost(const QString& serverName, const QString& token)
{
    if (m_socket || serverName.isEmpty() || token.size() != int(kTokenLength)) return false;
    m_token = token;
    m_socket = new QLocalSocket(this);
    m_channel = new SurfaceChannel(m_socket, this);
    connect(m_channel, &SurfaceChannel::message, this, &SurfaceClientSession::onMessage);
    connect(m_channel, &SurfaceChannel::protocolError, this, [this](const QString& what) {
        qCWarning(lcSurfaceClient) << "protocol error:" << what;
    });
    connect(m_channel, &SurfaceChannel::closed, this, [this] {
        if (m_closed) return;
        m_closed = true;
        m_welcomed = false;
        emit closed();
    });
    connect(m_socket, &QLocalSocket::connected, this, [this] {
        Hello h;
        h.pid = uint64_t(QCoreApplication::applicationPid());
        h.graphicsApi = SurfaceProducer::graphicsApiValue();
        h.slotKinds = viewSlotKinds(h.graphicsApi);
        h.token = m_token.toLatin1();
        m_channel->send(MessageType::Hello, h.encode());
    });
    connect(m_socket, &QLocalSocket::errorOccurred, this, [this](QLocalSocket::LocalSocketError e) {
        if (m_closed) return;
        qCWarning(lcSurfaceClient) << "socket error" << e << m_socket->errorString();
        if (m_socket->state() != QLocalSocket::ConnectedState) {
            m_closed = true;
            emit closed();
        }
    });
    m_socket->connectToServer(serverName);
    return true;
}

QQuickWindow* SurfaceClientSession::window()
{
    QQuickWindow* w = m_producer->window();
    // A credit may have arrived before the module asked for its window.
    QMetaObject::invokeMethod(this, &SurfaceClientSession::tryRender, Qt::QueuedConnection);
    return w;
}

QString SurfaceClientSession::transport() const
{
    return m_welcomed ? transportNameForKind(m_kind) : QStringLiteral("none");
}

void SurfaceClientSession::onMessage(const Header& header, const QByteArray& payload)
{
    switch (MessageType(header.type)) {
    case MessageType::Welcome: {
        Welcome w;
        if (!Welcome::decode(payload, &w) || !w.accepted) {
            qCWarning(lcSurfaceClient) << "not welcome";
            m_channel->close(ByeProtocolError);
            return;
        }
        m_kind = w.slotKind;
        m_kindRejected = false;
        resetSlots();
        if (!m_welcomed) {
            m_welcomed = true;
            emit welcomed();
        }
        break;
    }
    case MessageType::Configure: {
        Configure c;
        if (!Configure::decode(payload, &c)) { m_channel->close(ByeProtocolError); return; }
        if (header.generation != m_generation) {
            m_generation = header.generation;
            resetSlots();
            m_haveCredit = false;
        }
        m_visible = c.visible != 0;
        m_producer->configure(QSize(int(c.logicalW), int(c.logicalH)), c.dpr);
        emit configured(QSize(int(c.logicalW), int(c.logicalH)), c.dpr, m_visible);
        break;
    }
    case MessageType::Attach: {
        Attach a;
        if (!Attach::decode(payload, &a) || a.slot >= kSlotCount) { m_channel->close(ByeProtocolError); return; }
        if (header.generation != m_generation) break;   // stale
        SlotEntry& e = m_slots[a.slot];
        e.slot.reset();
        e.opened = false;
        e.descriptor = SlotDescriptor::fromAttach(a, FormatRGBA8Premultiplied);
        e.attached = true;
        if (a.kind != m_kind) { m_channel->close(ByeProtocolError); return; }
        break;
    }
    case MessageType::Detach: {
        SlotRef s;
        if (!SlotRef::decode(payload, &s) || s.slot >= kSlotCount) { m_channel->close(ByeProtocolError); return; }
        if (header.generation == m_generation) {
            m_slots[s.slot].slot.reset();
            m_slots[s.slot].attached = false;
            m_slots[s.slot].opened = false;
        }
        break;
    }
    case MessageType::FrameRequest: {
        FrameRequest r;
        if (!FrameRequest::decode(payload, &r) || r.slot >= kSlotCount) { m_channel->close(ByeProtocolError); return; }
        if (header.generation != m_generation) break;   // stale credit; the host will re-grant
        m_haveCredit = true;
        m_credit = r.credit;
        m_creditSlot = r.slot;
        m_creditGeneration = header.generation;
        m_wantedSent = false;
        m_creditClock.start();
        // A credit is permission to render, not an order (RFC-7 §5): spend it
        // now only if the scene has changed since the last frame or nothing of
        // this generation has been shown yet; otherwise hold it until demand.
        if (m_producer->hasDemand() || m_renderedGeneration != m_generation)
            QMetaObject::invokeMethod(this, &SurfaceClientSession::tryRender, Qt::QueuedConnection);
        break;
    }
    case MessageType::Release:
        // Nothing to do on this side: the slot is simply grantable again.
        break;
    case MessageType::Input: {
        Input in;
        if (!Input::decode(payload, &in)) { m_channel->close(ByeProtocolError); return; }
        m_producer->replayInput(in);
        emit inputReplayed(in);
        break;
    }
    case MessageType::Bye:
        m_channel->close(ByeNormal);
        break;
    default:
        if (header.flags & kFlagMustUnderstand) m_channel->close(ByeProtocolError);
        break;
    }
}

bool SurfaceClientSession::openSlot(SlotEntry& e)
{
    if (e.opened) return true;
    if (!e.attached) return false;
    e.slot = createViewSlot(e.descriptor.kind);
    QRhi* rhi = m_producer->hasWindow() ? m_producer->window()->rhi() : nullptr;
    if (!e.slot || !e.slot->open(e.descriptor, rhi)) {
        e.slot.reset();
        if (e.descriptor.kind != SlotKindShm && !m_kindRejected) {
            // RFC-7 §3 KindRejected: this device cannot open the host's kind.
            m_kindRejected = true;
            m_channel->send(MessageType::KindRejected, KindRejected{e.descriptor.kind}.encode());
        }
        return false;
    }
    e.opened = true;
    return true;
}

void SurfaceClientSession::tryRender()
{
    if (!m_haveCredit || !m_channel || !m_channel->isOpen()) return;
    if (!m_producer->hasWindow()) return;          // module has not asked for its window yet
    if (m_creditGeneration != m_generation) { m_haveCredit = false; return; }
    SlotEntry& e = m_slots[m_creditSlot];
    // The slot's render target is built on the producer's QRhi, which exists
    // once the render control is initialised.
    if (!m_producer->ensureReady()) return;
    if (!openSlot(e)) return;

    const qint64 beforeRenderNs = m_creditClock.nsecsElapsed();
    if (!m_producer->renderFrame(e.slot.get())) {
        qCWarning(lcSurfaceClient) << "frame render failed; keeping credit";
        return;
    }
    FrameReady fr;
    fr.slot = m_creditSlot;
    fr.credit = m_credit;
    fr.dmgX = 0; fr.dmgY = 0;
    fr.dmgW = uint32_t(e.descriptor.pixelSize.width());
    fr.dmgH = uint32_t(e.descriptor.pixelSize.height());
    fr.frameIndex = ++m_frameIndex;
    m_haveCredit = false;
    m_renderedGeneration = m_generation;
    m_channel->send(MessageType::FrameReady, fr.encode(), m_generation);
    ++m_framesSent;
    qCDebug(lcSurfaceClient, "frame %llu: credit->render %.1f ms, render %.1f ms", (unsigned long long)fr.frameIndex, beforeRenderNs / 1e6, (m_creditClock.nsecsElapsed() - beforeRenderNs) / 1e6);
    // More to show already? Ask for the next credit right away.
    if (m_producer->hasDemand()) onDemand();
}

void SurfaceClientSession::onDemand()
{
    if (!m_welcomed || !m_channel || !m_channel->isOpen()) return;
    if (m_haveCredit) {
        QMetaObject::invokeMethod(this, &SurfaceClientSession::tryRender, Qt::QueuedConnection);
        return;
    }
    if (m_wantedSent) return;
    m_wantedSent = true;
    m_channel->send(MessageType::FrameWanted, {}, m_generation);
}

void SurfaceClientSession::resetSlots()
{
    for (SlotEntry& e : m_slots) {
        e.slot.reset();
        e.attached = false;
        e.opened = false;
    }
}

} // namespace logos::surface
