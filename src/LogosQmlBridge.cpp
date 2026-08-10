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
#include "logos_call_error.h"
#include "logos_object.h"
#include "logos_json_convert.h"  // logos::qvariantToNlohmann (canonical result serialization)

namespace {
QString makeErrorPayload(const QString& error,
                         const QString& module = QString(),
                         const QString& method = QString(),
                         const QString& detail = QString())
{
    QJsonObject obj;
    obj.insert(QStringLiteral("error"), error);
    if (!module.isEmpty()) obj.insert(QStringLiteral("module"), module);
    if (!method.isEmpty()) obj.insert(QStringLiteral("method"), method);
    if (!detail.isEmpty()) obj.insert(QStringLiteral("message"), detail);
    return QString::fromUtf8(QJsonDocument(obj).toJson(QJsonDocument::Compact));
}

} // namespace

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
        return makeErrorPayload(QStringLiteral("Module not connected"), module);

    logos::CallError err;
    QVariant result = client->invokeRemoteMethod(module, method, args,
                                                 Timeout(), &err);
    if (!err.ok()) {
        return makeErrorPayload(QStringLiteral("Module source unavailable"),
                                module, method,
                                QString::fromStdString(err.message));
    }
    if (!result.isValid())
        return makeErrorPayload(QStringLiteral("Invalid response"), module, method);

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
        invokeCallback(makeErrorPayload(QStringLiteral("Module not connected"), module));
        return;
    }

    if (timeoutMs > 0) {
        QTimer::singleShot(timeoutMs, this, [invokeCallback, module, method]() mutable {
            invokeCallback(makeErrorPayload(QStringLiteral("timeout"), module, method));
        });
    }

    client->invokeRemoteMethodAsync(
        module, method, args,
        LogosAPIClient::AsyncResultErrorCallback(
            [invokeCallback, module, method](QVariant result, const logos::CallError& err) mutable {
                if (!err.ok()) {
                    invokeCallback(makeErrorPayload(
                        QStringLiteral("Module source unavailable"),
                        module, method,
                        QString::fromStdString(err.message)));
                    return;
                }
                if (!result.isValid()) {
                    invokeCallback(makeErrorPayload(
                        QStringLiteral("Invalid response"), module, method));
                    return;
                }
                invokeCallback(LogosQmlBridge::serializeResultForTesting(result));
            }));
}

void LogosQmlBridge::watch(const QVariant& pendingCall,
                           QJSValue onSuccess,
                           QJSValue onError)
{
    auto call = pendingCall.value<QRemoteObjectPendingCall>();

    // Convert QtRO's returnValue (a QVariant) to a JS value preserving its
    // type (int → number, bool → bool, QString → string, QVariantMap → object,
    // …).
    auto toJs = [this](const QVariant& v) -> QJSValue {
        if (auto* engine = qjsEngine(this))
            return engine->toScriptValue(v);
        return QJSValue(v.toString());
    };

    if (call.isFinished()) {
        if (onSuccess.isCallable()) {
            onSuccess.call(QJSValueList() << toJs(call.returnValue()));
        }
        return;
    }

    auto* watcher = new QRemoteObjectPendingCallWatcher(call, this);
    connect(watcher, &QRemoteObjectPendingCallWatcher::finished, this,
        [onSuccess, onError, watcher, toJs]() mutable {
            QVariant rv = watcher->returnValue();
            if (rv.isValid() && onSuccess.isCallable()) {
                onSuccess.call(QJSValueList() << toJs(rv));
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

QObject* LogosQmlBridge::model(const QString& moduleName, const QString& modelName,
                                bool prefetch)
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

    const auto initialAction = prefetch ? QtRemoteObjects::PrefetchData
                                        : QtRemoteObjects::FetchRootSize;
    QAbstractItemModelReplica* m = node->acquireModel(key, initialAction);
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

bool LogosQmlBridge::onModuleEvent(const QString& moduleName, const QString& eventName)
{
    // This runs from Component.onCompleted, i.e. while the QML view is being
    // built — which in Basecamp is immediately after PluginLoader has *spawned*
    // the core dependency's host process, and well before that process has
    // called listen(). It therefore must not ask "is the module connected?"
    // and must not block waiting for a replica.
    //
    // It used to do both: `if (!client->isConnected()) return false;` followed
    // by a blocking requestObject(). Once logos-protocol 1238316 made
    // isConnected() truthful, that guard started refusing every subscription
    // made at view-construction time, one shot, with nothing recorded and no
    // retry — so QML events never arrived while method calls kept working
    // (calls reach the replica by a path that never asks). Reverting 1238316 is
    // not the answer; it removed ~417 s (macOS) / 361 s (Linux) of blocked GUI
    // thread at startup. The subscription becomes deferrable instead.
    if (!m_logosAPI) {
        qWarning() << "LogosQmlBridge::onModuleEvent: LogosAPI not available";
        return false;
    }
    if (moduleName.isEmpty() || eventName.isEmpty()) {
        qWarning() << "LogosQmlBridge::onModuleEvent: empty module or event name"
                   << moduleName << eventName;
        return false;
    }
    if (m_viewModuleSockets.contains(moduleName)) {
        // A view module's signals come off its typed replica, not the IPC event
        // channel; subscribing here would wait forever for something that never
        // publishes on that name.
        // One formatted string, not a stream of operands: QDebug quotes each
        // QString it is given and separates operands with a space, which turns
        // the QML snippet below into logos.module(" "chat_module" ") — the one
        // part of this message a reader is meant to copy.
        qWarning().noquote()
            << QStringLiteral("LogosQmlBridge::onModuleEvent: %1 is a VIEW module -- "
                              "use logos.module(\"%1\") and a Connections block for "
                              "its signals").arg(moduleName);
        return false;
    }

    LogosAPIClient* client = m_logosAPI->getClient(moduleName);
    if (!client) {
        qWarning() << "LogosQmlBridge::onModuleEvent: no client for" << moduleName;
        return false;
    }

    // Idempotent: QML may re-run Component.onCompleted. But CHECK the record
    // against the registry rather than trusting it — a subscription this bridge
    // believes is live may have been dropped underneath it, and short-circuiting
    // on a stale record would turn a legitimate re-subscribe into a silent
    // success that never arms. Unknown means the registry is not tracking that
    // id at all, so fall through and subscribe again.
    const QPair<QString, QString> key(moduleName, eventName);
    auto known = m_eventSubscriptions.constFind(key);
    if (known != m_eventSubscriptions.constEnd()) {
        if (client->eventSubscriptionState(known.value()) != LogosSubscriptionState::Unknown)
            return true;
        qDebug() << "LogosQmlBridge::onModuleEvent: stale subscription record for"
                 << moduleName << "::" << eventName << "-- re-subscribing";
        m_eventSubscriptions.remove(key);
    }

    QPointer<LogosQmlBridge> self(this);
    const QString mod = moduleName;
    // Non-blocking by construction: no isConnected() probe, no requestObject(),
    // no nested event loop on this path. The subscription is armed by the
    // transport when the module becomes reachable, and the layer below owns the
    // diagnostics (warn once on deferral, log on arm, warn loudly if it ever
    // becomes impossible).
    const quint64 id = client->onEventWhenAvailable(
        moduleName, eventName,
        [self, mod](const QString& event, const QVariantList& data) {
            if (self)
                emit self->moduleEventReceived(mod, event, data);
        },
        [self, key](bool armed) {
            // Abandoned subscriptions must not linger in the de-dupe set, or a
            // later retry from QML would be swallowed as a duplicate.
            if (self && !armed)
                self->m_eventSubscriptions.remove(key);
        });
    if (!id) {
        qWarning() << "LogosQmlBridge::onModuleEvent: subscription refused for"
                   << moduleName << "::" << eventName;
        return false;
    }
    m_eventSubscriptions.insert(key, id);

    qDebug() << "LogosQmlBridge: subscription accepted for" << moduleName << "::" << eventName;
    return true;
}

QStringList LogosQmlBridge::pendingEventSubscriptions() const
{
    if (!m_logosAPI) return QStringList();
    QStringList out;
    QSet<QString> seen;
    for (auto it = m_eventSubscriptions.keyBegin(), end = m_eventSubscriptions.keyEnd();
         it != end; ++it) {
        const QString& module = (*it).first;
        if (seen.contains(module)) continue;
        seen.insert(module);
        if (LogosAPIClient* client = m_logosAPI->getClient(module))
            out += client->pendingEventSubscriptions();
    }
    return out;
}

// ── Helpers ─────────────────────────────────────────────────────────────────

QString LogosQmlBridge::serializeResultForTesting(const QVariant& result)
{
    if (!result.isValid()) return QStringLiteral("null");

    // Serialize with logos-protocol's canonical QVariant->JSON converter, the
    // same one every transport (lp/std/cdylib) already uses, so ALL types
    // round-trip to QML/JS identically: scalars as bare literals, containers
    // preserved, integers kept as integers (not degraded to double), bytes in
    // the tagged {"_bytes":...} form, and the custom LogosResult metatype as
    // {success, value, error}. QJsonValue::fromVariant (used previously) could
    // not convert a LogosResult — it silently degraded a `result`-type return
    // to the literal "null" — and lost int64 precision.
    return QString::fromStdString(logos::qvariantToNlohmann(result).dump());
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
