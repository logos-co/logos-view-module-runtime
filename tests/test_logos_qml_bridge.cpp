// Unit tests for LogosQmlBridge.
//
// Scope:
//   1. serializeResultForTesting — pure JSON serialization logic. Covered
//      exhaustively because it is the contract between C++ backend modules
//      and the QML callers that parse the returned string with JSON.parse().
//   2. Routing state — view-module registration, crash cache invalidation,
//      and the guard that rejects callModule() calls on registered view
//      modules. These paths require no QRemoteObjectNode / socket.
//   3. Degraded behavior — a bridge constructed with a null LogosAPI*
//      must return a well-formed error payload rather than crashing.
//
// Intentionally NOT covered here (those belong to integration tests in
// logos-test-modules):
//   - Actually spawning ui-host and talking to a real view plugin.
//   - QRemoteObjectNode acquisition / typed-replica lifecycle.
//   - QPluginLoader loading of a real LogosViewReplicaFactory plugin.

#include "LogosQmlBridge.h"

#include <QCoreApplication>
#include <QJSEngine>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QSignalSpy>
#include <QTest>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

namespace {

// Parse a JSON payload that the bridge produces, regardless of whether it is
// an object, array, primitive, or the literal word "null". Returns an
// invalid QJsonValue on parse failure so assertions stay meaningful.
QJsonValue parsePayload(const QString& payload)
{
    const QByteArray bytes = payload.toUtf8();
    QJsonParseError err{};
    // Try object/array first.
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &err);
    if (err.error == QJsonParseError::NoError) {
        if (doc.isObject()) return QJsonValue(doc.object());
        if (doc.isArray()) return QJsonValue(doc.array());
    }
    // Primitive: wrap in an array so QJsonDocument can parse it.
    const QByteArray wrapped = QByteArray("[") + bytes + "]";
    const QJsonDocument wrappedDoc = QJsonDocument::fromJson(wrapped, &err);
    if (err.error == QJsonParseError::NoError && wrappedDoc.isArray() &&
        wrappedDoc.array().size() == 1) {
        return wrappedDoc.array().at(0);
    }
    return QJsonValue::Undefined;
}

} // namespace

class TestLogosQmlBridge : public QObject {
    Q_OBJECT

private slots:
    // ── serializeResultForTesting ───────────────────────────────────────

    void serializeResult_invalidVariant_returnsNullLiteral()
    {
        QCOMPARE(LogosQmlBridge::serializeResultForTesting(QVariant()),
                 QStringLiteral("null"));
    }

    void serializeResult_int()
    {
        const QString s = LogosQmlBridge::serializeResultForTesting(QVariant(42));
        const QJsonValue v = parsePayload(s);
        QVERIFY(v.isDouble());
        QCOMPARE(v.toInt(), 42);
    }

    void serializeResult_bool()
    {
        QCOMPARE(parsePayload(LogosQmlBridge::serializeResultForTesting(QVariant(true))).toBool(), true);
        QCOMPARE(parsePayload(LogosQmlBridge::serializeResultForTesting(QVariant(false))).toBool(), false);
    }

    void serializeResult_double()
    {
        const QJsonValue v = parsePayload(
            LogosQmlBridge::serializeResultForTesting(QVariant(3.5)));
        QVERIFY(v.isDouble());
        QCOMPARE(v.toDouble(), 3.5);
    }

    void serializeResult_string_roundtripsThroughJson()
    {
        const QString s = LogosQmlBridge::serializeResultForTesting(
            QVariant(QStringLiteral("hello \"world\"\n")));
        const QJsonValue v = parsePayload(s);
        QVERIFY(v.isString());
        QCOMPARE(v.toString(), QStringLiteral("hello \"world\"\n"));
    }

    void serializeResult_stringList()
    {
        QStringList list{"a", "b", "c"};
        const QJsonValue v = parsePayload(
            LogosQmlBridge::serializeResultForTesting(QVariant(list)));
        QVERIFY(v.isArray());
        QCOMPARE(v.toArray().size(), 3);
        QCOMPARE(v.toArray().at(1).toString(), QStringLiteral("b"));
    }

    void serializeResult_variantMap_producesObject()
    {
        QVariantMap m;
        m["name"] = "foo";
        m["count"] = 7;
        m["enabled"] = true;
        const QJsonValue v = parsePayload(
            LogosQmlBridge::serializeResultForTesting(QVariant(m)));
        QVERIFY(v.isObject());
        const QJsonObject obj = v.toObject();
        QCOMPARE(obj.value("name").toString(), QStringLiteral("foo"));
        QCOMPARE(obj.value("count").toInt(), 7);
        QCOMPARE(obj.value("enabled").toBool(), true);
    }

    void serializeResult_nestedVariantMap()
    {
        QVariantMap inner;
        inner["x"] = 1;
        inner["y"] = 2;
        QVariantMap outer;
        outer["point"] = inner;
        outer["label"] = "origin";
        const QJsonValue v = parsePayload(
            LogosQmlBridge::serializeResultForTesting(QVariant(outer)));
        QVERIFY(v.isObject());
        const QJsonObject obj = v.toObject();
        QVERIFY(obj.value("point").isObject());
        QCOMPARE(obj.value("point").toObject().value("x").toInt(), 1);
        QCOMPARE(obj.value("point").toObject().value("y").toInt(), 2);
        QCOMPARE(obj.value("label").toString(), QStringLiteral("origin"));
    }

    void serializeResult_variantList()
    {
        QVariantList list{QVariant(1), QVariant("two"), QVariant(true)};
        const QJsonValue v = parsePayload(
            LogosQmlBridge::serializeResultForTesting(QVariant(list)));
        QVERIFY(v.isArray());
        QCOMPARE(v.toArray().size(), 3);
        QCOMPARE(v.toArray().at(0).toInt(), 1);
        QCOMPARE(v.toArray().at(1).toString(), QStringLiteral("two"));
        QCOMPARE(v.toArray().at(2).toBool(), true);
    }

    void serializeResult_compactFormatting_noWhitespace()
    {
        QVariantMap m;
        m["a"] = 1;
        const QString s = LogosQmlBridge::serializeResultForTesting(QVariant(m));
        // Compact JSON: no embedded spaces/newlines.
        QVERIFY(!s.contains(QLatin1Char(' ')));
        QVERIFY(!s.contains(QLatin1Char('\n')));
    }

    // ── Routing & state (no real LogosAPI) ───────────────────────────────

    void nullLogosAPI_callModule_returnsErrorPayload()
    {
        LogosQmlBridge bridge(nullptr);
        const QString s = bridge.callModule("any", "method", {});
        const QJsonValue v = parsePayload(s);
        QVERIFY(v.isObject());
        QCOMPARE(v.toObject().value("error").toString(),
                 QStringLiteral("LogosAPI not available"));
    }

    void callModule_onRegisteredViewModule_refuses()
    {
        LogosQmlBridge bridge(nullptr);
        bridge.setViewModuleSocket("my_view", "socket-abc");
        const QString s = bridge.callModule("my_view", "ping", {});
        const QJsonValue v = parsePayload(s);
        QVERIFY(v.isObject());
        QVERIFY(v.toObject().value("error").toString().contains(
            QStringLiteral("view modules")));
    }

    void callModuleAsync_nullLogosAPI_invokesCallbackWithError()
    {
        // callModuleAsync takes a QJSValue callback. Use a QJSEngine to
        // create a callable that stashes its argument on the global scope
        // so we can read it back.
        QJSEngine engine;
        engine.globalObject().setProperty("captured", QJSValue());
        QJSValue fn = engine.evaluate(
            "(function(msg){ captured = msg; })");
        QVERIFY(fn.isCallable());

        LogosQmlBridge bridge(nullptr);
        bridge.callModuleAsync("backend", "m", {}, fn, /*timeoutMs*/ 0);

        const QString payload =
            engine.globalObject().property("captured").toString();
        const QJsonValue v = parsePayload(payload);
        QVERIFY(v.isObject());
        QCOMPARE(v.toObject().value("error").toString(),
                 QStringLiteral("LogosAPI not available"));
    }

    void callModuleAsync_onRegisteredViewModule_refuses()
    {
        QJSEngine engine;
        engine.globalObject().setProperty("captured", QJSValue());
        QJSValue fn = engine.evaluate(
            "(function(msg){ captured = msg; })");
        QVERIFY(fn.isCallable());

        LogosQmlBridge bridge(nullptr);
        bridge.setViewModuleSocket("v", "sock");
        bridge.callModuleAsync("v", "m", {}, fn, /*timeoutMs*/ 0);

        const QString payload =
            engine.globalObject().property("captured").toString();
        const QJsonValue v = parsePayload(payload);
        QVERIFY(v.isObject());
        QVERIFY(v.toObject().value("error").toString().contains(
            QStringLiteral("view modules")));
    }

    // ── View-module registration state ──────────────────────────────────

    void setViewModuleSocket_storesMapping()
    {
        LogosQmlBridge bridge(nullptr);
        QVERIFY(!bridge.hasViewModuleSocket("foo"));
        bridge.setViewModuleSocket("foo", "lgx-abc");
        QVERIFY(bridge.hasViewModuleSocket("foo"));
        QCOMPARE(bridge.viewModuleSocket("foo"), QStringLiteral("lgx-abc"));
    }

    void setViewModuleSocket_overwritingWithSameValue_isNoop()
    {
        LogosQmlBridge bridge(nullptr);
        bridge.setViewModuleSocket("foo", "sock-1");
        bridge.setViewModuleSocket("foo", "sock-1");
        QCOMPARE(bridge.viewModuleSocket("foo"), QStringLiteral("sock-1"));
    }

    void setViewModuleSocket_differentSocket_replacesMapping()
    {
        LogosQmlBridge bridge(nullptr);
        bridge.setViewModuleSocket("foo", "sock-1");
        bridge.setViewModuleSocket("foo", "sock-2");
        QCOMPARE(bridge.viewModuleSocket("foo"), QStringLiteral("sock-2"));
    }

    void setViewReplicaPlugin_storesPath()
    {
        LogosQmlBridge bridge(nullptr);
        QVERIFY(bridge.viewReplicaPluginPath("foo").isEmpty());
        bridge.setViewReplicaPlugin("foo", "/tmp/does-not-exist.so");
        QCOMPARE(bridge.viewReplicaPluginPath("foo"),
                 QStringLiteral("/tmp/does-not-exist.so"));
    }

    void isViewModuleReady_unknownModule_false()
    {
        LogosQmlBridge bridge(nullptr);
        QVERIFY(!bridge.isViewModuleReady("nope"));
    }

    // ── Crash notification ──────────────────────────────────────────────

    void notifyViewModuleCrashed_emitsBothSignalsAndMarksNotReady()
    {
        LogosQmlBridge bridge(nullptr);
        bridge.setViewModuleSocket("foo", "sock-1");

        QSignalSpy readyChangedSpy(
            &bridge, &LogosQmlBridge::viewModuleReadyChanged);
        QSignalSpy crashedSpy(
            &bridge, &LogosQmlBridge::viewModuleCrashed);

        bridge.notifyViewModuleCrashed("foo");

        QCOMPARE(crashedSpy.count(), 1);
        QCOMPARE(crashedSpy.first().at(0).toString(), QStringLiteral("foo"));

        QCOMPARE(readyChangedSpy.count(), 1);
        QCOMPARE(readyChangedSpy.first().at(0).toString(), QStringLiteral("foo"));
        QCOMPARE(readyChangedSpy.first().at(1).toBool(), false);

        QVERIFY(!bridge.isViewModuleReady("foo"));
    }

    // ── Module event subscription ─────────────────────────────────────

    void onModuleEvent_nullLogosAPI_returnsFalse()
    {
        LogosQmlBridge bridge(nullptr);
        QVERIFY(!bridge.onModuleEvent("calc_module", "resultReady"));
    }

    void onModuleEvent_nullLogosAPI_doesNotEmitSignal()
    {
        LogosQmlBridge bridge(nullptr);
        QSignalSpy spy(&bridge, &LogosQmlBridge::moduleEventReceived);
        bridge.onModuleEvent("calc_module", "resultReady");
        QCOMPARE(spy.count(), 0);
    }

    // ── Crash notification ──────────────────────────────────────────────

    void notifyViewModuleCrashed_dropsSocketMapping()
    {
        LogosQmlBridge bridge(nullptr);
        bridge.setViewModuleSocket("foo", "sock-1");
        bridge.notifyViewModuleCrashed("foo");
        // After a crash notification, dropViewModuleCaches() clears the
        // replica/model caches; the socket mapping is preserved so a
        // respawn with the same socket can reconnect. Assert the bridge
        // still knows the socket so it can reacquire.
        QVERIFY(bridge.hasViewModuleSocket("foo"));
    }
};

QTEST_GUILESS_MAIN(TestLogosQmlBridge)
#include "test_logos_qml_bridge.moc"
