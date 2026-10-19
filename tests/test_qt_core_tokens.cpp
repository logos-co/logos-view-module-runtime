// saveCoreTokenToQtStore: core's tokens reach the store a Qt host reads.

#include <QtTest/QtTest>

#include "QtCoreTokens.h"
#include "token_manager.h"

class QtCoreTokensTest : public QObject {
    Q_OBJECT

private slots:
    void aCoreTokenLandsInTheQtStoreAndAReloadReplacesIt()
    {
        logos::ui::saveCoreTokenToQtStore("capability_module", "first");
        QCOMPARE(TokenManager::instance().getToken(QStringLiteral("capability_module")),
                 QStringLiteral("first"));
        logos::ui::saveCoreTokenToQtStore("capability_module", "second");
        QCOMPARE(TokenManager::instance().getToken(QStringLiteral("capability_module")),
                 QStringLiteral("second"));
    }
};

QTEST_GUILESS_MAIN(QtCoreTokensTest)
#include "test_qt_core_tokens.moc"
