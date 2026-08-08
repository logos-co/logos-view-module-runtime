// Regression guard: a QML plugin subscribing in Component.onCompleted, BEFORE
// the core module it depends on is reachable.
//
// This is the bug. `logos.onModuleEvent(...)` is documented (LogosQmlBridge.h)
// to be called from Component.onCompleted, but the view is built while the
// module's registry connection is still in flight -- basecamp's PluginLoader
// loads the core deps immediately before the QML view, and loadModule()
// returns as soon as the subprocess is SPAWNED, not once its logos_host has
// called listen(). onModuleEvent used to guard on `client->isConnected()` and
// bail with a single qWarning, one shot, no retry: the subscription was never
// attempted again for the life of the process. Method CALLS kept working
// because they reach the replica through acquireCachedObject(), which skips
// that guard -- so the failure looked like "events are broken", not "the
// subscription never happened".
//
// It only became DETERMINISTIC with logos-protocol 1238316, which made
// isConnected() truthful (it now probes the endpoint for a listener instead of
// returning a latch set by connectToNode()). That fix is correct and must NOT
// be reverted -- the stale latch cost ~417 s of blocked GUI thread on macOS and
// 361 s on Linux at basecamp startup. The subscription has to become
// deferrable instead.
//
// Everything below uses only the pre-existing public surface (onModuleEvent's
// bool + the moduleEventReceived signal), so this exact file compiles against
// the un-fixed tree and reports the defect rather than a build error.
#include "LogosQmlBridge.h"

#include "logos_api.h"                 // LogosAPI, getTokenManager
#include "logos_api_client.h"          // LogosAPIClient::cancelEventSubscription
#include "token_manager.h"             // TokenManager::saveToken
#include "logos_provider_interface.h"  // LogosProviderObject
#include "module_proxy.h"              // ModuleProxy
#include "remote_transport.h"          // RemoteTransportHost
#include "logos_instance.h"            // LogosInstance
#include "logos_mode.h"                // LogosModeConfig / LogosMode

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QSignalSpy>
#include <QString>
#include <QTest>
#include <QVariant>
#include <QVariantList>

#include <memory>

namespace {

class EchoProvider : public LogosProviderObject {
public:
    // ModuleProxy installs this in its constructor, so it is live from the
    // moment the proxy exists -- the test can fire an event at will.
    EventCallback emitFn;

    QVariant callMethod(const QString& method, const QVariantList& args) override {
        if (method == QLatin1String("echo") && !args.isEmpty())
            return args.first();
        return QVariant();
    }
    bool informModuleToken(const QString&, const QString&) override { return true; }
    QJsonArray getMethods() override { return QJsonArray{}; }
    void setEventListener(EventCallback cb) override { emitFn = std::move(cb); }
    void init(void*) override {}
    QString providerName() const override { return QStringLiteral("echo_module"); }
    QString providerVersion() const override { return QStringLiteral("1.0.0"); }
};

// The provider side, constructed on demand so a test can decide WHEN the
// module starts listening. Every case uses its own module name: the registry
// URL is derived from a process-global LogosInstance::id(), so two cases
// sharing a name would share a socket and the second could attach to the
// first's still-listening host and pass for the wrong reason.
struct Publisher {
    EchoProvider echo;
    ModuleProxy proxy;
    RemoteTransportHost host;

    explicit Publisher(const QString& moduleName)
        : proxy(&echo)
        , host(LogosInstance::id(moduleName))
    {
        proxy.saveToken(QStringLiteral("caller"), QStringLiteral("tok"));
        host.publishObject(moduleName, &proxy);
    }
};

void pump(int ms)
{
    QElapsedTimer t; t.start();
    while (t.elapsed() < ms)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
}

// Fire `eventName` repeatedly until the bridge delivers it or the budget runs
// out. Repetition is deliberate: an event is NOT buffered by Qt Remote Objects
// -- one fired before the replica connects is simply gone -- so a single shot
// would make this test a race on when the reconnect timer happens to land.
// Returns true if at least one delivery was observed.
bool fireUntilDelivered(EchoProvider& echo, const QString& eventName,
                        QSignalSpy& spy, int budgetMs)
{
    QElapsedTimer t; t.start();
    while (t.elapsed() < budgetMs) {
        if (echo.emitFn)
            echo.emitFn(eventName, QVariantList() << 42);
        pump(100);
        if (!spy.isEmpty())
            return true;
    }
    return false;
}

} // namespace

class TestLogosQmlBridgeDeferred : public QObject {
    Q_OBJECT
private slots:

    // ── CONTROL ────────────────────────────────────────────────────────────
    // Identical to the primary case except the module is published FIRST.
    // This must be GREEN both before and after the fix. Without it a red
    // primary case proves nothing: it could be red because the fixture is
    // mis-wired (wrong socket, event never emitted, spy on the wrong signal)
    // rather than because of the defect under test.
    void publishBeforeSubscribe_deliversImmediately()
    {
        const QString mod = QStringLiteral("echo_ctrl_module");
        LogosModeConfig::setMode(LogosMode::Remote);

        Publisher pub(mod);

        LogosAPI api(QStringLiteral("caller"));
        api.getTokenManager()->saveToken(mod, QStringLiteral("tok"));
        LogosQmlBridge bridge(&api);

        const bool accepted = bridge.onModuleEvent(mod, QStringLiteral("ev0"));
        QVERIFY2(accepted, "control: subscribing to an ALREADY-published module was refused "
                           "-- the fixture is broken, not the code under test");

        QSignalSpy spy(&bridge, &LogosQmlBridge::moduleEventReceived);
        QVERIFY2(fireUntilDelivered(pub.echo, QStringLiteral("ev0"), spy, 10000),
                 "control: event never reached the bridge even though the module was "
                 "published before the subscription -- fixture broken");

        QCOMPARE(spy.first().at(0).toString(), mod);
        QCOMPARE(spy.first().at(1).toString(), QStringLiteral("ev0"));
        QCOMPARE(spy.first().at(2).toList().value(0).toInt(), 42);
    }

    // ── PRIMARY REGRESSION ─────────────────────────────────────────────────
    // Subscribe while the module is NOT reachable, then bring it up. The
    // subscription must survive and arm.
    //
    // BEFORE THE FIX this fails twice over: onModuleEvent returns false at the
    // isConnected() guard, and no event is ever delivered.
    void subscribeBeforePublish_deliversAfterPublish()
    {
        const QString mod = QStringLiteral("echo_deferred_module");
        LogosModeConfig::setMode(LogosMode::Remote);

        LogosAPI api(QStringLiteral("caller"));
        api.getTokenManager()->saveToken(mod, QStringLiteral("tok"));
        LogosQmlBridge bridge(&api);

        // Nothing is listening on this module's socket yet -- exactly the
        // Component.onCompleted window.
        //
        // Deliberately recorded and checked LAST: the return value is the
        // smaller half of the defect. Asserting it here would abort the test
        // function before the delivery check, and "the event never arrived" is
        // the failure that actually matters.
        const bool accepted = bridge.onModuleEvent(mod, QStringLiteral("ev0"));

        // Let the un-reachable state persist for a while, so a fix that merely
        // happened to win a startup race would not pass.
        pump(500);

        // NOW the module loads (this is what a mid-session package install
        // looks like from the bridge's point of view).
        Publisher pub(mod);

        QSignalSpy spy(&bridge, &LogosQmlBridge::moduleEventReceived);
        QVERIFY2(fireUntilDelivered(pub.echo, QStringLiteral("ev0"), spy, 15000),
                 "event from a module that appeared AFTER the subscription was never "
                 "delivered -- the deferred subscription never armed");

        QCOMPARE(spy.first().at(0).toString(), mod);
        QCOMPARE(spy.first().at(1).toString(), QStringLiteral("ev0"));
        QCOMPARE(spy.first().at(2).toList().value(0).toInt(), 42);

        QVERIFY2(accepted,
                 "onModuleEvent refused a subscription for a module that is not "
                 "reachable YET -- callers cannot tell 'wait for it' from 'never'");
    }

    // ── IDEMPOTENCY ────────────────────────────────────────────────────────
    // QML re-runs Component.onCompleted on a view reload, so onModuleEvent can
    // legitimately be called twice for the same (module, event). One emission
    // must reach the QML side, not two.
    //
    // BEFORE THE FIX this fails with 2: each call took the UNCACHED
    // requestObject() path, so each got its own QRemoteObjectDynamicReplica and
    // its own event helper (and leaked both).
    //
    // Publishes first on purpose -- this case is about duplicate subscriptions,
    // not about deferral, and must fail for exactly one reason.
    void duplicateSubscribe_deliversOnce()
    {
        const QString mod = QStringLiteral("echo_dup_module");
        LogosModeConfig::setMode(LogosMode::Remote);

        Publisher pub(mod);

        LogosAPI api(QStringLiteral("caller"));
        api.getTokenManager()->saveToken(mod, QStringLiteral("tok"));
        LogosQmlBridge bridge(&api);

        bridge.onModuleEvent(mod, QStringLiteral("ev0"));
        bridge.onModuleEvent(mod, QStringLiteral("ev0"));

        // Prove both subscriptions are live before counting, so a count of 1
        // cannot be "the second one had not armed yet".
        QSignalSpy warmup(&bridge, &LogosQmlBridge::moduleEventReceived);
        QVERIFY2(fireUntilDelivered(pub.echo, QStringLiteral("ev0"), warmup, 10000),
                 "duplicate-subscribe fixture never delivered anything at all");
        pump(500);

        QSignalSpy spy(&bridge, &LogosQmlBridge::moduleEventReceived);
        pub.echo.emitFn(QStringLiteral("ev0"), QVariantList() << 42);
        pump(1000);

        QCOMPARE(spy.count(), 1);
    }

    // ── THE DE-DUPE MUST BE VERIFIED, NOT ASSUMED ──────────────────────────
    // The idempotency above is what makes the failure below possible: if the
    // bridge short-circuits on its own record without checking whether the
    // subscription is still live, a view that re-subscribes after the
    // subscription was dropped underneath it gets `true` and nothing else --
    // a permanently silent success, which is the exact shape of the original
    // bug wearing the fix's clothes.
    //
    // Cancelling through the client is the reachable way to make the record
    // stale; the same happens whenever the layer below stops tracking it.
    void staleSubscriptionRecord_reSubscribeReArms()
    {
        const QString mod = QStringLiteral("echo_stale_module");
        LogosModeConfig::setMode(LogosMode::Remote);

        LogosAPI api(QStringLiteral("caller"));
        api.getTokenManager()->saveToken(mod, QStringLiteral("tok"));
        LogosQmlBridge bridge(&api);

        // Subscribe while the module is absent, so the subscription is PENDING
        // and nothing is attached to a handle yet. That matters: cancelling an
        // ARMED subscription deliberately leaves its callback on the shared
        // handle, so events would keep arriving and the test would pass for the
        // wrong reason.
        QVERIFY(bridge.onModuleEvent(mod, QStringLiteral("ev0")));

        LogosAPIClient* client = api.getClient(mod);
        QVERIFY(client != nullptr);
        QVERIFY2(!client->pendingEventSubscriptions().isEmpty(),
                 "control: the subscription has to BE pending for this to test anything");

        // Drop it behind the bridge's back, leaving the bridge's own record
        // claiming it is still subscribed. Ids are per-consumer and start at 1,
        // and this client is fresh, so this is the subscription just made --
        // asserted rather than assumed.
        QVERIFY2(client->cancelEventSubscription(1),
                 "control: id 1 was not the subscription just made -- fixture assumption broken");
        QVERIFY(client->pendingEventSubscriptions().isEmpty());

        Publisher pub(mod);

        // Re-subscribing must ARM again rather than being swallowed as a
        // duplicate. Proven by DELIVERY, not by the return value: `true` is
        // exactly what the trusting-the-record version also returns, which is
        // what made the failure silent.
        QVERIFY(bridge.onModuleEvent(mod, QStringLiteral("ev0")));
        QSignalSpy spy(&bridge, &LogosQmlBridge::moduleEventReceived);
        QVERIFY2(fireUntilDelivered(pub.echo, QStringLiteral("ev0"), spy, 10000),
                 "re-subscribing after the record went stale was swallowed as a "
                 "duplicate: onModuleEvent returned true and armed nothing");
    }

    // ── NON-BLOCKING GUARD ─────────────────────────────────────────────────
    // Constraint: nothing on this path may block the GUI thread. The pre-1238316
    // behaviour fell through to requestObject() and sat in waitForSource() for
    // up to 20 s against a module that was not there. Deferral must not
    // reintroduce that by "just calling requestObject() from the retry".
    //
    // Passes before AND after the fix -- it is a guard, not a reproduction.
    void onModuleEvent_unreachableModule_returnsWithoutBlocking()
    {
        const QString mod = QStringLiteral("echo_never_module");
        LogosModeConfig::setMode(LogosMode::Remote);

        LogosAPI api(QStringLiteral("caller"));
        LogosQmlBridge bridge(&api);

        QElapsedTimer t; t.start();
        bridge.onModuleEvent(mod, QStringLiteral("ev0"));
        const qint64 elapsed = t.elapsed();

        QVERIFY2(elapsed < 2000,
                 qPrintable(QStringLiteral("onModuleEvent blocked the calling thread for %1 ms "
                                           "against an absent module").arg(elapsed)));
    }
};

QTEST_GUILESS_MAIN(TestLogosQmlBridgeDeferred)
#include "test_logos_qml_bridge_deferred.moc"
