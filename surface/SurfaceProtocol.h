#pragma once

// RFC-7 §3: the wire protocol of the `channel` SurfaceTransport.
//
// Every message is a 32-byte Header followed by `length` payload bytes. All
// integers are little-endian; doubles are IEEE binary64. Payload structs are
// fixed-layout and serialised field by field (encode/decode below), so no
// struct is ever memcpy'd off the wire and every length is checked against
// the header before a field is read. The only variable-length fields are
// `Attach.key`, `Key.text` and `Touch.points`, each preceded by its count.
//
// Both ends of the channel compile this header; nothing else does.

#include <QtGlobal>
#include <QByteArray>
#include <QChar>
#include <QList>
#include <QString>
#include <cstdint>
#include <cstring>

namespace logos::surface {

constexpr uint32_t kMagic = 0x3153564Cu;           // 'L' 'V' 'S' '1' (little-endian)
constexpr uint16_t kVersion = 1;
constexpr uint32_t kHeaderSize = 32;
constexpr uint32_t kMaxPayload = 64 * 1024;          // RFC-7 §4 bounds
constexpr uint32_t kMaxReadBuffer = 1024 * 1024;
constexpr uint32_t kSlotCount = 3;
constexpr uint32_t kMaxPixelDimension = 8192;
constexpr uint64_t kMaxSlotBytes = 256ull * 1024 * 1024;
constexpr int kHelloTimeoutMs = 10000;
constexpr uint32_t kTokenLength = 32;

constexpr uint32_t kFlagMustUnderstand = 0x1;

enum class MessageType : uint16_t {
    Hello = 1,
    Welcome = 2,
    KindRejected = 3,
    Configure = 4,
    Attach = 5,
    Detach = 6,
    FrameRequest = 7,
    FrameWanted = 8,
    FrameReady = 9,
    Release = 10,
    Input = 11,
    Bye = 12,
};

// Slot storage kinds, as a bitmask in Hello.slotKinds and a value elsewhere.
enum SlotKind : uint32_t {
    SlotKindNone = 0,
    SlotKindShm = 1,
    SlotKindD3D11 = 2,
    SlotKindIOSurface = 4,
};

// The producer's scene-graph API, so the host can tell which native kinds the
// view could render into directly (QSGRendererInterface::GraphicsApi values
// are not stable across Qt versions; these are ours).
enum GraphicsApi : uint32_t {
    ApiUnknown = 0,
    ApiSoftware = 1,
    ApiOpenGL = 2,
    ApiDirect3D11 = 3,
    ApiDirect3D12 = 4,
    ApiMetal = 5,
    ApiVulkan = 6,
};

enum PixelFormat : uint32_t {
    FormatRGBA8Premultiplied = 1,
    FormatBGRA8Premultiplied = 2,
};

enum ByeReason : uint32_t {
    ByeNormal = 0,
    ByeProtocolError = 1,
    ByeUnsupported = 2,
};

enum class InputKind : uint32_t {
    MouseMove = 1,
    MousePress = 2,
    MouseRelease = 3,
    MouseDoubleClick = 4,
    Wheel = 5,
    Touch = 6,
    KeyPress = 7,
    KeyRelease = 8,
    FocusIn = 9,
    FocusOut = 10,
    Leave = 11,
};

struct Header {
    uint32_t magic = kMagic;
    uint16_t version = kVersion;
    uint16_t type = 0;
    uint32_t length = 0;
    uint32_t generation = 0;
    uint32_t sequence = 0;
    uint32_t flags = 0;
    uint32_t reserved0 = 0;
    uint32_t reserved1 = 0;
};

// ── Serialisation primitives ───────────────────────────────────────────────

class Writer {
public:
    void u8(uint8_t v) { m_data.append(char(v)); }
    void u16(uint16_t v) { put(&v, 2); }
    void u32(uint32_t v) { put(&v, 4); }
    void i32(int32_t v) { put(&v, 4); }
    void u64(uint64_t v) { put(&v, 8); }
    void f64(double v) { put(&v, 8); }
    void bytes(const QByteArray& b) { m_data.append(b); }
    QByteArray take() { return std::move(m_data); }
    const QByteArray& data() const { return m_data; }

private:
    void put(const void* p, int n)
    {
        // Host byte order on every supported target is little-endian; the
        // swap below makes the format explicit should that ever change.
        uint8_t buf[8];
        std::memcpy(buf, p, size_t(n));
        if (Q_BYTE_ORDER == Q_BIG_ENDIAN) {
            for (int i = 0; i < n / 2; ++i) std::swap(buf[i], buf[n - 1 - i]);
        }
        m_data.append(reinterpret_cast<const char*>(buf), n);
    }
    QByteArray m_data;
};

class Reader {
public:
    explicit Reader(const QByteArray& d) : m_data(d) {}
    bool ok() const { return m_ok; }
    bool atEnd() const { return m_pos == m_data.size(); }
    int remaining() const { return m_data.size() - m_pos; }
    uint8_t u8() { uint8_t v = 0; get(&v, 1); return v; }
    uint16_t u16() { uint16_t v = 0; get(&v, 2); return v; }
    uint32_t u32() { uint32_t v = 0; get(&v, 4); return v; }
    int32_t i32() { int32_t v = 0; get(&v, 4); return v; }
    uint64_t u64() { uint64_t v = 0; get(&v, 8); return v; }
    double f64() { double v = 0; get(&v, 8); return v; }
    QByteArray bytes(int n)
    {
        if (n < 0 || n > remaining()) { m_ok = false; return {}; }
        QByteArray b = m_data.mid(m_pos, n);
        m_pos += n;
        return b;
    }

private:
    void get(void* p, int n)
    {
        if (!m_ok || n > remaining()) { m_ok = false; std::memset(p, 0, size_t(n)); return; }
        uint8_t buf[8];
        std::memcpy(buf, m_data.constData() + m_pos, size_t(n));
        if (Q_BYTE_ORDER == Q_BIG_ENDIAN) {
            for (int i = 0; i < n / 2; ++i) std::swap(buf[i], buf[n - 1 - i]);
        }
        std::memcpy(p, buf, size_t(n));
        m_pos += n;
    }
    QByteArray m_data;
    int m_pos = 0;
    bool m_ok = true;
};

inline QByteArray encodeHeader(const Header& h)
{
    Writer w;
    w.u32(h.magic); w.u16(h.version); w.u16(h.type); w.u32(h.length);
    w.u32(h.generation); w.u32(h.sequence); w.u32(h.flags);
    w.u32(h.reserved0); w.u32(h.reserved1);
    return w.take();
}

// Returns false when the 32 bytes are not a header this version accepts.
inline bool decodeHeader(const QByteArray& bytes, Header* h)
{
    if (bytes.size() < int(kHeaderSize)) return false;
    Reader r(bytes.left(int(kHeaderSize)));
    h->magic = r.u32(); h->version = r.u16(); h->type = r.u16(); h->length = r.u32();
    h->generation = r.u32(); h->sequence = r.u32(); h->flags = r.u32();
    h->reserved0 = r.u32(); h->reserved1 = r.u32();
    return r.ok() && h->magic == kMagic && h->version == kVersion && h->length <= kMaxPayload;
}

// ── Payloads ───────────────────────────────────────────────────────────────

struct Hello {
    uint64_t pid = 0;
    uint32_t slotKinds = SlotKindShm;
    uint32_t graphicsApi = ApiUnknown;
    QByteArray token;   // exactly kTokenLength bytes

    QByteArray encode() const
    {
        Writer w; w.u64(pid); w.u32(slotKinds); w.u32(graphicsApi);
        QByteArray t = token.left(int(kTokenLength));
        t.append(int(kTokenLength) - t.size(), '\0');
        w.bytes(t);
        return w.take();
    }
    static bool decode(const QByteArray& p, Hello* m)
    {
        Reader r(p); m->pid = r.u64(); m->slotKinds = r.u32(); m->graphicsApi = r.u32();
        m->token = r.bytes(int(kTokenLength));
        return r.ok() && r.atEnd();
    }
};

struct Welcome {
    uint32_t accepted = 0;
    uint32_t slotKind = SlotKindShm;
    uint32_t slotCount = kSlotCount;
    uint32_t maxPixelW = kMaxPixelDimension;
    uint32_t maxPixelH = kMaxPixelDimension;
    uint64_t hostDevice = 0;

    QByteArray encode() const
    {
        Writer w; w.u32(accepted); w.u32(slotKind); w.u32(slotCount);
        w.u32(maxPixelW); w.u32(maxPixelH); w.u64(hostDevice);
        return w.take();
    }
    static bool decode(const QByteArray& p, Welcome* m)
    {
        Reader r(p); m->accepted = r.u32(); m->slotKind = r.u32(); m->slotCount = r.u32();
        m->maxPixelW = r.u32(); m->maxPixelH = r.u32(); m->hostDevice = r.u64();
        return r.ok() && r.atEnd();
    }
};

struct KindRejected {
    uint32_t kind = 0;
    QByteArray encode() const { Writer w; w.u32(kind); return w.take(); }
    static bool decode(const QByteArray& p, KindRejected* m)
    { Reader r(p); m->kind = r.u32(); return r.ok() && r.atEnd(); }
};

struct Configure {
    uint32_t logicalW = 0, logicalH = 0;
    uint32_t pixelW = 0, pixelH = 0;
    double dpr = 1.0;
    uint32_t visible = 0;
    uint32_t format = FormatRGBA8Premultiplied;

    QByteArray encode() const
    {
        Writer w; w.u32(logicalW); w.u32(logicalH); w.u32(pixelW); w.u32(pixelH);
        w.f64(dpr); w.u32(visible); w.u32(format);
        return w.take();
    }
    static bool decode(const QByteArray& p, Configure* m)
    {
        Reader r(p); m->logicalW = r.u32(); m->logicalH = r.u32(); m->pixelW = r.u32();
        m->pixelH = r.u32(); m->dpr = r.f64(); m->visible = r.u32(); m->format = r.u32();
        return r.ok() && r.atEnd();
    }
};

struct Attach {
    uint32_t slot = 0;
    uint32_t kind = SlotKindShm;
    uint32_t pixelW = 0, pixelH = 0;
    uint32_t stride = 0;
    uint64_t byteLength = 0;
    uint64_t nativeHandle = 0;
    QByteArray key;

    QByteArray encode() const
    {
        Writer w; w.u32(slot); w.u32(kind); w.u32(pixelW); w.u32(pixelH); w.u32(stride);
        w.u64(byteLength); w.u64(nativeHandle); w.u32(uint32_t(key.size())); w.bytes(key);
        return w.take();
    }
    static bool decode(const QByteArray& p, Attach* m)
    {
        Reader r(p); m->slot = r.u32(); m->kind = r.u32(); m->pixelW = r.u32(); m->pixelH = r.u32();
        m->stride = r.u32(); m->byteLength = r.u64(); m->nativeHandle = r.u64();
        const uint32_t n = r.u32();
        if (!r.ok() || n > 4096) return false;
        m->key = r.bytes(int(n));
        return r.ok() && r.atEnd();
    }
};

struct SlotRef {   // Detach, Release
    uint32_t slot = 0;
    QByteArray encode() const { Writer w; w.u32(slot); return w.take(); }
    static bool decode(const QByteArray& p, SlotRef* m)
    { Reader r(p); m->slot = r.u32(); return r.ok() && r.atEnd(); }
};

struct FrameRequest {
    uint32_t slot = 0;
    uint32_t credit = 0;
    uint32_t deadlineUs = 16667;
    QByteArray encode() const { Writer w; w.u32(slot); w.u32(credit); w.u32(deadlineUs); return w.take(); }
    static bool decode(const QByteArray& p, FrameRequest* m)
    { Reader r(p); m->slot = r.u32(); m->credit = r.u32(); m->deadlineUs = r.u32(); return r.ok() && r.atEnd(); }
};

struct FrameReady {
    uint32_t slot = 0;
    uint32_t credit = 0;
    uint32_t dmgX = 0, dmgY = 0, dmgW = 0, dmgH = 0;
    uint64_t frameIndex = 0;
    QByteArray encode() const
    {
        Writer w; w.u32(slot); w.u32(credit); w.u32(dmgX); w.u32(dmgY); w.u32(dmgW); w.u32(dmgH);
        w.u64(frameIndex);
        return w.take();
    }
    static bool decode(const QByteArray& p, FrameReady* m)
    {
        Reader r(p); m->slot = r.u32(); m->credit = r.u32(); m->dmgX = r.u32(); m->dmgY = r.u32();
        m->dmgW = r.u32(); m->dmgH = r.u32(); m->frameIndex = r.u64();
        return r.ok() && r.atEnd();
    }
};

struct Bye {
    uint32_t reason = ByeNormal;
    QByteArray encode() const { Writer w; w.u32(reason); return w.take(); }
    static bool decode(const QByteArray& p, Bye* m)
    { Reader r(p); m->reason = r.u32(); return r.ok() && r.atEnd(); }
};

// ── Input (RFC-7 §6) ───────────────────────────────────────────────────────

struct MouseInput {           // MouseMove/Press/Release/DoubleClick
    double x = 0, y = 0, gx = 0, gy = 0;
    uint32_t button = 0, buttons = 0, modifiers = 0;
    uint64_t timestamp = 0;
};

struct WheelInput {
    double x = 0, y = 0, gx = 0, gy = 0;
    int32_t pixelDx = 0, pixelDy = 0, angleDx = 0, angleDy = 0;
    uint32_t buttons = 0, modifiers = 0, phase = 0, inverted = 0;
    uint64_t timestamp = 0;
};

struct TouchPointInput {
    int32_t id = 0;
    uint32_t state = 0;       // QEventPoint::State
    double x = 0, y = 0, gx = 0, gy = 0, pressure = 1.0;
};

struct TouchInput {
    uint32_t type = 0;        // QEvent::TouchBegin/Update/End/Cancel
    uint32_t modifiers = 0;
    uint64_t timestamp = 0;
    QList<TouchPointInput> points;   // bounded to 64
};

struct KeyInput {
    int32_t key = 0;
    uint32_t modifiers = 0, nativeScanCode = 0, nativeVirtualKey = 0, nativeModifiers = 0;
    uint32_t autoRepeat = 0;
    uint16_t count = 1;
    QString text;
};

constexpr int kMaxTouchPoints = 64;
constexpr int kMaxKeyText = 256;

struct Input {
    InputKind kind = InputKind::MouseMove;
    MouseInput mouse;
    WheelInput wheel;
    TouchInput touch;
    KeyInput key;

    QByteArray encode() const
    {
        Writer w; w.u32(uint32_t(kind));
        switch (kind) {
        case InputKind::MouseMove: case InputKind::MousePress:
        case InputKind::MouseRelease: case InputKind::MouseDoubleClick:
            w.f64(mouse.x); w.f64(mouse.y); w.f64(mouse.gx); w.f64(mouse.gy);
            w.u32(mouse.button); w.u32(mouse.buttons); w.u32(mouse.modifiers); w.u64(mouse.timestamp);
            break;
        case InputKind::Wheel:
            w.f64(wheel.x); w.f64(wheel.y); w.f64(wheel.gx); w.f64(wheel.gy);
            w.i32(wheel.pixelDx); w.i32(wheel.pixelDy); w.i32(wheel.angleDx); w.i32(wheel.angleDy);
            w.u32(wheel.buttons); w.u32(wheel.modifiers); w.u32(wheel.phase); w.u32(wheel.inverted);
            w.u64(wheel.timestamp);
            break;
        case InputKind::Touch: {
            w.u32(touch.type); w.u32(touch.modifiers); w.u64(touch.timestamp);
            const int n = qMin(int(touch.points.size()), kMaxTouchPoints);
            w.u32(uint32_t(n));
            for (int i = 0; i < n; ++i) {
                const TouchPointInput& p = touch.points.at(i);
                w.i32(p.id); w.u32(p.state); w.f64(p.x); w.f64(p.y); w.f64(p.gx); w.f64(p.gy); w.f64(p.pressure);
            }
            break;
        }
        case InputKind::KeyPress: case InputKind::KeyRelease: {
            w.i32(key.key); w.u32(key.modifiers); w.u32(key.nativeScanCode); w.u32(key.nativeVirtualKey);
            w.u32(key.nativeModifiers); w.u32(key.autoRepeat); w.u16(key.count);
            const QString t = key.text.left(kMaxKeyText);
            w.u16(uint16_t(t.size()));
            for (QChar c : t) w.u16(c.unicode());
            break;
        }
        case InputKind::FocusIn: case InputKind::FocusOut: case InputKind::Leave:
            break;
        }
        return w.take();
    }

    static bool decode(const QByteArray& p, Input* m)
    {
        Reader r(p);
        const uint32_t k = r.u32();
        if (!r.ok() || k < 1 || k > uint32_t(InputKind::Leave)) return false;
        m->kind = InputKind(k);
        switch (m->kind) {
        case InputKind::MouseMove: case InputKind::MousePress:
        case InputKind::MouseRelease: case InputKind::MouseDoubleClick:
            m->mouse.x = r.f64(); m->mouse.y = r.f64(); m->mouse.gx = r.f64(); m->mouse.gy = r.f64();
            m->mouse.button = r.u32(); m->mouse.buttons = r.u32(); m->mouse.modifiers = r.u32();
            m->mouse.timestamp = r.u64();
            break;
        case InputKind::Wheel:
            m->wheel.x = r.f64(); m->wheel.y = r.f64(); m->wheel.gx = r.f64(); m->wheel.gy = r.f64();
            m->wheel.pixelDx = r.i32(); m->wheel.pixelDy = r.i32(); m->wheel.angleDx = r.i32();
            m->wheel.angleDy = r.i32(); m->wheel.buttons = r.u32(); m->wheel.modifiers = r.u32();
            m->wheel.phase = r.u32(); m->wheel.inverted = r.u32(); m->wheel.timestamp = r.u64();
            break;
        case InputKind::Touch: {
            m->touch.type = r.u32(); m->touch.modifiers = r.u32(); m->touch.timestamp = r.u64();
            const uint32_t n = r.u32();
            if (!r.ok() || n > uint32_t(kMaxTouchPoints)) return false;
            m->touch.points.clear();
            for (uint32_t i = 0; i < n; ++i) {
                TouchPointInput tp;
                tp.id = r.i32(); tp.state = r.u32(); tp.x = r.f64(); tp.y = r.f64(); tp.gx = r.f64();
                tp.gy = r.f64(); tp.pressure = r.f64();
                m->touch.points.append(tp);
            }
            break;
        }
        case InputKind::KeyPress: case InputKind::KeyRelease: {
            m->key.key = r.i32(); m->key.modifiers = r.u32(); m->key.nativeScanCode = r.u32();
            m->key.nativeVirtualKey = r.u32(); m->key.nativeModifiers = r.u32(); m->key.autoRepeat = r.u32();
            m->key.count = r.u16();
            const uint16_t n = r.u16();
            if (!r.ok() || n > kMaxKeyText) return false;
            m->key.text.clear();
            for (uint16_t i = 0; i < n; ++i) m->key.text.append(QChar(r.u16()));
            break;
        }
        case InputKind::FocusIn: case InputKind::FocusOut: case InputKind::Leave:
            break;
        }
        return r.ok() && r.atEnd();
    }
};

} // namespace logos::surface
