#include <QtTest/QTest>
#include <QtTest/QSignalSpy>
#include <QApplication>
#include <QSplitter>
#include <QElapsedTimer>
#include <QVBoxLayout>
#include <QToolButton>
#include <QPushButton>
#include <QButtonGroup>
#include <QLineEdit>
#include <QListView>
#include <QStringListModel>
#include <QLabel>
#include <QFrame>
#include <QScreen>
#include <QSet>
#include <QFontInfo>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QScrollBar>
#include <QScrollArea>
#include <QAbstractButton>
#include <QTextDocument>
#include <QtMath>
#include <Qsci/qsciscintilla.h>
#include "controller.h"
#include "typeselectorpopup.h"
#include "hextoolbarpopup.h"
#include "paintutil.h"
#include "widgets/enum_picker_popup.h"
#include "widgets/popup_chrome.h"
#include "themes/theme.h"
#include "themes/thememanager.h"
#include "widgets/category_chip.h"
#include "fontutil.h"
#include "core.h"

Q_DECLARE_METATYPE(rcx::TypeEntry)

using namespace rcx;

static void buildTwoRootTree(NodeTree& tree) {
    tree.baseAddress = 0;

    Node a;
    a.kind = NodeKind::Struct;
    a.name = "Alpha";
    a.structTypeName = "Alpha";
    a.parentId = 0;
    a.offset = 0;
    int ai = tree.addNode(a);
    uint64_t aId = tree.nodes[ai].id;

    { Node n; n.kind = NodeKind::Int32; n.name = "x"; n.parentId = aId; n.offset = 0; tree.addNode(n); }
    { Node n; n.kind = NodeKind::Int32; n.name = "y"; n.parentId = aId; n.offset = 4; tree.addNode(n); }

    Node b;
    b.kind = NodeKind::Struct;
    b.name = "Bravo";
    b.structTypeName = "Bravo";
    b.parentId = 0;
    b.offset = 0x100;
    int bi = tree.addNode(b);
    uint64_t bId = tree.nodes[bi].id;

    { Node n; n.kind = NodeKind::Float; n.name = "speed"; n.parentId = bId; n.offset = 0; tree.addNode(n); }
}

static QByteArray makeBuffer() {
    return QByteArray(0x200, '\0');
}

// ── Popup family chrome probes ──
// The popup family rule (the comment above SourceChooserPopup::paintEvent,
// src/sourcechooserpopup.cpp) pinned in pixels for the three popups this
// target links, the way test_source_chooser pins the reference: rendered
// through QWidget::render onto a scaled QImage, so paintutil's edge fills
// pick device rows exactly as they do on a 125 % screen.
namespace {

// A shipped theme by JSON basename — the built-in file next to the exe
// (build/themes/), the same one the app loads.
Theme shippedTheme(const QString& baseName) {
    QFile f(QCoreApplication::applicationDirPath() + QStringLiteral("/themes/") + baseName
            + QStringLiteral(".json"));
    if (!f.open(QIODevice::ReadOnly)) return Theme();
    return Theme::fromJson(QJsonDocument::fromJson(f.readAll()).object());
}

bool sameColour(QRgb px, const QColor& c, int tol = 2) {
    return qAbs(qRed(px) - c.red()) <= tol && qAbs(qGreen(px) - c.green()) <= tol
        && qAbs(qBlue(px) - c.blue()) <= tol;
}

int countColour(const QImage& img, const QRect& r, const QColor& c, int tol = 2) {
    int n = 0;
    for (int y = qMax(0, r.top()); y <= r.bottom() && y < img.height(); ++y)
        for (int x = qMax(0, r.left()); x <= r.right() && x < img.width(); ++x)
            if (sameColour(img.pixel(x, y), c, tol)) ++n;
    return n;
}

QImage renderAt(QWidget& w, qreal dpr) {
    QImage img(qRound(w.width() * dpr), qRound(w.height() * dpr), QImage::Format_ARGB32);
    img.setDevicePixelRatio(dpr);
    img.fill(Qt::black);
    QPainter p(&img);
    w.render(&p);
    return img;
}

// The device rows / columns the edge fills pick for a logical rect at
// `dpr`: qFloor(edge +- 0.5), paintutil's rule.
struct Edges { int top, bottom, left, right; };
Edges edgesOf(const QRect& logical, qreal dpr) {
    const QRectF dev(logical.left() * dpr, logical.top() * dpr,
                     logical.width() * dpr, logical.height() * dpr);
    return { qFloor(dev.top() + 0.5), qFloor(dev.bottom() - 0.5),
             qFloor(dev.left() + 0.5), qFloor(dev.right() - 0.5) };
}

// The frame: theme.border on exactly the outermost device row / column of
// each side, unbroken, and none on the row / column just inside.
bool frameIsOneDeviceRow(const QImage& img, const QRect& popupRect, const Theme& t, qreal dpr,
                         QString* why) {
    const Edges e = edgesOf(popupRect, dpr);
    const int w = e.right - e.left + 1, h = e.bottom - e.top + 1;
    auto check = [&](const QRect& r, int want, const char* what) {
        const int got = countColour(img, r, t.border);
        if (got == want) return true;
        // Name the first offending pixel: where, and what colour sits there.
        QString where;
        for (int y = r.top(); y <= r.bottom() && where.isEmpty(); ++y)
            for (int x = r.left(); x <= r.right() && where.isEmpty(); ++x)
                if (sameColour(img.pixel(x, y), t.border) != (want > 0))
                    where = QStringLiteral(" first at (%1,%2)=%3").arg(x).arg(y).arg(QColor(img.pixel(x, y)).name());
        *why = QStringLiteral("%1: %2 of %3 border px at dpr %4%5").arg(QLatin1String(what)).arg(got).arg(want).arg(dpr).arg(where);
        return false;
    };
    return e.top == 0 && e.left == 0 && e.bottom == img.height() - 1 && e.right == img.width() - 1
        && check(QRect(e.left, e.top, w, 1), w, "top row")
        && check(QRect(e.left, e.bottom, w, 1), w, "bottom row")
        && check(QRect(e.left, e.top, 1, h), h, "left column")
        && check(QRect(e.right, e.top, 1, h), h, "right column")
        && check(QRect(e.left + 1, e.top + 1, w - 2, 1), 0, "row inside the top")
        && check(QRect(e.left + 1, e.bottom - 1, w - 2, 1), 0, "row inside the bottom")
        && check(QRect(e.left + 1, e.top + 1, 1, h - 2), 0, "column inside the left")
        && check(QRect(e.right - 1, e.top + 1, 1, h - 2), 0, "column inside the right");
}

// One surface: `cols` device columns just inside a side border, full
// height, are the ground or a seam row (containerBorderColor / the field's
// borderFocused) and nothing else — no backgroundAlt strip, no second
// surface, no accent bar.
bool edgeStripIsGround(const QImage& img, const QRect& popupRect, const Theme& t, qreal dpr,
                       int cols, bool leftSide, QString* why, const QColor& alsoFine = QColor()) {
    const Edges e = edgesOf(popupRect, dpr);
    const QRect strip = leftSide ? QRect(e.left + 1, e.top + 1, cols, e.bottom - e.top - 1)
                                 : QRect(e.right - cols, e.top + 1, cols, e.bottom - e.top - 1);
    const int ground  = countColour(img, strip, t.background);
    const int seams   = countColour(img, strip, containerBorderColor(t));
    const int focused = countColour(img, strip, t.borderFocused);
    const int extra   = alsoFine.isValid() ? countColour(img, strip, alsoFine) : 0;
    const int all     = strip.width() * strip.height();
    if (ground + seams + focused + extra != all) {
        // Name the stray: the first pixel that is none of the above.
        QString where;
        for (int y = strip.top(); y <= strip.bottom() && where.isEmpty(); ++y)
            for (int x = strip.left(); x <= strip.right() && where.isEmpty(); ++x) {
                const QRgb px = img.pixel(x, y);
                if (!sameColour(px, t.background) && !sameColour(px, containerBorderColor(t))
                    && !sameColour(px, t.borderFocused) && !(alsoFine.isValid() && sameColour(px, alsoFine)))
                    where = QStringLiteral(" first stray at (%1,%2)=%3").arg(x).arg(y).arg(QColor(px).name());
            }
        *why = QStringLiteral("edge strip at x=%1 (dpr %2): %3 ground + %4 seam + %5 focus + %6 other of %7%8")
                   .arg(strip.left()).arg(dpr).arg(ground).arg(seams).arg(focused).arg(extra).arg(all).arg(where);
        return false;
    }
    if (countColour(img, strip, t.backgroundAlt, 0) != 0 && t.backgroundAlt != t.background) {
        *why = QStringLiteral("backgroundAlt inside the edge strip at x=%1").arg(strip.left());
        return false;
    }
    return true;
}

// The field: its bottom device row is the seam (containerBorderColor at
// rest, borderFocused while focused) across the whole field, the row above
// carries neither, and the band above the seam is the surface.
bool fieldHasOneSeam(const QImage& img, const QRect& fieldRect, const Theme& t, qreal dpr,
                     QString* why) {
    const Edges f = edgesOf(fieldRect, dpr);
    const int fw = f.right - f.left + 1;
    const QRect seamRow(f.left, f.bottom, fw, 1);
    const int rest    = countColour(img, seamRow, containerBorderColor(t));
    const int focused = countColour(img, seamRow, t.borderFocused);
    if (rest != fw && focused != fw) {
        *why = QStringLiteral("field seam: %1 rest / %2 focused of %3 (dpr %4)").arg(rest).arg(focused).arg(fw).arg(dpr);
        return false;
    }
    const QRect above(f.left, f.bottom - 1, fw, 1);
    if (countColour(img, above, containerBorderColor(t)) || countColour(img, above, t.borderFocused)) {
        *why = QStringLiteral("a second seam row above the field's (dpr %1)").arg(dpr);
        return false;
    }
    const QRect band(f.left, f.top, fw, f.bottom - f.top);
    if (countColour(img, band, t.background) <= band.width() * band.height() / 2) {
        *why = QStringLiteral("the field is not the surface (dpr %1)").arg(dpr);
        return false;
    }
    return true;
}

// A catalogue big enough to scroll the chooser's list: every primitive the
// app knows plus three structs, one with a name longer than the banner
// (the render harness's list).
QVector<TypeEntry> fullCatalogue() {
    QVector<TypeEntry> types;
    for (const auto& m : kKindMeta) {
        if (m.kind == NodeKind::Struct || m.kind == NodeKind::Array) continue;
        TypeEntry e;
        e.entryKind     = TypeEntry::Primitive;
        e.primitiveKind = m.kind;
        e.displayName   = QString::fromLatin1(m.typeName);
        e.sizeBytes     = m.size;
        e.alignment     = m.align;
        types.append(e);
    }
    auto composite = [&](const QString& name) {
        TypeEntry e;
        e.entryKind    = TypeEntry::Composite;
        e.structId     = 100 + types.size();
        e.displayName  = name;
        e.classKeyword = QStringLiteral("struct");
        e.kindGroup    = QStringLiteral("Ctr");
        e.sizeBytes    = 32;
        types.append(e);
    };
    composite(QStringLiteral("PlayerEntity"));
    composite(QStringLiteral("CameraState"));
    composite(QStringLiteral("PlayerEntityControllerStateMachineComponent"));
    return types;
}

// One interior device row, from the column after the left frame to the
// column before the right one, is entirely `a` (or entirely `b` when given):
// a seam that reaches the frame on both sides.
bool rowIsSeam(const QImage& img, const Edges& e, int y, const QColor& a, const QColor& b, QString* why) {
    const int inner = e.right - e.left - 1;
    const QRect row(e.left + 1, y, inner, 1);
    const int na = countColour(img, row, a);
    const int nb = b.isValid() ? countColour(img, row, b) : 0;
    if (na == inner || nb == inner) return true;
    QString where;
    for (int x = row.left(); x <= row.right() && where.isEmpty(); ++x) {
        const QRgb px = img.pixel(x, y);
        if (!sameColour(px, a) && !(b.isValid() && sameColour(px, b)))
            where = QStringLiteral(" first break at x=%1 (%2)").arg(x).arg(QColor(px).name());
    }
    *why = QStringLiteral("device row %1: %2 seam px of %3%4").arg(y).arg(qMax(na, nb)).arg(inner).arg(where);
    return false;
}

}  // namespace

// Define to skip GUI integration tests that flash windows (editor/controller tests).
// Keeps only TypeSelectorPopup-focused tests + benchmarks.
#define SKIP_GUI_INTEGRATION

class TestTypeSelector : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() {
        qRegisterMetaType<TypeEntry>("TypeEntry");
    }

    // ── Chevron span detection ──

    void testChevronSpanDetected() {
        QString text = QStringLiteral("[\u25B8] struct Alpha {");
        ColumnSpan span = commandRowChevronSpan(text);
        QVERIFY(span.valid);
        QCOMPARE(span.start, 0);
        QCOMPARE(span.end, 4);  // includes trailing space for easier clicking
    }

    void testChevronSpanRejects() {
        QVERIFY(!commandRowChevronSpan(QStringLiteral("Hi")).valid);
        QVERIFY(!commandRowChevronSpan(QStringLiteral("\u25B8 0x1000")).valid);
        // Old down-triangle glyph must not match
        QVERIFY(!commandRowChevronSpan(QStringLiteral("[\u25BE] 0x1000")).valid);
    }

    // ── Existing spans unbroken by chevron prefix ──

    void testSpansWithPrefix() {
        QString text = QStringLiteral("[\u25B8] struct Alpha {");

        // No source or address cell any more: the keyword follows the
        // chevron directly.
        ColumnSpan rootType = commandRowRootTypeSpan(text);
        QVERIFY(rootType.valid);
        QCOMPARE(rootType.start, commandRowChevronSpan(text).end);
        QCOMPARE(text.mid(rootType.start, rootType.end - rootType.start), QStringLiteral("struct"));

        ColumnSpan rootName = commandRowRootNameSpan(text);
        QVERIFY(rootName.valid);
        QCOMPARE(text.mid(rootName.start, rootName.end - rootName.start).trimmed(), QString("Alpha"));
    }

    // ── Benchmark: warmUp() + cached reuse vs cold new/delete ──

    void benchmarkPopupOpen() {
#ifdef SKIP_GUI_INTEGRATION
        QSKIP("GUI integration test disabled");
#endif
        auto makeComposite = [](uint64_t id, const QString& name, const QString& kw) {
            TypeEntry e;
            e.entryKind = TypeEntry::Composite;
            e.structId = id;
            e.displayName = name;
            e.classKeyword = kw;
            return e;
        };
        QVector<TypeEntry> types;
        types.append(makeComposite(1, "Alpha", "struct"));
        types.append(makeComposite(2, "Bravo", "struct"));
        types.append(makeComposite(3, "Charlie", "struct"));
        types.append(makeComposite(4, "Delta", "class"));

        TypeEntry cur1 = makeComposite(1, "Alpha", "struct");
        TypeEntry cur2 = makeComposite(2, "Bravo", "struct");

        QFont font("Consolas", 12);
        font.setFixedPitch(true);

        auto ms = [](qint64 ns) { return QString::number(ns / 1000000.0, 'f', 2); };

        // --- Measure cold path: new popup, first show ever ---
        {
            QElapsedTimer total;
            total.start();
            auto* popup = new TypeSelectorPopup();
            popup->setFont(font);
            popup->setTypes(types, &cur1);
            popup->popup(QPoint(100, 100));
            QApplication::processEvents();
            qint64 tCold = total.nsecsElapsed();
            popup->hide();
            QApplication::processEvents();

            qDebug() << "";
            qDebug().noquote() << QString("=== COLD (new popup, no warmUp) ===");
            qDebug().noquote() << QString("  Total: %1 ms").arg(ms(tCold));

            // --- Measure cached reuse of same instance ---
            {
                QElapsedTimer t2;
                t2.start();
                popup->setTypes(types, &cur2);
                popup->popup(QPoint(100, 100));
                QApplication::processEvents();
                qint64 tReuse = t2.nsecsElapsed();
                popup->hide();
                QApplication::processEvents();

                qDebug() << "";
                qDebug().noquote() << QString("=== WARM (reuse same popup) ===");
                qDebug().noquote() << QString("  Total: %1 ms").arg(ms(tReuse));
            }

            delete popup;
        }

        // --- Measure warmUp() approach ---
        {
            QElapsedTimer tWarmup;
            tWarmup.start();
            auto* popup2 = new TypeSelectorPopup();
            popup2->warmUp();
            qint64 tWarmMs = tWarmup.nsecsElapsed();

            qDebug() << "";
            qDebug().noquote() << QString("=== warmUp() cost (constructor + hidden show/hide) ===");
            qDebug().noquote() << QString("  Total: %1 ms").arg(ms(tWarmMs));

            // First user-visible show after warmUp
            QElapsedTimer t3;
            t3.start();
            popup2->setFont(font);
            popup2->setTypes(types, &cur1);
            popup2->popup(QPoint(100, 100));
            QApplication::processEvents();
            qint64 tFirst = t3.nsecsElapsed();
            popup2->hide();
            QApplication::processEvents();

            qDebug() << "";
            qDebug().noquote() << QString("=== FIRST visible show after warmUp() ===");
            qDebug().noquote() << QString("  Total: %1 ms").arg(ms(tFirst));

            // Second show (fully warm)
            QElapsedTimer t4;
            t4.start();
            popup2->setTypes(types, &cur2);
            popup2->popup(QPoint(100, 100));
            QApplication::processEvents();
            qint64 tSecond = t4.nsecsElapsed();
            popup2->hide();
            QApplication::processEvents();

            qDebug() << "";
            qDebug().noquote() << QString("=== SECOND visible show after warmUp() ===");
            qDebug().noquote() << QString("  Total: %1 ms").arg(ms(tSecond));

            delete popup2;
        }
    }

    // ── Isolate first-show cost with different window flags ──

    void benchmarkFirstShow() {
#ifdef SKIP_GUI_INTEGRATION
        QSKIP("GUI integration test disabled");
#endif
        auto ms = [](qint64 ns) { return QString::number(ns / 1000000.0, 'f', 2); };

        struct FlagTest {
            const char* name;
            Qt::WindowFlags flags;
        };
        FlagTest tests[] = {
            {"Qt::Popup|Frameless",         Qt::Popup | Qt::FramelessWindowHint},
            {"Qt::Tool|Frameless",          Qt::Tool | Qt::FramelessWindowHint},
            {"Qt::ToolTip",                 Qt::ToolTip},
            {"Qt::Window|Frameless",        Qt::Window | Qt::FramelessWindowHint},
            {"Qt::Popup|Frameless (2nd)",   Qt::Popup | Qt::FramelessWindowHint},
        };

        for (const auto& test : tests) {
            auto* f = new QFrame(nullptr, test.flags);
            f->resize(300, 400);

            QElapsedTimer t; t.start();
            f->show();
            qint64 t1 = t.nsecsElapsed(); t.restart();
            QApplication::processEvents();
            qint64 t2 = t.nsecsElapsed();
            f->hide();
            QApplication::processEvents();

            t.restart();
            f->show();
            qint64 t3 = t.nsecsElapsed(); t.restart();
            QApplication::processEvents();
            qint64 t4 = t.nsecsElapsed();
            f->hide();
            QApplication::processEvents();

            qDebug() << "";
            qDebug().noquote() << QString("=== %1 ===").arg(test.name);
            qDebug().noquote() << QString("  1st: show=%1ms events=%2ms | 2nd: show=%3ms events=%4ms")
                .arg(ms(t1)).arg(ms(t2)).arg(ms(t3)).arg(ms(t4));
            delete f;
        }

        // TypeSelectorPopup: cold vs after warmUp
        {
            auto* popup = new TypeSelectorPopup();
            TypeEntry dummy;
            dummy.entryKind = TypeEntry::Primitive;
            dummy.primitiveKind = NodeKind::Hex8;
            dummy.displayName = "test";
            popup->setTypes({dummy});

            QElapsedTimer t; t.start();
            popup->show();
            qint64 t1 = t.nsecsElapsed(); t.restart();
            QApplication::processEvents();
            qint64 t2 = t.nsecsElapsed();
            popup->hide();
            QApplication::processEvents();

            t.restart();
            popup->show();
            qint64 t3 = t.nsecsElapsed(); t.restart();
            QApplication::processEvents();
            qint64 t4 = t.nsecsElapsed();
            popup->hide();
            QApplication::processEvents();

            qDebug() << "";
            qDebug().noquote() << QString("=== TypeSelectorPopup (cold, Qt::Popup) ===");
            qDebug().noquote() << QString("  1st: show=%1ms events=%2ms | 2nd: show=%3ms events=%4ms")
                .arg(ms(t1)).arg(ms(t2)).arg(ms(t3)).arg(ms(t4));
            delete popup;
        }

        // Clean order test: dummy popup with children FIRST, then TypeSelectorPopup
        qDebug() << "";
        qDebug() << "=== CLEAN: dummy popup first, then TypeSelectorPopup ===";
        {
            auto* dummy = new QFrame(nullptr, Qt::Popup | Qt::FramelessWindowHint);
            dummy->resize(300, 400);
            auto* dLay = new QVBoxLayout(dummy);
            dLay->addWidget(new QLabel("dummy"));
            dLay->addWidget(new QLineEdit);
            auto* dModel = new QStringListModel(dummy);
            QStringList dItems; for (int i = 0; i < 10; i++) dItems << "x";
            dModel->setStringList(dItems);
            auto* dLv = new QListView; dLv->setModel(dModel);
            dLay->addWidget(dLv);

            QElapsedTimer t; t.start();
            dummy->show();
            qint64 t1 = t.nsecsElapsed(); t.restart();
            QApplication::processEvents();
            qint64 t2 = t.nsecsElapsed();
            dummy->hide();
            QApplication::processEvents();
            qDebug().noquote() << QString("  Dummy popup: show=%1ms events=%2ms").arg(ms(t1)).arg(ms(t2));
            delete dummy;
        }
        {
            auto* popup = new TypeSelectorPopup();
            TypeEntry e;
            e.entryKind = TypeEntry::Primitive;
            e.primitiveKind = NodeKind::Hex8;
            e.displayName = "test";
            popup->setTypes({e});
            popup->resize(300, 400);
            QElapsedTimer t; t.start();
            popup->show();
            qint64 t1 = t.nsecsElapsed(); t.restart();
            QApplication::processEvents();
            qint64 t2 = t.nsecsElapsed();
            popup->hide();
            QApplication::processEvents();
            qDebug().noquote() << QString("  TypeSelectorPopup (after dummy): show=%1ms events=%2ms").arg(ms(t1)).arg(ms(t2));
            delete popup;
        }
    }

    // ── Benchmark: large SDK (5000 structs) ──

    void benchmarkLargeSDK() {
        auto ms = [](qint64 ns) { return QString::number(ns / 1000000.0, 'f', 2); };

        // Build 5000 composite types with field summaries (simulates WinSDK)
        QVector<TypeEntry> types;
        types.reserve(5000);
        for (int i = 0; i < 5000; i++) {
            TypeEntry e;
            e.entryKind = TypeEntry::Composite;
            e.structId = (uint64_t)(i + 1);
            e.displayName = QStringLiteral("_STRUCT_%1").arg(i, 4, 10, QChar('0'));
            e.classKeyword = QStringLiteral("struct");
            e.sizeBytes = 64 + (i % 256) * 8;
            e.alignment = 8;
            e.fieldCount = 5 + (i % 20);
            for (int f = 0; f < qMin(6, e.fieldCount); f++)
                e.fieldSummary << QStringLiteral("0x%1: int32_t field_%2")
                    .arg(f * 4, 2, 16, QChar('0')).arg(f);
            types.append(e);
        }

        QFont font("Consolas", 12);
        font.setFixedPitch(true);
        auto* popup = new TypeSelectorPopup();
        popup->warmUp();
        popup->setFont(font);

        // Measure setTypes (data loading)
        QElapsedTimer t;
        t.start();
        popup->setTypes(types, nullptr);
        qint64 tSetTypes = t.nsecsElapsed();

        // Measure popup show (broken down)
        t.restart();
        popup->popup(QPoint(100, 100));
        qint64 tPopupCall = t.nsecsElapsed();
        t.restart();
        QApplication::processEvents();
        qint64 tProcessEvents = t.nsecsElapsed();
        qint64 tShow = tPopupCall + tProcessEvents;

        // Second popup show (warm)
        popup->hide();
        QApplication::processEvents();
        t.restart();
        popup->popup(QPoint(100, 100));
        qint64 tPopup2 = t.nsecsElapsed();
        t.restart();
        QApplication::processEvents();
        qint64 tProcess2 = t.nsecsElapsed();

        // Measure filter with 1-char (worst case: most matches)
        t.restart();
        auto* filterEdit = popup->findChild<QLineEdit*>();
        QVERIFY(filterEdit);

        filterEdit->setText(QStringLiteral("S"));
        qint64 tFilter1 = t.nsecsElapsed();

        // Measure filter with 3-char (moderate filtering)
        t.restart();
        filterEdit->setText(QStringLiteral("STR"));
        qint64 tFilter3 = t.nsecsElapsed();

        // Measure filter with 6-char (narrow results)
        t.restart();
        filterEdit->setText(QStringLiteral("STRUCT"));
        qint64 tFilter6 = t.nsecsElapsed();

        // Measure clear filter (back to grouped view)
        t.restart();
        filterEdit->setText(QString());
        qint64 tClear = t.nsecsElapsed();

        popup->hide();
        QApplication::processEvents();

        qDebug() << "";
        qDebug().noquote() << "=== Large SDK Benchmark (5000 structs) ===";
        qDebug().noquote() << QString("  setTypes:        %1 ms").arg(ms(tSetTypes));
        qDebug().noquote() << QString("  popup() call:    %1 ms").arg(ms(tPopupCall));
        qDebug().noquote() << QString("  processEvents:   %1 ms").arg(ms(tProcessEvents));
        qDebug().noquote() << QString("  popup total:     %1 ms").arg(ms(tShow));
        qDebug().noquote() << QString("  popup2() call:   %1 ms  (warm)").arg(ms(tPopup2));
        qDebug().noquote() << QString("  processEvents2:  %1 ms  (warm)").arg(ms(tProcess2));
        qDebug().noquote() << QString("  popup2 total:    %1 ms  (warm)").arg(ms(tPopup2 + tProcess2));
        qDebug().noquote() << QString("  filter 'S':    %1 ms").arg(ms(tFilter1));
        qDebug().noquote() << QString("  filter 'STR':  %1 ms").arg(ms(tFilter3));
        qDebug().noquote() << QString("  filter 'STRUCT': %1 ms").arg(ms(tFilter6));
        qDebug().noquote() << QString("  clear filter:  %1 ms").arg(ms(tClear));
        QVERIFY(tSetTypes > 0);

        delete popup;
    }

    // ── Popup data model ──

    void testPopupListsRootStructs() {
        NodeTree tree;
        buildTwoRootTree(tree);

        QVector<TypeEntry> types;
        for (const auto& n : tree.nodes) {
            if (n.parentId == 0 && n.kind == NodeKind::Struct) {
                TypeEntry e;
                e.entryKind = TypeEntry::Composite;
                e.structId = n.id;
                e.displayName = n.structTypeName.isEmpty() ? n.name : n.structTypeName;
                e.classKeyword = n.resolvedClassKeyword();
                types.append(e);
            }
        }

        QCOMPARE(types.size(), 2);
        QCOMPARE(types[0].displayName, QString("Alpha"));
        QCOMPARE(types[1].displayName, QString("Bravo"));
    }

    // ── Popup signals ──

    void testPopupSignals() {
        TypeSelectorPopup popup;

        TypeEntry eA;
        eA.entryKind = TypeEntry::Composite;
        eA.structId = 1;
        eA.displayName = "A";
        eA.classKeyword = "struct";
        TypeEntry eB;
        eB.entryKind = TypeEntry::Composite;
        eB.structId = 2;
        eB.displayName = "B";
        eB.classKeyword = "struct";
        QVector<TypeEntry> types;
        types.append(eA);
        types.append(eB);
        popup.setTypes(types, &eA);

        QSignalSpy typeSpy(&popup, &TypeSelectorPopup::typeSelected);
        QSignalSpy createSpy(&popup, &TypeSelectorPopup::createNewTypeRequested);

        emit popup.typeSelected(eB, QStringLiteral("B"));
        QCOMPARE(typeSpy.count(), 1);
        // Verify the entry came through — check the fullText (second arg)
        QCOMPARE(typeSpy.at(0).at(1).toString(), QStringLiteral("B"));

        emit popup.createNewTypeRequested(0, 0, QString(), QString());
        QCOMPARE(createSpy.count(), 1);
    }

    // ── Simple-by-default type list + "show all" + filter-finds-all ──
    // Default (simple) mode shows only the common primitive set + user
    // structs, hiding long-tail primitives and the std-lib "Common Types"
    // section behind a bottom expand toggle. Crucially, typing in the
    // filter still surfaces hidden types (the fuzzy path searches all),
    // so nothing is ever unreachable.
    void testSimpleModeHidesLongTailButFilterFindsAll() {
        TypeSelectorPopup popup;
        popup.setMode(TypePopupMode::FieldType);

        auto prim = [](NodeKind k, const QString& name) {
            TypeEntry e;
            e.entryKind = TypeEntry::Primitive;
            e.primitiveKind = k;
            e.displayName = name;
            e.sizeBytes = sizeForKind(k);
            return e;
        };
        QVector<TypeEntry> types;
        types << prim(NodeKind::Hex64, "hex64")       // common
              << prim(NodeKind::UInt32, "uint32_t")   // common
              << prim(NodeKind::Vec3, "vec3");        // long-tail (hidden)
        TypeEntry stdlib;                             // std-lib "Common" (hidden)
        stdlib.entryKind = TypeEntry::Composite;
        stdlib.displayName = "UNICODE_STRING";
        stdlib.classKeyword = "struct";
        stdlib.kindGroup = "Common";
        types << stdlib;
        TypeEntry userStruct;                         // user struct (always shown)
        userStruct.entryKind = TypeEntry::Composite;
        userStruct.structId = 7;
        userStruct.displayName = "MyStruct";
        userStruct.classKeyword = "struct";
        types << userStruct;

        popup.setTypes(types, nullptr);  // default = simple, empty filter

        auto names = [&]() {
            QStringList out;
            for (const auto& e : popup.filteredTypes())
                if (e.entryKind != TypeEntry::Section && !e.isExpandToggle)
                    out << e.displayName;
            return out;
        };

        QStringList simple = names();
        QVERIFY(simple.contains("hex64"));
        QVERIFY(simple.contains("uint32_t"));
        QVERIFY(simple.contains("MyStruct"));          // user structs always shown
        QVERIFY(!simple.contains("vec3"));             // long-tail hidden
        QVERIFY(!simple.contains("UNICODE_STRING"));   // std-lib section hidden

        // Collapsible section headers replace the old global "Show all types"
        // toggle: long-tail groups (Vec, std-lib "Common types") appear as
        // collapsed headers whose members are hidden until expanded.
        bool hasCollapsibleSection = false;
        for (const auto& e : popup.filteredTypes())
            if (e.entryKind == TypeEntry::Section && e.sectionCollapsible)
                { hasCollapsibleSection = true; break; }
        QVERIFY2(hasCollapsibleSection, "expected collapsible section headers");

        // Filtering surfaces hidden types even though we're in simple mode.
        auto* filterEdit = popup.findChild<QLineEdit*>();
        QVERIFY(filterEdit);
        filterEdit->setText("vec3");
        QApplication::processEvents();
        QVERIFY2(names().contains("vec3"),
                 "filter must search ALL types regardless of simple mode");
    }

    // ── Full GUI integration ──
    // Single test method to avoid QScintilla reinit issues.

    void testViewSwitchingAndCreateType() {
#ifdef SKIP_GUI_INTEGRATION
        QSKIP("GUI integration test disabled");
#endif
        auto* doc = new RcxDocument();
        buildTwoRootTree(doc->tree);
        doc->provider = std::make_unique<BufferProvider>(makeBuffer());

        auto* splitter = new QSplitter();
        auto* ctrl = new RcxController(doc, nullptr);
        auto* editor = ctrl->addSplitEditor(splitter);

        splitter->resize(800, 600);
        splitter->show();
        QVERIFY(QTest::qWaitForWindowExposed(splitter));

        // Initial refresh so compose populates meta + editor text
        ctrl->refresh();
        QApplication::processEvents();

        auto* sci = editor->scintilla();

        // -- Command row starts with [U+25B8] --
        {
            const LineMeta* meta = editor->metaForLine(0);
            QVERIFY(meta);
            QCOMPARE(meta->lineKind, LineKind::CommandRow);

            QString line0 = sci->text(0);
            if (line0.endsWith('\n')) line0.chop(1);
            QVERIFY2(line0.startsWith(QStringLiteral("[\u25B8]")),
                     qPrintable("Expected chevron prefix, got: " + line0.left(10)));
        }

        // -- Find root IDs --
        uint64_t alphaId = 0, bravoId = 0;
        for (const auto& n : doc->tree.nodes) {
            if (n.parentId == 0 && n.kind == NodeKind::Struct) {
                if (n.name == "Alpha") alphaId = n.id;
                if (n.name == "Bravo") bravoId = n.id;
            }
        }
        QVERIFY(alphaId != 0);
        QVERIFY(bravoId != 0);
        QCOMPARE(ctrl->viewRootId(), (uint64_t)0);

        // -- Switch to Bravo: command row + fields update --
        ctrl->setViewRootId(bravoId);
        QApplication::processEvents();

        QCOMPARE(ctrl->viewRootId(), bravoId);
        QVERIFY2(sci->text(0).contains("Bravo"),
                 qPrintable("Expected 'Bravo' in command row, got: " + sci->text(0)));
        QVERIFY2(sci->text().contains("speed"),
                 "View should show Bravo's 'speed' field");

        // -- Switch to Alpha --
        ctrl->setViewRootId(alphaId);
        QApplication::processEvents();

        QCOMPARE(ctrl->viewRootId(), alphaId);
        QVERIFY2(sci->text(0).contains("Alpha"),
                 qPrintable("Expected 'Alpha' in command row, got: " + sci->text(0)));

        // -- Create new type (no name) --
        int nodesBefore = doc->tree.nodes.size();

        Node newNode;
        newNode.kind = NodeKind::Struct;
        newNode.name = QString();
        newNode.parentId = 0;
        newNode.offset = 0;
        newNode.id = doc->tree.reserveId();
        uint64_t newId = newNode.id;

        doc->undoStack.push(new RcxCommand(ctrl, cmd::Insert{newNode}));
        ctrl->setViewRootId(newId);
        QApplication::processEvents();

        // Verify new struct
        int idx = doc->tree.indexOfId(newId);
        QVERIFY(idx >= 0);
        QVERIFY(doc->tree.nodes[idx].name.isEmpty());
        QCOMPARE(doc->tree.nodes[idx].kind, NodeKind::Struct);
        QCOMPARE(doc->tree.nodes[idx].parentId, (uint64_t)0);
        QCOMPARE(ctrl->viewRootId(), newId);

        // Command row shows "Untitled" placeholder for empty-named struct
        // (was "NoName" — replaced because that programmer-grade default
        // looked like a bug to users; matches MainWindow::rootName fallback).
        QVERIFY2(sci->text(0).contains("Untitled"),
                 qPrintable("Expected 'Untitled' in command row, got: " + sci->text(0)));

        // -- Undo removes the new struct --
        doc->undoStack.undo();
        QApplication::processEvents();
        QCOMPARE(doc->tree.nodes.size(), nodesBefore);

        // Cleanup
        delete ctrl;
        delete splitter;
        delete doc;
    }

    // ── parseTypeSpec tests ──

    void testParseTypeSpecPlain() {
        TypeSpec spec = parseTypeSpec("int32_t");
        QCOMPARE(spec.baseName, QString("int32_t"));
        QVERIFY(!spec.isPointer);
        QCOMPARE(spec.arrayCount, 0);
    }

    void testParseTypeSpecArray() {
        TypeSpec spec = parseTypeSpec("int32_t[10]");
        QCOMPARE(spec.baseName, QString("int32_t"));
        QVERIFY(!spec.isPointer);
        QCOMPARE(spec.arrayCount, 10);
    }

    void testParseTypeSpecPointer() {
        TypeSpec spec = parseTypeSpec("Ball*");
        QCOMPARE(spec.baseName, QString("Ball"));
        QVERIFY(spec.isPointer);
        QCOMPARE(spec.ptrDepth, 1);
        QCOMPARE(spec.arrayCount, 0);
    }

    void testParseTypeSpecDoublePointer() {
        TypeSpec spec = parseTypeSpec("Ball**");
        QCOMPARE(spec.baseName, QString("Ball"));
        QVERIFY(spec.isPointer);
        QCOMPARE(spec.ptrDepth, 2);
    }

    void testParseTypeSpecEmpty() {
        TypeSpec spec = parseTypeSpec("");
        QVERIFY(spec.baseName.isEmpty());
        QVERIFY(!spec.isPointer);
        QCOMPARE(spec.arrayCount, 0);
    }

    void testParseTypeSpecWhitespace() {
        TypeSpec spec = parseTypeSpec("  Ball *  ");
        // trimmed → "Ball *", ends with '*'
        QCOMPARE(spec.baseName, QString("Ball"));
        QVERIFY(spec.isPointer);
    }

    void testParseTypeSpecArrayZero() {
        // [0] parses baseName but arrayCount stays 0 (invalid count)
        TypeSpec spec = parseTypeSpec("int32_t[0]");
        QCOMPARE(spec.baseName, QString("int32_t"));
        QCOMPARE(spec.arrayCount, 0);
    }

    // ── Regression: re-filtering must PRESERVE the user's selection, not snap
    // it back to the first row. Before the fix, typing one more character (or
    // toggling a chip / re-sorting) reset the selection to row 0, so a
    // following Enter committed the wrong type. Pure model-state test —
    // deterministic headless, no GUI integration needed.
    void testSelectionSurvivesRefilter() {
        TypeSelectorPopup popup;
        auto mk = [](const QString& n, NodeKind k) {
            TypeEntry e; e.entryKind = TypeEntry::Primitive;
            e.primitiveKind = k; e.displayName = n; return e;
        };
        QVector<TypeEntry> types;
        types << mk("alpha", NodeKind::Int32)
              << mk("gamma_target", NodeKind::Int32)
              << mk("gamble", NodeKind::Int32);
        popup.setTypes(types);

        auto* listView = popup.findChild<QListView*>();
        QVERIFY(listView);
        auto* filterEdit = popup.findChild<QLineEdit*>();
        QVERIFY(filterEdit);
        auto* model = listView->model();
        QVERIFY(model);

        // Select "gamma_target".
        int targetRow = -1;
        for (int r = 0; r < model->rowCount(); ++r)
            if (model->index(r, 0).data().toString().contains("gamma_target")) { targetRow = r; break; }
        QVERIFY(targetRow >= 0);
        listView->setCurrentIndex(model->index(targetRow, 0));

        // Type a filter that still matches the selected entry (and others).
        filterEdit->setText(QStringLiteral("gam"));
        QApplication::processEvents();

        // Selection must still be on "gamma_target", NOT snapped to row 0.
        QModelIndex cur = listView->currentIndex();
        QVERIFY(cur.isValid());
        QVERIFY2(cur.data().toString().contains("gamma_target"),
                 qPrintable(QStringLiteral("selection moved to: %1").arg(cur.data().toString())));
    }

    // ── Regression: dismissed() means "closed WITHOUT a pick". It must NOT
    // fire when the user actually committed a selection (the hideEvent that
    // follows an accept previously emitted dismissed() too). Pure signal-state
    // test — no GUI position needed.
    void testDismissedNotEmittedOnPick() {
        TypeSelectorPopup popup;
        TypeEntry e; e.entryKind = TypeEntry::Primitive;
        e.primitiveKind = NodeKind::Int32; e.displayName = "int32_t";
        popup.setTypes({e});

        QSignalSpy dismissSpy(&popup, &TypeSelectorPopup::dismissed);
        QVERIFY(dismissSpy.isValid());

        // 1) Plain dismiss (no pick) DOES emit dismissed().
        popup.show();
        QApplication::processEvents();
        popup.hide();
        QApplication::processEvents();
        QCOMPARE(dismissSpy.count(), 1);

        // 2) A committed pick (via "New" button) must NOT emit dismissed() again.
        // The create/OK buttons are DialogButtons (QPushButton), not QToolButton.
        QPushButton* newBtn = nullptr;
        for (auto* b : popup.findChildren<QPushButton*>())
            if (b->text().contains("New")) { newBtn = b; break; }
        QVERIFY2(newBtn, "Could not find 'New' button");

        QSignalSpy createSpy(&popup, &TypeSelectorPopup::createNewTypeRequested);
        popup.show();
        QApplication::processEvents();
        newBtn->click();  // create pick → createNewTypeRequested + hide
        QApplication::processEvents();
        QCOMPARE(createSpy.count(), 1);    // the pick did fire
        QCOMPARE(dismissSpy.count(), 1);   // ...but dismissed did NOT fire again
    }

    // Regression: opening near a screen edge must yield an on-screen popup of a
    // usable size. The old geometry math sized height as
    // `avail.bottom() - globalPos.y()` (and width likewise), which collapses to
    // ~1px — or negative — when opened at the bottom/right edge. placeOnScreen()
    // now clamps to a minimum and shifts the origin back on-screen. Geometry is
    // set before show(), so this is deterministic + headless (no GUI flake).
    void testPopupStaysOnScreenAtEdge() {
        QScreen* screen = QApplication::primaryScreen();
        QVERIFY(screen);
        const QRect avail = screen->availableGeometry();

        TypeSelectorPopup popup;
        TypeEntry e; e.entryKind = TypeEntry::Primitive;
        e.primitiveKind = NodeKind::Int32; e.displayName = "int32_t";
        popup.setTypes({e});

        // Worst case for the old shrink-to-fit math: the extreme bottom-right.
        popup.popupLoading(QPoint(avail.right() - 1, avail.bottom() - 1));
        QApplication::processEvents();

        const QSize sz = popup.size();
        // Never degenerate. (A tiny headless 'offscreen' work area caps the
        // popup to the screen, so compare against min(120, work-area).)
        QVERIFY2(sz.width()  >= qMin(120, avail.width())
              && sz.height() >= qMin(120, avail.height()),
                 qPrintable(QString("degenerate popup size %1x%2 (work area %3x%4)")
                    .arg(sz.width()).arg(sz.height()).arg(avail.width()).arg(avail.height())));

        // And it must sit fully within the work area — origin shifted back
        // on-screen rather than spilling off the right/bottom edge.
        const QRect g = popup.geometry();
        QVERIFY2(g.left() >= avail.left() && g.top() >= avail.top()
              && g.right() <= avail.right() && g.bottom() <= avail.bottom(),
                 qPrintable(QString("popup (%1,%2 %3x%4) spills off work area (%5,%6 %7x%8)")
                    .arg(g.left()).arg(g.top()).arg(g.width()).arg(g.height())
                    .arg(avail.left()).arg(avail.top()).arg(avail.width()).arg(avail.height())));
    }

    // ── FieldType popup: selecting a composite (struct) type changes node kind + structTypeName + collapsed ──

    void testFieldTypeCompositeChangesNodeToStruct() {
#ifdef SKIP_GUI_INTEGRATION
        QSKIP("GUI integration test disabled");
#endif
        auto* doc = new RcxDocument();
        buildTwoRootTree(doc->tree);
        doc->provider = std::make_unique<BufferProvider>(makeBuffer());

        auto* splitter = new QSplitter();
        auto* ctrl = new RcxController(doc, nullptr);
        ctrl->addSplitEditor(splitter);

        splitter->resize(800, 600);
        splitter->show();
        QVERIFY(QTest::qWaitForWindowExposed(splitter));
        ctrl->refresh();
        QApplication::processEvents();

        // Find the "x" field (Int32) inside Alpha struct, and Bravo struct id
        int xIdx = -1;
        uint64_t bravoId = 0;
        for (int i = 0; i < doc->tree.nodes.size(); i++) {
            const auto& n = doc->tree.nodes[i];
            if (n.name == "x" && n.kind == NodeKind::Int32) xIdx = i;
            if (n.name == "Bravo" && n.kind == NodeKind::Struct) bravoId = n.id;
        }
        QVERIFY(xIdx >= 0);
        QVERIFY(bravoId != 0);

        QCOMPARE(doc->tree.nodes[xIdx].kind, NodeKind::Int32);
        // Leaf nodes default to collapsed=true; set to false to verify
        // that ChangePointerRef correctly sets collapsed=true for struct refs.
        doc->tree.nodes[xIdx].collapsed = false;
        uint64_t xNodeId = doc->tree.nodes[xIdx].id;

        // Simulate the plain-struct path of applyTypePopupResult:
        // beginMacro → changeNodeKind(Struct) → ChangeStructTypeName → ChangePointerRef → endMacro
        doc->undoStack.beginMacro(QStringLiteral("Change to composite type"));
        ctrl->changeNodeKind(xIdx, NodeKind::Struct);

        xIdx = doc->tree.indexOfId(xNodeId);
        QVERIFY(xIdx >= 0);

        int bravoIdx = doc->tree.indexOfId(bravoId);
        QVERIFY(bravoIdx >= 0);
        QString targetName = doc->tree.nodes[bravoIdx].structTypeName;

        doc->undoStack.push(new RcxCommand(ctrl,
            cmd::ChangeStructTypeName{xNodeId, doc->tree.nodes[xIdx].structTypeName, targetName}));

        // Set refId so compose can expand referenced struct children (auto-collapses)
        doc->undoStack.push(new RcxCommand(ctrl,
            cmd::ChangePointerRef{xNodeId, 0, bravoId}));

        doc->undoStack.endMacro();
        QApplication::processEvents();

        // Verify: Struct with correct name, refId, AND collapsed
        xIdx = doc->tree.indexOfId(xNodeId);
        QVERIFY(xIdx >= 0);
        QCOMPARE(doc->tree.nodes[xIdx].kind, NodeKind::Struct);
        QCOMPARE(doc->tree.nodes[xIdx].structTypeName, QString("Bravo"));
        QCOMPARE(doc->tree.nodes[xIdx].refId, bravoId);
        QVERIFY(doc->tree.nodes[xIdx].collapsed);

        // Single undo reverses the entire macro
        doc->undoStack.undo();
        QApplication::processEvents();
        xIdx = doc->tree.indexOfId(xNodeId);
        QVERIFY(xIdx >= 0);
        QCOMPARE(doc->tree.nodes[xIdx].kind, NodeKind::Int32);
        QCOMPARE(doc->tree.nodes[xIdx].refId, uint64_t(0));
        QVERIFY(doc->tree.nodes[xIdx].structTypeName.isEmpty());

        delete ctrl;
        delete splitter;
        delete doc;
    }

    // ── FieldType popup: selecting a composite with * modifier creates Pointer64 + refId ──

    void testFieldTypeCompositeWithPointerModifier() {
#ifdef SKIP_GUI_INTEGRATION
        QSKIP("GUI integration test disabled");
#endif
        auto* doc = new RcxDocument();
        buildTwoRootTree(doc->tree);
        doc->provider = std::make_unique<BufferProvider>(makeBuffer());

        auto* splitter = new QSplitter();
        auto* ctrl = new RcxController(doc, nullptr);
        ctrl->addSplitEditor(splitter);

        splitter->resize(800, 600);
        splitter->show();
        QVERIFY(QTest::qWaitForWindowExposed(splitter));
        ctrl->refresh();
        QApplication::processEvents();

        // Find the "x" field (Int32) and Bravo struct
        int xIdx = -1;
        uint64_t bravoId = 0;
        for (int i = 0; i < doc->tree.nodes.size(); i++) {
            const auto& n = doc->tree.nodes[i];
            if (n.name == "x" && n.kind == NodeKind::Int32) xIdx = i;
            if (n.name == "Bravo" && n.kind == NodeKind::Struct) bravoId = n.id;
        }
        QVERIFY(xIdx >= 0);
        QVERIFY(bravoId != 0);

        uint64_t xNodeId = doc->tree.nodes[xIdx].id;

        // Simulate the pointer path of applyTypePopupResult:
        // beginMacro → changeNodeKind(Pointer64) → ChangePointerRef → endMacro
        doc->undoStack.beginMacro(QStringLiteral("Change to composite type"));
        ctrl->changeNodeKind(xIdx, NodeKind::Pointer64);

        xIdx = doc->tree.indexOfId(xNodeId);
        QVERIFY(xIdx >= 0);
        QCOMPARE(doc->tree.nodes[xIdx].kind, NodeKind::Pointer64);

        doc->undoStack.push(new RcxCommand(ctrl,
            cmd::ChangePointerRef{xNodeId, 0, bravoId}));
        doc->undoStack.endMacro();
        QApplication::processEvents();

        // Verify: Pointer64 with refId pointing to Bravo, auto-collapsed
        xIdx = doc->tree.indexOfId(xNodeId);
        QVERIFY(xIdx >= 0);
        QCOMPARE(doc->tree.nodes[xIdx].kind, NodeKind::Pointer64);
        QCOMPARE(doc->tree.nodes[xIdx].refId, bravoId);
        QVERIFY(doc->tree.nodes[xIdx].collapsed);

        // Single undo reverses the entire macro
        doc->undoStack.undo();
        QApplication::processEvents();
        xIdx = doc->tree.indexOfId(xNodeId);
        QVERIFY(xIdx >= 0);
        QCOMPARE(doc->tree.nodes[xIdx].kind, NodeKind::Int32);
        QCOMPARE(doc->tree.nodes[xIdx].refId, uint64_t(0));

        delete ctrl;
        delete splitter;
        delete doc;
    }

    // ── FieldType popup: selecting a primitive type still works ──

    void testFieldTypePrimitiveStillWorks() {
#ifdef SKIP_GUI_INTEGRATION
        QSKIP("GUI integration test disabled");
#endif
        auto* doc = new RcxDocument();
        buildTwoRootTree(doc->tree);
        doc->provider = std::make_unique<BufferProvider>(makeBuffer());

        auto* splitter = new QSplitter();
        auto* ctrl = new RcxController(doc, nullptr);
        ctrl->addSplitEditor(splitter);

        splitter->resize(800, 600);
        splitter->show();
        QVERIFY(QTest::qWaitForWindowExposed(splitter));
        ctrl->refresh();
        QApplication::processEvents();

        // Find the "x" field (Int32)
        int xIdx = -1;
        for (int i = 0; i < doc->tree.nodes.size(); i++) {
            if (doc->tree.nodes[i].name == "x") { xIdx = i; break; }
        }
        QVERIFY(xIdx >= 0);
        QCOMPARE(doc->tree.nodes[xIdx].kind, NodeKind::Int32);

        // Change to Float via changeNodeKind (same path as primitive TypeEntry)
        ctrl->changeNodeKind(xIdx, NodeKind::Float);
        QApplication::processEvents();
        QCOMPARE(doc->tree.nodes[xIdx].kind, NodeKind::Float);

        // Undo
        doc->undoStack.undo();
        QApplication::processEvents();
        QCOMPARE(doc->tree.nodes[xIdx].kind, NodeKind::Int32);

        delete ctrl;
        delete splitter;
        delete doc;
    }

    // ── Section headers in filtered list ──

    void testSectionHeadersPresent() {
        TypeSelectorPopup popup;

        // Build entries with both primitives and composites
        QVector<TypeEntry> types;
        TypeEntry prim;
        prim.entryKind = TypeEntry::Primitive;
        prim.primitiveKind = NodeKind::Int32;
        prim.displayName = "int32_t";
        types.append(prim);

        TypeEntry comp;
        comp.entryKind = TypeEntry::Composite;
        comp.structId = 42;
        comp.displayName = "MyStruct";
        comp.classKeyword = "struct";
        types.append(comp);

        popup.setTypes(types);
        // After setTypes, the internal filtered list should have section headers
        // We can verify this indirectly by checking the model row count
        // (should be > 2 due to section headers)
        auto* listView = popup.findChild<QListView*>();
        QVERIFY(listView);
        QVERIFY(listView->model()->rowCount() > 2);
    }

    // Regression: each section header's kindGroup (which drives its pip color)
    // must be a real group KEY, not the first word of its human label. The bug
    // derived it via `title.left(indexOf(' '))`, yielding "Pointer"/"String"/
    // "Type" — none of which kindGroupColor() knows (it wants Ptr/Str/Ctr), so
    // those pips fell back to a neutral default. A composite buckets to "Ctr"
    // (label "Type"), so it exercises the exact failure. Deterministic.
    void testSectionKindGroupKeysAreValid() {
        TypeSelectorPopup popup;
        TypeEntry comp; comp.entryKind = TypeEntry::Composite;
        comp.structId = 7; comp.displayName = "MyStruct"; comp.classKeyword = "struct";
        TypeEntry prim; prim.entryKind = TypeEntry::Primitive;
        prim.primitiveKind = NodeKind::Int32; prim.displayName = "int32_t";
        popup.setTypes({comp, prim});

        static const QSet<QString> validKeys = {
            QStringLiteral("Hex"), QStringLiteral("Int"), QStringLiteral("Float"),
            QStringLiteral("Ptr"), QStringLiteral("Vec"), QStringLiteral("Str"),
            QStringLiteral("Ctr"), QStringLiteral("Common")};
        int sections = 0;
        for (const auto& e : popup.filteredTypes()) {
            if (e.entryKind != TypeEntry::Section) continue;
            ++sections;
            QVERIFY2(validKeys.contains(e.kindGroup),
                qPrintable(QString("section '%1' has bogus pip key '%2' "
                    "(expected Hex/Int/Float/Ptr/Vec/Str/Ctr/Common)")
                    .arg(e.displayName, e.kindGroup)));
        }
        QVERIFY2(sections > 0, "no section headers produced");
    }
    // Direct unit test of the shared helper that ALL the font-size derivations
    // (scanner headers, typechooser chips, hex/profiler/sourcechooser popups)
    // now route through. Guards the foundation in isolation, independent of any
    // one consumer's widget plumbing.
    void testResolvedPointSizeHandlesPixelFonts() {
        // Point-sized font → returned unchanged.
        QFont pt(QStringLiteral("Arial"), 13);
        QCOMPARE(rcx::resolvedPointSize(pt), 13);
        // Pixel-sized font → pointSize() is -1, but resolvedPointSize must give a
        // concrete positive size (the whole point — -1 is what collapsed derived
        // sizes to the floor).
        QFont px(QStringLiteral("Arial"));
        px.setPixelSize(32);
        QCOMPARE(px.pointSize(), -1);
        const int r = rcx::resolvedPointSize(px);
        QVERIFY2(r > 0,
            qPrintable(QString("resolvedPointSize returned %1 for a pixel font "
                "(must be a concrete >0 size, never -1)").arg(r)));
    }

    // Regression: derived (chip / title / small) fonts must not collapse to the
    // 7pt floor when the base font is specified in PIXELS. QFont::pointSize()
    // returns -1 for a pixel-sized font, so `qMax(7, pointSize()*k)` snapped
    // every derived font to 7pt; resolvedPointSize() resolves a real size via
    // QFontInfo first. A 40px base stays well above 10pt at any DPI, so a
    // correctly-derived chip font (≈0.75×) clears 7. Deterministic.
    void testDerivedFontsDontSnapToFloorWithPixelFont() {
        TypeSelectorPopup popup;
        QFont pixelFont(QStringLiteral("Arial"));
        pixelFont.setPixelSize(40);                 // pointSize() == -1
        QCOMPARE(pixelFont.pointSize(), -1);        // sanity: it really is pixel-sized
        popup.setFont(pixelFont);

        QList<rcx::CategoryChip*> chips;
        for (auto* w : popup.findChildren<QWidget*>())
            if (auto* c = dynamic_cast<rcx::CategoryChip*>(w))
                chips.append(c);
        QVERIFY2(!chips.isEmpty(), "no category chips found");
        for (auto* chip : chips) {
            // QFontInfo resolves a concrete point size whether the font is in
            // point- or pixel-mode (raw font().pointSize() reports -1 in pixel
            // mode, which is why we don't use it here). Bug → ~7pt; fix → the
            // 0.75×-of-resolved size, comfortably above 7 for a 40px base.
            const int ps = QFontInfo(chip->font()).pointSize();
            QVERIFY2(ps > 7,
                qPrintable(QString("chip font resolved to %1pt (expected >7 for a "
                    "40px base font — resolvedPointSize regression)").arg(ps)));
        }
    }
    // ── FieldType popup: primitive with [n] creates an array ──

    void testFieldTypePrimitiveArrayCreation() {
#ifdef SKIP_GUI_INTEGRATION
        QSKIP("GUI integration test disabled");
#endif
        auto* doc = new RcxDocument();
        buildTwoRootTree(doc->tree);
        doc->provider = std::make_unique<BufferProvider>(makeBuffer());

        auto* splitter = new QSplitter();
        auto* ctrl = new RcxController(doc, nullptr);
        ctrl->addSplitEditor(splitter);

        splitter->resize(800, 600);
        splitter->show();
        QVERIFY(QTest::qWaitForWindowExposed(splitter));
        ctrl->refresh();
        QApplication::processEvents();

        // Find the "x" field (Int32)
        int xIdx = -1;
        for (int i = 0; i < doc->tree.nodes.size(); i++) {
            if (doc->tree.nodes[i].name == "x") { xIdx = i; break; }
        }
        QVERIFY(xIdx >= 0);
        QCOMPARE(doc->tree.nodes[xIdx].kind, NodeKind::Int32);
        uint64_t xNodeId = doc->tree.nodes[xIdx].id;

        // Simulate the primitive-array path of applyTypePopupResult:
        // beginMacro → changeNodeKind(Array) → ChangeArrayMeta → endMacro
        doc->undoStack.beginMacro(QStringLiteral("Change to primitive array"));
        ctrl->changeNodeKind(xIdx, NodeKind::Array);
        xIdx = doc->tree.indexOfId(xNodeId);
        QVERIFY(xIdx >= 0);
        doc->undoStack.push(new RcxCommand(ctrl,
            cmd::ChangeArrayMeta{xNodeId, doc->tree.nodes[xIdx].elementKind,
                                 NodeKind::Int32,
                                 doc->tree.nodes[xIdx].arrayLen, 4}));
        doc->undoStack.endMacro();
        QApplication::processEvents();

        // Node should now be an Array
        xIdx = doc->tree.indexOfId(xNodeId);
        QVERIFY(xIdx >= 0);
        QCOMPARE(doc->tree.nodes[xIdx].kind, NodeKind::Array);
        QCOMPARE(doc->tree.nodes[xIdx].elementKind, NodeKind::Int32);
        QCOMPARE(doc->tree.nodes[xIdx].arrayLen, 4);

        // Single undo reverses the entire macro
        doc->undoStack.undo();
        QApplication::processEvents();
        xIdx = doc->tree.indexOfId(xNodeId);
        QVERIFY(xIdx >= 0);
        QCOMPARE(doc->tree.nodes[xIdx].kind, NodeKind::Int32);

        delete ctrl;
        delete splitter;
        delete doc;
    }
    // ── Test: SVG icon and gutter scale with font size ──

    void testDelegateIconScalesWithFont() {
        // Create a popup and set two different font sizes.
        // The delegate sizeHint row height should scale with font.
        TypeSelectorPopup popup;

        TypeEntry prim;
        prim.entryKind = TypeEntry::Primitive;
        prim.primitiveKind = NodeKind::Int32;
        prim.displayName = QStringLiteral("int32_t");

        TypeEntry comp;
        comp.entryKind = TypeEntry::Composite;
        comp.structId = 100;
        comp.displayName = QStringLiteral("TestStruct");
        comp.classKeyword = QStringLiteral("struct");

        // Small font
        QFont small(QStringLiteral("Consolas"), 9);
        popup.setFont(small);
        popup.setTypes({prim, comp});
        popup.popup(QPoint(-9999, -9999));  // offscreen
        QApplication::processEvents();

        auto* listView = popup.findChild<QListView*>();
        QVERIFY(listView);
        auto* delegate = listView->itemDelegate();
        QVERIFY(delegate);

        // Find first non-section row for consistent measurement
        int dataRow = -1;
        for (int i = 0; i < listView->model()->rowCount(); i++) {
            QSize h = delegate->sizeHint(QStyleOptionViewItem(), listView->model()->index(i, 0));
            // Non-section rows are taller (font.height + 8 vs + 2)
            if (h.height() > QFontMetrics(small).height() + 4) { dataRow = i; break; }
        }
        QVERIFY2(dataRow >= 0, "Should find a non-section row");

        QSize smallHint = delegate->sizeHint(QStyleOptionViewItem(), listView->model()->index(dataRow, 0));
        popup.hide();

        // Large font (simulates zoomed editor)
        QFont large(QStringLiteral("Consolas"), 18);
        popup.setFont(large);
        popup.setTypes({prim, comp});
        popup.popup(QPoint(-9999, -9999));
        QApplication::processEvents();

        QSize largeHint = delegate->sizeHint(QStyleOptionViewItem(), listView->model()->index(dataRow, 0));
        popup.hide();

        // Large font should produce taller rows than small font
        QVERIFY2(largeHint.height() > smallHint.height(),
                 qPrintable(QString("Large hint %1 should be > small hint %2")
                     .arg(largeHint.height()).arg(smallHint.height())));

        // The ratio should roughly match the font size ratio (18/9 = 2x)
        double ratio = double(largeHint.height()) / double(smallHint.height());
        QVERIFY2(ratio > 1.4, qPrintable(QString("Row height ratio %1 should be > 1.4").arg(ratio)));
    }

    void testPopupWidthScalesWithFont() {
        TypeSelectorPopup popup;

        // Use a very long name so even font-9 exceeds the minimum popup width
        TypeEntry comp;
        comp.entryKind = TypeEntry::Composite;
        comp.structId = 100;
        comp.displayName = QStringLiteral("MyExtremelyLongStructNameThatExceedsMinWidth");
        comp.classKeyword = QStringLiteral("struct");
        popup.setTypes({comp});

        // Small font
        QFont small(QStringLiteral("Consolas"), 9);
        popup.setFont(small);
        popup.popup(QPoint(-9999, -9999));
        QApplication::processEvents();
        int smallW = popup.width();
        popup.hide();

        // Large font
        QFont large(QStringLiteral("Consolas"), 18);
        popup.setFont(large);
        popup.setTypes({comp});
        popup.popup(QPoint(-9999, -9999));
        QApplication::processEvents();
        int largeW = popup.width();
        popup.hide();

        // Popup with larger font should be wider
        QVERIFY2(largeW > smallW,
                 qPrintable(QString("Large popup width %1 should be > small %2")
                     .arg(largeW).arg(smallW)));
    }
    // ── Test: popup updates colors when theme changes ──

    // ── The popup family rule, in pixels ──

    void testTypeSelectorFollowsThePopupFamilyRule() {
        // ONE surface (theme.background — the body was backgroundAlt with
        // the filter strip and the detail pane as a second surface on it)
        // inside ONE device-exact theme.border frame, the filter the same
        // surface with a single seam row, under tw and vs at dpr 1.0 and
        // 1.25. applyTheme's theme drives the chrome AND the rows (the
        // delegate paints from popup.theme()), so tw here means tw
        // everywhere regardless of what ThemeManager holds.
        for (const QString& name : {QStringLiteral("tw"), QStringLiteral("vs")}) {
            const Theme t = shippedTheme(name);
            QVERIFY2(t.background.isValid(), qPrintable(name + QStringLiteral(".json not found next to the exe")));
            for (const qreal dpr : { 1.0, 1.25 }) {
                TypeSelectorPopup popup;
                popup.applyTheme(t);
                popup.setFont(QFont(QStringLiteral("Consolas"), 10));
                popup.setMode(TypePopupMode::FieldType);
                QVector<TypeEntry> types;
                for (NodeKind k : {NodeKind::Hex64, NodeKind::Int32, NodeKind::Float, NodeKind::Pointer64}) {
                    TypeEntry e;
                    e.entryKind = TypeEntry::Primitive;
                    e.primitiveKind = k;
                    e.displayName = QString::fromLatin1(kindMeta(k)->typeName);
                    e.sizeBytes = sizeForKind(k);
                    types.append(e);
                }
                TypeEntry c;
                c.entryKind = TypeEntry::Composite;
                c.structId = 7;
                c.displayName = QStringLiteral("PlayerEntity");
                c.classKeyword = QStringLiteral("struct");
                c.kindGroup = QStringLiteral("Ctr");
                types.append(c);
                popup.setTypes(types, nullptr);
                popup.popup(QPoint(100, 100));
                QTest::qWait(30);
                QApplication::processEvents();
                const QImage img = renderAt(popup, dpr);
                // Every probe, then one verdict — and the image on disk when
                // any of them fails, so a red run can be looked at.
                QStringList failed;
                QString why;
                if (!frameIsOneDeviceRow(img, popup.rect(), t, dpr, &why)) failed << why;
                // The current row's t.selected fill runs to the frame by
                // design (it is inside the 1-px inset); nothing else may.
                if (!edgeStripIsGround(img, popup.rect(), t, dpr, 4, true, &why, t.selected)) failed << why;
                if (!edgeStripIsGround(img, popup.rect(), t, dpr, 4, false, &why, t.selected)) failed << why;
                if (!failed.isEmpty())
                    img.save(QDir::tempPath() + QStringLiteral("/typeselector_family_%1_%2.png").arg(name).arg(dpr));
                QVERIFY2(failed.isEmpty(), qPrintable(name + QStringLiteral(": ") + failed.join(QStringLiteral(" | "))));
                const QRect all(0, 0, img.width(), img.height());
                QVERIFY2(countColour(img, all, t.background) > all.width() * all.height() / 2,
                         "theme.background is not the dominant surface");
                auto* field = popup.findChild<QLineEdit*>();
                QVERIFY(field);
                const QRect fieldRect(field->mapTo(&popup, QPoint(0, 0)), field->size());
                QVERIFY2(fieldHasOneSeam(img, fieldRect, t, dpr, &why), qPrintable(name + QStringLiteral(": ") + why));
                // The scrollbar wears the popup's local rule, never the
                // app-wide 8-px rod: 6 px, track = the surface.
                QCOMPARE(popup.findChild<QListView*>()->verticalScrollBar()->styleSheet(),
                         popupScrollBarQss(t, t.background));
                popup.hide();
            }
        }
    }

    void testEnumPickerFollowsThePopupFamilyRule() {
        // The enum picker paints from ThemeManager's current theme, so the
        // probe runs under whatever that is: one surface, the device-exact
        // frame (was a 1-logical QPen in borderFocused), the filter's seam,
        // the footer on the same surface (was a backgroundAlt band under a
        // 1-logical border-top). The rows' accent stripe ends at the house
        // gutter: at the row's x=0 it was a coloured edge welded to the
        // left frame down the whole list, so the LEFT strip is probed too,
        // and the stripe is looked for further in on the current row.
        const Theme& t = ThemeManager::instance().current();
        const QColor accent(0xc5, 0x86, 0xc0);
        QVector<EnumPickerPopup::Member> members;
        for (int i = 0; i < 12; ++i)
            members.append({QStringLiteral("MEMBER_%1").arg(i), int64_t(i)});
        for (const qreal dpr : { 1.0, 1.25 }) {
            EnumPickerPopup popup;
            popup.show(QStringLiteral("Kind"), members, 3, accent, QPoint(100, 100));
            QTest::qWait(30);
            QApplication::processEvents();
            const QImage img = renderAt(popup, dpr);
            QString why;
            QVERIFY2(frameIsOneDeviceRow(img, popup.rect(), t, dpr, &why), qPrintable(why));
            // The current value's row is selected (t.selected across the row,
            // to the frame) by design; everything else in the strip is ground.
            QVERIFY2(edgeStripIsGround(img, popup.rect(), t, dpr, 4, false, &why, t.selected), qPrintable(why));
            QVERIFY2(edgeStripIsGround(img, popup.rect(), t, dpr, 4, true, &why, t.selected), qPrintable(why));
            {
                auto* view = popup.findChild<QListView*>();
                QVERIFY(view);
                QVERIFY(view->currentIndex().isValid());
                const QRect cur = view->visualRect(view->currentIndex())
                                      .translated(view->viewport()->mapTo(&popup, QPoint(0, 0)));
                const Edges e = edgesOf(popup.rect(), dpr);
                const Edges c = edgesOf(cur, dpr);
                // The stripe: full accent on the current row, past the clean
                // strip and before the gutter's end (kGutter logical px).
                const QRect stripe(e.left + 5, c.top + 1, qCeil(kGutter * dpr) - 4, c.bottom - c.top - 1);
                QVERIFY2(countColour(img, stripe, accent) > 0, "the current row's stripe should sit before the gutter");
            }
            auto* field = popup.findChild<QLineEdit*>();
            QVERIFY(field);
            QVERIFY2(!field->isHidden(), "twelve members should show the filter");
            const QRect fieldRect(field->mapTo(&popup, QPoint(0, 0)), field->size());
            QVERIFY2(fieldHasOneSeam(img, fieldRect, t, dpr, &why), qPrintable(why));
            // The footer: the surface under one seam row on its top edge.
            QLabel* footer = nullptr;
            for (QLabel* l : popup.findChildren<QLabel*>())
                if (l->text().contains(QStringLiteral("navigate"))) footer = l;
            QVERIFY(footer);
            const QRect footerRect(footer->mapTo(&popup, QPoint(0, 0)), footer->size());
            const Edges f = edgesOf(footerRect, dpr);
            const Edges e = edgesOf(popup.rect(), dpr);
            const int w = e.right - e.left - 1;
            QCOMPARE(countColour(img, QRect(e.left + 1, f.top, w, 1), containerBorderColor(t)), w);
            if (t.backgroundAlt != t.background)
                QCOMPARE(countColour(img, QRect(f.left, f.top + 1, f.right - f.left + 1, f.bottom - f.top),
                                     t.backgroundAlt, 0), 0);
            popup.hide();
        }
    }

    void testHexToolbarFollowsThePopupFamilyRule() {
        // The hex toolbar: one surface (theme.background — it was the
        // tooltip's backgroundAlt) inside the device-exact frame (four
        // 1-logical strips before). Its size buttons start 4 px in, so the
        // strips probed are 3 device columns.
        const Theme& t = ThemeManager::instance().current();
        for (const qreal dpr : { 1.0, 1.25 }) {
            HexToolbarPopup popup;
            popup.setFont(QFont(QStringLiteral("Consolas"), 10));
            HexPopupContext ctx;
            ctx.currentKind = NodeKind::Hex64;
            ctx.data = QByteArray(8, 'A');
            popup.setContext(ctx);
            popup.popup(QPoint(100, 100));
            QTest::qWait(30);
            QApplication::processEvents();
            const QImage img = renderAt(popup, dpr);
            QString why;
            QVERIFY2(frameIsOneDeviceRow(img, popup.rect(), t, dpr, &why), qPrintable(why));
            QVERIFY2(edgeStripIsGround(img, popup.rect(), t, dpr, 3, true, &why), qPrintable(why));
            QVERIFY2(edgeStripIsGround(img, popup.rect(), t, dpr, 3, false, &why), qPrintable(why));
            const QRect all(0, 0, img.width(), img.height());
            QVERIFY2(countColour(img, all, t.background) > all.width() * all.height() / 2,
                     "theme.background is not the dominant surface");
            popup.hide();
        }
    }

    void testSeamsReachTheFrameAtEveryWidth() {
        // At 125 % the layout's 1-logical inset is 1.25 device px, so at the
        // widths where W * dpr has a .5 or .75 fraction the children ended
        // one device column short of the right frame, and every seam they
        // painted — the field's, the section rules, the track's share —
        // stopped with a notch of ground before it. The popup now paints
        // every seam itself across its whole width, the frame last. Four
        // consecutive widths cover every phase; the probe walks each seam
        // from the column after the left frame to the column before the
        // right one, under the scroll track included.
        const qreal dpr = 1.25;
        for (const QString& name : {QStringLiteral("tw"), QStringLiteral("vs")}) {
            const Theme t = shippedTheme(name);
            QVERIFY(t.background.isValid());
            for (int w = 360; w <= 363; ++w) {
                TypeSelectorPopup popup;
                popup.applyTheme(t);
                popup.setFont(QFont(QStringLiteral("Consolas"), 10));
                popup.setMode(TypePopupMode::FieldType);
                popup.setTypes(fullCatalogue(), nullptr);
                popup.popup(QPoint(100, 100));
                popup.setFixedSize(w, popup.height());
                // The list lays its rows out in batches on a timer
                // (QListView::Batched): a visualRect is invalid until it ran.
                QTest::qWait(30);
                QApplication::processEvents();
                popup.layout()->activate();
                auto* list = popup.findChild<QListView*>();
                QVERIFY(list);
                QVERIFY2(list->verticalScrollBar()->maximum() > 0, "the list should scroll");
                QImage img = renderAt(popup, dpr);
                const Edges e = edgesOf(popup.rect(), dpr);
                const QString tag = QStringLiteral("%1 w=%2: ").arg(name).arg(w);
                QString why;
                QVERIFY2(frameIsOneDeviceRow(img, popup.rect(), t, dpr, &why), qPrintable(tag + why));
                // The field's seam, in whichever colour the field is in.
                auto* field = popup.findChild<QLineEdit*>();
                QVERIFY(field);
                const Edges f = edgesOf(QRect(field->mapTo(&popup, QPoint(0, 0)), field->size()), dpr);
                QVERIFY2(rowIsSeam(img, e, f.bottom, containerBorderColor(t), t.borderFocused, &why),
                         qPrintable(tag + QStringLiteral("field seam: ") + why));
                // Every section rule on screen — at the top, then scrolled
                // to the bottom (the popup's mirror of the rules has to
                // follow the scroll).
                const QPoint vp = list->viewport()->mapTo(&popup, QPoint(0, 0));
                const auto& rows = popup.filteredTypes();
                auto probeRules = [&](const char* where) {
                    int probed = 0;
                    for (int r = 1; r < rows.size(); ++r) {
                        if (rows[r].entryKind != TypeEntry::Section) continue;
                        const QRect vr = list->visualRect(list->model()->index(r, 0));
                        if (!vr.isValid() || vr.top() < 0 || vr.top() >= list->viewport()->height()) continue;
                        const Edges h = edgesOf(vr.translated(vp), dpr);
                        if (!rowIsSeam(img, e, h.top, containerBorderColor(t), QColor(), &why)) {
                            why = tag + QStringLiteral("section rule %1 (%2): ").arg(r).arg(QLatin1String(where)) + why;
                            return -1;
                        }
                        ++probed;
                    }
                    return probed;
                };
                int probed = probeRules("top");
                QVERIFY2(probed >= 1, qPrintable(probed < 0 ? why : tag + QStringLiteral("no section rule on screen at the top")));
                list->scrollToBottom();
                QTest::qWait(30);
                QApplication::processEvents();
                img = renderAt(popup, dpr);
                probed = probeRules("bottom");
                QVERIFY2(probed >= 1, qPrintable(probed < 0 ? why : tag + QStringLiteral("no section rule on screen at the bottom")));
                popup.hide();
            }
        }
    }

    void testSelectedSectionHeaderKeepsItsSeam() {
        // A selected (or hovered) collapsible header paints t.selected
        // across its row; the rule above it used to be painted first and
        // covered, while the scrollbar still carried its share across the
        // track — a short grey dash at the row's right end. The rule is
        // painted after the fill now: the header's top device row is the
        // seam from frame to frame, with no t.selected on it, and the row
        // below it IS the selection.
        const Theme t = shippedTheme(QStringLiteral("tw"));
        QVERIFY(t.background.isValid());
        for (const qreal dpr : { 1.0, 1.25 }) {
            TypeSelectorPopup popup;
            popup.applyTheme(t);
            popup.setFont(QFont(QStringLiteral("Consolas"), 10));
            popup.setMode(TypePopupMode::FieldType);
            popup.setTypes(fullCatalogue(), nullptr);
            popup.popup(QPoint(100, 100));
            QTest::qWait(30);
            QApplication::processEvents();
            auto* list = popup.findChild<QListView*>();
            QVERIFY(list);
            QVERIFY2(list->verticalScrollBar()->maximum() > 0, "the list should scroll");
            const auto& rows = popup.filteredTypes();
            int hdr = -1;
            for (int r = 1; r < rows.size() && hdr < 0; ++r)
                if (rows[r].entryKind == TypeEntry::Section && rows[r].sectionCollapsible) hdr = r;
            QVERIFY2(hdr > 0, "a collapsible section header below the first row");
            list->setCurrentIndex(list->model()->index(hdr, 0));
            QApplication::processEvents();
            const QImage img = renderAt(popup, dpr);
            const Edges e = edgesOf(popup.rect(), dpr);
            const QRect vr = list->visualRect(list->model()->index(hdr, 0))
                                 .translated(list->viewport()->mapTo(&popup, QPoint(0, 0)));
            const Edges h = edgesOf(vr, dpr);
            QString why;
            QVERIFY2(rowIsSeam(img, e, h.top, containerBorderColor(t), QColor(), &why), qPrintable(why));
            const int inner = e.right - e.left - 1;
            QCOMPARE(countColour(img, QRect(e.left + 1, h.top, inner, 1), t.selected), 0);
            const QRect below(e.left + 1, h.top + 1, inner, 1);
            QVERIFY2(countColour(img, below, t.selected) > inner / 2,
                     "the header's row under the seam should be the selection");
            popup.hide();
        }
    }

    void testCurrentRowStripeEndsAtTheGutter() {
        // The current row's kind-colour stripe used to start at the row's
        // x=0 — device columns 1..3, welded to the frame column. It ends at
        // the house gutter now: the four columns inside the left frame are
        // ground (or the row's t.selected), and the stripe sits past them.
        const Theme t = shippedTheme(QStringLiteral("tw"));
        QVERIFY(t.background.isValid());
        for (const qreal dpr : { 1.0, 1.25 }) {
            TypeSelectorPopup popup;
            popup.applyTheme(t);
            popup.setFont(QFont(QStringLiteral("Consolas"), 10));
            popup.setMode(TypePopupMode::FieldType);
            popup.setTypes(fullCatalogue(), nullptr);
            popup.popup(QPoint(100, 100));
            QTest::qWait(30);
            QApplication::processEvents();
            auto* list = popup.findChild<QListView*>();
            QVERIFY(list);
            const auto& rows = popup.filteredTypes();
            int item = -1;
            for (int r = 0; r < rows.size() && item < 0; ++r)
                if (rows[r].entryKind == TypeEntry::Primitive && rows[r].enabled) item = r;
            QVERIFY(item >= 0);
            list->setCurrentIndex(list->model()->index(item, 0));
            QApplication::processEvents();
            const QImage img = renderAt(popup, dpr);
            QString why;
            QVERIFY2(edgeStripIsGround(img, popup.rect(), t, dpr, 4, true, &why, t.selected), qPrintable(why));
            const QColor stripeCol = kindGroupColor(rows[item].kindGroup);
            const QRect vr = list->visualRect(list->model()->index(item, 0))
                                 .translated(list->viewport()->mapTo(&popup, QPoint(0, 0)));
            const Edges e = edgesOf(popup.rect(), dpr);
            const Edges c = edgesOf(vr, dpr);
            const QRect stripe(e.left + 5, c.top + 1, qCeil(kGutter * dpr) - 4, c.bottom - c.top - 1);
            QVERIFY2(countColour(img, stripe, stripeCol) > 0, "the current row's stripe should sit before the gutter");
            popup.hide();
        }
    }

    void testBannerElidesInsteadOfClipping() {
        // The action row's banner yields to the buttons; a QLabel never
        // elides, so at the default width a long name was cut mid-glyph
        // ("PlayerEntit"). The banner elides its name now — an ellipsis, a
        // rich text no wider than the label, the label ending before the
        // OK button and the button inside the frame — and shows the whole
        // name again once the popup is wide enough.
        const Theme t = shippedTheme(QStringLiteral("tw"));
        QVERIFY(t.background.isValid());
        TypeSelectorPopup popup;
        popup.applyTheme(t);
        popup.setFont(QFont(QStringLiteral("Consolas"), 10));
        popup.setMode(TypePopupMode::FieldType);
        popup.setTypes(fullCatalogue(), nullptr);
        popup.popup(QPoint(100, 100));
        QTest::qWait(30);
        QApplication::processEvents();
        auto* list = popup.findChild<QListView*>();
        QVERIFY(list);
        const QString longName = QStringLiteral("PlayerEntityControllerStateMachineComponent");
        const auto& rows = popup.filteredTypes();
        int row = -1;
        for (int r = 0; r < rows.size() && row < 0; ++r)
            if (rows[r].displayName == longName) row = r;
        QVERIFY2(row >= 0, "the long struct should be listed");
        list->setCurrentIndex(list->model()->index(row, 0));
        QApplication::processEvents();
        QLabel* banner = nullptr;
        for (QLabel* l : popup.findChildren<QLabel*>())
            if (l->accessibleName() == QStringLiteral("Type preview")) banner = l;
        QVERIFY(banner);
        QAbstractButton* ok = nullptr;
        for (QAbstractButton* b : popup.findChildren<QAbstractButton*>())
            if (b->accessibleName() == QStringLiteral("OK")) ok = b;
        QVERIFY(ok);
        auto shown = [&]() {
            QTextDocument doc;
            doc.setHtml(banner->text());
            return doc.toPlainText();
        };
        const QString elided = shown();
        // The buttons leave the banner ~11 glyphs: the size tail goes and
        // the name keeps at least eight of them before the ellipsis (the
        // rule was "Pla… → 32B" otherwise, which names nothing).
        QVERIFY2(elided.startsWith(QStringLiteral("PlayerEn")), qPrintable(elided.toUtf8().toPercentEncoding()));
        QVERIFY2(elided.contains(QChar(0x2026)), qPrintable(QStringLiteral("not elided: ") + elided.toUtf8().toPercentEncoding()));
        QVERIFY2(!elided.contains(longName), "the full name cannot fit beside the buttons here");
        QVERIFY2(!elided.contains(QStringLiteral("32B")), "the size tail should go before the name shrinks that far");
        QVERIFY2(banner->sizeHint().width() > banner->width(), "the banner should be asking for its full text");
        QVERIFY(QFontMetrics(banner->font()).horizontalAdvance(elided) <= banner->width());
        QVERIFY(banner->geometry().right() < ok->geometry().left());
        QVERIFY(ok->geometry().right() < popup.width() - 1);
        // Wide enough: the whole name, no ellipsis. The render flushes the
        // pending resize the hidden test desktop's closed popup would
        // otherwise hold back from the label (it re-elides on resize).
        popup.setFixedSize(popup.width() + 400, popup.height());
        QApplication::processEvents();
        popup.layout()->activate();
        renderAt(popup, 1.0);
        QApplication::processEvents();
        const QString full = shown();
        QVERIFY2(full.contains(longName) && !full.contains(QChar(0x2026)) && full.contains(QStringLiteral("32B")),
                 qPrintable(full.toUtf8().toPercentEncoding()));
        popup.hide();
    }

    void testHexToolbarOutlinesAndRingAreDeviceExact() {
        // Every button outline is the same device-exact frame as the popup's
        // (four 1-logical strips read a different weight on each edge at
        // 125 %); the keyboard ring is two device rows of borderFocused on
        // the focused button's own edges (it was a 2-logical QPen in the
        // accent, and painted only when pinned); the pin is the tinted
        // house glyph, not the raw SVG's #C5C5C5 ink; the accent is spent on
        // the current size button alone.
        const Theme& t = ThemeManager::instance().current();
        const QColor bakedInk(0xC5, 0xC5, 0xC5);
        for (const qreal dpr : { 1.0, 1.25 }) {
            HexToolbarPopup popup;
            popup.setFont(QFont(QStringLiteral("Consolas"), 10));
            HexPopupContext ctx;
            ctx.currentKind = NodeKind::Hex64;
            ctx.data = QByteArray(8, 'A');
            ctx.hasPtr = true;
            ctx.ptrSymbol = QStringLiteral("g_World");
            popup.setContext(ctx);
            popup.popup(QPoint(100, 100));
            QTest::qWait(30);
            QApplication::processEvents();
            QImage img = renderAt(popup, dpr);
            const QVector<QRect> hits = popup.hitRectsForTest();
            QVERIFY2(hits.size() >= 6, "five size buttons and the pin");
            // One device row / column of `c` on each edge of `r`, none just
            // inside; names the first edge that is not.
            auto outlineIsOneDeviceRow = [&](const QRect& r, const QColor& c, QString* why) {
                const Edges b = edgesOf(r, dpr);
                const int w = b.right - b.left + 1, h = b.bottom - b.top + 1;
                struct Probe { QRect r; int want; const char* what; };
                const Probe probes[] = {
                    { QRect(b.left, b.top, w, 1), w, "top" },
                    { QRect(b.left, b.bottom, w, 1), w, "bottom" },
                    { QRect(b.left, b.top, 1, h), h, "left" },
                    { QRect(b.right, b.top, 1, h), h, "right" },
                    { QRect(b.left + 1, b.top + 1, w - 2, 1), 0, "row inside the top" },
                    { QRect(b.left + 1, b.bottom - 1, w - 2, 1), 0, "row inside the bottom" },
                    { QRect(b.left + 1, b.top + 1, 1, h - 2), 0, "column inside the left" },
                    { QRect(b.right - 1, b.top + 1, 1, h - 2), 0, "column inside the right" },
                };
                for (const Probe& pr : probes) {
                    const int got = countColour(img, pr.r, c);
                    if (got == pr.want) continue;
                    *why = QStringLiteral("%1: %2 of %3 outline px at dpr %4 (button %5,%6 %7x%8)")
                               .arg(QLatin1String(pr.what)).arg(got).arg(pr.want).arg(dpr)
                               .arg(r.x()).arg(r.y()).arg(r.width()).arg(r.height());
                    return false;
                }
                return true;
            };
            // Sizes 8, 16, 32, [64 = current], 128.
            for (int i = 0; i < 5; ++i) {
                QString why;
                QVERIFY2(outlineIsOneDeviceRow(hits[i], i == 3 ? t.indHoverSpan : t.border, &why), qPrintable(why));
            }
            // The accent, in the size row, lives inside the current button.
            {
                const Edges b = edgesOf(hits[3], dpr);
                const QRect sizeRow(0, b.top, img.width(), b.bottom - b.top + 1);
                const QRect cur(b.left, b.top, b.right - b.left + 1, b.bottom - b.top + 1);
                QCOMPARE(countColour(img, sizeRow, t.indHoverSpan), countColour(img, cur, t.indHoverSpan));
            }
            // The pin: tinted ink, none of the SVG's baked grey. This target
            // links no resources.qrc (every icon in it is blank), so the
            // glyph is probed only where the SVG exists — the
            // typeselector_render harness's `hex` mode links the qrc and
            // shows it for real.
            if (QFile::exists(QStringLiteral(":/vsicons/pin.svg"))) {
                const Edges b = edgesOf(hits[5], dpr);
                const QRect pin(b.left, b.top, b.right - b.left + 1, b.bottom - b.top + 1);
                QVERIFY2(countColour(img, pin, t.textDim, 8) > 0, "the pin glyph should be textDim");
                if (!sameColour(t.textDim.rgb(), bakedInk, 12))
                    QCOMPARE(countColour(img, pin, bakedInk, 4), 0);
            }
            // The keyboard ring on the first size button, unpinned.
            QKeyEvent right(QEvent::KeyPress, Qt::Key_Right, Qt::NoModifier);
            QApplication::sendEvent(&popup, &right);
            QApplication::processEvents();
            QCOMPARE(popup.focusedHitForTest(), 0);
            img = renderAt(popup, dpr);
            {
                const Edges b = edgesOf(hits[0], dpr);
                const int w = b.right - b.left + 1, h = b.bottom - b.top + 1;
                QCOMPARE(countColour(img, QRect(b.left, b.top, w, 2), t.borderFocused), 2 * w);
                QCOMPARE(countColour(img, QRect(b.left, b.bottom - 1, w, 2), t.borderFocused), 2 * w);
                QCOMPARE(countColour(img, QRect(b.left, b.top, 2, h), t.borderFocused), 2 * h);
                QCOMPARE(countColour(img, QRect(b.right - 1, b.top, 2, h), t.borderFocused), 2 * h);
                QCOMPARE(countColour(img, QRect(b.left + 2, b.top + 2, w - 4, 1), t.borderFocused), 0);
                QCOMPARE(countColour(img, QRect(b.left + 2, b.top + 2, 1, h - 4), t.borderFocused), 0);
            }
            popup.hide();
        }
    }

    void testHexToolbarPinnedRowsSpendNoAccent() {
        // Pinned, the popup adds the suggestion row: its labels were the
        // accent, so a pointer suggestion showed two accent elements at
        // once beside the current size button. They are plain text now,
        // their outlines the device-exact frame, and the seam under the
        // size row reaches both frame columns at a width whose device
        // width has a .5 fraction (the notch case).
        const Theme& t = ThemeManager::instance().current();
        const qreal dpr = 1.25;
        HexToolbarPopup popup;
        popup.setFont(QFont(QStringLiteral("Consolas"), 10));
        HexPopupContext ctx;
        ctx.currentKind = NodeKind::Hex64;
        ctx.data = QByteArray(8, 'A');
        ctx.hasPtr = true;
        ctx.ptrSymbol = QStringLiteral("g_World");
        ctx.hasFloat = true;
        ctx.floatVal = 1.5f;
        popup.setContext(ctx);
        popup.popup(QPoint(100, 100));
        QTest::qWait(30);
        QApplication::processEvents();
        QImage img = renderAt(popup, dpr);
        QVector<QRect> hits = popup.hitRectsForTest();
        QVERIFY(hits.size() >= 6);
        // Pin it through its own hit rect (togglePin is private).
        QMouseEvent press(QEvent::MouseButtonPress, QPointF(hits[5].center()),
                          popup.mapToGlobal(hits[5].center()),
                          Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&popup, &press);
        QApplication::processEvents();
        QVERIFY(popup.isPinned());
        img = renderAt(popup, dpr);
        hits = popup.hitRectsForTest();
        QVERIFY2(hits.size() >= 8, "two suggestion buttons should follow the pin");
        const Edges e = edgesOf(popup.rect(), dpr);
        QString why;
        QVERIFY2(frameIsOneDeviceRow(img, popup.rect(), t, dpr, &why), qPrintable(why));
        for (int i = 6; i < 8; ++i) {
            const Edges b = edgesOf(hits[i], dpr);
            const int w = b.right - b.left + 1, h = b.bottom - b.top + 1;
            const QRect r(b.left, b.top, w, h);
            QCOMPARE(countColour(img, r, t.indHoverSpan), 0);
            QCOMPARE(countColour(img, QRect(b.left, b.top, w, 1), t.border), w);
            QCOMPARE(countColour(img, QRect(b.left, b.bottom, w, 1), t.border), w);
            QCOMPARE(countColour(img, QRect(b.left + 1, b.top + 1, w - 2, 1), t.border), 0);
            QVERIFY2(countColour(img, r, t.text) > 0, "a suggestion label should be plain text");
        }
        // The seam under the size row: the device row after the size
        // buttons' bottom edge plus the 2-px gap, frame to frame.
        {
            const int y = hits[0].bottom() + 1 + 2;
            const Edges s = edgesOf(QRect(0, y, popup.width(), 1), dpr);
            QVERIFY2(rowIsSeam(img, e, s.top, containerBorderColor(t), QColor(), &why), qPrintable(why));
        }
        popup.hide();
    }

    void testDetailPaneDividersAreTheSeamColour() {
        // The detail pane's dividers are the family's interior seam colour
        // (containerBorderColor), never the frame's theme.border. They are
        // 1-px CSS rules on <div>s, which Qt's rich-text engine does not
        // draw at all (borders render on table cells only), so in pixels
        // the probe can only be negative: no frame-coloured rule across
        // the pane. The pane has no toggle in the UI today (commented out),
        // so the test hook shows it.
        //
        // The AMBIENT theme, not a file one: that HTML is built by
        // updateDetailPane(), which reads ThemeManager::current() instead of
        // the theme applyTheme handed the popup (the hook above is its only
        // caller). In the app the two can never disagree — the ctor seeds
        // m_theme from the manager and themeChanged keeps it in step — but a
        // file theme applied by hand does: the divider rule would then be
        // checked against whatever theme happens to be loaded (VS2022 Dark
        // on this box) and fail wherever the saved theme is not tw. The
        // sibling chrome tests that DO apply a file theme are pixel checks
        // on the widget, which paints the theme it was handed.
        const Theme t = ThemeManager::instance().current();
        QVERIFY(t.background.isValid());
        TypeSelectorPopup popup;
        popup.applyTheme(t);
        popup.setFont(QFont(QStringLiteral("Consolas"), 10));
        popup.setMode(TypePopupMode::FieldType);
        popup.setTypes(fullCatalogue(), nullptr);
        popup.popup(QPoint(100, 100));
        QTest::qWait(30);
        QApplication::processEvents();
        auto* list = popup.findChild<QListView*>();
        QVERIFY(list);
        const auto& rows = popup.filteredTypes();
        int item = -1;
        for (int r = 0; r < rows.size() && item < 0; ++r)
            if (rows[r].entryKind == TypeEntry::Primitive && rows[r].enabled) item = r;
        QVERIFY(item >= 0);
        list->setCurrentIndex(list->model()->index(item, 0));
        popup.setShowDetailForTest(true);
        QApplication::processEvents();
        popup.layout()->activate();
        QApplication::processEvents();
        QLabel* content = nullptr;
        for (QLabel* l : popup.findChildren<QLabel*>())
            if (l->text().contains(QStringLiteral("border-bottom"))) content = l;
        QVERIFY2(content, "the detail pane's HTML should be up");
        const QString html = content->text();
        const QString seamRule = QStringLiteral("1px solid %1").arg(containerBorderColor(t).name());
        const QString frameRule = QStringLiteral("solid %1'").arg(t.border.name());
        QVERIFY2(html.contains(QStringLiteral("border-bottom:") + seamRule), "dividers in the seam colour");
        QVERIFY2(!html.contains(QStringLiteral("border-bottom:1px ") + frameRule)
                 && !html.contains(QStringLiteral("border-top:1px ") + frameRule),
                 "no divider in the frame colour");
        // In pixels: inside the pane no row is a full-width rule in the
        // frame colour (the pane is up: its content is mostly the surface).
        auto* pane = popup.findChild<QScrollArea*>();
        QVERIFY(pane);
        QVERIFY(!pane->isHidden());
        const qreal dpr = 1.25;
        const QImage img = renderAt(popup, dpr);
        const Edges p = edgesOf(QRect(pane->mapTo(&popup, QPoint(0, 0)), pane->size()), dpr);
        const int w = p.right - p.left + 1;
        const QRect all(p.left, p.top, w, p.bottom - p.top + 1);
        QVERIFY2(countColour(img, all, t.background) > all.width() * all.height() / 3,
                 "the pane should be on the popup surface");
        for (int y = p.top; y <= p.bottom; ++y) {
            const QRect row(p.left, y, w, 1);
            QVERIFY2(countColour(img, row, t.border) < w * 9 / 10,
                     qPrintable(QStringLiteral("a frame-coloured rule across the pane at device row %1").arg(y)));
        }
        popup.hide();
    }

    void testPopupUpdatesOnThemeChange() {
        auto& tm = ThemeManager::instance();
        int origIdx = tm.currentIndex();

        // Ensure at least two themes exist
        QVERIFY2(tm.themes().size() >= 2,
                 "Need at least 2 themes to test theme switching");

        // Create popup with current theme
        TypeSelectorPopup popup;
        TypeEntry prim;
        prim.entryKind = TypeEntry::Primitive;
        prim.primitiveKind = NodeKind::Int32;
        prim.displayName = QStringLiteral("int32_t");
        popup.setTypes({prim});

        QColor bgBefore = popup.palette().color(QPalette::Window);

        // Switch to a different theme
        int otherIdx = (origIdx == 0) ? 1 : 0;
        tm.setCurrent(otherIdx);
        QApplication::processEvents();

        // The popup should have applyTheme connected to themeChanged
        popup.applyTheme(tm.current());

        // Verify applyTheme didn't crash and child widgets exist.
        // Note: exact palette color checks are unreliable for unrealized widgets
        // because Qt's app-wide palette (set by applyGlobalTheme inside setCurrent)
        // may override the widget-local palette via the resolve mask.
        auto* filterEdit = popup.findChild<QLineEdit*>();
        QVERIFY(filterEdit);

        auto* listView = popup.findChild<QListView*>();
        QVERIFY(listView);

        // Restore original theme
        tm.setCurrent(origIdx);
    }

    void testPopupAutoConnectsThemeChange() {
        auto& tm = ThemeManager::instance();
        int origIdx = tm.currentIndex();
        QVERIFY2(tm.themes().size() >= 2, "Need >= 2 themes");

        TypeSelectorPopup popup;

        // applyTheme is a public slot — verify it can be connected
        connect(&tm, &ThemeManager::themeChanged,
                &popup, &TypeSelectorPopup::applyTheme);

        QColor bgBefore = popup.palette().color(QPalette::Window);

        int otherIdx = (origIdx == 0) ? 1 : 0;
        tm.setCurrent(otherIdx);
        QApplication::processEvents();

        // After theme change + signal, popup palette should match new theme:
        // the popup is ONE surface, theme.background (the family rule).
        QCOMPARE(popup.palette().color(QPalette::Window),
                 tm.current().background);

        // Restore
        tm.setCurrent(origIdx);
    }

    // ── parseTypeSpec: primitive pointer ptrDepth ──

    void testParseTypeSpecPrimitiveStar() {
        TypeSpec spec = parseTypeSpec("int32_t*");
        QCOMPARE(spec.baseName, QString("int32_t"));
        QVERIFY(spec.isPointer);
        QCOMPARE(spec.ptrDepth, 1);
        QCOMPARE(spec.arrayCount, 0);
    }

    void testParseTypeSpecPrimitiveDoubleStar() {
        TypeSpec spec = parseTypeSpec("f64**");
        QCOMPARE(spec.baseName, QString("f64"));
        QVERIFY(spec.isPointer);
        QCOMPARE(spec.ptrDepth, 2);
        QCOMPARE(spec.arrayCount, 0);
    }

    // ── Primitive pointer creation via applyTypePopupResult path ──

    void testPrimitivePointerCreation() {
#ifdef SKIP_GUI_INTEGRATION
        QSKIP("GUI integration test disabled");
#endif
        auto* doc = new RcxDocument();
        buildTwoRootTree(doc->tree);
        doc->provider = std::make_unique<BufferProvider>(makeBuffer());

        auto* splitter = new QSplitter();
        auto* ctrl = new RcxController(doc, nullptr);
        ctrl->addSplitEditor(splitter);

        splitter->resize(800, 600);
        splitter->show();
        QVERIFY(QTest::qWaitForWindowExposed(splitter));
        ctrl->refresh();
        QApplication::processEvents();

        // Find the "x" field (Int32) inside Alpha
        int xIdx = -1;
        for (int i = 0; i < doc->tree.nodes.size(); i++) {
            if (doc->tree.nodes[i].name == "x") { xIdx = i; break; }
        }
        QVERIFY(xIdx >= 0);
        QCOMPARE(doc->tree.nodes[xIdx].kind, NodeKind::Int32);
        uint64_t xNodeId = doc->tree.nodes[xIdx].id;

        // Simulate the primitive-pointer path: Int32 → Pointer64 + elementKind=Int32 + ptrDepth=1
        doc->undoStack.beginMacro(QStringLiteral("Change to primitive pointer"));
        ctrl->changeNodeKind(xIdx, NodeKind::Pointer64);
        int idx = doc->tree.indexOfId(xNodeId);
        QVERIFY(idx >= 0);
        doc->tree.nodes[idx].elementKind = NodeKind::Int32;
        doc->tree.nodes[idx].ptrDepth = 1;
        doc->undoStack.endMacro();
        QApplication::processEvents();

        // Verify: Pointer64 with elementKind=Int32, ptrDepth=1, refId=0
        idx = doc->tree.indexOfId(xNodeId);
        QVERIFY(idx >= 0);
        QCOMPARE(doc->tree.nodes[idx].kind, NodeKind::Pointer64);
        QCOMPARE(doc->tree.nodes[idx].elementKind, NodeKind::Int32);
        QCOMPARE(doc->tree.nodes[idx].ptrDepth, 1);
        QCOMPARE(doc->tree.nodes[idx].refId, uint64_t(0));

        // Undo reverses the macro
        doc->undoStack.undo();
        QApplication::processEvents();
        idx = doc->tree.indexOfId(xNodeId);
        QVERIFY(idx >= 0);
        QCOMPARE(doc->tree.nodes[idx].kind, NodeKind::Int32);

        delete ctrl;
        delete splitter;
        delete doc;
    }

    void testDoublePointerCreation() {
#ifdef SKIP_GUI_INTEGRATION
        QSKIP("GUI integration test disabled");
#endif
        auto* doc = new RcxDocument();
        buildTwoRootTree(doc->tree);
        doc->provider = std::make_unique<BufferProvider>(makeBuffer());

        auto* splitter = new QSplitter();
        auto* ctrl = new RcxController(doc, nullptr);
        ctrl->addSplitEditor(splitter);

        splitter->resize(800, 600);
        splitter->show();
        QVERIFY(QTest::qWaitForWindowExposed(splitter));
        ctrl->refresh();
        QApplication::processEvents();

        // Find the "x" field (Int32) inside Alpha
        int xIdx = -1;
        for (int i = 0; i < doc->tree.nodes.size(); i++) {
            if (doc->tree.nodes[i].name == "x") { xIdx = i; break; }
        }
        QVERIFY(xIdx >= 0);
        uint64_t xNodeId = doc->tree.nodes[xIdx].id;

        // Simulate: Int32 → Pointer64 + elementKind=Double + ptrDepth=2
        doc->undoStack.beginMacro(QStringLiteral("Change to double pointer"));
        ctrl->changeNodeKind(xIdx, NodeKind::Pointer64);
        int idx = doc->tree.indexOfId(xNodeId);
        QVERIFY(idx >= 0);
        doc->tree.nodes[idx].elementKind = NodeKind::Double;
        doc->tree.nodes[idx].ptrDepth = 2;
        doc->undoStack.endMacro();
        QApplication::processEvents();

        // Verify: Pointer64 with elementKind=Double, ptrDepth=2
        idx = doc->tree.indexOfId(xNodeId);
        QVERIFY(idx >= 0);
        QCOMPARE(doc->tree.nodes[idx].kind, NodeKind::Pointer64);
        QCOMPARE(doc->tree.nodes[idx].elementKind, NodeKind::Double);
        QCOMPARE(doc->tree.nodes[idx].ptrDepth, 2);
        QCOMPARE(doc->tree.nodes[idx].refId, uint64_t(0));

        delete ctrl;
        delete splitter;
        delete doc;
    }

    // ── ptrDepth JSON round-trip ──

    void testPtrDepthJsonRoundTrip() {
        Node n;
        n.kind = NodeKind::Pointer64;
        n.name = "pData";
        n.elementKind = NodeKind::Float;
        n.ptrDepth = 2;
        n.id = 42;

        QJsonObject obj = n.toJson();
        QCOMPARE(obj["ptrDepth"].toInt(), 2);

        Node restored = Node::fromJson(obj);
        QCOMPARE(restored.ptrDepth, 2);
        QCOMPARE(restored.elementKind, NodeKind::Float);
        QCOMPARE(restored.kind, NodeKind::Pointer64);
    }

    void testPtrDepthJsonDefault() {
        // Nodes without ptrDepth in JSON should default to 0
        Node n;
        n.kind = NodeKind::Pointer64;
        n.name = "pVoid";
        n.id = 99;

        QJsonObject obj = n.toJson();
        // ptrDepth==0 is not serialized
        QVERIFY(!obj.contains("ptrDepth"));

        Node restored = Node::fromJson(obj);
        QCOMPARE(restored.ptrDepth, 0);
    }

    // ── setMode always resets modifier buttons ──

    void testSetModeResetsModifierInPointerTargetMode() {
        TypeSelectorPopup popup;

        // Set FieldType mode and select * modifier
        popup.setMode(TypePopupMode::FieldType);
        popup.setModifier(1);  // select *

        // Now switch to PointerTarget mode — should reset to plain
        popup.setMode(TypePopupMode::PointerTarget);

        // Verify: modifier buttons are hidden but internally reset to plain (modId=0)
        // This means primitives will be visible in applyFilter
        TypeEntry prim;
        prim.entryKind = TypeEntry::Primitive;
        prim.primitiveKind = NodeKind::Int32;
        prim.displayName = "int32_t";

        TypeEntry voidEntry;
        voidEntry.entryKind = TypeEntry::Primitive;
        voidEntry.primitiveKind = NodeKind::Pointer64;
        voidEntry.displayName = "void";

        popup.setTypes({prim, voidEntry});

        // Both primitives should be visible (not filtered out)
        auto* listView = popup.findChild<QListView*>();
        QVERIFY(listView);
        int rowCount = listView->model()->rowCount();
        // Should have section header + 2 primitives = at least 3 rows
        QVERIFY2(rowCount >= 3,
                 qPrintable(QString("Expected >=3 rows (header+2 prims), got %1").arg(rowCount)));
    }

    // ── setModifier preselection ──

    void testSetModifierPreselects() {
        TypeSelectorPopup popup;

        // The segmented control's QButtonGroup ids are 0=Value/1=Pointer/2=Array;
        // the legacy modifier id (1=*, 2=**, 3=[]) is exposed via currentModId().
        popup.setMode(TypePopupMode::FieldType);
        popup.setModifier(1);                     // *
        QCOMPARE(popup.currentModId(), 1);

        popup.setMode(TypePopupMode::FieldType);
        popup.setModifier(2);                     // ** (Pointer + ×2 depth)
        QCOMPARE(popup.currentModId(), 2);

        popup.setMode(TypePopupMode::FieldType);
        popup.setModifier(3, 8);                  // [8]
        QCOMPARE(popup.currentModId(), 3);
    }

    // ── isValidPrimitivePtrTarget ──

    void testIsValidPrimitivePtrTarget() {
        // Hex types → NOT valid (deref shows same hex as void*)
        QVERIFY(!isValidPrimitivePtrTarget(NodeKind::Hex8));
        QVERIFY(!isValidPrimitivePtrTarget(NodeKind::Hex16));
        QVERIFY(!isValidPrimitivePtrTarget(NodeKind::Hex32));
        QVERIFY(!isValidPrimitivePtrTarget(NodeKind::Hex64));

        // Pointer types → NOT valid (use composite * for chains)
        QVERIFY(!isValidPrimitivePtrTarget(NodeKind::Pointer32));
        QVERIFY(!isValidPrimitivePtrTarget(NodeKind::Pointer64));

        // Function pointers → NOT valid
        QVERIFY(!isValidPrimitivePtrTarget(NodeKind::FuncPtr32));
        QVERIFY(!isValidPrimitivePtrTarget(NodeKind::FuncPtr64));

        // Containers → NOT valid
        QVERIFY(!isValidPrimitivePtrTarget(NodeKind::Struct));
        QVERIFY(!isValidPrimitivePtrTarget(NodeKind::Array));

        // Value types → valid
        QVERIFY(isValidPrimitivePtrTarget(NodeKind::Int32));
        QVERIFY(isValidPrimitivePtrTarget(NodeKind::UInt64));
        QVERIFY(isValidPrimitivePtrTarget(NodeKind::Float));
        QVERIFY(isValidPrimitivePtrTarget(NodeKind::Double));
        QVERIFY(isValidPrimitivePtrTarget(NodeKind::Bool));
        QVERIFY(isValidPrimitivePtrTarget(NodeKind::Vec3));
        QVERIFY(isValidPrimitivePtrTarget(NodeKind::UTF8));
    }

    // ── hex64* falls back to void* ──

    void testHex64StarFallsBackToVoidPointer() {
#ifdef SKIP_GUI_INTEGRATION
        QSKIP("GUI integration test disabled");
#endif
        auto* doc = new RcxDocument();
        buildTwoRootTree(doc->tree);
        doc->provider = std::make_unique<BufferProvider>(makeBuffer());

        auto* splitter = new QSplitter();
        auto* ctrl = new RcxController(doc, nullptr);
        ctrl->addSplitEditor(splitter);

        splitter->resize(800, 600);
        splitter->show();
        QVERIFY(QTest::qWaitForWindowExposed(splitter));
        ctrl->refresh();
        QApplication::processEvents();

        // Find the "x" field (Int32)
        int xIdx = -1;
        for (int i = 0; i < doc->tree.nodes.size(); i++) {
            if (doc->tree.nodes[i].name == "x") { xIdx = i; break; }
        }
        QVERIFY(xIdx >= 0);
        uint64_t xNodeId = doc->tree.nodes[xIdx].id;

        // Build a TypeEntry for hex64
        TypeEntry hexEntry;
        hexEntry.entryKind = TypeEntry::Primitive;
        hexEntry.primitiveKind = NodeKind::Hex64;
        hexEntry.displayName = "hex64";

        // Apply it with pointer modifier (fullText = "hex64*")
        ctrl->applyTypePopupResult(TypePopupMode::FieldType, xIdx,
                                   hexEntry, QStringLiteral("hex64*"));
        QApplication::processEvents();

        // Should be a void pointer: Pointer64, ptrDepth=0, refId=0
        int idx = doc->tree.indexOfId(xNodeId);
        QVERIFY(idx >= 0);
        QCOMPARE(doc->tree.nodes[idx].kind, NodeKind::Pointer64);
        QCOMPARE(doc->tree.nodes[idx].ptrDepth, 0);
        QCOMPARE(doc->tree.nodes[idx].refId, uint64_t(0));

        delete ctrl;
        delete splitter;
        delete doc;
    }

    void testHex8StarFallsBackToVoidPointer() {
#ifdef SKIP_GUI_INTEGRATION
        QSKIP("GUI integration test disabled");
#endif
        auto* doc = new RcxDocument();
        buildTwoRootTree(doc->tree);
        doc->provider = std::make_unique<BufferProvider>(makeBuffer());

        auto* splitter = new QSplitter();
        auto* ctrl = new RcxController(doc, nullptr);
        ctrl->addSplitEditor(splitter);

        splitter->resize(800, 600);
        splitter->show();
        QVERIFY(QTest::qWaitForWindowExposed(splitter));
        ctrl->refresh();
        QApplication::processEvents();

        int xIdx = -1;
        for (int i = 0; i < doc->tree.nodes.size(); i++) {
            if (doc->tree.nodes[i].name == "x") { xIdx = i; break; }
        }
        QVERIFY(xIdx >= 0);
        uint64_t xNodeId = doc->tree.nodes[xIdx].id;

        TypeEntry hexEntry;
        hexEntry.entryKind = TypeEntry::Primitive;
        hexEntry.primitiveKind = NodeKind::Hex8;
        hexEntry.displayName = "hex8";

        ctrl->applyTypePopupResult(TypePopupMode::FieldType, xIdx,
                                   hexEntry, QStringLiteral("hex8*"));
        QApplication::processEvents();

        int idx = doc->tree.indexOfId(xNodeId);
        QVERIFY(idx >= 0);
        QCOMPARE(doc->tree.nodes[idx].kind, NodeKind::Pointer64);
        QCOMPARE(doc->tree.nodes[idx].ptrDepth, 0);
        QCOMPARE(doc->tree.nodes[idx].refId, uint64_t(0));

        delete ctrl;
        delete splitter;
        delete doc;
    }

    void testPtr64StarFallsBackToVoidPointer() {
#ifdef SKIP_GUI_INTEGRATION
        QSKIP("GUI integration test disabled");
#endif
        auto* doc = new RcxDocument();
        buildTwoRootTree(doc->tree);
        doc->provider = std::make_unique<BufferProvider>(makeBuffer());

        auto* splitter = new QSplitter();
        auto* ctrl = new RcxController(doc, nullptr);
        ctrl->addSplitEditor(splitter);

        splitter->resize(800, 600);
        splitter->show();
        QVERIFY(QTest::qWaitForWindowExposed(splitter));
        ctrl->refresh();
        QApplication::processEvents();

        int xIdx = -1;
        for (int i = 0; i < doc->tree.nodes.size(); i++) {
            if (doc->tree.nodes[i].name == "x") { xIdx = i; break; }
        }
        QVERIFY(xIdx >= 0);
        uint64_t xNodeId = doc->tree.nodes[xIdx].id;

        TypeEntry ptrEntry;
        ptrEntry.entryKind = TypeEntry::Primitive;
        ptrEntry.primitiveKind = NodeKind::Pointer64;
        ptrEntry.displayName = "ptr64";

        ctrl->applyTypePopupResult(TypePopupMode::FieldType, xIdx,
                                   ptrEntry, QStringLiteral("ptr64*"));
        QApplication::processEvents();

        int idx = doc->tree.indexOfId(xNodeId);
        QVERIFY(idx >= 0);
        QCOMPARE(doc->tree.nodes[idx].kind, NodeKind::Pointer64);
        QCOMPARE(doc->tree.nodes[idx].ptrDepth, 0);
        QCOMPARE(doc->tree.nodes[idx].refId, uint64_t(0));

        delete ctrl;
        delete splitter;
        delete doc;
    }

    // ── Valid primitive pointers still work ──

    void testInt32StarStillCreatesPrimitivePointer() {
#ifdef SKIP_GUI_INTEGRATION
        QSKIP("GUI integration test disabled");
#endif
        auto* doc = new RcxDocument();
        buildTwoRootTree(doc->tree);
        doc->provider = std::make_unique<BufferProvider>(makeBuffer());

        auto* splitter = new QSplitter();
        auto* ctrl = new RcxController(doc, nullptr);
        ctrl->addSplitEditor(splitter);

        splitter->resize(800, 600);
        splitter->show();
        QVERIFY(QTest::qWaitForWindowExposed(splitter));
        ctrl->refresh();
        QApplication::processEvents();

        int xIdx = -1;
        for (int i = 0; i < doc->tree.nodes.size(); i++) {
            if (doc->tree.nodes[i].name == "x") { xIdx = i; break; }
        }
        QVERIFY(xIdx >= 0);
        uint64_t xNodeId = doc->tree.nodes[xIdx].id;

        TypeEntry intEntry;
        intEntry.entryKind = TypeEntry::Primitive;
        intEntry.primitiveKind = NodeKind::Int32;
        intEntry.displayName = "int32_t";

        ctrl->applyTypePopupResult(TypePopupMode::FieldType, xIdx,
                                   intEntry, QStringLiteral("int32_t*"));
        QApplication::processEvents();

        int idx = doc->tree.indexOfId(xNodeId);
        QVERIFY(idx >= 0);
        QCOMPARE(doc->tree.nodes[idx].kind, NodeKind::Pointer64);
        QCOMPARE(doc->tree.nodes[idx].ptrDepth, 1);
        QCOMPARE(doc->tree.nodes[idx].elementKind, NodeKind::Int32);
        QCOMPARE(doc->tree.nodes[idx].refId, uint64_t(0));

        delete ctrl;
        delete splitter;
        delete doc;
    }

    // ── 32-bit target: the "*" modifier MUST yield Pointer32 (4 bytes),
    // not Pointer64 — pointer size follows the attached process width ──

    void testPrimitivePointerOn32BitTargetUsesPointer32() {
#ifdef SKIP_GUI_INTEGRATION
        QSKIP("GUI integration test disabled");
#endif
        auto* doc = new RcxDocument();
        buildTwoRootTree(doc->tree);
        doc->tree.pointerSize = 4;   // 32-bit target process
        doc->provider = std::make_unique<BufferProvider>(makeBuffer());

        auto* splitter = new QSplitter();
        auto* ctrl = new RcxController(doc, nullptr);
        ctrl->addSplitEditor(splitter);

        splitter->resize(800, 600);
        splitter->show();
        QVERIFY(QTest::qWaitForWindowExposed(splitter));
        ctrl->refresh();
        QApplication::processEvents();

        int xIdx = -1;
        for (int i = 0; i < doc->tree.nodes.size(); i++) {
            if (doc->tree.nodes[i].name == "x") { xIdx = i; break; }
        }
        QVERIFY(xIdx >= 0);
        uint64_t xNodeId = doc->tree.nodes[xIdx].id;

        // uint8_t* on a 32-bit target → Pointer32 + elementKind=UInt8 + ptrDepth=1
        TypeEntry u8Entry;
        u8Entry.entryKind = TypeEntry::Primitive;
        u8Entry.primitiveKind = NodeKind::UInt8;
        u8Entry.displayName = "uint8_t";

        ctrl->applyTypePopupResult(TypePopupMode::FieldType, xIdx,
                                   u8Entry, QStringLiteral("uint8_t*"));
        QApplication::processEvents();

        int idx = doc->tree.indexOfId(xNodeId);
        QVERIFY(idx >= 0);
        QCOMPARE(doc->tree.nodes[idx].kind, NodeKind::Pointer32);
        QCOMPARE(doc->tree.nodes[idx].ptrDepth, 1);
        QCOMPARE(doc->tree.nodes[idx].elementKind, NodeKind::UInt8);
        QCOMPARE(doc->tree.nodes[idx].refId, uint64_t(0));
        QCOMPARE(doc->tree.nodes[idx].byteSize(), 4);

        delete ctrl;
        delete splitter;
        delete doc;
    }

    void testVoidPointerFallbackOn32BitTargetUsesPointer32() {
#ifdef SKIP_GUI_INTEGRATION
        QSKIP("GUI integration test disabled");
#endif
        auto* doc = new RcxDocument();
        buildTwoRootTree(doc->tree);
        doc->tree.pointerSize = 4;   // 32-bit target process
        doc->provider = std::make_unique<BufferProvider>(makeBuffer());

        auto* splitter = new QSplitter();
        auto* ctrl = new RcxController(doc, nullptr);
        ctrl->addSplitEditor(splitter);

        splitter->resize(800, 600);
        splitter->show();
        QVERIFY(QTest::qWaitForWindowExposed(splitter));
        ctrl->refresh();
        QApplication::processEvents();

        int xIdx = -1;
        for (int i = 0; i < doc->tree.nodes.size(); i++) {
            if (doc->tree.nodes[i].name == "x") { xIdx = i; break; }
        }
        QVERIFY(xIdx >= 0);
        uint64_t xNodeId = doc->tree.nodes[xIdx].id;

        // hex8* is not a valid primitive pointer target → void pointer.
        // On a 32-bit target the void pointer must be Pointer32.
        TypeEntry hexEntry;
        hexEntry.entryKind = TypeEntry::Primitive;
        hexEntry.primitiveKind = NodeKind::Hex8;
        hexEntry.displayName = "hex8";

        ctrl->applyTypePopupResult(TypePopupMode::FieldType, xIdx,
                                   hexEntry, QStringLiteral("hex8*"));
        QApplication::processEvents();

        int idx = doc->tree.indexOfId(xNodeId);
        QVERIFY(idx >= 0);
        QCOMPARE(doc->tree.nodes[idx].kind, NodeKind::Pointer32);
        QCOMPARE(doc->tree.nodes[idx].ptrDepth, 0);
        QCOMPARE(doc->tree.nodes[idx].refId, uint64_t(0));

        delete ctrl;
        delete splitter;
        delete doc;
    }

    void testCompositePointerOn32BitTargetUsesPointer32() {
#ifdef SKIP_GUI_INTEGRATION
        QSKIP("GUI integration test disabled");
#endif
        auto* doc = new RcxDocument();
        buildTwoRootTree(doc->tree);
        doc->tree.pointerSize = 4;   // 32-bit target process
        doc->provider = std::make_unique<BufferProvider>(makeBuffer());

        auto* splitter = new QSplitter();
        auto* ctrl = new RcxController(doc, nullptr);
        ctrl->addSplitEditor(splitter);

        splitter->resize(800, 600);
        splitter->show();
        QVERIFY(QTest::qWaitForWindowExposed(splitter));
        ctrl->refresh();
        QApplication::processEvents();

        int xIdx = -1;
        uint64_t bravoId = 0;
        for (int i = 0; i < doc->tree.nodes.size(); i++) {
            const auto& n = doc->tree.nodes[i];
            if (n.name == "x" && n.kind == NodeKind::Int32) xIdx = i;
            if (n.name == "Bravo" && n.kind == NodeKind::Struct) bravoId = n.id;
        }
        QVERIFY(xIdx >= 0);
        QVERIFY(bravoId != 0);
        uint64_t xNodeId = doc->tree.nodes[xIdx].id;

        // Bravo* on a 32-bit target → Pointer32 + refId (target stays wired)
        TypeEntry bravoEntry;
        bravoEntry.entryKind = TypeEntry::Composite;
        bravoEntry.structId = bravoId;
        bravoEntry.displayName = "Bravo";

        ctrl->applyTypePopupResult(TypePopupMode::FieldType, xIdx,
                                   bravoEntry, QStringLiteral("Bravo*"));
        QApplication::processEvents();

        int idx = doc->tree.indexOfId(xNodeId);
        QVERIFY(idx >= 0);
        QCOMPARE(doc->tree.nodes[idx].kind, NodeKind::Pointer32);
        QCOMPARE(doc->tree.nodes[idx].ptrDepth, 0);
        QCOMPARE(doc->tree.nodes[idx].refId, bravoId);

        delete ctrl;
        delete splitter;
        delete doc;
    }

    void testDoubleDoubleStarStillCreatesPrimitivePointer() {
#ifdef SKIP_GUI_INTEGRATION
        QSKIP("GUI integration test disabled");
#endif
        auto* doc = new RcxDocument();
        buildTwoRootTree(doc->tree);
        doc->provider = std::make_unique<BufferProvider>(makeBuffer());

        auto* splitter = new QSplitter();
        auto* ctrl = new RcxController(doc, nullptr);
        ctrl->addSplitEditor(splitter);

        splitter->resize(800, 600);
        splitter->show();
        QVERIFY(QTest::qWaitForWindowExposed(splitter));
        ctrl->refresh();
        QApplication::processEvents();

        int xIdx = -1;
        for (int i = 0; i < doc->tree.nodes.size(); i++) {
            if (doc->tree.nodes[i].name == "x") { xIdx = i; break; }
        }
        QVERIFY(xIdx >= 0);
        uint64_t xNodeId = doc->tree.nodes[xIdx].id;

        TypeEntry dblEntry;
        dblEntry.entryKind = TypeEntry::Primitive;
        dblEntry.primitiveKind = NodeKind::Double;
        dblEntry.displayName = "double";

        ctrl->applyTypePopupResult(TypePopupMode::FieldType, xIdx,
                                   dblEntry, QStringLiteral("double**"));
        QApplication::processEvents();

        int idx = doc->tree.indexOfId(xNodeId);
        QVERIFY(idx >= 0);
        QCOMPARE(doc->tree.nodes[idx].kind, NodeKind::Pointer64);
        QCOMPARE(doc->tree.nodes[idx].ptrDepth, 2);
        QCOMPARE(doc->tree.nodes[idx].elementKind, NodeKind::Double);
        QCOMPARE(doc->tree.nodes[idx].refId, uint64_t(0));

        delete ctrl;
        delete splitter;
        delete doc;
    }

    // ── Defense: compose/format treat invalid ptrDepth as void* ──

    void testComposeShowsVoidPtrForHexPtrDepth() {
#ifdef SKIP_GUI_INTEGRATION
        QSKIP("GUI integration test disabled");
#endif
        // If a node somehow has ptrDepth>0 with hex elementKind
        // (e.g. from old JSON), compose should show "void*" not "hex64*"
        NodeTree tree;
        tree.baseAddress = 0x1000;

        Node root;
        root.kind = NodeKind::Struct;
        root.name = "Test";
        root.structTypeName = "Test";
        root.parentId = 0;
        tree.addNode(root);
        uint64_t rootId = tree.nodes[0].id;

        Node ptr;
        ptr.kind = NodeKind::Pointer64;
        ptr.name = "badPtr";
        ptr.parentId = rootId;
        ptr.offset = 0;
        ptr.ptrDepth = 1;
        ptr.elementKind = NodeKind::Hex64;  // invalid target
        tree.addNode(ptr);

        QByteArray buf(0x100, '\0');
        BufferProvider prov(buf);

        ComposeResult result = compose(tree, prov);

        // The composed text should NOT contain "hex64*" — the invalid target
        // should fall through to normal void pointer display
        QVERIFY2(!result.text.contains("hex64*"),
                 qPrintable("Should not show 'hex64*', got: " + result.text));
    }
    // ── Category chips and three-group filtering ──

    void testCategoryEnumOnEntry() {
        // Verify that Category enum values exist and are distinct
        TypeEntry prim;
        prim.category = TypeEntry::CatPrimitive;
        QCOMPARE(prim.category, TypeEntry::CatPrimitive);

        TypeEntry typ;
        typ.category = TypeEntry::CatType;
        QCOMPARE(typ.category, TypeEntry::CatType);

        TypeEntry en;
        en.category = TypeEntry::CatEnum;
        QCOMPARE(en.category, TypeEntry::CatEnum);

        QVERIFY(TypeEntry::CatPrimitive != TypeEntry::CatType);
        QVERIFY(TypeEntry::CatType != TypeEntry::CatEnum);
    }

    void testCategoryDefaultIsPrimitive() {
        TypeEntry e;
        QCOMPARE(e.category, TypeEntry::CatPrimitive);
    }

    void testCompositesCategorizedInController() {
#ifdef SKIP_GUI_INTEGRATION
        QSKIP("GUI integration test disabled");
#endif
        // Build tree with struct and enum types
        NodeTree tree;
        tree.baseAddress = 0;

        Node st;
        st.kind = NodeKind::Struct;
        st.name = "Ball";
        st.structTypeName = "Ball";
        st.parentId = 0;
        int si = tree.addNode(st);
        uint64_t stId = tree.nodes[si].id;

        { Node n; n.kind = NodeKind::Int32; n.name = "x"; n.parentId = stId;
          n.offset = 0; tree.addNode(n); }

        Node en;
        en.kind = NodeKind::Struct;
        en.name = "Color";
        en.structTypeName = "Color";
        en.classKeyword = QStringLiteral("enum");
        en.parentId = 0;
        tree.addNode(en);

        // Simulate controller logic: tag composites
        QVector<TypeEntry> entries;
        for (const auto& n : tree.nodes) {
            if (n.parentId != 0 || n.kind != NodeKind::Struct) continue;
            TypeEntry e;
            e.entryKind    = TypeEntry::Composite;
            e.structId     = n.id;
            e.displayName  = n.structTypeName.isEmpty() ? n.name : n.structTypeName;
            e.classKeyword = n.resolvedClassKeyword();
            e.category     = (e.classKeyword == QStringLiteral("enum"))
                           ? TypeEntry::CatEnum : TypeEntry::CatType;
            entries.append(e);
        }

        QCOMPARE(entries.size(), 2);
        // Ball → CatType, Color → CatEnum
        bool foundType = false, foundEnum = false;
        for (const auto& e : entries) {
            if (e.displayName == "Ball") {
                QCOMPARE(e.category, TypeEntry::CatType);
                foundType = true;
            }
            if (e.displayName == "Color") {
                QCOMPARE(e.category, TypeEntry::CatEnum);
                foundEnum = true;
            }
        }
        QVERIFY(foundType);
        QVERIFY(foundEnum);
    }

    void testThreeGroupSections() {
        // Create popup and set types with mixed categories
        TypeSelectorPopup popup;
        popup.setMode(TypePopupMode::FieldType);

        QVector<TypeEntry> types;

        // A primitive
        TypeEntry prim;
        prim.entryKind = TypeEntry::Primitive;
        prim.primitiveKind = NodeKind::Int32;
        prim.displayName = QStringLiteral("int32_t");
        prim.category = TypeEntry::CatPrimitive;
        types.append(prim);

        // A struct type
        TypeEntry st;
        st.entryKind = TypeEntry::Composite;
        st.structId = 1;
        st.displayName = QStringLiteral("Player");
        st.classKeyword = QStringLiteral("struct");
        st.category = TypeEntry::CatType;
        types.append(st);

        // An enum type
        TypeEntry en;
        en.entryKind = TypeEntry::Composite;
        en.structId = 2;
        en.displayName = QStringLiteral("Color");
        en.classKeyword = QStringLiteral("enum");
        en.category = TypeEntry::CatEnum;
        types.append(en);

        popup.setTypes(types);

        // The popup should have three sections in field mode:
        // primitives → types → enums
        // We can access via the internal model
        auto* model = popup.findChild<QStringListModel*>();
        QVERIFY(model != nullptr);
        QStringList items = model->stringList();

        // Should contain kind-group section headers (Int/Bool for int32_t,
        // "Your classes" for the struct/enum composites).
        bool hasIntSection = false, hasTypeSection = false;
        for (const auto& item : items) {
            if (item.contains(QStringLiteral("Int"))) hasIntSection = true;
            if (item.contains(QStringLiteral("Your classes"))) hasTypeSection = true;
        }
        QVERIFY2(hasIntSection, "Missing 'Int' section header");
        QVERIFY2(hasTypeSection, "Missing 'Your classes' section header");
    }

    // ── Test: struct embed auto-selects the current composite in popup ──

    void testStructEmbedAutoSelectsCurrent() {
#ifdef SKIP_GUI_INTEGRATION
        QSKIP("GUI integration test disabled");
#endif
        TypeSelectorPopup popup;
        popup.setMode(TypePopupMode::FieldType);
        QFont font(QStringLiteral("Consolas"), 10);
        popup.setFont(font);

        // Build entries: a primitive + two composites
        QVector<TypeEntry> types;

        TypeEntry prim;
        prim.entryKind = TypeEntry::Primitive;
        prim.primitiveKind = NodeKind::Int32;
        prim.displayName = QStringLiteral("int32_t");
        types.append(prim);

        TypeEntry alpha;
        alpha.entryKind = TypeEntry::Composite;
        alpha.structId = 100;
        alpha.displayName = QStringLiteral("Alpha");
        alpha.classKeyword = QStringLiteral("struct");
        alpha.category = TypeEntry::CatType;
        types.append(alpha);

        TypeEntry bravo;
        bravo.entryKind = TypeEntry::Composite;
        bravo.structId = 200;
        bravo.displayName = QStringLiteral("Bravo");
        bravo.classKeyword = QStringLiteral("struct");
        bravo.category = TypeEntry::CatType;
        types.append(bravo);

        // Set Bravo as the current type (simulates struct embed field with refId=200)
        popup.setTypes(types, &bravo);
        popup.popup(QPoint(-9999, -9999));
        QApplication::processEvents();

        // The list view should auto-select the row matching Bravo
        auto* listView = popup.findChild<QListView*>();
        QVERIFY(listView != nullptr);
        QModelIndex sel = listView->currentIndex();
        QVERIFY2(sel.isValid(), "No item selected — auto-select failed");

        // The selected row text should contain "Bravo"
        QString selectedText = sel.data().toString();
        QVERIFY2(selectedText.contains(QStringLiteral("Bravo")),
                 qPrintable(QString("Expected 'Bravo' in selected text, got '%1'").arg(selectedText)));

        popup.hide();
    }

    // ── createNewTypeRequested carries modifier state ──

    void testCreateNewSignalCarriesModifier() {
        TypeSelectorPopup popup;
        popup.warmUp();
        popup.setMode(TypePopupMode::FieldType);
        popup.setPointerSize(8);

        TypeEntry prim;
        prim.entryKind = TypeEntry::Primitive;
        prim.primitiveKind = NodeKind::Int32;
        prim.displayName = "int32_t";
        popup.setTypes({prim});

        QSignalSpy spy(&popup, &TypeSelectorPopup::createNewTypeRequested);
        QVERIFY(spy.isValid());

        // ── Plain (no modifier) ──
        popup.setModifier(0);
        // The create button is a DialogButton (QPushButton), not QToolButton.
        QPushButton* newBtn = nullptr;
        for (auto* btn : popup.findChildren<QPushButton*>()) {
            if (btn->text().contains("New")) { newBtn = btn; break; }
        }
        QVERIFY2(newBtn, "Could not find 'New' button in popup");

        newBtn->click();
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toInt(), 0);   // modifierId = 0 (plain)
        QCOMPARE(spy.at(0).at(1).toInt(), 0);   // arrayCount = 0

        // ── Pointer (*) modifier ──
        spy.clear();
        popup.show();
        popup.setModifier(1);
        newBtn->click();
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toInt(), 1);   // modifierId = 1 (*)
        QCOMPARE(spy.at(0).at(1).toInt(), 0);

        // ── Double pointer (**) modifier ──
        spy.clear();
        popup.show();
        popup.setModifier(2);
        newBtn->click();
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toInt(), 2);   // modifierId = 2 (**)

        // ── Array ([]) modifier ──
        spy.clear();
        popup.show();
        popup.setModifier(3, 16);
        newBtn->click();
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toInt(), 3);   // modifierId = 3 ([])
        QCOMPARE(spy.at(0).at(1).toInt(), 16);  // arrayCount = 16
    }

    // ── Controller: createNewType with pointer modifier → node becomes Pointer64 ──

    void testCreateNewTypeWithPtrModifier() {
#ifdef SKIP_GUI_INTEGRATION
        QSKIP("GUI integration test disabled");
#endif
        // Setup: tree with one Hex64 field
        NodeTree tree;
        tree.baseAddress = 0;
        Node root;
        root.kind = NodeKind::Struct;
        root.name = "Root";
        root.structTypeName = "Root";
        root.parentId = 0;
        root.offset = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        Node field;
        field.kind = NodeKind::Hex64;
        field.name = "target";
        field.parentId = rootId;
        field.offset = 0;
        int fi = tree.addNode(field);
        uint64_t fieldId = tree.nodes[fi].id;

        auto* doc = new RcxDocument();
        doc->tree = tree;
        doc->loadData(makeBuffer());
        doc->tree.baseAddress = 0;

        auto* splitter = new QSplitter;
        auto ctrl = new RcxController(doc, splitter);
        ctrl->addSplitEditor(splitter);
        ctrl->setViewRootId(rootId);
        ctrl->refresh();

        // Simulate: popup selects "New" with * modifier (modifierId=1)
        // This creates a new root struct and calls applyTypePopupResult with "NewClass*"
        TypeEntry newEntry;
        newEntry.entryKind = TypeEntry::Composite;

        // Create the new struct manually (mimics what the handler does)
        Node newStruct;
        newStruct.kind = NodeKind::Struct;
        newStruct.structTypeName = "TestNewClass";
        newStruct.name = "instance";
        newStruct.parentId = 0;
        newStruct.offset = 0;
        newStruct.id = doc->tree.reserveId();
        doc->undoStack.push(new RcxCommand(ctrl, cmd::Insert{newStruct}));
        newEntry.structId = newStruct.id;

        // Apply with pointer modifier fullText
        ctrl->applyTypePopupResult(TypePopupMode::FieldType, fi, newEntry,
                                   QStringLiteral("TestNewClass*"));

        // Verify: field should now be Pointer64 with refId pointing to the new struct
        int updatedIdx = doc->tree.indexOfId(fieldId);
        QVERIFY(updatedIdx >= 0);
        const Node& updated = doc->tree.nodes[updatedIdx];
        QCOMPARE(updated.kind, NodeKind::Pointer64);
        QCOMPARE(updated.refId, newStruct.id);

        delete splitter;
        delete doc;
    }

    // ── Controller: createNewType with array modifier → node becomes Array ──

    void testCreateNewTypeWithArrayModifier() {
#ifdef SKIP_GUI_INTEGRATION
        QSKIP("GUI integration test disabled");
#endif
        NodeTree tree;
        tree.baseAddress = 0;
        Node root;
        root.kind = NodeKind::Struct;
        root.name = "Root";
        root.structTypeName = "Root";
        root.parentId = 0;
        root.offset = 0;
        int ri = tree.addNode(root);
        uint64_t rootId = tree.nodes[ri].id;

        Node field;
        field.kind = NodeKind::Hex64;
        field.name = "target";
        field.parentId = rootId;
        field.offset = 0;
        int fi = tree.addNode(field);
        uint64_t fieldId = tree.nodes[fi].id;

        auto* doc = new RcxDocument();
        doc->tree = tree;
        doc->loadData(makeBuffer());
        doc->tree.baseAddress = 0;

        auto* splitter = new QSplitter;
        auto ctrl = new RcxController(doc, splitter);
        ctrl->addSplitEditor(splitter);
        ctrl->setViewRootId(rootId);
        ctrl->refresh();

        // Create the new struct
        Node newStruct;
        newStruct.kind = NodeKind::Struct;
        newStruct.structTypeName = "ArrayElemType";
        newStruct.name = "instance";
        newStruct.parentId = 0;
        newStruct.offset = 0;
        newStruct.id = doc->tree.reserveId();
        doc->undoStack.push(new RcxCommand(ctrl, cmd::Insert{newStruct}));

        TypeEntry newEntry;
        newEntry.entryKind = TypeEntry::Composite;
        newEntry.structId = newStruct.id;

        // Apply with array modifier fullText
        ctrl->applyTypePopupResult(TypePopupMode::FieldType, fi, newEntry,
                                   QStringLiteral("ArrayElemType[10]"));

        // Verify: field should now be Array with elementKind=Struct, arrayLen=10, refId set
        int updatedIdx = doc->tree.indexOfId(fieldId);
        QVERIFY(updatedIdx >= 0);
        const Node& updated = doc->tree.nodes[updatedIdx];
        QCOMPARE(updated.kind, NodeKind::Array);
        QCOMPARE(updated.elementKind, NodeKind::Struct);
        QCOMPARE(updated.arrayLen, 10);
        QCOMPARE(updated.refId, newStruct.id);

        delete splitter;
        delete doc;
    }
    // ── Cold vs Warm open latency benchmark ──
    // Measures actual user-perceived popup open time.
    // Run ONLY this test:  test_type_selector benchmarkColdVsWarm

    void benchmarkColdVsWarm() {
        auto ms = [](qint64 ns) { return QString::number(ns / 1000000.0, 'f', 2); };

        // Build a realistic type list (primitives + 200 composites)
        QVector<TypeEntry> types;
        for (const auto& m : kKindMeta) {
            if (m.kind == NodeKind::Struct || m.kind == NodeKind::Array) continue;
            TypeEntry e;
            e.entryKind = TypeEntry::Primitive;
            e.primitiveKind = m.kind;
            e.displayName = QString::fromLatin1(m.typeName);
            e.sizeBytes = m.size;
            e.alignment = m.align;
            e.kindGroup = kindGroupFor(m.kind);
            types.append(e);
        }
        for (int i = 0; i < 200; i++) {
            TypeEntry e;
            e.entryKind = TypeEntry::Composite;
            e.structId = (uint64_t)(i + 100);
            e.displayName = QStringLiteral("_STRUCT_%1").arg(i, 3, 10, QChar('0'));
            e.classKeyword = QStringLiteral("struct");
            e.sizeBytes = 64 + (i % 64) * 8;
            e.alignment = 8;
            e.fieldCount = 4 + (i % 12);
            for (int f = 0; f < qMin(4, e.fieldCount); f++)
                e.fieldSummary << QStringLiteral("0x%1: int32_t f%2").arg(f*4, 2, 16, QChar('0')).arg(f);
            types.append(e);
        }

        QFont font("Consolas", 12);
        font.setFixedPitch(true);
        TypeEntry cur;
        cur.entryKind = TypeEntry::Primitive;
        cur.primitiveKind = NodeKind::Int32;
        cur.displayName = "int32_t";

        auto openPopup = [&](TypeSelectorPopup* popup) -> qint64 {
            QElapsedTimer t;
            t.start();
            popup->setFont(font);
            popup->setMode(TypePopupMode::FieldType);
            popup->setCurrentNodeSize(4);
            popup->setTypes(types, &cur);
            popup->popup(QPoint(-9999, -9999));
            QApplication::processEvents();
            qint64 elapsed = t.nsecsElapsed();
            popup->hide();
            QApplication::processEvents();
            return elapsed;
        };

        // ─── COLD: first-ever popup in this process ───
        qint64 tCold;
        {
            QElapsedTimer t; t.start();
            auto* popup = new TypeSelectorPopup();
            tCold = t.nsecsElapsed(); // ctor time
            tCold += openPopup(popup);
            delete popup;
        }

        // ─── preload(): static one-time primer (what main() calls) ───
        // Note: if COLD above already triggered the DLL init, preload() will
        // short-circuit (s_primerDone is true). This is the real-world scenario:
        // main() calls preload() before any popup is created.
        qint64 tPreload;
        {
            QElapsedTimer t; t.start();
            TypeSelectorPopup::preload();
            tPreload = t.nsecsElapsed();
        }

        // ─── 1ST user show (after preload, new popup with warmUp) ───
        qint64 tWarmUp, t1st;
        TypeSelectorPopup* popup;
        {
            QElapsedTimer t; t.start();
            popup = new TypeSelectorPopup();
            popup->warmUp();
            tWarmUp = t.nsecsElapsed();

            t1st = openPopup(popup);
        }

        // ─── 2ND show (fully warm, same cached popup) ───
        qint64 t2nd = openPopup(popup);

        // ─── 3RD show (another new popup, no warmUp — DLL already init'd) ───
        qint64 t3rd;
        {
            auto* p2 = new TypeSelectorPopup();
            t3rd = openPopup(p2);
            delete p2;
        }

        delete popup;

        qDebug() << "";
        qDebug().noquote() << "==========================================================";
        qDebug().noquote() << "  TypeSelectorPopup Open Latency Benchmark";
        qDebug().noquote() << "==========================================================";
        qDebug().noquote() << QString("  COLD (first-ever):          %1 ms").arg(ms(tCold).rightJustified(8));
        qDebug().noquote() << QString("  preload() (static primer):  %1 ms").arg(ms(tPreload).rightJustified(8));
        qDebug().noquote() << QString("  warmUp() (ctor+show/hide):  %1 ms").arg(ms(tWarmUp).rightJustified(8));
        qDebug().noquote() << QString("  1ST show (after warmUp):    %1 ms  <-- user-visible").arg(ms(t1st).rightJustified(8));
        qDebug().noquote() << QString("  2ND show (same popup):      %1 ms  <-- user-visible").arg(ms(t2nd).rightJustified(8));
        qDebug().noquote() << QString("  3RD show (new popup, no WU):%1 ms  <-- user-visible").arg(ms(t3rd).rightJustified(8));
        qDebug().noquote() << "==========================================================";
        double speedup = (double)tCold / qMax((qint64)1, t1st);
        qDebug().noquote() << QString("  Speedup cold->1st: %1x").arg(speedup, 0, 'f', 1);
        qDebug().noquote() << "==========================================================";

        // Assert: 1st user show after warmUp must be under 50ms
        QVERIFY2(t1st < 50000000LL,
                 qPrintable(QString("1st show after warmUp (%1ms) should be < 50ms")
                     .arg(ms(t1st))));

        // Assert: 2nd show (cached) must be under 50ms
        QVERIFY2(t2nd < 50000000LL,
                 qPrintable(QString("2nd show (%1ms) should be < 50ms")
                     .arg(ms(t2nd))));

        // Assert: 3rd show (new popup, no warmUp, but DLL already init'd) under 50ms
        QVERIFY2(t3rd < 50000000LL,
                 qPrintable(QString("3rd show (new popup) (%1ms) should be < 50ms")
                     .arg(ms(t3rd))));
    }
};

QTEST_MAIN(TestTypeSelector)
#include "test_type_selector.moc"
