#pragma once
// Ribbon icon rasterisers (header-only). Two rules keep everything crisp:
//
//  1. Glyph pixmaps are painted at DEVICE resolution into an unscaled
//     QImage(cellDev, cellDev) with AA off, converted to a QPixmap, and the
//     devicePixelRatio is stamped AFTER painting — the mirror image of the SVG
//     gotcha in svgicon.h. Every ink pixel is therefore a whole device pixel at
//     100 / 125 / 150 / 200 %.
//  2. Every input that changes a pixel (kind, arg, size, dpr, ink, bg) is in the
//     cache key, so the static cache never needs invalidation; theme switches
//     and monitor moves simply miss the cache once.
//
// Codicons are tinted with a SourceIn fill (tab_source_icon.h technique) which
// works on baked-colour icons like symbol-class / debug-stop, unlike the
// "#C5C5C5" string replace in svgicon.h.

#include "pixelglyphs.h"
#include "ribbon_spec.h"
#include "themes/theme.h"

#include <QColor>
#include <QHash>
#include <QImage>
#include <QPainter>
#include <QPixmap>
#include <QString>
#include <QSvgRenderer>
#include <QtMath>
#include <cmath>

namespace rcx {

// ── WCAG contrast ──

inline double wcagLuminance(const QColor& c) {
    auto lin = [](double v) {
        return v <= 0.03928 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * lin(c.redF()) + 0.7152 * lin(c.greenF()) + 0.0722 * lin(c.blueF());
}

inline double wcagContrast(const QColor& a, const QColor& b) {
    const double la = wcagLuminance(a), lb = wcagLuminance(b);
    const double hi = qMax(la, lb), lo = qMin(la, lb);
    return (hi + 0.05) / (lo + 0.05);
}

inline constexpr double kRibbonMinContrast = 3.0;

// Family → theme token with a contrast guard: the first candidate that reads
// at ≥ 3.0 : 1 against `onBg` wins; `text` is the fallback for every family.
inline QColor ribbonFamilyColour(GlyphFamily family, const Theme& t, const QColor& onBg) {
    QVector<QColor> candidates;
    switch (family) {
    case GlyphFamily::Hex:      candidates = {t.text}; break;
    case GlyphFamily::Signed:   candidates = {t.markerPtr}; break;
    case GlyphFamily::Unsigned: candidates = {t.indHintGreen, t.indDataChanged, t.syntaxNumber}; break;
    case GlyphFamily::Float:    candidates = {t.syntaxKeyword, t.syntaxType}; break;
    case GlyphFamily::Text:     candidates = {t.syntaxString, t.syntaxPreproc}; break;
    case GlyphFamily::Pointer:
        if (t.syntaxType.isValid() && t.syntaxType != t.text) candidates.append(t.syntaxType);
        candidates.append(t.indHoverSpan);
        break;
    case GlyphFamily::Bits:     candidates = {t.syntaxNumber}; break;
    case GlyphFamily::Plain:    candidates = {t.text}; break;
    }
    for (const QColor& c : candidates)
        if (c.isValid() && wcagContrast(c, onBg) >= kRibbonMinContrast) return c;
    return t.text;
}

namespace detail {

inline QHash<QString, QPixmap>& ribbonIconCache() {
    static QHash<QString, QPixmap> cache;
    return cache;
}

inline QString ribbonIconKey(const QString& kind, const QString& arg, int logicalSize,
                             qreal dpr, const QColor& ink, const QColor& bg) {
    return kind + QLatin1Char('|') + arg + QLatin1Char('|') + QString::number(logicalSize)
         + QLatin1Char('|') + QString::number(dpr, 'f', 3)
         + QLatin1Char('|') + ink.name(QColor::HexArgb)
         + QLatin1Char('|') + bg.name(QColor::HexArgb);
}

inline int ribbonCellDev(int logicalSize, qreal dpr) {
    return qMax(1, qRound(logicalSize * dpr));
}

inline int ribbonUnit(qreal dpr) { return qMax(1, qRound(dpr)); }

// Fresh transparent device-sized canvas.
inline QImage ribbonCanvas(int cellDev) {
    QImage img(cellDev, cellDev, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    return img;
}

// Convert + stamp the dpr AFTER painting (rule 1).
inline QPixmap ribbonFinish(const QImage& img, qreal dpr) {
    QPixmap pm = QPixmap::fromImage(img);
    pm.setDevicePixelRatio(dpr > 0 ? dpr : 1.0);
    return pm;
}

// Renders a Codicon into the device-sized canvas and tints it with SourceIn.
// The painter is unscaled and the SVG is rendered into the full device rect,
// which is exactly what "set dpr before the painter" achieves for pixmaps.
inline bool ribbonRenderTintedSvg(QImage& img, const QString& path, const QColor& tint) {
    QSvgRenderer r(path);
    if (!r.isValid()) return false;
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    r.render(&p, QRectF(0, 0, img.width(), img.height()));
    p.setCompositionMode(QPainter::CompositionMode_SourceIn);
    p.fillRect(img.rect(), tint);
    p.end();
    return true;
}

// Number label bottom-right of a composite (Add-N / Insert-N): 1-char numbers
// start at the 2× scale, longer ones at k, shrunk until they fit the cell AND
// clear the top-left bitmap (whose device extent ends at clearRight /
// clearBottom) either horizontally or vertically. At 150 % (cell 24, k 2) a
// 1-digit number at 4× would otherwise sit on the plus bar / arrow head.
inline void ribbonDrawCornerNumber(QPainter& p, int cellDev, int k, const QString& num,
                                   const QColor& ink, int clearRight, int clearBottom) {
    const int w = pixelLabelWidth(num);
    int s = num.size() <= 1 ? 2 * k : k;
    auto placeX = [&](int sc) { return qMax(0, cellDev - k - w * sc); };
    auto placeY = [&](int sc) { return cellDev - k - kGlyphH * sc; };
    auto fits = [&](int sc) { return w * sc + k <= cellDev && kGlyphH * sc + 2 * k <= cellDev; };
    auto clears = [&](int sc) { return placeX(sc) >= clearRight || placeY(sc) >= clearBottom; };
    while (s > 1 && (!fits(s) || !clears(s))) --s;
    drawPixelLabel(p, placeX(s), placeY(s), num, s, ink);
}

}  // namespace detail

// ── Public rasterisers (all return a dpr-stamped QPixmap) ──

// Pixel-font type label ("H64", "U32", "F", "WSTR") centred in the cell.
inline QPixmap typeGlyphIcon(const QString& label, GlyphFamily family, int logicalSize,
                             qreal dpr, const Theme& theme) {
    const QColor ink = ribbonFamilyColour(family, theme, theme.background);
    const QString key = detail::ribbonIconKey(QStringLiteral("glyph"), label, logicalSize,
                                              dpr, ink, theme.background);
    auto& cache = detail::ribbonIconCache();
    if (auto it = cache.constFind(key); it != cache.constEnd()) return it.value();

    const int cell = detail::ribbonCellDev(logicalSize, dpr);
    QImage img = detail::ribbonCanvas(cell);
    {
        QPainter p(&img);
        const int s = pixelLabelScale(label, cell, dpr);
        const int w = pixelLabelWidth(label) * s;
        const int h = kGlyphH * s;
        drawPixelLabel(p, (cell - w) / 2, (cell - h) / 2, label, s, ink);
    }
    QPixmap pm = detail::ribbonFinish(img, dpr);
    cache.insert(key, pm);
    return pm;
}

// Green "+" top-left, byte count bottom-right.
inline QPixmap addBytesIcon(int n, int logicalSize, qreal dpr, const Theme& theme) {
    const QColor ink = ribbonFamilyColour(GlyphFamily::Unsigned, theme, theme.background);
    const QString key = detail::ribbonIconKey(QStringLiteral("add"), QString::number(n),
                                              logicalSize, dpr, ink, theme.background);
    auto& cache = detail::ribbonIconCache();
    if (auto it = cache.constFind(key); it != cache.constEnd()) return it.value();

    const int cell = detail::ribbonCellDev(logicalSize, dpr);
    const int k = detail::ribbonUnit(dpr);
    QImage img = detail::ribbonCanvas(cell);
    {
        QPainter p(&img);
        drawBitmap(p, k, k, kPlus5x5, k, ink);
        detail::ribbonDrawCornerNumber(p, cell, k, QString::number(n), ink,
                                       k + kPlus5x5.w * k, k + kPlus5x5.h * k);
    }
    QPixmap pm = detail::ribbonFinish(img, dpr);
    cache.insert(key, pm);
    return pm;
}

// Blue arch arrow top-left, byte count bottom-right.
inline QPixmap insertBytesIcon(int n, int logicalSize, qreal dpr, const Theme& theme) {
    const QColor ink = ribbonFamilyColour(GlyphFamily::Float, theme, theme.background);
    const QString key = detail::ribbonIconKey(QStringLiteral("insert"), QString::number(n),
                                              logicalSize, dpr, ink, theme.background);
    auto& cache = detail::ribbonIconCache();
    if (auto it = cache.constFind(key); it != cache.constEnd()) return it.value();

    const int cell = detail::ribbonCellDev(logicalSize, dpr);
    const int k = detail::ribbonUnit(dpr);
    QImage img = detail::ribbonCanvas(cell);
    {
        QPainter p(&img);
        drawBitmap(p, 0, k, kHookArrow7x7, k, ink);
        detail::ribbonDrawCornerNumber(p, cell, k, QString::number(n), ink,
                                       0 + kHookArrow7x7.w * k, k + kHookArrow7x7.h * k);
    }
    QPixmap pm = detail::ribbonFinish(img, dpr);
    cache.insert(key, pm);
    return pm;
}

// markerPtr ✕, scaled to the largest whole multiple of 7 that leaves a k margin.
inline QPixmap deleteIcon(int logicalSize, qreal dpr, const Theme& theme) {
    const QColor ink = theme.markerPtr;
    const QString key = detail::ribbonIconKey(QStringLiteral("delete"), QString(),
                                              logicalSize, dpr, ink, theme.background);
    auto& cache = detail::ribbonIconCache();
    if (auto it = cache.constFind(key); it != cache.constEnd()) return it.value();

    const int cell = detail::ribbonCellDev(logicalSize, dpr);
    const int k = detail::ribbonUnit(dpr);
    QImage img = detail::ribbonCanvas(cell);
    {
        QPainter p(&img);
        int s = qMax(1, (cell - 2 * k) / kCross7x7.w);
        const int side = kCross7x7.w * s;
        drawBitmap(p, (cell - side) / 2, (cell - side) / 2, kCross7x7, s, ink);
    }
    QPixmap pm = detail::ribbonFinish(img, dpr);
    cache.insert(key, pm);
    return pm;
}

// Row of byte squares above a 3-char label ("000" / "FFF" / "???").
inline QPixmap fillIcon(const QString& text, GlyphFamily family, int logicalSize, qreal dpr,
                        const Theme& theme) {
    const QColor ink = ribbonFamilyColour(family, theme, theme.background);
    const QColor squares = theme.textDim.isValid() ? theme.textDim : theme.text;
    const QString key = detail::ribbonIconKey(QStringLiteral("fill"), text, logicalSize, dpr,
                                              ink, theme.background)
                      + QLatin1Char('|') + squares.name(QColor::HexArgb);
    auto& cache = detail::ribbonIconCache();
    if (auto it = cache.constFind(key); it != cache.constEnd()) return it.value();

    const int cell = detail::ribbonCellDev(logicalSize, dpr);
    const int k = detail::ribbonUnit(dpr);
    QImage img = detail::ribbonCanvas(cell);
    {
        QPainter p(&img);
        int s = k;
        while (s > 1 && (kSquaresRow11x3.w * s > cell || (kSquaresRow11x3.h + 2 + kGlyphH) * s > cell)) --s;
        const int blockH = (kSquaresRow11x3.h + 2 + kGlyphH) * s;
        const int y0 = (cell - blockH) / 2;
        const int x0 = (cell - kSquaresRow11x3.w * s) / 2;
        drawBitmap(p, x0, y0, kSquaresRow11x3, s, squares);
        const int lw = pixelLabelWidth(text) * s;
        drawPixelLabel(p, (cell - lw) / 2, y0 + (kSquaresRow11x3.h + 2) * s, text, s, ink);
    }
    QPixmap pm = detail::ribbonFinish(img, dpr);
    cache.insert(key, pm);
    return pm;
}

// Any Codicon tinted with SourceIn (works on baked-colour icons).
inline QPixmap tintedSvgIcon(const QString& path, const QColor& tint, int logicalSize, qreal dpr) {
    const QString key = detail::ribbonIconKey(QStringLiteral("svg"), path, logicalSize, dpr,
                                              tint, QColor(0, 0, 0, 0));
    auto& cache = detail::ribbonIconCache();
    if (auto it = cache.constFind(key); it != cache.constEnd()) return it.value();

    const int cell = detail::ribbonCellDev(logicalSize, dpr);
    QImage img = detail::ribbonCanvas(cell);
    detail::ribbonRenderTintedSvg(img, path, tint);   // invalid path → blank (never crashes)
    QPixmap pm = detail::ribbonFinish(img, dpr);
    cache.insert(key, pm);
    return pm;
}

// symbol-class in the Pointer colour with a red "*" punched into the
// bottom-right corner — the ReClassEx "C*" idiom.
inline QPixmap classPtrIcon(int logicalSize, qreal dpr, const Theme& theme) {
    const QColor ink = ribbonFamilyColour(GlyphFamily::Pointer, theme, theme.background);
    const QColor star = theme.markerPtr;
    const QString key = detail::ribbonIconKey(QStringLiteral("classptr"), QString(), logicalSize,
                                              dpr, ink, theme.background)
                      + QLatin1Char('|') + star.name(QColor::HexArgb);
    auto& cache = detail::ribbonIconCache();
    if (auto it = cache.constFind(key); it != cache.constEnd()) return it.value();

    const int cell = detail::ribbonCellDev(logicalSize, dpr);
    const int k = detail::ribbonUnit(dpr);
    QImage img = detail::ribbonCanvas(cell);
    detail::ribbonRenderTintedSvg(img, QStringLiteral(":/vsicons/symbol-class.svg"), ink);
    {
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing, false);
        int s = k;
        const int side = kStar5x5.w * s;
        const int x = cell - side, y = cell - side;
        // Punch a hole so the star reads on top of the class glyph.
        p.setCompositionMode(QPainter::CompositionMode_Clear);
        p.fillRect(x - k, y - k, side + k, side + k, Qt::transparent);
        p.setCompositionMode(QPainter::CompositionMode_SourceOver);
        drawBitmap(p, x, y, kStar5x5, s, star);
    }
    QPixmap pm = detail::ribbonFinish(img, dpr);
    cache.insert(key, pm);
    return pm;
}

// Dispatcher used by the ribbon paint path and by applyTheme's action icons.
inline QPixmap ribbonIcon(const RibbonIconSpec& spec, int logicalSize, qreal dpr,
                          const Theme& theme) {
    using K = RibbonIconSpec::Kind;
    switch (spec.kind) {
    case K::TypeGlyph:   return typeGlyphIcon(spec.arg, spec.family, logicalSize, dpr, theme);
    case K::AddBytes:    return addBytesIcon(spec.arg.toInt(), logicalSize, dpr, theme);
    case K::InsertBytes: return insertBytesIcon(spec.arg.toInt(), logicalSize, dpr, theme);
    case K::DeleteCross: return deleteIcon(logicalSize, dpr, theme);
    case K::FillSquares: return fillIcon(spec.arg, spec.family, logicalSize, dpr, theme);
    case K::ClassPtr:    return classPtrIcon(logicalSize, dpr, theme);
    case K::Codicon: {
        const QColor tint = ribbonFamilyColour(spec.family, theme, theme.background);
        return tintedSvgIcon(QStringLiteral(":/vsicons/") + spec.arg + QStringLiteral(".svg"),
                             tint, logicalSize, dpr);
    }
    }
    return QPixmap();
}

}  // namespace rcx
