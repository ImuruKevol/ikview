#include "document.h"
#include "nativeimage.h"
#include "dropfiles.h"
#include <QtTest>
#include <QBuffer>
#include <QColorSpace>
#include <QFile>
#include <QFileInfo>
#include <QMimeData>
#include <QTemporaryDir>
#include <QElapsedTimer>
#include <QTimer>
#include <archive.h>
#include <archive_entry.h>
#ifdef Q_OS_MACOS
#include <CoreGraphics/CoreGraphics.h>
#endif

class PdfTests : public QObject {
    Q_OBJECT
    static QByteArray pdf(const QList<QColor> &colors, QSizeF points = {240, 160}, bool locked = false) {
#ifdef Q_OS_MACOS
        CFMutableDataRef bytes = CFDataCreateMutable(kCFAllocatorDefault, 0);
        CGDataConsumerRef consumer = CGDataConsumerCreateWithCFData(bytes);
        CGRect box = CGRectMake(0, 0, points.width(), points.height());
        CFMutableDictionaryRef info = CFDictionaryCreateMutable(kCFAllocatorDefault, 0,
            &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFDictionarySetValue(info, kCGPDFContextTitle, CFSTR("IKView PDF test"));
        if (locked) {
            CFDictionarySetValue(info, kCGPDFContextUserPassword, CFSTR("test-user-password"));
            CFDictionarySetValue(info, kCGPDFContextOwnerPassword, CFSTR("test-owner-password"));
        }
        CGContextRef context = CGPDFContextCreate(consumer, &box, info);
        for (const auto &color : colors) {
            CGPDFContextBeginPage(context, nullptr);
            CGContextSetRGBFillColor(context, color.redF(), color.greenF(), color.blueF(), 1);
            CGContextFillRect(context, box);
            CGPDFContextEndPage(context);
        }
        CGPDFContextClose(context);
        CGContextRelease(context); CFRelease(info); CGDataConsumerRelease(consumer);
        const QByteArray result(reinterpret_cast<const char *>(CFDataGetBytePtr(bytes)), CFDataGetLength(bytes));
        CFRelease(bytes); return result;
#else
        Q_UNUSED(colors) Q_UNUSED(points) Q_UNUSED(locked)
        return {};
#endif
    }
    static QByteArray pagePdf(const QByteArray &pageProperties, const QByteArray &stream) {
        const QList<QByteArray> objects = {
            "<< /Type /Catalog /Pages 2 0 R >>",
            "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
            "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 100] " + pageProperties + " /Resources << >> /Contents 4 0 R >>",
            "<< /Length " + QByteArray::number(stream.size()) + " >>\nstream\n" + stream + "\nendstream"
        };
        QByteArray result = "%PDF-1.4\n";
        QList<int> offsets;
        for (int i = 0; i < objects.size(); ++i) {
            offsets.append(result.size());
            result += QByteArray::number(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
        }
        const int xref = result.size();
        result += "xref\n0 5\n0000000000 65535 f \n";
        for (const auto offset : offsets) result += QByteArray::number(offset).rightJustified(10, '0') + " 00000 n \n";
        result += "trailer\n<< /Size 5 /Root 1 0 R >>\nstartxref\n" + QByteArray::number(xref) + "\n%%EOF\n";
        return result;
    }
    static bool write(const QString &path, const QByteArray &bytes) {
        QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
    }
    static ik::Entry entry(const QString &path) {
        const QFileInfo file(path); return {file.fileName(), file.absoluteFilePath(), {}, {}, file.size()};
    }
    static bool archivePdf(const QString &path, const QByteArray &bytes) {
        archive *writer = archive_write_new(); archive_write_set_format_zip(writer);
        if (archive_write_open_filename(writer, QFile::encodeName(path).constData()) != ARCHIVE_OK) {
            archive_write_free(writer); return false;
        }
        archive_entry *file = archive_entry_new(); archive_entry_set_pathname(file, "document.pdf");
        archive_entry_set_filetype(file, AE_IFREG); archive_entry_set_perm(file, 0644); archive_entry_set_size(file, bytes.size());
        const bool ok = archive_write_header(writer, file) == ARCHIVE_OK && archive_write_data(writer, bytes.constData(), bytes.size()) == bytes.size();
        archive_entry_free(file); archive_write_close(writer); archive_write_free(writer); return ok;
    }
private slots:
    void nativeDecodePageAndMetadata() {
        const auto bytes = pdf({Qt::red, Qt::blue});
        const auto first = ik::decodeNativeImage({}, bytes, 0);
        QVERIFY2(!first.image.isNull(), qPrintable(first.error));
        QVERIFY(first.pdf); QCOMPARE(first.format, QString("PDF")); QCOMPARE(first.pageCount, 2);
        QCOMPARE(first.pageSizePoints, QSizeF(240, 160)); QCOMPARE(first.image.size(), QSize(480, 320));
        QCOMPARE(first.image.pixelColor(240, 160), QColor(Qt::red));
        QVERIFY(first.metadata.contains(QStringLiteral("PDF.Title: IKView PDF test")));
        const auto second = ik::decodeNativeImage({}, bytes, 1);
        QCOMPARE(second.image.pixelColor(240, 160), QColor(Qt::blue));
        QVERIFY(second.image.colorSpace().isValid());
        QVERIFY(second.image.dotsPerMeterX() > 5600 && second.image.dotsPerMeterX() < 5700);
    }
    void correctTopOriginAndWhitePaper() {
        const auto bytes = pagePdf({}, "1 0 0 rg 0 50 100 50 re f\n0 0 1 rg 100 0 100 50 re f");
        const auto page = ik::decodeNativeImage({}, bytes, 0);
        QVERIFY2(!page.image.isNull(), qPrintable(page.error));
        QCOMPARE(page.image.size(), QSize(400, 200));
        QCOMPARE(page.image.pixelColor(50, 50), QColor(Qt::red));
        QCOMPARE(page.image.pixelColor(350, 150), QColor(Qt::blue));
        QCOMPARE(page.image.pixelColor(350, 50), QColor(Qt::white));
        QCOMPARE(page.image.pixelColor(50, 150), QColor(Qt::white));
    }
    void thumbnailIsRenderedWithinItsBudget() {
        const auto page = ik::decodeNativeImage({}, pdf({Qt::blue}), 0, {120, 120});
        QVERIFY2(!page.image.isNull(), qPrintable(page.error));
        QCOMPARE(page.image.size(), QSize(120, 80));
        QCOMPARE(page.image.pixelColor(2, 2), QColor(Qt::blue));
        QCOMPARE(page.image.pixelColor(117, 77), QColor(Qt::blue));
    }
    void cropAndPageRotation() {
        const auto bytes = pagePdf("/Rotate 90 /CropBox [0 0 100 100]", "0 1 0 rg 0 0 100 100 re f\n1 0 0 rg 100 0 100 100 re f");
        const auto page = ik::decodeNativeImage({}, bytes, 0);
        QVERIFY2(!page.image.isNull(), qPrintable(page.error));
        QCOMPARE(page.pageSizePoints, QSizeF(100, 100)); QCOMPARE(page.image.size(), QSize(200, 200));
        QCOMPARE(page.image.pixelColor(100, 100), QColor(Qt::green));
        const auto rotated = ik::decodeNativeImage({}, pagePdf("/Rotate 90", "1 0 0 rg 0 0 200 100 re f"), 0);
        QCOMPARE(rotated.pageSizePoints, QSizeF(100, 200)); QCOMPARE(rotated.image.size(), QSize(200, 400));
    }
    void hugePageHasBoundedRaster() {
        const auto page = ik::decodeNativeImage({}, pdf({Qt::green}, {100000, 100000}), 0);
        QVERIFY2(!page.image.isNull(), qPrintable(page.error));
        QVERIFY(qint64(page.image.width()) * page.image.height() <= 16 * 1024 * 1024);
        QVERIFY(page.image.width() <= 8192 && page.image.height() <= 8192);
        QCOMPARE(page.pageSizePoints, QSizeF(100000, 100000));
    }
    void longPageHasBoundedEdge() {
        const auto page = ik::decodeNativeImage({}, pdf({Qt::blue}, {100000, 100}), 0);
        QVERIFY2(!page.image.isNull(), qPrintable(page.error));
        QVERIFY(page.image.width() <= 8192 && page.image.height() >= 1);
        QVERIFY(qint64(page.image.width()) * page.image.height() <= 16 * 1024 * 1024);
    }
    void corruptionAndPasswordHaveUsefulErrors() {
        const auto corrupt = ik::decodeNativeImage("broken.pdf", "%PDF-1.4\nthis is broken", 0);
        QVERIFY(corrupt.image.isNull()); QVERIFY(corrupt.error.contains(QStringLiteral("PDF")));
        const auto locked = ik::decodeNativeImage({}, pdf({Qt::red}, {240, 160}, true), 0);
        QVERIFY(locked.image.isNull()); QVERIFY2(locked.error.contains(QStringLiteral("암호")), qPrintable(locked.error));
        const auto outside = ik::decodeNativeImage({}, pdf({Qt::red}), 3);
        QVERIFY(outside.image.isNull()); QVERIFY(outside.error.contains(QStringLiteral("페이지")));
    }
    void renamedPdfAndDropDiscovery() {
        QTemporaryDir temp; const auto renamed = temp.filePath("renamed.data");
        QVERIFY(write(renamed, pdf({Qt::red}))); QVERIFY(ik::Document::isImageFile(renamed));
        QVERIFY(ik::Document::supportedExtensions().contains("pdf"));
        QMimeData mime; mime.setUrls({QUrl::fromLocalFile(renamed)});
        QCOMPARE(ik::imageDropPaths(&mime), QStringList({renamed}));
        QString error; ik::ImageInfo info;
        const auto image = ik::Document::decodeEntry(entry(renamed), 0, &error, &info);
        QVERIFY2(!image.isNull(), qPrintable(error)); QVERIFY(info.pdf); QCOMPARE(info.pageCount, 1);
    }
    void imageMetadataCannotMasqueradeAsPdf() {
        QTemporaryDir temp; const auto path = temp.filePath("real-image.png");
        QImage image(16, 16, QImage::Format_RGB32); image.fill(Qt::red);
        image.setText("Comment", "%PDF-1.7"); QVERIFY(image.save(path));
        QFile file(path); QVERIFY(file.open(QIODevice::ReadOnly)); const auto bytes = file.readAll();
        QVERIFY(bytes.first(1024).contains("%PDF-"));
        QVERIFY(!ik::isNativePdfSource(path)); QVERIFY(!ik::isNativePdfSource({}, bytes));
        QString error; ik::ImageInfo info;
        const auto pixels = ik::Document::decodeEntry(entry(path), 0, &error, &info);
        QVERIFY2(!pixels.isNull(), qPrintable(error)); QVERIFY(!info.pdf);
        QCOMPARE(pixels.pixelColor(0, 0), QColor(Qt::red));
    }
    void folderAndExplicitScope() {
        QTemporaryDir temp; const auto path = temp.filePath("01.pdf"); QVERIFY(write(path, pdf({Qt::red, Qt::blue})));
        QImage image(24, 16, QImage::Format_RGB32); image.fill(Qt::green); QVERIFY(image.save(temp.filePath("02.png")));
        ik::Document document; document.openFiles({path}, false);
        QTRY_VERIFY_WITH_TIMEOUT(!document.isBusy() && !document.currentImage().isNull(), 5000);
        QCOMPARE(document.count(), 1); QVERIFY(document.currentInfo().pdf); QCOMPARE(document.currentInfo().pageCount, 2);
        document.setPage(1); QTRY_VERIFY_WITH_TIMEOUT(!document.isBusy(), 5000);
        QCOMPARE(document.currentInfo().page, 1); QCOMPARE(document.currentImage().pixelColor(240, 160), QColor(Qt::blue));
        document.open(temp.path()); QTRY_VERIFY_WITH_TIMEOUT(!document.isBusy() && document.count() == 2, 5000);
        QCOMPARE(document.entries()[0].name, QString("01.pdf"));
        document.goTo(1); QTRY_VERIFY_WITH_TIMEOUT(!document.isBusy(), 5000);
        QVERIFY(!document.currentInfo().pdf); QCOMPARE(document.currentImage().pixelColor(0, 0), QColor(Qt::green));
    }
    void archivePageAndWorkerExport() {
        QTemporaryDir temp; const auto path = temp.filePath("pdf.cbz"); QVERIFY(archivePdf(path, pdf({Qt::red, Qt::blue})));
        ik::Document document; document.open(path);
        QTRY_VERIFY_WITH_TIMEOUT(!document.isBusy() && !document.currentImage().isNull(), 5000);
        QCOMPARE(document.count(), 1); QVERIFY(document.currentEntry().isArchive()); QVERIFY(document.currentInfo().pdf);
        QCOMPARE(document.currentInfo().pageCount, 2);
        QString error; ik::ImageInfo info;
        const auto page = ik::Document::decodeEntry(document.currentEntry(), 1, &error, &info);
        QVERIFY2(!page.isNull(), qPrintable(error)); QCOMPARE(info.page, 1); QCOMPARE(page.pixelColor(240, 160), QColor(Qt::blue));
    }
    void rapidPageRequestKeepsLatestAndNeverBlocksCaller() {
        QTemporaryDir temp; const auto path = temp.filePath("pages.pdf");
        QVERIFY(write(path, pdf({Qt::red, Qt::green, Qt::blue}, {10000, 10000})));
        ik::Document document; document.openFiles({path}, false);
        QTRY_VERIFY_WITH_TIMEOUT(!document.isBusy() && !document.currentImage().isNull(), 5000);
        QElapsedTimer elapsed; elapsed.start();
        document.setPage(1); QCOMPARE(document.currentInfo().pageCount, 3); document.setPage(2);
        QVERIFY2(elapsed.elapsed() < 100, "PDF rasterization ran on the caller thread");
        QTRY_VERIFY_WITH_TIMEOUT(!document.isBusy() && !document.currentImage().isNull(), 5000);
        QCOMPARE(document.currentInfo().page, 2); QCOMPARE(document.currentImage().pixelColor(50, 50), QColor(Qt::blue));
        document.setPage(0); QTRY_VERIFY_WITH_TIMEOUT(!document.isBusy(), 5000);
        QCOMPARE(document.currentInfo().page, 0); QCOMPARE(document.currentImage().pixelColor(50, 50), QColor(Qt::red));
    }
    void exportCancellationAndInvalidPage() {
        QTemporaryDir temp; const auto path = temp.filePath("one.pdf"); QVERIFY(write(path, pdf({Qt::red})));
        auto cancel = std::make_shared<std::atomic_bool>(true); QString error;
        QVERIFY(ik::Document::decodeEntry(entry(path), 0, &error, nullptr, cancel).isNull());
        QVERIFY(error.contains(QStringLiteral("취소")));
        QVERIFY(ik::Document::decodeEntry(entry(path), 2, &error).isNull()); QVERIFY(error.contains(QStringLiteral("페이지")));
    }
};
QTEST_MAIN(PdfTests)
#include "pdf_tests.moc"
