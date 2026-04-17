// Unit tests for ViewModuleHost::buildSocketName.

#include "ViewModuleHost.h"

#include <QTest>
#include <QDir>

class ViewModuleHostTest : public QObject {
    Q_OBJECT

private:
    // Helper: verify the socket name fits within the 104-byte limit for the
    // given tempPath, accounting for path separator, ".lock" sibling, and NUL.
    void verifyFits(const QString& name, const QString& tempPath)
    {
        const int fullPathBytes =
            tempPath.toUtf8().size() + 1 + name.toUtf8().size();
        QVERIFY2(fullPathBytes + 5 + 1 <= 104,
                 qPrintable(QStringLiteral("socket path %1/%2 = %3 bytes (limit 104)")
                                .arg(tempPath, name)
                                .arg(fullPathBytes + 6)));
    }

private slots:
    // Short module names should pass through unchanged.
    void shortNameIsVerbatim()
    {
        const QString name = ViewModuleHost::buildSocketName(
            "chat_ui", "abcd1234", "/tmp");
        QCOMPARE(name, QStringLiteral("logos_view_chat_ui_abcd1234"));
    }

    // Long module names must be truncated+hashed so the full filesystem
    // path stays within the 104-byte sockaddr_un limit.
    void longNameIsTruncated()
    {
        const QString tempPath = "/var/folders/XX/XXXXXXXXXXXXXXXXXX/T";
        const QString name = ViewModuleHost::buildSocketName(
            "liblogos_execution_zone_wallet_module_with_extra_suffix",
            "abcd1234",
            tempPath);

        verifyFits(name, tempPath);
    }

    // Golden test: pin the exact derived name so refactors don't silently
    // change the socket naming scheme.
    void goldenLongName()
    {
        // Use a realistic macOS tempPath that forces truncation.
        const QString tempPath = "/var/folders/Ab/AbCdEfGhIjKlMnOpQr/T";
        const QString name = ViewModuleHost::buildSocketName(
            "liblogos_execution_zone_wallet_module_with_extra_suffix",
            "abcd1234",
            tempPath);
        QCOMPARE(name,
                 QStringLiteral("logos_view_liblogos_execution_zone__dc0d0a2280b70b17_abcd1234"));
        verifyFits(name, tempPath);
    }

    // Same inputs always produce the same socket name (deterministic).
    void deterministic()
    {
        const QString a = ViewModuleHost::buildSocketName(
            "long_module_name_that_exceeds_limits_easily",
            "abcd1234", "/tmp");
        const QString b = ViewModuleHost::buildSocketName(
            "long_module_name_that_exceeds_limits_easily",
            "abcd1234", "/tmp");
        QCOMPARE(a, b);
    }

    // Different module names produce different socket names, even when both
    // are truncated to the same prefix length.
    void differentNamesProduceDifferentSockets()
    {
        const QString a = ViewModuleHost::buildSocketName(
            "long_module_name_that_exceeds_limits_variant_a",
            "abcd1234", "/tmp");
        const QString b = ViewModuleHost::buildSocketName(
            "long_module_name_that_exceeds_limits_variant_b",
            "abcd1234", "/tmp");
        QVERIFY(a != b);
    }

    // Very long tempPath that leaves barely any room: the function must
    // still produce a valid name without exceeding the limit.
    void veryLongTempPath()
    {
        // 80-byte temp path leaves only ~17 bytes for the socket name.
        const QString longTemp = QString("/tmp/") + QString(75, 'x');
        const QString name = ViewModuleHost::buildSocketName(
            "some_module", "abcd1234", longTemp);

        QVERIFY(!name.isEmpty());
        verifyFits(name, longTemp);
    }

    // Extreme temp path: even the hash+suffix barely fits.
    void extremelyLongTempPath()
    {
        // 90-byte temp path leaves only ~7 bytes for the socket name.
        const QString extremeTemp = QString("/tmp/") + QString(85, 'x');
        const QString name = ViewModuleHost::buildSocketName(
            "some_module", "ab12", extremeTemp);

        QVERIFY(!name.isEmpty());
        verifyFits(name, extremeTemp);
    }

    // UTF-8 module names must not split multi-byte characters.
    void utf8SafeTruncation()
    {
        const QString unicodeName = QString::fromUtf8(
            "模块_пример_モジュール_الوحدة_가나다라마바사아자차카타파하");
        const QString name = ViewModuleHost::buildSocketName(
            unicodeName, "abcd1234", "/tmp");

        QVERIFY(!name.isEmpty());
        // Round-trips cleanly through UTF-8
        QCOMPARE(QString::fromUtf8(name.toUtf8()), name);
        verifyFits(name, "/tmp");
    }
};

QTEST_GUILESS_MAIN(ViewModuleHostTest)
#include "test_view_module_host.moc"
