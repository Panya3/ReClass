#include <QTest>
#include <QString>
#include "core.h"

using namespace rcx;

// The command row (Scintilla line 0) as RcxController::updateCommandRow
// builds it — through buildCommandRowText — parsed back through the SAME
// span functions the editor's hit test, pills and hover read it with. No
// hand copy of the format lives here: a change to the builder or to a
// parser fails these, not a stale replica of them. test_breadcrumb holds
// the other end of the equation: a live controller's line 0 equals the
// builder's output, and stays so when the base changes.
//
// Row shape: "[▸] <keyword> <ClassName> {" — the class header alone. The
// source control and the base address are the address bar's; nothing sits
// between the chevron and the keyword any more, so nothing there is left
// to parse.

namespace {

QString spanText(const QString& row, const ColumnSpan& s) {
    return s.valid ? row.mid(s.start, s.end - s.start) : QString();
}

// Parse a built row back: no source control, no address cell, the keyword
// right after the chevron, the root spans reading as keyword and name.
void checkRow(const QString& row, const QString& keyword, const QString& className) {
    QVERIFY2(!row.contains(QChar(0x25BE)), qPrintable(row));              // no source control
    QVERIFY2(!row.contains(QStringLiteral("0x")), qPrintable(row));        // no address cell
    const ColumnSpan chev = commandRowChevronSpan(row);
    QVERIFY(chev.valid);
    QCOMPARE(chev.start, 0);
    QCOMPARE(chev.end, 4);
    const ColumnSpan rt = commandRowRootTypeSpan(row);
    QVERIFY2(rt.valid, qPrintable(row));
    QCOMPARE(rt.start, chev.end);
    QCOMPARE(spanText(row, rt), keyword);
    QCOMPARE(spanText(row, commandRowRootNameSpan(row)), className);
}

}  // namespace

class TestCommandRow : public QObject {
    Q_OBJECT

private slots:

    // ---------------------------------------------------------------
    // The built row
    // ---------------------------------------------------------------

    void row_structHeader() {
        const QString row = buildCommandRowText(QStringLiteral("struct"), QStringLiteral("Player"), false);
        QCOMPARE(row, QStringLiteral("[▸] struct Player {"));
        checkRow(row, QStringLiteral("struct"), QStringLiteral("Player"));
    }

    void row_untitledIsComposePlaceholderShape() {
        // compose.cpp's line-0 placeholder is this exact string; test_compose
        // checks the other side of the equation against compose() itself.
        const QString row = buildCommandRowText(QStringLiteral("struct"), QStringLiteral("Untitled"), false);
        QCOMPARE(row, QStringLiteral("[▸] struct Untitled {"));
        checkRow(row, QStringLiteral("struct"), QStringLiteral("Untitled"));
    }

    void row_classAndEnumKeywords() {
        const QString cls = buildCommandRowText(QStringLiteral("class"), QStringLiteral("Foo"), false);
        QCOMPARE(cls, QStringLiteral("[▸] class Foo {"));
        checkRow(cls, QStringLiteral("class"), QStringLiteral("Foo"));
        const QString en = buildCommandRowText(QStringLiteral("enum"), QStringLiteral("Color"), false);
        QCOMPARE(en, QStringLiteral("[▸] enum Color {"));
        checkRow(en, QStringLiteral("enum"), QStringLiteral("Color"));
    }

    void row_braceWrapOnAndOff() {
        const QString off = buildCommandRowText(QStringLiteral("struct"), QStringLiteral("Player"), false);
        const QString on  = buildCommandRowText(QStringLiteral("struct"), QStringLiteral("Player"), true);
        QCOMPARE(off, QStringLiteral("[▸] struct Player {"));
        QCOMPARE(on,  QStringLiteral("[▸] struct Player"));
        checkRow(off, QStringLiteral("struct"), QStringLiteral("Player"));
        checkRow(on,  QStringLiteral("struct"), QStringLiteral("Player"));
    }

    void row_nameThatContainsAKeyword() {
        // The root parsers take the keyword right after the chevron and
        // nothing later on the row, so a class name built from one still
        // reads whole.
        for (const QString& name : {QStringLiteral("StructOfClass"), QStringLiteral("my_struct_view"),
                                    QStringLiteral("enum_class_t"), QStringLiteral("_PEB64")}) {
            const QString row = buildCommandRowText(QStringLiteral("struct"), name, false);
            checkRow(row, QStringLiteral("struct"), name);
        }
    }

    void row_nothingBetweenChevronAndKeyword() {
        // The guarantee the demotion made: the text after the chevron IS
        // the header. No address cell, no source label, no elision — a
        // long name is printed whole.
        const QString longName = QStringLiteral("AVeryLongClassNameThatUsedToShareTheRowWithAnAddress");
        for (const QString& kw : {QStringLiteral("struct"), QStringLiteral("class"), QStringLiteral("enum")}) {
            const QString row = buildCommandRowText(kw, longName, false);
            QCOMPARE(row.mid(commandRowChevronSpan(row).end), kw + QLatin1Char(' ') + longName + QStringLiteral(" {"));
            checkRow(row, kw, longName);
        }
    }

    // ---------------------------------------------------------------
    // Rows the builder does not make
    // ---------------------------------------------------------------

    void span_statusRowIsInert() {
        // The MCP status tool writes "[▸] [Claude: text]" onto line 0. With
        // no address cell there is nothing for a parser to anchor on: only
        // the chevron reads, the root spans stay invalid — even when the
        // text carries the old source-control glyph, a hex number, or a
        // class keyword (the parser once anchored on the LAST whole-word
        // keyword anywhere on the row: "renamed class Player" grew a name
        // span "Player]", tinted, editable, and its commit renamed the
        // real root struct).
        for (const QString& row : {QStringLiteral("[▸] [Claude: rebased to 0x1000]"),
                                   QStringLiteral("[▸] [Claude: 'game.exe'▾ 0x1000]"),
                                   QStringLiteral("[▸] [Claude: scanning]"),
                                   QStringLiteral("[▸] [Claude: renamed class Player]"),
                                   QStringLiteral("[▸] [Claude: struct Player rebased]"),
                                   QStringLiteral("[▸] [Claude: enum Color added]")}) {
            QVERIFY2(commandRowChevronSpan(row).valid, qPrintable(row));
            QVERIFY2(!commandRowRootTypeSpan(row).valid, qPrintable(row));
            QVERIFY2(!commandRowRootNameSpan(row).valid, qPrintable(row));
        }
    }

    void span_rootParsersAnchorOnTheChevron() {
        // The keyword is accepted in ONE place: where buildCommandRowText
        // puts it, right at the chevron span's end. Text in front of it,
        // an extra space, or no chevron at all — no root spans. (The
        // inverse of the retired span_rootParsersAnchorOnTheLastKeyword.)
        for (const QString& row : {QStringLiteral("[▸] some text  struct Player {"),
                                   QStringLiteral("[▸]  struct Player {"),
                                   QStringLiteral("[▸] xstruct Player {"),
                                   QStringLiteral("struct Player {"),
                                   QStringLiteral("[▸] struct"),          // keyword without its space
                                   QStringLiteral("[▸] structPlayer {")}) {
            QVERIFY2(!commandRowRootTypeSpan(row).valid, qPrintable(row));
            QVERIFY2(!commandRowRootNameSpan(row).valid, qPrintable(row));
        }
        // …and where it is, a keyword later on the row is part of the name.
        const QString row = QStringLiteral("[▸] struct Player class {");
        QCOMPARE(commandRowRootTypeSpan(row).start, commandRowChevronSpan(row).end);
        QCOMPARE(spanText(row, commandRowRootTypeSpan(row)), QStringLiteral("struct"));
        QCOMPARE(spanText(row, commandRowRootNameSpan(row)), QStringLiteral("Player class"));
    }

    void span_rowWithoutRootPart() {
        // The chevron alone, or nothing: no root spans, no crash.
        for (const QString& row : {QStringLiteral("[▸] "), QStringLiteral("[▸]"), QString()}) {
            QVERIFY2(!commandRowRootTypeSpan(row).valid, qPrintable(row));
            QVERIFY2(!commandRowRootNameSpan(row).valid, qPrintable(row));
        }
        QVERIFY(commandRowChevronSpan(QStringLiteral("[▸] ")).valid);
        QVERIFY(!commandRowChevronSpan(QString()).valid);
    }

    void span_chevronRejects() {
        QVERIFY(!commandRowChevronSpan(QStringLiteral("Hi")).valid);
        QVERIFY(!commandRowChevronSpan(QStringLiteral("▸ struct Foo {")).valid);
        QVERIFY(!commandRowChevronSpan(QStringLiteral("[▾] struct Foo {")).valid);   // the old glyph
    }
};

QTEST_MAIN(TestCommandRow)
#include "test_command_row.moc"
