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
#include <QPainter>
#include <QString>
#include <QStringView>
#include <QtGlobal>
#include <QtMath>
#include <cstdint>

namespace rcx {

inline constexpr int kGlyphMaxW = 5;   // storage grid; each glyph declares its own advance
inline constexpr int kGlyphH    = 7;
inline constexpr int kGlyphGap  = 1;

// One glyph: an advance width in font pixels (3, 4 or 5) plus 7 row masks,
// bit (kGlyphMaxW-1) = leftmost column. Variable width is what buys the
// legibility: digits and most caps get a proper 4-wide bowl, `I`/`T`/`Y` stay
// 3 so they do not float in a too-wide cell, and `M`/`W`/`V`/`*` take the full
// 5 they need — a 5-wide `V` with a real point can no longer be mistaken for a
// 4-wide `U`, which is exactly what the old 3x5 grid could not express.
struct PixelGlyph {
    uint8_t w = 0;
    uint8_t rows[kGlyphH] = {};

    constexpr bool operator==(const PixelGlyph& o) const {
        if (w != o.w) return false;
        for (int r = 0; r < kGlyphH; ++r)
            if (rows[r] != o.rows[r]) return false;
        return true;
    }
    constexpr bool operator!=(const PixelGlyph& o) const { return !(*this == o); }
};

namespace detail {
constexpr uint8_t packRow(const char* r) {
    uint8_t v = 0;
    for (int c = 0; c < kGlyphMaxW; ++c) {
        v = static_cast<uint8_t>(v << 1);
        if (r[c] == '#') v = static_cast<uint8_t>(v | 1);
    }
    return v;
}
// Rows are always written 5 chars wide; `w` says how many of them advance.
constexpr PixelGlyph g(int w, const char* r0, const char* r1, const char* r2,
                       const char* r3, const char* r4, const char* r5, const char* r6) {
    return PixelGlyph{static_cast<uint8_t>(w),
                      {packRow(r0), packRow(r1), packRow(r2), packRow(r3),
                       packRow(r4), packRow(r5), packRow(r6)}};
}
}  // namespace detail

// 5x7 variable-width bitmap font. Unknown characters and space return w = 0
// glyphs that advance a word space and draw nothing.
constexpr PixelGlyph pixelGlyph(char ch) {
    using detail::g;
    switch (ch) {
    // ── digits ──
    case '0': return g(4, ".##..", "#..#.", "#..#.", "#..#.", "#..#.", "#..#.", ".##..");
    case '1': return g(4, "..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###.");
    case '2': return g(4, ".##..", "#..#.", "...#.", "..#..", ".#...", "#....", "####.");
    case '3': return g(4, "###..", "...#.", "...#.", ".##..", "...#.", "#..#.", ".##..");
    case '4': return g(4, "..##.", ".#.#.", "#..#.", "####.", "...#.", "...#.", "...#.");
    case '5': return g(4, "####.", "#....", "###..", "...#.", "...#.", "#..#.", ".##..");
    case '6': return g(4, ".##..", "#..#.", "#....", "###..", "#..#.", "#..#.", ".##..");
    case '7': return g(4, "####.", "...#.", "...#.", "..#..", "..#..", ".#...", ".#...");
    case '8': return g(4, ".##..", "#..#.", "#..#.", ".##..", "#..#.", "#..#.", ".##..");
    case '9': return g(4, ".##..", "#..#.", "#..#.", ".###.", "...#.", "#..#.", ".##..");
    // ── letters ──
    case 'A': return g(4, ".##..", "#..#.", "#..#.", "####.", "#..#.", "#..#.", "#..#.");
    case 'B': return g(4, "###..", "#..#.", "#..#.", "###..", "#..#.", "#..#.", "###..");
    case 'C': return g(4, ".##..", "#..#.", "#....", "#....", "#....", "#..#.", ".##..");
    case 'D': return g(4, "###..", "#..#.", "#..#.", "#..#.", "#..#.", "#..#.", "###..");
    case 'E': return g(4, "####.", "#....", "#....", "###..", "#....", "#....", "####.");
    case 'F': return g(4, "####.", "#....", "#....", "###..", "#....", "#....", "#....");
    case 'G': return g(4, ".##..", "#..#.", "#....", "#.##.", "#..#.", "#..#.", ".###.");
    case 'H': return g(4, "#..#.", "#..#.", "#..#.", "####.", "#..#.", "#..#.", "#..#.");
    case 'I': return g(3, "###..", ".#...", ".#...", ".#...", ".#...", ".#...", "###..");
    case 'J': return g(4, "..##.", "...#.", "...#.", "...#.", "#..#.", "#..#.", ".##..");
    case 'K': return g(4, "#..#.", "#.#..", "##...", "##...", "#.#..", "#..#.", "#..#.");
    case 'L': return g(4, "#....", "#....", "#....", "#....", "#....", "#....", "####.");
    case 'M': return g(5, "#...#", "##.##", "#.#.#", "#.#.#", "#...#", "#...#", "#...#");
    case 'N': return g(4, "#..#.", "##.#.", "##.#.", "#.##.", "#.##.", "#..#.", "#..#.");
    case 'O': return g(4, ".##..", "#..#.", "#..#.", "#..#.", "#..#.", "#..#.", ".##..");
    case 'P': return g(4, "###..", "#..#.", "#..#.", "###..", "#....", "#....", "#....");
    case 'Q': return g(4, ".##..", "#..#.", "#..#.", "#..#.", "#.##.", "#..#.", ".###.");
    case 'R': return g(4, "###..", "#..#.", "#..#.", "###..", "##...", "#.#..", "#..#.");
    case 'S': return g(4, ".###.", "#....", "#....", ".##..", "...#.", "...#.", "###..");
    case 'T': return g(3, "###..", ".#...", ".#...", ".#...", ".#...", ".#...", ".#...");
    case 'U': return g(4, "#..#.", "#..#.", "#..#.", "#..#.", "#..#.", "#..#.", ".##..");
    case 'V': return g(5, "#...#", "#...#", "#...#", "#...#", ".#.#.", ".#.#.", "..#..");
    case 'W': return g(5, "#...#", "#...#", "#...#", "#.#.#", "#.#.#", "##.##", "#...#");
    case 'X': return g(4, "#..#.", "#..#.", ".##..", ".##..", ".##..", "#..#.", "#..#.");
    case 'Y': return g(3, "#.#..", "#.#..", "#.#..", ".#...", ".#...", ".#...", ".#...");
    case 'Z': return g(4, "####.", "...#.", "..#..", ".#...", "#....", "#....", "####.");
    // ── punctuation ──
    case '+': return g(5, ".....", "..#..", "..#..", "#####", "..#..", "..#..", ".....");
    case '-': return g(4, ".....", ".....", ".....", "####.", ".....", ".....", ".....");
    // Raised, not centred: `*` only ever marks a pointer (FN*), and a
    // full-height star read as a third letter rather than a modifier.
    case '*': return g(5, "#.#.#", ".###.", "#####", ".###.", "#.#.#", ".....", ".....");
    case '?': return g(4, ".##..", "#..#.", "...#.", "..#..", ".#...", ".....", ".#...");
    case '[': return g(3, "##...", "#....", "#....", "#....", "#....", "#....", "##...");
    case ']': return g(3, ".##..", "..#..", "..#..", "..#..", "..#..", "..#..", ".##..");
    case '.': return g(2, ".....", ".....", ".....", ".....", ".....", ".....", "#....");
    default:  return PixelGlyph{};
    }
}

// Advance of one character: its own width, or a 3-wide word space when the
// character is not in the table (space included).
constexpr int pixelGlyphAdvance(char ch) {
    const PixelGlyph g = pixelGlyph(ch);
    return g.w > 0 ? int(g.w) : 3;
}

constexpr bool pixelGlyphBit(const PixelGlyph& g, int row, int col) {
    return ((g.rows[row] >> (kGlyphMaxW - 1 - col)) & 1u) != 0;
}

// Fixed bitmaps ('#' = ink), drawn with drawBitmap(). Height ≤ 8 rows.
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
inline int pixelLabelWidth(QStringView label) {
    const int n = int(label.size());
    if (n <= 0) return 0;
    int w = (n - 1) * kGlyphGap;
    for (int i = 0; i < n; ++i) {
        const QChar qc = label.at(i).toUpper();
        w += pixelGlyphAdvance(qc.unicode() < 128 ? char(qc.unicode()) : '?');
    }
    return w;
}

// ONE scale per DPR for every label: s = round(1.6·dpr) → 1.0: 2, 1.25: 2,
// 1.5: 2, 2.0: 3 (ink 14 / 14 / 14 / 21 device px). "H64" and "F" are the same
// height in the same panel; the per-length seed (2× for 1–2 chars, 1× for
// longer) made 3-char labels half the size of their neighbours at 125 %.
// ROUND, not ceil: with the 7-row font ceil pushed 150 % to s = 3 (a 21-px
// glyph beside a 24-px Codicon) and 200 % to s = 4, and the extra scale has to
// be paid for in cell width at every DPR. Round keeps s/dpr ≤ 2 everywhere.
// Logical width of the ribbon cell that holds a pixel label. Two logical px
// per font px is the worst case ratio of the uniform integer scale to the DPR
// (s/dpr = 2 at 100 %; 1.6 at 125 %, 1.33 at 150 %, 1.5 at 200 %), so sizing at
// 2x guarantees pixelLabelScale's shrink guard never fires and every label in a
// panel keeps the same scale. THE one rule: ribbonIconCellWidth (layout) and
// typeGlyphIcon / fillSquaresIcon (painting) all call this, because when they
// each carried their own copy the painter drew into a cell 2 px narrower than
// the layout reserved and silently halved the glyph.
inline int pixelLabelCellWidth(QStringView label) {
    return qMax(16, 2 * pixelLabelWidth(label));
}

inline int pixelGlyphScale(qreal dpr) {
    return qMax(1, qRound(1.6 * dpr));
}

// The uniform scale, shrunk until both the label width and the 5-row height
// fit a cellWDev × cellHDev cell (the guard never fires for the ribbon's
// 8·max(2, len)-wide cells; it does for the 16×16 QMenu icons). Never below 1.
inline int pixelLabelScale(QStringView label, int cellWDev, int cellHDev, qreal dpr) {
    const int w = pixelLabelWidth(label);
    int s = pixelGlyphScale(dpr);
    while (s > 1 && (w * s > cellWDev || kGlyphH * s > cellHDev)) --s;
    return qMax(1, s);
}

inline int pixelLabelScale(QStringView label, int cellDev, qreal dpr) {
    return pixelLabelScale(label, cellDev, cellDev, dpr);
}

// Paints one bitmap at device position (xDev, yDev), each set bit as an s×s block.
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
inline void drawPixelLabel(QPainter& p, int xDev, int yDev, QStringView label,
                           int scale, const QColor& ink) {
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setPen(Qt::NoPen);
    int x = xDev;
    for (int i = 0; i < label.size(); ++i) {
        const QChar qc = label.at(i).toUpper();
        const char ch = qc.unicode() < 128 ? char(qc.unicode()) : '?';
        const PixelGlyph g = pixelGlyph(ch);
        for (int r = 0; r < kGlyphH; ++r)
            for (int c = 0; c < kGlyphMaxW; ++c)
                if (pixelGlyphBit(g, r, c))
                    p.fillRect(x + c * scale, yDev + r * scale, scale, scale, ink);
        x += (pixelGlyphAdvance(ch) + kGlyphGap) * scale;
    }
}

}  // namespace rcx
