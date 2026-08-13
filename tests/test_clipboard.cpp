#include "clipboard.h"
#include "core.h"
#include <QtTest/QtTest>
#include <QMimeData>

using namespace rcx;

class TestClipboard : public QObject {
    Q_OBJECT
private slots:

    // Build a small tree: one root struct with 3 Hex32 children.
    static NodeTree makeSimple() {
        NodeTree t;
        t.m_nextId = 1;
        Node root;
        root.kind = NodeKind::Struct;
        root.name = "root";
        root.structTypeName = "Root";
        root.parentId = 0;
        root.offset = 0;
        t.addNode(root);
        uint64_t rid = t.nodes[0].id;

        for (int i = 0; i < 3; i++) {
            Node c;
            c.kind = NodeKind::Hex32;
            c.name = QStringLiteral("f%1").arg(i);
            c.parentId = rid;
            c.offset = i * 4;
            t.addNode(c);
        }
        return t;
    }

    void roundTripLeafNode() {
        NodeTree t = makeSimple();
        uint64_t leafId = t.nodes[1].id;

        auto* mime = ClipboardCodec::serialize(t, {leafId});
        QVERIFY(mime != nullptr);
        QVERIFY(mime->hasFormat(ClipboardCodec::kMimeType));
        QVERIFY(!mime->text().isEmpty());

        NodeTree target = makeSimple();
        auto result = ClipboardCodec::deserialize(target, mime);
        delete mime;

        QCOMPARE(result.nodes.size(), 1);
        QCOMPARE(result.nodes[0].kind, NodeKind::Hex32);
        QCOMPARE(result.nodes[0].name, QStringLiteral("f0"));
        QCOMPARE(result.rootIds.size(), 1);
        // New id must NOT collide with anything in target.
        QVERIFY(target.indexOfId(result.rootIds[0]) < 0);
    }

    void roundTripSubtree() {
        // Copy the root struct — subtree should carry along.
        NodeTree t = makeSimple();
        uint64_t rid = t.nodes[0].id;
        auto* mime = ClipboardCodec::serialize(t, {rid});

        NodeTree target;
        target.m_nextId = 100;  // deliberately offset id space
        auto result = ClipboardCodec::deserialize(target, mime);
        delete mime;

        // 1 root + 3 children pasted.
        QCOMPARE(result.nodes.size(), 4);

        // IDs remapped, parent pointers re-wired.
        uint64_t newRoot = result.rootIds[0];
        int children = 0;
        for (const Node& n : result.nodes) {
            if (n.parentId == newRoot) ++children;
        }
        QCOMPARE(children, 3);
    }

    void noMatchForUnknownMime() {
        NodeTree t = makeSimple();
        QMimeData foreign;
        foreign.setText("random stuff");
        auto result = ClipboardCodec::deserialize(t, &foreign);
        QVERIFY(result.nodes.isEmpty());
        QVERIFY(result.rootIds.isEmpty());
    }

    void nullMime() {
        NodeTree t = makeSimple();
        auto result = ClipboardCodec::deserialize(t, nullptr);
        QVERIFY(result.nodes.isEmpty());
    }

    void pastedIdsAreFresh() {
        // Paste into the SAME tree — every id from the paste must be new.
        NodeTree t = makeSimple();
        QSet<uint64_t> preexisting;
        for (const auto& n : t.nodes) preexisting.insert(n.id);
        uint64_t rid = t.nodes[0].id;

        auto* mime = ClipboardCodec::serialize(t, {rid});
        auto result = ClipboardCodec::deserialize(t, mime);
        delete mime;

        for (const Node& n : result.nodes)
            QVERIFY2(!preexisting.contains(n.id),
                     "pasted node id collides with existing");
    }

    void plainDumpReadable() {
        NodeTree t = makeSimple();
        QString dump = ClipboardCodec::plainDump(t, {t.nodes[0].id});
        QVERIFY(dump.contains("Root") || dump.contains("root"));
        QVERIFY(dump.contains("+0x0000"));
        QVERIFY(dump.contains("f0"));
    }

    void multipleRootsPreserveOrder() {
        // Roots must serialize in the order given (struct order from the
        // caller) — the paste handler places pasted roots in the order
        // they appear in result.nodes, so a LIFO reversal would paste the
        // later sibling first and scramble the pasted layout.
        NodeTree t = makeSimple();
        uint64_t id0 = t.nodes[1].id;  // f0 @ 0
        uint64_t id1 = t.nodes[2].id;  // f1 @ 4
        uint64_t id2 = t.nodes[3].id;  // f2 @ 8

        auto* mime = ClipboardCodec::serialize(t, {id0, id1, id2});
        NodeTree target;
        target.m_nextId = 1;
        auto result = ClipboardCodec::deserialize(target, mime);
        delete mime;

        QCOMPARE(result.rootIds.size(), 3);
        QVERIFY(result.rootIds[0] != 0);
        QVERIFY(result.rootIds[1] != 0);
        QVERIFY(result.rootIds[2] != 0);

        // The roots appear in result.nodes in the same order they were
        // passed in (deserialize preserves the JSON array order).
        QVector<uint64_t> inNodes;
        for (const Node& n : result.nodes)
            if (result.rootIds.contains(n.id)) inNodes.append(n.id);
        QCOMPARE(inNodes.size(), 3);
        QCOMPARE(inNodes[0], result.rootIds[0]);
        QCOMPARE(inNodes[1], result.rootIds[1]);
        QCOMPARE(inNodes[2], result.rootIds[2]);
    }
};

QTEST_GUILESS_MAIN(TestClipboard)
#include "test_clipboard.moc"
