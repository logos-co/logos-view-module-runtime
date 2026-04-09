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
#include <QPluginLoader>
#include <QRemoteObjectHost>
#include <QAbstractItemModel>
#include <QMetaProperty>
#include <QTextStream>
#include <QDebug>

#include "logos_api.h"
#include "LogosViewPlugin.h"

int main(int argc, char* argv[])
{
    QCoreApplication app(argc, argv);
    app.setApplicationName("ui-host");

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
    qDebug() << "ui-host: remoting enabled on" << socketName << "as" << moduleName;

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

    return app.exec();
}
