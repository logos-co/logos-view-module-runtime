#pragma once

// RFC-7: the host end of one channel session. Owns the QLocalServer, the
// single accepted connection, the slot ring, the geometry generation, the
// one outstanding credit and the input sender. Lives on the GUI thread.
//
// Two methods are called from the scene graph's render thread while the GUI
// thread is blocked in the sync phase (i.e. from QQuickItem::updatePaintNode):
// takeReadyForDisplay() and displayedSlot(). Everything else is GUI-thread
// only; in particular frameSwapped() must be invoked with a queued connection
// from QQuickWindow::frameSwapped, which fires on the render thread while the
// GUI thread is running.

#include "SlotStorage.h"
#include "SurfaceExport.h"
#include "SurfaceProtocol.h"

#include <QObject>
#include <QPointer>
#include <QSize>
#include <QString>
#include <array>
#include <memory>

class QLocalServer;
class QLocalSocket;
class QQuickWindow;
class QTimer;

namespace logos::surface {

class SurfaceChannel;

class LOGOS_VIEW_SURFACE_EXPORT SurfaceHostSession : public QObject {
    Q_OBJECT
public:
    explicit SurfaceHostSession(QObject* parent = nullptr);
    ~SurfaceHostSession() override;

    // Starts listening on a fresh random name (UserAccessOption). False when
    // the server could not be created.
    bool listen();
    QString serverName() const;

    // The spawn this session is for. Must be set before the view can connect.
    void expect(const QString& token, qint64 pid);

    // The host window whose scene graph will sample the slots. Needed before
    // the first Configure for native kinds; shm works without it.
    void setHostWindow(QQuickWindow* window);
    // LOGOS_SURFACE_TRANSPORT=shm: only ever negotiate shm.
    void setForcedKind(uint32_t kind) { m_forcedKind = kind; }

    // Host-authoritative geometry (RFC-6 §1 c). A change bumps the generation
    // and reallocates the ring.
    void configure(const QSize& logicalSize, qreal devicePixelRatio, bool visible);
    void setVisible(bool visible);
    bool isVisible() const { return m_visible; }

    void sendInput(const Input& input);

    bool isConnected() const { return m_connected; }
    uint32_t slotKind() const { return m_kind; }
    QString transport() const;          // "channel-shm" | ... | "none"
    uint32_t generation() const { return m_generation; }
    QSize logicalSize() const { return m_logicalSize; }
    QSize pixelSize() const { return m_pixelSize; }   // the ring's slot size

    // Render thread (GUI blocked). Moves the newest Ready slot to Displayed
    // and returns it (nullptr when there is nothing new). The previously
    // displayed slot becomes Retiring and is released after frameSwapped().
    HostSlot* takeReadyForDisplay();
    HostSlot* displayedSlot() const;

    // GUI thread, queued from QQuickWindow::frameSwapped: retires slots and
    // sends Release for them.
    void frameSwapped();

    // Orderly end: Bye, socket closed, server stopped. Safe to call twice.
    void close();

    // Test seams. outstandingCredit() is the NUMBER of credits in flight,
    // which the protocol caps at one.
    int outstandingCredit() const { return m_outstandingCredit != 0 ? 1 : 0; }
    int releasesSent() const { return m_releasesSent; }
    int framesReceived() const { return m_framesReceived; }

signals:
    void connected();
    void frameReady();                       // a new frame can be displayed
    void transportChanged();
    void error(const QString& code);         // "protocolError" | "clientExited" | "allocationFailed"
    void closed();

private:
    enum class SlotState { Unallocated, Free, Granted, Ready, Displayed, Retiring };
    struct Entry {
        std::unique_ptr<HostSlot> slot;
        SlotState state = SlotState::Unallocated;
        uint32_t generation = 0;
        uint64_t frameIndex = 0;
    };

    void onNewConnection();
    void onMessage(const Header& header, const QByteArray& payload);
    void onHello(const QByteArray& payload);
    void onFrameReady(const Header& header, const QByteArray& payload);
    void onChannelClosed();
    void fail(const QString& code, const QString& what);

    uint32_t chooseKind(uint32_t viewKinds, uint32_t viewApi) const;
    bool allocateRing();
    void sendGeometry();
    void maybeGrant();
    void releaseSlot(Entry& e);
    void dropConnection();

    QLocalServer* m_server = nullptr;
    QLocalSocket* m_socket = nullptr;
    SurfaceChannel* m_channel = nullptr;
    QTimer* m_helloTimer = nullptr;
    QPointer<QQuickWindow> m_hostWindow;

    QString m_expectedToken;
    qint64 m_expectedPid = 0;
    bool m_connected = false;
    bool m_closed = false;
    uint32_t m_kind = SlotKindNone;
    uint32_t m_forcedKind = SlotKindNone;
    uint32_t m_viewKinds = 0;
    uint32_t m_viewApi = ApiUnknown;

    QSize m_logicalSize;
    QSize m_pixelSize;
    qreal m_dpr = 1.0;
    bool m_visible = false;
    bool m_configured = false;
    uint32_t m_generation = 0;
    bool m_geometryDirty = false;

    std::array<Entry, kSlotCount> m_slots;
    std::unique_ptr<HostSlot> m_oldDisplayed;   // last frame of a previous generation
    int m_displayed = -1;                        // index into m_slots, or -1

    uint32_t m_creditCounter = 0;
    uint32_t m_outstandingCredit = 0;
    int m_grantedSlot = -1;
    bool m_wanted = false;
    bool m_shownOnce = false;
    uint64_t m_frameCounter = 0;

    int m_releasesSent = 0;
    int m_framesReceived = 0;
};

} // namespace logos::surface
