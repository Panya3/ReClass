#pragma once
// ── The popup family's shared chrome ──
// The pieces every popup under the family rule (the comment above
// SourceChooserPopup::paintEvent, src/sourcechooserpopup.cpp) is built from,
// so the source chooser, the type chooser, the enum picker and the hex
// toolbar cannot drift apart again: the text field on the popup's own
// ground with its device-exact seam, the local 6-px scrollbar rule, and a
// scrollbar that carries the list's seams across its track. The frame
// itself is paintutil's fillDeviceFrameOfRect.
#include <QColor>
#include <QLineEdit>
#include <QPaintEvent>
#include <QPainter>
#include <QRectF>
#include <QScrollBar>
#include <QString>
#include <QVector>
#include <functional>

#include "paintutil.h"
#include "themes/theme.h"

namespace rcx {

// panelFieldInteriorQss's shape (src/widgets/panel_search_field.h) on the
// popup's ground instead of editor paper — the popup is ONE surface, and
// editorPaperColor is a shade off it on every dark theme. No box at rest,
// square corners, textDim ink, the theme's selection band; `leadPad` lands
// the first glyph where the popup's rows start (QLineEdit adds its own 2-px
// horizontal margin inside the padding). The trailing clear action, when a
// popup adds one, is the house 12-px tinted glyph on a t.hover hover.
inline QString popupFieldQss(const Theme& t, const QColor& ground, int leadPad = 2) {
    return QStringLiteral(
        "QLineEdit { background: %1; color: %2; border: none;"
        " border-radius: 0px; padding: 0px 6px 0px %3px;"
        " selection-background-color: %4; }"
        "QLineEdit QToolButton { padding: 0px 6px; }"
        "QLineEdit QToolButton:hover { background: %5; }")
        .arg(ground.name(), t.textDim.name()).arg(leadPad)
        .arg(t.selection.name(), t.hover.name());
}

// The field itself: the rule above plus ONE device-exact hairline
// underneath — containerBorderColor at rest, borderFocused while it has
// focus — never a second surface, never a QSS box. The seam is painted
// here and not by the popup because the field's QSS ground covers its
// whole rect: a row the parent painted first would be gone. A popup that
// seats something beside the field (the type chooser's close glyph) paints
// the same row across the rest of its width in seamColor().
class PopupFilterField : public QLineEdit {
public:
    using QLineEdit::QLineEdit;
    // PanelSearchField::kFieldHeight — the floor; a popup set in the editor
    // face grows it to fm.height() + 8.
    static constexpr int kMinHeight = 26;

    void applyTheme(const Theme& t, const QColor& ground, int leadPad = 2) {
        const QString qss = popupFieldQss(t, ground, leadPad);
        // Re-asserted per paint by a popup with no applyTheme of its own
        // (HexToolbarPopup): a no-op when nothing the field draws changed.
        if (m_themed && qss == styleSheet() && t.border == m_theme.border
            && t.background == m_theme.background && t.borderFocused == m_theme.borderFocused)
            return;
        m_theme  = t;
        m_themed = true;
        if (qss != styleSheet()) setStyleSheet(qss);
        update();
    }
    QColor seamColor() const {
        return hasFocus() ? m_theme.borderFocused : containerBorderColor(m_theme);
    }

protected:
    void paintEvent(QPaintEvent* e) override {
        QLineEdit::paintEvent(e);
        if (!m_themed) return;
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, false);
        fillBottomDeviceRowOfRect(p, QRectF(rect()), seamColor());
    }
    void focusInEvent(QFocusEvent* e) override  { QLineEdit::focusInEvent(e);  update(); }
    void focusOutEvent(QFocusEvent* e) override { QLineEdit::focusOutEvent(e); update(); }

private:
    Theme m_theme;
    bool  m_themed = false;
};

// The scrollbar, locally. The app-wide rule (MainWindow::applyGlobalTheme,
// src/main.cpp) is an 8-px solid textFaint bar on palette(window), which
// beside a popup's rows reads as a heavy rod down the whole track. Here the
// track is the surface by name, the handle textFaint at 45 % over it (an
// opaque blend, so a scan can name it), textDim under the mouse, 6 px.
inline QString popupScrollBarQss(const Theme& t, const QColor& surface) {
    const QColor handle = mixColor(surface, t.textFaint, 0.45);
    return QStringLiteral(
        "QScrollBar:vertical { background: %1; width: 6px; margin: 0; border: none; }"
        "QScrollBar::handle:vertical { background: %2; min-height: 20px; border: none; }"
        "QScrollBar::handle:vertical:hover { background: %3; }"
        "QScrollBar::add-line:vertical, QScrollBar::sub-line:vertical { height: 0; }"
        "QScrollBar::add-page:vertical, QScrollBar::sub-page:vertical { background: none; }")
        .arg(surface.name(), handle.name(), t.textDim.name());
}

// A list's vertical scrollbar that carries the rows' seams across its
// track. A section hairline or a divider the delegate paints ends at the
// viewport's edge, so while the list scrolled the 6-px track sat on those
// rows as a bare strip — 8 device px short of the right frame. The bar
// starts on the same device row as the viewport (both at the top of a
// frameless list), so a rect in viewport coordinates picks the same device
// row here as it did there; the popup hands over the seams it wants
// continued, and the bar repaints with every scroll.
class SeamScrollBar : public QScrollBar {
public:
    using QScrollBar::QScrollBar;
    // A seam: the top or bottom device row of `rect` (viewport-relative).
    struct Seam { QRectF rect; bool bottomEdge; QColor color; };
    std::function<QVector<Seam>()> seams;

protected:
    void paintEvent(QPaintEvent* e) override {
        QScrollBar::paintEvent(e);
        if (!seams) return;
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, false);
        for (const Seam& s : seams()) {
            const QRectF across(0, s.rect.top(), width(), s.rect.height());
            if (s.bottomEdge) fillBottomDeviceRowOfRect(p, across, s.color);
            else              fillTopDeviceRowOfRect(p, across, s.color);
        }
    }
};

} // namespace rcx
