#include "imagecanvas.h"

#include <QApplication>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFile>
#include <QMimeData>
#include <QNativeGestureEvent>
#include <QPointingDevice>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QUrl>
#include <QWheelEvent>
#include <QtTest>

namespace {
QQuickWindow *surfaceFor(ik::ImageCanvas &canvas)
{
    for (QWindow *window : QGuiApplication::allWindows()) {
        auto *surface = qobject_cast<QQuickWindow *>(window);
        if (surface && surface->property("ikviewCanvas").value<QObject *>() == &canvas) return surface;
    }
    return nullptr;
}

QImage solidImage()
{
    QImage image(720, 480, QImage::Format_RGB32);
    image.fill(QColor("#de493c"));
    return image;
}

QImage coordinateImage()
{
    // The color identifies the source coordinate. Holding the cursor anchor
    // during zoom must retain that color at the same on-screen point.
    QImage image(4000, 3000, QImage::Format_RGB32);
    for (int y = 0; y < image.height(); ++y) {
        auto *row = reinterpret_cast<QRgb *>(image.scanLine(y));
        for (int x = 0; x < image.width(); ++x)
            row[x] = qRgb(x * 255 / (image.width() - 1), y * 255 / (image.height() - 1), 64);
    }
    return image;
}

QColor colorAt(ik::ImageCanvas &canvas, const QPointF &logicalPoint)
{
    const QImage frame = canvas.viewportImage();
    if (frame.isNull()) return {};
    const int x = qBound(0, qRound(logicalPoint.x() * frame.width() / canvas.width()), frame.width() - 1);
    const int y = qBound(0, qRound(logicalPoint.y() * frame.height() / canvas.height()), frame.height() - 1);
    return frame.pixelColor(x, y);
}

void wheel(QQuickWindow *surface, const QPoint &pixels, const QPoint &angles,
           Qt::ScrollPhase phase, Qt::KeyboardModifiers modifiers = Qt::NoModifier,
           const QPointF &position = QPointF(200, 150))
{
    const QPointF global = surface->mapToGlobal(position.toPoint());
    QWheelEvent event(position, global, pixels, angles, Qt::NoButton, modifiers, phase, false);
    QCoreApplication::sendEvent(surface, &event);
}
}

class InputTests : public QObject {
    Q_OBJECT
private slots:
    void nativePinchChangesZoom()
    {
        ik::ImageCanvas canvas;
        canvas.resize(640, 480);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        canvas.setImage(solidImage());
        auto *surface = surfaceFor(canvas);
        QVERIFY(surface);
        QSignalSpy changes(&canvas, &ik::ImageCanvas::zoomChanged);
        const double before = canvas.zoom();
        const QPointF anchor(240, 180);
        const QPointF global = surface->mapToGlobal(anchor.toPoint());
        QNativeGestureEvent enlarge(Qt::ZoomNativeGesture, QPointingDevice::primaryPointingDevice(), 2,
                                    anchor, anchor, global, 0.25, QPointF());
        QCoreApplication::sendEvent(surface, &enlarge);
        QVERIFY(enlarge.isAccepted());
        QVERIFY(canvas.zoom() > before);
        QVERIFY(canvas.isManualZoom());
        QVERIFY(!changes.isEmpty());
        const double enlarged = canvas.zoom();
        QNativeGestureEvent reduce(Qt::ZoomNativeGesture, QPointingDevice::primaryPointingDevice(), 2,
                                   anchor, anchor, global, -0.2, QPointF());
        QCoreApplication::sendEvent(surface, &reduce);
        QVERIFY(reduce.isAccepted());
        QVERIFY(canvas.zoom() < enlarged);
    }

    void modifiedWheelKeepsCursorAnchor_data()
    {
        QTest::addColumn<int>("modifier");
        // Qt's macOS modifier mapping is Command -> ControlModifier,
        // physical Control -> MetaModifier. Both must work in the native view.
        QTest::newRow("Command") << int(Qt::ControlModifier);
        QTest::newRow("physical-Control") << int(Qt::MetaModifier);
    }

    void modifiedWheelKeepsCursorAnchor()
    {
        QFETCH(int, modifier);
        ik::ImageCanvas canvas;
        canvas.resize(640, 480);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        canvas.setImage(coordinateImage());
        canvas.setFitMode(ik::FitMode::Actual);
        auto *surface = surfaceFor(canvas);
        QVERIFY(surface);
        const QPointF anchor(130, 115);
        QTRY_VERIFY_WITH_TIMEOUT(qAbs(colorAt(canvas, anchor).blue() - 64) <= 1, 3000);
        const QColor beforeColor = colorAt(canvas, anchor);
        // Stay above the bottom-right minimap: that overlay intentionally keeps
        // its thumbnail image unchanged when only the viewport zoom changes.
        const QPointF farPoint(canvas.width() - 70, 80);
        const QColor beforeFarColor = colorAt(canvas, farPoint);
        const double beforeZoom = canvas.zoom();
        QSignalSpy changes(&canvas, &ik::ImageCanvas::zoomChanged);
        QSignalSpy navigation(&canvas, &ik::ImageCanvas::navigateRequested);
        wheel(surface, {}, QPoint(0, 120), Qt::NoScrollPhase, Qt::KeyboardModifiers(modifier), anchor);
        QVERIFY(canvas.zoom() > beforeZoom);
        QVERIFY(canvas.isManualZoom());
        QVERIFY(!changes.isEmpty());
        QCOMPARE(navigation.size(), 0);
        // Wait for the new zoomed frame, rather than accepting a stale frame
        // whose anchor color would coincidentally also match.
        QTRY_VERIFY2_WITH_TIMEOUT(colorAt(canvas, farPoint).red() < beforeFarColor.red() - 2,
            qPrintable(QStringLiteral("Before far %1, after far %2; zoom %3 -> %4; anchor %5 -> %6")
                .arg(beforeFarColor.name(), colorAt(canvas, farPoint).name()).arg(beforeZoom).arg(canvas.zoom())
                .arg(beforeColor.name(), colorAt(canvas, anchor).name())), 3000);
        QTRY_VERIFY_WITH_TIMEOUT(qAbs(colorAt(canvas, anchor).red() - beforeColor.red()) <= 2
                                    && qAbs(colorAt(canvas, anchor).green() - beforeColor.green()) <= 2, 3000);
    }

    void rightClickRequestsOneContextMenu()
    {
        ik::ImageCanvas canvas;
        canvas.resize(640, 480);
        canvas.setContextMenuPolicy(Qt::CustomContextMenu);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        canvas.setImage(solidImage());
        auto *surface = surfaceFor(canvas);
        QVERIFY(surface);
        QSignalSpy requests(&canvas, &QWidget::customContextMenuRequested);
        const QPoint position(180, 140);
        QTest::mouseClick(surface, Qt::RightButton, Qt::NoModifier, position);
        QCoreApplication::processEvents();
        QCOMPARE(requests.size(), 1);
        QCOMPARE(requests.first().first().toPoint(), position);
    }

    void mouseBackAndForwardNavigate()
    {
        ik::ImageCanvas canvas;
        canvas.resize(640, 480);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        canvas.setImage(solidImage());
        auto *surface = surfaceFor(canvas);
        QVERIFY(surface);
        QSignalSpy navigation(&canvas, &ik::ImageCanvas::navigateRequested);
        QTest::mouseClick(surface, Qt::BackButton, Qt::NoModifier, QPoint(200, 150));
        QTest::mouseClick(surface, Qt::ForwardButton, Qt::NoModifier, QPoint(200, 150));
        QCOMPARE(navigation.size(), 2);
        QCOMPARE(navigation[0][0].toInt(), -1);
        QCOMPARE(navigation[1][0].toInt(), 1);
    }

    void trackpadGestureDoesNotSkipSeveralImages()
    {
        ik::ImageCanvas canvas;
        canvas.resize(640, 480);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        canvas.setImage(solidImage());
        canvas.setFitMode(ik::FitMode::Fit);
        auto *surface = surfaceFor(canvas);
        QVERIFY(surface);
        QSignalSpy navigation(&canvas, &ik::ImageCanvas::navigateRequested);
        wheel(surface, {}, {}, Qt::ScrollBegin);
        for (int i = 0; i < 20; ++i) wheel(surface, QPoint(0, -20), {}, Qt::ScrollUpdate);
        wheel(surface, QPoint(0, -200), {}, Qt::ScrollMomentum);
        wheel(surface, QPoint(0, -200), {}, Qt::ScrollEnd);
        QCOMPARE(navigation.size(), 1);
        QCOMPARE(navigation[0][0].toInt(), 1);

        // A new gesture in the opposite direction is independent of the old
        // gesture's remainder and inertial tail.
        wheel(surface, {}, {}, Qt::ScrollBegin);
        for (int i = 0; i < 20; ++i) wheel(surface, QPoint(0, 20), {}, Qt::ScrollUpdate);
        wheel(surface, QPoint(0, 200), {}, Qt::ScrollEnd);
        QCOMPARE(navigation.size(), 2);
        QCOMPARE(navigation[1][0].toInt(), -1);
    }

    void fileDropNeverNegotiatesMove_data()
    {
        QTest::addColumn<int>("possibleActions");
        QTest::addColumn<int>("acceptedAction");
        QTest::newRow("Finder-copy-or-move") << int(Qt::CopyAction | Qt::MoveAction) << int(Qt::CopyAction);
        QTest::newRow("copy-only") << int(Qt::CopyAction) << int(Qt::CopyAction);
        QTest::newRow("link-or-move") << int(Qt::LinkAction | Qt::MoveAction) << int(Qt::LinkAction);
        QTest::newRow("reject-move-only") << int(Qt::MoveAction) << int(Qt::IgnoreAction);
    }

    void interruptedTrackpadDoesNotDisableMouseWheel()
    {
        ik::ImageCanvas canvas; canvas.resize(640, 480); canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas)); canvas.setImage(solidImage());
        canvas.setFitMode(ik::FitMode::Fit);
        auto *surface = surfaceFor(canvas); QVERIFY(surface);
        QSignalSpy navigation(&canvas, &ik::ImageCanvas::navigateRequested);
        wheel(surface, {}, {}, Qt::ScrollBegin);
        wheel(surface, QPoint(0, -100), {}, Qt::ScrollUpdate);
        QCOMPARE(navigation.size(), 1);
        QEvent interrupted(QEvent::WindowDeactivate); QCoreApplication::sendEvent(surface, &interrupted);
        // A wheel mouse does not produce a trackpad ScrollEnd event.
        wheel(surface, {}, QPoint(0, -120), Qt::NoScrollPhase);
        QCOMPARE(navigation.size(), 2);
    }

    void horizontalModifiedWheelKeepsAutomaticFit()
    {
        ik::ImageCanvas canvas; canvas.resize(640, 480); canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas)); canvas.setImage(solidImage());
        canvas.setFitMode(ik::FitMode::Fit);
        auto *surface = surfaceFor(canvas); QVERIFY(surface);
        const double before = canvas.zoom();
        wheel(surface, {}, QPoint(120, 0), Qt::NoScrollPhase, Qt::ControlModifier);
        QCOMPARE(canvas.zoom(), before);
        QVERIFY(!canvas.isManualZoom());
    }

    void largeImagePreparationLatency()
    {
        ik::ImageCanvas canvas; canvas.resize(640, 480); canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas)); canvas.setImage(solidImage());
        QImage large(10000, 8000, QImage::Format_RGBA8888); large.fill(QColor(64, 128, 192));
        QElapsedTimer elapsed; elapsed.start(); canvas.setImage(large);
        const auto imageCallMs = elapsed.elapsed();
        elapsed.restart(); canvas.setGamma(2.2);
        const auto effectCallMs = elapsed.elapsed();
        QTRY_VERIFY_WITH_TIMEOUT(!canvas.isPreparing(), 15000);
        const auto readyMs = elapsed.elapsed();
        qInfo("80MP canvas: setImage call=%lld ms; gamma call=%lld ms; gamma ready=%lld ms",
              qlonglong(imageCallMs), qlonglong(effectCallMs), qlonglong(readyMs));
        QVERIFY2(effectCallMs < 100, "Large color correction must be submitted without blocking the UI");
        QVERIFY(canvas.renderStats().previewPixels <= 4LL * 1024 * 1024);
        canvas.setFitMode(ik::FitMode::Actual);
        QTRY_VERIFY_WITH_TIMEOUT(!canvas.isPreparing(), 10000);
        QVERIFY(canvas.renderStats().detailPixels > 0);
        QVERIFY(canvas.renderStats().detailPixels <= 8LL * 1024 * 1024);
        QTRY_VERIFY_WITH_TIMEOUT(colorAt(canvas, QPointF(320, 240)).red() > 125, 3000);
        // Rapid source/effect requests have one latest pending slot. No older
        // completed worker may replace this final blue/inverted request.
        QImage blue(2600, 1800, QImage::Format_RGB32); blue.fill(Qt::blue);
        QImage red(2600, 1800, QImage::Format_RGB32); red.fill(Qt::red);
        for (int i = 0; i < 20; ++i) { canvas.setImage(i % 2 ? blue : red); canvas.setGamma(1.0 + (i % 3)); }
        canvas.setImage(blue); canvas.setGamma(1.0); canvas.setInverted(true);
        QTRY_VERIFY_WITH_TIMEOUT(!canvas.isPreparing(), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(colorAt(canvas, QPointF(320, 240)).red() > 230
                                 && colorAt(canvas, QPointF(320, 240)).blue() < 20, 3000);
    }

    void rotatedScreenAxisMirrors_data()
    {
        QTest::addColumn<int>("turns"); QTest::addColumn<bool>("horizontal");
        for (int turns = 0; turns < 4; ++turns) for (bool horizontal : {false, true})
            QTest::newRow(qPrintable(QStringLiteral("r%1-%2").arg(turns).arg(horizontal ? "h" : "v"))) << turns << horizontal;
    }
    void rotatedScreenAxisMirrors()
    {
        QFETCH(int, turns); QFETCH(bool, horizontal);
        ik::ImageCanvas canvas;
        QImage source(3, 2, QImage::Format_RGBA8888);
        for (int y = 0; y < 2; ++y) for (int x = 0; x < 3; ++x) source.setPixelColor(x, y, QColor(20 + x * 80, 30 + y * 150, x * 20 + y * 10, 90 + x * 50));
        canvas.setImage(source); canvas.rotate(turns);
        const QImage rotated = source.transformed(QTransform().rotate(turns * 90), Qt::FastTransformation);
        if (horizontal) canvas.flipHorizontal(); else canvas.flipVertical();
        QCOMPARE(canvas.transformedImage(), rotated.mirrored(horizontal, !horizontal));
        const auto snapshot = canvas.transformState();
        canvas.resetTransform();
        QCOMPARE(ik::ImageCanvas::applyTransform(source, snapshot), rotated.mirrored(horizontal, !horizontal));
        QCOMPARE(source.pixelColor(0, 0).alpha(), 90);
    }

    void colorCorrectionPreservesAlpha()
    {
        QImage source(1, 1, QImage::Format_RGBA8888); source.setPixelColor(0, 0, QColor(64, 128, 192, 37));
        ik::ImageTransformState state; state.gamma = 2.0; state.inverted = true;
        const auto result = ik::ImageCanvas::applyTransform(source, state);
        const auto color = result.pixelColor(0, 0);
        QCOMPARE(color.alpha(), 37);
        QCOMPARE(color.red(), qRound(std::sqrt(191.0 / 255) * 255));
        QCOMPARE(color.green(), qRound(std::sqrt(127.0 / 255) * 255));
        QCOMPARE(color.blue(), qRound(std::sqrt(63.0 / 255) * 255));
        QCOMPARE(source.pixelColor(0, 0), QColor(64, 128, 192, 37));
    }

    void spreadRemovalClampsLockedPan()
    {
        ik::ImageCanvas canvas; canvas.resize(640, 480); canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QImage first(100, 100, QImage::Format_RGB32); first.fill(Qt::red);
        QImage second(1000, 100, QImage::Format_RGB32); second.fill(Qt::blue);
        canvas.setImage(first); canvas.setSecondaryImage(second); canvas.setSpread(true);
        canvas.setZoom(4.0); canvas.setZoomLocked(true); QVERIFY(canvas.panViewport(QPointF(10000, 0)));
        canvas.setSecondaryImage({});
        QTRY_VERIFY_WITH_TIMEOUT(colorAt(canvas, QPointF(320, 240)).red() > 230, 3000);
        QVERIFY(!canvas.canPan());
        QVERIFY(!canvas.panViewportPage(1, 0));
    }

    void viewStateAndDprEvents()
    {
        ik::ImageCanvas canvas; canvas.resize(640, 480); canvas.setImage(solidImage());
        canvas.setZoom(2.75); canvas.setZoomLocked(true);
        const auto state = canvas.viewState();
        ik::ImageCanvas restored; restored.resize(640, 480); restored.restoreViewState(state); restored.setImage(solidImage());
        QCOMPARE(restored.zoom(), 2.75); QVERIFY(restored.zoomLocked()); QVERIFY(restored.isManualZoom());
        QSignalSpy changes(&restored, &ik::ImageCanvas::zoomChanged);
        QEvent dpr(QEvent::DevicePixelRatioChange); QCoreApplication::sendEvent(&restored, &dpr);
        QVERIFY(!changes.isEmpty()); QCOMPARE(restored.zoom(), 2.75);
        restored.setZoomLocked(false); restored.setFitMode(ik::FitMode::Fit);
        const double fit = restored.zoom();
        QEvent again(QEvent::DevicePixelRatioChange); QCoreApplication::sendEvent(&restored, &again);
        QCOMPARE(restored.zoom(), fit); QVERIFY(!restored.isManualZoom());
    }

    void minimapMovesAndUsesAdjustedColors()
    {
        ik::ImageCanvas canvas; canvas.resize(640, 480); canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas)); canvas.setImage(coordinateImage()); canvas.setFitMode(ik::FitMode::Actual);
        QTRY_VERIFY_WITH_TIMEOUT(!canvas.isPreparing(), 10000);
        const auto before = colorAt(canvas, QPointF(260, 180));
        auto *surface = surfaceFor(canvas); QVERIFY(surface);
        // Left side of the bottom-right minimap re-centers the viewport near the source's left edge.
        QTest::mouseClick(surface, Qt::LeftButton, Qt::NoModifier, QPoint(475, 390));
        QTRY_VERIFY_WITH_TIMEOUT(!canvas.isPreparing(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(colorAt(canvas, QPointF(260, 180)).red() < before.red() - 30, 3000);
        canvas.setImage(solidImage().scaled(4000, 3000)); canvas.setInverted(true);
        QTRY_VERIFY_WITH_TIMEOUT(!canvas.isPreparing(), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(colorAt(canvas, QPointF(538, 389)).red() < 100
                                 && colorAt(canvas, QPointF(538, 389)).green() > 150
                                 && colorAt(canvas, QPointF(538, 389)).blue() > 150, 3000);
        QTest::mousePress(surface, Qt::LeftButton, Qt::NoModifier, QPoint(200, 150));
        QCOMPARE(surface->cursor().shape(), Qt::ClosedHandCursor);
        QEvent deactivate(QEvent::WindowDeactivate); QCoreApplication::sendEvent(surface, &deactivate);
        QVERIFY(surface->cursor().shape() != Qt::ClosedHandCursor);
    }

    void transparentDetailIsNotCompositedTwice()
    {
        ik::ImageCanvas canvas; canvas.resize(640, 480); canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QImage image(4000, 3000, QImage::Format_RGBA8888); image.fill(QColor(64, 128, 192, 128));
        canvas.setImage(image); canvas.setFitMode(ik::FitMode::Actual); canvas.setSmooth(false);
        QTRY_VERIFY_WITH_TIMEOUT(!canvas.isPreparing(), 10000);
        QVERIFY(canvas.renderStats().detailPixels > 0);
        QTRY_VERIFY_WITH_TIMEOUT(colorAt(canvas, QPointF(260, 180)).blue() >= 110
                                 && colorAt(canvas, QPointF(260, 180)).blue() <= 128, 3000);
    }

    void hugeViewportDetailBudget()
    {
        // No native 10k window/texture is needed to exercise viewport planning.
        ik::ImageCanvas canvas; canvas.resize(10000, 10000);
        QImage image(18000, 1000, QImage::Format_RGB32); image.fill(Qt::green);
        canvas.setImage(image); canvas.setFitMode(ik::FitMode::Actual);
        QTRY_VERIFY_WITH_TIMEOUT(!canvas.isPreparing(), 15000);
        auto stats = canvas.renderStats();
        QVERIFY(stats.previewPixels <= 4LL * 1024 * 1024);
        QVERIFY(stats.detailPixels > 0); QVERIFY(stats.detailPixels <= 8LL * 1024 * 1024);
        QVERIFY(stats.detailResolutionLimited);
        canvas.setZoom(8.0);
        QTRY_VERIFY_WITH_TIMEOUT(!canvas.isPreparing(), 10000);
        QVERIFY(!canvas.renderStats().detailResolutionLimited);
    }

    void pendingSpreadKeepsFitStable()
    {
        ik::ImageCanvas canvas; canvas.resize(640, 480);
        canvas.setImage(solidImage()); canvas.setSpread(true); canvas.setSecondaryPending(true);
        const double reserved = canvas.zoom();
        canvas.setSecondaryImage(solidImage()); QCOMPARE(canvas.zoom(), reserved);
    }

    void smartZoomPanAndQuarterTurnGesture()
    {
        ik::ImageCanvas canvas; canvas.resize(640, 480); canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas)); canvas.setImage(coordinateImage());
        auto *surface = surfaceFor(canvas); QVERIFY(surface);
        const QPointF position(230, 170); const auto global = surface->mapToGlobal(position.toPoint());
        const auto gesture = [&](Qt::NativeGestureType type, double value, QPointF delta = {}) {
            QNativeGestureEvent event(type, QPointingDevice::primaryPointingDevice(), 2, position, position, global, value, delta);
            QCoreApplication::sendEvent(surface, &event); return event.isAccepted();
        };
        QVERIFY(gesture(Qt::ZoomNativeGesture, 0)); QVERIFY(!canvas.isManualZoom());
        QVERIFY(gesture(Qt::SmartZoomNativeGesture, 1)); QCOMPARE(canvas.zoom(), 1.0);
        QTRY_VERIFY_WITH_TIMEOUT(!canvas.isPreparing(), 10000);
        const QColor before = colorAt(canvas, QPointF(260, 180));
        QVERIFY(gesture(Qt::PanNativeGesture, 0, QPointF(-80, -60)));
        QTRY_VERIFY_WITH_TIMEOUT(colorAt(canvas, QPointF(260, 180)).red() > before.red() + 3, 3000);
        QVERIFY(gesture(Qt::SmartZoomNativeGesture, 0)); QVERIFY(!canvas.isManualZoom());
        QCOMPARE(canvas.fitMode(), ik::FitMode::Fit);
        QVERIFY(gesture(Qt::BeginNativeGesture, 0)); QVERIFY(gesture(Qt::RotateNativeGesture, 30));
        QVERIFY(gesture(Qt::RotateNativeGesture, 30)); QVERIFY(gesture(Qt::EndNativeGesture, 0));
        QCOMPARE(canvas.transformState().quarterTurns, 1);
        QVERIFY(gesture(Qt::BeginNativeGesture, 0)); QVERIFY(gesture(Qt::RotateNativeGesture, -90));
        QVERIFY(gesture(Qt::EndNativeGesture, 0)); QCOMPARE(canvas.transformState().quarterTurns, 0);
    }

    void veryWideImageRetainsOriginalPixelDetail()
    {
        ik::ImageCanvas canvas; canvas.resize(640, 480); canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QImage image(24000, 512, QImage::Format_RGB32);
        for (int y = 0; y < image.height(); ++y) {
            auto *row = reinterpret_cast<QRgb *>(image.scanLine(y));
            for (int x = 0; x < image.width(); ++x) row[x] = x % 2 ? qRgb(255, 255, 255) : qRgb(0, 0, 0);
        }
        canvas.setImage(image); canvas.setFitMode(ik::FitMode::Actual); canvas.setSmooth(false);
        QTRY_VERIFY_WITH_TIMEOUT(!canvas.isPreparing(), 10000);
        QImage frame;
        QTRY_VERIFY_WITH_TIMEOUT(!(frame = canvas.viewportImage()).isNull(), 3000);
        int transitions = 0;
        for (int x = frame.width() / 2 - 32; x < frame.width() / 2 + 32; ++x)
            if (qAbs(frame.pixelColor(x, frame.height() / 2).red() - frame.pixelColor(x + 1, frame.height() / 2).red()) > 180) ++transitions;
        QVERIFY2(transitions >= 60, qPrintable(QStringLiteral("Only %1 of 64 source-pixel stripe transitions retained").arg(transitions)));
        QVERIFY(canvas.renderStats().detailPixels <= 8LL * 1024 * 1024);
    }

    void widthShrinkAndRightToLeftSpread()
    {
        ik::ImageCanvas canvas; canvas.resize(640, 480); canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QImage tall(100, 1000, QImage::Format_RGB32); tall.fill(Qt::red);
        canvas.setImage(tall); canvas.setFitMode(ik::FitMode::Width);
        QVERIFY(qAbs(canvas.zoom() - (canvas.width() - 48.0) * canvas.devicePixelRatioF() / tall.width()) < .001);
        QVERIFY(canvas.canPan()); QVERIFY(canvas.panViewportPage(0, -1));
        canvas.setFitMode(ik::FitMode::Shrink); QVERIFY(canvas.zoom() <= 1.0);
        QImage first(100, 100, QImage::Format_RGB32); first.fill(Qt::red);
        QImage second(160, 60, QImage::Format_RGB32); second.fill(Qt::blue);
        canvas.setImage(first); canvas.setSecondaryImage(second); canvas.setSpread(true, true); canvas.setFitMode(ik::FitMode::Actual);
        const double unit = 1.0 / canvas.devicePixelRatioF();
        QTRY_VERIFY_WITH_TIMEOUT(colorAt(canvas, QPointF(320 + 80 * unit, 240)).red() > 230
                                 && colorAt(canvas, QPointF(320 - 50 * unit, 240)).blue() > 230, 3000);
    }

    void detailTextureReplacementKeepsBorrowedTexturesAlive()
    {
        ik::ImageCanvas canvas; canvas.resize(640, 480); canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QImage image(2600, 1800, QImage::Format_RGBA8888); image.fill(QColor(64, 128, 192, 128));
        canvas.setImage(image); canvas.setFitMode(ik::FitMode::Actual);
        for (int i = 0; i < 12; ++i) {
            canvas.setGamma(i % 2 ? 2.0 : 1.0);
            QTRY_VERIFY_WITH_TIMEOUT(!canvas.isPreparing(), 10000);
            const QColor expected = i % 2 ? QColor(100, 130, 150) : QColor(55, 90, 120);
            QTRY_VERIFY_WITH_TIMEOUT(qAbs(colorAt(canvas, QPointF(260, 180)).blue() - expected.blue()) < 18, 3000);
        }
        // Replace a detailed image with a tiny opaque one while its scene graph
        // has active preview strips; this used to leave borrowed textures stale.
        QImage small(80, 60, QImage::Format_RGB32); small.fill(Qt::green);
        canvas.setImage(small); canvas.setFitMode(ik::FitMode::Actual);
        QTRY_VERIFY_WITH_TIMEOUT(colorAt(canvas, QPointF(320, 240)).green() > 230, 3000);
        QCOMPARE(canvas.renderStats().detailPixels, 0);
    }

    void unchangedCanvasStateDoesNotScheduleRendering()
    {
        ik::ImageCanvas canvas; canvas.resize(640, 480); canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QImage image(2000, 1600, QImage::Format_RGB32); image.fill(Qt::red);
        const auto before = canvas.activityStats();
        canvas.setImage(image);
        // This frame needs only shared QImage references, even though it is
        // larger than the old 1MP synchronous threshold.
        QVERIFY(!canvas.isPreparing());
        QCOMPARE(canvas.activityStats().preparationJobs, before.preparationJobs);
        QTRY_VERIFY_WITH_TIMEOUT(colorAt(canvas, QPointF(320, 240)).red() > 230, 3000);
        const auto settled = canvas.activityStats();
        for (int i = 0; i < 100; ++i) {
            canvas.setImage(image); canvas.setSecondaryImage({}); canvas.setSecondaryPending(false);
            canvas.setStatusMessage({}, {}); canvas.setSpread(false); canvas.setZoomLocked(false);
            canvas.setSmooth(true); canvas.setBackgroundColor(QColor("#101318"));
        }
        QTest::qWait(80);
        const auto after = canvas.activityStats();
        QCOMPARE(after.sceneSyncCalls, settled.sceneSyncCalls);
        QCOMPARE(after.preparationJobs, settled.preparationJobs);
        QCOMPARE(after.sceneUpdates, settled.sceneUpdates);
        QCOMPARE(after.textureUploads, settled.textureUploads);
        QCOMPARE(after.overlayPaints, settled.overlayPaints);
    }

    void panningWithinPreparedGuardBandReusesDetailTexture()
    {
        ik::ImageCanvas canvas; canvas.resize(640, 480); canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QImage image(6000, 4000, QImage::Format_RGB32); image.fill(Qt::red);
        canvas.setImage(image); canvas.setFitMode(ik::FitMode::Actual);
        QTRY_VERIFY_WITH_TIMEOUT(!canvas.isPreparing(), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(colorAt(canvas, QPointF(320, 240)).red() > 230, 3000);
        const auto before = canvas.activityStats();
        for (int i = 0; i < 18; ++i) QVERIFY(canvas.panViewport(QPointF(-8, 0)));
        QTest::qWait(80);
        const auto after = canvas.activityStats();
        QCOMPARE(after.preparationJobs, before.preparationJobs);
        QCOMPARE(after.detailUploads, before.detailUploads);
        QCOMPARE(after.previewUploads, before.previewUploads);
        QVERIFY(canvas.renderStats().detailPixels <= 8LL * 1024 * 1024);
        QVERIFY(!canvas.renderStats().detailResolutionLimited);
    }

    void secondaryPreparationReusesPrimaryDetailPixels()
    {
        ik::ImageCanvas canvas; canvas.resize(640, 480); canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QImage first(2600, 1800, QImage::Format_RGB32); first.fill(Qt::red);
        QImage second(2600, 1800, QImage::Format_RGB32); second.fill(Qt::blue);
        canvas.setImage(first); canvas.setSpread(true); canvas.setSecondaryPending(true); canvas.setFitMode(ik::FitMode::Actual);
        QTRY_VERIFY_WITH_TIMEOUT(!canvas.isPreparing(), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(colorAt(canvas, QPointF(260, 180)).red() > 230, 3000);
        const auto before = canvas.activityStats();
        canvas.setSecondaryImage(second);
        QTRY_VERIFY_WITH_TIMEOUT(!canvas.isPreparing(), 10000);
        QTRY_VERIFY_WITH_TIMEOUT(colorAt(canvas, QPointF(380, 180)).blue() > 230, 3000);
        const auto after = canvas.activityStats();
        QCOMPARE(after.previewUploads, before.previewUploads + 1);
        QCOMPARE(after.detailUploads, before.detailUploads + 1);
    }

    void backgroundChangesKeepImageTextures()
    {
        ik::ImageCanvas canvas; canvas.resize(640, 480); canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        QImage image(80, 60, QImage::Format_RGB32); image.fill(Qt::red);
        canvas.setImage(image); canvas.setFitMode(ik::FitMode::Actual);
        QTRY_VERIFY_WITH_TIMEOUT(colorAt(canvas, QPointF(320, 240)).red() > 230, 3000);
        const auto before = canvas.activityStats();
        canvas.setBackgroundColor(QColor("#203040"));
        QTRY_COMPARE_WITH_TIMEOUT(colorAt(canvas, QPointF(10, 10)), QColor("#203040"), 3000);
        const auto after = canvas.activityStats();
        QCOMPARE(after.sceneSyncCalls, before.sceneSyncCalls);
        QCOMPARE(after.previewUploads, before.previewUploads);
        QCOMPARE(after.detailUploads, before.detailUploads);
    }

    void atomicImageSwitchResetsTransformsAndKeepsLockedZoom()
    {
        ik::ImageCanvas canvas; canvas.resize(640, 480); canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        const QImage first = solidImage();
        QImage second(1000, 500, QImage::Format_RGB32); second.fill(Qt::green);
        canvas.setImage(first); canvas.setSecondaryImage(first); canvas.setSpread(true);
        canvas.rotate(1); canvas.flipHorizontal(); canvas.setGamma(2.0); canvas.setInverted(true);
        canvas.setZoom(2.75); canvas.setZoomLocked(true); canvas.panViewport(QPointF(-30, 20));
        const auto before = canvas.activityStats();
        canvas.setImage(second, true);
        QCOMPARE(canvas.activityStats().sceneSyncCalls, before.sceneSyncCalls + 1);
        QCOMPARE(canvas.zoom(), 2.75); QVERIFY(canvas.zoomLocked()); QVERIFY(canvas.isManualZoom());
        const auto transform = canvas.transformState();
        QCOMPARE(transform.quarterTurns, 0); QVERIFY(!transform.mirrorX && !transform.mirrorY);
        QCOMPARE(transform.gamma, 1.0); QVERIFY(!transform.inverted);
        QCOMPARE(canvas.transformedImage(), second);
        QTRY_VERIFY_WITH_TIMEOUT(colorAt(canvas, QPointF(320, 240)).green() > 230, 3000);
        canvas.setSpread(false); canvas.setZoomLocked(false); canvas.setFitMode(ik::FitMode::Width);
        QImage tall(100, 1000, QImage::Format_RGB32); tall.fill(Qt::red);
        canvas.setImage(tall, true);
        QVERIFY(!canvas.isManualZoom());
        QVERIFY(qAbs(canvas.zoom() - (canvas.width() - 48.0) * canvas.devicePixelRatioF() / tall.width()) < .001);
        // Width mode starts at the top, as in clear/reset/setImage cold navigation.
        QVERIFY(!canvas.panViewport(QPointF(0, 10000)));
        canvas.rotate(1); canvas.setGamma(1.5); canvas.setImage(tall, true);
        QCOMPARE(canvas.transformState().quarterTurns, 0); QCOMPARE(canvas.transformState().gamma, 1.0);
    }

    void viewportWithinDetailBudgetKeepsFullResolution()
    {
        ik::ImageCanvas canvas;
        const auto dpr = canvas.devicePixelRatioF();
        canvas.resize(qRound(8320.0 / dpr), qRound(1200.0 / dpr));
        QImage image(10000, 1000, QImage::Format_RGB32); image.fill(Qt::green);
        canvas.setImage(image); canvas.setFitMode(ik::FitMode::Actual);
        QTRY_VERIFY_WITH_TIMEOUT(!canvas.isPreparing(), 15000);
        const auto stats = canvas.renderStats();
        QVERIFY(stats.detailPixels > 8LL * 1000 * 1000);
        QVERIFY(stats.detailPixels <= 8LL * 1024 * 1024);
        QVERIFY2(!stats.detailResolutionLimited, "Guard-band padding must not downsample a viewport that fits the pixel budget");
    }

    void fileDropNeverNegotiatesMove()
    {
        QFETCH(int, possibleActions);
        QFETCH(int, acceptedAction);
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString path = temp.filePath("source.png");
        QVERIFY(solidImage().save(path));
        QFile original(path);
        QVERIFY(original.open(QIODevice::ReadOnly));
        const QByteArray sourceBytes = original.readAll();
        original.close();
        ik::ImageCanvas canvas;
        canvas.resize(640, 480);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        auto *surface = surfaceFor(canvas);
        QVERIFY(surface);
        QSignalSpy opened(&canvas, &ik::ImageCanvas::filesDropped);
        QMimeData mime;
        mime.setUrls({QUrl::fromLocalFile(path), QUrl::fromLocalFile(path)});
        const auto actions = Qt::DropActions(possibleActions);
        QDragEnterEvent enter(QPoint(180, 140), actions, &mime, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(surface, &enter);
        QDragMoveEvent move(QPoint(190, 150), actions, &mime, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(surface, &move);
        QDropEvent drop(QPointF(190, 150), actions, &mime, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(surface, &drop);
        const bool accepted = acceptedAction != int(Qt::IgnoreAction);
        QCOMPARE(enter.isAccepted(), accepted);
        QCOMPARE(move.isAccepted(), accepted);
        QCOMPARE(drop.isAccepted(), accepted);
        QCOMPARE(opened.size(), accepted ? 1 : 0);
        if (accepted) {
            QCOMPARE(int(drop.dropAction()), acceptedAction);
            QCOMPARE(opened.first().first().toStringList(), QStringList{path});
        }
        QVERIFY(original.open(QIODevice::ReadOnly));
        QCOMPARE(original.readAll(), sourceBytes);
    }

    void fileDropFiltersNonImages_data()
    {
        QTest::addColumn<int>("kind");
        QTest::newRow("reject-existing-Markdown") << 0;
        QTest::newRow("mixed-files-open-images-only") << 1;
        QTest::newRow("recognize-image-content-with-unknown-extension") << 2;
    }

    void fileDropFiltersNonImages()
    {
        QFETCH(int, kind);
        QTemporaryDir temp;
        QVERIFY(temp.isValid());
        const QString notesPath = temp.filePath("notes.md");
        QFile notes(notesPath);
        QVERIFY(notes.open(QIODevice::WriteOnly));
        notes.write("# Not an image\nKeep the currently viewed picture.\n");
        notes.close();
        const QString imagePath = temp.filePath(kind == 2 ? "picture.unknown" : "picture.png");
        const QImage original = solidImage();
        QVERIFY(original.save(imagePath, "PNG"));
        ik::ImageCanvas canvas;
        canvas.resize(640, 480);
        canvas.show();
        QVERIFY(QTest::qWaitForWindowExposed(&canvas));
        canvas.setImage(original);
        auto *surface = surfaceFor(canvas);
        QVERIFY(surface);
        QSignalSpy opened(&canvas, &ik::ImageCanvas::filesDropped);
        QMimeData mime;
        if (kind == 0) mime.setUrls({QUrl::fromLocalFile(notesPath)});
        else if (kind == 1) mime.setUrls({QUrl::fromLocalFile(notesPath), QUrl::fromLocalFile(imagePath),
                                        QUrl::fromLocalFile(notesPath), QUrl::fromLocalFile(imagePath)});
        else mime.setUrls({QUrl::fromLocalFile(imagePath)});
        QDragEnterEvent enter(QPoint(180, 140), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(surface, &enter);
        QDragMoveEvent move(QPoint(190, 150), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(surface, &move);
        QDropEvent drop(QPointF(190, 150), Qt::CopyAction, &mime, Qt::LeftButton, Qt::NoModifier);
        QCoreApplication::sendEvent(surface, &drop);
        const bool accepted = kind != 0;
        QCOMPARE(enter.isAccepted(), accepted);
        QCOMPARE(move.isAccepted(), accepted);
        QCOMPARE(drop.isAccepted(), accepted);
        QCOMPARE(opened.size(), accepted ? 1 : 0);
        if (accepted) QCOMPARE(opened.first().first().toStringList(), QStringList{imagePath});
        // Rejected drags do not clear or replace the current visible image.
        QCOMPARE(canvas.transformedImage(), original);
    }
};

int main(int argc, char **argv)
{
#ifdef Q_OS_MACOS
    QQuickWindow::setGraphicsApi(QSGRendererInterface::Metal);
#endif
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName("ikview-tests");
    QCoreApplication::setApplicationName("input-tests");
    QStandardPaths::setTestModeEnabled(true);
    InputTests tests;
    return QTest::qExec(&tests, argc, argv);
}
#include "input_tests.moc"
