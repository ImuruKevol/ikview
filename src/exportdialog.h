#pragma once
#include "document.h"
#include "imagecanvas.h"
#include "imagebackground.h"
#include <QDialog>
#include <QColor>
#include <atomic>
#include <memory>
#include <mutex>

namespace ik {
// Only the displayed source needs a snapshot. Other sources are decoded one at
// a time in the export worker, including archive members and PDF pages.
struct ExportSource {
    Entry entry;
    int page = 0;
    QImage snapshot;
    ImageTransformState transform;
};
enum class PdfPaper { Image, A4, Letter };
enum class ExportPurpose { Convert, RemoveWhiteBackground };
bool exportSupportsTransparency(const QByteArray &format);
struct ExportOptions {
    QString output;
    QByteArray format = "png";
    QSize dimensions; // Empty preserves each source's dimensions.
    int quality = 95;
    int padding = 0;
    QColor paddingColor = Qt::white;
    bool removeWhite = false;
    WhiteBackgroundOptions whiteBackground;
    bool applyViewTransform = true;
    PdfPaper pdfPaper = PdfPaper::Image;
    double pdfMarginMm = 0;
};
struct ExportProgress {
    std::atomic<bool> cancelled {false};
    std::atomic<bool> committing {false};
    std::atomic<int> completed {0};
    std::atomic<int> total {0};
    std::mutex phaseMutex;
    bool requestCancel();
    bool beginCommit();
};
struct ExportResult {
    QString error;
    bool cancelled = false;
    bool success() const { return error.isEmpty() && !cancelled; }
};
// Synchronous worker entry point. No QWidget or GUI-thread decoder dependency.
ExportResult exportImages(const QVector<ExportSource> &sources, const ExportOptions &options,
                          const std::shared_ptr<ExportProgress> &progress);

class ExportDialog : public QDialog {
    Q_OBJECT
public:
    explicit ExportDialog(QVector<ExportSource> sources, QWidget *parent = nullptr,
                          ExportPurpose purpose = ExportPurpose::Convert);
    ~ExportDialog() override;
    bool isSaving() const;
    QVector<ExportSource> orderedSources() const;
public slots:
    void accept() override;
    void reject() override;
signals:
    void savingChanged(bool saving);
    void exportFinished(bool success, bool cancelled, const QString &error, const QString &output);
protected:
    void keyPressEvent(QKeyEvent *event) override;
    void changeEvent(QEvent *event) override;
private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
}
