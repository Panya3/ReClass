// Tone-ladder guard for the shipped themes.
//
// Two rules that the editor's visual hierarchy depends on, and that a theme
// edit can silently break because nothing else reads these tokens together:
//
//   1. The row bands. theme.selected must be BRIGHTER than theme.hover on a
//      dark theme. A hover is transient ("the pointer is here"); a selection
//      is a state that has to survive the pointer moving away, so it needs the
//      stronger step. Three shipped themes had them inverted, which made
//      clicking a row look like the row went dead.
//
//   2. The text ladder textFaint < textMuted < textDim < text. The editor
//      assigns these four tokens to furniture, zero bytes / ASCII preview,
//      the type column and the data respectively (IND_HEX_DIM, IND_ZERO,
//      IND_HEX_TYPE, lexer default). If a theme reorders them the document's
//      reading order inverts even though every individual colour looks fine.
//
// The JSON is parsed directly rather than going through ThemeManager: this is
// a guard on the shipped DATA, and it must fail even if the loader gains a
// fallback that papers over a missing or malformed value.
#include <QtTest/QtTest>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>

namespace {

// WCAG relative luminance. sRGB→linear per channel, then the standard weights.
double luminance(const QColor& c) {
    auto lin = [](double v) {
        v /= 255.0;
        return v <= 0.04045 ? v / 12.92 : std::pow((v + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * lin(c.red()) + 0.7152 * lin(c.green()) + 0.0722 * lin(c.blue());
}

QColor tokenOf(const QJsonObject& o, const char* key) {
    return QColor(o.value(QLatin1String(key)).toString());
}

QString describe(const QString& theme, const char* aKey, const QColor& a,
                 const char* bKey, const QColor& b) {
    return QStringLiteral("%1: %2 %3 (lum %4) vs %5 %6 (lum %7)")
        .arg(theme, QLatin1String(aKey), a.name()).arg(luminance(a), 0, 'f', 4)
        .arg(QLatin1String(bKey), b.name()).arg(luminance(b), 0, 'f', 4);
}

} // namespace

class TestThemeTones : public QObject {
    Q_OBJECT

private:
    QVector<QPair<QString, QJsonObject>> m_themes;

private slots:
    void initTestCase() {
        QDir dir(QStringLiteral(RCX_THEME_DIR));
        QVERIFY2(dir.exists(), qPrintable(QStringLiteral("theme dir missing: %1")
                                              .arg(dir.absolutePath())));
        const QStringList files = dir.entryList(QStringList{QStringLiteral("*.json")},
                                                QDir::Files, QDir::Name);
        QVERIFY2(!files.isEmpty(), "no theme JSON files found");
        for (const QString& f : files) {
            QFile file(dir.filePath(f));
            QVERIFY2(file.open(QIODevice::ReadOnly), qPrintable(f));
            QJsonParseError err{};
            const QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &err);
            QVERIFY2(err.error == QJsonParseError::NoError,
                     qPrintable(QStringLiteral("%1: %2").arg(f, err.errorString())));
            QVERIFY2(doc.isObject(), qPrintable(f));
            m_themes.append({f, doc.object()});
        }
        qDebug() << "themes under test:" << m_themes.size();
    }

    // Every tone token the editor reads must exist and parse. A missing key
    // reads back as an invalid QColor, which QColor silently renders black —
    // exactly the kind of failure the ladder test below would then "pass".
    void testEveryToneTokenParses() {
        static const char* keys[] = { "background", "backgroundAlt", "hover",
                                      "selected", "text", "textDim",
                                      "textMuted", "textFaint", "border" };
        for (const auto& t : m_themes)
            for (const char* k : keys)
                QVERIFY2(tokenOf(t.second, k).isValid(),
                         qPrintable(QStringLiteral("%1: missing/invalid '%2'")
                                        .arg(t.first, QLatin1String(k))));
    }

    // Rule 1 — the selection band outranks the hover band on dark themes.
    void testSelectedBandBrighterThanHover() {
        for (const auto& t : m_themes) {
            const QColor bg = tokenOf(t.second, "background");
            if (luminance(bg) >= 0.2) {
                // A light theme's selection is a HUE (tw.json uses a pale
                // blue), not a step up the greyscale, so a luminance
                // comparison says nothing there.
                qDebug() << "skipping light theme" << t.first;
                continue;
            }
            const QColor sel = tokenOf(t.second, "selected");
            const QColor hov = tokenOf(t.second, "hover");
            QVERIFY2(luminance(sel) > luminance(hov),
                     qPrintable(QStringLiteral(
                         "selection band is not brighter than hover — %1")
                         .arg(describe(t.first, "selected", sel, "hover", hov))));
        }
    }

    // Rule 2 — the four text tones stay in order.
    void testTextToneLadder() {
        for (const auto& t : m_themes) {
            const QColor faint = tokenOf(t.second, "textFaint");
            const QColor muted = tokenOf(t.second, "textMuted");
            const QColor dim   = tokenOf(t.second, "textDim");
            const QColor text  = tokenOf(t.second, "text");
            const QColor bg = tokenOf(t.second, "background");
            // The ladder is about CONTRAST AGAINST THE PAPER, not absolute
            // luminance — on a dark theme each step is lighter, on a light one
            // each step is darker, and comparing raw luminance only works for
            // one polarity. Distance from the background works for both.
            //
            // This matters: tw.json used to paint all four tones pure black,
            // which satisfied a non-strict luminance check trivially. But the
            // three dim tiers are used as CHROME FILLS (the scrollbar handle,
            // the zoom slider handle), so "black" drew a solid black bar down
            // the editor in light mode. The ladder is now real in both
            // polarities and this test pins it strictly for every theme.
            const auto contrast = [&bg](const QColor& c) {
                return qAbs(luminance(c) - luminance(bg));
            };
            const auto lt = [&contrast](const QColor& a, const QColor& b) {
                return contrast(a) < contrast(b);
            };
            QVERIFY2(lt(faint, muted),
                     qPrintable(describe(t.first, "textFaint", faint, "textMuted", muted)));
            QVERIFY2(lt(muted, dim),
                     qPrintable(describe(t.first, "textMuted", muted, "textDim", dim)));
            QVERIFY2(lt(dim, text),
                     qPrintable(describe(t.first, "textDim", dim, "text", text)));
        }
    }

    // Rule 4 — focusGlow is a WARNING tone, never the accent. It marks a
    // stale source (the status chip's dot, the address bar's chip dot) and
    // the MCP focus pulse; Theme::fromJson defaults a missing value to
    // borderFocused, which on vs.json IS indHoverSpan — so a dead source
    // read as "selected" in purple. Every shipped theme names it, apart
    // from the accent and readable on the paper.
    void testFocusGlowIsNotTheAccent() {
        for (const auto& t : m_themes) {
            const QColor glow = tokenOf(t.second, "focusGlow");
            QVERIFY2(glow.isValid(),
                     qPrintable(QStringLiteral("%1: focusGlow missing (would default to "
                                               "borderFocused)").arg(t.first)));
            const QColor accent = tokenOf(t.second, "indHoverSpan");
            QVERIFY2(glow != accent,
                     qPrintable(QStringLiteral("%1: focusGlow %2 is the accent")
                                    .arg(t.first, glow.name())));
            const QColor bg = tokenOf(t.second, "background");
            const QColor paper = luminance(bg) >= 0.2 ? QColor(Qt::white) : bg.darker(115);
            const double a = luminance(glow) + 0.05, b = luminance(paper) + 0.05;
            const double ratio = a > b ? a / b : b / a;
            QVERIFY2(ratio >= 2.0,
                     qPrintable(QStringLiteral("%1: focusGlow %2 is illegible on paper %3 (%4:1)")
                                    .arg(t.first, glow.name(), paper.name())
                                    .arg(ratio, 0, 'f', 2)));
        }
    }

    // textFaint is now furniture INK (braces, "};", fold arrows, the footer's
    // byte-count comment) rather than a whole-row wash, so it has to be
    // legible on the editor paper on its own. Reported, not enforced at the
    // 2.6:1 target: two themes cannot reach it without also moving textMuted,
    // which would flatten the ladder above.
    void testFurnitureToneContrastReported() {
        for (const auto& t : m_themes) {
            const QColor bg = tokenOf(t.second, "background");
            if (luminance(bg) >= 0.2) continue;
            // editorPaperColor(): background.darker(115) on a dark theme.
            const QColor paper = bg.darker(115);
            const QColor faint = tokenOf(t.second, "textFaint");
            const double a = luminance(faint) + 0.05, b = luminance(paper) + 0.05;
            const double ratio = a > b ? a / b : b / a;
            qDebug().noquote() << QStringLiteral("%1 textFaint %2 on paper %3 = %4:1")
                                      .arg(t.first, faint.name(), paper.name())
                                      .arg(ratio, 0, 'f', 2);
            QVERIFY2(ratio >= 1.7,
                     qPrintable(QStringLiteral("%1: textFaint %2 is illegible on "
                                               "paper %3 (%4:1)")
                                    .arg(t.first, faint.name(), paper.name())
                                    .arg(ratio, 0, 'f', 2)));
        }
    }
};

QTEST_MAIN(TestThemeTones)
#include "test_theme_tones.moc"
