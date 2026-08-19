// ── The per-plugin identity acceptance test ─────────────────────────────────
//
// This reproduces, and then closes, the measured escalation:
//
//   A pure-QML module runs with the host's AMBIENT TOKEN RING, not merely the
//   host's name. logos-liblogos' module_manager writes `name -> that module's
//   root auth token` into the process-global TokenManager::instance() for
//   EVERY loaded module. On the hot path a client asserts no identity at all —
//   LogosAPIClient::invokeRemoteMethod reads the store first and only mints on
//   a miss — so a QML view handed the HOST's LogosAPI finds the target's own
//   root token already sitting there, presents it, and the provider (which
//   accepts any token in its image's store) authorises. No `requestModule`
//   appears anywhere, so no policy is ever consulted.
//
// The fixture below is that world, built out of real objects: two published
// providers over real QtRO transports, an ambient ring pre-seeded exactly the
// way module_manager pre-seeds it, and a capability_module that mints only for
// declared (origin, target) pairs.
//
// Every one of these tests would pass just as happily if `origin` were fixed
// and nothing else — which is the trap this work exists to avoid — EXCEPT that
// the assertions are about the handshake COUNT and the STORE CONTENTS, never
// about the name a call carries.
#include "LogosQmlBridge.h"

#include "logos_api.h"
#include "logos_instance.h"
#include "logos_mode.h"
#include "logos_provider_interface.h"
#include "module_proxy.h"
#include "remote_transport.h"
#include "token_manager.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <QString>
#include <QTest>
#include <QVariant>
#include <QVariantList>

namespace {

constexpr const char* kBackendRootToken = "backend-root-token-0001";
constexpr const char* kCapToken         = "cap-token-0001";

// A perfectly ordinary backend module. It declares no opinion about who may
// call it — the point being that the refusal, when it comes, is capability
// policy and not something this object did.
class BackendProvider : public LogosProviderObject {
public:
    int calls = 0;
    QVariant callMethod(const QString& method, const QVariantList& args) override {
        if (method == QLatin1String("echo") && !args.isEmpty()) {
            ++calls;
            return args.first();
        }
        return QVariant();
    }
    bool informModuleToken(const QString&, const QString&) override { return true; }
    QJsonArray getMethods() override { return QJsonArray{}; }
    void setEventListener(EventCallback) override {}
    void init(void*) override {}
    QString providerName() const override { return QStringLiteral("backend_module"); }
    QString providerVersion() const override { return QStringLiteral("1.0.0"); }
};

// capability_module with the two gates that matter here:
//   * a known-caller gate, standing in for the real one's tokenKeys() lookup;
//   * an access policy — `declared` is the (origin -> targets) set a module
//     actually declared as dependencies.
// A refusal is an empty mint, exactly as the real module returns {}.
class CapabilityProvider : public LogosProviderObject {
public:
    ModuleProxy* backendProxy = nullptr;
    QSet<QString> knownCallers;                  // origins registered by the host
    QHash<QString, QSet<QString>> declared;      // origin -> declared targets
    int requestModuleCalls = 0;
    QStringList refusedOrigins;

    QVariant callMethod(const QString& method, const QVariantList& args) override {
        if (method == QLatin1String("requestModule") && args.size() >= 2) {
            ++requestModuleCalls;
            const QString origin = args[0].toString();
            const QString target = args[1].toString();
            if (!knownCallers.contains(origin)) {
                refusedOrigins << origin;
                return QString();          // unknown caller — fail closed
            }
            if (!declared.value(origin).contains(target)) {
                refusedOrigins << origin;
                return QString();          // undeclared target — policy denies
            }
            const QString mint = QStringLiteral("minted-for-%1").arg(origin);
            if (backendProxy) backendProxy->saveToken(origin, mint);
            return mint;
        }
        return QVariant();
    }
    bool informModuleToken(const QString&, const QString&) override { return true; }
    QJsonArray getMethods() override { return QJsonArray{}; }
    void setEventListener(EventCallback) override {}
    void init(void*) override {}
    QString providerName() const override { return QStringLiteral("capability_module"); }
    QString providerVersion() const override { return QStringLiteral("1.0.0"); }
};

// The host process: two live providers plus the ambient ring a real host has.
struct HostFixture {
    RemoteTransportHost backendHost;
    BackendProvider backend;
    ModuleProxy backendProxy;

    RemoteTransportHost capHost;
    CapabilityProvider cap;
    ModuleProxy capProxy;

    HostFixture()
        : backendHost(LogosInstance::id("backend_module"))
        , backendProxy(&backend)
        , capHost(LogosInstance::id("capability_module"))
        , capProxy(&cap)
    {
        LogosModeConfig::setMode(LogosMode::Remote);
        cap.backendProxy = &backendProxy;

        backendHost.publishObject(QStringLiteral("backend_module"), &backendProxy);
        capHost.publishObject(QStringLiteral("capability_module"), &capProxy);

        // THE AMBIENT RING. This is the line the whole task is about:
        // module_manager.cpp does exactly this for every module it loads, and
        // it is what a host-identity caller finds when it looks up a target.
        TokenManager::instance().saveToken(QStringLiteral("backend_module"),
                                           QString::fromLatin1(kBackendRootToken));
        // The bootstrap token every host pre-seeds before any module loads.
        TokenManager::instance().saveToken(QStringLiteral("capability_module"),
                                           QString::fromLatin1(kCapToken));
    }

    // Register `identity` the way a host does before handing out a bridge.
    void registerIdentity(const QString& identity, const QStringList& declaredTargets)
    {
        cap.knownCallers.insert(identity);
        cap.declared[identity] = QSet<QString>(declaredTargets.constBegin(),
                                               declaredTargets.constEnd());
    }

    void pump(int rounds = 40)
    {
        for (int i = 0; i < rounds; ++i)
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    }
};

bool payloadIsError(const QString& json)
{
    return QJsonDocument::fromJson(json.toUtf8()).object().contains(QStringLiteral("error"));
}

} // namespace

class TestBridgeIdentity : public QObject {
    Q_OBJECT
private slots:

    // ── 1. The escalation, stated as a fact about the OLD wiring ────────────
    //
    // A bridge built on the host's LogosAPI reaches a backend it never
    // declared, and capability_module is not consulted even once. Kept green
    // deliberately: it is not the bug being fixed, it is the REASON the bridge
    // must not be given the host's LogosAPI. If this ever goes red the ambient
    // ring changed shape and every conclusion below needs re-deriving.
    void aHostIdentityBridgeReachesAnUndeclaredBackendWithNoHandshake()
    {
        HostFixture fx;
        // Note what is NOT here: no registerIdentity, no declared targets.
        LogosAPI hostApi(QStringLiteral("basecamp_host"));
        LogosQmlBridge bridge(&hostApi);

        const QString payload = bridge.callModule(
            QStringLiteral("backend_module"), QStringLiteral("echo"),
            QVariantList() << QStringLiteral("hello"));

        QVERIFY2(!payloadIsError(payload), qPrintable(payload));
        QCOMPARE(fx.backend.calls, 1);
        QCOMPARE(fx.cap.requestModuleCalls, 0);   // ← no handshake at all
    }

    // ── 2. The isolated store does not hold the target's root token ─────────
    //
    // The escalation in one line, and the sharpest single reading in this file.
    void anIsolatedIdentitysStoreLacksTheTargetsRootToken()
    {
        HostFixture fx;
        LogosQmlBridge* bridge = LogosQmlBridge::forIdentity(QStringLiteral("view_store"));
        QVERIFY(bridge != nullptr);
        QCOMPARE(bridge->identity(), QStringLiteral("view_store"));

        QVERIFY(TokenManager::isIsolated(QStringLiteral("view_store")));
        // Assert on the store the BRIDGE actually presents from, not on the
        // registry: a bridge can be labelled with an isolated identity and
        // still hold &TokenManager::instance(), which is precisely the
        // "looks fixed, isn't" shape.
        QVERIFY(bridge->tokenStore() != nullptr);
        QVERIFY(bridge->tokenStore() != &TokenManager::instance());
        // The host still has it — this is not "the ring was cleared".
        QVERIFY(TokenManager::instance().hasToken(QStringLiteral("backend_module")));
        // The identity does not.
        QVERIFY(!bridge->tokenStore()->hasToken(QStringLiteral("backend_module")));
        delete bridge;
    }

    // ── 3. …but it does carry the bootstrap, or it could never ask ─────────
    void anIsolatedIdentitysStoreCarriesTheBootstrap()
    {
        HostFixture fx;
        LogosQmlBridge* bridge = LogosQmlBridge::forIdentity(QStringLiteral("view_bootstrap"));
        QVERIFY(bridge != nullptr);

        TokenManager* store = bridge->tokenStore();
        QVERIFY(store != nullptr);
        QVERIFY(store != &TokenManager::instance());
        QCOMPARE(store->getToken(QStringLiteral("capability_module")),
                 QString::fromLatin1(kCapToken));
        // Bootstrap ONLY: "core" was never seeded by this fixture, and
        // backend_module — which the ambient ring does hold — is absent.
        QCOMPARE(store->tokenCount(), 1);
        delete bridge;
    }

    // ── 4. A declared target now costs a real handshake ────────────────────
    //
    // Same call, same providers, same process. The only thing that changed is
    // which store the bridge presents tokens from, and now capability_module
    // is in the path.
    void anIdentityBridgeHandshakesForADeclaredTarget()
    {
        HostFixture fx;
        fx.registerIdentity(QStringLiteral("view_declared"),
                            {QStringLiteral("backend_module")});

        LogosQmlBridge* bridge = LogosQmlBridge::forIdentity(QStringLiteral("view_declared"));
        QVERIFY(bridge != nullptr);

        const QString payload = bridge->callModule(
            QStringLiteral("backend_module"), QStringLiteral("echo"),
            QVariantList() << QStringLiteral("hello"));

        QVERIFY2(!payloadIsError(payload), qPrintable(payload));
        QCOMPARE(fx.backend.calls, 1);
        QCOMPARE(fx.cap.requestModuleCalls, 1);   // ← the handshake happened
        // And the token it presented is the MINTED one, not the root token.
        QCOMPARE(bridge->tokenStore()->getToken(QStringLiteral("backend_module")),
                 QStringLiteral("minted-for-view_declared"));
        delete bridge;
    }

    // ── 5. THE ACCEPTANCE TEST ─────────────────────────────────────────────
    //
    // An UNDECLARED backend is refused. Test 1 is the same call from the same
    // process against the same providers and it succeeded; the difference is
    // entirely the store.
    void anIdentityBridgeIsRefusedForAnUndeclaredBackend()
    {
        HostFixture fx;
        // Known caller — so this is a POLICY refusal, not "who are you".
        fx.registerIdentity(QStringLiteral("view_undeclared"), {});

        LogosQmlBridge* bridge = LogosQmlBridge::forIdentity(QStringLiteral("view_undeclared"));
        QVERIFY(bridge != nullptr);

        const QString payload = bridge->callModule(
            QStringLiteral("backend_module"), QStringLiteral("echo"),
            QVariantList() << QStringLiteral("hello"));

        QVERIFY2(payloadIsError(payload),
                 qPrintable(QStringLiteral("undeclared backend was REACHED: ") + payload));
        QCOMPARE(fx.backend.calls, 0);            // the method never ran
        QVERIFY(fx.cap.requestModuleCalls >= 1);  // it had to ask, and was told no
        QVERIFY(fx.cap.refusedOrigins.contains(QStringLiteral("view_undeclared")));
        // The ambient ring is untouched: the refusal is about WHO ASKED, not
        // about the token having gone missing globally.
        QCOMPARE(TokenManager::instance().getToken(QStringLiteral("backend_module")),
                 QString::fromLatin1(kBackendRootToken));
        delete bridge;
    }

    // ── 6. Two identities in one process do not share authority ────────────
    void twoIdentitiesInOneProcessDoNotShareAToken()
    {
        HostFixture fx;
        fx.registerIdentity(QStringLiteral("view_alpha"), {QStringLiteral("backend_module")});
        fx.registerIdentity(QStringLiteral("view_beta"), {});

        LogosQmlBridge* alpha = LogosQmlBridge::forIdentity(QStringLiteral("view_alpha"));
        LogosQmlBridge* beta  = LogosQmlBridge::forIdentity(QStringLiteral("view_beta"));
        QVERIFY(alpha && beta);

        QVERIFY(!payloadIsError(alpha->callModule(
            QStringLiteral("backend_module"), QStringLiteral("echo"),
            QVariantList() << QStringLiteral("a"))));

        // alpha now holds a working token for backend_module. beta must not
        // see it — under the old wiring both read the same object.
        QVERIFY(alpha->tokenStore()->hasToken(QStringLiteral("backend_module")));
        QVERIFY(alpha->tokenStore() != beta->tokenStore());
        QVERIFY(!beta->tokenStore()->hasToken(QStringLiteral("backend_module")));

        QVERIFY2(payloadIsError(beta->callModule(
                     QStringLiteral("backend_module"), QStringLiteral("echo"),
                     QVariantList() << QStringLiteral("b"))),
                 "beta rode alpha's token");
        delete alpha;
        delete beta;
    }

    // ── 7. A half-isolated identity is refused outright ────────────────────
    //
    // Once a client for a name has captured the shared store by raw pointer,
    // isolating that name would leave one client on the ambient ring and one on
    // the private store. forIdentity must return nullptr so the caller fails
    // the load rather than shipping a plugin that only looks contained.
    void forIdentityRefusesANameAlreadyOnTheSharedStore()
    {
        HostFixture fx;
        // A plain LogosAPI vends the shared store under this name.
        LogosAPI ambient(QStringLiteral("view_too_late"));
        QCOMPARE(ambient.getTokenManager(), &TokenManager::instance());

        QCOMPARE(LogosQmlBridge::forIdentity(QStringLiteral("view_too_late")),
                 static_cast<LogosQmlBridge*>(nullptr));
        QVERIFY(!TokenManager::isIsolated(QStringLiteral("view_too_late")));
    }

    // ── 8. Nothing changed for a caller that never opts in ─────────────────
    void anUnisolatedIdentityStillGetsTheImageStore()
    {
        LogosAPI plain(QStringLiteral("view_never_isolated"));
        QCOMPARE(plain.getTokenManager(), &TokenManager::instance());
    }
};

QTEST_MAIN(TestBridgeIdentity)
#include "test_bridge_identity.moc"
