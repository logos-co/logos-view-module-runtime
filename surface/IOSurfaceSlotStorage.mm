// RFC-7 §4, kind `iosurface` (macOS): one IOSurface per slot, created by the
// host and handed to the view as a Mach send right. Each side wraps it in an
// MTLTexture on its own device: the view renders through
// QQuickRenderTarget::fromMetalTexture, the host samples through
// QNativeInterface::QSGMetalTexture::fromNative. Zero copies.
//
// Transfer: IOSurface global IDs are deprecated and world-readable, so the
// right travels over a Mach port. The host registers a bootstrap service
// named "co.logos.surface.<token>" (the RFC-2 token is the capability);
// the view looks it up once, receives the three IOSurface ports
// (IOSurfaceCreateMachPort / IOSurfaceLookupFromMachPort) and the host
// deregisters. The Attach `nativeHandle` is the slot's position in that
// exchange (0..2).
//
// Synchronisation: IOSurface has no cross-process fence. Metal orders the
// view's render before the surface is handed over because finishRender()
// waits for the command buffer (QRhi::finish()) before FrameReady is sent;
// the host reads only slots in Displayed state, which the view never touches.
//
// STATUS: written against the documented APIs (IOSurface.h, bootstrap.h,
// QRhiMetalNativeHandles, QSGMetalTexture, QQuickRenderTarget::
// fromMetalTexture) but NOT COMPILED OR RUN in the workspace it was authored
// in, which has no macOS build host. Expect to fix Mach message layout and
// ObjC/ARC details here first, then run tests/test_surface_channel with the
// kind forced to iosurface.

#include "SlotStorage.h"

#if defined(Q_OS_MACOS)

#include <QLoggingCategory>
#include <QQuickRenderTarget>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QtQuick/qsgtexture_platform.h>
#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>

#import <IOSurface/IOSurface.h>
#import <Metal/Metal.h>
#include <mach/mach.h>
#include <servers/bootstrap.h>
#include <thread>

Q_LOGGING_CATEGORY(lcSurfaceIOSurface, "logos.surface.iosurface")

namespace logos::surface {

static constexpr MTLPixelFormat kSlotFormat = MTLPixelFormatBGRA8Unorm;

// Mach message carrying one IOSurface send right.
struct SurfacePortMessage {
    mach_msg_header_t header;
    mach_msg_body_t body;
    mach_msg_port_descriptor_t port;
    uint32_t slot;
    mach_msg_trailer_t trailer;   // receive side only
};

static QByteArray serviceName(const QByteArray& token)
{
    return "co.logos.surface." + token;
}

// ── Host ───────────────────────────────────────────────────────────────────

class IOSurfaceHostSlot final : public HostSlot {
public:
    ~IOSurfaceHostSlot() override
    {
        if (m_texture) { [m_texture release]; m_texture = nil; }
        if (m_surface) { CFRelease(m_surface); m_surface = nullptr; }
    }

    bool allocate(uint32_t index, const QSize& pixelSize, uint32_t, qint64, QQuickWindow* hostWindow) override
    {
        if (!slotGeometryWithinBounds(pixelSize)) return false;
        if (!hostWindow || !hostWindow->rendererInterface()
            || hostWindow->rendererInterface()->graphicsApi() != QSGRendererInterface::Metal) {
            qCWarning(lcSurfaceIOSurface) << "host scene graph is not Metal";
            return false;
        }
        const size_t stride = IOSurfaceAlignProperty(kIOSurfaceBytesPerRow, size_t(pixelSize.width()) * 4);
        const size_t length = IOSurfaceAlignProperty(kIOSurfaceAllocSize, stride * size_t(pixelSize.height()));
        NSDictionary* props = @{
            (id)kIOSurfaceWidth : @(pixelSize.width()),
            (id)kIOSurfaceHeight : @(pixelSize.height()),
            (id)kIOSurfaceBytesPerElement : @4,
            (id)kIOSurfaceBytesPerRow : @(stride),
            (id)kIOSurfaceAllocSize : @(length),
            (id)kIOSurfacePixelFormat : @(uint32_t('BGRA')),
        };
        m_surface = IOSurfaceCreate((CFDictionaryRef)props);
        if (!m_surface) {
            qCWarning(lcSurfaceIOSurface) << "IOSurfaceCreate failed";
            return false;
        }
        m_descriptor.slot = index;
        m_descriptor.kind = SlotKindIOSurface;
        m_descriptor.pixelSize = pixelSize;
        m_descriptor.stride = uint32_t(stride);
        m_descriptor.byteLength = length;
        m_descriptor.nativeHandle = index;   // position in the port exchange
        m_descriptor.format = FormatBGRA8Premultiplied;
        return true;
    }

    // Used by the rendezvous (see iosurfaceBeginRendezvous below).
    IOSurfaceRef surface() const { return m_surface; }
    void setToken(const QByteArray& token) { m_descriptor.key = token; }

    QSGTexture* createTexture(QQuickWindow* window) override
    {
        if (!m_surface || !window) return nullptr;
        auto device = (id<MTLDevice>)window->rendererInterface()->getResource(window, QSGRendererInterface::DeviceResource);
        if (!device) return nullptr;
        MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:kSlotFormat
                                                                                     width:NSUInteger(m_descriptor.pixelSize.width())
                                                                                    height:NSUInteger(m_descriptor.pixelSize.height())
                                                                                 mipmapped:NO];
        d.usage = MTLTextureUsageShaderRead;
        d.storageMode = MTLStorageModeShared;
        if (m_texture) [m_texture release];
        m_texture = [device newTextureWithDescriptor:d iosurface:m_surface plane:0];
        if (!m_texture) return nullptr;
        return QNativeInterface::QSGMetalTexture::fromNative(m_texture, window, m_descriptor.pixelSize,
                                                             QQuickWindow::TextureHasAlphaChannel);
    }

    QString transportName() const override { return transportNameForKind(SlotKindIOSurface); }

private:
    IOSurfaceRef m_surface = nullptr;
    id<MTLTexture> m_texture = nil;
};

// Host side of the rendezvous, run on a helper thread so the GUI thread is
// never blocked on the view: register the service, wait for the view's
// request, answer with the three surface ports, deregister.
static void iosurfaceServeThread(QByteArray token, IOSurfaceRef surfaces[kSlotCount], int timeoutMs)
{
    mach_port_t service = MACH_PORT_NULL;
    kern_return_t kr = bootstrap_check_in(bootstrap_port, serviceName(token).constData(), &service);
    if (kr != KERN_SUCCESS) {
        qCWarning(lcSurfaceIOSurface) << "bootstrap_check_in failed:" << kr;
    } else {
        // Wait for the view's "give me the ports" message (its reply port).
        struct { mach_msg_header_t header; mach_msg_trailer_t trailer; } req{};
        kr = mach_msg(&req.header, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof(req), service,
                      mach_msg_timeout_t(timeoutMs), MACH_PORT_NULL);
        if (kr != KERN_SUCCESS) {
            qCWarning(lcSurfaceIOSurface) << "view did not rendezvous:" << kr;
        } else {
            const mach_port_t reply = req.header.msgh_remote_port;
            for (uint32_t i = 0; i < kSlotCount; ++i) {
                SurfacePortMessage msg{};
                msg.header.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0) | MACH_MSGH_BITS_COMPLEX;
                msg.header.msgh_size = offsetof(SurfacePortMessage, trailer);
                msg.header.msgh_remote_port = reply;
                msg.header.msgh_local_port = MACH_PORT_NULL;
                msg.header.msgh_id = 0x4C535631;   // 'LSV1'
                msg.body.msgh_descriptor_count = 1;
                msg.port.name = IOSurfaceCreateMachPort(surfaces[i]);
                msg.port.disposition = MACH_MSG_TYPE_MOVE_SEND;
                msg.port.type = MACH_MSG_PORT_DESCRIPTOR;
                msg.slot = i;
                kr = mach_msg(&msg.header, MACH_SEND_MSG | MACH_SEND_TIMEOUT, msg.header.msgh_size, 0,
                              MACH_PORT_NULL, mach_msg_timeout_t(timeoutMs), MACH_PORT_NULL);
                if (kr != KERN_SUCCESS) {
                    qCWarning(lcSurfaceIOSurface) << "sending surface port failed:" << kr;
                    break;
                }
            }
            mach_port_deallocate(mach_task_self(), reply);
        }
        mach_port_mod_refs(mach_task_self(), service, MACH_PORT_RIGHT_RECEIVE, -1);
    }
    for (auto& s : surfaces) if (s) CFRelease(s);
}

// Called by the host session after a ring of kind iosurface is allocated and
// before the Attach messages go out. Stamps the token into each descriptor
// key (the view uses it as the service name) and starts the serving thread.
void iosurfaceBeginRendezvous(const QByteArray& token, HostSlot* const slots[kSlotCount])
{
    IOSurfaceRef surfaces[kSlotCount] = {nullptr, nullptr, nullptr};
    for (uint32_t i = 0; i < kSlotCount; ++i) {
        auto* hs = dynamic_cast<IOSurfaceHostSlot*>(slots[i]);
        if (!hs || !hs->surface()) {
            for (auto& s : surfaces) if (s) CFRelease(s);
            return;
        }
        hs->setToken(token);
        surfaces[i] = (IOSurfaceRef)CFRetain(hs->surface());
    }
    std::thread([token, s0 = surfaces[0], s1 = surfaces[1], s2 = surfaces[2]]() mutable {
        IOSurfaceRef arr[kSlotCount] = {s0, s1, s2};
        iosurfaceServeThread(token, arr, kHelloTimeoutMs);
    }).detach();
}

// ── View ───────────────────────────────────────────────────────────────────

// View side of the rendezvous: performed once per generation by the first
// slot that opens; the surfaces are cached for the other two.
static IOSurfaceRef g_received[kSlotCount] = {nullptr, nullptr, nullptr};
static QByteArray g_receivedToken;

static bool iosurfaceReceivePorts(const QByteArray& token, int timeoutMs)
{
    if (g_receivedToken == token && g_received[0]) return true;
    for (auto& s : g_received) { if (s) CFRelease(s); s = nullptr; }
    g_receivedToken.clear();

    mach_port_t service = MACH_PORT_NULL;
    kern_return_t kr = bootstrap_look_up(bootstrap_port, serviceName(token).constData(), &service);
    if (kr != KERN_SUCCESS) {
        qCWarning(lcSurfaceIOSurface) << "bootstrap_look_up failed:" << kr;
        return false;
    }
    mach_port_t reply = MACH_PORT_NULL;
    mach_port_allocate(mach_task_self(), MACH_PORT_RIGHT_RECEIVE, &reply);
    mach_port_insert_right(mach_task_self(), reply, reply, MACH_MSG_TYPE_MAKE_SEND);

    mach_msg_header_t req{};
    req.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, MACH_MSG_TYPE_MAKE_SEND);
    req.msgh_size = sizeof(req);
    req.msgh_remote_port = service;
    req.msgh_local_port = reply;
    req.msgh_id = 0x4C535630;   // 'LSV0'
    kr = mach_msg(&req, MACH_SEND_MSG | MACH_SEND_TIMEOUT, req.msgh_size, 0, MACH_PORT_NULL,
                  mach_msg_timeout_t(timeoutMs), MACH_PORT_NULL);
    bool ok = kr == KERN_SUCCESS;
    for (uint32_t i = 0; i < kSlotCount && ok; ++i) {
        SurfacePortMessage msg{};
        kr = mach_msg(&msg.header, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof(msg), reply,
                      mach_msg_timeout_t(timeoutMs), MACH_PORT_NULL);
        ok = kr == KERN_SUCCESS && msg.slot < kSlotCount && (msg.header.msgh_bits & MACH_MSGH_BITS_COMPLEX);
        if (ok) {
            g_received[msg.slot] = IOSurfaceLookupFromMachPort(msg.port.name);
            mach_port_deallocate(mach_task_self(), msg.port.name);
            ok = g_received[msg.slot] != nullptr;
        }
    }
    mach_port_mod_refs(mach_task_self(), reply, MACH_PORT_RIGHT_RECEIVE, -1);
    mach_port_deallocate(mach_task_self(), service);
    if (ok) g_receivedToken = token;
    return ok;
}

// The token reaches the view slot through the descriptor key (the host
// session copies it there for this kind).
class IOSurfaceViewSlot final : public ViewSlot {
public:
    ~IOSurfaceViewSlot() override
    {
        if (m_texture) { [m_texture release]; m_texture = nil; }
    }

    bool open(const SlotDescriptor& descriptor, QRhi* rhi) override
    {
        if (descriptor.kind != SlotKindIOSurface || !rhi || rhi->backend() != QRhi::Metal) return false;
        if (descriptor.nativeHandle >= kSlotCount) return false;
        if (!iosurfaceReceivePorts(descriptor.key, 2000)) return false;
        IOSurfaceRef surface = g_received[descriptor.nativeHandle];
        if (!surface) return false;
        if (IOSurfaceGetWidth(surface) != size_t(descriptor.pixelSize.width())
            || IOSurfaceGetHeight(surface) != size_t(descriptor.pixelSize.height())) {
            qCWarning(lcSurfaceIOSurface) << "IOSurface does not match its description";
            return false;
        }
        const auto* nh = static_cast<const QRhiMetalNativeHandles*>(rhi->nativeHandles());
        if (!nh || !nh->dev) return false;
        auto device = (id<MTLDevice>)nh->dev;
        MTLTextureDescriptor* d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:kSlotFormat
                                                                                     width:NSUInteger(descriptor.pixelSize.width())
                                                                                    height:NSUInteger(descriptor.pixelSize.height())
                                                                                 mipmapped:NO];
        d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        d.storageMode = MTLStorageModeShared;
        m_texture = [device newTextureWithDescriptor:d iosurface:surface plane:0];
        if (!m_texture) return false;
        m_descriptor = descriptor;
        return true;
    }

    QQuickRenderTarget renderTarget() override
    {
        if (!m_texture) return {};
        return QQuickRenderTarget::fromMetalTexture(m_texture, uint(kSlotFormat), m_descriptor.pixelSize, 1);
    }

    bool finishRender(QRhi* rhi) override
    {
        // No cross-process fence on IOSurface: complete the GPU work before
        // FrameReady leaves this process.
        if (rhi) rhi->finish();
        return true;
    }

private:
    id<MTLTexture> m_texture = nil;
};

std::unique_ptr<HostSlot> createIOSurfaceHostSlot() { return std::make_unique<IOSurfaceHostSlot>(); }
std::unique_ptr<ViewSlot> createIOSurfaceViewSlot() { return std::make_unique<IOSurfaceViewSlot>(); }

} // namespace logos::surface

#endif // Q_OS_MACOS
