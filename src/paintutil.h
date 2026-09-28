#pragma once
#include <QPainter>
#include <QRectF>
#include <QTransform>
#include <QColor>
#include <QtMath>

namespace rcx {

// ── Shared chrome metrics ──
// The first ink of every horizontal strip (title label, ribbon tabs and
// panels, breadcrumb, pane tabs, status text, doc-tab icon pad) starts this
// many logical px from the strip's left edge so the left margin reads as ONE
// line down the window instead of five slightly different ones.
inline constexpr int kGutter = 8;

// One tab grammar: "current" is always this many DEVICE rows of the accent on
// the strip's bottom edge — ribbon tabs, checked ribbon items, document tabs
// and the pane view tabs. Painted through fillBottomDeviceRowsOfRect below.
inline constexpr int kUnderlineRows = 2;

// ── Device-pixel-exact edge fills ──
// A 1-logical-px fillRect covers 1.25 device px at DPR 1.25 and snaps to TWO
// filled rows/columns — a "1px" line next to a device-exact one (the editor
// paints its borders in device pixels) reads as a double line. These map the
// target rect through the painter's deviceTransform, pick the edge device
// row/column, and fill exactly that, at any DPR.
//
// Edge selection is half-a-pixel INWARD (qFloor(edge - 0.5), not
// qCeil(edge) - 1): Qt rounds the widget's system clip with qRound
// semantics, so at fractional DPR a .25-phase edge puts the outermost
// device row/col OUTSIDE the clip and the fill vanishes entirely
// (empirically: DPR 1.25, widget width 181 → no line at all). Picking the
// row/col that the rect overlaps by ≥ half a device px is identical at
// integer edges and stays inside the clip in every fractional phase.
// Regression-pinned by tests/test_hairline_dpr.cpp (real-window phase sweep
// at QT_SCALE_FACTOR=1.25).

inline void fillTopDeviceRowOfRect(QPainter& p, const QRectF& r, const QColor& c) {
    const QTransform dt = p.deviceTransform();
    const QRectF dev = dt.mapRect(r);
    const QRectF devRow(dev.left(), qFloor(dev.top() + 0.5), dev.width(), 1.0);
    p.fillRect(dt.inverted().mapRect(devRow), c);
}

inline void fillBottomDeviceRowOfRect(QPainter& p, const QRectF& r, const QColor& c) {
    const QTransform dt = p.deviceTransform();
    const QRectF dev = dt.mapRect(r);
    const QRectF devRow(dev.left(), qFloor(dev.bottom() - 0.5), dev.width(), 1.0);
    p.fillRect(dt.inverted().mapRect(devRow), c);
}

inline void fillLeftDeviceColOfRect(QPainter& p, const QRectF& r, const QColor& c) {
    const QTransform dt = p.deviceTransform();
    const QRectF dev = dt.mapRect(r);
    const QRectF devCol(qFloor(dev.left() + 0.5), dev.top(), 1.0, dev.height());
    p.fillRect(dt.inverted().mapRect(devCol), c);
}

inline void fillRightDeviceColOfRect(QPainter& p, const QRectF& r, const QColor& c) {
    const QTransform dt = p.deviceTransform();
    const QRectF dev = dt.mapRect(r);
    const QRectF devCol(qFloor(dev.right() - 0.5), dev.top(), 1.0, dev.height());
    p.fillRect(dt.inverted().mapRect(devCol), c);
}

// ── N-row edge fills (accent underlines) ──
// Fills the bottom / top `n` device rows of `r` in ONE inverse-mapped fill:
// rows [floor(devBottom - 0.5) - (n - 1) … floor(devBottom - 0.5)] (top:
// [floor(devTop + 0.5) … + n - 1]). Never two 1-row calls: at DPR 1.25 the
// second call's "1 logical px up" is 1.25 device px, so the pair skips a row
// in three of four phases. The last row is the same device row the 1-row
// helper picks, so a 2-row underline sits exactly on (and replaces) a 1-row
// hairline painted from the same rect. Regression-pinned by
// tests/test_hairline_dpr.cpp (2-row phase sweep).
inline void fillBottomDeviceRowsOfRect(QPainter& p, const QRectF& r, int n, const QColor& c) {
    if (n <= 0) return;
    const QTransform dt = p.deviceTransform();
    const QRectF dev = dt.mapRect(r);
    const qreal last = qFloor(dev.bottom() - 0.5);
    const QRectF devRows(dev.left(), last - (n - 1), dev.width(), qreal(n));
    p.fillRect(dt.inverted().mapRect(devRows), c);
}

inline void fillTopDeviceRowsOfRect(QPainter& p, const QRectF& r, int n, const QColor& c) {
    if (n <= 0) return;
    const QTransform dt = p.deviceTransform();
    const QRectF dev = dt.mapRect(r);
    const qreal first = qFloor(dev.top() + 0.5);
    const QRectF devRows(dev.left(), first, dev.width(), qreal(n));
    p.fillRect(dt.inverted().mapRect(devRows), c);
}

} // namespace rcx
