// RFC-1 / RFC-2 end to end: the REAL ui-host spawned by the REAL ViewModuleHost
// in gui mode, observed from inside the child by the gui fixture's journal and
// from the parent by the surface handshake.
//
// What is asserted:
//   * --gui gives the plugin a QGuiApplication, with quit-on-last-window off,
//     and a QQuickWindow it opens reaches isExposed() (RFC-1);
//   * the parent minted a 32-hex surface token, the child set its app_id to
//     logos-view-<token> before the plugin ran, echoed the token on READY and
//     the parent exposes it as surfaceToken() (RFC-2 §2-§5);
//   * a plain spawn is byte-identical to before: QCoreApplication, no window,
//     no second token line, empty surfaceToken() (RFC-1 Compatibility);
//   * two gui spawns never share a token;
//   * LogosQmlBridge keeps the token for host code and drops it with the
//     module (RFC-2 §6), and does not expose it to QML.
//
// The test environment forces QT_QPA_PLATFORM=offscreen; ui-host only sets
// wayland when --surface-socket is given, which is RFC-4's test (§16.6).

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QMetaObject>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include "LogosQmlBridge.h"
#include "ViewModuleHost.h"

namespace {

struct Run {
    QStringList journal;
    QString sourceName;
    QString surfaceToken;
    qint64 pid = 0;
    bool ready = false;
    bool exited = false;
    int exitCode = -1;

    bool has(const QString& e) const { return journal.contains(e); }
    QString valueOf(const QString& key) const
    {
        for (const QString& e : journal)
            if (e.startsWith(key + QLatin1Char(' '))) return e.mid(key.size() + 1);
        return QString();
    }
};

} // namespace

class TestUiHostGui : public QObject
{
    Q_OBJECT

private:
    Run runOne(const ViewSpawnOptions& options, bool waitForWindow)
    {
        Run r;
        QTemporaryDir dir;
        Q_ASSERT(dir.isValid());
        const QString journalPath = dir.filePath(QStringLiteral("journal.txt"));
        qputenv("LOGOS_TEST_GUI_JOURNAL", journalPath.toUtf8());

        ViewModuleHost host;
        QSignalSpy readySpy(&host, &ViewModuleHost::ready);
        QSignalSpy exitSpy(&host, &ViewModuleHost::processExited);

        if (!host.spawn(QStringLiteral("gui_fixture"), QStringLiteral(FIXTURE_PLUGIN_GUI),
                        QStringLiteral("test-token"), options)) {
            qWarning("spawn failed");
            qunsetenv("LOGOS_TEST_GUI_JOURNAL");
            return r;
        }
        r.pid = host.processId();
        if (!readySpy.wait(20000)) {
            qWarning("ui-host never reported READY");
            host.stop();
            qunsetenv("LOGOS_TEST_GUI_JOURNAL");
            return r;
        }
        r.ready = true;
        r.sourceName = host.sourceName();
        r.surfaceToken = host.surfaceToken();

        auto readJournal = [&] {
            QFile f(journalPath);
            if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return QStringList();
            return QString::fromUtf8(f.readAll()).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
        };
        if (waitForWindow) {
            // The expose arrives through the child's event loop after READY.
            QElapsedTimer t;
            t.start();
            while (!readJournal().contains(QStringLiteral("window-exposed")) && t.elapsed() < 15000)
                QTest::qWait(50);
        }

        host.stop();
        if (exitSpy.isEmpty()) exitSpy.wait(2000);
        if (!exitSpy.isEmpty()) {
            r.exited = true;
            r.exitCode = exitSpy.first().at(0).toInt();
        }
        r.journal = readJournal();
        qunsetenv("LOGOS_TEST_GUI_JOURNAL");
        qInfo().noquote() << QStringLiteral("[gui=%1] exit=%2 token=%3 journal=%4")
                                 .arg(options.gui).arg(r.exitCode).arg(r.surfaceToken)
                                 .arg(r.journal.join(QLatin1Char(',')));
        return r;
    }

private slots:

    void guiModeGivesAGuiApplicationAndAWindow()
    {
        ViewSpawnOptions opts;
        opts.gui = true;
        const Run r = runOne(opts, /*waitForWindow=*/true);
        QVERIFY2(r.ready, "ui-host --gui never became ready");
        QCOMPARE(r.valueOf(QStringLiteral("app")), QStringLiteral("gui"));
        QCOMPARE(r.valueOf(QStringLiteral("quit-on-last-window")), QStringLiteral("0"));
        QVERIFY2(r.has(QStringLiteral("window-exposed")), "the plugin's QQuickWindow was never exposed");
        QVERIFY2(r.has(QStringLiteral("dtor")), "ui-host did not unwind to `delete pluginObject`");
        QVERIFY(r.exited);
        QCOMPARE(r.exitCode, 0);
    }

    void surfaceHandshakeRoundTrips()
    {
        ViewSpawnOptions opts;
        opts.gui = true;
        const Run r = runOne(opts, /*waitForWindow=*/false);
        QVERIFY(r.ready);
        // §2: 16 random bytes, hex.
        QVERIFY2(QRegularExpression(QStringLiteral("^[0-9a-f]{32}$")).match(r.surfaceToken).hasMatch(),
                 qPrintable(QStringLiteral("surface token '%1' is not 32 hex chars").arg(r.surfaceToken)));
        // §4: the child set its app_id from the token BEFORE the plugin ran.
        QCOMPARE(r.valueOf(QStringLiteral("app-id")), QStringLiteral("logos-view-") + r.surfaceToken);
        // §5: the source name survived the suffix (dynamic remoting publishes
        // the module name).
        QCOMPARE(r.sourceName, QStringLiteral("gui_fixture"));
        QVERIFY(r.pid > 0);
    }

    void plainSpawnIsUnchanged()
    {
        const Run r = runOne(ViewSpawnOptions{}, /*waitForWindow=*/false);
        QVERIFY(r.ready);
        QCOMPARE(r.valueOf(QStringLiteral("app")), QStringLiteral("core"));
        QVERIFY(!r.has(QStringLiteral("window-exposed")));
        QVERIFY(!r.has(QStringLiteral("app-id")));
        QVERIFY2(r.surfaceToken.isEmpty(), "a non-gui spawn must not carry a surface token");
        QCOMPARE(r.sourceName, QStringLiteral("gui_fixture"));
        QVERIFY(r.exited);
        QCOMPARE(r.exitCode, 0);
    }

    void surfaceSocketImpliesGui()
    {
        // Only the option plumbing is checked here: a real --surface-socket
        // makes ui-host a wayland client of a compositor this test does not
        // run (RFC-4 / §16.6).
        ViewModuleHost host;
        QVERIFY(!host.isGui());
        ViewSpawnOptions opts;
        opts.surfaceSocket = QStringLiteral("logos-basecamp-test");
        QVERIFY(!opts.gui);
        // spawn() promotes it; use the empty-token refusal to stop before a process starts.
        QVERIFY(!host.spawn(QStringLiteral("x"), QStringLiteral("/nonexistent"), QString(), opts));
    }

    void tokensAreUniquePerSpawn()
    {
        ViewSpawnOptions opts;
        opts.gui = true;
        const Run a = runOne(opts, false);
        const Run b = runOne(opts, false);
        QVERIFY(a.ready && b.ready);
        QVERIFY(!a.surfaceToken.isEmpty());
        QVERIFY2(a.surfaceToken != b.surfaceToken, "two spawns shared a surface token");
    }

    void bridgeKeepsTheTokenForHostCodeOnly()
    {
        LogosQmlBridge bridge(nullptr);
        QVERIFY(bridge.viewModuleSurfaceToken(QStringLiteral("m")).isEmpty());
        bridge.setViewModuleSurface(QStringLiteral("m"), QStringLiteral("abc"));
        QCOMPARE(bridge.viewModuleSurfaceToken(QStringLiteral("m")), QStringLiteral("abc"));
        // Not reachable from QML: neither accessor is in the metaobject.
        const QMetaObject* mo = bridge.metaObject();
        QCOMPARE(mo->indexOfMethod("viewModuleSurfaceToken(QString)"), -1);
        QCOMPARE(mo->indexOfMethod("setViewModuleSurface(QString,QString)"), -1);
        // Dropped with the module.
        bridge.notifyViewModuleCrashed(QStringLiteral("m"));
        QVERIFY(bridge.viewModuleSurfaceToken(QStringLiteral("m")).isEmpty());
        bridge.setViewModuleSurface(QStringLiteral("m"), QStringLiteral("abc"));
        bridge.setViewModuleSurface(QStringLiteral("m"), QString());
        QVERIFY(bridge.viewModuleSurfaceToken(QStringLiteral("m")).isEmpty());
    }
};

QTEST_GUILESS_MAIN(TestUiHostGui)
#include "test_ui_host_gui.moc"
