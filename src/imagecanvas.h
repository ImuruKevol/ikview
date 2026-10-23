#pragma once
#include <QWidget>
#include <QImage>
#include <QPointF>
#include <memory>

class QPainter;
class QNativeGestureEvent;
namespace ik {
enum class FitMode { Fit, Width, Actual, Shrink };
enum class ImageFilterMode { None, Soften, Sharpen, SoftenSharpen };
struct ImageTransformState {
    int quarterTurns = 0;
    bool mirrorX = false;
    bool mirrorY = false;
    double gamma = 1.0;
    bool inverted = false;
};
struct CanvasViewState {
    FitMode fitMode = FitMode::Fit;
    double zoom = 1.0;
    bool manualZoom = false;
    bool zoomLocked = false;
};
struct CanvasRenderStats {
    qint64 previewPixels = 0;
    qint64 detailPixels = 0;
    bool detailResolutionLimited = false;
    bool preparing = false;
};
struct CanvasActivityStats {
    quint64 sceneSyncCalls = 0;
    quint64 sceneUpdates = 0;
    quint64 preparationJobs = 0;
    quint64 textureUploads = 0;
    quint64 previewUploads = 0;
    quint64 detailUploads = 0;
    quint64 checkerUploads = 0;
    quint64 overlayPaints = 0;
};
class ImageCanvas : public QWidget {
    Q_OBJECT
public:
    explicit ImageCanvas(QWidget *parent = nullptr);
    ~ImageCanvas() override;
    void setImage(const QImage &image, bool resetViewTransform = false);
    void setSecondaryImage(const QImage &image);
    void setSecondaryPending(bool pending);
    void setStatusMessage(const QString &title, const QString &detail = {});
    void setSpread(bool enabled, bool rightToLeft = false);
    void setFitMode(FitMode mode);
    FitMode fitMode() const;
    void zoomBy(double factor);
    void setZoom(double factor);
    double zoom() const;
    bool isManualZoom() const;
    void setZoomLocked(bool locked);
    bool zoomLocked() const;
    CanvasViewState viewState() const;
    void restoreViewState(const CanvasViewState &state);
    bool canPan() const;
    bool panViewport(const QPointF &delta);
    bool panViewportPage(int horizontalPages, int verticalPages);
    bool isPreparing() const;
    CanvasRenderStats renderStats() const;
    CanvasActivityStats activityStats() const;
    void rotate(int quarterTurns);
    void flipHorizontal();
    void flipVertical();
    void resetTransform();
    void setSmooth(bool enabled);
    void setImageFilter(ImageFilterMode mode);
    ImageFilterMode imageFilter() const;
    // Display-only pixel filter; sampling is chosen separately by the canvas.
    // The normal preview/detail path calls it only on bounded worker images.
    static QImage applyDisplayFilter(const QImage &image, ImageFilterMode mode);
    void setGamma(double gamma);
    void setInverted(bool inverted);
    void setBackgroundColor(const QColor &color);
    ImageTransformState transformState() const;
    static QImage applyTransform(const QImage &image, const ImageTransformState &state);
    QImage transformedImage() const;
    QImage viewportImage();
    QString rendererDescription() const;
signals:
    void zoomChanged(double factor);
    void navigateRequested(int direction);
    void filesDropped(const QStringList &paths);
    void fullScreenRequested();
    void preparationChanged(bool preparing);
    void framePrepared();
protected:
    bool event(QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    bool eventFilter(QObject *watched, QEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void wheelEvent(QWheelEvent *event) override;
    void dragEnterEvent(QDragEnterEvent *event) override;
    void dragMoveEvent(QDragMoveEvent *event) override;
    void dragLeaveEvent(QDragLeaveEvent *event) override;
    void dropEvent(QDropEvent *event) override;
private:
    bool nativeGestureEvent(QNativeGestureEvent *event);
    void setDropHovered(bool hovered);
    void requestPreparation();
    void startPreparation();
    void syncScene();
    void paintOverlay(::QPainter &painter);
    struct Impl;
    std::unique_ptr<Impl> d;
};
}
