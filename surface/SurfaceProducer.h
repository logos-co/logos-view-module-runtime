#pragma once

// RFC-7 §1 / RFC-8: the view-side QQuickWindow a module renders into under
// the channel transport, driven by a QQuickRenderControl. Never shown; sized
// by the host's Configure; renders exactly one frame per granted credit into
// the ViewSlot it is handed; replays host input as ordinary QEvents.

#include "SurfaceExport.h"
#include "SurfaceProtocol.h"

#include <QPointer>
#include <QQuickRenderControl>
#include <QSize>

class QQuickWindow;
class QPointingDevice;

namespace logos::surface {

class ViewSlot;

class LOGOS_VIEW_SURFACE_EXPORT SurfaceProducer : public QQuickRenderControl {
    Q_OBJECT
public:
    explicit SurfaceProducer(QObject* parent = nullptr);
    ~SurfaceProducer() override;

    // Creates the window on first call (the module has chosen its graphics
    // API by then, RFC-8 §2). Main thread.
    QQuickWindow* window();
    bool hasWindow() const { return m_window != nullptr; }

    // Our scene-graph API as a protocol value, for Hello.
    static uint32_t graphicsApiValue();

    // Host geometry for the current generation.
    void configure(const QSize& logicalSize, qreal devicePixelRatio);

    // Renders one frame into `slot`. False when nothing could be rendered
    // (no window yet, slot not openable, GPU failure); the caller then keeps
    // or drops the credit as RFC-7 §5 says.
    bool renderFrame(ViewSlot* slot);

    void replayInput(const Input& input);

    // True when the scene asked for a frame since the last renderFrame().
    bool hasDemand() const { return m_demand; }
    void clearDemand() { m_demand = false; }

    QWindow* renderWindow(QPoint* offset) override;

signals:
    // Emitted (coalesced) when the scene changes and a frame would be useful.
    void demand();

private:
public:
    // Initialises the render control (and its QRhi) once a window exists.
    bool ensureReady();
    bool isSoftware() const;
private:
    void noteDemand();

    QQuickWindow* m_window = nullptr;
    QPointingDevice* m_touchDevice = nullptr;
    QSize m_logicalSize;
    qreal m_dpr = 1.0;
    bool m_initialized = false;
    bool m_demand = false;
    bool m_demandQueued = false;
};

} // namespace logos::surface
