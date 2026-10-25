#include "ViewModuleHost.h"

#include <QLoggingCategory>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPointer>
#include <QRandomGenerator>
#include <QUuid>
#include <QDebug>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

// Off by default: child stderr is one line per log statement the ui-host makes,
// which is hundreds per app load and drowns everything else. Turn it back on
// with QT_LOGGING_RULES="logos.viewhost.debug=true" when a child misbehaves.
Q_LOGGING_CATEGORY(lcViewHost, "logos.viewhost", QtWarningMsg)

#ifdef Q_OS_WIN
namespace {
// Kill-on-close job, as logos-container-subprocess uses for logos_host: when this
// process dies, crash included, the job's last handle closes and every ui-host dies.
HANDLE uiHostJob()
{
    static HANDLE job = []() -> HANDLE {
        HANDLE h = ::CreateJobObjectW(nullptr, nullptr);
        if (h == nullptr) {
            const DWORD err = ::GetLastError();
            qCWarning(lcViewHost) << "CreateJobObject failed:" << err
                                  << "- orphaned ui-host processes will not be reaped"
                                     " if this process dies abruptly";
            return nullptr;
        }
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li{};
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if (!::SetInformationJobObject(h, JobObjectExtendedLimitInformation,
                                       &li, sizeof(li))) {
            const DWORD err = ::GetLastError();
            qCWarning(lcViewHost) << "SetInformationJobObject failed:" << err
                                  << "- continuing without kill-on-close";
        }
        return h;
    }();
    return job;
}
} // namespace
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
    return spawn(moduleName, pluginPath, authToken, ViewSpawnOptions{});
}

namespace {
// RFC-2 §2: 16 bytes from the system CSPRNG, hex. Distinct from the auth token
// (a capability credential); it only ever identifies a window to the host.
QString mintSurfaceToken()
{
    QByteArray bytes(16, Qt::Uninitialized);
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32*>(bytes.data()),
                                          bytes.size() / static_cast<int>(sizeof(quint32)));
    return QString::fromLatin1(bytes.toHex());
}
} // namespace

bool ViewModuleHost::spawn(const QString& moduleName, const QString& pluginPath,
                           const QString& authToken, const ViewSpawnOptions& options)
{
    if (m_process) {
        qCWarning(lcViewHost) << "process already running for" << m_moduleName;
        return false;
    }
    if (authToken.isEmpty()) {
        qCWarning(lcViewHost) << "refusing to spawn ui-host for" << moduleName
                   << "with an empty auth token";
        return false;
    }

    m_moduleName = moduleName;
    m_stdoutBuffer.clear();
    m_readyEmitted = false;
    m_options = options;
    if (!m_options.surfaceSocket.isEmpty()) m_options.gui = true;
    m_surfaceToken.clear();
    m_mintedSurfaceToken = m_options.gui ? mintSurfaceToken() : QString();

    QString uniqueId = QUuid::createUuid().toString(QUuid::Id128).left(8);
    m_socketName = QStringLiteral("logos_view_%1_%2").arg(moduleName, uniqueId);

    QString appDir = QCoreApplication::applicationDirPath();
    QString uiHostPath = QDir(appDir).filePath("ui-host");

#ifdef Q_OS_WIN
    uiHostPath += ".exe";
#endif

    if (!QFile::exists(uiHostPath)) {
        qCWarning(lcViewHost) << "ui-host binary not found at" << uiHostPath;
        return false;
    }

    const QString tokenSocketName = m_socketName + QStringLiteral("_token");
    QLocalServer::removeServer(tokenSocketName);
    m_tokenServer = new QLocalServer(this);
    m_tokenServer->setSocketOptions(QLocalServer::UserAccessOption);
    if (!m_tokenServer->listen(tokenSocketName)) {
        qCWarning(lcViewHost) << "failed to listen on token socket for"
                   << moduleName << ":" << m_tokenServer->errorString();
        delete m_tokenServer;
        m_tokenServer = nullptr;
        return false;
    }

    // RFC-2 §3: the auth token, then -- only for a gui spawn -- the surface
    // token as a second line. A ui-host that predates the second line would
    // take the whole payload as its auth token, but such a ui-host also
    // rejects --gui as an unknown option and never reads the socket at all.
    QPointer<QLocalServer> serverGuard(m_tokenServer);
    QByteArray payload = authToken.toUtf8();
    if (m_options.gui) {
        payload += '\n';
        payload += "SURFACE " + m_mintedSurfaceToken.toLatin1() + '\n';
    }
    connect(m_tokenServer, &QLocalServer::newConnection, this,
        [serverGuard, payload]() {
            if (!serverGuard) return;
            QLocalSocket* sock = serverGuard->nextPendingConnection();
            if (!sock) return;
            sock->write(payload);
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
            // ui-host is a console-subsystem PE, so a GUI parent (which has no
            // console to share) gets one console WINDOW per view module. Its
            // stdout/stderr are already on QProcess pipes.
            args->flags |= CREATE_NO_WINDOW;
            // Suspended until started() has put it in the job, so nothing it spawns escapes.
            args->flags |= CREATE_SUSPENDED;
        });
    connect(process, &QProcess::started, this, [this]() {
        const auto* pi = static_cast<PROCESS_INFORMATION*>(m_procInfo);
        m_procInfo = nullptr;  // QProcess owns and frees it; never hold it
        m_mainThreadId = pi ? pi->dwThreadId : 0;
        if (pi) {
            if (HANDLE job = uiHostJob(); job && !::AssignProcessToJobObject(job, pi->hProcess)) {
                const DWORD err = ::GetLastError();
                qCWarning(lcViewHost) << "AssignProcessToJobObject failed:" << err
                                      << "for" << m_moduleName;
            }
            // Undo CREATE_SUSPENDED; if this fails the child never runs.
            if (::ResumeThread(pi->hThread) == static_cast<DWORD>(-1)) {
                const DWORD err = ::GetLastError();
                qCCritical(lcViewHost) << "ResumeThread failed:" << err
                                       << "- ui-host for" << m_moduleName << "will not start";
            }
        }
        if (m_mainThreadId == 0) {
            qCWarning(lcViewHost) << "no main thread id for" << m_moduleName
                       << "- shutdown will fall back to kill()";
        }
    });
#endif

    connect(process, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this, [this, process](int exitCode, QProcess::ExitStatus) {
        qCDebug(lcViewHost) << "process exited for" << m_moduleName << "code" << exitCode;
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
            const QByteArray trimmed = line.trimmed();
            if (!m_readyEmitted
                && (trimmed == "READY" || trimmed.startsWith("READY "))) {
                // "READY <source>[ SURFACE <token>]" (RFC-2 §5). The token is
                // accepted only if it is the one this spawn minted.
                QByteArray rest = trimmed.mid(6).trimmed();
                QByteArray echoed;
                const int surfaceIdx = rest.indexOf(" SURFACE ");
                if (surfaceIdx != -1) {
                    echoed = rest.mid(surfaceIdx + 9).trimmed();
                    rest = rest.left(surfaceIdx).trimmed();
                } else if (rest.startsWith("SURFACE ")) {
                    // Empty source name: "READY SURFACE <token>".
                    echoed = rest.mid(8).trimmed();
                    rest.clear();
                }
                if (!echoed.isEmpty()) {
                    if (QString::fromLatin1(echoed) == m_mintedSurfaceToken) {
                        m_surfaceToken = m_mintedSurfaceToken;
                    } else {
                        qCWarning(lcViewHost) << "ui-host for" << m_moduleName
                                              << "echoed a surface token that was not minted for it";
                    }
                }
                m_sourceName = QString::fromUtf8(rest);
                m_readyEmitted = true;
                qCDebug(lcViewHost) << "process ready for" << m_moduleName;
                emit ready();
            }
        }
    });

    connect(process, &QProcess::readyReadStandardError, this, [this, process]() {
        QByteArray data = process->readAllStandardError();
        qCDebug(lcViewHost).noquote() << "ui-host" << m_moduleName << ":" << data.trimmed();
    });

    QStringList args;
    args << "--name" << moduleName
         << "--path" << pluginPath
         << "--socket" << m_socketName;
    // RFC-1. The surface TOKEN is never on argv (world-readable on Linux);
    // only the compositor socket name is, and that is not a secret.
    if (m_options.gui) args << "--gui";
    if (!m_options.surfaceSocket.isEmpty()) args << "--surface-socket" << m_options.surfaceSocket;

    qCDebug(lcViewHost) << "spawning" << uiHostPath << "for" << moduleName;
    m_process->start(uiHostPath, args);

    if (!process->waitForStarted(5000)) {
        qCWarning(lcViewHost) << "failed to start ui-host for" << moduleName;
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

    qCDebug(lcViewHost) << "stopping process for" << m_moduleName;

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
        qCWarning(lcViewHost) << "WM_QUIT not honoured by" << m_moduleName;
    } else {
        qCWarning(lcViewHost) << "no main thread id for" << m_moduleName;
    }
    // Fall through to kill(): terminate() would only add dead time here.
    qCWarning(lcViewHost) << "process did not exit gracefully, killing" << m_moduleName;
    process->kill();
    process->waitForFinished(1000);
#else
    process->terminate();

    if (!process->waitForFinished(3000)) {
        qCWarning(lcViewHost) << "process did not exit gracefully, killing" << m_moduleName;
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

QString ViewModuleHost::sourceName() const
{
    return m_sourceName;
}

QString ViewModuleHost::surfaceToken() const
{
    return m_surfaceToken;
}

qint64 ViewModuleHost::processId() const
{
    return isRunning() ? m_process->processId() : 0;
}

bool ViewModuleHost::isGui() const
{
    return m_options.gui;
}
