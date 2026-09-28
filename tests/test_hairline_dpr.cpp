// Phase-sweep regression pin for the device-pixel-exact edge fills in
// src/paintutil.h at fractional display scaling.
//
// Why this exists: the original edge selection (qCeil(edge) - 1) picked a
// device row/column that Qt's qRound-ed widget system clip EXCLUDES whenever
// the widget's device edge lands on a .25 phase — at 125% scaling a 1-in-4
// range of widget geometries made the line vanish entirely (e.g. the Project
// dock's right hairline at dock width 181, or a HairlineSeparator at window
// y ≡ 0 mod 4). The fixed selection (qFloor(edge ∓ 0.5)) must always paint
// exactly ONE device row/column, in every phase.
//
// The clip rounding only happens in the real backing-store paint path, so
// this test shows a real window and grabs actual screen pixels (same
// requirement as the editor_render/scanner_render harnesses — needs the
// "windows" platform, not offscreen). QT_SCALE_FACTOR is forced to 1.25 in
// a custom main() BEFORE QApplication, so the test is independent of the
// machine's physical DPI.

#include <QtTest/QTest>
#include <QApplication>
#include <QWidget>
#include <QPainter>
#include <QImage>
#include <QScreen>
#include "paintutil.h"

namespace {

class HairlineW : public QWidget {  // mirrors HairlineSeparator (main.cpp)
public:
    explicit HairlineW(QWidget* parent) : QWidget(parent) { setFixedHeight(1); }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        rcx::fillBottomDeviceRowOfRect(p, QRectF(rect()), QColor(255, 0, 0));
    }
};

class SidePanel : public QWidget {  // mirrors WorkspacePanel / tab side borders
public:
    bool leftEdge = false;
    explicit SidePanel(QWidget* parent) : QWidget(parent) {}
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), QColor(200, 0, 0));
        if (leftEdge)
            rcx::fillLeftDeviceColOfRect(p, QRectF(rect()), QColor(0, 0, 255));
        else
            rcx::fillRightDeviceColOfRect(p, QRectF(rect()), QColor(0, 0, 255));
    }
};

// 2-row accent underline (ribbon active tab / checked button): bottom or top
// N device rows of the widget rect in ONE fill.
class TwoRowW : public QWidget {
public:
    bool top = false;
    explicit TwoRowW(QWidget* parent) : QWidget(parent) {}
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), QColor(255, 255, 255));
        if (top) rcx::fillTopDeviceRowsOfRect(p, QRectF(rect()), 2, QColor(0, 255, 0));
        else     rcx::fillBottomDeviceRowsOfRect(p, QRectF(rect()), 2, QColor(0, 255, 0));
    }
};

bool isRed(QRgb px)   { return qRed(px) > 200 && qGreen(px) < 60 && qBlue(px) < 60; }
bool isBlue(QRgb px)  { return qBlue(px) > 200 && qRed(px) < 60 && qGreen(px) < 60; }
bool isGreen(QRgb px) { return qGreen(px) > 200 && qRed(px) < 60 && qBlue(px) < 60; }

} // namespace

class TestHairlineDpr : public QObject {
    Q_OBJECT

    QWidget m_win;
    QImage  m_img;
    qreal   m_dpr = 1.0;

    // Geometry covering all four mod-4 phases at DPR 1.25.
    const QList<int> m_hairlineYs   = {96, 101, 106, 111};   // bottom-row fills
    const QList<int> m_leftXs       = {4, 5, 6, 7};          // left-col fills
    const QList<int> m_rightWidths  = {180, 181, 182, 183};  // right-col fills
    static constexpr int kRightX = 120, kRightY0 = 130, kRowH = 20, kRowGap = 6;
    static constexpr int kLeftY0 = 130, kLeftW = 80;
    // 2-row sweep: eight heights (20 … 27) / eight y offsets so the bottom
    // (resp. top) device edge lands on every .0/.25/.5/.75 phase twice.
    static constexpr int kTwoRowN = 8, kTwoRowY0 = 270, kTwoRowW = 90;
    static constexpr int kBottomX = 10, kTopX = 120;

private slots:
    void initTestCase() {
        m_win.setWindowFlags(Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint);
        m_win.setFixedSize(360, 540);
        m_win.setStyleSheet(QStringLiteral("background:#ffffff;"));

        for (int y : m_hairlineYs) {
            auto* h = new HairlineW(&m_win);
            h->setGeometry(10, y, 90, 1);
        }
        for (int i = 0; i < m_leftXs.size(); ++i) {
            auto* p = new SidePanel(&m_win);
            p->leftEdge = true;
            p->setGeometry(m_leftXs[i], kLeftY0 + i * (kRowH + kRowGap), kLeftW, kRowH);
        }
        for (int i = 0; i < m_rightWidths.size(); ++i) {
            auto* p = new SidePanel(&m_win);
            p->setGeometry(kRightX, kRightY0 + i * (kRowH + kRowGap),
                           m_rightWidths[i], kRowH);
        }

        for (int i = 0; i < kTwoRowN; ++i) {
            auto* b = new TwoRowW(&m_win);
            b->setGeometry(kBottomX, kTwoRowY0 + i * 32, kTwoRowW, 20 + i);
            auto* t = new TwoRowW(&m_win);
            t->top = true;
            t->setGeometry(kTopX, kTwoRowY0 + i * 33, kTwoRowW, 20);
        }

        m_win.move(80, 80);
        m_win.show();
        m_win.raise();
        m_win.activateWindow();
        QVERIFY(QTest::qWaitForWindowExposed(&m_win));
        QTest::qWait(400);  // let the backing store settle before the grab

        m_dpr = m_win.devicePixelRatioF();
        QVERIFY2(qAbs(m_dpr - 1.25) < 0.001,
                 qPrintable(QStringLiteral("expected forced DPR 1.25, got %1").arg(m_dpr)));
        m_img = m_win.screen()->grabWindow(m_win.winId()).toImage()
                    .convertToFormat(QImage::Format_ARGB32);
        QVERIFY(!m_img.isNull());
    }

    void bottomRowPaintsOneDeviceRowInEveryPhase() {
        const int xDev = int(50 * m_dpr);  // column through the hairlines
        for (int y : m_hairlineYs) {
            int rows = 0;
            // search the hairline's device span generously (±2 rows)
            const int lo = int(y * m_dpr) - 2, hi = int((y + 1) * m_dpr) + 2;
            for (int ry = lo; ry <= hi; ++ry)
                if (isRed(m_img.pixel(xDev, ry))) ++rows;
            QVERIFY2(rows == 1, qPrintable(QStringLiteral(
                "hairline at y=%1 (devBottom %2) painted %3 rows, want exactly 1")
                .arg(y).arg((y + 1) * m_dpr).arg(rows)));
        }
    }

    void leftColPaintsOneDeviceColInEveryPhase() {
        // Left and right panels share rows — scan only the left panels' half.
        const int xSplit = int(100 * m_dpr);
        for (int i = 0; i < m_leftXs.size(); ++i) {
            const int yDev = int((kLeftY0 + i * (kRowH + kRowGap) + kRowH / 2) * m_dpr);
            int cols = 0;
            for (int x = 0; x < xSplit; ++x)
                if (isBlue(m_img.pixel(x, yDev))) ++cols;
            QVERIFY2(cols == 1, qPrintable(QStringLiteral(
                "left panel at x=%1 (devLeft %2) painted %3 border cols, want exactly 1")
                .arg(m_leftXs[i]).arg(m_leftXs[i] * m_dpr).arg(cols)));
        }
    }

    // A 2-row underline must be exactly two CONTIGUOUS device rows in every
    // phase — two 1-row fills 1 logical px apart skip a row at 1.25.
    void twoBottomRowsAreContiguousInEveryPhase() {
        const int xDev = int((kBottomX + kTwoRowW / 2) * m_dpr);
        for (int i = 0; i < kTwoRowN; ++i) {
            const int y = kTwoRowY0 + i * 32, h = 20 + i;
            const int lo = int(y * m_dpr) - 2, hi = int((y + h) * m_dpr) + 3;
            int rows = 0, first = -1, last = -1;
            for (int ry = lo; ry <= hi; ++ry)
                if (isGreen(m_img.pixel(xDev, ry))) { ++rows; if (first < 0) first = ry; last = ry; }
            QVERIFY2(rows == 2 && last == first + 1, qPrintable(QStringLiteral(
                "bottom 2-row fill y=%1 h=%2 (devBottom %3): %4 green rows (%5..%6), want 2 contiguous")
                .arg(y).arg(h).arg((y + h) * m_dpr).arg(rows).arg(first).arg(last)));
            // … and the pair ends on the row the 1-row helper would pick, so a
            // 2-row underline replaces a 1-row hairline from the same rect.
            QCOMPARE(last, int(qFloor((y + h) * m_dpr - 0.5)));
        }
    }

    void twoTopRowsAreContiguousInEveryPhase() {
        const int xDev = int((kTopX + kTwoRowW / 2) * m_dpr);
        for (int i = 0; i < kTwoRowN; ++i) {
            const int y = kTwoRowY0 + i * 33, h = 20;
            const int lo = int(y * m_dpr) - 3, hi = int((y + h) * m_dpr) + 2;
            int rows = 0, first = -1, last = -1;
            for (int ry = lo; ry <= hi; ++ry)
                if (isGreen(m_img.pixel(xDev, ry))) { ++rows; if (first < 0) first = ry; last = ry; }
            QVERIFY2(rows == 2 && last == first + 1, qPrintable(QStringLiteral(
                "top 2-row fill y=%1 (devTop %2): %3 green rows (%4..%5), want 2 contiguous")
                .arg(y).arg(y * m_dpr).arg(rows).arg(first).arg(last)));
            QCOMPARE(first, int(qFloor(y * m_dpr + 0.5)));
        }
    }

    void rightColPaintsOneDeviceColInEveryPhase() {
        // Left and right panels share rows — scan only the right panels' half.
        const int xSplit = int(100 * m_dpr);
        for (int i = 0; i < m_rightWidths.size(); ++i) {
            const int yDev = int((kRightY0 + i * (kRowH + kRowGap) + kRowH / 2) * m_dpr);
            int cols = 0;
            for (int x = xSplit; x < m_img.width(); ++x)
                if (isBlue(m_img.pixel(x, yDev))) ++cols;
            QVERIFY2(cols == 1, qPrintable(QStringLiteral(
                "right panel width=%1 (devRight %2) painted %3 border cols, want exactly 1")
                .arg(m_rightWidths[i]).arg((kRightX + m_rightWidths[i]) * m_dpr).arg(cols)));
        }
    }
};

// Custom main: the scale factor must be in the environment BEFORE
// QApplication is constructed (QTEST_MAIN would be too late).
int main(int argc, char** argv) {
    qputenv("QT_ENABLE_HIGHDPI_SCALING", "0");
    qputenv("QT_SCALE_FACTOR", "1.25");
    QApplication app(argc, argv);
    TestHairlineDpr tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "test_hairline_dpr.moc"
