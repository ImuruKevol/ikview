#include "exportdialog.h"
#include "viewericons.h"
#include "dialoglayout.h"
#include <QScrollArea>
#include <QScrollBar>
#include <QGroupBox>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QDrag>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QImageWriter>
#include <QLabel>
#include <QKeyEvent>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QMimeData>
#include <QPageLayout>
#include <QPageSize>
#include <QPainter>
#include <QPdfWriter>
#include <QProgressBar>
#include <QPushButton>
#include <QSaveFile>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>
#include <QtConcurrent>
#include <algorithm>
#include <cmath>
#ifdef Q_OS_UNIX
#include <sys/stat.h>
#endif

namespace ik {
namespace {
constexpr qint64 maxOutputPixels = 128LL * 1024 * 1024;
QColor opaqueBackground(const QColor &color) {
    const int alpha = color.alpha();
    const auto overWhite = [alpha](int channel) { return (channel * alpha + 255 * (255 - alpha) + 127) / 255; };
    return QColor(overWhite(color.red()), overWhite(color.green()), overWhite(color.blue()));
}
struct BackgroundPreview { QImage before, after; };
class BackgroundPreviewWidget final : public QWidget {
public:
    explicit BackgroundPreviewWidget(QWidget *parent = nullptr) : QWidget(parent) {
        setObjectName("whiteBackgroundPreview"); setMinimumHeight(164); setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        setAccessibleName("흰 배경 제거 전후 미리보기");
    }
    void setPreview(BackgroundPreview value, const QString &message = {}) {
        m_images = std::move(value); m_message = message; setProperty("previewReady", !m_images.after.isNull()); update();
    }
protected:
    void paintEvent(QPaintEvent *) override {
        QPainter painter(this); painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.setPen(palette().color(QPalette::WindowText));
        const int cellWidth = (width() - 12) / 2;
        for (int index = 0; index < 2; ++index) {
            const QRect area(index * (cellWidth + 12), 24, cellWidth, height() - 24);
            painter.drawText(QRect(area.x(), 0, area.width(), 20), Qt::AlignLeft | Qt::AlignVCenter, index ? "제거 후" : "제거 전");
            painter.save(); painter.setClipRect(area);
            for (int y = area.top(); y <= area.bottom(); y += 8)
                for (int x = area.left(); x <= area.right(); x += 8)
                    painter.fillRect(QRect(x, y, 8, 8), ((x - area.left()) / 8 + (y - area.top()) / 8) % 2 ? QColor(205, 210, 216) : QColor(242, 244, 247));
            const auto &image = index ? m_images.after : m_images.before;
            if (!image.isNull()) {
                QSize size = image.size(); size.scale(area.size() - QSize(12, 12), Qt::KeepAspectRatio);
                painter.drawImage(QRect(area.center() - QPoint(size.width() / 2, size.height() / 2), size), image);
            } else {
                painter.fillRect(area, palette().color(QPalette::Base)); painter.setPen(palette().color(QPalette::Text));
                painter.drawText(area.adjusted(10, 10, -10, -10), Qt::AlignCenter | Qt::TextWordWrap, m_message);
            }
            painter.restore(); painter.setPen(palette().color(QPalette::Mid)); painter.drawRect(area.adjusted(0, 0, -1, -1));
            painter.setPen(palette().color(QPalette::WindowText));
        }
    }
private:
    BackgroundPreview m_images;
    QString m_message = QStringLiteral("미리보기 준비 중…");
};
class ExportOrderList final : public QListWidget {
    bool ownsPageDrag(const QDropEvent *event) const {
        // QObject properties stay inside this process, unlike MIME bytes.
        const bool ownedMime = event->mimeData()->property("ikviewExportSource").value<QObject *>() == this;
        return (event->source() == this || ownedMime) && event->mimeData()->hasFormat("application/x-ikview-export-page");
    }
public:
    using QListWidget::QListWidget;
    bool moveSource(int from, int to) {
        if (from < 0 || from >= count() || to < 0 || to >= count() || from == to) return false;
        auto *m = model();
        const auto sourceData = m->itemData(m->index(from, 0));
        {
            const QSignalBlocker blocker(m);
            const int step = to > from ? 1 : -1;
            for (int row = from; row != to; row += step) m->setItemData(m->index(row, 0), m->itemData(m->index(row + step, 0)));
            m->setItemData(m->index(to, 0), sourceData);
        }
        // Keep row identities stable. Qt 6.12's Cocoa bridge may re-enter the
        // accessibility cache while model resets delete its transient cells.
        // Updating data retains those interfaces for both buttons and dragging.
        emit m->dataChanged(m->index(std::min(from, to), 0), m->index(std::max(from, to), 0),
                            {Qt::DisplayRole, Qt::ToolTipRole, Qt::UserRole});
        setCurrentRow(to); scrollToItem(currentItem()); viewport()->update();
        return true;
    }
protected:
    void dragEnterEvent(QDragEnterEvent *event) override {
        if (!ownsPageDrag(event)) { event->ignore(); return; }
        if (event->source() == this) QListWidget::dragEnterEvent(event);
        else { event->setDropAction(Qt::MoveAction); event->accept(); }
    }
    void dragMoveEvent(QDragMoveEvent *event) override {
        if (!ownsPageDrag(event)) { event->ignore(); return; }
        if (event->source() == this) QListWidget::dragMoveEvent(event);
        else { setState(QAbstractItemView::DraggingState); event->setDropAction(Qt::MoveAction); event->accept(); }
    }
    void startDrag(Qt::DropActions) override {
        const auto indexes = selectionModel()->selectedIndexes();
        if (indexes.size() != 1) return;
        auto *mime = model()->mimeData(indexes);
        if (!mime) return;
        mime->setData("application/x-ikview-export-page", QByteArray::number(indexes.first().row()));
        mime->setProperty("ikviewExportSource", QVariant::fromValue<QObject *>(this));
        auto *drag = new QDrag(this); drag->setMimeData(mime); drag->setPixmap(viewport()->grab(visualRect(indexes.first())));
        // Own the whole drag operation: QAbstractItemView's standard move path
        // removes source rows after a drop, which would invalidate AX cells.
        drag->exec(Qt::MoveAction, Qt::MoveAction);
    }
    void dropEvent(QDropEvent *event) override {
        if (!ownsPageDrag(event)) { event->ignore(); return; }
        bool valid = false; const int from = event->mimeData()->data("application/x-ikview-export-page").toInt(&valid);
        if (!valid || from < 0 || from >= count()) { event->ignore(); return; }
        const auto hit = indexAt(event->position().toPoint());
        int insertion = hit.isValid() ? hit.row() + (event->position().y() > visualRect(hit).center().y() ? 1 : 0) : count();
        const int to = std::clamp(insertion > from ? insertion - 1 : insertion, 0, count() - 1);
        if (to != from) moveSource(from, to);
        setState(QAbstractItemView::NoState); event->setDropAction(Qt::MoveAction); event->accept(); viewport()->update();
    }
};
struct SourceIdentity {
    QString path;
    bool valid = false;
    quint64 device = 0, inode = 0;
    qint64 bytes = -1, seconds = 0, nanos = 0;
    bool operator==(const SourceIdentity &) const = default;
};
SourceIdentity identity(const QString &path) {
    SourceIdentity result; result.path = path;
#ifdef Q_OS_UNIX
    struct stat s {};
    if (::stat(QFile::encodeName(path).constData(), &s) == 0) {
        result.valid = true; result.device = s.st_dev; result.inode = s.st_ino; result.bytes = s.st_size;
#ifdef Q_OS_MACOS
        result.seconds = s.st_mtimespec.tv_sec; result.nanos = s.st_mtimespec.tv_nsec;
#else
        result.seconds = s.st_mtim.tv_sec; result.nanos = s.st_mtim.tv_nsec;
#endif
    }
#else
    const QFileInfo info(path); result.valid = info.isFile(); result.bytes = info.size();
    result.seconds = info.lastModified().toMSecsSinceEpoch();
#endif
    return result;
}
QString sourcePath(const ExportSource &source) { return source.entry.isArchive() ? source.entry.archivePath : source.entry.path; }
bool sameFile(const QString &first, const QString &second) {
    if (first.isEmpty() || second.isEmpty()) return false;
    const QFileInfo a(first), b(second);
    if (a.absoluteFilePath() == b.absoluteFilePath()) return true;
    if (!a.canonicalFilePath().isEmpty() && a.canonicalFilePath() == b.canonicalFilePath()) return true;
    const auto ia = identity(first), ib = identity(second);
    return ia.valid && ib.valid && ia.inode && ia.device == ib.device && ia.inode == ib.inode;
}
QString protectOriginals(const QVector<ExportSource> &sources, const QString &output) {
    for (const auto &source : sources)
        if (sameFile(sourcePath(source), output)) return QStringLiteral("원본 파일에는 저장할 수 없습니다. 다른 이름이나 위치를 선택하세요.");
    return {};
}
bool validSize(const QSize &size, int padding) {
    const qint64 w = qint64(size.width()) + qint64(padding) * 2;
    const qint64 h = qint64(size.height()) + qint64(padding) * 2;
    return size.width() > 0 && size.height() > 0 && padding >= 0 && w <= 65535 && h <= 65535 && w * h <= maxOutputPixels;
}
QImage preparedImage(const ExportSource &source, const ExportOptions &options, const std::shared_ptr<ExportProgress> &progress, QString *error) {
    QImage image = source.snapshot;
    if (image.isNull()) image = Document::decodeEntry(source.entry, source.page, error, nullptr,
        std::shared_ptr<std::atomic_bool>(progress, &progress->cancelled));
    if (progress->cancelled.load()) return {};
    if (image.isNull()) { if (error->isEmpty()) *error = QStringLiteral("이미지를 읽을 수 없습니다: ") + source.entry.name; return {}; }
    if (options.applyViewTransform) image = ImageCanvas::applyTransform(image, source.transform);
    if (options.removeWhite) {
        image = removeWhiteBackground(image, options.whiteBackground,
            std::shared_ptr<std::atomic_bool>(progress, &progress->cancelled));
        if (image.isNull()) { if (!progress->cancelled.load()) *error = QStringLiteral("흰 배경을 제거하지 못했습니다. 원본 크기는 최대 128 메가픽셀입니다."); return {}; }
    }
    const QSize dimensions = options.dimensions.isValid() ? options.dimensions : image.size();
    if (!validSize(dimensions, options.padding)) { *error = QStringLiteral("여백을 포함한 출력 크기는 최대 128 메가픽셀이며 각 변은 65535픽셀 이하여야 합니다."); return {}; }
    if (image.size() != dimensions) image = image.scaled(dimensions, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    if (image.isNull()) { *error = QStringLiteral("출력 이미지의 메모리를 할당하지 못했습니다."); return {}; }
    if (options.padding) {
        QImage padded(dimensions + QSize(options.padding * 2, options.padding * 2), QImage::Format_ARGB32_Premultiplied);
        if (padded.isNull()) { *error = QStringLiteral("여백 이미지의 메모리를 할당하지 못했습니다."); return {}; }
        // PDF paints its paper once, including image padding. Do not apply a
        // translucent color twice where padded image and paper overlap.
        padded.fill(options.format == "pdf" ? QColor(Qt::transparent) : exportSupportsTransparency(options.format) ? options.paddingColor : opaqueBackground(options.paddingColor)); QPainter painter(&padded); painter.drawImage(options.padding, options.padding, image); painter.end(); image = std::move(padded);
    }
    return image;
}
QSizeF paperPoints(const QImage &image, const ExportOptions &options) {
    // 96 pixels per inch preserves a useful physical size for image-sized PDF
    // pages. The image keeps its original pixel data inside the PDF.
    if (options.pdfPaper == PdfPaper::Image) {
        QSizeF size(image.width() * .75, image.height() * .75);
        // PDF 1.4 viewers support pages up to 200 inches. Keep very long
        // panoramas portable while retaining all source pixels in the image.
        if (std::max(size.width(), size.height()) > 14400.) size *= 14400. / std::max(size.width(), size.height());
        return size;
    }
    QSizeF size = QPageSize(options.pdfPaper == PdfPaper::A4 ? QPageSize::A4 : QPageSize::Letter).size(QPageSize::Point);
    if (image.width() > image.height()) size.transpose();
    return size;
}
}

bool exportSupportsTransparency(const QByteArray &format) {
    const auto normalized = format.toLower();
    return normalized == "png" || normalized == "webp" || normalized == "tiff" || normalized == "tif" || normalized == "pdf";
}

bool ExportProgress::requestCancel() { const std::lock_guard lock(phaseMutex); if (committing.load()) return false; cancelled.store(true); return true; }
bool ExportProgress::beginCommit() { const std::lock_guard lock(phaseMutex); if (cancelled.load()) return false; committing.store(true); return true; }

ExportResult exportImages(const QVector<ExportSource> &sources, const ExportOptions &options, const std::shared_ptr<ExportProgress> &progress) {
    if (sources.isEmpty()) return {QStringLiteral("저장할 이미지가 없습니다.")};
    if (!progress) return {QStringLiteral("저장 작업 상태가 없습니다.")};
    progress->total.store(int(sources.size())); progress->completed.store(0);
    if (progress->cancelled.load()) return {{}, true};
    if (options.output.trimmed().isEmpty()) return {QStringLiteral("저장 위치를 입력하세요.")};
    if (sources.size() != 1 && options.format != "pdf") return {QStringLiteral("여러 이미지는 하나의 PDF로 저장할 수 있습니다.")};
    if (options.removeWhite && !exportSupportsTransparency(options.format)) return {QStringLiteral("흰 배경을 제거하려면 PNG, WebP, TIFF 또는 PDF 형식을 선택하세요.")};
    const QString protection = protectOriginals(sources, options.output); if (!protection.isEmpty()) return {protection};
    QVector<SourceIdentity> identities;
    for (const auto &source : sources) { const auto path = sourcePath(source); if (!path.isEmpty()) identities.append(identity(path)); }
    if (!QDir().mkpath(QFileInfo(options.output).absolutePath())) return {QStringLiteral("출력 폴더를 만들 수 없습니다.")};
    QSaveFile file(options.output); file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) return {QStringLiteral("출력 파일을 만들 수 없습니다. ") + file.errorString()};
    QString error;
    if (options.format == "pdf") {
        // Scope the writer before committing: its destructor writes the PDF
        // cross-reference table and final trailer to the atomic temporary file.
        {
            QPdfWriter writer(&file); writer.setResolution(96); writer.setCreator(QStringLiteral("IKView")); writer.setTitle(QFileInfo(options.output).completeBaseName());
            QPainter painter;
            for (int i = 0; i < sources.size(); ++i) {
                if (progress->cancelled.load()) { error.clear(); break; }
                QImage image = preparedImage(sources[i], options, progress, &error);
                if (image.isNull()) break;
                if (progress->cancelled.load()) break;
                const QSizeF points = paperPoints(image, options);
                const double margin = options.pdfMarginMm * 72.0 / 25.4;
                if (!std::isfinite(margin) || margin < 0 || points.width() <= margin * 2 || points.height() <= margin * 2) { error = QStringLiteral("PDF 여백이 페이지보다 큽니다. 여백을 줄이거나 A4/Letter 용지를 선택하세요."); break; }
                const QPageSize page(points, QPageSize::Point, QStringLiteral("IKView image"), QPageSize::ExactMatch);
                QPageLayout pageLayout(page, QPageLayout::Portrait, QMarginsF(), QPageLayout::Point);
                pageLayout.setMode(QPageLayout::FullPageMode);
                if (!writer.setPageLayout(pageLayout)) { error = QStringLiteral("PDF 페이지 크기를 설정할 수 없습니다."); break; }
                if (i == 0) {
                    if (!painter.begin(&writer)) { error = QStringLiteral("PDF 기록을 시작할 수 없습니다."); break; }
                } else if (!writer.newPage()) { error = QStringLiteral("PDF 페이지를 추가할 수 없습니다."); break; }
                const int marginPixels = qRound(margin * writer.resolution() / 72.0);
                const QRect area = writer.pageLayout().fullRectPixels(writer.resolution()).adjusted(marginPixels, marginPixels, -marginPixels, -marginPixels);
                QSize scaled = image.size(); scaled.scale(area.size(), Qt::KeepAspectRatio);
                const QRect target(area.x() + (area.width() - scaled.width()) / 2, area.y() + (area.height() - scaled.height()) / 2, scaled.width(), scaled.height());
                // Unpainted PDF regions stay transparent for reuse/compositing.
                // Viewer paper can still look white; an opaque color remains
                // the default, while selected alpha is preserved in the file.
                if (options.paddingColor.alpha()) painter.fillRect(writer.pageLayout().fullRectPixels(writer.resolution()), options.paddingColor);
                painter.setRenderHint(QPainter::SmoothPixmapTransform); painter.drawImage(target, image);
                progress->completed.store(i + 1);
            }
            if (painter.isActive() && !painter.end() && error.isEmpty()) error = QStringLiteral("PDF 기록을 마칠 수 없습니다.");
        }
    } else {
        QImage image = preparedImage(sources.first(), options, progress, &error);
        if (!image.isNull() && !progress->cancelled.load()) {
            if (!exportSupportsTransparency(options.format) && image.hasAlphaChannel()) {
                QImage opaque(image.size(), QImage::Format_RGB32);
                if (opaque.isNull()) { file.cancelWriting(); return {QStringLiteral("출력 이미지의 메모리를 할당하지 못했습니다.")}; }
                opaque.fill(opaqueBackground(options.paddingColor)); QPainter painter(&opaque); painter.drawImage(0, 0, image); painter.end(); image = std::move(opaque);
            }
            QImageWriter writer(&file, options.format); writer.setQuality(options.quality);
            if (!writer.write(image)) error = QStringLiteral("이미지를 기록할 수 없습니다. ") + writer.errorString();
            else progress->completed.store(1);
        }
    }
    if (progress->cancelled.load()) { file.cancelWriting(); return {{}, true}; }
    if (!error.isEmpty()) { file.cancelWriting(); return {error}; }
    if (file.error() != QFileDevice::NoError) { file.cancelWriting(); return {QStringLiteral("출력 파일을 기록할 수 없습니다. ") + file.errorString()}; }
    for (const auto &before : identities)
        if (before.valid && identity(before.path) != before) { file.cancelWriting(); return {QStringLiteral("작업 중 원본 파일이 변경되었습니다. 원본과 기존 대상 파일을 보존했습니다. 파일을 다시 열고 시도하세요.")}; }
    const QString finalProtection = protectOriginals(sources, options.output); if (!finalProtection.isEmpty()) { file.cancelWriting(); return {finalProtection}; }
    if (!progress->beginCommit()) { file.cancelWriting(); return {{}, true}; }
    if (!file.commit()) return {QStringLiteral("저장을 완료할 수 없습니다. ") + file.errorString()};
    return {};
}

struct ExportDialog::Impl {
    ExportDialog *q;
    QVector<ExportSource> sources;
    QLineEdit *path;
    QComboBox *format, *paper;
    QSpinBox *width, *height, *quality, *padding;
    QDoubleSpinBox *margin;
    QCheckBox *aspect, *transform;
    QCheckBox *transparentPadding = nullptr, *removeWhite = nullptr, *softEdges = nullptr;
    QSpinBox *whiteTolerance = nullptr;
    QComboBox *whiteRegion = nullptr;
    QGroupBox *whiteControls = nullptr;
    QLabel *alphaHint = nullptr;
    BackgroundPreviewWidget *preview = nullptr;
    QTimer *previewTimer = nullptr;
    QFutureWatcher<BackgroundPreview> *previewWatcher = nullptr;
    std::shared_ptr<std::atomic_bool> previewCancelled;
    quint64 previewGeneration = 0, runningPreviewGeneration = 0;
    QColor paddingColor = Qt::white;
    QPushButton *paddingBackground = nullptr, *up = nullptr, *down = nullptr;
    QListWidget *order = nullptr;
    QWidget *controls, *sizeRow;
    QFormLayout *form, *outputForm;
    QGroupBox *advancedGroup = nullptr;
    QPushButton *advancedButton = nullptr;
    QLabel *validation;
    QProgressBar *progressBar;
    QDialogButtonBox *buttons;
    QFutureWatcher<ExportResult> *watcher;
    QFutureWatcher<QImage> *sourceWatcher = nullptr;
    std::shared_ptr<std::atomic_bool> sourceCancelled;
    QTimer *poll;
    std::shared_ptr<ExportProgress> progress;
    QSize original, transformedSize;
    bool saving = false, closeAfterCancel = false;
    QString output;
    bool multiple() const { return sources.size() > 1; }
    bool whiteEnabled() const { return removeWhite && !multiple() && exportSupportsTransparency(format->currentData().toByteArray()) && removeWhite->isChecked(); }
    QColor selectedPaddingColor() const {
        if (exportSupportsTransparency(format->currentData().toByteArray())) return transparentPadding->isChecked() ? QColor(Qt::transparent) : paddingColor;
        return opaqueBackground(paddingColor);
    }
    WhiteBackgroundOptions whiteOptions() const {
        WhiteBackgroundOptions result; result.tolerance = whiteTolerance->value(); result.softness = softEdges->isChecked() ? 16 : 0; result.connectedOnly = whiteRegion->currentIndex() == 0; return result;
    }
    void schedulePreview() {
        if (!previewTimer) return;
        ++previewGeneration;
        if (previewCancelled) previewCancelled->store(true);
        whiteControls->setVisible(whiteEnabled());
        if (!whiteEnabled() || saving) { previewTimer->stop(); return; }
        preview->setPreview({}, "미리보기 준비 중…"); previewTimer->start();
    }
    void startPreview() {
        if (!whiteEnabled() || saving || sources.isEmpty() || sources.first().snapshot.isNull() || previewWatcher->isRunning()) return;
        const auto image = sources.first().snapshot; const auto transformState = sources.first().transform;
        const bool applyTransform = transform->isChecked(); const auto options = whiteOptions();
        previewCancelled = std::make_shared<std::atomic_bool>(false); const auto cancelled = previewCancelled;
        runningPreviewGeneration = previewGeneration;
        previewWatcher->setFuture(QtConcurrent::run([image, transformState, applyTransform, options, cancelled] {
            BackgroundPreview result;
            if (cancelled->load()) return result;
            // Preview work is bounded; saving always processes full-resolution
            // source pixels. The single worker drops superseded requests.
            result.before = image.width() > 720 || image.height() > 480 ? image.scaled(720, 480, Qt::KeepAspectRatio, Qt::SmoothTransformation) : image;
            if (applyTransform) result.before = ImageCanvas::applyTransform(result.before, transformState);
            result.after = removeWhiteBackground(result.before, options, cancelled); return result;
        }));
    }
    void updatePaddingColor() {
        if (!paddingBackground) return;
        const auto color = selectedPaddingColor();
        QPixmap swatch(QSize(18, 18) * q->devicePixelRatioF()); swatch.setDevicePixelRatio(q->devicePixelRatioF()); swatch.fill(Qt::transparent);
        QPainter painter(&swatch); painter.setRenderHint(QPainter::Antialiasing);
        painter.setClipRect(QRect(1, 1, 16, 16));
        for (int y = 1; y < 17; y += 4) for (int x = 1; x < 17; x += 4) painter.fillRect(x, y, 4, 4, ((x / 4 + y / 4) % 2) ? QColor(195, 201, 209) : QColor(242, 244, 247));
        painter.fillRect(1, 1, 16, 16, color); painter.setClipping(false);
        painter.setPen(q->palette().color(QPalette::Mid)); painter.setBrush(Qt::NoBrush); painter.drawRoundedRect(QRectF(1, 1, 16, 16), 3, 3); painter.end();
        paddingBackground->setIcon(QIcon(swatch)); paddingBackground->setIconSize(QSize(18, 18));
        const auto name = !color.alpha() ? QStringLiteral("투명") : color.name(color.alpha() == 255 ? QColor::HexRgb : QColor::HexArgb).toUpper();
        paddingBackground->setText(name); paddingBackground->setAccessibleName("여백 배경색 " + name);
        paddingBackground->setEnabled(!exportSupportsTransparency(format->currentData().toByteArray()) || !transparentPadding->isChecked());
    }
    void syncExtension() {
        if (path->text().trimmed().isEmpty()) return;
        const QFileInfo file(path->text()); const auto extension = QString::fromLatin1(format->currentData().toByteArray()); const auto suffix = file.suffix().toLower();
        if (suffix == extension || (extension == "jpeg" && suffix == "jpg") || (extension == "tiff" && suffix == "tif")) return;
        const auto base = file.suffix().isEmpty() ? file.fileName() : file.completeBaseName(); path->setText(QDir(file.absolutePath()).filePath(base + '.' + extension));
    }
    void updateFormat() {
        const bool pdf = format->currentData() == QByteArray("pdf"); quality->setEnabled(!pdf && format->currentData() != QByteArray("bmp"));
        if (!multiple()) form->setRowVisible(quality, !pdf && format->currentData() != QByteArray("bmp")); outputForm->setRowVisible(format, !multiple());
        form->setRowVisible(paper, pdf); form->setRowVisible(margin, pdf); paper->setEnabled(pdf); margin->setEnabled(pdf); syncExtension();
        const bool alpha = exportSupportsTransparency(format->currentData().toByteArray());
        transparentPadding->setEnabled(alpha); transparentPadding->setToolTip(alpha ? "이미지 여백을 투명하게 저장합니다." : "이 형식은 불투명 배경으로 저장됩니다.");
        removeWhite->setEnabled(alpha && !multiple());
        alphaHint->setText(pdf ? "PDF의 투명 여백도 뷰어에서는 흰 용지처럼 보일 수 있습니다." : alpha ? "체크무늬는 투명 영역을 뜻합니다." : "이 형식은 투명 영역을 선택한 배경색과 합성합니다.");
        updatePaddingColor(); schedulePreview();
    }
    void updateOrderButtons() { if (up) { up->setEnabled(order->currentRow() > 0); down->setEnabled(order->currentRow() >= 0 && order->currentRow() < order->count() - 1); } }
    void setSaving(bool value) {
        saving = value; controls->setEnabled(!value); advancedButton->setEnabled(!value); buttons->button(QDialogButtonBox::Save)->setEnabled(!value);
        buttons->button(QDialogButtonBox::Cancel)->setText(value ? QStringLiteral("저장 취소") : QStringLiteral("취소")); progressBar->setVisible(value);
        if (value) { poll->start(); previewTimer->stop(); if (previewCancelled) previewCancelled->store(true); }
        else { poll->stop(); schedulePreview(); }
        emit q->savingChanged(value);
    }
};

ExportDialog::ExportDialog(QVector<ExportSource> sources, QWidget *parent, ExportPurpose purpose) : QDialog(parent), d(std::make_unique<Impl>()) {
    const bool removeBackground = purpose == ExportPurpose::RemoveWhiteBackground && sources.size() == 1;
    d->q = this; d->sources = std::move(sources); setObjectName("imageExportDialog"); setWindowTitle(d->multiple() ? QStringLiteral("선택한 이미지를 PDF로 저장") : removeBackground ? QStringLiteral("흰 배경 제거하여 저장") : QStringLiteral("이미지 변환 / 저장")); setMinimumWidth(540);
    auto *layout = new QVBoxLayout(this); configureDialog(this, 680, removeBackground ? 680 : d->multiple() ? 540 : 420); layout->setSpacing(12); setMinimumSize(600, 420);
    layout->addWidget(dialogHeading(d->multiple() ? "이미지를 PDF로 묶기" : removeBackground ? "흰 배경 제거하여 저장" : "이미지 변환 / 저장",
        d->multiple() ? QString("선택한 %1개 이미지의 순서와 출력 설정을 확인하세요.").arg(d->sources.size()) : removeBackground ? "미리보기로 확인하고 투명 이미지로 저장합니다. 원본은 보존합니다." : "출력 형식과 크기를 정해 새 파일로 저장합니다."));
    auto *scroll = new QScrollArea; scroll->setObjectName("exportScrollArea"); scroll->setWidgetResizable(true); scroll->setFrameShape(QFrame::NoFrame); scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    d->controls = new QWidget; d->controls->setObjectName("exportControls"); auto *controlLayout = new QVBoxLayout(d->controls); controlLayout->setContentsMargins(0, 0, 10, 0); controlLayout->setSpacing(12);
    scroll->setWidget(d->controls); layout->addWidget(scroll, 1);
    if (d->multiple()) {
        auto *orderHeader = new QHBoxLayout; orderHeader->setSpacing(8); auto *hint = new QLabel("끌어서 페이지 순서 변경"); hint->setProperty("tone", "muted"); orderHeader->addWidget(hint, 1); controlLayout->addLayout(orderHeader);
        d->order = new ExportOrderList; d->order->setObjectName("exportSourceOrder"); d->order->setAccessibleName("PDF 페이지 순서"); d->order->setDragDropMode(QAbstractItemView::InternalMove); d->order->setDefaultDropAction(Qt::MoveAction); useDetailList(d->order); d->order->setFixedHeight(std::min(3, int(d->sources.size())) * 64 + 4); controlLayout->addWidget(d->order);
        for (int i=0; i<d->sources.size(); ++i) { const auto &s = d->sources[i]; auto *item = new QListWidgetItem(s.entry.name.isEmpty() ? QStringLiteral("이미지 %1").arg(i + 1) : s.entry.name, d->order); item->setData(Qt::UserRole, i); setDetailText(item, s.entry.displayPath()); item->setToolTip(s.entry.displayPath() + (s.page ? QStringLiteral(" · %1페이지").arg(s.page + 1) : QString())); item->setFlags(item->flags() & ~Qt::ItemIsDropEnabled); }
        d->up = new QPushButton("위로"); d->up->setObjectName("exportMoveUp"); d->up->setProperty("subtle", true); d->down = new QPushButton("아래로"); d->down->setObjectName("exportMoveDown"); d->down->setProperty("subtle", true); d->up->setIcon(viewerIcon("up", palette().color(QPalette::ButtonText))); d->down->setIcon(viewerIcon("down", palette().color(QPalette::ButtonText))); d->up->setIconSize(QSize(20, 20)); d->down->setIconSize(QSize(20, 20)); orderHeader->addWidget(d->up); orderHeader->addWidget(d->down);
        const auto move = [this](int direction) {
            const int row = d->order->currentRow(), next = row + direction;
            if (row < 0 || next < 0 || next >= d->order->count()) return;
            static_cast<ExportOrderList *>(d->order)->moveSource(row, next);
            d->updateOrderButtons();
        };
        connect(d->up, &QPushButton::clicked, this, [move] { move(-1); }); connect(d->down, &QPushButton::clicked, this, [move] { move(1); }); connect(d->order, &QListWidget::currentRowChanged, this, [this] { d->updateOrderButtons(); }); connect(d->order->model(), &QAbstractItemModel::rowsMoved, this, [this] { d->updateOrderButtons(); }); d->order->setCurrentRow(0); d->updateOrderButtons();
    }
    auto *outputGroup = new QGroupBox("저장"); outputGroup->setObjectName("exportOutputGroup"); outputGroup->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed); controlLayout->addWidget(outputGroup); auto *form = new QFormLayout(outputGroup); configureForm(form); form->setContentsMargins(0, 0, 0, 0); d->outputForm = form; d->path = new QLineEdit; d->path->setObjectName("exportPath"); d->path->setAccessibleName("저장할 파일 경로");
    const auto first = d->sources.isEmpty() ? ExportSource{} : d->sources.first(); const auto directory = QFileInfo(sourcePath(first)).absolutePath(); const auto base = QFileInfo(first.entry.name).completeBaseName();
    d->path->setText(QDir(directory).filePath((base.isEmpty() ? QStringLiteral("images") : base) + (d->multiple() ? QStringLiteral("-combined.pdf") : removeBackground ? QStringLiteral("-transparent.png") : QStringLiteral("-edited.png"))));
    auto *pathRow = new QWidget; auto *pathLayout = new QHBoxLayout(pathRow); pathLayout->setContentsMargins(0, 0, 0, 0); pathLayout->addWidget(d->path); auto *browse = new QPushButton("찾기…"); browse->setObjectName("browseExportPath"); pathLayout->addWidget(browse); form->addRow("저장 위치", pathRow);
    d->path->setToolTip(d->path->text()); connect(d->path, &QLineEdit::textChanged, d->path, &QLineEdit::setToolTip);
    d->format = new QComboBox; d->format->setObjectName("exportFormat"); d->format->setAccessibleName("출력 형식"); const auto supported = QImageWriter::supportedImageFormats();
    if (!d->multiple()) for (const QByteArray &candidate : {QByteArray("png"), QByteArray("jpeg"), QByteArray("webp"), QByteArray("bmp"), QByteArray("tiff")}) if (supported.contains(candidate) && (!removeBackground || exportSupportsTransparency(candidate))) d->format->addItem(QString::fromLatin1(candidate).toUpper(), candidate);
    if (!removeBackground) d->format->addItem("PDF", QByteArray("pdf")); form->addRow("출력 형식", d->format);
    d->advancedButton = new QPushButton("추가 출력 설정"); d->advancedButton->setObjectName("exportAdvancedToggle"); d->advancedButton->setCheckable(true); d->advancedButton->setProperty("subtle", true); d->advancedButton->setIcon(viewerIcon("right", palette().color(QPalette::ButtonText))); d->advancedButton->setIconSize(QSize(20, 20)); d->advancedButton->setToolTip("크기·압축 품질·여백·보기 효과·PDF 용지를 설정합니다.");
    d->advancedGroup = new QGroupBox("추가 출력 설정"); d->advancedGroup->setObjectName("exportAdvancedOptions"); d->advancedGroup->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed); controlLayout->addWidget(d->advancedGroup); d->advancedGroup->hide();
    form = new QFormLayout(d->advancedGroup); configureForm(form); form->setContentsMargins(0, 0, 0, 0); d->form = form;
    connect(d->advancedButton, &QPushButton::toggled, this, [this, scroll](bool expanded) {
        d->advancedGroup->setVisible(expanded); d->advancedButton->setIcon(viewerIcon(expanded ? "down" : "right", palette().color(QPalette::ButtonText)));
        d->advancedButton->setAccessibleDescription(expanded ? "펼쳐짐" : "접힘");
        if (expanded) QTimer::singleShot(0, this, [this, scroll] { if (d->advancedButton->isChecked()) scroll->verticalScrollBar()->setValue(d->advancedGroup->y()); });
    });
    d->original = first.snapshot.size(); d->transformedSize = d->original; if (first.transform.quarterTurns % 2) d->transformedSize.transpose();
    const bool sourceSizeKnown = !d->original.isEmpty();
    d->original = d->transformedSize; d->width = new QSpinBox(d->controls); d->height = new QSpinBox(d->controls); d->width->setObjectName("exportWidth"); d->height->setObjectName("exportHeight"); d->width->setAccessibleName("출력 너비"); d->height->setAccessibleName("출력 높이"); d->width->setRange(sourceSizeKnown ? 1 : 0, 65535); d->height->setRange(sourceSizeKnown ? 1 : 0, 65535); if (!sourceSizeKnown) { d->width->setSpecialValueText("원본"); d->height->setSpecialValueText("원본"); } d->width->setValue(sourceSizeKnown ? d->original.width() : 0); d->height->setValue(sourceSizeKnown ? d->original.height() : 0);
    d->width->setFixedWidth(116); d->height->setFixedWidth(116);
    d->sizeRow = new QWidget(d->controls); auto *sizeLayout = new QHBoxLayout(d->sizeRow); sizeLayout->setContentsMargins(0, 0, 0, 0); sizeLayout->setSpacing(8); sizeLayout->addWidget(d->width); sizeLayout->addWidget(new QLabel("×")); sizeLayout->addWidget(d->height); sizeLayout->addWidget(new QLabel("px")); sizeLayout->addStretch(); if (!d->multiple()) form->addRow("크기", d->sizeRow); else d->sizeRow->hide();
    d->aspect = new QCheckBox("종횡비 유지", d->controls); d->aspect->setObjectName("exportKeepAspect"); d->aspect->setChecked(true); if (!d->multiple()) form->addRow("", d->aspect); else d->aspect->hide();
    if (!sourceSizeKnown) { d->sizeRow->setEnabled(false); d->aspect->setEnabled(false); d->sizeRow->setToolTip("원본 크기를 확인 중입니다. 확인 전에도 원본 크기로 저장할 수 있습니다."); }
    d->quality = new QSpinBox(d->controls); d->quality->setObjectName("exportQuality"); d->quality->setAccessibleName("압축 품질"); d->quality->setRange(1, 100); d->quality->setValue(95); d->quality->setFixedWidth(116); d->quality->setSuffix(" %"); if (!d->multiple()) form->addRow("압축 품질", d->quality); else d->quality->hide();
    d->padding = new QSpinBox(d->controls); d->padding->setObjectName("exportPadding"); d->padding->setAccessibleName("이미지 여백, 각 변 픽셀 수"); d->padding->setRange(0, 8192); d->padding->setSuffix(" px"); d->padding->setFixedWidth(116); if (!d->multiple()) form->addRow("이미지 여백", d->padding); else d->padding->hide();
    auto *backgroundRow = new QWidget; auto *backgroundLayout = new QHBoxLayout(backgroundRow); backgroundLayout->setContentsMargins(0, 0, 0, 0); backgroundLayout->setSpacing(12);
    d->paddingBackground = new QPushButton("#ffffff"); d->paddingBackground->setObjectName("exportPaddingColor"); d->paddingBackground->setFixedWidth(156); backgroundLayout->addWidget(d->paddingBackground);
    d->transparentPadding = new QCheckBox("투명"); d->transparentPadding->setObjectName("exportTransparentPadding"); d->transparentPadding->setChecked(removeBackground); backgroundLayout->addWidget(d->transparentPadding); backgroundLayout->addStretch(); form->addRow("여백 배경색", backgroundRow);
    d->alphaHint = new QLabel; d->alphaHint->setObjectName("exportAlphaHint"); d->alphaHint->setProperty("tone", "muted"); d->alphaHint->setWordWrap(true); form->addRow("", d->alphaHint);
    connect(d->transparentPadding, &QCheckBox::toggled, this, [this] { d->updatePaddingColor(); });
    connect(d->paddingBackground, &QPushButton::clicked, this, [this] {
        const bool alpha = exportSupportsTransparency(d->format->currentData().toByteArray());
        const auto chosen = QColorDialog::getColor(alpha ? d->paddingColor : opaqueBackground(d->paddingColor), this, "여백 배경색", alpha ? QColorDialog::ShowAlphaChannel : QColorDialog::ColorDialogOptions{});
        if (chosen.isValid()) { d->paddingColor = chosen; d->transparentPadding->setChecked(alpha && !chosen.alpha()); d->updatePaddingColor(); }
    });
    d->transform = new QCheckBox(d->multiple() ? "현재 이미지의 회전·반전·효과 적용" : "현재 회전·반전·효과 적용"); d->transform->setObjectName("exportApplyViewTransform"); d->transform->setChecked(true); form->addRow("", d->transform);
    d->paper = new QComboBox; d->paper->setObjectName("exportPdfPaper"); d->paper->setAccessibleName("PDF 용지 크기"); d->paper->addItem("각 이미지 크기에 맞춤", int(PdfPaper::Image)); d->paper->addItem("A4 (가로/세로 자동)", int(PdfPaper::A4)); d->paper->addItem("Letter (가로/세로 자동)", int(PdfPaper::Letter)); form->addRow("PDF 용지", d->paper);
    d->margin = new QDoubleSpinBox; d->margin->setObjectName("exportPdfMargin"); d->margin->setAccessibleName("PDF 페이지 여백, 각 변 밀리미터"); d->margin->setRange(0, 50); d->margin->setDecimals(1); d->margin->setSuffix(" mm"); d->margin->setFixedWidth(116); form->addRow("페이지 여백", d->margin);
    d->removeWhite = new QCheckBox("흰 배경 제거"); d->removeWhite->setObjectName("exportRemoveWhiteBackground"); d->removeWhite->setChecked(removeBackground);
    if (!d->multiple() && !removeBackground) controlLayout->insertWidget(controlLayout->indexOf(d->advancedGroup), d->removeWhite); else { d->removeWhite->setParent(d->controls); d->removeWhite->hide(); }
    d->whiteControls = new QGroupBox("흰 배경 제거"); d->whiteControls->setObjectName("whiteBackgroundControls"); d->whiteControls->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed); auto *whiteLayout = new QVBoxLayout(d->whiteControls); whiteLayout->setContentsMargins(0, 0, 0, 0); whiteLayout->setSpacing(8);
    auto *whiteForm = new QFormLayout; configureForm(whiteForm); whiteForm->setVerticalSpacing(8); whiteLayout->addLayout(whiteForm);
    d->whiteRegion = new QComboBox; d->whiteRegion->setObjectName("whiteBackgroundRegion"); d->whiteRegion->setAccessibleName("흰 배경 제거 범위"); d->whiteRegion->addItems({"가장자리와 이어진 영역", "이미지의 모든 흰색"}); whiteForm->addRow("제거 범위", d->whiteRegion);
    d->whiteTolerance = new QSpinBox; d->whiteTolerance->setObjectName("whiteBackgroundTolerance"); d->whiteTolerance->setAccessibleName("흰색 허용 범위"); d->whiteTolerance->setToolTip("0은 순백색을 기준으로 합니다. 값을 높이면 회색에 가까운 부분도 제거합니다."); d->whiteTolerance->setRange(0, 100); d->whiteTolerance->setValue(20); d->whiteTolerance->setFixedWidth(88);
    auto *toleranceRow = new QWidget; auto *toleranceLayout = new QHBoxLayout(toleranceRow); toleranceLayout->setContentsMargins(0, 0, 0, 0); toleranceLayout->setSpacing(12); toleranceLayout->addWidget(d->whiteTolerance);
    d->softEdges = new QCheckBox("가장자리 부드럽게"); d->softEdges->setObjectName("whiteBackgroundSoftEdges"); d->softEdges->setChecked(true); toleranceLayout->addWidget(d->softEdges); toleranceLayout->addStretch(); whiteForm->addRow("허용 범위", toleranceRow);
    auto *whiteHint = new QLabel("테두리와 이어진 흰색을 제거하고 내부의 흰색은 유지합니다."); whiteHint->setWordWrap(true); whiteHint->setProperty("tone", "muted"); whiteLayout->addWidget(whiteHint);
    d->preview = new BackgroundPreviewWidget; whiteLayout->addWidget(d->preview);
    auto *previewHint = new QLabel("축소 미리보기 · 원본 해상도로 저장"); previewHint->setProperty("tone", "muted"); whiteLayout->addWidget(previewHint);
    controlLayout->insertWidget(controlLayout->indexOf(d->advancedGroup), d->whiteControls); d->whiteControls->hide();
    d->previewTimer = new QTimer(this); d->previewTimer->setSingleShot(true); d->previewTimer->setInterval(150);
    d->previewWatcher = new QFutureWatcher<BackgroundPreview>(this);
    connect(d->previewTimer, &QTimer::timeout, this, [this] { d->startPreview(); });
    connect(d->previewWatcher, &QFutureWatcher<BackgroundPreview>::finished, this, [this] {
        if (!d->whiteEnabled() || d->saving) return;
        if (d->runningPreviewGeneration == d->previewGeneration && !d->previewCancelled->load()) {
            const auto result = d->previewWatcher->result(); d->preview->setPreview(result, result.after.isNull() ? "미리보기를 만들 수 없습니다." : QString());
        } else if (!d->previewTimer->isActive()) d->startPreview();
    });
    connect(d->removeWhite, &QCheckBox::toggled, this, [this](bool checked) {
        if (checked && d->transparentPadding->isEnabled()) d->transparentPadding->setChecked(true);
        d->schedulePreview();
    });
    connect(d->whiteTolerance, &QSpinBox::valueChanged, this, [this] { d->schedulePreview(); });
    connect(d->whiteRegion, &QComboBox::currentIndexChanged, this, [this, whiteHint] {
        whiteHint->setText(d->whiteRegion->currentIndex() == 0 ? "테두리와 이어진 흰색을 제거하고 내부의 흰색은 유지합니다." : "그림 안의 흰색도 함께 제거합니다."); d->schedulePreview();
    });
    connect(d->softEdges, &QCheckBox::toggled, this, [this] { d->schedulePreview(); });
    connect(d->transform, &QCheckBox::toggled, this, [this] { d->schedulePreview(); });
    auto *note = new QLabel(d->multiple() ? "이미지마다 한 페이지로 저장합니다. 애니메이션·다중 페이지 파일은 선택한 페이지 또는 첫 프레임만 사용합니다. 원본 파일과 메타데이터는 변경하지 않습니다." : "애니메이션과 다중 페이지 파일은 현재 프레임/페이지를 저장합니다. 메타데이터는 복사하지 않습니다. 확장자는 선택한 형식에 맞춰 저장합니다."); note->setWordWrap(true); note->setObjectName("secondaryNote"); form->addRow(note); controlLayout->addStretch();
    d->validation = new QLabel; d->validation->setObjectName("exportValidationMessage"); d->validation->setWordWrap(true); d->validation->hide(); layout->addWidget(d->validation); d->progressBar = new QProgressBar; d->progressBar->setObjectName("exportProgress"); d->progressBar->setTextVisible(false); d->progressBar->hide(); layout->addWidget(d->progressBar);
    d->buttons = new QDialogButtonBox(QDialogButtonBox::Save | QDialogButtonBox::Cancel); d->buttons->button(QDialogButtonBox::Save)->setText("저장"); d->buttons->button(QDialogButtonBox::Cancel)->setText("취소");
    auto *footer = dialogFooter(d->buttons); qobject_cast<QHBoxLayout *>(footer->layout())->insertWidget(0, d->advancedButton); layout->addWidget(footer);
    connect(d->buttons, &QDialogButtonBox::accepted, this, &ExportDialog::accept); connect(d->buttons, &QDialogButtonBox::rejected, this, &ExportDialog::reject);
    connect(browse, &QPushButton::clicked, this, [this] { const auto codec = QString::fromLatin1(d->format->currentData().toByteArray()); const auto selected = QFileDialog::getSaveFileName(this, "저장 위치", d->path->text(), QString("%1 (*.%2);;모든 파일 (*)").arg(codec.toUpper(), codec)); if (!selected.isEmpty()) { d->path->setText(selected); d->syncExtension(); } });
    connect(d->format, &QComboBox::currentIndexChanged, this, [this] { d->updateFormat(); }); connect(d->path, &QLineEdit::editingFinished, this, [this] { d->syncExtension(); });
    connect(d->width, &QSpinBox::valueChanged, this, [this](int value) { if (d->aspect->isChecked() && !d->original.isEmpty()) { QSignalBlocker blocker(d->height); d->height->setValue(qRound(double(value) * d->original.height() / d->original.width())); } });
    connect(d->height, &QSpinBox::valueChanged, this, [this](int value) { if (d->aspect->isChecked() && !d->original.isEmpty()) { QSignalBlocker blocker(d->width); d->width->setValue(qRound(double(value) * d->original.width() / d->original.height())); } });
    connect(d->transform, &QCheckBox::toggled, this, [this](bool enabled) { if (d->sources.isEmpty()) return; d->original = enabled ? d->transformedSize : d->sources.first().snapshot.size(); if (d->original.isEmpty()) return; QSignalBlocker bw(d->width), bh(d->height); d->width->setValue(d->original.width()); d->height->setValue(d->original.height()); });
    d->watcher = new QFutureWatcher<ExportResult>(this); d->poll = new QTimer(this); d->poll->setInterval(80);
    connect(d->poll, &QTimer::timeout, this, [this] { if (d->progress) { d->progressBar->setRange(0, std::max(1, d->progress->total.load())); d->progressBar->setValue(d->progress->completed.load()); } });
    connect(d->watcher, &QFutureWatcher<ExportResult>::finished, this, [this] {
        const auto result = d->watcher->result(); d->setSaving(false);
        emit exportFinished(result.success(), result.cancelled, result.error, d->output);
        if (result.success()) QDialog::accept();
        else if (result.cancelled && d->closeAfterCancel) QDialog::reject();
        else setMessage(d->validation, result.cancelled ? QStringLiteral("저장을 취소했습니다. 설정을 바꾸거나 다시 저장할 수 있습니다.") : QStringLiteral("저장하지 못했습니다. 설정을 수정하고 다시 시도하세요.\n") + result.error, result.cancelled ? "muted" : "error");
    });
    d->updateFormat();
    if (!d->multiple() && !sourceSizeKnown && !d->sources.isEmpty()) {
        // A selected item need not be the image currently shown by the viewer.
        // Resolve its geometry away from the GUI thread; until then empty
        // ExportOptions::dimensions preserves the decoded source size.
        d->sourceCancelled = std::make_shared<std::atomic_bool>(false);
        d->sourceWatcher = new QFutureWatcher<QImage>(this);
        connect(d->sourceWatcher, &QFutureWatcher<QImage>::finished, this, [this] {
            const auto image = d->sourceWatcher->result();
            if (d->sourceCancelled->load()) return;
            if (image.isNull()) { d->previewTimer->stop(); d->preview->setPreview({}, "원본 이미지를 읽을 수 없습니다."); return; }
            d->sources.first().snapshot = image;
            d->transformedSize = image.size(); if (d->sources.first().transform.quarterTurns % 2) d->transformedSize.transpose();
            d->original = d->transform->isChecked() ? d->transformedSize : image.size();
            const QSignalBlocker bw(d->width), bh(d->height);
            d->width->setMinimum(1); d->height->setMinimum(1); d->width->setSpecialValueText({}); d->height->setSpecialValueText({}); d->width->setValue(d->original.width()); d->height->setValue(d->original.height());
            d->sizeRow->setEnabled(true); d->aspect->setEnabled(true); d->sizeRow->setToolTip({});
            d->schedulePreview();
        });
        const auto source = d->sources.first(); const auto cancelled = d->sourceCancelled;
        d->sourceWatcher->setFuture(QtConcurrent::run([source, cancelled] { return Document::decodeEntry(source.entry, source.page, nullptr, nullptr, cancelled); }));
    }
    // Only Save is the primary/default operation. Browsing or changing a
    // setting must not acquire default-button styling when it receives focus.
    for (auto *button : d->controls->findChildren<QPushButton *>()) { button->setAutoDefault(false); button->setDefault(false); }
    d->advancedButton->setAutoDefault(false); d->advancedButton->setDefault(false);
}
ExportDialog::~ExportDialog() { if (d->sourceCancelled) d->sourceCancelled->store(true); if (d->previewCancelled) d->previewCancelled->store(true); if (d->saving) { d->progress->requestCancel(); d->watcher->waitForFinished(); } }
bool ExportDialog::isSaving() const { return d->saving; }
QVector<ExportSource> ExportDialog::orderedSources() const {
    if (!d->order) return d->sources;
    QVector<ExportSource> result; result.reserve(d->sources.size());
    for (int i=0; i<d->order->count(); ++i) result.append(d->sources[d->order->item(i)->data(Qt::UserRole).toInt()]);
    return result;
}
void ExportDialog::accept() {
    if (d->saving) return;
    setMessage(d->validation, {}); if (d->path->text().trimmed().isEmpty()) { setMessage(d->validation, "저장 위치를 입력하세요.", "error"); d->path->setFocus(); return; }
    d->syncExtension(); const auto output = QFileInfo(d->path->text()).absoluteFilePath();
    const auto protection = protectOriginals(d->sources, output); if (!protection.isEmpty()) { setMessage(d->validation, protection, "error"); return; }
    ExportOptions options; options.output = output; options.format = d->format->currentData().toByteArray(); options.dimensions = d->multiple() || d->original.isEmpty() ? QSize() : QSize(d->width->value(), d->height->value()); options.quality = d->quality->value(); options.padding = d->padding->value(); options.paddingColor = d->paddingColor; options.applyViewTransform = d->transform->isChecked(); options.pdfPaper = PdfPaper(d->paper->currentData().toInt()); options.pdfMarginMm = d->margin->value();
    options.paddingColor = d->selectedPaddingColor(); options.removeWhite = d->whiteEnabled(); options.whiteBackground = d->whiteOptions();
    if (options.dimensions.isValid() && !validSize(options.dimensions, options.padding)) { setMessage(d->validation, "여백을 포함한 출력 크기는 최대 128 메가픽셀입니다. 크기나 여백을 줄여주세요.", "error"); return; }
    if (QFileInfo(output).isDir()) { setMessage(d->validation, "저장 위치에 폴더가 아닌 파일 이름을 입력하세요.", "error"); return; }
    if (QFileInfo::exists(output) && questionDialog(this, "덮어쓰기", "파일이 이미 있습니다. 덮어쓸까요?", QMessageBox::Yes | QMessageBox::No, QMessageBox::No, "덮어쓰기") != QMessageBox::Yes) return;
    d->output = output; d->closeAfterCancel = false; d->progress = std::make_shared<ExportProgress>(); d->progress->total.store(int(d->sources.size())); d->progressBar->setRange(0, int(d->sources.size())); d->progressBar->setValue(0); setMessage(d->validation, "이미지를 변환하고 있습니다. 저장 취소로 작업을 중단할 수 있습니다.", "info");
    const auto sources = orderedSources(); const auto progress = d->progress; d->setSaving(true); d->watcher->setFuture(QtConcurrent::run([sources, options, progress] { return exportImages(sources, options, progress); }));
}
void ExportDialog::reject() {
    if (!d->saving) { QDialog::reject(); return; }
    d->closeAfterCancel = true;
    if (d->progress->requestCancel()) setMessage(d->validation, "안전하게 취소할 때까지 기다려 주세요.", "info");
    else setMessage(d->validation, "저장을 마무리하고 있습니다. 잠시 기다려 주세요.", "info");
}
void ExportDialog::changeEvent(QEvent *event) {
    QDialog::changeEvent(event);
    if (event->type() == QEvent::PaletteChange && d) {
        d->updatePaddingColor();
        if (d->advancedButton) d->advancedButton->setIcon(viewerIcon(d->advancedButton->isChecked() ? "down" : "right", palette().color(QPalette::ButtonText)));
        if (d->up) d->up->setIcon(viewerIcon("up", palette().color(QPalette::ButtonText)));
        if (d->down) d->down->setIcon(viewerIcon("down", palette().color(QPalette::ButtonText)));
    }
}
void ExportDialog::keyPressEvent(QKeyEvent *event) {
    if ((event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter)
        && focusWidget() != d->buttons->button(QDialogButtonBox::Save)
        && focusWidget() != d->buttons->button(QDialogButtonBox::Cancel)) {
        event->accept(); return;
    }
    QDialog::keyPressEvent(event);
}
}
