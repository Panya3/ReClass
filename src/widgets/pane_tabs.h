#pragma once

#include "themes/theme.h"
#include "paintutil.h"

#include <QString>

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

} // namespace rcx
