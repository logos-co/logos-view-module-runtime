// End-to-end proof of the module teardown hook on the ui-host path.
//
// Drives the REAL ui-host binary through the REAL parent (ViewModuleHost), so
// what is exercised is the shipping shutdown sequence: SIGTERM ->
// self-pipe notifier -> app.quit() -> app.exec() returns ->
// runPluginAboutToUnload() -> delete pluginObject -> exit. Nothing here is a
// stand-in for any of it.
//
// WHY THIS TEST HAS TO EXIST. A host-side hook resolved BY NAME is a silent
// no-op when the name is not there: invokeMethod returns false, nothing is
// logged, the process still exits 0 and still on time. That is indistinguishable
// from every module answering Synchronous, which is also the correct behaviour.
// So a passing build proves nothing about this feature unless something
// OBSERVES the wait -- hence the fixtures' journal, and their heartbeat.
//
// Every assertion below is on recorded entries and their order. The elapsed
// times are printed for the record, and used only where a bound genuinely is
// the contract (the hard-kill budget).

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include "ViewModuleHost.h"

namespace {

struct Run {
    QStringList journal;      // entries, in the order the plugin wrote them
    int exitCode = -1;
    qint64 stopMs = -1;       // wall clock across ViewModuleHost::stop()
    bool exited = false;

    int count(const QString& entry) const { return journal.count(entry); }
    bool has(const QString& entry) const { return journal.contains(entry); }
    int indexOf(const QString& entry) const { return journal.indexOf(entry); }
};

} // namespace

class TestUiHostUnload : public QObject
{
    Q_OBJECT

private:
    // Spawn ui-host on `pluginPath`, wait for READY, then stop it exactly the
    // way Basecamp does and collect what the plugin recorded.
    Run runOne(const QString& pluginPath)
    {
        Run r;

        QTemporaryDir dir;
        Q_ASSERT(dir.isValid());
        const QString journalPath = dir.filePath(QStringLiteral("journal.txt"));
        // The child reads this; ui-host passes its environment through.
        qputenv("LOGOS_TEST_UNLOAD_JOURNAL", journalPath.toUtf8());

        ViewModuleHost host;
        QSignalSpy readySpy(&host, &ViewModuleHost::ready);
        QSignalSpy exitSpy(&host, &ViewModuleHost::processExited);

        if (!host.spawn(QStringLiteral("unload_fixture"), pluginPath,
                        QStringLiteral("test-token"))) {
            qWarning("spawn failed for %s", qPrintable(pluginPath));
            return r;
        }

        // READY is printed only after the plugin is loaded, initLogos delivered
        // and remoting enabled -- i.e. once the process is in the state a real
        // teardown would find it in.
        if (!readySpy.wait(15000)) {
            qWarning("ui-host never reported READY for %s", qPrintable(pluginPath));
            host.stop();
            return r;
        }

        QElapsedTimer t;
        t.start();
        host.stop();               // terminate() + waitForFinished(3000) + kill()
        r.stopMs = t.elapsed();

        // stop() blocks until the child is reaped, so finished() has already
        // been queued; let it land.
        if (exitSpy.isEmpty()) exitSpy.wait(2000);
        if (!exitSpy.isEmpty()) {
            r.exited = true;
            r.exitCode = exitSpy.first().at(0).toInt();
        }

        QFile f(journalPath);
        if (f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            const QString text = QString::fromUtf8(f.readAll());
            r.journal = text.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        }
        qunsetenv("LOGOS_TEST_UNLOAD_JOURNAL");

        qInfo().noquote()
            << QStringLiteral("[%1] exit=%2 stop=%3ms journal=%4")
                   .arg(QFileInfo(pluginPath).fileName())
                   .arg(r.exitCode).arg(r.stopMs)
                   .arg(r.journal.join(QLatin1Char(',')));
        return r;
    }

private slots:

    // ---------------------------------------------------------------------
    // 1. No hook: the common case, and it must stay free.
    //
    // The plugin's meta-object genuinely has no aboutToUnload, so the host's
    // invokeMethod returns false and teardown proceeds immediately. What makes
    // this a real assertion rather than a tautology is the pair below it: the
    // same host binary, the same code path, DOES wait for the other two.
    // ---------------------------------------------------------------------
    void noHookIsSilentAndImmediate()
    {
        const Run r = runOne(QStringLiteral(FIXTURE_PLUGIN_NONE));

        QVERIFY2(r.exited, "ui-host did not exit");
        QCOMPARE(r.exitCode, 0);

        QVERIFY2(r.has(QStringLiteral("dtor")),
                 "the plugin was never destroyed -- ui-host did not reach "
                 "`delete pluginObject`");
        QVERIFY2(!r.has(QStringLiteral("about-to-unload")),
                 "a plugin with no such meta-method somehow answered the hook");
        QVERIFY2(!r.has(QStringLiteral("tick")),
                 "no hook was declared, so nothing should have run an event "
                 "loop after exec() returned");
        QCOMPARE(r.journal, QStringList{QStringLiteral("dtor")});
    }

    // ---------------------------------------------------------------------
    // 2. Asynchronous, finishes: the host waits, and the plugin's last work
    //    actually runs BEFORE it is destroyed.
    //
    // "work-done" landing above "dtor" is the whole point of the feature. It
    // can only happen if a running event loop dispatched a timer after
    // app.exec() had already returned -- which is precisely the nested loop
    // runPluginAboutToUnload spins.
    // ---------------------------------------------------------------------
    void asynchronousThatFinishesIsWaitedFor()
    {
        const Run r = runOne(QStringLiteral(FIXTURE_PLUGIN_FINISHES));

        QVERIFY2(r.exited, "ui-host did not exit");
        QCOMPARE(r.exitCode, 0);

        QVERIFY2(r.has(QStringLiteral("about-to-unload")),
                 "the host never reached the plugin's hook -- if this fails and "
                 "test 1 passes, the hook is being resolved by a name the "
                 "plugin does not publish");
        QVERIFY2(r.has(QStringLiteral("work-done")),
                 "the plugin's deferred work never ran: the host tore it down "
                 "without waiting");
        QVERIFY2(r.has(QStringLiteral("dtor")), "the plugin was never destroyed");

        // THE ordering assertion.
        QVERIFY2(r.indexOf(QStringLiteral("work-done")) < r.indexOf(QStringLiteral("dtor")),
                 "the plugin finished its work only AFTER it had been destroyed");
        QVERIFY2(r.indexOf(QStringLiteral("about-to-unload")) < r.indexOf(QStringLiteral("work-done")),
                 "work completed before the hook was even called");

        // The nested loop really was running: the heartbeat could not have
        // ticked otherwise.
        QVERIFY2(r.count(QStringLiteral("tick")) > 0,
                 "no heartbeat ticks -- the host did not run an event loop "
                 "while waiting, so an async module doing real work would "
                 "never make progress");

        // dtor is last, and nothing the plugin scheduled ran after it.
        QCOMPARE(r.journal.last(), QStringLiteral("dtor"));
    }

    // ---------------------------------------------------------------------
    // 3. Asynchronous, never signals: a grace period, not a veto.
    //
    // The host must give up and proceed. Critically it must do so from within
    // its own budget: ViewModuleHost::stop() hard-kills at 3000ms, so a grace
    // period that overran would show up here as a non-zero exit code, not as a
    // slow pass.
    // ---------------------------------------------------------------------
    void asynchronousThatHangsIsAbandoned()
    {
        const Run r = runOne(QStringLiteral(FIXTURE_PLUGIN_HANGS));

        QVERIFY2(r.exited, "ui-host did not exit");

        // The hook is a grace period, not a veto: the host proceeds regardless.
        QVERIFY2(r.has(QStringLiteral("about-to-unload")),
                 "the host never reached the plugin's hook");
        QVERIFY2(!r.has(QStringLiteral("work-done")),
                 "this fixture must never complete -- it exists to be given up on");
        QVERIFY2(r.has(QStringLiteral("dtor")),
                 "the host waited forever: a module that never signals took the "
                 "process hostage, which is exactly what the deadline exists to "
                 "prevent");
        QCOMPARE(r.journal.last(), QStringLiteral("dtor"));

        // It genuinely WAITED before giving up -- ticks can only come from the
        // nested loop.
        QVERIFY2(r.count(QStringLiteral("tick")) > 0,
                 "no ticks: the host skipped the wait entirely rather than "
                 "bounding it");

        // ...and it exited cleanly rather than being hard-killed by the parent.
        // This is the assertion that the grace period FITS the budget it was
        // carved out of. A 3000ms grace here (logos_host's constant) would
        // consume ViewModuleHost::stop()'s whole 3000ms window and turn this
        // into a kill.
        QCOMPARE(r.exitCode, 0);
        QVERIFY2(r.stopMs < 3000,
                 qPrintable(QStringLiteral("teardown took %1ms, at or past the "
                                           "parent's 3000ms hard-kill threshold")
                                .arg(r.stopMs)));
    }

    // ---------------------------------------------------------------------
    // 4. The two async modes, compared against each other.
    //
    // This is what separates "waited and was released" from "waited out the
    // deadline" WITHOUT trusting a clock: both runs heartbeat at the same
    // interval, so more ticks means more time. The hanging plugin must
    // accumulate strictly more of them, because its wait ends at the 2000ms
    // deadline while the other's ends at its ~1000ms completion.
    //
    // If a future change made "finished" and "gave up" indistinguishable --
    // say by losing the `deadline.isActive()` check, or by never connecting the
    // signal at all -- both runs would sit out the full grace period and these
    // counts would converge. Nothing else in this file would notice.
    // ---------------------------------------------------------------------
    void givingUpOutlastsFinishing()
    {
        const Run finishes = runOne(QStringLiteral(FIXTURE_PLUGIN_FINISHES));
        const Run hangs    = runOne(QStringLiteral(FIXTURE_PLUGIN_HANGS));

        const int finishTicks = finishes.count(QStringLiteral("tick"));
        const int hangTicks   = hangs.count(QStringLiteral("tick"));
        qInfo("heartbeat ticks: finishes=%d hangs=%d", finishTicks, hangTicks);

        QVERIFY2(finishTicks > 0 && hangTicks > 0, "one of the runs never waited");
        QVERIFY2(hangTicks > finishTicks,
                 qPrintable(QStringLiteral(
                     "the hanging plugin was released after %1 ticks and the "
                     "finishing one after %2 -- the host is not distinguishing "
                     "unloadFinished() from the deadline")
                         .arg(hangTicks).arg(finishTicks)));

        // And the release really was the signal, not the deadline: the run that
        // signalled must be the shorter one, by more than a tick of slack.
        QVERIFY2(finishes.stopMs < hangs.stopMs,
                 "the plugin that signalled completion did not shorten teardown");
    }
};

QTEST_MAIN(TestUiHostUnload)
#include "test_ui_host_unload.moc"
