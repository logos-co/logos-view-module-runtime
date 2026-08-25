#pragma once

#include <limits>

#include <QString>
#include <QStringList>
#include <QVariant>
#include <QVariantList>
#include <QVariantMap>

// ── LogosIntent ──────────────────────────────────────────────────────────────
//
// The FROZEN vocabulary of the app-to-app intent surface: the six error codes,
// the intent-name grammar, the payload rules, and the result envelope.
//
// Header-only and not a QObject: no moc, no Qt Quick dependency. Both the
// runtime bridge and a host's broker include it, so the two can never disagree
// about what an error code is or what a legal name looks like.
//
namespace logos::intent {

// ── The six error codes ──────────────────────────────────────────────────────
//
// Who may mint each one is part of the contract:
//   not_declared, unavailable  — the BROKER only. A provider must never be able
//                                to claim either, because both leak whether a
//                                provider exists at all.
//   cancelled, timeout, failed — a provider may report these; the broker may
//                                also mint them.
inline QString errNotDeclared() { return QStringLiteral("not_declared"); }
inline QString errUnavailable() { return QStringLiteral("unavailable"); }
inline QString errCancelled()   { return QStringLiteral("cancelled"); }
inline QString errTimeout()     { return QStringLiteral("timeout"); }
inline QString errFailed()      { return QStringLiteral("failed"); }

// The payload was wrong, not the world. Either it could not cross an app
// boundary at all (see isCanonicalPayload) or the provider judged the values
// unusable. Distinct from `failed` because the caller can act on it: fix what
// you sent, rather than retry or give up.
//
// MINTABLE BY BOTH the broker and a provider, deliberately. If only the broker
// could mint it, receiving it would prove no provider was consulted — and that
// is an existence oracle. Both minting it makes the two indistinguishable.
inline QString errBadRequest()  { return QStringLiteral("bad_request"); }

inline QStringList allErrorCodes()
{
    return { errNotDeclared(), errUnavailable(), errBadRequest(),
             errCancelled(), errTimeout(), errFailed() };
}

inline bool isErrorCode(const QString& code)
{
    return allErrorCodes().contains(code);
}

// Coerce a PROVIDER-supplied failure into the closed set.
//
// A provider that answers ok:false with "no such key in keystore" must reach the
// requester as exactly "failed" — free text in the requester's error path is
// both an information leak and an un-switchable API. A provider that tries to
// claim "unavailable" or "not_declared" is likewise coerced, since those two
// carry meaning it is not entitled to assert.
//
// ok == true always yields an empty error.
inline QString normalizeError(bool ok, const QString& error)
{
    if (ok) return QString();
    if (error == errCancelled() || error == errTimeout() || error == errFailed()
        || error == errBadRequest())
        return error;
    return errFailed();
}

// ── Intent-name grammar ──────────────────────────────────────────────────────
//
//   intent  := segment ( "." segment ){1,3}        // 2..4 segments
//   segment := [a-z] [a-z0-9]* ( "_" [a-z0-9]+ )*
//   3 <= length <= 64
//
// Matching is byte-exact everywhere. There is no normalisation, no case folding
// and no Unicode: a name is a contract between two independently shipped apps,
// so "looks the same" is not good enough.
inline bool isValidSegment(const QString& seg)
{
    if (seg.isEmpty()) return false;

    const QChar first = seg.at(0);
    if (first < QLatin1Char('a') || first > QLatin1Char('z'))
        return false;   // rejects uppercase, digits and '_' in first position

    bool prevUnderscore = false;
    for (int i = 0; i < seg.size(); ++i) {
        const QChar c = seg.at(i);
        if (c == QLatin1Char('_')) {
            if (prevUnderscore) return false;          // "__"
            if (i == seg.size() - 1) return false;     // trailing "_"
            prevUnderscore = true;
            continue;
        }
        const bool lower = (c >= QLatin1Char('a') && c <= QLatin1Char('z'));
        const bool digit = (c >= QLatin1Char('0') && c <= QLatin1Char('9'));
        if (!lower && !digit) return false;            // '-', '.', space, non-ASCII
        prevUnderscore = false;
    }
    return true;
}

inline bool isValidName(const QString& name)
{
    if (name.size() < 3 || name.size() > 64) return false;

    const QStringList segments = name.split(QLatin1Char('.'));
    if (segments.size() < 2 || segments.size() > 4) return false;

    for (const QString& seg : segments)
        if (!isValidSegment(seg)) return false;

    return true;
}

inline bool isReservedName(const QString& name)
{
    return name.startsWith(QStringLiteral("logos."));
}

// ── Payload rules ────────────────────────────────────────────────────────────
//
// A payload crosses an app boundary and is re-rendered by code the sender does
// not control, so the type set is closed rather than "whatever QVariant holds".
// Everything outside it — QObject*, QJSValue, QDateTime, QUrl, uint, float — is
// refused rather than silently degraded.
//
// The bounds exist so a hostile or buggy requester cannot wedge the provider's
// UI thread with a pathological structure.
namespace detail {

// Running totals for one payload. Node count alone does not bound size: 999
// strings of 64 KB each is ~125 MiB of character data inside a "1000 node"
// payload, which defeats the point of having bounds at all.
struct PayloadBudget {
    int       nodes = 0;
    qsizetype chars = 0;   // UTF-16 units across every string AND every key
};

// 4 MiB of characters, across every string and key in one payload.
//
// DELIBERATELY FAR ABOVE ANY PLAUSIBLE CALLER. This bound is not solving a
// problem anyone has: nothing sends payloads near it, and an app that wanted to
// exhaust memory can do so without intents, since it runs QML in the shell's
// own process. What it stops is the ASYMMETRIC case — one app forcing another
// app's engine, and the broker's pending map, to materialise something the
// receiving side never agreed to.
//
// It exists NOW because the payload rules are frozen. Loosening a bound later
// is safe; introducing one later breaks every app already living within the old
// rules. So the last cheap moment to have any aggregate limit is before the
// freeze, and the safe way to take it is a ceiling generous enough that it
// cannot bite a legitimate caller.
//
// An earlier draft used 256 KiB. That silently encoded a design position —
// "intents carry references, not content" — which may well be right, but had
// not been argued and would have quietly foreclosed a future logos.share
// passing an image or a document. 4 MiB leaves that question open while still
// bounding the 125 MiB case that prompted the check.
constexpr qsizetype kMaxPayloadChars = 4 * 1024 * 1024;

inline bool checkPayloadValue(const QVariant& v, int depth, PayloadBudget& budget)
{
    if (depth > 8) return false;             // 8 levels of nesting, no more
    if (++budget.nodes > 1000) return false; // 1000 nodes total

    if (!v.isValid()) return true;           // an absent value is legal

    switch (v.typeId()) {
    case QMetaType::Bool:
        return true;

    case QMetaType::Int:
    case QMetaType::LongLong: {
        const qlonglong n = v.toLongLong();
        return n <= 9007199254740991LL && n >= -9007199254740991LL;
    }

    case QMetaType::Double: {
        const double d = v.toDouble();
        return d == d && d < std::numeric_limits<double>::infinity()
                      && d > -std::numeric_limits<double>::infinity();
    }

    case QMetaType::QString: {
        const qsizetype n = v.toString().size();
        if (n > 65536) return false;         // any single string
        budget.chars += n;
        return budget.chars <= kMaxPayloadChars;
    }

    case QMetaType::QVariantList: {
        const QVariantList list = v.toList();
        for (const QVariant& element : list)
            if (!checkPayloadValue(element, depth + 1, budget)) return false;
        return true;
    }

    case QMetaType::QVariantMap: {
        const QVariantMap map = v.toMap();
        for (auto it = map.cbegin(); it != map.cend(); ++it) {
            if (it.key().size() > 64) return false;
            budget.chars += it.key().size();          // keys count too
            if (budget.chars > kMaxPayloadChars) return false;
            if (!checkPayloadValue(it.value(), depth + 1, budget)) return false;
        }
        return true;
    }

    default:
        return false;
    }
}

} // namespace detail

inline bool isCanonicalPayload(const QVariantMap& params)
{
    detail::PayloadBudget budget;
    return detail::checkPayloadValue(QVariant(params), 1, budget);
}

inline bool isCanonicalValue(const QVariant& value)
{
    detail::PayloadBudget budget;
    return detail::checkPayloadValue(value, 1, budget);
}

// ── The result envelope ──────────────────────────────────────────────────────
//   { ok: true,  data: <anything>, error: ""      }
//   { ok: false, data: undefined,  error: "<code>" }
inline QVariantMap makeEnvelope(bool ok, const QVariant& data, const QString& error)
{
    QVariantMap envelope;
    envelope.insert(QStringLiteral("ok"), ok);
    envelope.insert(QStringLiteral("data"), ok ? data : QVariant());
    envelope.insert(QStringLiteral("error"), ok ? QString() : error);
    return envelope;
}

} // namespace logos::intent
