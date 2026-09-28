#pragma once
// Data model for the ReClassEx-style ribbon (tabs → panels → items). The
// tables themselves live in ribbon.cpp (`defaultRibbonSpec()`); the wiring
// side depends only on the item ids and the `data` payload documented there:
//   type.*   → data = int(NodeKind)
//   add.*    → data = int(byte count)
//   insert.* → data = int(byte count)

#include <QString>
#include <QVariant>
#include <QVector>

namespace rcx {

// Colour family of a glyph icon — mapped to theme tokens by ribbonFamilyColour()
// (ribbon_icons.h). Mirrors the original palette: hex black, signed red,
// unsigned green, float/text blue.
enum class GlyphFamily { Hex, Signed, Unsigned, Float, Text, Pointer, Bits, Plain };

enum class RibbonItemSize { Large, Small };

struct RibbonIconSpec {
    enum class Kind {
        TypeGlyph,    // arg = pixel-font label ("H64", "I32", "F" …)
        AddBytes,     // arg = byte count → green "+" and the number
        InsertBytes,  // arg = byte count → hook arrow and the number
        DeleteCross,  // markerPtr ✕
        FillSquares,  // arg = "000" | "FFF" | "???"
        ClassPtr,     // symbol-class Codicon + red "*" overlay
        Codicon       // arg = Codicon basename ("symbol-class") tinted by family
    };
    Kind        kind   = Kind::Codicon;
    QString     arg;
    GlyphFamily family = GlyphFamily::Plain;
};

struct RibbonItemSpec {
    QString        id;                 // stable action id ("type.int32")
    QString        label;              // button text
    QString        tooltip;
    RibbonItemSize size = RibbonItemSize::Small;
    RibbonIconSpec icon;
    bool           columnBreakBefore = false;  // `|`  — start a new column
    bool           separatorBefore   = false;  // `||` — new column + hairline
    bool           iconOnly          = false;  // label used only for the tooltip
    QVariant       data;
};

struct RibbonPanelSpec {
    QString id;
    QString caption;
    // Narrow-width behaviour. Stage 1 drops labels highest dropPriority first;
    // stage 2 hides whole panels lowest dropPriority first, skipping neverHide.
    // glyphLabels marks panels whose icons already read as labels (Type, Edit):
    // their labels are the first to go even in LabelMode::All.
    int  dropPriority = 0;
    bool neverHide    = false;
    bool glyphLabels  = false;
    QVector<RibbonItemSpec> items;
};

struct RibbonTabSpec {
    QString id;
    QString title;
    QVector<RibbonPanelSpec> panels;
};

// Home + Modify tables. Defined in ribbon.cpp.
const QVector<RibbonTabSpec>& defaultRibbonSpec();

}  // namespace rcx
