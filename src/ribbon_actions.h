#pragma once
// RibbonActions — the wiring half of the ReClassEx-style ribbon.
//
// Owns one QAction per Modify-tab button and connects each `triggered` to
// the matching RcxController selection op. The controller is resolved
// THROUGH THE CALLBACK AT TRIGGER TIME (MainWindow reassigns the active
// dock directly in several places, so a cached pointer would go stale);
// `setActiveController` only decides which controller's signals drive the
// enabled-state refresh. Built from two std::functions so tests can drive
// it without a MainWindow, exactly like the controller-resolving lambda
// MainWindow hands around today.
//
// Action ids (the ribbon widget and the overflow menu look them up by id;
// `data()` carries the NodeKind for `type.*` and the byte count for
// `add.*` / `insert.*` so one slot serves a whole family):
//   type.hex64 type.hex32 type.hex16 type.hex8
//   type.int64 type.int32 type.int16 type.int8
//   type.uint64 type.uint32 type.uint16 type.uint8
//   type.double type.float type.bool
//   type.vec2 type.vec3 type.vec4 type.mat4x4
//   type.pointer type.funcptr type.ptrclass
//   type.class type.array type.custom
//   type.utf8 type.utf16
//   add.4 add.8 add.64 add.1024 add.2048
//   insert.4 insert.8 insert.64 insert.1024 insert.2048
//   sel.delete sel.duplicate sel.comment sel.zero sel.ff sel.random
//   sel.swap (Big endian — checkable) sel.rtti (no op here; MainWindow
//     forwards it to Tools ▸ RTTI Browser)
//   edit.undo edit.redo — the title-strip quick-access pair; they have no
//     ribbon button, so their label + tooltip are passed in rather than read
//     from defaultRibbonSpec().
//
// Never addAction() these to a widget: the editor already owns the
// Delete / Insert / 1-5 / P / F / S / U keys; shortcuts appear in tooltips
// only.
#include "core.h"
#include <QAction>
#include <QHash>
#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QVariant>
#include <QVector>
#include <functional>

namespace rcx {

class RcxController;
class RcxEditor;

class RibbonActions : public QObject {
    Q_OBJECT
public:
    using ControllerFn = std::function<RcxController*()>;
    using EditorFn     = std::function<RcxEditor*()>;

    RibbonActions(ControllerFn ctrl, EditorFn editor, QObject* parent = nullptr);
    ~RibbonActions() override;

    // nullptr for an unknown id.
    QAction* action(const QString& id) const;
    // Every id, in creation (= ribbon) order.
    QStringList ids() const { return m_ids; }

    // Rebind the enabled-state refresh to `ctrl`'s signals (selection,
    // source status/liveness, document change, undo-stack index / canUndo /
    // canRedo). Disconnects the previous controller first; nullptr detaches
    // and disables everything. Refreshes immediately.
    void setActiveController(RcxController* ctrl);
    RcxController* activeController() const { return m_active; }

    // Coalesced refresh: many controller signals fire back-to-back (e.g.
    // selectionChanged from inside updateCommandRow right after a push),
    // so the actual recompute runs once on the next event-loop turn.
    void scheduleRefresh();
    // Recompute every action's enabled state (and the pointer-size labels)
    // from the current selection. Emits refreshed().
    void refreshEnabled();

    // One snapshot of "what is selected" — the input to every enabled
    // predicate. Public so tests (and the ribbon widget's tooltips) can
    // inspect the classification.
    struct SelectionSummary {
        QVector<uint64_t> baseIds;     // every selected row decoded to an existing node (deduped)
        QVector<uint64_t> plainIds;    // plain rows only, ascending root-relative offset
        int  plainCount = 0;
        bool anyFooter = false;
        bool anyMember = false;
        bool anyArrayElem = false;
        bool anyRoot = false;          // a plain row that is a root struct
        bool allLeaf = false;          // plainCount > 0, no footer/member/root, no container among baseIds
        bool anyScalar = false;        // a plain leaf whose kind isEndianSwappable (Swap target)
        bool allBigEndian = false;     // …and every one of them is already big-endian (the
                                       // checked state of the Big endian toggle)
        bool anyPtrConvertible = false;// a plain 4/8-byte leaf without refId (Ptr→Class target)
        bool byteSel = false;          // an editor holds a non-empty byte selection
        bool editing = false;          // any of the controller's editors is inline-editing
        bool hasDoc = false;
        bool targetStruct = false;     // an append target (view root / first root struct) exists
        bool targetIsEnum = false;     // …but it is an enum (takes members, not raw bytes)
        bool targetIsBitfield = false; // …but it is a bitfield (same)
        bool writable = false;         // provider writable and no read-only override
        bool regionOk = false;         // regionFromCurrentSelection() has a value
        bool showComments = false;
        bool canUndo = false;
        bool canRedo = false;
        int  ptrSize = 8;
        uint64_t insertAnchorId = 0;   // lowest-offset plain non-root node (Insert anchor)
        uint64_t footerTargetId = 0;   // lowest-offset selected Struct/Array footer's container
                                       // (Insert → append fallback); enum / bitfield footers never
        int  deletableCount = 0;       // what deleteSelection would remove: plain rows, footer
                                       // rows' containers (a root's only row) and array-element
                                       // rows' Arrays, deduped by node
        int  duplicableCount = 0;      // plain leaf rows with a parent (duplicateNode refuses containers)
    };
    SelectionSummary summarize() const;
    const SelectionSummary& lastSummary() const { return m_last; }

signals:
    void refreshed();
    // Short user-facing message for the status bar (a trigger that found
    // nothing usable selected, etc.). MainWindow routes it to setAppStatus.
    void statusHint(const QString& text);
    // sel.rtti has no controller op of its own; MainWindow owns the browser.
    // Routing it through a signal (rather than connecting to the QAction
    // directly) keeps it behind wire()'s inline-edit guard like every other
    // ribbon command.
    void rttiRequested();

private:
    QAction* add(const QString& id, const QString& text, const QString& tip,
                 const QVariant& data = QVariant());
    // Label + tooltip from defaultRibbonSpec() (ribbon_spec.h) — the single
    // source of a command's words.
    QAction* add(const QString& id, const QVariant& data = QVariant());
    // Every trigger goes through here: the inline-edit guard runs first,
    // then the op. Nothing signals when an inline edit BEGINS (F2 / Enter
    // path), so the enabled predicate can be stale — the guard at trigger
    // time is what actually protects the typed text.
    void wire(QAction* a, std::function<void()> fn);
    bool blockedByInlineEdit();
    static bool anyEditorEditing(RcxController* c, RcxEditor* extra);
    RcxController* ctrl() const;
    RcxEditor* editor() const;
    void buildActions();
    // The display line of a node's first Field row in `ed` (-1 if not shown).
    static int lineOfNode(RcxEditor* ed, uint64_t nodeId);

    ControllerFn m_ctrlFn;
    EditorFn m_editorFn;
    QHash<QString, QAction*> m_actions;
    QStringList m_ids;
    QPointer<RcxController> m_active;
    QVector<QMetaObject::Connection> m_conns;
    bool m_refreshPending = false;
    SelectionSummary m_last;
};

} // namespace rcx
