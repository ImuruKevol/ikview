#include "imagecanvas.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QImage>
#include <QSignalSpy>
#include <QtTest>

namespace {
QImage edgeImage(QSize size = QSize(512, 256))
{
    QImage image(size, QImage::Format_RGB32);
    for (int y = 0; y < size.height(); ++y) {
        auto *row = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < size.width(); ++x) {
            const int value = x < size.width() / 2 ? 64 : 192;
            row[x] = qRgb(value, value, value);
        }
    }
    return image;
}
QColor centerOffset(ik::ImageCanvas &canvas, int x, int y = 0)
{
    const QImage frame = canvas.viewportImage();
    if (frame.isNull()) return {};
    return frame.pixelColor(frame.width()/2+x,frame.height()/2+y);
}
}

class ImageFilterTests : public QObject {
    Q_OBJECT
private slots:
    void unprocessedModesShareSource_data()
    {
        QTest::addColumn<int>("mode");
        QTest::newRow("none") << int(ik::ImageFilterMode::None);
        QTest::newRow("soft-interpolation") << int(ik::ImageFilterMode::Soften);
    }
    void unprocessedModesShareSource()
    {
        QFETCH(int, mode);
        const auto source = edgeImage();
        const auto filtered = ik::ImageCanvas::applyDisplayFilter(source, ik::ImageFilterMode(mode));
        QCOMPARE(filtered.cacheKey(), source.cacheKey());
        QCOMPARE(filtered, source);
    }
    void sharpenIncreasesEdgeContrast_data()
    {
        QTest::addColumn<int>("mode");
        QTest::newRow("sharp") << int(ik::ImageFilterMode::Sharpen);
        QTest::newRow("soft-plus-sharp") << int(ik::ImageFilterMode::SoftenSharpen);
    }
    void sharpenIncreasesEdgeContrast()
    {
        QFETCH(int, mode);
        const auto source = edgeImage();
        const qint64 key = source.cacheKey();
        const auto filtered = ik::ImageCanvas::applyDisplayFilter(source, ik::ImageFilterMode(mode));
        QVERIFY(!filtered.isNull()); QCOMPARE(filtered.size(), source.size());
        const int x = source.width()/2, y = source.height()/2;
        QVERIFY(filtered.pixelColor(x-1,y).red() < source.pixelColor(x-1,y).red());
        QVERIFY(filtered.pixelColor(x,y).red() > source.pixelColor(x,y).red());
        QCOMPARE(filtered.pixelColor(0,y), source.pixelColor(0,y));
        QCOMPARE(filtered.pixelColor(source.width()-1,y), source.pixelColor(source.width()-1,y));
        QCOMPARE(source.cacheKey(),key);
        QCOMPARE(source.pixelColor(x-1,y),QColor(64,64,64));
    }
    void combinedUsesMilderSharpening()
    {
        const auto source = edgeImage();
        const auto sharp = ik::ImageCanvas::applyDisplayFilter(source, ik::ImageFilterMode::Sharpen);
        const auto combined = ik::ImageCanvas::applyDisplayFilter(source, ik::ImageFilterMode::SoftenSharpen);
        const int x = source.width()/2, y = source.height()/2;
        QVERIFY(sharp.pixelColor(x,y).red() > combined.pixelColor(x,y).red());
        QVERIFY(sharp.pixelColor(x-1,y).red() < combined.pixelColor(x-1,y).red());
    }
    void transparentHiddenColorsNeverTintVisiblePixels()
    {
        QImage first(25, 9, QImage::Format_RGBA8888), second(first.size(), first.format());
        const int alpha[] = {0,16,64,128,255};
        for (int y=0; y<first.height(); ++y) for (int x=0; x<first.width(); ++x) {
            const int a = alpha[x%5];
            first.setPixelColor(x,y,a ? QColor(180,90,40,a) : QColor(0,255,0,0));
            second.setPixelColor(x,y,a ? QColor(180,90,40,a) : QColor(0,0,255,0));
        }
        for (const auto mode : {ik::ImageFilterMode::Sharpen,ik::ImageFilterMode::SoftenSharpen}) {
            const auto a = ik::ImageCanvas::applyDisplayFilter(first,mode);
            const auto b = ik::ImageCanvas::applyDisplayFilter(second,mode);
            for (int y=0; y<first.height(); ++y) for (int x=0; x<first.width(); ++x) {
                const int coverage = first.pixelColor(x,y).alpha();
                QCOMPARE(a.pixelColor(x,y).alpha(),coverage);
                QCOMPARE(b.pixelColor(x,y).alpha(),coverage);
                if (coverage) {
                    QCOMPARE(a.pixelColor(x,y),first.pixelColor(x,y));
                    QCOMPARE(a.pixelColor(x,y),b.pixelColor(x,y));
                }
            }
        }
    }
    void constantTinyImagesAndAlphaStayUnchanged_data()
    {
        QTest::addColumn<QSize>("size"); QTest::addColumn<int>("alpha");
        for (const QSize size : {QSize(1,1),QSize(1,7),QSize(7,1),QSize(13,11)})
            for (const int alpha : {0,37,255})
                QTest::newRow(qPrintable(QString("%1x%2-a%3").arg(size.width()).arg(size.height()).arg(alpha))) << size << alpha;
    }
    void constantTinyImagesAndAlphaStayUnchanged()
    {
        QFETCH(QSize,size); QFETCH(int,alpha);
        QImage image(size,QImage::Format_RGBA8888); image.fill(QColor(90,140,170,alpha));
        for (const auto mode : {ik::ImageFilterMode::Sharpen,ik::ImageFilterMode::SoftenSharpen})
            QCOMPARE(ik::ImageCanvas::applyDisplayFilter(image,mode),image);
        QVERIFY(ik::ImageCanvas::applyDisplayFilter({},ik::ImageFilterMode::Sharpen).isNull());
    }
    void persistentModeSurvivesImageChangesAndTransformReset()
    {
        ik::ImageCanvas canvas; canvas.resize(640,480); canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QCOMPARE(canvas.imageFilter(),ik::ImageFilterMode::Soften);
        canvas.setImageFilter(ik::ImageFilterMode::Sharpen);
        const auto image = edgeImage(); canvas.setImage(image,true); canvas.setFitMode(ik::FitMode::Actual);
        QTRY_VERIFY_WITH_TIMEOUT(!canvas.isPreparing(),10000);
        QTRY_VERIFY_WITH_TIMEOUT(centerOffset(canvas,-1).red() < 64,3000);
        QCOMPARE(canvas.imageFilter(),ik::ImageFilterMode::Sharpen);
        QCOMPARE(canvas.transformedImage().cacheKey(),image.cacheKey());
        canvas.rotate(1); canvas.flipHorizontal(); canvas.resetTransform();
        QCOMPARE(canvas.imageFilter(),ik::ImageFilterMode::Sharpen);
        QTRY_VERIFY_WITH_TIMEOUT(!canvas.isPreparing(),10000);
        QTRY_VERIFY_WITH_TIMEOUT(centerOffset(canvas,-1).red() < 64,3000);
        QImage next = edgeImage(); next.setPixelColor(0,0,Qt::red); canvas.setImage(next,true);
        QCOMPARE(canvas.imageFilter(),ik::ImageFilterMode::Sharpen);
        QTRY_VERIFY_WITH_TIMEOUT(!canvas.isPreparing(),10000);
        QTRY_VERIFY_WITH_TIMEOUT(centerOffset(canvas,0).red() > 192,3000);
        const auto before = canvas.activityStats();
        canvas.setImageFilter(ik::ImageFilterMode::Sharpen);
        QCOMPARE(canvas.activityStats().sceneSyncCalls,before.sceneSyncCalls);
        QCOMPARE(canvas.activityStats().preparationJobs,before.preparationJobs);
        canvas.setSmooth(true); QCOMPARE(canvas.imageFilter(),ik::ImageFilterMode::SoftenSharpen);
        canvas.setSmooth(false); QCOMPARE(canvas.imageFilter(),ik::ImageFilterMode::Sharpen);
    }
    void linearInterpolationDiffersFromNearest()
    {
        ik::ImageCanvas canvas; canvas.resize(640,480); canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QImage image(2,1,QImage::Format_RGB32);image.setPixelColor(0,0,Qt::black);image.setPixelColor(1,0,Qt::white);
        canvas.setImage(image); canvas.setZoom(64);
        canvas.setImageFilter(ik::ImageFilterMode::None);
        QTRY_VERIFY_WITH_TIMEOUT(!canvas.isPreparing(),10000);
        QTRY_VERIFY_WITH_TIMEOUT(centerOffset(canvas,-1).red() < 4,3000);
        canvas.setImageFilter(ik::ImageFilterMode::Soften);
        QTRY_VERIFY_WITH_TIMEOUT(!canvas.isPreparing(),10000);
        QTRY_VERIFY_WITH_TIMEOUT(centerOffset(canvas,-1).red() > 70 && centerOffset(canvas,-1).red() < 180,3000);
    }
    void detailedPixelsMatchKernelAndReuseGuardBand()
    {
        ik::ImageCanvas canvas; canvas.resize(640,480); canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QImage image(6000,4000,QImage::Format_RGB32);
        for (int y=0;y<image.height();++y) {
            auto *row=reinterpret_cast<QRgb *>(image.scanLine(y));
            for (int x=0;x<image.width();++x) {const int c=80+((x/11+y/7)%2)*80;row[x]=qRgb(c,c,c);}
        }
        canvas.setImageFilter(ik::ImageFilterMode::Sharpen); canvas.setImage(image); canvas.setFitMode(ik::FitMode::Actual);
        QTRY_VERIFY_WITH_TIMEOUT(!canvas.isPreparing(),15000);
        QVERIFY(canvas.renderStats().detailPixels>0); QVERIFY(canvas.renderStats().detailPixels<=8LL*1024*1024);
        const int sx=image.width()/2-1,sy=image.height()/2;
        const QImage neighborhood = image.copy(sx-1,sy-1,3,3);
        const QColor expected = ik::ImageCanvas::applyDisplayFilter(neighborhood,ik::ImageFilterMode::Sharpen).pixelColor(1,1);
        QTRY_COMPARE_WITH_TIMEOUT(centerOffset(canvas,-1),expected,3000);
        const auto before=canvas.activityStats();
        canvas.panViewport(QPointF(1,0));
        QTRY_VERIFY_WITH_TIMEOUT(!canvas.isPreparing(),10000);
        QCOMPARE(canvas.activityStats().preparationJobs,before.preparationJobs);
        QCOMPARE(canvas.activityStats().previewUploads,before.previewUploads);
        QCOMPARE(canvas.activityStats().detailUploads,before.detailUploads);
    }
    void largeModeChangesStayAsyncAndLatestWins()
    {
        ik::ImageCanvas canvas;canvas.resize(640,480);canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QImage image(10000,8000,QImage::Format_RGB32);image.fill(QColor(80,120,160));
        canvas.setImage(image); QTRY_VERIFY_WITH_TIMEOUT(!canvas.isPreparing(),15000);
        const auto before=canvas.activityStats();
        QElapsedTimer timer;timer.start();canvas.setImageFilter(ik::ImageFilterMode::Sharpen);
        const qint64 call=timer.elapsed();qInfo()<<"80MP filter mode change GUI call"<<call<<"ms";
        QVERIFY2(call<100,"Large sharpening must run on the preparation worker");
        canvas.setImageFilter(ik::ImageFilterMode::SoftenSharpen);canvas.setImageFilter(ik::ImageFilterMode::None);
        QTRY_VERIFY_WITH_TIMEOUT(!canvas.isPreparing(),15000);
        QCOMPARE(canvas.imageFilter(),ik::ImageFilterMode::None);
        QTRY_COMPARE_WITH_TIMEOUT(centerOffset(canvas,0),QColor(80,120,160),3000);
        QCOMPARE(canvas.transformedImage().cacheKey(),image.cacheKey());
        QVERIFY(canvas.activityStats().preparationJobs-before.preparationJobs<=3);
        QVERIFY(canvas.renderStats().previewPixels<=4LL*1024*1024);
        const auto stable=canvas.activityStats();QTest::qWait(80);
        QCOMPARE(canvas.activityStats().preparationJobs,stable.preparationJobs);
        QCOMPARE(canvas.activityStats().previewUploads,stable.previewUploads);
    }
};

QTEST_MAIN(ImageFilterTests)
#include "imagefilter_tests.moc"
