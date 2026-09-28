// Drill-down trail (v3 — CLICK-driven focus path) and the AddressBar that
// renders it. The trail reflects where the selection sits in the inline-
// expanded tree: selecting a typed pointer (or any row inside its expansion)
// adds it; there is no follow-arrow affordance. Crumb click = collapse below
// + scroll. Exercises focusChainToNode / handleNodeClick / collapseToFocus /
// addressBarState on the controller side, and — RibbonBar-style — the
// AddressBar widget: geometry through itemRect(id), hover through a
// synthesized MouseMove, pixels through grab(). Never waits for window
// exposure (show() + qWait + processEvents is all that's needed), so the
// whole target runs on the hidden desktop.
//
// One hidden-desktop fact the P3 tests lean on: a Qt::Popup (the places
// QMenu) is closed by the platform at the first event pump after show(),
// so a menu is inspected and driven synchronously, before any pump. Focus
// is asserted through the focus chain (window()->focusWidget()), which is
// kept whether or not the window is active; the focus-out test hands the
// overlay the event itself if the platform delivered none.

#include <QtTest/QTest>
#include <QtTest/QSignalSpy>
#include <QApplication>
#include <QEvent>
#include <QFile>
#include <QFocusEvent>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QSettings>
#include <QSplitter>
#include <QVBoxLayout>
#include <QtEndian>
#include <Qsci/qsciscintilla.h>

#include "address_callbacks.h"
#include "controller.h"
#include "core.h"
#include "gotoaddressdialog.h"
#include "paintutil.h"
#include "providers/buffer_provider.h"
#include "sourcechooserpopup.h"
#include "widgets/address_bar.h"

using namespace rcx;

namespace {

bool sameColour(QRgb px, const QColor& c, int tol = 2) {
    return qAbs(qRed(px) - c.red()) <= tol && qAbs(qGreen(px) - c.green()) <= tol
        && qAbs(qBlue(px) - c.blue()) <= tol;
}

// Device-pixel rect of a logical rect in a grab() image.
QRect devRect(const QImage& img, const QRect& logical) {
    const qreal dpr = img.devicePixelRatio() > 0 ? img.devicePixelRatio() : 1.0;
    return QRect(qRound(logical.left() * dpr), qRound(logical.top() * dpr),
                 qRound(logical.width() * dpr), qRound(logical.height() * dpr));
}

// Number of pixels in `r` (device) that are exactly `c`.
int countColour(const QImage& img, const QRect& r, const QColor& c) {
    int n = 0;
    for (int y = r.top(); y <= r.bottom() && y < img.height(); ++y)
        for (int x = r.left(); x <= r.right() && x < img.width(); ++x)
            if (y >= 0 && x >= 0 && sameColour(img.pixel(x, y), c)) ++n;
    return n;
}

// Mean distance of the rect's pixels from the ground — a proxy for "how
// much ink is there" (the disabled-cell / dimmed-icon opacity probe).
// `exclude` (logical) masks a sub-rect out of the average — the liveness
// dot sits on the chip icon and is painted at full opacity either way.
double inkAmount(const QImage& img, const QRect& logical, const QColor& bg,
                 const QRect& exclude = QRect()) {
    const QRect r = devRect(img, logical);
    const QRect ex = exclude.isNull() ? QRect() : devRect(img, exclude);
    double sum = 0; int n = 0;
    for (int y = r.top(); y <= r.bottom() && y < img.height(); ++y)
        for (int x = r.left(); x <= r.right() && x < img.width(); ++x) {
            if (!ex.isNull() && ex.contains(x, y)) continue;
            const QRgb px = img.pixel(x, y);
            sum += qAbs(qRed(px) - bg.red()) + qAbs(qGreen(px) - bg.green()) + qAbs(qBlue(px) - bg.blue());
            ++n;
        }
    return n ? sum / n : 0.0;
}

// Plain hover move (no buttons) delivered straight to the widget.
void hoverAt(QWidget& w, const QPoint& pos) {
    QMouseEvent mv(QEvent::MouseMove, pos, w.mapToGlobal(pos), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&w, &mv);
}

QImage grabOf(QWidget& w) {
    QApplication::processEvents();
    return w.grab().toImage().convertToFormat(QImage::Format_ARGB32);
}

void showBar(AddressBar& bar, int width) {
    bar.resize(width, AddressBar::kAddressBarHeight);
    bar.show();
    QTest::qWait(30);
    QApplication::processEvents();
}

// A shipped theme by file name (test_ribbon_layout's helper), so a colour
// rule can be checked under both polarities regardless of what
// ThemeManager holds in this bare target.
Theme loadTheme(const QString& baseName) {
    QFile f(QStringLiteral(RCX_SOURCE_DIR) + QStringLiteral("/src/themes/defaults/") + baseName
            + QStringLiteral(".json"));
    if (!f.open(QIODevice::ReadOnly)) return Theme();
    return Theme::fromJson(QJsonDocument::fromJson(f.readAll()).object());
}

// The first device column (left → right) holding a pixel that is not the
// ground, scanning every row but the seam (the bottom device row is the
// hairline end to end). -1 when nothing is inked.
int firstInkColumn(const QImage& img, const QColor& bg) {
    for (int x = 0; x < img.width(); ++x)
        for (int y = 0; y < img.height() - 1; ++y) {
            const QRgb px = img.pixel(x, y);
            if (qAbs(qRed(px) - bg.red()) + qAbs(qGreen(px) - bg.green())
                + qAbs(qBlue(px) - bg.blue()) >= 12)
                return x;
        }
    return -1;
}

// The bar's bottom device row under a logical x-range: where the seam and,
// while editing, the focus ring live.
QRect seamRowUnder(const QImage& img, const QRect& logical) {
    const QRect d = devRect(img, logical);
    return QRect(d.left(), img.height() - 1, d.width(), 1);
}

QStringList actionTexts(const QMenu* menu) {
    QStringList out;
    for (QAction* a : menu->actions()) out << a->text();
    return out;
}
QAction* actionWithText(const QMenu* menu, const QString& text) {
    for (QAction* a : menu->actions())
        if (a->text() == text) return a;
    return nullptr;
}
// The bar frees a menu with deleteLater on hide, which a processEvents()
// at the test's loop level does not run — a hidden predecessor can still
// be a child. The one that is up is the one that is visible.
QMenu* visibleMenu(const QWidget* w, const QString& name) {
    for (QMenu* m : w->findChildren<QMenu*>(name))
        if (m->isVisible()) return m;
    return nullptr;
}

}  // namespace

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

// Crumbs in the production dotted shape (addressBarState): "Class.field"
// ancestors, a bare "Class" deepest, rootId = crumb index. classId is any
// non-zero id — the widget only reads label, index and address.
static Crumb crumb(const QString& label, uint64_t index, uint64_t classId = 1) {
    Crumb c; c.label = label; c.rootId = index; c.classId = classId;
    return c;
}
static QVector<Crumb> twoLevel() {
    return { crumb(QStringLiteral("RcxEditor.vptr"), 0, 1),
             crumb(QStringLiteral("QWidgetPrivate"), 1, 2) };
}
// A live process source over the given trail — what a real push looks like.
static AddressBarState stateWith(const QVector<Crumb>& crumbs) {
    AddressBarState s;
    s.sourceName   = QStringLiteral("REECLASS.exe");
    s.sourceKindId = QStringLiteral("processmemory");
    s.liveness     = liveness::Live;
    s.baseAddress  = 0x7FF600000000ULL;
    s.resolvedBase = s.baseAddress;
    s.crumbs       = crumbs;
    return s;
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
    QVector<Crumb> crumbs() const { return m_editor->addressBar()->state().crumbs; }
    QStringList segments() const { return m_editor->addressBar()->segments(); }
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
        // show() + qWait, never qWaitForWindowExposed: an unrendered desktop
        // never composites, so an exposure wait fails on the hidden desktop.
        m_splitter->show();
        QTest::qWait(30);
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
        const QStringList seg = segments();
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
        const QStringList seg = segments();
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
        // Fold-collapse vptr behind the trail's back; refresh reconciles.
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
        const QStringList seg = segments();
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

    // ── rebaseTo announces itself: documentChanged once, refresh not thrice ──
    // Upstream's full-file rewrite in 60f80cd dropped these two; carried
    // forward (see ticket 20 resolution) because they guard the single
    // rebase path ticket 18 established — the bookmarks dock, doc-tab source
    // icon and MCP bridge listen to documentChanged, and nothing else in the
    // suite pins its emission count.

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
        // The bar shows exactly that address on the crumb's tooltip.
        QApplication::processEvents();
        AddressBar* bar = m_editor->addressBar();
        hoverAt(*bar, bar->itemRect(QStringLiteral("crumb:1")).center());
        QVERIFY2(bar->toolTip().contains(QStringLiteral("@ 0x20")), qPrintable(bar->toolTip()));
        hoverAt(*bar, bar->itemRect(QStringLiteral("crumb:2")).center());
        QVERIFY2(bar->toolTip().contains(QStringLiteral("@ 0x30")), qPrintable(bar->toolTip()));

        // Null / unreadable pointers: the frame is unknown, not "0x0 + offset".
        m_doc->provider = std::make_unique<BufferProvider>(QByteArray(64, '\0'));
        m_ctrl->refresh();
        c = crumbs();
        QCOMPARE(c.size(), 3);                         // the trail itself survives
        QCOMPARE(c[0].address, 0x00ULL);
        QCOMPARE(c[1].address, 0x00ULL);
        QCOMPARE(c[2].address, 0x00ULL);
        QVERIFY2(bar->toolTip().contains(QStringLiteral("(unreadable)")), qPrintable(bar->toolTip()));
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

    void testCrumbAddressOfRefIdEmbedIsItsRow() {
        // `Stats stats` embedded BY REFERENCE: a Struct field carrying a refId
        // and no children of its own (the type chooser's embed-class shape;
        // compose renders the referenced class's fields at this row). It
        // drills through refId like a pointer but dereferences nothing, so
        // its frame is its own row — not the enclosing frame's base, which
        // the pointer rule handed back (ptrBase never moves for it).
        Node cls; cls.kind = NodeKind::Struct; cls.structTypeName = "Stats";
        cls.parentId = 0; cls.collapsed = false;
        const uint64_t statsCls = m_doc->tree.nodes[m_doc->tree.addNode(cls)].id;
        Node hp; hp.kind = NodeKind::UInt32; hp.name = "hp"; hp.parentId = statsCls; hp.offset = 0;
        const uint64_t hpId = m_doc->tree.nodes[m_doc->tree.addNode(hp)].id;
        Node st; st.kind = NodeKind::Struct; st.name = "stats"; st.refId = statsCls;
        st.parentId = m_id.editor; st.offset = 24; st.collapsed = false;
        const uint64_t statsId = m_doc->tree.nodes[m_doc->tree.addNode(st)].id;
        m_ctrl->refresh();

        const int ln = lineOf(hpId);
        QVERIFY(ln >= 0);
        m_ctrl->handleNodeClick(m_editor, ln, hpId, Qt::NoModifier);
        QCOMPARE(m_ctrl->focusPath(), (QVector<uint64_t>{ statsId }));
        QVector<Crumb> c = crumbs();
        QCOMPARE(c.size(), 2);
        QCOMPARE(c[0].label, QStringLiteral("RcxEditor.stats"));
        QCOMPARE(c[1].label, QStringLiteral("Stats"));
        QCOMPARE(c[1].classId, statsCls);
        QCOMPARE(c[1].pointerId, statsId);
        QCOMPARE(c[0].address, 0ULL);
        QCOMPARE(c[1].address, 24ULL);                  // base + stats.offset
        QVERIFY(m_ctrl->rebaseTo(QStringLiteral("0x100")));
        c = crumbs();
        QCOMPARE(c[1].address, 0x118ULL);
    }

    void testCrumbAddressOfArrayHopIsTheElement() {
        // An Array of struct with a refId, nested under an EXPANDED pointer
        // (so the enclosing frame has a non-zero ptrBase to leak). The array
        // drills through refId but dereferences nothing: its frame is the
        // element compose rendered — the [0] separator's address — not the
        // pointer's target the old pointer rule returned.
        m_doc->provider = std::make_unique<BufferProvider>(chainBytes());
        Node cls; cls.kind = NodeKind::Struct; cls.structTypeName = "Item";
        cls.parentId = 0; cls.collapsed = false;
        const uint64_t itemCls = m_doc->tree.nodes[m_doc->tree.addNode(cls)].id;
        Node v; v.kind = NodeKind::UInt32; v.name = "v"; v.parentId = itemCls; v.offset = 0;
        const uint64_t vId = m_doc->tree.nodes[m_doc->tree.addNode(v)].id;
        Node arr; arr.kind = NodeKind::Array; arr.name = "items"; arr.refId = itemCls;
        arr.elementKind = NodeKind::Struct; arr.arrayLen = 2;
        arr.parentId = m_id.priv; arr.offset = 8; arr.collapsed = false;
        const uint64_t itemsId = m_doc->tree.nodes[m_doc->tree.addNode(arr)].id;
        expand(m_id.vptr);   // QWidgetPrivate at 0x20 renders inline, items at 0x28

        const int ln = lineOf(vId);
        QVERIFY(ln >= 0);
        m_ctrl->handleNodeClick(m_editor, ln, vId, Qt::NoModifier);
        QCOMPARE(m_ctrl->focusPath(), (QVector<uint64_t>{ m_id.vptr, itemsId }));
        const QVector<Crumb> c = crumbs();
        QCOMPARE(c.size(), 3);
        QCOMPARE(c[1].label, QStringLiteral("QWidgetPrivate.items"));
        QCOMPARE(c[2].label, QStringLiteral("Item"));
        QCOMPARE(c[2].classId, itemCls);
        QCOMPARE(c[1].address, 0x20ULL);                // the pointer's target
        QCOMPARE(c[2].address, 0x28ULL);                // element 0, not 0x20
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
        // The bar echoes exactly the labels — no italic field connector, no
        // separator token (the chevron cells are geometry, not text).
        QApplication::processEvents();
        QCOMPARE(segments(),
                 (QStringList{ QStringLiteral("RcxEditor.vptr"),
                               QStringLiteral("QWidgetPrivate.parent"),
                               QStringLiteral("QWidget") }));
        // Lone root: one undotted crumb naming the view root.
        m_ctrl->clearSelection();
        c = crumbs();
        QCOMPARE(c.size(), 1);
        QVERIFY(!c[0].label.contains(QLatin1Char('.')));
        QCOMPARE(c[0].classId, m_id.editor);
        QCOMPARE(c[0].pointerId, 0ULL);
    }

    // ── The state the controller pushes ──

    void testAddressBarStateCarriesSourceAndBase() {
        QVERIFY(m_ctrl->rebaseTo(QStringLiteral("0x20+0x4")));
        QApplication::processEvents();
        const AddressBarState& s = m_editor->addressBar()->state();
        QCOMPARE(s.baseAddress, 0x24ULL);
        QCOMPARE(s.resolvedBase, 0x24ULL);
        QCOMPARE(s.baseFormula, QStringLiteral("0x20+0x4"));
        // A BufferProvider is never live: None before the first status tick,
        // Static after it (the tick is timer-driven, so either is fine here;
        // P2b wires sourceStatusChanged → setLiveness for the in-between).
        QVERIFY(s.liveness == liveness::None || s.liveness == liveness::Static);
        QVERIFY(!s.canBack && !s.canForward);   // P5 wires history
        QVERIFY(!s.canUp);                      // nothing drilled
        drillTwoLevels();
        QVERIFY(m_editor->addressBar()->state().canUp);
        // A second split pane is seeded with the current state at once — the
        // per-refresh push is change-guarded and would otherwise leave it
        // empty until the trail next moved.
        RcxEditor* pane2 = m_ctrl->addSplitEditor(m_splitter);
        QCOMPARE(pane2->addressBar()->state(), m_editor->addressBar()->state());
        QCOMPARE(pane2->addressBar()->state().crumbs.size(), 3);
    }

    // ── AddressBar widget ──
    // Driven RibbonBar-style: a bare bar, show() + qWait, geometry through
    // itemRect(id), hover through a synthesized MouseMove, pixels via grab().

    void testBarGeometry() {
        AddressBar bar;
        bar.setState(stateWith(twoLevel()));
        showBar(bar, 800);
        QCOMPARE(bar.height(), 26);
        QCOMPARE(bar.height(), PanelSearchField::kFieldHeight);
        // kGutter spent once, on the Back glyph's INK (pinned by
        // testFirstInkLandsOnTheGutter): the cell starts earlier by the
        // glyph's centring pad and the SVG's inset, like a doc tab's rect.
        QVERIFY(bar.itemRect(QStringLiteral("back")).left() <= kGutter);
        QVERIFY(bar.itemRect(QStringLiteral("back")).left() >= 0);
        // Every cell of the anatomy is laid out, left → right, without
        // overlaps, on one 22-px row.
        const QStringList order = {
            QStringLiteral("back"), QStringLiteral("fwd"), QStringLiteral("hist"), QStringLiteral("up"),
            QStringLiteral("src"), QStringLiteral("src.chev"), QStringLiteral("root.chev"),
            QStringLiteral("base"), QStringLiteral("crumb:0"), QStringLiteral("chev:0"),
            QStringLiteral("crumb:1"), QStringLiteral("chev:1"), QStringLiteral("space"),
            QStringLiteral("recent") };
        int lastRight = -1;
        for (const QString& id : order) {
            const QRect r = bar.itemRect(id);
            QVERIFY2(!r.isNull(), qPrintable(id + QStringLiteral(" not laid out")));
            QVERIFY2(r.left() > lastRight, qPrintable(id + QStringLiteral(" overlaps its left neighbour")));
            QCOMPARE(r.top(), AddressBar::kCellTop);
            QCOMPARE(r.height(), AddressBar::kCellH);
            lastRight = r.right();
        }
        QVERIFY(bar.itemRect(QStringLiteral("overflow")).isNull());   // nothing folded
        // The field divider sits between `up` and `src`; `recent` hugs the
        // right edge.
        QVERIFY(bar.itemRect(QStringLiteral("src")).left() - bar.itemRect(QStringLiteral("up")).right()
                >= 2 * AddressBar::kDividerPad);
        QCOMPARE(bar.itemRect(QStringLiteral("recent")).right() + 1 + AddressBar::kRightMargin, bar.width());
        // Hit-testing round-trips; the gutter belongs to nobody.
        QCOMPARE(bar.itemIdAt(bar.itemRect(QStringLiteral("crumb:1")).center()), QStringLiteral("crumb:1"));
        QCOMPARE(bar.itemIdAt(bar.itemRect(QStringLiteral("base")).center()), QStringLiteral("base"));
        QVERIFY(bar.itemIdAt(QPoint(0, 13)).isEmpty());
        QCOMPARE(bar.focusPolicy(), Qt::NoFocus);
        QVERIFY(!bar.isEditing());
    }

    void testFirstInkLandsOnTheGutter() {
        // The bar's leftmost ink — the Back arrow, enabled so it paints in
        // full — sits on the device column kGutter maps to, the column the
        // doc-tab title's first ink sits on. The cell pad and the SVG's own
        // inset used to push it ~5.6 px right of every other strip.
        AddressBar bar;
        bar.applyTheme(ThemeManager::instance().current());
        AddressBarState s = stateWith(twoLevel());
        s.canBack = true;
        bar.setState(s);
        showBar(bar, 800);
        const QImage img = grabOf(bar);
        const int ink = firstInkColumn(img, editorPaperColor(bar.theme()));
        const qreal dpr = img.devicePixelRatio() > 0 ? img.devicePixelRatio() : 1.0;
        QVERIFY2(qAbs(ink - qRound(kGutter * dpr)) <= 1,
                 qPrintable(QStringLiteral("first ink at device column %1, gutter is %2 (dpr %3)")
                                .arg(ink).arg(qRound(kGutter * dpr)).arg(dpr)));
        // Still no ink in the gutter itself: nothing hangs off the left edge.
        QVERIFY(ink >= 1);
    }

    void testCrumbClickFiresIndexNotDeepest() {
        drillTwoLevels();   // RcxEditor.vptr › QWidgetPrivate.parent › QWidget
        QApplication::processEvents();
        AddressBar* bar = m_editor->addressBar();
        const Theme& t = bar->theme();
        QSignalSpy spy(m_editor, &RcxEditor::crumbClicked);
        // The deepest crumb is "you are here": its click fires the index
        // past the focus path, which collapseToFocus turns into a scroll
        // to that class's header and nothing else — no collapse, no undo
        // entry, the trail as it was. Visually it stays inert: no hover
        // fill, arrow cursor.
        const QRect deep = bar->itemRect(QStringLiteral("crumb:2"));
        QVERIFY(!deep.isNull());
        const int undoBefore = m_doc->undoStack.count();
        const bool vptrWas = collapsed(m_id.vptr), parentWas = collapsed(m_id.parent);
        hoverAt(*bar, deep.center());
        QImage img = grabOf(*bar);
        QVERIFY2(countColour(img, devRect(img, deep), t.hover) < 16, "deepest crumb took a hover fill");
        QVERIFY(bar->cursor().shape() == Qt::ArrowCursor);
        QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier, deep.center());
        QApplication::processEvents();
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toInt(), 2);                   // n - 1
        QCOMPARE(m_ctrl->focusPath(), (QVector<uint64_t>{ m_id.vptr, m_id.parent }));
        QCOMPARE(m_doc->undoStack.count(), undoBefore);
        QCOMPARE(collapsed(m_id.vptr), vptrWas);
        QCOMPARE(collapsed(m_id.parent), parentWas);
        QCOMPARE(segments().size(), 3);
        spy.clear();
        // Press inside, release outside → no click.
        const QRect r1 = bar->itemRect(QStringLiteral("crumb:1"));
        QVERIFY(!r1.isNull());
        QTest::mousePress(bar, Qt::LeftButton, Qt::NoModifier, r1.center());
        QTest::mouseRelease(bar, Qt::LeftButton, Qt::NoModifier, QPoint(r1.center().x(), r1.bottom() + 40));
        QApplication::processEvents();
        QCOMPARE(spy.count(), 0);
        // An ancestor click → crumbClicked(index) → collapseToFocus(index).
        QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier, r1.center());
        QApplication::processEvents();
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toInt(), 1);
        QCOMPARE(m_ctrl->focusPath(), (QVector<uint64_t>{ m_id.vptr }));
        QVERIFY(collapsed(m_id.parent));
        QCOMPARE(segments(), (QStringList{ QStringLiteral("RcxEditor.vptr"),
                                           QStringLiteral("QWidgetPrivate") }));
    }

    void testBarNeverTakesFocus() {
        // NoFocus contract: a click on the bar must not steal focus from the
        // document (here: a sibling QLineEdit standing in for Scintilla).
        QWidget win;
        auto* lay = new QVBoxLayout(&win);
        lay->setContentsMargins(0, 0, 0, 0);
        auto* bar = new AddressBar(&win);
        int clicked = -1;
        AddressBar::Callbacks cb;
        cb.onCrumb = [&](int i) { clicked = i; };
        bar->setCallbacks(std::move(cb));
        bar->setState(stateWith(twoLevel()));
        auto* edit = new QLineEdit(&win);
        lay->addWidget(bar);
        lay->addWidget(edit);
        win.resize(800, 80);
        win.show();
        edit->setFocus();
        QTest::qWait(50);
        QApplication::processEvents();
        QCOMPARE(win.focusWidget(), edit);
        QCOMPARE(bar->focusPolicy(), Qt::NoFocus);

        const QRect r = bar->itemRect(QStringLiteral("crumb:0"));
        QVERIFY(!r.isNull());
        QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier, r.center());
        QApplication::processEvents();
        QCOMPARE(clicked, 0);
        QCOMPARE(win.focusWidget(), edit);
        QVERIFY(!bar->hasFocus());
    }

    // Pixel contract: nothing filled at rest, hover is the `hover` fill on
    // ancestors only, the tone ladder is textDim (ancestor) / text (deepest)
    // / textDim (lone), the band is paper, the seam is one device row of
    // containerBorderColor, and no accent pixel exists anywhere on the bar.
    void testPixelsRestHoverAndTones() {
        AddressBar bar;
        bar.applyTheme(ThemeManager::instance().current());
        const Theme& t = bar.theme();
        bar.setState(stateWith(twoLevel()));
        showBar(bar, 800);
        QImage img = grabOf(bar);
        const QRect all(0, 0, img.width(), img.height());
        // Text anti-aliasing can hit a fill colour by coincidence, so a
        // handful of stray pixels is tolerated — a fill would be hundreds.
        QVERIFY2(countColour(img, all, t.hover) < 16, "hover fill at rest");
        QVERIFY2(countColour(img, all, pressedFill(t)) < 16, "pressed fill at rest");
        QCOMPARE(countColour(img, all, t.indHoverSpan), 0);
        // The band is paper — sample the stretch.
        const QRect space = devRect(img, bar.itemRect(QStringLiteral("space")));
        QVERIFY(space.width() > 4);
        QVERIFY(sameColour(img.pixel(space.center()), editorPaperColor(t)));
        // The seam: the bottom device row is containerBorderColor end to end.
        const QRect bottom(0, img.height() - 1, img.width(), 1);
        QVERIFY(countColour(img, bottom, containerBorderColor(t)) > img.width() * 9 / 10);
        QVERIFY2(countColour(img, QRect(0, img.height() - 2, img.width(), 1), containerBorderColor(t))
                     < img.width() / 10, "seam is more than one device row");
        // Tones: the ancestor is textDim with no `text` ink; the deepest
        // carries `text` (DemiBold).
        const QRect c0 = devRect(img, bar.itemRect(QStringLiteral("crumb:0")));
        const QRect c1 = devRect(img, bar.itemRect(QStringLiteral("crumb:1")));
        QVERIFY(countColour(img, c0, t.textDim) > 0);
        QCOMPARE(countColour(img, c0, t.text), 0);
        QVERIFY(countColour(img, c1, t.text) > 0);

        // Hover on an ancestor: the fill, and the tone steps up to text.
        hoverAt(bar, bar.itemRect(QStringLiteral("crumb:0")).center());
        img = grabOf(bar);
        QVERIFY(countColour(img, c0, t.hover) > 0);
        QVERIFY(countColour(img, c0, t.text) > 0);
        QCOMPARE(countColour(img, all, t.indHoverSpan), 0);
        // Hover on the deepest: inert, no fill.
        hoverAt(bar, bar.itemRect(QStringLiteral("crumb:1")).center());
        img = grabOf(bar);
        QVERIFY2(countColour(img, c1, t.hover) < 16, "deepest crumb took a hover fill");
        // Hover on the base: a text field, no fill either.
        const QRect base = devRect(img, bar.itemRect(QStringLiteral("base")));
        hoverAt(bar, bar.itemRect(QStringLiteral("base")).center());
        img = grabOf(bar);
        QVERIFY2(countColour(img, base, t.hover) < 16, "base took a hover fill");
        // Leaving clears the fill.
        QEvent leave(QEvent::Leave);
        QApplication::sendEvent(&bar, &leave);
        img = grabOf(bar);
        QVERIFY2(countColour(img, all, t.hover) < 16, "hover fill survived leave");

        // A lone crumb only repeats what the doc tab already names: textDim,
        // regular — no `text` ink.
        bar.setState(stateWith({ crumb(QStringLiteral("RcxEditor"), 0) }));
        img = grabOf(bar);
        const QRect lone = devRect(img, bar.itemRect(QStringLiteral("crumb:0")));
        QVERIFY(countColour(img, lone, t.textDim) > 0);
        QCOMPARE(countColour(img, lone, t.text), 0);
    }

    void testOverflowFoldsFromTheRootNeverTheDeepest() {
        AddressBar bar;
        QVector<Crumb> trail;
        QStringList labels;
        for (int i = 0; i < 6; ++i) {
            const QString l = i < 5 ? QStringLiteral("C%1.f%1").arg(i) : QStringLiteral("C5");
            trail.push_back(crumb(l, uint64_t(i), uint64_t(i + 1)));
            labels << l;
        }
        bar.setState(stateWith(trail));
        showBar(bar, 300);
        QVERIFY(!bar.itemRect(QStringLiteral("overflow")).isNull());
        QVERIFY(bar.itemRect(QStringLiteral("crumb:0")).isNull());
        QVERIFY(!bar.itemRect(QStringLiteral("crumb:5")).isNull());
        QVERIFY(bar.itemRect(QStringLiteral("crumb:5")).left() > bar.itemRect(QStringLiteral("overflow")).right());
        const QStringList hidden = bar.overflowMenuLabels();
        QVERIFY(hidden.size() >= 1 && hidden.size() <= 5);
        QCOMPARE(hidden.first(), QStringLiteral("C0.f0"));     // folded from the root
        QVERIFY(!hidden.contains(QStringLiteral("C5")));       // never the deepest
        QCOMPARE(bar.segments().first(), QStringLiteral("«"));
        // Hidden and shown partition the trail, in order.
        QCOMPARE(hidden + bar.segments().mid(1), labels);

        // Wide: nothing folds, everything fits inside the strip.
        bar.resize(1200, AddressBar::kAddressBarHeight);
        QApplication::processEvents();
        QVERIFY(bar.itemRect(QStringLiteral("overflow")).isNull());
        for (int i = 0; i < 6; ++i)
            QVERIFY2(!bar.itemRect(QStringLiteral("crumb:%1").arg(i)).isNull(), "crumb dropped at 1200 px");
        QVERIFY(bar.overflowMenuLabels().isEmpty());
        QCOMPARE(bar.segments(), labels);
        QVERIFY(bar.itemRect(QStringLiteral("chev:5")).right() < bar.itemRect(QStringLiteral("recent")).left());
        QCOMPARE(bar.itemRect(QStringLiteral("recent")).right() + 1 + AddressBar::kRightMargin, 1200);
        // Narrow again: folds again (the layout follows the width).
        bar.resize(300, AddressBar::kAddressBarHeight);
        QApplication::processEvents();
        QVERIFY(!bar.itemRect(QStringLiteral("overflow")).isNull());
    }

    void testEqualStatePushAppliesOnce() {
        // The controller pushes every refresh tick; an equal state is free.
        AddressBar bar;
        const AddressBarState s = stateWith(twoLevel());
        bar.setState(s);
        QCOMPARE(bar.stateApplyCount(), 1);
        bar.setState(s);
        QCOMPARE(bar.stateApplyCount(), 1);
        AddressBarState s2 = s;
        s2.liveness = liveness::Stale;
        bar.setState(s2);
        QCOMPARE(bar.stateApplyCount(), 2);
        // Through the editor too: the controller's push of what the bar
        // already shows must not count. Settle with one refresh first — the
        // status tick can move m_lastStatus between two pushes.
        m_ctrl->refresh();
        const int n = m_editor->addressBar()->stateApplyCount();
        m_editor->setAddressBarState(m_editor->addressBar()->state());
        m_ctrl->refresh();
        QCOMPARE(m_editor->addressBar()->stateApplyCount(), n);
    }

    void testTooltipsFollowHover() {
        // The widget toolTip mirrors the hovered cell (the bridge shows it);
        // QEvent::ToolTip is never handled here.
        AddressBar bar;
        QVector<Crumb> trail = twoLevel();
        trail[0].address = 0x1000;
        trail[1].address = 0x7FF6DEAD0030ULL;
        bar.setState(stateWith(trail));
        showBar(bar, 800);
        QVERIFY(bar.toolTip().isEmpty());
        hoverAt(bar, bar.itemRect(QStringLiteral("base")).center());
        QVERIFY2(bar.toolTip().contains(QStringLiteral("Base address")), qPrintable(bar.toolTip()));
        QVERIFY(bar.toolTip().contains(QStringLiteral("0x7FF600000000")));
        hoverAt(bar, bar.itemRect(QStringLiteral("crumb:1")).center());
        QVERIFY2(bar.toolTip().contains(QStringLiteral("@ 0x7FF6DEAD0030")), qPrintable(bar.toolTip()));
        QVERIFY(bar.toolTip().startsWith(QStringLiteral("QWidgetPrivate")));
        // An unreadable frame says so; the text is republished in place on
        // the state push, without a dismiss.
        trail[1].address = 0;
        bar.setState(stateWith(trail));
        QVERIFY2(bar.toolTip().contains(QStringLiteral("(unreadable)")), qPrintable(bar.toolTip()));
        hoverAt(bar, bar.itemRect(QStringLiteral("back")).center());
        QCOMPARE(bar.toolTip(), QStringLiteral("Back  Alt+Left"));
        hoverAt(bar, bar.itemRect(QStringLiteral("src")).center());
        QVERIFY2(bar.toolTip().contains(QStringLiteral("Process REECLASS.exe")), qPrintable(bar.toolTip()));
        QVERIFY(bar.toolTip().contains(QStringLiteral("Live")));
        hoverAt(bar, bar.itemRect(QStringLiteral("chev:0")).center());
        QCOMPARE(bar.toolTip(), QStringLiteral("Fields of RcxEditor"));
        // Leaving clears it.
        QEvent leave(QEvent::Leave);
        QApplication::sendEvent(&bar, &leave);
        QVERIFY(bar.toolTip().isEmpty());
    }

    void testBackCellDisabledWhenNoHistory() {
        // No history yet (P5): the Back cell is painted at 40 % and ignores
        // clicks; with history it paints in full and fires.
        AddressBar bar;
        bar.applyTheme(ThemeManager::instance().current());
        const Theme& t = bar.theme();
        int backs = 0;
        AddressBar::Callbacks cb;
        cb.onBack = [&] { ++backs; };
        bar.setCallbacks(std::move(cb));
        AddressBarState s = stateWith(twoLevel());   // canBack = false
        bar.setState(s);
        showBar(bar, 800);
        const QRect back = bar.itemRect(QStringLiteral("back"));
        QVERIFY(!back.isNull());
        const double inkOff = inkAmount(grabOf(bar), back, editorPaperColor(t));
        QTest::mouseClick(&bar, Qt::LeftButton, Qt::NoModifier, back.center());
        QApplication::processEvents();
        QCOMPARE(backs, 0);

        s.canBack = true;
        bar.setState(s);
        const double inkOn = inkAmount(grabOf(bar), back, editorPaperColor(t));
        QVERIFY2(inkOn > 3.0, qPrintable(QStringLiteral("enabled Back paints no ink (%1)").arg(inkOn)));
        QVERIFY2(inkOff < inkOn * 0.6,
                 qPrintable(QStringLiteral("disabled Back didn't dim: on=%1 off=%2").arg(inkOn).arg(inkOff)));
        QTest::mouseClick(&bar, Qt::LeftButton, Qt::NoModifier, back.center());
        QApplication::processEvents();
        QCOMPARE(backs, 1);
    }

    // ── Source chip + base segment ──

    void testSourceChipTooltipNamesKindAndStatus() {
        AddressBar bar;
        bar.setState(stateWith(twoLevel()));   // Process REECLASS.exe, Live
        showBar(bar, 800);
        hoverAt(bar, bar.itemRect(QStringLiteral("src")).center());
        const QString tip = bar.toolTip();
        QVERIFY2(tip.contains(kindLabelFor(QStringLiteral("processmemory"))), qPrintable(tip));
        QVERIFY(tip.contains(QStringLiteral("REECLASS.exe")));
        QVERIFY(tip.contains(QStringLiteral("Live — reading")));   // the status chip's words
        QVERIFY(tip.contains(QStringLiteral("click to change source")));
        QCOMPARE(bar.sourceDisplayText(), QStringLiteral("REECLASS.exe"));
        // The chevron is part of the chip: same tip, same hover group.
        hoverAt(bar, bar.itemRect(QStringLiteral("src.chev")).center());
        QCOMPARE(bar.toolTip(), tip);
        // Status words follow the liveness, republished in place.
        AddressBarState s = stateWith(twoLevel());
        s.liveness = liveness::Disconnected;
        bar.setState(s);
        QVERIFY2(bar.toolTip().contains(QStringLiteral("Disconnected")), qPrintable(bar.toolTip()));
        s.liveness = liveness::Static;
        s.sourceKindId = QStringLiteral("File");
        bar.setState(s);
        QVERIFY(bar.toolTip().startsWith(kindLabelFor(QStringLiteral("File"))));
        QVERIFY(bar.toolTip().contains(QStringLiteral("Static file")));
        // No provider: the chip invites one, in the cell and in the tip.
        s.sourceName.clear(); s.sourceKindId.clear(); s.liveness = liveness::None;
        bar.setState(s);
        QCOMPARE(bar.sourceDisplayText(), QStringLiteral("Select source"));
        QVERIFY2(bar.toolTip().contains(QStringLiteral("Select source")), qPrintable(bar.toolTip()));
        QVERIFY(bar.livenessDotRect().isNull());
    }

    void testSourceChipLivenessDot() {
        // The status chip's mapping on the bar: green reading, focusGlow
        // stale, markerError disconnected (with the icon at 40 %), nothing
        // for a static file. The dot rides the icon's bottom-right corner.
        AddressBar bar;
        bar.applyTheme(ThemeManager::instance().current());
        const Theme& t = bar.theme();
        const QColor paper = editorPaperColor(t);
        AddressBarState s = stateWith(twoLevel());   // Live
        bar.setState(s);
        showBar(bar, 800);
        const QRect icon = bar.sourceIconRect();
        const QRect dot  = bar.livenessDotRect();
        QVERIFY(!icon.isNull());
        QVERIFY(!dot.isNull());
        QCOMPARE(dot.size(), QSize(AddressBar::kDotPx, AddressBar::kDotPx));
        QVERIFY(icon.contains(dot.topLeft()));            // on the icon...
        QVERIFY(dot.right() > icon.right() && dot.bottom() > icon.bottom());   // ...over its corner
        QVERIFY(dot.bottom() < bar.itemRect(QStringLiteral("src")).bottom()); // inside the cell
        // The ring around the dot is excluded from the icon-ink probe.
        const QRect ring = dot.adjusted(-AddressBar::kDotRing, -AddressBar::kDotRing,
                                        AddressBar::kDotRing, AddressBar::kDotRing);
        QImage img = grabOf(bar);
        QRect dd = devRect(img, dot);
        QVERIFY(countColour(img, dd, t.indHintGreen) > 0);
        const double inkLive = inkAmount(img, icon, paper, ring);
        QVERIFY2(inkLive > 3.0, qPrintable(QStringLiteral("live icon paints no ink (%1)").arg(inkLive)));

        s.liveness = liveness::Stale;
        bar.setState(s);
        img = grabOf(bar);
        QVERIFY(countColour(img, dd, t.focusGlow) > 0);
        QCOMPARE(countColour(img, dd, t.indHintGreen), 0);

        s.liveness = liveness::Disconnected;
        bar.setState(s);
        img = grabOf(bar);
        QVERIFY(countColour(img, dd, t.markerError) > 0);
        const double inkGone = inkAmount(img, icon, paper, ring);
        QVERIFY2(inkGone < inkLive * 0.6,
                 qPrintable(QStringLiteral("disconnected icon didn't dim: live=%1 gone=%2").arg(inkLive).arg(inkGone)));

        // A static file has nothing to be alive: no dot at all, icon at full ink.
        s.liveness = liveness::Static;
        bar.setState(s);
        QVERIFY(bar.livenessDotRect().isNull());
        img = grabOf(bar);
        QCOMPARE(countColour(img, dd, t.indHintGreen), 0);
        QCOMPARE(countColour(img, dd, t.focusGlow), 0);
        QCOMPARE(countColour(img, dd, t.markerError), 0);
        QVERIFY(inkAmount(img, icon, paper, ring) > inkGone);

        // setLiveness alone — the sourceStatusChanged path — moves the dot
        // without counting as a state push.
        const int n = bar.stateApplyCount();
        bar.setLiveness(liveness::Live);
        QCOMPARE(bar.stateApplyCount(), n);
        img = grabOf(bar);
        QVERIFY(countColour(img, dd, t.indHintGreen) > 0);
        QCOMPARE(countColour(img, QRect(0, 0, img.width(), img.height()), t.indHoverSpan), 0);
    }

    void testBaseFormulaElidedForDisplayOnly() {
        // The regression this pins: the command row elided a long formula
        // for display and then edited the elided text, feeding the ellipsis
        // to the parser. Here the display is elided, the state keeps every
        // character, and the resolved address follows in textMuted.
        AddressBar bar;
        bar.applyTheme(ThemeManager::instance().current());
        const Theme& t = bar.theme();
        AddressBarState s = stateWith(twoLevel());
        bar.setState(s);
        showBar(bar, 1200);
        // A literal: shown as-is, no suffix (it would repeat the number).
        QCOMPARE(bar.baseDisplayText(), QStringLiteral("0x7FF600000000"));
        QImage img = grabOf(bar);
        QVERIFY2(countColour(img, devRect(img, bar.itemRect(QStringLiteral("base"))), t.textMuted) < 16,
                 "resolved suffix drawn beside a literal");

        const QString formula = QStringLiteral("<REECLASS.exe>+0x1234+[0x10]*2");
        QCOMPARE(formula.size(), 30);
        s.baseFormula  = formula;
        s.resolvedBase = 0x7FF6DEAD1234ULL;
        bar.setState(s);
        QCOMPARE(bar.state().baseFormula, formula);            // the state: every character
        const QString shown = bar.baseDisplayText();           // the display: elided
        QVERIFY2(shown.size() < formula.size(), qPrintable(shown));
        QVERIFY(shown.contains(QChar(0x2026)));
        QVERIFY2(shown.startsWith(QStringLiteral("<REE")) && shown.endsWith(QStringLiteral("*2")),
                 qPrintable(shown));                            // middle elision keeps both ends
        QVERIFY(QFontMetrics(bar.font()).horizontalAdvance(shown) <= AddressBar::kBaseMaxW);
        img = grabOf(bar);
        const QRect base = devRect(img, bar.itemRect(QStringLiteral("base")));
        QVERIFY(countColour(img, base, t.textMuted) > 8);      // the "→ 0x…" suffix
        QCOMPARE(countColour(img, base, t.indHoverSpan), 0);
        // The tooltip carries what the segment could not.
        hoverAt(bar, bar.itemRect(QStringLiteral("base")).center());
        QVERIFY2(bar.toolTip().contains(formula), qPrintable(bar.toolTip()));
        QVERIFY(bar.toolTip().contains(QStringLiteral("0x7FF6DEAD1234")));
        QVERIFY(bar.toolTip().startsWith(QStringLiteral("Base address")));

        // 30 px short of the natural width: overflow step 1 takes only what
        // is over — the formula loses a few characters and keeps its suffix.
        // (`recent` hugs the right edge, so the natural width is the trail's
        // right end plus the recent cell.)
        const int naturalW = bar.itemRect(QStringLiteral("chev:1")).right() + 1
                           + AddressBar::kRecentW + AddressBar::kRightMargin;
        QVERIFY(naturalW < 1200);
        bar.resize(naturalW - 30, AddressBar::kAddressBarHeight);
        QApplication::processEvents();
        QVERIFY(bar.itemRect(QStringLiteral("overflow")).isNull());
        const QString slightly = bar.baseDisplayText();
        QVERIFY2(slightly.size() < formula.size() && slightly.contains(QChar(0x2026)), qPrintable(slightly));
        QVERIFY2(QFontMetrics(bar.font()).horizontalAdvance(slightly) > AddressBar::kBaseMinW,
                 "a 30-px squeeze fell straight to the 90-px floor");
        img = grabOf(bar);
        QVERIFY2(countColour(img, devRect(img, bar.itemRect(QStringLiteral("base"))), t.textMuted) > 8,
                 "resolved suffix dropped for a 30-px squeeze");
        // A real squeeze: the base at its 90-px floor, suffix gone, the
        // state untouched.
        bar.resize(300, AddressBar::kAddressBarHeight);
        QApplication::processEvents();
        QVERIFY(QFontMetrics(bar.font()).horizontalAdvance(bar.baseDisplayText()) <= AddressBar::kBaseMinW);
        img = grabOf(bar);
        QVERIFY2(countColour(img, devRect(img, bar.itemRect(QStringLiteral("base"))), t.textMuted) < 16,
                 "resolved suffix survived the narrow fold");
        QCOMPARE(bar.state().baseFormula, formula);
    }

    void testSourceChipClickOpensPopupUnderTheBar() {
        // Click the chip → sourcePopupRequested with the anchor on the bar's
        // bottom edge → the controller's SourceChooserPopup opens there and
        // the chip reads pressed until the popup hides, by any route.
        //
        // A Qt::Popup cannot keep OS focus on the hidden test desktop: the
        // first event pump after show() takes it away again and the platform
        // closes the popup (the artifact behind test_source_chooser's
        // exposure failures). So the pressed probe grabs BEFORE the pump and
        // the un-pressed probe after it — the platform's close and an
        // explicit hide() both leave through hideEvent → dismissed().
        QApplication::processEvents();
        AddressBar* bar = m_editor->addressBar();
        const Theme& t = bar->theme();
        QSignalSpy spy(m_editor, &RcxEditor::sourcePopupRequested);
        const QRect src = bar->itemRect(QStringLiteral("src"));
        QVERIFY(!src.isNull());
        QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier, src.center());
        QCOMPARE(spy.count(), 1);
        const QPoint anchor = spy.at(0).at(0).toPoint();
        QCOMPARE(anchor.y(), bar->mapToGlobal(QPoint(0, bar->height())).y());
        QCOMPARE(anchor.x(), bar->mapToGlobal(QPoint(src.left(), 0)).x());
        auto* popup = m_editor->findChild<SourceChooserPopup*>();
        QVERIFY(popup);
        QVERIFY(popup->isVisible());
        QImage img = bar->grab().toImage().convertToFormat(QImage::Format_ARGB32);
        QVERIFY2(countColour(img, devRect(img, src), pressedFill(t)) > 0, "chip not pressed while the popup is up");
        QCOMPARE(countColour(img, QRect(0, 0, img.width(), img.height()), t.indHoverSpan), 0);
        QApplication::processEvents();
        popup->hide();
        QApplication::processEvents();
        QVERIFY(!popup->isVisible());
        img = grabOf(*bar);
        QVERIFY2(countColour(img, devRect(img, src), pressedFill(t)) < 16, "chip stayed pressed after the popup hid");
    }

    void testSourceStatusSignalMovesTheDot() {
        // The controller's status flip reaches the chip without a refresh
        // push: emit the signal and read the bar's liveness back.
        AddressBar* bar = m_editor->addressBar();
        emit m_ctrl->sourceStatusChanged(RcxController::SourceStatus::Stale);
        QCOMPARE(bar->state().liveness, liveness::Stale);
        emit m_ctrl->sourceStatusChanged(RcxController::SourceStatus::Disconnected);
        QCOMPARE(bar->state().liveness, liveness::Disconnected);
        // A second pane gets its own connection.
        RcxEditor* pane2 = m_ctrl->addSplitEditor(m_splitter);
        emit m_ctrl->sourceStatusChanged(RcxController::SourceStatus::Live);
        QCOMPARE(pane2->addressBar()->state().liveness, liveness::Live);
        QCOMPARE(bar->state().liveness, liveness::Live);
    }

    void testCrumbPathTextMatchesTheModel() {
        // "Copy path" rebuilds the dotted path from the labels alone and must
        // agree with trailPathText() over the tree for the same trail.
        AddressBar bar;
        bar.setState(stateWith({ crumb(QStringLiteral("RcxEditor.vptr"), 0),
                                 crumb(QStringLiteral("QWidgetPrivate.parent"), 1),
                                 crumb(QStringLiteral("QWidget"), 2) }));
        QCOMPARE(bar.crumbPathText(0), QStringLiteral("RcxEditor"));
        QCOMPARE(bar.crumbPathText(1), QStringLiteral("RcxEditor.vptr"));
        QCOMPARE(bar.crumbPathText(2), QStringLiteral("RcxEditor.vptr.parent"));
        drillTwoLevels();
        QApplication::processEvents();
        QCOMPARE(m_editor->addressBar()->crumbPathText(2),
                 trailPathText(m_doc->tree, m_ctrl->viewRootId(), m_ctrl->focusPath()));
    }

    // ── P3: the base edit ──

    void testBaseClickOpensOverlayOnFullFormula() {
        // The regression the overlay exists for: the display elides a long
        // formula, the EDIT opens on every character of it — never on the
        // ellipsis the command row used to hand the parser.
        AddressBar bar;
        AddressBarState s = stateWith(twoLevel());
        const QString formula = QStringLiteral("<REECLASS.exe>+0x1234+[0x10]*2");
        QCOMPARE(formula.size(), 30);
        s.baseFormula  = formula;
        s.resolvedBase = 0x7FF6DEAD1234ULL;
        bar.setState(s);
        showBar(bar, 800);
        const QString shown = bar.baseDisplayText();
        QVERIFY2(shown.size() < formula.size() && shown.contains(QChar(0x2026)), qPrintable(shown));
        QVERIFY(!bar.isEditing());
        const QRect base = bar.itemRect(QStringLiteral("base"));
        QTest::mouseClick(&bar, Qt::LeftButton, Qt::NoModifier, base.center());
        QApplication::processEvents();
        QVERIFY(bar.isEditing());
        QCOMPARE(bar.editText(), formula);
        QCOMPARE(bar.editWidget()->selectedText(), formula);      // selectAll: typing replaces
        QVERIFY(bar.editWidget()->isVisible());
        QCOMPARE(bar.editWidget()->font(), bar.font());           // the chrome face
        // Geometry: starts at the base cell, at least kEditMinW wide, inside
        // the strip, on the cell row.
        const QRect r = bar.editRect();
        QCOMPARE(r, bar.editWidget()->geometry());
        QCOMPARE(r.left(), base.left());
        QVERIFY(r.width() >= AddressBar::kEditMinW);
        QVERIFY(bar.rect().contains(r));
        QCOMPARE(r.top(), AddressBar::kCellTop);
        QCOMPARE(r.height(), AddressBar::kCellH);
        // The bar is focusable only while the overlay is up.
        QCOMPARE(bar.focusPolicy(), Qt::StrongFocus);
        QCOMPARE(bar.window()->focusWidget(), bar.editWidget());
        // A second click while open just re-selects; still one overlay.
        QTest::mouseClick(&bar, Qt::LeftButton, Qt::NoModifier, base.center());
        QVERIFY(bar.isEditing());
        QCOMPARE(bar.findChildren<QLineEdit*>().size(), 1);
        // The interior is PanelSearchField's rule set, verbatim.
        QCOMPARE(bar.editWidget()->styleSheet(), panelFieldInteriorQss(bar.theme()));
        QVERIFY(bar.editWidget()->styleSheet().contains(QStringLiteral("border-radius: 0px")));
        QVERIFY(bar.editWidget()->styleSheet().contains(QStringLiteral("border: none")));
    }

    void testBaseCommitRebasesUndoably() {
        QApplication::processEvents();
        AddressBar* bar = m_editor->addressBar();
        QSignalSpy commit(m_editor, &RcxEditor::baseCommitRequested);
        QCOMPARE(m_doc->tree.baseAddress, 0ULL);
        const int undoBefore = m_doc->undoStack.count();
        QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier, bar->itemRect(QStringLiteral("base")).center());
        QVERIFY(bar->isEditing());
        QCOMPARE(bar->editText(), QStringLiteral("0x0"));
        QTest::keyClicks(bar->editWidget(), QStringLiteral("0x1000"));
        QCOMPARE(bar->editText(), QStringLiteral("0x1000"));
        // A literal parses: the live preview is the controller's evaluator.
        QVERIFY(bar->editTextValid());
        QCOMPARE(bar->editPreviewText(), QStringLiteral("\u2192 0x1000"));
        QTest::keyClick(bar->editWidget(), Qt::Key_Return);
        QApplication::processEvents();
        QCOMPARE(commit.count(), 1);
        QCOMPARE(commit.at(0).at(0).toString(), QStringLiteral("0x1000"));
        QCOMPARE(m_doc->undoStack.count(), undoBefore + 1);
        QCOMPARE(m_doc->tree.baseAddress, 0x1000ULL);
        QVERIFY(m_doc->tree.baseAddressFormula.isEmpty());       // bare literal
        QVERIFY(!bar->isEditing());                              // accepted → closed
        QVERIFY(!bar->editWidget()->isVisible());
        QCOMPARE(bar->state().baseAddress, 0x1000ULL);
        QCOMPARE(bar->baseDisplayText(), QStringLiteral("0x1000"));
        QCOMPARE(bar->focusPolicy(), Qt::NoFocus);
        QCOMPARE(m_editor->window()->focusWidget(), m_editor->scintilla());
        // Undo puts the old base back on the segment through the push.
        m_doc->undoStack.undo();
        QApplication::processEvents();
        QCOMPARE(bar->state().baseAddress, 0ULL);
    }

    void testBaseCommitRefusedKeepsOverlay() {
        QApplication::processEvents();
        AddressBar* bar = m_editor->addressBar();
        const Theme& t = bar->theme();
        QSignalSpy hint(m_ctrl, &RcxController::statusHint);
        const int undoBefore = m_doc->undoStack.count();
        QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier, bar->itemRect(QStringLiteral("base")).center());
        QVERIFY(bar->isEditing());
        QTest::keyClicks(bar->editWidget(), QStringLiteral("garbage"));
        QTest::keyClick(bar->editWidget(), Qt::Key_Return);
        QApplication::processEvents();
        QCOMPARE(m_doc->undoStack.count(), undoBefore);          // nothing pushed
        QCOMPARE(m_doc->tree.baseAddress, 0ULL);
        QVERIFY(!hint.isEmpty());
        QVERIFY2(hint.last().at(0).toString().startsWith(QStringLiteral("Base: ")),
                 qPrintable(hint.last().at(0).toString()));
        QVERIFY(bar->isEditing());                               // refused → stays open
        QCOMPARE(bar->editText(), QStringLiteral("garbage"));
        QVERIFY(!bar->editTextValid());
        QVERIFY(!bar->editPreviewText().isEmpty());              // the parser's words
        // The seam under the field is markerError, one device row.
        const QImage img = grabOf(*bar);
        const QRect seam = seamRowUnder(img, bar->editRect());
        QVERIFY(countColour(img, seam, t.markerError) > seam.width() / 2);
        QCOMPARE(countColour(img, seam, t.borderFocused), 0);
        QCOMPARE(countColour(img, QRect(0, 0, img.width(), img.height()), t.indHoverSpan), 0);
        // Typing again answers the refusal: the seam follows the new text.
        QTest::keyClicks(bar->editWidget(), QStringLiteral("\b\b\b\b\b\b\b0x10"));
        QVERIFY(bar->editTextValid());
        QTest::keyClick(bar->editWidget(), Qt::Key_Escape);
        QVERIFY(!bar->isEditing());
    }

    void testEditSeamFollowsValidity() {
        // While typing, the parser's verdict picks the seam colour under
        // the field, and a parsing formula shows what it resolves to.
        AddressBar bar;
        bar.applyTheme(ThemeManager::instance().current());
        const Theme& t = bar.theme();
        AddressBar::Callbacks cb;
        cb.evaluate = [](const QString& s) { return s == QStringLiteral("0x10") ? QStringLiteral("0x10") : QString(); };
        bar.setCallbacks(std::move(cb));
        bar.setState(stateWith(twoLevel()));
        showBar(bar, 800);
        // At rest: the plain seam, no ring.
        QImage img = grabOf(bar);
        QRect seam = seamRowUnder(img, bar.itemRect(QStringLiteral("base")));
        QCOMPARE(countColour(img, seam, t.borderFocused), 0);
        QCOMPARE(countColour(img, seam, t.markerError), 0);
        QTest::mouseClick(&bar, Qt::LeftButton, Qt::NoModifier, bar.itemRect(QStringLiteral("base")).center());
        QVERIFY(bar.isEditing());
        QTest::keyClicks(bar.editWidget(), QStringLiteral("0x10"));
        QVERIFY(bar.editTextValid());
        QCOMPARE(bar.editPreviewText(), QStringLiteral("\u2192 0x10"));
        img = grabOf(bar);
        seam = seamRowUnder(img, bar.editRect());
        QVERIFY(countColour(img, seam, t.borderFocused) > seam.width() / 2);
        QCOMPARE(countColour(img, seam, t.markerError), 0);
        // The preview sits right after the overlay in textMuted.
        const QRect after(bar.editRect().right() + 1, AddressBar::kCellTop,
                          bar.itemRect(QStringLiteral("recent")).left() - bar.editRect().right() - 1,
                          AddressBar::kCellH);
        QVERIFY(countColour(img, devRect(img, after), t.textMuted) > 0);
        // An unclosed deref does not parse: the ring turns markerError and
        // the parser's words replace the preview.
        QTest::keyClicks(bar.editWidget(), QStringLiteral("\b\b\b\b[0x100"));
        QCOMPARE(bar.editText(), QStringLiteral("[0x100"));
        QVERIFY(!bar.editTextValid());
        QVERIFY(!bar.editPreviewText().isEmpty());
        QVERIFY(!bar.editPreviewText().startsWith(QStringLiteral("\u2192")));
        img = grabOf(bar);
        QVERIFY(countColour(img, seam, t.markerError) > seam.width() / 2);
        QCOMPARE(countColour(img, seam, t.borderFocused), 0);
    }

    void testEscapeRestoresAndReturnsFocus() {
        QApplication::processEvents();
        AddressBar* bar = m_editor->addressBar();
        const QString shownBefore = bar->baseDisplayText();
        QSignalSpy commit(m_editor, &RcxEditor::baseCommitRequested);
        QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier, bar->itemRect(QStringLiteral("base")).center());
        QVERIFY(bar->isEditing());
        QCOMPARE(bar->window()->focusWidget(), bar->editWidget());
        QTest::keyClicks(bar->editWidget(), QStringLiteral("0x5555"));
        QTest::keyClick(bar->editWidget(), Qt::Key_Escape);
        QApplication::processEvents();
        QVERIFY(!bar->isEditing());
        QCOMPARE(commit.count(), 0);
        QCOMPARE(bar->baseDisplayText(), shownBefore);
        QCOMPARE(m_doc->tree.baseAddress, 0ULL);
        QCOMPARE(bar->focusPolicy(), Qt::NoFocus);
        QCOMPARE(m_editor->window()->focusWidget(), m_editor->scintilla());
        QVERIFY(!bar->editWidget()->isVisible());
    }

    void testFocusOutReverts() {
        // Click-away is a cancel, never a commit (Explorer / Goto-dialog
        // semantics): focus moving to a sibling field drops the typed text.
        QWidget win;
        auto* lay = new QVBoxLayout(&win);
        lay->setContentsMargins(0, 0, 0, 0);
        auto* bar = new AddressBar(&win);
        QStringList commits;
        AddressBar::Callbacks cb;
        cb.onBaseCommit = [&](const QString& s) { commits << s; };
        bar->setCallbacks(std::move(cb));
        bar->setState(stateWith(twoLevel()));
        auto* sibling = new QLineEdit(&win);
        lay->addWidget(bar);
        lay->addWidget(sibling);
        win.resize(800, 80);
        win.show();
        QTest::qWait(30);
        QApplication::processEvents();
        const QString shownBefore = bar->baseDisplayText();
        QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier, bar->itemRect(QStringLiteral("base")).center());
        QVERIFY(bar->isEditing());
        QCOMPARE(win.focusWidget(), bar->editWidget());
        QTest::keyClicks(bar->editWidget(), QStringLiteral("0x77"));
        sibling->setFocus(Qt::MouseFocusReason);
        QApplication::processEvents();
        QCOMPARE(win.focusWidget(), sibling);
        if (bar->isEditing()) {
            // Not the active window (the hidden desktop): the focus chain
            // moved but no FocusOut was delivered. Hand the overlay the
            // event the click would have carried.
            QFocusEvent out(QEvent::FocusOut, Qt::MouseFocusReason);
            QApplication::sendEvent(bar->editWidget(), &out);
        }
        QVERIFY(!bar->isEditing());
        QVERIFY(commits.isEmpty());
        QCOMPARE(bar->baseDisplayText(), shownBefore);
        QCOMPARE(bar->focusPolicy(), Qt::NoFocus);
        QCOMPARE(win.focusWidget(), sibling);                    // no focus steal on a click-away
    }

    void testStatePushDuringEditIsDeferred() {
        // A live tick mid-edit must never stomp the typed text or move the
        // overlay: the state is stored, the relayout waits for the edit to
        // end, and then applies.
        AddressBar bar;
        bar.setState(stateWith(twoLevel()));
        showBar(bar, 800);
        QTest::mouseClick(&bar, Qt::LeftButton, Qt::NoModifier, bar.itemRect(QStringLiteral("base")).center());
        QVERIFY(bar.isEditing());
        QTest::keyClicks(bar.editWidget(), QStringLiteral("0x42"));
        const QRect overlay = bar.editRect();
        QVERIFY(bar.itemRect(QStringLiteral("crumb:2")).isNull());
        AddressBarState s = stateWith({ crumb(QStringLiteral("RcxEditor.vptr"), 0),
                                        crumb(QStringLiteral("QWidgetPrivate.parent"), 1),
                                        crumb(QStringLiteral("QWidget"), 2) });
        s.baseAddress = 0x1234; s.resolvedBase = 0x1234;
        const int n = bar.stateApplyCount();
        bar.setState(s);
        QCOMPARE(bar.stateApplyCount(), n + 1);                  // stored…
        QCOMPARE(bar.state().crumbs.size(), 3);
        QCOMPARE(bar.editText(), QStringLiteral("0x42"));       // …the text untouched…
        QCOMPARE(bar.editRect(), overlay);                       // …the overlay where it was…
        QVERIFY(bar.itemRect(QStringLiteral("crumb:2")).isNull());   // …the layout frozen.
        QVERIFY(bar.isEditing());
        QTest::keyClick(bar.editWidget(), Qt::Key_Escape);
        QVERIFY(!bar.isEditing());
        QVERIFY(!bar.itemRect(QStringLiteral("crumb:2")).isNull());  // applied now
        QCOMPARE(bar.baseDisplayText(), QStringLiteral("0x1234"));
        QCOMPARE(bar.segments().size(), 3);
    }

    void testStalePushDuringEditKeepsCellsSane() {
        // The frozen layout can outlive its crumbs: a push with FEWER crumbs
        // mid-edit leaves cells whose index is past the new trail. Hovering
        // one must not index out of range — empty tip — and the new state
        // lands after Escape.
        AddressBar bar;
        bar.setState(stateWith({ crumb(QStringLiteral("RcxEditor.vptr"), 0),
                                 crumb(QStringLiteral("QWidgetPrivate.parent"), 1),
                                 crumb(QStringLiteral("QWidget"), 2) }));
        showBar(bar, 900);
        QTest::mouseClick(&bar, Qt::LeftButton, Qt::NoModifier, bar.itemRect(QStringLiteral("base")).center());
        QVERIFY(bar.isEditing());
        const QRect stale = bar.itemRect(QStringLiteral("crumb:2"));
        QVERIFY(!stale.isNull());
        QVERIFY(stale.left() > bar.editRect().right());          // uncovered, hoverable
        bar.setState(stateWith({ crumb(QStringLiteral("RcxEditor"), 0) }));
        QCOMPARE(bar.state().crumbs.size(), 1);
        hoverAt(bar, stale.center());
        QVERIFY2(bar.toolTip().isEmpty(), qPrintable(bar.toolTip()));
        hoverAt(bar, bar.itemRect(QStringLiteral("chev:2")).center());
        QVERIFY2(bar.toolTip().isEmpty(), qPrintable(bar.toolTip()));
        grabOf(bar);                                             // paints the stale cells: no crash
        // A click on a stale crumb cell is a no-op, not an out-of-range pick.
        // The press lands on the bar (StrongFocus while editing), so it is
        // also a click-away: the overlay loses focus and the edit reverts,
        // which applies the lone-crumb state.
        int picked = -1;
        AddressBar::Callbacks cb;
        cb.onCrumb = [&](int i) { picked = i; };
        bar.setCallbacks(std::move(cb));
        QTest::mouseClick(&bar, Qt::LeftButton, Qt::NoModifier, bar.itemRect(QStringLiteral("crumb:1")).center());
        QCOMPARE(picked, -1);
        QVERIFY(!bar.isEditing());
        QCOMPARE(bar.focusPolicy(), Qt::NoFocus);
        QVERIFY(bar.itemRect(QStringLiteral("crumb:2")).isNull());
        QVERIFY(bar.itemRect(QStringLiteral("crumb:1")).isNull());
        QCOMPARE(bar.segments(), QStringList{ QStringLiteral("RcxEditor") });
    }

    void testDownOpensPlacesMenuAndPickInserts() {
        // Down in the edit lists the Goto recents, the document's bookmarks
        // and the source's modules, fuzzy-filtered by the typed text; a
        // pick fills the overlay and does NOT commit. Driven before any
        // event pump: the hidden desktop closes a popup at the first one.
        GotoAddressDialog::clearRecent();
        GotoAddressDialog::pushRecent(QStringLiteral("0x7FF60000"));
        GotoAddressDialog::pushRecent(QStringLiteral("<REECLASS.exe>+0x40"));   // most recent first
        Bookmark b; b.name = QStringLiteral("spawn"); b.addressFormula = QStringLiteral("<REECLASS.exe>+0x100");
        m_doc->tree.bookmarks.append(b);
        QApplication::processEvents();
        AddressBar* bar = m_editor->addressBar();
        QSignalSpy commit(m_editor, &RcxEditor::baseCommitRequested);
        QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier, bar->itemRect(QStringLiteral("base")).center());
        QVERIFY(bar->isEditing());
        QVERIFY(!visibleMenu(bar, QStringLiteral("rcxAddressBarEditMenu")));
        bar->editWidget()->clear();                              // empty text: unfiltered
        QTest::keyClick(bar->editWidget(), Qt::Key_Down);
        QMenu* menu = visibleMenu(bar, QStringLiteral("rcxAddressBarEditMenu"));
        QVERIFY(menu);
        QVERIFY(menu->isVisible());
        const QStringList texts = actionTexts(menu);
        QVERIFY2(texts.contains(QStringLiteral("<REECLASS.exe>+0x40")), qPrintable(texts.join('|')));
        QVERIFY2(texts.contains(QStringLiteral("0x7FF60000")), qPrintable(texts.join('|')));
        QVERIFY2(texts.contains(QStringLiteral("spawn  <REECLASS.exe>+0x100")), qPrintable(texts.join('|')));
        // Recent before bookmarks, most recent first.
        QVERIFY(texts.indexOf(QStringLiteral("<REECLASS.exe>+0x40")) < texts.indexOf(QStringLiteral("0x7FF60000")));
        QVERIFY(texts.indexOf(QStringLiteral("0x7FF60000")) < texts.indexOf(QStringLiteral("spawn  <REECLASS.exe>+0x100")));
        QVERIFY(!texts.contains(QStringLiteral("Clear recent")));       // the edit's menu inserts only
        QAction* pick = actionWithText(menu, QStringLiteral("<REECLASS.exe>+0x40"));
        QVERIFY(pick);
        pick->trigger();
        menu->hide();
        QTest::qWait(10);                                        // runs the menu's deleteLater
        QCOMPARE(bar->editText(), QStringLiteral("<REECLASS.exe>+0x40"));
        QVERIFY(bar->isEditing());                               // inserted, not committed
        QCOMPARE(commit.count(), 0);
        QCOMPARE(m_doc->tree.baseAddress, 0ULL);
        // Filtered: a substring of the bookmark's name leaves only it
        // (rcx::fuzzyScore is the strict matcher: contiguous substring or
        // word-start initials — "spa" finds "spawn", "spwn" finds nothing).
        bar->editWidget()->setText(QStringLiteral("spa"));
        QTest::keyClick(bar->editWidget(), Qt::Key_Down);
        menu = visibleMenu(bar, QStringLiteral("rcxAddressBarEditMenu"));
        QVERIFY(menu);
        QStringList entries;
        for (QAction* a : menu->actions()) if (!a->isSeparator() && a->data().isValid()) entries << a->text();
        QCOMPARE(entries, QStringList{ QStringLiteral("spawn  <REECLASS.exe>+0x100") });
        menu->hide();
        QTest::qWait(10);
        // Nothing matches → no menu at all.
        bar->editWidget()->setText(QStringLiteral("zzzz"));
        QTest::keyClick(bar->editWidget(), Qt::Key_Down);
        QVERIFY(!visibleMenu(bar, QStringLiteral("rcxAddressBarEditMenu")));
        QTest::keyClick(bar->editWidget(), Qt::Key_Escape);
        QVERIFY(!bar->isEditing());
    }

    void testLineZeroAddressClickEditsInTheBar() {
        // One base-edit implementation: a click on the command row's address
        // span opens the BAR's overlay; no Scintilla inline edit starts.
        QApplication::processEvents();
        QsciScintilla* sci = m_editor->scintilla();
        const int len = (int)sci->SendScintilla(QsciScintillaBase::SCI_LINELENGTH, (unsigned long)0);
        QVERIFY(len > 0);
        QByteArray buf(len + 1, '\0');
        sci->SendScintilla(QsciScintillaBase::SCI_GETLINE, (unsigned long)0, (void*)buf.data());
        QString line = QString::fromUtf8(buf.constData(), len);
        while (line.endsWith('\n') || line.endsWith('\r')) line.chop(1);
        const ColumnSpan as = commandRowAddrSpan(line);
        QVERIFY2(as.valid, qPrintable(line));
        // Character column → byte position (the row holds ▸ and ▾), then to
        // a viewport point on that glyph's row.
        const int col = (as.start + as.end) / 2;
        const long pos = (long)sci->SendScintilla(QsciScintillaBase::SCI_POSITIONFROMLINE, (unsigned long)0)
                       + line.left(col).toUtf8().size();
        const int x = (int)sci->SendScintilla(QsciScintillaBase::SCI_POINTXFROMPOSITION, 0UL, pos);
        const int y = (int)sci->SendScintilla(QsciScintillaBase::SCI_POINTYFROMPOSITION, 0UL, pos);
        const int lh = (int)sci->SendScintilla(QsciScintillaBase::SCI_TEXTHEIGHT, 0UL);
        AddressBar* bar = m_editor->addressBar();
        QVERIFY(!bar->isEditing());
        QTest::mouseClick(sci->viewport(), Qt::LeftButton, Qt::NoModifier, QPoint(x + 2, y + lh / 2));
        QApplication::processEvents();
        QVERIFY2(bar->isEditing(), "line-0 address click did not open the bar's edit");
        QVERIFY2(!m_editor->isEditing(), "a Scintilla inline edit started as well");
        QCOMPARE(bar->editText(), QStringLiteral("0x0"));
        QTest::keyClick(bar->editWidget(), Qt::Key_Escape);
        QVERIFY(!bar->isEditing());
        QVERIFY(!m_editor->isEditing());
    }

    void testRecentCellMenuGotoClearAndPickRebases() {
        // The recent cell: the same places, unfiltered, plus "Go to
        // address…" and "Clear recent"; a pick rebases through the
        // controller, undoably.
        GotoAddressDialog::clearRecent();
        GotoAddressDialog::pushRecent(QStringLiteral("0x7FF60000"));
        Bookmark b; b.name = QStringLiteral("spawn"); b.addressFormula = QStringLiteral("0x40");
        m_doc->tree.bookmarks.append(b);
        QApplication::processEvents();
        AddressBar* bar = m_editor->addressBar();
        const Theme& t = bar->theme();
        QSignalSpy pickSpy(m_editor, &RcxEditor::recentPickRequested);
        QSignalSpy gotoSpy(m_editor, &RcxEditor::gotoDialogRequested);
        const int undoBefore = m_doc->undoStack.count();
        const QRect recent = bar->itemRect(QStringLiteral("recent"));
        QVERIFY(!recent.isNull());
        QTest::mousePress(bar, Qt::LeftButton, Qt::NoModifier, recent.center());
        QMenu* menu = visibleMenu(bar, QStringLiteral("rcxAddressBarRecentMenu"));
        QVERIFY(menu);
        const QStringList texts = actionTexts(menu);
        QVERIFY2(texts.contains(QStringLiteral("0x7FF60000")), qPrintable(texts.join('|')));
        QVERIFY2(texts.contains(QStringLiteral("spawn  0x40")), qPrintable(texts.join('|')));
        QVERIFY2(texts.contains(QStringLiteral("Go to address\u2026\tCtrl+G")), qPrintable(texts.join('|')));
        QVERIFY2(texts.contains(QStringLiteral("Clear recent")), qPrintable(texts.join('|')));
        QVERIFY(actionWithText(menu, QStringLiteral("Clear recent"))->isEnabled());
        // The cell reads pressed while its menu is up (grab before the pump).
        QImage img = bar->grab().toImage().convertToFormat(QImage::Format_ARGB32);
        QVERIFY2(countColour(img, devRect(img, recent), pressedFill(t)) > 0, "recent cell not pressed while its menu is up");
        // "Go to address…" is a request the editor forwards; main.cpp wires
        // it in a later phase, so here only the signal is checked.
        actionWithText(menu, QStringLiteral("Go to address\u2026\tCtrl+G"))->trigger();
        QCOMPARE(gotoSpy.count(), 1);
        // A bookmark pick → recentPickRequested(formula) → rebaseTo.
        actionWithText(menu, QStringLiteral("spawn  0x40"))->trigger();
        menu->hide();
        QTest::mouseRelease(bar, Qt::LeftButton, Qt::NoModifier, recent.center());
        QTest::qWait(10);
        QCOMPARE(pickSpy.count(), 1);
        QCOMPARE(pickSpy.at(0).at(0).toString(), QStringLiteral("0x40"));
        QCOMPARE(m_doc->tree.baseAddress, 0x40ULL);
        QCOMPARE(m_doc->undoStack.count(), undoBefore + 1);
        QCOMPARE(bar->state().baseAddress, 0x40ULL);
        m_doc->undoStack.undo();
        QCOMPARE(m_doc->tree.baseAddress, 0ULL);
        img = grabOf(*bar);
        QVERIFY2(countColour(img, devRect(img, recent), pressedFill(t)) < 16, "recent cell stayed pressed after its menu hid");
        // Clear recent empties the shared list (restored by cleanup()).
        QTest::mousePress(bar, Qt::LeftButton, Qt::NoModifier, recent.center());
        menu = visibleMenu(bar, QStringLiteral("rcxAddressBarRecentMenu"));
        QVERIFY(menu);
        actionWithText(menu, QStringLiteral("Clear recent"))->trigger();
        menu->hide();
        QTest::mouseRelease(bar, Qt::LeftButton, Qt::NoModifier, recent.center());
        QTest::qWait(10);
        QVERIFY(GotoAddressDialog::loadRecent().isEmpty());
    }

    void testUpCellCollapsesOneLevel() {
        // Up = the parent crumb: one undoable collapse of the deepest hop,
        // the trail one shorter, the cell disabled again at the root.
        drillTwoLevels();
        QApplication::processEvents();
        AddressBar* bar = m_editor->addressBar();
        QVERIFY(bar->state().canUp);
        QSignalSpy spy(m_editor, &RcxEditor::navUpRequested);
        const int undoBefore = m_doc->undoStack.count();
        QCOMPARE(segments().size(), 3);
        const QRect up = bar->itemRect(QStringLiteral("up"));
        QVERIFY(!up.isNull());
        QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier, up.center());
        QApplication::processEvents();
        QCOMPARE(spy.count(), 1);
        QCOMPARE(m_ctrl->focusPath(), (QVector<uint64_t>{ m_id.vptr }));
        QCOMPARE(m_doc->undoStack.count(), undoBefore + 1);
        QVERIFY(collapsed(m_id.parent));
        QVERIFY(!collapsed(m_id.vptr));
        QCOMPARE(segments(), (QStringList{ QStringLiteral("RcxEditor.vptr"), QStringLiteral("QWidgetPrivate") }));
        QVERIFY(bar->state().canUp);
        QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier, up.center());
        QApplication::processEvents();
        QVERIFY(m_ctrl->focusPath().isEmpty());
        QCOMPARE(m_doc->undoStack.count(), undoBefore + 2);
        QVERIFY(!bar->state().canUp);
        // Disabled now: the click is ignored.
        QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier, up.center());
        QApplication::processEvents();
        QCOMPARE(spy.count(), 2);
        QCOMPARE(m_doc->undoStack.count(), undoBefore + 2);
        // Undo restores the trail through the push.
        m_doc->undoStack.undo();
        m_doc->undoStack.undo();
        QApplication::processEvents();
        QVERIFY(!collapsed(m_id.parent));
    }

    void testStaleDotIsNeverTheAccent() {
        // vs.json shipped without focusGlow, and Theme::fromJson's default
        // for it is borderFocused — which there IS the accent — so a stale
        // source's dot painted purple. The theme data now names a warning
        // amber everywhere; pinned under both polarities.
        for (const QString& name : { QStringLiteral("tw"), QStringLiteral("vs") }) {
            const Theme theme = loadTheme(name);
            QVERIFY2(theme.background.isValid(), qPrintable(name + QStringLiteral(".json not found under RCX_SOURCE_DIR")));
            QVERIFY2(theme.focusGlow != theme.indHoverSpan, qPrintable(name));
            AddressBar bar;
            bar.applyTheme(theme);
            AddressBarState s = stateWith(twoLevel());
            s.liveness = liveness::Stale;
            bar.setState(s);
            showBar(bar, 800);
            const QImage img = grabOf(bar);
            QCOMPARE(countColour(img, QRect(0, 0, img.width(), img.height()), theme.indHoverSpan), 0);
            QVERIFY2(countColour(img, devRect(img, bar.livenessDotRect()), theme.focusGlow) > 0, qPrintable(name));
        }
    }

    void testChipTooltipWithoutSavedKind() {
        // A live attach with no saved-source entry (self-attach, MCP,
        // kernel) has no identifier: the tip is the bare name, not
        // kindLabelFor("")'s "Plugin".
        AddressBar bar;
        AddressBarState s = stateWith(twoLevel());
        s.sourceKindId.clear();
        bar.setState(s);
        showBar(bar, 800);
        hoverAt(bar, bar.itemRect(QStringLiteral("src")).center());
        const QString tip = bar.toolTip();
        QVERIFY2(tip.startsWith(QStringLiteral("REECLASS.exe")), qPrintable(tip));
        QVERIFY2(!tip.contains(QStringLiteral("Plugin")), qPrintable(tip));
        QVERIFY(tip.contains(QStringLiteral("Live")));
    }
};

QTEST_MAIN(TestBreadcrumb)
#include "test_breadcrumb.moc"
