#include "document.h"
#include "imagecanvas.h"
#include "mainwindow.h"
#include "preferencesdialog.h"
#include "shortcutmanager.h"

#include <QAction>
#include <QApplication>
#include <QBuffer>
#include <QCheckBox>
#include <QComboBox>
#include <QDataStream>
#include <QDialog>
#include <QDir>
#include <QFile>
#include <QLineEdit>
#include <QKeySequenceEdit>
#include <QListView>
#include <QListWidget>
#include <QPointer>
#include <QPushButton>
#include <QQuickWindow>
#include <QSettings>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QtTest>
#include <archive.h>
#include <archive_entry.h>
#include <memory>

namespace {
bool seedImages(const QString &directory)
{
    if (!QDir().mkpath(directory)) return false;
    for (int i = 0; i < 3; ++i) {
        QImage image(80, 60, QImage::Format_RGB32);
        image.fill(QColor::fromHsv(i * 90, 200, 220));
        if (!image.save(QDir(directory).filePath(QString("page%1.png").arg(i + 1)))) return false;
    }
    return true;
}

QQuickWindow *surfaceFor(ik::MainWindow &window)
{
    auto *canvas = window.findChild<ik::ImageCanvas *>();
    for (auto *native : QGuiApplication::allWindows()) {
        auto *surface = qobject_cast<QQuickWindow *>(native);
        if (surface && surface->property("ikviewCanvas").value<QObject *>() == canvas) return surface;
    }
    return nullptr;
}

QListWidget *bookmarkList(QDialog *dialog)
{
    auto *list = dialog->findChild<QListWidget *>("bookmarksList");
    return list ? list : dialog->findChild<QListWidget *>("bookmarkList");
}

QPushButton *bookmarkButton(QDialog *dialog, bool open)
{
    auto *button = dialog->findChild<QPushButton *>(open ? "openBookmarkButton" : "removeBookmarkButton");
    return button ? button : dialog->findChild<QPushButton *>(open ? "bookmarkOpenButton" : "bookmarkDeleteButton");
}

class ModalWatchdog {
    QTimer timer;
public:
    explicit ModalWatchdog(QObject *context)
    {
        timer.setSingleShot(true);
        QObject::connect(&timer, &QTimer::timeout, context, [] {
            if (auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget())) dialog->reject();
        });
        timer.start(5000);
    }
};

bool openFirstBookmark(QToolButton *button, ik::MainWindow &window)
{
    bool observed = false;
    ModalWatchdog watchdog(&window);
    // The dialog is modal; inject selection while its event loop runs.
    QTimer::singleShot(0, &window, [&] {
        auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
        if (!dialog) return;
        auto *list = bookmarkList(dialog);
        auto *open = bookmarkButton(dialog, true);
        if (dialog->objectName() == "bookmarksDialog" && list && list->count() && open) {
            list->setCurrentRow(0);
            observed = open->isEnabled();
            if (observed) open->click();
        }
        if (!observed) dialog->reject();
    });
    button->click();
    return observed;
}

bool writeTwoPageTiff(const QString &path)
{
    QByteArray bytes;
    QDataStream data(&bytes, QIODevice::WriteOnly);
    data.setByteOrder(QDataStream::LittleEndian);
    data.writeRawData("II", 2);
    data << quint16(42) << quint32(8);
    for (int page = 0; page < 2; ++page) {
        data << quint16(10);
        const auto tag = [&](quint16 id, quint16 type, quint32 count, quint32 value) { data << id << type << count << value; };
        tag(256, 4, 1, 2); tag(257, 4, 1, 2); tag(258, 3, 3, 260 + page * 6);
        tag(259, 3, 1, 1); tag(262, 3, 1, 2); tag(273, 4, 1, 272 + page * 12);
        tag(277, 3, 1, 3); tag(278, 4, 1, 2); tag(279, 4, 1, 12); tag(284, 3, 1, 1);
        data << quint32(page == 0 ? 134 : 0);
    }
    for (int i = 0; i < 6; ++i) data << quint16(8);
    for (int page = 0; page < 2; ++page)
        for (int i = 0; i < 4; ++i) data << quint8(page == 0 ? 255 : 0) << quint8(0) << quint8(page == 1 ? 255 : 0);
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

bool writeComic(const QString &path)
{
    archive *writer = archive_write_new();
    archive_write_set_format_zip(writer);
    if (archive_write_open_filename(writer, QFile::encodeName(path).constData()) != ARCHIVE_OK) {
        archive_write_free(writer);
        return false;
    }
    bool ok = true;
    for (int i = 0; i < 2; ++i) {
        QByteArray bytes;
        QBuffer buffer(&bytes);
        buffer.open(QIODevice::WriteOnly);
        QImage image(40, 30, QImage::Format_RGB32); image.fill(i ? Qt::blue : Qt::red);
        ok &= image.save(&buffer, "PNG");
        const QByteArray name = QString("page%1.png").arg(i + 1).toUtf8();
        archive_entry *entry = archive_entry_new();
        archive_entry_set_pathname_utf8(entry, name.constData());
        archive_entry_set_size(entry, bytes.size()); archive_entry_set_filetype(entry, AE_IFREG); archive_entry_set_perm(entry, 0644);
        ok &= archive_write_header(writer, entry) == ARCHIVE_OK;
        ok &= archive_write_data(writer, bytes.constData(), bytes.size()) == bytes.size();
        archive_entry_free(entry);
    }
    archive_write_close(writer); archive_write_free(writer);
    return ok;
}
}

class PreferencesIntegrationTests : public QObject {
    Q_OBJECT
    std::unique_ptr<QTemporaryDir> m_temp;
    QString settingsPath() const { return m_temp->filePath("settings.ini"); }
    QString imagesPath() const { return m_temp->filePath("images"); }
    QVariantList bookmarks() const {
        QSettings settings(settingsPath(), QSettings::IniFormat);
        settings.sync();
        return settings.value("bookmarks").toList();
    }
private slots:
    void init()
    {
        m_temp = std::make_unique<QTemporaryDir>();
        QVERIFY(m_temp->isValid());
        qputenv("IKVIEW_SETTINGS_PATH", settingsPath().toUtf8());
    }
    void cleanup()
    {
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        qunsetenv("IKVIEW_SETTINGS_PATH");
        m_temp.reset();
    }

    void toolbarReplacesViewSelectorAndOpensBookmarkList()
    {
        ik::MainWindow window; window.show();
        auto *toolbar = window.findChild<QToolBar *>("mainToolbar");
        auto *button = window.findChild<QToolButton *>("bookmarksButton");
        QVERIFY(toolbar && button);
        QVERIFY(toolbar->isAncestorOf(button));
        for (auto *other : toolbar->findChildren<QToolButton *>()) QVERIFY(other->text() != QStringLiteral("보기 ▾"));
        bool observed = false;
        ModalWatchdog watchdog(&window);
        QTimer::singleShot(0, &window, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (!dialog) return;
            auto *list = bookmarkList(dialog);
            auto *open = bookmarkButton(dialog, true);
            auto *remove = bookmarkButton(dialog, false);
            observed = dialog->objectName() == "bookmarksDialog" && list && list->count() == 0
                && open && remove && !open->isEnabled() && !remove->isEnabled();
            dialog->reject();
        });
        button->click();
        QVERIFY(observed);
    }

    void singleFileParentPreference_data()
    {
        QTest::addColumn<bool>("includeParent");
        QTest::newRow("selected-file-only") << false;
        QTest::newRow("containing-folder") << true;
    }
    void singleFileParentPreference()
    {
        QFETCH(bool, includeParent);
        QVERIFY(seedImages(imagesPath()));
        QSettings settings(settingsPath(), QSettings::IniFormat);
        settings.setValue("openContainingFolder", includeParent); settings.sync();
        ik::MainWindow window; window.show();
        auto *doc = window.findChild<ik::Document *>(); QVERIFY(doc);
        const auto second = QDir(imagesPath()).filePath("page2.png");
        window.openPaths({second});
        QTRY_VERIFY_WITH_TIMEOUT(!doc->isBusy() && !doc->currentImage().isNull(), 5000);
        QCOMPARE(doc->count(), includeParent ? 3 : 1);
        QCOMPARE(doc->currentEntry().path, second);
        QCOMPARE(doc->isFileScoped(), !includeParent);
        doc->close();
        const auto third = QDir(imagesPath()).filePath("page3.png");
        window.openPaths({third});
        QTRY_VERIFY_WITH_TIMEOUT(!doc->isBusy() && !doc->currentImage().isNull(), 5000);
        QCOMPARE(doc->count(), includeParent ? 3 : 1);
        QCOMPARE(doc->currentEntry().path, third);
        window.openPaths({imagesPath()});
        QTRY_VERIFY_WITH_TIMEOUT(!doc->isBusy() && doc->count() == 3 && !doc->currentImage().isNull(), 5000);
    }

    void bookmarksDeleteOnlyAfterSuccessfulOpen_data()
    {
        QTest::addColumn<bool>("removeAfterOpen");
        QTest::newRow("keep") << false;
        QTest::newRow("remove-after-open") << true;
    }
    void bookmarksDeleteOnlyAfterSuccessfulOpen()
    {
        QFETCH(bool, removeAfterOpen);
        QVERIFY(seedImages(imagesPath()));
        QSettings settings(settingsPath(), QSettings::IniFormat);
        settings.setValue("deleteBookmarkAfterOpen", removeAfterOpen); settings.sync();
        ik::MainWindow window; window.show(); window.openPaths({imagesPath()});
        auto *doc = window.findChild<ik::Document *>();
        auto *add = window.findChild<QAction *>("addBookmarkAction");
        auto *button = window.findChild<QToolButton *>("bookmarksButton");
        QVERIFY(doc && add && button);
        QTRY_VERIFY_WITH_TIMEOUT(!doc->isBusy() && !doc->currentImage().isNull(), 5000);
        doc->goTo(1); QTRY_VERIFY_WITH_TIMEOUT(!doc->isBusy() && doc->currentIndex() == 1, 5000);
        const auto target = doc->currentEntry().path;
        add->trigger(); QCOMPARE(bookmarks().size(), 1);
        doc->goTo(0); QTRY_VERIFY_WITH_TIMEOUT(!doc->isBusy() && doc->currentIndex() == 0, 5000);
        QVERIFY(openFirstBookmark(button, window));
        QTRY_VERIFY_WITH_TIMEOUT(!doc->isBusy() && doc->currentEntry().path == target && !doc->currentImage().isNull(), 5000);
        QTRY_COMPARE_WITH_TIMEOUT(bookmarks().size(), removeAfterOpen ? 0 : 1, 3000);
    }

    void failedBookmarkOpenKeepsRecordAndCurrentImage()
    {
        QVERIFY(seedImages(imagesPath()));
        const QString missing = m_temp->filePath("missing.png");
        QSettings settings(settingsPath(), QSettings::IniFormat);
        settings.setValue("deleteBookmarkAfterOpen", true);
        settings.setValue("bookmarks", QVariantList{QVariantMap{{"path", missing}, {"selectionPath", missing}, {"name", "Missing"}, {"page", 0}}});
        settings.sync();
        ik::MainWindow window; window.show(); window.openPaths({imagesPath()});
        auto *doc = window.findChild<ik::Document *>();
        auto *latest = window.findChild<QAction *>("latestBookmarkAction"); QVERIFY(doc && latest);
        QTRY_VERIFY_WITH_TIMEOUT(!doc->isBusy() && !doc->currentImage().isNull(), 5000);
        const auto original = doc->currentEntry().path;
        QSignalSpy errors(doc, &ik::Document::errorOccurred); latest->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!errors.isEmpty(), 5000);
        QCOMPARE(bookmarks().size(), 1);
        QCOMPARE(doc->currentEntry().path, original);
        QVERIFY(!doc->currentImage().isNull());
    }

    void bookmarkAutoDeleteWaitsForFilteredCanvasPreparation()
    {
        const auto path = m_temp->filePath("filtered.png");
        QImage image(80, 60, QImage::Format_RGB32); image.fill(QColor("#63a7d2"));
        QVERIFY(image.save(path));
        QSettings settings(settingsPath(), QSettings::IniFormat);
        settings.setValue("imageFilter", "sharpen");
        settings.setValue("deleteBookmarkAfterOpen", true);
        settings.setValue("bookmarks", QVariantList{QVariantMap{{"path", path}, {"selectionPath", path}, {"name", "Filtered image"}, {"page", 0}}});
        settings.sync();
        ik::MainWindow window; window.show();
        auto *doc = window.findChild<ik::Document *>();
        auto *canvas = window.findChild<ik::ImageCanvas *>();
        auto *button = window.findChild<QToolButton *>("bookmarksButton"); QVERIFY(doc && canvas && button);
        bool observedPreparingFrame = false;
        bool deletedBeforePreparationFinished = false;
        // MainWindow's earlier imageReady connection starts async filtering.
        // This callback runs in the same delivery, before completion events
        // can apply the prepared image, so early bookmark deletion is visible.
        connect(doc, &ik::Document::imageReady, &window, [&](const QImage &, const ik::ImageInfo &) {
            if (doc->currentEntry().path != path || !canvas->isPreparing()) return;
            observedPreparingFrame = true;
            deletedBeforePreparationFinished |= bookmarks().size() != 1;
        });
        QVERIFY(openFirstBookmark(button, window));
        QTRY_VERIFY_WITH_TIMEOUT(!doc->isBusy() && !doc->currentImage().isNull(), 5000);
        QVERIFY(observedPreparingFrame);
        QVERIFY(!deletedBeforePreparationFinished);
        QTRY_VERIFY_WITH_TIMEOUT(!canvas->isPreparing() && bookmarks().isEmpty(), 5000);
        QCOMPARE(canvas->imageFilter(), ik::ImageFilterMode::Sharpen);
    }

    void successfulTiffPageRestoreDeletesOnlyMatchingBookmark()
    {
        const auto path = m_temp->filePath("pages.tiff"); QVERIFY(writeTwoPageTiff(path));
        QSettings settings(settingsPath(), QSettings::IniFormat);
        settings.setValue("deleteBookmarkAfterOpen", true); settings.sync();
        ik::MainWindow window; window.show(); window.openPathsAt({path}, 0, 1);
        auto *doc = window.findChild<ik::Document *>();
        auto *add = window.findChild<QAction *>("addBookmarkAction");
        auto *latest = window.findChild<QAction *>("latestBookmarkAction"); QVERIFY(doc && add && latest);
        QTRY_VERIFY_WITH_TIMEOUT(!doc->isBusy() && doc->currentInfo().page == 1, 5000);
        add->trigger(); QCOMPARE(bookmarks().size(), 1);
        doc->setPage(0); QTRY_VERIFY_WITH_TIMEOUT(!doc->isBusy() && doc->currentInfo().page == 0, 5000);
        latest->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!doc->isBusy() && doc->currentInfo().page == 1 && !doc->currentImage().isNull(), 5000);
        QCOMPARE(doc->currentImage().pixelColor(0, 0), QColor(Qt::blue));
        QTRY_COMPARE_WITH_TIMEOUT(bookmarks().size(), 0, 3000);
    }

    void archiveBookmarkFollowsMemberAndPreservesOtherRecord()
    {
        const auto path = m_temp->filePath("comic.cbz"); QVERIFY(writeComic(path));
        QSettings settings(settingsPath(), QSettings::IniFormat);
        settings.setValue("deleteBookmarkAfterOpen", true); settings.sync();
        ik::MainWindow window; window.show(); window.openPaths({path});
        auto *doc = window.findChild<ik::Document *>();
        auto *add = window.findChild<QAction *>("addBookmarkAction");
        auto *latest = window.findChild<QAction *>("latestBookmarkAction");
        auto *sort = window.findChild<QAction *>("sortAction1"); QVERIFY(doc && add && latest && sort);
        QTRY_VERIFY_WITH_TIMEOUT(!doc->isBusy() && !doc->currentImage().isNull(), 5000);
        add->trigger(); doc->goTo(1); QTRY_VERIFY_WITH_TIMEOUT(!doc->isBusy() && doc->currentIndex() == 1, 5000);
        add->trigger(); QCOMPARE(bookmarks().size(), 2);
        sort->trigger(); doc->close(); latest->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!doc->isBusy() && doc->currentEntry().member == "page2.png" && !doc->currentImage().isNull(), 5000);
        QCOMPARE(doc->currentImage().pixelColor(0, 0), QColor(Qt::blue));
        QTRY_COMPARE_WITH_TIMEOUT(bookmarks().size(), 1, 3000);
        QCOMPARE(bookmarks().first().toMap().value("member").toString(), "page1.png");
    }

    void missingTiffPageKeepsBookmark()
    {
        const auto path = m_temp->filePath("pages.tiff"); QVERIFY(writeTwoPageTiff(path));
        QSettings settings(settingsPath(), QSettings::IniFormat);
        settings.setValue("deleteBookmarkAfterOpen", true);
        settings.setValue("bookmarks", QVariantList{QVariantMap{{"path", path}, {"selectionPath", path}, {"name", "Removed page"}, {"page", 20}}});
        settings.sync();
        ik::MainWindow window; window.show();
        auto *doc = window.findChild<ik::Document *>();
        auto *latest = window.findChild<QAction *>("latestBookmarkAction"); QVERIFY(doc && latest); latest->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!doc->isBusy() && !doc->currentImage().isNull(), 5000);
        QCOMPARE(bookmarks().size(), 1);
        QCOMPARE(doc->currentInfo().page, 0);
    }

    void supersededBookmarkOpenKeepsBookmark()
    {
        QVERIFY(seedImages(imagesPath()));
        const auto bookmarked = QDir(imagesPath()).filePath("page1.png");
        const auto replacement = QDir(imagesPath()).filePath("page3.png");
        QSettings settings(settingsPath(), QSettings::IniFormat);
        settings.setValue("deleteBookmarkAfterOpen", true);
        settings.setValue("bookmarks", QVariantList{QVariantMap{{"path", bookmarked}, {"selectionPath", bookmarked}, {"name", "Pending"}, {"page", 0}}});
        settings.sync();
        ik::MainWindow window; window.show();
        auto *doc = window.findChild<ik::Document *>();
        auto *latest = window.findChild<QAction *>("latestBookmarkAction"); QVERIFY(doc && latest);
        latest->trigger();
        window.openPaths({replacement});
        QTRY_VERIFY_WITH_TIMEOUT(!doc->isBusy() && doc->currentEntry().path == replacement && !doc->currentImage().isNull(), 5000);
        QCOMPARE(bookmarks().size(), 1);
    }

    void missingArchiveMemberKeepsBookmark()
    {
        const auto path = m_temp->filePath("comic.cbz"); QVERIFY(writeComic(path));
        QSettings settings(settingsPath(), QSettings::IniFormat);
        settings.setValue("deleteBookmarkAfterOpen", true);
        settings.setValue("bookmarks", QVariantList{QVariantMap{{"path", path}, {"selectionPath", path}, {"member", "removed.png"}, {"occurrence", 0}, {"name", "Removed"}, {"index", 1}, {"page", 0}}});
        settings.sync();
        ik::MainWindow window; window.show();
        auto *doc = window.findChild<ik::Document *>();
        auto *latest = window.findChild<QAction *>("latestBookmarkAction"); QVERIFY(doc && latest); latest->trigger();
        QTRY_VERIFY_WITH_TIMEOUT(!doc->isBusy() && !doc->currentImage().isNull(), 5000);
        QCOMPARE(bookmarks().size(), 1);
        QVERIFY(doc->currentEntry().member != "removed.png");
    }

    void viewerBookmarkShortcutWorksAcrossFocus_data()
    {
        QTest::addColumn<QString>("target");
        QTest::newRow("native-metal-surface") << QString("native");
        QTest::newRow("canvas-widget") << QString("canvas");
        QTest::newRow("file-list") << QString("list");
        QTest::newRow("toolbar-button") << QString("button");
    }
    void viewerBookmarkShortcutWorksAcrossFocus()
    {
        QFETCH(QString, target);
        QVERIFY(seedImages(imagesPath()));
        ik::MainWindow window; window.show(); window.openPaths({imagesPath()});
        auto *doc = window.findChild<ik::Document *>();
        auto *add = window.findChild<QAction *>("addBookmarkAction"); QVERIFY(doc && add);
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QTRY_VERIFY_WITH_TIMEOUT(!doc->isBusy() && !doc->currentImage().isNull(), 5000);
        window.activateWindow(); QApplication::setActiveWindow(&window);
        QSignalSpy triggered(add, &QAction::triggered);
        if (target == "native") {
            auto *surface = surfaceFor(window); QVERIFY(surface); surface->requestActivate();
            QTest::keyClick(surface, Qt::Key_B);
        } else {
            QWidget *receiver = target == "canvas" ? static_cast<QWidget *>(window.findChild<ik::ImageCanvas *>())
                : target == "list" ? static_cast<QWidget *>(window.findChild<QListView *>("fileList"))
                : static_cast<QWidget *>(window.findChild<QToolButton *>("bookmarksButton"));
            QVERIFY(receiver); receiver->setFocus(); QTest::keyClick(receiver, Qt::Key_B);
        }
        QTRY_COMPARE_WITH_TIMEOUT(triggered.size(), 1, 3000);
        QCOMPARE(bookmarks().size(), 1);
    }

    void filterTypingDoesNotTriggerViewerShortcuts()
    {
        QVERIFY(seedImages(imagesPath()));
        ik::MainWindow window; window.show(); window.openPaths({imagesPath()});
        auto *doc = window.findChild<ik::Document *>();
        auto *add = window.findChild<QAction *>("addBookmarkAction");
        auto *filter = window.findChild<QLineEdit *>("fileFilter"); QVERIFY(doc && add && filter);
        QTRY_VERIFY_WITH_TIMEOUT(!doc->isBusy() && !doc->currentImage().isNull(), 5000);
        window.activateWindow(); QApplication::setActiveWindow(&window); filter->setFocus();
        QSignalSpy triggered(add, &QAction::triggered);
        QTest::keyClicks(filter, "bookmark");
        QCOMPARE(filter->text(), "bookmark"); QCOMPARE(triggered.size(), 0); QCOMPARE(bookmarks().size(), 0);
    }

    void nativeNavigationIgnoresStaleFilteredListFocus()
    {
        QVERIFY(seedImages(imagesPath()));
        ik::MainWindow window; window.show(); window.openPaths({imagesPath()});
        auto *doc = window.findChild<ik::Document *>();
        auto *filter = window.findChild<QLineEdit *>("fileFilter");
        auto *list = window.findChild<QListView *>("fileList"); QVERIFY(doc && filter && list);
        QVERIFY(QTest::qWaitForWindowExposed(&window));
        QTRY_VERIFY_WITH_TIMEOUT(!doc->isBusy() && !doc->currentImage().isNull(), 5000);
        auto *surface = surfaceFor(window); QVERIFY(surface);
        window.activateWindow(); QApplication::setActiveWindow(&window);
        filter->setText("page1");
        QTRY_COMPARE_WITH_TIMEOUT(list->model()->rowCount(), 1, 3000);
        list->setFocus(); QTRY_VERIFY_WITH_TIMEOUT(list->hasFocus(), 3000);
        QCOMPARE(QApplication::focusWidget(), static_cast<QWidget *>(list));
        QCOMPARE(doc->currentIndex(), 0);
        // Deliver to the native image surface without updating QWidget's
        // recorded focus, reproducing Cocoa's stale embedded-window focus.
        // This must advance the document rather than clamp the one-row filter.
        QTest::keyClick(surface, Qt::Key_Right);
        QTRY_VERIFY_WITH_TIMEOUT(!doc->isBusy() && doc->currentIndex() == 1 && !doc->currentImage().isNull(), 5000);
        QCOMPARE(doc->currentEntry().name, "page2.png");
        QCOMPARE(filter->text(), QString());
        QCOMPARE(list->model()->rowCount(), 3);
        QCOMPARE(list->currentIndex().data(Qt::UserRole).toInt(), 1);
    }

    void editedShortcutReplacesOriginalOnNativeSurface()
    {
        QVERIFY(seedImages(imagesPath()));
        ik::MainWindow window; window.show(); window.openPaths({imagesPath()});
        auto *doc = window.findChild<ik::Document *>();
        auto *add = window.findChild<QAction *>("addBookmarkAction");
        auto *manager = window.findChild<ik::ShortcutManager *>(); QVERIFY(doc && add && manager);
        QTRY_VERIFY_WITH_TIMEOUT(!doc->isBusy() && !doc->currentImage().isNull(), 5000);
        QString id;
        for (const auto &command : manager->entries()) if (command.action == add) id = command.id;
        QVERIFY(!id.isEmpty()); auto bindings = manager->bindings(); bindings[id] = {QKeySequence("Y")};
        QString error; QVERIFY2(manager->applyBindings(bindings, &error), qPrintable(error));
        auto *surface = surfaceFor(window); QVERIFY(surface);
        window.activateWindow(); QApplication::setActiveWindow(&window); surface->requestActivate();
        QSignalSpy triggered(add, &QAction::triggered);
        QTest::keyClick(surface, Qt::Key_B); QCOMPARE(triggered.size(), 0);
        QTest::keyClick(surface, Qt::Key_Y); QTRY_COMPARE_WITH_TIMEOUT(triggered.size(), 1, 3000);
        QCOMPARE(bookmarks().size(), 1);
    }

    void initialImageFilterMatchesSettingAndExclusiveAction_data()
    {
        QTest::addColumn<QString>("filterId");
        QTest::addColumn<int>("filterMode");
        QTest::addColumn<QString>("actionName");
        QTest::newRow("default-soften") << QString() << int(ik::ImageFilterMode::Soften) << QString("imageFilterSoftenAction");
        QTest::newRow("none") << QString("none") << int(ik::ImageFilterMode::None) << QString("imageFilterNoneAction");
        QTest::newRow("soften") << QString("soften") << int(ik::ImageFilterMode::Soften) << QString("imageFilterSoftenAction");
        QTest::newRow("sharpen") << QString("sharpen") << int(ik::ImageFilterMode::Sharpen) << QString("imageFilterSharpenAction");
        QTest::newRow("soften-sharpen") << QString("soften-sharpen") << int(ik::ImageFilterMode::SoftenSharpen) << QString("imageFilterSoftenSharpenAction");
    }
    void initialImageFilterMatchesSettingAndExclusiveAction()
    {
        QFETCH(QString, filterId); QFETCH(int, filterMode); QFETCH(QString, actionName);
        QSettings settings(settingsPath(), QSettings::IniFormat);
        if (!filterId.isEmpty()) settings.setValue("imageFilter", filterId);
        settings.sync();
        ik::MainWindow window;
        auto *canvas = window.findChild<ik::ImageCanvas *>(); QVERIFY(canvas);
        QCOMPARE(int(canvas->imageFilter()), filterMode);
        QCOMPARE(window.property("imageFilterMode").toString(), filterId.isEmpty() ? QString("soften") : filterId);
        int checked = 0;
        for (const auto &name : {"imageFilterNoneAction", "imageFilterSoftenAction", "imageFilterSharpenAction", "imageFilterSoftenSharpenAction"}) {
            auto *choice = window.findChild<QAction *>(name); QVERIFY(choice);
            QVERIFY(choice->isCheckable());
            QCOMPARE(choice->isChecked(), choice->objectName() == actionName);
            checked += choice->isChecked();
        }
        QCOMPARE(checked, 1);
    }

    void selectedImageFilterPersistsAcrossImagesResetAndRelaunch_data()
    {
        QTest::addColumn<QString>("filterId");
        QTest::addColumn<int>("filterMode");
        QTest::addColumn<QString>("actionName");
        QTest::newRow("none") << QString("none") << int(ik::ImageFilterMode::None) << QString("imageFilterNoneAction");
        QTest::newRow("soften") << QString("soften") << int(ik::ImageFilterMode::Soften) << QString("imageFilterSoftenAction");
        QTest::newRow("sharpen") << QString("sharpen") << int(ik::ImageFilterMode::Sharpen) << QString("imageFilterSharpenAction");
        QTest::newRow("soften-sharpen") << QString("soften-sharpen") << int(ik::ImageFilterMode::SoftenSharpen) << QString("imageFilterSoftenSharpenAction");
    }
    void selectedImageFilterPersistsAcrossImagesResetAndRelaunch()
    {
        QFETCH(QString, filterId); QFETCH(int, filterMode); QFETCH(QString, actionName);
        QVERIFY(seedImages(imagesPath()));
        QSettings settings(settingsPath(), QSettings::IniFormat);
        settings.setValue("imageFilter", filterId == "none" ? "soften" : "none"); settings.sync();
        {
            ik::MainWindow window; window.show(); window.openPaths({imagesPath()});
            auto *doc = window.findChild<ik::Document *>();
            auto *canvas = window.findChild<ik::ImageCanvas *>();
            auto *choice = window.findChild<QAction *>(actionName); QVERIFY(doc && canvas && choice);
            QTRY_VERIFY_WITH_TIMEOUT(!doc->isBusy() && !doc->currentImage().isNull(), 5000);
            QVERIFY(int(canvas->imageFilter()) != filterMode);
            const QImage original = doc->currentImage();
            choice->trigger();
            QCOMPARE(int(canvas->imageFilter()), filterMode);
            QVERIFY(choice->isChecked());
            QCOMPARE(doc->currentImage(), original);
            settings.sync(); QCOMPARE(settings.value("imageFilter").toString(), filterId);
            doc->goTo(1);
            QTRY_VERIFY_WITH_TIMEOUT(!doc->isBusy() && doc->currentIndex() == 1 && !doc->currentImage().isNull(), 5000);
            QCOMPARE(int(canvas->imageFilter()), filterMode);
            canvas->resetTransform();
            QCOMPARE(int(canvas->imageFilter()), filterMode);
            QVERIFY(choice->isChecked());
        }
        ik::MainWindow restored;
        auto *canvas = restored.findChild<ik::ImageCanvas *>();
        auto *choice = restored.findChild<QAction *>(actionName); QVERIFY(canvas && choice);
        QCOMPARE(int(canvas->imageFilter()), filterMode);
        QCOMPARE(restored.property("imageFilterMode").toString(), filterId);
        QVERIFY(choice->isChecked());
    }

    void preferencesSaveAppliesAndPersistsWhileCancelLeavesState()
    {
        QSettings settings(settingsPath(), QSettings::IniFormat);
        settings.setValue("theme", "dark");
        settings.setValue("imageFilter", "soften");
        settings.setValue("cachePreset", "auto");
        settings.setValue("openContainingFolder", false);
        settings.setValue("deleteBookmarkAfterOpen", false); settings.sync();
        {
            ik::MainWindow window; window.show();
            auto *preferences = window.findChild<QAction *>("preferencesAction");
            auto *add = window.findChild<QAction *>("addBookmarkAction");
            auto *manager = window.findChild<ik::ShortcutManager *>(); QVERIFY(preferences && add && manager);
            auto *canvas = window.findChild<ik::ImageCanvas *>(); QVERIFY(canvas);
            QString id;
            for (const auto &command : manager->entries()) if (command.action == add) id = command.id;
            QVERIFY(!id.isEmpty());
            for (const bool save : {false, true}) {
                bool observed = false;
                ModalWatchdog watchdog(&window);
                QTimer::singleShot(0, &window, [&] {
                    auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
                    if (!dialog) return;
                    auto *theme = dialog->findChild<QComboBox *>("themeMode");
                    auto *imageFilter = dialog->findChild<QComboBox *>("imageFilterMode");
                    auto *cache = dialog->findChild<QComboBox *>("imageCachePreset");
                    auto *folder = dialog->findChild<QCheckBox *>("openContainingFolder");
                    auto *remove = dialog->findChild<QCheckBox *>("deleteBookmarkAfterOpen");
                    auto *binding = dialog->findChild<QKeySequenceEdit *>("shortcutBinding_" + id);
                    observed = dialog->objectName() == "preferencesDialog" && theme && imageFilter && cache && folder && remove && binding
                        && !dialog->findChild<QLineEdit *>("photoFolderPath")
                        && !dialog->findChild<QLineEdit *>("secondaryPhotoFolderPath")
                        && !dialog->findChild<QLineEdit *>("editorPath");
                    if (!observed) { dialog->reject(); return; }
                    theme->setCurrentIndex(theme->findData("light"));
                    imageFilter->setCurrentIndex(imageFilter->findData("soften-sharpen"));
                    cache->setCurrentIndex(cache->findData("low"));
                    folder->setChecked(true); remove->setChecked(true);
                    binding->setKeySequence(QKeySequence("Y"));
                    // The live viewer and registry must keep their old state
                    // until the user explicitly saves this staged dialog.
                    observed &= window.property("darkTheme").toBool() && add->shortcut() == QKeySequence("B")
                        && canvas->imageFilter() == ik::ImageFilterMode::Soften;
                    if (save) dialog->accept(); else dialog->reject();
                });
                preferences->trigger(); QVERIFY(observed);
                settings.sync();
                QCOMPARE(settings.value("theme").toString(), save ? QString("light") : QString("dark"));
                QCOMPARE(settings.value("imageFilter").toString(), save ? QString("soften-sharpen") : QString("soften"));
                QCOMPARE(canvas->imageFilter(), save ? ik::ImageFilterMode::SoftenSharpen : ik::ImageFilterMode::Soften);
                QCOMPARE(settings.value("cachePreset").toString(), save ? QString("low") : QString("auto"));
                QCOMPARE(settings.value("openContainingFolder").toBool(), save);
                QCOMPARE(settings.value("deleteBookmarkAfterOpen").toBool(), save);
                QCOMPARE(window.property("darkTheme").toBool(), !save);
                QCOMPARE(add->shortcut(), QKeySequence(save ? "Y" : "B"));
                if (save) {
                    QCOMPARE(window.property("decodedCacheMB").toInt(), ik::resolvedCacheMB("low", ik::detectSystemProfile()));
                    QVERIFY(QApplication::palette().color(QPalette::Window).lightness() > 180);
                }
            }
        }
        ik::MainWindow restored; restored.show();
        QCOMPARE(restored.property("themeMode").toString(), "light");
        QVERIFY(!restored.property("darkTheme").toBool());
        auto *canvas = restored.findChild<ik::ImageCanvas *>(); QVERIFY(canvas);
        QCOMPARE(canvas->imageFilter(), ik::ImageFilterMode::SoftenSharpen);
        QCOMPARE(restored.property("imageFilterMode").toString(), "soften-sharpen");
        QCOMPARE(restored.property("decodedCacheMB").toInt(), ik::resolvedCacheMB("low", ik::detectSystemProfile()));
        auto *add = restored.findChild<QAction *>("addBookmarkAction"); QVERIFY(add);
        QCOMPARE(add->shortcut(), QKeySequence("Y"));
    }
};

QTEST_MAIN(PreferencesIntegrationTests)
#include "preferences_integration_tests.moc"
