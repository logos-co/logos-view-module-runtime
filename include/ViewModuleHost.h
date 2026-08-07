#pragma once

#include <QObject>
#include <QProcess>
#include <QString>

class QLocalServer;

class ViewModuleHost : public QObject {
    Q_OBJECT
public:
    explicit ViewModuleHost(QObject* parent = nullptr);
    ~ViewModuleHost();

    bool spawn(const QString& moduleName, const QString& pluginPath,
               const QString& authToken);
    void stop();
    bool isRunning() const;
    QString socketName() const;

signals:
    void processExited(int exitCode);
    void ready();

private:
    QProcess* m_process = nullptr;
    QLocalServer* m_tokenServer = nullptr;
    QString m_moduleName;
    QString m_socketName;
    QByteArray m_stdoutBuffer;
    bool m_readyEmitted = false;
#ifdef Q_OS_WIN
    // The child's MAIN THREAD id, which is what stop() posts WM_QUIT to.
    // QProcess does not expose it directly, but CreateProcessArguments carries
    // the PROCESS_INFORMATION pointer it hands to CreateProcess -- capture the
    // pointer in the modifier (which runs BEFORE CreateProcess, so dwThreadId is
    // not filled in yet) and read it once started() has fired.
    //
    // m_procInfo is owned by QProcess and deleted in its cleanup(); it is nulled
    // as soon as started() reads through it and is never dereferenced elsewhere.
    void* m_procInfo = nullptr;
    unsigned long m_mainThreadId = 0;   // DWORD
#endif
};
