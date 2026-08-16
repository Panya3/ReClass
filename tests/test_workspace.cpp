// Regression tests for the Project-explorer workspace model (workspace_model.h),
// specifically the empty-state row count that drives the EmptyHintTreeView
// "No types yet" overlay in the workspace dock.
#include "workspace_model.h"
#include "session.h"
#include <QtTest/QtTest>
#include <QDateTime>
#include <QFile>
#include <QJsonDocument>
#include <QStandardItemModel>
#include <QTemporaryDir>

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

    // ── Session (open-tabs) serialization ──

    void testSessionRoundTrip() {
        NodeTree tree;
        Node s1; s1.kind = NodeKind::Struct;
        s1.structTypeName = QStringLiteral("Alpha"); s1.parentId = 0;
        int i1 = tree.addNode(s1);
        Node s2; s2.kind = NodeKind::Struct;
        s2.structTypeName = QStringLiteral("Beta"); s2.parentId = 0;
        int i2 = tree.addNode(s2);

        rcx::Session s;
        s.activeTab = 1;

        // Untitled doc with an embedded content snapshot
        rcx::SessionDoc d0;
        d0.title = QStringLiteral("Untitled");
        d0.contentJson = QJsonDocument(tree.toJson()).toJson(QJsonDocument::Compact);
        d0.hasContent = true;
        s.docs.append(d0);

        // Saved doc — travels by path only
        rcx::SessionDoc d1;
        d1.filePath = QStringLiteral("C:/proj/foo.rcx");
        d1.title = QStringLiteral("foo");
        s.docs.append(d1);

        s.tabs.append(rcx::SessionTab{ 0, tree.nodes[i1].id, QStringLiteral("Alpha") });
        s.tabs.append(rcx::SessionTab{ 1, 0, QStringLiteral("foo") });   // active
        s.tabs.append(rcx::SessionTab{ 0, tree.nodes[i2].id, QStringLiteral("Beta") });

        QJsonObject j = rcx::sessionToJson(s);
        rcx::Session out;
        QVERIFY(rcx::sessionFromJson(j, out));
        QCOMPARE(out.version, rcx::kSessionFileVersion);
        QCOMPARE(out.activeTab, 1);
        QCOMPARE(out.docs.size(), 2);
        QVERIFY(out.docs[0].hasContent);
        QCOMPARE(out.docs[1].filePath, QStringLiteral("C:/proj/foo.rcx"));
        QCOMPARE(out.tabs.size(), 3);
        QCOMPARE(out.tabs[0].docIndex, 0);
        QCOMPARE(out.tabs[0].viewRootId, tree.nodes[i1].id);
        QCOMPARE(out.tabs[0].title, QStringLiteral("Alpha"));
        QCOMPARE(out.tabs[1].docIndex, 1);
        QCOMPARE(out.tabs[1].viewRootId, uint64_t(0));
        QCOMPARE(out.tabs[2].docIndex, 0);

        // Embedded content must round-trip back to the same tree shape
        QJsonDocument cd = QJsonDocument::fromJson(out.docs[0].contentJson);
        QVERIFY(!cd.isNull() && cd.isObject());
        NodeTree t2 = rcx::NodeTree::fromJson(cd.object());
        QCOMPARE(t2.nodes.size(), tree.nodes.size());
    }

    void testSessionRejectsUnknownVersion() {
        rcx::Session s;
        QJsonObject j = rcx::sessionToJson(s);
        j["sessionVersion"] = rcx::kSessionFileVersion + 1;   // from a newer build
        rcx::Session out;
        QVERIFY(!rcx::sessionFromJson(j, out));

        QJsonObject legacy;   // missing sessionVersion = not a session
        legacy["docs"] = QJsonArray();
        legacy["tabs"] = QJsonArray();
        QVERIFY(!rcx::sessionFromJson(legacy, out));
    }

    // ── Restore-domain helpers ──

    // viewRootId 0 is the valid "show all roots" view — the restore must
    // KEEP it, not treat it as a stale id. Regression: the first version
    // of restoreSessionIfAny fell back to the first root struct for any
    // vr == 0, force-zooming an overview tab into the first struct.
    void testResolveRestoredViewRootKeepsShowAll() {
        NodeTree tree;
        Node a; a.kind = NodeKind::Struct;
        a.structTypeName = QStringLiteral("Alpha"); a.parentId = 0;
        tree.addNode(a);
        Node b; b.kind = NodeKind::Struct;
        b.structTypeName = QStringLiteral("Beta"); b.parentId = 0;
        tree.addNode(b);

        QCOMPARE(rcx::resolveRestoredViewRoot(tree, 0), uint64_t(0));   // show-all kept
    }

    void testResolveRestoredViewRootKeepsValidId() {
        NodeTree tree;
        Node a; a.kind = NodeKind::Struct;
        a.structTypeName = QStringLiteral("Alpha"); a.parentId = 0;
        int ia = tree.addNode(a);
        const uint64_t id = tree.nodes[ia].id;
        QCOMPARE(rcx::resolveRestoredViewRoot(tree, id), id);
    }

    void testResolveRestoredViewRootFallsBackOnStaleId() {
        NodeTree tree;
        Node a; a.kind = NodeKind::Struct;
        a.structTypeName = QStringLiteral("Alpha"); a.parentId = 0;
        int ia = tree.addNode(a);
        Node b; b.kind = NodeKind::Struct;
        b.structTypeName = QStringLiteral("Beta"); b.parentId = 0;
        tree.addNode(b);

        // Stale id (file edited elsewhere / old session) → first root struct
        const uint64_t stale = 0xFFFFFFFFFFFFULL;
        QCOMPARE(rcx::resolveRestoredViewRoot(tree, stale), tree.nodes[ia].id);

        // Stale id with no root struct left → back to show-all (0), never
        // a dangling id
        NodeTree empty;
        Node hex; hex.kind = NodeKind::Hex64; hex.parentId = 0;
        empty.addNode(hex);
        QCOMPARE(rcx::resolveRestoredViewRoot(empty, stale), uint64_t(0));
    }

    // The shadow-fresher check drives whether restore loads foo.rcx or
    // foo.rcx.autosave. Regression: restore previously loaded the plain
    // path, silently dropping edits the 60s autosave had already captured.
    void testShadowIsFresher() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString orig = dir.filePath("foo.rcx");
        const QString shadow = orig + ".autosave";

        { QFile f(orig); QVERIFY(f.open(QIODevice::WriteOnly)); f.write("old"); }
        { QFile f(shadow); QVERIFY(f.open(QIODevice::WriteOnly)); f.write("new"); }

        auto setMtime = [](const QString& p, const QDateTime& dt) {
            // Qt6's setFileTime operates on the open device handle on
            // Windows — the file must be open for writing first.
            QFile f(p);
            if (!f.open(QIODevice::ReadWrite))
                return false;
            return f.setFileTime(dt, QFileDevice::FileModificationTime);
        };

        // Shadow older than the original → not fresher (stale shadow)
        QVERIFY(setMtime(orig, QDateTime::currentDateTime()));
        QVERIFY(setMtime(shadow, QDateTime::currentDateTime().addSecs(-60)));
        QVERIFY(!rcx::shadowIsFresher(orig));

        // Shadow newer than the original → fresher → restore should load it
        QVERIFY(setMtime(shadow, QDateTime::currentDateTime().addSecs(60)));
        QVERIFY(rcx::shadowIsFresher(orig));

        // No shadow at all → not fresher
        QVERIFY(QFile::remove(shadow));
        QVERIFY(!rcx::shadowIsFresher(orig));
    }

    // Opening a file must find the tab layout the session remembered for
    // THAT file (matched by path). Regression: project_open collapsed every
    // open to a single tab and never consulted the session, so a saved tab
    // layout never came back when reopening a file mid-session.
    void testSessionDocIndexForPath() {
        rcx::Session s;
        rcx::SessionDoc d0; d0.title = QStringLiteral("Untitled");  // no path
        s.docs.append(d0);
        rcx::SessionDoc d1;
        d1.filePath = QStringLiteral("C:/proj/foo.rcx");
        s.docs.append(d1);

        QCOMPARE(rcx::sessionDocIndexForPath(s, QStringLiteral("C:/proj/foo.rcx")), 1);
        QCOMPARE(rcx::sessionDocIndexForPath(s, QStringLiteral("C:/proj/other.rcx")), -1);
        QCOMPARE(rcx::sessionDocIndexForPath(s, QStringLiteral("")), -1);   // untitled never matches
    }
};

QTEST_MAIN(TestWorkspace)
#include "test_workspace.moc"
