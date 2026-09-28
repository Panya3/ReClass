// Headless tests for the ribbon's pixel-glyph icons (src/pixelglyphs.h +
// src/ribbon_icons.h). Locks in:
//   - crispness: every ink pixel is fully opaque at dpr 1.0 / 1.25 / 2.0
//     (painted in device pixels, dpr stamped after painting)
//   - the ink bounding box is exactly pixelLabelWidth·s × 5·s and fits the cell
//   - the scale rule ("B" → 2×, "H64" → 1×, "1024" fits 16 px)
//   - the ink colour is exactly the requested family colour
//   - I32 and U32 (signed vs unsigned) are visibly different
//   - family colours clear WCAG 3.0 against the body background on every
//     built-in theme (loaded straight from src/themes/defaults/*.json)
//   - the SourceIn tint really recolours a baked-colour Codicon
#include <QtTest/QTest>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPixmap>

#include "pixelglyphs.h"
#include "ribbon_icons.h"
#include "ribbon_spec.h"
#include "themes/theme.h"

using namespace rcx;

namespace {

struct Bbox { int minX = 1 << 30, minY = 1 << 30, maxX = -1, maxY = -1; int ink = 0;
    int w() const { return maxX < 0 ? 0 : maxX - minX + 1; }
    int h() const { return maxY < 0 ? 0 : maxY - minY + 1; } };

Bbox inkBbox(const QImage& img, int alphaMin = 1) {
    Bbox b;
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x)
            if (qAlpha(img.pixel(x, y)) >= alphaMin) {
                b.minX = qMin(b.minX, x); b.maxX = qMax(b.maxX, x);
                b.minY = qMin(b.minY, y); b.maxY = qMax(b.maxY, y);
                ++b.ink;
            }
    return b;
}

QImage straight(const QPixmap& pm) { return pm.toImage().convertToFormat(QImage::Format_ARGB32); }

QVector<Theme> builtInThemes() {
    QVector<Theme> out;
    QDir dir(QStringLiteral(RCX_SOURCE_DIR) + QStringLiteral("/src/themes/defaults"));
    for (const QString& name : dir.entryList({QStringLiteral("*.json")}, QDir::Files, QDir::Name)) {
        QFile f(dir.filePath(name));
        if (!f.open(QIODevice::ReadOnly)) continue;
        const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
        if (doc.isObject()) out.append(Theme::fromJson(doc.object()));
    }
    return out;
}

Theme darkTheme() {
    for (const Theme& t : builtInThemes())
        if (t.name.contains(QStringLiteral("VS2022"))) return t;
    return builtInThemes().value(0);
}

const char* familyName(GlyphFamily f) {
    switch (f) {
    case GlyphFamily::Hex: return "Hex"; case GlyphFamily::Signed: return "Signed";
    case GlyphFamily::Unsigned: return "Unsigned"; case GlyphFamily::Float: return "Float";
    case GlyphFamily::Text: return "Text"; case GlyphFamily::Pointer: return "Pointer";
    case GlyphFamily::Bits: return "Bits"; case GlyphFamily::Plain: return "Plain";
    }
    return "?";
}

}  // namespace

class TestPixelGlyphs : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void glyphTableHasEveryUsedChar();
    void glyphsAreCrispAndBounded_data();
    void glyphsAreCrispAndBounded();
    void scaleRule();
    void inkColourIsExact();
    void signedDiffersFromUnsigned();
    void compositesFitAndAreCrisp();
    void familyContrastOnAllThemes();
    void sourceInTintWhitensBakedColourCodicon();
    void cacheKeyIncludesEveryInput();

private:
    QVector<Theme> m_themes;
    Theme m_dark;
    QVector<QPair<QString, GlyphFamily>> m_labels;   // every TypeGlyph in the spec
};

void TestPixelGlyphs::initTestCase() {
    m_themes = builtInThemes();
    QVERIFY2(m_themes.size() >= 8, qPrintable(QStringLiteral("expected ≥8 built-in themes, found %1 in %2")
        .arg(m_themes.size()).arg(QStringLiteral(RCX_SOURCE_DIR "/src/themes/defaults"))));
    m_dark = darkTheme();
    for (const RibbonTabSpec& tab : defaultRibbonSpec())
        for (const RibbonPanelSpec& panel : tab.panels)
            for (const RibbonItemSpec& it : panel.items)
                if (it.icon.kind == RibbonIconSpec::Kind::TypeGlyph)
                    m_labels.append({it.icon.arg, it.icon.family});
    QVERIFY(m_labels.size() >= 20);
}

void TestPixelGlyphs::glyphTableHasEveryUsedChar() {
    // Every character of every spec label (and of the fill/number composites)
    // must have a bitmap — an unknown char draws nothing and the button
    // silently loses part of its label.
    QString used;
    for (const auto& l : m_labels) used += l.first;
    used += QStringLiteral("0123456789???FFF000");
    for (const QChar& c : used) {
        const uint16_t g = pixelGlyph3x5(c.toUpper().toLatin1());
        QVERIFY2(g != 0, qPrintable(QStringLiteral("no glyph for '%1'").arg(c)));
    }
    // The measured originals.
    QCOMPARE(pixelGlyph3x5('H'), detail::g3x5("#.#", "#.#", "###", "#.#", "#.#"));
    QCOMPARE(pixelGlyph3x5('I'), detail::g3x5("###", ".#.", ".#.", ".#.", "###"));
    QCOMPARE(pixelGlyph3x5('4'), detail::g3x5("..#", "#.#", "###", "..#", "..#"));
    QCOMPARE(pixelGlyph3x5('x'), uint16_t(0));   // lowercase is not in the table (drawPixelLabel upper-cases)
    QCOMPARE(pixelGlyph3x5('~'), uint16_t(0));
    QCOMPARE(pixelLabelWidth(QStringLiteral("H64")), 11);
    QCOMPARE(pixelLabelWidth(QStringLiteral("1024")), 15);
    QCOMPARE(pixelLabelWidth(QStringLiteral("F")), 3);
}

void TestPixelGlyphs::glyphsAreCrispAndBounded_data() {
    QTest::addColumn<QString>("label");
    QTest::addColumn<int>("family");
    QTest::addColumn<double>("dpr");
    for (const auto& l : m_labels)
        for (double dpr : {1.0, 1.25, 2.0})
            QTest::newRow(qPrintable(QStringLiteral("%1@%2").arg(l.first).arg(dpr)))
                << l.first << int(l.second) << dpr;
}

void TestPixelGlyphs::glyphsAreCrispAndBounded() {
    QFETCH(QString, label);
    QFETCH(int, family);
    QFETCH(double, dpr);
    const int logical = 16;
    const QPixmap pm = typeGlyphIcon(label, GlyphFamily(family), logical, dpr, m_dark);
    QCOMPARE(pm.devicePixelRatio(), dpr);
    const QImage img = straight(pm);
    const int cellDev = qRound(logical * dpr);
    QCOMPARE(img.width(), cellDev);
    QCOMPARE(img.height(), cellDev);

    // Crisp: no partial alpha anywhere.
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x) {
            const int a = qAlpha(img.pixel(x, y));
            QVERIFY2(a == 0 || a == 255, qPrintable(QStringLiteral("%1@%2: alpha %3 at (%4,%5)")
                .arg(label).arg(dpr).arg(a).arg(x).arg(y)));
        }

    const int s = pixelLabelScale(label, cellDev, dpr);
    const Bbox b = inkBbox(img);
    QVERIFY2(b.ink > 0, "glyph rendered nothing");
    QCOMPARE(b.w(), pixelLabelWidth(label) * s);
    QCOMPARE(b.h(), kGlyphH * s);
    QVERIFY(b.minX >= 0 && b.maxX < cellDev && b.minY >= 0 && b.maxY < cellDev);
    // Ink count is a whole multiple of s·s (every bit is a solid s×s block).
    QCOMPARE(b.ink % (s * s), 0);
}

void TestPixelGlyphs::scaleRule() {
    // 1–2 chars start at 2·k, 3+ at k; shrunk until they fit.
    QCOMPARE(pixelLabelScale(QStringLiteral("B"), 16, 1.0), 2);
    QCOMPARE(pixelLabelScale(QStringLiteral("V2"), 16, 1.0), 2);
    QCOMPARE(pixelLabelScale(QStringLiteral("H64"), 16, 1.0), 1);
    QCOMPARE(pixelLabelScale(QStringLiteral("1024"), 16, 1.0), 1);
    QVERIFY(pixelLabelWidth(QStringLiteral("1024")) * 1 <= 16);
    // 125 %: k stays 1 (round(1.25)), the cell is 20 device px.
    QCOMPARE(pixelLabelScale(QStringLiteral("B"), 20, 1.25), 2);
    QCOMPARE(pixelLabelScale(QStringLiteral("H64"), 20, 1.25), 1);
    QCOMPARE(pixelLabelScale(QStringLiteral("WSTR"), 20, 1.25), 1);
    // 200 %: k = 2 → 4× for short labels, 2× for long ones.
    QCOMPARE(pixelLabelScale(QStringLiteral("B"), 32, 2.0), 4);
    QCOMPARE(pixelLabelScale(QStringLiteral("H64"), 32, 2.0), 2);
    QCOMPARE(pixelLabelScale(QStringLiteral("1024"), 32, 2.0), 2);
    // A cell too small for even 1× still returns 1 (never 0).
    QCOMPARE(pixelLabelScale(QStringLiteral("WSTR"), 8, 1.0), 1);
}

void TestPixelGlyphs::inkColourIsExact() {
    for (const auto& l : m_labels) {
        const QColor want = ribbonFamilyColour(l.second, m_dark, m_dark.background);
        const QImage img = straight(typeGlyphIcon(l.first, l.second, 16, 1.0, m_dark));
        long r = 0, g = 0, b = 0; int n = 0;
        for (int y = 0; y < img.height(); ++y)
            for (int x = 0; x < img.width(); ++x) {
                const QRgb px = img.pixel(x, y);
                if (qAlpha(px) == 0) continue;
                r += qRed(px); g += qGreen(px); b += qBlue(px); ++n;
            }
        QVERIFY(n > 0);
        const QColor got(int(r / n), int(g / n), int(b / n));
        QVERIFY2(qAbs(got.red() - want.red()) <= 1 && qAbs(got.green() - want.green()) <= 1
                 && qAbs(got.blue() - want.blue()) <= 1,
                 qPrintable(QStringLiteral("%1 (%2): got %3 want %4").arg(l.first)
                            .arg(familyName(l.second)).arg(got.name()).arg(want.name())));
    }
}

void TestPixelGlyphs::signedDiffersFromUnsigned() {
    const QImage a = straight(typeGlyphIcon(QStringLiteral("I32"), GlyphFamily::Signed, 16, 1.0, m_dark));
    const QImage b = straight(typeGlyphIcon(QStringLiteral("U32"), GlyphFamily::Unsigned, 16, 1.0, m_dark));
    const Bbox ba = inkBbox(a), bb = inkBbox(b);
    const int x0 = qMin(ba.minX, bb.minX), x1 = qMax(ba.maxX, bb.maxX);
    const int y0 = qMin(ba.minY, bb.minY), y1 = qMax(ba.maxY, bb.maxY);
    int diff = 0, total = 0;
    for (int y = y0; y <= y1; ++y)
        for (int x = x0; x <= x1; ++x) {
            ++total;
            if (a.pixel(x, y) != b.pixel(x, y)) ++diff;
        }
    QVERIFY2(diff * 100 > total * 20, qPrintable(QStringLiteral("I32 vs U32: %1/%2 differ").arg(diff).arg(total)));
    // And the colours are different families.
    QVERIFY(ribbonFamilyColour(GlyphFamily::Signed, m_dark, m_dark.background)
            != ribbonFamilyColour(GlyphFamily::Unsigned, m_dark, m_dark.background));
}

void TestPixelGlyphs::compositesFitAndAreCrisp() {
    // 1.5 and 2.5 are the cases where k = round(dpr) rounds UP (cell 24 / 40
    // with k 2 / 3): a 1-digit number at 2k used to land on the plus bar /
    // arrow head. The number must clear the top-left bitmap at every dpr.
    for (double dpr : {1.0, 1.25, 1.5, 2.0, 2.5}) {
        const int cell = qRound(16 * dpr);
        const int k = qMax(1, qRound(dpr));
        for (int n : {4, 8, 64, 1024, 2048}) {
            for (int which = 0; which < 2; ++which) {
                const bool add = (which == 0);
                const QImage img = straight(add ? addBytesIcon(n, 16, dpr, m_dark)
                                                : insertBytesIcon(n, 16, dpr, m_dark));
                const QString tag = QStringLiteral("%1 %2@%3").arg(add ? "add" : "insert").arg(n).arg(dpr);
                QCOMPARE(img.width(), cell);
                const Bbox b = inkBbox(img);
                QVERIFY2(b.ink > 0, qPrintable(tag + QStringLiteral(" empty")));
                for (int y = 0; y < img.height(); ++y)
                    for (int x = 0; x < img.width(); ++x) {
                        const int a = qAlpha(img.pixel(x, y));
                        QVERIFY2(a == 0 || a == 255, qPrintable(tag + QStringLiteral(" alpha %1").arg(a)));
                    }
                // Re-draw the bitmap alone at the position ribbon_icons.h uses
                // (plus at (k,k), hook at (0,k)); whatever ink the composite has
                // beyond it is the number — the two bboxes must not intersect.
                QImage bmOnly(cell, cell, QImage::Format_ARGB32_Premultiplied);
                bmOnly.fill(Qt::transparent);
                {
                    QPainter p(&bmOnly);
                    if (add) drawBitmap(p, k, k, kPlus5x5, k, Qt::white);
                    else     drawBitmap(p, 0, k, kHookArrow7x7, k, Qt::white);
                }
                const QImage bmS = bmOnly.convertToFormat(QImage::Format_ARGB32);
                const Bbox bitmapBox = inkBbox(bmS);
                Bbox numberBox;
                for (int y = 0; y < cell; ++y)
                    for (int x = 0; x < cell; ++x)
                        if (qAlpha(img.pixel(x, y)) == 255 && qAlpha(bmS.pixel(x, y)) == 0) {
                            numberBox.minX = qMin(numberBox.minX, x); numberBox.maxX = qMax(numberBox.maxX, x);
                            numberBox.minY = qMin(numberBox.minY, y); numberBox.maxY = qMax(numberBox.maxY, y);
                            ++numberBox.ink;
                        }
                QVERIFY2(numberBox.ink > 0, qPrintable(tag + QStringLiteral(": number missing")));
                // Every bitmap pixel is present in the composite (nothing painted over it).
                for (int y = 0; y < cell; ++y)
                    for (int x = 0; x < cell; ++x)
                        if (qAlpha(bmS.pixel(x, y)) == 255)
                            QVERIFY2(qAlpha(img.pixel(x, y)) == 255, qPrintable(tag + QStringLiteral(": bitmap pixel lost")));
                const QRect bmRect(bitmapBox.minX, bitmapBox.minY, bitmapBox.w(), bitmapBox.h());
                const QRect numRect(numberBox.minX, numberBox.minY, numberBox.w(), numberBox.h());
                QVERIFY2(!bmRect.intersects(numRect),
                         qPrintable(tag + QStringLiteral(": number bbox %1,%2 %3x%4 overlaps bitmap bbox %5,%6 %7x%8")
                                    .arg(numRect.x()).arg(numRect.y()).arg(numRect.width()).arg(numRect.height())
                                    .arg(bmRect.x()).arg(bmRect.y()).arg(bmRect.width()).arg(bmRect.height())));
                // Number ink is whole s×s blocks and sits inside the cell.
                QVERIFY(numberBox.maxX < cell && numberBox.maxY < cell);
            }
        }
        for (const QImage& img : {straight(deleteIcon(16, dpr, m_dark)),
                                  straight(fillIcon(QStringLiteral("000"), GlyphFamily::Hex, 16, dpr, m_dark)),
                                  straight(fillIcon(QStringLiteral("???"), GlyphFamily::Bits, 16, dpr, m_dark))}) {
            const Bbox b = inkBbox(img);
            QVERIFY(b.ink > 0);
            QVERIFY(b.w() <= cell && b.h() <= cell);
            for (int y = 0; y < img.height(); ++y)
                for (int x = 0; x < img.width(); ++x) {
                    const int a = qAlpha(img.pixel(x, y));
                    QVERIFY(a == 0 || a == 255);
                }
        }
        // Delete ✕ is markerPtr.
        {
            const QImage img = straight(deleteIcon(16, dpr, m_dark));
            const Bbox b = inkBbox(img);
            QCOMPARE(QColor(img.pixel(b.minX, b.minY)).name(), m_dark.markerPtr.name());
        }
        // Ptr → Class carries the red star in the bottom-right corner.
        {
            const QImage img = straight(classPtrIcon(16, dpr, m_dark));
            QVERIFY(inkBbox(img).ink > 0);
            const QRgb corner = img.pixel(img.width() - 1, img.height() - 1);
            QCOMPARE(qAlpha(corner), 255);
            QCOMPARE(QColor(corner).name(), m_dark.markerPtr.name());
        }
    }
}

void TestPixelGlyphs::familyContrastOnAllThemes() {
    const GlyphFamily fams[] = {GlyphFamily::Hex, GlyphFamily::Signed, GlyphFamily::Unsigned,
                                GlyphFamily::Float, GlyphFamily::Text, GlyphFamily::Pointer,
                                GlyphFamily::Bits, GlyphFamily::Plain};
    for (const Theme& t : m_themes) {
        for (GlyphFamily f : fams) {
            const QColor c = ribbonFamilyColour(f, t, t.background);
            const double cr = wcagContrast(c, t.background);
            QVERIFY2(cr >= 3.0, qPrintable(QStringLiteral("%1 / %2: %3 on %4 = %5")
                .arg(t.name).arg(familyName(f)).arg(c.name()).arg(t.background.name()).arg(cr, 0, 'f', 2)));
        }
        // The guard must never hand back the pink error-row background for Signed.
        QVERIFY(ribbonFamilyColour(GlyphFamily::Signed, t, t.background) != t.markerError
                || t.markerError == t.markerPtr);
    }
}

void TestPixelGlyphs::sourceInTintWhitensBakedColourCodicon() {
    // debug-stop.svg ships with fill="#F48771"; the "#C5C5C5" string replace
    // in svgicon.h can't recolour it, SourceIn can.
    const QImage img = straight(tintedSvgIcon(QStringLiteral(":/vsicons/debug-stop.svg"), Qt::white, 16, 1.0));
    int opaque = 0;
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x) {
            const QRgb px = img.pixel(x, y);
            if (qAlpha(px) < 200) continue;
            ++opaque;
            QVERIFY2(qRed(px) >= 250 && qGreen(px) >= 250 && qBlue(px) >= 250,
                     qPrintable(QStringLiteral("pixel (%1,%2) = %3 not white").arg(x).arg(y).arg(QColor(px).name())));
        }
    QVERIFY2(opaque > 10, "debug-stop.svg rendered (almost) nothing — qrc alias missing?");
    // Every Codicon the spec references must resolve (a missing alias renders blank).
    for (const RibbonTabSpec& tab : defaultRibbonSpec())
        for (const RibbonPanelSpec& panel : tab.panels)
            for (const RibbonItemSpec& it : panel.items) {
                if (it.icon.kind != RibbonIconSpec::Kind::Codicon) continue;
                const QImage ic = straight(tintedSvgIcon(QStringLiteral(":/vsicons/") + it.icon.arg
                                                         + QStringLiteral(".svg"), Qt::white, 16, 1.0));
                QVERIFY2(inkBbox(ic, 30).ink > 5, qPrintable(QStringLiteral("%1: Codicon '%2' is blank").arg(it.id, it.icon.arg)));
            }
}

void TestPixelGlyphs::cacheKeyIncludesEveryInput() {
    // Same inputs → same cached pixmap; any pixel-affecting input → a different one.
    const QPixmap a = typeGlyphIcon(QStringLiteral("H64"), GlyphFamily::Hex, 16, 1.0, m_dark);
    const QPixmap b = typeGlyphIcon(QStringLiteral("H64"), GlyphFamily::Hex, 16, 1.0, m_dark);
    QCOMPARE(a.cacheKey(), b.cacheKey());
    QVERIFY(typeGlyphIcon(QStringLiteral("H64"), GlyphFamily::Hex, 16, 1.25, m_dark).cacheKey() != a.cacheKey());
    QVERIFY(typeGlyphIcon(QStringLiteral("H64"), GlyphFamily::Hex, 32, 1.0, m_dark).cacheKey() != a.cacheKey());
    QVERIFY(typeGlyphIcon(QStringLiteral("H64"), GlyphFamily::Signed, 16, 1.0, m_dark).cacheKey() != a.cacheKey());
    Theme light = m_dark;
    light.background = Qt::white;
    light.text = Qt::black;
    QVERIFY(typeGlyphIcon(QStringLiteral("H64"), GlyphFamily::Hex, 16, 1.0, light).cacheKey() != a.cacheKey());
}

QTEST_MAIN(TestPixelGlyphs)
#include "test_pixel_glyphs.moc"
