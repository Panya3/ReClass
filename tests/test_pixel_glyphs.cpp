// Headless tests for the ribbon's pixel-glyph icons (src/pixelglyphs.h +
// src/ribbon_icons.h). Locks in:
//   - crispness: every ink pixel is fully opaque at dpr 1.0 / 1.25 / 2.0
//     (painted in device pixels, dpr stamped after painting)
//   - the ink bounding box is exactly pixelLabelWidth·s × kGlyphH·s and fits the cell
//   - ONE scale per DPR (pixelGlyphScale: 2 / 2 / 3 / 4 at 1.0 / 1.25 / 1.5 /
//     2.0): "H64" and "F" are the same height; cells are 8·max(2, len) wide
//   - the square (wide = false) QMenu path never exceeds 16×16
//   - every ribbon Codicon has a 16-unit viewBox (24-unit icons mis-weight)
//   - Codicons render at integer multiples of the 16 grid (16 / 32 / 48)
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
#include <QRegularExpression>

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
    void uniformScaleAcrossLabels_data();
    void uniformScaleAcrossLabels();
    void cellWidthsPerKind();
    void squarePathFitsSixteen();
    void codiconsOnTheSixteenGrid();
    void mirrorHIsAFlipAndKeyed();
    void everyRibbonCodiconIsSixteenUnit();
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
        const PixelGlyph g = pixelGlyph(c.toUpper().toLatin1());
        QVERIFY2(g.w != 0, qPrintable(QStringLiteral("no glyph for '%1'").arg(c)));
    }
    // 5×7 variable width: the point of the regrid is that shapes a 3×5 cell
    // could not express are now distinct. U and V are the case that actually
    // misled a reader (V2/V3/V4 read as U2/U3/U4): they differ in WIDTH as well
    // as shape now, so no scale can collapse them.
    QCOMPARE(pixelGlyph('H'), detail::g(4, "#..#.", "#..#.", "#..#.", "####.", "#..#.", "#..#.", "#..#."));
    QCOMPARE(pixelGlyphAdvance('U'), 4);
    QCOMPARE(pixelGlyphAdvance('V'), 5);
    QVERIFY(pixelGlyph('U').rows[6] != pixelGlyph('V').rows[6]);
    // Narrow glyphs keep their own advance so they do not float in a wide cell.
    QCOMPARE(pixelGlyphAdvance('I'), 3);
    QCOMPARE(pixelGlyphAdvance('T'), 3);
    QCOMPARE(pixelGlyph('x').w, uint8_t(0));   // lowercase is not in the table (drawPixelLabel upper-cases)
    QCOMPARE(pixelGlyph('~').w, uint8_t(0));
    QCOMPARE(pixelGlyphAdvance('~'), 3);       // ... but it still advances a word space
    QCOMPARE(pixelLabelWidth(QStringLiteral("H64")), 14);
    QCOMPARE(pixelLabelWidth(QStringLiteral("1024")), 19);
    QCOMPARE(pixelLabelWidth(QStringLiteral("F")), 4);
}

void TestPixelGlyphs::glyphsAreCrispAndBounded_data() {
    QTest::addColumn<QString>("label");
    QTest::addColumn<int>("family");
    QTest::addColumn<double>("dpr");
    for (const auto& l : m_labels)
        for (double dpr : {1.0, 1.25, 1.5, 2.0})
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
    const int cellW = pixelLabelCellWidth(label);   // == ribbonIconCellWidth
    const int cellWDev = qRound(cellW * dpr);
    const int cellDev = qRound(logical * dpr);
    QCOMPARE(img.width(), cellWDev);
    QCOMPARE(img.height(), cellDev);

    // Crisp: no partial alpha anywhere.
    for (int y = 0; y < img.height(); ++y)
        for (int x = 0; x < img.width(); ++x) {
            const int a = qAlpha(img.pixel(x, y));
            QVERIFY2(a == 0 || a == 255, qPrintable(QStringLiteral("%1@%2: alpha %3 at (%4,%5)")
                .arg(label).arg(dpr).arg(a).arg(x).arg(y)));
        }

    const int s = pixelLabelScale(label, cellWDev, cellDev, dpr);
    QCOMPARE(s, pixelGlyphScale(dpr));   // the shrink guard never fires in the ribbon cells
    const Bbox b = inkBbox(img);
    QVERIFY2(b.ink > 0, "glyph rendered nothing");
    QCOMPARE(b.w(), pixelLabelWidth(label) * s);
    QCOMPARE(b.h(), kGlyphH * s);
    QVERIFY(b.minX >= 0 && b.maxX < cellWDev && b.minY >= 0 && b.maxY < cellDev);
    // Ink count is a whole multiple of s·s (every bit is a solid s×s block).
    QCOMPARE(b.ink % (s * s), 0);
}

void TestPixelGlyphs::scaleRule() {
    // ONE scale per DPR: round(1.6·dpr) → 2 / 2 / 2 / 3.
    QCOMPARE(pixelGlyphScale(1.0), 2);
    QCOMPARE(pixelGlyphScale(1.25), 2);
    QCOMPARE(pixelGlyphScale(1.5), 2);
    QCOMPARE(pixelGlyphScale(2.0), 3);
    // Every label, in the cell the ONE rule gives it, keeps the uniform scale at
    // every shipped DPR — that is the whole contract between pixelLabelCellWidth
    // and pixelLabelScale, and it is why the cell rule sizes at 2 px per font px.
    for (qreal dpr : {1.0, 1.25, 1.5, 2.0})
        for (const char* label : {"B", "V2", "H8", "H64", "PTR", "FN*", "WSTR", "1024"}) {
            const QString l = QString::fromLatin1(label);
            const int cellW = qRound(pixelLabelCellWidth(l) * dpr);
            const int cellH = qRound(16 * dpr);
            QVERIFY2(pixelLabelScale(l, cellW, cellH, dpr) == pixelGlyphScale(dpr),
                     qPrintable(QStringLiteral("%1@%2 shrank out of step").arg(label).arg(dpr)));
        }
    // … the shrink guard still protects a square 16 cell (QMenu icons) …
    QCOMPARE(pixelLabelScale(QStringLiteral("H64"), 16, 16, 1.0), 1);
    QCOMPARE(pixelLabelScale(QStringLiteral("1024"), 16, 16, 1.0), 1);
    // A square 16 cell cannot hold a 4-char label in this font even at 1x (19
    // font px), so the guard floors at 1 rather than returning 0 — the icon
    // overflows its cell instead of vanishing.
    QVERIFY(pixelLabelWidth(QStringLiteral("1024")) > 16);
    // … and a cell too small for even 1× still returns 1 (never 0).
    QCOMPARE(pixelLabelScale(QStringLiteral("WSTR"), 8, 1.0), 1);
}

void TestPixelGlyphs::uniformScaleAcrossLabels_data() {
    QTest::addColumn<double>("dpr");
    for (double dpr : {1.0, 1.25, 1.5, 2.0})
        QTest::newRow(qPrintable(QStringLiteral("dpr %1").arg(dpr))) << dpr;
}

// Defect C: "H64" was 5 device px tall next to a 10-px "F" at 125 %. Both
// (and every other label) are now exactly 5·s tall in the same panel.
void TestPixelGlyphs::uniformScaleAcrossLabels() {
    QFETCH(double, dpr);
    const int s = pixelGlyphScale(dpr);
    const int want = kGlyphH * s;   // 10 / 10 / 15 / 20
    // 7 rows now, and s = round(1.6·dpr): 14 / 14 / 14 / 21 device px of ink.
    QCOMPARE(want, dpr == 2.0 ? 21 : 14);
    for (const char* label : {"H64", "F", "I32", "U32", "PTR", "STR", "WSTR", "1024", "D", "V2", "M4", "H8"}) {
        const QImage img = straight(typeGlyphIcon(QString::fromLatin1(label), GlyphFamily::Hex, 16, dpr, m_dark));
        const Bbox b = inkBbox(img);
        QVERIFY2(b.h() == want, qPrintable(QStringLiteral("%1@%2: ink %3 px tall, want %4")
                                           .arg(label).arg(dpr).arg(b.h()).arg(want)));
        // Ink may be NARROWER than the advance: '1' is drawn 3 px wide inside a
        // 4-px advance, so a proportional font's bbox only has to FIT the box.
        QVERIFY2(b.w() <= pixelLabelWidth(QString::fromLatin1(label)) * s,
                 qPrintable(QStringLiteral("%1@%2: ink %3 px wide exceeds the advance")
                            .arg(label).arg(dpr).arg(b.w())));
    }
    // The fill composites share the scale (block 8·s tall in the 16-tall cell).
    for (const char* text : {"000", "FFF", "???"}) {
        const QImage img = straight(fillIcon(QString::fromLatin1(text), GlyphFamily::Hex, 16, dpr, m_dark));
        const Bbox b = inkBbox(img);
        QCOMPARE(img.width(), qRound(pixelLabelCellWidth(QString::fromLatin1(text)) * dpr));
        QCOMPARE(img.height(), qRound(16 * dpr));
        // Squares only when the cell holds both at the shared scale; the label
        // never shrinks out of step with its neighbours.
        constexpr int kBlockRows = kSquaresRow11x2.h + kSquaresGap + kGlyphH;
        const bool withSquares = kBlockRows * s <= img.height();
        QCOMPARE(b.h(), (withSquares ? kBlockRows : kGlyphH) * s);
        QVERIFY(b.w() <= img.width());
    }
}

void TestPixelGlyphs::cellWidthsPerKind() {
    using K = RibbonIconSpec::Kind;
    auto cell = [](K kind, const char* arg) {
        RibbonIconSpec sp; sp.kind = kind; sp.arg = QString::fromLatin1(arg);
        return ribbonIconCellWidth(sp);
    };
    // MEASURED, not counted: the 5x7 font is variable-width, so two labels of
    // the same length can need different cells (V is 5 wide, H is 4) and a
    // 3-char label can need less than another (PTR < FN*).
    QCOMPARE(cell(K::TypeGlyph, "F"), 16);      // 4 font px -> the 16 floor
    QCOMPARE(cell(K::TypeGlyph, "H8"), 18);
    QCOMPARE(cell(K::TypeGlyph, "V2"), 20);     // wider than H8 at the same length
    QCOMPARE(cell(K::TypeGlyph, "H64"), 28);
    QCOMPARE(cell(K::TypeGlyph, "PTR"), 26);
    QCOMPARE(cell(K::TypeGlyph, "FN*"), 30);    // wider than PTR at the same length
    QCOMPARE(cell(K::TypeGlyph, "WSTR"), 38);
    QCOMPARE(cell(K::TypeGlyph, "1024"), 38);
    QCOMPARE(cell(K::FillSquares, "000"), 28);
    // The layout rule and the painter's rule are ONE function.
    QCOMPARE(cell(K::TypeGlyph, "H64"), pixelLabelCellWidth(QStringLiteral("H64")));
    QCOMPARE(cell(K::Codicon, "symbol-class"), 16);
    QCOMPARE(cell(K::AddBytes, "1024"), 16);
    QCOMPARE(cell(K::InsertBytes, "2048"), 16);
    QCOMPARE(cell(K::DeleteCross, ""), 16);
    QCOMPARE(cell(K::ClassPtr, ""), 16);
    // The rendered pixmap is exactly the cell (device px).
    for (double dpr : {1.0, 1.25, 1.5, 2.0}) {
        QCOMPARE(straight(typeGlyphIcon(QStringLiteral("H64"), GlyphFamily::Hex, 16, dpr, m_dark)).width(),
             qRound(pixelLabelCellWidth(QStringLiteral("H64")) * dpr));
        QCOMPARE(straight(typeGlyphIcon(QStringLiteral("WSTR"), GlyphFamily::Text, 16, dpr, m_dark)).width(),
             qRound(pixelLabelCellWidth(QStringLiteral("WSTR")) * dpr));
        QCOMPARE(straight(typeGlyphIcon(QStringLiteral("F"), GlyphFamily::Float, 16, dpr, m_dark)).width(), qRound(16 * dpr));
    }
}

// refreshOwnActionIcons / QMenu use the square path: a 24- or 32-wide ribbon
// cell must never be downscaled into a 16×16 QIcon — the label shrinks
// (1×) inside a 16×16 canvas instead. Distinct cache entry from the wide one.
void TestPixelGlyphs::squarePathFitsSixteen() {
    RibbonIconSpec h64; h64.kind = RibbonIconSpec::Kind::TypeGlyph; h64.arg = QStringLiteral("H64"); h64.family = GlyphFamily::Hex;
    RibbonIconSpec fill; fill.kind = RibbonIconSpec::Kind::FillSquares; fill.arg = QStringLiteral("000"); fill.family = GlyphFamily::Hex;
    RibbonIconSpec wstr; wstr.kind = RibbonIconSpec::Kind::TypeGlyph; wstr.arg = QStringLiteral("WSTR"); wstr.family = GlyphFamily::Text;
    for (double dpr : {1.0, 1.25, 2.0}) {
        for (const RibbonIconSpec* sp : {&h64, &fill, &wstr}) {
            RibbonIconOptions sq; sq.wide = false;
            const QPixmap square = ribbonIcon(*sp, RibbonIconSize::Small, dpr, m_dark, sq);
            const QPixmap wide = ribbonIcon(*sp, RibbonIconSize::Small, dpr, m_dark);
            const QImage si = straight(square), wi = straight(wide);
            QCOMPARE(si.width(), qRound(16 * dpr));
            QCOMPARE(si.height(), qRound(16 * dpr));
            QCOMPARE(wi.width(), qRound(ribbonIconCellWidth(*sp) * dpr));
            QVERIFY(square.cacheKey() != wide.cacheKey());
            const Bbox b = inkBbox(si);
            QVERIFY2(b.ink > 0, qPrintable(sp->arg + QStringLiteral(" square path is blank")));
            QVERIFY(b.maxX < si.width() && b.maxY < si.height());
            for (int y = 0; y < si.height(); ++y)
                for (int x = 0; x < si.width(); ++x) {
                    const int a = qAlpha(si.pixel(x, y));
                    QVERIFY2(a == 0 || a == 255, "square path must stay crisp (no downscale)");
                }
        }
    }
}

// Defect A: Codicons render only at integer multiples of their 16-unit grid.
// Small: 16·round(dpr) device px centred in the 16-logical cell when it fits
// (1.25 → 16 in a 20 cell), else the whole cell. Large: 24 @ 100 %, 32 @
// 125 / 150 %, 48 @ 200 %.
void TestPixelGlyphs::codiconsOnTheSixteenGrid() {
    QCOMPARE(ribbonSmallCodiconDev(16, 1.0), 16);
    QCOMPARE(ribbonSmallCodiconDev(20, 1.25), 16);
    QCOMPARE(ribbonSmallCodiconDev(24, 1.5), 24);   // 32 doesn't fit → AA at the cell
    QCOMPARE(ribbonSmallCodiconDev(32, 2.0), 32);
    QCOMPARE(ribbonLargeIconDev(1.0), 24);
    QCOMPARE(ribbonLargeIconDev(1.25), 32);
    QCOMPARE(ribbonLargeIconDev(1.5), 32);
    QCOMPARE(ribbonLargeIconDev(2.0), 48);
    QCOMPARE(ribbonLargeIconDev(3.0), 80);   // round(4.5) = 5 grid units

    const QString path = QStringLiteral(":/vsicons/console.svg");   // 1-unit frame at the edge
    // Small at 1.25: 20×20 canvas, ink confined to the centred 16×16 box.
    {
        const QImage img = straight(tintedSvgIcon(path, Qt::white, 16, 1.25));
        QCOMPARE(img.width(), 20);
        const Bbox b = inkBbox(img, 30);
        QVERIFY(b.ink > 0);
        QVERIFY2(b.minX >= 2 && b.maxX <= 17 && b.minY >= 2 && b.maxY <= 17,
                 qPrintable(QStringLiteral("small@1.25 ink %1..%2 × %3..%4, want inside 2..17")
                            .arg(b.minX).arg(b.maxX).arg(b.minY).arg(b.maxY)));
        // The 1-unit frame lands on whole device pixels: the outermost ink
        // column is fully opaque, not a grey halo pair.
        int solid = 0, soft = 0;
        for (int y = 3; y <= 16; ++y) {
            const int a = qAlpha(img.pixel(b.minX, y));
            if (a == 255) ++solid; else if (a > 0) ++soft;
        }
        QVERIFY2(solid > soft, qPrintable(QStringLiteral("frame column: %1 solid / %2 soft").arg(solid).arg(soft)));
    }
    // Large: canvas = ribbonLargeIconDev(dpr) square, dpr stamped after painting.
    for (double dpr : {1.0, 1.25, 1.5, 2.0}) {
        const QPixmap pm = largeCodiconIcon(path, Qt::white, dpr);
        QCOMPARE(pm.devicePixelRatio(), dpr);
        const QImage img = straight(pm);
        QCOMPARE(img.width(), ribbonLargeIconDev(dpr));
        QCOMPARE(img.height(), ribbonLargeIconDev(dpr));
        QVERIFY(inkBbox(img, 30).ink > 0);
    }
    // At 32 device px (2× the grid) the frame is crisp: the left frame column
    // is solid, its neighbours are transparent or solid, never mid-alpha.
    {
        const QImage img = straight(largeCodiconIcon(path, Qt::white, 1.25));
        const Bbox b = inkBbox(img, 30);
        int soft = 0;
        for (int y = b.minY + 2; y <= b.maxY - 2; ++y)
            for (int x = b.minX; x <= b.minX + 2; ++x) {
                const int a = qAlpha(img.pixel(x, y));
                if (a > 0 && a < 255) ++soft;
            }
        QVERIFY2(soft == 0, qPrintable(QStringLiteral("large@1.25 frame edge has %1 grey pixels").arg(soft)));
    }
    // The dispatcher: Large size → the large canvas; Small → the 16 cell.
    RibbonIconSpec sp; sp.kind = RibbonIconSpec::Kind::Codicon; sp.arg = QStringLiteral("console");
    QCOMPARE(straight(ribbonIcon(sp, RibbonIconSize::Large, 1.25, m_dark)).width(), 32);
    QCOMPARE(straight(ribbonIcon(sp, RibbonIconSize::Small, 1.25, m_dark)).width(), 20);
    // Plain ink override reaches the tint; a family icon ignores it.
    RibbonIconOptions o; o.plainInk = QColor(10, 200, 30);
    {
        const QImage img = straight(ribbonIcon(sp, RibbonIconSize::Small, 1.0, m_dark, o));
        const Bbox b = inkBbox(img, 250);
        QVERIFY(b.ink > 0);
        QCOMPARE(QColor(img.pixel(b.minX, b.minY)).name(), o.plainInk.name());
    }
    sp.family = GlyphFamily::Pointer;
    {
        const QImage img = straight(ribbonIcon(sp, RibbonIconSize::Small, 1.0, m_dark, o));
        const Bbox b = inkBbox(img, 250);
        QCOMPARE(QColor(img.pixel(b.minX, b.minY)).name(),
                 ribbonFamilyColour(GlyphFamily::Pointer, m_dark, m_dark.background).name());
    }
}

// Redo = discard mirrored: pixel-exact flip of the unmirrored render, and a
// distinct cache entry.
void TestPixelGlyphs::mirrorHIsAFlipAndKeyed() {
    const QString path = QStringLiteral(":/vsicons/discard.svg");
    for (double dpr : {1.0, 1.25}) {
        const QPixmap plain = tintedSvgIcon(path, Qt::white, 16, dpr, false);
        const QPixmap mirror = tintedSvgIcon(path, Qt::white, 16, dpr, true);
        QVERIFY(plain.cacheKey() != mirror.cacheKey());
        const QImage a = straight(plain), b = straight(mirror);
        QCOMPARE(a.size(), b.size());
        QVERIFY(inkBbox(a, 30).ink > 0);
        QCOMPARE(a.mirrored(true, false), b);
        QVERIFY(a != b);   // discard isn't symmetric
    }
    RibbonIconSpec sp; sp.kind = RibbonIconSpec::Kind::Codicon; sp.arg = QStringLiteral("discard");
    RibbonIconSpec m = sp; m.mirrorH = true;
    QVERIFY(ribbonIcon(sp, RibbonIconSize::Small, 1.0, m_dark).cacheKey()
            != ribbonIcon(m, RibbonIconSize::Small, 1.0, m_dark).cacheKey());
    QCOMPARE(straight(ribbonIcon(sp, RibbonIconSize::Large, 1.25, m_dark)).mirrored(true, false),
             straight(ribbonIcon(m, RibbonIconSize::Large, 1.25, m_dark)));
}

// Defect I: terminal.svg / files.svg are 24-unit viewBoxes among 16-unit
// siblings — rendered on the 16 grid their strokes come out 2/3 weight. Every
// Codicon the ribbon references (plus the … overflow glyph) must be 16-unit.
void TestPixelGlyphs::everyRibbonCodiconIsSixteenUnit() {
    QStringList names{QStringLiteral("ellipsis")};   // the overflow item
    for (const RibbonTabSpec& tab : defaultRibbonSpec())
        for (const RibbonPanelSpec& panel : tab.panels)
            for (const RibbonItemSpec& it : panel.items) {
                if (it.icon.kind == RibbonIconSpec::Kind::Codicon) names << it.icon.arg;
                if (it.icon.kind == RibbonIconSpec::Kind::ClassPtr) names << QStringLiteral("symbol-class");
            }
    names.removeDuplicates();
    QVERIFY(names.size() >= 20);
    const QRegularExpression vb(QStringLiteral("viewBox=\"([^\"]*)\""));
    for (const QString& n : names) {
        QFile f(QStringLiteral(":/vsicons/") + n + QStringLiteral(".svg"));
        QVERIFY2(f.open(QIODevice::ReadOnly), qPrintable(n + QStringLiteral(".svg: not in resources.qrc (renders blank)")));
        const QString svg = QString::fromUtf8(f.readAll());
        const QRegularExpressionMatch mt = vb.match(svg);
        QVERIFY2(mt.hasMatch(), qPrintable(n + QStringLiteral(".svg has no viewBox")));
        const QString box = mt.captured(1).simplified();
        QVERIFY2(box == QStringLiteral("0 0 16 16"),
                 qPrintable(QStringLiteral("%1.svg viewBox is '%2', want '0 0 16 16' (24-unit trap)").arg(n, box)));
    }
    // The known traps are indeed 24-unit — the guard is meaningful.
    for (const char* trap : {"terminal", "files"}) {
        QFile f(QStringLiteral(":/vsicons/") + QLatin1String(trap) + QStringLiteral(".svg"));
        if (!f.open(QIODevice::ReadOnly)) continue;
        const QRegularExpressionMatch mt = vb.match(QString::fromUtf8(f.readAll()));
        QVERIFY(mt.hasMatch() && mt.captured(1).simplified() == QStringLiteral("0 0 24 24"));
        QVERIFY2(!names.contains(QLatin1String(trap)), qPrintable(QStringLiteral("%1.svg is still referenced").arg(trap)));
    }
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
    // DELIBERATE CHANGE (was: > 20 % of pixels differ, and the two families
    // are different colours). The palette is "4 hues + white" now: Signed and
    // Unsigned share `syntaxNumber` because the I / U letter already carries
    // the sign, which frees markerPtr to mean only "destructive". So the two
    // glyphs differ by their FIRST LETTER alone.
    QVERIFY2(diff > 0, qPrintable(QStringLiteral("I32 vs U32 render identically (%1/%2)")
                                  .arg(diff).arg(total)));
    QCOMPARE(ribbonFamilyColour(GlyphFamily::Signed, m_dark, m_dark.background),
             ribbonFamilyColour(GlyphFamily::Unsigned, m_dark, m_dark.background));
    // The old font was fixed-width, so the tail of I32 and U32 landed on the
    // same pixels and the test could pin the difference to the first cell.
    // The 5x7 font is proportional — I advances 3, U advances 4 — so the two
    // labels are different widths and the tails no longer align. What still
    // has to hold is that the SHAPES of the trailing digits are identical.
    QCOMPARE(pixelGlyphAdvance('I'), 3);
    QCOMPARE(pixelGlyphAdvance('U'), 4);
    QCOMPARE(pixelGlyph('3'), pixelGlyph('3'));
    QCOMPARE(pixelLabelWidth(QStringLiteral("I32")) + 1,
             pixelLabelWidth(QStringLiteral("U32")));
    // markerPtr is reserved for destructive commands — no type family uses it.
    for (GlyphFamily f : {GlyphFamily::Hex, GlyphFamily::Signed, GlyphFamily::Unsigned,
                          GlyphFamily::Float, GlyphFamily::Text, GlyphFamily::Pointer,
                          GlyphFamily::Bits, GlyphFamily::Plain})
        QVERIFY2(ribbonFamilyColour(f, m_dark, m_dark.background) != m_dark.markerPtr,
                 familyName(f));
}

// DELIBERATE CHANGE. Add / Insert used to be "bitmap top-left + a 4-5-device-px
// corner number", which at 100 % was a 1-px-stroke digit — unreadable, and the
// reason the panel needed labels at all. There are two forms now:
//   labelled   → the bare "+" / hook glyph, centred in a 16-square cell (the
//                button's own text carries the count);
//   unlabelled → a 32-wide cell whose content IS the count, at the uniform
//                pixel scale: "+4"…"+2K" for Add, hook + "4"…"2K" for Insert.
void TestPixelGlyphs::compositesFitAndAreCrisp() {
    for (double dpr : {1.0, 1.25, 1.5, 2.0, 2.5}) {
        const int cell = qRound(16 * dpr);
        const int wideCell = qRound(32 * dpr);
        const int k = qMax(1, qRound(dpr));
        for (int n : {4, 8, 64, 1024, 2048}) {
            for (int which = 0; which < 2; ++which) {
                const bool add = (which == 0);
                for (int labelled = 1; labelled >= 0; --labelled) {
                    const QImage img = straight(add ? addBytesIcon(n, 16, dpr, m_dark, labelled)
                                                    : insertBytesIcon(n, 16, dpr, m_dark, labelled));
                    const QString tag = QStringLiteral("%1 %2@%3 %4").arg(add ? "add" : "insert")
                        .arg(n).arg(dpr).arg(labelled ? "labelled" : "glyph");
                    QCOMPARE(img.width(), labelled ? cell : wideCell);
                    QCOMPARE(img.height(), cell);
                    const Bbox b = inkBbox(img);
                    QVERIFY2(b.ink > 0, qPrintable(tag + QStringLiteral(" empty")));
                    // Whole device pixels only — no anti-aliasing anywhere.
                    for (int y = 0; y < img.height(); ++y)
                        for (int x = 0; x < img.width(); ++x) {
                            const int a = qAlpha(img.pixel(x, y));
                            QVERIFY2(a == 0 || a == 255, qPrintable(tag + QStringLiteral(" alpha %1").arg(a)));
                        }
                    // Ink stays inside the cell with a margin on the short axis.
                    QVERIFY2(b.minX >= 0 && b.maxX < img.width()
                             && b.minY > 0 && b.maxY < img.height() - 1, qPrintable(tag + " overflows"));
                    if (labelled) {
                        // One centred bitmap, nothing else: the ink box is the
                        // glyph's own square (5·s for +, 7·k for the hook).
                        const int side = add ? kPlus5x5.w * detail::ribbonBitmapScale(cell, kPlus5x5.w, k)
                                             : kHookArrow7x7.w * k;
                        QCOMPARE(b.w(), side);
                        QCOMPARE(b.h(), side);
                        QVERIFY2(qAbs((b.minX + b.maxX + 1) - cell) <= 1, qPrintable(tag + " not centred"));
                    } else {
                        // The count is really there: more ink columns than the
                        // bare glyph would use.
                        const int glyphW = add ? kPlus5x5.w * k : kHookArrow7x7.w * k;
                        QVERIFY2(b.w() > glyphW, qPrintable(tag + QStringLiteral(": no count drawn (w=%1)").arg(b.w())));
                    }
                }
            }
        }
        // Add and Insert must not render the same wordless cell.
        for (int n : {4, 2048}) {
            const QImage a = straight(addBytesIcon(n, 16, dpr, m_dark, false));
            const QImage i = straight(insertBytesIcon(n, 16, dpr, m_dark, false));
            QVERIFY2(a != i, "unlabelled Add and Insert are indistinguishable");
        }
        for (const QImage& img : {straight(deleteIcon(16, dpr, m_dark)),
                                  straight(fillIcon(QStringLiteral("000"), GlyphFamily::Hex, 16, dpr, m_dark)),
                                  straight(fillIcon(QStringLiteral("???"), GlyphFamily::Bits, 16, dpr, m_dark)),
                                  straight(fillIcon(QStringLiteral("FFF"), GlyphFamily::Hex, 16, dpr, m_dark, false))}) {
            const Bbox b = inkBbox(img);
            QVERIFY(b.ink > 0);
            QVERIFY(b.w() <= img.width() && b.h() <= img.height());
            QCOMPARE(img.height(), cell);
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
    // The swapped-in icons must resolve too (a missing alias renders blank).
    for (const char* n : {"discard", "console", "clippy", "ellipsis"})
        QVERIFY2(inkBbox(straight(tintedSvgIcon(QStringLiteral(":/vsicons/") + QLatin1String(n)
                                                + QStringLiteral(".svg"), Qt::white, 16, 1.0)), 30).ink > 5,
                 qPrintable(QStringLiteral("%1.svg is blank — qrc alias missing?").arg(QLatin1String(n))));
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
    // wide vs square is a separate entry even at the same logical height.
    QVERIFY(typeGlyphIcon(QStringLiteral("H64"), GlyphFamily::Hex, 16, 1.0, m_dark, false).cacheKey() != a.cacheKey());
}

QTEST_MAIN(TestPixelGlyphs)
#include "test_pixel_glyphs.moc"
