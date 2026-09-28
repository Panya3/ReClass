#pragma once
#include "rcxtooltip.h"
#include <QAbstractItemView>
#include <QApplication>
#include <QHeaderView>
#include <QHelpEvent>
#include <QMenu>
#include <QMouseEvent>
#include <QPointer>
#include <QTabBar>
#include <QToolTip>

namespace rcx {

// App-wide tooltip bridge: rcx::RcxTooltip is the ONLY tooltip in the app.
// One filter on qApp catches every QEvent::ToolTip, resolves the text for
// whatever is under the cursor, and paints it with RcxTooltip.
//
// ── Why this is not just "read widget->toolTip()" ─────────────────────────
// Half the app's tooltips do not live in that property. QTabBar keeps them
// per-tab, item views keep them in Qt::ToolTipRole, QHeaderView per section,
// QMenu per action. Qt's own handlers for those call QToolTip::showText, so
// before the resolver chain below the app showed TWO different tooltips: the
// painted one on buttons and dock headers, and Qt's native balloon on tabs,
// menus and list rows — styled by a different stylesheet, with different
// padding, font and colour. They alternated as the cursor moved.
//
// ── Why dismissal is driven by MouseMove ──────────────────────────────────
// QEvent::ToolTip propagates UP the parent chain until something accepts it,
// and an application filter re-runs on every hop. The old code dismissed on
// each hop where the widget had no tooltip of its own, so a tooltip living on
// a CONTAINER produced hide → hide → show on a single hover tick, and the
// idempotence guard could not help because its own state had been cleared two
// hops earlier in the same delivery. Nothing in the ToolTip path hides
// anything now; the mouse decides, which is what Qt itself does.
//
// ── Why there is no WindowDeactivate clause ───────────────────────────────
// Showing a Qt::ToolTip window can deactivate the window underneath it even
// with WA_ShowWithoutActivating, so dismissing on WindowDeactivate meant the
// tooltip's own appearance dismissed it — measured, twice per hover, in
// tests/test_tooltip_flicker.cpp. Leave, the mouse-follow below, QEvent::Hide
// and RcxTooltip's expiry timer cover every real "user went away" case.
class GlobalTooltipBridge : public QObject {
    // No Q_OBJECT — the bridge has no signals/slots, only an eventFilter
    // override, and skipping the macro lets it live in a header-only file
    // without an AUTOMOC dance on every test target that includes it.
public:
    explicit GlobalTooltipBridge(QObject* parent = nullptr) : QObject(parent) {}

    // Text plus the rect it belongs to, in the resolving widget's coords.
    // The rect is what lets the move-handler tell "left this tab" from
    // "left the whole tab bar" — without it, a widget hosting many virtual
    // items (the ribbon, a tab bar, a list) can never expire its own tip.
    struct Resolved {
        QString text;
        QRect   rect;      // null = the whole widget
        bool ok() const { return !text.isEmpty(); }
    };

    // Ordered resolvers. First hit wins; the property is the fallback.
    static Resolved resolve(QWidget* w, const QPoint& pos) {
        if (auto* tabs = qobject_cast<QTabBar*>(w)) {
            const int i = tabs->tabAt(pos);
            if (i >= 0 && !tabs->tabToolTip(i).isEmpty())
                return {tabs->tabToolTip(i), tabs->tabRect(i)};
        }
        if (auto* hdr = qobject_cast<QHeaderView*>(w)) {
            const int li = hdr->logicalIndexAt(pos);
            if (li >= 0 && hdr->model()) {
                const QVariant v = hdr->model()->headerData(li, hdr->orientation(),
                                                            Qt::ToolTipRole);
                if (v.isValid() && !v.toString().isEmpty()) {
                    const int p = hdr->sectionViewportPosition(li);
                    const QRect r = hdr->orientation() == Qt::Horizontal
                        ? QRect(p, 0, hdr->sectionSize(li), hdr->height())
                        : QRect(0, p, hdr->width(), hdr->sectionSize(li));
                    return {v.toString(), r};
                }
            }
        }
        // Item views deliver ToolTip to the VIEWPORT, so resolve from there
        // and map the index rect back into viewport coordinates.
        if (auto* vp = w->parentWidget()) {
            if (auto* view = qobject_cast<QAbstractItemView*>(vp);
                view && view->viewport() == w) {
                const QModelIndex idx = view->indexAt(pos);
                if (idx.isValid()) {
                    const QVariant v = idx.data(Qt::ToolTipRole);
                    if (v.isValid() && !v.toString().isEmpty())
                        return {v.toString(), view->visualRect(idx)};
                }
            }
        }
        if (auto* menu = qobject_cast<QMenu*>(w)) {
            if (QAction* a = menu->actionAt(pos);
                a && !a->toolTip().isEmpty() && a->toolTip() != a->text())
                return {a->toolTip(), menu->actionGeometry(a)};
        }
        if (!w->toolTip().isEmpty()) return {w->toolTip(), QRect()};
        return {};
    }

protected:
    bool eventFilter(QObject* obj, QEvent* e) override {
        const auto t = e->type();

        if (t == QEvent::ToolTip) {
            auto* w = qobject_cast<QWidget*>(obj);
            if (!w) return false;
            auto* he = static_cast<QHelpEvent*>(e);
            const Resolved r = resolve(w, he->pos());
            if (!r.ok()) {
                // Nothing here — this is a HOP IN A WALK, not a decision.
                // Let the event carry on to the parent. Dismissing here is
                // what made every container-level tooltip flicker.
                return false;
            }
            // Idempotent: same target, same text, and the shared widget is
            // still showing THAT text. The last clause matters because other
            // owners (the type chooser, the editor) can repopulate the shared
            // instance behind us; trusting only m_lastText showed one
            // widget's tooltip under another widget's identity.
            auto* tip = sharedRcxTooltip();
            if (m_target == w && m_lastText == r.text
                && tip->isVisible() && tip->bodyText() == r.text)
                return true;
            m_target   = w;
            m_lastText = r.text;
            m_itemRect = r.rect;
            showRcxTooltip(he->globalPos(), r.text, w->font());
            return true;   // suppress Qt's native tooltip widget
        }

        // ── Follow the mouse ─────────────────────────────────────────────
        // The tooltip is UPDATED IN PLACE here, never hidden-then-reshown.
        // That distinction is the whole fix for "it flashes really fast when I
        // move the mouse around": Qt re-arms its tooltip wake-up at ~20 ms
        // once a tip is up, so a real hover is a 20 ms stream of interleaved
        // MouseMove + ToolTip. Anything that hides between two ticks strobes.
        // showAt() only calls show() when the widget is hidden, so
        // repopulating a visible tip emits no Hide/Show pair at all.
        if (t == QEvent::MouseMove) {
            auto* tip = sharedRcxTooltip();
            if (!m_target || !tip->isVisible()) return false;
            // The EVENT's position, not QCursor::pos(). The OS cursor is a
            // different source of truth: it lags, it clamps to the physical
            // screen, and it disagrees with the widget geometry whenever the
            // window is not where the compositor thinks. Reading it here made
            // "cursor left the widget" fire while the pointer was sitting
            // still in the middle of a button.
            const QPoint g = static_cast<QMouseEvent*>(e)->globalPosition().toPoint();
            const QPoint local = m_target->mapFromGlobal(g);
            // Leaving the WIDGET is the only thing that hides on a move.
            if (!m_target->rect().contains(local)) { clear(); return false; }
            tip->keepAlive();   // still on the control — do not let it expire
            const Resolved r = resolve(m_target, local);
            if (!r.ok()) {
                // Still inside the widget, but over dead space: the 2-px gap
                // between two ribbon glyphs, a panel caption, the strip
                // margin. KEEP SHOWING. Hiding here is what made the tooltip
                // "flicker as I jiggle the mouse around the button", and it is
                // why Modify felt broken while Home felt fine — Home's buttons
                // are wide words, Modify's are 22-px glyphs separated by 2 px,
                // so on Modify almost every small movement crossed dead space.
                // The user's mental model is the right one: while the pointer
                // is on the control, the tip stays put.
                return false;
            }
            if (r.text != m_lastText) {
                // Moved to a different virtual item of the same widget (a
                // ribbon button, a tab, a list row). Re-anchor and repaint;
                // the window stays mapped throughout.
                m_lastText = r.text;
                m_itemRect = r.rect;
                showRcxTooltip(g, r.text, m_target->font());
            }
            return false;
        }
        if (t == QEvent::MouseButtonPress || t == QEvent::Wheel
                   || t == QEvent::KeyPress) {
            clear();
        } else if (t == QEvent::Leave) {
            if (m_target && obj == m_target.data()) clear();
        } else if (t == QEvent::Hide) {
            if (m_target && (obj == m_target.data() || obj == m_target->window()))
                clear();
        }
        return false;
    }

private:
    void clear() {
        m_target = nullptr;
        m_lastText.clear();
        m_itemRect = QRect();
        rcx::dismissRcxTooltip();
    }

    QPointer<QWidget> m_target;
    QString           m_lastText;
    QRect             m_itemRect;   // in m_target's coords; null = whole widget
};

} // namespace rcx
