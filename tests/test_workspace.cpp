// Regression tests for the Project-explorer workspace model (workspace_model.h),
// specifically the empty-state row count that drives the EmptyHintTreeView
// "No types yet" overlay in the workspace dock.
#include "workspace_model.h"
#include <QtTest/QtTest>
#include <QStandardItemModel>

using namespace rcx;

class TestWorkspace : public QObject {
    Q_OBJECT
private slots:
    // An empty project (no tabs) must yield ZERO rows so the tree's empty-state
    // overlay can fire. Regression: buildProjectExplorer used to append an
    // UNCONDITIONAL "ALL TYPES" section header, leaving the model permanently at
    // rowCount >= 1 — the overlay's `rowCount > 0 -> return` guard then never
    // painted the placeholder on a genuinely empty project.
    void testEmptyProjectHasNoRows() {
        QStandardItemModel model;
        buildProjectExplorer(&model, {}, {});
        QCOMPARE(model.rowCount(), 0);
    }

    // A tab whose tree holds no top-level Struct types is empty for the explorer
    // too (only Struct nodes are listed), so still zero rows.
    void testNonStructTabHasNoRows() {
        NodeTree tree;
        Node n; n.kind = NodeKind::Hex64; n.parentId = 0; tree.addNode(n);
        QVector<TabInfo> tabs{ TabInfo{ &tree, QStringLiteral("T"), nullptr } };
        QStandardItemModel model;
        buildProjectExplorer(&model, tabs, {});
        QCOMPARE(model.rowCount(), 0);
    }

    // One struct type → "ALL TYPES" header row + 1 type row (header still emits
    // when there's content under it).
    void testStructTabHasHeaderAndRow() {
        NodeTree tree;
        Node s; s.kind = NodeKind::Struct;
        s.structTypeName = QStringLiteral("MyType"); s.parentId = 0;
        tree.addNode(s);
        QVector<TabInfo> tabs{ TabInfo{ &tree, QStringLiteral("T"), nullptr } };
        QStandardItemModel model;
        buildProjectExplorer(&model, tabs, {});
        QCOMPARE(model.rowCount(), 2);   // ALL TYPES header + the type
        QVERIFY(!model.item(0)->data(RoleSectionHeader).toString().isEmpty());
        QVERIFY(model.item(1)->data(RoleSectionHeader).toString().isEmpty());
    }

    // The badge highlight (Qt::UserRole + 3) must update IN PLACE when the
    // open-tab set changes — no rebuild needed. Regression: MainWindow's
    // generation gate skipped rebuilds on tab open/close (the gate's hash
    // doesn't cover tab/viewed state), so a closed tab's item stayed lit and
    // a freshly opened one never lit.
    void testViewedFlagsRefreshInPlace() {
        NodeTree tree;
        Node a; a.kind = NodeKind::Struct;
        a.structTypeName = QStringLiteral("Alpha"); a.parentId = 0;
        int ai = tree.addNode(a);
        uint64_t idA = tree.nodes[ai].id;
        Node b; b.kind = NodeKind::Struct;
        b.structTypeName = QStringLiteral("Beta"); b.parentId = 0;
        int bi = tree.addNode(b);
        uint64_t idB = tree.nodes[bi].id;

        QVector<TabInfo> tabs{ TabInfo{ &tree, QStringLiteral("T"), nullptr } };
        QStandardItemModel model;
        buildProjectExplorer(&model, tabs, {});

        auto itemById = [&](uint64_t id) -> QStandardItem* {
            for (int i = 0; i < model.rowCount(); ++i) {
                auto* it = model.item(i);
                if (!it || !it->data(RoleSectionHeader).toString().isEmpty()) continue;
                if (it->data(Qt::UserRole + 1).toULongLong() == id) return it;
            }
            return nullptr;
        };

        QStandardItem* ia = itemById(idA);
        QStandardItem* ib = itemById(idB);
        QVERIFY(ia && ib);

        // Tab for Alpha open → only Alpha lit
        applyViewedPinnedFlags(&model, {idA}, {});
        QVERIFY(ia->data(Qt::UserRole + 3).toBool());
        QVERIFY(!ib->data(Qt::UserRole + 3).toBool());

        // Tab for Beta opens too → both lit
        applyViewedPinnedFlags(&model, {idA, idB}, {});
        QVERIFY(ib->data(Qt::UserRole + 3).toBool());

        // Both tabs closed → both dim (regression: previously stayed lit
        // because the gated rebuild never ran)
        applyViewedPinnedFlags(&model, {}, {});
        QVERIFY(!ia->data(Qt::UserRole + 3).toBool());
        QVERIFY(!ib->data(Qt::UserRole + 3).toBool());

        // Pinned flag rides along without disturbing the viewed state
        applyViewedPinnedFlags(&model, {idA}, {idB});
        QVERIFY(ia->data(Qt::UserRole + 3).toBool());
        QVERIFY(ib->data(Qt::UserRole + 4).toBool());
        QVERIFY(!ib->data(Qt::UserRole + 3).toBool());
    }
};

QTEST_MAIN(TestWorkspace)
#include "test_workspace.moc"
