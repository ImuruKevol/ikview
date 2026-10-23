#include "document.h"
#include "exportdialog.h"
#include "imagecanvas.h"
#include "mainwindow.h"
#include "preferencesdialog.h"
#include <QAction>
#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QCheckBox>
#include <QContextMenuEvent>
#include <QDir>
#include <QFileSystemWatcher>
#include <QItemSelectionModel>
#include <QLabel>
#include <QListView>
#include <QMenu>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QtTest>
#include <memory>

class WorkflowTests : public QObject {
    Q_OBJECT
    std::unique_ptr<QTemporaryDir> temp;
    QString directory;
    bool seed() {
        directory=temp->filePath("images"); if (!QDir().mkpath(directory)) return false;
        for(int i=0;i<4;++i) { QImage image(120,80,QImage::Format_RGB32); image.fill(QColor::fromHsv(i*80,230,220)); if(!image.save(QDir(directory).filePath(QString("%1.png").arg(i+1)))) return false; }
        return true;
    }
    bool open(ik::MainWindow &window, const QString &path={}) {
        window.show(); window.activateWindow(); window.openPaths({path.isEmpty()?directory:path});
        auto *doc=window.findChild<ik::Document *>();
        return QTest::qWaitFor([doc]{return doc && !doc->isBusy() && !doc->currentImage().isNull();},5000);
    }
    QListView *list(ik::MainWindow &window) {return window.findChild<QListView *>("fileList");}
    void choose(QListView *view, int row, Qt::KeyboardModifiers mods={}) {
        view->scrollTo(view->model()->index(row,0));
        QTest::mouseClick(view->viewport(),Qt::LeftButton,mods,view->visualRect(view->model()->index(row,0)).center());
    }
    QString pdf() {
        const auto path=temp->filePath("pages.pdf");
        QPdfWriter writer(path); writer.setResolution(72); writer.setPageSize(QPageSize(QSizeF(120,80),QPageSize::Point)); writer.setPageMargins(QMarginsF(0,0,0,0));
        QPainter painter(&writer); painter.fillRect(QRect(0,0,120,80),Qt::red); writer.newPage(); painter.fillRect(QRect(0,0,120,80),Qt::blue); painter.end(); return path;
    }
private slots:
    void init() {temp=std::make_unique<QTemporaryDir>(); QVERIFY(temp->isValid()); qputenv("IKVIEW_SETTINGS_PATH",temp->filePath("preferences.ini").toUtf8()); QVERIFY(seed());}
    void cleanup() {if(auto *menu=qobject_cast<QMenu *>(QApplication::activePopupWidget())) menu->close(); QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete); qunsetenv("IKVIEW_SETTINGS_PATH"); temp.reset();}
    void toolbarContainsOnlyRequestedControls() {
        ik::MainWindow window; QVERIFY(open(window)); auto *bar=window.findChild<QToolBar *>("mainToolbar"); QVERIFY(bar);
        for(const auto &id:{"openPathsAction","previousImageAction","nextImageAction"}) QVERIFY(!bar->actions().contains(window.findChild<QAction *>(id)));
        auto *toggle=window.findChild<QToolButton *>("fileListToggleButton"); QVERIFY(toggle); QCOMPARE(toggle->toolButtonStyle(),Qt::ToolButtonIconOnly);
        QCOMPARE(window.findChild<QAction *>("openPathsAction")->shortcut(),QKeySequence("Ctrl+O"));
        for(auto *action:window.findChildren<QAction *>()) QVERIFY(action->shortcut()!=QKeySequence("F"));
    }
    void intervalChoiceStartsImmediatelyAndPersists() {
        ik::MainWindow window; QVERIFY(open(window)); auto *button=window.findChild<QToolButton *>("slideshowButton"); QVERIFY(button); QCOMPARE(button->popupMode(),QToolButton::InstantPopup);
        window.findChild<QAction *>("slideshowInterval5Action")->trigger(); QVERIFY(window.findChild<QAction *>("slideshowAction")->isChecked());
        QSettings settings(qEnvironmentVariable("IKVIEW_SETTINGS_PATH"),QSettings::IniFormat); QCOMPARE(settings.value("slideshowSeconds").toInt(),5);
        QVERIFY(button->text().contains("5")); window.findChild<QAction *>("stopSlideshowFromButtonAction")->trigger(); QVERIFY(!window.findChild<QAction *>("slideshowAction")->isChecked());
        ik::PreferencesDialog preferences({},ik::detectSystemProfile(),new QWidget); QVERIFY(!preferences.findChild<QWidget *>("slideshowInterval"));
    }
    void imageContextOffersSharedFilterSubmenu() {
        ik::MainWindow window; QVERIFY(open(window)); auto *canvas=window.findChild<ik::ImageCanvas *>(); bool found=false;
        QTimer::singleShot(0,&window,[&]{auto *menu=qobject_cast<QMenu *>(QApplication::activePopupWidget()); if(!menu)return; for(auto *action:menu->actions()) if(action->menu() && action->menu()->objectName()=="imageFilterMenu") {found=action->menu()->actions().size()==4; } menu->close();});
        QMetaObject::invokeMethod(canvas,"customContextMenuRequested",Qt::DirectConnection,Q_ARG(QPoint,QPoint(40,40))); QVERIFY(found);
    }
    void commandClickKeepsMultipleSelectionWhileNavigating() {
        ik::MainWindow window; QVERIFY(open(window)); auto *view=list(window); choose(view,0); choose(view,2,Qt::ControlModifier);
        QTRY_COMPARE(view->selectionModel()->selectedRows().size(),2); auto *doc=window.findChild<ik::Document *>(); QTRY_COMPARE(doc->currentIndex(),2); QTRY_VERIFY(!doc->isBusy()); QCOMPARE(view->selectionModel()->selectedRows().size(),2);
        choose(view,2,Qt::ControlModifier); QTRY_COMPARE(view->selectionModel()->selectedRows().size(),1); QTRY_COMPARE(doc->currentIndex(),0); QCOMPARE(view->currentIndex().row(),0);
    }
    void shiftClickSelectsRange() {
        ik::MainWindow window; QVERIFY(open(window)); auto *view=list(window); choose(view,0); choose(view,3,Qt::ShiftModifier); QTRY_COMPARE(view->selectionModel()->selectedRows().size(),4);
        QTRY_VERIFY(!window.findChild<ik::Document *>()->isBusy()); QCOMPARE(view->selectionModel()->selectedRows().size(),4);
    }
    void keyboardRangeAndSelectAllRemainListCommands() {
        ik::MainWindow window; QVERIFY(open(window)); auto *view=list(window); choose(view,0); view->setFocus(); QTest::keyClick(view,Qt::Key_Down,Qt::ShiftModifier); QTRY_COMPARE(view->selectionModel()->selectedRows().size(),2);
        QTest::keyClick(view,Qt::Key_A,Qt::ControlModifier); QTRY_COMPARE(view->selectionModel()->selectedRows().size(),4);
    }
    void selectedFilesSurviveCollectionRefresh() {
        ik::MainWindow window; QVERIFY(open(window)); auto *view=list(window); choose(view,0); choose(view,2,Qt::ControlModifier); QTRY_COMPARE(view->selectionModel()->selectedRows().size(),2);
        auto *doc=window.findChild<ik::Document *>(); QTRY_VERIFY(!doc->isBusy());
        QImage extra(30,20,QImage::Format_RGB32); extra.fill(Qt::green); QVERIFY(extra.save(QDir(directory).filePath("5.png"))); doc->refreshCollection(); QTRY_COMPARE(doc->count(),5); QTRY_COMPARE(view->selectionModel()->selectedRows().size(),2);
    }
    void commandTOpensSelectionMenuWithBatchActions() {
        ik::MainWindow window; QVERIFY(open(window)); auto *view=list(window); view->selectAll(); view->setFocus();
        QTest::keyClick(view,Qt::Key_T,Qt::ControlModifier); auto *menu=qobject_cast<QMenu *>(QApplication::activePopupWidget()); QVERIFY(menu); QCOMPARE(menu->objectName(),QString("selectionContextMenu"));
        auto *convert=menu->findChild<QAction *>("convertSelectionAction"); QVERIFY(convert); QVERIFY(convert->text().contains("PDF")); QVERIFY(!menu->actions().contains(window.findChild<QAction *>("addBookmarkAction")));
        auto *copy=menu->findChild<QAction *>("copySelectedPathsAction"); QVERIFY(copy); copy->trigger(); const auto paths=QApplication::clipboard()->text().split('\n'); QCOMPARE(paths.size(),4); QVERIFY(paths[0].endsWith("1.png")); QVERIFY(paths[3].endsWith("4.png")); menu->close();
    }
    void rightClickPreservesSelectedSet() {
        ik::MainWindow window; QVERIFY(open(window)); auto *view=list(window); choose(view,0); choose(view,2,Qt::ControlModifier); QTRY_COMPARE(view->selectionModel()->selectedRows().size(),2);
        const auto point=view->visualRect(view->model()->index(0,0)).center(); QTest::mouseClick(view->viewport(),Qt::RightButton,{},point);
        QContextMenuEvent event(QContextMenuEvent::Mouse,point,view->viewport()->mapToGlobal(point)); QApplication::sendEvent(view->viewport(),&event);
        auto *menu=qobject_cast<QMenu *>(QApplication::activePopupWidget()); QVERIFY(menu); QCOMPARE(view->selectionModel()->selectedRows().size(),2); menu->close();
    }
    void selectionConversionReceivesOrderedSources() {
        ik::MainWindow window; QVERIFY(open(window)); auto *view=list(window); choose(view,3); choose(view,1,Qt::ControlModifier); QTRY_COMPARE(view->selectionModel()->selectedRows().size(),2); QTRY_VERIFY(!window.findChild<ik::Document *>()->isBusy());
        window.findChild<QAction *>("exportImageAction")->trigger(); auto *menu=qobject_cast<QMenu *>(QApplication::activePopupWidget()); QVERIFY(menu); auto *convert=menu->findChild<QAction *>("convertSelectionAction"); QVERIFY(convert); menu->close(); bool observed=false;
        QTimer::singleShot(0,&window,[&]{auto *dialog=qobject_cast<ik::ExportDialog *>(QApplication::activeModalWidget()); if(!dialog)return; const auto sources=dialog->orderedSources(); observed=sources.size()==2 && sources[0].entry.name=="2.png" && sources[1].entry.name=="4.png"; dialog->reject();});
        convert->trigger(); QVERIFY(observed);
    }
    void pdfPagesUseArrowsAndFooterNumber() {
        ik::MainWindow window; QVERIFY(open(window,pdf())); auto *doc=window.findChild<ik::Document *>(); QVERIFY(doc->currentInfo().pdf); QCOMPARE(doc->count(),1); QCOMPARE(doc->currentInfo().pageCount,2);
        auto *controls=window.findChild<QWidget *>("pdfPageControls"); QVERIFY(controls->isVisible()); auto *spin=window.findChild<QSpinBox *>("pdfPageNumber"); QCOMPARE(spin->maximum(),2);
        window.findChild<QAction *>("nextImageAction")->trigger(); QTRY_VERIFY(!doc->isBusy() && doc->currentInfo().page==1); QCOMPARE(spin->value(),2); QCOMPARE(doc->currentImage().pixelColor(80,60),QColor(Qt::blue));
        spin->setValue(1); QTRY_VERIFY(!doc->isBusy() && doc->currentInfo().page==0); QCOMPARE(doc->currentImage().pixelColor(80,60),QColor(Qt::red));
    }
    void whiteRemovalActionUsesOpenImageDespiteMultipleSelection() {
        ik::MainWindow window; QVERIFY(open(window)); auto *view = list(window); view->selectAll();
        auto *doc = window.findChild<ik::Document *>(); const auto pixels = doc->currentImage().cacheKey(); const auto entry = doc->currentEntry();
        bool observed = false;
        QTimer::singleShot(0, &window, [&] {
            auto *dialog = qobject_cast<ik::ExportDialog *>(QApplication::activeModalWidget()); if (!dialog) return;
            const auto sources = dialog->orderedSources(); auto *remove = dialog->findChild<QCheckBox *>("exportRemoveWhiteBackground");
            observed = sources.size() == 1 && sources.first().entry.key() == entry.key() && remove && remove->isChecked(); dialog->reject();
        });
        auto *action = window.findChild<QAction *>("removeWhiteBackgroundAction"); QVERIFY(action && action->isEnabled()); action->trigger(); QVERIFY(observed); QCOMPARE(doc->currentImage().cacheKey(), pixels);
        auto *canvas = window.findChild<ik::ImageCanvas *>(); bool contextFound = false;
        QTimer::singleShot(0, &window, [&] { auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget()); if (menu) { contextFound = menu->actions().contains(action); menu->close(); } });
        QMetaObject::invokeMethod(canvas, "customContextMenuRequested", Qt::DirectConnection, Q_ARG(QPoint, QPoint(40, 40))); QVERIFY(contextFound);
    }
    void pdfFooterIsOneRowAndRestoresImageControls() {
        ik::MainWindow window; QVERIFY(open(window, pdf())); window.resize(840, 620); QCoreApplication::processEvents();
        auto *footer = window.findChild<QWidget *>("imageFooter"); auto *controls = window.findChild<QWidget *>("pdfPageControls"); auto *name = window.findChild<QLabel *>("imageFilename");
        auto *position = window.findChild<QLabel *>("imagePosition"); auto *jump = window.findChild<QPushButton *>("goToImageButton");
        QVERIFY(footer && controls && name && position && jump); QVERIFY(footer->height() <= 52); QVERIFY(position->isHidden()); QVERIFY(jump->isHidden());
        QCOMPARE(name->mapTo(footer, name->rect().center()).y(), controls->mapTo(footer, controls->rect().center()).y());
        QVERIFY(footer->rect().contains(QRect(controls->pos(), controls->size()))); QVERIFY(!name->geometry().intersects(controls->geometry()));
        auto *details = window.findChild<QLabel *>("statusDetailsLabel"); QVERIFY(details); QVERIFY(!details->text().contains("페이지"));
        const auto pdfHeight = footer->height(); window.openPaths({directory}); auto *doc = window.findChild<ik::Document *>(); QTRY_VERIFY(!doc->isBusy() && !doc->currentInfo().pdf); QCoreApplication::processEvents();
        QVERIFY(controls->isHidden()); QVERIFY(!position->isHidden()); QVERIFY(!jump->isHidden()); QVERIFY(qAbs(footer->height() - pdfHeight) <= 4);
    }
    void pdfHomeEndAndSpreadDoNotSkipNextFile() {
        const auto path=pdf();
        const auto nextFile=temp->filePath("z-next.png"); QVERIFY(QFile::copy(QDir(directory).filePath("1.png"),nextFile));
        ik::MainWindow window; window.show(); window.openPaths({path,nextFile}); auto *doc=window.findChild<ik::Document *>();
        QTRY_VERIFY(!doc->isBusy() && doc->currentInfo().pdf);
        for(auto *action:window.findChildren<QAction *>()) if(action->text()==QString::fromUtf8("두 장 보기")) action->trigger();
        for(auto *action:window.findChildren<QAction *>()) if(action->text()==QString::fromUtf8("마지막 이미지")) action->trigger();
        QTRY_VERIFY(!doc->isBusy() && doc->currentInfo().page==1);
        auto *next=window.findChild<QAction *>("nextImageAction"); QVERIFY(next->isEnabled()); next->trigger();
        QTRY_VERIFY(!doc->isBusy() && !doc->currentInfo().pdf); QCOMPARE(doc->currentEntry().name,QString("z-next.png"));
    }
    void pdfSlideshowAdvancesPagesAndManualNavigationStops() {
        ik::MainWindow window; QVERIFY(open(window,pdf())); auto *doc=window.findChild<ik::Document *>(); window.findChild<QAction *>("slideshowInterval1Action")->trigger();
        QVERIFY(window.findChild<QAction *>("slideshowAction")->isChecked()); QTRY_VERIFY_WITH_TIMEOUT(!doc->isBusy() && doc->currentInfo().page==1,2400);
        window.findChild<QAction *>("previousImageAction")->trigger(); QTRY_VERIFY(!doc->isBusy() && doc->currentInfo().page==0); QVERIFY(!window.findChild<QAction *>("slideshowAction")->isChecked());
    }
    void pdfWheelAtBoundaryDoesNotWrapToFirstPage() {
        ik::MainWindow window; QVERIFY(open(window,pdf())); auto *doc=window.findChild<ik::Document *>();
        doc->setPage(1); QTRY_VERIFY(!doc->isBusy() && doc->currentInfo().page==1);
        const auto pixels=doc->currentImage().cacheKey(); auto *canvas=window.findChild<ik::ImageCanvas *>();
        QMetaObject::invokeMethod(canvas,"navigateRequested",Qt::DirectConnection,Q_ARG(int,1));
        QVERIFY(!doc->isBusy()); QCOMPARE(doc->currentInfo().page,1); QCOMPARE(doc->currentImage().cacheKey(),pixels);
        QTest::qWait(60); QCOMPARE(doc->currentInfo().page,1);
    }
};
int main(int argc,char **argv) {
#ifdef Q_OS_MACOS
    qputenv("QSG_RHI_BACKEND","metal");
#endif
    QApplication app(argc,argv); WorkflowTests tests; return QTest::qExec(&tests,argc,argv);
}
#include "workflow_tests.moc"
