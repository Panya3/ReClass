// Drill-down breadcrumb (v3 — CLICK-driven focus path): the breadcrumb reflects
// where the selection sits in the inline-expanded tree. Selecting a typed
// pointer (or any row inside its expansion) adds it to the breadcrumb; there is
// no follow-arrow affordance. Crumb click = collapse below + scroll. Exercises
// focusChainToNode / handleNodeClick / collapseToFocus and the BreadcrumbBar.

#include <QtTest/QTest>
#include <QtTest/QSignalSpy>
#include <QApplication>
#include <QSettings>
#include <QSplitter>
#include <QToolButton>

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

    // ── BreadcrumbBar widget ──

    void testBarAlwaysVisibleForSingleClass() {
        BreadcrumbBar bar;
        bar.setCrumbs({ { QStringLiteral("RcxEditor"), 0, false } });
        QVERIFY(bar.barVisible());
    }

    void testBarRendersTrailAndSeparators() {
        BreadcrumbBar bar;
        bar.setCrumbs({
            { QStringLiteral("RcxEditor"),      0, false },
            { QStringLiteral("vptr"),           0, true  },
            { QStringLiteral("QWidgetPrivate"), 1, false },
        });
        QVERIFY(bar.barVisible());
        const QStringList seg = bar.segments();
        QVERIFY(seg.contains(QStringLiteral("RcxEditor")));
        QVERIFY(seg.contains(QStringLiteral("vptr")));
        QVERIFY(seg.contains(QStringLiteral("QWidgetPrivate")));
        QVERIFY(seg.contains(QStringLiteral("›")));
    }

    void testBarClickFiresCrumbIndex() {
        BreadcrumbBar bar;
        int clicked = -1;
        bar.setOnCrumb([&](uint64_t i) { clicked = (int)i; });
        bar.setCrumbs({
            { QStringLiteral("RcxEditor"),      0, false },
            { QStringLiteral("vptr"),           0, true  },
            { QStringLiteral("QWidgetPrivate"), 1, false },
        });
        QToolButton* rootBtn = nullptr;
        for (auto* b : bar.findChildren<QToolButton*>())
            if (b->text() == QStringLiteral("RcxEditor")) rootBtn = b;
        QVERIFY(rootBtn != nullptr);
        rootBtn->click();
        QCOMPARE(clicked, 0);
    }

    void testBarHasNoBackButton() {
        BreadcrumbBar bar;
        bar.setCrumbs({
            { QStringLiteral("A"), 0, false },
            { QStringLiteral("f"), 0, true  },
            { QStringLiteral("B"), 1, false },
        });
        for (auto* b : bar.findChildren<QToolButton*>())
            QVERIFY(b->text() != QStringLiteral("↩"));
    }

    // ── Tone ladder (the band is paper, not a third chrome strip) ──
    // Depth 1 only repeats the class the doc tab and the command row already
    // name, so the lone crumb stays secondary: textDim, regular weight.
    void testDepth1CrumbIsDimAndRegular() {
        BreadcrumbBar bar;
        bar.applyTheme(ThemeManager::instance().current());
        bar.setCrumbs({ { QStringLiteral("RcxEditor"), 0, false } });
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
        bar.setCrumbs({
            { QStringLiteral("RcxEditor"),      0, false },
            { QStringLiteral("vptr"),           0, true  },
            { QStringLiteral("QWidgetPrivate"), 1, false },
        });
        const auto& t = ThemeManager::instance().current();
        QString rootQss, deepQss;
        for (auto* b : bar.findChildren<QToolButton*>()) {
            if (b->text() == QStringLiteral("RcxEditor"))      rootQss = b->styleSheet();
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
        const QVector<Crumb> trail = {
            { QStringLiteral("RcxEditor.vptr"), 0, false },
            { QStringLiteral("QWidgetPrivate"), 1, false },
        };
        bar.setCrumbs(trail);
        const auto before = bar.findChildren<QToolButton*>();
        QCOMPARE(before.size(), 2);
        bar.setCrumbs(trail);
        const auto after = bar.findChildren<QToolButton*>();
        QCOMPARE(after, before);            // same widget objects
        bar.setCrumbs({ { QStringLiteral("RcxEditor"), 0, false } });
        // The old buttons are deleteLater'd; plain processEvents() skips
        // deferred deletes outside an event loop, so flush them explicitly.
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        QCOMPARE(bar.findChildren<QToolButton*>().size(), 1);
    }

    void testBarCollapsesDeepTrailToEllipsis() {
        BreadcrumbBar bar;
        QVector<Crumb> crumbs;
        for (int i = 0; i < 6; ++i) {
            crumbs.push_back({ QStringLiteral("C%1").arg(i), uint64_t(i), false });
            if (i < 5) crumbs.push_back({ QStringLiteral("f%1").arg(i), 0, true });
        }
        bar.setCrumbs(crumbs);
        const QStringList seg = bar.segments();
        QVERIFY(seg.contains(QStringLiteral("…")));
        QVERIFY(seg.contains(QStringLiteral("C0")));
        QVERIFY(seg.contains(QStringLiteral("C5")));
        QVERIFY(!seg.contains(QStringLiteral("C2")));
    }
};

QTEST_MAIN(TestBreadcrumb)
#include "test_breadcrumb.moc"
