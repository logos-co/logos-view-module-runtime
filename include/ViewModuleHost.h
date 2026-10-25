#pragma once

#include <QObject>
#include <QProcess>
#include <QString>

class QLocalServer;

// How a view module's ui-host is to be run (RFC-1 / RFC-2).
//
// Default-constructed options spawn exactly what spawn() always spawned: a
// QCoreApplication ui-host with --name/--path/--socket and nothing else.
struct ViewSpawnOptions {
    // RFC-1: run ui-host as a QGuiApplication so the plugin may open windows.
    // The host mints a per-spawn SURFACE token and hands it to the child on
    // the token socket; the child reports it back on its READY line.
    bool gui = false;
    // RFC-2: the Wayland socket of the host's nested compositor. Implies gui.
    // ui-host sets QT_QPA_PLATFORM=wayland and WAYLAND_DISPLAY=<this> for
    // itself before constructing its application, so its windows reach that
    // compositor and no other display.
    QString surfaceSocket;
};

class ViewModuleHost : public QObject {
    Q_OBJECT
public:
    explicit ViewModuleHost(QObject* parent = nullptr);
    ~ViewModuleHost();

    bool spawn(const QString& moduleName, const QString& pluginPath,
               const QString& authToken);
    bool spawn(const QString& moduleName, const QString& pluginPath,
               const QString& authToken, const ViewSpawnOptions& options);
    void stop();
    bool isRunning() const;
    QString socketName() const;
    QString sourceName() const;

    // RFC-2. The surface token this spawn was minted with: 32 lowercase hex
    // characters, the child's xdg app_id being "logos-view-<token>". Empty
    // for a non-gui spawn, and until ready() confirms the child echoed it.
    QString surfaceToken() const;
    // The child's pid (0 when not running), for the compositor's pid check.
    qint64 processId() const;
    bool isGui() const;

signals:
    void processExited(int exitCode);
    void ready();

private:
    QProcess* m_process = nullptr;
    QLocalServer* m_tokenServer = nullptr;
    QString m_moduleName;
    QString m_socketName;
    QString m_sourceName;
    QByteArray m_stdoutBuffer;
    bool m_readyEmitted = false;
    ViewSpawnOptions m_options;
    QString m_mintedSurfaceToken;   // what this spawn was given
    QString m_surfaceToken;         // what the child echoed on READY
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
