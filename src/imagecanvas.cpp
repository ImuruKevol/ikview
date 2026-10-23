#include "apptheme.h"
#include <QApplication>
#include "viewerplaceholder.h"
#include "imagecanvas.h"
#include "dropfiles.h"

#include <QCursor>
#include <QColorSpace>
#include <cstring>
#include <QContextMenuEvent>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDragLeaveEvent>
#include <QDropEvent>
#include <QMatrix4x4>
#include <QMimeData>
#include <QMouseEvent>
#include <QNativeGestureEvent>
#include <QPainter>
#include <QPainterPath>
#include <QQuickItem>
#include <QQuickPaintedItem>
#include <QQuickWindow>
#include <QResizeEvent>
#include <QSGRendererInterface>
#include <QSGSimpleTextureNode>
#include <QSGTexture>
#include <QSGTransformNode>
#include <QVariant>
#include <QWheelEvent>
#include <QTimer>
#include <QFutureWatcher>
#include <QThreadPool>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <atomic>
#include <optional>
#include <vector>

namespace ik {
namespace {
constexpr double minZoom = 0.0001;
constexpr double maxZoom = 64.0;
constexpr qreal imageMargin = 24.0;
constexpr qreal spreadGap = 12.0;
// Metal's universally available maximum 2D texture extent on supported Macs.
constexpr int previewLimit = 16384;
constexpr qint64 previewPixelBudget = 4LL * 1024 * 1024;
constexpr qint64 detailPixelBudget = 8LL * 1024 * 1024;

bool acceptReadOnlyDrop(QDropEvent *event)
{
    // Opening an image must never negotiate Move with Finder and remove its source.
    if (event->possibleActions().testFlag(Qt::CopyAction)) event->setDropAction(Qt::CopyAction);
    else if (event->possibleActions().testFlag(Qt::LinkAction)) event->setDropAction(Qt::LinkAction);
    else { event->ignore(); return false; }
    event->accept();
    return true;
}

QSizeF orientedSize(const QImage &image, int rotation)
{
    if (image.isNull()) return {};
    return rotation % 2 ? QSizeF(image.height(), image.width()) : QSizeF(image.size());
}

QImage mirroredImage(const QImage &image, bool horizontal, bool vertical)
{
#if QT_VERSION >= QT_VERSION_CHECK(6, 9, 0)
    Qt::Orientations orientations;
    if (horizontal) orientations |= Qt::Horizontal;
    if (vertical) orientations |= Qt::Vertical;
    return image.flipped(orientations);
#else
    return image.mirrored(horizontal, vertical);
#endif
}

QImage colorAdjusted(const QImage &image, double gamma, bool inverted, const std::function<bool()> &cancelled = {})
{
    if (image.isNull() || (!inverted && std::abs(gamma - 1.0) < 0.0001)) return image;
    std::array<uchar, 256> lookup{};
    for (int i = 0; i < 256; ++i) {
        const double value = (inverted ? 255 - i : i) / 255.0;
        lookup[i] = static_cast<uchar>(std::clamp(std::lround(std::pow(value, 1.0 / gamma) * 255), 0L, 255L));
    }
    QImage result = image.convertToFormat(QImage::Format_RGBA8888);
    for (int y = 0; y < result.height(); ++y) {
        if (cancelled && cancelled()) return {};
        uchar *row = result.scanLine(y);
        for (int x = 0; x < result.width(); ++x) {
            row[4 * x] = lookup[row[4 * x]];
            row[4 * x + 1] = lookup[row[4 * x + 1]];
            row[4 * x + 2] = lookup[row[4 * x + 2]];
        }
    }
    return result;
}

bool sharpFilter(ImageFilterMode mode)
{
    return mode == ImageFilterMode::Sharpen || mode == ImageFilterMode::SoftenSharpen;
}
bool softFilter(ImageFilterMode mode)
{
    return mode == ImageFilterMode::Soften || mode == ImageFilterMode::SoftenSharpen;
}

QImage sharpened(const QImage &image, ImageFilterMode mode, const std::function<bool()> &cancelled = {})
{
    if (image.isNull() || !sharpFilter(mode)) return image;
    const bool opaque = !image.hasAlphaChannel();
    const QImage source = image.convertToFormat(QImage::Format_RGBA8888);
    if (source.isNull() || (cancelled && cancelled())) return {};
    QImage output(source.size(), QImage::Format_RGBA8888);
    if (output.isNull()) return {};
    output.setColorSpace(source.colorSpace()); output.setDevicePixelRatio(source.devicePixelRatio());
    using WeightedPixel = std::array<quint32,4>;
    using Row = std::vector<WeightedPixel>;
    Row top(source.width()), middle(source.width()), bottom(source.width());
    const auto horizontal = [&](int y, Row &row) {
        if (cancelled && cancelled()) return false;
        const uchar *input = source.constScanLine(std::clamp(y,0,source.height()-1));
        for (int x=0; x<source.width(); ++x) {
            const auto *left = input + 4*std::max(0,x-1);
            const auto *center = input + 4*x;
            const auto *right = input + 4*std::min(source.width()-1,x+1);
            const quint32 leftAlpha = opaque ? 1 : left[3], centerAlpha = opaque ? 1 : center[3], rightAlpha = opaque ? 1 : right[3];
            for (int c=0; c<3; ++c) row[x][c] = left[c]*leftAlpha + 2*center[c]*centerAlpha + right[c]*rightAlpha;
            row[x][3] = leftAlpha + 2*centerAlpha + rightAlpha;
        }
        return true;
    };
    if (!horizontal(-1,top) || !horizontal(0,middle) || !horizontal(1,bottom)) return {};
    // Mild fixed unsharp masking. The combined interpolation+sharpen mode uses
    // a lower amount to avoid ringing on already smoothed scans and lettering.
    const int amount = mode == ImageFilterMode::Sharpen ? 192 : 160;
    for (int y=0; y<source.height(); ++y) {
        if (cancelled && cancelled()) return {};
        const uchar *input = source.constScanLine(y);
        uchar *result = output.scanLine(y);
        for (int x=0; x<source.width(); ++x) {
            const quint32 alphaWeight = top[x][3] + 2*middle[x][3] + bottom[x][3];
            for (int c=0; c<3; ++c) {
                const int original = input[4*x+c];
                if (input[4*x+3] == 0 || alphaWeight == 0) { result[4*x+c] = original; continue; }
                const quint32 weighted = top[x][c] + 2*middle[x][c] + bottom[x][c];
                const int blurred = opaque ? int((weighted+8)>>4) : int((weighted+alphaWeight/2)/alphaWeight);
                const int difference = original-blurred;
                const int adjustment = (difference*amount + (difference>=0 ? 128 : -128))/256;
                result[4*x+c] = uchar(std::clamp(original+adjustment,0,255));
            }
            // Keep coverage exactly. Fully transparent neighbors cannot tint
            // visible pixels because both separable passes retain alpha weights.
            result[4*x+3] = input[4*x+3];
        }
        if (y+1<source.height()) {
            top.swap(middle); middle.swap(bottom);
            if (!horizontal(y+2,bottom)) return {};
        }
    }
    return output;
}

QSize boundedSize(QSize size, qint64 pixels)
{
    if (size.isEmpty()) return {};
    const double ratio = std::min({1.0, double(previewLimit) / size.width(), double(previewLimit) / size.height(),
        std::sqrt(double(pixels) / (double(size.width()) * size.height()))});
    return {std::max(1, int(std::floor(size.width() * ratio))), std::max(1, int(std::floor(size.height() * ratio)))};
}

struct PreparedPage {
    QImage preview;
    QImage detail;
    QRect detailRect;
    bool limited = false;
};
struct PageRequest {
    QImage source;
    QImage cachedPreview;
    QImage cachedDetail;
    QRect detailRect;
    QSize detailSize;
    bool limited = false;
};
struct PreparationRequest {
    PageRequest first;
    PageRequest second;
    double gamma = 1;
    bool inverted = false;
    ImageFilterMode filter = ImageFilterMode::Soften;
    quint64 serial = 0;
    quint64 epoch = 0;
};
struct PreparedFrame {
    PreparedPage first;
    PreparedPage second;
    PreparationRequest request;
};
bool samePageRequest(const PageRequest &a, const PageRequest &b) {
    return a.source.cacheKey() == b.source.cacheKey() && a.detailRect == b.detailRect && a.detailSize == b.detailSize;
}
bool sameRequest(const PreparationRequest &a, const PreparationRequest &b) {
    return a.gamma == b.gamma && a.inverted == b.inverted && a.filter == b.filter && samePageRequest(a.first, b.first) && samePageRequest(a.second, b.second);
}

PreparedFrame prepareFrame(const PreparationRequest &request, const std::shared_ptr<std::atomic<quint64>> &epoch)
{
    const auto cancelled = [&] { return epoch->load() != request.epoch; };
    const auto prepare = [&](const PageRequest &page) {
        PreparedPage ready;
        if (page.source.isNull() || cancelled()) return ready;
        if (!page.cachedPreview.isNull()) ready.preview = page.cachedPreview;
        else {
            const QSize target = boundedSize(page.source.size(), previewPixelBudget);
            const QImage scaled = target == page.source.size() ? page.source
                : page.source.scaled(target, Qt::IgnoreAspectRatio, softFilter(request.filter) ? Qt::SmoothTransformation : Qt::FastTransformation);
            if (cancelled()) return ready;
            ready.preview = sharpened(colorAdjusted(scaled, request.gamma, request.inverted, cancelled), request.filter, cancelled);
        }
        if (!page.detailRect.isEmpty() && !cancelled()) {
            // Draw directly from the immutable source region. Copying the full
            // source rectangle first could allocate hundreds of megabytes when
            // zoomed out, although the visible output is only a few megapixels.
            if (!page.cachedDetail.isNull()) ready.detail = page.cachedDetail;
            else {
                // Sharpen with one output-pixel halo, then crop. A halo in
                // source pixels would be too small when this ROI is downscaled.
                const int halo = sharpFilter(request.filter) ? 1 : 0;
                QImage pixels(page.detailSize + QSize(halo*2,halo*2), QImage::Format_RGBA8888);
                pixels.fill(Qt::transparent);
                const qreal sx = qreal(page.detailRect.width())/page.detailSize.width();
                const qreal sy = qreal(page.detailRect.height())/page.detailSize.height();
                const QRectF sampled = QRectF(page.detailRect).adjusted(-halo*sx,-halo*sy,halo*sx,halo*sy);
                QPainter painter(&pixels);
                painter.setRenderHint(QPainter::SmoothPixmapTransform, softFilter(request.filter) && page.detailSize != page.detailRect.size());
                painter.drawImage(QRectF(QPointF(), pixels.size()), page.source, sampled);
                painter.end();
                if (halo) {
                    // At the true source border repeat the rendered edge, as
                    // the full-preview filter does. Transparent padding here
                    // would otherwise produce a different boundary kernel.
                    if (page.detailRect.left() == 0 || page.detailRect.right()+1 == page.source.width()) {
                        for (int y=0; y<pixels.height(); ++y) {
                            uchar *row=pixels.scanLine(y);
                            if (page.detailRect.left() == 0) std::memcpy(row,row+4,4);
                            if (page.detailRect.right()+1 == page.source.width()) std::memcpy(row+4*(pixels.width()-1),row+4*(pixels.width()-2),4);
                        }
                    }
                    if (page.detailRect.top() == 0) std::memcpy(pixels.scanLine(0),pixels.constScanLine(1),pixels.bytesPerLine());
                    if (page.detailRect.bottom()+1 == page.source.height()) std::memcpy(pixels.scanLine(pixels.height()-1),pixels.constScanLine(pixels.height()-2),pixels.bytesPerLine());
                }
                const QImage adjusted = colorAdjusted(pixels, request.gamma, request.inverted, cancelled);
                QImage filtered = sharpened(adjusted, request.filter, cancelled);
                ready.detail = halo && !filtered.isNull() ? filtered.copy(halo,halo,page.detailSize.width(),page.detailSize.height()) : filtered;
            }
            ready.detailRect = page.detailRect;
            ready.limited = page.limited;
        }
        return ready;
    };
    PreparedFrame ready;
    ready.request = request;
    ready.first = prepare(request.first);
    ready.second = prepare(request.second);
    return ready;
}

struct ActivityCounters {
    std::atomic<quint64> sceneSyncCalls{0};
    std::atomic<quint64> sceneUpdates{0};
    std::atomic<quint64> preparationJobs{0};
    std::atomic<quint64> previewUploads{0};
    std::atomic<quint64> detailUploads{0};
    std::atomic<quint64> checkerUploads{0};
    std::atomic<quint64> overlayPaints{0};
};

struct SceneState {
    qint64 first = 0, second = 0, firstDetail = 0, secondDetail = 0;
    QRect firstDetailRect, secondDetailRect;
    QSize originalFirst, originalSecond, viewport;
    QPointF pan;
    qreal scale = 1;
    int rotation = 0;
    bool mirrorX = false, mirrorY = false, spread = false, secondaryPending = false, rightToLeft = false, smooth = true;
    bool operator==(const SceneState &) const = default;
};
struct WelcomeState {
    QString title, detail;
    QSize viewport;
    bool sourceEmpty = true, previewEmpty = true;
    bool operator==(const WelcomeState &) const = default;
};
struct MinimapState {
    qint64 preview = 0;
    QSize viewport;
    QPointF pan;
    qreal zoom = 1;
    qreal dpr = 1;
    int rotation = 0;
    bool mirrorX = false, mirrorY = false;
    bool operator==(const MinimapState &) const = default;
};

struct ImageNodes : QSGTransformNode {
    QSGSimpleTextureNode *checker = new QSGSimpleTextureNode;
    QSGSimpleTextureNode *image = new QSGSimpleTextureNode;
    std::array<QSGSimpleTextureNode *, 3> remainder {{nullptr, nullptr, nullptr}};
    QSGSimpleTextureNode *detail = nullptr;
    qint64 key = 0;
    qint64 detailKey = 0;
    ImageNodes() { appendChildNode(checker); appendChildNode(image); }
};
struct CanvasNodes : QSGNode {
    ImageNodes *first = new ImageNodes;
    ImageNodes *second = nullptr;
    CanvasNodes() { appendChildNode(first); }
};

// C++ scene graph item: decoding never runs here. Texture uploads happen only
// when the QImage changes; pan/zoom/rotation update GPU transform nodes only.
class ImageItem final : public QQuickItem {
public:
    std::shared_ptr<ActivityCounters> activity;
    QImage first;
    QImage second;
    QImage firstDetail;
    QImage secondDetail;
    QRect firstDetailRect;
    QRect secondDetailRect;
    QSize originalFirst;
    QSize originalSecond;
    QPointF pan;
    qreal scale = 1;
    int rotation = 0;
    bool mirrorX = false;
    bool mirrorY = false;
    bool spread = false;
    bool secondaryPending = false;
    bool rightToLeft = false;
    bool smooth = true;
    std::function<void(QDragEnterEvent *)> dragEntered;
    std::function<void(QDragMoveEvent *)> dragMoved;
    std::function<void(QDragLeaveEvent *)> dragLeft;
    std::function<void(QDropEvent *)> dropped;
    ImageItem() { setFlag(ItemHasContents); setFlag(ItemAcceptsDrops); setClip(true); }
protected:
    void dragEnterEvent(QDragEnterEvent *event) override { if (dragEntered) dragEntered(event); else event->ignore(); }
    void dragMoveEvent(QDragMoveEvent *event) override { if (dragMoved) dragMoved(event); else event->ignore(); }
    void dragLeaveEvent(QDragLeaveEvent *event) override { if (dragLeft) dragLeft(event); else event->ignore(); }
    void dropEvent(QDropEvent *event) override { if (dropped) dropped(event); else event->ignore(); }
    QSGNode *updatePaintNode(QSGNode *oldNode, UpdatePaintNodeData *) override
    {
        if (activity) activity->sceneUpdates.fetch_add(1, std::memory_order_relaxed);
        if (first.isNull() || !window()) { delete oldNode; return nullptr; }
        auto *root = static_cast<CanvasNodes *>(oldNode);
        if (!root) root = new CanvasNodes;
        const bool showSecond = spread && (!second.isNull() || secondaryPending);
        const bool secondPixelsReady = showSecond && !second.isNull();
        const bool attachSecond = secondPixelsReady && !root->second;
        if (attachSecond) {
            root->second = new ImageNodes;
        } else if (!secondPixelsReady && root->second) {
            root->removeChildNode(root->second);
            delete root->second;
            root->second = nullptr;
        }
        const QSizeF a = rotation % 2 ? QSizeF(originalFirst.height(), originalFirst.width()) : QSizeF(originalFirst);
        const QSizeF b = rotation % 2 ? QSizeF(originalSecond.height(), originalSecond.width()) : QSizeF(originalSecond);
        const qreal totalWidth = showSecond ? a.width() + b.width() + spreadGap : a.width();
        const auto sync = [&](ImageNodes *node, const QImage &pixels, const QImage &detailPixels, const QRect &detailRect, QSize original, qreal offset, bool visible) {
            if (!visible) {
                QMatrix4x4 hidden;
                hidden.scale(0);
                node->setMatrix(hidden);
                return;
            }
            if (node->key != pixels.cacheKey()) {
                // These nodes borrow the preview texture. Detach them while
                // that texture is still alive: material batching may compare
                // sibling textures during setTexture()/markDirty().
                for (auto *&part : node->remainder) if (part) { node->removeChildNode(part); delete part; part = nullptr; }
                if (activity) activity->previewUploads.fetch_add(1, std::memory_order_relaxed);
                QSGTexture *texture = window()->createTextureFromImage(pixels,
                    pixels.hasAlphaChannel() ? QQuickWindow::CreateTextureOptions{} : QQuickWindow::TextureIsOpaque);
                node->image->setTexture(texture);
                node->image->setOwnsTexture(true);
                node->key = pixels.cacheKey();
            }
            const bool hasDetail = !detailPixels.isNull() && !detailRect.isEmpty();
            const bool attachDetail = hasDetail && !node->detail;
            if (attachDetail) node->detail = new QSGSimpleTextureNode;
            if (!hasDetail && node->detail) { node->removeChildNode(node->detail); delete node->detail; node->detail = nullptr; node->detailKey = 0; }
            if (hasDetail && node->detailKey != detailPixels.cacheKey()) {
                if (activity) activity->detailUploads.fetch_add(1, std::memory_order_relaxed);
                node->detail->setTexture(window()->createTextureFromImage(detailPixels));
                node->detail->setOwnsTexture(true);
                node->detailKey = detailPixels.cacheKey();
            }
            if (!node->checker->texture()) {
                QImage pattern(32, 32, QImage::Format_RGB32);
                pattern.fill(QColor("#282b30"));
                QPainter checker(&pattern);
                checker.fillRect(0, 0, 16, 16, QColor("#34373c"));
                checker.fillRect(16, 16, 16, 16, QColor("#34373c"));
                checker.end();
                if (activity) activity->checkerUploads.fetch_add(1, std::memory_order_relaxed);
                auto *texture = window()->createTextureFromImage(pattern, QQuickWindow::TextureIsOpaque);
                texture->setHorizontalWrapMode(QSGTexture::Repeat);
                texture->setVerticalWrapMode(QSGTexture::Repeat);
                node->checker->setTexture(texture);
                node->checker->setOwnsTexture(true);
                node->checker->setFiltering(QSGTexture::Nearest);
            }
            const QRectF imageRect(-original.width() / 2.0, -original.height() / 2.0, original.width(), original.height());
            node->image->setFiltering(smooth ? QSGTexture::Linear : QSGTexture::Nearest);
            if (hasDetail) {
                // Leave a hole in the preview below the detail layer. Drawing
                // both layers over one another would double-composite alpha.
                const QRectF roi = detailRect;
                node->detail->setRect(roi.translated(-original.width() / 2.0, -original.height() / 2.0));
                node->detail->setFiltering(smooth ? QSGTexture::Linear : QSGTexture::Nearest);
                if (attachDetail) node->appendChildNode(node->detail);
                const std::array<QRectF, 4> strips {{
                    QRectF(0, 0, original.width(), roi.top()),
                    QRectF(0, roi.bottom(), original.width(), original.height() - roi.bottom()),
                    QRectF(0, roi.top(), roi.left(), roi.height()),
                    QRectF(roi.right(), roi.top(), original.width() - roi.right(), roi.height())
                }};
                for (int i = 0; i < 4; ++i) {
                    auto *part = node->image;
                    const bool attachPart = i && !node->remainder[i - 1];
                    if (i) {
                        if (attachPart) node->remainder[i - 1] = new QSGSimpleTextureNode;
                        part = node->remainder[i - 1];
                        if (part->texture() != node->image->texture()) part->setTexture(node->image->texture());
                    }
                    part->setFiltering(smooth ? QSGTexture::Linear : QSGTexture::Nearest);
                    part->setRect(strips[i].translated(-original.width() / 2.0, -original.height() / 2.0));
                    part->setSourceRect(strips[i].x() * pixels.width() / original.width(), strips[i].y() * pixels.height() / original.height(),
                        strips[i].width() * pixels.width() / original.width(), strips[i].height() * pixels.height() / original.height());
                    if (attachPart) node->insertChildNodeBefore(part, node->detail);
                }
            } else {
                node->image->setRect(imageRect); node->image->setSourceRect(QRectF(QPointF(), pixels.size()));
                for (auto *&part : node->remainder) if (part) { node->removeChildNode(part); delete part; part = nullptr; }
            }
            node->checker->setRect(imageRect);
            node->checker->setSourceRect(0, 0, original.width(), original.height());
            QMatrix4x4 matrix;
            matrix.translate(float(width() / 2 + pan.x()), float(height() / 2 + pan.y()));
            matrix.scale(float(scale));
            matrix.translate(float(offset), 0);
            matrix.rotate(float(rotation * 90), 0, 0, 1);
            matrix.scale(float(mirrorX ? -1 : 1), float(mirrorY ? -1 : 1));
            node->setMatrix(matrix);
        };
        const qreal firstOffset = !showSecond ? 0 : rightToLeft
            ? totalWidth / 2 - a.width() / 2 : -totalWidth / 2 + a.width() / 2;
        const qreal secondOffset = rightToLeft
            ? -totalWidth / 2 + b.width() / 2 : totalWidth / 2 - b.width() / 2;
        sync(root->first, first, firstDetail, firstDetailRect, originalFirst, firstOffset, true);
        if (root->second) {
            sync(root->second, second, secondDetail, secondDetailRect, originalSecond, secondOffset, true);
            // The software renderer visits inserted nodes immediately. Every
            // texture must be initialized before a branch becomes observable.
            if (attachSecond) root->appendChildNode(root->second);
        }
        return root;
    }
};

class Overlay final : public QQuickPaintedItem {
public:
    std::shared_ptr<ActivityCounters> activity;
    std::function<void(QPainter &)> draw;
    explicit Overlay(QQuickItem *parent) : QQuickPaintedItem(parent)
    {
        setAntialiasing(true);
        setAcceptedMouseButtons(Qt::NoButton);
        setZ(10);
    }
    void paint(QPainter *painter) override
    {
        if (activity) activity->overlayPaints.fetch_add(1, std::memory_order_relaxed);
        painter->setRenderHint(QPainter::Antialiasing);
        if (draw) draw(*painter);
    }
};
}

struct ImageCanvas::Impl {
    std::shared_ptr<ActivityCounters> activity = std::make_shared<ActivityCounters>();
    QImage image;
    QImage secondary;
    QImage minimap;
    bool minimapDirty = true;
    QQuickWindow *surface = nullptr;
    QWidget *container = nullptr;
    ImageItem *scene = nullptr;
    Overlay *welcome = nullptr;
    Overlay *minimapOverlay = nullptr;
    Overlay *warning = nullptr;
    Overlay *dropHint = nullptr;
    bool dropHovered = false;
    const QMimeData *dragMime = nullptr;
    QStringList dragPaths;
    QString statusTitle;
    QString statusDetail;
    bool secondaryPending = false;
    bool draggingMinimap = false;
    QImage displayFirst;
    QImage displaySecond;
    bool primaryPreview = false;
    bool secondaryPreview = false;
    PreparedPage preparedFirst;
    PreparedPage preparedSecond;
    std::optional<PreparationRequest> preparedRequest;
    std::optional<PreparationRequest> wantedRequest;
    std::optional<PreparationRequest> activeRequest;
    std::optional<PreparationRequest> pendingRequest;
    QThreadPool preparationPool;
    QFutureWatcher<PreparedFrame> *preparationWatcher = nullptr;
    std::shared_ptr<std::atomic<quint64>> preparationEpoch = std::make_shared<std::atomic<quint64>>(0);
    quint64 preparationSerial = 0;
    bool preparing = false;
    QImage pendingPlaceholder;
    std::optional<SceneState> sceneState;
    std::optional<WelcomeState> welcomeState;
    std::optional<MinimapState> minimapState;
    int warningState = -1;
    Impl() {
        preparationPool.setMaxThreadCount(1);
        preparationPool.setExpiryTimeout(10000);
        pendingPlaceholder = QImage(1, 1, QImage::Format_RGB32);
        pendingPlaceholder.fill(QColor("#172029"));
    }
    FitMode mode = FitMode::Fit;
    double magnification = 1.0;
    bool manualZoom = false;
    bool locked = false;
    bool smooth = true;
    ImageFilterMode filter = ImageFilterMode::Soften;
    bool spread = false;
    bool rightToLeft = false;
    int rotation = 0;
    bool mirrorX = false;
    bool mirrorY = false;
    double gamma = 1.0;
    bool inverted = false;
    QColor background{QStringLiteral("#101318")};
    QPointF pan;
    QPointF dragPosition;
    bool dragging = false;
    double wheelRemainder = 0.0;
    bool trackpadNavigationLatched = false;
    double nativeRotation = 0;

    QRectF minimapThumb(const QSizeF &viewport) const {
        const QRectF outer(viewport.width() - 184, viewport.height() - 146, 164, 126);
        const auto size = orientedSize(image, rotation).scaled(152, 100, Qt::KeepAspectRatio);
        QRectF thumb(QPointF(), size);
        thumb.moveCenter(QPointF(outer.center().x(), outer.top() + 55));
        return thumb;
    }
    bool minimapVisible(const QSizeF &viewport, qreal dpr) const {
        const auto displayed = displaySize(dpr);
        return !image.isNull() && !displayFirst.isNull() && !hasSecond() && viewport.width() > 360 && viewport.height() > 260
            && (displayed.width() > viewport.width() + 1 || displayed.height() > viewport.height() + 1);
    }
    void moveMinimap(const QPointF &position, const QSizeF &viewport, qreal dpr) {
        const auto thumb = minimapThumb(viewport);
        const auto displayed = displaySize(dpr);
        const double x = std::clamp((position.x() - thumb.left()) / thumb.width(), 0.0, 1.0);
        const double y = std::clamp((position.y() - thumb.top()) / thumb.height(), 0.0, 1.0);
        pan = QPointF((0.5 - x) * displayed.width(), (0.5 - y) * displayed.height());
        clampPan(viewport, dpr);
    }

    PreparationRequest makeRequest(const QSizeF &viewport, qreal dpr) const {
        PreparationRequest request;
        request.gamma = gamma; request.inverted = inverted; request.filter = filter;
        request.epoch = preparationEpoch->load();
        const QSizeF a = orientedSize(image, rotation);
        const QSizeF b = rotation % 2 ? QSizeF(secondSize().height(), secondSize().width()) : QSizeF(secondSize());
        const double total = hasSecond() ? a.width() + b.width() + spreadGap : a.width();
        const double firstOffset = !hasSecond() ? 0 : rightToLeft ? total / 2 - a.width() / 2 : -total / 2 + a.width() / 2;
        const double secondOffset = rightToLeft ? -total / 2 + b.width() / 2 : total / 2 - b.width() / 2;
        const auto page = [&](const QImage &source, const PreparedPage &prior, const PageRequest *priorRequest, double offset) {
            PageRequest result; result.source = source;
            if (source.isNull()) return result;
            const bool sameBase = preparedRequest && priorRequest && priorRequest->source.cacheKey() == source.cacheKey()
                && preparedRequest->gamma == gamma && preparedRequest->inverted == inverted && preparedRequest->filter == filter;
            if (sameBase) result.cachedPreview = prior.preview;
            const QSize previewSize = boundedSize(source.size(), previewPixelBudget);
            const double previewScale = std::min(double(previewSize.width()) / source.width(), double(previewSize.height()) / source.height());
            if (previewSize == source.size() || magnification <= previewScale * 1.02) return result;
            QTransform matrix;
            matrix.translate(viewport.width() / 2 + pan.x(), viewport.height() / 2 + pan.y());
            matrix.scale(logicalScale(dpr), logicalScale(dpr));
            matrix.translate(offset, 0); matrix.rotate(rotation * 90);
            matrix.scale(mirrorX ? -1 : 1, mirrorY ? -1 : 1);
            matrix.translate(-source.width() / 2.0, -source.height() / 2.0);
            bool invertible = false;
            const auto inverse = matrix.inverted(&invertible);
            if (!invertible) return result;
            const QRect visible = inverse.mapRect(QRectF(QPointF(), viewport)).toAlignedRect().intersected(source.rect());
            if (visible.isEmpty()) return result;
            const double requiredScale = std::min(1.0, magnification);
            if (sameBase && !prior.detail.isNull() && !prior.limited && prior.detailRect.contains(visible)
                && double(prior.detail.width()) / prior.detailRect.width() >= requiredScale * 0.995
                && double(prior.detail.height()) / prior.detailRect.height() >= requiredScale * 0.995) {
                result.detailRect = prior.detailRect; result.detailSize = prior.detail.size();
                result.cachedDetail = prior.detail;
                return result;
            }
            constexpr int quantum = 128;
            const auto region = [&](double guard) {
                const int marginX = std::max(24, int(visible.width() * guard));
                const int marginY = std::max(24, int(visible.height() * guard));
                const int left = std::max(0, (visible.left() - marginX) / quantum * quantum);
                const int top = std::max(0, (visible.top() - marginY) / quantum * quantum);
                const int right = std::min(source.width(), (visible.right() + marginX + 1 + quantum) / quantum * quantum);
                const int bottom = std::min(source.height(), (visible.bottom() + marginY + 1 + quantum) / quantum * quantum);
                return QRect(left, top, right - left, bottom - top);
            };
            const auto resolution = [&](const QRect &rect) {
                return QSize(std::max(1, int(std::ceil(rect.width() * requiredScale))),
                             std::max(1, int(std::ceil(rect.height() * requiredScale))));
            };
            const auto withinBudget = [&](const QRect &rect) {
                const QSize pixels = resolution(rect);
                return pixels.width() <= previewLimit && pixels.height() <= previewLimit
                    && qint64(pixels.width()) * pixels.height() <= detailPixelBudget;
            };
            // Keep nearby source pixels ready so a continuous pan normally
            // changes only the GPU matrix. Spend spare detail budget on this
            // guard band without reducing visible source-pixel resolution.
            result.detailRect = region(0);
            // Padding must not lower visible source-pixel resolution when the
            // viewport itself still fits the detail budget.
            if (!withinBudget(result.detailRect) && withinBudget(visible)) result.detailRect = visible;
            if (withinBudget(result.detailRect)) {
                double low = 0, high = 0.25;
                for (int i = 0; i < 8; ++i) {
                    const double mid = (low + high) / 2;
                    const QRect candidate = region(mid);
                    if (withinBudget(candidate)) { low = mid; result.detailRect = candidate; }
                    else high = mid;
                }
            }
            const QSize desired = resolution(result.detailRect);
            result.detailSize = boundedSize(desired, detailPixelBudget);
            result.limited = result.detailSize != desired;
            if (sameBase && prior.detailRect == result.detailRect && prior.detail.size() == result.detailSize)
                result.cachedDetail = prior.detail;
            return result;
        };
        request.first = page(image, preparedFirst, preparedRequest ? &preparedRequest->first : nullptr, firstOffset);
        request.second = page(secondary, preparedSecond, preparedRequest ? &preparedRequest->second : nullptr, secondOffset);
        return request;
    }

    bool hasSecond() const { return spread && (!secondary.isNull() || secondaryPending); }
    QSize secondSize() const { return secondary.isNull() && secondaryPending ? image.size() : secondary.size(); }
    QSizeF contentSize() const
    {
        const auto first = orientedSize(image, rotation);
        if (!hasSecond()) return first;
        const auto second = rotation % 2 ? QSizeF(secondSize().height(), secondSize().width()) : QSizeF(secondSize());
        return {first.width() + second.width() + spreadGap,
                std::max(first.height(), second.height())};
    }
    double logicalScale(qreal dpr) const { return magnification / dpr; }
    QSizeF displaySize(qreal dpr) const { return contentSize() * logicalScale(dpr); }
    void clampPan(const QSizeF &viewport, qreal dpr)
    {
        const auto displayed = displaySize(dpr);
        const qreal xLimit = displayed.width() > viewport.width()
            ? (displayed.width() - viewport.width()) / 2.0 + imageMargin : 0.0;
        const qreal yLimit = displayed.height() > viewport.height()
            ? (displayed.height() - viewport.height()) / 2.0 + imageMargin : 0.0;
        pan.setX(std::clamp(pan.x(), -xLimit, xLimit));
        pan.setY(std::clamp(pan.y(), -yLimit, yLimit));
    }
    void fit(const QSizeF &viewport, qreal dpr)
    {
        const auto size = contentSize();
        if (size.isEmpty() || viewport.isEmpty()) return;
        const auto available = QSizeF(std::max(1.0, viewport.width() - 2 * imageMargin),
                                     std::max(1.0, viewport.height() - 2 * imageMargin));
        const double fitScale = std::min(available.width() / size.width(),
                                         available.height() / size.height()) * dpr;
        switch (mode) {
        case FitMode::Fit: magnification = fitScale; break;
        case FitMode::Width: magnification = available.width() / size.width() * dpr; break;
        case FitMode::Actual: magnification = 1.0; break;
        case FitMode::Shrink: magnification = std::min(1.0, fitScale); break;
        }
        magnification = std::clamp(magnification, minZoom, maxZoom);
        pan = {};
        if (mode == FitMode::Width) {
            const double displayedHeight = displaySize(dpr).height();
            if (displayedHeight > viewport.height())
                pan.setY((displayedHeight - viewport.height()) / 2.0 + imageMargin);
        }
        clampPan(viewport, dpr);
    }
    void zoomAt(double value, const QPointF &anchor, const QSizeF &viewport, qreal dpr)
    {
        value = std::clamp(value, minZoom, maxZoom);
        const QPointF center(viewport.width() / 2, viewport.height() / 2);
        const double ratio = value / magnification;
        pan = anchor - center - (anchor - center - pan) * ratio;
        magnification = value;
        manualZoom = true;
        clampPan(viewport, dpr);
    }
};

ImageCanvas::ImageCanvas(QWidget *parent)
    : QWidget(parent), d(std::make_unique<Impl>())
{
    setAcceptDrops(true);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    setMinimumSize(160, 120);
    setObjectName(QStringLiteral("imageCanvas"));
    setAccessibleName(QStringLiteral("이미지 보기 영역"));
    d->surface = new QQuickWindow;
    d->surface->setObjectName(QStringLiteral("imageSurface"));
    d->surface->setProperty("ikviewCanvas", QVariant::fromValue(static_cast<QObject *>(this)));
    d->surface->setColor(d->background);
    d->surface->setPersistentGraphics(true);
    d->surface->setPersistentSceneGraph(true);
    d->surface->installEventFilter(this);
    d->container = QWidget::createWindowContainer(d->surface, this);
    d->container->setObjectName(QStringLiteral("imageContainer"));
    d->container->setAcceptDrops(true);
    d->container->installEventFilter(this);
    d->container->setFocusPolicy(Qt::NoFocus);
    d->container->setMouseTracking(true);
    d->scene = new ImageItem;
    d->scene->activity = d->activity;
    d->scene->setParentItem(d->surface->contentItem());
    d->scene->dragEntered = [this](QDragEnterEvent *event) { dragEnterEvent(event); };
    d->scene->dragMoved = [this](QDragMoveEvent *event) { dragMoveEvent(event); };
    d->scene->dragLeft = [this](QDragLeaveEvent *event) { dragLeaveEvent(event); };
    d->scene->dropped = [this](QDropEvent *event) { dropEvent(event); };
    const auto makeOverlay = [this] {
        auto *overlay = new Overlay(d->scene);
        overlay->activity = d->activity;
        overlay->draw = [this, overlay](QPainter &painter) {
            painter.translate(-overlay->x(), -overlay->y());
            paintOverlay(painter);
        };
        return overlay;
    };
    d->welcome = makeOverlay();
    d->minimapOverlay = makeOverlay();
    d->warning = makeOverlay();
    d->dropHint = new Overlay(d->scene);
    d->dropHint->activity = d->activity;
    d->dropHint->setZ(20);
    d->dropHint->draw = [this](QPainter &painter) {
        const bool dark = qGray(d->background.rgb()) < 128;
        const QRectF area(2, 2, d->dropHint->width() - 4, d->dropHint->height() - 4);
        painter.setPen(QPen(QColor(dark ? "#70ddc5" : "#137c67"), 1.5));
        painter.setBrush(dark ? QColor(17, 47, 45, 245) : QColor(225, 244, 239, 245));
        painter.drawRoundedRect(area, 12, 12);
        QFont label = font(); label.setPixelSize(17); label.setWeight(QFont::DemiBold);
        painter.setFont(label); painter.setPen(QColor(dark ? "#bdf6e5" : "#075747"));
        painter.drawText(area.adjusted(12, 12, -12, -38), Qt::AlignCenter, QStringLiteral("놓아서 이미지 열기"));
        label.setPixelSize(12); label.setWeight(QFont::Normal); painter.setFont(label);
        painter.setPen(QColor(dark ? "#94b9ad" : "#426e61"));
        painter.drawText(area.adjusted(12, 48, -12, -10), Qt::AlignCenter, QStringLiteral("이미지 · PDF · 폴더 · 압축파일"));
    };
    syncScene();
}

ImageCanvas::~ImageCanvas()
{
    d->preparationEpoch->fetch_add(1);
    if (d->preparationWatcher) disconnect(d->preparationWatcher, nullptr, this, nullptr);
    d->pendingRequest.reset();
    d->preparationPool.clear();
    d->preparationPool.waitForDone();
    // The container owns the native Metal window and its scene graph. Destroy it
    // while the data read by the painted overlay callbacks is still alive.
    delete d->container;
    d->container = nullptr;
    d->surface = nullptr;
    d->scene = nullptr;
}

void ImageCanvas::setImage(const QImage &image, bool resetViewTransform)
{
    if (!resetViewTransform && image.cacheKey() == d->image.cacheKey()
        && (!image.isNull() || (d->secondary.isNull() && !d->secondaryPending))) return;
    const bool sizeChanged = d->image.size() != image.size();
    if (resetViewTransform) {
        if (d->gamma != 1.0 || d->inverted) d->preparationEpoch->fetch_add(1);
        d->rotation = 0; d->mirrorX = d->mirrorY = false;
        d->gamma = 1.0; d->inverted = false;
        d->pan = {}; d->secondary = {}; d->secondaryPending = false;
        if (!d->locked) d->manualZoom = false;
    }
    if (sizeChanged || image.isNull()) {
        d->preparationEpoch->fetch_add(1);
        d->preparedRequest.reset(); d->wantedRequest.reset(); d->pendingRequest.reset();
        d->preparedFirst = {}; d->preparedSecond = {};
        d->displayFirst = {}; d->displaySecond = {};
        d->primaryPreview = d->secondaryPreview = false;
    }
    d->image = image;
    d->minimapDirty = true;
    if (image.isNull()) {
        d->secondary = {}; d->secondaryPending = false;
        d->pan = {};
        if (resetViewTransform) emit zoomChanged(d->magnification);
    } else if ((sizeChanged || resetViewTransform) && !d->locked) {
        d->manualZoom = false;
        d->fit(size(), devicePixelRatioF());
        emit zoomChanged(d->magnification);
    } else {
        d->clampPan(size(), devicePixelRatioF());
        if (resetViewTransform) emit zoomChanged(d->magnification);
    }
    syncScene();
}

void ImageCanvas::setSecondaryImage(const QImage &image)
{
    if (image.cacheKey() == d->secondary.cacheKey() && (image.isNull() || !d->secondaryPending)) return;
    const bool changedSize = d->secondary.size() != image.size();
    d->secondary = image;
    if (!image.isNull()) d->secondaryPending = false;
    if (changedSize && !d->manualZoom && !d->locked) {
        d->fit(size(), devicePixelRatioF());
        emit zoomChanged(d->magnification);
    }
    d->clampPan(size(), devicePixelRatioF());
    syncScene();
}

void ImageCanvas::setSecondaryPending(bool pending) {
    if (d->secondaryPending == pending) return;
    d->secondaryPending = pending;
    if (!d->manualZoom && !d->locked) { d->fit(size(), devicePixelRatioF()); emit zoomChanged(d->magnification); }
    d->clampPan(size(), devicePixelRatioF());
    syncScene();
}

void ImageCanvas::setStatusMessage(const QString &title, const QString &detail) {
    if (d->statusTitle == title && d->statusDetail == detail) return;
    d->statusTitle = title; d->statusDetail = detail; syncScene();
}

void ImageCanvas::setSpread(bool enabled, bool rightToLeft)
{
    if (d->spread == enabled && d->rightToLeft == rightToLeft) return;
    d->spread = enabled;
    d->rightToLeft = rightToLeft;
    if (!d->manualZoom && !d->locked) {
        d->fit(size(), devicePixelRatioF());
        emit zoomChanged(d->magnification);
    }
    d->clampPan(size(), devicePixelRatioF());
    syncScene();
}

void ImageCanvas::setFitMode(FitMode mode)
{
    d->mode = mode;
    d->manualZoom = false;
    d->fit(size(), devicePixelRatioF());
    emit zoomChanged(d->magnification);
    syncScene();
}

FitMode ImageCanvas::fitMode() const { return d->mode; }
double ImageCanvas::zoom() const { return d->magnification; }
bool ImageCanvas::isManualZoom() const { return d->manualZoom || d->locked; }

void ImageCanvas::zoomBy(double factor)
{
    if (!std::isfinite(factor) || factor <= 0) return;
    setZoom(d->magnification * factor);
}

void ImageCanvas::setZoom(double factor)
{
    if (!std::isfinite(factor) || factor <= 0) return;
    d->zoomAt(factor, rect().center(), size(), devicePixelRatioF());
    emit zoomChanged(d->magnification);
    syncScene();
}

void ImageCanvas::setZoomLocked(bool locked) {
    if (d->locked == locked) return;
    d->locked = locked;
    if (!locked && !d->manualZoom) d->fit(size(), devicePixelRatioF());
    d->clampPan(size(), devicePixelRatioF());
    emit zoomChanged(d->magnification);
    syncScene();
}
bool ImageCanvas::zoomLocked() const { return d->locked; }
CanvasViewState ImageCanvas::viewState() const { return {d->mode, d->magnification, d->manualZoom, d->locked}; }
void ImageCanvas::restoreViewState(const CanvasViewState &state) {
    d->mode = static_cast<FitMode>(std::clamp(static_cast<int>(state.fitMode), 0, 3));
    d->locked = state.zoomLocked;
    d->manualZoom = state.manualZoom;
    d->magnification = std::isfinite(state.zoom) && state.zoom > 0 ? std::clamp(state.zoom, minZoom, maxZoom) : 1.0;
    if (!d->manualZoom && !d->locked) d->fit(size(), devicePixelRatioF());
    d->clampPan(size(), devicePixelRatioF());
    emit zoomChanged(d->magnification);
    syncScene();
}
bool ImageCanvas::canPan() const {
    const auto displayed = d->displaySize(devicePixelRatioF());
    return displayed.width() > width() + 1 || displayed.height() > height() + 1;
}
bool ImageCanvas::panViewport(const QPointF &delta) {
    if (!canPan() || !std::isfinite(delta.x()) || !std::isfinite(delta.y())) return false;
    const QPointF before = d->pan;
    d->pan += delta;
    d->clampPan(size(), devicePixelRatioF());
    if (d->pan == before) return false;
    syncScene();
    return true;
}
bool ImageCanvas::panViewportPage(int horizontalPages, int verticalPages) {
    return panViewport(QPointF(horizontalPages * width() * 0.85, verticalPages * height() * 0.85));
}

void ImageCanvas::rotate(int quarterTurns)
{
    d->rotation = ((d->rotation + quarterTurns) % 4 + 4) % 4;
    if (!d->manualZoom && !d->locked) d->fit(size(), devicePixelRatioF());
    d->clampPan(size(), devicePixelRatioF());
    d->minimapDirty = true;
    emit zoomChanged(d->magnification);
    syncScene();
}

void ImageCanvas::flipHorizontal() {
    if (d->rotation % 2) d->mirrorY = !d->mirrorY; else d->mirrorX = !d->mirrorX;
    d->minimapDirty = true; syncScene();
}
void ImageCanvas::flipVertical() {
    if (d->rotation % 2) d->mirrorX = !d->mirrorX; else d->mirrorY = !d->mirrorY;
    d->minimapDirty = true; syncScene();
}

void ImageCanvas::resetTransform()
{
    d->rotation = 0;
    d->mirrorX = d->mirrorY = false;
    if (d->gamma != 1.0 || d->inverted) d->preparationEpoch->fetch_add(1);
    d->gamma = 1.0;
    d->inverted = false;
    d->minimapDirty = true;
    if (!d->locked) {
        d->manualZoom = false;
        d->fit(size(), devicePixelRatioF());
    }
    d->pan = {};
    emit zoomChanged(d->magnification);
    syncScene();
}

void ImageCanvas::setSmooth(bool enabled)
{
    setImageFilter(enabled ? (sharpFilter(d->filter) ? ImageFilterMode::SoftenSharpen : ImageFilterMode::Soften)
        : (sharpFilter(d->filter) ? ImageFilterMode::Sharpen : ImageFilterMode::None));
}
void ImageCanvas::setImageFilter(ImageFilterMode mode)
{
    if (mode < ImageFilterMode::None || mode > ImageFilterMode::SoftenSharpen) mode = ImageFilterMode::Soften;
    if (d->filter == mode) return;
    d->filter = mode; d->smooth = softFilter(mode);
    d->preparationEpoch->fetch_add(1); d->minimapDirty = true;
    syncScene();
}
ImageFilterMode ImageCanvas::imageFilter() const { return d->filter; }
QImage ImageCanvas::applyDisplayFilter(const QImage &image, ImageFilterMode mode) { return sharpened(image,mode); }
void ImageCanvas::setGamma(double gamma)
{
    if (!std::isfinite(gamma)) return;
    gamma = std::clamp(gamma, 0.1, 5.0);
    if (d->gamma == gamma) return;
    d->preparationEpoch->fetch_add(1);
    d->gamma = gamma;
    d->minimapDirty = true;
    syncScene();
}
void ImageCanvas::setInverted(bool inverted) {
    if (d->inverted == inverted) return;
    d->preparationEpoch->fetch_add(1);
    d->inverted = inverted; d->minimapDirty = true; syncScene();
}
void ImageCanvas::setBackgroundColor(const QColor &color)
{
    if (!color.isValid() || color == d->background) return;
    d->background = color;
    d->surface->setColor(color);
    d->pendingPlaceholder.fill(qGray(color.rgb()) < 128 ? QColor("#172029") : QColor("#dce4eb"));
    // Overlay textures are cached independently of the canvas background. A
    // theme transition invalidates their colors once, without touching source
    // images, prepared previews/details or the normal pan/zoom update path.
    d->welcomeState.reset();
    d->minimapState.reset();
    d->warningState = -1;
    if (d->dropHint->isVisible()) d->dropHint->update();
    // A plain image has no CPU-painted overlay to recolor. Its window clear
    // color can change without synchronizing the image scene at all. Hidden
    // overlays retain their invalid state until they are next exposed.
    const bool visibleOverlay = d->welcome->isVisible() || d->minimapOverlay->isVisible() || d->warning->isVisible();
    const bool placeholderVisible = d->spread && d->secondaryPending && d->secondary.isNull();
    if (visibleOverlay || placeholderVisible) syncScene();
}

ImageTransformState ImageCanvas::transformState() const { return {d->rotation, d->mirrorX, d->mirrorY, d->gamma, d->inverted}; }
QImage ImageCanvas::transformedImage() const { return applyTransform(d->image, transformState()); }

QImage ImageCanvas::applyTransform(const QImage &image, const ImageTransformState &state)
{
    QImage result = image;
    if (result.isNull()) return {};
    if (state.mirrorX || state.mirrorY) result = mirroredImage(result, state.mirrorX, state.mirrorY);
    if (state.quarterTurns) {
        QTransform transform;
        transform.rotate(state.quarterTurns * 90);
        result = result.transformed(transform, Qt::FastTransformation);
    }
    if (state.inverted || std::abs(state.gamma - 1.0) > 0.0001) {
        std::array<uchar, 256> lookup{};
        for (int i = 0; i < 256; ++i) {
            const double value = (state.inverted ? 255 - i : i) / 255.0;
            lookup[i] = static_cast<uchar>(std::clamp(std::lround(std::pow(value, 1.0 / std::clamp(state.gamma, 0.1, 5.0)) * 255), 0L, 255L));
        }
        result = result.convertToFormat(QImage::Format_RGBA8888);
        for (int y = 0; y < result.height(); ++y) {
            uchar *row = result.scanLine(y);
            for (int x = 0; x < result.width(); ++x) {
                row[4 * x] = lookup[row[4 * x]];
                row[4 * x + 1] = lookup[row[4 * x + 1]];
                row[4 * x + 2] = lookup[row[4 * x + 2]];
            }
        }
    }
    return result;
}

QImage ImageCanvas::viewportImage() { return d->surface->grabWindow(); }

QString ImageCanvas::rendererDescription() const
{
    QString api;
    switch (d->surface->rendererInterface()->graphicsApi()) {
    case QSGRendererInterface::Metal: api = QStringLiteral("Metal"); break;
    case QSGRendererInterface::OpenGL: api = QStringLiteral("OpenGL"); break;
    case QSGRendererInterface::Vulkan: api = QStringLiteral("Vulkan"); break;
    case QSGRendererInterface::Direct3D11: api = QStringLiteral("Direct3D 11"); break;
    case QSGRendererInterface::Software: api = QStringLiteral("Software"); break;
    default: api = QStringLiteral("Qt RHI"); break;
    }
    QString result = api + QStringLiteral(" · C++ scene graph");
    if (d->primaryPreview || d->secondaryPreview)
        result += QStringLiteral(" · 4MP 미리보기 / 가시 영역 8MP 상세");
    if (d->preparedFirst.limited || d->preparedSecond.limited) result += QStringLiteral(" · 상세 해상도 예산 제한");
    return result;
}

void ImageCanvas::resizeEvent(QResizeEvent *event)
{
    QWidget::resizeEvent(event);
    d->container->setGeometry(rect());
    d->scene->setSize(size());
    if (!d->manualZoom && !d->locked) {
        d->fit(size(), devicePixelRatioF());
        emit zoomChanged(d->magnification);
    }
    d->clampPan(size(), devicePixelRatioF());
    syncScene();
}

bool ImageCanvas::event(QEvent *event)
{
    if (event->type() == QEvent::NativeGesture && nativeGestureEvent(static_cast<QNativeGestureEvent *>(event))) return true;
    const bool handled = QWidget::event(event);
    if (event->type() == QEvent::DevicePixelRatioChange || event->type() == QEvent::ScreenChangeInternal) {
        if (!d->manualZoom && !d->locked) d->fit(size(), devicePixelRatioF());
        d->clampPan(size(), devicePixelRatioF());
        emit zoomChanged(d->magnification); syncScene();
    }
    return handled;
}

bool ImageCanvas::eventFilter(QObject *watched, QEvent *event)
{
    if (watched != d->surface && watched != d->container) return QWidget::eventFilter(watched, event);
    switch (event->type()) {
    case QEvent::MouseButtonPress: mousePressEvent(static_cast<QMouseEvent *>(event)); return event->isAccepted();
    case QEvent::MouseMove: mouseMoveEvent(static_cast<QMouseEvent *>(event)); return d->dragging;
    case QEvent::MouseButtonRelease: mouseReleaseEvent(static_cast<QMouseEvent *>(event)); return event->isAccepted();
    case QEvent::MouseButtonDblClick: mouseDoubleClickEvent(static_cast<QMouseEvent *>(event)); return event->isAccepted();
    case QEvent::Wheel: wheelEvent(static_cast<QWheelEvent *>(event)); return event->isAccepted();
    case QEvent::DragEnter: dragEnterEvent(static_cast<QDragEnterEvent *>(event)); return event->isAccepted();
    case QEvent::DragMove: dragMoveEvent(static_cast<QDragMoveEvent *>(event)); return event->isAccepted();
    case QEvent::DragLeave: dragLeaveEvent(static_cast<QDragLeaveEvent *>(event)); return true;
    case QEvent::Drop: dropEvent(static_cast<QDropEvent *>(event)); return event->isAccepted();
    case QEvent::NativeGesture: return nativeGestureEvent(static_cast<QNativeGestureEvent *>(event));
    case QEvent::ContextMenu: {
        auto *menu = static_cast<QContextMenuEvent *>(event);
        if (menu->reason() == QContextMenuEvent::Keyboard)
            emit customContextMenuRequested(rect().center());
        event->accept();
        return true;
    }
    case QEvent::WindowDeactivate:
    case QEvent::Hide:
        d->dragging = false;
        d->draggingMinimap = false;
        d->trackpadNavigationLatched = false; d->wheelRemainder = 0; d->nativeRotation = 0;
        d->dragMime = nullptr; d->dragPaths.clear();
        unsetCursor(); d->surface->unsetCursor();
        setDropHovered(false);
        return QWidget::eventFilter(watched, event);
    case QEvent::DevicePixelRatioChange:
        QTimer::singleShot(0, this, [this] {
            if (!d->manualZoom && !d->locked) d->fit(size(), devicePixelRatioF());
            d->clampPan(size(), devicePixelRatioF());
            emit zoomChanged(d->magnification); syncScene();
        });
        return QWidget::eventFilter(watched, event);
    default: return QWidget::eventFilter(watched, event);
    }
}

bool ImageCanvas::isPreparing() const { return d->preparing; }
CanvasRenderStats ImageCanvas::renderStats() const {
    const auto pixels = [](const QImage &image) { return qint64(image.width()) * image.height(); };
    return {pixels(d->preparedFirst.preview) + pixels(d->preparedSecond.preview),
            pixels(d->preparedFirst.detail) + pixels(d->preparedSecond.detail),
            d->preparedFirst.limited || d->preparedSecond.limited, d->preparing};
}

CanvasActivityStats ImageCanvas::activityStats() const {
    const auto &c = *d->activity;
    CanvasActivityStats stats;
    stats.sceneSyncCalls = c.sceneSyncCalls.load(std::memory_order_relaxed);
    stats.sceneUpdates = c.sceneUpdates.load(std::memory_order_relaxed);
    stats.preparationJobs = c.preparationJobs.load(std::memory_order_relaxed);
    stats.previewUploads = c.previewUploads.load(std::memory_order_relaxed);
    stats.detailUploads = c.detailUploads.load(std::memory_order_relaxed);
    stats.checkerUploads = c.checkerUploads.load(std::memory_order_relaxed);
    stats.textureUploads = stats.previewUploads + stats.detailUploads + stats.checkerUploads;
    stats.overlayPaints = c.overlayPaints.load(std::memory_order_relaxed);
    return stats;
}

void ImageCanvas::requestPreparation()
{
    auto request = d->makeRequest(size(), devicePixelRatioF());
    d->wantedRequest = request;
    const bool alreadyReady = d->preparedRequest && sameRequest(*d->preparedRequest, request);
    const bool waiting = !d->image.isNull() && !alreadyReady;
    if (waiting != d->preparing) { d->preparing = waiting; emit preparationChanged(waiting); }
    if (d->image.isNull()) {
        d->preparedFirst = {}; d->preparedSecond = {};
        d->displayFirst = {}; d->displaySecond = {}; d->pendingRequest.reset();
        return;
    }
    if (alreadyReady) { d->pendingRequest.reset(); return; }
    if (d->activeRequest && d->activeRequest->epoch == request.epoch && sameRequest(*d->activeRequest, request)) {
        d->pendingRequest.reset(); return;
    }
    if (d->pendingRequest && d->pendingRequest->epoch == request.epoch && sameRequest(*d->pendingRequest, request)) { startPreparation(); return; }
    request.serial = ++d->preparationSerial;
    d->pendingRequest = std::move(request);
    startPreparation();
}

void ImageCanvas::startPreparation()
{
    if (d->activeRequest || !d->pendingRequest) return;
    const PreparationRequest request = std::move(*d->pendingRequest);
    d->pendingRequest.reset();
    const auto apply = [this](PreparedFrame ready) {
        if (ready.request.epoch != d->preparationEpoch->load() || ready.first.preview.isNull()
            || ready.request.first.source.cacheKey() != d->image.cacheKey()
            || ready.request.second.source.cacheKey() != d->secondary.cacheKey()
            || (d->preparedRequest && ready.request.serial < d->preparedRequest->serial)) return;
        d->preparedFirst = std::move(ready.first); d->preparedSecond = std::move(ready.second);
        d->preparedRequest = std::move(ready.request);
        const bool previewChanged = d->displayFirst.cacheKey() != d->preparedFirst.preview.cacheKey();
        d->displayFirst = d->preparedFirst.preview; d->displaySecond = d->preparedSecond.preview;
        d->primaryPreview = d->displayFirst.size() != d->image.size();
        d->secondaryPreview = !d->secondary.isNull() && d->displaySecond.size() != d->secondary.size();
        d->minimapDirty = d->minimapDirty || previewChanged;
        emit framePrepared();
    };
    // Unmodified images within the preview budget require only QImage sharing,
    // even at 4MP. Keep pixel processing above 1MP off the UI thread.
    const qint64 sourcePixels = qint64(request.first.source.width()) * request.first.source.height()
        + qint64(request.second.source.width()) * request.second.source.height();
    const auto directPage = [](const PageRequest &page) {
        return page.source.isNull() || (page.detailRect.isEmpty() && boundedSize(page.source.size(), previewPixelBudget) == page.source.size());
    };
    const bool directFrame = request.gamma == 1.0 && !request.inverted && !sharpFilter(request.filter) && directPage(request.first) && directPage(request.second);
    if (directFrame || (!sharpFilter(request.filter) && sourcePixels <= 1024LL * 1024)) {
        apply(prepareFrame(request, d->preparationEpoch));
        const bool waiting = d->wantedRequest && (!d->preparedRequest || !sameRequest(*d->preparedRequest, *d->wantedRequest));
        if (d->preparing != waiting) { d->preparing = waiting; emit preparationChanged(waiting); }
        return;
    }
    d->activeRequest = request;
    auto *watcher = new QFutureWatcher<PreparedFrame>(this);
    d->preparationWatcher = watcher;
    connect(watcher, &QFutureWatcher<PreparedFrame>::finished, this, [this, watcher, apply] {
        const auto ready = watcher->result();
        d->preparationWatcher = nullptr; d->activeRequest.reset(); watcher->deleteLater();
        apply(ready);
        // Recompute the visible request against the newly prepared cache. The
        // single pending slot always replaces intermediate pan/resize requests.
        syncScene();
    });
    d->activity->preparationJobs.fetch_add(1, std::memory_order_relaxed);
    watcher->setFuture(QtConcurrent::run(&d->preparationPool, [request, epoch = d->preparationEpoch] {
        return prepareFrame(request, epoch);
    }));
}

void ImageCanvas::syncScene()
{
    d->activity->sceneSyncCalls.fetch_add(1, std::memory_order_relaxed);
    if (!d->scene) return;
    requestPreparation();
    const QImage second = d->secondaryPending && d->secondary.isNull() ? d->pendingPlaceholder : d->secondary.isNull() ? QImage() : d->displaySecond;
    const SceneState scene {
        d->displayFirst.cacheKey(), second.cacheKey(), d->preparedFirst.detail.cacheKey(), d->preparedSecond.detail.cacheKey(),
        d->preparedFirst.detailRect, d->preparedSecond.detailRect, d->image.size(), d->secondSize(), size(),
        d->pan, d->logicalScale(devicePixelRatioF()), d->rotation, d->mirrorX, d->mirrorY, d->spread,
        d->secondaryPending, d->rightToLeft, d->smooth
    };
    if (!d->sceneState || *d->sceneState != scene) {
        d->sceneState = scene;
        d->scene->first = d->displayFirst;
        d->scene->second = second;
        d->scene->firstDetail = d->preparedFirst.detail;
        d->scene->secondDetail = d->preparedSecond.detail;
        d->scene->firstDetailRect = d->preparedFirst.detailRect;
        d->scene->secondDetailRect = d->preparedSecond.detailRect;
        d->scene->secondaryPending = d->secondaryPending;
        d->scene->originalFirst = d->image.size();
        d->scene->originalSecond = d->secondSize();
        d->scene->pan = d->pan;
        d->scene->scale = d->logicalScale(devicePixelRatioF());
        d->scene->rotation = d->rotation;
        d->scene->mirrorX = d->mirrorX;
        d->scene->mirrorY = d->mirrorY;
        d->scene->spread = d->spread;
        d->scene->rightToLeft = d->rightToLeft;
        d->scene->smooth = d->smooth;
        d->scene->update();
    }
    const auto place = [](Overlay *item, const QRectF &rect, bool visible, bool contentChanged) {
        const bool resized = item->size() != rect.size();
        const bool exposed = visible && !item->isVisible();
        item->setPosition(rect.topLeft());
        if (resized) item->setSize(rect.size());
        item->setVisible(visible);
        if (visible && (resized || exposed || contentChanged)) item->update();
    };
    // CPU-painted overlays change only when their own visible content changes.
    // A detail upload does not repaint an unchanged minimap or welcome panel.
    const WelcomeState welcome { d->statusTitle, d->statusDetail, size(), d->image.isNull(), d->displayFirst.isNull() };
    const bool welcomeChanged = !d->welcomeState || *d->welcomeState != welcome;
    d->welcomeState = welcome;
    place(d->welcome, QRectF(width() / 2.0 - 240, height() / 2.0 - 190, 480, 340),
          (welcome.sourceEmpty || welcome.previewEmpty) && !d->dropHovered, welcomeChanged);
    const MinimapState minimap { d->displayFirst.cacheKey(), size(), d->pan, d->magnification, devicePixelRatioF(), d->rotation, d->mirrorX, d->mirrorY };
    const bool minimapChanged = d->minimapDirty || !d->minimapState || *d->minimapState != minimap;
    d->minimapState = minimap;
    place(d->minimapOverlay, QRectF(width() - 184, height() - 146, 184, 146), d->minimapVisible(size(), devicePixelRatioF()), minimapChanged);
    const int warning = d->preparedFirst.limited || d->preparedSecond.limited ? 3 : d->secondaryPending ? 2 : d->preparing ? 1 : 0;
    const bool warningChanged = d->warningState != warning;
    d->warningState = warning;
    place(d->warning, QRectF(0, 0, std::min(width(), 680), 44), warning != 0, warningChanged);
    const qreal dropWidth = std::min(420, std::max(140, width() - 32));
    place(d->dropHint, QRectF((width() - dropWidth) / 2.0, height() / 2.0 - 50, dropWidth, 100), d->dropHovered, false);
}

void ImageCanvas::paintOverlay(QPainter &painter)
{
    const qreal dpr = devicePixelRatioF();
    const bool dark = qGray(d->background.rgb()) < 128;
    const auto ink = [dark](const char *night, const char *day) { return QColor(dark ? night : day); };
    if (d->image.isNull() || d->displayFirst.isNull()) {
        const auto title = !d->image.isNull() ? QStringLiteral("이미지를 준비하고 있습니다")
            : d->statusTitle.isEmpty() ? QStringLiteral("이미지를 열어보세요") : d->statusTitle;
        const auto detail = !d->image.isNull() ? QStringLiteral("잠시 기다려 주세요.")
            : d->statusTitle.isEmpty() ? QStringLiteral("이미지 · PDF · 폴더 · 압축파일을 여기로 끌어놓으세요.\n파일 메뉴에서도 파일과 폴더를 선택할 수 있습니다.") : d->statusDetail;
        paintViewerPlaceholder(painter, size(), viewerPalette(dark), title, detail);
    } else {
        const auto displayed = d->displaySize(dpr);
        const bool overflow = displayed.width() > width() + 1 || displayed.height() > height() + 1;
        if (overflow && !d->hasSecond() && width() > 360 && height() > 260) {
            if (d->minimapDirty) {
                QImage thumbnail = d->displayFirst.scaled(156, 104, Qt::KeepAspectRatio, Qt::FastTransformation);
                if (thumbnail.isNull()) thumbnail = colorAdjusted(d->image.scaled(156, 104, Qt::KeepAspectRatio, Qt::FastTransformation), d->gamma, d->inverted);
                if (d->mirrorX || d->mirrorY) thumbnail = mirroredImage(thumbnail, d->mirrorX, d->mirrorY);
                if (d->rotation) thumbnail = thumbnail.transformed(QTransform().rotate(d->rotation * 90));
                d->minimap = thumbnail;
                d->minimapDirty = false;
            }
            const QRectF outer(width() - 184, height() - 146, 164, 126);
            painter.setPen(QPen(ink("#414b57", "#adbeca"), 1));
            painter.setBrush(dark ? QColor(14, 18, 24, 225) : QColor(248, 250, 252, 240));
            painter.drawRoundedRect(outer, 6, 6);
            const QSizeF thumbnailSize = d->minimap.size().scaled(152, 100, Qt::KeepAspectRatio);
            QRectF thumb(QPointF(), thumbnailSize);
            thumb.moveCenter(QPointF(outer.center().x(), outer.top() + 55));
            painter.drawImage(thumb, d->minimap);
            const double sx = thumb.width() / displayed.width();
            const double sy = thumb.height() / displayed.height();
            const QPointF imageTopLeft(width() / 2.0 + d->pan.x() - displayed.width() / 2,
                                       height() / 2.0 + d->pan.y() - displayed.height() / 2);
            const QRectF visible((0 - imageTopLeft.x()) * sx + thumb.left(),
                                 (0 - imageTopLeft.y()) * sy + thumb.top(), width() * sx, height() * sy);
            painter.setClipRect(thumb);
            painter.setBrush(dark ? QColor(157, 229, 189, 22) : QColor(22, 129, 108, 25));
            painter.setPen(QPen(ink("#a4e9c2", "#16816c"), 1.5));
            painter.drawRect(visible);
            painter.setClipping(false);
            QFont label = font();
            label.setPixelSize(10);
            painter.setFont(label);
            painter.setPen(ink("#aebbc8", "#425967"));
            painter.drawText(outer.adjusted(4, 105, -4, -3), Qt::AlignCenter,
                             QStringLiteral("%1%  ·  드래그로 이동").arg(d->magnification * 100, 0, 'f', 0));
        }
        if (d->preparing || d->preparedFirst.limited || d->preparedSecond.limited || d->secondaryPending) {
            // The notice can overlap a white or dark photograph. Give it a
            // readable surface rather than relying on the photograph's color.
            const QRectF notice(8, 6, std::max(0, std::min(width(), 680) - 16), 32);
            painter.setPen(Qt::NoPen);
            painter.setBrush(dark ? QColor(16, 21, 28, 235) : QColor(244, 246, 248, 240));
            painter.drawRoundedRect(notice, 5, 5);
            painter.setPen(ink("#c6ccd4", "#425967"));
            QFont label = font();
            label.setPixelSize(11);
            painter.setFont(label);
            painter.drawText(QRectF(16, 12, width() - 32, 24), Qt::AlignLeft,
                             d->preparedFirst.limited || d->preparedSecond.limited
                                ? QStringLiteral("가시 영역 상세 표시가 8MP 예산으로 제한됩니다 · 확대하면 원본 세부 표시")
                                : d->secondaryPending ? QStringLiteral("다음 페이지 준비 중…") : QStringLiteral("화면 준비 중…"));
        }
    }
}

void ImageCanvas::mousePressEvent(QMouseEvent *event)
{
    setFocus(Qt::MouseFocusReason);
    if (event->button() == Qt::RightButton) {
        if (contextMenuPolicy() == Qt::CustomContextMenu) emit customContextMenuRequested(event->position().toPoint());
        event->accept();
    } else if (event->button() == Qt::BackButton || event->button() == Qt::ForwardButton) {
        emit navigateRequested(event->button() == Qt::BackButton ? -1 : 1);
        event->accept();
    } else if (event->button() == Qt::MiddleButton) {
        emit fullScreenRequested();
        event->accept();
    } else if (event->button() == Qt::LeftButton && !d->image.isNull()) {
        d->dragging = true;
        d->draggingMinimap = d->minimapVisible(size(), devicePixelRatioF()) && d->minimapThumb(size()).contains(event->position());
        if (d->draggingMinimap) { d->moveMinimap(event->position(), size(), devicePixelRatioF()); syncScene(); }
        d->dragPosition = event->position();
        setCursor(Qt::ClosedHandCursor);
        d->surface->setCursor(QCursor(Qt::ClosedHandCursor));
        event->accept();
    } else {
        QWidget::mousePressEvent(event);
    }
}

void ImageCanvas::mouseMoveEvent(QMouseEvent *event)
{
    if (d->dragging) {
        if (d->draggingMinimap) d->moveMinimap(event->position(), size(), devicePixelRatioF());
        else d->pan += event->position() - d->dragPosition;
        d->dragPosition = event->position();
        d->clampPan(size(), devicePixelRatioF());
        syncScene();
        event->accept();
    } else {
        const auto displayed = d->displaySize(devicePixelRatioF());
        const auto shape = displayed.width() > width() || displayed.height() > height()
                      ? Qt::OpenHandCursor : Qt::ArrowCursor;
        setCursor(shape);
        d->surface->setCursor(QCursor(shape));
        QWidget::mouseMoveEvent(event);
    }
}

void ImageCanvas::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        d->dragging = false; d->draggingMinimap = false;
        unsetCursor();
        d->surface->unsetCursor();
        event->accept();
    } else {
        QWidget::mouseReleaseEvent(event);
    }
}

void ImageCanvas::mouseDoubleClickEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        emit fullScreenRequested();
        event->accept();
    } else {
        QWidget::mouseDoubleClickEvent(event);
    }
}

void ImageCanvas::wheelEvent(QWheelEvent *event)
{
    if (d->image.isNull()) { event->ignore(); return; }
    setFocus(Qt::MouseFocusReason);
    if (event->phase() == Qt::NoScrollPhase) d->trackpadNavigationLatched = false;
    if (event->phase() == Qt::ScrollBegin || event->phase() == Qt::ScrollEnd) {
        d->wheelRemainder = 0;
        d->trackpadNavigationLatched = false;
        if (event->phase() == Qt::ScrollEnd) { event->accept(); return; }
    }
    // Qt maps Command to ControlModifier on macOS, and the physical Control key
    // to MetaModifier. Accept both so external mice and Mac trackpads agree.
    if (event->modifiers().testFlag(Qt::ControlModifier)
        || event->modifiers().testFlag(Qt::MetaModifier)) {
        const double steps = event->angleDelta().y() != 0 ? event->angleDelta().y() / 120.0
                                                        : event->pixelDelta().y() / 80.0;
        if (qFuzzyIsNull(steps)) { event->accept(); return; }
        d->zoomAt(d->magnification * std::pow(1.18, steps), event->position(), size(), devicePixelRatioF());
        emit zoomChanged(d->magnification);
        syncScene();
    } else {
        const auto displayed = d->displaySize(devicePixelRatioF());
        const bool overflowY = displayed.height() > height() + 1;
        const bool overflowX = displayed.width() > width() + 1;
        const auto pixels = event->pixelDelta();
        const auto angles = event->angleDelta();
        if (overflowY || overflowX) {
            double deltaY = pixels.isNull() ? angles.y() * (80.0 / 120) : pixels.y();
            double deltaX = pixels.isNull() ? angles.x() * (80.0 / 120) : pixels.x();
            if (event->modifiers().testFlag(Qt::ShiftModifier) || !overflowY) {
                d->pan.rx() += deltaX != 0 ? deltaX : deltaY;
            } else {
                d->pan.ry() += deltaY;
                d->pan.rx() += deltaX;
            }
            d->clampPan(size(), devicePixelRatioF());
            syncScene();
        } else {
            // A trackpad gesture should move one image, not continue skipping
            // images while its inertial scrolling coasts to a stop.
            if (event->phase() == Qt::ScrollMomentum || d->trackpadNavigationLatched) {
                event->accept();
                return;
            }
            const bool horizontal = std::abs(pixels.x()) > std::abs(pixels.y())
                || (pixels.isNull() && std::abs(angles.x()) > std::abs(angles.y()));
            const double delta = pixels.isNull()
                ? (horizontal ? angles.x() : angles.y())
                : (horizontal ? pixels.x() : pixels.y()) * 1.5;
            if (delta * d->wheelRemainder < 0) d->wheelRemainder = 0;
            d->wheelRemainder += delta;
            if (std::abs(d->wheelRemainder) >= 120) {
                emit navigateRequested(d->wheelRemainder > 0 ? -1 : 1);
                d->wheelRemainder = 0;
                d->trackpadNavigationLatched = event->phase() != Qt::NoScrollPhase;
            }
        }
    }
    event->accept();
}

bool ImageCanvas::nativeGestureEvent(QNativeGestureEvent *event)
{
    if (d->image.isNull()) { event->ignore(); return false; }
    const auto type = event->gestureType();
    if (type == Qt::BeginNativeGesture || type == Qt::EndNativeGesture) {
        if (type == Qt::EndNativeGesture && std::abs(d->nativeRotation) >= 45) rotate(d->nativeRotation > 0 ? 1 : -1);
        d->nativeRotation = 0;
        event->accept();
        return true;
    }
    if (type == Qt::ZoomNativeGesture) {
        if (qFuzzyIsNull(event->value())) { event->accept(); return true; }
        setFocus(Qt::MouseFocusReason);
        d->zoomAt(d->magnification * std::max(0.05, 1.0 + event->value()), event->position(), size(), devicePixelRatioF());
        emit zoomChanged(d->magnification);
        syncScene();
    } else if (type == Qt::RotateNativeGesture) {
        if (std::isfinite(event->value())) d->nativeRotation += std::clamp(event->value(), -360.0, 360.0);
        const int turns = int(d->nativeRotation / 90.0);
        if (turns) { d->nativeRotation -= turns * 90.0; rotate(turns); }
    } else if (type == Qt::SmartZoomNativeGesture) {
        if (std::abs(d->magnification - 1.0) < 0.001) setFitMode(FitMode::Fit);
        else {
            d->zoomAt(1.0, event->position(), size(), devicePixelRatioF());
            emit zoomChanged(d->magnification);
            syncScene();
        }
    } else if (type == Qt::PanNativeGesture) {
        const auto displayed = d->displaySize(devicePixelRatioF());
        if (displayed.width() <= width() && displayed.height() <= height()) { event->ignore(); return false; }
        d->pan += event->delta();
        d->clampPan(size(), devicePixelRatioF());
        syncScene();
    } else {
        event->ignore();
        return false;
    }
    event->accept();
    return true;
}

void ImageCanvas::setDropHovered(bool hovered)
{
    if (d->dropHovered == hovered) return;
    d->dropHovered = hovered;
    syncScene();
}

void ImageCanvas::dragEnterEvent(QDragEnterEvent *event)
{
    d->dragMime = event->mimeData();
    d->dragPaths = imageDropPaths(d->dragMime);
    const bool valid = !d->dragPaths.isEmpty();
    setDropHovered(valid && acceptReadOnlyDrop(event));
    if (!valid) event->ignore();
}

void ImageCanvas::dragMoveEvent(QDragMoveEvent *event)
{
    if (d->dragMime != event->mimeData()) {
        d->dragMime = event->mimeData(); d->dragPaths = imageDropPaths(d->dragMime);
    }
    const bool valid = !d->dragPaths.isEmpty();
    setDropHovered(valid && acceptReadOnlyDrop(event));
    if (!valid) event->ignore();
}

void ImageCanvas::dragLeaveEvent(QDragLeaveEvent *event)
{
    d->dragMime = nullptr; d->dragPaths.clear();
    setDropHovered(false);
    event->accept();
}

void ImageCanvas::dropEvent(QDropEvent *event)
{
    setDropHovered(false);
    const QStringList paths = imageDropPaths(event->mimeData());
    d->dragMime = nullptr; d->dragPaths.clear();
    if (!paths.isEmpty() && acceptReadOnlyDrop(event)) {
        setFocus(Qt::OtherFocusReason);
        emit filesDropped(paths);
    } else event->ignore();
}
}
