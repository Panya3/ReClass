# The ribbon (Home | Modify)

A ReClassEx-style tabbed ribbon sits between the title/menu bar and the
document tabs. It is the fastest way to build out a class: one click retypes
the selection (`Hex 64`, `Int 32`, `UInt 32`, `Float`, `Pointer`…), `Add N`
grows the class, `Insert N` inserts above the selection, `Delete` removes it.

```
 Modify   Home
 ━━━━━━───────────────────────────────────────────────────────────────────────────────────────────────
  ↶    +4     +1024  │  ⤵4        ⤵1024  │  ✕ Delete    000  │  H64 Hex 64 ‖ I64 Int 64 ‖ U64 UInt 64 …
  ↷    +8     +2048  │  ⤵8        ⤵2048  │  ⧉ Duplicate FFF  │  H32 Hex 32 ‖ I32 Int 32 ‖ U32 UInt 32
       +64           │  ⤵64              │  ≡ Comment   ??? ⟲│  H16 Hex 16 ‖ I16 Int 16 ‖ U16 UInt 16
 Edit       Add      │       Insert      │      Selected     │                 Type
 ─────────────────────────────────────────────────────────────────────────────────────────────────────
```

Flat: no panel boxes or caption bands — one 1-device-px divider per panel gap,
a dim 9 pt caption under each panel, a hairline above and below the body. The
active tab is a 2-device-row accent underline on the strip's hairline.

## Pieces

| File | Role |
|---|---|
| `src/pixelglyphs.h` | 3×5 pixel font + fixed bitmaps, painted in whole **device** pixels (crisp at 100/125/150/200 %). |
| `src/ribbon_icons.h` | Glyph / composite / Codicon icon builders with theme-token family colours and a contrast guard; dpr-keyed cache. |
| `src/ribbon_spec.h` + `defaultRibbonSpec()` (in `ribbon.cpp`) | Data-driven description: tabs → panels → items (id, label, tooltip, size, icon, column breaks). |
| `src/ribbon.h/.cpp` | `rcx::RibbonBar` — one custom-painted widget: tab row, flat panels with dim captions, 3-row small-button columns, large buttons, label compaction, `…` overflow menu, minimise. `Qt::NoFocus`. |
| `src/paintutil.h` | Device-pixel-exact 1-row / 1-col edge fills plus the N-row `fillBottomDeviceRowsOfRect` / `fillTopDeviceRowsOfRect` used for the accent underlines (one inverse-mapped fill — two 1-row fills skip a row at 125 %). |
| `src/ribbon_actions.h/.cpp` | `rcx::RibbonActions` — one `QAction` per Modify button wired to `RcxController` selection ops; enabled-state follows the active controller's signals. |
| `src/main.cpp` `createRibbon()` | Hosts the ribbon in a chrome-less `QToolBar` (`RibbonHost`), binds Home-tab buttons to the existing menu `QAction`s, persists settings, theme hook. |
| `tools/ribbon_render.cpp` | Harness: `ribbon_render <prefix> [theme] [labels 0|1|2]` renders both tabs at 760/1080/1350/1920 px (1080 = the 125 % user window). |
| `tests/test_pixel_glyphs.cpp`, `tests/test_ribbon_layout.cpp`, `tests/test_ribbon_actions.cpp`, `tests/test_ribbon_mainwindow.cpp`, `tests/test_ribbon_ids.cpp` | Crispness/contrast, layout/overflow/focus, controller-op, QMainWindow-placement (flush under the title bar, full width, DockOverlay skips it), and id-parity (every `RibbonActions` id has a button, every Modify button has an action, `data()` agrees) regression tests. |

## Action ids

Every button is addressable by id (`RibbonBar::action(id)`, `RibbonActions::action(id)`):

- `edit.undo edit.redo`
- `add.4 add.8 add.64 add.1024 add.2048` — `RcxController::appendBytes(viewRoot, n)`
- `insert.4 … insert.2048` — `insertBytesAbove(lowest-offset selected node, n)`
- `sel.delete sel.duplicate sel.comment` — `deleteSelection` / `duplicateSelection` / `commentSelection`
- `sel.zero sel.ff sel.random sel.swap` — `fillSelectionBytes(Zero|FF|Random)` / `toggleBigEndianSelection`
- `type.hex64 … type.hex8`, `type.int64 … type.int8`, `type.uint64 … type.uint8`, `type.double type.float type.bool`, `type.vec2 type.vec3 type.vec4 type.mat4x4`, `type.pointer type.funcptr` (64/32-bit from `tree.pointerSize`), `type.utf8 type.utf16` — `retypeSelection(kind)`; `type.ptrclass` — `convertSelectionToTypedPointers`; `type.class` — Break into Class; `type.array` — `makeArrayFromSelection`; `type.custom` — the type chooser
- Menu-backed (the same `QAction` objects as the menus): `home.project.newclass/open/save/newstruct/newenum/close`, `home.process.refresh/goto`, `home.code.split`, `home.tools.scanner/symbols/bookmarks/console/rtti`
- Ribbon-only: `home.code.codeview` / `home.code.bothview` → `setViewMode(VM_Rendered / VM_Both)`; `home.process.attach` pops the Data Source menu under the button; `home.code.generate` pops the Export menu

`type.*` actions carry `data() = int(NodeKind)`, `add.*`/`insert.*` the byte count.

## Metrics and visual policy

All logical px at 10 pt JetBrains Mono (`fm.height()` 17); "device" = physical px.

| | |
|---|---|
| Tab row | `fm.height()+3` = **20**; `menuBarColor` strip; one `border` hairline on its last device row. Tabs from x = 6, `advance + 2·10` wide, 4 px apart; text inactive `textMuted` / hover `text` / active `text`; **active = 2 device rows of `indHoverSpan` on the tab's bottom edge** (replaces the hairline under it). No fill, no box, in any state. |
| Body | `background`; padTop 2 + 3 rows × `rowH = max(18, fm.height()+1)` + caption **12** + hairline 1 = **69** → **89** total (minimized: 21). |
| Panels | width = max(columns, caption + 2), columns centred when the caption wins (Edit). `kPanelGap 13`; **one 1-device-px `border` divider at `A.right()+7`**, 2 px clear of both hairlines; none before the first panel; one before the `…` item. `‖` family separators (Type): rows only, `kSepGap 7`; `kColGap 3`. Caption: 9 pt, `textMuted`, centred in the 12-px rect. |
| Small item | `colW × 18`; icon cell 16 tall, **width per icon kind** (`ribbonIconCellWidth`: TypeGlyph / FillSquares = `8·max(2, len)` → 16 / 24 / 32, else 16) at `(x+3, y+1)`; label from `x+3+cell+4` to `right−6`; width `3 + cell + 4 + text + 6`, icon-only `cell + 6`. |
| Large item | `clamp(text + 8, 48, 80)` wide, 54 tall; icon + 2 + one text line centred as a block; label `ElideRight`. |
| Pixel glyphs | **one scale per DPR**: `s = ceil(1.6·dpr)` → 2 / 2 / 3 / 4 at 100 / 125 / 150 / 200 % (ink 10 / 10 / 15 / 20 device px — `H64` and `F` are the same height). QMenu / QAction icons use the square 16×16 path (`wide = false`). |
| Codicons | rendered **only on integer multiples of the 16-unit grid**: small `16·round(dpr)` device px centred in the cell when it fits (125 % → 16 in the 20 cell), else the cell with AA; large `16·max(2, round(1.5·dpr))` → 32 device at 125 / 150 %, 48 at 200 % (24 with AA below 112.5 %). Every referenced SVG must be a 16-unit viewBox (`terminal`, `files`, `output`, `debug-console`, `more`, `debug-alt` are 24-unit traps — guarded by `test_pixel_glyphs`). `RibbonIconSpec::mirrorH` flips (redo = mirrored `discard`). |
| States | paint order: opacity → fill → icon + label → underline. **Rest: nothing** (Plain icons + labels `textDim`, family icons full colour). Hover: `hover` fill only, tone `text`. Pressed: `button` fill (`selected` when `button == background`). Checked: tone `indHoverSpan` + 2 device rows underline (only when the bound action is checkable and checked). Disabled: `setOpacity(0.40)` before anything (underline dims too), never hover / pressed. `RibbonItemSpec::destructive` (Delete): `markerPtr` in every state. |
| Tones | `textDim` / `textMuted` go through the 3 : 1 `wcagContrast` guard with `text` as the fallback (`mid.json` tones fail it). Purple appears only as the active-tab underline and the checked state; red only on Delete. |
| Home hierarchy | size first, brightness second, hue once: all Home Codicons `GF::Plain`; `home.project.newclass / newstruct / newenum` are `GF::Pointer` (the editor's class colour). Every Home panel is 1–3 Large + one Small column (Project L L L ‖ s s s; Process L ‖ s s; Code L L ‖ s s; Tools L L ‖ s s s). Home ≈ 1020 px fully labelled — fits a 1080-px window. |
| Compaction | stage 1 drops labels by `labelDrop` (higher first, equal values together; `glyphLabels` panels always first, even in *All*): Modify Type 30 · Add 20 · Insert 20 · Selected 0; Home Tools 30 · Code 20 · Process 10 · Project 0. Stage 2 hides by `hideOrder` (lower first, `neverHide` respected): Modify Selected 0 · Insert 10 · Add 20 (Type, Edit never); Home Tools 0 · Code 10 · Process 20 (Project never). Hidden panels sit behind a single middle-row 22×18 `…` item at `(dividerX+5, itemsTop+18)`. |

## Adding a button

1. Add a `RibbonItemSpec` to the right panel in `defaultRibbonSpec()` (`src/ribbon.cpp`): id, label, tooltip, size, icon (`TypeGlyph` label + family, `Codicon` path, or a composite kind), `columnBreakBefore`/`separatorBefore`.
2. If it is a Modify-tab op, create the `QAction` in `RibbonActions::buildActions()` and add its enabled predicate to `refreshEnabled()`; the controller op should be a public `RcxController` method (macro + `m_suppressRefresh` idiom, ids re-resolved inside loops).
3. If it reuses a menu action, capture the action in `createMenus()` and `bind()` it in `createRibbon()`.
4. Run `test_ribbon_layout` (no overlaps / overflow order / rest-hover-checked pixel probes), `test_pixel_glyphs` (crispness, uniform scale, 16-unit viewBox guard), `test_ribbon_actions`, and `ribbon_render` on the hidden desktop (`tools/run_tests_hidden.py`) — then look at the PNGs at 125 %.

## Settings (QSettings "REECLASS")

`showRibbon` (bool, View ▸ Ribbon), `ribbonLabels` (0 Auto — default: labels drop panel by panel when the window is narrow, whole panels hide only as a last resort; 1 All labels; 2 Icons only; View ▸ Ribbon Labels), `ribbonTab` (`modify` default), `ribbonMinimized` (double-click the tab row).

## Notes

- The ribbon never takes focus and never registers shortcuts; the editor owns the plain keys (Delete, Insert, 1–5, P/F/S/U). Shortcuts show in tooltips only.
- Family colours come from theme tokens (hex → `text`, signed → `markerPtr`, unsigned → `indHintGreen`…, contrast-guarded per theme); Codicons are tinted with a SourceIn fill so baked-colour icons follow the theme too. Plain Codicons get three tint pixmaps (`textDim` / `text` / `indHoverSpan`) — ink is part of the cache key.
- `DockOverlay::contentRect()` skips the ribbon host so drag-drop zones never cover it. Ctrl+Click on the ribbon reports region `ribbon` with the button id.
- `RCX_RIBBON_DEBUG=1 REECLASS.exe --screenshot out.png` prints the live ribbon geometry (window/host/ribbon rects, natural width, overflowed panels, font, dpr) to stderr — the only way to see what the ribbon was actually given inside the real window. Note `--screenshot` PNGs are device pixels: a 1350-px capture at 125 % is a 1080-logical-px window.
- Left out for now: Bits/bitfield, Show/Hide (no hidden nodes), VTable/PCHAR/PWCHAR kinds, 128-bit / half-float types (use Custom…).
