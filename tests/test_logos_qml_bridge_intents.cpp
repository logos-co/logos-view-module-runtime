// Unit tests for the app-to-app intent surface on LogosQmlBridge.
//
// Everything here runs with a null LogosAPI, no sockets, no ui-host and no
// packages. The bridge's intent half deliberately depends on nothing except a
// LogosIntentRouter, which is what makes it testable this way — and what makes
// it replaceable later.
//
// Three things these tests exist to pin, beyond ordinary coverage:
//
//   1. The callback contract: exactly once, always asynchronous, real JS
//      objects (not the JSON strings callModuleAsync deals in). An app that
//      accidentally comes to depend on the opposite of any of these is a
//      migration we cannot perform.
//
//   2. The responsibility boundary: the bridge does NOT validate ownership on
//      respond(). If someone "helpfully" adds that check, testRespondForwards
//      ForeignId fails — which is the point. One enforcement point, in the
//      router, is worth more than two that can disagree.
//
//   3. The trust boundary: the host-side API must stay out of the metaobject,
//      because QML can call anything the metaobject exposes.

#include "LogosQmlBridge.h"
#include "LogosIntent.h"
#include "LogosIntentRouter.h"

#include <QCoreApplication>
#include <QJSEngine>
#include <QMetaMethod>
#include <QSignalSpy>
#include <limits>

#include <QTest>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

using namespace logos::intent;

namespace {

// Records everything the bridge routes, and can be told to answer inline so the
// "synchronous router reply is still delivered asynchronously" case is testable.
class FakeIntentRouter : public LogosIntentRouter {
public:
    struct Routed {
        LogosQmlBridge* from = nullptr;
        QString requestId;
        QString intent;
        QVariantMap params;
    };
    struct Responded {
        LogosQmlBridge* from = nullptr;
        QString requestId;
        bool ok = false;
        QVariant data;
        QString error;
    };

    QList<Routed> routed;
    QList<Responded> responded;
    QList<QPair<LogosQmlBridge*, QStringList>> abandoned;
    QList<LogosQmlBridge*> destroyed;

    // When set, routeIntent answers on the same call stack.
    bool replyInline = false;
    QVariantMap inlineEnvelope;

    void routeIntent(LogosQmlBridge* from, const QString& requestId,
                     const QString& intent, const QVariantMap& params) override
    {
        routed.append({from, requestId, intent, params});
        if (replyInline)
            from->deliverIntentResult(requestId, inlineEnvelope);
    }

    void routeIntentResponse(LogosQmlBridge* from, const QString& requestId,
                             bool ok, const QVariant& data,
                             const QString& error) override
    {
        responded.append({from, requestId, ok, data, error});
    }

    void intentsAbandoned(LogosQmlBridge* from, const QStringList& ids) override
    {
        abandoned.append({from, ids});
    }

    void bridgeDestroyed(LogosQmlBridge* bridge) override
    {
        destroyed.append(bridge);
    }
};

// R2: the router contract must stay a plain abstract class. If it ever gains a
// QObject base it stops being implementable by a class that already has one.
static_assert(!std::is_base_of_v<QObject, LogosIntentRouter>,
              "LogosIntentRouter must not be a QObject");
static_assert(std::is_abstract_v<LogosIntentRouter>,
              "LogosIntentRouter must stay abstract");

void pump(int rounds = 8)
{
    for (int i = 0; i < rounds; ++i)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
}

} // namespace

class TestLogosQmlBridgeIntents : public QObject {
    Q_OBJECT

private slots:
    // ── R1: the frozen vocabulary ───────────────────────────────────────
    void testNameGrammarAccepts();
    void testNameGrammarRejects();
    void testReservedNames();
    void testErrorCodesClosedSet();
    void testNormalizeErrorCoercesProviderText();
    void testEnvelopeShape();
    void testCanonicalPayloadAccepts();
    void testCanonicalPayloadRejects();

    // ── R4: request / callback contract ─────────────────────────────────
    void testNullRouterDeliversUnavailableExactlyOnce();
    void testCallbackNeverFiresBeforeRequestReturns();
    void testRequestIdsAreUnique();
    void testParamsRoundTripWithNesting();
    void testSynchronousRouterReplyIsStillAsync();
    void testEnvelopeReachesQmlAsObjectNotString();
    void testSecondDeliverIsNoOp();
    void testUnknownIdDeliverIsNoOp();
    void testNonCallableCallbackStillRoutes();

    // ── R4: provider delivery + responsibility boundary ─────────────────
    void testDeliverIntentRequestReceiverCount();
    void testRespondForwardsForeignIdVerbatim();
    void testRespondFlattensEngineBoundValues();
    void testRespondWithNullRouterIsSilent();

    // ── R4: lifecycle ───────────────────────────────────────────────────
    void testPendingIdsEmptyAfterDelivery();
    void testAbandonFiresOnceAndInvokesNothing();
    void testAbandonCancelsAnAlreadyQueuedDelivery();
    void testPayloadAggregateStringBudget();
    void testPayloadRejectsNonFiniteDoubles();
    void testDestructorNotifiesRouterOnceWithoutCallbacks();

    // ── R7: the trust boundary ──────────────────────────────────────────
    void testFrozenSurfaceIsInTheMetaobject();
    void testHostApiIsNotInTheMetaobject();
};

// ─────────────────────────────────────────────────────────────────────────────
// R1
// ─────────────────────────────────────────────────────────────────────────────

void TestLogosQmlBridgeIntents::testNameGrammarAccepts()
{
    QVERIFY(isValidName(QStringLiteral("packages.show")));
    QVERIFY(isValidName(QStringLiteral("logos.repositories.manage")));
    QVERIFY(isValidName(QStringLiteral("a.b_c.d0")));
    QVERIFY(isValidName(QStringLiteral("wallet.send")));
    QVERIFY(isValidName(QStringLiteral("a.b.c.d")));            // 4 segments, the max
}

void TestLogosQmlBridgeIntents::testNameGrammarRejects()
{
    QVERIFY(!isValidName(QStringLiteral("Packages.show")));     // uppercase
    QVERIFY(!isValidName(QStringLiteral("packages.")));         // empty trailing segment
    QVERIFY(!isValidName(QStringLiteral("packages..show")));    // empty middle segment
    QVERIFY(!isValidName(QStringLiteral("packages.show-me")));  // hyphen
    QVERIFY(!isValidName(QStringLiteral("packages._show")));    // segment starts with _
    QVERIFY(!isValidName(QStringLiteral("packages.show_")));    // segment ends with _
    QVERIFY(!isValidName(QStringLiteral("packages.sh__ow")));   // double underscore
    QVERIFY(!isValidName(QStringLiteral("packages.9show")));    // segment starts with a digit
    QVERIFY(!isValidName(QStringLiteral("packages")));          // only one segment
    QVERIFY(!isValidName(QStringLiteral("a.b.c.d.e")));         // five segments
    QVERIFY(!isValidName(QStringLiteral("ab")));                // shorter than 3
    QVERIFY(!isValidName(QString(65, QLatin1Char('a')).insert(30, QLatin1Char('.'))));
    QVERIFY(!isValidName(QStringLiteral("packages.shöw")));     // non-ASCII
    QVERIFY(!isValidName(QString()));
}

void TestLogosQmlBridgeIntents::testReservedNames()
{
    QVERIFY(isReservedName(QStringLiteral("logos.repositories.manage")));
    QVERIFY(!isReservedName(QStringLiteral("packages.show")));
    // "logosx.foo" must not be caught by a sloppy prefix test.
    QVERIFY(!isReservedName(QStringLiteral("logosx.foo")));
}

void TestLogosQmlBridgeIntents::testErrorCodesClosedSet()
{
    QCOMPARE(allErrorCodes().size(), 6);
    for (const QString& code : allErrorCodes())
        QVERIFY(isErrorCode(code));
    QVERIFY(!isErrorCode(QStringLiteral("ambiguous")));   // internal only, never delivered
    QVERIFY(!isErrorCode(QString()));
    QVERIFY(!isErrorCode(QStringLiteral("Failed")));      // byte-exact

    // bad_request belongs to the set even though most codes are broker-minted:
    // it is the one code a provider and the shell can both produce, and leaving
    // it out would make isErrorCode() disagree with normalizeError().
    QVERIFY(isErrorCode(errBadRequest()));
}

void TestLogosQmlBridgeIntents::testNormalizeErrorCoercesProviderText()
{
    // A provider may report these four.
    QCOMPARE(normalizeError(false, errCancelled()),  errCancelled());
    QCOMPARE(normalizeError(false, errTimeout()),    errTimeout());
    QCOMPARE(normalizeError(false, errFailed()),     errFailed());
    // bad_request survives from a provider on purpose: a provider is the party
    // that knows its own params are unusable, and if only the shell could say
    // so, the code itself would prove no provider had been consulted.
    QCOMPARE(normalizeError(false, errBadRequest()), errBadRequest());

    // Free text must never reach the requester's error path.
    QCOMPARE(normalizeError(false, QStringLiteral("no such key in keystore")), errFailed());
    QCOMPARE(normalizeError(false, QString()), errFailed());

    // A provider must not be able to claim either broker-only code: both leak
    // whether a provider exists at all.
    QCOMPARE(normalizeError(false, errUnavailable()), errFailed());
    QCOMPARE(normalizeError(false, errNotDeclared()), errFailed());

    // Success always clears the error.
    QCOMPARE(normalizeError(true, errFailed()), QString());
}

void TestLogosQmlBridgeIntents::testEnvelopeShape()
{
    const QVariantMap ok = makeEnvelope(true, QVariant(QStringLiteral("v")), QString());
    QCOMPARE(ok.value(QStringLiteral("ok")).toBool(), true);
    QCOMPARE(ok.value(QStringLiteral("data")).toString(), QStringLiteral("v"));
    QCOMPARE(ok.value(QStringLiteral("error")).toString(), QString());
    QCOMPARE(ok.size(), 3);

    const QVariantMap bad = makeEnvelope(false, QVariant(QVariantMap{}), errTimeout());
    QCOMPARE(bad.value(QStringLiteral("ok")).toBool(), false);
    // data must be genuinely absent on failure, not an empty object: `res.data`
    // has to be falsy in QML.
    QVERIFY(!bad.value(QStringLiteral("data")).isValid());
    QCOMPARE(bad.value(QStringLiteral("error")).toString(), errTimeout());
    QCOMPARE(bad.size(), 3);
}

void TestLogosQmlBridgeIntents::testCanonicalPayloadAccepts()
{
    QVariantMap p;
    p.insert(QStringLiteral("chain_id"), 1);
    p.insert(QStringLiteral("to"), QStringLiteral("0xabc"));
    p.insert(QStringLiteral("ok"), true);
    p.insert(QStringLiteral("ratio"), 1.5);
    p.insert(QStringLiteral("tags"), QVariantList{ QStringLiteral("a"), 2 });
    p.insert(QStringLiteral("nested"), QVariantMap{ { QStringLiteral("k"), QStringLiteral("v") } });
    p.insert(QStringLiteral("absent"), QVariant());
    QVERIFY(isCanonicalPayload(p));

    QVERIFY(isCanonicalPayload(QVariantMap{}));

    // Exactly at the integer boundary a JS number can still represent.
    QVERIFY(isCanonicalPayload(QVariantMap{ { QStringLiteral("n"), 9007199254740991LL } }));
}

void TestLogosQmlBridgeIntents::testCanonicalPayloadRejects()
{
    // A QObject* would hand the provider a live pointer into the requester.
    QObject stray;
    QVERIFY(!isCanonicalPayload(QVariantMap{
        { QStringLiteral("o"), QVariant::fromValue(&stray) } }));

    // A QJSValue is engine-bound and meaningless in another engine.
    QJSEngine engine;
    QVERIFY(!isCanonicalPayload(QVariantMap{
        { QStringLiteral("j"), QVariant::fromValue(engine.newObject()) } }));

    // Beyond exact JS integer range — refused rather than silently rounded.
    QVERIFY(!isCanonicalPayload(QVariantMap{ { QStringLiteral("n"), 9007199254740993LL } }));

    // Types outside the closed set, even though QVariant is happy to hold them.
    QVERIFY(!isCanonicalPayload(QVariantMap{
        { QStringLiteral("d"), QVariant::fromValue(QDateTime::currentDateTime()) } }));

    // Nine levels of nesting: eight is the limit.
    QVariant deep = QVariant(QStringLiteral("leaf"));
    for (int i = 0; i < 9; ++i)
        deep = QVariant(QVariantMap{ { QStringLiteral("d"), deep } });
    QVERIFY(!isCanonicalPayload(deep.toMap()));

    // An over-long key.
    QVERIFY(!isCanonicalPayload(QVariantMap{
        { QString(65, QLatin1Char('k')), 1 } }));

    // Too many nodes.
    QVariantList many;
    for (int i = 0; i < 1500; ++i) many.append(i);
    QVERIFY(!isCanonicalPayload(QVariantMap{ { QStringLiteral("l"), many } }));
}

// ─────────────────────────────────────────────────────────────────────────────
// R4 — request / callback contract
// ─────────────────────────────────────────────────────────────────────────────

void TestLogosQmlBridgeIntents::testNullRouterDeliversUnavailableExactlyOnce()
{
    LogosQmlBridge bridge(nullptr);
    QJSEngine engine;
    engine.setObjectOwnership(&bridge, QJSEngine::CppOwnership);
    engine.globalObject().setProperty(QStringLiteral("logos"), engine.newQObject(&bridge));

    QJSValue res = engine.evaluate(QStringLiteral(R"JS(
        var __calls = [];
        logos.request("packages.show", {}, function (r) { __calls.push(r); });
    )JS"));
    QVERIFY2(!res.isError(), qPrintable(res.toString()));

    pump();

    QJSValue calls = engine.globalObject().property(QStringLiteral("__calls"));
    QCOMPARE(calls.property(QStringLiteral("length")).toInt(), 1);
    QCOMPARE(calls.property(0).property(QStringLiteral("ok")).toBool(), false);
    QCOMPARE(calls.property(0).property(QStringLiteral("error")).toString(), errUnavailable());

    // Nothing is left pending — a locally-failed request must not leak a record.
    QVERIFY(bridge.pendingIntentRequestIds().isEmpty());
}

void TestLogosQmlBridgeIntents::testCallbackNeverFiresBeforeRequestReturns()
{
    // Even the immediate no-router failure must be queued. This is the case an
    // app is most likely to accidentally depend on being synchronous.
    LogosQmlBridge bridge(nullptr);
    QJSEngine engine;
    engine.setObjectOwnership(&bridge, QJSEngine::CppOwnership);
    engine.globalObject().setProperty(QStringLiteral("logos"), engine.newQObject(&bridge));

    QJSValue res = engine.evaluate(QStringLiteral(R"JS(
        var __firedDuringCall = false;
        var __done = false;
        var __inCall = true;
        logos.request("packages.show", {}, function (r) {
            if (__inCall) __firedDuringCall = true;
            __done = true;
        });
        __inCall = false;
    )JS"));
    QVERIFY2(!res.isError(), qPrintable(res.toString()));

    QCOMPARE(engine.globalObject().property(QStringLiteral("__firedDuringCall")).toBool(), false);
    QCOMPARE(engine.globalObject().property(QStringLiteral("__done")).toBool(), false);

    pump();
    QCOMPARE(engine.globalObject().property(QStringLiteral("__done")).toBool(), true);
}

void TestLogosQmlBridgeIntents::testRequestIdsAreUnique()
{
    FakeIntentRouter router;
    LogosQmlBridge bridge(nullptr);
    bridge.setIntentRouter(&router);

    bridge.request(QStringLiteral("a.b"), QVariantMap{}, QJSValue());
    bridge.request(QStringLiteral("a.b"), QVariantMap{}, QJSValue());
    bridge.request(QStringLiteral("a.b"), QVariantMap{}, QJSValue());

    QCOMPARE(router.routed.size(), 3);
    QSet<QString> ids;
    for (const auto& r : router.routed) {
        QVERIFY(!r.requestId.isEmpty());
        QVERIFY(!r.requestId.contains(QLatin1Char('{')));   // braces stripped
        ids.insert(r.requestId);
        QCOMPARE(r.from, &bridge);
    }
    QCOMPARE(ids.size(), 3);
    QCOMPARE(bridge.pendingIntentRequestIds().size(), 3);
}

void TestLogosQmlBridgeIntents::testParamsRoundTripWithNesting()
{
    FakeIntentRouter router;
    LogosQmlBridge bridge(nullptr);
    bridge.setIntentRouter(&router);

    QVariantMap params;
    params.insert(QStringLiteral("chain_id"), 1);
    params.insert(QStringLiteral("nested"),
                  QVariantMap{ { QStringLiteral("deep"), QVariantList{ 1, QStringLiteral("x") } } });

    bridge.request(QStringLiteral("wallet.send"), params, QJSValue());

    QCOMPARE(router.routed.size(), 1);
    QCOMPARE(router.routed.first().intent, QStringLiteral("wallet.send"));
    QCOMPARE(router.routed.first().params, params);
}

void TestLogosQmlBridgeIntents::testSynchronousRouterReplyIsStillAsync()
{
    FakeIntentRouter router;
    router.replyInline = true;
    router.inlineEnvelope = makeEnvelope(true, QVariant(QStringLiteral("inline")), QString());

    LogosQmlBridge bridge(nullptr);
    bridge.setIntentRouter(&router);

    QJSEngine engine;
    engine.setObjectOwnership(&bridge, QJSEngine::CppOwnership);
    engine.globalObject().setProperty(QStringLiteral("logos"), engine.newQObject(&bridge));

    QJSValue res = engine.evaluate(QStringLiteral(R"JS(
        var __sync = false, __value = null, __inCall = true;
        logos.request("a.b", {}, function (r) {
            if (__inCall) __sync = true;
            __value = r.data;
        });
        __inCall = false;
    )JS"));
    QVERIFY2(!res.isError(), qPrintable(res.toString()));

    // The router answered on the same stack; the bridge must still queue it.
    QCOMPARE(engine.globalObject().property(QStringLiteral("__sync")).toBool(), false);

    pump();
    QCOMPARE(engine.globalObject().property(QStringLiteral("__value")).toString(),
             QStringLiteral("inline"));
}

void TestLogosQmlBridgeIntents::testEnvelopeReachesQmlAsObjectNotString()
{
    // Guards against anyone copying callModuleAsync's JSON-string path. If this
    // regresses, every documented `res.data.tx_hash` in the tree silently breaks.
    FakeIntentRouter router;
    LogosQmlBridge bridge(nullptr);
    bridge.setIntentRouter(&router);

    QJSEngine engine;
    engine.setObjectOwnership(&bridge, QJSEngine::CppOwnership);
    engine.globalObject().setProperty(QStringLiteral("logos"), engine.newQObject(&bridge));

    QJSValue res = engine.evaluate(QStringLiteral(R"JS(
        var __hash = null, __typeofRes = "", __typeofData = "", __hasData = false;
        logos.request("wallet.send", {}, function (r) {
            __typeofRes = typeof r;
            __typeofData = typeof r.data;
            __hasData = ("data" in r);
            __hash = r.data.tx_hash;
        });
    )JS"));
    QVERIFY2(!res.isError(), qPrintable(res.toString()));

    QCOMPARE(router.routed.size(), 1);
    bridge.deliverIntentResult(
        router.routed.first().requestId,
        makeEnvelope(true,
                     QVariant(QVariantMap{ { QStringLiteral("tx_hash"), QStringLiteral("0xdead") } }),
                     QString()));
    pump();

    QCOMPARE(engine.globalObject().property(QStringLiteral("__typeofRes")).toString(),
             QStringLiteral("object"));
    QCOMPARE(engine.globalObject().property(QStringLiteral("__typeofData")).toString(),
             QStringLiteral("object"));
    QCOMPARE(engine.globalObject().property(QStringLiteral("__hasData")).toBool(), true);
    QCOMPARE(engine.globalObject().property(QStringLiteral("__hash")).toString(),
             QStringLiteral("0xdead"));
}

void TestLogosQmlBridgeIntents::testSecondDeliverIsNoOp()
{
    FakeIntentRouter router;
    LogosQmlBridge bridge(nullptr);
    bridge.setIntentRouter(&router);

    QJSEngine engine;
    engine.setObjectOwnership(&bridge, QJSEngine::CppOwnership);
    engine.globalObject().setProperty(QStringLiteral("logos"), engine.newQObject(&bridge));
    engine.evaluate(QStringLiteral(
        "var __n = 0; logos.request('a.b', {}, function (r) { __n++; });"));

    const QString id = router.routed.first().requestId;
    bridge.deliverIntentResult(id, makeEnvelope(true, QVariant(1), QString()));
    bridge.deliverIntentResult(id, makeEnvelope(true, QVariant(2), QString()));
    pump();

    QCOMPARE(engine.globalObject().property(QStringLiteral("__n")).toInt(), 1);
}

void TestLogosQmlBridgeIntents::testUnknownIdDeliverIsNoOp()
{
    FakeIntentRouter router;
    LogosQmlBridge bridge(nullptr);
    bridge.setIntentRouter(&router);

    // Must not crash, must not invent a callback.
    bridge.deliverIntentResult(QStringLiteral("no-such-id"),
                               makeEnvelope(true, QVariant(1), QString()));
    pump();
    QVERIFY(bridge.pendingIntentRequestIds().isEmpty());
}

void TestLogosQmlBridgeIntents::testNonCallableCallbackStillRoutes()
{
    // An app that passes a non-function must still have its intent routed —
    // the request is meaningful even if the answer goes nowhere.
    FakeIntentRouter router;
    LogosQmlBridge bridge(nullptr);
    bridge.setIntentRouter(&router);

    bridge.request(QStringLiteral("a.b"), QVariantMap{}, QJSValue(42));
    QCOMPARE(router.routed.size(), 1);

    bridge.deliverIntentResult(router.routed.first().requestId,
                               makeEnvelope(true, QVariant(1), QString()));
    pump();   // must not crash
    QVERIFY(bridge.pendingIntentRequestIds().isEmpty());
}

// ─────────────────────────────────────────────────────────────────────────────
// R4 — provider delivery + the responsibility boundary
// ─────────────────────────────────────────────────────────────────────────────

void TestLogosQmlBridgeIntents::testDeliverIntentRequestReceiverCount()
{
    // The count is TOTAL receivers, so it is only a proxy for "does the view
    // have a handler" while nothing else is connected. A QSignalSpy is itself a
    // receiver — which is exactly why the zero case is measured on a bridge
    // with no spy attached, and why a host must never connect to this signal.
    {
        LogosQmlBridge bare(nullptr);
        QCOMPARE(bare.deliverIntentRequest(QStringLiteral("r1"), QStringLiteral("a.b"),
                                           QVariantMap{}, QStringLiteral("chat_ui")), 0);
    }

    {
        LogosQmlBridge bridge(nullptr);
        QString seenIntent, seenRequester, seenId;
        QVariantMap seenParams;
        QObject::connect(&bridge, &LogosQmlBridge::intentRequested, &bridge,
                         [&](const QString& id, const QString& intent,
                             const QVariantMap& params, const QString& requester) {
                             seenId = id;
                             seenIntent = intent;
                             seenParams = params;
                             seenRequester = requester;
                         });

        QCOMPARE(bridge.deliverIntentRequest(QStringLiteral("r2"), QStringLiteral("a.b"),
                                             QVariantMap{ { QStringLiteral("k"), 7 } },
                                             QStringLiteral("chat_ui")), 1);
        QCOMPARE(seenId, QStringLiteral("r2"));
        QCOMPARE(seenIntent, QStringLiteral("a.b"));
        QCOMPARE(seenParams.value(QStringLiteral("k")).toInt(), 7);
        QCOMPARE(seenRequester, QStringLiteral("chat_ui"));
    }

    {
        // Emission itself happens exactly once per delivery.
        LogosQmlBridge bridge(nullptr);
        QSignalSpy spy(&bridge, &LogosQmlBridge::intentRequested);
        bridge.deliverIntentRequest(QStringLiteral("r3"), QStringLiteral("a.b"),
                                    QVariantMap{}, QString());
        QCOMPARE(spy.count(), 1);
    }
}

void TestLogosQmlBridgeIntents::testRespondForwardsForeignIdVerbatim()
{
    // PINS THE RESPONSIBILITY BOUNDARY. The bridge deliberately does not check
    // that it owns this id, does not normalise the error, and does not inspect
    // the data. If someone adds any of that here, this test fails — and it
    // should, because two enforcement points can disagree and the weaker one
    // silently becomes the real policy.
    FakeIntentRouter router;
    LogosQmlBridge bridge(nullptr);
    bridge.setIntentRouter(&router);

    bridge.respond(QStringLiteral("an-id-this-bridge-never-requested"),
                   false, QVariant(QStringLiteral("payload")),
                   QStringLiteral("some free text"));

    QCOMPARE(router.responded.size(), 1);
    const auto& r = router.responded.first();
    QCOMPARE(r.from, &bridge);
    QCOMPARE(r.requestId, QStringLiteral("an-id-this-bridge-never-requested"));
    QCOMPARE(r.ok, false);
    QCOMPARE(r.data.toString(), QStringLiteral("payload"));
    QCOMPARE(r.error, QStringLiteral("some free text"));   // NOT normalised here
}

void TestLogosQmlBridgeIntents::testRespondWithNullRouterIsSilent()
{
    LogosQmlBridge bridge(nullptr);
    bridge.respond(QStringLiteral("x"), true, QVariant(), QString());   // must not crash
    QVERIFY(true);
}

// ─────────────────────────────────────────────────────────────────────────────
// R4 — lifecycle
// ─────────────────────────────────────────────────────────────────────────────

void TestLogosQmlBridgeIntents::testPendingIdsEmptyAfterDelivery()
{
    FakeIntentRouter router;
    LogosQmlBridge bridge(nullptr);
    bridge.setIntentRouter(&router);

    bridge.request(QStringLiteral("a.b"), QVariantMap{}, QJSValue());
    QCOMPARE(bridge.pendingIntentRequestIds().size(), 1);

    bridge.deliverIntentResult(router.routed.first().requestId,
                               makeEnvelope(true, QVariant(1), QString()));
    QVERIFY(bridge.pendingIntentRequestIds().isEmpty());   // erased before invoking
}

void TestLogosQmlBridgeIntents::testAbandonFiresOnceAndInvokesNothing()
{
    FakeIntentRouter router;
    LogosQmlBridge bridge(nullptr);
    bridge.setIntentRouter(&router);

    QJSEngine engine;
    engine.setObjectOwnership(&bridge, QJSEngine::CppOwnership);
    engine.globalObject().setProperty(QStringLiteral("logos"), engine.newQObject(&bridge));
    engine.evaluate(QStringLiteral(
        "var __n = 0;"
        "logos.request('a.b', {}, function () { __n++; });"
        "logos.request('a.c', {}, function () { __n++; });"));

    QCOMPARE(bridge.pendingIntentRequestIds().size(), 2);

    bridge.abandonPendingIntents();
    pump();

    QCOMPARE(router.abandoned.size(), 1);
    QCOMPARE(router.abandoned.first().first, &bridge);
    QCOMPARE(router.abandoned.first().second.size(), 2);
    QVERIFY(bridge.pendingIntentRequestIds().isEmpty());
    // Abandon means "dropped", not "failed": no callback runs.
    QCOMPARE(engine.globalObject().property(QStringLiteral("__n")).toInt(), 0);

    // A second call is a no-op, not a second notification.
    bridge.abandonPendingIntents();
    QCOMPARE(router.abandoned.size(), 1);
}

void TestLogosQmlBridgeIntents::testDestructorNotifiesRouterOnceWithoutCallbacks()
{
    FakeIntentRouter router;
    auto* bridge = new LogosQmlBridge(nullptr);
    bridge->setIntentRouter(&router);

    QJSEngine engine;
    engine.setObjectOwnership(bridge, QJSEngine::CppOwnership);
    engine.globalObject().setProperty(QStringLiteral("logos"), engine.newQObject(bridge));
    engine.evaluate(QStringLiteral(
        "var __n = 0; logos.request('a.b', {}, function () { __n++; });"));
    QCOMPARE(bridge->pendingIntentRequestIds().size(), 1);

    LogosQmlBridge* raw = bridge;
    delete bridge;
    pump();

    QCOMPARE(router.destroyed.size(), 1);
    QCOMPARE(router.destroyed.first(), raw);
    // A destructor must never enter JS.
    QCOMPARE(engine.globalObject().property(QStringLiteral("__n")).toInt(), 0);
}

// ─────────────────────────────────────────────────────────────────────────────
// R7 — the trust boundary
// ─────────────────────────────────────────────────────────────────────────────

void TestLogosQmlBridgeIntents::testFrozenSurfaceIsInTheMetaobject()
{
    const QMetaObject* mo = &LogosQmlBridge::staticMetaObject;

    QVERIFY2(mo->indexOfMethod("request(QString,QVariantMap,QJSValue)") >= 0,
             "the frozen request() signature changed");
    QVERIFY2(mo->indexOfMethod("respond(QString,bool,QVariant,QString)") >= 0,
             "the frozen respond() signature changed");
    QVERIFY2(mo->indexOfSignal("intentRequested(QString,QString,QVariantMap,QString)") >= 0,
             "the frozen intentRequested() signature changed");
}

void TestLogosQmlBridgeIntents::testHostApiIsNotInTheMetaobject()
{
    // QML can call ANYTHING in the metaobject. So the host-side API's absence
    // from it is the trust boundary: an app must not be able to install its own
    // router, deliver a result to itself, or read another request's id.
    const QMetaObject* mo = &LogosQmlBridge::staticMetaObject;
    const QByteArrayList forbidden = {
        "setIntentRouter", "deliverIntentRequest", "deliverIntentResult",
        "abandonPendingIntents", "pendingIntentRequestIds"
    };

    for (int i = 0; i < mo->methodCount(); ++i) {
        const QByteArray name = mo->method(i).name();
        QVERIFY2(!forbidden.contains(name),
                 qPrintable(QStringLiteral("host-side API '%1' is reachable from QML")
                                .arg(QString::fromUtf8(name))));
    }
}

// A QJSValue is engine-bound: handed to another app's engine it reads as null.
// respond()'s `data` is an untyped QVariant, so QML passes the wrapper straight
// through — this pins that it is flattened to plain containers before routing,
// which is what stops a provider's return payload arriving empty.
void TestLogosQmlBridgeIntents::testRespondFlattensEngineBoundValues()
{
    QJSEngine engine;
    LogosQmlBridge bridge(nullptr, nullptr);
    engine.newQObject(&bridge);

    FakeIntentRouter router;
    bridge.setIntentRouter(&router);

    QJSValue obj = engine.newObject();
    obj.setProperty(QStringLiteral("provider"), QStringLiteral("wallet_b"));

    bridge.respond(QStringLiteral("dispatch-1"), true,
                   QVariant::fromValue(obj), QString());

    QCOMPARE(router.responded.size(), 1);
    const QVariant routed = router.responded.first().data;

    // Plain containers on the far side, not an engine handle.
    QVERIFY(routed.userType() != qMetaTypeId<QJSValue>());
    QCOMPARE(routed.toMap().value(QStringLiteral("provider")).toString(),
             QStringLiteral("wallet_b"));
}

void TestLogosQmlBridgeIntents::testAbandonCancelsAnAlreadyQueuedDelivery()
{
    // THE WINDOW: deliverIntentResult() erases the request from the pending map
    // and only then queues the JS callback. Between those two points the map is
    // empty, so abandonPendingIntents() has nothing left to find — clearing it
    // cancels nothing, and the callback would run against torn-down JS on the
    // next event-loop turn.
    //
    // The existing abandonment test abandons BEFORE any result arrives, so it
    // never enters this window.
    FakeIntentRouter router;
    LogosQmlBridge bridge(nullptr);
    bridge.setIntentRouter(&router);

    QJSEngine engine;
    engine.setObjectOwnership(&bridge, QJSEngine::CppOwnership);
    engine.globalObject().setProperty(QStringLiteral("logos"), engine.newQObject(&bridge));
    engine.evaluate(QStringLiteral(
        "var __n = 0;"
        "logos.request('a.b', {}, function () { __n++; });"));

    const QStringList ids = bridge.pendingIntentRequestIds();
    QCOMPARE(ids.size(), 1);

    QVariantMap envelope;
    envelope.insert(QStringLiteral("ok"), true);
    envelope.insert(QStringLiteral("data"), QVariant());
    envelope.insert(QStringLiteral("error"), QString());

    bridge.deliverIntentResult(ids.first(), envelope);   // erases, then QUEUES
    QVERIFY(bridge.pendingIntentRequestIds().isEmpty()); // …map already empty

    bridge.abandonPendingIntents();                      // must still cancel it
    pump();

    QCOMPARE(engine.globalObject().property(QStringLiteral("__n")).toInt(), 0);
}

void TestLogosQmlBridgeIntents::testPayloadAggregateStringBudget()
{
    // Node count alone is not a size bound. Under the per-string limit and the
    // 1000-node limit, a flat payload could still carry ~125 MiB of characters —
    // which is the opposite of what the bounds are documented to prevent.
    // 80 strings at the per-string maximum: ~5.2M chars, over the 4 MiB cap.
    QVariantMap fat;
    for (int i = 0; i < 80; ++i) {
        fat.insert(QStringLiteral("k%1").arg(i), QString(65536, QLatin1Char('x')));
    }
    QVERIFY(!isCanonicalPayload(fat));

    // Every individual rule is satisfied — each string is exactly at the
    // per-string limit, and 80 nodes is far under the 1000-node limit. Only the
    // aggregate refuses it, which is the whole point: node count is not a size
    // bound, and without this the same shape scales to ~125 MiB.
    QCOMPARE(fat.size(), 80);
    for (auto it = fat.cbegin(); it != fat.cend(); ++it)
        QVERIFY(it.value().toString().size() <= 65536);

    // A realistic payload passes with room to spare — the cap is deliberately
    // far above any plausible caller, so it cannot bite legitimate use.
    QVariantMap lean;
    lean.insert(QStringLiteral("body"), QString(1000, QLatin1Char('x')));
    QVERIFY(isCanonicalPayload(lean));

    // And so does something well beyond anything shipping today: 1 MiB.
    QVariantMap chunky;
    for (int i = 0; i < 16; ++i)
        chunky.insert(QStringLiteral("k%1").arg(i), QString(65536, QLatin1Char('x')));
    QVERIFY(isCanonicalPayload(chunky));
}

void TestLogosQmlBridgeIntents::testPayloadRejectsNonFiniteDoubles()
{
    // Legal doubles, illegal JSON: Qt maps both to null, so a JSON-shaped
    // transport would silently change the value instead of refusing it.
    // "Canonical" has to mean the value survives the crossing unchanged.
    const double inf = std::numeric_limits<double>::infinity();
    const double nan = std::numeric_limits<double>::quiet_NaN();

    QVERIFY(!isCanonicalPayload({{QStringLiteral("v"), inf}}));
    QVERIFY(!isCanonicalPayload({{QStringLiteral("v"), -inf}}));
    QVERIFY(!isCanonicalPayload({{QStringLiteral("v"), nan}}));

    // Ordinary doubles are unaffected, including the extremes.
    QVERIFY(isCanonicalPayload({{QStringLiteral("v"), 12.5}}));
    QVERIFY(isCanonicalPayload({{QStringLiteral("v"), 0.0}}));
    QVERIFY(isCanonicalPayload({{QStringLiteral("v"),
                                 std::numeric_limits<double>::max()}}));
}

QTEST_MAIN(TestLogosQmlBridgeIntents)
#include "test_logos_qml_bridge_intents.moc"
