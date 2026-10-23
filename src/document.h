#pragma once
#include <QObject>
#include <QImage>
#include <QSize>
#include <QSizeF>
#include <QStringList>
#include <QVector>
#include <memory>
#include <atomic>

namespace ik {
struct Entry {
    QString name;
    QString path;
    QString archivePath;
    QString member;
    qint64 bytes = 0;
    int memberOccurrence = 0;
    qint64 modifiedMs = 0;
    bool operator==(const Entry &) const = default;
    bool isArchive() const { return !archivePath.isEmpty(); }
    QString key() const {
        // Length prefixes distinguish duplicate members and names containing
        // separators without conflating an archive identity with a real path.
        return isArchive() ? QStringLiteral("archive:") + QString::number(archivePath.size()) + ':' + archivePath
            + ':' + QString::number(member.size()) + ':' + member + ':' + QString::number(memberOccurrence) : path;
    }
    QString displayPath() const {
        return isArchive() ? archivePath + QStringLiteral(" :: ") + member
            + (memberOccurrence ? QStringLiteral(" (중복 항목 %1)").arg(memberOccurrence + 1) : QString()) : path;
    }
};
struct ImageInfo {
    QString format;
    QSize size;
    qint64 fileBytes = 0;
    int page = 0;
    int pageCount = 1;
    qint64 decodeMs = 0;
    QStringList metadata;
    bool animated = false;
    bool pdf = false;
    QSizeF pageSizePoints;
};
enum class SortOrder { Name, NameDescending, Modified, Size };

class Document : public QObject {
    Q_OBJECT
public:
    explicit Document(QObject *parent = nullptr);
    ~Document() override;
    void open(const QString &path);
    // File-only scope avoids implicitly enumerating siblings of a file selected
    // through a macOS picker. Explicit folders and archives keep normal scope.
    void openFiles(const QStringList &paths, bool includeSiblingFiles = true);
    void close();
    void navigate(int delta);
    void goTo(int index);
    void setPage(int page);
    void adjacentContainer(int direction);
    void setSortOrder(SortOrder order);
    void setCacheLimitMB(int mb);
    void setAnimationPaused(bool paused);
    void invalidateCurrent();
    // Filesystem operations are performed by the caller. Remove only the
    // captured identity and retain the collection's original access scope.
    void removeEntry(const QString &key);
    void refreshCollection();
    int currentIndex() const;
    int count() const;
    const QVector<Entry> &entries() const;
    Entry currentEntry() const;
    QStringList collectionPaths() const;
    QImage currentImage() const;
    ImageInfo currentInfo() const;
    QImage cachedImage(int index) const;
    QString neighborError(int index) const;
    bool isBusy() const;
    bool isFileScoped() const;
    bool isMonitoringLimited() const;
    static QStringList supportedExtensions();
    static bool isImageFile(const QString &path);
    static bool isArchivePath(const QString &path);
    // Synchronous worker-side decode for conversion/export; never touches a
    // Document instance or UI state. Cancellation is checked during archive IO
    // and around native decoders, whose draw calls cannot be interrupted.
    static QImage decodeEntry(const Entry &entry, int page = 0, QString *error = nullptr,
                             ImageInfo *info = nullptr, const std::shared_ptr<std::atomic_bool> &cancelled = {});
signals:
    void entriesChanged();
    void currentChanged(int index);
    void imageReady(const QImage &image, const ik::ImageInfo &info);
    void neighborReady(int index, const QImage &image);
    void neighborFailed(int index, const QString &message);
    void loadingChanged(bool loading);
    void errorOccurred(const QString &message);
    void cacheUsageChanged(qint64 bytes);
private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
}
Q_DECLARE_METATYPE(ik::ImageInfo)
