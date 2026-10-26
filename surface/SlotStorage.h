#pragma once

// RFC-7 §4: one frame slot, as seen from each end of the channel.
//
// The host allocates a HostSlot in OS-native storage and describes it in an
// Attach message; the view opens a ViewSlot from that description, renders
// into it and hands it back. The storage kind decides the copy count:
//
//   shm        QSharedMemory; view reads back into it, host uploads from it
//   d3d11      shared ID3D11Texture2D + keyed mutex (Windows)
//   iosurface  IOSurface + MTLTexture (macOS)
//
// Implementations: ShmSlotStorage.cpp (all OSes), D3D11SlotStorage.cpp
// (Q_OS_WIN), IOSurfaceSlotStorage.mm (Q_OS_MACOS).

#include "SurfaceExport.h"
#include "SurfaceProtocol.h"

#include <QImage>
#include <QSize>
#include <QString>
#include <memory>

class QQuickWindow;
class QQuickRenderControl;
class QQuickRenderTarget;
class QRhi;
class QSGTexture;

namespace logos::surface {

struct SlotDescriptor {
    uint32_t slot = 0;
    uint32_t kind = SlotKindShm;
    QSize pixelSize;
    uint32_t stride = 0;
    uint64_t byteLength = 0;
    uint64_t nativeHandle = 0;
    QByteArray key;
    uint32_t format = FormatRGBA8Premultiplied;

    Attach toAttach() const
    {
        Attach a;
        a.slot = slot; a.kind = kind; a.pixelW = uint32_t(pixelSize.width());
        a.pixelH = uint32_t(pixelSize.height()); a.stride = stride; a.byteLength = byteLength;
        a.nativeHandle = nativeHandle; a.key = key;
        return a;
    }
    static SlotDescriptor fromAttach(const Attach& a, uint32_t format)
    {
        SlotDescriptor d;
        d.slot = a.slot; d.kind = a.kind; d.pixelSize = QSize(int(a.pixelW), int(a.pixelH));
        d.stride = a.stride; d.byteLength = a.byteLength; d.nativeHandle = a.nativeHandle;
        d.key = a.key; d.format = format;
        return d;
    }
};

// Checks RFC-7 §4 bounds for a requested slot geometry.
LOGOS_VIEW_SURFACE_EXPORT bool slotGeometryWithinBounds(const QSize& pixelSize);

// ── Host side ──────────────────────────────────────────────────────────────

class LOGOS_VIEW_SURFACE_EXPORT HostSlot {
public:
    virtual ~HostSlot() = default;

    // Allocates storage of exactly `pixelSize`. `peerPid` is the view's pid
    // (d3d11 duplicates the handle into it). `hostWindow` gives native-texture
    // kinds the scene graph device; may be null for shm.
    virtual bool allocate(uint32_t index, const QSize& pixelSize, uint32_t format,
                          qint64 peerPid, QQuickWindow* hostWindow) = 0;
    const SlotDescriptor& descriptor() const { return m_descriptor; }

    // Called on the host's render thread before sampling the slot; false
    // means "not readable right now" (d3d11 WAIT_TIMEOUT) and the frame is
    // skipped. Default: always readable.
    virtual bool acquireForRead() { return true; }
    // Called when the host has finished with the slot (RFC-7 §4 Displayed →
    // Free), before Release is sent.
    virtual void releaseAfterRead() {}
    // True when acquireForRead reported the storage abandoned by the peer;
    // the host then reallocates the slot.
    virtual bool isAbandoned() const { return false; }

    // Creates the scene-graph texture that samples this slot. Render thread.
    virtual QSGTexture* createTexture(QQuickWindow* window) = 0;

    // The slot's pixels for software/offscreen hosts and tests. Only shm
    // can answer; native kinds return a null image.
    virtual QImage image() const { return {}; }

    virtual QString transportName() const = 0;

protected:
    SlotDescriptor m_descriptor;
};

// ── View side ──────────────────────────────────────────────────────────────

class LOGOS_VIEW_SURFACE_EXPORT ViewSlot {
public:
    virtual ~ViewSlot() = default;

    // Opens the storage the host described. `rhi` is the producer's QRhi
    // (null under the software backend).
    virtual bool open(const SlotDescriptor& descriptor, QRhi* rhi) = 0;
    const SlotDescriptor& descriptor() const { return m_descriptor; }

    // The render target QQuickRenderControl renders into for this slot.
    // For shm this is a private QRhiTexture read back in finishRender().
    virtual QQuickRenderTarget renderTarget() = 0;

    // Around one frame. beginRender false → the frame is skipped (d3d11
    // AcquireSync timed out). finishRender completes GPU work and, for shm,
    // copies pixels into the shared mapping. Both on the producer's thread.
    virtual bool beginRender() { return true; }
    virtual bool finishRender(QRhi* rhi) = 0;

    // Software scene graph path (no QRhi): the slot takes the grabbed image.
    virtual bool storeSoftwareFrame(const QImage& frame) { Q_UNUSED(frame); return false; }

protected:
    SlotDescriptor m_descriptor;
};

// Factories. Return nullptr for a kind this build has no storage for.
LOGOS_VIEW_SURFACE_EXPORT std::unique_ptr<HostSlot> createHostSlot(uint32_t kind);
LOGOS_VIEW_SURFACE_EXPORT std::unique_ptr<ViewSlot> createViewSlot(uint32_t kind);

// The kinds this build can offer on each side (bitmask of SlotKind).
LOGOS_VIEW_SURFACE_EXPORT uint32_t hostSlotKinds();
LOGOS_VIEW_SURFACE_EXPORT uint32_t viewSlotKinds(uint32_t graphicsApi);

// RFC-6 §3 names.
LOGOS_VIEW_SURFACE_EXPORT QString transportNameForKind(uint32_t kind);

} // namespace logos::surface
