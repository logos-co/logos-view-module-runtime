// A view plugin that records what kind of process ui-host gave it (RFC-1) and
// what xdg app_id its windows would carry (RFC-2), then opens a QQuickWindow
// and records when it is exposed.
//
// Same journal discipline as test_unload_fixture.cpp: one line per entry,
// flushed, so the test reads facts the plugin observed from inside the child
// rather than inferring them from timing or exit codes.
//
//   app <gui|core>                 qobject_cast<QGuiApplication*>(qApp) != nullptr
//   app-id <desktopFileName|->     QGuiApplication::desktopFileName() at initLogos
//   window-exposed                 the QQuickWindow reached isExposed()
//   quit-on-last-window <0|1>      QGuiApplication::quitOnLastWindowClosed()
//   dtor                           the plugin was destroyed (ui-host unwound)
//
// Without --gui the plugin still loads (it is the "old host" case of RFC-1
// Compatibility) and must not crash: it records `app core`, opens no window.
#include <QCoreApplication>
#include <QGuiApplication>
#include <QObject>
#include <QQuickWindow>
#include <QTimer>
#include <QtPlugin>

#include <cstdio>
#include <cstdlib>
#include <memory>

// ui-host resolves initLogos(LogosAPI*) by signature; the fixture only needs
// the name to match, never the type.
class LogosAPI;
Q_DECLARE_OPAQUE_POINTER(LogosAPI*)
Q_DECLARE_METATYPE(LogosAPI*)

namespace {

void journal(const QByteArray& entry)
{
    const char* path = ::getenv("LOGOS_TEST_GUI_JOURNAL");
    if (!path || !*path) return;
    FILE* f = ::fopen(path, "a");
    if (!f) return;
    ::fprintf(f, "%s\n", entry.constData());
    ::fflush(f);
    ::fclose(f);
}

} // namespace

class GuiFixturePlugin : public QObject
{
    Q_OBJECT
    Q_PLUGIN_METADATA(IID "co.logos.test.GuiFixturePlugin")
public:
    GuiFixturePlugin() = default;
    ~GuiFixturePlugin() override { journal("dtor"); }

    // ui-host invokes this by signature; the pointer is not used.
    Q_INVOKABLE void initLogos(LogosAPI* /*logosAPI*/)
    {
        auto* gui = qobject_cast<QGuiApplication*>(QCoreApplication::instance());
        journal(gui ? "app gui" : "app core");
        if (!gui) return;  // old-host case: nothing to render into

        const QString appId = QGuiApplication::desktopFileName();
        journal("app-id " + (appId.isEmpty() ? QByteArray("-") : appId.toUtf8()));
        journal(QByteArray("quit-on-last-window ") + (QGuiApplication::quitOnLastWindowClosed() ? "1" : "0"));

        // No GPU in the test sandbox: the software scene graph renders
        // everywhere the offscreen platform does.
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Software);
        m_window = std::make_unique<QQuickWindow>();
        m_window->setTitle(QStringLiteral("gui fixture"));
        m_window->resize(160, 120);
        QObject::connect(m_window.get(), &QQuickWindow::sceneGraphInitialized, m_window.get(), [] {
            journal("scene-graph");
        });
        m_window->show();
        // isExposed() flips from the platform's expose event, which arrives
        // through the event loop after show(); poll briefly from a timer so
        // the entry is written once it is true.
        auto* poll = new QTimer(this);
        poll->setInterval(20);
        QObject::connect(poll, &QTimer::timeout, this, [this, poll] {
            if (m_window && m_window->isExposed()) {
                journal("window-exposed");
                poll->stop();
                poll->deleteLater();
            }
        });
        poll->start();
    }

    Q_INVOKABLE QString ping() const { return QStringLiteral("pong"); }

private:
    std::unique_ptr<QQuickWindow> m_window;
};

#include "test_gui_fixture.moc"
