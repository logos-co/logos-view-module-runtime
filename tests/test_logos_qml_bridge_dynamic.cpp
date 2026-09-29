// View module backends as dynamic replicas, against real published sources.
//
// The host loads no code from a view module: the replica is built from the
// schema the backend sends, and the .rep's enums reach QML through a
// Logos.<Source> singleton that reads them from that replica. What this pins:
//
//   * prepareViewModule() reports ready only once the replica is Valid, and
//     reports a backend that never appears instead of waiting forever.
//   * Enums work from QML the way apps use them — unversioned import, bare
//     and scoped constants, compared against an enum-typed property.
//   * A backend replaced by one whose enum VALUES differ (a reinstall at
//     another version) is seen correctly by the next engine, in the same
//     process. This is the case that used to need a restart. Likewise one
//     whose property layout changed.

#include "LogosQmlBridge.h"
#include "rep_test_rep_backend_source.h"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QRemoteObjectHost>
#include <QSignalSpy>
#include <QTest>
#include <QUrl>

#include <memory>

namespace {

class BackendV1 : public QObject {
    Q_OBJECT
    Q_PROPERTY(State state READ state NOTIFY stateChanged)
public:
    enum State { Idle = 0, Busy = 1, Done = 2 };
    Q_ENUM(State)
    State state() const { return Done; }
signals:
    void stateChanged();
};

// Same enum name and keys, different numbers, plus one more key — and a
// property ahead of `state`, so the property layout differs as well.
class BackendV2 : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString label READ label NOTIFY labelChanged)
    Q_PROPERTY(State state READ state NOTIFY stateChanged)
public:
    enum State { Idle = 0, Busy = 5, Done = 7, Failed = 9 };
    Q_ENUM(State)
    QString label() const { return QStringLiteral("v2"); }
    State state() const { return Done; }
signals:
    void labelChanged();
    void stateChanged();
};

// The same two properties in the opposite order: a reinstall that changes
// the property layout without changing what QML asks for.
class OrderAB : public QObject {
    Q_OBJECT
    Q_PROPERTY(QString alpha READ alpha NOTIFY alphaChanged)
    Q_PROPERTY(int beta READ beta NOTIFY betaChanged)
public:
    QString alpha() const { return QStringLiteral("alpha"); }
    int beta() const { return 42; }
signals:
    void alphaChanged();
    void betaChanged();
};

class OrderBA : public QObject {
    Q_OBJECT
    Q_PROPERTY(int beta READ beta NOTIFY betaChanged)
    Q_PROPERTY(QString alpha READ alpha NOTIFY alphaChanged)
public:
    int beta() const { return 42; }
    QString alpha() const { return QStringLiteral("alpha"); }
signals:
    void betaChanged();
    void alphaChanged();
};

constexpr int kReadyTimeoutMs = 10000;
const QString kSource = QStringLiteral("TestDynBackend");
const QString kModule = QStringLiteral("test_dyn_ui");

QString socketFor(const char* tag)
{
    return QStringLiteral("lvmr-dyn-%1-%2").arg(QLatin1String(tag))
        .arg(QCoreApplication::applicationPid());
}

struct Prepared {
    bool called = false;
    bool ok = false;
    QString error;
};

Prepared prepare(LogosQmlBridge& bridge, int timeoutMs)
{
    auto result = std::make_shared<Prepared>();
    bridge.prepareViewModule(kModule, timeoutMs,
        [result](bool ok, const QString& error) {
            result->called = true;
            result->ok = ok;
            result->error = error;
        });
    // Never synchronous: a host may still be wiring things up when it asks.
    if (result->called) return {true, false, QStringLiteral("called synchronously")};
    QDeadlineTimer deadline(timeoutMs + 5000);
    while (!result->called && !deadline.hasExpired())
        QTest::qWait(10);
    return *result;
}

// Mirrors how apps use .rep enums. The root is a C++-registered holder so the
// test depends on no QML import path — the sandbox has none.
const QByteArray kQml = R"(
import LvmrTest
import Logos.TestDynBackend
Holder {
    property var backend: logos.module("test_dyn_ui")
    property int done: TestDynBackend.Done
    property int scopedDone: TestDynBackend.State.Done
    property bool stateIsDone: backend.state === TestDynBackend.Done
    property bool failedDefined: TestDynBackend.Failed !== undefined
    property var label: backend.label
}
)";

std::unique_ptr<QObject> loadView(QQmlEngine& engine, LogosQmlBridge& bridge)
{
    static const int holderType = qmlRegisterType<QObject>("LvmrTest", 1, 0, "Holder");
    Q_UNUSED(holderType);
    engine.rootContext()->setContextProperty(QStringLiteral("logos"), &bridge);
    QQmlComponent component(&engine);
    component.setData(kQml, QUrl(QStringLiteral("qrc:/view.qml")));
    std::unique_ptr<QObject> view(component.create());
    if (!view) qWarning() << component.errors();
    return view;
}

} // namespace

class TestLogosQmlBridgeDynamic : public QObject {
    Q_OBJECT

private slots:
    void prepare_reportsReadyOnceValid()
    {
        const QString socket = socketFor("ready");
        BackendV1 backend;
        QRemoteObjectHost host(QUrl(QStringLiteral("local:") + socket));
        QVERIFY(host.enableRemoting(&backend, kSource));

        LogosQmlBridge bridge(nullptr);
        bridge.setViewModuleSocket(kModule, socket, kSource);

        const Prepared p = prepare(bridge, kReadyTimeoutMs);
        QVERIFY2(p.ok, qPrintable(p.error));
        QVERIFY(bridge.isViewModuleReady(kModule));
    }

    void prepare_backendNeverAppears_fails()
    {
        LogosQmlBridge bridge(nullptr);
        bridge.setViewModuleSocket(kModule, socketFor("absent"), kSource);

        const Prepared p = prepare(bridge, 300);
        QVERIFY(p.called);
        QVERIFY(!p.ok);
        QVERIFY(!p.error.isEmpty());
    }

    void viewBuiltAfterReady_seesReadyWhenReplayed()
    {
        // Views such as storage_ui hold their content until they see
        // viewModuleReadyChanged(true) — which prepareViewModule() consumed
        // before the view existed.
        const QString socket = socketFor("replay");
        BackendV1 backend;
        QRemoteObjectHost host(QUrl(QStringLiteral("local:") + socket));
        QVERIFY(host.enableRemoting(&backend, kSource));

        LogosQmlBridge bridge(nullptr);
        bridge.setViewModuleSocket(kModule, socket, kSource);
        const Prepared p = prepare(bridge, kReadyTimeoutMs);
        QVERIFY2(p.ok, qPrintable(p.error));

        // A view built now connects too late for the ready edge.
        QSignalSpy readySpy(&bridge, &LogosQmlBridge::viewModuleReadyChanged);
        QTest::qWait(50);
        QCOMPARE(readySpy.count(), 0);

        bridge.replayViewModuleState();
        QCOMPARE(readySpy.count(), 1);
        QCOMPARE(readySpy.first().at(0).toString(), kModule);
        QVERIFY(readySpy.first().at(1).toBool());
    }

    void enums_reachQmlFromTheLiveBackend()
    {
        const QString socket = socketFor("enums");
        BackendV1 backend;
        QRemoteObjectHost host(QUrl(QStringLiteral("local:") + socket));
        QVERIFY(host.enableRemoting(&backend, kSource));

        LogosQmlBridge bridge(nullptr);
        bridge.setViewModuleSocket(kModule, socket, kSource);
        const Prepared p = prepare(bridge, kReadyTimeoutMs);
        QVERIFY2(p.ok, qPrintable(p.error));

        QQmlEngine engine;
        auto view = loadView(engine, bridge);
        QVERIFY(view);
        QCOMPARE(view->property("done").toInt(), 2);
        QCOMPARE(view->property("scopedDone").toInt(), 2);
        QVERIFY(view->property("stateIsDone").toBool());
        QVERIFY(!view->property("failedDefined").toBool());
    }

    void enums_followAReplacedBackend()
    {
        // v1, then v2 with different numbers, then v1 again — each with a
        // fresh bridge and engine, as a reinstall produces.
        for (int round = 0; round < 6; ++round) {
            const bool v2 = (round % 2 == 1);
            const QString socket = socketFor(v2 ? "v2" : "v1") + QString::number(round);

            std::unique_ptr<QObject> backend;
            if (v2) backend = std::make_unique<BackendV2>();
            else    backend = std::make_unique<BackendV1>();
            QRemoteObjectHost host(QUrl(QStringLiteral("local:") + socket));
            QVERIFY(host.enableRemoting(backend.get(), kSource));

            LogosQmlBridge bridge(nullptr);
            bridge.setViewModuleSocket(kModule, socket, kSource);
            const Prepared p = prepare(bridge, kReadyTimeoutMs);
            QVERIFY2(p.ok, qPrintable(p.error));

            QQmlEngine engine;
            auto view = loadView(engine, bridge);
            QVERIFY(view);
            QCOMPARE(view->property("done").toInt(), v2 ? 7 : 2);
            QCOMPARE(view->property("scopedDone").toInt(), v2 ? 7 : 2);
            QVERIFY2(view->property("stateIsDone").toBool(),
                     "the enum-typed property and the constant disagree");
            QCOMPARE(view->property("failedDefined").toBool(), v2);
            QCOMPARE(view->property("label").toString(), v2 ? QStringLiteral("v2") : QString());
        }
    }

    void enums_surviveReopeningARepcBackend()
    {
        // As a host does it: each open is a fresh backend process (here, a
        // fresh source on a fresh socket), a fresh engine, and a bridge owned
        // by that engine; the previous open is fully torn down first.
        static const QByteArray qml = R"(
import LvmrTest
import Logos.TestRepBackend
Holder {
    property var backend: logos.module("test_rep_ui")
    property int done: TestRepBackend.Done
    property bool stateIsDone: backend.state === TestRepBackend.Done
}
)";
        static const int holderType = qmlRegisterType<QObject>("LvmrTest", 1, 0, "Holder");
        Q_UNUSED(holderType);

        for (int round = 0; round < 3; ++round) {
            const QString socket = socketFor("rep") + QString::number(round);
            TestRepBackendSimpleSource backend;
            QRemoteObjectHost host(QUrl(QStringLiteral("local:") + socket));
            QVERIFY(host.enableRemoting<TestRepBackendSourceAPI>(&backend));

            auto engine = std::make_unique<QQmlEngine>();
            auto* bridge = new LogosQmlBridge(nullptr, engine.get());
            bridge->setViewModuleSocket(QStringLiteral("test_rep_ui"), socket,
                                        QStringLiteral("TestRepBackend"));
            auto result = std::make_shared<Prepared>();
            bridge->prepareViewModule(QStringLiteral("test_rep_ui"), kReadyTimeoutMs,
                [result](bool ok, const QString& error) {
                    result->called = true; result->ok = ok; result->error = error;
                });
            QTRY_VERIFY_WITH_TIMEOUT(result->called, kReadyTimeoutMs + 5000);
            QVERIFY2(result->ok, qPrintable(result->error));

            engine->rootContext()->setContextProperty(QStringLiteral("logos"), bridge);
            QQmlComponent component(engine.get());
            component.setData(qml, QUrl(QStringLiteral("qrc:/rep.qml")));
            std::unique_ptr<QObject> view(component.create());
            QVERIFY2(view, qPrintable(component.errorString()));
            QVERIFY2(view->property("done").toInt() == 2,
                     qPrintable(QStringLiteral("round %1: enum constant lost").arg(round)));
            QVERIFY2(view->property("stateIsDone").toBool(),
                     qPrintable(QStringLiteral("round %1: enum property mismatch").arg(round)));

            // Fails if the bridge ever deletes a node before its replica: the
            // replica then re-registers the enum types, and QtRO refuses them
            // to the next connection.
            view.reset();
            engine.reset();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        }
    }

    void properties_followAReorderedBackend()
    {
        // The bridge is a child of the view's engine, as hosts create it, so a
        // replica never outlives the engine that cached its property layout.
        static const QByteArray qml = R"(
import LvmrTest
Holder {
    property var backend: logos.module("test_dyn_ui")
    property var alpha: backend.alpha
    property var beta: backend.beta
}
)";
        static const int holderType = qmlRegisterType<QObject>("LvmrTest", 1, 0, "Holder");
        Q_UNUSED(holderType);

        for (int round = 0; round < 20; ++round) {
            const QString socket = socketFor("order") + QString::number(round);
            std::unique_ptr<QObject> backend;
            if (round % 2) backend = std::make_unique<OrderBA>();
            else           backend = std::make_unique<OrderAB>();
            QRemoteObjectHost host(QUrl(QStringLiteral("local:") + socket));
            QVERIFY(host.enableRemoting(backend.get(), kSource));

            auto engine = std::make_unique<QQmlEngine>();
            auto* bridge = new LogosQmlBridge(nullptr, engine.get());
            bridge->setViewModuleSocket(kModule, socket, kSource);
            const Prepared p = prepare(*bridge, kReadyTimeoutMs);
            QVERIFY2(p.ok, qPrintable(p.error));

            engine->rootContext()->setContextProperty(QStringLiteral("logos"), bridge);
            QQmlComponent component(engine.get());
            component.setData(qml, QUrl(QStringLiteral("qrc:/order.qml")));
            std::unique_ptr<QObject> view(component.create());
            QVERIFY2(view, qPrintable(component.errorString()));
            QCOMPARE(view->property("alpha").toString(), QStringLiteral("alpha"));
            QCOMPARE(view->property("beta").toInt(), 42);

            view.reset();
            engine.reset();
            QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        }
    }
};

QTEST_MAIN(TestLogosQmlBridgeDynamic)
#include "test_logos_qml_bridge_dynamic.moc"
