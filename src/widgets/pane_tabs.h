#pragma once

#include "themes/theme.h"
#include "themes/thememanager.h"
#include "paintutil.h"

#include <QPaintEvent>
#include <QResizeEvent>
#include <QShowEvent>
#include <QPainter>
#include <QSplitter>
#include <QString>
#include <QTabBar>
#include <QTabWidget>
#include <QVBoxLayout>

namespace rcx {

// ── The per-pane view tabs (bottom of every editor pane) ──
// One label per command: these tabs OWN the view mode, so tab 0 is the
// structure view — it used to carry the application name, which put the brand
// on screen twice (title bar + a tab) and named a *view* after the product.
struct PaneTabSpec { const char* name; const char* tip; };

inline constexpr PaneTabSpec kPaneTabs[] = {
    { "Structure", "Structure view" },
    { "Code",      "Generated code" },
    { "Debug",     "Layout diagnostics (compose dump)" },
    { "Both",      "Structure and code side by side" },
};
inline constexpr int kPaneTabCount = int(sizeof(kPaneTabs) / sizeof(kPaneTabs[0]));

// Single source of truth for the pane-tab strip — used BOTH at pane creation
// and in MainWindow::applyTheme. The two sites used to carry hand-written
// sheets that disagreed (paper vs background, bottom underline vs a 3-px top
// accent with a negative padding), so a theme switch silently changed the
// grammar of the strip.
//
// One tab grammar (shared with the ribbon tabs and the doc tabs): no box, no
// fill, no radius — the selected tab is marked only by 2 px of accent on the
// BOTTOM edge, inactive is textDim, hover and active are text.
inline QString paneTabStyle(const Theme& t, const QString& family) {
    return QStringLiteral(
        // The whole strip is one surface. Styling only ::tab left the tabs on
        // paper and the rest of the bar on theme.background, which drew a
        // faint box around the tab group — the exact "fill at rest" cue the
        // underline grammar exists to remove.
        "QTabWidget { background: %1; }"
        // The zoom/format corner widget is part of the SAME strip, so it
        // shares the surface. Without this the strip read two-tone: paper
        // under the tabs, theme.background under the zoom control.
        "QWidget#rcxPaneCorner { background: %1; }"
        "QTabWidget::pane { border: none; background: %1; }"
        "QTabBar { border: none; background: %1; }"
        "QTabBar::tab {"
        "  background: %1; color: %2; padding: 0px %6px; border: none;"
        // Reserve the underline on EVERY tab. A border-bottom only on
        // :selected adds to that tab's box, so the selected tab stood 2 px
        // taller than its siblings (and than the corner widget, pinned to 26).
        "  border-bottom: 2px solid transparent;"
        "  border-radius: 0px; height: 26px;"
        "  font-family: '%5'; font-size: 10pt;"
        "}"
        // kGutter is spent ONCE: the margin puts the first tab's box at the
        // gutter, so its padding-left has to go, or the first ink lands at 16.
        "QTabBar::tab:first { margin-left: %6px; padding-left: 0px; }"
        "QTabBar::tab:selected { color: %3; border-bottom: 2px solid %4; }"
        "QTabBar::tab:hover { color: %3; }")
        .arg(editorPaperColor(t).name(), t.textDim.name(), t.text.name(),
             t.indHoverSpan.name(), family)
        .arg(kGutter);   // the strip's first ink lines up with every other strip
}

// ── The pane's outline ──
//
// The box used to be painted by the CONTENT: main.cpp's EditorContainer drew a
// 4-edge box around itself. That made the outline track "was this view wrapped
// in a container" rather than "this is a pane", and it showed three ways:
// Structure and Code were boxed (both are wrapped), Debug was bare (its
// QsciScintilla IS the tab page), Both drew TWO boxes (one container per
// splitter side), and the view-tab strip — outside every container — hung below
// the box as an unbordered band, pure white on a light theme.
//
// So a container around the whole pane draws it instead: one outline, the view
// tabs inside it, identical in all four view modes. The tab widget is held with
// a 1-logical-px margin so the edges are never covered by it; painting them on
// the QTabWidget itself does not work, because its page and tab bar are
// children that cover the parent completely.
//
// EditorContainer keeps only its BOTTOM edge, which is the seam between the
// document and the tab strip. Any other edge it drew would land on this box's
// device rows and double.
//
// No Q_OBJECT: this declares no signals or slots, and both the functor+context
// connect and findChildren work without moc (the rule BreadcrumbBar and
// EnumPickerPopup already follow).
class PaneBox : public QWidget {
public:
    explicit PaneBox(QWidget* inner, QWidget* parent = nullptr) : QWidget(parent) {
        auto* lay = new QVBoxLayout(this);
        lay->setContentsMargins(1, 1, 1, 1);   // room for the box
        lay->setSpacing(0);
        lay->addWidget(inner);
        m_inner = inner;
        applyBoxTheme(ThemeManager::instance().current());
        connect(&ThemeManager::instance(), &ThemeManager::themeChanged, this,
                [this](const Theme& t) { applyBoxTheme(t); });
    }

    QColor boxColor() const { return m_box; }

protected:
    void paintEvent(QPaintEvent*) override {
        if (!m_box.isValid()) return;
        QPainter p(this);
        const QRectF r(rect());
        rcx::fillTopDeviceRowOfRect(p, r, m_box);
        rcx::fillBottomDeviceRowOfRect(p, r, m_box);
        rcx::fillLeftDeviceColOfRect(p, r, m_box);
        rcx::fillRightDeviceColOfRect(p, r, m_box);
    }

private:
    void applyBoxTheme(const Theme& t) {
        m_box = containerBorderColor(t);
        // Both mode puts the two views in a splitter inside this box, so its
        // handle is an interior seam and has to match. Done here so no
        // theme-apply site has to know the pane owns a splitter.
        const QString handle = QStringLiteral("QSplitter::handle { background: %1; }")
                                   .arg(m_box.name());
        if (m_inner)
            for (QSplitter* sp : m_inner->findChildren<QSplitter*>())
                sp->setStyleSheet(handle);
        update();
    }

    QWidget* m_inner = nullptr;
    QColor   m_box;
};

} // namespace rcx
