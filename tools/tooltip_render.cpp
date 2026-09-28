// Render harness for rcx::RcxTooltip. Tooltips are separate top-level
// windows, so a MainWindow::grab() never contains them — this grabs the
// tooltip widget itself, which is the only way to eyeball what the app
// actually paints.
//
// Renders a short tip, a multi-line tip and a long single-line tip (the case
// that used to be CLIPPED mid-word before recalc() learned to wrap) into one
// contact sheet.
//
// Needs the "windows" platform (the offscreen plugin isn't installed) — run
// it on the hidden desktop via tools/run_tests_hidden.py's run_hidden().
//
// Usage: tooltip_render <out.png> [themeJsonPath|themeName]
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFontDatabase>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QTextStream>

#include "rcxtooltip.h"
#include "themes/theme.h"
#include "themes/thememanager.h"

using namespace rcx;

static bool loadThemeFile(const QString& path, Theme& out) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    if (!doc.isObject()) return false;
    out = Theme::fromJson(doc.object());
    return true;
}

static Theme resolveTheme(const QString& nameOrPath) {
    Theme t = ThemeManager::instance().current();
    if (nameOrPath.isEmpty()) return t;
    if (QFileInfo(nameOrPath).isFile() && loadThemeFile(nameOrPath, t)) return t;
    const QString dir = QStringLiteral(RCX_SOURCE_DIR "/src/themes/defaults");
    const QString guess = dir + QLatin1Char('/') + nameOrPath + QStringLiteral(".json");
    if (QFileInfo(guess).isFile() && loadThemeFile(guess, t)) return t;
    return t;
}

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QTextStream err(stderr);
    if (argc < 2) {
        err << "usage: tooltip_render <out.png> [themeJsonPath|themeName]\n";
        return 2;
    }
    const QString out = QString::fromLocal8Bit(argv[1]);
    const Theme theme = resolveTheme(argc > 2 ? QString::fromLocal8Bit(argv[2]) : QString());

    QFont f(QStringLiteral("JetBrains Mono"), 10);
    f.setFixedPitch(true);

    struct Case { const char* label; QString title; QString body; };
    const QList<Case> cases = {
        {"short",  QString(),                       QStringLiteral("Collapse the ribbon (Ctrl+F1)")},
        {"multi",  QString(),                       QStringLiteral("Extract Class\nCtrl+Shift+B")},
        {"titled", QStringLiteral("UnnamedClass0"), QStringLiteral("field_00  hex64\nfield_08  ptr64\nfield_10  float")},
        // The regression case: one line, far wider than kMaxW. Used to be cut
        // mid-word; must now wrap to several lines inside the same max width.
        {"long",   QString(),
         QStringLiteral("Type keys: P ptr - F float - S int - U uint - left/right "
                        "cycle same-size types - click the segment to go to that "
                        "address (Ctrl+G). Wrapping, not clipping.")},
    };

    QList<QPixmap> shots;
    for (const Case& c : cases) {
        auto* tip = new RcxTooltip(nullptr);
        tip->setTheme(theme.backgroundAlt, theme.border,
                      theme.text, theme.text, theme.border);
        tip->populate(c.title, c.body, f);
        tip->showAt(QPoint(600, 400));
        QApplication::processEvents();
        shots.append(tip->grab());
        err << c.label << ": " << shots.last().width() << "x" << shots.last().height() << "\n";
        tip->dismiss();
        delete tip;
    }

    int w = 0, h = 0;
    for (const QPixmap& p : shots) { w = qMax(w, p.width()); h += p.height() + 12; }
    QPixmap sheet(w + 24, h + 12);
    sheet.fill(theme.background);
    {
        QPainter p(&sheet);
        int y = 12;
        for (const QPixmap& s : shots) { p.drawPixmap(12, y, s); y += s.height() + 12; }
    }
    if (!sheet.save(out)) { err << "failed to write " << out << "\n"; return 1; }
    err << "wrote " << out << "\n";
    return 0;
}
