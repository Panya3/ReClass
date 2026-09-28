// RibbonBar layout / interaction tests. Runs on the hidden desktop: never
// waits for window exposure (show() + qWait + processEvents / grab() is all
// that's needed).
//
// Label modes: the widget (like the app's `ribbonLabels` setting) defaults to
// Auto, which compacts every panel before hiding any — so the overflow tests
// switch to All, where Add/Insert/Selected keep their labels and the panels
// hide behind the » chevron at 760 px.
#include <QtTest/QTest>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFontDatabase>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLineEdit>
#include <QMenu>
#include <QMouseEvent>
#include <QSignalSpy>
#include <QVBoxLayout>

#include "ribbon.h"
#include "themes/theme.h"

using namespace rcx;

namespace {

Theme loadTheme(const QString& baseName) {
    QFile f(QStringLiteral(RCX_SOURCE_DIR) + QStringLiteral("/src/themes/defaults/") + baseName
            + QStringLiteral(".json"));
    if (!f.open(QIODevice::ReadOnly)) return Theme();
    return Theme::fromJson(QJsonDocument::fromJson(f.readAll()).object());
}

QStringList visibleIds(const RibbonBar& bar) {
    QStringList out;
    for (const QString& id : bar.actionIds())
        if (!bar.itemRect(id).isNull()) out << id;
    return out;
}

// Mean distance of the rect's pixels from the body background — a proxy for
// "how much ink is there".
double inkAmount(const QImage& img, const QRect& logical, const QColor& bg) {
    // grab() returns device pixels (1.25× on this machine) — scale the rect.
    const qreal dpr = img.devicePixelRatio() > 0 ? img.devicePixelRatio() : 1.0;
    const QRect r(qRound(logical.left() * dpr), qRound(logical.top() * dpr),
                  qRound(logical.width() * dpr), qRound(logical.height() * dpr));
    double sum = 0; int n = 0;
    for (int y = r.top(); y <= r.bottom() && y < img.height(); ++y)
        for (int x = r.left(); x <= r.right() && x < img.width(); ++x) {
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

}  // namespace

class TestRibbonLayout : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void basics();
    void noOverlapAndThreePerColumn_data();
    void noOverlapAndThreePerColumn();
    void overflowByWidth();
    void overflowMenuListsHiddenActions();
    void overflowMenuIconsForExternalActions();
    void typeLabelsCompactBeforePanelsHide();
    void labelModes();
    void autoDropOrder();
    void setActionSwapAndDisabledDims();
    void hiddenActionRemovesRect();
    void clickTriggersWithoutTakingFocus();
    void tooltipFollowsHover();
    void tabsAndMinimize();
    void doubleClickWhileMinimizedRestoresOnce();
    void lightThemeStillPaints();

private:
    Theme m_dark;
};

void TestRibbonLayout::initTestCase() {
    QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/JetBrainsMono.ttf"));
    m_dark = loadTheme(QStringLiteral("vs"));
    QVERIFY2(m_dark.background.isValid(), "vs.json not found under RCX_SOURCE_DIR/src/themes/defaults");
}

void TestRibbonLayout::basics() {
    RibbonBar bar;
    bar.applyTheme(m_dark);
    QCOMPARE(bar.focusPolicy(), Qt::NoFocus);
    QCOMPARE(bar.labelMode(), RibbonBar::LabelMode::Auto);   // same default as the app
    QCOMPARE(bar.currentTab(), QStringLiteral("modify"));
    QCOMPARE(bar.tabIds(), QStringList({QStringLiteral("modify"), QStringLiteral("home")}));
    QVERIFY(bar.actionIds().contains(QStringLiteral("type.int32")));
    QVERIFY(bar.actionIds().contains(QStringLiteral("home.tools.scanner")));
    QVERIFY(bar.actionIds().contains(QStringLiteral("edit.undo")));
    QCOMPARE(bar.actionIds().count(QStringLiteral("edit.undo")), 1);   // shared by both tabs, one action
    // data payloads the wiring track relies on
    QCOMPARE(bar.action(QStringLiteral("add.1024"))->data().toInt(), 1024);
    QCOMPARE(bar.action(QStringLiteral("insert.8"))->data().toInt(), 8);
    QVERIFY(bar.action(QStringLiteral("type.hex64"))->data().isValid());
    QVERIFY(bar.action(QStringLiteral("type.int32"))->data().toInt()
            != bar.action(QStringLiteral("type.uint32"))->data().toInt());
    QCOMPARE(bar.action(QStringLiteral("type.int32"))->text(), QStringLiteral("Int 32"));
    QVERIFY(!bar.action(QStringLiteral("type.int32"))->icon().isNull());
    QVERIFY(!bar.action(QStringLiteral("sel.delete"))->toolTip().isEmpty());
    // ≈ 96 px at 100 %: tab row + body
    QVERIFY2(bar.preferredHeight() >= 88 && bar.preferredHeight() <= 110,
             qPrintable(QStringLiteral("height %1").arg(bar.preferredHeight())));
    QCOMPARE(bar.sizeHint().height(), bar.preferredHeight());
    QVERIFY(bar.minimumSizeHint().width() < 200);
    bar.resize(1920, bar.preferredHeight());
    QVERIFY(!bar.panelRect(QStringLiteral("type")).isNull());
    QVERIFY(!bar.panelRect(QStringLiteral("add")).isNull());
    QVERIFY(bar.panelRect(QStringLiteral("add")).left() >= 6);
    // The ribbon never owns a static toolTip — the hovered item's text is
    // published on demand (see tooltipFollowsHover).
    QVERIFY(bar.toolTip().isEmpty());
}

void TestRibbonLayout::noOverlapAndThreePerColumn_data() {
    QTest::addColumn<QString>("tab");
    QTest::addColumn<int>("width");
    QTest::addColumn<int>("mode");
    for (const QString& tab : {QStringLiteral("modify"), QStringLiteral("home")})
        for (int w : {1920, 1350, 760})
            for (int mode : {0, 1})
                QTest::newRow(qPrintable(QStringLiteral("%1@%2 %3").arg(tab).arg(w).arg(mode ? "All" : "Auto")))
                    << tab << w << mode;
}

void TestRibbonLayout::noOverlapAndThreePerColumn() {
    QFETCH(QString, tab);
    QFETCH(int, width);
    QFETCH(int, mode);
    RibbonBar bar;
    bar.applyTheme(m_dark);
    bar.setLabelMode(mode ? RibbonBar::LabelMode::All : RibbonBar::LabelMode::Auto);
    bar.setCurrentTab(tab);
    bar.resize(width, bar.preferredHeight());

    const QStringList ids = visibleIds(bar);
    QVERIFY(ids.size() >= 4);
    QHash<int, int> perColumn;
    for (int i = 0; i < ids.size(); ++i) {
        const QRect a = bar.itemRect(ids[i]);
        QVERIFY2(bar.rect().contains(a), qPrintable(QStringLiteral("%1 %2 outside the widget").arg(ids[i]).arg(a.right())));
        QVERIFY2(a.top() >= bar.tabRowHeight(), "item overlaps the tab row");
        QCOMPARE(bar.itemIdAt(a.center()), ids[i]);
        perColumn[bar.itemColumn(ids[i])]++;
        for (int j = i + 1; j < ids.size(); ++j) {
            const QRect b = bar.itemRect(ids[j]);
            QVERIFY2(!a.intersects(b), qPrintable(QStringLiteral("%1 overlaps %2").arg(ids[i], ids[j])));
        }
    }
    for (auto it = perColumn.cbegin(); it != perColumn.cend(); ++it)
        QVERIFY2(it.value() <= 3, qPrintable(QStringLiteral("column %1 holds %2 items").arg(it.key()).arg(it.value())));
    // Overflow chevron (when present) doesn't overlap any item either.
    const QRect ov = bar.overflowButtonRect();
    if (!ov.isNull()) {
        QVERIFY(bar.rect().contains(ov));
        for (const QString& id : ids) QVERIFY(!bar.itemRect(id).intersects(ov));
    }
}

void TestRibbonLayout::overflowByWidth() {
    RibbonBar bar;
    bar.applyTheme(m_dark);
    bar.setLabelMode(RibbonBar::LabelMode::All);
    bar.setCurrentTab(QStringLiteral("modify"));
    bar.resize(1920, bar.preferredHeight());
    QVERIFY2(bar.overflowedPanelIds().isEmpty(), qPrintable(bar.overflowedPanelIds().join(',')));
    QVERIFY(bar.overflowButtonRect().isNull());
    bar.resize(1350, bar.preferredHeight());
    QVERIFY2(bar.overflowedPanelIds().isEmpty(), qPrintable(bar.overflowedPanelIds().join(',')));
    bar.resize(760, bar.preferredHeight());
    const QStringList hidden = bar.overflowedPanelIds();
    QVERIFY2(!hidden.isEmpty(), "expected panels to overflow at 760 px");
    QVERIFY(!hidden.contains(QStringLiteral("type")));
    QVERIFY(!hidden.contains(QStringLiteral("edit")));
    QVERIFY(!bar.panelRect(QStringLiteral("type")).isNull());
    QVERIFY(!bar.overflowButtonRect().isNull());
    for (const QString& p : hidden) QVERIFY(bar.panelRect(p).isNull());
    // Selected goes first, then Insert, then Add.
    QCOMPARE(hidden.first(), QStringLiteral("selected"));
    // Hidden panels' items have no rect.
    QVERIFY(bar.itemRect(QStringLiteral("sel.delete")).isNull());
    // Growing back restores everything.
    bar.resize(1920, bar.preferredHeight());
    QVERIFY(bar.overflowedPanelIds().isEmpty());
    QVERIFY(!bar.itemRect(QStringLiteral("sel.delete")).isNull());
}

void TestRibbonLayout::overflowMenuListsHiddenActions() {
    RibbonBar bar;
    bar.applyTheme(m_dark);
    bar.setLabelMode(RibbonBar::LabelMode::All);
    bar.resize(760, bar.preferredHeight());
    const QStringList hidden = bar.overflowedPanelIds();
    QVERIFY(!hidden.isEmpty());
    QMenu* menu = bar.overflowMenu();
    QVERIFY(menu);
    QCOMPARE(menu->actions().size(), hidden.size());
    QSet<QAction*> listed;
    for (QAction* top : menu->actions()) {
        QVERIFY2(top->menu(), "each hidden panel is a submenu");
        for (QAction* a : top->menu()->actions()) listed.insert(a);
    }
    for (const QString& id : bar.actionIds()) {
        const bool onHiddenPanel = std::any_of(hidden.cbegin(), hidden.cend(), [&](const QString& p) {
            const QString prefix = p == QStringLiteral("selected") ? QStringLiteral("sel.") : p + QLatin1Char('.');
            return id.startsWith(prefix);
        });
        if (onHiddenPanel)
            QVERIFY2(listed.contains(bar.action(id)), qPrintable(id + QStringLiteral(" missing from the overflow menu")));
    }
    // The menu carries the glyph icons too.
    QVERIFY(!bar.action(QStringLiteral("sel.delete"))->icon().isNull());
}

// Regression: MainWindow binds an external RibbonActions QAction (no icon) to
// every Modify id, and the » submenus list effectiveAction(id) — so the menu
// showed no icons at all. The ribbon now stamps its glyph on icon-less
// externals and keeps them in sync across theme changes.
void TestRibbonLayout::overflowMenuIconsForExternalActions() {
    RibbonBar bar;
    bar.applyTheme(m_dark);
    bar.setLabelMode(RibbonBar::LabelMode::All);
    bar.resize(760, bar.preferredHeight());
    QVERIFY(bar.overflowedPanelIds().contains(QStringLiteral("selected")));

    QAction ext(QStringLiteral("Delete"));
    QVERIFY(ext.icon().isNull());
    bar.setAction(QStringLiteral("sel.delete"), &ext);
    QVERIFY2(!ext.icon().isNull(), "icon-less external must receive the ribbon's glyph");
    QCOMPARE(bar.action(QStringLiteral("sel.delete")), &ext);

    QMenu* menu = bar.overflowMenu();
    bool found = false;
    for (QAction* top : menu->actions())
        for (QAction* a : top->menu()->actions())
            if (a == &ext) { found = true; QVERIFY(!a->icon().isNull()); }
    QVERIFY2(found, "external action missing from the Selected submenu");

    // A theme change re-rasterises the supplied icon (new QIcon → new cacheKey).
    const qint64 before = ext.icon().cacheKey();
    bar.applyTheme(loadTheme(QStringLiteral("tw")));
    QVERIFY(ext.icon().cacheKey() != before);
    QVERIFY(!ext.icon().isNull());

    // An external that brings its own icon keeps it, through theme changes too.
    QAction own(QStringLiteral("Duplicate"));
    own.setIcon(QIcon(QStringLiteral(":/vsicons/files.svg")));
    const qint64 ownKey = own.icon().cacheKey();
    bar.setAction(QStringLiteral("sel.duplicate"), &own);
    QCOMPARE(own.icon().cacheKey(), ownKey);
    bar.applyTheme(m_dark);
    QCOMPARE(own.icon().cacheKey(), ownKey);

    // Unbinding stops the re-stamping.
    bar.setAction(QStringLiteral("sel.delete"), nullptr);
    const qint64 detached = ext.icon().cacheKey();
    bar.applyTheme(loadTheme(QStringLiteral("tw")));
    QCOMPARE(ext.icon().cacheKey(), detached);
}

void TestRibbonLayout::typeLabelsCompactBeforePanelsHide() {
    for (auto mode : {RibbonBar::LabelMode::Auto, RibbonBar::LabelMode::All}) {
        RibbonBar bar;
        bar.applyTheme(m_dark);
        bar.setLabelMode(mode);
        bar.resize(1920, bar.preferredHeight());
        QVERIFY(bar.itemLabelShown(QStringLiteral("type.hex64")));
        QVERIFY(bar.itemLabelShown(QStringLiteral("add.1024")));
        const int wide = bar.itemRect(QStringLiteral("type.hex64")).width();
        bar.resize(1350, bar.preferredHeight());
        // Type is icon-only at 1350 (the glyphs are the labels) while every panel stays.
        QVERIFY(!bar.itemLabelShown(QStringLiteral("type.hex64")));
        QVERIFY(bar.itemRect(QStringLiteral("type.hex64")).width() < wide);
        QVERIFY(bar.itemLabelShown(QStringLiteral("add.1024")));
        QVERIFY(bar.overflowedPanelIds().isEmpty());
        // Icon-only items never got a label rect either.
        QVERIFY(!bar.itemLabelShown(QStringLiteral("sel.zero")));
    }
}

void TestRibbonLayout::labelModes() {
    RibbonBar bar;
    bar.applyTheme(m_dark);
    QSignalSpy spy(&bar, &RibbonBar::labelModeChanged);
    bar.resize(1920, bar.preferredHeight());
    QCOMPARE(bar.labelMode(), RibbonBar::LabelMode::Auto);
    bar.setLabelMode(RibbonBar::LabelMode::All);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy.last().at(0).toInt(), 1);
    const int allW = bar.naturalWidth();
    bar.setLabelMode(RibbonBar::LabelMode::IconsOnly);
    QCOMPARE(spy.count(), 2);
    QCOMPARE(spy.last().at(0).toInt(), 2);
    QVERIFY(!bar.itemLabelShown(QStringLiteral("add.1024")));
    QVERIFY(bar.naturalWidth() < allW / 2);
    bar.setLabelMode(RibbonBar::LabelMode::Auto);
    QCOMPARE(spy.count(), 3);
    QCOMPARE(spy.last().at(0).toInt(), 0);
    QVERIFY(bar.itemLabelShown(QStringLiteral("add.1024")));   // room at 1920
    bar.resize(760, bar.preferredHeight());
    // Auto compacts every panel before hiding any, so Modify fits at 760.
    QVERIFY(bar.overflowedPanelIds().isEmpty());
    QVERIFY(!bar.itemLabelShown(QStringLiteral("add.1024")));
    bar.setLabelMode(RibbonBar::LabelMode::Auto);   // no-op → no signal
    QCOMPARE(spy.count(), 3);
}

// Regression: relayout() sorted the stage-1 candidates by dropPriority alone,
// while the header documents "glyphLabels panels first". Pin the documented
// order on both tabs by scanning the width down and recording where each
// panel's labels vanish: Modify = Type, Edit, Add, Insert, Selected;
// Home = Edit, Project, Process, Code, Tools.
void TestRibbonLayout::autoDropOrder() {
    struct Probe { QString tab; QStringList ids; };
    const Probe probes[] = {
        {QStringLiteral("modify"), {QStringLiteral("type.hex64"), QStringLiteral("edit.undo"),
                                    QStringLiteral("add.1024"), QStringLiteral("insert.1024"),
                                    QStringLiteral("sel.duplicate")}},
        {QStringLiteral("home"),   {QStringLiteral("edit.undo"), QStringLiteral("home.project.open"),
                                    QStringLiteral("home.process.goto"), QStringLiteral("home.code.generate"),
                                    QStringLiteral("home.tools.scanner")}},
    };
    for (const Probe& probe : probes) {
        RibbonBar bar;
        bar.applyTheme(m_dark);
        bar.setLabelMode(RibbonBar::LabelMode::Auto);
        bar.setCurrentTab(probe.tab);
        bar.resize(2400, bar.preferredHeight());
        const int fitW = bar.naturalWidth() + 6;   // right margin
        bar.resize(fitW, bar.preferredHeight());
        for (const QString& id : probe.ids)
            QVERIFY2(bar.itemLabelShown(id), qPrintable(probe.tab + QStringLiteral(": ") + id + QStringLiteral(" should still be labelled at the natural width")));
        // One px too narrow: exactly the first candidate loses its labels.
        bar.resize(fitW - 1, bar.preferredHeight());
        QVERIFY2(!bar.itemLabelShown(probe.ids[0]), qPrintable(probe.tab + QStringLiteral(": ") + probe.ids[0] + QStringLiteral(" must drop first")));
        for (int i = 1; i < probe.ids.size(); ++i)
            QVERIFY2(bar.itemLabelShown(probe.ids[i]), qPrintable(probe.tab + QStringLiteral(": ") + probe.ids[i] + QStringLiteral(" dropped too early")));
        // Scan down: the width at which each panel's labels disappear must be
        // strictly decreasing in the documented order.
        QVector<int> gone(probe.ids.size(), -1);
        for (int w = fitW; w >= 300; --w) {
            bar.resize(w, bar.preferredHeight());
            for (int i = 0; i < probe.ids.size(); ++i)
                if (gone[i] < 0 && !bar.itemLabelShown(probe.ids[i])) gone[i] = w;
        }
        for (int i = 0; i < probe.ids.size(); ++i)
            QVERIFY2(gone[i] > 0, qPrintable(probe.ids[i] + QStringLiteral(" never lost its label")));
        for (int i = 1; i < probe.ids.size(); ++i)
            QVERIFY2(gone[i] < gone[i - 1], qPrintable(QStringLiteral("%1: %2 (%3) should drop after %4 (%5)")
                .arg(probe.tab, probe.ids[i]).arg(gone[i]).arg(probe.ids[i - 1]).arg(gone[i - 1])));
    }
}

void TestRibbonLayout::setActionSwapAndDisabledDims() {
    RibbonBar bar;
    bar.applyTheme(m_dark);
    bar.resize(1920, bar.preferredHeight());
    bar.show();
    QTest::qWait(30);
    QApplication::processEvents();

    QAction* own = bar.action(QStringLiteral("type.int32"));
    QVERIFY(own);
    QAction ext(QStringLiteral("External Int 32"));
    bar.setAction(QStringLiteral("type.int32"), &ext);
    QCOMPARE(bar.action(QStringLiteral("type.int32")), &ext);

    const QRect r = bar.itemRect(QStringLiteral("type.int32"));
    QVERIFY(!r.isNull());
    const QImage before = bar.grab().toImage().convertToFormat(QImage::Format_ARGB32);
    const double inkOn = inkAmount(before, r, m_dark.background);
    QVERIFY2(inkOn > 3.0, qPrintable(QStringLiteral("enabled button paints no ink (%1)").arg(inkOn)));

    ext.setEnabled(false);
    QApplication::processEvents();
    const QImage after = bar.grab().toImage().convertToFormat(QImage::Format_ARGB32);
    const double inkOff = inkAmount(after, r, m_dark.background);
    QVERIFY2(inkOff < inkOn * 0.6, qPrintable(QStringLiteral("disabled didn't dim: on=%1 off=%2").arg(inkOn).arg(inkOff)));

    // Click on the disabled one does nothing; restoring the own action works.
    QSignalSpy extSpy(&ext, &QAction::triggered);
    QTest::mouseClick(&bar, Qt::LeftButton, Qt::NoModifier, r.center());
    QCOMPARE(extSpy.count(), 0);
    bar.setAction(QStringLiteral("type.int32"), nullptr);
    QCOMPARE(bar.action(QStringLiteral("type.int32")), own);
}

void TestRibbonLayout::hiddenActionRemovesRect() {
    RibbonBar bar;
    bar.applyTheme(m_dark);
    bar.resize(1920, bar.preferredHeight());
    QVERIFY(!bar.itemRect(QStringLiteral("type.bool")).isNull());
    const QRect floatBefore = bar.itemRect(QStringLiteral("type.float"));
    QAction ext(QStringLiteral("Bool"));
    bar.setAction(QStringLiteral("type.bool"), &ext);
    ext.setVisible(false);
    QVERIFY(bar.itemRect(QStringLiteral("type.bool")).isNull());
    QVERIFY(bar.itemIdAt(floatBefore.center()) == QStringLiteral("type.float"));
    // The own action's visibility works the same way.
    bar.action(QStringLiteral("type.float"))->setVisible(false);
    QVERIFY(bar.itemRect(QStringLiteral("type.float")).isNull());
    bar.action(QStringLiteral("type.float"))->setVisible(true);
    ext.setVisible(true);
    QVERIFY(!bar.itemRect(QStringLiteral("type.bool")).isNull());
    QVERIFY(!bar.itemRect(QStringLiteral("type.float")).isNull());
}

void TestRibbonLayout::clickTriggersWithoutTakingFocus() {
    QWidget win;
    auto* lay = new QVBoxLayout(&win);
    lay->setContentsMargins(0, 0, 0, 0);
    auto* bar = new RibbonBar(&win);
    bar->applyTheme(m_dark);
    auto* edit = new QLineEdit(&win);
    lay->addWidget(bar);
    lay->addWidget(edit);
    win.resize(1920, 300);
    win.show();
    edit->setFocus();
    QTest::qWait(50);
    QApplication::processEvents();
    QCOMPARE(win.focusWidget(), edit);
    QCOMPARE(bar->focusPolicy(), Qt::NoFocus);

    QSignalSpy spy(bar->action(QStringLiteral("type.int32")), &QAction::triggered);
    const QRect r = bar->itemRect(QStringLiteral("type.int32"));
    QVERIFY(!r.isNull());
    QTest::mouseClick(bar, Qt::LeftButton, Qt::NoModifier, r.center());
    QApplication::processEvents();
    QCOMPARE(spy.count(), 1);
    QCOMPARE(win.focusWidget(), edit);
    QVERIFY(!bar->hasFocus());

    // Press inside, release outside → no trigger.
    QTest::mousePress(bar, Qt::LeftButton, Qt::NoModifier, r.center());
    QTest::mouseRelease(bar, Qt::LeftButton, Qt::NoModifier, QPoint(r.right() + 400, r.center().y()));
    QApplication::processEvents();
    QCOMPARE(spy.count(), 1);
    QCOMPARE(win.focusWidget(), edit);
}

// Regression: the ribbon used to handle QEvent::ToolTip itself while the app's
// GlobalTooltipBridge (qApp filter, runs first) dismissed on the empty widget
// toolTip → hide + re-show on every hover tick. The widget toolTip now mirrors
// the hovered item so the bridge (idempotent for same widget + text) owns it.
void TestRibbonLayout::tooltipFollowsHover() {
    RibbonBar bar;
    bar.applyTheme(m_dark);
    bar.setLabelMode(RibbonBar::LabelMode::All);
    bar.resize(1920, bar.preferredHeight());
    bar.show();
    QTest::qWait(30);
    QVERIFY(bar.toolTip().isEmpty());

    const QRect r = bar.itemRect(QStringLiteral("type.int32"));
    hoverAt(bar, r.center());
    QCOMPARE(bar.toolTip(), bar.action(QStringLiteral("type.int32"))->toolTip());
    QVERIFY(!bar.toolTip().isEmpty());

    // Binding an external with a shortcut refreshes the published text in place.
    QAction ext(QStringLiteral("Int 32"));
    ext.setToolTip(QStringLiteral("Retype the selection to int32_t"));
    ext.setShortcut(QKeySequence(QStringLiteral("Ctrl+3")));
    bar.setAction(QStringLiteral("type.int32"), &ext);
    QVERIFY(bar.toolTip().startsWith(QStringLiteral("Retype the selection to int32_t")));
    QVERIFY(bar.toolTip().contains(QKeySequence(QStringLiteral("Ctrl+3")).toString(QKeySequence::NativeText)));
    ext.setToolTip(QStringLiteral("Changed"));
    QVERIFY(bar.toolTip().startsWith(QStringLiteral("Changed")));

    // Another item → its text; the caption strip → none; the tab row → none.
    hoverAt(bar, bar.itemRect(QStringLiteral("sel.delete")).center());
    QCOMPARE(bar.toolTip(), bar.action(QStringLiteral("sel.delete"))->toolTip());
    hoverAt(bar, QPoint(bar.panelRect(QStringLiteral("type")).center().x(),
                        bar.panelRect(QStringLiteral("type")).bottom() - 3));
    QVERIFY(bar.toolTip().isEmpty());
    hoverAt(bar, bar.tabRect(QStringLiteral("home")).center());
    QVERIFY(bar.toolTip().isEmpty());

    // The » chevron.
    bar.resize(760, bar.preferredHeight());
    const QRect ov = bar.overflowButtonRect();
    QVERIFY(!ov.isNull());
    hoverAt(bar, ov.center());
    QCOMPARE(bar.toolTip(), QStringLiteral("More panels"));

    // Leaving clears it.
    QEvent leave(QEvent::Leave);
    QApplication::sendEvent(&bar, &leave);
    QVERIFY(bar.toolTip().isEmpty());
}

void TestRibbonLayout::tabsAndMinimize() {
    RibbonBar bar;
    bar.applyTheme(m_dark);
    bar.resize(1920, bar.preferredHeight());
    bar.show();
    QTest::qWait(30);
    QSignalSpy tabSpy(&bar, &RibbonBar::currentTabChanged);
    QSignalSpy minSpy(&bar, &RibbonBar::minimizedChanged);

    const QRect homeTab = bar.tabRect(QStringLiteral("home"));
    QVERIFY(!homeTab.isNull());
    QVERIFY(homeTab.bottom() < bar.tabRowHeight());
    QTest::mouseClick(&bar, Qt::LeftButton, Qt::NoModifier, homeTab.center());
    QCOMPARE(bar.currentTab(), QStringLiteral("home"));
    QCOMPARE(tabSpy.count(), 1);
    QVERIFY(!bar.itemRect(QStringLiteral("home.tools.scanner")).isNull());
    QVERIFY(bar.itemRect(QStringLiteral("type.int32")).isNull());
    // Large items span the full three rows.
    const QRect large = bar.itemRect(QStringLiteral("home.tools.scanner"));
    const QRect small = bar.itemRect(QStringLiteral("home.process.refresh"));
    QVERIFY(large.height() >= 3 * small.height());
    QVERIFY(large.width() >= 48);

    const int fullH = bar.preferredHeight();
    QTest::mouseDClick(&bar, Qt::LeftButton, Qt::NoModifier, homeTab.center());
    QVERIFY(bar.isMinimized());
    QCOMPARE(minSpy.count(), 1);
    QVERIFY(bar.preferredHeight() < fullH / 2);
    QVERIFY(bar.itemRect(QStringLiteral("home.tools.scanner")).isNull());
    QVERIFY(bar.itemIdAt(large.center()).isEmpty());
    // A plain click on a tab restores the body.
    const QRect modifyTab = bar.tabRect(QStringLiteral("modify"));
    QTest::mouseClick(&bar, Qt::LeftButton, Qt::NoModifier, modifyTab.center());
    QVERIFY(!bar.isMinimized());
    QCOMPARE(bar.currentTab(), QStringLiteral("modify"));
    QCOMPARE(minSpy.count(), 2);
    bar.setCurrentTab(QStringLiteral("nope"));
    QCOMPARE(bar.currentTab(), QStringLiteral("modify"));
}

// Regression: from the minimized state a double-click on a tab restored the
// body on the first press and then re-minimized it on the DblClick event
// (two QMainWindow relayouts, ribbon ends up collapsed again).
void TestRibbonLayout::doubleClickWhileMinimizedRestoresOnce() {
    RibbonBar bar;
    bar.applyTheme(m_dark);
    bar.resize(1920, bar.preferredHeight());
    bar.show();
    QTest::qWait(30);
    bar.setMinimized(true);
    QVERIFY(bar.isMinimized());
    QSignalSpy minSpy(&bar, &RibbonBar::minimizedChanged);
    const int settle = QApplication::doubleClickInterval() + 50;

    // (1) QTest's synthetic double-click: Qt 6 delivers a LONE MouseButtonDblClick
    //     to the widget (no Press first) — verified with an event-filter trace.
    const QPoint homeCenter = bar.tabRect(QStringLiteral("home")).center();
    QTest::mouseDClick(&bar, Qt::LeftButton, Qt::NoModifier, homeCenter);
    QApplication::processEvents();
    QVERIFY2(!bar.isMinimized(), "double-click while minimized must leave the ribbon expanded");
    QCOMPARE(minSpy.count(), 1);
    QCOMPARE(minSpy.last().at(0).toBool(), false);
    QCOMPARE(bar.currentTab(), QStringLiteral("home"));
    QVERIFY(!bar.itemRect(QStringLiteral("home.tools.scanner")).isNull());

    // (2) The real-window sequence: Press (restores) · Release · Press · DblClick · Release.
    bar.setMinimized(true);
    QCOMPARE(minSpy.count(), 2);
    QTest::qWait(settle);
    const QPoint modifyCenter = bar.tabRect(QStringLiteral("modify")).center();
    auto send = [&](QEvent::Type type, Qt::MouseButtons held) {
        QMouseEvent ev(type, modifyCenter, bar.mapToGlobal(modifyCenter), Qt::LeftButton, held, Qt::NoModifier);
        QApplication::sendEvent(&bar, &ev);
    };
    send(QEvent::MouseButtonPress, Qt::LeftButton);
    QVERIFY(!bar.isMinimized());
    QCOMPARE(minSpy.count(), 3);
    send(QEvent::MouseButtonRelease, Qt::NoButton);
    send(QEvent::MouseButtonPress, Qt::LeftButton);
    send(QEvent::MouseButtonDblClick, Qt::LeftButton);
    send(QEvent::MouseButtonRelease, Qt::NoButton);
    QVERIFY2(!bar.isMinimized(), "the DblClick half of the restoring click pair must not re-minimize");
    QCOMPARE(minSpy.count(), 3);
    QCOMPARE(bar.currentTab(), QStringLiteral("modify"));

    // (3) After the double-click interval a double-click minimizes as usual …
    QTest::qWait(settle);
    QTest::mouseDClick(&bar, Qt::LeftButton, Qt::NoModifier, modifyCenter);
    QVERIFY(bar.isMinimized());
    QCOMPARE(minSpy.count(), 4);
    // … and a single restoring click followed later by a double-click still toggles.
    QTest::mouseClick(&bar, Qt::LeftButton, Qt::NoModifier, homeCenter);
    QVERIFY(!bar.isMinimized());
    QCOMPARE(bar.currentTab(), QStringLiteral("home"));
    QCOMPARE(minSpy.count(), 5);
    QTest::qWait(settle);
    QTest::mouseDClick(&bar, Qt::LeftButton, Qt::NoModifier, homeCenter);
    QVERIFY(bar.isMinimized());
    QCOMPARE(minSpy.count(), 6);
}

void TestRibbonLayout::lightThemeStillPaints() {
    // tw.json: text == textDim == textMuted; disabled must still dim via opacity.
    const Theme light = loadTheme(QStringLiteral("tw"));
    QVERIFY(light.background.isValid());
    RibbonBar bar;
    bar.applyTheme(light);
    bar.resize(1920, bar.preferredHeight());
    bar.show();
    QTest::qWait(30);
    const QRect r = bar.itemRect(QStringLiteral("sel.delete"));
    QVERIFY(!r.isNull());
    const QImage on = bar.grab().toImage().convertToFormat(QImage::Format_ARGB32);
    const double inkOn = inkAmount(on, r, light.background);
    QVERIFY(inkOn > 3.0);
    bar.action(QStringLiteral("sel.delete"))->setEnabled(false);
    const QImage off = bar.grab().toImage().convertToFormat(QImage::Format_ARGB32);
    QVERIFY(inkAmount(off, r, light.background) < inkOn * 0.6);
}

QTEST_MAIN(TestRibbonLayout)
#include "test_ribbon_layout.moc"
