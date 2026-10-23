#include "document.h"
#include "nativeimage.h"

#include <QBuffer>
#include <QCollator>
#include <QColorSpace>
#include <QDir>
#include <QDirIterator>
#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QFileSystemWatcher>
#include <QFutureWatcher>
#include <QImageReader>
#include <QMovie>
#include <QSet>
#include <QThreadPool>
#include <QTimer>
#include <QtConcurrent/QtConcurrentRun>
#include <archive.h>
#include <archive_entry.h>
#include <algorithm>
#include <atomic>
#include <functional>
#include <limits>
#ifdef Q_OS_UNIX
#include <sys/stat.h>
#endif
#ifdef IKVIEW_HAS_EXIV2
#include <exiv2/exiv2.hpp>
#endif

namespace ik {
namespace {
constexpr qint64 MaxEncodedBytes = 256LL * 1024 * 1024;
constexpr qint64 MaxPixels = 128LL * 1024 * 1024;
constexpr int MaxEntries = 200000;
using Epoch = std::shared_ptr<std::atomic<quint64>>;
struct ArchiveDeleter { void operator()(archive *a) const { if (a) archive_read_free(a); } };
using ArchivePtr = std::unique_ptr<archive, ArchiveDeleter>;
QString archiveName(archive_entry *entry) {
    const char *name = archive_entry_pathname_utf8(entry);
    if (name) return QString::fromUtf8(name);
    name = archive_entry_pathname(entry);
    return name ? QString::fromLocal8Bit(name) : QString();
}
ArchivePtr openArchive(const QString &path, QString &error) {
    ArchivePtr a(archive_read_new());
    archive_read_support_filter_all(a.get());
    archive_read_support_format_all(a.get());
#ifdef Q_OS_WIN
    int result = archive_read_open_filename_w(a.get(), reinterpret_cast<const wchar_t *>(path.utf16()), 65536);
#else
    const QByteArray name = QFile::encodeName(path);
    int result = archive_read_open_filename(a.get(), name.constData(), 65536);
#endif
    if (result != ARCHIVE_OK) {
        error = QStringLiteral("압축파일을 열 수 없습니다: %1").arg(QString::fromUtf8(archive_error_string(a.get())));
        return {};
    }
    return a;
}
bool imageName(const QString &path, const QSet<QString> &extensions) {
    return extensions.contains(QFileInfo(path).suffix().toLower());
}
qint64 archiveModifiedMs(archive_entry *entry) {
    if (!archive_entry_mtime_is_set(entry)) return 0;
    const qint64 seconds = archive_entry_mtime(entry);
    constexpr auto maximum = std::numeric_limits<qint64>::max();
    constexpr auto minimum = std::numeric_limits<qint64>::min();
    if (seconds > maximum / 1000) return maximum;
    if (seconds < minimum / 1000) return minimum;
    const auto base = seconds * 1000;
    const auto millis = std::clamp<qint64>(archive_entry_mtime_nsec(entry) / 1000000, 0, 999);
    return base > maximum - millis ? maximum : base + millis;
}
void sortEntries(QVector<Entry> &entries, SortOrder order) {
    QCollator collator;
    collator.setNumericMode(true);
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    QHash<QString, qint64> modified;
    if (order == SortOrder::Modified) {
        for (const auto &entry : entries)
            modified.insert(entry.key(), entry.isArchive() ? entry.modifiedMs : QFileInfo(entry.path).lastModified().toMSecsSinceEpoch());
    }
    std::stable_sort(entries.begin(), entries.end(), [&](const Entry &a, const Entry &b) {
        if (order == SortOrder::Size && a.bytes != b.bytes) return a.bytes < b.bytes;
        if (order == SortOrder::Modified && modified.value(a.key()) != modified.value(b.key()))
            return modified.value(a.key()) < modified.value(b.key());
        const int comparison = collator.compare(a.name, b.name);
        return order == SortOrder::NameDescending ? comparison > 0 : comparison < 0;
    });
}
struct FileStamp {
    qint64 bytes = 0;
    qint64 modified = 0;
    quint64 device = 0;
    quint64 inode = 0;
    qint32 nanosecond = 0;
    qint64 changed = 0;
    qint32 changedNanosecond = 0;
    bool operator==(const FileStamp &) const = default;
};
FileStamp fileStamp(const QFileInfo &file) {
#ifdef Q_OS_UNIX
    struct stat value {};
    if (::stat(QFile::encodeName(file.absoluteFilePath()).constData(), &value) == 0) {
#ifdef Q_OS_MACOS
        const auto time = value.st_mtimespec;
        const auto changed = value.st_ctimespec;
#else
        const auto time = value.st_mtim;
        const auto changed = value.st_ctim;
#endif
        return {qint64(value.st_size), qint64(time.tv_sec), quint64(value.st_dev), quint64(value.st_ino), qint32(time.tv_nsec),
                qint64(changed.tv_sec), qint32(changed.tv_nsec)};
    }
#endif
    const auto modified = file.lastModified();
    return {file.size(), modified.toSecsSinceEpoch(), 0, 0, qint32(modified.time().msec() * 1000000)};
}
struct ScanResult {
    QVector<Entry> entries;
    QString container;
    QString error;
    QHash<QString, FileStamp> stamps;
    QSet<QString> directorySources;
    bool unchanged = false;
    void addFile(const QFileInfo &file) {
        const auto path = file.absoluteFilePath();
        const auto stamp = fileStamp(file);
        stamps.insert(path, stamp);
        entries.append({file.fileName(), path, {}, {}, stamp.bytes, 0, file.lastModified().toMSecsSinceEpoch()});
    }
};
ScanResult scanPaths(const QStringList &paths, const QSet<QString> &extensions, SortOrder order,
                     const Epoch &epoch, quint64 generation, bool includeSiblingFiles, const QString &selected = {}) {
    ScanResult result;
    if (paths.isEmpty()) return result;
    const auto cancelled = [&] { return epoch->load() != generation; };
    const QString first = QFileInfo(paths.first()).absoluteFilePath();
    if (paths.size() == 1 && Document::isArchivePath(first)) {
        result.container = first;
        result.stamps.insert(first, fileStamp(QFileInfo(first)));
        auto a = openArchive(first, result.error);
        if (!a) return result;
        archive_entry *entry = nullptr;
        int status = ARCHIVE_OK;
        int headers = 0;
        QHash<QString, int> occurrences;
        while (!cancelled() && (status = archive_read_next_header(a.get(), &entry)) != ARCHIVE_EOF) {
            if (status < ARCHIVE_WARN) { result.error = QString::fromUtf8(archive_error_string(a.get())); break; }
            if (++headers > 1000000) { result.error = QStringLiteral("압축파일의 항목 수가 너무 많습니다."); break; }
            const QString name = archiveName(entry);
            if (archive_entry_filetype(entry) == AE_IFREG && imageName(name, extensions)) {
                result.entries.append({name, {}, first, name, std::max<qint64>(0, archive_entry_size(entry)), occurrences[name]++, archiveModifiedMs(entry)});
                if (result.entries.size() >= MaxEntries) { result.error = QStringLiteral("이미지가 너무 많아 처음 200,000개만 불러왔습니다."); break; }
            }
            archive_read_data_skip(a.get());
        }
    } else if (paths.size() == 1) {
        QFileInfo info(first);
        if (!info.exists()) { result.error = QStringLiteral("파일 또는 폴더가 없습니다: %1").arg(first); return result; }
        if (info.isDir()) result.directorySources.insert(first);
        if (info.isFile() && !includeSiblingFiles) {
            // A native file picker or Finder drop grants the selected file, not
            // necessarily its parent. Do not block opening it on parent access.
            result.container = first;
            result.addFile(info);
        } else {
            result.container = info.isDir() ? first : info.absolutePath();
            // Hidden files are intentionally absent from an ordinary folder
            // listing, but an explicitly selected file must always be opened.
            const QFileInfo chosen(info.isFile() ? first : selected);
            const QString chosenPath = chosen.isFile() && chosen.absolutePath() == result.container ? chosen.absoluteFilePath() : QString();
            if (!chosenPath.isEmpty()) result.addFile(chosen);
            QDirIterator iterator(result.container, QDir::Files | QDir::Readable);
            while (!cancelled() && iterator.hasNext()) {
                iterator.next();
                const QFileInfo file = iterator.fileInfo();
                if (file.absoluteFilePath() != chosenPath && imageName(file.fileName(), extensions)) {
                    result.addFile(file);
                    if (result.entries.size() >= MaxEntries) { result.error = QStringLiteral("이미지가 너무 많아 처음 200,000개만 불러왔습니다."); break; }
                }
            }
        }
    } else {
        QSet<QString> seen;
        for (const QString &path : paths) {
            if (cancelled()) break;
            QFileInfo file(path);
            if (file.isDir() || Document::isArchivePath(path)) {
                auto group = scanPaths({file.absoluteFilePath()}, extensions, order, epoch, generation, includeSiblingFiles, selected);
                result.stamps.insert(group.stamps);
                result.directorySources.unite(group.directorySources);
                for (const auto &entry : group.entries) {
                    if (seen.contains(entry.key())) continue;
                    seen.insert(entry.key());
                    result.entries.append(entry);
                    if (result.entries.size() >= MaxEntries) break;
                }
                if (result.error.isEmpty() && !group.error.isEmpty()) result.error = group.error;
            } else if (file.isFile() && Document::isImageFile(file.absoluteFilePath()) && !seen.contains(file.absoluteFilePath())) {
                seen.insert(file.absoluteFilePath());
                result.addFile(file);
            }
            if (result.entries.size() >= MaxEntries) break;
        }
        result.container = QFileInfo(first).absolutePath();
    }
    if (!cancelled()) sortEntries(result.entries, order);
    if (result.entries.isEmpty() && result.error.isEmpty() && !cancelled())
        result.error = QStringLiteral("표시할 수 있는 이미지가 없습니다.");
    return result;
}
QByteArray memberBytes(const Entry &source, QString &error, const Epoch &epoch, quint64 generation,
                       const std::shared_ptr<std::atomic_bool> &cancel = {}) {
    auto a = openArchive(source.archivePath, error);
    if (!a) return {};
    archive_entry *entry = nullptr;
    int status;
    int occurrence = 0;
    const auto cancelled = [&] { return epoch->load() != generation || (cancel && cancel->load()); };
    while (!cancelled() && (status = archive_read_next_header(a.get(), &entry)) != ARCHIVE_EOF) {
        if (status < ARCHIVE_WARN) { error = QString::fromUtf8(archive_error_string(a.get())); return {}; }
        if (archive_entry_filetype(entry) != AE_IFREG || archiveName(entry) != source.member
            || occurrence++ != source.memberOccurrence) { archive_read_data_skip(a.get()); continue; }
        if (archive_entry_size(entry) > MaxEncodedBytes) { error = QStringLiteral("압축 이미지가 입력 한도 256 MiB를 초과합니다."); return {}; }
        QByteArray data;
        if (archive_entry_size(entry) > 0) data.reserve(int(archive_entry_size(entry)));
        char chunk[65536];
        while (!cancelled()) {
            const la_ssize_t count = archive_read_data(a.get(), chunk, sizeof(chunk));
            if (count == 0) return data;
            if (count < 0) { error = QString::fromUtf8(archive_error_string(a.get())); return {}; }
            if (data.size() + count > MaxEncodedBytes) { error = QStringLiteral("압축 이미지가 입력 한도 256 MiB를 초과합니다."); return {}; }
            data.append(chunk, count);
        }
        return {};
    }
    if (!cancelled()) error = QStringLiteral("압축파일에서 해당 이미지를 찾을 수 없습니다.");
    return {};
}
struct Decoded {
    QImage image;
    ImageInfo info;
    QString error;
    bool animated = false;
    QByteArray encoded;
    qint64 cost() const { return image.sizeInBytes() + encoded.size(); }
};
Decoded decode(const Entry &source, int page, const Epoch &epoch, quint64 generation, bool clampPage = false,
               const std::shared_ptr<std::atomic_bool> &cancel = {}) {
    Decoded result;
    const auto cancelled = [&] { return epoch->load() != generation || (cancel && cancel->load()); };
    if (cancelled()) return result;
    QElapsedTimer timer;
    timer.start();
    QBuffer buffer;
    std::unique_ptr<QImageReader> reader;
    QByteArray encoded;
    if (source.isArchive()) {
        encoded = memberBytes(source, result.error, epoch, generation, cancel);
        if (!result.error.isEmpty() || cancelled()) return result;
        buffer.setData(encoded);
        buffer.open(QIODevice::ReadOnly);
        reader = std::make_unique<QImageReader>(&buffer);
    } else {
        if (!isNativePdfSource(source.path) && QFileInfo(source.path).size() > MaxEncodedBytes) {
            result.error = QStringLiteral("이미지가 입력 한도 256 MiB를 초과합니다."); return result;
        }
        reader = std::make_unique<QImageReader>(source.path);
    }
    // PDF bypasses QImageReader/ImageIO and optional Exiv2. Its native URL
    // provider reads lazily, and page rasterization runs in this worker only.
    if (isNativePdfSource(source.isArchive() ? source.member : source.path, encoded)) {
        auto native = decodeNativeImage(source.path, encoded, page);
        if (clampPage && native.image.isNull() && native.pageCount > 0 && page >= native.pageCount) {
            page = native.pageCount - 1;
            native = decodeNativeImage(source.path, encoded, page);
        }
        if (cancelled()) return {};
        result.image = native.image;
        result.error = native.error;
        result.info.format = QStringLiteral("PDF");
        result.info.pdf = true;
        result.info.page = page;
        result.info.pageCount = native.pageCount;
        result.info.pageSizePoints = native.pageSizePoints;
        result.info.fileBytes = source.bytes;
        result.info.metadata = native.metadata;
        result.info.size = result.image.size();
        result.info.decodeMs = timer.elapsed();
        return result;
    }
    reader->setAutoTransform(true);
    reader->setDecideFormatFromContent(true);
    // Query count before size: the TIFF plugin may leave its device positioned
    // past the header while answering size(), which breaks a later count scan.
    const int qtPageCount = reader->imageCount();
    result.info.pageCount = std::max(1, qtPageCount);
    const QSize size = reader->size();
    if (size.isValid() && qint64(size.width()) * size.height() > MaxPixels) {
        result.error = QStringLiteral("이미지가 할당 한도 128 메가픽셀을 초과합니다."); return result;
    }
    result.info.format = QString::fromLatin1(reader->format()).toUpper();
    result.info.fileBytes = source.bytes;
    result.animated = reader->supportsAnimation();
    result.info.animated = result.animated;
    if (result.animated) result.info.pageCount = 1;
    if (clampPage && qtPageCount > 0) page = std::clamp(page, 0, result.info.pageCount - 1);
    result.info.page = page;
    // Some sequential handlers (notably libpng) cannot seek back to metadata
    // once read() has consumed IEND. Query text before consuming pixels.
    for (const auto &key : reader->textKeys()) {
        const QString value = reader->text(key);
        if (!value.isEmpty()) result.info.metadata.append(key + QStringLiteral(": ") + value.left(4096));
        if (result.info.metadata.size() >= 40) break;
    }
    const bool qtPageReady = page == 0 || reader->jumpToImage(page);
    if (cancelled()) return result;
    if (qtPageReady) result.image = reader->read();
    if (result.image.isNull()) {
        NativeDecoded native = decodeNativeImage(source.path, encoded, page);
        if (clampPage && native.image.isNull() && native.pageCount > 0 && page >= native.pageCount) {
            page = native.pageCount - 1;
            native = decodeNativeImage(source.path, encoded, page);
            result.info.page = page;
        }
        if (native.image.isNull()) {
            result.error = native.error.isEmpty() ? QStringLiteral("이미지를 읽을 수 없습니다: %1").arg(reader->errorString()) : native.error;
            return result;
        }
        result.image = native.image;
        result.info.format = QFileInfo(source.name).suffix().toUpper() + QStringLiteral(" · ImageIO");
        result.info.pageCount = native.pageCount;
        result.info.metadata.append(native.metadata);
        result.animated = false;
        result.info.animated = false;
    }
#ifdef IKVIEW_HAS_EXIV2
    if (!cancelled()) {
        try {
            Exiv2::Image::UniquePtr metadata;
            if (source.isArchive()) metadata = Exiv2::ImageFactory::open(reinterpret_cast<const Exiv2::byte *>(encoded.constData()), encoded.size());
            else {
                // Read through QFile so non-ASCII Windows paths are supported independently of the Exiv2 CRT.
                QFile file(source.path);
                if (file.size() <= 32LL * 1024 * 1024 && file.open(QIODevice::ReadOnly)) {
                    encoded = file.readAll();
                    metadata = Exiv2::ImageFactory::open(reinterpret_cast<const Exiv2::byte *>(encoded.constData()), encoded.size());
                }
            }
            if (metadata) {
                metadata->readMetadata();
                int count = 0;
                for (const auto &datum : metadata->exifData()) {
                    result.info.metadata.append(QString::fromStdString(datum.key()) + QStringLiteral(": ") + QString::fromStdString(datum.toString()).left(1024));
                    if (++count >= 80) break;
                }
            }
        } catch (const Exiv2::Error &) { /* Metadata is optional; pixel decoding remains usable. */ }
        catch (const std::exception &) {}
    }
#endif
    if (cancelled()) return {};
    if (result.image.colorSpace().isValid() && result.image.colorSpace() != QColorSpace(QColorSpace::SRgb))
        result.image = result.image.convertedToColorSpace(QColorSpace::SRgb);
    result.image = result.image.convertToFormat(QImage::Format_RGBA8888);
    result.info.size = result.image.size();
    result.info.decodeMs = timer.elapsed();
    if (result.animated && source.isArchive()) result.encoded = encoded;
    return result;
}
}

struct Document::Impl {
    Document *q;
    QThreadPool pool;
    QVector<Entry> list;
    QStringList sourcePaths;
    bool sourceIncludesSiblings = true;
    QString container;
    int index = -1;
    bool busy = false;
    bool scanning = false;
    QStringList pendingPaths;
    QString pendingSelected;
    int pendingPreviousIndex = 0;
    int pendingPreviousPage = 0;
    bool pendingPreserve = true;
    bool pendingIncludesSiblings = true;
    bool pendingRefresh = false;
    bool paused = false;
    SortOrder sort = SortOrder::Name;
    QImage current;
    ImageInfo info;
    QMovie *movie = nullptr;
    QBuffer *movieBuffer = nullptr;
    QFileSystemWatcher *watcher;
    QTimer *refresh;
    Epoch scanEpoch = std::make_shared<std::atomic<quint64>>(0);
    Epoch decodeEpoch = std::make_shared<std::atomic<quint64>>(0);
    struct Cached { Decoded decoded; quint64 touch = 0; QString source; };
    QHash<QString, Cached> cache;
    QHash<QString, FileStamp> stamps;
    QSet<QString> directorySources;
    qint64 cacheLimit = 256LL * 1024 * 1024;
    qint64 cacheBytes = 0;
    quint64 clock = 0;
    QSet<QString> pending;
    QHash<QString, QString> prefetchErrors;
    QString lastError;
    bool monitoringLimited = false;

    explicit Impl(Document *owner) : q(owner), watcher(new QFileSystemWatcher(owner)), refresh(new QTimer(owner)) {
        pool.setMaxThreadCount(2);
        pool.setExpiryTimeout(10000);
        refresh->setSingleShot(true);
        refresh->setInterval(180);
        QObject::connect(watcher, &QFileSystemWatcher::directoryChanged, refresh, qOverload<>(&QTimer::start));
        QObject::connect(watcher, &QFileSystemWatcher::fileChanged, refresh, qOverload<>(&QTimer::start));
        QObject::connect(refresh, &QTimer::timeout, q, [this] {
            if (sourcePaths.isEmpty() || (scanning && !pendingRefresh)) return;
            const QString selected = entry().key();
            scan(sourcePaths, selected, index, false, sourceIncludesSiblings, info.page, true);
        });
    }
    Entry entry() const { return index >= 0 && index < list.size() ? list[index] : Entry{}; }
    void reportError(const QString &message) {
        if (message.isEmpty() || message == lastError) return;
        lastError = message;
        emit q->errorOccurred(message);
    }
    void setBusy(bool value) { if (busy != value) { busy = value; emit q->loadingChanged(value); } }
    void stopMovie() {
        if (movie) { movie->stop(); delete movie; movie = nullptr; }
        if (movieBuffer) { delete movieBuffer; movieBuffer = nullptr; }
    }
    void clearCache() { cache.clear(); cacheBytes = 0; pending.clear(); prefetchErrors.clear(); emit q->cacheUsageChanged(0); }
    void trimCache() {
        while (cacheBytes > cacheLimit && !cache.isEmpty()) {
            auto oldest = cache.begin();
            for (auto it = cache.begin(); it != cache.end(); ++it) if (it->touch < oldest->touch) oldest = it;
            cacheBytes -= oldest->decoded.cost();
            cache.erase(oldest);
        }
        emit q->cacheUsageChanged(cacheBytes);
    }
    void remember(const Entry &source, const Decoded &image) {
        if (image.image.isNull() || image.cost() > cacheLimit) return;
        const QString key = source.key();
        auto old = cache.find(key);
        if (old != cache.end()) cacheBytes -= old->decoded.cost();
        cache.insert(key, {image, ++clock, source.isArchive() ? source.archivePath : source.path});
        cacheBytes += image.cost();
        trimCache();
    }
    void setWatchPaths() {
        monitoringLimited = false;
        QStringList wanted;
        QSet<QString> desired;
        constexpr int maximumWatches = 128;
        const auto add = [&](const QString &path) {
            if (path.isEmpty() || desired.contains(path)) return true;
            if (wanted.size() == maximumWatches) { monitoringLimited = true; return false; }
            desired.insert(path); wanted.append(path); return true;
        };
        if (sourcePaths.size() == 1) {
            add(container);
            if (sourceIncludesSiblings && Document::isArchivePath(container)) add(QFileInfo(container).absolutePath());
        } else if (!sourcePaths.isEmpty()) {
            // Explicit multi-file access does not grant parent directories.
            // Keep the current file first when a large selection exceeds the
            // bounded watcher budget or the operating system's watch limit.
            const auto currentEntry = entry();
            add(currentEntry.isArchive() ? currentEntry.archivePath : currentEntry.path);
            for (const auto &path : sourcePaths) if (!add(path)) break;
        }
        const QStringList existing = watcher->directories() + watcher->files();
        const QSet<QString> registered(existing.begin(), existing.end());
        QStringList removed, added;
        for (const auto &path : existing) if (!desired.contains(path)) removed.append(path);
        for (const auto &path : wanted) if (!registered.contains(path)) added.append(path);
        if (!removed.isEmpty()) watcher->removePaths(removed);
        if (!added.isEmpty()) monitoringLimited |= !watcher->addPaths(added).isEmpty();
    }
    void scan(const QStringList &paths, const QString &selected, int previousIndex = 0, bool preserveOnFailure = true,
              bool includeSiblingFiles = true, int previousPage = 0, bool refreshing = false) {
        const quint64 generation = ++*scanEpoch;
        if (!refreshing) { ++*decodeEpoch; stopMovie(); }
        lastError.clear();
        refresh->stop();
        if (!refreshing) {
            if (!watcher->directories().isEmpty()) watcher->removePaths(watcher->directories());
            if (!watcher->files().isEmpty()) watcher->removePaths(watcher->files());
        }
        scanning = true;
        pendingPaths = paths;
        pendingSelected = selected;
        pendingPreviousIndex = previousIndex;
        pendingPreviousPage = previousPage;
        pendingPreserve = preserveOnFailure;
        pendingIncludesSiblings = includeSiblingFiles;
        pendingRefresh = refreshing;
        if (!refreshing) setBusy(true);
        QSet<QString> extensions;
        for (const auto &extension : Document::supportedExtensions()) extensions.insert(extension);
        auto *future = new QFutureWatcher<ScanResult>(q);
        QObject::connect(future, &QFutureWatcher<ScanResult>::finished, q,
                         [this, future, generation, selected, previousIndex, previousPage, paths, preserveOnFailure, includeSiblingFiles, refreshing] {
            const ScanResult result = future->result();
            future->deleteLater();
            if (scanEpoch->load() != generation) return;
            scanning = false;
            pendingPaths.clear();
            if (refreshing && result.unchanged && !current.isNull()) { setWatchPaths(); return; }
            if (result.entries.isEmpty() && preserveOnFailure && !list.isEmpty()) {
                setBusy(false);
                setWatchPaths();
                reportError(result.error.isEmpty() ? QStringLiteral("표시할 이미지가 없습니다.") : result.error);
                // An invalid new open must leave the previous document usable.
                if (!current.isNull()) {
                    emit q->imageReady(current, info);
                    const auto previous = cache.constFind(entry().key());
                    if (previous != cache.cend()) startMovie(previous->decoded, decodeEpoch->load());
                }
                else if (index >= 0) load(0);
                return;
            }
            const QString selectedKey = refreshing ? entry().key() : selected;
            const int selectedIndex = refreshing ? index : previousIndex;
            const int selectedPage = refreshing ? info.page : previousPage;
            const Entry previousEntry = entry();
            const auto source = previousEntry.isArchive() ? previousEntry.archivePath : previousEntry.path;
            const bool unchangedImage = refreshing && !current.isNull() && !info.animated
                && stamps.contains(source) && result.stamps.contains(source) && stamps.value(source) == result.stamps.value(source);
            Decoded retained;
            if (unchangedImage) { retained.image = current; retained.info = info; }
            if (refreshing) {
                ++*decodeEpoch; stopMovie(); pending.clear(); prefetchErrors.clear();
                for (auto cached = cache.begin(); cached != cache.end();) {
                    if (!result.stamps.contains(cached->source) || !stamps.contains(cached->source)
                        || result.stamps.value(cached->source) != stamps.value(cached->source)) {
                        cacheBytes -= cached->decoded.cost(); cached = cache.erase(cached);
                    } else ++cached;
                }
                emit q->cacheUsageChanged(cacheBytes);
            } else clearCache();
            current = {};
            info = {};
            list = result.entries;
            container = result.container;
            sourcePaths = paths;
            sourceIncludesSiblings = includeSiblingFiles;
            stamps = result.stamps;
            directorySources = result.directorySources;
            // Once an ordinary file is opened, the directory is the watched
            // collection. Deleting that initially selected file must not make
            // subsequent refreshes fail the "file exists" check.
            if (includeSiblingFiles && sourcePaths.size() == 1 && !Document::isArchivePath(container) && !container.isEmpty())
                sourcePaths = {container};
            index = -1;
            setWatchPaths();
            emit q->entriesChanged();
            if (!result.error.isEmpty()) reportError(result.error);
            if (list.isEmpty()) { setBusy(false); emit q->currentChanged(-1); return; }
            int target = std::clamp(selectedIndex, 0, int(list.size()) - 1);
            bool retainedSelection = false;
            if (!selectedKey.isEmpty()) for (int i = 0; i < list.size(); ++i) if (list[i].key() == selectedKey) { target = i; retainedSelection = true; break; }
            // A folder change can be an unrelated settings/non-image write.
            // Decode the retained page directly: emitting page zero first can
            // overwrite saved positions and trigger another watcher refresh.
            if (retainedSelection && unchangedImage) {
                index = target; current = retained.image; info = retained.info;
                emit q->currentChanged(index);
                deliver(retained, decodeEpoch->load(), false);
            } else select(target, retainedSelection ? selectedPage : 0, retainedSelection && selectedPage > 0);
        });
        const Epoch epoch = scanEpoch;
        const SortOrder order = sort;
        const auto previousEntries = refreshing ? list : QVector<Entry>();
        const auto previousStamps = refreshing ? stamps : QHash<QString, FileStamp>();
        const auto previousContainer = container;
        future->setFuture(QtConcurrent::run(&pool, [paths, extensions, order, epoch, generation, includeSiblingFiles, selected,
                                                  refreshing, previousEntries, previousStamps, previousContainer] {
            auto result = scanPaths(paths, extensions, order, epoch, generation, includeSiblingFiles, selected);
            result.unchanged = refreshing && result.error.isEmpty() && result.container == previousContainer
                && result.entries == previousEntries && result.stamps == previousStamps;
            return result;
        }));
    }
    void startMovie(const Decoded &decoded, quint64 generation) {
        if (!decoded.animated || !QMovie::supportedFormats().contains(decoded.info.format.toLatin1().toLower())) return;
        const Entry source = entry();
        if (source.isArchive()) {
            movieBuffer = new QBuffer(q);
            movieBuffer->setData(decoded.encoded);
            movieBuffer->open(QIODevice::ReadOnly);
            movie = new QMovie(movieBuffer, QByteArray(), q);
        } else movie = new QMovie(source.path, QByteArray(), q);
        movie->setCacheMode(QMovie::CacheNone);
        QObject::connect(movie, &QMovie::frameChanged, q, [this, generation](int) {
            if (decodeEpoch->load() != generation || !movie) return;
            QImage frame = movie->currentImage();
            if (frame.isNull()) return;
            current = frame.convertToFormat(QImage::Format_RGBA8888);
            info.size = current.size();
            emit q->imageReady(current, info);
        });
        movie->start();
        if (paused) movie->setPaused(true);
    }
    void deliver(const Decoded &decoded, quint64 generation, bool animate) {
        current = decoded.image;
        info = decoded.info;
        setBusy(false);
        if (!decoded.error.isEmpty()) { reportError(decoded.error); return; }
        lastError.clear();
        emit q->imageReady(current, info);
        if (animate) startMovie(decoded, generation);
        prefetch(generation);
    }
    void prefetch(quint64 generation) {
        for (const int offset : {1, -1}) {
            const int neighbor = index + offset;
            if (neighbor < 0 || neighbor >= list.size()) continue;
            const Entry source = list[neighbor];
            const QString key = source.key();
            if (auto found = cache.find(key); found != cache.end()) {
                emit q->neighborReady(neighbor, found->decoded.image);
                continue;
            }
            if (pending.contains(key)) continue;
            pending.insert(key);
            auto *future = new QFutureWatcher<Decoded>(q);
            QObject::connect(future, &QFutureWatcher<Decoded>::finished, q, [this, future, generation, key, source] {
                Decoded decoded = future->result();
                future->deleteLater();
                if (decodeEpoch->load() != generation) return;
                pending.remove(key);
                if (decoded.image.isNull()) {
                    const auto error = decoded.error.isEmpty() ? QStringLiteral("주변 이미지를 읽을 수 없습니다.") : decoded.error;
                    prefetchErrors.insert(key, error);
                    for (int i = 0; i < list.size(); ++i) if (list[i].key() == key) { emit q->neighborFailed(i, error); break; }
                    return;
                }
                prefetchErrors.remove(key);
                remember(source, decoded);
                for (int i = 0; i < list.size(); ++i) {
                    if (list[i].key() == key) { emit q->neighborReady(i, decoded.image); break; }
                }
            });
            const Epoch epoch = decodeEpoch;
            future->setFuture(QtConcurrent::run(&pool, [source, epoch, generation] { return decode(source, 0, epoch, generation); }));
        }
    }
    void select(int target, int page = 0, bool clampPage = false) {
        auto cached = cache.find(list[target].key());
        if (page == 0 && cached != cache.end()) {
            const Decoded decoded = cached->decoded;
            cached->touch = ++clock;
            const quint64 generation = ++*decodeEpoch;
            pending.clear(); prefetchErrors.clear(); stopMovie(); lastError.clear();
            index = target; current = decoded.image; info = decoded.info;
            if (sourcePaths.size() > 1) setWatchPaths();
            // This transition completes synchronously. Observers receive the
            // selected pixels before currentChanged and never see a false
            // loading cycle that disables/re-enables every toolbar action.
            setBusy(false);
            emit q->currentChanged(index);
            deliver(decoded, generation, true);
            return;
        }
        index = target;
        current = {};
        info = {};
        setBusy(true);
        emit q->currentChanged(index);
        if (sourcePaths.size() > 1) setWatchPaths();
        load(page, clampPage);
    }
    void load(int page, bool clampPage = false) {
        const Entry source = entry();
        if (source.key().isEmpty()) return;
        const quint64 generation = ++*decodeEpoch;
        pending.clear();
        prefetchErrors.clear();
        stopMovie();
        lastError.clear();
        current = {};
        // Retain multipage metadata while requesting another page so rapid
        // navigation can target the next page without temporarily seeing 1/1.
        info.page = page;
        setBusy(true);
        if (page == 0) {
            auto found = cache.find(source.key());
            if (found != cache.end()) {
                found->touch = ++clock;
                const Decoded decoded = found->decoded;
                deliver(decoded, generation, true);
                return;
            }
        }
        auto *future = new QFutureWatcher<Decoded>(q);
        QObject::connect(future, &QFutureWatcher<Decoded>::finished, q, [this, future, generation, source] {
            const Decoded decoded = future->result();
            future->deleteLater();
            if (decodeEpoch->load() != generation) return;
            if (decoded.info.page == 0) remember(source, decoded);
            deliver(decoded, generation, decoded.info.page == 0);
        });
        const Epoch epoch = decodeEpoch;
        future->setFuture(QtConcurrent::run(&pool, [source, page, epoch, generation, clampPage] { return decode(source, page, epoch, generation, clampPage); }));
    }
};

Document::Document(QObject *parent) : QObject(parent), d(std::make_unique<Impl>(this)) {
    QImageReader::setAllocationLimit(512);
    qRegisterMetaType<ik::ImageInfo>();
}
Document::~Document() {
    ++*d->scanEpoch;
    ++*d->decodeEpoch;
    d->stopMovie();
    // Worker closures contain values only. Finish them before destroying their result watchers.
    d->pool.waitForDone();
}
void Document::open(const QString &path) {
    if (path.isEmpty()) return;
    openFiles({path});
}
void Document::openFiles(const QStringList &paths, bool includeSiblingFiles) {
    QStringList absolutePaths;
    for (const auto &path : paths) if (!path.isEmpty()) absolutePaths.append(QDir::cleanPath(QFileInfo(path).absoluteFilePath()));
    absolutePaths.removeDuplicates();
    if (absolutePaths.size() == 1) {
        const QFileInfo info(absolutePaths.first());
        d->scan(absolutePaths, info.isFile() && !isArchivePath(absolutePaths.first()) ? info.absoluteFilePath() : QString(),
                0, true, (includeSiblingFiles && paths.size() == 1) || info.isDir());
        return;
    }
    // A multi-selection is an explicit collection. If it later shrinks to one
    // file, a refresh must not suddenly enumerate that file's parent directory.
    if (!absolutePaths.isEmpty()) d->scan(absolutePaths, absolutePaths.first(), 0, true, false);
}
void Document::close() {
    ++*d->scanEpoch;
    ++*d->decodeEpoch;
    d->refresh->stop();
    d->stopMovie();
    d->paused = false;
    d->scanning = false;
    d->pendingPaths.clear();
    d->pendingIncludesSiblings = true;
    if (!d->watcher->directories().isEmpty()) d->watcher->removePaths(d->watcher->directories());
    if (!d->watcher->files().isEmpty()) d->watcher->removePaths(d->watcher->files());
    d->sourcePaths.clear();
    d->stamps.clear(); d->directorySources.clear();
    d->sourceIncludesSiblings = true;
    d->container.clear();
    d->current = {};
    d->info = {};
    d->list.clear();
    d->index = -1;
    d->lastError.clear();
    d->monitoringLimited = false;
    d->clearCache();
    d->setBusy(false);
    emit entriesChanged();
    emit currentChanged(-1);
}
void Document::navigate(int delta) { if (!d->list.isEmpty()) goTo(std::clamp(d->index + delta, 0, int(d->list.size()) - 1)); }
void Document::goTo(int index) {
    if (index < 0 || index >= d->list.size()) return;
    if (d->index == index && !d->current.isNull() && d->info.page == 0) return;
    d->select(index);
}
void Document::setPage(int page) {
    if (page < 0 || page >= d->info.pageCount || d->info.page == page) return;
    d->load(page);
}
void Document::adjacentContainer(int direction) {
    if (d->container.isEmpty() || direction == 0) return;
    // File-scoped sessions have no permission to enumerate neighboring folders.
    if (!d->sourceIncludesSiblings || d->sourcePaths.size() != 1) return;
    const QFileInfo current(d->container);
    const QString parent = current.absolutePath();
    const QString currentPath = current.absoluteFilePath();
    const Epoch epoch = d->scanEpoch;
    const quint64 generation = epoch->load();
    auto *future = new QFutureWatcher<QString>(this);
    connect(future, &QFutureWatcher<QString>::finished, this, [this, future, generation] {
        const QString target = future->result();
        future->deleteLater();
        if (d->scanEpoch->load() == generation && !target.isEmpty()) open(target);
    });
    future->setFuture(QtConcurrent::run(&d->pool, [parent, currentPath, direction, epoch, generation] {
        QStringList containers;
        QDirIterator iterator(parent, QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot | QDir::Readable);
        while (epoch->load() == generation && iterator.hasNext()) {
            iterator.next();
            const auto info = iterator.fileInfo();
            if (info.isDir() || Document::isArchivePath(info.filePath())) containers.append(info.absoluteFilePath());
        }
        QCollator collator;
        collator.setNumericMode(true);
        collator.setCaseSensitivity(Qt::CaseInsensitive);
        std::sort(containers.begin(), containers.end(), [&](const auto &a, const auto &b) { return collator.compare(a, b) < 0; });
        const int position = containers.indexOf(currentPath);
        const int next = position + (direction > 0 ? 1 : -1);
        return position >= 0 && next >= 0 && next < containers.size() ? containers[next] : QString();
    }));
}
void Document::setSortOrder(SortOrder order) {
    d->sort = order;
    if (d->scanning && !d->pendingPaths.isEmpty()) {
        const auto paths = d->pendingPaths;
        const auto selected = d->pendingSelected;
        d->scan(paths, selected, d->pendingPreviousIndex, d->pendingPreserve, d->pendingIncludesSiblings, d->pendingPreviousPage, d->pendingRefresh);
        return;
    }
    if (d->list.isEmpty()) {
        if (d->busy && !d->sourcePaths.isEmpty())
            d->scan(d->sourcePaths, QString(), 0, true, d->sourceIncludesSiblings);
        return;
    }
    const quint64 generation = ++*d->scanEpoch;
    auto entries = d->list;
    auto *future = new QFutureWatcher<QVector<Entry>>(this);
    connect(future, &QFutureWatcher<QVector<Entry>>::finished, this, [this, future, generation] {
        const auto sorted = future->result();
        future->deleteLater();
        if (d->scanEpoch->load() != generation) return;
        const QString selected = d->entry().key();
        d->list = sorted;
        d->index = -1;
        for (int i = 0; i < d->list.size(); ++i) if (d->list[i].key() == selected) { d->index = i; break; }
        emit entriesChanged();
        emit currentChanged(d->index);
        // The UI clears its previous frame on currentChanged even when only
        // the ordering changed. Re-deliver the retained static frame; an
        // in-flight decode will deliver its own result when it completes.
        if (!d->current.isNull()) emit imageReady(d->current, d->info);
        d->prefetch(d->decodeEpoch->load());
    });
    future->setFuture(QtConcurrent::run(&d->pool, [entries, order]() mutable {
        sortEntries(entries, order);
        return entries;
    }));
}
void Document::setCacheLimitMB(int mb) { d->cacheLimit = qint64(std::clamp(mb, 32, 2048)) * 1024 * 1024; d->trimCache(); }
void Document::setAnimationPaused(bool paused) { d->paused = paused; if (d->movie) d->movie->setPaused(paused); }
void Document::invalidateCurrent() {
    const QString key = d->entry().key();
    auto found = d->cache.find(key);
    if (found != d->cache.end()) { d->cacheBytes -= found->decoded.cost(); d->cache.erase(found); emit cacheUsageChanged(d->cacheBytes); }
    if (d->index >= 0) d->load(0);
}
void Document::removeEntry(const QString &key) {
    int removed = -1;
    for (int i = 0; i < d->list.size(); ++i) if (d->list[i].key() == key) { removed = i; break; }
    if (removed < 0) return;
    const Entry removedEntry = d->list[removed];
    const QString previousKey = d->entry().key();
    const int previousPage = d->info.page;
    ++*d->scanEpoch;
    ++*d->decodeEpoch;
    d->refresh->stop();
    d->scanning = false;
    d->pendingPaths.clear();
    d->stopMovie();
    if (!removedEntry.isArchive()) d->sourcePaths.removeAll(removedEntry.path);
    if (d->sourcePaths.isEmpty()) d->container.clear();
    else if (d->sourcePaths.size() == 1 && !d->sourceIncludesSiblings) d->container = d->sourcePaths.first();
    d->list.removeAt(removed);
    auto cached = d->cache.find(key);
    if (cached != d->cache.end()) { d->cacheBytes -= cached->decoded.cost(); d->cache.erase(cached); emit cacheUsageChanged(d->cacheBytes); }
    d->pending.clear();
    d->current = {}; d->info = {};
    d->index = -1;
    d->lastError.clear();
    int target = -1;
    for (int i = 0; i < d->list.size(); ++i) if (d->list[i].key() == previousKey) { target = i; break; }
    const bool currentWasRemoved = target < 0;
    if (target < 0 && !d->list.isEmpty()) target = std::min(removed, int(d->list.size()) - 1);
    emit entriesChanged();
    if (target < 0) {
        d->setWatchPaths(); d->setBusy(false); emit currentChanged(-1); return;
    }
    d->index = target;
    d->setBusy(true);
    emit currentChanged(target);
    d->setWatchPaths();
    d->load(currentWasRemoved ? 0 : previousPage);
}
void Document::refreshCollection() {
    // A new open owns pendingPaths. Refreshing the old committed collection
    // must not cancel that request or leave its loading state stranded.
    if (d->sourcePaths.isEmpty() || (d->scanning && !d->pendingRefresh)) return;
    const auto paths = d->sourcePaths;
    d->scan(paths, d->entry().key(), d->index, false, d->sourceIncludesSiblings, d->info.page, true);
}
int Document::currentIndex() const { return d->index; }
int Document::count() const { return d->list.size(); }
const QVector<Entry> &Document::entries() const { return d->list; }
Entry Document::currentEntry() const { return d->entry(); }
QStringList Document::collectionPaths() const { return d->sourcePaths; }
QImage Document::currentImage() const { return d->current; }
ImageInfo Document::currentInfo() const { return d->info; }
QImage Document::cachedImage(int index) const {
    if (index < 0 || index >= d->list.size()) return {};
    const auto found = d->cache.constFind(d->list[index].key());
    return found == d->cache.cend() ? QImage() : found->decoded.image;
}
QString Document::neighborError(int index) const {
    return index >= 0 && index < d->list.size() ? d->prefetchErrors.value(d->list[index].key()) : QString();
}
bool Document::isBusy() const { return d->busy; }
bool Document::isFileScoped() const {
    return !d->sourceIncludesSiblings && d->sourcePaths.size() == 1 && !d->directorySources.contains(d->sourcePaths.first());
}
bool Document::isMonitoringLimited() const { return d->monitoringLimited; }
bool Document::isImageFile(const QString &path) {
    const QFileInfo file(path);
    return file.isFile() && (supportedExtensions().contains(file.suffix().toLower())
        || isNativePdfSource(path) || !QImageReader::imageFormat(path).isEmpty());
}
QImage Document::decodeEntry(const Entry &entry, int page, QString *error, ImageInfo *info,
                             const std::shared_ptr<std::atomic_bool> &cancelled) {
    const Epoch epoch = std::make_shared<std::atomic<quint64>>(0);
    const auto decoded = decode(entry, page, epoch, 0, false, cancelled);
    if (error) *error = cancelled && cancelled->load() ? QStringLiteral("변환을 취소했습니다.") : decoded.error;
    if (info) *info = decoded.info;
    return decoded.image;
}
QStringList Document::supportedExtensions() {
    static const QStringList extensions = [] {
        QStringList result = nativeImageExtensions();
        for (const auto &format : QImageReader::supportedImageFormats()) result.append(QString::fromLatin1(format).toLower());
        if (result.contains(QStringLiteral("jpeg"))) result.append(QStringLiteral("jpg"));
        if (result.contains(QStringLiteral("tiff"))) result.append(QStringLiteral("tif"));
        result.removeDuplicates();
        return result;
    }();
    return extensions;
}
bool Document::isArchivePath(const QString &path) {
    static const QSet<QString> formats = {QStringLiteral("zip"), QStringLiteral("cbz"), QStringLiteral("rar"), QStringLiteral("cbr"), QStringLiteral("7z"), QStringLiteral("tar"), QStringLiteral("cbt"), QStringLiteral("lzh"), QStringLiteral("lha")};
    return formats.contains(QFileInfo(path).suffix().toLower());
}
}
