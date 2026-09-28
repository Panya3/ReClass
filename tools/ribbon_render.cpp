// Render harness for the ReClassEx-style ribbon. Builds a RibbonBar, applies
// a theme, and grabs BOTH tabs at 760 / 1350 / 1920 logical px to PNGs so the
// panel / caption / column metrics and the pixel-glyph icons can be eyeballed
// against the ReClassEx reference. Needs the "windows" platform (the offscreen
// plugin isn't installed) — run it on the hidden desktop via
// tools/run_tests_hidden.py's run_hidden(). Set QT_SCALE_FACTOR for HiDPI.
//
// Usage: ribbon_render <out-prefix> [themeJsonPath|themeName] [labels 0|1|2]
//   themeName matches the "name" field or the basename of a JSON in
//   <exe>/themes or <source>/src/themes/defaults (default: vs.json).
//   labels: 0 = Auto (default -- what the app uses), 1 = All, 2 = IconsOnly.
#include <QApplication>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>

#include "ribbon.h"
#include "themes/theme.h"

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

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/JetBrainsMono.ttf"));
    QFontDatabase::addApplicationFont(QStringLiteral(":/fonts/IBMPlexMono.ttf"));

    QTextStream out(stdout);
    const QString prefix = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("ribbon_render");

    Theme theme;
    bool haveTheme = false;
    if (argc > 2) {
        haveTheme = resolveTheme(QString::fromLocal8Bit(argv[2]), theme);
        if (!haveTheme) out << "theme not found: " << argv[2] << " — using the default\n";
    }
    if (!haveTheme) haveTheme = resolveTheme(QStringLiteral("vs"), theme);

    RibbonBar bar;
    if (haveTheme) bar.applyTheme(theme);
    // Default Auto so the PNGs compact exactly like the app (MainWindow reads
    // `ribbonLabels` with Auto as its default).
    const int mode = argc > 3 ? QString::fromLocal8Bit(argv[3]).toInt() : 0;
    bar.setLabelMode(mode == 1 ? RibbonBar::LabelMode::All
                   : mode == 2 ? RibbonBar::LabelMode::IconsOnly
                               : RibbonBar::LabelMode::Auto);

    out << "theme: " << bar.theme().name << "  dpr: " << bar.devicePixelRatioF()
        << "  tabRow: " << bar.tabRowHeight() << "  body: " << bar.bodyHeight()
        << "  total: " << bar.preferredHeight() << "\n";

    bar.show();
    for (const QString& tab : {QStringLiteral("modify"), QStringLiteral("home")}) {
        bar.setCurrentTab(tab);
        for (int w : {760, 1350, 1920}) {
            bar.resize(w, bar.preferredHeight());
            app.processEvents();
            app.processEvents();
            const QString path = QStringLiteral("%1_%2_%3.png").arg(prefix, tab).arg(w);
            bar.grab().save(path);
            out << path << "  natural=" << bar.naturalWidth()
                << "  overflow=[" << bar.overflowedPanelIds().join(QLatin1Char(',')) << "]"
                << "  typeLabels=" << (bar.itemLabelShown(QStringLiteral("type.hex64")) ? 1 : 0)
                << "\n";
        }
    }
    out.flush();
    return 0;
}
