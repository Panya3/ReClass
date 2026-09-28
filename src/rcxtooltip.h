#pragma once
#include <QWidget>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>
#include <QApplication>
#include <QMouseEvent>
#include <QTimer>
#include <QList>
#include "themes/thememanager.h"

namespace rcx {

// ── Modern arrow tooltip ──
// Draws a rounded-rect body with a triangular arrow whose tip touches
// the anchor point (center of the dwell area).
//
// Bypasses Fusion/CSS/DWM entirely — everything is manual QPainter on a
// WA_TranslucentBackground layered window.  The DarkTitleBar property is
// pre-set to prevent DarkApp::notify from calling DwmSetWindowAttribute
// (which was the root cause of the previous transparent-window failure).
//
// Usage:
//   tip->setTheme(bg, border, titleCol, bodyCol, sepCol);
//   tip->populate("Title", "line1\nline2", font);
//   tip->showAt(QPoint(midX, lineBottom));  // arrow tip at this point
//   tip->dismiss();

class RcxTooltip : public QWidget {
public:
    static constexpr int kArrowH = 8;
    static constexpr int kArrowW = 14;
    static constexpr int kRadius = 0;  // square corners — matches the app's no-rounded-corner convention (arcTo no-ops at r=0)
    static constexpr int kPad    = 10;
    static constexpr int kGap    = 4;
    static constexpr int kMaxW   = 550;

    explicit RcxTooltip(QWidget* parent = nullptr)
        : QWidget(parent, Qt::ToolTip | Qt::FramelessWindowHint | Qt::WindowTransparentForInput)
    {
        // ── Key fix: prevent DwmSetWindowAttribute on this window ──
        // DarkApp::notify checks this property and skips DWM calls.
        // Without this, DWMWA_USE_IMMERSIVE_DARK_MODE breaks WS_EX_LAYERED
        // alpha compositing on Windows 10/11.
        setProperty("DarkTitleBar", true);

        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_DeleteOnClose, false);
        // Pass every mouse event through to the window under the tooltip.
        // Without this the tooltip sits on top of the cursor's actual target,
        // the original widget gets a Leave, its owner dismisses the tip, the
        // cursor is then over the widget again, and the tip re-shows. Strobe.
        //
        // BOTH of these are required, and the attribute ALONE is not enough:
        // WA_TransparentForMouseEvents is a QWidget-level attribute that Qt
        // does NOT translate into WS_EX_TRANSPARENT for a top-level native
        // window — only the Qt::WindowTransparentForInput FLAG (above) does.
        // Measured on the live tooltip HWND: with the attribute alone the
        // EXSTYLE was 0x00080088 (LAYERED=1, TRANSPARENT=0) and
        // WindowFromPoint at the anchor returned the TOOLTIP; with the flag it
        // is 0x000800A8 and WindowFromPoint returns the app window.
        //
        // The bridge anchors AT THE CURSOR, so its window contains the pointer
        // and this bit is what stops Windows retargeting the mouse to it. The
        // editor's own tooltip anchors at the bottom edge of the hovered LINE,
        // never under the cursor — which is exactly why the editor's tooltips
        // always behaved and every other surface flickered.
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setMouseTracking(true);

        // ONE tooltip on screen at a time, whoever owns it. The editor keeps
        // a private instance and the bridge drives a shared one; before this
        // registry both could be up simultaneously, because neither knew the
        // other existed.
        liveTips().append(this);

        // Qt's tooltips self-expire; this one had no timer at all, so a tip
        // could outlive its target (dock closed, list scrolled, window
        // switched) and sit on screen forever. Duration scales with length
        // the way QTipLabel's does: long text gets longer to read.
        m_expiry.setSingleShot(true);
        QObject::connect(&m_expiry, &QTimer::timeout, this, [this]() { dismiss(); });
    }

    ~RcxTooltip() override { liveTips().removeAll(this); }

    // Every RcxTooltip alive in the process, in construction order.
    static QList<RcxTooltip*>& liveTips() {
        static QList<RcxTooltip*> s_all;
        return s_all;
    }
    // Hide every other tooltip. Called from showAt, so no call site has to
    // remember to do it.
    static void dismissOthers(RcxTooltip* keep) {
        for (RcxTooltip* t : liveTips())
            if (t != keep && t->isVisible()) t->hide();
    }

    // What is ACTUALLY on screen — the bridge's idempotence guard used to
    // trust its own cached string, so another owner repopulating the shared
    // instance went unnoticed and a button could show the type chooser's text.
    QString bodyText()  const { return m_body; }
    QString titleText() const { return m_title; }

    void setTheme(const QColor& bg, const QColor& border,
                  const QColor& title, const QColor& body, const QColor& sep) {
        m_bg = bg; m_border = border;
        m_titleCol = title; m_bodyCol = body; m_sepCol = sep;
    }

    void populate(const QString& title, const QString& body, const QFont& font) {
        if (title == m_title && body == m_body && isVisible()) return;
        m_title = title; m_body = body;
        m_font = font;
        m_font.setPointSizeF(font.pointSizeF() * 0.9);
        m_bold = m_font; m_bold.setBold(true);
        recalc();
    }

    // `anchor`: global screen point where the arrow tip touches.
    // Typically the center-bottom of the hovered span.
    //
    // `preferAbove`: when true, force the tooltip body to render ABOVE
    // the anchor (arrow points downward into the anchor) whenever there
    // is room above. Falls back to below if the top of the screen is
    // hit. When false (default), the legacy behaviour applies — body
    // below if it fits, else above. Used by the byte-selection tooltip
    // so it pops upward off the selected row (anchored at the top of
    // the line) and doesn't cover other hex rows below.
    void showAt(const QPoint& anchor, bool preferAbove = false) {
        QRect scr = screenAt(anchor);
        int w = m_bw, h = m_bh + kArrowH;
        if (preferAbove) {
            m_up = (anchor.y() - h < scr.top());  // flip down only if no room above
        } else {
            m_up = (anchor.y() + h <= scr.bottom());
        }
        int x = qBound(scr.left() + 2, anchor.x() - w / 2, scr.right() - w - 2);
        int y = m_up ? anchor.y() : anchor.y() - h;
        m_ax = qBound(kRadius + kArrowW/2 + 1, anchor.x() - x,
                       w - kRadius - kArrowW/2 - 1);
        setFixedSize(w, h);
        move(x, y);
        dismissOthers(this);
        if (!isVisible()) show();
        m_expiry.start(expiryMs());
        update();
    }

    // Still hovering the same thing: push the expiry out, the way Qt's own
    // QToolTip::showText restarts QTipLabel's expire timer on every repeat
    // tick. Without this the timer is only ever armed by showAt, which the
    // bridge calls only when the TEXT changes — so parking on one button hid
    // the tip after exactly 5 s and the next tick re-showed it.
    void keepAlive() { if (isVisible()) m_expiry.start(expiryMs()); }

    void dismiss() { m_expiry.stop(); if (isVisible()) hide(); }

protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        // Body rect (excludes arrow space)
        QRectF b(0.5, m_up ? kArrowH + 0.5 : 0.5,
                 width() - 1.0, m_bh - 1.0);
        qreal r = kRadius, ax = m_ax, ah = kArrowW / 2.0;

        // ── Single contiguous path: rounded rect + arrow notch ──
        // No QPainterPath::united() — that causes junction artifacts.
        // Clockwise from top-left, inserting the arrow inline.
        QPainterPath pp;
        pp.moveTo(b.left() + r, b.top());
        if (m_up) {
            pp.lineTo(ax - ah, b.top());
            pp.lineTo(ax, 0.5);
            pp.lineTo(ax + ah, b.top());
        }
        pp.lineTo(b.right() - r, b.top());
        pp.arcTo(b.right() - 2*r, b.top(), 2*r, 2*r, 90, -90);
        pp.lineTo(b.right(), b.bottom() - r);
        pp.arcTo(b.right() - 2*r, b.bottom() - 2*r, 2*r, 2*r, 0, -90);
        if (!m_up) {
            pp.lineTo(ax + ah, b.bottom());
            pp.lineTo(ax, height() - 0.5);
            pp.lineTo(ax - ah, b.bottom());
        }
        pp.lineTo(b.left() + r, b.bottom());
        pp.arcTo(b.left(), b.bottom() - 2*r, 2*r, 2*r, 270, -90);
        pp.lineTo(b.left(), b.top() + r);
        pp.arcTo(b.left(), b.top(), 2*r, 2*r, 180, -90);
        pp.closeSubpath();

        p.setPen(QPen(m_border, 1));
        p.setBrush(m_bg);
        p.drawPath(pp);

        // ── Content: title + separator + body ──
        qreal cy = (m_up ? kArrowH : 0) + kPad;
        QFontMetrics tf(m_bold), bf(m_font);

        if (!m_title.isEmpty()) {
            p.setFont(m_bold); p.setPen(m_titleCol);
            p.drawText(QPointF(kPad, cy + tf.ascent()), m_title);
            cy += tf.height() + kGap;
            p.setPen(m_sepCol);
            p.drawLine(QPointF(kPad, cy), QPointF(width() - kPad, cy));
            cy += 1 + kGap;
        }
        p.setFont(m_font); p.setPen(m_bodyCol);
        for (const auto& l : m_lines) {
            p.drawText(QPointF(kPad, cy + bf.ascent()), l);
            cy += bf.lineSpacing();
        }
    }

private:
    // Mirrors QTipLabel: a base dwell plus reading time for long text.
    int expiryMs() const {
        const int n = m_body.size() + m_title.size();
        return qBound(5000, 5000 + 40 * qMax(0, n - 60), 20000);
    }

    static QRect screenAt(const QPoint& pt) {
        auto* s = QApplication::screenAt(pt);
        return s ? s->availableGeometry() : QRect(0, 0, 1920, 1080);
    }

    // Wrap one logical line to `avail` px, breaking on spaces and falling
    // back to a hard character break for an unbroken run (a long path, a
    // mangled symbol). Returns at least one piece so an empty line still
    // occupies a row.
    QStringList wrapLine(const QString& line, int avail, const QFontMetrics& fm) const {
        QStringList out;
        if (fm.horizontalAdvance(line) <= avail || avail <= 0) { out << line; return out; }
        QString cur;
        const QStringList words = line.split(QLatin1Char(' '));
        for (const QString& w : words) {
            const QString cand = cur.isEmpty() ? w : cur + QLatin1Char(' ') + w;
            if (fm.horizontalAdvance(cand) <= avail) { cur = cand; continue; }
            if (!cur.isEmpty()) { out << cur; cur.clear(); }
            // A single word wider than the line: hard-break it.
            QString piece = w;
            while (fm.horizontalAdvance(piece) > avail && piece.size() > 1) {
                int n = piece.size();
                while (n > 1 && fm.horizontalAdvance(piece.left(n)) > avail) --n;
                out << piece.left(n);
                piece = piece.mid(n);
            }
            cur = piece;
        }
        if (!cur.isEmpty()) out << cur;
        if (out.isEmpty()) out << QString();
        return out;
    }

    void recalc() {
        QFontMetrics tf(m_bold), bf(m_font);
        const int textRowH = bf.lineSpacing();
        const int avail = kMaxW - 2 * kPad;

        // WRAP, don't clip. The body used to be drawn with plain drawText at
        // a width clamped to kMaxW, so any tooltip longer than ~550 px was
        // silently cut mid-word — several shipped strings are well over it.
        m_lines.clear();
        for (const QString& raw : m_body.split(QChar::LineFeed))
            m_lines += wrapLine(raw, avail, bf);

        int maxW = m_title.isEmpty() ? 0 : tf.horizontalAdvance(m_title);
        for (const auto& l : m_lines) maxW = qMax(maxW, bf.horizontalAdvance(l));

        m_bw = qMin(maxW + 2 * kPad, kMaxW);
        m_bh = kPad + (m_title.isEmpty() ? 0 : tf.height() + kGap + 1 + kGap)
             + m_lines.size() * textRowH + kPad;
    }

    QString m_title, m_body;
    QStringList m_lines;
    QFont m_font, m_bold;
    QColor m_bg{30, 30, 30}, m_border{60, 60, 60};
    QColor m_titleCol{220, 220, 220}, m_bodyCol{180, 180, 180}, m_sepCol{60, 60, 60};
    bool m_up = true;
    int m_ax = 0, m_bw = 0, m_bh = 0;
    QTimer m_expiry;
};

// Shared process-wide tooltip. The global QEvent::ToolTip bridge (set up
// in main.cpp) and the few direct callers (TypeSelectorPopup delegate)
// route through this single instance so we don't litter the heap with
// per-widget tooltip objects, and the visible tip simply repositions
// when the user moves between widgets.
//
// Lazy-init on first call; theme is read from ThemeManager each show
// so theme changes apply without rewiring.
inline RcxTooltip* sharedRcxTooltip() {
    static RcxTooltip* s_tip = nullptr;
    if (!s_tip) s_tip = new RcxTooltip(nullptr);
    return s_tip;
}

// Convenience: show plain text from a global cursor position. Caller
// passes their preferred font (usually the widget's font or the editor
// font). Theme colors pulled from ThemeManager.
inline void showRcxTooltip(const QPoint& globalAnchor,
                            const QString& text,
                            const QFont& font) {
    auto* tip = sharedRcxTooltip();
    const auto& theme = ThemeManager::instance().current();
    // Body in theme.text, not textDim: this widget replaced Qt's native
    // tooltip, whose stylesheet used theme.text, so a dimmer body was one of
    // the tells that made the two systems look like different widgets.
    tip->setTheme(theme.backgroundAlt, theme.border,
                  theme.text, theme.text, theme.border);
    tip->populate(QString(), text, font);
    tip->showAt(globalAnchor);
}
inline void dismissRcxTooltip() {
    if (auto* t = sharedRcxTooltip(); t->isVisible()) t->dismiss();
}

} // namespace rcx
