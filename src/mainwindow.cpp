#include "mainwindow.h"
#include "apptheme.h"
#include "preferencesdialog.h"
#include "shortcutmanager.h"
#include "document.h"
#include "dropfiles.h"
#include "imagecanvas.h"
#include "fileoperations.h"
#include "systemintegration.h"
#include "exportdialog.h"
#include "nativeimage.h"
#include "viewericons.h"
#include "viewertoolbutton.h"
#include "dialoglayout.h"
#include "viewerstatusbar.h"
#include <QPlainTextEdit>
#include <QGroupBox>
#include <QGridLayout>

#include <QAction>
#include <QActionGroup>
#include <QAbstractListModel>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QCheckBox>
#include <QCache>
#include <QCryptographicHash>
#include <QStyleHints>
#include <QUuid>
#include <QClipboard>
#include <QCloseEvent>
#include <QColorDialog>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QContextMenuEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFutureWatcher>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QImageWriter>
#include <QImageReader>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QKeyEvent>
#include <QListWidget>
#include <QListView>
#include <QMenu>
#include <QMenuBar>
#include <QMimeData>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPrintDialog>
#include <QPrinter>
#include <QProcess>
#include <QProgressBar>
#include <QPushButton>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScrollBar>
#include <QScopedValueRollback>
#include <QSettings>
#include <QShortcut>
#include <QSignalBlocker>
#include <QSortFilterProxyModel>
#include <QSet>
#include <QSpinBox>
#include <QSplitter>
#include <QStandardPaths>
#include <QStatusBar>
#include <QStyle>
#include <QTimer>
#include <QThreadPool>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QWindow>
#include <QUrl>
#include <QVBoxLayout>
#include <QtConcurrent/QtConcurrentRun>
#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <QFontMetrics>
#include <QMenuBar>
#include <QTextStream>
#include <QPointer>
#include <functional>
#include <utility>
#ifdef Q_OS_UNIX
#include <sys/stat.h>
#endif

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace ik {
namespace {
std::unique_ptr<QSettings> viewerSettings() {
    QString path = qEnvironmentVariable("IKVIEW_SETTINGS_PATH");
    if (path.isEmpty()) path = qEnvironmentVariable("IKVIEW_SETTINGS_FILE");
    return path.isEmpty() ? std::make_unique<QSettings>("ikview", "ikview")
                          : std::make_unique<QSettings>(path, QSettings::IniFormat);
}
struct OperationState {
    std::atomic<bool> cancelled {false};
    std::atomic<bool> committing {false};
    std::atomic<qint64> completed {0};
    std::atomic<qint64> total {0};
    std::mutex phaseMutex;
    bool requestCancel() { const std::lock_guard lock(phaseMutex); if (committing.load()) return false; cancelled.store(true); return true; }
    bool beginCommit() { const std::lock_guard lock(phaseMutex); if (cancelled.load()) return false; committing.store(true); return true; }
};
struct OperationResult { QString error; bool cancelled = false; };
struct FileIdentity {
    bool valid = false;
    quint64 device = 0, inode = 0;
    qint64 bytes = -1, modifiedSeconds = 0, modifiedNanos = 0;
    friend bool operator==(const FileIdentity &, const FileIdentity &) = default;
};
FileIdentity fileIdentity(const QFile &file, bool openedHandle) {
#ifdef Q_OS_UNIX
    struct stat status {};
    const auto name = QFile::encodeName(file.fileName());
    const bool valid = openedHandle ? ::fstat(int(file.handle()), &status) == 0 : ::stat(name.constData(), &status) == 0;
    if (!valid) return {};
#ifdef Q_OS_MACOS
    const auto modified = status.st_mtimespec;
#else
    const auto modified = status.st_mtim;
#endif
    return {true, quint64(status.st_dev), quint64(status.st_ino), qint64(status.st_size), qint64(modified.tv_sec), qint64(modified.tv_nsec)};
#else
    Q_UNUSED(openedHandle);
    const QFileInfo info(file); return {info.exists(), 0, 0, info.size(), info.lastModified().toMSecsSinceEpoch(), 0};
#endif
}
struct PendingRestore {
    QString source;
    QString member;
    int occurrence = 0;
    int index = -1;
    int page = -1;
    quint64 serial = 0;
    bool byIndex = false;
    bool active() const { return !source.isEmpty(); }
};
QString bytesText(qint64 bytes) {
    if (bytes >= 1024LL * 1024 * 1024) return QString::number(double(bytes) / (1024 * 1024 * 1024), 'f', 2) + " GB";
    if (bytes >= 1024 * 1024) return QString::number(double(bytes) / (1024 * 1024), 'f', 1) + " MB";
    if (bytes >= 1024) return QString::number(double(bytes) / 1024, 'f', 1) + " KB";
    return QString::number(bytes) + " B";
}

template<class T> void useSymbol(T *target, const QString &symbol) {
    target->setProperty("ikviewSymbol", symbol); target->setIcon(viewerIcon(symbol, QApplication::palette().color(QPalette::ButtonText)));
}



bool needsExplicitFolderSelection(const QString &path) {
#ifdef Q_OS_MACOS
    // A file selected in an Open panel or dropped from Finder grants access to
    // that file, not necessarily permission to enumerate its protected parent.
    const QString absolute = QFileInfo(path).absoluteFilePath();
    QStringList protectedRoots {QStringLiteral("/Volumes")};
    for (const auto location : {QStandardPaths::DocumentsLocation, QStandardPaths::DesktopLocation, QStandardPaths::DownloadLocation}) {
        const auto root = QStandardPaths::writableLocation(location);
        if (!root.isEmpty()) protectedRoots.append(QDir::cleanPath(root));
    }
    for (const auto &root : protectedRoots)
        if (absolute == root || absolute.startsWith(root + '/')) return true;
#else
    Q_UNUSED(path);
#endif
    return false;
}

// The view asks only for visible rows. Even a directory containing hundreds of
// thousands of entries does not allocate one QWidget or QListWidgetItem per file.
class ImageListModel final : public QAbstractListModel {
public:
    explicit ImageListModel(Document *document, QObject *parent) : QAbstractListModel(parent), m_document(document) {
        m_pool.setMaxThreadCount(2); m_pool.setExpiryTimeout(10000);
        m_thumbnailDpr = qApp->devicePixelRatio();
        refreshPlaceholder();
    }
    void refreshPlaceholder() {
        QPixmap placeholder(QSize(72, 54) * m_thumbnailDpr); placeholder.setDevicePixelRatio(m_thumbnailDpr);
        placeholder.fill(Qt::transparent);
        QPainter painter(&placeholder);
        viewerIcon("open", QApplication::palette().color(QPalette::PlaceholderText)).paint(&painter, QRect(24, 15, 24, 24));
        painter.end(); m_placeholder = QIcon(placeholder);
        if (!m_entries.isEmpty()) emit dataChanged(index(0, 0), index(m_entries.size() - 1, 0), {Qt::DecorationRole});
    }
    void setEntries(const QVector<Entry> &entries) {
        beginResetModel(); ++m_generation; m_epoch->store(m_generation); m_entries = entries; m_thumbnails.clear(); m_pending.clear(); m_failed.clear(); endResetModel();
    }
    void setThumbnailsEnabled(bool enabled) {
        if (m_thumbnailsEnabled == enabled) return;
        m_thumbnailsEnabled = enabled; ++m_generation; m_epoch->store(m_generation);
        m_pending.clear(); m_failed.clear(); m_thumbnails.clear();
        if (!m_entries.isEmpty()) emit dataChanged(index(0, 0), index(m_entries.size() - 1, 0));
    }
    void refreshDecoded(int row, const QImage &image) {
        if (!m_thumbnailsEnabled || row < 0 || row >= m_entries.size() || image.isNull()) return;
        const auto key = m_entries[row].key();
        if (m_thumbnails.contains(key)) return;
        m_failed.remove(key);
        queueThumbnail(row, image);
    }
    int rowCount(const QModelIndex &parent = {}) const override { return parent.isValid() ? 0 : int(m_entries.size()); }
    Qt::ItemFlags flags(const QModelIndex &index) const override {
        auto result = QAbstractListModel::flags(index);
        if (index.isValid() && index.row() < m_entries.size() && !m_entries[index.row()].isArchive()) result |= Qt::ItemIsDragEnabled;
        return result;
    }
    QStringList mimeTypes() const override { return {QStringLiteral("text/uri-list")}; }
    QMimeData *mimeData(const QModelIndexList &indexes) const override {
        auto *mime = new QMimeData; QList<QUrl> urls;
        for (const auto &index : indexes) {
            if (!index.isValid() || index.column() != 0 || index.row() >= m_entries.size()) continue;
            const auto &entry = m_entries[index.row()];
            if (!entry.isArchive() && !entry.path.isEmpty()) urls.append(QUrl::fromLocalFile(entry.path));
        }
        mime->setUrls(urls); return mime;
    }
    Qt::DropActions supportedDragActions() const override { return Qt::CopyAction; }
    QVariant data(const QModelIndex &index, int role) const override {
        if (!index.isValid() || index.row() < 0 || index.row() >= m_entries.size()) return {};
        const auto &entry = m_entries[index.row()];
        if (role == Qt::DisplayRole) return entry.name;
        if (role == Qt::SizeHintRole) return QSize(180, m_thumbnailsEnabled ? 76 : std::max(32, QFontMetrics(qApp->font()).height() + 16));
        if (role == Qt::ToolTipRole) return entry.displayPath();
        if (role == Qt::EditRole) return entry.name;
        if (role == Qt::UserRole) return index.row();
        if (role == Qt::UserRole + 1) return entry.key();
        if (role == Qt::DecorationRole) {
            if (!m_thumbnailsEnabled) return {};
            if (const auto *icon = m_thumbnails.object(entry.key())) return *icon;
            requestThumbnail(index.row());
            return m_placeholder;
        }
        return {};
    }
private:
    void requestThumbnail(int row) const {
        const auto entry = m_entries[row];
        const auto key = entry.key();
        if (m_pending.contains(key) || m_failed.contains(key) || m_pending.size() >= 32) return;
        // Archive thumbnails reuse decoded neighbors, never rescan/decompress an
        // archive merely because a row became visible while scrolling.
        const QImage cached = entry.isArchive() ? m_document->cachedImage(row) : QImage();
        if (entry.isArchive() && cached.isNull()) return;
        queueThumbnail(row, cached);
    }
    void queueThumbnail(int row, const QImage &cached) const {
        const auto entry = m_entries[row];
        const auto key = entry.key();
        if (m_pending.contains(key) || m_pending.size() >= 32) return;
        m_pending.insert(key);
        const quint64 generation = m_generation;
        auto *self = const_cast<ImageListModel *>(this);
        auto *watcher = new QFutureWatcher<QImage>(self);
        QObject::connect(watcher, &QFutureWatcher<QImage>::finished, self, [self, watcher, key, generation, row] {
            const QImage image = watcher->result(); watcher->deleteLater();
            if (self->m_generation != generation) return;
            self->m_pending.remove(key);
            if (!image.isNull()) self->m_thumbnails.insert(key, new QIcon(QPixmap::fromImage(image)), std::max(1, int(image.sizeInBytes() / 1024)));
            else if (self->m_document->currentEntry().key() == key && !self->m_document->currentImage().isNull()) {
                self->queueThumbnail(row, self->m_document->currentImage()); return;
            } else self->m_failed.insert(key);
            if (row < self->rowCount()) emit self->dataChanged(self->index(row, 0), self->index(row, 0), { Qt::DecorationRole });
        });
        const auto epoch = m_epoch;
        const qreal dpr = m_thumbnailDpr;
        const QSize pixelSize(qRound(72 * dpr), qRound(54 * dpr));
        watcher->setFuture(QtConcurrent::run(&m_pool, [entry, cached, epoch, generation, pixelSize, dpr] {
            if (epoch->load() != generation) return QImage();
            if (!cached.isNull()) { auto result = cached.scaled(pixelSize, Qt::KeepAspectRatio, Qt::SmoothTransformation); result.setDevicePixelRatio(dpr); return result; }
            if (entry.path.endsWith(".pdf", Qt::CaseInsensitive)) {
                auto result = decodeNativeImage(entry.path, {}, 0, pixelSize).image;
                if (epoch->load() != generation) return QImage();
                result.setDevicePixelRatio(dpr); return result;
            }
            QImageReader reader(entry.path); reader.setAutoTransform(true);
            const QSize size = reader.size();
            if (qint64(size.width()) * size.height() > 64LL * 1024 * 1024 && !reader.supportsOption(QImageIOHandler::ScaledSize)) return QImage();
            if (size.isValid()) reader.setScaledSize(size.scaled(pixelSize, Qt::KeepAspectRatio));
            if (epoch->load() != generation) return QImage();
            const auto image = reader.read();
            if (epoch->load() != generation || image.isNull()) return QImage();
            auto result = image.scaled(pixelSize, Qt::KeepAspectRatio, Qt::SmoothTransformation); result.setDevicePixelRatio(dpr); return result;
        }));
    }
    QVector<Entry> m_entries;
    Document *m_document;
    mutable QThreadPool m_pool;
    mutable QCache<QString, QIcon> m_thumbnails { 16 * 1024 };
    mutable QSet<QString> m_pending;
    mutable QSet<QString> m_failed;
    quint64 m_generation = 0;
    std::shared_ptr<std::atomic<quint64>> m_epoch = std::make_shared<std::atomic<quint64>>(0);
    bool m_thumbnailsEnabled = true;
    qreal m_thumbnailDpr = 1.;
    QIcon m_placeholder;
};
}

struct MainWindow::Impl {
    MainWindow *q;
    std::unique_ptr<QSettings> settingsStore = viewerSettings();
    QSettings &settings = *settingsStore;
    Document *doc;
    ImageCanvas *canvas;
    QSplitter *splitter;
    QWidget *filePanel;
    QWidget *infoPanel;
    QListView *files;
    ImageListModel *fileModel;
    QSortFilterProxyModel *fileProxy;
    QLineEdit *filter;
    QTreeWidget *metadata;
    QLabel *fileCount;
    QLabel *filename;
    QLabel *details;
    QLabel *position;
    QLabel *zoomLabel;
    QWidget *footer;
    QWidget *errorBanner;
    QLabel *errorText;
    QPushButton *positionButton = nullptr;
    QProgressBar *progress;
    QWidget *sourceHint;
    bool sourceHintDismissed = false;
    QLabel *sourceHintText;
    QPushButton *openSourceFolderButton;
    QLabel *emptyListLabel;
    QWidget *operationPanel;
    QLabel *operationLabel;
    QPushButton *cancelOperationButton;
    QTimer *operationTimer;
    std::shared_ptr<OperationState> operationState;
    bool closingAfterOperation = false;
    QString operationTitle;
    QStringList activePaths;
    QStringList pendingRecentPaths;
    QString recentPositionKey;
    quint64 openSerial = 0;
    PendingRestore pendingRestore;
    QPointer<QObject> draggedMime;
    QStringList draggedPaths;
    QToolBar *toolbar;
    QMenu *recentMenu = nullptr;
    QList<QStringList> recentMenuPaths;
    QTimer *slideshow;
    QTimer *powerTimer;
    QList<QAction *> needsImage;
    QList<QAction *> needsFile;
    QList<QAction *> panActions;
    QMap<QString,QAction *> imageFilterActions;
    QAction *spreadAction;
    QAction *rtlAction;
    QAction *slideshowAction;
    QAction *fullScreenAction;
    QAction *listAction;
    QAction *infoAction;
    QAction *animationAction;
    QAction *invertAction;
    QAction *zoomLockAction;
    QAction *gpsAction;
    QAction *toolbarAction;
    QAction *statusAction;
    QAction *cleanAction;
    QAction *restoreAction;
    QAction *previousAction;
    QAction *nextAction;
    QAction *firstAction;
    QAction *lastAction;
    QAction *goToAction;
    QAction *fitAction;
    QAction *actualAction;
    QAction *widthAction;
    QAction *shrinkAction;
    QAction *zoomInAction;
    QAction *zoomOutAction;
    QAction *nextPageAction;
    QAction *previousPageAction;
    QAction *thumbnailAction;
    QAction *refreshAction;
    QAction *loopSlideshowAction;
    QAction *shuffleSlideshowAction;
    QAction *copyMetadataValueAction;
    QAction *copyMetadataRowAction;
    QAction *copyMetadataAllAction;
    QActionGroup *fitGroup = nullptr;
    QString loadedKey;
    bool warmImageTransition = false;
    bool pendingImageReset = false;

    bool spread = false;
    bool rtl = false;
    bool operationPending = false;
    bool cleanView = false;
    bool syncingSelection = false;
    bool rememberedWindowedClean = false;
    bool wasFullScreen = false;
    QString currentKey;
    QString metadataKey;
    ImageInfo metadataInfo;
    QPointF cachedGps {999, 999};
    qint64 cacheBytes = 0;
    int slideshowSeconds = 3;
    int cacheMB = 256;
    ShortcutManager *shortcuts;
    QList<QAction *> commandActions;
    bool buildingCommands = true;
    bool routingListKeys = false;
    QLabel *listHint = nullptr;
    QToolButton *bookmarksButton = nullptr;
    QAction *slideshowMenuAction = nullptr;
    QToolButton *slideshowButton = nullptr;
    QMenu *slideshowMenu = nullptr;
    QMenu *displayFilterMenu = nullptr;
    QWidget *pdfControls = nullptr;
    QSpinBox *pdfPageNumber = nullptr;
    QLabel *pdfPageCount = nullptr;
    QPointer<ExportDialog> exportDialog;
    QVariantMap pendingBookmarkDeletion;
    quint64 pendingBookmarkSerial = 0;

    explicit Impl(MainWindow *window) : q(window), doc(new Document(window)), canvas(new ImageCanvas(window)), slideshow(new QTimer(window)) {
        slideshowSeconds = std::clamp(settings.value("slideshowSeconds", 3).toInt(), 1, 300);
        shortcuts = new ShortcutManager(q);
        const auto profile = detectSystemProfile();
        if (!settings.contains("cachePreset")) settings.setValue("cachePreset", settings.contains("cacheMB")
            ? cachePresetForLegacyMB(settings.value("cacheMB").toInt(), profile) : QStringLiteral("auto"));
        cacheMB = resolvedCacheMB(settings.value("cachePreset", "auto").toString(), profile);
        doc->setCacheLimitMB(cacheMB);
        slideshow->setSingleShot(true);
        build();
        connectSignals();
        applyPreferences();
        QObject::connect(QGuiApplication::styleHints(), &QStyleHints::colorSchemeChanged, q, [this] {
            if (settings.value("theme", "dark").toString() == "system") QTimer::singleShot(0, q, [this] { applyTheme(); });
        });
        powerTimer = new QTimer(q); powerTimer->setInterval(2000);
        QObject::connect(powerTimer, &QTimer::timeout, q, [this] { refreshPowerPolicy(); });
        powerTimer->start();
        q->resize(1280, 820);
        if (settings.contains("geometry")) q->restoreGeometry(settings.value("geometry").toByteArray());
        if (settings.contains("splitter")) splitter->restoreState(settings.value("splitter").toByteArray());
        listAction->setChecked(settings.value("filePanel", true).toBool());
        infoAction->setChecked(settings.value("infoPanel", false).toBool());
        toolbarAction->setChecked(settings.value("toolbar", true).toBool());
        statusAction->setChecked(settings.value("status", true).toBool());
        zoomLockAction->setChecked(settings.value("zoomLocked", false).toBool());
        const CanvasViewState view {static_cast<FitMode>(std::clamp(settings.value("fitMode", 0).toInt(), 0, 3)), settings.value("viewZoom", 1.).toDouble(), settings.value("viewManualZoom", false).toBool(), zoomLockAction->isChecked()};
        canvas->restoreViewState(view); updateFitActions();
        setSpread(settings.value("spread", false).toBool(), settings.value("rightToLeft", false).toBool());
        applyChrome();
        q->setAcceptDrops(true);
        updateEnabled();
    }

    ~Impl() {
        qApp->removeEventFilter(q);
        // Child widgets can emit selection/geometry signals while their base
        // destructors run, after this state object has already been released.
        for (auto *child : q->findChildren<QObject *>()) QObject::disconnect(child, nullptr, q, nullptr);
        QObject::disconnect(q, nullptr, q, nullptr);
        inhibitDisplaySleep(false);
        settings.setValue("geometry", q->saveGeometry());
        settings.setValue("splitter", splitter->saveState());
        settings.setValue("filePanel", listAction->isChecked());
        settings.setValue("infoPanel", infoAction->isChecked());
        settings.setValue("toolbar", toolbarAction->isChecked());
        settings.setValue("status", statusAction->isChecked());
        settings.setValue("zoomLocked", zoomLockAction->isChecked());
        const auto view = canvas->viewState(); settings.setValue("viewZoom", view.zoom); settings.setValue("viewManualZoom", view.manualZoom); settings.setValue("fitMode", int(view.fitMode));
        settings.setValue("spread", spread);
        settings.setValue("rightToLeft", rtl);
        settings.sync();
    }

    QAction *action(QMenu *menu, const QString &label, const QKeySequence &key, const std::function<void()> &callback) {
        auto *result = new QAction(label, menu ? static_cast<QObject *>(menu) : static_cast<QObject *>(q));
        result->setShortcut(key);
        if (menu) menu->addAction(result);
        q->addAction(result);
        if (buildingCommands && menu != recentMenu) {
            result->setProperty("commandLabel", label);
            result->setProperty("commandCategory", menu ? menu->title().remove('&') : QStringLiteral("탐색"));
            commandActions.append(result);
        }
        QObject::connect(result, &QAction::triggered, q, [callback] { callback(); });
        return result;
    }

    QAction *imageAction(QMenu *menu, const QString &label, const QKeySequence &key, const std::function<void()> &callback, bool fileOnly = false) {
        auto *result = action(menu, label, key, callback);
        if (fileOnly) needsFile << result; else needsImage << result;
        return result;
    }

    QAction *shortcut(const QString &label, const QKeySequence &key, const std::function<void()> &callback) {
        return action(nullptr, label, key, callback);
    }
    void registerShortcuts() {
        previousAction->setShortcuts({QKeySequence("Left"),QKeySequence("Up"),QKeySequence("PgUp"),QKeySequence("Backspace"),QKeySequence("Shift+Space")});
        nextAction->setShortcuts({QKeySequence("Right"),QKeySequence("Down"),QKeySequence("PgDown"),QKeySequence("Space")});
        zoomInAction->setShortcuts({QKeySequence("+"),QKeySequence("=")});
        for (auto *command : std::as_const(commandActions)) {
            const auto label = command->property("commandLabel").toString();
            const auto category = command->property("commandCategory").toString();
            QString id = command->objectName();
            if (id.isEmpty()) {
                id = "command_" + QString::fromLatin1(QCryptographicHash::hash((category + '/' + label).toUtf8(), QCryptographicHash::Sha256).toHex().left(20));
                command->setObjectName(id);
            }
            auto scope = ShortcutScope::Window;
            for (const auto &sequence : command->shortcuts()) {
                if (!sequence.isEmpty() && !(sequence[0].keyboardModifiers() & (Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier))
                    && !(sequence[0].key() >= Qt::Key_F1 && sequence[0].key() <= Qt::Key_F35) && sequence[0].key() != Qt::Key_Escape)
                    scope = ShortcutScope::Viewer;
            }
            if (command == cleanAction) scope = ShortcutScope::Canvas;
            auto displayCategory = category;
#ifdef Q_OS_MACOS
            displayCategory.remove(QRegularExpression("\\([A-Za-z]\\)$"));
#endif
            shortcuts->addAction(id, label, displayCategory, command, command->shortcuts(), scope);
        }
        shortcuts->load(settings);
        QObject::connect(shortcuts, &ShortcutManager::bindingsChanged, q, [this] { updateShortcutLabels(); });
        buildingCommands = false;
        updateShortcutLabels();
    }
    void updateShortcutLabels() {
        const auto native = [](QAction *command) { return command->shortcut().toString(QKeySequence::NativeText); };
        if (listHint) {
            const auto bookmarkKey = native(q->findChild<QAction *>("addBookmarkAction"));
            const auto navigationKeys = native(previousAction) + " / " + native(nextAction);
            listHint->setText(navigationKeys + "  탐색\n" + (bookmarkKey.isEmpty() ? QStringLiteral("책갈피는 상단 버튼에서 확인하세요") : bookmarkKey + QStringLiteral("  책갈피 추가")));
        }
        if (positionButton && goToAction) positionButton->setToolTip(goToAction->toolTip());
        if (auto *hide = q->findChild<QToolButton *>("hideMetadataButton")) {
            const auto key = native(infoAction);
            hide->setToolTip(QStringLiteral("이미지 정보 숨기기") + (key.isEmpty() ? QString() : " (" + key + ")"));
        }
        if (auto *hide = q->findChild<QToolButton *>("hideFileListButton")) {
            const auto key = native(listAction);
            hide->setToolTip(QStringLiteral("이미지 목록 숨기기") + (key.isEmpty() ? QString() : " (" + key + ")"));
        }
    }

    void build() {
        q->setWindowTitle("ikview");
        q->setObjectName("ikviewWindow");
        q->setProperty("documentState", "empty"); q->setProperty("fileOperationPending", false);
        auto *fileMenu = q->menuBar()->addMenu("파일(&F)");
        auto *viewMenu = q->menuBar()->addMenu("보기(&V)");
        auto *imageMenu = q->menuBar()->addMenu("이미지(&I)");
        auto *toolsMenu = q->menuBar()->addMenu("도구(&T)");
        auto *bookmarkMenu = q->menuBar()->addMenu("책갈피(&B)");
        auto *helpMenu = q->menuBar()->addMenu("도움말(&H)");

        auto *openAction = action(fileMenu, "파일 / 폴더 열기…", QKeySequence("Ctrl+O"), [this] { chooseFile(); });
        openAction->setObjectName("openPathsAction");
        useSymbol(openAction, "open");
        recentMenu = fileMenu->addMenu("최근 열기");
        refreshRecent();
        fileMenu->addSeparator();
        imageAction(fileMenu, "탐색기에서 보기", QKeySequence("Ctrl+Return"), [this] { revealCurrent(); });
        imageAction(fileMenu, "외부 편집기로 열기", QKeySequence("Ctrl+E"), [this] { externalEditor(); }, true);
        fileMenu->addSeparator();
        imageAction(fileMenu, "사진 보관함으로 복사…", QKeySequence("Insert"), [this] { storePhoto(false, false); }, true)->setObjectName("copyPhotoAction");
        imageAction(fileMenu, "사진 보관함으로 이동…", QKeySequence("Ctrl+Insert"), [this] { storePhoto(true, false); }, true)->setObjectName("movePhotoAction");
        imageAction(fileMenu, "보조 보관함으로 복사…", QKeySequence("Shift+Insert"), [this] { storePhoto(false, true); }, true);
        fileMenu->addSeparator();
        imageAction(fileMenu, "휴지통으로 이동…", QKeySequence("Delete"), [this] { deleteCurrent(false); }, true);
        imageAction(fileMenu, "완전히 삭제…", QKeySequence("Shift+Delete"), [this] { deleteCurrent(true); }, true)->setObjectName("deleteFileAction");
        fileMenu->addSeparator();
        imageAction(fileMenu, "인쇄…", QKeySequence("Ctrl+P"), [this] { printImage(); });
#ifdef Q_OS_WIN
        imageAction(fileMenu, "바탕화면 배경으로 지정", QKeySequence("Ctrl+Alt+W"), [this] { setWallpaper(); }, true);
#elif defined(Q_OS_MACOS)
        imageAction(fileMenu, "데스크탑 배경으로 지정", QKeySequence("Ctrl+Alt+W"), [this] { setWallpaper(); }, true);
#endif
        fileMenu->addSeparator();
        action(fileMenu, "창 닫기", QKeySequence("Ctrl+Shift+W"), [this] { q->close(); });
        QKeySequence quitKey(QKeySequence::Quit);
        if (quitKey.isEmpty()) quitKey = QKeySequence("Ctrl+Q");
        auto *quit = action(fileMenu, "ikview 종료", quitKey, [this] { q->close(); }); quit->setObjectName("quitApplicationAction"); quit->setMenuRole(QAction::QuitRole);
        quit->setShortcuts({quitKey, QKeySequence("Ctrl+W")});

        previousAction = action(viewMenu, "이전 이미지", QKeySequence(Qt::Key_Left), [this] { navigateFromKey(-1); });
        previousAction->setObjectName("previousImageAction"); useSymbol(previousAction, "left");
        nextAction = action(viewMenu, "다음 이미지", QKeySequence(Qt::Key_Right), [this] { navigateFromKey(1); });
        nextAction->setObjectName("nextImageAction"); useSymbol(nextAction, "right");
        firstAction = action(viewMenu, "처음 이미지", QKeySequence(Qt::Key_Home), [this] { if (doc->currentInfo().pdf) { stopSlideshow(); cancelPendingPosition(); doc->setPage(0); } else selectIndex(0); });
        lastAction = action(viewMenu, "마지막 이미지", QKeySequence(Qt::Key_End), [this] { if (doc->currentInfo().pdf) { stopSlideshow(); cancelPendingPosition(); doc->setPage(doc->currentInfo().pageCount - 1); } else selectIndex(doc->count() - 1); });
        goToAction = action(viewMenu, "이미지 번호로 이동…", QKeySequence("Ctrl+J"), [this] { goToImage(); });
        goToAction->setObjectName("goToImageAction");
        action(viewMenu, "이전 폴더 / 압축파일", QKeySequence("["), [this] { if (!operationPending) { cancelPendingPosition(); doc->adjacentContainer(-1); } });
        action(viewMenu, "다음 폴더 / 압축파일", QKeySequence("]"), [this] { if (!operationPending) { cancelPendingPosition(); doc->adjacentContainer(1); } });
        action(viewMenu, "무작위 이미지", QKeySequence("Ctrl+Alt+PgDown"), [this] { if (doc->count()) selectIndex(QRandomGenerator::global()->bounded(doc->count())); });
        viewMenu->addSeparator();
        fitAction = action(viewMenu, "화면에 맞춤", QKeySequence("1"), [this] { setFitMode(FitMode::Fit); });
        fitAction->setObjectName("fitImageAction"); useSymbol(fitAction, "fit");
        actualAction = action(viewMenu, "원본 크기 (100%)", QKeySequence("0"), [this] { setFitMode(FitMode::Actual); });
        actualAction->setObjectName("actualSizeAction");
        widthAction = action(viewMenu, "폭에 맞춤", QKeySequence("8"), [this] { setFitMode(FitMode::Width); });
        shrinkAction = action(viewMenu, "큰 이미지만 축소", QKeySequence("Z"), [this] { setFitMode(FitMode::Shrink); });
        fitGroup = new QActionGroup(q); fitGroup->setExclusionPolicy(QActionGroup::ExclusionPolicy::ExclusiveOptional);
        for (auto *a : {fitAction, widthAction, actualAction, shrinkAction}) { a->setCheckable(true); fitGroup->addAction(a); }
        spreadAction = action(viewMenu, "두 장 보기", QKeySequence("7"), [this] { setSpread(spreadAction->isChecked(), rtl); });
        spreadAction->setCheckable(true);
        rtlAction = action(viewMenu, "두 장 보기: 오른쪽부터", QKeySequence("6"), [this] { setSpread(true, !rtl); });
        rtlAction->setCheckable(true);
        zoomLockAction = action(viewMenu, "확대 배율 유지", QKeySequence("Ctrl+K"), [this] { canvas->setZoomLocked(zoomLockAction->isChecked()); });
        zoomLockAction->setObjectName("zoomLockAction"); zoomLockAction->setCheckable(true);
        viewMenu->addSeparator();
        slideshowAction = action(viewMenu, "슬라이드 쇼 시작", QKeySequence("Ctrl+1"), [this] { toggleSlideshow(); });
        slideshowAction->setObjectName("slideshowAction"); slideshowAction->setCheckable(true); useSymbol(slideshowAction, "play");
        action(viewMenu, "슬라이드 쇼 중지", QKeySequence("Ctrl+0"), [this] { stopSlideshow(); });
        auto *slideOptions = viewMenu->addMenu("슬라이드 쇼 설정"); slideOptions->setObjectName("slideshowOptionsMenu");
        loopSlideshowAction = action(slideOptions, "마지막에서 처음부터 반복", {}, [this] { settings.setValue("slideshowLoop", loopSlideshowAction->isChecked()); });
        loopSlideshowAction->setObjectName("slideshowLoopAction"); loopSlideshowAction->setCheckable(true); loopSlideshowAction->setChecked(settings.value("slideshowLoop", false).toBool());
        shuffleSlideshowAction = action(slideOptions, "무작위 순서", {}, [this] { settings.setValue("slideshowShuffle", shuffleSlideshowAction->isChecked()); });
        shuffleSlideshowAction->setObjectName("slideshowShuffleAction"); shuffleSlideshowAction->setCheckable(true); shuffleSlideshowAction->setChecked(settings.value("slideshowShuffle", false).toBool());
        slideshowMenu = new QMenu("슬라이드쇼 간격", q); slideshowMenu->setObjectName("slideshowIntervalMenu");
        auto *intervalGroup = new QActionGroup(q);
        for (const int interval : {1, 3, 5, 10, 15, 30, 60}) {
            auto *choice = action(slideshowMenu, QString("%1초 간격으로 시작").arg(interval), {}, [this, interval] {
                startSlideshow(interval);
            }); choice->setObjectName(QString("slideshowInterval%1Action").arg(interval)); choice->setCheckable(true); choice->setChecked(slideshowSeconds == interval); intervalGroup->addAction(choice);
        }
        slideshowMenu->addSeparator();
        auto *stopSlide = action(slideshowMenu, "슬라이드쇼 중지", {}, [this] { stopSlideshow(); }); stopSlide->setObjectName("stopSlideshowFromButtonAction");
        slideshowMenu->addSeparator(); slideshowMenu->addAction(loopSlideshowAction); slideshowMenu->addAction(shuffleSlideshowAction);
        QObject::connect(slideshowMenu, &QMenu::aboutToShow, q, [this, stopSlide] { stopSlide->setEnabled(slideshowAction->isChecked()); });

        fullScreenAction = action(viewMenu, "전체화면", QKeySequence("F11"), [this] { toggleFullscreen(); });
        fullScreenAction->setShortcuts({QKeySequence("F11"), QKeySequence("Ctrl+Meta+F")});
        fullScreenAction->setObjectName("fullScreenAction"); useSymbol(fullScreenAction, "fullscreen"); fullScreenAction->setCheckable(true);
        auto *alwaysOnTop = action(viewMenu, "항상 위에 표시", QKeySequence("Ctrl+Alt+A"), [this] {
            const bool on = !(q->windowFlags() & Qt::WindowStaysOnTopHint);
            q->setWindowFlag(Qt::WindowStaysOnTopHint, on); q->show();
        });
        alwaysOnTop->setCheckable(true);
        viewMenu->addSeparator();
        listAction = action(viewMenu, "이미지 목록 표시", QKeySequence("Ctrl+Shift+L"), [this] { revealChrome(); applyChrome(); if (!filePanel->isVisible()) canvas->setFocus(); });
        listAction->setObjectName("toggleFileListAction"); listAction->setCheckable(true); useSymbol(listAction, "sidebar");
        infoAction = action(viewMenu, "이미지 정보 표시", QKeySequence("Ctrl+I"), [this] { revealChrome(); applyChrome(); });
        infoAction->setObjectName("toggleMetadataAction"); infoAction->setCheckable(true); useSymbol(infoAction, "info");
        toolbarAction = action(viewMenu, "상단 도구 모음 표시", QKeySequence("Ctrl+Shift+T"), [this] { revealChrome(); applyChrome(); });
        toolbarAction->setObjectName("toggleToolbarAction"); toolbarAction->setCheckable(true);
        statusAction = action(viewMenu, "하단 정보 표시", QKeySequence("Ctrl+Shift+B"), [this] { revealChrome(); applyChrome(); });
        statusAction->setObjectName("toggleStatusAction"); statusAction->setCheckable(true);
        cleanAction = action(viewMenu, "집중 보기", QKeySequence(Qt::Key_Tab), [this] { setCleanView(cleanAction->isChecked()); });
        cleanAction->setObjectName("cleanViewAction"); cleanAction->setCheckable(true);
        q->removeAction(cleanAction); canvas->addAction(cleanAction); cleanAction->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        restoreAction = action(viewMenu, "기본 화면으로 복원", QKeySequence("Ctrl+Shift+R"), [this] { restoreView(); });
        restoreAction->setObjectName("restoreViewAction");
        thumbnailAction = action(viewMenu, "목록에 섬네일 표시", {}, [this] { fileModel->setThumbnailsEnabled(thumbnailAction->isChecked()); files->doItemsLayout(); settings.setValue("thumbnails", thumbnailAction->isChecked()); });
        thumbnailAction->setObjectName("toggleThumbnailsAction"); thumbnailAction->setCheckable(true); thumbnailAction->setChecked(settings.value("thumbnails", true).toBool());
        auto *sortMenu = viewMenu->addMenu("파일 정렬");
        auto *sortGroup = new QActionGroup(q);
        const QStringList sortLabels { "이름순", "이름 역순", "수정 시간순", "파일 크기순" };
        const int savedSort = std::clamp(settings.value("sort", 0).toInt(), 0, 3);
        for (int i = 0; i < sortLabels.size(); ++i) {
            auto *a = action(sortMenu, sortLabels[i], {}, [this, i] { settings.setValue("sort", i); doc->setSortOrder(static_cast<SortOrder>(i)); });
            a->setObjectName(QString("sortAction%1").arg(i)); a->setCheckable(true); sortGroup->addAction(a); a->setChecked(i == savedSort);
        }
        doc->setSortOrder(static_cast<SortOrder>(savedSort));

        auto *rotateRight = imageAction(imageMenu, "오른쪽으로 회전", QKeySequence("Ctrl+R"), [this] { canvas->rotate(1); });
        useSymbol(rotateRight, "rotate");
        imageAction(imageMenu, "왼쪽으로 회전", QKeySequence("Ctrl+L"), [this] { canvas->rotate(-1); });
        imageAction(imageMenu, "좌우 반전", QKeySequence("Ctrl+Alt+M"), [this] { canvas->flipHorizontal(); });
        imageAction(imageMenu, "상하 반전", QKeySequence("Ctrl+Alt+F"), [this] { canvas->flipVertical(); });
        imageAction(imageMenu, "보기 변환 초기화", QKeySequence("Ctrl+Alt+0"), [this] { canvas->resetTransform(); invertAction->setChecked(false); });
        imageMenu->addSeparator();
        zoomInAction = action(imageMenu, "확대", QKeySequence("+"), [this] { manualZoom(1.25); });
        zoomOutAction = action(imageMenu, "축소", QKeySequence("-"), [this] { manualZoom(0.8); });
        auto *panMenu = imageMenu->addMenu("확대한 이미지 영역 이동");
        const QList<QPair<QString, QPoint>> pans {{"왼쪽", {-1, 0}}, {"오른쪽", {1, 0}}, {"위쪽", {0, -1}}, {"아래쪽", {0, 1}}};
        const QStringList panKeys {"Alt+Left", "Alt+Right", "Alt+Up", "Alt+Down"};
        for (int i = 0; i < pans.size(); ++i) {
            auto *pan = action(panMenu, pans[i].first + "으로 이동", QKeySequence(panKeys[i]), [this, direction = pans[i].second] { canvas->panViewportPage(direction.x(), direction.y()); });
            pan->setObjectName(QString("panImage%1Action").arg(i));
            panActions.append(pan);
        }
        imageAction(imageMenu, "감마 보정…", QKeySequence("Ctrl+G"), [this] {
            bool accepted = false;
            const double value = decimalPrompt(q, "감마 보정", "감마 (1.0 = 원본)", 1.0, 0.1, 5.0, 2, &accepted);
            if (accepted) canvas->setGamma(value);
        });
        invertAction = imageAction(imageMenu, "색 반전", QKeySequence("Ctrl+Alt+I"), [this] { canvas->setInverted(invertAction->isChecked()); });
        invertAction->setCheckable(true);
        auto *filterMenu = imageMenu->addMenu("이미지 필터"); filterMenu->setObjectName("imageFilterMenu"); displayFilterMenu = filterMenu;
        auto *filterGroup = new QActionGroup(q); filterGroup->setExclusive(true);
        const QStringList filterIds {"none", "soften", "sharpen", "soften-sharpen"};
        const QStringList filterLabels {"필터 없음", "부드럽게 보기", "선명하게 보기", "부드럽게+선명하게 보기"};
        const QStringList filterKeys {"U", "I", "Shift+S", "S"};
        const QStringList filterObjects {"imageFilterNoneAction", "imageFilterSoftenAction", "imageFilterSharpenAction", "imageFilterSoftenSharpenAction"};
        for (int i=0; i<filterIds.size(); ++i) {
            const auto id = filterIds[i];
            auto *choice = action(filterMenu, filterLabels[i], QKeySequence(filterKeys[i]), [this, id] {
                settings.setValue("imageFilter", id); applyImageFilter(); settings.sync();
                q->statusBar()->showMessage("이미지 필터: " + imageFilterActions.value(id)->text(), 3000);
            });
            choice->setObjectName(filterObjects[i]); choice->setCheckable(true); filterGroup->addAction(choice); imageFilterActions.insert(id, choice);
        }
        animationAction = action(imageMenu, "애니메이션 일시정지", QKeySequence("P"), [this] { doc->setAnimationPaused(animationAction->isChecked()); });
        animationAction->setObjectName("pauseAnimationAction"); animationAction->setCheckable(true);
        nextPageAction = action(imageMenu, "다중 이미지: 다음 페이지", QKeySequence("Alt+PgDown"), [this] { stopSlideshow(); cancelPendingPosition(); doc->setPage(doc->currentInfo().page + 1); });
        previousPageAction = action(imageMenu, "다중 이미지: 이전 페이지", QKeySequence("Alt+PgUp"), [this] { stopSlideshow(); cancelPendingPosition(); doc->setPage(doc->currentInfo().page - 1); });
        nextPageAction->setObjectName("nextDocumentPageAction"); previousPageAction->setObjectName("previousDocumentPageAction");

        imageAction(toolsMenu, "이미지 변환 / 저장…", QKeySequence("Ctrl+T"), [this] {
            if (selectedEntries().size() > 1) showSelectionMenu(); else convertImage();
        })->setObjectName("exportImageAction");
        imageAction(toolsMenu, "흰 배경 제거하여 저장…", {}, [this] {
            convertImage({doc->currentEntry()}, ExportPurpose::RemoveWhiteBackground);
        })->setObjectName("removeWhiteBackgroundAction");
        toolsMenu->addSeparator();
        imageAction(toolsMenu, "이미지 복사", QKeySequence("Ctrl+C"), [this] { QApplication::clipboard()->setImage(canvas->transformedImage()); });
        imageAction(toolsMenu, "현재 화면 복사", QKeySequence("Ctrl+Alt+C"), [this] { QApplication::clipboard()->setImage(canvas->viewportImage()); });
        imageAction(toolsMenu, "파일 경로 복사", QKeySequence("Ctrl+Alt+Shift+C"), [this] { QApplication::clipboard()->setText(doc->currentEntry().displayPath()); });
        imageAction(toolsMenu, "EXIF 정보 복사", QKeySequence("Ctrl+Shift+C"), [this] { QApplication::clipboard()->setText(doc->currentInfo().metadata.join('\n')); });
        gpsAction = action(toolsMenu, "촬영 위치 지도에서 보기", QKeySequence("Ctrl+Shift+G"), [this] {
            const auto location = gpsLocation();
            if (location.x() != 999) QDesktopServices::openUrl(QUrl(QString("https://www.openstreetmap.org/?mlat=%1&mlon=%2#map=15/%1/%2").arg(location.x(), 0, 'f', 7).arg(location.y(), 0, 'f', 7)));
        }); gpsAction->setObjectName("openGpsMapAction");
        toolsMenu->addSeparator();
        auto *preferencesAction = action(toolsMenu, "환경설정…", QKeySequence("Ctrl+,"), [this] { preferences(); });
        preferencesAction->setObjectName("preferencesAction"); preferencesAction->setMenuRole(QAction::PreferencesRole);
        refreshAction = action(viewMenu, "현재 목록 새로 고침", QKeySequence("F5"), [this] { if (!operationPending) { stopSlideshow(); cancelPendingPosition(); doc->refreshCollection(); } });
        refreshAction->setObjectName("refreshCollectionAction");
        imageAction(bookmarkMenu, "현재 위치에 책갈피 추가", QKeySequence("B"), [this] { addBookmark(); })->setObjectName("addBookmarkAction");
        auto *manageBookmarksAction = action(bookmarkMenu, "책갈피 목록…", QKeySequence("F3"), [this] { bookmarksDialog(); }); manageBookmarksAction->setObjectName("manageBookmarksAction");
        auto *latestBookmarkAction = action(bookmarkMenu, "최근 책갈피 열기", QKeySequence("Ctrl+B"), [this] {
            const auto bookmarks = settings.value("bookmarks").toList();
            if (!bookmarks.isEmpty()) openBookmark(bookmarks.last().toMap());
        }); latestBookmarkAction->setObjectName("latestBookmarkAction");
        auto *aboutAction = action(helpMenu, "ikview 정보 / 지원 형식", {}, [this] { about(); }); aboutAction->setObjectName("aboutAction"); aboutAction->setMenuRole(QAction::AboutRole);
        auto *helpAction = action(helpMenu, "키보드 사용법", QKeySequence("F1"), [this] { showKeyboardHelp(); }); helpAction->setObjectName("keyboardHelpAction");
#ifdef Q_OS_MACOS
        auto *windowMenu = q->menuBar()->addMenu("윈도우");
        action(windowMenu, "최소화", QKeySequence("Ctrl+M"), [this] { q->showMinimized(); })->setObjectName("minimizeWindowAction");
#endif


        toolbar = q->addToolBar("상단 도구 모음");
        toolbar->setObjectName("mainToolbar");
        toolbar->setMovable(false); toolbar->setIconSize(QSize(24, 24)); toolbar->setToolButtonStyle(Qt::ToolButtonIconOnly);
        auto *brand = new QLabel("ikview"); brand->setObjectName("appBrand");
        brand->setContentsMargins(2, 0, 18, 0); toolbar->addWidget(brand);
        const auto addToolbarAction = [this](QAction *command, const QString &name = QString(), Qt::ToolButtonStyle presentation = Qt::ToolButtonIconOnly) {
            auto *proxy = new ViewerToolbarAction(command, name, presentation, toolbar);
            toolbar->addAction(proxy); return qobject_cast<QToolButton *>(toolbar->widgetForAction(proxy));
        };
        auto *listToggle = addToolbarAction(listAction, "fileListToggleButton");
        listToggle->setAccessibleName("이미지 목록 표시 / 숨기기"); toolbar->addSeparator();
        addToolbarAction(fitAction);
        useSymbol(actualAction, "1:1"); addToolbarAction(actualAction);
        useSymbol(zoomOutAction, "−"); useSymbol(zoomInAction, "+");
        addToolbarAction(zoomOutAction); addToolbarAction(zoomInAction); addToolbarAction(rotateRight);
        toolbar->addSeparator();
        slideshowMenuAction = new QAction("슬라이드쇼", q); slideshowMenuAction->setObjectName("slideshowMenuButtonAction"); slideshowMenuAction->setMenu(slideshowMenu);
        slideshowMenuAction->setToolTip("간격을 선택하면 슬라이드쇼를 시작합니다."); useSymbol(slideshowMenuAction, "play");
        slideshowButton = addToolbarAction(slideshowMenuAction, "slideshowButton", Qt::ToolButtonTextBesideIcon);
        slideshowButton->setAccessibleName("슬라이드쇼 간격 선택");
        auto *spacer = new QWidget; spacer->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred); toolbar->addWidget(spacer);
        useSymbol(manageBookmarksAction, "bookmark"); manageBookmarksAction->setIconText("책갈피");
        bookmarksButton = addToolbarAction(manageBookmarksAction, "bookmarksButton", Qt::ToolButtonTextBesideIcon);
        bookmarksButton->setAccessibleName("책갈피 목록");
        addToolbarAction(infoAction); addToolbarAction(fullScreenAction);

        splitter = new QSplitter(Qt::Horizontal, q);
        splitter->setHandleWidth(1);
        filePanel = new QWidget(splitter); filePanel->setObjectName("filePanel"); filePanel->setMinimumWidth(220); filePanel->setMaximumWidth(520);
        auto *fileLayout = new QVBoxLayout(filePanel); fileLayout->setContentsMargins(0, 0, 0, 0); fileLayout->setSpacing(0);
        auto *listHeader = new QWidget; listHeader->setObjectName("filePanelHeader"); auto *headerLayout = new QHBoxLayout(listHeader); headerLayout->setContentsMargins(16, 12, 12, 12); headerLayout->setSpacing(8);
        auto *listTitle = new QLabel("이미지 목록"); listTitle->setObjectName("listTitle");
        fileCount = new QLabel("0"); fileCount->setAlignment(Qt::AlignRight | Qt::AlignVCenter); fileCount->setObjectName("fileCount");
        headerLayout->addWidget(listTitle, 1); headerLayout->addWidget(fileCount);
        auto *hideList = new ViewerToolButton; hideList->setObjectName("hideFileListButton"); hideList->setCompact(true); hideList->setAccessibleName("이미지 목록 숨기기"); useSymbol(hideList, "hide"); hideList->setToolTip("이미지 목록 숨기기 (" + QKeySequence("Ctrl+Shift+L").toString(QKeySequence::NativeText) + ")"); hideList->setAutoRaise(true);
        QObject::connect(hideList, &QToolButton::clicked, q, [this] { listAction->setChecked(false); applyChrome(); canvas->setFocus(); });
        headerLayout->addWidget(hideList); fileLayout->addWidget(listHeader);
        filter = new QLineEdit; filter->setObjectName("fileFilter"); filter->setPlaceholderText("파일 이름으로 찾기"); filter->setClearButtonEnabled(true);
        auto *filterBox = new QWidget; auto *filterLayout = new QVBoxLayout(filterBox); filterLayout->setContentsMargins(12, 0, 12, 12); filterLayout->addWidget(filter); fileLayout->addWidget(filterBox);
        files = new QListView; files->setObjectName("fileList"); files->setSelectionMode(QAbstractItemView::ExtendedSelection); files->setSelectionBehavior(QAbstractItemView::SelectRows); files->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel); files->setUniformItemSizes(true); files->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); files->setTextElideMode(Qt::ElideMiddle); files->setIconSize(QSize(72, 54)); files->setSpacing(0); files->setEditTriggers(QAbstractItemView::NoEditTriggers);
        files->setContextMenuPolicy(Qt::CustomContextMenu);
        QObject::connect(files, &QListView::customContextMenuRequested, q, [this](const QPoint &point) { showSelectionMenu(files->viewport()->mapToGlobal(point)); });
        fileModel = new ImageListModel(doc, q); fileProxy = new QSortFilterProxyModel(q);
        fileProxy->setSourceModel(fileModel); fileProxy->setFilterRole(Qt::EditRole); fileProxy->setFilterCaseSensitivity(Qt::CaseInsensitive);
        files->setModel(fileProxy); fileModel->setThumbnailsEnabled(thumbnailAction->isChecked());
        files->setDragEnabled(true); files->setDragDropMode(QAbstractItemView::DragOnly); files->setDefaultDropAction(Qt::CopyAction);
        fileLayout->addWidget(files, 1);
        emptyListLabel = new QLabel("이미지를 열면 목록이 표시됩니다."); emptyListLabel->setObjectName("emptyFileListLabel"); emptyListLabel->setWordWrap(true); emptyListLabel->setAlignment(Qt::AlignCenter); emptyListLabel->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Expanding);  fileLayout->addWidget(emptyListLabel);
        listHint = new QLabel; listHint->setObjectName("listHint"); listHint->setWordWrap(true); fileLayout->addWidget(listHint);

        auto *center = new QWidget(splitter); auto *centerLayout = new QVBoxLayout(center); centerLayout->setContentsMargins(0, 0, 0, 0); centerLayout->setSpacing(0);
        sourceHint = new QWidget; sourceHint->setObjectName("sourceScopeHint"); auto *scopeLayout = new QHBoxLayout(sourceHint); scopeLayout->setContentsMargins(12, 7, 12, 7);
        sourceHintText = new QLabel("같은 폴더의 이미지도 보려면 폴더를 선택하세요."); sourceHintText->setWordWrap(true); scopeLayout->addWidget(sourceHintText, 1);
        openSourceFolderButton = new QPushButton("같은 폴더 열기…"); openSourceFolderButton->setObjectName("openSourceFolderButton"); scopeLayout->addWidget(openSourceFolderButton);
        QObject::connect(openSourceFolderButton, &QPushButton::clicked, q, [this] {
            if (operationPending || !doc->count()) return;
            const auto restore = currentPosition();
            chooseFolder(QFileInfo(restore.source).absolutePath(), restore);
        });
        auto *dismissSourceHint = new ViewerToolButton; dismissSourceHint->setObjectName("dismissSourceHintButton"); dismissSourceHint->setCompact(true); useSymbol(dismissSourceHint, "hide"); dismissSourceHint->setAccessibleName("폴더 안내 숨기기"); dismissSourceHint->setToolTip("이번 창에서 폴더 안내 숨기기"); scopeLayout->addWidget(dismissSourceHint);
        QObject::connect(dismissSourceHint, &QToolButton::clicked, q, [this] { sourceHintDismissed = true; updateSourceHint(); });
        centerLayout->addWidget(sourceHint); sourceHint->hide();
        operationPanel = new QWidget; operationPanel->setObjectName("fileOperationPanel"); auto *operationLayout = new QHBoxLayout(operationPanel); operationLayout->setContentsMargins(12, 6, 12, 6);
        operationLabel = new QLabel; operationLabel->setObjectName("fileOperationLabel"); operationLabel->setWordWrap(true); operationLabel->setTextFormat(Qt::PlainText); operationLayout->addWidget(operationLabel, 1);
        cancelOperationButton = new QPushButton("작업 취소"); cancelOperationButton->setObjectName("cancelFileOperationButton"); operationLayout->addWidget(cancelOperationButton);
        QObject::connect(cancelOperationButton, &QPushButton::clicked, q, [this] { cancelFileOperation(); }); centerLayout->addWidget(operationPanel); operationPanel->hide();
        operationTimer = new QTimer(q); operationTimer->setInterval(100); QObject::connect(operationTimer, &QTimer::timeout, q, [this] { updateOperationProgress(); });
        progress = new QProgressBar; progress->setObjectName("fileLoadingProgress"); progress->setRange(0, 0); progress->setTextVisible(false); progress->hide(); centerLayout->addWidget(progress);
        centerLayout->addWidget(canvas, 1);
        footer = new QWidget; footer->setObjectName("imageFooter");
        auto *bottomLayout = new QHBoxLayout(footer); bottomLayout->setContentsMargins(16, 6, 12, 6); bottomLayout->setSpacing(12);
        filename = new QLabel("이미지·PDF·폴더·압축파일을 끌어놓으세요"); filename->setMinimumWidth(80); filename->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred); filename->setTextFormat(Qt::PlainText); filename->setTextInteractionFlags(Qt::TextSelectableByMouse); filename->setObjectName("imageFilename");
        position = new QLabel("이미지 없음"); position->setObjectName("imagePosition");
        positionButton = new QPushButton("번호로 이동…"); positionButton->setFlat(true); positionButton->setToolTip("이미지 번호로 이동 (" + QKeySequence("Ctrl+J").toString(QKeySequence::NativeText) + ")"); positionButton->setObjectName("goToImageButton");
        QObject::connect(positionButton, &QPushButton::clicked, q, [this] { goToImage(); });
        bottomLayout->addWidget(filename, 1); bottomLayout->addWidget(position); bottomLayout->addWidget(positionButton);
        pdfControls = new QWidget; pdfControls->setObjectName("pdfPageControls"); auto *pdfLayout = new QHBoxLayout(pdfControls); pdfLayout->setContentsMargins(0, 0, 0, 0); pdfLayout->setSpacing(6);
        auto *previousPage = new ViewerToolButton; previousPage->setObjectName("previousPdfPageButton"); previousPage->setCompact(true); previousPage->setDefaultAction(previousPageAction); useSymbol(previousPageAction, "left");
        auto *nextPage = new ViewerToolButton; nextPage->setObjectName("nextPdfPageButton"); nextPage->setCompact(true); nextPage->setDefaultAction(nextPageAction); useSymbol(nextPageAction, "right");
        pdfPageNumber = new QSpinBox; pdfPageNumber->setObjectName("pdfPageNumber"); pdfPageNumber->setAccessibleName("PDF 페이지 번호"); pdfPageNumber->setToolTip("페이지 번호를 입력하고 Return 키로 이동"); pdfPageNumber->setMinimum(1); pdfPageNumber->setKeyboardTracking(false); pdfPageNumber->setMinimumWidth(84); pdfPageCount = new QLabel; pdfPageCount->setObjectName("pdfPageCount");
        auto *pageLabel = new QLabel("페이지"); pageLabel->setBuddy(pdfPageNumber);
        pdfLayout->addWidget(previousPage); pdfLayout->addWidget(pageLabel); pdfLayout->addWidget(pdfPageNumber); pdfLayout->addWidget(pdfPageCount); pdfLayout->addWidget(nextPage); bottomLayout->addWidget(pdfControls); pdfControls->hide();
        QObject::connect(pdfPageNumber, &QSpinBox::valueChanged, q, [this](int page) { if (ready() && page - 1 != doc->currentInfo().page) { stopSlideshow(); cancelPendingPosition(); doc->setPage(page - 1); } });
        centerLayout->addWidget(footer);
        errorBanner = new QWidget; errorBanner->setObjectName("errorBanner");
        auto *errorLayout = new QHBoxLayout(errorBanner); errorLayout->setContentsMargins(14, 7, 10, 7); errorText = new QLabel; errorText->setWordWrap(true); errorText->setTextFormat(Qt::PlainText); errorLayout->addWidget(errorText, 1);
        auto *dismissError = new ViewerToolButton; dismissError->setObjectName("dismissErrorButton"); dismissError->setCompact(true); dismissError->setAccessibleName("알림 닫기"); useSymbol(dismissError, "hide"); dismissError->setToolTip("알림 닫기"); errorLayout->addWidget(dismissError);
        QObject::connect(dismissError, &QToolButton::clicked, q, [this] { errorBanner->hide(); });
        centerLayout->insertWidget(0, errorBanner); errorBanner->hide();

        infoPanel = new QWidget(splitter); infoPanel->setObjectName("metadataPanel"); infoPanel->setMinimumWidth(230); infoPanel->setMaximumWidth(440);
        auto *infoLayout = new QVBoxLayout(infoPanel); infoLayout->setContentsMargins(0, 0, 0, 0);
        auto *infoHeader = new QWidget; infoHeader->setObjectName("metadataHeader");
        auto *infoHeaderLayout = new QHBoxLayout(infoHeader); infoHeaderLayout->setContentsMargins(16, 12, 12, 12); infoHeaderLayout->setSpacing(8);
        auto *infoTitle = new QLabel("이미지 정보"); infoTitle->setObjectName("infoTitle"); infoHeaderLayout->addWidget(infoTitle, 1);
        auto *hideInfo = new ViewerToolButton; hideInfo->setObjectName("hideMetadataButton"); hideInfo->setCompact(true); useSymbol(hideInfo, "hide"); hideInfo->setAccessibleName("이미지 정보 숨기기"); hideInfo->setToolTip("이미지 정보 숨기기");
        QObject::connect(hideInfo, &QToolButton::clicked, q, [this] { infoAction->setChecked(false); applyChrome(); canvas->setFocus(); });
        infoHeaderLayout->addWidget(hideInfo); infoLayout->addWidget(infoHeader);
        metadata = new QTreeWidget; metadata->setColumnCount(2); metadata->setHeaderLabels({ "항목", "값" }); metadata->setRootIsDecorated(false);
        metadata->setObjectName("imageMetadata"); metadata->setSelectionMode(QAbstractItemView::ExtendedSelection);
        metadata->setContextMenuPolicy(Qt::CustomContextMenu);
        copyMetadataValueAction = new QAction("값 복사", metadata); copyMetadataValueAction->setObjectName("copyMetadataValueAction"); copyMetadataValueAction->setShortcut(QKeySequence::Copy); copyMetadataValueAction->setShortcutContext(Qt::WidgetWithChildrenShortcut); metadata->addAction(copyMetadataValueAction);
        copyMetadataRowAction = new QAction("선택한 행 복사", metadata); copyMetadataRowAction->setObjectName("copyMetadataRowAction");
        copyMetadataAllAction = new QAction("전체 정보 복사", metadata); copyMetadataAllAction->setObjectName("copyMetadataAllAction");
        QObject::connect(copyMetadataValueAction, &QAction::triggered, q, [this] { copyMetadata(false, false); });
        QObject::connect(copyMetadataRowAction, &QAction::triggered, q, [this] { copyMetadata(true, false); });
        QObject::connect(copyMetadataAllAction, &QAction::triggered, q, [this] { copyMetadata(true, true); });
        QObject::connect(metadata, &QTreeWidget::customContextMenuRequested, q, [this](const QPoint &point) { QMenu menu(metadata); menu.addAction(copyMetadataValueAction); menu.addAction(copyMetadataRowAction); menu.addAction(copyMetadataAllAction); menu.exec(metadata->viewport()->mapToGlobal(point)); });
        QObject::connect(metadata, &QTreeWidget::itemSelectionChanged, q, [this] { updateMetadataActions(); });
        metadata->header()->setSectionResizeMode(0, QHeaderView::ResizeToContents); metadata->header()->setStretchLastSection(true); metadata->setAlternatingRowColors(true); infoLayout->addWidget(metadata, 1);
        splitter->addWidget(filePanel); splitter->addWidget(center); splitter->addWidget(infoPanel);
        splitter->setStretchFactor(0, 0); splitter->setStretchFactor(1, 1); splitter->setStretchFactor(2, 0); splitter->setSizes({ 240, 800, 280 });
        q->setCentralWidget(splitter);

        auto *status = new ViewerStatusBar; q->setStatusBar(status); details = status->detailsLabel(); zoomLabel = new QLabel("—");
        status->setObjectName("mainStatusBar"); status->addPermanentWidget(zoomLabel);

        shortcut("10장 앞으로 이동", QKeySequence("Ctrl+PgDown"), [this] { jumpImages(10); });
        shortcut("10장 뒤로 이동", QKeySequence("Ctrl+PgUp"), [this] { jumpImages(-10); });
        shortcut("100장 앞으로 이동", QKeySequence("Ctrl+Shift+Right"), [this] { jumpImages(100); });
        shortcut("100장 뒤로 이동", QKeySequence("Ctrl+Shift+Left"), [this] { jumpImages(-100); });
        shortcut("이미지 목록 검색", QKeySequence("Ctrl+F"), [this] { listAction->setChecked(true); revealChrome(); applyChrome(); filter->setFocus(); filter->selectAll(); })->setObjectName("searchImagesAction");
        shortcut("보기 상태에서 나가기", QKeySequence("Escape"), [this] { escapeView(); })->setObjectName("escapeViewAction");
        registerShortcuts();
#ifdef Q_OS_MACOS
        // Register with legacy categories first: saved shortcut IDs must stay
        // stable when Windows-style menu mnemonics are removed on macOS.
        for (auto *item : q->menuBar()->actions()) if (auto *menu = item->menu()) {
            auto title = menu->title(); title.remove(QRegularExpression("\\(&[A-Za-z]\\)$")); menu->setTitle(title);
        }
#endif
        canvas->setContextMenuPolicy(Qt::CustomContextMenu);
        QObject::connect(canvas, &QWidget::customContextMenuRequested, q, [this, openAction, rotateRight](const QPoint &point) {
            QMenu menu(q); menu.setObjectName("imageContextMenu"); menu.addAction(openAction); menu.addSeparator(); menu.addAction(fitAction); menu.addAction(actualAction); menu.addAction(spreadAction); menu.addAction(rotateRight); menu.addMenu(displayFilterMenu); menu.addSeparator(); menu.addAction(q->findChild<QAction *>("exportImageAction")); menu.addAction(q->findChild<QAction *>("removeWhiteBackgroundAction")); menu.addSeparator(); menu.addAction(listAction); menu.addAction(infoAction); menu.addAction(toolbarAction); menu.addAction(statusAction); menu.addAction(cleanAction); menu.addAction(fullScreenAction); menu.addSeparator(); menu.addAction(restoreAction); menu.exec(canvas->mapToGlobal(point));
        });
    }

    void connectSignals() {
        QObject::connect(doc, &Document::entriesChanged, q, [this] {
            const bool preserveScroll = pendingRecentPaths.isEmpty() && !currentKey.isEmpty();
            QSet<QString> selectedKeys;
            if (preserveScroll) for (const auto &index : files->selectionModel()->selectedRows()) selectedKeys.insert(index.data(Qt::UserRole + 1).toString());
            const int previousScroll = files->verticalScrollBar()->value(); const QString previousKey = currentKey, previousFilter = filter->text(); const quint64 serial = openSerial;
            syncingSelection = true;
            { QSignalBlocker blocker(files->selectionModel()); fileModel->setEntries(doc->entries()); }
            syncingSelection = false;
            applyFilter(); syncSelection(false);
            if (selectedKeys.size() > 1) {
                QScopedValueRollback guard(syncingSelection, true);
                QItemSelection selection;
                for (int row=0; row<fileProxy->rowCount(); ++row) { const auto index=fileProxy->index(row,0); if (selectedKeys.contains(index.data(Qt::UserRole+1).toString())) selection.select(index,index); }
                if (!selection.isEmpty()) files->selectionModel()->select(selection, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
            }
            if (preserveScroll) QTimer::singleShot(0, q, [this, previousScroll, previousKey, previousFilter, serial] {
                if (openSerial == serial && currentKey == previousKey && filter->text() == previousFilter) { files->doItemsLayout(); files->verticalScrollBar()->setValue(previousScroll); }
            });
            if (pendingRestore.active() && pendingRestore.serial == openSerial && doc->count() > 0) {
                int index = -1;
                if (pendingRestore.byIndex) {
                    index = std::clamp(pendingRestore.index, 0, doc->count() - 1);
                    const auto &entry = doc->entries()[index];
                    pendingRestore.source = entry.isArchive() ? entry.archivePath : entry.path;
                    pendingRestore.member = entry.member; pendingRestore.occurrence = entry.memberOccurrence; pendingRestore.byIndex = false;
                } else if (Document::isArchivePath(pendingRestore.source)) {
                    for (int i = 0; i < doc->count(); ++i) {
                        const auto &entry = doc->entries()[i];
                        if (entry.archivePath == pendingRestore.source && !pendingRestore.member.isEmpty() && entry.member == pendingRestore.member && entry.memberOccurrence == pendingRestore.occurrence) { index = i; break; }
                    }
                    if (index < 0 && pendingRestore.member.isEmpty() && pendingRestore.index >= 0) index = std::clamp(pendingRestore.index, 0, doc->count() - 1);
                } else for (int i = 0; i < doc->count(); ++i) if (doc->entries()[i].path == pendingRestore.source) { index = i; break; }
                if (index >= 0) {
                    pendingRestore.index = index;
                    const QString expectedKey = doc->entries()[index].key(); const quint64 serial = openSerial;
                    QTimer::singleShot(0, q, [this, index, expectedKey, serial] {
                        if (serial == openSerial && index < doc->count() && doc->entries()[index].key() == expectedKey) doc->goTo(index);
                    });
                } else { pendingRestore = {}; pendingBookmarkDeletion.clear(); q->statusBar()->showMessage("저장한 위치의 이미지가 없어 목록의 첫 이미지를 표시합니다.", 6000); }
            }
            updateSourceHint();
            updateEnabled();
        });
        QObject::connect(doc, &Document::currentChanged, q, [this](int index) {
            const Entry entry = doc->currentEntry();
            const bool changed = entry.key() != currentKey;
            warmImageTransition = index >= 0 && !doc->isBusy() && !doc->currentImage().isNull();
            if (!warmImageTransition) loadedKey.clear();
            if (slideshowAction->isChecked() && (changed || !warmImageTransition)) slideshow->stop();
            if (changed) {
                pendingImageReset = true;
                if (!warmImageTransition) canvas->setImage({});
                invertAction->setChecked(false); animationAction->setChecked(false); doc->setAnimationPaused(false);
                if (!warmImageTransition) { metadata->clear(); metadataKey.clear(); }
                currentKey = entry.key();
            }
            if (index < 0) {
                pendingImageReset = false; warmImageTransition = false;
                canvas->setImage({}); canvas->setSecondaryPending(false); canvas->setStatusMessage({}, {}); q->setProperty("documentState", "empty"); metadata->clear(); metadataKey.clear(); zoomLabel->setText("—");
                filename->setText("이미지·PDF·폴더·압축파일을 끌어놓으세요");
                position->setText("이미지 없음"); details->setText("이미지를 열어 시작하세요");
            } else {
                filename->setText(entry.name); position->setText(QString("%1 / %2").arg(index + 1).arg(doc->count()));
                if (!warmImageTransition) { canvas->setStatusMessage("이미지를 읽는 중…", entry.name); q->setProperty("documentState", "loading"); }
            }
            filename->setToolTip(entry.displayPath()); q->setWindowTitle(entry.name.isEmpty() ? "ikview" : entry.name + " — ikview");
            // A watcher refresh or sort can re-emit the same current entry.
            // Preserve a filter that intentionally excludes that image.
            syncSelection(changed);

            updateSourceHint(); if (!warmImageTransition) updateEnabled();
        });
        QObject::connect(doc, &Document::imageReady, q, [this](const QImage &image, const ImageInfo &info) {
            const QString key = doc->currentEntry().key();
            const bool firstFrame = loadedKey != key;
            const bool propertiesChanged = metadataKey != key || metadataInfo.size != info.size || metadataInfo.page != info.page
                || metadataInfo.pageCount != info.pageCount || metadataInfo.fileBytes != info.fileBytes || metadataInfo.animated != info.animated
                || metadataInfo.format != info.format || metadataInfo.metadata != info.metadata;
            loadedKey = key;
            if (firstFrame || propertiesChanged) { canvas->setStatusMessage({}, {}); q->setProperty("documentState", "ready"); }
            canvas->setImage(image, std::exchange(pendingImageReset, false));
            const bool deferredActions = std::exchange(warmImageTransition, false);
            // Animation advances pixel content while its document properties
            // stay constant. Keep native menus, metadata, layout and filesystem
            // state out of this frame-delivery path.
            finishBookmarkOpen(info);
            if (!firstFrame && !propertiesChanged) { if (deferredActions) updateEnabled(); return; }
            fileModel->refreshDecoded(doc->currentIndex(), image);
            if (spread) updateSecondaryImage();
            details->setText(QString("%1 × %2 px   ·   %3   ·   %4%5")
                .arg(info.size.width()).arg(info.size.height()).arg(info.format.toUpper()).arg(bytesText(info.fileBytes))
                .arg(!info.pdf && info.pageCount > 1 ? QString("   ·   페이지 %1/%2").arg(info.page + 1).arg(info.pageCount) : QString()));
            details->setToolTip(QString("디코딩 %1 ms · 캐시 %2").arg(info.decodeMs).arg(bytesText(cacheBytes)));
            if (propertiesChanged) {
                updateMetadata(info); metadataKey = loadedKey; metadataInfo = info;
            }
            updateEnabled(); updateFitActions();
            if (pendingRestore.active() && pendingRestore.serial == openSerial && restoreMatches(doc->currentEntry())) {
                const int page = pendingRestore.page; const QString key = doc->currentEntry().key(); const quint64 serial = openSerial;
                pendingRestore = {};
                if (page > 0 && page < info.pageCount && page != info.page) QTimer::singleShot(0, q, [this, page, key, serial] {
                    if (serial == openSerial && doc->currentEntry().key() == key) doc->setPage(page);
                });
            }
            if (!pendingRestore.active()) persistPosition();
            if (slideshowAction->isChecked()) slideshow->start(slideshowSeconds * 1000);
        });
        QObject::connect(doc, &Document::neighborReady, q, [this](int index, const QImage &image) {
            fileModel->refreshDecoded(index, image);
            if (spread && index == doc->currentIndex() + 1) canvas->setSecondaryImage(image);
        });
        QObject::connect(doc, &Document::neighborFailed, q, [this](int index, const QString &message) {
            if (spread && index == doc->currentIndex() + 1) {
                canvas->setSecondaryPending(false);
                q->statusBar()->showMessage("다음 이미지를 읽지 못했습니다. " + message, 8000);
            }
        });
        QObject::connect(doc, &Document::loadingChanged, q, [this](bool busy) {
            progress->setVisible(busy || operationPending); updateEnabled();
            if (busy) { details->setText("이미지를 읽는 중…"); if (doc->currentImage().isNull()) { canvas->setStatusMessage("이미지를 읽는 중…", "잠시 기다려 주세요."); q->setProperty("documentState", "loading"); } }
            else if (doc->currentImage().isNull() && doc->count()) details->setText("이미지를 표시하지 못했습니다.");
        });
        QObject::connect(doc, &Document::errorOccurred, q, [this](const QString &message) {
            stopSlideshow(); pendingRestore = {}; pendingBookmarkDeletion.clear(); pendingRecentPaths.clear(); errorText->setText(message); errorBanner->show();
            if (doc->currentImage().isNull()) { loadedKey.clear(); canvas->setSecondaryPending(false); canvas->setStatusMessage("이미지를 표시할 수 없습니다", "다른 이미지를 선택하거나 파일을 다시 열어 주세요.\n" + message); q->setProperty("documentState", "error"); details->setText("이미지를 표시하지 못했습니다. 다른 이미지를 선택하거나 다시 열어주세요."); } updateEnabled();
        });
        QObject::connect(doc, &Document::cacheUsageChanged, q, [this](qint64 bytes) { cacheBytes = bytes; });
        QObject::connect(canvas, &ImageCanvas::zoomChanged, q, [this](double zoom) {
            zoomLabel->setText(QString::number(zoom * 100, 'f', zoom < 0.1 ? 1 : 0) + "%"); updateFitActions(); if (!warmImageTransition) updatePanActions(ready());
        });
        QObject::connect(canvas, &ImageCanvas::preparationChanged, q, [this](bool preparing) {
            if (!preparing && !pendingBookmarkDeletion.isEmpty()) QTimer::singleShot(0, q, [this] { finishBookmarkOpen(doc->currentInfo()); });
        });
        QObject::connect(canvas, &ImageCanvas::navigateRequested, q, [this](int direction) { navigate(direction); });
        QObject::connect(canvas, &ImageCanvas::filesDropped, q, [this](const QStringList &paths) { open(paths); });
        QObject::connect(canvas, &ImageCanvas::fullScreenRequested, q, [this] { toggleFullscreen(); });
        QObject::connect(files->selectionModel(), &QItemSelectionModel::currentChanged, q, [this](const QModelIndex &index, const QModelIndex &) {
            if (!syncingSelection && index.isValid() && !operationPending) {
                QScopedValueRollback guard(syncingSelection, true);
                stopSlideshow(); cancelPendingPosition(); doc->goTo(fileProxy->mapToSource(index).row());
            }
        });
        QObject::connect(files->selectionModel(), &QItemSelectionModel::selectionChanged, q, [this] {
            const auto count = files->selectionModel()->selectedRows().size(); q->setProperty("selectedImageCount", count);
            if (count > 1 && !syncingSelection) {
                const auto keys = ShortcutManager::displaySequences(q->findChild<QAction *>("exportImageAction")->shortcuts());
                q->statusBar()->showMessage(QString("%1개 선택 · ").arg(count) + (keys.isEmpty() ? QStringLiteral("우클릭으로 선택 메뉴") : QString("우클릭 또는 %1로 선택 메뉴").arg(keys)), 5000);
            }
            if (!syncingSelection) QTimer::singleShot(0, q, [this] {
                const auto selected = files->selectionModel()->selectedRows();
                if (!operationPending && !selected.isEmpty() && !files->selectionModel()->isSelected(files->currentIndex()))
                    files->selectionModel()->setCurrentIndex(selected.first(), QItemSelectionModel::NoUpdate);
            });
        });
        QObject::connect(filter, &QLineEdit::textChanged, q, [this] { applyFilter(); });
        QObject::connect(slideshow, &QTimer::timeout, q, [this] {
            if (!slideshowAction->isChecked()) return;
            if (doc->isBusy() || operationPending || QApplication::activeModalWidget() || QApplication::activePopupWidget()) { slideshow->start(slideshowSeconds * 1000); return; }
            const auto info = doc->currentInfo();
            if (info.pdf && info.page + 1 < info.pageCount) { doc->setPage(info.page + 1); return; }
            if (info.pdf && doc->count() == 1) { if (loopSlideshowAction->isChecked()) doc->setPage(0); else stopSlideshow(); return; }
            int next = doc->currentIndex() + (spread && !info.pdf ? 2 : 1);
            if (shuffleSlideshowAction->isChecked() && doc->count() > 1) next = (doc->currentIndex() + 1 + QRandomGenerator::global()->bounded(doc->count() - 1)) % doc->count();
            else if (next >= doc->count() && loopSlideshowAction->isChecked()) next = 0;
            if (next >= doc->count()) stopSlideshow(); else doc->goTo(next);
        });
    }

    bool ready() const { return !operationPending && !doc->isBusy() && !loadedKey.isEmpty() && loadedKey == doc->currentEntry().key() && !doc->currentImage().isNull(); }
    void updateEnabled() {
        const bool enabled = ready(), any = doc->count() > 0 && !operationPending;
        const auto entry = doc->currentEntry(); const auto info = doc->currentInfo();
        for (auto *a : needsImage) a->setEnabled(enabled);
        for (auto *a : needsFile) a->setEnabled(enabled && !entry.isArchive());
        gpsAction->setEnabled(enabled && gpsLocation().x() != 999);
        previousAction->setEnabled(any && (doc->currentIndex() > 0 || (info.pdf && info.page > 0)));
        nextAction->setEnabled(any && (doc->currentIndex() + (spread && !info.pdf ? 2 : 1) < doc->count() || (info.pdf && info.page + 1 < info.pageCount)));
        firstAction->setEnabled(any && (info.pdf ? info.page > 0 : doc->currentIndex() > 0)); lastAction->setEnabled(any && (info.pdf ? info.page + 1 < info.pageCount : doc->currentIndex() + 1 < doc->count()));
        goToAction->setEnabled(any); positionButton->setEnabled(any);
        zoomInAction->setEnabled(enabled); zoomOutAction->setEnabled(enabled);
        nextPageAction->setEnabled(enabled && info.page + 1 < info.pageCount);
        previousPageAction->setEnabled(enabled && info.page > 0);
        animationAction->setEnabled(enabled && info.animated);
        const bool canSlideshow = any && (doc->count() > 1 || (info.pdf && info.pageCount > 1));
        slideshowAction->setEnabled(canSlideshow); slideshowMenuAction->setEnabled(canSlideshow);
        for (auto *choice : slideshowMenu->actions()) if (choice->objectName().startsWith("slideshowInterval")) choice->setEnabled(canSlideshow);
        pdfControls->setVisible(info.pdf && doc->count() > 0); pdfPageNumber->setEnabled(enabled);
        { QSignalBlocker blocker(pdfPageNumber); pdfPageNumber->setRange(1, std::max(1, info.pageCount)); pdfPageNumber->setValue(info.page + 1); }
        pdfPageCount->setText(QString("/ %1").arg(info.pageCount)); pdfPageCount->setToolTip(QString("총 %1페이지").arg(info.pageCount));
        positionButton->setVisible(!info.pdf); position->setVisible(!info.pdf || doc->count() > 1);
        if (info.pdf && doc->count() > 1) position->setText(QString("파일 %1 / %2").arg(doc->currentIndex() + 1).arg(doc->count()));
        updatePanActions(enabled);
        if (refreshAction) refreshAction->setEnabled(!operationPending && !doc->isBusy() && !doc->collectionPaths().isEmpty());
        if (openSourceFolderButton) openSourceFolderButton->setEnabled(any);
        updateMetadataActions(); updateSourceHint();
    }
    void updatePanActions(bool enabled) { const bool canPan = enabled && canvas->canPan(); for (auto *pan : panActions) pan->setEnabled(canPan); }
    void updateMetadataActions() {
        const bool selected = !metadata->selectedItems().isEmpty(); copyMetadataValueAction->setEnabled(selected); copyMetadataRowAction->setEnabled(selected); copyMetadataAllAction->setEnabled(metadata->topLevelItemCount() > 0);
    }
    QPointF gpsLocation() const { return cachedGps; }
    static QPointF parseGpsLocation(const QStringList &lines) {
        QMap<QString, QString> values;
        for (const auto &line : lines) {
            const auto separator = line.indexOf(": ");
            if (separator > 0) values.insert(line.left(separator), line.mid(separator + 2));
        }
        auto coordinate = [&](const QString &nativeKey, const QString &exifKey, const QString &reference) {
            bool ok = false;
            if (values.contains(nativeKey)) { const double value = values.value(nativeKey).toDouble(&ok); if (ok) return value; }
            const auto components = values.value(exifKey).split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
            if (components.isEmpty()) return 999.0;
            double result = 0, multiplier = 1;
            for (const auto &component : components) {
                const auto fraction = component.split('/');
                double value = fraction[0].toDouble(&ok); if (!ok) return 999.0;
                if (fraction.size() == 2) { const double denominator = fraction[1].toDouble(&ok); if (!ok || denominator == 0) return 999.0; value /= denominator; }
                result += value / multiplier; multiplier *= 60;
            }
            const auto ref = values.value(reference); if (ref == "S" || ref == "W") result = -result;
            return result;
        };
        const double latitude = coordinate("GPS.Latitude", "Exif.GPSInfo.GPSLatitude", "Exif.GPSInfo.GPSLatitudeRef");
        const double longitude = coordinate("GPS.Longitude", "Exif.GPSInfo.GPSLongitude", "Exif.GPSInfo.GPSLongitudeRef");
        return qAbs(latitude) <= 90 && qAbs(longitude) <= 180 ? QPointF(latitude, longitude) : QPointF(999, 999);
    }
    void syncSelection(bool revealCurrent) {
        if (syncingSelection) return;
        syncingSelection = true;
        // Keep view repaint signals live; syncingSelection guards only our navigation callback.
        auto visible = fileProxy->mapFromSource(fileModel->index(doc->currentIndex(), 0));
        if (!visible.isValid() && doc->currentIndex() >= 0 && revealCurrent && !filter->text().isEmpty()) {
            QSignalBlocker filterBlock(filter); filter->clear(); fileProxy->setFilterFixedString({});
            visible = fileProxy->mapFromSource(fileModel->index(doc->currentIndex(), 0));
        }
        if (visible.isValid()) {
            const bool retain = files->selectionModel()->selectedRows().size() > 1 && files->selectionModel()->isSelected(visible);
            files->selectionModel()->setCurrentIndex(visible, retain ? QItemSelectionModel::NoUpdate : QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows);
            if (revealCurrent) files->scrollTo(visible, QAbstractItemView::EnsureVisible);
        } else { files->selectionModel()->clear(); }
        fileCount->setText(filter->text().isEmpty() ? QString::number(doc->count()) : QString("%1 / %2").arg(fileProxy->rowCount()).arg(doc->count()));
        syncingSelection = false;
        const bool empty = fileProxy->rowCount() == 0;
        emptyListLabel->setText(filter->text().isEmpty() ? "이미지를 열면 목록이 표시됩니다." : "일치하는 파일이 없습니다. 검색어를 지우면 전체 목록으로 돌아갑니다.");
        emptyListLabel->setVisible(empty); files->setVisible(!empty);
    }
    void applyFilter() {
        if (syncingSelection) return;
        { QSignalBlocker blocker(files->selectionModel()); fileProxy->setFilterFixedString(filter->text()); }
        syncSelection(false);
    }
    void updateMetadata(const ImageInfo &info) {
        cachedGps = parseGpsLocation(info.metadata);
        const QSignalBlocker blocker(metadata);
        metadata->clear();
        auto add = [this](const QString &key, const QString &value) { auto *item = new QTreeWidgetItem(metadata, { key, value }); item->setToolTip(1, value); };
        add("파일", doc->currentEntry().name); add("크기", QString("%1 × %2 px").arg(info.size.width()).arg(info.size.height()));
        add("형식", info.format.toUpper()); add("용량", bytesText(info.fileBytes)); add("경로", doc->currentEntry().displayPath());
        for (const auto &line : info.metadata) {
            int separator = line.indexOf(": ");
            if (separator < 0) separator = line.indexOf('=');
            if (separator >= 0) add(line.left(separator), line.mid(separator + (line[separator] == ':' ? 2 : 1))); else add("정보", line);
        }
    }
    void jumpImages(int delta) { if (operationPending || doc->count() < 1) return; stopSlideshow(); selectIndex(std::clamp(doc->currentIndex() + delta, 0, doc->count() - 1)); }
    void navigate(int direction) {
        if (operationPending || doc->count() < 1) return;
        const auto info = doc->currentInfo();
        if (info.pdf && info.page + direction >= 0 && info.page + direction < info.pageCount) {
            stopSlideshow(); cancelPendingPosition(); doc->setPage(info.page + direction); return;
        }
        stopSlideshow();
        const int target = std::clamp(doc->currentIndex() + direction * (spread && !info.pdf ? 2 : 1), 0, doc->count() - 1);
        if (target != doc->currentIndex()) selectIndex(target);
    }
    void cancelPendingPosition() {
        ++openSerial; pendingRestore = {}; pendingBookmarkDeletion.clear();
    }
    void selectIndex(int index) {
        if (operationPending || index < 0 || index >= doc->count()) return;
        cancelPendingPosition();
        const bool sameIndex = index == doc->currentIndex();
        errorBanner->hide(); doc->goTo(index);
        if (sameIndex) syncSelection(true);
    }
    void goToImage() {
        if (doc->count() < 1) return;
        stopSlideshow(); bool accepted = false;
        if (doc->currentInfo().pdf) {
            const auto info = doc->currentInfo();
            const int page = integerPrompt(q, "PDF 페이지로 이동", QString("페이지 (1–%1)").arg(info.pageCount), info.page + 1, 1, info.pageCount, 1, &accepted);
            if (accepted) { cancelPendingPosition(); doc->setPage(page - 1); } canvas->setFocus(); return;
        }
        const int page = integerPrompt(q, "이미지 번호로 이동", QString("이미지 번호 (1–%1)").arg(doc->count()), doc->currentIndex() + 1, 1, doc->count(), 1, &accepted);
        if (accepted) selectIndex(page - 1);
        canvas->setFocus();
    }
    void setFitMode(FitMode mode) {
        zoomLockAction->setChecked(false); canvas->setZoomLocked(false); canvas->setFitMode(mode); settings.setValue("fitMode", static_cast<int>(mode)); updateFitActions();
    }
    void manualZoom(double factor) { canvas->zoomBy(factor); updateFitActions(); }
    void updateFitActions() {
        if (!fitGroup) return;
        const bool manual = canvas->isManualZoom();
        fitAction->setChecked(!manual && canvas->fitMode() == FitMode::Fit);
        widthAction->setChecked(!manual && canvas->fitMode() == FitMode::Width);
        actualAction->setChecked(!manual && canvas->fitMode() == FitMode::Actual);
        shrinkAction->setChecked(!manual && canvas->fitMode() == FitMode::Shrink);
    }
    void setSpread(bool enabled, bool rightToLeft) {
        spread = enabled; rtl = rightToLeft;
        spreadAction->setChecked(spread); rtlAction->setChecked(spread && rtl);
        canvas->setSpread(spread, rtl);
        updateSecondaryImage();
        if (positionButton) updateEnabled();
    }
    void updateSecondaryImage() {
        const int index = doc->currentIndex() + 1;
        const QImage next = spread && !doc->currentInfo().pdf && index < doc->count() ? doc->cachedImage(index) : QImage();
        canvas->setSecondaryImage(next);
        const QString error = doc->neighborError(index);
        canvas->setSecondaryPending(spread && !doc->currentInfo().pdf && doc->currentIndex() >= 0 && index < doc->count() && next.isNull() && error.isEmpty());
        if (spread && !error.isEmpty()) q->statusBar()->showMessage("다음 이미지를 읽지 못했습니다. " + error, 8000);
    }
    void chooseFile() {
        stopSlideshow();
        const auto paths = chooseViewerPaths(settings.value("lastDirectory", QDir::homePath()).toString(), q);
        if (!paths.isEmpty()) open(paths);
    }
    void chooseFolder(const QString &initial = {}, const PendingRestore &restore = {}) {
        const auto path = QFileDialog::getExistingDirectory(q, "이미지 폴더 열기", initial.isEmpty() ? settings.value("lastDirectory", QDir::homePath()).toString() : initial);
        if (!path.isEmpty()) open({ path }, restore);
    }
    PendingRestore currentPosition() const {
        const auto entry = doc->currentEntry(); if (entry.key().isEmpty()) return {};
        PendingRestore restore; restore.source = entry.isArchive() ? entry.archivePath : entry.path; restore.member = entry.member;
        restore.occurrence = entry.memberOccurrence; restore.index = doc->currentIndex(); restore.page = doc->currentInfo().page; return restore;
    }
    QString archivePositionKey(const QString &path) const { return "archivePositions/" + QString::fromLatin1(path.toUtf8().toBase64(QByteArray::Base64UrlEncoding)); }
    PendingRestore restoreFromRecord(const QVariantMap &record) const {
        PendingRestore result;
        result.source = record.value("selectionPath", record.value("path")).toString();
        if (!result.source.isEmpty()) result.source = QFileInfo(result.source).absoluteFilePath();
        result.member = record.value("member").toString(); result.occurrence = record.value("occurrence", 0).toInt();
        result.index = record.value("index", -1).toInt(); result.page = record.value("page", 0).toInt(); return result;
    }
    bool restoreMatches(const Entry &entry) const {
        if (!pendingRestore.active()) return false;
        if (entry.isArchive()) return entry.archivePath == pendingRestore.source
            && (pendingRestore.member.isEmpty() ? doc->currentIndex() == pendingRestore.index : entry.member == pendingRestore.member && entry.memberOccurrence == pendingRestore.occurrence);
        return entry.path == pendingRestore.source;
    }
    QVariantList recentSessions() const {
        if (settings.contains("recentSessions")) return settings.value("recentSessions").toList();
        QVariantList sessions;
        // Legacy recent records had no selection identity. Let the normal open
        // path select a file or restore the separate legacy archive position.
        for (const auto &path : settings.value("recent").toStringList()) sessions.append(QVariantMap{{"paths", QStringList{path}}});
        return sessions;
    }
    void persistPosition() {
        if (doc->currentImage().isNull() || pendingRestore.active()) return;
        const auto entry = doc->currentEntry();
        const auto paths = doc->collectionPaths();
        if (paths.isEmpty()) return;
        if (!pendingRecentPaths.isEmpty()) { activePaths = paths; pendingRecentPaths.clear(); recentPositionKey.clear(); }
        if (activePaths.isEmpty()) activePaths = paths;
        const auto identity = entry.key() + QString("/page:%1").arg(doc->currentInfo().page);
        if (identity == recentPositionKey && activePaths == paths) return;
        activePaths = paths; recentPositionKey = identity;
        if (entry.isArchive()) settings.setValue(archivePositionKey(entry.archivePath), QVariantMap{{"index", doc->currentIndex()}, {"member", entry.member}, {"occurrence", entry.memberOccurrence}, {"page", doc->currentInfo().page}});
        auto sessions = recentSessions();
        for (int i = sessions.size() - 1; i >= 0; --i) if (sessions[i].toMap().value("paths").toStringList() == paths) sessions.removeAt(i);
        QVariantMap record {{"paths", paths}, {"selectionPath", entry.isArchive() ? entry.archivePath : entry.path}, {"member", entry.member}, {"occurrence", entry.memberOccurrence}, {"index", doc->currentIndex()}, {"page", doc->currentInfo().page}, {"scoped", doc->isFileScoped()}};
        sessions.prepend(record); while (sessions.size() > 12) sessions.removeLast(); settings.setValue("recentSessions", sessions); settings.remove("recent"); refreshRecent();
    }
    void open(const QStringList &paths, const PendingRestore &restore = {}, bool scoped = false) {
        if (paths.isEmpty() || operationPending) return;
        pendingBookmarkDeletion.clear();
        ++openSerial; pendingRestore = restore; pendingRestore.serial = openSerial;
        pendingRecentPaths.clear(); for (const auto &path : paths) pendingRecentPaths.append(QFileInfo(path).absoluteFilePath());
        stopSlideshow(); filter->clear(); errorBanner->hide();
        const QString first = pendingRecentPaths.first();
        settings.setValue("lastDirectory", QFileInfo(first).isDir() ? first : QFileInfo(first).absolutePath());
        if (paths.size() == 1) {
            if (!pendingRestore.active() && settings.value("resumeArchive", true).toBool() && Document::isArchivePath(first)) {
                const auto stored = settings.value(archivePositionKey(first)); const auto record = stored.toMap();
                if (!record.isEmpty()) { pendingRestore = restoreFromRecord(record); pendingRestore.source = first; }
                else if (stored.isValid()) { pendingRestore.source = first; pendingRestore.index = stored.toInt(); pendingRestore.page = 0; }
                pendingRestore.serial = openSerial;
            }
            if (QFileInfo(first).isFile()) doc->openFiles({first}, !scoped && settings.value("openContainingFolder", false).toBool());
            else doc->open(first);
        } else doc->openFiles(pendingRecentPaths, false);
        canvas->setFocus(); updateEnabled();
    }
    void refreshRecent() {
        const auto sessions = recentSessions();
        QList<QStringList> pathsInMenu;
        for (const auto &value : sessions) { const auto paths = value.toMap().value("paths").toStringList(); if (!paths.isEmpty()) pathsInMenu.append(paths); }
        // Moving within a collection changes its saved selection, not the menu
        // rows. Keep Cocoa menu objects and query the latest record on activation.
        if (pathsInMenu == recentMenuPaths && !recentMenu->actions().isEmpty()) return;
        recentMenuPaths = pathsInMenu; recentMenu->clear();
        for (const auto &value : sessions) {
            const auto record = value.toMap(); auto paths = record.value("paths").toStringList(); if (paths.isEmpty()) continue;
            const QString first = paths.first();
            QString label = QDir::toNativeSeparators(first); if (paths.size() > 1) label += QString(" 외 %1개").arg(paths.size() - 1);
            auto *item = action(recentMenu, label, {}, [this, paths] {
                QVariantMap record;
                for (const auto &value : recentSessions()) { const auto candidate = value.toMap(); if (candidate.value("paths").toStringList() == paths) { record = candidate; break; } }
                if (record.isEmpty()) return;
                const auto restore = restoreFromRecord(record);
                if (paths.size() == 1 && QFileInfo(paths.first()).isDir() && needsExplicitFolderSelection(paths.first())) chooseFolder(paths.first(), restore);
                else open(paths, restore, record.value("scoped", false).toBool());
            }); item->setObjectName("recentSessionAction"); item->setToolTip(paths.join('\n'));
        }
        if (recentMenu->actions().isEmpty()) recentMenu->addAction("최근 열기 기록이 없습니다")->setEnabled(false);
        else { recentMenu->addSeparator(); auto *clear = action(recentMenu, "기록 지우기", {}, [this] { settings.setValue("recentSessions", QVariantList{}); settings.remove("recent"); refreshRecent(); }); clear->setObjectName("clearRecentAction"); }
    }
    void updateSourceHint() {
        const bool scoped = doc->count() > 0 && doc->isFileScoped() && !doc->currentEntry().isArchive();
        sourceHint->setVisible(!cleanView && !sourceHintDismissed && scoped && (doc->isBusy() || !doc->currentImage().isNull()) && errorBanner->isHidden());
        openSourceFolderButton->setEnabled(!operationPending && scoped);
        q->setProperty("monitoringLimited", doc->isMonitoringLimited());
        fileCount->setToolTip(doc->isMonitoringLimited() ? "일부 폴더의 변경을 자동 감지할 수 없습니다. F5로 목록을 새로 고침하세요." : QString());
    }
    void copyMetadata(bool includeKeys, bool all) {
        QStringList lines;
        const auto selected = metadata->selectedItems();
        for (int i = 0; i < metadata->topLevelItemCount(); ++i) { auto *item = metadata->topLevelItem(i); if (all || selected.contains(item)) lines.append(includeKeys ? item->text(0) + ": " + item->text(1) : item->text(1)); }
        if (!lines.isEmpty()) QApplication::clipboard()->setText(lines.join('\n'));
    }
    void showKeyboardHelp() {
        QDialog dialog(q); dialog.setObjectName("keyboardHelpDialog"); dialog.setWindowTitle("키보드 사용법"); dialog.setMinimumSize(520, 440);
        auto *layout = new QVBoxLayout(&dialog); configureDialog(&dialog, 680, 620);
        layout->addWidget(dialogHeading("키보드 사용법", "환경설정 → 단축키에서 변경할 수 있습니다. 문자 키는 검색 입력 중에는 동작하지 않습니다."));
        auto *search = new QLineEdit; search->setObjectName("keyboardHelpSearch"); search->setPlaceholderText("기능 또는 단축키 검색"); search->setClearButtonEnabled(true); search->setAccessibleName("키보드 사용법 검색"); layout->addWidget(search);
        auto *table = new QTreeWidget; table->setObjectName("keyboardHelpTable"); table->setColumnCount(2); table->setHeaderLabels({"기능", "단축키"}); table->setRootIsDecorated(false); table->setAlternatingRowColors(true);
        table->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); table->setSelectionMode(QAbstractItemView::NoSelection);
        table->header()->setSectionResizeMode(0, QHeaderView::Stretch); table->header()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
        for (const auto &command : shortcuts->entries()) {
            const auto keys = ShortcutManager::displaySequences(command.sequences); if (keys.isEmpty()) continue;
            auto *item = new QTreeWidgetItem(table, {command.label, keys}); item->setToolTip(0, command.category + " · " + command.label); item->setToolTip(1, keys);
        }
        QObject::connect(search, &QLineEdit::textChanged, &dialog, [table](const QString &query) { for (int i = 0; i < table->topLevelItemCount(); ++i) { auto *item = table->topLevelItem(i); item->setHidden(!(item->text(0) + ' ' + item->text(1)).contains(query, Qt::CaseInsensitive)); } });
        layout->addWidget(table, 1);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close); QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject); layout->addWidget(dialogFooter(buttons)); dialog.exec();
    }
    void applyChrome() {
        toolbar->setVisible(!cleanView && toolbarAction->isChecked());
        filePanel->setVisible(!cleanView && listAction->isChecked());
        infoPanel->setVisible(!cleanView && infoAction->isChecked());
        footer->setVisible(!cleanView && statusAction->isChecked());
        q->statusBar()->setVisible(!cleanView && statusAction->isChecked());
        q->menuBar()->setVisible(!cleanView); updateSourceHint();
    }
    void setCleanView(bool enabled) {
        cleanView = enabled; cleanAction->setChecked(enabled); applyChrome(); canvas->setFocus();
    }
    void revealChrome() { if (cleanView) setCleanView(false); }
    void restoreView() {
        rememberedWindowedClean = false; sourceHintDismissed = false;
        if (q->isFullScreen()) q->showNormal();
        listAction->setChecked(true); infoAction->setChecked(false); toolbarAction->setChecked(true); statusAction->setChecked(true);
        zoomLockAction->setChecked(false); canvas->setZoomLocked(false); canvas->resetTransform();
        setFitMode(FitMode::Fit); setSpread(false, false); setCleanView(false); filter->clear();
        splitter->setSizes({280, 900, 280}); syncSelection(true);
    }
    void windowStateChanged() {
        const bool full = q->isFullScreen();
        if (full != wasFullScreen) {
            if (full) { rememberedWindowedClean = cleanView; wasFullScreen = true; setCleanView(true); }
            else { wasFullScreen = false; setCleanView(rememberedWindowedClean); }
        }
        fullScreenAction->setChecked(full);
    }
    void toggleFullscreen() {
        if (q->isFullScreen()) q->showNormal(); else q->showFullScreen();
        windowStateChanged();
    }
    void escapeView() {
        stopSlideshow();
        if (q->isFullScreen()) { rememberedWindowedClean = false; q->showNormal(); }
        setCleanView(false); errorBanner->hide();
        if (!filter->text().isEmpty()) filter->clear();
        canvas->setFocus();
    }
    void navigateFromKey(int direction) {
        if (routingListKeys && !filter->text().isEmpty()) {
            const int row = std::clamp(files->currentIndex().row() + direction, 0, std::max(0, fileProxy->rowCount() - 1));
            if (fileProxy->rowCount()) selectIndex(fileProxy->mapToSource(fileProxy->index(row, 0)).row());
        } else navigate(direction);
    }
    bool routeViewKey(QKeyEvent *event, bool execute, QWidget *receiver, bool nativeCanvas) {
        if (QApplication::activeModalWidget() || QApplication::activePopupWidget()) return false;
        auto *focus = QApplication::focusWidget();
        const auto isEditor = [](QWidget *widget) {
            for (auto *ancestor = widget; ancestor; ancestor = ancestor->parentWidget())
                if (qobject_cast<QLineEdit *>(ancestor) || qobject_cast<QAbstractSpinBox *>(ancestor)
                    || ancestor->inherits("QTextEdit") || ancestor->inherits("QPlainTextEdit") || ancestor->inherits("QKeySequenceEdit")) return true;
            return false;
        };
        // A real QQuickWindow key target is authoritative: Cocoa can retain a
        // stale QWidget editor focus after the embedded Metal surface activates.
        const bool editor = isEditor(receiver) || (!nativeCanvas && isEditor(focus));
        const bool metadataInput = !nativeCanvas && ((receiver && (receiver == metadata || metadata->isAncestorOf(receiver)))
            || (!receiver && focus && (focus == metadata || metadata->isAncestorOf(focus))));
        if (metadataInput && event->matches(QKeySequence::Copy)) { if (execute) copyMetadata(false, false); return true; }
        const bool infoInput = !nativeCanvas && ((receiver && infoPanel->isAncestorOf(receiver)) || (focus && infoPanel->isAncestorOf(focus)));
        const bool canvasInput = nativeCanvas || (receiver && (receiver == canvas || canvas->isAncestorOf(receiver)))
            || (!receiver && (!focus || focus == q || focus == canvas || canvas->isAncestorOf(focus)));
        const bool listInput = !nativeCanvas && ((receiver && (receiver == files || files->isAncestorOf(receiver)))
            || (!receiver && focus && (focus == files || files->isAncestorOf(focus))));
        // Native list selection owns range selection and select-all. Viewer
        // navigation remains available for ordinary arrows and canvas input.
        if (listInput && ((event->key() >= Qt::Key_Left && event->key() <= Qt::Key_Down && event->modifiers() != Qt::NoModifier)
            || event->matches(QKeySequence::SelectAll))) return false;
        QScopedValueRollback<bool> origin(routingListKeys, listInput);
        return shortcuts->handleKey(event, execute, !editor && !infoInput, canvasInput);
    }
    void clearDrop() { draggedMime.clear(); draggedPaths.clear(); }
    bool dropFiles(const QMimeData *mime, QEvent::Type phase) {
        if (operationPending || !mime) { clearDrop(); return false; }
        if (phase == QEvent::Drop) {
            // Files can disappear or be replaced during a drag; validate once
            // more at the point where the document is actually opened.
            const auto paths = imageDropPaths(mime); clearDrop();
            if (paths.isEmpty()) return false; open(paths); return true;
        }
        if (phase == QEvent::DragEnter || draggedMime != mime) {
            draggedMime = const_cast<QMimeData *>(mime); draggedPaths = imageDropPaths(mime);
        }
        return !draggedPaths.isEmpty();
    }
    void updateSlideshowButton() {
        if (!slideshowMenuAction) return;
        const bool playing = slideshowAction->isChecked();
        slideshowMenuAction->setText(playing ? QString("슬라이드쇼 · %1초").arg(slideshowSeconds) : QString("슬라이드쇼"));
        useSymbol(slideshowMenuAction, playing ? "pause" : "play");
        slideshowButton->setAccessibleName(playing ? QString("슬라이드쇼 재생 중 · %1초 간격, 간격 변경 또는 중지").arg(slideshowSeconds) : QString("슬라이드쇼 간격 선택"));
    }
    void toggleSlideshow() {
        if (!slideshowAction->isChecked()) { stopSlideshow(); return; }
        if (!doc->count()) { slideshowAction->setChecked(false); return; }
        updateSlideshowButton(); cancelPendingPosition(); slideshowAction->setText("슬라이드 쇼 중지"); slideshowAction->setToolTip(slideshowAction->text() + " (" + ShortcutManager::displaySequences(slideshowAction->shortcuts()) + ")"); if (ready()) slideshow->start(slideshowSeconds * 1000); q->statusBar()->showMessage(QString("슬라이드 쇼 · %1초 간격").arg(slideshowSeconds), 3000);
    }
    void startSlideshow(int seconds) {
        if (operationPending || doc->count() < 1) return;
        slideshowSeconds = std::clamp(seconds, 1, 300); settings.setValue("slideshowSeconds", slideshowSeconds); settings.sync();
        for (auto *choice : slideshowMenu->actions()) if (choice->objectName().startsWith("slideshowInterval")) choice->setChecked(choice->objectName() == QString("slideshowInterval%1Action").arg(slideshowSeconds));
        slideshowAction->setChecked(true); toggleSlideshow();
    }
    void stopSlideshow() {
        slideshow->stop(); slideshowAction->setChecked(false);
        updateSlideshowButton();
        if (slideshowAction->text() != "슬라이드 쇼 시작") slideshowAction->setText("슬라이드 쇼 시작");
        const auto keys = ShortcutManager::displaySequences(slideshowAction->shortcuts());
        slideshowAction->setToolTip(slideshowAction->text() + (keys.isEmpty() ? QString() : " (" + keys + ")"));
    }
    void revealCurrent() {
        const auto entry = doc->currentEntry(); const auto path = entry.isArchive() ? entry.archivePath : entry.path;
#ifdef Q_OS_MACOS
        QProcess::startDetached("/usr/bin/open", { "-R", path });
#elif defined(Q_OS_WIN)
        QProcess::startDetached("explorer.exe", { "/select,", QDir::toNativeSeparators(path) });
#else
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
#endif
    }
    void externalEditor() {
        QString editor = settings.value("editor").toString();
        if (editor.isEmpty()) {
            editor = QFileDialog::getOpenFileName(q, "외부 편집 프로그램 선택");
            if (editor.isEmpty()) return;
            settings.setValue("editor", editor);
        }
        if (!QFileInfo::exists(editor)) { warningDialog(q, "실행 실패", "설정한 편집 프로그램이 없습니다. 환경설정에서 경로를 다시 선택하세요."); return; }
#ifdef Q_OS_MACOS
        if (editor.endsWith(".app", Qt::CaseInsensitive)) {
            // Launch Services reports missing executables and invalid bundles
            // via open's exit status, after the launcher itself starts.
            auto *launcher = new QProcess(q); launcher->setObjectName("externalEditorLauncher");
            QObject::connect(launcher, &QProcess::errorOccurred, q, [this, launcher](QProcess::ProcessError) { errorText->setText("편집 프로그램을 실행할 수 없습니다. " + launcher->errorString()); errorBanner->show(); launcher->deleteLater(); });
            QObject::connect(launcher, qOverload<int, QProcess::ExitStatus>(&QProcess::finished), q, [this, launcher](int exitCode, QProcess::ExitStatus status) {
                if (exitCode != 0 || status != QProcess::NormalExit) { errorText->setText("편집 프로그램을 실행할 수 없습니다. 환경설정에서 경로를 확인하세요.\n" + QString::fromUtf8(launcher->readAllStandardError()).trimmed()); errorBanner->show(); }
                launcher->deleteLater();
            });
            launcher->start("/usr/bin/open", { "-a", editor, doc->currentEntry().path }); return;
        }
        const bool opened = QFileInfo(editor).isFile() && QFileInfo(editor).isExecutable() && QProcess::startDetached(editor, { doc->currentEntry().path });
#else
        const bool opened = QProcess::startDetached(editor, { doc->currentEntry().path });
#endif
        if (!opened) warningDialog(q, "실행 실패", "편집 프로그램을 실행할 수 없습니다. 환경설정에서 경로를 확인하세요.");
    }

    void updateOperationProgress() {
        if (!operationPending || !operationState) return;
        const bool committing = operationState->committing.load();
        const bool cancelled = operationState->cancelled.load();
        cancelOperationButton->setEnabled(!committing && !cancelled);
        const qint64 total = operationState->total.load(), done = operationState->completed.load();
        if (total > 0) { progress->setRange(0, 1000); progress->setValue(int(std::clamp<qint64>(done * 1000 / total, 0, 1000))); }
        else progress->setRange(0, 0);
        operationLabel->setText(cancelled && !committing ? "취소 요청을 처리하고 있습니다. 현재 연산이 끝나면 안전하게 취소합니다." : committing ? operationTitle + " · 파일 반영 중…" : total > 0 ? operationTitle + QString(" · %1 / %2").arg(bytesText(done), bytesText(total)) : operationTitle + "…");
    }
    void cancelFileOperation() {
        if (!operationPending || !operationState || operationState->committing.load()) return;
        operationState->requestCancel(); updateOperationProgress();
    }
    bool requestWindowClose() {
        if (!operationPending) return true;
        closingAfterOperation = true;
        if (exportDialog) { exportDialog->reject(); return false; }
        cancelFileOperation();
        operationLabel->setText("진행 중인 작업을 안전하게 마친 후 창을 닫습니다."); return false;
    }
    void runFileOperation(const std::function<OperationResult(const std::shared_ptr<OperationState> &)> &operation,
                          const QString &title, const QString &success, const QString &removedKey = {}, bool cancellable = true, bool notifyErrors = true) {
        if (operationPending) return;
        operationPending = true; q->setProperty("fileOperationPending", true); operationTitle = title;
        operationState = std::make_shared<OperationState>(); operationState->committing.store(!cancellable);
        const quint64 serial = openSerial;
        updateEnabled(); progress->setRange(0, 0); progress->show(); operationPanel->show(); cancelOperationButton->setVisible(cancellable); updateOperationProgress(); operationTimer->start();
        auto *watcher = new QFutureWatcher<OperationResult>(q);
        QObject::connect(watcher, &QFutureWatcher<OperationResult>::finished, q, [this, watcher, success, removedKey, serial, notifyErrors] {
            const auto result = watcher->result(); watcher->deleteLater(); operationPending = false; q->setProperty("fileOperationPending", false);
            operationTimer->stop(); operationPanel->hide(); operationState.reset(); progress->setRange(0, 0); progress->setVisible(doc->isBusy());
            if (!result.error.isEmpty()) {
                closingAfterOperation = false;
                if (notifyErrors) { errorText->setText("작업을 완료하지 못했습니다. " + result.error); errorBanner->show(); }
            } else if (!result.cancelled) {
                q->statusBar()->showMessage(success, 5000);
                if (serial == openSerial && !removedKey.isEmpty()) doc->removeEntry(removedKey);
            } else q->statusBar()->showMessage("작업을 취소했습니다. 원본과 기존 대상 파일은 변경하지 않았습니다.", 5000);
            updateEnabled(); updateSourceHint();
            const QPointer<MainWindow> guard(q);
            emit q->fileOperationFinished(result.error.isEmpty() && !result.cancelled, result.cancelled, result.error);
            if (!guard) return;
            if (closingAfterOperation) { closingAfterOperation = false; QTimer::singleShot(0, q, &QWidget::close); }
        });
        const auto state = operationState;
        watcher->setFuture(QtConcurrent::run([operation, state] { return operation(state); }));
    }
    void deleteCurrent(bool permanent) {
        if (!ready() || doc->currentEntry().isArchive()) return;
        stopSlideshow(); const auto entry = doc->currentEntry();
        const QString question = permanent ? QString("‘%1’을(를) 완전히 삭제할까요?\n이 작업은 되돌릴 수 없습니다.").arg(entry.name) : QString("‘%1’을(를) 휴지통으로 이동할까요?").arg(entry.name);
        if (questionDialog(q, permanent ? "완전히 삭제" : "휴지통으로 이동", question, QMessageBox::Yes | QMessageBox::No, QMessageBox::No, permanent ? "완전히 삭제" : "휴지통으로 이동") != QMessageBox::Yes) return;
        runFileOperation([path = entry.path, permanent](const std::shared_ptr<OperationState> &) -> OperationResult {
            const bool success = permanent ? QFile::remove(path) : QFile::moveToTrash(path);
            return {success ? QString() : QStringLiteral("파일을 삭제할 수 없습니다. 권한과 파일 사용 여부를 확인하세요."), false};
        }, permanent ? "파일 삭제" : "휴지통으로 이동", permanent ? "파일을 삭제했습니다." : "파일을 휴지통으로 이동했습니다.", entry.key(), false);
    }
    void storePhoto(bool move, bool secondary) {
        if (!ready() || doc->currentEntry().isArchive()) return;
        stopSlideshow(); const QString key = secondary ? "secondaryPhotoFolder" : "photoFolder";
        QString folder = settings.value(key).toString();
        if (folder.isEmpty() || !QDir(folder).exists()) {
            folder = QFileDialog::getExistingDirectory(q, secondary ? "보조 사진 보관함 선택" : "사진 보관함 선택", QStandardPaths::writableLocation(QStandardPaths::PicturesLocation));
            if (folder.isEmpty()) return; settings.setValue(key, folder);
        }
        const auto entry = doc->currentEntry(); const QString target = QDir(folder).filePath(QFileInfo(entry.path).fileName());
        if (samePhysicalFile(entry.path, target)) { informationDialog(q, "사진 보관함", "이미 보관함에 있는 파일입니다."); return; }
        if (QFileInfo::exists(target) && questionDialog(q, "동일한 파일 이름", "대상 파일이 이미 있습니다. 덮어쓸까요?", QMessageBox::Yes | QMessageBox::No, QMessageBox::No, "덮어쓰기") != QMessageBox::Yes) return;
        if (move && questionDialog(q, "사진 이동", QString("‘%1’을(를) 다음 위치로 이동할까요?\n%2").arg(entry.name, folder), QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes) return;
        runFileOperation([source = entry.path, target, move](const std::shared_ptr<OperationState> &state) -> OperationResult {
            QFile input(source); if (!input.open(QIODevice::ReadOnly)) return {"원본 파일을 열 수 없습니다. " + input.errorString()};
            const FileIdentity openedIdentity = fileIdentity(input, true);
            if (!openedIdentity.valid) return {QStringLiteral("원본 파일의 상태를 확인할 수 없습니다. 파일을 다시 열고 시도하세요.")};
            state->total.store(input.size()); const auto modified = QFileInfo(source).lastModified(); const auto permissions = input.permissions();
            QSaveFile output(target); if (!output.open(QIODevice::WriteOnly)) return {"대상 파일을 만들 수 없습니다. " + output.errorString()};
            while (!input.atEnd()) {
                if (state->cancelled.load()) { output.cancelWriting(); return {{}, true}; }
                const auto buffer = input.read(1024 * 1024);
                if (buffer.isEmpty() && input.error() != QFileDevice::NoError) return {"원본을 읽을 수 없습니다. " + input.errorString()};
                if (output.write(buffer) != buffer.size()) return {"대상에 기록할 수 없습니다. " + output.errorString()};
                state->completed.fetch_add(buffer.size());
            }
            if (state->cancelled.load()) { output.cancelWriting(); return {{}, true}; }
            if (fileIdentity(input, true) != openedIdentity || fileIdentity(input, false) != openedIdentity) {
                output.cancelWriting(); return {QStringLiteral("작업 중 원본 파일이 변경되었습니다. 원본과 기존 대상 파일을 보존했습니다. 변경된 이미지를 다시 연 뒤 시도하세요.")};
            }
            output.setPermissions(permissions); if (!state->beginCommit()) { output.cancelWriting(); return {{}, true}; }
            if (!output.commit()) return {"대상 파일을 반영할 수 없습니다. " + output.errorString()};
            QFile committed(target); if (committed.open(QIODevice::ReadOnly)) committed.setFileTime(modified, QFileDevice::FileModificationTime);
            if (move && (fileIdentity(input, true) != openedIdentity || fileIdentity(input, false) != openedIdentity))
                return {QStringLiteral("복사는 완료했지만 원본 파일이 변경되어 삭제하지 않았습니다. 새 원본과 보관함 파일을 확인하세요.")};
            input.close();
            if (move && !QFile::remove(source)) return {QStringLiteral("복사는 완료했지만 원본을 삭제하지 못했습니다. 대상 파일을 확인하세요.")};
            return {};
        }, move ? "사진을 보관함으로 이동" : "사진을 보관함에 복사", move ? "사진을 보관함으로 이동했습니다." : "사진을 보관함에 복사했습니다.", move ? entry.key() : QString());
    }

    QVector<Entry> selectedEntries() const {
        auto rows = files->selectionModel()->selectedRows();
        std::sort(rows.begin(), rows.end(), [](const QModelIndex &a, const QModelIndex &b) { return a.row() < b.row(); });
        QVector<Entry> result;
        for (const auto &row : rows) { const int index = fileProxy->mapToSource(row).row(); if (index >= 0 && index < doc->count()) result.append(doc->entries()[index]); }
        if (result.isEmpty() && doc->currentIndex() >= 0) result.append(doc->currentEntry());
        return result;
    }
    void showSelectionMenu(const QPoint &requested = {}) {
        if (operationPending) return;
        const auto entries = selectedEntries(); if (entries.isEmpty()) return;
        auto *menu = new QMenu(q); menu->setObjectName("selectionContextMenu"); menu->setAttribute(Qt::WA_DeleteOnClose);
        auto *title = menu->addAction(QString("%1개 선택됨").arg(entries.size())); title->setEnabled(false); menu->addSeparator();
        auto *convert = menu->addAction(entries.size() > 1 ? "선택한 이미지로 PDF 만들기…" : "이미지 변환 / 저장…"); convert->setObjectName("convertSelectionAction");
        QObject::connect(convert, &QAction::triggered, q, [this, entries] { convertImage(entries); });
        auto *copy = menu->addAction("선택한 파일 경로 복사"); copy->setObjectName("copySelectedPathsAction");
        QObject::connect(copy, &QAction::triggered, q, [entries] { QStringList paths; for (const auto &entry : entries) paths.append(entry.displayPath()); QApplication::clipboard()->setText(paths.join('\n')); });
        if (entries.size() == 1) {
            auto *removeWhite = menu->addAction("흰 배경 제거하여 저장…"); removeWhite->setObjectName("removeSelectionWhiteBackgroundAction");
            QObject::connect(removeWhite, &QAction::triggered, q, [this, entries] { convertImage(entries, ExportPurpose::RemoveWhiteBackground); });
            menu->addSeparator(); if (entries.first().key() == doc->currentEntry().key()) menu->addAction(q->findChild<QAction *>("addBookmarkAction"));
            auto *reveal = menu->addAction("Finder에서 보기"); QObject::connect(reveal, &QAction::triggered, q, [entry=entries.first()] {
                const auto path = entry.isArchive() ? entry.archivePath : entry.path;
#ifdef Q_OS_MACOS
                QProcess::startDetached("/usr/bin/open", {"-R",path});
#else
                QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(path).absolutePath()));
#endif
            });
        }
        menu->addSeparator();
        auto *all = menu->addAction("목록 전체 선택"); all->setObjectName("selectAllImagesAction"); QObject::connect(all, &QAction::triggered, files, &QListView::selectAll);
        if (entries.size() > 1) {
            auto *clear = menu->addAction("현재 이미지만 선택"); clear->setObjectName("selectCurrentImageOnlyAction");
            QObject::connect(clear, &QAction::triggered, q, [this] { const auto current = files->currentIndex(); if (current.isValid()) files->selectionModel()->select(current, QItemSelectionModel::ClearAndSelect | QItemSelectionModel::Rows); });
        }
        QPoint point = requested;
        if (point.isNull()) { const auto rect = files->visualRect(files->currentIndex()); point = files->isVisible() ? files->viewport()->mapToGlobal(rect.isValid() ? rect.center() : QPoint(24,24)) : canvas->mapToGlobal(canvas->rect().center()); }
        menu->popup(point);
    }
    void convertImage(const QVector<Entry> &requested = {}, ExportPurpose purpose = ExportPurpose::Convert) {
        if (!ready()) return;
        stopSlideshow();
        const auto entries = requested.isEmpty() ? selectedEntries() : requested;
        QVector<ExportSource> sources;
        for (const auto &entry : entries) {
            ExportSource source; source.entry = entry;
            if (entry.key() == doc->currentEntry().key()) { source.page = doc->currentInfo().page; source.snapshot = doc->currentImage(); source.transform = canvas->transformState(); }
            sources.append(source);
        }
        ExportDialog dialog(sources, q, purpose); exportDialog = &dialog;
        QObject::connect(&dialog, &ExportDialog::savingChanged, q, [this](bool saving) { operationPending = saving; q->setProperty("fileOperationPending", saving); updateEnabled(); });
        QObject::connect(&dialog, &ExportDialog::exportFinished, q, [this](bool success, bool cancelled, const QString &error, const QString &) {
            if (success) q->statusBar()->showMessage("변환한 파일을 저장했습니다.", 5000);
            emit q->fileOperationFinished(success, cancelled, error);
        });
        dialog.exec(); exportDialog.clear();
        if (closingAfterOperation) { closingAfterOperation = false; QTimer::singleShot(0, q, &QWidget::close); }
    }

    void printImage() {
        if (!ready()) return;
        stopSlideshow();
        const QImage image = canvas->transformedImage();
        QPrinter printer(QPrinter::HighResolution); QPrintDialog dialog(&printer, q);
        if (dialog.exec() != QDialog::Accepted) return;
        QPainter painter(&printer); if (!painter.isActive()) { warningDialog(q, "인쇄 실패", "프린터를 열 수 없습니다."); return; }
        const QRect area = printer.pageLayout().paintRectPixels(printer.resolution());
        QSize scaled = image.size(); scaled.scale(area.size(), Qt::KeepAspectRatio);
        const QRect target(area.x() + (area.width() - scaled.width()) / 2, area.y() + (area.height() - scaled.height()) / 2, scaled.width(), scaled.height());
        painter.setRenderHint(QPainter::SmoothPixmapTransform); painter.drawImage(target, image); painter.end();
    }
#ifdef Q_OS_WIN
    void setWallpaper() {
        const auto path = QDir::toNativeSeparators(doc->currentEntry().path).toStdWString();
        if (!SystemParametersInfoW(SPI_SETDESKWALLPAPER, 0, const_cast<wchar_t *>(path.c_str()), SPIF_UPDATEINIFILE | SPIF_SENDCHANGE)) warningDialog(q, "배경화면", "배경화면을 설정할 수 없습니다.");
    }
#elif defined(Q_OS_MACOS)
    void setWallpaper() {
        if (!ready() || doc->currentEntry().isArchive()) return;
        QString error;
        if (!setDesktopWallpaper(doc->currentEntry().path, &error)) warningDialog(q, "데스크탑 배경", error);
        else q->statusBar()->showMessage("데스크탑 배경을 변경했습니다.", 5000);
    }
#endif
    void addBookmark() {
        const auto entry = doc->currentEntry();
        QVariantMap bookmark { { "id", QUuid::createUuid().toString(QUuid::WithoutBraces) }, { "path", entry.isArchive() ? entry.archivePath : entry.path }, { "selectionPath", entry.isArchive() ? entry.archivePath : entry.path }, { "member", entry.member }, { "occurrence", entry.memberOccurrence }, { "index", doc->currentIndex() }, { "name", entry.name }, { "page", doc->currentInfo().page }, { "scoped", doc->isFileScoped() } };
        if (entry.key().isEmpty() || doc->currentImage().isNull()) return;
        auto bookmarks = settings.value("bookmarks").toList();
        for (int i = bookmarks.size() - 1; i >= 0; --i) if (bookmarks[i].toMap().value("path") == bookmark.value("path") && bookmarks[i].toMap().value("member") == bookmark.value("member") && bookmarks[i].toMap().value("occurrence", 0) == bookmark.value("occurrence", 0) && bookmarks[i].toMap().value("page", 0) == bookmark.value("page", 0)) bookmarks.removeAt(i);
        bookmarks.append(bookmark); settings.setValue("bookmarks", bookmarks); q->statusBar()->showMessage("책갈피를 추가했습니다.", 3000);
    }
    void openBookmark(const QVariantMap &bookmark) {
        if (operationPending) return;
        const auto path = bookmark.value("path").toString(); if (path.isEmpty()) return;
        const quint64 previous = openSerial;
        open({path}, restoreFromRecord(bookmark), bookmark.value("scoped", false).toBool());
        if (openSerial != previous && settings.value("deleteBookmarkAfterOpen", false).toBool()) {
            pendingBookmarkDeletion = bookmark; pendingBookmarkSerial = openSerial;
        }
    }
    void finishBookmarkOpen(const ImageInfo &info) {
        if (pendingBookmarkDeletion.isEmpty() || pendingBookmarkSerial != openSerial || doc->isBusy() || canvas->isPreparing()) return;
        const auto requested = restoreFromRecord(pendingBookmarkDeletion);
        const auto entry = doc->currentEntry();
        const bool same = entry.isArchive() ? entry.archivePath == requested.source
            && (requested.member.isEmpty() ? doc->currentIndex() == requested.index : entry.member == requested.member && entry.memberOccurrence == requested.occurrence)
            : entry.path == requested.source;
        if (same && (requested.page < 0 || requested.page >= info.pageCount)) { pendingBookmarkDeletion.clear(); return; }
        if (!same || info.page != requested.page || doc->currentImage().isNull()) return;
        auto bookmarks = settings.value("bookmarks").toList();
        for (qsizetype i=bookmarks.size(); i>0; --i) if (bookmarks[i-1].toMap() == pendingBookmarkDeletion) bookmarks.removeAt(i-1);
        pendingBookmarkDeletion.clear(); settings.setValue("bookmarks", bookmarks);
        q->statusBar()->showMessage("책갈피를 열고 목록에서 삭제했습니다.", 4000);
    }
    void bookmarksDialog() {
        QDialog dialog(q); dialog.setObjectName("bookmarksDialog"); dialog.setWindowTitle("책갈피 목록"); dialog.setMinimumSize(520, 400);
        auto *layout = new QVBoxLayout(&dialog); configureDialog(&dialog, 720, 500);
        layout->addWidget(dialogHeading("책갈피", "저장한 이미지와 페이지로 돌아가세요."));
        auto *list = new QListWidget; list->setObjectName("bookmarksList"); useDetailList(list); list->setAccessibleName("저장한 책갈피"); layout->addWidget(list, 1);
        const auto *add = q->findChild<QAction *>("addBookmarkAction");
        const auto key = add ? add->shortcut().toString(QKeySequence::NativeText) : QString();
        auto *empty = new QLabel(key.isEmpty() ? "책갈피 메뉴에서 현재 위치를 저장하세요." : "이미지를 연 뒤 " + key + " 키로 현재 위치를 저장하세요."); empty->setObjectName("emptyBookmarksMessage"); empty->setWordWrap(true); empty->setAlignment(Qt::AlignCenter); empty->setProperty("tone", "muted");
        auto *emptyState = new QWidget; emptyState->setObjectName("bookmarkEmptyState"); auto *emptyLayout = new QVBoxLayout(emptyState); emptyLayout->setContentsMargins(32, 32, 32, 32); emptyLayout->setSpacing(12); emptyLayout->addStretch();
        auto *emptyIcon = new QLabel; emptyIcon->setAlignment(Qt::AlignCenter); emptyIcon->setPixmap(viewerIcon("bookmark", qApp->palette().color(QPalette::PlaceholderText)).pixmap(QSize(40, 40), q->devicePixelRatioF())); emptyLayout->addWidget(emptyIcon);
        auto *emptyTitle = new QLabel("아직 저장한 책갈피가 없습니다"); emptyTitle->setObjectName("emptyStateTitle"); emptyTitle->setAlignment(Qt::AlignCenter); emptyLayout->addWidget(emptyTitle); emptyLayout->addWidget(empty); emptyLayout->addStretch(); layout->addWidget(emptyState, 1);
        auto bookmarks = settings.value("bookmarks").toList();
        auto *row = new QHBoxLayout; row->setContentsMargins(0, 16, 0, 0); row->setSpacing(8); auto *remove = new QPushButton("선택 삭제"); remove->setProperty("destructive", true); remove->setProperty("subtle", true); remove->setObjectName("removeBookmarkButton"); auto *rename = new QPushButton("이름 변경"); rename->setObjectName("renameBookmarkButton"); rename->setProperty("subtle", true); row->addWidget(remove); row->addWidget(rename); row->addStretch(); auto *openButton = new QPushButton("열기"); openButton->setObjectName("openBookmarkButton"); openButton->setProperty("primary", true); openButton->setDefault(true); auto *close = new QPushButton("닫기"); row->addWidget(close); row->addWidget(openButton); auto *footerWidget = new QWidget; footerWidget->setObjectName("dialogFooter"); footerWidget->setLayout(row); layout->addWidget(footerWidget);
        auto updateButtons = [&] { const bool selected = list->currentRow() >= 0; remove->setEnabled(selected); rename->setEnabled(selected); openButton->setEnabled(selected && !operationPending); emptyState->setVisible(bookmarks.isEmpty()); list->setVisible(!bookmarks.isEmpty()); };
        auto refresh = [&](int selected) {
            list->clear();
            for (const auto &value : bookmarks) {
                const auto bookmark = value.toMap(); QString location = bookmark.value("path").toString(); const auto member = bookmark.value("member").toString();
                if (!member.isEmpty()) location += " :: " + member;
                const int occurrence = bookmark.value("occurrence", 0).toInt(); if (occurrence > 0) location += QString(" (동일 이름 %1번째)").arg(occurrence + 1);
                const int page = bookmark.value("page", 0).toInt(); QString name = bookmark.value("name").toString(); if (page > 0) name += QString(" · 페이지 %1").arg(page + 1);
                auto *item = new QListWidgetItem(name, list); setDetailText(item, location); item->setToolTip(name + "\n" + location);
            }
            if (!bookmarks.isEmpty()) list->setCurrentRow(std::clamp(selected, 0, int(bookmarks.size() - 1))); updateButtons();
        };
        QObject::connect(list, &QListWidget::currentRowChanged, &dialog, [&](int) { updateButtons(); }); refresh(0);
        QObject::connect(remove, &QPushButton::clicked, &dialog, [&] { const int index = list->currentRow(); if (index < 0) return; bookmarks.removeAt(index); settings.setValue("bookmarks", bookmarks); refresh(index); });
        QObject::connect(rename, &QPushButton::clicked, &dialog, [&] { const int index = list->currentRow(); if (index < 0) return; auto value = bookmarks[index].toMap(); bool ok = false; const auto name = textPrompt(&dialog, "책갈피 이름", "이름", value.value("name").toString(), &ok); if (ok && !name.trimmed().isEmpty()) { value["name"] = name.trimmed(); bookmarks[index] = value; settings.setValue("bookmarks", bookmarks); refresh(index); } });
        auto openSelected = [&] { const int index = list->currentRow(); if (index < 0) return; openBookmark(bookmarks[index].toMap()); dialog.accept(); };
        QObject::connect(openButton, &QPushButton::clicked, &dialog, openSelected); QObject::connect(list, &QListWidget::itemDoubleClicked, &dialog, [&](QListWidgetItem *) { openSelected(); }); QObject::connect(close, &QPushButton::clicked, &dialog, &QDialog::accept); dialog.exec();
    }
    void applyTheme() {
        auto theme = settings.value("theme", "dark").toString();
        if (theme != "dark" && theme != "light" && theme != "system") theme = "dark";
        applyApplicationTheme(theme); canvas->setBackgroundColor(viewerThemeBackground(theme));
        if (fileModel) fileModel->refreshPlaceholder();
        q->setProperty("themeMode", theme); q->setProperty("darkTheme", applicationThemeIsDark(theme));
        for (auto *command : q->findChildren<QAction *>()) if (command->property("ikviewSymbol").isValid()) useSymbol(command, command->property("ikviewSymbol").toString());
        for (auto *button : q->findChildren<QToolButton *>()) if (button->property("ikviewSymbol").isValid()) useSymbol(button, button->property("ikviewSymbol").toString());
    }
    void applyImageFilter() {
        auto id = settings.value("imageFilter", "soften").toString();
        if (!imageFilterActions.contains(id)) id = "soften";
        const auto mode = id == "none" ? ImageFilterMode::None : id == "sharpen" ? ImageFilterMode::Sharpen
            : id == "soften-sharpen" ? ImageFilterMode::SoftenSharpen : ImageFilterMode::Soften;
        canvas->setImageFilter(mode); q->setProperty("imageFilterMode", id);
        for (auto it=imageFilterActions.begin(); it!=imageFilterActions.end(); ++it)
            if (it.value()->isChecked() != (it.key() == id)) it.value()->setChecked(it.key() == id);
    }
    void applyPreferences() {
        cacheMB = resolvedCacheMB(settings.value("cachePreset", "auto").toString(), detectSystemProfile());
        doc->setCacheLimitMB(cacheMB); q->setProperty("decodedCacheMB", cacheMB); applyTheme(); applyImageFilter();
    }
    void refreshPowerPolicy() {
        inhibitDisplaySleep(settings.value("keepDisplayAwake", false).toBool() && q->isVisible()
            && q->isActiveWindow() && !q->isMinimized() && !loadedKey.isEmpty());
    }
    void preferences() {
        PreferencesValues values;
        values.imageFilter = settings.value("imageFilter", "soften").toString();
        values.theme = settings.value("theme", "dark").toString(); values.cachePreset = settings.value("cachePreset", "auto").toString();
        values.preventSleep = settings.value("keepDisplayAwake", false).toBool();
        values.resumeArchive = settings.value("resumeArchive", true).toBool();
        values.deleteBookmarkAfterOpen = settings.value("deleteBookmarkAfterOpen", false).toBool();
        values.openContainingFolder = settings.value("openContainingFolder", false).toBool();
        auto *editor = new ShortcutEditor(shortcuts);
        PreferencesDialog dialog(values, detectSystemProfile(), editor, q);
        dialog.setValidationCallback([editor](QString *error) { return editor->apply(error); });
        if (dialog.exec() != QDialog::Accepted) return;
        values = dialog.values();
        if (dialog.clearArchivePositionsRequested()) { settings.remove("archivePositions"); recentPositionKey.clear(); }
        settings.setValue("imageFilter", values.imageFilter); settings.setValue("theme", values.theme); settings.setValue("cachePreset", values.cachePreset);
        settings.setValue("keepDisplayAwake", values.preventSleep);
        settings.setValue("resumeArchive", values.resumeArchive); settings.setValue("deleteBookmarkAfterOpen", values.deleteBookmarkAfterOpen);
        settings.setValue("openContainingFolder", values.openContainingFolder); shortcuts->save(settings);
        // Retire obsolete freeform appearance input; it must not override theme.
        settings.remove("background"); applyPreferences(); refreshPowerPolicy(); settings.sync();
    }
    void about() {
        QDialog dialog(q); dialog.setObjectName("aboutDialog"); dialog.setWindowTitle("IKView 정보"); dialog.setMinimumSize(480, 420);
        auto *layout = new QVBoxLayout(&dialog); configureDialog(&dialog, 620, 580);
        layout->addWidget(dialogHeading("IKView " + QCoreApplication::applicationVersion(), "빠르게 열고, 편하게 살펴보는 이미지·PDF 뷰어"));
        auto *summary = new QLabel("폴더와 압축파일 탐색 · 책갈피 · 이미지 변환 · PDF 만들기"); summary->setWordWrap(true); summary->setProperty("tone", "muted"); layout->addWidget(summary);
        auto *system = new QGroupBox("현재 실행 환경"); auto *form = new QFormLayout(system); configureForm(form);
        const auto field = [](const QString &text) { auto *label = new QLabel(text); label->setWordWrap(true); label->setTextFormat(Qt::PlainText); label->setTextInteractionFlags(Qt::TextSelectableByMouse); return label; };
        form->addRow("렌더러", field(canvas->rendererDescription())); form->addRow("이미지 캐시", field(bytesText(cacheBytes))); form->addRow("최근 디코딩", field(QString("%1 ms").arg(doc->currentInfo().decodeMs))); layout->addWidget(system);
        auto *label = new QLabel("이 환경에서 지원하는 파일 형식"); label->setProperty("tone", "muted"); layout->addWidget(label);
        auto *formats = new QPlainTextEdit; formats->setObjectName("supportedFormats"); formats->setReadOnly(true); formats->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff); formats->setWordWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere); formats->setPlainText(Document::supportedExtensions().join("  ·  ")); layout->addWidget(formats, 1);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Close); QObject::connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject); layout->addWidget(dialogFooter(buttons)); dialog.exec();
    }

};

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    d = std::make_unique<Impl>(this);
    qApp->installEventFilter(this);
}
MainWindow::~MainWindow() { qApp->removeEventFilter(this); }
bool MainWindow::eventFilter(QObject *watched, QEvent *event) {
    if (!d) return QMainWindow::eventFilter(watched, event);
    auto *widget = qobject_cast<QWidget *>(watched);
    auto *native = qobject_cast<QWindow *>(watched);
    bool belongs = widget && widget->window() == this;
    if (native) {
        if (native->property("ikviewCanvas").value<QObject *>() == d->canvas) belongs = true;
        for (auto *ancestor = native; ancestor && !belongs; ancestor = ancestor->parent()) if (ancestor == windowHandle()) belongs = true;
    }
    if (!belongs) return QMainWindow::eventFilter(watched, event);
    if ((event->type() == QEvent::ShortcutOverride || event->type() == QEvent::KeyPress)
        && d->routeViewKey(static_cast<QKeyEvent *>(event), event->type() == QEvent::KeyPress, widget, native && native->property("ikviewCanvas").value<QObject *>() == d->canvas)) {
        event->accept(); return true;
    }
    // The native Metal surface handles its own drag highlight. Ordinary widget
    // areas are also drop destinations, including the toolbar and file list.
    const bool canvasChild = widget && (widget == d->canvas || d->canvas->isAncestorOf(widget));
    if (!native && !canvasChild && !QApplication::activeModalWidget()) {
        if (event->type() == QEvent::DragLeave) d->clearDrop();
        if (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove || event->type() == QEvent::Drop) {
            auto *drop = static_cast<QDropEvent *>(event);
            const auto allowed = drop->possibleActions();
            const auto action = allowed.testFlag(Qt::CopyAction) ? Qt::CopyAction
                : allowed.testFlag(Qt::LinkAction) ? Qt::LinkAction : Qt::IgnoreAction;
            if (action == Qt::IgnoreAction && drop->mimeData() && drop->mimeData()->hasUrls()) {
                drop->ignore(); return true;
            }
            if (d->dropFiles(drop->mimeData(), event->type())) {
                drop->setDropAction(action); drop->accept(); return true;
            }
        }
    }
    return QMainWindow::eventFilter(watched, event);
}
void MainWindow::changeEvent(QEvent *event) {
    QMainWindow::changeEvent(event);
    if (d && event->type() == QEvent::WindowStateChange) d->windowStateChanged();
}
void MainWindow::closeEvent(QCloseEvent *event) {
    if (d && !d->requestWindowClose()) { event->ignore(); return; }
    QMainWindow::closeEvent(event);
}
void MainWindow::openPaths(const QStringList &paths) { d->open(paths); }
void MainWindow::openFolderKeepingCurrent(const QString &folder) { if (!folder.isEmpty()) d->open({folder}, d->currentPosition()); }
void MainWindow::openPathsAt(const QStringList &paths, int imageIndex, int page) {
    if (paths.isEmpty()) return;
    PendingRestore restore; restore.source = QFileInfo(paths.first()).absoluteFilePath(); restore.index = std::max(0, imageIndex); restore.page = std::max(0, page); restore.byIndex = true;
    d->open(paths, restore);
}
}
