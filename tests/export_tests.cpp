#include "exportdialog.h"
#include <QAccessible>
#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDropEvent>
#include <QFile>
#include <QFileInfo>
#include <QElapsedTimer>
#include <QImageReader>
#include <QImageWriter>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMimeData>
#include <QPainter>
#include <QPushButton>
#include <QSignalSpy>
#include <QScopeGuard>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTest>
#include <QtConcurrent>
#ifdef Q_OS_UNIX
#include <unistd.h>
#endif
#ifdef Q_OS_MACOS
#include <CoreGraphics/CoreGraphics.h>
#endif

using namespace ik;
class ExportTests : public QObject {
    Q_OBJECT
    QTemporaryDir temp;
    ExportSource seed(const QString &name, const QColor &color, QSize size = QSize(160, 100), bool snapshot = false) {
        ExportSource source; source.entry.path = temp.filePath(name); source.entry.name = name;
        QImage image(size, QImage::Format_ARGB32); image.fill(color);
        if (!image.save(source.entry.path)) return {};
        if (snapshot) source.snapshot = image;
        return source;
    }
    static QByteArray bytes(const QString &path) { QFile file(path); if (!file.open(QIODevice::ReadOnly)) return {}; return file.readAll(); }
    static bool nearColor(const QColor &actual, const QColor &expected, int tolerance=8) {
        return qAbs(actual.red()-expected.red()) <= tolerance && qAbs(actual.green()-expected.green()) <= tolerance && qAbs(actual.blue()-expected.blue()) <= tolerance;
    }
#ifdef Q_OS_MACOS
    static QImage pdfOnTransparentSurface(const QString &path) {
        const auto encoded = QFile::encodeName(path);
        CFURLRef url = CFURLCreateFromFileSystemRepresentation(nullptr, reinterpret_cast<const UInt8 *>(encoded.constData()), encoded.size(), false);
        if (!url) return {}; const auto releaseUrl = qScopeGuard([url] { CFRelease(url); });
        CGPDFDocumentRef document = CGPDFDocumentCreateWithURL(url); if (!document) return {};
        const auto releaseDocument = qScopeGuard([document] { CGPDFDocumentRelease(document); });
        CGPDFPageRef page = CGPDFDocumentGetPage(document, 1); if (!page) return {};
        const auto box = CGPDFPageGetBoxRect(page, kCGPDFCropBox); if (box.size.width <= 0 || box.size.height <= 0) return {};
        QImage pixels(600, qRound(600. * box.size.height / box.size.width), QImage::Format_RGBA8888_Premultiplied); pixels.fill(Qt::transparent);
        CGColorSpaceRef space = CGColorSpaceCreateWithName(kCGColorSpaceSRGB); if (!space) return {};
        const auto releaseSpace = qScopeGuard([space] { CGColorSpaceRelease(space); });
        CGContextRef context = CGBitmapContextCreate(pixels.bits(), pixels.width(), pixels.height(), 8, pixels.bytesPerLine(), space, CGBitmapInfo(kCGImageAlphaPremultipliedLast) | CGBitmapInfo(kCGBitmapByteOrder32Big));
        if (!context) return {}; const auto releaseContext = qScopeGuard([context] { CGContextRelease(context); });
        CGContextConcatCTM(context, CGPDFPageGetDrawingTransform(page, kCGPDFCropBox, CGRectMake(0, 0, pixels.width(), pixels.height()), 0, false));
        CGContextDrawPDFPage(context, page); return pixels;
    }
#endif
private slots:
    void initTestCase() { QVERIFY(temp.isValid()); }
    void rasterMaintainsTransformsSizingPaddingAndOriginal() {
        auto source = seed("raster-source.png", Qt::red, QSize(120,80), true); const auto original = bytes(source.entry.path);
        source.transform.quarterTurns = 1; source.transform.inverted = true;
        ExportOptions options; options.output = temp.filePath("raster-result.png"); options.dimensions = QSize(40,60); options.padding = 5; options.paddingColor = Qt::blue;
        const auto result = exportImages({source}, options, std::make_shared<ExportProgress>()); QVERIFY2(result.success(), qPrintable(result.error));
        const QImage image(options.output); QCOMPARE(image.size(), QSize(50,70)); QVERIFY(nearColor(image.pixelColor(0,0), Qt::blue)); QVERIFY(nearColor(image.pixelColor(25,35), Qt::cyan)); QCOMPARE(bytes(source.entry.path), original);
    }
    void jpegFlattensTransparencyOnWhite() {
        auto source = seed("transparent.png", Qt::transparent, QSize(80,60), true);
        ExportOptions options; options.output = temp.filePath("opaque.jpeg"); options.format = "jpeg";
        const auto result = exportImages({source}, options, std::make_shared<ExportProgress>()); QVERIFY2(result.success(), qPrintable(result.error));
        QCOMPARE(QImageReader::imageFormat(options.output), QByteArray("jpeg")); QVERIFY(nearColor(QImage(options.output).pixelColor(20,20), Qt::white));
    }
    void transparentPaddingRoundTrips_data() {
        QTest::addColumn<QByteArray>("format");
        for (const auto &format : {QByteArray("png"), QByteArray("webp"), QByteArray("tiff")})
            if (QImageWriter::supportedImageFormats().contains(format)) QTest::newRow(format.constData()) << format;
    }
    void transparentPaddingRoundTrips() {
        QFETCH(QByteArray, format);
        const auto source = seed("alpha-padding-source-" + format + ".png", Qt::red, QSize(40, 30), true);
        const auto original = bytes(source.entry.path); ExportOptions options;
        options.format = format; options.output = temp.filePath("alpha-padding-result." + format); options.padding = 4; options.paddingColor = Qt::transparent;
        const auto result = exportImages({source}, options, std::make_shared<ExportProgress>()); QVERIFY2(result.success(), qPrintable(result.error));
        const QImage output(options.output); QCOMPARE(output.size(), QSize(48, 38)); QCOMPARE(output.pixelColor(0, 0).alpha(), 0); QCOMPARE(output.pixelColor(47, 37).alpha(), 0);
        QCOMPARE(output.pixelColor(20, 15).alpha(), 255); QVERIFY(nearColor(output.pixelColor(20, 15), Qt::red)); QCOMPARE(bytes(source.entry.path), original);
        options.output = temp.filePath("partial-alpha-padding." + format); options.paddingColor = QColor(30, 70, 160, 90);
        const auto partial = exportImages({source}, options, std::make_shared<ExportProgress>()); QVERIFY2(partial.success(), qPrintable(partial.error)); QCOMPARE(QImage(options.output).pixelColor(0, 0).alpha(), 90);
        const auto artifacts = qEnvironmentVariable("IKVIEW_EXPORT_ARTIFACT_DIR");
        if (!artifacts.isEmpty()) { QDir().mkpath(artifacts); const auto transparentOutput = temp.filePath("alpha-padding-result." + format); QFile::remove(QDir(artifacts).filePath("transparent-padding." + format)); QVERIFY(QFile::copy(transparentOutput, QDir(artifacts).filePath("transparent-padding." + format))); }
    }
    void opaqueFormatsUseSelectedMatte_data() {
        QTest::addColumn<QByteArray>("format"); QTest::newRow("jpeg") << QByteArray("jpeg"); QTest::newRow("bmp") << QByteArray("bmp");
    }
    void opaqueFormatsUseSelectedMatte() {
        QFETCH(QByteArray, format); const auto source = seed("matte-source-" + format + ".png", Qt::transparent, QSize(30, 20), true);
        ExportOptions options; options.format = format; options.output = temp.filePath("matte-result." + format); options.paddingColor = QColor(50, 90, 150); options.padding = 3;
        const auto result = exportImages({source}, options, std::make_shared<ExportProgress>()); QVERIFY2(result.success(), qPrintable(result.error));
        const QImage image(options.output); QCOMPARE(image.pixelColor(0, 0).alpha(), 255); QVERIFY(nearColor(image.pixelColor(0, 0), options.paddingColor)); QVERIFY(nearColor(image.pixelColor(15, 10), options.paddingColor));
    }
#ifdef Q_OS_MACOS
    void pdfPreservesTransparentAndPartialAlphaPaper() {
        auto source = seed("pdf-alpha-source.png", Qt::transparent, QSize(160, 100), true);
        { QPainter painter(&source.snapshot); painter.fillRect(QRect(40, 25, 80, 50), Qt::red); }
        ExportOptions options; options.format = "pdf"; options.pdfPaper = PdfPaper::A4; options.padding = 6; options.pdfMarginMm = 20;
        for (const int alpha : {0, 90, 255}) {
            options.output = temp.filePath(QString("pdf-alpha-%1.pdf").arg(alpha)); options.paddingColor = QColor(30, 70, 160, alpha);
            const auto result = exportImages({source}, options, std::make_shared<ExportProgress>()); QVERIFY2(result.success(), qPrintable(result.error));
            const auto rendered = pdfOnTransparentSurface(options.output); QVERIFY(!rendered.isNull());
            const int middle = rendered.height() / 2;
            qInfo() << "PDF paper alpha expected/actual:" << alpha << rendered.pixelColor(5, 5).alpha();
            const auto debugArtifacts = qEnvironmentVariable("IKVIEW_EXPORT_ARTIFACT_DIR");
            if (!debugArtifacts.isEmpty()) { QDir().mkpath(debugArtifacts); const auto copy = QDir(debugArtifacts).filePath(QString("pdf-alpha-%1.pdf").arg(alpha)); QFile::remove(copy); QVERIFY(QFile::copy(options.output, copy)); }
            QVERIFY(qAbs(rendered.pixelColor(5, 5).alpha() - alpha) <= 1); QCOMPARE(rendered.pixelColor(300, middle).alpha(), 255); QVERIFY(nearColor(rendered.pixelColor(300, middle), Qt::red));
            // Both the page margin and transparent image/padding sit over one
            // paper layer, so partial alpha must not accumulate twice.
            QVERIFY(qAbs(rendered.pixelColor(100, middle).alpha() - alpha) <= 1);
        }
        auto white = seed("pdf-white-background.png", Qt::white, QSize(160, 100), true);
        { QPainter painter(&white.snapshot); painter.fillRect(QRect(40, 25, 80, 50), Qt::blue); }
        options.output = temp.filePath("pdf-white-removed.pdf"); options.paddingColor = Qt::transparent; options.removeWhite = true;
        const auto result = exportImages({white}, options, std::make_shared<ExportProgress>()); QVERIFY2(result.success(), qPrintable(result.error));
        const auto rendered = pdfOnTransparentSurface(options.output); QCOMPARE(rendered.pixelColor(5, 5).alpha(), 0); QCOMPARE(rendered.pixelColor(100, rendered.height() / 2).alpha(), 0); QVERIFY(nearColor(rendered.pixelColor(300, rendered.height() / 2), Qt::blue));
        const auto artifacts = qEnvironmentVariable("IKVIEW_EXPORT_ARTIFACT_DIR");
        if (!artifacts.isEmpty()) { QDir().mkpath(artifacts); QFile::remove(QDir(artifacts).filePath("transparent-paper.pdf")); QVERIFY(QFile::copy(options.output, QDir(artifacts).filePath("transparent-paper.pdf"))); }
    }
#endif
    void whiteRemovalKeepsEnclosedWhitesAndSourcePixels() {
        QImage source(40, 30, QImage::Format_ARGB32); source.fill(Qt::white);
        { QPainter painter(&source); painter.fillRect(QRect(8, 6, 24, 18), Qt::red); painter.fillRect(QRect(13, 10, 14, 10), Qt::white); }
        const QImage original = source.copy(); WhiteBackgroundOptions options; options.tolerance = 0; options.softness = 0;
        const auto output = removeWhiteBackground(source, options); QVERIFY(!output.isNull()); QCOMPARE(source, original);
        QCOMPARE(output.pixelColor(0, 0).alpha(), 0); QCOMPARE(output.pixelColor(20, 15), QColor(Qt::white)); QCOMPARE(output.pixelColor(9, 7), QColor(Qt::red));
        options.connectedOnly = false; const auto all = removeWhiteBackground(source, options); QCOMPARE(all.pixelColor(20, 15).alpha(), 0); QCOMPARE(all.pixelColor(9, 7), QColor(Qt::red));
        for (const auto &size : {QSize(1, 1), QSize(1, 20), QSize(20, 1)}) { QImage narrow(size, QImage::Format_RGB32); narrow.fill(Qt::white); options.connectedOnly = true; const auto result = removeWhiteBackground(narrow, options); QCOMPARE(result.size(), size); QCOMPARE(result.pixelColor(size.width() - 1, size.height() - 1).alpha(), 0); }
    }
    void whiteRemovalSoftensEdgesAndPreservesExistingAlpha() {
        QImage source(5, 1, QImage::Format_ARGB32); source.fill(Qt::white); source.setPixelColor(1, 0, QColor(230, 230, 230)); source.setPixelColor(2, 0, QColor(255, 0, 0, 64)); source.setPixelColor(3, 0, QColor(230, 230, 230, 100));
        WhiteBackgroundOptions options; options.tolerance = 10; options.softness = 30; options.connectedOnly = false;
        const auto output = removeWhiteBackground(source, options); QCOMPARE(output.pixelColor(0, 0).alpha(), 0);
        const auto edge = output.pixelColor(1, 0); QVERIFY(edge.alpha() > 0 && edge.alpha() < 255); QVERIFY(edge.red() < 230);
        QCOMPARE(output.pixelColor(2, 0), QColor(255, 0, 0, 64)); QVERIFY(output.pixelColor(3, 0).alpha() > 0 && output.pixelColor(3, 0).alpha() <= 100);
        options.softness = 0; const auto hard = removeWhiteBackground(source, options); QCOMPARE(hard.pixelColor(1, 0), source.pixelColor(1, 0));
        options.tolerance = 30; QCOMPARE(removeWhiteBackground(source, options).pixelColor(1, 0).alpha(), 0);
    }
    void transparentPixelsConnectToBorderButDiagonalWhitesDoNot() {
        QImage source(7, 7, QImage::Format_ARGB32); source.fill(Qt::black);
        for (int y = 0; y < 3; ++y) source.setPixel(3, y, qRgba(0, 0, 0, 0)); source.setPixelColor(3, 3, Qt::white);
        source.setPixelColor(0, 0, Qt::white); source.setPixelColor(1, 1, Qt::white);
        WhiteBackgroundOptions options; options.tolerance = 0; options.softness = 0;
        const auto output = removeWhiteBackground(source, options); QCOMPARE(output.pixelColor(3, 3).alpha(), 0); QCOMPARE(output.pixelColor(0, 0).alpha(), 0); QCOMPARE(output.pixelColor(1, 1), QColor(Qt::white));
    }
    void whiteRemovalIsCancellableAndKeepsOriginalOnSave() {
        auto source = seed("white-original.png", Qt::white, QSize(120, 80), true);
        { QPainter painter(&source.snapshot); painter.fillRect(QRect(30, 20, 60, 40), Qt::blue); } QVERIFY(source.snapshot.save(source.entry.path)); const auto original = bytes(source.entry.path);
        ExportOptions options; options.output = temp.filePath("white-removed.png"); options.removeWhite = true; options.padding = 3; options.paddingColor = Qt::transparent;
        const auto result = exportImages({source}, options, std::make_shared<ExportProgress>()); QVERIFY2(result.success(), qPrintable(result.error));
        const QImage output(options.output); QCOMPARE(output.size(), QSize(126, 86)); QCOMPARE(output.pixelColor(5, 5).alpha(), 0); QCOMPARE(output.pixelColor(60, 40), QColor(Qt::blue)); QCOMPARE(bytes(source.entry.path), original);
        options.output = source.entry.path; QVERIFY(!exportImages({source}, options, std::make_shared<ExportProgress>()).success()); QCOMPARE(bytes(source.entry.path), original);
        options.output = temp.filePath("white-unsupported.jpeg"); options.format = "jpeg"; const auto unsupported = exportImages({source}, options, std::make_shared<ExportProgress>()); QVERIFY(!unsupported.success()); QVERIFY(unsupported.error.contains("PNG")); QVERIFY(!QFileInfo::exists(options.output));
        auto cancelled = std::make_shared<std::atomic_bool>(true); QVERIFY(removeWhiteBackground(source.snapshot, {}, cancelled).isNull());
        cancelled->store(false); QImage large(3600, 2400, QImage::Format_RGB32); large.fill(Qt::white);
        QElapsedTimer timer; timer.start(); const auto full = removeWhiteBackground(large, {}, cancelled); QVERIFY(!full.isNull()); qInfo() << "8.64 MP white-background removal:" << timer.elapsed() << "ms";
        auto future = QtConcurrent::run([large, cancelled] { return removeWhiteBackground(large, {}, cancelled); }); cancelled->store(true); future.waitForFinished(); QVERIFY(future.result().isNull());
    }
    void transparentFormatControlsAndDedicatedRemovalSave() {
        auto source = seed("white-dialog-source.png", Qt::white, QSize(120, 80), true);
        { QPainter painter(&source.snapshot); painter.fillRect(QRect(30, 20, 60, 40), Qt::blue); } QVERIFY(source.snapshot.save(source.entry.path)); const auto original = bytes(source.entry.path);
        ExportDialog normal({source}); auto *format = normal.findChild<QComboBox *>("exportFormat"); auto *transparent = normal.findChild<QCheckBox *>("exportTransparentPadding"); auto *remove = normal.findChild<QCheckBox *>("exportRemoveWhiteBackground");
        QVERIFY(format && transparent && remove); QVERIFY(transparent->isEnabled()); transparent->setChecked(true); remove->setChecked(true);
        format->setCurrentIndex(format->findData(QByteArray("jpeg"))); QVERIFY(!transparent->isEnabled()); QVERIFY(!remove->isEnabled()); QVERIFY(normal.findChild<QWidget *>("whiteBackgroundControls")->isHidden());
        format->setCurrentIndex(format->findData(QByteArray("png"))); QVERIFY(transparent->isEnabled() && transparent->isChecked()); QVERIFY(remove->isEnabled() && remove->isChecked());
        format->setCurrentIndex(format->findData(QByteArray("pdf"))); QVERIFY(transparent->isEnabled() && transparent->isChecked()); QVERIFY(remove->isEnabled());
        format->setCurrentIndex(format->findData(QByteArray("png")));
        ExportDialog dedicated({source}, nullptr, ExportPurpose::RemoveWhiteBackground); dedicated.show(); format = dedicated.findChild<QComboBox *>("exportFormat");
        for (int i = 0; i < format->count(); ++i) QVERIFY(exportSupportsTransparency(format->itemData(i).toByteArray()));
        QVERIFY(dedicated.findChild<QCheckBox *>("exportRemoveWhiteBackground")->isChecked()); QVERIFY(dedicated.findChild<QCheckBox *>("exportTransparentPadding")->isChecked()); QVERIFY(dedicated.findChild<QLineEdit *>("exportPath")->text().endsWith("-transparent.png"));
        auto *preview = dedicated.findChild<QWidget *>("whiteBackgroundPreview"); QVERIFY(preview); QTRY_VERIFY_WITH_TIMEOUT(preview->property("previewReady").toBool(), 5000);
        auto *tolerance = dedicated.findChild<QSpinBox *>("whiteBackgroundTolerance"); for (int i = 0; i < 20; ++i) tolerance->setValue(i); QTRY_VERIFY_WITH_TIMEOUT(preview->property("previewReady").toBool(), 5000);
        const auto output = temp.filePath("white-dialog-result.png"); dedicated.findChild<QLineEdit *>("exportPath")->setText(output); QSignalSpy finished(&dedicated, &ExportDialog::exportFinished); dedicated.accept();
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000); QVERIFY2(finished[0][0].toBool(), qPrintable(finished[0][2].toString())); QCOMPARE(QImage(output).pixelColor(0, 0).alpha(), 0); QCOMPARE(QImage(output).pixelColor(60, 40), QColor(Qt::blue)); QCOMPARE(bytes(source.entry.path), original);
    }
    void removalPreviewResolvesNoncurrentSourceAndSavesImmediately() {
        const auto source = seed("white-noncurrent.png", Qt::white, QSize(120, 80));
        ExportDialog preview({source}, nullptr, ExportPurpose::RemoveWhiteBackground);
        auto *widget = preview.findChild<QWidget *>("whiteBackgroundPreview"); QVERIFY(widget); QTRY_VERIFY_WITH_TIMEOUT(widget->property("previewReady").toBool(), 5000);
        ExportDialog immediate({source}, nullptr, ExportPurpose::RemoveWhiteBackground); const auto output = temp.filePath("white-noncurrent-result.png"); immediate.findChild<QLineEdit *>("exportPath")->setText(output);
        QSignalSpy finished(&immediate, &ExportDialog::exportFinished); immediate.accept(); QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000); QVERIFY2(finished[0][0].toBool(), qPrintable(finished[0][2].toString())); const QImage image(output); QCOMPARE(image.size(), QSize(120, 80)); QCOMPARE(image.pixelColor(10, 10).alpha(), 0);
    }
    void originalAndHardLinkCannotBeOverwritten() {
        const auto source = seed("protected.png", Qt::green); const auto original = bytes(source.entry.path);
        ExportOptions options; options.output = source.entry.path;
        auto result = exportImages({source}, options, std::make_shared<ExportProgress>()); QVERIFY(!result.success()); QVERIFY(result.error.contains("원본")); QCOMPARE(bytes(source.entry.path), original);
#ifdef Q_OS_UNIX
        const auto hardlink = temp.filePath("protected-link.png"); QCOMPARE(::link(QFile::encodeName(source.entry.path).constData(), QFile::encodeName(hardlink).constData()),0);
        options.output = hardlink; result = exportImages({source}, options, std::make_shared<ExportProgress>()); QVERIFY(!result.success()); QVERIFY(result.error.contains("원본")); QCOMPARE(bytes(hardlink), original);
#endif
    }
    void cancelPreservesExistingTarget() {
        const auto source = seed("cancel-source.png", Qt::yellow); ExportOptions options; options.output = temp.filePath("cancel.pdf"); options.format = "pdf";
        QFile target(options.output); QVERIFY(target.open(QIODevice::WriteOnly)); target.write("existing target"); target.close();
        const auto progress = std::make_shared<ExportProgress>(); QVERIFY(progress->requestCancel());
        const auto result = exportImages({source}, options, progress); QVERIFY(result.cancelled); QCOMPARE(bytes(options.output), QByteArray("existing target"));
    }
    void failedBatchPreservesExistingTarget() {
        const auto source = seed("valid-first.png", Qt::blue); ExportSource broken; broken.entry.path = temp.filePath("missing.png"); broken.entry.name = "missing.png";
        ExportOptions options; options.output = temp.filePath("failure.pdf"); options.format = "pdf";
        QFile target(options.output); QVERIFY(target.open(QIODevice::WriteOnly)); target.write("existing target"); target.close();
        const auto progress = std::make_shared<ExportProgress>(); const auto result = exportImages({source, broken}, options, progress); QVERIFY(!result.success()); QVERIFY(!result.error.isEmpty()); QCOMPARE(progress->completed.load(),1); QCOMPARE(bytes(options.output), QByteArray("existing target"));
    }
    void pdfPagesReopenInSpecifiedOrderAndNativeDimensions() {
        const auto red = seed("pdf-red.png", Qt::red, QSize(240,120)); const auto green = seed("pdf-green.png", Qt::green, QSize(120,240)); const auto blue = seed("pdf-blue.png", Qt::blue, QSize(160,160));
        QImage landmarks(blue.entry.path); { QPainter painter(&landmarks); painter.fillRect(QRect(0,0,20,20),Qt::red); painter.fillRect(QRect(140,140,20,20),Qt::green); } QVERIFY(landmarks.save(blue.entry.path));
        ExportOptions options; options.output = temp.filePath("ordered.pdf"); options.format = "pdf";
        const auto progress = std::make_shared<ExportProgress>(); const auto result = exportImages({blue,red,green},options,progress); QVERIFY2(result.success(),qPrintable(result.error)); QCOMPARE(progress->completed.load(),3);
        Entry entry; entry.path = options.output; QString error; ImageInfo info;
        const QColor expected[] = {Qt::blue, Qt::red, Qt::green};
        for (int i=0;i<3;++i) { const auto page = Document::decodeEntry(entry,i,&error,&info); QVERIFY2(!page.isNull(),qPrintable(error)); QCOMPARE(info.pageCount,3); QVERIFY(nearColor(page.pixelColor(page.width()/2,page.height()/2),expected[i]));
            if (i == 0) { QVERIFY(nearColor(page.pixelColor(page.width()/20,page.height()/20),Qt::red)); QVERIFY(nearColor(page.pixelColor(page.width()*19/20,page.height()*19/20),Qt::green)); }
            if (i == 1) QVERIFY(page.width() > page.height()); if (i == 2) QVERIFY(page.height() > page.width()); }
        const auto snapshotDir = qEnvironmentVariable("IKVIEW_EXPORT_ARTIFACT_DIR");
        if (!snapshotDir.isEmpty()) { QDir().mkpath(snapshotDir); QFile::remove(QDir(snapshotDir).filePath("ordered.pdf")); QVERIFY(QFile::copy(options.output,QDir(snapshotDir).filePath("ordered.pdf"))); }
    }
    void pdfA4MarginCenteredWithVisiblePaper() {
        const auto source = seed("a4-red.png", Qt::red, QSize(200,200)); ExportOptions options; options.output = temp.filePath("a4.pdf"); options.format="pdf"; options.pdfPaper=PdfPaper::A4; options.pdfMarginMm=20; options.paddingColor=Qt::white;
        const auto result=exportImages({source},options,std::make_shared<ExportProgress>()); QVERIFY2(result.success(),qPrintable(result.error));
        Entry entry; entry.path=options.output; QString error; ImageInfo info; const auto page=Document::decodeEntry(entry,0,&error,&info); QVERIFY2(!page.isNull(),qPrintable(error));
        QVERIFY(page.height()>page.width()); QVERIFY(nearColor(page.pixelColor(5,5),Qt::white)); QVERIFY(nearColor(page.pixelColor(page.width()/2,page.height()/2),Qt::red));
        const int y=page.height()/2; int left=0,right=page.width()-1; while(left<page.width()&&page.pixelColor(left,y).red()>240&&page.pixelColor(left,y).green()>240)++left; while(right>0&&page.pixelColor(right,y).red()>240&&page.pixelColor(right,y).green()>240)--right;
        QVERIFY(left>page.width()*.08); QVERIFY(qAbs(left-(page.width()-1-right))<5);
        const auto snapshotDir=qEnvironmentVariable("IKVIEW_EXPORT_ARTIFACT_DIR"); if(!snapshotDir.isEmpty()){QDir().mkpath(snapshotDir);QFile::remove(QDir(snapshotDir).filePath("a4.pdf"));QVERIFY(QFile::copy(options.output,QDir(snapshotDir).filePath("a4.pdf")));}
    }
    void selectedPdfPageCanBeReexported() {
        const auto a=seed("first.png",Qt::red), b=seed("second.png",Qt::blue); ExportOptions options; options.output=temp.filePath("page-source.pdf");options.format="pdf";
        auto result=exportImages({a,b},options,std::make_shared<ExportProgress>());QVERIFY2(result.success(),qPrintable(result.error));
        ExportSource pdf;pdf.entry.path=options.output;pdf.entry.name="page-source.pdf";pdf.page=1;options.output=temp.filePath("page-selected.pdf");result=exportImages({pdf},options,std::make_shared<ExportProgress>());QVERIFY2(result.success(),qPrintable(result.error));
        Entry entry;entry.path=options.output;QString error;ImageInfo info;const auto image=Document::decodeEntry(entry,0,&error,&info);QVERIFY2(!image.isNull(),qPrintable(error));QCOMPARE(info.pageCount,1);QVERIFY(nearColor(image.pixelColor(image.width()/2,image.height()/2),Qt::blue));
    }
    void dialogReordersAndPdfSelectionKeepsSingleOptions() {
        const auto a=seed("dialog-a.png",Qt::red),b=seed("dialog-b.png",Qt::blue),c=seed("dialog-c.png",Qt::green);
        ExportDialog multi({a,b,c});auto *order=multi.findChild<QListWidget*>("exportSourceOrder");auto *up=multi.findChild<QPushButton*>("exportMoveUp");QVERIFY(order&&up);order->setCurrentRow(2);up->click();up->click();
        const auto sorted=multi.orderedSources();QCOMPARE(sorted[0].entry.name,c.entry.name);QCOMPARE(sorted[1].entry.name,a.entry.name);QCOMPARE(sorted[2].entry.name,b.entry.name);QCOMPARE(multi.findChild<QComboBox*>("exportFormat")->currentData().toByteArray(),QByteArray("pdf"));
        ExportDialog single({seed("dialog-single.png",Qt::red,QSize(120,80),true)});auto *format=single.findChild<QComboBox*>("exportFormat");QVERIFY(format);QVERIFY(format->findData(QByteArray("pdf"))>=0);auto *width=single.findChild<QSpinBox*>("exportWidth");auto *height=single.findChild<QSpinBox*>("exportHeight");QVERIFY(width&&height);width->setValue(60);QCOMPARE(height->value(),40);format->setCurrentIndex(format->findData(QByteArray("pdf")));QVERIFY(single.findChild<QLineEdit*>("exportPath")->text().endsWith(".pdf"));QCOMPARE(width->value(),60);
    }
    void accessibleSelectionStaysValidThroughRepeatedReorderAndSave() {
        const bool wasActive=QAccessible::isActive();QAccessible::setActive(true);const auto restore=qScopeGuard([wasActive]{QAccessible::setActive(wasActive);});
        const auto a=seed("accessible-a.png",Qt::red),b=seed("accessible-b.png",Qt::blue),c=seed("accessible-c.png",Qt::green);
        for(int iteration=0;iteration<4;++iteration){
            ExportDialog dialog({a,b,c});dialog.show();auto *order=dialog.findChild<QListWidget*>("exportSourceOrder");auto *down=dialog.findChild<QPushButton*>("exportMoveDown"),*up=dialog.findChild<QPushButton*>("exportMoveUp");QVERIFY(order&&down&&up);
            QSignalSpy inserted(order->model(),&QAbstractItemModel::rowsInserted),removed(order->model(),&QAbstractItemModel::rowsRemoved),reset(order->model(),&QAbstractItemModel::modelReset),moved(order->model(),&QAbstractItemModel::rowsMoved);
            auto *accessible=QAccessible::queryAccessibleInterface(order);QVERIFY(accessible);auto *selection=accessible->selectionInterface();QVERIFY(selection);QCOMPARE(accessible->childCount(),3);for(int row=0;row<3;++row){auto *child=accessible->child(row);QVERIFY(child&&child->isValid());QVERIFY(QAccessible::uniqueId(child));}
            const auto checkSelection=[&](){auto *currentAccessible=QAccessible::queryAccessibleInterface(order);if(!currentAccessible)return false;auto *currentSelection=currentAccessible->selectionInterface();if(!currentSelection)return false;const auto selected=currentSelection->selectedItems();if(selected.size()!=1||!selected.first()||!selected.first()->isValid())return false;return selected.first()->text(QAccessible::Name)==order->currentItem()->text();};
            QVERIFY(checkSelection());for(int move=0;move<8;++move){down->click();QVERIFY(checkSelection());up->click();QVERIFY(checkSelection());QCoreApplication::processEvents();QVERIFY(checkSelection());}
            QCOMPARE(inserted.size(),0);QCOMPARE(removed.size(),0);QCOMPARE(reset.size(),0);QCOMPARE(moved.size(),0);
            auto *buttons=dialog.findChild<QDialogButtonBox*>();QVERIFY(buttons);QCOMPARE(buttons->button(QDialogButtonBox::Save)->text(),QString("저장"));QCOMPARE(buttons->button(QDialogButtonBox::Cancel)->text(),QString("취소"));
            const auto output=temp.filePath(QString("accessible-result-%1.pdf").arg(iteration));dialog.findChild<QLineEdit*>("exportPath")->setText(output);QSignalSpy finished(&dialog,&ExportDialog::exportFinished);dialog.accept();QVERIFY(checkSelection());QTRY_COMPARE_WITH_TIMEOUT(finished.size(),1,5000);QVERIFY(finished[0][0].toBool());QVERIFY(checkSelection());
        }
    }
    void internalDropReordersSourcesWithoutReplacingAccessibleRows() {
        const bool wasActive=QAccessible::isActive();QAccessible::setActive(true);const auto restore=qScopeGuard([wasActive]{QAccessible::setActive(wasActive);});
        const auto a=seed("drop-a.png",Qt::red),b=seed("drop-b.png",Qt::blue),c=seed("drop-c.png",Qt::green);ExportDialog dialog({a,b,c});dialog.show();QCoreApplication::processEvents();auto *order=dialog.findChild<QListWidget*>("exportSourceOrder");QVERIFY(order);
        auto *accessible=QAccessible::queryAccessibleInterface(order);QVERIFY(accessible);QCOMPARE(accessible->childCount(),3);for(int row=0;row<3;++row)QVERIFY(accessible->child(row)->isValid());
        QSignalSpy inserted(order->model(),&QAbstractItemModel::rowsInserted),removed(order->model(),&QAbstractItemModel::rowsRemoved),reset(order->model(),&QAbstractItemModel::modelReset),moved(order->model(),&QAbstractItemModel::rowsMoved),changed(order->model(),&QAbstractItemModel::dataChanged);
        std::unique_ptr<QMimeData> mime(order->model()->mimeData({order->model()->index(0,0)}));mime->setData("application/x-ikview-export-page","0");mime->setProperty("ikviewExportSource",QVariant::fromValue<QObject*>(order));const auto last=order->visualItemRect(order->item(2));const QPoint position(last.center().x(),last.bottom()-1);QDragEnterEvent enter(position,Qt::MoveAction,mime.get(),Qt::LeftButton,Qt::NoModifier);QCoreApplication::sendEvent(order->viewport(),&enter);QVERIFY(enter.isAccepted());QDragMoveEvent move(position,Qt::MoveAction,mime.get(),Qt::LeftButton,Qt::NoModifier);QCoreApplication::sendEvent(order->viewport(),&move);QVERIFY(move.isAccepted());QDropEvent drop(QPointF(position),Qt::MoveAction,mime.get(),Qt::LeftButton,Qt::NoModifier);QCoreApplication::sendEvent(order->viewport(),&drop);QVERIFY(drop.isAccepted());
        const auto sorted=dialog.orderedSources();QCOMPARE(sorted[0].entry.name,b.entry.name);QCOMPARE(sorted[1].entry.name,c.entry.name);QCOMPARE(sorted[2].entry.name,a.entry.name);QCOMPARE(order->count(),3);QCOMPARE(order->currentRow(),2);QVERIFY(changed.size()>0);QCOMPARE(inserted.size(),0);QCOMPARE(removed.size(),0);QCOMPARE(reset.size(),0);QCOMPARE(moved.size(),0);
        const auto selected=QAccessible::queryAccessibleInterface(order)->selectionInterface()->selectedItems();QCOMPARE(selected.size(),1);QVERIFY(selected.first()->isValid());QCOMPARE(selected.first()->text(QAccessible::Name),a.entry.name);
    }
    void dialogFailureRetryAndAsyncFinish() {
        ExportDialog dialog({seed("async.png",Qt::red,QSize(120,80),true)});dialog.show();QSignalSpy finished(&dialog,&ExportDialog::exportFinished),saving(&dialog,&ExportDialog::savingChanged);
        QFile blocker(temp.filePath("blocker"));QVERIFY(blocker.open(QIODevice::WriteOnly));blocker.write("block");blocker.close();auto *path=dialog.findChild<QLineEdit*>("exportPath");auto *width=dialog.findChild<QSpinBox*>("exportWidth");width->setValue(90);path->setText(temp.filePath("blocker/file.png"));dialog.accept();QTRY_COMPARE_WITH_TIMEOUT(finished.size(),1,5000);QVERIFY(dialog.isVisible());QVERIFY(!finished[0][0].toBool());QCOMPARE(width->value(),90);QVERIFY(dialog.findChild<QLabel*>("exportValidationMessage")->text().contains("저장하지 못했습니다"));
        path->setText(temp.filePath("async-success.png"));dialog.accept();QTRY_COMPARE_WITH_TIMEOUT(finished.size(),2,5000);QVERIFY(finished[1][0].toBool());QVERIFY(!dialog.isVisible());QCOMPARE(QImage(path->text()).width(),90);QCOMPARE(saving.size(),4);
    }
    void singleNoncurrentSourcePreservesOriginalSize_data() {
        QTest::addColumn<bool>("waitForGeometry");QTest::addColumn<bool>("rotate");
        QTest::newRow("immediate-original")<<false<<false;QTest::newRow("immediate-rotated")<<false<<true;QTest::newRow("ready-original")<<true<<false;QTest::newRow("ready-rotated")<<true<<true;
    }
    void singleNoncurrentSourcePreservesOriginalSize() {
        QFETCH(bool,waitForGeometry);QFETCH(bool,rotate);const auto tag=QString::fromLatin1(QTest::currentDataTag());auto source=seed("noncurrent-"+tag+".png",Qt::red,QSize(120,80));QVERIFY(source.snapshot.isNull());if(rotate)source.transform.quarterTurns=1;
        ExportDialog dialog({source});dialog.show();const QSize expected=rotate?QSize(80,120):QSize(120,80);auto *width=dialog.findChild<QSpinBox*>("exportWidth"),*height=dialog.findChild<QSpinBox*>("exportHeight");QVERIFY(width&&height);
        if(waitForGeometry){QTRY_COMPARE_WITH_TIMEOUT(width->value(),expected.width(),5000);QCOMPARE(height->value(),expected.height());QVERIFY(width->isEnabled());}
        const auto output=temp.filePath("noncurrent-result-"+tag+".png");dialog.findChild<QLineEdit*>("exportPath")->setText(output);QSignalSpy finished(&dialog,&ExportDialog::exportFinished);dialog.accept();QTRY_COMPARE_WITH_TIMEOUT(finished.size(),1,5000);QVERIFY2(finished[0][0].toBool(),qPrintable(finished[0][2].toString()));QCOMPARE(QImage(output).size(),expected);
    }
    void noncurrentGeometryEnablesAspectPreservingResize() {
        ExportDialog dialog({seed("noncurrent-resize.png",Qt::red,QSize(120,80))});dialog.show();auto *width=dialog.findChild<QSpinBox*>("exportWidth"),*height=dialog.findChild<QSpinBox*>("exportHeight");QTRY_COMPARE_WITH_TIMEOUT(width->value(),120,5000);QVERIFY(width->isEnabled());width->setValue(60);QCOMPARE(height->value(),40);
        const auto output=temp.filePath("noncurrent-resized.png");dialog.findChild<QLineEdit*>("exportPath")->setText(output);QSignalSpy finished(&dialog,&ExportDialog::exportFinished);dialog.accept();QTRY_COMPARE_WITH_TIMEOUT(finished.size(),1,5000);QVERIFY(finished[0][0].toBool());QCOMPARE(QImage(output).size(),QSize(60,40));
    }
    void dialogCancelDefersClosingUntilWorkerFinishes() {
        auto source=seed("async-cancel.png",Qt::red,QSize(3200,2400),true);QVector<ExportSource> sources(24,source);ExportDialog dialog(sources);dialog.show();auto *path=dialog.findChild<QLineEdit*>("exportPath");const auto output=temp.filePath("async-cancel.pdf");path->setText(output);QSignalSpy finished(&dialog,&ExportDialog::exportFinished);
        dialog.accept();QVERIFY(dialog.isSaving());dialog.reject();QVERIFY(dialog.isVisible());QTRY_COMPARE_WITH_TIMEOUT(finished.size(),1,5000);QVERIFY(finished[0][1].toBool());QVERIFY(!dialog.isSaving());QVERIFY(!dialog.isVisible());QVERIFY(!QFileInfo::exists(output));
    }
    void returnOnFormatFieldDoesNotStartExport() {
        ExportDialog dialog({seed("return-source.png",Qt::red,QSize(120,80),true)});dialog.show();auto *format=dialog.findChild<QComboBox*>("exportFormat");format->setFocus();QTest::keyClick(&dialog,Qt::Key_Return);QVERIFY(!dialog.isSaving());QVERIFY(dialog.isVisible());
    }
    void outputBudgetAndInvalidMarginLeaveNoPartialFiles() {
        const auto source=seed("budget.png",Qt::red);ExportOptions options;options.output=temp.filePath("budget.pdf");options.format="pdf";options.dimensions=QSize(65535,65535);
        auto result=exportImages({source},options,std::make_shared<ExportProgress>());QVERIFY(!result.success());QVERIFY(!QFileInfo::exists(options.output));options.dimensions={};options.pdfMarginMm=50;result=exportImages({source},options,std::make_shared<ExportProgress>());QVERIFY(!result.success());QVERIFY(result.error.contains("여백"));QVERIFY(!QFileInfo::exists(options.output));
    }
};
QTEST_MAIN(ExportTests)
#include "export_tests.moc"
