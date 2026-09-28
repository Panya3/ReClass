#include "sourcechooserpopup.h"
#include "fontutil.h"
#include "paintutil.h"
#include "svgicon.h"
#include "widgets/popup_chrome.h"
#include "widgets/section_header.h"
#include <QAction>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QStringListModel>
#include <QStyledItemDelegate>
#include <QPainter>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QApplication>
#include <QScreen>
#include <QScrollBar>
#include <QToolButton>
#include <cstring>

namespace rcx {

// ── Fuzzy scoring ──

static constexpr int kMaxFuzzyLen = 64;

static int fuzzyScore(const QString& pattern, const QString& text,
                      QVector<int>* outPositions = nullptr) {
    int pLen = pattern.size(), tLen = text.size();
    if (pLen == 0) return 1;
    if (pLen > tLen) return 0;
    if (pLen > kMaxFuzzyLen || tLen > 256) {
        if (text.startsWith(pattern, Qt::CaseInsensitive)) return 1;
        return 0;
    }
    QChar pLow[kMaxFuzzyLen];
    for (int i = 0; i < pLen; i++) pLow[i] = pattern[i].toLower();
    QChar tLow[256];
    for (int i = 0; i < tLen; i++) tLow[i] = text[i].toLower();
    { int pi = 0;
      for (int ti = 0; ti < tLen && pi < pLen; ti++)
          if (pLow[pi] == tLow[ti]) pi++;
      if (pi < pLen) return 0;
    }
    int bestPos[kMaxFuzzyLen], curPos[kMaxFuzzyLen];
    int best = 0, bestLen = 0;
    auto solve = [&](auto& self, int pi, int ti, int curLen, int score) -> void {
        if (pi == pLen) {
            if (score > best) { best = score; bestLen = curLen; memcpy(bestPos, curPos, curLen * sizeof(int)); }
            return;
        }
        int maxTi = tLen - (pLen - pi), branches = 0;
        for (int i = ti; i <= maxTi && branches < 4; i++) {
            if (pLow[pi] != tLow[i]) continue;
            int bonus = 1;
            if (i == 0)                                          bonus = 10;
            else if (text[i - 1] == '_' || text[i - 1] == ' ') bonus = 8;
            else if (text[i].isUpper() && text[i - 1].isLower()) bonus = 8;
            if (curLen > 0 && i == curPos[curLen - 1] + 1)      bonus += 5;
            curPos[curLen] = i;
            self(self, pi + 1, i + 1, curLen + 1, score + bonus);
            branches++;
        }
    };
    solve(solve, 0, 0, 0, 0);
    if (best > 0) {
        best += qMax(0, 20 - (tLen - pLen));
        if (pLen == tLen) best += 20;
        if (outPositions) { outPositions->resize(bestLen); memcpy(outPositions->data(), bestPos, bestLen * sizeof(int)); }
    }
    return best;
}

// ── Per-row delete (×) button geometry ──
// Shared by the delegate (paints it) and the popup (hit-tests clicks) so the
// glyph and the clickable area always agree. Right-anchored, vertically
// centred on the whole card. Only drawn on SavedSource rows.
static QRect deleteBtnRect(const QRect& itemRect) {
    constexpr int kSz = 16;
    return QRect(itemRect.right() - 6 - kSz,
                 itemRect.top() + (itemRect.height() - kSz) / 2, kSz, kSz);
}

// ── Chrome metrics ──

// Gap above the Clear All row that holds its divider, so the destructive
// action reads as separate from the provider list rather than crammed onto
// its tail. Shared by the delegate's sizeHint and its paint.
static constexpr int kClearGap = 9;

// The divider's rect: one device row at the top of the gap the Clear All
// row reserves. One place, because two things paint it — the delegate
// across the viewport and the scrollbar across its track.
static QRect clearDividerRect(const QRect& itemRect) { return itemRect.adjusted(0, 4, 0, 0); }

// A disabled row (Clear All with nothing to clear) is the whole card at
// 40 % — the bar's rule for a disabled cell, never textDim x 0.4.
static constexpr qreal kDisabledOpacity = 0.40;

// The house 12-px glyph for the filter's clear action.
static constexpr int kClearIconPx = 12;

// ── The filter box ──
// The house field grammar (rcx::PanelSearchField, src/widgets/
// panel_search_field.h) on the popup's own ground — rcx::PopupFilterField
// (widgets/popup_chrome.h), shared with the type chooser and the enum
// picker. Not the panel widget itself: that one wears chromeFont() and
// reads ThemeManager::current(); this field is set in the popup's editor
// face and paints from the theme the popup was handed, so chrome and rows
// can never disagree.

// ── The list ──
// sizeHint is the real content height — every row through the delegate —
// so the popup sizes itself from layout()->sizeHint() instead of a
// hand-duplicated table of row-height constants that drifted from the
// delegate by 3 px and put a scrollbar on a list that fit.
class SourceListView : public QListView {
public:
    using QListView::QListView;
    QSize sizeHint() const override {
        int h = 0;
        if (model())
            for (int r = 0, n = model()->rowCount(); r < n; ++r) h += sizeHintForRow(r);
        return QSize(QListView::sizeHint().width(), h + 2 * frameWidth());
    }
};

// ── Custom delegate (two-line card layout) ──

class SourceChooserDelegate : public QStyledItemDelegate {
public:
    const QVector<SourceEntry>* entries = nullptr;
    const QVector<QVector<int>>* matchPositions = nullptr;
    // The theme applyTheme() received. The popup's palette, QSS and frame
    // come from it, so the rows must too: reading ThemeManager::current()
    // here painted chrome and rows from two sources of truth (a previewed
    // theme, or the harness's, showed rows in the app's theme).
    const Theme* theme = nullptr;
    QFont baseFont;
    // Row index whose × delete button the mouse is currently over (-1 = none).
    // Set by the popup's MouseMove handler so the × gets its own hover state.
    int hoverDeleteRow = -1;

    explicit SourceChooserDelegate(QObject* parent) : QStyledItemDelegate(parent) {}

    bool hasSubline(const SourceEntry& e) const {
        return e.entryKind == SourceEntry::SavedSource
            || (e.entryKind == SourceEntry::ProviderAction && !e.dllFileName.isEmpty());
    }

    QFont smallFont() const {
        QFont s = baseFont;
        s.setPointSize(qMax(7, rcx::resolvedPointSize(baseFont) - 2));
        return s;
    }

    QSize sizeHint(const QStyleOptionViewItem& opt, const QModelIndex& idx) const override {
        const int row = idx.row();
        const QFontMetrics fm(baseFont);
        if (entries && row >= 0 && row < entries->size()) {
            const auto& e = (*entries)[row];
            if (e.entryKind == SourceEntry::SectionHeader)
                return QSize(opt.rect.width(),
                             sectionHeaderHeight(QFontMetrics(sectionHeaderFont(baseFont))));
            if (e.entryKind == SourceEntry::ClearAction)
                return QSize(opt.rect.width(), fm.height() + 12 + kClearGap);
            if (hasSubline(e))
                return QSize(opt.rect.width(),
                             fm.height() + QFontMetrics(smallFont()).height() + 14);
        }
        return QSize(opt.rect.width(), fm.height() + 12);
    }

    void paint(QPainter* p, const QStyleOptionViewItem& opt, const QModelIndex& idx) const override {
        const int row = idx.row();
        if (!entries || !theme || row < 0 || row >= entries->size()) return;
        const auto& e = (*entries)[row];
        const Theme& t = *theme;
        const QColor surface = t.background;

        p->setRenderHint(QPainter::Antialiasing, false);
        p->setFont(baseFont);
        const QFontMetrics fm(baseFont);
        const QFont small = smallFont();
        const QFontMetrics sfm(small);
        const QRect r = opt.rect;

        // ── Section header: the house SectionHeader spec (section_header.h)
        // on the popup's ground — 8-pt regular textDim, uppercase, caption
        // at kGutter, one device-exact containerBorderColor hairline UNDER
        // the row across its full width. Was bold textFaint over a
        // 1-logical-px theme.border line inset 10 px each side, which
        // doubled to two device rows at some phases.
        if (e.entryKind == SourceEntry::SectionHeader) {
            p->fillRect(r, surface);
            const QFont hf = sectionHeaderFont(baseFont);
            const QFontMetrics hfm(hf);
            p->setFont(hf);
            p->setPen(t.textDim);
            p->drawText(r.x() + kGutter,
                        r.y() + (r.height() + hfm.ascent() - hfm.descent()) / 2,
                        e.displayName);
            fillBottomDeviceRowOfRect(*p, QRectF(r), containerBorderColor(t));
            return;
        }

        // ── Ground: the surface, t.hover under the mouse, t.selected for
        // the keyboard's current row — nothing else, and neither on a
        // disabled row (Clear All with nothing to clear): a row that cannot
        // be clicked must not light up as if it could. A stale row used to
        // get a hand-mixed pink (bg + (12,-4,-4)) that was no theme token
        // and painted over the frame column; it says "(exited)" in
        // markerPtr and dims its icon instead. No side bar for the active
        // source either: purple is budgeted, and a bar at the row's x=0 sat
        // ON the popup's border.
        const bool selected = (opt.state & QStyle::State_Selected) && e.enabled;
        const bool hovered  = (opt.state & QStyle::State_MouseOver) && e.enabled;
        p->fillRect(r, selected ? t.selected : hovered ? t.hover : surface);

        // The Clear All divider: one device row of the seam colour at the
        // top of the gap the row reserves, across the viewport; the
        // scrollbar continues it across its track (SeamScrollBar).
        QRect body = r;
        if (e.entryKind == SourceEntry::ClearAction) {
            fillTopDeviceRowOfRect(*p, QRectF(clearDividerRect(r)), containerBorderColor(t));
            body = r.adjusted(0, kClearGap, 0, 0);
        }

        const qreal prevOpacity = p->opacity();
        if (!e.enabled) p->setOpacity(prevOpacity * kDisabledOpacity);

        const bool twoLine = hasSubline(e);
        int x = body.left() + kGutter;

        // ── Layout: row1 and row2 baselines ──
        int row1Y, row2Y = 0;
        if (twoLine) {
            const int totalTextH = fm.height() + sfm.height() + 3;
            const int topPad = (body.height() - totalTextH) / 2;
            row1Y = body.top() + topPad + fm.ascent();
            row2Y = row1Y + fm.descent() + 3 + sfm.ascent();
        } else {
            row1Y = body.top() + (body.height() - fm.height()) / 2 + fm.ascent();
        }

        // ── Icon: the vsicon tinted textDim at device resolution
        // (themedVsIcon caches by path / tint / size / dpr), snapped to a
        // whole device pixel, full opacity; a stale source's at 0.40,
        // drawTabSourceIcon's not-live rule. The raw #C5C5C5 SVG at 0.8
        // opacity was a ghost on light paper (24/765 against tw's ground).
        const int iconSz = twoLine ? fm.height() + 4 : fm.height();
        const int iconY = body.top() + (body.height() - iconSz) / 2;
        if (!e.iconPath.isEmpty()) {
            const qreal dpr = p->device() ? p->device()->devicePixelRatioF() : 1.0;
            const QPixmap pm = themedVsIcon(e.iconPath, t.textDim, iconSz, dpr)
                                   .pixmap(QSize(iconSz, iconSz), dpr);
            if (!pm.isNull()) {
                const qreal o = p->opacity();
                if (e.isStale) p->setOpacity(o * 0.40);
                drawPixmapSnapped(*p, QPointF(x, iconY), pm);
                p->setOpacity(o);
            }
        }
        x += iconSz + kGutter;
        const int textX = x;

        // ── Row 1: the name — text; textDim for the Clear All action;
        // textMuted when stale. Bold marks the active source, with the word
        // "active" on row 2: the one accent this popup spends.
        const QColor nameColor = e.isStale ? t.textMuted
                               : e.entryKind == SourceEntry::ClearAction ? t.textDim
                               : t.text;

        const QVector<int>* positions = (matchPositions && row < matchPositions->size())
            ? &(*matchPositions)[row] : nullptr;
        QSet<int> hlSet;
        if (positions) for (int pi : *positions) hlSet.insert(pi);

        QFont nameFont = baseFont;
        if (e.isActive) nameFont.setBold(true);

        for (int ci = 0; ci < e.displayName.size(); ci++) {
            const QChar ch = e.displayName[ci];
            const bool hl = hlSet.contains(ci);
            p->setPen(hl ? t.indHoverSpan : nameColor);
            QFont charFont = nameFont;
            if (hl) charFont.setBold(true);
            p->setFont(charFont);
            p->drawText(x, row1Y, QString(ch));
            x += QFontMetrics(charFont).horizontalAdvance(ch);
        }
        p->setFont(baseFont);

        // Stale suffix — process exited / file vanished — in the theme's
        // warning hue so it follows a theme switch.
        if (e.isStale) {
            x += 6;
            p->setFont(small);
            p->setPen(t.markerPtr);
            p->drawText(x, row1Y, QStringLiteral("(exited)"));
            p->setFont(baseFont);
        }

        // Per-row delete (×) for saved sources — lets the user remove a
        // single bound source instead of only "Clear All". Hit-tested in the
        // popup's viewport event filter via the shared deleteBtnRect(). Its
        // OWN hover state: textMuted at rest, text when the mouse is over
        // the glyph itself (not just the row).
        if (e.entryKind == SourceEntry::SavedSource) {
            const QRect xr = deleteBtnRect(r);
            p->setFont(baseFont);
            p->setPen(row == hoverDeleteRow ? t.text : t.textMuted);
            p->drawText(xr, Qt::AlignCenter, QStringLiteral("\u00d7"));
        }

        if (!twoLine) { p->setOpacity(prevOpacity); return; }

        // ── Row 2: plain tokens, two spaces apart — kind and PID in
        // textMuted, the arch in textDim, "active" in the accent. The old
        // anti-aliased rounded pills were alpha blends of no theme token,
        // and the x86 pill spent the accent on every non-x64 row.
        x = textX;
        p->setFont(small);
        const int gap = sfm.horizontalAdvance(QLatin1Char(' ')) * 2;
        auto token = [&](const QString& s, const QColor& c) {
            p->setPen(c);
            p->drawText(x, row2Y, s);
            x += sfm.horizontalAdvance(s) + gap;
        };

        if (e.entryKind == SourceEntry::SavedSource) {
            if (!e.kindLabel.isEmpty()) token(e.kindLabel, t.textMuted);
            if (!e.pid.isEmpty())       token(QStringLiteral("PID ") + e.pid, t.textMuted);
            if (!e.arch.isEmpty())      token(e.arch, t.textDim);
            if (e.isActive)             token(QStringLiteral("active"), t.indHoverSpan);

            // Base address or file path (right-aligned on row 2)
            QString rightText;
            if (!e.filePath.isEmpty())
                rightText = e.filePath;
            else if (!e.baseAddress.isEmpty())
                rightText = e.baseAddress;

            if (!rightText.isEmpty()) {
                // Reserve a right gutter for the × delete button so the
                // path/base-address never slides under it.
                const int rightEdge = r.right() - 28;
                const int maxW = rightEdge - x - 4;
                if (maxW > 0) {
                    const QString elided = sfm.elidedText(rightText, Qt::ElideMiddle, maxW);
                    const int tw = sfm.horizontalAdvance(elided);
                    p->setPen(t.textFaint);
                    p->drawText(rightEdge - tw, row2Y, elided);
                }
            }
        }

        // Provider: kind + DLL name on row 2
        if (e.entryKind == SourceEntry::ProviderAction) {
            if (!e.kindLabel.isEmpty()) token(e.kindLabel, t.textMuted);
            if (!e.dllFileName.isEmpty()) {
                p->setPen(t.textFaint);
                p->drawText(x, row2Y, e.dllFileName);
            }
        }

        p->setFont(baseFont);
        p->setOpacity(prevOpacity);
    }
};

// ── SourceChooserPopup ──

SourceChooserPopup::SourceChooserPopup(QWidget* parent)
    : QFrame(parent, Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint)
{
    setAttribute(Qt::WA_DeleteOnClose, false);
    setFocusPolicy(Qt::StrongFocus);

    auto* layout = new QVBoxLayout(this);
    // 1 logical px on every side: the frame's device row / column lives
    // inside the first logical px (paintEvent), so the field, the list
    // viewport, its scrollbar and every delegate fill end AT the frame at
    // any dpr instead of on it — the old (0, 1, 0, 1) put the filter, the
    // list and the scrollbar over the side borders for the whole list
    // height. A band (the filter seam, a section hairline, the Clear All
    // divider) spans this interior width: it ends at the frame, never on it.
    layout->setContentsMargins(1, 1, 1, 1);
    layout->setSpacing(0);

    // Title row: "Data Source" at the house gutter and a plain Esc button —
    // borderless, like TypeSelectorPopup's close and EnumPickerPopup's Esc
    // label; its rounded 1-px QSS box was the one radius in the family.
    {
        auto* row = new QHBoxLayout;
        row->setContentsMargins(kGutter, 6, 6, 2);
        row->setSpacing(4);
        m_titleLabel = new QLabel(QStringLiteral("Data Source"));
        QFont bold = font();
        bold.setBold(true);
        m_titleLabel->setFont(bold);
        row->addWidget(m_titleLabel);
        row->addStretch();

        auto* escBtn = new QToolButton;
        escBtn->setText(QStringLiteral("Esc"));
        escBtn->setAutoRaise(true);
        escBtn->setFocusPolicy(Qt::NoFocus);
        escBtn->setFixedSize(32, 20);
        connect(escBtn, &QToolButton::clicked, this, &QFrame::hide);
        m_escBtn = escBtn;
        row->addWidget(escBtn);

        layout->addLayout(row);
    }

    // Filter field — the popup surface plus its own seam (PopupFilterField).
    // The clear action is the house 12-px tinted close glyph, shown only
    // while there is text (PanelSearchField's), not Qt's stock button.
    auto* filter = new PopupFilterField;
    filter->setPlaceholderText(QStringLiteral("Filter sources..."));
    filter->setFrame(false);
    filter->setFixedHeight(PopupFilterField::kMinHeight);
    m_clearAction = filter->addAction(QIcon(), QLineEdit::TrailingPosition);
    m_clearAction->setVisible(false);
    m_clearAction->setToolTip(QStringLiteral("Clear"));
    connect(m_clearAction, &QAction::triggered, filter, &QLineEdit::clear);
    connect(filter, &QLineEdit::textChanged, m_clearAction,
            [this](const QString& s) { m_clearAction->setVisible(!s.isEmpty()); });
    m_filterEdit = filter;
    layout->addWidget(m_filterEdit);

    // List view
    m_listView = new SourceListView;
    m_model = new QStringListModel(this);
    m_listView->setModel(m_model);
    m_listView->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_listView->setSelectionMode(QAbstractItemView::SingleSelection);
    m_listView->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_listView->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
    m_listView->setMouseTracking(true);
    m_listView->setFrameShape(QFrame::NoFrame);
    m_listView->setUniformItemSizes(false);

    // The scrollbar carries the section hairlines and the Clear All divider
    // across its track: the delegate's rows end at the viewport, and while
    // the list scrolled those seams stopped 8 device px short of the right
    // frame under the bare track.
    auto* seamBar = new SeamScrollBar(Qt::Vertical);
    seamBar->seams = [this]() {
        QVector<SeamScrollBar::Seam> out;
        const QColor seam = containerBorderColor(m_theme);
        const QRect visible = m_listView->viewport()->rect();
        for (int r = 0; r < m_filteredEntries.size(); ++r) {
            const auto& e = m_filteredEntries[r];
            if (e.entryKind != SourceEntry::SectionHeader
                && e.entryKind != SourceEntry::ClearAction) continue;
            const QRect vr = m_listView->visualRect(m_model->index(r));
            if (!vr.isValid() || !vr.intersects(visible)) continue;
            if (e.entryKind == SourceEntry::SectionHeader)
                out.append({QRectF(vr), /*bottomEdge*/ true, seam});
            else
                out.append({QRectF(clearDividerRect(vr)), /*bottomEdge*/ false, seam});
        }
        return out;
    };
    m_listView->setVerticalScrollBar(seamBar);
    // The popup paints the same seams under its children (paintEvent), so
    // it repaints whenever they move with the scroll.
    connect(seamBar, &QScrollBar::valueChanged, this, [this] { update(); });
    connect(seamBar, &QScrollBar::rangeChanged, this, [this] { update(); });

    auto* delegate = new SourceChooserDelegate(this);
    m_listView->setItemDelegate(delegate);
    layout->addWidget(m_listView, 1);

    // Footer: the key hints, left at the gutter with kGutter/2 above and
    // below; the popup paints the seam over it (paintEvent).
    m_footerLabel = new QLabel;
    m_footerLabel->setAlignment(Qt::AlignLeft | Qt::AlignVCenter);
    m_footerLabel->setContentsMargins(kGutter, kGutter / 2, 6, kGutter / 2);
    layout->addWidget(m_footerLabel);

    // Connections
    connect(m_filterEdit, &QLineEdit::textChanged,
            this, &SourceChooserPopup::applyFilter);
    connect(m_listView, &QListView::clicked,
            this, [this](const QModelIndex& idx) { acceptIndex(idx.row()); });

    m_filterEdit->installEventFilter(this);
    m_listView->installEventFilter(this);
    m_listView->viewport()->installEventFilter(this);
}

void SourceChooserPopup::setFont(const QFont& font) {
    m_font = font;
    m_filterEdit->setFont(font);
    // The field's height follows the face (fm.height() + 8, the section
    // band rule) with PanelSearchField's 26 as the floor: the popup is set
    // in the editor face one point down, taller than the chrome face the
    // shared field is built for.
    m_filterEdit->setFixedHeight(qMax(PopupFilterField::kMinHeight, QFontMetrics(font).height() + 8));
    m_listView->setFont(font);

    QFont bold = font;
    bold.setBold(true);
    m_titleLabel->setFont(bold);

    QFont small = font;
    small.setPointSize(qMax(7, rcx::resolvedPointSize(font) - 2));
    m_footerLabel->setFont(small);
    m_escBtn->setFont(small);

    auto* delegate = static_cast<SourceChooserDelegate*>(m_listView->itemDelegate());
    delegate->baseFont = font;
}

void SourceChooserPopup::applyTheme(const Theme& theme) {
    m_theme = theme;
    const Theme& t = m_theme;
    const QColor surface = t.background;

    QPalette pal = palette();
    pal.setColor(QPalette::Window, surface);
    pal.setColor(QPalette::WindowText, t.text);
    pal.setColor(QPalette::Base, surface);
    pal.setColor(QPalette::Text, t.text);
    pal.setColor(QPalette::PlaceholderText, t.textFaint);
    pal.setColor(QPalette::Highlight, t.selected);
    pal.setColor(QPalette::HighlightedText, t.text);
    setPalette(pal);

    m_titleLabel->setStyleSheet(
        QStringLiteral("color: %1;").arg(t.text.name()));

    // The field: the house field's shape over the popup ground instead of
    // editor paper (popupFieldQss) — no box, no radius, textDim ink, the
    // theme's selection band. The lead pad lands the first glyph on kGutter.
    // The seam under it is the field's own (PopupFilterField), never a QSS
    // border and never a second surface: the old backgroundAlt band under
    // a 1-logical-px separator was a cream strip on a grey body.
    auto* filter = static_cast<PopupFilterField*>(m_filterEdit);
    filter->applyTheme(m_theme, surface, kGutter - 2);
    m_clearAction->setIcon(themedVsIcon(QStringLiteral(":/vsicons/close.svg"),
                                        t.textDim, kClearIconPx, devicePixelRatioF()));
    // QLineEdit creates the action's button lazily; size it once it exists
    // so the glyph is the spec'd 12 px, not Qt's 16.
    for (auto* b : filter->findChildren<QToolButton*>())
        b->setIconSize(QSize(kClearIconPx, kClearIconPx));

    m_listView->setStyleSheet(QStringLiteral(
        "QListView { background: %1; border: none; }"
        "QListView::item { border: none; background: transparent; }")
        .arg(surface.name()));

    // The scrollbar, locally (popupScrollBarQss): the app-wide rule
    // (MainWindow::applyGlobalTheme, src/main.cpp) is an 8-px solid
    // textFaint bar on palette(window), which beside these rows read as a
    // heavy rod down the whole track — and, before the 1-px layout inset,
    // sat on the right border.
    m_listView->verticalScrollBar()->setStyleSheet(popupScrollBarQss(t, surface));

    m_footerLabel->setStyleSheet(
        QStringLiteral("color: %1;").arg(t.textMuted.name()));
    m_escBtn->setStyleSheet(QStringLiteral(
        "QToolButton { color: %1; border: none; padding: 1px 4px; }"
        "QToolButton:hover { background: %2; color: %3; }")
        .arg(t.textFaint.name(), t.hover.name(), t.text.name()));

    auto* delegate = static_cast<SourceChooserDelegate*>(m_listView->itemDelegate());
    delegate->theme = &m_theme;
    update();
    m_listView->viewport()->update();
}

void SourceChooserPopup::setSources(const QVector<SourceEntry>& entries) {
    m_allEntries = entries;
    m_cachedMaxNameLen = 0;
    for (const auto& e : entries) {
        if (e.entryKind != SourceEntry::SectionHeader)
            m_cachedMaxNameLen = qMax(m_cachedMaxNameLen, (int)e.displayName.size());
    }

    auto* delegate = static_cast<SourceChooserDelegate*>(m_listView->itemDelegate());
    delegate->entries = &m_filteredEntries;
    delegate->matchPositions = &m_matchPositions;

    m_filterEdit->clear();
    applyFilter(QString());

    // Rebuilt while up — a per-row x delete — the popup kept the height the
    // old list needed, and the list's stretch turned the difference into
    // bare ground between the last row and the footer. Re-fit to the new
    // rows; the edge on the bar stays where the user is looking.
    if (isVisible()) fitToScreen(/*inPlace*/ true);
}

void SourceChooserPopup::setLivenessResults(const QVector<bool>& alive) {
    bool changed = false;
    for (auto& e : m_allEntries) {
        if (e.savedIndex < 0 || e.savedIndex >= alive.size()) continue;
        bool stale = !alive[e.savedIndex];
        if (e.isStale != stale) { e.isStale = stale; changed = true; }
    }
    if (changed)
        applyFilter(m_filterEdit->text());
}

void SourceChooserPopup::applyFilter(const QString& text) {
    m_filteredEntries.clear();
    m_matchPositions.clear();

    if (text.isEmpty()) {
        m_filteredEntries = m_allEntries;
        m_matchPositions.resize(m_allEntries.size());
    } else {
        struct Scored { int srcIdx; int score; QVector<int> positions; };
        QVector<Scored> scored;
        for (int i = 0; i < m_allEntries.size(); i++) {
            const auto& e = m_allEntries[i];
            if (e.entryKind == SourceEntry::SectionHeader) continue;
            QString searchable = e.displayName;
            if (!e.kindLabel.isEmpty()) searchable += QChar(' ') + e.kindLabel;
            if (!e.pid.isEmpty()) searchable += QChar(' ') + e.pid;
            if (!e.dllFileName.isEmpty()) searchable += QChar(' ') + e.dllFileName;
            if (!e.filePath.isEmpty()) searchable += QChar(' ') + e.filePath;
            QVector<int> pos;
            int sc = fuzzyScore(text, searchable, &pos);
            if (sc > 0)
                scored.append({i, sc, pos});
        }
        std::sort(scored.begin(), scored.end(), [](const Scored& a, const Scored& b) {
            return a.score > b.score;
        });
        for (const auto& s : scored) {
            m_filteredEntries.append(m_allEntries[s.srcIdx]);
            m_matchPositions.append(s.positions);
        }
    }

    QStringList displayStrings;
    displayStrings.reserve(m_filteredEntries.size());
    for (const auto& e : m_filteredEntries)
        displayStrings.append(e.displayName);
    m_model->setStringList(displayStrings);
    // The list's sizeHint just changed with its rows, and the layout's
    // QWidgetItem caches a widget's hint until the widget says otherwise —
    // without this the re-fit setSources() runs measured the OLD list.
    m_listView->updateGeometry();
    m_listView->verticalScrollBar()->update();   // the seams it continues moved
    update();                                    // and the popup's mirror of them

    // Footer
    int total = 0;
    for (const auto& e : m_allEntries)
        if (e.entryKind != SourceEntry::SectionHeader) total++;

    if (text.isEmpty()) {
        m_footerLabel->setText(
            QStringLiteral("\u2191\u2193 navigate  \u21B5 select  Esc close"));
    } else if (m_filteredEntries.isEmpty()) {
        m_footerLabel->setText(
            QStringLiteral("No matches for \"%1\"").arg(text));
    } else {
        m_footerLabel->setText(
            QStringLiteral("%1 of %2 sources").arg(m_filteredEntries.size()).arg(total));
    }

    // Only pre-select when filtering — otherwise the first row (Open File)
    // appears permanently highlighted before the user has hovered or
    // navigated to it. Filter focus is in m_filterEdit; pressing Down
    // shifts focus to the list and selects the first row at that point.
    if (!text.isEmpty()) {
        int first = nextSelectableRow(0, 1);
        if (first >= 0)
            m_listView->setCurrentIndex(m_model->index(first));
    } else {
        m_listView->setCurrentIndex(QModelIndex());
    }
}

void SourceChooserPopup::popup(const QPoint& globalPos, int anchorTop) {
    m_anchor    = globalPos;
    m_anchorTop = anchorTop;
    const QFontMetrics fm(m_font);
    const int charW = fm.horizontalAdvance(QChar('M'));
    const int popupW = qBound(360, (m_cachedMaxNameLen + 24) * charW, 560);
    const QRect screen = QApplication::screenAt(globalPos)
        ? QApplication::screenAt(globalPos)->availableGeometry()
        : QRect(0, 0, 1920, 1080);
    setFixedWidth(popupW);
    move(qBound(screen.left(), globalPos.x(), screen.right() - popupW), globalPos.y());
    fitToScreen(/*inPlace*/ false);

    show();
    raise();
    activateWindow();
    m_filterEdit->setFocus();
    m_filterEdit->selectAll();
}

void SourceChooserPopup::warmUp() {
    show();
    hide();
}

// Height from the real hints — the rows through SourceListView::sizeHint
// (the delegate's heights), the chrome through the layout — capped by the
// SCREEN under the anchor, not a constant: the old 520-px cap with
// hand-copied row constants left the default list 3 px taller than its
// viewport in the app's font, so a scrollbar with a 90 % handle appeared
// for nothing. When the room below is too small and there is more above,
// the popup flips over the bar (EnumPickerPopup's rule) — ending one device
// row above the bar's TOP edge when the caller named it (m_anchorTop), not
// on the anchor, which is the bar's bottom edge: a popup ending there
// covered the bar and the chip that opened it. A re-fit while up
// (setSources after a per-row x delete) keeps the edge that sits on the
// bar: the top edge under it — and, flipped above it, the BOTTOM edge.
// Always keeping the top edge left a flipped popup detached from the bar
// by the removed row's height.
void SourceChooserPopup::fitToScreen(bool inPlace) {
    layout()->invalidate();
    layout()->activate();
    const int wanted = layout()->sizeHint().height();
    const QRect screen = QApplication::screenAt(m_anchor)
        ? QApplication::screenAt(m_anchor)->availableGeometry()
        : QRect(0, 0, 1920, 1080);
    constexpr int kScreenPad = 8;
    constexpr int kMinH = 140;
    int y = pos().y();
    int popupH;
    if (inPlace && y < m_anchor.y()) {
        // Flipped above the bar: the bottom edge stays, the top moves.
        const int bottom = y + height();
        popupH = qMax(kMinH, qMin(wanted, bottom - screen.top() - kScreenPad));
        y = bottom - popupH;
    } else if (inPlace) {
        // Under the bar: the top edge stays, only the bottom moves.
        popupH = qMin(wanted, screen.bottom() - y - kScreenPad);
    } else {
        const int barTop = m_anchorTop >= 0 ? m_anchorTop : m_anchor.y();
        const int below = screen.bottom() - m_anchor.y() - kScreenPad;
        const int above = barTop - screen.top() - kScreenPad;
        popupH = qMin(wanted, below);
        y = m_anchor.y();
        if (popupH < wanted && above > below) {
            popupH = qMin(wanted, above);
            y = barTop - popupH;
        }
    }
    popupH = qMax(kMinH, popupH);
    setFixedSize(width(), popupH);
    move(pos().x(), y);
}

// ── The popup family rule ──
// A popup is ONE surface — theme.background, the ground MenuBarStyle's
// PE_FrameMenu (src/main.cpp) already gives the bar's own QMenu dropdowns —
// inside ONE device-exact 1-px theme.border frame the popup paints itself
// with paintutil's four edge fills (fillDeviceFrameOfRect), with its layout
// inset 1 logical px so no child (field, list viewport, scrollbar, delegate
// fill, accent) ever touches the frame. Square corners,
// Qt::NoDropShadowWindowHint (the platform's DWM shadow was the one shadow
// in the app), and a text field inside it is the same surface plus a
// device-exact bottom hairline (containerBorderColor at rest, borderFocused
// while focused; PopupFilterField) — never a second surface, never a QSS
// box. Seams inside the popup (the field, the section rows, the Clear All
// divider, the footer) are single device rows of containerBorderColor
// spanning the interior width — under the scroll track too (SeamScrollBar);
// only the frame is full theme.border. Scrollbars are the popup's local
// 6-px rule (popupScrollBarQss), icons are themedVsIcon at a real tone drawn
// with drawPixmapSnapped, and the accent (indHoverSpan) is budgeted to one
// "current" marker per popup at most — and never a stripe against the
// frame: a row marker (the type chooser's kind stripe, the enum picker's)
// ends at kGutter, with clean ground between it and the frame column.
//
// Why device fills and not 1-logical-px fillRect strips: 1 logical px is
// 1.25 device px at 125 %, which snaps to ONE or TWO device rows depending
// on where the window lands, so the frame read a different weight on each
// edge and left stray corner pixels. The same fraction is why the POPUP
// paints every interior seam itself, across its whole width and before the
// frame: the children's 1-logical inset is not device-exact either, so at
// half the widths at 125 % (and always at 150 % and above) a child ends one
// device column short of the frame, and a seam only the field, the delegate
// or the scrollbar painted stopped with a notch of ground before it. The
// children paint the same device row over their share; the popup's fill is
// what reaches the frame, and the frame, painted last, is what ends it. The
// shared pieces live in widgets/popup_chrome.h; TypeSelectorPopup,
// EnumPickerPopup and HexToolbarPopup follow this rule. The one exception
// is the editor's HoverPopupHost: a hover popup is transient like
// RcxTooltip, so it keeps the TOOLTIP surface (backgroundAlt) — but its
// frame is this same device-exact one, not a QFrame::Box.
void SourceChooserPopup::paintEvent(QPaintEvent*) {
    const Theme& t = m_theme;
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);
    p.fillRect(rect(), t.background);
    const QColor seam = containerBorderColor(t);
    // The field's seam, in the field's own (focus-aware) colour: the field
    // paints the same row over its rect, which ends short of the frame at
    // half the widths (see above).
    const QRect fe = m_filterEdit->geometry();
    fillBottomDeviceRowOfRect(p, QRectF(0, fe.top(), width(), fe.height()),
                              static_cast<PopupFilterField*>(m_filterEdit)->seamColor());
    // The seam over the footer: the list's viewport ends where the footer
    // starts, and a QLabel paints no ground, so the row shows through.
    const QRect fg = m_footerLabel->geometry();
    fillTopDeviceRowOfRect(p, QRectF(0, fg.top(), width(), fg.height()), seam);
    // The section hairlines and the Clear All divider, under the list and
    // its track (the SeamScrollBar hands over the same rects, viewport-
    // relative; the viewport's offset is whole logical px, so the device
    // row picked here is the delegate's).
    auto* bar = static_cast<SeamScrollBar*>(m_listView->verticalScrollBar());
    if (bar->seams && !m_listView->isHidden()) {
        const int vy = m_listView->viewport()->mapTo(this, QPoint(0, 0)).y();
        for (const SeamScrollBar::Seam& s : bar->seams()) {
            const QRectF across(0, s.rect.top() + vy, width(), s.rect.height());
            if (s.bottomEdge) fillBottomDeviceRowOfRect(p, across, s.color);
            else              fillTopDeviceRowOfRect(p, across, s.color);
        }
    }
    fillDeviceFrameOfRect(p, QRectF(rect()), t.border);
}

void SourceChooserPopup::hideEvent(QHideEvent* event) {
    QFrame::hideEvent(event);
    // Every way out — a pick, Esc, a click elsewhere — lands here, so this
    // is the one place that can say "the popup is gone". The address bar's
    // source chip stays t.hover from popup() until it hears this.
    emit dismissed();
}

bool SourceChooserPopup::eventFilter(QObject* obj, QEvent* event) {
    // Pointing-hand cursor over selectable rows (arrow over section headers),
    // mirroring how the RcxEditor swaps its viewport cursor on hover. Don't
    // consume — the hover highlight still needs the move event.
    if (event->type() == QEvent::MouseMove && obj == m_listView->viewport()) {
        auto* me = static_cast<QMouseEvent*>(event);
        QModelIndex idx = m_listView->indexAt(me->pos());
        bool clickable = false;
        int hoverX = -1;
        if (idx.isValid() && idx.row() < m_filteredEntries.size()) {
            const auto& e = m_filteredEntries[idx.row()];
            clickable = e.enabled && e.entryKind != SourceEntry::SectionHeader;
            // Track the × delete button under the cursor so it gets its own
            // hover highlight (repaint only when the hovered × row changes).
            if (e.entryKind == SourceEntry::SavedSource && e.savedIndex >= 0
                && deleteBtnRect(m_listView->visualRect(idx)).contains(me->pos()))
                hoverX = idx.row();
        }
        m_listView->viewport()->setCursor(clickable ? Qt::PointingHandCursor
                                                     : Qt::ArrowCursor);
        auto* dg = static_cast<SourceChooserDelegate*>(m_listView->itemDelegate());
        if (dg && dg->hoverDeleteRow != hoverX) {
            dg->hoverDeleteRow = hoverX;
            m_listView->viewport()->update();
        }
    }
    // Reset the × hover when the cursor leaves the list.
    if (event->type() == QEvent::Leave && obj == m_listView->viewport()) {
        auto* dg = static_cast<SourceChooserDelegate*>(m_listView->itemDelegate());
        if (dg && dg->hoverDeleteRow != -1) {
            dg->hoverDeleteRow = -1;
            m_listView->viewport()->update();
        }
    }

    // Click on a saved row's × deletes just that source (popup stays open).
    if (event->type() == QEvent::MouseButtonRelease && obj == m_listView->viewport()) {
        auto* me = static_cast<QMouseEvent*>(event);
        QModelIndex idx = m_listView->indexAt(me->pos());
        if (idx.isValid() && idx.row() < m_filteredEntries.size()) {
            const auto& e = m_filteredEntries[idx.row()];
            if (e.entryKind == SourceEntry::SavedSource && e.savedIndex >= 0
                && deleteBtnRect(m_listView->visualRect(idx)).contains(me->pos())) {
                emit removeRequested(e.savedIndex);
                return true;  // consume — don't activate/switch to the row
            }
        }
    }

    if (event->type() == QEvent::KeyPress) {
        auto* ke = static_cast<QKeyEvent*>(event);

        if (ke->key() == Qt::Key_Escape) {
            hide();
            return true;
        }

        if (obj == m_filterEdit) {
            if (ke->key() == Qt::Key_Down) {
                m_listView->setFocus();
                int first = nextSelectableRow(0, 1);
                if (first >= 0)
                    m_listView->setCurrentIndex(m_model->index(first));
                return true;
            }
            if (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) {
                acceptCurrent();
                return true;
            }
        }

        if (obj == m_listView) {
            if (ke->key() == Qt::Key_Up) {
                int cur = m_listView->currentIndex().row();
                int prev = nextSelectableRow(cur - 1, -1);
                if (prev >= 0)
                    m_listView->setCurrentIndex(m_model->index(prev));
                else
                    m_filterEdit->setFocus();
                return true;
            }
            if (ke->key() == Qt::Key_Down) {
                int cur = m_listView->currentIndex().row();
                int next = nextSelectableRow(cur + 1, 1);
                if (next >= 0)
                    m_listView->setCurrentIndex(m_model->index(next));
                return true;
            }
            if (ke->key() == Qt::Key_Return || ke->key() == Qt::Key_Enter) {
                acceptCurrent();
                return true;
            }
            if (ke->key() == Qt::Key_Delete) {
                int cur = m_listView->currentIndex().row();
                if (cur >= 0 && cur < m_filteredEntries.size()) {
                    const auto& e = m_filteredEntries[cur];
                    if (e.entryKind == SourceEntry::SavedSource && e.savedIndex >= 0) {
                        emit removeRequested(e.savedIndex);
                        return true;
                    }
                }
            }
            if (ke->key() == Qt::Key_Backspace) {
                QString t = m_filterEdit->text();
                if (!t.isEmpty()) {
                    m_filterEdit->setText(t.left(t.size() - 1));
                    m_filterEdit->setFocus();
                }
                return true;
            }
            QString text = ke->text();
            if (!text.isEmpty() && text[0].isPrint()) {
                m_filterEdit->setText(m_filterEdit->text() + text);
                m_filterEdit->setFocus();
                return true;
            }
        }
    }

    return QFrame::eventFilter(obj, event);
}

void SourceChooserPopup::acceptCurrent() {
    QModelIndex idx = m_listView->currentIndex();
    if (idx.isValid())
        acceptIndex(idx.row());
}

void SourceChooserPopup::acceptIndex(int row) {
    if (row < 0 || row >= m_filteredEntries.size()) return;
    const auto& e = m_filteredEntries[row];
    if (!e.enabled || e.entryKind == SourceEntry::SectionHeader) return;

    if (e.entryKind == SourceEntry::SavedSource && e.isActive) {
        hide();
        return;
    }

    hide();
    if (e.entryKind == SourceEntry::ClearAction)
        emit clearRequested();
    else if (e.entryKind == SourceEntry::ProviderAction)
        emit providerSelected(e.providerIdentifier);
    else if (e.entryKind == SourceEntry::SavedSource && e.savedIndex >= 0)
        emit sourceSelected(e.savedIndex);
}

int SourceChooserPopup::nextSelectableRow(int from, int direction) const {
    for (int i = from; i >= 0 && i < m_filteredEntries.size(); i += direction) {
        const auto& e = m_filteredEntries[i];
        if (e.entryKind != SourceEntry::SectionHeader && e.enabled)
            return i;
    }
    return -1;
}

} // namespace rcx
