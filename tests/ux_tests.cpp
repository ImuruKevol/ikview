#include "document.h"
#include "imagecanvas.h"
#include "mainwindow.h"

#include <QAction>
#include <QAbstractButton>
#include <QApplication>
#include <QBuffer>
#include <QClipboard>
#include <QComboBox>
#include <QDragEnterEvent>
#include <QDragLeaveEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QDir>
#include <QDialog>
#include <QDataStream>
#include <QFile>
#include <QFileSystemWatcher>
#include <QIcon>
#include <QImageReader>
#include <QImageWriter>
#include <QImage>
#include <QItemSelectionModel>
#include <QLineEdit>
#include <QLabel>
#include <QListWidget>
#include <QListView>
#include <QMimeData>
#include <QPaintEvent>
#include <QPointer>
#include <QMessageBox>
#include <QPushButton>
#include <QQuickWindow>
#include <QSettings>
#include <QSpinBox>
#include <QScrollBar>
#include <QSGRendererInterface>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolBar>
#include <QTreeWidget>
#include <QUrl>
#include <QtTest>
#include <archive.h>
#include <archive_entry.h>
#include <memory>
#include <algorithm>
#include <vector>

namespace {
bool seedImages(const QString &directory, int count = 3)
{
    if (!QDir().mkpath(directory)) return false;
    for (int i = 0; i < count; ++i) {
        QImage image(120 + i, 80, QImage::Format_RGB32);
        image.fill(QColor::fromHsv(i * 43 % 360, 220, 230));
        if (!image.save(QDir(directory).filePath(QStringLiteral("page%1.png").arg(i + 1)))) return false;
    }
    return true;
}

QQuickWindow *surfaceFor(ik::MainWindow &window)
{
    auto *canvas = window.findChild<ik::ImageCanvas *>();
    if (!canvas) return nullptr;
    if (auto *child = canvas->findChild<QQuickWindow *>(QStringLiteral("imageSurface"))) return child;
    for (auto *candidate : QGuiApplication::allWindows()) {
        auto *quick = qobject_cast<QQuickWindow *>(candidate);
        if (quick && quick->isVisible() && quick->objectName() == QStringLiteral("imageSurface")) return quick;
    }
    return nullptr;
}

bool selectionMatches(QListView *list, ik::Document *document)
{
    if (!list || !document || document->currentIndex() < 0) return false;
    const auto selected = list->selectionModel()->selectedRows();
    return selected.size() == 1 && list->currentIndex() == selected.first()
        && selected.first().data(Qt::UserRole).toInt() == document->currentIndex()
        && selected.first().data(Qt::EditRole).toString() == document->currentEntry().name;
}

void confirmNextQuestion(QObject *context)
{
    auto *timer = new QTimer(context); timer->setInterval(20);
    QObject::connect(timer, &QTimer::timeout, context, [timer] {
        if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) {
            if (auto *yes = box->button(QMessageBox::Yes)) { yes->click(); timer->stop(); timer->deleteLater(); }
        }
    }); timer->start();
    QTimer::singleShot(6000, timer, [timer] { timer->stop(); timer->deleteLater(); if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) dialog->reject(); });
}

struct DropResult {
    bool enterAccepted = false;
    bool moveAccepted = false;
    bool dropAccepted = false;
    Qt::DropAction enterAction = Qt::IgnoreAction;
    Qt::DropAction moveAction = Qt::IgnoreAction;
    Qt::DropAction dropAction = Qt::IgnoreAction;
};

DropResult deliverDrop(QObject *target, const QList<QUrl> &urls, Qt::DropActions actions = Qt::CopyAction)
{
    QMimeData mime;
    mime.setUrls(urls);
    const QPoint point(20, 20);
    QDragEnterEvent enter(point, actions, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(target, &enter);
    QDragMoveEvent move(point, actions, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(target, &move);
    QDropEvent drop(QPointF(point), actions, &mime, Qt::LeftButton, Qt::NoModifier);
    QApplication::sendEvent(target, &drop);
    return {enter.isAccepted(), move.isAccepted(), drop.isAccepted(), enter.dropAction(), move.dropAction(), drop.dropAction()};
}

QByteArray png(const QColor &color)
{
    QImage image(80, 60, QImage::Format_RGB32);
    image.fill(color);
    QByteArray encoded;
    QBuffer buffer(&encoded);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return encoded;
}

bool writeComic(const QString &path)
{
    archive *writer = archive_write_new();
    archive_write_set_format_zip(writer);
    if (archive_write_open_filename(writer, QFile::encodeName(path).constData()) != ARCHIVE_OK) {
        archive_write_free(writer);
        return false;
    }
    bool success = true;
    for (int i = 0; i < 2; ++i) {
        const QByteArray bytes = png(i == 0 ? Qt::red : Qt::green);
        const QByteArray name = i == 0 ? "page1.png" : "page2.png";
        archive_entry *entry = archive_entry_new();
        archive_entry_set_pathname_utf8(entry, name.constData());
        archive_entry_set_size(entry, bytes.size());
        archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_perm(entry, 0644);
        success &= archive_write_header(writer, entry) == ARCHIVE_OK;
        success &= archive_write_data(writer, bytes.constData(), bytes.size()) == bytes.size();
        archive_entry_free(entry);
    }
    archive_write_close(writer);
    archive_write_free(writer);
    return success;
}

bool writeTwoPageTiff(const QString &path)
{
    QByteArray bytes; QDataStream data(&bytes, QIODevice::WriteOnly); data.setByteOrder(QDataStream::LittleEndian);
    data.writeRawData("II", 2); data << quint16(42) << quint32(8);
    for (int page = 0; page < 2; ++page) {
        data << quint16(10);
        const auto tag = [&](quint16 id, quint16 type, quint32 count, quint32 value) { data << id << type << count << value; };
        tag(256, 4, 1, 2); tag(257, 4, 1, 2); tag(258, 3, 3, 260 + page * 6); tag(259, 3, 1, 1); tag(262, 3, 1, 2);
        tag(273, 4, 1, 272 + page * 12); tag(277, 3, 1, 3); tag(278, 4, 1, 2); tag(279, 4, 1, 12); tag(284, 3, 1, 1);
        data << quint32(page == 0 ? 134 : 0);
    }
    for (int i = 0; i < 6; ++i) data << quint16(8);
    for (int page = 0; page < 2; ++page) for (int i = 0; i < 4; ++i) data << quint8(page == 0 ? 255 : 0) << quint8(0) << quint8(page == 1 ? 255 : 0);
    QFile file(path); return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

QColor thumbnailCenter(QListView *list, int row)
{
    const auto decoration = list->model()->index(row, 0).data(Qt::DecorationRole);
    const QImage image = qvariant_cast<QIcon>(decoration).pixmap(72, 54).toImage();
    return image.isNull() ? QColor() : image.pixelColor(image.width() / 2, image.height() / 2);
}

class ViewportPaintObserver final : public QObject {
public:
    QRegion painted;
    bool eventFilter(QObject *, QEvent *event) override
    {
        if (event->type() == QEvent::Paint) painted += static_cast<QPaintEvent *>(event)->region();
        return false;
    }
};
}

class UxTests : public QObject {
    Q_OBJECT
    std::unique_ptr<QTemporaryDir> m_temp;
private slots:
    void init()
    {
        m_temp = std::make_unique<QTemporaryDir>();
        QVERIFY(m_temp->isValid());
        qputenv("IKVIEW_SETTINGS_PATH", m_temp->filePath(QStringLiteral("test-settings.ini")).toUtf8());
    }

    void cleanup()
    {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        qunsetenv("IKVIEW_SETTINGS_PATH");
        m_temp.reset();
    }

    void listTabPreservesChromeAndMetadataCopyUsesText()
    {
        const QString directory = m_temp->filePath("photos"); QVERIFY(seedImages(directory));
        ik::MainWindow window; window.show(); window.openPaths({directory});
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *document = window.findChild<ik::Document *>(); auto *list = window.findChild<QListView *>("fileList");
        auto *clean = window.findChild<QAction *>("cleanViewAction"); auto *info = window.findChild<QAction *>("toggleMetadataAction"); auto *metadata = window.findChild<QTreeWidget *>("imageMetadata");
        QVERIFY(document && list && clean && info && metadata);
        QTRY_VERIFY_WITH_TIMEOUT(!document->isBusy() && !document->currentImage().isNull(), 5000);
        window.activateWindow(); QApplication::setActiveWindow(&window); list->setFocus(); QTRY_VERIFY_WITH_TIMEOUT(list->hasFocus(), 3000);
        QTest::keyClick(list, Qt::Key_Tab); QVERIFY(!clean->isChecked());
        if (!info->isChecked()) info->trigger();
        metadata->setCurrentItem(metadata->topLevelItem(0)); metadata->topLevelItem(0)->setSelected(true); metadata->setFocus();
        QApplication::clipboard()->setText("before");
        QTest::keyClick(metadata, Qt::Key_C, Qt::ControlModifier);
        QCOMPARE(QApplication::clipboard()->text(), document->currentEntry().name);
        window.findChild<QAction *>("copyMetadataRowAction")->trigger();
        QCOMPARE(QApplication::clipboard()->text(), "파일: " + document->currentEntry().name);
        window.findChild<QAction *>("copyMetadataAllAction")->trigger(); QVERIFY(QApplication::clipboard()->text().contains("경로: "));
    }

    void emptyFilterAndTextListHaveUsefulStates()
    {
        const QString directory = m_temp->filePath("photos"); QVERIFY(seedImages(directory));
        ik::MainWindow window; window.show(); window.openPaths({directory});
        auto *document = window.findChild<ik::Document *>(); auto *list = window.findChild<QListView *>("fileList");
        auto *filter = window.findChild<QLineEdit *>("fileFilter"); auto *empty = window.findChild<QLabel *>("emptyFileListLabel"); auto *thumbnails = window.findChild<QAction *>("toggleThumbnailsAction");
        QVERIFY(document && list && filter && empty && thumbnails);
        QTRY_VERIFY_WITH_TIMEOUT(!document->isBusy() && !document->currentImage().isNull(), 5000);
        const int large = list->model()->index(0, 0).data(Qt::SizeHintRole).toSize().height();
        thumbnails->trigger(); QVERIFY(!thumbnails->isChecked());
        QVERIFY(list->model()->index(0, 0).data(Qt::DecorationRole).isNull());
        QVERIFY(list->model()->index(0, 0).data(Qt::SizeHintRole).toSize().height() < large);
        filter->setText("zz-no-result-zz"); QCOMPARE(list->model()->rowCount(), 0); QVERIFY(!empty->isHidden()); QVERIFY(empty->text().contains("검색")); QVERIFY(list->isHidden());
        filter->clear(); QVERIFY(empty->isHidden()); QVERIFY(!list->isHidden()); QCOMPARE(list->model()->rowCount(), 3);
    }

    void zoomKeepsUnchangedNativeCommandStates()
    {
        const QString directory = m_temp->filePath("photos"); QVERIFY(seedImages(directory));
        ik::MainWindow window; window.show(); window.openPaths({directory}); auto *document = window.findChild<ik::Document *>(); auto *canvas = window.findChild<ik::ImageCanvas *>(); QVERIFY(document && canvas);
        QTRY_VERIFY_WITH_TIMEOUT(!document->isBusy() && !document->currentImage().isNull(), 5000); canvas->setZoom(.25);
        std::vector<std::unique_ptr<QSignalSpy>> changes;
        for (const auto &name : {"panImage0Action", "panImage1Action", "panImage2Action", "panImage3Action", "openGpsMapAction", "pauseAnimationAction", "nextDocumentPageAction", "previousDocumentPageAction"}) {
            auto *command = window.findChild<QAction *>(name); QVERIFY(command); QVERIFY(!command->isEnabled()); changes.push_back(std::make_unique<QSignalSpy>(command, &QAction::changed));
        }
        for (int i = 0; i < 20; ++i) canvas->setZoom(i % 2 ? .25 : .3);
        for (const auto &change : changes) QCOMPARE(change->size(), 0);
    }

    void gpsCommandTracksMetadataReload()
    {
        const QString path = m_temp->filePath("position.png");
        auto write = [&](bool gps) { QImage image(120, 80, QImage::Format_RGB32); image.fill(Qt::red); QImageWriter writer(path, "png"); if (gps) { writer.setText("GPS.Latitude", "37.5"); writer.setText("GPS.Longitude", "-122.25"); } return writer.write(image); };
        QVERIFY(write(true)); ik::MainWindow window; window.show(); window.openPaths({path}); auto *document = window.findChild<ik::Document *>(); auto *map = window.findChild<QAction *>("openGpsMapAction"); QVERIFY(document && map);
        QTRY_VERIFY_WITH_TIMEOUT(!document->isBusy() && !document->currentImage().isNull() && map->isEnabled(), 5000);
        QVERIFY(write(false)); document->refreshCollection();
        QTRY_VERIFY_WITH_TIMEOUT(!document->isBusy() && !document->currentImage().isNull() && !map->isEnabled(), 5000);
        QVERIFY(write(true)); document->refreshCollection();
        QTRY_VERIFY_WITH_TIMEOUT(!document->isBusy() && !document->currentImage().isNull() && map->isEnabled(), 5000);
    }

    void cachedNavigationKeepsCommandsAndRecentMenuStable()
    {
        const QString directory = m_temp->filePath("photos"); QVERIFY(seedImages(directory));
        ik::MainWindow window; window.show(); window.openPaths({directory}); auto *document = window.findChild<ik::Document *>(); auto *canvas = window.findChild<ik::ImageCanvas *>(); QVERIFY(document && canvas);
        QTRY_VERIFY_WITH_TIMEOUT(!document->isBusy() && !document->currentImage().isNull() && !document->cachedImage(1).isNull(), 5000);
        auto *exportCommand = window.findChild<QAction *>("exportImageAction"); auto *copyCommand = window.findChild<QAction *>("copyPhotoAction"); auto *next = window.findChild<QAction *>("nextImageAction"); QVERIFY(exportCommand && copyCommand && next);
        QPointer<QAction> recent = window.findChild<QAction *>("recentSessionAction"); QVERIFY(recent); QVERIFY(exportCommand->isEnabled() && copyCommand->isEnabled());
        canvas->rotate(1); canvas->flipHorizontal(); canvas->setGamma(1.4); canvas->setInverted(true);
        QSignalSpy exportChanges(exportCommand, &QAction::changed), copyChanges(copyCommand, &QAction::changed), loading(document, &ik::Document::loadingChanged);
        const auto syncs = canvas->activityStats().sceneSyncCalls; next->trigger();
        QCOMPARE(document->currentIndex(), 1); QVERIFY(!document->isBusy()); QVERIFY(!document->currentImage().isNull());
        QCOMPARE(loading.size(), 0); QCOMPARE(exportChanges.size(), 0); QCOMPARE(copyChanges.size(), 0);
        QVERIFY(recent); QCOMPARE(window.findChild<QAction *>("recentSessionAction"), recent.data());
        QVERIFY(canvas->activityStats().sceneSyncCalls - syncs <= 2);
        const auto transform = canvas->transformState(); QCOMPARE(transform.quarterTurns, 0); QVERIFY(!transform.mirrorX && !transform.mirrorY && !transform.inverted); QCOMPARE(transform.gamma, 1.);
        QCOMPARE(window.property("documentState").toString(), "ready");
    }

    void widgetDragMovesReuseValidationAndDropRevalidates()
    {
        const QString path = m_temp->filePath("drag.png"); QImage image(10, 10, QImage::Format_RGB32); image.fill(Qt::blue); QVERIFY(image.save(path));
        ik::MainWindow window; window.show(); auto *document = window.findChild<ik::Document *>(); QVERIFY(document);
        QMimeData mime; mime.setUrls({QUrl::fromLocalFile(path)}); QDragEnterEvent enter(QPointF(20, 20), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier); QApplication::sendEvent(&window, &enter); QVERIFY(enter.isAccepted());
        QVERIFY(QFile::remove(path));
        for (int i = 0; i < 3; ++i) { QDragMoveEvent move(QPointF(20 + i, 20), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier); QApplication::sendEvent(&window, &move); QVERIFY2(move.isAccepted(), "DragMove must use the validated drag entry instead of probing the file again"); }
        QDropEvent drop(QPointF(20, 20), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier); QApplication::sendEvent(&window, &drop); QVERIFY(!drop.isAccepted()); QCOMPARE(document->count(), 0); QVERIFY(!document->isBusy());
        QDragEnterEvent again(QPointF(20, 20), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier); QApplication::sendEvent(&window, &again); QVERIFY(!again.isAccepted());
    }

    void refreshingSameEntryPreservesFilterAndBrowsingScroll()
    {
        const QString directory = m_temp->filePath("photos"); QVERIFY(seedImages(directory, 40));
        ik::MainWindow window; window.show(); window.openPaths({directory}); auto *document = window.findChild<ik::Document *>(); auto *list = window.findChild<QListView *>("fileList"); auto *filter = window.findChild<QLineEdit *>("fileFilter"); QVERIFY(document && list && filter);
        QTRY_VERIFY_WITH_TIMEOUT(!document->isBusy() && !document->currentImage().isNull(), 5000); filter->setText("page");
        QTRY_VERIFY_WITH_TIMEOUT(list->verticalScrollBar()->maximum() > 500, 3000); list->verticalScrollBar()->setValue(500); const auto selected = document->currentEntry().key();
        document->refreshCollection(); QTRY_VERIFY_WITH_TIMEOUT(!document->isBusy() && !document->currentImage().isNull(), 5000);
        QCOMPARE(filter->text(), "page"); QCOMPARE(document->currentEntry().key(), selected); QTRY_COMPARE_WITH_TIMEOUT(list->verticalScrollBar()->value(), 500, 3000);
    }

    void preferencesCancelPreservesArchiveHistory()
    {
        const QString key = "archivePositions/example";
        QSettings settings(qEnvironmentVariable("IKVIEW_SETTINGS_PATH"), QSettings::IniFormat); settings.setValue(key, 7);
        ik::MainWindow window; window.show(); auto *preferences = window.findChild<QAction *>("preferencesAction"); QVERIFY(preferences);
        QCOMPARE(preferences->menuRole(), QAction::PreferencesRole); QCOMPARE(preferences->shortcut(), QKeySequence("Ctrl+,"));
        QCOMPARE(window.findChild<QAction *>("refreshCollectionAction")->shortcut(), QKeySequence("F5"));
        bool handled = false;
        QTimer::singleShot(0, &window, [&] { auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()); if (!dialog) return; auto *clear = dialog->findChild<QPushButton *>("clearArchivePositionsButton"); if (clear) { clear->click(); handled = true; } dialog->reject(); });
        preferences->trigger(); QVERIFY(handled); QCOMPARE(settings.value(key).toInt(), 7);
        QTimer::singleShot(0, &window, [&] { auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()); if (!dialog) return; if (auto *clear = dialog->findChild<QPushButton *>("clearArchivePositionsButton")) clear->click(); dialog->accept(); });
        preferences->trigger(); QVERIFY(!settings.contains(key));
    }

    void recentSessionsRestoreExplicitCollectionAndActionsStayBounded()
    {
        const QString first = m_temp->filePath("one"), second = m_temp->filePath("two"); QVERIFY(seedImages(first)); QVERIFY(seedImages(second));
        const QStringList paths {QDir(first).filePath("page1.png"), QDir(second).filePath("page3.png")};
        ik::MainWindow window; window.show(); window.openPaths(paths); auto *document = window.findChild<ik::Document *>(); QVERIFY(document);
        QTRY_VERIFY_WITH_TIMEOUT(!document->isBusy() && !document->currentImage().isNull(), 5000); QCOMPARE(document->count(), 2);
        for (int i = 0; i < 10; ++i) { document->goTo(i % 2); QTRY_VERIFY_WITH_TIMEOUT(!document->isBusy() && !document->currentImage().isNull(), 5000); }
        QCOMPARE(window.findChildren<QAction *>("recentSessionAction").size(), 1);
        QSettings settings(qEnvironmentVariable("IKVIEW_SETTINGS_PATH"), QSettings::IniFormat);
        const auto records = settings.value("recentSessions").toList(); QCOMPARE(records.size(), 1); QCOMPARE(records.first().toMap().value("paths").toStringList(), paths);
        document->close(); QCOMPARE(document->count(), 0);
        window.findChild<QAction *>("recentSessionAction")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!document->isBusy() && document->count() == 2 && !document->currentImage().isNull(), 5000);
        QCOMPARE(document->currentEntry().path, paths.last());
    }

    void archiveBookmarksFollowMemberAfterSortAndExplicitSelectionWins()
    {
        const QString path = m_temp->filePath("comic.cbz"); QVERIFY(writeComic(path));
        ik::MainWindow window; window.show(); window.openPaths({path}); auto *document = window.findChild<ik::Document *>(); QVERIFY(document);
        QTRY_VERIFY_WITH_TIMEOUT(!document->isBusy() && !document->currentImage().isNull(), 5000);
        document->goTo(1); QTRY_VERIFY_WITH_TIMEOUT(!document->isBusy(), 5000);
        window.findChild<QAction *>("addBookmarkAction")->trigger();
        window.findChild<QAction *>("sortAction1")->trigger(); QCOMPARE(document->currentEntry().member, "page2.png");
        document->close(); window.findChild<QAction *>("latestBookmarkAction")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!document->isBusy() && document->currentEntry().member == "page2.png" && !document->currentImage().isNull(), 5000); QCOMPARE(document->currentIndex(), 0);
        window.openPathsAt({path}, 1, 0);
        QTRY_VERIFY_WITH_TIMEOUT(!document->isBusy() && document->currentEntry().member == "page1.png" && !document->currentImage().isNull(), 5000); QCOMPARE(document->currentIndex(), 1);
    }

    void legacyRecentArchiveAndNumericPositionMigrate()
    {
        const QString path = m_temp->filePath("comic.cbz"); QVERIFY(writeComic(path));
        const QString key = "archivePositions/" + QString::fromLatin1(path.toUtf8().toBase64(QByteArray::Base64UrlEncoding));
        QSettings settings(qEnvironmentVariable("IKVIEW_SETTINGS_PATH"), QSettings::IniFormat); settings.setValue("recent", QStringList{path}); settings.setValue(key, 1);
        ik::MainWindow window; window.show(); auto *document = window.findChild<ik::Document *>(); auto *recent = window.findChild<QAction *>("recentSessionAction"); QVERIFY(document && recent); recent->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!document->isBusy() && document->currentEntry().member == "page2.png" && !document->currentImage().isNull(), 5000);
        QCOMPARE(settings.value(key).toMap().value("member").toString(), "page2.png"); QVERIFY(!settings.contains("recent")); QCOMPARE(settings.value("recentSessions").toList().size(), 1);
    }

    void bookmarksEmptyAndDeletionKeepValidSelection()
    {
        ik::MainWindow window; window.show(); auto *manage = window.findChild<QAction *>("manageBookmarksAction"); QVERIFY(manage); bool observed = false;
        QTimer::singleShot(0, &window, [&] { auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()); if (!dialog) return; auto *open = dialog->findChild<QPushButton *>("openBookmarkButton"); auto *remove = dialog->findChild<QPushButton *>("removeBookmarkButton"); observed = open && remove && !open->isEnabled() && !remove->isEnabled(); dialog->reject(); });
        manage->trigger(); QVERIFY(observed);
        QSettings settings(qEnvironmentVariable("IKVIEW_SETTINGS_PATH"), QSettings::IniFormat);
        settings.setValue("bookmarks", QVariantList{QVariantMap{{"path", "/missing/a.png"}, {"name", "A"}}, QVariantMap{{"path", "/missing/b.png"}, {"name", "B"}}});
        observed = false;
        QTimer::singleShot(0, &window, [&] { auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()); if (!dialog) return; auto *list = dialog->findChild<QListWidget *>("bookmarksList"); auto *remove = dialog->findChild<QPushButton *>("removeBookmarkButton"); if (list && remove) { list->setCurrentRow(1); remove->click(); observed = list->currentRow() == 0 && list->count() == 1 && remove->isEnabled(); } dialog->reject(); });
        manage->trigger(); QVERIFY(observed);
    }

    void tiffPageBookmarksAndFailedRestoreAreIsolated()
    {
        const QString path = m_temp->filePath("pages.tiff"); QVERIFY(writeTwoPageTiff(path));
        ik::MainWindow window; window.show(); window.openPathsAt({path}, 0, 0); auto *document = window.findChild<ik::Document *>(); QVERIFY(document);
        QTRY_VERIFY_WITH_TIMEOUT(!document->isBusy() && document->currentInfo().pageCount == 2, 5000);
        window.findChild<QAction *>("addBookmarkAction")->trigger(); document->setPage(1);
        QTRY_VERIFY_WITH_TIMEOUT(!document->isBusy() && document->currentInfo().page == 1, 5000); window.findChild<QAction *>("addBookmarkAction")->trigger();
        QSettings settings(qEnvironmentVariable("IKVIEW_SETTINGS_PATH"), QSettings::IniFormat); auto bookmarks = settings.value("bookmarks").toList(); QCOMPARE(bookmarks.size(), 2);
        bookmarks.append(QVariantMap{{"path", m_temp->filePath("missing.tiff")}, {"name", "missing"}, {"page", 1}}); settings.setValue("bookmarks", bookmarks);
        QSignalSpy errors(document, &ik::Document::errorOccurred); window.findChild<QAction *>("latestBookmarkAction")->trigger(); QTRY_VERIFY_WITH_TIMEOUT(!errors.isEmpty(), 5000);
        document->setPage(0); QTRY_VERIFY_WITH_TIMEOUT(!document->isBusy() && document->currentInfo().page == 0, 5000);
        QTest::qWait(60); QCOMPARE(document->currentInfo().page, 0);
        QVERIFY(window.property("documentState").toString() != "error");
        window.openPathsAt({path}, 0, 1); QTRY_VERIFY_WITH_TIMEOUT(!document->isBusy() && document->currentInfo().page == 1, 5000);
    }

    void coldExplicitTiffPageSurvivesSiblingSettingsWritesAndRefresh()
    {
        { QSettings preferences(qEnvironmentVariable("IKVIEW_SETTINGS_PATH"), QSettings::IniFormat); preferences.setValue("openContainingFolder", true); }
        // The settings file deliberately shares the image's watched directory,
        // as in the packaged CLI smoke. Its write must not reset the TIFF page.
        const QString path = m_temp->filePath("pages.tiff"); QVERIFY(writeTwoPageTiff(path));
        ik::MainWindow window; window.show(); auto *document = window.findChild<ik::Document *>(); QVERIFY(document);
        auto *watcher = document->findChild<QFileSystemWatcher *>(); QVERIFY(watcher); QSignalSpy directoryChanges(watcher, &QFileSystemWatcher::directoryChanged);
        QSignalSpy entries(document, &ik::Document::entriesChanged); window.openPathsAt({path}, 0, 1);
        QTRY_VERIFY_WITH_TIMEOUT(!document->isBusy() && document->currentInfo().page == 1 && !document->currentImage().isNull(), 5000);
        QCOMPARE(document->currentImage().pixelColor(0, 0), QColor(Qt::blue));
        QTest::qWait(1200);
        QVERIFY2(!directoryChanges.isEmpty(), "The sibling settings write must actually notify the watched directory");
        QCOMPARE(entries.size(), 1); // Unchanged images keep the model and decoded page.
        QVERIFY(!document->isBusy()); QCOMPARE(document->currentInfo().page, 1); QCOMPARE(document->currentImage().pixelColor(0, 0), QColor(Qt::blue));
        document->refreshCollection(); QTRY_VERIFY_WITH_TIMEOUT(!document->isBusy() && document->currentInfo().page == 1 && !document->currentImage().isNull(), 5000);
        QCOMPARE(document->currentImage().pixelColor(0, 0), QColor(Qt::blue));
        window.openPaths({path}); QTRY_VERIFY_WITH_TIMEOUT(!document->isBusy() && document->currentInfo().page == 0, 5000);
    }

    void slideshowLoopsAndStopsAfterManualNavigation()
    {
        const QString directory = m_temp->filePath("photos"); QVERIFY(seedImages(directory, 2));
        ik::MainWindow window; window.show(); window.openPaths({directory}); auto *document = window.findChild<ik::Document *>(); QVERIFY(document);
        QTRY_VERIFY_WITH_TIMEOUT(!document->isBusy() && !document->currentImage().isNull(), 5000);
        window.findChild<QAction *>("slideshowInterval1Action")->trigger(); window.findChild<QAction *>("slideshowLoopAction")->trigger();
        document->goTo(1); QTRY_VERIFY_WITH_TIMEOUT(!document->isBusy(), 5000); auto *slideshow = window.findChild<QAction *>("slideshowAction"); QVERIFY(slideshow); window.findChild<QAction *>("slideshowInterval1Action")->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(document->currentIndex() == 0 && !document->isBusy(), 2200); QVERIFY(slideshow->isChecked());
        window.findChild<QAction *>("nextImageAction")->trigger(); QVERIFY(!slideshow->isChecked());
        QTRY_VERIFY_WITH_TIMEOUT(!document->isBusy(), 5000); QCOMPARE(document->currentIndex(), 1);
    }

    void lockedZoomSurvivesReopenAndExplicitFitUnlocks()
    {
        const QString directory = m_temp->filePath("photos"); QVERIFY(seedImages(directory));
        {
            ik::MainWindow window; window.show(); window.openPaths({directory}); auto *document = window.findChild<ik::Document *>(); QVERIFY(document);
            QTRY_VERIFY_WITH_TIMEOUT(!document->isBusy() && !document->currentImage().isNull(), 5000); auto *canvas = window.findChild<ik::ImageCanvas *>(); QVERIFY(canvas);
            canvas->setZoom(2.5); window.findChild<QAction *>("zoomLockAction")->trigger(); QVERIFY(canvas->zoomLocked());
        }
        ik::MainWindow window; window.show(); window.openPaths({directory}); auto *document = window.findChild<ik::Document *>(); QVERIFY(document);
        QTRY_VERIFY_WITH_TIMEOUT(!document->isBusy() && !document->currentImage().isNull(), 5000); auto *canvas = window.findChild<ik::ImageCanvas *>(); QVERIFY(canvas);
        QVERIFY(canvas->zoomLocked()); QVERIFY(qAbs(canvas->zoom() - 2.5) < .001);
        window.findChild<QAction *>("fitImageAction")->trigger(); QVERIFY(!canvas->zoomLocked()); QVERIFY(!window.findChild<QAction *>("zoomLockAction")->isChecked()); QCOMPARE(canvas->fitMode(), ik::FitMode::Fit);
    }

    void deleteScopedFileNeverExpandsSiblingAccess()
    {
        const QString directory = m_temp->filePath("photos"); QVERIFY(seedImages(directory)); const QString chosen = QDir(directory).filePath("page2.png");
        ik::MainWindow window; window.show(); auto *document = window.findChild<ik::Document *>(); QVERIFY(document); document->openFiles({chosen}, false);
        QTRY_VERIFY_WITH_TIMEOUT(!document->isBusy() && !document->currentImage().isNull(), 5000); QCOMPARE(document->count(), 1); QVERIFY(document->isFileScoped());
        auto *hint = window.findChild<QWidget *>("sourceScopeHint"); QVERIFY(hint); QVERIFY(!hint->isHidden());
        QSignalSpy finished(&window, &ik::MainWindow::fileOperationFinished); confirmNextQuestion(&window);
        window.findChild<QAction *>("deleteFileAction")->trigger(); QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000); QVERIFY(finished.last()[0].toBool());
        QCOMPARE(document->count(), 0); QVERIFY(!QFileInfo::exists(chosen)); QVERIFY(QFileInfo::exists(QDir(directory).filePath("page1.png"))); QVERIFY(QFileInfo::exists(QDir(directory).filePath("page3.png")));
        QVERIFY(document->currentImage().isNull());
    }

    void expandingScopedFolderKeepsCurrentImageAndTiffPage()
    {
        const QString directory = m_temp->filePath("photos"); QVERIFY(seedImages(directory)); const QString chosen = QDir(directory).filePath("pages.tiff"); QVERIFY(writeTwoPageTiff(chosen));
        ik::MainWindow window; window.show(); auto *document = window.findChild<ik::Document *>(); QVERIFY(document); document->openFiles({chosen}, false);
        QTRY_VERIFY_WITH_TIMEOUT(!document->isBusy() && !document->currentImage().isNull(), 5000); document->setPage(1);
        QTRY_VERIFY_WITH_TIMEOUT(!document->isBusy() && document->currentInfo().page == 1, 5000); QCOMPARE(document->count(), 1);
        auto *expand = window.findChild<QPushButton *>("openSourceFolderButton"); QVERIFY(expand && expand->isEnabled());
        // Use the folder-opening API after native picker authorization. The
        // picker itself is covered by manual macOS QA; test the resulting view.
        window.openFolderKeepingCurrent(directory);
        QTRY_VERIFY_WITH_TIMEOUT(!document->isBusy() && document->count() == 4 && document->currentEntry().path == chosen && document->currentInfo().page == 1 && !document->currentImage().isNull(), 5000);
        QCOMPARE(document->currentImage().pixelColor(0, 0), QColor(Qt::blue)); QVERIFY(!document->isFileScoped());
    }

    void fileMoveKeepsExplicitCollectionAndCloseDoesNotResurrect()
    {
        const QString one = m_temp->filePath("one"), two = m_temp->filePath("two"), library = m_temp->filePath("library"); QVERIFY(seedImages(one)); QVERIFY(seedImages(two)); QVERIFY(QDir().mkpath(library));
        const QString a = QDir(one).filePath("page1.png"), b = QDir(two).filePath("page3.png");
        QSettings settings(qEnvironmentVariable("IKVIEW_SETTINGS_PATH"), QSettings::IniFormat); settings.setValue("photoFolder", library);
        ik::MainWindow window; window.show(); window.openPaths({a, b}); auto *document = window.findChild<ik::Document *>(); QVERIFY(document);
        QTRY_VERIFY_WITH_TIMEOUT(!document->isBusy() && !document->currentImage().isNull(), 5000);
        QSignalSpy finished(&window, &ik::MainWindow::fileOperationFinished);
        confirmNextQuestion(&window);
        window.findChild<QAction *>("movePhotoAction")->trigger();
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000); QVERIFY(finished.last()[0].toBool());
        QTRY_VERIFY_WITH_TIMEOUT(!document->isBusy() && !document->currentImage().isNull(), 5000);
        QCOMPARE(document->count(), 1); QCOMPARE(document->currentEntry().path, b); QVERIFY(!QFileInfo::exists(a)); QVERIFY(QFileInfo::exists(QDir(library).filePath("page1.png")));
        window.findChild<QAction *>("copyPhotoAction")->trigger(); QVERIFY(window.property("fileOperationPending").toBool());
        window.findChild<QAction *>("quitApplicationAction")->trigger();
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 2, 5000);
        QVERIFY(finished.last()[0].toBool() || finished.last()[1].toBool());
        QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 3000);
        QVERIFY(QFileInfo::exists(b)); QCOMPARE(document->count(), 1);
    }

    void cancelledFileCopyPreservesSourceAndExistingDestination_data()
    {
        QTest::addColumn<bool>("quit"); QTest::addColumn<bool>("shortcut");
        QTest::newRow("cancel-button") << false << false;
        QTest::newRow("standard-quit") << true << false;
        QTest::newRow("command-w") << true << true;
    }
    void cancelledFileCopyPreservesSourceAndExistingDestination()
    {
        QFETCH(bool, quit); QFETCH(bool, shortcut);
        const QString source = m_temp->filePath("large.png"), library = m_temp->filePath("library"); QVERIFY(QDir().mkpath(library));
        QFile input(source); QVERIFY(input.open(QIODevice::WriteOnly)); const auto encoded = png(Qt::red); QCOMPARE(input.write(encoded), encoded.size());
        const QByteArray block(1024 * 1024, '\0'); for (int i = 0; i < 64; ++i) QCOMPARE(input.write(block), block.size()); input.close();
        const QString destination = QDir(library).filePath("large.png"); QFile output(destination); QVERIFY(output.open(QIODevice::WriteOnly)); output.write("original destination"); output.close();
        QSettings settings(qEnvironmentVariable("IKVIEW_SETTINGS_PATH"), QSettings::IniFormat); settings.setValue("photoFolder", library);
        ik::MainWindow window; window.show(); window.openPaths({source}); auto *document = window.findChild<ik::Document *>(); QVERIFY(document);
        QTRY_VERIFY_WITH_TIMEOUT(!document->isBusy() && !document->currentImage().isNull(), 5000); QSignalSpy finished(&window, &ik::MainWindow::fileOperationFinished);
        confirmNextQuestion(&window);
        window.findChild<QAction *>("copyPhotoAction")->trigger();
        if (quit) { auto *quitAction = window.findChild<QAction *>("quitApplicationAction"); QVERIFY(quitAction); QCOMPARE(quitAction->menuRole(), QAction::QuitRole); if (shortcut) QTest::keyClick(&window, Qt::Key_W, Qt::ControlModifier); else quitAction->trigger(); }
        else window.findChild<QPushButton *>("cancelFileOperationButton")->click();
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000); QVERIFY(finished.last()[1].toBool()); QVERIFY(!finished.last()[0].toBool()); QVERIFY(QFileInfo::exists(source));
        QVERIFY(output.open(QIODevice::ReadOnly)); QCOMPARE(output.readAll(), QByteArray("original destination"));
        QVERIFY(!window.property("fileOperationPending").toBool());
        if (quit) QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 3000);
    }

    void movingFileChangedDuringCopyPreservesBothPaths()
    {
        const QString source = m_temp->filePath("large.png"), replacement = m_temp->filePath("replacement.data"), backup = m_temp->filePath("previous.data"), library = m_temp->filePath("library"); QVERIFY(QDir().mkpath(library));
        QFile input(source); QVERIFY(input.open(QIODevice::WriteOnly)); const auto encoded = png(Qt::red); QCOMPARE(input.write(encoded), encoded.size());
        const QByteArray block(1024 * 1024, '\0'); for (int i = 0; i < 240; ++i) QCOMPARE(input.write(block), block.size()); input.close();
        QFile newer(replacement); QVERIFY(newer.open(QIODevice::WriteOnly)); const auto newBytes = png(Qt::blue); QCOMPARE(newer.write(newBytes), newBytes.size()); newer.close();
        const QString target = QDir(library).filePath("large.png"); QFile previousTarget(target); QVERIFY(previousTarget.open(QIODevice::WriteOnly)); previousTarget.write("existing target"); previousTarget.close();
        QSettings settings(qEnvironmentVariable("IKVIEW_SETTINGS_PATH"), QSettings::IniFormat); settings.setValue("photoFolder", library);
        ik::MainWindow window; window.show(); window.openPaths({source}); auto *document = window.findChild<ik::Document *>(); QVERIFY(document);
        QTRY_VERIFY_WITH_TIMEOUT(!document->isBusy() && !document->currentImage().isNull(), 5000);
        QSignalSpy finished(&window, &ik::MainWindow::fileOperationFinished);
        // A move with an existing target asks twice. Keep the answer timer alive
        // through both confirmations, then stop before touching any real IO.
        QTimer answers; answers.setInterval(5); connect(&answers, &QTimer::timeout, &window, [] { if (auto *box = qobject_cast<QMessageBox *>(QApplication::activeModalWidget())) if (auto *yes = box->button(QMessageBox::Yes)) yes->click(); }); answers.start();
        window.findChild<QAction *>("movePhotoAction")->trigger(); answers.stop(); QVERIFY(window.property("fileOperationPending").toBool());
        // The named QSaveFile appears only after the worker has opened and
        // captured the original inode. Avoid event processing until replacement.
        QElapsedTimer elapsed; elapsed.start(); bool temporarySeen = false;
        while (elapsed.elapsed() < 2000) {
            if (QDir(library).entryList(QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot).size() > 1) { temporarySeen = true; break; }
        }
        QVERIFY2(temporarySeen, "A temporary output must exist while this generated 240 MiB source is being copied");
        QVERIFY(QFile::rename(source, backup)); QVERIFY(QFile::rename(replacement, source));
        QTRY_COMPARE_WITH_TIMEOUT(finished.size(), 1, 5000); QVERIFY(!finished.last()[0].toBool()); QVERIFY(finished.last()[2].toString().contains("원본 파일이 변경"));
        QFile current(source); QVERIFY(current.open(QIODevice::ReadOnly)); QCOMPARE(current.readAll(), newBytes);
        QVERIFY(previousTarget.open(QIODevice::ReadOnly)); QCOMPARE(previousTarget.readAll(), QByteArray("existing target")); QVERIFY(QFileInfo::exists(backup));
    }

    void exportRetryKeepsSettingsAndMatchesCodecExtension()
    {
        const QString directory = m_temp->filePath("photos"); QVERIFY(seedImages(directory)); const QString outputPath = m_temp->filePath("converted.png");
        ik::MainWindow window; window.show(); window.openPaths({directory}); auto *document = window.findChild<ik::Document *>(); QVERIFY(document);
        QTRY_VERIFY_WITH_TIMEOUT(!document->isBusy() && !document->currentImage().isNull(), 5000);
        QFile blocker(m_temp->filePath("blocked")); QVERIFY(blocker.open(QIODevice::WriteOnly)); blocker.write("not a directory"); blocker.close();
        bool retryObserved = false, submitted = false;
        QTimer retry; retry.setInterval(20);
        connect(&retry, &QTimer::timeout, &window, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget()); if (!dialog || dialog->objectName() != "imageExportDialog") return;
            auto *path = dialog->findChild<QLineEdit *>("exportPath"); auto *width = dialog->findChild<QSpinBox *>("exportWidth"); auto *format = dialog->findChild<QComboBox *>("exportFormat"); auto *message = dialog->findChild<QLabel *>("exportValidationMessage");
            if (!path || !width || !format || !message) { dialog->reject(); return; }
            if (!submitted) { submitted = true; width->setValue(90); path->setText(m_temp->filePath("blocked/output.png")); dialog->accept(); }
            else if (!window.property("fileOperationPending").toBool() && message->text().contains("저장하지 못했습니다")) { retryObserved = width->value() == 90; path->setText(outputPath); format->setCurrentIndex(format->findData(QByteArray("jpeg"))); dialog->accept(); retry.stop(); }
        });
        QTimer watchdog; watchdog.setSingleShot(true); connect(&watchdog, &QTimer::timeout, &window, [] { if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) dialog->reject(); }); watchdog.start(8000); retry.start();
        window.findChild<QAction *>("exportImageAction")->trigger(); retry.stop(); watchdog.stop(); QVERIFY(submitted && retryObserved);
        const QString jpeg = m_temp->filePath("converted.jpeg"); QVERIFY(QFileInfo::exists(jpeg)); QCOMPARE(QImageReader::imageFormat(jpeg), QByteArray("jpeg")); QCOMPARE(QImage(jpeg).width(), 90); QVERIFY(!QFileInfo::exists(outputPath));
    }

    void fileDropAcrossWindowSurfaces_data()
    {
        QTest::addColumn<QString>("targetName");
        QTest::newRow("window") << QStringLiteral("window");
        QTest::newRow("sidebar") << QStringLiteral("sidebar");
        QTest::newRow("toolbar") << QStringLiteral("toolbar");
        QTest::newRow("native-metal-surface") << QStringLiteral("surface");
    }

    void fileDropAcrossWindowSurfaces()
    {
        { QSettings preferences(qEnvironmentVariable("IKVIEW_SETTINGS_PATH"), QSettings::IniFormat); preferences.setValue("openContainingFolder", true); }
        QFETCH(QString, targetName);
        const QString directory = m_temp->filePath(QStringLiteral("photos"));
        QVERIFY(seedImages(directory));
        ik::MainWindow window;
        window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *document = window.findChild<ik::Document *>();
        auto *list = window.findChild<QListView *>(QStringLiteral("fileList"));
        auto *toolbar = window.findChild<QToolBar *>(QStringLiteral("mainToolbar"));
        QVERIFY(document); QVERIFY(list); QVERIFY(toolbar);
        QObject *target = &window;
        if (targetName == QStringLiteral("sidebar")) target = list->viewport();
        if (targetName == QStringLiteral("toolbar")) target = toolbar;
        if (targetName == QStringLiteral("surface")) {
            QTRY_VERIFY_WITH_TIMEOUT(surfaceFor(window), 3000);
            target = surfaceFor(window);
        }
        const auto result = deliverDrop(target, {QUrl::fromLocalFile(QDir(directory).filePath(QStringLiteral("page2.png")))});
        QVERIFY2(result.enterAccepted, "A valid local image must be accepted on drag entry");
        QVERIFY2(result.moveAccepted, "Cocoa native dragging requires acceptance while moving as well as on entry");
        QVERIFY2(result.dropAccepted, "The accepted drag must actually open when dropped");
        QTRY_VERIFY_WITH_TIMEOUT(!document->isBusy() && !document->currentImage().isNull(), 5000);
        QCOMPARE(document->count(), 3);
        QCOMPARE(document->currentEntry().name, QStringLiteral("page2.png"));
        QTRY_VERIFY_WITH_TIMEOUT(selectionMatches(list, document), 3000);
    }

    void windowDropsNeverNegotiateMove_data()
    {
        QTest::addColumn<QString>("targetName");
        QTest::addColumn<int>("possibleActions");
        QTest::addColumn<int>("expectedAction");
        const QStringList targets {QStringLiteral("window"), QStringLiteral("toolbar"), QStringLiteral("sidebar")};
        const QList<QPair<QString, Qt::DropActions>> actions {
            {QStringLiteral("copy"), Qt::CopyAction},
            {QStringLiteral("link"), Qt::LinkAction},
            {QStringLiteral("move-only"), Qt::MoveAction},
            {QStringLiteral("copy-and-move"), Qt::CopyAction | Qt::MoveAction},
            {QStringLiteral("link-and-move"), Qt::LinkAction | Qt::MoveAction}
        };
        for (const auto &target : targets) for (const auto &item : actions) {
            const auto expected = item.second.testFlag(Qt::CopyAction) ? Qt::CopyAction
                : item.second.testFlag(Qt::LinkAction) ? Qt::LinkAction : Qt::IgnoreAction;
            QTest::newRow(qPrintable(target + '-' + item.first)) << target << int(item.second) << int(expected);
        }
    }

    void windowDropsNeverNegotiateMove()
    {
        QFETCH(QString, targetName);
        QFETCH(int, possibleActions);
        QFETCH(int, expectedAction);
        const QString directory = m_temp->filePath(QStringLiteral("photos"));
        QVERIFY(seedImages(directory));
        ik::MainWindow window; window.show(); window.openPaths({directory});
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *document = window.findChild<ik::Document *>();
        auto *list = window.findChild<QListView *>(QStringLiteral("fileList"));
        auto *toolbar = window.findChild<QToolBar *>(QStringLiteral("mainToolbar"));
        QVERIFY(document); QVERIFY(list); QVERIFY(toolbar);
        QTRY_VERIFY_WITH_TIMEOUT(!document->isBusy() && !document->currentImage().isNull(), 5000);
        const QString before = document->currentEntry().key();
        const QImage pixels = document->currentImage();
        const QString droppedFile = QDir(directory).filePath(QStringLiteral("page2.png"));
        QObject *target = targetName == QStringLiteral("toolbar") ? static_cast<QObject *>(toolbar)
            : targetName == QStringLiteral("sidebar") ? static_cast<QObject *>(list->viewport()) : static_cast<QObject *>(&window);
        const auto result = deliverDrop(target, {QUrl::fromLocalFile(droppedFile)}, Qt::DropActions(possibleActions));
        if (expectedAction == int(Qt::IgnoreAction)) {
            QVERIFY(!result.enterAccepted && !result.moveAccepted && !result.dropAccepted);
            QVERIFY(!document->isBusy());
            QCOMPARE(document->currentEntry().key(), before);
            QCOMPARE(document->currentImage(), pixels);
        } else {
            QVERIFY(result.enterAccepted && result.moveAccepted && result.dropAccepted);
            QCOMPARE(int(result.enterAction), expectedAction);
            QCOMPARE(int(result.moveAction), expectedAction);
            QCOMPARE(int(result.dropAction), expectedAction);
            QTRY_VERIFY_WITH_TIMEOUT(!document->isBusy() && !document->currentImage().isNull(), 5000);
            QCOMPARE(document->currentEntry().name, QStringLiteral("page2.png"));
        }
        QVERIFY2(QFileInfo::exists(droppedFile), "A viewer drop must never remove or move the source file");
    }

    void folderAndMultipleFileDrops()
    {
        const QString directory = m_temp->filePath(QStringLiteral("photos"));
        QVERIFY(seedImages(directory, 4));
        ik::MainWindow window; window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *document = window.findChild<ik::Document *>(); QVERIFY(document);
        QTRY_VERIFY_WITH_TIMEOUT(surfaceFor(window), 3000);
        auto result = deliverDrop(surfaceFor(window), {QUrl::fromLocalFile(directory)});
        QVERIFY(result.enterAccepted && result.moveAccepted && result.dropAccepted);
        QTRY_VERIFY_WITH_TIMEOUT(!document->isBusy() && !document->currentImage().isNull(), 5000);
        QCOMPARE(document->count(), 4);
        result = deliverDrop(&window, {
            QUrl::fromLocalFile(QDir(directory).filePath(QStringLiteral("page4.png"))),
            QUrl::fromLocalFile(QDir(directory).filePath(QStringLiteral("page2.png")))
        });
        QVERIFY(result.enterAccepted && result.moveAccepted && result.dropAccepted);
        QTRY_VERIFY_WITH_TIMEOUT(!document->isBusy() && document->count() == 2 && !document->currentImage().isNull(), 5000);
        QCOMPARE(document->entries()[0].name, QStringLiteral("page2.png"));
        QCOMPARE(document->entries()[1].name, QStringLiteral("page4.png"));
    }

    void invalidDropsKeepCurrentDocument()
    {
        const QString directory = m_temp->filePath(QStringLiteral("photos"));
        QVERIFY(seedImages(directory));
        ik::MainWindow window; window.show(); window.openPaths({directory});
        auto *document = window.findChild<ik::Document *>(); QVERIFY(document);
        QTRY_VERIFY_WITH_TIMEOUT(!document->isBusy() && !document->currentImage().isNull(), 5000);
        const QString before = document->currentEntry().key();
        const QImage pixels = document->currentImage();
        QFile unsupported(m_temp->filePath(QStringLiteral("notes.txt")));
        QVERIFY(unsupported.open(QIODevice::WriteOnly)); unsupported.write("not an image"); unsupported.close();
        const QList<QList<QUrl>> invalid = {
            {QUrl(QStringLiteral("https://example.com/photo.png"))},
            {QUrl::fromLocalFile(m_temp->filePath(QStringLiteral("missing.png")))},
            {QUrl::fromLocalFile(unsupported.fileName())},
        };
        for (const auto &urls : invalid) {
            const auto result = deliverDrop(&window, urls);
            QVERIFY(!result.enterAccepted && !result.dropAccepted);
            QCOMPARE(document->currentEntry().key(), before);
            QCOMPARE(document->currentImage(), pixels);
            QVERIFY(!document->isBusy());
        }
        QMimeData text; text.setText(QStringLiteral("not an image"));
        QDragEnterEvent textEnter(QPoint(20, 20), Qt::CopyAction, &text, Qt::LeftButton, Qt::NoModifier);
        QApplication::sendEvent(&window, &textEnter);
        QVERIFY(!textEnter.isAccepted());
        QCOMPARE(document->currentEntry().key(), before);
    }

    void keyboardNavigationKeepsSidebarSelectedAndVisible()
    {
        const QString directory = m_temp->filePath(QStringLiteral("photos"));
        QVERIFY(seedImages(directory, 35));
        ik::MainWindow window; window.resize(960, 620); window.show(); window.openPaths({directory});
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *document = window.findChild<ik::Document *>();
        auto *list = window.findChild<QListView *>(QStringLiteral("fileList"));
        QVERIFY(document); QVERIFY(list);
        QTRY_VERIFY_WITH_TIMEOUT(!document->isBusy() && !document->currentImage().isNull(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(surfaceFor(window), 3000);
        window.activateWindow();
        auto *surface = surfaceFor(window);
        QTest::mouseClick(surface, Qt::LeftButton, Qt::NoModifier, QPoint(surface->width() / 2, surface->height() / 2));
        QTest::keyClick(surface, Qt::Key_Right);
        QTRY_COMPARE_WITH_TIMEOUT(document->currentIndex(), 1, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(selectionMatches(list, document), 3000);
        QTest::keyClick(surface, Qt::Key_End);
        QTRY_COMPARE_WITH_TIMEOUT(document->currentIndex(), 34, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(selectionMatches(list, document), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(list->viewport()->rect().intersects(list->visualRect(list->currentIndex())), 3000);
        QTest::keyClick(surface, Qt::Key_Home);
        QTRY_COMPARE_WITH_TIMEOUT(document->currentIndex(), 0, 3000);
        list->setFocus();
        QTest::keyClick(list, Qt::Key_Down);
        QTRY_COMPARE_WITH_TIMEOUT(document->currentIndex(), 1, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(selectionMatches(list, document), 3000);
        QTest::keyClick(list, Qt::Key_Down);
        QTRY_COMPARE_WITH_TIMEOUT(document->currentIndex(), 2, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(selectionMatches(list, document), 3000);
    }

    void cachedNavigationRepaintsPreviousAndCurrentSelection()
    {
        const QString directory = m_temp->filePath(QStringLiteral("photos"));
        QVERIFY(seedImages(directory));
        ik::MainWindow window; window.resize(960, 620); window.show(); window.openPaths({directory});
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *document = window.findChild<ik::Document *>();
        auto *list = window.findChild<QListView *>(QStringLiteral("fileList"));
        QVERIFY(document); QVERIFY(list);
        QTRY_VERIFY_WITH_TIMEOUT(!document->isBusy() && !document->currentImage().isNull(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!document->cachedImage(1).isNull(), 3000);
        for (int row = 0; row < 3; ++row) {
            const QColor expected = QColor::fromHsv(row * 43 % 360, 220, 230);
            QTRY_COMPARE_WITH_TIMEOUT(thumbnailCenter(list, row).rgb(), expected.rgb(), 3000);
        }
        QTRY_VERIFY_WITH_TIMEOUT(surfaceFor(window), 3000);
        window.activateWindow();
        auto *surface = surfaceFor(window);
        QTest::mouseClick(surface, Qt::LeftButton, Qt::NoModifier, QPoint(surface->width() / 2, surface->height() / 2));
        QTest::qWait(150); // Let initial focus and thumbnail paints finish before observing navigation.
        QSignalSpy thumbnailChanges(list->model(), &QAbstractItemModel::dataChanged);
        ViewportPaintObserver observer;
        list->viewport()->installEventFilter(&observer);
        const QPoint previousRow = list->visualRect(list->model()->index(0, 0)).center();
        const QPoint currentRow = list->visualRect(list->model()->index(1, 0)).center();
        QVERIFY(list->viewport()->rect().contains(previousRow));
        QVERIFY(list->viewport()->rect().contains(currentRow));

        QTest::keyClick(surface, Qt::Key_Right);
        QTRY_COMPARE_WITH_TIMEOUT(document->currentIndex(), 1, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(selectionMatches(list, document), 3000);
        // Observe naturally delivered paint events: grab()/render() would hide the missing update.
        QTRY_VERIFY_WITH_TIMEOUT(observer.painted.contains(previousRow) && observer.painted.contains(currentRow), 1500);
        QCOMPARE(thumbnailChanges.count(), 0);
    }

    void typingFilterDoesNotTriggerViewerCommands()
    {
        const QString directory = m_temp->filePath(QStringLiteral("photos")); QVERIFY(seedImages(directory));
        ik::MainWindow window; window.show(); window.openPaths({directory});
        auto *document = window.findChild<ik::Document *>();
        auto *filter = window.findChild<QLineEdit *>(QStringLiteral("fileFilter"));
        auto *list = window.findChild<QListView *>(QStringLiteral("fileList"));
        QVERIFY(document); QVERIFY(filter); QVERIFY(list);
        QTRY_VERIFY_WITH_TIMEOUT(!document->isBusy() && !document->currentImage().isNull(), 5000);
        const QString before = document->currentEntry().key();
        filter->setFocus();
        bool unexpectedDialog = false;
        QTimer dialogWatchdog;
        connect(&dialogWatchdog, &QTimer::timeout, &window, [&] {
            if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) {
                unexpectedDialog = true;
                dialog->reject();
            }
        });
        dialogWatchdog.start(50);
        QTest::keyClicks(filter, QStringLiteral("fbp17"));
        dialogWatchdog.stop();
        QVERIFY(!unexpectedDialog);
        QCOMPARE(filter->text(), QStringLiteral("fbp17"));
        QCOMPARE(document->currentEntry().key(), before);
        QVERIFY(!QApplication::activeModalWidget());
        QVERIFY(!window.isFullScreen());
        filter->setText(QStringLiteral("page1"));
        QTRY_COMPARE_WITH_TIMEOUT(list->model()->rowCount(), 1, 3000);
        auto *next = window.findChild<QAction *>(QStringLiteral("nextImageAction")); QVERIFY(next);
        next->trigger();
        QTRY_COMPARE_WITH_TIMEOUT(document->currentIndex(), 1, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(selectionMatches(list, document), 3000);
        QCOMPARE(filter->text(), QString());
    }

    void editorTargetedKeysStayTextWithCanvasFocus()
    {
        const QString directory = m_temp->filePath(QStringLiteral("photos")); QVERIFY(seedImages(directory));
        ik::MainWindow window; window.show(); window.openPaths({directory});
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *document = window.findChild<ik::Document *>();
        auto *filter = window.findChild<QLineEdit *>(QStringLiteral("fileFilter"));
        auto *canvas = window.findChild<ik::ImageCanvas *>();
        QVERIFY(document); QVERIFY(filter); QVERIFY(canvas);
        QTRY_VERIFY_WITH_TIMEOUT(!document->isBusy() && !document->currentImage().isNull(), 5000);
        const QString before = document->currentEntry().key();
        QApplication::setActiveWindow(&window);
        canvas->setFocus();
        QCOMPARE(QApplication::focusWidget(), canvas);

        bool unexpectedDialog = false;
        QTimer dialogWatchdog;
        connect(&dialogWatchdog, &QTimer::timeout, &window, [&] {
            if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) {
                unexpectedDialog = true;
                dialog->reject();
            }
        });
        dialogWatchdog.start(50);
        // Receiver context must win even while native activation leaves global focus on the canvas.
        QTest::keyClicks(filter, QStringLiteral("fbp17"));
        dialogWatchdog.stop();
        QVERIFY(!unexpectedDialog);
        QCOMPARE(filter->text(), QStringLiteral("fbp17"));
        QCOMPARE(document->currentEntry().key(), before);
        QVERIFY(!window.isFullScreen());
    }

    void animationFramesDoNotResetFilterOrSidebarScroll()
    {
        { QSettings preferences(qEnvironmentVariable("IKVIEW_SETTINGS_PATH"), QSettings::IniFormat); preferences.setValue("openContainingFolder", true); }
        const QString directory = m_temp->filePath(QStringLiteral("photos"));
        QVERIFY(seedImages(directory, 40));
        const QByteArray animation = QByteArray::fromHex(
            "47494638396101000100800000ff00000000ff"
            "21ff0b4e45545343415045322e300301000000"
            "21f90400050000002c0000000001000100000202440100"
            "21f90400050000002c00000000010001000002024c01003b");
        QFile file(QDir(directory).filePath(QStringLiteral("animation.gif")));
        QVERIFY(file.open(QIODevice::WriteOnly));
        QCOMPARE(file.write(animation), animation.size()); file.close();
        ik::MainWindow window; window.resize(960, 620); window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *document = window.findChild<ik::Document *>();
        auto *filter = window.findChild<QLineEdit *>(QStringLiteral("fileFilter"));
        auto *list = window.findChild<QListView *>(QStringLiteral("fileList"));
        auto *metadata = window.findChild<QTreeWidget *>();
        auto *infoAction = window.findChild<QAction *>(QStringLiteral("toggleMetadataAction"));
        QVERIFY(document); QVERIFY(filter); QVERIFY(list); QVERIFY(metadata); QVERIFY(infoAction);
        QSignalSpy frames(document, &ik::Document::imageReady);
        window.openPaths({file.fileName()});
        QTRY_VERIFY_WITH_TIMEOUT(!document->isBusy() && frames.count() >= 3, 5000);
        QCOMPARE(document->currentEntry().name, QStringLiteral("animation.gif"));
        const QString playing = document->currentEntry().key();
        infoAction->trigger(); metadata->setMaximumHeight(110);
        QVERIFY(metadata->topLevelItemCount() >= 4);
        auto *selectedMetadata = metadata->topLevelItem(3);
        metadata->setCurrentItem(selectedMetadata, 0, QItemSelectionModel::ClearAndSelect);
        QTRY_VERIFY_WITH_TIMEOUT(metadata->verticalScrollBar()->maximum() > 0, 3000);
        metadata->verticalScrollBar()->setValue(metadata->verticalScrollBar()->maximum());
        const int metadataScroll = metadata->verticalScrollBar()->value();

        filter->setText(QStringLiteral("page"));
        QTRY_COMPARE_WITH_TIMEOUT(list->model()->rowCount(), 40, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(list->verticalScrollBar()->maximum() > 0, 3000);
        list->verticalScrollBar()->setValue(std::min(500, list->verticalScrollBar()->maximum() / 2));
        const int filteredScroll = list->verticalScrollBar()->value();
        const int filteredFrames = frames.count();
        auto *canvas = window.findChild<ik::ImageCanvas *>(); auto *pan = window.findChild<QAction *>("panImage0Action"); QVERIFY(canvas && pan);
        const auto syncsBefore = canvas->activityStats().sceneSyncCalls; QSignalSpy panChanges(pan, &QAction::changed);
        QTRY_VERIFY_WITH_TIMEOUT(frames.count() >= filteredFrames + 3, 2000);
        QCOMPARE(panChanges.size(), 0);
        QVERIFY(canvas->activityStats().sceneSyncCalls - syncsBefore <= quint64(frames.count() - filteredFrames + 4));
        QVERIFY2(document->currentEntry().key() == playing, qPrintable("Unexpected selection while animating: " + document->currentEntry().name));
        QCOMPARE(filter->text(), QStringLiteral("page"));
        QCOMPARE(list->model()->rowCount(), 40);
        QCOMPARE(list->verticalScrollBar()->value(), filteredScroll);
        QCOMPARE(document->currentEntry().key(), playing);
        QCOMPARE(metadata->selectedItems().size(), 1);
        QCOMPARE(metadata->selectedItems().first(), selectedMetadata);
        QCOMPARE(metadata->verticalScrollBar()->value(), metadataScroll);

        // A visible-but-offscreen current row must not pull the list back to
        // the top every time the same animation produces another frame.
        filter->clear();
        QTRY_COMPARE_WITH_TIMEOUT(list->model()->rowCount(), 41, 3000);
        list->verticalScrollBar()->setValue(std::min(500, list->verticalScrollBar()->maximum() / 2));
        const int browsingScroll = list->verticalScrollBar()->value();
        QVERIFY(browsingScroll > 0);
        const int browsingFrames = frames.count();
        QTRY_VERIFY_WITH_TIMEOUT(frames.count() >= browsingFrames + 3, 2000);
        QCOMPARE(list->verticalScrollBar()->value(), browsingScroll);
        QCOMPARE(document->currentEntry().key(), playing);
        QCOMPARE(metadata->selectedItems().size(), 1);
        QCOMPARE(metadata->selectedItems().first(), selectedMetadata);
        QCOMPARE(metadata->verticalScrollBar()->value(), metadataScroll);
    }

    void chromeTogglesAndCleanViewRestorePreviousLayout()
    {
        ik::MainWindow window; window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *listAction = window.findChild<QAction *>(QStringLiteral("toggleFileListAction"));
        auto *toolbarAction = window.findChild<QAction *>(QStringLiteral("toggleToolbarAction"));
        auto *statusAction = window.findChild<QAction *>(QStringLiteral("toggleStatusAction"));
        auto *clean = window.findChild<QAction *>(QStringLiteral("cleanViewAction"));
        auto *restore = window.findChild<QAction *>(QStringLiteral("restoreViewAction"));
        auto *panel = window.findChild<QWidget *>(QStringLiteral("filePanel"));
        auto *toolbar = window.findChild<QToolBar *>(QStringLiteral("mainToolbar"));
        QVERIFY(listAction); QVERIFY(toolbarAction); QVERIFY(statusAction); QVERIFY(clean); QVERIFY(restore); QVERIFY(panel); QVERIFY(toolbar);
        QVERIFY(!panel->isHidden()); QVERIFY(!toolbar->isHidden());
        listAction->trigger(); QVERIFY(panel->isHidden()); QVERIFY(!listAction->isChecked());
        listAction->trigger(); QVERIFY(!panel->isHidden()); QVERIFY(listAction->isChecked());
        toolbarAction->trigger(); QVERIFY(toolbar->isHidden()); QVERIFY(!toolbarAction->isChecked());
        clean->trigger();
        QVERIFY(panel->isHidden()); QVERIFY(toolbar->isHidden()); QVERIFY(window.statusBar()->isHidden());
        clean->trigger();
        QVERIFY(!panel->isHidden()); QVERIFY(toolbar->isHidden()); QVERIFY(!window.statusBar()->isHidden());
        restore->trigger();
        QVERIFY(!panel->isHidden()); QVERIFY(!toolbar->isHidden()); QVERIFY(!window.statusBar()->isHidden());
        QVERIFY(listAction->isChecked()); QVERIFY(toolbarAction->isChecked()); QVERIFY(statusAction->isChecked());
    }

    void chromePreferencesSurviveReopening()
    {
        {
            ik::MainWindow window; window.show();
            auto *list = window.findChild<QAction *>(QStringLiteral("toggleFileListAction"));
            auto *toolbar = window.findChild<QAction *>(QStringLiteral("toggleToolbarAction"));
            QVERIFY(list); QVERIFY(toolbar);
            if (list->isChecked()) list->trigger();
            if (toolbar->isChecked()) toolbar->trigger();
        }
        ik::MainWindow restored; restored.show();
        auto *list = restored.findChild<QAction *>(QStringLiteral("toggleFileListAction"));
        auto *toolbar = restored.findChild<QAction *>(QStringLiteral("toggleToolbarAction"));
        auto *panel = restored.findChild<QWidget *>(QStringLiteral("filePanel"));
        auto *bar = restored.findChild<QToolBar *>(QStringLiteral("mainToolbar"));
        QVERIFY(list); QVERIFY(toolbar); QVERIFY(panel); QVERIFY(bar);
        QVERIFY(!list->isChecked()); QVERIFY(!toolbar->isChecked());
        QVERIFY(panel->isHidden()); QVERIFY(bar->isHidden());
    }

    void nativeTabAndEscapeRestoreChrome()
    {
        ik::MainWindow window; window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *clean = window.findChild<QAction *>(QStringLiteral("cleanViewAction"));
        auto *toolbarAction = window.findChild<QAction *>(QStringLiteral("toggleToolbarAction"));
        auto *panel = window.findChild<QWidget *>(QStringLiteral("filePanel"));
        auto *toolbar = window.findChild<QToolBar *>(QStringLiteral("mainToolbar"));
        QVERIFY(clean); QVERIFY(toolbarAction); QVERIFY(panel); QVERIFY(toolbar);
        toolbarAction->trigger();
        QVERIFY(toolbar->isHidden());
        QTRY_VERIFY_WITH_TIMEOUT(surfaceFor(window), 3000);
        auto *surface = surfaceFor(window);
        window.activateWindow();
        QTest::mouseClick(surface, Qt::LeftButton, Qt::NoModifier, QPoint(surface->width() / 2, surface->height() / 2));
        QTest::keyClick(surface, Qt::Key_Tab);
        QTRY_VERIFY_WITH_TIMEOUT(clean->isChecked() && panel->isHidden(), 3000);
        QTest::keyClick(surface, Qt::Key_Tab);
        QTRY_VERIFY_WITH_TIMEOUT(!clean->isChecked() && !panel->isHidden(), 3000);
        QVERIFY(toolbar->isHidden());
        QTest::keyClick(surface, Qt::Key_Tab);
        QTRY_VERIFY_WITH_TIMEOUT(clean->isChecked(), 3000);
        QTest::keyClick(surface, Qt::Key_Escape);
        QTRY_VERIFY_WITH_TIMEOUT(!clean->isChecked() && !panel->isHidden(), 3000);
        QVERIFY(toolbar->isHidden());
    }

    void fullScreenEscapePreservesHiddenToolbar()
    {
        ik::MainWindow window; window.show();
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        auto *fullscreen = window.findChild<QAction *>(QStringLiteral("fullScreenAction"));
        auto *toolbarAction = window.findChild<QAction *>(QStringLiteral("toggleToolbarAction"));
        auto *toolbar = window.findChild<QToolBar *>(QStringLiteral("mainToolbar"));
        QVERIFY(fullscreen); QVERIFY(toolbarAction); QVERIFY(toolbar);
        toolbarAction->trigger();
        fullscreen->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(window.isFullScreen(), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(surfaceFor(window), 3000);
        QTest::keyClick(surfaceFor(window), Qt::Key_Escape);
        QTRY_VERIFY_WITH_TIMEOUT(!window.isFullScreen(), 3000);
        QVERIFY(toolbar->isHidden());
        QVERIFY(!toolbarAction->isChecked());
    }

    void archiveThumbnailsArriveWithoutReopening()
    {
        const QString path = m_temp->filePath(QStringLiteral("comic.cbz")); QVERIFY(writeComic(path));
        ik::MainWindow window; window.show(); window.openPaths({path});
        auto *document = window.findChild<ik::Document *>();
        auto *list = window.findChild<QListView *>(QStringLiteral("fileList"));
        QVERIFY(document); QVERIFY(list);
        QTRY_VERIFY_WITH_TIMEOUT(!document->isBusy() && document->count() == 2 && !document->currentImage().isNull(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(thumbnailCenter(list, 0).red() > 200 && thumbnailCenter(list, 0).green() < 80, 5000);
        QTRY_VERIFY_WITH_TIMEOUT(thumbnailCenter(list, 1).green() > 200 && thumbnailCenter(list, 1).red() < 80, 5000);
        const QString before = document->currentEntry().key();
        QCOMPARE(before, document->entries()[0].key());
        QTRY_VERIFY_WITH_TIMEOUT(selectionMatches(list, document), 3000);
    }

    void rapidNavigationLeavesOneConsistentSelection()
    {
        const QString directory = m_temp->filePath(QStringLiteral("photos")); QVERIFY(seedImages(directory, 20));
        ik::MainWindow window; window.show(); window.openPaths({directory});
        auto *document = window.findChild<ik::Document *>();
        auto *list = window.findChild<QListView *>(QStringLiteral("fileList"));
        auto *next = window.findChild<QAction *>(QStringLiteral("nextImageAction"));
        QVERIFY(document); QVERIFY(list); QVERIFY(next);
        QTRY_VERIFY_WITH_TIMEOUT(!document->isBusy() && !document->currentImage().isNull(), 5000);
        for (int i = 0; i < 12; ++i) next->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!document->isBusy() && !document->currentImage().isNull(), 5000);
        QCOMPARE(document->currentIndex(), 12);
        QTRY_VERIFY_WITH_TIMEOUT(selectionMatches(list, document), 3000);
        auto *canvas = window.findChild<ik::ImageCanvas *>(); QVERIFY(canvas);
        QTRY_COMPARE_WITH_TIMEOUT(canvas->transformedImage().size(), document->currentImage().size(), 3000);
    }

    void closeShortcutClosesWindowInsteadOfClearingImage()
    {
        const QString directory = m_temp->filePath(QStringLiteral("photos")); QVERIFY(seedImages(directory));
        ik::MainWindow window; window.show(); window.openPaths({directory});
        auto *document = window.findChild<ik::Document *>();
        auto *canvas = window.findChild<ik::ImageCanvas *>();
        auto *list = window.findChild<QListView *>(QStringLiteral("fileList"));
        auto *quit = window.findChild<QAction *>(QStringLiteral("quitApplicationAction"));
        auto *metadata = window.findChild<QTreeWidget *>();
        QVERIFY(document); QVERIFY(canvas); QVERIFY(list); QVERIFY(quit); QVERIFY(metadata);
        QVERIFY(!window.findChild<QAction *>(QStringLiteral("closeImageAction")));
        QTRY_VERIFY_WITH_TIMEOUT(!document->isBusy() && !document->currentImage().isNull(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(metadata->topLevelItemCount() > 0, 3000);
        QVERIFY(quit->shortcuts().contains(QKeySequence("Ctrl+W")));
        QTest::keyClick(&window, Qt::Key_W, Qt::ControlModifier);
        QTRY_VERIFY_WITH_TIMEOUT(!window.isVisible(), 3000);
        QCOMPARE(document->count(), 3);
        QVERIFY(!document->currentImage().isNull());
    }
};

int main(int argc, char **argv)
{
    QCoreApplication::setAttribute(Qt::AA_DontUseNativeDialogs);
#ifdef Q_OS_MACOS
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Metal);
#endif
    QApplication application(argc, argv);
    QApplication::setQuitOnLastWindowClosed(false);
    QCoreApplication::setOrganizationName(QStringLiteral("ikview-ux-tests"));
    QCoreApplication::setApplicationName(QStringLiteral("ux-tests"));
    QStandardPaths::setTestModeEnabled(true);
    UxTests tests;
    return QTest::qExec(&tests, argc, argv);
}

#include "ux_tests.moc"
