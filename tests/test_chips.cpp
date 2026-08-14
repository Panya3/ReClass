// Unit tests for the unified tail-chip model (LineMeta::chips).
//
// Each compose pass attaches a vector of chips to every Field/Header
// LineMeta that has annotations to display: Enum, TypeHint, Rtti,
// Comment. The editor reads these for indicator colors, hit-testing,
// and inline-edit span lookup.
//
// These tests target the chip *data* path — they don't run the editor
// because chip rendering is a Scintilla concern verified by test_editor.
// What we lock down here:
//   - Each chip kind emits when its preconditions hold and is suppressed
//     when its View toggle is false.
//   - Chip text carries the right glyph prefix ((), [], {}, /).
//   - startCol / endCol bracket the chip's text in the rendered line.
//   - Chip order on a single line is Enum → TypeHint → Rtti → Comment
//     (the order the editor's indicator pass and click router expect).

#include <QtTest/QTest>
#include <QByteArray>
#include <cstring>
#include "core.h"
#include "providers/buffer_provider.h"

using namespace rcx;

namespace {

// Lay out an address space with: a synthetic vtable in module range so
// the RTTI walker's MSVC path catches it, plus a struct region past the
// module that holds whatever fields the test plants.
constexpr uint64_t kImageBase  = 0x10000;
constexpr uint64_t kStructBase = 0x30000;
constexpr uint32_t kVtableRva  = 0x1000;
constexpr uint32_t kColRva     = 0x1900;
constexpr uint32_t kTdRva      = 0x1100;
constexpr uint32_t kChdRva     = 0x1400;
constexpr uint32_t kBcaRva     = 0x1500;
constexpr uint32_t kBcdRva     = 0x1600;

template<class T>
void writeAt(QByteArray& buf, qsizetype at, T value) {
    std::memcpy(buf.data() + at, &value, sizeof(T));
}

// Mini RTTI fixture — just enough for walkRtti's MSVC path to demangle a
// single class name "Foo". Mirrors test_rtti_hint.cpp's shape.
QByteArray buildRttiAddressSpace() {
    QByteArray buf(kStructBase + 0x1000, '\0');
    // Vtable[-1] = COL VA
    writeAt<uint64_t>(buf, kImageBase + kVtableRva - 8, kImageBase + kColRva);
    // Type descriptor with mangled name ".?AVFoo@@" (MSVC mangling for class Foo)
    writeAt<uint64_t>(buf, kImageBase + kTdRva + 0, 0xDEADBEEF);
    writeAt<uint64_t>(buf, kImageBase + kTdRva + 8, 0);
    const char* mangled = ".?AVFoo@@";
    std::memcpy(buf.data() + kImageBase + kTdRva + 16, mangled,
                std::strlen(mangled) + 1);
    // Class hierarchy descriptor: 1 base
    writeAt<uint32_t>(buf, kImageBase + kChdRva + 0x00, 0);
    writeAt<uint32_t>(buf, kImageBase + kChdRva + 0x04, 0);
    writeAt<uint32_t>(buf, kImageBase + kChdRva + 0x08, 1);
    writeAt<uint32_t>(buf, kImageBase + kChdRva + 0x0C, kBcaRva);
    // Base-class array → single base
    writeAt<uint32_t>(buf, kImageBase + kBcaRva + 0, kBcdRva);
    // Base-class descriptor → type descriptor for Foo itself
    writeAt<uint32_t>(buf, kImageBase + kBcdRva + 0, kTdRva);
    // Complete object locator: signature, offset, cdOffset, type-desc, chd, imageBase
    writeAt<uint32_t>(buf, kImageBase + kColRva + 0x00, 1);
    writeAt<uint32_t>(buf, kImageBase + kColRva + 0x04, 0);
    writeAt<uint32_t>(buf, kImageBase + kColRva + 0x08, 0);
    writeAt<uint32_t>(buf, kImageBase + kColRva + 0x0C, kTdRva);
    writeAt<uint32_t>(buf, kImageBase + kColRva + 0x10, kChdRva);
    writeAt<uint32_t>(buf, kImageBase + kColRva + 0x14, (uint32_t)kImageBase);
    return buf;
}

class FakeModuleProvider : public BufferProvider {
public:
    FakeModuleProvider(QByteArray d, const QString& n)
        : BufferProvider(std::move(d), n) {}
    QVector<ModuleEntry> enumerateModules() const override {
        return { ModuleEntry{ QStringLiteral("synthetic.dll"),
                              QStringLiteral("synthetic.dll"),
                              kImageBase, 0x10000 } };
    }
};

// Helper: count chips of a given kind across all meta lines.
int countChips(const ComposeResult& r, ChipKind k) {
    int n = 0;
    for (const auto& lm : r.meta)
        for (const auto& c : lm.chips)
            if (c.kind == k) ++n;
    return n;
}

// Helper: find first line that has a chip of `k`, returning the chip.
const LineChip* firstChipOfKind(const ComposeResult& r, ChipKind k) {
    for (const auto& lm : r.meta)
        if (auto* c = findChip(lm, k))
            return c;
    return nullptr;
}

} // anon

class TestChips : public QObject {
    Q_OBJECT
private slots:

    // ── Enum chip emits on int field whose refId resolves to an enum,
    //    and is suppressed by showEnumChips=false ──
    void enumChipFiresAndCanBeSuppressed() {
        NodeTree tree;
        tree.baseAddress = kStructBase;

        Node enumNode;
        enumNode.kind = NodeKind::Struct;
        enumNode.classKeyword = QStringLiteral("enum");
        enumNode.structTypeName = QStringLiteral("Status");
        enumNode.name = QStringLiteral("Status");
        enumNode.enumMembers = {
            {QStringLiteral("READY"),   0},
            {QStringLiteral("RUNNING"), 1},
            {QStringLiteral("DONE"),    2},
        };
        int ei = tree.addNode(enumNode);
        uint64_t enumId = tree.nodes[ei].id;

        Node root;
        root.kind = NodeKind::Struct;
        root.structTypeName = QStringLiteral("Holder");
        root.name = QStringLiteral("Holder");
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        Node field;
        field.kind = NodeKind::UInt32;
        field.name = QStringLiteral("status");
        field.parentId = rootId;
        field.offset = 0;
        field.refId = enumId;
        tree.addNode(field);

        QByteArray data(kStructBase + 16, '\0');
        uint32_t v = 1;  // RUNNING
        std::memcpy(data.data() + kStructBase, &v, 4);

        BufferProvider prov(std::move(data), QStringLiteral("synthetic"));

        // Default: enum chip fires. Value matched member RUNNING(1): the
        // pill carries "NAME (value)" and replaces the raw number.
        ComposeResult r = compose(tree, prov, rootId);
        const LineChip* c = firstChipOfKind(r, ChipKind::Enum);
        QVERIFY2(c, "enum chip should fire on int field with refId→enum");
        QCOMPARE(c->text, QStringLiteral("RUNNING (1)"));
        QCOMPARE(c->enumCurrentValue, (int64_t)1);
        QCOMPARE(c->enumRefNodeId, enumId);
        QVERIFY(c->startCol >= 0);
        QVERIFY(c->endCol > c->startCol);

        // The chip span brackets exactly the pill text in the rendered
        // line, and the line no longer ends with a bare numeric value.
        {
            QString pillLine;
            for (int i = 0; i < r.meta.size() && i < r.lineStarts.size(); i++) {
                if (!findChip(r.meta[i], ChipKind::Enum)) continue;
                int ls = r.lineStarts[i];
                int le = (i + 1 < r.lineStarts.size()) ? r.lineStarts[i + 1] - 1
                                                       : r.text.size();
                pillLine = r.text.mid(ls, le - ls);
                break;
            }
            QVERIFY2(!pillLine.isEmpty(), "found chip line for span check");
            QCOMPARE(pillLine.mid(c->startCol, c->endCol - c->startCol),
                     QStringLiteral("RUNNING (1)"));
            QVERIFY2(!pillLine.endsWith(QLatin1Char('1')),
                qPrintable(QStringLiteral("value column should not keep the "
                    "bare number after the pill; line was: ") + pillLine));
        }

        // Toggle off: chip suppressed.
        ComposeResult r2 = compose(tree, prov, rootId,
            /*compactColumns=*/false, /*treeLines=*/false,
            /*braceWrap=*/false, /*typeHints=*/false,
            /*showComments=*/true, /*symbolLookup=*/{},
            /*showRtti=*/true, /*showEnumChips=*/false);
        QCOMPARE(countChips(r2, ChipKind::Enum), 0);
    }

    // ── Value with no matching member → no pill, plain number stays ──
    void enumNoMatchShowsPlainNumber() {
        NodeTree tree;
        tree.baseAddress = kStructBase;

        Node enumNode;
        enumNode.kind = NodeKind::Struct;
        enumNode.classKeyword = QStringLiteral("enum");
        enumNode.structTypeName = QStringLiteral("Status");
        enumNode.name = QStringLiteral("Status");
        enumNode.enumMembers = {
            {QStringLiteral("READY"),   0},
            {QStringLiteral("RUNNING"), 1},
            {QStringLiteral("DONE"),    2},
        };
        int ei = tree.addNode(enumNode);
        uint64_t enumId = tree.nodes[ei].id;

        Node root;
        root.kind = NodeKind::Struct;
        root.structTypeName = QStringLiteral("Holder");
        root.name = QStringLiteral("Holder");
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        Node field;
        field.kind = NodeKind::UInt32;
        field.name = QStringLiteral("status");
        field.parentId = rootId;
        field.offset = 0;
        field.refId = enumId;
        tree.addNode(field);

        QByteArray data(kStructBase + 16, '\0');
        uint32_t v = 5;  // no member with value 5
        std::memcpy(data.data() + kStructBase, &v, 4);

        BufferProvider prov(std::move(data), QStringLiteral("synthetic"));

        ComposeResult r = compose(tree, prov, rootId);
        QCOMPARE(countChips(r, ChipKind::Enum), 0);

        // The value column still renders the plain number (line tail).
        bool sawNumber = false;
        for (int i = 0; i < r.meta.size() && i < r.lineStarts.size(); i++) {
            if (r.meta[i].nodeKind != NodeKind::UInt32) continue;
            int ls = r.lineStarts[i];
            int le = (i + 1 < r.lineStarts.size()) ? r.lineStarts[i + 1] - 1
                                                   : r.text.size();
            QString line = r.text.mid(ls, le - ls);
            int e = line.size();
            while (e > 0 && line[e - 1] == QLatin1Char(' ')) --e;
            int s = e;
            while (s > 0 && line[s - 1] != QLatin1Char(' ')) --s;
            // UInt32 renders in hex (fmtUInt32 → "0x…"); the unmatched
            // value must stay exactly as it would normally display.
            QCOMPARE(line.mid(s, e - s), QStringLiteral("0x5"));
            sawNumber = true;
        }
        QVERIFY2(sawNumber, "found the UInt32 field line");
    }

    // ── Signed field with negative value: parens show the unsigned form ──
    void enumNegativeValueShowsNonNegativeNumber() {
        NodeTree tree;
        tree.baseAddress = kStructBase;

        Node enumNode;
        enumNode.kind = NodeKind::Struct;
        enumNode.classKeyword = QStringLiteral("enum");
        enumNode.structTypeName = QStringLiteral("Err");
        enumNode.name = QStringLiteral("Err");
        enumNode.enumMembers = {
            {QStringLiteral("OK"),  0},
            {QStringLiteral("NEG"), -1},
        };
        int ei = tree.addNode(enumNode);
        uint64_t enumId = tree.nodes[ei].id;

        Node root;
        root.kind = NodeKind::Struct;
        root.structTypeName = QStringLiteral("Holder");
        root.name = QStringLiteral("Holder");
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        Node field;
        field.kind = NodeKind::Int32;
        field.name = QStringLiteral("err");
        field.parentId = rootId;
        field.offset = 0;
        field.refId = enumId;
        tree.addNode(field);

        QByteArray data(kStructBase + 16, '\0');
        int32_t v = -1;  // matches NEG
        std::memcpy(data.data() + kStructBase, &v, 4);

        BufferProvider prov(std::move(data), QStringLiteral("synthetic"));

        ComposeResult r = compose(tree, prov, rootId);
        const LineChip* c = firstChipOfKind(r, ChipKind::Enum);
        QVERIFY2(c, "enum chip should fire on signed int field");
        QCOMPARE(c->text, QStringLiteral("NEG (4294967295)"));
        QCOMPARE(c->enumCurrentValue, (int64_t)-1);
    }

    // ── Big-endian fields match on the DISPLAYED (swapped) value, so a
    //    match can never contradict the number the value column shows ──
    void enumBigEndianMatchesDisplayedValue() {
        NodeTree tree;
        tree.baseAddress = kStructBase;

        Node enumNode;
        enumNode.kind = NodeKind::Struct;
        enumNode.classKeyword = QStringLiteral("enum");
        enumNode.structTypeName = QStringLiteral("Endian");
        enumNode.name = QStringLiteral("Endian");
        // Bytes 01 00 00 00 read raw as 1 (would falsely match ONE); the
        // big-endian display is 0x01000000 → must match BIG instead.
        enumNode.enumMembers = {
            {QStringLiteral("ONE"), 1},
            {QStringLiteral("BIG"), 0x01000000},
        };
        int ei = tree.addNode(enumNode);
        uint64_t enumId = tree.nodes[ei].id;

        Node root;
        root.kind = NodeKind::Struct;
        root.structTypeName = QStringLiteral("Holder");
        root.name = QStringLiteral("Holder");
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        Node field;
        field.kind = NodeKind::UInt32;
        field.name = QStringLiteral("be");
        field.parentId = rootId;
        field.offset = 0;
        field.refId = enumId;
        field.bigEndian = true;
        tree.addNode(field);

        QByteArray data(kStructBase + 16, '\0');
        uint32_t v = 1;  // LE bytes: 01 00 00 00
        std::memcpy(data.data() + kStructBase, &v, 4);

        BufferProvider prov(std::move(data), QStringLiteral("synthetic"));

        ComposeResult r = compose(tree, prov, rootId);
        const LineChip* c = firstChipOfKind(r, ChipKind::Enum);
        QVERIFY2(c, "enum chip should fire on big-endian field");
        QCOMPARE(c->text, QStringLiteral("BIG (16777216)"));
        QCOMPARE(c->enumCurrentValue, (int64_t)0x01000000);
    }

    // ── Struct-kind enum field (the chooser models an enum pick as an
    //    inline Struct ref: kind=Struct + refId→enum + structTypeName).
    //    Such rows render as a header with no value column — the pill must
    //    still resolve the member name onto that row ──
    void enumStructLeafShowsPillOnHeader() {
        NodeTree tree;
        tree.baseAddress = kStructBase;

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
        int ei = tree.addNode(enumNode);
        uint64_t enumId = tree.nodes[ei].id;

        Node root;
        root.kind = NodeKind::Struct;
        root.structTypeName = QStringLiteral("PgIXmlObject");
        root.name = QStringLiteral("instance0");
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        Node field;
        field.kind = NodeKind::Struct;
        field.name = QStringLiteral("m_eID");
        field.parentId = rootId;
        field.offset = 32;
        field.refId = enumId;
        field.structTypeName = QStringLiteral("XmlObjectID");
        field.elementKind = NodeKind::UInt8;
        field.collapsed = true;
        tree.addNode(field);

        QByteArray data(kStructBase + 0x40, '\0');
        data[kStructBase + 32] = 2;  // ID_NPC
        BufferProvider prov(std::move(data), QStringLiteral("synthetic"));

        ComposeResult r = compose(tree, prov, rootId);
        const LineChip* c = firstChipOfKind(r, ChipKind::Enum);
        QVERIFY2(c, "enum pill should fire on Struct-kind enum ref header");
        QCOMPARE(c->text, QStringLiteral("ID_NPC (2)"));
        QCOMPARE(c->enumRefNodeId, enumId);
        QCOMPARE(c->enumCurrentValue, (int64_t)2);

        // Unmatched value on the same shape: still a pill, but showing the
        // raw number (hex, matching the value column) instead of a member
        // name — a blank header would re-create the "no value shown" bug
        // for the common uninitialized/non-member case, and the pill keeps
        // the row clickable so the picker's custom-value row can fix it.
        // (data was moved into prov — write through the provider's buffer.)
        prov.data()[kStructBase + 32] = 0x7F;
        ComposeResult r2 = compose(tree, prov, rootId);
        const LineChip* c2 = firstChipOfKind(r2, ChipKind::Enum);
        QVERIFY2(c2, "enum pill should still fire on no-match (raw value)");
        QCOMPARE(c2->text, QStringLiteral("0x7f"));  // hexVal uses lowercase
        QCOMPARE(c2->enumCurrentValue, (int64_t)0x7F);
    }

    // ── TypeHint chip fires on hex node with strong inference and is
    //    always emitted as an overlay-only chip (no inline text). The
    //    previous typeHints flag was retired when chips moved to the
    //    ChipOverlay widget — overlays are unobtrusive enough that
    //    always-on is fine. ──
    void typeHintChipFiresAsOverlay() {
        NodeTree tree;
        tree.baseAddress = kStructBase;

        Node root;
        root.kind = NodeKind::Struct;
        root.structTypeName = QStringLiteral("Holder");
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        Node hex;
        hex.kind = NodeKind::Hex64;
        hex.name = QStringLiteral("payload");
        hex.parentId = rootId;
        hex.offset = 0;
        tree.addNode(hex);

        // Plant two int32s side by side — inferTypes treats this as
        // int32×2 with strong confidence.
        QByteArray data(kStructBase + 16, '\0');
        int32_t a = 14;
        int32_t b = 20;
        std::memcpy(data.data() + kStructBase + 0, &a, 4);
        std::memcpy(data.data() + kStructBase + 4, &b, 4);
        BufferProvider prov(std::move(data), QStringLiteral("synthetic"));

        // typeHints defaults OFF (the inference scan is gated for perf) — opt
        // in explicitly to exercise the TypeHint chip.
        ComposeResult r = compose(tree, prov, rootId,
            /*compactColumns=*/false, /*treeLines=*/false,
            /*braceWrap=*/false, /*typeHints=*/true);
        const LineChip* c = firstChipOfKind(r, ChipKind::TypeHint);
        QVERIFY2(c, "type-inference chip should fire on hex node with strong inference");
        // Inline rendering: chip text is appended to lineText, startCol/endCol
        // bracket the range, indicator pass styles it as a clickable pill.
        QVERIFY2(c->startCol >= 0, "TypeHint chip must have a startCol set");
        QVERIFY2(c->endCol > c->startCol, "endCol must be past startCol");
        QVERIFY2(!c->text.contains('['),
            qPrintable(QStringLiteral("chip text should be plain (no brackets), got: ") + c->text));
        QVERIFY(!c->typeHintKinds.isEmpty());
    }

    // ── TypeHint memoization regression: with the per-compose inferTypes
    //    cache, MULTIPLE hex fields carrying the same strong-inference bytes
    //    must EACH still emit their own TypeHint chip (the cache stores the
    //    decision, not the chip placement). Two fields with identical bytes
    //    yield two identical chips. ──
    void typeHintMemoizationMultiNode() {
        NodeTree tree;
        tree.baseAddress = kStructBase;

        Node root;
        root.kind = NodeKind::Struct;
        root.structTypeName = QStringLiteral("Holder");
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        Node h0; h0.kind = NodeKind::Hex64; h0.name = QStringLiteral("a");
        h0.parentId = rootId; h0.offset = 0;  tree.addNode(h0);
        Node h1; h1.kind = NodeKind::Hex64; h1.name = QStringLiteral("b");
        h1.parentId = rootId; h1.offset = 8;  tree.addNode(h1);

        // Identical int32×2 bytes under each field → both infer the same type.
        QByteArray data(kStructBase + 24, '\0');
        int32_t a = 14, b = 20;
        for (int base : {0, 8}) {
            std::memcpy(data.data() + kStructBase + base + 0, &a, 4);
            std::memcpy(data.data() + kStructBase + base + 4, &b, 4);
        }
        BufferProvider prov(std::move(data), QStringLiteral("synthetic"));

        ComposeResult r = compose(tree, prov, rootId,
            /*compactColumns=*/false, /*treeLines=*/false,
            /*braceWrap=*/false, /*typeHints=*/true);

        QCOMPARE(countChips(r, ChipKind::TypeHint), 2);
        // Both chips must be fully populated (kinds carried through the cache).
        for (const auto& lm : r.meta)
            for (const auto& chip : lm.chips)
                if (chip.kind == ChipKind::TypeHint)
                    QVERIFY2(!chip.typeHintKinds.isEmpty(),
                             "memoized TypeHint chip lost its kinds");
    }

    // ── RTTI chip fires when a hex64's value points at a known vtable,
    //    and showRtti=false suppresses it ──
    void rttiChipFiresAndCanBeSuppressed() {
        QByteArray data = buildRttiAddressSpace();
        uint64_t vtable = kImageBase + kVtableRva;
        std::memcpy(data.data() + kStructBase, &vtable, 8);

        FakeModuleProvider prov(std::move(data), QStringLiteral("synthetic"));

        NodeTree tree;
        tree.baseAddress = kStructBase;
        Node root;
        root.kind = NodeKind::Struct;
        root.structTypeName = QStringLiteral("Demo");
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        Node f;
        f.kind = NodeKind::Hex64;
        f.name = QStringLiteral("vtbl");
        f.parentId = rootId;
        f.offset = 0;
        tree.addNode(f);

        ComposeResult rOn = compose(tree, prov, rootId);
        const LineChip* c = firstChipOfKind(rOn, ChipKind::Rtti);
        QVERIFY2(c, "RTTI chip should fire on hex64 whose value is a vtable");
        // Plain demangled name — indicator pass styles it as a pill, no
        // "{RTTI: …}" prefix needed. Symbol suffix dropped per
        // annotation-merge rule (RTTI supersedes Symbol).
        QCOMPARE(c->text, QStringLiteral("Foo"));
        QCOMPARE(c->rttiVtableAddr, vtable);
        QVERIFY2(c->startCol >= 0, "RTTI chip must have a startCol set");
        QVERIFY2(c->endCol > c->startCol, "endCol must be past startCol");

        ComposeResult rOff = compose(tree, prov, rootId,
            /*compactColumns=*/false, /*treeLines=*/false,
            /*braceWrap=*/false, /*typeHints=*/false,
            /*showComments=*/true, /*symbolLookup=*/{},
            /*showRtti=*/false);
        QCOMPARE(countChips(rOff, ChipKind::Rtti), 0);
    }

    // ── Comment chip fires for Node::comment, suppressed by showComments=false ──
    void commentChipFiresAndCanBeSuppressed() {
        NodeTree tree;
        tree.baseAddress = kStructBase;

        Node root;
        root.kind = NodeKind::Struct;
        root.structTypeName = QStringLiteral("Holder");
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        Node f;
        f.kind = NodeKind::UInt32;
        f.name = QStringLiteral("count");
        f.parentId = rootId;
        f.offset = 0;
        f.comment = QStringLiteral("ref count from header");
        tree.addNode(f);

        QByteArray data(kStructBase + 16, '\0');
        BufferProvider prov(std::move(data), QStringLiteral("synthetic"));

        ComposeResult rOn = compose(tree, prov, rootId);
        const LineChip* c = firstChipOfKind(rOn, ChipKind::Comment);
        QVERIFY2(c, "comment chip should fire when Node::comment is set");
        // Comment chip text is the raw comment — the green pill carries
        // the "this is a comment" signal, no glyph prefix needed.
        QCOMPARE(c->text, QStringLiteral("ref count from header"));

        // Suppressed when showComments=false.
        ComposeResult rOff = compose(tree, prov, rootId,
            /*compactColumns=*/false, /*treeLines=*/false,
            /*braceWrap=*/false, /*typeHints=*/false,
            /*showComments=*/false);
        QCOMPARE(countChips(rOff, ChipKind::Comment), 0);
    }

    // ── Own-address comment fallback is suppressed on pointer/fnptr rows ──
    // The value's Symbol chip already annotates where a pointer points, so a
    // second "module+0x<own-slot-rva>" comment was redundant ("why two
    // addresses" — user). Data fields keep the fallback; user comments always
    // show.
    void ownAddressCommentSuppressedOnPointers() {
        NodeTree tree;
        tree.baseAddress = kStructBase;
        Node root; root.kind = NodeKind::Struct;
        root.structTypeName = QStringLiteral("Holder");
        uint64_t rootId = tree.nodes[tree.addNode(root)].id;

        auto addField = [&](NodeKind k, const char* name, int off) {
            Node f; f.kind = k; f.name = QString::fromLatin1(name);
            f.parentId = rootId; f.offset = off;
            tree.addNode(f);
        };
        addField(NodeKind::UInt32,    "data",  0);   // data field — fallback fires
        addField(NodeKind::Pointer64, "ptr",   8);   // pointer — suppressed
        addField(NodeKind::FuncPtr64, "fnptr", 16);  // fnptr — suppressed

        QByteArray buf(kStructBase + 64, '\0');
        BufferProvider prov(std::move(buf), QStringLiteral("synthetic"));
        // No-PDB fallback form returned for every address.
        auto symLookup = [](uint64_t) -> QString { return QStringLiteral("REECLASS.exe+0x10"); };

        ComposeResult r = compose(tree, prov, rootId,
            /*compactColumns=*/false, /*treeLines=*/false, /*braceWrap=*/false,
            /*typeHints=*/false, /*showComments=*/true, symLookup);

        auto hasCommentOn = [&](const ComposeResult& res, const QString& nm) -> bool {
            for (const auto& lm : res.meta) {
                if (lm.nodeIdx < 0 || lm.nodeIdx >= tree.nodes.size()) continue;
                if (tree.nodes[lm.nodeIdx].name == nm && findChip(lm, ChipKind::Comment))
                    return true;
            }
            return false;
        };
        QVERIFY2(hasCommentOn(r, QStringLiteral("data")),
                 "data field should keep the own-address comment fallback");
        QVERIFY2(!hasCommentOn(r, QStringLiteral("ptr")),
                 "pointer row must NOT get the redundant own-address comment");
        QVERIFY2(!hasCommentOn(r, QStringLiteral("fnptr")),
                 "fnptr row must NOT get the redundant own-address comment");

        // A user-authored comment on a pointer ALWAYS shows.
        for (int i = 0; i < tree.nodes.size(); i++)
            if (tree.nodes[i].name == QStringLiteral("ptr"))
                tree.nodes[i].comment = QStringLiteral("the d_ptr");
        ComposeResult r2 = compose(tree, prov, rootId,
            false, false, false, false, /*showComments=*/true, symLookup);
        bool userComment = false;
        for (const auto& lm : r2.meta) {
            if (lm.nodeIdx < 0 || lm.nodeIdx >= tree.nodes.size()) continue;
            if (tree.nodes[lm.nodeIdx].name != QStringLiteral("ptr")) continue;
            const LineChip* c = findChip(lm, ChipKind::Comment);
            if (c && c->text == QStringLiteral("the d_ptr")) userComment = true;
        }
        QVERIFY2(userComment, "user-authored comment on a pointer must still show");
    }

    // ── Multi-line newlines in a comment collapse to a middle-dot
    //    separator (defensive against phantom Scintilla rows) ──
    void multilineCommentStaysOnOneLine() {
        NodeTree tree;
        tree.baseAddress = kStructBase;
        Node root;
        root.kind = NodeKind::Struct;
        root.structTypeName = QStringLiteral("X");
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;
        Node f;
        f.kind = NodeKind::UInt32;
        f.name = QStringLiteral("v");
        f.parentId = rootId;
        f.offset = 0;
        f.comment = QStringLiteral("first line\nsecond line\nthird");
        tree.addNode(f);
        QByteArray data(kStructBase + 16, '\0');
        BufferProvider prov(std::move(data), QStringLiteral("synthetic"));
        ComposeResult r = compose(tree, prov, rootId);
        const LineChip* c = firstChipOfKind(r, ChipKind::Comment);
        QVERIFY(c);
        QVERIFY2(!c->text.contains(QChar('\n')),
            qPrintable(QStringLiteral("comment chip text must not contain newlines: ")
                + c->text));
        QVERIFY(c->text.contains(QStringLiteral("first line")));
    }

    // ── Chip render order on a single line: Enum → TypeHint → Rtti → Comment ──
    // Build a row that triggers Comment + TypeHint + Rtti at once. (Enum
    // can't coexist with hex64 because Enum requires int kind; TypeHint /
    // Rtti both want hex64. Verify the three that *can* coexist.)
    void chipsAppearInDefinedOrder() {
        QByteArray data = buildRttiAddressSpace();
        uint64_t vtable = kImageBase + kVtableRva;
        std::memcpy(data.data() + kStructBase, &vtable, 8);
        FakeModuleProvider prov(std::move(data), QStringLiteral("synthetic"));

        NodeTree tree;
        tree.baseAddress = kStructBase;
        Node root;
        root.kind = NodeKind::Struct;
        root.structTypeName = QStringLiteral("Demo");
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;
        Node f;
        f.kind = NodeKind::Hex64;
        f.name = QStringLiteral("vtbl");
        f.parentId = rootId;
        f.offset = 0;
        f.comment = QStringLiteral("the vtable");
        tree.addNode(f);

        ComposeResult r = compose(tree, prov, rootId,
            /*compactColumns=*/false, /*treeLines=*/false,
            /*braceWrap=*/false, /*typeHints=*/true,
            /*showComments=*/true);

        // Find the field row (must have at least Rtti+Comment to be useful).
        const LineMeta* row = nullptr;
        for (const auto& lm : r.meta) {
            if (findChip(lm, ChipKind::Rtti) && findChip(lm, ChipKind::Comment)) {
                row = &lm; break;
            }
        }
        QVERIFY2(row, "expected a row carrying both Rtti + Comment chips");

        // Order check: for INLINE chips only (overlay chips have
        // startCol == -1 — they don't participate in the linear chip
        // strip), every adjacent pair by ChipKind enum order should
        // have monotonic startCol. Locks the editor's per-row click
        // router into a deterministic left-to-right layout.
        int prevStart = -1;
        ChipKind prevKind = ChipKind::Enum;
        for (const auto& c : row->chips) {
            if (c.startCol < 0) continue;  // skip overlay-only chips
            if (prevStart >= 0) {
                QVERIFY2(prevStart < c.startCol,
                    qPrintable(QStringLiteral("chip startCol non-monotonic: %1 then %2")
                        .arg(prevStart).arg(c.startCol)));
                QVERIFY2(static_cast<int>(prevKind) <= static_cast<int>(c.kind),
                    qPrintable(QStringLiteral("chip order broken: kind %1 followed by %2")
                        .arg((int)prevKind).arg((int)c.kind)));
            }
            prevStart = c.startCol;
            prevKind = c.kind;
        }

        // The Comment chip must be the last INLINE chip on the row —
        // user-authored text is always rightmost so the comment-edit
        // flow can strip from chip.startCol → end-of-line.
        const LineChip* lastInline = nullptr;
        for (const auto& c : row->chips) {
            if (c.startCol < 0) continue;
            lastInline = &c;
        }
        QVERIFY(lastInline);
        QCOMPARE(lastInline->kind, ChipKind::Comment);
    }

    // ── Chip startCol/endCol bracket their text in the rendered line ──
    void chipSpansMatchRenderedText() {
        NodeTree tree;
        tree.baseAddress = kStructBase;
        Node root;
        root.kind = NodeKind::Struct;
        root.structTypeName = QStringLiteral("Holder");
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;
        Node f;
        f.kind = NodeKind::UInt32;
        f.name = QStringLiteral("v");
        f.parentId = rootId;
        f.offset = 0;
        f.comment = QStringLiteral("this is a comment");
        tree.addNode(f);
        QByteArray data(kStructBase + 16, '\0');
        BufferProvider prov(std::move(data), QStringLiteral("synthetic"));
        ComposeResult r = compose(tree, prov, rootId);

        // Walk to the field's line. Its line text + startCol/endCol should
        // bracket the chip's text exactly.
        bool checked = false;
        for (int i = 0; i < r.meta.size(); ++i) {
            const auto& lm = r.meta[i];
            const LineChip* c = findChip(lm, ChipKind::Comment);
            if (!c) continue;
            int lineStart = (i < r.lineStarts.size()) ? r.lineStarts[i] : -1;
            int lineEnd   = (i + 1 < r.lineStarts.size())
                ? r.lineStarts[i + 1] - 1
                : r.text.size();
            QVERIFY(lineStart >= 0);
            QString lineText = r.text.mid(lineStart, lineEnd - lineStart);
            QVERIFY2(c->endCol <= lineText.size(),
                qPrintable(QStringLiteral("endCol %1 must be <= line length %2")
                    .arg(c->endCol).arg(lineText.size())));
            QString slice = lineText.mid(c->startCol, c->endCol - c->startCol);
            QCOMPARE(slice, c->text);
            checked = true;
        }
        QVERIFY2(checked, "test should have hit the comment-chip line");
    }
};

QTEST_MAIN(TestChips)
#include "test_chips.moc"
