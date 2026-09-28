// Drill-down breadcrumb (v3 — CLICK-driven focus path): the breadcrumb reflects
// where the selection sits in the inline-expanded tree. Selecting a typed
// pointer (or any row inside its expansion) adds it to the breadcrumb; there is
// no follow-arrow affordance. Crumb click = collapse below + scroll. Exercises
// focusChainToNode / handleNodeClick / collapseToFocus and the BreadcrumbBar.

#include <QtTest/QTest>
#include <QtTest/QSignalSpy>
#include <QApplication>
#include <QSettings>
#include <QLabel>
#include <QSplitter>
#include <QToolButton>
#include <QtEndian>

#include "address_callbacks.h"
#include "controller.h"
#include "core.h"
#include "gotoaddressdialog.h"
#include "providers/buffer_provider.h"
#include "widgets/breadcrumb_bar.h"

using namespace rcx;

// RcxEditor.vptr → QWidgetPrivate.parent → QWidget. dptr is a non-drillable
// pointer (no refId); leaf is a plain top-level field.
struct Ids { uint64_t editor, priv, widget, vptr, dptr, parent, leaf; };

static Ids buildChain(NodeTree& tree) {
    tree.baseAddress = 0;
    Ids id{};
    auto addRoot = [&](const char* type) -> uint64_t {
        Node n; n.kind = NodeKind::Struct; n.structTypeName = type;
        n.parentId = 0; n.offset = 0; n.collapsed = false;
        return tree.nodes[tree.addNode(n)].id;
    };
    auto addPtr = [&](uint64_t parent, const char* name, uint64_t refId, int off) -> uint64_t {
        Node n; n.kind = NodeKind::Pointer64; n.name = name;
        n.parentId = parent; n.offset = off; n.refId = refId; n.collapsed = true;
        return tree.nodes[tree.addNode(n)].id;
    };
    id.editor = addRoot("RcxEditor");
    id.priv   = addRoot("QWidgetPrivate");
    id.widget = addRoot("QWidget");
    id.vptr   = addPtr(id.editor, "vptr",   id.priv,   0);
    id.dptr   = addPtr(id.editor, "dptr",   0,         8);   // no refId → not drillable
    id.parent = addPtr(id.priv,   "parent", id.widget, 0);
    Node lf; lf.kind = NodeKind::UInt32; lf.name = "leaf"; lf.parentId = id.editor; lf.offset = 16;
    id.leaf = tree.nodes[tree.addNode(lf)].id;
    return id;
}

// Crumbs in the production dotted shape (pushBreadcrumb): "Class.field"
// ancestors, a bare "Class" deepest, rootId = crumb index. classId is any
// non-zero id — the widget only reads label and index.
static Crumb crumb(const QString& label, uint64_t index, uint64_t classId = 1) {
    Crumb c; c.label = label; c.rootId = index; c.classId = classId;
    return c;
}
static QVector<Crumb> twoLevel() {
    return { crumb(QStringLiteral("RcxEditor.vptr"), 0, 1),
             crumb(QStringLiteral("QWidgetPrivate"), 1, 2) };
}

class TestBreadcrumb : public QObject {
    Q_OBJECT
private:
    RcxDocument*   m_doc = nullptr;
    RcxController* m_ctrl = nullptr;
    QSplitter*     m_splitter = nullptr;
    RcxEditor*     m_editor = nullptr;
    Ids            m_id{};
    // rebaseTo pushes into the Goto dialog's persisted recent list (the
    // user's real QSettings) — snapshot it per test and put it back.
    QStringList    m_savedRecent;
    bool           m_hadRecent = false;

    int idx(uint64_t nodeId) const { return m_doc->tree.indexOfId(nodeId); }
    bool collapsed(uint64_t nodeId) const {
        int i = idx(nodeId);
        return i >= 0 && m_doc->tree.nodes[i].collapsed;
    }
    void expand(uint64_t id) { m_doc->tree.nodes[idx(id)].collapsed = false; m_ctrl->refresh(); }
    int lineOf(uint64_t id) const {
        const auto& m = m_ctrl->lastResult().meta;
        for (int i = 0; i < m.size(); ++i)
            if (m[i].nodeId == id && m[i].lineKind != LineKind::Footer) return i;
        return -1;
    }
    QVector<Crumb> crumbs() const { return m_editor->breadcrumbBar()->crumbs(); }
    // Bytes holding real little-endian pointers for buildChain at base 0,
    // pointerSize 8: RcxEditor@0 .vptr = 0x20 → QWidgetPrivate@0x20 .parent
    // = 0x30 → QWidget@0x30. 64 bytes, so every target is readable.
    static QByteArray chainBytes() {
        QByteArray b(64, '\0');
        qToLittleEndian<quint64>(0x20, b.data() + 0x00);
        qToLittleEndian<quint64>(0x30, b.data() + 0x20);
        return b;
    }
    void drillTwoLevels() {
        expand(m_id.vptr);
        expand(m_id.parent);
        m_ctrl->handleNodeClick(m_editor, lineOf(m_id.parent), m_id.parent, Qt::NoModifier);
        QCOMPARE(m_ctrl->focusPath(), (QVector<uint64_t>{ m_id.vptr, m_id.parent }));
    }

private slots:
    void init() {
        {
            QSettings s("REECLASS", "REECLASS");
            m_hadRecent = s.contains(GotoAddressDialog::kSettingsKey);
            m_savedRecent = s.value(GotoAddressDialog::kSettingsKey).toStringList();
        }
        m_doc = new RcxDocument();
        m_id = buildChain(m_doc->tree);
        m_doc->provider = std::make_unique<BufferProvider>(QByteArray(64, '\0'));
        m_splitter = new QSplitter();
        m_ctrl = new RcxController(m_doc, nullptr);
        m_editor = m_ctrl->addSplitEditor(m_splitter);
        m_splitter->resize(800, 600);
        m_splitter->show();
        QVERIFY(QTest::qWaitForWindowExposed(m_splitter));
        QApplication::processEvents();
        m_ctrl->setViewRootId(m_id.editor);
    }

    void cleanup() {
        delete m_ctrl; m_ctrl = nullptr;
        m_editor = nullptr;
        delete m_splitter; m_splitter = nullptr;
        delete m_doc; m_doc = nullptr;
        QSettings s("REECLASS", "REECLASS");
        if (m_hadRecent) s.setValue(GotoAddressDialog::kSettingsKey, m_savedRecent);
        else             s.remove(GotoAddressDialog::kSettingsKey);
    }

    // ── focusChainToNode: the click-driven focus logic ──

    void testTopLevelSelectionHasEmptyFocus() {
        // A collapsed top-level pointer (nothing "inside") and a plain field
        // both yield the bare root crumb.
        QVERIFY(m_ctrl->focusChainToNode(m_id.vptr).isEmpty());
        QVERIFY(m_ctrl->focusChainToNode(m_id.leaf).isEmpty());
    }

    void testSelectExpandedPointerAddsIt() {
        expand(m_id.vptr);
        const QVector<uint64_t> chain = m_ctrl->focusChainToNode(m_id.vptr);
        QCOMPARE(chain.size(), 1);
        QCOMPARE(chain[0], m_id.vptr);
    }

    void testSelectInsideExpansionAddsContainingPointer() {
        expand(m_id.vptr);   // QWidgetPrivate renders inline; `parent` sits in it
        const QVector<uint64_t> chain = m_ctrl->focusChainToNode(m_id.parent);
        QCOMPARE(chain.size(), 1);
        QCOMPARE(chain[0], m_id.vptr);   // selecting inside QWidgetPrivate shows vptr
    }

    void testNestedSelectionBuildsFullChain() {
        expand(m_id.vptr);
        expand(m_id.parent);  // QWidget renders inside QWidgetPrivate
        const QVector<uint64_t> chain = m_ctrl->focusChainToNode(m_id.parent);
        QCOMPARE(chain.size(), 2);
        QCOMPARE(chain[0], m_id.vptr);
        QCOMPARE(chain[1], m_id.parent);
    }

    void testHandleNodeClickSetsFocusPath() {
        expand(m_id.vptr);
        const int ln = lineOf(m_id.vptr);
        QVERIFY(ln >= 0);
        m_ctrl->handleNodeClick(m_editor, ln, m_id.vptr, Qt::NoModifier);
        QCOMPARE(m_ctrl->focusPath().size(), 1);
        QCOMPARE(m_ctrl->focusPath()[0], m_id.vptr);
        // Clicking a top-level row clears focus back to the root crumb.
        const int leafLn = lineOf(m_id.leaf);
        QVERIFY(leafLn >= 0);
        m_ctrl->handleNodeClick(m_editor, leafLn, m_id.leaf, Qt::NoModifier);
        QVERIFY(m_ctrl->focusPath().isEmpty());
    }

    void testDottedCrumbLabels() {
        // Selecting an expanded pointer renders "<containingClass>.<field>" for
        // the source crumb and the bare class for the current one.
        expand(m_id.vptr);
        m_ctrl->handleNodeClick(m_editor, lineOf(m_id.vptr), m_id.vptr, Qt::NoModifier);
        QApplication::processEvents();
        const QStringList seg = m_editor->breadcrumbBar()->segments();
        QVERIFY(seg.contains(QStringLiteral("RcxEditor.vptr")));   // class.field source
        QVERIFY(seg.contains(QStringLiteral("QWidgetPrivate")));   // current class
        QVERIFY(!seg.contains(QStringLiteral("vptr")));            // no bare field crumb
    }

    void testClearSelectionResetsFocus() {
        expand(m_id.vptr);
        m_ctrl->handleNodeClick(m_editor, lineOf(m_id.vptr), m_id.vptr, Qt::NoModifier);
        QCOMPARE(m_ctrl->focusPath().size(), 1);
        m_ctrl->clearSelection();
        QVERIFY(m_ctrl->focusPath().isEmpty());
    }

    void testCollapseToCrumbTruncatesAndCollapses() {
        expand(m_id.vptr);
        expand(m_id.parent);
        m_ctrl->handleNodeClick(m_editor, lineOf(m_id.parent), m_id.parent, Qt::NoModifier);
        QCOMPARE(m_ctrl->focusPath().size(), 2);

        m_ctrl->collapseToFocus(1);   // collapse below QWidgetPrivate (parent)
        QCOMPARE(m_ctrl->focusPath().size(), 1);
        QCOMPARE(m_ctrl->focusPath()[0], m_id.vptr);
        QVERIFY(collapsed(m_id.parent));
        QVERIFY(!collapsed(m_id.vptr));

        m_ctrl->collapseToFocus(0);   // collapse vptr; back to bare root
        QVERIFY(m_ctrl->focusPath().isEmpty());
        QVERIFY(collapsed(m_id.vptr));
    }

    void testMultiLevelDottedLabels() {
        expand(m_id.vptr);
        expand(m_id.parent);
        m_ctrl->handleNodeClick(m_editor, lineOf(m_id.parent), m_id.parent, Qt::NoModifier);
        QApplication::processEvents();
        const QStringList seg = m_editor->breadcrumbBar()->segments();
        QVERIFY(seg.contains(QStringLiteral("RcxEditor.vptr")));        // depth-0 class.field
        QVERIFY(seg.contains(QStringLiteral("QWidgetPrivate.parent"))); // depth-1 class.field
        QVERIFY(seg.contains(QStringLiteral("QWidget")));               // current class
    }

    void testCollapseToFocusOutOfRangeSafe() {
        expand(m_id.vptr);
        m_ctrl->handleNodeClick(m_editor, lineOf(m_id.vptr), m_id.vptr, Qt::NoModifier);
        QCOMPARE(m_ctrl->focusPath().size(), 1);
        m_ctrl->collapseToFocus(-1);   // negative → no-op
        QCOMPARE(m_ctrl->focusPath().size(), 1);
        m_ctrl->collapseToFocus(99);   // past the end → no crash, no collapse
        QCOMPARE(m_ctrl->focusPath().size(), 1);
        QVERIFY(!collapsed(m_id.vptr));
    }

    void testCyclicRefIdSeenGuard() {
        // A.pa → B, B.pb → A (a refId cycle), viewed from an UNRELATED root C
        // so no chain reaches the view root. focusChainTo's seen-guard must
        // break the cycle and return empty rather than spin.
        m_doc->tree.nodes.clear();
        m_doc->tree.invalidateIdCache();
        auto addCls = [&](const char* t) -> uint64_t {
            Node n; n.kind = NodeKind::Struct; n.structTypeName = t; n.parentId = 0;
            n.collapsed = false; return m_doc->tree.nodes[m_doc->tree.addNode(n)].id;
        };
        auto addPtr = [&](uint64_t parent, const char* nm, uint64_t ref) -> uint64_t {
            Node n; n.kind = NodeKind::Pointer64; n.name = nm; n.parentId = parent;
            n.refId = ref; n.collapsed = false;
            return m_doc->tree.nodes[m_doc->tree.addNode(n)].id;
        };
        uint64_t a = addCls("A"), b = addCls("B"), c = addCls("C");
        uint64_t pa = addPtr(a, "pa", b);
        uint64_t pb = addPtr(b, "pb", a);
        m_ctrl->setViewRootId(c);          // unreachable from the A↔B cycle
        QVector<uint64_t> chain = m_ctrl->focusChainToNode(pb);
        QVERIFY(chain.isEmpty());          // seen-guard fired, no hang
        // Sanity: from the proper root A, the chain is bounded and correct.
        m_ctrl->setViewRootId(a);
        QVector<uint64_t> ok = m_ctrl->focusChainToNode(pb);
        QCOMPARE(ok.size(), 2);
        QCOMPARE(ok[0], pa);
        QCOMPARE(ok[1], pb);
    }

    void testReconcileTrimsFoldCollapsedFocus() {
        expand(m_id.vptr);
        expand(m_id.parent);
        m_ctrl->handleNodeClick(m_editor, lineOf(m_id.parent), m_id.parent, Qt::NoModifier);
        QCOMPARE(m_ctrl->focusPath().size(), 2);
        // Fold-collapse vptr behind the breadcrumb's back; refresh reconciles.
        m_doc->tree.nodes[idx(m_id.vptr)].collapsed = true;
        m_ctrl->refresh();
        QVERIFY(m_ctrl->focusPath().isEmpty());
    }

    // ── Embedded structs are drill frames of their own ──

    void testEmbeddedStructCrumbLabel() {
        // RcxEditor.stats is an EMBEDDED (non-pointer) struct expanded in
        // place. It is its own drill hop (drillTargetId(n) == n.id), so the
        // deepest crumb must name the embedded class — not fall through
        // refId == 0 to the first root class ("RcxEditor" twice).
        Node st; st.kind = NodeKind::Struct; st.name = "stats";
        st.structTypeName = "Stats"; st.parentId = m_id.editor; st.offset = 24;
        st.collapsed = false;
        const uint64_t statsId = m_doc->tree.nodes[m_doc->tree.addNode(st)].id;
        Node hp; hp.kind = NodeKind::UInt32; hp.name = "hp"; hp.parentId = statsId; hp.offset = 0;
        const uint64_t hpId = m_doc->tree.nodes[m_doc->tree.addNode(hp)].id;
        m_ctrl->refresh();

        const int ln = lineOf(hpId);
        QVERIFY(ln >= 0);
        m_ctrl->handleNodeClick(m_editor, ln, hpId, Qt::NoModifier);
        QCOMPARE(m_ctrl->focusPath().size(), 1);
        QCOMPARE(m_ctrl->focusPath()[0], statsId);
        QApplication::processEvents();
        const QStringList seg = m_editor->breadcrumbBar()->segments();
        QVERIFY(seg.contains(QStringLiteral("RcxEditor.stats")));
        QVERIFY(seg.contains(QStringLiteral("Stats")));
        QVERIFY(!seg.contains(QStringLiteral("RcxEditor")));   // not the root twice

        // The path survives reconcileFocusPath on the next refresh (the
        // container check and the label must agree on what a frame is).
        m_ctrl->refresh();
        QCOMPARE(m_ctrl->focusPath().size(), 1);
        QCOMPARE(m_ctrl->focusPath()[0], statsId);
        // Fold-collapsing the embedded struct trims it like a pointer.
        m_doc->tree.nodes[idx(statsId)].collapsed = true;
        m_ctrl->refresh();
        QVERIFY(m_ctrl->focusPath().isEmpty());
    }

    // ── rebaseTo: the one base-address mutation ──

    void testNavigateToFormulaIsUndoable() {
        // Seed a formula base so undo has both fields to restore.
        QVERIFY(m_ctrl->rebaseTo(QStringLiteral("0x20+0x4")));
        QCOMPARE(m_doc->undoStack.count(), 1);
        QCOMPARE(m_doc->tree.baseAddress, 0x24ULL);
        QCOMPARE(m_doc->tree.baseAddressFormula, QStringLiteral("0x20+0x4"));

        QVERIFY(m_ctrl->navigateToFormula(QStringLiteral("0x40")));
        QCOMPARE(m_doc->undoStack.count(), 2);   // exactly one command
        QCOMPARE(m_doc->tree.baseAddress, 0x40ULL);
        QVERIFY(m_doc->tree.baseAddressFormula.isEmpty());   // bare literal

        m_doc->undoStack.undo();
        QCOMPARE(m_doc->tree.baseAddress, 0x24ULL);
        QCOMPARE(m_doc->tree.baseAddressFormula, QStringLiteral("0x20+0x4"));
        m_doc->undoStack.redo();
        QCOMPARE(m_doc->tree.baseAddress, 0x40ULL);
        QVERIFY(m_doc->tree.baseAddressFormula.isEmpty());
    }

    void testRebaseToInvalidIsRefused() {
        QSignalSpy hint(m_ctrl, &RcxController::statusHint);
        const uint64_t before = m_doc->tree.baseAddress;
        QString err;
        QVERIFY(!m_ctrl->rebaseTo(QStringLiteral("[0x100"), &err));   // unclosed deref
        QVERIFY(!err.isEmpty());
        QCOMPARE(m_doc->undoStack.count(), 0);
        QCOMPARE(m_doc->tree.baseAddress, before);
        QVERIFY(!hint.isEmpty());
        QVERIFY(hint.last().at(0).toString().startsWith(QStringLiteral("Base: ")));
        // Empty / whitespace is refused the same way, not treated as 0.
        QVERIFY(!m_ctrl->rebaseTo(QStringLiteral("   "), &err));
        QCOMPARE(m_doc->undoStack.count(), 0);
    }

    void testRebaseToLiteralClearsFormulaExpressionKeepsIt() {
        QVERIFY(m_ctrl->rebaseTo(QStringLiteral("0x10*2")));
        QCOMPARE(m_doc->tree.baseAddress, 0x20ULL);
        QCOMPARE(m_doc->tree.baseAddressFormula, QStringLiteral("0x10*2"));

        QVERIFY(m_ctrl->rebaseTo(QStringLiteral(" 0x30 ")));
        QCOMPARE(m_doc->tree.baseAddress, 0x30ULL);
        QVERIFY(m_doc->tree.baseAddressFormula.isEmpty());

        // WinDbg backticks are stripped before the parser sees the text.
        QVERIFY(m_ctrl->rebaseTo(QStringLiteral("0x7ff6`0000")));
        QCOMPARE(m_doc->tree.baseAddress, 0x7ff60000ULL);
        QVERIFY(m_doc->tree.baseAddressFormula.isEmpty());

        // Same base + same formula → nothing pushed (no no-op undo entries).
        const int n = m_doc->undoStack.count();
        QVERIFY(m_ctrl->rebaseTo(QStringLiteral("0x7FF60000")));
        QCOMPARE(m_doc->undoStack.count(), n);
    }

    void testRebaseToKeepsFocusPath() {
        expand(m_id.vptr);
        expand(m_id.parent);
        m_ctrl->handleNodeClick(m_editor, lineOf(m_id.parent), m_id.parent, Qt::NoModifier);
        const QVector<uint64_t> before = m_ctrl->focusPath();
        QCOMPARE(before.size(), 2);
        QVERIFY(m_ctrl->rebaseTo(QStringLiteral("0x10")));
        QCOMPARE(m_ctrl->focusPath(), before);   // the expanded chain is still on screen
        m_doc->undoStack.undo();
        QCOMPARE(m_ctrl->focusPath(), before);
    }

    // ── makeAddressCallbacks: one callback set for every caller ──

    void testCallbackFactoryHasKernelPagingWhenProviderDoes() {
        // No source: nothing wired; the parser reports module / symbol /
        // pointer terms with its own "unavailable" error.
        const AddressParserCallbacks none = makeAddressCallbacks(nullptr, 8);
        QVERIFY(!none.resolveModule && !none.readPointer && !none.resolveIdentifier);
        QVERIFY(!none.vtop && !none.cr3 && !none.physRead);

        // A plain provider: the memory / symbol trio, no paging.
        BufferProvider plain(QByteArray(16, '\0'));
        const AddressParserCallbacks p = makeAddressCallbacks(&plain, 8);
        QVERIFY(p.resolveModule && p.readPointer && p.resolveIdentifier);
        QVERIFY(!p.vtop && !p.cr3 && !p.physRead);

        // readPointer reads at the document's pointer size through Provider::read.
        BufferProvider bytes(QByteArray::fromHex("efbeadde00000000"));
        const AddressParserCallbacks q = makeAddressCallbacks(&bytes, 4);
        bool ok = false;
        QCOMPARE(q.readPointer(0, &ok), 0xDEADBEEFULL);
        QVERIFY(ok);

        // A provider that reports kernel paging gets vtop / cr3 / physRead
        // too — the branch the live evaluator and navigateToFormula lacked.
        struct PagingProvider : BufferProvider {
            using BufferProvider::BufferProvider;
            bool hasKernelPaging() const override { return true; }
            uint64_t getCr3() const override { return 0x1000; }
        } paging(QByteArray(16, '\0'));
        const AddressParserCallbacks k = makeAddressCallbacks(&paging, 8);
        QVERIFY(k.vtop && k.cr3 && k.physRead);
        QCOMPARE(k.cr3(0, &ok), 0x1000ULL);
        QVERIFY(ok);
    }

    // ── Crumb payload: class id, entry hop, address, keyword ──

    void testCrumbAddressesFollowPointerValues() {
        // Real pointer values in the buffer: the address of the class at
        // crumb i+1 is what compose dereferenced for hop i (LineMeta::ptrBase
        // on the first row rendered inside it). QWidget gets one field so its
        // expansion renders a row to carry the base.
        m_doc->provider = std::make_unique<BufferProvider>(chainBytes());
        Node fl; fl.kind = NodeKind::UInt32; fl.name = "flags"; fl.parentId = m_id.widget; fl.offset = 0;
        m_doc->tree.addNode(fl);
        drillTwoLevels();
        QVector<Crumb> c = crumbs();
        QCOMPARE(c.size(), 3);
        QCOMPARE(c[0].address, 0x00ULL);
        QCOMPARE(c[1].address, 0x20ULL);
        QCOMPARE(c[2].address, 0x30ULL);

        // Null / unreadable pointers: the frame is unknown, not "0x0 + offset".
        m_doc->provider = std::make_unique<BufferProvider>(QByteArray(64, '\0'));
        m_ctrl->refresh();
        c = crumbs();
        QCOMPARE(c.size(), 3);                         // the trail itself survives
        QCOMPARE(c[0].address, 0x00ULL);
        QCOMPARE(c[1].address, 0x00ULL);
        QCOMPARE(c[2].address, 0x00ULL);
    }

    void testCrumbAddressOfEmbeddedStructIsItsRow() {
        // An embedded struct is its own hop and sits at base + its offset —
        // compose's ptrBase does not change for it (it is not a pointer), so
        // the crumb takes the row's own absolute address instead.
        Node st; st.kind = NodeKind::Struct; st.name = "stats"; st.structTypeName = "Stats";
        st.parentId = m_id.editor; st.offset = 24; st.collapsed = false;
        const uint64_t statsId = m_doc->tree.nodes[m_doc->tree.addNode(st)].id;
        Node hp; hp.kind = NodeKind::UInt32; hp.name = "hp"; hp.parentId = statsId; hp.offset = 0;
        const uint64_t hpId = m_doc->tree.nodes[m_doc->tree.addNode(hp)].id;
        m_ctrl->refresh();
        m_ctrl->handleNodeClick(m_editor, lineOf(hpId), hpId, Qt::NoModifier);
        QCOMPARE(m_ctrl->focusPath(), (QVector<uint64_t>{ statsId }));
        QVector<Crumb> c = crumbs();
        QCOMPARE(c.size(), 2);
        QCOMPARE(c[0].address, 0ULL);
        QCOMPARE(c[1].address, 24ULL);
        QCOMPARE(c[1].classId, statsId);
        QCOMPARE(c[1].pointerId, statsId);              // its own hop
        QCOMPARE(c[1].label, QStringLiteral("Stats"));
        // A rebase moves every crumb with it (the focus path is kept).
        QVERIFY(m_ctrl->rebaseTo(QStringLiteral("0x100")));
        c = crumbs();
        QCOMPARE(c.size(), 2);
        QCOMPARE(c[0].address, 0x100ULL);
        QCOMPARE(c[1].address, 0x118ULL);
    }

    void testCrumbPointerIdsAndClassIds() {
        drillTwoLevels();
        const QVector<uint64_t>& fp = m_ctrl->focusPath();
        const QVector<Crumb> c = crumbs();
        QCOMPARE(c.size(), 3);
        // pointerId = the hop crumb i was entered through = focusPath[i-1].
        QCOMPARE(c[0].pointerId, 0ULL);
        QCOMPARE(c[1].pointerId, fp[0]);
        QCOMPARE(c[2].pointerId, fp[1]);
        // classId = the container at each depth.
        QCOMPARE(c[0].classId, m_id.editor);
        QCOMPARE(c[1].classId, m_id.priv);
        QCOMPARE(c[2].classId, m_id.widget);
        for (int i = 0; i < 3; ++i) {
            QCOMPARE(c[i].rootId, uint64_t(i));
            QCOMPARE(c[i].keyword, QStringLiteral("struct"));   // resolved default
        }
        // The keyword follows the class node's resolved keyword.
        m_doc->tree.nodes[idx(m_id.widget)].classKeyword = QStringLiteral("class");
        m_doc->tree.nodes[idx(m_id.priv)].classKeyword = QStringLiteral("union");
        m_ctrl->refresh();
        QCOMPARE(crumbs()[2].keyword, QStringLiteral("class"));
        QCOMPARE(crumbs()[1].keyword, QStringLiteral("union"));
        QCOMPARE(crumbs()[0].keyword, QStringLiteral("struct"));
    }

    void testProductionCrumbsHaveNoConnector() {
        drillTwoLevels();
        QVector<Crumb> c = crumbs();
        QCOMPARE(c.size(), 3);
        for (int i = 0; i < c.size(); ++i) {
            QVERIFY(!c[i].label.isEmpty());
            QVERIFY(c[i].classId != 0);
            // Ancestors are dotted ("Class.field"); the deepest is bare.
            QCOMPARE(c[i].label.contains(QLatin1Char('.')), i < c.size() - 1);
        }
        // The widget echoes exactly the labels with one separator between
        // them — no italic field connector is ever rendered.
        QCOMPARE(m_editor->breadcrumbBar()->segments(),
                 (QStringList{ QStringLiteral("RcxEditor.vptr"), QStringLiteral("›"),
                               QStringLiteral("QWidgetPrivate.parent"), QStringLiteral("›"),
                               QStringLiteral("QWidget") }));
        // Lone root: one undotted crumb naming the view root.
        m_ctrl->clearSelection();
        c = crumbs();
        QCOMPARE(c.size(), 1);
        QVERIFY(!c[0].label.contains(QLatin1Char('.')));
        QCOMPARE(c[0].classId, m_id.editor);
        QCOMPARE(c[0].pointerId, 0ULL);
    }

    // ── rebaseTo announces itself: documentChanged once, refresh not thrice ──

    void testGotoRebaseEmitsDocumentChanged() {
        // The bookmarks dock, the doc-tab source icon and the MCP bridge all
        // listen to documentChanged; a Goto / bookmark / scanner rebase must
        // reach them like the old navigateToFormula did.
        QSignalSpy changed(m_doc, &RcxDocument::documentChanged);
        QVERIFY(m_ctrl->navigateToFormula(QStringLiteral("0x40")));
        QCOMPARE(changed.count(), 1);
        // Same base again: nothing changed, nothing announced.
        QVERIFY(m_ctrl->navigateToFormula(QStringLiteral("0x40")));
        QCOMPARE(changed.count(), 1);
        // A refused formula announces nothing.
        QVERIFY(!m_ctrl->navigateToFormula(QStringLiteral("[0x1")));
        QCOMPARE(changed.count(), 1);
        // Undo / redo go through the undo stack, which never emits it.
        m_doc->undoStack.undo();
        m_doc->undoStack.redo();
        QCOMPARE(changed.count(), 1);
    }

    void testInlineBaseCommitAddsNoExtraRefresh() {
        // refresh() always reaches applyDocument, which always emits
        // documentApplied, so the spy counts recomposes. The inline command
        // row edit must cost exactly what a direct rebaseTo costs — it used
        // to add a third refresh after rebaseTo's own — and a refused edit
        // exactly one (the canonical text restored over the typed text).
        // Only RELATIVE counts are meaningful: a refresh whose command-row
        // text changed emits documentApplied twice (applyDocument, then the
        // mirror re-emit in setCommandRowText), so a direct rebase reads 3
        // for its two refreshes (command apply + documentChanged).
        QSignalSpy applied(m_editor, &RcxEditor::documentApplied);
        QVERIFY(m_ctrl->rebaseTo(QStringLiteral("0x40")));
        const int direct = applied.count();
        QVERIFY(direct >= 1);

        applied.clear();
        emit m_editor->inlineEditCommitted(-1, 0, EditTarget::BaseAddress, QStringLiteral("0x50"), 0);
        QCOMPARE(applied.count(), direct);
        QCOMPARE(m_doc->tree.baseAddress, 0x50ULL);

        applied.clear();
        emit m_editor->inlineEditCommitted(-1, 0, EditTarget::BaseAddress, QStringLiteral("[0x1"), 0);
        QCOMPARE(applied.count(), 1);
        QCOMPARE(m_doc->tree.baseAddress, 0x50ULL);

        // Same value re-committed: no command, one refresh (canonical text).
        applied.clear();
        const int n = m_doc->undoStack.count();
        emit m_editor->inlineEditCommitted(-1, 0, EditTarget::BaseAddress, QStringLiteral("0x50"), 0);
        QCOMPARE(applied.count(), 1);
        QCOMPARE(m_doc->undoStack.count(), n);
    }

    // ── BreadcrumbBar widget ──
    // Fed the production dotted shape (see crumb()/twoLevel() above): the
    // field is part of the ancestor's label; there is no connector segment.

    void testBarAlwaysVisibleForSingleClass() {
        BreadcrumbBar bar;
        bar.setCrumbs({ crumb(QStringLiteral("RcxEditor"), 0) });
        QVERIFY(bar.barVisible());
    }

    void testBarRendersTrailAndSeparators() {
        BreadcrumbBar bar;
        bar.setCrumbs(twoLevel());
        QVERIFY(bar.barVisible());
        // Exactly the labels with one separator between them.
        QCOMPARE(bar.segments(), (QStringList{ QStringLiteral("RcxEditor.vptr"),
                                               QStringLiteral("›"),
                                               QStringLiteral("QWidgetPrivate") }));
        QCOMPARE(bar.findChildren<QToolButton*>().size(), 2);
        const auto labels = bar.findChildren<QLabel*>();
        QCOMPARE(labels.size(), 1);                     // the one '›'
        QCOMPARE(labels[0]->text(), QStringLiteral("›"));
    }

    void testBarClickFiresCrumbIndex() {
        BreadcrumbBar bar;
        int clicked = -1;
        bar.setOnCrumb([&](uint64_t i) { clicked = (int)i; });
        bar.setCrumbs(twoLevel());
        QToolButton* rootBtn = nullptr;
        QToolButton* deepBtn = nullptr;
        for (auto* b : bar.findChildren<QToolButton*>()) {
            if (b->text() == QStringLiteral("RcxEditor.vptr")) rootBtn = b;
            if (b->text() == QStringLiteral("QWidgetPrivate")) deepBtn = b;
        }
        QVERIFY(rootBtn != nullptr && deepBtn != nullptr);
        rootBtn->click();
        QCOMPARE(clicked, 0);
        deepBtn->click();                               // deepest reports its index too
        QCOMPARE(clicked, 1);
    }

    void testBarHasNoBackButton() {
        BreadcrumbBar bar;
        bar.setCrumbs({ crumb(QStringLiteral("A.f"), 0, 1), crumb(QStringLiteral("B"), 1, 2) });
        for (auto* b : bar.findChildren<QToolButton*>())
            QVERIFY(b->text() != QStringLiteral("↩"));
        // ...and no connector label either: every QLabel is a separator.
        for (auto* l : bar.findChildren<QLabel*>())
            QCOMPARE(l->text(), QStringLiteral("›"));
    }

    // ── Tone ladder (the band is paper, not a third chrome strip) ──
    // Depth 1 only repeats the class the doc tab and the command row already
    // name, so the lone crumb stays secondary: textDim, regular weight.
    void testDepth1CrumbIsDimAndRegular() {
        BreadcrumbBar bar;
        bar.applyTheme(ThemeManager::instance().current());
        bar.setCrumbs({ crumb(QStringLiteral("RcxEditor"), 0) });
        const auto& t = ThemeManager::instance().current();
        QToolButton* only = nullptr;
        for (auto* b : bar.findChildren<QToolButton*>()) only = b;
        QVERIFY(only != nullptr);
        const QString qss = only->styleSheet();
        QVERIFY(qss.contains(t.textDim.name()));
        QVERIFY(!qss.contains(QStringLiteral("font-weight")));
    }

    // From depth 2 the trail is navigation: ancestors stay textDim, the
    // deepest crumb steps up to text + DemiBold (the ONE weight step here).
    void testDeepestCrumbIsTextDemiBold() {
        BreadcrumbBar bar;
        bar.applyTheme(ThemeManager::instance().current());
        bar.setCrumbs(twoLevel());
        const auto& t = ThemeManager::instance().current();
        QString rootQss, deepQss;
        for (auto* b : bar.findChildren<QToolButton*>()) {
            if (b->text() == QStringLiteral("RcxEditor.vptr")) rootQss = b->styleSheet();
            if (b->text() == QStringLiteral("QWidgetPrivate")) deepQss = b->styleSheet();
        }
        QVERIFY(!rootQss.isEmpty() && !deepQss.isEmpty());
        QVERIFY(rootQss.contains(t.textDim.name()));
        QVERIFY(!rootQss.contains(QStringLiteral("font-weight")));
        QVERIFY(deepQss.contains(t.text.name()));
        QVERIFY(deepQss.contains(QStringLiteral("font-weight:600")));
        // Hover is a link cue (text + underline), not the accent — purple is
        // spent on current/selected only.
        QVERIFY(deepQss.contains(QStringLiteral("text-decoration:underline")));
        QVERIFY(!deepQss.contains(t.indHoverSpan.name()));
    }

    // The bottom seam is painted device-exact (paintEvent), because a QSS
    // "1px" border is TWO device rows at 125 % DPI next to the editor's
    // device-exact frame.
    void testBandHasNoQssBorderAndIsPaper() {
        BreadcrumbBar bar;
        const auto& t = ThemeManager::instance().current();
        bar.applyTheme(t);
        const QString qss = bar.styleSheet();
        QVERIFY(!qss.contains(QStringLiteral("border")));
        QVERIFY(qss.contains(rcx::editorPaperColor(t).name()));
        QVERIFY(!qss.contains(rcx::menuBarColor(t).name()));
    }

    void testSetCrumbsSkipsIdenticalTrail() {
        // The controller pushes the trail every refresh tick; an equal
        // trail must leave the existing buttons alone (no teardown/rebuild).
        BreadcrumbBar bar;
        const QVector<Crumb> trail = twoLevel();
        bar.setCrumbs(trail);
        const auto before = bar.findChildren<QToolButton*>();
        QCOMPARE(before.size(), 2);
        bar.setCrumbs(trail);
        const auto after = bar.findChildren<QToolButton*>();
        QCOMPARE(after, before);            // same widget objects
        bar.setCrumbs({ crumb(QStringLiteral("RcxEditor"), 0) });
        // The old buttons are deleteLater'd; plain processEvents() skips
        // deferred deletes outside an event loop, so flush them explicitly.
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCOMPARE(bar.findChildren<QToolButton*>().size(), 1);
    }

    void testBarCollapsesDeepTrailToEllipsis() {
        BreadcrumbBar bar;
        QVector<Crumb> trail;
        for (int i = 0; i < 6; ++i)
            trail.push_back(crumb(i < 5 ? QStringLiteral("C%1.f%1").arg(i) : QStringLiteral("C5"),
                                  uint64_t(i), uint64_t(i + 1)));
        bar.setCrumbs(trail);
        const QStringList seg = bar.segments();
        QVERIFY(seg.contains(QStringLiteral("…")));
        QVERIFY(seg.contains(QStringLiteral("C0.f0")));
        QVERIFY(seg.contains(QStringLiteral("C5")));
        QVERIFY(!seg.contains(QStringLiteral("C2.f2")));
        // The gap's tooltip lists the hidden crumbs, already dotted, joined
        // by the separator only.
        QLabel* ell = nullptr;
        for (auto* l : bar.findChildren<QLabel*>())
            if (l->text() == QStringLiteral("…")) ell = l;
        QVERIFY(ell != nullptr);
        QCOMPARE(ell->toolTip(), QStringLiteral("C1.f1 › C2.f2"));
    }
};

QTEST_MAIN(TestBreadcrumb)
#include "test_breadcrumb.moc"
