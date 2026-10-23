#include "macosservices.h"
#include "document.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QMetaObject>
#include <QPointer>
#include <QSet>
#include <QThread>
#include <QThreadPool>
#include <QtConcurrent/QtConcurrentRun>
#include <atomic>
#include <memory>
#include <optional>
#include <utility>
#import <AppKit/AppKit.h>

namespace {
struct ServiceState {
    std::function<void(QStringList)> openPaths;
    std::function<void(QString)> reportError;
    std::atomic_bool active {true};
};
std::shared_ptr<ServiceState> serviceState;
id previousProvider = nil;
id installedProvider = nil;

void serviceError(NSString **error, NSString *message)
{
    if (error) *error = message;
}

bool imageSuffix(const QString &suffix)
{
    static const QSet<QString> extensions = [] {
        const auto values = ik::Document::supportedExtensions();
        return QSet<QString>(values.begin(), values.end());
    }();
    return extensions.contains(suffix.toLower());
}

QStringList classifyPaths(const QStringList &paths, bool containingFolders,
                          const std::function<bool()> &cancelled = {})
{
    QStringList result;
    QSet<QString> seen;
    for (const auto &path : paths) {
        if (cancelled && cancelled()) return {};
        if (path.isEmpty()) continue;
        const QFileInfo file(QDir::cleanPath(path));
        if (!file.exists() || (!file.isFile() && !file.isDir())) continue;
        QString accepted;
        if (containingFolders) accepted = file.isDir() ? file.absoluteFilePath() : file.absolutePath();
        else if (file.isDir() || imageSuffix(file.suffix()) || ik::Document::isArchivePath(file.filePath()))
            accepted = file.absoluteFilePath();
        if (accepted.isEmpty()) continue;
        accepted = QDir::cleanPath(accepted);
        if (!seen.contains(accepted)) {
            seen.insert(accepted);
            result.append(accepted);
        }
    }
    return result;
}

struct ServiceRequest {
    QStringList selected;
    bool containingFolders = false;
    quint64 generation = 0;
    std::shared_ptr<ServiceState> recipient;
};
struct ValidationResult {
    QStringList paths;
    QString error;
};
struct ValidationCounters {
    std::atomic<quint64> jobs {0};
    std::atomic<quint64> onGuiThread {0};
};

class ServiceScheduler final : public QObject {
public:
    explicit ServiceScheduler(QCoreApplication *application) : QObject(application), pool(new QThreadPool(application))
    {
        setObjectName(QStringLiteral("ikviewFinderServiceScheduler"));
        pool->setObjectName(QStringLiteral("ikviewFinderServicePool"));
        pool->setMaxThreadCount(1);
        pool->setExpiryTimeout(10000);
    }
    ~ServiceScheduler() override { cancel(); }

    void submit(QStringList paths, bool containingFolders, const std::shared_ptr<ServiceState> &recipient)
    {
        if (!recipient->active.load(std::memory_order_relaxed)) return;
        // New selections replace the single pending request and cancel stale
        // per-path work. Never enqueue a task for every service invocation.
        pending = ServiceRequest{std::move(paths), containingFolders, ++*epoch, recipient};
        startNext();
    }

    void cancel()
    {
        ++*epoch;
        pending.reset();
    }

    ik::MacFinderServiceActivity activity() const
    {
        return {counters->jobs.load(std::memory_order_relaxed), counters->onGuiThread.load(std::memory_order_relaxed),
                watcher ? 1 : 0, pending ? 1 : 0};
    }

private:
    QThreadPool *pool;
    QFutureWatcher<ValidationResult> *watcher = nullptr;
    std::optional<ServiceRequest> pending;
    std::shared_ptr<std::atomic<quint64>> epoch = std::make_shared<std::atomic<quint64>>(0);
    std::shared_ptr<ValidationCounters> counters = std::make_shared<ValidationCounters>();

    void startNext()
    {
        if (watcher || !pending) return;
        const ServiceRequest request = std::move(*pending);
        pending.reset();
        if (!request.recipient->active.load(std::memory_order_relaxed)) return;
        watcher = new QFutureWatcher<ValidationResult>(this);
        auto *completion = watcher;
        const auto generation = request.generation;
        const auto recipient = request.recipient;
        QObject::connect(completion, &QFutureWatcher<ValidationResult>::finished, this, [this, completion, generation, recipient] {
            const ValidationResult result = completion->result();
            watcher = nullptr;
            completion->deleteLater();
            if (epoch->load() == generation && recipient->active.load(std::memory_order_relaxed)) {
                if (!result.error.isEmpty()) {
                    if (recipient->reportError) recipient->reportError(result.error);
                } else if (!result.paths.isEmpty() && recipient->openPaths) recipient->openPaths(result.paths);
            }
            startNext();
        });
        // Pool and callbacks stay owned on the GUI side. Worker captures only
        // immutable path data, cancellation token and atomic diagnostics.
        const QStringList selected = request.selected;
        const bool containingFolders = request.containingFolders;
        QThread *guiThread = thread();
        completion->setFuture(QtConcurrent::run(pool, [selected, containingFolders, generation, token = epoch, counts = counters, guiThread] {
            counts->jobs.fetch_add(1, std::memory_order_relaxed);
            if (QThread::currentThread() == guiThread) counts->onGuiThread.fetch_add(1, std::memory_order_relaxed);
            const auto cancelled = [&] { return token->load() != generation; };
            ValidationResult result;
            if (cancelled()) return result;
            result.paths = classifyPaths(selected, containingFolders, cancelled);
            if (!cancelled() && result.paths.isEmpty()) result.error = containingFolders
                ? QStringLiteral("선택한 로컬 파일 또는 폴더에 접근할 수 없습니다. 위치와 접근 권한을 확인해 주세요.")
                : QStringLiteral("열 수 있는 이미지·압축파일 또는 폴더가 없습니다. 위치와 지원 형식을 확인해 주세요.");
            return result;
        }));
    }
};
QPointer<ServiceScheduler> scheduler;

void enqueuePaths(const QStringList &paths, bool containingFolders, const std::shared_ptr<ServiceState> &recipient)
{
    auto *application = QCoreApplication::instance();
    if (!application) return;
    const auto enqueue = [paths, containingFolders, recipient] {
        if (scheduler && recipient->active.load(std::memory_order_relaxed)) scheduler->submit(paths, containingFolders, recipient);
    };
    if (QThread::currentThread() == application->thread()) enqueue();
    else QMetaObject::invokeMethod(application, enqueue, Qt::QueuedConnection);
}
}

@interface IKViewFinderServices : NSObject {
@public
    std::shared_ptr<ServiceState> state;
}
- (void)openInIKView:(NSPasteboard *)pasteboard userData:(NSString *)userData error:(NSString **)error;
@end

@implementation IKViewFinderServices
- (void)openInIKView:(NSPasteboard *)pasteboard userData:(NSString *)userData error:(NSString **)error
{
    const bool containingFolders = [userData isEqualToString:@"containing-folder"];
    if (![userData isEqualToString:@"open"] && !containingFolders) {
        serviceError(error, @"알 수 없는 IKView 서비스 요청입니다.");
        return;
    }
    if (!state || !state->active.load(std::memory_order_relaxed) || !state->openPaths || !QCoreApplication::instance()) {
        serviceError(error, @"IKView가 종료 중입니다. 앱을 다시 열어 주세요.");
        return;
    }
    QStringList selected;
    @try {
        // Read only this service's pasteboard. URL decoding does no file
        // existence/type queries; filesystem classification runs off-GUI.
        const NSArray *urls = [pasteboard readObjectsForClasses:@[[NSURL class]]
            options:@{NSPasteboardURLReadingFileURLsOnlyKey: @YES}];
        for (NSURL *url in urls) {
            if ([url isFileURL] && [url path]) selected.append(QString::fromNSString([url path]));
        }
        if (selected.isEmpty()) {
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
            const id filenames = [pasteboard propertyListForType:NSFilenamesPboardType];
#pragma clang diagnostic pop
            if ([filenames isKindOfClass:[NSArray class]]) {
                for (id path in filenames) if ([path isKindOfClass:[NSString class]] && [path length])
                    selected.append(QString::fromNSString(path));
            }
        }
    } @catch (NSException *exception) {
        Q_UNUSED(exception);
        serviceError(error, @"Finder에서 선택한 파일을 읽을 수 없습니다.");
        return;
    }
    if (selected.isEmpty()) {
        serviceError(error, @"로컬 파일 또는 폴더를 선택해 주세요.");
        return;
    }
    enqueuePaths(selected, containingFolders, state);
}
@end

namespace ik {
QStringList selectedServicePaths(const QStringList &paths, bool containingFolders)
{
    return classifyPaths(paths, containingFolders);
}

void installMacFinderServices(std::function<void(QStringList)> openPathsCallback, std::function<void(QString)> errorCallback)
{
    auto *application = QCoreApplication::instance();
    if (!application || QThread::currentThread() != application->thread()) return;
    removeMacFinderServices();
    if (!scheduler) scheduler = new ServiceScheduler(application);
    NSApplication *nativeApplication = [NSApplication sharedApplication];
    previousProvider = [nativeApplication servicesProvider];
#if !__has_feature(objc_arc)
    [previousProvider retain];
#endif
    serviceState = std::make_shared<ServiceState>();
    serviceState->openPaths = std::move(openPathsCallback);
    serviceState->reportError = std::move(errorCallback);
    IKViewFinderServices *provider = [[IKViewFinderServices alloc] init];
    provider->state = serviceState;
    installedProvider = provider;
    [nativeApplication setServicesProvider:provider];
#if !__has_feature(objc_arc)
    [provider release];
#endif
}

void removeMacFinderServices()
{
    if (serviceState) serviceState->active.store(false, std::memory_order_relaxed);
    if (scheduler) scheduler->cancel();
    if (installedProvider && [NSApp servicesProvider] == installedProvider) [NSApp setServicesProvider:previousProvider];
    installedProvider = nil;
#if !__has_feature(objc_arc)
    [previousProvider release];
#endif
    previousProvider = nil;
    serviceState.reset();
}

MacFinderServiceActivity macFinderServiceActivity()
{
    return scheduler ? scheduler->activity() : MacFinderServiceActivity{};
}
}
