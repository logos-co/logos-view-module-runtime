#include "UiPluginLoader.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QGuiApplication>
#include <QIcon>
#include <QMutexLocker>
#include <QPluginLoader>
#include <QQmlComponent>
#include <QQmlContext>
#include <QQmlEngine>
#include <QQmlError>
#include <QQuickWidget>
#include <QScreen>
#include <QThread>
#include <QTimer>
#include <QUrl>

#include <cmath>
#include <memory>

#include "CoreDependencyLoader.h"
#include "DependencyEntry.h"
#include "IComponent.h"
#include "LogosQmlBridge.h"
#include "QmlLiveView.h"
#include "QmlSandbox.h"
#include "ViewModuleHost.h"
#include "logos_api.h"
#include "logos_consumer.h"
#include "win_dll_search.h"

namespace logos::ui {

namespace {

constexpr int kUiHostReadyTimeoutMs = 30000;
// From ui-host's READY to its backend replica being Valid: one round trip for
// the schema, answered even while the backend waits on synchronous IPC. Only a
// wedged backend reaches this; one that exits fails the load at once.
constexpr int kBackendReadyTimeoutMs = 30000;

// Two frames of the primary screen's refresh, floored at one 60 Hz frame:
// long enough for a loading spinner to paint before anything blocks.
int spinnerPaintDelayMs()
{
    qreal hz = 0;
    if (const QScreen* screen = QGuiApplication::primaryScreen())
        hz = screen->refreshRate();
    if (hz < 1.0) hz = 60.0;
    return qBound(16, static_cast<int>(std::ceil(2000.0 / hz)), 50);
}

QWidget* createWidgetByInvocation(QObject* plugin, LogosAPI* api)
{
    QWidget* widget = nullptr;
    const bool ok = QMetaObject::invokeMethod(plugin, "createWidget", Qt::DirectConnection,
                                              Q_RETURN_ARG(QWidget*, widget),
                                              Q_ARG(LogosAPI*, api));
    if (!ok || !widget)
        QMetaObject::invokeMethod(plugin, "createWidget", Qt::DirectConnection,
                                  Q_RETURN_ARG(QWidget*, widget));
    return widget;
}

} // namespace

UiPluginLoader::UiPluginLoader(LogosAPI* hostApi, LoadDependency loadDependency,
                               QObject* parent)
    : QObject(parent)
    , m_hostApi(hostApi)
    , m_loadDependency(std::move(loadDependency))
    , m_dependencyLoader(new CoreDependencyLoader(this))
{
}

UiPluginLoader::~UiPluginLoader() = default;

void UiPluginLoader::load(const UiPluginRequest& request)
{
    if (isLoading(request.name)) {
        qDebug() << "Plugin" << request.name << "is already loading";
        return;
    }
    setLoading(request.name, true);

    // A zero timer paints no frame of the spinner before the load blocks.
    QTimer::singleShot(spinnerPaintDelayMs(), this, [this, request]() {
        startLoad(request);
    });
}

bool UiPluginLoader::isLoading(const QString& name) const
{
    QMutexLocker lock(&m_mutex);
    return m_loading.contains(name);
}

QStringList UiPluginLoader::loadingPlugins() const
{
    QMutexLocker lock(&m_mutex);
    return m_loading.values();
}

void UiPluginLoader::setLoading(const QString& name, bool loading)
{
    {
        QMutexLocker lock(&m_mutex);
        if (loading)
            m_loading.insert(name);
        else
            m_loading.remove(name);
    }
    emit loadingChanged();
}

void UiPluginLoader::fail(const QString& name, const QString& error)
{
    qWarning().noquote() << "Failed to load UI plugin" << name << "-" << error;
    setLoading(name, false);
    emit pluginLoadFailed(name, error);
}

logos::ConsumerIdentity UiPluginLoader::consumerFor(const QString& name)
{
    if (name.isEmpty()) {
        qWarning() << "UiPluginLoader: refusing to build an identity for an unnamed plugin";
        return {};
    }
    const auto it = m_consumers.constFind(name);
    if (it != m_consumers.constEnd())
        return it.value();

    // Isolated store, minted credential registered over the host's trusted
    // channel. Falling back to the host's identity would grant its authority.
    logos::ConsumerIdentity consumer = logos::admitConsumer(name, m_hostApi, this);
    if (!consumer) {
        qWarning() << "UiPluginLoader: could not admit" << name << "as a consumer";
        return {};
    }
    m_consumers.insert(name, consumer);
    return consumer;
}

void UiPluginLoader::startLoad(const UiPluginRequest& request)
{
    if (request.dependencies.isEmpty() && request.optionalDependencies.isEmpty()) {
        continueLoad(request);
        return;
    }
    loadDependencies(request);
}

void UiPluginLoader::loadDependencies(const UiPluginRequest& request)
{
    QStringList required;
    for (const QVariant& dep : request.dependencies) {
        const logos::DependencyEntry entry = logos::readDependencyEntry(dep);
        if (entry.kind == logos::DependencyEntryKind::Unrecognised) {
            // Mounting with a declared dependency silently unloaded fails later
            // and far from here.
            qWarning() << "Unrecognised dependency entry" << dep << "for" << request.name;
            fail(request.name, QStringLiteral("Unrecognised dependency entry in ")
                                   + request.name + QStringLiteral("'s manifest"));
            return;
        }
        required << entry.name;
    }
    QStringList optional;
    for (const QVariant& dep : request.optionalDependencies) {
        const logos::DependencyEntry entry = logos::readDependencyEntry(dep);
        if (entry.kind == logos::DependencyEntryKind::Unrecognised) {
            qWarning() << "Unrecognised optional dependency entry" << dep
                       << "for" << request.name << "- skipping";
            continue;
        }
        optional << entry.name;
    }

    qDebug() << "Loading core dependencies for" << request.name << ":" << required;
    // The worker gets a copy of the callback, never `this`.
    m_dependencyLoader->enqueue(required, optional,
        [load = m_loadDependency, name = request.name](const QString& dep, bool isRequired) {
            if (!load)
                return false;
            if (load(dep, isRequired))
                return true;
            if (!isRequired)
                qInfo() << "Optional dependency" << dep << "for" << name
                        << "is unavailable; continuing without it";
            return false;
        },
        [this, request]() { continueLoad(request); },
        [this, request](const QString& failed) {
            fail(request.name, QStringLiteral("Failed to load core dependency ")
                                   + failed + QStringLiteral(" for ") + request.name);
        });
}

void UiPluginLoader::continueLoad(const UiPluginRequest& request)
{
    switch (request.kind) {
    case UiPluginKind::UiQml:
        loadUiQml(request);
        break;
    case UiPluginKind::Legacy:
        loadLegacyAsync(request);
        break;
    }
}

// ---------- legacy plugins ----------

void UiPluginLoader::loadLegacyAsync(const UiPluginRequest& request)
{
    // Map the library off the GUI thread; QPluginLoader's cache makes the
    // main-thread load below instant. The plugin's own directory joins the
    // DLL search so libraries vendored beside it resolve (no-op off Windows).
    QThread* thread = QThread::create([path = request.pluginPath]() {
        ModuleLib::preloadPluginWithOwnDirSearch(path);
        QPluginLoader loader(path);
        loader.load();
    });
    connect(thread, &QThread::finished, this, [this, thread, request]() {
        thread->deleteLater();
        finishLegacyLoad(request);
    });
    thread->start();
}

void UiPluginLoader::finishLegacyLoad(const UiPluginRequest& request)
{
    ModuleLib::preloadPluginWithOwnDirSearch(request.pluginPath);
    QPluginLoader loader(request.pluginPath);
    if (!loader.load()) {
        fail(request.name, loader.errorString());
        return;
    }
    QObject* plugin = loader.instance();
    if (!plugin) {
        fail(request.name, QStringLiteral("Failed to get plugin instance"));
        return;
    }
    IComponent* component = qobject_cast<IComponent*>(plugin);
    if (!component && !m_acceptInvokable) {
        loader.unload();
        fail(request.name, QStringLiteral("Plugin does not implement IComponent"));
        return;
    }

    // The plugin's own identity: it calls modules through this LogosAPI.
    const logos::ConsumerIdentity consumer = consumerFor(request.name);
    if (!consumer) {
        loader.unload();
        fail(request.name, QStringLiteral("Could not establish an isolated identity for ")
                               + request.name);
        return;
    }

    QWidget* widget = component ? component->createWidget(consumer.api)
                                : createWidgetByInvocation(plugin, consumer.api);
    if (!widget) {
        loader.unload();
        fail(request.name, QStringLiteral("Component returned null widget"));
        return;
    }
    if (!request.iconPath.isEmpty())
        widget->setWindowIcon(QIcon(request.iconPath));

    setLoading(request.name, false);
    emit pluginLoaded(request.name, widget, component, UiPluginKind::Legacy, nullptr, nullptr);
}

// ---------- ui_qml plugins ----------

void UiPluginLoader::loadUiQml(const UiPluginRequest& request)
{
    if (request.qmlViewPath.isEmpty() || !QFile::exists(request.qmlViewPath)) {
        fail(request.name, QStringLiteral("QML view file not found: ") + request.qmlViewPath);
        return;
    }

    // One admission for both paths: it makes the plugin a known caller for the
    // QML's first request, and its credential is what ui-host serves with.
    const logos::ConsumerIdentity consumer = consumerFor(request.name);
    if (!consumer) {
        fail(request.name, QStringLiteral("Could not establish an isolated identity for ")
                               + request.name);
        return;
    }
    auto* bridge = new LogosQmlBridge(consumer.api, this);

    if (request.mainFilePath.isEmpty()) {
        loadQmlView(request, bridge, nullptr);
        return;
    }

    // Admission already registered the credential, so a backend constructor's
    // first call from ui-host is not refused.
    auto* viewHost = new ViewModuleHost(this);
    if (!viewHost->spawn(request.name, request.mainFilePath, consumer.credential)) {
        delete viewHost;
        delete bridge;
        fail(request.name, QStringLiteral("Failed to spawn ui-host for ") + request.name);
        return;
    }

    auto onHostReady = [this, request, bridge, viewHost]() {
        bridge->setViewModuleSocket(request.name, viewHost->socketName(),
                                    viewHost->sourceName());
        auto exitConn = std::make_shared<QMetaObject::Connection>(
            connect(viewHost, &ViewModuleHost::processExited, bridge,
                [bridge, name = request.name](int) { bridge->notifyViewModuleCrashed(name); }));
        bridge->prepareViewModule(request.name, kBackendReadyTimeoutMs,
            [this, request, bridge, viewHost, exitConn](bool ok, const QString& error) {
                QObject::disconnect(*exitConn);
                if (!ok) {
                    viewHost->stop();
                    viewHost->deleteLater();
                    delete bridge;
                    fail(request.name, QStringLiteral("Backend of ") + request.name
                                           + QStringLiteral(" not ready: ") + error);
                    return;
                }
                loadQmlView(request, bridge, viewHost);
            });
    };

    auto* timeout = new QTimer(this);
    timeout->setSingleShot(true);
    auto readyConn = std::make_shared<QMetaObject::Connection>();
    auto timeoutConn = std::make_shared<QMetaObject::Connection>();
    *readyConn = connect(viewHost, &ViewModuleHost::ready, this,
        [timeout, readyConn, timeoutConn, onHostReady]() {
            QObject::disconnect(*readyConn);
            QObject::disconnect(*timeoutConn);
            timeout->stop();
            timeout->deleteLater();
            onHostReady();
        });
    *timeoutConn = connect(timeout, &QTimer::timeout, this,
        [this, request, viewHost, bridge, timeout, readyConn, timeoutConn]() {
            QObject::disconnect(*readyConn);
            QObject::disconnect(*timeoutConn);
            timeout->deleteLater();
            viewHost->stop();
            viewHost->deleteLater();
            delete bridge;
            fail(request.name, QStringLiteral("Timeout waiting for ui-host for ") + request.name);
        });
    timeout->start(kUiHostReadyTimeoutMs);
}

void UiPluginLoader::loadQmlView(const UiPluginRequest& request, LogosQmlBridge* bridge,
                                 ViewModuleHost* viewHost)
{
    const QString appLibDir =
        QDir(QCoreApplication::applicationDirPath() + "/../lib").canonicalPath();
    auto prepare = [request, appLibDir](QQmlEngine* engine) {
        QmlSandbox::configure(engine, request.installDir, request.qmlViewPath,
                              appLibDir, request.name);
        engine->setBaseUrl(QUrl::fromLocalFile(request.installDir + "/"));
        engine->rootContext()->setContextProperty("isActiveTab", true);
    };

    if (request.liveReload) {
        // The live view compiles on construction; a failed first compile keeps
        // the container so the next save can bring the view up.
        if (m_bridgeSetup)
            m_bridgeSetup(request.name, bridge);
        auto* view = new QmlLiveView(request.installDir, request.qmlViewPath, bridge, prepare);
        bridge->setParent(view);
        if (!request.iconPath.isEmpty())
            view->setWindowIcon(QIcon(request.iconPath));
        setLoading(request.name, false);
        emit pluginLoaded(request.name, view, nullptr, UiPluginKind::UiQml, viewHost, bridge);
        return;
    }

    auto* qmlWidget = new QQuickWidget;
    qmlWidget->setResizeMode(QQuickWidget::SizeRootObjectToView);
    prepare(qmlWidget->engine());

    // Compile asynchronously; setSource() then hits the engine's type cache.
    auto* preloader = new QQmlComponent(qmlWidget->engine(),
                                        QUrl::fromLocalFile(request.qmlViewPath),
                                        QQmlComponent::Asynchronous);
    auto finishOrCleanup = [this, preloader, qmlWidget, request, bridge,
                            viewHost](QQmlComponent::Status status) {
        preloader->deleteLater();
        if (status == QQmlComponent::Ready) {
            finishQmlView(qmlWidget, request, bridge, viewHost);
            return;
        }
        QString errors;
        for (const auto& e : preloader->errors())
            errors += e.toString() + QStringLiteral("\n");
        qmlWidget->deleteLater();
        delete bridge;
        if (viewHost) { viewHost->stop(); delete viewHost; }
        fail(request.name, errors);
    };
    if (preloader->isReady() || preloader->isError())
        finishOrCleanup(preloader->status());
    else
        connect(preloader, &QQmlComponent::statusChanged, this, finishOrCleanup);
}

void UiPluginLoader::finishQmlView(QQuickWidget* qmlWidget, const UiPluginRequest& request,
                                   LogosQmlBridge* bridge, ViewModuleHost* viewHost)
{
    // The engine, not the widget: the replica must be gone before the engine
    // prunes QML's property-cache entry for it (see LogosQmlBridge.h, module()).
    bridge->setParent(qmlWidget->engine());
    // Before setSource(), which is where the QML first runs.
    if (m_bridgeSetup)
        m_bridgeSetup(request.name, bridge);
    qmlWidget->rootContext()->setContextProperty("logos", bridge);
    qmlWidget->setSource(QUrl::fromLocalFile(request.qmlViewPath));
    if (!request.iconPath.isEmpty())
        qmlWidget->setWindowIcon(QIcon(request.iconPath));

    if (qmlWidget->status() == QQuickWidget::Error) {
        QString errors;
        for (const QQmlError& error : qmlWidget->errors())
            errors += error.toString() + QStringLiteral("\n");
        qmlWidget->deleteLater();
        if (viewHost) { viewHost->stop(); delete viewHost; }
        fail(request.name, QStringLiteral("Failed to load QML view for ") + request.name
                               + QStringLiteral("\n") + errors);
        return;
    }

    // The backend went ready before this QML existed (see onHostReady), so tell
    // the view now; views wait for viewModuleReadyChanged to show content.
    bridge->replayViewModuleState();

    setLoading(request.name, false);
    emit pluginLoaded(request.name, qmlWidget, nullptr, UiPluginKind::UiQml, viewHost, bridge);
}

} // namespace logos::ui
