// UI regression tests for the ProcessPicker dialog (used by the Process Memory
// provider to pick a target). Constructs the dialog with a fixed custom process
// list — no live process enumeration, no modal exec — so these run headless.
//
// Regression under test: typing a filter that matches nothing and then clearing
// it used to scramble the table. populateTable() filled cells one at a time
// while sorting was enabled, so Qt re-sorted mid-fill and rows ended up with a
// PID but empty Process Name / Path cells (or cells from another process).
#include <QtTest/QTest>
#include <QApplication>
#include <QTableWidget>
#include <QLineEdit>
#include <QHash>
#include <QPair>
#include "processpicker.h"

class TestProcessPicker : public QObject {
    Q_OBJECT

    static QList<ProcessInfo> sampleProcesses() {
        QList<ProcessInfo> procs;
        auto add = [&procs](uint32_t pid, const QString& name, const QString& path) {
            ProcessInfo p;
            p.pid  = pid;
            p.name = name;
            p.path = path;
            procs.append(p);
        };
        add(2000,  "explorer.exe", "C:\\Windows\\explorer.exe");
        add(400,   "svchost.exe",  "C:\\Windows\\System32\\svchost.exe");
        add(30000, "game.exe",     "D:\\Games\\game.exe");
        add(12345, "cmd.exe",      "C:\\Windows\\System32\\cmd.exe");
        add(7,     "winlogon.exe", "C:\\Windows\\System32\\winlogon.exe");
        return procs;
    }

private slots:

    // Regression: filter to zero matches, clear, and every row must come back
    // with PID + Process Name + Path all intact and paired with the right PID.
    void testClearFilterRestoresAllColumns() {
        const QList<ProcessInfo> procs = sampleProcesses();

        ProcessPicker picker(procs);
        auto* filter = picker.findChild<QLineEdit*>("filterEdit");
        auto* table  = picker.findChild<QTableWidget*>("processTable");
        QVERIFY2(filter, "filterEdit not found");
        QVERIFY2(table,  "processTable not found");

        // Sanity: the initial list is fully populated.
        QCOMPARE(table->rowCount(), procs.size());

        // A filter that matches nothing empties the list.
        filter->setText(QStringLiteral("aslclkzxncmlkasmdklaslk"));
        QCOMPARE(table->rowCount(), 0);

        // Clearing the filter must restore every row.
        filter->clear();
        QCOMPARE(table->rowCount(), procs.size());

        QHash<uint32_t, QPair<QString, QString>> expect;
        for (const auto& p : procs)
            expect.insert(p.pid, {p.name, p.path});

        for (int row = 0; row < table->rowCount(); ++row) {
            auto* pidItem  = table->item(row, 0);
            auto* nameItem = table->item(row, 1);
            auto* pathItem = table->item(row, 2);
            QVERIFY2(pidItem,  qPrintable(QString("row %1: PID cell missing").arg(row)));
            QVERIFY2(nameItem, qPrintable(QString("row %1: Name cell missing").arg(row)));
            QVERIFY2(pathItem, qPrintable(QString("row %1: Path cell missing").arg(row)));
            QVERIFY2(!nameItem->text().isEmpty(),
                     qPrintable(QString("row %1: Process Name is empty").arg(row)));
            QVERIFY2(!pathItem->text().isEmpty(),
                     qPrintable(QString("row %1: Path is empty").arg(row)));

            // The three cells on a row must belong to the same process.
            const uint32_t pid = (uint32_t)pidItem->data(Qt::EditRole).toUInt();
            QVERIFY2(expect.contains(pid),
                     qPrintable(QString("row %1: unexpected PID %2").arg(row).arg(pid)));
            QCOMPARE(nameItem->data(Qt::UserRole).toString(), expect.value(pid).first);
            QCOMPARE(pathItem->text(), expect.value(pid).second);
        }
    }

    // Filter that matches a subset still pairs the surviving rows correctly
    // (and the unfiltered restore after narrowing is equally intact).
    void testNarrowingFilterKeepsCellsPaired() {
        const QList<ProcessInfo> procs = sampleProcesses();

        ProcessPicker picker(procs);
        auto* filter = picker.findChild<QLineEdit*>("filterEdit");
        auto* table  = picker.findChild<QTableWidget*>("processTable");
        QVERIFY(filter && table);

        filter->setText(QStringLiteral("system32"));  // matches paths only
        QVERIFY(table->rowCount() > 0 && table->rowCount() < procs.size());

        QHash<uint32_t, QPair<QString, QString>> expect;
        for (const auto& p : procs)
            expect.insert(p.pid, {p.name, p.path});

        for (int row = 0; row < table->rowCount(); ++row) {
            const uint32_t pid = (uint32_t)table->item(row, 0)->data(Qt::EditRole).toUInt();
            QVERIFY(expect.contains(pid));
            QCOMPARE(table->item(row, 1)->data(Qt::UserRole).toString(), expect.value(pid).first);
            QCOMPARE(table->item(row, 2)->text(), expect.value(pid).second);
        }

        filter->clear();
        QCOMPARE(table->rowCount(), procs.size());
        for (int row = 0; row < table->rowCount(); ++row) {
            const uint32_t pid = (uint32_t)table->item(row, 0)->data(Qt::EditRole).toUInt();
            QVERIFY(expect.contains(pid));
            QCOMPARE(table->item(row, 1)->data(Qt::UserRole).toString(), expect.value(pid).first);
            QCOMPARE(table->item(row, 2)->text(), expect.value(pid).second);
        }
    }
};

QTEST_MAIN(TestProcessPicker)
#include "test_process_picker.moc"
