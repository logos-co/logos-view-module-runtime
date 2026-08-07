#include "ViewModuleHost.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPointer>
#include <QUuid>
#include <QDebug>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

ViewModuleHost::ViewModuleHost(QObject* parent)
    : QObject(parent)
{
}

ViewModuleHost::~ViewModuleHost()
{
    stop();
}

bool ViewModuleHost::spawn(const QString& moduleName, const QString& pluginPath,
                           const QString& authToken)
{
    if (m_process) {
        qWarning() << "ViewModuleHost: process already running for" << m_moduleName;
        return false;
    }
    if (authToken.isEmpty()) {
        qWarning() << "ViewModuleHost: refusing to spawn ui-host for" << moduleName
                   << "with an empty auth token";
        return false;
    }

    m_moduleName = moduleName;
    m_stdoutBuffer.clear();
    m_readyEmitted = false;

    QString uniqueId = QUuid::createUuid().toString(QUuid::Id128).left(8);
    m_socketName = QStringLiteral("logos_view_%1_%2").arg(moduleName, uniqueId);

    QString appDir = QCoreApplication::applicationDirPath();
    QString uiHostPath = QDir(appDir).filePath("ui-host");

#ifdef Q_OS_WIN
    uiHostPath += ".exe";
#endif

    if (!QFile::exists(uiHostPath)) {
        qWarning() << "ViewModuleHost: ui-host binary not found at" << uiHostPath;
        return false;
    }

    const QString tokenSocketName = m_socketName + QStringLiteral("_token");
    QLocalServer::removeServer(tokenSocketName);
    m_tokenServer = new QLocalServer(this);
    m_tokenServer->setSocketOptions(QLocalServer::UserAccessOption);
    if (!m_tokenServer->listen(tokenSocketName)) {
        qWarning() << "ViewModuleHost: failed to listen on token socket for"
                   << moduleName << ":" << m_tokenServer->errorString();
        delete m_tokenServer;
        m_tokenServer = nullptr;
        return false;
    }

    QPointer<QLocalServer> serverGuard(m_tokenServer);
    const QString tokenToSend = authToken;
    connect(m_tokenServer, &QLocalServer::newConnection, this,
        [serverGuard, tokenToSend]() {
            if (!serverGuard) return;
            QLocalSocket* sock = serverGuard->nextPendingConnection();
            if (!sock) return;
            sock->write(tokenToSend.toUtf8());
            sock->flush();
            sock->waitForBytesWritten(2000);
            sock->disconnectFromServer();
            sock->deleteLater();
            serverGuard->close();
        });

    QProcess* process = new QProcess(this);
    m_process = process;

#ifdef Q_OS_WIN
    // See stop(): terminating a windowless child on Windows needs its main
    // THREAD id, not its pid. The modifier runs before CreateProcess, so
    // dwThreadId is not populated yet -- capture the pointer and read it in
    // started().
    m_procInfo = nullptr;
    m_mainThreadId = 0;
    process->setCreateProcessArgumentsModifier(
        [this](QProcess::CreateProcessArguments* args) {
            m_procInfo = args->processInformation;
        });
    connect(process, &QProcess::started, this, [this]() {
        m_mainThreadId = m_procInfo
            ? static_cast<PROCESS_INFORMATION*>(m_procInfo)->dwThreadId : 0;
        m_procInfo = nullptr;  // QProcess owns and frees it; never hold it
        if (m_mainThreadId == 0) {
            qWarning() << "ViewModuleHost: no main thread id for" << m_moduleName
                       << "- shutdown will fall back to kill()";
        }
    });
#endif

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

    qDebug() << "ViewModuleHost: spawning" << uiHostPath << "for" << moduleName;
    m_process->start(uiHostPath, args);

    if (!process->waitForStarted(5000)) {
        qWarning() << "ViewModuleHost: failed to start ui-host for" << moduleName;
        m_process = nullptr;
        delete process;
        if (m_tokenServer) {
            m_tokenServer->close();
            m_tokenServer->deleteLater();
            m_tokenServer = nullptr;
        }
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
#ifdef Q_OS_WIN
    // REPLACES terminate() on Windows rather than preceding it, because
    // terminate() here is measurably dead time. QProcess::terminate() is
    // EnumWindows(WM_CLOSE) + PostThreadMessage(tid, WM_CLOSE); ui-host is a
    // windowless QCoreApplication (no Qt6::Gui at all), so the enumeration finds
    // nothing -- measured: 0 top-level and 0 thread windows, while the same
    // enumeration in the same run found 7 for Basecamp. The thread message IS
    // delivered, but Qt's dispatcher only branches on WM_QUIT, so WM_CLOSE falls
    // through to a no-op. terminate() returns void, so it "succeeds" doing
    // nothing and the child is then hard-killed after the full grace period:
    // measured exit code 62097 (0xF291, Qt's KillProcessExitCode).
    //
    // Same disease and same cure as the module host in
    // logos-container-subprocess, which also replaces (not supplements) its
    // no-op request_exit() with PostThreadMessage(WM_QUIT).
    // Measured against the shipped ui-host.exe: terminate() left it alive for
    // the full 3000 ms; WM_QUIT exited it in 8 ms with code 0.
    bool posted = false;
    if (m_mainThreadId != 0 && process->state() == QProcess::Running)
        posted = ::PostThreadMessageW(m_mainThreadId, WM_QUIT, 0, 0);
    if (posted) {
        if (process->waitForFinished(3000))
            return;
        qWarning() << "ViewModuleHost: WM_QUIT not honoured by" << m_moduleName;
    } else {
        qWarning() << "ViewModuleHost: no main thread id for" << m_moduleName;
    }
    // Fall through to kill(): terminate() would only add dead time here.
    qWarning() << "ViewModuleHost: process did not exit gracefully, killing" << m_moduleName;
    process->kill();
    process->waitForFinished(1000);
#else
    process->terminate();

    if (!process->waitForFinished(3000)) {
        qWarning() << "ViewModuleHost: process did not exit gracefully, killing" << m_moduleName;
        process->kill();
        process->waitForFinished(1000);
    }
#endif
}

bool ViewModuleHost::isRunning() const
{
    return m_process && m_process->state() == QProcess::Running;
}

QString ViewModuleHost::socketName() const
{
    return m_socketName;
}
