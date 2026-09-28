// RibbonBar layout / interaction tests. Runs on the hidden desktop: never
// waits for window exposure (show() + qWait + processEvents / grab() is all
// that's needed).
//
// Label modes: the widget (like the app's `ribbonLabels` setting) defaults to
// Auto, which compacts every panel before hiding any — so the overflow tests
// switch to All, where Add/Insert/Selected keep their labels and the panels
// hide behind the … overflow item at 760 px.
//
// Pixel probes (flat ribbon): nothing is filled or outlined at rest, hover is
// the `hover` fill with no outline, checked = a 2-device-row indHoverSpan
// underline, disabled dims the underline too. grab() returns device pixels.
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
#include "ribbon_icons.h"
#include "themes/theme.h"

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

// Rows (device) of `r` that contain at least one pixel of `c`, in order.
QVector<int> rowsWith(const QImage& img, const QRect& r, const QColor& c) {
    QVector<int> rows;
    for (int y = r.top(); y <= r.bottom() && y < img.height(); ++y) {
        bool hit = false;
        for (int x = r.left(); x <= r.right() && x < img.width() && !hit; ++x)
            if (sameColour(img.pixel(x, y), c)) hit = true;
        if (hit) rows << y;
    }
    return rows;
}

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
    void flatMetrics();
    void nothingFilledAtRest();
    void hoverIsFillOnly();
    void checkedUnderline();
    void disabledDimsUnderline();
    void activeTabUnderline();
    void overflowIsMiddleRowItem();

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
    // 89 px at 10 pt: tab row 20 + body 69 (padTop 2 + 3×18 + caption 12 + hairline)
    QVERIFY2(bar.preferredHeight() >= 86 && bar.preferredHeight() <= 92,
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
    // hideOrder: Selected (0) goes first, then Insert (10), then Add (20).
    // Type glyph-only (369) + Edit + Add + Insert (All keeps their labels)
    // ≈ 840 > 760, so exactly Selected + Insert hide.
    QCOMPARE(hidden, QStringList({QStringLiteral("selected"), QStringLiteral("insert")}));
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

// Stage-1 order (labelDrop, higher first; glyphLabels panels first): Modify =
// Type, then Add + Insert together, then Selected; Home = Tools, Code,
// Process, Project. Edit is icon-only for good, so it is not a probe. Pinned
// by scanning the width down and recording where each panel's labels vanish.
void TestRibbonLayout::autoDropOrder() {
    struct Probe { QString tab; QStringList ids; };
    const Probe probes[] = {
        {QStringLiteral("modify"), {QStringLiteral("type.hex64"), QStringLiteral("add.1024"),
                                    QStringLiteral("sel.duplicate")}},
        {QStringLiteral("home"),   {QStringLiteral("home.tools.scanner"), QStringLiteral("home.code.generate"),
                                    QStringLiteral("home.process.goto"), QStringLiteral("home.project.open")}},
    };
    // Edit never shows a label at any width.
    {
        RibbonBar bar;
        bar.applyTheme(m_dark);
        bar.resize(2400, bar.preferredHeight());
        QVERIFY(!bar.itemLabelShown(QStringLiteral("edit.undo")));
        QVERIFY(!bar.itemLabelShown(QStringLiteral("edit.redo")));
        QVERIFY(!bar.itemRect(QStringLiteral("edit.undo")).isNull());
    }
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
        // Equal labelDrop values drop together: Add and Insert lose their
        // labels at the same width.
        if (probe.tab == QStringLiteral("modify")) {
            int addGone = -1, insGone = -1;
            for (int w = fitW; w >= 300 && (addGone < 0 || insGone < 0); --w) {
                bar.resize(w, bar.preferredHeight());
                if (addGone < 0 && !bar.itemLabelShown(QStringLiteral("add.1024"))) addGone = w;
                if (insGone < 0 && !bar.itemLabelShown(QStringLiteral("insert.1024"))) insGone = w;
            }
            QCOMPARE(insGone, addGone);
        }
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

    // The … overflow item.
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

void TestRibbonLayout::flatMetrics() {
    RibbonBar bar;
    bar.applyTheme(m_dark);
    bar.resize(1920, bar.preferredHeight());
    const QFontMetrics fm(bar.font());
    QCOMPARE(bar.tabRowHeight(), fm.height() + 3);
    QCOMPARE(bar.bodyHeight(), 2 + 3 * qMax(18, fm.height() + 1) + 12 + 1);
    QCOMPARE(bar.preferredHeight(), bar.tabRowHeight() + bar.bodyHeight());
    // Tabs: 4 px apart, no overlap.
    const QRect modify = bar.tabRect(QStringLiteral("modify")), home = bar.tabRect(QStringLiteral("home"));
    QCOMPARE(home.left(), modify.right() + 1 + 4);
    QCOMPARE(modify.left(), 6);
    // Panels: 13 px gap; first row starts 2 px under the tab row.
    const QRect edit = bar.panelRect(QStringLiteral("edit")), add = bar.panelRect(QStringLiteral("add"));
    QCOMPARE(add.left(), edit.right() + 1 + 13);
    QCOMPARE(edit.top(), bar.tabRowHeight() + 2);
    QCOMPARE(edit.height(), 3 * qMax(18, fm.height() + 1) + 12);
    // Small rect is colW × 18 with a per-kind icon cell: H64 (24) column wider
    // than the H8 (16) column by exactly the cell difference (glyph-only).
    bar.setLabelMode(RibbonBar::LabelMode::IconsOnly);
    QCOMPARE(bar.itemRect(QStringLiteral("type.hex64")).width(), 24 + 6);
    QCOMPARE(bar.itemRect(QStringLiteral("type.hex8")).width(), 16 + 6);
    QCOMPARE(bar.itemRect(QStringLiteral("type.utf16")).width(), 32 + 6);
    QCOMPARE(bar.itemRect(QStringLiteral("type.class")).width(), 16 + 6);
    QCOMPARE(bar.itemRect(QStringLiteral("type.hex64")).height(), qMax(18, fm.height() + 1));
    bar.setLabelMode(RibbonBar::LabelMode::All);
    QCOMPARE(bar.itemRect(QStringLiteral("type.hex64")).width(),
             3 + 24 + 4 + fm.horizontalAdvance(QStringLiteral("Hex 64")) + 6);
    // Large: clamp(label + 8, 48, 80).
    bar.setCurrentTab(QStringLiteral("home"));
    QCOMPARE(bar.itemRect(QStringLiteral("home.project.open")).width(), 48);
    QCOMPARE(bar.itemRect(QStringLiteral("home.project.newclass")).width(),
             qBound(48, fm.horizontalAdvance(QStringLiteral("New Class")) + 8, 80));
    // Tools rhythm: Scanner + Symbols large, Bookmarks / Console / RTTI one small column.
    QCOMPARE(bar.itemRect(QStringLiteral("home.tools.symbols")).height(), 3 * qMax(18, fm.height() + 1));
    QCOMPARE(bar.itemRect(QStringLiteral("home.tools.console")).height(), qMax(18, fm.height() + 1));
    QCOMPARE(bar.itemColumn(QStringLiteral("home.tools.bookmarks")), bar.itemColumn(QStringLiteral("home.tools.rtti")));
    // Home fits fully labelled in the user's 1080-px window.
    bar.setLabelMode(RibbonBar::LabelMode::Auto);
    bar.resize(1080, bar.preferredHeight());
    QVERIFY2(bar.naturalWidth() + 6 <= 1080, qPrintable(QStringLiteral("Home natural %1").arg(bar.naturalWidth())));
    for (const char* id : {"home.project.newclass", "home.process.goto", "home.code.generate", "home.tools.scanner", "home.tools.rtti"})
        QVERIFY2(bar.itemLabelShown(QLatin1String(id)), id);
    QVERIFY(bar.overflowedPanelIds().isEmpty());
    // … and Modify keeps Add / Insert / Selected labelled with Type glyph-only.
    bar.setCurrentTab(QStringLiteral("modify"));
    QVERIFY(!bar.itemLabelShown(QStringLiteral("type.hex64")));
    QVERIFY(bar.itemLabelShown(QStringLiteral("add.1024")));
    QVERIFY(bar.itemLabelShown(QStringLiteral("insert.1024")));
    QVERIFY(bar.itemLabelShown(QStringLiteral("sel.duplicate")));
    QVERIFY(bar.overflowedPanelIds().isEmpty());
}

// Defects D / E: no panel boxes, no caption bands, no fills or outlines at
// rest. Every pixel between the strip hairline and the body hairline that
// isn't icon / text ink is the body background, and the only `border`-coloured
// pixels in the body are the divider columns (one per panel gap) and the
// family separators.
void TestRibbonLayout::nothingFilledAtRest() {
    RibbonBar bar;
    bar.applyTheme(m_dark);
    bar.setLabelMode(RibbonBar::LabelMode::All);
    bar.resize(1920, bar.preferredHeight());
    bar.show();
    QTest::qWait(30);
    QApplication::processEvents();
    const QImage img = bar.grab().toImage().convertToFormat(QImage::Format_ARGB32);
    const qreal dpr = img.devicePixelRatio() > 0 ? img.devicePixelRatio() : 1.0;

    // No backgroundAlt / selected / hover / indHoverSpan surfaces anywhere in
    // the body. Text anti-aliasing can hit a dark grey by coincidence, so a
    // handful of stray pixels is tolerated — a fill would be thousands.
    const QRect body = devRect(img, QRect(0, bar.tabRowHeight() + 1, bar.width(), bar.bodyHeight() - 2));
    QVERIFY2(countColour(img, body, m_dark.backgroundAlt) < 16, "caption band / panel box painted");
    QVERIFY2(countColour(img, body, m_dark.hover) < 16, "hover fill at rest");
    QVERIFY2(countColour(img, body, m_dark.selected) < 16, "selected fill at rest");
    QVERIFY2(countColour(img, body, m_dark.indHoverSpan) < 16, "accent in the body at rest");

    // A panel's outline rows/cols are plain background (no box): sample the
    // top row, bottom row (caption bottom) and both side columns of Add.
    const QRect add = bar.panelRect(QStringLiteral("add"));
    const QRect addDev = devRect(img, add);
    // Device rows/cols the panel really covers (a rounded devRect can land on
    // the body hairline one row below the caption).
    const int addTop = qFloor(add.top() * dpr + 0.5), addBottom = qFloor((add.bottom() + 1) * dpr - 0.5);
    const int addLeft = qFloor(add.left() * dpr + 0.5), addRight = qFloor((add.right() + 1) * dpr - 0.5);
    QCOMPARE(countColour(img, QRect(addLeft, addTop, addRight - addLeft + 1, 1), m_dark.border), 0);
    QCOMPARE(countColour(img, QRect(addLeft, addBottom, addRight - addLeft + 1, 1), m_dark.border), 0);
    QCOMPARE(countColour(img, QRect(addLeft, addTop, 1, addBottom - addTop + 1), m_dark.border), 0);
    QCOMPARE(countColour(img, QRect(addRight, addTop, 1, addBottom - addTop + 1), m_dark.border), 0);
    // The caption row is background, not a band.
    const QRect capDev = devRect(img, QRect(add.left(), add.bottom() - 11, add.width(), 12));
    QVERIFY(countColour(img, capDev, m_dark.background) > capDev.width() * capDev.height() / 2);

    // Exactly one divider column between Add and Insert, at Add.right()+7,
    // clear of both hairlines (2 px), and nothing else in the gap.
    const QRect ins = bar.panelRect(QStringLiteral("insert"));
    const int gapL = add.right() + 1, gapR = ins.left() - 1;
    QCOMPARE(gapR - gapL + 1, 13);
    int cols = 0;
    const int yMid = qRound((add.top() + 20) * dpr);
    for (int x = qRound(gapL * dpr); x <= qRound((gapR + 1) * dpr); ++x)
        if (sameColour(img.pixel(x, yMid), m_dark.border)) ++cols;
    QCOMPARE(cols, 1);
    const int divX = qFloor((add.right() + 7) * dpr + 0.5);
    QVERIFY(sameColour(img.pixel(divX, yMid), m_dark.border));
    // 2 px clear of the strip hairline: rows tabRowH … tabRowH+1 are background at divX.
    for (int y = qRound(bar.tabRowHeight() * dpr); y < qRound((bar.tabRowHeight() + 2) * dpr); ++y)
        QVERIFY2(!sameColour(img.pixel(divX, y), m_dark.border), qPrintable(QStringLiteral("divider touches the strip hairline at row %1").arg(y)));
    const int bodyBottom = bar.tabRowHeight() + bar.bodyHeight();
    for (int y = qRound((bodyBottom - 3) * dpr); y < qFloor(bodyBottom * dpr - 0.5); ++y)
        QVERIFY2(!sameColour(img.pixel(divX, y), m_dark.border), qPrintable(QStringLiteral("divider touches the body hairline at row %1").arg(y)));
    // No divider before the first panel.
    const QRect edit = bar.panelRect(QStringLiteral("edit"));
    for (int x = 0; x < qRound(edit.left() * dpr); ++x)
        QVERIFY(!sameColour(img.pixel(x, yMid), m_dark.border));
    // Body bottom hairline is one device row, full width.
    const QVector<int> rows = rowsWith(img, QRect(0, qRound((bodyBottom - 2) * dpr), 8, qRound(3 * dpr)), m_dark.border);
    QCOMPARE(rows.size(), 1);
    QCOMPARE(rows.first(), int(qFloor(bodyBottom * dpr - 0.5)));
}

// Hover = the `hover` fill, nothing else: the rect's edge pixels are the hover
// colour (no `border` outline), and the label brightens to `text`.
void TestRibbonLayout::hoverIsFillOnly() {
    RibbonBar bar;
    bar.applyTheme(m_dark);
    bar.setLabelMode(RibbonBar::LabelMode::All);
    bar.resize(1920, bar.preferredHeight());
    bar.show();
    QTest::qWait(30);
    QApplication::processEvents();
    const QRect r = bar.itemRect(QStringLiteral("add.1024"));
    QVERIFY(!r.isNull());
    hoverAt(bar, r.center());
    QApplication::processEvents();
    const QImage img = bar.grab().toImage().convertToFormat(QImage::Format_ARGB32);
    const QRect d = devRect(img, r);
    // Edge rows / cols: never border / indHoverSpan (no outline). Sampled one
    // device px inside the rounded device rect so a .25-phase edge doesn't
    // land on the neighbour's background.
    const QRect in = d.adjusted(1, 1, -1, -1);
    for (const QRect& edge : {QRect(d.left(), d.top(), d.width(), 2), QRect(d.left(), d.bottom() - 1, d.width(), 2),
                              QRect(d.left(), d.top(), 2, d.height()), QRect(d.right() - 1, d.top(), 2, d.height())}) {
        QCOMPARE(countColour(img, edge, m_dark.border), 0);
        QCOMPARE(countColour(img, edge, m_dark.indHoverSpan), 0);
    }
    for (const QRect& edge : {QRect(in.left(), in.top(), in.width(), 1), QRect(in.left(), in.bottom(), in.width(), 1),
                              QRect(in.left(), in.top(), 1, in.height()), QRect(in.right(), in.top(), 1, in.height())})
        QVERIFY2(countColour(img, edge, m_dark.hover) >= (edge.width() * edge.height()) / 2,
                 qPrintable(QStringLiteral("hover edge %1,%2 %3x%4 is not the hover fill (%5)")
                            .arg(edge.x()).arg(edge.y()).arg(edge.width()).arg(edge.height())
                            .arg(countColour(img, edge, m_dark.hover))));
    QVERIFY(countColour(img, d, m_dark.hover) > d.width() * d.height() / 3);
    // Label tone: text on hover, textDim at rest.
    QVERIFY(countColour(img, d, m_dark.text) > 0);
    QEvent leave(QEvent::Leave);
    QApplication::sendEvent(&bar, &leave);
    QApplication::processEvents();
    const QImage rest = bar.grab().toImage().convertToFormat(QImage::Format_ARGB32);
    QCOMPARE(countColour(rest, d, m_dark.hover), 0);
    QCOMPARE(countColour(rest, d, m_dark.text), 0);
    QVERIFY(countColour(rest, d, ribbonToneColour(m_dark.textDim, m_dark, m_dark.background)) > 0);
    // Pressed = `button` fill (vs.json: button != background). A press is
    // always preceded by a move in real life; QTest::mousePress isn't.
    hoverAt(bar, r.center());
    QTest::mousePress(&bar, Qt::LeftButton, Qt::NoModifier, r.center());
    QApplication::processEvents();
    const QImage pressed = bar.grab().toImage().convertToFormat(QImage::Format_ARGB32);
    QVERIFY(countColour(pressed, d, m_dark.button) > d.width() * d.height() / 3);
    QTest::mouseRelease(&bar, Qt::LeftButton, Qt::NoModifier, r.center());
}

// Checked (bound checkable action, checked) = exactly two contiguous device
// rows of indHoverSpan on the item's bottom edge, label in indHoverSpan, no
// fill, no ring.
void TestRibbonLayout::checkedUnderline() {
    RibbonBar bar;
    bar.applyTheme(m_dark);
    bar.setLabelMode(RibbonBar::LabelMode::All);
    bar.setCurrentTab(QStringLiteral("home"));
    bar.resize(1920, bar.preferredHeight());
    bar.show();
    QTest::qWait(30);
    QApplication::processEvents();
    QAction code(QStringLiteral("Code"));
    code.setCheckable(true);
    bar.setAction(QStringLiteral("home.code.codeview"), &code);
    const QRect r = bar.itemRect(QStringLiteral("home.code.codeview"));
    QVERIFY(!r.isNull());
    {
        const QImage off = bar.grab().toImage().convertToFormat(QImage::Format_ARGB32);
        QCOMPARE(countColour(off, devRect(off, r), m_dark.indHoverSpan), 0);
    }
    code.setChecked(true);
    QApplication::processEvents();
    const QImage img = bar.grab().toImage().convertToFormat(QImage::Format_ARGB32);
    const qreal dpr = img.devicePixelRatio() > 0 ? img.devicePixelRatio() : 1.0;
    const QRect d = devRect(img, r);
    // Underline: two contiguous rows ending on the rect's last device row.
    const int xMid = d.left() + d.width() / 2;
    const QVector<int> rows = rowsWith(img, QRect(xMid, d.top(), 1, d.height() + 2), m_dark.indHoverSpan);
    // The label is indHoverSpan too, so only count the rows in the bottom quarter.
    QVector<int> bottom;
    for (int y : rows) if (y >= d.top() + d.height() * 3 / 4) bottom << y;
    QCOMPARE(bottom.size(), 2);
    QCOMPARE(bottom[1], bottom[0] + 1);
    QCOMPARE(bottom[1], int(qFloor((r.bottom() + 1) * dpr - 0.5)));
    // Full width of the item.
    QVERIFY(countColour(img, QRect(d.left(), bottom[0], d.width(), 2), m_dark.indHoverSpan) >= d.width() * 2 - 4);
    // No ring: the top row and side columns carry no accent, no border, no fill.
    QCOMPARE(countColour(img, QRect(d.left(), d.top(), d.width(), 1), m_dark.indHoverSpan), 0);
    QCOMPARE(countColour(img, QRect(d.left(), d.top(), 1, d.height() - 3), m_dark.indHoverSpan), 0);
    QCOMPARE(countColour(img, d, m_dark.border), 0);
    // No fill: the rect is mostly plain background (text AA can hit
    // `selected` by coincidence; a fill would cover the whole rect).
    QVERIFY(countColour(img, d, m_dark.background) > d.width() * d.height() * 6 / 10);
    QVERIFY(countColour(img, d, m_dark.selected) < d.width() * d.height() / 10);
    // Label in the accent.
    QVERIFY(countColour(img, QRect(d.left(), d.top(), d.width(), d.height() - 3), m_dark.indHoverSpan) > 0);
    // Unchecked again: nothing purple.
    code.setChecked(false);
    QApplication::processEvents();
    const QImage off = bar.grab().toImage().convertToFormat(QImage::Format_ARGB32);
    QCOMPARE(countColour(off, devRect(off, r), m_dark.indHoverSpan), 0);
}

// Defect Q: a checked + disabled item used to draw a full-opacity ring over
// 40 % content. Opacity is applied before anything is painted, so the
// underline dims with the icon and label (no pixel keeps the pure accent).
void TestRibbonLayout::disabledDimsUnderline() {
    RibbonBar bar;
    bar.applyTheme(m_dark);
    bar.setCurrentTab(QStringLiteral("home"));
    bar.resize(1920, bar.preferredHeight());
    bar.show();
    QTest::qWait(30);
    QApplication::processEvents();
    QAction both(QStringLiteral("Both"));
    both.setCheckable(true);
    both.setChecked(true);
    bar.setAction(QStringLiteral("home.code.bothview"), &both);
    const QRect r = bar.itemRect(QStringLiteral("home.code.bothview"));
    QVERIFY(!r.isNull());
    const QImage on = bar.grab().toImage().convertToFormat(QImage::Format_ARGB32);
    const QRect d = devRect(on, r);
    QVERIFY(countColour(on, QRect(d.left(), d.bottom() - 2, d.width(), 3), m_dark.indHoverSpan) > d.width());
    both.setEnabled(false);
    QApplication::processEvents();
    const QImage off = bar.grab().toImage().convertToFormat(QImage::Format_ARGB32);
    QCOMPARE(countColour(off, d, m_dark.indHoverSpan), 0);
    // … but the underline is still there, dimmed (not background).
    int dimmed = 0;
    for (int x = d.left(); x <= d.right(); ++x)
        for (int y = d.bottom() - 2; y <= d.bottom(); ++y)
            if (!sameColour(off.pixel(x, y), m_dark.background)) ++dimmed;
    QVERIFY2(dimmed > d.width(), "disabled checked item lost its underline entirely");
    // Hovering a disabled item paints no fill.
    hoverAt(bar, r.center());
    QApplication::processEvents();
    const QImage hov = bar.grab().toImage().convertToFormat(QImage::Format_ARGB32);
    QCOMPARE(countColour(hov, d, m_dark.hover), 0);
}

// Defect F / P: the active tab is exactly two contiguous device rows of
// indHoverSpan on the tab's bottom edge, ending on the strip's hairline row;
// no fill, no box; inactive text textMuted, hover text.
void TestRibbonLayout::activeTabUnderline() {
    RibbonBar bar;
    bar.applyTheme(m_dark);
    bar.resize(1920, bar.preferredHeight());
    bar.show();
    QTest::qWait(30);
    QApplication::processEvents();
    const QImage img = bar.grab().toImage().convertToFormat(QImage::Format_ARGB32);
    const qreal dpr = img.devicePixelRatio() > 0 ? img.devicePixelRatio() : 1.0;
    const QRect active = bar.tabRect(QStringLiteral("modify")), other = bar.tabRect(QStringLiteral("home"));
    const QRect ad = devRect(img, active), od = devRect(img, other);
    const int hairRow = qFloor(bar.tabRowHeight() * dpr - 0.5);
    // Two contiguous accent rows under the active tab, the last on the hairline row.
    const QVector<int> rows = rowsWith(img, QRect(ad.left() + ad.width() / 2, 0, 1, hairRow + 3), m_dark.indHoverSpan);
    QCOMPARE(rows, QVector<int>({hairRow - 1, hairRow}));
    QVERIFY(countColour(img, QRect(ad.left(), hairRow - 1, ad.width(), 2), m_dark.indHoverSpan) >= ad.width() * 2 - 4);
    // The hairline elsewhere is `border`, and there is exactly one such row.
    const QVector<int> hair = rowsWith(img, QRect(od.left() + od.width() / 2, 0, 1, hairRow + 3), m_dark.border);
    QCOMPARE(hair, QVector<int>({hairRow}));
    QCOMPARE(countColour(img, od, m_dark.indHoverSpan), 0);
    // No box: no border / background fill inside the active tab above the underline.
    const QRect inner(ad.left(), 0, ad.width(), hairRow - 1);
    QCOMPARE(countColour(img, inner, m_dark.border), 0);
    QCOMPARE(countColour(img, inner, m_dark.hover), 0);
    QVERIFY(countColour(img, inner, menuBarColor(m_dark)) > inner.width() * inner.height() / 2);
    // Text ladder: inactive textMuted (guarded), active text.
    QVERIFY(countColour(img, od, ribbonToneColour(m_dark.textMuted, m_dark, menuBarColor(m_dark))) > 0);
    QCOMPARE(countColour(img, od, m_dark.text), 0);
    QVERIFY(countColour(img, inner, m_dark.text) > 0);
    hoverAt(bar, other.center());
    QApplication::processEvents();
    const QImage hov = bar.grab().toImage().convertToFormat(QImage::Format_ARGB32);
    QVERIFY(countColour(hov, od, m_dark.text) > 0);
    QCOMPARE(countColour(hov, od, m_dark.hover), 0);   // no hover fill on tabs
    // Minimized: the underline sits on the (only) hairline under the strip.
    bar.setMinimized(true);
    bar.resize(1920, bar.preferredHeight());
    QApplication::processEvents();
    const QImage mini = bar.grab().toImage().convertToFormat(QImage::Format_ARGB32);
    const int miniHair = qFloor((bar.tabRowHeight() + 1) * dpr - 0.5);
    QCOMPARE(rowsWith(mini, QRect(ad.left() + ad.width() / 2, 0, 1, miniHair + 2), m_dark.indHoverSpan),
             QVector<int>({miniHair - 1, miniHair}));
}

// Defect O: the overflow control is a single middle-row 22×18 item to the
// right of a normal divider, not a full-height floating chevron.
void TestRibbonLayout::overflowIsMiddleRowItem() {
    RibbonBar bar;
    bar.applyTheme(m_dark);
    bar.setLabelMode(RibbonBar::LabelMode::All);
    bar.resize(760, bar.preferredHeight());
    QVERIFY(!bar.overflowedPanelIds().isEmpty());
    const QRect ov = bar.overflowButtonRect();
    QCOMPARE(ov.size(), QSize(22, 18));
    QCOMPARE(ov.top(), bar.tabRowHeight() + 2 + 18);   // middle row
    // Right of the last visible panel: divider at right+7, item at divider+5.
    int lastRight = -1;
    for (const QString& id : {QStringLiteral("edit"), QStringLiteral("add"), QStringLiteral("insert"),
                              QStringLiteral("selected"), QStringLiteral("type")})
        if (const QRect r = bar.panelRect(id); !r.isNull()) lastRight = qMax(lastRight, r.right());
    QCOMPARE(ov.left(), lastRight + 7 + 5);
    QVERIFY(ov.right() + 6 <= 760);
    bar.show();
    QTest::qWait(30);
    QApplication::processEvents();
    const QImage img = bar.grab().toImage().convertToFormat(QImage::Format_ARGB32);
    const qreal dpr = img.devicePixelRatio() > 0 ? img.devicePixelRatio() : 1.0;
    const QRect d = devRect(img, ov);
    // Rest: no fill, ink present but never brighter than textDim (the
    // ellipsis dots are anti-aliased circles, so no pixel need be exactly
    // the tone); the divider column is there.
    QCOMPARE(countColour(img, d, m_dark.hover), 0);
    QVERIFY(inkAmount(img, ov, m_dark.background) > 1.0);
    QCOMPARE(countColour(img, d, m_dark.text), 0);
    QVERIFY(sameColour(img.pixel(qFloor((lastRight + 7) * dpr + 0.5), d.top() + d.height() / 2), m_dark.border));
    hoverAt(bar, ov.center());
    QApplication::processEvents();
    const QImage hov = bar.grab().toImage().convertToFormat(QImage::Format_ARGB32);
    QVERIFY(countColour(hov, d, m_dark.hover) > d.width() * d.height() / 3);
    QCOMPARE(countColour(hov, QRect(d.left(), d.top(), d.width(), 1), m_dark.border), 0);
}

QTEST_MAIN(TestRibbonLayout)
#include "test_ribbon_layout.moc"
