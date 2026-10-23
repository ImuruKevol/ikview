#include "document.h"
#include "nativeimage.h"
#include "dropfiles.h"
#include <QtTest>
#include <QBuffer>
#include <QFile>
#include <QDataStream>
#include <QImageReader>
#include <QSaveFile>
#include <QFileSystemWatcher>
#include <QTemporaryDir>
#include <QColorSpace>
#include <QTransform>
#include <archive.h>
#include <archive_entry.h>

class DocumentTests : public QObject {
    Q_OBJECT
    static bool saveImage(const QString &path, const QColor &color, QSize size = {48, 32}) {
        QImage image(size, QImage::Format_RGB32);
        image.fill(color);
        return image.save(path);
    }
    static QByteArray png(const QColor &color) {
        QImage image(48, 32, QImage::Format_RGB32);
        image.fill(color);
        QByteArray bytes;
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::WriteOnly);
        image.save(&buffer, "PNG");
        return bytes;
    }
    static QByteArray tiff(const QList<QColor> &colors) {
        QByteArray bytes;
        QDataStream data(&bytes, QIODevice::WriteOnly);
        data.setByteOrder(QDataStream::LittleEndian);
        data.writeRawData("II", 2);
        data << quint16(42) << quint32(8);
        const int bitsOffset = 8 + 126 * colors.size(), pixelsOffset = bitsOffset + 6 * colors.size();
        for (int page = 0; page < colors.size(); ++page) {
            data << quint16(10);
            const auto tag = [&](quint16 id, quint16 type, quint32 count, quint32 value) { data << id << type << count << value; };
            tag(256, 4, 1, 2); tag(257, 4, 1, 2); tag(258, 3, 3, bitsOffset + page * 6);
            tag(259, 3, 1, 1); tag(262, 3, 1, 2); tag(273, 4, 1, pixelsOffset + page * 12);
            tag(277, 3, 1, 3); tag(278, 4, 1, 2); tag(279, 4, 1, 12); tag(284, 3, 1, 1);
            data << quint32(page + 1 < colors.size() ? 8 + 126 * (page + 1) : 0);
        }
        for (int i = 0; i < colors.size() * 3; ++i) data << quint16(8);
        for (const auto &color : colors) for (int pixel = 0; pixel < 4; ++pixel)
            data << quint8(color.red()) << quint8(color.green()) << quint8(color.blue());
        return bytes;
    }
    static bool writeArchive(const QString &path, const QList<QPair<QString, QByteArray>> &files,
                             const QByteArray &format = "zip", const QList<qint64> &modified = {}) {
        archive *writer = archive_write_new();
        if (format == "7z") archive_write_set_format_7zip(writer);
        else if (format == "tar") archive_write_set_format_pax_restricted(writer);
        else archive_write_set_format_zip(writer);
        const QByteArray encodedPath = QFile::encodeName(path);
        if (archive_write_open_filename(writer, encodedPath.constData()) != ARCHIVE_OK) {
            archive_write_free(writer); return false;
        }
        bool ok = true;
        int index = 0;
        for (const auto &file : files) {
            archive_entry *entry = archive_entry_new();
            const QByteArray name = file.first.toUtf8();
            archive_entry_set_pathname_utf8(entry, name.constData());
            archive_entry_set_size(entry, file.second.size());
            archive_entry_set_filetype(entry, AE_IFREG);
            archive_entry_set_perm(entry, 0644);
            if (index < modified.size()) archive_entry_set_mtime(entry, modified[index], 0);
            ++index;
            ok &= archive_write_header(writer, entry) == ARCHIVE_OK;
            ok &= archive_write_data(writer, file.second.constData(), file.second.size()) == file.second.size();
            archive_entry_free(entry);
        }
        archive_write_close(writer);
        archive_write_free(writer);
        return ok;
    }
private slots:
    void duplicateArchiveMembersRemainDistinct() {
        QTemporaryDir temp;
        const QString path = temp.filePath("duplicate.cbz");
        QVERIFY(writeArchive(path, {{"same.png", png(Qt::red)}, {"same.png", png(Qt::blue)}}));
        ik::Document document; document.open(path);
        QTRY_VERIFY_WITH_TIMEOUT(!document.isBusy() && !document.currentImage().isNull(), 5000);
        QCOMPARE(document.count(), 2);
        QCOMPARE(document.entries()[0].memberOccurrence, 0);
        QCOMPARE(document.entries()[1].memberOccurrence, 1);
        QVERIFY(document.entries()[0].key() != document.entries()[1].key());
        QCOMPARE(document.currentImage().pixelColor(0, 0), QColor(Qt::red));
        document.goTo(1);
        QTRY_VERIFY_WITH_TIMEOUT(!document.isBusy(), 5000);
        QCOMPARE(document.currentImage().pixelColor(0, 0), QColor(Qt::blue));
        document.goTo(0);
        QTRY_VERIFY_WITH_TIMEOUT(!document.isBusy(), 5000);
        QCOMPARE(document.currentImage().pixelColor(0, 0), QColor(Qt::red));
        document.setSortOrder(ik::SortOrder::NameDescending);
        QTRY_COMPARE_WITH_TIMEOUT(document.currentEntry().memberOccurrence, 0, 5000);
        QCOMPARE(document.count(), 2);
    }
    void explicitUnknownExtensionAndHiddenImagesAreKept() {
        QTemporaryDir temp;
        const QString unknown = temp.filePath("chosen.data");
        QFile bytes(unknown); QVERIFY(bytes.open(QIODevice::WriteOnly));
        const auto encoded = png(Qt::red); QCOMPARE(bytes.write(encoded), encoded.size()); bytes.close();
        const QString visible = temp.filePath("visible.png");
        const QString hidden = temp.filePath(".chosen.png");
        QVERIFY(saveImage(visible, Qt::blue)); QVERIFY(saveImage(hidden, Qt::green));
        QMimeData mime; mime.setUrls({QUrl::fromLocalFile(unknown), QUrl::fromLocalFile(visible)});
        const auto paths = ik::imageDropPaths(&mime); QCOMPARE(paths.size(), 2);
        ik::Document document; document.openFiles(paths);
        QTRY_VERIFY_WITH_TIMEOUT(!document.isBusy() && !document.currentImage().isNull(), 5000);
        QCOMPARE(document.count(), 2);
        QCOMPARE(document.currentEntry().path, unknown);
        QCOMPARE(document.currentImage().pixelColor(0, 0), QColor(Qt::red));
        document.open(hidden);
        QTRY_VERIFY_WITH_TIMEOUT(!document.isBusy() && document.currentEntry().path == hidden, 5000);
        QCOMPARE(document.currentImage().pixelColor(0, 0), QColor(Qt::green));
        QCOMPARE(document.count(), 2); // Explicit hidden file plus normal sibling.
        document.refreshCollection();
        QTRY_VERIFY_WITH_TIMEOUT(!document.isBusy() && !document.currentImage().isNull(), 5000);
        QCOMPARE(document.currentEntry().path, hidden);
        QCOMPARE(document.currentImage().pixelColor(0, 0), QColor(Qt::green));
    }
    void archiveModificationTimesControlSorting() {
        QTemporaryDir temp;
        const QString path = temp.filePath("dates.cbz");
        QVERIFY(writeArchive(path, {{"1.png", png(Qt::red)}, {"2.png", png(Qt::blue)}}, "zip", {2000, 1000}));
        ik::Document document; document.open(path);
        QTRY_VERIFY_WITH_TIMEOUT(!document.isBusy() && !document.currentImage().isNull(), 5000);
        QCOMPARE(document.entries()[0].modifiedMs, qint64(2000000));
        document.setSortOrder(ik::SortOrder::Modified);
        QTRY_COMPARE_WITH_TIMEOUT(document.entries()[0].name, QString("2.png"), 5000);
        QCOMPARE(document.currentEntry().name, QString("1.png"));
        QCOMPARE(document.currentImage().pixelColor(0, 0), QColor(Qt::red));
    }
    void explicitCollectionWatchesFilesAndRetainsScopeAfterRemoval() {
        QTemporaryDir first, second;
        const QString a = first.filePath("1.png"), b = second.filePath("2.png");
        QVERIFY(saveImage(a, Qt::red)); QVERIFY(saveImage(b, Qt::blue));
        QVERIFY(saveImage(second.filePath("unselected.png"), Qt::yellow));
        ik::Document document; document.openFiles({QDir::current().relativeFilePath(a), QDir::current().relativeFilePath(b)});
        QTRY_VERIFY_WITH_TIMEOUT(!document.isBusy() && !document.currentImage().isNull(), 5000);
        QVERIFY(saveImage(a, Qt::green, {69, 41}));
        QTRY_COMPARE_WITH_TIMEOUT(document.currentImage().size(), QSize(69, 41), 5000);
        QCOMPARE(document.currentImage().pixelColor(0, 0), QColor(Qt::green));
        QCOMPARE(document.count(), 2);
        document.goTo(1); QTRY_VERIFY_WITH_TIMEOUT(!document.isBusy(), 5000);
        const QString removedKey = document.currentEntry().key();
        QVERIFY(QFile::remove(b)); document.removeEntry(removedKey);
        QTRY_VERIFY_WITH_TIMEOUT(!document.isBusy() && !document.currentImage().isNull(), 5000);
        QCOMPARE(document.count(), 1); QCOMPARE(document.currentEntry().path, a);
        QCOMPARE(document.collectionPaths(), QStringList({a})); QVERIFY(document.isFileScoped());
        document.refreshCollection(); QTRY_VERIFY_WITH_TIMEOUT(!document.isBusy(), 5000);
        QCOMPARE(document.count(), 1); QCOMPARE(document.currentEntry().path, a);
        document.removeEntry(removedKey); QCOMPARE(document.count(), 1);
        const QString finalKey = document.currentEntry().key();
        QVERIFY(QFile::remove(a)); document.removeEntry(finalKey);
        QCOMPARE(document.count(), 0); QCOMPARE(document.currentIndex(), -1);
        QVERIFY(document.currentImage().isNull()); QVERIFY(!document.isBusy());
        QVERIFY(document.collectionPaths().isEmpty());
    }
    void removingLastFolderEntryRetainsFolderMonitoring() {
        QTemporaryDir temp;
        const QString a = temp.filePath("1.png"); QVERIFY(saveImage(a, Qt::red));
        ik::Document document; document.open(temp.path());
        QTRY_VERIFY_WITH_TIMEOUT(!document.isBusy() && !document.currentImage().isNull(), 5000);
        const auto key = document.currentEntry().key();
        QVERIFY(QFile::remove(a)); document.removeEntry(key);
        QCOMPARE(document.count(), 0); QCOMPARE(document.collectionPaths(), QStringList({temp.path()}));
        QVERIFY(saveImage(temp.filePath("new.png"), Qt::blue));
        QTRY_VERIFY_WITH_TIMEOUT(!document.isBusy() && document.count() == 1 && !document.currentImage().isNull(), 5000);
        QCOMPARE(document.currentEntry().name, QString("new.png"));
        QCOMPARE(document.currentImage().pixelColor(0, 0), QColor(Qt::blue));
    }
    void eachFailedRequestReportsItsOwnError() {
        QTemporaryDir temp; ik::Document document;
        QSignalSpy errors(&document, &ik::Document::errorOccurred);
        const QString missing = temp.filePath("missing.png");
        for (int attempt = 1; attempt <= 2; ++attempt) {
            document.open(missing);
            QTRY_VERIFY_WITH_TIMEOUT(!document.isBusy(), 5000);
            QCOMPARE(errors.size(), attempt);
        }
        const QString broken = temp.filePath("broken.png");
        QFile file(broken); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("broken"); file.close();
        document.openFiles({broken}, false);
        QTRY_VERIFY_WITH_TIMEOUT(!document.isBusy(), 5000); const int first = errors.size();
        document.goTo(0); QTRY_VERIFY_WITH_TIMEOUT(!document.isBusy(), 5000);
        QCOMPARE(errors.size(), first + 1);
    }
    void nativeUnsupportedTypeIsNotAPageError() {
#ifdef Q_OS_MACOS
        QTemporaryDir temp; const QString unsupported = temp.filePath("unsupported.svg");
        QFile file(unsupported); QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"10\" height=\"10\"><rect width=\"10\" height=\"10\"/></svg>"); file.close();
        const auto invalid = ik::decodeNativeImage(unsupported, {}, 0);
        QVERIFY(invalid.image.isNull()); QVERIFY(!invalid.error.isEmpty());
        QVERIFY(!invalid.error.contains(QStringLiteral("페이지")));
        const QString valid = temp.filePath("valid.png"); QVERIFY(saveImage(valid, Qt::red));
        const auto outside = ik::decodeNativeImage(valid, {}, 1);
        QVERIFY(outside.image.isNull()); QVERIFY(outside.error.contains(QStringLiteral("페이지")));
#endif
    }
    void neighborFailureIsObservableAndCanRecover() {
        QTemporaryDir temp;
        QVERIFY(saveImage(temp.filePath("0.png"), Qt::red));
        const QString broken = temp.filePath("1.png");
        QFile file(broken); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("broken"); file.close();
        ik::Document document; QSignalSpy failed(&document, &ik::Document::neighborFailed);
        document.open(temp.path());
        QTRY_VERIFY_WITH_TIMEOUT(!document.isBusy() && !document.currentImage().isNull(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!failed.isEmpty(), 5000);
        QCOMPARE(failed.first()[0].toInt(), 1); QVERIFY(!document.neighborError(1).isEmpty());
        QCOMPARE(document.currentImage().pixelColor(0, 0), QColor(Qt::red));
        QVERIFY(saveImage(broken, Qt::green)); document.goTo(1);
        QTRY_VERIFY_WITH_TIMEOUT(!document.isBusy() && !document.currentImage().isNull(), 5000);
        QCOMPARE(document.currentImage().pixelColor(0, 0), QColor(Qt::green));
        QVERIFY(document.neighborError(1).isEmpty());
    }
    void archiveFormats_data() {
        QTest::addColumn<QByteArray>("format");
        QTest::newRow("zip") << QByteArray("zip");
        QTest::newRow("7z") << QByteArray("7z");
        QTest::newRow("tar") << QByteArray("tar");
    }
    void archiveFormats() {
        QFETCH(QByteArray, format); QTemporaryDir temp;
        const QString path = temp.filePath("archive." + QString::fromLatin1(format));
        QVERIFY(writeArchive(path, {{"1.png", png(Qt::red)}, {"folder/2.png", png(Qt::blue)}}, format));
        ik::Document document; document.open(path);
        QTRY_VERIFY_WITH_TIMEOUT(!document.isBusy() && !document.currentImage().isNull(), 5000);
        QCOMPARE(document.count(), 2); QCOMPARE(document.currentImage().pixelColor(0, 0), QColor(Qt::red));
        document.goTo(1); QTRY_VERIFY_WITH_TIMEOUT(!document.isBusy(), 5000);
        QCOMPARE(document.currentImage().pixelColor(0, 0), QColor(Qt::blue));
    }
    void displayP3PixelsAreConvertedAndAlphaSurvives() {
        QTemporaryDir temp;
        QImage source(3, 2, QImage::Format_RGBA8888); source.setColorSpace(QColorSpace::DisplayP3);
        source.fill(QColor(200, 120, 90, 128)); source.setPixelColor(2, 1, QColor(50, 170, 140, 220));
        const QImage expected = source.convertedToColorSpace(QColorSpace::SRgb).convertToFormat(QImage::Format_RGBA8888);
        QVERIFY(expected.pixelColor(0, 0) != source.pixelColor(0, 0));
        const QString path = temp.filePath("icc.png"); QVERIFY(source.save(path));
        ik::Document document; document.openFiles({path}, false);
        QTRY_VERIFY_WITH_TIMEOUT(!document.isBusy() && !document.currentImage().isNull(), 5000);
        QCOMPARE(document.currentImage().colorSpace(), QColorSpace(QColorSpace::SRgb));
        QCOMPARE(document.currentImage().pixelColor(0, 0), expected.pixelColor(0, 0));
        QCOMPARE(document.currentImage().pixelColor(2, 1), expected.pixelColor(2, 1));
    }
    void jpegExifOrientationIsAppliedToPixels() {
        QTemporaryDir temp; QImage source(12, 8, QImage::Format_RGB32);
        for (int y = 0; y < source.height(); ++y) for (int x = 0; x < source.width(); ++x)
            source.setPixelColor(x, y, x < 6 ? (y < 4 ? QColor(Qt::red) : QColor(Qt::green)) : (y < 4 ? QColor(Qt::blue) : QColor(Qt::yellow)));
        QByteArray bytes; QBuffer buffer(&bytes); QVERIFY(buffer.open(QIODevice::WriteOnly)); QVERIFY(source.save(&buffer, "JPEG", 95)); buffer.close();
        const QByteArray exif = QByteArray::fromHex("45786966000049492a0008000000010012010300010000000600000000000000");
        QByteArray app1 = QByteArray::fromHex("ffe1"); const int length = exif.size() + 2;
        app1.append(char(length >> 8)); app1.append(char(length & 255)); app1.append(exif); bytes.insert(2, app1);
        const QString path = temp.filePath("oriented.jpg"); QFile file(path); QVERIFY(file.open(QIODevice::WriteOnly)); QCOMPARE(file.write(bytes), bytes.size()); file.close();
        QImageReader reader(path); reader.setAutoTransform(false); const QImage raw = reader.read(); QVERIFY(!raw.isNull());
        const QImage expected = raw.transformed(QTransform().rotate(90)).convertToFormat(QImage::Format_RGBA8888);
        ik::Document document; document.openFiles({path}, false);
        QTRY_VERIFY_WITH_TIMEOUT(!document.isBusy() && !document.currentImage().isNull(), 5000);
        QCOMPARE(document.currentImage().size(), QSize(8, 12));
        QCOMPARE(document.currentImage().pixelColor(0, 0), expected.pixelColor(0, 0));
        QCOMPARE(document.currentImage().pixelColor(7, 11), expected.pixelColor(7, 11));
    }
    void invalidOpenKeepsWorkingDocument() {
        QTemporaryDir temp;
        QVERIFY(saveImage(temp.filePath("1.png"), Qt::red));
        QVERIFY(saveImage(temp.filePath("2.png"), Qt::blue));
        ik::Document document;
        document.open(temp.path());
        QTRY_VERIFY_WITH_TIMEOUT(!document.isBusy() && !document.currentImage().isNull(), 5000);
        const QString selected = document.currentEntry().key();
        QSignalSpy errors(&document,&ik::Document::errorOccurred);
        document.open(temp.filePath("missing-folder"));
        QTRY_VERIFY_WITH_TIMEOUT(!errors.isEmpty() && !document.isBusy(), 5000);
        QCOMPARE(document.count(), 2);
        QCOMPARE(document.currentEntry().key(), selected);
        QCOMPARE(document.currentImage().pixelColor(0,0), QColor(Qt::red));
        document.navigate(1);
        QTRY_VERIFY_WITH_TIMEOUT(!document.isBusy(), 5000);
        QCOMPARE(document.currentImage().pixelColor(0,0), QColor(Qt::blue));
    }
    void multipleFoldersFilesAndArchivesCombineWithoutDuplicates() {
        QTemporaryDir temp;
        QVERIFY(QDir().mkpath(temp.filePath("a")));
        QVERIFY(QDir().mkpath(temp.filePath("b")));
        QVERIFY(saveImage(temp.filePath("a/1.png"), Qt::red));
        QVERIFY(saveImage(temp.filePath("a/2.png"), Qt::blue));
        QVERIFY(saveImage(temp.filePath("b/3.png"), Qt::green));
        QVERIFY(writeArchive(temp.filePath("comic.cbz"), {{"4.png", png(Qt::cyan)}, {"5.png", png(Qt::yellow)}}));
        ik::Document document;
        document.openFiles({temp.filePath("a"),temp.filePath("b"),temp.filePath("a/1.png"),temp.filePath("comic.cbz")});
        QTRY_VERIFY_WITH_TIMEOUT(!document.isBusy() && !document.currentImage().isNull(), 5000);
        QCOMPARE(document.count(), 5);
        document.goTo(4);
        QTRY_VERIFY_WITH_TIMEOUT(!document.isBusy(), 5000);
        QVERIFY(document.currentEntry().isArchive());
        QCOMPARE(document.currentImage().pixelColor(0,0), QColor(Qt::yellow));
    }
    void closeCancelsPendingOpenAndReopensNormally() {
        QTemporaryDir temp;
        QVERIFY(saveImage(temp.filePath("1.png"), Qt::red));
        ik::Document document;
        document.open(temp.path());
        document.close();
        QTest::qWait(150);
        QCOMPARE(document.count(), 0);
        QCOMPARE(document.currentIndex(), -1);
        QVERIFY(!document.isBusy());
        QVERIFY(document.currentImage().isNull());
        document.open(temp.path());
        QTRY_VERIFY_WITH_TIMEOUT(!document.isBusy() && !document.currentImage().isNull(), 5000);
        QCOMPARE(document.count(), 1);
    }
    void sortingDuringOpenKeepsRequestedCollection() {
        QTemporaryDir oldFolder, newFolder;
        QVERIFY(saveImage(oldFolder.filePath("old.png"), Qt::red));
        QVERIFY(saveImage(newFolder.filePath("new1.png"), Qt::blue));
        QVERIFY(saveImage(newFolder.filePath("new2.png"), Qt::green));
        ik::Document document;
        document.open(oldFolder.path());
        QTRY_VERIFY_WITH_TIMEOUT(!document.isBusy() && !document.currentImage().isNull(), 5000);
        document.open(newFolder.path());
        document.setSortOrder(ik::SortOrder::NameDescending);
        QTRY_VERIFY_WITH_TIMEOUT(!document.isBusy() && document.count()==2, 5000);
        QCOMPARE(document.entries().first().name, QString("new2.png"));
        QVERIFY(document.currentEntry().path.startsWith(newFolder.path()));
    }
    void selectedFileScopeSurvivesSortRefreshAndFailedOpen() {
        QTemporaryDir temp;
        const QString selected = temp.filePath("selected.png");
        QVERIFY(saveImage(selected, Qt::green));
        QVERIFY(saveImage(temp.filePath("sibling1.png"), Qt::red));
        QVERIFY(saveImage(temp.filePath("sibling2.png"), Qt::yellow));
        ik::Document document;
        document.openFiles({selected}, false);
        // Restart while the first scan is pending, before its completion can
        // reach this thread. Sorting must retain the requested file scope.
        document.setSortOrder(ik::SortOrder::NameDescending);
        QTRY_VERIFY_WITH_TIMEOUT(!document.isBusy() && !document.currentImage().isNull(), 5000);
        QCOMPARE(document.count(), 1);
        QCOMPARE(document.currentEntry().path, selected);
        QCOMPARE(document.currentImage().pixelColor(0, 0), QColor(Qt::green));

        QSignalSpy ready(&document, &ik::Document::imageReady);
        document.setSortOrder(ik::SortOrder::Modified);
        QTRY_VERIFY_WITH_TIMEOUT(!ready.isEmpty(), 5000);
        QCOMPARE(document.count(), 1);
        QVERIFY(saveImage(selected, Qt::blue, {61, 37}));
        QTRY_COMPARE_WITH_TIMEOUT(document.currentImage().size(), QSize(61, 37), 5000);
        QCOMPARE(document.currentImage().pixelColor(0, 0), QColor(Qt::blue));
        QCOMPARE(document.count(), 1);

        QSignalSpy errors(&document, &ik::Document::errorOccurred);
        document.open(temp.filePath("missing-folder"));
        QTRY_VERIFY_WITH_TIMEOUT(!errors.isEmpty() && !document.isBusy(), 5000);
        QCOMPARE(document.count(), 1);
        QCOMPARE(document.currentEntry().path, selected);
        QVERIFY(saveImage(selected, Qt::cyan, {63, 39}));
        QTRY_COMPARE_WITH_TIMEOUT(document.currentImage().size(), QSize(63, 39), 5000);
        QCOMPARE(document.currentImage().pixelColor(0, 0), QColor(Qt::cyan));
        QCOMPARE(document.count(), 1);

        QSignalSpy changes(&document, &ik::Document::entriesChanged);
        QVERIFY(saveImage(temp.filePath("sibling3.png"), Qt::magenta));
        document.adjacentContainer(1);
        QTest::qWait(350);
        QVERIFY(changes.isEmpty());
        QCOMPARE(document.currentEntry().path, selected);
        document.close();
        document.open(selected);
        QTRY_VERIFY_WITH_TIMEOUT(!document.isBusy() && !document.currentImage().isNull(), 5000);
        QCOMPARE(document.count(), 4);
        QCOMPARE(document.currentEntry().path, selected);
    }
    void explicitDirectoryScopeStillIncludesImages() {
        QTemporaryDir temp;
        QVERIFY(saveImage(temp.filePath("1.png"), Qt::red));
        QVERIFY(saveImage(temp.filePath("2.png"), Qt::blue));
        ik::Document document;
        document.openFiles({temp.path()}, false);
        QTRY_VERIFY_WITH_TIMEOUT(!document.isBusy() && !document.currentImage().isNull(), 5000);
        QCOMPARE(document.count(), 2);
        QVERIFY(saveImage(temp.filePath("3.png"), Qt::green));
        QTRY_COMPARE_WITH_TIMEOUT(document.count(), 3, 5000);
    }
    void selectedArchiveScopeRefreshesWithoutFollowingParent() {
        QTemporaryDir temp;
        const QString selected = temp.filePath("a.cbz");
        QVERIFY(writeArchive(selected, {{"1.png", png(Qt::red)}, {"2.png", png(Qt::green)}}));
        QVERIFY(writeArchive(temp.filePath("b.cbz"), {{"next.png", png(Qt::yellow)}}));
        ik::Document document;
        document.openFiles({selected}, false);
        document.setSortOrder(ik::SortOrder::NameDescending);
        QTRY_VERIFY_WITH_TIMEOUT(!document.isBusy() && !document.currentImage().isNull(), 5000);
        QCOMPARE(document.count(), 2);
        QCOMPARE(document.currentEntry().archivePath, selected);
        document.goTo(1);
        QTRY_VERIFY_WITH_TIMEOUT(!document.isBusy(), 5000);
        QCOMPARE(document.currentImage().pixelColor(0, 0), QColor(Qt::red));
        QSignalSpy changes(&document, &ik::Document::entriesChanged);
        document.adjacentContainer(1);
        QVERIFY(saveImage(temp.filePath("unrelated.png"), Qt::blue));
        QTest::qWait(350);
        QVERIFY(changes.isEmpty());
        QCOMPARE(document.currentEntry().archivePath, selected);
        QVERIFY(writeArchive(selected, {{"1.png", png(Qt::cyan)}, {"2.png", png(Qt::green)}, {"3.png", png(Qt::blue)}}));
        QTRY_VERIFY_WITH_TIMEOUT(!document.isBusy() && document.count() == 3, 5000);
        QCOMPARE(document.currentEntry().archivePath, selected);
        QCOMPARE(document.currentEntry().name, QString("1.png"));
        QCOMPARE(document.currentImage().pixelColor(0, 0), QColor(Qt::cyan));
    }
    void nativeDecoderPreservesImageRows() {
#ifdef Q_OS_MACOS
        QTemporaryDir temp;
        QImage source(3, 3, QImage::Format_RGBA8888);
        for (int x = 0; x < 3; ++x) {
            source.setPixelColor(x, 0, Qt::red);
            source.setPixelColor(x, 1, Qt::blue);
            source.setPixelColor(x, 2, QColor(0, 255, 0, 128));
        }
        QVERIFY(source.save(temp.filePath("native.png")));
        const auto decoded = ik::decodeNativeImage(temp.filePath("native.png"), {}, 0);
        QCOMPARE(decoded.image.size(), source.size());
        QCOMPARE(decoded.image.pixelColor(1, 0), source.pixelColor(1, 0));
        QCOMPARE(decoded.image.pixelColor(1, 2), source.pixelColor(1, 2));
        QVERIFY(!ik::nativeImageExtensions().isEmpty());
#else
        QSKIP("Native ImageIO decoder is available on macOS.");
#endif
    }
    void animationPlaysAndPauses() {
        QTemporaryDir temp;
        const QByteArray animation = QByteArray::fromHex(
            "47494638396101000100800000ff00000000ff"
            "21ff0b4e45545343415045322e300301000000"
            "21f90400050000002c0000000001000100000202440100"
            "21f90400050000002c00000000010001000002024c01003b");
        QFile file(temp.filePath("animation.gif"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(animation), animation.size());
        file.close();
        ik::Document document;
        document.open(file.fileName());
        QTRY_VERIFY_WITH_TIMEOUT(!document.isBusy() && !document.currentImage().isNull(), 10000);
        QCOMPARE(document.currentInfo().pageCount, 1);
        QVERIFY(document.currentInfo().animated);
        QTRY_COMPARE_WITH_TIMEOUT(document.currentImage().pixelColor(0, 0), QColor(Qt::blue), 1000);
        document.setAnimationPaused(true);
        const QImage paused = document.currentImage();
        QTest::qWait(160);
        QCOMPARE(document.currentImage(), paused);
    }
    void multipageTiffNavigation() {
        if (!QImageReader::supportedImageFormats().contains("tiff")) QSKIP("TIFF plugin unavailable");
        QTemporaryDir temp;
        QByteArray bytes;
        QDataStream data(&bytes, QIODevice::WriteOnly);
        data.setByteOrder(QDataStream::LittleEndian);
        data.writeRawData("II", 2);
        data << quint16(42) << quint32(8);
        for (int page = 0; page < 2; ++page) {
            data << quint16(10);
            const auto tag = [&](quint16 id, quint16 type, quint32 count, quint32 value) {
                data << id << type << count << value;
            };
            tag(256, 4, 1, 2); tag(257, 4, 1, 2);
            tag(258, 3, 3, 260 + page * 6);
            tag(259, 3, 1, 1); tag(262, 3, 1, 2);
            tag(273, 4, 1, 272 + page * 12);
            tag(277, 3, 1, 3); tag(278, 4, 1, 2);
            tag(279, 4, 1, 12); tag(284, 3, 1, 1);
            data << quint32(page == 0 ? 134 : 0);
        }
        for (int i = 0; i < 6; ++i) data << quint16(8);
        for (int page = 0; page < 2; ++page)
            for (int i = 0; i < 4; ++i) data << quint8(page == 0 ? 255 : 0) << quint8(0) << quint8(page == 1 ? 255 : 0);
        QFile file(temp.filePath("pages.tiff"));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(bytes), bytes.size());
        file.close();
        ik::Document document;
        document.open(file.fileName());
        QTRY_VERIFY_WITH_TIMEOUT(!document.isBusy() && !document.currentImage().isNull(), 10000);
        QCOMPARE(document.currentInfo().pageCount, 2);
        QCOMPARE(document.currentImage().pixelColor(0, 0), QColor(Qt::red));
        document.setPage(1);
        QTRY_VERIFY_WITH_TIMEOUT(!document.isBusy(), 10000);
        QCOMPARE(document.currentInfo().page, 1);
        QCOMPARE(document.currentImage().pixelColor(0, 0), QColor(Qt::blue));
    }
    void multipageRefreshRetainsPageAndClampsChangedDocuments() {
        if (!QImageReader::supportedImageFormats().contains("tiff")) QSKIP("TIFF plugin unavailable");
        QTemporaryDir temp;
        const QString path = temp.filePath("pages.tiff");
        const auto replace = [&](const QByteArray &bytes) {
            QSaveFile file(path);
            return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size() && file.commit();
        };
        QVERIFY(replace(tiff({Qt::red, Qt::blue})));
        ik::Document document; document.open(path);
        QTRY_VERIFY_WITH_TIMEOUT(!document.isBusy() && !document.currentImage().isNull(), 5000);
        document.setPage(1);
        QTRY_VERIFY_WITH_TIMEOUT(!document.isBusy() && document.currentInfo().page == 1, 5000);
        QSignalSpy refreshes(&document, &ik::Document::entriesChanged);
        QSignalSpy ready(&document, &ik::Document::imageReady);
        QSignalSpy errors(&document, &ik::Document::errorOccurred);
        auto *watcher = document.findChild<QFileSystemWatcher *>(); QVERIFY(watcher);
        QSignalSpy filesystemChanges(watcher, &QFileSystemWatcher::directoryChanged);

        // Settings and other non-image files can change in the watched folder.
        QFile settings(temp.filePath("settings.ini")); QVERIFY(settings.open(QIODevice::WriteOnly));
        settings.write("lastPage=2\n"); settings.close();
        QTRY_VERIFY_WITH_TIMEOUT(!filesystemChanges.isEmpty(), 5000);
        QCOMPARE(document.currentInfo().page, 1);
        QCOMPARE(document.currentImage().pixelColor(0, 0), QColor(Qt::blue));
        QTest::qWait(800); QCOMPARE(document.currentInfo().page, 1);
        QVERIFY(refreshes.isEmpty()); QVERIFY(ready.isEmpty());
        for (const auto &event : ready) QCOMPARE(qvariant_cast<ik::ImageInfo>(event[1]).page, 1);

        document.refreshCollection();
        QTRY_VERIFY_WITH_TIMEOUT(!document.isBusy(), 5000);
        QCOMPARE(document.currentInfo().page, 1);
        ready.clear(); refreshes.clear();
        QVERIFY(replace(tiff({Qt::red, Qt::green})));
        QTRY_VERIFY_WITH_TIMEOUT(!refreshes.isEmpty() && !document.isBusy(), 5000);
        QCOMPARE(document.currentInfo().page, 1);
        QCOMPARE(document.currentImage().pixelColor(0, 0), QColor(Qt::green));
        for (const auto &event : ready) QCOMPARE(qvariant_cast<ik::ImageInfo>(event[1]).page, 1);

        refreshes.clear();
        QVERIFY(replace(tiff({Qt::yellow})));
        QTRY_VERIFY_WITH_TIMEOUT(!refreshes.isEmpty() && !document.isBusy(), 5000);
        QCOMPARE(document.currentInfo().pageCount, 1); QCOMPARE(document.currentInfo().page, 0);
        QCOMPARE(document.currentImage().pixelColor(0, 0), QColor(Qt::yellow));
        QVERIFY(errors.isEmpty());

        QVERIFY(replace(tiff({Qt::red, Qt::blue})));
        document.refreshCollection();
        QTRY_VERIFY_WITH_TIMEOUT(!document.isBusy() && document.currentInfo().pageCount == 2, 5000);
        document.setPage(1);
        QTRY_VERIFY_WITH_TIMEOUT(!document.isBusy() && document.currentInfo().page == 1, 5000);
        document.open(path); // A deliberate new open starts at the first page.
        QTRY_VERIFY_WITH_TIMEOUT(!document.isBusy(), 5000);
        QCOMPARE(document.currentInfo().page, 0);
    }
    void naturalSortAndSelectedFile() {
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        QVERIFY(saveImage(temp.filePath("frame10.png"), Qt::blue));
        QVERIFY(saveImage(temp.filePath("frame2.png"), Qt::green));
        QVERIFY(saveImage(temp.filePath("frame1.png"), Qt::red));
        ik::Document document;
        QSignalSpy ready(&document, &ik::Document::imageReady);
        document.open(temp.filePath("frame2.png"));
        QTRY_VERIFY_WITH_TIMEOUT(!ready.isEmpty(), 10000);
        QCOMPARE(document.count(), 3);
        QCOMPARE(document.entries()[0].name, QString("frame1.png"));
        QCOMPARE(document.entries()[1].name, QString("frame2.png"));
        QCOMPARE(document.entries()[2].name, QString("frame10.png"));
        QCOMPARE(document.currentIndex(), 1);
        QCOMPARE(document.currentImage().pixelColor(0, 0), QColor(Qt::green));
        const ik::ImageInfo beforeSort = document.currentInfo();
        ready.clear();
        document.setSortOrder(ik::SortOrder::NameDescending);
        QTRY_VERIFY_WITH_TIMEOUT(!ready.isEmpty(), 10000);
        QCOMPARE(document.entries()[0].name, QString("frame10.png"));
        QCOMPARE(document.currentEntry().name, QString("frame2.png"));
        QCOMPARE(document.currentImage().pixelColor(0, 0), QColor(Qt::green));
        QCOMPARE(document.currentInfo().size, beforeSort.size);
        QCOMPARE(document.currentInfo().metadata, beforeSort.metadata);
        QVERIFY(!document.isBusy());
    }
    void archiveUsesMemoryAndSkipsNonImages() {
        QTemporaryDir temp;
        QVERIFY(writeArchive(temp.filePath("comic.cbz"), {
            {"../outside.png", png(Qt::red)},
            {"folder/page2.png", png(Qt::green)},
            {"readme.txt", QByteArray("not an image")}
        }));
        ik::Document document;
        QSignalSpy ready(&document, &ik::Document::imageReady);
        document.open(temp.filePath("comic.cbz"));
        QTRY_VERIFY_WITH_TIMEOUT(!ready.isEmpty(), 10000);
        QCOMPARE(document.count(), 2);
        QVERIFY(document.currentEntry().isArchive());
        QVERIFY(!QFile::exists(temp.filePath("outside.png")));
        QCOMPARE(document.currentImage().pixelColor(0, 0), QColor(Qt::red));
        document.goTo(1);
        QTRY_VERIFY_WITH_TIMEOUT(!document.isBusy(), 10000);
        QCOMPARE(document.currentImage().pixelColor(0, 0), QColor(Qt::green));
    }
    void latestOpenAndNavigationWin() {
        QTemporaryDir first;
        QTemporaryDir second;
        QVERIFY(saveImage(first.filePath("stale.png"), Qt::red));
        QVERIFY(saveImage(second.filePath("1.png"), Qt::green));
        QVERIFY(saveImage(second.filePath("2.png"), Qt::red));
        QVERIFY(saveImage(second.filePath("3.png"), Qt::blue));
        ik::Document document;
        document.open(first.path());
        document.open(second.path());
        QTRY_VERIFY_WITH_TIMEOUT(!document.isBusy() && !document.currentImage().isNull(), 10000);
        QCOMPARE(document.count(), 3);
        document.goTo(1);
        document.goTo(2);
        QTRY_VERIFY_WITH_TIMEOUT(!document.isBusy(), 10000);
        QCOMPARE(document.currentEntry().name, QString("3.png"));
        QCOMPARE(document.currentImage().pixelColor(0, 0), QColor(Qt::blue));
    }
    void failedDecodeRecovers() {
        QTemporaryDir temp;
        QFile broken(temp.filePath("1.png"));
        QVERIFY(broken.open(QIODevice::WriteOnly));
        broken.write("broken image");
        broken.close();
        QVERIFY(saveImage(temp.filePath("2.png"), Qt::green));
        ik::Document document;
        QSignalSpy errors(&document, &ik::Document::errorOccurred);
        document.open(broken.fileName());
        QTRY_VERIFY_WITH_TIMEOUT(!errors.isEmpty() && !document.isBusy(), 10000);
        QVERIFY(document.currentImage().isNull());
        document.goTo(1);
        QTRY_VERIFY_WITH_TIMEOUT(!document.isBusy(), 10000);
        QVERIFY(!document.currentImage().isNull());
        QCOMPARE(document.currentImage().pixelColor(0, 0), QColor(Qt::green));
    }
    void folderChangesRefreshList() {
        QTemporaryDir temp;
        QVERIFY(saveImage(temp.filePath("1.png"), Qt::red));
        ik::Document document;
        document.open(temp.path());
        QTRY_VERIFY_WITH_TIMEOUT(!document.isBusy(), 10000);
        QCOMPARE(document.count(), 1);
        QVERIFY(saveImage(temp.filePath("2.png"), Qt::blue));
        QTRY_COMPARE_WITH_TIMEOUT(document.count(), 2, 10000);
        QVERIFY(QFile::remove(temp.filePath("1.png")));
        QTRY_COMPARE_WITH_TIMEOUT(document.count(), 1, 10000);
        QTRY_VERIFY_WITH_TIMEOUT(!document.isBusy(), 10000);
        QCOMPARE(document.currentEntry().name, QString("2.png"));
    }
    void unrelatedFolderWritesKeepImagesAndCache() {
        QTemporaryDir temp;
        const QString first = temp.filePath("1.png"), second = temp.filePath("2.png");
        QVERIFY(saveImage(first, Qt::red)); QVERIFY(saveImage(second, Qt::blue));
        ik::Document document; document.open(temp.path());
        QTRY_VERIFY_WITH_TIMEOUT(!document.isBusy() && !document.cachedImage(1).isNull(), 5000);
        const auto currentKey = document.currentImage().cacheKey(), neighborKey = document.cachedImage(1).cacheKey();
        auto *watcher = document.findChild<QFileSystemWatcher *>(); QVERIFY(watcher);
        QSignalSpy filesystemChanges(watcher, &QFileSystemWatcher::directoryChanged);
        QSignalSpy entries(&document, &ik::Document::entriesChanged), ready(&document, &ik::Document::imageReady);
        QSignalSpy loading(&document, &ik::Document::loadingChanged);
        QFile settings(temp.filePath("settings.ini")); QVERIFY(settings.open(QIODevice::WriteOnly));
        settings.write("geometry=unchanged\n"); settings.close();
        QTRY_VERIFY_WITH_TIMEOUT(!filesystemChanges.isEmpty(), 5000);
        QTest::qWait(500);
        QVERIFY(entries.isEmpty()); QVERIFY(ready.isEmpty()); QVERIFY(loading.isEmpty());
        QCOMPARE(document.currentImage().cacheKey(), currentKey);
        QCOMPARE(document.cachedImage(1).cacheKey(), neighborKey);

        QVERIFY(saveImage(temp.filePath("3.png"), Qt::yellow));
        QTRY_COMPARE_WITH_TIMEOUT(document.count(), 3, 5000);
        QCOMPARE(document.currentImage().cacheKey(), currentKey);
        QCOMPARE(document.cachedImage(1).cacheKey(), neighborKey);
        QSaveFile changed(second); QVERIFY(changed.open(QIODevice::WriteOnly));
        const auto green = png(Qt::green); QCOMPARE(changed.write(green), green.size()); QVERIFY(changed.commit());
        QTRY_VERIFY_WITH_TIMEOUT(!document.cachedImage(1).isNull()
            && document.cachedImage(1).pixelColor(0, 0) == QColor(Qt::green), 5000);
        QCOMPARE(document.currentImage().cacheKey(), currentKey);
        QVERIFY(QFile::rename(second, temp.filePath("4.png")));
        QTRY_COMPARE_WITH_TIMEOUT(document.entries().last().name, QString("4.png"), 5000);
        QCOMPARE(document.currentImage().cacheKey(), currentKey);
        QVERIFY(QFile::remove(temp.filePath("4.png")));
        QTRY_COMPARE_WITH_TIMEOUT(document.count(), 2, 5000);
        QCOMPARE(document.currentImage().cacheKey(), currentKey);
    }
    void sameSizeAndMtimeReplacementsStillReload() {
#ifndef Q_OS_UNIX
        QSKIP("Unix inode/ctime identity regression");
#endif
        QTemporaryDir temp; const QString path = temp.filePath("image.bmp");
        const auto bmp = [](const QColor &color) {
            QImage image(48, 32, QImage::Format_RGB32); image.fill(color);
            QByteArray bytes; QBuffer buffer(&bytes); buffer.open(QIODevice::WriteOnly); image.save(&buffer, "BMP"); return bytes;
        };
        const auto modified = QDateTime::fromSecsSinceEpoch(1700000000);
        const auto write = [&](const QColor &color, bool atomic) {
            const auto bytes = bmp(color);
            if (atomic) {
                QSaveFile file(path);
                if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) return false;
            } else {
                QFile file(path);
                if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size()) return false;
            }
            QFile file(path); return file.open(QIODevice::ReadOnly) && file.setFileTime(modified, QFileDevice::FileModificationTime);
        };
        QCOMPARE(bmp(Qt::red).size(), bmp(Qt::blue).size());
        QVERIFY(write(Qt::red, true));
        ik::Document document; document.openFiles({path}, false);
        QTRY_VERIFY_WITH_TIMEOUT(!document.isBusy() && !document.currentImage().isNull(), 5000);
        QVERIFY(document.isFileScoped());
        QVERIFY(write(Qt::blue, true));
        QCOMPARE(QFileInfo(path).lastModified(), modified);
        QTRY_VERIFY_WITH_TIMEOUT(!document.currentImage().isNull()
            && document.currentImage().pixelColor(0, 0) == QColor(Qt::blue), 5000);
        QVERIFY(write(Qt::green, false));
        QCOMPARE(QFileInfo(path).lastModified(), modified);
        QTRY_VERIFY_WITH_TIMEOUT(!document.currentImage().isNull()
            && document.currentImage().pixelColor(0, 0) == QColor(Qt::green), 5000);
    }
    void boundedMultiWatcherRearmsCurrentReplacement() {
        QTemporaryDir temp; QStringList paths;
        for (int i = 0; i < 150; ++i) {
            const QString path = temp.filePath(QString("%1.png").arg(i, 3, 10, QChar('0')));
            QVERIFY(saveImage(path, Qt::red)); paths.append(path);
        }
        ik::Document document; document.openFiles(paths, false);
        QTRY_VERIFY_WITH_TIMEOUT(!document.isBusy() && !document.currentImage().isNull(), 5000);
        auto *watcher = document.findChild<QFileSystemWatcher *>(); QVERIFY(watcher);
        QCOMPARE(watcher->files().size(), 128); QVERIFY(watcher->directories().isEmpty());
        QVERIFY(document.isMonitoringLimited());
        document.goTo(149);
        QTRY_VERIFY_WITH_TIMEOUT(!document.isBusy() && !document.currentImage().isNull(), 5000);
        QCOMPARE(watcher->files().size(), 128); QVERIFY(watcher->files().contains(paths.last()));
        for (const auto &color : {QColor(Qt::blue), QColor(Qt::green)}) {
            QSaveFile file(paths.last()); QVERIFY(file.open(QIODevice::WriteOnly));
            const auto bytes = png(color); QCOMPARE(file.write(bytes), bytes.size()); QVERIFY(file.commit());
            QTRY_VERIFY_WITH_TIMEOUT(!document.currentImage().isNull()
                && document.currentImage().pixelColor(0, 0) == color, 5000);
            QCOMPARE(document.currentEntry().path, paths.last());
            QVERIFY(watcher->files().contains(paths.last()));
            QVERIFY(watcher->directories().isEmpty());
        }
    }
    void cachedSelectionIsAtomicWithoutLoadingCycle() {
        QTemporaryDir temp;
        for (int i = 0; i < 10; ++i) QVERIFY(saveImage(temp.filePath(QString("%1.png").arg(i)), QColor::fromHsv(i * 30, 200, 200)));
        ik::Document document; document.open(temp.path());
        QTRY_VERIFY_WITH_TIMEOUT(!document.isBusy() && !document.cachedImage(1).isNull(), 5000);
        const auto cachedKey = document.cachedImage(1).cacheKey();
        QSignalSpy loading(&document, &ik::Document::loadingChanged);
        QStringList events;
        bool readyDuringSelection = false;
        QObject::connect(&document, &ik::Document::currentChanged, &document, [&](int) {
            events.append("current");
            readyDuringSelection = !document.isBusy() && !document.currentImage().isNull();
        });
        QObject::connect(&document, &ik::Document::imageReady, &document, [&](const QImage &, const ik::ImageInfo &) { events.append("image"); });
        document.goTo(1);
        QVERIFY(readyDuringSelection); QVERIFY(loading.isEmpty());
        QCOMPARE(events, QStringList({"current", "image"}));
        QCOMPARE(document.currentImage().cacheKey(), cachedKey);
        events.clear(); readyDuringSelection = true;
        document.goTo(9); // Neither the selected image nor its adjacent prefetch.
        QVERIFY(!readyDuringSelection); QVERIFY(document.isBusy());
        QCOMPARE(loading.size(), 1); QCOMPARE(loading.first().first().toBool(), true);
        QTRY_VERIFY_WITH_TIMEOUT(!document.isBusy() && !document.currentImage().isNull(), 5000);
        QCOMPARE(loading.size(), 2); QCOMPARE(loading.last().first().toBool(), false);
    }
    void refreshDoesNotReplacePendingOpen() {
        QTemporaryDir first, second;
        const QString a = first.filePath("a.png"), b = second.filePath("b.png");
        QVERIFY(saveImage(a, Qt::red)); QVERIFY(saveImage(b, Qt::blue));
        ik::Document document; document.open(a);
        QTRY_VERIFY_WITH_TIMEOUT(!document.isBusy() && !document.currentImage().isNull(), 5000);
        document.open(b);
        document.refreshCollection();
        document.refreshCollection();
        QTRY_VERIFY_WITH_TIMEOUT(!document.isBusy() && document.currentEntry().path == b, 5000);
        QCOMPARE(document.currentImage().pixelColor(0, 0), QColor(Qt::blue));
        QCOMPARE(document.collectionPaths(), QStringList({second.path()}));
    }
    void cacheStaysWithinBudget() {
        QTemporaryDir temp;
        for (int i = 0; i < 5; ++i)
            QVERIFY(saveImage(temp.filePath(QString::number(i) + ".png"), QColor::fromHsv(i * 30, 200, 200), {2000, 2000}));
        ik::Document document;
        document.setCacheLimitMB(32);
        QSignalSpy usage(&document, &ik::Document::cacheUsageChanged);
        document.open(temp.path());
        QTRY_VERIFY_WITH_TIMEOUT(!document.isBusy() && !document.currentImage().isNull(), 10000);
        for (int i = 1; i < 5; ++i) {
            document.goTo(i);
            QTRY_VERIFY_WITH_TIMEOUT(!document.isBusy(), 10000);
        }
        for (const auto &event : usage) QVERIFY(event[0].toLongLong() <= 32LL * 1024 * 1024);
        QCOMPARE(document.currentInfo().size, QSize(2000, 2000));
    }
};
QTEST_MAIN(DocumentTests)
#include "document_tests.moc"
