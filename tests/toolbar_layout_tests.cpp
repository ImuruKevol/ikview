#include "apptheme.h"
#include "mainwindow.h"
#include "viewertoolbutton.h"
#include "viewericons.h"
#include "document.h"
#include "exportdialog.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QImage>
#include <QLayout>
#include <QMenu>
#include <QPainter>
#include <QSettings>
#include <QPushButton>
#include <QTemporaryDir>
#include <QToolBar>
#include <QtTest>

class ToolbarLayoutTests : public QObject {
    Q_OBJECT
    QTemporaryDir temporary;
private slots:
    void initTestCase() {
        QCOMPARE(QGuiApplication::platformName(), QString("offscreen"));
        QVERIFY(temporary.isValid());
        qputenv("IKVIEW_SETTINGS_PATH", temporary.filePath("preferences.ini").toUtf8());
    }
    void consistentToolbarAndCompactControls() {
        ik::MainWindow window;
        auto *bar = window.findChild<QToolBar *>("mainToolbar"); QVERIFY(bar);
        QCOMPARE(bar->iconSize(), QSize(24, 24));
        int buttons = 0;
        for (auto *button : bar->findChildren<QToolButton *>()) {
            if (button->objectName() == "qt_toolbar_ext_button") continue;
            QVERIFY(dynamic_cast<ik::ViewerToolButton *>(button));
            QCOMPARE(button->iconSize(), QSize(24, 24));
            QCOMPARE(button->sizeHint().height(), 40);
            QVERIFY(!button->icon().isNull());
            QVERIFY(!button->accessibleName().isEmpty() || !button->text().isEmpty());
            ++buttons;
        }
        QCOMPARE(buttons, 10);
        for (const auto &name : {"hideFileListButton", "previousPdfPageButton", "nextPdfPageButton", "dismissErrorButton", "hideMetadataButton"}) {
            auto *button = window.findChild<QToolButton *>(name); QVERIFY(button);
            QCOMPARE(button->iconSize(), QSize(20, 20));
            QCOMPARE(button->sizeHint(), QSize(32, 32));
        }
        QVERIFY(!window.isVisible());
    }
    void menuIndicatorStaysCenteredAndSeparated_data() {
        QTest::addColumn<QString>("label"); QTest::addColumn<bool>("rtl");
        for (const auto &label : {QString("슬라이드쇼"), QString("슬라이드쇼 · 300초"), QString("Slideshow · 300 seconds")}) {
            QTest::newRow(qPrintable(label + " LTR")) << label << false;
            QTest::newRow(qPrintable(label + " RTL")) << label << true;
        }
    }
    void menuIndicatorStaysCenteredAndSeparated() {
        QFETCH(QString, label); QFETCH(bool, rtl);
        ik::ViewerToolButton button; QMenu menu;
        button.setIcon(ik::viewerIcon("play", Qt::black)); button.setText(label); button.setMenu(&menu);
        button.setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
        button.setPopupMode(QToolButton::InstantPopup);
        button.setLayoutDirection(rtl ? Qt::RightToLeft : Qt::LeftToRight);
        button.resize(button.sizeHint());
        const auto areas = button.contentRects();
        QCOMPARE(areas.icon.size(), QSize(24, 24));
        QCOMPARE(areas.indicator.size(), QSize(10, 10));
        QVERIFY(qAbs(areas.icon.center().y() - areas.text.center().y()) <= 1);
        QVERIFY(qAbs(areas.indicator.center().y() - areas.text.center().y()) <= 1);
        QVERIFY(!areas.icon.intersects(areas.text)); QVERIFY(!areas.indicator.intersects(areas.text));
        QVERIFY(areas.text.width() >= button.fontMetrics().horizontalAdvance(label));
        QVERIFY(button.rect().contains(areas.icon)); QVERIFY(button.rect().contains(areas.indicator));
        QCOMPARE(button.popupMode(), QToolButton::InstantPopup);
        QVERIFY(!button.isVisible());
    }
    void actionsStillActivateThroughIconOnlyButtons() {
        ik::ViewerToolButton button; QAction action("이미지 목록 표시 / 숨기기", &button);
        action.setCheckable(true); button.setDefaultAction(&action); button.setToolButtonStyle(Qt::ToolButtonIconOnly);
        QSignalSpy spy(&action, &QAction::triggered);
        button.click(); QCOMPARE(spy.size(), 1); QVERIFY(action.isChecked()); QVERIFY(button.isChecked());
        action.setEnabled(false); button.click(); QCOMPARE(spy.size(), 1); QVERIFY(!button.isEnabled());
    }
    void overflowRetainsCommandsAndSubmenus() {
        ik::MainWindow window;
        auto *bar = window.findChild<QToolBar *>("mainToolbar"); QVERIFY(bar);
        QMenu overflow; int commands = 0, submenus = 0;
        for (auto *action : bar->actions()) {
            auto *proxy = dynamic_cast<ik::ViewerToolbarAction *>(action);
            if (!proxy) continue;
            // A null widget request makes QMenu draw a standard menu command.
            QVERIFY(!proxy->requestWidget(&overflow)); overflow.addAction(proxy);
            QVERIFY(!proxy->text().isEmpty()); QVERIFY(proxy->shortcuts().isEmpty());
            if (proxy->menu()) { ++submenus; QVERIFY(!proxy->menu()->actions().isEmpty()); }
            ++commands;
        }
        QCOMPARE(commands, 10); QCOMPARE(submenus, 1);
        auto *source = window.findChild<QAction *>("toggleFileListAction"); QVERIFY(source);
        QSignalSpy spy(source, &QAction::triggered);
        for (auto *proxy : overflow.actions()) if (proxy->text() == source->text()) {
            const bool before = source->isChecked(); proxy->trigger();
            QCOMPARE(source->isChecked(), !before); QCOMPARE(proxy->isChecked(), source->isChecked());
        }
        QCOMPARE(spy.size(), 1);
        for (const int width : {480, 700, 1080}) {
            window.resize(width, 820); window.layout()->setGeometry(window.rect()); window.layout()->activate();
            QVERIFY(bar->width() >= width);
        }
        QVERIFY(!window.isVisible()); QVERIFY(!overflow.isVisible());
    }
    void bookmarkLabelSurvivesActionChanges() {
        ik::MainWindow window;
        auto *action = window.findChild<QAction *>("manageBookmarksAction"); QVERIFY(action);
        auto *button = window.findChild<QToolButton *>("bookmarksButton"); QVERIFY(button);
        action->setIcon(ik::viewerIcon("bookmark", Qt::red));
        action->setShortcut(QKeySequence("Ctrl+Alt+B"));
        QCOMPARE(button->text(), QString("책갈피"));
        QCOMPARE(action->text(), QString("책갈피 목록…"));
    }
    void shortcutAndIntervalUseSameSlideshowState() {
        QStringList paths;
        for (int i = 0; i < 2; ++i) {
            const auto path = temporary.filePath(QString("%1.png").arg(i));
            QImage image(32, 24, QImage::Format_RGB32); image.fill(i ? Qt::red : Qt::blue); QVERIFY(image.save(path)); paths.append(path);
        }
        ik::MainWindow window; window.openPaths(paths);
        auto *command = window.findChild<QAction *>("slideshowAction"); QVERIFY(command);
        auto *menuCommand = window.findChild<QAction *>("slideshowMenuButtonAction"); QVERIFY(menuCommand);
        auto *button = window.findChild<QToolButton *>("slideshowButton"); QVERIFY(button);
        QTRY_VERIFY_WITH_TIMEOUT(command->isEnabled(), 5000);
        command->trigger(); QVERIFY(command->isChecked()); QVERIFY(button->text().contains("초"));
        QCOMPARE(menuCommand->property("ikviewSymbol").toString(), QString("pause"));
        window.findChild<QAction *>("stopSlideshowFromButtonAction")->trigger();
        QCOMPARE(button->text(), QString("슬라이드쇼")); QCOMPARE(menuCommand->property("ikviewSymbol").toString(), QString("play"));
        window.findChild<QAction *>("slideshowInterval5Action")->trigger(); QVERIFY(command->isChecked()); QVERIFY(button->text().contains("5초"));
        window.findChild<QAction *>("stopSlideshowFromButtonAction")->trigger();
        QVERIFY(!window.isVisible());
    }
    void pdfOrderButtonsUseThemeAwareVectorArrows() {
        ik::ExportSource source; source.snapshot = QImage(20, 20, QImage::Format_RGB32); source.snapshot.fill(Qt::blue);
        ik::ExportDialog dialog({source, source});
        auto *up = dialog.findChild<QPushButton *>("exportMoveUp"); auto *down = dialog.findChild<QPushButton *>("exportMoveDown");
        QVERIFY(up); QVERIFY(down); QVERIFY(!up->icon().isNull()); QVERIFY(!down->icon().isNull());
        QCOMPARE(up->text(), QString("위로")); QCOMPARE(down->text(), QString("아래로"));
        QCOMPARE(up->iconSize(), QSize(20, 20)); QCOMPARE(down->iconSize(), QSize(20, 20));
        const auto previous = up->icon().cacheKey(); ik::applyApplicationTheme("light");
        QCoreApplication::sendPostedEvents(); QVERIFY(up->icon().cacheKey() != previous);
        QVERIFY(!dialog.isVisible());
    }
    void renderToolbarWithoutOpeningAnyWindow() {
        const QString output = qEnvironmentVariable("IKVIEW_TOOLBAR_PREVIEW_DIR");
        if (!output.isEmpty()) QVERIFY(QDir().mkpath(output));
        for (const auto &theme : {QString("dark"), QString("light")}) {
            { QSettings settings(qEnvironmentVariable("IKVIEW_SETTINGS_PATH"), QSettings::IniFormat); settings.setValue("theme", theme); settings.sync(); }
            ik::MainWindow window;
            QCOMPARE(window.property("darkTheme").toBool(), theme == "dark");
            auto *bar = window.findChild<QToolBar *>("mainToolbar"); QVERIFY(bar);
            // Exercise enabled and checked icon states without loading a file.
            for (auto *button : bar->findChildren<QToolButton *>()) button->setEnabled(true);
            window.ensurePolished(); window.resize(1080, 820); window.layout()->setGeometry(window.rect()); window.layout()->activate();
            bar->ensurePolished(); bar->layout()->activate();
            QCOMPARE(bar->width(), 1080);
            for (auto *button : bar->findChildren<QToolButton *>()) {
                auto *viewer = dynamic_cast<ik::ViewerToolButton *>(button);
                if (!viewer || button->toolButtonStyle() != Qt::ToolButtonTextBesideIcon) continue;
                QCOMPARE(button->fontMetrics().elidedText(button->text(), Qt::ElideRight, viewer->contentRects().text.width()), button->text());
            }
            QImage image(bar->size() * 2, QImage::Format_ARGB32_Premultiplied);
            image.setDevicePixelRatio(2); image.fill(Qt::transparent);
            QPainter painter(&image); bar->render(&painter); painter.end();
            QVERIFY(!image.isNull());
            QCOMPARE(image.width(), bar->width() * 2);
            QVERIFY(image.pixelColor(image.width() - 4, image.height() / 2).alpha() > 0);
            if (!output.isEmpty()) QVERIFY(image.save(QDir(output).filePath("toolbar-" + theme + ".png")));
            for (auto *widget : QApplication::topLevelWidgets()) QVERIFY(!widget->isVisible());
        }
    }
};

int main(int argc, char **argv) {
    // Set before QApplication: this binary must never create a desktop window.
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication application(argc, argv);
    ToolbarLayoutTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "toolbar_layout_tests.moc"
