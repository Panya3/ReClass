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
// (pixelBitmapScale). The `wide = false` path renders the same label into a
// square 16×16 cell for QAction / QMenu icons, so the 24/32-wide ribbon cells
// are never smooth-scaled into 16×16 action icons. That square is too narrow
// for the strip's font — it really is shrunk to fit now, at its own size
// (pixelFontSizeSquareDev, capped at the 3-character fit so the vocabulary
// stays in step); before 2026-09-08 it was drawn at the strip size and
// clipped instead, losing both edges of every 3-character label.
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
// Pixel-font ink is 1-device-px strokes at 100 %: it needs the body-text
// ratio, not the large-graphic one. Codicons (2-unit strokes on a 16 grid)
// keep 3.0.
inline constexpr double kRibbonPixelInkContrast = 4.5;

// Family → theme token with a contrast guard: the first candidate that reads
// at ≥ `minContrast` : 1 against `onBg` wins; `text` is the fallback.
//
// Four hues plus white. Signed AND Unsigned are both `syntaxNumber` — the
// I / U letter already carries the sign, and giving Int the red back freed
// `markerPtr` to mean exactly one thing on the strip: destructive.
inline QColor ribbonFamilyColour(GlyphFamily family, const Theme& t, const QColor& onBg,
                                 double minContrast = kRibbonMinContrast) {
    QVector<QColor> candidates;
    switch (family) {
    case GlyphFamily::Hex:      candidates = {t.text}; break;
    case GlyphFamily::Signed:   candidates = {t.syntaxNumber, t.indDataChanged}; break;
    case GlyphFamily::Unsigned: candidates = {t.syntaxNumber, t.indDataChanged}; break;
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
        if (c.isValid() && wcagContrast(c, onBg) >= minContrast) return c;
    return t.text;
}

// State tone as a LADDER, not a cliff: try the asked-for tone, then `textDim`,
// then `text`; the first that reads at ≥ 3 : 1 on `onBg` wins. Jumping straight
// to `text` made panel captions and inactive tabs render at full brightness on
// vs.json (whose `textMuted` misses the guard by 0.02) — the caption then
// shouted exactly as loudly as the labels it was captioning.
inline QColor ribbonToneColour(const QColor& tone, const Theme& t, const QColor& onBg) {
    if (tone.isValid() && wcagContrast(tone, onBg) >= kRibbonMinContrast) return tone;
    if (t.textDim.isValid() && wcagContrast(t.textDim, onBg) >= kRibbonMinContrast) return t.textDim;
    return t.text;
}

// ── Cell / Codicon sizing ──

// The pixel label an unlabelled Add / Insert cell carries: "+4" … "+2K" for
// Add (the sign is the verb), "4" … "2K" for Insert (the hook glyph is).
inline QString ribbonBytesGlyphLabel(int n, bool withPlus) {
    const QString num = (n >= 1024 && (n % 1024) == 0)
        ? QString::number(n / 1024) + QLatin1Char('K') : QString::number(n);
    return withPlus ? QLatin1Char('+') + num : num;
}

// Width of an unlabelled Add / Insert cell: it paints its own count ("+1K",
// "↳ 2K") in the pixel font, so it is sized from that text. THE one rule —
// ribbonIconCellWidth (layout) and both painters call this. They used to each
// hardcode 2 * logicalSize, which clipped the leading '+' the moment the font
// grew.
inline int bytesCellWidth(int n, bool add, int logicalSize, bool labelled) {
    if (labelled) return logicalSize;
    return qMax(2 * logicalSize,
                pixelLabelCellWidth(ribbonBytesGlyphLabel(n, add)) + (add ? 0 : 12));
}

// Logical width of an icon's 16-tall cell. Pixel labels are MEASURED, not
// counted: the 5×7 font is variable-width, so `V2` (V is 5 wide) needs more
// than `H8` even though both are two characters, and `PTR` needs less than
// `FN*`. Two logical px per font px is the worst case ratio of the uniform
// integer scale to the DPR (s/dpr = 2 at 100 %; 1.6 at 125 %, 1.33 at 150 %),
// so sizing at 2× guarantees the label never has to shrink out of step with
// its neighbours. The old 8·max(2, len) predated variable width and could not
// hold a 7-row glyph at 100 %.
// An UNLABELLED Add / Insert cell carries the byte count itself, so it widens
// to 32 — that is where the count went when the 4-5-device-px corner numbers
// (illegible at 100 %) were deleted.
inline int ribbonIconCellWidth(const RibbonIconSpec& spec, bool labelled = true) {
    using K = RibbonIconSpec::Kind;
    if (spec.kind == K::TypeGlyph || spec.kind == K::FillSquares)
        return pixelLabelCellWidth(spec.arg);
    if (spec.kind == K::AddBytes || spec.kind == K::InsertBytes)
        return bytesCellWidth(spec.arg.toInt(), spec.kind == K::AddBytes, 16, labelled);
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

// Largest whole multiple of a square bitmap that leaves a k margin in a
// cellDev-square cell (the Add "+" and the Delete "✕" share this rule).
inline int ribbonBitmapScale(int cellDev, int side, int k) {
    return qMax(1, (cellDev - 2 * k) / side);
}

}  // namespace detail

// ── Public rasterisers (all return a dpr-stamped QPixmap) ──

// Pixel-font type label ("H64", "U32", "F", "WSTR") centred in its cell:
// 8·max(2, len) × logicalSize when `wide`, logicalSize square otherwise.
inline QPixmap typeGlyphIcon(const QString& label, GlyphFamily family, int logicalSize,
                             qreal dpr, const Theme& theme, bool wide = true) {
    const QColor ink = ribbonFamilyColour(family, theme, theme.background, kRibbonPixelInkContrast);
    const int cellW = wide ? pixelLabelCellWidth(label) : logicalSize;
    const QString key = detail::ribbonIconKey(QStringLiteral("glyph"), label, cellW, logicalSize,
                                              dpr, ink, theme.background, wide);
    auto& cache = detail::ribbonIconCache();
    if (auto it = cache.constFind(key); it != cache.constEnd()) return it.value();

    const QSize cell = detail::ribbonCellDev(cellW, logicalSize, dpr);
    QImage img = detail::ribbonCanvas(cell);
    {
        QPainter p(&img);
        // The square QAction / QMenu cell is far narrower than the strip's, so
        // it has its own size; drawing the strip's glyphs in it cut both edges
        // off every 3-character label. See pixelFontSizeSquareDev.
        const int sz = wide ? pixelFontSizeDev(dpr)
                            : pixelFontSizeSquareDev(label, cell.width(), dpr);
        const int w = pixelLabelWidthDevAt(label, sz);
        const int h = pixelLabelHeightDevAt(sz);
        drawPixelLabelAt(p, (cell.width() - w) / 2, (cell.height() - h) / 2, label, sz, ink);
    }
    QPixmap pm = detail::ribbonFinish(img, dpr);
    cache.insert(key, pm);
    return pm;
}

// Add N. Labelled ("Add" + the count sit beside the icon): a bare, centred
// "+" in a 16-square cell. Unlabelled: the cell widens to 32 and the count
// becomes real pixel text ("+4" … "+2K") at the uniform glyph scale — the
// only form in which the number is actually readable.
inline QPixmap addBytesIcon(int n, int logicalSize, qreal dpr, const Theme& theme,
                            bool labelled = true, const QColor& inkOverride = QColor()) {
    const QColor ink = inkOverride.isValid()
        ? inkOverride
        : ribbonFamilyColour(GlyphFamily::Plain, theme, theme.background, kRibbonPixelInkContrast);
    const int cellW = bytesCellWidth(n, true, logicalSize, labelled);
    const QString key = detail::ribbonIconKey(QStringLiteral("add"), QString::number(n),
                                              cellW, logicalSize, dpr, ink, theme.background, labelled);
    auto& cache = detail::ribbonIconCache();
    if (auto it = cache.constFind(key); it != cache.constEnd()) return it.value();

    const QSize cell = detail::ribbonCellDev(cellW, logicalSize, dpr);
    const int k = detail::ribbonUnit(dpr);
    QImage img = detail::ribbonCanvas(cell);
    {
        QPainter p(&img);
        if (labelled) {
            const int s = detail::ribbonBitmapScale(qMin(cell.width(), cell.height()), kPlus5x5.w, k);
            const int side = kPlus5x5.w * s;
            drawBitmap(p, (cell.width() - side) / 2, (cell.height() - side) / 2, kPlus5x5, s, ink);
        } else {
            // RIGHT-ALIGNED, not centred. These sit in a column (+4 / +8 /
            // +64), and centring each one in its own cell made the shorter
            // counts wander left of the longer one. Right-aligning gives the
            // digits a shared edge — the same reason a column of numbers is
            // right-aligned anywhere else. `pad` keeps the block off the
            // cell's right edge by the same 3 px a labelled icon uses.
            const QString text = ribbonBytesGlyphLabel(n, true);
            const int pad = 3 * k;
            drawPixelLabel(p, cell.width() - pad - pixelLabelWidthDev(text, dpr),
                           (cell.height() - pixelLabelHeightDev(dpr)) / 2, text, dpr, ink);
        }
    }
    QPixmap pm = detail::ribbonFinish(img, dpr);
    cache.insert(key, pm);
    return pm;
}

// Insert N. Labelled: the bare arch arrow in a 16-square cell. Unlabelled:
// arrow + the count as pixel text side by side in the 32-wide cell (the arrow
// is what tells Insert apart from Add once both are wordless).
inline QPixmap insertBytesIcon(int n, int logicalSize, qreal dpr, const Theme& theme,
                               bool labelled = true, const QColor& inkOverride = QColor()) {
    const QColor ink = inkOverride.isValid()
        ? inkOverride
        : ribbonFamilyColour(GlyphFamily::Plain, theme, theme.background, kRibbonPixelInkContrast);
    const int cellW = bytesCellWidth(n, false, logicalSize, labelled);
    const QString key = detail::ribbonIconKey(QStringLiteral("insert"), QString::number(n),
                                              cellW, logicalSize, dpr, ink, theme.background, labelled);
    auto& cache = detail::ribbonIconCache();
    if (auto it = cache.constFind(key); it != cache.constEnd()) return it.value();

    const QSize cell = detail::ribbonCellDev(cellW, logicalSize, dpr);
    const int k = detail::ribbonUnit(dpr);
    QImage img = detail::ribbonCanvas(cell);
    {
        QPainter p(&img);
        const int arrowSide = kHookArrow7x7.w * k;
        if (labelled) {
            drawBitmap(p, (cell.width() - arrowSide) / 2, (cell.height() - arrowSide) / 2,
                       kHookArrow7x7, k, ink);
        } else {
            const QString text = ribbonBytesGlyphLabel(n, false);
            const int textW = pixelLabelWidthDev(text, dpr);
            const int blockW = arrowSide + 2 * k + textW;
            // Right-aligned like Add's, so the counts line up down the column
            // and the hook arrows stay a fixed gap to their left.
            const int x0 = cell.width() - 3 * k - blockW;
            drawBitmap(p, x0, (cell.height() - arrowSide) / 2, kHookArrow7x7, k, ink);
            drawPixelLabel(p, x0 + arrowSide + 2 * k,
                           (cell.height() - pixelLabelHeightDev(dpr)) / 2, text, dpr, ink);
        }
    }
    QPixmap pm = detail::ribbonFinish(img, dpr);
    cache.insert(key, pm);
    return pm;
}

// The 8-px ▾ that marks a button which opens a menu rather than acting.
// Painted in the item's state tone, in whole device pixels like the glyphs.
inline QPixmap ribbonMenuChevron(const QColor& ink, qreal dpr) {
    const QString key = detail::ribbonIconKey(QStringLiteral("menuchev"), QString(), 8, 8, dpr,
                                              ink, QColor(0, 0, 0, 0));
    auto& cache = detail::ribbonIconCache();
    if (auto it = cache.constFind(key); it != cache.constEnd()) return it.value();

    const int cell = detail::ribbonCellDev(8, dpr);
    const int k = detail::ribbonUnit(dpr);
    QImage img = detail::ribbonCanvas(cell, cell);
    {
        QPainter p(&img);
        const int w = kChevronDown7x4.w * k, h = kChevronDown7x4.h * k;
        drawBitmap(p, (cell - w) / 2, (cell - h) / 2, kChevronDown7x4, k, ink);
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
        const int s = detail::ribbonBitmapScale(cell, kCross7x7.w, k);
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
                        const Theme& theme, bool wide = true,
                        const QColor& inkOverride = QColor()) {
    // `inkOverride` is the item's state tone: a fill button is chrome, not a
    // type, so its squares and label follow rest / hover / disabled like every
    // other Plain icon instead of sitting at a fixed family colour.
    const QColor ink = inkOverride.isValid()
        ? inkOverride
        : ribbonFamilyColour(family, theme, theme.background, kRibbonPixelInkContrast);
    const QColor squares = inkOverride.isValid()
        ? inkOverride : ribbonToneColour(theme.textDim, theme, theme.background);
    const int cellW = wide ? pixelLabelCellWidth(text) : logicalSize;
    const QString key = detail::ribbonIconKey(QStringLiteral("fill"), text, cellW, logicalSize, dpr,
                                              ink, theme.background, wide)
                      + QLatin1Char('|') + squares.name(QColor::HexArgb);
    auto& cache = detail::ribbonIconCache();
    if (auto it = cache.constFind(key); it != cache.constEnd()) return it.value();

    const QSize cell = detail::ribbonCellDev(cellW, logicalSize, dpr);
    QImage img = detail::ribbonCanvas(cell);
    {
        QPainter p(&img);
        // The squares strip rides ABOVE the label only when the cell can hold
        // both at the shared scale. With the 5x7 font it cannot at 100 % (2 + 1
        // + 7 = 10 font px is 20 device in a 16-device cell), and shrinking the
        // composite alone would leave 000 / FFF / ??? half the size of every
        // neighbouring glyph — the exact inconsistency the one-scale rule
        // exists to prevent. The label alone is the information; the squares
        // are decoration, so the decoration is what gives way.
        const int sz = wide ? pixelFontSizeDev(dpr)
                            : pixelFontSizeSquareDev(text, cell.width(), dpr);
        const int glyphH = pixelLabelHeightDevAt(sz);
        const int s = detail::ribbonUnit(dpr);       // scale for the SQUARES bitmap
        const int kBlockRows = kSquaresRow11x2.h * s + kSquaresGap * s + glyphH;
        const bool withSquares = kSquaresRow11x2.w * s <= cell.width()
                              && kBlockRows <= cell.height();   // kBlockRows is already device px
        const int blockH = withSquares ? kBlockRows : glyphH;
        const int y0 = (cell.height() - blockH) / 2;
        if (withSquares) {
            const int x0 = (cell.width() - kSquaresRow11x2.w * s) / 2;
            drawBitmap(p, x0, y0, kSquaresRow11x2, s, squares);
        }
        const int lw = pixelLabelWidthDevAt(text, sz);
        drawPixelLabelAt(p, (cell.width() - lw) / 2,
                         y0 + (withSquares ? (kSquaresRow11x2.h + kSquaresGap) * s : 0), text, sz, ink);
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
    QColor plainInk;      // state tone for GlyphFamily::Plain icons (rest / hover / checked /
                          // destructive); invalid = the family colour. Family-tinted icons ignore it.
    bool   labelled = true;  // the button shows a text label — Add / Insert then draw the bare
                             // glyph; unlabelled they draw the byte count themselves (32-wide cell).
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
    // Plain icons take the item's state tone; family-tinted ones keep their hue.
    const QColor plain = (spec.family == GlyphFamily::Plain) ? o.plainInk : QColor();
    switch (spec.kind) {
    case K::TypeGlyph:   return typeGlyphIcon(spec.arg, spec.family, small, dpr, theme, o.wide);
    case K::AddBytes:    return addBytesIcon(spec.arg.toInt(), small, dpr, theme, o.labelled, plain);
    case K::InsertBytes: return insertBytesIcon(spec.arg.toInt(), small, dpr, theme, o.labelled, plain);
    case K::DeleteCross: return deleteIcon(small, dpr, theme);
    case K::FillSquares: return fillIcon(spec.arg, spec.family, small, dpr, theme, o.wide, plain);
    case K::ClassPtr:    return classPtrIcon(small, dpr, theme);
    case K::Codicon: {
        const QColor tint = plain.isValid()
            ? plain : ribbonFamilyColour(spec.family, theme, theme.background);
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
