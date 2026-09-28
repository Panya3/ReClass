#include "ribbon_actions.h"
#include "controller.h"
#include "editor.h"
#include <QTimer>
#include <QUndoStack>
#include <algorithm>

namespace rcx {

RibbonActions::RibbonActions(ControllerFn ctrl, EditorFn editor, QObject* parent)
    : QObject(parent)
    , m_ctrlFn(std::move(ctrl))
    , m_editorFn(std::move(editor)) {
    buildActions();
    refreshEnabled();
}

RibbonActions::~RibbonActions() {
    for (auto& cn : m_conns) QObject::disconnect(cn);
}

RcxController* RibbonActions::ctrl() const {
    RcxController* c = m_ctrlFn ? m_ctrlFn() : nullptr;
    return c ? c : m_active.data();
}

RcxEditor* RibbonActions::editor() const {
    RcxEditor* ed = m_editorFn ? m_editorFn() : nullptr;
    if (!ed)
        if (auto* c = ctrl()) ed = c->primaryEditor();
    return ed;
}

QAction* RibbonActions::action(const QString& id) const {
    return m_actions.value(id, nullptr);
}

QAction* RibbonActions::add(const QString& id, const QString& text,
                            const QString& tip, const QVariant& data) {
    auto* a = new QAction(text, this);
    a->setObjectName(id);
    a->setToolTip(tip);
    a->setStatusTip(tip);
    if (data.isValid()) a->setData(data);
    m_actions.insert(id, a);
    m_ids.append(id);
    return a;
}

bool RibbonActions::anyEditorEditing(RcxController* c, RcxEditor* extra) {
    if (extra && extra->isEditing()) return true;
    if (c)
        for (auto* e : c->editors())
            if (e && e->isEditing()) return true;
    return false;
}

bool RibbonActions::blockedByInlineEdit() {
    if (!anyEditorEditing(ctrl(), editor())) return false;
    emit statusHint(QStringLiteral("Finish the current edit first (Enter / Esc)"));
    return true;
}

void RibbonActions::wire(QAction* a, std::function<void()> fn) {
    connect(a, &QAction::triggered, this, [this, fn = std::move(fn)]() {
        if (blockedByInlineEdit()) return;
        fn();
    });
}

int RibbonActions::lineOfNode(RcxEditor* ed, uint64_t nodeId) {
    if (!ed) return -1;
    for (int i = 0;; ++i) {
        const LineMeta* lm = ed->metaForLine(i);
        if (!lm) return -1;
        if (lm->nodeId == nodeId && lm->lineKind == LineKind::Field
            && !lm->isContinuation && !lm->isMemberLine)
            return i;
    }
}

void RibbonActions::buildActions() {
    // ── Type panel ──
    // Fixed-kind buttons: one slot, the kind rides in data().
    auto typeAct = [this](const QString& id, NodeKind k, const QString& text,
                          const QString& tip) {
        QAction* a = add(id, text, tip, int(k));
        wire(a, [this, k]() {
            if (auto* c = ctrl()) c->retypeSelection(k);
        });
    };
    typeAct(QStringLiteral("type.hex64"),  NodeKind::Hex64,  QStringLiteral("Hex 64"),
            QStringLiteral("Change to hex64 — 8 raw bytes (key 4)"));
    typeAct(QStringLiteral("type.hex32"),  NodeKind::Hex32,  QStringLiteral("Hex 32"),
            QStringLiteral("Change to hex32 — 4 raw bytes (key 3)"));
    typeAct(QStringLiteral("type.hex16"),  NodeKind::Hex16,  QStringLiteral("Hex 16"),
            QStringLiteral("Change to hex16 — 2 raw bytes (key 2)"));
    typeAct(QStringLiteral("type.hex8"),   NodeKind::Hex8,   QStringLiteral("Hex 8"),
            QStringLiteral("Change to hex8 — 1 raw byte (key 1)"));
    typeAct(QStringLiteral("type.int64"),  NodeKind::Int64,  QStringLiteral("Int 64"),
            QStringLiteral("Change to int64_t — signed 8-byte integer (S on an 8-byte field)"));
    typeAct(QStringLiteral("type.int32"),  NodeKind::Int32,  QStringLiteral("Int 32"),
            QStringLiteral("Change to int32_t — signed 4-byte integer (S on a 4-byte field)"));
    typeAct(QStringLiteral("type.int16"),  NodeKind::Int16,  QStringLiteral("Int 16"),
            QStringLiteral("Change to int16_t — signed 2-byte integer (S on a 2-byte field)"));
    typeAct(QStringLiteral("type.int8"),   NodeKind::Int8,   QStringLiteral("Int 8"),
            QStringLiteral("Change to int8_t — signed byte (S on a 1-byte field)"));
    typeAct(QStringLiteral("type.uint64"), NodeKind::UInt64, QStringLiteral("UInt 64"),
            QStringLiteral("Change to uint64_t — unsigned 8-byte integer (U on an 8-byte field)"));
    typeAct(QStringLiteral("type.uint32"), NodeKind::UInt32, QStringLiteral("UInt 32"),
            QStringLiteral("Change to uint32_t — unsigned 4-byte integer (U on a 4-byte field)"));
    typeAct(QStringLiteral("type.uint16"), NodeKind::UInt16, QStringLiteral("UInt 16"),
            QStringLiteral("Change to uint16_t — unsigned 2-byte integer (U on a 2-byte field)"));
    typeAct(QStringLiteral("type.uint8"),  NodeKind::UInt8,  QStringLiteral("UInt 8"),
            QStringLiteral("Change to uint8_t — unsigned byte (U on a 1-byte field)"));
    typeAct(QStringLiteral("type.double"), NodeKind::Double, QStringLiteral("Double"),
            QStringLiteral("Change to double — 8-byte float (F on an 8-byte field)"));
    typeAct(QStringLiteral("type.float"),  NodeKind::Float,  QStringLiteral("Float"),
            QStringLiteral("Change to float — 4-byte float (F on a 4-byte field)"));
    typeAct(QStringLiteral("type.bool"),   NodeKind::Bool,   QStringLiteral("Bool"),
            QStringLiteral("Change to bool — 1 byte"));
    typeAct(QStringLiteral("type.vec2"),   NodeKind::Vec2,   QStringLiteral("Vec 2"),
            QStringLiteral("Change to vec2 — 2 floats, 8 bytes"));
    typeAct(QStringLiteral("type.vec3"),   NodeKind::Vec3,   QStringLiteral("Vec 3"),
            QStringLiteral("Change to vec3 — 3 floats, 12 bytes"));
    typeAct(QStringLiteral("type.vec4"),   NodeKind::Vec4,   QStringLiteral("Vec 4"),
            QStringLiteral("Change to vec4 — 4 floats, 16 bytes"));
    typeAct(QStringLiteral("type.mat4x4"), NodeKind::Mat4x4, QStringLiteral("Mat 4x4"),
            QStringLiteral("Change to mat4x4 — 16 floats, 64 bytes"));

    // Pointer / function pointer: 64 vs 32 decided by tree.pointerSize at
    // trigger time; data() and the tooltip are relabelled on every refresh.
    {
        QAction* a = add(QStringLiteral("type.pointer"), QStringLiteral("Pointer"),
                         QStringLiteral("Change to ptr64 — 8-byte pointer (P key)"),
                         int(NodeKind::Pointer64));
        wire(a, [this]() {
            auto* c = ctrl();
            if (!c || !c->document()) return;
            c->retypeSelection(c->document()->tree.pointerSize >= 8
                               ? NodeKind::Pointer64 : NodeKind::Pointer32);
        });
        QAction* f = add(QStringLiteral("type.funcptr"), QStringLiteral("Func Ptr"),
                         QStringLiteral("Change to fnptr64 — 8-byte function pointer"),
                         int(NodeKind::FuncPtr64));
        wire(f, [this]() {
            auto* c = ctrl();
            if (!c || !c->document()) return;
            c->retypeSelection(c->document()->tree.pointerSize >= 8
                               ? NodeKind::FuncPtr64 : NodeKind::FuncPtr32);
        });
    }
    {
        QAction* a = add(QStringLiteral("type.ptrclass"), QStringLiteral("Ptr→Class"),
                         QStringLiteral("Change to a pointer to a new class (each selected 4/8-byte field)"));
        wire(a, [this]() {
            if (auto* c = ctrl()) c->convertSelectionToTypedPointers();
        });
    }
    {
        QAction* a = add(QStringLiteral("type.class"), QStringLiteral("Class"),
                         QStringLiteral("Break the selected bytes / fields into a new embedded class (Ctrl+Shift+B)"));
        wire(a, [this]() {
            auto* c = ctrl();
            if (!c) return;
            auto region = c->regionFromCurrentSelection(editor());
            if (!region) {
                emit statusHint(QStringLiteral("Break: select bytes or fields first"));
                return;
            }
            c->extractByteSelectionToNewClass(region->first, region->second);
        });
    }
    {
        QAction* a = add(QStringLiteral("type.array"), QStringLiteral("Array"),
                         QStringLiteral("Turn the selected field (or contiguous same-type fields) into an array"));
        wire(a, [this]() {
            if (auto* c = ctrl()) c->makeArrayFromSelection();
        });
    }
    {
        QAction* a = add(QStringLiteral("type.custom"), QStringLiteral("Custom…"),
                         QStringLiteral("Type any type name for the selected field (inline type edit)"));
        wire(a, [this]() {
            auto* c = ctrl();
            auto* ed = editor();
            if (!c || !ed) return;
            const SelectionSummary s = summarize();
            if (s.plainCount != 1) {
                emit statusHint(QStringLiteral("Custom type: select exactly one field"));
                return;
            }
            // A selected field can be hidden (its parent folded): refresh()
            // keeps the selection but composes no row for it. Without a row
            // beginInlineEdit(-1) would edit whatever sits at the Scintilla
            // cursor — a different node.
            const int line = lineOfNode(ed, s.plainIds.first());
            if (line < 0) {
                emit statusHint(QStringLiteral("Custom type: the selected field is not visible — expand its parent first"));
                return;
            }
            ed->beginInlineEdit(EditTarget::Type, line);
        });
    }
    typeAct(QStringLiteral("type.utf8"),  NodeKind::UTF8,  QStringLiteral("ASCII"),
            QStringLiteral("Change to str — ASCII / UTF-8 text"));
    typeAct(QStringLiteral("type.utf16"), NodeKind::UTF16, QStringLiteral("UTF-16"),
            QStringLiteral("Change to wstr — UTF-16 text"));

    // ── Add / Insert panels ──
    static const int kByteCounts[] = {4, 8, 64, 1024, 2048};
    for (int n : kByteCounts) {
        QAction* a = add(QStringLiteral("add.%1").arg(n), QStringLiteral("Add %1").arg(n),
                         QStringLiteral("Append %1 bytes to the end of the class").arg(n), n);
        wire(a, [this, n]() {
            if (auto* c = ctrl()) c->appendBytes(0, n);
        });
    }
    for (int n : kByteCounts) {
        QAction* a = add(QStringLiteral("insert.%1").arg(n), QStringLiteral("Insert %1").arg(n),
                         QStringLiteral("Insert %1 bytes above the selected field").arg(n), n);
        wire(a, [this, n]() {
            auto* c = ctrl();
            if (!c) return;
            const SelectionSummary s = summarize();
            if (s.insertAnchorId)
                c->insertBytesAbove(s.insertAnchorId, n);
            else if (s.footerTargetId)
                c->appendBytes(s.footerTargetId, n);   // footer row = "after the last field"
            else
                emit statusHint(QStringLiteral("Insert: select a field first"));
        });
    }

    // ── Selected panel ──
    {
        QAction* a = add(QStringLiteral("sel.delete"), QStringLiteral("Delete"),
                         QStringLiteral("Delete the selected fields (Delete)"));
        wire(a, [this]() {
            if (auto* c = ctrl()) c->deleteSelection();
        });
        QAction* d = add(QStringLiteral("sel.duplicate"), QStringLiteral("Duplicate"),
                         QStringLiteral("Duplicate the selected fields below themselves (Ctrl+D)"));
        wire(d, [this]() {
            if (auto* c = ctrl()) c->duplicateSelection();
        });
        QAction* cm = add(QStringLiteral("sel.comment"), QStringLiteral("Comment"),
                          QStringLiteral("Edit the comment of the selected field(s) (;)"));
        wire(cm, [this]() {
            if (auto* c = ctrl()) c->commentSelection(editor());
        });
        QAction* z = add(QStringLiteral("sel.zero"), QStringLiteral("Zero"),
                         QStringLiteral("Fill the selected bytes with 00"));
        wire(z, [this]() {
            if (auto* c = ctrl()) c->fillSelectionBytes(RcxController::ByteFill::Zero);
        });
        QAction* ff = add(QStringLiteral("sel.ff"), QStringLiteral("FF"),
                          QStringLiteral("Fill the selected bytes with FF"));
        wire(ff, [this]() {
            if (auto* c = ctrl()) c->fillSelectionBytes(RcxController::ByteFill::FF);
        });
        QAction* r = add(QStringLiteral("sel.random"), QStringLiteral("Random"),
                         QStringLiteral("Fill the selected bytes with random values"));
        wire(r, [this]() {
            if (auto* c = ctrl()) c->fillSelectionBytes(RcxController::ByteFill::Random);
        });
        QAction* sw = add(QStringLiteral("sel.swap"), QStringLiteral("Swap"),
                          QStringLiteral("Toggle big-endian display of the selected scalar(s)"));
        wire(sw, [this]() {
            if (auto* c = ctrl()) c->toggleBigEndianSelection();
        });
    }

    // ── Edit ──
    {
        QAction* u = add(QStringLiteral("edit.undo"), QStringLiteral("Undo"),
                         QStringLiteral("Undo (Ctrl+Z)"));
        wire(u, [this]() {
            auto* c = ctrl();
            if (c && c->document()) c->document()->undoStack.undo();
        });
        QAction* rd = add(QStringLiteral("edit.redo"), QStringLiteral("Redo"),
                          QStringLiteral("Redo (Ctrl+Y)"));
        wire(rd, [this]() {
            auto* c = ctrl();
            if (c && c->document()) c->document()->undoStack.redo();
        });
    }
}

void RibbonActions::setActiveController(RcxController* c) {
    for (auto& cn : m_conns) QObject::disconnect(cn);
    m_conns.clear();
    m_active = c;
    if (c) {
        m_conns << connect(c, &RcxController::selectionChanged, this,
                           [this](int) { scheduleRefresh(); });
        m_conns << connect(c, &RcxController::sourceStatusChanged, this,
                           [this](RcxController::SourceStatus) { scheduleRefresh(); });
        m_conns << connect(c, &RcxController::sourceLivenessChanged, this,
                           [this](bool) { scheduleRefresh(); });
        // The controller can die under us (tab closed): drop every binding
        // rather than leave the document / undo-stack ones dangling.
        m_conns << connect(c, &QObject::destroyed, this, [this]() {
            for (auto& cn : m_conns) QObject::disconnect(cn);
            m_conns.clear();
            m_active = nullptr;
            scheduleRefresh();
        });
        // Inline edits: nothing announces the START of an edit (the
        // trigger-time guard covers that), but the END must re-enable the
        // ribbon promptly — onRefreshTick returns early while editing, so
        // without these the predicate would stay stale until the next
        // unrelated signal. Editors split later are not covered; the guard
        // still is.
        for (auto* e : c->editors()) {
            if (!e) continue;
            m_conns << connect(e, &RcxEditor::inlineEditCommitted, this,
                               [this](int, int, EditTarget, const QString&, uint64_t) {
                                   scheduleRefresh();
                               });
            m_conns << connect(e, &RcxEditor::inlineEditCancelled, this,
                               &RibbonActions::scheduleRefresh);
        }
        if (auto* doc = c->document()) {
            m_conns << connect(doc, &RcxDocument::documentChanged, this,
                               &RibbonActions::scheduleRefresh);
            m_conns << connect(&doc->undoStack, &QUndoStack::indexChanged, this,
                               [this](int) { scheduleRefresh(); });
            m_conns << connect(&doc->undoStack, &QUndoStack::canUndoChanged, this,
                               [this](bool) { scheduleRefresh(); });
            m_conns << connect(&doc->undoStack, &QUndoStack::canRedoChanged, this,
                               [this](bool) { scheduleRefresh(); });
        }
    }
    refreshEnabled();
}

void RibbonActions::scheduleRefresh() {
    if (m_refreshPending) return;
    m_refreshPending = true;
    QTimer::singleShot(0, this, [this]() {
        m_refreshPending = false;
        refreshEnabled();
    });
}

RibbonActions::SelectionSummary RibbonActions::summarize() const {
    SelectionSummary s;
    RcxController* c = ctrl();
    if (!c || !c->document()) return s;
    RcxDocument* doc = c->document();
    const NodeTree& tree = doc->tree;

    s.hasDoc = true;
    s.ptrSize = tree.pointerSize;
    s.canUndo = doc->undoStack.canUndo();
    s.canRedo = doc->undoStack.canRedo();
    s.showComments = c->showComments();
    s.writable = doc->provider && doc->provider->isWritable() && !c->readOnlyOverride();

    RcxEditor* ed = editor();
    s.editing = anyEditorEditing(c, ed);
    for (auto* e : c->editors())
        if (e && e->hasByteSelection() && e->byteSelectionByteCount() > 0) { s.byteSel = true; break; }
    s.regionOk = c->regionFromCurrentSelection(ed).has_value();

    // Append target: the viewed class, else the first root struct.
    {
        uint64_t t = c->viewRootId();
        if (t == 0 || tree.indexOfId(t) < 0) {
            t = 0;
            for (const auto& n : tree.nodes)
                if (n.kind == NodeKind::Struct && n.parentId == 0) { t = n.id; break; }
        }
        const int ti = t ? tree.indexOfId(t) : -1;
        if (ti >= 0) {
            s.targetStruct = true;
            s.targetIsEnum = tree.nodes[ti].isEnum();
            s.targetIsBitfield = tree.nodes[ti].isBitfield();
        }
    }

    // Selection classification. selIds are encoded (footer / array-element /
    // member high bits) — classify with selKindOf (priority order) and
    // decode with baseNodeIdFromSelId, never by testing the bits directly.
    struct P { uint64_t id; int64_t off; };
    auto byOffset = [](const P& a, const P& b) {
        return a.off != b.off ? a.off < b.off : a.id < b.id;
    };
    QVector<P> plain, footers;
    QSet<uint64_t> seen, deletable;
    bool anyContainer = false;
    for (uint64_t sid : c->selectedIds()) {
        const uint64_t nid = baseNodeIdFromSelId(sid);
        const int idx = tree.indexOfId(nid);
        if (idx < 0) continue;
        const Node& n = tree.nodes[idx];
        if (!seen.contains(nid)) {
            seen.insert(nid);
            s.baseIds.append(nid);
            if (isContainerKind(n.kind)) anyContainer = true;
        }
        switch (selKindOf(sid)) {
        case SelKind::Footer:
            s.anyFooter = true;
            // Only Struct / Array containers take raw bytes; an enum or
            // bitfield footer is never an append target.
            if (!n.isEnum() && !n.isBitfield()
                && std::none_of(footers.begin(), footers.end(),
                                [nid](const P& p) { return p.id == nid; }))
                footers.append({nid, tree.computeOffset(idx)});
            // deleteSelection treats the `}` row as its container's own row
            // (a root class has no other selectable row).
            deletable.insert(nid);
            break;
        case SelKind::Member:
            s.anyMember = true;
            break;
        case SelKind::ArrayElem:
            s.anyArrayElem = true;
            // deleteSelection treats an element row as its whole Array.
            if (n.kind == NodeKind::Array && n.parentId != 0) deletable.insert(nid);
            break;
        case SelKind::Plain:
            if (std::none_of(plain.begin(), plain.end(),
                             [nid](const P& p) { return p.id == nid; })) {
                plain.append({nid, tree.computeOffset(idx)});
                if (n.parentId == 0) s.anyRoot = true;
                deletable.insert(nid);   // a lone root goes through deleteRootStruct
                if (n.parentId != 0 && !isContainerKind(n.kind)) s.duplicableCount++;
            }
            break;
        }
    }
    std::sort(plain.begin(), plain.end(), byOffset);
    std::sort(footers.begin(), footers.end(), byOffset);
    if (!footers.isEmpty()) s.footerTargetId = footers.first().id;
    for (const P& p : plain) {
        s.plainIds.append(p.id);
        const Node& n = tree.nodes[tree.indexOfId(p.id)];
        if (n.parentId != 0 && !s.insertAnchorId) s.insertAnchorId = p.id;
        if (isContainerKind(n.kind)) continue;
        if (isEndianSwappable(n.kind)) s.anyScalar = true;
        const int sz = n.byteSize();
        if ((sz == 4 || sz == 8) && n.refId == 0) s.anyPtrConvertible = true;
    }
    s.plainCount = plain.size();
    s.deletableCount = deletable.size();
    s.allLeaf = s.plainCount > 0 && !s.anyFooter && !s.anyMember
             && !s.anyArrayElem && !s.anyRoot && !anyContainer;
    return s;
}

void RibbonActions::refreshEnabled() {
    m_refreshPending = false;
    const SelectionSummary s = summarize();
    m_last = s;

    const bool live     = s.hasDoc && !s.editing;
    const bool typeBase = live && s.allLeaf;
    const bool fillOk   = live && s.writable && s.regionOk;

    for (const QString& id : m_ids) {
        QAction* a = m_actions.value(id);
        bool en = false;
        if (id == QLatin1String("type.class"))         en = live && s.regionOk;
        else if (id == QLatin1String("type.ptrclass")) en = typeBase && s.anyPtrConvertible;
        else if (id == QLatin1String("type.custom"))   en = typeBase && s.plainCount == 1 && editor() != nullptr;
        else if (id.startsWith(QLatin1String("type.")))   en = typeBase;
        else if (id.startsWith(QLatin1String("add.")))    en = live && s.targetStruct && !s.targetIsEnum && !s.targetIsBitfield;
        else if (id.startsWith(QLatin1String("insert."))) en = live && (s.insertAnchorId || s.footerTargetId);
        else if (id == QLatin1String("sel.delete"))    en = live && s.deletableCount >= 1;
        else if (id == QLatin1String("sel.duplicate")) en = live && s.duplicableCount >= 1;
        else if (id == QLatin1String("sel.comment"))   en = live && s.showComments && s.plainCount >= 1;
        else if (id == QLatin1String("sel.zero")
              || id == QLatin1String("sel.ff")
              || id == QLatin1String("sel.random"))     en = fillOk;
        else if (id == QLatin1String("sel.swap"))      en = live && s.anyScalar;
        else if (id == QLatin1String("edit.undo"))     en = live && s.canUndo;
        else if (id == QLatin1String("edit.redo"))     en = live && s.canRedo;
        a->setEnabled(en);
    }

    // Pointer-size relabel: the P key and these two buttons follow the
    // document's pointer width.
    const bool p64 = s.ptrSize >= 8;
    if (QAction* p = m_actions.value(QStringLiteral("type.pointer"))) {
        p->setData(int(p64 ? NodeKind::Pointer64 : NodeKind::Pointer32));
        p->setToolTip(p64 ? QStringLiteral("Change to ptr64 — 8-byte pointer (P key)")
                          : QStringLiteral("Change to ptr32 — 4-byte pointer (P key)"));
        p->setStatusTip(p->toolTip());
    }
    if (QAction* f = m_actions.value(QStringLiteral("type.funcptr"))) {
        f->setData(int(p64 ? NodeKind::FuncPtr64 : NodeKind::FuncPtr32));
        f->setToolTip(p64 ? QStringLiteral("Change to fnptr64 — 8-byte function pointer")
                          : QStringLiteral("Change to fnptr32 — 4-byte function pointer"));
        f->setStatusTip(f->toolTip());
    }
    emit refreshed();
}

} // namespace rcx
