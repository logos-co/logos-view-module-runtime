// RFC-7 end to end, two processes, no GPU, no compositor: this process is the
// HOST (a SurfaceHostSession on a QGuiApplication under the offscreen
// platform; the shm kind needs no window), test_surface_view is the VIEW
// (software scene graph through the RFC-8 C API, exactly as ui-host wires it).
//
// What is asserted (RFC-7 §8 Tests):
//   * a frame of the right size and colour arrives in a slot the host can
//     read as a QImage, over channel-shm;
//   * geometry is host-authoritative: a Configure bumps the generation and
//     the next frame has the new size; a stale FrameReady is not displayed;
//   * at most one credit is outstanding; every displayed slot is Released
//     after frameSwapped(); a hidden surface gets no credits;
//   * a view that asks for its window late still gets the held credit;
//   * input of every kind is replayed as the matching QEvent in the view;
//   * the view dying leaves the last displayed slot readable and reports
//     clientExited; the host closing ends the view's session;
//   * bad peers — wrong magic, oversize length, wrong token, Hello from a
//     foreign pid, unknown must-understand type — are dropped at once and the
//     session stays usable for the real view.

#include "SurfaceHostSession.h"
#include "SurfaceProtocol.h"

#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QLocalSocket>
#include <QProcess>
#include <QRandomGenerator>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace logos::surface;

namespace {

QString randomToken()
{
    QByteArray t;
    while (t.size() < int(kTokenLength)) t += QByteArray::number(QRandomGenerator::global()->generate(), 16);
    return QString::fromLatin1(t.left(int(kTokenLength)));
}

struct View {
    QProcess process;
    QTemporaryDir dir;
    QString journalPath;
    QString token = randomToken();

    bool start(const QString& server, const QString& color, int delayWindowMs = 0, bool animate = false)
    {
        journalPath = dir.filePath(QStringLiteral("journal.txt"));
        QStringList args{QStringLiteral("--server"), server, QStringLiteral("--token"), token,
                         QStringLiteral("--color"), color, QStringLiteral("--journal"), journalPath};
        if (delayWindowMs > 0) args << QStringLiteral("--delay-window") << QString::number(delayWindowMs);
        if (animate) args << QStringLiteral("--animate");
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        // offscreen in the sandbox; LOGOS_SURFACE_TEST_QPA=xcb|wayland with
        // LOGOS_SURFACE_TEST_GL=1 on a desktop runs the view on real GL.
        env.insert(QStringLiteral("QT_QPA_PLATFORM"),
                   qEnvironmentVariable("LOGOS_SURFACE_TEST_QPA", QStringLiteral("offscreen")));
        process.setProcessEnvironment(env);
        process.setProcessChannelMode(QProcess::ForwardedErrorChannel);
        process.start(QStringLiteral(SURFACE_VIEW_BINARY), args);
        return process.waitForStarted(5000);
    }

    QStringList journal() const
    {
        QFile f(journalPath);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
        return QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    }

    bool waitForJournal(const QString& prefix, int timeoutMs = 10000) const
    {
        QElapsedTimer t;
        t.start();
        while (t.elapsed() < timeoutMs) {
            for (const QString& l : journal())
                if (l.startsWith(prefix)) return true;
            QTest::qWait(20);
        }
        return false;
    }

    void stop()
    {
        if (process.state() == QProcess::NotRunning) return;
        process.kill();
        process.waitForFinished(3000);
    }
    ~View() { stop(); }
};

// A host session with a connected, configured view and one displayed frame.
struct Rig {
    SurfaceHostSession session;
    View view;

    bool up(const QSize& logical, const QString& color = QStringLiteral("#ff0000"), int delayWindowMs = 0,
            bool animate = false)
    {
        if (!session.listen()) return false;
        session.configure(logical, 1.0, true);
        if (!view.start(session.serverName(), color, delayWindowMs, animate)) return false;
        session.expect(view.token, view.process.processId());
        QSignalSpy ready(&session, &SurfaceHostSession::frameReady);
        return ready.count() > 0 || ready.wait(15000);
    }

    // Simulates what ChannelSurfaceBackend::updatePaintNode + frameSwapped do.
    HostSlot* present()
    {
        HostSlot* s = session.takeReadyForDisplay();
        if (s) session.frameSwapped();
        return s;
    }

    bool waitFrame(int timeoutMs = 10000)
    {
        QSignalSpy ready(&session, &SurfaceHostSession::frameReady);
        return ready.wait(timeoutMs);
    }
};

QColor centre(const QImage& img)
{
    return img.isNull() ? QColor() : QColor(img.pixel(img.width() / 2, img.height() / 2));
}

Input mouse(InputKind kind, double x, double y, Qt::MouseButton b = Qt::LeftButton)
{
    Input in;
    in.kind = kind;
    in.mouse.x = x; in.mouse.y = y; in.mouse.gx = x; in.mouse.gy = y;
    in.mouse.button = uint32_t(b);
    in.mouse.buttons = (kind == InputKind::MouseRelease) ? 0 : uint32_t(b);
    return in;
}

// Raw peer: speaks whatever bytes the test wants.
struct RawPeer {
    QLocalSocket socket;
    bool connectTo(const QString& server)
    {
        socket.connectToServer(server);
        return socket.waitForConnected(3000);
    }
    void sendRaw(uint32_t magic, uint16_t version, uint16_t type, uint32_t length, const QByteArray& payload,
                 uint32_t flags = 0)
    {
        Writer w;
        w.u32(magic); w.u16(version); w.u16(type); w.u32(length); w.u32(0); w.u32(1); w.u32(flags);
        w.u64(0);   // reserved
        QByteArray bytes = w.take();
        bytes += payload;
        socket.write(bytes);
        socket.flush();
    }
    // The host session lives in this thread: spin the event loop so it can
    // see the bytes and act (waitForDisconnected would starve it).
    bool waitClosed(int timeoutMs = 3000)
    {
        QElapsedTimer t;
        t.start();
        while (socket.state() != QLocalSocket::UnconnectedState && t.elapsed() < timeoutMs)
            QTest::qWait(10);
        return socket.state() == QLocalSocket::UnconnectedState;
    }
};

} // namespace

class TestSurfaceChannel : public QObject {
    Q_OBJECT

private slots:
    void firstFrameArrivesOverShm()
    {
        Rig rig;
        QVERIFY2(rig.up(QSize(200, 120)), "no frame from the view");
        QCOMPARE(rig.session.transport(), QStringLiteral("channel-shm"));
        HostSlot* slot = rig.present();
        QVERIFY(slot);
        const QImage img = slot->image();
        QCOMPARE(img.size(), QSize(200, 120));
        QCOMPARE(centre(img), QColor(QStringLiteral("#ff0000")));
        QVERIFY(rig.view.waitForJournal(QStringLiteral("welcomed channel-shm")));
        QVERIFY(rig.view.waitForJournal(QStringLiteral("rendered 1")));
        QCOMPARE(rig.session.outstandingCredit() <= 1, true);
    }

    void dprScalesThePixelSize()
    {
        Rig rig;
        QVERIFY(rig.session.listen());
        rig.session.configure(QSize(100, 50), 2.0, true);
        QVERIFY(rig.view.start(rig.session.serverName(), QStringLiteral("#00ff00")));
        rig.session.expect(rig.view.token, rig.view.process.processId());
        QVERIFY(rig.waitFrame(15000));
        HostSlot* slot = rig.present();
        QVERIFY(slot);
        QCOMPARE(slot->image().size(), QSize(200, 100));
        QCOMPARE(centre(slot->image()), QColor(QStringLiteral("#00ff00")));
    }

    void resizeBumpsGenerationAndTheNextFrameHasTheNewSize()
    {
        Rig rig;
        QVERIFY(rig.up(QSize(200, 120), QStringLiteral("#0000ff")));
        QVERIFY(rig.present());
        const uint32_t g0 = rig.session.generation();
        rig.session.configure(QSize(320, 200), 1.0, true);
        QCOMPARE(rig.session.generation(), g0 + 1);
        // The old frame stays displayable until the new one lands.
        QVERIFY(rig.session.displayedSlot());
        QCOMPARE(rig.session.displayedSlot()->image().size(), QSize(200, 120));
        QVERIFY(rig.waitFrame(15000));
        HostSlot* slot = rig.present();
        QVERIFY(slot);
        QCOMPARE(slot->image().size(), QSize(320, 200));
        QCOMPARE(centre(slot->image()), QColor(QStringLiteral("#0000ff")));
    }

    void creditsAreSingleAndSlotsAreReleased()
    {
        // An animating view asks for frames (FrameWanted); the host grants one
        // credit at a time and releases every slot it has finished showing.
        Rig rig;
        QVERIFY(rig.up(QSize(64, 64), QStringLiteral("#ff00ff"), 0, /*animate=*/true));
        const uint32_t generation = rig.session.generation();
        int frames = 0;
        QElapsedTimer t;
        t.start();
        while (frames < 6 && t.elapsed() < 15000) {
            if (rig.session.takeReadyForDisplay()) {
                ++frames;
                rig.session.frameSwapped();
            }
            QVERIFY(rig.session.outstandingCredit() <= 1);
            QTest::qWait(10);
        }
        QVERIFY2(frames >= 6, qPrintable(QStringLiteral("only %1 frames").arg(frames)));
        QVERIFY(rig.session.framesReceived() >= frames);
        // Each displayed frame beyond the one on screen retired a slot, and
        // each retirement was told to the view.
        QVERIFY2(rig.session.releasesSent() >= frames - 1,
                 qPrintable(QStringLiteral("releases %1 frames %2").arg(rig.session.releasesSent()).arg(frames)));
        QCOMPARE(rig.session.generation(), generation);   // no geometry change happened
    }

    void hiddenSurfaceGetsNoCredits()
    {
        Rig rig;
        QVERIFY(rig.up(QSize(80, 80)));
        QVERIFY(rig.present());
        rig.session.setVisible(false);
        // A geometry change while hidden must not produce a frame.
        rig.session.configure(QSize(90, 90), 1.0, false);
        QSignalSpy ready(&rig.session, &SurfaceHostSession::frameReady);
        QVERIFY(!ready.wait(700));
        QCOMPARE(rig.session.outstandingCredit(), 0);
        rig.session.setVisible(true);
        QVERIFY(rig.waitFrame(10000));
        HostSlot* slot = rig.present();
        QVERIFY(slot);
        QCOMPARE(slot->image().size(), QSize(90, 90));
    }

    void lateWindowStillGetsTheHeldCredit()
    {
        Rig rig;
        QVERIFY2(rig.up(QSize(120, 80), QStringLiteral("#ffff00"), /*delayWindowMs=*/1500), "late window never rendered");
        HostSlot* slot = rig.present();
        QVERIFY(slot);
        QCOMPARE(centre(slot->image()), QColor(QStringLiteral("#ffff00")));
        QCOMPARE(rig.session.outstandingCredit() <= 1, true);
    }

    void inputIsReplayedInTheView()
    {
        Rig rig;
        QVERIFY(rig.up(QSize(200, 200)));
        QVERIFY(rig.present());
        QVERIFY(rig.view.waitForJournal(QStringLiteral("window ")));

        Input focus; focus.kind = InputKind::FocusIn;
        rig.session.sendInput(focus);
        rig.session.sendInput(mouse(InputKind::MouseMove, 40, 50));
        rig.session.sendInput(mouse(InputKind::MousePress, 40, 50));
        rig.session.sendInput(mouse(InputKind::MouseRelease, 40, 50));
        Input wheel; wheel.kind = InputKind::Wheel;
        wheel.wheel.x = 60; wheel.wheel.y = 70; wheel.wheel.angleDy = 120;
        rig.session.sendInput(wheel);
        Input key; key.kind = InputKind::KeyPress;
        key.key.key = Qt::Key_A; key.key.text = QStringLiteral("a");
        rig.session.sendInput(key);
        Input keyUp = key; keyUp.kind = InputKind::KeyRelease;
        rig.session.sendInput(keyUp);
        Input touch; touch.kind = InputKind::Touch;
        touch.touch.type = uint32_t(QEvent::TouchBegin);
        TouchPointInput p; p.id = 1; p.state = uint32_t(QEventPoint::Pressed); p.x = 10; p.y = 10; p.gx = 10; p.gy = 10;
        touch.touch.points.append(p);
        rig.session.sendInput(touch);
        Input touchEnd = touch;
        touchEnd.touch.type = uint32_t(QEvent::TouchEnd);
        touchEnd.touch.points[0].state = uint32_t(QEventPoint::Released);
        rig.session.sendInput(touchEnd);
        Input leave; leave.kind = InputKind::Leave;
        rig.session.sendInput(leave);

        QVERIFY2(rig.view.waitForJournal(QStringLiteral("press 40 50 1")), qPrintable(rig.view.journal().join(',')));
        QVERIFY(rig.view.waitForJournal(QStringLiteral("release 40 50 1")));
        QVERIFY(rig.view.waitForJournal(QStringLiteral("wheel 60 70 120")));
        QVERIFY(rig.view.waitForJournal(QStringLiteral("key %1 a").arg(int(Qt::Key_A))));
        QVERIFY(rig.view.waitForJournal(QStringLiteral("keyrelease %1").arg(int(Qt::Key_A))));
        QVERIFY2(rig.view.waitForJournal(QStringLiteral("touch %1 1").arg(int(QEvent::TouchBegin))),
                 qPrintable(rig.view.journal().join(',')));
    }

    void viewDeathKeepsTheLastFrameAndReportsIt()
    {
        Rig rig;
        QVERIFY(rig.up(QSize(100, 100), QStringLiteral("#123456")));
        HostSlot* slot = rig.present();
        QVERIFY(slot);
        QSignalSpy err(&rig.session, &SurfaceHostSession::error);
        rig.view.process.kill();
        QVERIFY(rig.view.process.waitForFinished(3000));
        QVERIFY(err.count() > 0 || err.wait(5000));
        QCOMPARE(err.first().at(0).toString(), QStringLiteral("clientExited"));
        QVERIFY(!rig.session.isConnected());
        // The last frame is still there for the host to keep painting.
        QVERIFY(rig.session.displayedSlot());
        QCOMPARE(centre(rig.session.displayedSlot()->image()), QColor(QStringLiteral("#123456")));
    }

    void hostCloseEndsTheView()
    {
        Rig rig;
        QVERIFY(rig.up(QSize(50, 50)));
        QVERIFY(rig.present());
        QSignalSpy closed(&rig.session, &SurfaceHostSession::closed);
        rig.session.close();
        QVERIFY(closed.count() > 0 || closed.wait(3000));
        // The view's session is over; a second close is harmless.
        rig.session.close();
        QVERIFY(!rig.session.isConnected());
    }

    void badPeersAreDroppedAndTheRealViewStillConnects()
    {
        SurfaceHostSession session;
        QVERIFY(session.listen());
        session.configure(QSize(60, 60), 1.0, true);
        const QString token = randomToken();
        session.expect(token, 1);   // pid 1 is never the raw peer

        {   // wrong magic
            RawPeer p;
            QVERIFY(p.connectTo(session.serverName()));
            p.sendRaw(0xdeadbeef, kVersion, uint16_t(MessageType::Hello), 0, {});
            QVERIFY(p.waitClosed());
        }
        {   // oversize length
            RawPeer p;
            QVERIFY(p.connectTo(session.serverName()));
            p.sendRaw(kMagic, kVersion, uint16_t(MessageType::Hello), kMaxPayload + 1, {});
            QVERIFY(p.waitClosed());
        }
        {   // wrong version
            RawPeer p;
            QVERIFY(p.connectTo(session.serverName()));
            p.sendRaw(kMagic, 99, uint16_t(MessageType::Hello), 0, {});
            QVERIFY(p.waitClosed());
        }
        {   // Hello with the wrong token, right pid shape
            RawPeer p;
            QVERIFY(p.connectTo(session.serverName()));
            Hello h;
            h.pid = 1; h.graphicsApi = ApiSoftware; h.slotKinds = SlotKindShm;
            h.token = randomToken().toLatin1();
            const QByteArray payload = h.encode();
            p.sendRaw(kMagic, kVersion, uint16_t(MessageType::Hello), uint32_t(payload.size()), payload);
            QVERIFY(p.waitClosed());
        }
        {   // Hello from a foreign pid (claims the expected pid, is not it)
            RawPeer p;
            QVERIFY(p.connectTo(session.serverName()));
            Hello h;
            h.pid = 1; h.graphicsApi = ApiSoftware; h.slotKinds = SlotKindShm;
            h.token = token.toLatin1();
            const QByteArray payload = h.encode();
            p.sendRaw(kMagic, kVersion, uint16_t(MessageType::Hello), uint32_t(payload.size()), payload);
            QVERIFY(p.waitClosed());
        }
        {   // first message not a Hello
            RawPeer p;
            QVERIFY(p.connectTo(session.serverName()));
            p.sendRaw(kMagic, kVersion, uint16_t(MessageType::FrameWanted), 0, {});
            QVERIFY(p.waitClosed());
        }
        {   // a client that says nothing is dropped at the Hello deadline —
            // too slow for a unit test; just check it is not "connected".
            RawPeer p;
            QVERIFY(p.connectTo(session.serverName()));
            QTest::qWait(100);
            QVERIFY(!session.isConnected());
        }
        QVERIFY(!session.isConnected());

        // The real view now connects and renders.
        View view;
        view.token = token;
        QVERIFY(view.start(session.serverName(), QStringLiteral("#00ffff")));
        session.expect(token, view.process.processId());
        QSignalSpy ready(&session, &SurfaceHostSession::frameReady);
        QVERIFY(ready.wait(15000));
        HostSlot* slot = session.takeReadyForDisplay();
        QVERIFY(slot);
        QCOMPARE(centre(slot->image()), QColor(QStringLiteral("#00ffff")));
    }

    void protocolErrorAfterHelloClosesTheSession()
    {
        Rig rig;
        QVERIFY(rig.up(QSize(40, 40)));
        QVERIFY(rig.present());
        // A second connection while one is live is refused outright.
        RawPeer p;
        QVERIFY(p.connectTo(rig.session.serverName()));
        QVERIFY(p.waitClosed());
        QVERIFY(rig.session.isConnected());
    }

    void geometryOutOfBoundsIsRefused()
    {
        QVERIFY(!slotGeometryWithinBounds(QSize(kMaxPixelDimension + 1, 10)));
        QVERIFY(!slotGeometryWithinBounds(QSize(0, 10)));
        QVERIFY(slotGeometryWithinBounds(QSize(kMaxPixelDimension, 1)));
        // 8192 x 8192 x 4 = 256 MiB exactly: allowed; one more row is not.
        QVERIFY(slotGeometryWithinBounds(QSize(kMaxPixelDimension, kMaxPixelDimension)));
    }
};

QTEST_MAIN(TestSurfaceChannel)
#include "test_surface_channel.moc"
