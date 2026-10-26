#include "SurfaceHostSession.h"

#include "SurfaceChannel.h"

#include <QLocalServer>
#include <QLocalSocket>
#include <QLoggingCategory>
#include <QQuickWindow>
#include <QRandomGenerator>
#include <QSGRendererInterface>
#include <QTimer>
#include <QtMath>

Q_LOGGING_CATEGORY(lcSurfaceHost, "logos.surface.host")

namespace logos::surface {

#if defined(Q_OS_WIN)
uint64_t d3d11HostDeviceLuid(QQuickWindow* window);           // D3D11SlotStorage.cpp
#endif
#if defined(Q_OS_MACOS)
void iosurfaceBeginRendezvous(const QByteArray& token, HostSlot* const slots[kSlotCount]);   // IOSurfaceSlotStorage.mm
#endif

namespace {

QString randomServerName()
{
    QByteArray raw(4, Qt::Uninitialized);
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32*>(raw.data()), 1);
    return QStringLiteral("logos-surface-%1-%2")
        .arg(QCoreApplication::applicationPid())
        .arg(QString::fromLatin1(raw.toHex()));
}

uint32_t hostApiFor(QQuickWindow* window)
{
    if (!window) return ApiUnknown;
    switch (window->graphicsApi()) {
    case QSGRendererInterface::Software: return ApiSoftware;
    case QSGRendererInterface::OpenGL: return ApiOpenGL;
    case QSGRendererInterface::Direct3D11: return ApiDirect3D11;
    case QSGRendererInterface::Direct3D12: return ApiDirect3D12;
    case QSGRendererInterface::Metal: return ApiMetal;
    case QSGRendererInterface::Vulkan: return ApiVulkan;
    default: return ApiUnknown;
    }
}

} // namespace

SurfaceHostSession::SurfaceHostSession(QObject* parent) : QObject(parent) {}

SurfaceHostSession::~SurfaceHostSession()
{
    close();
}

bool SurfaceHostSession::listen()
{
    if (m_server) return true;
    m_server = new QLocalServer(this);
    m_server->setSocketOptions(QLocalServer::UserAccessOption);
    // A stale name from a crashed host of the same pid is near-impossible
    // with the random suffix, but remove it rather than fail.
    const QString name = randomServerName();
    QLocalServer::removeServer(name);
    if (!m_server->listen(name)) {
        qCWarning(lcSurfaceHost) << "listen failed:" << m_server->errorString();
        delete m_server;
        m_server = nullptr;
        return false;
    }
    connect(m_server, &QLocalServer::newConnection, this, &SurfaceHostSession::onNewConnection);
    return true;
}

QString SurfaceHostSession::serverName() const
{
    return m_server ? m_server->serverName() : QString();
}

void SurfaceHostSession::expect(const QString& token, qint64 pid)
{
    m_expectedToken = token;
    m_expectedPid = pid;
}

void SurfaceHostSession::setHostWindow(QQuickWindow* window)
{
    m_hostWindow = window;
}

QString SurfaceHostSession::transport() const
{
    return m_connected ? transportNameForKind(m_kind) : QStringLiteral("none");
}

// ── Connection and handshake ───────────────────────────────────────────────

void SurfaceHostSession::onNewConnection()
{
    while (QLocalSocket* s = m_server->nextPendingConnection()) {
        if (m_socket) {
            qCWarning(lcSurfaceHost) << "second connection refused";
            s->abort();
            s->deleteLater();
            continue;
        }
        m_socket = s;
        m_socket->setParent(this);
        m_channel = new SurfaceChannel(m_socket, this);
        connect(m_channel, &SurfaceChannel::message, this, &SurfaceHostSession::onMessage);
        connect(m_channel, &SurfaceChannel::protocolError, this,
                [this](const QString& what) { fail(QStringLiteral("protocolError"), what); });
        connect(m_channel, &SurfaceChannel::closed, this, &SurfaceHostSession::onChannelClosed);

        m_helloTimer = new QTimer(this);
        m_helloTimer->setSingleShot(true);
        m_helloTimer->setInterval(kHelloTimeoutMs);
        connect(m_helloTimer, &QTimer::timeout, this, [this] {
            if (!m_connected) fail(QStringLiteral("protocolError"), QStringLiteral("no Hello"));
        });
        m_helloTimer->start();
    }
}

void SurfaceHostSession::onMessage(const Header& header, const QByteArray& payload)
{
    const auto type = MessageType(header.type);
    if (!m_connected) {
        if (type != MessageType::Hello) {
            fail(QStringLiteral("protocolError"), QStringLiteral("first message is not Hello"));
            return;
        }
        onHello(payload);
        return;
    }
    switch (type) {
    case MessageType::Hello:
        fail(QStringLiteral("protocolError"), QStringLiteral("second Hello"));
        break;
    case MessageType::KindRejected: {
        KindRejected m;
        if (!KindRejected::decode(payload, &m) || m.kind != m_kind || m_kind == SlotKindShm) {
            fail(QStringLiteral("protocolError"), QStringLiteral("bad KindRejected"));
            break;
        }
        qCInfo(lcSurfaceHost) << "view rejected kind" << m_kind << "- falling back to shm";
        m_kind = SlotKindShm;
        emit transportChanged();
        Welcome w;
        w.accepted = 1; w.slotKind = m_kind; w.slotCount = kSlotCount;
        m_channel->send(MessageType::Welcome, w.encode());
        if (m_configured) { ++m_generation; sendGeometry(); }
        break;
    }
    case MessageType::FrameWanted:
        // Informational since the standing credit: the view has demand and no
        // credit, which means the ring is full until the host paints.
        maybeGrant();
        break;
    case MessageType::FrameReady:
        onFrameReady(header, payload);
        break;
    case MessageType::Bye:
        qCInfo(lcSurfaceHost) << "view said Bye";
        dropConnection();
        emit error(QStringLiteral("clientExited"));
        break;
    default:
        if (header.flags & kFlagMustUnderstand)
            fail(QStringLiteral("protocolError"), QStringLiteral("unknown must-understand message"));
        // else: skipped (forward compatibility)
        break;
    }
}

void SurfaceHostSession::onHello(const QByteArray& payload)
{
    Hello m;
    if (!Hello::decode(payload, &m)) {
        fail(QStringLiteral("protocolError"), QStringLiteral("bad Hello"));
        return;
    }
    const qint64 peerPid = SurfaceChannel::peerProcessId(m_socket);
    const QByteArray expectedToken = m_expectedToken.toLatin1();
    const bool pidOk = m_expectedPid > 0 && qint64(m.pid) == m_expectedPid
                       && (peerPid == 0 || peerPid == m_expectedPid);
    const bool tokenOk = expectedToken.size() == int(kTokenLength) && m.token == expectedToken;
    if (!pidOk || !tokenOk) {
        qCWarning(lcSurfaceHost) << "refusing Hello: pid" << m.pid << "peer" << peerPid
                                 << "expected" << m_expectedPid << "token ok" << tokenOk;
        fail(QStringLiteral("protocolError"), QStringLiteral("Hello refused"));
        return;
    }
    if (m_helloTimer) { m_helloTimer->stop(); m_helloTimer->deleteLater(); m_helloTimer = nullptr; }

    m_viewKinds = m.slotKinds;
    m_viewApi = m.graphicsApi;
    m_kind = chooseKind(m_viewKinds, m_viewApi);
    m_connected = true;

    Welcome w;
    w.accepted = 1;
    w.slotKind = m_kind;
    w.slotCount = kSlotCount;
#if defined(Q_OS_WIN)
    if (m_kind == SlotKindD3D11) w.hostDevice = d3d11HostDeviceLuid(m_hostWindow);
#endif
    m_channel->send(MessageType::Welcome, w.encode());
    qCInfo(lcSurfaceHost) << "view connected, kind" << transportNameForKind(m_kind);
    emit connected();
    emit transportChanged();

    if (m_configured) {
        ++m_generation;
        sendGeometry();
    }
}

uint32_t SurfaceHostSession::chooseKind(uint32_t viewKinds, uint32_t) const
{
    if (m_forcedKind != SlotKindNone) return m_forcedKind;
    const uint32_t common = hostSlotKinds() & viewKinds;
    const uint32_t hostApi = hostApiFor(m_hostWindow);
    if ((common & SlotKindD3D11) && hostApi == ApiDirect3D11) return SlotKindD3D11;
    if ((common & SlotKindIOSurface) && hostApi == ApiMetal) return SlotKindIOSurface;
    return SlotKindShm;
}

// ── Geometry ───────────────────────────────────────────────────────────────

void SurfaceHostSession::configure(const QSize& logicalSize, qreal devicePixelRatio, bool visible)
{
    const QSize pixel(qMax(1, qRound(logicalSize.width() * devicePixelRatio)),
                      qMax(1, qRound(logicalSize.height() * devicePixelRatio)));
    const bool geometryChanged = !m_configured || pixel != m_pixelSize
                                 || logicalSize != m_logicalSize || !qFuzzyCompare(devicePixelRatio, m_dpr);
    m_logicalSize = logicalSize;
    m_pixelSize = pixel;
    m_dpr = devicePixelRatio;
    m_configured = true;
    const bool visibilityChanged = visible != m_visible;
    m_visible = visible;

    if (geometryChanged) {
        ++m_generation;
        if (m_connected) sendGeometry();
    } else if (visibilityChanged && m_connected) {
        Configure c;
        c.logicalW = uint32_t(m_logicalSize.width()); c.logicalH = uint32_t(m_logicalSize.height());
        c.pixelW = uint32_t(m_pixelSize.width()); c.pixelH = uint32_t(m_pixelSize.height());
        c.dpr = m_dpr; c.visible = m_visible ? 1 : 0;
        m_channel->send(MessageType::Configure, c.encode(), m_generation);
        maybeGrant();
    }
}

void SurfaceHostSession::setVisible(bool visible)
{
    if (!m_configured) { m_visible = visible; return; }
    configure(m_logicalSize, m_dpr, visible);
}

bool SurfaceHostSession::allocateRing()
{
    // Old slots: the displayed one keeps its storage alive (the host is still
    // painting it) until a frame of the new generation replaces it; the rest
    // go now. All of them are detached on the view side.
    for (uint32_t i = 0; i < kSlotCount; ++i) {
        Entry& e = m_slots[i];
        if (e.state == SlotState::Unallocated) continue;
        if (m_channel && m_channel->isOpen())
            m_channel->send(MessageType::Detach, SlotRef{i}.encode(), e.generation);
        if (int(i) == m_displayed) {
            m_oldDisplayed = std::move(e.slot);
            m_displayed = -1;
        }
        e.slot.reset();
        e.state = SlotState::Unallocated;
    }
    m_outstandingCredit = 0;
    m_grantedSlot = -1;

    if (!slotGeometryWithinBounds(m_pixelSize)) {
        qCWarning(lcSurfaceHost) << "requested geometry out of bounds" << m_pixelSize;
        return false;
    }
    for (uint32_t i = 0; i < kSlotCount; ++i) {
        Entry& e = m_slots[i];
        e.slot = createHostSlot(m_kind);
        if (!e.slot || !e.slot->allocate(i, m_pixelSize, FormatRGBA8Premultiplied, m_expectedPid, m_hostWindow)) {
            qCWarning(lcSurfaceHost) << "slot allocation failed, kind" << m_kind;
            e.slot.reset();
            return false;
        }
        e.state = SlotState::Free;
        e.generation = m_generation;
    }
#if defined(Q_OS_MACOS)
    if (m_kind == SlotKindIOSurface) {
        HostSlot* raw[kSlotCount];
        for (uint32_t i = 0; i < kSlotCount; ++i) raw[i] = m_slots[i].slot.get();
        iosurfaceBeginRendezvous(m_expectedToken.toLatin1(), raw);
    }
#endif
    return true;
}

void SurfaceHostSession::sendGeometry()
{
    if (!m_connected || !m_channel) return;
    if (!allocateRing()) {
        if (m_kind != SlotKindShm) {
            // Native storage failed on this host: negotiate down once.
            m_kind = SlotKindShm;
            emit transportChanged();
            Welcome w; w.accepted = 1; w.slotKind = m_kind; w.slotCount = kSlotCount;
            m_channel->send(MessageType::Welcome, w.encode());
            if (allocateRing()) goto attached;
        }
        fail(QStringLiteral("allocationFailed"), QStringLiteral("could not allocate the slot ring"));
        return;
    }
attached:
    Configure c;
    c.logicalW = uint32_t(m_logicalSize.width()); c.logicalH = uint32_t(m_logicalSize.height());
    c.pixelW = uint32_t(m_pixelSize.width()); c.pixelH = uint32_t(m_pixelSize.height());
    c.dpr = m_dpr; c.visible = m_visible ? 1 : 0; c.format = FormatRGBA8Premultiplied;
    m_channel->send(MessageType::Configure, c.encode(), m_generation);
    for (uint32_t i = 0; i < kSlotCount; ++i)
        m_channel->send(MessageType::Attach, m_slots[i].slot->descriptor().toAttach().encode(), m_generation);
    maybeGrant();
}

// ── Credits and frames ─────────────────────────────────────────────────────

// A credit is a permission, not a request (RFC-7 §5): the host hands the
// view one as soon as a slot is free, and the view spends it only when its
// scene changes. So while the host paints frame k the view already holds the
// slot for k+1 and renders it the moment it has something to draw, with no
// round trip on the critical path; an idle view holds its credit and costs
// nothing. The ring bounds the pipeline: Displayed + Granted + one spare.
// FrameWanted is now informational (the view says so when it has demand and
// no credit); the grant condition does not depend on it.
void SurfaceHostSession::maybeGrant()
{
    if (!m_connected || !m_channel || !m_configured) return;
    if (!m_visible) return;
    if (m_outstandingCredit != 0) return;
    int free = -1;
    for (uint32_t i = 0; i < kSlotCount; ++i) {
        const Entry& e = m_slots[i];
        if (e.generation != m_generation) continue;
        // A frame the host has not painted yet means the view is ahead of the
        // host; the next credit waits for that paint, so the view renders one
        // frame per host paint instead of racing ahead and being superseded
        // (RFC-7 §5: the view never renders frames the host cannot show).
        if (e.state == SlotState::Ready) return;
        if (e.state == SlotState::Free && free < 0) free = int(i);
    }
    if (free < 0) return;

    FrameRequest r;
    r.slot = uint32_t(free);
    r.credit = ++m_creditCounter;
    r.deadlineUs = 16667;
    m_slots[free].state = SlotState::Granted;
    m_grantedSlot = free;
    m_outstandingCredit = r.credit;
    m_grantClock.start();
    if (m_readyClock.isValid()) qCDebug(lcSurfaceHost, "grant slot %d: %.1f ms after the last FrameReady", free, m_readyClock.nsecsElapsed() / 1e6);
    m_channel->send(MessageType::FrameRequest, r.encode(), m_generation);
}

void SurfaceHostSession::onFrameReady(const Header& header, const QByteArray& payload)
{
    FrameReady m;
    if (!FrameReady::decode(payload, &m) || m.slot >= kSlotCount) {
        fail(QStringLiteral("protocolError"), QStringLiteral("bad FrameReady"));
        return;
    }
    if (header.generation != m_generation) {
        // A frame rendered before the view saw the new geometry: give the
        // old slot back (if it still exists on the view side) and ignore.
        if (m_channel->isOpen())
            m_channel->send(MessageType::Release, SlotRef{m.slot}.encode(), header.generation);
        return;
    }
    Entry& e = m_slots[m.slot];
    if (m_outstandingCredit == 0 || m.credit != m_outstandingCredit || int(m.slot) != m_grantedSlot
        || e.state != SlotState::Granted) {
        fail(QStringLiteral("protocolError"), QStringLiteral("FrameReady does not match the outstanding credit"));
        return;
    }
    e.state = SlotState::Ready;
    e.frameIndex = ++m_frameCounter;
    qCDebug(lcSurfaceHost, "frame %llu in slot %u: grant->ready %.1f ms", (unsigned long long)e.frameIndex, m.slot, m_grantClock.nsecsElapsed() / 1e6);
    m_outstandingCredit = 0;
    m_grantedSlot = -1;
    ++m_framesReceived;
    m_readyClock.start();
    emit frameReady();
    // The view may already want the next one.
    maybeGrant();
}

HostSlot* SurfaceHostSession::takeReadyForDisplay()
{
    int newest = -1;
    for (uint32_t i = 0; i < kSlotCount; ++i) {
        const Entry& e = m_slots[i];
        if (e.state == SlotState::Ready && e.generation == m_generation
            && (newest < 0 || e.frameIndex > m_slots[newest].frameIndex))
            newest = int(i);
    }
    if (newest < 0) return nullptr;
    if (!m_slots[newest].slot->acquireForRead()) {
        if (m_slots[newest].slot->isAbandoned()) {
            // Reallocate on the GUI thread; this frame is lost.
            QMetaObject::invokeMethod(this, [this] { if (m_connected) { ++m_generation; sendGeometry(); } },
                                      Qt::QueuedConnection);
        }
        return nullptr;
    }
    // Any other Ready slot was superseded without being shown.
    for (uint32_t i = 0; i < kSlotCount; ++i) {
        if (int(i) != newest && m_slots[i].state == SlotState::Ready) {
            m_slots[i].state = SlotState::Retiring;
        }
    }
    if (m_displayed >= 0 && m_displayed != newest) m_slots[m_displayed].state = SlotState::Retiring;
    m_oldDisplayed.reset();
    m_slots[newest].state = SlotState::Displayed;
    m_displayed = newest;
    // The frame the view was ahead by is being painted: the view may start on
    // the next one now rather than after this paint finishes. Queued, since
    // this runs on the render thread and the socket lives on the session's.
    QMetaObject::invokeMethod(this, &SurfaceHostSession::maybeGrant, Qt::QueuedConnection);
    return m_slots[newest].slot.get();
}

HostSlot* SurfaceHostSession::displayedSlot() const
{
    if (m_displayed >= 0) return m_slots[m_displayed].slot.get();
    return m_oldDisplayed.get();
}

void SurfaceHostSession::releaseSlot(Entry& e)
{
    e.slot->releaseAfterRead();
    e.state = SlotState::Free;
    if (m_channel && m_channel->isOpen()) {
        const uint32_t index = uint32_t(&e - &m_slots[0]);
        m_channel->send(MessageType::Release, SlotRef{index}.encode(), e.generation);
        ++m_releasesSent;
    }
}

void SurfaceHostSession::frameSwapped()
{
    bool freed = false;
    for (Entry& e : m_slots) {
        if (e.state == SlotState::Retiring) { releaseSlot(e); freed = true; }
    }
    if (freed) maybeGrant();
}

// ── Input ──────────────────────────────────────────────────────────────────

void SurfaceHostSession::sendInput(const Input& input)
{
    if (!m_connected || !m_channel) return;
    m_channel->send(MessageType::Input, input.encode(), m_generation);
}

// ── Teardown ───────────────────────────────────────────────────────────────

void SurfaceHostSession::fail(const QString& code, const QString& what)
{
    qCWarning(lcSurfaceHost) << code << ":" << what;
    dropConnection();
    emit error(code);
}

void SurfaceHostSession::onChannelClosed()
{
    // Our own close() sends Bye and tears the channel down; the channel's
    // closed() from that is not the view going away.
    if (!m_socket || m_closed) return;
    const bool wasConnected = m_connected;
    dropConnection();
    if (wasConnected) emit error(QStringLiteral("clientExited"));
}

void SurfaceHostSession::dropConnection()
{
    if (m_helloTimer) { m_helloTimer->stop(); m_helloTimer->deleteLater(); m_helloTimer = nullptr; }
    if (m_channel) {
        m_channel->disconnect(this);
        m_channel->deleteLater();
        m_channel = nullptr;
    }
    if (m_socket) {
        m_socket->disconnect(this);
        m_socket->abort();
        m_socket->deleteLater();
        m_socket = nullptr;
    }
    m_connected = false;
    m_outstandingCredit = 0;
    m_grantedSlot = -1;
    // Keep the displayed slot's storage: the host goes on painting the last
    // frame until the module is re-spawned or the item detaches.
    for (uint32_t i = 0; i < kSlotCount; ++i) {
        Entry& e = m_slots[i];
        if (e.state == SlotState::Unallocated) continue;
        if (int(i) == m_displayed) continue;
        e.slot.reset();
        e.state = SlotState::Unallocated;
    }
    emit transportChanged();
}

void SurfaceHostSession::close()
{
    if (m_closed) return;
    m_closed = true;
    if (m_channel && m_channel->isOpen()) m_channel->close(ByeNormal);
    dropConnection();
    if (m_server) { m_server->close(); m_server->deleteLater(); m_server = nullptr; }
    emit closed();
}

} // namespace logos::surface
