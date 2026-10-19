#pragma once

#include <QHash>
#include <QMetaType>
#include <QMutex>
#include <QObject>
#include <QSet>
#include <QStringList>
#include <QVariantList>

#include <functional>

// logos::ConsumerIdentity, cached by value below.
#include "logos_consumer.h"

class IComponent;
class LogosAPI;
class LogosQmlBridge;
class QQuickWidget;
class QWidget;
class ViewModuleHost;

namespace logos::ui {

class CoreDependencyLoader;

enum class UiPluginKind {
    Legacy,  // an IComponent Qt plugin that builds a QWidget in-process
    UiQml,   // a QML view, with an optional backend run in ui-host
};

struct UiPluginRequest {
    QString name;
    UiPluginKind kind = UiPluginKind::Legacy;
    QString pluginPath;                  // Legacy: the plugin library
    QString iconPath;
    QVariantList dependencies;           // must load, or the plugin fails
    QVariantList optionalDependencies;   // best-effort

    // UiQml
    QString installDir;                  // import root; the sandbox confines the view here
    QString qmlViewPath;                 // entry QML file
    QString mainFilePath;                // backend plugin; empty for QML-only
    bool liveReload = false;             // rebuild the view when its files change
};

// Loads UI plugins into a Qt host: required and optional core dependencies
// off the GUI thread, one admitted consumer identity per plugin, then either
// the legacy widget or the sandboxed QML view (backend in ui-host).
class UiPluginLoader : public QObject {
    Q_OBJECT

public:
    // Called on a worker thread; true when the module ends up loaded.
    using LoadDependency = std::function<bool(const QString& name, bool required)>;
    // Runs before the view's QML does, e.g. to attach the bridge to intents.
    using BridgeSetup = std::function<void(const QString& name, LogosQmlBridge* bridge)>;

    // `hostApi` is the host's trusted identity; plugins never get it.
    UiPluginLoader(LogosAPI* hostApi, LoadDependency loadDependency,
                   QObject* parent = nullptr);
    ~UiPluginLoader() override;

    void setBridgeSetup(BridgeSetup setup) { m_bridgeSetup = std::move(setup); }
    // Also accept legacy plugins that only expose an invokable createWidget().
    void setAcceptInvokableWidgetFactories(bool accept) { m_acceptInvokable = accept; }

    void load(const UiPluginRequest& request);

    bool isLoading(const QString& name) const;
    QStringList loadingPlugins() const;

signals:
    // `component` is null for UiQml and for invokable-only legacy plugins;
    // `viewHost` and `bridge` are null for Legacy.
    void pluginLoaded(const QString& name, QWidget* widget, IComponent* component,
                      logos::ui::UiPluginKind kind, ViewModuleHost* viewHost,
                      LogosQmlBridge* bridge);
    void pluginLoadFailed(const QString& name, const QString& error);
    void loadingChanged();

private:
    void startLoad(const UiPluginRequest& request);
    void loadDependencies(const UiPluginRequest& request);
    void continueLoad(const UiPluginRequest& request);

    void loadLegacyAsync(const UiPluginRequest& request);
    void finishLegacyLoad(const UiPluginRequest& request);

    void loadUiQml(const UiPluginRequest& request);
    void loadQmlView(const UiPluginRequest& request, LogosQmlBridge* bridge,
                     ViewModuleHost* viewHost);
    void finishQmlView(QQuickWidget* view, const UiPluginRequest& request,
                       LogosQmlBridge* bridge, ViewModuleHost* viewHost);

    void fail(const QString& name, const QString& error);
    void setLoading(const QString& name, bool loading);

    // Cached: a LogosAPI keeps its store by pointer and its clients cache
    // tokens, and re-admitting would revoke the credential already handed out.
    logos::ConsumerIdentity consumerFor(const QString& name);

    LogosAPI* m_hostApi;
    LoadDependency m_loadDependency;
    BridgeSetup m_bridgeSetup;
    bool m_acceptInvokable = false;
    CoreDependencyLoader* m_dependencyLoader;
    QHash<QString, logos::ConsumerIdentity> m_consumers;

    mutable QMutex m_mutex;
    QSet<QString> m_loading;
};

} // namespace logos::ui

Q_DECLARE_METATYPE(logos::ui::UiPluginKind)
