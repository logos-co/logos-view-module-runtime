#include "ViewModuleHost.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QUuid>
#include <QDebug>

ViewModuleHost::ViewModuleHost(QObject* parent)
    : QObject(parent)
{
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
