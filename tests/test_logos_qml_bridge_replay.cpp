// Readiness replay against a REAL Valid replica.
//
// replayViewModuleState() exists for QML hot reload: the host rebuilds the view
// on a fresh QQmlEngine while the backend process keeps running, so the new
// object tree's Connections are created after viewModuleReadyChanged already
// fired and the cached replica — Valid, and staying Valid — never transitions
// again to give them a second chance. The view then waits forever for readiness
// it missed.
//
// The unit suite can only reach the negative half of that (empty map, dropped
// map), where the replay loop never meets a Valid replica and so cannot catch a
// regression in the branch that matters. This one publishes a real source over
// QtRO, acquires through the bridge's own factory-plugin path, waits for the
// replica to actually reach Valid, and then pins what replay emits.

#include "LogosQmlBridge.h"

#include <QCoreApplication>
#include <QRemoteObjectHost>
#include <QSignalSpy>
#include <QTest>
#include <QUrl>

namespace {

// The remoted source. A property is not incidental: a source with no members
// still publishes, but giving the replica something to receive keeps this
// close to a real view module's backend.
class TestViewSource : public QObject {
    Q_OBJECT
    Q_PROPERTY(int counter READ counter WRITE setCounter NOTIFY counterChanged)

public:
    int counter() const { return m_counter; }
    void setCounter(int value)
    {
        if (m_counter == value) return;
        m_counter = value;
        emit counterChanged(m_counter);
    }

signals:
    void counterChanged(int value);

private:
    int m_counter = 0;
};

constexpr int kReadyTimeoutMs = 10000;

} // namespace

class TestLogosQmlBridgeReplay : public QObject {
    Q_OBJECT

private slots:
    void replayViewModuleState_validReplica_reemitsReady()
    {
        // Socket name carries the pid so a leftover socket from a crashed run
        // cannot make this test bind to — or acquire from — the wrong host.
        const QString socket =
            QStringLiteral("lvmr-replay-%1").arg(QCoreApplication::applicationPid());

        TestViewSource source;
        QRemoteObjectHost host(QUrl(QStringLiteral("local:") + socket));
        QVERIFY(host.enableRemoting(&source, QStringLiteral("test_view")));

        LogosQmlBridge bridge(nullptr);
        bridge.setViewModuleSocket(QStringLiteral("test_view"), socket);
        bridge.setViewReplicaPlugin(QStringLiteral("test_view"),
                                    QStringLiteral(TEST_REPLICA_FACTORY_PLUGIN));

        QSignalSpy readySpy(&bridge, &LogosQmlBridge::viewModuleReadyChanged);

        QObject* replica = bridge.module(QStringLiteral("test_view"));
        QVERIFY(replica != nullptr);

        // Acquisition is asynchronous: the replica is Default until the source
        // meta arrives over the socket.
        QVERIFY(readySpy.wait(kReadyTimeoutMs));
        QVERIFY(bridge.isViewModuleReady(QStringLiteral("test_view")));

        // ── The reload. A fresh engine asks for the same module and gets the
        // cached replica back; on its own that is silent, which is the bug.
        readySpy.clear();
        QCOMPARE(bridge.module(QStringLiteral("test_view")), replica);
        QCOMPARE(readySpy.count(), 0);

        // ── What the host calls once the new object tree is complete.
        bridge.replayViewModuleState();

        QCOMPARE(readySpy.count(), 1);
        QCOMPARE(readySpy.first().at(0).toString(), QStringLiteral("test_view"));
        QCOMPARE(readySpy.first().at(1).toBool(), true);

        // Replay is a report of current state, not a one-shot: a view reloaded
        // twice must be told twice.
        readySpy.clear();
        bridge.replayViewModuleState();
        QCOMPARE(readySpy.count(), 1);
        QCOMPARE(readySpy.first().at(1).toBool(), true);
    }
};

QTEST_MAIN(TestLogosQmlBridgeReplay)
#include "test_logos_qml_bridge_replay.moc"
