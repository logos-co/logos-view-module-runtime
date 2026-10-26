// RFC-7 §4, kind `shm`: a host-created, fixed-size QSharedMemory segment per
// slot. The view renders into a private QRhiTexture (or, under the software
// scene graph, grabs a QImage) and copies the pixels into the mapping; the
// host wraps the mapping in a QImage and uploads it with
// QQuickWindow::createTextureFromImage. One copy in the view, one upload in
// the host; works on every OS and under QT_QPA_PLATFORM=offscreen, which is
// what makes it the CI path for the whole channel protocol.

#include "SlotStorage.h"

#include <QLoggingCategory>
#include <QQuickRenderTarget>
#include <QQuickWindow>
#include <QRandomGenerator>
#include <QSGTexture>
#include <QSharedMemory>
#include <rhi/qrhi.h>

#include <cstring>

Q_LOGGING_CATEGORY(lcSurfaceShm, "logos.surface.shm")

namespace logos::surface {

bool slotGeometryWithinBounds(const QSize& pixelSize)
{
    if (pixelSize.width() <= 0 || pixelSize.height() <= 0) return false;
    if (uint32_t(pixelSize.width()) > kMaxPixelDimension) return false;
    if (uint32_t(pixelSize.height()) > kMaxPixelDimension) return false;
    const uint64_t bytes = uint64_t(pixelSize.width()) * 4u * uint64_t(pixelSize.height());
    return bytes <= kMaxSlotBytes;
}

QString transportNameForKind(uint32_t kind)
{
    switch (kind) {
    case SlotKindShm: return QStringLiteral("channel-shm");
    case SlotKindD3D11: return QStringLiteral("channel-d3d11");
    case SlotKindIOSurface: return QStringLiteral("channel-iosurface");
    default: return QStringLiteral("none");
    }
}

namespace {

QImage::Format imageFormatFor(uint32_t format)
{
    return format == FormatBGRA8Premultiplied ? QImage::Format_ARGB32_Premultiplied
                                              : QImage::Format_RGBA8888_Premultiplied;
}

QString randomKey()
{
    QByteArray raw(16, Qt::Uninitialized);
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32*>(raw.data()), 4);
    return QStringLiteral("logos-surface-") + QString::fromLatin1(raw.toHex());
}

// ── Host ───────────────────────────────────────────────────────────────────

class ShmHostSlot final : public HostSlot {
public:
    bool allocate(uint32_t index, const QSize& pixelSize, uint32_t format, qint64, QQuickWindow*) override
    {
        if (!slotGeometryWithinBounds(pixelSize)) return false;
        const uint32_t stride = uint32_t(pixelSize.width()) * 4u;
        const uint64_t bytes = uint64_t(stride) * uint64_t(pixelSize.height());

        m_shm = std::make_unique<QSharedMemory>(QSharedMemory::platformSafeKey(randomKey()));
        if (!m_shm->create(qsizetype(bytes), QSharedMemory::ReadWrite)) {
            qCWarning(lcSurfaceShm) << "create failed:" << m_shm->errorString();
            m_shm.reset();
            return false;
        }
        std::memset(m_shm->data(), 0, size_t(bytes));

        m_descriptor.slot = index;
        m_descriptor.kind = SlotKindShm;
        m_descriptor.pixelSize = pixelSize;
        m_descriptor.stride = stride;
        m_descriptor.byteLength = bytes;
        m_descriptor.nativeHandle = 0;
        m_descriptor.key = m_shm->nativeIpcKey().toString().toUtf8();
        m_descriptor.format = format;
        return true;
    }

    QImage image() const override
    {
        if (!m_shm || !m_shm->constData()) return {};
        // Borrows the mapping. Valid until the slot is released or destroyed,
        // which the ring guarantees happens after the texture was uploaded.
        return QImage(static_cast<const uchar*>(m_shm->constData()), m_descriptor.pixelSize.width(),
                      m_descriptor.pixelSize.height(), int(m_descriptor.stride),
                      imageFormatFor(m_descriptor.format));
    }

    QSGTexture* createTexture(QQuickWindow* window) override
    {
        const QImage img = image();
        if (img.isNull() || !window) return nullptr;
        return window->createTextureFromImage(img, QQuickWindow::TextureHasAlphaChannel);
    }

    QString transportName() const override { return transportNameForKind(SlotKindShm); }

private:
    std::unique_ptr<QSharedMemory> m_shm;
};

// ── View ───────────────────────────────────────────────────────────────────

class ShmViewSlot final : public ViewSlot {
public:
    ~ShmViewSlot() override { releaseRhi(); }

    bool open(const SlotDescriptor& descriptor, QRhi* rhi) override
    {
        if (descriptor.kind != SlotKindShm) return false;
        if (!slotGeometryWithinBounds(descriptor.pixelSize)) return false;
        const uint64_t needed = uint64_t(descriptor.stride) * uint64_t(descriptor.pixelSize.height());
        if (descriptor.stride < uint32_t(descriptor.pixelSize.width()) * 4u || needed > descriptor.byteLength)
            return false;

        const QNativeIpcKey key = QNativeIpcKey::fromString(QString::fromUtf8(descriptor.key));
        if (key.isEmpty()) return false;
        m_shm = std::make_unique<QSharedMemory>(key);
        if (!m_shm->attach(QSharedMemory::ReadWrite)) {
            qCWarning(lcSurfaceShm) << "attach failed:" << m_shm->errorString();
            m_shm.reset();
            return false;
        }
        if (uint64_t(m_shm->size()) < needed) {
            qCWarning(lcSurfaceShm) << "segment smaller than described" << m_shm->size() << needed;
            m_shm.reset();
            return false;
        }
        m_descriptor = descriptor;
        m_rhi = rhi;
        return true;
    }

    QQuickRenderTarget renderTarget() override
    {
        if (!m_rhi) return {};
        if (!m_texture) {
            m_texture.reset(m_rhi->newTexture(QRhiTexture::RGBA8, m_descriptor.pixelSize, 1,
                                              QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
            if (!m_texture->create()) { releaseRhi(); return {}; }
            m_renderTarget.reset(m_rhi->newTextureRenderTarget({ m_texture.get() }));
            m_renderPass.reset(m_renderTarget->newCompatibleRenderPassDescriptor());
            m_renderTarget->setRenderPassDescriptor(m_renderPass.get());
            if (!m_renderTarget->create()) { releaseRhi(); return {}; }
        }
        return QQuickRenderTarget::fromRhiRenderTarget(m_renderTarget.get());
    }

    bool finishRender(QRhi* rhi) override
    {
        if (!rhi || !m_texture || !m_shm) return false;
        // A separate offscreen frame whose end blocks until the GPU has
        // finished and the readback has landed: RFC-7 §4, "the view's
        // readback completes before FrameReady".
        QRhiCommandBuffer* cb = nullptr;
        if (rhi->beginOffscreenFrame(&cb) != QRhi::FrameOpSuccess) return false;
        QRhiReadbackResult result;
        QRhiResourceUpdateBatch* batch = rhi->nextResourceUpdateBatch();
        batch->readBackTexture(QRhiReadbackDescription(m_texture.get()), &result);
        cb->resourceUpdate(batch);
        rhi->endOffscreenFrame();
        if (result.data.isEmpty()) return false;

        const int w = m_descriptor.pixelSize.width();
        const int h = m_descriptor.pixelSize.height();
        const int srcStride = w * 4;
        if (result.data.size() < srcStride * h) return false;
        const bool flip = rhi->isYUpInFramebuffer();
        uchar* dst = static_cast<uchar*>(m_shm->data());
        const uchar* src = reinterpret_cast<const uchar*>(result.data.constData());
        for (int y = 0; y < h; ++y) {
            const int sy = flip ? (h - 1 - y) : y;
            std::memcpy(dst + size_t(y) * m_descriptor.stride, src + size_t(sy) * srcStride, size_t(srcStride));
        }
        return true;
    }

    bool storeSoftwareFrame(const QImage& frame) override
    {
        if (!m_shm || frame.isNull()) return false;
        QImage img = frame;
        const QImage::Format want = imageFormatFor(m_descriptor.format);
        if (img.format() != want) img = img.convertToFormat(want);
        if (img.size() != m_descriptor.pixelSize) {
            // The host configured pixelSize; a mismatch means a stale frame.
            return false;
        }
        uchar* dst = static_cast<uchar*>(m_shm->data());
        const int rowBytes = img.width() * 4;
        for (int y = 0; y < img.height(); ++y)
            std::memcpy(dst + size_t(y) * m_descriptor.stride, img.constScanLine(y), size_t(rowBytes));
        return true;
    }

private:
    void releaseRhi()
    {
        m_renderTarget.reset();
        m_renderPass.reset();
        m_texture.reset();
    }

    std::unique_ptr<QSharedMemory> m_shm;
    QRhi* m_rhi = nullptr;
    std::unique_ptr<QRhiTexture> m_texture;
    std::unique_ptr<QRhiRenderPassDescriptor> m_renderPass;
    std::unique_ptr<QRhiTextureRenderTarget> m_renderTarget;
};

} // namespace

// Native kinds are provided by their platform files; declared here so the
// factories below can reach them without a header per kind.
std::unique_ptr<HostSlot> createD3D11HostSlot();
std::unique_ptr<ViewSlot> createD3D11ViewSlot();
std::unique_ptr<HostSlot> createIOSurfaceHostSlot();
std::unique_ptr<ViewSlot> createIOSurfaceViewSlot();

std::unique_ptr<HostSlot> createHostSlot(uint32_t kind)
{
    switch (kind) {
    case SlotKindShm: return std::make_unique<ShmHostSlot>();
#if defined(Q_OS_WIN)
    case SlotKindD3D11: return createD3D11HostSlot();
#endif
#if defined(Q_OS_MACOS)
    case SlotKindIOSurface: return createIOSurfaceHostSlot();
#endif
    default: return nullptr;
    }
}

std::unique_ptr<ViewSlot> createViewSlot(uint32_t kind)
{
    switch (kind) {
    case SlotKindShm: return std::make_unique<ShmViewSlot>();
#if defined(Q_OS_WIN)
    case SlotKindD3D11: return createD3D11ViewSlot();
#endif
#if defined(Q_OS_MACOS)
    case SlotKindIOSurface: return createIOSurfaceViewSlot();
#endif
    default: return nullptr;
    }
}

uint32_t hostSlotKinds()
{
    uint32_t kinds = SlotKindShm;
#if defined(Q_OS_WIN)
    kinds |= SlotKindD3D11;
#endif
#if defined(Q_OS_MACOS)
    kinds |= SlotKindIOSurface;
#endif
    return kinds;
}

uint32_t viewSlotKinds(uint32_t graphicsApi)
{
    uint32_t kinds = SlotKindShm;
#if defined(Q_OS_WIN)
    if (graphicsApi == ApiDirect3D11) kinds |= SlotKindD3D11;
#endif
#if defined(Q_OS_MACOS)
    if (graphicsApi == ApiMetal) kinds |= SlotKindIOSurface;
#endif
    Q_UNUSED(graphicsApi);
    return kinds;
}

} // namespace logos::surface
