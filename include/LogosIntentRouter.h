#pragma once

#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantMap>

class LogosQmlBridge;

// The seam between the frozen QML surface and whatever routes intents.
//
// LogosQmlBridge does NOTHING with an intent except hand it here: no `uses`
// check, no name grammar, no payload check, no provider lookup. All of that is
// policy, and the router implementing it (basecamp's IntentBroker) is expected
// to be deleted once the core runtime takes over provider selection. Policy in
// the bridge would outlive it, and removing it would mean changing a frozen
// surface.
//
// Not a QObject and not Q_DECLARE_INTERFACE'd — this is never loaded through
// QPluginLoader, so an implementation needs no moc.
//
// No registerApp(): the runtime hands out bridge POINTERS as identity and never
// learns app names. The first argument of every method IS the requester,
// established by construction, so there is nothing for an app to forge.
class LogosIntentRouter {
public:
    virtual ~LogosIntentRouter() = default;

    // May reply synchronously: the bridge records the callback before routing,
    // precisely so that works.
    virtual void routeIntent(LogosQmlBridge* from,
                             const QString& requestId,
                             const QString& intent,
                             const QVariantMap& params) = 0;

    // Forwarded verbatim: the bridge does not check that `from` owns
    // `requestId`. Ownership is the router's single enforcement point — better
    // than two that can disagree.
    virtual void routeIntentResponse(LogosQmlBridge* from,
                                     const QString& requestId,
                                     bool ok,
                                     const QVariant& data,
                                     const QString& error) = 0;

    // A hot reload rebound the QML engine, so these callbacks are going away
    // undelivered. The router should fail them.
    virtual void intentsAbandoned(LogosQmlBridge* from,
                                  const QStringList& requestIds) = 0;

    // Called exactly once, from the destructor. Drop every reference to this
    // pointer before returning — it dangles afterwards.
    virtual void bridgeDestroyed(LogosQmlBridge* bridge) = 0;
};
