#include "SurfaceProducer.h"

#include "SlotStorage.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFocusEvent>
#include <QKeyEvent>
#include <QLoggingCategory>
#include <QMouseEvent>
#include <QPointingDevice>
#include <QQuickItem>
#include <QQuickRenderTarget>
#include <QQuickWindow>
#include <QTouchEvent>
#include <QWheelEvent>
#include <QtMath>

Q_LOGGING_CATEGORY(lcSurfaceProducer, "logos.surface.producer")

namespace logos::surface {

SurfaceProducer::SurfaceProducer(QObject* parent) : QQuickRenderControl(parent)
{
    connect(this, &QQuickRenderControl::renderRequested, this, &SurfaceProducer::noteDemand);
    connect(this, &QQuickRenderControl::sceneChanged, this, &SurfaceProducer::noteDemand);
}

SurfaceProducer::~SurfaceProducer()
{
    // The window must go before the render control it belongs to; invalidate
    // first so the scene graph releases its resources on this thread.
    if (m_initialized && !isSoftware()) invalidate();
    delete m_window;
    m_window = nullptr;
}

uint32_t SurfaceProducer::graphicsApiValue()
{
    switch (QQuickWindow::graphicsApi()) {
    case QSGRendererInterface::Software: return ApiSoftware;
    case QSGRendererInterface::OpenGL: return ApiOpenGL;
    case QSGRendererInterface::Direct3D11: return ApiDirect3D11;
    case QSGRendererInterface::Direct3D12: return ApiDirect3D12;
    case QSGRendererInterface::Metal: return ApiMetal;
    case QSGRendererInterface::Vulkan: return ApiVulkan;
    default: return ApiUnknown;
    }
}

QQuickWindow* SurfaceProducer::window()
{
    if (!m_window) {
        m_window = new QQuickWindow(this);
        m_window->setColor(Qt::transparent);
        if (m_logicalSize.isValid()) {
            m_window->resize(m_logicalSize);
            m_window->contentItem()->setSize(QSizeF(m_logicalSize));
        }
        m_touchDevice = new QPointingDevice(QStringLiteral("logos-surface-touch"), 1,
                                            QInputDevice::DeviceType::TouchScreen,
                                            QPointingDevice::PointerType::Finger,
                                            QInputDevice::Capability::Position | QInputDevice::Capability::Pressure,
                                            10, 0, QString(), QPointingDeviceUniqueId(), m_window);
    }
    return m_window;
}

QWindow* SurfaceProducer::renderWindow(QPoint* offset)
{
    if (offset) *offset = QPoint();
    return nullptr;
}

void SurfaceProducer::configure(const QSize& logicalSize, qreal devicePixelRatio)
{
    m_logicalSize = logicalSize;
    m_dpr = devicePixelRatio;
    if (m_window) {
        m_window->resize(logicalSize);
        m_window->contentItem()->setSize(QSizeF(logicalSize));
    }
    m_demand = true;
}

bool SurfaceProducer::isSoftware() const
{
    return m_window && m_window->rendererInterface()
           && m_window->rendererInterface()->graphicsApi() == QSGRendererInterface::Software;
}

bool SurfaceProducer::ensureReady()
{
    if (m_initialized) return true;
    if (!m_window) return false;
    // The software adaptation has no QRhi: QQuickRenderControl::initialize()
    // would try to create one (and refuse the render context afterwards);
    // QQuickRenderControlPrivate::grab() drives polish/sync/render on its own.
    if (isSoftware()) {
        m_initialized = true;
        return true;
    }
    // With no graphics device set, the render control creates a QRhi for the
    // window's graphics API.
    if (!initialize()) {
        qCWarning(lcSurfaceProducer) << "QQuickRenderControl::initialize failed";
        return false;
    }
    m_initialized = true;
    return true;
}

bool SurfaceProducer::renderFrame(ViewSlot* slot)
{
    if (!slot || !m_window) return false;
    if (!ensureReady()) return false;
    m_demand = false;

    if (isSoftware()) {
        // QQuickWindow::grabWindow() on a render-controlled window forwards to
        // QQuickRenderControlPrivate::grab(): polish, sync and render into a
        // QImage of the window size × DPR (software path only).
        QImage frame = m_window->grabWindow();
        // The software renderer paints at the platform screen's DPR (there is
        // no custom render target to carry the host's); when the host asked
        // for another scale, resample to the slot's pixel size. The GPU paths
        // below render at the host's DPR natively.
        const QSize pixelSize(qMax(1, qRound(m_logicalSize.width() * m_dpr)),
                              qMax(1, qRound(m_logicalSize.height() * m_dpr)));
        if (!frame.isNull() && frame.size() != pixelSize)
            frame = frame.scaled(pixelSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        return slot->storeSoftwareFrame(frame);
    }

    QRhi* rhi = m_window->rhi();
    if (!rhi) return false;
    QQuickRenderTarget target = slot->renderTarget();
    if (target.isNull()) return false;
    target.setDevicePixelRatio(m_dpr);
    m_window->setRenderTarget(target);

    if (!slot->beginRender()) return false;
    QElapsedTimer clock;
    clock.start();
    polishItems();
    beginFrame();
    sync();
    render();
    endFrame();
    const qint64 renderedUs = clock.nsecsElapsed() / 1000;
    const bool ok = slot->finishRender(rhi);
    // Per-frame cost split: scene graph (sync+render+submit) vs. the slot's
    // own finish (for shm: the GPU readback into the ring). Debug category
    // only; nothing is formatted unless logos.surface.producer.debug is on.
    qCDebug(lcSurfaceProducer, "frame %dx%d: render %lld us, finish %lld us", m_logicalSize.width(),
            m_logicalSize.height(), (long long)renderedUs, (long long)(clock.nsecsElapsed() / 1000 - renderedUs));
    return ok;
}

void SurfaceProducer::noteDemand()
{
    m_demand = true;
    if (m_demandQueued) return;
    m_demandQueued = true;
    QMetaObject::invokeMethod(this, [this] {
        m_demandQueued = false;
        if (m_demand) emit demand();
    }, Qt::QueuedConnection);
}

// ── Input replay ───────────────────────────────────────────────────────────

void SurfaceProducer::replayInput(const Input& in)
{
    if (!m_window) return;
    switch (in.kind) {
    case InputKind::MouseMove:
    case InputKind::MousePress:
    case InputKind::MouseRelease:
    case InputKind::MouseDoubleClick: {
        QEvent::Type type = QEvent::MouseMove;
        if (in.kind == InputKind::MousePress) type = QEvent::MouseButtonPress;
        else if (in.kind == InputKind::MouseRelease) type = QEvent::MouseButtonRelease;
        else if (in.kind == InputKind::MouseDoubleClick) type = QEvent::MouseButtonDblClick;
        const QPointF local(in.mouse.x, in.mouse.y);
        QMouseEvent ev(type, local, local, QPointF(in.mouse.gx, in.mouse.gy),
                       Qt::MouseButton(in.mouse.button), Qt::MouseButtons(in.mouse.buttons),
                       Qt::KeyboardModifiers(in.mouse.modifiers));
        ev.setTimestamp(in.mouse.timestamp);
        QCoreApplication::sendEvent(m_window, &ev);
        break;
    }
    case InputKind::Wheel: {
        const QPointF local(in.wheel.x, in.wheel.y);
        QWheelEvent ev(local, QPointF(in.wheel.gx, in.wheel.gy),
                       QPoint(in.wheel.pixelDx, in.wheel.pixelDy), QPoint(in.wheel.angleDx, in.wheel.angleDy),
                       Qt::MouseButtons(in.wheel.buttons), Qt::KeyboardModifiers(in.wheel.modifiers),
                       Qt::ScrollPhase(in.wheel.phase), in.wheel.inverted != 0);
        ev.setTimestamp(in.wheel.timestamp);
        QCoreApplication::sendEvent(m_window, &ev);
        break;
    }
    case InputKind::Touch: {
        QList<QEventPoint> points;
        points.reserve(in.touch.points.size());
        for (const TouchPointInput& p : in.touch.points) {
            points.append(QEventPoint(p.id, QEventPoint::State(p.state), QPointF(p.x, p.y), QPointF(p.gx, p.gy)));
        }
        QTouchEvent ev(QEvent::Type(in.touch.type), m_touchDevice, Qt::KeyboardModifiers(in.touch.modifiers), points);
        ev.setTimestamp(in.touch.timestamp);
        QCoreApplication::sendEvent(m_window, &ev);
        break;
    }
    case InputKind::KeyPress:
    case InputKind::KeyRelease: {
        QKeyEvent ev(in.kind == InputKind::KeyPress ? QEvent::KeyPress : QEvent::KeyRelease, in.key.key,
                     Qt::KeyboardModifiers(in.key.modifiers), in.key.nativeScanCode, in.key.nativeVirtualKey,
                     in.key.nativeModifiers, in.key.text, in.key.autoRepeat != 0, in.key.count);
        QCoreApplication::sendEvent(m_window, &ev);
        break;
    }
    case InputKind::FocusIn: {
        QFocusEvent ev(QEvent::FocusIn, Qt::OtherFocusReason);
        QCoreApplication::sendEvent(m_window, &ev);
        break;
    }
    case InputKind::FocusOut: {
        // Cancel whatever gesture was in flight (RFC-7 §6): the host lost the
        // pointer and will not send the matching release.
        QTouchEvent cancel(QEvent::TouchCancel, m_touchDevice);
        QCoreApplication::sendEvent(m_window, &cancel);
        QFocusEvent ev(QEvent::FocusOut, Qt::OtherFocusReason);
        QCoreApplication::sendEvent(m_window, &ev);
        break;
    }
    case InputKind::Leave: {
        QEvent ev(QEvent::Leave);
        QCoreApplication::sendEvent(m_window, &ev);
        break;
    }
    }
}

} // namespace logos::surface
