#include "ribbon.h"

#include "core.h"
#include "fontutil.h"
#include "paintutil.h"
#include "rcxtooltip.h"
#include "ribbon_icons.h"
#include "themes/thememanager.h"

#include <QApplication>
#include <QFontDatabase>
#include <QFontMetrics>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QMouseEvent>
#include <QPainter>
#include <QResizeEvent>
#include <QSet>
#include <algorithm>

namespace rcx {

// ═══════════════════════════════════════════════════════════════════════════
// Spec tables
// ═══════════════════════════════════════════════════════════════════════════

namespace {

using IK = RibbonIconSpec::Kind;
using GF = GlyphFamily;

RibbonItemSpec glyphItem(const char* id, const char* label, const char* tip,
                         const char* glyph, GF family, NodeKind kind,
                         bool brk = false, bool sep = false) {
    RibbonItemSpec it;
    it.id = QString::fromLatin1(id);
    it.label = QString::fromUtf8(label);
    it.tooltip = QString::fromUtf8(tip);
    it.size = RibbonItemSize::Small;
    it.icon = {IK::TypeGlyph, QString::fromLatin1(glyph), family};
    it.columnBreakBefore = brk || sep;
    it.separatorBefore = sep;
    it.data = int(kind);
    return it;
}

RibbonItemSpec codiconItem(const char* id, const char* label, const char* tip,
                           const char* icon, GF family = GF::Plain,
                           RibbonItemSize size = RibbonItemSize::Small,
                           bool brk = false, bool sep = false,
                           QVariant data = QVariant()) {
    RibbonItemSpec it;
    it.id = QString::fromLatin1(id);
    it.label = QString::fromUtf8(label);
    it.tooltip = QString::fromUtf8(tip);
    it.size = size;
    it.icon = {IK::Codicon, QString::fromLatin1(icon), family};
    it.columnBreakBefore = brk || sep;
    it.separatorBefore = sep;
    it.data = data;
    return it;
}

RibbonItemSpec bytesItem(const char* prefix, IK kind, int n, bool brk) {
    RibbonItemSpec it;
    const bool add = (kind == IK::AddBytes);
    it.id = QStringLiteral("%1.%2").arg(QLatin1String(prefix)).arg(n);
    it.label = QStringLiteral("%1 %2").arg(add ? QStringLiteral("Add") : QStringLiteral("Insert")).arg(n);
    it.tooltip = add
        ? QStringLiteral("Append %1 bytes to the end of the class").arg(n)
        : QStringLiteral("Insert %1 bytes above the selection").arg(n);
    it.size = RibbonItemSize::Small;
    it.icon = {kind, QString::number(n), add ? GF::Unsigned : GF::Float};
    it.columnBreakBefore = brk;
    it.data = n;
    return it;
}

RibbonPanelSpec editPanel() {
    RibbonPanelSpec p;
    p.id = QStringLiteral("edit");
    p.caption = QStringLiteral("Edit");
    p.neverHide = true;
    // Icon-only for good: the two arrows never needed a label, and a labelled
    // 2-item column with an empty third row read as a boxed card. Redo is the
    // discard arrow mirrored (no 16-unit redo Codicon ships).
    RibbonItemSpec undo = codiconItem("edit.undo", "Undo", "Undo the last change", "discard");
    undo.iconOnly = true;
    RibbonItemSpec redo = codiconItem("edit.redo", "Redo", "Redo the last undone change", "discard");
    redo.iconOnly = true;
    redo.icon.mirrorH = true;
    p.items = {undo, redo};
    return p;
}

QVector<RibbonTabSpec> buildDefaultSpec() {
    QVector<RibbonTabSpec> tabs;

    // ── Modify ──
    {
        RibbonTabSpec tab;
        tab.id = QStringLiteral("modify");
        tab.title = QStringLiteral("Modify");
        tab.panels.append(editPanel());

        RibbonPanelSpec add;
        add.id = QStringLiteral("add");
        add.caption = QStringLiteral("Add");
        add.labelDrop = 20;   // Add + Insert lose their labels together
        add.hideOrder = 20;
        add.items = {
            bytesItem("add", IK::AddBytes, 4, false),
            bytesItem("add", IK::AddBytes, 8, false),
            bytesItem("add", IK::AddBytes, 64, false),
            bytesItem("add", IK::AddBytes, 1024, true),
            bytesItem("add", IK::AddBytes, 2048, false),
        };
        tab.panels.append(add);

        RibbonPanelSpec ins;
        ins.id = QStringLiteral("insert");
        ins.caption = QStringLiteral("Insert");
        ins.labelDrop = 20;
        ins.hideOrder = 10;
        ins.items = {
            bytesItem("insert", IK::InsertBytes, 4, false),
            bytesItem("insert", IK::InsertBytes, 8, false),
            bytesItem("insert", IK::InsertBytes, 64, false),
            bytesItem("insert", IK::InsertBytes, 1024, true),
            bytesItem("insert", IK::InsertBytes, 2048, false),
        };
        tab.panels.append(ins);

        RibbonPanelSpec sel;
        sel.id = QStringLiteral("selected");
        sel.caption = QStringLiteral("Selected");
        sel.labelDrop = 0;
        sel.hideOrder = 0;    // first panel to hide
        {
            RibbonItemSpec del;
            del.id = QStringLiteral("sel.delete");
            del.label = QStringLiteral("Delete");
            del.tooltip = QStringLiteral("Delete the selected fields");
            del.icon = {IK::DeleteCross, QString(), GF::Signed};
            del.destructive = true;
            sel.items.append(del);
            sel.items.append(codiconItem("sel.duplicate", "Duplicate",
                                         "Duplicate the selected fields", "clippy"));
            sel.items.append(codiconItem("sel.comment", "Comment",
                                         "Edit the comment of the selected field", "note"));

            auto fill = [](const char* id, const char* label, const char* tip,
                           const char* text, GF fam, bool brk) {
                RibbonItemSpec it;
                it.id = QString::fromLatin1(id);
                it.label = QString::fromUtf8(label);
                it.tooltip = QString::fromUtf8(tip);
                it.icon = {IK::FillSquares, QString::fromLatin1(text), fam};
                it.iconOnly = true;
                it.columnBreakBefore = brk;
                return it;
            };
            sel.items.append(fill("sel.zero", "Zero", "Fill the selected bytes with 00", "000", GF::Hex, true));
            sel.items.append(fill("sel.ff", "FF", "Fill the selected bytes with FF", "FFF", GF::Hex, false));
            sel.items.append(fill("sel.random", "Random", "Fill the selected bytes with random data", "???", GF::Bits, true));
            RibbonItemSpec swap = codiconItem("sel.swap", "Swap", "Toggle big-endian display of the selected scalars", "sync");
            swap.iconOnly = true;
            sel.items.append(swap);
        }
        tab.panels.append(sel);

        RibbonPanelSpec type;
        type.id = QStringLiteral("type");
        type.caption = QStringLiteral("Type");
        type.labelDrop = 30;  // its glyphs ARE the labels: first to go, even in All
        type.neverHide = true;
        type.glyphLabels = true;
        type.items = {
            glyphItem("type.hex64", "Hex 64", "Change the selected fields to Hex 64", "H64", GF::Hex, NodeKind::Hex64),
            glyphItem("type.hex32", "Hex 32", "Change the selected fields to Hex 32", "H32", GF::Hex, NodeKind::Hex32),
            glyphItem("type.hex16", "Hex 16", "Change the selected fields to Hex 16", "H16", GF::Hex, NodeKind::Hex16),
            glyphItem("type.hex8",  "Hex 8",  "Change the selected fields to Hex 8",  "H8",  GF::Hex, NodeKind::Hex8, true),

            glyphItem("type.int64", "Int 64", "Change the selected fields to int64_t", "I64", GF::Signed, NodeKind::Int64, true, true),
            glyphItem("type.int32", "Int 32", "Change the selected fields to int32_t", "I32", GF::Signed, NodeKind::Int32),
            glyphItem("type.int16", "Int 16", "Change the selected fields to int16_t", "I16", GF::Signed, NodeKind::Int16),
            glyphItem("type.int8",  "Int 8",  "Change the selected fields to int8_t",  "I8",  GF::Signed, NodeKind::Int8, true),

            glyphItem("type.uint64", "UInt 64", "Change the selected fields to uint64_t", "U64", GF::Unsigned, NodeKind::UInt64, true, true),
            glyphItem("type.uint32", "UInt 32", "Change the selected fields to uint32_t", "U32", GF::Unsigned, NodeKind::UInt32),
            glyphItem("type.uint16", "UInt 16", "Change the selected fields to uint16_t", "U16", GF::Unsigned, NodeKind::UInt16),
            glyphItem("type.uint8",  "UInt 8",  "Change the selected fields to uint8_t",  "U8",  GF::Unsigned, NodeKind::UInt8, true),

            glyphItem("type.double", "Double", "Change the selected fields to double", "D", GF::Float, NodeKind::Double, true, true),
            glyphItem("type.float",  "Float",  "Change the selected fields to float",  "F", GF::Float, NodeKind::Float),
            glyphItem("type.bool",   "Bool",   "Change the selected fields to bool",   "B", GF::Bits,  NodeKind::Bool),

            glyphItem("type.vec2",   "Vec 2",   "Change the selected fields to a 2-float vector", "V2", GF::Float, NodeKind::Vec2, true, true),
            glyphItem("type.vec3",   "Vec 3",   "Change the selected fields to a 3-float vector", "V3", GF::Float, NodeKind::Vec3),
            glyphItem("type.vec4",   "Vec 4",   "Change the selected fields to a 4-float vector", "V4", GF::Float, NodeKind::Vec4),
            glyphItem("type.mat4x4", "Mat 4x4", "Change the selected fields to a 4×4 float matrix", "M4", GF::Float, NodeKind::Mat4x4, true),

            glyphItem("type.pointer", "Pointer",  "Change the selected fields to a pointer", "PTR", GF::Pointer, NodeKind::Pointer64, true, true),
            glyphItem("type.funcptr", "Func Ptr", "Change the selected fields to a function pointer", "FN*", GF::Pointer, NodeKind::FuncPtr64),
        };
        {
            RibbonItemSpec pc;
            pc.id = QStringLiteral("type.ptrclass");
            pc.label = QStringLiteral("Ptr → Class");
            pc.tooltip = QStringLiteral("Turn the selected pointer into a pointer to a new class");
            pc.icon = {IK::ClassPtr, QString(), GF::Pointer};
            pc.data = int(NodeKind::Pointer64);
            type.items.append(pc);

            RibbonItemSpec cls = codiconItem("type.class", "Class", "Break the selection into a new class",
                                             "symbol-class", GF::Pointer, RibbonItemSize::Small, true);
            cls.data = int(NodeKind::Struct);
            type.items.append(cls);
            RibbonItemSpec arr = codiconItem("type.array", "Array", "Turn the selection into an array",
                                             "symbol-array");
            arr.data = int(NodeKind::Array);
            type.items.append(arr);
            type.items.append(codiconItem("type.custom", "Custom…", "Pick any type from the type chooser",
                                          "symbol-misc"));
        }
        type.items.append(glyphItem("type.utf8",  "ASCII",  "Change the selected fields to an ASCII string", "STR",  GF::Text, NodeKind::UTF8, true, true));
        type.items.append(glyphItem("type.utf16", "UTF-16", "Change the selected fields to a UTF-16 string", "WSTR", GF::Text, NodeKind::UTF16));
        tab.panels.append(type);

        tabs.append(tab);
    }

    // ── Home ──
    {
        RibbonTabSpec tab;
        tab.id = QStringLiteral("home");
        tab.title = QStringLiteral("Home");
        tab.panels.append(editPanel());

        RibbonPanelSpec project;
        project.id = QStringLiteral("project");
        project.caption = QStringLiteral("Project");
        project.labelDrop = 0;    // last labels to go
        project.neverHide = true;
        // Size is the hierarchy, brightness the second tier (Plain = textDim
        // at rest, text on hover); the ONE hue is the editor's class colour
        // (GF::Pointer -> syntaxType) on the three "new type" buttons.
        project.items = {
            codiconItem("home.project.newclass", "New Class", "Create a new class", "symbol-class", GF::Pointer, RibbonItemSize::Large),
            codiconItem("home.project.open", "Open", "Open a project", "folder-opened", GF::Plain, RibbonItemSize::Large),
            codiconItem("home.project.save", "Save", "Save the project", "save", GF::Plain, RibbonItemSize::Large),
            codiconItem("home.project.newstruct", "New Struct", "Create a new struct", "symbol-structure", GF::Pointer, RibbonItemSize::Small, true),
            codiconItem("home.project.newenum", "New Enum", "Create a new enum", "symbol-enum", GF::Pointer),
            codiconItem("home.project.close", "Close", "Close the project", "close"),
        };
        tab.panels.append(project);

        RibbonPanelSpec process;
        process.id = QStringLiteral("process");
        process.caption = QStringLiteral("Process");
        process.labelDrop = 10;
        process.hideOrder = 20;
        process.items = {
            codiconItem("home.process.attach", "Attach", "Attach to a process or open a data source", "plug", GF::Plain, RibbonItemSize::Large),
            codiconItem("home.process.refresh", "Refresh", "Refresh the memory view", "refresh", GF::Plain, RibbonItemSize::Small, true),
            codiconItem("home.process.goto", "Go to Address", "Go to an address", "symbol-numeric"),
        };
        tab.panels.append(process);

        RibbonPanelSpec code;
        code.id = QStringLiteral("code");
        code.caption = QStringLiteral("Code");
        code.labelDrop = 20;
        code.hideOrder = 10;
        code.items = {
            codiconItem("home.code.codeview", "Code", "Show the generated code", "code", GF::Plain, RibbonItemSize::Large),
            codiconItem("home.code.bothview", "Both", "Show the structure and the generated code side by side", "split-horizontal", GF::Plain, RibbonItemSize::Large),
            codiconItem("home.code.generate", "Generate", "Export the generated code", "file-code", GF::Plain, RibbonItemSize::Small, true),
            codiconItem("home.code.split", "Split View", "Split the editor", "split-vertical"),
        };
        tab.panels.append(code);

        RibbonPanelSpec tools;
        tools.id = QStringLiteral("tools");
        tools.caption = QStringLiteral("Tools");
        tools.labelDrop = 30;   // first labels to go ...
        tools.hideOrder = 0;    // ... and the first panel to hide
        // Two Large + a small column (like every other Home panel) instead of
        // five consecutive towers. console.svg / clippy.svg replace the
        // 24-unit terminal / files (stroke weight mismatch on the 16 grid).
        tools.items = {
            codiconItem("home.tools.scanner", "Scanner", "Open the memory scanner", "search", GF::Plain, RibbonItemSize::Large),
            codiconItem("home.tools.symbols", "Symbols", "Open the symbol browser", "symbol-key", GF::Plain, RibbonItemSize::Large),
            codiconItem("home.tools.bookmarks", "Bookmarks", "Open the bookmarks", "bookmark", GF::Plain, RibbonItemSize::Small, true),
            codiconItem("home.tools.console", "Console", "Show the console", "console"),
            codiconItem("home.tools.rtti", "RTTI", "Open the RTTI browser", "symbol-interface"),
        };
        tab.panels.append(tools);

        tabs.append(tab);
    }

    return tabs;
}

// Sensible colours when ThemeManager has nothing loaded (tests, harnesses run
// from a directory without themes/). Mirrors src/themes/defaults/vs.json.
Theme fallbackTheme() {
    static const char* kJson = R"({
        "name": "VS2022 Dark", "background": "#181818", "backgroundAlt": "#2d2d30",
        "surface": "#333337", "border": "#3f3f46", "borderFocused": "#b180d7",
        "button": "#3f3f46", "text": "#dcdcdc", "textDim": "#858585",
        "textMuted": "#636369", "textFaint": "#4d4d55", "hover": "#242427",
        "selected": "#1e1e21", "selection": "#264f78", "syntaxKeyword": "#569cd6",
        "syntaxNumber": "#b5cea8", "syntaxString": "#d69d85", "syntaxComment": "#57a64a",
        "syntaxPreproc": "#9b9b9b", "syntaxType": "#4ec9b0", "indHoverSpan": "#b180d7",
        "indCmdPill": "#2d2d30", "indDataChanged": "#8fbc7a", "indHintGreen": "#5a8248",
        "indRttiHint": "#D7BA7D", "markerPtr": "#f44747", "markerCycle": "#e5a00d",
        "markerError": "#7a2e2e" })";
    return Theme::fromJson(QJsonDocument::fromJson(kJson).object());
}

constexpr int kLeftMargin   = 6;   // frameless resize strips cover the outer 5 px
constexpr int kRightMargin  = 6;
constexpr int kPanelGap     = 13;  // between panels ...
constexpr int kDividerInset = 7;   // ... with the 1-device-px divider at A.right() + 7
constexpr int kColGap       = 3;   // `|`
constexpr int kSepGap       = 7;   // `||` (hairline in the middle)
constexpr int kTabPad       = 10;
constexpr int kTabGap       = 4;
constexpr int kOverflowW    = 22;  // the ... item: middle row, right of a divider
constexpr int kOverflowH    = 18;
constexpr int kOverflowInset = 5;  // overflow x = dividerX + 5
constexpr int kLargeMinW    = 48;
constexpr int kLargeMaxW    = 80;
constexpr int kLargeIconOnlyW = 40;
constexpr int kUnderlineRows = 2;  // DEVICE rows: active tab / checked item accent
constexpr double kDisabledOpacity = 0.40;

// Draws a dpr-stamped pixmap so its top-left lands on a whole device pixel
// (no SmoothPixmapTransform: the pixmap is already at device resolution).
void drawPixmapSnapped(QPainter& p, const QPointF& logicalPos, const QPixmap& pm) {
    const QTransform dt = p.deviceTransform();
    QPointF dev = dt.map(logicalPos);
    dev = QPointF(qRound(dev.x()), qRound(dev.y()));
    p.drawPixmap(dt.inverted().map(dev), pm);
}

// Pressed surface: `button`, or `selected` on themes whose button colour is
// the body colour (tw.json) so a press is still visible.
QColor pressedFill(const Theme& t) {
    return (t.button.isValid() && t.button != t.background) ? t.button
         : (t.selected.isValid() ? t.selected : t.hover);
}

}  // namespace

const QVector<RibbonTabSpec>& defaultRibbonSpec() {
    static const QVector<RibbonTabSpec> spec = buildDefaultSpec();
    return spec;
}

// ═══════════════════════════════════════════════════════════════════════════
// RibbonBar
// ═══════════════════════════════════════════════════════════════════════════

RibbonBar::RibbonBar(QWidget* parent)
    : RibbonBar(defaultRibbonSpec(), parent) {}

RibbonBar::RibbonBar(const QVector<RibbonTabSpec>& spec, QWidget* parent)
    : QWidget(parent), m_spec(spec) {
    init();
}

void RibbonBar::init() {
    setFocusPolicy(Qt::NoFocus);
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setAttribute(Qt::WA_OpaquePaintEvent, true);

    const QString family = QStringLiteral("JetBrains Mono");
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
    const bool hasMono = QFontDatabase::hasFamily(family);
#else
    const bool hasMono = QFontDatabase().hasFamily(family);
#endif
    if (hasMono) setFont(QFont(family, 10));

    m_theme = ThemeManager::instance().current();
    if (!m_theme.background.isValid() || !m_theme.text.isValid())
        m_theme = fallbackTheme();

    for (int t = 0; t < m_spec.size(); ++t)
        for (int pi = 0; pi < m_spec[t].panels.size(); ++pi)
            for (int ii = 0; ii < m_spec[t].panels[pi].items.size(); ++ii) {
                const QString& id = m_spec[t].panels[pi].items[ii].id;
                if (m_index.contains(id)) continue;   // "edit.*" lives on both tabs
                m_index.insert(id, ItemRef{t, pi, ii});
                m_ids.append(id);
            }
    if (!m_spec.isEmpty()) m_currentTab = m_spec.first().id;

    buildActions();
    refreshOwnActionIcons();
}

void RibbonBar::buildActions() {
    for (const QString& id : m_ids) {
        const RibbonItemSpec* it = itemSpec(id);
        auto* a = new QAction(it->label, this);
        a->setToolTip(it->tooltip.isEmpty() ? it->label : it->tooltip);
        a->setData(it->data);
        connect(a, &QAction::changed, this, [this] { markLayoutDirty(); update(); });
        m_own.insert(id, a);
    }
}

void RibbonBar::refreshOwnActionIcons() {
    const qreal dpr = devicePixelRatioF();
    for (auto it = m_own.begin(); it != m_own.end(); ++it) {
        const RibbonItemSpec* spec = itemSpec(it.key());
        if (!spec) continue;
        // Square 16x16 cells for QAction / QMenu use: the ribbon's 24/32-wide
        // glyph cells would otherwise be downscaled into blur.
        RibbonIconOptions o;
        o.wide = false;
        o.plainInk = m_theme.text;
        const QIcon icon(ribbonIcon(spec->icon, RibbonIconSize::Small, dpr, m_theme, o));
        it.value()->setIcon(icon);
        // Externals that were bound without an icon carry ours (so the >>
        // overflow submenus show glyphs in the real app); keep them in sync
        // across theme / DPI changes.
        if (m_suppliedIcons.contains(it.key()))
            if (auto ext = m_external.value(it.key()); !ext.isNull()) ext->setIcon(icon);
    }
}

// ── Actions ──

QAction* RibbonBar::effectiveAction(const QString& id) const {
    auto ext = m_external.constFind(id);
    if (ext != m_external.constEnd() && !ext.value().isNull()) return ext.value().data();
    return m_own.value(id, nullptr);
}

QAction* RibbonBar::action(const QString& id) const { return effectiveAction(id); }

void RibbonBar::setAction(const QString& id, QAction* external) {
    if (!m_index.contains(id)) return;
    if (auto old = m_external.value(id); !old.isNull())
        disconnect(old.data(), nullptr, this, nullptr);
    m_suppliedIcons.remove(id);
    if (external) {
        m_external.insert(id, external);
        if (external->icon().isNull()) {
            if (QAction* own = m_own.value(id)) external->setIcon(own->icon());
            m_suppliedIcons.insert(id);
        }
        connect(external, &QAction::changed, this, [this] { markLayoutDirty(); refreshToolTip(); update(); });
        connect(external, &QObject::destroyed, this, [this] { markLayoutDirty(); update(); });
    } else {
        m_external.remove(id);
    }
    markLayoutDirty();
    refreshToolTip();
    update();
}

QStringList RibbonBar::actionIds() const { return m_ids; }

const RibbonItemSpec* RibbonBar::itemSpec(const ItemRef& ref) const {
    if (ref.tab < 0 || ref.tab >= m_spec.size()) return nullptr;
    const auto& panels = m_spec[ref.tab].panels;
    if (ref.panel < 0 || ref.panel >= panels.size()) return nullptr;
    const auto& items = panels[ref.panel].items;
    if (ref.item < 0 || ref.item >= items.size()) return nullptr;
    return &items[ref.item];
}

const RibbonItemSpec* RibbonBar::itemSpec(const QString& id) const {
    auto it = m_index.constFind(id);
    return it == m_index.constEnd() ? nullptr : itemSpec(it.value());
}

bool RibbonBar::isItemVisible(const QString& id) const {
    QAction* a = effectiveAction(id);
    return a ? a->isVisible() : true;
}

int RibbonBar::tabIndex(const QString& tabId) const {
    for (int i = 0; i < m_spec.size(); ++i)
        if (m_spec[i].id == tabId) return i;
    return -1;
}

QStringList RibbonBar::tabIds() const {
    QStringList out;
    for (const auto& t : m_spec) out << t.id;
    return out;
}

// ── Presentation ──

void RibbonBar::applyTheme(const Theme& theme) {
    m_theme = theme;
    if (!m_theme.background.isValid()) m_theme = fallbackTheme();
    refreshOwnActionIcons();
    markLayoutDirty();
    update();
}

void RibbonBar::setLabelMode(LabelMode mode) {
    if (mode == m_labelMode) return;
    m_labelMode = mode;
    markLayoutDirty();
    updateGeometry();
    update();
    emit labelModeChanged(int(mode));
}

void RibbonBar::setCurrentTab(const QString& tabId) {
    if (tabId == m_currentTab || tabIndex(tabId) < 0) return;
    m_currentTab = tabId;
    m_hoverId.clear();
    m_pressedId.clear();
    markLayoutDirty();
    updateGeometry();
    update();
    emit currentTabChanged(tabId);
}

void RibbonBar::setMinimized(bool minimized) {
    if (minimized == m_minimized) return;
    m_minimized = minimized;
    m_hoverId.clear();
    m_pressedId.clear();
    updateGeometry();
    update();
    emit minimizedChanged(minimized);
}

// ── Metrics ──

RibbonBar::Metrics RibbonBar::metrics() const {
    Metrics m;
    const QFontMetrics fm(font());
    m.tabRowH  = fm.height() + 3;              // 20 at 10 pt
    m.rowH     = qMax(18, fm.height() + 1);    // 18
    m.captionH = 12;                           // 9 pt caption; glyphs overhang the rect
    m.padTop   = 2;
    // padTop + 3 rows + caption + body bottom hairline -> 69; 89 with the tab row
    m.bodyH = m.padTop + 3 * m.rowH + m.captionH + 1;
    const qreal dpr = devicePixelRatioF() > 0 ? devicePixelRatioF() : 1.0;
    m.smallIcon    = 16;
    m.largeIconDev = ribbonLargeIconDev(dpr);
    m.largeIcon    = m.largeIconDev / dpr;
    return m;
}

// 9 pt under a 10 pt body: the only second type size on the strip.
QFont RibbonBar::captionFont() const {
    QFont f = font();
    f.setPointSize(qMax(6, resolvedPointSize(font()) - 1));
    return f;
}

int RibbonBar::tabRowHeight() const { return metrics().tabRowH; }
int RibbonBar::bodyHeight() const { return metrics().bodyH; }
int RibbonBar::preferredHeight() const {
    const Metrics m = metrics();
    return m_minimized ? m.tabRowH + 1 : m.tabRowH + m.bodyH;
}

QSize RibbonBar::sizeHint() const { return QSize(naturalWidth() + kRightMargin, preferredHeight()); }
QSize RibbonBar::minimumSizeHint() const { return QSize(80, preferredHeight()); }

// 3 + icon cell + 4 + label + 6; icon-only = cell + 6. The cell is per icon
// kind (ribbonIconCellWidth: 16 / 24 / 32) so the pixel labels share one scale.
int RibbonBar::smallItemWidth(const RibbonItemSpec& it, bool label, const QFontMetrics& fm) const {
    const int cellW = ribbonIconCellWidth(it.icon);
    if (!label || it.iconOnly) return cellW + 6;
    return 3 + cellW + 4 + fm.horizontalAdvance(it.label) + 6;
}

// clamp(label + 8, 48, 80); the label is elided (never for shipped labels).
int RibbonBar::largeItemWidth(const RibbonItemSpec& it, bool label, const QFontMetrics& fm) const {
    if (!label) return kLargeIconOnlyW;
    return qBound(kLargeMinW, fm.horizontalAdvance(it.label) + 8, kLargeMaxW);
}

// ── Layout engine ──

RibbonBar::Layout RibbonBar::computeLayout(int tabIdx, int availW,
                                           const QSet<QString>& dropLabels,
                                           const QStringList& hidePanels,
                                           bool reserveOverflow) const {
    Layout L;
    L.forWidth = availW;
    L.tab = m_currentTab;
    if (tabIdx < 0 || tabIdx >= m_spec.size()) return L;
    const Metrics m = metrics();
    const QFontMetrics fm(font());
    const QFontMetrics cfm(captionFont());
    const int itemsTop = m.tabRowH + m.padTop;
    const int itemsH   = 3 * m.rowH;
    const int captionTop = itemsTop + itemsH;

    int x = kLeftMargin;
    int column = 0;
    const RibbonTabSpec& tab = m_spec[tabIdx];
    for (int pi = 0; pi < tab.panels.size(); ++pi) {
        const RibbonPanelSpec& panel = tab.panels[pi];
        if (hidePanels.contains(panel.id)) continue;
        const bool labels = (m_labelMode != LabelMode::IconsOnly) && !dropLabels.contains(panel.id);

        struct Col {
            QVector<int> items;   // indexes into panel.items
            bool large = false;
            bool sep = false;
            bool brk = false;
            int  w = 0;
        };
        QVector<Col> cols;
        for (int ii = 0; ii < panel.items.size(); ++ii) {
            const RibbonItemSpec& it = panel.items[ii];
            if (!isItemVisible(it.id)) continue;
            const bool large = (it.size == RibbonItemSize::Large);
            bool newCol = cols.isEmpty() || large || cols.last().large
                       || it.columnBreakBefore || it.separatorBefore
                       || cols.last().items.size() >= 3;
            if (newCol) {
                Col c;
                c.large = large;
                c.sep = it.separatorBefore && !cols.isEmpty();
                c.brk = (it.columnBreakBefore || it.separatorBefore) && !cols.isEmpty();
                cols.append(c);
            }
            Col& c = cols.last();
            c.items.append(ii);
            const int w = large ? largeItemWidth(it, labels, fm) : smallItemWidth(it, labels, fm);
            c.w = qMax(c.w, w);
        }
        if (cols.isEmpty()) continue;

        // Panel width is the wider of its columns and its caption (+2); the
        // columns are centred when the caption wins (Edit).
        int itemsW = 0;
        for (int ci = 0; ci < cols.size(); ++ci) {
            if (ci > 0) itemsW += cols[ci].sep ? kSepGap : kColGap;
            itemsW += cols[ci].w;
        }
        const int captionW = cfm.horizontalAdvance(panel.caption) + 2;
        const int panelW = qMax(itemsW, captionW);

        LaidPanel lp;
        lp.id = panel.id;
        lp.labelsDropped = !labels;
        const int panelX = x;
        int cx = panelX + (panelW - itemsW) / 2;
        for (int ci = 0; ci < cols.size(); ++ci) {
            const Col& c = cols[ci];
            if (ci > 0) {
                if (c.sep) { lp.separatorXs << cx + kSepGap / 2; cx += kSepGap; }
                else cx += kColGap;
            }
            for (int r = 0; r < c.items.size(); ++r) {
                const RibbonItemSpec& it = panel.items[c.items[r]];
                LaidItem li;
                li.id = it.id;
                li.ref = ItemRef{tabIdx, pi, c.items[r]};
                li.large = c.large;
                li.label = labels && !it.iconOnly;
                li.column = column;
                li.rect = c.large ? QRect(cx, itemsTop, c.w, itemsH)
                                  : QRect(cx, itemsTop + r * m.rowH, c.w, m.rowH);
                L.items.append(li);
            }
            cx += c.w;
            ++column;
        }
        lp.rect = QRect(panelX, itemsTop, panelW, itemsH + m.captionH);
        lp.captionRect = QRect(panelX, captionTop, panelW, m.captionH);
        // One divider per gap (none before the first panel).
        if (!L.panels.isEmpty()) L.dividerXs << L.panels.last().rect.right() + kDividerInset;
        L.panels.append(lp);
        x = panelX + panelW + kPanelGap;
    }
    // Hidden panels are reported in the order they were hidden (stage 2 order).
    for (const QString& pid : hidePanels)
        for (const auto& panel : tab.panels)
            if (panel.id == pid) { L.hiddenPanels << pid; break; }
    // x now sits one gap past the last panel
    int right = L.panels.isEmpty() ? kLeftMargin : x - kPanelGap;
    L.naturalWidth = right;
    if (reserveOverflow || !L.hiddenPanels.isEmpty()) {
        // ... = a normal divider, then a single middle-row 22x18 item.
        const int dividerX = right - 1 + kDividerInset;
        L.dividerXs << dividerX;
        L.overflowRect = QRect(dividerX + kOverflowInset, itemsTop + m.rowH, kOverflowW, kOverflowH);
        right = L.overflowRect.right() + 1;
    }
    L.width = right + kRightMargin;
    return L;
}

void RibbonBar::ensureLayout() const {
    // resize() on a hidden widget delivers no resizeEvent, so the dirty flag
    // alone can't be trusted — the cache also remembers the width it was
    // computed for.
    if (m_layoutDirty || m_layout.forWidth != width() || m_layout.tab != m_currentTab) relayout();
}

void RibbonBar::relayout() const {
    m_layoutDirty = false;
    const int tabIdx = tabIndex(m_currentTab);
    const int availW = width();
    QSet<QString> drop;
    QStringList hide;

    Layout L = computeLayout(tabIdx, availW, drop, hide, false);
    if (L.width <= availW || tabIdx < 0) { m_layout = L; return; }

    const auto& panels = m_spec[tabIdx].panels;
    // Stage 1: drop labels -- panels whose icons already read as labels
    // (glyphLabels: Type) first, then by labelDrop, highest first; panels
    // with the same labelDrop go together (Add + Insert). In All mode only
    // the glyphLabels panels are candidates; in Auto every panel is.
    if (m_labelMode != LabelMode::IconsOnly) {
        QVector<const RibbonPanelSpec*> cands;
        for (const auto& p : panels)
            if (m_labelMode == LabelMode::Auto || p.glyphLabels) cands.append(&p);
        std::stable_sort(cands.begin(), cands.end(),
                         [](const RibbonPanelSpec* a, const RibbonPanelSpec* b) {
                             if (a->glyphLabels != b->glyphLabels) return a->glyphLabels;
                             return a->labelDrop > b->labelDrop;
                         });
        for (int i = 0; i < cands.size();) {
            int j = i;
            do { drop.insert(cands[j]->id); ++j; }
            while (j < cands.size() && cands[j]->glyphLabels == cands[i]->glyphLabels
                   && cands[j]->labelDrop == cands[i]->labelDrop);
            L = computeLayout(tabIdx, availW, drop, hide, false);
            if (L.width <= availW) { m_layout = L; return; }
            i = j;
        }
    }
    // Stage 2: hide whole panels, lowest hideOrder first, never `neverHide`.
    QVector<const RibbonPanelSpec*> hideCands;
    for (const auto& p : panels)
        if (!p.neverHide) hideCands.append(&p);
    std::stable_sort(hideCands.begin(), hideCands.end(),
                     [](const RibbonPanelSpec* a, const RibbonPanelSpec* b) {
                         return a->hideOrder < b->hideOrder;
                     });
    for (const auto* p : hideCands) {
        hide << p->id;
        L = computeLayout(tabIdx, availW, drop, hide, true);
        if (L.width <= availW) { m_layout = L; return; }
    }
    m_layout = L;   // still too wide — best effort
}

void RibbonBar::markLayoutDirty() { m_layoutDirty = true; }

int RibbonBar::naturalWidth() const {
    return computeLayout(tabIndex(m_currentTab), width(), {}, {}, false).naturalWidth;
}

// ── Geometry queries ──

QString RibbonBar::itemIdAt(QPoint pos) const {
    if (m_minimized) return QString();
    ensureLayout();
    for (const LaidItem& li : m_layout.items)
        if (li.rect.contains(pos)) return li.id;
    return QString();
}

QRect RibbonBar::itemRect(const QString& id) const {
    if (m_minimized) return QRect();
    ensureLayout();
    for (const LaidItem& li : m_layout.items)
        if (li.id == id) return li.rect;
    return QRect();
}

int RibbonBar::itemColumn(const QString& id) const {
    if (m_minimized) return -1;
    ensureLayout();
    for (const LaidItem& li : m_layout.items)
        if (li.id == id) return li.column;
    return -1;
}

bool RibbonBar::itemLabelShown(const QString& id) const {
    if (m_minimized) return false;
    ensureLayout();
    for (const LaidItem& li : m_layout.items)
        if (li.id == id) return li.label;
    return false;
}

QRect RibbonBar::tabRect(const QString& tabId) const {
    const Metrics m = metrics();
    const QFontMetrics fm(font());
    int x = kLeftMargin;
    for (const auto& t : m_spec) {
        const int w = fm.horizontalAdvance(t.title) + 2 * kTabPad;
        if (t.id == tabId) return QRect(x, 0, w, m.tabRowH);
        x += w + kTabGap;
    }
    return QRect();
}

QString RibbonBar::tabIdAt(QPoint pos) const {
    for (const auto& t : m_spec)
        if (tabRect(t.id).contains(pos)) return t.id;
    return QString();
}

QRect RibbonBar::panelRect(const QString& panelId) const {
    if (m_minimized) return QRect();
    ensureLayout();
    for (const LaidPanel& lp : m_layout.panels)
        if (lp.id == panelId) return lp.rect;
    return QRect();
}

QStringList RibbonBar::overflowedPanelIds() const {
    ensureLayout();
    return m_layout.hiddenPanels;
}

QRect RibbonBar::overflowButtonRect() const {
    if (m_minimized) return QRect();
    ensureLayout();
    return m_layout.hiddenPanels.isEmpty() ? QRect() : m_layout.overflowRect;
}

QMenu* RibbonBar::overflowMenu() {
    ensureLayout();
    if (m_overflowMenu) m_overflowMenu->deleteLater();
    m_overflowMenu = new QMenu(this);
    const int tabIdx = tabIndex(m_currentTab);
    if (tabIdx < 0) return m_overflowMenu;
    for (const QString& pid : m_layout.hiddenPanels) {
        for (const RibbonPanelSpec& panel : m_spec[tabIdx].panels) {
            if (panel.id != pid) continue;
            QMenu* sub = m_overflowMenu->addMenu(panel.caption);
            for (const RibbonItemSpec& it : panel.items) {
                QAction* a = effectiveAction(it.id);
                if (!a || !a->isVisible()) continue;
                sub->addAction(a);
            }
        }
    }
    return m_overflowMenu;
}

void RibbonBar::showOverflowMenu() {
    QMenu* menu = overflowMenu();
    const QRect r = overflowButtonRect();
    // The ... item stays pressed while its menu is up.
    m_overflowOpen = true;
    connect(menu, &QMenu::aboutToHide, this, [this] { m_overflowOpen = false; update(); });
    update();
    menu->popup(mapToGlobal(QPoint(r.left(), r.bottom() + 1)));
}

QString RibbonBar::tooltipFor(const QString& id) const {
    QAction* a = effectiveAction(id);
    const RibbonItemSpec* spec = itemSpec(id);
    QString tip = a ? a->toolTip() : QString();
    if (tip.isEmpty() && spec) tip = spec->tooltip.isEmpty() ? spec->label : spec->tooltip;
    if (a && !a->shortcut().isEmpty())
        tip += QStringLiteral("  (%1)").arg(a->shortcut().toString(QKeySequence::NativeText));
    return tip;
}

// ── Events ──

void RibbonBar::resizeEvent(QResizeEvent* e) {
    QWidget::resizeEvent(e);
    markLayoutDirty();
}

void RibbonBar::changeEvent(QEvent* e) {
    QWidget::changeEvent(e);
    if (e->type() == QEvent::FontChange || e->type() == QEvent::StyleChange) {
        markLayoutDirty();
        updateGeometry();
        update();
    }
}

void RibbonBar::updateHover(QPoint pos) {
    QString id;
    if (pos.y() < metrics().tabRowH) {
        const QString t = tabIdAt(pos);
        if (!t.isEmpty()) id = QStringLiteral("tab:") + t;
    } else if (!m_minimized) {
        id = itemIdAt(pos);
        if (id.isEmpty() && overflowButtonRect().contains(pos)) id = QStringLiteral("overflow");
    }
    if (id != m_hoverId) {
        m_hoverId = id;
        refreshToolTip();
        dismissRcxTooltip();
        update();
    }
}

// The app's GlobalTooltipBridge (a qApp event filter) owns QEvent::ToolTip:
// it reads widget->toolTip() and shows / keeps the shared RcxTooltip
// idempotently for the same widget + text. Handling ToolTip here as well made
// the bridge dismiss (empty toolTip) and this widget re-show on every hover
// tick -- visible flicker. So the ribbon just keeps its toolTip property equal
// to the hovered item's text and lets the bridge (or QToolTip) show it.
void RibbonBar::refreshToolTip() {
    QString tip;
    if (m_hoverId == QLatin1String("overflow")) tip = QStringLiteral("More panels");
    else if (!m_hoverId.isEmpty() && !m_hoverId.startsWith(QLatin1String("tab:"))) tip = tooltipFor(m_hoverId);
    if (tip != toolTip()) setToolTip(tip);
}

void RibbonBar::mouseMoveEvent(QMouseEvent* e) {
    updateHover(e->pos());
    QWidget::mouseMoveEvent(e);
}

void RibbonBar::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) { QWidget::mousePressEvent(e); return; }
    dismissRcxTooltip();
    const QPoint pos = e->pos();
    if (pos.y() < metrics().tabRowH) {
        const QString t = tabIdAt(pos);
        if (!t.isEmpty()) {
            setCurrentTab(t);
            if (m_minimized) {
                setMinimized(false);
                m_restoreTimer.start();
            }
        }
        e->accept();
        return;
    }
    if (m_minimized) { e->accept(); return; }
    if (overflowButtonRect().contains(pos)) {
        m_pressedId.clear();
        showOverflowMenu();
        e->accept();
        return;
    }
    const QString id = itemIdAt(pos);
    if (!id.isEmpty()) {
        m_pressedId = id;
        update();
    }
    e->accept();
}

void RibbonBar::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) { QWidget::mouseReleaseEvent(e); return; }
    if (!m_pressedId.isEmpty()) {
        const QString id = m_pressedId;
        m_pressedId.clear();
        update();
        if (itemIdAt(e->pos()) == id) {
            if (QAction* a = effectiveAction(id); a && a->isEnabled()) a->trigger();
        }
    }
    e->accept();
}

void RibbonBar::mouseDoubleClickEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton && e->pos().y() < metrics().tabRowH) {
        // A double-click on a tab while minimized is a "restore" gesture, not
        // a toggle. Qt may deliver it as Press (which already restored) …
        // DblClick — or, for synthetic clicks, as a lone DblClick — so:
        //  * still minimized → restore now (and switch to the clicked tab);
        //  * restored within the double-click interval (the press half of this
        //    very click pair did it) → leave it expanded;
        //  * otherwise → the usual toggle to minimized.
        const QString t = tabIdAt(e->pos());
        if (!t.isEmpty()) setCurrentTab(t);
        if (m_minimized) {
            setMinimized(false);
            m_restoreTimer.start();
        } else {
            const bool justRestored = m_restoreTimer.isValid()
                && m_restoreTimer.elapsed() <= QApplication::doubleClickInterval();
            if (!justRestored) setMinimized(true);
        }
        e->accept();
        return;
    }
    // A double-click on a button is two clicks: the second press was already
    // delivered as a press, so treat this like one to keep the pressed state.
    mousePressEvent(e);
}

void RibbonBar::leaveEvent(QEvent* e) {
    QWidget::leaveEvent(e);
    if (!m_hoverId.isEmpty()) { m_hoverId.clear(); refreshToolTip(); update(); }
    dismissRcxTooltip();
}

// ── Painting ──

void RibbonBar::paintEvent(QPaintEvent*) {
    ensureLayout();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);
    const Metrics m = metrics();
    const Theme& t = m_theme;

    // Whole widget first (the host may hand us more height than we asked for),
    // then the tab strip in the title-bar colour. The body is plain
    // `background`: no panel boxes, no caption bands.
    p.fillRect(rect(), t.background);
    p.fillRect(QRect(0, 0, width(), m.tabRowH), menuBarColor(t));
    if (!m_minimized) paintBody(p, m);
    paintTabs(p, m);
}

void RibbonBar::paintTabs(QPainter& p, const Metrics& m) {
    const Theme& t = m_theme;
    p.setFont(font());

    // One full-width hairline on the strip's last device row (minimized: the
    // extra row under the strip is the only line). The active tab replaces
    // it with kUnderlineRows device rows of the accent -- bottom-underline
    // model, painted in ONE fill (two 1-row fills skip a row at 125 %).
    const QRectF strip(0, 0, width(), m_minimized ? m.tabRowH + 1 : m.tabRowH);
    fillBottomDeviceRowOfRect(p, strip, t.border);

    const QColor inactive = ribbonToneColour(t.textMuted, t, menuBarColor(t));
    for (const auto& tab : m_spec) {
        const QRect r = tabRect(tab.id);
        const bool active = (tab.id == m_currentTab);
        const bool hovered = (m_hoverId == QStringLiteral("tab:") + tab.id);
        if (active)
            fillBottomDeviceRowsOfRect(p, QRectF(r.left(), 0, r.width(), strip.height()),
                                       kUnderlineRows, t.indHoverSpan);
        // Text ladder (same as the REECLASS / Code / Both pane tabs):
        // inactive textMuted, hover text, active text. No fill, no box.
        p.setPen((active || hovered) ? t.text : inactive);
        p.drawText(r, Qt::AlignCenter, tab.title);
    }
}

void RibbonBar::paintBody(QPainter& p, const Metrics& m) {
    const Theme& t = m_theme;
    const int bodyTop = m.tabRowH;
    const int bodyBottom = bodyTop + m.bodyH;          // exclusive
    const int itemsTop = bodyTop + m.padTop;
    const int itemsH = 3 * m.rowH;
    const qreal dpr = devicePixelRatioF();

    // Panel dividers: one 1-device-px column per gap, 2 px clear of the strip
    // hairline above and the body hairline below (never a grid junction).
    const int divTop = bodyTop + 2;
    const int divBottom = bodyBottom - 1 - 2;
    for (int dx : m_layout.dividerXs)
        fillLeftDeviceColOfRect(p, QRectF(dx, divTop, 1, divBottom - divTop), t.border);

    const int tabIdx = tabIndex(m_currentTab);
    const QColor captionTone = ribbonToneColour(t.textMuted, t, t.background);
    for (const LaidPanel& lp : m_layout.panels) {
        // `||` family separators span the rows only.
        for (int sx : lp.separatorXs)
            fillLeftDeviceColOfRect(p, QRectF(sx, itemsTop + 3, 1, itemsH - 6), t.border);
        p.setFont(captionFont());
        p.setPen(captionTone);
        QString caption = lp.id;
        if (tabIdx >= 0)
            for (const auto& ps : m_spec[tabIdx].panels)
                if (ps.id == lp.id) { caption = ps.caption; break; }
        p.drawText(lp.captionRect, Qt::AlignHCenter | Qt::AlignVCenter, caption);
    }

    p.setFont(font());
    for (const LaidItem& li : m_layout.items) paintItem(p, li, m);

    if (!m_layout.hiddenPanels.isEmpty()) {
        // The ... item: a small button like any other (rest textDim, hover
        // `hover` fill + text, pressed `button` while the menu is open).
        const QRect r = m_layout.overflowRect;
        const bool hovered = (m_hoverId == QStringLiteral("overflow"));
        const bool pressed = m_overflowOpen;
        if (pressed) p.fillRect(r, pressedFill(t));
        else if (hovered) p.fillRect(r, t.hover);
        const QColor tone = (hovered || pressed) ? t.text : ribbonToneColour(t.textDim, t, t.background);
        const QPixmap pm = tintedSvgIcon(QStringLiteral(":/vsicons/ellipsis.svg"), tone, m.smallIcon, dpr);
        drawPixmapSnapped(p, QPointF(r.left() + (r.width() - m.smallIcon) / 2,
                                     r.top() + (r.height() - m.smallIcon) / 2), pm);
    }

    fillBottomDeviceRowOfRect(p, QRectF(0, bodyBottom - 1, width(), 1), t.border);
}

// States, in paint order: opacity (disabled: everything below dims together)
// -> fill (pressed `button` / hover `hover`, no outline) -> icon + label in
// the state tone -> checked underline. Rest paints nothing but icon + label.
//   rest      Plain icon + label textDim; family icons full colour
//   hover     `hover` fill; Plain icon + label text
//   pressed   `button` fill (fallback `selected`)
//   checked   Plain icon + label indHoverSpan + 2 device rows underline
//   disabled  40 % opacity, never hover / pressed
//   destructive (Delete)  icon + label markerPtr in every state
void RibbonBar::paintItem(QPainter& p, const LaidItem& li, const Metrics& m) {
    const Theme& t = m_theme;
    const RibbonItemSpec* spec = itemSpec(li.ref);
    if (!spec) return;
    QAction* a = effectiveAction(li.id);
    const bool enabled = !a || a->isEnabled();
    const bool checked = a && a->isCheckable() && a->isChecked();
    const bool hovered = (m_hoverId == li.id) && enabled;
    const bool pressed = hovered && (m_pressedId == li.id);
    const QRect r = li.rect;

    const qreal prevOpacity = p.opacity();
    if (!enabled) p.setOpacity(prevOpacity * kDisabledOpacity);

    if (pressed) p.fillRect(r, pressedFill(t));
    else if (hovered) p.fillRect(r, t.hover);

    const QColor tone = spec->destructive ? t.markerPtr
                      : checked           ? t.indHoverSpan
                      : hovered           ? t.text
                                          : ribbonToneColour(t.textDim, t, t.background);
    RibbonIconOptions o;
    o.plainInk = tone;   // Plain Codicons only; family-tinted icons keep their colour
    p.setPen(tone);
    p.setFont(font());
    const qreal dpr = devicePixelRatioF();
    const QFontMetrics fm(font());

    if (li.large) {
        const QPixmap pm = ribbonIcon(spec->icon, RibbonIconSize::Large, dpr, t, o);
        const qreal iconL = m.largeIcon;   // logical, fractional at 125 % (32 device)
        if (li.label) {
            // icon + 2 + one text line, centred as a block in the 54-px cell
            const qreal blockH = iconL + 2 + fm.height();
            const qreal top = r.top() + (r.height() - blockH) / 2.0;
            drawPixmapSnapped(p, QPointF(r.left() + (r.width() - iconL) / 2.0, top), pm);
            const int labelTop = qRound(top + iconL + 2);
            // Elide only when the (integer) advance exceeds the budget: the
            // shaped run of a label that fits by the metric can be a fraction
            // wider, and elidedText would then trim it ("New Cla…").
            const int budget = r.width() - 8;
            const QString text = fm.horizontalAdvance(spec->label) > budget
                ? fm.elidedText(spec->label, Qt::ElideRight, budget) : spec->label;
            const QRect labelRect(r.left(), labelTop, r.width(), r.bottom() - labelTop + 1);
            p.drawText(labelRect, Qt::AlignHCenter | Qt::AlignTop | Qt::TextDontClip, text);
        } else {
            drawPixmapSnapped(p, QPointF(r.left() + (r.width() - iconL) / 2.0,
                                         r.top() + (r.height() - iconL) / 2.0), pm);
        }
    } else {
        const QPixmap pm = ribbonIcon(spec->icon, RibbonIconSize::Small, dpr, t, o);
        const int cellW = ribbonIconCellWidth(spec->icon);
        const int iconY = r.top() + (r.height() - m.smallIcon) / 2;
        if (li.label) {
            drawPixmapSnapped(p, QPointF(r.left() + 3, iconY), pm);
            const QRect labelRect(r.left() + 3 + cellW + 4, r.top(),
                                  r.width() - (3 + cellW + 4) - 6, r.height());
            p.drawText(labelRect, Qt::AlignLeft | Qt::AlignVCenter, spec->label);
        } else {
            drawPixmapSnapped(p, QPointF(r.left() + (r.width() - cellW) / 2, iconY), pm);
        }
    }
    if (checked) fillBottomDeviceRowsOfRect(p, r, kUnderlineRows, t.indHoverSpan);
    p.setOpacity(prevOpacity);
}


}  // namespace rcx
