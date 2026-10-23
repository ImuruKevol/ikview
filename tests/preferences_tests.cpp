#include "preferencesdialog.h"
#include "imagecanvas.h"
#include "viewerplaceholder.h"

#include <QCheckBox>
#include <QComboBox>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>
#include <QTabWidget>
#include <QtTest>

namespace {
constexpr quint64 GiB = 1024ull * 1024ull * 1024ull;
ik::SystemProfile profile(quint64 ram = 16 * GiB)
{
    return {ram, 10, QStringLiteral("arm64"), QStringLiteral("Apple M5")};
}

int titleInkPixels(ik::ImageCanvas &canvas, bool lightTheme)
{
    const QImage frame = canvas.viewportImage();
    if (frame.isNull()) return 0;
    // Only the empty-state title. The colored icon sits above this rectangle
    // and must not allow a stale, low-contrast title to pass this test.
    const double scale = double(frame.width()) / canvas.width();
    const auto logicalTitle = ik::placeholderLayout(canvas.size()).title;
    const QRect title(qRound(logicalTitle.x() * scale), qRound(logicalTitle.y() * scale), qRound(logicalTitle.width() * scale), qRound(logicalTitle.height() * scale));
    int count = 0;
    for (int y = title.top(); y <= title.bottom() && y < frame.height(); ++y)
        for (int x = title.left(); x <= title.right() && x < frame.width(); ++x) {
            const auto pixel = frame.pixelColor(x, y);
            if (lightTheme ? pixel.red() < 120 && pixel.green() < 120 && pixel.blue() < 120
                           : pixel.red() > 180 && pixel.green() > 180 && pixel.blue() > 180) ++count;
        }
    return count;
}

QColor canvasCorner(ik::ImageCanvas &canvas)
{
    const auto frame = canvas.viewportImage();
    return frame.isNull() ? QColor() : frame.pixelColor(2, 2);
}

QColor canvasCenter(ik::ImageCanvas &canvas)
{
    const auto frame = canvas.viewportImage();
    return frame.isNull() ? QColor() : frame.pixelColor(frame.width() / 2, frame.height() / 2);
}
}

class PreferencesTests : public QObject {
    Q_OBJECT
private slots:
    void physicalMemoryPresets_data()
    {
        QTest::addColumn<quint64>("memory");
        QTest::addColumn<int>("recommended");
        QTest::addColumn<int>("maximum");
        QTest::newRow("unknown") << quint64(0) << 256 << 512;
        QTest::newRow("1GiB") << 1 * GiB << 32 << 64;
        QTest::newRow("4GiB") << 4 * GiB << 128 << 256;
        QTest::newRow("8GiB") << 8 * GiB << 256 << 512;
        QTest::newRow("16GiB") << 16 * GiB << 512 << 1024;
        QTest::newRow("32GiB") << 32 * GiB << 1024 << 2048;
        QTest::newRow("128GiB") << 128 * GiB << 1024 << 2048;
    }

    void physicalMemoryPresets()
    {
        QFETCH(quint64, memory);
        QFETCH(int, recommended);
        QFETCH(int, maximum);
        const auto hardware = profile(memory);
        const auto presets = ik::cachePresets(hardware);
        QCOMPARE(presets.size(), 4);
        QCOMPARE(ik::resolvedCacheMB(QStringLiteral("auto"), hardware), recommended);
        QCOMPARE(ik::resolvedCacheMB(QStringLiteral("high"), hardware), maximum);
        QCOMPARE(ik::resolvedCacheMB(QStringLiteral("unrecognized"), hardware), recommended);
        for (const auto &preset : presets) {
            QVERIFY(preset.megabytes >= 32 && preset.megabytes <= 2048);
            if (memory) QVERIFY(quint64(preset.megabytes) * 1024ull * 1024ull <= memory / 16);
            QVERIFY(preset.label.contains(QString::number(preset.megabytes)));
        }
    }

    void legacyLimitMigratesWithoutInflatingBudget()
    {
        const auto hardware = profile();
        QCOMPARE(ik::cachePresetForLegacyMB(512, hardware), QStringLiteral("auto"));
        QCOMPARE(ik::cachePresetForLegacyMB(256, hardware), QStringLiteral("low"));
        QCOMPARE(ik::cachePresetForLegacyMB(1024, hardware), QStringLiteral("high"));
        QCOMPARE(ik::cachePresetForLegacyMB(0, hardware), QStringLiteral("auto"));
        QCOMPARE(ik::cachePresetForLegacyMB(4096, hardware), QStringLiteral("auto"));
        const auto small = profile(4 * GiB);
        QVERIFY(ik::resolvedCacheMB(ik::cachePresetForLegacyMB(2048, small), small) <= 256);
    }

    void hardwareDetectionIsCachedAndBounded()
    {
        const auto first = ik::detectSystemProfile();
        const auto second = ik::detectSystemProfile();
        QCOMPARE(first.memoryBytes, second.memoryBytes);
        QCOMPARE(first.architecture, second.architecture);
        QVERIFY(first.logicalCores >= 1);
        QVERIFY(!first.architecture.isEmpty());
        QVERIFY(ik::resolvedCacheMB(QStringLiteral("auto"), first) <= 1024);
    }

    void dialogStagesRequestedSettings()
    {
        ik::PreferencesValues initial;
        initial.theme = QStringLiteral("light");
        initial.cachePreset = QStringLiteral("high");
        initial.preventSleep = true;
        ik::PreferencesDialog dialog(initial, profile(), new QWidget);
        QCOMPARE(dialog.findChild<QComboBox *>(QStringLiteral("themeMode"))->currentData().toString(), QStringLiteral("light"));
        auto *imageFilter = dialog.findChild<QComboBox *>(QStringLiteral("imageFilterMode"));
        QVERIFY(imageFilter);
        QCOMPARE(imageFilter->currentData().toString(), QStringLiteral("soften"));
        QCOMPARE(imageFilter->count(), 4);
        imageFilter->setCurrentIndex(imageFilter->findData(QStringLiteral("soften-sharpen")));
        QCOMPARE(dialog.findChild<QComboBox *>(QStringLiteral("imageCachePreset"))->currentData().toString(), QStringLiteral("high"));
        QVERIFY(dialog.findChild<QLabel *>(QStringLiteral("hardwareSummary"))->text().contains(QStringLiteral("16 GiB")));
        QVERIFY(!dialog.findChild<QWidget *>(QStringLiteral("photoFolderPath")));
        QVERIFY(!dialog.findChild<QWidget *>(QStringLiteral("secondaryPhotoFolderPath")));
        QVERIFY(!dialog.findChild<QWidget *>(QStringLiteral("editorPath")));
        dialog.findChild<QCheckBox *>(QStringLiteral("openContainingFolder"))->setChecked(true);
        dialog.findChild<QCheckBox *>(QStringLiteral("deleteBookmarkAfterOpen"))->setChecked(true);
        dialog.findChild<QCheckBox *>(QStringLiteral("resumeArchivePosition"))->setChecked(false);
        QVERIFY(!dialog.findChild<QSpinBox *>(QStringLiteral("slideshowInterval")));
        dialog.findChild<QPushButton *>(QStringLiteral("clearArchivePositionsButton"))->click();
        const auto draft = dialog.values();
        QVERIFY(draft.openContainingFolder);
        QCOMPARE(draft.imageFilter, QStringLiteral("soften-sharpen"));
        QVERIFY(draft.deleteBookmarkAfterOpen);
        QVERIFY(!draft.resumeArchive);
        QVERIFY(dialog.clearArchivePositionsRequested());
        dialog.reject();
        QCOMPARE(dialog.result(), int(QDialog::Rejected));
        QVERIFY(!initial.openContainingFolder);
        QVERIFY(!initial.deleteBookmarkAfterOpen);
        QCOMPARE(initial.imageFilter, QStringLiteral("soften"));
    }

    void conflictingShortcutsBlockSaveWithoutClosing()
    {
        ik::PreferencesDialog dialog({}, profile(), new QWidget);
        bool invalid = true;
        dialog.setValidationCallback([&](QString *error) {
            if (invalid) *error = QStringLiteral("같은 단축키가 사용 중입니다.");
            return !invalid;
        });
        dialog.accept();
        QCOMPARE(dialog.result(), int(QDialog::Rejected));
        QCOMPARE(dialog.findChild<QTabWidget *>(QStringLiteral("preferencesTabs"))->currentIndex(), 2);
        QCOMPARE(dialog.findChild<QLabel *>(QStringLiteral("preferencesValidationError"))->text(), QStringLiteral("같은 단축키가 사용 중입니다."));
        invalid = false;
        dialog.accept();
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
    }

    void enterInPreferencesFieldsDoesNotSave()
    {
        ik::PreferencesDialog dialog({}, profile(), new QWidget);
        dialog.show();
        dialog.activateWindow();
        QVERIFY(QTest::qWaitForWindowExposed(&dialog));
        auto *theme = dialog.findChild<QComboBox *>(QStringLiteral("themeMode"));
        QVERIFY(theme);
        theme->setFocus();
        QTRY_VERIFY(theme->hasFocus());
        QTest::keyClick(&dialog, Qt::Key_Return);
        QCOMPARE(dialog.result(), int(QDialog::Rejected));
        QVERIFY(dialog.isVisible());
        auto *save = dialog.findChild<QPushButton *>(QStringLiteral("savePreferencesButton"));
        QVERIFY(save);
        save->click();
        QCOMPARE(dialog.result(), int(QDialog::Accepted));
        QVERIFY(!dialog.isVisible());
    }

    void canvasThemeRefreshesCachedWelcomeWithoutReuploadingImage()
    {
        ik::ImageCanvas canvas;
        canvas.resize(640, 480);
        const QColor dark(QStringLiteral("#10151c"));
        const QColor light(QStringLiteral("#e8edf2"));
        canvas.setBackgroundColor(dark);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QTRY_COMPARE_WITH_TIMEOUT(canvasCorner(canvas), dark, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(titleInkPixels(canvas, false) > 100, 3000);
        canvas.setBackgroundColor(light);
        QTRY_COMPARE_WITH_TIMEOUT(canvasCorner(canvas), light, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(titleInkPixels(canvas, true) > 100, 3000);
        const auto unchanged = canvas.activityStats();
        canvas.setBackgroundColor(light);
        QCOMPARE(canvas.activityStats().sceneSyncCalls, unchanged.sceneSyncCalls);
        canvas.setBackgroundColor(dark);
        QTRY_COMPARE_WITH_TIMEOUT(canvasCorner(canvas), dark, 3000);
        QTRY_VERIFY_WITH_TIMEOUT(titleInkPixels(canvas, false) > 100, 3000);

        QImage image(800, 500, QImage::Format_RGB32);
        image.fill(Qt::red);
        canvas.setImage(image);
        QTRY_COMPARE_WITH_TIMEOUT(canvasCenter(canvas), QColor(Qt::red), 3000);
        const auto before = canvas.activityStats();
        canvas.setBackgroundColor(light);
        QTRY_COMPARE_WITH_TIMEOUT(canvasCorner(canvas), light, 3000);
        const auto after = canvas.activityStats();
        QCOMPARE(after.sceneSyncCalls, before.sceneSyncCalls);
        QCOMPARE(after.preparationJobs, before.preparationJobs);
        QCOMPARE(after.previewUploads, before.previewUploads);
        QCOMPARE(after.detailUploads, before.detailUploads);
        QCOMPARE(canvas.transformedImage().cacheKey(), image.cacheKey());
    }
};

QTEST_MAIN(PreferencesTests)
#include "preferences_tests.moc"
