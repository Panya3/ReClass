# The ribbon (Home | Modify)

A ReClassEx-style tabbed ribbon sits between the title/menu bar and the
document tabs. It is the fastest way to build out a class: one click retypes
the selection (`Hex 64`, `Int 32`, `UInt 32`, `Float`, `Pointer`…), `Add N`
grows the class, `Insert N` inserts above the selection, `Delete` removes it.

```
 Modify │ Home
┌ Edit ─┐┌ Add ──────────┐┌ Insert ───────────┐┌ Selected ─────┐┌ Type ────────────────────────────────┐
│ Undo  ││ +4     +1024  ││ ⤵4        ⤵1024   ││ ✕ Delete  000 ││ H64 Hex 64 │ I64 Int 64 │ U64 UInt 64 …│
│ Redo  ││ +8     +2048  ││ ⤵8        ⤵2048   ││ ⧉ Duplicate FFF││ H32 Hex 32 │ I32 Int 32 │ U32 UInt 32  │
│       ││ +64           ││ ⤵64               ││ ≡ Comment ??? ⟲││ H16 Hex 16 │ I16 Int 16 │ U16 UInt 16  │
└───────┘└───────────────┘└───────────────────┘└───────────────┘└──────────────────────────────────────┘
```

## Pieces

| File | Role |
|---|---|
| `src/pixelglyphs.h` | 3×5 pixel font + fixed bitmaps, painted in whole **device** pixels (crisp at 100/125/150/200 %). |
| `src/ribbon_icons.h` | Glyph / composite / Codicon icon builders with theme-token family colours and a contrast guard; dpr-keyed cache. |
| `src/ribbon_spec.h` + `defaultRibbonSpec()` (in `ribbon.cpp`) | Data-driven description: tabs → panels → items (id, label, tooltip, size, icon, column breaks). |
| `src/ribbon.h/.cpp` | `rcx::RibbonBar` — one custom-painted widget: tab row, panels with caption strips, 3-row small-button columns, large buttons, label compaction, `»` overflow menu, minimise. `Qt::NoFocus`. |
| `src/ribbon_actions.h/.cpp` | `rcx::RibbonActions` — one `QAction` per Modify button wired to `RcxController` selection ops; enabled-state follows the active controller's signals. |
| `src/main.cpp` `createRibbon()` | Hosts the ribbon in a chrome-less `QToolBar` (`RibbonHost`), binds Home-tab buttons to the existing menu `QAction`s, persists settings, theme hook. |
| `tools/ribbon_render.cpp` | Harness: `ribbon_render <prefix> [theme] [labels 0|1|2]` renders both tabs at 760/1350/1920 px. |
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

## Adding a button

1. Add a `RibbonItemSpec` to the right panel in `defaultRibbonSpec()` (`src/ribbon.cpp`): id, label, tooltip, size, icon (`TypeGlyph` label + family, `Codicon` path, or a composite kind), `columnBreakBefore`/`separatorBefore`.
2. If it is a Modify-tab op, create the `QAction` in `RibbonActions::buildActions()` and add its enabled predicate to `refreshEnabled()`; the controller op should be a public `RcxController` method (macro + `m_suppressRefresh` idiom, ids re-resolved inside loops).
3. If it reuses a menu action, capture the action in `createMenus()` and `bind()` it in `createRibbon()`.
4. Run `test_ribbon_layout` (no overlaps / overflow order), `test_ribbon_actions`, and `ribbon_render` on the hidden desktop (`tools/run_tests_hidden.py`).

## Settings (QSettings "REECLASS")

`showRibbon` (bool, View ▸ Ribbon), `ribbonLabels` (0 Auto — default: labels drop panel by panel when the window is narrow, whole panels hide only as a last resort; 1 All labels; 2 Icons only; View ▸ Ribbon Labels), `ribbonTab` (`modify` default), `ribbonMinimized` (double-click the tab row).

## Notes

- The ribbon never takes focus and never registers shortcuts; the editor owns the plain keys (Delete, Insert, 1–5, P/F/S/U). Shortcuts show in tooltips only.
- Family colours come from theme tokens (hex → `text`, signed → `markerPtr`, unsigned → `indHintGreen`…, contrast-guarded per theme); Codicons are tinted with a SourceIn fill so baked-colour icons follow the theme too.
- `DockOverlay::contentRect()` skips the ribbon host so drag-drop zones never cover it. Ctrl+Click on the ribbon reports region `ribbon` with the button id.
- `RCX_RIBBON_DEBUG=1 REECLASS.exe --screenshot out.png` prints the live ribbon geometry (window/host/ribbon rects, natural width, overflowed panels, font, dpr) to stderr — the only way to see what the ribbon was actually given inside the real window. Note `--screenshot` PNGs are device pixels: a 1350-px capture at 125 % is a 1080-logical-px window.
- Left out for now: Bits/bitfield, Show/Hide (no hidden nodes), VTable/PCHAR/PWCHAR kinds, 128-bit / half-float types (use Custom…).
