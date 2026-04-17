#include "ViewModuleHost.h"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QUuid>
#include <QDebug>

ViewModuleHost::ViewModuleHost(QObject* parent)
    : QObject(parent)
{
}

static QString leftUtf8Bytes(const QString& value, int maxBytes)
{
    if (maxBytes <= 0)
        return QString();

    const QByteArray utf8 = value.toUtf8();
    if (utf8.size() <= maxBytes)
        return value;

    QByteArray truncated = utf8.left(maxBytes);
    while (!truncated.isEmpty()) {
        const QString decoded =
            QString::fromUtf8(truncated.constData(), truncated.size());
        if (decoded.toUtf8().size() == truncated.size())
            return decoded;
        truncated.chop(1);
    }
    return QString();
}

QString ViewModuleHost::buildSocketName(const QString& moduleName,
                                        const QString& uniqueId,
                                        const QString& tempPath)
{
    constexpr int kHashHexLen = 16;

    const QByteArray prefixUtf8 = QByteArrayLiteral("logos_view_");
    const QByteArray suffixUtf8 = QByteArrayLiteral("_") + uniqueId.toUtf8();
    const QByteArray baseUtf8 = prefixUtf8 + moduleName.toUtf8() + suffixUtf8;

    // sockaddr_un.sun_path is 104 bytes on macOS/BSD.
    // Full path: <tempPath>/<socketName> plus Qt creates a ".lock" sibling.
    const int tempBytes = tempPath.toUtf8().size() + 1; // +1 for path separator
    const int maxBytes = 104 - tempBytes - 5 - 1;       // -5 ".lock", -1 NUL

    if (baseUtf8.size() <= maxBytes)
        return QString::fromUtf8(baseUtf8);

    const QByteArray moduleHashBytes =
        QCryptographicHash::hash(moduleName.toUtf8(), QCryptographicHash::Sha1)
            .toHex()
            .left(kHashHexLen);
    const QString moduleHash = QString::fromLatin1(moduleHashBytes);

    // Minimum result when we drop the readable prefix and the logos_view_
    // prefix: just "<hash>_<uniqueId>".
    const int minimalBytes = kHashHexLen + suffixUtf8.size();
    if (maxBytes < minimalBytes) {
        // Extreme case: even hash+suffix doesn't fit. Truncate the hash.
        const int hashBudget = qMax(4, maxBytes - suffixUtf8.size());
        return QString::fromLatin1(moduleHashBytes.left(hashBudget))
               + QString::fromUtf8(suffixUtf8);
    }

    // Fixed overhead: "logos_view_" + "_" + hash + suffix
    const int fixedBytes = prefixUtf8.size() + 1 + kHashHexLen + suffixUtf8.size();
    if (maxBytes < fixedBytes) {
        return moduleHash + QString::fromUtf8(suffixUtf8);
    }

    const int modulePrefixBytes = maxBytes - fixedBytes;
    const QString modulePrefix = leftUtf8Bytes(moduleName, modulePrefixBytes);

    if (modulePrefix.isEmpty())
        return QStringLiteral("logos_view_") + moduleHash + QString::fromUtf8(suffixUtf8);

    return QStringLiteral("logos_view_%1_%2%3")
        .arg(modulePrefix, moduleHash, QString::fromUtf8(suffixUtf8));
}

ViewModuleHost::~ViewModuleHost()
{
    stop();
}

bool ViewModuleHost::spawn(const QString& moduleName, const QString& pluginPath)
{
    if (m_process) {
        qWarning() << "ViewModuleHost: process already running for" << m_moduleName;
        return false;
    }

    m_moduleName = moduleName;
    m_stdoutBuffer.clear();
    m_readyEmitted = false;

    QString uniqueId = QUuid::createUuid().toString(QUuid::Id128).left(8);
    m_socketName = buildSocketName(moduleName, uniqueId, QDir::tempPath());

    QString appDir = QCoreApplication::applicationDirPath();
    QString uiHostPath = QDir(appDir).filePath("ui-host");

#ifdef Q_OS_WIN
    uiHostPath += ".exe";
#endif

    if (!QFile::exists(uiHostPath)) {
        qWarning() << "ViewModuleHost: ui-host binary not found at" << uiHostPath;
        return false;
    }

    QProcess* process = new QProcess(this);
    m_process = process;

    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, process](int exitCode, QProcess::ExitStatus) {
        qDebug() << "ViewModuleHost: process exited for" << m_moduleName << "with code" << exitCode;
        if (m_process == process) {
            m_process = nullptr;
        }
        process->deleteLater();
        emit processExited(exitCode);
    });

    connect(process, &QProcess::readyReadStandardOutput, this, [this, process]() {
        m_stdoutBuffer.append(process->readAllStandardOutput());
        // Parse complete lines so the READY marker isn't missed if it spans
        // multiple read chunks. Emit ready() at most once.
        int newlineIdx;
        while ((newlineIdx = m_stdoutBuffer.indexOf('\n')) != -1) {
            QByteArray line = m_stdoutBuffer.left(newlineIdx);
            m_stdoutBuffer.remove(0, newlineIdx + 1);
            if (!m_readyEmitted && line.trimmed() == "READY") {
                m_readyEmitted = true;
                qDebug() << "ViewModuleHost: process ready for" << m_moduleName;
                emit ready();
            }
        }
    });

    connect(process, &QProcess::readyReadStandardError, this, [this, process]() {
        QByteArray data = process->readAllStandardError();
        qDebug() << "ui-host [" << m_moduleName << "]:" << data.trimmed();
    });

    QStringList args;
    args << "--name" << moduleName
         << "--path" << pluginPath
         << "--socket" << m_socketName;

    qDebug() << "ViewModuleHost: spawning" << uiHostPath << args;
    m_process->start(uiHostPath, args);

    if (!process->waitForStarted(5000)) {
        qWarning() << "ViewModuleHost: failed to start ui-host for" << moduleName;
        m_process = nullptr;
        delete process;
        return false;
    }

    return true;
}

void ViewModuleHost::stop()
{
    QProcess* process = m_process;
    if (!process) {
        return;
    }

    qDebug() << "ViewModuleHost: stopping process for" << m_moduleName;

    // Leave m_process pointing at the QProcess until the finished() handler
    // clears it, so any in-flight readyRead lambdas still see a valid pointer.
    process->terminate();

    if (!process->waitForFinished(3000)) {
        qWarning() << "ViewModuleHost: process did not exit gracefully, killing" << m_moduleName;
        process->kill();
        process->waitForFinished(1000);
    }
}

bool ViewModuleHost::isRunning() const
{
    return m_process && m_process->state() == QProcess::Running;
}

QString ViewModuleHost::socketName() const
{
    return m_socketName;
}
