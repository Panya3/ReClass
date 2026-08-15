#include <QtTest/QTest>
#include <QtTest/QSignalSpy>
#include <QApplication>
#include <QClipboard>
#include <QSplitter>
#include <QTemporaryFile>
#include <QTemporaryDir>
#include <QJsonDocument>
#include <QMouseEvent>
#include <QKeyEvent>
#include <Qsci/qsciscintilla.h>
#include <Qsci/qsciscintillabase.h>
#include "clipboard.h"
#include "controller.h"
#include "core.h"
#include "typeselectorpopup.h"
#include "widgets/fieldlayoutdialog.h"
#include "widgets/nestedstructdialog.h"
#include <QTreeWidget>
#include <QHeaderView>
#include <QComboBox>
#include <QLineEdit>
#include <QPushButton>

using namespace rcx;

// Provider with a configurable base address (for testing source-switch logic)
class BaseAwareProvider : public Provider {
    QByteArray m_data;
    uint64_t   m_base;
public:
    BaseAwareProvider(QByteArray data, uint64_t base)
        : m_data(std::move(data)), m_base(base) {}
    bool read(uint64_t addr, void* buf, int len) const override {
        if (addr + len > (uint64_t)m_data.size()) return false;
        std::memcpy(buf, m_data.constData() + addr, len);
        return true;
    }
    int size() const override { return m_data.size(); }
    uint64_t base() const override { return m_base; }
    bool isLive() const override { return true; }
    QString name() const override { return QStringLiteral("test"); }
    QString kind() const override { return QStringLiteral("Process"); }
};

// Small tree: one root struct with a few typed fields at known offsets.
// Keeps tests fast and deterministic (no giant PEB tree).
static void buildSmallTree(NodeTree& tree) {
    tree.baseAddress = 0;

    Node root;
    root.kind = NodeKind::Struct;
    root.structTypeName = "TestStruct";
    root.name = "root";
    root.parentId = 0;
    root.offset = 0;
    root.collapsed = false;
    int ri = tree.addNode(root);
    uint64_t rootId = tree.nodes[ri].id;

    auto field = [&](int off, NodeKind k, const char* name) {
        Node n;
        n.kind = k;
        n.name = name;
        n.parentId = rootId;
        n.offset = off;
        tree.addNode(n);
    };

    field(0,  NodeKind::UInt32,  "field_u32");    // 4 bytes
    field(4,  NodeKind::Float,   "field_float");   // 4 bytes
    field(8,  NodeKind::UInt8,   "field_u8");      // 1 byte
    field(9,  NodeKind::Hex16,   "pad0");           // 2 bytes
    field(11, NodeKind::Hex8,    "pad1");           // 1 byte
    field(12, NodeKind::Hex32,   "field_hex");     // 4 bytes
}

// 64-byte buffer with recognizable pattern
static QByteArray makeSmallBuffer() {
    QByteArray data(64, '\0');
    // field_u32 at offset 0 = 0xDEADBEEF
    uint32_t v32 = 0xDEADBEEF;
    memcpy(data.data() + 0, &v32, 4);
    // field_float at offset 4 = 3.14f
    float vf = 3.14f;
    memcpy(data.data() + 4, &vf, 4);
    // field_u8 at offset 8 = 0x42
    data[8] = 0x42;
    // pad0 at offset 9 = 0x00 0x00 0x00
    // field_hex at offset 12 = 0xCAFEBABE
    uint32_t vhex = 0xCAFEBABE;
    memcpy(data.data() + 12, &vhex, 4);
    return data;
}

class TestController : public QObject {
    Q_OBJECT
private:
    RcxDocument* m_doc = nullptr;
    RcxController* m_ctrl = nullptr;
    QSplitter* m_splitter = nullptr;
    RcxEditor* m_editor = nullptr;

private slots:
    void init() {
        m_doc = new RcxDocument();
        buildSmallTree(m_doc->tree);
        m_doc->provider = std::make_unique<BufferProvider>(makeSmallBuffer());

        m_splitter = new QSplitter();
        // Pass nullptr as parent so controller is not auto-deleted with splitter
        m_ctrl = new RcxController(m_doc, nullptr);
        // Tests drive refresh() explicitly — stop the GUI auto-refresh timer
        // (started in the controller ctor) so a spurious 200ms tick can't fire
        // during processEvents(). onRefreshTick reads 4096-byte pages; the test
        // providers are tiny, so a tick mid-test snapshotting all-zero pages
        // silently relights value-history heat (flaky testClearValueHistoryResetsHeat).
        m_ctrl->setWindowState(false, false);
        m_editor = m_ctrl->addSplitEditor(m_splitter);

        m_splitter->resize(800, 600);
        m_splitter->show();
        QVERIFY(QTest::qWaitForWindowExposed(m_splitter));
        QApplication::processEvents();
    }

    void cleanup() {
        // Delete controller first (disconnects from editor signals)
        delete m_ctrl;
        m_ctrl = nullptr;
        m_editor = nullptr;  // owned by splitter
        delete m_splitter;
        m_splitter = nullptr;
        delete m_doc;
        m_doc = nullptr;
    }

    // ── File-level .rcx save→load round-trip ──
    // The primary persistence path: RcxDocument::save writes the tree PLUS the
    // document-level typeAliases map; load() reads both back (the aliases after
    // the validation pass). This guards that the whole file round-trips — a
    // regression dropping the typeAliases read would silently lose user aliases.
    void testFileSaveLoadRoundTrip() {
        auto* doc = new RcxDocument();
        Node root; root.kind = NodeKind::Struct; root.name = "Root";
        root.structTypeName = "MyType"; root.parentId = 0; root.offset = 0;
        doc->tree.addNode(root);
        doc->typeAliases[NodeKind::Float] = QStringLiteral("vec_component");

        QTemporaryFile f;
        QVERIFY(f.open());
        const QString path = f.fileName();
        f.close();
        QVERIFY(doc->save(path));

        auto* doc2 = new RcxDocument();
        QVERIFY(doc2->load(path));
        QCOMPARE(doc2->tree.nodes.size(), doc->tree.nodes.size());
        QCOMPARE(doc2->tree.nodes[0].name, QString("Root"));
        QCOMPARE(doc2->tree.nodes[0].structTypeName, QString("MyType"));
        QVERIFY2(doc2->typeAliases.contains(NodeKind::Float),
                 "typeAliases must survive the file round-trip");
        QCOMPARE(doc2->typeAliases.value(NodeKind::Float), QString("vec_component"));

        delete doc;
        delete doc2;
    }

    // ── File-format version key ──
    // save() stamps fileVersion; load() tolerates a missing key (legacy
    // file) and flags files written by a NEWER build (best-effort load
    // with a loud flag) instead of silently mis-reading them.
    void testFileVersionHandling() {
        auto* doc = new RcxDocument();
        Node root; root.kind = NodeKind::Struct; root.name = "Root"; root.parentId = 0;
        doc->tree.addNode(root);

        QTemporaryFile f;
        QVERIFY(f.open());
        const QString path = f.fileName();
        f.close();

        // 1. save() stamps the current version; the raw JSON carries it.
        QVERIFY(doc->save(path));
        {
            QFile rf(path);
            QVERIFY(rf.open(QIODevice::ReadOnly));
            const QJsonObject saved = QJsonDocument::fromJson(rf.readAll()).object();
            QCOMPARE(saved["fileVersion"].toInt(-1), rcx::kRcxFileVersion);
        }

        // 2. A legacy file (no key) loads cleanly and is not flagged.
        {
            QFile wf(path);
            QVERIFY(wf.open(QIODevice::WriteOnly | QIODevice::Truncate));
            QJsonObject o;
            o["baseAddress"] = QStringLiteral("400000");
            o["nodes"] = QJsonArray();
            wf.write(QJsonDocument(o).toJson());
        }
        auto* legacy = new RcxDocument();
        QVERIFY(legacy->load(path));
        QCOMPARE(legacy->m_loadFileVersionTooNew, 0);

        // 3. A file claiming a newer version loads best-effort + flags.
        {
            QFile wf(path);
            QVERIFY(wf.open(QIODevice::WriteOnly | QIODevice::Truncate));
            QJsonObject o;
            o["baseAddress"] = QStringLiteral("400000");
            o["fileVersion"] = rcx::kRcxFileVersion + 1;
            o["nodes"] = QJsonArray();
            wf.write(QJsonDocument(o).toJson());
        }
        auto* future = new RcxDocument();
        QVERIFY(future->load(path));
        QCOMPARE(future->m_loadFileVersionTooNew, rcx::kRcxFileVersion + 1);

        delete doc;
        delete legacy;
        delete future;
    }

    // ── Draft lifecycle (automatic system) ──
    // A draft only persists while its offset actually conflicts; the
    // auto-clear sweep in applyCommand wipes it the moment the conflict
    // disappears (or if it never existed). Survives a .rcx round-trip
    // while the conflict is live.
    void testDraftLifecycleAndFileRoundTrip() {
        auto idxOf = [&](const QString& name) {
            for (int i = 0; i < m_doc->tree.nodes.size(); i++)
                if (m_doc->tree.nodes[i].name == name) return i;
            return -1;
        };
        int hex = idxOf(QStringLiteral("field_hex"));  // 0xC..0x10
        int pad1 = idxOf(QStringLiteral("pad1"));      // 0xB
        QVERIFY(hex >= 0 && pad1 >= 0);

        // Create a live conflict: pad1 (1 byte) moved onto field_hex
        m_doc->tree.nodes[pad1].offset = 0xE;           // 0xE..0xF overlaps
        uint64_t hexId = m_doc->tree.nodes[hex].id;

        // setNodeDraft pushes a command → the auto-clear sweep runs after it;
        // the conflict is real, so the draft persists.
        m_ctrl->setNodeDraft(hexId, true);
        QVERIFY(m_doc->tree.nodes[m_doc->tree.indexOfId(hexId)].draft);
        m_doc->undoStack.undo();
        QVERIFY(!m_doc->tree.nodes[m_doc->tree.indexOfId(hexId)].draft);

        // File round-trip keeps the flag while the conflict is live
        m_ctrl->setNodeDraft(hexId, true);
        QTemporaryFile f;
        QVERIFY(f.open());
        const QString path = f.fileName();
        f.close();
        QVERIFY(m_doc->save(path));

        auto* doc2 = new RcxDocument();
        QVERIFY(doc2->load(path));
        bool found = false;
        for (const auto& n : doc2->tree.nodes)
            if (n.name == QStringLiteral("field_hex")) { QVERIFY(n.draft); found = true; }
        QVERIFY(found);
        delete doc2;
    }

    // ── Auto-clear sweep: the draft disappears the moment its offset no
    //    longer conflicts — the user's "เมื่อเงื่อนไขถูกต้อง สถานะ draft
    //    ก็จะหายไป" rule. No manual toggle exists anymore, so this sweep
    //    is the ONLY way a stale draft gets un-stuck.
    void testDraftAutoClearSweep() {
        auto idxOf = [&](const QString& name) {
            for (int i = 0; i < m_doc->tree.nodes.size(); i++)
                if (m_doc->tree.nodes[i].name == name) return i;
            return -1;
        };
        int hex = idxOf(QStringLiteral("field_hex"));  // 0xC..0x10
        int pad1 = idxOf(QStringLiteral("pad1"));      // 0xB
        uint64_t hexId = m_doc->tree.nodes[hex].id;

        // Conflict live → draft sticks
        m_doc->tree.nodes[pad1].offset = 0xE;
        m_ctrl->setNodeDraft(hexId, true);
        QVERIFY(m_doc->tree.nodes[m_doc->tree.indexOfId(hexId)].draft);

        // Fix = delete the conflicting sibling → next command auto-clears
        m_ctrl->removeNode(m_doc->tree.indexOfId(m_doc->tree.nodes[pad1].id));
        QVERIFY(!m_doc->tree.nodes[m_doc->tree.indexOfId(hexId)].draft);

        // A draft on a conflict-free field never sticks at all
        int floatIdx = idxOf(QStringLiteral("field_float"));
        uint64_t floatId = m_doc->tree.nodes[floatIdx].id;
        m_ctrl->setNodeDraft(floatId, true);
        QVERIFY(!m_doc->tree.nodes[m_doc->tree.indexOfId(floatId)].draft);
    }

    // ── Two delete modes ──
    // removeNode(idx, keepOffsets=true) leaves the remaining siblings' offsets
    // untouched (the deleted span stays as a gap); the default compacts them
    // up by the deleted size. Both must undo cleanly.
    void testDeleteKeepOffsets() {
        auto idxOf = [&](const QString& name) {
            for (int i = 0; i < m_doc->tree.nodes.size(); i++)
                if (m_doc->tree.nodes[i].name == name) return i;
            return -1;
        };
        int u8 = idxOf(QStringLiteral("field_u8"));  // offset 8, 1 byte
        QVERIFY(u8 >= 0);

        // Mode 2: keep offsets
        m_ctrl->removeNode(u8, /*keepOffsets=*/true);
        QCOMPARE(m_doc->tree.nodes[idxOf(QStringLiteral("pad0"))].offset, 9);
        QCOMPARE(m_doc->tree.nodes[idxOf(QStringLiteral("field_hex"))].offset, 12);
        m_doc->undoStack.undo();
        QCOMPARE(m_doc->tree.nodes[idxOf(QStringLiteral("field_u8"))].offset, 8);

        // Mode 1: compact (default) — later siblings shift up by 1
        int u8b = idxOf(QStringLiteral("field_u8"));
        m_ctrl->removeNode(u8b);
        QCOMPARE(m_doc->tree.nodes[idxOf(QStringLiteral("pad0"))].offset, 8);
        QCOMPARE(m_doc->tree.nodes[idxOf(QStringLiteral("field_hex"))].offset, 11);
        m_doc->undoStack.undo();
        QCOMPARE(m_doc->tree.nodes[idxOf(QStringLiteral("field_u8"))].offset, 8);
    }

    // ── Offset conflict descriptions for the insert/edit dialogs ──
    // buildSmallTree: field_u32 @0 (4B), field_float @4 (4B), ...
    void testDescribeOffsetConflict() {
        uint64_t rootId = m_doc->tree.nodes[0].id;  // TestStruct
        // Duplicate start offset
        QVERIFY(!m_ctrl->describeOffsetConflict(rootId, 0, 4).isEmpty());
        // Different offset but size eats into a neighbour
        QVERIFY(!m_ctrl->describeOffsetConflict(rootId, 2, 4).isEmpty());
        // Free slot → empty description
        QVERIFY(m_ctrl->describeOffsetConflict(rootId, 0x20, 4).isEmpty());
        // Zero-sized placements never conflict (containers being drafted)
        QVERIFY(m_ctrl->describeOffsetConflict(rootId, 0, 0).isEmpty());
        // Root-level (parentId 0) is exempt
        QVERIFY(m_ctrl->describeOffsetConflict(0, 0, 4).isEmpty());
        // Moving a node onto its own slot is fine (excludeId)
        int fi = -1;
        for (int i = 0; i < m_doc->tree.nodes.size(); i++)
            if (m_doc->tree.nodes[i].name == QStringLiteral("field_u32")) { fi = i; break; }
        QVERIFY(fi >= 0);
        QVERIFY(m_ctrl->describeOffsetConflict(rootId, 0, 4,
                    m_doc->tree.nodes[fi].id).isEmpty());
    }

    // ── Shift-offsets selection collection: array-element / member rows
    //    all decode to the SAME node id, so the collected index list must
    //    be deduped — otherwise the shift delta would apply twice and
    //    double-move the node. Cross-parent selections must be refused.
    void testCollectSameParentIndicesDedupes() {
        uint64_t rootId = m_doc->tree.nodes[0].id;  // TestStruct

        // Add an array field to TestStruct
        rcx::Node arr;
        arr.kind = NodeKind::Array;
        arr.name = QStringLiteral("arr");
        arr.parentId = rootId;
        arr.offset = 0x30;
        arr.arrayLen = 4;
        arr.elementKind = NodeKind::UInt32;
        m_doc->tree.addNode(arr);
        uint64_t arrId = m_doc->tree.nodes.last().id;

        // Two element rows of the same array in one selection
        QSet<uint64_t> sel;
        sel.insert(rcx::makeArrayElemSelId(arrId, 0));
        sel.insert(rcx::makeArrayElemSelId(arrId, 1));

        QVector<int> indices;
        uint64_t parent = 0;
        QVERIFY(m_ctrl->collectSameParentIndices(sel, indices, parent));
        QCOMPARE(indices.size(), 1);              // deduped to the array node
        QCOMPARE(m_doc->tree.nodes[indices[0]].id, arrId);
        QCOMPARE(parent, rootId);

        // Mixed selection: an element row + a real sibling field → same parent
        int fieldIdx = -1;
        for (int i = 0; i < m_doc->tree.nodes.size(); i++)
            if (m_doc->tree.nodes[i].name == QStringLiteral("field_u32")) { fieldIdx = i; break; }
        QVERIFY(fieldIdx >= 0);
        QSet<uint64_t> mixed;
        mixed.insert(rcx::makeArrayElemSelId(arrId, 2));
        mixed.insert(m_doc->tree.nodes[fieldIdx].id);
        QVERIFY(m_ctrl->collectSameParentIndices(mixed, indices, parent));
        QCOMPARE(indices.size(), 2);

        // Cross-parent selection → refused
        rcx::Node other;
        other.kind = NodeKind::Struct;
        other.name = QStringLiteral("Other");
        other.parentId = 0;  // a second top-level class
        other.offset = 0x40;
        m_doc->tree.addNode(other);
        uint64_t otherId = m_doc->tree.nodes.last().id;
        QSet<uint64_t> cross;
        cross.insert(rcx::makeArrayElemSelId(arrId, 0));
        cross.insert(otherId);
        QVERIFY(!m_ctrl->collectSameParentIndices(cross, indices, parent));
    }

    // ── Insert dialog button honesty: a malformed / negative offset /
    //    unknown type must disable OK (nothing committable). A real overlap
    //    does NOT disable it — Insert resolves it by pushing the colliding
    //    siblings down (no more draft escape hatch).
    void testFieldDialogOkDisabledOnBadOffset() {
        using rcx::FieldLayoutDialog;
        FieldLayoutDialog dlg(FieldLayoutDialog::InsertField, 0x10,
                              NodeKind::Hex32, QStringLiteral("f"),
                              [](int off, NodeKind) -> QString {
                                  return off == 0x10
                                      ? QStringLiteral("0x10 is taken")
                                      : QString();
                              },
                              QStringLiteral("Insert Field"));

        auto* offsetEdit = dlg.findChild<QLineEdit*>();
        QVERIFY(offsetEdit);
        QPushButton* ok = nullptr;
        for (auto* b : dlg.findChildren<QPushButton*>()) {
            if (b->text() == QStringLiteral("Insert")) {
                ok = b;
                break;
            }
        }
        QVERIFY(ok);

        // Default offset conflicts → Insert stays enabled (push resolves it)
        QVERIFY(ok->isEnabled());
        QCOMPARE(ok->text(), QStringLiteral("Insert"));

        // Malformed offset → OK disabled
        offsetEdit->setText(QStringLiteral("zzz"));
        QVERIFY(!ok->isEnabled());

        // Negative offset → OK disabled
        offsetEdit->setText(QStringLiteral("-0x4"));
        QVERIFY(!ok->isEnabled());

        // Unknown type text → OK disabled
        offsetEdit->setText(QStringLiteral("0x20"));
        auto* typeCombo = dlg.findChild<QComboBox*>();
        QVERIFY(typeCombo);
        typeCombo->setCurrentText(QStringLiteral("not_a_type"));
        QVERIFY(!ok->isEnabled());

        // Free offset + known type → plain "Insert", enabled
        typeCombo->setCurrentText(QStringLiteral("hex32"));
        QVERIFY(ok->isEnabled());
        QCOMPARE(ok->text(), QStringLiteral("Insert"));
    }

    // ── Push-mode insert plan (Insert Field with a conflicting offset):
    //    an offset landing inside an existing field snaps up to that
    //    field's start, then every sibling at/after the point shifts down
    //    by the new field's size. Free offsets push nothing.
    void testPushAdjustmentsForInsert() {
        uint64_t rootId = m_doc->tree.nodes[0].id;  // TestStruct
        auto offOf = [&](const QString& name) {
            for (int i = 0; i < m_doc->tree.nodes.size(); i++)
                if (m_doc->tree.nodes[i].name == name)
                    return m_doc->tree.nodes[i].offset;
            return -1;
        };
        // Layout: field_u32@0(4), field_float@4(4), field_u8@8(1),
        // pad0@9(2), pad1@11(1), field_hex@12(4).

        // Case 1: exact start of an existing field → no snap, push it and
        // everything after down by the insert size.
        int off = 4;
        auto adjs = m_ctrl->pushAdjustmentsForInsert(rootId, 4, off);
        QCOMPARE(off, 4);                        // offset kept
        QCOMPARE(adjs.size(), 5);                // field_float + u8 + pad0 + pad1 + hex
        QCOMPARE(offOf(QStringLiteral("field_float")), 4);
        QCOMPARE(offOf(QStringLiteral("field_u8")), 8);
        for (const auto& a : adjs) {
            QCOMPARE(a.newOffset, a.oldOffset + 4);
            QVERIFY(a.oldOffset >= 4);
        }

        // Case 2: offset in the middle of a field → snap to that field's
        // start, then push everything from there down.
        off = 2;
        adjs = m_ctrl->pushAdjustmentsForInsert(rootId, 4, off);
        QCOMPARE(off, 0);                        // snapped up to field_u32
        QCOMPARE(adjs.size(), 6);                // every sibling shifts
        for (const auto& a : adjs)
            QCOMPARE(a.newOffset, a.oldOffset + 4);

        // Case 3: free offset → no push at all, offset untouched.
        off = 0x20;
        adjs = m_ctrl->pushAdjustmentsForInsert(rootId, 4, off);
        QVERIFY(adjs.isEmpty());
        QCOMPARE(off, 0x20);

        // Case 4: exact start of a later field, size eats into the next
        // ones → push from that field on (no snap needed).
        off = 8;
        adjs = m_ctrl->pushAdjustmentsForInsert(rootId, 4, off);
        QCOMPARE(off, 8);
        QCOMPARE(adjs.size(), 4);                // u8 + pad0 + pad1 + hex
        for (const auto& a : adjs) {
            QVERIFY(a.oldOffset >= 8);
            QCOMPARE(a.newOffset, a.oldOffset + 4);
        }

        // Case 5: container (size 0) — span unknown at insert time, so a
        // conflicting offset never pushes; the placement stays as-is.
        off = 4;
        adjs = m_ctrl->pushAdjustmentsForInsert(rootId, 0, off);
        QVERIFY(adjs.isEmpty());
        QCOMPARE(off, 4);
    }

    // ── Create Field dialog: overlaps are allowed by design — OK stays
    //    enabled on a conflict (no push, no draft) and the button reads
    //    "Create"; malformed offsets still disable it.
    void testCreateFieldDialogAllowsOverlap() {
        using rcx::FieldLayoutDialog;
        FieldLayoutDialog dlg(FieldLayoutDialog::CreateField, 0x10,
                              NodeKind::Hex32, QStringLiteral("f"),
                              [](int off, NodeKind) -> QString {
                                  return off == 0x10
                                      ? QStringLiteral("0x10 is taken")
                                      : QString();
                              },
                              QStringLiteral("Create Field"));

        auto* offsetEdit = dlg.findChild<QLineEdit*>();
        QVERIFY(offsetEdit);
        QPushButton* ok = nullptr;
        for (auto* b : dlg.findChildren<QPushButton*>()) {
            if (b->text() == QStringLiteral("Create")) { ok = b; break; }
        }
        QVERIFY(ok);

        // Conflict → still committable as "Create" (overlap allowed).
        QVERIFY(ok->isEnabled());
        QCOMPARE(ok->text(), QStringLiteral("Create"));

        // Malformed offset → disabled (nothing committable).
        offsetEdit->setText(QStringLiteral("zzz"));
        QVERIFY(!ok->isEnabled());

        // Free offset → enabled "Create".
        offsetEdit->setText(QStringLiteral("0x20"));
        QVERIFY(ok->isEnabled());
        QCOMPARE(ok->text(), QStringLiteral("Create"));
    }

    // ── Nested Struct dialog: row type combos behave like the Insert Field
    //    dialog's — editable (type-to-filter), height-bounded, and OK is
    //    refused while any row's type text resolves to nothing.
    void testNestedDialogTypeComboEditableBoundedAndValidates() {
        using rcx::NestedStructDialog;
        NestedStructDialog dlg(0);
        auto* tree = dlg.findChild<QTreeWidget*>();
        QVERIFY(tree);
        QVERIFY(tree->topLevelItemCount() >= 1);
        // Column order: Offset | Type | Name | Type name — the combo lives
        // in column 1.
        auto* combo = qobject_cast<QComboBox*>(
            tree->itemWidget(tree->topLevelItem(0), 1));
        QVERIFY(combo);

        // The fix: editable (type-to-filter) + bounded popup height.
        QVERIFY(combo->isEditable());
        QCOMPARE(combo->maxVisibleItems(), 12);

        // The Offset input spans the full row width (grows with the form).
        auto* offEdit = dlg.findChild<QLineEdit*>();
        QVERIFY(offEdit);
        QVERIFY(offEdit->sizePolicy().horizontalPolicy() == QSizePolicy::Expanding);

        // Offset (0) and Type (1) are fixed, Type 25% wider than Offset;
        // Name (2) and Type name (3) both Stretch, splitting the remaining
        // space equally (Qt 6 has no stretch-factor API).
        QCOMPARE(tree->header()->sectionResizeMode(0), QHeaderView::Interactive);
        QCOMPARE(tree->header()->sectionResizeMode(1), QHeaderView::Interactive);
        QCOMPARE(tree->header()->sectionResizeMode(2), QHeaderView::Stretch);
        QCOMPARE(tree->header()->sectionResizeMode(3), QHeaderView::Stretch);
        // Type is 25% wider than Offset.
        QCOMPARE(tree->columnWidth(1) * 4, tree->columnWidth(0) * 5);
        // The seed row packs to offset 0: plain "0" (no 0x prefix),
        // centered in the column — the edit editor stays left-aligned
        // (OffsetEditorDelegate), the contrast is intentional.
        QCOMPARE(tree->topLevelItem(0)->text(0), QStringLiteral("0"));
        QVERIFY(tree->topLevelItem(0)->textAlignment(0) & Qt::AlignHCenter);
        // The column sizes to the combo: the item's SizeHintRole mirrors the
        // combo's hint (live combo sizeHint can drift a pixel or two after
        // reparenting, so check the mechanism, not exact equality).
        const QSize hint = tree->topLevelItem(0)->sizeHint(1);
        QVERIFY(hint.width() > 50 && hint.height() > 15);

        QPushButton* ok = nullptr;
        for (auto* b : dlg.findChildren<QPushButton*>()) {
            if (b->text() == QStringLiteral("Insert")) { ok = b; break; }
        }
        QVERIFY(ok);

        // Ambiguous typed text ("int" matches int8/16/32/64/128) → OK refuses.
        combo->setCurrentText(QStringLiteral("int"));
        ok->click();
        QVERIFY(dlg.result() != QDialog::Accepted);

        // A typed unique prefix/name resolves → OK accepts (same policy as
        // the Insert Field type combo).
        combo->setCurrentText(QStringLiteral("int32_t"));
        ok->click();
        QCOMPARE(dlg.result(), QDialog::Accepted);
    }

    // ── Nested Struct dialog: the Offset column's editor is a
    //    left-aligned QLineEdit that fills the whole cell (the item's
    //    alignment must not leak into it, and it must not shrink to the
    //    content width).
    void testNestedDialogOffsetEditorLeftAlignedAndFullWidth() {
        using rcx::NestedStructDialog;
        NestedStructDialog dlg(0);
        auto* tree = dlg.findChild<QTreeWidget*>();
        QVERIFY(tree);
        QVERIFY(tree->topLevelItemCount() >= 1);

        // The delegate for the Offset column is installed.
        auto* del = tree->itemDelegateForColumn(0);
        QVERIFY(del);

        // Open the editor for the first row's Offset cell and inspect it.
        const QModelIndex idx = tree->model()->index(0, 0);
        QVERIFY(idx.isValid());
        QStyleOptionViewItem opt;
        opt.rect = QRect(0, 0, 200, 30);
        QWidget* ed = del->createEditor(tree->viewport(), opt, idx);
        QVERIFY(ed);
        auto* le = qobject_cast<QLineEdit*>(ed);
        QVERIFY(le);
        // Left-aligned text, not centered.
        QVERIFY(le->alignment() & Qt::AlignLeft);
        QVERIFY(!(le->alignment() & Qt::AlignHCenter));
        // The editor must span the FULL column — from the column's true
        // left edge (x=0, not the 20px branch strip QTreeView reserves in
        // column 0) to its right edge — and keep the row's vertical span.
        del->updateEditorGeometry(le, opt, idx);
        QCOMPARE(le->geometry().left(), 0);
        QCOMPARE(le->geometry().width(), tree->header()->sectionSize(0));
        QCOMPARE(le->geometry().top(), opt.rect.top());
        QCOMPARE(le->geometry().height(), opt.rect.height());
        delete ed;
    }

    // ── Nested Struct dialog: when the user single-clicks the Offset cell,
    //    the live editor must sit flush at the column's start X (QTreeView
    //    otherwise indents it 20px past the branch strip) and span the
    //    whole column width.
    void testNestedDialogOffsetEditorFlushWithColumnStart() {
        using rcx::NestedStructDialog;
        NestedStructDialog dlg(0);
        dlg.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dlg));
        auto* tree = dlg.findChild<QTreeWidget*>();
        QVERIFY(tree);
        QVERIFY(tree->topLevelItemCount() >= 1);
        tree->editItem(tree->topLevelItem(0), 0);
        // The Type column's editable combo has an internal QLineEdit too,
        // so only take a line edit that is a direct child of the viewport.
        QLineEdit* le = nullptr;
        const auto children = tree->viewport()->findChildren<QLineEdit*>();
        for (auto* c : children)
            if (c->parent() == tree->viewport()) { le = c; break; }
        QVERIFY(le);
        // Starts at the column's true left edge — NOT 20px in — and spans
        // the full column width.
        QCOMPARE(le->geometry().left(),
                 tree->header()->sectionViewportPosition(0));
        QCOMPARE(le->geometry().width(), tree->header()->sectionSize(0));
        dlg.close();
    }

    // ── Nested Struct dialog: the displayed offset number is centered in
    //    the FULL Offset column, not in the branch-indented cell (QTreeView
    //    reserves a 20px strip at the left of column 0, which would push
    //    the center 10px right of the column's true center).
    void testNestedDialogOffsetDisplayCenteredInColumn() {
        using rcx::NestedStructDialog;
        NestedStructDialog dlg(0);
        dlg.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dlg));
        auto* tree = dlg.findChild<QTreeWidget*>();
        QVERIFY(tree);
        const QImage img = tree->viewport()->grab().toImage();
        const QColor base = tree->viewport()->palette().color(QPalette::Base);
        const int colW = tree->header()->sectionSize(0);
        const int rowH = tree->visualItemRect(tree->topLevelItem(0)).height();
        // Row 0 occupies y 0..rowH; scan column 0 (x 0..colW) for text
        // pixels and measure the text's horizontal center.
        int minX = -1, maxX = -1;
        for (int y = 0; y < rowH; ++y) {
            for (int x = 0; x < colW; ++x) {
                const QColor c = img.pixelColor(x, y);
                if (qAbs(c.red() - base.red()) > 40 ||
                    qAbs(c.green() - base.green()) > 40 ||
                    qAbs(c.blue() - base.blue()) > 40) {
                    if (minX < 0) minX = x;
                    maxX = x;
                }
            }
        }
        QVERIFY(minX >= 0);  // some text pixels found
        const double center = (minX + maxX) / 2.0;
        // Within 4px of the column's true center (36 for a 72px column).
        QVERIFY2(qAbs(center - colW / 2.0) <= 4.0,
                 qPrintable(QStringLiteral("offset text center %1 vs column center %2")
                                .arg(center).arg(colW / 2.0)));
        dlg.close();
    }

    // ── Nested Struct dialog: an offset cell whose text doesn't parse
    //    must fall back to auto-packing (matching what insert does —
    //    collectChildren only sets offsetManual when the text parses), not
    //    be honored as a manual offset of 0.
    void testNestedDialogInvalidManualOffsetFallsBackToAuto() {
        using rcx::NestedStructDialog;
        NestedStructDialog dlg(0);
        auto* tree = dlg.findChild<QTreeWidget*>();
        QVERIFY(tree);
        auto* item = tree->topLevelItem(0);
        QCOMPARE(item->text(0), QStringLiteral("0"));
        // Fires itemChanged: the handler marks the row manual, then
        // packChildren must reject the unparseable text and re-pack auto.
        item->setText(0, QStringLiteral("zz"));
        QCOMPARE(item->data(0, Qt::UserRole).toBool(), false);
        QCOMPARE(item->text(0), QStringLiteral("0"));   // auto value restored
        QString name, typeName, keyword;
        int offset = 0;
        QVector<rcx::NestedStructSpec> children;
        dlg.collectResult(name, typeName, keyword, offset, children);
        QCOMPARE(children.size(), 1);
        QCOMPARE(children[0].offsetManual, false);
    }

    // ── Nested Struct dialog: a click on the branch strip at the left of
    //    column 0 (the expand/collapse arrow zone) must NOT open the offset
    //    editor; a click inside the cell must.
    void testNestedDialogClickBranchStripDoesNotEdit() {
        using rcx::NestedStructDialog;
        NestedStructDialog dlg(0);
        dlg.show();
        QVERIFY(QTest::qWaitForWindowExposed(&dlg));
        auto* tree = dlg.findChild<QTreeWidget*>();
        QVERIFY(tree);
        auto* viewport = tree->viewport();
        const int cellLeft = tree->visualItemRect(tree->topLevelItem(0)).left();
        QVERIFY(cellLeft > 0);   // branch strip reserved
        // Click in the strip (left of the cell): must not start editing.
        // (The Type column's editable combo has an internal QLineEdit, so
        // only a line edit that is a DIRECT viewport child is an editor.)
        auto directEditor = [&]() {
            const auto les = tree->viewport()->findChildren<QLineEdit*>();
            for (auto* le : les)
                if (le->parent() == viewport) return true;
            return false;
        };
        QTest::mouseClick(viewport, Qt::LeftButton, Qt::NoModifier,
                          QPoint(cellLeft / 2, 12));
        QVERIFY(!directEditor());
        // Click inside the cell: must open the offset editor.
        QTest::mouseClick(viewport, Qt::LeftButton, Qt::NoModifier,
                          QPoint(cellLeft + 10, 12));
        QVERIFY(directEditor());
        dlg.close();
    }

    // ── Nested Struct dialog: free-text typing must NOT prune a container
    //    row's children (only a committed selection does).
    void testNestedDialogTypingDoesNotPruneChildren() {
        using rcx::NestedStructDialog;
        NestedStructDialog dlg(0);
        auto* tree = dlg.findChild<QTreeWidget*>();
        QVERIFY(tree);

        QPushButton* addContainer = nullptr;
        QPushButton* addField = nullptr;
        for (auto* b : dlg.findChildren<QPushButton*>()) {
            if (b->text() == QStringLiteral("Add Nested Struct")) addContainer = b;
            else if (b->text() == QStringLiteral("Add Field"))      addField = b;
        }
        QVERIFY(addContainer && addField);

        addContainer->click();                 // new container row (top-level)
        const int containerIdx = tree->topLevelItemCount() - 1;
        auto* container = tree->topLevelItem(containerIdx);
        QVERIFY(container);
        addField->click();                     // one child under the container
        QCOMPARE(container->childCount(), 1);

        auto* combo = qobject_cast<QComboBox*>(
            tree->itemWidget(container, 1));   // Type column (index 1)
        QVERIFY(combo);

        // Typed text alone never prunes — even when it resolves to a
        // primitive (the row is only pruned on a committed selection, so
        // free typing can't destroy children).
        combo->setCurrentText(QStringLiteral("int"));
        QCOMPARE(container->childCount(), 1);
        combo->setCurrentText(QStringLiteral("uint8_t"));
        QCOMPARE(container->childCount(), 1);

        // A committed selection (a real item chosen from the list) prunes
        // at once — the existing change-type semantics.
        int u8Idx = -1;
        for (int i = 0; i < combo->count(); i++)
            if (combo->itemText(i) == QStringLiteral("uint8_t")) { u8Idx = i; break; }
        QVERIFY(u8Idx >= 0);
        combo->setCurrentIndex(u8Idx);
        QCOMPARE(container->childCount(), 0);

        // Rebuild a child, then switch the row to a primitive by typing:
        // the child survives typing, but OK drops it so the insert matches
        // what is displayed.
        QVERIFY(addField);
        // Back to a container (committed selection) so Add Field nests
        // under the row again.
        int structIdx = -1;
        for (int i = 0; i < combo->count(); i++)
            if (combo->itemText(i) == QStringLiteral("struct")) { structIdx = i; break; }
        QVERIFY(structIdx >= 0);
        combo->setCurrentIndex(structIdx);
        tree->setCurrentItem(container);
        addField->click();
        QCOMPARE(container->childCount(), 1);
        combo->setCurrentText(QStringLiteral("uint8_t"));
        QCOMPARE(container->childCount(), 1);   // typing never prunes

        QPushButton* ok = nullptr;
        for (auto* b : dlg.findChildren<QPushButton*>()) {
            if (b->text() == QStringLiteral("Insert")) { ok = b; break; }
        }
        QVERIFY(ok);
        ok->click();
        QCOMPARE(dlg.result(), QDialog::Accepted);
        QCOMPARE(container->childCount(), 0);   // orphan children dropped at OK
    }

    // ── Nested Struct dialog: an offset typed into the tree's Offset
    //    column (single-click editable) is honored as a manual override —
    //    collectResult carries it through with offsetManual=true instead of
    //    re-packing over it.
    void testNestedDialogHonorsManualOffset() {
        using rcx::NestedStructDialog;
        NestedStructDialog dlg(0);
        auto* tree = dlg.findChild<QTreeWidget*>();
        QVERIFY(tree);
        QVERIFY(tree->topLevelItemCount() >= 1);
        auto* row = tree->topLevelItem(0);

        // Simulate the user typing an offset into the Offset column: the
        // itemChanged handler marks it manual (Qt::UserRole on column 0).
        row->setText(0, QStringLiteral("30"));
        row->setData(0, Qt::UserRole, true);

        QString name, typeName, keyword;
        int offset = -1;
        QVector<rcx::NestedStructSpec> children;
        dlg.collectResult(name, typeName, keyword, offset, children);
        QCOMPARE(children.size(), 1);
        QVERIFY(children[0].offsetManual);
        QCOMPARE(children[0].offset, 0x30);
    }

    // ── Insert Nested Struct honors user-typed (manual) child offsets: a
    //    spec with offsetManual=true keeps its offset verbatim and later
    //    siblings pack after it instead of re-deriving from zero.
    void testInsertNestedStructHonorsManualOffsets() {
        uint64_t rootId = m_doc->tree.nodes[0].id;
        NestedStructSpec a; a.kind = NodeKind::Int32; a.name = "a";
        NestedStructSpec b; b.kind = NodeKind::Int32; b.name = "b";
        b.offset       = 0x30;               // typed by the user
        b.offsetManual = true;
        QVector<NestedStructSpec> kids = {a, b};
        uint64_t memId = m_ctrl->insertNestedStruct(rootId, 0, "Inner", QString(),
                                                    "struct", kids);
        QVERIFY(memId != 0);
        QVector<int> kidsIdx;
        for (int i = 0; i < m_doc->tree.nodes.size(); i++)
            if (m_doc->tree.nodes[i].parentId == memId) kidsIdx.append(i);
        QCOMPARE(kidsIdx.size(), 2);
        const Node& na = m_doc->tree.nodes[kidsIdx[0]];
        const Node& nb = m_doc->tree.nodes[kidsIdx[1]];
        QCOMPARE(na.name, QStringLiteral("a"));
        QCOMPARE(nb.name, QStringLiteral("b"));
        // a auto-packs at 0; b honors the typed 0x30.
        QCOMPARE(na.offset, 0);
        QCOMPARE(nb.offset, 0x30);
    }

    // ── Insert Nested Struct is CREATE semantics: a conflicting offset
    //    places the member as-is (overlap allowed) — no draft flag, no
    //    sibling shifting.
    void testInsertNestedStructNoDraftOnConflict() {
        uint64_t rootId = m_doc->tree.nodes[0].id;
        // field_u32 lives at 0..3 — place the nested member right on top.
        NestedStructSpec x; x.kind = NodeKind::Int32; x.name = "x";
        QVector<NestedStructSpec> kids = {x};
        uint64_t memId = m_ctrl->insertNestedStruct(rootId, 0, "Inner", QString(),
                                                    "struct", kids);
        QVERIFY(memId != 0);
        int mi = m_doc->tree.indexOfId(memId);
        QVERIFY(mi >= 0);
        const Node& member = m_doc->tree.nodes[mi];
        QCOMPARE(member.offset, 0);
        QVERIFY(!member.draft);                  // overlap allowed, no draft
        // Sibling offsets untouched (no push).
        bool u32Intact = false;
        for (const auto& n : m_doc->tree.nodes)
            if (n.name == QStringLiteral("field_u32") && n.offset == 0) u32Intact = true;
        QVERIFY(u32Intact);
    }

    // ── Repro: renaming a field around a virtually-expanded typed pointer
    //    must not duplicate anything across a save/load round trip ──
    //
    // Mirrors a real report: a class whose first field is `NewClass* field_0000`
    // (refId into a second root class, rendered by virtual expansion). Editing
    // field_0000 and saving produced a duplicate.
    //
    // Two distinct renames are exercised, because they hit different nodes:
    //   1. the POINTER field itself (a real child of the outer class)
    //   2. a field inside the referenced class (what the expansion displays —
    //      its LineMeta points at the ref class's own child, shared by every
    //      pointer that references it)
    // After each, the node count and the root-class count must be unchanged:
    // validate(repair) runs on load and re-roots orphans, so a broken parentId
    // would surface as an extra top-level class rather than a lost node.
    void testRenameAroundVirtualPointerDoesNotDuplicateOnSave() {
        // Build into the controller's own document — renameNode() routes
        // through m_ctrl's undo stack, which only sees m_doc.
        RcxDocument* doc = m_doc;
        auto& tree = doc->tree;
        tree.nodes.clear();
        tree.invalidateIdCache();

        Node outer; outer.kind = NodeKind::Struct;
        outer.structTypeName = "OFFSET_EACMAIN"; outer.parentId = 0; outer.collapsed = false;
        const uint64_t outerId = tree.nodes[tree.addNode(outer)].id;

        Node refCls; refCls.kind = NodeKind::Struct;
        refCls.structTypeName = "NewClass"; refCls.parentId = 0; refCls.collapsed = false;
        const uint64_t refId = tree.nodes[tree.addNode(refCls)].id;

        Node r0; r0.kind = NodeKind::UInt64; r0.name = "field_0000";
        r0.parentId = refId; r0.offset = 0;   tree.addNode(r0);
        Node r1; r1.kind = NodeKind::UInt64; r1.name = "field_0008";
        r1.parentId = refId; r1.offset = 8;   tree.addNode(r1);

        // The pointer that virtually expands NewClass.
        Node ptr; ptr.kind = NodeKind::Pointer64; ptr.name = "field_0000";
        ptr.parentId = outerId; ptr.offset = 0; ptr.refId = refId; ptr.collapsed = false;
        const uint64_t ptrId = tree.nodes[tree.addNode(ptr)].id;

        const int nodesBefore = tree.nodes.size();
        auto rootCount = [](const NodeTree& t) {
            int n = 0;
            for (const auto& x : t.nodes) if (x.parentId == 0) n++;
            return n;
        };
        const int rootsBefore = rootCount(tree);

        auto roundTrip = [&](const char* what) {
            QTemporaryFile f;
            QVERIFY2(f.open(), what);
            const QString path = f.fileName();
            f.close();
            QVERIFY2(doc->save(path), what);
            auto* re = new RcxDocument();
            QVERIFY2(re->load(path), what);
            QCOMPARE(re->tree.nodes.size(), nodesBefore);
            QVERIFY2(rootCount(re->tree) == rootsBefore,
                     qPrintable(QString("%1: root-class count changed %2 -> %3")
                                .arg(what).arg(rootsBefore).arg(rootCount(re->tree))));
            // No two siblings may share a name+offset under the same parent.
            for (int i = 0; i < re->tree.nodes.size(); i++) {
                for (int j = i + 1; j < re->tree.nodes.size(); j++) {
                    const auto& a = re->tree.nodes[i];
                    const auto& b = re->tree.nodes[j];
                    if (a.parentId != b.parentId) continue;
                    QVERIFY2(!(a.name == b.name && a.offset == b.offset && !a.name.isEmpty()),
                             qPrintable(QString("%1: duplicate sibling '%2' @+0x%3")
                                        .arg(what).arg(a.name).arg(a.offset, 0, 16)));
                }
            }
            delete re;
        };

        roundTrip("baseline, before any edit");

        // 1. rename the pointer field itself
        int ptrIdx = tree.indexOfId(ptrId);
        QVERIFY(ptrIdx >= 0);
        m_ctrl->renameNode(ptrIdx, QStringLiteral("m_head"));
        QCOMPARE(tree.nodes.size(), nodesBefore);
        roundTrip("after renaming the pointer field");

        // 2. rename a field inside the referenced class
        int innerIdx = -1;
        for (int i = 0; i < tree.nodes.size(); i++)
            if (tree.nodes[i].parentId == refId && tree.nodes[i].offset == 0) innerIdx = i;
        QVERIFY(innerIdx >= 0);
        m_ctrl->renameNode(innerIdx, QStringLiteral("m_vtable"));
        QCOMPARE(tree.nodes.size(), nodesBefore);
        roundTrip("after renaming a field inside the referenced class");
    }

    // ── Test: expanded-pointer footer carries append pills that target the
    //    REFERENCED class (not the 8-byte pointer → no invalid child) ──
    void testExpandedPointerFooterPillsTargetRefClass() {
        m_doc->tree.nodes.clear();
        m_doc->tree.invalidateIdCache();
        Node root; root.kind = NodeKind::Struct; root.structTypeName = "Root";
        root.parentId = 0; root.collapsed = false;
        uint64_t rootId = m_doc->tree.nodes[m_doc->tree.addNode(root)].id;
        Node cls; cls.kind = NodeKind::Struct; cls.structTypeName = "C"; cls.parentId = 0;
        uint64_t clsId = m_doc->tree.nodes[m_doc->tree.addNode(cls)].id;
        Node c0; c0.kind = NodeKind::Hex64; c0.name = "c0"; c0.parentId = clsId; c0.offset = 0;
        m_doc->tree.addNode(c0);
        Node c1; c1.kind = NodeKind::Hex64; c1.name = "c1"; c1.parentId = clsId; c1.offset = 8;
        m_doc->tree.addNode(c1);
        Node p; p.kind = NodeKind::Pointer64; p.name = "p"; p.parentId = rootId; p.offset = 0;
        p.refId = clsId; p.collapsed = false;  // EXPANDED inline
        uint64_t pId = m_doc->tree.nodes[m_doc->tree.addNode(p)].id;

        m_ctrl->setViewRootId(rootId);
        m_ctrl->refresh();

        // The expanded pointer's footer line carries the pills.
        bool footerHasPills = false;
        const ComposeResult& res = m_ctrl->lastResult();
        const QStringList lines = res.text.split('\n');
        for (int i = 0; i < res.meta.size(); ++i)
            if (res.meta[i].lineKind == LineKind::Footer
                && res.meta[i].nodeKind == NodeKind::Pointer64
                && i < lines.size()
                && lines[i].contains(QStringLiteral("+1 +10h +100h +1000h Trim Top")))
                footerHasPills = true;
        QVERIFY2(footerHasPills, "expanded pointer footer should carry the append pills");

        // "+1" targets the refId class, NOT the pointer (no invalid child).
        int before = m_doc->tree.childrenOf(clsId).size();
        emit m_editor->appendSingleFieldRequested(pId);
        QCOMPARE(m_doc->tree.childrenOf(clsId).size(), before + 1);
        QVERIFY2(m_doc->tree.childrenOf(pId).isEmpty(),
                 "pointer must NOT receive a child");

        // "+10h" (appendBytes) also targets the refId class.
        int before2 = m_doc->tree.childrenOf(clsId).size();
        emit m_editor->appendBytesRequested(pId, 0x10);
        QVERIFY2(m_doc->tree.childrenOf(clsId).size() > before2,
                 "appendBytes should grow the refId class");
        QVERIFY2(m_doc->tree.childrenOf(pId).isEmpty(),
                 "pointer must still have no child");
    }

    // ── Test: setNodeValue writes bytes to provider ──
    void testSetNodeValueWritesData() {
        // Find field_u32 (index 1, child of root at index 0)
        int idx = -1;
        for (int i = 0; i < m_doc->tree.nodes.size(); i++) {
            if (m_doc->tree.nodes[i].name == "field_u32") { idx = i; break; }
        }
        QVERIFY(idx >= 0);

        // Verify original value in provider
        uint64_t addr = m_doc->tree.computeOffset(idx);
        QByteArray origBytes = m_doc->provider->readBytes(addr, 4);
        uint32_t origVal;
        memcpy(&origVal, origBytes.data(), 4);
        QCOMPARE(origVal, (uint32_t)0xDEADBEEF);

        // Write new value "42" (decimal)
        m_ctrl->setNodeValue(idx, 0, "42");
        QApplication::processEvents();

        // Read back: should be 42 in little-endian
        QByteArray newBytes = m_doc->provider->readBytes(addr, 4);
        uint32_t newVal;
        memcpy(&newVal, newBytes.data(), 4);
        QCOMPARE(newVal, (uint32_t)42);
    }

    // ── Test: setNodeValue undo/redo restores data ──
    void testSetNodeValueUndoRedo() {
        int idx = -1;
        for (int i = 0; i < m_doc->tree.nodes.size(); i++) {
            if (m_doc->tree.nodes[i].name == "field_u32") { idx = i; break; }
        }
        QVERIFY(idx >= 0);
        uint64_t addr = m_doc->tree.computeOffset(idx);

        // Original: 0xDEADBEEF
        QByteArray orig = m_doc->provider->readBytes(addr, 4);
        uint32_t origVal;
        memcpy(&origVal, orig.data(), 4);
        QCOMPARE(origVal, (uint32_t)0xDEADBEEF);

        // Write new value
        m_ctrl->setNodeValue(idx, 0, "99");
        QApplication::processEvents();

        uint32_t newVal;
        QByteArray after = m_doc->provider->readBytes(addr, 4);
        memcpy(&newVal, after.data(), 4);
        QCOMPARE(newVal, (uint32_t)99);

        // Undo → should restore original
        m_doc->undoStack.undo();
        QApplication::processEvents();

        QByteArray undone = m_doc->provider->readBytes(addr, 4);
        uint32_t undoneVal;
        memcpy(&undoneVal, undone.data(), 4);
        QCOMPARE(undoneVal, (uint32_t)0xDEADBEEF);

        // Redo → should restore new value
        m_doc->undoStack.redo();
        QApplication::processEvents();

        QByteArray redone = m_doc->provider->readBytes(addr, 4);
        uint32_t redoneVal;
        memcpy(&redoneVal, redone.data(), 4);
        QCOMPARE(redoneVal, (uint32_t)99);
    }

    // ── Test: setNodeValue on Float field ──
    void testSetNodeValueFloat() {
        int idx = -1;
        for (int i = 0; i < m_doc->tree.nodes.size(); i++) {
            if (m_doc->tree.nodes[i].name == "field_float") { idx = i; break; }
        }
        QVERIFY(idx >= 0);
        uint64_t addr = m_doc->tree.computeOffset(idx);

        // Original: 3.14f
        QByteArray orig = m_doc->provider->readBytes(addr, 4);
        float origVal;
        memcpy(&origVal, orig.data(), 4);
        QVERIFY(qAbs(origVal - 3.14f) < 0.01f);

        // Write "1.5"
        m_ctrl->setNodeValue(idx, 0, "1.5");
        QApplication::processEvents();

        QByteArray after = m_doc->provider->readBytes(addr, 4);
        float newVal;
        memcpy(&newVal, after.data(), 4);
        QCOMPARE(newVal, 1.5f);

        // Undo
        m_doc->undoStack.undo();
        QApplication::processEvents();
        QByteArray undone = m_doc->provider->readBytes(addr, 4);
        float undoneVal;
        memcpy(&undoneVal, undone.data(), 4);
        QVERIFY(qAbs(undoneVal - 3.14f) < 0.01f);
    }

    // ── Test: renameNode changes name and undo restores ──
    void testRenameNode() {
        int idx = -1;
        for (int i = 0; i < m_doc->tree.nodes.size(); i++) {
            if (m_doc->tree.nodes[i].name == "field_u32") { idx = i; break; }
        }
        QVERIFY(idx >= 0);
        QCOMPARE(m_doc->tree.nodes[idx].name, QString("field_u32"));

        m_ctrl->renameNode(idx, "myRenamedField");
        QApplication::processEvents();

        QCOMPARE(m_doc->tree.nodes[idx].name, QString("myRenamedField"));

        // Undo
        m_doc->undoStack.undo();
        QApplication::processEvents();
        QCOMPARE(m_doc->tree.nodes[idx].name, QString("field_u32"));

        // Redo
        m_doc->undoStack.redo();
        QApplication::processEvents();
        QCOMPARE(m_doc->tree.nodes[idx].name, QString("myRenamedField"));
    }

    // ── Test: changeNodeKind changes type and undo restores ──
    void testChangeNodeKind() {
        int idx = -1;
        for (int i = 0; i < m_doc->tree.nodes.size(); i++) {
            if (m_doc->tree.nodes[i].name == "field_u32") { idx = i; break; }
        }
        QVERIFY(idx >= 0);
        QCOMPARE(m_doc->tree.nodes[idx].kind, NodeKind::UInt32);

        m_ctrl->changeNodeKind(idx, NodeKind::Float);
        QApplication::processEvents();

        QCOMPARE(m_doc->tree.nodes[idx].kind, NodeKind::Float);

        // Undo
        m_doc->undoStack.undo();
        QApplication::processEvents();
        QCOMPARE(m_doc->tree.nodes[idx].kind, NodeKind::UInt32);
    }

    // ── A shrink-split type change clears the selection (so a single click
    //    doesn't immediately re-open the type chooser), while a same-size
    //    change keeps the node selected. User: "auto highlights the two
    //    emitted nodes ... triggers single-click typechooser too easily". ──
    void testHexSplitClearsSelection() {
        auto* doc = new RcxDocument();
        doc->tree.baseAddress = 0;
        Node root; root.kind = NodeKind::Struct; root.name = "R"; root.parentId = 0;
        root.offset = 0;
        int ri = doc->tree.addNode(root);
        uint64_t rootId = doc->tree.nodes[ri].id;
        Node h; h.kind = NodeKind::Hex64; h.name = "blob"; h.parentId = rootId;
        h.offset = 0;
        int hi = doc->tree.addNode(h);
        uint64_t hexId = doc->tree.nodes[hi].id;
        doc->provider = std::make_unique<BufferProvider>(QByteArray(32, '\0'));

        auto* splitter = new QSplitter();
        auto* ctrl = new RcxController(doc, nullptr);
        auto* ed = ctrl->addSplitEditor(splitter);
        splitter->resize(800, 600);
        splitter->show();
        QVERIFY(QTest::qWaitForWindowExposed(splitter));
        ctrl->refresh();
        QApplication::processEvents();

        auto lineOf = [&](uint64_t id) {
            for (int i = 0; i < 60; ++i) {
                const LineMeta* lm = ed->metaForLine(i);
                if (lm && lm->nodeId == id && lm->lineKind == LineKind::Field) return i;
            }
            return -1;
        };

        // Select the Hex64, then shrink to Hex32 (split) → selection cleared.
        int hexLine = lineOf(hexId);
        QVERIFY2(hexLine >= 0, "Hex64 row not found");
        ctrl->handleNodeClick(ed, hexLine, hexId, Qt::NoModifier);
        QCOMPARE(ctrl->selectedIds().size(), 1);

        int idx = doc->tree.indexOfId(hexId);
        ctrl->changeNodeKind(idx, NodeKind::Hex32);
        QApplication::processEvents();
        QCOMPARE(ctrl->selectedIds().size(), 0);   // split clears selection

        // Re-select the (now Hex32) node, then a SAME-size change keeps it.
        int hex32Line = lineOf(hexId);
        QVERIFY2(hex32Line >= 0, "Hex32 row not found after split");
        ctrl->handleNodeClick(ed, hex32Line, hexId, Qt::NoModifier);
        QCOMPARE(ctrl->selectedIds().size(), 1);
        idx = doc->tree.indexOfId(hexId);
        ctrl->changeNodeKind(idx, NodeKind::Int32);   // 4→4 bytes, no split
        QApplication::processEvents();
        QCOMPARE(ctrl->selectedIds().size(), 1);      // same-size keeps selection

        delete ctrl;
        delete splitter;
        delete doc;
    }

    // ── A shrink-split must NOT spuriously heat the shrunk node + the new
    //    pad. After the split the value history is reset (new pad id / cleared
    //    history), so the first read records ONE value (heatLevel 0). On
    //    static bytes repeated refreshes record nothing further, so heat
    //    stays 0 — no "two highlighted rows" from false heat. ──
    void testHexSplitNoFalseHeat() {
        auto* doc = new RcxDocument();
        doc->tree.baseAddress = 0;
        Node root; root.kind = NodeKind::Struct; root.name = "R"; root.parentId = 0;
        root.offset = 0;
        int ri = doc->tree.addNode(root);
        uint64_t rootId = doc->tree.nodes[ri].id;
        Node h; h.kind = NodeKind::Hex64; h.name = "blob"; h.parentId = rootId;
        h.offset = 0;
        int hi = doc->tree.addNode(h);
        uint64_t hexId = doc->tree.nodes[hi].id;
        // Non-zero, varied bytes so the value isn't empty (still constant).
        QByteArray buf(32, '\0');
        for (int i = 0; i < 16; ++i) buf[i] = char(0x10 + i);
        doc->provider = std::make_unique<BufferProvider>(buf);

        auto* splitter = new QSplitter();
        auto* ctrl = new RcxController(doc, nullptr);
        auto* ed = ctrl->addSplitEditor(splitter);
        splitter->resize(800, 600);
        splitter->show();
        QVERIFY(QTest::qWaitForWindowExposed(splitter));
        ctrl->refresh();
        QApplication::processEvents();

        int idx = doc->tree.indexOfId(hexId);
        ctrl->changeNodeKind(idx, NodeKind::Hex32);   // split → Hex32 + Hex32 pad
        QApplication::processEvents();
        // A couple more refreshes on identical bytes — heat must stay flat.
        ctrl->refresh(); QApplication::processEvents();
        ctrl->refresh(); QApplication::processEvents();

        // Every Field row must have heatLevel 0 and dataChanged false on
        // unchanging bytes (the shrunk node + pad included).
        for (int i = 0; i < 60; ++i) {
            const LineMeta* lm = ed->metaForLine(i);
            if (!lm || lm->lineKind != LineKind::Field) continue;
            QVERIFY2(lm->heatLevel == 0,
                     qPrintable(QString("row %1 nodeId %2 heatLevel %3 (expected 0)")
                                .arg(i).arg(lm->nodeId).arg(lm->heatLevel)));
            QVERIFY2(!lm->dataChanged,
                     qPrintable(QString("row %1 dataChanged on static bytes").arg(i)));
        }

        delete ctrl;
        delete splitter;
        delete doc;
    }

    // ── Test: insertNode adds a node, removeNode removes it, undo restores ──
    void testInsertAndRemoveNode() {
        int origSize = m_doc->tree.nodes.size();
        uint64_t rootId = m_doc->tree.nodes[0].id;

        // Insert a new Hex64 at offset 16
        m_ctrl->insertNode(rootId, 16, NodeKind::Hex64, "newHex");
        QApplication::processEvents();

        QCOMPARE(m_doc->tree.nodes.size(), origSize + 1);

        // Find the inserted node
        int newIdx = -1;
        for (int i = 0; i < m_doc->tree.nodes.size(); i++) {
            if (m_doc->tree.nodes[i].name == "newHex") { newIdx = i; break; }
        }
        QVERIFY(newIdx >= 0);
        QCOMPARE(m_doc->tree.nodes[newIdx].kind, NodeKind::Hex64);
        QCOMPARE(m_doc->tree.nodes[newIdx].offset, 16);

        // Remove it
        m_ctrl->removeNode(newIdx);
        QApplication::processEvents();

        QCOMPARE(m_doc->tree.nodes.size(), origSize);

        // Undo remove → node restored
        m_doc->undoStack.undo();
        QApplication::processEvents();

        QCOMPARE(m_doc->tree.nodes.size(), origSize + 1);

        // Find again
        newIdx = -1;
        for (int i = 0; i < m_doc->tree.nodes.size(); i++) {
            if (m_doc->tree.nodes[i].name == "newHex") { newIdx = i; break; }
        }
        QVERIFY(newIdx >= 0);
    }

    // ── Test: setNodeValue on a Struct-kind enum ref (the shape the type
    //    chooser produces for an enum pick) writes through the field's
    //    elementKind width — without the redirect it would parse against
    //    Struct (size 0) and silently no-op. Also covers the fallback to
    //    the enum's own underlying kind when the field's elementKind isn't
    //    an integer kind.
    void testSetNodeValueStructEnumRef() {
        auto* doc = new RcxDocument();
        doc->tree.baseAddress = 0;

        Node enumNode;
        enumNode.kind = NodeKind::Struct;
        enumNode.classKeyword = QStringLiteral("enum");
        enumNode.structTypeName = QStringLiteral("XmlObjectID");
        enumNode.name = QStringLiteral("UnnamedEnum2");
        enumNode.elementKind = NodeKind::UInt8;
        enumNode.enumMembers = {
            {QStringLiteral("ID_NONE"), 0},
            {QStringLiteral("ID_PC"),   1},
            {QStringLiteral("ID_NPC"),  2},
        };
        int ei = doc->tree.addNode(enumNode);
        uint64_t enumId = doc->tree.nodes[ei].id;

        Node root; root.kind = NodeKind::Struct; root.name = "R";
        root.parentId = 0; root.offset = 0;
        int ri = doc->tree.addNode(root);
        uint64_t rootId = doc->tree.nodes[ri].id;

        Node field;
        field.kind = NodeKind::Struct;
        field.name = QStringLiteral("m_eID");
        field.parentId = rootId;
        field.offset = 8;
        field.refId = enumId;
        field.structTypeName = QStringLiteral("XmlObjectID");
        // elementKind left at Struct on purpose: exercises the fallback to
        // the enum's underlying UInt8 (the chooser usually sets UInt8, but
        // legacy/foreign files can omit it).
        field.elementKind = NodeKind::Struct;
        int fi = doc->tree.addNode(field);
        QVERIFY(fi >= 0);

        QByteArray buf(32, '\0');
        buf[8] = 1;  // ID_PC
        doc->provider = std::make_unique<BufferProvider>(buf);

        auto* splitter = new QSplitter();
        auto* ctrl = new RcxController(doc, nullptr);
        ctrl->setWindowState(false, false);
        ctrl->addSplitEditor(splitter);

        // Commit a picker selection (what the enum pill's onChosen does).
        ctrl->setNodeValue(fi, 0, QString::number(2));
        QApplication::processEvents();
        QCOMPARE(doc->provider->readBytes(8, 1)[0], (char)2);
        QCOMPARE(doc->provider->readBytes(9, 1)[0], (char)0);  // no overflow

        // Undo restores the original byte.
        doc->undoStack.undo();
        QApplication::processEvents();
        QCOMPARE(doc->provider->readBytes(8, 1)[0], (char)1);

        delete ctrl;
        delete splitter;
        delete doc;
    }

    // ── Test: setNodeValue with Hex32 (space-separated hex bytes) ──
    void testSetNodeValueHex() {
        int idx = -1;
        for (int i = 0; i < m_doc->tree.nodes.size(); i++) {
            if (m_doc->tree.nodes[i].name == "field_hex") { idx = i; break; }
        }
        QVERIFY(idx >= 0);
        uint64_t addr = m_doc->tree.computeOffset(idx);

        // Original: 0xCAFEBABE
        QByteArray orig = m_doc->provider->readBytes(addr, 4);
        uint32_t origVal;
        memcpy(&origVal, orig.data(), 4);
        QCOMPARE(origVal, (uint32_t)0xCAFEBABE);

        // Write space-separated hex bytes "AA BB CC DD"
        m_ctrl->setNodeValue(idx, 0, "AA BB CC DD");
        QApplication::processEvents();

        QByteArray after = m_doc->provider->readBytes(addr, 4);
        QCOMPARE((uint8_t)after[0], (uint8_t)0xAA);
        QCOMPARE((uint8_t)after[1], (uint8_t)0xBB);
        QCOMPARE((uint8_t)after[2], (uint8_t)0xCC);
        QCOMPARE((uint8_t)after[3], (uint8_t)0xDD);

        // Undo
        m_doc->undoStack.undo();
        QApplication::processEvents();
        QByteArray undone = m_doc->provider->readBytes(addr, 4);
        uint32_t undoneVal;
        memcpy(&undoneVal, undone.data(), 4);
        QCOMPARE(undoneVal, (uint32_t)0xCAFEBABE);
    }

    // ── Test: full inline edit round-trip (type in editor → commit → verify provider) ──
    void testInlineEditRoundTrip() {
        // Refresh to get composed output
        m_ctrl->refresh();
        QApplication::processEvents();

        // Find field_u8 line (UInt8 at offset 8, value = 0x42 = 66)
        ComposeResult result = m_doc->compose();
        int fieldLine = -1;
        for (int i = 0; i < result.meta.size(); i++) {
            if (result.meta[i].nodeKind == NodeKind::UInt8 &&
                result.meta[i].lineKind == LineKind::Field) {
                fieldLine = i;
                break;
            }
        }
        QVERIFY(fieldLine >= 0);

        m_editor->applyDocument(result);
        QApplication::processEvents();

        // Select this node so edit is allowed
        uint64_t nodeId = result.meta[fieldLine].nodeId;
        QSet<uint64_t> sel;
        sel.insert(nodeId);
        m_editor->applySelectionOverlay(sel);
        QApplication::processEvents();

        // Begin value edit
        bool ok = m_editor->beginInlineEdit(EditTarget::Value, fieldLine);
        QVERIFY2(ok, "Should be able to begin value edit on UInt8 field");
        QVERIFY(m_editor->isEditing());

        // UInt8 values display in hex (e.g., "0x42"). beginInlineEdit selects
        // the value text. Replace it directly via Scintilla API (sendEvent with
        // key presses doesn't reliably reach QScintilla in headless test mode).
        {
            QByteArray replacement = QByteArrayLiteral("0xFF");
            m_editor->scintilla()->SendScintilla(
                QsciScintillaBase::SCI_REPLACESEL,
                (uintptr_t)0, replacement.constData());
        }
        QApplication::processEvents();

        // Commit
        QSignalSpy spy(m_editor, &RcxEditor::inlineEditCommitted);
        QKeyEvent enter(QEvent::KeyPress, Qt::Key_Return, Qt::NoModifier);
        QApplication::sendEvent(m_editor->scintilla(), &enter);

        QCOMPARE(spy.count(), 1);
        QList<QVariant> args = spy.first();
        int nodeIdx = args.at(0).toInt();
        QString text = args.at(3).toString().trimmed();
        QVERIFY2(text.contains("FF", Qt::CaseInsensitive),
                 qPrintable(QString("Expected '0xFF', got '%1'").arg(text)));

        // Now simulate what controller does: setNodeValue
        m_ctrl->setNodeValue(nodeIdx, 0, text);
        QApplication::processEvents();

        // Verify provider data changed
        int u8Idx = -1;
        for (int i = 0; i < m_doc->tree.nodes.size(); i++) {
            if (m_doc->tree.nodes[i].name == "field_u8") { u8Idx = i; break; }
        }
        QVERIFY(u8Idx >= 0);
        uint64_t addr = m_doc->tree.computeOffset(u8Idx);
        QByteArray bytes = m_doc->provider->readBytes(addr, 1);
        QCOMPARE((uint8_t)bytes[0], (uint8_t)0xFF);
    }

    // ── Test: source switch preserves existing base address ──
    void testSourceSwitchPreservesBase() {
        // Set a non-zero baseAddress to simulate a loaded .rcx file
        m_doc->tree.baseAddress = 0x1000;
        QCOMPARE(m_doc->tree.baseAddress, (uint64_t)0x1000);

        // Simulate attaching a new provider whose base differs (e.g. 0x400000)
        auto prov = std::make_shared<BaseAwareProvider>(makeSmallBuffer(), 0x400000);
        uint64_t newBase = prov->base();
        QCOMPARE(newBase, (uint64_t)0x400000);

        m_doc->provider = prov;
        // Controller logic: keep existing baseAddress when non-zero
        if (m_doc->tree.baseAddress == 0)
            m_doc->tree.baseAddress = newBase;

        // baseAddress must stay at the original value
        QCOMPARE(m_doc->tree.baseAddress, (uint64_t)0x1000);
        // provider base is unchanged (no setBase sync) — provider reports its own initial base
        QCOMPARE(m_doc->provider->base(), (uint64_t)0x400000);
    }

    // ── Test: source switch on fresh doc uses provider default ──
    void testSourceSwitchFreshDocUsesProviderBase() {
        // Simulate a fresh document (no loaded .rcx → baseAddress == 0)
        m_doc->tree.baseAddress = 0;

        auto prov = std::make_shared<BaseAwareProvider>(makeSmallBuffer(), 0x7FFE0000);
        uint64_t newBase = prov->base();

        m_doc->provider = prov;
        if (m_doc->tree.baseAddress == 0)
            m_doc->tree.baseAddress = newBase;

        // Fresh doc should adopt the provider's default base
        QCOMPARE(m_doc->tree.baseAddress, (uint64_t)0x7FFE0000);
    }

    // ── Test: toggleCollapse + undo ──
    void testToggleCollapse() {
        // Root is index 0, a Struct node
        QCOMPARE(m_doc->tree.nodes[0].kind, NodeKind::Struct);
        QCOMPARE(m_doc->tree.nodes[0].collapsed, false);

        m_ctrl->toggleCollapse(0);
        QApplication::processEvents();
        QCOMPARE(m_doc->tree.nodes[0].collapsed, true);

        m_ctrl->toggleCollapse(0);
        QApplication::processEvents();
        QCOMPARE(m_doc->tree.nodes[0].collapsed, false);

        // Undo twice: uncollapse → collapse → original (false)
        m_doc->undoStack.undo();
        QApplication::processEvents();
        QCOMPARE(m_doc->tree.nodes[0].collapsed, true);

        m_doc->undoStack.undo();
        QApplication::processEvents();
        QCOMPARE(m_doc->tree.nodes[0].collapsed, false);
    }
    // ── Test: value history popup only appears during inline editing ──
    void testValueHistoryPopupOnlyDuringEdit() {
        // Record value history for field_u32 so it has heat
        auto& tree = m_doc->tree;
        int idx = -1;
        for (int i = 0; i < tree.nodes.size(); i++) {
            if (tree.nodes[i].name == "field_u32") { idx = i; break; }
        }
        QVERIFY(idx >= 0);
        uint64_t nodeId = tree.nodes[idx].id;

        QHash<uint64_t, ValueHistory> history;
        history[nodeId].record("100");
        history[nodeId].record("200");
        history[nodeId].record("300");
        QVERIFY(history[nodeId].uniqueCount() > 1);

        m_editor->setValueHistoryRef(&history);

        // Refresh and compose so editor has meta with heatLevel
        m_ctrl->refresh();
        QApplication::processEvents();
        ComposeResult result = m_doc->compose();
        // Manually set heat on the node's line meta
        for (auto& lm : result.meta) {
            if (lm.nodeId == nodeId) lm.heatLevel = 2;
        }
        m_editor->applyDocument(result);
        QApplication::processEvents();

        // Popup should not exist or not be visible (no editing active)
        auto* popup = m_editor->findChild<QWidget*>(QString(), Qt::FindDirectChildrenOnly);
        // Even if popup widget exists, it should not be visible
        bool popupVisible = false;
        for (auto* child : m_editor->findChildren<QFrame*>(QString(), Qt::FindDirectChildrenOnly)) {
            if (child->isVisible() && child->windowFlags() & Qt::ToolTip)
                popupVisible = true;
        }
        QVERIFY2(!popupVisible, "Popup should not be visible when not editing");

        // Start inline edit on value column of field_u32
        int fieldLine = -1;
        for (int i = 0; i < result.meta.size(); i++) {
            if (result.meta[i].nodeId == nodeId && result.meta[i].lineKind == LineKind::Field) {
                fieldLine = i; break;
            }
        }
        QVERIFY(fieldLine >= 0);

        bool ok = m_editor->beginInlineEdit(EditTarget::Value, fieldLine);
        QVERIFY(ok);
        QVERIFY(m_editor->isEditing());

        // Trigger hover cursor update (simulates mouse move during editing)
        QApplication::processEvents();

        // Cancel edit to clean up
        m_editor->cancelInlineEdit();
        QApplication::processEvents();

        m_editor->setValueHistoryRef(nullptr);
    }

    // ── Test: delete node clears value history for shifted siblings ──
    void testDeleteClearsHeatForShiftedNodes() {
        // Replace with a live provider so refresh() actually records values
        m_doc->provider = std::make_unique<BaseAwareProvider>(makeSmallBuffer(), 0x1000);
        m_ctrl->refresh();
        QApplication::processEvents();

        auto& tree = m_doc->tree;

        // Locate field_u32 (the node we'll delete) and the siblings after it.
        // The small tree has: field_u32(0), field_float(4), field_u8(8),
        //                     pad0/Hex16(9), pad1/Hex8(11), field_hex/Hex32(12)
        // field_float and field_u8 are regular (non-hex) types.
        int delIdx = -1;
        for (int i = 0; i < tree.nodes.size(); i++) {
            if (tree.nodes[i].name == "field_u32") { delIdx = i; break; }
        }
        QVERIFY(delIdx >= 0);
        uint64_t delId = tree.nodes[delIdx].id;

        // Collect sibling node IDs that come after field_u32 (will be shifted)
        uint64_t parentId = tree.nodes[delIdx].parentId;
        int deletedSize = tree.nodes[delIdx].byteSize(); // 4 bytes
        int deletedEnd = tree.nodes[delIdx].offset + deletedSize;
        QVector<uint64_t> shiftedIds;
        QHash<uint64_t, QString> nameMap;  // for debug messages
        for (int i = 0; i < tree.nodes.size(); i++) {
            if (tree.nodes[i].parentId == parentId && i != delIdx
                && tree.nodes[i].offset >= deletedEnd) {
                shiftedIds.append(tree.nodes[i].id);
                nameMap[tree.nodes[i].id] = tree.nodes[i].name;
            }
        }
        QVERIFY2(!shiftedIds.isEmpty(), "Should have siblings after field_u32");

        // Seed value history for shifted siblings (simulate accumulated heat)
        auto& history = const_cast<QHash<uint64_t, ValueHistory>&>(m_ctrl->valueHistory());
        for (uint64_t id : shiftedIds) {
            history[id].record("old_val_1");
            history[id].record("old_val_2");
            history[id].record("old_val_3");
            QVERIFY2(history[id].heatLevel() >= 2,
                     qPrintable(QString("Pre-delete: %1 should have heat>=2")
                                .arg(nameMap[id])));
        }

        // Also seed the to-be-deleted node
        history[delId].record("del_1");
        history[delId].record("del_2");
        QVERIFY(history.contains(delId));

        // Delete field_u32 — this shifts all subsequent siblings
        m_ctrl->removeNode(delIdx);
        QApplication::processEvents();

        // The deleted node's history should be gone
        QVERIFY2(!m_ctrl->valueHistory().contains(delId),
                 "Deleted node's value history should be cleared");

        // All shifted siblings should have heat=0 after the delete.
        // With a live provider, refresh() inside removeNode re-records one new
        // value at the new offset → count=1 → heatLevel=0.
        for (uint64_t id : shiftedIds) {
            int heat = m_ctrl->valueHistory().contains(id)
                ? m_ctrl->valueHistory()[id].heatLevel() : 0;
            QVERIFY2(heat == 0,
                     qPrintable(QString("Shifted node '%1' (id=%2) should have heat=0, got %3")
                                .arg(nameMap[id]).arg(id).arg(heat)));
        }
    }

    // ── Test: value history records and cycles correctly ──
    void testValueHistoryRingBuffer() {
        ValueHistory vh;
        QCOMPARE(vh.count, 0);
        QCOMPARE(vh.heatLevel(), 0);

        vh.record("10");
        QCOMPARE(vh.count, 1);
        QCOMPARE(vh.heatLevel(), 0);  // 1 unique = static

        // Duplicate should not increase count
        vh.record("10");
        QCOMPARE(vh.count, 1);

        vh.record("20");
        QCOMPARE(vh.count, 2);
        QCOMPARE(vh.heatLevel(), 1);  // cold

        vh.record("30");
        QCOMPARE(vh.count, 3);
        QCOMPARE(vh.heatLevel(), 2);  // warm

        vh.record("40");
        vh.record("50");
        QCOMPARE(vh.count, 5);
        QCOMPARE(vh.heatLevel(), 3);  // hot

        QCOMPARE(vh.last(), QString("50"));

        // Ring buffer: uniqueCount() caps at kCapacity
        for (int i = 0; i < 20; i++)
            vh.record(QString::number(100 + i));
        QCOMPARE(vh.uniqueCount(), ValueHistory::kCapacity);
        QVERIFY(vh.count > ValueHistory::kCapacity);

        // forEach iterates oldest→newest within ring
        QStringList vals;
        vh.forEach([&](const QString& v) { vals.append(v); });
        QCOMPARE(vals.size(), ValueHistory::kCapacity);
        QCOMPARE(vals.last(), vh.last());
    }
    // ── Test: inline edit "int32_t[4]" on primitive converts to array ──
    void testInlineEditPrimitiveArray() {
        // Find a primitive field to convert
        int idx = -1;
        for (int i = 0; i < m_doc->tree.nodes.size(); i++) {
            if (m_doc->tree.nodes[i].name == "field_u32") { idx = i; break; }
        }
        QVERIFY(idx >= 0);
        QCOMPARE(m_doc->tree.nodes[idx].kind, NodeKind::UInt32);
        uint64_t nodeId = m_doc->tree.nodes[idx].id;

        // Emit inlineEditCommitted with array syntax
        emit m_editor->inlineEditCommitted(idx, 0, EditTarget::Type,
                                           QStringLiteral("int32_t[4]"));
        QApplication::processEvents();

        // Node should now be an Array with elementKind=Int32, arrayLen=4
        int newIdx = m_doc->tree.indexOfId(nodeId);
        QVERIFY(newIdx >= 0);
        QCOMPARE(m_doc->tree.nodes[newIdx].kind, NodeKind::Array);
        QCOMPARE(m_doc->tree.nodes[newIdx].elementKind, NodeKind::Int32);
        QCOMPARE(m_doc->tree.nodes[newIdx].arrayLen, 4);

        // Undo should restore to UInt32
        m_doc->undoStack.undo();
        QApplication::processEvents();
        newIdx = m_doc->tree.indexOfId(nodeId);
        QVERIFY(newIdx >= 0);
        QCOMPARE(m_doc->tree.nodes[newIdx].kind, NodeKind::UInt32);
    }
    // ── Test: clearing value history actually resets heat to 0 ──
    void testClearValueHistoryResetsHeat() {
        // Use a live provider so value tracking runs during refresh()
        m_doc->provider = std::make_unique<BaseAwareProvider>(makeSmallBuffer(), 0);
        m_ctrl->setTrackValues(true);

        // Do initial refresh to populate m_lastResult.meta
        m_ctrl->refresh();
        QApplication::processEvents();

        // Find field_u32 nodeId
        uint64_t targetId = 0;
        for (const auto& n : m_doc->tree.nodes) {
            if (n.name == "field_u32") { targetId = n.id; break; }
        }
        QVERIFY(targetId != 0);

        // Seed value history with multiple changes to get heat > 0
        auto& history = const_cast<QHash<uint64_t, ValueHistory>&>(m_ctrl->valueHistory());
        history[targetId].record("val_1");
        history[targetId].record("val_2");
        history[targetId].record("val_3");
        QVERIFY2(history[targetId].heatLevel() >= 2,
                 "Pre-clear: should have heat >= 2 (warm)");

        // Refresh so heatLevel propagates to LineMeta
        m_ctrl->refresh();
        QApplication::processEvents();

        // Verify heat is visible in meta
        bool foundHot = false;
        for (const auto& lm : m_ctrl->lastResult().meta) {
            if (lm.nodeId == targetId && lm.heatLevel > 0) {
                foundHot = true;
                break;
            }
        }
        QVERIFY2(foundHot, "Pre-clear: LineMeta should show heat > 0");

        // Clear value history exactly as the "Clear All History" context-menu
        // action does — resetChangeTracking() wipes the per-node history AND
        // the raw-byte change-detection cache together, then refresh()
        // re-composes. (The earlier version of this test reached straight into
        // the private history map, which left the byte cache stale — a state
        // no real code path produces now that change-detection keys on bytes.)
        m_ctrl->resetChangeTracking();
        m_ctrl->refresh();
        QApplication::processEvents();

        // Immediately after clear, heatLevel must be 0 for this node.
        for (const auto& lm : m_ctrl->lastResult().meta) {
            if (lm.nodeId == targetId) {
                QCOMPARE(lm.heatLevel, 0);
            }
        }

        // resetChangeTracking arms a short cooldown that suppresses re-recording
        // for a few ticks (so a refresh burst right after a clear can't relight
        // heat). Pump past it; the buffer is static, so exactly ONE baseline
        // value re-records — the raw-byte guard suppresses re-recording the
        // unchanged bytes on every subsequent tick. End state: uniqueCount 1,
        // heat 0 (calm, not spuriously hot).
        for (int i = 0; i < 8; ++i) {
            m_ctrl->refresh();
            QApplication::processEvents();
        }
        QVERIFY(history.contains(targetId));
        QCOMPARE(history[targetId].heatLevel(), 0);
        QCOMPARE(history[targetId].uniqueCount(), 1);
    }

    // ── Regression: a type change that only REFORMATS identical bytes
    // must not register as a value change. Hex64 "0x0" -> Pointer64
    // "nullptr" is the user's exact complaint ("nullptr and 0 are the same
    // value underneath, it's annoying") — the previous-values popup fired
    // and the heatmap lit up even though no memory moved. Change-detection
    // now keys on the raw bytes, so this stays calm.
    void testTypeReformatDoesNotBumpHeat() {
        auto* doc = new RcxDocument();
        doc->tree.baseAddress = 0;

        Node root;
        root.kind = NodeKind::Struct;
        root.name = "root";
        root.parentId = 0;
        root.offset = 0;
        root.collapsed = false;
        int ri = doc->tree.addNode(root);
        uint64_t rootId = doc->tree.nodes[ri].id;

        Node f;
        f.kind = NodeKind::Hex64;
        f.name = "field";
        f.parentId = rootId;
        f.offset = 0;
        int fi = doc->tree.addNode(f);
        uint64_t fieldId = doc->tree.nodes[fi].id;

        // 16 zeroed bytes — the Hex64 field reads value 0 ("0x0").
        doc->provider = std::make_unique<BaseAwareProvider>(QByteArray(16, '\0'), 0);

        auto* ctrl = new RcxController(doc, nullptr);
        ctrl->setTrackValues(true);

        // Establish the baseline "0x0" record.
        for (int i = 0; i < 3; ++i) { ctrl->refresh(); QApplication::processEvents(); }
        QVERIFY(ctrl->valueHistory().contains(fieldId));
        QCOMPARE(ctrl->valueHistory()[fieldId].uniqueCount(), 1);
        QCOMPARE(ctrl->valueHistory()[fieldId].heatLevel(), 0);

        // Reformat the SAME zero bytes: Hex64 -> Pointer64. The displayed
        // value flips from "0x0" to "nullptr"; the bytes do not change.
        int idx = doc->tree.indexOfId(fieldId);
        ctrl->changeNodeKind(idx, NodeKind::Pointer64);
        for (int i = 0; i < 4; ++i) { ctrl->refresh(); QApplication::processEvents(); }

        // No new history entry, no heat — the reformat is invisible to
        // change-detection. (Before the byte-guard fix this recorded a 2nd
        // value -> cold heat -> the previous-values popup fired spuriously.)
        QVERIFY(ctrl->valueHistory().contains(fieldId));
        QCOMPARE(ctrl->valueHistory()[fieldId].uniqueCount(), 1);
        QCOMPARE(ctrl->valueHistory()[fieldId].heatLevel(), 0);

        delete ctrl;
        delete doc;
    }

    // ── Regression: a SIZE-CHANGING kind change (Hex64 -> Int32, the
    // int32x2 split path) over a STATIC buffer must not fire value history.
    // The raw-byte change-detector would otherwise compare the stale 8-byte
    // sample against the new 4-byte read, always mismatch, and record a
    // spurious change — lighting the heatmap on memory that never moved.
    // (User: "i set int32x2, why did it fire value histories on static data".)
    void testKindChangeShrinkDoesNotBumpHeat() {
        auto* doc = new RcxDocument();
        doc->tree.baseAddress = 0;

        Node root;
        root.kind = NodeKind::Struct;
        root.name = "root";
        root.parentId = 0;
        root.offset = 0;
        root.collapsed = false;
        int ri = doc->tree.addNode(root);
        uint64_t rootId = doc->tree.nodes[ri].id;

        Node f;
        f.kind = NodeKind::Hex64;
        f.name = "field";
        f.parentId = rootId;
        f.offset = 0;
        int fi = doc->tree.addNode(f);
        uint64_t fieldId = doc->tree.nodes[fi].id;

        // 16 static bytes with a recognizable non-zero low word.
        QByteArray buf(16, '\0');
        uint32_t lo = 0xDEADBEEF;
        memcpy(buf.data(), &lo, 4);
        doc->provider = std::make_unique<BaseAwareProvider>(buf, 0);

        auto* ctrl = new RcxController(doc, nullptr);
        ctrl->setTrackValues(true);

        // Baseline as Hex64.
        for (int i = 0; i < 3; ++i) { ctrl->refresh(); QApplication::processEvents(); }
        QVERIFY(ctrl->valueHistory().contains(fieldId));
        QCOMPARE(ctrl->valueHistory()[fieldId].uniqueCount(), 1);

        // Shrink to Int32 (size 8 -> 4): this is what the int32x2 split does to
        // the first half. The node keeps its id.
        int idx = doc->tree.indexOfId(fieldId);
        ctrl->changeNodeKind(idx, NodeKind::Int32);
        for (int i = 0; i < 4; ++i) { ctrl->refresh(); QApplication::processEvents(); }

        // Re-baselined: exactly one value, no heat. (Before the fix the stale
        // 8-byte sample vs new 4-byte read mismatched every tick -> false heat.)
        QVERIFY(ctrl->valueHistory().contains(fieldId));
        QCOMPARE(ctrl->valueHistory()[fieldId].uniqueCount(), 1);
        QCOMPARE(ctrl->valueHistory()[fieldId].heatLevel(), 0);

        delete ctrl;
        delete doc;
    }

    // ── Keyboard shortcut logic tests ──

    void testQuickTypeChangeHexSameSize() {
        // Hex32 → Int32 (same 4-byte size, no split/join)
        int hexIdx = m_doc->tree.indexOfId(
            [&]{ for (auto& n : m_doc->tree.nodes)
                     if (n.name == "field_hex") return n.id;
                 return (uint64_t)0; }());
        QVERIFY(hexIdx >= 0);
        QCOMPARE(m_doc->tree.nodes[hexIdx].kind, NodeKind::Hex32);

        m_ctrl->changeNodeKind(hexIdx, NodeKind::Int32);
        hexIdx = m_doc->tree.indexOfId(m_doc->tree.nodes[hexIdx].id);
        QCOMPARE(m_doc->tree.nodes[hexIdx].kind, NodeKind::Int32);
    }

    void testQuickTypeChangeHexShrink() {
        // Hex32 → Hex16 (shrink: should insert padding)
        int hexIdx = -1;
        uint64_t hexId = 0;
        for (int i = 0; i < m_doc->tree.nodes.size(); i++) {
            if (m_doc->tree.nodes[i].name == "field_hex") {
                hexIdx = i; hexId = m_doc->tree.nodes[i].id; break;
            }
        }
        QVERIFY(hexIdx >= 0);
        int oldOffset = m_doc->tree.nodes[hexIdx].offset;

        m_ctrl->changeNodeKind(hexIdx, NodeKind::Hex16);
        int newIdx = m_doc->tree.indexOfId(hexId);
        QVERIFY(newIdx >= 0);
        QCOMPARE(m_doc->tree.nodes[newIdx].kind, NodeKind::Hex16);
        QCOMPARE(m_doc->tree.nodes[newIdx].offset, oldOffset);

        // Padding should exist after the shrunk node
        bool foundPad = false;
        for (const auto& n : m_doc->tree.nodes) {
            if (n.offset == oldOffset + 2 && isHexNode(n.kind)) {
                foundPad = true; break;
            }
        }
        QVERIFY2(foundPad, "Expected padding after shrink");
    }

    void testQuickTypeChangeHexGrow() {
        // Hex32 → Hex64 (grow: should shift siblings)
        int hexIdx = -1;
        uint64_t hexId = 0;
        for (int i = 0; i < m_doc->tree.nodes.size(); i++) {
            if (m_doc->tree.nodes[i].name == "field_hex") {
                hexIdx = i; hexId = m_doc->tree.nodes[i].id; break;
            }
        }
        QVERIFY(hexIdx >= 0);
        int oldOffset = m_doc->tree.nodes[hexIdx].offset; // 12

        m_ctrl->changeNodeKind(hexIdx, NodeKind::Hex64);
        int newIdx = m_doc->tree.indexOfId(hexId);
        QVERIFY(newIdx >= 0);
        QCOMPARE(m_doc->tree.nodes[newIdx].kind, NodeKind::Hex64);
        QCOMPARE(m_doc->tree.nodes[newIdx].offset, oldOffset);
        // Size grew from 4 to 8, siblings after offset 16 shifted by 4
    }

    void testCycleSameSizeTypeVariants() {
        // Get field_hex (Hex32 at offset 12)
        int hexIdx = -1;
        uint64_t hexId = 0;
        for (int i = 0; i < m_doc->tree.nodes.size(); i++) {
            if (m_doc->tree.nodes[i].name == "field_hex") {
                hexIdx = i; hexId = m_doc->tree.nodes[i].id; break;
            }
        }
        QVERIFY(hexIdx >= 0);
        QCOMPARE(m_doc->tree.nodes[hexIdx].kind, NodeKind::Hex32);

        // Build the same-size variant list as the controller does
        int sz = sizeForKind(NodeKind::Hex32); // 4
        QVector<NodeKind> variants;
        for (const auto& m : kKindMeta) {
            if (m.size == sz && m.kind != NodeKind::Struct && m.kind != NodeKind::Array)
                variants.append(m.kind);
        }
        QVERIFY(variants.size() > 1);
        QVERIFY(variants.contains(NodeKind::Hex32));
        QVERIFY(variants.contains(NodeKind::Int32));
        QVERIFY(variants.contains(NodeKind::Float));

        // Cycle forward: Hex32 → next variant
        int curIdx = variants.indexOf(NodeKind::Hex32);
        NodeKind expected = variants[(curIdx + 1) % variants.size()];
        m_ctrl->changeNodeKind(hexIdx, expected);
        int newIdx = m_doc->tree.indexOfId(hexId);
        QVERIFY(newIdx >= 0);
        QCOMPARE(m_doc->tree.nodes[newIdx].kind, expected);
    }

    void testDeleteKeyRemovesNode() {
        int countBefore = m_doc->tree.nodes.size();
        // Find field_u8
        uint64_t u8Id = 0;
        for (const auto& n : m_doc->tree.nodes)
            if (n.name == "field_u8") { u8Id = n.id; break; }
        QVERIFY(u8Id != 0);

        int idx = m_doc->tree.indexOfId(u8Id);
        m_ctrl->removeNode(idx);
        QVERIFY(m_doc->tree.indexOfId(u8Id) < 0);
        QVERIFY(m_doc->tree.nodes.size() < countBefore);
    }

    void testDuplicateNode() {
        int countBefore = m_doc->tree.nodes.size();
        int idx = -1;
        for (int i = 0; i < m_doc->tree.nodes.size(); i++)
            if (m_doc->tree.nodes[i].name == "field_float") { idx = i; break; }
        QVERIFY(idx >= 0);

        m_ctrl->duplicateNode(idx);
        QCOMPARE(m_doc->tree.nodes.size(), countBefore + 1);

        // Find the copy
        bool foundCopy = false;
        for (const auto& n : m_doc->tree.nodes)
            if (n.name == "field_float_copy") { foundCopy = true; break; }
        QVERIFY2(foundCopy, "Expected duplicated node with _copy suffix");
    }

    void testSplitHexNode() {
        // Find field_hex (Hex32 at offset 12) and split it
        uint64_t hexId = 0;
        for (const auto& n : m_doc->tree.nodes)
            if (n.name == "field_hex") { hexId = n.id; break; }
        QVERIFY(hexId != 0);

        m_ctrl->splitHexNode(hexId);

        // Original should be gone, two Hex16 nodes at offsets 12 and 14
        QVERIFY(m_doc->tree.indexOfId(hexId) < 0);
        int found16 = 0;
        for (const auto& n : m_doc->tree.nodes)
            if (n.kind == NodeKind::Hex16 && (n.offset == 12 || n.offset == 14))
                found16++;
        QCOMPARE(found16, 2);
    }

    void testSplitHexNodeUndo() {
        uint64_t hexId = 0;
        for (const auto& n : m_doc->tree.nodes)
            if (n.name == "field_hex") { hexId = n.id; break; }
        QVERIFY(hexId != 0);
        int countBefore = m_doc->tree.nodes.size();

        m_ctrl->splitHexNode(hexId);
        m_doc->undoStack.undo();

        QCOMPARE(m_doc->tree.nodes.size(), countBefore);
        QVERIFY(m_doc->tree.indexOfId(hexId) >= 0);
        QCOMPARE(m_doc->tree.nodes[m_doc->tree.indexOfId(hexId)].kind, NodeKind::Hex32);
    }

    void testGroupIntoUnion() {
        // Select field_u32 and field_float, group into union
        uint64_t u32Id = 0, floatId = 0;
        for (const auto& n : m_doc->tree.nodes) {
            if (n.name == "field_u32") u32Id = n.id;
            if (n.name == "field_float") floatId = n.id;
        }
        QVERIFY(u32Id != 0 && floatId != 0);

        QSet<uint64_t> ids = {u32Id, floatId};
        m_ctrl->groupIntoUnion(ids);

        // There should now be a union node containing the two fields
        bool foundUnion = false;
        for (const auto& n : m_doc->tree.nodes) {
            if (n.isUnion()) {
                foundUnion = true;
                // Children should be at offset 0
                auto kids = m_doc->tree.childrenOf(n.id);
                QCOMPARE(kids.size(), 2);
                for (int ci : kids)
                    QCOMPARE(m_doc->tree.nodes[ci].offset, 0);
                break;
            }
        }
        QVERIFY2(foundUnion, "Expected a union node after groupIntoUnion");
    }

    void testToggleCollapseRoundTrip() {
        // Root struct should be uncollapsed
        uint64_t rootId = m_doc->tree.nodes[0].id;
        QCOMPARE(m_doc->tree.nodes[0].collapsed, false);

        int ri = m_doc->tree.indexOfId(rootId);
        m_ctrl->toggleCollapse(ri);
        QCOMPARE(m_doc->tree.nodes[m_doc->tree.indexOfId(rootId)].collapsed, true);

        m_ctrl->toggleCollapse(m_doc->tree.indexOfId(rootId));
        QCOMPARE(m_doc->tree.nodes[m_doc->tree.indexOfId(rootId)].collapsed, false);
    }

    void testInsertNodeAutoOffset() {
        uint64_t rootId = m_doc->tree.nodes[0].id;
        int countBefore = m_doc->tree.nodes.size();

        // Insert with offset -1 = auto-place after last sibling
        m_ctrl->insertNode(rootId, -1, NodeKind::Hex64, "appended");
        QCOMPARE(m_doc->tree.nodes.size(), countBefore + 1);

        // Find the new node
        bool found = false;
        for (const auto& n : m_doc->tree.nodes) {
            if (n.name == "appended") { found = true; QVERIFY(n.offset > 0); break; }
        }
        QVERIFY(found);
    }

    // ── Test: insertNestedStruct builds a recursive inline member tree ──
    // struct A { struct B { int32_t x; int32_t y; } Inner;
    //            struct { int32_t z; } Inner2; }
    void testInsertNestedStruct() {
        uint64_t rootId = m_doc->tree.nodes[0].id;
        const int before = m_doc->tree.nodes.size();

        NestedStructSpec x; x.kind = NodeKind::Int32; x.name = "x";
        NestedStructSpec y; y.kind = NodeKind::Int32; y.name = "y";
        QVector<NestedStructSpec> innerKids = {x, y};
        uint64_t innerId = m_ctrl->insertNestedStruct(rootId, 0, "Inner", "B",
                                                      "struct", innerKids);
        QVERIFY(innerId != 0);

        NestedStructSpec z; z.kind = NodeKind::Int32; z.name = "z";
        QVector<NestedStructSpec> inner2Kids = {z};
        uint64_t inner2Id = m_ctrl->insertNestedStruct(rootId, 8, "Inner2", QString(),
                                                       "struct", inner2Kids);
        QVERIFY(inner2Id != 0);

        // Inner: named struct member, children x@0, y@4
        int ii = m_doc->tree.indexOfId(innerId);
        QVERIFY(ii >= 0);
        const Node& inner = m_doc->tree.nodes[ii];
        QCOMPARE(inner.kind, NodeKind::Struct);
        QCOMPARE(inner.name, QStringLiteral("Inner"));
        QCOMPARE(inner.structTypeName, QStringLiteral("B"));
        QCOMPARE(inner.classKeyword, QStringLiteral("struct"));
        QCOMPARE(inner.parentId, rootId);
        QCOMPARE(inner.offset, 0);
        QVector<int> ikids;
        for (int i = 0; i < m_doc->tree.nodes.size(); i++)
            if (m_doc->tree.nodes[i].parentId == innerId) ikids.append(i);
        QCOMPARE(ikids.size(), 2);
        QCOMPARE(m_doc->tree.nodes[ikids[0]].name, QStringLiteral("x"));
        QCOMPARE(m_doc->tree.nodes[ikids[0]].offset, 0);
        QCOMPARE(m_doc->tree.nodes[ikids[1]].name, QStringLiteral("y"));
        QCOMPARE(m_doc->tree.nodes[ikids[1]].offset, 4);

        // Inner2: anonymous struct member, child z@0
        int i2 = m_doc->tree.indexOfId(inner2Id);
        QVERIFY(i2 >= 0);
        const Node& inner2 = m_doc->tree.nodes[i2];
        QCOMPARE(inner2.kind, NodeKind::Struct);
        QCOMPARE(inner2.structTypeName, QString());
        QCOMPARE(inner2.offset, 8);
        QVector<int> i2kids;
        for (int i = 0; i < m_doc->tree.nodes.size(); i++)
            if (m_doc->tree.nodes[i].parentId == inner2Id) i2kids.append(i);
        QCOMPARE(i2kids.size(), 1);
        QCOMPARE(m_doc->tree.nodes[i2kids[0]].name, QStringLiteral("z"));
        QCOMPARE(m_doc->tree.nodes[i2kids[0]].offset, 0);

        // Each insertNestedStruct is one undo macro: undo once removes
        // Inner2's subtree only, undo again removes Inner's subtree.
        m_doc->undoStack.undo();
        QCOMPARE(m_doc->tree.nodes.size(), before + 3);  // Inner + x + y
        m_doc->undoStack.undo();
        QCOMPARE(m_doc->tree.nodes.size(), before);
    }

    // ── Test: union overlap at 0 + unlimited recursion depth ──
    void testInsertNestedStructUnionAndDeep() {
        uint64_t rootId = m_doc->tree.nodes[0].id;

        // Union member: every child packs to offset 0.
        NestedStructSpec a; a.kind = NodeKind::UInt64; a.name = "a";
        NestedStructSpec b; b.kind = NodeKind::UInt32; b.name = "b";
        QVector<NestedStructSpec> uKids = {a, b};
        uint64_t uId = m_ctrl->insertNestedStruct(rootId, 0, "U", QString(),
                                                  "union", uKids);
        QVERIFY(uId != 0);
        int uCount = 0;
        for (int i = 0; i < m_doc->tree.nodes.size(); i++) {
            if (m_doc->tree.nodes[i].parentId == uId) {
                uCount++;
                QCOMPARE(m_doc->tree.nodes[i].offset, 0);
            }
        }
        QCOMPARE(uCount, 2);

        // Deep recursion: Outer -> Deep(struct C) -> d1 ; sibling x packs
        // before Deep with natural alignment (x@0, Deep@4).
        NestedStructSpec d1; d1.kind = NodeKind::Int32; d1.name = "d1";
        NestedStructSpec deep; deep.kind = NodeKind::Struct; deep.name = "Deep";
        deep.keyword = "struct"; deep.typeName = "C";
        deep.children.append(d1);
        NestedStructSpec x; x.kind = NodeKind::Int32; x.name = "x";
        QVector<NestedStructSpec> outerKids = {x, deep};
        uint64_t outerId = m_ctrl->insertNestedStruct(rootId, 16, "Outer", "B",
                                                      "struct", outerKids);
        QVERIFY(outerId != 0);

        int deepId = -1;
        for (int i = 0; i < m_doc->tree.nodes.size(); i++) {
            const Node& n = m_doc->tree.nodes[i];
            if (n.parentId == outerId && n.name == QStringLiteral("Deep")) {
                deepId = i;
                QCOMPARE(n.offset, 4);
                QCOMPARE(n.structTypeName, QStringLiteral("C"));
            }
        }
        QVERIFY(deepId >= 0);
        const uint64_t deepNodeId = m_doc->tree.nodes[deepId].id;
        int d1Count = 0;
        for (int i = 0; i < m_doc->tree.nodes.size(); i++) {
            if (m_doc->tree.nodes[i].parentId == deepNodeId) {
                d1Count++;
                QCOMPARE(m_doc->tree.nodes[i].name, QStringLiteral("d1"));
                QCOMPARE(m_doc->tree.nodes[i].offset, 0);
            }
        }
        QCOMPARE(d1Count, 1);
    }

    // ── Test: Change Type accepts any name on a container (inline declared) ──
    void testRetypeStructNodeAcceptsFreeName() {
        uint64_t rootId = m_doc->tree.nodes[0].id;
        QVector<NestedStructSpec> noKids;
        uint64_t memId = m_ctrl->insertNestedStruct(rootId, 0x20, "Inner", QString(),
                                                    "struct", noKids);
        int mi = m_doc->tree.indexOfId(memId);
        QVERIFY(mi >= 0);

        // Unknown name on a container → structTypeName set freely.
        emit m_editor->inlineEditCommitted(mi, 0, EditTarget::Type, "B", 0);
        mi = m_doc->tree.indexOfId(memId);
        QCOMPARE(m_doc->tree.nodes[mi].kind, NodeKind::Struct);
        QCOMPARE(m_doc->tree.nodes[mi].structTypeName, QStringLiteral("B"));

        // Whitespace → no change.
        emit m_editor->inlineEditCommitted(mi, 0, EditTarget::Type, "   ", 0);
        QCOMPARE(m_doc->tree.nodes[mi].structTypeName, QStringLiteral("B"));

        // Non-container node with an unknown name → still ignored (no
        // silent conversion to an empty struct shell).
        int fi = -1;
        for (int i = 0; i < m_doc->tree.nodes.size(); i++)
            if (m_doc->tree.nodes[i].name == QStringLiteral("field_u32")) { fi = i; break; }
        QVERIFY(fi >= 0);
        emit m_editor->inlineEditCommitted(fi, 0, EditTarget::Type, "NoSuchType", 0);
        QCOMPARE(m_doc->tree.nodes[fi].kind, NodeKind::UInt32);
        QCOMPARE(m_doc->tree.nodes[fi].structTypeName, QString());
    }

    void testUnionAppendPlacesAtExactEnd() {
        // A union whose members end at 0x14 (member at offset 4 sized 0x10)
        // must append the next member at exactly 0x14 — not round it up to
        // 0x18 via Hex64 alignment, which would skip 4 bytes.
        Node u;
        u.kind = NodeKind::Struct;
        u.name = "u";
        u.classKeyword = "union";
        u.parentId = m_doc->tree.nodes[0].id;
        u.offset = 0;
        int ui = m_doc->tree.addNode(u);
        uint64_t unionId = m_doc->tree.nodes[ui].id;

        Node a; a.kind = NodeKind::Hex64; a.name = "a";
        a.parentId = unionId; a.offset = 0;
        m_doc->tree.addNode(a);
        Node b; b.kind = NodeKind::Hex64; b.name = "b";
        b.parentId = unionId; b.offset = 8;
        m_doc->tree.addNode(b);
        Node c; c.kind = NodeKind::Hex128; c.name = "c";
        c.parentId = unionId; c.offset = 4;
        m_doc->tree.addNode(c);

        // Union size = 0x10 (largest member); members' extent = 0x14.
        QCOMPARE(m_doc->tree.unionSize(unionId), 0x10);

        // +1 pill path (Hex8 auto-place) must land exactly at 0x14, the
        // last member's end — never rounded up.
        emit m_editor->appendSingleFieldRequested(unionId);
        int bi = -1;
        for (int i = 0; i < m_doc->tree.nodes.size(); i++)
            if (m_doc->tree.nodes[i].name.startsWith(QStringLiteral("field_"))
                && m_doc->tree.nodes[i].parentId == unionId) { bi = i; break; }
        QVERIFY(bi >= 0);
        QCOMPARE(m_doc->tree.nodes[bi].offset, 0x14);

        // +10h-style append (Hex64 auto-place) must also land exactly at
        // the new last member's end (0x14 + 1) — 0x15, not 0x18.
        m_ctrl->insertNode(unionId, -1, NodeKind::Hex64, "appended");
        int ai = -1;
        for (int i = 0; i < m_doc->tree.nodes.size(); i++)
            if (m_doc->tree.nodes[i].name == "appended") { ai = i; break; }
        QVERIFY(ai >= 0);
        QCOMPARE(m_doc->tree.nodes[ai].offset, 0x15);
    }

    void testDeleteUnionShiftsSiblingToUnionStart() {
        // Model: a union's footprint in its parent is its C size (largest
        // member), NOT the extent of member offsets. A member at offset 4
        // sized 0x10 keeps the union at 0x10 — so a sibling placed right
        // after the '}' (offset 0x10) must shift back to 0x0 when the union
        // is deleted. Deleting the union must not leave a hole at offset 0
        // where the union started.
        struct Probe { int memberOff; int memberSz; int sibOff; };
        QVector<Probe> probes = {
            {0,    8,    8},     // trivial: size == extent, sibling right after
            {4,    0x10, 0x10},  // the reported bug: member at 4 sized 0x10
                                 // -> union size 0x10, sibling after '}'
        };
        for (const auto& p : probes) {
            RcxDocument* doc = new RcxDocument();
            Node root; root.kind = NodeKind::Struct; root.name = "R";
            root.parentId = 0; root.offset = 0;
            int ri = doc->tree.addNode(root);
            uint64_t rootId = doc->tree.nodes[ri].id;

            Node u; u.kind = NodeKind::Struct; u.name = "u";
            u.classKeyword = "union"; u.parentId = rootId; u.offset = 0;
            int ui = doc->tree.addNode(u);
            uint64_t uid = doc->tree.nodes[ui].id;
            Node m1; m1.kind = (p.memberSz == 8) ? NodeKind::Hex64 : NodeKind::Hex128;
            m1.name = "m1";
            m1.parentId = uid; m1.offset = p.memberOff;
            doc->tree.addNode(m1);
            Node f; f.kind = NodeKind::Hex64; f.name = "field";
            f.parentId = rootId; f.offset = p.sibOff;
            doc->tree.addNode(f);

            // The union's footprint (structSpan) is its C size, not the
            // extent of the member at a non-zero offset.
            QCOMPARE(doc->tree.unionSize(uid), p.memberSz);
            QCOMPARE(doc->tree.structSpan(uid), p.memberSz);

            QByteArray buf(128, '\0');
            doc->provider = std::make_unique<BufferProvider>(buf);
            auto* splitter = new QSplitter();
            auto* ctrl = new RcxController(doc, nullptr);
            ctrl->addSplitEditor(splitter);

            ctrl->removeNode(ui);

            // The sibling must land exactly where the union started — no
            // hole at offset 0, no leftover gap.
            int fi = -1;
            for (int i = 0; i < doc->tree.nodes.size(); i++)
                if (doc->tree.nodes[i].name == "field") { fi = i; break; }
            QVERIFY(fi >= 0);
            QCOMPARE(doc->tree.nodes[fi].offset, 0);

            delete ctrl; delete splitter; delete doc;
        }
    }

    void testUnionSiblingAppendAfterNonZeroOffset() {
        // User's shape: union at parent offset 4 with a member at rel 4
        // sized 0x10. C size = 0x10, so a sibling appended after the union
        // must land at 0x14 (4 + 0x10) — not 0x18 (4 + extent 0x14).
        RcxDocument* doc = new RcxDocument();
        Node root; root.kind = NodeKind::Struct; root.name = "R";
        root.parentId = 0; root.offset = 0;
        int ri = doc->tree.addNode(root);
        uint64_t rootId = doc->tree.nodes[ri].id;

        Node u; u.kind = NodeKind::Struct; u.name = "u";
        u.classKeyword = "union"; u.parentId = rootId; u.offset = 4;
        int ui = doc->tree.addNode(u);
        uint64_t uid = doc->tree.nodes[ui].id;
        Node m1; m1.kind = NodeKind::Hex32; m1.name = "field1";
        m1.parentId = uid; m1.offset = 0;
        doc->tree.addNode(m1);
        Node m2; m2.kind = NodeKind::Hex128; m2.name = "field2";
        m2.parentId = uid; m2.offset = 4;
        doc->tree.addNode(m2);

        // C size 0x10 (largest member), footprint in parent = 0x10.
        QCOMPARE(doc->tree.unionSize(uid), 0x10);
        QCOMPARE(doc->tree.structSpan(uid), 0x10);

        QByteArray buf(128, '\0');
        doc->provider = std::make_unique<BufferProvider>(buf);
        auto* splitter = new QSplitter();
        auto* ctrl = new RcxController(doc, nullptr);
        RcxEditor* editor = ctrl->addSplitEditor(splitter);

        // +1 on the parent struct row → sibling after the union.
        emit editor->appendSingleFieldRequested(rootId);

        int fi = -1;
        for (int i = 0; i < doc->tree.nodes.size(); i++)
            if (doc->tree.nodes[i].parentId == rootId
                && doc->tree.nodes[i].kind == NodeKind::Hex8) { fi = i; break; }
        QVERIFY(fi >= 0);
        QCOMPARE(doc->tree.nodes[fi].offset, 0x14);

        delete ctrl; delete splitter; delete doc;
    }

    void testPasteUnionShiftUsesCSize() {
        // Pasting a union block below a selection shifts later siblings
        // down by the union's C size (largest member, 0x10) — not the
        // member extent (0x14). A member at rel 4 sized 0x10 must not push
        // the tail 4 bytes too far, recreating the 0x14→0x18 gap on paste.
        m_doc->tree.nodes.clear();
        m_doc->tree.invalidateIdCache();
        Node root; root.kind = NodeKind::Struct; root.structTypeName = "R";
        root.parentId = 0; root.collapsed = false;
        uint64_t rootId = m_doc->tree.nodes[m_doc->tree.addNode(root)].id;

        Node a; a.kind = NodeKind::Hex8; a.name = "a";
        a.parentId = rootId; a.offset = 0;
        uint64_t aId = m_doc->tree.nodes[m_doc->tree.addNode(a)].id;

        Node u; u.kind = NodeKind::Struct; u.name = "u"; u.classKeyword = "union";
        u.parentId = rootId; u.offset = 4;
        uint64_t uid = m_doc->tree.nodes[m_doc->tree.addNode(u)].id;
        Node m1; m1.kind = NodeKind::Hex32; m1.name = "m1";
        m1.parentId = uid; m1.offset = 0;
        m_doc->tree.addNode(m1);
        Node m2; m2.kind = NodeKind::Hex128; m2.name = "m2";
        m2.parentId = uid; m2.offset = 4;
        m_doc->tree.addNode(m2);

        Node tail; tail.kind = NodeKind::Hex64; tail.name = "tail";
        tail.parentId = rootId; tail.offset = 0x18;
        m_doc->tree.addNode(tail);

        QCOMPARE(m_doc->tree.unionSize(uid), 0x10);
        QCOMPARE(m_doc->tree.structSpan(uid), 0x10);

        m_ctrl->setViewRootId(rootId);
        m_ctrl->refresh();

        // Select the anchor row, then paste the union block below it.
        m_ctrl->handleNodeClick(m_ctrl->primaryEditor(), 1, aId, Qt::NoModifier);
        QVERIFY(m_ctrl->selectedIds().contains(aId));
        // Release any previous clipboard ownership first — a stale owner
        // makes the next setMimeData fail with OpenClipboard COM errors.
        QApplication::clipboard()->clear();
        QApplication::processEvents();
        QApplication::clipboard()->setMimeData(
            ClipboardCodec::serialize(m_doc->tree, {uid}));
        emit m_editor->pasteNodesRequested();

        // Original union and tail shift down by the C size (0x10), not the
        // extent (0x14): 4 + 0x10 = 0x14, 0x18 + 0x10 = 0x28.
        int ui = m_doc->tree.indexOfId(uid);
        QVERIFY(ui >= 0);
        QCOMPARE(m_doc->tree.nodes[ui].offset, 0x14);
        int ti = -1;
        for (int i = 0; i < m_doc->tree.nodes.size(); i++)
            if (m_doc->tree.nodes[i].name == "tail") { ti = i; break; }
        QVERIFY(ti >= 0);
        QCOMPARE(m_doc->tree.nodes[ti].offset, 0x28);

        // The pasted union lands right after the anchor (offset 1).
        int pasted = -1;
        for (int i = 0; i < m_doc->tree.nodes.size(); i++)
            if (m_doc->tree.nodes[i].name == "u"
                && m_doc->tree.nodes[i].parentId == rootId
                && m_doc->tree.nodes[i].id != uid) { pasted = i; break; }
        QVERIFY(pasted >= 0);
        QCOMPARE(m_doc->tree.nodes[pasted].offset, 0x1);
        QCOMPARE(m_doc->tree.unionSize(m_doc->tree.nodes[pasted].id), 0x10);
    }

    void testPasteMultiRootUnionPlacement() {
        // Copy a block {union with member at rel 4 sized 0x10 (C size
        // 0x10), following sibling} and paste below a selection: the pasted
        // sibling must land exactly one C-size (0x10) past the pasted
        // union's start — pre-fix extent math would place it 0x14 past,
        // then align it to 0x18, leaving a 4-byte gap.
        m_doc->tree.nodes.clear();
        m_doc->tree.invalidateIdCache();
        Node root; root.kind = NodeKind::Struct; root.structTypeName = "R";
        root.parentId = 0; root.collapsed = false;
        uint64_t rootId = m_doc->tree.nodes[m_doc->tree.addNode(root)].id;

        Node a; a.kind = NodeKind::Hex64; a.name = "a";
        a.parentId = rootId; a.offset = 0;
        uint64_t aId = m_doc->tree.nodes[m_doc->tree.addNode(a)].id;

        Node u; u.kind = NodeKind::Struct; u.name = "u"; u.classKeyword = "union";
        u.parentId = rootId; u.offset = 0x10;
        uint64_t uid = m_doc->tree.nodes[m_doc->tree.addNode(u)].id;
        Node m1; m1.kind = NodeKind::Hex32; m1.name = "m1";
        m1.parentId = uid; m1.offset = 0;
        m_doc->tree.addNode(m1);
        Node m2; m2.kind = NodeKind::Hex128; m2.name = "m2";
        m2.parentId = uid; m2.offset = 4;
        m_doc->tree.addNode(m2);

        Node after; after.kind = NodeKind::Hex64; after.name = "after";
        after.parentId = rootId; after.offset = 0x20;
        uint64_t afterId = m_doc->tree.nodes[m_doc->tree.addNode(after)].id;

        QCOMPARE(m_doc->tree.unionSize(uid), 0x10);

        m_ctrl->setViewRootId(rootId);
        m_ctrl->refresh();
        m_ctrl->handleNodeClick(m_ctrl->primaryEditor(), 1, aId, Qt::NoModifier);
        QVERIFY(m_ctrl->selectedIds().contains(aId));

        // Roots in struct order (union@0x10 before after@0x20) — the
        // codec preserves the given order, so paste places union first.
        QApplication::clipboard()->clear();
        QApplication::processEvents();
        QApplication::clipboard()->setMimeData(
            ClipboardCodec::serialize(m_doc->tree, {uid, afterId}));
        emit m_editor->pasteNodesRequested();

        // Find the pasted pair (original union/after were shifted, not
        // duplicated, so any node past the anchor with these names is new).
        int pu = -1, pa = -1;
        for (int i = 0; i < m_doc->tree.nodes.size(); i++) {
            const Node& n = m_doc->tree.nodes[i];
            if (n.parentId != rootId) continue;
            if (n.name == "u" && n.id != uid)      pu = i;
            if (n.name == "after" && n.id != afterId) pa = i;
        }
        QVERIFY2(pu >= 0 && pa >= 0, "expected pasted union + sibling");

        // Pasted union right after the anchor (anchorEnd 8, Struct align 1).
        QCOMPARE(m_doc->tree.nodes[pu].offset, 0x8);
        // Pasted sibling lands exactly one C-size later (align 8 keeps 0x18).
        QCOMPARE(m_doc->tree.nodes[pa].offset - m_doc->tree.nodes[pu].offset, 0x10);
        QCOMPARE(m_doc->tree.nodes[pa].offset, 0x18);
    }

    void testUnionAbsorbsOverlappingSiblings() {
        // User's scenario: fields at 0, 4, 8, 0xC, 0x10 → group 4+8 into
        // a union → grow a member to int8_t[16] → the union now spans
        // [0x4, 0x14), swallowing the fields at 0xC and 0x10. They must
        // be absorbed into the union as members (rel 8 / rel 0xC), not
        // left as overlapping siblings.
        m_doc->tree.nodes.clear();
        m_doc->tree.invalidateIdCache();
        Node root; root.kind = NodeKind::Struct; root.structTypeName = "R";
        root.parentId = 0; root.collapsed = false;
        uint64_t rootId = m_doc->tree.nodes[m_doc->tree.addNode(root)].id;

        for (int off : {0, 4, 8, 0xC, 0x10}) {
            Node f; f.kind = NodeKind::Hex32;
            f.name = QStringLiteral("f%1").arg(off, 0, 16);
            f.parentId = rootId; f.offset = off;
            m_doc->tree.addNode(f);
        }

        m_ctrl->setViewRootId(rootId);
        m_ctrl->refresh();

        // Group the fields at 4 and 8 into a union.
        QVector<uint64_t> g;
        for (int i = 0; i < m_doc->tree.nodes.size(); i++) {
            const Node& n = m_doc->tree.nodes[i];
            if (n.parentId == rootId && (n.offset == 4 || n.offset == 8))
                g.append(n.id);
        }
        QCOMPARE(g.size(), 2);
        QSet<uint64_t> gset;
        for (uint64_t id : g) gset.insert(id);
        m_ctrl->groupIntoUnion(gset);

        int ui = -1;
        for (int i = 0; i < m_doc->tree.nodes.size(); i++)
            if (m_doc->tree.nodes[i].isUnion()) { ui = i; break; }
        QVERIFY(ui >= 0);
        uint64_t uid = m_doc->tree.nodes[ui].id;
        QCOMPARE(m_doc->tree.unionSize(uid), 4);

        // Grow a member (rel 0) to int8_t[16] via the type-chooser path.
        int mi = -1;
        for (int i = 0; i < m_doc->tree.nodes.size(); i++)
            if (m_doc->tree.nodes[i].parentId == uid
                && m_doc->tree.nodes[i].offset == 0) { mi = i; break; }
        QVERIFY(mi >= 0);
        TypeEntry e;
        e.entryKind = TypeEntry::Primitive;
        e.primitiveKind = NodeKind::Int8;
        m_ctrl->applyTypePopupResult(TypePopupMode::FieldType, mi, e,
                                     QStringLiteral("int8_t[16]"));

        // Union now spans [0x4, 0x14).
        QCOMPARE(m_doc->tree.unionSize(uid), 0x10);

        // The fields formerly at 0xC and 0x10 are union members at
        // rel 8 / rel 0xC (offset relative to the union at 0x4).
        // (The union also keeps its original second member at rel 0.)
        int at8 = 0, atC = 0;
        for (int i = 0; i < m_doc->tree.nodes.size(); i++) {
            const Node& n = m_doc->tree.nodes[i];
            if (n.parentId == uid && n.kind == NodeKind::Hex32) {
                if (n.offset == 8)      at8++;
                else if (n.offset == 0xC) atC++;
            }
        }
        QCOMPARE(at8, 1);
        QCOMPARE(atC, 1);
        QVERIFY(m_doc->tree.findOverlaps().isEmpty());

        // One undo restores the pre-growth layout: 0xC/0x10 back as
        // siblings of the union, union back to size 4.
        m_doc->undoStack.undo();
        QCOMPARE(m_doc->tree.unionSize(uid), 4);
        int sibs = 0;
        for (int i = 0; i < m_doc->tree.nodes.size(); i++) {
            const Node& n = m_doc->tree.nodes[i];
            if (n.parentId == rootId && n.kind == NodeKind::Hex32) {
                sibs++;
                QVERIFY2(n.offset == 0 || n.offset == 0xC || n.offset == 0x10,
                    qPrintable(QString("sibling back at unexpected offset 0x%1")
                                   .arg(n.offset, 0, 16)));
            }
        }
        QCOMPARE(sibs, 3);  // f@0 plus the restored 0xC and 0x10
    }

    void testUnionAbsorbsOnMemberKindGrow() {
        // Same shape, but the member grows via changeNodeKind directly
        // (Hex32 → Hex128): the union spans [0x4, 0x14) and the fields at
        // 0xC/0x10 are absorbed in the same undo step.
        m_doc->tree.nodes.clear();
        m_doc->tree.invalidateIdCache();
        Node root; root.kind = NodeKind::Struct; root.structTypeName = "R";
        root.parentId = 0; root.collapsed = false;
        uint64_t rootId = m_doc->tree.nodes[m_doc->tree.addNode(root)].id;

        for (int off : {0, 4, 8, 0xC, 0x10}) {
            Node f; f.kind = NodeKind::Hex32;
            f.name = QStringLiteral("f%1").arg(off, 0, 16);
            f.parentId = rootId; f.offset = off;
            m_doc->tree.addNode(f);
        }
        m_ctrl->setViewRootId(rootId);
        m_ctrl->refresh();

        QVector<uint64_t> g;
        for (int i = 0; i < m_doc->tree.nodes.size(); i++) {
            const Node& n = m_doc->tree.nodes[i];
            if (n.parentId == rootId && (n.offset == 4 || n.offset == 8))
                g.append(n.id);
        }
        QSet<uint64_t> gset;
        for (uint64_t id : g) gset.insert(id);
        m_ctrl->groupIntoUnion(gset);

        int ui = -1;
        for (int i = 0; i < m_doc->tree.nodes.size(); i++)
            if (m_doc->tree.nodes[i].isUnion()) { ui = i; break; }
        QVERIFY(ui >= 0);
        uint64_t uid = m_doc->tree.nodes[ui].id;

        int mi = -1;
        for (int i = 0; i < m_doc->tree.nodes.size(); i++)
            if (m_doc->tree.nodes[i].parentId == uid) { mi = i; break; }
        QVERIFY(mi >= 0);
        m_ctrl->changeNodeKind(mi, NodeKind::Hex128);

        QCOMPARE(m_doc->tree.unionSize(uid), 0x10);
        int at8 = 0, atC = 0;
        for (int i = 0; i < m_doc->tree.nodes.size(); i++) {
            const Node& n = m_doc->tree.nodes[i];
            if (n.parentId == uid && n.kind == NodeKind::Hex32) {
                if (n.offset == 8)      at8++;
                else if (n.offset == 0xC) atC++;
            }
        }
        QCOMPARE(at8, 1);
        QCOMPARE(atC, 1);
        QVERIFY(m_doc->tree.findOverlaps().isEmpty());
    }

    void testUnionMemberGrowDoesNotShiftMembers() {
        // A union's members keep their deliberate union-relative offsets —
        // growing one member must NOT shift the others (they overlap by
        // design). Pre-absorption this was latent; after absorption the
        // members sit at rel 8/rel 0xC and an unguarded shift would move
        // them, corrupting the layout.
        m_doc->tree.nodes.clear();
        m_doc->tree.invalidateIdCache();
        Node root; root.kind = NodeKind::Struct; root.structTypeName = "R";
        root.parentId = 0; root.collapsed = false;
        uint64_t rootId = m_doc->tree.nodes[m_doc->tree.addNode(root)].id;

        Node u; u.kind = NodeKind::Struct; u.name = "u"; u.classKeyword = "union";
        u.parentId = rootId; u.offset = 0;
        uint64_t uid = m_doc->tree.nodes[m_doc->tree.addNode(u)].id;
        Node a; a.kind = NodeKind::Hex32; a.name = "a";
        a.parentId = uid; a.offset = 0;
        m_doc->tree.addNode(a);
        Node b; b.kind = NodeKind::Hex32; b.name = "b";
        b.parentId = uid; b.offset = 8;
        m_doc->tree.addNode(b);

        m_ctrl->setViewRootId(rootId);
        m_ctrl->refresh();

        int ai = -1;
        for (int i = 0; i < m_doc->tree.nodes.size(); i++)
            if (m_doc->tree.nodes[i].name == "a") { ai = i; break; }
        QVERIFY(ai >= 0);
        m_ctrl->changeNodeKind(ai, NodeKind::Hex64);  // 4 → 8 bytes

        // Member b keeps its deliberate offset — it must not be shifted.
        int bi = -1;
        for (int i = 0; i < m_doc->tree.nodes.size(); i++)
            if (m_doc->tree.nodes[i].name == "b") { bi = i; break; }
        QVERIFY(bi >= 0);
        QCOMPARE(m_doc->tree.nodes[bi].offset, 8);
    }

    void testUnionArrayGrowAbsorbSingleUndo() {
        // Finding 1 (scrutinize): appendBytesRequested on an array that is a
        // union member must wrap the array growth AND the absorption in one
        // undo step — one Ctrl+Z restores the whole pre-growth layout (array
        // length back AND siblings back out of the union), instead of first
        // undoing only the absorption and leaving the array grown with the
        // overlapping siblings restored.
        m_doc->tree.nodes.clear();
        m_doc->tree.invalidateIdCache();
        Node root; root.kind = NodeKind::Struct; root.structTypeName = "R";
        root.parentId = 0; root.collapsed = false;
        uint64_t rootId = m_doc->tree.nodes[m_doc->tree.addNode(root)].id;

        // Union at 0x4 with an int8[4] array member (size 4).
        Node u; u.kind = NodeKind::Struct; u.name = "u"; u.classKeyword = "union";
        u.parentId = rootId; u.offset = 4;
        uint64_t uid = m_doc->tree.nodes[m_doc->tree.addNode(u)].id;
        Node arr; arr.kind = NodeKind::Array; arr.name = "arr";
        arr.parentId = uid; arr.offset = 0;
        arr.elementKind = NodeKind::Int8; arr.arrayLen = 4;
        uint64_t arrId = m_doc->tree.nodes[m_doc->tree.addNode(arr)].id;

        // Siblings after the union that the growth will swallow.
        for (int off : {0xC, 0x10}) {
            Node f; f.kind = NodeKind::Hex32;
            f.name = QStringLiteral("f%1").arg(off, 0, 16);
            f.parentId = rootId; f.offset = off;
            m_doc->tree.addNode(f);
        }
        m_ctrl->setViewRootId(rootId);
        m_ctrl->refresh();

        // +10h on the array member: 4 + 16 = 20 elements → member size 0x14
        // → union span [0x4, 0x18) swallows 0xC and 0x10.
        emit m_editor->appendBytesRequested(arrId, 0x10);

        QCOMPARE(m_doc->tree.unionSize(uid), 0x14);
        int at8 = 0, atC = 0;
        for (int i = 0; i < m_doc->tree.nodes.size(); i++) {
            const Node& n = m_doc->tree.nodes[i];
            if (n.parentId == uid && n.kind == NodeKind::Hex32) {
                if (n.offset == 8)        at8++;   // 0xC - 4
                else if (n.offset == 0xC) atC++;   // 0x10 - 4
            }
        }
        QCOMPARE(at8, 1);
        QCOMPARE(atC, 1);
        QVERIFY(m_doc->tree.findOverlaps().isEmpty());

        // ONE undo restores arrayLen AND pushes both siblings back out.
        m_doc->undoStack.undo();
        int ai = m_doc->tree.indexOfId(arrId);
        QVERIFY(ai >= 0);
        QCOMPARE(m_doc->tree.nodes[ai].arrayLen, 4);
        QCOMPARE(m_doc->tree.unionSize(uid), 4);
        int sibs = 0;
        for (int i = 0; i < m_doc->tree.nodes.size(); i++) {
            const Node& n = m_doc->tree.nodes[i];
            if (n.parentId == rootId && n.kind == NodeKind::Hex32) {
                sibs++;
                QVERIFY2(n.offset == 0xC || n.offset == 0x10,
                    qPrintable(QString("sibling back at unexpected offset 0x%1")
                                   .arg(n.offset, 0, 16)));
            }
        }
        QCOMPARE(sibs, 2);
    }

    void testUnionAbsorbCascadesLargerSibling() {
        // Finding 2 (scrutinize): absorbing a sibling LARGER than the
        // union's current C-size grows the union (unionSize = max member
        // size), exposing further siblings that were outside the old span.
        // absorbUnionOverlaps must loop until a full pass absorbs nothing.
        //
        // Pre-state (broken layout, as produced by the old extent-based exe
        // or a paste bug): union [0,4) with a Hex32 member, then a Hex128
        // sibling at 4 sized 0x10 and a Hex32 sibling at 0xC overlapping it.
        // Growing the member to Hex64 (span [0,8)) absorbs the Hex128 at 4;
        // that 0x10 member grows the union to [0,0x10), which then swallows
        // the sibling at 0xC — only reachable via a second pass.
        m_doc->tree.nodes.clear();
        m_doc->tree.invalidateIdCache();
        Node root; root.kind = NodeKind::Struct; root.structTypeName = "R";
        root.parentId = 0; root.collapsed = false;
        uint64_t rootId = m_doc->tree.nodes[m_doc->tree.addNode(root)].id;

        Node u; u.kind = NodeKind::Struct; u.name = "u"; u.classKeyword = "union";
        u.parentId = rootId; u.offset = 0;
        uint64_t uid = m_doc->tree.nodes[m_doc->tree.addNode(u)].id;
        Node a; a.kind = NodeKind::Hex32; a.name = "a";
        a.parentId = uid; a.offset = 0;
        m_doc->tree.addNode(a);

        Node s1; s1.kind = NodeKind::Hex128; s1.name = "s1";
        s1.parentId = rootId; s1.offset = 4;
        m_doc->tree.addNode(s1);
        Node s2; s2.kind = NodeKind::Hex32; s2.name = "s2";
        s2.parentId = rootId; s2.offset = 0xC;
        m_doc->tree.addNode(s2);
        m_ctrl->setViewRootId(rootId);
        m_ctrl->refresh();

        // Grow the member 4 → 8 (Hex64): first pass span [0, 8).
        int mi = -1;
        for (int i = 0; i < m_doc->tree.nodes.size(); i++)
            if (m_doc->tree.nodes[i].name == "a") { mi = i; break; }
        QVERIFY(mi >= 0);
        m_ctrl->changeNodeKind(mi, NodeKind::Hex64);

        // s1 absorbed (starts at 4 < 8) and — via the loop — s2 absorbed too
        // (after s1 grows the union to [0, 0x10), s2 at 0xC is inside).
        QCOMPARE(m_doc->tree.unionSize(uid), 0x10);
        int inU = 0, rel4 = 0, relC = 0;
        for (int i = 0; i < m_doc->tree.nodes.size(); i++) {
            const Node& n = m_doc->tree.nodes[i];
            if (n.parentId == uid) {
                inU++;
                if (n.offset == 4)  rel4++;
                if (n.offset == 0xC) relC++;
            }
        }
        QCOMPARE(inU, 3);   // member a + s1 + s2 all inside the union
        QCOMPARE(rel4, 1);
        QCOMPARE(relC, 1);
        QVERIFY(m_doc->tree.findOverlaps().isEmpty());
    }

    void testBatchChangeKind() {
        // Change field_u32 and field_float to Hex64
        QVector<int> indices;
        for (int i = 0; i < m_doc->tree.nodes.size(); i++) {
            const auto& n = m_doc->tree.nodes[i];
            if (n.name == "field_u32" || n.name == "field_float")
                indices.append(i);
        }
        QCOMPARE(indices.size(), 2);
        m_ctrl->batchChangeKind(indices, NodeKind::Hex64);
        // Both should now be Hex64
        for (const auto& n : m_doc->tree.nodes) {
            if (n.name == "field_u32" || n.name == "field_float")
                QCOMPARE(n.kind, NodeKind::Hex64);
        }
    }

    void testConvertToTypedPointer() {
        // Convert field_hex (Hex32) to typed pointer
        uint64_t hexId = 0;
        for (const auto& n : m_doc->tree.nodes)
            if (n.name == "field_hex") { hexId = n.id; break; }
        QVERIFY(hexId != 0);

        m_ctrl->convertToTypedPointer(hexId);
        int ni = m_doc->tree.indexOfId(hexId);
        QVERIFY(ni >= 0);
        // Should be a pointer now with a refId
        const auto& node = m_doc->tree.nodes[ni];
        QVERIFY(node.kind == NodeKind::Pointer64 || node.kind == NodeKind::Pointer32);
        QVERIFY(node.refId != 0);
    }

    void testRenameNodeUndoRedo() {
        uint64_t u8Id = 0;
        for (const auto& n : m_doc->tree.nodes)
            if (n.name == "field_u8") { u8Id = n.id; break; }
        QVERIFY(u8Id != 0);

        int idx = m_doc->tree.indexOfId(u8Id);
        m_ctrl->renameNode(idx, "renamed_field");
        QCOMPARE(m_doc->tree.nodes[m_doc->tree.indexOfId(u8Id)].name, QStringLiteral("renamed_field"));

        m_doc->undoStack.undo();
        QCOMPARE(m_doc->tree.nodes[m_doc->tree.indexOfId(u8Id)].name, QStringLiteral("field_u8"));

        m_doc->undoStack.redo();
        QCOMPARE(m_doc->tree.nodes[m_doc->tree.indexOfId(u8Id)].name, QStringLiteral("renamed_field"));
    }

    void testInsertNodeAboveShiftsOffsets() {
        // Find field_float at offset 4
        int floatIdx = -1;
        for (int i = 0; i < m_doc->tree.nodes.size(); i++)
            if (m_doc->tree.nodes[i].name == "field_float") { floatIdx = i; break; }
        QVERIFY(floatIdx >= 0);
        QCOMPARE(m_doc->tree.nodes[floatIdx].offset, 4);

        // Insert 8 bytes above field_float → should shift float to offset 12
        m_ctrl->insertNodeAbove(floatIdx, NodeKind::Hex64, "inserted");

        // Re-find field_float (index may have changed)
        for (const auto& n : m_doc->tree.nodes) {
            if (n.name == "field_float")
                QCOMPARE(n.offset, 12);  // shifted by 8
        }
    }

    void testDeleteRootStruct() {
        // Add a second root struct, then delete it
        uint64_t rootId = m_doc->tree.nodes[0].id;
        rcx::Node root2;
        root2.kind = NodeKind::Struct;
        root2.structTypeName = "Deletable";
        root2.name = "del";
        root2.parentId = 0;
        int ri = m_doc->tree.addNode(root2);
        uint64_t r2Id = m_doc->tree.nodes[ri].id;
        int countBefore = m_doc->tree.nodes.size();

        m_ctrl->deleteRootStruct(r2Id);
        QVERIFY(m_doc->tree.indexOfId(r2Id) < 0);
        QVERIFY(m_doc->tree.nodes.size() < countBefore);
        // Original root should still exist
        QVERIFY(m_doc->tree.indexOfId(rootId) >= 0);
    }

    void testMoveNodeSwapsOffsets() {
        // Find field_u32 (offset 0) and field_float (offset 4) — adjacent siblings
        uint64_t u32Id = 0, floatId = 0;
        for (const auto& n : m_doc->tree.nodes) {
            if (n.name == "field_u32") u32Id = n.id;
            if (n.name == "field_float") floatId = n.id;
        }
        QVERIFY(u32Id != 0 && floatId != 0);
        QCOMPARE(m_doc->tree.nodes[m_doc->tree.indexOfId(u32Id)].offset, 0);
        QCOMPARE(m_doc->tree.nodes[m_doc->tree.indexOfId(floatId)].offset, 4);

        // Sort siblings to find their order, then simulate move down
        uint64_t rootId = m_doc->tree.nodes[0].id;
        auto siblings = m_doc->tree.childrenOf(rootId);
        std::sort(siblings.begin(), siblings.end(), [&](int a, int b) {
            return m_doc->tree.nodes[a].offset < m_doc->tree.nodes[b].offset;
        });
        int u32Idx = m_doc->tree.indexOfId(u32Id);
        int pos = siblings.indexOf(u32Idx);
        QVERIFY(pos >= 0 && pos + 1 < siblings.size());
        int swapIdx = siblings[pos + 1];

        // Swap offsets (what moveNodeRequested does)
        m_doc->undoStack.beginMacro("swap");
        m_doc->undoStack.push(new RcxCommand(m_ctrl,
            cmd::ChangeOffset{u32Id, 0, 4}));
        m_doc->undoStack.push(new RcxCommand(m_ctrl,
            cmd::ChangeOffset{m_doc->tree.nodes[swapIdx].id, 4, 0}));
        m_doc->undoStack.endMacro();

        QCOMPARE(m_doc->tree.nodes[m_doc->tree.indexOfId(u32Id)].offset, 4);
        QCOMPARE(m_doc->tree.nodes[m_doc->tree.indexOfId(floatId)].offset, 0);

        // Undo should restore original offsets
        m_doc->undoStack.undo();
        QCOMPARE(m_doc->tree.nodes[m_doc->tree.indexOfId(u32Id)].offset, 0);
        QCOMPARE(m_doc->tree.nodes[m_doc->tree.indexOfId(floatId)].offset, 4);
    }

    void testChangeBaseAddress() {
        uint64_t oldBase = m_doc->tree.baseAddress;
        m_doc->undoStack.push(new RcxCommand(m_ctrl,
            cmd::ChangeBase{oldBase, 0x7FF600000000ULL, QString(), QString()}));
        QCOMPARE(m_doc->tree.baseAddress, 0x7FF600000000ULL);

        m_doc->undoStack.undo();
        QCOMPARE(m_doc->tree.baseAddress, oldBase);
    }

    void testChangeArrayMeta() {
        // Add an array node, then change its element kind and length
        uint64_t rootId = m_doc->tree.nodes[0].id;
        Node arr;
        arr.kind = NodeKind::Array;
        arr.name = "testArr";
        arr.parentId = rootId;
        arr.offset = 100;
        arr.elementKind = NodeKind::UInt8;
        arr.arrayLen = 10;
        arr.id = m_doc->tree.reserveId();
        m_doc->undoStack.push(new RcxCommand(m_ctrl, cmd::Insert{arr}));

        uint64_t arrId = arr.id;
        m_doc->undoStack.push(new RcxCommand(m_ctrl,
            cmd::ChangeArrayMeta{arrId, NodeKind::UInt8, NodeKind::Float, 10, 4}));

        int ai = m_doc->tree.indexOfId(arrId);
        QCOMPARE(m_doc->tree.nodes[ai].elementKind, NodeKind::Float);
        QCOMPARE(m_doc->tree.nodes[ai].arrayLen, 4);

        m_doc->undoStack.undo();
        ai = m_doc->tree.indexOfId(arrId);
        QCOMPARE(m_doc->tree.nodes[ai].elementKind, NodeKind::UInt8);
        QCOMPARE(m_doc->tree.nodes[ai].arrayLen, 10);
    }

    void testChangeClassKeyword() {
        uint64_t rootId = m_doc->tree.nodes[0].id;
        QString oldKw = m_doc->tree.nodes[0].resolvedClassKeyword();
        m_doc->undoStack.push(new RcxCommand(m_ctrl,
            cmd::ChangeClassKeyword{rootId, oldKw, QStringLiteral("class")}));
        QCOMPARE(m_doc->tree.nodes[m_doc->tree.indexOfId(rootId)].classKeyword,
                 QStringLiteral("class"));

        m_doc->undoStack.undo();
        QCOMPARE(m_doc->tree.nodes[m_doc->tree.indexOfId(rootId)].resolvedClassKeyword(), oldKw);
    }

    void testChangeComment() {
        uint64_t u32Id = 0;
        for (const auto& n : m_doc->tree.nodes)
            if (n.name == "field_u32") { u32Id = n.id; break; }
        QVERIFY(u32Id != 0);

        m_doc->undoStack.push(new RcxCommand(m_ctrl,
            cmd::ChangeComment{u32Id, QString(), QStringLiteral("health points")}));
        QCOMPARE(m_doc->tree.nodes[m_doc->tree.indexOfId(u32Id)].comment,
                 QStringLiteral("health points"));

        m_doc->undoStack.undo();
        QVERIFY(m_doc->tree.nodes[m_doc->tree.indexOfId(u32Id)].comment.isEmpty());
    }

    void testCollapseExpandAll() {
        // Expand root first
        uint64_t rootId = m_doc->tree.nodes[0].id;
        int ri = m_doc->tree.indexOfId(rootId);
        QCOMPARE(m_doc->tree.nodes[ri].collapsed, false);

        // Collapse all
        m_ctrl->setSuppressRefresh(true);
        m_doc->undoStack.beginMacro("collapse");
        for (auto& n : m_doc->tree.nodes)
            if (isContainerKind(n.kind) && !n.collapsed)
                m_doc->undoStack.push(new RcxCommand(m_ctrl, cmd::Collapse{n.id, false, true}));
        m_doc->undoStack.endMacro();
        m_ctrl->setSuppressRefresh(false);

        QCOMPARE(m_doc->tree.nodes[m_doc->tree.indexOfId(rootId)].collapsed, true);

        // Expand all
        m_ctrl->setSuppressRefresh(true);
        m_doc->undoStack.beginMacro("expand");
        for (auto& n : m_doc->tree.nodes)
            if (isContainerKind(n.kind) && n.collapsed)
                m_doc->undoStack.push(new RcxCommand(m_ctrl, cmd::Collapse{n.id, true, false}));
        m_doc->undoStack.endMacro();
        m_ctrl->setSuppressRefresh(false);

        QCOMPARE(m_doc->tree.nodes[m_doc->tree.indexOfId(rootId)].collapsed, false);

        // Undo should re-collapse
        m_doc->undoStack.undo();
        QCOMPARE(m_doc->tree.nodes[m_doc->tree.indexOfId(rootId)].collapsed, true);
    }

    void testNullptrPointerDisplay() {
        // Verify fmtPointer64(0) returns "nullptr"
        QCOMPARE(rcx::fmt::fmtPointer64(0), QStringLiteral("nullptr"));
        QCOMPARE(rcx::fmt::fmtPointer32(0), QStringLiteral("nullptr"));
        // Non-zero still returns hex
        QVERIFY(rcx::fmt::fmtPointer64(0x400000).startsWith("0x"));
        QVERIFY(rcx::fmt::fmtPointer32(0x1000).startsWith("0x"));
    }

    void testBatchRemoveMultipleNodes() {
        int before = m_doc->tree.nodes.size();
        uint64_t id1 = 0, id2 = 0;
        for (const auto& n : m_doc->tree.nodes) {
            if (n.name == "field_u32") id1 = n.id;
            if (n.name == "field_float") id2 = n.id;
        }
        QVERIFY(id1 != 0 && id2 != 0);

        QVector<int> indices;
        indices.append(m_doc->tree.indexOfId(id1));
        indices.append(m_doc->tree.indexOfId(id2));
        m_ctrl->batchRemoveNodes(indices);

        QVERIFY(m_doc->tree.indexOfId(id1) < 0);
        QVERIFY(m_doc->tree.indexOfId(id2) < 0);
        QVERIFY(m_doc->tree.nodes.size() < before);

        // Undo should restore both
        m_doc->undoStack.undo();
        QVERIFY(m_doc->tree.indexOfId(id1) >= 0);
        QVERIFY(m_doc->tree.indexOfId(id2) >= 0);
    }

    void testSetNodeValueBool() {
        // Add a Bool field and write true/false
        uint64_t rootId = m_doc->tree.nodes[0].id;
        Node boolNode; boolNode.kind = NodeKind::Bool;
        boolNode.name = "alive"; boolNode.parentId = rootId;
        boolNode.offset = 50; boolNode.id = m_doc->tree.reserveId();
        m_doc->undoStack.push(new RcxCommand(m_ctrl, cmd::Insert{boolNode}));
        int bi = m_doc->tree.indexOfId(boolNode.id);
        QVERIFY(bi >= 0);
        // Write true — should succeed (provider is writable)
        m_ctrl->setNodeValue(bi, 0, QStringLiteral("true"));
        // Read back
        uint8_t val = m_doc->provider->readU8(50);
        QCOMPARE(val, (uint8_t)1);
    }

    void testSetNodeValueNegativeInt() {
        // Write -128 to an Int8 field
        uint64_t rootId = m_doc->tree.nodes[0].id;
        Node i8; i8.kind = NodeKind::Int8; i8.name = "temp";
        i8.parentId = rootId; i8.offset = 51; i8.id = m_doc->tree.reserveId();
        m_doc->undoStack.push(new RcxCommand(m_ctrl, cmd::Insert{i8}));
        int idx = m_doc->tree.indexOfId(i8.id);
        QVERIFY(idx >= 0);
        m_ctrl->setNodeValue(idx, 0, QStringLiteral("-128"));
        int8_t val = (int8_t)m_doc->provider->readU8(51);
        QCOMPARE(val, (int8_t)-128);
    }

    void testValueHistoryClear() {
        ValueHistory vh;
        vh.record(QStringLiteral("1"));
        vh.record(QStringLiteral("2"));
        QCOMPARE(vh.uniqueCount(), 2);
        vh.clear();
        QCOMPARE(vh.uniqueCount(), 0);
        QCOMPARE(vh.heatLevel(), 0);
    }

    void testMultiSelectBatchCycleType() {
        // Select field_u32 (UInt32) and field_float (Float) — both 4 bytes
        // Batch change to Hex32 should work on both
        uint64_t u32Id = 0, floatId = 0;
        for (const auto& n : m_doc->tree.nodes) {
            if (n.name == "field_u32") u32Id = n.id;
            if (n.name == "field_float") floatId = n.id;
        }
        QVERIFY(u32Id != 0 && floatId != 0);
        QVector<int> indices;
        indices.append(m_doc->tree.indexOfId(u32Id));
        indices.append(m_doc->tree.indexOfId(floatId));
        m_ctrl->batchChangeKind(indices, NodeKind::Hex32);
        QCOMPARE(m_doc->tree.nodes[m_doc->tree.indexOfId(u32Id)].kind, NodeKind::Hex32);
        QCOMPARE(m_doc->tree.nodes[m_doc->tree.indexOfId(floatId)].kind, NodeKind::Hex32);
        // Undo
        m_doc->undoStack.undo();
        QCOMPARE(m_doc->tree.nodes[m_doc->tree.indexOfId(u32Id)].kind, NodeKind::UInt32);
        QCOMPARE(m_doc->tree.nodes[m_doc->tree.indexOfId(floatId)].kind, NodeKind::Float);
    }

    void testNodeToJsonOmitsDefaults() {
        Node n;
        n.id = 1; n.kind = NodeKind::Int32; n.name = "x";
        QJsonObject json = n.toJson();
        // isRelative defaults to false
        QVERIFY(!json.contains("isRelative"));
        // ptrDepth defaults to 0
        QVERIFY(!json.contains("ptrDepth"));
    }

    void testNodeToJsonIncludesIsRelative() {
        Node n;
        n.id = 1; n.kind = NodeKind::Pointer64; n.name = "rva";
        n.isRelative = true;
        QJsonObject json = n.toJson();
        QVERIFY(json.contains("isRelative"));
        QCOMPARE(json["isRelative"].toBool(), true);
    }

    void testCycleExcludesStringAndVectorTypes() {
        // Build variant list for 1-byte types (Hex8 origin)
        // Should NOT contain UTF8 (string type)
        NodeKind cur = NodeKind::Hex8;
        int sz = sizeForKind(cur);
        bool curStr = isStringKind(cur);
        bool curVec = isVectorKind(cur);
        QVector<NodeKind> variants;
        for (const auto& m : kKindMeta) {
            if (m.size != sz || isContainerKind(m.kind)) continue;
            if (!curStr && isStringKind(m.kind)) continue;
            if (!curVec && isVectorKind(m.kind)) continue;
            variants.append(m.kind);
        }
        QVERIFY(!variants.contains(NodeKind::UTF8));
        QVERIFY(variants.contains(NodeKind::Hex8));
        QVERIFY(variants.contains(NodeKind::Int8));
        QVERIFY(variants.contains(NodeKind::Bool));

        // 8-byte from Hex64 should NOT contain Vec2
        cur = NodeKind::Hex64;
        sz = sizeForKind(cur);
        curVec = isVectorKind(cur);
        variants.clear();
        for (const auto& m : kKindMeta) {
            if (m.size != sz || isContainerKind(m.kind)) continue;
            if (!curStr && isStringKind(m.kind)) continue;
            if (!curVec && isVectorKind(m.kind)) continue;
            variants.append(m.kind);
        }
        QVERIFY(!variants.contains(NodeKind::Vec2));
        QVERIFY(variants.contains(NodeKind::Hex64));
        QVERIFY(variants.contains(NodeKind::Double));

        // But FROM Vec2, Vec2 should be in the list
        cur = NodeKind::Vec2;
        curVec = isVectorKind(cur);
        variants.clear();
        for (const auto& m : kKindMeta) {
            if (m.size != 8 || isContainerKind(m.kind)) continue;
            if (!isStringKind(cur) && isStringKind(m.kind)) continue;
            if (!curVec && isVectorKind(m.kind)) continue;
            variants.append(m.kind);
        }
        QVERIFY(variants.contains(NodeKind::Vec2));
    }

    void testSpaceResizeWrapAndMultiSelect() {
        // Test Space wrap: Hex128 → Space → Hex8
        // (Hex128 is index 4, next is index 0 = Hex8)
        static constexpr NodeKind hexCycle[] = {
            NodeKind::Hex8, NodeKind::Hex16, NodeKind::Hex32,
            NodeKind::Hex64, NodeKind::Hex128 };
        int hi = 4; // Hex128
        NodeKind next = hexCycle[(hi + 1) % 5];
        QCOMPARE(next, NodeKind::Hex8);

        // Test reverse: Hex8 → Shift+Space → Hex128
        hi = 0; // Hex8
        NodeKind prev = hexCycle[(hi - 1 + 5) % 5];
        QCOMPARE(prev, NodeKind::Hex128);

        // Test multi-select batch: change 2 Hex32 fields together
        uint64_t rootId = m_doc->tree.nodes[0].id;
        // Use field_hex (Hex32 at offset 12) — already in test tree
        uint64_t hexId = 0;
        for (const auto& n : m_doc->tree.nodes)
            if (n.name == "field_hex") { hexId = n.id; break; }
        QVERIFY(hexId != 0);

        // Add a second Hex32
        Node h2; h2.kind = NodeKind::Hex32; h2.name = "hex2";
        h2.parentId = rootId; h2.offset = 16; h2.id = m_doc->tree.reserveId();
        m_doc->undoStack.push(new RcxCommand(m_ctrl, cmd::Insert{h2}));

        // Batch change both to Hex64
        QVector<int> indices;
        indices.append(m_doc->tree.indexOfId(hexId));
        indices.append(m_doc->tree.indexOfId(h2.id));
        m_ctrl->batchChangeKind(indices, NodeKind::Hex64);

        QCOMPARE(m_doc->tree.nodes[m_doc->tree.indexOfId(hexId)].kind, NodeKind::Hex64);
        QCOMPARE(m_doc->tree.nodes[m_doc->tree.indexOfId(h2.id)].kind, NodeKind::Hex64);
    }

    // ── Space hex resize stress tests ──

    void testSpaceCycleFullCircle() {
        // Create a clean tree with a single hex64 at offset 0
        rcx::NodeTree tree;
        tree.baseAddress = 0;
        rcx::Node root; root.kind = NodeKind::Struct;
        root.structTypeName = "Test"; root.name = "t"; root.collapsed = false;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        rcx::Node h; h.kind = NodeKind::Hex64; h.name = "field";
        h.parentId = rootId; h.offset = 0;
        int hi = tree.addNode(h);
        uint64_t origId = tree.nodes[hi].id;

        // Setup controller with this tree
        auto doc = new RcxDocument();
        doc->tree = tree;
        QByteArray buf(64, '\0');
        doc->provider = std::make_unique<BufferProvider>(buf);
        auto* splitter = new QSplitter();
        auto* ctrl = new RcxController(doc, nullptr);
        ctrl->addSplitEditor(splitter);

        // Verify initial state
        QCOMPARE(doc->tree.nodes[doc->tree.indexOfId(origId)].kind, NodeKind::Hex64);
        QCOMPARE(doc->tree.nodes[doc->tree.indexOfId(origId)].offset, 0);

        // Step 1: hex64 → hex8 (shrink)
        int ni = doc->tree.indexOfId(origId);
        ctrl->changeNodeKind(ni, NodeKind::Hex8);
        ni = doc->tree.indexOfId(origId);
        QVERIFY2(ni >= 0, "Node ID should survive shrink");
        QCOMPARE(doc->tree.nodes[ni].kind, NodeKind::Hex8);
        QCOMPARE(doc->tree.nodes[ni].offset, 0);

        // Count total children — should be more than 1 (hex8 + padding)
        auto kids = doc->tree.childrenOf(rootId);
        QVERIFY2(kids.size() > 1, qPrintable(QString("Expected padding nodes, got %1 kids").arg(kids.size())));

        // Verify no overlapping offsets
        QSet<int> offsets;
        int totalBytes = 0;
        for (int ci : kids) {
            const auto& n = doc->tree.nodes[ci];
            QVERIFY2(!offsets.contains(n.offset),
                qPrintable(QString("OVERLAP at offset %1").arg(n.offset)));
            offsets.insert(n.offset);
            totalBytes += sizeForKind(n.kind);
        }
        QCOMPARE(totalBytes, 8);  // original 8 bytes preserved

        // Step 2: hex8 → hex16 (join with adjacent hex8)
        ni = doc->tree.indexOfId(origId);
        ctrl->joinHexNodes(origId, NodeKind::Hex16);
        // origId is gone — find the new node at offset 0
        uint64_t newId = 0;
        for (const auto& n : doc->tree.nodes)
            if (n.parentId == rootId && n.offset == 0) { newId = n.id; break; }
        QVERIFY2(newId != 0, "Should find joined node at offset 0");
        QCOMPARE(doc->tree.nodes[doc->tree.indexOfId(newId)].kind, NodeKind::Hex16);

        // Step 3: hex16 → hex32
        ctrl->joinHexNodes(newId, NodeKind::Hex32);
        newId = 0;
        for (const auto& n : doc->tree.nodes)
            if (n.parentId == rootId && n.offset == 0) { newId = n.id; break; }
        QVERIFY2(newId != 0, "Should find joined node at offset 0");
        QCOMPARE(doc->tree.nodes[doc->tree.indexOfId(newId)].kind, NodeKind::Hex32);

        // Step 4: hex32 → hex64
        ctrl->joinHexNodes(newId, NodeKind::Hex64);
        newId = 0;
        for (const auto& n : doc->tree.nodes)
            if (n.parentId == rootId && n.offset == 0) { newId = n.id; break; }
        QVERIFY2(newId != 0, "Should find joined node at offset 0");
        QCOMPARE(doc->tree.nodes[doc->tree.indexOfId(newId)].kind, NodeKind::Hex64);

        // Verify we're back to 1 child
        kids = doc->tree.childrenOf(rootId);
        QCOMPARE(kids.size(), 1);
        QCOMPARE(doc->tree.nodes[kids[0]].offset, 0);
        QCOMPARE(sizeForKind(doc->tree.nodes[kids[0]].kind), 8);

        delete ctrl;
        delete splitter;
        delete doc;
    }

    void testSpaceNoOverlapAfterGrow() {
        // hex64 at +0, hex32 at +8 (different kinds, adjacent)
        // Join hex64+hex32 should NOT create overlap
        rcx::NodeTree tree;
        tree.baseAddress = 0;
        rcx::Node root; root.kind = NodeKind::Struct;
        root.structTypeName = "T"; root.name = "t"; root.collapsed = false;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        rcx::Node h1; h1.kind = NodeKind::Hex64; h1.name = "a";
        h1.parentId = rootId; h1.offset = 0;
        tree.addNode(h1);
        rcx::Node h2; h2.kind = NodeKind::Hex32; h2.name = "b";
        h2.parentId = rootId; h2.offset = 8;
        tree.addNode(h2);
        rcx::Node h3; h3.kind = NodeKind::Hex32; h3.name = "c";
        h3.parentId = rootId; h3.offset = 12;
        tree.addNode(h3);

        // Total: 16 bytes at +0..+15

        auto doc = new RcxDocument();
        doc->tree = tree;
        QByteArray buf(64, '\0');
        doc->provider = std::make_unique<BufferProvider>(buf);
        auto* splitter = new QSplitter();
        auto* ctrl = new RcxController(doc, nullptr);
        ctrl->addSplitEditor(splitter);

        // Try to join h1 (hex64 @0) with neighbors to make... well,
        // joinHexNodes needs tgtSz > curSz. No hex kind > 8 except hex128.
        // Skip this — test joining hex32+hex32 → hex64 instead
        uint64_t h2Id = doc->tree.nodes[2].id;  // hex32 at +8
        uint64_t h3Id = doc->tree.nodes[3].id;  // hex32 at +12

        // Verify no overlap before
        auto kids = doc->tree.childrenOf(rootId);
        QSet<int> offsBefore;
        for (int ci : kids) {
            QVERIFY2(!offsBefore.contains(doc->tree.nodes[ci].offset),
                "Overlap before join");
            offsBefore.insert(doc->tree.nodes[ci].offset);
        }

        // Join hex32@8 + hex32@12 → hex64@8
        ctrl->joinHexNodes(h2Id, NodeKind::Hex64);

        // Verify no overlap after
        kids = doc->tree.childrenOf(rootId);
        QSet<int> offsAfter;
        for (int ci : kids) {
            QVERIFY2(!offsAfter.contains(doc->tree.nodes[ci].offset),
                qPrintable(QString("OVERLAP at +%1 after join").arg(doc->tree.nodes[ci].offset)));
            offsAfter.insert(doc->tree.nodes[ci].offset);
        }

        // Should now be: hex64@0 + hex64@8 = 2 nodes
        QCOMPARE(kids.size(), 2);

        delete ctrl;
        delete splitter;
        delete doc;
    }

    void testSpaceSelectionSurvivesJoin() {
        rcx::NodeTree tree;
        tree.baseAddress = 0;
        rcx::Node root; root.kind = NodeKind::Struct;
        root.structTypeName = "T"; root.name = "t"; root.collapsed = false;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        rcx::Node h1; h1.kind = NodeKind::Hex32; h1.name = "a";
        h1.parentId = rootId; h1.offset = 0;
        int i1 = tree.addNode(h1);
        rcx::Node h2; h2.kind = NodeKind::Hex32; h2.name = "b";
        h2.parentId = rootId; h2.offset = 4;
        tree.addNode(h2);

        auto doc = new RcxDocument();
        doc->tree = tree;
        QByteArray buf(64, '\0');
        doc->provider = std::make_unique<BufferProvider>(buf);
        auto* splitter = new QSplitter();
        auto* ctrl = new RcxController(doc, nullptr);
        ctrl->addSplitEditor(splitter);

        // Select the first node
        uint64_t h1Id = doc->tree.nodes[i1].id;
        ctrl->handleNodeClick(ctrl->primaryEditor(), 1, h1Id, Qt::NoModifier);
        QVERIFY(ctrl->selectedIds().contains(h1Id));

        // Join hex32@0 + hex32@4 → hex64@0
        ctrl->joinHexNodes(h1Id, NodeKind::Hex64);

        // Selection should have transferred to the new joined node
        QVERIFY2(!ctrl->selectedIds().isEmpty(),
            "Selection should not be empty after join");
        // The new node should be at offset 0 with kind Hex64
        uint64_t selId = *ctrl->selectedIds().begin();
        int selIdx = doc->tree.indexOfId(selId);
        QVERIFY(selIdx >= 0);
        QCOMPARE(doc->tree.nodes[selIdx].kind, NodeKind::Hex64);
        QCOMPARE(doc->tree.nodes[selIdx].offset, 0);

        delete ctrl;
        delete splitter;
        delete doc;
    }

    // Repro (REAL mouse+key events — the programmatic path missed this):
    // click a node row to select it, press Delete → the node is deleted but
    // the selection must NOT jump to whatever node shifts up into the vacated
    // slot. User: "it auto-selects after delete; it should deselect entirely."
    void testDeleteClearsSelection() {
        rcx::NodeTree tree;
        tree.baseAddress = 0;
        rcx::Node root; root.kind = NodeKind::Struct;
        root.structTypeName = "T"; root.name = "t"; root.collapsed = false;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;
        QVector<uint64_t> ids;
        for (int i = 0; i < 12; ++i) {
            rcx::Node n; n.kind = NodeKind::Hex32;
            n.name = QStringLiteral("f%1").arg(i);
            n.parentId = rootId; n.offset = i * 4;
            ids.append(tree.nodes[tree.addNode(n)].id);
        }

        auto doc = new RcxDocument();
        doc->tree = tree;
        doc->provider = std::make_unique<BufferProvider>(QByteArray(64, '\0'));
        auto* splitter = new QSplitter();
        auto* ctrl = new RcxController(doc, nullptr);
        auto* editor = ctrl->addSplitEditor(splitter);
        splitter->resize(900, 400);
        splitter->show();
        QVERIFY(QTest::qWaitForWindowExposed(splitter));
        ctrl->refresh();
        QApplication::processEvents();

        auto* sci = editor->scintilla();
        auto* vp = sci->viewport();

        auto lineForId = [&](uint64_t id) -> int {
            for (int i = 0; ; ++i) {
                const LineMeta* lm = editor->metaForLine(i);
                if (!lm) return -1;
                if (lm->nodeId == id && lm->lineKind == LineKind::Field) return i;
            }
        };
        // A point inside the TYPE column of a row (col 12) — left of the hex
        // value bytes, so the click selects the node without arming a byte
        // drag-selection.
        auto rowPoint = [&](int line) -> QPoint {
            long pos = sci->SendScintilla(QsciScintillaBase::SCI_FINDCOLUMN,
                                          (unsigned long)line, (long)12);
            int x = (int)sci->SendScintilla(QsciScintillaBase::SCI_POINTXFROMPOSITION, 0UL, pos);
            int y = (int)sci->SendScintilla(QsciScintillaBase::SCI_POINTYFROMPOSITION, 0UL, pos);
            return QPoint(x, y + 2);
        };
        auto sendPress = [&](QPoint p, Qt::KeyboardModifiers mods) {
            QMouseEvent press(QEvent::MouseButtonPress, QPointF(p), QPointF(p),
                              Qt::LeftButton, Qt::LeftButton, mods);
            QApplication::sendEvent(vp, &press);
            QApplication::processEvents();
        };
        auto sendRelease = [&](QPoint p, Qt::KeyboardModifiers mods) {
            QMouseEvent rel(QEvent::MouseButtonRelease, QPointF(p), QPointF(p),
                            Qt::LeftButton, Qt::NoButton, mods);
            QApplication::sendEvent(vp, &rel);
            QApplication::processEvents();
        };
        auto clickRow = [&](int line, Qt::KeyboardModifiers mods) {
            QPoint p = rowPoint(line);
            sendPress(p, mods);
            sendRelease(p, mods);
        };
        auto sendDelete = [&]() {
            QKeyEvent k(QEvent::KeyPress, Qt::Key_Delete, Qt::NoModifier);
            QApplication::sendEvent(sci, &k);
            QApplication::processEvents();
        };
        auto dumpSel = [&](const char* when) {
            QStringList s;
            for (uint64_t id : ctrl->selectedIds()) s << QString::number(id, 16);
            qDebug("%s: %d selected [%s]", when, ctrl->selectedIds().size(),
                   qPrintable(s.join(',')));
        };

        // ── Single click + Delete ──
        uint64_t f2 = ids[2];
        clickRow(lineForId(f2), Qt::NoModifier);
        dumpSel("after click f2");
        QVERIFY2(ctrl->selectedIds().contains(f2),
            qPrintable(QString("click didn't select f2 (got %1 ids)").arg(ctrl->selectedIds().size())));
        sendDelete();
        dumpSel("after delete f2");
        QCOMPARE(doc->tree.indexOfId(f2), -1);
        QVERIFY2(ctrl->selectedIds().isEmpty(),
            qPrintable(QString("single-delete left %1 id(s) selected (auto-reselect bug)")
                .arg(ctrl->selectedIds().size())));

        // ── Multi-select (shift-click range) + Delete ──
        ctrl->refresh();
        QApplication::processEvents();
        uint64_t g1 = ids[1], g3 = ids[3];   // f1..f3 still exist (f0,f2 deleted? no: only f2 gone)
        // f2 was deleted above; remaining: f0,f1,f3,f4,f5. Select f1..f4 range.
        uint64_t r0 = ids[1], r1 = ids[4];
        clickRow(lineForId(r0), Qt::NoModifier);
        clickRow(lineForId(r1), Qt::ShiftModifier);
        dumpSel("after shift-range select");
        QVERIFY2(ctrl->selectedIds().size() >= 2,
            qPrintable(QString("shift-range selected only %1").arg(ctrl->selectedIds().size())));
        sendDelete();
        dumpSel("after delete range");
        QVERIFY2(ctrl->selectedIds().isEmpty(),
            qPrintable(QString("multi-delete left %1 id(s) selected (auto-reselect bug)")
                .arg(ctrl->selectedIds().size())));
        (void)g1; (void)g3;

        // ── Stale click after delete must NOT select the shifted node ──
        // The root defect: a deferred/queued nodeClicked can arrive after a
        // delete shifted the rows, carrying the deleted node's id + its OLD
        // line. handleNodeClick's effectiveId() derives the selection from the
        // row at that line, so without a guard it selects whatever node moved
        // up into that line. Replay that exact stale click deterministically.
        ctrl->refresh();
        QApplication::processEvents();
        int l0 = -1, l1 = -1; uint64_t id0 = 0;
        for (int i = 0; ; ++i) {
            const LineMeta* lm = editor->metaForLine(i);
            if (!lm) break;
            if (lm->lineKind == LineKind::Field && lm->nodeId) {
                if (l0 < 0) { l0 = i; id0 = lm->nodeId; }
                else { l1 = i; break; }
            }
        }
        QVERIFY(l0 >= 0 && l1 >= 0);     // need a node that will shift up
        clickRow(l0, Qt::NoModifier);
        QVERIFY(ctrl->selectedIds().contains(id0));
        sendDelete();
        QCOMPARE(doc->tree.indexOfId(id0), -1);
        QVERIFY(ctrl->selectedIds().isEmpty());
        // Replay the stale deferred click: deleted id0 + its old line l0,
        // which now shows the shifted-up node. Must be ignored, not selected.
        ctrl->handleNodeClick(editor, l0, id0, Qt::NoModifier);
        dumpSel("after stale click replay");
        QVERIFY2(ctrl->selectedIds().isEmpty(),
            qPrintable(QString("stale click after delete auto-selected %1 node(s)")
                .arg(ctrl->selectedIds().size())));

        delete ctrl;
        delete splitter;
        delete doc;
    }

    void testSpaceRapidCycleNoCorruption() {
        // Simulate pressing Space 20 times rapidly on a hex64
        // The tree should not have overlapping offsets at any point
        rcx::NodeTree tree;
        tree.baseAddress = 0;
        rcx::Node root; root.kind = NodeKind::Struct;
        root.structTypeName = "T"; root.name = "t"; root.collapsed = false;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        rcx::Node h; h.kind = NodeKind::Hex64; h.name = "field";
        h.parentId = rootId; h.offset = 0;
        tree.addNode(h);

        auto doc = new RcxDocument();
        doc->tree = tree;
        QByteArray buf(64, '\0');
        doc->provider = std::make_unique<BufferProvider>(buf);
        auto* splitter = new QSplitter();
        auto* ctrl = new RcxController(doc, nullptr);
        ctrl->addSplitEditor(splitter);

        // The hex cycle: 8→16→32→64→8→16→...
        static constexpr NodeKind hexCycle[] = {
            NodeKind::Hex8, NodeKind::Hex16, NodeKind::Hex32, NodeKind::Hex64 };
        int curCycleIdx = 3;  // start at hex64

        for (int press = 0; press < 20; press++) {
            int nextCycleIdx = (curCycleIdx + 1) % 4;
            NodeKind target = hexCycle[nextCycleIdx];

            // Find the node at offset 0
            uint64_t nodeId = 0;
            for (const auto& n : doc->tree.nodes)
                if (n.parentId == rootId && n.offset == 0 && isHexNode(n.kind))
                    { nodeId = n.id; break; }
            QVERIFY2(nodeId != 0, qPrintable(QString("No hex node at offset 0 on press %1").arg(press)));

            int ni = doc->tree.indexOfId(nodeId);
            NodeKind curKind = doc->tree.nodes[ni].kind;
            int curSz = sizeForKind(curKind);
            int tgtSz = sizeForKind(target);

            if (tgtSz > curSz)
                ctrl->joinHexNodes(nodeId, target);
            else if (tgtSz < curSz)
                ctrl->changeNodeKind(ni, target);

            // Verify NO overlapping offsets
            doc->tree.invalidateIdCache();
            auto kids = doc->tree.childrenOf(rootId);
            QMap<int, NodeKind> offMap;
            for (int ci : kids) {
                const auto& n = doc->tree.nodes[ci];
                if (offMap.contains(n.offset)) {
                    QString dump;
                    for (int di : kids)
                        dump += QStringLiteral("  +%1 %2(%3)\n")
                            .arg(doc->tree.nodes[di].offset)
                            .arg(kindToString(doc->tree.nodes[di].kind))
                            .arg(sizeForKind(doc->tree.nodes[di].kind));
                    QVERIFY2(false, qPrintable(
                        QStringLiteral("OVERLAP at +%1 on press %2: %3 vs %4\nAll nodes:\n%5")
                            .arg(n.offset).arg(press)
                            .arg(kindToString(offMap.value(n.offset, NodeKind::Hex8)))
                            .arg(kindToString(n.kind)).arg(dump)));
                }
                offMap[n.offset] = n.kind;
            }

            // Verify total bytes = 8 (always)
            int total = 0;
            for (int ci : kids)
                total += sizeForKind(doc->tree.nodes[ci].kind);
            QCOMPARE(total, 8);

            curCycleIdx = nextCycleIdx;
        }

        delete ctrl;
        delete splitter;
        delete doc;
    }

    // ── Autosave shadow path: must NEVER stack ".autosave" suffixes ──
    void testAutosaveShadowPath() {
        // Plain project file → single suffix
        QCOMPARE(autosaveShadowPath(QStringLiteral("C:/data/foo.rcx")),
                 QStringLiteral("C:/data/foo.rcx.autosave"));
        // A shadow opened directly → stays a single suffix, no stacking
        QCOMPARE(autosaveShadowPath(QStringLiteral("C:/data/foo.rcx.autosave")),
                 QStringLiteral("C:/data/foo.rcx.autosave"));
        // Path already clobbered by the old buggy autosave → collapsed
        QCOMPARE(autosaveShadowPath(
                     QStringLiteral("C:/data/foo.rcx.autosave.autosave.autosave")),
                 QStringLiteral("C:/data/foo.rcx.autosave"));
        // ".autosave" in the middle of the name is not a suffix → untouched
        QCOMPARE(autosaveShadowPath(QStringLiteral("C:/data/foo.autosave.rcx")),
                 QStringLiteral("C:/data/foo.autosave.rcx.autosave"));
        // Windows-style path
        QCOMPARE(autosaveShadowPath(QStringLiteral("C:\\data\\foo.rcx")),
                 QStringLiteral("C:\\data\\foo.rcx.autosave"));
    }

    // The autosave writer must be a pure snapshot: it leaves filePath,
    // modified and the undo stack untouched, so repeated rounds write the
    // same shadow file instead of stacking suffixes and a later Ctrl+S
    // still targets the real file.
    void testAutosaveDoesNotMutateDocument() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString real = dir.filePath(QStringLiteral("foo.rcx"));

        RcxDocument doc;
        doc.filePath = real;
        rcx::Node root; root.kind = NodeKind::Struct;
        root.structTypeName = "T"; root.name = "t";
        doc.tree.addNode(root);
        // Dirty undo stack → doc counts as modified
        doc.undoStack.push(new QUndoCommand(QStringLiteral("edit")));
        QVERIFY(doc.modified);
        QVERIFY(doc.undoStack.canUndo());

        // Round 1 — the exact call MainWindow::autosaveAllModifiedDocs makes
        QVERIFY(doc.saveCopy(autosaveShadowPath(doc.filePath)));
        QVERIFY(QFile::exists(real + QStringLiteral(".autosave")));
        QCOMPARE(doc.filePath, real);              // still points at the real file
        QVERIFY(doc.modified);                     // still dirty (snapshot, not save)
        QVERIFY(doc.undoStack.canUndo());          // undo history preserved

        // Round 2 — same shadow path, no stacked suffix
        QVERIFY(doc.saveCopy(autosaveShadowPath(doc.filePath)));
        QVERIFY(QFile::exists(real + QStringLiteral(".autosave")));
        QVERIFY(!QFile::exists(real + QStringLiteral(".autosave.autosave")));
        QCOMPARE(doc.filePath, real);

        // Round 3 — even a doc opened from a shadow (filePath ending in
        // .autosave) writes that same path instead of stacking
        doc.filePath = real + QStringLiteral(".autosave");
        QVERIFY(doc.saveCopy(autosaveShadowPath(doc.filePath)));
        QVERIFY(!QFile::exists(real + QStringLiteral(".autosave.autosave")));
        QCOMPARE(doc.filePath, real + QStringLiteral(".autosave"));
    }

    // ── Virtual-function (vftable) block ──
    void testAddVirtualFunctionCreatesBlockAndShiftsFields() {
        // Root struct with fields at 0/4/8... — first "Add Virtual
        // Function" must create the __vptr block at offset 0 and shift
        // every existing field down by one pointer (8 on x64).
        uint64_t rootId = m_doc->tree.nodes[0].id;
        QVERIFY(!m_ctrl->classHasVftable(rootId));

        auto before = m_doc->tree.childrenOf(rootId);
        QVERIFY(before.size() >= 3);
        const int field0Before = m_doc->tree.nodes[before[0]].offset;

        uint64_t blockId = m_ctrl->addVirtualFunction(rootId);
        QVERIFY(blockId != 0);
        QVERIFY(m_ctrl->classHasVftable(rootId));

        int bi = m_doc->tree.indexOfId(blockId);
        QVERIFY(bi >= 0);
        const Node& block = m_doc->tree.nodes[bi];
        QVERIFY(block.isVftable());
        QCOMPARE(block.kind, NodeKind::Pointer64);
        QCOMPARE(block.name, QStringLiteral("__vptr"));
        QCOMPARE(block.offset, 0);

        // Exactly one entry fn0 at slot 0.
        auto kids = m_doc->tree.childrenOf(blockId);
        QCOMPARE(kids.size(), 1);
        QCOMPARE(m_doc->tree.nodes[kids[0]].kind, NodeKind::FuncPtr64);
        QCOMPARE(m_doc->tree.nodes[kids[0]].name, QStringLiteral("fn0"));
        QCOMPARE(m_doc->tree.nodes[kids[0]].offset, 0);

        // Old fields shifted down by 8; the first one now starts at 8.
        int field0After = -1;
        for (int ci : m_doc->tree.childrenOf(rootId)) {
            const Node& n = m_doc->tree.nodes[ci];
            if (n.name == QStringLiteral("field_u32"))
                field0After = n.offset;
        }
        QCOMPARE(field0After, field0Before + 8);
    }

    void testAddVirtualFunctionAppendsEntries() {
        uint64_t rootId = m_doc->tree.nodes[0].id;
        uint64_t blockId = m_ctrl->addVirtualFunction(rootId);
        QVERIFY(blockId != 0);

        m_ctrl->appendVirtualFunction(blockId);
        m_ctrl->appendVirtualFunction(blockId);

        auto kids = m_doc->tree.childrenOf(blockId);
        QCOMPARE(kids.size(), 3);
        QCOMPARE(m_doc->tree.nodes[kids[0]].offset, 0);
        QCOMPARE(m_doc->tree.nodes[kids[1]].offset, 8);
        QCOMPARE(m_doc->tree.nodes[kids[2]].offset, 16);
        QCOMPARE(m_doc->tree.nodes[kids[2]].name, QStringLiteral("fn2"));
    }

    void testRemoveVftableBlockRestoresOffsets() {
        uint64_t rootId = m_doc->tree.nodes[0].id;
        int field0Before = -1;
        for (int ci : m_doc->tree.childrenOf(rootId)) {
            const Node& n = m_doc->tree.nodes[ci];
            if (n.name == QStringLiteral("field_u32"))
                field0Before = n.offset;
        }

        uint64_t blockId = m_ctrl->addVirtualFunction(rootId);
        m_ctrl->appendVirtualFunction(blockId);
        m_ctrl->removeVftableBlock(blockId);

        QVERIFY(!m_ctrl->classHasVftable(rootId));
        int field0After = -1;
        for (int ci : m_doc->tree.childrenOf(rootId)) {
            const Node& n = m_doc->tree.nodes[ci];
            if (n.name == QStringLiteral("field_u32"))
                field0After = n.offset;
        }
        QCOMPARE(field0After, field0Before);
    }

    void testAddVirtualFunctionRefusesUnion() {
        Node root; root.kind = NodeKind::Struct;
        root.name = QStringLiteral("u");
        root.structTypeName = QStringLiteral("U");
        root.classKeyword = QStringLiteral("union");
        root.parentId = 0; root.offset = 0;
        m_doc->tree.addNode(root);
        uint64_t uid = m_doc->tree.nodes.last().id;

        uint64_t blockId = m_ctrl->addVirtualFunction(uid);
        QCOMPARE(blockId, (uint64_t)0);
        QVERIFY(!m_ctrl->classHasVftable(uid));
    }

    void testAddVirtualFunctionUndoRestoresOffsets() {
        uint64_t rootId = m_doc->tree.nodes[0].id;
        int field0Before = -1;
        for (int ci : m_doc->tree.childrenOf(rootId)) {
            const Node& n = m_doc->tree.nodes[ci];
            if (n.name == QStringLiteral("field_u32"))
                field0Before = n.offset;
        }

        uint64_t blockId = m_ctrl->addVirtualFunction(rootId);
        QVERIFY(blockId != 0);
        QVERIFY(m_ctrl->classHasVftable(rootId));

        m_doc->undoStack.undo();
        QVERIFY(!m_ctrl->classHasVftable(rootId));
        int field0After = -1;
        for (int ci : m_doc->tree.childrenOf(rootId)) {
            const Node& n = m_doc->tree.nodes[ci];
            if (n.name == QStringLiteral("field_u32"))
                field0After = n.offset;
        }
        QCOMPARE(field0After, field0Before);
    }

    void testAddVirtualFunction32Bit() {
        m_doc->tree.pointerSize = 4;
        uint64_t rootId = m_doc->tree.nodes[0].id;

        uint64_t blockId = m_ctrl->addVirtualFunction(rootId);
        QVERIFY(blockId != 0);

        int bi = m_doc->tree.indexOfId(blockId);
        QVERIFY(bi >= 0);
        QCOMPARE(m_doc->tree.nodes[bi].kind, NodeKind::Pointer32);

        auto kids = m_doc->tree.childrenOf(blockId);
        QCOMPARE(kids.size(), 1);
        QCOMPARE(m_doc->tree.nodes[kids[0]].kind, NodeKind::FuncPtr32);

        // Fields shifted down by 4, not 8.
        int field0After = -1;
        for (int ci : m_doc->tree.childrenOf(rootId)) {
            const Node& n = m_doc->tree.nodes[ci];
            if (n.name == QStringLiteral("field_u32"))
                field0After = n.offset;
        }
        QCOMPARE(field0After, 4);

        m_ctrl->appendVirtualFunction(blockId);
        kids = m_doc->tree.childrenOf(blockId);
        QCOMPARE(kids.size(), 2);
        QCOMPARE(m_doc->tree.nodes[kids[1]].offset, 4);
    }

    void testAppendVirtualFunctionSkipsRenamedSlot() {
        uint64_t rootId = m_doc->tree.nodes[0].id;
        uint64_t blockId = m_ctrl->addVirtualFunction(rootId);
        auto kids = m_doc->tree.childrenOf(blockId);
        QCOMPARE(kids.size(), 1);

        // Rename fn0 -> speak, then append twice. The first append should
        // reuse fn1 (free), the second must NOT collide with a renamed entry.
        int fi = m_doc->tree.indexOfId(m_doc->tree.nodes[kids[0]].id);
        m_ctrl->renameNode(fi, QStringLiteral("speak"));
        m_ctrl->appendVirtualFunction(blockId);
        m_ctrl->appendVirtualFunction(blockId);

        auto after = m_doc->tree.childrenOf(blockId);
        QCOMPARE(after.size(), 3);
        QStringList names;
        for (int ci : after)
            names << m_doc->tree.nodes[ci].name;
        QVERIFY(names.contains(QStringLiteral("speak")));
        QVERIFY(names.contains(QStringLiteral("fn1")));
        QVERIFY(names.contains(QStringLiteral("fn0")));
    }

    void testVfSignatureRoundTrip() {
        uint64_t rootId = m_doc->tree.nodes[0].id;
        uint64_t blockId = m_ctrl->addVirtualFunction(rootId);
        auto kids = m_doc->tree.childrenOf(blockId);
        uint64_t fnId = m_doc->tree.nodes[kids[0]].id;

        // Direct tree mutation is how editVfSignature commits its dialog
        // (dialog itself is interactive); verify the storage + JSON round-trip.
        int fi = m_doc->tree.indexOfId(fnId);
        m_doc->tree.nodes[fi].vfReturnType = QStringLiteral("int");
        m_doc->tree.nodes[fi].vfParams = QStringLiteral("DWORD dw");

        RcxDocument copy;
        QTemporaryFile f;
        QVERIFY(f.open());
        const QString path = f.fileName();
        f.close();
        QVERIFY(m_doc->save(path));
        QVERIFY(copy.load(path));

        int ci = copy.tree.indexOfId(fnId);
        QVERIFY(ci >= 0);
        QCOMPARE(copy.tree.nodes[ci].vfReturnType, QStringLiteral("int"));
        QCOMPARE(copy.tree.nodes[ci].vfParams, QStringLiteral("DWORD dw"));
    }

    // save() keeps its original contract: write the file AND make the
    // document reflect that save (filePath retargeted, undo stack clean).
    void testSaveStillRetargetsDocument() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString real = dir.filePath(QStringLiteral("foo.rcx"));

        RcxDocument doc;
        doc.filePath = real;
        rcx::Node root; root.kind = NodeKind::Struct;
        root.structTypeName = "T"; root.name = "t";
        doc.tree.addNode(root);

        QVERIFY(doc.save(real));
        QVERIFY(QFile::exists(real));
        QCOMPARE(doc.filePath, real);
        QVERIFY(!doc.modified);
        QVERIFY(!doc.undoStack.canUndo());
    }
};

QTEST_MAIN(TestController)
#include "test_controller.moc"
