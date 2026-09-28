#pragma once
// ── rcx::PanelSearchField — the one filter box for every side panel ──
// Project, Bookmarks and Symbols each hand-rolled their own QLineEdit: three
// different paddings, two different icon sizes, one with a 1-logical-px rounded
// border (two device rows at 125 %) and one framed by a pair of separate
// HairlineSeparator widgets. This is the shared one: editor-paper ground, no
// box at rest, square corners, a 12-px tinted leading icon, a trailing clear
// action that only appears with text, and a single device-exact hairline
// underneath that turns borderFocused while the field has focus — the focus
// ring the dialog conventions ask for, without a box appearing at rest.
#include <QAction>
#include <QColor>
#include <QIcon>
#include <QLineEdit>
#include <QPainter>
#include <QRectF>
#include <QString>
#include <QToolButton>

#include "dock_header.h"   // rcx::chromeFont
#include "paintutil.h"
#include "svgicon.h"
#include "themes/theme.h"
#include "themes/thememanager.h"

namespace rcx {

class PanelSearchField : public QLineEdit {
public:
    static constexpr int kFieldHeight = 26;
    static constexpr int kLeadIconPx  = 12;

    PanelSearchField(const QString& leadIconPath, const QString& placeholder,
                     QWidget* parent = nullptr)
        : QLineEdit(parent), m_leadIconPath(leadIconPath) {
        setPlaceholderText(placeholder);
        setFixedHeight(kFieldHeight);
        // The field is chrome, so it wears the chrome face — not whatever the
        // app font happens to be. The three call sites used to each set (or
        // forget) this themselves, which is how the Project filter box silently
        // dropped out of JetBrains Mono when it moved onto this shared widget.
        setFont(chromeFont());
        m_lead = addAction(QIcon(), QLineEdit::LeadingPosition);
        m_clear = addAction(QIcon(QStringLiteral(":/vsicons/close.svg")),
                            QLineEdit::TrailingPosition);
        m_clear->setVisible(false);
        m_clear->setToolTip(QStringLiteral("Clear"));
        connect(m_clear, &QAction::triggered, this, &QLineEdit::clear);
        connect(this, &QLineEdit::textChanged, m_clear,
                [this](const QString& s) { m_clear->setVisible(!s.isEmpty()); });
        // The action buttons are created lazily by QLineEdit; size them once
        // they exist so the leading glyph is the spec'd 12 px, not Qt's 16.
        sizeActionButtons();
        applyTheme(ThemeManager::instance().current());
    }

    void applyTheme(const Theme& t) {
        // Re-read the chrome face: a font change writes the setting and then
        // re-themes, so this is the one place every panel field picks it up.
        setFont(chromeFont());
        const QColor paper = editorPaperColor(t);
        setStyleSheet(QStringLiteral(
            "QLineEdit { background: %1; color: %2; border: none;"
            " border-radius: 0px; padding: 0px 6px 0px 2px;"
            " selection-background-color: %4; }"
            "QLineEdit QToolButton { padding: 0px 6px; }"
            "QLineEdit QToolButton:hover { background: %3; }")
            .arg(paper.name(), t.textDim.name(), t.hover.name(),
                 t.selection.name()));
        m_lead->setIcon(themedVsIcon(m_leadIconPath, t.textDim, kLeadIconPx,
                                     devicePixelRatioF()));
        m_clear->setIcon(themedVsIcon(QStringLiteral(":/vsicons/close.svg"),
                                      t.textDim, kLeadIconPx, devicePixelRatioF()));
        sizeActionButtons();
        update();
    }

protected:
    void paintEvent(QPaintEvent* e) override {
        QLineEdit::paintEvent(e);
        const auto& t = ThemeManager::instance().current();
        QPainter p(this);
        fillBottomDeviceRowOfRect(p, QRectF(rect()),
                                  hasFocus() ? t.borderFocused
                                             : containerBorderColor(t));
    }
    void focusInEvent(QFocusEvent* e) override  { QLineEdit::focusInEvent(e);  update(); }
    void focusOutEvent(QFocusEvent* e) override { QLineEdit::focusOutEvent(e); update(); }

private:
    void sizeActionButtons() {
        for (auto* b : findChildren<QToolButton*>())
            b->setIconSize(QSize(kLeadIconPx, kLeadIconPx));
    }

    QString  m_leadIconPath;
    QAction* m_lead  = nullptr;
    QAction* m_clear = nullptr;
};

}  // namespace rcx
