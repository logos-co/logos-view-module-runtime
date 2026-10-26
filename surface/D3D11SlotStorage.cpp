// RFC-7 §4, kind `d3d11` (Windows): a shared ID3D11Texture2D per slot, created
// on the host scene graph's device with D3D11_RESOURCE_MISC_SHARED_NTHANDLE |
// D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX, its NT handle duplicated into the view
// process. The view renders straight into it through
// QQuickRenderTarget::fromD3D11Texture; the host samples it through
// QNativeInterface::QSGD3D11Texture::fromNative. Zero copies.
//
// Keyed mutex protocol: key 0 = the view may render, key 1 = the host may
// read. The host only ever AcquireSync()s with a zero timeout.
//
// STATUS: written against the documented APIs (ID3D11Device1::
// OpenSharedResource1, IDXGIResource1::CreateSharedHandle, IDXGIKeyedMutex,
// QSGD3D11Texture, QQuickRenderTarget::fromD3D11Texture) but NOT COMPILED OR
// RUN in the workspace it was authored in, which has no Windows build host.
// The first Windows build should expect to fix include/COM details here and
// must run tests/test_surface_channel with the kind forced to d3d11.

#include "SlotStorage.h"

#if defined(Q_OS_WIN)

#include <QLoggingCategory>
#include <QQuickRenderTarget>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QtQuick/qsgtexture_platform.h>
#include <rhi/qrhi.h>
#include <rhi/qrhi_platform.h>

#include <d3d11_1.h>
#include <dxgi1_2.h>
#include <windows.h>
#include <wrl/client.h>

Q_LOGGING_CATEGORY(lcSurfaceD3D11, "logos.surface.d3d11")

namespace logos::surface {

using Microsoft::WRL::ComPtr;

static constexpr DXGI_FORMAT kSlotFormat = DXGI_FORMAT_B8G8R8A8_UNORM;

static uint64_t luidOf(ID3D11Device* device)
{
    if (!device) return 0;
    ComPtr<IDXGIDevice> dxgiDevice;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dxgiDevice)))) return 0;
    ComPtr<IDXGIAdapter> adapter;
    if (FAILED(dxgiDevice->GetAdapter(&adapter))) return 0;
    DXGI_ADAPTER_DESC desc{};
    if (FAILED(adapter->GetDesc(&desc))) return 0;
    return (uint64_t(uint32_t(desc.AdapterLuid.HighPart)) << 32) | uint64_t(desc.AdapterLuid.LowPart);
}

static ID3D11Device* hostDevice(QQuickWindow* window)
{
    if (!window || !window->rendererInterface()) return nullptr;
    if (window->rendererInterface()->graphicsApi() != QSGRendererInterface::Direct3D11) return nullptr;
    return static_cast<ID3D11Device*>(
        window->rendererInterface()->getResource(window, QSGRendererInterface::DeviceResource));
}

uint64_t d3d11HostDeviceLuid(QQuickWindow* window)
{
    return luidOf(hostDevice(window));
}

// ── Host ───────────────────────────────────────────────────────────────────

class D3D11HostSlot final : public HostSlot {
public:
    ~D3D11HostSlot() override
    {
        if (m_sharedHandle) CloseHandle(m_sharedHandle);
    }

    bool allocate(uint32_t index, const QSize& pixelSize, uint32_t, qint64 peerPid, QQuickWindow* hostWindow) override
    {
        if (!slotGeometryWithinBounds(pixelSize)) return false;
        ID3D11Device* device = hostDevice(hostWindow);
        if (!device) {
            qCWarning(lcSurfaceD3D11) << "host scene graph is not Direct3D 11";
            return false;
        }
        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = UINT(pixelSize.width());
        desc.Height = UINT(pixelSize.height());
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = kSlotFormat;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &m_texture))) {
            qCWarning(lcSurfaceD3D11) << "CreateTexture2D failed";
            return false;
        }
        if (FAILED(m_texture.As(&m_mutex))) return false;

        ComPtr<IDXGIResource1> resource;
        if (FAILED(m_texture.As(&resource))) return false;
        if (FAILED(resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                                nullptr, &m_sharedHandle))) {
            qCWarning(lcSurfaceD3D11) << "CreateSharedHandle failed";
            return false;
        }

        // The capability transfer: the duplicate lives in the peer's handle
        // table; its numeric value means nothing to anyone else (RFC-7 §4).
        HANDLE peer = OpenProcess(PROCESS_DUP_HANDLE, FALSE, DWORD(peerPid));
        if (!peer) {
            qCWarning(lcSurfaceD3D11) << "OpenProcess(PROCESS_DUP_HANDLE) failed for" << peerPid;
            return false;
        }
        HANDLE inPeer = nullptr;
        const BOOL ok = DuplicateHandle(GetCurrentProcess(), m_sharedHandle, peer, &inPeer, 0, FALSE,
                                        DUPLICATE_SAME_ACCESS);
        CloseHandle(peer);
        if (!ok) {
            qCWarning(lcSurfaceD3D11) << "DuplicateHandle failed";
            return false;
        }

        m_descriptor.slot = index;
        m_descriptor.kind = SlotKindD3D11;
        m_descriptor.pixelSize = pixelSize;
        m_descriptor.stride = uint32_t(pixelSize.width()) * 4u;
        m_descriptor.byteLength = uint64_t(m_descriptor.stride) * uint64_t(pixelSize.height());
        m_descriptor.nativeHandle = uint64_t(reinterpret_cast<uintptr_t>(inPeer));
        m_descriptor.format = FormatBGRA8Premultiplied;
        return true;
    }

    bool acquireForRead() override
    {
        if (!m_mutex) return false;
        const HRESULT hr = m_mutex->AcquireSync(1, 0);   // never a non-zero timeout
        if (hr == S_OK) { m_held = true; return true; }
        if (hr == WAIT_ABANDONED) { m_abandoned = true; return false; }
        return false;                                     // WAIT_TIMEOUT or error: skip
    }

    void releaseAfterRead() override
    {
        if (m_mutex && m_held) {
            m_mutex->ReleaseSync(0);
            m_held = false;
        }
    }

    bool isAbandoned() const override { return m_abandoned; }

    QSGTexture* createTexture(QQuickWindow* window) override
    {
        if (!m_texture || !window) return nullptr;
        return QNativeInterface::QSGD3D11Texture::fromNative(m_texture.Get(), window, m_descriptor.pixelSize,
                                                             QQuickWindow::TextureHasAlphaChannel);
    }

    QString transportName() const override { return transportNameForKind(SlotKindD3D11); }

private:
    ComPtr<ID3D11Texture2D> m_texture;
    ComPtr<IDXGIKeyedMutex> m_mutex;
    HANDLE m_sharedHandle = nullptr;
    bool m_held = false;
    bool m_abandoned = false;
};

// ── View ───────────────────────────────────────────────────────────────────

class D3D11ViewSlot final : public ViewSlot {
public:
    ~D3D11ViewSlot() override
    {
        if (m_handle) CloseHandle(m_handle);
    }

    bool open(const SlotDescriptor& descriptor, QRhi* rhi) override
    {
        if (descriptor.kind != SlotKindD3D11 || !rhi || rhi->backend() != QRhi::D3D11) return false;
        const auto* nh = static_cast<const QRhiD3D11NativeHandles*>(rhi->nativeHandles());
        if (!nh || !nh->dev) return false;
        auto* device = static_cast<ID3D11Device*>(nh->dev);
        ComPtr<ID3D11Device1> device1;
        if (FAILED(device->QueryInterface(IID_PPV_ARGS(&device1)))) return false;

        m_handle = reinterpret_cast<HANDLE>(uintptr_t(descriptor.nativeHandle));
        if (FAILED(device1->OpenSharedResource1(m_handle, IID_PPV_ARGS(&m_texture)))) {
            qCWarning(lcSurfaceD3D11) << "OpenSharedResource1 failed";
            m_handle = nullptr;
            return false;
        }
        D3D11_TEXTURE2D_DESC desc{};
        m_texture->GetDesc(&desc);
        if (desc.Width != UINT(descriptor.pixelSize.width()) || desc.Height != UINT(descriptor.pixelSize.height())
            || desc.Format != kSlotFormat || desc.SampleDesc.Count != 1) {
            qCWarning(lcSurfaceD3D11) << "shared texture does not match its description";
            m_texture.Reset();
            return false;
        }
        if (FAILED(m_texture.As(&m_mutex))) return false;
        m_descriptor = descriptor;
        return true;
    }

    QQuickRenderTarget renderTarget() override
    {
        if (!m_texture) return {};
        return QQuickRenderTarget::fromD3D11Texture(m_texture.Get(), uint(kSlotFormat), m_descriptor.pixelSize, 1);
    }

    bool beginRender() override
    {
        if (!m_mutex) return false;
        const HRESULT hr = m_mutex->AcquireSync(0, 2000);
        if (hr != S_OK) {
            qCWarning(lcSurfaceD3D11) << "AcquireSync(0) returned" << hr;
            return false;
        }
        return true;
    }

    bool finishRender(QRhi*) override
    {
        // The keyed mutex orders the GPU work: the host's AcquireSync(1)
        // succeeds only after the work issued before this release completes.
        return m_mutex && SUCCEEDED(m_mutex->ReleaseSync(1));
    }

private:
    ComPtr<ID3D11Texture2D> m_texture;
    ComPtr<IDXGIKeyedMutex> m_mutex;
    HANDLE m_handle = nullptr;
};

std::unique_ptr<HostSlot> createD3D11HostSlot() { return std::make_unique<D3D11HostSlot>(); }
std::unique_ptr<ViewSlot> createD3D11ViewSlot() { return std::make_unique<D3D11ViewSlot>(); }

} // namespace logos::surface

#endif // Q_OS_WIN
