// The VIEW process of test_surface_channel: what a view_surface module does
// under the channel transport, driven exactly the way ui-host drives it —
// through the C functions of SurfaceClientApi.h and the header-only
// <logos/view_surface.h> on top of them (RFC-7 / RFC-8).
//
//   test_surface_view --server <name> --token <32 hex> --color <#rrggbb>
//                     [--journal <path>] [--delay-window <ms>]
//
// Paints the whole surface in --color through a QQuickPaintedItem and writes
// every input event it receives to the journal, one line each:
//
//   welcomed <transport>
//   configured <w>x<h> <dpr>
//   press <x> <y> <button>  | release ... | move <x> <y> | dblclick ...
//   wheel <x> <y> <angleDy>
//   key <key> <text>        | keyrelease <key>
//   touch <type> <n>
//   focus <in|out>
//   leave
//   rendered <n>            (after the n-th frame)
//
// --delay-window holds off the first LogosViewSurface::window() call, which
// is how a module that asks late is simulated (the credit must be held).

#include "SurfaceClientApi.h"
#include "logos/view_surface.h"

#include <QCommandLineParser>
#include <QGuiApplication>
#include <QPainter>
#include <QQuickItem>
#include <QQuickPaintedItem>
#include <QQuickWindow>
#include <QTimer>

#include <cstdio>

namespace {

QString g_journalPath;

void journal(const QString& line)
{
    if (g_journalPath.isEmpty()) return;
    FILE* f = ::fopen(g_journalPath.toLocal8Bit().constData(), "a");
    if (!f) return;
    ::fprintf(f, "%s\n", line.toUtf8().constData());
    ::fclose(f);
}

class Probe : public QQuickPaintedItem {
public:
    explicit Probe(const QColor& color) : m_color(color)
    {
        setAcceptedMouseButtons(Qt::AllButtons);
        setAcceptHoverEvents(true);
        setAcceptTouchEvents(true);
        setFlag(ItemAcceptsInputMethod, false);
        setFocus(true);
    }

    void paint(QPainter* p) override { p->fillRect(boundingRect(), m_color); }

protected:
    void mousePressEvent(QMouseEvent* e) override
    {
        journal(QStringLiteral("press %1 %2 %3").arg(e->position().x()).arg(e->position().y()).arg(int(e->button())));
        e->accept();
    }
    void mouseReleaseEvent(QMouseEvent* e) override
    {
        journal(QStringLiteral("release %1 %2 %3").arg(e->position().x()).arg(e->position().y()).arg(int(e->button())));
    }
    void mouseMoveEvent(QMouseEvent* e) override
    {
        journal(QStringLiteral("move %1 %2").arg(e->position().x()).arg(e->position().y()));
    }
    void mouseDoubleClickEvent(QMouseEvent* e) override
    {
        journal(QStringLiteral("dblclick %1 %2").arg(e->position().x()).arg(e->position().y()));
    }
    void hoverMoveEvent(QHoverEvent* e) override
    {
        journal(QStringLiteral("hover %1 %2").arg(e->position().x()).arg(e->position().y()));
    }
    void wheelEvent(QWheelEvent* e) override
    {
        journal(QStringLiteral("wheel %1 %2 %3").arg(e->position().x()).arg(e->position().y()).arg(e->angleDelta().y()));
        e->accept();
    }
    void keyPressEvent(QKeyEvent* e) override
    {
        journal(QStringLiteral("key %1 %2").arg(e->key()).arg(e->text()));
        e->accept();
    }
    void keyReleaseEvent(QKeyEvent* e) override
    {
        journal(QStringLiteral("keyrelease %1").arg(e->key()));
    }
    void touchEvent(QTouchEvent* e) override
    {
        journal(QStringLiteral("touch %1 %2").arg(int(e->type())).arg(e->points().size()));
        e->accept();
    }
    void focusInEvent(QFocusEvent*) override { journal(QStringLiteral("focus in")); }
    void focusOutEvent(QFocusEvent*) override { journal(QStringLiteral("focus out")); }

private:
    QColor m_color;
};

} // namespace

int main(int argc, char* argv[])
{
    // No GPU in the test sandbox: software scene graph, chosen before the
    // first window() call as RFC-8 §2 requires. LOGOS_SURFACE_TEST_GL=1 (a
    // developer desktop with a GL-capable platform plugin) takes the RHI
    // OpenGL path instead, so the texture render-target + readback half of
    // the slot storage gets exercised too.
    QQuickWindow::setGraphicsApi(qEnvironmentVariableIntValue("LOGOS_SURFACE_TEST_GL") > 0
                                     ? QSGRendererInterface::OpenGL
                                     : QSGRendererInterface::Software);
    QGuiApplication app(argc, argv);

    QCommandLineParser parser;
    QCommandLineOption serverOpt(QStringLiteral("server"), QString(), QStringLiteral("name"));
    QCommandLineOption tokenOpt(QStringLiteral("token"), QString(), QStringLiteral("hex"));
    QCommandLineOption colorOpt(QStringLiteral("color"), QString(), QStringLiteral("rgb"), QStringLiteral("#ff0000"));
    QCommandLineOption journalOpt(QStringLiteral("journal"), QString(), QStringLiteral("path"));
    QCommandLineOption delayOpt(QStringLiteral("delay-window"), QString(), QStringLiteral("ms"), QStringLiteral("0"));
    QCommandLineOption animateOpt(QStringLiteral("animate"), QStringLiteral("repaint every 30 ms"));
    parser.addOptions({serverOpt, tokenOpt, colorOpt, journalOpt, delayOpt, animateOpt});
    parser.process(app);
    g_journalPath = parser.value(journalOpt);

    const QByteArray server = parser.value(serverOpt).toUtf8();
    const QByteArray token = parser.value(tokenOpt).toLatin1();
    const QColor color(parser.value(colorOpt));

    if (logos_view_surface_init(token.constData(), server.constData(), "channel") != 0) {
        journal(QStringLiteral("init-failed"));
        return 2;
    }
    journal(QStringLiteral("transport-before %1").arg(LogosViewSurface::transport()));

    Probe* probe = nullptr;
    int frames = 0;
    auto attach = [&] {
        QQuickWindow* win = LogosViewSurface::window();
        if (!win) { journal(QStringLiteral("no-window")); return; }
        QQuickItem* root = LogosViewSurface::rootItem();
        probe = new Probe(color);
        probe->setParentItem(root);
        probe->setSize(root->size());
        QObject::connect(root, &QQuickItem::widthChanged, probe, [probe, root] { probe->setWidth(root->width()); });
        QObject::connect(root, &QQuickItem::heightChanged, probe, [probe, root] { probe->setHeight(root->height()); });
        probe->forceActiveFocus();
        QObject::connect(win, &QQuickWindow::afterRendering, &app, [&frames] {
            journal(QStringLiteral("rendered %1").arg(++frames));
        });
        journal(QStringLiteral("window %1x%2").arg(win->width()).arg(win->height()));
        if (parser.isSet(animateOpt)) {
            // A changing scene: the producer reports demand, the session asks
            // for credit (FrameWanted), the host grants one at a time.
            auto* tick = new QTimer(probe);
            tick->setInterval(30);
            QObject::connect(tick, &QTimer::timeout, probe, [probe] { probe->update(); });
            tick->start();
        }
    };

    const int delay = parser.value(delayOpt).toInt();
    if (delay > 0) QTimer::singleShot(delay, &app, attach);
    else attach();

    // Report the negotiated transport once it is known.
    auto* poll = new QTimer(&app);
    poll->setInterval(10);
    QObject::connect(poll, &QTimer::timeout, &app, [poll] {
        const QString t = LogosViewSurface::transport();
        if (t.startsWith(QLatin1String("channel-"))) {
            journal(QStringLiteral("welcomed %1").arg(t));
            poll->stop();
        }
    });
    poll->start();

    const int rc = app.exec();
    logos_view_surface_shutdown();
    return rc;
}
