#pragma once

// RFC-8: the one window a `view_surface` module renders into, on every
// SurfaceTransport (RFC-6).
//
//     #include <logos/view_surface.h>
//
//     QQuickWindow* win = LogosViewSurface::window();   // nullptr: no transport
//     QQuickItem* root = LogosViewSurface::rootItem();  // parent your content here
//     QString how = LogosViewSurface::transport();      // "wayland-shm", "channel-d3d11", ..., "none"
//
// Header-only. The runtime (ui-host, or the in-process container) publishes a
// versioned C function table as a `void*` property named "logos.viewSurface.v1"
// on the QCoreApplication; an old runtime publishes nothing and every call
// here returns nullptr / "none", so the module falls back to a window of its
// own. Nothing in the module links the surface library.
//
// Rules (RFC-8 §2): call from the main thread, from initLogos() or later;
// choose QQuickWindow::setGraphicsApi() BEFORE the first window() call; never
// show(), resize(), setScreen() or delete the window — geometry is the host's.

#include <QCoreApplication>
#include <QString>
#include <QVariant>
#include <cstdint>

class QQuickWindow;
class QQuickItem;

struct LogosViewSurfaceV1 {
    uint32_t size;
    uint32_t version;
    QQuickWindow* (*window)(void* ctx);
    QQuickItem* (*rootItem)(void* ctx);
    const char* (*transport)(void* ctx);
    void* ctx;
};

#define LOGOS_VIEW_SURFACE_PROPERTY "logos.viewSurface.v1"

namespace LogosViewSurface {

inline const LogosViewSurfaceV1* table()
{
    QCoreApplication* app = QCoreApplication::instance();
    if (!app) return nullptr;
    const QVariant v = app->property(LOGOS_VIEW_SURFACE_PROPERTY);
    if (!v.isValid() || v.typeId() != QMetaType::VoidStar) return nullptr;
    const auto* t = static_cast<const LogosViewSurfaceV1*>(v.value<void*>());
    if (!t || t->version != 1 || t->size < sizeof(LogosViewSurfaceV1)) return nullptr;
    return t;
}

inline QQuickWindow* window()
{
    const LogosViewSurfaceV1* t = table();
    return t && t->window ? t->window(t->ctx) : nullptr;
}

inline QQuickItem* rootItem()
{
    const LogosViewSurfaceV1* t = table();
    return t && t->rootItem ? t->rootItem(t->ctx) : nullptr;
}

inline QString transport()
{
    const LogosViewSurfaceV1* t = table();
    return t && t->transport ? QString::fromLatin1(t->transport(t->ctx)) : QStringLiteral("none");
}

} // namespace LogosViewSurface
