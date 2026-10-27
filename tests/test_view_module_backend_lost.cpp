// A view whose backend runs as a core module reaches it over an authenticated
// local socket. When that backend's process dies, the view must be told
// (viewModuleCrashed) instead of waiting on "Connecting to backend..." forever,
// and a connection the host drops on purpose (a new socket, a teardown) must
// not be reported as a crash.
#include "LogosQmlBridge.h"

#include <QCoreApplication>
#include <QLocalServer>
#include <QLocalSocket>
#include <QRemoteObjectHost>
#include <QSignalSpy>
#include <QTest>
#include <QUrl>
#include <QUuid>

namespace {

class Source : public QObject {
    Q_OBJECT
    Q_PROPERTY(int value READ value CONSTANT)
public:
    int value() const { return 7; }
};

// Speaks the backend side of the handshake the way LogosViewChannel does:
// "<secret>\n" -> "ok\n", then the socket belongs to QtRO.
class FakeBackend {
public:
    FakeBackend()
        : name(QStringLiteral("lv_test_") + QUuid::createUuid().toString(QUuid::Id128).left(8))
    {
        host.setHostUrl(QUrl(QStringLiteral("logosview:") + name),
                        QRemoteObjectHost::AllowExternalRegistration);
        host.enableRemoting(&source, QStringLiteral("Backend"));
        QObject::connect(&server, &QLocalServer::newConnection, &server, [this]() {
            while (QLocalSocket* socket = server.nextPendingConnection()) {
                QObject::connect(socket, &QLocalSocket::readyRead, socket, [this, socket]() {
                    if (!socket->canReadLine()) return;
                    QObject::disconnect(socket, &QLocalSocket::readyRead, socket, nullptr);
                    if (socket->readLine().trimmed() != secret) { socket->abort(); return; }
                    socket->write("ok\n");
                    socket->flush();
                    host.addHostSideConnection(socket);
                    connections << socket;
                });
            }
        });
        server.listen(name);
    }

    // What the view sees when the backend's process exits.
    void die()
    {
        for (QLocalSocket* socket : connections) socket->abort();
        server.close();
    }

    const QByteArray secret = "0123456789abcdef0123456789abcdef";
    QString name;
    QLocalServer server;
    QRemoteObjectHost host;
    Source source;
    QList<QLocalSocket*> connections;
};

bool connectView(LogosQmlBridge& bridge, const QString& module)
{
    bool done = false, ok = false;
    bridge.prepareViewModule(module, 5000, [&](bool success, const QString&) {
        done = true;
        ok = success;
    });
    for (int waited = 0; !done && waited < 6000; waited += 20) QTest::qWait(20);
    return done && ok;
}

} // namespace

class TestViewModuleBackendLost : public QObject {
    Q_OBJECT
private slots:
    void backendDies_viewIsTold()
    {
        FakeBackend backend;
        LogosQmlBridge bridge(nullptr);
        bridge.setViewModuleSocket(QStringLiteral("app"), backend.name, QStringLiteral("Backend"),
                                   QString::fromLatin1(backend.secret));
        QVERIFY2(connectView(bridge, QStringLiteral("app")),
                 "control: the view never connected to the fake backend");

        QSignalSpy crashed(&bridge, &LogosQmlBridge::viewModuleCrashed);
        QSignalSpy ready(&bridge, &LogosQmlBridge::viewModuleReadyChanged);
        backend.die();

        QTRY_COMPARE_WITH_TIMEOUT(crashed.size(), 1, 5000);
        QCOMPARE(crashed.first().at(0).toString(), QStringLiteral("app"));
        QVERIFY(!ready.isEmpty());
        QCOMPARE(ready.last().at(1).toBool(), false);
    }

    void hostReplacesSocket_isNotACrash()
    {
        FakeBackend first;
        FakeBackend second;
        LogosQmlBridge bridge(nullptr);
        bridge.setViewModuleSocket(QStringLiteral("app"), first.name, QStringLiteral("Backend"),
                                   QString::fromLatin1(first.secret));
        QVERIFY(connectView(bridge, QStringLiteral("app")));

        QSignalSpy crashed(&bridge, &LogosQmlBridge::viewModuleCrashed);
        // A new endpoint drops the old connection on purpose.
        bridge.setViewModuleSocket(QStringLiteral("app"), second.name, QStringLiteral("Backend"),
                                   QString::fromLatin1(second.secret));
        first.die();
        QTest::qWait(500);
        QCOMPARE(crashed.size(), 0);
    }
};

QTEST_GUILESS_MAIN(TestViewModuleBackendLost)
#include "test_view_module_backend_lost.moc"
