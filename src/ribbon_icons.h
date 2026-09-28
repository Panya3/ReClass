#pragma once
// Ribbon icon rasterisers (header-only). Three rules keep everything crisp:
//
//  1. Glyph pixmaps are painted at DEVICE resolution into an unscaled
//     QImage(cellWDev, cellHDev) with AA off, converted to a QPixmap, and the
//     devicePixelRatio is stamped AFTER painting — the mirror image of the SVG
//     gotcha in svgicon.h. Every ink pixel is therefore a whole device pixel at
//     100 / 125 / 150 / 200 %.
//  2. Codicons are rendered only at integer multiples of their 16-unit grid
//     (ribbonSmallCodiconDev / ribbonLargeIconDev): a 16-unit path scaled
//     2.5× with AA reads as a blur next to the pixel bitmaps; at 16 / 32 / 48
//     device px its 1-unit strokes land on whole device pixels.
//  3. Every input that changes a pixel (kind, arg, cell size, dpr, ink, bg,
//     mirror, wide/square) is in the cache key, so the static cache never needs
//     invalidation; theme switches and monitor moves simply miss the cache once.
//
// Cells: 16 logical px tall; TypeGlyph / FillSquares cells are 8·max(2, len)
// wide (F = 16, H64 = 24, WSTR = 32) so every label shares ONE pixel scale
// (pixelGlyphScale). The `wide = false` path renders the same label into a
// square 16×16 cell (shrunk to fit) for QAction / QMenu icons, so the 24/32-
// wide ribbon cells are never downscaled into 16×16 action icons.
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

// State tone (textDim for rest labels / icons, textMuted for inactive tabs and
// captions) through the same 3 : 1 guard — mid.json / phosphor.json tones are
// near-invisible on their backgrounds, `text` is the fallback.
inline QColor ribbonToneColour(const QColor& tone, const Theme& t, const QColor& onBg) {
    if (tone.isValid() && wcagContrast(tone, onBg) >= kRibbonMinContrast) return tone;
    return t.text;
}

// ── Cell / Codicon sizing ──

// Logical width of an icon's 16-tall cell: pixel labels get 8·max(2, len)
// (F / H8 / V2 = 16, H64 / PTR / 000 = 24, WSTR / 1024 = 32); everything else 16.
inline int ribbonIconCellWidth(const RibbonIconSpec& spec) {
    using K = RibbonIconSpec::Kind;
    if (spec.kind == K::TypeGlyph || spec.kind == K::FillSquares)
        return 8 * qMax(2, int(spec.arg.size()));
    return 16;
}

// Small Codicon: 16·round(dpr) device px when that fits the cell (1.25 → 16
// device, centred in the 20-device cell: 1-device-px strokes like the k = 1
// bitmaps), else the whole cell with AA (1.5 → 24).
inline int ribbonSmallCodiconDev(int cellDev, qreal dpr) {
    const int grid = 16 * qMax(1, qRound(dpr));
    return grid <= cellDev ? grid : cellDev;
}

// Large Codicon: 16·max(2, round(1.5·dpr)) device px → 1.25: 32 (25.6
// logical), 1.5: 32, 2.0: 48; below 112.5 % a 24-device AA render keeps the
// familiar 24 px at 100 %.
inline int ribbonLargeIconDev(qreal dpr) {
    if (dpr < 1.125) return 24;
    return 16 * qMax(2, qRound(1.5 * dpr));
}

namespace detail {

inline QHash<QString, QPixmap>& ribbonIconCache() {
    static QHash<QString, QPixmap> cache;
    return cache;
}

inline QString ribbonIconKey(const QString& kind, const QString& arg, int logicalW, int logicalH,
                             qreal dpr, const QColor& ink, const QColor& bg, bool wide = true) {
    return kind + QLatin1Char('|') + arg
         + QLatin1Char('|') + QString::number(logicalW) + QLatin1Char('x') + QString::number(logicalH)
         + QLatin1Char('|') + QString::number(dpr, 'f', 3)
         + QLatin1Char('|') + ink.name(QColor::HexArgb)
         + QLatin1Char('|') + bg.name(QColor::HexArgb)
         + QLatin1Char('|') + (wide ? QLatin1Char('w') : QLatin1Char('s'));
}

inline int ribbonCellDev(int logicalSize, qreal dpr) {
    return qMax(1, qRound(logicalSize * dpr));
}

inline QSize ribbonCellDev(int logicalW, int logicalH, qreal dpr) {
    return QSize(ribbonCellDev(logicalW, dpr), ribbonCellDev(logicalH, dpr));
}

inline int ribbonUnit(qreal dpr) { return qMax(1, qRound(dpr)); }

// Fresh transparent device-sized canvas.
inline QImage ribbonCanvas(int wDev, int hDev) {
    QImage img(qMax(1, wDev), qMax(1, hDev), QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    return img;
}

inline QImage ribbonCanvas(const QSize& dev) { return ribbonCanvas(dev.width(), dev.height()); }

// Convert + stamp the dpr AFTER painting (rule 1).
inline QPixmap ribbonFinish(const QImage& img, qreal dpr) {
    QPixmap pm = QPixmap::fromImage(img);
    pm.setDevicePixelRatio(dpr > 0 ? dpr : 1.0);
    return pm;
}

// Renders a Codicon into a sideDev-square box centred in the canvas (AA on —
// arcs need it; at an integer multiple of the 16-unit grid the straight
// strokes are whole device pixels anyway), tints it with SourceIn and
// optionally mirrors it (redo = mirrored discard).
inline bool ribbonRenderTintedSvg(QImage& img, const QString& path, const QColor& tint,
                                  int sideDev, bool mirrorH = false) {
    QSvgRenderer r(path);
    if (!r.isValid()) return false;
    const int side = qBound(1, sideDev, qMin(img.width(), img.height()));
    const int x = (img.width() - side) / 2, y = (img.height() - side) / 2;
    {
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing, true);
        r.render(&p, QRectF(x, y, side, side));
        p.setCompositionMode(QPainter::CompositionMode_SourceIn);
        p.fillRect(img.rect(), tint);
    }
    if (mirrorH) img = img.mirrored(true, false);
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

// Pixel-font type label ("H64", "U32", "F", "WSTR") centred in its cell:
// 8·max(2, len) × logicalSize when `wide`, logicalSize square otherwise.
inline QPixmap typeGlyphIcon(const QString& label, GlyphFamily family, int logicalSize,
                             qreal dpr, const Theme& theme, bool wide = true) {
    const QColor ink = ribbonFamilyColour(family, theme, theme.background);
    const int cellW = wide ? 8 * qMax(2, int(label.size())) : logicalSize;
    const QString key = detail::ribbonIconKey(QStringLiteral("glyph"), label, cellW, logicalSize,
                                              dpr, ink, theme.background, wide);
    auto& cache = detail::ribbonIconCache();
    if (auto it = cache.constFind(key); it != cache.constEnd()) return it.value();

    const QSize cell = detail::ribbonCellDev(cellW, logicalSize, dpr);
    QImage img = detail::ribbonCanvas(cell);
    {
        QPainter p(&img);
        const int s = pixelLabelScale(label, cell.width(), cell.height(), dpr);
        const int w = pixelLabelWidth(label) * s;
        const int h = kGlyphH * s;
        drawPixelLabel(p, (cell.width() - w) / 2, (cell.height() - h) / 2, label, s, ink);
    }
    QPixmap pm = detail::ribbonFinish(img, dpr);
    cache.insert(key, pm);
    return pm;
}

// Green "+" top-left, byte count bottom-right (16-square cell).
inline QPixmap addBytesIcon(int n, int logicalSize, qreal dpr, const Theme& theme) {
    const QColor ink = ribbonFamilyColour(GlyphFamily::Unsigned, theme, theme.background);
    const QString key = detail::ribbonIconKey(QStringLiteral("add"), QString::number(n),
                                              logicalSize, logicalSize, dpr, ink, theme.background);
    auto& cache = detail::ribbonIconCache();
    if (auto it = cache.constFind(key); it != cache.constEnd()) return it.value();

    const int cell = detail::ribbonCellDev(logicalSize, dpr);
    const int k = detail::ribbonUnit(dpr);
    QImage img = detail::ribbonCanvas(cell, cell);
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

// Blue arch arrow top-left, byte count bottom-right (16-square cell).
inline QPixmap insertBytesIcon(int n, int logicalSize, qreal dpr, const Theme& theme) {
    const QColor ink = ribbonFamilyColour(GlyphFamily::Float, theme, theme.background);
    const QString key = detail::ribbonIconKey(QStringLiteral("insert"), QString::number(n),
                                              logicalSize, logicalSize, dpr, ink, theme.background);
    auto& cache = detail::ribbonIconCache();
    if (auto it = cache.constFind(key); it != cache.constEnd()) return it.value();

    const int cell = detail::ribbonCellDev(logicalSize, dpr);
    const int k = detail::ribbonUnit(dpr);
    QImage img = detail::ribbonCanvas(cell, cell);
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
                                              logicalSize, logicalSize, dpr, ink, theme.background);
    auto& cache = detail::ribbonIconCache();
    if (auto it = cache.constFind(key); it != cache.constEnd()) return it.value();

    const int cell = detail::ribbonCellDev(logicalSize, dpr);
    const int k = detail::ribbonUnit(dpr);
    QImage img = detail::ribbonCanvas(cell, cell);
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

// Row of byte squares above a 3-char label ("000" / "FFF" / "???") at the
// uniform glyph scale: 8·s-tall block (2 rows + gap + 5) in a 24-wide cell.
inline QPixmap fillIcon(const QString& text, GlyphFamily family, int logicalSize, qreal dpr,
                        const Theme& theme, bool wide = true) {
    const QColor ink = ribbonFamilyColour(family, theme, theme.background);
    const QColor squares = ribbonToneColour(theme.textDim, theme, theme.background);
    const int cellW = wide ? 8 * qMax(2, int(text.size())) : logicalSize;
    const QString key = detail::ribbonIconKey(QStringLiteral("fill"), text, cellW, logicalSize, dpr,
                                              ink, theme.background, wide)
                      + QLatin1Char('|') + squares.name(QColor::HexArgb);
    auto& cache = detail::ribbonIconCache();
    if (auto it = cache.constFind(key); it != cache.constEnd()) return it.value();

    const QSize cell = detail::ribbonCellDev(cellW, logicalSize, dpr);
    QImage img = detail::ribbonCanvas(cell);
    {
        QPainter p(&img);
        constexpr int kBlockRows = kSquaresRow11x2.h + kSquaresGap + kGlyphH;   // 8
        int s = pixelLabelScale(text, cell.width(), cell.height(), dpr);
        while (s > 1 && (kSquaresRow11x2.w * s > cell.width() || kBlockRows * s > cell.height())) --s;
        const int blockH = kBlockRows * s;
        const int y0 = (cell.height() - blockH) / 2;
        const int x0 = (cell.width() - kSquaresRow11x2.w * s) / 2;
        drawBitmap(p, x0, y0, kSquaresRow11x2, s, squares);
        const int lw = pixelLabelWidth(text) * s;
        drawPixelLabel(p, (cell.width() - lw) / 2, y0 + (kSquaresRow11x2.h + kSquaresGap) * s, text, s, ink);
    }
    QPixmap pm = detail::ribbonFinish(img, dpr);
    cache.insert(key, pm);
    return pm;
}

// Any Codicon tinted with SourceIn (works on baked-colour icons), small rule:
// 16·round(dpr) device px centred in the logicalSize cell when it fits, else
// the whole cell with AA.
inline QPixmap tintedSvgIcon(const QString& path, const QColor& tint, int logicalSize, qreal dpr,
                             bool mirrorH = false) {
    const QString key = detail::ribbonIconKey(QStringLiteral("svg"), path, logicalSize, logicalSize,
                                              dpr, tint, QColor(0, 0, 0, 0))
                      + (mirrorH ? QStringLiteral("|m") : QString());
    auto& cache = detail::ribbonIconCache();
    if (auto it = cache.constFind(key); it != cache.constEnd()) return it.value();

    const int cell = detail::ribbonCellDev(logicalSize, dpr);
    QImage img = detail::ribbonCanvas(cell, cell);
    detail::ribbonRenderTintedSvg(img, path, tint, ribbonSmallCodiconDev(cell, dpr), mirrorH);   // invalid path → blank (never crashes)
    QPixmap pm = detail::ribbonFinish(img, dpr);
    cache.insert(key, pm);
    return pm;
}

// Large-button Codicon: the canvas is exactly ribbonLargeIconDev(dpr) square
// (32 device at 125 / 150 %, 48 at 200 %, 24 at 100 %) — an integer multiple
// of the 16-unit grid, so strokes are whole device pixels. Logical size =
// side / dpr (fractional, e.g. 25.6 at 125 %); blit with drawPixmapSnapped.
inline QPixmap largeCodiconIcon(const QString& path, const QColor& tint, qreal dpr,
                                bool mirrorH = false) {
    const int side = ribbonLargeIconDev(dpr);
    const QString key = detail::ribbonIconKey(QStringLiteral("svgL"), path, side, side,
                                              dpr, tint, QColor(0, 0, 0, 0))
                      + (mirrorH ? QStringLiteral("|m") : QString());
    auto& cache = detail::ribbonIconCache();
    if (auto it = cache.constFind(key); it != cache.constEnd()) return it.value();

    QImage img = detail::ribbonCanvas(side, side);
    detail::ribbonRenderTintedSvg(img, path, tint, side, mirrorH);
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
                                              logicalSize, dpr, ink, theme.background)
                      + QLatin1Char('|') + star.name(QColor::HexArgb);
    auto& cache = detail::ribbonIconCache();
    if (auto it = cache.constFind(key); it != cache.constEnd()) return it.value();

    const int cell = detail::ribbonCellDev(logicalSize, dpr);
    const int k = detail::ribbonUnit(dpr);
    QImage img = detail::ribbonCanvas(cell, cell);
    detail::ribbonRenderTintedSvg(img, QStringLiteral(":/vsicons/symbol-class.svg"), ink,
                                  ribbonSmallCodiconDev(cell, dpr));
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

// ── Dispatcher ──

enum class RibbonIconSize { Small, Large };

struct RibbonIconOptions {
    bool   wide = true;   // per-kind cell width (ribbon buttons); false = 16-square (QAction / QMenu)
    QColor plainInk;      // state tone for GlyphFamily::Plain Codicons (rest / hover / checked);
                          // invalid = the family colour. Family-tinted icons ignore it.
};

inline QString ribbonCodiconPath(const QString& name) {
    return QStringLiteral(":/vsicons/") + name + QStringLiteral(".svg");
}

// Used by the ribbon paint path (Small = 16-tall cells, Large = the large
// Codicon rule) and by applyTheme's action icons (Small, wide = false).
inline QPixmap ribbonIcon(const RibbonIconSpec& spec, RibbonIconSize size, qreal dpr,
                          const Theme& theme, const RibbonIconOptions& o = {}) {
    using K = RibbonIconSpec::Kind;
    const int small = 16;
    const bool large = (size == RibbonIconSize::Large);
    switch (spec.kind) {
    case K::TypeGlyph:   return typeGlyphIcon(spec.arg, spec.family, small, dpr, theme, o.wide);
    case K::AddBytes:    return addBytesIcon(spec.arg.toInt(), small, dpr, theme);
    case K::InsertBytes: return insertBytesIcon(spec.arg.toInt(), small, dpr, theme);
    case K::DeleteCross: return deleteIcon(small, dpr, theme);
    case K::FillSquares: return fillIcon(spec.arg, spec.family, small, dpr, theme, o.wide);
    case K::ClassPtr:    return classPtrIcon(small, dpr, theme);
    case K::Codicon: {
        const QColor tint = (spec.family == GlyphFamily::Plain && o.plainInk.isValid())
            ? o.plainInk : ribbonFamilyColour(spec.family, theme, theme.background);
        const QString path = ribbonCodiconPath(spec.arg);
        return large ? largeCodiconIcon(path, tint, dpr, spec.mirrorH)
                     : tintedSvgIcon(path, tint, small, dpr, spec.mirrorH);
    }
    }
    return QPixmap();
}

// Back-compat shim for callers that pass a logical size (≤ 16 → Small).
inline QPixmap ribbonIcon(const RibbonIconSpec& spec, int logicalSize, qreal dpr,
                          const Theme& theme) {
    return ribbonIcon(spec, logicalSize > 16 ? RibbonIconSize::Large : RibbonIconSize::Small,
                      dpr, theme, RibbonIconOptions{});
}

}  // namespace rcx
