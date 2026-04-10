#include "LogosQmlBridge.h"
#include "LogosViewReplicaFactory.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QJSEngine>
#include <QQmlEngine>
#include <QRemoteObjectNode>
#include <QRemoteObjectReplica>
#include <QRemoteObjectPendingCall>
#include <QAbstractItemModelReplica>
#include <QPluginLoader>
#include <QFileInfo>
#include <QTimer>
#include <QPointer>
#include <QDebug>
#include <memory>

#include "logos_api.h"
#include "logos_api_client.h"

LogosQmlBridge::LogosQmlBridge(LogosAPI* api, QObject* parent)
    : QObject(parent)
    , m_logosAPI(api)
{
}

QString LogosQmlBridge::callModule(const QString& module,
                                   const QString& method,
                                   const QVariantList& args)
{
    if (m_viewModuleSockets.contains(module)) {
        qWarning() << "LogosQmlBridge::callModule:" << module
                   << "is a view module — use logos.module(\"" << module
                   << "\")." << method << "(...) instead.";
        return QStringLiteral("{\"error\":\"view modules must be called via logos.module()\"}");
    }

    if (!m_logosAPI)
        return QStringLiteral("{\"error\":\"LogosAPI not available\"}");

    LogosAPIClient* client = m_logosAPI->getClient(module);
    if (!client || !client->isConnected())
        return QStringLiteral("{\"error\":\"Module not connected\"}");

    QVariant result = client->invokeRemoteMethod(module, method, args);
    if (!result.isValid())
        return QStringLiteral("{\"error\":\"Invalid response\"}");

    return LogosQmlBridge::serializeResultForTesting(result);
}

void LogosQmlBridge::callModuleAsync(const QString& module,
                                     const QString& method,
                                     const QVariantList& args,
                                     QJSValue callback,
                                     int timeoutMs)
{
    auto fired = std::make_shared<bool>(false);
    auto invokeCallback = [callback, fired](const QString& payload) mutable {
        if (*fired) return;
        *fired = true;
        if (callback.isCallable()) {
            callback.call(QJSValueList() << QJSValue(payload));
        }
    };

    if (m_viewModuleSockets.contains(module)) {
        qWarning() << "LogosQmlBridge::callModuleAsync:" << module
                   << "is a view module — use logos.module() instead.";
        invokeCallback("{\"error\":\"view modules must be called via logos.module()\"}");
        return;
    }

    if (!m_logosAPI) {
        invokeCallback("{\"error\":\"LogosAPI not available\"}");
        return;
    }

    LogosAPIClient* client = m_logosAPI->getClient(module);
    if (!client || !client->isConnected()) {
        invokeCallback("{\"error\":\"Module not connected\"}");
        return;
    }

    if (timeoutMs > 0) {
        QTimer::singleShot(timeoutMs, this, [invokeCallback, module, method]() mutable {
            invokeCallback(QStringLiteral("{\"error\":\"timeout\",\"module\":\"%1\",\"method\":\"%2\"}")
                               .arg(module, method));
        });
    }

    client->invokeRemoteMethodAsync(
        module, method, args,
        [this, invokeCallback](QVariant result) mutable {
            if (!result.isValid()) {
                invokeCallback("{\"error\":\"Invalid response\"}");
                return;
            }
            invokeCallback(LogosQmlBridge::serializeResultForTesting(result));
        });
}

void LogosQmlBridge::watch(const QVariant& pendingCall,
                           QJSValue onSuccess,
                           QJSValue onError)
{
    auto call = pendingCall.value<QRemoteObjectPendingCall>();

    if (call.isFinished()) {
        if (onSuccess.isCallable()) {
            onSuccess.call(QJSValueList() << QJSValue(call.returnValue().toString()));
        }
        return;
    }

    auto* watcher = new QRemoteObjectPendingCallWatcher(call, this);
    connect(watcher, &QRemoteObjectPendingCallWatcher::finished, this,
        [onSuccess, onError, watcher]() mutable {
            QVariant rv = watcher->returnValue();
            if (rv.isValid() && onSuccess.isCallable()) {
                onSuccess.call(QJSValueList() << QJSValue(rv.toString()));
            } else if (!rv.isValid() && onError.isCallable()) {
                onError.call(QJSValueList() << QJSValue(QStringLiteral("call failed")));
            }
            watcher->deleteLater();
        });
}

// ── View module API ─────────────────────────────────────────────────────────

QObject* LogosQmlBridge::module(const QString& moduleName)
{
    auto it = m_replicas.constFind(moduleName);
    if (it != m_replicas.cend() && it.value()) {
        QQmlEngine::setObjectOwnership(it.value(), QQmlEngine::CppOwnership);
        return it.value();
    }

    auto* factory = loadFactory(moduleName);
    if (!factory) return nullptr;

    auto* node = getOrCreateNode(moduleName);
    if (!node) return nullptr;

    QObject* replica = factory->acquire(node);
    if (!replica) {
        qWarning() << "LogosQmlBridge::module: factory->acquire() returned null for"
                   << moduleName;
        return nullptr;
    }
    replica->setParent(this);
    m_replicas[moduleName] = replica;
    QQmlEngine::setObjectOwnership(replica, QQmlEngine::CppOwnership);

    // Forward replica readiness as a signal QML can bind to.
    if (auto* rep = qobject_cast<QRemoteObjectReplica*>(replica)) {
        QPointer<LogosQmlBridge> self(this);
        QString name = moduleName;
        QObject::connect(rep, &QRemoteObjectReplica::stateChanged, this,
            [self, name](QRemoteObjectReplica::State newState,
                         QRemoteObjectReplica::State /*old*/) {
                if (!self) return;
                const bool ready = (newState == QRemoteObjectReplica::Valid);
                emit self->viewModuleReadyChanged(name, ready);
            });
        if (rep->state() == QRemoteObjectReplica::Valid) {
            emit viewModuleReadyChanged(moduleName, true);
        }
    }

    return replica;
}

QObject* LogosQmlBridge::model(const QString& moduleName, const QString& modelName)
{
    const QString key = moduleName + QLatin1Char('/') + modelName;
    auto it = m_modelReplicas.constFind(key);
    if (it != m_modelReplicas.cend()) {
        if (auto* obj = static_cast<QObject*>(it.value())) {
            QQmlEngine::setObjectOwnership(obj, QQmlEngine::CppOwnership);
        }
        return static_cast<QObject*>(it.value());
    }

    auto* node = getOrCreateNode(moduleName);
    if (!node) {
        qWarning() << "LogosQmlBridge::model: no node for" << moduleName;
        return nullptr;
    }

    QAbstractItemModelReplica* m = node->acquireModel(key);
    if (!m) {
        qWarning() << "LogosQmlBridge::model: acquireModel failed for" << key;
        return nullptr;
    }
    m_modelReplicas[key] = m;

    QObject* asObj = static_cast<QObject*>(m);
    QQmlEngine::setObjectOwnership(asObj, QQmlEngine::CppOwnership);
    return asObj;
}

bool LogosQmlBridge::isViewModuleReady(const QString& moduleName) const
{
    auto it = m_replicas.constFind(moduleName);
    if (it == m_replicas.cend() || !it.value()) return false;
    if (auto* rep = qobject_cast<QRemoteObjectReplica*>(it.value())) {
        return rep->state() == QRemoteObjectReplica::Valid;
    }
    return false;
}

void LogosQmlBridge::setViewModuleSocket(const QString& moduleName,
                                         const QString& socketName)
{
    auto currentIt = m_viewModuleSockets.constFind(moduleName);
    if (currentIt != m_viewModuleSockets.cend() && currentIt.value() == socketName) {
        return;
    }
    dropViewModuleCaches(moduleName);
    m_viewModuleSockets[moduleName] = socketName;
}

void LogosQmlBridge::setViewReplicaPlugin(const QString& moduleName,
                                          const QString& pluginPath)
{
    auto currentIt = m_replicaPluginPaths.constFind(moduleName);
    if (currentIt != m_replicaPluginPaths.cend() && currentIt.value() == pluginPath) {
        return;
    }
    if (auto repIt = m_replicas.find(moduleName); repIt != m_replicas.end()) {
        if (auto* r = repIt.value()) r->deleteLater();
        m_replicas.erase(repIt);
    }
    if (auto facIt = m_factories.find(moduleName); facIt != m_factories.end()) {
        m_factories.erase(facIt);
    }
    if (auto loaderIt = m_factoryLoaders.find(moduleName); loaderIt != m_factoryLoaders.end()) {
        if (auto* l = loaderIt.value()) {
            l->unload();
            l->deleteLater();
        }
        m_factoryLoaders.erase(loaderIt);
    }
    m_replicaPluginPaths[moduleName] = pluginPath;

    (void)loadFactory(moduleName);
}

void LogosQmlBridge::notifyViewModuleCrashed(const QString& moduleName)
{
    qWarning() << "LogosQmlBridge: view module crashed" << moduleName;
    dropViewModuleCaches(moduleName);
    emit viewModuleReadyChanged(moduleName, false);
    emit viewModuleCrashed(moduleName);
}

// ── Helpers ─────────────────────────────────────────────────────────────────

QString LogosQmlBridge::serializeResultForTesting(const QVariant& result)
{
    if (!result.isValid()) return QStringLiteral("null");

    QJsonValue jsonValue = QJsonValue::fromVariant(result);
    if (jsonValue.isObject()) {
        return QString::fromUtf8(QJsonDocument(jsonValue.toObject())
                                     .toJson(QJsonDocument::Compact));
    }
    if (jsonValue.isArray()) {
        return QString::fromUtf8(QJsonDocument(jsonValue.toArray())
                                     .toJson(QJsonDocument::Compact));
    }
    if (!jsonValue.isUndefined()) {
        QJsonDocument doc(QJsonArray{jsonValue});
        QString wrapped = QString::fromUtf8(doc.toJson(QJsonDocument::Compact));
        if (wrapped.size() >= 2 && wrapped.front() == '[' && wrapped.back() == ']') {
            return wrapped.mid(1, wrapped.size() - 2);
        }
        return wrapped;
    }

    QJsonDocument doc = QJsonDocument::fromVariant(result);
    if (!doc.isNull() && !doc.isEmpty()) {
        return QString::fromUtf8(doc.toJson(QJsonDocument::Compact));
    }

    QJsonDocument docString(QJsonArray{QJsonValue(result.toString())});
    QString wrapped = QString::fromUtf8(docString.toJson(QJsonDocument::Compact));
    if (wrapped.size() >= 2 && wrapped.front() == '[' && wrapped.back() == ']') {
        return wrapped.mid(1, wrapped.size() - 2);
    }
    return wrapped;
}

QRemoteObjectNode* LogosQmlBridge::getOrCreateNode(const QString& moduleName)
{
    auto it = m_replicaNodes.constFind(moduleName);
    if (it != m_replicaNodes.cend()) return it.value();

    auto socketIt = m_viewModuleSockets.constFind(moduleName);
    if (socketIt == m_viewModuleSockets.cend()) {
        qWarning() << "LogosQmlBridge: no socket registered for view module" << moduleName;
        return nullptr;
    }

    auto* node = new QRemoteObjectNode(this);
    node->connectToNode(QUrl(QStringLiteral("local:") + socketIt.value()));
    m_replicaNodes[moduleName] = node;
    return node;
}

LogosViewReplicaFactory* LogosQmlBridge::loadFactory(const QString& moduleName)
{
    auto it = m_factories.constFind(moduleName);
    if (it != m_factories.cend() && it.value()) return it.value();

    auto pathIt = m_replicaPluginPaths.constFind(moduleName);
    if (pathIt == m_replicaPluginPaths.cend() || pathIt.value().isEmpty()) {
        qWarning() << "LogosQmlBridge: no replica factory plugin registered for"
                   << moduleName;
        return nullptr;
    }

    const QString path = pathIt.value();
    if (!QFileInfo::exists(path)) {
        qWarning() << "LogosQmlBridge: replica factory plugin not found at" << path;
        return nullptr;
    }

    auto* loader = new QPluginLoader(path, this);
    QObject* instance = loader->instance();
    if (!instance) {
        qWarning() << "LogosQmlBridge: failed to load replica factory plugin"
                   << path << ":" << loader->errorString();
        loader->deleteLater();
        return nullptr;
    }
    auto* factory = qobject_cast<LogosViewReplicaFactory*>(instance);
    if (!factory) {
        qWarning() << "LogosQmlBridge: plugin at" << path
                   << "does not implement LogosViewReplicaFactory";
        loader->unload();
        loader->deleteLater();
        return nullptr;
    }
    m_factoryLoaders[moduleName] = loader;
    m_factories[moduleName] = factory;
    return factory;
}

bool LogosQmlBridge::hasViewModuleSocket(const QString& moduleName) const
{
    return m_viewModuleSockets.contains(moduleName);
}

QString LogosQmlBridge::viewModuleSocket(const QString& moduleName) const
{
    return m_viewModuleSockets.value(moduleName);
}

QString LogosQmlBridge::viewReplicaPluginPath(const QString& moduleName) const
{
    return m_replicaPluginPaths.value(moduleName);
}

void LogosQmlBridge::dropViewModuleCaches(const QString& moduleName)
{
    if (auto repIt = m_replicas.find(moduleName); repIt != m_replicas.end()) {
        if (auto* r = repIt.value()) r->deleteLater();
        m_replicas.erase(repIt);
    }

    const QString prefix = moduleName + QLatin1Char('/');
    for (auto mIt = m_modelReplicas.begin(); mIt != m_modelReplicas.end(); ) {
        if (mIt.key().startsWith(prefix)) {
            if (auto* m = mIt.value()) reinterpret_cast<QObject*>(m)->deleteLater();
            mIt = m_modelReplicas.erase(mIt);
        } else {
            ++mIt;
        }
    }

    if (auto facIt = m_factories.find(moduleName); facIt != m_factories.end()) {
        m_factories.erase(facIt);
    }
    if (auto loaderIt = m_factoryLoaders.find(moduleName); loaderIt != m_factoryLoaders.end()) {
        if (auto* l = loaderIt.value()) {
            l->unload();
            l->deleteLater();
        }
        m_factoryLoaders.erase(loaderIt);
    }

    if (auto nodeIt = m_replicaNodes.find(moduleName); nodeIt != m_replicaNodes.end()) {
        if (auto* node = nodeIt.value()) node->deleteLater();
        m_replicaNodes.erase(nodeIt);
    }
}
