#include "UiPluginPackage.h"

#include "QmlLiveView.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>

namespace logos::ui {

namespace {

QJsonObject readJsonObject(const QString& path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly)
        ? QJsonDocument::fromJson(file.readAll()).object()
        : QJsonObject();
}

// The library a package declares, or empty. manifest.json names real files,
// keyed by variant when it is an object; metadata.json a logical name without
// platform prefix or suffix. Never a guess from the directory's contents.
QString resolveBackendLib(const QString& dir, const QJsonObject& metadata,
                          const QJsonObject& manifest)
{
    const QString variant = [&] {
        QFile f(dir + "/variant");
        return f.open(QIODevice::ReadOnly) ? QString::fromUtf8(f.readAll()).trimmed()
                                           : QString();
    }();

    const QJsonValue manifestMain = manifest.value("main");
    if (manifestMain.isObject()) {
        const QJsonObject byVariant = manifestMain.toObject();
        QStringList keys;
        if (!variant.isEmpty() && byVariant.contains(variant)) keys << variant;
        for (const QString& k : byVariant.keys())
            if (k != variant) keys << k;
        for (const QString& k : keys) {
            const QString file = byVariant.value(k).toString();
            if (!file.isEmpty() && QFile::exists(dir + "/" + file)) return dir + "/" + file;
        }
        return QString();
    }
    if (manifestMain.isString() && !manifestMain.toString().isEmpty()) {
        const QString path = dir + "/" + manifestMain.toString();
        return QFile::exists(path) ? path : QString();
    }

    const QString name = metadata.value("main").toString().trimmed();
    if (name.isEmpty()) return QString();
    for (const QString& candidate : {
#if defined(Q_OS_WIN)
             name + ".dll", "lib" + name + ".dll",
#elif defined(Q_OS_MAC)
             name + ".dylib", "lib" + name + ".dylib",
#else
             "lib" + name + ".so", name + ".so",
#endif
         }) {
        const QString path = dir + "/" + candidate;
        if (QFile::exists(path)) return path;
    }
    return QString();
}

// DEV_QML_PATH points at the directory holding the view's entry file.
void applyDevQmlPath(const QString& viewField, UiPluginRequest* request)
{
    const QString dev = QString::fromUtf8(qgetenv("DEV_QML_PATH")).trimmed();
    if (dev.isEmpty()) return;
    if (!QFileInfo(dev).isDir()) {
        qWarning().noquote() << "DEV_QML_PATH is not a directory:" << dev
                             << "- using installed view";
        return;
    }
    const QString entry = QDir(dev).absoluteFilePath(QFileInfo(viewField).fileName());
    if (!QFile::exists(entry)) {
        qWarning().noquote() << "DEV_QML_PATH set but entry not found:" << entry
                             << "- using installed view";
        return;
    }
    qInfo().noquote() << "DEV_QML_PATH override active:" << entry;
    request->qmlViewPath = entry;
    request->installDir = QDir(dev).absolutePath();
    request->liveReload = QmlLiveView::isEnabledFor(request->installDir);
}

bool setError(QString* error, const QString& message)
{
    if (error) *error = message;
    return false;
}

} // namespace

bool resolveUiPluginPath(const QString& path, UiPluginRequest* request, QString* error)
{
    const QFileInfo info(path);
    const QString resolved = info.absoluteFilePath();
    *request = UiPluginRequest{};

    if (info.isFile()) {
        request->name = info.baseName();
        request->kind = UiPluginKind::Legacy;
        request->pluginPath = resolved;
        return true;
    }
    if (!info.isDir())
        return setError(error, QStringLiteral("UI plugin not found: ") + resolved);

    // Both files are read: they disagree about what `main` means.
    const QJsonObject metadata = readJsonObject(resolved + "/metadata.json");
    const QJsonObject manifest = readJsonObject(resolved + "/manifest.json");
    const QJsonObject pluginInfo = metadata.isEmpty() ? manifest : metadata;
    if (pluginInfo.isEmpty())
        return setError(error, QStringLiteral("No metadata.json or manifest.json in ") + resolved);

    request->name = pluginInfo.value("name").toString();
    if (request->name.isEmpty()) {
        request->name = QFileInfo(resolved).baseName();
        qWarning() << "UI plugin metadata has no 'name'; using" << request->name;
    }
    request->dependencies = pluginInfo.value("dependencies").toArray().toVariantList();
    request->optionalDependencies =
        pluginInfo.value("optional_dependencies").toArray().toVariantList();

    const QString type = pluginInfo.value("type").toString();
    if (type == QLatin1String("ui_qml")) {
        const QString viewField = pluginInfo.value("view").toString();
        if (viewField.isEmpty())
            return setError(error, QStringLiteral("ui_qml plugin has no 'view': ") + resolved);
        request->kind = UiPluginKind::UiQml;
        request->installDir = resolved;
        request->qmlViewPath = resolved + "/" + viewField;
        // Empty for a QML-only plugin.
        request->mainFilePath = resolveBackendLib(resolved, metadata, manifest);
        applyDevQmlPath(viewField, request);
        return true;
    }
    if (type == QLatin1String("ui")) {
        request->kind = UiPluginKind::Legacy;
        request->pluginPath = resolveBackendLib(resolved, metadata, manifest);
        if (request->pluginPath.isEmpty())
            return setError(error, QStringLiteral("UI plugin declares no resolvable 'main': ")
                                       + resolved);
        return true;
    }
    return setError(error, QStringLiteral("Unknown UI plugin type '") + type
                               + QStringLiteral("' in ") + resolved);
}

} // namespace logos::ui
