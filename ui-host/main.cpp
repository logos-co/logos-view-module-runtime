// ui-host: Out-of-process host for view module plugins.
// Loads a Qt plugin and exposes it directly via a private QRemoteObjectHost
// socket. The plugin's Q_INVOKABLE methods, signals, Q_PROPERTYs and enums
// become available to the parent process via QRemoteObjectDynamicReplica.
// Any Q_PROPERTY of type QAbstractItemModel* is additionally remoted as a
// child source so QML can use it as a model directly (see Gap 2 design).
//
// Only the parent process (via LogosQmlBridge) knows the socket name, so the
// plugin is NOT discoverable by other modules.

#include <QCoreApplication>
#include <QCommandLineParser>
#include <QLocalSocket>
#include <QPluginLoader>
#include <QRemoteObjectHost>
#include <QAbstractItemModel>
#include <QMetaProperty>
#include <QSocketNotifier>
#include <QTextStream>
#include <QDebug>

#include <cerrno>
#include <csignal>
#include <cstring>
#include <thread>
#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif
#ifdef __linux__
#include <sys/prctl.h>
#endif

#include "logos_api.h"
#include "logos_consumer.h"
#include "logos_plugin_unload.h"
#include "LogosViewPlugin.h"

// The view's chance to finish, between "stop" and teardown.
//
// Budget. ViewModuleHost::stop() gives this process terminate() plus
// waitForFinished(3000) before it resorts to kill(), and EVERYTHING after the
// signal has to fit inside that 3s: unwinding app.exec() through the self-pipe
// notifier, this grace period, then `delete pluginObject`, the
// QRemoteObjectHost destructor that unlinks the QtRO socket, and process exit.
// 2s spends most of the window on the view while keeping a margin that is
// comfortably more than the teardown it precedes: the whole SIGTERM-to-exit
// sequence for a plugin with no hook at all measures 1ms
// (tests/test_ui_host_unload.cpp, noHookIsSilentAndImmediate), so the ~1s left
// over is three orders of magnitude more than it has to cover.
//
// Note that the deadline is a default (coarse) QTimer, which Qt may fire up to
// 5% early -- measured 1903ms for a nominal 2000ms. That errs toward giving up
// sooner, which is the safe direction for a budget carve-out.
//
// This is deliberately SMALLER than logos_host's 3000ms. The grace period is
// carved out of the caller's hard-kill budget, and ui-host's caller is less
// patient than the module container's (3s here, 5s there). Sharing the helper
// but not the constant is exactly why runPluginAboutToUnload takes the grace
// period as a parameter. Raising this to 3000 would consume the whole budget
// and leave the view hard-killed mid-teardown -- the precise failure the hook
// exists to prevent.
constexpr int kUnloadGraceMs = 2000;

int main(int argc, char* argv[])
{
#ifndef _WIN32
    // Out-of-process view-module hosts are spawned by a parent (Basecamp, or any
    // app embedding the view runtime). Mirror logos_host: (1) detach into our own
    // session/process group so tearing the module tree down can't leak a signal
    // into the parent's process group; (2) tie our lifetime to the parent so we
    // never linger as an orphan if the parent *crashes* (it normally kills us
    // explicitly). setsid() removes the controlling-terminal SIGHUP that used to
    // reap orphans, so we replace it with PR_SET_PDEATHSIG (Linux) plus a
    // portable getppid() watchdog. Compare against the parent's actual pid (not
    // pid 1) so a parent that is itself PID 1 (a container) is handled correctly.
    if (::setsid() == -1 && errno != EPERM) {
        ::setpgid(0, 0);
    }
    {
        const pid_t parent_pid = ::getppid();
#ifdef __linux__
        ::prctl(PR_SET_PDEATHSIG, SIGKILL);
#endif
        if (::getppid() != parent_pid) {
            _exit(0);
        }
        std::thread([parent_pid] {
            while (::getppid() == parent_pid) {
                ::sleep(1);
            }
            _exit(0);
        }).detach();
    }
#endif

    QCoreApplication app(argc, argv);
    app.setOrganizationName("Logos");
    app.setOrganizationDomain("logos.co");
    app.setApplicationName("ui-host");

    // Catch SIGTERM/SIGINT so the parent's QProcess::terminate() (and a
    // user-issued Ctrl-C) unwinds app.exec() cleanly. Without this the
    // default handler kills the process outright, skipping plugin
    // destructors. SIGKILL stays uncatchable and remains the force-kill
    // fallback in ViewModuleHost::stop().
    //
    // Self-pipe trick: the POSIX handler may only call async-signal-safe
    // functions, and QCoreApplication::quit() is not (it grabs Qt-internal
    // mutexes). Write a byte to a pipe from the handler — write(2) IS
    // async-signal-safe — and let a QSocketNotifier on the main thread
    // pick it up and call quit() from normal Qt context.
#ifndef _WIN32
    static int sigPipe[2] = {-1, -1};
    if (::pipe(sigPipe) != 0) {
        qWarning() << "ui-host: pipe() for signal handler failed:"
                   << ::strerror(errno);
    } else {
        // Non-blocking read end so the drain loop in the notifier callback
        // returns instead of blocking once the pipe is empty. The write end
        // stays blocking (write of 1 byte never blocks in practice since
        // the pipe buffer is 4 KiB+) so the handler stays trivial.
        ::fcntl(sigPipe[0], F_SETFL, ::fcntl(sigPipe[0], F_GETFL, 0) | O_NONBLOCK);

        auto* notifier = new QSocketNotifier(sigPipe[0], QSocketNotifier::Read, &app);
        QObject::connect(notifier, &QSocketNotifier::activated, &app, [&app]() {
            char buf[8];
            while (::read(sigPipe[0], buf, sizeof(buf)) > 0) {}
            qDebug() << "ui-host: received termination signal, quitting";
            app.quit();
        });

        struct sigaction sa{};
        sa.sa_handler = [](int) {
            const char c = 1;
            // Drop EINTR/EAGAIN silently — a missed wake-up just delays
            // shutdown until the next signal or socket activity.
            (void)::write(sigPipe[1], &c, 1);
        };
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESTART;
        ::sigaction(SIGTERM, &sa, nullptr);
        ::sigaction(SIGINT,  &sa, nullptr);
    }
#endif

    QCommandLineParser parser;
    parser.setApplicationDescription("Logos UI module host process");
    parser.addHelpOption();

    QCommandLineOption nameOpt(QStringList() << "name",
                               "Module name", "module_name");
    QCommandLineOption pathOpt(QStringList() << "path",
                               "Path to the plugin .so/.dylib", "plugin_path");
    QCommandLineOption socketOpt(QStringList() << "socket",
                                 "Local socket name for QRemoteObjectHost", "socket_name");

    parser.addOption(nameOpt);
    parser.addOption(pathOpt);
    parser.addOption(socketOpt);
    parser.process(app);

    if (!parser.isSet(nameOpt) || !parser.isSet(pathOpt) || !parser.isSet(socketOpt)) {
        qCritical() << "Usage: ui-host --name <module_name> --path <plugin.so> --socket <socket_name>";
        return 1;
    }

    const QString moduleName = parser.value(nameOpt);
    const QString pluginPath = parser.value(pathOpt);
    const QString socketName = parser.value(socketOpt);

    // Receive the per-spawn auth token from the parent (ViewModuleHost)
    QString authToken;
    {
        QLocalSocket client;
        client.connectToServer(socketName + QStringLiteral("_token"));
        if (!client.waitForConnected(10000)) {
            qCritical() << "ui-host: failed to connect to parent token socket for"
                        << moduleName << ":" << client.errorString();
            return 1;
        }
        if (!client.waitForReadyRead(5000)) {
            qCritical() << "ui-host: timeout waiting for auth token from parent for"
                        << moduleName;
            return 1;
        }
        authToken = QString::fromUtf8(client.readAll());
        client.disconnectFromServer();
    }
    if (authToken.isEmpty()) {
        qCritical() << "ui-host: parent sent empty auth token for" << moduleName;
        return 1;
    }

    QPluginLoader loader(pluginPath);
    if (!loader.load()) {
        qCritical() << "Failed to load plugin:" << loader.errorString();
        return 1;
    }

    QObject* pluginObject = loader.instance();
    if (!pluginObject) {
        qCritical() << "Failed to get plugin instance:" << loader.errorString();
        return 1;
    }

    qDebug() << "ui-host: loaded plugin" << moduleName << "from" << pluginPath;

    LogosAPI* logosAPI = new LogosAPI(moduleName);
    logosAPI->setParent(&app);

    // Adopt the per-spawn credential the PARENT minted and already registered
    // with capability_module (basecamp's PluginLoader / standalone's MainWindow,
    // both through logos::admitConsumer). This process only installs it — no
    // isolation and no second registration, which would invalidate the very
    // credential the parent is still holding.
    //
    // Was two saveToken() calls spelling "core" and "capability_module" by hand.
    // Those keys are TokenManager::bootstrapKeys(), and this was the fifth place
    // in the workspace that spelled them out; adoptConsumerCredential is the one
    // that owns the set.
    logos::adoptConsumerCredential(logosAPI, authToken);

    int methodIndex = pluginObject->metaObject()->indexOfMethod("initLogos(LogosAPI*)");
    if (methodIndex != -1) {
        QMetaObject::invokeMethod(pluginObject, "initLogos",
                                  Qt::DirectConnection,
                                  Q_ARG(LogosAPI*, logosAPI));
        qDebug() << "ui-host: called initLogos on plugin" << moduleName;
    }

    QRemoteObjectHost host(QUrl(QStringLiteral("local:") + socketName));

    // Prefer typed remoting via the LogosViewPlugin interface. The generated
    // <Foo>ViewPluginBase (from logos_module(REP_FILE ...)) performs
    // host->enableRemoting<FooSourceAPI>(backend), which publishes the typed
    // source signature so typed replicas on the client side reach the Valid
    // state. Without this, dynamic (name-based) remoting would use a
    // different signature hash and typed replicas would stall in Default.
    auto* viewPlugin = qobject_cast<LogosViewPlugin*>(pluginObject);
    if (!viewPlugin) {
        viewPlugin = dynamic_cast<LogosViewPlugin*>(pluginObject);
    }
    QObject* remoteTarget = viewPlugin ? viewPlugin->viewObject() : pluginObject;
    if (!remoteTarget) remoteTarget = pluginObject;

    bool remotingEnabled = false;
    if (viewPlugin) {
        remotingEnabled = viewPlugin->enableRemoting(&host);
        if (!remotingEnabled) {
            qWarning() << "ui-host: LogosViewPlugin::enableRemoting() returned "
                          "false, falling back to dynamic remoting";
        }
    }

    // Fallback: dynamic remoting. All Q_INVOKABLEs, slots, signals, and
    // Q_PROPERTYs (with NOTIFY) on the remote target propagate to a
    // QRemoteObjectDynamicReplica on the client side. Used for plugins
    // without a .rep / LogosViewPlugin implementation.
    if (!remotingEnabled) {
        if (!host.enableRemoting(remoteTarget, moduleName)) {
            qCritical() << "Failed to enable remoting for" << moduleName;
            return 1;
        }
    }
    qDebug() << "ui-host: remoting enabled for" << moduleName;

    // Gap 2: scan the remote target for Q_PROPERTYs whose value is a
    // QAbstractItemModel* and remote each as a child source named
    // "<moduleName>/<propertyName>". The parent side acquires these via
    // QRemoteObjectNode::acquireModel().
    const QMetaObject* mo = remoteTarget->metaObject();
    for (int i = 0; i < mo->propertyCount(); ++i) {
        QMetaProperty prop = mo->property(i);
        if (!prop.isReadable()) continue;
        QVariant value = prop.read(remoteTarget);
        auto* model = qvariant_cast<QAbstractItemModel*>(value);
        if (!model) continue;

        QList<int> roles = model->roleNames().keys();
        QString childName = QStringLiteral("%1/%2").arg(moduleName, QString::fromUtf8(prop.name()));
        if (!host.enableRemoting(model, childName, roles)) {
            qWarning() << "ui-host: failed to enable remoting for model" << childName;
            continue;
        }
        qDebug() << "ui-host: remoted model property" << prop.name()
                 << "as" << childName << "with roles" << roles;
    }

    QTextStream out(stdout);
    out << "READY" << Qt::endl;
    out.flush();

    const int rc = app.exec();

    // Safe to run a nested event loop here: the application loop has already
    // returned, so this is the same shape as logos_host's call site rather
    // than a re-entrant exec(). Must come BEFORE `delete pluginObject` -- it is
    // the plugin it asks, and the plugin has to still be alive to answer.
    logos::runPluginAboutToUnload(pluginObject, kUnloadGraceMs);

    delete pluginObject;
    return rc;
}
