#include "apptheme.h"
#include "document.h"
#include "exportdialog.h"
#include "imagecanvas.h"
#include "mainwindow.h"
#include "preferencesdialog.h"
#include "shortcutmanager.h"
#include "viewerplaceholder.h"
#include "dialoglayout.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFormLayout>
#include <QInputDialog>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLayout>
#include <QLineEdit>
#include <QListView>
#include <QListWidget>
#include <QKeySequenceEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPdfWriter>
#include <QPageSize>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSettings>
#include <QSpinBox>
#include <QStyleFactory>
#include <QStyleOptionComboBox>
#include <QStatusBar>
#include <QTableWidget>
#include <QTabWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QToolButton>
#include <QtTest>
#include <cmath>

class DesignTests final : public QObject {
    Q_OBJECT
    QTemporaryDir temporary;
    QString output;
    QJsonArray captures;
    void layout(QWidget *widget) {
        widget->ensurePolished();
        if (widget->layout()) { widget->layout()->setGeometry(widget->contentsRect()); widget->layout()->activate(); }
        for (auto *child : widget->findChildren<QWidget *>(QString(), Qt::FindDirectChildrenOnly)) {
            if (!child->isWindow()) layout(child);
        }
    }
    bool capture(QWidget *widget, const QString &name) {
        // show() here targets Qt's virtual offscreen platform only. Settling a
        // virtual surface exercises real scroll-area and native-child layout
        // without creating, focusing or raising a macOS window.
        Q_ASSERT(QGuiApplication::platformName() == "offscreen");
        const bool wasVisible = widget->isVisible();
        const bool virtualSurface = !qobject_cast<QMenu *>(widget);
        if (!wasVisible && virtualSurface) widget->show();
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        layout(widget);
        QImage image(widget->size() * 2, QImage::Format_ARGB32_Premultiplied); image.setDevicePixelRatio(2); image.fill(Qt::transparent);
        QPainter painter(&image); widget->render(&painter);
        bool canvasPixels = false;
        if (auto *canvas = widget->findChild<ik::ImageCanvas *>(); canvas && canvas->isVisible()) {
            const auto frame = canvas->viewportImage();
            if (!frame.isNull()) { painter.drawImage(QRect(canvas->mapTo(widget, QPoint()), canvas->size()), frame); canvasPixels = true; }
        }
        painter.end();
        captures.append(QJsonObject{{"name", name}, {"width", widget->width()}, {"height", widget->height()}, {"native_canvas_pixels", canvasPixels}});
        const bool saved = output.isEmpty() || image.save(QDir(output).filePath(name + ".png"));
        if (!wasVisible && virtualSurface) widget->hide();
        return saved;
    }
    bool modalCall(const std::function<void()> &open, const QString &name) {
        bool result = false;
        QTimer timer; timer.setSingleShot(true);
        connect(&timer, &QTimer::timeout, this, [&] {
            auto *dialog = qobject_cast<QDialog *>(QApplication::activeModalWidget());
            if (dialog) {
                result = capture(dialog, name);
                if (auto *box = qobject_cast<QMessageBox *>(dialog); box && box->button(QMessageBox::No)) box->button(QMessageBox::No)->click();
                else dialog->reject();
            }
        });
        timer.start(0); open(); timer.stop();
        return result;
    }
    bool modal(QAction *action, const QString &name) {
        return action && action->isEnabled() && modalCall([action] { action->trigger(); }, name);
    }
    QVector<ik::ExportSource> sources(int count) {
        QVector<ik::ExportSource> result;
        for (int i = 0; i < count; ++i) {
            ik::ExportSource source;
            source.snapshot = QImage(800, 600, QImage::Format_RGB32); source.snapshot.fill(QColor::fromHsv(i * 85, 180, 200));
            source.entry.path = temporary.filePath(QString("%1-photo-with-a-long-filename.png").arg(i + 1));
            source.entry.name = QFileInfo(source.entry.path).fileName(); source.snapshot.save(source.entry.path); result.append(source);
        }
        return result;
    }
private slots:
    void initTestCase() {
        QCOMPARE(QGuiApplication::platformName(), QString("offscreen")); QVERIFY(temporary.isValid());
        qputenv("IKVIEW_SETTINGS_PATH", temporary.filePath("preferences.ini").toUtf8());
        output = qEnvironmentVariable("IKVIEW_DESIGN_OUTPUT_DIR"); if (!output.isEmpty()) QVERIFY(QDir().mkpath(output));
    }
    void renderEveryApplicationScreen_data() {
        QTest::addColumn<QString>("theme"); QTest::newRow("dark") << QString("dark"); QTest::newRow("light") << QString("light");
    }
    void renderEveryApplicationScreen() {
        QFETCH(QString, theme);
        { QSettings settings(qEnvironmentVariable("IKVIEW_SETTINGS_PATH"), QSettings::IniFormat); settings.clear(); settings.setValue("theme", theme); settings.sync(); }
        ik::MainWindow window; window.resize(1280, 820); QVERIFY(capture(&window, theme + "-main-empty"));
        auto *manager = window.findChild<ik::ShortcutManager *>(); QVERIFY(manager);
        ik::PreferencesValues values; values.theme = theme;
        ik::PreferencesDialog preferences(values, ik::detectSystemProfile(), new ik::ShortcutEditor(manager));
        auto *tabs = preferences.findChild<QTabWidget *>("preferencesTabs"); QVERIFY(tabs);
        for (int tab = 0; tab < tabs->count(); ++tab) { tabs->setCurrentIndex(tab); QVERIFY(capture(&preferences, theme + QString("-preferences-%1").arg(tab))); }
        tabs->setCurrentIndex(2); preferences.resize(640, 480); QVERIFY(capture(&preferences, theme + "-preferences-compact"));
        auto *shortcutEditor = preferences.findChild<ik::ShortcutEditor *>(); QVERIFY(shortcutEditor);
        preferences.setValidationCallback([shortcutEditor](QString *error) { return shortcutEditor->validate(error); });
        const auto commands = manager->entries(); QVERIFY(commands.size() > 2);
        auto *binding = preferences.findChild<QKeySequenceEdit *>("shortcutBinding_" + commands[1].id); QVERIFY(binding);
        binding->setKeySequence(commands[0].defaults.first()); preferences.accept();
        QVERIFY(capture(&preferences, theme + "-preferences-conflict"));
        for (const int count : {1, 3}) {
            ik::ExportDialog dialog(sources(count));
            if (count == 1) {
                QVERIFY(capture(&dialog, theme + "-export-image"));
                dialog.findChild<QPushButton *>("exportAdvancedToggle")->click(); dialog.resize(680, 680);
                dialog.findChild<QCheckBox *>("exportTransparentPadding")->setChecked(true); dialog.findChild<QSpinBox *>("exportPadding")->setValue(24);
                QVERIFY(capture(&dialog, theme + "-export-transparent-padding"));
                auto *format = dialog.findChild<QComboBox *>("exportFormat"); format->setCurrentIndex(format->findData(QByteArray("pdf")));
                QVERIFY(capture(&dialog, theme + "-export-pdf"));
            } else {
                QVERIFY(capture(&dialog, theme + "-export-combined"));
                dialog.resize(600, 480); QVERIFY(capture(&dialog, theme + "-export-compact"));
            }
        }
        {
            auto whiteSources = sources(1); auto &source = whiteSources.first(); source.snapshot = QImage(720, 480, QImage::Format_ARGB32); source.snapshot.fill(Qt::white);
            { QPainter painter(&source.snapshot); painter.setRenderHint(QPainter::Antialiasing); painter.setPen(Qt::NoPen); painter.setBrush(QColor("#3479b8")); painter.drawRoundedRect(QRectF(120, 60, 480, 360), 60, 60); painter.setBrush(Qt::white); painter.drawEllipse(QRectF(250, 140, 220, 200)); }
            ik::ExportDialog removal(whiteSources, nullptr, ik::ExportPurpose::RemoveWhiteBackground);
            auto *preview = removal.findChild<QWidget *>("whiteBackgroundPreview"); QTRY_VERIFY_WITH_TIMEOUT(preview->property("previewReady").toBool(), 5000);
            QVERIFY(capture(&removal, theme + "-remove-white-background"));
            removal.resize(600, 480); QVERIFY(capture(&removal, theme + "-remove-white-compact"));
        }
        { ik::ExportDialog dialog(sources(1)); dialog.findChild<QLineEdit *>("exportPath")->clear(); dialog.accept(); QVERIFY(capture(&dialog, theme + "-export-validation")); }
        {
            ik::ExportDialog dialog(sources(3));
            auto *progress = dialog.findChild<QProgressBar *>("exportProgress"); progress->setRange(0, 3); progress->setValue(1); progress->show();
            ik::setMessage(dialog.findChild<QLabel *>("exportValidationMessage"), "1 / 3개 이미지 처리 중…", "info");
            dialog.findChild<QWidget *>("exportControls")->setEnabled(false); dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->setEnabled(false);
            QVERIFY(capture(&dialog, theme + "-export-progress-fixture"));
        }
        QVERIFY(modal(window.findChild<QAction *>("manageBookmarksAction"), theme + "-bookmarks-empty"));
        auto fixtures = sources(3); QStringList paths; for (const auto &source : fixtures) paths.append(source.entry.path);
        window.openPaths(paths); auto *document = window.findChild<ik::Document *>(); QVERIFY(document);
        QTRY_VERIFY_WITH_TIMEOUT(!document->isBusy() && !document->currentImage().isNull(), 5000);
        window.findChild<QAction *>("toggleMetadataAction")->trigger(); QVERIFY(capture(&window, theme + "-main-image-chrome"));
        window.resize(840, 620); QVERIFY(capture(&window, theme + "-main-compact")); window.resize(1280, 820);
        window.findChild<QLineEdit *>("fileFilter")->setText("no matching image"); QVERIFY(capture(&window, theme + "-main-no-results")); window.findChild<QLineEdit *>("fileFilter")->clear();
        window.findChild<QAction *>("addBookmarkAction")->trigger();
        QVERIFY(modal(window.findChild<QAction *>("manageBookmarksAction"), theme + "-bookmarks-populated"));
        QVERIFY(modal(window.findChild<QAction *>("keyboardHelpAction"), theme + "-keyboard-help"));
        for (auto *action : window.findChildren<QAction *>()) {
            if (action->property("commandLabel").toString() == "이미지 번호로 이동…") QVERIFY(modal(action, theme + "-go-to-image"));
            if (action->property("commandLabel").toString() == "감마 보정…") QVERIFY(modal(action, theme + "-gamma"));
            if (action->property("commandLabel").toString().contains("지원 형식")) QVERIFY(modal(action, theme + "-system-information"));
        }
        // Reuse actual action menus without opening popup windows.
        for (const auto &id : {"slideshowIntervalMenu", "imageFilterMenu"}) {
            if (auto *menu = window.findChild<QMenu *>(id)) { menu->resize(menu->sizeHint()); QVERIFY(capture(menu, theme + '-' + id)); }
        }
        auto *files = window.findChild<QListView *>("fileList"); files->selectAll();
        QMetaObject::invokeMethod(files, "customContextMenuRequested", Qt::DirectConnection, Q_ARG(QPoint, QPoint(24, 24)));
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
        if (auto *menu = window.findChild<QMenu *>("selectionContextMenu")) { QVERIFY(capture(menu, theme + "-selection-menu")); menu->close(); }
        {
            QTimer timer; timer.setSingleShot(true); bool saved = false;
            connect(&timer, &QTimer::timeout, this, [&] { if (auto *menu = qobject_cast<QMenu *>(QApplication::activePopupWidget())) { saved = capture(menu, theme + "-image-context-menu"); menu->close(); } });
            timer.start(0); auto *canvas = window.findChild<ik::ImageCanvas *>();
            QMetaObject::invokeMethod(canvas, "customContextMenuRequested", Qt::DirectConnection, Q_ARG(QPoint, QPoint(24, 24))); timer.stop(); QVERIFY(saved);
        }
        const auto pdf = temporary.filePath("sample.pdf");
        { QPdfWriter writer(pdf); writer.setResolution(72); writer.setPageSize(QPageSize(QSizeF(200, 140), QPageSize::Point)); QPainter painter(&writer); painter.fillRect(0, 0, 200, 140, Qt::white); painter.drawText(20, 40, "PDF page 1"); writer.newPage(); painter.drawText(20, 40, "PDF page 2"); }
        window.openPaths({pdf}); QTRY_VERIFY_WITH_TIMEOUT(!document->isBusy() && document->currentInfo().pdf, 5000);
        QVERIFY(capture(&window, theme + "-main-pdf"));
        window.resize(840, 620); QVERIFY(capture(&window, theme + "-main-pdf-compact")); QVERIFY(window.findChild<QWidget *>("imageFooter")->height() <= 52); window.resize(1280, 820);
        const auto damaged = temporary.filePath("damaged.png"); { QFile file(damaged); QVERIFY(file.open(QIODevice::WriteOnly)); file.write("invalid image"); }
        window.openPaths({damaged}); QTRY_COMPARE_WITH_TIMEOUT(window.property("documentState").toString(), QString("error"), 5000);
        QVERIFY(capture(&window, theme + "-main-error"));
        window.openPaths({pdf}); QTRY_VERIFY_WITH_TIMEOUT(!document->isBusy() && document->currentInfo().pdf, 5000);
        window.findChild<QToolButton *>("dismissSourceHintButton")->click(); QVERIFY(window.findChild<QWidget *>("sourceScopeHint")->isHidden());
        QVERIFY(capture(&window, theme + "-main-pdf-hint-dismissed"));
        window.findChild<QAction *>("cleanViewAction")->trigger(); QVERIFY(capture(&window, theme + "-main-focus"));
        bool accepted = true;
        QVERIFY(modalCall([&] { ik::textPrompt(&window, "책갈피 이름", "이름", "아주 긴 이름의 책갈피", &accepted); }, theme + "-bookmark-rename")); QVERIFY(!accepted);
        QMessageBox::StandardButton answer = QMessageBox::NoButton;
        QVERIFY(modalCall([&] { answer = ik::questionDialog(&window, "덮어쓰기", "같은 이름의 파일이 있습니다. 새 파일로 바꿀까요?", QMessageBox::Yes | QMessageBox::No, QMessageBox::No, "덮어쓰기"); }, theme + "-confirmation")); QCOMPARE(answer, QMessageBox::No);
        QVERIFY(modalCall([&] { ik::warningDialog(&window, "저장하지 못했습니다", "저장 위치와 파일 접근 권한을 확인한 뒤 다시 시도하세요."); }, theme + "-warning"));
        for (auto *widget : QApplication::topLevelWidgets()) QVERIFY(!widget->isVisible());
    }
    void compactLayoutsKeepFooterAndInputsAccessible() {
        ik::applyApplicationTheme("dark");
        ik::MainWindow window;
        auto *manager = window.findChild<ik::ShortcutManager *>(); QVERIFY(manager);
        ik::PreferencesDialog preferences({}, ik::detectSystemProfile(), new ik::ShortcutEditor(manager));
        preferences.resize(640, 480); preferences.findChild<QTabWidget *>()->setCurrentIndex(2); QVERIFY(capture(&preferences, "layout-preferences-check"));
        auto *table = preferences.findChild<QTableWidget *>(); QVERIFY(table); QCOMPARE(table->horizontalScrollBar()->maximum(), 0); QVERIFY(table->columnWidth(0) >= 180); QCOMPARE(table->columnWidth(3), 80);
        ik::ExportDialog dialog(sources(3)); dialog.resize(600, 480); QVERIFY(capture(&dialog, "layout-export-check"));
        auto *scroll = dialog.findChild<QScrollArea *>("exportScrollArea"); QVERIFY(scroll); QCOMPARE(scroll->horizontalScrollBar()->maximum(), 0); QVERIFY(scroll->verticalScrollBar()->maximum() > 0);
        auto *buttons = dialog.findChild<QDialogButtonBox *>(); QVERIFY(buttons);
        const QRect footer(buttons->mapTo(&dialog, QPoint()), buttons->size()); QVERIFY(dialog.rect().contains(footer));
        QVERIFY(!buttons->button(QDialogButtonBox::Save)->isHidden());
        window.findChild<QAction *>("toggleMetadataAction")->trigger(); QVERIFY(capture(&window, "layout-main-check"));
        QCOMPARE(window.findChild<QWidget *>("filePanelHeader")->height(), window.findChild<QWidget *>("metadataHeader")->height());
    }
    void exportCoreOptionsStayVisibleWithoutExpandingAdvancedSettings_data() {
        QTest::addColumn<bool>("removeWhite"); QTest::addColumn<int>("count");
        QTest::newRow("convert") << false << 1; QTest::newRow("white-removal") << true << 1; QTest::newRow("pdf-combine") << false << 3;
    }
    void exportCoreOptionsStayVisibleWithoutExpandingAdvancedSettings() {
        QFETCH(bool, removeWhite); QFETCH(int, count);
        ik::ExportDialog dialog(sources(count), nullptr, removeWhite ? ik::ExportPurpose::RemoveWhiteBackground : ik::ExportPurpose::Convert);
        QVERIFY(capture(&dialog, QString("layout-export-core-%1").arg(QString::fromLatin1(QTest::currentDataTag()))));
        auto *advanced = dialog.findChild<QWidget *>("exportAdvancedOptions"); auto *toggle = dialog.findChild<QPushButton *>("exportAdvancedToggle"); QVERIFY(advanced && toggle); QVERIFY(advanced->isHidden()); QVERIFY(!toggle->isChecked());
        auto *scroll = dialog.findChild<QScrollArea *>("exportScrollArea"); auto *path = dialog.findChild<QLineEdit *>("exportPath"); QVERIFY(scroll && path);
        const QRect pathRect(path->mapTo(scroll->viewport(), QPoint()), path->size()); QVERIFY(scroll->viewport()->rect().contains(pathRect)); QCOMPARE(scroll->horizontalScrollBar()->maximum(), 0);
        QVERIFY(dialog.rect().contains(QRect(toggle->mapTo(&dialog, QPoint()), toggle->size())));
        QVERIFY(!dialog.findChild<QPushButton *>("browseExportPath")->autoDefault()); QVERIFY(!toggle->autoDefault());
        QVERIFY(dialog.findChild<QDialogButtonBox *>()->button(QDialogButtonBox::Save)->isDefault());
        if (removeWhite) {
            auto *preview = dialog.findChild<QWidget *>("whiteBackgroundPreview"); const QRect previewRect(preview->mapTo(scroll->viewport(), QPoint()), preview->size());
            QVERIFY2(scroll->viewport()->rect().contains(previewRect), qPrintable(QString("preview %1+%2 exceeds viewport %3").arg(previewRect.y()).arg(previewRect.height()).arg(scroll->viewport()->height())));
        }
        toggle->click(); QVERIFY(!advanced->isHidden()); auto *padding = dialog.findChild<QSpinBox *>("exportPadding"); padding->setValue(32); toggle->click(); QVERIFY(advanced->isHidden()); toggle->click(); QCOMPARE(padding->value(), 32);
    }
    void controlIndicatorsAndThemeContrastRemainVisible_data() {
        QTest::addColumn<QString>("theme"); QTest::newRow("dark") << QString("dark"); QTest::newRow("light") << QString("light");
    }
    void publicReadmeScreens() {
        const auto fixture = QFINDTESTDATA("../assets/readme-sample.jpg"); QVERIFY(!fixture.isEmpty());
        const QString path = temporary.filePath("모래언덕.jpg"); QVERIFY(QFile::copy(fixture, path));
        for (const auto &theme : {QString("dark"), QString("light")}) {
            { QSettings settings(qEnvironmentVariable("IKVIEW_SETTINGS_PATH"), QSettings::IniFormat); settings.clear(); settings.setValue("theme", theme); settings.setValue("filePanel", true); settings.setValue("infoPanel", false); settings.sync(); }
            ik::MainWindow window; window.resize(1200, 760); window.openPaths({path}); auto *doc = window.findChild<ik::Document *>();
            QTRY_VERIFY_WITH_TIMEOUT(!doc->isBusy() && !doc->currentImage().isNull(), 5000); window.findChild<QToolButton *>("dismissSourceHintButton")->click(); window.statusBar()->clearMessage();
            QVERIFY(capture(&window, "readme-viewer-" + theme));
        }
        ik::applyApplicationTheme("dark"); auto whiteSources = sources(1); auto &source = whiteSources.first(); source.snapshot = QImage(720, 480, QImage::Format_ARGB32); source.snapshot.fill(Qt::white);
        { QPainter painter(&source.snapshot); painter.setRenderHint(QPainter::Antialiasing); painter.setPen(Qt::NoPen); painter.setBrush(QColor("#3479b8")); painter.drawRoundedRect(QRectF(120, 60, 480, 360), 60, 60); painter.setBrush(Qt::white); painter.drawEllipse(QRectF(250, 140, 220, 200)); }
        ik::ExportDialog removal(whiteSources, nullptr, ik::ExportPurpose::RemoveWhiteBackground); removal.findChild<QLineEdit *>("exportPath")->setText("/Users/demo/Pictures/도형-transparent.png");
        auto *preview = removal.findChild<QWidget *>("whiteBackgroundPreview"); QTRY_VERIFY_WITH_TIMEOUT(preview->property("previewReady").toBool(), 5000); QVERIFY(capture(&removal, "readme-remove-white"));
    }
    void transientStatusDoesNotOverlapDetailsAfterHidingChrome() {
        ik::MainWindow window; auto *bar = window.statusBar();
        auto *details = bar->findChild<QLabel *>("statusDetailsLabel"); auto *message = bar->findChild<QLabel *>("statusMessageLabel"); QVERIFY(details); QVERIFY(message);
        details->setText("800 × 600 px · PNG · 2.7 KB"); bar->showMessage("3개 선택 · 우클릭 또는 단축키로 선택 메뉴", 0);
        window.findChild<QAction *>("toggleStatusAction")->trigger(); window.findChild<QAction *>("toggleStatusAction")->trigger();
        QVERIFY(capture(&window, "layout-status-message")); QVERIFY(details->isHidden()); QVERIFY(!message->isHidden());
        QCOMPARE(message->text(), bar->currentMessage()); bar->clearMessage(); QVERIFY(capture(&window, "layout-status-details"));
        QVERIFY(!details->isHidden()); QVERIFY(message->isHidden());
    }
    void controlIndicatorsAndThemeContrastRemainVisible() {
        QFETCH(QString, theme); ik::applyApplicationTheme(theme);
        for (const auto &name : {"check", "up", "down"}) { QImage image(":/ikview/style/" + QString(name) + '-' + theme + ".svg"); QVERIFY2(!image.isNull(), name); }
        QComboBox combo; combo.addItems({"첫 번째", "두 번째"}); combo.resize(240, 36); combo.show(); QCoreApplication::processEvents();
        QStyleOptionComboBox option; option.initFrom(&combo); option.rect = combo.rect();
        const QRect arrow = combo.style()->subControlRect(QStyle::CC_ComboBox, &option, QStyle::SC_ComboBoxArrow, &combo).adjusted(4, 5, -4, -5);
        const auto raster = combo.grab().toImage(); const auto ink = combo.palette().color(QPalette::Text);
        int visibleInk = 0; for (int y = arrow.top(); y <= arrow.bottom(); ++y) for (int x = arrow.left(); x <= arrow.right(); ++x) { const auto c = raster.pixelColor(x, y); if (qAbs(c.red() - ink.red()) + qAbs(c.green() - ink.green()) + qAbs(c.blue() - ink.blue()) < 100) ++visibleInk; }
        QVERIFY(visibleInk >= 8); combo.hide();
        const auto luminance = [](QColor c) { const auto linear = [](double v) { v /= 255.; return v <= .04045 ? v / 12.92 : std::pow((v + .055) / 1.055, 2.4); }; return .2126 * linear(c.red()) + .7152 * linear(c.green()) + .0722 * linear(c.blue()); };
        const auto contrast = [&luminance](QColor a, QColor b) { auto first = luminance(a), second = luminance(b); return (std::max(first, second) + .05) / (std::min(first, second) + .05); };
        const auto palette = qApp->palette(); QVERIFY(contrast(palette.color(QPalette::Text), palette.color(QPalette::Base)) >= 4.5); QVERIFY(contrast(palette.color(QPalette::PlaceholderText), palette.color(QPalette::Window)) >= 4.5);
        QVERIFY(contrast(palette.color(QPalette::HighlightedText), palette.color(QPalette::Highlight)) >= 4.5);
    }
    void placeholderFitsSmallAndLargeViewports_data() {
        QTest::addColumn<QSize>("size");
        for (const auto &size : {QSize(160, 120), QSize(280, 240), QSize(800, 600)}) QTest::newRow(qPrintable(QString("%1x%2").arg(size.width()).arg(size.height()))) << size;
    }
    void placeholderFitsSmallAndLargeViewports() {
        QFETCH(QSize, size); const auto areas = ik::placeholderLayout(size); const QRect bounds(QPoint(), size);
        QVERIFY(bounds.contains(areas.title)); QVERIFY(bounds.contains(areas.detail)); QVERIFY(!areas.title.intersects(areas.detail)); if (!areas.icon.isEmpty()) QVERIFY(bounds.contains(areas.icon));
        for (const auto &theme : {QString("dark"), QString("light")}) {
            ik::applyApplicationTheme(theme);
            for (const bool error : {false, true}) {
                QImage image(size * 2, QImage::Format_ARGB32_Premultiplied); image.setDevicePixelRatio(2); image.fill(ik::viewerThemeBackground(theme)); QPainter painter(&image);
                ik::paintViewerPlaceholder(painter, size, qApp->palette(), error ? "이미지를 표시할 수 없습니다" : "이미지를 열어보세요", error ? "파일을 읽을 수 없습니다. 원본 파일과 접근 권한을 확인한 뒤 다시 시도하세요." : "이미지 · PDF · 폴더 · 압축파일을 여기로 끌어놓으세요.\n파일 메뉴에서도 파일과 폴더를 선택할 수 있습니다."); painter.end();
                const auto name = QString("%1-canvas-%2-%3x%4").arg(theme, error ? "error" : "empty").arg(size.width()).arg(size.height());
                captures.append(QJsonObject{{"name", name}, {"width", size.width()}, {"height", size.height()}, {"kind", "production CPU overlay painter"}});
                if (!output.isEmpty()) QVERIFY(image.save(QDir(output).filePath(name + ".png")));
            }
        }
    }
    void cleanupTestCase() {
        if (!output.isEmpty()) {
            QFile file(QDir(output).filePath("manifest.json")); QVERIFY(file.open(QIODevice::WriteOnly)); file.write(QJsonDocument(captures).toJson());
            for (int page = 0; page * 12 < captures.size(); ++page) {
                QImage sheet(1008, 1184, QImage::Format_RGB32); sheet.fill(QColor("#e8edf2")); QPainter painter(&sheet); QFont font("Helvetica Neue"); font.setPixelSize(12); painter.setFont(font);
                for (int slot = 0; slot < 12 && page * 12 + slot < captures.size(); ++slot) {
                    const auto entry = captures[page * 12 + slot].toObject(); const auto name = entry["name"].toString(); QImage source(QDir(output).filePath(name + ".png")); QVERIFY(!source.isNull());
                    const auto scaled = source.scaled(312, 252, Qt::KeepAspectRatio, Qt::SmoothTransformation); const int x = (slot % 3) * 336, y = (slot / 3) * 296;
                    painter.fillRect(QRect(x + 8, y + 8, 320, 280), Qt::white); painter.drawImage(x + 12 + (312 - scaled.width()) / 2, y + 12, scaled);
                    painter.setPen(QColor("#243543")); painter.drawText(QRect(x + 16, y + 264, 304, 20), Qt::AlignLeft, painter.fontMetrics().elidedText(name, Qt::ElideRight, 304));
                }
                painter.end(); QVERIFY(sheet.save(QDir(output).filePath(QString("contact-%1.png").arg(page + 1))));
            }
        }
        qunsetenv("IKVIEW_SETTINGS_PATH");
    }
};
int main(int argc, char **argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv); app.setQuitOnLastWindowClosed(false); app.setStyle(ik::createViewerStyle());
    QFont font("Helvetica Neue"); font.setPixelSize(13); app.setFont(font);
    QCoreApplication::setApplicationVersion("0.7.1");
    DesignTests tests; return QTest::qExec(&tests, argc, argv);
}
#include "design_tests.moc"
