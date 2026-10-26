#pragma once

// RFC-7: the view end of one channel session. Connects to the host's
// QLocalServer, authenticates with the RFC-2 token, opens the slots the host
// attaches, renders on credit through SurfaceProducer and replays input.

#include "SlotStorage.h"
#include "SurfaceExport.h"
#include "SurfaceProtocol.h"

#include <QElapsedTimer>
#include <QObject>
#include <QString>
#include <array>
#include <memory>

class QLocalSocket;
class QQuickWindow;

namespace logos::surface {

class SurfaceChannel;
class SurfaceProducer;

class LOGOS_VIEW_SURFACE_EXPORT SurfaceClientSession : public QObject {
    Q_OBJECT
public:
    explicit SurfaceClientSession(QObject* parent = nullptr);
    ~SurfaceClientSession() override;

    // Connects and sends Hello. Returns false when the connection could not
    // even be attempted; later failures arrive as closed().
    bool connectToHost(const QString& serverName, const QString& token);

    // RFC-8 window(): the producer's window, created on first call.
    QQuickWindow* window();
    QString transport() const;
    bool isWelcomed() const { return m_welcomed; }
    SurfaceProducer* producer() const { return m_producer; }

    // Test seams.
    uint32_t generation() const { return m_generation; }
    int framesSent() const { return m_framesSent; }

signals:
    void welcomed();
    void configured(const QSize& logicalSize, qreal dpr, bool visible);
    void inputReplayed(const logos::surface::Input& input);
    void closed();

private:
    struct SlotEntry {
        SlotDescriptor descriptor;
        std::unique_ptr<ViewSlot> slot;
        bool attached = false;
        bool opened = false;
    };

    void onMessage(const Header& header, const QByteArray& payload);
    void onDemand();
    void tryRender();
    bool openSlot(SlotEntry& e);
    void resetSlots();

    QLocalSocket* m_socket = nullptr;
    SurfaceChannel* m_channel = nullptr;
    SurfaceProducer* m_producer = nullptr;
    QString m_token;
    bool m_welcomed = false;
    bool m_closed = false;
    uint32_t m_kind = SlotKindNone;
    bool m_kindRejected = false;

    uint32_t m_generation = 0;
    bool m_visible = false;
    std::array<SlotEntry, kSlotCount> m_slots;

    bool m_haveCredit = false;
    uint32_t m_credit = 0;
    uint32_t m_creditSlot = 0;
    uint32_t m_creditGeneration = 0;
    uint32_t m_renderedGeneration = 0;  // generation of the last frame sent; 0 = none yet
    QElapsedTimer m_creditClock;      // debug: FrameRequest received -> FrameReady sent
    bool m_wantedSent = false;
    uint64_t m_frameIndex = 0;
    int m_framesSent = 0;
};

} // namespace logos::surface
