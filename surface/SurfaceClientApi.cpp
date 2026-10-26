#include "SurfaceClientApi.h"

#include "SurfaceClientSession.h"
#include "logos/view_surface.h"

#include <QCoreApplication>
#include <QLoggingCategory>
#include <QQuickItem>
#include <QQuickWindow>
#include <QString>
#include <memory>

Q_LOGGING_CATEGORY(lcSurfaceApi, "logos.surface.api")

namespace {

using logos::surface::SurfaceClientSession;

struct State {
    QString transport;                              // "wayland" | "channel"
    std::unique_ptr<SurfaceClientSession> session;  // channel only
    QQuickWindow* waylandWindow = nullptr;          // wayland only, lazily shown
    QByteArray transportName;                       // storage for the C string
    LogosViewSurfaceV1 table{};
};

State* g_state = nullptr;

QQuickWindow* apiWindow(void*)
{
    if (!g_state) return nullptr;
    if (g_state->session) return g_state->session->window();
    if (g_state->transport == QLatin1String("wayland")) {
        if (!g_state->waylandWindow) {
            // A plain top-level: Qt's Wayland client makes it the xdg toplevel
            // with the app_id RFC-2 set; the host sizes it (RFC-3).
            auto* w = new QQuickWindow();
            w->setTitle(QStringLiteral("Logos View"));
            w->setColor(Qt::transparent);
            w->resize(640, 480);
            w->show();
            g_state->waylandWindow = w;
        }
        return g_state->waylandWindow;
    }
    return nullptr;
}

QQuickItem* apiRootItem(void* ctx)
{
    QQuickWindow* w = apiWindow(ctx);
    return w ? w->contentItem() : nullptr;
}

const char* apiTransport(void*)
{
    if (!g_state) return "none";
    QString name;
    if (g_state->session) {
        // Before negotiation completes only the family is known.
        name = g_state->session->isWelcomed() ? g_state->session->transport() : QStringLiteral("channel");
    } else if (g_state->transport == QLatin1String("wayland")) {
        // The buffer kind (shm/dmabuf) is the host's to observe; the module
        // sees the family.
        name = QStringLiteral("wayland");
    } else {
        name = QStringLiteral("none");
    }
    g_state->transportName = name.toLatin1();
    return g_state->transportName.constData();
}

} // namespace

extern "C" int logos_view_surface_init(const char* token, const char* socketName, const char* transport)
{
    QCoreApplication* app = QCoreApplication::instance();
    if (!app) return 1;
    if (g_state) logos_view_surface_shutdown();

    auto* st = new State;
    st->transport = QString::fromLatin1(transport ? transport : "");
    if (st->transport == QLatin1String("channel")) {
        st->session = std::make_unique<SurfaceClientSession>();
        if (!st->session->connectToHost(QString::fromLatin1(socketName ? socketName : ""),
                                        QString::fromLatin1(token ? token : ""))) {
            qCWarning(lcSurfaceApi) << "channel connect could not be attempted";
            delete st;
            return 2;
        }
    } else if (st->transport != QLatin1String("wayland")) {
        qCWarning(lcSurfaceApi) << "unknown transport" << st->transport;
        delete st;
        return 3;
    }

    st->table.size = sizeof(LogosViewSurfaceV1);
    st->table.version = 1;
    st->table.window = apiWindow;
    st->table.rootItem = apiRootItem;
    st->table.transport = apiTransport;
    st->table.ctx = st;
    g_state = st;
    app->setProperty(LOGOS_VIEW_SURFACE_PROPERTY, QVariant::fromValue<void*>(&st->table));
    qCInfo(lcSurfaceApi) << "view surface ready, transport" << st->transport;
    return 0;
}

extern "C" void logos_view_surface_shutdown()
{
    if (!g_state) return;
    if (QCoreApplication* app = QCoreApplication::instance())
        app->setProperty(LOGOS_VIEW_SURFACE_PROPERTY, QVariant());
    State* st = g_state;
    g_state = nullptr;
    st->session.reset();
    delete st->waylandWindow;
    delete st;
}
