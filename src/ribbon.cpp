#include "ribbon.h"

#include "core.h"
#include "fontutil.h"
#include "paintutil.h"
#include "rcxtooltip.h"
#include "ribbon_icons.h"
#include "themes/thememanager.h"

#include <QApplication>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QSet>
#include <algorithm>

namespace rcx {

// The tab / panel / item tables live in ribbon_spec.h — the wiring half reads
// the same labels and tooltips from them, so a command has ONE name.

namespace {

// Sensible colours when ThemeManager has nothing loaded (tests, harnesses run
// from a directory without themes/). Mirrors src/themes/defaults/vs.json.
Theme fallbackTheme() {
    static const char* kJson = R"({
        "name": "VS2022 Dark", "background": "#181818", "backgroundAlt": "#2d2d30",
        "surface": "#333337", "border": "#3f3f46", "borderFocused": "#b180d7",
        "button": "#3f3f46", "text": "#dcdcdc", "textDim": "#858585",
        "textMuted": "#636369", "textFaint": "#585862", "hover": "#242427",
        "selected": "#2c2c31", "selection": "#264f78", "syntaxKeyword": "#569cd6",
        "syntaxNumber": "#b5cea8", "syntaxString": "#d69d85", "syntaxComment": "#57a64a",
        "syntaxPreproc": "#9b9b9b", "syntaxType": "#4ec9b0", "indHoverSpan": "#b180d7",
        "indCmdPill": "#2d2d30", "indDataChanged": "#8fbc7a", "indHintGreen": "#5a8248",
        "indRttiHint": "#D7BA7D", "markerPtr": "#f44747", "markerCycle": "#e5a00d",
        "markerError": "#7a2e2e" })";
    return Theme::fromJson(QJsonDocument::fromJson(kJson).object());
}

// The strip's first ink lines up with every other strip in the window
// (title label, breadcrumb, status) — one left edge down the whole app.
constexpr int kLeftMargin   = kGutter;
constexpr int kRightMargin  = 6;
constexpr int kPanelGap     = 13;  // between panels ...
constexpr int kDividerInset = 7;   // ... with the 1-device-px divider at A.right() + 7
constexpr int kColGap       = 3;   // `|`
constexpr int kSepGap       = 7;   // `||` (hairline in the middle)
constexpr int kTabPad       = kGutter;
constexpr int kTabGap       = 4;
constexpr int kOverflowW    = 22;  // the ... item: middle row, right of a divider
constexpr int kOverflowH    = 18;
constexpr int kOverflowInset = 5;  // overflow x = dividerX + 5
constexpr int kCollapseW    = 22;  // the collapse chevron, right end of the tab row
constexpr int kMenuChevronW = 10;  // width a `menu` item adds for its ▾
constexpr int kMenuChevron  = 8;   // ... the glyph cell inside it
constexpr int kLargeMinW    = 48;
constexpr int kLargeMaxW    = 80;
constexpr int kLargeIconOnlyW = 40;
constexpr double kDisabledOpacity = 0.40;

// Draws a dpr-stamped pixmap so its top-left lands on a whole device pixel
// (no SmoothPixmapTransform: the pixmap is already at device resolution).
void drawPixmapSnapped(QPainter& p, const QPointF& logicalPos, const QPixmap& pm) {
    const QTransform dt = p.deviceTransform();
    QPointF dev = dt.map(logicalPos);
    dev = QPointF(qRound(dev.x()), qRound(dev.y()));
    p.drawPixmap(dt.inverted().map(dev), pm);
}

// Pressed surface: `button`, or `selected` on themes whose button colour is
// the body colour (tw.json) so a press is still visible.
QColor pressedFill(const Theme& t) {
    return (t.button.isValid() && t.button != t.background) ? t.button
         : (t.selected.isValid() ? t.selected : t.hover);
}

}  // namespace

// ═══════════════════════════════════════════════════════════════════════════
// RibbonBar
// ═══════════════════════════════════════════════════════════════════════════

RibbonBar::RibbonBar(QWidget* parent)
    : RibbonBar(defaultRibbonSpec(), parent) {}

RibbonBar::RibbonBar(const QVector<RibbonTabSpec>& spec, QWidget* parent)
    : QWidget(parent), m_spec(spec) {
    init();
}

void RibbonBar::init() {
    setFocusPolicy(Qt::NoFocus);
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setAttribute(Qt::WA_OpaquePaintEvent, true);

    const QString family = QStringLiteral("JetBrains Mono");
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const bool hasMono = QFontDatabase::hasFamily(family);
#else
    const bool hasMono = QFontDatabase().hasFamily(family);
#endif
    if (hasMono) setFont(QFont(family, 10));

    m_theme = ThemeManager::instance().current();
    if (!m_theme.background.isValid() || !m_theme.text.isValid())
        m_theme = fallbackTheme();

    for (int t = 0; t < m_spec.size(); ++t)
        for (int pi = 0; pi < m_spec[t].panels.size(); ++pi)
            for (int ii = 0; ii < m_spec[t].panels[pi].items.size(); ++ii) {
                const QString& id = m_spec[t].panels[pi].items[ii].id;
                if (m_index.contains(id)) continue;   // "edit.*" lives on both tabs
                m_index.insert(id, ItemRef{t, pi, ii});
                m_ids.append(id);
            }
    if (!m_spec.isEmpty()) m_currentTab = m_spec.first().id;

    buildActions();
    refreshOwnActionIcons();
}

void RibbonBar::buildActions() {
    for (const QString& id : m_ids) {
        const RibbonItemSpec* it = itemSpec(id);
        auto* a = new QAction(it->label, this);
        a->setToolTip(it->tooltip.isEmpty() ? it->label : it->tooltip);
        a->setData(it->data);
        connect(a, &QAction::changed, this, [this] { markLayoutDirty(); update(); });
        m_own.insert(id, a);
    }
}

void RibbonBar::refreshOwnActionIcons() {
    const qreal dpr = devicePixelRatioF();
    for (auto it = m_own.begin(); it != m_own.end(); ++it) {
        const RibbonItemSpec* spec = itemSpec(it.key());
        if (!spec) continue;
        // Square 16x16 cells for QAction / QMenu use: the ribbon's 24/32-wide
        // glyph cells would otherwise be downscaled into blur.
        RibbonIconOptions o;
        o.wide = false;
        o.plainInk = m_theme.text;
        const QIcon icon(ribbonIcon(spec->icon, RibbonIconSize::Small, dpr, m_theme, o));
        it.value()->setIcon(icon);
        // Externals that were bound without an icon carry ours (so the >>
        // overflow submenus show glyphs in the real app); keep them in sync
        // across theme / DPI changes.
        if (m_suppliedIcons.contains(it.key()))
            if (auto ext = m_external.value(it.key()); !ext.isNull()) ext->setIcon(icon);
    }
}

// ── Actions ──

QAction* RibbonBar::effectiveAction(const QString& id) const {
    auto ext = m_external.constFind(id);
    if (ext != m_external.constEnd() && !ext.value().isNull()) return ext.value().data();
    return m_own.value(id, nullptr);
}

QAction* RibbonBar::action(const QString& id) const { return effectiveAction(id); }

void RibbonBar::setAction(const QString& id, QAction* external) {
    if (!m_index.contains(id)) return;
    if (auto old = m_external.value(id); !old.isNull())
        disconnect(old.data(), nullptr, this, nullptr);
    m_suppliedIcons.remove(id);
    if (external) {
        m_external.insert(id, external);
        if (external->icon().isNull()) {
            if (QAction* own = m_own.value(id)) external->setIcon(own->icon());
            m_suppliedIcons.insert(id);
        }
        connect(external, &QAction::changed, this, [this] { markLayoutDirty(); refreshToolTip(); update(); });
        connect(external, &QObject::destroyed, this, [this] { markLayoutDirty(); update(); });
    } else {
        m_external.remove(id);
    }
    markLayoutDirty();
    refreshToolTip();
    update();
}

QStringList RibbonBar::actionIds() const { return m_ids; }

const RibbonItemSpec* RibbonBar::itemSpec(const ItemRef& ref) const {
    if (ref.tab < 0 || ref.tab >= m_spec.size()) return nullptr;
    const auto& panels = m_spec[ref.tab].panels;
    if (ref.panel < 0 || ref.panel >= panels.size()) return nullptr;
    const auto& items = panels[ref.panel].items;
    if (ref.item < 0 || ref.item >= items.size()) return nullptr;
    return &items[ref.item];
}

const RibbonItemSpec* RibbonBar::itemSpec(const QString& id) const {
    auto it = m_index.constFind(id);
    return it == m_index.constEnd() ? nullptr : itemSpec(it.value());
}

bool RibbonBar::isItemVisible(const QString& id) const {
    QAction* a = effectiveAction(id);
    return a ? a->isVisible() : true;
}

int RibbonBar::tabIndex(const QString& tabId) const {
    for (int i = 0; i < m_spec.size(); ++i)
        if (m_spec[i].id == tabId) return i;
    return -1;
}

QStringList RibbonBar::tabIds() const {
    QStringList out;
    for (const auto& t : m_spec) out << t.id;
    return out;
}

// ── Presentation ──

void RibbonBar::applyTheme(const Theme& theme) {
    m_theme = theme;
    if (!m_theme.background.isValid()) m_theme = fallbackTheme();
    refreshOwnActionIcons();
    markLayoutDirty();
    update();
}

void RibbonBar::setLabelMode(LabelMode mode) {
    if (mode == m_labelMode) return;
    m_labelMode = mode;
    markLayoutDirty();
    updateGeometry();
    update();
    emit labelModeChanged(int(mode));
}

void RibbonBar::setCurrentTab(const QString& tabId) {
    if (tabId == m_currentTab || tabIndex(tabId) < 0) return;
    m_currentTab = tabId;
    m_hoverId.clear();
    m_pressedId.clear();
    markLayoutDirty();
    updateGeometry();
    update();
    emit currentTabChanged(tabId);
}

void RibbonBar::setMinimized(bool minimized) {
    if (minimized == m_minimized) return;
    m_minimized = minimized;
    m_hoverId.clear();
    m_pressedId.clear();
    refreshToolTip();   // the hovered item is gone — so is its text
    updateGeometry();
    update();
    emit minimizedChanged(minimized);
}

// ── Metrics ──

RibbonBar::Metrics RibbonBar::metrics() const {
    Metrics m;
    const QFontMetrics fm(font());
    m.tabRowH  = fm.height() + 3;              // 20 at 10 pt
    m.rowH     = qMax(18, fm.height() + 1);    // 18
    m.captionH = 12;                           // 9 pt caption; glyphs overhang the rect
    m.padTop   = 2;
    // padTop + 3 rows + caption + body bottom hairline -> 69; 89 with the tab row
    m.bodyH = m.padTop + 3 * m.rowH + m.captionH + 1;
    const qreal dpr = devicePixelRatioF() > 0 ? devicePixelRatioF() : 1.0;
    m.smallIcon    = 16;
    m.largeIconDev = ribbonLargeIconDev(dpr);
    m.largeIcon    = m.largeIconDev / dpr;
    return m;
}

// 9 pt under a 10 pt body: the only second type size on the strip.
QFont RibbonBar::captionFont() const {
    QFont f = font();
    f.setPointSize(qMax(6, resolvedPointSize(font()) - 1));
    return f;
}

int RibbonBar::tabRowHeight() const { return metrics().tabRowH; }
int RibbonBar::bodyHeight() const { return metrics().bodyH; }
int RibbonBar::preferredHeight() const {
    const Metrics m = metrics();
    return m_minimized ? m.tabRowH + 1 : m.tabRowH + m.bodyH;
}

QSize RibbonBar::sizeHint() const { return QSize(naturalWidth() + kRightMargin, preferredHeight()); }
QSize RibbonBar::minimumSizeHint() const { return QSize(80, preferredHeight()); }

// 3 + icon cell + 4 + label + 6; icon-only = cell + 6; +10 for a `menu` ▾.
// The cell is per icon kind (ribbonIconCellWidth: 16 / 24 / 32) so the pixel
// labels share one scale — and an unlabelled Add / Insert cell is the wide one
// (it carries the byte count itself).
int RibbonBar::smallItemWidth(const RibbonItemSpec& it, bool label, const QFontMetrics& fm) const {
    const bool labelled = label && !it.iconOnly;
    const int cellW = ribbonIconCellWidth(it.icon, labelled);
    const int chev = it.menu ? kMenuChevronW : 0;
    if (!labelled) return cellW + 6 + chev;
    return 3 + cellW + 4 + fm.horizontalAdvance(ribbonStripLabel(it)) + 6 + chev;
}

// clamp(label + 8, 48, 80) (+10 for a `menu` ▾); the label is elided (never
// for shipped labels).
int RibbonBar::largeItemWidth(const RibbonItemSpec& it, bool label, const QFontMetrics& fm) const {
    const int chev = it.menu ? kMenuChevronW : 0;
    if (!label) return kLargeIconOnlyW + chev;
    return qBound(kLargeMinW, fm.horizontalAdvance(ribbonStripLabel(it)) + 8, kLargeMaxW) + chev;
}

// ── Layout engine ──

RibbonBar::Layout RibbonBar::computeLayout(int tabIdx, int availW,
                                           const QSet<QString>& dropLabels,
                                           const QStringList& hidePanels,
                                           bool reserveOverflow) const {
    Layout L;
    L.forWidth = availW;
    L.tab = m_currentTab;
    if (tabIdx < 0 || tabIdx >= m_spec.size()) return L;
    const Metrics m = metrics();
    const QFontMetrics fm(font());
    const QFontMetrics cfm(captionFont());
    const int itemsTop = m.tabRowH + m.padTop;
    const int itemsH   = 3 * m.rowH;
    const int captionTop = itemsTop + itemsH;

    int x = kLeftMargin;
    int column = 0;
    const RibbonTabSpec& tab = m_spec[tabIdx];
    for (int pi = 0; pi < tab.panels.size(); ++pi) {
        const RibbonPanelSpec& panel = tab.panels[pi];
        if (hidePanels.contains(panel.id)) continue;
        // A glyphLabels panel (Type) is glyph-only in EVERY mode — its icons
        // ARE its labels, so "All labels" no longer promises words it can't
        // deliver. `keepLabel` items (Custom…) opt back in per item.
        const bool labels = (m_labelMode != LabelMode::IconsOnly)
                         && !panel.glyphLabels && !dropLabels.contains(panel.id);

        struct Col {
            QVector<int> items;   // indexes into panel.items
            bool large = false;
            bool sep = false;
            bool brk = false;
            int  w = 0;
            QString groupCaption;   // set on the column that starts a caption group
            bool endsGroup = false; // closes the open group WITHOUT opening one
        };
        QVector<Col> cols;
        for (int ii = 0; ii < panel.items.size(); ++ii) {
            const RibbonItemSpec& it = panel.items[ii];
            if (!isItemVisible(it.id)) continue;
            const bool large = (it.size == RibbonItemSize::Large);
            bool newCol = cols.isEmpty() || large || cols.last().large
                       || it.columnBreakBefore || it.separatorBefore
                       || cols.last().items.size() >= 3;
            if (newCol) {
                Col c;
                c.large = large;
                c.sep = it.separatorBefore && !cols.isEmpty();
                c.brk = (it.columnBreakBefore || it.separatorBefore) && !cols.isEmpty();
                cols.append(c);
            }
            Col& c = cols.last();
            if (!it.groupCaption.isEmpty() && c.groupCaption.isEmpty())
                c.groupCaption = it.groupCaption;
            if (it.endsCaptionGroup) c.endsGroup = true;
            c.items.append(ii);
            const bool itemLabel = (labels || it.keepLabel) && !it.iconOnly;
            const int w = large ? largeItemWidth(it, itemLabel, fm)
                                : smallItemWidth(it, itemLabel, fm);
            c.w = qMax(c.w, w);
        }
        if (cols.isEmpty()) continue;

        // Size = frequency, and equal widths inside a panel: every Large column
        // takes the widest Large width, so Open/Save and Scanner/Symbols are
        // one block instead of two ragged towers.
        {
            int maxLargeW = 0;
            for (const Col& c : cols) if (c.large) maxLargeW = qMax(maxLargeW, c.w);
            if (maxLargeW > 0)
                for (Col& c : cols) if (c.large) c.w = maxLargeW;
        }

        // A group caption must fit over the columns it spans; widen the last
        // column of the group when it doesn't ("Byte:" over one 22-px column).
        for (int ci = 0; ci < cols.size(); ++ci) {
            if (cols[ci].groupCaption.isEmpty()) continue;
            int end = ci + 1;
            while (end < cols.size() && cols[end].groupCaption.isEmpty() && !cols[end].endsGroup) ++end;
            int span = 0;
            for (int k = ci; k < end; ++k) {
                if (k > ci) span += cols[k].sep ? kSepGap : kColGap;
                span += cols[k].w;
            }
            const int need = cfm.horizontalAdvance(cols[ci].groupCaption) + 2;
            if (need > span) cols[end - 1].w += need - span;
        }

        // Panel width is the wider of its columns and its caption (+2); the
        // columns are centred when the caption wins.
        int itemsW = 0;
        for (int ci = 0; ci < cols.size(); ++ci) {
            if (ci > 0) itemsW += cols[ci].sep ? kSepGap : kColGap;
            itemsW += cols[ci].w;
        }
        const bool grouped = std::any_of(cols.cbegin(), cols.cend(),
                                         [](const Col& c) { return !c.groupCaption.isEmpty(); });
        const int captionW = grouped ? 0 : cfm.horizontalAdvance(panel.caption) + 2;
        const int panelW = qMax(itemsW, captionW);

        LaidPanel lp;
        lp.id = panel.id;
        lp.labelsDropped = !labels;
        const int panelX = x;
        int cx = panelX + (panelW - itemsW) / 2;
        int groupStartX = cx;
        // Right edge of the last column placed, so a group's caption spans the
        // COLUMNS it names and not the gap that follows them.
        int groupEndX = cx;
        QString groupText;
        auto closeGroup = [&] {
            if (!groupText.isEmpty())
                lp.groupCaptions.append({QRect(groupStartX, captionTop,
                                               groupEndX - groupStartX, m.captionH), groupText});
            groupText.clear();
        };
        for (int ci = 0; ci < cols.size(); ++ci) {
            const Col& c = cols[ci];
            if (ci > 0) {
                if (c.sep) { lp.separatorXs << cx + kSepGap / 2; cx += kSepGap; }
                else cx += kColGap;
            }
            // A column either opens a new group or closes the open one; either
            // way the previous group ends at the previous column's right edge.
            if (!c.groupCaption.isEmpty() || c.endsGroup) {
                closeGroup();
                groupStartX = cx;
                groupText = c.groupCaption;   // empty for a pure terminator
            }
            for (int r = 0; r < c.items.size(); ++r) {
                const RibbonItemSpec& it = panel.items[c.items[r]];
                LaidItem li;
                li.id = it.id;
                li.ref = ItemRef{tabIdx, pi, c.items[r]};
                li.large = c.large;
                li.label = (labels || it.keepLabel) && !it.iconOnly;
                li.column = column;
                li.rect = c.large ? QRect(cx, itemsTop, c.w, itemsH)
                                  : QRect(cx, itemsTop + r * m.rowH, c.w, m.rowH);
                L.items.append(li);
            }
            cx += c.w;
            groupEndX = cx;
            ++column;
        }
        closeGroup();
        lp.rect = QRect(panelX, itemsTop, panelW, itemsH + m.captionH);
        lp.captionRect = QRect(panelX, captionTop, panelW, m.captionH);
        // One divider per gap (none before the first panel).
        if (!L.panels.isEmpty()) L.dividerXs << L.panels.last().rect.right() + kDividerInset;
        L.panels.append(lp);
        x = panelX + panelW + kPanelGap;
    }
    // Hidden panels are reported in the order they were hidden (stage 2 order).
    for (const QString& pid : hidePanels)
        for (const auto& panel : tab.panels)
            if (panel.id == pid) { L.hiddenPanels << pid; break; }
    // x now sits one gap past the last panel
    int right = L.panels.isEmpty() ? kLeftMargin : x - kPanelGap;
    L.naturalWidth = right;
    if (reserveOverflow || !L.hiddenPanels.isEmpty()) {
        // ... = a normal divider, then a single middle-row 22x18 item.
        const int dividerX = right - 1 + kDividerInset;
        L.dividerXs << dividerX;
        L.overflowRect = QRect(dividerX + kOverflowInset, itemsTop + m.rowH, kOverflowW, kOverflowH);
        right = L.overflowRect.right() + 1;
    }
    L.width = right + kRightMargin;
    return L;
}

void RibbonBar::ensureLayout() const {
    // resize() on a hidden widget delivers no resizeEvent, so the dirty flag
    // alone can't be trusted — the cache also remembers the width it was
    // computed for.
    if (m_layoutDirty || m_layout.forWidth != width() || m_layout.tab != m_currentTab) relayout();
}

void RibbonBar::relayout() const {
    m_layoutDirty = false;
    const int tabIdx = tabIndex(m_currentTab);
    const int availW = width();
    QSet<QString> drop;
    QStringList hide;

    Layout L = computeLayout(tabIdx, availW, drop, hide, false);
    if (L.width <= availW || tabIdx < 0) { m_layout = L; return; }

    const auto& panels = m_spec[tabIdx].panels;
    // Stage 1: drop labels by labelDrop, highest first; panels with the same
    // labelDrop go together (Add + Insert). Auto only — All means "show every
    // label the panel has", and glyphLabels panels never had any to drop.
    if (m_labelMode == LabelMode::Auto) {
        QVector<const RibbonPanelSpec*> cands;
        for (const auto& p : panels)
            if (!p.glyphLabels) cands.append(&p);
        std::stable_sort(cands.begin(), cands.end(),
                         [](const RibbonPanelSpec* a, const RibbonPanelSpec* b) {
                             return a->labelDrop > b->labelDrop;
                         });
        for (int i = 0; i < cands.size();) {
            int j = i;
            do { drop.insert(cands[j]->id); ++j; }
            while (j < cands.size() && cands[j]->labelDrop == cands[i]->labelDrop);
            L = computeLayout(tabIdx, availW, drop, hide, false);
            if (L.width <= availW) { m_layout = L; return; }
            i = j;
        }
    }
    // Stage 2: hide whole panels, lowest hideOrder first, never `neverHide`.
    QVector<const RibbonPanelSpec*> hideCands;
    for (const auto& p : panels)
        if (!p.neverHide) hideCands.append(&p);
    std::stable_sort(hideCands.begin(), hideCands.end(),
                     [](const RibbonPanelSpec* a, const RibbonPanelSpec* b) {
                         return a->hideOrder < b->hideOrder;
                     });
    for (const auto* p : hideCands) {
        hide << p->id;
        L = computeLayout(tabIdx, availW, drop, hide, true);
        if (L.width <= availW) { m_layout = L; return; }
    }
    m_layout = L;   // still too wide — best effort
}

void RibbonBar::markLayoutDirty() { m_layoutDirty = true; }

int RibbonBar::naturalWidth() const {
    return computeLayout(tabIndex(m_currentTab), width(), {}, {}, false).naturalWidth;
}

// ── Geometry queries ──

QString RibbonBar::itemIdAt(QPoint pos) const {
    if (m_minimized) return QString();
    ensureLayout();
    for (const LaidItem& li : m_layout.items)
        if (li.rect.contains(pos)) return li.id;
    return QString();
}

QRect RibbonBar::itemRect(const QString& id) const {
    if (m_minimized) return QRect();
    ensureLayout();
    for (const LaidItem& li : m_layout.items)
        if (li.id == id) return li.rect;
    return QRect();
}

int RibbonBar::itemColumn(const QString& id) const {
    if (m_minimized) return -1;
    ensureLayout();
    for (const LaidItem& li : m_layout.items)
        if (li.id == id) return li.column;
    return -1;
}

bool RibbonBar::itemLabelShown(const QString& id) const {
    if (m_minimized) return false;
    ensureLayout();
    for (const LaidItem& li : m_layout.items)
        if (li.id == id) return li.label;
    return false;
}

QRect RibbonBar::tabRect(const QString& tabId) const {
    const Metrics m = metrics();
    const QFontMetrics fm(font());
    int x = kLeftMargin;
    for (const auto& t : m_spec) {
        const int w = fm.horizontalAdvance(t.title) + 2 * kTabPad;
        if (t.id == tabId) return QRect(x, 0, w, m.tabRowH);
        x += w + kTabGap;
    }
    return QRect();
}

QString RibbonBar::tabIdAt(QPoint pos) const {
    for (const auto& t : m_spec)
        if (tabRect(t.id).contains(pos)) return t.id;
    return QString();
}

QRect RibbonBar::panelRect(const QString& panelId) const {
    if (m_minimized) return QRect();
    ensureLayout();
    for (const LaidPanel& lp : m_layout.panels)
        if (lp.id == panelId) return lp.rect;
    return QRect();
}

QRect RibbonBar::groupCaptionRect(const QString& caption) const {
    if (m_minimized) return QRect();
    ensureLayout();
    for (const LaidPanel& lp : m_layout.panels)
        for (const auto& gc : lp.groupCaptions)
            if (gc.second == caption) return gc.first;
    return QRect();
}

QStringList RibbonBar::overflowedPanelIds() const {
    ensureLayout();
    return m_layout.hiddenPanels;
}

QRect RibbonBar::overflowButtonRect() const {
    if (m_minimized) return QRect();
    ensureLayout();
    return m_layout.hiddenPanels.isEmpty() ? QRect() : m_layout.overflowRect;
}

// Always visible, at the right end of the tab row: "every persisted UI state
// has an on-screen control". Double-clicking a tab still works, but nothing
// on screen used to say the ribbon could collapse at all.
QRect RibbonBar::collapseButtonRect() const {
    const int h = metrics().tabRowH;
    // Never overlap the tabs: the chevron is hit-tested BEFORE tabIdAt, so at
    // very narrow widths an unclamped cell would eat clicks on the last tab.
    int minX = kLeftMargin;
    if (!m_spec.isEmpty()) {
        const QRect last = tabRect(m_spec.last().id);
        if (!last.isNull()) minX = last.right() + 1 + kTabGap;
    }
    return QRect(qMax(minX, width() - kRightMargin - kCollapseW), 0, kCollapseW, h);
}

QMenu* RibbonBar::overflowMenu() {
    ensureLayout();
    if (m_overflowMenu) m_overflowMenu->deleteLater();
    m_overflowMenu = new QMenu(this);
    const int tabIdx = tabIndex(m_currentTab);
    if (tabIdx < 0) return m_overflowMenu;
    for (const QString& pid : m_layout.hiddenPanels) {
        for (const RibbonPanelSpec& panel : m_spec[tabIdx].panels) {
            if (panel.id != pid) continue;
            QMenu* sub = m_overflowMenu->addMenu(panel.caption);
            for (const RibbonItemSpec& it : panel.items) {
                QAction* a = effectiveAction(it.id);
                if (!a || !a->isVisible()) continue;
                sub->addAction(a);
            }
        }
    }
    return m_overflowMenu;
}

void RibbonBar::showOverflowMenu() {
    QMenu* menu = overflowMenu();
    const QRect r = overflowButtonRect();
    // The ... item stays pressed while its menu is up.
    m_overflowOpen = true;
    connect(menu, &QMenu::aboutToHide, this, [this] { m_overflowOpen = false; update(); });
    update();
    menu->popup(mapToGlobal(QPoint(r.left(), r.bottom() + 1)));
}

// Qt's qt_strippedText: what QAction::toolTip() returns when nobody ever set
// one ("&Close Project" -> "Close Project").
static QString strippedActionText(QString s) {
    s.remove(QStringLiteral("..."));
    for (int i = 0; i < s.size(); ++i)
        if (s.at(i) == QLatin1Char('&')) s.remove(i, 1);
    return s.trimmed();
}

QString RibbonBar::tooltipFor(const QString& id) const {
    QAction* a = effectiveAction(id);
    const RibbonItemSpec* spec = itemSpec(id);
    QString tip = a ? a->toolTip() : QString();
    // An external action that never set a tooltip hands back Qt's echo of its
    // MENU text, which is a different name from the one on the strip ("Close"
    // the button vs "Close Project" the tooltip). That is not a tooltip, so
    // fall through to the spec — the one place a command's words live. An
    // external that really does carry its own tooltip still wins.
    if (a && spec && tip == strippedActionText(a->text())) tip.clear();
    if (tip.isEmpty() && spec) tip = spec->tooltip.isEmpty() ? spec->label : spec->tooltip;
    // ONE hint format everywhere: "<what it does> — Ctrl+D" — but a spec
    // tooltip that already names its shortcut ("Open a project (Ctrl+O)")
    // must not get it a second time.
    if (a && !a->shortcut().isEmpty()) {
        const QString sc = a->shortcut().toString(QKeySequence::NativeText);
        if (!tip.contains(sc, Qt::CaseInsensitive))
            tip += QStringLiteral(" — %1").arg(sc);
    }
    return tip;
}

// ── Events ──

void RibbonBar::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
    markLayoutDirty();
}

void RibbonBar::changeEvent(QEvent* e) {
    QWidget::changeEvent(e);
    if (e->type() == QEvent::FontChange || e->type() == QEvent::StyleChange) {
        markLayoutDirty();
        updateGeometry();
        update();
    }
}

void RibbonBar::updateHover(QPoint pos) {
    QString id;
    if (pos.y() < metrics().tabRowH) {
        if (collapseButtonRect().contains(pos)) id = QStringLiteral("collapse");
        else if (const QString t = tabIdAt(pos); !t.isEmpty()) id = QStringLiteral("tab:") + t;
    } else if (!m_minimized) {
        id = itemIdAt(pos);
        if (id.isEmpty() && overflowButtonRect().contains(pos)) id = QStringLiteral("overflow");
    }
    if (id != m_hoverId) {
        m_hoverId = id;
        refreshToolTip();
        dismissRcxTooltip();
        update();
    }
}

// The app's GlobalTooltipBridge (a qApp event filter) owns QEvent::ToolTip:
// it reads widget->toolTip() and shows / keeps the shared RcxTooltip
// idempotently for the same widget + text. Handling ToolTip here as well made
// the bridge dismiss (empty toolTip) and this widget re-show on every hover
// tick -- visible flicker. So the ribbon just keeps its toolTip property equal
// to the hovered item's text and lets the bridge (or QToolTip) show it.
void RibbonBar::refreshToolTip() {
    QString tip;
    if (m_hoverId == QLatin1String("overflow")) tip = QStringLiteral("More panels");
    else if (m_hoverId == QLatin1String("collapse"))
        tip = m_minimized ? QStringLiteral("Expand the ribbon (Ctrl+F1)")
                          : QStringLiteral("Collapse the ribbon (Ctrl+F1)");
    else if (!m_hoverId.isEmpty() && !m_hoverId.startsWith(QLatin1String("tab:"))) tip = tooltipFor(m_hoverId);
    if (tip != toolTip()) setToolTip(tip);
}

void RibbonBar::mouseMoveEvent(QMouseEvent* e) {
    updateHover(e->pos());
    QWidget::mouseMoveEvent(e);
}

void RibbonBar::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) { QWidget::mousePressEvent(e); return; }
    dismissRcxTooltip();
    const QPoint pos = e->pos();
    if (pos.y() < metrics().tabRowH) {
        // The chevron owns its cell — checked before the tabs so it works
        // even when a tab's rect grows into it.
        if (collapseButtonRect().contains(pos)) {
            setMinimized(!m_minimized);
            // setMinimized() cleared the hover id; re-derive it from the cursor
            // so the cell keeps its fill and immediately says "Expand …".
            updateHover(pos);
            e->accept();
            return;
        }
        const QString t = tabIdAt(pos);
        if (!t.isEmpty()) {
            setCurrentTab(t);
            if (m_minimized) {
                setMinimized(false);
                m_restoreTimer.start();
            }
        }
        e->accept();
        return;
    }
    if (m_minimized) { e->accept(); return; }
    if (overflowButtonRect().contains(pos)) {
        m_pressedId.clear();
        showOverflowMenu();
        e->accept();
        return;
    }
    const QString id = itemIdAt(pos);
    if (!id.isEmpty()) {
        m_pressedId = id;
        update();
    }
    e->accept();
}

void RibbonBar::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) { QWidget::mouseReleaseEvent(e); return; }
    if (!m_pressedId.isEmpty()) {
        const QString id = m_pressedId;
        m_pressedId.clear();
        update();
        if (itemIdAt(e->pos()) == id) {
            if (QAction* a = effectiveAction(id); a && a->isEnabled()) a->trigger();
        }
    }
    e->accept();
}

void RibbonBar::mouseDoubleClickEvent(QMouseEvent* e) {
    // The chevron already toggled on the press half; a second toggle here
    // would land back where it started.
    if (e->button() == Qt::LeftButton && collapseButtonRect().contains(e->pos())) {
        e->accept();
        return;
    }
    if (e->button() == Qt::LeftButton && e->pos().y() < metrics().tabRowH) {
        // A double-click on a tab while minimized is a "restore" gesture, not
        // a toggle. Qt may deliver it as Press (which already restored) …
        // DblClick — or, for synthetic clicks, as a lone DblClick — so:
        //  * still minimized → restore now (and switch to the clicked tab);
        //  * restored within the double-click interval (the press half of this
        //    very click pair did it) → leave it expanded;
        //  * otherwise → the usual toggle to minimized.
        const QString t = tabIdAt(e->pos());
        if (!t.isEmpty()) setCurrentTab(t);
        if (m_minimized) {
            setMinimized(false);
            m_restoreTimer.start();
        } else {
            const bool justRestored = m_restoreTimer.isValid()
                && m_restoreTimer.elapsed() <= QApplication::doubleClickInterval();
            if (!justRestored) setMinimized(true);
        }
        e->accept();
        return;
    }
    // A double-click on a button is two clicks: the second press was already
    // delivered as a press, so treat this like one to keep the pressed state.
    mousePressEvent(e);
}

void RibbonBar::leaveEvent(QEvent* e) {
    QWidget::leaveEvent(e);
    if (!m_hoverId.isEmpty()) { m_hoverId.clear(); refreshToolTip(); update(); }
    dismissRcxTooltip();
}

// ── Painting ──

void RibbonBar::paintEvent(QPaintEvent*) {
    ensureLayout();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);
    const Metrics m = metrics();
    const Theme& t = m_theme;

    // ONE surface for the whole widget (the host may hand us more height than
    // we asked for). The tab row used to be painted menuBarColor, which read
    // as a second menu strip stacked on the first; it is the ribbon's own
    // header, so it shares the ribbon's `background`. No panel boxes, no
    // caption bands either.
    p.fillRect(rect(), t.background);
    if (!m_minimized) paintBody(p, m);
    paintTabs(p, m);
}

void RibbonBar::paintTabs(QPainter& p, const Metrics& m) {
    const Theme& t = m_theme;
    p.setFont(font());

    // One full-width hairline on the strip's last device row (minimized: the
    // extra row under the strip is the only line). The active tab replaces
    // it with kUnderlineRows device rows of the accent -- bottom-underline
    // model, painted in ONE fill (two 1-row fills skip a row at 125 %).
    const QRectF strip(0, 0, width(), m_minimized ? m.tabRowH + 1 : m.tabRowH);
    fillBottomDeviceRowOfRect(p, strip, t.border);

    // Chrome text has three tiers and an inactive tab is the SECONDARY one:
    // textDim through the ladder, never textMuted-escalated-to-text.
    const QColor inactive = ribbonToneColour(t.textDim, t, t.background);
    for (const auto& tab : m_spec) {
        const QRect r = tabRect(tab.id);
        const bool active = (tab.id == m_currentTab);
        const bool hovered = (m_hoverId == QStringLiteral("tab:") + tab.id);
        if (active)
            fillBottomDeviceRowsOfRect(p, QRectF(r.left(), 0, r.width(), strip.height()),
                                       kUnderlineRows, t.indHoverSpan);
        // Text ladder (same as the doc tabs and the pane view tabs):
        // inactive textDim, hover text, active text. No fill, no box.
        p.setPen((active || hovered) ? t.text : inactive);
        p.drawText(r, Qt::AlignCenter, tab.title);
    }

    // Collapse chevron, painted exactly like the … overflow item: nothing at
    // rest, `hover` fill + `text` under the pointer. Up = expanded (click to
    // fold), down = minimized (click to unfold).
    {
        const QRect r = collapseButtonRect();
        const bool hovered = (m_hoverId == QLatin1String("collapse"));
        if (hovered) p.fillRect(r.adjusted(0, 0, 0, -1), t.hover);
        const QColor tone = hovered ? t.text : inactive;
        const QString path = m_minimized ? QStringLiteral(":/vsicons/chevron-down.svg")
                                         : QStringLiteral(":/vsicons/chevron-up.svg");
        const int side = 16;
        const QPixmap pm = tintedSvgIcon(path, tone, side, devicePixelRatioF());
        drawPixmapSnapped(p, QPointF(r.left() + (r.width() - side) / 2.0,
                                     r.top() + (r.height() - side) / 2.0), pm);
    }
}

void RibbonBar::paintBody(QPainter& p, const Metrics& m) {
    const Theme& t = m_theme;
    const int bodyTop = m.tabRowH;
    const int bodyBottom = bodyTop + m.bodyH;          // exclusive
    const int itemsTop = bodyTop + m.padTop;
    const int itemsH = 3 * m.rowH;
    const qreal dpr = devicePixelRatioF();

    // Panel dividers: one 1-device-px column per gap, 2 px clear of the strip
    // hairline above and the body hairline below (never a grid junction).
    const int divTop = bodyTop + 2;
    const int divBottom = bodyBottom - 1 - 2;
    for (int dx : m_layout.dividerXs)
        fillLeftDeviceColOfRect(p, QRectF(dx, divTop, 1, divBottom - divTop), t.border);

    const int tabIdx = tabIndex(m_currentTab);
    // A caption is the SECONDARY tier: textDim through the ladder, so it can
    // never end up as bright as the labels it captions.
    const QColor captionTone = ribbonToneColour(t.textDim, t, t.background);
    for (const LaidPanel& lp : m_layout.panels) {
        // `||` family separators span the rows only.
        for (int sx : lp.separatorXs)
            fillLeftDeviceColOfRect(p, QRectF(sx, itemsTop + 3, 1, itemsH - 6), t.border);
        p.setFont(captionFont());
        p.setPen(captionTone);
        if (!lp.groupCaptions.isEmpty()) {
            // Per-group captions replace the panel caption: "Type" told you
            // nothing the glyphs didn't, "Hex: / Int: / UInt: / Byte:" names
            // the column you are pointing at.
            for (const auto& gc : lp.groupCaptions)
                p.drawText(gc.first, Qt::AlignHCenter | Qt::AlignVCenter, gc.second);
            continue;
        }
        QString caption = lp.id;
        if (tabIdx >= 0)
            for (const auto& ps : m_spec[tabIdx].panels)
                if (ps.id == lp.id) { caption = ps.caption; break; }
        p.drawText(lp.captionRect, Qt::AlignHCenter | Qt::AlignVCenter, caption);
    }

    p.setFont(font());
    for (const LaidItem& li : m_layout.items) paintItem(p, li, m);

    if (!m_layout.hiddenPanels.isEmpty()) {
        // The ... item: a small button like any other (rest textDim, hover
        // `hover` fill + text, pressed `button` while the menu is open).
        const QRect r = m_layout.overflowRect;
        const bool hovered = (m_hoverId == QStringLiteral("overflow"));
        const bool pressed = m_overflowOpen;
        if (pressed) p.fillRect(r, pressedFill(t));
        else if (hovered) p.fillRect(r, t.hover);
        const QColor tone = (hovered || pressed) ? t.text : ribbonToneColour(t.textDim, t, t.background);
        const QPixmap pm = tintedSvgIcon(QStringLiteral(":/vsicons/ellipsis.svg"), tone, m.smallIcon, dpr);
        drawPixmapSnapped(p, QPointF(r.left() + (r.width() - m.smallIcon) / 2,
                                     r.top() + (r.height() - m.smallIcon) / 2), pm);
    }

    fillBottomDeviceRowOfRect(p, QRectF(0, bodyBottom - 1, width(), 1), t.border);
}

// States, in paint order: opacity (disabled: everything below dims together)
// -> fill (pressed `button` / hover `hover`, no outline) -> icon + label in
// the state tone -> checked underline. Rest paints nothing but icon + label.
//   rest      Plain icon + label `text` — the ribbon IS the actionable
//             surface; dimming it at rest inverted the hierarchy against the
//             document beside it. Family icons keep their hue.
//   hover     `hover` fill; ink stays `text` (the fill carries the state)
//   pressed   `button` fill (fallback `selected`)
//   checked   Plain icon + label indHoverSpan + 2 device rows underline
//   disabled  40 % opacity of the SAME ink (never textDim x 0.4, which lands
//             under 2:1), never hover / pressed
//   destructive (Delete)  icon markerPtr always; the label goes red only
//             under the pointer, so one red glyph marks it at rest
void RibbonBar::paintItem(QPainter& p, const LaidItem& li, const Metrics& m) {
    const Theme& t = m_theme;
    const RibbonItemSpec* spec = itemSpec(li.ref);
    if (!spec) return;
    QAction* a = effectiveAction(li.id);
    const bool enabled = !a || a->isEnabled();
    const bool checked = a && a->isCheckable() && a->isChecked();
    const bool hovered = (m_hoverId == li.id) && enabled;
    const bool pressed = hovered && (m_pressedId == li.id);
    const QRect r = li.rect;

    const qreal prevOpacity = p.opacity();
    if (!enabled) p.setOpacity(prevOpacity * kDisabledOpacity);

    if (pressed) p.fillRect(r, pressedFill(t));
    else if (hovered) p.fillRect(r, t.hover);

    const QColor tone = checked ? t.indHoverSpan
                      : (spec->destructive && (hovered || pressed)) ? t.markerPtr
                                                                    : t.text;
    const QColor iconInk = spec->destructive ? t.markerPtr : tone;
    RibbonIconOptions o;
    o.plainInk = iconInk;   // Plain icons only; family-tinted ones keep their colour
    o.labelled = li.label;  // Add / Insert draw their own count when unlabelled
    p.setPen(tone);
    p.setFont(font());
    const qreal dpr = devicePixelRatioF();
    const QFontMetrics fm(font());
    // A `menu` item ends in a ▾: the cell reserved 10 px for it.
    const int chevW = spec->menu ? kMenuChevronW : 0;
    const QRect cellR = r.adjusted(0, 0, -chevW, 0);
    const QPixmap chev = chevW ? ribbonMenuChevron(tone, dpr) : QPixmap();
    // Small items put the ▾ at the right edge; a Large item's ▾ rides with the
    // label on row 3 (parked at the cell's right edge it read as a stray mark
    // floating beside the icon).
    if (chevW && !li.large)
        drawPixmapSnapped(p, QPointF(r.right() + 1 - chevW + (chevW - kMenuChevron) / 2.0,
                                     r.top() + (r.height() - kMenuChevron) / 2.0), chev);

    if (li.large) {
        const QPixmap pm = ribbonIcon(spec->icon, RibbonIconSize::Large, dpr, t, o);
        const qreal iconL = m.largeIcon;   // logical, fractional at 125 % (32 device)
        if (li.label) {
            // The label sits on ROW 3 — the same baseline row as the third
            // small button of the column next door — with the icon centred in
            // rows 1-2. Centring the icon+label block instead floated the text
            // ~5 px above that line and broke the strip's only horizontal
            // rhythm.
            drawPixmapSnapped(p, QPointF(r.left() + (r.width() - iconL) / 2.0,
                                         r.top() + (2 * m.rowH - iconL) / 2.0), pm);
            // Elide only when the (integer) advance exceeds the budget: the
            // shaped run of a label that fits by the metric can be a fraction
            // wider, and elidedText would then trim it ("New Cla…").
            const QString& label = ribbonStripLabel(*spec);
            const int budget = cellR.width() - 8;
            const QString text = fm.horizontalAdvance(label) > budget
                ? fm.elidedText(label, Qt::ElideRight, budget) : label;
            // Label (+ ▾) centred as one block on row 3.
            const int textW = fm.horizontalAdvance(text);
            const int blockW = textW + (chevW ? 2 + kMenuChevron : 0);
            const int x0 = r.left() + (r.width() - blockW) / 2;
            const int labelTop = r.top() + 2 * m.rowH;
            p.drawText(QRect(x0, labelTop, textW, m.rowH),
                       Qt::AlignLeft | Qt::AlignVCenter | Qt::TextDontClip, text);
            if (chevW)
                drawPixmapSnapped(p, QPointF(x0 + textW + 2,
                                             labelTop + (m.rowH - kMenuChevron) / 2.0), chev);
        } else {
            drawPixmapSnapped(p, QPointF(cellR.left() + (cellR.width() - iconL) / 2.0,
                                         r.top() + (r.height() - iconL) / 2.0), pm);
        }
    } else {
        const QPixmap pm = ribbonIcon(spec->icon, RibbonIconSize::Small, dpr, t, o);
        const int cellW = ribbonIconCellWidth(spec->icon, li.label);
        const int iconY = r.top() + (r.height() - m.smallIcon) / 2;
        if (li.label) {
            drawPixmapSnapped(p, QPointF(cellR.left() + 3, iconY), pm);
            const QRect labelRect(cellR.left() + 3 + cellW + 4, r.top(),
                                  cellR.width() - (3 + cellW + 4) - 6, r.height());
            p.drawText(labelRect, Qt::AlignLeft | Qt::AlignVCenter, ribbonStripLabel(*spec));
        } else {
            drawPixmapSnapped(p, QPointF(cellR.left() + (cellR.width() - cellW) / 2, iconY), pm);
        }
    }
    if (checked) fillBottomDeviceRowsOfRect(p, r, kUnderlineRows, t.indHoverSpan);
    p.setOpacity(prevOpacity);
}


}  // namespace rcx
