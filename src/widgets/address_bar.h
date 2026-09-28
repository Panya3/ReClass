#pragma once
// rcx::AddressBar — the strip above the command row that answers Explorer's
// three questions for a class view: where am I reading from (the source
// chip), where in it (the base address and the drill-down trail) and where
// can I go (Back / Forward / history / Up, a chevron per level, recent
// places). It replaces BreadcrumbBar, which could only answer the middle one.
//
// ONE custom-painted QWidget on the RibbonBar template instead of a layout of
// QLabels and QToolButtons: per-item hit rects, hover fills, pressed and
// menu-open states, device-exact hairlines (a QSS border is two device rows
// at 125 %) and a single paintEvent — and no widget tree to tear down on
// every live-refresh tick. Intentionally no Q_OBJECT (mirrors EnumPickerPopup
// and the bar this replaces): outbound events are a struct of std::function
// callbacks that RcxEditor bridges to real signals, so this header does not
// enter every test target's AUTOMOC. It is still a QObject via QWidget, so
// the QMenu connects below work.
//
// State contract: the controller pushes one AddressBarState per refresh;
// setState() early-returns on operator== so an unchanged tick costs nothing.
// Geometry is a lazily recomputed Layout (paint and every query go through
// ensureLayout(); resize, font and state changes mark it dirty), and every
// cell is addressed by a string id — the surface the tests and the render
// harness drive:
//     back  fwd  hist  up | src  src.chev  root.chev  base
//     [overflow]  crumb:<i>  chev:<i> ...  space  recent
//
// Overflow, applied in this order until the strip fits: shrink the base
// (elide the formula, then drop its resolved-address suffix; floor 90 px),
// elide the source (floor 80 px), fold crumbs into « from the ROOT (never
// the deepest), middle-elide the deepest to 60 px, drop `up` and `hist`,
// drop the trailing chevron. Each knob turns only as far as the overshoot
// needs — a 30-px squeeze costs the base three characters, not half of it.
// Best effort after that — the deepest crumb is never dropped, because it
// is the one thing the bar exists to name.
//
// Chrome rules: the band is editorPaperColor (it sits inside the document
// column — paper, not a third chrome strip), the seam under it is one device
// row of containerBorderColor, hover is t.hover, pressed is pressedFill(t),
// tones walk text > textDim > textMuted > textFaint, and there is no accent
// anywhere on the bar — purple is spent on document tabs and real selection,
// not on a navigation aid.
//
// Tooltips: the widget's toolTip property mirrors the hovered cell and the
// app's GlobalTooltipBridge shows it. QEvent::ToolTip is never handled here
// and nothing is dismissed on a hover change (the bridge repositions on the
// next MouseMove); doing both strobed the ribbon's tips before it learned
// the same rule (src/ribbon.cpp refreshToolTip).
//
// Landed so far: layout, paint, crumbs, the « menu, tooltips, context menus
// (P2a); the source chip with its liveness dot and the popup click, and the
// base segment with its resolved-address suffix (P2b). The base and path
// edits, the chevron menus, history and keyboard mode land in the phases
// behind — their cells are laid out and painted now so the geometry is
// final, and their clicks are no-ops.

#include "core.h"
#include "paintutil.h"
#include "rcxtooltip.h"
#include "sourcechooserpopup.h"   // iconForProvider / kindLabelFor
#include "svgicon.h"
#include "tab_source_icon.h"
#include "themes/thememanager.h"
#include "address_bar_model.h"
#include "dock_header.h"          // chromeFont
#include "panel_search_field.h"   // kFieldHeight

#include <QAction>
#include <QClipboard>
#include <QColor>
#include <QContextMenuEvent>
#include <QFont>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QPoint>
#include <QRect>
#include <QResizeEvent>
#include <QSize>
#include <QString>
#include <QStringList>
#include <QVector>
#include <QWidget>
#include <functional>
#include <utility>

namespace rcx {

namespace address_bar_detail {

// Sensible colours when ThemeManager has nothing loaded (tests, harnesses
// run from a directory without themes/). The same VS2022 Dark values
// RibbonBar falls back to, so the two strips agree even in a bare target.
inline Theme fallbackTheme() {
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

inline QString hex(uint64_t v) {
    return QStringLiteral("0x") + QString::number(v, 16).toUpper();
}

// A crumb label is "Class.field" for an ancestor and a bare "Class" for the
// deepest; these split it back. Class names never carry a dot, so the first
// one is the seam.
inline QString classHalf(const QString& label) {
    const int d = label.indexOf(QLatin1Char('.'));
    return d < 0 ? label : label.left(d);
}
inline QString fieldHalf(const QString& label) {
    const int d = label.indexOf(QLatin1Char('.'));
    return d < 0 ? QString() : label.mid(d + 1);
}

}  // namespace address_bar_detail

class AddressBar : public QWidget {
public:
    // The bar is a field: the same height as the house text field beside it
    // (the panel filter boxes), so the two read as one control family.
    static constexpr int kAddressBarHeight = 26;
    static_assert(kAddressBarHeight == PanelSearchField::kFieldHeight,
                  "the address bar must match PanelSearchField's height");

    // Outbound events. RcxEditor bridges each to a real signal; the phase a
    // callback goes live in is noted — until then its cell is painted and
    // its click is a no-op.
    struct Callbacks {
        std::function<void(int)>           onCrumb;          // ancestor crumb / « pick → collapse to index
        std::function<void()>              onCurrentCrumb;   // the deepest crumb (inert; unwired for now)
        std::function<void(QPoint)>        onSourceClick;    // chip click — global anchor: the bar's bottom edge under the chip
        std::function<void(int, uint64_t)> onSiblingPick;    // P4 — chev:<level> menu pick
        std::function<void(uint64_t)>      onRootPick;       // P4 — root.chev menu pick
        std::function<void(QString)>       onBaseCommit;     // P3 — Enter in the base edit
        std::function<void(QString)>       onPathCommit;     // P4 — Enter in the path edit
        std::function<void(QString)>       onRecentPick;     // P3 — recent-places menu pick
        std::function<void()>              onBack;           // P5
        std::function<void()>              onForward;        // P5
        std::function<void()>              onUp;             // P5
        std::function<void(int)>           onHistoryJump;    // P5 — hist menu pick
        std::function<void()>              onRefresh;        // source context menu (P3)
    };

    explicit AddressBar(QWidget* parent = nullptr) : QWidget(parent) {
        setObjectName(QStringLiteral("rcxAddressBar"));
        // NoFocus at rest: a click on the bar must not steal focus from the
        // document (P5's keyboard mode takes StrongFocus only while it is on).
        setFocusPolicy(Qt::NoFocus);
        setMouseTracking(true);
        setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setFixedHeight(kAddressBarHeight);
        setAttribute(Qt::WA_OpaquePaintEvent, true);
        // The bar is chrome, so it wears the chrome face — the user's editor
        // family at 10 pt — not whatever font the parent happens to have.
        setFont(chromeFont());
        m_theme = ThemeManager::instance().current();
        if (!m_theme.background.isValid() || !m_theme.text.isValid())
            m_theme = address_bar_detail::fallbackTheme();
    }

    void setCallbacks(Callbacks cb) { m_cb = std::move(cb); }

    // One snapshot per refresh. Equal states are free; a changed one relays
    // out lazily. While the inline edit overlay is up (P3) the state is
    // stored but the relayout waits for commit / cancel, so a live tick can
    // never stomp what the user is typing.
    void setState(const AddressBarState& s) {
        if (s == m_state) return;
        m_state = s;
        ++m_stateApplyCount;
        if (m_editVisible) m_relayoutDeferred = true;
        else               markLayoutDirty();
        refreshToolTip();   // the hovered cell may have moved or gone
        update();
    }

    void applyTheme(const Theme& t) {
        m_theme = t;
        if (!m_theme.background.isValid() || !m_theme.text.isValid())
            m_theme = address_bar_detail::fallbackTheme();
        // A font change writes the setting and then re-themes, so this is
        // where the bar picks the new chrome face up (PanelSearchField does
        // the same). setFont → changeEvent(FontChange) → markLayoutDirty.
        setFont(chromeFont());
        update();
    }
    const Theme& theme() const { return m_theme; }

    // Liveness alone changes no geometry (the chip's dot and icon opacity),
    // so it bypasses the relayout the full state push would trigger. Fed by
    // the controller's sourceStatusChanged, so the dot turns the moment a
    // read fails rather than on the next refresh tick.
    void setLiveness(int liveness) {
        if (m_state.liveness == liveness) return;
        m_state.liveness = liveness;
        refreshToolTip();
        update();
    }

    // The source chooser is the one dropdown the bar does not own (the
    // controller opens the shared SourceChooserPopup), so the controller
    // says when it is up and when it went away; in between the chip reads
    // pressed, exactly like a cell with its own QMenu open.
    void setSourceMenuOpen(bool open) {
        const QString id = QStringLiteral("src");
        if (open) { if (m_menuOpenId == id) return; m_menuOpenId = id; }
        else      { if (m_menuOpenId != id) return; m_menuOpenId.clear(); }
        update();
    }

    // ── Geometry queries (the test + harness surface) ──
    QString itemIdAt(QPoint pos) const {
        ensureLayout();
        for (const LaidItem& li : m_layout.items)
            if (li.rect.contains(pos)) return li.id;
        return QString();
    }
    QRect itemRect(const QString& id) const {   // null when not laid out
        const LaidItem* li = itemById(id);
        return li ? li->rect : QRect();
    }

    // ── Test hooks ──
    // The rendered trail left→right: « when crumbs are folded behind it,
    // then the visible crumb labels (full, not the elided display text).
    QStringList segments() const {
        ensureLayout();
        QStringList out;
        if (m_layout.fold > 0) out << QStringLiteral("«");
        for (int i = m_layout.fold; i < m_state.crumbs.size(); ++i)
            out << m_state.crumbs[i].label;
        return out;
    }
    // The crumbs the « menu would list, top-down (root first).
    QStringList overflowMenuLabels() const {
        ensureLayout();
        QStringList out;
        for (int i = 0; i < m_layout.fold && i < m_state.crumbs.size(); ++i)
            out << m_state.crumbs[i].label;
        return out;
    }
    const AddressBarState& state() const { return m_state; }
    int stateApplyCount() const { return m_stateApplyCount; }

    // The dotted path down to crumb i — "RcxEditor.vptr.parent" for the third
    // crumb of "RcxEditor.vptr › QWidgetPrivate.parent › QWidget": the class
    // half of the root crumb, then the field half of every crumb above i.
    // Built from the labels the controller emitted, so it agrees with
    // trailPathText() without the bar knowing the tree. What "Copy path"
    // puts on the clipboard.
    QString crumbPathText(int i) const {
        using namespace address_bar_detail;
        if (m_state.crumbs.isEmpty()) return QString();
        QString out = classHalf(m_state.crumbs[0].label);
        for (int k = 0; k < i && k < m_state.crumbs.size(); ++k) {
            const QString f = fieldHalf(m_state.crumbs[k].label);
            if (!f.isEmpty()) out += QLatin1Char('.') + f;
        }
        return out;
    }

    // What the chip and the base segment actually DRAW — elided to their
    // width budgets — as opposed to the full strings in state(). The bug
    // these pin: the command row used to elide a long formula for display
    // and then edit (and evaluate) the elided form, ellipsis and all.
    QString sourceDisplayText() const {
        const LaidItem* li = itemById(QStringLiteral("src"));
        return li ? li->text : QString();
    }
    QString baseDisplayText() const {
        const LaidItem* li = itemById(QStringLiteral("base"));
        return li ? li->text : QString();
    }
    // The chip's icon square and its liveness dot, logical px. The dot rect
    // is null when nothing is painted there (static file, no source).
    QRect sourceIconRect() const {
        const LaidItem* li = itemById(QStringLiteral("src"));
        return li ? chipIconRect(li->rect) : QRect();
    }
    QRect livenessDotRect() const {
        const QRect icon = sourceIconRect();
        return (icon.isNull() || !livenessDotColor().isValid()) ? QRect() : dotRectFor(icon);
    }

    // ── Metrics (logical px; the strip is 26 tall, cells 22 tall at y 2) ──
    static constexpr int kNavBtnW      = 22;
    static constexpr int kHistW        = 14;
    static constexpr int kNavGap       = 2;
    static constexpr int kNavIconPx    = 14;
    static constexpr int kChevIconPx   = 12;
    static constexpr int kDividerPad   = 6;    // either side of the field divider
    static constexpr int kChipPad      = 4;
    static constexpr int kChipIconPx   = 16;
    static constexpr int kChipTextGap  = 4;
    static constexpr int kChipChevGap  = 2;
    static constexpr int kDotPx        = 6;    // liveness dot on the chip icon
    static constexpr int kDotRing      = 1;    // paper ring that lifts it off the icon
    static constexpr int kChevW        = 14;   // every chevron cell
    static constexpr int kSrcMaxW      = 160;
    static constexpr int kSrcMinW      = 80;
    static constexpr int kBasePad      = 6;
    static constexpr int kBaseMaxW     = 200;
    static constexpr int kBaseMinW     = 90;
    static constexpr int kCrumbPad     = 6;
    static constexpr int kDeepestMinW  = 60;
    static constexpr int kOverflowW    = 18;
    static constexpr int kRecentW      = 16;
    static constexpr int kRightMargin  = 6;
    static constexpr int kCellTop      = 2;
    static constexpr int kCellH        = 22;
    static constexpr double kDisabledOpacity = 0.40;

protected:
    void paintEvent(QPaintEvent*) override {
        ensureLayout();
        const Theme& t = m_theme;
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing, false);
        p.fillRect(rect(), editorPaperColor(t));
        // The field divider: one device column between the nav cluster and
        // the field, inset 4 px top and bottom so it reads as a separator
        // inside the strip rather than a cut through it.
        if (m_layout.dividerX >= 0)
            fillLeftDeviceColOfRect(p, QRectF(m_layout.dividerX, 4, 1, kAddressBarHeight - 8),
                                    containerBorderColor(t));
        for (const LaidItem& li : m_layout.items) paintItem(p, li);
        // The seam: one device row, never a QSS border. P3 turns the part
        // under the field borderFocused while an edit is open.
        fillBottomDeviceRowOfRect(p, QRectF(rect()), containerBorderColor(t));
    }

    void resizeEvent(QResizeEvent* e) override {
        QWidget::resizeEvent(e);
        markLayoutDirty();
    }

    void changeEvent(QEvent* e) override {
        QWidget::changeEvent(e);
        if (e->type() == QEvent::FontChange || e->type() == QEvent::StyleChange)
            markLayoutDirty();
    }

    void mouseMoveEvent(QMouseEvent* e) override {
        updateHover(e->pos());
        QWidget::mouseMoveEvent(e);
    }

    void mousePressEvent(QMouseEvent* e) override {
        if (e->button() != Qt::LeftButton) { QWidget::mousePressEvent(e); return; }
        dismissRcxTooltip();
        const QString id = itemIdAt(e->pos());
        if (id == QLatin1String("overflow")) {
            // Menus open on press (like every other dropdown in the app);
            // the cell shows pressed through m_menuOpenId until it hides.
            m_pressedId.clear();
            showOverflowMenu();
            e->accept();
            return;
        }
        if (!id.isEmpty()) {
            m_pressedId = id;
            update();
        }
        e->accept();
    }

    // Press-then-release on the SAME cell is a click; dragging off cancels.
    void mouseReleaseEvent(QMouseEvent* e) override {
        if (e->button() != Qt::LeftButton) { QWidget::mouseReleaseEvent(e); return; }
        if (!m_pressedId.isEmpty()) {
            const QString id = m_pressedId;
            m_pressedId.clear();
            update();
            if (itemIdAt(e->pos()) == id) activate(id);
        }
        e->accept();
    }

    void leaveEvent(QEvent* e) override {
        QWidget::leaveEvent(e);
        if (!m_hoverId.isEmpty()) {
            m_hoverId.clear();
            refreshToolTip();
            unsetCursor();
            update();
        }
        dismissRcxTooltip();
    }

    void contextMenuEvent(QContextMenuEvent* e) override {
        using namespace address_bar_detail;
        const QString id = itemIdAt(e->pos());
        const LaidItem* li = itemById(id);
        if (!li) { e->ignore(); return; }
        auto copy = [](const QString& s) { QGuiApplication::clipboard()->setText(s); };
        QMenu menu(this);
        switch (li->kind) {
        case Cell::Crumb: {
            const int i = li->index;
            const Crumb& c = m_state.crumbs[i];
            menu.addAction(QStringLiteral("Copy path"), this,
                           [this, i, copy] { copy(crumbPathText(i)); });
            QAction* addr = menu.addAction(QStringLiteral("Copy address"), this,
                                           [c, copy] { copy(hex(c.address)); });
            addr->setEnabled(c.address != 0 || i == 0);   // the root is base, never "unknown"
            menu.addAction(QStringLiteral("Copy class name"), this,
                           [c, copy] { copy(classHalf(c.label)); });
            break;
        }
        case Cell::Base: {
            menu.addAction(QStringLiteral("Copy address"), this,
                           [this, copy] { copy(hex(m_state.resolvedBase ? m_state.resolvedBase
                                                                        : m_state.baseAddress)); });
            QAction* formula = menu.addAction(QStringLiteral("Copy formula"), this,
                                              [this, copy] { copy(m_state.baseFormula); });
            formula->setEnabled(!m_state.baseFormula.isEmpty());
            break;
        }
        case Cell::Src:
        case Cell::SrcChev: {
            QAction* name = menu.addAction(QStringLiteral("Copy source name"), this,
                                           [this, copy] { copy(m_state.sourceName); });
            name->setEnabled(!m_state.sourceName.isEmpty());
            break;
        }
        default:
            e->ignore();
            return;
        }
        dismissRcxTooltip();
        m_menuOpenId = id;
        update();
        menu.exec(e->globalPos());
        m_menuOpenId.clear();
        update();
        e->accept();
    }

private:
    enum class Cell { Back, Fwd, Hist, Up, Src, SrcChev, RootChev, Base,
                      Crumb, Chev, Overflow, Space, Recent };

    struct LaidItem {
        QString id;
        Cell    kind = Cell::Space;
        int     index = -1;       // crumb index for Crumb / Chev
        QRect   rect;
        bool    enabled = true;
        QString text;             // display text (possibly elided) for text cells
        QString text2;            // base only: the "  → 0x…" resolved-address suffix
    };
    struct Layout {
        QVector<LaidItem> items;
        int dividerX = -1;        // the field divider column, logical x
        int fold     = 0;         // crumbs [0, fold) are behind «
        int width    = 0;         // width the strip needs with an empty `space`
        int forWidth = -1;        // widget width this layout was computed for
    };
    // The knobs the overflow rule turns, in the order it turns them.
    struct Budget {
        int  baseMaxW      = kBaseMaxW;
        bool showResolved  = true;   // the base's "  → 0x…" suffix; goes with step 1
        int  srcMaxW       = kSrcMaxW;
        int  fold          = 0;
        int  deepestMaxW   = 0;   // 0 = never elide the deepest
        bool dropUpHist    = false;
        bool dropTrailChev = false;
    };

    // ── Layout ──

    void ensureLayout() const {
        // resize() on a hidden widget delivers no resizeEvent, so the dirty
        // flag alone cannot be trusted — the cache also remembers the width
        // it was computed for (RibbonBar's rule).
        if (m_layoutDirty || m_layout.forWidth != width()) relayout();
    }
    void markLayoutDirty() { m_layoutDirty = true; }

    QFont deepestFont() const {
        QFont f = font();
        f.setWeight(QFont::DemiBold);   // the single weight step on this surface
        return f;
    }

    QString sourceFullText() const {
        return m_state.sourceName.isEmpty() ? QStringLiteral("Select source") : m_state.sourceName;
    }
    QString baseFullText() const {
        return m_state.baseFormula.isEmpty() ? address_bar_detail::hex(m_state.baseAddress)
                                             : m_state.baseFormula;
    }
    // What a formula resolves to right now, one tone down beside it. Only
    // for a formula — beside a literal it would repeat the number.
    QString baseSuffixText() const {
        return m_state.baseFormula.isEmpty()
            ? QString()
            : QStringLiteral("  \u2192 ") + address_bar_detail::hex(m_state.resolvedBase);
    }

    Layout computeLayout(const Budget& b) const {
        Layout L;
        L.fold = b.fold;
        const QFontMetrics fm(font());
        const QFontMetrics fmDeep(deepestFont());
        // Elide only when the (integer) advance exceeds the budget: elidedText
        // on a string that fits by the metric can still trim it.
        auto elide = [](const QFontMetrics& f, const QString& s, int maxW) {
            return f.horizontalAdvance(s) > maxW ? f.elidedText(s, Qt::ElideMiddle, maxW) : s;
        };
        int x = kGutter;   // spent once: the first ink is the Back icon
        auto add = [&](const QString& id, Cell kind, int index, int w, bool enabled,
                       const QString& text = QString()) {
            LaidItem li;
            li.id = id; li.kind = kind; li.index = index;
            li.rect = QRect(x, kCellTop, w, kCellH);
            li.enabled = enabled; li.text = text;
            L.items.push_back(li);
            x += w;
        };

        // Nav cluster. Disabled cells stay laid out (Back/Forward always,
        // history + Up unless the strip is too narrow) so the field never
        // shifts left when history appears.
        add(QStringLiteral("back"), Cell::Back, -1, kNavBtnW, m_state.canBack);
        x += kNavGap;
        add(QStringLiteral("fwd"), Cell::Fwd, -1, kNavBtnW, m_state.canForward);
        if (!b.dropUpHist) {
            x += kNavGap;
            add(QStringLiteral("hist"), Cell::Hist, -1, kHistW, m_state.canBack || m_state.canForward);
            x += kNavGap;
            add(QStringLiteral("up"), Cell::Up, -1, kNavBtnW, m_state.canUp);
        }

        // Field divider — everything right of it is "the field".
        x += kDividerPad;
        L.dividerX = x;
        x += 1 + kDividerPad;

        // Source chip: icon + name, then its chevron. Two cells (so the
        // chevron can grow its own menu later) painted as ONE hover group.
        const QString srcText = elide(fm, sourceFullText(), b.srcMaxW);
        add(QStringLiteral("src"), Cell::Src, -1,
            kChipPad + kChipIconPx + kChipTextGap + fm.horizontalAdvance(srcText) + kChipChevGap,
            true, srcText);
        add(QStringLiteral("src.chev"), Cell::SrcChev, -1, kChevW, true);
        x += kChipPad;

        add(QStringLiteral("root.chev"), Cell::RootChev, -1, kChevW, true);

        // Base: the formula if one is set, else the literal, then the
        // address the formula resolves to. Elided for DISPLAY only — the
        // state carries the full string and the edit (P3) opens on that,
        // never on the elided text (the command row fed its ellipsis to
        // the parser and silently no-op'd).
        const QString baseText   = elide(fm, baseFullText(), b.baseMaxW);
        const QString baseSuffix = b.showResolved ? baseSuffixText() : QString();
        add(QStringLiteral("base"), Cell::Base, -1,
            kBasePad + fm.horizontalAdvance(baseText) + fm.horizontalAdvance(baseSuffix) + kBasePad,
            true, baseText);
        L.items.last().text2 = baseSuffix;

        // Crumbs, « in front of the run when folded, a chevron after each
        // (the trailing one drills further; it is the last thing dropped).
        const int n = m_state.crumbs.size();
        if (b.fold > 0) add(QStringLiteral("overflow"), Cell::Overflow, -1, kOverflowW, true);
        for (int i = b.fold; i < n; ++i) {
            const bool deepest = (i == n - 1);
            const QFontMetrics& f = (deepest && n > 1) ? fmDeep : fm;
            QString label = m_state.crumbs[i].label;
            if (deepest && b.deepestMaxW > 0) label = elide(f, label, b.deepestMaxW);
            add(QStringLiteral("crumb:%1").arg(i), Cell::Crumb, i,
                kCrumbPad + f.horizontalAdvance(label) + kCrumbPad, true, label);
            if (!deepest || !b.dropTrailChev)
                add(QStringLiteral("chev:%1").arg(i), Cell::Chev, i, kChevW, true);
        }

        // The stretch, then `recent` pinned to the right edge — or pushed
        // past it when nothing fits; the strip never drops a crumb to make
        // room for a menu button.
        L.width = x + kRecentW + kRightMargin;
        const int recentX = qMax(x, width() - kRightMargin - kRecentW);
        if (recentX > x) add(QStringLiteral("space"), Cell::Space, -1, recentX - x, true);
        x = recentX;
        add(QStringLiteral("recent"), Cell::Recent, -1, kRecentW, true);
        L.forWidth = width();
        return L;
    }

    // The overflow rule: one knob per step, in order, until it fits. The two
    // text knobs turn only as far as the current overshoot needs, measured
    // on the text as laid out (elision lands a glyph or so short of its
    // budget, so a turn is re-taken up to three times before the floor —
    // never a jump to the floor for a few px).
    void relayout() const {
        m_layoutDirty = false;
        const int avail = width();
        const QFontMetrics fm(font());
        Budget b;
        Layout L = computeLayout(b);
        auto fits = [&] { return L.width <= avail; };
        auto over = [&] { return L.width - avail; };
        auto laidTextW = [&](const char* id) {
            for (const LaidItem& li : L.items)
                if (li.id == QLatin1String(id)) return fm.horizontalAdvance(li.text);
            return 0;
        };
        if (fits()) { m_layout = L; return; }
        // 1. The base: elide the formula by the overshoot; when its floor
        //    cannot absorb it, drop the resolved-address suffix instead and
        //    give the formula back whatever that freed beyond the need.
        for (int turn = 0; turn < 3 && !fits(); ++turn) {
            const int cur     = laidTextW("base");
            const int suffixW = b.showResolved ? fm.horizontalAdvance(baseSuffixText()) : 0;
            if (cur - over() >= kBaseMinW) {
                b.baseMaxW = cur - over();
            } else if (suffixW > 0) {
                b.showResolved = false;
                b.baseMaxW = qMax(kBaseMinW, cur - qMax(0, over() - suffixW));
            } else if (cur > kBaseMinW) {
                b.baseMaxW = kBaseMinW;
            } else {
                break;   // already at the floor
            }
            L = computeLayout(b);
        }
        if (fits()) { m_layout = L; return; }
        b.baseMaxW = kBaseMinW; b.showResolved = false;
        L = computeLayout(b); if (fits()) { m_layout = L; return; }
        // 2. The source name, the same way.
        for (int turn = 0; turn < 3 && !fits(); ++turn) {
            const int cur = laidTextW("src");
            if (cur <= kSrcMinW) break;
            b.srcMaxW = qMax(kSrcMinW, cur - over());
            L = computeLayout(b);
        }
        if (fits()) { m_layout = L; return; }
        b.srcMaxW = kSrcMinW;
        L = computeLayout(b); if (fits()) { m_layout = L; return; }
        for (int f = 1; f < m_state.crumbs.size(); ++f) {   // never the deepest
            b.fold = f;
            L = computeLayout(b); if (fits()) { m_layout = L; return; }
        }
        b.deepestMaxW = kDeepestMinW;
        L = computeLayout(b); if (fits()) { m_layout = L; return; }
        b.dropUpHist = true;
        L = computeLayout(b); if (fits()) { m_layout = L; return; }
        b.dropTrailChev = true;
        L = computeLayout(b);
        m_layout = L;   // still too wide — best effort
    }

    const LaidItem* itemById(const QString& id) const {
        ensureLayout();
        for (const LaidItem& li : m_layout.items)
            if (li.id == id) return &li;
        return nullptr;
    }

    bool isDeepest(const LaidItem& li) const {
        return li.kind == Cell::Crumb && li.index == m_state.crumbs.size() - 1;
    }

    // The chip and its chevron light up together.
    static bool sameGroup(const QString& a, const QString& b) {
        auto chip = [](const QString& s) {
            return s == QLatin1String("src") || s == QLatin1String("src.chev");
        };
        return a == b || (chip(a) && chip(b));
    }

    // ── Painting ──

    void drawIcon(QPainter& p, const QRect& cell, const char* path, int px, const QColor& tint) const {
        const qreal dpr = devicePixelRatioF();
        const QPixmap pm = themedVsIcon(QString::fromLatin1(path), tint, px, dpr)
                               .pixmap(QSize(px, px), dpr);
        drawPixmapSnapped(p, QPointF(cell.left() + (cell.width() - px) / 2.0,
                                     cell.top() + (cell.height() - px) / 2.0), pm);
    }

    // The chip's 16-px icon square: after the pad, centred in the cell.
    static QRect chipIconRect(const QRect& cell) {
        return QRect(cell.left() + kChipPad, cell.top() + (cell.height() - kChipIconPx) / 2,
                     kChipIconPx, kChipIconPx);
    }
    // The liveness dot rides the icon's bottom-right corner, 2 px over the
    // edge, so it reads as a badge ON the icon and not as a fourth glyph in
    // the row (the 2 px land in the icon-to-name gap, which stays clear).
    static QRect dotRectFor(const QRect& icon) {
        return QRect(icon.right() + 3 - kDotPx, icon.bottom() + 3 - kDotPx, kDotPx, kDotPx);
    }
    // The status-bar chip's mapping, so the two never disagree: green while
    // reads succeed, focusGlow while they fail, markerError when the source
    // is gone — and nothing for a static file or no source, where "alive"
    // has no meaning.
    QColor livenessDotColor() const {
        if (m_state.sourceName.isEmpty()) return QColor();
        switch (m_state.liveness) {
        case liveness::Live:         return m_theme.indHintGreen;
        case liveness::Stale:        return m_theme.focusGlow;
        case liveness::Disconnected: return m_theme.markerError;
        default:                     return QColor();
        }
    }

    void paintItem(QPainter& p, const LaidItem& li) const {
        const Theme& t = m_theme;
        const bool hovered = li.enabled && !m_hoverId.isEmpty() && sameGroup(m_hoverId, li.id);
        const bool pressed = li.enabled
            && ((hovered && !m_pressedId.isEmpty() && sameGroup(m_pressedId, li.id))
                || (!m_menuOpenId.isEmpty() && sameGroup(m_menuOpenId, li.id)));
        const QRect r = li.rect;
        const bool deepest = isDeepest(li);
        const qreal prevOpacity = p.opacity();
        // Disabled = the whole cell at 40 %, never textDim × 0.4.
        if (!li.enabled) p.setOpacity(prevOpacity * kDisabledOpacity);

        // Fill: the deepest crumb is inert (you are here — nothing to do),
        // the base is a text field (IBeam, no fill) and the stretch is air.
        const bool fillable = li.kind != Cell::Space && li.kind != Cell::Base && !deepest;
        if (fillable) {
            if (pressed)      p.fillRect(r, pressedFill(t));
            else if (hovered) p.fillRect(r, t.hover);
        }

        p.setFont(font());
        const int textFlags = Qt::AlignLeft | Qt::AlignVCenter | Qt::TextSingleLine;
        switch (li.kind) {
        case Cell::Back:
            drawIcon(p, r, ":/vsicons/arrow-left.svg", kNavIconPx, hovered ? t.text : t.textDim);
            break;
        case Cell::Fwd:
            drawIcon(p, r, ":/vsicons/arrow-right.svg", kNavIconPx, hovered ? t.text : t.textDim);
            break;
        case Cell::Up:
            drawIcon(p, r, ":/vsicons/arrow-up.svg", kNavIconPx, hovered ? t.text : t.textDim);
            break;
        case Cell::Hist:
            drawIcon(p, r, ":/vsicons/chevron-down.svg", kChevIconPx, hovered ? t.text : t.textDim);
            break;
        case Cell::Src: {
            // Explorer's root icon: the provider's icon, dimmed to 40 % when
            // the source is gone, with the liveness dot on its corner.
            const QRect icon = chipIconRect(r);
            const QString path = m_state.sourceName.isEmpty()
                ? QStringLiteral(":/vsicons/plug.svg") : iconForProvider(m_state.sourceKindId);
            drawTabSourceIcon(&p, icon, path, m_state.liveness != liveness::Disconnected, t.textDim);
            const QColor dot = livenessDotColor();
            if (dot.isValid()) {
                // A one-px ring of paper under the dot lifts it off the
                // icon's ink. The dot is the one round thing on the bar —
                // a badge, not a box.
                const QRect d = dotRectFor(icon);
                p.setRenderHint(QPainter::Antialiasing, true);
                p.setPen(Qt::NoPen);
                p.setBrush(editorPaperColor(t));
                p.drawEllipse(d.adjusted(-kDotRing, -kDotRing, kDotRing, kDotRing));
                p.setBrush(dot);
                p.drawEllipse(d);
                p.setBrush(Qt::NoBrush);
                p.setRenderHint(QPainter::Antialiasing, false);
            }
            p.setPen(hovered ? t.text : t.textDim);
            p.drawText(QRect(icon.right() + 1 + kChipTextGap, r.top(), r.width(), r.height()),
                       textFlags, li.text);
            break;
        }
        case Cell::SrcChev:
            drawIcon(p, r, ":/vsicons/chevron-down.svg", kChevIconPx, hovered ? t.textDim : t.textFaint);
            break;
        case Cell::RootChev:
        case Cell::Chev:
            // Explorer's › → ˅ flip: the chevron turns down under the pointer
            // to say "this opens a list", and steps up one tone to be read.
            drawIcon(p, r, hovered ? ":/vsicons/chevron-down.svg" : ":/vsicons/chevron-right.svg",
                     kChevIconPx, hovered ? t.textDim : t.textFaint);
            break;
        case Cell::Base: {
            // The formula (or literal) in the field tone, then what it
            // resolves to one rung down — until now a number only the Goto
            // dialog would show.
            const QRect tr = r.adjusted(kBasePad, 0, -kBasePad, 0);
            p.setPen(hovered ? t.text : t.textDim);
            p.drawText(tr, textFlags, li.text);
            if (!li.text2.isEmpty()) {
                p.setPen(t.textMuted);
                p.drawText(tr.adjusted(p.fontMetrics().horizontalAdvance(li.text), 0, 0, 0),
                           textFlags, li.text2);
            }
            break;
        }
        case Cell::Crumb: {
            // Tone ladder: a lone crumb only repeats what the doc tab and the
            // command row already name, so it stays textDim and regular. From
            // depth 2 the trail is navigation — ancestors textDim (text under
            // the pointer: a link cue, not the accent), the deepest text at
            // DemiBold, and the deepest ignores hover because it is inert.
            const bool lone = (m_state.crumbs.size() == 1);
            const bool primary = deepest && !lone;
            if (primary) p.setFont(deepestFont());
            p.setPen(primary ? t.text : (hovered && !deepest) ? t.text : t.textDim);
            p.drawText(r.adjusted(kCrumbPad, 0, -kCrumbPad, 0), textFlags, li.text);
            break;
        }
        case Cell::Overflow:
            p.setPen(hovered ? t.text : t.textDim);
            p.drawText(r, Qt::AlignCenter | Qt::TextSingleLine, QStringLiteral("«"));
            break;
        case Cell::Space:
            break;
        case Cell::Recent:
            drawIcon(p, r, ":/vsicons/chevron-down.svg", kChevIconPx, hovered ? t.textDim : t.textFaint);
            break;
        }
        p.setOpacity(prevOpacity);
    }

    // ── Hover / tooltip / cursor ──

    void updateHover(const QPoint& pos) {
        const QString id = itemIdAt(pos);
        if (id == m_hoverId) return;
        m_hoverId = id;
        // Publish the new cell's text and stop. NO dismiss here: once a tip
        // is up Qt re-arms its wake-up at ~20 ms, so dismissing on every
        // hovered-cell change strobed the ribbon's tooltip as the cursor
        // crossed a row of buttons. The bridge repositions on the next move.
        refreshToolTip();
        updateCursor();
        update();
    }

    void refreshToolTip() {
        const QString tip = m_hoverId.isEmpty() ? QString() : tooltipFor(m_hoverId);
        if (tip != toolTip()) setToolTip(tip);
    }

    void updateCursor() {
        const LaidItem* li = itemById(m_hoverId);
        Qt::CursorShape shape = Qt::ArrowCursor;
        if (li) {
            switch (li->kind) {
            case Cell::Base:
            case Cell::Space:    shape = Qt::IBeamCursor; break;          // click-to-type
            case Cell::Crumb:    shape = isDeepest(*li) ? Qt::ArrowCursor
                                                        : Qt::PointingHandCursor; break;
            case Cell::Back: case Cell::Fwd: case Cell::Hist: case Cell::Up:
                shape = li->enabled ? Qt::PointingHandCursor : Qt::ArrowCursor; break;
            default:             shape = Qt::PointingHandCursor; break;
            }
        }
        if (shape == Qt::ArrowCursor) unsetCursor();
        else                          setCursor(shape);
    }

    QString sourceTooltip() const {
        if (m_state.sourceName.isEmpty())
            return QStringLiteral("Select source  ·  click to choose one");
        // Same words as the status-bar chip, so the two never disagree.
        QString live;
        switch (m_state.liveness) {
        case liveness::Live:         live = QStringLiteral("Live — reading"); break;
        case liveness::Stale:        live = QStringLiteral("Stale — read failing"); break;
        case liveness::Disconnected: live = QStringLiteral("Disconnected"); break;
        case liveness::Static:       live = QStringLiteral("Static file"); break;
        default: break;
        }
        QString s = kindLabelFor(m_state.sourceKindId) + QLatin1Char(' ') + m_state.sourceName;
        if (!live.isEmpty()) s += QStringLiteral("  ·  ") + live;
        return s + QStringLiteral("  ·  click to change source");
    }

    QString tooltipFor(const QString& id) const {
        using namespace address_bar_detail;
        const LaidItem* li = itemById(id);
        if (!li) return QString();
        switch (li->kind) {
        case Cell::Back:     return QStringLiteral("Back  Alt+Left");
        case Cell::Fwd:      return QStringLiteral("Forward  Alt+Right");
        case Cell::Hist:     return QStringLiteral("Recent locations");
        case Cell::Up:       return QStringLiteral("Up one level  Alt+Up");
        case Cell::Src:
        case Cell::SrcChev:  return sourceTooltip();
        case Cell::RootChev: return QStringLiteral("Other classes");
        case Cell::Base: {
            // The full formula lives here when the segment had to elide it.
            const QString addr = hex(m_state.resolvedBase ? m_state.resolvedBase : m_state.baseAddress);
            const QString head = m_state.baseFormula.isEmpty()
                ? QStringLiteral("Base address  %1").arg(addr)
                : QStringLiteral("Base address  %1  \u2192  %2").arg(m_state.baseFormula, addr);
            return head + QStringLiteral("\nclick to edit · Down for recent · Ctrl+G go to");
        }
        case Cell::Crumb: {
            // Where that class sits in memory — invisible everywhere else
            // once you are two pointers deep. The root's address is the base,
            // which is never "unknown".
            const Crumb& c = m_state.crumbs[li->index];
            return (c.address != 0 || li->index == 0)
                ? QStringLiteral("%1  @ %2").arg(c.label, hex(c.address))
                : QStringLiteral("%1  @ (unreadable)").arg(c.label);
        }
        case Cell::Chev:
            return QStringLiteral("Fields of %1").arg(classHalf(m_state.crumbs[li->index].label));
        case Cell::Overflow:
            return overflowMenuLabels().join(QStringLiteral(" › "));
        case Cell::Space:    return QStringLiteral("Edit the path  Alt+D");
        case Cell::Recent:   return QStringLiteral("Recent addresses and bookmarks");
        }
        return QString();
    }

    // ── Activation ──

    void activate(const QString& id) {
        const LaidItem* li = itemById(id);
        if (!li || !li->enabled) return;
        switch (li->kind) {
        case Cell::Crumb:
            if (isDeepest(*li)) { if (m_cb.onCurrentCrumb) m_cb.onCurrentCrumb(); }
            else if (m_cb.onCrumb) m_cb.onCrumb(li->index);
            break;
        case Cell::Back:    if (m_cb.onBack)    m_cb.onBack();    break;
        case Cell::Fwd:     if (m_cb.onForward) m_cb.onForward(); break;
        case Cell::Up:      if (m_cb.onUp)      m_cb.onUp();      break;
        case Cell::Src:
        case Cell::SrcChev: {
            // The anchor is the bar's bottom edge under the chip, not the
            // cell's bottom: the popup hangs below the seam instead of
            // covering it. The controller marks the chip pressed while the
            // popup is up (setSourceMenuOpen) — it, not the bar, knows when
            // the popup closed.
            const QRect chip = itemRect(QStringLiteral("src"));
            if (m_cb.onSourceClick) m_cb.onSourceClick(mapToGlobal(QPoint(chip.left(), height())));
            break;
        }
        // P3: base → beginBaseEdit, recent menu. P4: root.chev / chev:<i>
        // menus, space → beginPathEdit. P5: hist menu. Painted now, inert
        // until then.
        default: break;
        }
    }

    void showOverflowMenu() {
        const QRect r = itemRect(QStringLiteral("overflow"));
        if (r.isNull()) return;
        // Built per open and freed when it hides: the hidden set changes with
        // every fold, so there is nothing worth caching.
        auto* menu = new QMenu(this);
        for (int i = 0; i < m_layout.fold && i < m_state.crumbs.size(); ++i) {
            QAction* a = menu->addAction(m_state.crumbs[i].label);
            connect(a, &QAction::triggered, this, [this, i] { if (m_cb.onCrumb) m_cb.onCrumb(i); });
        }
        // The « cell stays pressed while its menu is up.
        m_menuOpenId = QStringLiteral("overflow");
        connect(menu, &QMenu::aboutToHide, this, [this, menu] {
            m_menuOpenId.clear();
            update();
            menu->deleteLater();
        });
        update();
        menu->popup(mapToGlobal(QPoint(r.left(), r.bottom() + 1)));
    }

    // ── State ──

    Callbacks       m_cb;
    AddressBarState m_state;
    Theme           m_theme;
    int             m_stateApplyCount = 0;

    mutable Layout  m_layout;
    mutable bool    m_layoutDirty = true;

    QString m_hoverId;
    QString m_pressedId;
    QString m_menuOpenId;      // cell kept pressed while its menu (or the source popup) is up
    // P3: the inline QLineEdit overlay. While it is visible a state push is
    // stored but not laid out (see setState); the commit / cancel path
    // applies the deferred relayout.
    bool m_editVisible = false;
    bool m_relayoutDeferred = false;
};

}  // namespace rcx
