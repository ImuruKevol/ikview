#include "imagecanvas.h"
#include "mainwindow.h"
#include "document.h"
#include "fileoperations.h"
#include <QApplication>
#include <QAction>
#include <QCheckBox>
#include <QDialog>
#include <QDir>
#include <QLineEdit>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QSpinBox>
#include <QTimer>
#include <QTemporaryDir>
#include <QtTest>

class ViewerTests : public QObject {
    Q_OBJECT
private slots:
    void nativeTextureRendering() {
        ik::ImageCanvas canvas;
        canvas.resize(480,320);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QImage image(160,100,QImage::Format_RGBA8888);
        image.fill(QColor("#ef362f"));
        canvas.setImage(image);
        canvas.setFitMode(ik::FitMode::Actual);
        QTest::qWait(300);
        const QImage frame=canvas.viewportImage();
        QVERIFY(!frame.isNull());
        QColor center=frame.pixelColor(frame.width()/2,frame.height()/2);
        QVERIFY2(center.red()>200 && center.green()<100, qPrintable(center.name()));
#ifdef Q_OS_MACOS
        QVERIFY2(canvas.rendererDescription().contains("Metal",Qt::CaseInsensitive),qPrintable(canvas.rendererDescription()));
#endif
    }
    void viewTransformsPreserveSource() {
        ik::ImageCanvas canvas;
        QImage source(3,2,QImage::Format_RGBA8888);
        source.fill(Qt::black);
        source.setPixelColor(0,0,Qt::red);
        source.setPixelColor(2,1,Qt::blue);
        canvas.setImage(source);
        canvas.flipHorizontal();
        QImage mirrored=canvas.transformedImage();
        QCOMPARE(mirrored.pixelColor(2,0),QColor(Qt::red));
        QCOMPARE(source.pixelColor(0,0),QColor(Qt::red));
        canvas.resetTransform();
        canvas.rotate(1);
        QCOMPARE(canvas.transformedImage().size(),QSize(2,3));
        canvas.resetTransform();
        QCOMPARE(canvas.transformedImage(),source);
    }
    void zoomAndSpread() {
        ik::ImageCanvas canvas;
        canvas.resize(480,320);canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QImage left(100,100,QImage::Format_RGBA8888);left.fill(Qt::red);
        QImage right(100,100,QImage::Format_RGBA8888);right.fill(Qt::blue);
        canvas.setImage(left);canvas.setSecondaryImage(right);canvas.setSpread(true);
        canvas.setFitMode(ik::FitMode::Actual);
        QTest::qWait(250);
        QImage frame=canvas.viewportImage();
        QVERIFY(!frame.isNull());
        // Actual size means one source pixel per physical framebuffer pixel.
        auto first=frame.pixelColor(frame.width()/2-50,frame.height()/2);
        auto second=frame.pixelColor(frame.width()/2+50,frame.height()/2);
        QVERIFY2(first.red()>200 && second.blue()>200,"Spread must show distinct left and right image textures");
        canvas.setZoom(2.0);QVERIFY(qAbs(canvas.zoom()-2.0)<0.001);
        canvas.zoomBy(1.5);QVERIFY(qAbs(canvas.zoom()-3.0)<0.001);
    }
    void folderNavigationIntegration() {
        QTemporaryDir temp;QVERIFY(temp.isValid());
        QImage first(180,120,QImage::Format_RGB32);first.fill(Qt::green);
        QImage second(140,100,QImage::Format_RGB32);second.fill(Qt::blue);
        QVERIFY(first.save(temp.filePath("page2.png")));
        QVERIFY(second.save(temp.filePath("page10.png")));
        ik::MainWindow window;window.show();
        window.openPaths({temp.path()});
        auto *document=window.findChild<ik::Document*>();
        QVERIFY(document);
        QTRY_COMPARE_WITH_TIMEOUT(document->count(),2,5000);
        QTRY_VERIFY_WITH_TIMEOUT(!document->currentImage().isNull(),5000);
        QCOMPARE(document->currentEntry().name,QString("page2.png"));
        QQuickWindow *nativeWindow=nullptr;
        for(auto *candidate:QGuiApplication::allWindows())
            if(auto *quick=qobject_cast<QQuickWindow*>(candidate);quick && quick->isVisible()) nativeWindow=quick;
        QVERIFY(nativeWindow);
        window.activateWindow();
        QTest::mouseClick(nativeWindow,Qt::LeftButton,Qt::NoModifier,QPoint(nativeWindow->width()/2,nativeWindow->height()/2));
        QTest::keyClick(nativeWindow,Qt::Key_Right);
        QTRY_COMPARE_WITH_TIMEOUT(document->currentIndex(),1,3000);
        QTRY_VERIFY_WITH_TIMEOUT(!document->isBusy(),5000);
        QCOMPARE(document->currentEntry().name,QString("page10.png"));
        QCOMPARE(document->currentImage().size(),QSize(140,100));
        auto *canvas=window.findChild<ik::ImageCanvas*>();QVERIFY(canvas);
        QTRY_COMPARE_WITH_TIMEOUT(canvas->transformedImage().size(),QSize(140,100),5000);
        document->setSortOrder(ik::SortOrder::NameDescending);
        QTRY_COMPARE_WITH_TIMEOUT(document->currentIndex(),0,5000);
        QTRY_COMPARE_WITH_TIMEOUT(canvas->transformedImage().size(),QSize(140,100),5000);
    }
    void photoFolderAliasCannotMoveOntoItself() {
        QTemporaryDir temp;QVERIFY(temp.isValid());
        QVERIFY(QDir().mkdir(temp.filePath("photos")));
        QFile image(temp.filePath("photos/source.png"));QVERIFY(image.open(QIODevice::WriteOnly));image.write("original");image.close();
        QVERIFY(QFile::link(temp.filePath("photos"),temp.filePath("alias")));
        QVERIFY(ik::samePhysicalFile(image.fileName(),temp.filePath("alias/source.png")));
        QVERIFY(!ik::samePhysicalFile(image.fileName(),temp.filePath("photos/different.png")));
    }
    void exportWritesRequestedSizeAndPadding() {
        QTemporaryDir temp;QVERIFY(temp.isValid());
        const QString originalPath=temp.filePath("source.png"), outputPath=temp.filePath("out.png");
        QImage original(120,80,QImage::Format_RGB32);original.fill(Qt::red);QVERIFY(original.save(originalPath));
        ik::MainWindow window;window.show();window.openPaths({originalPath});
        auto *document=window.findChild<ik::Document*>();QVERIFY(document);
        QTRY_VERIFY_WITH_TIMEOUT(!document->isBusy() && !document->currentImage().isNull(),5000);
        auto *canvas=window.findChild<ik::ImageCanvas*>();QVERIFY(canvas);canvas->rotate(1);
        QAction *exportAction=nullptr;
        for(auto *candidate:window.findChildren<QAction*>())
            if(candidate->text().startsWith(QStringLiteral("이미지 변환 / 저장"))) exportAction=candidate;
        QVERIFY(exportAction);QVERIFY(exportAction->isEnabled());
        bool configured=false;
        QTimer timeout;
        timeout.setSingleShot(true);
        connect(&timeout,&QTimer::timeout,&window,[]{if(auto *dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget())) dialog->reject();});
        timeout.start(3000);
        QTimer::singleShot(100,&window,[&]{
            auto *dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget());if(!dialog)return;
            auto *path=dialog->findChild<QLineEdit*>();
            const auto sizes=dialog->findChildren<QSpinBox*>();
            if(!path || sizes.size()!=4){dialog->reject();return;}
            configured=sizes[0]->value()==80 && sizes[1]->value()==120;
            for(auto *box:dialog->findChildren<QCheckBox*>()) if(box->text().contains(QStringLiteral("종횡비"))) box->setChecked(false);
            path->setText(outputPath);sizes[0]->setValue(60);sizes[1]->setValue(40);sizes[3]->setValue(4);
            dialog->accept();
        });
        exportAction->trigger();timeout.stop();QVERIFY(configured);
        QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(outputPath),5000);
        QImage output(outputPath);QCOMPARE(output.size(),QSize(68,48));
        QCOMPARE(output.pixelColor(0,0),QColor(Qt::white));
        QCOMPARE(output.pixelColor(20,20),QColor(Qt::red));
        QCOMPARE(QImage(originalPath),original);
    }
};
int main(int argc,char **argv) {
#ifdef Q_OS_MACOS
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Metal);
#endif
    QApplication app(argc,argv);
    QCoreApplication::setOrganizationName("ikview-tests");
    QCoreApplication::setApplicationName("canvas-tests");
    QStandardPaths::setTestModeEnabled(true);
    QTemporaryDir settingsDirectory;
    qputenv("IKVIEW_SETTINGS_PATH",settingsDirectory.filePath("canvas-test-settings.ini").toUtf8());
    ViewerTests tests;return QTest::qExec(&tests,argc,argv);
}
#include "canvas_tests.moc"
