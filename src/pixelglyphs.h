#pragma once
// 5×7 variable-width pixel bitmap font + the handful of fixed bitmaps the
// ribbon icons are
// composed from. Everything here paints in DEVICE pixels: callers hand us a
// painter attached to an unscaled QImage sized to the device cell and we
// fillRect whole `scale × scale` blocks with antialiasing off, so every ink
// pixel is a full device pixel at 100 / 125 / 150 / 200 %.
//
// The grid was 3×5 (shapes traced off the original ReClassEx toolbar bitmaps)
// until 2026-09-06, when the user called the result "not very sharp". It was
// sharp — every ink pixel landed on a whole device pixel — but 3 columns and 5
// rows cannot DRAW a legible digit: `6` read as `b`, `4` as `y`, `2` as `Z`,
// and `U` and `V` were the same glyph but for one row. 5×7 with a per-glyph
// advance fixes the letterforms without widening a single ribbon cell.

#include <QColor>
#include <QFont>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QPainter>
#include <QString>
#include <QStringView>
#include <QtGlobal>
#include <QtMath>
#include <cstdint>

namespace rcx {

struct PixelBitmap {
    int w;
    int h;
    const char* rows[8];
};

// Green "+" of the Add-N icons (top-left corner).
inline constexpr PixelBitmap kPlus5x5{5, 5, {
    "..#..",
    "..#..",
    "#####",
    "..#..",
    "..#.."}};

// The Insert-N arch arrow: up from the left, over, and down onto the row.
inline constexpr PixelBitmap kHookArrow7x7{7, 7, {
    ".###...",
    "#...#..",
    "#...#..",
    "....#..",
    "..#####",
    "...###.",
    "....#.."}};

// Delete ✕.
inline constexpr PixelBitmap kCross7x7{7, 7, {
    "#.....#",
    ".#...#.",
    "..#.#..",
    "...#...",
    "..#.#..",
    ".#...#.",
    "#.....#"}};

// Row of three byte "cells" above the fill label (000 / FFF / ???). Two rows
// + a 1-row gap + the 5-row label = 8 font rows, so the block is 8·s tall and
// fits a 16-device cell at 100 % (s = 2) — a 3-row band overflowed it.
inline constexpr PixelBitmap kSquaresRow11x2{11, 2, {
    "###.###.###",
    "###.###.###"}};
inline constexpr int kSquaresGap = 1;   // rows between the squares and the label

// The ▾ that marks a ribbon button which opens a menu instead of acting.
// Solid, not an outline: at k = 1 (100 / 125 %) an outlined triangle is three
// disconnected pixels.
inline constexpr PixelBitmap kChevronDown7x4{7, 4, {
    "#######",
    ".#####.",
    "..###..",
    "...#..."}};

// The red "*" that turns the class glyph into "pointer to class" (ReClassEx C*).
inline constexpr PixelBitmap kStar5x5{5, 5, {
    "#.#.#",
    ".###.",
    "..#..",
    ".###.",
    "#.#.#"}};

// Width in font pixels of a label at scale 1: the sum of the per-glyph
// advances plus one gap between each pair (variable width, so this is no
// longer a formula in the character count).
// ── Text: Departure Mono, a real pixel font ──
// Replaced a hand-drawn 5x7 bitmap table on 2026-09-07. That table was crisp
// but had to be drawn at 2x to be legible, so every "pixel" was a 2x2 block —
// the user's "blocky", and at that grid a 4 read as a 9 and U was a V.
// Departure Mono (Helena Zhang, SIL OFL 1.1, no Reserved Font Name; licence in
// src/fonts/DepartureMono-LICENSE.txt) is pixel-perfect at multiples of its
// 11 px design size, so at 11 DEVICE px it draws 1-device-pixel strokes: same
// physical size as the old 2x blocks, four times the detail, and ~25 % narrower.
inline constexpr int kPixelFontDesign = 11;   // do not render at non-multiples

// The font is drawn into the icon painters' DEVICE-resolution canvas, so the
// size is chosen in device px and antialiasing is off — the two things a pixel
// font needs to stay pixel-perfect at fractional DPI (a logical pixelSize at
// dpr 1.25 would land on 13.75 device px and blur).
inline int pixelFontSizeDev(qreal dpr) {
    return kPixelFontDesign * qMax(1, qRound(dpr * 0.75));   // 11 up to 150 %, 22 at 200 %
}

inline QFont pixelLabelFont(qreal dpr) {
    static const QString family = []() -> QString {
        const int id = QFontDatabase::addApplicationFont(
            QStringLiteral(":/fonts/DepartureMono.otf"));
        const QStringList fams = id >= 0 ? QFontDatabase::applicationFontFamilies(id)
                                         : QStringList();
        return fams.isEmpty() ? QStringLiteral("Courier New") : fams.first();
    }();
    QFont f(family);
    f.setPixelSize(pixelFontSizeDev(dpr));
    f.setStyleStrategy(QFont::NoAntialias);   // pixel font: never smooth it
    f.setHintingPreference(QFont::PreferNoHinting);
    return f;
}

// Advance width of `label` in DEVICE px at this dpr.
inline int pixelLabelWidthDev(QStringView label, qreal dpr) {
    return QFontMetrics(pixelLabelFont(dpr)).horizontalAdvance(label.toString());
}

// Ink height in DEVICE px — the cap box, used to centre a label in its cell.
inline int pixelLabelHeightDev(qreal dpr) {
    const QFontMetrics fm(pixelLabelFont(dpr));
    return fm.capHeight() > 0 ? int(fm.capHeight()) : fm.ascent();
}

// Logical width of the ribbon cell that holds a pixel label. THE one rule:
// ribbonIconCellWidth (layout) and the icon painters both call this.
//
// Sized from the DEVICE advance at the design size, unscaled. Layout is in
// logical px and must not depend on dpr, and the worst case (widest in logical
// terms) is dpr 1.0, where 1 device px IS 1 logical px. So this is exact at
// 100 % and leaves slack at every higher DPI.
inline int pixelLabelCellWidth(QStringView label) {
    return qMax(16, pixelLabelWidthDev(label, 1.0) + 6);
}

// Scale for the fixed BITMAPS above (the plus, the hook arrow, the rail
// chevron, the fill squares). Text no longer uses this — it is a real font
// now — but these shapes are still hand-drawn grids and want whole device
// pixels per grid cell: 2 up to 150 %, 3 at 200 %.
inline int pixelBitmapScale(qreal dpr) {
    return qMax(1, qRound(1.6 * dpr));
}

inline void drawBitmap(QPainter& p, int xDev, int yDev, const PixelBitmap& bm,
                       int scale, const QColor& ink) {
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setPen(Qt::NoPen);
    for (int r = 0; r < bm.h; ++r) {
        const char* row = bm.rows[r];
        for (int c = 0; c < bm.w; ++c)
            if (row[c] == '#')
                p.fillRect(xDev + c * scale, yDev + r * scale, scale, scale, ink);
    }
}

// Paints `label` (upper-cased) starting at device position (xDev, yDev).
// Paints `label` at DEVICE position (xDev, yDev) — yDev is the TOP of the cap
// box, matching how the old bitmap API was called.
//
// The text is rendered into a scratch image and its alpha HARD-THRESHOLDED
// before it is composited. Departure Mono ships as an OTF, i.e. CFF outlines,
// so Qt rasterises it through the normal vector path and antialiases the edges
// even with NoAntialias + TextAntialiasing off — both are hints the font engine
// may ignore. Measured: stray alpha 26 pixels along the stems. A pixel font
// with soft edges is just a small blurry font, which is the thing this change
// set out to fix, so the threshold is not optional polish — it is the feature.
// At the exact 11 px design size the outlines land on the grid, so thresholding
// snaps to the pixels the designer drew rather than inventing any.
inline void drawPixelLabel(QPainter& p, int xDev, int yDev, QStringView label,
                           qreal dpr, const QColor& ink) {
    const QFont f = pixelLabelFont(dpr);
    const QFontMetrics fm(f);
    const QString text = label.toString();
    const int w = qMax(1, fm.horizontalAdvance(text) + 2);
    const int h = qMax(1, fm.height() + 2);

    QImage mask(w, h, QImage::Format_ARGB32_Premultiplied);
    mask.fill(Qt::transparent);
    {
        QPainter mp(&mask);
        mp.setRenderHint(QPainter::Antialiasing, false);
        mp.setRenderHint(QPainter::TextAntialiasing, false);
        mp.setFont(f);
        mp.setPen(ink);
        mp.drawText(QPointF(1, 1 + fm.ascent()), text);
    }
    // Any pixel at least half covered becomes solid ink; the rest vanishes.
    const QRgb solid = qPremultiply(qRgba(ink.red(), ink.green(), ink.blue(), 255));
    for (int y = 0; y < h; ++y) {
        QRgb* row = reinterpret_cast<QRgb*>(mask.scanLine(y));
        for (int x = 0; x < w; ++x)
            row[x] = qAlpha(row[x]) >= 128 ? solid : 0u;
    }
    // Line the cap box up with what the caller asked for.
    p.drawImage(QPoint(xDev - 1, yDev + 1 - (fm.ascent() - pixelLabelHeightDev(dpr))), mask);
}

}  // namespace rcx
