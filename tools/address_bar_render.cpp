// Render harness for rcx::AddressBar (the strip above the command row).
// Builds the bar standalone — no controller, no editor, no NodeTree — feeds
// it a synthetic AddressBarState of 1, 3 and 6 crumbs, and grabs the three
// stacked at 300 / 480 / 760 / 1080 / 1920 logical px so the overflow rule
// (« from 480 down, elided base and source, dropped `up` / `hist` under
// 420 px), the seam, the chip's liveness dot and the tone ladder can be
// eyeballed (1080 = the user's ~1080-logical window at 125 %). Needs the
// "windows" platform (the offscreen plugin isn't installed) — run it on the
// hidden desktop via tools/run_tests_hidden.py's run_hidden(). Set
// QT_SCALE_FACTOR for HiDPI.
//
// Usage: address_bar_render <out-prefix> [themeJsonPath|themeName]
//   themeName matches the "name" field or the basename of a JSON in
//   <exe>/themes or <source>/src/themes/defaults (default: vs.json).
//
// Output: <out-prefix>_<w>.png per width — three bars (1, 3, 6 crumbs) top
// to bottom with a magenta gap between them (never black: black would read
// as an unpainted band) — and, on stdout, per bar the logical itemRect of
// every laid-out id plus the dpr, so a pixel scan of the PNG can crop a
// cell exactly (multiply by the dpr, add the row's device y offset).
//
// The strip's floor is about 395 px with a process source and a formula
// base (nav 48 + divider 13 + chip 110 + root chevron 14 + base 102 + «
// 18 + deepest 68 + recent 22): below it the deepest crumb and `recent`
// are laid out past the right edge — the documented best effort, and what
// the 300-px sheet shows.
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QPixmap>
#include <QTextStream>

#include "themes/theme.h"
#include "widgets/address_bar.h"

using namespace rcx;

static bool loadThemeFile(const QString& path, Theme& out) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject()) return false;
    out = Theme::fromJson(doc.object());
    return out.background.isValid();
}

static QStringList themeDirs() {
    QStringList dirs;
    dirs << QCoreApplication::applicationDirPath() + QStringLiteral("/themes");
#ifdef RCX_SOURCE_DIR
    dirs << QStringLiteral(RCX_SOURCE_DIR) + QStringLiteral("/src/themes/defaults");
#endif
    return dirs;
}

static bool resolveTheme(const QString& arg, Theme& out) {
    if (arg.endsWith(QStringLiteral(".json"), Qt::CaseInsensitive) && QFileInfo::exists(arg))
        return loadThemeFile(arg, out);
    for (const QString& dir : themeDirs()) {
        QDir d(dir);
        if (!d.exists()) continue;
        for (const QString& name : d.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name)) {
            if (QFileInfo(name).completeBaseName().compare(arg, Qt::CaseInsensitive) == 0
                && loadThemeFile(d.filePath(name), out)) return true;
            Theme t;
            if (loadThemeFile(d.filePath(name), t) && t.name.compare(arg, Qt::CaseInsensitive) == 0) {
                out = t;
                return true;
            }
        }
    }
    return false;
}

// A crumb in the production dotted shape ("Class.field" ancestors, a bare
// "Class" deepest); the address is what the per-crumb tooltip prints.
static Crumb crumb(const QString& label, uint64_t index, uint64_t address) {
    Crumb c;
    c.label     = label;
    c.rootId    = index;
    c.classId   = 100 + index;
    c.pointerId = index ? 200 + index : 0;
    c.address   = address;
    c.keyword   = QStringLiteral("class");
    return c;
}

// A live process source over a formula base (so the base segment carries
// its "→ 0x…" resolved-address suffix, the first thing the overflow rule
// takes away) and a trail of `depth` crumbs. Back is on, Forward off, Up
// follows the depth — the cells the app lights at that place.
static AddressBarState stateWithDepth(int depth) {
    AddressBarState s;
    s.sourceName   = QStringLiteral("REECLASS.exe");
    s.sourceKindId = QStringLiteral("processmemory");
    s.liveness     = liveness::Live;
    s.baseAddress  = 0x7FF6DEAD0000ULL;
    s.baseFormula  = QStringLiteral("<REECLASS.exe>+0x1234");
    s.resolvedBase = 0x7FF6DEAD1234ULL;
    s.viewRootId   = 100;
    s.canBack      = true;
    s.canForward   = false;
    s.canUp        = depth > 1;
    static const char* const kTrail[] = {
        "RcxEditor.vptr", "QWidgetPrivate.parent", "QWidget.d_ptr",
        "QObjectPrivate.parent", "QObject.d_ptr", "QObjectData",
    };
    static const char* const kDeepest[] = {
        "RcxEditor", "QWidgetPrivate", "QWidget", "QObjectPrivate", "QObject", "QObjectData",
    };
    const int n = qBound(1, depth, 6);
    for (int i = 0; i < n; ++i) {
        const bool deepest = (i == n - 1);
        s.crumbs << crumb(QString::fromLatin1(deepest ? kDeepest[i] : kTrail[i]), i,
                          s.resolvedBase + 0x100ULL * i);
    }
    s.trailPath = QStringLiteral("RcxEditor");
    for (int i = 0; i + 1 < n; ++i) {
        const QString label = QString::fromLatin1(kTrail[i]);
        s.trailPath += QLatin1Char('.') + label.mid(label.indexOf(QLatin1Char('.')) + 1);
    }
    return s;
}

// Every id the bar can lay out, in strip order; the ones itemRect() answers
// for are the ones laid out at this width.
static QStringList allIds(int crumbCount) {
    QStringList ids{ QStringLiteral("back"), QStringLiteral("fwd"), QStringLiteral("hist"),
                     QStringLiteral("up"), QStringLiteral("src"), QStringLiteral("src.chev"),
                     QStringLiteral("root.chev"), QStringLiteral("base"),
                     QStringLiteral("overflow") };
    for (int i = 0; i < crumbCount; ++i)
        ids << QStringLiteral("crumb:%1").arg(i) << QStringLiteral("chev:%1").arg(i);
    ids << QStringLiteral("space") << QStringLiteral("recent");
    return ids;
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/JetBrainsMono.ttf"));
    QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/IBMPlexMono.ttf"));

    QTextStream out(stdout);
    if (argc < 2) {
        out << "usage: address_bar_render <outPrefix> [themeName|path]\n";
        out.flush();
        return 2;
    }
    const QString prefix = QString::fromLocal8Bit(argv[1]);

    Theme theme;
    bool haveTheme = false;
    if (argc > 2) {
        haveTheme = resolveTheme(QString::fromLocal8Bit(argv[2]), theme);
        if (!haveTheme) out << "theme not found: " << argv[2] << " — using the default\n";
    }
    if (!haveTheme) haveTheme = resolveTheme(QStringLiteral("vs"), theme);

    const int depths[] = { 1, 3, 6 };
    const int gap = 3;   // magenta rows between the stacked bars

    AddressBar probe;
    if (haveTheme) probe.applyTheme(theme);
    out << "theme: " << probe.theme().name << "  dpr: " << probe.devicePixelRatioF()
        << "  height: " << AddressBar::kAddressBarHeight
        << "  font: " << probe.font().family() << " " << probe.font().pointSizeF() << "pt\n";

    for (int w : {300, 480, 760, 1080, 1920}) {
        QVector<QPixmap> rows;
        QStringList reports;
        for (int depth : depths) {
            AddressBar bar;
            if (haveTheme) bar.applyTheme(theme);
            bar.setState(stateWithDepth(depth));
            bar.show();
            bar.resize(w, AddressBar::kAddressBarHeight);
            app.processEvents();
            app.processEvents();
            rows << bar.grab();
            QString rep;
            QTextStream rs(&rep);
            rs << "  crumbs=" << depth << "  segments=[" << bar.segments().join(QLatin1String(" | "))
               << "]  overflowMenu=[" << bar.overflowMenuLabels().join(QLatin1String(" | "))
               << "]  src=\"" << bar.sourceDisplayText() << "\"  base=\"" << bar.baseDisplayText()
               << "\"\n    rects:";
            for (const QString& id : allIds(depth)) {
                const QRect r = bar.itemRect(id);
                if (!r.isNull())
                    rs << " " << id << "=" << r.x() << "," << r.y() << "," << r.width() << "," << r.height();
            }
            const QRect dot = bar.livenessDotRect();
            if (!dot.isNull())
                rs << " dot=" << dot.x() << "," << dot.y() << "," << dot.width() << "," << dot.height();
            reports << rep;
            bar.hide();
        }
        // Stacked in DEVICE pixels: each grab is copied whole at an integer
        // device row, so every bar keeps its own seam row and a logical
        // offset rounded through the dpr can never crop the last one.
        const qreal dpr = rows.first().devicePixelRatio();
        const int gapDev = qRound(gap * dpr);
        int sheetH = gapDev * (rows.size() - 1);
        for (const QPixmap& r : rows) sheetH += r.height();
        QImage sheet(rows.first().width(), sheetH, QImage::Format_ARGB32);
        sheet.fill(QColor(255, 0, 255));
        QVector<int> rowTopDev;
        {
            QPainter sp(&sheet);
            int y = 0;
            for (const QPixmap& r : rows) {
                QImage ri = r.toImage();
                ri.setDevicePixelRatio(1.0);
                sp.drawImage(0, y, ri);
                rowTopDev << y;
                y += ri.height() + gapDev;
            }
        }
        const QString path = QStringLiteral("%1_%2.png").arg(prefix).arg(w);
        sheet.save(path);
        out << path << "  width=" << w << "  dpr=" << dpr << "  rowH=" << AddressBar::kAddressBarHeight
            << "  rowHdev=" << rows.first().height() << "  gapDev=" << gapDev << "\n";
        for (int i = 0; i < reports.size(); ++i)
            out << "  row " << i << " yDev=" << rowTopDev[i] << reports[i] << "\n";
    }
    out.flush();
    return 0;
}
