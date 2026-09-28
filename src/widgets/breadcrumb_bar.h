#pragma once

#include "core.h"
#include "paintutil.h"
#include "themes/thememanager.h"

#include <QWidget>
#include <QHBoxLayout>
#include <QToolButton>
#include <QLabel>
#include <QLayoutItem>
#include <QPainter>
#include <QStyle>
#include <QStyleOption>
#include <functional>

namespace rcx {

// BreadcrumbBar — compact, clickable drill-down trail shown above the command
// row in RcxEditor. ALWAYS visible while a class is in view; reflects the
// inline-expansion focus path (the chain of expanded hops from the root class
// down to the deepest drilled one) in the dotted shape the controller emits:
// `RcxEditor.vptr › QWidgetPrivate.parent › QWidget` — each ancestor crumb is
// the class you were in plus the field you left it through, the deepest is
// the bare class you are standing in. Every crumb is a class crumb; rootId
// carries the crumb INDEX (0 = root, i = the class opened by focus hop i-1).
// Clicking one fires onCrumb(index); the editor re-emits it as crumbClicked
// so the controller collapses everything below that class and scrolls to it.
// There is NO back button (clicking a crumb is the back affordance). Deep
// trails collapse the middle to an ellipsis: Head › … › Tail-1 › Tail.
//
// Intentionally no Q_OBJECT (mirrors EnumPickerPopup) — a std::function
// callback avoids dragging this header into every test target's AUTOMOC. It is
// still a QObject (via QWidget) so findChildren<QToolButton*>() and the
// QToolButton::clicked connects below work without a custom moc.
class BreadcrumbBar : public QWidget {
public:
    using CrumbFn = std::function<void(uint64_t /*crumbIndex*/)>;

    explicit BreadcrumbBar(QWidget* parent = nullptr) : QWidget(parent) {
        setObjectName(QStringLiteral("rcxBreadcrumbBar"));
        setAttribute(Qt::WA_StyledBackground, true);
        setFixedHeight(24);  // fixed strip height so Qt::AlignVCenter has a
                             // stable axis — labels and buttons of differing
                             // natural heights then center on the same line.
        m_layout = new QHBoxLayout(this);
        // kGutter, like every other strip's first ink — the left margin has to
        // read as ONE line down the window, not five slightly different ones.
        m_layout->setContentsMargins(kGutter, 0, kGutter, 0);
        m_layout->setSpacing(2);
        applyTheme(ThemeManager::instance().current());
        setVisible(false);  // until first setCrumbs (no document yet)
    }

    void setOnCrumb(CrumbFn fn) { m_onCrumb = std::move(fn); }

//TODO-DELETE(setMaxClassCrumbs)     // Beyond this many class crumbs the middle collapses to an ellipsis.
//    void setMaxClassCrumbs(int n) { m_maxClassCrumbs = qMax(2, n); rebuild(); }

    // Replace the rendered trail: one clickable crumb per entry, rootId =
    // crumb index. Always shown when there is ≥1 crumb.
    void setCrumbs(const QVector<Crumb>& crumbs) {
        // The controller pushes the trail on EVERY refresh tick (live memory
        // recomposes several times a second); an unchanged trail must not
        // tear down and recreate every label and button. applyTheme still
        // rebuilds unconditionally — the crumbs are equal but the QSS is not.
        if (crumbs == m_crumbs) return;
        m_crumbs = crumbs;
        rebuild();
    }

    void applyTheme(const Theme& t) {
        m_theme = t;
        // The band sits INSIDE the document column, so it is paper — not a
        // third chrome strip. It used to be menuBarColor (the title/menu tone),
        // which made a navigation aid outrank the data it navigates. The
        // bottom seam moved out of QSS and into paintEvent (see below).
        setStyleSheet(QStringLiteral("#rcxBreadcrumbBar { background:%1; }")
            .arg(editorPaperColor(t).name()));
        rebuild();
    }

protected:
    // The bottom seam is painted, not QSS: a "1px" CSS border covers 1.25
    // device px at 125 % DPI and snaps to TWO rows, so it read as a double
    // line beside the editor's device-exact frame. containerBorderColor is the
    // tone for every seam that touches the document.
    void paintEvent(QPaintEvent*) override {
        // QWidget::paintEvent is a no-op, so a subclass that reimplements it
        // has to draw the stylesheet background itself (Qt's documented
        // PE_Widget idiom) or WA_StyledBackground stops working.
        QStyleOption opt;
        opt.initFrom(this);
        QPainter p(this);
        style()->drawPrimitive(QStyle::PE_Widget, &opt, &p, this);
        fillBottomDeviceRowOfRect(p, QRectF(rect()), containerBorderColor(m_theme));
    }

public:
    // ── Test accessors ──
    bool barVisible() const { return isVisible(); }
    // Rendered tokens left→right: crumb labels, "›" separators, and "…" for
    // a collapsed gap.
    QStringList segments() const { return m_segments; }
    // The trail as pushed (labels plus the per-crumb class / hop / address).
    const QVector<Crumb>& crumbs() const { return m_crumbs; }

private:
    void clearLayout() {
        QLayoutItem* it;
        while ((it = m_layout->takeAt(0)) != nullptr) {
            if (it->widget()) {
                // hide() NOW: a deleteLater'd widget stays VISIBLE (and painted
                // at its old position) until the event loop processes the
                // deferred delete — which plain processEvents() skips — so
                // without this the previous crumbs overlap the new ones on
                // every rebuild. deleteLater (not delete) is still required:
                // rebuild() runs from a crumb button's own clicked handler, so
                // deleting it synchronously would be a use-after-free.
                it->widget()->hide();
                it->widget()->deleteLater();
            }
            delete it;
        }
        m_segments.clear();
    }

    QLabel* addLabel(const QString& text, const QColor& fg, bool italic, bool bold) {
        auto* l = new QLabel(text, this);
        l->setStyleSheet(QStringLiteral("color:%1;%2%3")
            .arg(fg.name(),
                 italic ? QStringLiteral("font-style:italic;") : QString(),
                 bold   ? QStringLiteral("font-weight:bold;")  : QString()));
        m_layout->addWidget(l, 0, Qt::AlignVCenter);
        return l;
    }

    void addSep() {
        addLabel(QStringLiteral("›"), m_theme.textFaint, false, false);  // ›
        m_segments.push_back(QStringLiteral("›"));
    }

    // `deepest` = the crumb you are standing on; `lone` = it is the ONLY crumb.
    // Tone ladder: a single crumb only repeats the class the doc tab and the
    // command row already name, so it stays textDim and regular. From depth 2
    // the trail becomes navigation — ancestors textDim, the deepest one text at
    // DemiBold, the single weight step on this surface.
    void makeClickable(const QString& text, uint64_t index, bool deepest, bool lone) {
        auto* btn = new QToolButton(this);
        btn->setText(text);
        btn->setAutoRaise(true);
        btn->setCursor(Qt::PointingHandCursor);
        const bool primary = deepest && !lone;
        // The layout already spends kGutter on the left edge, so the FIRST
        // crumb must not spend it again: with the button's own padding the
        // band's first ink landed further in than the pane tabs' and the doc
        // tabs', which is the one thing the shared gutter exists to prevent.
        const bool firstInStrip = (m_layout->count() == 0);
        btn->setStyleSheet(QStringLiteral(
            "QToolButton { border:none; padding:0 2px; padding-left:%4px;"
            " color:%1; background:transparent;%2 }"
            "QToolButton:hover { color:%3; text-decoration:underline; }")
            .arg((primary ? m_theme.text : m_theme.textDim).name(),
                 primary ? QStringLiteral(" font-weight:600;") : QString(),
                 m_theme.text.name())    // hover = link cue, not the accent
            .arg(firstInStrip ? 0 : 2));
        connect(btn, &QToolButton::clicked, this, [this, index]() {
            if (m_onCrumb) m_onCrumb(index);
        });
        m_layout->addWidget(btn, 0, Qt::AlignVCenter);
    }

    void rebuild() {
        if (!m_layout) return;
        clearLayout();

        const int n = m_crumbs.size();
        if (n == 0) { setVisible(false); return; }

        // Which crumbs render in full; -1 = the collapsed ellipsis gap.
        QVector<int> show;
        if (n <= m_maxClassCrumbs) {
            for (int i = 0; i < n; ++i) show.push_back(i);
        } else {
            show.push_back(0);
            show.push_back(-1);
            for (int i = n - (m_maxClassCrumbs - 1); i < n; ++i)
                show.push_back(i);
        }

        bool first = true;
        for (int idx : show) {
            if (idx < 0) {  // ellipsis gap
                if (!first) addSep();
                auto* ell = addLabel(QStringLiteral("…"), m_theme.textMuted, false, false);
                // The hidden crumbs are already dotted ("Class.field"), so the
                // tooltip only needs the separator between them.
                QStringList hidden;
                for (int h = 1; h < n - (m_maxClassCrumbs - 1); ++h)
                    hidden << m_crumbs[h].label;
                ell->setToolTip(hidden.join(QStringLiteral(" › ")));
                m_segments.push_back(QStringLiteral("…"));
                first = false;
                continue;
            }
            const Crumb& c = m_crumbs[idx];
            if (!first) addSep();
            const bool isCurrent = (idx == n - 1);
            // Every crumb is clickable (collapse-to + scroll). The
            // current/deepest one just scrolls (nothing deeper to collapse);
            // it carries the weight step that marks "you are here".
            makeClickable(c.label, c.rootId, isCurrent, n == 1);
            m_segments.push_back(c.label);
            first = false;
        }

        m_layout->addStretch(1);
        setVisible(true);
    }

    QHBoxLayout*   m_layout = nullptr;
    QVector<Crumb> m_crumbs;
    QStringList    m_segments;
    CrumbFn        m_onCrumb;
    Theme          m_theme;
    int            m_maxClassCrumbs = 4;
};

} // namespace rcx
