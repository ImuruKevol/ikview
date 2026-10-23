#include "viewericons.h"

#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFont>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <QtTest>

namespace {
const QStringList symbols {
    "sidebar", "fit", "1:1", "−", "+", "rotate", "play", "pause",
    "stop", "bookmark", "info", "fullscreen", "hide", "open", "folder",
    "left", "right", "up", "down", "chevron-down"
};

struct Ink {
    QRect bounds;
    qreal weight = 0;
    QPointF center;
};

Ink measure(const QImage &image)
{
    Ink result;
    qreal xsum = 0, ysum = 0;
    for (int y = 0; y < image.height(); ++y) {
        for (int x = 0; x < image.width(); ++x) {
            const int alpha = image.pixelColor(x, y).alpha();
            if (alpha > 8) result.bounds |= QRect(x, y, 1, 1);
            result.weight += alpha;
            xsum += (x + .5) * alpha;
            ysum += (y + .5) * alpha;
        }
    }
    if (result.weight > 0) result.center = QPointF(xsum / result.weight, ysum / result.weight);
    return result;
}

QImage render(const QString &symbol, qreal dpr = 1, const QColor &color = Qt::white,
              QIcon::Mode mode = QIcon::Normal)
{
    return ik::viewerIcon(symbol, color).pixmap(QSize(24, 24), dpr, mode).toImage();
}
}

class IconTests final : public QObject {
    Q_OBJECT
private slots:
    void everyGlyphHasRoomAndVisibleInk_data()
    {
        QTest::addColumn<QString>("symbol");
        for (const auto &symbol : symbols) QTest::newRow(qPrintable(symbol)) << symbol;
    }

    void everyGlyphHasRoomAndVisibleInk()
    {
        QFETCH(QString, symbol);
        const auto image = render(symbol, 4);
        const auto ink = measure(image);
        QVERIFY(!ink.bounds.isEmpty());
        // No icon can touch its cell edge or disappear into a tiny font glyph.
        QVERIFY(ink.bounds.left() >= 4);
        QVERIFY(ink.bounds.top() >= 4);
        QVERIFY(ink.bounds.right() < 92);
        QVERIFY(ink.bounds.bottom() < 92);
        QVERIFY(ink.bounds.width() >= 28);
        QVERIFY(ink.bounds.height() >= 24);
        QVERIFY(ink.weight > 255 * 24 * 16);
        QVERIFY(qAbs(ink.center.x() / 4 - 12) < 2);
        QVERIFY(qAbs(ink.center.y() / 4 - 12) < 2);
    }

    void toolbarGlyphsHaveComparableOpticalSize()
    {
        qreal minimum = 24, maximum = 0;
        for (const auto &symbol : {"sidebar", "fit", "1:1", "−", "+", "rotate", "info", "fullscreen"}) {
            const auto bounds = measure(render(symbol, 4)).bounds;
            const qreal extent = qMax(bounds.width(), bounds.height()) / 4.;
            minimum = qMin(minimum, extent); maximum = qMax(maximum, extent);
        }
        QVERIFY(minimum >= 17);
        QVERIFY(maximum / minimum <= 1.2);
    }

    void devicePixelRatioRetainsLogicalSize_data()
    {
        QTest::addColumn<qreal>("dpr");
        for (const qreal dpr : {1., 1.25, 1.5, 2., 3., 4.}) QTest::newRow(qPrintable(QString::number(dpr))) << dpr;
    }

    void devicePixelRatioRetainsLogicalSize()
    {
        QFETCH(qreal, dpr);
        for (const auto &symbol : symbols) {
            const auto image = render(symbol, dpr);
            QCOMPARE(image.size(), QSize(qRound(24 * dpr), qRound(24 * dpr)));
            QCOMPARE(image.devicePixelRatio(), dpr);
            const auto ink = measure(image);
            const auto reference = measure(render(symbol, 4));
            QVERIFY2(qAbs(ink.bounds.width() / dpr - reference.bounds.width() / 4.) <= 1.6, qPrintable(symbol));
            QVERIFY2(qAbs(ink.bounds.height() / dpr - reference.bounds.height() / 4.) <= 1.6, qPrintable(symbol));
            QVERIFY2(qAbs(ink.center.x() / dpr - reference.center.x() / 4.) <= .5, qPrintable(symbol));
            QVERIFY2(qAbs(ink.center.y() / dpr - reference.center.y() / 4.) <= .5, qPrintable(symbol));
        }
    }

    void disabledIconsRetainShapeAndLoseEmphasis()
    {
        for (const auto &symbol : symbols) {
            const auto normal = measure(render(symbol, 2));
            const auto disabled = measure(render(symbol, 2, Qt::white, QIcon::Disabled));
            QVERIFY(qAbs(disabled.center.x() - normal.center.x()) < .05);
            QVERIFY(qAbs(disabled.center.y() - normal.center.y()) < .05);
            QVERIFY(disabled.weight / normal.weight > .37);
            QVERIFY(disabled.weight / normal.weight < .39);
        }
    }

    void themeChangesColorWithoutMovingGlyphs()
    {
        for (const auto &symbol : symbols) {
            const auto light = render(symbol, 2, QColor("#202b38"));
            const auto dark = render(symbol, 2, QColor("#e4ecf5"));
            QCOMPARE(measure(light).bounds, measure(dark).bounds);
            QCOMPARE(measure(light).center, measure(dark).center);
            bool foundSolid = false;
            for (int y = 0; y < light.height(); ++y) {
                for (int x = 0; x < light.width(); ++x) {
                    if (light.pixelColor(x, y).alpha() == 255) {
                        for (const auto &sample : {qMakePair(light.pixelColor(x, y), QColor("#202b38")),
                                                   qMakePair(dark.pixelColor(x, y), QColor("#e4ecf5"))}) {
                            // Raster compositing can round premultiplied RGB
                            // by one level at the solid/antialiased boundary.
                            QVERIFY(qAbs(sample.first.red() - sample.second.red()) <= 1);
                            QVERIFY(qAbs(sample.first.green() - sample.second.green()) <= 1);
                            QVERIFY(qAbs(sample.first.blue() - sample.second.blue()) <= 1);
                        }
                        foundSolid = true;
                    }
                }
            }
            QVERIFY(foundSolid);
        }
    }

    void numericAndZoomGlyphsDoNotDependOnFonts()
    {
        const auto original = QApplication::font();
        for (const auto &symbol : {"1:1", "+", "−"}) {
            const auto before = render(symbol, 2);
            QApplication::setFont(QFont("An unavailable icon font", 48, QFont::Black, true));
            QCOMPARE(render(symbol, 2), before);
            QApplication::setFont(original);
        }
    }

    void paintCentersInsideNonSquareTarget()
    {
        QImage image(40, 30, QImage::Format_ARGB32_Premultiplied);
        image.fill(Qt::transparent);
        QPainter painter(&image);
        ik::viewerIcon("info", Qt::white).paint(&painter, QRect(0, 0, 40, 30));
        painter.end();
        const auto ink = measure(image);
        QVERIFY(qAbs(QRectF(ink.bounds).center().x() - 20) < .1);
        QVERIFY(qAbs(QRectF(ink.bounds).center().y() - 15) < .1);
        QVERIFY(qAbs(ink.bounds.width() - ink.bounds.height()) <= 1);
    }

    void writeStaticContactSheet()
    {
        const QString output = qEnvironmentVariable("IKVIEW_ICON_QA_DIR");
        if (output.isEmpty()) return;
        QVERIFY(QDir().mkpath(output));
        // Pure raster output: no widget, window, native menu, or event activation.
        QImage sheet(1120, 72 + symbols.size() * 100, QImage::Format_ARGB32_Premultiplied);
        sheet.fill(QColor("#f0f3f6"));
        QPainter painter(&sheet);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.setFont(QFont("Arial", 13));
        painter.setPen(QColor("#202b38"));
        painter.drawText(QRect(20, 12, 1080, 42), Qt::AlignVCenter,
                         "IKView vectors · 24 logical px · light / dark / disabled / 96 physical px");
        QJsonArray geometry;
        for (qsizetype row = 0; row < symbols.size(); ++row) {
            const QString symbol = symbols[row];
            const int y = 72 + int(row) * 100;
            painter.setPen(QColor("#202b38"));
            painter.drawText(QRect(20, y, 170, 60), Qt::AlignVCenter, symbol);
            const auto paintCell = [&](int x, QColor background, QColor foreground, QIcon::Mode mode) {
                painter.fillRect(QRect(x, y, 128, 60), background);
                ik::viewerIcon(symbol, foreground).paint(&painter, QRect(x + 15, y + 18, 24, 24), Qt::AlignCenter, mode);
                ik::viewerIcon(symbol, foreground).paint(&painter, QRect(x + 70, y + 6, 48, 48), Qt::AlignCenter, mode);
            };
            paintCell(200, QColor("#ffffff"), QColor("#202b38"), QIcon::Normal);
            paintCell(350, QColor("#17212c"), QColor("#e4ecf5"), QIcon::Normal);
            paintCell(500, QColor("#17212c"), QColor("#e4ecf5"), QIcon::Disabled);
            const QImage large = render(symbol, 4, QColor("#202b38"));
            painter.drawImage(QRect(690, y - 12, 96, 96), large);
            const auto ink = measure(large);
            const QRectF logical(ink.bounds.x() / 4., ink.bounds.y() / 4., ink.bounds.width() / 4., ink.bounds.height() / 4.);
            painter.drawText(QRect(805, y, 300, 60), Qt::AlignVCenter,
                             QString("ink %1 × %2 px\ncenter %3, %4")
                                 .arg(logical.width()).arg(logical.height())
                                 .arg(ink.center.x() / 4., 0, 'f', 2).arg(ink.center.y() / 4., 0, 'f', 2));
            geometry.append(QJsonObject {{"symbol", symbol}, {"width", logical.width()}, {"height", logical.height()},
                                          {"centerX", ink.center.x() / 4.}, {"centerY", ink.center.y() / 4.}});
        }
        painter.end();
        QVERIFY(sheet.save(output + "/icon-system-contact-sheet.png"));
        QFile record(output + "/icon-system-geometry.json");
        QVERIFY(record.open(QIODevice::WriteOnly));
        QVERIFY(record.write(QJsonDocument(geometry).toJson()) > 0);
    }
};

int main(int argc, char **argv)
{
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    IconTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "icon_tests.moc"
