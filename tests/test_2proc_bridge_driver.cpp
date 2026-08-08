// Process B of the 2-process reproduction: the "parent basecamp" side. Runs the
// LogosQmlBridge on a shared LogosAPI and drives sync/async/fan-out call chains
// through a QJSEngine — but the echo + capability modules live in a SEPARATE
// process (spawned host, process A), so every call crosses the process boundary
// over QtRO, with the two event loops fully independent. This is the one factor
// no in-process harness could reproduce.
//
// Usage: test_2proc_bridge_driver <path-to-modules-host> [chains...]
//   chains default to: sync async fanout
// Env passthrough to the host: LOGOS_FIRE_EVENTS, LOGOS_ROTATE_TOKENS.
#include "LogosQmlBridge.h"

#include "logos_api.h"
#include "token_manager.h"
#include "logos_instance.h"
#include "logos_mode.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJSEngine>
#include <QJSValue>
#include <QProcess>
#include <QString>
#include <QStringList>
#include <QTextStream>
#include <memory>

static QTextStream out(stdout);
static QTextStream err(stderr);

// Build a fresh bridge on its own LogosAPI, subscribe N events, then run one
// call chain of the given kind. Returns how many of N calls completed.
static int runChain(const QString& kind, int N, int nSubs)
{
    LogosAPI api(QStringLiteral("caller"));
    // Pre-seed ONLY the capability token (the app-boot auth token). echo_module's
    // token must be minted via the real cross-process requestModule handshake.
    api.getTokenManager()->saveToken(QStringLiteral("capability_module"),
                                     QStringLiteral("cap-token"));

    LogosQmlBridge bridge(&api);

    for (int i = 0; i < nSubs; ++i)
        bridge.onModuleEvent(QStringLiteral("echo_module"), QStringLiteral("ev%1").arg(i));
    for (int k = 0; k < 80; ++k)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);

    // onModuleEvent's return value no longer says anything about the
    // cross-process connection: a subscription made before the peer is
    // listening is now ACCEPTED and armed later, which is the whole point. The
    // connection probe is the pending list draining instead.
    const QStringList stillPending = bridge.pendingEventSubscriptions();
    if (!stillPending.isEmpty()) {
        err << "  [" << kind << "] WARNING: " << stillPending.size()
            << " event subscription(s) still unarmed after the settle window: "
            << stillPending.join(QStringLiteral(", ")) << "\n";
    }

    QJSEngine engine;
    engine.setObjectOwnership(&bridge, QJSEngine::CppOwnership);
    engine.globalObject().setProperty(QStringLiteral("logos"), engine.newQObject(&bridge));

    QString js;
    if (kind == QLatin1String("sync")) {
        js = QStringLiteral(R"JS(
            var __results = [];
            for (var i = 0; i < %1; ++i) {
              var r = logos.callModule("echo_module", "echo", [i]);
              __results.push(JSON.parse(r));
            }
        )JS").arg(N);
    } else if (kind == QLatin1String("fanout")) {
        js = QStringLiteral(R"JS(
            var __results = [];
            for (var i = 0; i < %1; ++i)
              logos.callModuleAsync("echo_module", "echo", [i], function (p) { __results.push(JSON.parse(p)); });
        )JS").arg(N);
    } else { // async chain
        js = QStringLiteral(R"JS(
            var __results = [];
            function fireNext(i) {
              if (i >= %1) return;
              logos.callModuleAsync("echo_module", "echo", [i], function (p) {
                __results.push(JSON.parse(p)); fireNext(i + 1);
              });
            }
            fireNext(0);
        )JS").arg(N);
    }

    QJSValue driver = engine.evaluate(js);
    if (driver.isError()) {
        err << "  [" << kind << "] JS error: " << driver.toString() << "\n";
        return -1;
    }

    QJSValue results = engine.globalObject().property(QStringLiteral("__results"));
    QElapsedTimer t; t.start();
    // For the sync chain the loop already ran to completion inside evaluate();
    // this pump only matters for the async/fanout kinds.
    while (results.property(QStringLiteral("length")).toInt() < N && t.elapsed() < 20000)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 20);

    return results.property(QStringLiteral("length")).toInt();
}

int main(int argc, char** argv)
{
    QCoreApplication app(argc, argv);

    if (argc < 2) {
        err << "usage: " << argv[0] << " <modules-host-binary> [sync|async|fanout ...]\n";
        return 2;
    }
    const QString hostPath = QString::fromLocal8Bit(argv[1]);
    QStringList chains;
    for (int i = 2; i < argc; ++i) chains << QString::fromLocal8Bit(argv[i]);
    if (chains.isEmpty()) chains << "sync" << "async" << "fanout";

    // Fix the instance id BEFORE spawning the host so both processes derive the
    // same socket names. Child inherits LOGOS_INSTANCE_ID via the environment.
    qputenv("LOGOS_INSTANCE_ID", QByteArrayLiteral("twoproc-test"));
    LogosModeConfig::setMode(LogosMode::Remote);

    // ── Spawn process A (modules host) and wait for READY ────────────────────
    QProcess host;
    host.setProcessChannelMode(QProcess::SeparateChannels);
    host.setProcessEnvironment(QProcessEnvironment::systemEnvironment());
    host.start(hostPath, {});
    if (!host.waitForStarted(10000)) {
        err << "failed to start modules host: " << host.errorString() << "\n";
        return 2;
    }
    bool ready = false;
    QElapsedTimer t; t.start();
    QByteArray acc;
    while (!ready && t.elapsed() < 15000) {
        if (host.waitForReadyRead(500))
            acc += host.readAllStandardOutput();
        if (acc.contains("READY")) ready = true;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        if (host.state() == QProcess::NotRunning) break;
    }
    if (!ready) {
        err << "modules host never signalled READY (state="
            << host.state() << ", stderr=" << host.readAllStandardError() << ")\n";
        host.kill(); host.waitForFinished(3000);
        return 2;
    }
    out << "host READY: " << acc.trimmed() << "\n"; out.flush();

    // Give the host's sockets a beat to accept connections.
    QElapsedTimer settle; settle.start();
    while (settle.elapsed() < 300) QCoreApplication::processEvents(QEventLoop::AllEvents, 10);

    // ── Run the chains ───────────────────────────────────────────────────────
    constexpr int N = 12;
    constexpr int SUBS = 14;
    bool allOk = true;
    for (const QString& kind : chains) {
        const int done = runChain(kind, N, SUBS);
        const bool ok = (done == N);
        allOk = allOk && ok;
        out << "  chain=" << kind << " completed=" << done << "/" << N
            << (ok ? "  OK" : "  *** STALL/DROP ***") << "\n";
        out.flush();
    }

    host.kill();
    host.waitForFinished(3000);

    out << (allOk ? "RESULT: ALL OK\n" : "RESULT: FAILURE (a chain stalled/dropped)\n");
    out.flush();
    return allOk ? 0 : 1;
}
