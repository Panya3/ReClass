#pragma once
// 3×5 pixel bitmap font + the handful of fixed bitmaps the ribbon icons are
// composed from. Everything here paints in DEVICE pixels: callers hand us a
// painter attached to an unscaled QImage sized to the device cell and we
// fillRect whole `scale × scale` blocks with antialiasing off, so every ink
// pixel is a full device pixel at 100 / 125 / 150 / 200 %. The 3×5 letter
// shapes are lifted from the original ReClassEx toolbar bitmaps (H, I and the
// digits 0-8 are measured; the rest are designed in the same grid).

#include <QColor>
#include <QPainter>
#include <QString>
#include <QStringView>
#include <QtGlobal>
#include <QtMath>
#include <cstdint>

namespace rcx {

inline constexpr int kGlyphW   = 3;
inline constexpr int kGlyphH   = 5;
inline constexpr int kGlyphGap = 1;

namespace detail {
// Packs five 3-char rows ('#' = ink) into 15 bits, row-major, MSB = row 0 col 0.
constexpr uint16_t g3x5(const char* r0, const char* r1, const char* r2,
                        const char* r3, const char* r4) {
    const char* rows[5] = {r0, r1, r2, r3, r4};
    uint16_t v = 0;
    for (int r = 0; r < 5; ++r)
        for (int c = 0; c < 3; ++c) {
            v = static_cast<uint16_t>(v << 1);
            if (rows[r][c] == '#') v = static_cast<uint16_t>(v | 1);
        }
    return v;
}
}  // namespace detail

// Row-major 15-bit bitmap for an uppercase letter, digit or one of `* ? + - [ ] .`.
// Unknown characters (and space) return 0 — they occupy a cell but draw nothing.
constexpr uint16_t pixelGlyph3x5(char ch) {
    using detail::g3x5;
    switch (ch) {
    // ── digits (0-8 measured from the ReClassEx Hex/Int cells) ──
    case '0': return g3x5(".#.", "#.#", "#.#", "#.#", ".#.");
    case '1': return g3x5(".#.", "##.", ".#.", ".#.", "###");
    case '2': return g3x5("##.", "..#", ".#.", "#..", "###");
    case '3': return g3x5("###", "..#", ".#.", "..#", "###");
    case '4': return g3x5("..#", "#.#", "###", "..#", "..#");
    case '5': return g3x5("###", "#..", "##.", "..#", "##.");
    case '6': return g3x5(".#.", "#..", "##.", "#.#", ".#.");
    case '7': return g3x5("###", "..#", ".#.", ".#.", ".#.");
    case '8': return g3x5(".#.", "#.#", ".#.", "#.#", ".#.");
    case '9': return g3x5(".#.", "#.#", ".##", "..#", ".#.");
    // ── letters (H and I measured) ──
    case 'A': return g3x5(".#.", "#.#", "###", "#.#", "#.#");
    case 'B': return g3x5("##.", "#.#", "##.", "#.#", "##.");
    case 'C': return g3x5(".##", "#..", "#..", "#..", ".##");
    case 'D': return g3x5("##.", "#.#", "#.#", "#.#", "##.");
    case 'E': return g3x5("###", "#..", "##.", "#..", "###");
    case 'F': return g3x5("###", "#..", "##.", "#..", "#..");
    case 'G': return g3x5(".##", "#..", "#.#", "#.#", ".##");
    case 'H': return g3x5("#.#", "#.#", "###", "#.#", "#.#");
    case 'I': return g3x5("###", ".#.", ".#.", ".#.", "###");
    case 'J': return g3x5("..#", "..#", "..#", "#.#", ".#.");
    case 'K': return g3x5("#.#", "#.#", "##.", "#.#", "#.#");
    case 'L': return g3x5("#..", "#..", "#..", "#..", "###");
    case 'M': return g3x5("#.#", "###", "###", "#.#", "#.#");
    case 'N': return g3x5("##.", "#.#", "#.#", "#.#", "#.#");
    case 'O': return g3x5(".#.", "#.#", "#.#", "#.#", ".#.");
    case 'P': return g3x5("##.", "#.#", "##.", "#..", "#..");
    case 'Q': return g3x5(".#.", "#.#", "#.#", ".#.", "..#");
    case 'R': return g3x5("##.", "#.#", "##.", "#.#", "#.#");
    case 'S': return g3x5(".##", "#..", ".#.", "..#", "##.");
    case 'T': return g3x5("###", ".#.", ".#.", ".#.", ".#.");
    case 'U': return g3x5("#.#", "#.#", "#.#", "#.#", "###");
    case 'V': return g3x5("#.#", "#.#", "#.#", "#.#", ".#.");
    case 'W': return g3x5("#.#", "#.#", "#.#", "###", "#.#");
    case 'X': return g3x5("#.#", "#.#", ".#.", "#.#", "#.#");
    case 'Y': return g3x5("#.#", "#.#", ".#.", ".#.", ".#.");
    case 'Z': return g3x5("###", "..#", ".#.", "#..", "###");
    // ── punctuation ──
    case '*': return g3x5("#.#", ".#.", "#.#", "...", "...");  // superscript-style star
    case '?': return g3x5("##.", "..#", ".#.", "...", ".#.");
    case '+': return g3x5("...", ".#.", "###", ".#.", "...");
    case '-': return g3x5("...", "...", "###", "...", "...");
    case '[': return g3x5("##.", "#..", "#..", "#..", "##.");
    case ']': return g3x5(".##", "..#", "..#", "..#", ".##");
    case '.': return g3x5("...", "...", "...", "...", ".#.");
    default:  return 0;
    }
}

constexpr bool pixelGlyphBit(uint16_t glyph, int row, int col) {
    return ((glyph >> (14 - (row * kGlyphW + col))) & 1u) != 0;
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

// Width in font pixels of a label at scale 1: chars*3 + (chars-1) gaps.
inline int pixelLabelWidth(QStringView label) {
    const int n = int(label.size());
    return n <= 0 ? 0 : n * kGlyphW + (n - 1) * kGlyphGap;
}

// ONE scale per DPR for every label: s = ceil(1.6·dpr) → 1.0: 2, 1.25: 2,
// 1.5: 3, 2.0: 4 (ink 10 / 10 / 15 / 20 device px). "H64" and "F" are the
// same height in the same panel; the per-length seed (2× for 1–2 chars, 1×
// for longer) made 3-char labels half the size of their neighbours at 125 %.
inline int pixelGlyphScale(qreal dpr) {
    return qMax(1, qCeil(1.6 * dpr - 1e-6));
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
        const uint16_t g = pixelGlyph3x5(ch);
        for (int r = 0; r < kGlyphH; ++r)
            for (int c = 0; c < kGlyphW; ++c)
                if (pixelGlyphBit(g, r, c))
                    p.fillRect(x + c * scale, yDev + r * scale, scale, scale, ink);
        x += (kGlyphW + kGlyphGap) * scale;
    }
}

}  // namespace rcx
