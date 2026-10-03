// resolveUiPluginPath: what a host loads for a UI plugin on disk.

#include <QtTest/QtTest>

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include "UiPluginPackage.h"

using logos::ui::UiPluginKind;
using logos::ui::UiPluginRequest;
using logos::ui::resolveUiPluginPath;

namespace {

void writeFile(const QString& path, const QByteArray& content = {})
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly));
    file.write(content);
}

void writeJson(const QString& path, const QJsonObject& object)
{
    writeFile(path, QJsonDocument(object).toJson());
}

QString platformLibrary(const QString& logicalName)
{
#if defined(Q_OS_WIN)
    return logicalName + ".dll";
#elif defined(Q_OS_MAC)
    return logicalName + ".dylib";
#else
    return "lib" + logicalName + ".so";
#endif
}

} // namespace

class UiPluginPackageTest : public QObject {
    Q_OBJECT

private slots:
    void cleanup()
    {
        qunsetenv("DEV_QML_PATH");
        qunsetenv("LOGOS_QML_HOT_RELOAD");
    }

    void aPluginLibraryIsALegacyPlugin()
    {
        QTemporaryDir dir;
        const QString lib = dir.filePath("chat_ui.so");
        writeFile(lib);
        UiPluginRequest request;
        QString error;
        QVERIFY2(resolveUiPluginPath(lib, &request, &error), qPrintable(error));
        QCOMPARE(request.kind, UiPluginKind::Legacy);
        QCOMPARE(request.name, QStringLiteral("chat_ui"));
        QCOMPARE(request.pluginPath, QFileInfo(lib).absoluteFilePath());
    }

    void aUiQmlPackageResolvesItsViewBackendAndDependencies()
    {
        QTemporaryDir dir;
        writeFile(dir.filePath("qml/Main.qml"));
        writeFile(dir.filePath(platformLibrary("notes_ui_plugin")));
        writeFile(dir.filePath("notes_ui_replica_factory.so"));
        writeJson(dir.filePath("metadata.json"), {
            {"name", "notes_ui"}, {"type", "ui_qml"}, {"view", "qml/Main.qml"},
            {"main", "notes_ui_plugin"},
            {"dependencies", QJsonArray{"notes_module",
                QJsonObject{{"name", "storage_module"}, {"version", "^1.0.0"}}}},
            {"optional_dependencies", QJsonArray{"sync_module"}},
        });
        UiPluginRequest request;
        QString error;
        QVERIFY2(resolveUiPluginPath(dir.path(), &request, &error), qPrintable(error));
        QCOMPARE(request.kind, UiPluginKind::UiQml);
        QCOMPARE(request.name, QStringLiteral("notes_ui"));
        QCOMPARE(request.installDir, QFileInfo(dir.path()).absoluteFilePath());
        QCOMPARE(request.qmlViewPath, request.installDir + "/qml/Main.qml");
        QCOMPARE(request.mainFilePath, request.installDir + "/" + platformLibrary("notes_ui_plugin"));
        QCOMPARE(request.dependencies.size(), 2);
        QCOMPARE(request.dependencies[0].toString(), QStringLiteral("notes_module"));
        QCOMPARE(request.dependencies[1].toMap().value("name").toString(),
                 QStringLiteral("storage_module"));
        QCOMPARE(request.optionalDependencies, QVariantList{QStringLiteral("sync_module")});
        QVERIFY(!request.liveReload);
    }

    void aQmlOnlyPackageHasNoBackend()
    {
        QTemporaryDir dir;
        writeFile(dir.filePath("Main.qml"));
        writeJson(dir.filePath("metadata.json"),
                  {{"name", "clock_ui"}, {"type", "ui_qml"}, {"view", "Main.qml"}});
        UiPluginRequest request;
        QString error;
        QVERIFY2(resolveUiPluginPath(dir.path(), &request, &error), qPrintable(error));
        QVERIFY(request.mainFilePath.isEmpty());
    }

    void theManifestPicksTheInstalledVariant()
    {
        QTemporaryDir dir;
        writeFile(dir.filePath("Main.qml"));
        writeFile(dir.filePath("a.so"));
        writeFile(dir.filePath("b.so"));
        writeFile(dir.filePath("variant"), "variant-b\n");
        writeJson(dir.filePath("manifest.json"), {
            {"name", "pick_ui"}, {"type", "ui_qml"}, {"view", "Main.qml"},
            {"main", QJsonObject{{"variant-a", "a.so"}, {"variant-b", "b.so"}}},
        });
        UiPluginRequest request;
        QString error;
        QVERIFY2(resolveUiPluginPath(dir.path(), &request, &error), qPrintable(error));
        QCOMPARE(QFileInfo(request.mainFilePath).fileName(), QStringLiteral("b.so"));
    }

    void unloadablePackagesAreRefused_data()
    {
        QTest::addColumn<QByteArray>("metadata");
        QTest::addColumn<QString>("expected");
        QTest::newRow("no view") << QByteArray(R"({"name":"x","type":"ui_qml"})")
                                 << QStringLiteral("no 'view'");
        QTest::newRow("unknown type") << QByteArray(R"({"name":"x","type":"core"})")
                                      << QStringLiteral("Unknown UI plugin type");
        QTest::newRow("legacy without main") << QByteArray(R"({"name":"x","type":"ui"})")
                                             << QStringLiteral("no resolvable 'main'");
    }

    void unloadablePackagesAreRefused()
    {
        QFETCH(QByteArray, metadata);
        QFETCH(QString, expected);
        QTemporaryDir dir;
        writeFile(dir.filePath("metadata.json"), metadata);
        UiPluginRequest request;
        QString error;
        QVERIFY(!resolveUiPluginPath(dir.path(), &request, &error));
        QVERIFY2(error.contains(expected), qPrintable(error));
    }

    void aDirectoryWithoutMetadataOrAMissingPathIsRefused()
    {
        QTemporaryDir dir;
        UiPluginRequest request;
        QString error;
        QVERIFY(!resolveUiPluginPath(dir.path(), &request, &error));
        QVERIFY(!resolveUiPluginPath(dir.filePath("absent"), &request, &error));
    }

    void devQmlPathReplacesTheViewAndReloadsLive()
    {
        QTemporaryDir installed;
        QTemporaryDir dev;
        writeFile(installed.filePath("qml/Main.qml"));
        writeFile(dev.filePath("Main.qml"));
        writeJson(installed.filePath("metadata.json"),
                  {{"name", "dev_ui"}, {"type", "ui_qml"}, {"view", "qml/Main.qml"}});
        qputenv("DEV_QML_PATH", dev.path().toUtf8());

        UiPluginRequest request;
        QString error;
        QVERIFY2(resolveUiPluginPath(installed.path(), &request, &error), qPrintable(error));
        QCOMPARE(QFileInfo(request.qmlViewPath).canonicalFilePath(),
                 QFileInfo(dev.filePath("Main.qml")).canonicalFilePath());
        QCOMPARE(QDir(request.installDir).canonicalPath(), QDir(dev.path()).canonicalPath());
        QVERIFY(request.liveReload);

        qputenv("LOGOS_QML_HOT_RELOAD", "0");
        QVERIFY(resolveUiPluginPath(installed.path(), &request, &error));
        QVERIFY(!request.liveReload);
    }

    void devQmlPathWithoutTheEntryKeepsTheInstalledView()
    {
        QTemporaryDir installed;
        QTemporaryDir dev;
        writeFile(installed.filePath("qml/Main.qml"));
        writeJson(installed.filePath("metadata.json"),
                  {{"name", "dev_ui"}, {"type", "ui_qml"}, {"view", "qml/Main.qml"}});
        qputenv("DEV_QML_PATH", dev.path().toUtf8());

        UiPluginRequest request;
        QString error;
        QVERIFY(resolveUiPluginPath(installed.path(), &request, &error));
        QCOMPARE(request.qmlViewPath, request.installDir + "/qml/Main.qml");
        QVERIFY(!request.liveReload);
    }
};

QTEST_GUILESS_MAIN(UiPluginPackageTest)
#include "test_ui_plugin_package.moc"
