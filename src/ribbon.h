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
// Layout (10 pt, fm.height 17): tab row fm.height()+3 = 20, body 69
// (padTop 2 + 3 rows × 18 + caption 12 + hairline 1) → 89 total.
//   Flat: no panel boxes, no caption bands — one 1-device-px `border`
//   divider column per panel gap (kPanelGap 13, divider at A.right()+7),
//   a 9 pt caption (textDim through the tone LADDER) under each panel, and
//   hairlines above and below the body. The tab row is `background` too: it
//   is the ribbon's own header, not a second menu strip. Panel = columns of
//   Small items (3 per column, new column on columnBreakBefore /
//   separatorBefore / when full) or full-height Large items; a panel whose
//   items carry `groupCaption`s draws those under their column spans instead
//   of one panel caption.
//   Item tones: rest = `text` (icons and labels — the ribbon is the actionable
//   surface), hover = `hover` fill, pressed = `button`, checked =
//   indHoverSpan + a 2-device-row underline (same as the active tab), disabled
//   = 40 % opacity of the same ink (never textDim × 0.4), destructive = the
//   icon in markerPtr always and the label red only under the pointer.
// Narrow widths: stage 1 drops labels (LabelMode::Auto only) by labelDrop,
// higher first, equal values together; stage 2 hides whole panels (lower
// hideOrder first, never `neverHide`) behind a middle-row … button whose QMenu
// carries one submenu per hidden panel.
// A 22-px chevron at the right end of the tab row collapses / expands the body
// (Ctrl+F1, or double-click a tab); the state is persisted by the host through
// the single `ribbonState` key (see ribbonStateFromSettings).

#include "ribbon_spec.h"
#include "themes/theme.h"

#include <QAction>
#include <QElapsedTimer>
#include <QHash>
#include <QPair>
#include <QPointer>
#include <QRect>
#include <QSet>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>

class QMenu;

namespace rcx {

// ── Persisted ribbon state ──
// ONE key, three values: the chevron, the double-click and View ▸ Ribbon all
// write it, so the strip can never disagree with the menu (it used to keep
// `showRibbon` and `ribbonMinimized` separately and the double-click only
// touched the second).
enum RibbonState { RibbonFull = 0, RibbonCollapsed = 1, RibbonHidden = 2 };

// Reads `ribbonState`, migrating the two legacy keys once on first read (and
// removing them, so no second persisted state survives).
inline int ribbonStateFromSettings(QSettings& s) {
    if (!s.contains(QStringLiteral("ribbonState"))) {
        const bool shown = s.value(QStringLiteral("showRibbon"), true).toBool();
        const bool mini  = s.value(QStringLiteral("ribbonMinimized"), false).toBool();
        s.setValue(QStringLiteral("ribbonState"),
                   !shown ? int(RibbonHidden) : mini ? int(RibbonCollapsed) : int(RibbonFull));
        // Only the migrating read writes: this is called on every launch (and
        // from tests), and a "read" that dirties the store on every call is a
        // trap for the next caller.
        s.remove(QStringLiteral("showRibbon"));
        s.remove(QStringLiteral("ribbonMinimized"));
    }
    const int v = s.value(QStringLiteral("ribbonState"), int(RibbonFull)).toInt();
    return (v < RibbonFull || v > RibbonHidden) ? int(RibbonFull) : v;
}

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
    // The rect a per-column group caption ("Hex:", "Str:") is drawn in, on the
    // current tab. Null when no laid-out panel carries that caption.
    QRect   groupCaptionRect(const QString& caption) const;
    QStringList overflowedPanelIds() const;
    QRect   overflowButtonRect() const;
    // The collapse chevron at the right end of the tab row (always present).
    QRect   collapseButtonRect() const;
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
        QRect   rect;            // items + caption (no box is painted)
        QRect   captionRect;
        QVector<int> separatorXs;   // `||` hairlines inside the panel
        // Per-column-group captions (Type: "Hex:" "Int:" …). Non-empty means
        // the panel's own caption is NOT drawn.
        QVector<QPair<QRect, QString>> groupCaptions;
        bool    labelsDropped = false;
    };
    struct Layout {
        QVector<LaidItem>  items;
        QVector<LaidPanel> panels;
        QVector<int>       dividerXs;      // 1-device-px columns between panels (+ before …)
        QStringList        hiddenPanels;
        QRect              overflowRect;   // the … item (middle row) when panels are hidden
        int                width = 0;
        int                naturalWidth = 0;
        int                forWidth = -1;   // widget width this layout was computed for
        QString            tab;
    };
    struct Metrics {
        int tabRowH = 25, rowH = 18, captionH = 12, captionGap = 4, padTop = 2, bodyH = 73;
        int   smallIcon = 16;          // small cell height (logical)
        int   largeIconDev = 32;       // large Codicon side in DEVICE px (ribbonLargeIconDev)
        qreal largeIcon = 25.6;        // … in logical px (largeIconDev / dpr)
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

    QString m_hoverId;       // item id, "tab:<id>", "overflow" or "collapse"
    QString m_pressedId;
    bool    m_overflowOpen = false;   // … stays pressed while its menu is up
    // Started when a tab press restores the minimized body: the DblClick that
    // completes that same click pair must not collapse it again.
    QElapsedTimer m_restoreTimer;
    QMenu*  m_overflowMenu = nullptr;
};

}  // namespace rcx
