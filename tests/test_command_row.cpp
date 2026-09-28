#include <QTest>
#include <QString>
#include "core.h"

using namespace rcx;

// The command row (Scintilla line 0) as RcxController::updateCommandRow
// builds it — through buildCommandRowText — parsed back through the SAME
// span functions the editor's hit test, pills and hover read it with. No
// hand copy of the format lives here: a change to the builder or to a
// parser fails these, not a stale replica of them.
//
// Row shape: "[▸] <address>  <keyword> <ClassName> {". The source control
// ('name'▾) moved to the address bar's chip; the address cell stays until
// P7 demotes the row to the class header alone.

namespace {

QString spanText(const QString& row, const ColumnSpan& s) {
    return s.valid ? row.mid(s.start, s.end - s.start) : QString();
}

// Parse a built row back: the address cell must follow the chevron directly
// (nothing sits between them any more) and read as `addr`; the root spans
// must read as the keyword and the class name.
void checkRow(const QString& row, const QString& addr, const QString& keyword,
              const QString& className) {
    QVERIFY2(!row.contains(QChar(0x25BE)), qPrintable(row));   // no source control, no ▾
    const ColumnSpan chev = commandRowChevronSpan(row);
    QVERIFY(chev.valid);
    QCOMPARE(chev.start, 0);
    QCOMPARE(chev.end, 4);
    const ColumnSpan as = commandRowAddrSpan(row);
    QVERIFY2(as.valid, qPrintable(row));
    QCOMPARE(as.start, chev.end);
    QCOMPARE(spanText(row, as), addr);
    QCOMPARE(spanText(row, commandRowRootTypeSpan(row)), keyword);
    QCOMPARE(spanText(row, commandRowRootNameSpan(row)), className);
}

}  // namespace

class TestCommandRow : public QObject {
    Q_OBJECT

private slots:

    // ---------------------------------------------------------------
    // The built row
    // ---------------------------------------------------------------

    void row_literalAddress() {
        const QString row = buildCommandRowText(QStringLiteral("0x140000000"), QStringLiteral("struct"),
                                                QStringLiteral("Player"), false);
        QCOMPARE(row, QStringLiteral("[▸] 0x140000000  struct Player {"));
        checkRow(row, QStringLiteral("0x140000000"), QStringLiteral("struct"), QStringLiteral("Player"));
    }

    void row_zeroBaseIsComposePlaceholderShape() {
        // compose.cpp's line-0 placeholder is this exact string; test_compose
        // checks the other side of the equation against compose() itself.
        const QString row = buildCommandRowText(QStringLiteral("0x0"), QStringLiteral("struct"),
                                                QStringLiteral("Untitled"), false);
        QCOMPARE(row, QStringLiteral("[▸] 0x0  struct Untitled {"));
        checkRow(row, QStringLiteral("0x0"), QStringLiteral("struct"), QStringLiteral("Untitled"));
    }

    void row_formulaStartingWithAngle() {
        const QString f = QStringLiteral("<game.exe>+0x40");
        const QString row = buildCommandRowText(f, QStringLiteral("class"), QStringLiteral("Foo"), false);
        QCOMPARE(row, QStringLiteral("[▸] <game.exe>+0x40  class Foo {"));
        checkRow(row, f, QStringLiteral("class"), QStringLiteral("Foo"));
    }

    void row_formulaStartingWithBracket() {
        const QString f = QStringLiteral("[<game.exe>+0x58]");
        const QString row = buildCommandRowText(f, QStringLiteral("struct"), QStringLiteral("Foo"), false);
        checkRow(row, f, QStringLiteral("struct"), QStringLiteral("Foo"));
    }

    void row_bareIdentifierFormulaIsSpannedWhole() {
        // The defect: "game.exe+0x40" used to be spanned from its "0x", so a
        // click edited "0x40" and the module was lost on commit.
        const QString f = QStringLiteral("game.exe+0x40");
        const QString row = buildCommandRowText(f, QStringLiteral("struct"), QStringLiteral("Foo"), false);
        checkRow(row, f, QStringLiteral("struct"), QStringLiteral("Foo"));
        // A symbol, and a formula with spaces, read the same way.
        const QString sym = QStringLiteral("ntdll!LdrpHeap + 8");
        checkRow(buildCommandRowText(sym, QStringLiteral("struct"), QStringLiteral("Foo"), false),
                 sym, QStringLiteral("struct"), QStringLiteral("Foo"));
    }

    void row_braceWrapOnAndOff() {
        const QString off = buildCommandRowText(QStringLiteral("0x10"), QStringLiteral("struct"),
                                                QStringLiteral("Player"), false);
        const QString on  = buildCommandRowText(QStringLiteral("0x10"), QStringLiteral("struct"),
                                                QStringLiteral("Player"), true);
        QCOMPARE(off, QStringLiteral("[▸] 0x10  struct Player {"));
        QCOMPARE(on,  QStringLiteral("[▸] 0x10  struct Player"));
        checkRow(off, QStringLiteral("0x10"), QStringLiteral("struct"), QStringLiteral("Player"));
        checkRow(on,  QStringLiteral("0x10"), QStringLiteral("struct"), QStringLiteral("Player"));
    }

    void row_enumKeyword() {
        const QString row = buildCommandRowText(QStringLiteral("0x20"), QStringLiteral("enum"),
                                                QStringLiteral("Color"), false);
        checkRow(row, QStringLiteral("0x20"), QStringLiteral("enum"), QStringLiteral("Color"));
    }

    void row_elidedAddress() {
        // 24 chars fit as they are; the 25th turns the cell into 23 + "…",
        // and the span covers the elided text, ellipsis included.
        const QString fits = QStringLiteral("<a_module_name.exe>+0x40");     // 24 chars
        QCOMPARE(fits.size(), kCommandRowAddrMaxChars);
        checkRow(buildCommandRowText(fits, QStringLiteral("struct"), QStringLiteral("Foo"), false),
                 fits, QStringLiteral("struct"), QStringLiteral("Foo"));

        const QString longF = QStringLiteral("<a_longer_module_name.exe>+0x1A0");
        QVERIFY(longF.size() > kCommandRowAddrMaxChars);
        const QString elided = longF.left(kCommandRowAddrMaxChars - 1) + QChar(0x2026);
        QCOMPARE(commandRowElide(longF, kCommandRowAddrMaxChars), elided);
        QCOMPARE(elided.size(), kCommandRowAddrMaxChars);
        const QString row = buildCommandRowText(longF, QStringLiteral("struct"), QStringLiteral("Foo"), false);
        QCOMPARE(row, QStringLiteral("[▸] ") + elided + QStringLiteral("  struct Foo {"));
        checkRow(row, elided, QStringLiteral("struct"), QStringLiteral("Foo"));
    }

    void elide_edges() {
        QCOMPARE(commandRowElide(QStringLiteral("abc"), 0), QString());
        QCOMPARE(commandRowElide(QStringLiteral("abc"), 1), QStringLiteral("…"));
        QCOMPARE(commandRowElide(QStringLiteral("abc"), 3), QStringLiteral("abc"));
        QCOMPARE(commandRowElide(QStringLiteral("abcd"), 3), QStringLiteral("ab…"));
    }

    // ---------------------------------------------------------------
    // The address span on rows the builder does not make
    // ---------------------------------------------------------------

    void span_addressOnlyRow() {
        // The editor tests write rows with no root part at all.
        const QString row = QStringLiteral("[▸] 0xABCD1234");
        const ColumnSpan as = commandRowAddrSpan(row);
        QVERIFY(as.valid);
        QCOMPARE(as.start, 4);
        QCOMPARE(spanText(row, as), QStringLiteral("0xABCD1234"));
        QVERIFY(!commandRowRootNameSpan(row).valid);
    }

    void span_rowWithoutAddressCell() {
        // P7's shape: chevron straight to the keyword. No address cell, the
        // root spans intact — the parser must not hand the header back as
        // an address.
        const QString row = QStringLiteral("[▸] struct Foo {");
        QVERIFY(!commandRowAddrSpan(row).valid);
        QCOMPARE(spanText(row, commandRowRootTypeSpan(row)), QStringLiteral("struct"));
        QCOMPARE(spanText(row, commandRowRootNameSpan(row)), QStringLiteral("Foo"));
        QVERIFY(!commandRowAddrSpan(QStringLiteral("[▸] ")).valid);
        QVERIFY(!commandRowAddrSpan(QString()).valid);
    }

    void span_fallsBackToHexRunForOddPrefix() {
        // Text that opens with none of '<', '[', a letter, a digit or '_'
        // is not a formula shape the parser knows: it takes the "0x" run.
        const QString row = QStringLiteral("[▸] = 0x40  struct Foo {");
        QCOMPARE(spanText(row, commandRowAddrSpan(row)), QStringLiteral("0x40"));
    }

    void span_chevronRejects() {
        QVERIFY(!commandRowChevronSpan(QStringLiteral("Hi")).valid);
        QVERIFY(!commandRowChevronSpan(QStringLiteral("▸ 0x0")).valid);
        QVERIFY(!commandRowChevronSpan(QStringLiteral("[▾] 0x0")).valid);   // the old glyph
    }
};

QTEST_MAIN(TestCommandRow)
#include "test_command_row.moc"
