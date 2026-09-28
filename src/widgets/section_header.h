#pragma once
// ── One SectionHeader spec, three consumers ──
// The Project tree's section rows (WorkspaceDelegate), the scanner's RESULTS
// label and its collapsible SCAN FOR toggle used to carry three near-identical
// but subtly different specs (0.67x vs -2pt, DemiBold vs Normal, letter-spacing
// 1.2 vs 1.5, a 0.5-width QPen line vs a QSS `border-bottom:1px`). At 125 %
// scaling the QSS border rendered as TWO device rows and the 0.5-px pen as a
// grey smear, so the same "section divider" read three different weights in one
// window. This is the single spec: 8 pt regular textDim, uppercase,
// letter-spacing 1.5, height fm.height() + 8, and one device-exact hairline in
// containerBorderColor underneath.
#include <QColor>
#include <QEvent>
#include <QFont>
#include <QFontMetrics>
#include <QLabel>
#include <QPainter>
#include <QPushButton>
#include <QRect>
#include <QRectF>
#include <QString>

#include "fontutil.h"
#include "paintutil.h"
#include "themes/theme.h"
#include "themes/thememanager.h"

namespace rcx {

// Derived through resolvedPointSize so a pixel-sized base font doesn't collapse
// the whole section-header family to the 8 pt floor (pixel-font pointSize bug
// class). Normal weight, NOT DemiBold: a section caption sits BELOW its content
// on the tone ladder, so it must not out-weigh the labels it introduces.
inline QFont sectionHeaderFont(const QFont& base) {
    QFont f = base;
    f.setPointSize(qMax(8, resolvedPointSize(base) - 2));
    f.setWeight(QFont::Normal);
    f.setCapitalization(QFont::AllUppercase);
    f.setLetterSpacing(QFont::AbsoluteSpacing, 1.5);
    return f;
}

inline int sectionHeaderHeight(const QFontMetrics& fm) { return fm.height() + 8; }

// Paints a whole section row: paper ground, caption at the shared kGutter, one
// device-exact hairline across the FULL row width. (The old workspace variant
// started the line after the text and stopped at the row's right inset, so two
// adjacent sections produced two lines of different lengths.)
inline void paintSectionHeaderRow(QPainter& p, const QRect& r,
                                  const QString& label, const Theme& t) {
    p.fillRect(r, editorPaperColor(t));
    const QFont f = sectionHeaderFont(p.font());
    const QFontMetrics fm(f);
    p.setFont(f);
    p.setPen(t.textDim);
    p.drawText(r.x() + kGutter,
               r.y() + (r.height() + fm.ascent() - fm.descent()) / 2, label);
    fillBottomDeviceRowOfRect(p, QRectF(r), containerBorderColor(t));
}

// The two widget flavours of the same divider. Both let their base class draw
// text/hover from the stylesheet and add ONLY the device-exact hairline, so the
// QSS no longer needs a `border-bottom` (which is what doubled at 125 %).
class SectionHeaderLabel : public QLabel {
public:
    using QLabel::QLabel;
protected:
    void paintEvent(QPaintEvent* e) override {
        QLabel::paintEvent(e);
        QPainter p(this);
        fillBottomDeviceRowOfRect(p, QRectF(rect()),
                                  containerBorderColor(ThemeManager::instance().current()));
    }
    // The caller sets sectionHeaderFont() on us; take the spec's height from
    // it too, so a widget header and a delegate-painted one are the same band.
    void changeEvent(QEvent* e) override {
        QLabel::changeEvent(e);
        if (e->type() == QEvent::FontChange)
            setFixedHeight(sectionHeaderHeight(fontMetrics()));
    }
};

class SectionHeaderButton : public QPushButton {
public:
    using QPushButton::QPushButton;
protected:
    void paintEvent(QPaintEvent* e) override {
        QPushButton::paintEvent(e);
        QPainter p(this);
        fillBottomDeviceRowOfRect(p, QRectF(rect()),
                                  containerBorderColor(ThemeManager::instance().current()));
    }
    void changeEvent(QEvent* e) override {
        QPushButton::changeEvent(e);
        if (e->type() == QEvent::FontChange)
            setFixedHeight(sectionHeaderHeight(fontMetrics()));
    }
};

}  // namespace rcx
