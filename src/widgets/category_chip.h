#pragma once

#include "themes/thememanager.h"
#include <QAbstractButton>
#include <QPainter>
#include <QFontMetrics>

namespace rcx {

// CategoryChip — flat custom-painted toggle button used as a filter pill.
// No CSS, no Fusion — pure QPainter with direct theme colors. Originally
// lived in typeselectorpopup.cpp; extracted here so the unified symbol
// panel can reuse the same visual language.
class CategoryChip : public QAbstractButton {
public:
    explicit CategoryChip(const QString& label, QWidget* parent = nullptr)
        : QAbstractButton(parent), m_label(label)
    {
        setCheckable(true);
        setChecked(true);
        setCursor(Qt::PointingHandCursor);
        setAttribute(Qt::WA_Hover, true);
        setMouseTracking(true);
    }

    void setCount(int n) { m_count = n; m_totalCount = -1; countChanged(); }
    void setCount(int visible, int total) { m_count = visible; m_totalCount = total; countChanged(); }
    // Narrow hosts (the ~270 px Symbols dock) drop the count from the label;
    // the full text stays in the tooltip.
    void setShowCount(bool on) { if (m_showCount == on) return; m_showCount = on; countChanged(); }
    void setGroupColor(const QColor& c) { m_groupColor = c; update(); }
//TODO-DELETE(CategoryChip::setLabel)     void setLabel(const QString& s) { m_label = s; update(); }

    QSize sizeHint() const override {
        QFontMetrics fm(font());
        QString text = chipText();
        return QSize(5 + 4 + fm.horizontalAdvance(text) + 16, fm.height() + 4);
    }

    // Chips may shrink below their text width (the text elides) but never
    // below pip + ellipsis, so a row of them can't overlap.
    QSize minimumSizeHint() const override {
        QFontMetrics fm(font());
        return QSize(5 + 4 + fm.horizontalAdvance(QStringLiteral("\u2026")) + 16, fm.height() + 4);
    }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, false);
        const auto& t = ThemeManager::instance().current();
        bool hov = underMouse();
        bool chk = isChecked();
        QColor gc = m_groupColor.isValid() ? m_groupColor : t.textMuted;

        if (hov) p.fillRect(rect(), t.hover);

        const int pipSz = 5;
        const int gap   = 4;
        QFontMetrics fm(font());
        // Elide instead of overflowing: a block wider than the widget used to
        // centre off both edges and read as overlapping neighbours.
        const int avail = qMax(0, width() - pipSz - gap - 16);
        const QString text = fm.elidedText(chipText(), Qt::ElideRight, avail);
        int textW  = fm.horizontalAdvance(text);
        int blockW = pipSz + gap + textW;
        int x      = qMax(8, (width() - blockW) / 2);
        int baseline = (height() + fm.ascent() - fm.descent()) / 2;

        p.fillRect(x, (height() - pipSz) / 2, pipSz, pipSz, chk ? gc : t.textFaint);
        x += pipSz + gap;

        p.setPen(chk ? gc : t.textMuted);
        p.setFont(font());
        p.drawText(x, baseline, text);
    }

    void enterEvent(QEnterEvent*) override { update(); }
    void leaveEvent(QEvent*) override { update(); }

private:
    void countChanged() {
        setToolTip(fullText());
        updateGeometry();
        update();
    }
    QString chipText() const { return m_showCount ? fullText() : m_label; }
    QString fullText() const {
        if (m_count < 0) return m_label;
        if (m_totalCount >= 0 && m_totalCount != m_count)
            return QStringLiteral("%1 (%2/%3)").arg(m_label).arg(m_count).arg(m_totalCount);
        return QStringLiteral("%1 (%2)").arg(m_label).arg(m_count);
    }

    QString m_label;
    int m_count = -1;
    int m_totalCount = -1;
    bool m_showCount = true;
    QColor m_groupColor;
};

} // namespace rcx
