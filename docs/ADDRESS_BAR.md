# The address bar

The 26-px strip between the document tabs and the command row (Scintilla
line 0). It is Explorer's bar for a class view: it answers **where am I
reading from** (the source chip), **where in it** (the base address and the
drill trail) and **where can I go** (Back / Forward / history / Up, a chevron
per level, recent places), and lets you act on each. One per split pane; the
controller pushes one `AddressBarState` per refresh and every pane shows the
same trail (only the pane a gesture came from scrolls).

```
 ← → ˅ ↑ │ ▣● REECLASS.exe ˅ › <REECLASS.exe>+0x1234  → 0x7FF6DEAD1234  « QWidget.d_ptr › QObjectPrivate.parent › QObjectData ›            ˅
 ────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────────
  back fwd hist up   src        src.chev root.chev base                        overflow crumb:2  chev:2 crumb:3  chev:3  crumb:4  chev:4  space recent
```

Paper, not chrome: the band is `editorPaperColor` (it sits inside the
document column), the seam under it one device row of `containerBorderColor`,
hover `t.hover`, pressed `pressedFill(t)`, tones walk `text > textDim >
textMuted > textFaint`, and there is **no accent anywhere on the bar** —
purple is spent on document tabs and real selection, not on a navigation aid.
The first ink (the Back glyph) sits on `kGutter`, the same device column every
other strip starts on.

## Pieces

| File | Role |
|---|---|
| `src/widgets/address_bar.h` | `rcx::AddressBar` — ONE custom-painted, header-only widget on the RibbonBar template: lazily recomputed `Layout` of string-addressed cells, hover / pressed / menu-open / disabled / keyboard-focused states, the 6-step overflow rule, tooltips, context menus, the two edit scopes over one hidden `QLineEdit`, every dropdown as a transient `QMenu`. No `Q_OBJECT`: outbound events are a `Callbacks` struct of `std::function` that `RcxEditor` bridges to signals. `Qt::NoFocus` at rest. |
| `src/widgets/address_bar_model.h` | Core-only model: `AddressBarState` (the value type pushed per refresh, `operator==` guards the relayout), `siblingFieldsOf` / `rootClassEntries` / `trailPathText` / `resolveDrillPath` over a `NodeTree`, and `AddressBarTreeQueries` (what the menus and the path edit pull on demand). |
| `src/nav_history.h` | `NavEntry` (a PLACE: view root, trail, base + formula, saved-source index, scroll anchor, label) and `NavHistory` (push dedupes the head, truncates forward, cap 50, `back()` / `forward()` skip stale entries, `forgetSource` / `forgetAllSources` follow the saved-source list). |
| `src/address_callbacks.h` | `makeAddressCallbacks(Provider*, ptrSize)` — the one `AddressParserCallbacks` block (module lookup, memory read, kernel paging) every evaluator shares. |
| `src/controller.cpp` | `addressBarState()` / `pushAddressBarState()`, `rebaseTo`, `switchSibling`, `navigateToDrillPath`, `collapseToFocus`, `recordNav` / `goBack` / `goForward` / `goUp` / `jumpToHistory` / `restoreNav`, `containerOf` and the focus-path reconcile. |
| `src/editor.cpp` | Owns the bar (layout item 0 above `m_sci`), bridges `Callbacks` to signals, routes the shortcuts through its KeyPress switch. Line 0 under it is the class header alone (see below). |
| `src/paintutil.h` | `kGutter`, the device-exact edge fills (seam, divider, focus ring), `drawPixmapSnapped`, `pressedFill`. |
| `tools/address_bar_render.cpp` | Harness: `address_bar_render <prefix> [theme]` — 1 / 3 / 6-crumb bars at 300 / 480 / 760 / 1080 / 1920 px, one sheet per width, every cell's rect + dpr on stdout. |
| `tools/editor_render.cpp drill` | The in-context render (F06 in the book): a real controller + editor, the trail grown by a click, the bar grabbed 4× to `bc_bar_4x.png` and the chip's five liveness states to `bc_bar_states_4x.png`. |
| `tests/test_breadcrumb.cpp`, `tests/test_address_bar_model.cpp`, `tests/test_hairline_dpr.cpp` | Controller + widget (geometry, pixels, menus, edits, history, keyboard, split panes), the pure model + `NavHistory`, and the focus ring at 100 / 125 %. |

## Item ids

Every cell is addressed by a string id — the namespace `itemRect(id)`,
`itemIdAt(pos)`, `focusId()` and the harness speak. Left to right:

| Id | Cell | Width | Click |
|---|---|---|---|
| `back` `fwd` | Back / Forward, `arrow-left/right.svg` 14 px | 22 | one history step; right-click or press-and-hold on `back` opens the history list (so it stays reachable when `hist` is dropped) |
| `hist` | `chevron-down.svg` 12 px | 14 | the history menu: Back entries nearest first, a separator, Forward entries nearest first |
| `up` | `arrow-up.svg` | 22 | the parent crumb (`collapseToFocus` on the deepest hop) |
| — | field divider: one device column, `containerBorderColor`, inset 4 px | 6 + 1 + 6 | everything right of it is "the field" |
| `src` | the source chip: 16-px `iconForProvider` icon (0.40 opacity when disconnected), a 6-px liveness dot at its bottom-right, the provider name (`textDim`, hover `text`); empty provider = `plug.svg` + "Select source" | 4 + 16 + 4 + text + 2 | the source chooser, anchored under the chip; the chip reads pressed while it is up |
| `src.chev` | `chevron-down` `textFaint` | 14 | same as `src` (one hover group, one keyboard stop) |
| `root.chev` | `chevron-right`, flips to `chevron-down` on hover | 14 | menu of the root classes → `setViewRootId` |
| `base` | the formula if set else `0x…`, plus a `textMuted` `→ 0x…` suffix (what the formula resolves to; only beside a formula) | 6 + text + suffix + 6 | the base edit |
| `overflow` | `«` in `textDim`, present only when crumbs are folded | 18 | menu of the hidden crumbs, root first → `onCrumb(i)` |
| `crumb:<i>` | one crumb: `Class.field` for an ancestor, bare `Class` for the deepest | 6 + text + 6 | ancestor → collapse below + scroll (one undo entry, one history entry); the deepest is inert (no fill, no hand) and only scrolls its header up |
| `chev:<i>` | after crumb *i*, same › → ˅ flip | 14 | menu of the drillable fields of the class at crumb *i*, the trail's hop checked; the trailing one drills further |
| `space` | the stretch, IBeam cursor | rest | the path edit |
| `recent` | `chevron-down` `textFaint`, pinned at `width − 6 − 16` | 16 | the places menu: Recent (`GotoAddressDialog::loadRecent`), Bookmarks, `<module>` names, then "Go to address… Ctrl+G", "Clear recent" → `rebaseTo` |

Liveness dot: `Live = indHintGreen`, `Stale = focusGlow`, `Disconnected =
markerError`, `Static` / none = no dot (the status chip's mapping, existing
tokens only).

## Tones and states

| | |
|---|---|
| Crumbs | a lone crumb is `textDim` regular (it repeats the doc tab); from depth 2 the ancestors are `textDim` and the deepest is `text` DemiBold — the single weight step on this surface. |
| Rest | nothing filled. Nav glyphs, chevrons and the chip's chevron are `textDim` / `textFaint`. |
| Hover | `t.hover` fill under the cell, tone → `text`; a chevron flips › → ˅. The chip and its chevron light as one rect. |
| Pressed / menu open | `pressedFill(t)` for as long as the menu is up (`aboutToHide` releases it). |
| Disabled | `setOpacity(0.40)` around the whole cell — never `textDim × 0.4`. Back / Forward / Up / history follow the controller's `canBack` / `canForward` / `canUp`; a disabled cell is laid out (the field never shifts when history appears) and its click is ignored. |
| Keyboard focus | one device-exact 1-px ring in `borderFocused` (the four edge fills — a `QPen` rect is two rows at 125 %). |
| Editing | the overlay is up; the seam row under the field turns `borderFocused` while the text parses / resolves and `markerError` while it does not (or the controller refused it). |
| Tooltips | the widget's `toolTip` mirrors the hovered cell and the app's `GlobalTooltipBridge` shows it; `QEvent::ToolTip` is never handled here and nothing is dismissed on a hover change (the ribbon rule). Per crumb: `QWidgetPrivate  @ 0x7FF6…` from `LineMeta::ptrBase`, or `@ (unreadable)`. |

## Overflow

Applied in `relayout()` in this order until the strip fits; each knob turns
only as far as the current overshoot needs (a 30-px squeeze costs the base
three characters, not half of it).

1. **The base** — middle-elide the formula by the overshoot (floor 90 px of text); when the floor cannot absorb it, drop the `→ 0x…` suffix and give the formula back what that freed.
2. **The source name** — the same way, floor 80 px.
3. **Fold crumbs into `«`** from the ROOT, one at a time — never the deepest.
4. **Middle-elide the deepest** to 60 px.
5. **Drop `up` and `hist`** (below ~420 px).
6. **Drop the trailing chevron.**

Best effort after that: the deepest crumb is never dropped, because it is the
one thing the bar exists to name. With a process source and a formula base
the floor is about 395 px; narrower than that, the deepest crumb and `recent`
are laid out past the right edge (the 300-px sheet from the harness shows
it). The elision is DISPLAY only — `state()` carries the full strings and the
base edit always opens on the full formula (the command row used to feed its
ellipsis to the parser and silently no-op).

## Edits

Two scopes, one hidden `QLineEdit` in `PanelSearchField`'s interior; the bar
paints the focus seam under it. Enter is the only commit; Esc and losing
focus restore (the Explorer / Goto-dialog rule). While an overlay is up the
layout is frozen — a live tick's state push is stored and its relayout waits
for the edit to end — and the cells it covers stop answering to hover.

| Scope | Opened by | Text | Grammar | Commit |
|---|---|---|---|---|
| **Base** | click `base`, F2 on `base` in keyboard mode, "Edit address" in its context menu | the FULL formula (never the elided display), selected; the overlay is at least 180 px | anything `AddressParser` takes: `0x1000`, `<mod.exe>+0x40`, `[<mod.exe>+0x10]*2`. Live: `fmt::validateBaseAddress` + the controller's evaluator drive the seam colour and a `→ 0x…` preview; Down / `▾` opens a fuzzy-filtered menu of recents, bookmarks and `Provider::modulesCached()` as `<mod.exe>` | `onBaseCommit` → `rebaseTo`: strip backticks / CRLF → evaluate → bare-literal rule → `cmd::ChangeBase` only on change → `GotoAddressDialog::pushRecent`. Invalid input is REFUSED: the overlay stays, the seam goes `markerError`, the status bar says `Base: <error>`. |
| **Path** | click `space`, Alt+D, Ctrl+L, F2 on a crumb in keyboard mode | the dotted trail (`trailPathText`): `RcxEditor.vptr.parent`, from the base's right edge to `recent` | `Root.field.field…` — the first segment names a top-level struct by type name or name, every later one a drillable field of the class the segments before it reach (a pointer's `refId` class or an embedded struct). Whitespace, a trailing dot and doubled dots are tolerated. Down completes the last segment from that class's drillable fields. | `onPathCommit` → `navigateToDrillPath`: one macro "Navigate to path" (expand every collapsed hop), `focusPath` = the resolved hops, one history entry. An unknown segment is refused with the seam in `markerError` and a hint naming it (`No field 'nope' in RcxEditor`). |

## The command row under it

Scintilla line 0 is the class header alone:

```
[▸] struct Player {
```

The chevron opens the view chooser (`typeSelectorRequested`), the class
name is an inline rename (`EditTarget::RootClassName`), the keyword converts
through its right-click menu, and the brace is furniture. The source label
and the base address that used to sit between the chevron and the keyword
are the bar's cells now: line 0 scrolls away with the document, the bar
never does, so the two navigation surfaces no longer hide each other, and
there is one base edit, not two. `buildCommandRowText` (core.h) is the one
builder — `compose.cpp`'s placeholder and `RcxController::updateCommandRow`
both produce that shape — and `commandRowChevronSpan`,
`commandRowRootTypeSpan`, `commandRowRootNameSpan` are the only parsers
left; `EditTarget::BaseAddress` went with the cell. The MCP `status.set`
tool may overwrite the row with `[▸] [Claude: text]`: the text is inert (no
parser anchors on it, nothing is tinted editable, a click starts nothing),
and the tool strips newlines and the old `▾` glyph so a status can never
grow a control of its own.

## Sideways: the chevron menus

`chev:<i>` lists **all** drillable siblings of the class at crumb *i*
(`siblingFieldsOf`: `childrenOf` filtered by `drillTargetId`, stable-sorted by
offset, union members in declaration order), expanded or not, the trail's hop
checked. A pick is a **switch**, not a jump between things already open:
`switchSibling(level, id)` collapses the current hop and expands the chosen one
in ONE undoable macro ("Switch to <field>"), sets `focusPath = prefix + id`,
records one history entry and scrolls the new class header up. Picking the hop
already there is a no-op (no undo entry, no history entry). The chevron after
the deepest crumb lists that class's fields with nothing checked: drill
further. `root.chev` lists the other roots (`rootClassEntries`, the same order
as `rootClassNames`): a pick is a root jump — new view root, empty trail, one
history entry.

## History versus undo

Back / Forward are Explorer's, not the undo stack's. An entry is a **place**
(view root, trail, base + formula, saved-source index) plus a scroll anchor
and a label; it is recorded at the gesture that leaves it — F12, `rebaseTo`,
a source switch, a sibling or root pick (the bar's `root.chev` and the line-0
class chooser alike), a path edit, a crumb click, Up — never inside
`setViewRootId` (load, new tab and delete-root call that too), never during a
document load or a restore.

- **Raw restore.** `restoreNav` writes `m_viewRootId`, `m_focusPath`,
  `tree.baseAddress` / `baseAddressFormula` and the active source directly;
  no `cmd::ChangeBase` is pushed. The one thing that goes through the undo
  stack is a "Reopen path" macro when a hop on the restored trail was
  collapsed since (and none at all when the path is still open — the common
  case: Back after F12). Undo / redo never move history.
- **One gesture = at most one undo entry + at most one history entry.** A
  Back / Forward = zero of both (plus the optional reopen macro). Pushing the
  place already on top is a no-op, so a gesture that lands where it started
  (rebase to the same address, re-picking the current root) never grows the
  stack.
- **Ctrl+Z after Back still undoes the original rebase** — the entry that
  rebase pushed is exactly where it was.
- **Stale entries** (a deleted root, a removed hop) are skipped by `back()` /
  `forward()` and left out of `backEntries()` / `forwardEntries()`, so a
  history-menu row's position equals the number of steps its pick walks.
  `canBack` / `canForward` are validity-aware: a cell is never lit for an
  entry that cannot be restored.
- **Saved sources.** An entry's `activeSourceIdx` is a raw index into the
  controller's saved-source list; `removeSavedSource` and `clearSources`
  mirror themselves into the history (`forgetSource` shifts later indices
  down and marks the removed one `kNavSourceRemoved`; `forgetAllSources`
  marks them all). A restore whose source is gone — removed, its file
  vanished, or the attach refused — keeps the current source, restores the
  rest of the place and says so on the status bar.
- **A Back across a File-source switch wipes the undo stack.** Switching to a
  File source is `RcxDocument::loadData`, which clears the undo stack — the
  pre-existing behaviour of every source switch, not something the history
  adds. The history itself survives.
- **Per document, not per pane.** The trail and the history are
  controller-global: both panes of a split show the same state and only the
  pane the gesture came from scrolls (its first visible row is the anchor the
  entry records).

## Shortcuts

Routed through `RcxEditor::eventFilter`'s KeyPress switch (where F2 / F12
live), so the bar itself registers no shortcuts and never takes focus at
rest. During a Scintilla inline edit the arrows keep the caret and navigation
waits for Enter / Esc.

| Key | Does |
|---|---|
| Alt+D, Ctrl+L | open the path edit (Explorer's and the browser's key; the scanner's Ctrl+L is a `WidgetWithChildrenShortcut` on its own panel) |
| Alt+Left / Alt+Right | Back / Forward |
| Alt+Up | Up one level (the parent crumb); Alt+Down is nobody's |
| XButton1 / XButton2 (mouse Back / Forward) | Back / Forward; consumed always, inert during an inline edit |
| F6 | keyboard mode: focus into the bar, starting at the deepest crumb |
| in keyboard mode: Left / Right, Tab / Shift+Tab, Home / End | walk the enabled cells (the chip is one stop, `space` is none) |
| Enter, Space | synthesise a press + release at the cell's centre (the ordinary click path, menus included) |
| Down | open the focused cell's menu when it has one |
| F2 | the base edit on `base`, the path edit on a crumb |
| Esc | back to the document (`scintilla()->setFocus()`, the bar returns to `NoFocus`) |
| in an edit: Enter / Esc / Down | commit / restore / the completion or places menu |
| Ctrl+G | the Goto dialog (main window); also the places menu's "Go to address…" |

A real mouse press anywhere on the bar, or focus leaving it for anything but
a popup, ends keyboard mode.

## Context menus

| On | Items |
|---|---|
| a crumb | Copy path (`crumbPathText(i)`, the dotted path down to it) · Copy address · Copy class name |
| `base` | Copy address · Copy formula · Edit address · Go to address… Ctrl+G |
| the chip | Copy source name · Change source… · Refresh F5 |
| `back` | the history list (the narrow-width fallback for `hist`) |

## Test surface and harness

- `itemRect(id)` / `itemIdAt(pos)` — geometry by id; null when not laid out.
  `segments()` — the rendered trail (`«` first when folded, then the full
  labels); `overflowMenuLabels()`, `sourceDisplayText()`,
  `baseDisplayText()` (the ELIDED text, against `state()`'s full strings),
  `sourceIconRect()`, `livenessDotRect()`, `crumbPathText(i)`.
- `state()`, `stateApplyCount()` — the equal-state early return.
- `isEditing()` / `isBaseEditing()` / `isPathEditing()`, `editText()`,
  `editRect()`, `editCoveredRect()`, `editTextValid()`, `editPreviewText()`;
  `baseCommitFinished` / `pathCommitFinished` are the controller's verdicts.
- `enterKeyboardMode()`, `inKeyboardMode()`, `focusId()`, `traversalIds()`;
  `setHoldDelayMs()`, `historyMenuOpenCount()` for the press-and-hold.
- Menus are `QMenu` children named `rcxAddressBarHistoryMenu`,
  `rcxAddressBarSiblingMenu`, …; on the hidden desktop a popup is closed at
  the first event pump, so a test opens it with `QTest::mousePress`, reads
  and triggers it synchronously, then releases.
- Pixels: `grab()` at real width, `countColour` over `devRect`; the seam is
  the bottom device row, the hover fill `t.hover`, and there must be no
  `indHoverSpan` pixel anywhere on the bar.
- Line 0: `test_command_row` parses `buildCommandRowText`'s output back
  through the real span functions; `test_breadcrumb` checks a live
  controller's line 0 equals it and stays byte-identical across a rebase;
  `test_editor` covers the row's tones, the root-name edit's keystroke rules
  and the inert MCP status row.
- Harness: build `address_bar_render` (EXCLUDE_FROM_ALL), run it on the
  hidden desktop (`tools/run_tests_hidden.py`'s `run_hidden()`; set
  `QT_SCALE_FACTOR=1.25` in the driver's environment for the HiDPI pass) as
  `address_bar_render <prefix> [tw|vs|<path>.json]`, then read
  `<prefix>_<w>.png` — three bars (1, 3, 6 crumbs) stacked with magenta gaps
  — and crop by the printed rects (logical × dpr, plus the row's device y).

## Rules

- No new theme tokens; no accent on the bar; no `border-radius`, no QSS
  border — every line is a device-exact fill.
- `setState()` early-returns on `operator==`: a live tick with nothing new
  costs nothing. Never tear down and rebuild children — there is exactly one
  (the edit overlay), hidden at rest.
- Every dropdown is built per open and freed on `aboutToHide`; the cell stays
  pressed until then. The source chooser is the one popup the bar does not
  own (`setSourceMenuOpen` mirrors it).
- The bar has no `NodeTree`: what its menus and the path edit need arrives
  as `AddressBarTreeQueries`, pulled when a menu opens or a key is typed —
  never per refresh. Likewise the module list is `Provider::modulesCached()`,
  never the enumerating syscall.
- History is recorded at the gesture, in the controller; the widget only
  asks (`onBack`, `onHistoryJump(delta)` — stepped, not indexed).
