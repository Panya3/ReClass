#pragma once
// rcx::RibbonBar — the ReClassEx-style tabbed ribbon (Home | Modify).
//
// One custom-painted QWidget instead of ~80 child buttons: device-exact
// hairlines via paintutil.h (QSS borders double up at 125 %), full control of
// the large / small / caption geometry, and a single paintEvent. Every button
// is still backed by a QAction (enabled / checked / visible / toolTip are read
// from it and `QAction::changed` repaints), so menus, the overflow menu and the
// command palette can reuse them. The ribbon never calls setFocus and never
// addAction()s its actions to a widget — the editor owns the plain keys;
// shortcuts only show up in tooltips.
//
// Layout (100 %): tab row fm.height()+5 ≈ 22, body ≈ 74 → ≈ 96 total.
//   panel = columns of Small items (3 per column, new column on
//   columnBreakBefore / separatorBefore / when full) or full-height Large
//   items, a caption strip underneath, hairline separators between panels.
// Narrow widths: stage 1 drops labels (glyphLabels panels first, then — in
// LabelMode::Auto — the rest by dropPriority), stage 2 hides whole panels
// (lowest dropPriority first, never `neverHide`) behind a » chevron whose
// QMenu carries one submenu per hidden panel.

#include "ribbon_spec.h"
#include "themes/theme.h"

#include <QAction>
#include <QElapsedTimer>
#include <QHash>
#include <QPointer>
#include <QRect>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

class QMenu;

namespace rcx {

class RibbonBar : public QWidget {
    Q_OBJECT
public:
    enum class LabelMode { Auto = 0, All = 1, IconsOnly = 2 };

    explicit RibbonBar(QWidget* parent = nullptr);
    RibbonBar(const QVector<RibbonTabSpec>& spec, QWidget* parent = nullptr);

    // ── Actions ──
    // The action currently backing `id`: the external one when setAction()
    // swapped it in, else the ribbon-owned default (text=label, toolTip, data).
    QAction* action(const QString& id) const;
    // Swap an application action in. The ribbon keeps its own label + icon but
    // reads enabled / checked / visible / toolTip / shortcut from `external`
    // (nullptr restores the ribbon-owned default).
    void setAction(const QString& id, QAction* external);
    QStringList actionIds() const;

    // ── Presentation ──
    void applyTheme(const Theme& theme);
    const Theme& theme() const { return m_theme; }

    void setLabelMode(LabelMode mode);
    LabelMode labelMode() const { return m_labelMode; }

    void setCurrentTab(const QString& tabId);
    QString currentTab() const { return m_currentTab; }
    QStringList tabIds() const;

    void setMinimized(bool minimized);
    bool isMinimized() const { return m_minimized; }

    // ── Geometry queries (current tab, current width) ──
    QString itemIdAt(QPoint pos) const;
    QRect   itemRect(const QString& id) const;   // null when hidden / not laid out
    int     itemColumn(const QString& id) const; // global column index, -1 when hidden
    bool    itemLabelShown(const QString& id) const;
    QRect   tabRect(const QString& tabId) const;
    QRect   panelRect(const QString& panelId) const;
    QStringList overflowedPanelIds() const;
    QRect   overflowButtonRect() const;
    // The » menu (rebuilt on every call; owned by the ribbon).
    QMenu*  overflowMenu();

    int tabRowHeight() const;
    int bodyHeight() const;
    int preferredHeight() const;   // tab row (+ body unless minimized)
    int naturalWidth() const;      // current tab, labels per LabelMode, no compaction

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

    const QVector<RibbonTabSpec>& spec() const { return m_spec; }

signals:
    void currentTabChanged(const QString& tabId);
    void labelModeChanged(int mode);
    void minimizedChanged(bool minimized);

protected:
    void paintEvent(QPaintEvent*) override;
    void resizeEvent(QResizeEvent*) override;
    void changeEvent(QEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
    void mouseDoubleClickEvent(QMouseEvent*) override;
    void leaveEvent(QEvent*) override;

private:
    struct ItemRef { int tab = -1; int panel = -1; int item = -1; };

    struct LaidItem {
        QString id;
        ItemRef ref;
        QRect   rect;
        int     column = 0;      // global column index (unique across panels)
        bool    large = false;
        bool    label = true;
    };
    struct LaidPanel {
        QString id;
        QRect   rect;            // full panel box (items + caption)
        QRect   captionRect;
        QVector<int> separatorXs;   // `||` hairlines inside the panel
        bool    labelsDropped = false;
    };
    struct Layout {
        QVector<LaidItem>  items;
        QVector<LaidPanel> panels;
        QStringList        hiddenPanels;
        QRect              overflowRect;
        int                width = 0;
        int                naturalWidth = 0;
        int                forWidth = -1;   // widget width this layout was computed for
        QString            tab;
    };
    struct Metrics {
        int tabRowH = 22, rowH = 18, captionH = 14, bodyPad = 3, bodyH = 74;
        int smallIcon = 16, largeIcon = 32;
    };

    void init();
    void buildActions();
    QAction* effectiveAction(const QString& id) const;
    const RibbonItemSpec* itemSpec(const QString& id) const;
    const RibbonItemSpec* itemSpec(const ItemRef& ref) const;
    int tabIndex(const QString& tabId) const;

    Metrics metrics() const;
    QFont captionFont() const;
    void relayout() const;             // recompute for the current width / tab
    void ensureLayout() const;         // lazily from paint / queries (mutable layout)
    void markLayoutDirty();
    Layout computeLayout(int tabIdx, int availW, const QSet<QString>& dropLabels,
                         const QStringList& hidePanels, bool reserveOverflow) const;
    int  smallItemWidth(const RibbonItemSpec& it, bool label, const QFontMetrics& fm) const;
    int  largeItemWidth(const RibbonItemSpec& it, bool label, const QFontMetrics& fm) const;
    bool isItemVisible(const QString& id) const;

    void paintTabs(QPainter& p, const Metrics& m);
    void paintBody(QPainter& p, const Metrics& m);
    void paintItem(QPainter& p, const LaidItem& li, const Metrics& m);
    void updateHover(QPoint pos);
    void refreshToolTip();             // widget toolTip <- hovered item (GlobalTooltipBridge shows it)
    void showOverflowMenu();
    QString tabIdAt(QPoint pos) const;
    QString tooltipFor(const QString& id) const;
    void refreshOwnActionIcons();      // own actions + externals whose icon we supplied

    QVector<RibbonTabSpec> m_spec;
    QHash<QString, ItemRef>   m_index;
    QHash<QString, QAction*>  m_own;
    QHash<QString, QPointer<QAction>> m_external;
    QSet<QString> m_suppliedIcons;     // externals that came without an icon -> we stamp ours
    QStringList m_ids;

    Theme     m_theme;
    LabelMode m_labelMode = LabelMode::Auto;   // matches the app's `ribbonLabels` default
    QString   m_currentTab;
    bool      m_minimized = false;

    mutable Layout m_layout;
    mutable bool   m_layoutDirty = true;

    QString m_hoverId;       // item id, "tab:<id>" or "overflow"
    QString m_pressedId;
    // Started when a tab press restores the minimized body: the DblClick that
    // completes that same click pair must not collapse it again.
    QElapsedTimer m_restoreTimer;
    QMenu*  m_overflowMenu = nullptr;
};

}  // namespace rcx
