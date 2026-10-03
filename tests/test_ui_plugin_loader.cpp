// UiPluginLoader's paths that end before a plugin is admitted: dependency
// loading, and plugins that cannot be found. No LogosAPI is needed for these.

#include <QtTest/QtTest>

#include <QMutex>
#include <QMutexLocker>
#include <QSignalSpy>
#include <QThread>

#include "UiPluginLoader.h"

using logos::ui::UiPluginKind;
using logos::ui::UiPluginLoader;
using logos::ui::UiPluginRequest;

namespace {

struct Attempt {
    QString name;
    bool required;
    QThread* thread;
};

struct Recorder {
    QMutex mutex;
    QList<Attempt> attempts;
    QSet<QString> failing;

    UiPluginLoader::LoadDependency bind()
    {
        return [this](const QString& name, bool required) {
            QMutexLocker lock(&mutex);
            attempts.append({name, required, QThread::currentThread()});
            return !failing.contains(name);
        };
    }
};

UiPluginRequest qmlRequest(const QString& name)
{
    UiPluginRequest request;
    request.name = name;
    request.kind = UiPluginKind::UiQml;
    request.qmlViewPath = QStringLiteral("/nonexistent/%1/Main.qml").arg(name);
    return request;
}

} // namespace

class UiPluginLoaderTest : public QObject {
    Q_OBJECT

private slots:
    void aRequiredDependencyThatFailsToLoadFailsThePlugin()
    {
        Recorder recorder;
        recorder.failing = {"dep_a"};
        UiPluginLoader loader(nullptr, recorder.bind());
        QSignalSpy failed(&loader, &UiPluginLoader::pluginLoadFailed);

        UiPluginRequest request = qmlRequest("p1");
        request.dependencies = {QStringLiteral("dep_a"), QStringLiteral("dep_b")};
        loader.load(request);
        QVERIFY(loader.isLoading("p1"));

        QTRY_COMPARE(failed.count(), 1);
        QCOMPARE(failed.at(0).at(0).toString(), QStringLiteral("p1"));
        QVERIFY(failed.at(0).at(1).toString().contains("dep_a"));
        QVERIFY(!loader.isLoading("p1"));
        QCOMPARE(recorder.attempts.size(), 1);
        QVERIFY(recorder.attempts[0].required);
        QVERIFY(recorder.attempts[0].thread != QThread::currentThread());
    }

    void anUnrecognisedDependencyEntryFailsBeforeLoadingAny()
    {
        Recorder recorder;
        UiPluginLoader loader(nullptr, recorder.bind());
        QSignalSpy failed(&loader, &UiPluginLoader::pluginLoadFailed);

        UiPluginRequest request = qmlRequest("p2");
        request.dependencies = {QVariant(42), QStringLiteral("dep_b")};
        loader.load(request);

        QTRY_COMPARE(failed.count(), 1);
        QVERIFY(failed.at(0).at(1).toString().contains("Unrecognised dependency"));
        QVERIFY(recorder.attempts.isEmpty());
    }

    void optionalDependenciesAreBestEffort()
    {
        Recorder recorder;
        recorder.failing = {"opt_a"};
        UiPluginLoader loader(nullptr, recorder.bind());
        QSignalSpy failed(&loader, &UiPluginLoader::pluginLoadFailed);

        UiPluginRequest request = qmlRequest("p3");
        request.dependencies = {QVariantMap{{"name", "dep_a"}, {"version", "^1.0.0"}}};
        request.optionalDependencies = {QStringLiteral("opt_a")};
        loader.load(request);

        // Past the dependencies: it fails on the missing view, not on opt_a.
        QTRY_COMPARE(failed.count(), 1);
        QVERIFY(failed.at(0).at(1).toString().contains("QML view file not found"));
        QCOMPARE(recorder.attempts.size(), 2);
        QCOMPARE(recorder.attempts[0].name, QStringLiteral("dep_a"));
        QVERIFY(recorder.attempts[0].required);
        QCOMPARE(recorder.attempts[1].name, QStringLiteral("opt_a"));
        QVERIFY(!recorder.attempts[1].required);
    }

    void aSecondLoadWhileLoadingIsIgnored()
    {
        Recorder recorder;
        UiPluginLoader loader(nullptr, recorder.bind());
        QSignalSpy failed(&loader, &UiPluginLoader::pluginLoadFailed);

        loader.load(qmlRequest("p4"));
        loader.load(qmlRequest("p4"));
        QCOMPARE(loader.loadingPlugins(), QStringList{"p4"});

        QTRY_COMPARE(failed.count(), 1);
        QTest::qWait(100);
        QCOMPARE(failed.count(), 1);
    }

    void aMissingLegacyPluginFails()
    {
        Recorder recorder;
        UiPluginLoader loader(nullptr, recorder.bind());
        QSignalSpy failed(&loader, &UiPluginLoader::pluginLoadFailed);
        QSignalSpy loaded(&loader, &UiPluginLoader::pluginLoaded);

        UiPluginRequest request;
        request.name = "legacy";
        request.kind = UiPluginKind::Legacy;
        request.pluginPath = "/nonexistent/legacy_plugin.so";
        loader.load(request);

        QTRY_COMPARE(failed.count(), 1);
        QCOMPARE(loaded.count(), 0);
        QVERIFY(!loader.isLoading("legacy"));
    }
};

QTEST_MAIN(UiPluginLoaderTest)
#include "test_ui_plugin_loader.moc"
