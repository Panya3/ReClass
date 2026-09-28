// test_ribbon_ids — the two halves of the ribbon agree on their action ids.
//
// MainWindow::createRibbon() binds by iterating RibbonActions::ids() and
// RibbonBar::setAction() silently ignores an id that isn't in the spec; a
// Modify-tab spec item without a RibbonActions counterpart keeps its
// ribbon-owned placeholder QAction (always enabled, no slot). Either drift
// produces a visible button that does nothing, so pin the join here.
#include <QtTest/QTest>
#include <QApplication>
#include <QSet>

#include "controller.h"   // complete RcxController for the QPointer in RibbonActions
#include "ribbon.h"
#include "ribbon_actions.h"
#include "ribbon_spec.h"

using namespace rcx;

class TestRibbonIds : public QObject {
    Q_OBJECT
private slots:
    // DELIBERATE EXCEPTION: `edit.undo` / `edit.redo` are the title-strip
    // quick-access pair (TitleBarWidget::setQuickActions). They stay in
    // RibbonActions because it owns the undo-stack predicates, but they have
    // no ribbon button by design — the Edit panel was two arrows, a caption
    // and a divider spent on commands the menu already carried.
    void testEveryRibbonActionIdExistsInTheSpec() {
        static const QSet<QString> kQuickAccessIds{QStringLiteral("edit.undo"),
                                                   QStringLiteral("edit.redo")};
        RibbonActions acts([]() -> RcxController* { return nullptr; },
                           []() -> RcxEditor* { return nullptr; });
        RibbonBar bar;
        const QStringList specList = bar.actionIds();   // by value: never pair iterators of two temporaries
        const QSet<QString> specIds(specList.begin(), specList.end());
        for (const QString& id : acts.ids()) {
            QVERIFY2(acts.action(id) != nullptr, qPrintable(id));
            if (kQuickAccessIds.contains(id)) {
                QVERIFY2(!specIds.contains(id),
                         qPrintable(QStringLiteral("quick-access id '%1' grew a ribbon button").arg(id)));
                continue;
            }
            QVERIFY2(specIds.contains(id),
                     qPrintable(QStringLiteral("RibbonActions id '%1' has no ribbon button").arg(id)));
        }
    }

    // Every command's words come from ONE place: the spec table. A button and
    // its action can no longer drift apart (Ptr→Class vs "Ptr → Class",
    // "Swap" vs "Big endian", "Class" vs "Extract Class"…).
    void testLabelsAndTooltipsComeFromTheSpec() {
        RibbonActions acts([]() -> RcxController* { return nullptr; },
                           []() -> RcxEditor* { return nullptr; });
        int checked = 0;
        for (const QString& id : acts.ids()) {
            const RibbonItemSpec* spec = ribbonSpecForId(id);
            if (!spec) continue;   // the quick-access pair
            QAction* a = acts.action(id);
            QCOMPARE(a->text(), spec->label);
            QCOMPARE(a->toolTip(), spec->tooltip);
            ++checked;
        }
        QVERIFY2(checked > 40, qPrintable(QStringLiteral("only %1 ids checked").arg(checked)));
        QCOMPARE(acts.action(QStringLiteral("type.hex64"))->text(), QStringLiteral("Hex 64"));
        QCOMPARE(acts.action(QStringLiteral("sel.swap"))->text(), QStringLiteral("Big endian"));
        QVERIFY(acts.action(QStringLiteral("sel.swap"))->isCheckable());
        QCOMPARE(acts.action(QStringLiteral("type.class"))->text(), QStringLiteral("Extract Class"));
    }

    void testEveryModifyButtonHasAnAction() {
        RibbonActions acts([]() -> RcxController* { return nullptr; },
                           []() -> RcxEditor* { return nullptr; });
        const QStringList actList = acts.ids();
        const QSet<QString> actIds(actList.begin(), actList.end());
        int checked = 0;
        for (const RibbonTabSpec& tab : defaultRibbonSpec()) {
            if (tab.id != QLatin1String("modify")) continue;
            for (const RibbonPanelSpec& panel : tab.panels)
                for (const RibbonItemSpec& item : panel.items) {
                    QVERIFY2(actIds.contains(item.id),
                             qPrintable(QStringLiteral("Modify button '%1' (%2) has no RibbonActions action")
                                        .arg(item.id, item.label)));
                    ++checked;
                }
        }
        QVERIFY2(checked > 40, qPrintable(QStringLiteral("only %1 Modify items found").arg(checked)));
    }

    // The bound action must carry the same NodeKind / byte count the widget
    // spec advertises, so the wiring side never retypes to the wrong kind.
    void testDataAgreesWithSpec() {
        RibbonActions acts([]() -> RcxController* { return nullptr; },
                           []() -> RcxEditor* { return nullptr; });
        RibbonBar bar;
        for (const RibbonTabSpec& tab : defaultRibbonSpec()) {
            if (tab.id != QLatin1String("modify")) continue;
            for (const RibbonPanelSpec& panel : tab.panels)
                for (const RibbonItemSpec& item : panel.items) {
                    QAction* a = acts.action(item.id);
                    // Ops without a kind (Ptr->Class, Class, Array, Custom) carry no data on the
                    // action side even when the spec annotates one; compare only when both do.
                    if (!a || !item.data.isValid() || !a->data().isValid()) continue;
                    if (item.id.startsWith(QLatin1String("type.pointer"))
                        || item.id.startsWith(QLatin1String("type.funcptr")))
                        continue;   // 64/32-bit chosen at trigger time from tree.pointerSize
                    QVERIFY2(a->data() == item.data,
                             qPrintable(QStringLiteral("%1: action data %2 != spec data %3")
                                        .arg(item.id, a->data().toString(), item.data.toString())));
                }
        }
    }
};

QTEST_MAIN(TestRibbonIds)
#include "test_ribbon_ids.moc"
