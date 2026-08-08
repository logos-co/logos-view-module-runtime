// The CALL path against a module that is not reachable yet.
//
// Its sibling test_logos_qml_bridge_deferred.cpp covers the same startup race
// for event subscriptions. This file covers calls, which had the identical
// guard (`if (!client->isConnected())`) and, until now, no test at all in
// either direction — the guard was dead code for years, so nothing exercised
// what happened when it went live.
//
// The two forms cannot behave the same way, and pinning that difference is the
// point of this file:
//
//   callModule()       is SYNCHRONOUS. It owes the caller a return value now,
//                      so it cannot wait for a module. It waits a short
//                      BOUNDED time and then answers with an error. What it
//                      must never do is sit in the transport's full acquire
//                      budget (20 s, paid twice) on the GUI thread.
//
//   callModuleAsync()  owes the caller a CALLBACK, so it can wait properly. A
//                      module that is still starting is not an error: the call
//                      is held and dispatched when the module appears, bounded
//                      by the deadline the caller already passes.
//
// Every case that asserts a success has a control that is green independently
// of this change, and every case that asserts a failure asserts the SHAPE of
// the failure, because "some error payload" is exactly what the broken version
// returned too.

#include "LogosQmlBridge.h"

#include "logos_api.h"
#include "logos_api_client.h"
#include "token_manager.h"
#include "logos_provider_interface.h"
#include "module_proxy.h"
#include "remote_transport.h"
#include "logos_instance.h"
#include "logos_mode.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJSEngine>
#include <QJSValue>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QString>
#include <QAtomicInt>
#include <QTest>
#include <QThread>
#include <QTimer>
#include <QVariantList>

#include <memory>

namespace {

class EchoProvider : public LogosProviderObject {
public:
    EventCallback emitFn;
    // Observed by the never-dispatched test: asserting on the callback alone
    // cannot tell "suppressed" from "never sent", and only the second is
    // at-most-once.
    QAtomicInt callCount{0};
    int sleepMs = 0;                   // simulate a legitimately slow method
    QVariant callMethod(const QString& method, const QVariantList& args) override {
        callCount.fetchAndAddRelaxed(1);
        if (sleepMs > 0) QThread::msleep(static_cast<unsigned long>(sleepMs));
        if (method == QLatin1String("echo") && !args.isEmpty()) return args.first();
        return QVariant();
    }
    bool informModuleToken(const QString&, const QString&) override { return true; }
    QJsonArray getMethods() override { return QJsonArray{}; }
    void setEventListener(EventCallback cb) override { emitFn = std::move(cb); }
    void init(void*) override {}
    QString providerName() const override { return QStringLiteral("echo_module"); }
    QString providerVersion() const override { return QStringLiteral("1.0.0"); }
};

// The module, brought up on demand so a test controls WHEN it starts listening.
struct Publisher {
    EchoProvider echo;
    ModuleProxy proxy;
    RemoteTransportHost host;
    explicit Publisher(const QString& moduleName)
        : proxy(&echo), host(LogosInstance::id(moduleName))
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

QJsonObject asObject(const QString& payload)
{
    return QJsonDocument::fromJson(payload.toUtf8()).object();
}

} // namespace

class TestLogosQmlBridgeCalls : public QObject {
    Q_OBJECT

private:
    // Drive callModuleAsync through a real QJSEngine, because its callback is a
    // QJSValue and a non-callable one is silently ignored — a C++ lambda would
    // not exercise the path QML actually takes.
    struct AsyncRun {
        QJSEngine engine;
        void arm(LogosQmlBridge* bridge)
        {
            engine.setObjectOwnership(bridge, QJSEngine::CppOwnership);
            engine.globalObject().setProperty(QStringLiteral("logos"),
                                              engine.newQObject(bridge));
            engine.evaluate(QStringLiteral("var __payload = null;"));
        }
        void call(const QString& module, int timeoutMs)
        {
            engine.evaluate(QStringLiteral(
                "logos.callModuleAsync(\"%1\", \"echo\", [7], "
                "function (p) { __payload = p; }, %2);").arg(module).arg(timeoutMs));
        }
        bool fired() { return !engine.evaluate(QStringLiteral("__payload")).isNull(); }
        QString payload() { return engine.evaluate(QStringLiteral("__payload")).toString(); }
        bool waitFired(int budgetMs)
        {
            QElapsedTimer t; t.start();
            while (t.elapsed() < budgetMs) { pump(50); if (fired()) return true; }
            return false;
        }
    };

private slots:

    // ── CONTROL ─────────────────────────────────────────────────────────────
    // Module already up. Green before and after; if this ever fails, the
    // fixture is broken and nothing else in the file means anything.
    void syncCall_moduleUp_returnsResult()
    {
        const QString mod = QStringLiteral("call_ctrl_module");
        LogosModeConfig::setMode(LogosMode::Remote);
        Publisher pub(mod);

        LogosAPI api(QStringLiteral("caller"));
        api.getTokenManager()->saveToken(mod, QStringLiteral("tok"));
        LogosQmlBridge bridge(&api);

        const QString payload = bridge.callModule(mod, QStringLiteral("echo"),
                                                  QVariantList() << 7);
        QVERIFY2(!asObject(payload).contains(QStringLiteral("error")),
                 qPrintable(QStringLiteral("control: call to an already-up module failed: %1")
                                .arg(payload)));
        QCOMPARE(payload.trimmed(), QStringLiteral("7"));
    }

    // ── THE REGRESSION callModule MUST NOT HAVE ─────────────────────────────
    // A synchronous call cannot wait for the module, but it must not sit in the
    // transport's full acquire budget either. The pre-guard behaviour blocked
    // the GUI thread for 20 s (twice over — the token handshake tries
    // capability_module first); ~417 s was measured in Basecamp.
    //
    // Bounded, AND the error says which problem it is: "Module not reachable
    // yet" rather than the generic payload, so a view can tell "still starting"
    // from "answered with nothing".
    void syncCall_moduleAbsent_isBoundedAndSaysWhy()
    {
        const QString mod = QStringLiteral("call_never_module");
        LogosModeConfig::setMode(LogosMode::Remote);

        LogosAPI api(QStringLiteral("caller"));
        api.getTokenManager()->saveToken(mod, QStringLiteral("tok"));
        LogosQmlBridge bridge(&api);

        QElapsedTimer t; t.start();
        const QString payload = bridge.callModule(mod, QStringLiteral("echo"),
                                                  QVariantList() << 7);
        const qint64 elapsed = t.elapsed();

        // The number that matters. 20000 is the old default; anything near it
        // means the bound was lost. Generous headroom over the 1500 ms budget
        // so this does not go flaky on a loaded builder.
        QVERIFY2(elapsed < 8000,
                 qPrintable(QStringLiteral("callModule blocked the calling thread for %1 ms "
                                           "against an absent module").arg(elapsed)));

        const QJsonObject obj = asObject(payload);
        QVERIFY2(obj.contains(QStringLiteral("error")),
                 qPrintable(QStringLiteral("expected an error payload, got: %1").arg(payload)));
        QCOMPARE(obj.value(QStringLiteral("error")).toString(),
                 QStringLiteral("Module not reachable yet"));
        QVERIFY2(obj.value(QStringLiteral("message")).toString()
                     .contains(QStringLiteral("callModuleAsync")),
                 "the error should point at the form that CAN wait");
    }

    // The bounded wait has to be a real wait, not a dressed-up fast fail: a
    // module that comes up DURING the budget must be called successfully.
    // The timer fires from inside waitForSource's nested event loop, which is
    // exactly the situation a QML view creates at startup.
    void syncCall_modulePublishedDuringTheWait_succeeds()
    {
        const QString mod = QStringLiteral("call_midwait_module");
        LogosModeConfig::setMode(LogosMode::Remote);

        LogosAPI api(QStringLiteral("caller"));
        api.getTokenManager()->saveToken(mod, QStringLiteral("tok"));
        LogosQmlBridge bridge(&api);

        // The timer must be SCOPED, not QTimer::singleShot. This test only
        // reaches its assertion if the call waits long enough for the timer to
        // fire inside it — so when the wait is short (which is precisely the
        // regression being tested) the function returns first, and a detached
        // singleShot would then fire on dangling references to these locals.
        // That is not hypothetical: it segfaulted the run that proved this test
        // red. Declaring `pub` before the timer means the timer is destroyed
        // first, cancelling any pending fire while its captures are still alive.
        std::unique_ptr<Publisher> pub;
        QTimer publishTimer;
        publishTimer.setSingleShot(true);
        QObject::connect(&publishTimer, &QTimer::timeout, &publishTimer,
                         [&pub, mod]() { pub = std::make_unique<Publisher>(mod); });
        publishTimer.start(150);

        const QString payload = bridge.callModule(mod, QStringLiteral("echo"),
                                                  QVariantList() << 7);
        QVERIFY2(!asObject(payload).contains(QStringLiteral("error")),
                 qPrintable(QStringLiteral("a module that appeared 150 ms into the bounded "
                                           "wait was not called: %1").arg(payload)));
        QCOMPARE(payload.trimmed(), QStringLiteral("7"));
    }

    // ── CONTROL for the async form ──────────────────────────────────────────
    void asyncCall_moduleUp_invokesCallbackWithResult()
    {
        const QString mod = QStringLiteral("acall_ctrl_module");
        LogosModeConfig::setMode(LogosMode::Remote);
        Publisher pub(mod);

        LogosAPI api(QStringLiteral("caller"));
        api.getTokenManager()->saveToken(mod, QStringLiteral("tok"));
        LogosQmlBridge bridge(&api);

        AsyncRun run;
        run.arm(&bridge);
        run.call(mod, 10000);
        QVERIFY2(run.waitFired(15000), "control: callback never fired for an up module");
        QVERIFY2(!asObject(run.payload()).contains(QStringLiteral("error")),
                 qPrintable(QStringLiteral("control: %1").arg(run.payload())));
        QCOMPARE(run.payload().trimmed(), QStringLiteral("7"));
    }

    // ── THE PRIMARY FIX ─────────────────────────────────────────────────────
    // Subscribe-shaped, but for a call: issue it while the module is NOT
    // reachable, bring the module up, and the call must be dispatched.
    //
    // BEFORE: `if (!client->isConnected())` returned {"error":"Module not
    // connected"} immediately and the view — which never retries — was stuck
    // with it for the life of the process.
    void asyncCall_moduleAppearsLater_isDispatched()
    {
        const QString mod = QStringLiteral("acall_late_module");
        LogosModeConfig::setMode(LogosMode::Remote);

        LogosAPI api(QStringLiteral("caller"));
        api.getTokenManager()->saveToken(mod, QStringLiteral("tok"));
        LogosQmlBridge bridge(&api);

        AsyncRun run;
        run.arm(&bridge);
        run.call(mod, 20000);          // generous: the module is what we are waiting on

        // Held, not answered. This is the assertion the old code fails: it had
        // already fired an error payload by now.
        pump(300);
        QVERIFY2(!run.fired(),
                 qPrintable(QStringLiteral("the call was answered before the module existed: %1")
                                .arg(run.payload())));

        Publisher pub(mod);

        QVERIFY2(run.waitFired(20000),
                 "a call issued before the module appeared was never dispatched");
        QVERIFY2(!asObject(run.payload()).contains(QStringLiteral("error")),
                 qPrintable(QStringLiteral("dispatched but failed: %1").arg(run.payload())));
        QCOMPARE(run.payload().trimmed(), QStringLiteral("7"));
    }

    // Waiting must not mean waiting forever. A module that never appears still
    // gets an answer, on the deadline the caller passed — otherwise "held until
    // the module appears" is just a hang with better manners.
    void asyncCall_moduleNeverAppears_timesOutOnTheCallersDeadline()
    {
        const QString mod = QStringLiteral("acall_never_module");
        LogosModeConfig::setMode(LogosMode::Remote);

        LogosAPI api(QStringLiteral("caller"));
        api.getTokenManager()->saveToken(mod, QStringLiteral("tok"));
        LogosQmlBridge bridge(&api);

        AsyncRun run;
        run.arm(&bridge);
        QElapsedTimer t; t.start();
        run.call(mod, 1000);

        QVERIFY2(run.waitFired(15000), "no answer at all for a module that never appeared");
        const qint64 elapsed = t.elapsed();
        QCOMPARE(asObject(run.payload()).value(QStringLiteral("error")).toString(),
                 QStringLiteral("timeout"));
        // Honours the CALLER's deadline rather than some internal budget.
        QVERIFY2(elapsed < 8000,
                 qPrintable(QStringLiteral("timeout payload took %1 ms for a 1000 ms deadline")
                                .arg(elapsed)));
    }

    // ── A TIMED-OUT CALL MUST NEVER BE SENT ────────────────────────────────
    // Holding a call is only at-most-once if the hold is ABANDONED when the
    // caller stops waiting. Otherwise an install / send / transfer executes
    // minutes after the view was told it failed — the callback was suppressed,
    // but the method still ran. Asserting on the callback alone cannot see
    // that, so this asserts on the PROVIDER: it must never observe the call.
    void asyncCall_timedOutThenModuleAppears_isNeverDispatched()
    {
        const QString mod = QStringLiteral("acall_notsent_module");
        LogosModeConfig::setMode(LogosMode::Remote);

        LogosAPI api(QStringLiteral("caller"));
        api.getTokenManager()->saveToken(mod, QStringLiteral("tok"));
        LogosQmlBridge bridge(&api);

        AsyncRun run;
        run.arm(&bridge);
        run.call(mod, 400);                       // short deadline, module absent

        QVERIFY2(run.waitFired(10000), "control: the deadline never fired");
        QCOMPARE(asObject(run.payload()).value(QStringLiteral("error")).toString(),
                 QStringLiteral("timeout"));

        // NOW the module turns up. The held call must be dropped, not sent.
        Publisher pub(mod);
        pump(3000);
        QCOMPARE(pub.echo.callCount.loadRelaxed(), 0);
    }

    // "pass 0 to disable" must not become "never calls back", and must not
    // become "blocks the GUI thread for the full default budget" either — the
    // dispatch-immediately shortcut reaches the synchronous acquire inside
    // invokeRemoteMethodAsync and stalls for 20 s, which is the exact
    // pathology this area exists to remove.
    void asyncCall_timeoutDisabled_doesNotBlockAndStillAnswers()
    {
        const QString mod = QStringLiteral("acall_zerotimeout_module");
        LogosModeConfig::setMode(LogosMode::Remote);

        LogosAPI api(QStringLiteral("caller"));
        api.getTokenManager()->saveToken(mod, QStringLiteral("tok"));
        LogosQmlBridge bridge(&api);

        AsyncRun run;
        run.arm(&bridge);
        QElapsedTimer t; t.start();
        run.call(mod, 0);                          // documented "disable"
        const qint64 elapsed = t.elapsed();

        QVERIFY2(elapsed < 250,
                 qPrintable(QStringLiteral("callModuleAsync(timeoutMs=0) blocked the calling "
                                           "thread for %1 ms").arg(elapsed)));

        // The module NEVER appears — deliberately. Publishing one here would
        // make this pass against the broken version too, because the stranding
        // only happens when nothing ever arrives to arm the deferred call. With
        // no deadline of its own and no fallback bounding the wait, the caller
        // is never answered at all.
        QVERIFY2(run.waitFired(40000),
                 "timeoutMs=0 was never answered: with no deadline on the call, the wait "
                 "for the module has to carry one, or 'no timeout' silently becomes "
                 "'no callback'");
        QCOMPARE(asObject(run.payload()).value(QStringLiteral("error")).toString(),
                 QStringLiteral("timeout"));
    }

    // The short startup budget must not cap a module that IS reachable, or
    // every legitimately slow method — a network fetch, a package install, a
    // chain RPC — starts failing, and old QML shipped inside a .lgx cannot opt
    // out because callModule takes no timeout argument.
    void syncCall_reachableButSlowModule_isNotCappedByTheStartupBudget()
    {
        const QString mod = QStringLiteral("call_slow_module");
        LogosModeConfig::setMode(LogosMode::Remote);

        Publisher pub(mod);
        pub.echo.sleepMs = 2500;                  // > kStartupCallBudgetMs (1500)

        LogosAPI api(QStringLiteral("caller"));
        api.getTokenManager()->saveToken(mod, QStringLiteral("tok"));
        LogosQmlBridge bridge(&api);

        const QString payload = bridge.callModule(mod, QStringLiteral("echo"),
                                                  QVariantList() << 7);
        QVERIFY2(!asObject(payload).contains(QStringLiteral("error")),
                 qPrintable(QStringLiteral("a reachable module taking 2500 ms was cut off by "
                                           "the startup budget: %1").arg(payload)));
        QCOMPARE(payload.trimmed(), QStringLiteral("7"));
    }

    // The whole point of the async form is that it does not block. Issuing one
    // against an absent module must return to QML immediately.
    void asyncCall_moduleAbsent_returnsWithoutBlocking()
    {
        const QString mod = QStringLiteral("acall_nonblock_module");
        LogosModeConfig::setMode(LogosMode::Remote);

        LogosAPI api(QStringLiteral("caller"));
        api.getTokenManager()->saveToken(mod, QStringLiteral("tok"));
        LogosQmlBridge bridge(&api);

        AsyncRun run;
        run.arm(&bridge);
        QElapsedTimer t; t.start();
        run.call(mod, 2000);
        const qint64 elapsed = t.elapsed();

        QVERIFY2(elapsed < 250,
                 qPrintable(QStringLiteral("callModuleAsync blocked the calling thread for "
                                           "%1 ms").arg(elapsed)));
    }
};

QTEST_GUILESS_MAIN(TestLogosQmlBridgeCalls)
#include "test_logos_qml_bridge_calls.moc"
