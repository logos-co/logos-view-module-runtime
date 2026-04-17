#pragma once

#include <QObject>
#include <QProcess>
#include <QString>

class ViewModuleHost : public QObject {
    Q_OBJECT
public:
    explicit ViewModuleHost(QObject* parent = nullptr);
    ~ViewModuleHost();

    bool spawn(const QString& moduleName, const QString& pluginPath);
    void stop();
    bool isRunning() const;
    QString socketName() const;

    // Build a Unix-domain-socket-safe name. Measures lengths in UTF-8
    // bytes (matching the on-disk encoding) and keeps the full path
    // <tempPath>/<name> within sockaddr_un.sun_path (104 bytes on
    // macOS/BSD). Short names produce "logos_view_<mod>_<uid>"; long
    // names produce "logos_view_<prefix>_<hash>_<uid>"; extremely long
    // temp paths progressively drop the prefix and logos_view_ prefix
    // to stay within budget.
    static QString buildSocketName(const QString& moduleName,
                                   const QString& uniqueId,
                                   const QString& tempPath);

signals:
    void processExited(int exitCode);
    void ready();

private:
    QProcess* m_process = nullptr;
    QString m_moduleName;
    QString m_socketName;
    QByteArray m_stdoutBuffer;
    bool m_readyEmitted = false;
};
