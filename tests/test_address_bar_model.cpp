// Address bar model + navigation history — the pure half of the address bar.
// Builds NodeTrees by hand (no controller, no editor, no widget) and pins:
//   siblingFieldsOf   offset order, drillTargetId filter, current flag, refId==0
//                     pointers excluded, union members keep declaration order
//   rootClassEntries  one-to-one with rootClassNames (ids, labels, order)
//   trailPathText     round-trips through resolveDrillPath across an embedded
//                     struct hop; unknown segments are named in the error
//   NavHistory        head de-dup, forward truncation, cap, stale-entry skip
//   AddressBarState   operator== sees every field

#include <QtTest/QTest>

#include "core.h"
#include "nav_history.h"
#include "widgets/address_bar_model.h"

using namespace rcx;

namespace {

// RcxEditor.vptr → QWidgetPrivate.parent → QWidget, plus an embedded struct
// hop: QWidget.geom (a Struct with parentId != 0, refId 0) with a plain
// field inside. dptr is a pointer without a refId (not drillable); leaf is a
// plain value.
struct Ids { uint64_t editor, priv, widget, vptr, dptr, parent, leaf, geom, geomX; };

Ids buildChain(NodeTree& tree) {
    tree.baseAddress = 0;
    Ids id{};
    auto addRoot = [&](const char* type, const char* keyword = nullptr) -> uint64_t {
        Node n; n.kind = NodeKind::Struct; n.structTypeName = type;
        n.parentId = 0; n.offset = 0; n.collapsed = false;
        if (keyword) n.classKeyword = keyword;
        return tree.nodes[tree.addNode(n)].id;
    };
    auto addPtr = [&](uint64_t parent, const char* name, uint64_t refId, int off,
                      bool collapsed = true) -> uint64_t {
        Node n; n.kind = NodeKind::Pointer64; n.name = name;
        n.parentId = parent; n.offset = off; n.refId = refId; n.collapsed = collapsed;
        return tree.nodes[tree.addNode(n)].id;
    };
    id.editor = addRoot("RcxEditor", "class");
    id.priv   = addRoot("QWidgetPrivate");
    id.widget = addRoot("QWidget");
    // Insert vptr AFTER dptr and leaf so tree-insertion order != offset order.
    id.dptr   = addPtr(id.editor, "dptr", 0, 8);          // no refId → not drillable
    Node lf; lf.kind = NodeKind::UInt32; lf.name = "leaf"; lf.parentId = id.editor; lf.offset = 16;
    id.leaf   = tree.nodes[tree.addNode(lf)].id;
    id.vptr   = addPtr(id.editor, "vptr", id.priv, 0, /*collapsed=*/false);
    id.parent = addPtr(id.priv, "parent", id.widget, 0);
    Node g; g.kind = NodeKind::Struct; g.name = "geom"; g.parentId = id.widget; g.offset = 0x10;
    g.collapsed = true;
    id.geom = tree.nodes[tree.addNode(g)].id;
    Node gx; gx.kind = NodeKind::Int32; gx.name = "x"; gx.parentId = id.geom; gx.offset = 0;
    id.geomX = tree.nodes[tree.addNode(gx)].id;
    return id;
}

NavEntry place(uint64_t root, QVector<uint64_t> path = {}, uint64_t base = 0) {
    NavEntry e;
    e.viewRootId = root;
    e.focusPath = std::move(path);
    e.baseAddress = base;
    return e;
}

} // namespace

class TestAddressBarModel : public QObject {
    Q_OBJECT
private slots:

    // ── siblingFieldsOf ──

    void testSiblingsAreOffsetOrderedAndDrillableOnly() {
        NodeTree tree; Ids id = buildChain(tree);
        // Add a second drillable pointer at a higher offset, inserted before
        // vptr in tree order, to prove the sort is by offset not insertion.
        Node n; n.kind = NodeKind::Pointer64; n.name = "zptr"; n.parentId = id.editor;
        n.offset = 0x40; n.refId = id.widget; n.collapsed = true;
        const uint64_t zptr = tree.nodes[tree.addNode(n)].id;
        tree.invalidateIdCache();

        const auto sibs = siblingFieldsOf(tree, id.editor, id.vptr);
        QCOMPARE(sibs.size(), 2);                 // dptr (refId 0) and leaf excluded
        QCOMPARE(sibs[0].id, id.vptr);
        QCOMPARE(sibs[0].field, QStringLiteral("vptr"));
        QCOMPARE(sibs[0].classLabel, QStringLiteral("QWidgetPrivate"));
        QCOMPARE(sibs[0].keyword, QStringLiteral("struct"));
        QVERIFY(sibs[0].expanded);
        QVERIFY(sibs[0].current);
        QCOMPARE(sibs[0].offset, 0);
        QCOMPARE(sibs[1].id, zptr);
        QCOMPARE(sibs[1].classLabel, QStringLiteral("QWidget"));
        QVERIFY(!sibs[1].expanded);
        QVERIFY(!sibs[1].current);
        QCOMPARE(sibs[1].offset, 0x40);
        for (const auto& s : sibs) QVERIFY(s.id != id.dptr && s.id != id.leaf);
    }

    void testSiblingsIncludeEmbeddedStructsAndFlagCurrent() {
        NodeTree tree; Ids id = buildChain(tree);
        const auto sibs = siblingFieldsOf(tree, id.widget, id.geom);
        QCOMPARE(sibs.size(), 1);
        QCOMPARE(sibs[0].id, id.geom);            // embedded struct: its own drill target
        QCOMPARE(sibs[0].classLabel, QStringLiteral("geom"));
        QVERIFY(sibs[0].current);
        // No current hop → nothing flagged.
        QVERIFY(!siblingFieldsOf(tree, id.widget, 0)[0].current);
        // Unknown class → empty, not a crash.
        QVERIFY(siblingFieldsOf(tree, 0xDEAD, 0).isEmpty());
    }

    void testUnionMembersKeepDeclarationOrder() {
        // Every member of a union sits at offset 0; a stable sort must leave
        // them in the order they were declared (what the document shows).
        NodeTree tree;
        Node u; u.kind = NodeKind::Struct; u.structTypeName = "Variant"; u.classKeyword = "union";
        const uint64_t uid = tree.nodes[tree.addNode(u)].id;
        Node a; a.kind = NodeKind::Struct; a.structTypeName = "A"; const uint64_t aid = tree.nodes[tree.addNode(a)].id;
        Node b; b.kind = NodeKind::Struct; b.structTypeName = "B"; const uint64_t bid = tree.nodes[tree.addNode(b)].id;
        Node c; c.kind = NodeKind::Struct; c.structTypeName = "C"; const uint64_t cid = tree.nodes[tree.addNode(c)].id;
        QVector<uint64_t> order;
        for (const char* nm : { "asC", "asA", "asB" }) {
            Node p; p.kind = NodeKind::Pointer64; p.name = nm; p.parentId = uid; p.offset = 0;
            p.refId = (nm[2] == 'A') ? aid : (nm[2] == 'B') ? bid : cid;
            order << tree.nodes[tree.addNode(p)].id;
        }
        const auto sibs = siblingFieldsOf(tree, uid, 0);
        QCOMPARE(sibs.size(), 3);
        for (int i = 0; i < 3; ++i) QCOMPARE(sibs[i].id, order[i]);
        QCOMPARE(sibs[0].classLabel, QStringLiteral("C"));
    }

    // ── rootClassEntries ──

    void testRootEntriesMirrorRootClassNames() {
        NodeTree tree; Ids id = buildChain(tree);
        // A duplicate-labelled root and a nameless one exercise the de-dup
        // and the "Untitled" rule.
        Node dup; dup.kind = NodeKind::Struct; dup.structTypeName = "QWidget"; tree.addNode(dup);
        Node anon; anon.kind = NodeKind::Struct; const uint64_t anonId = tree.nodes[tree.addNode(anon)].id;

        const QStringList names = rootClassNames(tree);
        const auto entries = rootClassEntries(tree);
        QCOMPARE(entries.size(), names.size());
        for (int i = 0; i < names.size(); ++i) {
            QCOMPARE(entries[i].label, names[i]);
            QVERIFY(tree.indexOfId(entries[i].id) >= 0);
            QCOMPARE(tree.nodes[tree.indexOfId(entries[i].id)].parentId, 0ULL);
        }
        QCOMPARE(entries[0].id, id.editor);
        QCOMPARE(entries[0].keyword, QStringLiteral("class"));
        QCOMPARE(entries[2].id, id.widget);          // first QWidget wins the label
        QCOMPARE(entries.last().id, anonId);
        QCOMPARE(entries.last().label, QStringLiteral("Untitled"));
        // Empty tree: no id to carry (rootClassNames' placeholder has none).
        QVERIFY(rootClassEntries(NodeTree{}).isEmpty());
    }

    // ── trailPathText ↔ resolveDrillPath ──

    void testTrailPathRoundTripsThroughEmbeddedStruct() {
        NodeTree tree; Ids id = buildChain(tree);
        const QVector<uint64_t> path{ id.vptr, id.parent, id.geom };
        const QString text = trailPathText(tree, id.editor, path);
        QCOMPARE(text, QStringLiteral("RcxEditor.vptr.parent.geom"));

        uint64_t root = 0; QVector<uint64_t> out; QString err = QStringLiteral("stale");
        QVERIFY(resolveDrillPath(tree, text, &root, &out, &err));
        QCOMPARE(root, id.editor);
        QCOMPARE(out, path);
        QVERIFY(err.isEmpty());

        // Root alone, show-all root (0 → first root struct), and the
        // whitespace / dot tolerance.
        QCOMPARE(trailPathText(tree, id.editor, {}), QStringLiteral("RcxEditor"));
        QCOMPARE(trailPathText(tree, 0, {}), QStringLiteral("RcxEditor"));
        QVERIFY(resolveDrillPath(tree, QStringLiteral("  RcxEditor . vptr .. parent. "), &root, &out, &err));
        QCOMPARE(out, (QVector<uint64_t>{ id.vptr, id.parent }));
        // A root matched by instance name, not just type name.
        Node named; named.kind = NodeKind::Struct; named.name = "inst"; tree.addNode(named);
        tree.invalidateIdCache();
        QVERIFY(resolveDrillPath(tree, QStringLiteral("inst"), &root, &out, &err));
        QVERIFY(out.isEmpty());
    }

    void testResolveDrillPathNamesTheMissingSegment() {
        NodeTree tree; Ids id = buildChain(tree);
        uint64_t root = 7; QVector<uint64_t> out{ 9 }; QString err;
        QVERIFY(!resolveDrillPath(tree, QStringLiteral("RcxEditor.vptr.nope"), &root, &out, &err));
        QVERIFY2(err.contains(QStringLiteral("nope")), qPrintable(err));
        QVERIFY2(err.contains(QStringLiteral("QWidgetPrivate")), qPrintable(err));   // the container it looked in
        QCOMPARE(root, 7ULL);                        // outputs untouched on failure
        QCOMPARE(out, (QVector<uint64_t>{ 9 }));

        QVERIFY(!resolveDrillPath(tree, QStringLiteral("Nada.vptr"), &root, &out, &err));
        QVERIFY2(err.contains(QStringLiteral("Nada")), qPrintable(err));
        // A field that exists but is not drillable is named too, distinctly.
        QVERIFY(!resolveDrillPath(tree, QStringLiteral("RcxEditor.leaf"), &root, &out, &err));
        QVERIFY2(err.contains(QStringLiteral("leaf")), qPrintable(err));
        QVERIFY(!resolveDrillPath(tree, QStringLiteral("RcxEditor.dptr"), &root, &out, &err));
        QVERIFY2(err.contains(QStringLiteral("dptr")), qPrintable(err));
        // Empty / dots only.
        QVERIFY(!resolveDrillPath(tree, QStringLiteral(" . . "), &root, &out, &err));
        QVERIFY(!err.isEmpty());
        Q_UNUSED(id);
    }

    void testNamelessHopUsesTypeNameFallbackBothWays() {
        NodeTree tree; Ids id = buildChain(tree);
        Node p; p.kind = NodeKind::Pointer64; p.parentId = id.widget; p.offset = 0x20; p.refId = id.priv;
        const uint64_t anonPtr = tree.nodes[tree.addNode(p)].id;
        tree.invalidateIdCache();
        const QString text = trailPathText(tree, id.widget, { anonPtr });
        QCOMPARE(text, QStringLiteral("QWidget.") + fmt::typeNameRaw(NodeKind::Pointer64));
        uint64_t root = 0; QVector<uint64_t> out; QString err;
        QVERIFY2(resolveDrillPath(tree, text, &root, &out, &err), qPrintable(err));
        QCOMPARE(out, (QVector<uint64_t>{ anonPtr }));
    }

    // ── NavHistory ──

    void testPushDedupesSamePlaceHead() {
        NavHistory h;
        QVERIFY(!h.canBack() && !h.canForward());
        h.push(place(1));
        NavEntry same = place(1); same.label = "different label"; same.anchorNodeId = 42;
        h.push(same);                                // same place: ignored
        QCOMPARE(h.backEntries().size(), 1);
        h.push(place(1, {5}));                       // deeper: a new place
        QCOMPARE(h.backEntries().size(), 2);
        h.push(place(1, {5}, 0x1000));               // rebased: a new place
        QCOMPARE(h.backEntries().size(), 3);
        NavEntry src = place(1, {5}, 0x1000); src.activeSourceIdx = 2;
        h.push(src);                                 // other source: a new place
        QCOMPARE(h.backEntries().size(), 4);
        NavEntry f = src; f.baseFormula = "<x.exe>";
        h.push(f);
        QCOMPARE(h.backEntries().size(), 5);
    }

    void testBackForwardAndTruncateOnPush() {
        NavHistory h;
        auto any = [](const NavEntry&) { return true; };
        h.push(place(1));
        h.push(place(2));
        QVERIFY(h.canBack());
        auto b = h.back(place(3), any);              // leaving 3, land on 2
        QVERIFY(b.has_value());
        QCOMPARE(b->viewRootId, 2ULL);
        QCOMPARE(h.backEntries().size(), 1);
        QCOMPARE(h.forwardEntries().size(), 1);
        QCOMPARE(h.forwardEntries().last().viewRootId, 3ULL);
        QVERIFY(h.canForward());
        auto f = h.forward(place(2), any);           // leaving 2, back to 3
        QVERIFY(f.has_value());
        QCOMPARE(f->viewRootId, 3ULL);
        QVERIFY(!h.canForward());
        QCOMPARE(h.backEntries().size(), 2);
        // Back again, then a NEW push wipes the forward stack.
        QVERIFY(h.back(place(3), any).has_value());
        QVERIFY(h.canForward());
        h.push(place(9));
        QVERIFY(!h.canForward());
        QVERIFY(h.forwardEntries().isEmpty());
        // Nothing left: back() returns nullopt and moves nothing.
        NavHistory empty;
        QVERIFY(!empty.back(place(1), any).has_value());
        QVERIFY(empty.forwardEntries().isEmpty());
        empty.clear();
        QVERIFY(!empty.canBack());
    }

    void testCapIsFifty() {
        NavHistory h;
        for (uint64_t i = 1; i <= 60; ++i) h.push(place(i));
        QCOMPARE(h.backEntries().size(), NavHistory::kMax);
        QCOMPARE(NavHistory::kMax, 50);
        QCOMPARE(h.backEntries().first().viewRootId, 11ULL);   // oldest dropped
        QCOMPARE(h.backEntries().last().viewRootId, 60ULL);
    }

    void testBackForwardSkipStaleEntries() {
        NavHistory h;
        h.push(place(1));
        h.push(place(2));       // will be "stale"
        h.push(place(3));       // will be "stale"
        auto notTwoOrThree = [](const NavEntry& e) { return e.viewRootId != 2 && e.viewRootId != 3; };
        auto b = h.back(place(4), notTwoOrThree);
        QVERIFY(b.has_value());
        QCOMPARE(b->viewRootId, 1ULL);              // 3 and 2 discarded on the way
        QVERIFY(h.backEntries().isEmpty());
        QCOMPARE(h.forwardEntries().size(), 1);      // only the place we left
        // All stale → nullopt, forward untouched.
        NavHistory h2;
        h2.push(place(2));
        QVERIFY(!h2.back(place(5), notTwoOrThree).has_value());
        QVERIFY(h2.forwardEntries().isEmpty());
        QVERIFY(!h2.canBack());                      // the stale entry is gone
    }

    void testNavEntryValidAgainstTree() {
        NodeTree tree; Ids id = buildChain(tree);
        QVERIFY(navEntryValid(tree, place(id.editor, { id.vptr, id.parent })));
        QVERIFY(navEntryValid(tree, place(0)));                       // show-all
        QVERIFY(!navEntryValid(tree, place(0xBAD)));
        QVERIFY(!navEntryValid(tree, place(id.editor, { id.vptr, 0xBAD })));
    }

    // ── AddressBarState ──

    void testStateEqualityCoversEveryField() {
        AddressBarState a;
        a.sourceName = "game.exe"; a.sourceKindId = "Process"; a.liveness = liveness::Live;
        a.baseAddress = 0x1000; a.baseFormula = "<game.exe>"; a.resolvedBase = 0x1000;
        Crumb c; c.label = "Player"; c.classId = 1; c.address = 0x1000; c.keyword = "struct";
        a.crumbs = { c };
        a.trailPath = "Player";
        a.canBack = true; a.canForward = false; a.canUp = true;
        AddressBarState b = a;
        QVERIFY(a == b);
        QVERIFY(!(a != b));

        auto differs = [&](auto mutate) {
            AddressBarState m = a;
            mutate(m);
            return m != a;
        };
        QVERIFY(differs([](AddressBarState& s) { s.sourceName = "other"; }));
        QVERIFY(differs([](AddressBarState& s) { s.sourceKindId = "File"; }));
        QVERIFY(differs([](AddressBarState& s) { s.liveness = liveness::Stale; }));
        QVERIFY(differs([](AddressBarState& s) { s.baseAddress = 0x2000; }));
        QVERIFY(differs([](AddressBarState& s) { s.baseFormula = "0x2000"; }));
        QVERIFY(differs([](AddressBarState& s) { s.resolvedBase = 0x2000; }));
        QVERIFY(differs([](AddressBarState& s) { s.crumbs[0].label = "Enemy"; }));
        QVERIFY(differs([](AddressBarState& s) { s.trailPath = "Player.stats"; }));
        QVERIFY(differs([](AddressBarState& s) { s.crumbs[0].classId = 2; }));
        QVERIFY(differs([](AddressBarState& s) { s.crumbs[0].pointerId = 3; }));
        QVERIFY(differs([](AddressBarState& s) { s.crumbs[0].address = 0; }));
        QVERIFY(differs([](AddressBarState& s) { s.crumbs[0].keyword = "class"; }));
        QVERIFY(differs([](AddressBarState& s) { s.crumbs[0].rootId = 1; }));
        QVERIFY(differs([](AddressBarState& s) { s.crumbs.clear(); }));
        QVERIFY(differs([](AddressBarState& s) { s.canBack = false; }));
        QVERIFY(differs([](AddressBarState& s) { s.canForward = true; }));
        QVERIFY(differs([](AddressBarState& s) { s.canUp = false; }));
        // The liveness ladder is the SourceStatus order (None…Disconnected).
        QCOMPARE(liveness::None, 0);
        QCOMPARE(liveness::Disconnected, 4);
    }

    // ── containerOf / expandedHopInto moved into core.h ──

    void testContainerOfAndExpandedHopIntoAreTreeFunctions() {
        NodeTree tree; Ids id = buildChain(tree);
        QCOMPARE(containerOf(tree, id.editor), 0ULL);          // a root has no container
        QCOMPARE(containerOf(tree, id.vptr), id.editor);
        QCOMPARE(containerOf(tree, id.parent), id.priv);
        QCOMPARE(containerOf(tree, id.geomX), id.geom);        // embedded struct is a frame
        QCOMPARE(containerOf(tree, id.geom), id.widget);
        QCOMPARE(containerOf(tree, 0xBAD), 0ULL);
        // vptr is the expanded hop into QWidgetPrivate; nothing opens QWidget yet.
        QCOMPARE(expandedHopInto(tree, id.priv), id.vptr);
        QCOMPARE(expandedHopInto(tree, id.widget), 0ULL);
        tree.nodes[tree.indexOfId(id.parent)].collapsed = false;
        QCOMPARE(expandedHopInto(tree, id.widget), id.parent);
        // A collapsed embedded struct is not an open hop; expanded it is its own.
        QCOMPARE(expandedHopInto(tree, id.geom), 0ULL);
        tree.nodes[tree.indexOfId(id.geom)].collapsed = false;
        QCOMPARE(expandedHopInto(tree, id.geom), id.geom);
    }
};

QTEST_GUILESS_MAIN(TestAddressBarModel)
#include "test_address_bar_model.moc"
